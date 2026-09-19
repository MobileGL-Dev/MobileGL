---

## 10. 线程模型

### Client
- **v1 不加线程。** 编码在调用方 GL 线程上直接写进 ring。前端本来就是 per-context 单线程契约（`GLContext` 无 mutex；`EGLState::MakeCurrent` 强制一个 owner 线程，`EGLState/Core.cpp:1215-1220`，测试在 `MG_Test/EGLState/EGLStateTest.cpp:39-92`）。
- **flow = per context，不是 per thread。** 今天恰好一个 flow。`eglMakeCurrent` 是 flow 所有权转移，在既有 `EGLOperationMutex`（`EGLImpl.cpp:241`）下发射。**顺手修既有漏洞**：`EGLImpl::ReleaseThread`（`:341-350`）与 `SwapInterval`（`:435-450`）今天不取该锁而另外三个（`MakeCurrent`/`SwapBuffers`/`DestroySurface`）取。
- **外来线程的 sync/query**：读全部从 `RingControl` 无锁 acquire load 回答（比取 registry mutex 更好）；少数必须发射的（`FenceSync`、`Begin*Query`，以及 §7.2 要求的轮询 publish）取 `ctrlMutex` 并走 CTRL socket 的 out-of-band `AuxRequest` 帧（SPSC ring 不允许第二个 producer）。
- **等待必须能挂起**：所有 client 侧等待（present credit、`kNeedsAck`、ring/stage 满、轮询升级）走 §6.2a 的 `producerParked` + 反向门铃，自旋窗口 `MOBILEGL_IPC_SPIN_US`（默认 50µs）。
- ShaderCompilePool 原样保留在 client（`ShaderCompilePool.h:77-82`，≤4 worker，为 RSS 上限）。
- 可选 `mgl-client-tx` 双缓冲发送线程：**P6 项，凭测量决定**。在 P6 的 Tracy 数据出来之前不要预先加线程（会引入拷贝或锁）。

### Server
| 线程 | 职责 |
|---|---|
| `mgl-srv-io` | asio `io_context::run`：封帧读写、`SCM_RIGHTS`、双向 doorbell、CTRL RPC |
| `mgl-srv-apply` | **终身持有原生 EGL/Vulkan context**：消费 ring → 解码 → apply 进 replica → 调 backend 表 |
| `mgl-srv-dec`（可选，P6） | FlatBuffers/边界校验前置，凭测量决定 |

因为 context 永不迁移：`g_backendContextOwnerThread`（`DirectGLES.cpp:10052`）只写一次；`DirectGLES::MakeCurrent` 的 8 缓存失效风暴（`:10123-10140`）变成启动期一次性成本；`IsBackendContextCurrentOnThisThread` 的每帧 EGL 复核（`:10195-10228`，动机是 `eglGetCurrentContext` 实测占渲染线程 16%）恒真。DirectGLES 的 off-thread 降级（`FenceSync` 返回 null 等）消失——**保真度提升**。延迟 replay 机制（`Managers.h:458-473` 的 `pendingRespecify`/`pendingRanges`/`pendingResidentWrites`）保留但永不触发。

### 核心放置（本轮新增，是性能主张的前提）

§5.1 明说 reconciler "就是 `PrepareForDraw` 的可达性遍历"。这意味着这套遍历**每 draw 跑两次**：client 的 `WireMirror` 一次，server 未改动的 `PrepareForDraw`（`DirectGLES.cpp:2916-2975`）一次，外加编码与解码。其中有些并不便宜：`CurrentUnitBindingsEpoch`（`DirectGLES.cpp:1421-1438`）在 `GetTextureBindGeneration()` 变动时会退化成对每个 touched texture unit 做 owner-equality 全走查，而代码自己注明这在冗余重绑时就会发生（"26.2 re-binds the unit's own sampler around every texture-unit switch"）。

所以拆分的全部性能主张都押在"两半落在两个都快的核上"。而 MobileGL 全库从不设置亲和性（`grep -rn 'sched_setaffinity\|cpu_set_t\|affinity' MobileGL/` 零命中），server 是 fork/exec 出来的独立进程、不继承 launcher 的亲和性，项目记忆 `pojav-bigcore-affinity-trap` 又记录过 `pojavBigCore=true` 把整个游戏 JVM 加 MobileGL worker 钉死单核、让一整批历史测量作废。若 `mgl-srv-apply` 落到 1.55GHz 小核，它做的工作严格多于 monolith 在 1.96GHz 大核上做的，拆分按构造就是回归，而 §15 P3 的"帧时在 monolith 10% 内"会以一个没人会正确归因的理由失败。

**规则**：
1. 计划里必须写出**总 CPU 工作量差**（client reconcile + encode + decode + server `PrepareForDraw`  vs  monolith 的 `PrepareForDraw`），不只是单侧成本。
2. 复用 `ShaderCompilePool` 已有的大核探测（`ShaderCompilePool.cpp:73-96` 的 `ReadCpuMaxFrequencyKHz` / `DetectBigCoreCount`）把 `mgl-srv-apply` 绑到大核，开关 `MOBILEGL_IPC_SERVER_AFFINITY`（默认 auto），并把解析出的 mask 打进日志。
3. P2.5 与 P3 必须报**逐线程 CPU 时间**，不只是墙钟帧时，这样"没有收益"的结论能被归因到放置 vs 编码成本。

### 拆机顺序（三条约束）
`Publish()` + server 排空并 ack → 停 apply 线程 → 关 transport →（client）排空 compile pool（必须先于 `glslang::FinalizeProcess()` 与 `pGLContext` 析构，`ShaderCompilePool.h:106-110`、`Init.cpp:56-62`）→ `MobileGL::Destroy()`（`EGLImpl.cpp:335-338`）→ 释放 sync/query handle（`GL_Sync.cpp:223-226`）。

---

## 11. EGL/窗口与进程生命周期

### 11.1 启动与握手

client 定位 server 的顺序（**本轮修正**）：
1. `MOBILEGL_IPC_SERVER_PATH`（**主要机制**）。
2. `dladdr(&MobileGL::Initialize)` → dirname → `libMobileGLServer.so`（**兜底**）。

上一版把 `dladdr` 当主要机制，但两个桌面验收门都因此找不到 server：`MG_IntegrationTest/CMakeLists.txt:28-35` 在非 Android 上把 `MGL_ITEST_MOBILEGL_TARGET` 设成 `MobileGL_s`（**静态链接**），`dladdr` 解析到测试可执行文件自身的路径而不是库目录；trace replay 则由 `tools/trace_replay/CMakeLists.txt:285-290` 显式传 `-DMOBILEGL_LIBRARY=$<TARGET_FILE:MobileGL>`，其目录是 MobileGL 的构建输出目录，而 CMake 默认把 `add_executable` 放在定义它的目录的 binary dir。

**配套**：把 `MobileGLServer` 的 `RUNTIME_OUTPUT_DIRECTORY` 设成 `$<TARGET_FILE_DIR:MobileGL>`，并把 `"MOBILEGL_IPC_SERVER_PATH=$<TARGET_FILE:MobileGLServer>"` 加进每一条新的 ctest `ENVIRONMENT`（经 `mgl_itest_join_environment` 与 `${MGL_ITEST_COMMON_ENV}` 合并）以及 `add_trace_replay_test` 的 `SPLIT` 分支。**并复核绝对路径能否活过 CI 的 artifact 搬运**：`.github/workflows/test.yml:174-185` 只重写 `CTestTestfile.cmake` 里的 `cmake` 路径，不重写 `ENVIRONMENT` 值——若不行，改为在测试启动时由 harness 相对 `argv[0]` 解析。

启动方式：`socketpair(AF_UNIX, SOCK_STREAM)` + `fork`/`execve`，fd 3 = socket（Windows 见 §11.5）。**无文件系统 socket 路径、无 abstract namespace、Android 上无 SELinux 争议。**

**子进程必须被强制成 monolith（本轮新增，修无界 fork 链）**：`MG_Config::Transport` 由 `ConfigLoader` 从环境变量读（与 `features.CoherentAsFlush = QueryEnvFlag(...)`（`ConfigLoader.cpp:185`）同形），而 `fork`/`execve` 的子进程会继承 `MOBILEGL_TRANSPORT=spawn`。server stub 里 `dlopen(libMobileGL.so)` + `dlsym("mobilegl_server_main")` 之后必然要起一个真 backend，即走 `MG_Backend::Init()`（`Init.cpp:48-70`）——变量还在，于是它再构造一个 `BackendObject_Remote` 并再 spawn 一次，首次 GL 调用时形成无界 fork 链。
**规则**：(a) spawn 时构造**显式 envp**，剔除 `MOBILEGL_TRANSPORT` 与所有 `MOBILEGL_IPC_*`（只保留 server 真正需要的少数几个，如 `MOBILEGL_BACKEND_TYPE`、日志路径）；(b) `mobilegl_server_main` 在能到达 `MG_Backend::Init()` 之前把 `MG_Config::Transport` 硬置为 `Monolith`。两条都做，任一条单独失效时另一条兜住。P0 增加一个 `MG_Test/Wire` 测试：spawn 一个 server 并断言进程树只多出**恰好一个**子进程。

`Hello{abiVersion, backendType, buildFingerprint, configBlob}` → `Welcome`。`configBlob` 转发 client 解析好的 `MG_Config::Features`，两半不可能对某个 quirk 开关有分歧。`buildFingerprint`（git hash + `Records.def` 的 hash）不匹配 → 握手期 `Fatal`。

### 11.2 `mobilegl_server_main` 的可见性（本轮新增）

`CMakeLists.txt:497-510` 在**非 Debug** 构建上给共享目标设 `C_VISIBILITY_PRESET hidden` / `CXX_VISIBILITY_PRESET hidden` / `VISIBILITY_INLINES_HIDDEN ON`——而 plugin 与 FCL 出货的正是 RelWithDebInfo（`MobileGL/build.gradle` 的 `fordebug` 类型强制 `-DCMAKE_BUILD_TYPE=RelWithDebInfo`）。所以 `dlsym("mobilegl_server_main")` 在 Debug 下能用、在设备上静默失败。

**规则**：入口点声明为
```cpp
extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv);
```
并在 P0 验收里加 `nm -D libMobileGL.so | grep mobilegl_server_main` 断言（与既有的 `nm --defined-only` 门并列）。若哪天 macOS/Windows 也要托管 server，还需同步 `MG_Impl/DyldInterpose/ExportedSymbols.txt` 与 `wgl.def`。

### 11.3 Android

**minSdk 26 没有任何公开 NDK API 能扁平化 `ANativeWindow`**（NDK r27.3 的 `android/native_window.h` 无 parcel 符号；`libbinder_ndk` 是 API 29，`binder_ibinder.h:191`；`ASurfaceControl` 是 API 29，`surface_control.h:67`）。`Feat/CS-Delta-IPC` 的 `nativeBlob`"binder-flattened ANativeWindow"（`protocol.fbs:377-379`）不可实现。

- **P1-P8 验证路径：无窗口。** 两个 PIE ELF。**实测**：从解压出的 nativeLibraryDir exec 在 API 36 上可行（`run-as … libtrace_replay_runner.so` → exit 132 = SIGILL，即 ELF 已被加载进入，而非 `EACCES`；文件 0755 / `u:object_r:apk_data_file:s0` 且无 MLS category，**跨 package 也可**）。`useLegacyPackaging = true` 在 FCL（`../FCL/build.gradle.kts:76-82`）与 plugin（`android-plugin/app/build.gradle.kts:198-203`）都已开。surface 用 pbuffer 或 `AImageReader` 支持的 `ANativeWindow`（`HeadlessGL.cpp:86-131,268-274`），trace replay 默认 pbuffer（`apitrace_glws_egl.cpp:614-618`）。
  **注意实测的域**：上述 SIGILL 证据是经 `run-as` 取得的，即 `runas_app` 域，而不是 trace Activity 所在的 `untrusted_app` 域。**P0 的 Android spike 必须从应用自身进程 `posix_spawn` 一次**（见 §15 P0）。
- **P9 生产路径**：Java `Surface`（Parcelable）→ Messenger/AIDL → `MobileGLServerService`（`android:process=":mgl"`）→ JNI `ANativeWindow_fromSurface(env, surface)`，就是 FCLauncher 今天在 `egl_bridge.c:81` 做的那一次调用。**仓内先例**：`android-plugin` 的 `BenchService` 已在 `android:process=":bench"` 里跑 MobileGL（`BenchService.java:19-77`）。代价：server 进程多一个 ART（~15-25MB）。
- **纠正一条过期笔记**：FCL 把游戏 JVM 跑在**主进程**，不是 `:jvm`（`../FCL/src/main/AndroidManifest.xml:112-121`，`JVMActivity` 没有 `android:process`；`:jvm` 是下载 Service）。第二个进程必须新建。
- **HeadlessGL 的 fork 预检与孤儿 server（本轮新增）**：`MG_IntegrationTest/Harness/HeadlessGL.cpp:344-368` 会 fork 一个子进程跑完整 EGL bring-up 然后 `_exit(step)`，注释（`:364-366`）明说这是刻意的——"every atexit handler and static destructor in this address space belongs to the parent's copy of the world"。拆分模式下那个子进程的 bring-up 会走到 `MG_Backend::Init()` 并 spawn 一个 server；`_exit` 不跑任何拆机，那个 server 成为孤儿，活到它发现 EOF 或撞上 `MOBILEGL_IPC_IDLE_EXIT_S`（默认 30s）。父进程随即对同一设备起自己的 server。`HeadlessGL.cpp:585-589` 已经把这种失败模式命名为"a leaked exclusive device, an environment the child did not have"。
  **规则**：server 的 EOF 检测必须**即时且无条件退出**（亚秒级，不靠 30s 看门狗）；client spawn 时把 socket fd 设成 `_exit` 会确定性关闭的形态（不设 `FD_CLOEXEC` 以外的保活）；再加一次**有界重试的就绪握手**，这样残留的预检 server 不会把父进程弄 flaky。这个交互本身列为 P1 验收步骤 1 的一部分，先于任何广度工作。

### 11.4 Linux / X11

`Window` 是 XID，`nativeToken:u64` 直接送。backend 自己 `XOpenDisplay(getenv("DISPLAY"))` 并构造 `VkXlibSurfaceCreateInfoKHR`（`VulkanRenderer.cpp:14486-14521`），只要同 `DISPLAY`/`XAUTHORITY` 就免费。Wayland 今天不支持（`BackendObject.h:529` TODO），维持。
WSL/CI：**永不开窗** —— `EGL_PLATFORM=surfaceless` + `EnsureHeadlessPlatform()`（`HeadlessGL.cpp:160-196`，它存在正是因为一台带 WSLg `DISPLAY` 的工作站曾把这条 lane 弄挂）。

### 11.5 Windows

`HWND` 进 `nativeToken`。Vulkan 可行（`hinstance` 是历史遗留，`VulkanRenderer.cpp:14456-14463`）；**WGL/ANGLE-DXGI 对外进程 HWND 不受支持 → headless only**。

transport：默认 named pipe（asio `windows::stream_handle`）。**"继承句柄就免掉 accept/connect"这句在 asio 上不能直接照搬（本轮修正）**：`windows::stream_handle` 的 IOCP 服务要求句柄是 **overlapped** 的，而 `CreatePipe` 造的匿名管道不是。所以句柄对必须这样造：用一个 GUID 唯一命名的 `CreateNamedPipeW(..., FILE_FLAG_OVERLAPPED)` 做 server 端，配一次 `CreateFileW(..., FILE_FLAG_OVERLAPPED)` 做 client 端，然后把 server 端句柄设为可继承并 `CreateProcess` 传下去。§11 必须把这套构造写清楚。

asio 1.38.2 在 Win32 上确实定义了 `ASIO_HAS_LOCAL_SOCKETS`（`3rdparty/asio/asio/include/asio/detail/config.hpp:1085-1092`，只排除 `ASIO_WINDOWS_RUNTIME`，且自带 `sockaddr_un_type` 于 `socket_types.hpp:220`），但其 IOCP `async_accept` 走 `AcceptEx`，AF_UNIX 从不支持它——AF_UNIX-everywhere 是 P6 的**可选简化**，需真编真跑验证，named pipe 是已知可用的默认。

### 11.6 崩溃

- **server 死**：client 读到 EOF/EPIPE → device-lost 闩锁：后续 GL 调用变 no-op、`eglSwapBuffers` 返回 `EGL_FALSE`+`EGL_CONTEXT_LOST`、`glGetGraphicsResetStatus`（若 robustness 分支落地）返回 `GL_UNKNOWN_CONTEXT_RESET`。`MOBILEGL_IPC_RESPAWN=1` 时重启 + `ResyncSnapshot`（默认关，静默重启会掩盖 bug；且与 `MOBILEGL_IPC_ADOPT_TIER != 2` 互斥，见 §5.8）。
- **client 死**：server 读到 EOF → **立即**销毁原生 context 并退出（不等看门狗）；`MOBILEGL_IPC_IDLE_EXIT_S`（默认 30）只作为 EOF 都收不到时的最后保险。

---

## 12. Monolith 保留与模式选择

**四层保证，从强到弱：**

1. **编译期折叠。** `MOBILEGL_BUILD_DISAGGREGATED`（默认 **OFF**）关闭时 `MobileGL/MG_Remote/**` 不进 `SOURCE_FILES`，`MG_Config::Transport` 是 `constexpr Monolith`，`MG_Backend/Init.cpp` 里的分支在编译期消失。**默认构建与今天字节一致。**
2. **唯一 hook 点。** 整个拆分入口是 `MG_Backend/Init.cpp:48-70` 里的一个分支：
```cpp
void Init() {
    MGLOG_D("Initializing MobileGL Backend...");
#if MOBILEGL_BUILD_DISAGGREGATED
    if (MG_Config::Transport != TransportKind::Monolith) {
        pActiveBackendObject = MakeUnique<MG_Remote::BackendObject_Remote>();
    } else
#endif
    switch (MG_Config::ActiveBackendType) { /* 原样不动 */ }
    if (!InitSpecificBackendLibs()) { /* 原样 */ }
    LogBackendInfo();
}
```
`BackendObject_Remote::GetBackendFunctions()` 返回发射表，`Initialize()` 负责 spawn/connect。下游 ~250 个边界调用点**零 `#ifdef`**。
3. **P4.5 的 allocator 改动必须同样包裹。** `PipeResource::MapAlignedAllocator` 与 `MipmapStorage` 的 level vector 住在 `MG_State`，改它们的 allocator 就改了类型；写成"分配器特化，option OFF 时逐字折叠回今天的 `MapAlignedAllocator`"，否则第 4 层会在 P4.5 变红。
4. **机械证明**：对 `libMobileGL.so` 做 `nm --defined-only` 与去调试信息后的 `.text` size diff，改前改后必须一致。**这是每个阶段的出口判据（P0…P9），不只是 P0**（上一版只在 P0 跑）。

### 12.1 两个 option，不是一个（本轮重大修正）

上一版说"OFF 时字节一致"，但**每一条部署路径都要求出货构建是 ON**：FCL 用户可编辑 env、plugin APK 的 V2 开关表、ctest `ENVIRONMENT` 变体、`/data/local/tmp` CTS 路径。而上一版又说 ON 构建里 `inproc` 会把 `pGLContext` 变成 thread-local 加 `operator->` shim。那个 shim 坐在全库最热的路径上：`grep -rho 'pGLContext->' MobileGL/MG_Impl | wc -l` = **1494**，加 DirectGLES 124、DirectVulkan 169。Android 上 dlopen 的共享库无法可靠使用 initial-exec TLS，每次访问会退化成一次 `__tls_get_addr` 调用，而今天那里只是一次对全局引用的加载（`Core.h:564` `extern UniquePtr<GLState::GLContext>& pGLContext`）。

**规则**：拆成两个 option。
- **`MOBILEGL_BUILD_DISAGGREGATED`**（出货形态）：只含 `spawn`/`unix:`/`pipe:`。每进程只有一个 `GLContext`、一份 `gBackendFunctionsTable`、一个 `pActiveBackendObject`、一份 `pDefaultFramebufferInfo` → 这四个**全部保持普通全局**，GL 热路径上没有任何 TLS 与间接。侵入面就是 `MG_Backend/Init.cpp` 里那一个可预测的分支。
- **`MOBILEGL_BUILD_DISAGGREGATED_INPROC`**（CI/调试形态，隐含开启前者）：额外加角色隔离 shim。

### 12.2 `inproc` 需要隔离的是**四个**进程全局，不是一个（本轮修正）

上一版只谈了 `pGLContext`。实际上 `inproc` 下同一进程要同时扮演两个角色，以下四个全局都必须按角色分身：

| 全局 | 定义处 | 谁读 |
|---|---|---|
| `MG_State::pGLContext` | `GLState/Core.h:564` 声明，`Core.cpp:1487` 定义，`Core.cpp:20` 构造，`Init.cpp:63` reset | 全部 |
| `MG_Backend::gBackendFunctionsTable` | `MG_Backend/Init.cpp:44` 赋值 | client 侧 MG_Impl（91 处）**与 server 侧 MG_Impl**（`GL_Texture.cpp:1621` `GenerateMipmap_Backend`、`:6713-6725` `GetTexImage` 回退链、`FixupGsStripCaptureOrder`、`CopyReadFramebufferIntoMipmapRegion` 的 `ReadPixels`） |
| `MG_Backend::pActiveBackendObject` | `MG_Backend/Init.cpp:53-61` 赋值 | MG_Impl 89 处 + backend 内部 |
| `MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo` | `GL_Framebuffer.cpp:3344` 定义，全库 22 处引用 | client 侧 MG_Impl 13 处（`GL_Framebuffer.cpp:495,1827,1837,1897,1905,1913,1927,1936,2549,2590,2598,2608,2611`）+ server 侧 backend 5 处（`DirectGLES.cpp:1917,2838,2867,9675`、`SwapchainObject.cpp:276`，其中 `SwapchainObject` 是**写**） |

一旦 client 装上发射表，`inproc` 里 applier 与 server 侧 MG_Impl 就没有任何路径能拿到真正的 DirectGLES/DirectVulkan 表；而一个进程也不可能同时持有 client 的 default-FBO 描述与 server 的（`SwapchainObject` 直接往里写 server 的视角）。

**shim 的完整需求**（上一版只提了 `operator->`）：`operator->`、`operator bool`、`get()`、`== nullptr` 相等比较、从 `MakeUnique` 赋值、`reset()`。非箭头用法的实际数量是 **133**（`grep -rn pGLContext MobileGL/ --include=*.cpp --include=*.h | grep -v 'pGLContext->' | wc -l` = 133，上一版写的"约 65 处"少了一倍），其中 MG_Impl 只有 2 处（`GL_Debug.cpp:99` 的 `.get()`、`GL_Program.cpp:1630` 的 `== nullptr`），绝大多数在 MG_Backend——尤其 DirectVulkan 里约 90 处 `MOBILEGL_ASSERT(MG_State::pGLContext, ...)` 的真值判断，另有 `DirectGLES.cpp:146` 的 `.get()` 与 `Managers.cpp` 里十来处 `if (MG_State::pGLContext)` 守卫。**因为 backend 侧那一簇恰恰是必须看到 replica 的，shim 的原型应当先拿 `MG_Backend/DirectVulkan/DirectVulkan.cpp` 的 assert 密集区开刀。**

**如果这层隔离的成本被判定过高**，退路是把 `inproc` 降级为**纯测试模式**：applier 通过显式传入的表指针工作，server 侧不跑 MG_Impl（于是 `GenerateMipmap_Backend` 那类回退不可用，需要在 `inproc` 下走另一条路径）。但那样 P2.5 就不再测量它本该测量的"monolith 渲染线程"交付物——**这个取舍必须在 P0 结束前拍板并写进文档，不能悬着**。

### 12.3 `inproc` 作为产品交付物

在隔离成本可接受的前提下，`inproc` 不只是测试脚手架：同进程第二个 `GLContext` + `mgl-srv-apply` 线程 = monolith 的渲染线程。今天 `PrepareForDraw`（状态调和、VAO/FBO/纹理/program/render-state sync、UBO ring memcpy）加驱动调用全部同步跑在 `glDrawElements` 里；把它们搬到 apply 线程，对 GL 线程 CPU-bound 的应用（本项目的 profiling 史说 Minecraft 就是）是**手上最大的单一杠杆**，且不需要任何 IPC/shm/平台工作。§15 的 P2.5 就是证伪它的门。

### 12.4 运行时选择与开关

`MOBILEGL_TRANSPORT = monolith(默认) | inproc | spawn | unix:<path> | pipe:<name>`，在 `ConfigLoader.cpp` 与既有开关并列解析。这一个选择免费换来：ctest `ENVIRONMENT` 变体、trace-replay 的 `setenv` 块（`trace_replay_core.cpp:134-207`）、FCL 的用户可编辑 env 偏好（`FCLauncher.java:417-430`）、plugin APK 的 V2 开关表（`android-plugin/app/build.gradle.kts:77-103`，由 `.github/scripts/validate-plugin-apks.sh` 校验）、`/data/local/tmp` CTS 路径。**零新增管线。**

保留全部既有负面对照开关（`MOBILEGL_ESPRYT_DISABLE_{UBO,UNPACK,UPLOAD}_RING`、`_INVALIDATE_FLUSH`、`MOBILEGL_DISABLE_LARGE_BUFFER_ADOPTION`），新增：`MOBILEGL_IPC_SHADOW_SHM`、`MOBILEGL_IPC_ADOPT_TIER`、`MOBILEGL_IPC_PROGRAM`、`MOBILEGL_IPC_INLINE_PAYLOADS`、`MOBILEGL_IPC_PRESENT_CREDIT`、`MOBILEGL_IPC_SPIN_US`、`MOBILEGL_IPC_POLL_ESCALATE`、`MOBILEGL_IPC_PERSISTENT_BLOCK_KB`、`MOBILEGL_IPC_SERVER_AFFINITY`。

---

## 13. 构建布局

```
MobileGL/MG_Remote/
  Protocol/  protocol.fbs  protocol_generated.h(提交)  Records.def  RecordKinds.h
             Handles.h  Coverage.def  MutationCoverage.def
             generated/BackendStateSurface.inc(提交)  generated/ImplMutationSurface.inc(提交)
  Transport/ ITransport.h  InProcessTransport.{h,cpp}  SocketTransport.{h,cpp}
             Framing.h  Ring.{h,cpp}  ShmSegment.{h,cpp} ShmSegmentPosix.cpp ShmSegmentWin32.cpp
             FdPassing.{h,cpp}  Doorbell.{h,cpp}
  Shared/    XfbAccounting.{h,cpp}   # client 与 applier 共用的 MG_Impl-side mutation helper
             MipmapLevelPlan.{h,cpp}
  Client/    WireMirror.{h,cpp}  EmitTable.cpp  EmitBufferOps.cpp
             BackendObject_Remote.{h,cpp}  CapsMirror.{h,cpp}
             ClientArrayBounds.cpp  CompositeResolver.cpp  ShadowArena.{h,cpp}
             PersistentMapTracker.{h,cpp}  GpuWritePending.{h,cpp}
             CoverageAssert.cpp  Surface/{X11,Win32,Android,Headless}.cpp
  Server/    ReplicaContext.{h,cpp}  Applier.cpp  ServerLoop.{h,cpp}
             ReplyPool.{h,cpp}  EventRing.{h,cpp}  ServerMain.cpp
  ServerJni.cpp                                  # Android，与 DriverPostJni.cpp 并列
scripts/     gen_protocol.py  gen_backend_state_surface.py  gen_impl_mutation_surface.py
MobileGL/MG_Test/Wire/CMakeLists.txt             # 复制自 MG_Test/Buffer/（27 行）+ MobileGL_Protocol
```

CMake：
- `MG_Remote/**` 仅在 `MOBILEGL_BUILD_DISAGGREGATED` 下追加进 `SOURCE_FILES`（`CMakeLists.txt:226-419`），因此 `MobileGL`（`:485`）与 `MobileGL_s`（`:552`）都拿到。
- `MobileGLServer`：桌面 `add_executable` 链接 `MobileGL_s`，`RUNTIME_OUTPUT_DIRECTORY` 设为 `$<TARGET_FILE_DIR:MobileGL>`（§11.1）；**Android** `add_executable` + `set_target_properties(MobileGLServer PROPERTIES PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "MobileGLServer")` 并链接**共享**的 `MobileGL`（一份 ~43MB 的 glslang/SPIRV-Cross/SPIRV-Tools），由 AGP 打进 `jniLibs`。server 主体是 ~30 行 stub：`dlopen(libMobileGL.so)` → `dlsym("mobilegl_server_main")`（可见性见 §11.2）。**一份共享库、两个角色，版本必然匹配**（对比 `Feat/CS-Delta-IPC` 的四件必须互相匹配的产物）。
  **AGP 能否打包一个被改名成 `lib*.so` 的 `add_executable`，是 P0 spike 的验证项之一**（`MobileGL/build.gradle` 没有设 `targets` 列表，上一版把这条当成已知事实）。
- **FlatBuffers**：submodule `3rdparty/flatbuffers` 置于既有的 `if (EXISTS .../flatbuffers/CMakeLists.txt)` 保护下，**去掉 `if (NOT ANDROID)` 一刀切**。因为 `protocol_generated.h` 已提交，**默认构建图里没有 `flatc`，也不 `add_subdirectory(3rdparty/flatbuffers)`**（§7.1）。运行时是 header-only，只需要 `3rdparty/flatbuffers/include` 在 include path 上。
  **guard（本轮新增）**：若 `MOBILEGL_BUILD_DISAGGREGATED=ON` 而 `3rdparty/flatbuffers/include` 不存在，强制把该 option 设回 OFF 并 `message(WARNING ...)`——否则 `MG_Remote/**` 已经进了 `SOURCE_FILES` 而头文件找不到，构建以一个莫名其妙的错误失败（现有的 `EXISTS` 保护只包住 Protocol 子目录）。
  `MOBILEGL_FLATC_EXECUTABLE` 只服务 CI 的 `flatc-check`，经 `MobileGL/build.gradle:17-21` 已在用的 `externalNativeBuild { cmake { arguments } }` 槽传入。
- 测试接线：
  - `MG_Test/Wire/`（label `unit`）→ 现有 CI `test` job 自动收，**无需改 workflow**。
  - `MG_IntegrationTest/CMakeLists.txt` 每 backend 增加一条 `gtest_discover_tests`（`TEST_PREFIX "DirectGLES.Split."` / `"DirectVulkan.Split."`），**必须用 `mgl_itest_join_environment(... ${MGL_ITEST_COMMON_ENV})` 构造**，并带上 `MOBILEGL_IPC_SERVER_PATH`。三个已被文档记录的陷阱要遵守：ctest `ENVIRONMENT` 是**替换而非追加**（`:339-343`）、`;` 必须转义（`:322-332`）、property 覆盖 job env（`test.yml:253-262`）。
  - **trace replay 的 `SPLIT` 接线（本轮补细节）**：`add_trace_replay_test` 今天把测试命名为 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}`（`tools/trace_replay/CMakeLists.txt:330-332`），加一个 `SPLIT` 参数会与同 case+backend 的现有测试**重名**。改成 `MobileGLTraceReplay.${CASE_NAME}.${BACKEND}${SPLIT_SUFFIX}`。另外该测试的命令是 `cmake -P run_trace_case.cmake` 加约 18 个 `-DTRACE_*` 变量，所以还要加 `-DTRACE_TRANSPORT=` 并在 `run_trace_case.cmake` 里消费它——**这两个文件都要列进 P2 的交付物**。
- CI 新增三个 step：`flatc-check`（重生成 `protocol_generated.h` + `git diff --exit-code`）、`coverage-check`（重生成两个 `.inc` + `git diff --exit-code`）、`monolith-abi-check`（OFF 构建与 ON+monolith 构建的 `nm --defined-only` / `.text` size 对基线）。
- **CI 新增一条 grep 门**：禁止 `MG_Backend/` 与 `MG_State/` 下出现 `fprintf(stderr` / `printf(`。

---

## 14. 对 `Feat/CS-Delta-IPC` 的复用清单

### REUSE（原样取）
| 路径 | commit | 备注 |
|---|---|---|
| `docs/CS_Refactor/HandleSessionGeneration.md` | `546895aa` | 分支上最好的产物。三处修改：handle 清单补 `RenderbufferObject::GetLifetimeId()` **与 `GetVersion()`**；把第 2 节的 server 侧 share-group 要求降为 v2；把"lifetimeId 不符 → 销毁重建"改成 `Fatal`（§5.4） |
| `MobileGL/Protocol/mg_protocol_base.h` | `546895aa` | 干净无依赖的词汇（`MobileGLResult`、span、`ShmRegion`、id typedef、structSize-first 版本纪律） |
| `MobileGL/Protocol/tests/ProtocolSmoke.cpp` | `546895aa` | schema 往返门（默认改 ON） |
| 根 `CMakeLists.txt` 的 `EXISTS` 保护 + `.gitmodules` 条目 | `546895aa` | 去掉 `NOT ANDROID`，另加 §13 的 include-dir guard |
| `docs/CS_Refactor/HANDOFF.md` 第 6 节"已知坑清单" | `d5c00b9d`/`5964628d` | 逐字留作事后复盘：路径转换、versionCode 降级、双设备 `ANDROID_SERIAL`、flatbuffers camelCase accessor、union vector 产生指针、Release 下 `MGLOG_D` 被编译掉、嵌套 submodule 配方、`assembleTraceDebug` 改名 |

### CHANGE（取走并改造）
| 路径 | commit | 改造 |
|---|---|---|
| `MobileGL/Protocol/protocol.fbs` | `546895aa` | 保留 delta 目录、`RenderStateBlob` 整块思想、`BufferShmAdopt`、命令清单、事件分类学。改：热路径转 `struct` + ring；删掉冗余的 `inlineBytes`/`data` 双胞胎（`:111-112`、`:125-126`，两半代码对哪个字段是真的意见不一：`ServerCore.cpp:184-208` 只读 `data`，`StateEmitter.h:60,111` 只写 `inlineBytes`）；加 `ResyncSnapshot`、`AuxRequest`；给 `ProgramPublish.reflection` 与 `ObjectCreate.params` 真 schema；kind 枚举生成 + 每 kind `static_assert` + 运行期边界检查 |
| `MobileGL/Protocol/CMakeLists.txt` 的 flatc 解析 | `546895aa`/`65717b4c` | **不再照搬**：`add_subdirectory(3rdparty/flatbuffers)` 从默认路径整段删除（它就是那个 NDK 陷阱本体）；只保留 `MOBILEGL_FLATC_EXECUTABLE` 供 CI；`enable_testing()` 移到根 |
| `MobileGL/ServerCore/ServerCore.{h,cpp}` | `65717b4c`+`c2260dd8` | 保留握手→解码→apply→credit 形状与 plugin manifest loader 思路。修：单次校验 + 零拷贝解码（今天校验两次外加一次整体拷贝，`:492-498` 与 `:218-221`）；io/apply 分线程（`:404-406` 自承 worker 从未落地）；完整事件集（`SendEvent` 只实现 `BATCH_APPLIED`，`:373-382`）；credit 用最后一条实际 seq（`:427` 的 `baseSeq + items.size()`）；接收缓冲不能是对着 64MiB 帧上限的固定 4MiB（`:478`）；真正的段生命周期（`m_segments` 只增不减，`blobOwners` 只 push 不释放） |
| `ServerCore/tests/LoopbackSmoke.cpp` + `Backends/Dummy/` | `65717b4c` | 分支上最便宜的端到端门，**第一个重建**，重定向到真 applier |
| `MobileGL/Remote/InProcessTransport.h` | `65717b4c` | 重表述在 C++ `ITransport` 上；单侧 shutdown（今天 `:89-92` 连对端 inbox 一起关）；真段生命周期（`Unmap`/`Close` 今天是 no-op）；补 §6.2a 的双向 doorbell（condvar 版） |
| `MobileGL/Remote/Framing.h` | `65717b4c` | 保留帧格式；`m_pendingSize`/`m_haveHeader` 改 `mutable`（今天 `const_cast`，`:81,85`）；`Feed()` 真校验 magic 与长度（今天永远返回 OK，坏 magic = 静默永久挂起）；缓冲不足返回所需大小且**保留消息**；真正在 socket transport 里使用它（今天是死代码） |
| `MobileGL/RemoteClient/StateEmitter.h:39-307`（**仅 emit 半边**） | `b50f3348`+`d96be9f3` | 各域字段遍历是真知识，抬进 `WireMirror`/`ResyncSnapshot`。GL name 换 `lifetimeId`（今天 `:48-49,85,166-168,203,230` 全把 GL name 塞进 `handle`）；`:175-181,:244-249,:253-258,:293-298` 的 O(n²) 线性扫描换 handle map；固定 6 attachment（`:232-236`）换 `MaxColorAttachments`；补上被跳过的 texture view（`:70-74`）。**不取 applier 半边（`:312-501`）** |
| `scripts/extract_backend_read_inventory.py` | `546895aa` | 改造成 `gen_backend_state_surface.py`：删掉前缀兜底（`:234-241`），未知 accessor 一律 UNMAPPED 并**编译失败**；把真 pull point 与 signature handle 化分开统计。**另写一个全新的 `gen_impl_mutation_surface.py`**（§5.9b），它在原分支没有对应物 |

### DROP
| 路径 | 理由 |
|---|---|
| `MobileGL/Protocol/bfa.h`（480 行） | "strict C ABI"不是 C ABI：`ServerCore.cpp:177-179` 把 FlatBuffers 生成表的指针交给插件，插件必须是 C++ 且链接 FlatBuffers（`StateEmitter.h:330,351,362,372` 就是这么用的）。手抄的 60 字段 `MobileGLDynamicParameters`（`:63-129`）自承尾部不全、同步脚本从未写过——正是已在本项目造成 481 例 CTS 失败簇的那类数据的**长期静默漂移炸弹**。而本设计根本不需要 delta-apply vtable |
| `MobileGL/Protocol/mgruntime_api.h` + `MobileGL/UtilRuntime/*` | 360 行契约对 ~50 行实现（8 域实现 2 域）；唯一消费者传 `nullptr`（`ServerCore.cpp:61`）；缓存每次命中整份拷贝（`:79`）、按 `clear()` 淘汰（`:91-93`）；smoke 断言 `api->metrics == nullptr`（`RuntimeApiSmoke.cpp:66`）。它的唯一理由随 BFA 消失；且本设计里翻译全在 server（它无论如何要链 SPIRV-Cross），glslang 全在 client |
| `MobileGL/Remote/LocalSocketTransport.{h,cpp}`、`ShmFactory.{h,cpp}` 实现 | 从未被任何测试执行（`LoopbackSmoke` 用的是 `InProcessTransport`，唯一另一个消费者 `ServerHost` 编译不过）；每次 send 都 use-after-free（`:199`，`asio::buffer(next)` 指向局部 vector 而 lambda 捕获的是另一份拷贝）；按 wire 长度无上限分配（`:232-236`）；`Start` 里阻塞 accept/connect（`:116`、`:139-144`）；无 strand 且 `framesSent++` 非原子（`:177-178`）；**且完全没有 POSIX fd 传递**（`:296` 硬编码 `fd=-1`），Linux/Android 数据面一字节过不去。只保留 `ShmFactory.h:4-12` 作平台矩阵规格 |
| `MobileGL/ServerHost/main.cpp` | 编译不过（`:31,39,44,53-54` 对指针用 `.`，`c2260dd8` 改返回类型后成为死码）。`MobileGLServer` 在默认 ALL target 里，**分支 tip 无法完成一次完整构建** |
| `MobileGL/RemoteClient/tests/StateEquivalenceTest.cpp` | 把 delta apply 进第二个 `MG_State::GLContext`——验证的是它自己的 thin-server 前提说不该存在的数据路径；与生产 apply 路径零共享代码；只测全量 resync；`d96be9f3` 声称五域逐字段而文件只比了纹理、buffer、render-state blob、buffer binding slot（没有 VAO 属性/FBO attachment/RBO 格式比较） |
| `c7c9e346` + `29d721ef` 全部（share-group sessioning） | 非 v1 前提（monolith 只有一个 `GLContext`：`GLState/Core.cpp:20,1487`）；且非可合并质量：`VertexArrayState.cpp:+20-26` 往已共享的表里再压一个 default VAO 并重复 `Insert(0)`；四个头文件 `public:` 未复位泄漏私有成员；current session 是无锁进程全局，连它自己的 per-thread current 都没兑现；在状态权威里塞 `MOBILEGL_SESSION_SWAP` env kill switch 与 `s_defaultAdopted` 偷 context 的 hack。日后作为独立 PR 带多 context 测试落 `dev` |
| `b50f3348` 的 `RenderState::InstallParameters` + `public:` | 本设计不需要 Install setter（D3）；若日后需要整块安装，用正确作用域的方法或单条 friend，绝不靠裸 `public:` |
| `d96be9f3` 的 TRIAGE 指令（`DirectGLES.cpp:+2583-2590`） | per-draw `fprintf(stderr)`。**分支上每一次测量都跑在它上面。** 同规则适用于当前工作树的 `[IBOTX]`/`[BUFTX]`（P0 清除） |
