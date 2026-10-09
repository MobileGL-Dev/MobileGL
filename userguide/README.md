# MobileGL user guide

MobileGL is a desktop OpenGL implementation, up to OpenGL 4.6 core profile. Applications call
OpenGL through EGL, GLX or WGL. MobileGL runs those calls on one of two backends:

| Backend | Internal name | Runs on top of |
|---|---|---|
| **Espryt** | `DirectGLES` (the default) | the platform's OpenGL ES 3.x driver (EGL + GLES) |
| **Magma** | `DirectVulkan` | the platform's Vulkan driver |

You pick the backend at run time with `MOBILEGL_BACKEND_TYPE` (see [configuration.md](configuration.md)).

This guide is for people who build, install, configure and run MobileGL. Developer notes live under
`docs/`; where they disagree with this guide, this guide follows the current code.

> **Status.** MobileGL is in development and ships no prebuilt binaries. Everything here is built
> from source. Commands were checked against the `feat/disaggregated` branch.

## Topologies: where the backend runs

The GL frontend (state tracking, shader translation) always runs in the application. The backend
can run in three places:

| Topology | `MOBILEGL_TRANSPORT` | What happens | Needs a split build |
|---|---|---|---|
| **Monolith** | `monolith` (default) | Frontend and backend run on the application's GL thread. | No |
| **In-process split** | `inproc` | The backend runs on a second thread of the same process, fed through a ring buffer. | Yes |
| **Out-of-process split** | `spawn` | The backend runs in a separate server process. The client either launches its own server or connects to a running one over a Unix socket or TCP. | Yes |

A *split build* is a library built with `MOBILEGL_BUILD_DISAGGREGATED=ON`. In a build without it,
`MOBILEGL_TRANSPORT` is silently ignored and everything runs monolith.

Out-of-process rendering needs somewhere to present frames. A server process started on its own has
no window. On Android, a client's window cannot be handed to another process, so a windowed Android
app can only render out of process into a window that the **server** owns. See
[android-plugin.md](android-plugin.md#out-of-process-rendering-with-a-window).

## Which setup do I need?

| You want to... | Setup | Guide |
|---|---|---|
| Play Minecraft on Android with the copy of MobileGL that is built into an FCL build | FCL embedded, monolith | [android-fcl.md](android-fcl.md) |
| Use MobileGL in FCL (or another launcher that reads FCL renderer plugins) without rebuilding the launcher | Renderer plugin APK, monolith or `inproc` | [android-plugin.md](android-plugin.md) |
| Test out-of-process rendering on Android, with frames shown on screen | Plugin APK: on-screen display server + `spawn` client | [android-plugin.md](android-plugin.md#out-of-process-rendering-with-a-window) |
| Render on a phone's GPU for a program on a Linux PC | Plugin (server) + Linux client over TCP | [android-plugin.md](android-plugin.md#tcp-server-for-remote-clients), [linux.md](linux.md#tcp-client) |
| Give a Linux container on Android (anland) GPU acceleration through the Android drivers | Linux client library as the system glvnd/GBM vendor, server on Android | [linux.md](linux.md#system-wide-split-client-glvnd-vendor-gbm-backend) |
| Run a Linux program on a Linux desktop through MobileGL | Linux library, monolith (experimental) | [linux.md](linux.md#monolith-on-a-linux-desktop) |
| Run a Windows OpenGL program (for example Minecraft) through MobileGL | WGL `opengl32.dll` drop-in, monolith | [windows.md](windows.md) |
| Build from source | | [building.md](building.md) |
| Look up a setting | | [configuration.md](configuration.md) |
| Fix something that does not work | | [troubleshooting.md](troubleshooting.md) |

When **not** to use the split topologies: if you only want the best frame rate in one app, use
monolith (FCL embedded) or `inproc` (plugin). `spawn` adds a process boundary and a socket for every
frame. It exists for sharing one GPU-owning server between processes, sandboxes or machines.

## Platform support at a glance

| Platform | Monolith | `inproc` | `spawn` (Unix socket) | `spawn` (TCP) | Server |
|---|---|---|---|---|---|
| Android (FCL embedded) | yes | no (built without the split layer) | no | no | no |
| Android (plugin APK) | yes | yes | yes | yes | yes, offscreen or on-screen |
| Linux (glibc) | experimental | yes | yes | yes | yes, offscreen only |
| Windows (WGL) | yes, but see the build note in [windows.md](windows.md) | not supported | not supported | not supported | no |
| macOS / iOS | the code exists (CGL and NSOpenGL frontends); not covered by CI and not verified for this guide. The project README describes the macOS build. | | | | |

## How to tell MobileGL is active

- `GL_RENDERER` starts with `Espryt (MobileGL Core)` or `Magma (MobileGL Core)`.
- `GL_VENDOR` contains `MobileGL-Dev`.
- `GL_VERSION` looks like `4.6 MobileGL 26.10.0-dev..., Direct (GLES) Backend, GIT@<hash>` (or
  `Direct (Vulkan)`).
- In Minecraft, the F3 screen shows these strings.
- The log says `Config: Active backend type set to DirectGLES` (or `DirectVulkan`). Logs go to
  logcat (tag `MobileGL`) on Android, and to the file named by `MOBILEGL_LOG_FILE_PATH` elsewhere.
  See [troubleshooting.md](troubleshooting.md#logs).

## Files in this guide

| File | Contents |
|---|---|
| [android-fcl.md](android-fcl.md) | MobileGL built into FCL: monolith, choosing the backend, settings |
| [android-plugin.md](android-plugin.md) | The renderer plugin APK: install, `inproc`, the render server, windowed out-of-process rendering, TCP, same-device external programs, embedding the server in your own APK |
| [linux.md](linux.md) | Linux library: glvnd EGL/GLX vendor, GBM backend, `/etc/mobilegl/client.conf`, the server, TCP |
| [windows.md](windows.md) | WGL `opengl32.dll` drop-in |
| [building.md](building.md) | Toolchains, CMake options, Gradle properties, artifacts |
| [configuration.md](configuration.md) | User-facing `MOBILEGL_*` settings and where they can be set |
| [troubleshooting.md](troubleshooting.md) | Logs, common failures and their fixes |
