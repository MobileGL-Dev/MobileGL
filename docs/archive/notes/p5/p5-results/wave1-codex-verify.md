# Wave-1 adversarial review — verification of the ten "how to confirm" perturbations

Every perturbation the cross-family review proposed but did not execute has now been executed.

- **Tree:** `~/w7/p5-verify`, branch `p5/verify`, from the integration head `4ba14fa4`
  (the review itself read `5e5bf7b9`; the delta is noted per finding where it matters).
- **Build:** `build-split` = `-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_DISAGGREGATED=ON
  -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON` over the preamble's `$COMMON`.
  Generator `Ninja`, compiler `/usr/sbin/clang++`, `build-split/CTestTestfile.cmake` present,
  1974 ctest entries under label `unit`.
- **Method:** one perturbation at a time; rebuild; run exactly the named test(s); revert with
  `git checkout -- <files>`; `git status --porcelain` empty before the next. Every perturbed run
  below is paired with the same test's **unperturbed** line, so no verdict rests on a build error.
- **Nothing is committed.** `git status` is empty; the worktree is left in place at `4ba14fa4`.

## Two build-harness notes, both of them the preamble's C-10 trap, both hit for real

1. The first `build-split` configure lost `$COMMON` in the argument path and silently produced a
   **GCC / Unix Makefiles** tree that then failed to compile. `CTestTestfile.cmake` existed the
   whole time, so the preamble's check alone would not have caught it; what caught it was reading
   `CMAKE_GENERATOR:INTERNAL` and `CMAKE_CXX_COMPILER` out of `CMakeCache.txt`. Every build below
   runs from `~/w7/p5-verify-build.sh` / `~/w7/p5-verify-bt.sh`, i.e. a **script file**, never an
   inline command string.
2. One early perturbation run reported `build rc=0` on a build that had emitted
   `9 errors generated. ninja: build stopped`, and the ctest that followed ran a **stale binary
   and passed**. `~/w7/p5-verify-bt.sh` now refuses to run any test when the build's exit status
   is non-zero. That result was discarded and re-run; the finding-4 numbers below are from the
   re-run.

---

## Pre-existing red at the integration head — needs an integrator ruling, not in the review

Before any perturbation, on the pristine tree:

```
$ cd ~/w7/p5-verify/build-split && ctest -L unit -j 14
99% tests passed, 1 tests failed out of 1974

The following tests FAILED:
	1947 - PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot (Failed) unit
```

```
../MobileGL/MG_Test/Wire/PipeWireCodecTest.cpp:1203: Failure
Expected equality of these values:
  MGPipeCallFlagsFor(MGPWireOp::ResourceCreate) & static_cast<Uint32>(kReplySlot)
    Which is: 16
  0u
    Which is: 0
```

Two landed commits disagree:

- `d55e3733 [Fix] (MG_Pipe): give the four acceptance calls the kReplySlot their applier entry
  points have always answered through` — `PipeCalls.def:100` now has
  `X(ResourceCreate, MGPResourceDesc, kScreen, kReplySlot)`.
- `79b78665 [Fix] (MG_Remote, Wire): ... answer a reply slot only for rows the catalogue flags
  kReplySlot` — wrote `ASSERT_EQ(... & kReplySlot, 0u)` for exactly that row.

`ReplySlot.h`'s own header rules for `d55e3733`'s side ("the four Bool acceptance entry points —
ResourceCreate, ResourceRespecify, ResourceSubData, SetTextureParams — say false (R-5)"), so the
**test** is the stale half. It is a catalogue-flag question either way, so it is the integrator's,
not a package's. **This is the unperturbed-tree baseline for every "the unit lane is otherwise
green" statement below.**

---

## 1. Empty staging segments can reject a blob that fits — **CONFIRMED**

**Perturbation.** Exactly the finding's: extend
`PipeWireCodecTest.StagedBytesAreReclaimedOnlyBehindRetiredSeq` so that, after its 64-byte
allocation has retired and `StagedBytesInFlight() == 0`, it calls `StageBytes` with a source of
exactly `Wire2::kStageBytes` bytes and asserts `Offset == 0`.
(`~/w7/p5-verify-f1-perturb.py`)

**Unperturbed**

```
1/1 Test #1944: PipeWireCodecTest.StagedBytesAreReclaimedOnlyBehindRetiredSeq ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

**Perturbed**

```
=== build target=PipeWireCodecTest rc=0
1/1 Test #1944: PipeWireCodecTest.StagedBytesAreReclaimedOnlyBehindRetiredSeq ...Subprocess aborted***Exception:   0.01 sec
0% tests passed, 1 tests failed out of 1
```

The test's own probe, printed immediately before the call:

```
FINDING1: inFlight=0 capacity=262144 requesting=262144
```

and the `FINDING1: ALLOCATED ...` line that follows the call never prints. The abort is inside the
allocator, not anywhere else:

```
$ gdb -batch -ex run -ex bt ./MobileGL/MG_Test/Wire/PipeWireCodecTest \
      --args ... --gtest_filter=PipeWireCodecTest.StagedBytesAreReclaimedOnlyBehindRetiredSeq
Program received signal SIGABRT, Aborted.
#2  0x00007ffff782567d in abort () from /usr/lib/libc.so.6
#3  0x0000555555658ccb in MobileGL::MG_Remote::Wire::PipeWireEncoder::StageAllocate(unsigned long) ()
#4  0x0000555555658e72 in MobileGL::MG_Remote::Wire::PipeWireEncoder::StageBytes(void const*, unsigned long) ()
#5  0x00005555555d63d3 in PipeWireCodecTest_StagedBytesAreReclaimedOnlyBehindRetiredSeq_Test::TestBody() ()
```

`need == m_stageCapacity`, so the first bound (`PipeWireCodec.cpp:709`,
`if (need > m_stageCapacity)`) does **not** fire; the abort is the second one at `:734`, the
`Fatal{RingOverrun}` whose own message reports `%llu bytes still in flight` — with **zero** in
flight.

**Discriminator (the part w1-v1.md:341 gets wrong).** The identical request on a *fresh* encoder
succeeds, so the rejection is the retired head offset and nothing else:

```
FINDING1B: fresh encoder ALLOCATED offset=0 size=262144
[       OK ] PipeWireCodecTest.Finding1FreshEncoderTakesTheWholeCapacity (0 ms)
```

Arithmetic, from `PipeWireCodec.cpp:720-728`: head = tail = 64, `offset = 64`,
`skip = capacity - 64`, and the test is `(head + skip + need) - tail <= capacity`, i.e.
`2*capacity - 64 <= capacity` — false at every occupancy, and `ReclaimStagedBytes()` cannot move an
already-empty tail, so the second attempt fails identically.

- **Fix shape:** when `m_stageHead == m_stageTail`, reset both cursors to zero before computing the
  wrap skip (or charge the skip against the freed suffix instead of against capacity).
- **Control (R-16):** the extension above, kept — "red once by doing X" = X is *removing the
  empty-stage reset*, which returns the case to today's `SIGABRT` in `StageAllocate` at
  `inFlight=0`.
- **Owner:** **w1** (`MG_Remote/Wire/PipeWireCodec.cpp`).
- **No integrator ruling needed** — no wire struct, no segment size, no catalogue flag.

---

## 2. Default reply geometry cannot hold its advertised readback — **CONFIRMED**, and the E2 half the review left "unverified" is now verified and worse

**(a) `MaxReplyBytes()` from the constants.** `SessionSegmentSizes::ReplyBytes = 8 MiB`
(`SessionRings.h:89`), `ReplySlotCount = kDefaultReplySlotCount = 8` (`ReplySlot.h:89`),
`slotBytes = 8388608 / 8 = 1048576`, `sizeof(ReplySlotHeader) == 16` (static_assert,
`ReplySlot.h:70`), so `MaxReplyBytes() = 1048576 - 16 = 1048560` (`ReplySlot.h:150`).
512×512×4 = **1048576**. Short by **16 bytes**.

**(b) The throwaway case** (not committed; `~/w7/p5-verify-f2-perturb.py`) builds a **default**
`SessionSegments` + `ReplySlotPool` and posts 512×512×4:

```
FINDING2: SessionSegmentSizes::ReplyBytes=8388608 slots=8 SlotBytes=1048576 sizeof(ReplySlotHeader)=16 MaxReplyBytes=1048560
FINDING2: 512x512 RGBA8 = 1048576 bytes -> DOES NOT FIT the slot (short by 16)
FINDING2: 640x480 RGBA8 (OpenRA's declared size) = 1228800 bytes -> DOES NOT FIT the slot (short by 180240)
[       OK ] SessionTestFinding2.DefaultReplyGeometryAgainstA512x512Rgba8Answer (4 ms)
```

The case's `EXPECT_DEATH(pool.Post(1, kReplyStatusOk, answer.data(), answer.size()), "")` passes,
so **`Post` takes the oversize-abort branch** (`ReplySlot.h:182`) — the review's predicted branch,
not a truncation and not a silent short write.

**(c) Does the OpenRA E2 retrace actually issue a full-surface `ReadPixels`? YES.** No LFS was
fetched: `tools/trace_replay/fixtures/openra.tgz` is the one hydrated fixture (7,232,355 bytes on
disk), and the system `apitrace 14.0-1` dumped it locally.

- The trace **itself** issues none. `apitrace dump --calls=0-31249 openra.trace` over all 31,249
  calls to the target: zero matches for `ReadPixels` / `ReadnPixels` / `GetTexImage`.
- The **harness** issues one, unconditionally. The snapshot the SSIM comparison needs comes from
  apitrace's `getDrawBufferImage`, which for an ES context forces `format = GL_RGBA;
  type = GL_UNSIGNED_BYTE` (`3rdparty/apitrace/retrace/glstate_images.cpp:1465-1469`) and then
  calls `glReadPixels(0, 0, width, height, format, type, image->pixels)` (`:1492`) — the whole
  surface, 4 bytes per pixel.
- That call is routed into MobileGL, not the host GL:
  `tools/trace_replay/apitrace_glproc_mobilegl.cpp:126-127` returns `MobileGLTraceReadPixels` for
  `glReadPixels`, which forwards to `LookupSymbol("glReadPixels")` in the loaded MobileGL library.
- The surface is 640×480: `trace_cases.json:22-23`, the trace's last `glViewport(0, 0, 640, 480)`,
  and `openra.0000031249.png` is `PNG image data, 640 x 480`.

So the E2 split retrace's single readback is **1,228,800 bytes**, 180,240 over the slot cap —
`Post` aborts, not marginally but by 17%. It does not fire *today* only because `EmitAndWait` /
`ApplyOne` are still stubs; it fires on the commit that lands c1.

- **Fix shape:** size `SEG_REPLY` from the largest scenario read that contract §2 row 23 already
  mandates — at these defaults, 8 slots × 2 MiB (16 MiB `ReplyBytes`), or keep 8 MiB and drop to
  4 slots, plus a client-side pre-emit check against `MaxReplyBytes()` that names the geometry.
- **Control (R-16):** a case that posts exactly `640*480*4` into the **default** pool and requires
  it to succeed — "red once by doing X" = X is *reverting `ReplyBytes` to 8 MiB at 8 slots*, which
  returns it to the `EXPECT_DEATH` above.
- **Owner:** **s1** (`Transport/ReplySlot.h`, `Transport/SessionRings.h`).
- **⚠ INTEGRATOR RULING REQUIRED — this changes a SEGMENT SIZE.** `SessionSegmentSizes::ReplyBytes`
  and/or `ReplySlotCount` are contract §2 row 23 and contract §5 numbers; and §5 has **no knob for
  SEG_REPLY at all** (`MOBILEGL_IPC_RING_MB` and `MOBILEGL_IPC_STAGE_MB` move SEG_CMD and
  SEG_STAGE only — the review is right that raising them does not enlarge this pool), so either the
  default moves or §5 gains a row. Both are **c0**'s file.
- **Also falsifies `s1-v1.md:134`** ("1 MiB, which covers a 512×512 RGBA8 `ReadPixels`") — by 16 bytes.

---

## 3. Blob validation accepts the wrong segment and bypasses staging audit — **CONFIRMED**

**Perturbation.** Exactly the finding's: the sampler round-trip fixture
(`PipeWireCodecTest.SamplerParametersCrossByteForByteIncludingBorderColorForm`) installs a mapped
`SEG_REPLY` view holding the `SamplerParameters` and the record references *that* view
(`Parameters.Seg = kSegReply`, `Offset = 0`, `Size = sizeof(SamplerParameters)`) instead of staging
them; `Decoder().SetAuditPoison(true)` arms rule C's mechanical control; the case then asserts
rejection. (`~/w7/p5-verify-f3-perturb.py`)

**Unperturbed**

```
1/1 Test #1937: PipeWireCodecTest.SamplerParametersCrossByteForByteIncludingBorderColorForm ...   Passed    0.00 sec
```

**Perturbed**

```
=== build target=PipeWireCodecTest rc=0
1/1 Test #1937: PipeWireCodecTest.SamplerParametersCrossByteForByteIncludingBorderColorForm ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
```

```
FINDING3: seg=3 accepted=1 poisoned=0
[       OK ] PipeWireCodecTest.SamplerParametersCrossByteForByteIncludingBorderColorForm (0 ms)
```

`seg=3` is `kSegReply` — a **server-owned** segment. `accepted=1` is `DecodeAndApply` returning
true, so the encoder *and* the decoder accepted the record and the sampler applier received those
bytes. `poisoned=0` with the audit **on** is the second half: `NoteResolvedRun`
(`PipeWireCodec.cpp:1109`) opens with `if (blob.Size == 0 || blob.Seg != kSegStage) { return; }`,
so the run was never recorded and `PoisonedStageBytes()` stayed zero. `RequireDeclaredBlob`
(`:454-467`) checks only that the blob *resolves inside some* segment — there is no `SEG_STAGE`
requirement anywhere on the path, though contract §2 table 1 row 17 assigns `CreateSamplerState`'s
bytes to `SEG_STAGE` and nowhere else.

- **Fix shape:** in `RequireDeclaredBlob`, require `blob.Seg == kSegStage` for every client→server
  content blob (contract §2 Groups A/B/C), `Fatal{ProtocolCorruption}` naming the segment
  otherwise — which also makes `NoteResolvedRun`'s filter unreachable rather than silently
  load-bearing.
- **Control (R-16):** the perturbed fixture above, kept as its own case asserting the refusal by
  message — "red once by doing X" = X is *removing the `kSegStage` requirement*, which returns it
  to `seg=3 accepted=1 poisoned=0`.
- **Owner:** **w1** (`MG_Remote/Wire/PipeWireCodec.cpp`).
- **No integrator ruling needed** for the minimal fix (§2 already mandates `SEG_STAGE`); it becomes
  one only if w1 wants a per-op *legal segment* column generated out of the catalogue.

---

## 4. Wire MapPersistent bypasses the adoption-tier refusal — **CONFIRMED** (both forbidden tiers)

**ConfigLoader, as asked.** `ConfigLoader.cpp:371` is
`ipc.AdoptTier = QueryEnvUint32("MOBILEGL_IPC_ADOPT_TIER", 2, 0, 2);` — **0, 1 and 2 are all
accepted**, exactly as contract §5 intends ("0 and 1 parse and are `Fatal` at use, naming P11").

**Is there any tier check on the wire decode path? No — not one.** `AdoptTier` appears in exactly
six places tree-wide, and none of them is the codec:

```
MobileGL/MG_Remote/Client/PersistentMapTracker.cpp:118:    Bool AdoptTierIsEmulate() {
MobileGL/MG_Remote/Client/PersistentMapTracker.h:137:    Bool AdoptTierIsEmulate();
MobileGL/Config.h:488:        Uint32 AdoptTier = 2;
MobileGL/MG_Pipe/PipeApply.cpp:2006:            MG_Remote::Client::AdoptTierIsEmulate()) {
MobileGL/ConfigLoader.cpp:371: ...
MobileGL/MG_Test/Buffer/SplitBufferTest.cpp: ... (the T0/T1 death test, on the applier helper)
```

`PipeApply.cpp:2005-2007` is the only production caller
(`if (MG_Config::Transport != Monolith && MG_Remote::Client::AdoptTierIsEmulate())`), and
`PipeWireCodec.cpp:1296` never reaches it — the decoder's `MapPersistent` arm is an unconditional
`PostReply(op, seq, ReplySink::kStatusDeclined, nullptr, 0); return true;`.

**Perturbation.** Exactly the finding's: set split transport and each forbidden tier inside
`PipeWireCodecTest.KReplySlotMapPersistentIsAConstantDecline`, RAII-restored.
(`~/w7/p5-verify-f4-perturb.py`)

**Unperturbed**

```
1/1 Test #1923: PipeWireCodecTest.KReplySlotMapPersistentIsAConstantDecline ...   Passed    0.00 sec
```

**Perturbed, `MG_Config::Transport = InProcess` + `Ipc.AdoptTier = 0`**

```
=== build target=PipeWireCodecTest rc=0
1/1 Test #1923: PipeWireCodecTest.KReplySlotMapPersistentIsAConstantDecline ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

**Perturbed, same but `Ipc.AdoptTier = 1`**

```
=== build target=PipeWireCodecTest rc=0
1/1 Test #1923: PipeWireCodecTest.KReplySlotMapPersistentIsAConstantDecline ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

Both forbidden tiers: the successful-decline assertions
(`Status == kStatusDeclined`, `Bytes.empty()`) still pass and no fatal occurs. Contract §5's
"0 and 1 parse and are `Fatal` at use" is not honoured on the wire path, only on the monolith
applier path.

- **Fix shape:** call `MG_Remote::Client::AdoptTierIsEmulate()` in the codec's `MapPersistent` arm
  *before* posting DECLINED, so the split path refuses T0/T1 at the same point the applier does.
- **Control (R-16):** a forked/death case that sets `Ipc.AdoptTier = 0` and requires the
  `MOBILEGL_IPC_ADOPT_TIER=0 names adoption tier T0, which P11 implements` diagnostic — "red once
  by doing X" = X is *removing that call*, which returns it to the green decline above.
- **Owner:** **w1** (the call site is `MG_Remote/Wire/PipeWireCodec.cpp:1296`);
  `AdoptTierIsEmulate` itself is **b1**'s and needs no change.
- **No integrator ruling needed** — no wire struct, no segment size, no catalogue flag.
  (`PersistentMapTracker.cpp:118`'s own comment already files the "move it to parse time" variant
  for the integrator, because `ConfigLoader.cpp` is c0's; the fix above does not need that.)

---

## 5. The new pad-bit regression test never sends a pad bit — **CONFIRMED** (but the head already carries a working replacement)

**Perturbation.** Exactly the finding's: delete `&& header.kind == kRingPadRecordKind` from
`RingConsumer::Pop` (`Ring.cpp:272`), leaving `if ((header.flags & kRecPad) != 0) {`.

**Unperturbed**

```
1/1 Test #1901: SessionTest.ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller ...   Passed    0.00 sec
```

**Perturbed**

```
=== build target=SessionTest rc=0
1/1 Test #1901: SessionTest.ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
```

The named control cannot go red for its own reason, exactly as claimed: `RingProducer::Reserve`
(`Ring.cpp:178`) stores `header.flags = flags & ~kRecPad`, so the test's `Reserve(7, kRecPad, 16)`
puts `flags = 0` on the wire and the consumer's pad arm is never entered either way.

**What the review could not know, because it read `5e5bf7b9`.** Head `4ba14fa4` landed
`ID-43 RingTest.TheTwoFlagSpacesAreDisjointByTranslation`, which stamps the header **outside**
`Reserve`. Under the *same* perturbation:

```
1/1 Test #1859: RingTest.TheTwoFlagSpacesAreDisjointByTranslation ...***Failed    0.00 sec
0% tests passed, 1 tests failed out of 1
ctest exit=8
```

So the defence *is* covered at head — by a different test than the one s1's report names.

- **Fix shape:** one line — `SessionTest.cpp:1036`'s producer must write the header itself (or
  call the same path ID-43's case uses) instead of going through `Reserve`, which strips the bit.
- **Control (R-16):** `RingTest.TheTwoFlagSpacesAreDisjointByTranslation` already is one — "red
  once by doing X" = X is the `Ring.cpp:272` deletion above, shown red here.
- **Owner:** **s1** (`MG_Test/Wire/SessionTest.cpp`). Cheapest correct action is to fix
  **`s1-v1.md §8.2`**, which names the defective case as its kind-check regression control, to name
  `RingTest.TheTwoFlagSpacesAreDisjointByTranslation` instead — and fix or delete the SessionTest case.
- **No integrator ruling needed.**

---

## 6. ABI sensitivity tests exercise a helper production no longer calls — **CONFIRMED**

**Perturbation.** Exactly the finding's: `CapsCodec.cpp:472`'s body replaced with
`Uint64 CapsAbiFingerprint() { return 1; }` — every ABI input removed, a fixed nonzero returned.

**Unperturbed**

```
1/1 Test #1894: SessionTest.TheAbiFingerprintChangesWhenAnyOfItsInputsDoes ...   Passed    0.00 sec
1/1 Test #1955: PipeWireCodecTest.TheAbiFingerprintIsStableWithinABuildAndNotZero ...   Passed    0.00 sec
```

**Perturbed**

```
=== build rc=0
1/1 Test #1894: SessionTest.TheAbiFingerprintChangesWhenAnyOfItsInputsDoes ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
1/1 Test #1955: PipeWireCodecTest.TheAbiFingerprintIsStableWithinABuildAndNotZero ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
```

Both remain green. **And so does the entire unit lane** — under this perturbation
`ctest -L unit -j 14` gives `99% tests passed, 1 tests failed out of 1974`, the one failure being
the pre-existing `NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot` above. Nothing in the tree
detects that the real handshake fingerprint stopped depending on any ABI input.

**The mechanism, sharper than the review states it.** `MixAbiFingerprint` has **zero production
call sites**; its only callers in the whole tree are `SessionTest.cpp:776-787`. The two handshakes
call the other function: `ServerSession.cpp:250` and `ClientSession.cpp:158`, both
`CapsAbiFingerprint()`.

- **Fix shape:** make `CapsAbiFingerprint()` *be* `MixAbiFingerprint(sizeof…, …, GIT_COMMIT_HASH_SHORT)`
  — one implementation, the sensitivity test then reaches production through it — or delete
  `MixAbiFingerprint` and give the sensitivity test injectable inputs on the real function.
- **Control (R-16):** the sensitivity case re-pointed at the production entry point — "red once by
  doing X" = X is exactly this perturbation, `return 1;`, which must then fail
  `SessionTest.TheAbiFingerprintChangesWhenAnyOfItsInputsDoes`.
- **Owner:** **s1** (`MG_Remote/CapsCodec.cpp`, `Transport/Ring.cpp` + `SessionRings.h`,
  `MG_Test/Wire/SessionTest.cpp` — all on s1's list).
- **No integrator ruling needed** — the fingerprint is a handshake value, not a wire struct field.

---

## 7. The null-union regression test never reaches either handshake guard — **CONFIRMED**

**Perturbation.** Exactly the finding's: delete `envelope->msg_as_Hello() == nullptr` from
`ServerSession.cpp:233` and `envelope->msg_as_Welcome() == nullptr` from `ClientSession.cpp:198`,
leaving each guard as `if (envelope == nullptr || envelope->msg_type() != …) {`.

**Unperturbed**

```
1/1 Test #1898: SessionTest.AVerifiableFrameCanCarryANullUnionPayload ...   Passed    0.00 sec
```

**Perturbed**

```
=== build target=SessionTest rc=0
1/1 Test #1898: SessionTest.AVerifiableFrameCanCarryANullUnionPayload ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
```

**And the whole unit lane with both guards gone:** `99% tests passed, 1 tests failed out of 1974`
— again only the pre-existing red. **Nothing in the tree protects those two guards.** The case
(`SessionTest.cpp:931`) constructs and verifies its own envelope and observes its own null member;
it never calls `ServerSession::Accept` or `ClientSession::Start`, so it proves a FlatBuffers
property and nothing about MobileGL.

- **Fix shape:** drive the null-union frame *through* `Accept` / `Start` and require
  `MOBILEGL_ERR_PROTOCOL_MISMATCH` plus the "not a verifiable Hello/Welcome" diagnostic — the
  existing case stays as the reachability premise, renamed to say so.
- **Control (R-16):** "red once by doing X" = X is deleting either `msg_as_*() == nullptr` clause,
  shown green here and required to be red after the fix.
- **Owner:** **s1** (`Session*`, `MG_Test/Wire/SessionTest.cpp`).
- **No integrator ruling needed.**
- **Also falsifies `s1-v1.md:338`** ("so the guard cannot be simplified away").

---

## 8. CI negative controls still accept unrelated failures — **CONFIRMED** (both halves)

CI cannot be run here, so each control's `run:` block was extracted to a local script with its
inputs stubbed, as instructed. The blocks are **byte-for-byte the workflow's**; only
`RUNNER_TEMP` / `GITHUB_WORKSPACE` / `${{ matrix.* }}` and the `ctest` binary were substituted.

### (a) E1 / E3(a) — `.github/workflows/test.yml:1092`, `:1114`

Harness `~/w7/p5-verify-f8-e1-harness.sh`, stub `~/w7/p5-verify-f8-stub/ctest`. Stub behaviour,
chosen to be the **best** case for the control: `ctest -N` reports a **non-empty** selection
(one matching test), and the control's own run fails with a reason that has nothing to do with the
knob.

```
### the stubbed world:
###   ctest -N   -> one matching test  (a NON-EMPTY selection, as the finding requires)
###   ctest run  -> exits 1 after printing UNRELATED_CONTROL_FAILURE

1/1 Test #1: DirectGLES.Split.TriangleScenario ...***Failed
split entries that actually ran: 1
1/1 Test #1: DirectGLES.Split.TriangleScenario ...***Failed
UNRELATED_CONTROL_FAILURE: the harness aborted in setup before the knob was read
negative control E1 (MOBILEGL_IPC_VERB_BARRIER=0) turned 1 selected entries red, as it must
1/1 Test #1: DirectGLES.Split.TriangleScenario ...***Failed
UNRELATED_CONTROL_FAILURE: the harness aborted in setup before the knob was read
negative control E3(a) (MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0) turned 1 selected entries red, as it must
HARNESS_RC=0 (the step would have SUCCEEDED)
HARNESS_EXIT=0
```

Both controls print their success message on a failure that is not theirs, and the step exits 0.
Two distinct defects, both as claimed:

- `run_control` treats *any* non-zero ctest exit as "the knob is load-bearing". The `matched >= 1`
  guard above it checks the selection is non-empty but says nothing about **why** the run failed.
- The arming run `ctest … || true` + the JUnit counter counts a case that **ran and FAILED** as
  "ran" (`armed: 1`), because the python only excludes `<skipped/>` and
  `status in ('notrun','disabled')`.

### (b) The pull-library retrace control — `.github/workflows/test.yml:1980`, `:1988`

Harness `~/w7/p5-verify-f8-retrace-harness.sh`. The identity check is exercised for real against a
compiled ELF `.so` defining no `MG_Remote` symbol, and the `ctest` is the **real** ctest in the
real `build-split`, with the finding's perturbation — a case/backend regex matching no tests.

```
--- the identity check PASSED: the stand-in defines no MG_Remote symbol
...
No tests were found!!!
Errors while running CTest
--- real ctest exit for a regex matching NO tests: 8
the pull library turned the split retrace red, as it must (ctest exit 8)
HARNESS_RC=0 (the step would have SUCCEEDED)
HARNESS_EXIT=0
```

`--no-tests=error` on an empty selection is exit 8, and the step's final
`if [ "${control_rc}" -eq 0 ]` accepts it. This control has **no `ctest -N` selection guard at all**
— unlike `run_control` — so an empty selection is the *only* thing it cannot distinguish from a
working transport-identity assertion. Its `nm` check establishes library identity and nothing about
why the replay failed, exactly as the review says.

- **Fix shape:** both controls must assert their **own** failure reason: grep the failing case's
  output for the knob's / the transport assertion's named diagnostic
  (`run_trace_case.cmake`'s transport-resolution message for the retrace control, and the
  BARRIER-PULLED / persistent-push wording for E1/E3(a)), and the retrace control additionally
  needs a `ctest -N … | grep -c '^ *Test *#'` selection guard like `run_control`'s. The arming
  counter must count **passed** cases, not "not skipped".
- **Control (R-16):** the two harnesses above, kept in-repo as a control-run smoke test — "red once
  by doing X" = X is exactly what is shown here (an `UNRELATED_CONTROL_FAILURE` with a non-empty
  selection; and a no-match regex), which must make the step exit non-zero after the fix.
- **Owner:** **t1** (`.github/workflows/*`).
- **No integrator ruling needed.**
- **Also falsifies `t1-v1.md:419`** ("so the control cannot pass for the wrong reason either").

---

## 9. gen_pipe's self-test retains the any-SystemExit defect fixed in p1 — **CONFIRMED**, including the exit-code-zero clause

**Perturbation.** Exactly the finding's: `scripts/gen_pipe.py:1425`'s unknown-flag control
callback replaced, `lambda: check_call_flags_are_known([flag_typo])` →
`lambda: sys.exit("unrelated parser failure")`.

**Unperturbed** (`python3 scripts/gen_pipe.py --self-test`)

```
gen_pipe: self-test call flag that is not an MGPipeCallFlags enumerator: tripped as expected (PipeCalls.def: Canned carries flag kHasBlobb, which is not an MGPipeCallFlags enumerator (kNeedsAck,)
gen_pipe: self-test: 9 negative-control trip(s), positive control OK
RC=0
```

**Perturbed**

```
gen_pipe: self-test call flag that is not an MGPipeCallFlags enumerator: tripped as expected (unrelated parser failure)
gen_pipe: self-test: 9 negative-control trip(s), positive control OK
RC=0
```

Nine trips, positive control OK, exit 0 — the control contributed its trip for a diagnostic that
has nothing to do with the guard it names, and the nine-control success condition is unchanged.

**The exit-code-zero half of the claim, also true.** Same line replaced with `lambda: sys.exit(0)`:

```
gen_pipe: self-test call flag that is not an MGPipeCallFlags enumerator: tripped as expected (0)
gen_pipe: self-test: 9 negative-control trip(s), positive control OK
```

`expect_trip` (`:1379`) catches bare `except SystemExit as trip` and never looks at the message or
the code, so a **successful** exit reads as a trip.

- **Fix shape:** give `expect_trip` a third parameter — a required substring of the guard's
  message — and assert `str(trip)` contains it (and that the code is not 0), which is precisely
  what `gen_pipe_field_ownership.py` already does.
- **Control (R-16):** the two replacements above, as a meta-control — "red once by doing X" = X is
  either `sys.exit("unrelated parser failure")` or `sys.exit(0)` on any one control, which must make
  `--self-test` exit non-zero after the fix.
- **Owner:** **p1** (`scripts/gen_pipe*.py`).
- **No integrator ruling needed.**

---

## 10. Session death controls accept crashes without their required diagnostics — **CONFIRMED** (both controls)

**Perturbation (the one the finding names).** The backwards-watermark diagnostic-and-abort block in
`AdvanceMonotonic` (`Ring.cpp:366-375`, which `Watermark::AdvanceApplied` at `:390` funnels into)
replaced by a bare `std::abort()`:

```
REPLACED:
                if (to < current) {
                    MGLOG_F("MGPipe: Fatal{ProtocolCorruption, \"watermark\"} %s moved backwards, "
                            ... );
                    std::abort();
                }
WITH:
                if (to < current) {
                    (void)name;
                    std::abort();
                }
```

**Unperturbed**

```
1/1 Test #1905: SessionTestDeath.AWatermarkThatMovesBackwardsIsFatalRatherThanIgnored ...   Passed    0.01 sec
```

**Perturbed**

```
=== build target=SessionTest rc=0
1/1 Test #1905: SessionTestDeath.AWatermarkThatMovesBackwardsIsFatalRatherThanIgnored ...   Passed    0.01 sec
100% tests passed, 0 tests failed out of 1
ctest exit=0
```

**The oversized-reply half of the claim, verified separately.** `ReplySlotPool::Post`'s oversize
refusal (`ReplySlot.h:182`) reduced to a bare `std::abort()` with its whole
`Fatal{ProtocolCorruption}` `WireLogError` deleted:

```
=== build target=SessionTest rc=0
1/1 Test #1904: SessionTestDeath.AReplyLargerThanItsSlotIsFatalRatherThanTruncated ...   Passed    0.01 sec
100% tests passed, 0 tests failed out of 1
```

Both `EXPECT_DEATH(..., "")` regexes are empty (`SessionTest.cpp:581`, `:805`), so each accepts any
fatal termination whatsoever — a refusal path that aborts for the wrong reason, or a segfault in a
broken one, is indistinguishable from the specified protocol-fault handling.

- **Fix shape:** replace each `""` with the diagnostic's distinctive wording —
  `"watermark\\\" appliedSeq moved backwards"` and
  `"reply pool: Fatal\\{ProtocolCorruption\\} - a .* byte answer"`.
- **Control (R-16):** "red once by doing X" = X is exactly the two bare-`std::abort()` replacements
  shown here, which must then fail their cases.
- **Owner:** **s1** (`MG_Test/Wire/SessionTest.cpp`; the two diagnostics live in
  `Transport/Ring.cpp` and `Transport/ReplySlot.h`, also s1's).
- **No integrator ruling needed.**

---

## Summary

| # | finding | verdict | owner | integrator ruling? |
|---|---|---|---|---|
| 1 | Empty staging segment rejects a blob that fits | **CONFIRMED** | w1 | no |
| 2 | Default reply geometry cannot hold its advertised readback | **CONFIRMED** (+ E2 consequence now verified: 1,228,800 B needed vs 1,048,560 B cap) | s1 (+ c0) | **YES — segment size** |
| 3 | Blob validation accepts the wrong segment, bypasses staging audit | **CONFIRMED** | w1 | no |
| 4 | Wire `MapPersistent` bypasses the adoption-tier refusal (T0 **and** T1) | **CONFIRMED** | w1 | no |
| 5 | Pad-bit regression test never sends a pad bit | **CONFIRMED** (head already carries a working replacement, `RingTest.TheTwoFlagSpacesAreDisjointByTranslation`) | s1 | no |
| 6 | ABI sensitivity tests exercise a helper with **zero** production callers | **CONFIRMED** (whole unit lane blind) | s1 | no |
| 7 | Null-union regression test never reaches either handshake guard | **CONFIRMED** (whole unit lane blind) | s1 | no |
| 8 | CI negative controls accept unrelated failures (E1/E3(a) **and** pull-retrace) | **CONFIRMED** | t1 | no |
| 9 | `gen_pipe` self-test accepts any `SystemExit`, including code 0 | **CONFIRMED** | p1 | no |
| 10 | Session death controls accept crashes without their diagnostics (both) | **CONFIRMED** | s1 | no |

**10 CONFIRMED / 0 REFUTED / 0 PARTIAL.**

Two items beyond the ten, both for the integrator:

- **Finding 2 is the only fix that cannot be taken inside a package** — it moves
  `SessionSegmentSizes::ReplyBytes` and/or `ReplySlotCount`, which are contract §2 row 23 numbers,
  and contract §5 has no `SEG_REPLY` knob to move instead. c0's file either way.
- **`PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot` is RED at `4ba14fa4`**
  (1973/1974 in `build-split`'s unit lane), from `d55e3733` giving `ResourceCreate` the
  `kReplySlot` flag while `79b78665` asserts it has none. A catalogue-flag conflict between two
  landed commits — the integrator's, not a package's, and `ReplySlot.h`'s R-5 comment says the
  test is the stale half.

## Provenance

Scripts and logs, all under `~/w7/` with the `p5-verify-` prefix:
`p5-verify-build.sh` (configure/build, with the `CMAKE_GENERATOR` + `CTestTestfile.cmake` checks),
`p5-verify-bt.sh` (build-then-test, refuses to test a failed build), `p5-verify-run.sh`,
`p5-verify-submodlinks.sh`, `p5-verify-f{1,1b,2,3,4,10,10b}-perturb.py`,
`p5-verify-f8-e1-harness.sh`, `p5-verify-f8-retrace-harness.sh`, `p5-verify-f8-stub/ctest`,
`p5-verify-cfg-split.log`, `p5-verify-build-split.log`, `p5-verify-final-build.log`.

Worktree left in place and clean: `~/w7/p5-verify` @ `4ba14fa4`, branch `p5/verify`,
`git status --porcelain` empty (with `p5-verify-submodlinks.sh off`). No commits. No LFS fetch.
No adb.
