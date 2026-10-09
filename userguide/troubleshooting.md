# Troubleshooting

## Logs

The default log level of all builds is INFO: info, warning, error and fatal lines are compiled in.
Debug lines need a build with `MOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_DEBUG`
(Gradle: `-Pmobilegl.logLevel=MOBILEGL_LOG_LEVEL_DEBUG`).

| Setup | Where |
|---|---|
| Android, any app | logcat, tag `MobileGL`: `adb logcat -s MobileGL` |
| FCL embedded (monolith build) | also `/sdcard/MG/latest.log`, if the process may write there |
| Plugin in a launcher (split build) | also `/sdcard/MG/latest.client.log` and `latest.server.log` (the `inproc` backend thread writes the `.server` file) |
| Plugin render server | logcat tags `MobileGL`, `MobileGLServer`, `MobileGLDisplay`; files `mgl.server.log` / `mglwin.server.log` in the app's private files directory |
| Linux, Windows | only the file named by `MOBILEGL_LOG_FILE_PATH`. Split builds write `<name>.client.<ext>` and `<name>.server.<ext>`; a remote server's lines are forwarded into the client's `.server` file. |

Native crashes on Android: `adb logcat -b crash -d`.

Lines worth searching for:

| Line | Meaning |
|---|---|
| `Config: Active backend type set to DirectGLES` / `DirectVulkan` | Settings were read; which backend is used |
| `Config: debug.mobilegl.env sets ...` | A setting came from the Android property |
| `Config: MOBILEGL_TRANSPORT=inproc - ...` / `=spawn - ...` | A split topology was selected |
| `MG_Remote client: spawn ARMED - the server role runs in pid N` | `spawn` with `fork` started its own server |
| `MG_Remote client: control=tcp data=stream server=...` / `control=unix data=shm ...` | Connected to a running server |
| `MG_Remote server: pid=N listening on <endpoint>` | A server is ready |
| `Loaded GL backend library: <name>` | Espryt found the system (or ANGLE) EGL/GLES |

## MobileGL is not used at all

The renderer string does not mention Espryt or Magma.

- **FCL:** check the renderer selection, and that no leftover `debug.mobilegl.*` property or line in
  the Environment Variables editor points somewhere else.
- **Windows:** the program loaded the system `opengl32.dll`. Put the copy next to the executable that
  creates the GL context (for Java, next to `java.exe`/`javaw.exe`).
- **Linux, system-wide setup:** the log says
  `no MobileGL server answers at "<endpoint>" ... declining, so the system's next GL implementation serves this process`.
  The server is not running or the endpoint in `client.conf` is wrong. `MOBILEGL_IPC_PROBE_TIMEOUT_MS`
  raises the probe's patience (default 250 ms).
- **Linux, GLX:** `__GLX_VENDOR_LIBRARY_NAME=mobilegl` is not set in that process, or
  `libGLX_mobilegl.so.0` is not in the system library directory.

## The transport setting has no effect

- No `Config: MOBILEGL_TRANSPORT=...` line: the library was built without the split layer. The
  FCL-embedded copy is always built that way; use the plugin or rebuild with
  `MOBILEGL_BUILD_DISAGGREGATED=ON`.
- `Config: MOBILEGL_TRANSPORT='unix:...' names a transport ... staying on monolith`: put the endpoint
  in `MOBILEGL_IPC_CONTROL` and set `MOBILEGL_TRANSPORT=spawn`.
- The plugin runs monolith in the launcher although you expected `inproc`: the APK was built without
  an explicit `-Pmobilegl.buildDisaggregated=ON` (or `MOBILEGL_BUILD_DISAGGREGATED=ON` in the environment), so its launcher environment has no
  `MOBILEGL_TRANSPORT`. Rebuild, or add `MOBILEGL_TRANSPORT=inproc` in the launcher's environment
  editor.

## Out-of-process rendering on Android

| Message | Cause | Fix |
|---|---|---|
| `Fatal{UnmigratedSurface, "AndroidNativeWindow:use-MOBILEGL_IPC_SURFACE=server"}` (a `MetalLayer:use-MOBILEGL_IPC_SURFACE=server` twin exists for macOS) | A `spawn` client created a window surface with `MOBILEGL_IPC_SURFACE=offscreen`. An Android window cannot cross processes. The limitation is permanent. (Servers older than this fix print `AndroidNativeWindow@P12` and mention a development phase; it means the same thing.) | Use the on-screen display server and `MOBILEGL_IPC_SURFACE=server` ([android-plugin.md](android-plugin.md#out-of-process-rendering-with-a-window)), or use `inproc` |
| `Refuse ServerOwned (NoServerDisplay)` | `MOBILEGL_IPC_SURFACE=server`, but the server is offscreen (the plugin's service, a spawned server, any Linux server) | Start the plugin's server with **On-screen window** ticked |
| `Refuse ServerOwned (NoServerWindow) ... is the display's surface visible?` | The display server's window was not visible when the client created its window surface | Bring the MobileGL display window to the front, then start the game |
| `SurfaceModeMismatch` | The session's first surface was a pbuffer, so the session is offscreen; a later server window surface is refused | The application must create its window surface first |

## Connection refusals

The client logs `MGPipe: peer Refuse{<code>} <detail> ...`:

| Code / detail | Fix |
|---|---|
| `Authentication` `token mismatch` | Set the same `MOBILEGL_IPC_TOKEN` on client and server |
| `WireFingerprint` `wire layout` / `ProtocolVersion` | Client and server come from different source revisions; build both from one commit |
| `BuildFingerprint` `build identity` | Different builds, and `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1` (or `fork`, which always requires the same build). A stale `libMobileGLServer.so` next to the library, or a stale `MOBILEGL_IPC_SERVER_PATH`, causes this too. |
| `Backend` `Hello.backendType disagrees with pinned backend` | The server is pinned to the other backend: by its `MOBILEGL_BACKEND_TYPE`, or, for the on-screen display server, by its first session. Match the client's backend, or restart the server. |
| `Busy` | The server takes no more sessions (`--max-sessions`) |

A warning `compatible wire with different build` means the two ends are different builds of a
compatible protocol. It works, but build both ends from one commit when possible.

## The server does not start

`libMobileGLServer.so` exit codes:

| Code | Cause |
|---|---|
| 64 | `MOBILEGL_IPC_DIAL=no` missing from its environment |
| 65 | It inherited `MOBILEGL_TRANSPORT`, `MOBILEGL_IPC_SERVER_PATH`, `MOBILEGL_IPC_RING_MB` or `MOBILEGL_IPC_STAGE_MB`; unset them |
| 71 | Bad arguments: no endpoint, an unknown option, or `--max-sessions` outside 1-64 or used with TCP or without `--serve` |
| 72 | Could not listen. Also when `MOBILEGL_IPC_TOKEN` is shorter than 16 bytes, or a non-loopback TCP address has no token. The log says `Refuse{Authentication} ...`. |
| 73 | Accepting a connection failed or timed out |

In the plugin's **Render Server** screen:

- `Non-loopback listen requires a token of at least 16 bytes.`: `0.0.0.0` counts as non-loopback.
  Tap **Generate token**, or listen on `tcp://127.0.0.1:<port>`.
- `A server is already running; stop it before reconfiguring.`: tap **Stop server** first.
- The on-screen server keeps its first configuration for the life of its process. To change it, stop
  it (or force-stop the app) and start again.

## Device loss and hangs

- `MOBILEGL_GPU_HANG_BUDGET_MS` (default 2000) ends a session whose GPU submission runs longer than
  that. Slow software rasterizers trip it; set it to `0` there.
- When a server dies or its window goes away, the client's session is lost. With the default
  `MOBILEGL_IPC_RECOVER=1`, the application's next context or surface creation starts a fresh
  session; contexts from before the loss stay lost.
- A GPU hang caused by one session stalls every GPU user on the device until the kernel resets the GPU
  (about 2 s, longer in some cases). This is a hardware/driver property, not something MobileGL can
  shorten.

## Rendering problems

- Try the other backend (`MOBILEGL_BACKEND_TYPE`). Backend-specific bugs are common.
- Older or non-conformant programs: `MOBILEGL_RELAXED_SEMANTICS=1`.
- Engines that write persistent buffer maps without flushing them: `MOBILEGL_COHERENT_AS_FLUSH=1`.
- Suspect shader translation: `MOBILEGL_SHADER_CACHE=0` rules out the translation cache.
- Magma on a driver with broken subgroup support: `MOBILEGL_MAGMA_DISABLE_SUBGROUP=1`.

## Platform-specific

**Windows**

- `Failed to open ANGLE libGLESv2.dll` (Espryt): put `libEGL.dll`, `libGLESv2.dll` and
  `d3dcompiler_47.dll` from one x64 ANGLE build next to the program.
- Magma finds no device: install your GPU vendor's Vulkan driver; the Vulkan SDK alone has none.
- Crashes inside a third-party overlay's Vulkan layer (often at exit): disable implicit layers
  for the program with the Vulkan loader's `VK_LOADER_LAYERS_DISABLE=~implicit~`.
- The build fails with `error C2610` in `MG_Util/Damage/Damage.h`: see the build status note in
  [windows.md](windows.md).

**Linux**

- Windows show `presented through wl_shm` instead of `linux-dmabuf shared images`: zero-copy needs a
  server that allocates shared images (the Android server) and a compositor with `linux-dmabuf`.
  The `wl_shm` path works, but is slower.
- `eglCreateWindowSurface` fails on a GBM display: by design. GBM displays are offscreen in MobileGL.
- Replacing `libMobileGL.so` in place crashes running programs: install to a new file and rename it
  over the old one.

**Building**

- `ENABLE_OPT set but SPIR-V tools not found` (or similar) at configure: run
  `python3 update_glslang_sources.py` in `3rdparty/glslang`.
- `MOBILEGL_BUILD_DISAGGREGATED=ON but 3rdparty/flatbuffers/include is missing`: run
  `git submodule update --init 3rdparty/flatbuffers`.
- Tests run with wrong environments under CMake 4.x: build and test with CMake 3.31.x.
- Gradle builds are slow: `-Pmobilegl.enableLto=OFF` skips ThinLTO (the library is then slower).
