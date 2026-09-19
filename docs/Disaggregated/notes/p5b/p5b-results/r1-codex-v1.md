# P5b r1 — server ownership, readback byte order, retirement back-pressure

Completed 2026-09-16. Branch `p5b/r1-codex`, isolated tree
`/home/swung/w7/p5b-r1-codex`, commit
`db8b3479520862cd47c546e96506b7332158abcf`.
Base is r2 `d50183cb`, so merge r2 first or merge this branch including its ancestry.
This implements ID-73 findings 1–4; it is not another adversarial review or a phase gate.

The inherited Windows tree `/mnt/c/Users/geekerwan/p5b-r1` was at `37fc4fdb`
with three uncommitted files: `PersistentMapTracker.cpp`, `.h`, and `BufferObject.cpp`.
Their complete file contents were copied into `/home/swung/w7/p5b-r1-codex-backup/`
before continuing in this new tree. The old tree and any old Claude process were left alone.

## Changes and evidence

| Finding | Implementation | Focused evidence |
|---|---|---|
| 1: coherent map bypass | Under split, retained backend `SyncPersistentMappedRange` calls return on the apply thread. `PushAllMembers`, `PushBlocksFor`, and the final `PushMappedSpanBlock` producer refuse server-role entry with `Fatal{RoleViolation,...}`, before reading map bytes or modifying frontend content serials. Server map-state teardown keeps membership valid but does not emit client live-write state. | Real GPU store readback test now holds a coherently mapped frontend object with bytes A and a server shadow with bytes B; Ensure uploads B. A live client session also asserts the producer's named apply-thread refusal. Separately, removing only client pre-verb push calls makes both coherent-pointer pixel cases fail while backend sync sites remain intact; restoring the calls makes both pass. |
| 2: framebuffer death on client | GLES' shared frontend-death callback forwards synchronously through the existing apply-thread control mailbox before any twin destruction or binding-cache changes. This covers every kind in that callback; framebuffer still has no invented wire opcode. | Native `glDeleteFramebuffers` interception observes one deletion on the apply thread and verifies that the native draw binding becomes zero. The previously failing `DirectGLES.P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact` now passes. |
| 3: PACK_SWAP_BYTES | After validating the tight reply, swap each requested component/packed word on the client. Apply to both direct and bounce paths, before scatter; byte types remain unchanged. `FLOAT_32_UNSIGNED_INT_24_8_REV` swaps two separate 32-bit words. | Live emit-table peer covers ushort, float, packed uint, mixed depth/stencil, and byte data with swap off/on and tight/scattered layouts; sentinel gaps are preserved. Actual GL test reads a non-symmetric ushort from RGBA16F with tight and skipped/strided pack destinations: both monolith and inproc pass. RGBA8 was deliberately avoided because expanded ushort components repeat the same byte and cannot expose a missing swap. |
| 4: applied before retired | Preserve r2's staging allocator wait and truthful wait counter unchanged. `ClientSession::EmitAndWait` now treats command-ring capacity exhaustion as back-pressure after the encoder's individual-record size check. It waits for enough retired bytes including the wrap pad, then retries the same record without changing its sequence. Shutdown/timeout gets a named `RetirementWaitFailed` instead of a silent drop or indefinite wait. | Two live-session cases hold the server between applied publication and retirement using r2's scheduling-only hook. The staging case fills 3/4 of 1 MiB twice and observes one wait. The command case positions a legal 393 KiB draw record so its successor needs a wrap pad, observes the producer parked, releases retirement, and verifies the next ordinal is exactly prior+1. |

The death callback uses the existing inproc control mailbox, not a new wire lifetime record.
P6's separate address space still needs the already-planned lifetime/control transport work.
Staging's r2 five-second shutdown-aware bound and once-per-blocked-allocation counter are intact.
The command wait uses the session's existing `kBarrierTimeoutMs` (30 seconds); individually
oversized records still fail at `PipeWireEncoder::EncodeRecord`.

## Executed acceptance

All paths below are under `/home/swung/w7/`.

- Split build completed, `CTestTestfile.cmake` verified by the build driver. Logs:
  `p5b-r1-codex-config-split.log`, `p5b-r1-codex-build-split.log`,
  `p5b-r1-codex-build-fixes.log`. Build concurrency was at most 8.
- Final targeted run: **10/10 passed, zero skips**, `MOBILEGL_TRANSPORT=inproc`,
  `MOBILEGL_ITEST_REQUIRE_GPU=1`, `p5b-r1-codex-final-targeted.log`.
  Six unit entries plus four integration entries: the six ownership/wait/swap controls,
  actual ushort swap pixels, FBO/RBO deletion pixels, and two coherent-pointer draw cases.
- Actual GL ushort swap: **1/1 monolith and 1/1 inproc**, zero skips,
  `p5b-r1-codex-swap-{monolith,inproc}.log`.
- Client-only push suppression: **two own pixel failures** (CTest exit 8), plus the existing
  map-arm metadata case's one intentional skip. The skip is not counted as a red.
  `p5b-r1-codex-coherent-red.log`; driver `p5b-r1-codex-coherent-control.sh`.
  It restores the source through an EXIT trap and rebuilds the restored integration binary.
  The restored two pixel cases are included in the final 10/10 run.
- `git diff --check` passed. Final worktree clean, submodule links off. Commit is a single
  line without trailers, author/committer `Swung0x48 <swung0x48@outlook.com>`.

The primary negative control removes **only** `PushPersistentMapsBeforeVerb()` from the
client pre-verb hooks. It does not set the block-size-zero knob, alter the backend's retained
sync sites, or remove the new server-role guard. It therefore proves client omission is no
longer repaired by server producer re-entry.

No new all-lane census, four-flavor unit run, G1 binary comparison, full integration lane,
device operation, push, or phase review was run here: the integrator explicitly owns the
combined quick gate. All production changes outside MG_Remote are inside
`#if MOBILEGL_BUILD_DISAGGREGATED`, so pull code is structurally unaffected; this is not
reported as a new measured G1 result. No `.def`, generated catalogue, contract, or opcode changed.

## Integration notes

1. Keep r2's StageAllocate implementation, its session doorbell hookup, telemetry semantics,
   and before-retire hook. This commit changes command wait policy in ClientSession only;
   it does not replace r2's codec implementation.
2. Five new unit entries; the existing staged-shadow unit is strengthened to use a persistent
   coherent mapping. One new ordinary integration scenario, registered for both backends;
   `integration-split` remains the same 22 selected entries. The new scenario is
   `PixelStoreSweepScenario.ReadPixelsSwapsUnsignedShortComponentsAndPreservesPackGaps`.
3. The known 27 inproc ordinary failures should no longer be relabeled wholesale as unchanged:
   the GLES FBO/RBO deletion case is now green on this isolated tree. The merged census must
   determine the new aggregate; no count was extrapolated from this single case.

Tools retained: `p5b-r1-codex-submodlinks.sh`, `p5b-r1-codex-build.sh`,
`p5b-r1-codex-coherent-control.sh`; external backup directory above. No integration worktree,
other package source/build directory, adb device, or remote ref was modified.
