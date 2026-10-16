# P5d round 3, package T - transport: apply loop idle poll, clock-free spin, park counters

Worktree `MobileGL-p5d-T`, branch `p5d-T`, base `85362a85`. Not built, not committed. Patch:
`scratchpad/p5d/patches/T.patch` (13 files, +489/-25).

## What changed, per file

### `MG_Remote/Server/ServerLoop.{h,cpp}` - item 1, the idle poll's mutex
- New `std::atomic<Bool> m_controlPosted` replaces a `std::condition_variable` of the same name
  that had **no waiter and no notifier anywhere in the tree** (`m_controlDone` carries the whole
  handshake). `ControlIsPending()` is now one acquire load, no lock, and moved to the public
  section (a test reads it - see red-once); `PumpControlRequest()` gained a fast negative on the
  same load before it touches `m_controlMutex`.
- Protocol: the poster stores the shadow **last of the four fields, with release**, inside the
  existing critical section in `RunOnApplyThread`; the pump clears it **under the lock, when it
  takes the request**; the C2 exit block clears it under the same lock as the `m_running` clear.
  The C2 critical section, the NOT_INITIALIZED answer at exit, publish-then-ring and the
  re-entrant inline arm are unchanged in meaning; `ready` now reads only atomics.
- Why: simpleperf at `56a77348` put `mutex::lock` 12.4 + `mutex::unlock` 13.6 + the two
  `pthread_mutex_*` halves at ~49% of `mgl-srv-apply`, with `ApplyThreadMain` the **direct** caller
  of 58-60% and 99.8% of `pthread_mutex_lock`-via-`mutex::lock`. Those are these two call sites,
  twice per idle iteration; most of `@plt` 6.1% goes with them.
- Item 2 (`DrainRing` on an empty ring): **audited, no code change**, finding written into the
  function. `ApplyOne` -> `RingConsumer::Pop`, whose first act is one acquire load of `cmdHead`
  against `m_localTail` (`Ring.cpp:222-232`); `applied == 0` skips the retire block. No mutex, no
  clock, no allocation.

### `MG_Remote/Transport/Doorbell.{h,cpp}` - item 3, the clock-free spin
- `Detail::g_spinItersPerUs` + `Detail::CalibrateSpinItersPerUs()`, and an inline
  `SpinItersPerUs()` that is a relaxed load and a predicted branch - deliberately **not** a
  function-local static, whose guard-variable load on every `Wait` is the cost this removes.
  `Wait()`'s spin phase is now `SpinItersPerUs() * spinUs` iterations of `ready()` + `CpuRelax()`
  and reads **no clock at all**; the deadline is computed once, at the top of the park phase, and
  `kWaitForever` reads no clock even there.
- Calibration: two passes of 8192 `load(acquire)` + `CpuRelax()` timed with `steady_clock`, faster
  pass wins (a preempted pass reads absurdly slow, and under-spinning costs a park), clamped to
  [4, 2000] iters/us with a 200 fallback for a clock too coarse to see the probe; a race between
  two calibrators stores two legitimate budgets.
- **Overshoot bound** (documented in the file): the old loop overshot by at most one batch of 64
  yields; this one by the calibration error - a DVFS drop or little-core migration stretches it by
  the frequency ratio (~3-4x here, so 50 us can reach ~200 us), a preemption adds one slice as
  before, and the probe omits the caller's `ready()`, so a real iteration is *dearer* than a
  probed one and the spin ends early by that factor. Not a correctness bound: `ready()` is
  re-tested every iteration and the deadline is still real-clock. Stated drift: the spin is no
  longer charged against `timeoutMs`, so a wait is bounded by `timeoutMs + budget` - 50 us against
  millisecond-scale timeouts, and erring long is the safe direction for a timeout.
- `Doorbell::ParkEntries()` + a private relaxed counter incremented at the one place a waiter
  stops spinning and blocks - unconditional (one relaxed add on a path about to enter a condvar
  or `poll()`), so a test can read it without `MOBILEGL_PIPE_STATS`.
- Why: `steady_clock::now()` 2.34 self / 5.91 incl + `__kernel_clock_gettime` 2.37 + `clock_gettime`
  1.94 = ~6.6% of the GL thread at 900+ waits/frame, for a duration that is a hint.
  `WaitForApplied`'s common path paid 2+ reads per wait; it now pays zero unless it parks.

### `MG_Remote/Transport/SessionRings.h`, `Transport/Ring.cpp` - item 4, client half
- `SessionProducer::Waits()` (relaxed atomic, incremented in `Park` *after* the two refusals that
  never reach the bell) and `Parks()` (delegates to the self bell's `ParkEntries()` - only
  `Doorbell::Wait` knows whether its budget ran out). Atomic because `ClientSession::Stop`'s drain
  waits through this producer from the tearing-down thread; it is never copied (checked).

### `MG_Util/Metrics/PipeStats.{h,cpp}`, `Client/EmitTables.cpp`, `Client/ClientSession.cpp`
- Four gauges, **all inside existing `#if MOBILEGL_PIPE_PUSH` blocks**: `ServerWaits`,
  `ServerParks`, `ClientWaits`, `ClientParks`. New `wait[srv= srvpark= cli= clipark=]` group on
  the one `MGPipe stats:` line (it closes the `emit[` bracket the ring gauges were riding), same
  four under long names in the JSON dump, run totals like the gauges above them. Both server
  numbers, not one: `srv` is `ParkCount()`'s long-standing meaning (an intention, counted on the
  way toward a park - ServerLoopTest's C10 note), `srvpark` is a real block.
- No new include edge into MG_Util: the existing `PublishGauge` pattern already crosses that way
  (`EmitTables.cpp:1000`). The client pair is published from `EmitPresent`; the **server pair by
  the apply thread from its own loop**, because CONTRACT-P5C rule E layer 2 forbids a GL-thread
  read of `ServerLoop`'s memory - PipeStats is the meeting point precisely because it is below
  both roles. Both publishes sit behind `PipeStats::Enabled()`.
- `ClientSession::LogWireLedger` also gained `cliwait=`/`clipark=`: that line is in **every** split
  private log, and the round-3 profile runs did not set `MOBILEGL_PIPE_STATS`.

## Red-once

| change | goes red as |
|---|---|
| shadow **set** side dropped | `ServerLoopTest.AControlRequestRunsOnTheApplyThreadAndUnparksIt` (existing) - the predicate sees nothing, the poster blocks, the case says so within 5 s |
| shadow **clear** side dropped | `ServerLoopTest.TheControlShadowIsClearedSoTheLoopParksAgainAfterARequest` (**new**) - `ready` stays true, the loop spins for ever and never stores `consumerParked`, so the second `WaitUntilTrulyParked()` times out; plus a direct `ControlIsPending()` read |
| spin budget zero / calibration broken | `InProcessTransportTest.DoorbellSpinsWithoutParkingWhenReadyFlipsInsideTheBudget` (**new**) - `ParkEntries()` moves although the condition flipped on the 8th probe |
| spin never ends / park unreachable | `InProcessTransportTest.DoorbellStillParksWhenTheSpinBudgetRunsOut` (**new**) - `ParkEntries()` does not move |
| `wait[]` renamed or dropped | `PipeStatsTest.SummaryLineCarriesEveryClassAndGate` (extended) |

Kept passing by inspection: `DoorbellWakesAParkedWaiter`, `DoorbellReturnsImmediatelyWhenAlreadyReady`,
`DoorbellTimesOutWhenNothingHappens` (the uncharged spin only makes elapsed larger than its `>= 20 ms`),
`ShutdownUnparksAWaiterWithNoDeadline` (fence protocol and `Dead()` unchanged), `RingTest`, `SessionTest`.

## G1 / monolith impact

**None.** Every touched file is MG_Remote (built only under `MOBILEGL_BUILD_DISAGGREGATED`,
`CMakeLists.txt:547-578`), a test, or - for `PipeStats.{h,cpp}`, which *is* in the pull build -
edited exclusively inside pre-existing `#if MOBILEGL_PIPE_PUSH` regions (`PipeStats.h:186-236`,
`PipeStats.cpp:484-545`, `:600-622`), so `MOBILEGL_PIPE_PUSH=OFF` compiles no added token and the
pull build's text is unmoved. No include-closure probe is touched.

## Risks

1. **The spin's duration bound is weaker** (see above): it burns CPU before a park, cannot change
   a result, and the [4, 2000] clamp plus probed-cheaper-than-real bounds both directions. The
   probe reads the clock twice per pass, once per process (2-30 us) - no steady-state path.
2. `m_controlPosted` is a second copy of one mailbox bit. Disagreement is a performance fault in
   both directions (a spin that never parks; a wakeup the doorbell's own re-test recovers), never
   a lost request - the pump re-checks `m_controlPending` under the lock and clears a stale shadow.
3. `wait[` closes `emit[` earlier than before, but `PipeStatsWindow::CounterOrAbsent` matches
   `" name="` / `"[name="`, so `fbe`/`sve`/`csom`/`mpr`/`rsp`/... are unaffected and the four new
   names collide with nothing (`cli=` cannot match `clipark=`).

## What I did not do

- Did not build, run, commit or push. No `Co-Authored-By` anywhere.
- Did not hoist `ServerSession::Consumer()` / `Applier()` out of `DrainRing` - two out-of-line
  calls per idle iteration, a small win, but caching them touches `ApplyThreadMain`'s opening
  lines, which package D owns. Flagged for round 4 / D; nothing of `OnApplyThread`,
  `m_applyThreadId` or those lines is touched here. Also left: the per-park `steady_clock::now()`
  under `kWaitForever` (one read per real park), to keep the park phase structurally identical.
- Did not update `docs/Disaggregated/ARCHITECTURE.md:623`, which enumerates the summary line's
  counters and now omits `wait[]` - left to the round's docs pass so four packages do not collide.
  Same for stale `Doorbell.h:NNN` references nearby: already drifted at `85362a85`.
- The brief's "visible to `ControlIsPending` **without the pump**" is not directly constructible:
  `m_callerMutex` allows one outstanding request and a live apply thread pumps every iteration, so
  no window exists in which a posted-but-unpumped request is observable by a third thread. The new
  test pins the clear side (which nothing else could see), the existing unpark test the set side.


---

# Fix round (after review)

Two reviews, no blockers, three distinct majors (two of them the same defect seen from two
sides) and five minors. Every one is addressed below; nothing was widened. Patch regenerated
from `MobileGL-p5d-T`: 16 files, +749/-40 (was 13 / +489/-25). Still not built, not committed.

## Majors

### M1 (review 1) - the clear side's red-once did not fire

**Upheld.** `PumpControlRequest` clears the shadow twice - at the take (`ServerLoop.cpp`, under
the lock, when the work is handed out) and defensively on the next pump when it finds the shadow
set with `m_controlPending` false. The old case asserted only the END STATE ("the shadow is clear
after the request was answered, and the loop parks again"), and the defensive clear supplies that
end state on the very next iteration, so deleting the take-clear left the case green. The
reviewer's trace is exact.

**Fix: observe the window, keep both clears.** `ServerLoopTest.TheControlShadowIsClearedSoTheLoop
ParksAgainAfterARequest` is replaced by
`ServerLoopTest.TheControlShadowIsClearedWhenThePumpTakesTheRequest`. The posted work now blocks
on a gate the test thread holds; the test waits until the work is really running and reads
`ControlIsPending()` in that window. There `m_controlPending` is still true and no pump is
running, so the ONLY thing that can make the accessor answer false is the clear at the take -
delete it and the read returns true on every run, deterministically. The end-state assertions
(answer returned, `WaitUntilTrulyParked()` again) are kept as the symptom an operator would see,
with their comment now saying which clear each one pins.

The defensive clear is kept: it is a genuine self-heal for a shadow that outlived its request,
and with the window observed directly it no longer masks anything.

### M2 (both reviews) - `Parks()` was not the subset of `Waits()` that everything claimed

**Upheld, in all three of its parts**, and the fix is one mechanism for all three.

1. *Counted per `Park()`, not per `Wait()`.* The increment sat inside the park-retry loop, so a
   wait that parked, woke on a remembered `Notify`, re-tested false and parked again counted
   twice - a routine path, exactly as review 2 describes.
2. *Counted for `Park(0)`.* When the remaining deadline rounds down to 0 ms the loop spins
   through the counting site without ever blocking.
3. *Counted off a shared bell.* `SessionProducer::Parks()` returned `m_selfBell->ParkEntries()`,
   and `ClientSession::Start` installs that same object as the encoder's stage-retirement bell
   (`PipeWireCodec::SetStageRetirementDoorbell`). Those waits never touch `m_waits`, and with
   `spinUs == 0` they now park unconditionally, so `clipark` could print larger than `cli` on
   any lane with stage retirement (the SmallRing / E3(e) lane asserts `stageReclaimWaits >= 1`).

**Fix.**
- `Doorbell::Wait` gained a fifth, defaulted parameter `std::atomic<std::uint64_t>* parkTally`.
  It is bumped at the same site as the bell's own counter, and both are now guarded by
  `chunkMs != 0 && !countedPark`, where `countedPark` is a local hoisted above the park loop. One
  park per `Wait`, and only when the call really blocks.
- `SessionProducer` owns `m_parks` and `SessionProducer::Park` passes `&m_parks`, so the client
  pair counts the same events on both sides. `Parks()` reads it; `ParkEntries()` is no longer
  consulted by any ledger.
- The server half got the same treatment even though its bell has no second waiter today:
  `ServerLoop::m_parkBlocks` (+ `ParkBlockCount()`, reset in m-3's per-session block beside
  `m_parks`) is passed into the loop's one `Wait`, so `srvpark <= srv` cannot be broken later by
  anything else waiting on the consumer doorbell (`SessionConsumer::WaitForWork` waits on that
  same bell).
- Comments corrected where they asserted the old, false relation: `Doorbell::ParkEntries()`'s
  block now says it is per BELL and names the two waiters that share the client's,
  `SessionRings.h`'s `Waits()`/`Parks()` block says why the tally is ours and not the bell's,
  and `PipeStats.h` says the subset relation holds only because both halves are counted by the
  same path.

**Red-once for the fix** (both new, both deterministic):
- `InProcessTransportTest.AWaitThatParksTwiceOnARememberedNotifyIsStillOnePark` - `Notify()` with
  nobody waiting, then one `Wait` whose condition never flips: the first `Park` returns at once on
  the remembered wakeup and the loop parks again. `ParkEntries()` must move by exactly 1. Revert
  `countedPark` and it moves by 2.
- `SessionTest.TheProducersParkLedgerCountsItsOwnWaitsAndNotTheBellsOtherWaiters` - a foreign
  `Wait` on `clientTransport->SelfDoorbell()` (spinUs 0, condition never true - the encoder's
  exact shape) must move neither `producer.Waits()` nor `producer.Parks()`; the producer's own
  20 ms `WaitForApplied` must move both by exactly one. Revert `Parks()` to `ParkEntries()` and
  the first `EXPECT_EQ` on `Parks()` goes red.

## Minors

### m1 - `spinUs == 0` silently changed meaning (both reviews)

**Upheld; stated, and the probe no longer runs on that path.** The old batched loop ran 63
iterations before its first clock read could end it; the new budget is `itersPerUs * spinUs`, so
zero means zero. That is now an explicit `spinUs == 0 ? 0ull : ...` short-circuit with a comment
naming the delta, both callers that pass 0 (PipeWireCodec's SEG_STAGE retirement wait, and any
lane with `MOBILEGL_IPC_SPIN_US=0`), and the reason it is deliberate rather than accidental. The
short-circuit also keeps a zero-spin caller from paying the one-time calibration probe - review
1's second half of the finding. `PipeWireCodec.cpp`'s `0` is left as it is: the retirement wait is
rare, already slow, and wants the park; changing it is a codec decision, not this package's.

### m2 - `ControlIsPending()` no longer means what its name says

**Upheld; documented rather than renamed.** The brief names the accessor, and three call sites'
worth of churn buys nothing, so the header block now states the predicate literally - "posted and
not yet taken" - says that it answers false for the whole duration of `work(user)` while
`m_controlPending` is still true, says it is a PARK PREDICATE and not a liveness query, and notes
that the only in-tree reader besides the test is the apply thread's own `ready` lambda, which by
construction cannot be inside that window. The new test in M1 is built on exactly that window, so
the documented meaning is now the tested one.

### m3 - the calibration clamp's two opposite readings (review 2)

**Upheld.** `perUs` truncates to 0 for any pass longer than 8.192 ms, i.e. the heavily preempted
probe, and that landed on the same `best == 0` arm as "no usable clock" and installed 200 iters/us
- 50x the floor - on the machine least able to afford it. `CalibrateSpinItersPerUs` now carries
`sawUsableClock`, set whenever `elapsedNs > 0`: measured-but-slow clamps UP to
`kMinSpinItersPerUs`, and `kFallbackSpinItersPerUs` is reserved for the case where no pass was
measurable at all. The `Doorbell.h` paragraph already described this rule ("a probe that was
preempted cannot produce a budget smaller than kMinSpinItersPerUs"); it is now true.

### m4 - `Doorbell.h:NNN` citations invalidated by this package's insert (review 2)

**Upheld - and the first report's "already drifted at 85362a85" was wrong.** Checked at baseline:
`139-178` really was the spin loop through the `Dead()` arm, `186-193` really was the `};` plus
NotifyIfParked's PRECONDITION, `211-221` really was CondVarDoorbell/`Kill`. The +60 lines moved all
three, and `check_doc_citations.py` cannot see it because the file only grew. Every in-tree
citation is re-pointed to SYMBOL NAMES rather than to new line numbers, so the next insert cannot
break them again: `ServerLoop.cpp` (x3), `ServerLoop.h`, `SessionRings.h` (x3), `EventRing.h`,
`ClientSession.cpp`, `ClientSession.h` (x2) and `CONTRACT-P5.md:355`. The checker only resolves
`path:line` pairs, so dropping the numbers leaves every gate green.

### m5 (review 2's note, not filed as a finding) - `srv=0` under `spawn`

Not a defect of this patch, but the new `PipeStats.h` comment asserted "PipeStats is the meeting
point precisely because it is below both", which is true in one transport mode. The comment now
says so: under `spawn` the server pair is published into the server process's PipeStats and the
client's line prints `srv=0 srvpark=0` for the whole run - the same property `rsp` already has -
and the round this pair was added for is `inproc`.

## Red-once table, updated

| change | goes red as |
|---|---|
| shadow **set** side dropped | `ServerLoopTest.AControlRequestRunsOnTheApplyThreadAndUnparksIt` (existing) |
| shadow **take-clear** dropped | `ServerLoopTest.TheControlShadowIsClearedWhenThePumpTakesTheRequest` (**new, rewritten**) - reads `ControlIsPending()` while the work is still running, where the defensive clear cannot cover for it |
| park counted per `Park` instead of per `Wait` | `InProcessTransportTest.AWaitThatParksTwiceOnARememberedNotifyIsStillOnePark` (**new**) |
| client parks read off the shared bell again | `SessionTest.TheProducersParkLedgerCountsItsOwnWaitsAndNotTheBellsOtherWaiters` (**new**) |
| spin budget zero / calibration broken | `InProcessTransportTest.DoorbellSpinsWithoutParkingWhenReadyFlipsInsideTheBudget` |
| spin never ends / park unreachable | `InProcessTransportTest.DoorbellStillParksWhenTheSpinBudgetRunsOut` |
| `wait[]` renamed or dropped | `PipeStatsTest.SummaryLineCarriesEveryClassAndGate` (extended) |

## G1 / monolith impact

**Still none.** The fix round touched `Doorbell.{h,cpp}`, `Ring.cpp`, `SessionRings.h`,
`EventRing.h`, `ServerLoop.{h,cpp}`, `ClientSession.{h,cpp}`, `CONTRACT-P5.md` (all MG_Remote or
a doc), three test files, and `PipeStats.h` - the last one inside the same pre-existing
`#if MOBILEGL_PIPE_PUSH` region as before, comment text only. No new include edge; the new `Wait`
parameter is defaulted, so the two call sites that do not pass a tally (`PipeWireCodec.cpp`,
`SessionConsumer::Park` in `Ring.cpp`) are unchanged.

## Not changed, with reasons

- **The defensive clear in `PumpControlRequest` stays.** Review 1 offered "drop it so the
  take-clear is load-bearing" as an alternative; dropping a self-heal to make a test work is the
  wrong trade when the test can observe the real window instead.
- **`ControlIsPending` is not renamed** (see m2).
- **`PipeWireCodec.cpp`'s `spinUs == 0` is not changed** (see m1).
- **`ARCHITECTURE.md:623`'s summary-line enumeration** still omits `wait[]` - left to the round's
  docs pass, as before, so four packages do not collide on one file. The `Doorbell.h` citations
  were different: those were made wrong BY this patch, so this patch fixes them.
- Did not build, run, commit or push. No attribution lines anywhere.
