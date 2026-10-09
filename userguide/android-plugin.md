# MobileGL renderer plugin APK (Android)

The plugin is a standalone app (`top.mobilegl.plugin`, built from `android-plugin/`). It does three
jobs:

1. **Renderer plugin.** FCL and other launchers that read FCL renderer plugins find it and load its
   `libMobileGL.so` into the game process.
2. **Render server.** It can run a MobileGL server, either offscreen (a foreground service) or
   on-screen (a full-screen Activity that shows a client's frames).
3. **Driver self-test.** Its launcher screen (**POST** tab) tests the device's GLES and Vulkan
   drivers, and can run a short benchmark.

## When to use it

- You want MobileGL in a launcher without rebuilding the launcher.
- You want `inproc`, out-of-process (`spawn`) or TCP rendering on Android. Only split builds can do
  that, and the plugin is a split build by default.
- You want a phone to render for a client on another machine.

When not to use it: if your FCL build already embeds MobileGL and you only need monolith rendering,
the embedded copy is simpler ([android-fcl.md](android-fcl.md)).

## Build and install

Build instructions are in [building.md](building.md#android-plugin-apk). In short:

```sh
gradle -p android-plugin :app:assemblePluginRelease -Pmobilegl.buildDisaggregated=ON -Pmobilegl.buildDisaggregatedInproc=ON
```

The APK is
`android-plugin/app/build/outputs/apk/plugin/release/MobileGL-plugin-release-<git-hash>.apk`. A
release APK is signed only when the signing environment variables are set (see
[building.md](building.md#signing)); otherwise sign it yourself before installing.

```sh
adb install -r MobileGL-plugin-release-<git-hash>.apk
```

Android refuses to replace an installed plugin that was signed with a different key. Either
uninstall the old one first (the launcher then forgets its plugin settings) or build with
`-Pmobilegl.applicationIdSuffix=.dev` to install a second copy next to it.

Requirements: Android 8.0 (API 26) or newer. The default ABI is arm64-v8a
(`-Pmobilegl.abis=all` adds x86_64).

## Using the plugin in a launcher

The plugin advertises itself through manifest metadata. The launcher lists it as a renderer named
**MobileGL** and loads `libMobileGL.so` from the plugin's install directory.

| Metadata | Read by | Contents |
|---|---|---|
| `fclPlugin`, `renderer`, `boatEnv`, `pojavEnv` | launchers using the original FCL plugin format | Fixed environment: `LIBGL_ES=3`, `POJAV_RENDERER=opengles3`, `MOBILEGL_BACKEND_TYPE=DirectGLES`, plus `MOBILEGL_TRANSPORT=inproc` in an `inproc` build (see below) |
| `fclPlugin_V2` | launchers that read the v2 renderer-plugin format | The same environment plus user options: backend (`DirectGLES`/`DirectVulkan`), frames in flight, and toggles for `MOBILEGL_DISABLE_TIMERQUERY`, `MOBILEGL_MAGMA_DISABLE_SUBGROUP`, `MOBILEGL_MAGMA_R11G11B10F_FALLBACK`, `MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER`, `MOBILEGL_COHERENT_AS_FLUSH`, `MOBILEGL_RELAXED_SEMANTICS`, `MOBILEGL_ESPRYT_USE_ANGLE` |

The FCL build this guide was checked against reads only the original format, so it shows no plugin
options and always starts the plugin with Espryt. To change anything there, use FCL's
**Environment Variables** editor (applied after the plugin's variables), for example:

```text
MOBILEGL_BACKEND_TYPE=DirectVulkan
```

The `debug.mobilegl.*` properties described in [android-fcl.md](android-fcl.md#other-settings) also
work for the plugin. One more exists for split builds: `debug.mobilegl.transport` overrides
`MOBILEGL_TRANSPORT`.

## In-process split (`inproc`)

With `MOBILEGL_TRANSPORT=inproc`, the GL frontend runs on the game's thread and the backend runs on
a second thread. On a CPU-bound game this can raise the frame rate; it costs up to one extra frame of
latency.

The plugin's launcher environment includes `MOBILEGL_TRANSPORT=inproc` **only if** the APK was built
with `-Pmobilegl.buildDisaggregated=ON` or `-Pmobilegl.buildDisaggregatedInproc=ON` passed explicitly
(the CI build of `feat/disaggregated` does this). A plain `gradle :app:assemblePluginRelease`
still builds the split layer into the library, but the launcher environment then omits the transport
and the plugin runs monolith. Either rebuild with the property, or add
`MOBILEGL_TRANSPORT=inproc` in the launcher's environment editor.

Check the log for:

```text
Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring to an apply thread
```

## The render server

Open the MobileGL app and switch to the **Render Server** tab. It starts one of two servers:

| Mode | Component | Process | What it is for |
|---|---|---|---|
| Offscreen (default) | `MobileGLServerService` (foreground service) running `libMobileGLServer.so <endpoint> --serve` | `:mglsrv` | Remote clients over TCP, offscreen (pbuffer) programs, same-device programs through the broker |
| On-screen (tick **On-screen window**) | `MobileGLDisplayActivity`, a full-screen Activity with an in-process server | `:mglwin` | Clients that set `MOBILEGL_IPC_SURFACE=server`: their window surface is drawn into this Activity |

Fields:

| Field | Default | Notes |
|---|---|---|
| Listen endpoint | `tcp://0.0.0.0:40613` | `tcp://host:port`, `@name` (abstract Unix socket) or a file path (relative paths resolve against the app's files directory) |
| Auth token | empty | Clients present it as `MOBILEGL_IPC_TOKEN`. A TCP listen on a non-loopback address (including `0.0.0.0`) **requires** a token of at least 16 bytes; without a token, use `tcp://127.0.0.1:<port>`. **Generate token** makes a random one. |
| Extra env | empty | `KEY=VALUE;KEY` list for the server process (`KEY` alone unsets). For example `MOBILEGL_BACKEND_TYPE=DirectVulkan` pins the server's backend. |

Only one server runs at a time: starting one stops the other. The plugin flavor does not export these
components, so other apps and `adb` cannot start them by component name; use the screen.

Backend selection: the client's `MOBILEGL_BACKEND_TYPE` decides the backend for its session. A
server started with `MOBILEGL_BACKEND_TYPE` in its extra env refuses clients that ask for the other
backend. The on-screen server keeps the first session's backend for its whole process lifetime; to
switch, stop the server and start it again.

Server logs go to logcat (tags `MobileGL`, `MobileGLServer`, `MobileGLDisplay`) and to
`mgl.server.log` (offscreen) or `mglwin.server.log` (on-screen) in the app's private files directory.

## Out-of-process rendering with a window

A spawned server process has no display, and an Android client's window cannot be passed to another
process. So a windowed app (a launcher's Minecraft, for example) can render out of process only into
a window the **server** owns: the plugin's on-screen display server.

Without that, a `spawn` client that creates a window surface fails. With the default
`MOBILEGL_IPC_SURFACE=offscreen`, the server rejects the window with
`Fatal{UnmigratedSurface, "AndroidNativeWindow:use-MOBILEGL_IPC_SURFACE=server"}`. With `MOBILEGL_IPC_SURFACE=server` and an
offscreen server, the client logs `Refuse ServerOwned (NoServerDisplay)`.

Steps (client in FCL, server in the plugin, same device):

1. In the MobileGL app, **Render Server** tab: set **Listen endpoint** to
   `tcp://127.0.0.1:40613`, leave the token empty, tick **On-screen window**, tap **Start server**.
   A black full-screen window appears; this is the display server.
2. In FCL, select the **MobileGL** plugin renderer. The FCL-embedded MobileGL entries cannot do
   this, because they are built without the split layer.
3. In FCL's **Environment Variables** editor, add:

   ```text
   MOBILEGL_TRANSPORT=spawn
   MOBILEGL_IPC_CONTROL=tcp://127.0.0.1:40613
   MOBILEGL_IPC_SURFACE=server
   ```

   If the server has a token, also add `MOBILEGL_IPC_TOKEN=<token>`.
4. Launch the game. Its frames are drawn into the display server's window, at the size the game
   asks for, letterboxed to the screen.

Limits:

- The display window only shows frames. It has no input handling: touch and keys go to whichever
  window has focus.
- The display window must be visible when the game creates its window surface. Otherwise the client
  logs `Refuse ServerOwned (NoServerWindow)`. If the display window goes away while a game renders
  into it, that session ends with a device loss. The server keeps listening, and the next session
  renders when the window is back.
- Use this setup to test out-of-process rendering. For normal play, monolith or `inproc` is faster
  and simpler.
- A known open issue: each run logs `MG_Remote server: shared image 1 could not be sent on the aux
  socket (rc=3)`. Rendering continues.

How to tell it works: the client log has
`MG_Remote client: control=tcp data=stream server=127.0.0.1:40613 ... dial=connect`, and the
server's logcat has `listening on tcp://127.0.0.1:40613 (in-process display server, display installed)`.

## TCP server for remote clients

The phone renders; a client on another machine sends GL commands over TCP. The client must be a
split build of the **same commit** (see [linux.md](linux.md#tcp-client)). Only window surfaces the
client presents itself (Linux X11/Wayland windows) or offscreen surfaces work this way.

1. Find the phone's address:

   ```sh
   adb shell ip -f inet addr show wlan0
   ```

2. In the **Render Server** tab: endpoint `tcp://0.0.0.0:40613`, tap **Generate token**, keep
   **On-screen window** unticked, tap **Start server**.
3. On the client, set `MOBILEGL_TRANSPORT=spawn`, `MOBILEGL_IPC_CONTROL=tcp://<phone-ip>:40613` and
   `MOBILEGL_IPC_TOKEN=<token>`.

If the network isolates devices, tunnel over adb instead: start the server on
`tcp://127.0.0.1:40613` (no token needed), then on the client machine run the command below and
point the client at `tcp://127.0.0.1:40613`:

```sh
adb forward tcp:40613 tcp:40613
```

## Same-device programs outside the app (Termux, `adb shell`)

Android does not let another app connect to the plugin's sockets directly. The plugin ships a broker
for this case: a helper class, run with `app_process` under the program's own user, asks the plugin
for a connected socket pair and then starts the program with it.

1. Start the **offscreen** server with a token of at least 16 bytes (**Generate token**).
2. Tap **Copy launch command**. It produces a line of this form:

   ```sh
   MOBILEGL_IPC_TOKEN='<token>' CLASSPATH=<plugin APK path> app_process / top.mobilegl.plugin.ExternalClientHelper -- <program> [args...]
   ```

3. Run it with your program. The program must use a split build of MobileGL built for Android
   (bionic). The helper starts it with `MOBILEGL_TRANSPORT=spawn`,
   `MOBILEGL_IPC_CONTROL=fd:<control>,<aux>` and `MOBILEGL_IPC_DATA=shm` already set.

Helper options: `--server <package>` (default: the APK it was loaded from), `--timeout-ms <n>`
(default 10000), `--keep-affinity` (do not keep the program off the server's reserved CPU core). The
token can also come from a file named by `MOBILEGL_IPC_TOKEN_FILE`; never put it on the command line.
The program renders offscreen: the offscreen server has no display.

## Embedding the server in your own APK

An app can host the server itself instead of using the plugin. The C interface is
`MobileGL/MG_Remote/Server/EmbeddedServer.h` (plain C types, also reachable through `dlsym`); the
library must be a split build.

1. Before loading `libMobileGL.so`, set `MOBILEGL_IPC_ROLE=server` and `MOBILEGL_IPC_DIAL=no`, and
   unset `MOBILEGL_TRANSPORT`, `MOBILEGL_IPC_SERVER_PATH`, `MOBILEGL_IPC_RING_MB` and
   `MOBILEGL_IPC_STAGE_MB`. Running the server in its own process keeps a GPU fault away from your UI.
2. For a windowed server, call `mobilegl_server_display_install_android(callback, user)`, then
   `mobilegl_server_display_attach_android(window, width, height)` with the `ANativeWindow` of your
   `Surface`, and `mobilegl_server_display_detach(timeout_ms)` when the surface is destroyed.
3. Call `mobilegl_server_serve_inprocess("<endpoint>")` on a thread of its own (it blocks). Stop it
   with `mobilegl_server_stop_inprocess()`, then `mobilegl_server_display_uninstall(timeout_ms)`.

Clients connect with `MOBILEGL_TRANSPORT=spawn`, `MOBILEGL_IPC_CONTROL=unix:@<name>` (or `tcp://...`)
and, for the windowed server, `MOBILEGL_IPC_SURFACE=server`. The anland Linux-container setup uses
exactly this interface ([linux.md](linux.md#the-anland-setup)). For more offscreen clients at the same
time, package `libMobileGLServer.so` and run it as
`libMobileGLServer.so @<name> --serve --max-sessions <1-64>` from your own service.
`MobileGLServerService.java` and `ServerEnvironment.java` in `android-plugin/` are working examples.

## The trace flavor

`assembleTraceRelease` builds `top.mobilegl.plugin.trace`, a development APK that replays recorded GL
traces and exports the server components so that `adb` can start them by name. It is not a renderer
plugin. See `android-plugin/TRACE_REPLAY.md`.
