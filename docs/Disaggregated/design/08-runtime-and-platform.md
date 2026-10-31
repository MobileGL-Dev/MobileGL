# 帧节奏、进程、EGL 与平台（原 ARCHITECTURE §14–§15）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 14. Present、线程与帧节奏

- `eglSwapBuffers` → `present{frameSerial}` → publish + 敲门铃 → 返回，除非超出 credit；**`present` 与 `eglSwapBuffers` 严格 1:1**（后端的帧边界排空只在 `Present` 内发生）。
- `MOBILEGL_IPC_PRESENT_CREDIT` 默认 1（range 1..8）：credit 是 run-ahead 唯一的稳态背压，`FrameSerial` 由 client 铸造、1 起算，server 在 `Present()` 返回后归还 credit；等待发生在**编码之前**。credit > 1 从未量过（P10）。
- 预算（~850 draw/帧）：`SEG_CMD` ~70 KB/帧，`SEG_STAGE` ~1 MB/帧；跨机时这 ~1 MB/帧对着的是 Wi-Fi。
- 线程：client v1 不加线程，编码在 GL 线程直接写 ring；server 有 io 线程与**终身持有原生 context** 的 `mgl-srv-apply`。全库无有效亲和性控制（Redmi 内核忽略 app 线程的 `sched_setaffinity`）。
- 拆机顺序：publish + server 排空 → 停 apply 线程 → 关传输 → client 排空编译池 → `MobileGL::Destroy()`（落地形状见 §17）。

### 14.1 apply 线程的亲和与自旋（`auto`）

`MOBILEGL_IPC_SERVER_AFFINITY=auto`（缺省）按会话对端（`Hello` 的 `DialMode`）与数据面选择 apply 线程的亲和与 park 前自旋（`MG_Remote/Server/ApplyThreadPolicy.h`，纯函数）：同进程、fork 出的、拨入但走 stream 的对端 → 全部大核与 `MOBILEGL_IPC_SPIN_US`；拨入且走共享段的对端 → 一个核（`ReservedApplyCore`：多个 prime 核时取最低的；只有一个 prime 核时取另一个大核；否则不钉，绝不给唯一的 prime 核）与 `kConnectedPeerSpinUs`（除非显式设了 `MOBILEGL_IPC_SPIN_US`）。显式掩码或 `off` 对任何对端优先。
- 拨入的共享段 client 直接写 apply 线程自旋的那条环，两边又落在同两颗 prime 核上：Redmi 上共享段 23–26 fps、TCP 45–54，手工分开后 118。stream 数据面由 io 线程喂 apply 线程，apply 不在 client 的写上自旋（apply CPU 7.9 ms/帧，共享段 26.8），所以维持全部大核。
- 取最低而非最高的 prime 核：调度器把前台应用的热 GL 线程放在最高的 prime 核（44 次采样 43 次在 cpu7）；apply 线程钉在那里 6.5 fps，钉在最低的 120 fps。
- 短自旋保留：立即 park（0）在所有量过的格子里都更慢（helper 路径 120 → 74 fps，shell 27 → 17）。
- 经 `ExternalClientHelper` 启动的 client 也被挡在保留核之外：supervisor 启动时公布该核（`ReservedApplyCoreForConfig`），broker 交给 helper；操作员给了掩码或 `off` 时不保留任何核。

## 15. 进程、EGL 与平台（P6 / P6.5 / P12）

### 15.1 启动与握手

- 端点：`fork`（`socketpair` + `fork`/`execve`，fd 3 = socket；server 由 `MOBILEGL_IPC_SERVER_PATH` 或 `dladdr` 同目录的 `libMobileGLServer.so` 定位）、`unix:<path>`、`tcp://host:port`。设备侧 supervisor `mobilegl_server_main <endpoint> --serve`：listen → 认证 → 每会话 fork 一个子进程（或进程内线程，P12），同时只服务一个会话，第二个连接 `Refuse{Busy}`。
- server 进程里 `Transport` 读作 `Spawn`，**不强制 monolith**（两百多处 `Transport != Monolith` 判断靠它选 server 臂；`CONTRACT-P6.md` §3.1 修订了原"子进程强制 monolith"的规则）。防无界 fork 链靠不继承的 `Dial == No`（`MOBILEGL_IPC_DIAL=no`），spawn 时构造的 envp 剔除 `MOBILEGL_TRANSPORT` 与 `MOBILEGL_IPC_*` 作第二道独立保险；server 主程序自设 `MOBILEGL_IPC_ROLE=server`。
- `Hello`（abi、后端类型、指纹、配置、令牌、`LinkTerms` 请求）→ `Welcome`（server 陈述的尺寸、段 / aux 名、`dataNonce`、真实 pid）或 `Refuse{code, detail, 对端值}`；握手路径零 abort。非 loopback 监听必须有令牌（≥16 字节、常量时间比较、数据面按 `dataNonce` 绑定到已认证的控制连接、fork 前认证；Ph，OQ-21 裁定不做 TLS）。
- TCP keepalive / 心跳失败**由传输层报告为挂断**，进 device-lost 闩；"闩绝不取自 apply 超时"不因此松动。冷启动期 server 发 `SurfaceProgress`，client 预算这段沉默（控制修订 2）。

#### 15.1.1 TCP supervisor 的预认证

TCP `--serve` 的 supervisor（`ServerMain.cpp` 的 `TcpSupervisor`，判定部分在 `PreAuthGate.h`）先认证、再把会话交出去（fork 子进程，或进程内线程）；未认证的对端最多占一个有界的 pending 槽：

- 新连接在首帧完整之前是 pending。supervisor 在一个 poll 循环里同时从所有 pending 连接非阻塞地读，任何未认证对端都拖不住别人；只读到首帧末尾，之后的字节属于会话。
- 每个 pending 连接有 `MOBILEGL_IPC_PREAUTH_MS`（默认 2000）读完首帧，同时最多 `MOBILEGL_IPC_PREAUTH_MAX`（默认 8）个。队列按地址共享：满时新连接挤掉占槽最多的地址最老的那个；它自己的地址已占同样多时，被拒 `Busy` 的是它。
- Hello 在 supervisor 里认证、令牌先于指纹：未认证的对端只收到 `Refuse{Authentication}`，得不到本构建的 wire / build 指纹。只为已认证的 Hello 交出会话，并把这一帧交给会话。
- 失败（令牌错、首帧畸形、超时、静默占槽超过 `kPreAuthProbeGraceMs` 后关闭或被挤掉）记在对端地址上；满 `MOBILEGL_IPC_AUTH_BACKOFF_AFTER`（默认 5，0 = 关）次后该地址在 accept 时、读任何字节之前即被拒，窗口自 `MOBILEGL_IPC_AUTH_BACKOFF_MS`（默认 1000）起逐次翻倍、上限一分钟；该地址认证成功一次即清零。宽限期内无字节就关闭的端口探测、指向无活会话的 DataBind、退避本身发出的拒绝都不算失败。
- DataBind 经 hand-off 连同 nonce 转给活会话（会话比较 nonce）；没有活会话时由 supervisor 具名拒绝。已认证的第二个 Hello 收到 `Busy`。
- 判定部分不含 I/O（`MG_Remote/Server/PreAuthGate.h`），由 `PreAuthGateTest` 覆盖；早于 `kAuthBackoffForgetMs` 的失败不再计入退避。

### 15.2 Android

- 交付链（spike A 已证）：APK 唯一可 exec 的位置是 `lib/<abi>/`，server 以 `add_executable` + `lib*.so` 命名构建；从 `untrusted_app` 进程 `fork`+`execve` 同域、零 avc denial；`posix_spawn` 在 minSdk 26 不可用。一份共享库两个角色（stub `dlopen` + `dlsym("mobilegl_server_main")`）。
- 离屏 server：前台 `MobileGLServerService`（`:mglsrv`），从 `nativeLibraryDir` exec，fork-per-session，EGL 是 pbuffer / surfaceless。
- **上屏 server（P12 子集）**：窗口归 **server 自己**（client 的窗口不跨进程）。`MobileGLDisplayActivity`（`:mglwin`）持全屏 SurfaceView，在自己的进程里用一个线程跑 TCP server（会话串行、会话间重置闩、后端按进程钉住）；`ServerDisplay` 以 acquire / release / 几何三个钩子持有窗口，apply 线程以**租约**使用它。client 设 `MOBILEGL_IPC_SURFACE=server` 后**无头**：`eglCreateWindowSurface(NULL)` 发一帧 `WindowKind::ServerOwned`（控制修订 3），不发 `SetWindowHandle`，尺寸由 reply 带回，resize 变成对 server 窗口的几何请求；client 没给 `EGL_WIDTH/HEIGHT`（SDL）时取 native window 的大小作请求，server 把窗口缓冲固定成它并等比留黑边（不然横屏游戏在竖屏窗口里被裁）。FCL 只能接外部 server（自带 spawn 会把窗口指针发过来，被 Rule H 拒），且要开版本设置「游戏退到后台时不暂停」，否则 FCL 一退后台游戏就暂停（见 git 历史）。一台设备同一时刻一个 server；一个会话一种表面模式（`SurfaceModeMismatch`）；无显示的 server 对 ServerOwned 具名拒绝（`NoServerDisplay`）。
- 失窗：`surfaceDestroyed` 有界（3 s）阻塞，直到 apply 线程（批内每条记录前也检查）放掉后端表面并以 `Fatal{ServerWindowLost}` 闩住会话；server 继续监听。进程内会话结束时手动做进程退出本来白给的清理（Espryt twin 与单元影子、swap interval、Magma renderer）。
- `HeadlessGL` 的 fork 预检会产生孤儿 server：server 的 EOF 检测即时且无条件退出。

### 15.3 Linux / Windows / 崩溃

- Linux / WSL / CI 永不开窗（`EGL_PLATFORM=surfaceless`）；Wayland 不支持。Windows / 桌面 client 在 TCP 终局下是真实形态（Windows 是第二批，开放问题 22）；macOS 不拆分；Windows 机器不是正确性门。
- server 死：client 读到挂断 → **device-lost 闩**（GL 调用 no-op、`eglSwapBuffers` 返回 `EGL_CONTEXT_LOST`、reset status `GL_UNKNOWN_CONTEXT_RESET`）；`Session::Fail` 漏斗向对端发 `SessionFault` 帧命名家族（`FatalFamilies.def`）。闩之前创建的 context 永久丢失（`eglMakeCurrent` / `eglSwapBuffers` 返回 `EGL_CONTEXT_LOST`，`glGetError` 报一次 `GL_CONTEXT_LOST`，销毁时不往新 session 发任何记录）。**进程可恢复**（`MOBILEGL_IPC_RECOVER`，默认 1）：应用下一次 `eglCreateContext` / `eglCreate*Surface` / `eglInitialize` 拆掉丢失的 session、重新拨号得到新 session，client 侧描述旧 server 的缓存全部清空（发布闩、CSO/sampler 缓存、emitter 闩、create window、applier-reset 序号、persistent map），wire epoch 前进使旧 GLContext 一律视为丢失；surface 保留并在下次 make-current 时于新 session 重建（EGL 对 power-management 事件的语义），Wayland 窗口的 shared image 在新 session 重新分配。in-process server 与 `fd:` 对不可重拨，仍是永久丢失。`MOBILEGL_IPC_RESPAWN`（server 重启后全量重推）仍是具名拒绝。client 死：server 读到 EOF 立即销毁 context 并退出（supervisor 继续服务下一连接）。

#### 15.3.1 Fatal 家族与 wire FatalCode

内部 Fatal 家族（`MG_Pipe/FatalFamilies.def`）远多于 wire 上 `FatalCode` 的 7 个值，所以家族是内部枚举：对端只收到粗粒度的码，细粒度的词留在本地日志与 `SessionFault` 帧的 `family` 字符串里。码只回答「这是哪一类结束」：

- `ProtocolCorruption`：无法正确执行的东西——畸形记录、未实现的 verb / surface、两个角色必须守住的不变式、不合契约的回复。对端读到的是「双方对协议的理解不一致」。
- `RingOverrun`：共享环或事件通道溢出，是传输容量问题。
- `SegmentMismatch`：共享段的尺寸放不下某个角色必须放进去的东西（尺寸错，不是一时满）。
- `AbiMismatch`：两端的布局或协议版本不一致。
- `ServerCrashed`：等待方归咎于对端的活性结束：排空未完成、apply 线程不在、server 拿不到必需的原生资源（`ResourceUnavailable`）、server 窗口被销毁（`ServerWindowLost`）、GPU 设备丢失（`BackendDeviceLost`，只闩住该 session）。拿不到资源不报 `SegmentMismatch`，那会把错归到共享段尺寸上；wire 上也没有「拿不到资源」的值，新增会改 `wireFingerprint`。
- `DeviceLost`：只由 device-lost 闩设置，没有家族投影到它。

### 15.4 共享图像（`shared_image`）

server 分配并以 dma-buf 描述符导出的颜色缓冲，让一个 client 的帧不离开 GPU 内存就进入另一个 client 的纹理。一行记录（`MGPSharedImageOp`），每个操作都经 reply slot 以 `MGPSharedImageReply` 回答；server 交给 client 的描述符走 aux socket，先于 reply 到达；`ImageId` 由 server 铸造，server 生命期内不复用。`Damage` 是 GL 窗口坐标、左下原点的矩形，0 个 = 整个表面。

| 操作 | 语义 |
|---|---|
| `Allocate` / `Release` | 新建 `Width×Height` 的 `Format` 图像，reply 给出 id，描述符经 aux socket 送达 / 会话放掉对 `ImageId` 的引用 |
| `Import` | 认领 client 在 aux socket 上排队的描述符（`MGPSharedImageFdOffer` 带本记录的 seq）；须是本 server 导出的图像，尺寸与格式一致 |
| `Present` | 当前 context 的默认帧缓冲按顶行在先拷进 `ImageId`（窗口帧成为 wl_buffer 的内容），只拷 `Damage`：client 按图像记录它自上次写入后缺的区域；GPU 拷完才回 reply |
| `Attach` | 纹理 `Texture` 的存储变成 `ImageId`（`glEGLImageTargetTexture2DOES`），直到它下一次定义存储的 respecify |
| `QueryBufferAge` | 在流中回答当前 draw surface 的 `EGL_BUFFER_AGE_EXT`（0 = 未知）；应用先声明绘制区域（`EGL_KHR_partial_update`）时 `Format = kMGPBufferAgeDamageRegionFollows` |
| `SetDamageRegion` | `eglSetDamageRegionKHR`：本帧将画的区域 |
| `Flush` | 往图像里渲染的会话（X server 的 glamor）的 `glFlush` / `glFinish`：自上个边界用过的图像按 flush 的 fence 发布为已写、已读（隐式同步），发布后回答 |
| `NativeFence` | `EGL_ANDROID_native_fence_sync`：会话已提交工作完成时 signal 的 sync_file（写图像的会话同时做 `Flush` 的发布），经 aux socket 先于 reply 送达，`Format = kMGPNativeFenceFdFollows`；`Format = 0` 表示 server 给不出描述符，reply 发出时工作已完成 |

### 15.5 EGL 归属：apply 线程终身持有原生 context

split 下应用线程从不调用驱动的 `eglMakeCurrent`：`BackendObject_Remote` 的 EGL 虚函数调用 `ServerLoop.h` 的 `Server*` 转发函数，每个都经 `RunSurfaceControlFrame` 成为阻塞控制请求，由 `mgl-srv-apply` 对 server 私有的后端对象执行。
- 为什么是这条线程：DirectGLES 的 `IsBackendContextCurrentOnThisThread()` 与 `Managers.cpp` 的 `CanTouchGLNow()` 守卫答「否」时静默降级——fence 恒为已 signal、query 返回空句柄、`Present` 不建帧 fence（buffer pool 的回收水位不再前进）、persistent map 拒绝采纳。apply 线程终身持有 context，这些守卫在 server 上都答「是」。
- 原生 `eglMakeCurrent` 只在 apply 线程当前绑定的 tuple 真正改变时运行，去重分两层：apply 线程上的 `ServerLoop::ApplyMakeCurrent` 用 `ClassifyEglMakeCurrent` 与上次绑定的 tuple 比较，相同即 no-op、不重发 caps，不同则转发并重发 caps 快照，client 的 release-current 只记录（`ClientReleaseCount`）不转发；`BackendObject_DirectGLES::MakeEGLCurrent` 在有传输时，若目标 draw surface 已是本线程原生当前、且虚 tuple 正是那次绑定所为（或那次绑定是 surface 自己的创建），跳过原生调用。同一 surface 上换虚 context 仍原生重绑，因为 `MakeCurrent` 的失效针对的是变了的前端 context。只记一个 tuple：线程上同时只有一个 current context，A → B → A 是三次原生绑定。
- 在原生绑定可能已改变、或 tuple 指向的 context 可能已不存在的每个事件之后遗忘 tuple（`ServerLoop::ForgetCurrentTuple`）：`ReleaseEGLResources`、释放它指向的 surface、surface（重）建（创建时原生绑定新 surface）、后端销毁。EGL 句柄值会复用；记住死 tuple 的代价是静默地没有 current context，遗忘的代价只是一次多余的转发。
- `ReleaseEGLResources` 与 `~BackendObject_DirectGLES` 必须阻塞：前者本身是控制请求，后者在 apply 线程退出前于该线程执行，`ServerLoop::Stop()` 等它结束；否则 `MobileGL::Destroy()` 会在 server 仍持有 context 时继续往下拆。
- 对照 `ServerLoopEglTest.MakeCurrentBindsNativelyOncePerContextAndNeverForwardsAClientRelease` 在 EGL 函数表处计数：surface 创建 + 两次相同 make-current + 一次 client release + release 后再绑定 = 一次原生 `eglMakeCurrent`、零次原生 release。
