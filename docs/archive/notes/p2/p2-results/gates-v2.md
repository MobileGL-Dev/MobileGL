# P2 package E — `p2/gates` — result (v2, rework round 2)

Tree `~/w7/p2-gates`, branch `p2/gates`, **eight** commits on the P2 contract commit `9c6a8a25`
(`refs/tags/p2/contract`). Not pushed. Two new commits this round, on top of v1's `aef43358`.

The review `scratchpad/wf5/p2-results/gates-review-v1.md` rejected v1 with 2 majors and 10 minors.
**Both majors are fixed. Eight of the ten minors are fixed** (including the two the reviewer left
to the integrator — the G7 control's real path is now proven end to end, and the `-j 4` flake's
pre-existence is now demonstrated by a control run, not argued). The two that remain are §4's D-1
and D-7, both of which are structurally outside package E and are re-declared below.

---

## 1. Commits

| sha | subject |
|---|---|
| `f02ca420` | `[Test] (Pipe): reproduce the handle ABA through public GL and pin the CSO content-addressing switch` |
| `1b25f2c8` | `[Feat] (Trace): record per-frame thread CPU time beside wall time so a paired A/B can be read as CPU cost` |
| `718818f1` | `[Feat] (Bench, Pipe): run the blend-toggle case in CI, give G7 a negative control, and record the two campaign devices` |
| `3766d842` | `[Test] (Pipe): register the CSO control in the pull build too, so pull and push name the same tests` |
| `bf62597c` | `[CI] (Pipe): make the dirty-surface report a gate and run the push-only unit tests on the verify runtime` |
| `aef43358` | `[Test] (Pipe): one case per CSO lane, because two of them would race on the lane's log` |
| **`07a7aa89`** | **`[Fix] (Test, Pipe): ask the BUILD, not the source tree, whether a control's arm exists, and probe the CSO emitter by content`** — majors 1 and 2, minor 4 |
| **`71b1bd27`** | **`[Fix] (Trace, Bench, CI): compute p50 by the device's own median rule, fail the profile guard closed, and give the new control step its sibling's environment`** — minors 1, 2, 3, 6 |

Whole branch vs the contract: 17 files, +2021 / −32. This round only (`aef43358..HEAD`): 11 files,
+234 / −72.

**Ownership (C.5): no violation.** Every committed path is E's (`.github/workflows/test.yml`,
`MobileGL/MG_Benchmark/**`, `MobileGL/MG_IntegrationTest/**`, `android-plugin/app/src/trace/cpp/**`,
`scripts/g7_negative_control.sh`, `tools/device_bench/**`,
`tools/trace_replay/run_android_retrace_local.py`). No fixture binary touched. One *uncommitted*
experiment did write package A's files temporarily — declared as **D-10** in §4, with the
byte-restore evidence.

---

## 2. The two majors

### MAJOR 1 — the capability markers asked the source tree, not this build

**Fixed in `07a7aa89`, in three places.**

1. **The whole capability block moved under `if (MOBILEGL_PIPE_PUSH)`**
   (`MG_IntegrationTest/CMakeLists.txt`), which is where `MGITEST_PIPE_PUSH_BUILD` already lived.
   A pull build now cannot set `MGITEST_HANDLE_REKEY_*`, `MGITEST_HANDLE_ABA_IMPLEMENTED` or
   `MGITEST_PIPE_TRACKER_PRESENT` whatever the sources contain, and says so at configure time:
   `Integration tests: pull build - HandleRecycle.{Handles,AbaControl} and CsoContentAddressing
   stay registered (G2) and SKIP: every arm they assert is compiled only under MOBILEGL_PIPE_PUSH`.
2. **A second lock inside the scenario.** `HandleRecycleScenario::SkipUnlessTheArmIsAssertableHere()`
   checks `MGITEST_PIPE_PUSH_BUILD` for the `Handles` and `AbaControl` arms **before** the per-arm
   markers, exactly the shape `CsoContentAddressingScenario.cpp:207-214` already carried. A
   hand-forced environment cannot arm an arm the library does not have.
3. **The push-only knobs are no longer injected into pull-build lanes.**
   `MOBILEGL_PIPE_LEGACY_MEMOS=0` (Handles lanes) and `MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1`
   (AbaControl lane) are set only when `MOBILEGL_PIPE_PUSH` is on. Test **names** are unaffected —
   an `ENVIRONMENT` property is not part of a name — so G2 still compares equal (verified below).

Verified, in `build-linux` (`MOBILEGL_PIPE_PUSH:BOOL=OFF`), with the two environments the reviewer
used to reproduce the defect:

```
env MOBILEGL_BACKEND_TYPE=DirectVulkan MGITEST_HANDLE_ARM=aba MOBILEGL_PIPE_PUSH=0 \
    MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1 MGITEST_HANDLE_ABA_IMPLEMENTED=1 \
    ./MobileGLIntegrationTest --gtest_filter='HandleRecycleScenario.AVertexArray*'
→ HandleRecycleScenario.cpp:289: Skipped
  "the AbaControl arm needs a library built with MOBILEGL_PIPE_PUSH, and this one was not: the
   {slot, gen} re-key and Features.PipeHandleAbaControl are both #if MOBILEGL_PIPE_PUSH …"
  [  PASSED  ] 0 tests.  [  SKIPPED ] 1 test        (was: 1 FAILED TEST)

env MOBILEGL_BACKEND_TYPE=DirectGLES MGITEST_HANDLE_ARM=handles MOBILEGL_PIPE_LEGACY_MEMOS=0 \
    MGITEST_HANDLE_REKEY_DirectGLES=1 …
→ same skip                                          (was: [ OK ] — a green asserting nothing)
```

Generated ctest environments, read out of `*_tests.cmake` in both builds:

| | `build-linux` (pull) | `build-push` |
|---|---|---|
| Handles lane env | `MGITEST_HANDLE_ARM=handles;__EGL_VENDOR…` | `MGITEST_HANDLE_ARM=handles;MOBILEGL_PIPE_LEGACY_MEMOS=0;MGITEST_PIPE_PUSH_BUILD=1;…` |
| AbaControl lane env | `MGITEST_HANDLE_ARM=aba;MOBILEGL_PIPE_PUSH=0;…` | `…;MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1;MGITEST_PIPE_PUSH_BUILD=1;…` |
| `MOBILEGL_PIPE_LEGACY_MEMOS=0` occurrences | **0** | 4 (2 files) |
| `MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1` | **0** | 1 |

**And the markers still arm when the packages land** — simulated by copying the real files out of
the other packages' trees into this one and reconfiguring (then removing them; the tree is
byte-identical afterwards, `git status` shows only `?? build-retrace/`):

```
copy p2-tracker/MG_Impl/Pipe/CsoCache.h, p2-espryt/MG_Backend/DirectGLES/SlotTables.h,
append one line naming kMGPipeSubsystemMagmaVertexInput / PipeHandleAbaControl to
MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp
→ build-push:  DirectGLES is keyed on {slot, gen} (SlotTables.h present)
               the CSO counters have an emitter (…/MobileGL/MG_Impl/Pipe/CsoCache.h)
               DirectVulkan's vertex input is keyed on {slot, gen}
               MOBILEGL_PIPE_HANDLE_ABA_CONTROL has a consumer
→ build-linux: pull build - … stay registered (G2) and SKIP
(after removal + restore, both builds print the "will SKIP" verdicts again;
 VertexInputStateFactory.cpp sha 0c5e2e00850dcfb2 before and after)
```

**One place the tree contradicts the review** (recorded, does not change the fix): the review's
third consequence — "`MOBILEGL_PIPE_LEGACY_MEMOS=0` with every subsystem bit clear is D14's startup
`Fatal{PipeLegacyMemosDisabled}`" — would **not** actually have aborted a pull build. Both
implementations of that trap are inside `#if MOBILEGL_PIPE_PUSH`
(`p2-espryt/.../Managers.cpp:173-197 ResolveEsprytSlotTablesArm`,
`p2-magma/.../MagmaPipeArms.h:13,47,79-91`), so a pull build would have *silently ignored* the
knob rather than stopped. The fix stands on its own ground — a lane must not set a knob the build
cannot honour — but the integrator should not expect an abort as the symptom.

### MAJOR 2 — G12's arming probe globbed a file package B does not create

**Fixed in `07a7aa89`.** The probe is now a **content** probe over everything the tracker package
owns, not a filename:

```cmake
file(GLOB MGL_ITEST_PIPE_CLIENT_SOURCES CONFIGURE_DEPENDS
    "${MGL_ITEST_ROOT}/MobileGL/MG_Impl/Pipe/*.h" "…/*.cpp")
foreach(... )   # each file added to CMAKE_CONFIGURE_DEPENDS
    file(STRINGS "${src}" hits REGEX "RenderStateCso(Mints|Binds)")
```

It looks for what the control actually reads — an emitter of `CallClass::RenderStateCsoMints` /
`RenderStateCsoBinds` — so the owning package keeps control of its file layout. Verified against
package B as it exists: `~/w7/p2-tracker`'s only match under `MG_Impl/Pipe/` is `CsoCache.h`
(header-only, no `Tracker.cpp`), and copying that one file in arms the marker (transcript above).
On the contract tree, no `MG_Impl/Pipe` source names either counter, so the entries still skip —
with a reason that is now true and layout-independent: *"no source under MobileGL/MG_Impl/Pipe/
names RenderStateCsoMints or RenderStateCsoBinds … this entry arms itself when they land, whatever
files that package chooses to put them in."* The scenario's file header comment was corrected the
same way.

---

## 3. The minors

| # | review's finding | this round |
|---|---|---|
| 1 | `format_benchmark`'s p50 was nearest-rank while the device median averages the two middle frames | **fixed** — new `series_median()` transcribes `SummarizeSeries` (`trace_replay_core.cpp:854-856`); p95/p99 stay nearest rank and the docstrings now say which rule is which. Re-run pre-flight: device `medianCpuMs=383.378`, printed `p50=383.378ms` (host value `383.3785`; the JSON writer rounds to 3 dp, which is the whole remaining difference). Rule check: `[1,2,3]→2`, `[1,2,3,4]→2.5`, `[]→-1.0` |
| 2 | the two new profiles claimed `profile.sh` refuses an unverified profile | **fixed** — `profile.sh` records a simpleperf profile and **pins nothing**, so it needs no guard; the claim is corrected in both `.env` files and in `README.md` rather than a guard added where there is nothing to guard |
| 3 | `require_verified_profile` failed open on a profile that omits the key | **fixed** — default is now `${PROFILE_VERIFIED:-0}` in `bench.sh` and `session.sh`, `odinlite.env` carries `PROFILE_VERIFIED=1` explicitly with the sentence that earns it, and the refusal reads *"does not carry PROFILE_VERIFIED=1 (it says 0, or says nothing at all)"*. Verified: a `/tmp` profile with only `DEVICE_SERIAL` → rc 2 from both scripts; `--allow-unverified-profile` → the three `[warn]` lines then proceeds; `devices/odinlite.env` → passes the guard and reaches adb; `devices/{xiaomi-adreno830,oppo-mali}.env` → rc 2 |
| 4 | an unrecognised `MGITEST_HANDLE_ARM` silently became `Legacy` | **fixed** — `ArmNameIsRecognised()` + `FAIL()` in `SetUp`, so a typo fails the fixture before the body runs. Verified: `MGITEST_HANDLE_ARM=handels` → `1 FAILED TEST`, *"unknown MGITEST_HANDLE_ARM value 'handels': the arms are handles / legacy / aba…"* |
| 5 | `g7_negative_control.sh` had never run its real path | **fixed (proven)** — see §3.1. Green before, **red naming `SetColorMask`** on the demotion, green again after the restore |
| 6 | the new CI control step was thinner than its sibling | **fixed** — it now carries `ulimit -c unlimited`, the `kernel.core_pattern` line and the three `MOBILEGL_MAGMA_*` variables, with a comment saying why it is the sibling's environment verbatim |
| 7 | the stale `2238` `build-verify` name count | **corrected**: `2230`. And a second stale figure the review did not catch: v1 quoted **918** `integration-gpu` entries in `build-linux`; the real number is **912** (contract `878` + E's `34`), measured in both builds |
| 8 | `pipe-gates` is red on this branch alone | **still true, still declared** (D-7). `gen_pipe_dirty_surface.py --check` exits 2 here; the flag is package B's and D.1 lands `tracker` first. The warning is repeated in §6 |
| 9 | the `-j 4` flake's pre-existence was argued, not demonstrated | **fixed (demonstrated)** — see §3.2 |
| 10 | D.2's "红 before the re-key" artefact is not produced | **still true, still declared** (D-1). The knob's consumer is package D's file |

### 3.1 The G7 negative control, run for real

Package A's five files were applied into this tree as an **uncommitted** simulation
(`git -C ~/w7/p2-spans diff refs/tags/p2/contract..HEAD -- <5 files> | git apply`, 1481 insertions),
`build-push` rebuilt, the control run, then everything restored:

```
[g7] 1 matching test(s) before the patch
[g7] RenderStateSpans\.SetterConsistency is green before the patch
[g7] demoting ColorMasks out of the pipeline half
[g7] the patched table still compiles, so the partition is still complete
[g7] negative control tripped, naming SetColorMask
[g7] restored; rebuilding
[g7] negative control tripped and the tree is green again
g7 rc=0
```
Restore evidence: the five files' sha256 list is identical before and after (`diff` empty),
`git status --porcelain` shows only `?? build-retrace/`, `build-push` was rebuilt from the restored
sources and `ctest -L unit` is `100% tests passed, 0 failed out of 1489`.

So all three halves of G7 are now proven on this tree: the refusal when the test is absent (rc 2),
the patch mechanism (`--verify-patch-only`, rc 0), and the control itself (rc 0, red for the right
reason). The integrator's D.3 run is now a repeat, not a first.

### 3.2 The `-j 4` `integration-gpu` flake — a control run, not an argument

Selection identical to what the tree ran **before** package E existed (E's 24 entries excluded),
in `build-linux`, `-j 4`:

```
ctest -L integration-gpu -E 'HandleRecycle|CsoContentAddressing' -j 4     (run 1)
  The following tests FAILED:
    2354 - DirectVulkan.PrimGenReroute.…TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn
    2362 - DirectGLES.PointSizeDemotion.…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn
    2367 - DirectVulkan.PointSizeDemotion.…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn
  (run 2, same selection: green)
```

That settles it in the direction v1 claimed but did not prove: the failure reproduces **with E's
entries removed from the selection**, and it is timing-dependent (a second run of the same
selection passed). E's 24 extra processes are not the cause. It remains latent, CI is safe from it
only because CI runs the label serially, and the fix (one filtered lane and one log per
log-reading case, in three scenarios E does not own) is still out of this package's scope.

---

## 4. Verification — every command and what it said

All in `~/w7/p2-gates` unless stated. Builds: `cmake --build {build-linux,build-push,build-verify}
-j 12` → rc 0, 0, 0.

### 4.1 Section-A gates reachable here

| gate | command | result |
|---|---|---|
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`, `.text +160 (+0.001%)`. The four resized are the contract's, unchanged from v1 (`RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9) — see C-2 |
| **G2** | `ctest -N` names, pull vs push | **diff empty**, 1402 each. Re-checked after the g7 rebuilds: still empty |
| **G2** | `ctest -L integration-gpu --no-tests=error` serial, both builds | `build-linux` **100% passed, 0 failed out of 912** (251 s); `build-push` **100% passed, 0 failed out of 912** (235 s) |
| **G5** | `RenderStateImpl` sha, contract vs HEAD vs baseline | all three `d8fd1c48…20efe27` |
| **G7** | `bash scripts/g7_negative_control.sh build-push` | §3.1 — rc 0, tripped naming `SetColorMask`, tree green again |
| **G8** | `ctest --test-dir build-verify -R HandleRecycle --no-tests=error` (with the CSO entries: 44) | `100% tests passed, 0 failed out of 44`; the six `.Legacy` cases and every `TheReproducerRecyclesEveryName` execute and pass; the push arms skip with the reasons in §2 |
| **G9** | `gen_pipe_dirty_surface.py --check` | rc **2** — package B's flag (D-7) |
| **G12** | `ctest --test-dir build-linux -R CsoContentAddressing -j 4` | all four skip, pull-build reason |
| **G13** | `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| **G13** | `check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `gen_pipe.py --check` / `--self-test` / `git diff --exit-code -- MG_Pipe/generated` | rc 0 / 0 / 0 |
| **G13** | the `pipe-gates` stdio alternation over `MG_Backend` + `MG_State`, verbatim from `test.yml:1575` | `stdio gate clean` |
| **G14** | contract `build-linux` names vs this tree | 1368 → 1402: **0 removed**, 34 added (baseline file unusable, C-1) |

### 4.2 Suites

| command | result |
|---|---|
| `ctest -L unit --no-tests=error -j 8` in `build-linux` / `build-push` / `build-verify` | `100% passed, 0 failed out of 1489` × 3 (16.4 s / 16.3 s / **14.3 s** — the last is the measured cost of the new CI step) |
| `ctest --test-dir build-verify -R 'HandleRecycle\|CsoContentAddressing' -j 4` | `100% passed, 0 failed out of 44` |
| `ctest --test-dir build-linux -R 'HandleRecycle\|CsoContentAddressing' -j 4` | 34 entries, all skip with the pull-build reason |
| `ctest --test-dir build-bench -R DriverBench` | `DriverBench` Passed 1.48 s, `DriverBenchStateToggle` Passed 0.24 s |
| entry counts | `build-linux` 1402 total / 912 `integration-gpu` / 34 new; `build-push` identical; `build-verify` 2230 / 1740 / 44; contract `build-linux` 1368 / 878 |

### 4.3 The CPU series (D.4.1), re-pre-flighted after the p50 fix

`build-retrace`, one real replay of `minecraft-1.17-main-menu-854` against `build-linux/libMobileGL.so`:

```
benchmark completed; frames=2, tailFrames=2, meanMs=1409.229, medianMs=1409.229, p95Ms=2801.059,
  meanCpuMs=383.378, medianCpuMs=383.378, p95CpuMs=757.154, fps=0.710
format_benchmark → frames=2 total=2.8s tail=2 mean=1409.229ms median=1409.229ms p95=2801.059ms
                   fps=0.7 | cpu mean=383.378ms p50=383.378ms p95=757.154ms p99=757.154ms
with frameCpuTimesMs removed → … | cpu unavailable (no per-thread CPU clock in this run)
```
`frameCpuTimesMs = [757.154, 9.603]`; host `series_median` = 757.154 and 9.603 averaged = `383.3785`
= the device's `medianFrameCpuMs` to the JSON's precision; host nearest-rank p95 = `757.154` =
the device's `p95FrameCpuMs` exactly.

---

## 5. Deviations from the brief

Carried from v1, unchanged (see `gates-v1.md` §4 for the full reasoning): **D-2** (`.Handles` and
the CSO control skip structurally), **D-3** (the CSO lanes register in the pull build too, for G2),
**D-4** (`g7_negative_control.sh` patches the header, because the landed chunk table is in the
header), **D-5** (the demotion is spelled as two extra boundaries), **D-6** (both new device
profiles ship `PROFILE_VERIFIED=0` and the two pinning scripts gained a refusal), **D-8** (the
`build-linux-verify` arming grep accepts either entry-point name, so package B's rename does not
red it), **D-9** (more commits than C.4's three).

Still open, and both are outside package E's ownership:

**D-1 (blocking, unchanged) — D18's `AbaControl` arm cannot assert the corruption on this tree.**
`Features.PipeHandleAbaControl` is parsed at `ConfigLoader.cpp:267` and its consumers live in
`MG_Backend/DirectVulkan/**`, which C.5 gives to package D. (Package D's tree now *does* implement
it — `VertexInputStateFactory.cpp` names both `PipeHandleAbaControl` and
`kMGPipeSubsystemMagmaVertexInput` — so the arm arms itself on the integrated tree.) **Integrator
action:** D.2's "red before" log has to be taken after D lands the knob and before D enables its
re-key; it is not producible from E alone, and E's merge must not be read as having produced it.

**D-7 (unchanged) — `pipe-gates` now runs a flag that does not exist yet.**
`gen_pipe_dirty_surface.py --check` / `--self-test` are package B's. **Do not run
`gh workflow run test.yml` on a tree that has `gates` without `tracker`.**

New this round:

**D-10 — an uncommitted, restored write to package A's files.** To run the G7 control's real path
(review minor 5) I applied `~/w7/p2-spans`'s diff into this tree, built, ran the control and
restored. Nothing was committed; the five files hash identically before and after; `git status`
shows only `?? build-retrace/`. I record it because C.5 gives those files to A and the rule is
about edits, not only about commits. `~/w7/p2-spans` itself was **not** written to (only
`git diff` was read out of it), and no other package's tree was modified — the two files copied in
for the arming simulation (§2) were copies, removed afterwards.

**D-11 — `profile.sh` did not gain the verified-profile guard** the two new `.env` files claimed it
had. It pins nothing (it records a simpleperf profile), so the claim was corrected instead of the
guard added. Review minor 2 offered either.

---

## 6. Where the tree contradicted the brief or the review

**C-1 — `~/w7/p2-before-ctest-names.txt` is not comparable with any pull build** (unchanged from
v1): it holds 2363 names; `ctest -N` reports 1364 in `~/w7/pipe/build-linux`, 1368 in
`~/w7/p2-contract/build-linux`, 1402 here, **2230** in `~/w7/p2-gates/build-verify`. `comm -23`
against it reports 999 "removed" for the *untouched contract tree* too. The integrator must
re-capture it from a pull build at `48268068`. G14 was evaluated against the contract tree instead.

**C-2 — the pull build's resized set is four symbols, not D15's three.** `_GLOBAL__sub_I_DirectGLES.cpp`
is −9 bytes at the contract commit. G1 as written is already violated by the contract alone; this
package builds no library source.

**C-3 — the tag `p2/contract` is ambiguous** with a branch of the same name; use
`refs/tags/p2/contract` or the SHA.

**C-4 — D19 says the control patches `MGPipeRenderStateSpans.cpp`;** the landed chunk table is a
`constexpr` boundary array in the **header** (D-4). D6's offset table also differs from the landed
header in one row (`StencilStates[0].Func`) — package A's business.

**C-5 — `ARCHITECTURE.md:504`'s `tools/bench.sh`** is `tools/device_bench/bench.sh` (the brief's
own correction 5); the doc citation is the integrator's to fix.

**C-6 — `test.yml` runs `integration-gpu` serially**, not `-j 4`; §3.2's flake is only reachable in
a parallel local run.

**C-7 (new) — the D14 `Fatal{PipeLegacyMemosDisabled}` is push-only in both implementations**, so
the review's third consequence of major 1 would have been a silent ignore, not an abort. Detail and
citations in §2.

**C-8 (new) — v1's `918` `integration-gpu` entries was stale**; the number is `912` in both the
pull and the push build (contract 878 + E's 34), and `build-verify` is 1740 of 2230.

---

## 7. Notes the integrator will want

* **Measured CI cost of the new steps**: `ctest -L unit` on the verify runtime **14.3 s** / 1489
  entries; the two negative-control lanes ~1.8 s / 44 entries; `DriverBenchStateToggle` 0.24 s.
  Serial `-L integration-gpu` is 251 s (pull) / 235 s (push) on this box, 912 entries.
* **The CSO control's numeric contract** (unchanged, so package B can meet it deliberately): 8
  toggle pairs = 16 draws in one frame; content-addressed arm `csom <= 4` **and** `csom < csob`;
  bit-63 arm `csom == csob`; both arms `csob >= 16`; the readback all-green and byte-identical
  across two consecutive toggle frames. The counters must be emitted through
  `PipeStats::AddCalls(CallClass::RenderStateCsoMints/Binds, …)` — and now the *arming* probe reads
  the same two names, so an emitter that used a different channel would both fail the assertion and
  leave the entry skipped, which is the honest pair.
* **`MOBILEGL_PIPE_STATS_PERIOD=1`** is what makes the control readable: the summary window is
  "since the previous line".
* **`DriverBenchStateToggle` is a "does this case still run" gate**, not the measurement; D.4.4's
  numbers still come from `run_driver_bench.sh {native,espryt,magma}`.
* §3.2's `-j` flake is worth a follow-up of its own; the fix shape is "a log-reading case owns its
  lane", in three scenarios E does not own.
* **Do not run `gh workflow run test.yml` on `gates` without `tracker`** (D-7).

---

## 8. Unfinished

* **D.2's "red before" log** (D-1) — integrator action, after package D lands the knob.
* **G3 / G4 not run here.** This package builds no library source, so the 40-trace SSIM corpus and
  the verify divergence lane would be measuring the contract commit. Integrator's D.3.
* **G6 / G10 / G11 unreachable here** — A's and B's tests, and G11 needs two devices. (G6's test was
  exercised transiently by §3.1's simulation and is green there, but that is A's evidence, not E's.)
* **The Android build of the CPU series is compile-unverified.** `trace_benchmark.cpp` and
  `trace_replay_core.cpp` are shared with the Android trace app; only the desktop CLI was built and
  run. `apk.yml` is where the NDK build first runs it.
* **The two device profiles are unverified by construction** (D-6) and cannot be finished without
  the devices. **No device lock was taken by this package**; no on-device measurement (D.4.2,
  D.4.3, D.4.5) was attempted.
* **`ctest -L benchmark` was run only for the two `DriverBench` entries**; the other five benchmark
  targets report `Not Run` because only `DriverBench` was built in `build-bench`.
