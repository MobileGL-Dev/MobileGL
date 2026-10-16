# fc — 控制面帧契约草稿（P5f 包 fc；CONTRACT-P5F.md 本体留给集成收口）

> Status: DRAFT, landed in code by package `fc` on branch `p5f/fc`. Authority once integrated: this
> feeds CONTRACT-P5F.md. Until then it supplements CONTRACT-P5C (audit row G4, discharged here) and
> CONTRACT-P5E §2.5 (the wait-before-forwarder discipline, unchanged), and it answers the
> control-plane rows of `docs/Disaggregated/P6-CONTRACT-DRAFT.md` §4 ahead of schedule.
> Code anchors: `MobileGL/MG_Remote/Server/SurfaceControlFrame.h` (the frame),
> `MobileGL/MG_Remote/Protocol/SurfaceOpCodec.{h,cpp}` (the wire codec and the named refusals),
> `MobileGL/MG_Remote/Server/ServerLoop.cpp` (`RunSurfaceControlFrame` /
> `ApplySurfaceControlFrame`).

---

## 1. Ownership

1. A **SurfaceControlFrame is a value**. The poster fills the request fields; ownership of the
   slot's content passes to the channel at publish; the dispatch on the apply thread fills the
   reply half (`ok`, `eglMajor`, `eglMinor`) into the same value; the poster's copy-out happens
   after the blocking handshake returns. No field is a pointer and none may become one — the
   exhaustive constexpr structured binding in `SurfaceControlFrame.h` checks every member
   as an integer or enum. Adding a member without extending the binding fails compilation;
   changing a checked member to a pointer fails its static assertion. Trivially-copyable /
   standard-layout remain separate copyability checks: neither trait rejects pointers.
2. An unnumbered local request (`seq == 0`) is numbered by `RunSurfaceControlFrame`, from 1
   per session. An already numbered wire request retains its client-minted sequence through
   dispatch and reply (P6-CONTRACT-DRAFT table 0); the server never renumbers it.
3. `display` / `surface` / `readSurface` / `context` are Uint64 **tokens**. Inproc they are the
   client's EGL handle values bit-cast — legal only because both roles share a process and a
   driver, and the N-3 tuple bookkeeping compares them by value. Under spawn they are
   client-minted dense tokens (P6-CONTRACT-DRAFT table 0, unchanged). No code may dereference a
   token as a pointer on a path a non-inproc transport can reach.
4. `windowBackend` + `nativeToken` + `width`/`height` carry a `WindowHandle` as values. The frame
   names the **backend** enum, never the wire's `WindowKind`; the two enums meet only inside
   `SurfaceOpCodec.cpp`'s explicit tables (`Surfaceless`/`Pbuffer` are surface shapes and answer
   "no backend" there). A `nativeToken` whose backend is Android is an `ANativeWindow*` — an
   address in the client's process — and may never cross (§3.5). MetalLayer carries a
   `CAMetalLayer*` and has the same restriction.

## 2. Ordering

1. The channel keeps its P5 semantics: one blocking slot, `m_callerMutex` serialising posters,
   pumped by the apply thread **between drain batches**, publish-then-ring. Frames are FIFO
   against each other; they are NOT ordered against SEG_CMD records by the channel itself.
2. The client-side discipline that orders frames against records is unchanged and remains
   mandatory under spawn: `WaitForApplyBeforeEglForwarder` waits `appliedSeq` to
   `LastPublishedSeq` before every forwarder (CONTRACT-P5E §2.5), because the channel is pumped
   between drain batches and a control frame would otherwise overtake in-flight records.
3. **All twelve ops stay blocking in fc**, including the three f0-egl §4.3 marked relaxable
   (`SetSwapInterval`, `ReleaseSurface`, `SetWindowHandle`). Relaxing them to fire-and-forget is
   a P6 decision and must preserve: a `SetWindowHandle` is in effect before any later
   `CreateWindowSurface` (one ordered channel suffices), and `ReleaseResources` NEVER relaxes —
   `MobileGL::Destroy()` walks on the moment it answers (ServerLoop.h's header note).
4. Re-entrancy is unchanged: a post from the apply thread dispatches inline (the
   `~BackendObject_DirectGLES` → `ReleaseEGLResources` path). This is thread semantics, not wire
   semantics; under spawn the destructor path runs server-side natively and never posts.

## 3. Error semantics

1. Loop down, backend alive → `Fatal{ApplyThreadNotRunning}` (M-7, unchanged).
2. Loop down, no backend → `MOBILEGL_ERR_NOT_INITIALIZED`, nothing runs on the caller (C2,
   unchanged).
3. Backend null at dispatch → `MOBILEGL_ERR_NOT_INITIALIZED` with `ok=false`; the forwarder's
   answer is the old `false`/void-drop, unchanged.
4. Successful void operations (`SetSwapInterval`, `ReleaseSurface`, `ReleaseResources`,
   `SetWindowHandle`) reply `ok=true` after completing on the apply thread; a null backend
   replies `ok=false`. The wire therefore preserves the success/failure distinction even
   though the inproc forwarder returns void.
5. Wire entry (`ServerApplyWireSurfaceOp`, the future P6 control pump's entry point; P5f drives
   it from tests only): a decode failure is **`Fatal{ProtocolCorruption, "SurfaceOp"}`**, never a
   dropped frame (a dropped control frame is a client waiting on a reply that never comes); a
   decoded `WindowKind::AndroidNativeWindow` is
   **`Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`** — the window kind is legal schema,
   the token is the part that means nothing in the server's process, and real window arrival is
   P12. A decoded `MetalLayer` is similarly **`Fatal{UnmigratedSurface, "MetalLayer@P12"}`**,
   because its `CAMetalLayer*` also belongs to the client process. Inproc-only kinds
   (`InitCapabilitiesInprocOnly`, `SwapBuffersInprocOnly`,
   `InitWindowSurfaceInprocOnly`, `ProbeForTesting`) cannot arrive: the encoder refuses them
   (`SurfaceWireError::InprocOnlyOpOnTheWire`).
6. A frame that reaches the inproc dispatch with `kind == None` or a kind the switch does not know
   is `Fatal{ProtocolCorruption, "SurfaceOp.kind"}`; a `windowBackend` tag outside the enum is
   `Fatal{ProtocolCorruption, "SurfaceOp.windowBackend"}`. Inproc these are corruption shapes, not
   bad input — same discipline as the ring's header check.

## 4. Reply carriers — and the deliberate non-use of `SurfaceReply.defaultFb`

1. The default framebuffer's shape already crosses as `kEventSurfaceChanged` on SEG_EVENT (P5c
   ev), and the client drains it at the RPC return (`BackendObject_Remote.cpp`'s create/resize
   arms). fc rules that `SurfaceReply.defaultFb` **stays unset**: two spellings of one fact is how
   the two sides come to disagree about it (the `CapsSnapshot` deprecated-fields lesson,
   `protocol.fbs`). Enabling it later is a schema amendment, not a silent flip.
2. The caps republish side effects are unchanged and are server-spontaneous frames on the wire:
   a real bind under `MakeCurrent` republishes (R-12 arm (a)); `InitCapabilities` publishes. The
   client's absorption point — pump the control plane after the RPC returns — is the same in both
   worlds: "after this op's reply, before the next op" is a total order the client can rely on.

## 5. What fc deliberately did NOT do

- No transport: no socket, no spawn pump, no client-minted token table (the frame fields are
  token-width already, so P6 is transport-only here).
- The two dead forwarders (`ServerSwapEGLBuffers`, `ServerInitWindowSurface`) and
  `ServerInitCapabilities` were NOT given wire ops (f0-egl F8/§4.2): they ride the inproc channel
  as inproc-only kinds because the function-pointer mailbox is gone entirely, and G14 forbids
  deleting their pinning tests.
- Blocking semantics, the shadow, the doorbell order and the C2 exit block: unchanged.
