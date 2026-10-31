# Plan: AHB as a dma-buf, the server as allocator (zero-copy windows) (2026-10-02)

> Follows [`handoff-legacy-mobilegl-unified.md`](handoff-legacy-mobilegl-unified.md). Research and device probes, no code changed. Hard constraint: **vendor-neutral** — depend only on public Android facilities (AHardwareBuffer, EGL_ANDROID_*, VK_ANDROID_external_memory_android_hardware_buffer, sync_file) and generic Linux facilities (dma-buf, SCM_RIGHTS, the libgbm backend ABI, the linux-dmabuf protocol); never touch kgsl, gralloc private layouts or vendor modifiers.

## Today: GPU→CPU→GPU per window per frame

KWin itself (`MOBILEGL_IPC_SURFACE=server`) draws straight onto the Android Surface, which is fine. Every other Wayland client:

1. **Producer side** (`EGLImpl.cpp:210` → `WaylandWindow.cpp:398`): the server renders into a pbuffer; on `eglSwapBuffers` the client `ReadPixels` the whole frame and cuts it into ~2 MiB reply slots, **one blocking round trip per slice**; the server `glReadPixels`/`vkCmdCopyImageToBuffer` + wait + reorder → copy into the reply slot → client memcpy → BGRA swizzle and flip into `wl_shm`. Full-frame CPU passes: 4 on Espryt, ~6 on Magma.
2. **Consumer side** (KWin is also a MobileGL client): `glTexSubImage2D` uploads wl_shm → unpack malloc and copy → layer shadow copy → the **whole layer** (not the dirty region) into SEG_STAGE → the server's `AdoptRun` copies again → driver upload. 4 more passes.
3. linux-dmabuf is not enabled at all: KWin 6.7's `EglBackend::initWayland` requires `drmDevice()`, the anland backend provides an empty device in MobileGL mode, and `kwin.patch` makes it fall back to wl_shm (together with a null-pointer crash fix for screencast).

A 2560×1412 window is 14.5 MB per frame; the above adds up to ~8–10 full-frame CPU passes plus several blocking round trips.

## Target shape

```
client(GL) ──render──▶ [AHB] ◀──sample── KWin(GL)        all in the server process: one AHB, imported by two sessions
      │                ▲  dma-buf fd                    zero CPU copies; the fd crosses once per buffer
      └─ wl_buffer(linux-dmabuf, fd) ─▶ KWin ─ eglCreateImage(DMA_BUF, fd) ─▶ server recognizes the AHB by identity
```

Key point: **producer and consumer are sessions of the same server**. So importing the dma-buf needs no driver support for `EGL_EXT_image_dma_buf_import` (Adreno's Android EGL does not have it) — the server only has to recognize "this is the AHB I handed out" and then use the standard Android import: Espryt uses `eglGetNativeClientBufferANDROID` + `EGL_NATIVE_BUFFER_ANDROID`, Magma uses `VK_ANDROID_external_memory_android_hardware_buffer`. Both are required by the Android specification and vendor-neutral.

## Device probe (HA27Q3LQ, kernel 6.1, Android 15)

- An AHB (RGBA8, GPU usage, with and without CPU usage) has a native_handle of 2 fds, both `/dmabuf:` files (the image and 32 KB of metadata).
- Sent from the Android side (`shell` domain) to the container process (`droidspacesd` domain) over SCM_RIGHTS: received, `mmap`ed, pixels correct point by point; both sides report the same `st_ino` → **identity by inode works**.
- The container has `/dev/dri/renderD128` (display driver msm_drm, group `droidspaces-gpu`); Mesa 26.3's libgbm loads `/usr/lib/gbm/<name>_gbm.so` from `GBM_BACKEND` or the driver name (ABI v1, `gbm_backend_abi.h` installed). **But this node is vendor-specific and other machines may not have it** — see "thin node" below.
- KWin 6.7: `RenderDevice::open(path)` = `DrmDevice`(open + fstat + `gbm_create_device`) → `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm)`; `initWayland` takes the tranche device id from `renderDevNode()`, otherwise `drmDevice->deviceId()`; dma-buf import goes through `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)`, and the format table through `eglQueryDmaBufFormats/ModifiersEXT`.

## Vendor-neutral rules

1. **Allocate only AHBs**, and obtain fds only through the public NDK: `AHardwareBuffer_sendHandleToUnixSocket` to the server's own socketpair, `recvmsg` to take the fd from SCM_RIGHTS (no LL-NDK `getNativeHandle`, no parsing the ints inside the handle).
2. **Do not assume which fd is the image**: send fd[0] as plane 0 and register every fd in the handle for identity.
3. **Do not assume the layout**: advertise `DRM_FORMAT_MOD_INVALID` by default ("driver-defined layout", exactly AHB's semantics). Advertise `LINEAR` only when the startup self-test passes: allocate an AHB with CPU usage, `lock` and write a pattern, `mmap` fd[0] and read it back using `describe().stride` for comparison. If the self-test fails, do not advertise it, and CPU mappings go through the server (`AHardwareBuffer_lock` + copy).
4. **Identity**: prefer `st_ino` (allocate two at startup to confirm the inodes differ — old kernels share one anon inode across all dma-bufs); when not unique, fall back to tagging with `DMA_BUF_SET_NAME` as `mgl:<id>` and reading `name:` from fdinfo. A dma-buf not allocated by this server → `EGL_BAD_MATCH` (KWin falls back to shm).
5. **Synchronize only through sync_file**: `EGL_ANDROID_native_fence_sync`, `VK_KHR_external_semaphore_fd` (SYNC_FD), and when necessary `DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE` (generic Linux ≥5.20/6.0). No DRM syncobj (that requires a real DRM driver).
6. **Thin node**: the GBM backend is selected with `GBM_BACKEND=mobilegl` (`/usr/lib/gbm/mobilegl_gbm.so`), not installed as `msm_drm_gbm.so`. It issues no ioctl on the device fd; the fd is only a token meaning "some device is open", and the real channel is the connection to the server. The node path comes from anland configuration (`MOBILEGL_GBM_NODE`): if an accessible DRM node exists, borrow it as identity (open/fstat only); otherwise use a placeholder node. MobileGL's existing fake EGL device likewise stops hardcoding `/dev/dri/renderD128` and follows this configuration.

## Ownership: mostly MobileGL's job

| Part | Where | Content |
|---|---|---|
| Allocator + identity registry | **MobileGL server** (Android side) | AHB allocation, fd export, inode registry, session lifetime / refcounts, implicit synchronization (the server sees both ends and acts as the "kernel") |
| Backend import | **MobileGL Espryt / Magma** | AHB → EGLImage → texture / renderbuffer; AHB → VkImage (one VkDevice per session, one import each) |
| Producer-side WSI | **MobileGL client EGL** (glibc) | `wl_egl_window` becomes an AHB swapchain + `zwp_linux_dmabuf_v1`, falling back to the current wl_shm when that global is absent |
| Consumer-side EGL | **MobileGL client EGL** | `EGL_EXT_image_dma_buf_import(_modifiers)`, `eglQueryDmaBuf*`, a real `eglCreateImage` (currently a stub), `glEGLImageTargetTexture2DOES`/`RenderbufferStorageOES` |
| GBM backend | **New MobileGL component** `mobilegl_gbm.so` (glibc) | gbm ABI v1: `bo_create*` → server allocation, `bo_import`, `bo_map`, format queries; plus a real GBM platform for EGL (GBM windows are currently rejected) |
| Compositor | anland | The backend opens the thin node and creates a `RenderDevice`; drop the two patches that skip dma-buf in `initWayland` and fix screencast; the startup script sets `GBM_BACKEND`/`MOBILEGL_GBM_NODE` |

ANativeWindow remains only for KWin's output; unchanged.

## Phases

**P0 de-risk (small)**
- Pass a dma-buf fd from the server's real domain `untrusted_app` to the container's `droidspacesd` across SELinux (ashmem segments already pass; the dma-buf class needs measuring; watch `avc: denied`).
- Confirm libgbm does not issue ioctls on the fd when `GBM_BACKEND` is specified (read Mesa 26.3 `backend.c` or measure with `/dev/null`).
- Confirm KWin's tranche/feedback does not break when it gets a non-DRM device id (placeholder node).
- Magma imports the same AHB twice across VkDevices; Espryt creates EGLImages for the same AHB in two contexts.

**P1 server allocator + registry**
- `SharedImageRegistry`: `Allocate(w,h,format,usage) → {id, fds, stride, modifier}`; look up by inode (or name tag); owning session + refcount; release when the session dies (key on a monotonic id, not a client handle — the P14 cross-session handle collision lesson).
- New wire verbs: allocate / release / import by fd (the fd uses the existing `ITransport::ShareFd`).
- Unit tests: registration, identity, and that a cross-session release only frees its own.

**P2 producer side: wl_egl_window over an AHB swapchain**
- Do the safest step first: the server still renders into the pbuffer/target, and on `eglSwapBuffers` **GPU-blits** (with a Y flip) into the currently free AHB — zero CPU copies, one extra GPU copy. Later let the default framebuffer land directly on the AHB (Magma already has a default-FB transform mechanism; Espryt emulates the default FB with an FBO).
- Client: bind `zwp_linux_dmabuf_v1`, create one `wl_buffer` per AHB (`create_immed`, `ABGR8888`/`XBGR8888` = the GL RGBA8 memory order, no more swizzling); rotate three buffers on `wl_buffer.release`; rebuild on resize. Without the global → current path.
- Synchronization, first version: before swap returns, the server CPU-waits for its own rendering (readback already waits fully today, so no regression); the server records each buffer's "last consumer fence" and the producer waits for it before reuse.

**P3 consumer side: KWin import**
- Client EGL advertises the extensions above; `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` sends the fd to the server → identity → backend import; `glEGLImageTargetTexture2DOES` binds it to KWin's texture.
- anland: the backend creates a real `DrmDevice` (thin node + `mobilegl_gbm.so`), drops the patches, and linux-dmabuf comes up. Together with P2 this is the first visible result: zero-copy plasmashell/Qt/GTK/glmark2-wayland.

**P4 GBM backend**
- `mobilegl_gbm.so` + EGL's GBM platform (`eglGetPlatformDisplay(GBM)`, `eglCreateImage` from a gbm bo). KWin's `GbmGraphicsBufferAllocator` then works → screencast over dma-buf, and the screencast patch can be dropped.
- Formats: channel orders AHB does not have, such as ARGB8888, are allocated as RGBA8; the server records the "declared format" and aligns semantics with a texture swizzle on import (both ends are MobileGL and the server sees both).

**P5 synchronization and performance**
- Remove the CPU wait at swap: sync_file lets the server do the GPU wait (Espryt `EGL_ANDROID_native_fence_sync` + `eglWaitSyncKHR`; Magma sync-fd semaphores across session devices).
- Optional `DMA_BUF_IOCTL_IMPORT_SYNC_FILE` for implicit synchronization with external CPU mappers.
- `EGL_EXT_buffer_age` + real damage regions, for Qt partial repaints.

**P6 other consumers (as needed)**
- Xwayland glamor on MobileGL's GBM platform (today it is forced to llvmpipe + `-shm`).
- Chrome's native GBM path (it enumerates nodes with `drmGetDeviceFromDevId` — may not work under a placeholder node, a risk; its current wl_egl_window path is already zero-copy after P2).

## Risks

- SELinux: dma-buf fds from `untrusted_app` into `droidspacesd` (the first P0 item).
- Orientation: the GL default framebuffer is bottom-up, `wl_buffer` is top-down. The P2 blit flips for free; when rendering directly, consider `wl_surface.set_buffer_transform(FLIPPED_180)` (a vertical flip, free in KWin's shader), which needs measuring that KWin handles it correctly.
- One VkDevice per session (Magma): the same AHB on two devices can only synchronize through sync_file or a CPU wait.
- Lifetime: when the client crashes, KWin still holds its `wl_buffer`/EGLImage — the importer must also hold an AHB reference, so it cannot be released with the producer session.
- Resources: three AHBs per window (about 44 MB at 2560×1412), not much less than today's pbuffer + three shm buffers, but the reply-slot bandwidth is gone.
