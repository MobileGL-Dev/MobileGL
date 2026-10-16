# P5 w1 — adversarial review

Package `p5/w1`, `bc969f9a..52bf9acc` (8 commits), worktree `~/w7/p5-w1` at `52bf9acc`,
base `feat/disaggregated @ 5175f9cc`. Report under review: `~/w7/notes/p5/p5-results/w1-v1.md`.

**Verdict: 0 BLOCKER / 5 MAJOR / 8 MINOR / 3 QUESTION. Let it into the integration**, with M4
and M5 ruled on by the integrator before s1 and c1/b1 land, because both are cross-package
disagreements that get more expensive the later they are settled. Nothing I could construct
made the decoder read out of bounds or apply wrong bytes on P5's reduced path.

Logs retained and cited below: `~/w7/p5-w1rev-gate.log` (G1 + twelve unit-lane runs),
`~/w7/p5-w1rev-itest.log` (integration-gpu, generators, `git diff`),
`~/w7/p5-w1rev-probe.log` (twelve review probes), `~/w7/p5-w1rev-symbols.md`.
The probes were spliced into `MG_Test/Wire/PipeWireCodecTest.cpp`, built into `build-split`,
run, and the file restored; `git status --porcelain` is empty and HEAD is unmoved at
`52bf9acc` (`~/w7/p5-w1rev-probe.log`, last lines).

---

## The flag-space collision: confirmed, and there is a third one

**Confirmed from both headers.** `MG_Pipe/MGPipe.h:39-54` gives
`kNeedsAck=1<<0, kHasBlob=1<<1, kVarTail=1<<2, kHostSpan=1<<3, kReplySlot=1<<4, kOptional=1<<5`.
`MG_Remote/Transport/Ring.h:143-150` gives
`kRecNeedsAck=1<<0, kRecHasBlob=1<<1, kRecPad=1<<2, kRecBorrowSlot=1<<3, kRecVarTail=1<<4`.
`RingRecordHeader` (`Ring.h:136-140`) and `MGPWireRecHeader` (`PipeWire.inc:35-39`) are the
same 8 bytes in the same order, and `RingConsumer::Pop` (`Ring.cpp`, the `Pop` body) skips a
record on **`(header.flags & kRecPad) != 0` alone** — no kind check, no checksum. So w1's
headline is exact: stamping `MGPipeCallFlagsFor(op)` would make `Pop` discard all nine
`kVarTail` opcodes as wrap fillers, silently. The encoder's translation
(`PipeWireCodec.cpp:709-713`) does the right thing, and `PipeWire.inc:37` still says
*"MGPipeCallFlags of the call"*, which is the comment w1 asked c0 to change and which has not
been changed.

**Pushing past it** — four answers w1 does not give:

- **The translation is total in one direction and lossless, but not injective as an
  interpretation.** `kNeedsAck→kRecNeedsAck`, `kHasBlob→kRecHasBlob`, `kVarTail→kRecVarTail`;
  `kReplySlot`, `kOptional`, `kHostSpan` get no ring bit, and none is needed because
  `MGPipeCallFlagsFor(op)` recovers all six from the opcode. Nothing is lost.
- **There is a third alias and nobody pinned it: `kReplySlot (1<<4) == kRecVarTail (1<<4)`**
  (finding m1). The four `static_assert`s at `PipeWireCodec.cpp:69-84` cover
  `kVarTail/kRecPad`, `kHostSpan/kRecBorrowSlot`, `kNeedsAck` and `kHasBlob` — not this pair.
  It matters in the *reverse* direction of w1's hazard, which is live today: the encoder
  stamps `kRecVarTail` on nine opcodes, and a reader who believes `PipeWire.inc:37` reads
  those nine as `kReplySlot`.
- **A record whose header flags were stamped by a future producer is diagnosed in exactly one
  of the six cases.** A `kVarTail` stamp is eaten by `Pop`. A `kRecPad`-free stamp
  (`kReplySlot`, `kOptional`, `kHasBlob`, `kNeedsAck`, `kHostSpan`) reaches
  `DecodeAndApply`, which checks only `flags & kRecPad` and `kind == kRingPadRecordKind`
  (`:925-933`) and then ignores `record.flags` entirely. There is **no decoder-side check that
  `record.flags` equals the translation of `MGPipeCallFlagsFor(op)`** — which is a one-line
  check the decoder is uniquely placed to make, and which would turn the whole class from
  silent into Fatal.
- **The translation is not pinned against either enum gaining a member.** The asserts pin four
  specific bit values; nothing states that `{kNeedsAck, kHasBlob, kVarTail}` is the whole
  translated set and `{kHostSpan, kReplySlot, kOptional}` the whole deliberately-dropped set.
  A seventh `MGPipeCallFlags` enumerator that needs a ring bit is dropped in silence, and a
  sixth `RingRecordFlags` bit colliding with it fires nothing. Probe
  `W1REV_AThirdCollisionReplySlotIsRecVarTail`.

---

## MAJOR

### M1 — R-2 arm 4 does not cover `MGHostSpan`; an out-of-segment span passes every check and reaches the sink as "validated"

**Evidence.** `PipeWireCodec.cpp:413-428`. `CheckHostSpanIsHonest(const MGHostSpan&)` takes no
`SegmentTable` and checks three things: `Ptr != nullptr` (arm 1), `Size != 0 && Seg == None`
(arm 3), and `Size == 0 && Seg != None` (w1's added shape). **There is no
`Resolve(span.Seg, span.Offset, span.Size)` anywhere on either path** — not in the encoder's
host-span pass (`:764-773`), not in the `SetShaderBuffers` arm (`:1293-1302`), not in the
`DrawVbo` arm (`:1463-1471`). Arm 4 is implemented for blobrefs only, via
`CheckBlobIsHonest`→`SegmentTable::Resolve`.

**Proved, not inferred.** Two forged records, both children exited 0 rather than aborting
(`~/w7/p5-w1rev-probe.log`):
- `W1REV_AHostSpanRunPastItsSegmentIsNotRefused` — `SetShaderBuffers`, `Count=HostSpanCount=1`,
  span `{Ptr=nullptr, Seg=kSegStage, Offset=kStageBytes-8, Size=1024}`.
- `W1REV_DrawVboUserIndexSpanReachesTheSinkWithoutARangeCheck` — `DrawVbo` with
  `kDrawHasUserIndices`, span `{Seg=kSegStage, Offset=0xFFFFFFFF, Size=0xFFFF}`; the span is
  `memcpy`d into a local and handed to `WireVerbSink::OnDrawVbo`.

**Failure scenario.** `PipeWireCodec.h:329-331` promises `OnDrawVbo`'s `userIndices` is part of
"a DECODED, VALIDATED argument list", and `:293-296` says the codec "bounds-checks, cross-checks
the tail, resolves the segments and hands over" that list. When P8 arms
`kCapNeedsHostIndexBytes`, v1's sink will believe that. A span off a corrupt or mis-computed
record then either resolves to `nullptr` through `MGPipeHostBytes` (`MGPipeHostSpan.h:48-55`)
and draws from a null index pointer, or — for any consumer that adds `Offset` to its own
`SEG_STAGE` base rather than going through the resolver — reads out of the segment. The layer
that was supposed to refuse it is this one; the fact that P5 emits no spans is what makes it
latent, not what makes it safe.

**Fix.** `CheckHostSpanIsHonest` is c0's signature, so either the integrator lets it take a
`const SegmentTable&` and it Fatals when `Resolve(...) == nullptr`, or the two callers do the
resolve themselves. One line either way.

### M2 — the encoder (the producer) publishes the consumer's SEG_STAGE cursors

**Evidence.** `PipeWireCodec.cpp:814-815`, inside `PipeWireEncoder::ReclaimStagedBytes`:

```
m_control->stageAppliedTail.store(upTo, std::memory_order_release);
m_control->stageRetiredTail.store(upTo, std::memory_order_release);
```

`Ring.h:104-109` labels the mirror-image cmd triple `cmdAppliedTail // consumer:` and
`cmdRetiredTail // consumer:`; the declared writers are `RingConsumer::PublishApplied` /
`PublishRetired` (`Ring.cpp:282, :290`); and `RingProducer::TailForReclaim` (`Ring.cpp:111`)
*reads* `stageRetiredTail` to compute `FreeBytes()`. Ring.h's five-watermark contract
(`:59-63`) says `retiredSeq` is "Advanced by the CONSUMER once it has finished with the
SEG_STAGE bytes a record referenced. The staging allocator reclaims behind it, **and nothing
else may**." `CONTRACT-P5.md` records the inversion nowhere — I read §0, §1 and §2 for it.

**Why this is the finding and not a preference.** w1's own §8.1(b) argues the opposite
principle for `appliedSeq` and calls a second writer "how a waiter resumes on a record the
server has not run". This is the same shape, one segment over, committed in the same file.

**Failure scenario.** s1 constructs `RingConsumer(control, stageBase, cap,
RingCursorSet::Stage)` — the obvious thing to do for a segment that owns a cursor triple — and
calls `PublishApplied()`/`PublishRetired()` on it. That consumer's `m_localTail` never moves,
because nothing ever `Pop`s the stage ring (the decoder resolves by offset, `:889`). Both
cursors are then written forward by the client and back to their initial value by the server;
`RingProducer::FreeBytes()` oscillates, and `StageBytes` either Fatals `RingOverrun` on a ring
that is empty or hands out a run the previous record is still being read from. Nothing on the
ring detects it.

**Fix.** Write the inversion into `CONTRACT-P5.md` table 1's "owner" column for SEG_STAGE and
forbid a `RingConsumer` on `RingCursorSet::Stage` outright, or move the reclaim to the server
where Ring.h puts it. Either is fine; the two statements coexisting is not.

### M3 — `PipeWireEncoder::m_stageMarks` grows without bound whenever one record is always in flight

**Evidence.** `:781-783` pushes one `StageMark{seq, cursor}` per encoded record.
`:794-805` walks `m_stageMarkFront` forward and then compacts **only** when the queue has
drained completely:

```
if (m_stageMarkFront != 0 && m_stageMarkFront == m_stageMarks.size()) {
    m_stageMarks.clear();
    m_stageMarkFront = 0;
}
```

Entries before `m_stageMarkFront` are never erased in any other path.

**Failure scenario.** Any pipelining deeper than "fully drained at every reclaim" — which is
precisely the regime w1's W-3 says the design exists to survive ("it keeps working when R-1's
barrier retires family by family"). One unretired record at each `ReclaimStagedBytes` means
`front < size()` for ever, the vector never clears, and it grows 16 bytes per encoded record
for the life of the context, with the reallocation copies on top. OpenRA at a few thousand
records per frame and 60 Hz is megabytes per second of `Vector<StageMark>`.

**Fix.** Erase the consumed prefix (`m_stageMarks.erase(begin, begin + front)`), or compact
whenever `front` exceeds a threshold rather than only when it equals `size()`, or use a
fixed-capacity ring since the live depth is bounded by records in flight.

### M4 — the decoder writes a reply slot for four opcodes the catalogue gives no `kReplySlot`

**Evidence.** `postAcceptance` (`:1013-1018`) posts into the reply sink on the record's own seq,
and is called for `ResourceCreate` (`:1040`), `ResourceRespecify` (`:1059`), `SetTextureParams`
(`:1369`) and `ResourceSubData` (`:1390`). `PipeWire.inc:141, 142, 187, 187` give those four
`kNone`, `kNeedsAck`, `kNone` and `kHasBlob|kVarTail` — **none carries `kReplySlot`**.
`CONTRACT-P5.md` table 0's `MGPReplySlot::Id` row says there are **ten** `kReplySlot` calls,
and the table's own count of `kReplySlot` rows is ten. The contract's reply-slot-header row,
meanwhile, says DECLINED "is how the four `Bool` acceptance entry points say `false` (R-5)".
The contract disagrees with itself and w1 implemented one half.

Probe `W1REV_AcceptanceRepliesArePostedForOpsTheCatalogueGivesNoReplySlot`: one reply recorded
for `ResourceCreate`, whose `MGPipeCallFlagsFor` is `0`.

**Failure scenario.** Table 0 says `kMGPipeCallFlags` is "read only through
`MGPipeCallFlagsFor(op)`" and is what "every package" reads, so s1 sizes `ReplyPool` and decides
"does this record need a slot" from it. No slot is then reserved for `ResourceSubData` — the
highest-frequency record class in the catalogue — yet every upload record writes
`SEG_REPLY[seq % slots]`. A client blocked on a `read_pixels` or `map_persistent` answer has its
slot overwritten by a later upload; because the slot header stamps the *writer's* seq for
self-check (table 0's slot-header row), the waiter's check fails for ever and the barrier
**hangs** rather than returning a wrong answer.

**Fix.** Integrator ruling, before s1 lands: either `gen_pipe.py` adds `kReplySlot` to those
four `PipeCalls.def` rows and the pool sizes accordingly, or the decoder stops posting and
R-5's acceptance answer rides `kNeedsAck`'s ack path instead.

### M5 — `BufferSubDataResident`'s layout claims a var tail its catalogue row denies, and the arm drops it

**Evidence.** `MGPipeWireRecordLayout` handles both subdata ops in one arm (`:530-536`), so a
`BufferSubDataResident` record with `RegionCount = 2` is *required* to carry
`2 * sizeof(MGPSubRegion)` = 80 bytes. But `PipeWire.inc:188` gives the op `kHasBlob|kOptional`
and no `kVarTail`, so `EncodeRecord` stamps no `kRecVarTail` on it (`:713`); and the decoder arm
(`:1395-1407`) calls `MGPipeApplyBufferSubDataResident(rec, bytes)`, which takes **no regions**
(`PipeApply.h:901`). Probe `W1REV_BufferSubDataResidentLayoutClaimsATailItsCatalogueRowDenies`
(`TailCount == 1`, `TailBytes[0] == 80`, call flags have no `kVarTail`).

**Failure scenario.** `MGPipeBuildSubDataRecord` is the shared builder that fills `RegionCount`
and is already the producer for both halves (contract table 1 rows 7 and 8). The moment c1/b1
route the resident path through it with a non-zero `RegionCount`, either `EncodeRecord` Fatals
on a caller that handed it no tail, or the record carries 80 bytes the applier never sees and
the upload silently loses its region list — the failure mode rule A exists to kill.

**Fix.** Either restrict the layout arm to `ResourceSubData` and make a non-zero `RegionCount`
on the resident op Fatal, or give the catalogue row `kVarTail` and pass `tailAt(0)` through.
The two statements as they stand cannot both be right.

---

## MINOR

**m1 — the third flag collision is unpinned, and the translation is not asserted exhaustive.**
`kReplySlot (MGPipe.h:50) == kRecVarTail (Ring.h:149)`; the asserts at `PipeWireCodec.cpp:69-84`
do not name it. Worse, nothing asserts that the three translated bits plus the three dropped
ones are the *whole* of `MGPipeCallFlags`, so a seventh enumerator that needs a ring bit is
dropped in silence. Probe `W1REV_AThirdCollisionReplySlotIsRecVarTail`. Fix: add the fourth
assert and an exhaustiveness assert over the two enums' full masks.

**m2 — nothing pins `MGPWireRecHeader`'s layout to `RingRecordHeader`'s.** The decoder does
`base = payload - sizeof(MGPWireRecHeader)` (`:935`) and `size = payloadSize +
sizeof(MGPWireRecHeader)` (`:936`). Both structs are separately asserted to be 8 bytes; no
assertion says the fields line up. If `RingRecordHeader` ever reorders, the four flag asserts
still pass and every payload read shifts. Probe
`W1REV_TheTwoRecordHeadersAreOnlyEquivalentByConvention` (they do match today). Fix: three
`offsetof` asserts beside the flag ones.

**m3 — `PoisonResolvedRuns` writes the addition `Resolve` refuses to write.** `:908` tests
`blob.Offset + blob.Size > view.Size`; `:339-341` writes the same test as
`offset > view.Size || size > view.Size - offset` with the comment *"written as a subtraction
so offset + size cannot wrap"*. Every run reaching the poison has been through `Resolve`, so it
is unreachable today — but it is the one place in the file where a wrap would be an arbitrary
0xDD `memset`. Fix: use the subtraction form here too.

**m4 — `gMGPipeWireRecordApply` is installed per record and never uninstalled.** `:946` stores
to an inline global from the apply thread on every `DecodeAndApply`, and nothing ever clears
it. After the first record it is live process-wide; only `t_activeDecoder == nullptr` (`:872`)
keeps `MG_Test/Pipe/PipeCatalogueTest.cpp:432`'s direct `MGPipeApplyWireRecord(Present, ...)`
call returning `false`. Fix: install once at construction / next to
`SegmentTable::InstallProcessResolver`, clear on teardown, exactly like the resolver it is
modelled on.

**m5 — a staged blob's ring header is framed as a wrap filler.** `StageBytes` (`:599-600`)
reserves with `kind = MGPWireOp::kInvalid == 0 == Transport::kRingPadRecordKind` and
`flags = kRecHasBlob`. `Pop` skips on `flags & kRecPad` only, so nothing is skipped today; but
`DecodeAndApply` treats `kind == kRingPadRecordKind` as a pad and Fatals (`:925-933`), so
anything that ever pops the stage ring gets the wrong diagnosis. Fix: reserve a non-zero kind
for staged runs.

**m6 — w1-v1 §6.4's list of unbounded counts is incomplete.** `SetShaderBuffers.Count` and
`SetStreamOutputTargets.Count` (`:494-514`) are held to no GL constant either — there are four
unbounded counts, not two, and these two are not named in the `Fatal{RingOverrun, "<call>"}`
the report calls "loud". Probe `W1REV_SetShaderBuffersAndStreamOutHaveNoGLBoundOnTheirCount`:
`Count = 100 000` yields a 2.4 MB / 2.8 MB tail from the layout. All four are still caught (by
the `record.Size` bound or by `MaxRecordBytes()`), so this is a documentation defect, not a
hole — but it is the sentence a later phase will trust.

**m7 — `m_resolved[8]`'s "seven blob members plus one tail" is dead.** `NoteResolvedRun`
(`:878-885`) filters `blob.Seg != kSegStage`; tails live in SEG_CMD, so a tail is never noted,
and `CreateShaderState`'s six per-stage runs are Fatal, so **at most one** run is recorded per
record. The audit's real coverage is "the one blob the arm resolved", not
`PipeWireCodec.h:387-391`'s "every SEG_STAGE byte this decoder resolved". `NoteResolvedRun`
also drops silently past eight rather than Fataling. Fix: correct the header, or make the
overflow loud.

**m8 — the encoder accepts a record the decoder Fatals on.** `CreateShaderState`'s
declared-`Spirv[i]`-is-Fatal rule lives only in the decoder (`:1198-1208`); the encoder's
honesty pass (`:756-763`) runs `CheckBlobIsHonest` over all seven slots, which accepts a fully
declared run. Probe `W1REV_TheEncoderAcceptsThePerStageSpirvRunTheDecoderCallsFatal` —
`EncodeRecord` returns a valid seq. That is exactly the asymmetry `EncodeRecord`'s own comment
(`:672-675`) says it exists to prevent: "dies here, on the producing side, rather than on a
peer that can only report a corrupt stream."

---

## QUESTION

**q1 — will the `0xDD` fill survive P6's spawn?** The poison `memset`s SEG_STAGE from the
server (`:914`). `ShmSegmentPosix.cpp:159` already carries a read-only mapping mode
(`prot = readOnly ? PROT_READ : (PROT_READ|PROT_WRITE)`), and "client stages, server copies;
the server only ever reads it" (contract table 1) is exactly the argument for mapping
SEG_STAGE `PROT_READ` on the server. Nothing passes `readOnly = true` today, so it works in P5
inproc; the day it does not, the phase's only mechanical control on rule C becomes a SIGSEGV
rather than a finding. One line in the contract now would settle it. *Judgement call: I do not
think this blocks P5.*

**q2 — is `W && H && D` the right content predicate for a texture upload?** `:1380-1383`
computes `carriesContent` for the texture half as `UnionBox.W != 0 && H != 0 && D != 0`, so a
record with a zero extent on any axis is read as carrying nothing and rule A's arm 2 never
fires for it. Probe `W1REV_ATextureUploadWithAZeroDepthBoxIsReadAsCarryingNoContent`: a 4×4×0
box with an all-zero blob encodes, decodes and applies. No emitter produces `D = 0` for a 2D
upload today and `PipeApply.cpp:948` has its own `BoxIsEmpty(...) && RegionCount == 0` arm, so
this is a question about which layer owns the predicate — but the predicate is w1's invention
and it is the only thing standing between rule A and "a content record that declares nothing".
*Judgement call.*

**q3 — the reclaim has no trigger in this package.** `ReclaimStagedBytes()` is called from
exactly one place in the tree, `StageBytes` on a failed `Reserve` (`:605`); the header says
"called by the client at its verb barrier", which is c1/b1 code that does not exist. Combined
with w1's own note to v1 (§5 v1-1: "whoever publishes `retiredSeq` should publish it together
with `appliedSeq`"), SEG_STAGE's liveness in P5 depends on two things landing elsewhere. If
`retiredSeq` is left at 0 — which is what `InitRingControl` leaves it at — the first 32 MiB of
staging ends in `Fatal{RingOverrun, "SEG_STAGE"}`. Should `Publish()` call it, or is the
session the owner? w1 flagged half of this; the other half is that nothing in this package
fails if nobody ever does.

---

## What I verified and it held

**The tail cross-check is sound, and I could not get past it.** Every count field in the
catalogue is `Uint32` (`MGPipeTypes.h`: `MGPSubData::RegionCount`, `MGPDrawInfo::NumDraws`,
`MGPShaderBuffers::{Count,HostSpanCount}`, `MGPStreamOutputTargets::Count`,
`MGPSamplerViews::{Start,Count}`, …), so `TailBytesFor(count, elementBytes)` (`:261`) at worst
computes `0xFFFFFFFF * 40 ≈ 1.7e11` and cannot overflow 64 bits; `Align8` cannot wrap at that
magnitude; and the product is bounded against `kMaxRecordBytesOnTheWire` (`:567-569`) **before**
anyone indexes with it. Probe `W1REV_AMaximalCountHitsTheWireSizeBoundRatherThanWrapping`:
`RegionCount = 0xFFFFFFFF` aborts with `Fatal{ProtocolCorruption, "record.Size"}`, not a wrap.
`ApplyChecked` requires `size == layout.TotalBytes` **exactly** (`:991-1000`), not `>=`, so a
short tail, a long tail and a trailing-byte record are all refused. I hand-checked the byte
arithmetic for all three multi-tail shapes against the encoder's gap-fill loop (`:735-750`):
`SetShaderBuffers` (`40 + 24C` then `+32C`), `SetStreamOutputTargets` (`32 + 24C` then `+4C`,
with the 4-byte trailing pad zeroed at `:748-750`) and `DrawVbo` (`64 + 12N`, realigned to 8
before the 32-byte span) all close exactly on `TotalBytes`, in both directions. A zero count is
legal on every row and produces a `nullptr` tail pointer that the appliers tolerate (probe
`W1REV_AZeroCountVarTailHandsTheApplierANullArray`, applied). A non-8-aligned `Size` is
refused three times over (`Pop`, `MGP_WIRE_CHECK_BOUNDS`, and `TotalBytes` being `Align8`'d).
**I could not construct a record that passes the new check and still reads out of bounds.**

**Three of R-2's four Fatal arms are unbypassable on the blobref path, and arm 4's arithmetic
cannot overflow.** `SegmentTable::Resolve` (`:341`) is written as
`offset > view.Size || size > view.Size - offset` — a subtraction, in `Uint64`, with the reason
in the comment. Every arm is an `MGLOG_F` + `std::abort()` with no early return, no
`#if !NDEBUG`, and no path that reaches an applier first: `RequireDeclaredBlob` (`:403-411`)
Fatals *before* `CheckBlobIsHonest`, and `ResolveOrFatal` (`:887-898`) runs
`RequireDeclaredBlob` before it resolves. The four forked death cases plus w1's eight others
all pass. The one gap is arm 4 on `MGHostSpan` — finding M1.

**Padding preservation is real, and I read every `memcpy`/`memset` in the codec to say so.**
The only writes into a record body are `memcpy(bytes, payload, payloadBytes)` (`:725`), the
tail `memcpy`s (`:743`), and three `memset`s — `:733` over `payloadSlack`, which is
`Align8(8+payloadBytes) - 8 - payloadBytes`, i.e. strictly *after* `sizeof(Payload)`; `:740`
over an inter-tail gap; and `:749` over the trailing record pad. None touches a byte inside
`sizeof(Payload)`. No field-by-field construction exists anywhere; the decoder hands the
payload to the applier by reference (`:981`, `:1040` etc.) with no copy at all. The one place a
pad is minted is `StageBytes`'s fresh `MGPBlobRef{..., Pad0 = 0}` (`:627-631`), which w1
documents. w1's `EveryPayloadByteCrossesIncludingTheOnesSpelledPad` (test `:577-629`) is a
genuine test of this: it stamps a whole `MGPSubData` — b1's struct — with `0xA5`, writes only
the validated fields, `memcmp`s all `sizeof(upload)` bytes as they land in SEG_CMD, and then
asserts at least one `0xA5` survived so the case cannot pass vacuously. It names no pad member.

**`SEG_STAGE` and the `0xDD` audit are armed correctly.** `MG_Config::Ipc.Audit` defaults to
`false` (`Config.h:502`) and is set from `MOBILEGL_IPC_AUDIT` only (`ConfigLoader.cpp:374`);
the decoder reads it at construction (`:853`). `PoisonResolvedRuns` is called from
`DecodeAndApply` at `:950`, **after** `MGPipeApplyWireRecord` returns at `:947` and **before**
the session publishes `appliedSeq`/`retiredSeq` (the fixture's `PumpOne`, test `:122-143`, is
the shape s1 must follow), so it cannot poison bytes the applier is still reading and cannot
poison bytes the client has been told it may recycle. The allocator cannot hand out an
overlapping range: `RingProducer::Reserve` bounds every reservation by
`FreeBytes()`/`TailForReclaim()`, `StageBytes` verifies that the resolved address equals the
address it wrote to (`:636-640`), and `ReclaimStagedBytes` only ever advances to the cursor
recorded for a record whose seq `retiredSeq` has passed (`:794-799`). Exhaustion mid-record is
one `ReclaimStagedBytes` retry and then a named
`Fatal{RingOverrun, "SEG_STAGE"}` printing the live capacity and free bytes (`:601-617`).

**The four rows nobody has consumed are honest about what they do.**
`SetShaderBuffers` (`:1282-1304`) and `SetStreamOutputTargets` (`:1306-1308`) run the full
layout and honesty pass and then `return false` rather than inventing a consumer — including
the `HostSpanCount is 0 or Count` rule (`:499-501`), which is the index-alignment invariant a
later phase would otherwise have to rediscover. `DrawVbo` (`:1455-1475`) validates the
conditional tail's presence and length against `layout` before it copies the span. And
`SetResidualValueState` (`:1343-1366`) — the row w1 was warned is the hardest — does the right
thing: `RequireDeclaredBlob`, then exact equality against `sizeof(ResidualValueBlock)` rather
than `>=` (so a peer built against an older ratchet is loud, not a short read of
`CapabilityBits`), then a `memcpy` into a local `ResidualValueBlock` before calling
`MGPipeApplySetResidualValueState(block)` — which is the only shape that applier's
`const ResidualValueBlock&` signature admits. `sizeof(ResidualValueBlock) ==
MGL_RESIDUAL_BLOCK_SIZE` is `static_assert`ed in the codec too (`:204`).

**The `MGPCaps` serializers refuse truncation and cannot be made to allocate an
attacker-chosen size.** `CapsCodec.cpp:105-114` `Reader::Raw` refuses before it reads;
`Str` (`:119-135`) weighs the length against `m_left` *before* sizing the string; every
length-prefixed array is gated on `RoomFor(elementBytes)` before the `resize`
(`:290-292`, `:314`, `:325`, `:433`), so the largest possible allocation is bounded by the
blob's own remaining bytes; every cell index is checked against `kFormatCells` (`:299`,
`:325`); a version mismatch and a table-dimension mismatch are named refusals (`:268-283`);
and both decoders end on `Finished()` (`:342`, `:456`), so trailing bytes are a refusal and
every failure path calls `out.Clear()` / `out = RendererInfo{}` first. Two hardening notes, not
findings: `static_cast<GLExtension>(value)` (`:444`) and `static_cast<VersionType>(type)`
(`:386`) accept any word off the wire without a range check, and `Writer` *appends* to `out`
rather than assigning it, so a reused vector silently concatenates (the `Finished()` check
turns that into a refusal rather than corruption).

**R-10's arithmetic is right, including the 4632.** Probe
`W1REV_TheLargestBoundedRecordIs4632Bytes` computes the layout directly:
`SetSamplerViews` with `Start = 0, Count = kMGPipeMaxTextureUnits (192)` gives
`TailBytes[0] = 192 × 24 = 4608` and `TotalBytes = 8 + 16 + 4608 = 4632` exactly; `SetShaderImages`
at 192 image units gives the same 4632. The 2 MiB cap checks out: `MOBILEGL_IPC_RING_MB`'s
8 MiB of SEG_CMD less the 4096-byte `RingControl` page (`Ring.h:101-102`,
`static_assert(sizeof(RingControl) == 4096)`) leaves a power-of-two ring of 4 MiB, and
`RingProducer::MaxRecordBytes() == m_capacity / 2` (`Ring.h:238`) is 2 MiB. 4632 / 2 MiB =
0.22 %, so the bounded catalogue is ~450× under the cap. Nothing in
`PipeWireCodec.{h,cpp}` derives a constant from `MOBILEGL_IPC_RING_MB`; every comparison goes
through `MaxRecordBytes()`/`Capacity()` at runtime (`:694`, `:702-703`), which is why s1's
correction cost no code. `CreateShaderState`'s record really is `8 + sizeof(MGPProgramDesc)`
regardless of archive size, because the archive goes through `StageBytes`.
The "both loud" claim is correct for the two rows w1 names — `Fatal{RingOverrun, "<call>"}`
with `std::abort()` at `:697-704`, not a log line — but see m6 for the two it does not name.

**The numbers re-ran clean.** All from `~/w7/p5-w1rev-gate.log` and `~/w7/p5-w1rev-itest.log`,
after a rebuild of all four directories at `52bf9acc` with `CTestTestfile.cmake` confirmed
present in each before any verdict:

| gate | result |
|---|---|
| G1 `symbol_report.py --threshold 0` vs `~/w7/p5-before-libMobileGL.so` | `.text 10 806 323 → 10 806 323 (+0, +0.000%)`; `.data/.bss/.rodata` all +0; **27 811 → 27 811 defined symbols, 0 added / 0 removed / 0 resized / 0 renamed**; 27 072 normalised names all unchanged |
| `nm --defined-only … \| grep -ic MG_Remote` | pull **0**, split **215** |
| `ctest -L unit`, four lanes × **three** runs at `-j 8` | pull 1785/1785 ×3, push 1785/1785 ×3, verify 1785/1785 ×3, split **1893/1893 ×3**. **No flake in twelve lane-runs**, fork cases included; w1's unnamed one-failure run did not reproduce |
| `ctest -N -R PipeWireCodec` | **51** cases |
| `ctest -L integration-gpu`, `MOBILEGL_TRANSPORT=monolith`, build-push | **1117/1117**; 1117 names, **0** not present at the P5 baseline |
| `gen_pipe.py --check` / `--self-test` | up to date; **9 negative-control trips, positive control OK** |
| `git diff 5175f9cc..HEAD -- MG_Pipe/PipeApply.{h,cpp}` | **empty** — confirmed, the decoder's "no semantics" rule holds as a number |
| files touched | 7, exactly as reported |

The generated `PipeWire.inc` change is minimal and correct: the hook is an inline variable and
its dispatch are both inside `#if MOBILEGL_BUILD_DISAGGREGATED` (`gen_pipe.py` diff), which is
what keeps G1 at `+0` — and the symbol report proves it rather than asserting it.
