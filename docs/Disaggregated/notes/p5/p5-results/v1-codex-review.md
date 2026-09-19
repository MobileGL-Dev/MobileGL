# P5 v1 adversarial review

Reviewed `p5/v1@571cbabb` against `5e5bf7b9`, plus the supplied joint-preview sources, seam diff, scripts and logs. Source paths below are relative to `MobileGL/` at `p5/v1`, unless explicitly marked otherwise. No builds, tests, perturbations, source edits or git-ref changes were performed. Confirmation perturbations below are proposals for an isolated verifier, not executed results.

## Findings, most severe first

### 1. blocker — ReadPixels can overwrite the server heap

**File:line:** `MG_Remote/Server/PipeApplier.cpp:187`, `:192`, `:195`.

**Ruling:** Contract §2, table 1 readback carrier; R-2 honest byte bounds; reduced-path ReadPixels correctness.

**Claim:** Allocating the client-declared `DstSize` without checking the backend's actual pack footprint permits an out-of-bounds write before the reply machinery can reject anything.

**Failure scenario:** Clear a framebuffer, set `GL_PACK_ROW_LENGTH=8`, `GL_PACK_SKIP_ROWS=1`, `GL_PACK_SKIP_PIXELS=2`, then read a 4×3 RGBA8 rectangle. Joint c1's `PackedReadbackBytes` returns 80 bytes, excluding the 40-byte initial skip (`p5-v1-joint/.../Client/EmitTables.cpp:213–240`). v1 allocates 80 bytes and calls the real backend, which honors the pack state (`DirectGLES.cpp:10879`); its last write ends at byte 120. This is a joint seam defect: c1 supplies an undersized extent, and v1 deliberately trusts it. Even without skips, copying the entire scratch extent back overwrites application row gaps with scratch contents instead of preserving them.

**How to confirm:** In an isolated ASan joint build, extend the clear/readpixels case with the RGBA8 pack parameters above and a sentinel-filled destination; check both the server allocation overflow and untouched gaps. Existing corroboration: `p5-v1-joint-inproc.log:10742–10768` records the analogous float-depth case, 6/12 pixels placed correctly, 14 gap elements overwritten, then SEGFAULT. Attribution of that particular crash to this allocation has not been established by a sanitizer trace.

### 2. blocker — A control request can be posted after the apply thread's final cancellation pass

**File:line:** `MG_Remote/Server/ServerLoop.cpp:310`, `:322`, `:428–446`.

**Ruling:** R-1 blocking lifecycle requests; table 3 bounded teardown.

**Claim:** The running check and mailbox publication are not synchronized with shutdown, allowing a caller to wait forever on a mailbox with no consumer.

**Failure scenario:** A caller observes `m_running == true` at line 428, then pauses before acquiring the caller/mailbox locks. Stop makes the apply thread perform its final pending-request check at lines 310–319 and exit. The caller resumes, sets `m_controlPending=true` and `m_controlFinished=false`, then waits solely for `m_controlFinished`. No thread can set it. A second caller waiting behind an earlier request can encounter the same window because the running check precedes `m_callerMutex`. Stop itself may complete within five seconds while the EGL caller remains hung.

**How to confirm:** Add a deterministic latch immediately after the successful running check; stop and join the loop from another thread, then release the latch. The request must return NOT_INITIALIZED; the current code waits indefinitely. Runtime reproduction is unverified.

### 3. major — Buffer staging is bypassed on creation and overwritten with the client's shadow at draw time

**File:line:** `MG_Backend/DirectGLES/Managers.cpp:2095–2096`, `:2117`, `:2928–2930`, `:3032`.

**Ruling:** R-2 honest inproc; R-11 / contract rule C's server-owned authoritative buffer shadow.

**Claim:** Adding `Adopt` at the two content callbacks does not make staged records authoritative on the real buffer draw path.

**Failure scenario:** A new VBO receives create/respecify/subdata records before its first draw. `Ops_H_Create` creates no twin, respecify returns without one, and subdata returns before copying any bytes. The draw creates the twin and reads `bufferObject->MappedData()`. For an existing twin, the draw also prefers that client pointer and republishes it into `resource->hostBytes`, replacing the server copy. A missing/corrupt staged upload can therefore still render the client's correct bytes, and SEG_STAGE poisoning cannot expose this bypass. `RequireCoverage` explicitly exempts the substituted base because it is not the store's vector.

**How to confirm:** Corrupt only the staged bytes of a VBO upload while leaving the frontend shadow intact, then run the first-draw triangle and a subsequent-update triangle under audit. A correct wire-authoritative path must reflect/reject the corruption; this path can recover the uncorrupted frontend bytes. Proposed perturbation, unverified.

### 4. major — Whole-store upload paths bypass the exact-coverage Fatal

**File:line:** `MG_Backend/DirectGLES/Managers.cpp:2980`, `:3052–3065`; `MG_Remote/Server/StagedShadow.h:71–75`.

**Ruling:** R-11 and the M-6 ruling: snapshot extent is exactly the staged range, never widened.

**Claim:** Checking pending-range drains does not protect the pool-reuse and whole-store respecification uploads that also read the server shadow.

**Failure scenario:** A handle-only ensure has a width-64 server shadow covering only [16,32), with a descriptor declaring content and storage needing recreation. `Adopt` allocated and zero-filled all 64 bytes, but marked only 16 covered. The ensure passes that base directly to a 64-byte `RespecifyStorageWith`, or the pool path passes it to whole-store `glBufferSubData`. Neither invokes `RequireCoverage`; unstaged zeroes become GPU contents rather than `Fatal{StageSnapshotTooNarrow}`. The two checked pending-range callers do not exhaust the shadow readers.

**How to confirm:** Drive handle-only ensure with the above sparse coverage and force storage recreation; intercept the upload or require the named Fatal. Current source instead uploads the whole extent. Runtime reachability of this handle-only setup in the current joint reduced lane is unverified.

### 5. major — Context destruction frees shadows while live twins retain their addresses

**File:line:** `MG_Backend/DirectGLES/Managers.cpp:2766`, `:2933`; `MG_Remote/Server/StagedShadow.h:90–94`.

**Ruling:** R-11 pointer lifetime; table 3 context-loss lifecycle.

**Claim:** `DropAll` frees shadow vectors without clearing the `hostBytes` pointers in the resource twins that survive context-generation changes.

**Failure scenario:** A twin retains an adopted server base. Context destruction calls `DropAll`; a later handle-only ensure finds that same twin. Its generation-reset block resets GL/storage fields but not `hostBytes`. `liveHostBase()` returns the freed allocation and a whole-store re-upload can read it. The code's lazy generation repair contradicts the new comment that the twins simply disappear with the shadows. This is a source-supported lifetime hole; an actual reduced-lane context-loss UAF remains unverified.

**How to confirm:** Under ASan, stage a buffer into an existing twin, destroy/recreate the native context, then call handle-only ensure before any new subdata. Assert that no freed base reaches the driver.

### 6. major — The seven backend-internal capability reads still use the client object

**File:line:** `MG_Remote/Server/ServerLoop.h:71–77`; `MG_Backend/DirectGLES/Utils.cpp:82`; `MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:819`.

**Ruling:** Table 3, `pActiveBackendObject` row.

**Claim:** The header describes passing the private server's format cache down, but none of those seven reads was converted.

**Failure scenario:** The private backend refreshes its format capabilities on make-current while the remote object's mirror still holds the previous snapshot. A server format/sample-count decision reads the remote object's cache rather than the cache just queried from its own driver. The shared inproc mirror can conceal the error once it catches up; an independent server cannot use that client object at all.

**How to confirm:** Read-only command: `wsl.exe -d Arch -- bash -lc "rg -n pActiveBackendObject ~/w7/p5-v1/MobileGL/MG_Backend/DirectGLES/{Utils.cpp,BackendObject_DirectGLES.cpp}"`. For behavioral confirmation, give the remote and private objects deliberately different format/sample masks and invoke the server helper; it follows the remote mask.

### 7. major — Make-current still rebinds and release-current still drops the native context

**File:line:** `MG_Remote/Server/ServerLoop.cpp:608–611`; `MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp:937–965`; `MG_Backend/DirectGLES/DirectGLES.cpp:11919–11973`.

**Ruling:** R-1 and v1 charter §2: apply thread holds the native context for life; one-off owner write/cache invalidation.

**Claim:** Moving the existing virtual call onto the apply thread does not implement the promised once-only native make-current behavior.

**Failure scenario:** Repeated client make-current calls reach `DirectGLES::MakeCurrent`, which unconditionally calls native EGL, rewrites the owner and invalidates caches. A client release-current request reaches `DirectGLES::ReleaseCurrent`, unbinds EGL and clears the owner. Subsequent server work before another bind encounters the very false context-ownership predicates the package claims to eliminate.

**How to confirm:** Instrument native `eglMakeCurrent` and the owner write, then issue bind, repeated bind, release, bind through the twelve-forwarder seam. The code has no already-current shortcut at this layer and forwards release unchanged. No runtime trace was collected.

### 8. major — The R-11 tests cannot detect removal of production copy/coverage wiring

**File:line:** `MG_Test/Wire/ServerLoopTest.cpp:509–558`, `:581–597`.

**Ruling:** R-16: a gate must fail when the production mechanism it claims to protect is removed.

**Claim:** Directly constructing `StagedShadowStore(true)` and calling its methods tests the container, not the buffer callbacks or the coverage call sites.

**Failure scenario:** Replace the split `MGL_SERVER_STAGED_ADOPT` macro in Managers.cpp with the original pointer expression, or remove all production `MGL_SERVER_STAGED_REQUIRE*` calls. The helper tests still copy/check their own manually constructed store. The audit test uses a texture upload and never establishes that the buffer backend retained the original bytes after poison. Its pre-poison memcmp reads the client's staging mapping, not bytes consumed by a backend.

**How to confirm:** Make either production-only perturbation above and run `ServerLoopTest.*:StagedShadowTest.*` in an isolated verifier. These tests have no dependency on the deleted buffer wiring. The supplied red-check instead edits StagedShadow.h, so its red result does not settle this gap.

The empty death regex at line 594 is not, by itself, an additional finding: lines 595–597 separately assert the exact Fatal string in the per-process log.

### 9. major — All ten red-check controls accept an unrelated test failure

**File:line:** `MG_Test/Wire/ServerLoopTest.cpp:387` and external `~/w7/p5-v1-redcheck.py:146–154`.

**Ruling:** R-16: every negative control asserts its own failure reason.

**Claim:** The red-check runner recognizes any nonzero ctest exit as success; it never asserts the expected assertion, Fatal, or timeout for an individual perturbation.

**Failure scenario:** A control produces an unrelated abort or fails fixture setup. The runner prints “RED as required” and can end with `P5_V1_REDCHECK_ALL_WENT_RED`. Printing lines containing “Failure” or “Timeout” is not an assertion. In particular, removing `m_drained` proves the tally exists, not that it is ordered before publication; renaming the affinity token proves logging text exists, not that the resolved mask is correct.

**How to confirm:** Make the runner return `(8, "unrelated fixture failure")` for one perturbed invocation while retaining green baseline results. It still accepts that control. This is a proposed script perturbation, not executed here.

The stored log does show the intended assertion locations for this historical run; that evidence does not make the runner enforce them on its next run.

### 10. major — The parking tests arm before the thread has actually parked

**File:line:** `MG_Remote/Server/ServerLoop.cpp:268–269`; `MG_Test/Wire/ServerLoopTest.cpp:230–234`, `:299–303`.

**Ruling:** R-16 behavioral arming; charter's parked-thread shutdown/wakeup check.

**Claim:** `ParkCount>0` observes entry toward Wait, not entry into a blocking park.

**Failure scenario:** The apply thread increments the counter, then sees the newly posted request during Wait's predicate/spin phase and never parks. Both tests nevertheless say they established a parked waiter. Replacing Wait with an immediate successful return makes the loop busy-spin while control and shutdown can still complete, so these tests can stay green with parking deleted.

**How to confirm:** Replace only the line-269 Wait expression with `true` and run `AControlRequestRunsOnTheApplyThreadAndUnparksIt` plus `StopKillsTheDoorbellJoinsAndTheThreadReallyExits`. They do not inspect `consumerParked` or an actual Park transition. Proposed perturbation, unverified.

### 11. minor — “RESOLVED mask” is not necessarily the effective affinity mask

**File:line:** `MG_Remote/Server/ServerLoop.cpp:118–119`, `:227–233`.

**Ruling:** Contract §5 affinity knob: log the resolved mask so ineffective pinning is visible.

**Claim:** Successful `sched_setaffinity` is followed by returning the requested mask, without querying the effective mask.

**Failure scenario:** A Linux cpuset permits only a subset of the requested CPUs. The kernel can accept the intersection and return success, while the log claims every requested CPU was applied. The unit test checks only “off” and a log token, so it cannot detect this discrepancy. Numeric overflow/sign handling and the 64-CPU topology limit also received no exercised coverage in the supplied tests.

**How to confirm:** In a cpuset-restricted process, request a mask containing one permitted and one excluded CPU; compare `sched_getaffinity` with the logged RESOLVED mask. Runtime behavior on this WSL host was not tested.

## Claims in v1-v1.md that the code or logs do not support

- **§1 presents the implementation as “The EGL migration (charter 2).”** The associated ServerLoop.cpp:608–611 comment claims subsequent same-surface binds cost nothing and the owner is written once, but DirectGLES.cpp:11919–11953 unconditionally performs the native bind, owner store and cache invalidations. Release-current also unbinds it. See finding 7.

- **§4: “Every control asserts its own string; no case constructs the state it observes.”** The red-check runner accepts any nonzero return at lines 146–154. StagedShadowTest constructs the copying mode, coverage and source poisoning itself; it does not drive Managers.cpp's production wiring. See findings 8–9.

- **§1: “The snapshot extent is exactly what the record declared, never widened.”** The coverage set has that intention, but whole-store uploads bypass it, and draw-time ensure substitutes the frontend shadow. A container property is not an end-to-end enforcement result. See findings 3–4.

- **§8: “428 passed, 185 skipped, 506 subprocess-aborted, 28 failed.”** Those four counts are individually present but total 1147, not 1149. The omitted outcomes are **two SEGFAULTs**: DirectVulkan.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters and DirectGLES.ForcedDepthStencilEmulation.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters. The final list identifies them at `p5-v1-joint-inproc.log:12173` and `:12371`. The complete census is **428 passed / 185 skipped / 506 aborted / 28 failed / 2 segfaulted**.

- **§8: “The 506 are the census's 65 Fatal{UnmigratedVerb} slots being reached, not a v1 defect.”** The ctest log records termination, not the Fatal reason for each abort. The report cites one separately probed example; this cannot establish the reason for all 506. The joint emit table itself asserts **64** unmigrated slots at `Client/EmitTables.cpp:477`, with two locally answered slots. Fatal-by-Fatal attribution remains unverified.

- **§8: barrier-off's “other eighteen ABORT with Fatal{BarrierViolation, \"DrawVbo\"}” and strict's “19 of 21 abort … Fatal{UnmigratedPipeInput, \"GetTextureContextId@Clear\"}.”** The supplied logs support 18 and 19 subprocess aborts respectively, but neither contains a `Fatal` line. The barrier-off log additionally supports one assertion failure and two skips. Exact Fatal names and originating thread require the corresponding MobileGL diagnostic logs.

- **§8: audit's “no 0xDD reached a draw.”** The normal and audit split logs each contain 19 passes and two skips. They establish matching test outcomes, not observation of every draw's upload source. The frontend-shadow bypass in finding 3 makes that stronger claim unproven.

- **§1 / tests: “the thread really PARKED first.”** ParkCount increments before Wait and before its spin/predicate checks. It is not evidence of an actual blocked waiter. See finding 10.

The 28 assertion failures are not class-C aborts. Their grouping is: 14 LayeredAttachmentShape, 3 packed-depth GetTexImage, 3 DirectVulkan primitives-generated/reroute, 5 framebuffer HandleRecycle, 1 texture-swizzle, 1 P4a framebuffer/renderbuffer deletion, and 1 persistent-map membership case. The last matches the already recorded ID-42 failure at scenario line 270. The other logs show wrong output/counters, but do not isolate v1 from c1 or shared backend behavior. The pack-layout crash is a concrete jointly defective c1/v1 seam; assigning all remaining failures to c1 is not justified by these logs. In particular, `p5-v1-joint-inproc.log:5255–5271` shows the post-deletion draw reading black, and `:10998–11024` shows both framebuffer attachment checks failing; neither supplies causal evidence identifying an owner.

## What I could not verify and why

- **Runtime reproductions and negative controls:** prohibited by the read-only/no-build/no-test instruction. All new perturbations above remain unexecuted. This review does not claim a sanitizer run, a successful reproducer, or fresh gate results.
- **A third instance of the exact “bookkeeping after appliedSeq” defect family:** not established. I traced the callback, SessionConsumer publication, retirement and control completion. The mailbox exit race is a separate synchronization defect; I am not relabeling it as an appliedSeq race. A verifier should pause immediately after AdvanceApplied and instrument subsequent writes to gPipeInputs/PipeStats/backend state while resuming the client.
- **All 37 applier bodies' transitive pointer lifetimes:** the five new verb sinks and twelve forwarders were inspected, as were the staging/decoder and buffer-shadow paths. I did not establish an exhaustive lifetime proof through every unchanged backend implementation. An audit/ASan record corpus must exercise each content consumer after its source is poisoned and reused.
- **Full EGL/app-exit behavior without explicit Destroy:** no such runtime was exercised. The supplied acceptance evidence covers fixture teardown, not every embedding application's exit sequence.
- **G1 binary identity:** source guards and the supplied report were inspected; no fresh binary comparison/build was performed. The extra destroy lookup in Managers.cpp belongs to the push-only handle section; it is not evidence of a pull-build regression by itself.
- **ID-41:** the added helper duplicates the range clamp, not the three-tier delivery ladder; the four actual drain callers remain two checked host-base callers and two MappedData callers. I found no basis to call this alone a forbidden reimplementation or to invent a fifth caller. Findings 3–4 concern bypassing authority/coverage, independently of that pin.
- **The 100× sweep:** the gate specifies `-j 8 --repeat until-fail:100`, and the log shows concurrent case processes. That supplies scheduling load, but no cases race control submission against shutdown, recreate shadow-owning contexts, or exercise production buffer-copy wiring. The sweep cannot settle those findings.
- **Merge completeness:** the ScopedApplierEntry bracket and weak-factory removal are present only in the supplied joint seam diff, not p5/v1. The standalone branch cannot be treated as the exact artifact that achieved the joint-preview passes.
