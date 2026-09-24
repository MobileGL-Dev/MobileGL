# Design docs and contracts for a server-owned on-screen window (P12 subset)

Paths are relative to `C:\Users\geekerwan\AndroidStudioProjects\FoldCraftLauncher\MobileGL-disagg\` at 9f669e52. **[V]** means I read it in the doc or code. **[I]** means it is my inference.

## 1. Decisions already made about the server-owned window

- **P12 scope was made smaller** [V] (`docs/Disaggregated/ROADMAP.md:46`, `P6-ENDSTATE-REVIEW.md:313`, row 6):
  - The Surface-passing half is deleted: no Java `Surface` → Messenger/AIDL → `ANativeWindow_fromSurface`.
  - The server app owns its own SurfaceView and the client has no window. The docs say minSdk 26's lack of an NDK call to flatten an `ANativeWindow` "no longer matters, since nothing needs flattening".
  - P12 items, verbatim:
    - an appended `WindowKind::ServerOwned`;
    - one `CreateEGLWindowSurface` arm per backend;
    - the four `m_windowHandle` stores plus the `sameHandle` dedup key;
    - DirectGLES `g_Display/g_Surface/g_Context` de-globalisation;
    - the `kEventSurfaceChanged` Width/Height gap;
    - the client stops sending `SetWindowHandle` unconditionally;
    - the server listens on `unix:` and `tcp://`;
    - server lifecycle bound to the Activity;
    - the freezer;
    - multi-context;
    - the FCL env / plugin switch table.
- **Rule H is permanent** [V] (`MobileGL/MG_Remote/CONTRACT-P6.md:43-45`, ENDSTATE:400). A value from the peer's window system never crosses, "not until P12". P12 gives the *server* its own display. So the `AndroidNativeWindow@P12` and `MetalLayer@P12` refusals stay forever. Rule G (CONTRACT-P6:31-37) allows only tokens.
- **Append timing** [V] (CONTRACT-P6:448-449): "Do not append `WindowKind::ServerOwned` in P6… P12 appends it." Today the enum ends at `MetalLayer = 6` (`protocol.fbs:239-250`), so the new value is 7.
- **Where to latch the window** [V] (CONTRACT-P6:458-466; `docs/Disaggregated/notes/p6/a6-audit-rows.md:292`):
  - one substitution at `RegisterEGLWindowSurface` (`MG_Backend/BackendObject.cpp:235`);
  - one refusal at the wire `SetWindowHandle` case (now `Server/ServerLoop.cpp:1232-1241`);
  - one preserve in `ResetEGLRuntimeState` (`:365`);
  - a new `sameHandle` key (`BackendObject_DirectGLES.cpp:962-964`) based on the surface token, not the window pointer (A8-10, rows:307).
- **Other a6 rows owned by P12** [V]:
  - A8-5 and A8-6 (rows:302-303): two `MakeEGLCurrent` paths reach `ActivateEGLSurface` → `SetWindowHandle(surfaceState->Window)` (`BackendObject.cpp:283`) with no wire op in front.
  - A8-7 (rows:304): a pbuffer create or a client release zeroes the window.
  - A8-8 (rows:305): the Width/Height writes at `:206-207` are dead today and become a clobber in P12.
  - A8-15 (rows:312): a client `eglTerminate` must not zero a Service-owned window.
  - Review additions (rows:331-332): the incoming-handle allow-lists at `BackendObject_DirectGLES.cpp:955-960` and `BackendObject_DirectVulkan.cpp:415-419` would refuse every create unless re-keyed.
  - A4-32 (rows:180): `SetWindowHandle` is a redundant blocking round trip.
- **Default-framebuffer shape** [V]: `kEventSurfaceChanged` is the only carrier. `SurfaceReply.defaultFb` is burned (CONTRACT-P6:75).
- **Magma is closer** [V] (ENDSTATE:313): rotation and preTransform are already server-side and keyed on the server's own surface.
- **Where the offscreen boundary came from** [V] (`P6-CONTRACT-DRAFT.md:104-109`; `P6-SPAWN-PLAN.md:36-42`; `CONTRACT-P65.md:7`): pbuffer / surfaceless is the P6 and P6.5 path, and real windows are excluded.
- **P12 acceptance gates** [V] (ROADMAP:46):
  - (a) FCL, same-machine spawn, Adreno 830, both backends in-world; killing the server gives a clean device-lost latch.
  - (b) a client on another machine over TCP reaches in-world on the Redmi, and the P6.5 mandatory numbers are recorded.
- **TLS** is kept as an open P12 item [V] (ROADMAP:279).

## 2. Constraints and gates the new path must satisfy

| Constraint | Source [V] | What it means here |
|---|---|---|
| G1: pull build 0/0/0/0 symbols, `.text` unchanged (`0xa52203`) | ROADMAP:15, :113; CONTRACT-P6:570; STATUS:23 | `BackendObject*.cpp`, `DirectGLES.cpp` (`g_*` at :15814-15817) and `MG_Impl/Init.cpp` compile into the pull build. Edits need `#if MOBILEGL_BUILD_DISAGGREGATED` with the pull arm byte-identical. Precedent: `BackendObject_DirectGLES.cpp:974-977` |
| G2/G14: test names may only be added | ROADMAP:15 | New lanes are additive |
| S5 named refusals | CONTRACT-P6:597; tests `SurfaceControlFrameTest.cpp:366,:388`, `PeerLatchTest.cpp:1250-1257` (`Outcome::Latched`) | AndroidNativeWindow and MetalLayer must stay refused |
| Handshake answers with `Refuse`, never aborts | CONTRACT-P6:249-257; CONTRACT-P65:15 | `RefuseCode` is append-only (`protocol.fbs:74-84`, max `MalformedHello = 8`) |
| Fatal census | CONTRACT-P7:79 | Scan roots include `MG_Backend`. `abort_sites` is a down-ratchet (79). Every `Fatal{Word` needs a `FatalFamilies.def` row; `UnmigratedSurface` already exists (:65) |
| Link ratchet at 171; a rising commit is refused | CONTRACT-P7:138; CONTRACT-P6:704-710 | Server-side code must not start calling client-only symbols |
| fbs is append-only; commit the generated header; flatc-check | CONTRACT-P6:93-102 (`Create*` arity trap); ARCHITECTURE:396-397 | Use builders with explicit `add_*` if appending table fields |
| Control-schema revision | `Protocol/mg_protocol_base.h:54-68` | Bump 2 → 3 and run `protocol_revision_pin.py --write`. `wireFingerprint` moves (`CapsCodec.cpp:480-485`), so the APK and the client redeploy together. An old server sends a new client no Refuse (CONTRACT-P7:182) |
| No branching on link kind above `Transport/` | ARCHITECTURE:451; CONTRACT-P6:640-641 | On-screen vs pbuffer must not be keyed off tcp vs shm |
| One session at a time (`Refuse{Busy}`); one guest per process | CONTRACT-P65:45; CONTRACT-P6:726-734 (D9: `eglTerminate` is process-wide) | — |
| TCP arrival order | CONTRACT-P65:39 | `SurfaceReply.eventHead` is how `SurfaceChanged` delivery is ordered today |
| Control-reply bound | CONTRACT-P6:334-340 | 5000 ms, reset by a `SurfaceProgress` every 250 ms, capped at 120 s. A server blocked waiting for `surfaceCreated` must heartbeat or it reads as "alive but silent" |
| Arm proof (ID-124) and red-once per package (R-16) | CONTRACT-P6:575-577; ROADMAP:12 | Each lane must log which arm actually ran |
| Offscreen stays a gate | CONTRACT-P7:111 (gate 3 runs `--use-pbuffer`) | This path must survive unchanged |
| Don't fix `dev` bugs in passing | ROADMAP:16 | — |

## 3. Documented lifecycle expectations

- **Activity-bound lifecycle** is named but not specified [V] (ROADMAP:46).
- **Freezer** [V]: SPAWN-PLAN:53 and :113 say the full handling is P12's and the diagnostic is P6's; `P6-CONTRACT-DRAFT.md:128-129` says the same. CONTRACT-P6:329-340 rules that a frozen peer gets a named diagnostic, not a device-lost latch. No doc defines what P12's "full handling" is.
- **Death** [V]: the latch comes only from `PeerHungUp()`, never from a timeout (CONTRACT-P6:310-312, :371-378). `MOBILEGL_IPC_RESPAWN=1` is a named refusal (CONTRACT-P6:312).
- **Today's form** [V]:
  - a foreground `dataSync` Service in `:mglsrv` (trace manifest:11-15) that exec's the supervisor, which forks one child per session (CONTRACT-P65:45, :49; ROADMAP:103);
  - clean client close is a half-close (CONTRACT-P65:47);
  - `onDestroy` destroys only the supervisor, so session children leak. This finding is explicitly deferred "→ P12" (`notes/p65/code-review-findings.md:9`; `MobileGLServerService.java:96-101`).
- **EGL teardown hazard** [V]: "Espryt teardown kills the process-default EGL display, so the bench must never share a process with the POST UI" (main `AndroidManifest.xml:25-28`).

## 4. What the docs require be recorded when this lands

- [V] ROADMAP P12 row status plus a milestone entry (the pattern at :230-231); `CURRENT_STAGE_STATUS.md` on every merge, push or device window (:3); red-once per package in notes (CONTRACT-P7:138); the five-part stage-exit gate (ARCHITECTURE §13.2); one Codex review at stage close (ROADMAP:17).
- [V] A comment row in `mg_protocol_base.h` plus the pins json.
- [I] Dispositions for a6 rows A8-3…A8-15 and for CONTRACT-P6 §7.2 D8.
- [I] By precedent: a new `MG_Remote/CONTRACT-P12.md` and an integrator decision log (the ID-P7-n pattern).
- [I] Rewrite ARCHITECTURE §15.2. Do not edit ENDSTATE; it says it is frozen (:22).

## 5. Contradictions between docs and the tree that matter here

1. **Stale P12 path.** `ARCHITECTURE.md:526` still describes the deleted Surface → AIDL → `:mgl` → `ANativeWindow_fromSurface` path. `protocol.fbs:258` still says "Android transfers the window out of band".
2. **Process name.** The docs say `:mgl` (ARCHITECTURE:526, CONTRACT-P6:667, SPAWN-PLAN:122). The tree uses `:mglsrv` (trace manifest:15).
3. **The D8 whitelist and the client-side mirror did NOT land** [V]:
   - `SurfaceOpCodec.cpp:113-123` and `:165-185` still accept X11, Win32Hwnd and None and copy `nativeToken`.
   - `ServerLoop.cpp:1050-1058` casts it to `void*`.
   - The string `"X11@P12"` exists only in CONTRACT-P6:440.
   - `BackendObject_Remote.cpp:215` still posts `SetWindowHandle` unconditionally, with no client refusal.
   - Consequence: a WSL client that announces X11 sends an XID straight into Android's `eglCreateWindowSurface`.
4. **Abort vs latch.** ROADMAP:46 says the first `eglCreateWindowSurface` "aborts" the server. Since Ph it latches the session instead (`ServerMain.cpp:363` arms the latch; `SurfaceOpCodec.cpp:215-219`).
5. **IPC order.** ROADMAP:19 has P6.5 → Ph → **P12** → P9. `CURRENT_STAGE_STATUS.md:73` and HANDOFF:22 have P9 → P10 → P11 → P12.
6. **Rule H.** `CURRENT_STAGE_PROGRESS.md:244` describes P12 as "real windows cross processes", which Rule H contradicts.
7. **Drifted line numbers.** The DirectGLES `SurfaceChanged` site is now `DirectGLES.cpp:15939-15950`, not :15793-15805. It still posts format only with W/H = 0, so the client keeps the 512×512 placeholder (`MG_Impl/Init.cpp:30-38`). The ServerLoop citations moved (:875-882 → :1050-1058; :1015 → :1232). The BackendObject.cpp citations still resolve.
8. **Death knobs.** ARCHITECTURE:533 describes RESPAWN / IDLE_EXIT as working; CONTRACT-P6:305-312 says they are parsed nowhere.

## OPEN QUESTIONS / RISKS

1. **[I, critical] Process topology.** "Server app owns a SurfaceView, nothing to flatten" only holds if rendering runs inside the ART process that owns that Surface. Today's server is an exec'd native child forked per session, and on minSdk 26 it cannot get that `ANativeWindow`. The options are:
   - run the session in-process (via JNI), which runs into D9 (`eglTerminate` is process-wide) and the manifest's warning that Espryt teardown kills the process-default EGL display;
   - have the child render into AHardwareBuffers and pass them over SCM_RIGHTS for the Activity to present;
   - some other approach. No doc decides this.
2. **Choosing the mode per session is undecided.** Options: a supervisor flag, a `Hello`/`Welcome` field, or a caps bit. Any schema field means another revision bump.
3. **Same-phone gate (a):** if the server app is in the foreground, FCL is backgrounded, and neither input forwarding nor the freezer's effect on the *client* is in any doc.
4. **Server-initiated resize or rotation** has no RPC to carry `eventHead` (CONTRACT-P65:39). The Width/Height `SurfaceChanged` would only be seen at the client's drain points.
5. The freezer's "full handling" is undefined.
6. [I] The `dataSync` foreground-service type may be time-limited on newer Android versions.
7. Multi-context and `g_*` de-globalisation are probably unnecessary while there is one session per process.

## MINIMAL CHANGE LIST

- **`MG_Remote/Protocol/protocol.fbs`:** append `ServerOwned = 7` to `WindowKind`; regenerate `generated/protocol_generated.h`; any mode field is appended the same way.
- **`Protocol/mg_protocol_base.h:68`:** `MOBILEGL_PROTOCOL_CONTROL_REVISION` → 3, plus a comment row; update the pins json.
- **`Protocol/SurfaceOpCodec.cpp`:**
  - update the static_assert (:58) and the `IsOutRange` bound (:178-179);
  - accept `ServerOwned` only with `nativeToken == 0` and only on a display-owning server; otherwise a named refusal;
  - land D8 (refuse X11, Win32Hwnd and None).
- **`MG_Remote/Client/BackendObject_Remote.cpp:210-216`:**
  - drop `ServerSetWindowHandle`;
  - send `CreateWindowSurface` as `ServerOwned` when the server announces a display;
  - otherwise keep today's offscreen path.
- **`Server/ServerLoop.cpp`:** in `UnpackWindowHandle` (:1050), substitute the server's window for `ServerOwned`; `SetWindowHandle` (:1232) becomes a named no-op or refusal.
- **`MG_Backend/BackendObject.cpp`:** substitute at :235, preserve at :365, ignore client extents at :206-207. All under `#if` so G1 holds.
- **`BackendObject_DirectGLES.cpp:955-964`:** new allow-list and a `sameHandle` keyed on the surface token.
- **`BackendObject_DirectVulkan.cpp:415-419`:** the equivalent allow-list change.
- **`DirectGLES.cpp:15939-15950`:** fill Width/Height from `eglQuerySurface` when the server owns the window.
- **android-plugin (trace flavour):**
  - add an Activity with a SurfaceView and the window hand-off chosen in Open Question 1;
  - add a mode extra on `MobileGLServerService`;
  - fix the `onDestroy` child leak.
- **Tests:** keep S5. Add red-once tests for:
  - `ServerOwned` with a non-zero token;
  - `ServerOwned` sent to an offscreen-only server;
  - the X11 whitelist.

  Add an arm-proof log line `window=server-owned|pbuffer`.
- **Docs:** see §4, and fix the contradictions in §5, items 1, 2, 5 and 6.