# Plan: X11 apps on the GPU, zero-copy (Xwayland glamor + GLX DRI3/Present) (2026-10-03)

> Follows [`plan-ahb-dmabuf.md`](plan-ahb-dmabuf.md) (the Wayland half: server-allocated AHB
> "shared images" exported as dma-bufs). Same hard rules: vendor-neutral (AHB, dma-buf, sync_file,
> the libgbm backend ABI, linux-dmabuf, DRI3/Present), no layout assumptions (modifier INVALID).

## Today

```
GLX app --glXSwapBuffers--> ReadPixels (server->client CPU) --xcb_put_image--> Xwayland (-shm, llvmpipe)
Xwayland --wl_shm--> KWin --glTexSubImage2D upload--> server
```
Two full-frame CPU trips per frame for every X11 GL app, and every ordinary X11 app is drawn by
pixman/llvmpipe and uploaded again by KWin.

## Target

```
GLX app  --render--> pbuffer --GPU copy (PresentToSharedImage)--> [AHB image]
         --DRI3 PixmapFromBuffer(s)(dma-buf, once per buffer)--> Xwayland pixmap (glamor: gbm_bo_import -> EGLImage)
         --Present PresentPixmap--> Xwayland flips it (attaches the pixmap's dma-buf wl_buffer) or GPU-copies it
Xwayland (glamor on MobileGL EGL/GBM) --linux-dmabuf wl_buffer (window pixmaps are shared images)--> KWin
```
Every hop is a session of the same MobileGL server sampling or writing the same AHardwareBuffer.
CPU copies: zero.

## Stage 1 - Xwayland on MobileGL (glamor)

What Xwayland 24.1.13 (`hw/xwayland/xwayland-glamor-gbm.c`, `glamor/glamor.c`) needs, and where it is:

| Need | Status |
|---|---|
| render node from linux-dmabuf v4 feedback `main_device` (drmGetDeviceFromDevId) | KWin's thin node: must be a real DRM render node (`MOBILEGL_GBM_NODE`=/dev/dri/renderD128). `/dev/null` makes glamor fail -> shm fallback |
| `gbm_create_device(fd)` with `GBM_BACKEND=mobilegl` | `mobilegl_gbm.so` (exists) |
| EGL client ext `EGL_KHR/MESA_platform_gbm`, `eglGetPlatformDisplay(GBM, gbm)` | exists; the foreign-device decline keys on the backend name `mobilegl` |
| `eglCreateContext(EGL_NO_CONFIG_KHR, core 3.1)` + surfaceless `eglMakeCurrent` | cherry-picked from the surfaceless branch (6125f941), to be dropped on rebase |
| GL >= 2.1 desktop, `GL_ARB_vertex_array_object`, `GL_OES_EGL_image`; renderer not llvmpipe | exists on a split client |
| `EGL_EXT_image_dma_buf_import(_modifiers)` -> dmabuf_capable | exists (shared images) |
| `gbm_bo_create_with_modifiers2(XRGB8888/ARGB8888, {INVALID, LINEAR})`, `gbm_bo_get_fd_for_plane`, `_handle_for_plane`, `_stride_for_plane`, `_offset`, `_modifier`, `gbm_bo_import(FD / FD_MODIFIER)` | exists; now covered by a host test through the real libgbm loader |
| `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` of its own bos, `glEGLImageTargetTexture2DOES` | exists (server identifies its image by inode) |
| KWin's tranche lists XRGB8888/ARGB8888 with modifier INVALID | KWin adds INVALID+LINEAR when the EGL reports no modifiers (ours) |
| **implicit sync**: glamor renders into window pixmaps and commits after `glFlush`, assuming the kernel orders KWin's read after it | **new**: see below |
| **Xwayland must never dial its own display** | **new**: see below |

### Implicit sync by flush (new shared_image op `Flush`)

The server already orders an image between sessions: a writer publishes a sync_file per write
(`PublishWrite`), a reader waits on it at its first use and publishes its frame's read fence at
its frame boundary (swap). glamor has neither a swap nor an explicit write: it renders into the
image through an FBO and calls `glFlush` before committing.

- Client: `glFlush`/`glFinish` in a process that has bound a shared image to a texture
  (`glEGLImageTargetTexture2DOES`) and does not own the server window (not KWin) send the new
  in-stream `shared_image Flush` op and wait for its reply. Policy is a pure function
  (`SharedImageFlushPolicy`), overridable with `MOBILEGL_SHARED_IMAGE_FLUSH_SYNC=0/1`.
- Server: `BackendObject::PublishSharedImageAccesses()` - every image the session used since its
  last boundary gets the flush's fence as a write (generation moves) and as a read.
  Espryt: `ExportFence` (EGL_ANDROID_native_fence_sync), Magma: release held images, submit with
  the exportable semaphore, export it. A session that has flushed this way also waits (GPU) for
  the pending read fences of an image before its first use in a frame (write-after-read).
- The reply comes after the fence is published, so KWin (which samples after Xwayland's commit,
  which follows the reply) always finds the new generation.

### X-connection safety (`MG_Util/X11/DisplayGuard`)

One rule, one function: `MayDialX11Display(name)` is false when this process **serves** that
display: it holds a listening unix socket named `/tmp/.X11-unix/X<n>` (path or abstract) per
`/proc/net/unix` + `/proc/self/fd`, or its executable is an X server (Xwayland, Xorg, Xvfb, ...).
`MOBILEGL_X11_DIAL=0` turns all dialing off. Every place MobileGL opens an X connection asks it
first: EGLState's default-visual query (eglGetDisplay of a non-X native display has platform NONE,
so the old "only on X displays" test still dialed for `eglGetDisplay(gbm_device)`), DirectGLES's
visual query and window-surface display, Magma's Xlib surface. GLX never opens a connection (it
uses the application's). KWin's launcher also drops `DISPLAY` from Xwayland's environment.

### KWin (anland `kwin.patch`, `mobilegl-startup.sh`)

`ANLAND_MOBILEGL=1` and `ANLAND_XWAYLAND_GLAMOR != 0`: Xwayland starts **without** `-shm`, with
`MOBILEGL_IPC_SURFACE=offscreen`, its own `MOBILEGL_LOG_FILE_PATH=/tmp/mgl-xwayland`, `GBM_BACKEND`
kept, no `LIBGL_ALWAYS_SOFTWARE`/`GALLIUM_DRIVER`, no `DISPLAY`. Otherwise the old `-shm` +
software environment. If glamor cannot initialise, Xwayland itself falls back to shm rendering.

## Stage 2 - GLX presentation through DRI3 + Present

Per GLX window drawable (`GLXImpl/X11Present.{h,cpp}`, logic behind two small interfaces so the
host tests drive it with fakes):

- **Path selection** (`SelectPresentPath`): DRI3 >= 1.0 and Present >= 1.0 on the connection,
  shared images available, depth 24/32, `MOBILEGL_GLX_PRESENT` not `readback` -> `Dri3Present`;
  else MIT-SHM >= 1.2 with fd passing -> `ShmPutImage`; else `PutImage` (today's bands).
- **Buffers**: up to 3 (4 when Xwayland holds all three past a 1 s wait) shared images of the
  window's format (depth 24 -> XRGB8888, 32 -> ARGB8888: exactly what glamor imports them as), each
  imported once as a pixmap (`PixmapFromBuffers` with modifier INVALID on DRI3 >= 1.2, else
  `PixmapFromBuffer`, checked). A buffer is busy from its PresentPixmap until its `IdleNotify`.
- **Present**: `PresentToSharedImage` (GPU copy, rows top first, returns once submitted with the
  write fence published), then `PresentPixmap` with an XFixes update region from the swap's
  damage (flipped to X's top-left origin; none = whole), serial per frame. Events arrive on a
  special-event queue (`xcb_register_for_special_xge`), never in the application's queue.
- **Pacing**: `FrameThrottle` (the Wayland rules) on `CompleteNotify`: interval >= 1 waits for the
  previous frame's completion, `target_msc = last_msc + interval`; interval 0 uses
  `PresentOptionAsync`, unpaced while completions come, ~1 fps once one is 200 ms overdue.
- **Errors**: a refused `PixmapFromBuffer`, a `PresentPixmap` error (checked lazily, no round
  trip), or the window's destruction (`ConfigureNotify` with `PresentWindowDestroyed`) latch the
  drawable onto the readback path for good, logged once.
- **Resize**: `ConfigureNotify` (and the existing throttled `XGetGeometry`) resize the pbuffer;
  buffers of the old size are dropped as they come back idle.
- **Fallback** (no DRI3/Present: Xwayland `-shm`, a remote X server): readback into a MIT-SHM
  segment (memfd, `xcb_shm_attach_fd`) + `xcb_shm_put_image`; without MIT-SHM, today's bands.
- `GLX_EXT_buffer_age` (`GLX_BACK_BUFFER_AGE_EXT`) answers the EGL buffer age of the drawable.

## Tests (host, WSL)

`X11DisplayGuardTest` (display names, /proc parsing, the Xwayland case), `X11PresentTest` (path
selection, rotation, idle/complete/configure handling, pacing, resize, error latching, fake xcb
and fake images), `SharedImageSync` additions (accesses publish as write + read),
`SharedImageFlushPolicyTest`, `GbmGlamorTest` (the real libgbm loader with `GBM_BACKEND=mobilegl`
over a fake shared-image library: glamor's create/export/import calls).

## Device verification (phase B)

`scripts/x11/` in the skill: deploy, check Xwayland's glamor log lines and its dma-buf wl_buffers in
KWin (`WAYLAND_DEBUG`-free: `/proc/<kwin>/fdinfo` dma-buf count and the server log's import lines),
glxgears/glxinfo/an X11 GL app before/after (fps, `top` CPU of app/Xwayland/KWin/server), and
ordinary X apps (xterm, xeyes) for glamor rendering.

## Risks

- `drmGetDeviceFromDevId` on the borrowed render node inside the container (sysfs visibility).
- Adreno: cross-context ordering relies on the Flush op's fence; a missed flush shows as tearing,
  not corruption of other windows.
- The extra round trip per Xwayland `glFlush` (glamor flushes once per main-loop wakeup).
- DRI3 clients that are not MobileGL (Mesa Vulkan WSI) hand Xwayland dma-bufs the server never
  allocated: `PixmapFromBuffers` fails with BadAlloc for them, as on any driver mismatch.

## Device results (2026-10-03, TB321FU / Adreno 750, x11-gpu 95aa618d server + client)

glxgears 1280x720, % of one core, `x11-verify.sh measure`; "before" = the old stack (`-shm`
llvmpipe Xwayland, GLX readback + PutImage).

| run | fps | phone | glxgears | Xwayland | kwin | server | GPU |
|---|---|---|---|---|---|---|---|
| before, unpaced | 160 | 229% | 28% | 42% | 71% | 37% | 15% |
| Espryt DRI3+Present, interval 1 | 98-101 | 210% | 12% | 5% | 28% | 32% | 12% |
| Espryt DRI3+Present, vblank_mode=0 | 1440-1780 | 296% | 29% | 29% | 36% | 62% | 72% |
| Espryt readback + MIT-SHM on glamor | 46 | 168-188% | 5% | 78% | 11% | 35% | 5% |
| Magma DRI3+Present, interval 1 | 105-112 | 210% | 12% | 5% | 24% | 43% | 16% |
| Magma DRI3+Present, vblank_mode=0 | 940-970 | 282% | 24% | 18% | 33% | 60% | 77% |
| Magma readback + MIT-SHM on glamor | 27 | 161% | 2% | 50% | 7% | 70% | 5% |

- Glamor engages on both backends: Xwayland runs without `-shm`, offers DRI3 1.2 + Present 1.2,
  its windows are linux-dmabuf shared images; GLX windows log `presented through DRI3+Present`.
- Found on the device and fixed: uploads into a texture bound to a shared image were dropped
  (Espryt) - an X server's PutImage into a window pixmap showed black windows. They are now
  drained at the producer's flush and written into the image (RGB-converted for X formats, which
  the driver holds as RGB8).
- The forced readback path is slower on a glamor Xwayland than on the old llvmpipe one (glamor
  uploads every frame through the server); it is only the fallback now.
- OPEN: after a hide/show or lock/unlock round, an X11 Qt window (kate) can keep stale or black
  regions until the app repaints them; XGetImage of the window shows the window pixmap itself
  lacks them, on both backends. Fresh windows render correctly. Workaround:
  `ANLAND_XWAYLAND_GLAMOR=0`.
