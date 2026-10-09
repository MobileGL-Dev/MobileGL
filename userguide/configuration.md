# Configuration

MobileGL is configured with `MOBILEGL_*` variables. This page lists the ones meant for users.
Variables that exist for development, CI and benchmarking (`MOBILEGL_PIPE_*`, most `MOBILEGL_IPC_*`
tuning knobs, quirk overrides) are left out on purpose. Some of them are negative controls that
break rendering by design.

## Where settings come from

Settings are read once, when the library initializes in a process. Changing them later has no effect
until the process restarts.

| Source | Platform | Precedence |
|---|---|---|
| `debug.mobilegl.backend` property | Android | Overrides `MOBILEGL_BACKEND_TYPE` from every other source |
| `debug.mobilegl.transport` property | Android, split builds | Overrides `MOBILEGL_TRANSPORT` from every other source |
| Process environment | all | Highest for everything else |
| `/etc/mobilegl/backend` (one line: the backend name) | desktop Linux | Below the environment; `MOBILEGL_BACKEND_TYPE` only |
| `/etc/mobilegl/client.conf` (`KEY=value` lines) | desktop Linux | Below the backend file |
| `debug.mobilegl.env` property (`KEY=VALUE;KEY=VALUE`, at most 91 characters) | Android | Below the environment |
| Built-in default | all | Lowest |

- Only keys starting with `MOBILEGL_` or `LIBGL_` are accepted from the environment and from
  `client.conf`.
- `MOBILEGL_CONFIG_FILE` and `MOBILEGL_BACKEND_FILE` name other files instead of the two `/etc`
  files; an empty value turns a file off.
- A server process (`MOBILEGL_IPC_ROLE=server`) reads neither file nor `debug.mobilegl.env`.
- Settings marked **env only** below are read straight from the process environment. They ignore the
  files and `debug.mobilegl.env`.
- On Android, use `adb shell setprop <property> <value>` (no root needed). Properties apply to every
  app on the device that loads MobileGL, and last until reboot.

Value rules:

- **Flag**: on when set to anything other than empty, `0` or `false` (case-insensitive).
- **Auto/on/off**: unset keeps the built-in choice; a flag-true value forces it on; anything else
  forces it off.
- **Integer**: a value outside the range is ignored with a warning in the log, and the default is used.

Where a setting takes effect: each process reads its own settings. When you connect to a server that
was started separately (the plugin's render server, a Linux server), the backend settings
(`MOBILEGL_ESPRYT_*`, `MOBILEGL_MAGMA_*`, `MOBILEGL_FRAMES_IN_FLIGHT`) belong in the **server's**
environment too. The backend itself is the exception: the client asks for it (see
`MOBILEGL_BACKEND_TYPE`).

## Backend and general

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `MOBILEGL_BACKEND_TYPE` | `DirectGLES`, `DirectVulkan` | `DirectGLES` | Espryt (GLES) or Magma (Vulkan). Case-sensitive. Any other value leaves MobileGL without a backend. In a split session the client's value is sent to the server, which uses it unless the server pinned a different one. |
| `MOBILEGL_FRAMES_IN_FLIGHT` | 1-64 | 3 | How many frames may be queued ahead of the GPU. Magma: its frame contexts. Espryt: after presenting frame N it waits for frame N+1-value to finish (1 = full CPU/GPU sync). Split topologies: the client may run `max(1, value - 2)` presents ahead of the server, so 4 gives one more frame of overlap (and of latency). The old name `MOBILEGL_MAGMA_FRAMESINFLIGHT` still works but is deprecated. |
| `MOBILEGL_RELAXED_SEMANTICS` | flag | off | Apply legacy leniencies (for example drawing with vertex array object 0, reusing texture names after delete) even to contexts that explicitly requested a core profile. Contexts that did not request core get them anyway. |
| `MOBILEGL_ADVERTISE_FP64` | flag | off | Advertise `GL_ARB_gpu_shader_fp64`. `double` types compile either way but run at 32-bit precision. |
| `MOBILEGL_DISABLE_TIMERQUERY` | flag | off | Hide and disable GPU timer queries |
| `MOBILEGL_COHERENT_AS_FLUSH` | flag | off | Treat persistent maps with `GL_MAP_FLUSH_EXPLICIT_BIT` as coherent, for engines that never flush them |
| `MOBILEGL_ASYNC_SHADER_COMPILE` | auto/on/off | on | Compile and link shaders on worker threads. Off runs them on the calling thread and withdraws `GL_KHR_parallel_shader_compile`. |
| `MOBILEGL_ASYNC_SHADER_COMPILE_THREADS` | 0-64 | 0 | Worker count; 0 = min(4, number of big cores) |
| `MOBILEGL_SHADER_CACHE` | auto/on/off | on | In-memory shader translation cache. Turn it off to rule it out when a shader renders wrongly. |
| `MOBILEGL_ES_CONTEXT_IDENTITY` | `1` | off | **env only.** Contexts created for the OpenGL ES API report themselves as OpenGL ES 3.2 instead of the desktop version |

## Espryt (DirectGLES)

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `MOBILEGL_ESPRYT_USE_ANGLE` | flag | off | Load `libEGL_angle.so` / `libGLESv2_angle.so` instead of the system EGL/GLES. Not used on Windows, where Espryt always loads ANGLE's `libEGL.dll` / `libGLESv2.dll`. |
| `MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER` | flag | off | Avoid mipmap minification filters in samplers (a workaround for certain driver bugs) |
| `MOBILEGL_YUV_DRIVER_CONVERSION` | `1` | off | **env only.** Convert YUV images with the driver's external-sampler conversion instead of MobileGL's own |

## Magma (DirectVulkan)

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `MOBILEGL_MAGMA_DISABLE_SUBGROUP` | flag | off | Do not use Vulkan subgroup operations in shaders |
| `MOBILEGL_MAGMA_R11G11B10F_FALLBACK` | flag | off | Use a fallback format for `GL_R11F_G11F_B10F` |
| `MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER` | 0-16777216 | 16384 | Submit a command buffer mid-frame after this many draws/dispatches (0 = never). Bounds driver memory in huge loading frames. |
| `MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES` | 0-1048576 | 120 | Free grown descriptor pools after this many quiet frames (0 = never) |
| `VK_ICD_FILENAMES` | path to an ICD JSON | loader default | Standard Vulkan loader variable: picks the Vulkan driver |

## Logging and diagnostics

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `MOBILEGL_LOG_FILE_PATH` | path | Android: `/sdcard/MG/latest.log`; elsewhere none | **env only.** Log file. Split builds write `<name>.client<ext>` and `<name>.server<ext>` instead (for example `latest.client.log`); in a client, `.server` receives the lines the server forwards. The file is truncated when opened. On Android, logs also go to logcat with tag `MobileGL`. |
| `MOBILEGL_FPS_LOG` | `1` | off | **env only** (Android: also `debug.mobilegl.fps_log`). One line about every two seconds with frame rate and frame time. |
| `MOBILEGL_GPU_HANG_BUDGET_MS` | 0-600000 | 2000 | **env only** (Android: also `debug.mobilegl.gpu_hang_budget_ms`). A GPU submission running longer than this ends its session as hung. 0 turns the watchdog off; do that on slow software rasterizers. |

## Transport (split builds only)

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `MOBILEGL_TRANSPORT` | `monolith`, `inproc`, `spawn` | `monolith` | Where the backend runs ([README.md](README.md#topologies-where-the-backend-runs)). `unix:...` / `pipe:...` are rejected (logged, and the run stays monolith); use `MOBILEGL_IPC_CONTROL`. Ignored in builds without the split layer. |
| `MOBILEGL_IPC_CONTROL` | `fork`, `unix:<path>`, `unix:@<name>`, `tcp://<host>:<port>` | `fork` | With `spawn`: `fork` launches a private server; the others connect to a running one. (`fd:<a>,<b>` is set by the plugin's external-client helper.) |
| `MOBILEGL_IPC_DATA` | `auto`, `shm`, `stream` | `auto` | Data plane. `auto` picks shared memory for Unix sockets and `fork`, and a stream for TCP; TCP accepts only `stream`, the others only `shm`. |
| `MOBILEGL_IPC_SURFACE` | `offscreen`, `server` | `offscreen` | `server`: window surfaces are created on the server's own window, and the client's window handle is ignored. Needs a server with a display (the plugin's on-screen server, or an app embedding one). Only meaningful with `spawn`. |
| `MOBILEGL_IPC_TOKEN` | string | none | **env only.** Shared secret; client and server must agree. A server listening on a non-loopback TCP address requires one of at least 16 bytes. |
| `MOBILEGL_IPC_SERVER_PATH` | path | next to `libMobileGL.so` | Server executable for `fork` |
| `MOBILEGL_IPC_PROBE_TIMEOUT_MS` | 0-60000 | 250 | Before starting a session, probe the configured `unix:`/`tcp://` endpoint; if nothing answers, the EGL/GLX/GBM entry points decline so the system's next GL implementation is used. 0 disables the probe. |
| `MOBILEGL_IPC_RECOVER` | 0, 1 | 1 | After the server dies or the device is lost, let the next `eglCreateContext` / `eglCreate*Surface` / `eglInitialize` start a fresh session. 0 makes the loss final for the process. |
| `MOBILEGL_IPC_CONTROL_TIMEOUT_MS` | 100-600000 | 5000 | How long the client waits for an answer to a surface operation |
| `MOBILEGL_IPC_COLD_START_MS` | 100-600000 | 20000 | The same wait while the server brings its backend up (first surface and first make-current) |
| `MOBILEGL_IPC_PRESENT_CREDIT` | 1-8 | derived from `MOBILEGL_FRAMES_IN_FLIGHT` | Explicit override of how many presents the client may run ahead |
| `MOBILEGL_IPC_REQUIRE_SAME_BUILD` | `1` | off | **env only.** Refuse a server built from a different revision (by default that is a warning) |

## Server process

`libMobileGLServer.so` and embedding apps (see [android-plugin.md](android-plugin.md#embedding-the-server-in-your-own-apk)):

| Variable | Meaning |
|---|---|
| `MOBILEGL_IPC_DIAL=no` | **Required** in the server's environment. It stops the server from acting as a client itself. |
| `MOBILEGL_IPC_ROLE=server` | Marks the server role. `libMobileGLServer.so` sets it itself; an embedding app sets it before loading the library. |
| `MOBILEGL_IPC_ENDPOINT` | Endpoint, when none is given on the command line |
| `MOBILEGL_BACKEND_TYPE` | Optional: pin the server to one backend and refuse clients asking for the other |
| `MOBILEGL_IPC_TOKEN` | Token clients must present |
| `MOBILEGL_IPC_INPROC_MAX_SESSIONS` | Concurrent sessions of an embedded Unix-socket server (default 16, at most 256) |

The server refuses to start (exit code 65) if it inherited `MOBILEGL_TRANSPORT`,
`MOBILEGL_IPC_SERVER_PATH`, `MOBILEGL_IPC_RING_MB` or `MOBILEGL_IPC_STAGE_MB`.

## Linux desktop

| Variable | Meaning |
|---|---|
| `MOBILEGL_CONFIG_FILE`, `MOBILEGL_BACKEND_FILE` | **env only.** Other config files (default `/etc/mobilegl/client.conf`, `/etc/mobilegl/backend`); empty = none |
| `MOBILEGL_DEVICE_DRM_NODE` | Render node reported as MobileGL's EGL device (default `/dev/dri/renderD128`) |
| `MOBILEGL_WAYLAND_DMABUF` | **env only.** `0` presents Wayland windows through `wl_shm` instead of dma-bufs |
| `MOBILEGL_GLX_PRESENT` | **env only.** `readback`, `shm` or `putimage` forces a GLX readback path instead of DRI3/Present |
| `MOBILEGL_X11_DIAL` | **env only.** Override whether MobileGL may open an X11 connection |
| `MOBILEGL_GBM_LIBRARY` | **env only.** Where `mobilegl_gbm.so` loads `libMobileGL.so` from |
| `__EGL_VENDOR_LIBRARY_FILENAMES`, `__GLX_VENDOR_LIBRARY_NAME`, `GBM_BACKEND` | glvnd / libgbm variables that select MobileGL; see [linux.md](linux.md) |
