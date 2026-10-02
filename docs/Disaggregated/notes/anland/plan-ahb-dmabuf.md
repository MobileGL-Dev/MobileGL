# 计划：AHB 当 dma-buf，server 当分配器（窗口零拷贝）（2026-10-02）

> 接 [`handoff-legacy-mobilegl-unified.md`](handoff-legacy-mobilegl-unified.md)。调研 + 设备探针，未动代码。硬约束：**厂商中立**——只依赖 Android 公开设施（AHardwareBuffer、EGL_ANDROID_*、VK_ANDROID_external_memory_android_hardware_buffer、sync_file）和通用 Linux 设施（dma-buf、SCM_RIGHTS、libgbm 后端 ABI、linux-dmabuf 协议），不碰 kgsl / gralloc 私有布局 / 厂商修饰符。

## 现状：每个窗口每帧 GPU→CPU→GPU

KWin 自己（`MOBILEGL_IPC_SURFACE=server`）直接画在 Android Surface 上，没问题。其余所有 Wayland 客户端：

1. **生产端**（`EGLImpl.cpp:210` → `WaylandWindow.cpp:398`）：server 渲染进 pbuffer；`eglSwapBuffers` 时客户端 `ReadPixels` 整帧，按 ~2 MiB 回复槽切带，**每带一次阻塞往返**；server `glReadPixels`/`vkCmdCopyImageToBuffer`+等待+重排 → 回复槽 memcpy → 客户端 memcpy → BGRA 换序+翻转写进 `wl_shm`。CPU 整帧遍历：Espryt 4 次，Magma ~6 次。
2. **消费端**（KWin 也是 MobileGL 客户端）：`glTexSubImage2D` 上传 wl_shm → 解包 malloc+拷贝 → 层影子拷贝 → **整层**（不是脏区）进 SEG_STAGE → server `AdoptRun` 再拷贝 → 驱动上传。又 4 次。
3. linux-dmabuf 根本没开：KWin 6.7 的 `EglBackend::initWayland` 要求 `drmDevice()`，anland 后端在 MobileGL 模式下给空设备，`kwin.patch` 让它直接退回 wl_shm（还连带了 screencast 的空指针崩溃补丁）。

2560×1412 的窗口一帧 14.5 MB，上面合计 ~8–10 次整帧 CPU 遍历 + 若干阻塞往返。

## 目标形状

```
client(GL) ──渲染──▶ [AHB] ◀──采样── KWin(GL)        全在 server 进程里：同一块 AHB，两个会话各自导入
      │                ▲  dma-buf fd                    CPU 拷贝 0 次；fd 每块缓冲只传一次
      └─ wl_buffer(linux-dmabuf, fd) ─▶ KWin ─ eglCreateImage(DMA_BUF, fd) ─▶ server 按身份认回 AHB
```

关键点：**生产者和消费者都是同一个 server 的会话**。所以"导入 dma-buf"不需要驱动支持 `EGL_EXT_image_dma_buf_import`（Adreno 的 Android EGL 就没有）——server 只需认出"这是我发出去的那块 AHB"，再走 Android 标准导入：Espryt 用 `eglGetNativeClientBufferANDROID` + `EGL_NATIVE_BUFFER_ANDROID`，Magma 用 `VK_ANDROID_external_memory_android_hardware_buffer`。两者都是 Android 规范要求的，厂商中立。

## 设备探针（HA27Q3LQ，内核 6.1，Android 15）

- AHB（RGBA8，GPU 用途，有/无 CPU 用途）的 native_handle = 2 个 fd，都是 `/dmabuf:` 文件（图像 + 32 KB 元数据）。
- 从 Android 侧（`shell` 域）经 SCM_RIGHTS 发给容器进程（`droidspacesd` 域）：收到、`mmap`、像素逐点正确；两边 `st_ino` 相同 → **按 inode 认身份可行**。
- 容器里有 `/dev/dri/renderD128`（显示驱动 msm_drm，组 `droidspaces-gpu`），Mesa 26.3 的 libgbm 会按 `GBM_BACKEND` 或驱动名加载 `/usr/lib/gbm/<name>_gbm.so`（ABI v1，`gbm_backend_abi.h` 已装）。**但这个节点是厂商的，别的机器未必有**——见下面"薄节点"。
- KWin 6.7：`RenderDevice::open(path)` = `DrmDevice`(open+fstat+`gbm_create_device`) → `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm)`；`initWayland` 的 tranche 设备号取 `renderDevNode()` 否则 `drmDevice->deviceId()`；dma-buf 导入走 `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)`，格式表走 `eglQueryDmaBufFormats/ModifiersEXT`。

## 厂商中立的几条规则

1. **只分配 AHB**，只用公开 NDK 拿 fd：`AHardwareBuffer_sendHandleToUnixSocket` 发到 server 自己的 socketpair，`recvmsg` 取 SCM_RIGHTS 里的 fd（不依赖 LL-NDK 的 `getNativeHandle`，也不解析 handle 里的 int）。
2. **不假设哪个 fd 是图像**：发 fd[0] 作 plane 0，身份登记 handle 里的所有 fd。
3. **不假设布局**：默认通告 `DRM_FORMAT_MOD_INVALID`（"驱动定义布局"，正好就是 AHB 的语义）。`LINEAR` 只在启动自检通过时才通告：分配带 CPU 用途的 AHB，`lock` 写图案，`mmap` fd[0] 按 `describe().stride` 读回比对。自检失败就不通告，CPU 映射走 server（`AHardwareBuffer_lock` + 拷贝）。
4. **身份**：首选 `st_ino`（启动时分配两块确认 inode 互异——老内核所有 dma-buf 共用一个 anon inode）；不唯一时退到 `DMA_BUF_SET_NAME` 打 `mgl:<id>` 标签、读 fdinfo 的 `name:`。非本 server 分配的 dma-buf → `EGL_BAD_MATCH`（KWin 会回退 shm）。
5. **同步只用 sync_file**：`EGL_ANDROID_native_fence_sync`、`VK_KHR_external_semaphore_fd`(SYNC_FD)、必要时 `DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE`（通用 Linux ≥5.20/6.0）。不用 DRM syncobj（要真 DRM 驱动）。
6. **薄节点**：GBM 后端用 `GBM_BACKEND=mobilegl` 选中（`/usr/lib/gbm/mobilegl_gbm.so`），不装成 `msm_drm_gbm.so`。它对设备 fd 不发任何 ioctl，只当"打开了某个设备"的令牌；真正的通道是到 server 的连接。节点路径由 anland 配置（`MOBILEGL_GBM_NODE`）：有可访问的 DRM 节点就借来当身份（只 open/fstat），没有就用占位节点。MobileGL 现有的假 EGL device 里写死的 `/dev/dri/renderD128` 默认值同样改成跟这个配置走。

## 归属：这事主要是 MobileGL 的

| 部分 | 在哪 | 内容 |
|---|---|---|
| 分配器 + 身份登记 | **MobileGL server**（Android 侧） | AHB 分配、fd 导出、inode 登记、会话生命周期/引用计数、隐式同步（server 看得见两端，就当"内核"） |
| 后端导入 | **MobileGL Espryt / Magma** | AHB → EGLImage → 纹理/渲染缓冲；AHB → VkImage（每会话各自 VkDevice，各导一次） |
| 生产端 WSI | **MobileGL 客户端 EGL**（glibc） | `wl_egl_window` 改为 AHB 交换链 + `zwp_linux_dmabuf_v1`，没有该 global 时退回现在的 wl_shm |
| 消费端 EGL | **MobileGL 客户端 EGL** | `EGL_EXT_image_dma_buf_import(_modifiers)`、`eglQueryDmaBuf*`、真 `eglCreateImage`（现在是空壳）、`glEGLImageTargetTexture2DOES`/`RenderbufferStorageOES` |
| GBM 后端 | **MobileGL 新组件** `mobilegl_gbm.so`（glibc） | gbm ABI v1：`bo_create*` → server 分配，`bo_import`，`bo_map`，格式查询；外加 EGL 的真 GBM 平台（现在 GBM 窗口被拒） |
| 合成器 | anland | 后端打开薄节点建 `RenderDevice`；撤掉 `initWayland` 跳过 dma-buf 与 screencast 两处补丁；启动脚本设 `GBM_BACKEND`/`MOBILEGL_GBM_NODE` |

ANativeWindow 只剩 KWin 输出那一块，不变。

## 分阶段

**P0 去风险（小）**
- server 真实所在域 `untrusted_app` → 容器 `droidspacesd` 传 dma-buf fd 过 SELinux（ashmem 段已经能过，dma-buf 类别要实测；看 `avc: denied`）。
- 确认 libgbm 在 `GBM_BACKEND` 指定时不对 fd 发 ioctl（读 Mesa 26.3 `backend.c` 或拿 `/dev/null` 实测）。
- 确认 KWin 拿非 DRM 的设备号（占位节点）时 tranche/feedback 不出事。
- Magma 跨 VkDevice 同一 AHB 两次导入可用；Espryt 同一 AHB 两个 context 的 EGLImage。

**P1 server 分配器 + 登记表**
- `SharedImageRegistry`：`Allocate(w,h,format,usage) → {id, fds, stride, modifier}`；按 inode（或 name 标签）查找；归属会话 + 引用计数；会话死掉释放（键用单调 id，不用客户端句柄——P14 的跨会话句柄冲突教训）。
- 线协议新动词：分配 / 释放 / 按 fd 导入（fd 走现成的 `ITransport::ShareFd`）。
- 单元测试：注册、身份、跨会话释放只放自己的。

**P2 生产端：wl_egl_window 走 AHB 交换链**
- 先做最稳的一步：server 照旧渲染进 pbuffer/目标，`eglSwapBuffers` 时**GPU blit**（带 Y 翻转）到当前空闲 AHB——CPU 零拷贝，只多一次 GPU 拷贝。之后再让默认帧缓冲直接落在 AHB 上（Magma 已有默认 FB 变换机制；Espryt 用 FBO 冒充默认 FB）。
- 客户端：绑定 `zwp_linux_dmabuf_v1`，每块 AHB 建一次 `wl_buffer`（`create_immed`，`ABGR8888`/`XBGR8888` = GL RGBA8 内存序，不再换序）；按 `wl_buffer.release` 轮转 3 块；resize 重建。没 global → 现路径。
- 同步第一版：swap 返回前 server CPU 等自己的渲染完成（现在 readback 本来就全等，不退步）；server 记录每块的"最后消费者 fence"，生产者重用前等它。

**P3 消费端：KWin 导入**
- 客户端 EGL 通告上述扩展；`eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` 把 fd 发给 server → 认身份 → 后端导入；`glEGLImageTargetTexture2DOES` 绑到 KWin 的纹理。
- anland：后端建真 `DrmDevice`（薄节点 + `mobilegl_gbm.so`），撤补丁，linux-dmabuf 起来。这一步和 P2 合起来就是第一个可见成果：plasmashell/Qt/GTK/glmark2-wayland 零拷贝。

**P4 GBM 后端**
- `mobilegl_gbm.so` + EGL 的 GBM 平台（`eglGetPlatformDisplay(GBM)`、`eglCreateImage` 从 gbm bo）。KWin 的 `GbmGraphicsBufferAllocator` 随之可用 → screencast 走 dma-buf，screencast 补丁可撤。
- 格式：ARGB8888 等 AHB 没有的通道序，按 RGBA8 分配，server 记"声明格式"，导入时用纹理 swizzle 对齐语义（两端都是 MobileGL，server 都看得见）。

**P5 同步与性能**
- 去掉 swap 时的 CPU 等待：sync_file 在 server 内做 GPU 等（Espryt `EGL_ANDROID_native_fence_sync` + `eglWaitSyncKHR`；Magma sync-fd 信号量跨会话设备）。
- 可选 `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` 给外部 CPU 映射者隐式同步。
- `EGL_EXT_buffer_age` + 真实损伤区，Qt 局部重绘。

**P6 其它消费者（按需）**
- Xwayland glamor 跑在 MobileGL 的 GBM 平台上（现在强制 llvmpipe + `-shm`）。
- Chrome 原生 GBM 路径（它会 `drmGetDeviceFromDevId` 枚举节点——占位节点下可能不行，是风险项；现在的 wl_egl_window 路径在 P2 后已经零拷贝）。

## 风险

- SELinux：dma-buf fd 从 `untrusted_app` 进 `droidspacesd`（P0 第一项）。
- 方向：GL 默认帧缓冲自下而上、`wl_buffer` 自上而下。P2 的 blit 顺手翻转；直接渲染时可选用 `wl_surface.set_buffer_transform(FLIPPED_180)`（竖直翻转，KWin 着色器里免费），需实测 KWin 处理正确。
- 每会话一个 VkDevice（Magma）：同一 AHB 在两个设备上，同步只能靠 sync_file 或 CPU 等。
- 生命周期：客户端崩溃时 KWin 仍持有它的 `wl_buffer`/EGLImage——AHB 引用要由导入者也持有，不能随生产会话一起释放。
- 资源：每窗口 3 块 AHB（2560×1412 约 44 MB），比现在的 pbuffer + 3 块 shm 少不了多少，但不再需要回复槽带宽。
