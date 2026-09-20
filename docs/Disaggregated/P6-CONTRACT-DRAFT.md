# CONTRACT-P6 (DRAFT) — the backend runs in a second process

> **P5f prerequisites completed** (2026-09-20): field ownership has zero
> `BARRIER_PULLED` rows; the dual-block and strict gates, context/lifetime work,
> registry and reverse-channel guards, closing review fixes, and final Redmi
> paired runs have passed. See [`P5F-WIRE-COMPLETENESS.md`](P5F-WIRE-COMPLETENESS.md),
> the [closing review](notes/p5f/close-review.md) and [device report](notes/p5f/device-report.md).
>
> **P6 has not started:** `a6`, `c6`, and spawn implementation remain unstarted.
> This remains a draft written before its own audit. Package `a6` ([`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md) §5)
> is a read-only audit whose output is this file's input. Rows that depend on it are marked
> **`PENDING a6`**. `c6` moves this file to `MobileGL/MG_Remote/CONTRACT-P6.md`, where it becomes
> normative.

Authority once landed: this file, beside `CONTRACT-P5.md` (table 0, byte carriers, R-1…R-17),
`CONTRACT-P5B.md` (class-C slots), `CONTRACT-P5C.md` (rule E, SEG_EVENT, the guards) and
`CONTRACT-P5E.md` (rule F, the barriered predicate, the wait rule). Disagreements: this file is
newer and wins; §9 lists them. Historical planning base `feat/disaggregated @ f23fbc1b`; every `file:line` re-resolves
at `c6`'s head. Paths under `MobileGL/` unless they start with `docs/`.

Changes go through the integrator, in `c6`'s copy. Packages compile against these rows from day one.

---

## §0 Rule G

Rules A–F bind unchanged. P6 adds:

**Rule G — a role may not name a process-local handle.** Rule E forbade naming the other role's
*memory*; rule G extends it to *identity*. A value whose meaning comes from the owning process's
kernel or loader state — `EGLDisplay`, `EGLSurface`, `EGLContext`, `ANativeWindow*`, any `void*`
window handle, an fd number, a pid, a mapped address — either crosses as a **token** defined below,
or does not cross. Descriptors cross only by `SCM_RIGHTS` (`Transport/FdPassing.h`), where the
kernel renumbers them.

Rule G's teeth are §8's arm-proof gate: a spawn lane that silently ran monolith satisfies every
other gate here.

## §1 Table 0 additions

| row | encoding | invalid | consumer |
|---|---|---|---|
| `DisplayToken` / `SurfaceToken` / `ContextToken` | `Uint64`, minted by the client, dense from 1, never reused in a session. Server keeps token → native handle; it never sees the client's EGL values. `protocol.fbs:186-196`'s `display` / `surface` are already `ulong` and are these. | `0` on any op but `ReleaseCurrent` → `Fatal{ProtocolCorruption}` | the server's surface map |
| `SurfaceOpKind` additions | **LANDED by P5f `fc`** (append-only, `protocol.fbs:166-179`): `SetSwapInterval=8`, `ReleaseResources=9`, `SetWindowHandle=10`, plus `WindowKind::MetalLayer=6` and `SurfaceOp.readSurface/context` (f0-egl's schema gap list, confirmed to the letter). | unknown tag → `Fatal{ProtocolCorruption}`, never ignored | `ServerMain`'s control pump |
| `WindowKind::AndroidNativeWindow` at the server | Refusal: `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`. | — | §4.3 |
| `FatalCode::ServerCrashed` / `DeviceLost` | Already allocated (`protocol.fbs:234-242`); P6 is the first phase that can produce either. | — | §5 |
| ABI fingerprint | Unchanged. `CapsCodec.h:23-29`: spawn is same-machine, same-binary, so the existing `sizeof(DynamicBackendParameters)` / `sizeof(MGPCaps)` / build-fingerprint handshake is inherited. Fixed-width rewrite stays P7's. | mismatch → `Fatal{AbiMismatch}` | `SessionHandshakeTest.cpp:115` |

## §2 The transport (`so`)

1. `SocketTransport` implements `ITransport` and adds nothing to it. The two doorbell accessors
   stay on the session (`Transport/SessionRings.h:279-285`, `Server/ServerSession.cpp:670-690`).
2. `ReceiveFrame`'s `BUFFER_TOO_SMALL` keeps the message queued. `ITransport.h` records that the
   earlier branch dropped it, wedging the stream at the first oversized message.
3. **`SocketTransport` must be green before a second process exists**: one `socketpair()`, two
   threads, the whole `InProcessTransportTest` suite. Same discipline as `Transport/SessionRings.h:22-28`.
4. The server creates the four segments (`SessionRings.h:120-130`); the client adopts them by
   `SCM_RIGHTS`. `ShmSegment::Adopt` (`Transport/ShmSegment.h:57`, `ShmSegmentPosix.cpp:120`) and
   the aux socket (`Transport/FdPassing.cpp:92`) exist and are tested (`MG_Test/Wire/FdPassingTest.cpp:114`,
   `:187`). `AttachInProcess` (`SessionRings.h:147-159`) is replaced by an adopt on the received fds.
5. Doorbells are `SocketDoorbell`. `Doorbell.h:385-420` chose a socket byte over a futex so peer
   death arrives as `POLLIN|POLLHUP` with `recv()==0` rather than as a hang. One syscall per wake
   is the price; §8.8 measures it.
6. Windows: `Adopt` is POSIX-only. `UnixSocket` and `NamedPipe` stay named refusals
   (`ConfigLoader.cpp:310-348`). P6 lands POSIX only.

## §3 The process (`sm`)

1. **The child must not be able to become a client.** Both required: (a) `MOBILEGL_TRANSPORT` and
   every role-selecting `MOBILEGL_IPC_*` removed from the child's envp; (b) the child's config
   cannot recursively select a client transport. The original plan expressed (b) as forcing
   `Monolith`. **`PENDING a6`**: separate that anti-recursion decision from backend server-role
   selection. P5f's record consumers currently select their server arm with an active transport;
   blindly forcing `Monolith` would select frontend glue. Preserve both anti-recursion controls
   while retaining the server-owned record path; `c6` must settle the concrete selector.
2. Image lookup: `MOBILEGL_IPC_SERVER_PATH` (already parsed, `Config.h` `IpcTable::ServerPath`),
   then `dladdr` to find `libMobileGLServer.so` beside the loaded library. **An unresolvable path
   is a named refusal, never a monolith fallback** — `ConfigLoader.cpp:315-317` names that accident.
3. `ServerMain`. Today's `MobileGLServer` (`CMakeLists.txt:923`) is the P0 spike stub
   (`tools/spikes/server_stub/main.cpp`). **`PENDING a6`**: whether the server image can link
   without `MG_Impl` (`MG_Backend/MGPipe/PipeInputs.h` claims it). If not, this file says so and the
   debt is recorded against P13 rather than left as a comment.
4. Handshake: `Hello` → `Welcome` → segment descriptors → first `CapsSnapshot`
   (`protocol.fbs:59-153`). Bounded retry; an exhausted budget is a named refusal.
5. Child exits on EOF; parent reaps. §8.4 makes this mechanical.

## §4 The control plane (`cp`)

1. **The framing itself LANDED in P5f, package `fc`** (`docs/Disaggregated/notes/p5f/fc-report.md`).
   The twelve `Server*` forwarders no longer post a function pointer plus a stack-local `void*` to a
   one-slot mailbox (that was P5c audit row G4, recorded as P6's by `CONTRACT-P5C.md:537`): each
   packs a value-only `Server::SurfaceControlFrame`
   (`MG_Remote/Server/SurfaceControlFrame.h`) and the one blocking slot carries it by value. P6's
   remainder is TRANSPORT ONLY: the client encodes the frame with
   `MG_Remote::EncodeSurfaceOpFrame` (`MG_Remote/Protocol/SurfaceOpCodec.h`) onto the control
   socket, the server's pump decodes and enters through
   `MG_Remote::ServerApplyWireSurfaceOp`, and the reply crosses as `SurfaceReply`.
2. Reconciliation against the enumerators (f0-egl §4.2, landed): `ServerSwapEGLBuffers` stays a
   record with its own present credit (P5e ruling, `Client/BackendObject_Remote.cpp`);
   `ServerInitCapabilities` is answered by `CapsSnapshot`; `ServerInitWindowSurface` is a
   client-side no-op. The three ride the inproc frame channel as inproc-only kinds that the codec
   refuses to encode (`SurfaceWireError::InprocOnlyOpOnTheWire`). The remaining nine map onto the
   seven plus §1's additions - ten wire ops with `ReleaseCurrent`.
3. **Real windows are refused, by name.** `WindowHandle::Handle` (`MG_Backend/BackendObject.h:561-566`)
   is an `ANativeWindow*` on Android. fc landed the refusal at the wire entry: a decoded
   `WindowKind::AndroidNativeWindow` dies `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`
   (`SurfaceOpCodec.cpp` `ServerApplyWireSurfaceOp`). P6 lands pbuffer / surfaceless / offscreen —
   what `HeadlessGL`, the split-equivalent lane and trace replay need, and what P6's exit gate
   measures. **Real window arrival is P12.**
4. `ForgetCurrentTuple`'s N-3 rule holds identically on the frame path: the forgetting calls moved
   INTO the dispatch (`ServerLoop.cpp` `ApplySurfaceControlFrame`), and ServerLoopTest's C7/N-3
   controls drive them through framed posts unchanged.

## §5 Death, and what P5e changed

1. The latch is already designed and is implemented unchanged: `docs/Disaggregated/ARCHITECTURE.md:504`
   — server dies → client reads EOF/EPIPE → device-lost latch (GL calls no-op, `eglSwapBuffers`
   returns `EGL_FALSE` + `EGL_CONTEXT_LOST`, `glGetGraphicsResetStatus` returns
   `GL_UNKNOWN_CONTEXT_RESET`); `MOBILEGL_IPC_RESPAWN=1` restarts and re-pushes (default off).
   Client dies → server reads EOF and exits; `MOBILEGL_IPC_IDLE_EXIT_S` (default 30) is a last resort.
2. What P5e added, and no existing document covers. Under lockstep a dead apply thread took the
   process with it, so "the client kept drawing" was unreachable. Under run-ahead the client is
   several records ahead by construction. Therefore:
   - A published, never-applied record is **discarded at the latch, not waited on**.
   - **Slow and dead are distinguished without a timeout threshold.** A server one frame behind is
     P5e's intended steady state; a server that is gone is `Fatal{ServerCrashed}`. `SocketDoorbell`
     already latches `m_dead` on EOF (`Doorbell.cpp`'s `Drain`) — take the fact from there.
   - **A frozen peer produces no hangup.** Android's cached-app freezer suspends the child without
     closing anything. P6 gives a named, bounded diagnostic; full handling is P12's.
3. This is P6's only new design; everything else is carriage. It shares a predicate with E1's
   outstanding re-specification (ID-122) — design the two together, and settle neither by choosing a
   threshold.

## §6 Process-lifetime statics (`st`)

**Implemented by P5f `fs`; P6 inherits and verifies it.** The statics recorded as P6 debt in
`CONTRACT-P5C.md:538-539` no longer carry unqualified synced state between contexts. The
[fs report](notes/p5f/fs-report.md) records the full census, lifecycle rules and red-once evidence.

- `ContextEpoch` combines native ES context generation, served-context serial and backend
  execution arm (monolith / transport server). `ScopedDefaultUnpackState` republishes defaults
  when that key changes; render-state invalidates its shadow before the version fast path.
  Identical parameter bytes or versions cannot make a new epoch inherit an old sync result.
  The transport raw-depth sampler owns a native sampler and rebuilds it on native generation change.
- XFB state is role-local; the server map is keyed by the record's non-reused
  `BoundStreamOutputLifetimeId`, including virtual name 0, with capture-local scatter scratch.
  **A served-context serial change / `applier_reset` is not object destruction.** Current binding
  state is reset, but still-live paused/pending spans survive and `set_context_values` selects
  their lifetime identity again; returning to a context does not emit another Begin. Native
  context generation change discards the old native ids/spans, and context destruction clears them.
- Server liveness follows successful make-current and the control lifecycle. Release-current,
  release-resources, context/backend destruction and loop stop clear it. Surface creation alone
  is not a current context, and a verb stamp cannot revive a released server context.

`a6` checks these established ownership/lifetime rules against the new process/session boundary;
`st` verifies their socket/spawn lifecycle wiring and G5 controls. It does not introduce a new
“clear every object at every reset” policy or reopen the P5f implementation work. Additional
process/session lifetime findings, if any, remain **`PENDING a6`**; P6 still does not add multi-context
support (§9.2).

## §7 Unchanged by P6

Rings, codec, record layout, applier, both verb sinks, both backends, the barriered predicate and
the wait rule (`CONTRACT-P5E.md` §2). P6's lane runs the same reduced path as `integration-split`
and must produce the same name set and the same numbers, not more (§8.2). The 511 `UnmigratedVerb`
class-C entries are not P6's.

## §8 The gate

Beyond the five-part gate (`docs/Disaggregated/ARCHITECTURE.md` §13.2):

1. G1 — pull build 0/0/0/0, `.text` unchanged.
2. G2/G14 — `integration-spawn`'s name set identical to `integration-split`'s.
3. `integration-spawn` green at the same count as `integration-split` (179/179 on the historical
   draft baseline; `c6` refreshes the actual discovery and result sets).
4. Process tree, mechanical — child count before and after every entry: exactly one while
   running, zero after. `HeadlessGL`'s fork pre-check leaves no orphan.
5. **Arm proof (ID-124)** — every spawn entry records the child pid and `transport=spawn` in its own
   private log. A spawn lane that fell back to monolith passes items 1-4.
6. Red-once per package (R-16), one named pair each.
7. Device — Redmi `2f7cbe2e`, reboot-clean, paired A/B in one thermal window; OpenRA on Adreno 830
   at split SSIM ≥ 0.99.
8. Performance recorded, not gated, but one number is mandatory: **the socket doorbell's cost against
   `inproc`'s condvar.** P5d took the client thread 15.0 → 9.2 ms/frame and part of that was the
   doorbell path. P6's only expected regression.

Negative controls, each run red once:

| # | knob | what it must falsify |
|---|---|---|
| S1 | `MOBILEGL_IPC_SERVER_PATH=/nonexistent` | named refusal, not a silent monolith fallback (§3.2) |
| S2 | `kill -9` the server mid-frame | §5.1's latch; the client never hangs and never keeps submitting |
| S3 | child envp not scrubbed | §3.1's second safety catches it, by name |
| S4 | inherited P5f epoch invalidation disabled in spawn lifecycle wiring | a replaced context inherits stale synced state; returning to a still-live XFB lifetime must retain its paused span (§6) |
| S5 | `WindowKind::AndroidNativeWindow` reaches the server | named `Fatal`, not a black screen (§4.3) |

S2's shape is settled before it is written. E1 demanded a `Fatal` that occurs zero times in the
entire run (ID-122); S2 must falsify *"the client can tell slow from dead"*.

## §9 Amendments to the earlier contracts

1. `CONTRACT-P5C.md:537-539` — EGL value framing was discharged by P5f `fc`, and the unpack /
   render-state epoch work by P5f `fs`. P6 carries the existing frames over the socket (§4) and
   verifies the inherited lifecycle rules in the second process (§6).
2. `CONTRACT-P5C.md:56` says P6's multi-context shape reads `MGPApplierReset::ContextSerial` for
   real. **Declined**: P6 stays single-context, multi-context moves to P12 where the Service
   lifecycle makes it concrete, and the assertion stands unchanged.
3. `Transport/ReplySlot.h` marks the 2 MiB reply cap and chunked readback "P6 debt". **Declined**:
   they are P9's. Taking them here would make "a transport swap and nothing else" false a second time.
4. `CapsCodec.h:23-29` — inherited unchanged; recorded in §1 so it is not re-litigated.

## §10 Not P6

Real window arrival and the `android:process=":mgl"` Service (P12); multi-context (P12, §9.2);
chunked readback and the reply-slot pool (P9, §9.3); `DynamicBackendParameters` fixed-width rewrite
(P7); `MOBILEGL_IPC_POLL_ESCALATE` (P10); remaining monolith-only frontend-object / twin-registry
glue cleanup (P3b/P4b) and named P7 functionality debts; Windows `pipe:` / `unix:` (§2.6); the 511
class-C entries (§7). P5f already eliminated `BARRIER_PULLED` ownership and transport frontend-keyed
registry access; those are completed prerequisites, not debts to carry forward.

E1's re-specification (ID-122) is **not** discharged by P6 and does not become P6's because P6
starts. It appears here only because §5.3 argues it shares a predicate.
