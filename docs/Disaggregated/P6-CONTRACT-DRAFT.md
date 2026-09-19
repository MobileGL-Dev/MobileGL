# CONTRACT-P6 (DRAFT) — the backend runs in a second process

> **THIS IS A DRAFT, AND IT IS WRITTEN BEFORE ITS OWN AUDIT.** Package `a6` (`P6-SPAWN-PLAN.md`
> §4) is a read-only audit whose output is this file's input. Every row below that depends on
> what `a6` finds is marked **`PENDING a6`** and states what it is waiting for. A contract that
> pretended to know those rows today would be the same defect this project has recorded three
> times: evidence asserted ahead of the thing it describes (ID-124, ID-132, ID-133).
>
> On landing, package `c6` moves this file to `MobileGL/MG_Remote/CONTRACT-P6.md`, beside its
> four siblings, and it becomes normative there. Until then it is a proposal.

Authority once landed: this file, beside `CONTRACT-P5.md` (table 0, byte carriers, field
ownership, R-1…R-17), `CONTRACT-P5B.md` (the class-C slots), `CONTRACT-P5C.md` (rule E, the two
named exemptions, SEG_EVENT, the guards) and `CONTRACT-P5E.md` (rule F, the barriered predicate,
the wait rule). Where it disagrees with any of them this file is newer and wins; §9 lists every
such place. Base: `feat/disaggregated @ f23fbc1b`. Every `file:line` was read at that commit and
must be re-resolved at `c6`'s head; paths are under `MobileGL/` unless they start with `docs/`.

**How to change it.** Package `c6`'s file, edited by the integrator first. A P6 package
(`so` / `sm` / `cp` / `st` / `t6`) that needs a row changed goes through the integrator; the
packages compile against the rows below from day one.

---

## §0 What P6 is, and the rule above every row

P5c made `inproc` honest — between the two roles nothing crosses except `SEG_CMD` / `SEG_STAGE` /
`SEG_REPLY` / `SEG_EVENT` / the caps snapshot / control frames — *so that P6 would be a transport
swap and nothing else* (`MG_Remote/CONTRACT-P5C.md:23-27`). P5e then retired the lockstep on the
Espryt draw path: the client publishes an unbarriered record and does not wait.

**P6 puts the server in a second process.** The rings, the codec, the applier, the sinks, the
backends and the wait rule are unchanged. What changes is that the two roles no longer share an
address space at all — so every assumption that survived P5c *because* they did is now load-bearing
for the first time.

Rules A–F bind unchanged. P6 adds one rule:

**Rule G — a role may not name a process-local handle.** Rule E (P5c) forbade a role from naming
the other role's *memory*. Rule G extends it to *identity*: a value whose meaning is supplied by
the owning process's kernel or loader state — `EGLDisplay`, `EGLSurface`, `EGLContext`,
`ANativeWindow*`, any `void*` window handle, a file descriptor number, a `pid`, a mapped address —
either crosses as a **token** this contract defines, or it does not cross. A descriptor crosses
only through `SCM_RIGHTS` (`Transport/FdPassing.h`), where the *kernel* re-numbers it, never as an
integer in a frame.

Rule G's teeth are §8's arm-proof gate, not a reviewer's attention: a spawn lane that silently ran
monolith satisfies every other gate in this document.

---

## §1 Table 0 additions — the encodings P6 introduces

| row | encoding | invalid | consumer |
|---|---|---|---|
| **`DisplayToken` / `SurfaceToken` / `ContextToken`** | `Uint64`, **minted by the client**, dense from 1, never reused within a session. The server keeps a token → native-handle map and never sees the client's `EGLDisplay`/`EGLSurface`/`EGLContext` values. `protocol.fbs:186-196`'s `SurfaceOp.display` / `.surface` are already `ulong` and are these. | `0` on any op other than `ReleaseCurrent` is `Fatal{ProtocolCorruption}` | the server's surface map |
| **`SurfaceOpKind` additions** | Append-only to `protocol.fbs:166-175`. Expected: `SetSwapInterval`, `ReleaseResources`, `SetWindowHandle`. **`PENDING a6`** — the count is the audit's item 4, not a guess this file may make. | an unknown tag is `Fatal{ProtocolCorruption}`, never ignored | `ServerMain`'s control pump |
| **`WindowKind::AndroidNativeWindow` arriving at the server** | Not an encoding but a refusal: `Fatal{UnmigratedSurface, "AndroidNativeWindow@P12"}`. `protocol.fbs:186-196`'s own comment already says Android transfers the window out of band; P12 is that band. | — | §4.3 |
| **`FatalCode::ServerCrashed` / `DeviceLost`** | Already allocated (`protocol.fbs:234-242`). P6 is the first phase that can produce either. | — | §5 |
| **ABI fingerprint** | Unchanged. `CapsCodec.h:23-29` states the ruling: P6's spawn is same-machine and same-binary, so the existing `sizeof(DynamicBackendParameters)` / `sizeof(MGPCaps)` / build-fingerprint handshake is inherited as-is and `Fatal{AbiMismatch}` keeps its meaning. **The fixed-width rewrite stays P7's account.** | mismatch → `Fatal{AbiMismatch}` | `SessionHandshakeTest.cpp:115` |

---

## §2 The transport (`so`)

**2.1 `SocketTransport` implements `ITransport` and adds nothing to it.** The two doorbell
accessors stay on the session, not on the interface — that ruling was made in P5's s1 precisely so
P6 would not discover it (`Transport/SessionRings.h:279-285`, `Server/ServerSession.cpp:670-690`).

**2.2 `ReceiveFrame`'s `BUFFER_TOO_SMALL` keeps the message queued.** This is not an optimization;
`ITransport.h` records that the earlier branch's transport failed the call *and dropped the
message*, which wedges the stream permanently the first time a message exceeds the reader's guess.
`SocketTransport` must pass the same assertion `InProcessTransport` passes.

**2.3 `SocketTransport` must be green before a second process exists.** One `socketpair()`, two
threads, the whole `InProcessTransportTest` suite. This is the same discipline that made `inproc`
use `ShmSegment` rather than `new[]` (`Transport/SessionRings.h:22-28`): a code path first executed
on the day the second process appears is the shape of every "it was green in CI" failure this phase
exists to avoid.

**2.4 Segment delivery.** The server owns and creates the four segments (`SessionRings.h:120-130`);
the client adopts them by `SCM_RIGHTS`. `ShmSegment::Adopt` (`Transport/ShmSegment.h:57`,
`ShmSegmentPosix.cpp:120`) and the dedicated `AF_UNIX SOCK_DGRAM` aux socket
(`Transport/FdPassing.cpp:92`) already exist and are tested (`MG_Test/Wire/FdPassingTest.cpp:114`,
`:187`). The client's `AttachInProcess` path (`SessionRings.h:147-159`) is replaced by an adopt on
the received descriptors — the same `Adopt` + `Map` calls it already makes.

**2.5 Doorbells are `SocketDoorbell`, and this is a deliberate cost.** `Doorbell.h:385-420` chose a
socket byte over a futex so that peer death arrives as `POLLIN|POLLHUP` with `recv()==0` rather
than as a hang. Implemented at `Doorbell.cpp:211-360`, four cases at
`MG_Test/Wire/FdPassingTest.cpp:197`, `:227`, `:260`, `:289`. **One syscall per wake is the price
of death detection**; §8.8 makes it a measurement, not an argument.

**2.6 Windows.** `ShmSegment::Adopt` is POSIX-only. `TransportMode::UnixSocket` and `NamedPipe`
stay **named refusals** (`ConfigLoader.cpp:310-348`). P6 lands POSIX only.

---

## §3 The process (`sm`)

**3.1 The child must not be able to become a client.** Two independent mechanisms, both required:
(a) `MOBILEGL_TRANSPORT` and every `MOBILEGL_IPC_*` that selects a role is **removed from the
child's envp**; (b) the child's own config is **forced to `Monolith`** regardless of what it reads.
Either alone is a single point of failure whose failure mode is a fork bomb.

**3.2 Finding the server image.** `MOBILEGL_IPC_SERVER_PATH` first (already parsed,
`Config.h`'s `IpcTable::ServerPath`), then `dladdr` on a symbol in the loaded library to locate
`libMobileGLServer.so` beside it. **A path that does not resolve is a named refusal and never a
fallback to monolith** — `ConfigLoader.cpp:315-317` names this exact accident as the thing its own
refusal exists to prevent.

**3.3 `ServerMain`.** Today's `MobileGLServer` (`CMakeLists.txt:923`) is the P0 spike stub
(`tools/spikes/server_stub/main.cpp`), Android-only and off by default; it writes a marker and
exits. `c6` lands the real target. **`PENDING a6`** — whether the server image can be linked
without `MG_Impl` (`MG_Backend/MGPipe/PipeInputs.h` asserts "P6, where MG_Impl is not in the
server"). If it cannot, this file says so and the debt is recorded against P13 rather than left as
a comment promising something no phase owns.

**3.4 Handshake.** Bounded retry, never unbounded. `Hello` → `Welcome` → segment descriptors →
first `CapsSnapshot` (`protocol.fbs:59-153`). A retry budget that is exhausted is a named refusal.

**3.5 No orphans.** The child exits on EOF of the control socket. The parent reaps. §8.4 makes this
mechanical rather than observed.

---

## §4 The control plane (`cp`)

**4.1 The twelve forwarders become frames.** `Server/ServerLoop.cpp:802-1022` holds twelve `Server*`
EGL forwarders; each posts a **function pointer plus a stack-local `void*`** to a one-slot mailbox
(`ServerLoop.cpp:533-568`, `RunOnApplyThread` at `:645`). This is P5c audit row G4, recorded there
as P6's (`CONTRACT-P5C.md:537`). Across a process boundary neither the pointer nor the `void*` has
meaning, so each becomes a `SurfaceOp` / `SurfaceReply` pair.

**4.2 The reconciliation.** Twelve forwarders against seven existing `SurfaceOpKind` enumerators:
- `ServerSwapEGLBuffers` does **not** become a frame. It travels as a record, in order, with its
  own present credit — `Client/BackendObject_Remote.cpp`'s own comment states this and P5e ruled it.
- `ServerInitCapabilities` is answered by `CapsSnapshot`, which already exists.
- `ServerInitWindowSurface` is a client-side no-op and crosses nothing.
- The remaining nine map onto the seven enumerators plus the additions of §1. **`PENDING a6`.**

**4.3 Real windows are refused, by name.** `WindowHandle::Handle` (`MG_Backend/BackendObject.h:561-566`)
is an `ANativeWindow*` on Android and means nothing in the child. P6 lands pbuffer / surfaceless /
offscreen — enough for `HeadlessGL`, the whole `integration-split`-equivalent lane and trace replay,
which is what P6's own exit gate measures. **The arrival of a real window is P12.** This row exists
because "P6 is done" will otherwise be read as "the game runs on the phone now". It will not.

**4.4 `ForgetCurrentTuple` semantics survive verbatim.** `ServerLoop.cpp:318-330`'s N-3 rule —
creating a different surface destroys and recreates the native context, so the bound tuple names a
dead context — is a property of the backend, not of the transport, and must hold identically on the
frame path.

---

## §5 Death, and what P5e changed about it

**5.1 The latch is already designed; P6 implements it unchanged.** `docs/Disaggregated/ARCHITECTURE.md:504`:
server dies → client reads EOF/EPIPE → device-lost latch (GL calls no-op, `eglSwapBuffers` returns
`EGL_FALSE` + `EGL_CONTEXT_LOST`, `glGetGraphicsResetStatus` returns `GL_UNKNOWN_CONTEXT_RESET`);
`MOBILEGL_IPC_RESPAWN=1` restarts and re-pushes (default off). Client dies → server reads EOF and
exits immediately; `MOBILEGL_IPC_IDLE_EXIT_S` (default 30) is a last resort only.

**5.2 What P5e added, and what no existing document covers.** Under lockstep, an apply thread that
died took the process with it, so "the client kept drawing" was not a reachable state. Under
run-ahead the client is several records ahead by construction. Therefore:

- **A published, never-applied record is discarded at the latch, not waited on.** The client must
  not block for a watermark that can no longer move.
- **Slow and dead must be distinguishable without a timeout threshold.** A server that is merely a
  frame behind is P5e's intended steady state; a server that is gone is `Fatal{ServerCrashed}`.
  `SocketDoorbell` already latches `m_dead` on EOF (`Doorbell.cpp`'s `Drain`) — the distinction is
  available as a *fact* and must be taken from there rather than inferred from elapsed time.
- **A frozen peer produces no hangup.** Android's cached-app freezer suspends the child without
  closing anything. P6 must give a named, bounded diagnostic instead of relying on "it will come
  back". Full handling is P12's; the diagnostic is P6's.

**5.3 This is P6's only new design.** Everything else in this contract is carriage. It is also the
nearest neighbour of the E1 control's outstanding re-specification (ID-122), which needs exactly the
same predicate: a test that can tell a slow apply from a dead one. **The two should be designed
together, and neither should be settled by choosing a threshold.**

---

## §6 Process-lifetime statics (`st`)

`g_syncedRenderStateParameters` (`MG_Backend/DirectGLES/DirectGLES.cpp:4586`) and
`ScopedDefaultUnpackState::s_synced` are **process-level statics whose semantics are per-context**.
P5c recorded both as P6's (`CONTRACT-P5C.md:538-539`). In one process they happen to track the
process; in a server process that outlives or is reused across client contexts they hand the next
context the previous one's synced state.

**The rule: any static whose meaning is per-context or per-session is keyed by the context
generation and reset at `applier_reset`.** **`PENDING a6`** — the two above are the known members;
the audit's item 2 is to establish the complete list. This class of defect is structurally invisible
to `inproc`, which is why it is being looked for rather than waited for.

---

## §7 What P6 does not change

The rings, the codec, the record layout, the applier, both verb sinks, both backends, the barriered
predicate and the wait rule (`CONTRACT-P5E.md` §2) are untouched. The reduced path is the same one
`integration-split` runs today: P6's lane must produce the **same test-name set and the same
numbers**, not more (§8.2). The 511 `UnmigratedVerb` class-C entries are not P6's account.

---

## §8 The gate

Beyond the standard five-part gate (`docs/Disaggregated/ARCHITECTURE.md` §13.2):

1. **G1** — pull build 0/0/0/0, `.text` unchanged.
2. **G2/G14** — `integration-spawn`'s test-name set is **identical** to `integration-split`'s. A
   differing name set means the spawn lane is quietly running less.
3. `integration-spawn` fully green, at the same count as `integration-split` (today 179/179).
4. **Process-tree gate, mechanical** — child count sampled before and after every entry: exactly
   one extra while running, zero after. `HeadlessGL`'s fork pre-check leaves no orphan.
5. **Arm-proof gate (ID-124)** — every spawn entry records the child pid and `transport=spawn` in
   **its own private log**. ID-124 established that a lane can report a green arm it never ran; a
   spawn lane that fell back to monolith would pass items 1-4.
6. **Red-once per package** (R-16), one named pair each.
7. **Device** — Redmi `2f7cbe2e`, reboot-clean, paired A/B in one thermal window; OpenRA on
   Adreno 830 at split SSIM ≥ 0.99.
8. **Performance is recorded, not gated** (roadmap discipline), but one number is mandatory: **the
   socket doorbell's cost against `inproc`'s condvar.** P5d took the client thread from 15.0 to
   9.2 ms/frame and a real part of that was the doorbell path. This is P6's only expected
   regression and it gets its own paired measurement rather than a footnote.

**Negative controls**, each run red once:

| # | knob | what it must falsify |
|---|---|---|
| S1 | `MOBILEGL_IPC_SERVER_PATH=/nonexistent` | a named refusal, **not** a silent monolith fallback (§3.2) |
| S2 | `kill -9` the server mid-frame | the §5.1 latch; the client never hangs and never keeps submitting |
| S3 | child envp not scrubbed | the second safety of §3.1 catches it, by name |
| S4 | `st`'s generation reset disabled | a second context inherits the first's synced state (§6) |
| S5 | `WindowKind::AndroidNativeWindow` reaches the server | named `Fatal`, not a black screen (§4.3) |

**S2's shape must be decided before it is written.** E1 is the cautionary case: it demanded a
`Fatal` that occurs zero times in the entire run (ID-122). S2 must falsify *"the client can tell
slow from dead"*, not *"some Fatal appeared"*.

---

## §9 Amendments to the earlier contracts

1. **`CONTRACT-P5C.md:537-539`** — the EGL forwarders' control plane and the `s_synced` /
   `g_syncedRenderStateParameters` generation resets are discharged here, §4 and §6.
2. **`CONTRACT-P5C.md:56`** — `MGPApplierReset::ContextSerial` is asserted equal, not dispatched
   on, because P5c has one context per session. It says P6's multi-context shape reads it for real.
   **This contract declines that**: P6 stays single-context and multi-context moves to P12, where
   the Service lifecycle makes it concrete. The assertion stands unchanged.
3. **`Transport/ReplySlot.h`** — the 2 MiB single-slot reply cap and chunked readback are marked
   "P6 debt" there. **This contract declines them too**: they belong to P9's reverse channel.
   Taking them here would make "a transport swap and nothing else" false a second time.
4. **`CapsCodec.h:23-29`** — inherited unchanged; recorded in §1 so it is not re-litigated.

---

## §10 Explicitly not P6

Real window arrival and the `android:process=":mgl"` Service (P12); multi-context (P12, §9.2);
chunked readback and the reply-slot pool (P9, §9.3); the `DynamicBackendParameters` fixed-width
rewrite (P7); `MOBILEGL_IPC_POLL_ESCALATE` and the polling-entry escalation (P10); object-class
BARRIER-PULLED rows and the frontend-keyed twin registry (P3b/P4b, P7); Windows `pipe:` /
`unix:` (§2.6); the 511 class-C `UnmigratedVerb` entries (§7).

The E1 control's re-specification (ID-122) is **not** discharged by P6 and does not become P6's
merely because P6 starts. It is named here only because §5.3 argues it shares a predicate.
