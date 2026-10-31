# Transport and data plane (formerly ARCHITECTURE §11)

> Design details. The design points and the index of all chapters are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; an `ARCHITECTURE.md §N` in a code comment is found by its number here.

## 11. Transport and data plane (`MobileGL/MG_Remote/`)

### 11.1 Segments

| Segment | Owner | Default | Contents |
|---|---|---|---|
| `SEG_CMD` | client (server read-only) | 8 MiB | `RingControl` page + POD records + small inline payloads |
| `SEG_STAGE` | client | 32 MiB | bulk bytes: buffer / texture subdata, UBOs, client arrays, persistent-map dirty blocks |
| `SEG_REPLY` | server | 16 MiB, 8 × 2 MiB slots | readback pixels, acceptance answers (ID-47) |
| `SEG_EVENT` | server | 256 KiB SPSC ring | callback events |
| `SEG_SHADOW` / `SEG_ADOPT` | client / server | — | zero-copy shadow (Phase 2) / ≥16 MiB adoption (P11); refused by name on the stream data plane (`MG_Remote/Transport/AdoptTier.cpp`, during the handshake, P11 A1) |

On the same machine the segments are created by `ShmSegment` (Android `ASharedMemory`, Linux `memfd`, Windows `CreateFileMappingW`), and descriptors are passed with `SCM_RIGHTS`; on the stream data plane each end holds a private segment of the same size (§11.9).

### 11.2 `RingControl` (`Ring.h`)

One 4 KiB page, one cache line per contention group, grouped by writer (P6 lk): the producer line holds `cmdHead` / `submittedSeq`; the consumer line holds two tails; the four watermarks written by the consumer are collected into `Progress` (`appliedSeq` / `retiredSeq` / `completedFrameSerial` / `presentAckSerial`); finally the epoch, generation, park flags and `eventRingFull` / `eventDropped`. Cursors are monotonic byte counts with a power-of-two mask and are never reset; pad records handle the wrap; an impossible head → `Fatal{ProtocolCorruption}`.

#### 11.2.1 The five watermarks

| Watermark | Advanced by | Meaning |
|---|---|---|
| `submittedSeq` | producer, after publishing | a diagnostic (how far the client is ahead of the server), nobody waits on it; it sits on the producer's cache line, so `LinkProgress` stays single-writer |
| `appliedSeq` | consumer, +1 per applied record | verb barriers and every reply wait read it, so it must not batch: batching leaves a waiter stuck on work that has already completed, or wakes it on work that has not |
| `retiredSeq` | consumer, once the `SEG_STAGE` bytes a record references are done with | the staging allocator reclaims only after it; never beyond `appliedSeq` |
| `completedFrameSerial` | server, when present completes | reclamation and aging wait on it; it trails `appliedSeq` by one GPU depth and must not be mixed with it |
| `presentAckSerial` | server, when the present credit is returned | present throttling waits on it; the only backpressure that limits latency rather than bytes |

The rule (`Watermark` in `SessionRings.h` is the only implementation): every wait uses `>=`, since either side may advance several steps at once and `==` would miss the wakeup. A watermark may only be published late, never early: late just means waiting longer, while early makes a waiter silently use work that has not happened, and no check on the ring can detect it; all but `appliedSeq` may be published lazily, and going backward is `Fatal{ProtocolCorruption}`. The wrap padding (`kRecPad`) does not advance seq: seq is the reply slot id, and if only one side counted the padding the two sides' sequence numbers would be off by one on every wrap and silently read another call's answer, so both sides skip it before counting (`RingConsumer::Pop`).

### 11.3 Bidirectional doorbell (`Doorbell.h`)

Both directions do "spin first, then set parked, then block", and the publisher rings only when the peer is parked; lost wakeups are closed off by two `seq_cst` fences. `CondVarDoorbell` (same process, with `Kill()`) and `SocketDoorbell` (across processes, where the peer closing is the death detection). On the stream data plane the doorbell is a local condition variable, rung by the read thread.

#### 11.3.1 Why lost wakeups rest on two fences

Waiter: set the park flag → `std::atomic_thread_fence(seq_cst)` → re-test the condition (`Doorbell::Wait`); notifier: publish the watermark → the same fence → read the park flag (`NotifyIfParked`). Both fences are in the seq_cst total order and must have an order, so by [atomics.order] at least one side sees the other side's writes, and the flag's own reads and writes can be relaxed.

Making only the flag's store / load seq_cst is not enough: the Dekker argument requires all four accesses to be in the total order, while the watermark publication is a release store (`RingProducer::Publish`) and the condition re-test is an acquire load. On x86 a release store is a plain MOV, and while it is still in the store buffer a plain MOV read of the flag may read 0, so the notifier does not ring the doorbell and the waiter parks forever on the old watermark; on ARMv8 STLR→LDAR is RCsc and merely happens to be fine. So neither fence can be removed, and `NotifyIfParked` must be called after the watermark publication: a fence only orders the accesses before it.

### 11.4 Control plane (`protocol.fbs`, `Framing.h`, `ITransport.h`)

- One FlatBuffers schema: the hot path uses `struct`s that go straight into the ring (aligned one by one with the POD records by `static_assert`); the rare / variable-length / evolving cases go through `CtrlMsg` on the control connection (`Hello`, `Welcome`, `Refuse`, `CapsSnapshot`, `SurfaceOp/SurfaceReply`, `SessionFault`, `LogLine` …), union tags are append-only, and the control revision is pinned to `sha256(protocol.fbs)`.
- The generated output is committed to the tree, and the CI `flatc-check` regenerates it and diffs; codegen is not in the default build graph.
- Framing is `[u32 'MGLF'][u32 len][payload]`, with a 64 MiB cap, validated on read, and a bad frame latches the failure immediately.

### 11.5 WAR hazards, the copy accounting and backpressure

Phase 1 (today): the bytes are copied into a stage slot at the moment of the GL call, the slot is immutable until apply passes it, hazards are eliminated by construction, and the cost is one memcpy; Phase 2 (`SEG_SHADOW` zero copy) is not done. The server has no second `BufferObject`, so there is no intermediate copy. Allocation failure escalates: grow → bounded wait on the oldest unretired batch → hard drain + generation bump (after which the tracker re-pushes everything).

### 11.6 Publish, sequence numbers and credit

- Every record release-stores `cmdHead`, and the doorbell is rung only when the consumer is parked; explicit doorbell points: `present`, `kNeedsAck`, `eglMakeCurrent`, `glFlush`, insufficient stage headroom, and polling-style entry points.
- seq = the record ordinal; byte credit and present credit are two independent windows.
- **The wait rule is decided by the `WaitClass` column (P5e)**: `kWaitReply` / `kWaitPresent` / `kWaitApplied` / `kWaitNone`; with run-ahead armed, `kWaitNone` returns as soon as it publishes. The barriered predicate (`MGPipeBarriered`) is computed with the same function and the same data on the client and on the server.
- Forced wait points: `MapBuffer(READ)` / the `GetBufferSubData` source goes through `SyncGpuWrites`; `glFinish` = wait until `appliedSeq` catches up, then drain the reverse channel; wait once before every `Server*` EGL forwarder. `glGetError` may lag by at most one present credit.

#### 11.6.1 The terms of the barriered predicate

`MGPipeBarriered(op, payload, applierState)` (`MG_Pipe/PipeApply.h`) is computed by the server's `ApplyOne` and decides whether this record may read client memory; the client does not call it, but evaluates the same terms before publishing (the wait rule in `ClientSession` reads the `WaitClass` column, and `ClientVerbIsBarriered` in `PipeFill.cpp` separately decides XFB). If the two sides' answers ever differ, the failure is silent (the client keeps running and the server reads a value that has already changed), so the terms read only data visible on the wire: the static `WaitClass` column, the record's own payload, and the state that earlier records on the ring wrote into the applier. There are only three terms:

1. `WaitClass != kWaitNone`;
2. a `kCtxVerb` inside an open XFB span (XFB stays lockstep);
3. a `draw_vbo` with `kDrawClientArrays`: under run-ahead the client refuses it before publishing, so this term exists only for the lockstep arm, to make the predicate complete.

A new term must be added to this function and to the corresponding client-side decision in the same change, never on one side only. Judging a candidate term means asking **which arm pays for it**, not which lane it turns green: once, to make the Espryt indirect multi-draw tier's read of `GL_DRAW_INDIRECT_BUFFER` bindings legitimate, every ordinary multi-draw was upgraded to barriered, so every `glMultiDraw*` in the default tier gained one more rendezvous, while that read happens only in two optional tiers; in the end the side that does the binding answers by handle instead (`BoundDrawIndirectBufferId` in `MultiDraw.cpp`), and the term was withdrawn.

### 11.7 Event return and overflow

The client drains `SEG_EVENT` in `glGetError`, query / sync queries, `eglSwapBuffers`, buffer readback, and each wait-loop round. Logs are lossy; semantic events are lossless: when the ring is full the server sets `eventRingFull`, stops applying at a record boundary, and rings the client; after draining, the client clears the flag and rings back (both halves are present, so there is no deadlock). Under run-ahead the producer blocks waiting for the drain, with a whole budget of `MOBILEGL_IPC_EVENT_WAIT_MS`; if the client does not drain / has gone away / the session stops → `ReverseChannelForfeit{…}` forfeits delivery and latches (Ph PH-6), instead of a `Fatal` taking down the server. A server that does not publish `kCapRunAheadApply` (the lockstep arm) keeps the P5c Fatal.

### 11.8 Fences and present-free workloads (P10)

Fence completion must come from real per-fence retirement, not from the present watermark (`g_completedFrameSerial` in DirectGLES today only advances in `Present`); a present-free loop needs the server to insert a non-present fence tick. The Magma half has landed with run-ahead.

### 11.9 The two axes of the transport stack (P6.5)

The end goal (2026-09-22): the client and the server may connect over TCP from different machines and different OSes / architectures, and same-machine `spawn` is kept. The transport stack is split into **two independently selectable, handshake-negotiated, mixable** axes, and the layers above see only two interfaces:

| Axis | Interface | Implementations | Options (`MOBILEGL_IPC_CONTROL` / `MOBILEGL_IPC_DATA`) |
|---|---|---|---|
| control plane | `ITransport` (carries only control frames, never descriptors) | `SocketTransport` | `fork` (inherited fd), `unix:<path>`, `tcp://host:port` |
| data plane | `ILink` (record reservation / commit, watermarks, reply, event, span resolution) | `ShmLink` (shared segment + `RingControl` + doorbell), `StreamLink` (chunked framing, watermarks and replies become messages, send window) | `shm`, `stream`, `auto` |

- The pairing is chosen once at session setup; `LinkTerms` (data plane, window sizes, `maxReplyBytes` …) is **stated by the server**, and the client's request may be clamped.
- Shared-segment delivery is decoupled from the control plane: `ShmLink` carries its own AF_UNIX aux rendezvous name, announced in `Welcome`, so "TCP control + shared-segment data" on the same machine is a valid pairing. The `auto` criterion is **that the segment actually delivered successfully**, and a fallback to `stream` must be named in the log.
- **Zero link-kind branches above**: `ShmLink` / `StreamLink` / `RingControl` and the like appear only in `Transport/` and its tests (a purity grep gate); role predicates answer "who is in which process", not "what link is used".
- The two ends are different binaries: `wireFingerprint` (the per-member layout digest derived from `PipeFields.def` + the catalog digest + byte order / pointer width) is always compared; `buildFingerprint` is compared only under `Dial == Fork` (or `MOBILEGL_IPC_REQUIRE_SAME_BUILD=1`); a mismatch is a `Refuse`, not an abort.
- Over TCP every `kWaitReply` is one RTT, and the `SEG_STAGE` bytes face a 30–100 MB/s link: these two columns turn from "record items" into feasibility numbers (open questions 19, 20).

The first wave (all-TCP control + stream data) has landed; same-machine mixing (nd `auto` / sd) is the second wave. Details and contracts are in the git history.
