# v1-review-v2 — adversarial review of package v1, round 2 (`p5/v1` `3fb97cc5..c9d84e33`)

Reviewer: Claude (Opus 5). Perturbations executed and reverted in `~/w7/p5-v1` and in v1's joint
scratch `~/w7/p5-v1-joint` (branch `p5/v1-joint2`, `ba0dcdb9` = `c9d84e33` + `p5/c1@f280baf2`, which
is `41992649` plus the docs-only merge `c3e736a3` — code-identical to the head v1 reports). Submodule
links toggled `off` before every git command and `on` before every build. Both worktrees are clean at
the end: `p5-v1` at `c9d84e33` with an empty `git status`, `p5-v1-joint` at `ba0dcdb9` with exactly the
three merge-time hunks it carried when I started (`Init.cpp` 19, `PipeApplier.cpp` 5, `ServerLoop.cpp` 26
lines, byte-identical diffstat before and after). Nothing committed. `~/w7/pipe/build-*`, `~/w7/p5-c1`
and every other `~/w7/p5-*` worktree were never written to.

**Counts over the 16 charter items: 7 CLOSED (1, 3, 8, 11, 12, 13, 16), 7 PARTIAL (2, 5, 6, 7, 9, 10, 14), 1 NOT CLOSED (4 - M-3 has no gate anywhere; ID-41 inside it is closed); item 15 is a measurement, reported below. Two sub-items inside a PARTIAL are themselves NOT CLOSED: M-6 (item 7) and the round-2 census correction (item 14). Plus 9 new findings - 5 major, 4 minor.**

Temp files kept because they are cited:
`~/w7/p5-v1rev2-{reprobe,probe4,census,failnames,c7-run,revert-r2-dgles,restore-joint}.sh`,
`~/w7/p5-v1rev2-{myperturb,jointperturb,c2-latch,c2-prefix,c9-pipeline,c7-instr,c7-tuple,item12,patcher}.py`,
and the logs `~/w7/p5-v1rev2-{gaterun,redcheck,myperturb,jointperturb,joint-inproc,joint-isplit,joint-isplit-audit,joint-isplit-barrier0,barrier0-direct}.log`,
`~/w7/p5-v1rev2-c7-*.out`, `~/w7/p5-v1rev2-probe-*.out`.

---

## The scoreboard

| # | item | verdict |
|---|---|---|
| 1 | ID-49 server half | **CLOSED** (the size half is ungated — see N-5) |
| 2 | M-1 / codex 8, the R-11 production gate | **PARTIAL** — the copy half is gated end-to-end, the coverage half is gated nowhere |
| 3 | M-2 / codex 3, the audit becomes real | **CLOSED** — and proved by a real red, which round 1 could not get |
| 4 | M-3 / codex 4, the whole-store readers | **NOT CLOSED as a gate** (code correct, nothing fails when it is deleted); ID-41 **CLOSED** |
| 5 | m-5 / codex 5, `DropAll` and the freed base | **PARTIAL** — container control only; no ASan-shaped control exists |
| 6 | M-4 + codex 11, the affinity gate | **PARTIAL** — `off` is now a real control; codex 11's own fix has none |
| 7 | M-6 / M-7 | M-7 **CLOSED**; M-6 **NOT CLOSED as a gate** |
| 8 | C2, the post-cancellation hang | **CLOSED**, two-directionally |
| 9 | C6, the seven capability reads | **PARTIAL** — the accessor is gated, the seven call sites are not |
| 10 | C7 / ID-54 | **PARTIAL** — "never unbind" measured true; "bind once per tuple" not demonstrated |
| 11 | C9, the red-check runner | **CLOSED** |
| 12 | C10, the park tests | **CLOSED** |
| 13 | item 12 / ID-55, `resource_flush_range` | **CLOSED** |
| 14 | the minors and the report corrections | minors CLOSED; **the census correction is wrong again** |
| 15 | the joint numbers | measured; one new major (N-1) and one refuted triage row (N-2) |
| 16 | G1 | **CLOSED** |

---

## Item by item

### 1. ID-49, the server half — CLOSED

`ServerVerbSink::OnReadPixels` (`PipeApplier.cpp:174`, the ID-49 block at `:205-267`) computes
`tight = w * h * GetInputBytesPerPixel(...)` (`:216`), forces neutral pack state through
`MGPipeApplySetPixelPackState` (`:248`), reads into the tight scratch, restores the saved state and
posts `tight` bytes (`:265`). `MGPPixelPackState` has exactly one member (`MGPipeTypes.h:944-947`), so
the save/restore round-trip is complete — not a partial restore.

**Executed, v1's own line:** the three `DepthReadbackHonoursThePackPixelStoreParameters` entries on the
joint under inproc — round 1 had two of them SEGFAULT —

```
$ cd ~/w7/p5-v1-joint/build-split && MOBILEGL_TRANSPORT=inproc ctest -R DepthReadbackHonoursThePackPixelStoreParameters
100% tests passed, 0 tests failed out of 3
```
and the full inproc census has **0 SEGFAULT** (`~/w7/p5-v1rev2-joint-inproc.log`).

**My perturbation (`~/w7/p5-v1rev2-jointperturb.py id49-neutral-pack-not-forced`, on the joint):** delete
the `MGPipeApplySetPixelPackState(neutralPack)` line only.

```
   integration-split: 100% tests passed, 0 tests failed out of 21
   depthpack: 0% tests passed, 3 tests failed out of 3 ; SEGFAULT lines=2
      DirectGLES.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters (SEGFAULT)
      DirectVulkan.DepthStencilReadbackMatrixScenario...                                            (Failed)
      DirectGLES.ForcedDepthStencilEmulation...DepthReadbackHonours...                              (SEGFAULT)
```

That is a genuine, executed red-once for the neutral-read half, end to end, reproducing exactly the
pre-fix shape. ID-49's server half is closed.

*But* (feeding N-5): the **tight-size** half has no control anywhere. Both
`id49-reply-uses-dstsize` (post `info.DstSize` instead of `tight`) and `id49-scratch-sized-by-dstsize`
(size the scratch from `info.DstSize`) leave the joint at 21/21, the depthpack family green, and v1's
19 unit cases green. That is not surprising — c1's `PackedReadbackBytes` is already the tight size, so
`tight == info.DstSize` on this head — but it means the `MGLOG_E_ONCE` mismatch branch (`:225`) and the
`bytesPerPixel == 0 → trust DstSize` fallback (`:216-219`) are unexercised dead paths, and a future c1
regression to a packed `DstSize` would be caught by nothing on v1's side.

### 2. M-1 / codex 8 — PARTIAL

**Half one, the copy, is now gated twice over.** The redcheck entry `m1-subdata-site-raw-offset` restores
`resource->hostBytes = static_cast<const Uint8*>(bytes) - offset;` at `Managers.cpp:2131-2133`:

```
=== m1-subdata-site-raw-offset (the exact rule-C violation R-11 exists to fix, at its production call site)
   RED for its own reason (rc=8): kept a pointer into SEG_STAGE
```

and — this is new, and stronger than anything v1 claims — the **same perturbation takes the joint's
reduced-path lane from 21/21 to 11/21** (`~/w7/p5-v1rev2-jointperturb.log`):

```
   integration-split: 52% tests passed, 10 tests failed out of 21
      DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels (Failed)
      ... TriangleScenario.AVboBackedTriangleReachesReadPixels (Failed)  [and 8 more]
```

So R-11's copy is load-bearing in production, not only in a container test. This is the measurement
round 1 said would settle M-1/M-2 and it comes out in v1's favour.

**Half two, the coverage clamp, is gated nowhere.** The charter's second perturbation —
neuter `RequireStagedCoverageForPendingRanges` (`Managers.cpp:1053-1065`, loop body → `continue`) —

```
$ python3 ~/w7/p5-v1rev2-myperturb.py require-pending-coverage-neutered
=== require-pending-coverage-neutered  (anchor count=1 in Managers.cpp)
   ctest rc=0
   *** NOTHING WENT RED ***
```
(over `ServerLoopTest|StagedShadowTest|StagedShadowProductionTest`, 19 cases), and on the joint
(`~/w7/p5-v1rev2-jointperturb.log`):
```
=== require-pending-coverage-neutered
   integration-split: 100% tests passed, 0 tests failed out of 21
   depthpack: 100% tests passed, 0 tests failed out of 3 ; SEGFAULT lines=0
```

**Answer to the charter's question:** restoring `hostBytes = raw - offset` goes red on
`StagedShadowProductionTest.SubDataThroughTheRealOpsTable…` with the string
`kept a pointer into SEG_STAGE`; neutering `RequireStagedCoverageForPendingRanges` goes red on
**nothing at all**, in the unit lane or on the joint. Round 1's M-1 is half closed.

### 3. M-2 / codex 3 — CLOSED

`liveHostBase()` (`Managers.cpp:2941-2960`) returns `resource->hostBytes` unconditionally under an
active transport; the `MappedData()` arm is `#if MOBILEGL_BUILD_DISAGGREGATED` + transport-guarded, so
monolith is untouched. **The audit is real, and I have the direct evidence round 1 lacked**: the
production copy's removal (item 2 above) turns the joint reduced-path lane red in 10 places. That is
exactly "the draw reads the shadow"; before M-2 the same perturbation was invisible (round 1's review
deleted the copy and all 1985 split unit cases stayed green).

Both gaps M-2 exposed are fixed and both are visible in the diff: `StagedShadowStore::HasShadow`
(`StagedShadow.h:136-146`) discriminating freed-on-context-loss from live-just-staged, and the lazy
twin minted at subdata (`Managers.cpp:2096-2110`, transport-guarded).

Monolith byte-identical, re-measured (`~/w7/p5-v1rev2-gaterun.log`):
`G2 pull-vs-push name diff lines: 0`; `G14 pull removed 0 / added 40`, `push removed 0 / added 40`;
`push integration-gpu: 100% tests passed, 0 tests failed out of 1128`;
`split/monolith integration-gpu: 100% tests passed, 0 tests failed out of 1149`.

### 4. M-3 / codex 4 — NOT CLOSED as a gate (the code is right; nothing can notice it going away)

Both whole-store readers do now go through the refusal: `Managers.cpp:3028-3029`
(`"pool_reuse_whole_store"`, before the whole-store `glBufferSubData`) and `:3104-3113`
(`"respecify_whole_store"`, called from both `RespecifyStorageWith` arms).

**My perturbation:** delete each one, then both.

```
=== m3-poolreuse-require-deleted   ctest rc=0   *** NOTHING WENT RED ***
=== m3-respecify-require-deleted   ctest rc=0   *** NOTHING WENT RED ***
=== m3-both-requires-deleted (joint)
   integration-split: 100% tests passed, 0 tests failed out of 21
   depthpack: 100% tests passed, 0 tests failed out of 3 ; SEGFAULT lines=0
```

v1-v2.md says "the production wiring is exercised end-to-end on the joint". It is not: the joint does
not move. The only red-once v1 offers for M-3 is `StagedShadowTest.ADrainOutsideTheStagedCoverageIsFatalByName`,
which perturbs `StagedShadowStore::RequireCoverage` — the class the case constructs. This is round 1's
M-1 shape recurring one level out.

**ID-41 — CLOSED.** Both G5 scripts pass in the worktree and `FlushPendingRangesFrom` hashes to the pin:

```
$ bash scripts/p3a_untouched_regions.sh ff2994d9 HEAD
172b022273db01b16e772d15b269ffcd797fe38c767f7354d83ce113a66040d0  FlushPendingRangesFrom
[p3a-untouched] the 11 pool / deferred-release / ring / flush-drain functions are byte-identical
$ bash scripts/p4a_untouched_regions.sh ff2994d9 HEAD
[p4a-untouched] the 17 ... regions are byte-identical between ff2994d9 and HEAD
```
Worth naming: `ShouldUseCaveatTextureFormat` is in p4a's pinned set and `Utils.cpp` is a file C6 edited
— the hash still matches, so C6 did not touch a pinned body.

### 5. m-5 / codex 5 — PARTIAL

`StagedShadowTest.HasShadowIsTrueAfterAdoptAndFalseAfterTheShadowIsDropped` asserts both directions and
is a real control for the *discriminator*. The **fix** — `if (!ServerStaged().HasShadow(resource)) resource->hostBytes = nullptr;`
at `Managers.cpp:2983-2985` — is gated by nothing:

```
=== m5-production-null-deleted   ctest rc=0   *** NOTHING WENT RED ***          (unit, 19 cases)
=== m5-production-null-deleted   integration-split: 100% ... out of 21          (joint)
```

The charter asks for "the ASan-shaped control". There is none: no ASan build, no context-loss case, and
the shipped control cannot reach the freed base. codex 5 is fixed in source and unverified in behaviour.

### 6. M-4 + codex 11 — PARTIAL

**The `off` half is now a real control.** Deleting `if (std::strcmp(raw, "off") == 0) return 0;`
(`ServerLoop.cpp:86`) — the exact perturbation that stayed green in round 1 —

```
=== affinity-off-branch-deleted
   ctest rc=8
   RED: ServerLoopTest.TheAffinityStringResolvesToAMaskAndTheMaskIsLogged (Failed)
      | ../MobileGL/MG_Test/Wire/ServerLoopTest.cpp:362: Failure
```
(`:362` is the new `EXPECT_EQ(log.find("is not `auto`, `off` or a number"), npos)`.) The explicit-mask
case passes and logs `RESOLVED mask 0x3`. Redcheck `resolved-mask-not-logged` is still red for its
own reason.

**codex 11's own fix is gated by nothing.** Replacing the `sched_getaffinity` read-back
(`ServerLoop.cpp:126-136`) with `return requested;`:

```
=== affinity-reports-requested   ctest rc=0   *** NOTHING WENT RED ***
```

`ServerLoopTest.cpp:373` says *"Red once by making ApplyAffinity return `requested`"*. That is false on
any box whose cpuset is not trimmed — which is every box the suite runs on — and the comment's next
clause admits as much. This is the R-16 clause about a red-once line that was never executed.

### 7. M-6 and M-7

**M-7 — CLOSED.** Making the `!m_running && backendAlive` arm an inline fallback again:

```
=== m7-fatal-becomes-fallback
   ctest rc=8
   RED: ServerLoopTest.AForwarderWithABackendButNoThreadIsFatalNotAnAppThreadFallback (Failed)
      | Expected: (log.find("Fatal{ApplyThreadNotRunning")) != (std::string::npos)
```
Which test dies: `AForwarderWithABackendButNoThreadIsFatalNotAnAppThreadFallback`, on the missing
`Fatal{ApplyThreadNotRunning` line in the log file (plus the `EXPECT_EXIT` signal assertion at `:774`).

**M-6 — NOT CLOSED as a gate.** The fix is the new `MG_Remote::Server::ServerLoopInstance().Stop();`
at `MG_Backend/Init.cpp:194`. Delete exactly that line:

```
=== m6-shutdown-stop-deleted  (anchor count=1 in Init.cpp)
   ctest rc=0
   *** NOTHING WENT RED ***
```

`ServerLoopTest.StopWithoutAStartedThreadStillDropsThePrivateBackend` (`:786`) gates
`ServerLoop::Stop()`'s `!joinable` arm — which round 1 already had (the diff does not touch it) — not
the new call. `ShutdownSplitRoles` still has no test of any kind.

### 8. C2 — CLOSED, two-directionally

The shipped deterministic case plus redcheck `c2-inline-after-stop` (red on
`did not return NOT_INITIALIZED`). Beyond that I **replayed the verifier's latch harness** on the fixed
tree (`~/w7/p5-v1rev2-c2-latch.py`, latch at the same program point: after the `OnApplyThread()` test,
before any lock):

```
[ RUN      ] ServerLoopVerifyC2.AControlRequestPostedAfterTheFinalCancellationPassReturnsNotInitialized
C2_ARM: the caller is past the running check and held
C2_STOP: Stop() returned in 0 ms; Running()=0
C2_RESULT: CALLER_RETURNED rc=1 (NOT_INITIALIZED=1)
[       OK ] ... (3 ms)
```
and the 19 shipped cases stay green with the latch compiled in but unarmed. Then, to prove the harness
is not vacuous, I restored the pre-fix shape (`~/w7/p5-v1rev2-c2-prefix.py`: the `m_running` check back
outside `m_controlMutex`, the clear back outside the cancellation-pass critical section) and ran the
same case:

```
C2_RESULT: CALLER_HUNG - no answer 30000 ms after the loop was joined
[  FAILED  ] ServerLoopVerifyC2....  (30002 ms)
```

Both directions executed; both trees reverted. v1's own reasoning for shipping only the deterministic
half (the fix works by mutual exclusion, so a latch cannot sit inside the new critical section without
starving the apply thread's exit) is correct — a latch at the *fixed* check point would deadlock
`Stop()` into `Fatal{ApplyThreadJoinTimeout}`. The harness above is the honest replacement and it is
not shipped; v1 should take it or its shape.

### 9. C6 — PARTIAL

The seven reads are converted through `ActiveBackendFormatCaps()` (`Utils.h:15-27`, `Utils.cpp:39-51`),
each `#if MOBILEGL_BUILD_DISAGGREGATED`-guarded; the pull build never sees the symbol:

```
$ nm --defined-only ~/w7/p5-v1/build-linux/libMobileGL.so | grep -c ActiveBackendFormatCaps
0
```
The two-object control passes and redcheck `c6-accessor-reads-global` is red on
`returned the process global's cache`.

**But the routing is ungated.** Reverting ONE of the seven (the `HasCachedFormatCapability` pair,
`Utils.cpp:89-112`) back to `pActiveBackendObject`:

```
=== c6-one-callsite-reverted   ctest rc=0   *** NOTHING WENT RED ***
```
The control gates the accessor, not the seven reads the ruling is about. That is codex 8's shape again,
transplanted: a test that would not notice the conversion being undone one site at a time. Mitigation:
G1 (`0 added / 0 removed`) and the `#else` arms make an accidental revert loud in review, not in CI.

C6 is also **not** the cause of any of the 23 inproc wrong answers — see item 15.

### 10. C7 / ID-54 — PARTIAL

`ClassifyEglMakeCurrent`'s release test is character-for-character `IsReleaseCurrentRequest`'s
(`BackendObject_DirectGLES.cpp:30-33`); the unit case
`MakeCurrentClassifiesRepeatAndReleaseWithoutRebinding` (`:829`) asserts all four arms of the pure
decision.

**The instrumented counts, measured (`~/w7/p5-v1rev2-c7-instr.py` + `c7-run.sh`, then reverted).** One
counter immediately before each native `eglMakeCurrent` (`DirectGLES.cpp:11925`, `:11964`) and one line
per `ApplyMakeCurrent` classification, same scenario, same binary:

| | monolith | inproc (split) |
|---|---|---|
| native `eglMakeCurrent` binds, per process | **2** | **2** |
| native `eglMakeCurrent(NO_CONTEXT)` releases | **1** | **0** |
| `ApplyMakeCurrent` actions | n/a | 1 × `NativeBind`, 1 × `ClientRelease`, **0 × `RepeatNoOp`** |

```
C7_ACTION pid=46515 0 tuple dpy=0x1 draw=0x1 read=0x1 ctx=0x1 | held=0 ...
C7_ACTION pid=46516 0 tuple dpy=0x1 draw=0x1 read=0x1 ctx=0x1 | held=0 ...   <- the pre-flight fork
C7_ACTION pid=46515 2 tuple dpy=0x1 draw=(nil) read=(nil) ctx=(nil) | held=1 ...
      2 C7_NATIVE_BIND pid=46515 ;  2 C7_NATIVE_BIND pid=46516 ;  0 C7_NATIVE_RELEASE
```

- **"Never unbind on a client release" — CONFIRMED.** The client's release-current is recorded and not
  forwarded: 1 native release under monolith, 0 under split, with a `ClientRelease` classified.
- **"Bind once per tuple" — NOT DEMONSTRATED.** The native bind count per process is **unchanged** (2
  under both transports). Only one of the two reaches `ServerMakeEGLCurrent`; the other comes from
  `DirectGLES`'s own internal `MakeCurrent()` calls (`DirectGLES.cpp:11829`, `:11847`), which the
  forwarder-level dedup cannot see. `RepeatNoOp` is classified **zero** times on the whole reduced-path
  lane, in all three scenario families. So the verifier's C7 count ("two binds in one process →
  `NATIVE_BIND #1`, `#2`") is still true at the DirectGLES layer after the fix, and ServerLoop.h:33-40's
  "makes DirectGLES.cpp's six cache invalidations a one-off startup cost" is not established.
- ID-54 names three controls ("2 identical binds → 1 native bind"; "release → 0 native releases";
  "bind after release, same tuple → 0 native binds"). **None of the three is shipped**, on `p5/v1` or
  on the joint, and `NativeBindCount()`/`ClientReleaseCount()` have zero readers anywhere (N-6).
- The production wiring is ungated: making `ApplyMakeCurrent` ignore the classification entirely
  (`c7-applymakecurrent-always-binds`) leaves all 19 cases green.
- See **N-3**: the cached tuple is never invalidated when the context or surface it names is destroyed.

### 11. C9 — CLOSED

Re-ran the shipped runner (`~/w7/p5-v1rev2-redcheck.log`): `C9 META-CONTROL … PASS`, **14 of 14**
perturbations `RED for its own reason`, `P5_V1_REDCHECK_ALL_WENT_RED`, exit 0.

The charter asks for the pipeline, not just `classify()`. `~/w7/p5-v1rev2-c9-pipeline.py` loads the
shipped runner as a module and stubs `build()`/`ctest()` so the real `main()` drives the real
patch/classify/report path:

```
A: every perturbed run returns an UNRELATED non-zero failure
main() returned 2, perturbed ctest calls=14
ALL_WENT_RED printed: False
   === FINDINGS
    - stamp-removed: red, but not for its own reason (expected /StampVerbBoundary did not run/)
    ... [all 14]
B: every perturbed run stays GREEN
main() returned 2 ; ALL_WENT_RED printed: False ; [all 14] "stayed GREEN with the implementation broken"
worktree after the two scenarios (empty == clean):  (clean)
```

Both halves of the charter's question answered: an unrelated failure does **not** count as red, and an
all-green sweep exits non-zero. Minor caveat in N-9.

### 12. C10 — CLOSED

`ServerFixture::WaitUntilTrulyParked()` polls `RingControl::consumerParked`, the flag `Doorbell::Wait`
sets inside its blocking section (`Doorbell.h:142`), and the three park-dependent cases plus the two new
ones use it. Redcheck `c10-wait-never-blocks` (the `:269` wait replaced by `true`):

```
=== c10-wait-never-blocks
   RED for its own reason (rc=8): never entered the blocking park
```

### 13. item 12 / ID-55 — CLOSED

**Executed red-once, precisely the one the charter names** (`~/w7/p5-v1rev2-item12.py`: drop the
`&& MG_Config::Transport == Monolith` guard at `PipeApply.cpp:1885-1886`, nothing else):

```
integration-split with the guard dropped: 67% tests passed, 7 tests failed out of 21
   DirectGLES.Split.PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw (Subprocess aborted)
   ... [all seven PersistentCoherentMapScenario entries, both lanes]
restored: build rc=0 ; 100% tests passed, 0 tests failed out of 21
```

21 → 14 → 21, and the seven are exactly the seven v1 names. Under monolith the guard is
`MG_Config::Transport == Monolith` with `Transport` an `inline constexpr TransportMode::Monolith`
(`Config.h:514`), so the expression folds to `&& true`; G1 reports `.text +0` and
`0 added / 0 removed / 0 resized / 0 renamed`, G2 reports 0 pull-vs-push name diff lines, and push
`integration-gpu` under monolith is 1128/1128. `MGPipeApplyResourceFlushRange` is unchanged under
monolith.

### 14. The minors and the report corrections

| | verdict |
|---|---|
| m-1 (present↔swap is a convention) | CLOSED — `PipeApplier.cpp:156-165` says so, and `Presents()` is exposed (`PipeApplier.h:128`) |
| m-2 (the death test names its mode) | CLOSED — `EXPECT_EXIT(..., KilledBySignal(SIGABRT), ".*")` + the exact Fatal grep; passes |
| m-3 (`Start` resets its diagnostics) | CLOSED — `./ServerLoopTest` with **no filter** is now `19 tests from 3 test suites ran. [ PASSED ] 19` (round 1 gave `DrainedRecords … 9 vs 8`) |
| m-4 (the doorbell Fatal text) | CLOSED — `ServerLoop.cpp:604-612` now says Notify suffices and Kill is the belt |
| m-6 (`AssertConsumerMaskIsHonest` re-runs) | present at `Init.cpp:224`, but see **N-8** — it cannot detect the thing its comment claims |
| m-7 (reply ordering documented) | CLOSED — documented against `ReplySlot.h:36` ("ORDERING") |

**Report corrections:**

* Round-1 census `428/185/506/28 + 2 SEGFAULT = 1149` — **correct**, re-derived from
  `~/w7/p5-v1-joint-inproc.log` (428 Passed, 185 Skipped, 508 `***Exception` of which 2 SEGFAULT, 28 Failed).
* "The 506 are class-C by one probed example, not per-abort" — correctly restated.
* "The `VERB_BARRIER=0` / `STRICT_ERRORS=1` failure NAMES come from the MobileGL log FILES" — **correct
  and reproduced**: with `MOBILEGL_LOG_FILE_PATH` set I read
  `[Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"}` verbatim
  (`~/w7/p5-v1rev2-barrier0-direct.log:78`). The `DirectGLES.Split.*` ctest entries still set no log
  path, so on the lane itself it remains unattributed (ID-53's plumbing, assigned to the joint-merge
  package by ID-56). See **N-1** for the much worse thing that has happened to that lane.
* "No 0xDD reached a draw is now ESTABLISHED" — **accepted**, and on better evidence than v1 gives: the
  claim rests on M-2 making the shadow the only base, and that is now a *measured* fact (removing the
  copy turns the lane red in 10 places), not an inference from a green lane.
* "The thread really PARKED first now rests on `consumerParked`" — correct (item 12).
* §8's `HandleRecycle.*` correction — the old sentence was indeed wrong, but the **replacement is also
  wrong**: see **N-2**.
* The round-2 census — **wrong, and in the flattering direction**: see **N-4**.

### 15. The joint numbers I measured

Joint = `p5/v1-joint2` `ba0dcdb9` (`c9d84e33` + `p5/c1@41992649`(+docs) + the two merge hunks), rebuilt
here, `BUILD_RC=0`.

| lane | result |
|---|---|
| `ctest -L integration-split` (inproc) | **21/21** — 19 pass + the 2 `TheMapLandsInTheArmItsLaneDeclares` design skips |
| `MOBILEGL_IPC_AUDIT=1 ctest -L integration-split` | **21/21** |
| `MOBILEGL_IPC_STRICT_ERRORS=1` | **19 of 21 red** (2 design skips) — the negative control still works |
| `MOBILEGL_IPC_VERB_BARRIER=0` | **21/21 "passed", ctest exit 0 — the negative control is GREEN.** See N-1 |
| inproc `ctest -L integration-gpu` census | **392 passed / 183 skipped / 551 subprocess-aborted / 23 failed / 0 SEGFAULT** (= 1149) |

`Fatal{BarrierViolation}` is readable — but only from a MobileGL log file the operator sets by hand.

**The 23, by family, with attribution I tested rather than assumed.** All 23 names match v1's table and
its per-family counts (12 Layered / 3 packed-depth GetTexImage / 3 PrimGen / 3 HandleRecycle / 1 swizzle
/ 1 P4aFinal).

Two ownership experiments, on the joint:

1. **C6 is not the cause.** Disabling `ActiveBackendFormatCaps`'s server arm (the obvious v1-shaped
   suspect for format/attachment answers) and rebuilding (`BUILD_RC=0`): all six probed entries still
   fail, identically. Reverted.
2. **v1's round 2 as a whole is not the cause.** I reverted *every* round-2 hunk in
   `Managers.cpp`, `Utils.{h,cpp}`, `BackendObject_DirectGLES.cpp` and `PipeApply.cpp` to their
   `3fb97cc5` text on the joint (`~/w7/p5-v1rev2-revert-r2-dgles.sh`), rebuilt (`BUILD_RC=0`) and re-ran:
   all six still fail identically under inproc, all six pass under monolith. (The same revert drops
   `integration-split` to 14/21, which independently confirms that v1's round 2 is what produced 21/21.)

So **v1's claim that none of the 23 is v1's holds** for the two families the charter asked me to test
(`P4aFinalFixScenario`, `HandleRecycle.Legacy`) and for four more besides. Their classification,
however, does not — see N-2. Against c1's own measured baseline on the same c1 head
(ID-56: 386/180/558/23/**2**), v1's round 2 is **+6 passed, −7 aborted, −2 segfault** — a small,
genuine improvement.

### 16. G1 — CLOSED

```
$ python3 scripts/symbol_report.py --before ~/w7/p5-before-libMobileGL.so \
      --after ~/w7/p5-v1/build-linux/libMobileGL.so --threshold 0
symbol-report: .text 10806611 -> 10806611 (+0, +0.000%)
symbol-report: 27814 -> 27814 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
```
`nm` pull `MG_Remote` = 0, split = 431, split `ServerLoop` = 27, pull `ActiveBackendFormatCaps` = 0.
Run after a fresh `cmake --build build-linux` (rc=0) inside the gate, so it is not a stale binary.

The other three self-tests re-measured in the same run (`~/w7/p5-v1rev2-gaterun.log`): unit
**1814/1814** in each of pull/push/verify; split **1994 with one red**
(`PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot`, c1's ID-31); flake sweep
`ctest -R 'ServerLoopTest|StagedShadowTest' -j 8 --repeat until-fail:100` → `100% tests passed, 0 tests
failed out of 18`; generators `--check` up to date; include-closure 0 problems.

---

## New findings, most severe first

### N-1 — major — `MOBILEGL_IPC_VERB_BARRIER=0`, R-1's negative control, is GREEN on the joint head: all 21 entries SKIP and ctest exits 0

**File:line:** `MobileGL/MG_IntegrationTest/CMakeLists.txt:1854-1857` (`MGL_ITEST_GLES_SPLIT_ENVIRONMENT`)
+ the harness's forked pre-flight EGL child (`Scenarios/../Harness/ScenarioFixture.h:111`).

**Claim:** with the barrier off, the pre-flight child now dies of
`Fatal{BarrierViolation, "SetTextureParams"}`; the harness reads a pre-flight child that died on
signal 6 as *"the EGL bring-up ABORTS on this platform"* and SKIPS every scenario; the entries carry
`SKIP_REGULAR_EXPRESSION`, so ctest records 21 skips and prints `100% tests passed`.

**Failure scenario:** exit gate E1 (`Fatal{BarrierViolation}` must be red under barrier-off) is retired
silently. Round 1's joint gave 19 of 21 red here. Anyone reading the lane sees a pass.

**How to confirm (executed):**
```
$ cd ~/w7/p5-v1-joint/build-split && MOBILEGL_IPC_VERB_BARRIER=0 ctest -L integration-split -j 4
100% tests passed, 0 tests failed out of 21        # every entry ***Skipped
$ MOBILEGL_IPC_VERB_BARRIER=0 MOBILEGL_ITEST_REQUIRE_GPU=1 ctest -L integration-split -j 4
0% tests passed, 21 tests failed out of 21         # red, but for "no usable GPU", not per-entry
$ ... MOBILEGL_LOG_FILE_PATH=... ./MobileGLIntegrationTest --gtest_filter=TriangleScenario.*
~/w7/p5-v1rev2-barrier0-direct.log:78: Fatal{BarrierViolation, "SetTextureParams"}
```
`MOBILEGL_IPC_STRICT_ERRORS=1` is unaffected (19 of 21 red) because it aborts *inside* the scenario,
after the pre-flight has succeeded.

**Owner:** not v1's code — the abort moved into the pre-flight because c1's round-2 R-17 routes
`SetTextureParams` through the wire. It belongs with ID-53's lane plumbing in the joint-merge package
(ID-56 already assigned the missing `MOBILEGL_LOG_FILE_PATH` there). Fix shape: the split lanes must set
`MOBILEGL_ITEST_REQUIRE_GPU=1` (or the pre-flight must not run the transport), and E1 must assert
`Fatal{BarrierViolation, "<slot>"}` from a per-entry log file.

**Severity:** major. It is a phase exit gate that currently cannot fail.

### N-2 — major — two of v1's six M-5 triage rows call a deterministic split defect a `-j` flake, on probes that ran zero tests

**File:line:** `~/w7/p5-v1-probe.sh:4-6` and its `primgen` / `handlerecyc` entries; v1-v2.md §"M-5 triage"
rows 3 and 4.

**Claim:** the probe exports `MOBILEGL_BACKEND_TYPE=DirectGLES` and an unprefixed gtest filter, so it
ran the plain `DirectGLES.PrimitivesGeneratedNoXfbScenario.*` and `DirectGLES.HandleRecycleScenario.*`
entries — which are **SKIPPED**, not passing. gtest exits 0 when everything skips, so `rc=0` was read as
"passes standalone". The entries that actually fail are `DirectVulkan.*`,
`DirectVulkan.PrimGenReroute.*`, `DirectGLES.HandleRecycle.Legacy.*`,
`DirectVulkan.HandleRecycle.{Legacy,AbaControl}.*` — different backends and different ctest lanes.

**How to confirm (executed):**
```
$ bash ~/w7/p5-v1rev2-reprobe.sh           # v1's own probe parameters
===== primgen rc=0 =====      [  SKIPPED ] ... ; [  PASSED  ] 0 tests.
===== handlerecyc rc=0 =====  [  SKIPPED ] ... ; [  PASSED  ] 0 tests.
$ bash ~/w7/p5-v1rev2-probe4.sh ~/w7/p5-v1-joint/build-split inproc     # the entries that DO fail
DirectGLES.HandleRecycle.Legacy...AFramebufferAtARecycledAddress...  [inproc]   0% tests passed, 1 failed
DirectVulkan.PrimGenReroute...CountsADrawMadeWithNoCaptureSpan       [inproc]   0% tests passed, 1 failed
$ bash ~/w7/p5-v1rev2-probe4.sh ~/w7/p5-v1-joint/build-split monolith
... all six: 100% tests passed
```
Six of the 23 (3 PrimGen + 3 HandleRecycle) are **deterministic, reproducible standalone under inproc,
green under monolith** — not flakes. The census log agrees: the DirectGLES entries v1 probed are
`***Skipped` in it.

**Consequence:** six wrong answers on the split path currently have no owner, because they were filed as
scheduling noise. This is R-16's third clause ("一条探针不得对着桩武装") applied to a triage probe.

**Severity:** major (as a report/triage defect; the underlying six are still not v1's — see item 15).

### N-3 — major — the C7 tuple cache is never invalidated when the context or surface it names is destroyed

**File:line:** `ServerLoop.h:195-202` (`m_haveCurrentTuple` at `:198`, `m_curDpy/Draw/Read/Ctx` at `:199-202`);
`ServerLoop.cpp:216` (the only clear, in `Start()`); `ServerLoop.cpp:798-813` (`ServerReleaseEGLResources`,
which runs `backend->ReleaseEGLResources()` → `DestroyEGLContext` and clears nothing);
`ServerLoop.cpp:264-290` (`ApplyMakeCurrent`).

**Claim:** `ApplyMakeCurrent` keys "already bound" on the *client's* `(dpy, draw, read, ctx)` handle
values and keeps that key across context and surface destruction.

**Failure scenario:** client binds (D,S,S,C) → `NativeBind`, tuple cached. Client destroys the context
and/or surface (`ServerReleaseEGLResources` / `ServerReleaseEGLSurface`) without the session being
restarted, then creates new ones. EGL handle values are routinely recycled — on this host every one of
them is literally `0x1` (`~/w7/p5-v1rev2-c7-clear.out`) — so the next make-current compares equal,
classifies `RepeatNoOp`, performs **no** native `eglMakeCurrent` and **skips the R-12 caps republish**.
Every later GL call on the apply thread then runs with no context current, silently, and
`ServerMakeEGLCurrent` returns `true`.

**How to confirm:** add `m_haveCurrentTuple = false` clearing to `ServerReleaseEGLResources` and to
`ServerReleaseEGLSurface` (when the surface matches `m_curDraw`/`m_curRead`), and to the backend-destroy
path in `ApplyThreadMain`; write a case that binds, releases resources, rebinds the same handle values
and asserts `NativeBindCount() == 2`. Today that case would read 1. Not reachable on the P5 reduced-path
lane (measured: exactly one make-current per process, item 10), so it is a latent hazard for the exit-gate
traces, OpenRA and P6 rather than a live P5 red.

**Severity:** major (silent wrong context, no diagnostic).

### N-4 — major — the round-2 inproc census in v1-v2.md is arithmetically impossible and overstates the result

**File:line:** `v1-v2.md` §"M-5 triage", first paragraph.

**Claim:** "575 passed / 183 skipped / 551 subprocess-aborted / 23 failed / 0 SEGFAULT" sums to **1332**,
not 1149. 575 is ctest's `passed + skipped` figure (1149 − 574), so the 183 skips are counted twice.

**How to confirm (executed, on v1's own log and on my re-run, byte-identical at 3071 lines):**
```
$ bash ~/w7/p5-v1rev2-census.sh ~/w7/p5-v1-joint2-inproc.log
Passed: 392 ; Skipped: 183 ; Aborted(***Exception): 551 ; Failed: 23 ; SEGFAULT: 0   (= 1149)
```

**Consequence:** the report presents "round 1 was 428/185/506/28/2" beside "575", i.e. as a jump of +147
passing tests. The honest comparison is **428 → 392: 36 fewer tests pass on the round-2 joint than on
the round-1 joint**, with aborts up 506 → 551. That delta is c1's R-17 routing (against c1's own
baseline on the same c1 head, 386 passed, v1 is +6), but it is the opposite sign from what the report
tells the integrator, and it is the second round in which this census has been mis-stated.

**Severity:** major as a report defect; the underlying tree is fine.

### N-5 — major — seven of the round's own fixes have no control that fails when the production wiring is removed

R-16's fourth clause ("每条门在报告里都要带一行'我把它弄红过一次，方式是 X'") — each of these ships a
red-once line that either names a container test or names a perturbation that does not go red here.
All executed with `~/w7/p5-v1rev2-myperturb.py` / `jointperturb.py`; each left the 19-case unit surface
AND the joint's 21/21 untouched:

| fix | file:line deleted | what went red |
|---|---|---|
| M-1's coverage half | `Managers.cpp:1058-1063` (loop body → `continue`) | nothing |
| M-3 pool-reuse | `Managers.cpp:3028-3029` | nothing |
| M-3 respecify | `Managers.cpp:3104-3113` | nothing |
| m-5's null | `Managers.cpp:2983-2985` | nothing |
| M-6's call | `Init.cpp:194` | nothing |
| codex 11's read-back | `ServerLoop.cpp:126-136` → `return requested;` | nothing |
| C7's classification | `ServerLoop.cpp:264` → always `NativeBind` | nothing |
| C6's routing (1 of 7 sites) | `Utils.cpp:89-112` | nothing |
| ID-49's tight size | `PipeApplier.cpp:216`/`:265` → `info.DstSize` | nothing (unit **and** joint) |

**Fix shape:** the ones that are cheap to gate today are M-6 (a case that calls `ShutdownSplitRoles`
after a forced `ClientSession::Start` failure and asserts `ServerLoop::Backend() == nullptr`), C7 (assert
`NativeBindCount()`/`ClientReleaseCount()` through the forwarders — the counters already exist and have
no readers), and C6 (assert each of the seven helpers' answer with two objects installed, not just the
accessor's address). M-3, m-5 and codex 11 need a sparse-shadow case, an ASan context-loss case and a
cpuset-restricted child respectively, and should be booked with owners rather than claimed.

**Severity:** major in aggregate — three of the round's charter items (M-3, m-5, M-6) rest on nothing
executable, and the report states red-once lines for two of them that do not hold.

### N-6 — minor — `NativeBindCount()` / `ClientReleaseCount()` have zero readers in the whole tree

`ServerLoop.h:133-138` says they are "read by the C7 control"; `rg NativeBindCount|ClientReleaseCount`
over `p5/v1` and over the joint returns only the declaration, the definition and the comments. The
shipped C7 control reads neither.

### N-7 — minor — m-5's null is `#if MOBILEGL_BUILD_DISAGGREGATED` but not transport-guarded

`Managers.cpp:2983-2985` runs in a disaggregated build under **monolith** transport too. There
`StagedShadowStore::m_copies` is false, so `Adopt` never sets `m_any`, `HasShadow` always answers false,
and `resource->hostBytes` is nulled on every twin's first ensure. Harmless today (split-under-monolith
`integration-gpu` is 1149/1149, and `liveHostBase()` prefers `MappedData()` there), but "Under monolith
nothing changes" — the comment two lines above it and the report's §"M-2 cascade" — is not exactly true,
and the two other round-2 Managers.cpp hunks *are* transport-guarded. Make it consistent.

### N-8 — minor — m-6's second `AssertConsumerMaskIsHonest` cannot detect what its comment says it closes

`Init.cpp:216-224`. The check only fires on `claimsResources && !hasResourceOps`. A client object that
*registers* into `MGPipeSetResourceOps` (the case the comment names — "a client-registered
`g_resourceOps` would flip the answer under the applier's feet") makes `hasResourceOps` **true**, which
the check treats as correct. The re-run can only newly fire if the table was *removed* between step 2
and step 5. Either re-word the comment or compare the pointer against the one step 2 saw.

### N-9 — minor — two red-check `expected` regexes carry almost no information

`~/w7/p5-v1-redcheck.py`: `drained-tally-removed` expects `/DrainedRecords/` — an identifier the case
prints in several assertions — and `park-predicate-loses-control` expects `/Timeout/`, which any ctest
timeout satisfies, including an unrelated hang. They are better than the round-1 `rc != 0`, but they are
the two entries round 1 called tautological and they are still the two weakest.

---

## Claims in `v1-v2.md` the code does not support

1. **"the round-2 joint inproc census … 575 passed / 183 skipped / 551 … / 23 failed / 0 SEGFAULT"** —
   sums to 1332, not 1149; v1's own log says 392 passed (N-4).
2. **"`PrimitivesGeneratedNoXfbScenario` … passes standalone (`p5-v1-probe-primgen` rc=0) … flaky under
   `-j 6`"** and **"`HandleRecycleScenario…` passes standalone … flaky under `-j`"** — both probes ran a
   different backend/lane from the failing entries and SKIPPED every test (`[ PASSED ] 0 tests`); the
   failing entries fail deterministically standalone (N-2).
3. **M-3: "the production wiring is exercised end-to-end on the joint"** — deleting both
   `MGL_SERVER_STAGED_REQUIRE` whole-store calls leaves the joint at 21/21 and the unit lane at 19/19
   (item 4).
4. **M-6: "Red once (unit `StopWithoutAStartedThreadStillDropsThePrivateBackend`)"** — that case gates
   `ServerLoop::Stop()`'s pre-existing `!joinable` arm, which round 2 did not change. Deleting M-6's
   actual fix (`Init.cpp:194`) goes red on nothing (item 7).
5. **m-5: "Red once (unit `StagedShadowTest.HasShadowIsTrueAfterAdoptAndFalse…`)"** — that gates the
   discriminator, not the fix. Deleting the fix goes red on nothing (item 5).
6. **`ServerLoopTest.cpp:373`: "Red once by making ApplyAffinity return `requested`"** — executed; the
   suite stays green. The effective-mask read-back has no control on an unrestricted box (item 6).
7. **C7: "the instrumented native-bind COUNT is a joint-lane control"** and `ServerLoop.h:133-138`'s
   "read by the C7 control" — no such control exists on `p5/v1` or on the joint; the counters have zero
   readers (N-6). Measured, the native bind count per process is unchanged at 2 and `RepeatNoOp` is
   classified zero times on the whole lane (item 10).
8. **"the R-12 republish decision" on an identical repeat** — ID-54 says a repeat is "a no-op apart from
   the R-12 republish decision"; the code skips the republish entirely on a repeat
   (`ServerLoop.cpp:733`, `if (!ok || !outcome.boundNatively) return MOBILEGL_OK;`). Harmless on this head only
   because c1's `BackendObject_Remote::InitCapabilities` asks the server directly
   (`BackendObject_Remote.cpp:118-125`), which the report does not say. Worth an integrator sentence.
9. **"201 split-only = wave-1 + v1's two suites"** — v1 now ships **three** suites on split
   (`ServerLoopTest`, `StagedShadowTest`, `StagedShadowProductionTest`); the gate's own line prints
   "split-only names that are NOT v1s suites: 183" because its grep does not know about the third.
   Cosmetic.

---

## What I could not verify, and why

* **Whether the audit poison itself is armed on the joint lane.** I established the stronger thing it
  exists to support (the draw's only base is the server shadow, provably), but I did not instrument
  `PipeWireCodec`'s 0xDD fill on the joint to count the runs it poisons.
* **N-3 as a live reproduction.** The reduced-path lane performs exactly one make-current per process,
  so the recycled-handle window cannot be reached without a scenario that destroys and recreates an EGL
  context — none exists in the tree today.
* **codex 11 under a real cpuset.** Confirming the effective-mask read-back needs a cgroup-restricted
  child; I did not create one.
* **ASan anything.** No ASan build exists in this worktree set and configuring one was out of budget;
  m-5 and codex 5 remain source-level only.
* **DirectVulkan under split on a device, OpenRA/SPLIT, adb** — out of scope and out of reach.
* **Whether the 23 are c1's specifically.** I established they are not v1's round 2 (two independent
  reverts) and that they are split-path-only. Naming the owning package needs c1's emit table, which is
  c1's round 3 and currently in flight in `~/w7/p5-c1`.

---

## Verdict

**Mergeable after one small fix round, and the fix round is about gates and the report, not about the
code.** Every behavioural item the charter names is either correct-and-proved (ID-49 server half, M-2,
M-7, C2, C9, C10, item 12, G1) or correct-in-source with the control missing (M-3, m-5, M-6, codex 11,
C7's bind half, C6's routing). The two blockers of round 1 are genuinely closed, and M-2 is closed with
the exact measurement round 1 said would settle it — the reduced-path lane now goes red when R-11's copy
is deleted, which it did not before.

What the fix round must close, in this order:

1. **N-4** — correct the census in `v1-v2.md` to 392/183/551/23/0 and state the 428 → 392 direction
   plainly, with the c1-baseline delta (+6/−7/−2) beside it. This is the second round with the same
   error and it currently reads as a large improvement where there is a small one.
2. **N-2** — withdraw the "flaky under `-j`" classification for `PrimitivesGeneratedNoXfbScenario` and
   `HandleRecycleScenario`; both are deterministic split-path wrong answers. Re-probe with the failing
   entries' own backend and lane environment, and give those six an owner.
3. **N-5, the three that are cheap** — a real gate for M-6 (`ShutdownSplitRoles` after a forced early
   `Start` failure), for C7 (`NativeBindCount`/`ClientReleaseCount` through the forwarders; the counters
   exist and nothing reads them), and for C6 (assert the seven helpers, not the accessor's address).
   Strike the red-once lines for M-6 and m-5, and the `ApplyAffinity` one at `ServerLoopTest.cpp:373`,
   or make them true.
4. **N-3** — clear `m_haveCurrentTuple` in `ServerReleaseEGLResources`, in `ServerReleaseEGLSurface` for
   a matching surface, and on backend destruction. One line each; the failure mode is a silent
   no-context-current.
5. **Item 10's prose** — ServerLoop.h:33-40 and v1-v2.md must stop claiming that the six DirectGLES
   cache invalidations became a one-off startup cost. Measured, the native bind count per process is
   unchanged; what ID-54 actually bought is the release half (1 → 0 native releases), which is real and
   worth saying on its own.

Track as follow-ups with owners rather than blocking: M-3's and m-5's missing controls (they need a
sparse-shadow case and an ASan context-loss case), codex 11's cpuset control, ID-49's tight-size half,
N-7, N-8, N-9.

**Not v1's, but it must not merge unnoticed:** **N-1**. `MOBILEGL_IPC_VERB_BARRIER=0` is E1's exit gate
and on the joint head it exits 0 with every entry skipped. It belongs with ID-53's lane plumbing in the
joint-merge package and it should be fixed before the joint merge is called green, not after.
