# CONTRACT-P6 (DRAFT) — the backend runs in a second process

> **Draft, written before its own audit.** Package `a6` ([`P6-SPAWN-PLAN.md`](P6-SPAWN-PLAN.md) §5)
> is a read-only audit whose output is this file's input. Rows that depend on it are marked
> **`PENDING a6`**. `c6` moves this file to `MobileGL/MG_Remote/CONTRACT-P6.md`, where it becomes
> normative.

Authority once landed: this file, beside `CONTRACT-P5.md` (table 0, byte carriers, R-1…R-17),
`CONTRACT-P5B.md` (class-C slots), `CONTRACT-P5C.md` (rule E, SEG_EVENT, the guards) and
`CONTRACT-P5E.md` (rule F, the barriered predicate, the wait rule). Disagreements: this file is
newer and wins; §9 lists them. Base `feat/disaggregated @ f23fbc1b`; every `file:line` re-resolves
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
| `SurfaceOpKind` additions | Append-only to `protocol.fbs:166-175`. Expected `SetSwapInterval`, `ReleaseResources`, `SetWindowHandle` — **`PENDING a6`** (audit item 4). | unknown tag → `Fatal{ProtocolCorruption}`, never ignored | `ServerMain`'s control pump |
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
   forced to `Monolith` regardless. Either alone fails as a fork bomb.
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

1. `Server/ServerLoop.cpp:802-1022`'s twelve `Server*` forwarders each post a function pointer plus
   a stack-local `void*` to a one-slot mailbox (`ServerLoop.cpp:533-568`, `:645`) — P5c audit row
   G4, recorded as P6's (`CONTRACT-P5C.md:537`). Neither has meaning across a process, so each
   becomes a `SurfaceOp` / `SurfaceReply` pair.
2. Reconciliation against the seven existing enumerators: `ServerSwapEGLBuffers` stays a record with
   its own present credit (P5e ruling, `Client/BackendObject_Remote.cpp`); `ServerInitCapabilities`
   is answered by `CapsSnapshot`; `ServerInitWindowSurface` is a client-side no-op. The remaining
   nine map onto the seven plus §1's additions. **`PENDING a6`**.
3. **Real windows are refused, by name.** `WindowHandle::Handle` (`MG_Backend/BackendObject.h:561-566`)
   is an `ANativeWindow*` on Android. P6 lands pbuffer / surfaceless / offscreen — what `HeadlessGL`,
   the split-equivalent lane and trace replay need, and what P6's exit gate measures. **Real window
   arrival is P12.**
4. `ForgetCurrentTuple`'s N-3 rule (`ServerLoop.cpp:318-330`) is a backend property, not a transport
   one, and must hold identically on the frame path.

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

`g_syncedRenderStateParameters` (`MG_Backend/DirectGLES/DirectGLES.cpp:4586`) and
`ScopedDefaultUnpackState::s_synced` are process-level statics with per-context semantics, recorded
as P6's by `CONTRACT-P5C.md:538-539`. In a server process reused across client contexts they hand
the next context the previous one's synced state.

**Rule: any static whose meaning is per-context or per-session is keyed by the context generation
and reset at `applier_reset`.** **`PENDING a6`** for the complete list (audit item 2); the two above
are the known members. This class is structurally invisible to `inproc`.

## §7 Unchanged by P6

Rings, codec, record layout, applier, both verb sinks, both backends, the barriered predicate and
the wait rule (`CONTRACT-P5E.md` §2). P6's lane runs the same reduced path as `integration-split`
and must produce the same name set and the same numbers, not more (§8.2). The 511 `UnmigratedVerb`
class-C entries are not P6's.

## §8 The gate

Beyond the five-part gate (`docs/Disaggregated/ARCHITECTURE.md` §13.2):

1. G1 — pull build 0/0/0/0, `.text` unchanged.
2. G2/G14 — `integration-spawn`'s name set identical to `integration-split`'s.
3. `integration-spawn` green at the same count as `integration-split` (today 179/179).
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
| S4 | `st`'s generation reset disabled | a second context inherits the first's synced state (§6) |
| S5 | `WindowKind::AndroidNativeWindow` reaches the server | named `Fatal`, not a black screen (§4.3) |

S2's shape is settled before it is written. E1 demanded a `Fatal` that occurs zero times in the
entire run (ID-122); S2 must falsify *"the client can tell slow from dead"*.

## §9 Amendments to the earlier contracts

1. `CONTRACT-P5C.md:537-539` — the EGL forwarders' control plane and the `s_synced` /
   `g_syncedRenderStateParameters` resets are discharged here, §4 and §6.
2. `CONTRACT-P5C.md:56` says P6's multi-context shape reads `MGPApplierReset::ContextSerial` for
   real. **Declined**: P6 stays single-context, multi-context moves to P12 where the Service
   lifecycle makes it concrete, and the assertion stands unchanged.
3. `Transport/ReplySlot.h` marks the 2 MiB reply cap and chunked readback "P6 debt". **Declined**:
   they are P9's. Taking them here would make "a transport swap and nothing else" false a second time.
4. `CapsCodec.h:23-29` — inherited unchanged; recorded in §1 so it is not re-litigated.

## §10 Not P6

Real window arrival and the `android:process=":mgl"` Service (P12); multi-context (P12, §9.2);
chunked readback and the reply-slot pool (P9, §9.3); `DynamicBackendParameters` fixed-width rewrite
(P7); `MOBILEGL_IPC_POLL_ESCALATE` (P10); object-class BARRIER-PULLED rows and the frontend-keyed
twin registry (P3b/P4b, P7); Windows `pipe:` / `unix:` (§2.6); the 511 class-C entries (§7).

E1's re-specification (ID-122) is **not** discharged by P6 and does not become P6's because P6
starts. It appears here only because §5.3 argues it shares a predicate.
