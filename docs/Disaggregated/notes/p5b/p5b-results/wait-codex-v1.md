# P5b closing-review fixes: FenceWait and ReadPixels status

2026-09-16. Branch `p5b/wait-codex`, dedicated tree `/home/swung/w7/p5b-wait-codex`.
Base: `348d22a4b9c15cc571af840dedc4b4edc8c53dbb`.
Commit: `6388035f809861fd9b45cd1ba67c458a14b8b836`.

## Changes

Closing review Major #2: `FenceWait` applied/reply wait now allows `ceil(TimeoutNs / 1,000,000) + 30,000` milliseconds. Quotient/remainder rounding avoids overflow at `UINT64_MAX`. The finite 64-bit budget is consumed in transport chunks of at most `UINT32_MAX - 1`, avoiding both narrowing and the transport's forever sentinel, and avoiding overflow from one enormous chrono deadline. Shutdown still returns the original `ShutDown` result immediately; finite exhaustion still reports `Fatal{BarrierTimeout}` with the full effective budget. Only CPU `FenceWait` expands its budget. All other verb barriers and command-space retirement retain 30 seconds; `FenceWaitServer` with `GL_TIMEOUT_IGNORED` remains an ordinary bounded CPU submission.

Closing review Minor #3: both tight and scattered ReadPixels paths now abort `Fatal{ReplyStatusInvalid, "ReadPixels"}` for unknown reply status. Existing ERROR/DECLINED diagnostics remain intact. Correctly sized pixels with status 3 can no longer return as successful readback.

## Directed verification

Built only `RemoteClientTest` in this dedicated split/inproc tree with Clang Release and `-j4`. Selected **7**, passed **7**, failed **0**, skipped **0** (80 ms runtime).

New runtime controls use an installed test peer and a wrapper of the real transport doorbell to observe the deadline actually selected by `EmitAndWait`; no 30/60-second sleep or production test switch is needed. Coverage:

- `FenceWait` timeouts 0 ns, 1 ns, 60 seconds and `UINT64_MAX`: requested timeout is respected and huge waits use a finite transport chunk.
- `FenceWaitServer` + `GL_TIMEOUT_IGNORED`: ordinary 30-second budget.
- Huge `FenceWait` interrupted by a dead doorbell: DECLINED, no reply, prompt return.
- Status 3 with a complete readback payload: named abort on both tight and bounce paths.
- Existing complete readback, short tight reply, ERROR reply and short bounce reply controls remain green.

Command:

```bash
build-split/MobileGL/MG_Test/Wire/RemoteClientTest --gtest_filter='RemoteClientControls.FenceWaitBudgetHonorsGlTimeoutAndFiniteTransportChunks:RemoteClientControls.LongFenceWaitBudgetStillWakesOnShutdown:RemoteClientControls.UnknownReadPixelsReplyStatusRefusesTightAndBounce:RemoteClientControls.ReadPixelsPutsTheTightExtentOnTheWire:RemoteClientControls.ShortOkReadPixelsReplyRefusesByName:RemoteClientControls.ErrorReadPixelsReplyRefusesByName:RemoteClientControls.BounceReadPixelsShortReplyRefusesBeforeScatter'
```

Evidence: `/home/swung/w7/p5b-wait-{configure,build,focused}.log`.
`git diff --check` passed. Commit tree is clean; dependency links are off. Old r1 tree/commit and all existing evidence remain intact. No repeated phase gate, additional adversarial review, device operation or push was performed.

The host full gate remains pinned to `348d22a4`; this directed result is explicitly a later review-fix delta. APKs must be built from the final integrated head containing both closing-review fixes.
