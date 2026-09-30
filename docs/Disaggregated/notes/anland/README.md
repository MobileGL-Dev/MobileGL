# MobileGL 作为 Anland 的 GL 提供者：调研（2026-09-30）

> 非阶段笔记。只读源码调研，**没做原型、没上设备**。Anland 审计于 `SuperTurtleDev/anland@cc169180`（6.x main），MobileGL 审计于 `p8/main@648800a2`（P8 收官头）。结论里凡是"要做 / 要改"的都是推断，事实部分在两份报告里逐行有出处。

| 文件 | 内容 |
|---|---|
| [`report-anland.md`](report-anland.md) | Anland 侧：缓冲入口契约、伪造 AHB、同步、帧节奏、进程与 SELinux、容器 GPU 现状、Xwayland glamor、衍生项目、改动插入点（382 处引用） |
| [`report-mobilegl.md`](report-mobilegl.md) | MobileGL 侧：Linux client 构建与导出、EGL / GLX 能力、split 的 present 路径、AHB / fd / fence 现状、会话与多窗口限制、GL 覆盖面、文档漂移 |
| [`brief-anland.md`](brief-anland.md)、[`brief-mobilegl.md`](brief-mobilegl.md) | 派给 Codex 的原始提示词。**两份都把后端名写反了**：Espryt = DirectGLES（GLES），Magma = DirectVulkan（Vulkan）；报告已纠正 |

## 结论

可行，但两边都要新东西：

- MobileGL 没有"把帧交回 client"这条路：`MGPPresent` 只有 `FrameSerial`，P12 画在 server 自己的 SurfaceView 上；没有 Wayland WSI、没有可导出的颜色图像、没有 sync_file。
- Anland 的 GPU 缓冲入口只有 `linux-dmabuf` v3 的单平面线性 AR24/XR24，靠高通 snapalloc 布局伪造 AHB（非高通直接失败，没有 CPU 退路）；没有原生 AHB 入口。

## Anland 是什么

root Android（KernelSU / SukiSU 模块）上的 Wayland 宿主：守护进程 `waylandbridge`（root，`awl_daemon` 域，bionic，链 Android EGL / GLESv3）实现 Wayland 协议，每个 `xdg_toplevel` 变成一个独立 Android 窗口；Droidspaces 容器经 `/run/anland/wayland-0`（绑定自 `/data/local/tmp/awl`）连入。容器内 GPU 加速 = Mesa freedreno / turnip on kgsl，只有 Adreno；Mali 等走 llvmpipe。

## 推荐形态

```
容器应用 ─ MobileGL client（glibc libEGL/libGL）
   │ 同机 unix socket + 共享内存（split 现成）
Android 侧 MobileGL render server（root 守护进程，厂商驱动：Espryt=GLES / Magma=Vulkan）
   │ 渲染进 AHB，完整句柄经本机 socket 交给 waylandbridge
waylandbridge ─ SurfaceControl / GLES 合成 ─ SurfaceFlinger
```

- Wayland 连接与窗口仍归 Linux 应用；MobileGL 只管帧。
- P12 的 server 自有全屏窗口不适用（Anland 要每窗一个 Android 窗口）。
- 容器内直接让 Magma 跑在 turnip 上：只有 Adreno，和现有 Mesa 同一驱动，价值小；MobileGL 的价值是把厂商驱动（含 Mali）带进来，只能靠 split 跨到 bionic。

## 两边现状

| | MobileGL | Anland |
|---|---|---|
| 帧入口 / 出口 | `MGPPresent{FrameSerial}`；P12 画在 server 窗口 | `wl_shm` v2（ARGB / XRGB）；`linux-dmabuf` v3 单平面线性 AR24/XR24 |
| 窗口系统 | 无 Wayland；EGL 扩展只有 `EGL_KHR_create_context`、`EGL_MESA_platform_surfaceless` | dma-buf → 高通 snapalloc donor 伪造 AHB（写死 HAL BGRA=5，要求 2 fd 句柄） |
| 同步 | 无 sync_file / SYNC_FD；`eglCreateSync` 创建即 signaled，`eglCreateImage` 只登记 | explicit sync v2（sync_file），acquire / release 都有；无 drm-syncobj |
| 原生缓冲 | AHB 只用于 P11 T0 的 BLOB 缓冲存储 | 无原生 AHB 入口；但 SC 池自己分配并上屏 RGBA AHB（`GPU_COLOR_OUTPUT\|GPU_SAMPLED_IMAGE\|COMPOSER_OVERLAY`），消费端能力已有 |
| 并发 | 每 supervisor 一个会话（`Refuse{Busy}`）；`pGLContext`、Espryt `g_Display/g_Context/g_Surface`、Magma `pVulkanRenderer` 全局；多 context 未做（DEBTS） | 客户端不限；每窗 ≤16 层；队列 8 |
| 部署 | Linux client 只验过 x86_64；无 aarch64 glibc；只有 `libEGL.so(.1)` 别名，无 glvnd | 模块启动，`awl_daemon` 域 permissive；容器进程在 `droidspacesd` 域 |

## MobileGL 要做的

| # | 项 | 内容 | 量级 |
|---|---|---|---|
| 1 | 帧回交 | server 每个窗口 surface 约 4 个 AHB 槽（RGBA8888）；Espryt 用 EGLImage 当默认帧缓冲，Magma 用外部内存 VkImage 代替 swapchain；Present 带 surface id / 槽号 | L |
| 2 | 真 fence | Espryt `EGL_ANDROID_native_fence_sync`、Magma `VK_KHR_external_semaphore_fd`（SYNC_FD）导出完成 fence；导入 release fence 后才复用槽 | M |
| 3 | 协议 | 新窗口类型 + 缓冲池注册 / 帧就绪（带 fence fd）/ 缓冲释放；现有 fd 传递单条 1 fd、sideband 256 B、client 只在握手收 → 要常驻接收；TCP 不传 fd，只能走 unix / `fd:` 端点 | M |
| 4 | client 端 Wayland | `EGL_PLATFORM_WAYLAND_KHR` / `wl_egl_window`；attach / damage / commit、frame callback 定节奏、configure / resize、viewporter 与分数缩放（缓冲像素 ≠ 逻辑尺寸）；Anland 隐藏窗口会扣住 callback，不能因此 `PresentCreditTimeout` 丢会话 | M |
| 5 | aarch64 glibc 构建与加载 | 交叉工具链 + CI + 与 Android server 的 wireFingerprint 验收；建议做 glvnd vendor（`__egl_Main` + egl_vendor.d JSON），容器 Mesa 就是 glvnd | M |
| 6 | 多 client / 多窗口 / 多 context | 先 worker-per-client（离屏 fork-per-session 有底子，去掉 Busy、做路由），再 per-surface 池，再真 share group；单窗口游戏可暂缓，GTK / Qt / 浏览器离不开 | L |
| 7 | 部署 | `libMobileGLServer.so --serve unix:/data/local/tmp/awl/mobilegl.sock` 作 root 守护进程由模块拉起、独立 SELinux 域，不走 APK；GLES 有 waylandbridge 先例，Vulkan 需实测 | S |
| 8 | 桌面程序会撞的空壳 | EGL sync / image 空壳；无 `EGL_KHR_surfaceless_context`；GLES context 名不副实（`GL_VERSION` 报桌面 4.6）；无固定管线；device lost 时 swap 不返回 `EGL_CONTEXT_LOST`（与 design/08 不符） | 按需 |
| 9 | X11 | GLX 外壳 + MIT-SHM 回读上屏（通用、慢）；DRI3 需要 Xwayland glamor（GBM + dma-buf 导入导出）→ 建议 Xwayland 继续用 Mesa / 软件 | 后置 |

## Anland 要改的

| 档 | 改动 | 得到 |
|---|---|---|
| 不改 | server 回读进 `wl_shm`（写进 client 的 shm pool） | 所有 GPU 可用；每帧回读 + 上传（1080p ≈ 8 MB）；正确性基线 |
| 不改（dma-buf） | server 专门分配线性 BGRA，client 解析 AHB 线格式取 fd[0] 走 `linux-dmabuf` | 只有高通，脆弱，不推荐 |
| 顺手修 | `DRM_FORMAT_MOD_INVALID` 写成 `0x00ffffff00000000`（UAPI 为 `0x00ffffffffffffff`）；offset 被忽略、modifier 不校验 | 修 bug；只往格式表加 AB24 不够（伪造写死 BGRA） |
| 推荐 | ① 给 render server 的本机注册通道（`AHardwareBuffer_recvHandleFromUnixSocket` 或新 binder 事务），返回绑定 client uid 的令牌 ② 新 Wayland 全局把令牌包成 `wl_buffer` ③ `awl_buffer` / `awl_bq_buffer` 改带类型标签（现把 `dmabuf_fd<0` 当空） ④ `awl_ahb_cache_get` 原生缓冲跳过伪造 ⑤ `awl_ahb_hwc_scanout_ok` 不作用于原生缓冲 ⑥ 原生缓冲只走显式 release ⑦ GL / SC 路径正确处理 RGBA | 不依赖高通、Mali 可用；保留 UBWC 与 HWC 直出；glibc client 不碰 gralloc |
| 部署 | `service.sh` 拉起 MobileGL server；sepolicy 新域：`droidspacesd` connectto、双向 fd use、SurfaceFlinger / HWC / system_server 对新域 fd 的 use | 必需 |
| 可选 | `linux-dmabuf` v4 feedback（main_device）、`wp_presentation` | Xwayland / 帧时统计 |

"推荐"档要 Anland 作者合入，先给他们接口草案。

## 分期

1. **M0**（Anland 不改）：aarch64 client + Wayland WSI + `wl_shm` 回读，单应用单窗口闭环；验节奏、resize、隐藏窗口。
2. **M1**：真 fence + Anland 原生 AHB 入口，零拷贝。
3. **M2**：多 client / 多窗口 / 多 context，GLES context。
4. **M3**：X11 走 MIT-SHM；glamor / GBM 视需求。

## 只能上设备回答的

- root 守护进程能否加载厂商 Vulkan（GLES 有 waylandbridge 先例）。
- enforcing 下完整的 SELinux 规则。
- 各家 gralloc 的 RGBA / UBWC 缓冲能否被 SF 直接上屏。
- Mali 上回读路径的性能。

## 旁支

- Termux 原生（bionic）程序可以进程内直接加载 MobileGL（monolith，厂商驱动），不需要 split，只要上表 1、2、4；但 Termux 线目前基于 Anland 5.x（lfdevs 维护）。
- 这项工作不在 [`../../ROADMAP.md`](../../ROADMAP.md) 里，要做是一个新阶段。
