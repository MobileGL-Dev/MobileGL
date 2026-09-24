# Client-side surfaces in split mode: map, open questions and change list (commit 9f669e52)

Tags used below: **[V]** means I read it in code. **[I]** means it is my inference. Paths are relative to `MobileGL-disagg/`.

## 1. How the client's EGL layer creates surfaces in split mode

**Entry points.** These are `MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp:12-15` (`eglCreateWindowSurface`), `:49-51` (`eglMakeCurrent`), `:112-117` (`eglSwapBuffers`) and `:120-122` (`eglCreatePbufferSurface`). Each forwards to `MG_Impl/EGLImpl/EGLImpl.cpp`. [V]

**Window surface** (`EGLImpl.cpp:116-160`). [V]
- A null native window is refused with `EGL_BAD_NATIVE_WINDOW` (`:130-133`). A surfaceless client therefore cannot reach this path at all.
- The `WindowHandle` is built as `{Backend = DetectWindowBackend(), Handle = window, Width/Height = attrib or 0}` (`:135-140`).
- `DetectWindowBackend` is a compile-time `#if` chain (`:59-71`): Android, then Apple (MetalLayer), then Win32, then Linux (X11).
- Next come `state->CreateWindowSurface` (`:142`) and then `backendObject->CreateEGLWindowSurface` (`:153`). `CreatePlatformWindowSurface` (`:673-717`) follows the same shape.
- The client's EGL state records a window surface with **no size** (`MG_State/EGLState/Core.cpp:811-839`; defaults `Width/Height = 0` at `Core.h:181-182`). `eglQuerySurface(EGL_WIDTH)` answers from that record (`Core.cpp:1076-1082`), so it returns 0 for a window unless `ResizePlatformWindowSurface` has run (`EGLImpl.cpp:719-739`).

**Pbuffer** (`EGLImpl.cpp:453-476`). The size defaults to 1x1 when no attribute is given (`:458-459`). It then goes through `backendObject->CreateEGLPbufferSurface(surface, w, h)` (`:469`). [V]

**Remote backend** (`MG_Remote/Client/BackendObject_Remote.cpp`). [V]
- `CreateEGLWindowSurface` (`:210-226`):
  - waits for the apply thread to catch up (`:214`);
  - calls `Server::ServerSetWindowHandle(handle)` **unconditionally** (`:215`);
  - calls `ServerCreateEGLWindowSurface` (`:216`);
  - calls `DrainPublishedEvents()` (`:224`) so the server's surface-changed event is applied immediately;
  - finally runs the base-class bookkeeping (`:225`).
- `CreateEGLPbufferSurface` (`:237-244`) has the same shape without the window handle.
- `MakeEGLCurrent` (`:246-277`) forwards first, then pumps the control plane, drains events and refreshes caps (`:266-275`).
- `SwapEGLBuffers` is deliberately **not forwarded** (`:279-288`). Present travels as a record.
- `SetEGLSwapInterval` is forwarded (`:290-298`).

**Forwarders** (`MG_Remote/Server/ServerLoop.cpp`). [V]
- `PackWindowHandle` (`:1023-1028`) stores `windowBackend = (Int)handle.Backend` and `nativeToken = (Uint64)handle.Handle`, plus width and height.
- `ServerCreateEGLWindowSurface` (`:1304-1311`), `ServerCreateEGLPbufferSurface` (`:1323-1331`) and `ServerSetWindowHandle` (`:1399-1404`) all funnel into `RunSurfaceControlFrame`.
- Under spawn or tcp, `RunSurfaceControlFrame` diverts to `m_remoteSink` (`:793-795`). `ClientSession` installs that sink at `ClientSession.cpp:1140`.

**Wire encoding** (`ClientSession::RunRemoteSurfaceControlFrame`, `ClientSession.cpp:1205-1413`). [V]
- It mints a sequence number (`:1212-1214`) and calls `EncodeSurfaceOpFrame` (`:1217`). An encode error returns `MOBILEGL_ERR_UNSUPPORTED` (`:1218-1226`).
- `EncodeSurfaceOpFrame` (`Protocol/SurfaceOpCodec.cpp:125-148`) maps the op to a `WindowKind`:
  - a pbuffer op is written as `WindowKind::Pbuffer` (`:133-135`);
  - a window or SetWindowHandle op goes through `WireWindowKindForWindowBackend` (`:102-111`): Android→`AndroidNativeWindow`, X11→`X11`, MetalLayer→`MetalLayer`, Win32→`Win32Hwnd`, Unknown→`None`;
  - `nativeToken` is copied raw into `SurfaceOp.nativeToken` (`:142-144`; schema at `protocol.fbs:239-268`).
- **There is no client-side refusal.** An Android client's `ANativeWindow*` encodes cleanly and is only refused on the server. This contradicts CONTRACT-P6 §7.3 (`MG_Remote/CONTRACT-P6.md:451-456`).

**Reply wait and cold-start budget.** [V]
- `ControlReplyBudgetMs` (`ClientSession.cpp:124-134`): the three bring-up ops (CreatePbufferSurface, CreateWindowSurface, MakeCurrent) get `max(MOBILEGL_IPC_CONTROL_TIMEOUT_MS=5000, MOBILEGL_IPC_COLD_START_MS=20000)` until the first successful MakeCurrent sets `m_serverBackendWarm` (`:1411`). Knobs are parsed at `ConfigLoader.cpp:432-433`.
- Before sending, the client flushes and waits out its command prefix (`:1238-1244`).
- Each `SurfaceProgress` restarts the budget, up to `max(budget, kBarrierTimeoutMs)` (`:1265-1276`, `:1352-1375`). A progress message naming the wrong seq is a protocol mismatch.
- A `CapsSnapshot` arriving mid-wait is adopted (`:1341-1351`), and `Fatal` latches device-lost (`:1376-1390`).
- After a `SurfaceReply`, the client waits for event delivery up to `eventHead` (`:1405-1408`).
- `SurfaceReply.defaultFb` is dead: the encoder writes 0 (`SurfaceOpCodec.cpp:192-193`) and the decoder never reads it (`:199-205`).

**Default framebuffer size** (`MG_Impl/Init.cpp:26-50`). [V]
- The client's framebuffer 0 is a 512x512 RGBA8 placeholder with `Depth32FStencil8` attachments.
- The consumer is `ApplySurfaceChangedToClient` (`ClientSession.cpp:306-347`), reached from the `kEventSurfaceChanged` case of `DrainEventRing` (`:407-420`). The payload is `EventSurfaceChangedHead{Width, Height, InternalFormat, ...}` (`Transport/EventRing.h:91-100`).
  - When Width and Height are non-zero, it reallocates all three attachments at that extent (`:316-338`).
  - When they are zero, it changes formats only and **the 512x512 extent is kept** (`:339-346`).
- DirectGLES posts **format only** (`MG_Backend/DirectGLES/DirectGLES.cpp:15938-15951`). Magma posts the swapchain extent (`DirectVulkan/Renderer/SwapchainObject.cpp:323-339`).
- So under Espryt the client's default FBO is 512x512 **even for pbuffers today**.
- The event never updates the client EGL state's `SurfaceObject.Width/Height`.

**Present.** [V]
- The `MGPPresent` record carries only `FrameSerial` (`MG_Pipe/MGPipeTypes.h:1768-1771`), emitted at `EmitTables.cpp:1116-1132`. There is no surface identity in it.
- On the server, DirectGLES calls `eglSwapBuffers(g_Display, g_Surface)` (`DirectGLES.cpp:16842`). The swap therefore hits whatever surface is current when the record is applied.

## 2. How the Linux/WSL client replays traces today

**glretrace windowing layer.** The build uses its own EGL layer, `tools/trace_replay/apitrace_glws_egl.cpp`, compiled into `mobilegl_trace_glretrace_common` (`tools/trace_replay/CMakeLists.txt:206-249`). [V]
- It uses `MOBILEGL_TRACE_SURFACE=window` to choose between window and pbuffer (`:321-324`, `:432-438`, `:592`, `:616`).
- **The window path exists only under `__APPLE__`** (`:443-466`). On Linux it always calls `eglCreatePbufferSurface(w, h)` (`:467-476`), even when the config was chosen with `EGL_WINDOW_BIT`.
- `resize()` destroys and recreates the pbuffer (`:387-400`). A trace can therefore produce several pbuffers over its lifetime.

**Knobs.** [V]
- `trace_replay_core.cpp:169` sets `MOBILEGL_TRACE_SURFACE` from `request.usePbuffer`, which defaults to true (`trace_replay_core.hpp:58`).
- The CLI flags are `--window-surface` and `--pbuffer-surface` (`trace_replay_cli.cpp:22-24`, `:142-145`).
- glretrace is **not** run with apitrace's own `--pbuffer` flag. The surface choice is made entirely inside the custom layer.

**Transport setup.** [V]
- `run_trace_case.cmake:46-60` sets `MOBILEGL_TRANSPORT`, `IPC_SERVER_PATH`, `IPC_CONTROL`, `DATA=stream`, `TOKEN`, `REQUIRE_SAME_BUILD` and `LOG_FORWARD`.
- ctest adds `EGL_PLATFORM=surfaceless` for DirectGLES (`tools/trace_replay/CMakeLists.txt:440-447`).
- `run_tcp_matrix.py:118-139` sets `MOBILEGL_TRANSPORT=spawn` plus a `tcp://` endpoint, and adds run-ahead and verb-barrier when requested.
- The local server fixture `scripts/ci/tcp_server_fixture.py:60-77` pins `EGL_PLATFORM=surfaceless`, removes `DISPLAY` and `WAYLAND_DISPLAY`, and runs `<server> <endpoint> --serve`.

**Integration harness** (`MG_IntegrationTest/Harness/HeadlessGL.cpp`). [V]
- On Linux it always uses a pbuffer (`UseWindowSurface` returns false at `:138-139`; pbuffer at `:276-277`) and forces `EGL_PLATFORM=surfaceless` (`:181-196`).
- On Android it uses a window from an `AImageReader` (`:136-137`, `:268-273`).
- The Split, Spawn, Tcp and TcpDevice arms are all host clients (`MG_IntegrationTest/CMakeLists.txt:2329-2336`).

**Can a Linux client create a window surface at all?** Yes, but only mechanically, and no lane does. [V]
- It would announce `WindowKind::X11` plus an XID.
- **At HEAD, X11 is NOT a named refusal.** `WindowBackendForWireWindowKind` accepts X11 and Win32 (`SurfaceOpCodec.cpp:113-123`). `DecodeWireSurfaceOp` refuses only `AndroidNativeWindow` and `MetalLayer` (`:165-175`, latched at `:217-228`).
- An accepted token is cast straight to `void*` by `UnpackWindowHandle` (`ServerLoop.cpp:1050-1058`) and used by `CreateEGLWindowSurface` / `SetWindowHandle` (`:1126-1138`, `:1232-1241`). That is a wild pointer in the server process.
- P6-ENDSTATE-REVIEW §2.1 (`docs/Disaggregated/P6-ENDSTATE-REVIEW.md:62-79`) and CONTRACT-P6 §7.1/§7.2 "D8" (`MG_Remote/CONTRACT-P6.md:426-449`) specify an allow-list:
  - X11 → `Fatal{UnmigratedSurface,"X11@P12"}`;
  - `None` on a window op is a Fatal;
  - `nativeToken != 0` is a Fatal;
  - reason: "Rule G": a value whose meaning comes from the peer's window system never crosses the wire.
- **None of D8 is implemented.**
- Android clients hit `AndroidNativeWindow@P12`. That is why the device lanes require `--use-pbuffer` (`run_android_retrace_local.py:604-611`, `trace_replay_jni.cpp:180-187`, `tools/device_bench/p6/README.md:117-120`).

**Surfaceless client and "show my framebuffer on the server's screen": options.**

- **A. Server-side policy, no client or wire change.** [I]
  - The APK (session config) says "present". The `CreatePbufferSurface` case of `ApplySurfaceControlFrame` (`ServerLoop.cpp:1144-1151`) then creates the session's surface as a **window surface on the APK's `ANativeWindow`**, after `ANativeWindow_setBuffersGeometry(win, frame.width, frame.height, fmt)`.
  - This keeps the client-fixed pbuffer size authoritative. SurfaceFlinger scales the buffer to the view, so client viewports, readbacks and goldens stay valid.
  - The offscreen path is unchanged when the policy is off.
  - The client keeps believing it has a pbuffer (EGL state, swap behaviour).
- **B. Client opt-in, the P12 shape.** [I]
  - Add a new knob, e.g. `MOBILEGL_IPC_SURFACE=server`, in `MG_Config::Ipc`. With it set, `BackendObject_Remote::CreateEGLPbufferSurface` and `CreateEGLWindowSurface` send `CreateWindowSurface` with an appended `WindowKind::ServerOwned` and `nativeToken=0`, and skip `ServerSetWindowHandle`.
  - The server refuses by name if it owns no window.
  - This needs a schema append and revision bump, and should land together with D8.
  - Size: for a pbuffer, the client's width and height are the requested buffer geometry. For a client window, the server's size has to flow back through `kEventSurfaceChanged` with a non-zero W/H, **and** into the client EGL state's surface size, which nothing updates today.
- **Recommendation.** [I] Use B's wire shape, gated by a server switch, with A's sizing rule (the client size wins via `setBuffersGeometry`). Map exactly **one** surface per session to the view: the first pbuffer or window created, or the one current at Present. All others stay offscreen, because an `ANativeWindow` accepts only one connected EGL surface.
- **Multiple pbuffers, verified caveat.** DirectGLES destroys and recreates the native context whenever a different surface is created (`BackendObject_DirectGLES.cpp:969-972`, `:1001-1004`). glretrace's `resize()` recreation therefore already costs a context today.

## 3. Tests and gates that pin today's behaviour

| Test / gate | Location | What it pins | Effect of the goal / D8 |
|---|---|---|---|
| `WindowBackendAndWireWindowKindMapExplicitlyBothWays` | `MG_Test/Wire/SurfaceControlFrameTest.cpp:193-228` | Mapping table, including X11 and Win32 | Needs a ServerOwned row [V] |
| `EveryFramedOpRoundTripsThroughTheWireEnvelope` | `SurfaceControlFrameTest.cpp:230-274` | **X11 plus token 0xDEADBEEF decodes as None** | Breaks under D8 [V] |
| `AnAndroidNativeWindowOnTheWireIsRefusedByName`, `AMetalLayerOnTheWireIsRefusedByName`, `AWireWindowKindThatNamesNoBackendFailsDecodeByName` | `:320-390` | Current refusals | Keep [V] |
| `ServerLoopEglTest.SuccessfulVoidWireOperationsReplyWithSuccessAndOriginalSequence` | `ServerLoopTest.cpp:1713-1740` | `SetWindowHandle` with `WindowKind::None` answers ok | Breaks under D8 [V] |
| PeerLatchTest rows | `PeerLatchTest.cpp:1249-1261`; sender at `:476-495`, token 0x1234 | AndroidNativeWindow and MetalLayer latched | Add X11, Win32, ServerOwned-without-a-window [V] |
| S5 | CONTRACT-P6 `:597` | Requires X11 to be refused and "token never reached `UnpackWindowHandle`" | **Not implemented**; only the AndroidNativeWindow row exists [V] |
| `ph_latch_sites.py` | `scripts/ci/ph_latch_sites.py:65-67` | `"SurfaceOp.windowBackend"` declared unreachable | Still true if the codec keeps mapping [V] |
| `fatal_census` | `scripts/ci/fatal_census_baseline.json:65` | Family `UnmigratedSurface` already present | New refusal strings in that family need no new family [V] |
| `protocol_revision_pin` | `scripts/ci/protocol_revision_pin.py`, pins json | Revisions 1 and 2 only | Any `protocol.fbs` append needs a bump plus `--write` [V] |
| `spawn_lane_parity` | `scripts/ci/spawn_lane_parity.py:34`, `:43` | Split and Spawn case sets must match | A device-only on-screen arm must be named there or kept outside the `integration-*` labels [V] |
| Pbuffer bring-up tests | `ServerSpawnTest.cpp:994-1070`, `ServerLoopTest.cpp:1627`, `:1871`, `:1936`, `:2188` | Pbuffer bring-up only | Stays green (offscreen default) [V] |

## OPEN QUESTIONS / RISKS

1. **D8 is specified but not landed.** Landing it breaks two existing tests (see §3). Should it ship before or together with ServerOwned? [V]
2. **Espryt's size gap.** The client default FBO stays at 512x512 and a client window's `eglQuerySurface` returns 0. The server window's size needs a return path: `kEventSurfaceChanged` with W/H, plus an update of the client EGL state's surface size. [V]
3. **Readback after swap on a window surface is undefined** (`EGL_BUFFER_DESTROYED`). The retrace snapshot timing in `apitrace_fbo_dump.cpp` has not been checked, so goldens could break on the on-screen path. [I]
4. **Present pacing.** A vsync-blocked server swap slows credit return (`MOBILEGL_IPC_PRESENT_CREDIT`). This is good for display, but on-screen benchmark numbers are not comparable to offscreen ones. [I]
5. **Server-side hazards.** `ForgetCurrentTuple` after a surface is (re)created (`ServerLoop.cpp:1136`, `:1150`) and the DirectGLES process-global `g_Surface` interact with any SurfaceView `surfaceDestroyed`/`surfaceChanged` that happens mid-session. [I]
6. **The pbuffer-to-window mapping, if silent, is a semantic lie** (EGL state says pbuffer, the swap really presents). It must be an explicit, logged mode that `runtime_mode_proof`-style gates can prove. [I]

## MINIMAL CHANGE LIST

- **`MG_Remote/Protocol/protocol.fbs:249`**: append `ServerOwned = 7`. Bump `MOBILEGL_PROTOCOL_CONTROL_REVISION` (`mg_protocol_base.h`), regenerate `protocol_generated.h`, run `protocol_revision_pin.py --write`.
- **`MG_Backend/BackendObject.h:569-577`**: add `WindowBackend::ServerOwned` before `WindowBackendCount`. The range check in `ServerLoop.cpp:1039-1048` follows automatically.
- **`Protocol/SurfaceOpCodec.cpp`**:
  - add both mapping rows (`:102-123`);
  - move the out-of-range upper bound from MetalLayer to ServerOwned (`:178-179`) and the `static_assert` (`:58`);
  - implement D8: X11 and Win32 give `UnmigratedSurface` "X11@P12"/"Win32Hwnd@P12"; `None` on a window op and `nativeToken != 0` give `ProtocolCorruption`, with latches next to `:217-235`;
  - refuse a ServerOwned op when the server has no window (named).
- **`Client/BackendObject_Remote.cpp:210-244`**:
  - when the new knob is set on spawn/tcp, send CreateWindowSurface / ServerOwned with token 0 and skip `ServerSetWindowHandle` (`:215`);
  - for a pbuffer, carry the client's w and h as the requested geometry;
  - mirror the refusal of `AndroidNativeWindow`/X11 on the client, before sending (CONTRACT-P6 §7.3).
- **`Config.h` ~`:596` and `ConfigLoader.cpp` ~`:433`**: add the knob (e.g. `MOBILEGL_IPC_SURFACE=offscreen|server`, default `offscreen`) and log it in the IPC line (`:476-484`).
- **`Client/ClientSession.cpp:306-347`**: when a surface-changed event carries W/H for the session's server-owned surface, also resize the client EGL state's surface (`state->ResizeSurface`, as used at `EGLImpl.cpp:724`).
- **`DirectGLES.cpp:15947-15950`**: publish the surface width and height (currently format only). Server side, and needed so the client's default FBO stops being 512x512.
- **Server side** (other slices; listed for completeness):
  - `ServerLoop.cpp:1126-1151`: a ServerOwned arm that substitutes the APK's `ANativeWindow` and calls `setBuffersGeometry`;
  - `BackendObject.cpp:232-238` register point and `BackendObject_DirectGLES.cpp:962-964` `sameHandle` key;
  - a CreateEGLWindowSurface arm per backend (DirectGLES `:955`, Vulkan `BackendObject_DirectVulkan.cpp:424`).
- **Trace tooling** (optional): no glws change is needed for approach B, because it maps pbuffers. Update the help text in `run_android_retrace_local.py:604-611` and `device_bench/p6/README.md:117-120`.
- **Tests**:
  - update `SurfaceControlFrameTest.cpp:193-274` (X11 becomes a refusal, add a ServerOwned row) and `ServerLoopTest.cpp:1713-1740` (drop `SetWindowHandle`/`None` from the "void ok" list);
  - add PeerLatch rows for X11, Win32 and ServerOwned-without-a-window (this is S5);
  - add a unit test proving the client knob's encode, and a device-only TcpDevice on-screen smoke test named in `spawn_lane_parity.py`.