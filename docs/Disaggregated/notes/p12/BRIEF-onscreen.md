# P12 subset "on-screen server window" — implementation brief (2026-09-23)

Base: `feat/disaggregated@9f669e52` (P7 closed). Package tree `~/w7/p12-onscreen`, branch `p12/onscreen`,
build dirs `build-split` (disaggregated, ITEST tcp port 40813) and `build-linux` (pull build, for G1), both
already built at base. G1 base: `.text` 0xa52203, symbols `~/w7/logs/p12/pull-syms-base.txt`.
Gate: `bash ~/w7/notes/p12/gate.sh ~/w7/p12-onscreen <tag>`.

## User rulings (binding)

1. The Android server APK creates an ANativeWindow (its own SurfaceView) and renders the IPC render stream
   ON SCREEN on it. **Both rendering paths stay available**: the existing offscreen (pbuffer) path and the
   new on-screen path.
2. **Only one of them is active at a time**, decided at server initialization / context creation.
3. Topology: **in-process server** — the display Activity's own app process runs the TCP server on a thread
   and renders straight into its SurfaceView; sessions run sequentially in that process. (Not fork + AHB.)
4. Client side: **the client opts in via a new `WindowKind::ServerOwned`** (control protocol revision 3,
   client env knob). In this mode the client can be **fully headless**: it asks the server to create the
   real (window) context/surface and gets the surface geometry etc. from the server — as if the client were
   rendering directly on the server's GPU and screen.

## Design

### D1 Client knob and headless window surfaces
- New IPC knob `MOBILEGL_IPC_SURFACE=offscreen|server` (default `offscreen`), added to `MG_Config::IpcTable`
  (`Config.h`, "a later phase's knob is added HERE") + `ConfigLoader.cpp`, printed in the IPC config log line.
  Meaningful only when the client talks to a remote server (spawn/tcp); ignored (logged) otherwise.
- With `server`: the client EGL layer's `eglCreateWindowSurface` / `eglCreatePlatformWindowSurface` accept ANY
  native window, **including NULL** (today NULL is `EGL_BAD_NATIVE_WINDOW`, `EGLImpl.cpp:130-133`) — a headless
  client has no window. `BackendObject_Remote::CreateEGLWindowSurface` then sends **one** `CreateWindowSurface`
  with `WindowKind::ServerOwned`, `nativeToken = 0`, width/height = the size the client asked for (the
  existing `WindowHandle.Width/Height` from `EGL_WIDTH/EGL_HEIGHT` attribs; 0/0 = "the server's own size"), and
  **does not send `SetWindowHandle`**. The client's native window value never reaches the wire (Rule G/H).
- `eglCreatePbufferSurface` stays a pbuffer in both modes (offscreen). See D4 for the one-mode-per-session rule.
- With `offscreen` (default) nothing changes, byte for byte.
- Geometry flows back: when a `kEventSurfaceChanged` with non-zero W/H arrives for the session's server-owned
  surface, the client updates the EGL state's surface size (so `eglQuerySurface(EGL_WIDTH/HEIGHT)` answers the
  server's real size; `state->ResizeSurface` as used at `EGLImpl.cpp:724`) in addition to the default-FB
  reallocation `ApplySurfaceChangedToClient` already does (`ClientSession.cpp:306-347`). Make sure the client
  has the size before `eglCreateWindowSurface` returns (the create path already drains events,
  `BackendObject_Remote.cpp:224`).

### D2 Wire
- Append `ServerOwned = 7` to `WindowKind` (`protocol.fbs:239-250`; append-only), regenerate + commit the
  generated header, bump `MOBILEGL_PROTOCOL_CONTROL_REVISION` 2 -> 3 (`mg_protocol_base.h`, with its comment
  row), `protocol_revision_pin.py --write`. wireFingerprint moves: APK and host redeploy together.
- Codec (`SurfaceOpCodec.{h,cpp}`): encoder maps the client's server-owned request to `ServerOwned`; decoder
  accepts `ServerOwned` only on `CreateWindowSurface` (on `SetWindowHandle` it is a named refusal), only with
  `nativeToken == 0` (non-zero = `ProtocolCorruption`, latched like the other peer-corruption rows).
  Update the static_assert / out-of-range bound. Keep `AndroidNativeWindow@P12` / `MetalLayer@P12` refusals.
- Do NOT add a `WindowBackend` enumerator in `MG_Backend/BackendObject.h` (pull build, G1). Carry the
  server-owned request in the MG_Remote frame with a MG_Remote-local marker; on the server it becomes
  `WindowBackend::Android` + the server's own `ANativeWindow*`, substituted ON THE SERVER at apply time. No
  pointer ever crosses the wire.
- D8 (X11/Win32/None whitelist, CONTRACT-P6 §7.2) is OUT of scope (separate follow-up).

### D3 Server display (the on-screen server's window)
- New `MG_Remote/Server/ServerDisplay.{h,cpp}`: process-wide holder of the server-owned window, set only by the
  in-process display server (Android). API sketch: `Attach(ANativeWindow*)` (acquire), `Detach()` (blocking,
  see D6), `HasDisplay()`, a geometry request hook (native -> Java up-call: `SurfaceHolder.setFixedSize(w,h)`
  for w,h > 0, `setSizeFromLayout()` for 0/0), and `AcquireFor(w,h, deadline)` that waits (bounded, ~10 s,
  wakes on detach/shutdown) until a window is attached and reports the requested size (or any size for 0/0).
  Linux/host and the exec'd supervisor have no display: `HasDisplay()` is false.
- `ServerLoop` `CreateWindowSurface` arm, for a ServerOwned frame: no display -> reply failure with a distinct
  result and a named log `Refuse ServerOwned: this server owns no display` (NOT a latch — it is a
  configuration error, not peer corruption); otherwise substitute the window and call the backend's
  `CreateEGLWindowSurface` exactly as monolith Android does (Register -> Activate -> SetWindowHandle ->
  InitWindowSurface). Log an arm proof once per surface: `MG_Remote server: surface=window WxH owner=server`
  (and `surface=pbuffer WxH` on the pbuffer arm). The apply thread may wait for the window; the existing
  `SurfaceProgress` heartbeat must keep the client's reply budget alive while it does.
- Reporting size: Magma already publishes swapchain extent in `OnSurfaceChanged` (`SwapchainObject.cpp:323-339`).
  Espryt publishes format only (`DirectGLES.cpp:15939-15950`): publish W/H (via `QueryCurrentSurfaceSize`) for a
  window surface in the disaggregated build only — `DirectGLES.cpp` is in the pull build, so guard it
  (`#if MOBILEGL_BUILD_DISAGGREGATED` or equivalent) and keep G1 byte-identical. Consider whether it must be
  limited to server-owned windows (publishing W/H switches the client to the reallocating arm).
- Espryt: `sameHandle` dedup (`BackendObject_DirectGLES.cpp:962-967`) keys on surface + window; with the
  server's pointer it dedups correctly. A new surface token re-creates the context (existing behaviour).
- Magma: a new renderer on the SAME ANativeWindow must not be created while the old renderer's VkSurface/
  swapchain still holds it (`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`); check `BackendObject_DirectVulkan.cpp:337-352`
  ordering and fix if needed (guard for G1 if that file is in the pull build).

### D4 One mode per session ("only one active", decided at context creation)
- A session's surface mode latches at its FIRST surface creation: ServerOwned -> on-screen; pbuffer -> offscreen.
  A later surface of the other kind in the same session is refused by name (`SurfaceModeMismatch`, reply
  failure, logged on both sides, no latch). A server with no display serves only offscreen sessions.
- Server-level: the offscreen supervisor (exec'd, `MobileGLServerService`) and the on-screen display server
  (in-process, `MobileGLDisplayActivity`) are mutually exclusive on the device (D8).

### D5 In-process server
- `RunSession` (`ServerMain.cpp:268-638`, `[[noreturn]]`, `_exit` on every path) becomes returning (an exit
  code); every fork site becomes `_exit(RunSession(...))` so the forked shapes are unchanged.
- A new exported entry for the in-process server (e.g. `mobilegl_server_serve_inprocess(endpoint, ...)`), same
  env preconditions as `mobilegl_server_main` (`:1035-1049`), TCP only, `--serve` semantics with THREADS
  instead of fork: pre-auth before hand-off unchanged, one session at a time, an authenticated second Hello
  while a session is live -> `Refuse{Busy}`. `TcpSupervisor`'s fork body closes fds per fd-table
  (`:990-995, 1009`); the thread hand-off must not. Reuse TcpSupervisor with a pluggable hand-off if tractable.
- `FatalFunnel`: add `ResetSessionLatch()`; the in-process server arms the latch for every session and resets
  it between sessions. Unarmed `SessionFail` still aborts (documented: in-process mode loses crash isolation).
- Backend pinned for the process lifetime: an explicit extra/env, else the first session's backend becomes the
  pin (Espryt and Magma must never share a process, `BenchService.java:12-17`); mismatch -> the existing
  pinned-backend refusal (`:371-375`).
- Everything else per `map-apk-process` §4 (config init per session, server role, PipeStats restartable).
- Must be testable on the Linux host: run the in-process entry on a thread in a test (pbuffer sessions) and
  prove two sequential sessions, latch reset (second session serves after the first latched), and Busy.

### D6 Window lost
- Java `surfaceDestroyed` -> JNI `Detach()` blocks (bounded ~3 s) until the apply thread has released the
  backend surface (post onto the apply thread; `ReleaseEGLResources`-style), then the live on-screen session is
  latched by name (new `FatalFamilies.def` row, e.g. `ServerWindowLost`) so the client gets a clean
  device-lost. The display server keeps listening; the next on-screen session waits for a window (D3) or is
  refused by name. `surfaceCreated` re-attaches. A session that never had a window is unaffected.

### D7 Android (trace flavor)
- New `MobileGLDisplayActivity` (trace flavor), `android:process=":mglwin"`, exported like the service,
  fullscreen SurfaceView, `FLAG_KEEP_SCREEN_ON`, `configChanges` like `TraceReplayActivity`,
  `android:hardwareAccelerated="false"` (keeps HWUI off EGL: Espryt teardown `eglTerminate`s the default
  display, `main/AndroidManifest.xml:25-28`). Extras: `listen`, `token`, `env` (same `K=V;K` grammar as
  `MobileGLServerService.applyEnvironment` — share the helper), optional `backend`. Applies env with
  `Os.setenv/unsetenv` (ROLE=server, DIAL=no, LOG_FORWARD=1, scrub list, TOKEN, LOG_FILE_PATH) BEFORE
  `System.loadLibrary("MobileGL")`. Aspect-fit (letterbox) the SurfaceView when a fixed buffer size is set.
- JNI in libMobileGL (Android + disaggregated only, next to `DriverPostJni.cpp` in `CMakeLists.txt`):
  start/stop the in-process server thread, attach/detach the window (`ANativeWindow_fromSurface`, acquire/
  release), the geometry up-call.
- Readiness line in logcat (`listening on tcp://...`) like the supervisor's.

### D8 Only one server active on the device
- The Activity on start stops `MobileGLServerService`; the service on start kills `:mglwin`
  (`ActivityManager.getRunningAppProcesses` + `Process.killProcess`, same uid) and waits for it to go.
- Forked session children get `prctl(PR_SET_PDEATHSIG, SIGKILL)` (+ the getppid race check) so they die with
  their supervisor (fixes the documented child leak, `notes/p65/code-review-findings.md:9`).
- The in-process listen retries `EADDRINUSE` for up to ~5 s (the previous server may still be dying).

### D9 Tools
- `tools/trace_replay/tcp_device_server.py --surface window|pbuffer` (default pbuffer = today): window starts
  the Activity (`am start -n <pkg>/top.mobilegl.plugin.MobileGLDisplayActivity` + the same extras).
- Linux glws `tools/trace_replay/apitrace_glws_egl.cpp`: `MOBILEGL_TRACE_SURFACE=window` on Linux creates
  `eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, {EGL_WIDTH,w,EGL_HEIGHT,h})` (only valid with
  `MOBILEGL_IPC_SURFACE=server`; otherwise the create fails by name); `resize()` destroys + recreates at the new
  size. So `trace_replay --window-surface` + `MOBILEGL_IPC_SURFACE=server` replays a trace on the phone screen.

## Constraints (all must hold)
- G1: pull build `.text` 0xa52203 and 0 symbols added/removed. Every edit to a file compiled into the pull
  build (`MG_Backend/*`, `MG_Impl/*`, `MG_State/*`, `DirectGLES.cpp`, ...) must be guarded so the pull build is
  byte-identical. Measure with the gate.
- fatal_census (79, down-ratchet; every new `Fatal{Word` needs a `FatalFamilies.def` row), link ratchet 171
  monotone, spawn_lane_parity 0 errors (device-only or new arms must be named there), protocol revision pin,
  wire_declines_audit, hygiene (stdio grep in MG_Backend/MG_State, gen_pipe clean, doc citations strict).
- fbs append-only; no branching on link kind above `Transport/`; handshake refusals are `Refuse`, never aborts.
- Every existing lane stays green (base numbers at 9f669e52: unit 2568, integration-split 322, spawn 239,
  tcp 243, magma-split 125, magma-spawn 104, magma-tcp 89, magma-full-split 558, integration-gpu 2188 x 2).
- Red-once per behaviour (a test that is red without the fix and green with it), noted in the report.
- Commits: single-line `[Scope, Scope] (P12): <what>` (e.g. `[MG_Remote, MG_Test] (P12): ...`), NO trailers of any
  kind (no Co-Authored-By). Several focused commits are fine.

## Working rules
- Edit files through `\\wsl.localhost\Arch\home\swung\w7\p12-onscreen\...` with Read/Edit/Write (UNC is writable).
- Run commands as `MSYS_NO_PATHCONV=1 wsl -d arch --cd /home/swung -- bash -lc '<cmd>'`. The Windows command
  line mangles heredocs and nested quotes: put anything non-trivial in a script file (Write via UNC, then run
  it; strip CR with `sed -i 's/\r$//'` if needed). Long jobs: `setsid nohup ... &` and poll the log.
- git ONLY inside `/home/swung/w7/p12-onscreen` via `wsl`. NEVER touch `~/w7/pipe` or the Windows checkouts.
  Never `git lfs pull`. Never push.
- The phone (Redmi 2f7cbe2e) is shared: any adb work runs under `flock -w 3600 /home/swung/w7/locks/2f7cbe2e.lock <cmd>`.
