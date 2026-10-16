# Adversarial review — package E `p2/gates` (v2)

Tree `~/w7/p2-gates` @ `71b1bd27`, **eight** commits on `refs/tags/p2/contract` (`9c6a8a25`).
Reviewed against BRIEF-P2 §A (G1–G14), §B (D1–D20), §C.4, §C.5, §D, §E, the v1 review
(`gates-review-v1.md`) and the implementer's `gates-v2.md`.

**Verdict: NOT APPROVED — 1 major.**

v1's two majors are genuinely fixed and I re-derived both fixes rather than taking the transcript
on trust (§1). The remaining major is a *new* always-on ctest entry this package adds and calls a
gate in two places, which cannot go red for the reason it exists — `ROADMAP.md:7`'s
"每个门必须能因它存在的理由变红", quoted in brief §A as a binding rule for P2.

---

## 0. What reproduced

All commands run in `~/w7/p2-gates` unless stated. Nothing in the tree was modified: `git status
--porcelain` reports only `?? build-retrace/` before and after this review.

| claim | my result |
|---|---|
| G1 `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`, `.text 10792579 -> 10792739 (+160)` — identical to the result file; the 4th resize is the contract's (minor 5) |
| G5 `RenderStateImpl` sha, contract / HEAD / `~/w7/p2-before-syncrenderstate.sha` | all three `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` |
| G2 `ctest -N` pull vs push | 1402 vs 1402, `diff` **empty** |
| G2 `-N -L integration-gpu` | 912 / 912 / 1740 (linux / push / verify) — the result file's corrected figure, not v1's 918 |
| G13 `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| G13 `check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` |
| G13 `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 |
| G9 `gen_pipe_dirty_surface.py --check` / `--self-test` | rc 2 / rc 2 — package B's flag (minor 3) |
| `ctest -L unit --no-tests=error -j 8` in build-linux / build-push / build-verify | `100% tests passed, 0 failed out of 1489` in all three |
| G8 `ctest --test-dir build-verify -R 'HandleRecycle\|CsoContentAddressing' -j 4` | `100% tests passed, 0 failed out of 44`; 15 executed (the 8 `TheReproducerRecyclesEveryName` lanes + the 6 `.Legacy` cases + 1), 29 skipped |
| G12 `ctest --test-dir build-{linux,push} -R 'HandleRecycle\|CsoContentAddressing' -j 4` | 34 entries each, the push arms and all four CSO entries skip with the pull-build / no-emitter reason |
| `ctest --test-dir build-bench -R DriverBench` | `DriverBench` and `DriverBenchStateToggle` both Passed (see the major) |
| G14 vs `~/w7/p2-contract/build-linux` | contract 1368 → gates 1402: `comm -23` **0 removed**, 34 added |
| C-1 (`p2-before-ctest-names.txt` unusable) | confirmed: 2363 names in the baseline; `comm -23 baseline contract-names` reports **999 "removed"** for the *untouched* contract tree |
| CI packaging really carries the two new steps' tests | simulated the `Package Linux verify runtime` tar verbatim from `build-verify`, unpacked into `/tmp/pkgsim`: `ctest -N -L unit` finds **1489**, `ctest -N -L integration-gpu -R 'HandleRecycle\|CsoContentAddressing'` finds **44**. The root `CTestTestfile.cmake` `subdirs()` straight into `MobileGL/MG_Test` and `MobileGL/MG_IntegrationTest`, so the missing `MobileGL/CTestTestfile.cmake` I went looking for does not exist and is not needed |
| Ownership (C.5) | `git diff --name-only refs/tags/p2/contract..HEAD` is 17 files, every one E's. `git diff --name-only -- tools/trace_replay/fixtures` empty. `git diff --stat -- MobileGL/MG_Pipe/ MobileGL/MG_Test/ MobileGL/MG_State/ MobileGL/MG_Backend/` empty. `MGPipeRenderStateSpans.h` hashes identical to the contract blob, so §3.1's simulation really was restored |
| Commit discipline | no `Co-Authored-By` / `Signed-off-by` / `Generated with` in any of the eight subjects or bodies; all `[Type] (Scope): …` |
| No hot-path instrumentation | the `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` lands in `android-plugin/app/src/trace/cpp/trace_benchmark.cpp`, not in the library; the diff touches no `MobileGL/` source that ships in `libMobileGL.so` |

**Chunk table / subset hash: no finding.** I re-derived the rule from
`MobileGL/MG_State/GLState/RenderState/RenderState.cpp` for ten setters and checked each against
the landed boundary array in `~/w7/p2-spans/MobileGL/MG_Pipe/MGPipeRenderStateSpans.h:*`
(`BumpVersions()` = `++m_version` + `++m_pipelineStateVersion`, `RenderState.h:159-162`):

| setter | line | bumps | member | chunk | half |
|---|---|---|---|---|---|
| `SetColorMask` | `:679-689` | `BumpVersions()` | `ColorMasks` | P1 `[BlendStates, ClearColor)` | pipeline ✓ |
| `SetSampleCoverage` | `:786-792` | `BumpVersions()` | `SampleCoverageValue/Invert` | P2 | pipeline ✓ |
| `SetPolygonMode` | `:174-179` | `BumpVersions()` | `PolygonModeFront/Back` | P5 | pipeline ✓ |
| `SetPatchVertices` | `:210-215` | `BumpVersions()` | `PatchVertices` | P0 | pipeline ✓ |
| `SetCapability` | `:308-338` | `BumpVersions()` per arm | the capability bools | P6 / P1 | pipeline ✓ |
| `SetStencilFunc` | `:637-650` | `++m_version`, `++m_pipelineStateVersion` **only if `Func` moved** | `Func` P2/P3, `Ref`/`ValueMask` D3/D4 | split | ✓ |
| `SetStencilMask` | `:652-658` | `++m_version` | `WriteMask` | D3/D4 | dynamic ✓ |
| `SetScissorBox` | `:926-949` | `++m_version` (incl. the `ScissorBoxWrittenMask` transition) | `ScissorBoxes`, mask | D7 | dynamic ✓ |
| `SetBlendColor` | `:740-745` | `++m_version` | `BlendColor` | D2 | dynamic ✓ |
| `SetPrimitiveRestartIndex` / `SetPointSize` | `:189-193` / `:199-204` | `++m_version` | `PrimitiveRestartIndex` / `PointSize` | D6 / D0 | dynamic ✓ |

So the G7 control's chosen break (`ColorMasks` demoted) is a genuine invariant violation and not a
control that cannot trip, and **the six `g7_negative_control.sh` header anchors and the two source
anchors each match exactly once against `~/w7/p2-spans@7c2c1456`** (checked with a counting script;
`kMGPipeRenderStateChunkCount = 15`, `kMGPipePipelineChunkCount == 7`, `kMGPipeDynamicChunkCount ==
8`, the two byte pins, the `BlendStates` boundary line, and the two `GlobalPipelineChunk(6)` /
`GlobalDynamicChunk(7)` tails). Inserting two boundaries at `ColorMasks` and at
`FramebufferSrgbEnabled` preserves the `(index % 2) == 1` parity rule
(`MGPipeRenderStateSpans.h`, the comment above `MGPipeRenderStateChunkIsPipeline`), so the patch
really does compile and really does demote exactly `ColorMasks`.

---

## 1. v1's two majors: fixed, and re-derived independently

**v1 MAJOR 1 (capability markers asked the source tree, not the build).** Fixed at `07a7aa89`.
I read the generated ctest environments out of the build directories rather than the transcript:

```
$ grep -rho 'MGITEST_PIPE_PUSH_BUILD=1' build-{linux,push,verify}/MobileGL/MG_IntegrationTest/
build-linux 0   build-push 24   build-verify 24
$ grep -rho 'MOBILEGL_PIPE_LEGACY_MEMOS=0' …
build-linux 0   build-push 8    build-verify 8
$ grep -rho 'MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1' …
build-linux 0   build-push 4    build-verify 4
```

The empty `${MGL_ITEST_HANDLES_ARM_KNOBS}` really does vanish (CMake drops an unquoted expansion of
an empty variable), the names are unchanged, and `ctest -N` still diffs empty pull vs push. The
second lock in `HandleRecycleScenario::SkipUnlessTheArmIsAssertableHere()`
(`HandleRecycleScenario.cpp:288-297`) checks `MGITEST_PIPE_PUSH_BUILD` before either per-arm
marker, so a hand-forced environment cannot arm an arm the library lacks. I also confirmed the
default the `Handles` lane depends on exists: `ConfigLoader.cpp:254` is
`QueryEnvUint64("MOBILEGL_PIPE_PUSH", MG_Pipe::kMGPipeSubsystemsMigratedAtP2)` under
`#if MOBILEGL_PIPE_PUSH` and `MGPipe.h:85` is `0x7full`, so `MOBILEGL_PIPE_LEGACY_MEMOS=0` with no
bitmask set is **not** the D14 `Fatal{PipeLegacyMemosDisabled}` condition in a push build.

**v1 MAJOR 2 (G12 armed off a file package B does not create).** Fixed at `07a7aa89` by a content
probe (`MG_IntegrationTest/CMakeLists.txt:393-412`). Verified against package B as it stands:

```
$ grep -rn 'RenderStateCsoMints\|RenderStateCsoBinds' ~/w7/p2-tracker/MobileGL/MG_Impl/Pipe/
…/CsoCache.h:162:   MG_Util::PipeStats::AddCalls(…::CallClass::RenderStateCsoMints, 1);
…/PipeFill.cpp:858: MG_Util::PipeStats::AddCalls(…::CallClass::RenderStateCsoBinds, 1);
```

Both are real emitter calls, not comments, and both are under the globbed directory, so
`MGITEST_PIPE_TRACKER_PRESENT` will arm when B lands. The `cso[csom= csob=]` bracket the scenario
parses is real and is `#if MOBILEGL_PIPE_PUSH`-guarded (`PipeStats.cpp:422-428`), which is what the
scenario's skip text claims.

Both fixes are correct. Neither is the major below.

---

## MAJOR — `DriverBenchStateToggle` is a new always-on gate that cannot go red for the reason it exists

`MobileGL/MG_Benchmark/Driver/CMakeLists.txt:33-34`:

```cmake
add_test(NAME DriverBenchStateToggle COMMAND DriverBench mc_state_toggle)
set_tests_properties(DriverBenchStateToggle PROPERTIES LABELS benchmark)
```

and `:31` states what it is for: *"the ctest entry is a 'does this case still run' gate, not the
measurement"*. Commit `718818f1`'s message states the same claim more strongly: *"The case has
existed in kBenchCases since P0 and nothing ran it, so nothing noticed if it broke."*

It cannot answer that question. `MobileGL/MG_Benchmark/Driver/DriverBench.c:483-500`:

```c
    for (int i = 0; i < kBenchCaseCount; ++i) {
        const BenchCaseDesc* c = &kBenchCases[i];
        if (argc > 1) {
            int wanted = 0;
            for (int j = 1; j < argc; ++j)
                if (strcmp(argv[j], c->name) == 0) wanted = 1;
            if (!wanted) continue;                       /* :489 */
        }
        …
        run_case(c->name, c->fn, a, c->b, ops);          /* void */
    }
    return 0;                                            /* :500 — unconditional */
```

A name that matches nothing in `kBenchCases` selects no case, runs nothing, and `main` still
returns 0. `run_case` is `void`, so a case that runs and produces nonsense also returns 0. The only
thing the entry can fail on is `boot_egl()` (`:479`) — i.e. it is an EGL smoke test wearing a
blend-toggle label.

**Reproduced** (`~/w7/p2-gates`):

```
$ ./build-bench/MobileGL/MG_Benchmark/Driver/DriverBench zzz_not_a_case > /tmp/db.txt 2>&1; echo rc=$?
rc=0
$ tail -2 /tmp/db.txt
version:  4.6 (Core Profile) Mesa 26.1.4-arch1.1
case,frames,ops_per_frame,median_frame_ms,ns_per_op,fps          <- the header, and nothing else

$ ./build-bench/MobileGL/MG_Benchmark/Driver/DriverBench mc_state_toggle | tail -1
mc_state_toggle,120,46,2.353,51145.2,425.0                        <- the healthy run, same rc 0
$ ctest --test-dir build-bench -R DriverBench --no-tests=error | tail -3
100% tests passed, 0 tests failed out of 2
```

So rename `mc_state_toggle` in `DriverBenchCases.inc`, or drop it from `kBenchCases`, and
`DriverBenchStateToggle` stays green while measuring nothing — the exact state the entry was added
to end. `ROADMAP.md:7`, quoted verbatim in brief §A as a P2 rule, is
**每个门必须能因它存在的理由变红**; this one cannot, and it is a gate this package introduces, in the
`benchmark` CI job, in the same commit that claims it closes the hole.

This also matters beyond hygiene: D.4.4 makes `mc_state_toggle` the GO/NO-GO's blend-toggle number
(`ROADMAP.md:51`), and the stated reason for wiring the ctest entry is that the number should come
from *"a case CI has been executing all along rather than from a code path whose first run is the
day it is measured"* (`CMakeLists.txt:26-28`). With the entry as written, CI executing the case is
not something a green tells you.

The sibling `add_test(NAME DriverBench COMMAND DriverBench draw_tiny)` (`:14`) has the identical
weakness and predates P2 — but the sibling never claimed to be a "does this case still run" gate,
and it is not being landed as part of an acceptance gate this week. A one-line
`PASS_REGULAR_EXPRESSION`/`FAIL_REGULAR_EXPRESSION` on the case's own CSV row would close it for
both. Not applied — this review changes nothing.

---

## Minors

1. **`g7_negative_control.sh` reports success when the control trips for the wrong reason.**
   `scripts/g7_negative_control.sh:219-225` greps the failing ctest log for `SetColorMask`; if it
   is absent the script prints *"tripped, but its output does not name SetColorMask"* on stderr and
   then falls straight through to `restore` and `exit 0` (`:227-242`). The integrator's D.3 runs
   `bash scripts/g7_negative_control.sh build-push` and reads the rc, so a `SetterConsistency` that
   had started failing for an unrelated reason would be recorded as "the negative control passed".
   Brief G7 does define the pass as a non-zero ctest rc, so this is not a deviation — but the script
   already knows the difference and throws the knowledge away in its exit status.
2. **The magma capability probe is still a single hard-coded *file* probe**, while the CSO probe
   next to it was widened to a content probe over a directory for exactly this reason.
   `MG_IntegrationTest/CMakeLists.txt:416-435` reads only
   `MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.cpp`. Package D already puts one of its
   two `Features.PipeHandleAbaControl` consumers outside that file
   (`~/w7/p2-magma/…/Renderer/VulkanRenderer.cpp:3691`,
   `const Bool compareLifetimeId = !MG_Config::Features.PipeHandleAbaControl;`) and one inside it
   (`…/VertexInputStateFactory.cpp:66`), so the marker arms today — but the CSO probe's own comment
   ("the owning package keeps control of its file layout") applies here word for word and was not
   acted on. One D-side refactor that moves `:66` is a permanent skip of the AbaControl arm, i.e. a
   recurrence of v1's MAJOR 2 in the other half of the same block.
3. **`pipe-gates` is red on this branch alone** (declared D-7, unchanged):
   `python3 scripts/gen_pipe_dirty_surface.py --check` and `--self-test` both exit 2 here, because
   the flags are package B's. The warning *"do not run `gh workflow run test.yml` on a tree that has
   `gates` without `tracker`"* must reach the integrator's notes; D.1's order (`gates` last) fixes it.
4. **D.2's "重键前红" artefact is still not produced** (declared D-1). C.4 makes it E's *first*
   deliverable; it is genuinely unreachable from E's ownership set (the knob's consumer is
   `MG_Backend/DirectVulkan/**`, package D per C.5). The integrator must take
   `~/w7/p2-handlerecycle-before.log` after D lands the knob and before D enables its re-key, and
   must not read E's merge as having produced it.
5. **G1's resized set is four symbols, not D15's three** (`_GLOBAL__sub_I_DirectGLES.cpp` −9),
   confirmed. Inherited from `9c6a8a25`; E builds no library source. Declared C-2, but G1 as written
   is already violated on the integrated tree and only the integrator can reconcile the brief.
6. **The new CI step's justification does not match the tree.** `.github/workflows/test.yml`'s
   `Unit tests on the verify runtime (G6, G10)` comment says G6/G10's tests are *"compiled only
   under MOBILEGL_PIPE_PUSH"* and *"before P2 those tests ran in no CI job at all"*. In package A's
   tree the suites are **registered in the pull build too** — `ctest --test-dir
   ~/w7/p2-spans/build-linux -N -R RenderStateSpans` names the same 11 entries as `build-push` —
   and each case is `#if MOBILEGL_PIPE_PUSH … GTEST_SKIP() << "push not compiled in"` inside the
   body (`~/w7/p2-spans/MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp:230-232, 324-327, …`). The
   step's *value* is real (the `test` job runs 11 skips; this step is the only CI place they
   assert), but the mechanism the comment describes is not the one in the tree, and a reader
   auditing "does CI run G6" will look for a registration that is not missing.
7. **The verified-profile guard reads the process environment as well as the profile.**
   `tools/device_bench/bench.sh:76` and `session.sh:59` are `[ "${PROFILE_VERIFIED:-0}" = "1" ]`,
   evaluated *after* `. "$DEVICE_ENV"`, so an operator with `PROFILE_VERIFIED=1` exported in their
   shell re-opens the fail-open hole for every profile that does not set the key. The round's whole
   point was to fail closed; `[ "$(. ...; echo "${PROFILE_VERIFIED:-0}")" ]` or an explicit
   `PROFILE_VERIFIED=0` before the source would close it.
8. **A profile path that cannot be sourced is diagnosed as an unverified profile.**
   `bench.sh:59` / `session.sh` do `. "$DEVICE_ENV"` with no `[ -r ]` check and no `|| exit`, and
   both scripts `cd "$(dirname "$0")"` first (`bench.sh:25`), so
   `bash tools/device_bench/bench.sh --device tools/device_bench/devices/odinlite.env --backend gl`
   from the repo root prints `bench.sh: line 59: …: No such file or directory` and then the new
   *"does not carry PROFILE_VERIFIED=1"* refusal — which names the wrong problem. The new default of
   0 is what makes this safe rather than catastrophic; it is now only a misleading message.
9. **The `AbaControl` arm can red an always-on `-L integration-gpu` lane for allocator reasons.**
   `HandleRecycleScenario.cpp:475-484` skips only when the GL *name* is not recycled, then asserts
   the **stale** pixels; the corruption it asserts needs the *heap address* to be reused, which the
   name recycle only proxies. The file documents this as intended (*"if the reproducer ever stops
   reproducing the ABA, this arm fails"*, `:53-57`), but the consequence — a required-green CI lane
   whose verdict depends on the allocator — is not written down next to the skip logic, and the
   `ObjectLifetimeIdTest` precedent the header cites is a *skip*, not a failure.
10. **The `-j 4` `integration-gpu` flake** is now demonstrated pre-existing (§3.2's control run
    excluded E's 34 entries and still reproduced three arming-lane failures) but remains latent, in
    three scenarios E does not own; CI is safe only because `test.yml:293-298` runs the label
    serially. Worth a follow-up of its own, as the result file says.
11. **`Begin()` takes the CPU baseline before the wall baseline** (`trace_benchmark.cpp:79`,
    `gLastBoundaryCpuMs = ThreadCpuMs();` ahead of `gStart`/`gLastBoundary`), which inflates frame
    0's CPU delta relative to its wall delta — the opposite of the ordering rule the file states for
    `OnFrameBoundary` (`:105-108`). Sub-microsecond, and frame 0 is outside the tail window; noted
    only because the file makes a point of the ordering.
12. **`<ctime>` rather than `<time.h>` for `clock_gettime` / `CLOCK_THREAD_CPUTIME_ID` /
    `struct timespec`** (`trace_benchmark.cpp:8-16`). Works on glibc and bionic, which is where this
    runs; the POSIX names are only guaranteed by `<time.h>`. The Android NDK build is
    compile-unverified (declared §8), so this is the one place that would show up there.

I verified the host-side statistics fix directly rather than from the transcript: loading
`tools/trace_replay/run_android_retrace_local.py` and calling its helpers gives
`series_median([1,2,3]) = 2`, `series_median([1,2,3,4]) = 2.5`, `series_median([]) = -1.0`,
`nearest_rank_percentile(1..100, 0.95) = 95`, `nearest_rank_percentile(1..100, 0.99) = 99`, and
`format_benchmark` on a synthetic report prints
`… | cpu mean=2.500ms p50=2.500ms p95=4.000ms p99=4.000ms` and, with `frameCpuTimesMs` removed,
`… | cpu unavailable (no per-thread CPU clock in this run)`. That is `SummarizeSeries`'
even-count rule transcribed correctly (`trace_replay_core.cpp:854-856`), so v1's minor 1 is fixed.
`cpu_tail` falls back to the whole series when `tailFrames` is absent or larger than the series,
which cannot disagree with the device because the device mins `tail` against the frame count.

---

## Gates I could not evaluate here, and why

* **G3 / G4** — E builds no library source, so a 40-trace SSIM run and a verify-divergence run
  would be measuring `9c6a8a25`. Integrator's D.3, correctly declared.
* **G6 / G10** — packages A and B own those tests; on this tree they are the contract's
  placeholders (`RenderStateSpans.PlaceholderUntilTheOwningPackageFillsThisIn`, and three
  siblings), which is why `ctest -L unit` is 1489 in all three build directories and why
  `g7_negative_control.sh build-push` correctly refuses with rc 2.
* **G7's real path** — I confirmed every patch anchor resolves uniquely against
  `~/w7/p2-spans@7c2c1456` and that the demotion is a genuine invariant break (§0), but I did not
  re-apply package A's diff into this tree: doing so would have rebuilt `build-push` and left the
  tree momentarily dirty, and I was asked to leave it exactly as found. The end-to-end red is
  therefore still the implementer's evidence, not mine; the integrator's D.3 run will repeat it.
* **G11 / D.4.2 / D.4.3 / D.4.5's device arms** — two devices, no lock taken by this package or by
  this review.
* **CI** — no workflow run. `pyyaml` is not installed in this WSL, so `test.yml` was not
  machine-parsed; the three new steps' indentation and `env:` blocks match their siblings in the
  same job by inspection.

---

## Tree state

Left exactly as found. `git status --porcelain` in `~/w7/p2-gates` reports only `?? build-retrace/`,
before and after. `~/w7/p2-spans`, `~/w7/p2-tracker`, `~/w7/p2-magma`, `~/w7/p2-espryt` and
`~/w7/p2-contract` were read only (`git log`, `grep`, `ctest -N`); no test was executed in any of
them and nothing was written. The packaging simulation wrote only to `/tmp/pkgsim`.
