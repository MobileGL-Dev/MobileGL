# P2 package E — `p2/gates` — result

Tree `~/w7/p2-gates`, branch `p2/gates`, six commits on top of the P2 contract commit `9c6a8a25`.
Not pushed.

The tree had to be created from the **SHA**, not from the tag: `p2/contract` is ambiguous in
`~/w7/pipe` (a branch of the same name exists), so `git worktree add … p2/contract` fails with
`fatal: ambiguous object name` and `wsl_p2_tree.sh` passes the ref straight through. Both refs
resolve to `9c6a8a25d88593d802e69a5d4fdee498a5b6816a`.

Build directories: `build-linux` (pull), `build-push`, `build-verify` from `wsl_p2_tree.sh`, plus
`build-retrace` (`-DMOBILEGL_BUILD_TRACE_REPLAY=ON`) and `build-bench`
(`-DMOBILEGL_BUILD_BENCHMARK=ON`) added here for this package's own pre-flights.

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

16 files, +1867 / −32 (before `aef43358`, which is a small net deletion).

**Ownership (C.5): no violation.** Every path touched is E's — `.github/workflows/test.yml`,
`MobileGL/MG_Benchmark/**`, `MobileGL/MG_IntegrationTest/**`, `android-plugin/app/src/trace/cpp/**`,
`scripts/g7_negative_control.sh`, `tools/device_bench/**`,
`tools/trace_replay/run_android_retrace_local.py`.

---

## 2. C.4's table, row by row

| row | state |
|---|---|
| CREATE `Scenarios/HandleRecycleScenario.cpp` (D18) | 4 cases, 3 arms, 5 ctest lanes |
| CREATE `Scenarios/CsoContentAddressingScenario.cpp` (G12) | 1 case, 2 arms × 2 backends |
| MODIFY `MG_IntegrationTest/CMakeLists.txt` | sources, a build-time capability-detection block, 9 new lanes |
| MODIFY `trace_benchmark.{hpp,cpp}` | `CLOCK_THREAD_CPUTIME_ID` beside the wall delta; `Report::frameCpuMs` |
| MODIFY `trace_replay_core.{hpp,cpp}` | `SummarizeSeries` factored out and reused for both series; `meanFrameCpuMs`/`medianFrameCpuMs`/`p95FrameCpuMs` and the whole `frameCpuTimesMs[]` |
| MODIFY `run_android_retrace_local.py` | `format_benchmark` prints cpu mean/p50/p95/**p99**, p50 and p99 reduced host-side from `frameCpuTimesMs[]` over the same trailing window |
| CREATE `devices/{xiaomi-adreno830.env, oppo-mali.env}` | both `PROFILE_VERIFIED=0` — deviation D-6 |
| MODIFY `MG_Benchmark/Driver/CMakeLists.txt` | `DriverBenchStateToggle` runs `mc_state_toggle` |
| CREATE `scripts/g7_negative_control.sh` (D19) | done |
| MODIFY `.github/workflows/test.yml` | `pipe-gates` becomes a gate; `ctest -L unit` on the verify runtime; the two controls by name; a rename-proof arming grep |

---

## 3. Verification — commands run and what they actually said

### 3.1 Section-A gates reachable on this tree

| gate | result |
|---|---|
| **G1** `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160 (+0.001%)`. All four resized are **inherited from the contract commit** — `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, and `_GLOBAL__sub_I_DirectGLES.cpp` −9 (see C-2). This package changes no file the pull library is built from. |
| **G2** `ctest -N` names, pull vs push | **diff empty**, 1402 entries each |
| **G5** `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' … \| sha256sum` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` — **equal** to `~/w7/p2-before-syncrenderstate.sha` |
| **G7** `bash scripts/g7_negative_control.sh build-push` | rc **2**, refusing: `no test matches RenderStateSpans\.SetterConsistency`. Designed behaviour before package A's `c2` — a missing test is not a pass |
| **G7 mechanism** `… --verify-patch-only` | rc **0**: the ColorMasks demotion applies, the patched table **still compiles** (the partition stays sorted/non-overlapping/complete, which is what makes the break invisible to the compiler and visible only to the test), sources restored byte-for-byte, `build-push` rebuilt from them, `git status --porcelain MobileGL/MG_Pipe/` empty |
| **G8** `ctest --test-dir build-verify -R HandleRecycle --no-tests=error --output-on-failure` | green; `.Legacy` **runs and passes** on both backends, `.Handles` and `.AbaControl` skip with the reasons in §3.3 |
| **G9** `python3 scripts/gen_pipe_dirty_surface.py --check` | rc **2** — the flag is package B's and does not exist yet (deviation D-7). `--summary` still works |
| **G12** `ctest -R CsoContentAddressing --no-tests=error` | green; every entry skips with the tracker-not-landed reason |
| **G13** `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | **empty** |
| **G13** `python3 scripts/check_include_closure.py` | rc 0 — `wire-header OK 2 headers in closure, 0 forbidden`; `4 probes, 0 skipped, 0 problem(s)` |
| **G13** `gen_pipe.py --check` / `--self-test` / `git diff --exit-code -- MobileGL/MG_Pipe/generated` / `symbol_report.py --self-test` | rc 0 / 0 / 0 / 0 |
| **G13** the `pipe-gates` stdio alternation over `MG_Backend` + `MG_State` | no match |
| **G14** `ctest -N` names, contract tree vs this tree | 1368 → 1402: **0 removed**, 34 added. The `p2-before-ctest-names.txt` baseline is unusable — see C-1 |

### 3.2 Suites

| command | result |
|---|---|
| `cmake --build {build-linux,build-push,build-verify} -j 12` | rc 0, 0, 0 |
| `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | `100% tests passed, 0 failed out of 1489` |
| `ctest --test-dir build-push -L unit …` | `100% passed, 0 failed out of 1489` |
| `ctest --test-dir build-verify -L unit …` | `100% passed, 0 failed out of 1489`, **13.65 s** — the measured cost of the new CI step |
| `ctest --test-dir build-linux -L integration-gpu --no-tests=error` (serial, as CI runs it) | **`100% tests passed, 0 tests failed out of 918`** |
| `ctest --test-dir build-{linux,push} -L integration-gpu -j 4` | 914/918 — four failures, **pre-existing and local-only**, see §3.6 |
| `ctest --test-dir build-verify -R 'HandleRecycle\|CsoContentAddressing' -j 4` | `100% passed, 0 failed out of 44` |
| `ctest --test-dir build-bench -R DriverBench --output-on-failure` | `DriverBench` Passed 2.23 s; **`DriverBenchStateToggle` Passed 0.69 s** |

`DriverBench mc_state_toggle` on this box, for the record:
`mc_state_toggle,120,46,3.232,70259.1,309.4` (case, frames, ops/frame, median_frame_ms, ns_per_op, fps).

### 3.3 The new entries, and the exact skip reasons

34 new names. Lanes: `{DirectGLES,DirectVulkan}.HandleRecycle.{Handles,Legacy}.`,
`DirectVulkan.HandleRecycle.AbaControl.` (DirectVulkan only — the knob reverts two DirectVulkan
guards and steers nothing on DirectGLES), and
`{DirectGLES,DirectVulkan}.CsoContentAddressing.{On,Off}.`.

**What actually executed rather than skipping**, which is the load-bearing part of the evidence:

```
DirectGLES.HandleRecycle.Legacy.…AVertexArrayAtARecycledAddress…    Passed (15 ms)
DirectGLES.HandleRecycle.Legacy.…ATextureAtARecycledAddress…        Passed
DirectGLES.HandleRecycle.Legacy.…AFramebufferAtARecycledAddress…    Passed ( 4 ms)
DirectVulkan.HandleRecycle.Legacy.…AVertexArrayAtARecycledAddress…  Passed (11 ms)
DirectVulkan.HandleRecycle.Legacy.…ATextureAtARecycledAddress…      Passed
DirectVulkan.HandleRecycle.Legacy.…AFramebufferAtARecycledAddress…  Passed ( 1 ms)
HandleRecycleScenario.TheReproducerRecyclesEveryName                Passed (every lane)
```

They **passed rather than skipping**, which means the ABA really was constructed:
`glGenVertexArrays` / `glGenTextures` / `glGenFramebuffers` handed every deleted name straight
back, and in the vertex-array case `glGenBuffers` did too, so the replacement VAO *and* its buffer
carry the dead objects' names with a byte-identical attribute configuration. Had any name not been
recycled the case would have reported `inconclusive, not proven` (the `ObjectLifetimeIdTest`
shape), not a pass. This is also the evidence that **today's** `lifetimeId` + `weak_ptr` guards do
stop this ABA — which is what the re-key has to be at least as strong as.

Skip reasons, verbatim from `ctest -V`:

* `HandleRecycle.Handles` — *"the Handles arm needs the backend's {slot, gen} re-key, and this
  build does not have it: no source under MobileGL/MG_Backend/DirectGLES mentions the Track H
  subsystem constant (P2 package C for DirectGLES, package D for DirectVulkan). The arm is
  registered and visible, and arms itself when that package lands."*
* `HandleRecycle.AbaControl` — *"the AbaControl arm needs MOBILEGL_PIPE_HANDLE_ABA_CONTROL to have
  a consumer, and this build has none: MG_Config parses the knob (ConfigLoader.cpp) but no source
  under MobileGL/MG_Backend/ reads Features.PipeHandleAbaControl, so the two guards the knob is
  supposed to defeat are still in force and the ABA cannot be reproduced. P2 package D owns that
  consumer."*
* `CsoContentAddressing`, **pull** build — *"this library was built without MOBILEGL_PIPE_PUSH, so
  there is no render-state CSO to mint, no cso[] bracket in the summary line and nothing for the
  content-addressing bit to steer. The entry is registered here anyway so that
  `ctest -L integration-gpu` names the same tests in the pull build and the push build (gate G2)."*
* `CsoContentAddressing`, **push** build — *"the CSO counters have no emitter in this build:
  MG_Impl/Pipe/Tracker.cpp does not exist, so nothing mints or binds a render-state CSO and
  csom / csob are structurally zero. P2 package B owns the tracker; this entry arms itself when it
  lands."*

Every one of those skips is decided by the **build**, not by a hand-written guard.
`MG_IntegrationTest/CMakeLists.txt` looks for `MG_Backend/DirectGLES/SlotTables.h`,
`MG_Impl/Pipe/Tracker.cpp` and the strings `kMGPipeSubsystemMagmaVertexInput` /
`PipeHandleAbaControl` inside `DirectVulkan/Renderer/VertexInputStateFactory.cpp`; the two
existence answers use `file(GLOB … CONFIGURE_DEPENDS)` (re-evaluated before every build, costing
nothing until the file appears), the one content grep adds that single small file to
`CMAKE_CONFIGURE_DEPENDS` (so an edit to it reconfigures, and an edit anywhere else in the backend
does not). Each verdict is printed at configure time:

```
Integration tests: DirectGLES has no SlotTables.h - HandleRecycle.Handles will SKIP on it
Integration tests: no MG_Impl/Pipe/Tracker.cpp - CsoContentAddressing will SKIP
Integration tests: DirectVulkan's vertex input is not re-keyed yet - HandleRecycle.Handles will SKIP on it
Integration tests: MOBILEGL_PIPE_HANDLE_ABA_CONTROL has no consumer - HandleRecycle.AbaControl will SKIP
```

### 3.4 The CPU series (D.4.1), pre-flighted on the desktop CLI

`build-retrace` configured and built (`rc=0`, 165/165). One real replay of
`minecraft-1.17-main-menu-854`:

```
mobilegl_trace_replay --trace … --backend DirectGLES --mobilegl-library build-linux/libMobileGL.so \
  --benchmark --benchmark-finish=0 --benchmark-tail-frames=50
→ benchmark completed; frames=2, tailFrames=2, meanMs=655.506, medianMs=655.506, p95Ms=1290.787,
  meanCpuMs=271.766, medianCpuMs=271.766, p95CpuMs=535.271, fps=1.526
```

`benchmark.json` checked programmatically: `frameCpuTimesMs[]` present, **same length** as
`frameTimesMs[]`, every entry ≥ 0, `sum(cpu) ≤ sum(wall)` (wall `[1290.787, 20.225]`,
cpu `[535.271, 8.261]`).

`format_benchmark` on that JSON:
```
frames=2 total=1.3s tail=2 mean=655.506ms median=655.506ms p95=1290.787ms fps=1.5 \
  | cpu mean=271.766ms p50=8.261ms p95=535.271ms p99=535.271ms
```
and with `frameCpuTimesMs` removed:
`… | cpu unavailable (no per-thread CPU clock in this run)`.

### 3.5 The device-profile guard

`bash -n` clean on `bench.sh` and `session.sh`. Both exit **2** against an unverified profile with
the refusal text; `--allow-unverified-profile` warns (three `[warn]` lines saying the run is not
comparable with a pinned one) and proceeds. `odinlite.env` sets no `PROFILE_VERIFIED` and the guard
defaults to "verified", so its behaviour is unchanged.

### 3.6 A pre-existing `-j 4` flake this run surfaced — **not** caused by this package

`ctest -L integration-gpu -j 4` fails four entries, identically in the pull and the push build:

```
DirectGLES.UnlocatedIoBlocks.…TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn
DirectVulkan.PrimGenReroute.…TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn
DirectGLES.PointSizeDemotion.…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn
DirectVulkan.PointSizeDemotion.…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn
```

Diagnosis, from `LastTest.log`: the failure message ends `Log appended by this test:` followed by
**nothing**. Each of those pinned lanes registers a whole scenario (`TEST_FILTER "<Scenario>.*"`)
against ONE `MOBILEGL_LOG_FILE_PATH`, and the library opens its log `fopen(path, "w")` — so under
`-j` a sibling case in the same lane truncates the file between this case's draw and its read.
This is exactly the hazard `MG_IntegrationTest/CMakeLists.txt` documents for the arming lane; those
three lanes do not follow the rule.

Evidence that it is a race and not a regression:

* `ctest -L integration-gpu` **serially** (which is how CI runs it — `test.yml` lines 293-298 use
  no `-j`) is `100% tests passed, 0 tests failed out of 918`;
* the same four run alone: all pass;
* the three pinned lanes together at `-j 4`, with none of this package's entries in the selection:
  `21/21` pass — the race is timing-dependent, not selection-dependent;
* the failing lanes contain no entry this package added, and this package changes no library source.

**Not fixed here**, deliberately: the fix is to give each arming case its own filtered lane and log
(six more entries across three unrelated scenarios), which is scope this package has no mandate
for and which churns the ctest name set that G2/G14 are read against. Recorded for the integrator:
it is latent, CI is safe from it today only because CI runs `integration-gpu` serially, and the
rule it violates is *"a log-reading case owns its lane"*, not *"…owns its log path"*.

The same defect existed in this package's own first draft — two CSO cases in one lane — and is
fixed in `aef43358` by folding them into one case.

---

## 4. Deviations from the brief, with reasons

**D-1 (blocking) — D18's `AbaControl` arm cannot assert the corruption on this tree, and the fix
is not mine to make.** D.2 says that on the contract tree `.AbaControl` PASSES by asserting the
corrupted pixels, and C.4 says that red is the "重键前红" artefact E records first. It cannot be
produced from package E alone: `Features.PipeHandleAbaControl` is parsed at
`ConfigLoader.cpp:267` and **read by nothing**. Its consumers — `VertexInputStateFactory::ComputeHash`
hashing `attr.Buffer.get()` and `LookupVaoDrawMemo` skipping the `vaoLifetimeId` compare — live in
`MobileGL/MG_Backend/DirectVulkan/**`, which C.5 gives to **package D**. Per instruction I did not
edit it and record it here instead.

What was done so the gate is not lost: the arm is registered, always-on and visible; it skips with
a message naming the missing consumer *and its owning package*; and the skip lifts automatically,
with no further edit to this package, the moment `PipeHandleAbaControl` appears in
`VertexInputStateFactory.cpp`.
**Integrator action:** package D must implement the knob (D18 specifies exactly what it does), and
D.2's "red before" log has to be taken **after D lands the knob and before D enables its re-key**,
not on the contract tree. The alternative is to move the knob's consumer into E's scope, which
changes C.5.

**D-2 — `.Handles` and the CSO control skip for the same structural reason.** `.Handles` needs C/D,
`CsoContentAddressing` needs B's tracker. D.2 already expects `.Handles` to skip; the CSO skip is
the same shape and is stated because C.4 does not mention it.

**D-3 — the CSO lanes register in the pull build too.** C.4 implies a push-only registration
(`ctest --test-dir build-push -R CsoContentAddressing`), which is what I wrote first. It breaks
**G2**: the push build would then name four tests the pull build does not. Commit `3766d842`
registers them unconditionally and skips in the pull build with the reason; the brief's command
still works unchanged.

**D-4 — `g7_negative_control.sh` patches `MGPipeRenderStateSpans.h` as well as `.cpp`.** D19 says
it patches the `.cpp`. In the landed contract the chunk table is a `constexpr` boundary array in
the **header**; the `.cpp` only holds the two half-arrays whose lengths follow from it. Tree wins
for facts. The script also relaxes the four measurement pins (`== 7`, `== 8`, `== 396`, `== 772`)
because those pin the shipped table rather than the invariant — leaving them would turn the control
into a build break, proving the `static_assert`s work rather than that the test still checks.

**D-5 — the demotion is spelled as two extra boundaries.** The table alternates dynamic/pipeline by
index parity, so inserting exactly **two** boundaries (at `ColorMasks` and at
`FramebufferSrgbEnabled`) demotes `ColorMasks` alone and leaves every later chunk's parity, and
therefore its half, unchanged. Verified: the patched table compiles.

**D-6 — the two device profiles ship `PROFILE_VERIFIED=0`, and `bench.sh`/`session.sh` gained a
refusal.** The device-specific facts those profiles need — cpufreq policy names, the real OPPs, the
GPU pin node, the thermal zone `type` — are **not knowable without the devices**, and `35d0befa` is
a Qualcomm part while the harness pins through MediaTek nodes
(`/proc/ppm/policy/hard_userlimit_*`, `/proc/gpufreq/gpufreq_opp_freq`). Guessing them is the worst
available outcome: `su -c 'echo … > /proc/…'` fails with a **zero** exit, so a run against a wrong
profile reports numbers it believes were taken under a frequency pin. So every device-specific
field is `TODO_VERIFY_ON_DEVICE`, both profiles declare `PROFILE_VERIFIED=0`, and the two scripts
that pin refuse such a profile unless `--allow-unverified-profile` is passed. **The guard itself is
a deviation** (C.4 asked only for the `.env` files); it is inside `tools/device_bench/**`, which
C.5 gives to E, and the README records what earns `PROFILE_VERIFIED=1`.

**D-7 — `pipe-gates` now runs a flag that does not exist yet.**
`gen_pipe_dirty_surface.py --check` / `--self-test` are package **B**'s (C.1); on this tree they
exit 2. D.1 lands `tracker` before `gates`, so by the time E merges the flags exist — but **do not
run `gh workflow run test.yml` on a tree that has `gates` without `tracker`.**

**D-8 — `build-linux-verify`'s arming grep now accepts either entry-point name.** Not in the brief.
Package B renames `MGPipeFillForVerb` → `MGPipeValidateForVerb` (D1), and the CI step greps `nm`
for the old name, so it would go red on the rename for a reason unrelated to what it tests. It now
requires `MGPipeValidateForVerb|MGPipeFillForVerb` and still fails when there is neither;
`MGPipeVerifyInputs` is unchanged and still checked on its own.

**D-9 — six commits, not C.4's three.** One commit covers both scenarios (they share
`MG_IntegrationTest/CMakeLists.txt` and the capability block, so splitting them would mean two
commits editing one hunk); the extra two are D-3's G2 fix and §3.6's lane fix, both found by
verification after the fact.

---

## 5. Where the tree contradicted the brief

**C-1 — `~/w7/p2-before-ctest-names.txt` is not comparable with any pull build.** It holds **2363**
names. `ctest -N` today reports **1364** in `~/w7/pipe/build-linux`, **1368** in
`~/w7/p2-contract/build-linux`, **1402** here, and **2238** in `~/w7/p2-gates/build-verify`. So the
baseline looks like it was captured from a verify-configured directory, and G14's
`comm -23 ~/w7/p2-before-ctest-names.txt -` reports 999 "removed" tests against **any** pull build,
including the untouched contract tree. **The integrator should re-capture it** from a pull build at
`48268068`, or state which configuration it belongs to. G14 was evaluated against the contract tree
instead: 1368 → 1402, **0 removed**, 34 added.

**C-2 — the pull build's resized set is four symbols, not D15's three.**
`_GLOBAL__sub_I_DirectGLES.cpp` is −9 bytes at the contract commit. This package touches no file
the pull library is built from, so the delta is package A's to attribute; G1 as written ("`resized`
is empty or exactly the one mangled `RenderState::RenderState()` symbol") is already violated by
the contract commit alone.

**C-3 — the tag `p2/contract` is ambiguous** with a branch of the same name;
`git worktree add … p2/contract` fails outright. Use `refs/tags/p2/contract` or the SHA.

**C-4 — D19's "patches `MGPipeRenderStateSpans.cpp`"** — the table is in the header (D-4). D6's
offset table also differs from the landed header in one row (the header puts
`StencilStates[0].Func` in pipeline chunk P2, D6's table puts it in P3). That is package A's
business and is noted only because `g7_negative_control.sh` had to read the real table to patch it.

**C-5 — `ARCHITECTURE.md:504` / `tools/bench.sh`** — confirmed as the brief's correction 5; the
script is `tools/device_bench/bench.sh`. This package edits that file; the doc citation is the
integrator's to fix.

**C-6 — `test.yml` runs `integration-gpu` serially**, not `-j 4`. Relevant because §3.6's flake is
only reachable in a parallel local run.

---

## 6. Notes the integrator will want

* **Measured CI cost of the new steps**: `ctest -L unit` on the verify runtime **13.65 s** / 1489
  entries; the two negative-control lanes **~1.8 s** / 44 entries; `DriverBenchStateToggle`
  **0.69 s**.
* **The CSO control's numeric contract**, so package B can meet it deliberately: 8 toggle pairs =
  16 draws in one frame, each changing the pipeline subset. Content-addressed arm `csom <= 4`
  **and** `csom < csob`; bit-63 arm `csom == csob`; both arms `csob >= 16`; and the readback is
  all-green and byte-identical across two consecutive toggle frames. If B emits the counters
  anywhere other than `PipeStats::AddCalls(CallClass::RenderStateCsoMints/Binds, …)` the control
  reads zeroes, and the `csob >= 16` assertion turns that into a failure rather than a pass.
* **`MOBILEGL_PIPE_STATS_PERIOD=1`** is what makes the control readable at all: the summary line's
  window is "since the previous line", so the case brackets its workload with two swaps and parses
  the last line.
* **`DriverBenchStateToggle` is a "does this case still run" gate** against whatever
  `$DRIVERBENCH_EGL_LIB` names, not the measurement. D.4.4's numbers still come from
  `run_driver_bench.sh {native,espryt,magma}`.
* §3.6's `-j` flake is worth a follow-up on its own; the fix shape is in that section.

---

## 7. Unfinished

* **D.2's "red before" log cannot be produced by this package** (D-1). As D.2 specifies it, it
  would today record two skips and a pass, not a red.
* **`scripts/g7_negative_control.sh` has never completed its real path**, only the refusal and
  `--verify-patch-only`. The first end-to-end run needs package A's
  `RenderStateSpansTest.SetterConsistency`; the script exits 2 rather than reporting a pass when
  that test is absent, so this cannot be mistaken for a green.
* **G3 / G4 not run here.** This package changes no source the library is built from, so the
  40-trace SSIM corpus and the verify divergence lane would be measuring the contract commit. They
  belong to the integrator's D.3.
* **G6 / G10 / G11 unreachable here** — A's tests, and G11 needs two devices.
* **The Android build of the CPU series is compile-unverified.** `trace_benchmark.cpp` and
  `trace_replay_core.cpp` are shared with the Android trace app; only the desktop CLI was built and
  run. The change is additive and the clock is guarded for non-POSIX, but `apk.yml` is where the
  NDK build of it first runs.
* **The two device profiles are unverified by construction** (D-6) and cannot be finished without
  the devices. **No device lock was taken by this package**, and no on-device measurement (D.4.2,
  D.4.3, D.4.5) was attempted.
* **`ctest -L benchmark` was run only for the two `DriverBench` entries.** The other five benchmark
  targets report `Not Run` because only the `DriverBench` target was built in `build-bench`; they
  are untouched by this package.
