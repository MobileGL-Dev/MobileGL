# MobileGL on Linux

On Linux (glibc), MobileGL builds as `libMobileGL.so`. One library provides:

- **EGL** for the surfaceless, Wayland and GBM platforms. On GBM it is offscreen only: window
  surfaces there are refused.
- **GLX** for X11.
- **glvnd vendor** entry points for both, so the system's `libEGL.so.1` / `libGL.so.1` (libglvnd) can
  route applications to it.
- In split builds, a GBM backend module (`mobilegl_gbm.so`) and the server executable
  (`libMobileGLServer.so`).

There are two ways to use it:

| Setup | When |
|---|---|
| [Split client](#system-wide-split-client-glvnd-vendor-gbm-backend) talking to a MobileGL server on Android | The main Linux use case: a Linux container on Android (anland) or a Linux PC rendering on a phone's GPU |
| [Monolith on a Linux desktop](#monolith-on-a-linux-desktop) | Experimental: running on the desktop's own GLES/Vulkan driver |

Build instructions: [building.md](building.md#linux).

## Build outputs

| File (in the build directory) | Build | Purpose |
|---|---|---|
| `libMobileGL.so` | always | The library |
| `libEGL.so`, `libEGL.so.1`, `libGL.so`, `libGL.so.1`, `libGLX_mobilegl.so.0` | always | Symlinks to `libMobileGL.so`, for direct loading and for glvnd's GLX vendor naming |
| `libMobileGLServer.so` | split | The server executable (despite the name, an executable) |
| `MobileGL/MG_Gbm/mobilegl_gbm.so` | split, and only if `gbm.h` and `gbm_backend_abi.h` are installed | GBM backend module |

## Presentation

Window surfaces are drawn by the backend and then handed to the window system by the client:

| Window system | Zero-copy path (split client with server-allocated shared images) | Fallback |
|---|---|---|
| Wayland (`wl_egl_window`) | `zwp_linux_dmabuf_v1` buffers | read back into `wl_shm` buffers |
| X11 (GLX) | DRI3 + Present | read back + MIT-SHM `PutImage`, or plain `PutImage` |

Shared images come from a server that can allocate them; the Android server can. The log names the
path chosen:

```text
Wayland: window surface 1280x720 presented through linux-dmabuf shared images (...)
glx: window 0x... (...) presented through DRI3+Present shared images (...)
```

`MOBILEGL_WAYLAND_DMABUF=0` forces `wl_shm`; `MOBILEGL_GLX_PRESENT=readback|shm|putimage` forces a
GLX readback path.

## Topologies on Linux

| `MOBILEGL_TRANSPORT` | `MOBILEGL_IPC_CONTROL` | Meaning |
|---|---|---|
| unset / `monolith` | | Backend in the application's thread |
| `inproc` | | Backend on a second thread (split build) |
| `spawn` | unset / `fork` | The client starts its own `libMobileGLServer.so`, found next to `libMobileGL.so` or at `MOBILEGL_IPC_SERVER_PATH` |
| `spawn` | `unix:/path` or `unix:@name` | Connect to a running server on a Unix socket (shared-memory data plane) |
| `spawn` | `tcp://host:port` | Connect to a running server over TCP (stream data plane) |

`MOBILEGL_TRANSPORT=unix:...` and `pipe:...` are **not** valid: they log an error and run monolith.
Put the endpoint in `MOBILEGL_IPC_CONTROL` instead.

Client and server must be built from the same source revision. Mismatched wire layouts are refused;
otherwise the client logs a `compatible wire with different build` warning.

## Running a server on Linux

A Linux server is offscreen only: it has no display of its own, so clients present their windows
themselves (table above). `MOBILEGL_IPC_SURFACE=server` is refused by a Linux server.

The server needs `MOBILEGL_IPC_DIAL=no` in its environment, and must **not** inherit
`MOBILEGL_TRANSPORT`, `MOBILEGL_IPC_SERVER_PATH`, `MOBILEGL_IPC_RING_MB` or `MOBILEGL_IPC_STAGE_MB`
(it exits with code 64 or 65 otherwise).

Unix socket, several clients (each session gets its own forked process and backend):

```sh
env -u MOBILEGL_TRANSPORT MOBILEGL_IPC_DIAL=no ./libMobileGLServer.so @mobilegl --serve --max-sessions 16
```

TCP, reachable from other machines (a token of at least 16 bytes is required for any non-loopback
address):

```sh
env -u MOBILEGL_TRANSPORT MOBILEGL_IPC_DIAL=no MOBILEGL_IPC_TOKEN=<16+ byte secret> ./libMobileGLServer.so tcp://0.0.0.0:40613 --serve
```

Arguments: `<endpoint>` (or `MOBILEGL_IPC_ENDPOINT`), `--serve` (keep accepting sessions; without it
the process serves one session and exits), `--max-sessions <1-64>` (Unix `--serve` only, default 1).
The client picks the backend for its session; set `MOBILEGL_BACKEND_TYPE` on the server to pin one.
The server prints `MG_Remote server: pid=<pid> listening on <endpoint>` when it is ready.

## TCP client

On a Linux machine, run any GL program against a remote server (for example a phone running the
plugin's render server, see [android-plugin.md](android-plugin.md#tcp-server-for-remote-clients)):

```sh
MOBILEGL_TRANSPORT=spawn MOBILEGL_IPC_CONTROL=tcp://192.168.1.20:40613 MOBILEGL_IPC_TOKEN=<token> MOBILEGL_LOG_FILE_PATH=/tmp/mgl.log __EGL_VENDOR_LIBRARY_FILENAMES=/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json eglinfo -B
```

(`eglinfo` and `glxinfo` come with your distribution's GL utilities package; any EGL or GLX program works. The vendor file is
described below.) The client writes `/tmp/mgl.client.log`; the server's log lines are forwarded to
the client and written to `/tmp/mgl.server.log`. A successful connection logs
`MG_Remote client: control=tcp data=stream server=192.168.1.20:40613 ... dial=connect`.

Every GL object creation, query and readback waits for a network round trip. Expect loading-heavy
programs to be slow over Wi-Fi.

## System-wide split client: glvnd vendor, GBM backend

This makes MobileGL the GL implementation for every process on the machine, while falling back to the
distribution's driver whenever no MobileGL server is reachable. It is how the anland container is set
up. Use it only with a split client configuration: in monolith mode MobileGL never declines, so it
would take over every EGL application.

How the fallback works: before a client starts a session, it probes the configured endpoint with one
non-blocking connect (bounded by `MOBILEGL_IPC_PROBE_TIMEOUT_MS`, default 250 ms; `0` disables the
probe). If nothing answers, the EGL vendor returns `EGL_NO_DISPLAY` and reports no devices, the GLX
vendor fails to load, and `mobilegl_gbm` fails `create_device`. libglvnd and libgbm then use the next
implementation.

### Files to install

Install the library under a directory of its own. Copy to a new file and rename it into place: never
overwrite a `.so` that running processes have mapped.

| Path | Contents |
|---|---|
| `/opt/mobilegl/lib/libMobileGL.so` | the library |
| `/opt/mobilegl/lib/libEGL.so.1`, `libGL.so.1` (and the unversioned names) | optional symlinks to `libMobileGL.so`, for programs started with `LD_LIBRARY_PATH=/opt/mobilegl/lib` |
| `/usr/share/glvnd/egl_vendor.d/10_mobilegl.json` | EGL vendor, sorted ahead of the distribution's `50_*.json` |
| `/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json` | the same JSON, for forcing MobileGL per process with `__EGL_VENDOR_LIBRARY_FILENAMES` |
| `<libdir>/libGLX_mobilegl.so.0` | symlink to `/opt/mobilegl/lib/libMobileGL.so`; `<libdir>` is the system library directory (`/usr/lib` on Arch, `/usr/lib/<triplet>` on Debian/Ubuntu) |
| `/etc/mobilegl/client.conf` | client settings (below) |
| `/etc/mobilegl/backend` | optional: one line, `DirectGLES` or `DirectVulkan` |
| `<gbm backend dir>/mobilegl_gbm.so` | GBM backend; the directory libgbm searches (`/usr/lib/gbm` on Arch; `GBM_BACKENDS_PATH` overrides it) |
| `/etc/environment.d/10-mobilegl.conf` and `/etc/profile.d/mobilegl.sh` | `__GLX_VENDOR_LIBRARY_NAME=mobilegl` and `GBM_BACKEND=mobilegl` for the systemd user session and login shells |

The vendor JSON (both copies):

```json
{"file_format_version":"1.0.0","ICD":{"library_path":"/opt/mobilegl/lib/libMobileGL.so"}}
```

`/etc/environment.d/10-mobilegl.conf`:

```text
__GLX_VENDOR_LIBRARY_NAME=mobilegl
GBM_BACKEND=mobilegl
```

`/etc/profile.d/mobilegl.sh`:

```sh
export __GLX_VENDOR_LIBRARY_NAME=mobilegl
export GBM_BACKEND=mobilegl
```

glvnd picks GLX vendors per X screen, so GLX needs `__GLX_VENDOR_LIBRARY_NAME`; EGL needs only the
JSON file.

### `/etc/mobilegl/client.conf`

Every process that loads `libMobileGL.so` reads this file. Format: `KEY=value` lines; blank lines
and `#` comments are skipped; one pair of matching quotes around a value is removed; only
`MOBILEGL_*` and `LIBGL_*` keys are taken. Example for a server on an abstract Unix socket:

```text
MOBILEGL_TRANSPORT=spawn
MOBILEGL_IPC_DATA=shm
MOBILEGL_IPC_CONTROL=unix:@anland-mobilegl
```

Precedence, highest first: the process environment, then `/etc/mobilegl/backend` (backend only), then
`client.conf`, then built-in defaults. `MOBILEGL_CONFIG_FILE=<path>` and
`MOBILEGL_BACKEND_FILE=<path>` point at other files; set either to an empty value to disable that
file. A server process (`MOBILEGL_IPC_ROLE=server`) reads neither file.

### Other knobs

| Variable | Use |
|---|---|
| `MOBILEGL_DEVICE_DRM_NODE` | DRM render node MobileGL reports as its EGL device (default `/dev/dri/renderD128`). Compositors match it against the node they opened. |
| `MOBILEGL_GBM_LIBRARY` | Path `mobilegl_gbm.so` loads `libMobileGL.so` from, if the process has not loaded it already |
| `MOBILEGL_X11_DIAL` | Override for whether MobileGL may open an X11 connection (by default it does not dial a display that the process itself serves, such as Xwayland) |

### How to tell it works

```sh
eglinfo -B
```

```sh
glxinfo -B
```

The renderer line names `Espryt (MobileGL Core)` or `Magma (MobileGL Core)` while the server is up,
and the distribution's driver while it is down. To see the fallback without stopping the server,
point one process at an endpoint nobody listens on:

```sh
sed 's|^MOBILEGL_IPC_CONTROL=.*|MOBILEGL_IPC_CONTROL=unix:@nobody|' /etc/mobilegl/client.conf > /tmp/down.conf
```

```sh
MOBILEGL_CONFIG_FILE=/tmp/down.conf eglinfo -B
```

## The anland setup

anland runs a Linux desktop (for example KDE Plasma in an Arch Linux ARM container) on a rooted
Android device. With MobileGL:

- the anland Android app embeds the MobileGL **server** through the C interface described in
  [android-plugin.md](android-plugin.md#embedding-the-server-in-your-own-apk): a windowed server whose
  window is the compositor's Android surface, and offscreen servers for the other applications
  (`docs/Disaggregated/guide/embedded-server.md` describes both shapes);
- inside the container, the **client** library (built for aarch64 glibc with
  `MOBILEGL_BUILD_DISAGGREGATED=ON`) is installed system-wide exactly as above, with `client.conf`
  pointing at `unix:@anland-mobilegl`. The container shares the device's network namespace, so the
  abstract socket is reachable;
- Wayland clients present zero-copy through `linux-dmabuf`, and GBM users (the compositor's GBM
  device, Chromium's GPU process) get MobileGL server images from `mobilegl_gbm.so`.

The app, its container scripts and the compositor patch belong to the anland project, not to this
repository. Build the server library with the Android NDK and the client with an aarch64 glibc cross
toolchain ([building.md](building.md)). The developer runbook for this exact setup is
`docs/Disaggregated/notes/anland/runbook-plasma.md`.

## Monolith on a Linux desktop

The library also runs monolith on a desktop's own driver: Espryt on the system EGL/GLES (it opens
`libEGL.so.1`, trying the system library directories first), Magma on the system Vulkan
loader. CI runs MobileGL's test binaries and trace replayer this way, on software drivers. Running
real applications this way is not covered by CI; treat it as experimental.

Per process, through glvnd (EGL applications):

```sh
MOBILEGL_BACKEND_TYPE=DirectVulkan MOBILEGL_LOG_FILE_PATH=/tmp/mgl.log __EGL_VENDOR_LIBRARY_FILENAMES=/opt/mobilegl/share/glvnd/egl_vendor.d/50_mobilegl.json eglinfo -B
```

GLX applications (with `libGLX_mobilegl.so.0` installed as above):

```sh
MOBILEGL_BACKEND_TYPE=DirectVulkan __GLX_VENDOR_LIBRARY_NAME=mobilegl glxinfo -B
```

Prefer Magma for EGL applications. Espryt loads the system `libEGL.so.1`, which on a glvnd system is
the same dispatcher. When `__EGL_VENDOR_LIBRARY_FILENAMES` lists only MobileGL, that dispatcher can
only route Espryt back to MobileGL. This combination has not been verified.

Without glvnd, start the program with `LD_LIBRARY_PATH` pointing at the build directory, so its
`libEGL.so.1`/`libGL.so.1` resolve to MobileGL's symlinks.
