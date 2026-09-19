# Wave-1 adversarial review

Reviewed `~/w7/pipe` at `5e5bf7b9`, against `ff2994d9`. Paths below are relative to that tree. Findings are based on source inspection; confirmation perturbations are proposed, **not executed**.

## Findings

### 1. Empty staging segments can reject a blob that fits — major

**Location / ruling:** `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:709`, `:720`, `:725`, `:734`; R-10, contract §2’s whole-`SEG_STAGE` blob carrier.

**Claim:** Wrapping charges the unused suffix against capacity even when all previous staging bytes have retired, causing a false `RingOverrun`.

**Failure scenario:** In a 32 MiB stage, allocate and retire 8 MiB: head = tail = 8 MiB. Request a 28 MiB blob. The allocator adds a 24 MiB wrap skip, then tests 52 MiB against 32 MiB. Reclaim cannot change the already-empty state, so it aborts. Both blobs individually fit; no outstanding consumer prevents reuse.

**How to confirm:** Extend `PipeWireCodecTest.StagedBytesAreReclaimedOnlyBehindRetiredSeq` (`:1119`): after its 64-byte allocation has retired and in-flight bytes equal zero, call `StageBytes` with a source of exactly `Wire2::kStageBytes` bytes. It aborts instead of allocating offset zero.

### 2. Default reply geometry cannot hold its advertised readback — major

**Location / ruling:** `MobileGL/MG_Remote/Transport/ReplySlot.h:89`, `:150`, `:182`; `Transport/SessionRings.h:89`; contract §2, row 23: size slots from the largest scenario read.

**Claim:** Eight slots divide the 8 MiB segment into 1 MiB slots including headers, leaving less than the claimed 512×512 RGBA8 payload capacity.

**Failure scenario:** A 512×512 RGBA8 answer is 1,048,576 bytes; `MaxReplyBytes()` is 1,048,560. `Post` aborts. A full 640×480 RGBA8 read at OpenRA’s declared dimensions (`tools/trace_replay/trace_cases.json:22`) needs 1,228,800 bytes. Increasing CMD/STAGE knobs does not enlarge this pool.

**How to confirm:** Construct a default `SessionSegments` and its `ReplySlotPool`, then post a 512×512×4-byte vector. It takes the oversize-abort branch. Whether E2’s capture actually requests that full RGBA8 answer is **unverified**; the capacity defect itself follows directly from the arithmetic.

### 3. Blob validation accepts the wrong segment and bypasses staging audit — major

**Location / ruling:** `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:454`, `:460`, `:1085`, `:1102`; R-10, R-2.5, contract §2 Group A.

**Claim:** Input blob validation checks that a segment exists, but never requires the mandated `SEG_STAGE`, while audit poisoning ignores every other segment.

**Failure scenario:** Put valid sampler bytes into mapped `SEG_REPLY`, then send `CreateSamplerState.Parameters={Seg=3, Offset=0, Size=sizeof(SamplerParameters)}`. Encoder and decoder accept it; the sampler applier receives those bytes. With audit enabled, `NoteResolvedRun` returns without recording the run, so no bytes are poisoned and `PoisonedStageBytes()` remains zero. This accepts a forbidden carrier whose ownership and reuse are unrelated to stage retirement.

**How to confirm:** Modify the sampler round-trip fixture to install a reply-segment view containing the parameters and reference that view instead of staging them; enable audit and assert rejection. The current codec accepts the record.

### 4. Wire MapPersistent bypasses the adoption-tier refusal — major

**Location / ruling:** `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1296`; `MobileGL/MG_Pipe/PipeApply.cpp:2005`; `MobileGL/MG_Remote/Client/PersistentMapTracker.cpp:118`; R-6, contract §5’s T0/T1 “Fatal at use” requirement.

**Claim:** The codec’s unconditional decline skips the only adoption-tier validation used by the normal applier.

**Failure scenario:** Set split transport and `Ipc.AdoptTier=0` or `1`, then decode a `MapPersistent` record. The decoder posts DECLINED and returns success without calling `AdoptTierIsEmulate`; the required P11 refusal never occurs. ConfigLoader permits both values (`ConfigLoader.cpp:371`).

**How to confirm:** Set each forbidden tier in `PipeWireCodecTest.KReplySlotMapPersistentIsAConstantDecline` (`:502`). Its successful-decline assertions still pass instead of observing the required fatal diagnostic.

### 5. The new pad-bit regression test never sends a pad bit — major

**Location / ruling:** `MobileGL/MG_Test/Wire/SessionTest.cpp:1036`; `MobileGL/MG_Remote/Transport/Ring.cpp:178`, `:272`; R-16 and R-9/R-17.

**Claim:** `ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller` cannot detect removal of the consumer’s kind check because `Reserve` strips the bit first.

**Failure scenario:** The test requests `Reserve(7, kRecPad, 16)`, but the stored header has `flags=0`. Reverting `Pop` to its old flag-only skip still delivers that record, and every test assertion passes. The claimed regression input never reaches the code being protected.

**How to confirm:** Remove `&& header.kind == kRingPadRecordKind` from `RingConsumer::Pop`; run only `SessionTest.ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller`. It remains green.

This is a separate test defect from the excluded `RingTest.cpp:774` failure.

### 6. ABI sensitivity tests exercise a helper production no longer calls — major

**Location / ruling:** `MobileGL/MG_Test/Wire/SessionTest.cpp:775`; `MobileGL/MG_Remote/CapsCodec.cpp:472`; `MobileGL/MG_Test/Wire/PipeWireCodecTest.cpp:1370`; R-16, contract §1 ABI-agreement row.

**Claim:** The input-sensitivity gate tests `MixAbiFingerprint`, while both handshakes use the separately implemented `CapsAbiFingerprint`.

**Failure scenario:** Remove every ABI input from the production fingerprint and return a fixed nonzero value. The sensitivity test still exercises the untouched, unused helper. The only test calling the real fingerprint checks stability and nonzero, so it also passes. Peers with different structure sizes can consequently agree without either fingerprint test detecting the regression.

**How to confirm:** Replace `CapsAbiFingerprint()` with `return 1;`; run `SessionTest.TheAbiFingerprintChangesWhenAnyOfItsInputsDoes` and `PipeWireCodecTest.TheAbiFingerprintIsStableWithinABuildAndNotZero`. Both remain green.

### 7. The null-union regression test never reaches either handshake guard — major

**Location / ruling:** `MobileGL/MG_Test/Wire/SessionTest.cpp:931`; guards at `MobileGL/MG_Remote/Server/ServerSession.cpp:233` and `Client/ClientSession.cpp:198`; R-16.

**Claim:** `AVerifiableFrameCanCarryANullUnionPayload` proves a FlatBuffers property, but does not protect the production null checks it claims to protect.

**Failure scenario:** Delete the added `msg_as_Hello()==nullptr` and `msg_as_Welcome()==nullptr` guards. A null-union handshake can again dereference null, but this test still constructs and verifies its own envelope and observes its null member. It never calls `Accept` or `Start`.

**How to confirm:** Delete those two guard clauses and run only `SessionTest.AVerifiableFrameCanCarryANullUnionPayload`; it remains green.

### 8. CI negative controls still accept unrelated failures — major

**Location / ruling:** `.github/workflows/test.yml:1092`, `:1114`, `:1980`, `:1988`; R-16, E1/E3(a), and the split-retrace identity control.

**Claim:** These controls interpret any nonzero ctest exit as success without asserting their own failure reason.

**Failure scenario:** E1/E3(a) accept a timeout, setup abort, or unrelated assertion as evidence that the selected knob is load-bearing. The pull-library control likewise accepts a loader failure or empty test selection as evidence that transport-identity detection worked. Its `nm` check establishes library identity, not why replay failed. The local arming run also suppresses ctest failure with `|| true` and counts failed cases as “ran.”

**How to confirm:** In an isolated control-run harness, preserve a nonempty selection but make the selected test abort with `UNRELATED_CONTROL_FAILURE`. The integration control prints its success message. For the retrace control, use a regex matching no tests: `--no-tests=error` produces a nonzero exit that its final check accepts.

Earlier positive CI steps may independently fail the overall job; they do not make these negative-control verdicts valid.

### 9. gen_pipe’s self-test retains the any-SystemExit defect fixed in p1 — major

**Location / ruling:** `scripts/gen_pipe.py:1379`, `:1424`, `:1430`; R-16.

**Claim:** `expect_trip` accepts every `SystemExit`, including an unrelated diagnostic or exit code zero, without checking the control’s expected message.

**Failure scenario:** Replace the new unknown-flag control’s callback with a `SystemExit("unrelated parser failure")`. It still contributes one “tripped as expected” result, and the nine-control success condition is unchanged. The positive control does not check which guard rejected each malformed input. This is the same harness failure mode fixed in `gen_pipe_field_ownership.py`, left in the sibling generator.

**How to confirm:** Make that one callback replacement and run `gen_pipe.py --self-test`; it still reports nine trips and a successful positive control.

### 10. Session death controls accept crashes without their required diagnostics — major

**Location / ruling:** `MobileGL/MG_Test/Wire/SessionTest.cpp:581`, `:805`; R-16.

**Claim:** Both new death controls use an empty diagnostic regex, so they accept unrelated fatal termination.

**Failure scenario:** Replace the oversized-reply refusal or backwards-watermark refusal with an unconditional abort lacking the named diagnostic. The corresponding test remains green. A segmentation fault caused by a broken refusal path can also satisfy the test, although neither demonstrates the specified protocol-fault handling.

**How to confirm:** Replace the backwards-watermark diagnostic-and-abort block with bare `std::abort()`; `SessionTestDeath.AWatermarkThatMovesBackwardsIsFatalRatherThanIgnored` still passes.

## What I could not verify and why

- No builds, ctest runs, runtime probes, or mutations were performed: the review was read-only and the build directories belong to other jobs. All proposed red checks above remain unexecuted.
- End-to-end reduced-path effects, including OpenRA’s actual readback size, remain unverified. They require an isolated build after c1/v1 land; the reviewed head still has `ClientSession::EmitAndWait` and `PipeApplier::ApplyOne` stubs. I did not inspect the actively edited wave-2 worktrees.
- I ran the permitted `python3 scripts/gen_pipe.py --check`; it reported generated files up to date. This does not validate the negative-control harness.
- I excluded the named integration reds and previously reported pending work. There was no `c0-review-v1.md` in the supplied results directory; c0’s package report and integrator decisions were available.

## Claims in the package reports that the code does not support

- **s1-v1.md:134:** “1 MiB, which covers a 512×512 RGBA8 `ReadPixels`.” The slot includes a 16-byte header; `ReplySlot.h:150` subtracts it. Finding 2.
- **s1-v1.md:338:** “so the guard cannot be simplified away.” The named test never calls either guarded handshake (`SessionTest.cpp:931`). Finding 7.
- **t1-v1.md:419:** “so the control cannot pass for the wrong reason either.” The pull-retrace control checks only nonzero ctest status after its identity check (`test.yml:1988`). Finding 8.
- **w1-v1.md:341:** “The archive’s bound is `MOBILEGL_IPC_STAGE_MB` (32 MiB).” The effective allocation limit also depends on the retired head offset; even an empty stage rejects some smaller blobs (`PipeWireCodec.cpp:725`). Finding 1.
- **s1-v1.md §8.2** names `ARecordWearingThePadBitIsDeliveredRatherThanEatenAsAFiller` as its kind-check regression control. The test’s producer removes the bit before consumption (`SessionTest.cpp:1036`, `Ring.cpp:178`). Finding 5.

