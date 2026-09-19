# s1 — adversarial review, v1

Reviewed: branch `p5/s1`, `5175f9cc..4cf59428` (4 commits, 16 files, +3405/−82), worktree `~/w7/p5-s1`.
Report under review: `~/w7/notes/p5/p5-results/s1-v1.md`.
Everything below was read at `4cf59428` with a clean worktree (`git status --porcelain` empty).

Independent re-runs: `~/w7/p5-s1rev-gaterun.log`, `~/w7/p5-s1rev-flake.log`, `~/w7/p5-s1rev-symbols.md`,
probe source + binary `~/w7/p5-s1rev-probe/`.

**Counts: 1 BLOCKER, 8 MAJOR, 7 MINOR, 3 QUESTION.**

---

## BLOCKER

### B-1. A wrong consumer mask is silent on both sides, and the default is derived from a process-global whose meaning differs between inproc and spawn

`ServerSession.cpp:120-125`:

```
Uint64 DeriveConsumedSubsystems() {
    if (MG_Pipe::MGPipeGetResourceOps() == nullptr) {
        return MG_Pipe::kMGPipeSubsystemsMigratedAtP2;      // 0x7f, bits 0..6
    }
    return MG_Pipe::kMGPipeSubsystemsMigratedAtP4a;          // 0x1fff, bits 0..12
}
```

`MGPipeGetResourceOps()` returns `g_resourceOps`, a **process-wide** global (`MG_Pipe/PipeApply.cpp:1119`).
Under `inproc` both roles are one process, so at `Accept` time this reads the **monolith/client's own
registration** and answers `0x1fff` regardless of what the server role's applier actually consumes. That is
precisely the trap `ServerSession.h:117-118` warns the *client* about — "under inproc a client reads it
correctly by accident and under spawn reads as null, silently disabling five whole record families" —
committed here by the server, one line below the warning.

Then `ServerSession::CallMask()` (`:179-183`) falls back to that derivation whenever `m_consumedSet` is
false, and `PublishCapsSnapshot` (`:354`) puts it on the wire **with no diagnostic at all**. Compare the
adjacent case: `Accept` logs `MGLOG_W` when there is no backend (`:321-323`). There is no equivalent line
for "this CallMask was guessed". `SetConsumedSubsystems` has no caller anywhere in the tree
(`grep -rn SetConsumedSubsystems` → declaration + definition only).

Failure scenario, end to end: P6 spawns the server; the server process has not yet registered its resource
ops when `Accept` runs; the default collapses to `0x7f`; `CapsMirror::ServerConsumes` returns false for
bits 7..12; the client's R-8 liveness gates stop emitting five P4a families; **the client still clears its
dirty flags on acceptance**, so the uploads are lost with nothing in any log saying so, and the split lane
is green. That is ID-39's 66 lost uploads reproduced from the other side — the exact class the brief names.

s1 flagged this as "a default, not a ruling — v1 should confirm or replace it" (report §2.6). Flagging it
in a markdown file is not a mechanism. Nothing in the code makes v1's confirmation mandatory, and nothing
makes the wrong answer loud.

What the fix has to be: `CallMask()` must not silently substitute a guess. Either (a) `PublishCapsSnapshot`
refuses to publish while `m_consumedSet` is false (`MOBILEGL_ERR_NOT_INITIALIZED`, the shape it already uses
for a missing backend), or (b) it emits one `MGLOG_E` per publish naming the derived value and the fact that
it was derived, and the derivation stops reading `MGPipeGetResourceOps()` — a global that answers a
different question in each delivery mode — in favour of something the server role owns.

*Judgement call on the severity*: this is a default that v1 is expected to replace. I rate it BLOCKER because
the failure mode is a green lane with lost work and there is no detector anywhere; it would be downgraded to
MAJOR by any mechanism that makes an unconfirmed mask loud or fatal.

---

## MAJOR

### M-1. The schema deletions silently re-used two frozen vtable slots with incompatible types, and the ABI fingerprint cannot see it

This is the question the brief predicted. **The deletions did renumber something — the new fields.**

`git show 5175f9cc:…/protocol_generated.h`, `CapsSnapshot`:

```
VT_APIVERSION = 12,
VT_MAXCOMPUTEWORKGROUPCOUNT = 14,   // [int]  -> 4-byte uoffset
VT_MAXCOMPUTEWORKGROUPSIZE  = 16,   // [int]  -> 4-byte uoffset
VT_TABLESLOTMASK = 18,
VT_PREFERSCPUXFBPRIMITIVEACCOUNTING = 20
```

at `4cf59428` (`protocol_generated.h:820-826`):

```
VT_APIVERSION = 12,
VT_CALLMASK   = 14,   // ulong -> 8-byte inline
VT_BACKENDTYPE = 16   // uint  -> 4-byte inline
```

Slots 14 and 16 now carry a different **type** than they did at the base commit. s1's report says "Four
appends, all safe: the deleted four were the LAST four fields, so no surviving field moved a vtable slot,
and every addition is at the end." The first half is true; the second is false in flatbuffers' sense —
a field id is a vtable slot, and a *deletion* frees the slot for the next appended field. The
flatbuffers-correct spelling of the contract's "DELETED, not renamed" is
`maxComputeWorkGroupCount: [int] (deprecated);` (and the other three), which reserves 14/18/20 and pushes
`callMask` to 22.

Three aggravating facts:

1. **`MOBILEGL_PROTOCOL_ABI_MAJOR` was not bumped** — still 1 at `mg_protocol_base.h:52`, while that same
   header's own rule at `:31` reads "Changing/removing/reordering existing fields is a MAJOR bump." Two
   builds straddling this commit both announce major=1 and the handshake accepts them.
2. **The ABI fingerprint is blind to it.** `CapsAbiFingerprint()` (`CapsCodec.cpp:41-48`) mixes three
   `sizeof`s, `MOBILEGL_ABI_VERSION(...)` and `GIT_COMMIT_HASH_SHORT`. A schema-only edit changes none of
   the first four. The fifth, the git stamp, is captured at CMake **configure** time — s1's own report §5
   flags it as weak, and it is the same trap as `android-build-bisect-traps`. So two incrementally-built
   peers straddling this commit produce equal fingerprints and disagree about slot 14.
3. **s1 cites the wrong test as evidence.** The report says "`ProtocolSmokeTest.cpp` was not edited,
   including its `UnionTagsAreFrozenWireValues` case — the schema change was not a wire break."
   `UnionTagsAreFrozenWireValues` (`ProtocolSmokeTest.cpp:98-117`) pins union tags and enum values only.
   Nothing in that file, or anywhere else in the tree, pins a table field id. Its green says nothing about
   this change.

Failure scenario: an older peer writes `maxComputeWorkGroupCount` (a 4-byte uoffset at slot 14); a newer
peer reads slot 14 as an 8-byte `ulong` `callMask`. The flatbuffers verifier's 8-byte alignment check will
usually reject it, so the realistic outcome is a `CapsSnapshot` that fails verification for a reason nobody
can diagnose — but the direction is not guaranteed, and a slot-16 `uint` read over a 4-byte uoffset
verifies cleanly and yields a nonsense `BackendType`, which `CapsMirror::Backend()` then feeds to the three
frontend branches `protocol.fbs:72-74` names.

Fix: mark the four deleted fields `(deprecated)` and regenerate, **or** bump
`MOBILEGL_PROTOCOL_ABI_MAJOR` and make the handshake reject across it. The first is one keyword per field.

### M-2. `MOBILEGL_IPC_RING_MB` and `MOBILEGL_IPC_STAGE_MB` are parsed, logged, and never applied

`ServerSession.cpp:192-194`:

```
if (m_sizes.CmdBytes == 0) {
    m_sizes = SizesFromConfig();
}
```

`m_sizes` is a default-constructed `Transport::SessionSegmentSizes` member (`ServerSession.h:160`), and
`SessionSegmentSizes::CmdBytes` has a default member initializer of 8 MiB (`SessionRings.h:71`). So
`m_sizes.CmdBytes == 0` is **never** true at `Accept`, `SizesFromConfig()` (`:86-95`) is dead, and
`MG_Config::Ipc.RingMb` / `StageMb` have no reader:

```
$ grep -rn "Ipc\.RingMb|Ipc\.StageMb" --include=*.cpp --include=*.h MobileGL/ | grep -v Config.h:
MobileGL/MG_Remote/Server/ServerSession.cpp:89:   const Uint64 ringMb  = MG_Config::Ipc.RingMb  == 0 ? 8u  : MG_Config::Ipc.RingMb;
MobileGL/MG_Remote/Server/ServerSession.cpp:90:   const Uint64 stageMb = MG_Config::Ipc.StageMb == 0 ? 32u : MG_Config::Ipc.StageMb;
```

— both inside the dead function. `ConfigLoader.cpp:363-364` parses them and `:382` echoes them into the
config line, so the log says `RingMb=32` while the session maps 8 MiB.

`Config.h:468-469`, four lines above the declaration, is the project's own statement of this exact bug:
"an environment variable that nothing parses is indistinguishable from one that is parsed and ignored."

Failure scenario: R-10's `Fatal{RingOverrun}` fires in a bring-up; the operator sets
`MOBILEGL_IPC_RING_MB=32` to get past it; the log confirms 32; nothing changes; the Fatal repeats. Also t1's
memory numbers are unmovable by the knob that exists to move them.

Fix: call `SizesFromConfig()` unconditionally and let `SetSegmentSizes` override it, or sentinel `m_sizes`
with 0 defaults and resolve in one place. s1's report §5 claims "s1 reads `RingMb`, `StageMb`, `SpinUs` and
`VerbBarrier`" — only the last two are true.

### M-3. A verifiable control frame with a null union payload null-derefs in both handshakes

`ServerSession.cpp:205-212`:

```
const CtrlEnvelope* envelope = ParseEnvelope(frame);
if (envelope == nullptr || envelope->msg_type() != CtrlMsg::Hello) { … return …; }
const Hello* hello = envelope->msg_as_Hello();
const char* theirStamp = hello->buildFingerprint() == nullptr ? nullptr : …;   // hello may be null
```

and identically `ClientSession.cpp:174-184` for `Welcome`. `ParseEnvelope` runs
`VerifyCtrlEnvelopeBuffer` — and flatbuffers' `Verifier::VerifyTable` is
`return !table || table->Verify(*this);` (`3rdparty/flatbuffers/include/flatbuffers/verifier.h:119-121`),
so a **null** union member passes verification. `msg_as_Hello()` then returns nullptr while
`msg_type() == Hello`.

Proven, not argued — `~/w7/p5-s1rev-probe/nullunion.cpp`:

```
$ clang++ -std=c++17 -I. -I3rdparty/flatbuffers/include nullunion.cpp -o nullunion && ./nullunion
bytes=24 verified=1 hasIdentifier=1 msg_type_is_Hello=1 msg_as_Hello=(nil)
REACHES ServerSession.cpp:212 hello->buildFingerprint() WITH hello == nullptr
```

A 24-byte frame. The pre-existing `ProtocolSmokeTest.cpp:56` does `ASSERT_NE(hello, nullptr)` — the test
code already knows; the production code does not, while `ServerSession.cpp:55-56` claims "Every message
from the peer is verified before a single field is read: the control plane is parsed from another process's
memory (P6) and from another role's (P5)."

Failure scenario: under P6 a truncated or hostile first frame crashes the server in `Accept` on a null
deref instead of returning `MOBILEGL_ERR_PROTOCOL_MISMATCH`. Fix: fold `msg_as_X() == nullptr` into the
existing `envelope == nullptr || msg_type() != X` guard in both files.

### M-4. A `Start()` that fails after `Accept()` succeeded leaks the server session, dangles its transport, and makes the pair permanently unusable

`ClientSession::Stop()`'s `!m_started` branch (`ClientSession.cpp:311-319`) tears down only the client:

```
m_producer.Detach();
m_shm.Close();
m_clientTransport.reset();
m_serverTransport.reset();
m_transport = nullptr;
return;
```

It never calls `Server::ServerSessionInstance().Close()`. Five call sites reach it after `Accept()` has
already returned OK — the `Welcome` geometry cross-check (`:210`), `m_cmd/m_stage` invalid (`:231`),
`AttachInProcess` failure (`:221`), `m_replies/m_events` invalid (`:245`), and `ServerLoop::Start` failure
(`:304`). Consequences, all at once:

* the server keeps its four mappings (48.25 MiB at the default geometry) and stays `m_accepted`, so the
  next `Start()` gets `MOBILEGL_ERR_INVALID_ARGUMENT` from `ServerSession::Accept`'s own guard
  (`ServerSession.cpp:188-190`) — the process can never open a session again;
* `Wire::SegmentTable::InstallProcessResolver()` stays installed (it is only undone in
  `ServerSession::Close`, `:376-380`);
* `m_serverTransport.reset()` destroys the transport while `ServerSession::m_transport` still points at it
  (`ServerSession.cpp:191`) and `ServerSession::m_consumer` holds two `Doorbell*` into it
  (`:241`) — dangling pointers reachable through `ServerSession::Control_Plane()` /
  `ConsumerDoorbell()` / `ProducerDoorbell()`.

Fix: the `!m_started` branch must close the server session before dropping the transports, in the same
order the `m_started` path already gets right (`:360-361`).

### M-5. `SessionConsumer::RetireThrough` retires **every** popped byte, `kRecBorrowSlot` and all — the two-tail design is defeated and `PublishRetiredUpTo` has no caller

`Ring.cpp:223-230`:

```
void SessionConsumer::RetireThrough(std::uint64_t seq) {
    Watermark::AdvanceRetired(*m_control, seq);   // clamps the SEQ
    m_cmd->PublishRetired();                      // publishes BOTH tails = m_localTail
    NotifyClient();
}
```

`RingConsumer::PublishRetired()` (`Ring.cpp:287-294`) stores `m_localTail` into **both** `appliedTail` and
`retiredTail`, i.e. it hands back every byte the consumer has popped, irrespective of `seq` and
irrespective of whether any of those records carried `kRecBorrowSlot`. The `seq` clamp is cosmetic: it
moves a number nobody reclaims against, while `RingProducer::TailForReclaim()` (`Ring.cpp:110-115`) reads
`retiredTail` and immediately treats the bytes as free.

This contradicts two header comments written to prevent exactly this:

* `Ring.h:24-27` — "TWO tails per ring, not one. Once the server borrows a ring slot into the GPU timeline
  … that slot can only be recycled after completedFrameSerial";
* `SessionRings.h:329-332` — "Records without `kRecBorrowSlot` retire as soon as they are applied; a
  borrowed slot retires on `completedFrameSerial`, which is why this is a separate call."

`RingConsumer::PublishRetiredUpTo(cursor)` — the API built for the borrow case, which clamps to
`appliedTail` and takes the `RingRecordView::cursor` the consumer is handed — has **zero callers**
(`grep -rn PublishRetiredUpTo` → declaration `Ring.h:285`, definition `Ring.cpp:296`, nothing else).

Latent today: nothing sets `kRecBorrowSlot` (only the enum at `Ring.h:148` and the two comments).
Failure scenario the day it is set: the producer overwrites a ring slot the server has borrowed into a
live command buffer, and the GPU reads a later record's bytes as the earlier record's payload.

Fix: either `RetireThrough` must track the cursor of the last non-borrowed record and use
`PublishRetiredUpTo`, or the header comments must be deleted and `kRecBorrowSlot` retired, so no later
package trusts a guarantee that is not implemented.

### M-6. `AttachInProcess` aliases the owner's `ShmSegment` objects instead of mapping — the attach half of the "one code path" claim is not true

The package's stated point (`SessionRings.h:22-28`, the report's §1.2 first bullet, the brief's item 6) is
that inproc goes through `ShmSegment` so that mapping, alignment, size rounding, the peer view and lifetime
are all exercised before P6. `Create` does this (`ShmSegment.cpp:165-186`: `Create` then `Map(false)`,
verified by `SessionTest.SessionSegmentsAreRealSharedMemoryAndNotAHeapAllocation`). **`AttachInProcess`
does not** (`ShmSegment.cpp:213-215`):

```
for (index…) m_segments[index] = owner.m_segments[index];   // raw aliases into owner.m_owned[]
```

No `Adopt`, no second `Map`, no second mapping. So the *attach* side — which is the side P6 replaces with
`ShmSegment::Adopt(fd)` + `Map` — is first exercised on the day the second process appears, which is the
exact criticism s1 levels at `new[]` three paragraphs earlier. It also creates a raw lifetime coupling:
`ClientSession::m_shm` holds pointers into `ServerSessionInstance().m_shm.m_owned[]` with no ownership,
which is what makes M-4's ordering matter and what `ClientSession::Stop()`'s `m_started` path has to get
right by hand (`:360-361`).

The available alternative costs nothing today: `ShmSegment::Adopt(::dup(owner.DescriptorFor(slot)), size, …)`
+ `Map(false)` gives a genuine second mapping of the same memfd in the same process, exercises both calls
now, makes the per-role ledger honest (two mappings, two fds), and removes the aliasing. `Adopt` already
exists and is already POSIX-complete (`ShmSegmentPosix.cpp:120-142`).

*Judgement call.* Overturned by a decision that P6's `Adopt` path is w1/P6's to prove, not s1's — but then
`SessionRings.h:22-28`'s argument applies to it equally and should say so.

### M-7. The 2 MiB ruling is right, but the reason given for rejecting the zero-cost alternative is false, and two live documents still say 4 MiB

The arithmetic checks out exactly:
`sizeof(RingControl) == 4096` (`Ring.h:127`); 8 MiB − 4096 = 8 384 512;
`LargestPowerOfTwoAtMost(8 384 512) = 4 194 304` (`Ring.cpp:22-31`); `MaxRecordBytes() = capacity/2`
(`Ring.h:238`) = **2 097 152 = 2 MiB**. Confirmed by
`SessionTest.TheRingIsTheLargestPowerOfTwoLeftAfterTheControlPage` and reproduced in my run.

The problem is the rejection of alternative one. `SessionRings.h:45-47` and report §2.1 both say announcing
`4096 + 8 MiB` "breaks the four sizes `ProtocolSmokeTest.cpp:72` pins". `ProtocolSmokeTest.cpp:71-80` builds
four `SegmentRef`s from its own literals inside the test body and round-trips them; the file never mentions
`SessionSegments`, `SessionSegmentSizes`, or any session at all (`grep -n "SessionSegment" ProtocolSmokeTest.cpp`
→ no match). It constrains the *schema's* ability to carry the number, not what the session announces. The
only thing that pins the session's geometry is `SessionTest.TheDefaultGeometryIsTheFourContractSizes` —
s1's own new test, written in the same commit.

So the rejected alternative was available at no cost: `CmdBytes = 4096 + 8 MiB` gives
`LargestPowerOfTwoAtMost(8 388 608) = 8 MiB` exactly, a 4 MiB record cap, CONTRACT-P5 §5 and `Config.h:472`
both true, and no unusable half-segment. As landed, half of SEG_CMD (4 190 208 B) and half of SEG_EVENT
(129 920 B) are mapped and unreachable, and `SessionSegments::MappedBytes()` — the number t1 reports —
counts them.

Left un-updated, both still asserting 4 MiB: `MobileGL/Config.h:471-473` and
`MobileGL/MG_Remote/CONTRACT-P5.md:384`. s1 flagged both in §4 and changed neither.

### M-8. A refused backwards watermark is a permanent hang, logged at ERROR and nothing else

`Ring.cpp:49-67`, `AdvanceMonotonic`: a `to < current` advance logs `MGLOG_E` and **returns**. The header
above it (`SessionRings.h:177-183`) calls this "refused loudly" and "the detectable half".

It is detectable, and then nothing is done about it. `SessionConsumer::ApplyOne` keeps incrementing its own
`m_appliedSeq` (`SessionRings.h:322`), so once the shared `appliedSeq` is behind, every later
`AdvanceApplied` is a no-op forever, and every `WaitForApplied(seq, kWaitForever)` — the verb barrier and
every reply wait — blocks permanently. The user sees a hang; the only evidence is one ERROR line in
`/sdcard/MG/latest.log`.

The tree's own handling of the same class of corruption on the same page is the opposite:
`RingCursorsValid` false is "a Fatal{ProtocolCorruption}, never a retry" (`Ring.h:181-185`), and
`RingConsumer::Pop`'s corrupt-header path sets `*outCorrupt` so the caller escalates
(`Ring.cpp:249-259`). Fix: escalate, or at minimum return a bool the caller must check.

---

## MINOR

### m-1. `SessionSegments::Close()` does not close owned segments on `Create`'s own failure path

`ShmSegment.cpp:165-194`: `m_owns = true` is set at `:188`, **after** the create/map loop. Both in-loop
failure branches call `Close()` (`:172`, `:183`) while `m_owns` is still false, and `Close()`'s
`if (m_owns) { for (ShmSegment& s : m_owned) s.Close(); }` (`:289-293`) is therefore skipped. A failure at
segment 3 leaves segments 0-2's descriptors and mappings open with `Valid()` false. They are reclaimed only
by `~SessionSegments` or by a later successful `Create` (`ShmSegment::Create` does `out.Close()` first,
`ShmSegmentPosix.cpp:55`), so this is not an unbounded leak — but `ShmSegment.h:66`'s contract for `Close()`
is "unmaps and releases the descriptor/handle", and it does not.

### m-2. `RoleMemory.h` says ERROR level; the implementation logs at INFO

`RoleMemory.h:80-83`: "Emits one line at ERROR level (the wire layer's only level - WireLog.h) so t1's
harness can grep it out of a lane log". `ShmSegment.cpp:118-133` uses `MGLOG_I` and its own comment says
"INFO, not DEBUG". t1 is the consumer named in the header and will grep for the wrong level.

### m-3. `ServerSession::Accept`'s post-attach failure paths leave dangling pointers

`:237-240`, `:247-250` and `:282-285` call `m_shm.Close()` and return, after `m_consumer.Attach(control, …)`
at `:241` and `m_replies = ReplyPool(m_shm.ReplyBase(), …)` at `:253`. `m_consumer`/`m_replies`/`m_events`
then hold pointers into unmapped memory until someone calls `Close()`. `m_transport` is also left set
(`:191`). Same family as M-4, error-path only.

### m-4. `ReplySlotHeader` is read and written with plain `memcpy` across threads

`ReplySlot.h:168-179` / `:199-202`. The release/acquire **fences** are correctly placed (payload before the
stamp on write; header, fence, payload on read), but the stamp itself is a non-atomic 16-byte `memcpy`, so
the guard read is a formal data race and the self-check relies on an untorn 8-byte `Seq`. Fine on
arm64/x86-64 with the default 8 MiB/8 geometry (1 MiB slots, 8-aligned); not fine if a future geometry makes
`sizeBytes / slotCount` non-8-aligned — `ReplySlotPool`'s constructor (`:100-118`) does not require it.

### m-5. The self-check cannot see a drift of exactly `slotCount`

`ReplySlot.h:203-215` catches seq 13 reading slot 5's answer (and the test pins it, `SessionTest.cpp:528`).
A sequence space drifted by 8, 16, … lands on the same slot with a matching stamp and is accepted as this
call's answer. Under R-1's depth-1 barrier this cannot arise; worth naming as P9's account since P9 is what
removes the barrier.

### m-6. `RetireThrough` is mandatory and nothing says so

`RingProducer::FreeBytes()` reclaims against `retiredTail` only (`Ring.cpp:110-115`), which nothing but
`RetireThrough` publishes. An apply loop that calls `ApplyOne` and never `RetireThrough` wedges the producer
on the first full ring. `SessionRings.h:329-332` documents `RetireThrough` as being *about borrowed slots*,
which reads as optional. v1 writes that loop.

### m-7. Dead members and an unused config guard

`ReplySlotPool::m_size` (`ReplySlot.h:245`) is stored and never read after construction;
`ClientSession::m_caps` (`ClientSession.h:146`) is declared and never assigned (`Caps()` returns
`CapsMirrorInstance()`); `ClientSession.cpp:237/249` read `MG_Config::Ipc.*` without the
`#if MOBILEGL_BUILD_DISAGGREGATED` guard `ServerSession.cpp:88/98` uses for the same struct.

---

## QUESTION

### q-1. Teardown's drain keys on `submittedSeq`, which the ring's contract lets lag

`ClientSession.cpp:326-337` reads `control->submittedSeq` and waits for `appliedSeq >= submittedSeq`.
`Ring.h:72-77` explicitly permits `submittedSeq` to be published lazily, and `Ring.h:243` encourages
batching it ("batching 8-16 only amortizes the doorbell store"). With the barrier armed the depth is 1 and
the number is exact; with `MOBILEGL_IPC_VERB_BARRIER=0` — R-1's negative control, which the phase is
required to run once — it is not, and the drain under-waits and drops records. Is that acceptable in the
negative-control lane, or should `SessionProducer` track its own last-published seq?

### q-2. `Watermark::AdvanceCompletedFrame` / `AdvancePresentAck` have no notify partner

Both are free functions with no wakeup (`Ring.cpp:87-93`). `ServerSession::AdvanceCompletedFrame` /
`ReturnPresentCredit` (`:446-460`) correctly call `m_consumer.NotifyClient()` after, and
`SessionTest.cpp:428-429` reproduces the pair by hand — but the free functions are public, and a v1 caller
that uses them directly leaves a client parked in `WaitForPresentAck(kWaitForever)` with no wakeup. Should
the notify be folded into the advance, or the free functions made private to the session?

### q-3. `EventRingProducer::PublishAndNotify` takes an arbitrary bell and flag

`EventRing.h:139-142` takes `(Doorbell&, std::atomic<uint32_t>&)` from the caller. Nothing in production
code calls it (it is v1's), so the one pairing that is correct — the client's own bell and `producerParked`
— exists only in `SessionTest.cpp:615-616`. Every other pairing compiles. Should it take the
`SessionConsumer` (which already holds the right two) the way the forward direction does?

---

## What I verified and it held

**Every acceptance number in `s1-v1.md` reproduces.** Independent four-directory configure + build +
gate, `~/w7/p5-s1rev-gaterun.log`:

| claim | my run |
|---|---|
| G1, `--threshold 0`, vs `~/w7/p5-before-libMobileGL.so` | `.text 10806323 -> 10806323 (+0, +0.000%)`; `27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed` — **exact match** |
| four builds rc=0, `CTestTestfile.cmake` present in each | confirmed before any verdict |
| `ctest -L unit` pull / push / verify | 1785 / 1785 / 1785, 0 failed |
| `ctest -L unit` split | **1860 / 1860** (1842 baseline + 18) |
| `ctest -L integration-gpu` in `build-push`, `MOBILEGL_TRANSPORT=monolith` | **1117 / 1117** |
| `ctest -N` name identity vs `p5-before-ctest-names.txt` | 2902 names, sorted diff **0 lines** (G2/G14). Note: the raw `ctest -N` order differs from the before-file because that file was captured sorted; the *set* is identical, which is what G14 is about. "byte-identical" in the report is loose phrasing, not a wrong result. |
| `gen_protocol.py --check` | clean — `--check` regenerates in place with the pinned flatc and compares; the committed `protocol_generated.h` is byte-identical to what flatc produces, so the header **was** regenerated and committed |
| `gen_pipe.py --check` | up to date |
| purity, `check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)`; `wire-header OK 2 headers in closure, 0 forbidden` — every new `Transport/` header stays clear of `Includes.h` |
| `nm` role check | pull `MG_Remote` 0, split 275 |
| s1's exact Wire regex in `build-split` | 75 / 75 |

**Flake hunt** (`~/w7/p5-s1rev-flake.log`): `ctest -R '^SessionTest' -j 8` × **20 reps** — 0 failing reps;
the `SessionTest` binary with `--gtest_repeat=30` — 18 tests × 30, all passed, rc=0. No flakes. The
shutdown case's join is genuinely bounded (`SessionTest.cpp:720-729`: 5 s deadline, `apply.detach()` then
`FAIL()`), and the detached thread holds its own `shared_ptr<Shared>` so the detach is not a UAF.

**Publish-then-notify order, every new call site.** All five are correct, and no fence was removed or
reordered — `Doorbell.h`/`Doorbell.cpp` are not in the diff at all:

* `SessionProducer::PublishAndNotify` (`Ring.cpp:119-134`): `Publish()` → `AdvanceSubmitted` →
  `NotifyIfParked(peer, consumerParked)`;
* `SessionConsumer::ApplyOne` (`SessionRings.h:310-327`): `apply(view)` → `++m_appliedSeq` →
  `AdvanceApplied` (release) → `PublishApplied` → `NotifyClient`;
* `SessionConsumer::RetireThrough` (`Ring.cpp:223-230`): advance → publish → notify;
* `ServerSession::AdvanceCompletedFrame` / `ReturnPresentCredit` (`:446-460`): advance → `NotifyClient`;
* `EventRingProducer::PublishAndNotify` (`EventRing.h:139-142`): `Publish()` → `NotifyIfParked`.

The fence pairing is intact on both sides: the waiter is `Doorbell::Wait`'s
`parked.store(relaxed)` → `atomic_thread_fence(seq_cst)` → `ready()` (acquire load of `cmdHead` /
a watermark); the notifier is a release store → `NotifyIfParked`'s `atomic_thread_fence(seq_cst)` →
relaxed load of the flag. `Watermark::AdvanceMonotonic` publishes with `memory_order_release` and
`Watermark::Reached` loads with `memory_order_acquire` (`Ring.cpp:66`, `SessionRings.h:207`).

**`appliedSeq` is +1 per record with a single writer.** `SessionConsumer::ApplyOne` is the only caller of
`AdvanceApplied` in the tree, the increment is on a member of a class documented single-threaded, and
`Attach` re-syncs `m_appliedSeq` from the shared page (`Ring.cpp:195`) so a re-attach cannot restart at 0
and stall the watermark. The 64-record batching R-9 bans is not merely unimplemented — there is no batching
code path at all, and `AppliedSeqAdvancesExactlyOncePerRecordAndIsNeverBatched` checks the watermark
*inside* the pop loop rather than at the end.

**`kRecPad` cannot be counted by one side and not the other.** `RingConsumer::Pop` skips fillers before
returning (`Ring.cpp:261-264`), so a pad never reaches `apply` and never reaches the increment;
`AWrapFillerDoesNotAdvanceTheSessionsAppliedSeq` drives ~5 laps of a 4 KiB ring with a 112-byte record that
cannot divide it, asserts `fillerBytes > 0` so the case cannot pass vacuously, and asserts
`view.flags & kRecPad == 0` inside the apply callback. The 20 000-record two-thread run wraps a 32 KiB ring
~10 times and ends with `appliedSeq == submittedSeq == 20000`.

**I withdrew one suspected defect.** I expected `EventRingConsumer::Drained()` (`EventRing.h:172-177`) to
break `head >= appliedTail >= retiredTail` by publishing only the retired cursor. It does not:
`RingConsumer::PublishRetired()` stores `m_localTail` into **both** tails (`Ring.cpp:287-294`,
"retiredTail must never overtake appliedTail, so publish both"). `RingCursorsValid` holds on the event ring.

**The ABI assertion runs where it is claimed to.** `ServerSession::Accept` steps 1-2
(`ServerSession.cpp:196-226`) receive `Hello`, compare `abiMajor`/`abiMinor` and then
`CapsAbiFingerprint()`, and only at `:229` is the first segment created — no ring, no `RingConsumer`, no
decode exists before the comparison. A mismatch goes through `FatalAbiMismatch` (`:68-84`), which is
`MGLOG_F` followed by `std::abort()` — a real abort, no downgrade branch, `[[noreturn]]`. The client's half
(`ClientSession.cpp:185-188`) is the same. The fingerprint is genuinely sensitive to each of its inputs
(`TheAbiFingerprintChangesWhenAnyOfItsInputsDoes`, seven negative controls including
`nullptr` vs `""`), and 0 is reserved for "not stated" (`Ring.cpp:268`). The two weaknesses are M-1
(it does not cover the schema) and s1's own §5 note about the configure-time git stamp.

**Shutdown ordering.** `ClientSession::Stop()`'s `m_started` path (`:322-369`) is drain →
`m_transport->Shutdown()` → `ServerLoopInstance().Stop()` (the join) → release. `InProcessTransport::Shutdown`
is `m_channel->Close()`, which calls `bell.Kill()` on **both** endpoints
(`InProcessTransport.cpp:74-90, 287`), so the apply thread parked on `kWaitForever` really can come back —
`Doorbell::Wait` returns false on `Dead()` (`Doorbell.h:169-175`) and `SessionConsumer::WaitForWork` turns
that into `SessionWait::ShutDown` (`Ring.cpp:220`). The client's attached view is closed *before* the
owner's (`:360-361`), which is the only order that is not a use-after-free given M-6's aliasing. The
"sticky" half is pinned too: a `WaitForWork` that *arrives* after the shutdown returns `ShutDown` in under
1 s rather than parking, and so does a producer blocked in the barrier (`SessionTest.cpp:737-746`).

**The four announced sizes.** `SessionTest.TheDefaultGeometryIsTheFourContractSizes` allocates the real
default geometry and gets 8 MiB / 32 MiB / 8 MiB / 256 KiB — the four contract numbers, unchanged.
`ProtocolSmokeTest.cpp` was not edited and its `CreateWelcome(builder, …, event)` call still compiles
because the two appended `Welcome` fields default (offset 0 / scalar 0), which is a correct flatbuffers
append — the `Welcome` and `Hello` additions are genuine tail appends onto slots nothing ever occupied.
Only `CapsSnapshot` has the M-1 problem.

**Ownership.** No new `.cpp` in the root `CMakeLists.txt`; the only file touched outside s1's ownership is
`Server/PipeApplier.cpp`, and the diff there is exactly one `#include` and one function body
(`ReplyPool::PostReply`, 2 statements) — no `PipeApplier::*` body, no header, no `.def`, no generator, as
claimed. The `MG_Test/Wire/CMakeLists.txt` change is a single appended `SessionTest` line.

**Reply pool.** Addressing is `seq & (slotCount-1)` in one place (`ReplySlot.h:240-242`), which
`ReplyPool::PostReply` delegates to rather than re-deriving; seq 0 is refused on both sides; `Clear()` runs
at `Accept` so a stale stamp cannot read as this session's answer; the oversize `Post` really aborts (the
death test is in the 18 and passes).

---

## Verdict

**I would let this into the integration, with B-1, M-1, M-2, M-3 and M-4 fixed first.** All five are small,
local edits; none requires a redesign. The watermark core — the part of the package that is hardest to fix
later and that the phase's correctness rests on — is sound: one writer, +1 per record, no batching path,
pads skipped before counting, release/acquire on every advance, and the publish-then-notify order right at
all five new call sites with both fences intact. M-5 and M-6 are the two design debts I would want written
into the accounts rather than left in a header comment that says the opposite of the code.
