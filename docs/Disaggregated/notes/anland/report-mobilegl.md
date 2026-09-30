# MobileGL → Anland：只读代码盘点

审计日期：2026-09-30。对象：Arch WSL 的 ~/w7/p9-main。实际分支为 **p8/main**，HEAD **648800a2da231e8152760d400da33e500d511ca6**，提交说明为关闭 P8；不是任务描述里的 feat/disaggregated。依据是本次只读 git status / git branch --show-current / git log -1；未切换分支。路线图也将 P8 标记完成（docs/Disaggregated/ROADMAP.md:3、:36）。所有源码引用均为此目录、此时点的树根相对路径。未构建、未执行测试或设备操作；“已验证”只指被引用的历史证据。Anland 的格式、modifier、同步和部署条件采用题目给定前提，未另行审计 Anland。

**结论：拆分 GL 执行已存在，Anland 所需的窗口系统与帧交付链尚不存在。** 已有 Linux client → Android vendor-driver server 的历史跨机验收；当前 P12 在服务器自己的 SurfaceView 上呈现，未把每帧 dma-buf / fence fd 交回 Linux client。要作为通用 Linux desktop GL provider，仍需 Linux WSI、可交付图像、跨进程同步、buffer 生命周期、多会话/多窗口与真实 context/share-group 支持。前两句有代码/验收直接依据；“所需工作”是 **inferred**，详见各节。（docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:3–17；MobileGL/MG_Remote/CONTRACT-P12.md:7、:16、:44；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:17252–17283；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:14528–14547）

先纠正名称：**Espryt = DirectGLES（原生 EGL/GLES）；Magma = DirectVulkan（Vulkan）**。题目第 3 项括号中的两个后端正好相反。（MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:812–822；MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:543–551）

## 1. Linux client 构建、库与加载器

### 1.1 哪些平台能确认

| 平台 / 问题 | 当前结论及依据 |
|---|---|
| Linux glibc x86_64 / WSL | **有实际 client 证据**。P6.5 明确部署 x86_64 WSL client ↔ aarch64 Redmi server；P12 的 WSL trace client 对 Android DirectGLES/stream 已验收 OpenRA 和 rd12。它证明 glibc client 可运行和跨 ABI 的特定组合可工作，不证明任意 Linux GUI 程序已可在 Anland 上显示。（docs/Disaggregated/notes/p65/README.md:61、:93；docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:3–17） |
| Linux CI | 存在 Ubuntu split build，clang/clang++ 20、Release、disaggregated/inproc/push 和 integration tests 配置。这里没有重跑 CI。（.github/workflows/test.yml:1206、:1260、:1269–1296） |
| Linux glibc aarch64 | **未找到该组合的现成 preset 或运行验收，不能写成“已验证支持”**。Linux POSIX transport 路径不限制 x86，wire 已为 x86_64↔aarch64 固定宽度/布局校验；因此原生或交叉 glibc aarch64 构建是合理移植目标，属于 **inferred**。Android arm64 NDK 构建不能替代 glibc arm64 验收。（CMakeLists.txt:177–183、:503–545、:783–835、:954–962；MobileGL/MG_Remote/Transport/ShmSegmentPosix.cpp:61–77；docs/Disaggregated/notes/p65/README.md:93；docs/Disaggregated/notes/OPEN-QUESTIONS.md:28） |
| “纯 client”库 | 当前是同一 libMobileGL 内编入两种角色，remote client 不构造本地具体 backend；server 可执行文件也链接这份库。没有产品级 client-only / server-only CMake 目标开关。**inferred**：精简 glibc client 的链接依赖是可选后续工作，而不是使用 split 的先决条件。（MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:22–26、:98–105；CMakeLists.txt:594–623、:783–785、:1033–1040；docs/Disaggregated/design/07-memory-readback-verification.md:27） |

可复用的配置形状（仅示意，本次未执行）：

    cmake -S . -B build-split -G Ninja
      -DCMAKE_BUILD_TYPE=Release
      -DMOBILEGL_BUILD_DISAGGREGATED=ON
      -DMOBILEGL_PIPE_PUSH=ON
      -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
      -DMOBILEGL_BUILD_BENCHMARK=OFF

INPROC 是测试/同进程两角色形态的超集，不是跨进程 client 必需项；INPROC 隐含 DISAGGREGATED，后者隐含 PIPE_PUSH。FlatBuffers submodule runtime header 缺失时 CMake 会警告并把 split 在本次配置中关闭。根目录/两层查找未见 CMakePresets 文件；项目说明和 CI 都用直接 CMake 选项。（CMakeLists.txt:23–30、:503–545；docs/Disaggregated/guide/build-and-run.md:7–12；.github/workflows/test.yml:1275–1293）

工具链最低 CMake 3.22.1、C++23；glslang、SPIRV-Cross、SPIRV-Tools、SPIRV-Reflect、xxHash、VMA、Threads 被链接，非 Android 配置还查找 Vulkan。CI 安装 Mesa EGL/GLES/Vulkan 开发依赖用于主机后端和测试。aarch64 glibc 应用同样需合适 sysroot/工具链并验证 wireFingerprint；目前没有证据可指定一个现成 arm64 glibc preset。（CMakeLists.txt:1、:177–183、:277–283、:719–731、:954–962；.github/workflows/test.yml:1260；docs/Disaggregated/design/06-transport.md:64；最后一句为 **inferred**。）

### 1.2 产物与 SONAME

| 产物 | 源码能确认的事实 |
|---|---|
| libMobileGL.so | CMake 项目 MobileGL 的 SHARED target；GL、EGL、Linux GLX 都在这份库里。（CMakeLists.txt:3、:401–406、:783–785） |
| libEGL.so、libEGL.so.1 | 非 Android、非 Apple UNIX 构建后创建的符号链接，指向上述 target。代码实际只列了这两个 alias。（CMakeLists.txt:825–834） |
| libGL.so / libGL.so.1、libOpenGL.so、libGLESv2.so | 未见同等构建 alias / 独立输出目标。不能因为 comment 写“GL/EGL loaders”就称已经产出 libGL；使用 libGL 的系统程序还需 loader 包装/部署策略，后半句为 **inferred**。（CMakeLists.txt:825–834；GL 导出见 MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:26–59） |
| libMobileGL_s.a | NOT ANDROID 下另建 MobileGL_s STATIC，同一源集合。（CMakeLists.txt:849–881） |
| libMobileGLServer.so | 实际是 PIE **executable**，不是供应用当 EGL vendor dlopen 的库；Android 与 desktop 都强制 lib 前缀、.so 后缀，链接 libMobileGL 并设 $ORIGIN RPATH。（CMakeLists.txt:1033–1085） |
| ELF SONAME | 未发现 VERSION/SOVERSION 或 SONAME override。按 CMake Linux 默认，预期 SONAME 为 libMobileGL.so（**inferred，未读 ELF 动态段**）；libEGL.so.1 是文件系统 alias，不证明 ELF SONAME 变成 libEGL.so.1。（CMakeLists.txt:783–835） |

### 1.3 API 导出与 GLVND

GL/EGL/GLX 导出宏都是 extern C + default visibility；EGL 导出覆盖 display/config/context/current、window/pbuffer/pixmap、swap、查询、EGL 1.5 sync/image/platform 接口，以及 eglGetPlatformDisplayEXT。除通用 glXGetProcAddress/ARB 包装外，具体 GLX 导出在 Linux 非 Android 条件下编译，包含 glXCreateContextAttribsARB（MobileGL/MG_Impl/GLXImpl/Exporting/Definitions.cpp:12–23；MobileGL/MG_Impl/GLXImpl/GLXImpl.h:38）。大量 GL core/extension 名称也被导出，但部分明确是 stub；“dlsym 成功”不能当能力证明。（MobileGL/Defines.h:33–44；MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp:12–25、:112–155、:198–240；MobileGL/MG_Impl/GLXImpl/GLXImpl.h:12–39；MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:26–35、:152、:395–429）

两个 backend 的目标/报告 GL 和 GLSL 都为 **4.6.0**，静态 identity 的 IsCompatibilityProfile=false；前端 GL_VERSION 直接从这个 TargetGLVersion 生成，而不是按当前 EGL bound API 切换 OpenGL ES 字符串。README 仍写短期目标 OpenGL 4.2 Core，不能把这些数字解释为“已完成 GL 4.6 conformance”。GLX 可接受 compatibility context 请求，但固定功能不完整，见第 7 节。（MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:817–822；MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:546–551；MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp:619–638；README.md:49–56）

**没有查到 libglvnd vendor 集成**：全项目检索没有 MobileGL 的 __egl_Main / __glx_Main 定义或 MobileGL egl_vendor.d JSON；目前是直接导出入口、libEGL alias / 由应用指定 MobileGL 加载的形态。CI/测试出现 egl_vendor.d 是选择服务器侧 Mesa/NVIDIA 原生 EGL vendor，不是注册 MobileGL 为 vendor。（CMakeLists.txt:825–834；MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp:12–127；MobileGL/MG_IntegrationTest/CMakeLists.txt:280–299；MobileGL/MG_Benchmark/Driver/run_driver_bench.sh:19）

**inferred：**Anland 集成需要决定“私有库搜索路径下直接替代 GL/EGL”还是实现 GLVND vendor ABI；已有 GLX 入口不会自动让系统 GLVND 将所有 GLX 应用派发给 MobileGL。依据为上述 alias 和导出形态。

## 2. EGL frontend 与 GLX 的实际能力

### 2.1 平台与 native window

- eglGetDisplay 将 native display key 登记为 EGL_NONE 平台；eglGetPlatformDisplay 将任意 platform/native key 放进显示表，前端并没有按 Wayland/X11/GBM 平台进行真实 WSI 分派。因此“返回非空 EGLDisplay”不构成该平台支持。（MobileGL/MG_State/EGLState/Core.cpp:316–324；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:708–715）
- window backend 按**编译平台**决定：Android / MetalLayer / Win32 / Linux X11；Linux 不是运行时看到 EGL_PLATFORM=wayland 就变为 Wayland。没有 wl_display、wl_egl_window 或 gbm_surface 的实现分支。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:64–75、:744–770；未发现这些 WSI 集成的检索结论也与 docs/Disaggregated/design/08-runtime-and-platform.md:32 一致。）
- EGL_PLATFORM=surfaceless 是 headless 测试 harness 在未显式设置时写给原生 Mesa loader 的环境变量；DirectGLES 后端仍调用原生 eglGetDisplay(EGL_DEFAULT_DISPLAY)。这不是 MobileGL frontend 完整实现了所有 EGL platform。（MobileGL/MG_IntegrationTest/Harness/HeadlessGL.cpp:172–191；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16587–16593）
- Linux **并非源码只有 surfaceless**：有真实 X11 window 代码，GLX 查询 XGetGeometry，Magma 创建 VkXlibSurfaceKHR；而拆分验收/WSL/CI 的平台政策是不打开窗口。Linux-local X11 支持和“Linux client → Android server → Anland”是不同能力，后一个并未打通。（MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:258–327；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:16450–16500；docs/Disaggregated/design/08-runtime-and-platform.md:32）
- “surfaceless 平台”也应与 EGL_NO_SURFACE context 区分：backend MakeEGLCurrent 要求注册 surface、draw=read，并拒绝 draw/read 为 EGL_NO_SURFACE 的有效 context bind。因此不能宣称完整 EGL_KHR_surfaceless_context；其广告表也没有该扩展。（MobileGL/MG_Backend/BackendObject.cpp:314–338；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:465–473）

### 2.2 Config 与 surface

Config 是前端合成值：每 display 两个默认 config（stencil=0 和 8），RGBA 各 8、depth 24；surface bits 宣告 WINDOW|PBUFFER|PIXMAP，renderable bits 宣告 OPENGL|ES2|ES3；swap interval 0..4；Linux visual 来自 X11 查询，Android visual ID 是 RGBA8888 AHB format 常量。它们不是由每个 frontend config 对等选择的 vendor config 完整镜像。（MobileGL/MG_State/EGLState/Core.cpp:153–182；DirectGLES 实际另选 native config：MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16500–16531、:16593）

查询还固定返回 EGL_RGB_BUFFER、EGL_CONFIG_CAVEAT=EGL_NONE、EGL_NATIVE_RENDERABLE=TRUE、EGL_SAMPLES/SAMPLE_BUFFERS=0，EGL_CONFORMANT 直接等于 RenderableType；这些广告不补足尚缺的 ES/pixmap 实现。（MobileGL/MG_State/EGLState/Core.cpp:535–599；第 2.2、2.3 节实现证据）

| Surface / API | 实际路径 |
|---|---|
| Window / platform window | 创建前端状态后调用 backend CreateEGLWindowSurface；split 经 SurfaceOp。普通模式拒绝空 native window；ServerOwned 可为空，且丢弃客户端窗口 token。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:121–204、:718–776） |
| Pbuffer | 创建前端状态，随后真调用 backend CreateEGLPbufferSurface；缺省 width/height=1；原生实现见第 3 节。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:498–520） |
| Pixmap / platform pixmap | frontend 只建 SurfaceObject，EGLImpl 没有配套 backend pixmap 创建；没有 GLX pixmap / DRI pixmap 通路可据此认定。**inferred：**广告 EGL_PIXMAP_BIT 高于当前可证明的原生能力。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:580–587、:801–807；MobileGL/MG_State/EGLState/Core.cpp:886–915、:991–1020；MobileGL/MG_Backend/BackendObject.cpp:324–338） |
| PbufferFromClientBuffer | 只转给 EGLState 登记，并无对应 backend 导入调用。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:571–578；MobileGL/MG_State/EGLState/Core.cpp:922–956） |
| BindTexImage / ReleaseTexImage / CopyBuffers | 检查句柄/参数后直接成功，未进行纹理绑定或 native pixmap copy；不是可用的图像共享捷径。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:523–568） |

### 2.3 Context、GLES、sharing 与线程

eglBindAPI(EGL_OPENGL_ES_API) **返回成功**，EGL_CLIENT_APIS 也写 OpenGL OpenGL_ES；甚至 OPENVG_API 也被接受。CreateContext 只检查 display/config/shareCtx 的存在，存 ClientAPI、version、profile、flags，然后铸造 frontend handle；不会在这里选择 ES 实现或创建原生 share context。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:240–245、:414–428、:463–464；MobileGL/MG_State/EGLState/Core.cpp:615–689）

所以答案是：**有 ES API 接受和 ES2/ES3 config bits，但没有证据可称为完整 GLES provider、或给出一个经确认的 frontend GLES 最大版本。** Espryt 内部确实请求原生 ES 3.2→3.1→3.0，但那是实现 desktop GL 的 backend context，不等于应用请求的独立 GLES context。GL_VERSION 路径依然返回 desktop target；glGetShaderPrecisionFormat 还是 stub。（MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16591–16612；MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp:619–626；MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:152）

前端 EGLState 有多个 context handle、per-thread current/error/API 表、context owner 检查；shareCtx 被存为 SharedContext。然而 GL 状态仍通过全局 pGLContext，active backend 是全局单例，Espryt 只有一套 g_Display/g_Context/g_Surface，Magma 只有全局 pVulkanRenderer。协议 SurfaceOp 没有 CreateContext/DestroyContext/share-group 消息。**inferred：**多个 EGL handle 可创建、context 可在线程间迁移，不等于 N 个独立 GL context 或正确 share-group 隔离；多线程同时发 GL 命令的通用实现不能视为已支持。（MobileGL/MG_State/EGLState/Core.cpp:101–126、:637–641、:1204–1278；MobileGL/MG_State/GLState/Core.h:701；MobileGL/GlobalObjects.cpp:23–24；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16183–16185、:16685；MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:38–39；MobileGL/MG_Remote/Protocol/protocol.fbs:240–258）

文档明确把多 context 留作未做；INPROC 角色隔离也明确不是给 1494 个 pGLContext 读点加 TLS。服务器 native context 常驻 apply 线程，客户端 ReleaseCurrent 只改变逻辑 live 状态，不真实解绑 native context。（MobileGL/MG_Remote/CONTRACT-P12.md:64；docs/Disaggregated/design/09-build-switches-counters.md:21；MobileGL/MG_Remote/Server/ServerLoop.cpp:330–391）

### 2.4 EGL extension 广告与真实实现

完整广告字符串如下，没有隐藏的 dma-buf/fence/damage/buffer-age 附加表：（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:458–473）

| QueryString 范围 | 广告 |
|---|---|
| EGL_NO_DISPLAY | EGL_EXT_client_extensions、EGL_EXT_platform_base、EGL_KHR_platform_base、EGL_MESA_platform_surfaceless |
| initialized display | EGL_KHR_create_context、EGL_MESA_platform_surfaceless |

以下均**未广告**：EGL_EXT_image_dma_buf_import、EGL_EXT_image_dma_buf_import_modifiers、EGL_MESA_image_dma_buf_export、EGL_KHR_fence_sync、EGL_ANDROID_native_fence_sync、EGL_KHR_swap_buffers_with_damage、EGL_EXT_swap_buffers_with_damage、EGL_EXT_buffer_age、EGL_KHR_surfaceless_context。（同上 MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:465–473）

eglCreateImage 虽有入口，却忽略 attribList，仅保存 target/buffer/context 到 map，不调用 driver 或解析 plane fd/offset/stride/modifier。eglCreateSync 只接受 EGL_SYNC_FENCE，创建时直接标 EGL_SIGNALED；ClientWaitSync 直接返回 EGL_CONDITION_SATISFIED，WaitSync 只验证归属。因此这些入口不能用作 Anland 的 dma-buf import/export 或 explicit sync。include/EGL/eglext.h 中的声明也不能改变这一事实。（MobileGL/MG_State/EGLState/Core.cpp:1336–1359、:1378–1394、:1430–1466；include/EGL/eglext.h:784–829、:1103–1111）

### 2.5 GLXImpl 是否是 stub

**不是纯 stub，也不是 Mesa DRI/DRI3 provider。** 它是 MobileGL EGL 上的一层 GLX facade；动态加载 libX11 并解析 XGetGeometry、XGetVisualInfo、XDefaultScreen/Visual、XVisualIDFromVisual、XFree、XSync；按 drawable 建立 EGL surface，swap 前最多每 250ms 查询一次几何并 resize，再调用 EGLImpl::SwapBuffers。没有查到实现 X11 GLX wire rendering、DRI loader driver 或 DRI3/Present buffer 交付的代码。（MobileGL/MG_Impl/GLXImpl/GLXImpl.h:12–17；MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:129–149、:258–327、:956–965、:1019–1023；后一个“没有”是对本实现及检索的结论。）

它的 QueryVersion 固定答 GLX 1.4，QueryServerString 复用本地 client string，并未向 X server 查询 GLX 能力。（MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:543–571）它广告 GLX_ARB_create_context、_no_error、_profile、ARB_get_proc_address，以及 EXT/MESA/SGI swap_control；CreateContextAttribsARB 把版本/profile/flags 翻译为 MobileGL EGL 属性。legacy CreateContext/NewContext 默认请求 3.3 compatibility。GLXWindow 就是输入 XID；draw/read 不同时实际用 draw 同时充当 read，却记录用户 read handle。（MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:84–86、:785–883、:941–953、:968–979）

**inferred：**这可作为将来 Linux X11 frontend 的复用起点；对 Anland/Xwayland，需要新增本地 X drawable 的图像提交机制（例如 DRI3/Present，或先用 CPU image copy），不能把 XID 发给 Android 后端当 ANativeWindow，更不能仅把 ServerOwned 打开就得到每个 X 窗口的内容。依据为该 facade、下面 Rule H / native window 路径。

## 3. Split 模式的 present 路径

### 3.1 共用调用链

1. 导出的 eglSwapBuffers → EGLImpl::SwapBuffers，检查 surface，进入 BackendObject_Remote::SwapEGLBuffers。
2. Remote 调用 base SwapEGLBuffers，验证当前线程、display、唯一 active surface，再通过 GetBackendFunctions().Present 调用 client emitter；不额外调用 ServerSwapEGLBuffers。
3. EmitPresent 先获取 present credit，铸造 FrameSerial，再 EmitAndWait(Present)；MGPPresent 本体**只有 FrameSerial**。
4. server 解码至 ServerVerbSink::OnPresent → backend table Present()；返回后更新 frame serial、报告已完成 GL fences、ReturnPresentCredit。
（MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp:112–117；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:207–228；MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:396–404；MobileGL/MG_Backend/BackendObject.cpp:369–397；MobileGL/MG_Remote/Client/EmitTables.cpp:1165–1188；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809；MobileGL/MG_Remote/Server/PipeApplier.cpp:388–438）

没有一条 present 回包包含 image/buffer fd、fence fd、surface id、format、stride、offset 或 modifier。它目前确认服务器本地 Present 已返回，不是“Linux compositor 已经采用或释放某个 buffer”。（上述 MGPPresent / OnPresent；后一语义解释为 **inferred**。）

### 3.2 三类 surface

| 模式 | 创建/目标/呈现 |
|---|---|
| (a) Client-owned native window | Remote 先 ServerSetWindowHandle 再 ServerCreateEGLWindowSurface；每个 EGL forwarder 先等已提交命令 apply，避免跨批次乱序。（MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:178–220）Rule H 永久禁止 peer-window-system-local value 跨进程，不是等 P12 将来补。（MobileGL/MG_Remote/CONTRACT-P6.md:31–45）wire 的 AndroidNativeWindow 和 MetalLayer 被 decoder 拒绝、以 UnmigratedSurface 闩会话。（MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:216–224、:303–314） |
| (a) X11/Win32/None 的残留例外 | decoder 仍接受这些 token，UnpackWindowHandle 强转 void*；Linux-local Magma 可用 XID+XOpenDisplay 建 Xlib surface，但 Android build 创建的是 VkAndroidSurfaceKHR。**inferred：**被 decoder 接受不是容器 XID 对 Android 有效，更不是 Anland 集成，存在未封住的窗口种类路径。（MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:124–143、:226–236；MobileGL/MG_Remote/Server/ServerLoop.cpp:1226–1264；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:16429–16500） |
| (b) MOBILEGL_IPC_SURFACE=server | Remote 不发 SetWindowHandle；WindowKind=ServerOwned，nativeToken=0。服务器从 ServerDisplay 取得窗口租约，替换为本进程 Android WindowHandle，调用同一 backend window 创建；reply 返回实际尺寸。Java SurfaceView 是目标，**不是 Anland wl_surface**。（MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:216–218；MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:160–169；MobileGL/MG_Remote/Server/ServerLoop.cpp:1600–1652；MobileGL/MG_Remote/Server/DisplayServerJni.cpp:11–24） |
| (c) Pbuffer / headless | CreatePbufferSurface → SurfaceOp → server backend pbuffer；会话闩为 Offscreen。Espryt 建原生 EGL pbuffer；Magma 创建带尺寸的 windowless renderer，在 Linux 用 VK_EXT_headless_surface，在 Android 无此扩展时使用 AImageReader 的 ANativeWindow 作为不可见 producer target。该 Android fallback 明确“never acquires images”，没有把图像回给 client。（MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:271–288；MobileGL/MG_Remote/Server/ServerLoop.cpp:1361–1381；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16667–16680；MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:378–387；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:16344–16410） |

即使 pbuffer 调用 eglSwapBuffers，也走同一 Present 命令，不是 readback-pixels 的自动交付。真正 EGL_NO_SURFACE 的应用 bind 受第 2 节限制；文档里的“surfaceless”通常描述 headless native display/pbuffer 路线。（MobileGL/MG_Backend/BackendObject.cpp:314–338、:369–397；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:498–520）

### 3.3 后端如何拿到 presentation target

**Espryt / DirectGLES：**窗口来自 backend 的 WindowHandle（ServerOwned 时为租约中的 ANativeWindow），用 vendor EGL 的 eglGetDisplay / eglInitialize / eglBindAPI(ES)，创建一个原生 ES context 和 eglCreateWindowSurface；Present 在 native GL 命令流插 GLsync，然后原生 eglSwapBuffers(g_Display,g_Surface)，轮询 GLsync 推进资源回收水位。这里不是 EGL native-fence fd。（MobileGL/MG_Remote/Server/ServerLoop.cpp:1623–1635；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16587–16612、:16639–16664、:17252–17290）

**Magma / DirectVulkan：**InitWindowSurface 将 m_windowHandle.Handle 放入全局 VulkanRenderer；Android CreateSurface 以此 ANativeWindow 调 vkCreateAndroidSurfaceKHR，再 vkCreateSwapchainKHR。Present 提交 VkCommandBuffer，调用 vkQueuePresentKHR，并继续 acquire/重建逻辑；VkFence/VkSemaphore 是该 renderer 内部同步。（MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:359–374；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:16429–16435、:14528–14569、:14634–14645；MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp:284）

### 3.4 AHB、dma-buf 与 fence fd：现有命中的真正用途

**未找到产品帧导出路径。** 有 AHB import、有 external-memory 扩展、有 fd passing；用途与“default framebuffer 导出给 client”不同。完整逐命中位置/用途索引见附录 A（主仓库 tracked tree 586 个匹配行、134 个文件；包含文档、历史日志、公共 EGL header，不把 submodule 内上游声明算作 MobileGL 实现）。

| 命中类别 | 用途与是否可直接用于 Anland present |
|---|---|
| P11 AdoptT0 | client 分配 **AHARDWAREBUFFER_FORMAT_BLOB**，height=1，usage=CPU read/write + GPU_DATA_BUFFER，持 CPU lock；通过 hop socket 传 handle，server 收到后校验 BLOB 宽度=size。是 persistent GL buffer 的 backing store，**不是颜色图像**。（MobileGL/MG_Remote/Transport/AdoptT0.cpp:35–75、:78–96、:108–125） |
| GLES eglGetNativeClientBufferANDROID | 用在 glBufferStorageExternalEXT(nativeClientBuffer)，然后 MapBufferRange persistent/coherent。没有把该 AHB 做 EGLImage render target。（MobileGL/MG_Backend/DirectGLES/Managers.cpp:2799–2817、:2835–2857、:2875–2897） |
| Vulkan VK_ANDROID_external_memory_android_hardware_buffer | VkBufferManager 将 client 的 AHB BLOB 导入 **VkBuffer**、dedicated VkDeviceMemory，并处理持久映射/生命周期；VulkanRenderer 按条件启用扩展供它用。不是导出 VkImage / swapchain image。（MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp:631–742；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:15348–15362；MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.h:75–80） |
| POST / T0 self-test | server bind 时按允许开关、平台 AHB、后端 import op 和 POST 决定 kCapAdoptT0；失败退 T2。产品 T1 已关闭；显式选择 T1 也退 T2。（MobileGL/MG_Remote/Server/ServerSession.cpp:1092–1143；MobileGL/MG_Remote/CONTRACT-P11.md:233–248） |
| extmem_probe 的 external_memory_fd / DMA_BUF / SCM_RIGHTS | 独立探针测试 T1 host-visible buffer fd 导出、T0 AHB BLOB 导入、T3 host memory。它确实有 vkGetMemoryFdKHR 与跨进程读写，但不是 renderer 的逐帧图像输出。（tools/spikes/extmem_probe/README.md:23–60；tools/spikes/extmem_probe/extmem_probe.cpp:8–20、:1718–1773、:2182–2188、:2356–2378、:3590–3664） |
| AImageReader / AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | integration、CTS、piglit harness 创建 Android 测试窗口；Magma headless fallback 也使用 AImageReader window，但未 acquire 导出图像。（MobileGL/MG_IntegrationTest/Harness/HeadlessGL.cpp:102；tools/cts/platform/tcuMobileGLPlatform.cpp:129–130；tools/cts/probe/mgprobe.c:158–159；tools/piglit-android/patches/waffle-mobilegl-android.patch:277–278；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:16355–16390） |
| SCM_RIGHTS 产品实现 | 共享段和门铃交付、supervisor 数据连接 handoff，以及 T0 的 hop；详见第 4 节。没有定义 FrameReady(bufferFd,fenceFd) 消息。（MobileGL/MG_Remote/Transport/FdPassing.cpp:108–149；MobileGL/MG_Remote/Client/ClientSession.cpp:1186–1251；MobileGL/MG_Remote/CONTRACT-P11.md:257） |
| include/EGL/eglext.h | Khronos 公共声明：AHB native client buffer、dma-buf import/modifiers/export。只有声明，不是 MobileGL 功能。（include/EGL/eglext.h:517–520、:784–829、:1103–1111） |
| 历史日志 | vendor Vulkan extension 枚举及运行诊断；证明该次 driver 暴露了名字，不证明 MobileGL 调用该扩展输出帧。逐行出处见附录 A。 |

对主仓库 tracked tree 的指定精确子串检索中，**sync_file 和 SYNC_FD 无命中**；另外检索未找到产品 vkGetSemaphoreFdKHR / eglDupNativeFenceFDANDROID 调用。EGL_ANDROID_native_fence_sync 未广告，前端 EGL sync 只是状态对象；GLsync / 完成水位也不是可交给 zwp_linux_explicit_synchronization_v1 的 sync_file fd。（检索范围见附录 A；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:465–473；MobileGL/MG_State/EGLState/Core.cpp:1336–1394；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:17252–17283）

**inferred：**Anland 需要新增 image 路线，不能把 P11 BLOB 改个名字就用。最直接候选是 server 分配/导入可渲染 AHB image（GLES：EGLImage+FBO；Vulkan：external-memory VkImage），输出单平面图像描述及真实同步；也可改造 AImageReader 为实际 acquire/release 的 buffer 消费链。必须验证四字符格式、stride/offset、LINEAR/INVALID modifier 以及 AHB/gralloc native handle 与普通 dma-buf 的可转换性，不能把 opaque fd 或 AHB 多 fd handle 默认当成 Anland 可导入的线性 ARGB/XRGB。依据：当前 BLOB/import 代码与题目给定 Anland buffer 限制；历史探针甚至在 Adreno/Mali 两台上都没得到 T1-dma-buf 导出（docs/Disaggregated/notes/p0/p0-device-findings.md:18–27），但该 buffer 探针也不能证明所有 image 路线必然失败。

## 4. Fd passing、transport 与 SELinux

### 4.1 今天实际可用的组合

| 拓扑/端点 | 实际数据面 | fd 能力 |
|---|---|---|
| inproc | 同进程共享段/双角色 | InProcessTransport 复制描述符给 peer，模拟所有权；不是跨 Android 安全域方案。（MobileGL/MG_Remote/Transport/InProcessTransport.cpp:200；docs/Disaggregated/guide/build-and-run.md:20–22） |
| spawn + fork | ShmLink | launch server 后连接本机 AF_UNIX；四段+三个门铃经 aux fd passing。配置只收 auto/shm。（MobileGL/MG_Remote/Client/ClientSession.cpp:954–1005、:1186–1251） |
| spawn + unix:path / @abstract | ShmLink | control 与 aux 配对；ShareFd/ReceiveFd 在 aux 上 SCM_RIGHTS。（MobileGL/MG_Remote/Client/ClientSession.cpp:928–950；MobileGL/MG_Remote/Transport/SocketTransport.cpp:849–868） |
| spawn + fd:control,aux | ShmLink | P11 B1 已连接的两根 AF_UNIX SOCK_STREAM socket；可由 Android helper/broker 交接。只收 auto/shm。（MobileGL/MG_Remote/CONTRACT-P11.md:110–125；MobileGL/MG_Remote/Client/ClientSession.cpp:877–924） |
| spawn + tcp://host:port | StreamLink | **不传 fd**。SocketTransport::ShareFd/ReceiveFd 对 m_tcp 返回 UNSUPPORTED；client 只接 auto/stream。（MobileGL/MG_Remote/Transport/SocketTransport.cpp:849–868；MobileGL/MG_Remote/Client/ClientSession.cpp:928–950、:1025–1026） |
| TCP control + shm data | 设计过，**当前未接线** | design/06 描述的 ShmLink 独立 AF_UNIX rendezvous 是第二波设计；同文件最后明确第一波仅 TCP+stream。当前上层按 IsTcp 选数据面并拒绝混搭，不能按愿景回答已经支持。（docs/Disaggregated/design/06-transport.md:54–67；MobileGL/MG_Remote/Client/ClientSession.cpp:928–936、:1025–1026） |

“控制面不传 fd”作为协议层概念成立：SCM_RIGHTS 不塞进 FlatBuffers 控制字节流。但当前实现的 **ITransport 接口仍保留 ShareFd / ReceiveFd**，由 SocketTransport 的 aux socket 实现；并非代码已经完全迁到 ShmLink 自有交付 API。新实现应尊重实际的 ITransport/aux 接口，而不是仅照旧契约措辞接线。（MobileGL/MG_Remote/Transport/ITransport.h:87–108；MobileGL/MG_Remote/Transport/SocketTransport.cpp:849–868；对照 MobileGL/MG_Remote/CONTRACT-P6.md:636–648）

### 4.2 “每帧 server → client buffer fd + fence fd”有多少现成基础

基础发送已是**双向**接口；server 现在就把共享段 fd 发给 client。SendFd 的 ancillary buffer 只放一个 int；ReceiveFd 虽收集至多四个以便关闭多余 fd，随后明确要求 receivedCount==1。sideband 最大 256 字节。不能原样把“两 fd”塞进一次现有消息。（MobileGL/MG_Remote/Transport/ITransport.h:89–108；MobileGL/MG_Remote/Transport/FdPassing.h:36–38、:48–73；MobileGL/MG_Remote/Transport/FdPassing.cpp:131–149、:253–267、:289–301；MobileGL/MG_Remote/Client/ClientSession.cpp:1194–1217）

**inferred：fd 搬运本身是局部扩展，完整呈现协议是显著工作。** 可选择两个带相同 frame/buffer serial、不同 fd-kind 的单 fd offer，或定义版本化 multi-fd packet；更有效的是 buffer pool 首次注册发 buffer fd，之后每次 present 仅发 buffer id + acquire fence。需要同时实现：

1. session/surface/buffer/generation/frame 关联、format/plane metadata、协商和协议修订；
2. client 持续接收 server aux offer 的 dispatcher，不能只在握手读取 7 个 fd；与已有 T0 aux 用途协调；
3. fence=-1 / 无 fence 情况、重复/陈旧/中断消息及 fd 关闭规则；
4. buffer release / release fence 的反向通道，在 compositor 未释放前禁止复用；
5. 有界发送和背压，避免 apply 线程在 client 不读 fd 时永久阻塞；
6. StreamLink 没有本地 fd 传递的 fallback：如同机额外 AF_UNIX，或显式像素传输模式。

这些是设计建议，不是现有功能。依据：一次一个 fd、有限 sideband、仅 serial 的 Present、T0 offer 协议及当前单独的阶段性接收逻辑。（MobileGL/MG_Remote/Transport/FdPassing.h:36–73；MobileGL/MG_Remote/Client/ClientSession.cpp:1186–1251；MobileGL/MG_Remote/CONTRACT-P11.md:257；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809；MobileGL/MG_Remote/Transport/SocketTransport.cpp:849–868）

### 4.3 谁创建共享段：文档 owner 不等于 allocation owner

**当前跨进程产品代码由 server 创建全部四段**：

- ServerSession::Accept 的非 stream 分支调用 Memory().Create(sizes, MemoryRole::Server)。
- SessionSegments::Create 枚举 mgl-cmd、mgl-stage、mgl-reply、mgl-event，并逐个 ShmSegment::Create / Map。
- client 收 7 个 fd（4 段+3 门铃），再 AdoptFromDescriptors(..., MemoryRole::Client)。

（MobileGL/MG_Remote/Server/ServerSession.cpp:713–737；MobileGL/MG_Remote/Transport/ShmSegment.cpp:236–284；MobileGL/MG_Remote/Client/ClientSession.cpp:1186–1251）

设计表把 SEG_CMD/SEG_STAGE “拥有者”写 client，适合解释写者/语义归属，**不能用于证明 fd 是 root container client 创建的**。Linux 的 ShmSegment::Create 用 raw memfd_create syscall + ftruncate，失败退 shm_open+立即 unlink；Android 用 ASharedMemory_create。若 server 是 Android app，其握手段走 Android 创建分支，而非 Linux client 的 memfd 分支。（docs/Disaggregated/design/06-transport.md:9–17；MobileGL/MG_Remote/Transport/ShmSegmentPosix.cpp:51–109；上段调用点）

StreamLink 用私有段传字节，本地 memfd/存储并不以 fd 共享给对端；这与 ShmLink 的跨进程共享不同。（docs/Disaggregated/design/06-transport.md:17、:59；MobileGL/MG_Remote/Server/ServerSession.cpp:713–737）

**inferred：**因此题目担心的“Android untrusted_app 使用 root-client 创建的 CMD/STAGE fd”不是当前握手路径的准确模型；真正需要验证的是 root/container 域接收和映射 app-origin fds、AF_UNIX connectto/namespace 可达性，以及今后客户端返回 release fence 的 fd use。P11 T0 确实是相反方向的 client-origin AHB，但 glibc client 没有该 Android AHB allocator，不能自动享有 T0；可先明确选择 T2。（MobileGL/MG_Remote/Transport/AdoptT0.cpp:35–54；MobileGL/MG_Remote/Transport/AdoptT0.h:56–61；MobileGL/MG_Remote/CONTRACT-P11.md:233–248）

历史 SELinux 测量：shell 和另一个 untrusted_app 连 server 的 abstract unix socket 被 connectto 拒；root relay 打通连接后，另一 app mmap server 创建的段可用。B1 因而实现令牌 broker + app_process helper + Binder PFD 交接，helper exec 外部程序并设置 fd: 端点。**该证据不是 Droidspaces root glibc/chroot 域的测试结论**，不能承诺所有 ROM 或 root 策略都允许。（docs/Disaggregated/notes/p11/B0-CROSS-APP.md:3–15；MobileGL/MG_Remote/CONTRACT-P11.md:110–125、:195–197）

还有部署限制：目前 broker 只认离屏 MobileGLServerService，显示 Activity :mglwin 在跑时也可答 server-not-running；它不是 P12 onscreen server 的通用 broker。（MobileGL/MG_Remote/CONTRACT-P11.md:147）

## 5. Session / 多 client / 多窗口限制

### 5.1 哪些是宿主管理策略，哪些深入进程状态

| 限制 | 所在层与深度 |
|---|---|
| “同设备一个 server” | P12 APK 产品策略：DisplayActivity 启动会 stopService；Service 启动会结束显示 server。不是 kernel / wire 全设备唯一锁。（MobileGL/MG_Remote/CONTRACT-P12.md:46；android-plugin/app/src/main/java/top/mobilegl/plugin/MobileGLDisplayActivity.java:119；android-plugin/app/src/main/java/top/mobilegl/plugin/MobileGLServerService.java:85–103、:139） |
| 这个口号已有例外 | B1 在主监听为 TCP 时另起一个私有 UNIX broker supervisor。两者是独立 process/supervisor；所以不能把“一个 device 最多一个 session”当已严格执行的全局不变量。**inferred：**不同 supervisor 各自的 Busy 不互相协调。（android-plugin/app/src/main/java/top/mobilegl/plugin/MobileGLServerService.java:147–162；MobileGL/MG_Remote/CONTRACT-P11.md:113；MobileGL/MG_Remote/Server/ServerMain.cpp:1222、:1363、:1548） |
| 每 supervisor 一个 active session | TCP authenticated second Hello → Refuse Busy；UNIX/thread/fork 路线也做 Busy。单个慢启动但已配对 client 可占槽；很晚才发 Hello 的第二个 client 可能只看见发送失败。（MobileGL/MG_Remote/Server/ServerMain.cpp:915、:1222–1260、:1363、:1548–1565；MobileGL/MG_Remote/CONTRACT-P11.md:130） |
| Offscreen fork-per-session | Service exec libMobileGLServer.so endpoint --serve；每 session 子进程 fork，并设 PR_SET_PDEATHSIG 随 supervisor 退出。它本来具备进程隔离结构，但调度仍串行。（android-plugin/app/src/main/java/top/mobilegl/plugin/MobileGLServerService.java:180–188；MobileGL/MG_Remote/Server/ServerMain.cpp:317、:1240、:1553；MobileGL/MG_Remote/CONTRACT-P12.md:45） |
| Onscreen 串行进程内 session | Activity 的 :mglwin 持 SurfaceView，server 用 Handoff::Thread；backend 按进程钉住，首次 session 可决定类型，之后不能换另一 backend。会话间重置 latch / 清 backend 状态。（MobileGL/MG_Remote/CONTRACT-P12.md:44；MobileGL/MG_Remote/Server/ServerMain.cpp:520–542、:1623；docs/Disaggregated/notes/p12/INTEGRATOR-DECISIONS-P12.md:10–14） |
| 一会话一个 surface mode | 首次成功 pbuffer → Offscreen；ServerOwned → OnScreen。另一类返回 SurfaceModeMismatch，拒绝不等于杀 session。X11/Win32/None 残留路径不闩模式，是未收紧的例外。（MobileGL/MG_Remote/CONTRACT-P12.md:17、:23；MobileGL/MG_Remote/Server/ServerLoop.cpp:1361–1381、:1539–1543、:1606–1614） |
| ServerOwned 只有一个实际窗口 | 多个 surface token 可以存在，但都指 server 的同一窗口；一个 extent 应用到全部。不是 M 个独立窗口。（docs/Disaggregated/notes/p12/INTEGRATOR-DECISIONS-P12.md:16；MobileGL/MG_Remote/Server/ServerLoop.cpp:1627–1648；MobileGL/MG_Remote/Client/ClientSession.cpp:449–463） |
| Session 单例 | ServerSession::Active 是一个进程全局 atomic 指针，ServerSessionInstance / ClientSessionInstance 为单例；segment resolver 也明确全进程唯一，反向 callback 表为全局。（MobileGL/MG_Remote/Server/ServerSession.cpp:527–543、:832–857；MobileGL/MG_Remote/Client/ClientSession.cpp:616–619；MobileGL/MG_Pipe/MGPipeCallbacks.h:76–77） |
| Backend / frontend 单例 | pGLContext、pActiveBackendObject、global function table；Espryt g_Display/g_Context/g_Surface；Magma pVulkanRenderer。切换 surface 会替换 renderer/运行时状态，不是独立 per-window/context renderer 集合。（MobileGL/MG_State/GLState/Core.h:701；MobileGL/GlobalObjects.cpp:23–24；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16183–16185；MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:38–39；MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:359–387） |
| 热路径缺乏多窗口身份 | Present 只有 FrameSerial；MGPSurfaceInfo 只有尺寸/格式/samples/layers/default 标记，没有 surface id。当前依赖 session 当前绑定目标，无法直接表达并行多窗口完成/释放。（MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809、:1999–2005） |

### 5.2 要服务 N clients × M windows，需要什么

以下均为 **inferred**，基于上表结构：

- **N 个 client 最小方案：每 client 一个 worker process。** 扩展 offscreen supervisor 为多 child/session registry、把控制/数据配对、认证、额度、日志、退出清理路由到对应 worker；这样可以暂时保留大量 backend process-global 状态。仅删一个 Busy 判断不够，当前 child/session handoff 和 Active 指针模型必须保持隔离。（基础：MobileGL/MG_Remote/Server/ServerMain.cpp:915、:1222–1260；MobileGL/MG_Remote/Server/ServerSession.cpp:527–543）
- **同一进程并发 N sessions** 则要 session 化 active backend、resolver、callback 表、latch、caps、backend resource/cache 全局，或完整线程/对象归属隔离；目前 P12 的结束清理只服务串行重用。（基础：MobileGL/MG_Remote/Server/ServerSession.cpp:832–857；MobileGL/MG_Pipe/MGPipeCallbacks.h:76–77；docs/Disaggregated/notes/p12/INTEGRATOR-DECISIONS-P12.md:12–14）
- **单 client 的 M windows** 要有每 surface 独立 framebuffer / image pool、尺寸世代、swap interval / credits、present/release identity；修改 active-surface backend 生命周期，避免换窗即丢弃该进程唯一 renderer。共享 GL 资源不能粗暴一窗口一进程来保证，还要真实 context/share-group。（基础：MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:359–387；MobileGL/MG_Backend/BackendObject.cpp:314–338；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809、:1999–2005）
- **Xwayland glamor** 应单独当系统级 EGL/GLES/图像共享消费者验证；应用 GLX facade 可用并不能保证 glamor 所需 EGLImage、dma-buf import/export、同步和 renderer 资源互通。可以先让 Xwayland 保持其他渲染/软件路径，仅给选定 GL 应用使用 MobileGL；这属于分阶段部署选择，不是现成配置承诺。（基础：第 2 节 image/sync/config 和 GLX 证据；Anland/Xwayland 场景取自题目。）

## 6. 窗口系统事件、几何、失效与 pacing

### 6.1 现有协议能表达什么

SurfaceOpKind 完整列表为 InitializeDisplay、CreateWindowSurface、CreatePbufferSurface、ResizeWindowSurface、ReleaseSurface、MakeCurrent、ReleaseCurrent、SetSwapInterval、ReleaseResources、SetWindowHandle、InitCapabilities（另有 None）。它有 seq/display/surface/windowKind/nativeToken/width/height/swapInterval/readSurface/context；没有 Wayland object、DRM format/modifier、damage region 或 buffer release。（MobileGL/MG_Remote/Protocol/protocol.fbs:240–305）

WindowKind 为 None=0、AndroidNativeWindow=1、X11=2、Win32Hwnd=3、Surfaceless=4、Pbuffer=5、MetalLayer=6、ServerOwned=7；**没有 Wayland/GBM**。ServerOwned 只合法于 CreateWindowSurface，token 必须 0。（MobileGL/MG_Remote/Protocol/protocol.fbs:261–276；MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:199–214）

SurfaceReply 含 seq/ok/EGL version/eventHead/width/height/refusal；defaultFb schema 字段还在，但 encoder 固定传 0，实际 framebuffer 几何通过 kEventSurfaceChanged。eventHead 保证更早的数据面事件先于 reply 被观察。（MobileGL/MG_Remote/Protocol/protocol.fbs:308–323；MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:242–260）

ServerOwned 的 resize 为 Java SurfaceHolder.setFixedSize / setSizeFromLayout 请求，窗口实际尺寸经 reply/event 返回；Activity 做等比 letterbox。kEventSurfaceChanged 带 Width/Height/InternalFormat/Samples/Layers/IsDefault，client 更新默认 framebuffer 和所有 ServerOwned surface extent。没有 frame callback、presentation timestamp 或 input forwarding。（MobileGL/MG_Remote/CONTRACT-P12.md:31–35、:64；MobileGL/MG_Remote/Server/DisplayServerJni.cpp:27–34；MobileGL/MG_Remote/Client/ClientSession.cpp:449–463；MobileGL/MG_Pipe/MGPipeTypes.h:1999–2005）

SurfaceProgress 是冷启动 SurfaceOp 心跳，不是“该帧可显示”的事件。P10 的 kEventFenceSignaled 是逻辑 GL fence 完成报告，不是 sync_file fd。（MobileGL/MG_Remote/Protocol/protocol.fbs:390–398；docs/Disaggregated/ROADMAP.md:38；MobileGL/MG_Remote/Server/PipeApplier.cpp:433–438）

### 6.2 device-lost：文档承诺与代码差异

现有 peer hangup 设置 ClientSession::m_deviceLost，一次日志后后续 wire verbs DECLINED；不自动 respawn。窗口被撤销时，先在 apply 线程释放 backend surface，再 Fatal ServerWindowLost，最后归还 lease；Detach 有界 3 秒。纯等待超时不应被当成 peer 死亡，但仍有自己的 Fatal timeout 失败家族。（MobileGL/MG_Remote/Client/ClientSession.cpp:595–613；MobileGL/MG_Remote/CONTRACT-P12.md:27–40；MobileGL/MG_Remote/Client/ClientSession.cpp:2432–2438）

**不能照抄“eglSwapBuffers 返回 EGL_CONTEXT_LOST”为当前已实现行为。** design/08:33 确实这样写，但实际 EGLImpl swap 只有 validation/backend false 时 SetError(EGL_BAD_SURFACE)，成功则 TRUE；base 调 void Present() 后返回 true，Remote 直接复用 base。检索产品代码中 EGL_CONTEXT_LOST 仅见 enum-to-string，未见 lost→EGL error 映射。因此：GL reset/device-lost 机制存在，EGL swap 的标准错误映射有缺口；**inferred：**死亡后某些调用可仍沿现有 Bool 路径成功，具体时序未运行验证。（docs/Disaggregated/design/08-runtime-and-platform.md:33；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:207–228；MobileGL/MG_Backend/BackendObject.cpp:390–397；MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:396–404；MobileGL/MG_Util/Converters/EGLToStr/EGLEnumConverter.cpp:77）

### 6.3 Present credit 与 compositor 怎样接

MOBILEGL_IPC_PRESENT_CREDIT 默认 1、范围 1..8；client 在编码 present **之前**等待 serial-credit 的 presentAckSerial。server 在 backend Present() 返回之后还 credit。它限制命令执行/本地 swap 的 run-ahead，独立于 CMD/STAGE 字节 credit。（docs/Disaggregated/design/08-runtime-and-platform.md:7–8；MobileGL/MG_Remote/Client/ClientSession.cpp:2385–2438；MobileGL/MG_Remote/Client/EmitTables.cpp:1165–1181；MobileGL/MG_Remote/Server/PipeApplier.cpp:426–438）

文档 design/08 仍称 credit>1 未测，但后来的 P12 跨机验收已测 1/2/3；报告应以后者为准，不能把它写成仍完全未知。（docs/Disaggregated/design/08-runtime-and-platform.md:8；docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:16、:23–34）

**inferred：接 Anland 至少要分开三种事件。**

| 事件 | 建议接线 / 不可混用的语义 |
|---|---|
| GPU frame ready | server 输出 image identity + acquire fence；client 可将 fd 交给 Wayland explicit synchronization。不能用现有 presentAckSerial 冒充 GPU fence。（基础：MobileGL/MG_Remote/Server/PipeApplier.cpp:426–438；题目给定 explicit sync） |
| compositor frame callback | 用于允许/建议下一帧节奏；可控制 frontend swap pacing、有限 credit 或单独 queue-depth。不能证明旧 buffer 已可重写。（基础：当前 credit 返回条件同上；新 compositor 语义为集成设计推断） |
| wl_buffer.release / explicit release fence | 发回 server 的 BufferReleased(surface,buffer,generation[,fence])，决定 image pool 的复用。多窗口要每窗/每池计数，不能只用全会话一个 frame serial。（基础：MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809、:1999–2005；题目给定 Anland buffer 路线） |

还需让隐藏/最小化/长期无 frame callback 的窗口不会触发现有“credit 长期不还→PresentCreditTimeout”而丢会话；必须定义暂停、丢帧、buffer pool 耗尽和断连回收规则。这是 **inferred** 的协议设计需求，不是现有行为。（MobileGL/MG_Remote/Client/ClientSession.cpp:2432–2438；MobileGL/MG_Remote/CONTRACT-P12.md:64）

## 7. 通用 Linux 桌面 GL 覆盖：一张短表

| 能力 | Espryt / DirectGLES | Magma / DirectVulkan | 对 Linux 应用的含义 |
|---|---|---|---|
| 最大报告/目标 GL、GLSL | 4.6.0 / 4.6.0，静态 core identity。（MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:817–822） | 同为 4.6.0，静态 core identity。（MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp:546–551） | 是广告/目标，不是完整一致性认证；README 仍写 4.2 Core 目标。（README.md:49–56） |
| Compatibility / fixed function | 两者共用前端：compat profile 属性可保存并用于宽松语义，但不是完整 legacy pipeline。导出/lookup 未找到核心 glBegin/glEnd、glMatrixMode、glLightfv、glMaterialfv、glNewList、glDrawPixels 的真实链；一些 EXT matrix 名只是扩展符号表存在。 | 同左 | 无法承诺旧 GL1.x/2.x fixed-function 桌面程序。依据：MobileGL/MG_State/GLState/Core.h:703–708；MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp:2426；MobileGL/MG_Impl/GetProcAddress.cpp:896–910、:1406–1407；MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:26–35；“无法承诺”为 **inferred**。 |
| GLES contexts | frontend BindAPI(ES) 接受；backend 内部 ES3.2→3.0 negotiation。 | frontend 同样接受 ES；backend 是 Vulkan。 | 没有完整 GLES profile/version 保证；GL_VERSION 不切 ES，GetShaderPrecisionFormat stub。（MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:414–428；MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:16595–16612；MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp:619–626；MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:152） |
| GLX_ARB_create_context | 共用 Linux GLX facade；广告并将版本/profile 转到 EGL。（MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:84–86、:818–883） | 同左 | 入口可用≠完整 share group / DRI3 / Xwayland present。（第 2、5 节证据；判断为 **inferred**。） |
| 已知非游戏专属缺口 | DebugMessageControl/Callback/GetDebugMessageLog、若干 robustness/GetnUniform 等导出仍 stub；EGL image/sync 属于状态壳。 | 同样共用前端 | 会影响调试、robustness、图像互通和 compositor 消费者。（MobileGL/MG_Impl/GLImpl/Exporting/Definitions.cpp:397–429；MobileGL/MG_State/EGLState/Core.cpp:1336–1466；影响判断为 **inferred**。） |
| docs 记录的未完项 | fp64 server 广告/client 编译配置不一致；driver-written texture 拒读仍可变 session Fatal；多 context 未做。 | 有 malformed indirect fixture 丢设备、设备对齐约束及 monolith 特有债。 | 应以真实目标应用/GL CTS 子集验收，不能用 Minecraft 或 trace 成功覆盖全部 desktop workloads。（docs/Disaggregated/notes/DEBTS.md:14、:22、:25–28、:42；验收建议为 **inferred**。） |

这里的固定功能否定基于实际入口/前端检索，不把“IsCompatibilityProfile=false”单独当证明；相反，EGL/GLX 的 compatibility 请求和 getter 确实存在，必须与完整功能实现区分。（MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:794–812；MobileGL/MG_State/EGLState/Core.cpp:657–659；MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.cpp:2426）

## 8. 文档是否已预见 Linux-on-Android / Wayland / AHB / 多 client / Termux

以下为原文节录。保留“历史设计”与“当前实现”的区别，不把旧推测当新验收。

| 主题 | 原文与出处 | 如何解读 |
|---|---|---|
| 容器 Linux / Termux | “**B 容器类 guest（DroidSpace / Winlator / Termux，共享内核）** … AF_UNIX … 同上（含 AHardwareBuffer_sendHandleToUnixSocket，API 26） … **整套 P6 机制逐字可用**。”（docs/Disaggregated/notes/p6/P6-ENDSTATE-REVIEW.md:138） | 同内核 transport 的历史论证；并非 Anland WSI 或 glibc AHB API 验收。 |
| 后来场景变更 | “**形态 B 不再是操作性场景；终局是 D，P6.5 在关键路径上。**”（docs/Disaggregated/notes/p6/P6-ENDSTATE-REVIEW.md:142） | 不能据历史容器表声称已为 Anland 交付。 |
| Linux client 范围 | “**默认先 Linux + Android（现有 CI 能验），Windows client 第二批 … macOS 仍不拆分。**”（docs/Disaggregated/notes/OPEN-QUESTIONS.md:28） | Linux client 是明确目标，但无 aarch64 glibc 专项验收。 |
| Wayland | “**Linux / WSL / CI 永不开窗（EGL_PLATFORM=surfaceless）；Wayland 不支持。**”（docs/Disaggregated/design/08-runtime-and-platform.md:32） | 当前声明不支持；本次也没有查到 Wayland frontend/present 实现。 |
| 历史 X11 设想 | “**Window 是 XID，nativeToken:u64 直接送。backend 自己 XOpenDisplay(getenv(\"DISPLAY\")) 并构造 VkXlibSurfaceCreateInfoKHR … Wayland 今天不支持 … 维持。**”（docs/Disaggregated/notes/recovered/wf1/final/part3.md:80） | 是同 X server 的 XID 设想，不能移植成 Android ANativeWindow token。 |
| T0 与 T1 | “**T0 = client 分配 AHardwareBuffer、server 导入；T1 = server 导出 opaque fd；T2 = 拒绝，client 保留 shadow 并推送。**”（MobileGL/MG_Remote/CONTRACT-P11.md:3） | 全部处于 persistent-map store 议题；不是 frames。T1 当前关闭见 :239。 |
| AHB 帧输出方案曾被讨论 | “**Child renders into AHardwareBuffers, sends them over a unix socket, app presents.**” 并明确需 “**a new buffer-ring/fence protocol, an Espryt default-framebuffer-to-AHB FBO, Magma AHB import**”。（docs/Disaggregated/notes/p12/map-apk-process.md:87–90；另见 docs/Disaggregated/notes/p12/map-docs-contract.md:103、docs/Disaggregated/notes/p12/map-server-surface.md:113） | 这是 P12 调研中的备选方案，确实预见了帧 AHB 交付；并未落地为当前 present 路径。 |
| P12 为何没有交付该方案 | “**in-process server … renders straight into its SurfaceView; sessions run sequentially in that process. (Not fork + AHB.)**”（docs/Disaggregated/notes/p12/BRIEF-onscreen.md:14–15） | 最终范围明确选服务器进程内 SurfaceView，放弃这一阶段的 fork+AHB 帧桥。 |
| dma-buf 历史设想 | “**T0 — server 导入 client 分配：client 分配 AHardwareBuffer/dma-buf … 理想但可用性未知。**”（docs/Disaggregated/notes/recovered/wf1/final/part2.md:172） | 旧设计；落地是 AHB BLOB T0。 |
| Export 的实际局限 | “**T1-dma-buf … UNSUPPORTED**”两台设备；“**Tier decision for P11 (persistent maps across the process boundary)** … T0 … BLOB。”（docs/Disaggregated/notes/p0/p0-device-findings.md:21、:27） | 不要将 persistent VkBuffer opaque-fd 成功解释为线性 frame dma-buf 成功。 |
| Termux 消费者 | “**消费者：同一台手机上从另一个 app（Termux、adb shell）启动、没有 Java Context 的原生 GL 程序。**”（MobileGL/MG_Remote/CONTRACT-P11.md:104） | 已落地 helper/broker/fd: 路线的真实目标；历史验收用 retrace/shell/第二 APK，非真实 Termux glibc 图形桌面。 |
| 跨 app 限制 | “**SELinux 挡的是连接这一步。** … 一旦连上 … 现有代码就走 SharedSegments … **缺的是跨上下文的连接中介。**”（docs/Disaggregated/notes/p11/B0-CROSS-APP.md:7） | 之后 B1 已实现中介；root container 仍是不同待验证安全域。 |
| 多 session / context | “**DirectGLES g_Display / g_Surface / g_Context 仍是全局 … 多 context；server 窗口不转发输入**”，状态为“**等出现第二个会话 / 第二个 context 的需求**”。（docs/Disaggregated/notes/DEBTS.md:42） | 明确尚未进入通用桌面多上下文阶段。 |
| 唯一会话 / Busy | “**同时只服务一个会话，第二个连接 Refuse{Busy}**。”（docs/Disaggregated/design/08-runtime-and-platform.md:17） | 精确实施范围是每个 supervisor；B1 双 supervisor 例外见第 5 节。 |
| 跨机 client | “**client 是 trace 重放器，不是活的 Minecraft 实例**。”（docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:11） | 更不能扩大成已经验证任意 Linux desktop app。 |

本次对 docs、notes、contracts 检索未找到 “Anland” 的专项实施方案；也未找到“导出 framebuffer AHB/dma-buf 给 Wayland client”的已落地设计。P12 调研**确实提出过 AHB frame bridge**，列明 buffer-ring/fence、GLES FBO、Magma AHB import 等新增工作；最终 brief 明确选择进程内 SurfaceView，未选择 fork+AHB。P11 则另行落地 persistent-buffer T0。这三件事应分开阅读。（docs/Disaggregated/notes/p12/map-apk-process.md:87–94；docs/Disaggregated/notes/p12/BRIEF-onscreen.md:14–15；MobileGL/MG_Remote/CONTRACT-P11.md:227–257；MobileGL/MG_Remote/CONTRACT-P12.md:7、:16、:44）

### 8.1 对接 Anland 的可实施分解（全部 inferred）

这些是基于本次源码盘点的实现建议，尚未构建或原型验证：

| 工作包 | 应做的具体事 | 可复用基础 / 缺口依据 |
|---|---|---|
| A. glibc arm64 client 与加载器 | 增加 aarch64 glibc toolchain/CI 和与 Android arm64 server 的 wireFingerprint 验收；决定 libGL/libEGL 直接替代部署或 GLVND vendor；先锁定目标 API/profile，修正虚假成功/广告。 | CMakeLists.txt:503–545、:783–835；.github/workflows/test.yml:1269–1293；MobileGL/MG_State/EGLState/Core.cpp:615–689、:1336–1466 |
| B. Linux WSI | EGL Wayland native display/window + resize/current/swap；client 在 glibc 侧持 wl_surface / wl_buffer。X11 路线补 local drawable 的 buffer present；另列 Xwayland glamor 支持门。 | MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:64–75；MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp:258–327、:956–965 |
| C. server image provider | 新增导出型 offscreen surface，而非重用 P12 fullscreen DisplayActivity 作为目标；GLES AHB-image/EGLImage/FBO 或 Vulkan external VkImage；初期只协商题设 Anland 接受的单平面格式/modifier。若图像导出不可行，可先显式 readback 到 wl_shm 建立正确性基线，不能称其零拷贝。 | MobileGL/MG_Remote/Server/ServerLoop.cpp:1600–1652；MobileGL/MG_Backend/DirectGLES/Managers.cpp:2799–2809；MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:14528–14547；现有 readback 入口/成本清单：docs/Disaggregated/design/07-memory-readback-verification.md:21–22 |
| D. transport 与同步 | 复用 UNIX/fd: aux，版本化 BufferCreate/FrameReady/BufferRelease；真实 acquire/release fence fd；buffer pool 与 fd 生命周期；compositor callback pacing。root-container 域做定向 fd/connect/mmap 验证。 | MobileGL/MG_Remote/Transport/FdPassing.cpp:108–149、:289–301；MobileGL/MG_Remote/Protocol/protocol.fbs:290–323；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809 |
| E. 多用户态 client / 多窗口 / context | N clients 先用 worker-per-client 隔离；M windows 要 per-surface image pool/事件/credit；context 状态、share groups、同步归属和并发 frontends 必须从全局状态拆出。 | MobileGL/MG_Remote/Server/ServerMain.cpp:1222–1260；MobileGL/GlobalObjects.cpp:23–24；MobileGL/MG_State/GLState/Core.h:701；MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp:38–39 |
| F. 通用应用验收 | 最小现代 GL EGL/GLX 程序→多 context/thread/share/window→桌面 toolkit/browser/Electron/Qt/Gtk 等目标工作负载→Xwayland glamor；按实测决定 GLES、compatibility 和 GL 版本广告。验证 resize、遮挡、无 callback、断连、释放 fence 与内容正确性。 | 第 2、5、6、7 节的具体缺口；历史跨机验收仅 trace：docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:10–17、:39–44 |

**inferred 的优先级判断：**最先证明“单 glibc arm64 client、单窗口、现代 GL、Android server 渲染→Anland 接受图像和同步→release 后复用”这一闭环，再扩大到 N×M 与 legacy/GLES。现有代码最有价值的是完整 command/state split 和已工作的 vendor-driver backend；最关键的新边界是 **frame/image ownership**，它目前不在 Present 协议中。（MobileGL/MG_Remote/Client/EmitTables.cpp:1165–1181；MobileGL/MG_Remote/Server/PipeApplier.cpp:388–438；MobileGL/MG_Pipe/MGPipeTypes.h:1807–1809）

### 8.2 本次识别的文档/代码漂移

| 文档说法 | 当前应采用的结论 |
|---|---|
| CMD/STAGE owner=client | producer 语义仍可这样称呼；跨进程四段的 allocator 是 server。（docs/Disaggregated/design/06-transport.md:9–17；MobileGL/MG_Remote/Server/ServerSession.cpp:737；MobileGL/MG_Remote/Transport/ShmSegment.cpp:253–284） |
| ShmLink 独立 aux，可 TCP+shm | 这是第二波设计；当前 client 拒绝该组合，fd API 仍在 ITransport/SocketTransport。（docs/Disaggregated/design/06-transport.md:62、:67；MobileGL/MG_Remote/Client/ClientSession.cpp:928–936；MobileGL/MG_Remote/Transport/ITransport.h:98–108） |
| split 今天固定 T2 / T0 握手 Fatal | 已被 P11 B2 取代：默认尝试 T0、不可用退 T2，T1 关闭。（docs/Disaggregated/design/07-memory-readback-verification.md:13；MobileGL/MG_Remote/CONTRACT-P11.md:233–248） |
| D8 debt 将 MetalLayer 列为仍接受 | 映射函数有 MetalLayer，但 wire decoder 先明确拒绝它；X11/Win32/None 才是残留接受项。（docs/Disaggregated/notes/DEBTS.md:34；MobileGL/MG_Remote/Protocol/SurfaceOpCodec.cpp:216–236） |
| device-lost 时 swap 返回 EGL_CONTEXT_LOST | 未找到实现映射；当前 Bool/void Present 链不能据文档认定标准 EGL 错误正确。（docs/Disaggregated/design/08-runtime-and-platform.md:33；MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp:207–228；MobileGL/MG_Backend/BackendObject.cpp:396–397） |
| present credit>1 从未测 | P12 已有 1/2/3 的跨机 trace 测量。（docs/Disaggregated/design/08-runtime-and-platform.md:8；docs/Disaggregated/notes/p12/CROSSHOST-ACCEPTANCE.md:23–34） |
| 一设备一个 server | 是 APK display/service 模式策略；B1 TCP service 同时有独立 UNIX broker supervisor，且进程全局不等于设备全局。（MobileGL/MG_Remote/CONTRACT-P12.md:46；android-plugin/app/src/main/java/top/mobilegl/plugin/MobileGLServerService.java:147–162） |

## 附录 A. 指定关键词逐命中用途索引（主仓库）

检索通过指定 WSL 入口进行；模式为大小写不敏感的 AHardwareBuffer / dma_buf / DMA_BUF / SCM_RIGHTS / sync_file / SYNC_FD / external_memory。先对主仓库 git tracked 内容做 git grep，再补查实际展开的 3rdparty 目录（附录 B）。主仓库结果 **586 个匹配行、134 个文件**，附录 B 另有 **2108 个匹配行、88 个文件**。同一行多个关键词只算一个匹配行。未把 build 产物、Git 元数据或二进制的任意字节串当产品代码；未执行被检索脚本。

下表列出每一个匹配行的位置；同文件同用途合并，连续数字表示区间内每一行均命中。引用方式为“文件路径 + 行号”。无主仓库 sync_file / SYNC_FD 命中；3rdparty 有标准 SYNC_FD 枚举，Asio 有 async_file 子串匹配，因此**不能说整个磁盘树完全没有这些字符串**。

| 文件（树根相对路径） | 所有命中行 | 用途 |
|---|---|---|
| CMakeLists.txt | 621 | 编译 P11 T0 store/hop 的产品源文件。 |
| CMakeLists.txt | 1138 | 将独立 extmem_probe 打包成 server 替身，测试 AHB 跨 app 路由。 |
| MobileGL/Config.h | 635 | T0 不可用时退 T2 的配置说明。 |
| MobileGL/MG_Backend/DirectGLES/Managers.cpp | 33, 1242, 1244, 2192, 2349, 2406, 2799, 2814, 2878, 3304, 3857, 4060 | P11 T0：导入 client AHB BLOB 为 GL buffer，persistent map / POST / GPU 完成后的引用释放；非 EGLImage 颜色目标。 |
| MobileGL/MG_Backend/DirectGLES/Managers.h | 1055, 1133, 1333 | P11 T0：导入 client AHB BLOB 为 GL buffer，persistent map / POST / GPU 完成后的引用释放；非 EGLImage 颜色目标。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.cpp | 266, 576, 631, 678, 683–684, 710, 742, 798, 920 | P11 T0：AHB BLOB→VkBuffer 的 external-memory import、映射、回读和延迟释放；非帧图像。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferManager.h | 145, 147, 265 | P11 T0：AHB BLOB→VkBuffer 的 external-memory import、映射、回读和延迟释放；非帧图像。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.cpp | 129 | P11 T0：AHB BLOB→VkBuffer 的 external-memory import、映射、回读和延迟释放；非帧图像。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VkBufferObject.h | 75–76, 80 | P11 T0：AHB BLOB→VkBuffer 的 external-memory import、映射、回读和延迟释放；非帧图像。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp | 15348, 15358, 15361–15362 | 仅为 P11 T0 buffer import 启用/记录 AHB external-memory device extensions。 |
| MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h | 171–172, 596 | 仅为 P11 T0 buffer import 启用/记录 AHB external-memory device extensions。 |
| MobileGL/MG_IntegrationTest/CMakeLists.txt | 2543, 2560, 4120, 4126 | T0 adoption 集成用例、主机没有 AHB 的回退断言和测试注册。 |
| MobileGL/MG_IntegrationTest/Harness/HeadlessGL.cpp | 102 | AImageReader 测试窗口的 sampled-image/color-output usage；测试 harness，不向 Linux compositor 导出帧。 |
| MobileGL/MG_IntegrationTest/Scenarios/LargeArenaAdoptionScenario.cpp | 103, 328, 414, 704 | T0 adoption 集成用例、主机没有 AHB 的回退断言和测试注册。 |
| MobileGL/MG_Pipe/MGPipeTypes.h | 155 | P11 T0 capability / resource ImportExternal 边界声明；persistent buffer store。 |
| MobileGL/MG_Pipe/PipeApply.h | 131, 134, 1319 | P11 T0 capability / resource ImportExternal 边界声明；persistent buffer store。 |
| MobileGL/MG_Remote/CONTRACT-P11.md | 3, 227, 240, 257, 275, 311–312 | P11 persistent map tiers、T0 BLOB offer/hop/import/POST 与测试；不是 present。 |
| MobileGL/MG_Remote/CONTRACT-P11.md | 80 | PairBind 后 aux 是 SCM_RIGHTS 的配对纪律。 |
| MobileGL/MG_Remote/CONTRACT-P11.md | 116 | B1 外部 client 握手接收 7 fd 的契约。 |
| MobileGL/MG_Remote/CONTRACT-P6.md | 39, 644 | Rule G 描述符边界及 aux/未来 ShmLink delivery 设计；不是帧协议。 |
| MobileGL/MG_Remote/Client/ClientSession.cpp | 858, 1187 | Client 采用 server 创建并发送的匿名共享段 fd。 |
| MobileGL/MG_Remote/Client/ClientSession.cpp | 2734, 2746, 2832, 2845 | T0 AHB store 协商、导入回复和回退/成功日志。 |
| MobileGL/MG_Remote/Client/ClientSession.h | 153, 171 | 共享段 Attach/Adopt/SCM_RIGHTS 启动契约。 |
| MobileGL/MG_Remote/Client/PersistentMapTracker.h | 272 | T0 persistent GL buffer 的客户端 backing store、wire map_persistent 及 CPU shadow 语义。 |
| MobileGL/MG_Remote/Client/WireTables.cpp | 951 | T0 persistent GL buffer 的客户端 backing store、wire map_persistent 及 CPU shadow 语义。 |
| MobileGL/MG_Remote/Protocol/mg_protocol_base.h | 105 | 跨进程 fd 交付/句柄存活期的协议边界说明。 |
| MobileGL/MG_Remote/Protocol/protocol.fbs | 46, 155 | SegmentRef/PairBind：fd 在 aux 交付，不把 fd 数字当跨进程对象。 |
| MobileGL/MG_Remote/Protocol/protocol.fbs | 73 | Hello 的 persistent-map adoptTier 说明。 |
| MobileGL/MG_Remote/Server/InProcessServer.h | 38 | Server 连接配对与 UNIX aux/共享段启动；不是 per-present buffer 交付。 |
| MobileGL/MG_Remote/Server/PairAcceptor.h | 12 | Server 连接配对与 UNIX aux/共享段启动；不是 per-present buffer 交付。 |
| MobileGL/MG_Remote/Server/ServerMain.cpp | 357, 1327 | Server 连接配对与 UNIX aux/共享段启动；不是 per-present buffer 交付。 |
| MobileGL/MG_Remote/Server/ServerSession.cpp | 1101, 1127, 1143, 1172, 1182, 1192 | T0 grant / 平台判定 / POST / 收 hop AHB / import / 回退。 |
| MobileGL/MG_Remote/Server/ServerSession.h | 302, 318 | T0 grant / 平台判定 / POST / 收 hop AHB / import / 回退。 |
| MobileGL/MG_Remote/Transport/AdoptT0.cpp | 42, 46, 50–54, 56, 60–61, 63–64, 85–86, 88, 101–103, 110–111, 113, 116–118, 121, 129, 133, 138, 144, 155, 172 | T0 AHB BLOB 分配、lock、hop send/recv、引用释放、host fallback；tier 名称/测试钩子。 |
| MobileGL/MG_Remote/Transport/AdoptT0.h | 10, 13, 18, 56, 61, 75, 85, 95 | T0 AHB BLOB 分配、lock、hop send/recv、引用释放、host fallback；tier 名称/测试钩子。 |
| MobileGL/MG_Remote/Transport/AdoptTier.h | 12 | T0 AHB BLOB 分配、lock、hop send/recv、引用释放、host fallback；tier 名称/测试钩子。 |
| MobileGL/MG_Remote/Transport/FdPassing.cpp | 147, 259 | 通用 AF_UNIX SCM_RIGHTS 单 fd sendmsg/recvmsg 与平台可用性。 |
| MobileGL/MG_Remote/Transport/FdPassing.h | 9, 12, 40 | 通用 AF_UNIX SCM_RIGHTS 单 fd sendmsg/recvmsg 与平台可用性。 |
| MobileGL/MG_Remote/Transport/ITransport.h | 89 | ITransport::ShareFd 契约：通过 aux 交 fd，调用者保留原 fd。 |
| MobileGL/MG_Remote/Transport/InProcessTransport.cpp | 200 | 同进程 transport 复制 fd，模拟 SCM_RIGHTS 所有权。 |
| MobileGL/MG_Remote/Transport/SessionRings.h | 129, 153, 165 | 匿名共享段创建、Attach/Adopt、映射/大小校验和描述符交付职责。 |
| MobileGL/MG_Remote/Transport/ShmSegment.cpp | 349, 423 | 匿名共享段创建、Attach/Adopt、映射/大小校验和描述符交付职责。 |
| MobileGL/MG_Remote/Transport/ShmSegment.h | 22, 55 | 匿名共享段创建、Attach/Adopt、映射/大小校验和描述符交付职责。 |
| MobileGL/MG_Remote/Transport/ShmSegmentPosix.cpp | 84 | 匿名共享段创建、Attach/Adopt、映射/大小校验和描述符交付职责。 |
| MobileGL/MG_Remote/Transport/ShmSegmentWin32.cpp | 89 | Windows 没有 SCM_RIGHTS；共享 section 按名称访问的说明。 |
| MobileGL/MG_Remote/Transport/SocketTransport.cpp | 650, 892 | control/aux socket 组织与 POSIX fd passing，Windows 不支持的路径。 |
| MobileGL/MG_Remote/Transport/SocketTransport.h | 34, 63, 77, 131 | control/aux socket 组织与 POSIX fd passing，Windows 不支持的路径。 |
| MobileGL/MG_Remote/Wire/PipeWireCodec.cpp | 1830 | T0 persistent GL buffer 的客户端 backing store、wire map_persistent 及 CPU shadow 语义。 |
| MobileGL/MG_Remote/Wire/PipeWireCodec.h | 509 | T0 persistent GL buffer 的客户端 backing store、wire map_persistent 及 CPU shadow 语义。 |
| MobileGL/MG_State/EGLState/Core.cpp | 180 | Android EGL config NativeVisualId 使用 RGBA8888 AHB format 常量；不是 AHB 分配/导出。 |
| MobileGL/MG_State/GLState/BufferState/BufferObject.cpp | 594 | T0 persistent GL buffer 的客户端 backing store、wire map_persistent 及 CPU shadow 语义。 |
| MobileGL/MG_Test/Wire/AdoptInboxTest.cpp | 11 | T0 offer/order/回退测试，主机用测试钩子；不证明 host 有 AHB allocator。 |
| MobileGL/MG_Test/Wire/CMakeLists.txt | 4, 26, 40 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| MobileGL/MG_Test/Wire/FdPassingTest.cpp | 9 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| MobileGL/MG_Test/Wire/InProcessTransportTest.cpp | 191 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| MobileGL/MG_Test/Wire/RemoteClientAdoptT0.inc | 3, 246, 249, 292, 303 | T0 offer/order/回退测试，主机用测试钩子；不证明 host 有 AHB allocator。 |
| MobileGL/MG_Test/Wire/ServerSpawnTest.cpp | 33, 257, 939 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| MobileGL/MG_Test/Wire/SessionTest.cpp | 1124 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| MobileGL/MG_Test/Wire/SocketTransportTest.cpp | 470 | SCM_RIGHTS / 共享段 / socket transport / spawn 的单元测试及构建注册。 |
| docs/Disaggregated/design/06-transport.md | 17 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/design/07-memory-readback-verification.md | 11 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p0/BRIEF-DOCS.md | 16 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p0/README.md | 37 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p0/p0-device-findings.md | 21, 23–24, 27 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p11/B0-CROSS-APP.md | 7, 43 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p11/INTEGRATOR-DECISIONS-P11.md | 23 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p11/PLAN-P11.md | 33, 37 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p12/device/2-onscreen-magma/check.log | 76, 99 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p12/device/trial-a4a940db/2-onscreen-magma/check.log | 64, 87 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p12/map-apk-process.md | 87–88 | P12 调研备选方案：child 渲染到 AHB，经 UNIX/SCM_RIGHTS 交 APK presenter；最终未选（BRIEF-onscreen.md:14–15）。 |
| docs/Disaggregated/notes/p12/map-docs-contract.md | 103 | P12 调研备选方案：child 渲染到 AHB，经 UNIX/SCM_RIGHTS 交 APK presenter；最终未选（BRIEF-onscreen.md:14–15）。 |
| docs/Disaggregated/notes/p12/map-server-surface.md | 113 | P12 调研备选方案：child 渲染到 AHB，经 UNIX/SCM_RIGHTS 交 APK presenter；最终未选（BRIEF-onscreen.md:14–15）。 |
| docs/Disaggregated/notes/p5/p5-results/s1-v1.md | 126, 358 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p5/scout-backend-install-and-thread.md | 350 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p5/scout-transport.md | 89, 100, 261, 289, 321 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p5c/p5c-audit-v1.md | 99 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p6/P6-CONTRACT-DRAFT.md | 33, 58 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p6/P6-ENDSTATE-REVIEW.md | 30, 137, 139, 143, 186, 399 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p6/P6-ENDSTATE-REVIEW.md | 138 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/p6/P6-SPAWN-PLAN.md | 17 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p6/README.md | 7, 16, 46 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p6/a6-audit-rows.md | 28 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p65/README.md | 17 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/p7/device-window-1/E0-attribution/E0a-inproc.log | 94, 158, 359, 416, 480, 1550, 1607, 1671, 1809, 1986, 2043, 2107, 2248, 2743, 2800, 2864, 2960, 3024, 3178, 3235, 3299, 3397, 3461, 3613, 3670, 3734, 3830, 3894, 4145, 4209, 4336 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E0-attribution/E0a-monolith.log | 175, 232, 296, 1914, 1971, 2035 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E1-lockstep/E1.log | 94, 158, 330, 387, 451, 592, 766, 823, 887, 983, 1047, 1201, 1258, 1322, 1419, 1483, 1693, 1757, 1902 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E7-coldcache/E7-cold-ra1-1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E7-coldcache/E7-cold-ra1-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E7-coldcache/E7-cold-ra1-3.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/E7-coldcache/E8-cold-fif1-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/F1-verify/F1-ra0.log | 130, 194, 367, 431 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/F1-verify/F1-ra1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/audit-cold-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/audit-warm-1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/bsl-mem-inproc.log | 68, 132, 263 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/bsl-mem-spawn.log | 67, 131 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/bsl-spawn.log | 67, 131, 499, 563 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump-inproc-1.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump-inproc-2.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump-inproc-3.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump-inproc-4.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump2-inproc-1.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump2-inproc-2.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump2-inproc-3.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump2-spawn-1.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/dump2-spawn-2.log | 96, 160 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif1-cold-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif4-1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif4-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif4-3.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif8-1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif8-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/fif8-3.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/matrix-inproc.log | 94, 158, 359, 416, 480, 1978, 2035, 2099, 2362, 2426, 2571 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/openra-cold-2.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/openra-cold-3.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/plain-cold-5.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/plain-warm-1.log | 94, 158 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/spawn-1.log | 93, 157 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/spawn-3.log | 93, 157 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/W4-verify/stats-lockstep-cold-1.log | 100 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/device-window-1/X1-verify/X1-dv-inproc.log | 41, 98, 162 | 历史运行日志中的 vendor Vulkan extension 枚举；不是调用点，也不是 frame export。 |
| docs/Disaggregated/notes/p7/ph-f.md | 104 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf1/final/part1.md | 135 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf1/final/part2.md | 18 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf1/final/part2.md | 171–172 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/recovered/wf1/final/part3.md | 16 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf1/final/part4.md | 13, 20, 50, 176–177, 191 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf1/final/part4.md | 178 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/recovered/wf2/final/part3.md | 139 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf2/final/part4.md | 19, 256 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf2/final/part4.md | 233 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| docs/Disaggregated/notes/recovered/wf2/final/synth-part3.md | 89 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf2/final/synth-part4.md | 16, 182 | 文档/历史审查：共享段、SCM_RIGHTS、配对、线程或 transport 平台边界；不新增实现。 |
| docs/Disaggregated/notes/recovered/wf2/final/synth-part4.md | 161 | 文档/历史设计或探针结果：外部内存 tiers、AHB BLOB persistent-map 或同内核 transport 可行性；不能当成当前 frame export。 |
| include/EGL/eglext.h | 517–518, 520 | Khronos EGL_ANDROID_get_native_client_buffer 的 AHB 类型/入口声明。 |
| include/EGL/eglext.h | 784–786, 788–796, 808, 810–822, 829 | Khronos dma-buf import/modifiers token 和 guard 声明；非实现。 |
| include/EGL/eglext.h | 1103–1104, 1111 | Khronos EGL_MESA_image_dma_buf_export guard 声明；非实现。 |
| scripts/ci/fatal_census.py | 206 | CI fatal/refusal census 对 T0 不可用→T2 的说明。 |
| tools/cts/platform/tcuMobileGLPlatform.cpp | 129–130 | AImageReader 测试窗口的 sampled-image/color-output usage；测试 harness，不向 Linux compositor 导出帧。 |
| tools/cts/probe/mgprobe.c | 158–159 | AImageReader 测试窗口的 sampled-image/color-output usage；测试 harness，不向 Linux compositor 导出帧。 |
| tools/piglit-android/README.md | 94 | 构建 piglit 时明确关闭 DMA_BUF tests 与 GBM。 |
| tools/piglit-android/patches/waffle-mobilegl-android.patch | 277–278 | AImageReader 测试窗口的 sampled-image/color-output usage；测试 harness，不向 Linux compositor 导出帧。 |
| tools/spikes/extmem_probe/CMakeLists.txt | 28 | 独立 probe 的 host 构建关闭 Android-only T0；保留其它 tier 探测。 |
| tools/spikes/extmem_probe/README.md | 23, 33, 50, 52–53, 55, 59–60, 157, 197–198 | 独立探针的 T0/T1/T3、BLOB 跨进程与真实 route 说明；不接 renderer Present。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 8, 16, 18, 20 | 独立探针 T1 fd export / T0 AHB import / T3 host import 的范围说明。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 407, 428, 469 | 独立探针自己的 SCM_RIGHTS sendmsg/recvmsg。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 580–581, 609, 627–630, 646–649, 651, 916, 918, 920, 925, 1477, 1491, 1628–1630, 1653–1657, 1673–1674, 1676, 1678–1679, 1695 | 探针 extension 枚举、external buffer capabilities/配置/类型和报告；没有 renderer 帧交付。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 1718, 1721–1722, 1771, 1773, 1904, 1983–1984, 1991, 2012, 2015, 2116, 2182–2183, 2188 | T1：host-visible VkBuffer 外部内存导出/接收/导入，opaque fd 或 DMA_BUF，及 GL memory-object import 对照。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 2356, 2370, 2374–2378, 2384, 2388–2389, 2395, 2404, 2407–2408, 2414, 2420, 2428, 2432, 2479–2480, 2487–2488, 2501, 2504, 2507, 2513, 2522, 2535–2536, 2778 | T0：AHB BLOB client allocation、CPU lock、socket handoff、Vulkan/GL buffer import 与字节验证。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 2784, 2796, 2808, 2812–2816, 2822, 2826–2827, 2830–2831, 2837, 2842, 2846, 2853, 2884, 2886, 2891, 2943, 2972–2973, 2981, 2989, 2999, 3005–3006 | T0S：保持 AHB CPU lock 的持续 CPU/GPU 读写一致性 probe。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 3219, 3232–3233, 3360, 3364, 3372, 3381–3383, 3394, 3399, 3451, 3455–3459, 3463, 3467–3468, 3471, 3488, 3492, 3515, 3569, 3576 | B2 route probe：经真实 app/socket direct 或 hop 交 AHB BLOB；host SKIP 分支。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 3590, 3632, 3638–3639, 3664, 3793, 3925–3926 | T3：VK_EXT_external_memory_host 导入 memfd/host mapping 的测试。 |
| tools/spikes/extmem_probe/extmem_probe.cpp | 4199–4200, 4204, 4206, 4208 | 探针 main 调度 T1/T0/T0S/T3，各为 buffer 内存测试。 |

## 附录 B. 已展开第三方目录的全部匹配

这些文件随依赖源码在树内，但不是 MobileGL 已接通的帧协议。其中 DiligentCore 的 add_subdirectory 在主 CMake 中仍注释掉（CMakeLists.txt:277），VMA/Vulkan headers/utilities 则进入依赖（CMakeLists.txt:280–282）。任何上游函数声明、loader、sample 或 Win32 export 都没有改变第 3 节的产品 Present 调用链。

| 文件（树根相对路径） | 所有命中行 | 用途 |
|---|---|---|
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan.cppm | 525, 1233, 1237, 1242, 1304, 1309, 1315, 1320, 1504, 1522, 1690, 2246, 2271, 2441, 2619, 2738, 3554, 3557, 3562, 3585, 3592, 3724, 3868, 4293, 4327, 4480, 4683, 4847, 6534, 6538, 6545, 6589, 6600, 6801, 7038, 7693, 7746, 7982, 8311, 8583 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan.hpp | 3087, 3104, 3217, 3227, 3244, 3715, 3718, 3726, 4268, 5205, 5301, 6036, 6246, 7196, 7199, 7507–7511, 7513–7517, 7520–7524, 7589–7591, 7593–7595, 7598–7600, 7603–7605, 7791–7793, 7808–7810, 7977–7979, 8534–8536, 8559–8561, 8729–8731, 8904–8906, 9022–9024, 11655, 11675, 11755, 11775, 12124, 12706, 15066, 15163, 16070, 17299, 18135, 18780, 18784, 18817, 18821, 18829, 18967, 19073, 19338, 19377, 19544, 19596, 20088, 20093, 20152, 20159, 20165, 20325, 20472, 20843, 20876, 21084, 21157, 21469, 21491, 21496, 21599, 21739, 22053, 22086, 22276, 22341 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_android.h | 46–50, 80, 108–109, 114, 120 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_core.h | 311–312, 565, 730, 973, 1052, 1165, 1302–1303, 5241–5265, 5270–5276, 5284, 5288, 5321, 5328, 10023–10026, 10058–10061, 10071–10074, 13544–13547, 13550–13554, 13559–13562, 13587–13590, 14152–14155, 15239–15242, 18172–18173, 18175–18176, 19325–19328 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_enums.hpp | 450–453, 887, 1059, 1312, 1389, 1509, 4239–4246, 4248, 4250–4251, 4253, 4255, 4257, 4260–4262, 4302–4304, 4329, 4419, 6184, 6190–6193, 6214–6216 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_extension_inspection.hpp | 55–56, 58, 114, 116, 127, 129, 131, 170, 174, 219, 356, 362, 402, 446, 476, 512, 519, 696, 699, 702, 705, 710, 743, 749, 752, 756, 759, 763, 766, 773, 990, 993, 998, 1008, 1013, 1170, 1173, 1929, 1932–1933, 1945, 1950, 1974, 1977, 2199, 2202, 2263, 2432, 2437, 2563, 2566, 2663–2664, 2778, 2780, 2782, 2784, 2787, 2789, 2907, 2911, 3269–3270, 3272, 3302, 3304, 3309, 3311, 3313, 3335, 3338, 3362, 3432, 3434, 3455, 3477, 3492, 3523, 3528, 3576 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_fuchsia.h | 45–48 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_funcs.hpp | 7522, 13547, 13590, 13610, 13632, 14389, 14415, 14428, 14449, 14486, 14502, 14522, 14556, 17337, 17343, 17357, 17362, 17377, 17382, 17401, 17413, 17419, 17422, 20728, 20757, 26007, 26031, 26072, 26466, 26489, 30367, 30390, 30409, 31687, 31708, 31747 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_handles.hpp | 756, 759, 764, 787, 794, 926, 1070, 1495, 1529, 1682, 1885, 2049, 13578, 13625, 13656, 14084, 14090, 14098, 14103, 14110, 14116, 15099, 16209, 16405, 17308, 17628, 18716, 18912 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_metal.h | 192–195 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_raii.hpp | 190, 233, 507, 529, 983, 1005, 1010, 1113, 1253, 1567, 1600, 1790, 1855, 2147, 2162, 2170, 2258, 2363, 2575, 2614, 2767, 2815, 3694, 3770, 4606, 4617, 4735, 4740, 4746, 4750, 4985, 5252, 5289, 5499, 5577, 9559, 17061, 19577, 19590, 19608, 19613, 19993, 20001, 20012, 20018, 20034, 20048, 20053, 20067, 21222, 21227, 21230, 21244, 21247, 21261, 21265, 21267, 22737, 22745, 25028, 25035, 25051, 25213, 25219, 26994, 27002, 27019, 27488, 27494, 27511 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_screen.h | 52–55 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_static_assertions.hpp | 2897, 2905, 2920, 2979, 3006, 3497, 4214, 6021, 6170, 6826, 7729, 8586 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_structs.hpp | 56639, 56668, 56699, 56728 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_to_string.hpp | 2803, 8124 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/include/vulkan/vulkan_win32.h | 51–54, 216–219 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/registry/validusage.json | 2596, 7211, 7216, 7310, 7315, 8044, 8049, 8373, 8378, 8735, 20628, 20683, 20703, 20708, 20743, 20748, 20753, 20758, 20763, 20768, 20773, 20783, 20788, 20793, 20798, 20803, 20808, 20813, 20833, 20873, 20878, 20883, 20937, 20942, 20947, 20952, 20962, 20967, 21179, 21213, 21445, 21503, 21537, 21580, 21590, 21595, 21619, 21629, 21634, 21682, 21692, 21711, 21720, 21749, 21759, 21867, 21891, 21953, 22345, 22403, 22437, 22690, 22883, 23299, 23990, 23995, 24020, 24025, 24446, 24460, 24705, 24798, 27017, 27022, 27185, 27190, 27419, 27577, 27744, 27912, 103968, 104608, 105427, 106451 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/Vulkan-Headers/registry/vk.xml | 236, 2441, 2485, 2937, 2943, 4275, 4559, 6973, 9052, 9612, 11249–11252, 11255–11257, 11361–11367, 11370–11372, 11380, 11393, 14840, 14847, 17627, 17646–17648, 19478, 19480–19481, 19490, 19492–19494, 19500, 19502–19503, 19511, 19731, 19733–19734, 19743–19749, 19752–19754, 19764, 19766–19769, 19778, 19780–19781, 19794, 19796–19797, 19808, 19830, 20274, 20464, 20466–20468, 20471, 20524, 20526–20528, 20543, 21488, 21490–21491, 21494–21496, 24101, 24103–24104, 24108, 24129, 24213, 24215–24216, 24218, 24220, 24278, 24280–24281, 24286–24288, 25124, 25126–25128, 25432, 26388, 26390–26392, 26397, 27139, 27232, 27234–27235, 27239–27241 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/DiligentCore/ThirdParty/glew/auto/doc/log.html | 40 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/auto/extensions/gl/EGL_EXT_image_dma_buf_import | 1, 3, 5, 7–15 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/auto/extensions/gl/EGL_EXT_image_dma_buf_import_modifiers | 1, 3, 5–15 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/auto/extensions/gl/EGL_MESA_image_dma_buf_export | 1, 3 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/doc/log.html | 138 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/include/GL/eglew.h | 798, 800–801, 803, 805–813, 826, 828, 830, 832–833, 835–845, 853, 855, 1801, 1803–1804, 1812, 1814, 2506–2507, 2568 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/src/glew.c | 17567–17568, 17629, 17861, 17863, 17873, 18177, 18179, 18189, 18476–18482, 18684–18687, 27867–27868, 27870, 27874–27875, 27877, 28313–28314, 28316 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/glew/src/glewinfo.c | 10644, 10646, 10648, 10651, 10653, 10655, 10657, 10663, 11276, 11278, 11280, 11286, 15990–15995, 16176–16178 | 上游 GLEW EGL extension 元数据、入口加载和 glewinfo 枚举；不实现 image 导入/导出。 |
| 3rdparty/DiligentCore/ThirdParty/volk/volk.c | 329, 331, 416, 418, 677, 680, 749, 751–752, 755, 863, 866, 983, 986–987, 990, 1187, 1189–1190, 1192, 1261, 1263, 1621, 1624, 1693, 1695–1696, 1699, 1807, 1810, 1927, 1930–1931, 1934, 2131, 2133–2134, 2136, 2205, 2207, 2601, 2604, 2710, 2712–2713, 2716, 2835, 2838, 2983, 2985–2986, 2989–2990, 2993, 3264, 3266–3267, 3269–3270, 3272, 3342, 3344 | 上游 volk Vulkan entrypoint dispatch loader；有函数指针不代表 MobileGL 调用了它。 |
| 3rdparty/DiligentCore/ThirdParty/volk/volk.h | 477, 482, 579, 583–584, 589, 743, 748, 909, 914–915, 920, 1201, 1205–1206, 1210, 1307, 1311, 1753, 1756, 1862, 1864–1865, 1868, 1987, 1990, 2135, 2137–2138, 2141–2142, 2145, 2416, 2418–2419, 2421–2422, 2424, 2494, 2496 | 上游 volk Vulkan entrypoint dispatch loader；有函数指针不代表 MobileGL 调用了它。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan.cppm | 434, 438, 467, 471, 476, 601, 708, 955, 982, 1057, 1184, 1247 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan.hpp | 4111, 4128, 4241, 4251, 4270, 4751, 4754, 4762, 5385, 6397, 6493, 6818, 7462, 7728, 8839, 8842, 9158–9162, 9164–9168, 9171–9175, 9240–9242, 9244–9246, 9249–9251, 9254–9256, 9442–9444, 9459–9461, 9636–9638, 10203–10205, 10228–10230, 10407–10409, 10412–10414, 10629–10631, 10785–10787, 13505, 13525, 13605, 13625, 13992, 14702, 17015, 17112, 18058, 18132, 19895, 21009, 22090, 22094, 22127, 22131, 22139, 22277, 22395, 22666, 22705, 22783, 22913, 22980, 23495, 23500, 23559, 23566, 23572, 23732, 23892, 24273, 24306, 24403, 24565, 24652, 24988, 25010, 25015, 25118, 25269, 25593, 25626, 25714, 25859, 25933 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_android.h | 48–52, 82, 110–111, 117, 125 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_core.h | 292–293, 563, 744, 986, 1073, 1099, 1258, 1450–1451, 5481–5506, 5511–5517, 5525, 5529, 5562, 5569, 10409–10412, 10446–10449, 10459–10462, 14715–14718, 14721–14725, 14730–14733, 14760–14763, 15355–15358, 16916–16919, 20010–20011, 20013–20014, 21289–21292 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_enums.hpp | 421–424, 893, 1081, 1333, 1420, 1446, 1619, 4437–4451, 4453, 4455–4456, 4458, 4460, 4462, 4465, 4468–4470, 4513–4518, 4546–4547, 4645–4646, 6697, 6703–6706, 6727–6729 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_extension_inspection.hpp | 58–59, 61, 117, 119, 130, 132, 134, 173, 177, 224, 363, 369, 412, 414, 469, 507, 557, 564, 756, 759, 763, 766, 771, 807, 813, 816, 821, 824, 829, 832, 840, 1067, 1070, 1075, 1086, 1091, 1268, 1271, 2063, 2066–2067, 2079, 2084, 2109, 2112, 2367, 2372, 2381, 2384, 2447, 2724, 2729, 2943, 2946, 3164–3165, 3289, 3291, 3293, 3295, 3298, 3300, 3422, 3426, 3824–3825, 3827, 3857, 3859, 3864, 3866, 3868, 3890, 3893, 3917, 3987, 3989, 4012, 4014, 4041, 4059, 4097, 4102, 4158 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_fuchsia.h | 47–50 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_funcs.hpp | 6628, 12530, 12573, 12593, 12613, 13345, 13370, 13383, 13403, 13438, 13454, 13473, 13504, 16197, 16203, 16215, 16220, 16239, 16244, 16261, 16272, 16278, 16281, 19666, 19691, 24641, 24663, 24702, 25064, 25085, 26384, 26406, 26425, 26456, 30136, 30158, 30177, 31542, 31562, 31597 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_handles.hpp | 755, 758, 763, 786, 793, 925, 1102, 1523, 1557, 1715, 1724, 2027, 2244, 14312, 14361, 14393, 14853, 14860, 14869, 14878, 14886, 14893, 16022, 17210, 17422, 17790, 18888, 19245, 20392, 20616 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_metal.h | 196–199 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_ohos.h | 22–23, 25–26 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_raii.hpp | 179, 222, 526, 548, 1033, 1055, 1060, 1163, 1314, 1638, 1671, 1759, 1904, 1978, 2282, 2297, 2305, 2393, 2509, 2727, 2766, 2835, 2957, 3013, 3893, 3976, 4763, 4774, 4895, 4900, 4906, 4910, 5147, 5403, 5440, 5543, 5751, 5824, 9910, 17419, 19870, 19883, 19901, 19907, 20295, 20303, 20314, 20320, 20336, 20350, 20355, 20369, 21554, 21559, 21562, 21576, 21579, 21592, 21596, 21598, 23134, 23142, 25369, 25376, 25392, 25554, 25560, 26225, 26232, 26248, 26263, 27776, 27784, 27801, 28316, 28322, 28339 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_screen.h | 56–59 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_static_assertions.hpp | 2897, 2905, 2920, 2979, 3006, 3497, 4415, 6212, 6361, 7023, 7061, 8456, 9528 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_structs.hpp | 77061, 77096, 77102, 77130, 77157 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_to_string.hpp | 2907, 8814 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/include/vulkan/vulkan_win32.h | 55–58, 232–235 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/registry/validusage.json | 2846, 7751, 7756, 7850, 7855, 8529, 8534, 8883, 8888, 9163, 9168, 9603, 22402, 22462, 22482, 22487, 22522, 22527, 22532, 22537, 22542, 22547, 22552, 22562, 22567, 22572, 22577, 22582, 22587, 22592, 22612, 22652, 22657, 22662, 22716, 22721, 22726, 22731, 22741, 22746, 22833, 22982, 23016, 23248, 23306, 23340, 23383, 23393, 23398, 23422, 23432, 23437, 23485, 23495, 23514, 23523, 23552, 23562, 23670, 23694, 23756, 24148, 24206, 24240, 24503, 24691, 25132, 25939, 25944, 25969, 25974, 26425, 26556, 26801, 26894, 29264, 29269, 29513, 29518, 29752, 29920, 30097, 30270, 30575, 31144, 118352, 119055, 120081, 121223 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Headers/registry/vk.xml | 245, 2565, 2609, 3082, 3088, 4549, 4833, 7281, 9443, 10058, 10954, 12457–12460, 12463–12465, 12571–12577, 12580–12582, 12590, 12603, 16214, 16221, 19260, 19279–19281, 21272, 21274–21275, 21284, 21286–21288, 21294, 21296–21297, 21305, 21535, 21537–21538, 21547–21553, 21556–21558, 21568, 21570–21573, 21582, 21584–21585, 21598, 21600–21601, 21612, 21634, 22104, 22294, 22296–22298, 22301, 22354, 22356–22358, 22373, 23486, 23488–23489, 23492–23494, 26166, 26168–26169, 26173, 26194, 26278, 26280–26281, 26283, 26285, 26343, 26345–26346, 26351–26353, 27203, 27205–27206, 27220, 27225, 27227–27229, 27427, 27620, 28724, 28726–28728, 28733, 29676, 29678–29679, 29683–29685 | 上游 Vulkan C/C++ API 类型、常量、dispatch/RAII wrapper 或 registry/valid-usage 规则；含 AHB/external memory/SYNC_FD 声明，不是 MobileGL 接线。 |
| 3rdparty/Vulkan-Utility-Libraries/include/vulkan/utility/vk_safe_struct.hpp | 11766 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/include/vulkan/utility/vk_struct_helper.hpp | 106–107, 580, 701, 909, 998, 1023, 1124 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/include/vulkan/vk_enum_string_helper.h | 295–298, 837–838, 1175–1176, 1641–1642, 1805–1806, 1857–1858, 2169–2170, 7719–7754, 7778–7783, 7813–7814, 7923–7924, 9969–9976, 10000–10005, 11602, 11604, 12530, 12760, 13160, 13332, 13382, 13579 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/scripts/generators/safe_struct_generator.py | 53 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/src/vulkan/vk_safe_struct_core.cpp | 7708, 7756 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/src/vulkan/vk_safe_struct_ext.cpp | 5408, 14352 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/src/vulkan/vk_safe_struct_utils.cpp | 129, 132, 1064, 1292, 1707, 1915, 1963, 2157, 2508, 2511, 3443, 3671, 4086, 4294, 4342, 4536 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/Vulkan-Utility-Libraries/src/vulkan/vk_safe_struct_vendor.cpp | 799, 10627, 14544, 19724 | 上游 Vulkan enum→string、sType、safe-struct 拷贝和生成器支持；不创建或发送呈现 buffer。 |
| 3rdparty/VulkanMemoryAllocator/CHANGELOG.md | 27 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/Doxyfile | 2469–2470 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/README.md | 56 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/faq.html | 126 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/globals.html | 110 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/globals_eval.html | 103 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/group__group__alloc.html | 2500–2501, 2547–2552, 2561, 2565 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/group__group__init.html | 132, 359, 557, 569 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/other_api_interop.html | 84, 89–90, 97, 101, 153, 191, 195, 209, 212 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/quick_start.html | 162 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/search/all_14.js | 66 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/search/enumvalues_0.js | 29 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/vk__mem__alloc_8h.html | 220 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/docs/html/vk_khr_external_memory_win32.html | 8, 78, 81–82, 86–87, 89, 91, 96, 100, 147, 151, 177, 189, 193 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/include/vk_mem_alloc.h | 242–245, 247, 251–254, 256, 480, 486, 1062, 1136, 1147, 1711, 2169, 2195, 2199–2200, 2221–2226, 2234, 2249–2250, 2265, 6396, 6483, 6552, 6559, 6670, 6672, 10716, 10721, 10729, 10731, 11137, 11143, 11458, 11474, 12078, 12086, 13217, 13310, 13313, 13324, 13326, 13350, 13356, 13540, 13648, 13702, 13959, 13967, 15625–15626, 17112, 17118, 17124–17129, 17133, 17298, 17523, 18933, 18946, 18949, 18966, 18970, 19028, 19070, 19075, 19120, 19123 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/src/Tests.cpp | 42, 8490–8491, 8497, 8506, 8530, 8536, 8584–8585, 8591, 8600, 8624, 8632 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/VulkanMemoryAllocator/src/VulkanSample.cpp | 72, 1494, 1540, 1542, 1933–1934, 2095–2096 | 上游 VMA 通用 external-memory allocation/Win32 handle 导出支持、示例测试及文档；没有 MobileGL 帧 dma-buf/fence 协议。 |
| 3rdparty/apitrace/specs/dxgi.py | 732, 740 | DXGI DMA_BUFFER_BOUNDARY 枚举；与 Linux dma-buf 无关的子串匹配。 |
| 3rdparty/apitrace/specs/eglapi.py | 62, 302, 304–312, 318–329, 422, 477 | apitrace 的 EGL API/enum trace 描述及 Khronos header；不是 MobileGL EGL extension 实现。 |
| 3rdparty/apitrace/specs/eglenum.py | 275, 277–285, 340–350 | apitrace 的 EGL API/enum trace 描述及 Khronos header；不是 MobileGL EGL extension 实现。 |
| 3rdparty/apitrace/thirdparty/khronos/EGL/eglext.h | 517–518, 520, 770–772, 774–782, 794, 796–808, 815, 1089–1090, 1097 | apitrace 的 EGL API/enum trace 描述及 Khronos header；不是 MobileGL EGL extension 实现。 |
| 3rdparty/asio/asio.manifest | 6515 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/asio/src/examples/cpp11/Makefile.am | 32, 184 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/asio/src/examples/cpp11/files/.gitignore | 1 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/asio/src/examples/cpp11/files/async_file_copy.cpp | 2, 79 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/asio/src/examples/cpp11/local/fd_passing_stream_client.cpp | 71 | Asio 的独立 AF_UNIX SCM_RIGHTS 示例程序；非 MobileGL transport。 |
| 3rdparty/asio/asio/src/examples/cpp11/local/fd_passing_stream_server.cpp | 87 | Asio 的独立 AF_UNIX SCM_RIGHTS 示例程序；非 MobileGL transport。 |
| 3rdparty/asio/boost_asio.manifest | 6951 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/src/examples/cpp11/Makefile.am | 32, 184 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/src/examples/cpp11/files/.gitignore | 1 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/src/examples/cpp11/files/async_file_copy.cpp | 2, 79 | async_file_copy 示例/清单；sync_file 命中是 async_file 的子串，非 Linux GPU sync_file。 |
| 3rdparty/asio/src/examples/cpp11/local/fd_passing_stream_client.cpp | 71 | Asio 的独立 AF_UNIX SCM_RIGHTS 示例程序；非 MobileGL transport。 |
| 3rdparty/asio/src/examples/cpp11/local/fd_passing_stream_server.cpp | 87 | Asio 的独立 AF_UNIX SCM_RIGHTS 示例程序；非 MobileGL transport。 |

