# MobileGL 作为 Anland 的 GL 提供者：调研（2026-09-30）

> 非阶段笔记。只读源码调研，**没做原型、没上设备**。Anland 审计于 `SuperTurtleDev/anland@cc169180`（6.x main），MobileGL 审计于 `p8/main@648800a2`（P8 收官头）。结论里凡是"要做 / 要改"的都是推断，事实部分在两份报告里逐行有出处（方案 B 与 Mesa 对照两处的 Anland 事实直接链到源码）。

| 文件 | 内容 |
|---|---|
| [`report-anland.md`](report-anland.md) | Anland 侧：缓冲入口契约、伪造 AHB、同步、帧节奏、进程与 SELinux、容器 GPU 现状、Xwayland glamor、衍生项目、改动插入点（382 处引用） |
| [`report-mobilegl.md`](report-mobilegl.md) | MobileGL 侧：Linux client 构建与导出、EGL / GLX 能力、split 的 present 路径、AHB / fd / fence 现状、会话与多窗口限制、GL 覆盖面、文档漂移 |
| [`kgsl-as-server.md`](kgsl-as-server.md) | 把内核 kgsl 看成 server：A / B 实为"缓冲归谁"之分，6.x + Mesa 是 A、5.x 是 B；拆分层级与 virtio-gpu native context / virgl 的对应 |
| [`plan-ahb-dmabuf.md`](plan-ahb-dmabuf.md) | 计划：server 分配 AHB 当 dma-buf，linux-dmabuf + GBM 后端 + 薄节点，窗口零拷贝；厂商中立规则与分阶段 |
| [`brief-anland.md`](brief-anland.md)、[`brief-mobilegl.md`](brief-mobilegl.md) | 派给 Codex 的原始提示词。**两份都把后端名写反了**：Espryt = DirectGLES（GLES），Magma = DirectVulkan（Vulkan）；报告已纠正 |

## 结论

可行，但两边都要新东西：

- MobileGL 没有"把帧交回 client"这条路：`MGPPresent` 只有 `FrameSerial`，P12 画在 server 自己的 SurfaceView 上；没有 Wayland WSI、没有可导出的颜色图像、没有 sync_file。
- Anland 的 GPU 缓冲入口只有 `linux-dmabuf` v3 的单平面线性 AR24/XR24，靠高通 snapalloc 布局伪造 AHB（非高通直接失败，没有 CPU 退路）；没有原生 AHB 入口。
- 两条路线：**A** 走标准 Wayland 缓冲（MobileGL 像 Mesa 一样交 `wl_buffer`）；**B** 让 Anland 把窗口的 Surface 直接交给 MobileGL server（FCL 形态）。一个应用一个 GL 顶层窗口（游戏）选 B：接口面小、零拷贝、厂商中立，MobileGL 侧主要是推广 P12 已有机制；A 的 `wl_shm` 回读作兜底，A 的原生 AHB 入口按需再做。

## Anland 是什么

root Android（KernelSU / SukiSU 模块）上的 Wayland 宿主：守护进程 `waylandbridge`（root，`awl_daemon` 域，bionic，链 Android EGL / GLESv3）实现 Wayland 协议。每个 `xdg_toplevel` 按需挂到一个独立的 Android 窗口（一个 Activity，来自宿主 APK 或接入 libawl 的应用；可随时分离，Wayland 窗口不死）；多个 toplevel 可同时各挂一个；popup 与子 surface 合成进父窗口，不单独开窗。Droidspaces 容器经 `/run/anland/wayland-0`（绑定自 `/data/local/tmp/awl`）连入。容器内 GPU 加速 = Mesa freedreno / turnip on kgsl，只有 Adreno；Mali 等走 llvmpipe。

## 两条路线

```
A：标准 Wayland 缓冲
容器应用 ─ MobileGL client（glibc libEGL/libGL）── wl_buffer（wl_shm / AHB 令牌）──→ waylandbridge ─ SC / GLES 合成 ─ SurfaceFlinger
   │ 同机 unix socket + 共享内存（split 现成）                                          ↑
Android 侧 MobileGL render server（root 守护进程，厂商驱动）── AHB 完整句柄（原生入口）──┘

B：窗口 Surface 直通
容器应用 ─ MobileGL client ─ 令牌（Anland 扩展）─→ waylandbridge
   │ 同机 unix socket + 共享内存                         │ binder：窗口 Surface + 挂上 / 分离 / 尺寸
Android 侧 MobileGL render server ←────────────────────────┘
   └ 厂商 EGL / Vulkan swapchain 直接画进该 Surface ─ SurfaceFlinger（Anland 的 popup / 子 surface / 光标作为子层叠在上面）
```

- 两条路线里 Wayland 连接、`xdg_toplevel`、输入与输入法都归 Linux 应用 / Anland；MobileGL 只管帧。
- P12 的 server 自有窗口不适用：Anland 下 Android 窗口归 Anland（Activity 的 Surface 交给 waylandbridge 绘制，输入也从这里转成 Wayland 事件），应用的帧必须作为 `wl_buffer` 提交到自己的 `wl_surface`；P12 画进 MobileGL 自己的 Activity，会绕开 Anland 的窗口与输入。B 不是 P12：窗口仍是 Anland 的，只是生产者换成 MobileGL。
- 容器内直接让 Magma 跑在 turnip 上：只有 Adreno，和现有 Mesa 同一驱动，价值小；MobileGL 的价值是把厂商驱动（含 Mali）带进来，只能靠 split 跨到 bionic。

## 对照：容器里现在的 Mesa（freedreno / turnip）

Mesa 属于 A，准确地说是 A 里"Anland 不改、走 dma-buf"那一档，只是渲染在应用进程内、没有 server。

| | Mesa freedreno / turnip | 方案 A | 方案 B |
|---|---|---|---|
| 在哪渲染 | 应用进程内，容器里直接调 kgsl（[anland-session.sh:108–112](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/anland-session/anland-session.sh#L108-L112)：`GALLIUM_DRIVER=kgsl`、`FD_FORCE_KGSL=1`） | Android 侧 render server | Android 侧 render server |
| 缓冲谁分配 | 应用进程里的 Mesa 经 kgsl 分配，没有 gralloc 元数据 | server 用 gralloc 分配 AHB | Anland 窗口 Surface 自己的缓冲队列 |
| 怎么交给 Anland | 标准 Wayland：`zwp_linux_dmabuf_v1` 单平面线性 AR24/XR24 | 标准 Wayland：`wl_buffer` | 不经过 Wayland 缓冲，直接画进 Surface |
| Anland 怎么处理 | 伪造 AHB（高通 snapalloc donor）→ SC / GLES 合成 | 原生 AHB 入口，不伪造 | 不经手，只在上面叠子层 |
| 同步 | 隐式：提交时 `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` 从 dma-buf 抓写 fence，失败就轮询 dma-buf（[awl_surface.c:199–209](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_surface.c#L199-L209)）；Mesa EGL 不走 explicit sync v1 是推断 | explicit sync（sync_file） | 缓冲队列内部 |

Anland 6.x 的 dma-buf 入口就是照 Mesa / kgsl 的产物做的：

- 伪造 AHB 的起因：注释写明容器 kgsl dma-buf 缺 gralloc 元数据，直接导入被拒（[awl_ahb.cpp:10–14](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_ahb.cpp#L10-L14)）；这也是只有 Adreno 能加速的原因之一。
- Xwayland 补丁迁就 kgsl：把 DRI3 报的错误 modifier `1274` 改成 LINEAR，强制提交 `DRM_FORMAT_MOD_INVALID`，无 v4 feedback 也启用 glamor（[anland-kgsl-glamor.patch](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/patches/xwayland/anland-kgsl-glamor.patch)）。
- SC 直出 / 拷贝切换按 turnip WSI 调：vkmark 123 fps 对 ~1300 fps（[awl_sc.cpp:60–75](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_sc.cpp#L60-L75)）。

对 MobileGL 的含义：Mesa 相当于"容器里的 monolith MobileGL"，因为 kgsl 能在 glibc 进程里直接用；厂商驱动（bionic）不能，所以 MobileGL 必须 split，渲染挪到 Android 侧，缓冲来源从 kgsl dma-buf 变成 gralloc AHB。"Magma 跑在容器 turnip 上"就是 Mesa 这条路的翻版。

B 在 6.x 没有对应物（今天只有 waylandbridge 自己往窗口 Surface 里画）。5.x 的私有 Anland Display Protocol 介于两者之间：

- 像 B：Android 端从窗口自己的缓冲队列 `dequeueBuffer`，只导出 `handle->data[0]` 给容器里的合成器（如 KWin + Mesa），合成器画完回 sync_file，Android 端 `queueBuffer`；缓冲属于窗口，零拷贝、不伪造（[protocol.h:7–49](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/common/protocol.h#L7-L49)、[native_consumer.c:144–172](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c#L144-L172)、[:955–1003](https://github.com/SuperTurtleDev/anland/blob/9ab13eb146bc9f3dd3cc44cabc268659d9eb0a46/consumers/anland_v5/android_consumer/app/src/main/jni/native_consumer.c#L955-L1003)）。
- 像 A：缓冲仍以 dma-buf 跨进程，依赖容器 Mesa / kgsl 能把 gralloc 的 `data[0]` 当线性缓冲导入，同样只有 Adreno。

6.x 为了让原生合成器不改就能接入，换回了标准 Wayland 缓冲（A 这条路）。

换个角度——把内核 kgsl 当 server 读这张表——见 [`kgsl-as-server.md`](kgsl-as-server.md)。

## 两边现状

| | MobileGL | Anland |
|---|---|---|
| 帧入口 / 出口 | `MGPPresent{FrameSerial}`；P12 画在 server 窗口 | `wl_shm` v2（ARGB / XRGB）；`linux-dmabuf` v3 单平面线性 AR24/XR24 |
| 窗口 / Surface | P12 `ServerDisplay` 租约（acquire / release / 几何），每进程一个自有窗口，失窗即 `Fatal{ServerWindowLost}` | 每个挂上的窗口一个 Surface，经 binder `T_SURFACE` 用 `ANativeWindow_readFromParcel` 收进守护进程（[waylandbridge.cpp:1587–1626](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/waylandbridge.cpp#L1587-L1626)）；EGL 模式守护进程是它的生产者（[awl_renderer.cpp:101](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_renderer.cpp#L101)），SC 模式只从它挂子层（[awl_sc.cpp:676](https://github.com/SuperTurtleDev/anland/blob/cc1691805fe6aca6851b6fcccb6a1e33019026c1/services/waylandbridge/awl_sc.cpp#L676)），缓冲队列空闲 |
| 窗口系统 | 无 Wayland；EGL 扩展只有 `EGL_KHR_create_context`、`EGL_MESA_platform_surfaceless` | dma-buf → 高通 snapalloc donor 伪造 AHB（写死 HAL BGRA=5，要求 2 fd 句柄） |
| 同步 | 无 sync_file / SYNC_FD；`eglCreateSync` 创建即 signaled，`eglCreateImage` 只登记 | explicit sync v2（sync_file），acquire / release 都有；无 drm-syncobj |
| 原生缓冲 | AHB 只用于 P11 T0 的 BLOB 缓冲存储 | 无原生 AHB 入口；但 SC 池自己分配并上屏 RGBA AHB（`GPU_COLOR_OUTPUT\|GPU_SAMPLED_IMAGE\|COMPOSER_OVERLAY`），消费端能力已有 |
| 并发 | 每 supervisor 一个会话（`Refuse{Busy}`）；`pGLContext`、Espryt `g_Display/g_Context/g_Surface`、Magma `pVulkanRenderer` 全局；多 context 未做（DEBTS） | 客户端不限；每窗 ≤16 层；队列 8；SC 模式为默认（`sc_enabled=1`） |
| 部署 | Linux client 只验过 x86_64；无 aarch64 glibc；只有 `libEGL.so(.1)` 别名，无 glvnd | 模块启动，`awl_daemon` 域 permissive；容器进程在 `droidspacesd` 域 |

## MobileGL 要做的

### 两条路线共用

| # | 项 | 内容 | 量级 |
|---|---|---|---|
| C1 | aarch64 glibc 构建与加载 | 交叉工具链 + CI + 与 Android server 的 wireFingerprint 验收；建议做 glvnd vendor（`__egl_Main` + egl_vendor.d JSON），容器 Mesa 就是 glvnd | M |
| C2 | 多 client / 多窗口 / 多 context | 先 worker-per-client（离屏 fork-per-session 有底子，去掉 Busy、做路由），再 per-surface 状态，再真 share group；单窗口游戏可暂缓，GTK / Qt / 浏览器离不开 | L |
| C3 | 部署 | `libMobileGLServer.so --serve unix:/data/local/tmp/awl/mobilegl.sock` 作 root 守护进程由模块拉起、独立 SELinux 域，不走 APK；GLES 有 waylandbridge 先例，Vulkan 需实测 | S |
| C4 | 桌面程序会撞的空壳 | EGL sync / image 空壳；无 `EGL_KHR_surfaceless_context`；GLES context 名不副实（`GL_VERSION` 报桌面 4.6）；无固定管线；device lost 时 swap 不返回 `EGL_CONTEXT_LOST`（与 design/08 不符） | 按需 |
| C5 | X11 | GLX 外壳 + MIT-SHM 回读上屏（通用、慢）；DRI3 需要 Xwayland glamor（GBM + dma-buf 导入导出）→ 建议 Xwayland 继续用 Mesa / 软件 | 后置 |

### 仅方案 A

| # | 项 | 内容 | 量级 |
|---|---|---|---|
| A1 | 帧回交 | server 每个窗口 surface 约 4 个 AHB 槽（RGBA8888）；Espryt 用 EGLImage 当默认帧缓冲，Magma 用外部内存 VkImage 代替 swapchain；Present 带 surface id / 槽号 | L |
| A2 | 真 fence | Espryt `EGL_ANDROID_native_fence_sync`、Magma `VK_KHR_external_semaphore_fd`（SYNC_FD）导出完成 fence；导入 release fence 后才复用槽 | M |
| A3 | 协议 | 新窗口类型 + 缓冲池注册 / 帧就绪（带 fence fd）/ 缓冲释放；现有 fd 传递单条 1 fd、sideband 256 B、client 只在握手收 → 要常驻接收；TCP 不传 fd，只能走 unix / `fd:` 端点 | M |
| A4 | client 端 Wayland | `EGL_PLATFORM_WAYLAND_KHR` / `wl_egl_window`；attach / damage / commit、frame callback 定节奏、configure / resize、viewporter 与分数缩放（缓冲像素 ≠ 逻辑尺寸）；Anland 隐藏窗口会扣住 callback，不能因此 `PresentCreditTimeout` 丢会话。`wl_shm` 兜底也要这一项 | M |

### 仅方案 B

| # | 项 | 内容 | 量级 |
|---|---|---|---|
| B1 | Surface 接入 | server 经 binder 从 Anland 接窗口 Surface（`ANativeWindow_readFromParcel`，API 34+，与 waylandbridge 同一要求），交给现有 swapchain 路径（Espryt `eglCreateWindowSurface`、Magma `vkCreateAndroidSurfaceKHR`） | S |
| B2 | 推广 `ServerDisplay` | P12 的租约从"进程唯一的自有窗口"推广为"每个 surface 由 Anland 按令牌提供的窗口"；尺寸由 Anland 事件驱动 | M |
| B3 | 失窗非致命 | Anland 分离窗口 → Surface 销毁 → 释放后端 surface，client 的 `eglSwapBuffers` 阻塞或丢帧直到重新挂上；今天 P12 是 `Fatal{ServerWindowLost}` 闩会话 | M |
| B4 | client 端 | `wl_egl_window` 只用来拿 `wl_surface` 与尺寸：从 Anland 扩展取令牌交给 server、转发 resize，不提交 `wl_buffer`；节奏靠 server 端 swapchain 的 FIFO | S |

## Anland 要改的

### 方案 A

| 档 | 改动 | 得到 |
|---|---|---|
| 不改 | server 回读进 `wl_shm`（写进 client 的 shm pool） | 所有 GPU 可用；每帧回读 + 上传（1080p ≈ 8 MB）；正确性基线，也是 B 覆盖不到时的兜底 |
| 不改（dma-buf） | server 专门分配线性 BGRA，client 解析 AHB 线格式取 fd[0] 走 `linux-dmabuf` | 只有高通，脆弱，不推荐 |
| 原生 AHB 入口 | ① 给 render server 的本机注册通道（`AHardwareBuffer_recvHandleFromUnixSocket` 或新 binder 事务），返回绑定 client uid 的令牌 ② 新 Wayland 全局把令牌包成 `wl_buffer` ③ `awl_buffer` / `awl_bq_buffer` 改带类型标签（现把 `dmabuf_fd<0` 当空） ④ `awl_ahb_cache_get` 原生缓冲跳过伪造 ⑤ `awl_ahb_hwc_scanout_ok` 不作用于原生缓冲 ⑥ 原生缓冲只走显式 release ⑦ GL / SC 路径正确处理 RGBA | 不依赖高通、Mali 可用；保留 UBWC 与 HWC 直出；glibc client 不碰 gralloc |

### 方案 B

| 改动 | 说明 |
|---|---|
| Wayland 扩展 | 把某个 `wl_surface` 标成外部渲染 → 返回令牌；允许不提交 `wl_buffer` 也能映射；frame callback 仍按 vsync 发 |
| Surface 交接 | 把该窗口的 Surface 与挂上 / 分离 / 尺寸事件交给 MobileGL server（守护进程 `ANativeWindow_writeToParcel` 转发，或 server 调 `anland.host` 新事务在回包里取）；分离前先通知收回 |
| SC 结构 | 该窗口强制 SC 模式；不给根 surface 建子层；popup / 子 surface / 光标照常作为子层叠在上面 |
| 鉴权 | 只把 Surface 交给与该 `wl_surface` 同一 client uid 的会话 |

局限：只覆盖 toplevel 的根 surface（子 surface 上的 GL 如视频播放器走 A）；GL 画面与其他图层不是原子提交；父层之下的子 surface 看不见；EGL 模式不可用；Anland 基于缓冲的机制（扣缓冲停车、SC 直出 / 拷贝切换）不作用于这个窗口。

### 共用

| 档 | 改动 | 得到 |
|---|---|---|
| 顺手修 | `DRM_FORMAT_MOD_INVALID` 写成 `0x00ffffff00000000`（UAPI 为 `0x00ffffffffffffff`）；offset 被忽略、modifier 不校验 | 修 bug；只往格式表加 AB24 不够（伪造写死 BGRA） |
| 部署 | `service.sh` 拉起 MobileGL server；sepolicy 新域：`droidspacesd` connectto、双向 fd use、SurfaceFlinger / HWC / system_server 对新域 fd 的 use；B 另需 binder 互调规则 | 必需 |
| 可选 | `linux-dmabuf` v4 feedback（main_device）、`wp_presentation` | Xwayland / 帧时统计 |

A 的原生 AHB 入口与 B 的扩展都要 Anland 作者合入，先给他们接口草案；B 贴合 Anland 现有的 SC 结构，但是 Anland 专用，不是标准协议。

## 分期

1. **M0**（Anland 不改）：C1 + A4 + `wl_shm` 回读，单应用单窗口闭环；验节奏、resize、隐藏窗口。之后作兜底。
2. **M1**（方案 B）：Anland 扩展 + B1–B4；一个 GL 顶层窗口的应用零拷贝。
3. **M2**：C2 多 client / 多窗口 / 多 context，GLES context。
4. **M3**（按需）：A1–A3 + Anland 原生 AHB 入口，覆盖子 surface 上的 GL 与 EGL 模式。
5. **M4**：C5 X11 走 MIT-SHM；glamor / GBM 视需求。

## 只能上设备回答的

- root 守护进程能否加载厂商 Vulkan（GLES 有 waylandbridge 先例）。
- enforcing 下完整的 SELinux 规则。
- B：外部进程连上 SurfaceView 父 Surface 当生产者，与 Anland 的子 SurfaceControl 并存是否正常；resize 时缓冲尺寸与层几何短暂不一致的表现。
- A：各家 gralloc 的 RGBA / UBWC 缓冲能否被 SF 直接上屏；Mali 上回读路径的性能。

## 旁支

- Termux 原生（bionic）程序可以进程内直接加载 MobileGL（monolith，厂商驱动），不需要 split；帧交接同样是 A（A1、A2、A4）或 B（Surface 交给该进程）二选一。但 Termux 线目前基于 Anland 5.x（lfdevs 维护）。
- 这项工作不在 [`../../ROADMAP.md`](../../ROADMAP.md) 里，要做是一个新阶段。
