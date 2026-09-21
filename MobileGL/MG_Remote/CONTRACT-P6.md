# CONTRACT-P6 — the backend runs in a second process

Authority: this file, beside `CONTRACT-P5.md` (table 0, byte carriers, field ownership, R-1…R-17),
`CONTRACT-P5B.md` (the class-C slots), `CONTRACT-P5C.md` (rule E, the two named exemptions, SEG_EVENT,
the guards), `CONTRACT-P5E.md` (rule F, the barriered predicate, the wait rule) and
`CONTRACT-MAGMA-RUNAHEAD.md`. Where it disagrees with any of them this file is newer and wins; §10
lists every such place. Base: `feat/disaggregated @ 785fed0b`. Every `file:line` was read at that
commit; paths are under `MobileGL/` unless they start with `docs/`.

**How to change it.** Package `c6`'s file, edited by the integrator first. A P6 package
(`lk` / `hs` / `so` / `sm` / `cp` / `dl` / `st` / `t6`) that needs a row changed goes through the
integrator; the packages compile against the rows below from day one.

**What this file is written from.** [`a6-audit-v1.md`](../../docs/Disaggregated/notes/p6/a6-audit-v1.md)
(198 rows, seven items, each adversarially re-resolved) and
[`a6-link-experiment.md`](../../docs/Disaggregated/notes/p6/a6-link-experiment.md) (the 184 symbols).
It supersedes [`P6-CONTRACT-DRAFT.md`](../../docs/Disaggregated/P6-CONTRACT-DRAFT.md), which was
written before its own audit and is **wrong in four places** (§10.1). The re-scoping against the
eventual client-in-a-VM end state is
[`P6-ENDSTATE-REVIEW.md`](../../docs/Disaggregated/P6-ENDSTATE-REVIEW.md); this file takes from it
only what P6 must not preclude, and §11 says what P6 explicitly does not take on.

---

## §0 What c6 is, and the rule above every row

`c6` lands **this file and two headers**. It changes no behaviour, adds no caller, and moves no
byte on any wire. Every rule below that describes an action names the package that implements it.
A rule with no package named is a statement about the tree as it already is.

**Rule G — a role may not name a process-local handle.** Rule E forbade naming the other role's
*memory*; rule G extends it to *identity*. A value whose meaning comes from the owning process's
kernel or loader state — `EGLDisplay`, `EGLSurface`, `EGLContext`, `ANativeWindow*`, any `void*`
window handle, an fd number, a pid, a mapped address — either crosses as a **token** defined in §2,
or does not cross. Descriptors cross only by a mechanism the transport **declares it supports**, and
where the transport declares none they do not cross at all and the bytes are transferred instead;
a transport may not invent a third answer.

> The draft hard-coded `SCM_RIGHTS` inside rule G. That is one transport's implementation of one
> clause, not the rule, and writing it into the rule is what would make the rule wrong the first
> time a transport has no descriptor passing at all.

**Rule H — a value whose meaning comes from the peer's window system never crosses. Permanently.**
Not "until P12". P12 gives the *server* its own display; it does not give the client's window a way
across. §6 is rule H's whole enforcement.

**Rule G's teeth** are §9's arm-proof gate: a spawn lane that silently ran monolith satisfies every
other gate in this file.

---

## §1 The one fact that makes c6 possible: `Spawn` is unreachable today

`ConfigLoader.cpp:339-346` is the only writer of `MG_Config::Transport` for the string `spawn`, and
it writes **`TransportMode::Monolith`** at `:344`, with a log line saying P5 does not implement it.

Two consequences, and the second is why this file can exist without changing behaviour:

1. **`ConfigLoader`'s named refusal cannot falsify itself.** It lands on the same value a server's
   resting state would have, so "the child was forced to monolith" and "the parent fell back" are
   one state. `sm` fixes this; until then, no lane can prove it ran spawn (§9.5).
2. **Every rule in this file conditioned on `Transport == Spawn` is vacuous in c6's own tree.** It
   constrains a state no process can enter. That is how D1 (§3.1) is stated normatively while
   changing nothing, and `c6`'s gate pins the vacuity so the `ConfigLoader` change cannot land early.

---

## §2 Table 0 additions

| row | encoding | invalid | consumer | package |
|---|---|---|---|---|
| `DisplayToken` / `SurfaceToken` / `ContextToken` | `Uint64`, minted by the client, dense from 1, never reused in a session. Server keeps token → native handle; it never sees the client's EGL values. `protocol.fbs`'s `display` / `surface` / `context` are already `ulong` and are these. **The client already mints them** (`MG_State/EGLState/Core.h`'s `EncodeHandle`), contrary to `Server/SurfaceControlFrame.h:26-29`, which says inproc bit-casts the handle — that comment is corrected by `cp`. | `0` on any op but `ReleaseCurrent` → `Fatal{ProtocolCorruption}` | the server's surface map | `cp` |
| `SurfaceOpKind::InitCapabilities = 11` | **Appended.** The draft claimed `ServerInitCapabilities` needs no wire form because "`CapsSnapshot` answers it". The *answer* is a `CapsSnapshot`; the **request** has a live client caller (`Client/BackendObject_Remote.cpp:126`) and the codec **refuses to encode it** (`Protocol/SurfaceOpCodec.cpp:119-122`, `InprocOnlyOpOnTheWire`). A spawn client cannot ask. | unknown tag → `Fatal{ProtocolCorruption}`, never ignored | the server's control pump | `cp` |
| `SurfaceReply.result: int` | **Appended, id 5.** Carries the `MobileGLResult` that `ServerApplyWireSurfaceOp` already computes and drops. Without it a client cannot tell a server that died mid-op from a legitimate refusal, and therefore cannot choose between arming the device-lost latch and returning `EGL_FALSE`. | — | the client's control demultiplexer | `hs` |
| `SurfaceReply.defaultFb` | **`(deprecated)`.** The encoder passes a literal `0` offset (`SurfaceOpCodec.cpp:184`) and the decoder never reads it; `kEventSurfaceChanged` is the landed carrier (P5f `fc`). The slot is **burned, never deleted** — a deleted field frees its vtable slot for the next append. | — | nobody, by construction | `hs` |
| `LinkTerms{dataPlane, wireForm, maxReplyBytes, cmdWindowBytes, stageWindowBytes}` | Appended to `Hello`/`Welcome`. `dataPlane` has exactly **one** legal value in P6, `SharedSegments`; `wireForm` exactly one, `StructImage`. Anything else is refused **by name**. The four sizes are **stated by the SERVER**. | any other value → `Refuse{ProtocolMismatch}` | the handshake | `hs` |
| `Refuse{code, detail, peer values}` | Appended `CtrlMsg` union tag. A handshake disagreement is an **answer**, not an abort (§5.1). | — | both sides | `hs` |
| `wireFingerprint` / `buildFingerprint` | The single `abiFingerprint` splits. §4. | mismatch → `Refuse`, never `Fatal` | the handshake | `hs` |
| `FatalCode` | **Unchanged**, 7 values (`protocol.fbs:248-256`). `table Fatal` appends `family: string`. §5.2. | — | — | `dl` |

All schema edits are **append-only**. A FlatBuffers table field's id is its vtable slot and an enum
value **is** the wire value; retired fields become `(deprecated)`, never disappear. `CapsSnapshot`'s
four deprecated fields are the worked precedent and their reasoning is in `protocol.fbs` itself.

> **`hs` implementation note, from c6's review.** `flatc` drops deprecated fields from the generated
> `Create*` helper. Deprecating `defaultFb` while appending `result` therefore changes
> `CreateSurfaceReply`'s arity, and the existing 6-argument call at `SurfaceOpCodec.cpp:183-184`
> would **silently bind its `/*defaultFb=*/0` to `result`**. `hs` converts that call site to
> `SurfaceReplyBuilder` with explicit `add_*` in the same commit, so a positional mistake is
> impossible rather than merely unlikely.

---

## §3 Role, Dial, and the six predicates

### 3.1 D1 — what `Transport` reads as in the server process

**`TransportMode::Spawn`. The child does not force `Monolith`.**

`ARCHITECTURE.md:488`'s literal instruction ("`mobilegl_server_main` hard-sets `MG_Config::Transport`
to `Monolith` before reaching `Init()`") is **amended by this file**. a6 measured why: 226 live lines
test `Transport != Monolith` to select the **server** arm, 148 of them in `MG_Backend`. Forcing
`Monolith` flips all of them to frontend glue in the one process with no frontend. The sharpest
instance is `PipeInputs::IsLive()` (`MG_Impl/Pipe/PipeFill.cpp:2135`): forced monolith, it reads
`MG_State::pGLContext`, which is null there, so **58 `MGB_CTX_LIVE` guards all answer "not live"**.

Anti-recursion moves to a **separate, non-inherited axis**: `MG_Config::Dial == DialMode::No` in the
child. The envp scrub stays as the second, independent catch, and S3 (§9) still falsifies it —
and the diagnostic must **name which of the two fired**, or the double safety is untestable as two
things. Package: `sm`.

### 3.2 D1c — there are six predicates, not three

a6 proposed three. Three do not cover the tree.

| predicate | asks | pull-build shape |
|---|---|---|
| `MGPipeSplitActive()` | do records cross a role boundary at all | `#else` returns `false` |
| `MGPipeServerArm()` | am I executing as the server **right now** | `#else` returns `false` |
| `MGPipeShouldDial()` | should this process connect out | `#else` returns `false` |
| `MGPipeSessionLive()` | is a peer session live | role-defined; `#else` `false` |
| `MGPipeBlocksAreDistinct()` | do the two roles share one storage object | `#else` `false` |
| `MGPipeBackendIsLocal()` | is the backend in **this** process | `#else` `true` |

`ContextEpoch::Server` and `ActiveXfbState`'s role index are **`MGPipeSplitActive()`** sites, not
`MGPipeServerArm()` sites: they are partition keys, and the `ServerArm` disjunct would make a
partition key thread-dependent.

**D14 — all six must fold away in the pull build**, and the required shape is `SlotCaps.h`'s: the
non-disaggregated build gets a **different definition** under `#else`, not a runtime-false predicate.
`MGPipeSplitActive()` must additionally remain exactly one load of a process global and must never
be latched into a `static Bool`. G1 is 0/0/0/0 and `.text` unchanged; anything else is a c6 defect.
Package: `sm`.

### 3.3 D13 — rename before introducing

`MGPipeRoleSplitActive()` (`MG_Backend/MGPipe/PipeInputs.cpp:292`) means
`Ipc.RoleSplitState && Transport != Monolith` — the **rehearsal knob**, default off. It is renamed
to **`MGPipeRoleSplitRehearsalActive()`**, which is what it has always meant, and
`MGPipeBlocksAreDistinct()` is introduced beside it as the wider predicate. a6's shape — rename, then
widen the same function — is **overturned**: a widened function under a narrow name is a name that lies.

Blast radius, measured at this head: **12 sites**, of which `DualBlockScenario.cpp`'s assertion
*message* embeds the old name and must be updated with it. Package: `sm`.

### 3.4 D10 — the four guards that cannot arm in a server-only process

| guard | today | becomes | package |
|---|---|---|---|
| `MGPipeApplierReset` layer 2 (`MG_Pipe/PipeApply.cpp:1353-1355`) | needs `ClientSession::Active() != nullptr`, permanently null in a server process | middle conjunct → `MGPipeSessionLive()`; third → `!MGPipeServerArm()` | `sm` |
| `MGPipeServerBlockNoteIdentity` (`PipeInputs.cpp:314`) | early-returns unless the rehearsal knob is on | gate → `MGPipeBlocksAreDistinct()` | `sm` |
| `CountBarrierPull`'s unconditional Fatal arm (`PipeInputs.cpp:174`) | knob-gated | rides `MGPipeBlocksAreDistinct()` | `sm` |
| the two `RefuseFromApplyThread` helpers (`MG_Impl/Pipe/SlotAllocator.cpp:29-57`) | `ServerLoop::OnApplyThread()` | OR'd with a process-role fact set once by `ServerMain` | `sm` |

**The one that crashes rather than merely failing to arm.** With `MGPipeServerBlockNoteIdentity`
early-returning, a spawn server's `gPipeInputs.ContextIdentity()` stays `nullptr` for the life of the
process, and `DirectGLES.cpp:199-213`'s fb-slot memo does a **raw pointer compare with no generation**
against it — so `nullptr == nullptr` reads as a **cache hit** and the first call uses an
uninitialised slot. `PipeInputs.h:878-884`'s own comment says a named Fatal was intended there.
`sm` lands the gate change; `st` lands the named Fatal.

---

## §4 The handshake (`hs`)

### 4.1 One equality test answers three different questions today

`AbiFingerprintInputs` (`Transport/SessionRings.h:544-566`) mixes wire properties, struct-layout
properties and pure build identity into one `Uint64`. It splits:

**`wireFingerprint` — always compared.** Inputs, in order:
an ordered **catalogue digest** over `PipeCalls.def`'s `(opcode, payload struct name, call-flag row,
WaitClass)`; a **payload layout digest** derived from `PipeFields.def` (per member: name, `offsetof`,
`sizeof`); the record-header layout; both blob-codec version stamps; the format-capability table's
two extents; `kOpCount`; `MOBILEGL_ABI_VERSION`; endianness; pointer width; **and**
`sizeof(MGPCaps)` and `sizeof(DynamicBackendParameters)`.

Those last two **stay compared** because they are wire facts, not build facts: `MGPWireRec_GetCaps`
is composed from `sizeof(MGPCaps)` (`MG_Pipe/generated/PipeWire.inc`). `sizeof(GLFunctionsTable)` is
**deleted** from the mix: it never crosses the wire, so adding one backend slot today invalidates
every peer for no wire reason.

> **Why the layout digest is not optional.** `SessionRings.h:561-565`'s own comment says the git
> stamp is in the mix because *two builds of the same sizes can still disagree about a FIELD ORDER,
> which no sizeof can see*. A catalogue digest over `(opcode, struct name, flags, WaitClass)` sees
> neither a member reorder inside a payload struct nor a width change. So the widely-proposed "drop
> the git stamp, add a catalogue hash" is **wrong as usually stated** — it removes a guard and does
> not replace it. The layout digest is the replacement, and `gen_pipe.py` already CI-enforces that
> `PipeFields.def` names every member, so it is a generator addition and not a toolchain.

**`buildFingerprint` — compared only while `Dial == Fork`.** Under fork the child *is* the same
binary, so comparing it catches a stale `MOBILEGL_IPC_SERVER_PATH` pointing at an old
`libMobileGLServer.so` — a real P6 failure mode. **P6's policy is therefore strictly stronger than
today's, not looser**; the end state is a policy flip, not a redesign.

### 4.2 The missing build stamp

a6 found `CMakeLists.txt:188-195` takes the git hash with **no `RESULT_VARIABLE` and no fallback**, so
a failed `git rev-parse` silently yields `""`. Verified live: building this worktree from WSL
produces `GIT_COMMIT_HASH_SHORT ""`, because the worktree's `.git` file holds a Windows path WSL's
git cannot resolve. Two builds from different commits then get **identical** fingerprints and the
stamp silently stops doing its documented job.

**The fix may not touch `GIT_COMMIT_HASH_SHORT`.** That macro is baked into
`glGetString(GL_VERSION)` (`MG_Impl/GLImpl/Getter/GL_Getter.cpp:614`), so redefining it changes an
**application-visible GL string** and the pull build's `.rodata` — a behaviour leak and a G1 break
at once. Instead `MGGitHash.h.in` gains a **second** pair, `MOBILEGL_BUILD_STAMP_VALUE` and
`MOBILEGL_BUILD_STAMP_PRESENT`, consumed only by the fingerprint, and the CMake step gains a
`RESULT_VARIABLE`. A build with no stamp sets `PRESENT` to 0, and a `Dial == Fork` handshake whose
peer reports `PRESENT == 0` is a **named refusal**: under fork the two peers are supposed to be the
same binary, and without a stamp that premise cannot be checked at all. Package: `hs`.

### 4.3 Two verified asymmetries `hs` also fixes

- The client checks only `abiFingerprint` and **never `abiMajor`/`abiMinor`** (`ClientSession.cpp:566-568`).
- `Hello::pid` is hard-coded `0` and `Welcome::serverPid` echoes the client's, so it is always 0
  (`ClientSession.cpp:519-521`, `ServerSession.cpp:532-534`). §9.5's arm-proof gate wants the child
  pid; `serverPid` is its natural carrier and must start carrying it.

### 4.4 Segment geometry (D-size)

`MOBILEGL_IPC_RING_MB` / `STAGE_MB` are read from the **client** process's env with ceilings of
1024 / 4096 MiB (`ConfigLoader.cpp:363-364`), while the **server** creates the segments. Under spawn
that is a guest environment variable sizing an allocation inside the renderer process. The four sizes
become **server-stated** `LinkTerms` fields; the client's values become a *request* the server may
clamp, and the clamped values are reported in `Welcome`.

`ShmSegment::Adopt`'s `fstat` "do not trust the peer's declared size" check
(`Transport/ShmSegmentPosix.cpp:127-133`) is today the **only** bound, and it has no analogue on a
link with no fd. The compile-time ceiling that refuses an oversized announcement by name is
**P6.5's** to define; P6 does not pretend to have answered it.

---

## §5 Death, refusal, and what P5e changed

### 5.1 The handshake never aborts

A version or terms disagreement is a `Refuse{code, detail, peer values}` control message plus
`MOBILEGL_ERR_PROTOCOL_MISMATCH` on **both** sides: the server sends `Refuse` and exits 0, the client
reaps the child and refuses **by name**, never falling back to monolith. Both `FatalAbiMismatch` call
sites go (`ServerSession.cpp:471-483`, `ClientSession.cpp:565-568`). The worked shape is eight lines
away in the same function: `ServerSession.cpp:456-466` already returns
`MOBILEGL_ERR_PROTOCOL_MISMATCH` for a malformed frame, with the correct `msg_as_Hello() != nullptr`
guard. Package: `hs`.

### 5.2 `Fatal{}` keeps aborting in P6, and gains one funnel

a6 censused **92 abort sites** under `MG_Remote/` carrying **30 distinct family words**, against a
**7-value** wire `FatalCode`. So `Session::Fail(FatalCode, detail)` as usually proposed **cannot carry
today's vocabulary in its first argument**.

**D2 ruling.** The first argument is an internal `MGFatalFamily`, generated from a new
`MG_Remote/FatalFamilies.def` — one row per family word, each row carrying its `FatalCode`
projection. The **wire** keeps `FatalCode` unchanged; `table Fatal` appends `family: string`. The
projection is a **total table**, not a switch with a `default:` arm — this build has **no `-Werror`
anywhere** (verified: `CMakeLists.txt`, every `*.cmake`, both workflows), so a gate premised on
`-Wswitch` failing the build is fiction.

`Session::Fail` stays `[[noreturn]]` in P6 and still ends in `abort()`. Log lines, CI greps and every
red-once are byte-identical. What the funnel buys, and what makes it worth `dl`'s two days:

- **one** place that publishes a `SessionFault{family, detail, seq, op}` frame to the peer **before**
  dying, so the guest's log names which record killed the session instead of reading a bare EOF;
- one telemetry point;
- a CI census gate refusing any **new** bare abort outside the funnel.

**The policy flip is not P6's.** Making `Fail` return needs a real return path invented at every site
whose callers rely on `[[noreturn]]`, and a6 counted how many do. That is `Ph`'s (§11).

Two sites carry **no `Fatal{` marker at all** (`Client/PersistentMapTracker.cpp:1052-1058`,
`Transport/WireLog.cpp:41-50`), so "the log stays verbatim" is not true of them and the family grep
cannot see them. `dl` gives each a family tag.

### 5.3 The device-lost latch does not exist, and P6 writes it

`P6-CONTRACT-DRAFT §5.1` says the latch "is implemented unchanged". It is not implemented at all:
`FatalCode::DeviceLost`/`ServerCrashed` have zero producers and zero consumers,
`glGetGraphicsResetStatus` returns `GL_NO_ERROR` unconditionally
(`MG_Impl/GLImpl/Getter/GL_Getter.cpp:2855-2861`), and `MOBILEGL_IPC_RESPAWN` / `IDLE_EXIT_S` are
parsed **nowhere** (only a comment at `Config.h:466`). Today a dead server leaves the client
silently `DECLINE`-ing every verb forever with `glGetError` clean.

**It is a package (`dl`), not carriage.** The latch is session-scoped and set from
`Doorbell::Dead()`, **never from a timeout**. `MOBILEGL_IPC_RESPAWN=1` is a named refusal until some
stage implements the re-push.

### 5.4 Slow, dead, and frozen are three states

- A published, never-applied record is **discarded at the latch, not waited on**.
- **Slow and dead are distinguished without a timeout threshold.** A server one frame behind is
  P5e's intended steady state; a server that is gone is `Fatal{ServerCrashed}`. `SocketDoorbell`
  already latches `m_dead` on EOF — take the fact from there.
- **A frozen peer produces no hangup**, and a6 found the tree has no defence: the control-reply wait
  `m_controlDone.wait(lock, pred)` (`Server/ServerLoop.cpp:724`) has **no deadline**, while the ring
  side has `kBarrierTimeoutMs`. A SIGSTOP'd or freezer'd server wedges the GL thread inside
  `eglMakeCurrent` with no diagnostic.

**D5 ruling — two waits, two answers.** (a) The server's *in-process* mailbox wait gains a bound and
its expiry is `Fatal{ControlReplyTimeout, "<op>@<seq>"}`. (b) The client's *cross-process* reply wait
is bounded by `MOBILEGL_IPC_CONTROL_TIMEOUT_MS` (default 5000) and its expiry is **not** fatal: the
client consults the doorbell's death latch — dead latches device-lost, alive-but-silent is a named
diagnostic. Packages: `cp` (a), `dl` (b).

---

## §6 The control plane (`cp`)

### 6.1 What `fc` landed, and what is left

P5f package `fc` framed the twelve `Server*` forwarders as value-only `SurfaceControlFrame`s and
landed the wire codec. The draft concluded `cp` is "transport only". **Three things are not
transport**, and a6 found all three:

1. The `InitCapabilities` **request** has no wire form (§2).
2. `SurfaceReply` cannot carry a `MobileGLResult` (§2).
3. **Neither side of the control plane is wired to a socket.** The apply thread's `ready` predicate
   has exactly three terms — stop, `ControlIsPending()`, `cmdHead != LocalTail()` — and a socket is
   not among them (`ServerLoop.cpp:398-402`), so a `SurfaceOp` arriving on the wire is read by
   **nobody**. The client's pump consumes only `CapsSnapshot` and drops everything else with a WARN
   (`ClientSession.cpp:1425-1432`).

### 6.2 D4 — who reads the control socket

A dedicated **`mgl-srv-io` thread**: `ReceiveFrame` → `DecodeWireSurfaceOp` → post into the
**existing** one-slot mailbox as an ordinary non-apply-thread poster. `ServerLoop.cpp:673-727` is not
changed. The io thread is additionally the server's **only** sender on the control transport:
`PublishCapsSnapshot` enqueues and the io thread drains one outbound FIFO — which is what fixes the
ordering hazard that `CapsSnapshot` is pushed from **inside the dispatch, on the apply thread,
mid-op** (`ServerLoop.cpp:959-969`).

Rejected: adding a fourth term to the apply thread's `ready` predicate. That puts frame decode on the
thread holding the native context and makes every control arrival a wake of the applier.

### 6.3 D7 — `seq`

The **client** mints, always, dense from 1, never reused in a session. `seq == 0` arriving on the
wire is `Fatal{ProtocolCorruption, "SurfaceOp.seq"}` at the **decoder**, not at
`RunSurfaceControlFrame`, whose local mint (`ServerLoop.cpp:659-662`) stays and becomes reachable only
for a local, non-wire post. Correlation lives in the client's control demultiplexer;
`DecodeWireSurfaceReply` keeps its void signature and stops being the place correlation was expected.

### 6.4 `ForgetCurrentTuple`

The N-3 rule holds identically on the frame path: the forgetting calls are inside the dispatch
(`ApplySurfaceControlFrame`) and `ServerLoopTest`'s C7/N-3 controls drive them through framed posts
unchanged. No change.

---

## §7 Windows — rule H's enforcement (`cp`)

### 7.1 The blacklist is open, and the end state's first guest walks through it

`WindowBackendForWireWindowKind` (`Protocol/SurfaceOpCodec.cpp:104-113`) **accepts**
`WindowKind::X11 → WindowBackend::X11` and `Win32Hwnd → Win32`. `DecodeWireSurfaceOp` (`:141-179`)
refuses only `AndroidNativeWindow` and `MetalLayer`. An accepted kind's `nativeToken` is copied
through and `UnpackWindowHandle` (`Server/ServerLoop.cpp:875-882`) casts it to `void*`
**unconditionally**, whence it reaches `eglCreateWindowSurface` in the server process.
`DetectWindowBackend()` is a compile-time `#if` chain (`MG_Impl/EGLImpl/EGLImpl.cpp:59-70`), so a
guest cannot avoid announcing its own window system.

### 7.2 D8 — whitelist, and in P6 the accept set for a window op is empty

- `X11` → `Fatal{UnmigratedSurface, "X11@P12"}`; `Win32Hwnd` → `"Win32Hwnd@P12"`.
- `None` on a **window** op → `Fatal{ProtocolCorruption, "SurfaceOp.windowKind"}`. Today it is
  accepted, maps to `WindowBackend::Unknown`, and the raw token is copied through anyway.
- Separately and independently: `nativeToken != 0` on **any** op →
  `Fatal{ProtocolCorruption, "SurfaceOp.nativeToken"}`.
- `Pbuffer` / `Surfaceless` are surface **shapes**, not window backends, and remain the only things
  P6 lands.

**Do not append `WindowKind::ServerOwned` in P6.** The schema is append-only, so an enumerator
appended now and re-meant later is a wire value burned; P6 has no consumer for it. P12 appends it.

### 7.3 The client emits unconditionally, and P6 must decide what that costs

`Client/BackendObject_Remote.cpp:214-216` posts `SetWindowHandle` with no split-arm check, so on a
real Android app the **first `eglCreateWindowSurface` aborts the server process** and the client sees
only a dead peer. `cp` mirrors the refusal on the **client** side, so the diagnostic names the cause
in the process that caused it. This is P6's, not P12's, because P6's device lanes run on a phone.

### 7.4 `m_windowHandle` has four stores, not one

a6's precise shape, correcting the corpus (and `P6-ENDSTATE-REVIEW §2.5`): four stores
(`MG_Backend/BackendObject.cpp:475`, `:365`, `:206`, `:207`), five reads, and `:283` is a **call**,
not a store. `.Width`/`.Height` have **no readers**. The smallest latch point for a server-owned
window is `RegisterEGLWindowSurface` (`:232-238`), and the non-obvious fourth edit is the
`sameHandle` dedup key (`BackendObject_DirectGLES.cpp:962-967`), which under server ownership never
matches and would rebuild the native context on every `eglCreateWindowSurface`. Recorded here for
P12; P6 changes none of it.

---

## §8 The data plane (`lk` implements, `c6` declares)

### 8.1 Why a seam at all

`ITransport.h:16-20` says in its own header that the hot path bypasses it: it is the **control**
plane, and that scope is already correct for a socket. Below it there is **no interface of any kind** —
`Ring.h`, `ReplySlot.h` and `EventRing.h` take raw `void* base + capacity`. So "swap the transport"
has nothing to swap.

`c6` declares `Transport/ILink.h` and `Transport/StreamLink.h`. `lk` implements `ShmLink` by moving
today's `SessionSegments` / `Ring` / `ReplySlotPool` / `EventRing` behind the interface, line for
line, with **zero behaviour change**.

### 8.2 The seam is a setup-time interface, not a hot-path one

`ILink` hands out POD handles **once at attach** and then stays out of the way. No `ILink` virtual is
called per record, per publish, or inside any spin predicate. Three grounds, and the second is
decisive:

1. Two virtuals per record at the 4000 records/frame that `SEG_CMD` is sized against is ~8000
   indirect calls/frame — about 0.5% of the measured 15.72 ms client frame. Affordable, but the only
   lane that could measure it has ±4% run-to-run dispersion, **eight times the effect**, so the gate
   `P6-ENDSTATE-REVIEW` demands could only ever print "within noise" — a green that proves nothing.
2. **The spin predicate forbids it outright.** `Doorbell::Wait` is a non-virtual template whose spin
   loop calls `ready()` up to `SpinItersPerUs() * spinUs` times; at 916 waits/frame with 99.8%
   resolved inside the spin, the predicate runs 10⁵–10⁶ times per frame and reads the watermarks
   straight off the page. That is the 31.58%-of-the-GL-thread path. A virtual there is not a rounding
   question.
3. The encode path already depends on four inline accessors around its single `Reserve`, and
   virtualising `Reserve` alone would force either four more virtuals or moving R-10's wrap counters
   behind the seam — and those are deliberately encoder-owned.

**The gate is therefore an `objdump` call-count of the encode path, not a wall clock**: after `lk` the
path must have no more out-of-line calls than today. That is falsifiable; "within noise" is not.

### 8.3 `RingControl`'s regroup is `lk`'s, and it is a wire break the day a second build exists

The watermark cache line is `{appliedSeq, submittedSeq, retiredSeq, completedFrameSerial,
presentAckSerial}`; `eventRingFull`/`eventDropped` sit in a **separate** `alignas(64)` group with
`serverEpoch`/`ringGeneration`/the two park flags. And `submittedSeq` is **producer-written**
(`Ring.h:50`) while the other four are consumer-written — so the group has **mixed writers**, and any
"single-writer progress block aliased over the watermark line" is unsound.

`lk` therefore **regroups**: `submittedSeq` moves to the producer's own line, beside `cmdHead`,
which is the other thing the producer writes; what is left — the four consumer-written watermarks —
becomes a named `LinkProgress` member with per-field `offsetof` static_asserts.

> **Correction, found while `lk` was measuring its own blast radius.** An earlier revision of this
> section, and of `ILink.h`, also promoted `eventRingFull`/`eventDropped` into the watermark group
> and called the result single-writer. **Both were wrong.** `eventRingFull` has two writers: the
> server latches it with `store(1)` when `SEG_EVENT` fills (`Transport/EventRing.h:140`) and the
> **client clears it with an `exchange(0)` on every drain** (`:200`). Promoting it would put a
> per-drain RMW by the client on the same cache line as `appliedSeq` — which the server
> release-stores every 64 records and the client's spin predicate reads 10⁵–10⁶ times per frame.
> That is new false sharing on the hottest line in the system, and it would land in exactly the
> numbers this package's gate has to hold constant. The two flags stay in the doorbell/generation
> group, where mixed writers already live beside `consumerParked`/`producerParked`, and cross the
> seam through a separate `EventFlags()` accessor on its own line.

It is free **today** — both peers are the same binary, enforced by the fingerprint — and a wire break
the day a second build exists. It is **`lk`'s and not `c6`'s** because the rename blast radius is
**109 sites in production alone**, not the 39 an earlier draft assumed, and because five of them sit
behind `#if defined(MGITEST_SPLIT_RUNTIME_PEEK) && !defined(__ANDROID__)` — an Android configure
compiles them out, so a missed rename is green on device and red only on a desktop split-peek build.
`lk` publishes the per-file itemisation before it starts.

### 8.4 What `StreamLink` is for

Every method is a named refusal, `Fatal{UnmigratedDataPlane, "StreamLink@P6.5"}`. It exists so the
seam's adequacy is a **compile**, not an argument: if `ILink` cannot name what a byte stream needs,
`StreamLink` cannot be declared against it. Writing it is what revealed that the seam needs
`FlushProgress()`, `ResolveSpan()` and a reply pair — three methods the brief's own list
(progress transmission, send window, flush) did not name.

`c6`'s red-once: delete one `ILink` method `StreamLink` declares and the build goes red.

### 8.5 The purity gate (`lk`)

After `lk`, the identifiers `RingControl`, `RingProducer`, `RingConsumer`, `ReplySlotPool` and
`EventRingConsumer`, and any `m_shm.` expression, appear **only** inside `Transport/ShmLink.*` and
the transport's own tests. Same shape as the existing A/B/C purity gates. The baseline is recomputed
mechanically at `lk`'s head and published per file — the earlier draft's ratchet number was wrong by
a factor of three, and a ratchet installed at the wrong number is red on day one for reasons that
have nothing to do with a leak.

### 8.6 `SEG_STAGE`'s cursors are deliberately dead

`Ring.h:105-129` states it and a test pins it: `SEG_STAGE` is **not a ring**; staging is an
encoder-local linear allocator reclaiming on `retiredSeq`, and all three stage cursors stay zero for
the whole of P5. A stream link's send window either revives them **completely** or does not use them;
the header names the middle state as *a guaranteed hang rather than a slow path*. P6.5's, recorded
here so it is not rediscovered.

---

## §9 The gate

Beyond the five-part gate (`docs/Disaggregated/ARCHITECTURE.md` §13.2):

1. **G1** — pull build 0/0/0/0, `.text` unchanged. Every predicate in §3.2 folds away.
2. **G2/G14** — `integration-spawn`'s case-name set identical to `integration-split`'s.
3. `integration-spawn` green at the same count as `integration-split`.
4. **Process tree, mechanical** — exactly one extra child while running, zero after; `HeadlessGL`'s
   fork pre-check leaves no orphan.
5. **Arm proof (ID-124)** — every spawn entry records the child pid and `transport=spawn` in its own
   private log. A spawn lane that fell back to monolith passes items 1–4. §1 is why this gate is not
   optional: today the fallback and the intended state are **the same value**.
6. **Red-once per package (R-16)**, one named pair each.
7. **Device** — Redmi `2f7cbe2e`, reboot-clean, paired A/B in one thermal window.
8. **Performance recorded, not gated**, but three numbers are mandatory: the socket doorbell's cost
   against `inproc`'s condvar; `SEG_STAGE` bytes/frame; and records/frame **post-chunking**. The last
   two are takeable on the existing inproc lane today and `MEASUREMENTS.md:342` says in as many words
   that no post-chunking distribution exists.

> Item 8's wording must be able to **accept a negative regression without looking suspicious**: today
> the client spins ~1600 times/frame on a shared cache line with 99.8% of waits resolved there, and a
> socket link has no such line. Converting that into ~4 blocking reads/frame may be **cheaper**.

Negative controls, each run red once:

| # | knob | what it must falsify | package |
|---|---|---|---|
| S1 | `MOBILEGL_IPC_SERVER_PATH=/nonexistent` | named refusal, not a silent monolith fallback | `sm` |
| S2 | `kill -9` the server mid-frame | the latch; the client never hangs and never keeps submitting. Red-once the **wrong implementation** too: set the latch from a timeout and the paired control "a server one frame behind is the normal state" must go red | `dl` |
| S3 | child envp not scrubbed | `Dial = No` catches it **and the diagnostic names which of the two catches fired** | `sm` |
| S4 | inherited P5f epoch invalidation disabled | a replaced context inherits stale synced state; a still-live XFB lifetime retains its paused span | `st` |
| S5 | `WindowKind::X11` with a plausible XID reaches the server | named Fatal, **and the gate falsifies that the token reached `UnpackWindowHandle`**. Today's S5 tests only `AndroidNativeWindow` and stays green with §7.1's hole open | `cp` |
| S6 | a peer offers `LinkTerms.dataPlane = StreamOnly` | refused by name, **and the server survives to serve the next connection** | `hs` |
| S7 | a bare `abort()` added outside `Session::Fail` | the census gate goes red | `dl` |
| S8 | — | `integration-spawn` asserts `SessionFaultCount() == 0` over a whole run: the **good-path** assertion E1/ID-122 wanted and could not express, and which S5/S6 can falsify | `t6` |

---

## §10 Amendments

### 10.1 To `P6-CONTRACT-DRAFT.md`, which this file supersedes

1. §1's "ABI fingerprint | Unchanged … same-machine, same-binary" → §4.
2. §5.1's "The latch … is implemented unchanged" → **false**; §5.3.
3. §4's "P6's remainder is TRANSPORT ONLY" → **false in three ways**; §6.1.
4. §7's "a transport swap and nothing else" → **no longer true**. This file adds `lk`, `hs` and `dl`
   to the package list and says so here rather than smuggling it.

### 10.2 To the earlier contracts

1. `CONTRACT-P5C.md:537-539` — EGL value framing was discharged by P5f `fc`, the unpack/render-state
   epoch work by P5f `fs`. P6 carries the existing frames over the socket and verifies the inherited
   lifecycle rules in the second process.
2. `CONTRACT-P5C.md:56` — P6's multi-context shape. **Declined**: P6 stays single-context. §11.
3. `Transport/ReplySlot.h`'s "P6 debt" for the 2 MiB reply cap and chunked readback. **Declined**:
   P9's. Note `P6-SPAWN-PLAN §8.5` already reassigned it and the header was never updated — two
   documents in the tree disagreed; this file settles it.
4. `Ring.h:71-77` — `appliedSeq` is excluded from lazy publication **by name**. P6 does not change
   the behaviour; it records that any future stream link **amends** this rule rather than inferring
   around it. Several designs have argued the late-never-early invariant licenses lazy `appliedSeq`;
   it explicitly carves `appliedSeq` out.
5. `ARCHITECTURE.md:488` — the child-forces-monolith rule. **Amended**; §3.1.
6. `ARCHITECTURE.md:9` and `MG_Backend/MGPipe/PipeInputs.h:281`, `:744`, `:957`, and
   `P5F-WIRE-COMPLETENESS §1.4` — "the server does not link `MG_Impl`". **False at this head.** §12.

---

## §11 Not P6

Real window arrival and the `android:process=":mgl"` Service (P12); multi-context (P12); chunked
readback and the reply-slot pool (P9); `DynamicBackendParameters`' fixed-width rewrite (P7);
`MOBILEGL_IPC_POLL_ESCALATE` (P10); remaining monolith-only frontend-object / twin-registry glue
(P3b/P4b) and the named P7 functionality debts; Windows `pipe:` / `unix:`; the 511 class-C entries.

**The stream data plane is P6.5**, and it must land before P11 and before P9 picks a mechanism.
**Untrusted-guest hardening is `Ph`**, and it must land before any shipping server app accepts a
connection from a guest it did not produce — including the `Fatal`-policy flip (§5.2), the
handle-slot budget, and the caps in §12.

E1's re-specification (ID-122) is **not** discharged by P6 and does not become P6's because P6 starts.

---

## §12 Recorded debt

### 12.1 The server image links `MG_Impl` in P6 (D12)

**P6's server image is the whole `libMobileGL.so`** — which is what `ARCHITECTURE.md:496`'s "one
shared library, two roles" always meant. The assertions that the server does not link `MG_Impl` are
false and become debt with named owners.

Measured: **184 symbols**, 177 of them real function calls, that the server set needs and only
`MG_Impl` / `MG_State` / `MG_Remote/Client` define. Owners: **P7 101**, **P3b/P4b 14**, both backends
**60**, **P6's own 6**. There is additionally **no module target to link** — `SOURCE_FILES` is one
flat list feeding both library targets — so establishing module boundaries is itself named work,
owed to P13. Method and full symbol list:
[`a6-link-experiment.md`](../../docs/Disaggregated/notes/p6/a6-link-experiment.md).

**P6's own six** are not P7's and are `sm`/`lk`'s to clear: `Server/PipeApplier.cpp` calls
`Client::ClientSession::NoteApplyThreadEnteredApplier()` / `NoteApplyThreadLeftApplier()`;
`Wire/PipeWireCodec.cpp` needs `MG_State::GLState::DecodeProgramArchive` and
`Client::AdoptTierIsEmulate()`; `MG_Pipe/PipeApply.cpp` needs `Client::ClientSession::Active()` and
`AdoptTierIsEmulate()`.

**Gate**: a ratchet recomputes the 184 in CI and asserts it only ever falls.

### 12.2 D11 — five unbounded peer-driven allocations, capped, with no `try`/`catch`

Every peer-declared count is bounded **before** it reaches an allocator and the bound is a named
refusal. `MG_Remote` gets no `try`/`catch`: a cap that is checked is better than an exception that is
caught, and the funnel (§5.2) is where the diagnostic lives.

| site | today | package |
|---|---|---|
| `MG_Pipe/PipeApply.cpp:1563-1569` | resizes to a peer-supplied `Cso.Slot + 1` with **no slot limit**; every sibling create-family goes through `RecordAt`'s bounded path. `Slot = 0xFFFFFFFE` asks for ~1.7 TB | `dl` |
| `Server/PipeApplier.cpp:416-441` | readback scratch resized from `Box.W * Box.H * bpp` **before** any size gate. 65535² RGBA8 asks for ~17 GB | `dl` |
| `MG_State/.../ProgramArtifactsCodec.cpp:166-175`, `:216-223` | count bounded by remaining **bytes**, then multiplied by `sizeof(element)` | `dl` |
| `Server/StagedTextureStore.h` | resize not bounded against the level's declared extent | `Ph` |
| `SEG_EVENT` overflow | a guest that merely stops draining kills the host renderer in 60 s | `Ph` |

### 12.3 One guest per server process (D9)

For the whole of P6, one server process serves exactly one client session, and it is a **structural
fact, not a preference**: `g_Display`/`g_Context`/`g_Surface`/`g_Config` are per-process file-statics
and `InitDisplayAndContext` opens with an unconditional `DestroyEGLContext()` whose last act is
`eglTerminate` — a **process-wide** EGL teardown. `ServerSession::Accept`'s same-object arm
(`ServerSession.cpp:425`) returns `MOBILEGL_ERR_INVALID_ARGUMENT` **with no log line at all**; `sm`
gives it one. Under `Dial == Fork` a second guest is unreachable by construction, so the invariant
becomes falsifiable only under `Dial == Connect`, which P6 does not land.

### 12.4 Settled by ruling rather than by experiment

- **Who runs the applier in the server process**: `ServerMain` **must** drive it through
  `ServerLoop::Start()`, so `g_applyThreadKey` is published and the dozen `OnApplyThread()` guards
  keep meaning what they mean. Owner `sm`; red-once drives the applier without `Start()`.
- **The server's exit primitive**: `_exit`, after an explicit log flush. It decides whether
  `g_processTeardown` is set and therefore whether seven twin destructors take their early-out.
  Owner `sm`.
- **The handshake asserts the resolved subsystem mask**; a mismatch is `Refuse`, not `Fatal`. The
  envp scrub list does **not** include `MOBILEGL_PIPE_*`, so the two processes can resolve different
  masks from their own environments. Owner `hs`.
- **P6 maps no segment read-only.** `ShmSegment.h:62-63`'s stated design — the peer gets
  `SEG_CMD`/`SEG_STAGE` read-only on the server side — **cannot be implemented as written**, and the
  comment is corrected rather than left to mislead. Owner `lk`.
- **`DualBlockScenario`'s `serverIdentity != nullptr`** is an in-process fact, and G2/G14 requires
  the spawn lane to keep the case name. Its assertions are **re-homed**, not weakened: `DualBlockPeek`
  becomes a control-plane query answered by the server process. The declared fallback — assert
  `MGPipeBlocksAreDistinct()` on the client side and carry the identity half by §3.4's server-side
  Fatal — must be **declared here** if taken, not discovered in review. Owner `t6`.
