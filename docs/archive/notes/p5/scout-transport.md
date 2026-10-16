# P5 scout — MG_Remote/Transport as it stands, and what P5 must add

Tree: `/home/swung/w7/pipe` @ `a29807cc` (P4a landed), branch `feat/disaggregated`. READ-ONLY survey;
nothing was built or run. All line numbers are from that commit.

TL;DR for the brief writer:

1. **The wire layer is complete and well tested, but it has ZERO production callers.** Nothing in
   `libMobileGL.so` constructs a `RingProducer`/`RingConsumer`, sends a frame, or parses a
   `CtrlEnvelope`. The only includers of `Ring.h`, `Framing.h` and `protocol_generated.h` outside
   `Transport/` itself are the five test binaries under `MG_Test/Wire/`.
2. **`InProcessTransport` today is a message-queue pair + two condvar doorbells. It touches neither
   the ring nor any codec.** P5's gate "inproc runs the SAME G3 codec as spawn" cannot be satisfied by
   editing `InProcessTransport.cpp`; it needs a new ring-owning object above `ITransport` (§3).
3. **The G3 codec has no encoder at all and a decoder that is a bounds gate returning `false`.**
   `MGPipeApplyWireRecord` (`MG_Pipe/generated/PipeWire.inc:708-928`) validates every record's bounds and
   then reports "not applied", by its own comment "because no applier exists until P5 wires
   `MG_Remote/Server/PipeApplier.cpp`". `MGPWireRec_*` has no writer anywhere in the tree.

---

## 1. File by file

`MobileGL/MG_Remote/` has exactly two subdirectories: `Protocol/` and `Transport/`. There is **no
`Client/` and no `Server/`** — ARCHITECTURE.md:573-576 marks both as `[P5+]`, and `SocketTransport`
under `Transport/` as `[P6]`.

Sizes: `Doorbell.cpp` 259, `Doorbell.h` 268, `FdPassing.cpp` 323, `FdPassing.h` 67, `Framing.h` 208,
`ITransport.h` 130, `InProcessTransport.cpp` 293, `InProcessTransport.h` 77, `Ring.cpp` 306,
`Ring.h` 256, `ShmSegment.cpp` 49, `ShmSegment.h` 87, `ShmSegmentPosix.cpp` 191,
`ShmSegmentWin32.cpp` 162, `WireLog.cpp` 33, `WireLog.h` 38 — 2747 lines total.

### Ring.h / Ring.cpp — fully implemented, no stub

* `RingControl` (`Ring.h:57-82`): one 4096-byte page, `static_assert`s on size and alignment at
  `Ring.h:84-89` plus lock-free asserts on the atomics. Cmd cursor triple `Ring.h:59-61`, stage triple
  `Ring.h:64-66`, the five watermarks `Ring.h:69-73`, epoch / generation / two park flags / two event
  flags `Ring.h:76-81`.
* `InitRingControl` zeroes the page and sets `serverEpoch = ringGeneration = 1` so 0 always reads as
  "uninitialized" (`Ring.cpp:44-50`).
* `RingCursorsValid` checks `head >= applied >= retired` and occupancy <= capacity (`Ring.cpp:52-61`).
* `HardDrainRing` refuses unless `head == applied == retired`, else bumps `ringGeneration`
  (`Ring.cpp:63-79`).
* `RingProducer`: ctor validates power-of-two capacity in `[kMinRingCapacity, kMaxRingCapacity]` and
  a non-null base, otherwise latches `Valid() == false` (`Ring.cpp:85-105`). `FreeBytes` is measured
  against the **retired** tail (`Ring.cpp:107-120`). `Reserve` (`Ring.cpp:122-181`) refuses > half the
  ring (`:128-149`), returns `nullptr` + `MGLOG_D` when full (`:157-162`), emits the wrap pad
  automatically (`:164-171`). `Publish` is a single release store on the head (`Ring.cpp:183-190`).
* `RingConsumer::Pop` (`Ring.cpp:218-276`) acquire-loads the head, bounds-checks the header
  (8-alignment, >= header, <= available, no straddle), sets `*outCorrupt` and returns false on
  violation, silently skips `kRecPad` records. `PublishApplied` / `PublishRetired` /
  `PublishRetiredUpTo` at `Ring.cpp:278-304`; `PublishRetired` publishes applied first so retired can
  never overtake it, and `PublishRetiredUpTo` clamps to applied.
* **What is NOT there:** every sequence watermark (`appliedSeq`, `submittedSeq`, `retiredSeq`,
  `completedFrameSerial`, `presentAckSerial`) and both event flags (`eventRingFull`, `eventDropped`)
  are *declared, zeroed by `InitRingControl`, and written by nothing in the tree*. Grepping the whole
  repo for them finds only `Ring.h`, `Ring.cpp:48-49,77` and `RingTest.cpp:90-91` (the `DirectGLES.cpp`
  hits on `completedFrameSerial`/`ringGeneration` are Espryt's own unrelated locals,
  `DirectGLES.cpp:11881`, `Managers.h:1051`). So the whole credit/backpressure protocol of
  ARCHITECTURE.md §11.6 (`byte credit` + `present credit`, `appliedSeq` every 64 records) is
  **unwritten** — P5 owns it.
* Nothing constructs a ring over an `ShmSegment` either: the only `#include` of `Ring.h` outside
  `Ring.cpp` is `RingTest.cpp:14`, whose fixture puts the control page and the byte area on the stack
  / in a `std::vector` (`RingTest.cpp:29-76`).

### Doorbell.h / Doorbell.cpp — fully implemented

* `Doorbell` base (`Doorbell.h:86-184`): `Notify` / `Park` / `Reset` / `Dead`, plus the `Wait`
  template (`Doorbell.h:120-180`) that is the actual spin-then-park protocol: test, spin `spinUs`,
  then loop { set `parked` relaxed → `atomic_thread_fence(seq_cst)` → re-test → `Park(chunk)` → clear
  `parked` → re-test → bail on `Dead()` → bail on deadline }.
* `NotifyIfParked` (`Doorbell.h:194-199`): fence, then relaxed read of the flag, then `Notify`. The
  documented precondition is that the watermark is already published — the file header
  (`Doorbell.h:39-52`) spells out why a seq_cst store/load pair would *not* be enough and why neither
  fence may be removed.
* `CondVarDoorbell` (`Doorbell.cpp:35-100`): counted signals (so a ring that arrives before the park
  is not lost), `Kill()` death latch set under the same mutex `Park` tests it under
  (`Doorbell.cpp:87-95`), `notify_all` on kill.
* `SocketDoorbell` (`Doorbell.cpp:108-255`, POSIX only, `Doorbell.h:229-266`): one byte over an
  `AF_UNIX SOCK_STREAM` end, `revents` inspected properly so a hung-up peer latches `m_dead` instead
  of spinning (`Doorbell.cpp:190-217`), `Drain()` level→edge conversion (`:221-248`).
* Windows has no socket doorbell; `Doorbell.h:233-236` says the Windows path will be an overlapped
  named pipe and is explicitly not part of this skeleton.

### ShmSegment.h / .cpp / Posix / Win32

* POSIX is complete: `Create` uses `ASharedMemory_create` on Android, raw `SYS_memfd_create` on
  desktop Linux, `shm_open` + immediate `shm_unlink` as fallback (`ShmSegmentPosix.cpp:50-117`);
  `Adopt` takes ownership of an SCM_RIGHTS fd and **verifies `fstat` size >= the announced size**
  before trusting the peer (`ShmSegmentPosix.cpp:119-141`); `Map(readOnly)` / `Unmap` / `Close`
  (`:148-188`). `OpenNamed` returns `MOBILEGL_ERR_UNSUPPORTED` on POSIX (`:143-146`).
* Win32 (`ShmSegmentWin32.cpp`) is written but the file header says **UNTESTED** and explains why
  (`ShmSegmentWin32.cpp:12-16`). Both platform halves are listed unconditionally in the CMake source
  list so neither can rot (`CMakeLists.txt:509-513`).
* Tested only indirectly, by `FdPassingTest.ChildSharesASegmentThatTheParentMapsAndVerifies`.

### FdPassing.h / FdPassing.cpp — fully implemented (POSIX), stubbed on Windows

`Supported/CreateSocketPair/SendFd/ReceiveFd` return `MOBILEGL_ERR_UNSUPPORTED` on non-POSIX
(`FdPassing.cpp:37-45`); the POSIX arm (`:81-320`) is a real `sendmsg`/`recvmsg` SCM_RIGHTS
implementation over a dedicated `AF_UNIX SOCK_DGRAM` socketpair (rationale `FdPassing.h:19-26`:
datagram boundaries keep ancillary data attached to its payload). `kMaxSidebandBytes = 256`
(`FdPassing.h:38`); a short sideband buffer is refused **before** the datagram is consumed, so no
descriptor is ever dropped.

### Framing.h — header-only codec, implemented, **zero production callers**

`[u32 'MGLF'][u32 len][payload]`, 64 MiB cap (`Framing.h:46-48`), compaction at 64 KiB
(`Framing.h:53`). `AppendFrame` (`:56-77`), `FrameReader::Feed/HasMessage/TakeMessage/ParseHeader/
Consume` (`:85-199`). Two contracts it exists to fix are documented at `Framing.h:14-26`: a bad magic
or oversized length **latches** the reader failed and returns `MOBILEGL_ERR_PROTOCOL_MISMATCH`
forever; a too-small destination returns `MOBILEGL_ERR_BUFFER_TOO_SMALL` with the required size and
**keeps** the message.
The only non-test includer is `InProcessTransport.cpp:12`, and it uses the header solely for the
constant `kMaxFramePayloadSize` (`InProcessTransport.cpp:117`). **Nothing frames a byte today** — the
codec goes live with the socket transport in P6.

### ITransport.h — interface only

See §4.

### InProcessTransport.h / .cpp — implemented, but a *message* transport (see §3)

### WireLog.h / WireLog.cpp

A single `WireLogError(fmt, ...)` (`WireLog.h:31-36`, `WireLog.cpp:18-31`) at ERROR level, so
`Framing.h` can log without pulling `MG_Util/Debug/Log.h` (and through it the 661-header frontend
umbrella) into a header of this layer. This is enforced by the purity gate's `wire-header` probe
(`scripts/check_include_closure.py:136-144`, forbidden closure member `MobileGL/Includes.h`).
**P5 note:** any new header under `Transport/` reachable from `ITransport.h` inherits this rule.

### Build wiring

`MOBILEGL_BUILD_DISAGGREGATED` option at `CMakeLists.txt:23` (default OFF); missing flatbuffers
submodule shadows it OFF for that configure only (`CMakeLists.txt:453-465`); the eight `.cpp` files at
`CMakeLists.txt:503-519`; `-DMOBILEGL_BUILD_DISAGGREGATED=1` at `CMakeLists.txt:572-574`; the
flatbuffers include path appended at `CMakeLists.txt:605+`. The Wire suite is registered only under the
option (`MobileGL/MG_Test/CMakeLists.txt:105-106`), and `FdPassingTest` is dropped on Windows
(`MobileGL/MG_Test/Wire/CMakeLists.txt:14-17`). CI asserts the pull build has no `MG_Remote` symbols
(`.github/workflows/test.yml:1490-1499`) and that `protocol_generated.h` is regenerable
(`.github/workflows/test.yml:739,764`).

---

## 2. What the five Wire suites pin (52 tests)

**`RingTest.cpp`** (16 tests) — fixture at `:29-76` (stack control page + `std::vector` byte area).
* `:80` `ControlPageLayoutIsTheSharedContract` — 4096/4096/8, post-init generations = 1, and that each
  contended group is 64-byte aligned **and that `cmdHead` and `cmdAppliedTail` are on different cache
  lines** (`:103-113`).
* `:115` non-power-of-two capacity rejected; `:124` capacity the 32-bit `size` field cannot describe
  rejected; `:308` a ring too small for the smallest record rejected.
* `:145` records round-trip in order; `:171` payload padded to 8; `:181` a record is never split across
  the wrap (pad emitted).
* `:205` full ring refuses and recovers once the consumer retires; `:228`/`:234` records larger than
  the ring / larger than half the ring refused; `:255` **placeability does not depend on the head
  offset**; `:281` a half-capacity record fits at every head offset. (These four are the ones that
  make `nullptr` unambiguous: with `FreeBytes() >= total` it can only mean "too big, chunk".)
* `:322` `HardDrainRing` bumps the generation only when quiesced.
* `:341` a corrupt header is refused with `*outCorrupt` rather than dispatched.
* `:373` `SpscProducerConsumerThreadsAgreeOnEveryRecord` — 20 000 records, two real threads, consumer
  retires as it goes, ends with `cmdRetiredTail == cmdHead`.
* `:446` `DoorbellHandoffWakesBothSidesOnEveryPublish` — the **publish-then-`NotifyIfParked`** order in
  both directions, 2000 records, with a 5 s `Wait` deadline so a lost wakeup is a red test, not a hang.
  Its comment (`:439-445`) is honest that it cannot prove the fence pairing, only the call order.

**`FramingTest.cpp`** (9) — `:36` byte-at-a-time reassembly of two messages; `:65` the magic's literal
on-wire bytes; `:76` empty payload; `:90` bad magic latches the reader dead; `:111` oversized length
rejected **before any allocation**; `:123` send-side cap; `:132` buffer-too-small reports the size and
keeps the message; `:160` take with nothing complete neither blocks nor corrupts; `:182` the buffer
compacts instead of growing.

**`InProcessTransportTest.cpp`** (13) — `:52` both directions; `:71` order; `:85` buffer-too-small keeps
the message; `:103` poll (timeout 0) and finite timeout do not block forever; `:121` shutdown drains
before it closes (queued messages survive the peer's `Shutdown`); `:141` a blocked receiver wakes on
send **and** on shutdown; `:166` the frame cap; `:180` fd + sideband hand-over; `:222` **a frame wakeup
is not eaten by a waiter blocked on descriptors** (the reason there are two condvars,
`InProcessTransport.cpp:50-51`); `:279` doorbell wakes a parked waiter and `NotifyIfParked` only rings
when the flag is set; `:306` an already-true condition never parks; `:319` timeout; `:344`
**`ShutdownUnparksAWaiterWithNoDeadline`** — the design's steady state (spun, `consumerParked=1`,
`kWaitForever`), pinned with a 5 s bounded join so a regression fails red instead of wedging CI. This
is the test that makes `CondVarDoorbell::Kill()` load-bearing for P5's thread join.

**`FdPassingTest.cpp`** (9, POSIX) — `:56` a real `fork()`: child creates a 64 KiB segment, fills it,
`SendFd`s it with a sideband; parent first proves a short sideband buffer is refused *without*
consuming the datagram (`:93-100`), then adopts, maps read-only and compares every byte; `:137`
timeout; `:152` bad arguments; `:166` a segment with no sideband still carries its descriptor; `:197`
`SocketDoorbell` wakes a parked waiter; `:227` times out and remembers an early wakeup; `:260` stops
parking when the peer hangs up; `:289` still delivers the last ring before a hangup.

**`ProtocolSmokeTest.cpp`** (5) — `:43` `Hello` round-trips through the generated header *after*
`VerifyCtrlEnvelopeBuffer` + identifier check; `:72` `Welcome` carries four `SegmentRef`s with the
canonical sizes (8/32/8 MiB + 256 KiB); `:98` **union tags are frozen wire values** (Hello=1 …
LogLine=10, `SegmentKind::Cmd`=1, `Adopt`=6, `LogLevel::Error`=3, `FatalCode::ProtocolCorruption`=1);
`:119` a truncated message fails verification; `:128` the same bytes travel across
`InProcessTransport` unchanged.

Not pinned anywhere: the `SEG_STAGE` cursor set in a real two-role flow (the fixture can select
`RingCursorSet::Stage` but no test drives both rings together), any watermark/credit behaviour, any
`ShmSegment`-backed ring, `Framing` over a real byte pipe.

---

## 3. `InProcessTransport` today — the crux

**It short-circuits.** `InProcessChannel` (`InProcessTransport.cpp:38-97`) holds, per direction, a
`std::mutex`, two `std::condition_variable`s (one for messages, one for fd offers — rationale at
`:44-49`), a `std::deque<std::vector<std::uint8_t>>` of whole messages, a `std::deque<FdOffer>`, and a
`closed` flag; plus two `CondVarDoorbell`s (`:95-96`).

* `SendFrame` (`:111-135`): argument checks, the `kMaxFramePayloadSize` cap (kept only so a payload
  legal here stays legal after the switch to `spawn`, `:116-122`), then **one `std::vector` copy onto
  the peer's deque** and `cv.notify_one()`. No `AppendFrame`, no magic, no length prefix, no ring.
* `ReceiveFrame` (`:137-179`): predicate wait with `kWaitForever` / `wait_for`, the buffer-too-small
  contract (`:164-170`, message stays queued), then a `memcpy` out and `pop_front`.
* `PeekFrameSize` (`:181-185`), `ShareFd` = `::dup` + queued offer (`:187-227`), `ReceiveFd`
  (`:229-281`) mirrors `FdPassing::ReceiveFd`'s capacity rule exactly.
* `Shutdown` (`:287`) → `InProcessChannel::Close` (`:74-92`), which sets both directions closed,
  `notify_all`s both cvs, and **`Kill()`s both doorbells** (`:89-91`) — `Kill`, not `Notify`, for the
  reason spelled out in the comment and pinned by the test at `InProcessTransportTest.cpp:344`.
* `PeerDoorbell()` / `SelfDoorbell()` (`:289-291`) hand out the two bells. This is the *only* coupling
  to the ring design: the transport **owns the bells but not the ring**.

So the P5 exit gate "`InProcessTransport` runs the SAME G3 encode/decode as spawn"
(ROADMAP.md:21; ARCHITECTURE.md:391 "`InProcessTransport` 走与 spawn **完全相同**的 G3 编解码路径,
只在门铃/拷贝机制上不同") is **about the hot path, not about this class**. `ITransport` is explicitly
*not* on the hot path (`ITransport.h:16-20`); the G3 path is
`emitter → RingProducer::Reserve → memcpy of MGPWireRec_X → Publish → NotifyIfParked →
RingConsumer::Pop → bounds gate → applier`. None of that chain exists above `Ring.cpp`.

Two readings of where the missing piece goes, and the evidence that settles it:
* (a) a new ring-owning session object in `Client/` and `Server/` — the client end holds a
  `RingProducer` + the peer bell, the server end a `RingConsumer` + its own bell, over a `RingControl`
  that lives in an `ShmSegment` for spawn and can live in either an `ShmSegment` or plain heap for
  inproc;
* (b) `InProcessTransport` itself grows ring accessors and `SocketTransport` mirrors them in P6.

ARCHITECTURE.md:573-576 settles it for (a): the P5 file list adds nothing under `Transport/` (the only
`[P6]` addition there is `SocketTransport`), while `Client/{PipeEmitter,EmitTables,…}` and
`Server/{PipeApplier,ServerLoop,ReplyPool,EventRing,…}` are both `[P5+]`. Guess: the segment
allocation and the `Welcome`/`SegmentRef` exchange belong to the same P5 object, since `Welcome`
already has exactly the four `SegmentRef` fields (`protocol.fbs:68-76`) and `ProtocolSmokeTest.cpp:72`
already pins their sizes.

A practical consequence worth putting in the brief: in `inproc` both roles are the same process, so
`ShmSegment::Create` + one `Map` is enough (no `ShareFd`) — but using `ShmSegment` anyway (rather than
`new`) is what keeps the spawn path from being a different code path, which is what the gate is asking
for.

---

## 4. What a second `ITransport` (P6 socket/spawn) must provide

Seven pure virtuals plus the role (`ITransport.h:46-127`):

| member | contract that binds an implementation |
|---|---|
| `SendFrame(MobileGLByteSpan)` (`:59`) | `bytes` is **borrowed**: copy it or finish the write before returning. `> kMaxFramePayloadSize` ⇒ `MOBILEGL_ERR_INVALID_ARGUMENT`. Caller serialises sends (`:27-29`). |
| `ReceiveFrame(span, outSize, timeoutMs)` (`:80-81`) | five codes: `OK` (consumed) / `BUFFER_TOO_SMALL` (**message stays queued**, `*outSize` = required) / `TIMEOUT` (`timeoutMs == 0` is a poll, `kWaitForever` never times out) / `TRANSPORT_CLOSED` (peer gone **and** nothing buffered) / `PROTOCOL_MISMATCH` (latched dead, never recovers). |
| `PeekFrameSize()` (`:85`) | size of the next pending message or 0. |
| `ShareFd(fd, sideband)` (`:98`) | POSIX SCM_RIGHTS; **Windows returns `MOBILEGL_ERR_UNSUPPORTED`**, the section name travels in `SegmentRef` instead. Caller keeps ownership of `fd`. |
| `ReceiveFd(outFd, sideband, outSidebandSize, timeoutMs)` (`:107-108`) | `*outFd` is owned by the receiver; `sideband` must be >= `FdPassing::kMaxSidebandBytes`, checked **before** anything is read. |
| `Shutdown()` (`:122`) | idempotent and **symmetric**: tears down the whole connection, both ends stop sending, every waiter on either side unblocks, but **already-queued messages stay readable**. |
| `Role()` (`:124`) | `Server` / `Client` / `InProcess` (`ITransport.h:40-44`). |

Ready-made parts for P6: `Framing.h` (the reassembler, still callerless), `FdPassing.cpp` (the aux
socketpair), `SocketDoorbell` (`Doorbell.cpp:108-255`). Not ready: nothing turns a socket fd into an
`ITransport`, and there is no `ServerMain` (ROADMAP.md:22 assigns both to P6).

**Design flag for the brief:** the two doorbell accessors are **not** on `ITransport` — they are
non-virtual extras on the concrete `InProcessTransport` (`InProcessTransport.h:64-68`). If P5's
client/server code reaches for them through the concrete type, P6 has to either hoist them into
`ITransport` or re-plumb P5's call sites. Deciding that in P5 costs nothing; discovering it in P6
costs a package.

---

## 5. Threading and lifetime

* **There is no thread creation anywhere in `MG_Remote` today.** `grep -rn 'std::thread|pthread_create|
  std::jthread' MobileGL/MG_Remote/` is empty; every thread in the Wire suites is the test's own.
  So P5 creates the first one.
* ARCHITECTURE.md:535 is the design of record:
  * **client** — *no new thread in v1*: the encode happens on the GL thread (the frontend is already a
    per-context single-thread contract). Foreign-thread sync/query reads are answered lock-free from
    `RingControl`; the few that must emit take a `ctrlMutex` and go out as an `AuxRequest` on the CTRL
    channel, because the SPSC ring admits no second producer. `ShaderCompilePool` stays client-side.
    An optional `mgl-client-tx` is measurement-gated, not planned.
  * **server** — `mgl-srv-io` (asio, framing, `SCM_RIGHTS`, doorbell, CTRL RPC) and `mgl-srv-apply`,
    which **holds the native context for its whole life** (`g_backendContextOwnerThread` written once,
    so the `MakeCurrent` invalidation storm becomes a one-off at startup and the off-thread downgrade
    disappears). Optional `mgl-srv-dec`.
  * Placement: ARCHITECTURE.md:536 — reuse `ShaderCompilePool`'s big-core detection to pin
    `mgl-srv-apply` (`MOBILEGL_IPC_SERVER_AFFINITY`, default auto, resolved mask logged), and report
    per-thread CPU time each phase. ARCHITECTURE.md:308 is the related rule that a texture pull blocks
    `mgl-srv-apply`, never the application thread.
* In `inproc` the apply side is a second thread **inside the game process**; ARCHITECTURE.md:26 frames
  that as the point ("同时就是 monolith 的渲染线程 … 手上最大的单一 CPU 杠杆").
* **Lifetime mechanism already in place:** `CondVarDoorbell::Kill()` (`Doorbell.h:211-221`,
  `Doorbell.cpp:87-95`) exists precisely so `Shutdown` can join an apply thread parked with
  `kWaitForever`; `InProcessChannel::Close` calls it (`InProcessTransport.cpp:89-91`) and
  `InProcessTransportTest.cpp:344` pins it. P5's `ServerLoop` should park through
  `Doorbell::Wait(control.consumerParked, …, kWaitForever)` and treat `Wait == false && Dead()` as
  "shut down", which is exactly the shape the test asserts.
* Role isolation: ARCHITECTURE.md:581 says `MOBILEGL_BUILD_DISAGGREGATED_INPROC` (the role-isolation
  shim for the two remaining process globals, the pipe tables and `pActiveBackendObject`) **does not
  exist yet** and is listed as "计划(P5)" in the switch table at ARCHITECTURE.md:~600. `InProcessTransport.h:19-23`
  repeats that nothing in the current code depends on it. Whether P5 needs it on day one depends on
  whether the inproc server role ever reads `pGLContext` — worth an explicit ruling in the brief.

---

## 6. The FlatBuffers control plane

`MobileGL/MG_Remote/Protocol/protocol.fbs` (235 lines) + `generated/protocol_generated.h` (58 866
bytes, committed) + `mg_protocol_base.h` (120 lines, pure C).

| message | lines | purpose |
|---|---|---|
| `SegmentKind` | `:36-44` | `Cmd=1 Stage=2 Reply=3 Event=4 Shadow=5 Adopt=6` |
| `SegmentRef` | `:48-53` | `{id, kind, sizeBytes, name}`; the fd never travels in the message (SCM_RIGHTS on POSIX, `name` on Windows) |
| `Hello` | `:59-66` | `{abiMajor, abiMinor, buildFingerprint, backendType, pid, configBlob}` |
| `Welcome` | `:68-76` | `{abi, serverPid}` + the four `SegmentRef`s |
| `CapsSnapshot` | `:86-96` | the P5 `MGPCaps` snapshot's transport form: three byte blobs (`dynamicParameters`, `rendererInfo`, `formatCaps`) + `extensions`, `apiVersion`, the two 3-entry compute limits, `tableSlotMask`, `prefersCpuXfbPrimitiveAccounting`. Comment says it replaces the 40 `pActiveBackendObject->` sites and 89 caps reads (cf. ARCHITECTURE.md:116) |
| `DefaultFramebufferInfo` | `:98-104` | width/height/color/depth/stencil |
| `SurfaceOpKind` / `WindowKind` / `SurfaceOp` / `SurfaceReply` | `:110-148` | the EGL/surface RPC: init display, create window/pbuffer, resize, release, make/release current; reply carries EGL version + `DefaultFramebufferInfo` |
| `ResyncRequest` / `ResyncDone` | `:157-161` | sent after the client sees `serverEpoch` move |
| `AuxRequestKind` / `AuxRequest` | `:163-176` | `FenceClientWait`, `QueryResult`, `ScalarGet` — the foreign-thread path that may not take the SPSC ring |
| `FatalCode` / `Fatal` | `:178-191` | `ProtocolCorruption RingOverrun SegmentMismatch DeviceLost ServerCrashed AbiMismatch` |
| `LogLevel` / `LogLine` | `:195-206` | `<= Warn` lossy, `>= Error` lossless + rate limited |
| `CtrlMsg` union / `CtrlEnvelope` | `:217-232` | root type, `file_identifier "MGLC"` (`:234-235`); union tags are wire values, **append only** |

* **Nothing uses any of it.** The only `#include` of `protocol_generated.h` in the tree is
  `ProtocolSmokeTest.cpp:13`. Regeneration is `scripts/gen_protocol.py`, never in the default build
  graph; CI's `flatc-check` regenerates and diffs (`.github/workflows/test.yml:739,764`).
* `mg_protocol_base.h` gives P5 its result codes (`:97-114`, incl. `MOBILEGL_ERR_BUFFER_TOO_SMALL=11`
  and `MOBILEGL_ERR_SHM_EXHAUSTED=8`), the two spans, `MobileGLShmRegion` (`:86-91`), the id typedefs,
  ABI 1.0 macros (`:51-56`) and the structSize-first versioning rules (`:27-34`).

Two gaps P5 should decide about explicitly:
1. **`FatalCode` has no `UnmigratedPipeInput`.** P5's exit gate "a read of an unmigrated field =
   `Fatal{UnmigratedPipeInput}`" is already served by a *local* fatal, not a wire message:
   `MGPipeInputPoisonFatalForVerb` = `MGLOG_F` + `std::abort()` (`MG_Backend/MGPipe/PipeInputs.h:18-47`),
   and — importantly — `MOBILEGL_PIPE_POISON` is **auto-armed by `MOBILEGL_BUILD_DISAGGREGATED`**
   (`PipeInputs.h:22-25`), so every P5 build has it on with no extra knob. The harness already greps
   for it (`tools/trace_replay/run_trace_case.cmake:174,183`). So the gate is about *exercising* the
   poison under split, not about implementing it. If the server role must instead *report* it to the
   client, that is a new `FatalCode` tag — an append, per `protocol.fbs:212-216`.
2. **There is no credit/ack message, by design**: ARCHITECTURE.md §11.6 puts present-credit and
   byte-credit entirely in `RingControl` (`presentAckSerial`, `appliedSeq` every 64 records, reverse
   doorbell), which is the unwritten half flagged in §1.

---

## 7. Shortest list of what P5 must add in this subject area

1. A ring-owning session/channel object pair (Client + Server), constructing `RingControl` +
   `RingProducer`/`RingConsumer` over `ShmSegment` for both `inproc` and `spawn`, with the doorbells
   `InProcessTransport` already hands out — this is what makes the gate's "same G3 codec" true.
2. The G3 **encoder** (`MGPWireRec_*` writers; nothing writes one today) and the real body of
   `MGPipeApplyWireRecord`, whose skeleton comment (`PipeWire.inc:703-707`) names P5 and
   `MG_Remote/Server/PipeApplier.cpp` as the owner. The bounds gate and the `Fatal` on a bad opcode
   (`PipeWire.inc:689-700, 924-927`) are already there and already fatal.
3. The watermark/credit protocol: somebody must actually write `appliedSeq`, `submittedSeq`,
   `retiredSeq`, `completedFrameSerial`, `presentAckSerial`, `eventRingFull`, `eventDropped`.
4. Handshake use of the control plane (`Hello`/`Welcome`/`CapsSnapshot`/`SurfaceOp`) — today zero
   callers.
5. `MOBILEGL_TRANSPORT` parsing in `ConfigLoader.cpp` (ARCHITECTURE.md:583; the string does not appear
   in any `.cpp` in the tree — only in docs and `InProcessTransport.h:19`), plus the `MOBILEGL_IPC_*`
   env family listed at ARCHITECTURE.md:615, of which only `MOBILEGL_IPC_SPIN_US`'s default is coded
   (`Doorbell.h:67`, `kDefaultSpinUs = 50`) and **nothing reads the env var**.
6. Test wiring: `add_trace_replay_test` needs the `SPLIT` suffix and `-DTRACE_TRANSPORT=`, and the
   ctest `ENVIRONMENT` trap is already documented (ARCHITECTURE.md:584 — property replaces rather than
   appends, `;` must be escaped, use `mgl_itest_join_environment`).

## 8. Cautions

* Do not add an include of `MG_Util/Debug/Log.h` (or anything reaching `MobileGL/Includes.h`) to a new
  header under `Transport/` reachable from `ITransport.h` — the purity gate fails
  (`scripts/check_include_closure.py:136-144`); use `WireLog.h`.
* Keep `MG_Remote` symbols out of the pull build or `.github/workflows/test.yml:1492-1499` goes red.
* `CtrlMsg`/`SegmentKind`/`FatalCode`/`LogLevel` numbering is pinned by
  `ProtocolSmokeTest.cpp:98-117`: append only, and if that test needs editing the change was a wire
  break.
* `RingProducer::Reserve` is non-blocking by contract; a P5 emitter that waits must wait on
  `producerParked` + the reverse doorbell, and must never interpret `nullptr` with
  `FreeBytes() >= total` as "wait" (`Ring.h:176-190`).
