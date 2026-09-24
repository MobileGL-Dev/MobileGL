# 帧节奏、进程、EGL 与平台（原 ARCHITECTURE §14–§15）

> 设计细节。设计要点与全部章节的索引见 [`../ARCHITECTURE.md`](../ARCHITECTURE.md)。章节编号沿用原 `ARCHITECTURE.md`，代码注释里的 `ARCHITECTURE.md §N` 按编号在这里找到。

## 14. Present、线程与帧节奏

- `eglSwapBuffers` → `present{frameSerial}` → publish + 敲门铃 → 返回，除非超出 credit；**`present` 与 `eglSwapBuffers` 严格 1:1**（后端的帧边界排空只在 `Present` 内发生）。
- `MOBILEGL_IPC_PRESENT_CREDIT` 默认 1（range 1..8）：credit 是 run-ahead 唯一的稳态背压，`FrameSerial` 由 client 铸造、1 起算，server 在 `Present()` 返回后归还 credit；等待发生在**编码之前**。credit > 1 从未量过（P10）。
- 预算（~850 draw/帧）：`SEG_CMD` ~70 KB/帧，`SEG_STAGE` ~1 MB/帧；跨机时这 ~1 MB/帧对着的是 Wi-Fi。
- 线程：client v1 不加线程，编码在 GL 线程直接写 ring；server 有 io 线程与**终身持有原生 context** 的 `mgl-srv-apply`。全库无有效亲和性控制（Redmi 内核忽略 app 线程的 `sched_setaffinity`）。
- 拆机顺序：publish + server 排空 → 停 apply 线程 → 关传输 → client 排空编译池 → `MobileGL::Destroy()`（落地形状见 §17）。

## 15. 进程、EGL 与平台（P6 / P6.5 / P12）

### 15.1 启动与握手

- 端点：`fork`（`socketpair` + `fork`/`execve`，fd 3 = socket；server 由 `MOBILEGL_IPC_SERVER_PATH` 或 `dladdr` 同目录的 `libMobileGLServer.so` 定位）、`unix:<path>`、`tcp://host:port`。设备侧 supervisor `mobilegl_server_main <endpoint> --serve`：listen → 认证 → 每会话 fork 一个子进程（或进程内线程，P12），同时只服务一个会话，第二个连接 `Refuse{Busy}`。
- server 进程里 `Transport` 读作 `Spawn`，**不强制 monolith**（两百多处 `Transport != Monolith` 判断靠它选 server 臂；`CONTRACT-P6.md` §3.1 修订了原"子进程强制 monolith"的规则）。防无界 fork 链靠不继承的 `Dial == No`（`MOBILEGL_IPC_DIAL=no`），spawn 时构造的 envp 剔除 `MOBILEGL_TRANSPORT` 与 `MOBILEGL_IPC_*` 作第二道独立保险；server 主程序自设 `MOBILEGL_IPC_ROLE=server`。
- `Hello`（abi、后端类型、指纹、配置、令牌、`LinkTerms` 请求）→ `Welcome`（server 陈述的尺寸、段 / aux 名、`dataNonce`、真实 pid）或 `Refuse{code, detail, 对端值}`；握手路径零 abort。非 loopback 监听必须有令牌（≥16 字节、常量时间比较、数据面按 `dataNonce` 绑定到已认证的控制连接、fork 前认证；Ph，OQ-21 裁定不做 TLS）。
- TCP keepalive / 心跳失败**由传输层报告为挂断**，进 device-lost 闩；"闩绝不取自 apply 超时"不因此松动。冷启动期 server 发 `SurfaceProgress`，client 预算这段沉默（控制修订 2）。

### 15.2 Android

- 交付链（spike A 已证）：APK 唯一可 exec 的位置是 `lib/<abi>/`，server 以 `add_executable` + `lib*.so` 命名构建；从 `untrusted_app` 进程 `fork`+`execve` 同域、零 avc denial；`posix_spawn` 在 minSdk 26 不可用。一份共享库两个角色（stub `dlopen` + `dlsym("mobilegl_server_main")`）。
- 离屏 server：前台 `MobileGLServerService`（`:mglsrv`），从 `nativeLibraryDir` exec，fork-per-session，EGL 是 pbuffer / surfaceless。
- **上屏 server（P12 子集）**：窗口归 **server 自己**（client 的窗口不跨进程）。`MobileGLDisplayActivity`（`:mglwin`）持全屏 SurfaceView，在自己的进程里用一个线程跑 TCP server（会话串行、会话间重置闩、后端按进程钉住）；`ServerDisplay` 以 acquire / release / 几何三个钩子持有窗口，apply 线程以**租约**使用它。client 设 `MOBILEGL_IPC_SURFACE=server` 后**无头**：`eglCreateWindowSurface(NULL)` 发一帧 `WindowKind::ServerOwned`（控制修订 3），不发 `SetWindowHandle`，尺寸由 reply 带回，resize 变成对 server 窗口的几何请求。一台设备同一时刻一个 server；一个会话一种表面模式（`SurfaceModeMismatch`）；无显示的 server 对 ServerOwned 具名拒绝（`NoServerDisplay`）。
- 失窗：`surfaceDestroyed` 有界（3 s）阻塞，直到 apply 线程（批内每条记录前也检查）放掉后端表面并以 `Fatal{ServerWindowLost}` 闩住会话；server 继续监听。进程内会话结束时手动做进程退出本来白给的清理（Espryt twin 与单元影子、swap interval、Magma renderer）。
- `HeadlessGL` 的 fork 预检会产生孤儿 server：server 的 EOF 检测即时且无条件退出。

### 15.3 Linux / Windows / 崩溃

- Linux / WSL / CI 永不开窗（`EGL_PLATFORM=surfaceless`）；Wayland 不支持。Windows / 桌面 client 在 TCP 终局下是真实形态（Windows 是第二批，开放问题 22）；macOS 不拆分；Windows 机器不是正确性门。
- server 死：client 读到挂断 → **device-lost 闩**（GL 调用 no-op、`eglSwapBuffers` 返回 `EGL_CONTEXT_LOST`、reset status `GL_UNKNOWN_CONTEXT_RESET`）；`Session::Fail` 漏斗向对端发 `SessionFault` 帧命名家族（`FatalFamilies.def`）。**不重启**：`MOBILEGL_IPC_RESPAWN` 今天是具名拒绝。client 死：server 读到 EOF 立即销毁 context 并退出（supervisor 继续服务下一连接）。
