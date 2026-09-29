# P12：server 自有屏幕窗口

2026-09-29。依据 `docs/Disaggregated/notes/p12/`（[`PLAN-P12.md`](../../docs/Disaggregated/notes/p12/PLAN-P12.md)、裁定 `INTEGRATOR-DECISIONS-P12.md` ID-P12-1..15、验收 [`CROSSHOST-ACCEPTANCE.md`](../../docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md) / [`FCL-ACCEPTANCE.md`](../../docs/Disaggregated/notes/p12/FCL-ACCEPTANCE.md)）。本文只记实现所需的协议与行为；验证结果以那些文件里的运行证据为准。

## 范围

server 应用自己拥有一个窗口，把 client 渲染的帧直接上屏；client 不需要窗口。**离屏（pbuffer）路径不变**，两条路径**每会话只有一条活跃**。不在本阶段：X11 / Win32 / None 窗口种类的白名单（D8）、cached-app freezer 的完整处理、多 context、TLS、把触摸转发给 client。

## 配置

- client：`MOBILEGL_IPC_SURFACE=offscreen|server`（默认 `offscreen`）。`server` 让 `eglCreateWindowSurface` 向远端 server 要它自己的窗口；没有远端 server 时忽略并说一次。
- server：进程是否拥有显示由宿主决定（`ServerDisplay` 钩子已安装 = 拥有）。fork 出的离屏 supervisor 不拥有。

## 线上格式（控制面修订 3）

- `WindowKind.ServerOwned = 7`，只在 `CreateWindowSurface` 上合法；`nativeToken` 恒为 0，**没有任何指针过线**（Rule G / H）。`SetWindowHandle` 不得指名它（`SurfaceRefusal::ServerOwnedOnSetWindowHandle`）。
- `SurfaceReply` 追加 `width`、`height`（server 窗口实际尺寸）与 `refusal`。`SurfaceRefusal`：`NoServerDisplay`（server 没有显示）、`NoServerWindow`（有显示但等不到窗口）、`SurfaceModeMismatch`（会话已被另一种表面模式闩定）、`ServerOwnedOnSetWindowHandle`。拒绝是具名的 `ok=false` 应答，**不闩会话**。
- 修订号 2 → 3，`wireFingerprint` 随之移动：client 与 server 一起重装。
- client 的 `SetWindowHandle` 不再无条件发送（server-owned 模式下不发）。

## 表面模式

会话的表面模式在**第一次成功的表面创建**时闩定：ServerOwned 窗口 → 上屏，pbuffer → 离屏；之后另一种具名拒绝（`SurfaceModeMismatch`）。client 自己命名的窗口（X11 / Win32 / None）既不闩也不拒（D8 未关）。

## 窗口租约与失窗

- `ServerDisplay` 持有窗口引用与租约；创建表面时 apply 线程按需取租约。窗口没有时等（有界），同时按 `SurfaceProgress` 心跳，client 读作"还在跑"而不是"活着但沉默"。
- **失窗的顺序即契约**（`surfaceDestroyed` → `Detach`）：apply 线程先让后端放掉窗口（Espryt 销毁 EGL surface / context，Magma 销毁 swapchain / VkSurface），再以 `Fatal{ServerWindowLost}` 闩会话，**最后**才结束租约、让回调返回。`Detach` 有界 3 s，超时不抽走窗口。失窗请求在 `DrainRing` 每次 pop 之前也检查。
- 窗口属于显示进程，不属于会话：会话结束后窗口还在，下一个会话接着用；Espryt 的孪生对象与单元影子随会话清空（`DropEveryTwinForEndedServerSession`），下一会话从空表开始。

## 几何

- client 请求 `W×H`：`W,H > 0` → server 显示 `SurfaceHolder.setFixedSize(W,H)`，视图**等比缩放、留黑边**放进屏幕；`0/0` → `setSizeFromLayout`（窗口自己的大小）。请求是异步应答，server 等布局回报（ID-P12-9）。
- **请求尺寸的来源**：`eglCreateWindowSurface` 的 `EGL_WIDTH/EGL_HEIGHT`；两者缺省（SDL 就是这样）且传入了 native window 时，取 `ANativeWindow_getWidth/Height`（Android、split 构建、server-owned）。缺这一步时游戏按自己的窗口渲染、server 窗口取屏幕尺寸，横屏游戏在竖屏窗口里被裁成左下角。
- `ResizeEGLWindowSurface` 走同一条几何请求，后端表面随窗口**真实**尺寸，reply 带回该尺寸，client 采用它（ID-P12-10）。`kEventSurfaceChanged` 携带宽高（Espryt 以前只发格式，client 的默认帧缓冲会停在 512×512 占位值）。

## 死亡与 device-lost

- device-lost 只由对端**挂断**闩住，从不由超时闩住（CONTRACT-P6 §5.4 不变）。杀 server 进程后 client 在毫秒级闩住，随后每个 verb 被 DECLINED，`glGetGraphicsResetStatus` 报 `GL_UNKNOWN_CONTEXT_RESET`。
- 闩锁的 `DEVICE LOST` 说一次；闩住之后 DECLINED 的 verb **不再逐条打 ERROR**（一个 FCL 游戏几千次/秒）。

## server 的形态

- **进程内显示 server**（`MobileGLDisplayActivity`，进程 `:mglwin`）：TCP / unix 监听线程 + 会话线程（`TcpSupervisor::Handoff::Thread`），预认证 / Busy / DataBind 路由与 fork 形状是同一份代码；会话串行；后端**按进程钉住**（无 `MOBILEGL_BACKEND_TYPE` 时取第一个会话的）；会话间 `ResetSessionLatch`。以崩溃隔离换窗口可达：会话里一个未武装的 `SessionFail` 会带走整个显示进程。
- **离屏 supervisor**（`MobileGLServerService`）：每会话 fork 一个子进程，子进程 `PR_SET_PDEATHSIG` 随 supervisor 去。
- **同一设备一个 server**：显示 Activity 与离屏 service 互相替换（谁后启谁拥有端口）。Render Server 屏（`ServerControlActivity`）是两种形态的入口，字段与 extras：`listen` / `token` / `env`（`KEY=VALUE;KEY`）/ `onscreen` / `start` / `stop`。
- 停止推流中的进程内 server 时，会话在 `loop.Stop()` 前放弃队列里剩余记录（`AbandonQueuedRecords`）；正常的 client EOF 仍 drain 完。
- 转发日志：进程内 server 只有会话线程与选择加入的线程转发，且在释放日志互斥之后、在独立转发互斥下发送。

## FCL 集成（FCL fork，不在本仓库）

- FCL 的界面一退后台就把游戏暂停；server 窗口要在屏幕上，FCL 必然在后台。版本设置 **`keepRunningInBackground`**（"游戏退到后台时不暂停"，默认关）：强制 TextureView 并保留它的 SurfaceTexture，界面离开前台时不通知 SDL 暂停、不撤销 GLFW focused / visible。
- FCL 默认的 spawn（游戏进程派生 server 子进程）把自己的窗口指针发给 server，按 Rule H 被拒（`UnmigratedSurface`）：**只能接外部 render server**，`MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:<port>`、`MOBILEGL_IPC_DATA=stream`、`MOBILEGL_IPC_SURFACE=server`；后端由 `mg_env.txt` 里的 `MOBILEGL_BACKEND_TYPE` 选。
- 时序：FCL 的游戏界面出现后要再等约 10 s 才能把 server 窗口切到前台。

## 不变量

- G1：pull 构建符号 0/0/0/0，`.text` 不变（全部新代码在 `MOBILEGL_BUILD_DISAGGREGATED` 下；`EGLImpl` 的新分支还要 `__ANDROID__`）。
- 离屏路径仍是门（gate 3 跑 `--use-pbuffer`）。`RefuseCode` 只追加。
- 会话里 server-owned 表面之外的表面（`SurfaceModeMismatch`）具名拒绝，会话继续。

## 仍开放

D8 窗口种类白名单（X11 / Win32 / None 仍被接受并把 token 强转为指针）；freezer 的完整处理；多 context；TLS；server 窗口不转发输入；DirectGLES 的 `g_Display/g_Surface/g_Context` 仍是全局（每进程一个会话时不需要）；修后的横屏 server 窗口没有重测。
