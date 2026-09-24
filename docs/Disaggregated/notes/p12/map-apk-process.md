# MobileGL-disagg: Android APK and process model for an on-screen server path (commit 9f669e52)

Paths below are relative to `C:\Users\geekerwan\AndroidStudioProjects\FoldCraftLauncher\MobileGL-disagg\`. **[V]** means I read it in the code. **[I]** means I inferred it. **[A]** is an Android platform fact I'm confident of but did not check in this tree.

## 1. How the APK is built and packaged [V]
- The app module has `minSdk = 26`, `targetSdk = 34` and ABI default `arm64-v8a` (`android-plugin/app/build.gradle.kts:124-125, 24-31, 152-154`). The library module and CMake also pin API 26 (`build.gradle:43`, `CMakeLists.txt:80`).
- There are two flavors, `plugin` and `trace` (`.trace` suffix), at `build.gradle.kts:171-181`. The app's own CMake is `src/trace/cpp/CMakeLists.txt` and builds `libtrace_replay_runner.so` for both flavors (`build.gradle.kts:204-209`). The `plugin` flavor then strips it (`:229-233`).
- `libMobileGL.so` comes from `implementation(project(":MobileGL"))` (`build.gradle.kts:236`), which is the root `build.gradle`: `com.android.library` using the root `CMakeLists.txt`. The disaggregated/inproc/push switches are passed in `build.gradle:58-66`.
- `useLegacyPackaging = true` (`build.gradle.kts:211-216`) extracts the `.so` files to `nativeLibraryDir`, and the exec path needs that.
- `libMobileGLServer.so` is `add_executable(MobileGLServer ServerEntry.cpp)`, built when `MOBILEGL_BUILD_DISAGGREGATED AND NOT WIN32`. It is PIE, links libMobileGL, has `RPATH $ORIGIN`, and on Android uses `PREFIX lib SUFFIX .so` inside `CMAKE_LIBRARY_OUTPUT_DIRECTORY` (`CMakeLists.txt:1000-1053`).
- `main()` in `MG_Remote/Server/ServerEntry.cpp:19` just calls `mobilegl_server_main`.
- `MOBILEGL_BUILD_SERVER_SPIKE` builds a separate `libMobileGLServerSpike.so` stub (`CMakeLists.txt:1055-1080`), passed only by the trace flavor (`build.gradle:96-110`).
- libMobileGL already carries Android JNI code (`DriverPostJni.cpp` and `DriverBenchJni.cpp`, `CMakeLists.txt:675-680`) and links `android` and `log` (`:866-867`).

## 2. Existing ownership of a Surface: TraceReplayActivity [V]
- The Activity runs in the default process (trace manifest `:67-76`, no `android:process`). It loads `trace_replay_runner` (`TraceReplayActivity.java:22-24`) and creates a `SurfaceView` (`:41-42`).
- If `width`/`height` extras are given, it calls `setFixedSize` (`:82-84`).
- `surfaceCreated` and `surfaceChanged` both call `scheduleReplay`, which starts the replay after 250 ms, only once (`:85-104, 121-134`). **`surfaceDestroyed` is empty** (`:96-98`).
- The session survives Activity recreation through `onRetainNonConfigurationInstance` (`:69-79, 106-109`). However, a recreated Activity's new Surface is never handed to the running replay, because of the `hasStarted` early return (`:122`).
- `configChanges=0x80000f80` (`res/values/config.xml:3-10`) exists precisely so rotation and overlay changes don't recreate the Activity and kill the Surface.
- JNI (`trace_replay_jni.cpp`):
  - A per-process lease blocks concurrent replays (`:125-133`).
  - With `use_pbuffer` there is no window at all (`:187`). Otherwise it calls `ANativeWindow_fromSurface`, or fails if the surface is already gone (`:188-196`).
  - The `:179-183` comment records that a spawned server cannot take a window until P12.
- GLWS (`apitrace_glws_android.cpp`):
  - A global `gNativeWindow` is acquired and released (`:60, 414-425`).
  - `createSurface` chooses `eglCreateWindowSurface` plus `setBuffersGeometry` when there is a window, else a pbuffer (`:198-219`). The config's `EGL_SURFACE_TYPE` follows the same choice (`:337`).
  - The `eglSwapBuffers` result is ignored (`:185-195`).
  - The app's own glws calls `eglTerminate` in `cleanup` (`:315-321`).
- **Conclusion:** today's harness is one-shot. It never re-targets a new Surface; after a destroy it keeps rendering into an abandoned one, and the window reference only prevents a use-after-free.

## 3. Today's server process model [V]
- **Service:** `MobileGLServerService` is an exported foreground service in `:mglsrv` (trace manifest `:11-15`) with a wake lock (`MobileGLServerService.java:26-39`).
  - It runs `ProcessBuilder(nativeLibraryDir/libMobileGLServer.so, endpoint, "--serve")` (`:60-61`).
  - Environment it sets: `ROLE=server`, `DIAL=no`, `LOG_FORWARD=1`; removes `TRANSPORT`, `CONTROL`, `ENDPOINT`, `SERVER_PATH`, `RING_MB`, `STAGE_MB`; sets `TOKEN`, `LOG_FILE_PATH`, `LD_LIBRARY_PATH` (`:63-75`).
  - Extras are `listen`, `token` and `env` (grammar `K=V;K`, `:42-50`). A reader thread pipes stdout to logcat and calls `stopSelf` on exit (`:78-88`). `onDestroy` calls `destroy()` (`:96-102`).
  - It is driven by `tools/trace_replay/tcp_device_server.py:77-79`.
- **`mobilegl_server_main`** (`ServerMain.cpp:1028-1139`):
  - It forces `ROLE=server` (`:1035-1038`).
  - Hard preconditions: `DIAL=no`, else exit 64; the scrub list, else exit 65 (`:1039-1049`).
  - `tcp://` with `--serve` runs `TcpSupervisor::Run` (`:1072-1079`). It authenticates each Hello before forking (`Judge` `:910-976`), then `Fork` (`:978-1010`).
  - In the fork child it closes the listener and every other pending fd and calls `RunSession` with `HandoffSource`. In the parent it calls `CloseLocalCopy` on its copy of the connection.
  - Unix `--serve` forks per session (`:1123-1133`).
  - **Single-session shape (no `--serve`):** the process *is* the session, with no fork. TCP uses `ListenerSource` (`:1096-1105`); unix uses `DataSource::None` (`:1106-1108`).
- **`RunSession` is `[[noreturn]]`** (`:268-638`). Every exit path calls `_exit`: `ExitBeforeAccept` (`:235-239`), and `:412, 429, 437, 455, 482, 637`.
  - Per-session process-global setup: `MG_ConfigLoader::Init`, `Transport=Spawn`, `MGPipeSetServerProcessRole(true)`, `ArmSessionLatch`, `PipeStats::Init`, `ActiveBackendType=hello` with the pinned-backend refusal, then `InitServerRoleForSpawn` (`:356-377`).
  - The log forwarder is a global (`:474-476, 634`).
- Design intent: one process per session means Espryt's process-level `eglTerminate` teardown is isolated. This is D9 (`docs/Disaggregated/ROADMAP.md:103`).

## 4. Could a session run inside an app process (no fork, no exec)?
**What `RunSession` and the server assume** [V unless marked]:
1. **`_exit` on every path** (§3). Inside an app it would kill the Activity at the end of every session, so a returning variant is needed.
2. **Session latch is process-global and never reset.**
   - `g_latchArmed`/`g_latched`/`g_latchCount`/`g_latchedLine` (`FatalFunnel.cpp:99-107, 145`) have no reset function.
   - After one latched session, every later session in that process is declined: `ServerApplyWireSurfaceOp` returns `MISMATCH` (`SurfaceOpCodec.cpp:211`), and the latch also stops the apply loop.
   - Unarmed faults go to `SessionFail`, which calls `std::abort()` (`FatalFunnel.cpp:131-133, 168-180`) and would take down the UI process.
3. **Server role is per process.** `g_serverProcessRole` makes every thread a server thread (`PipeInputs.cpp:357-375`). That is harmless only in a process where nothing else calls libMobileGL. [I]
4. **Backend latch.** `BenchService.java:12-17` says Espryt and Magma can never share a process. Sessions that switch backend within one process are unsafe; pin the backend with `MOBILEGL_BACKEND_TYPE`, which is already enforced at `ServerMain.cpp:371-375`.
5. **D9 / eglTerminate.**
   - Espryt uses `eglGetDisplay(EGL_DEFAULT_DISPLAY)` (`DirectGLES.cpp:16177`).
   - `DestroyEGLContext` calls `eglTerminate` (`:16876-16919`), and it runs at the start of *every* surface init (`InitDisplayAndContext` → `:16159`).
   - The repo claims this terminates HWUI's display (`main/AndroidManifest.xml:25-28`, `BenchService.java:14-16`). [A, medium confidence] AOSP libEGL reference-counts `eglTerminate` per display, which would contradict that. This has to be measured. The ANGLE path loads `libEGL_angle.so` separately (`Loader.cpp:570-580`).
6. **Everything else is reusable** (so no blockers there):
   - `ServerSession::Close` resets everything (`ServerSession.cpp:968-1007`).
   - `ServerLoop::Stop` resets the backend (`ServerLoop.cpp:920-926`; the `CreateBackend` guard is at `:169-172`).
   - `PipeStats` Init/Shutdown can be restarted (`PipeStats.cpp:381-422`).
   - Sockets use `MSG_NOSIGNAL`, so there is no SIGPIPE hazard (`SocketTransport.cpp:466`, `FdPassing.cpp:151`).
   - The server installs no signal handlers (the only `sigaction` is client-side, `PersistentMapTracker.cpp:423`).
   - There is no Android load-time constructor in libMobileGL.
7. **Environment and logging.** Reads from env include `DIAL`, the scrub list, `ROLE` (cached at first log), `LOG_FORWARD`, `BACKEND_TYPE`, `PreAuthKnobs::FromEnvironment` and `MOBILEGL_TEST_*`. In-process these must be set with `Os.setenv` or JNI `setenv` before the first libMobileGL call and before any server thread starts. [I]
8. **Supervisor re-used with threads instead of fork.** The `Fork` body assumes separate fd tables: the child closes the listener and pending fds, and the parent calls `CloseLocalCopy`. Doing either on a thread would close the supervisor's or the session's live fds (`ServerMain.cpp:990-995, 1009`). The single-session shape also closes the listener after Accept (`:402-405`).
9. **Different driver in an app process.** [A] `GraphicsEnvironment` (ANGLE and updatable-driver selection) applies only to app processes, and the driver sees a different process name (the logcat evidence has Adreno printing "Process Name: libMobileGLServer.so"). Numbers measured in the exec'd server may not transfer.

## 5. Options
- **(a) Session inside the app process that owns the SurfaceView** (JNI, background thread, no fork). Feasible.
  - Precedent: `BenchService` (`:bench`, `System.loadLibrary("MobileGL")`, JNI in libMobileGL at `DriverBenchJni.cpp:327`, `System.exit` after each run; `BenchService.java:45-73`).
  - The window is `ANativeWindow_fromSurface` in the same process, which is exactly what ROADMAP P12 assumes: "server app owns its own SurfaceView" (`ROADMAP.md:46`, `P6-ENDSTATE-REVIEW.md:313`).
  - Isolation lost:
    - Per-session address-space reset (D9, backend latch, session latch).
    - Crash containment: an abort or driver crash now kills the display Activity too.
    - Privilege separation between the display UI and code handling network bytes.
- **(b) Exec'd supervisor plus fork child, and pass the Surface to it.** Not feasible at minSdk 26.
  - [A] The NDK API to parcel an `ANativeWindow` (`ANativeWindow_writeToParcel` / `readFromParcel`) is API 34, and minSdk is 26 (verified).
  - [A] An untrusted app cannot register with servicemanager.
  - [A] libbinder refuses use in a fork child of a process that already used binder. The exec'd server does use binder freshly for gralloc and SurfaceFlinger (verified in logcat: `.trace-work/p7w6/bsl-spawn/.../logcat.txt:1778-1814`), but it has no channel over which to receive a Surface.
  - A Java `Surface` is Parcelable (AIDL Activity → `:mglsrv` service) [A], but that only lands the window in `:mglsrv`'s Java process, not in its exec'd children. That collapses into option (a).
- **(c) Child renders into AHardwareBuffers, sends them over a unix socket, app presents.** Feasible at API 26.
  - `AHardwareBuffer_sendHandleToUnixSocket` is API 26 [A], and the repo notes it at `P6-ENDSTATE-REVIEW.md:138`.
  - Presenting at minSdk 26 needs an EGLImage (`EGL_ANDROID_get_native_client_buffer`) and a GL blit in the app, because `ASurfaceTransaction_setBuffer` is API 29 [A].
  - It keeps fork-per-session and D9 intact. But it requires a new buffer-ring/fence protocol, an Espryt default-framebuffer-to-AHB FBO, Magma AHB import, and one copy per frame. It is the largest change.
- **(d) Existing precedent in the tree:** only `BenchService` for in-process work and the one-shot `TraceReplayActivity`. There is no AIDL, Messenger or AHB code.
- **Recommendation: (a)**, in a dedicated process such as `:mglwin` holding the Activity and the session thread. The offscreen FGS, exec and fork path in `:mglsrv` stays unchanged, and the user picks a path by which component they start.
  - Stage 1: one session per process launch (BenchService-style exit), which keeps D9, the backend latch and the latch semantics.
  - Stage 2: sequential sessions in one process, after the latch reset and the `eglTerminate`/HWUI measurement.

## 6. Surface lifecycle
- [A] After `surfaceDestroyed` returns, the window must no longer be rendered to. The callback has to block until the apply thread has released the EGL surface or swapchain; this is the GLSurfaceView pattern.
- Backgrounding destroys the surface and returning creates a new producer. Rotation with `configChanges` gives only `surfaceChanged`.
- While there is no surface, the session must keep consuming records. It should render offscreen and drop presents (or apply backpressure) rather than fail.
- Current backends make re-targeting expensive:
  - Espryt: any different window surface runs `DestroyEGLContext` plus `eglTerminate`, which loses every GL object (`BackendObject_DirectGLES.cpp:962-972`, `DirectGLES.cpp:16159`).
  - Magma: `InitWindowSurface` replaces the whole `VulkanRenderer` (`BackendObject_DirectVulkan.cpp:343-350`).
- [A] Once the Activity is backgrounded, the cached-app freezer can freeze `:mglwin` with no hangup (repo: `P6-CONTRACT-DRAFT.md:128`). Hold a foreground service in `:mglwin` for the duration of a session.

## OPEN QUESTIONS / RISKS
1. Does Espryt's `eglTerminate(EGL_DEFAULT_DISPLAY)` break HWUI in the same process? The repo comment and my AOSP understanding disagree; measure on Adreno and Redmi.
2. A window/context swap that keeps the context (Espryt) and a swapchain-only recreate (Magma) are prerequisites for surviving backgrounding. Without them a surface loss ends the session.
3. `kEventSurfaceChanged` has no width/height, so the client's default framebuffer size would not follow the server window (`ROADMAP.md:46`).
4. The client still sends `SetWindowHandle` unconditionally (`BackendObject_Remote.cpp:215`). Android handles latch by name (`SurfaceOpCodec.cpp:165-171`). X11 and Win32 tokens pass through to `UnpackWindowHandle` (`ServerLoop.cpp:1050-1058`). A server-owned substitution rule is needed.
5. Driver and profile differences in an app process versus the exec'd server (§4.9).
6. Both `MobileGLServerService` and the new Activity are exported with no permission, so any app on the device can start a network listener.
7. A single-session in-process listener cannot answer Busy while a session is live. A thread-based `TcpSupervisor` has the fd-table hazards in §4.8.

## MINIMAL CHANGE LIST
- **`android-plugin/app/src/trace/AndroidManifest.xml`:** add `ServerDisplayActivity` (`android:process=":mglwin"`, `configChanges=@integer/trace_replay_config_changes`, exported, fullscreen). Optionally add a foreground service in `:mglwin`. Leave `MobileGLServerService` as the offscreen path.
- **New `android-plugin/app/src/trace/java/top/mobilegl/plugin/ServerDisplayActivity.java`:**
  - Parse `listen`/`token`/`env` using the `applyEnvironment` grammar. Apply them with `Os.setenv`/`unsetenv` (`ROLE=server`, `DIAL=no`, `LOG_FORWARD=1`, scrub list, `TOKEN`, `LOG_FILE_PATH`, pinned `BACKEND_TYPE`) before `System.loadLibrary("MobileGL")`.
  - Start a server thread calling `nativeServe(endpoint)`.
  - `surfaceCreated`/`surfaceChanged` → `nativeSetWindow(surface, w, h)`. `surfaceDestroyed` → `nativeWindowLost()`, which blocks.
  - On session end, `finish()` and exit the process (stage 1).
- **New `MobileGL/MG_Remote/Server/ServerWindowJni.cpp`** (Android and disaggregated only), listed in `CMakeLists.txt:675-680` under `MOBILEGL_BUILD_DISAGGREGATED`: `ANativeWindow_fromSurface` and acquire/release, plus calls into the new server API.
- **`ServerMain.cpp`:**
  - Make `RunSession` return its exit code; fork sites become `_exit(RunSession(...))`, and `ExitBeforeAccept` returns.
  - Add `extern "C" int mobilegl_server_serve_inprocess(const char* endpoint)`. It runs the same env checks as `:1035-1049`, then the single-session shape (`:1096-1108`) without `_exit`. Stage 2 wraps it in a loop that keeps the listener open.
- **`FatalFunnel.{h,cpp}`:** add `ResetSessionLatch()`, needed for stage 2.
- **Server window slot** (e.g. `ServerLoop.{h,cpp}`):
  - A process-global server-owned `ANativeWindow*` with generation and size. The `CreateWindowSurface` / `UnpackWindowHandle` arm (`ServerLoop.cpp:1126-1138, 1050-1058`) substitutes it when it is set.
  - New internal `SurfaceControlOp`s, `WindowArrived` and `WindowLost`, posted from JNI through `RunSurfaceControlFrame` and executed on the apply thread.
- **Backends (other slices):** an Espryt window swap that keeps the context (`BackendObject_DirectGLES.cpp:948-972`, `DirectGLES.cpp:16159/16225`); Magma swapchain-only recreate (`BackendObject_DirectVulkan.cpp:337-352`); surface-changed width/height; `WindowKind::ServerOwned` plus a client-side change at `BackendObject_Remote.cpp:215`.
- **`tools/trace_replay/tcp_device_server.py`:** a `--window` mode that runs `am start -n …/.ServerDisplayActivity` with the same extras.
- **`build.gradle.kts` / `build.gradle`:** no changes needed.