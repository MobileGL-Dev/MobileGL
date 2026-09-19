# P1 package E (p1-verify) — the third CI mode, its lanes and its negative controls

Tree `~/w7/p1-verify`, branch `p1/verify`, base `p1/contract` = `bf86b1ed` (`feat/disaggregated@087685d1` + package A's contract commit). Created with `wsl_p1_tree.sh verify p1/contract verify`; `build-linux` / `build-push` / `build-verify` all configured and green at the base (955/955, 959/959, 959/959). Not pushed.

## Commits (4, on `p1/verify`)

| sha | subject |
|---|---|
| `62783e4f` | `[Test] (Pipe): the integration-verify lanes and their two always-on negative controls` |
| `c4170e56` | `[Test] (Retrace): make a retrace under MOBILEGL_PIPE_VERIFY prove it armed, and give the lane a label` |
| `09804491` | `[Feat] (Tooling): give symbol_report.py the two hard gates G1 needs, with the report written first` |
| `1a5252b6` | `[CI] (Pipe): the third CI mode - a verify build, its integration and retrace lanes, and the two negative controls as always-on steps` |

`git diff --stat p1/contract..HEAD`: 9 files, +1498 / −12 — `.github/workflows/test.yml` (+574), `MobileGL/MG_IntegrationTest/CMakeLists.txt` (+134), `Scenarios/PipeVerifyArmingScenario.cpp` (new, 243), `Scenarios/PoisonOmissionScenario.cpp` (new, 395), `scripts/symbol_report.py` (+66/−12), `tools/trace_replay/{CMakeLists.txt, run_trace_case.cmake, trace_cases.json, trace_cases.py}`. Nothing outside E's row of the C.5 ownership table was touched; `git status` clean apart from the three untracked build directories and `build-retrace/` (configured for the label check below).

## What landed

**v1 — the lanes.** Six `gtest_discover_tests` registrations inside one `if (MOBILEGL_PIPE_VERIFY)` block at the end of `MG_IntegrationTest/CMakeLists.txt`, all labelled `integration-gpu;integration-verify`, all `TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}` (900): ambient `DirectGLES.Verify.` / `DirectVulkan.Verify.` (`MOBILEGL_PIPE_VERIFY=1` + a per-lane `MOBILEGL_LOG_FILE_PATH`), `*.VerifyCorrupted.` (adds `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters MOBILEGL_PIPE_VERIFY_FATAL=0`, filter `PipeVerifyArmingScenario.CorruptedFieldIsReported`), `*.PoisonOmitted.` (adds `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit`, filter `PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb`). Every list appends `${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}`. Two new scenario files, added unconditionally to `add_executable`.

**v2 — retrace.** `run_trace_case.cmake` gains a verify block (arming line present; `Fatal{PipeVerifyDiffer` and `Fatal{UnmigratedPipeInput` absent; log must exist), inert unless `MOBILEGL_PIPE_VERIFY` is set to something other than `0`/`false`. `LABELS retrace` on both `set_tests_properties`. `"verify": true` on eight cases and `--format github-verify-matrix`.

**v3 — `symbol_report.py`.** `--fail-on-added-bytes N` implemented, `--fail-on-symbol-set-change` added, decision isolated in a pure `gate_failures()` the `--self-test` drives, report written before any gate fires, default behaviour (no flag → exit 0) unchanged.

**v4 — `test.yml`.** `build-linux-verify`, `integration-verify` (+2 always-on negative-control steps + an arming-line grep over the lane logs), `retrace-verify` (+ an inverted OpenRA step), `monolith-symbol-report` (dispatch-only, `baseline_sha` input, both gates, MG_Remote refusal), `remove-artifact-clutter` waits for `retrace-verify`, `trace-cases` exports `verify-matrix`, `pipe-gates` gains `gen_pipe.py --self-test` and `symbol_report.py --self-test` and its dirty-surface comment now says P2.

## Verification — every command and its actual output

All in WSL on `~/w7/p1-verify` unless marked. Logs under `~/w7/p1-verify-*.log` / `~/w7/p1-verify-*.txt`.

| # | command | result |
|---|---|---|
| 1 | `python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))" test.yml.copy` (**Windows python**, on a copy) | parses; 17 jobs. `build-linux-verify` timeout 120 / 15 steps; `integration-verify` needs `build-linux-verify`, timeout 180, 12 steps; `retrace-verify` needs `[build-linux-verify, build-retrace, trace-cases, trace-fixtures]`, timeout 240, 13 steps; `monolith-symbol-report` timeout 180, 11 steps; `remove-artifact-clutter` needs `['retrace-summary','retrace-verify']`; dispatch inputs `['baseline_sha']`; `trace-cases` outputs `['matrix','names','verify-matrix']`; `pipe-gates` steps now include both `--self-test`s. The file's UTF-8 BOM is preserved byte for byte (`efbbbf 6e616d65...`). |
| 2 | `trace_cases.py --ci --format github-verify-matrix` | **16** entries (8 cases × 2 backends), first three `OpenRA/DirectGLES`, `OpenRA/DirectVulkan`, `minecraft-1.21.4-startup/DirectGLES`. `--format github-test-matrix` still **77**; `--format cmake` and `--format names` unchanged in content (the `verify` key is not one of the emitter's keys). |
| 3 | `python3 scripts/symbol_report.py --self-test` | `self-test: OK (2 canned transcripts, 5 buckets, 2 gates)`, rc 0. |
| 4 | `cmake --build build-{linux,push,verify} -j 12` | rc 0, rc 0, rc 0. |
| 5 | `ctest -L unit --no-tests=error -j 12` | `build-linux` `100% tests passed … out of 1466`; `build-verify` idem. |
| 6 | `ctest --test-dir build-verify -N -L integration-verify \| grep -c 'Test #'` | **816** = 2 × 406 ambient `Verify.` + 2 `VerifyCorrupted.` + 2 `PoisonOmitted.` |
| 7 | `ctest -N -L integration-gpu` folded (`s/\.(Verify\|VerifyCorrupted\|PoisonOmitted)\./\./`, `sort -u`) | verify build 878 unique names, pull build 878 — `comm` empty in **both** directions: exact parity with the pull registration set. Raw counts: verify 1694, pull 878. |
| 8 | `ctest --test-dir build-verify -R 'DirectGLES\.Verify\.PipeVerifyArmingScenario' --output-on-failure` (REQUIRE_GPU=ON) | rc 8. `…Armed ***Failed` with "MOBILEGL_PIPE_VERIFY=1 is set for this process and a frame was cleared, drawn and read back, and the library never reported arming the comparator"; `…CorruptedFieldIsReported ***Skipped` (its knob is not set in that lane). **The arming case fails and never passes on the contract tree** — G8 half one. |
| 9 | `ctest --test-dir build-verify -R 'VerifyCorrupted\.\|PoisonOmitted\.' --no-tests=error` | rc 8, **4/4 red**, each naming its own reason ("the comparator is not comparing…", "It exited with status 0 instead - the poison is not armed…"). Correct for the contract tree: the comparator lands in core `c4`, the filler in `c2`. |
| 10 | `ctest --test-dir build-verify -R 'Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes'` | **2/2 pass** (0.48 s / 0.42 s) — the sibling control is green on both backends, so control B has a green entry to turn red. |
| 11 | `ctest --test-dir build-linux -N -L integration-verify` / `ctest --test-dir build-linux -L integration-verify --no-tests=error` | `Total Tests: 0`; rc **8**. The lanes exist only under the option, and `--no-tests=error` reds a build that lost it — G8 half two. |
| 12 | the two CI control steps, simulated locally in `build-verify` | control A selects **5** entries, control B selects **1**. With the knob exported in the job environment both still exit 0 on the contract tree, i.e. **the CI step would `exit 1` and say "the comparator is not comparing" / "the poison is not armed"** — correct, and the check that the knob reaches the process is below. |
| 13 | `grep -ho "MOBILEGL_PIPE_[A-Z_]*=…" build-verify/MobileGL/MG_IntegrationTest/*.cmake \| sort \| uniq -c` | `816 MOBILEGL_PIPE_VERIFY=1`, `2 MOBILEGL_PIPE_VERIFY_CORRUPT=…`, `2 MOBILEGL_PIPE_VERIFY_FATAL=0`, `2 MOBILEGL_PIPE_POISON_OMIT=…`. Only the four dedicated entries name the two control knobs, so the job environment reaches the 812 ambient ones — the ENVIRONMENT-property trap is avoided by construction and measured. |
| 14 | `run_trace_case.cmake`'s verify block, driven standalone against seven synthetic logs | env unset → silent, rc 0 · `=0` → "no verify assertions", rc 0 · armed log → "armed, zero divergences, zero unmigrated reads", rc 0 · **no arming line → rc 1** · **missing log → rc 1** · **`Fatal{PipeVerifyDiffer` → rc 1** (offending line echoed) · **`Fatal{UnmigratedPipeInput` → rc 1**. |
| 15 | `cmake -S . -B build-retrace -DMOBILEGL_BUILD_TRACE_REPLAY=ON …` then `ctest -N -L retrace` | configure rc 0; **80** cases listed under the new label (was: not selectable by label at all). |
| 16 | `symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | rc **0**. `.text 10792579 -> 10792579 (+0, +0.000%)`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, `27060 unchanged`. G1 holds — E adds no C++ to the pull build. |
| 17 | the same gates against `build-verify/libMobileGL.so` (a deliberately different library) | rc **1**, `FAIL --fail-on-symbol-set-change: 43 symbol(s) added, 0 removed`, `FAIL --fail-on-added-bytes 0: .text grew by 10416 bytes` — the gate can go red for its reason. Ungated run on the same pair: rc 0. |
| 18 | `python3 scripts/gen_pipe.py --check` | rc 0: `63 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes` / `0 UNMAPPED` / `generated files are up to date`. |
| 19 | `ctest --test-dir build-linux -N` vs `~/w7/p1-before-ctest-names.txt` | **removed 0**, added 10 — the four new scenario cases plus the child-worker case, × 2 backends. Additions only. |

## Deviations from the brief, and why

1. **`LABELS integration-gpu integration-verify` does not work; the list must be escaped.** With the brief's spelling (and with a plain `"a;b"`) the second label is lost: `ctest -N -L integration-verify` listed **0** entries while `-L integration-gpu` listed 1690. The file's own `mgl_itest_join_environment` documents the same hazard for `ENVIRONMENT`. Landed as `LABELS "integration-gpu\;integration-verify"`, measured 816.
2. **`set(MGL_ITEST_VERIFY_TIMEOUT 900)` and the whole block sit at the end of the file, not "after `:393`".** `gtest_discover_tests` needs `include(GoogleTest)`, which is at `:395`; a block at `:394` would not configure. The timeout is set inside the `if (MOBILEGL_PIPE_VERIFY)` block, next to the registrations that use it.
3. **The `PoisonOmitted.` lanes filter `PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb`, not `PoisonOmissionScenario.*`.** The brief says both, in the same row; the single-case spelling is what its own "+2 PoisonOmitted" count requires, and `*` would register a guaranteed skip beside it.
4. **CI negative control B targets `DirectGLES\.Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes`, not `DirectGLES\.Verify\..*Mipmap`.** Measured: **no** integration entry in the tree matches `Mipmap` (`ctest -N -R 'DirectGLES\.Verify\..*Mipmap'` → 0), and `glGenerateMipmap` appears in exactly one scenario file — the new one. With the brief's filter the step would select nothing, `--no-tests=error` would exit non-zero, and the `if ctest …; then error` shape would read that as "the control worked". **Both** control steps therefore now count their selection with `ctest -N` first and fail loudly on an empty one.
5. **`WithoutOmissionCompletes` does not skip when `MOBILEGL_PIPE_POISON_OMIT` is set** (C.4 says it should). It is the only entry control B can turn red; an entry that skips itself reports green either way. Its failure message says explicitly when the failure is the control doing its job.
6. **The poison sequence runs in `fork()` + `execve()` of the test binary, not a bare `fork()`.** With a bare fork (the brief's shape, following `HeadlessGL.cpp`) the DirectVulkan child wedged: `DirectVulkan.…WithoutOmissionCompletes` took **121.57 s** and timed out while DirectGLES passed in 0.33 s — the pre-flight forks *before* any context exists, this case cannot, and a fork of a process holding a live Vulkan device inherits driver mutexes with no threads to release them. After the change both backends pass in ~0.45 s. The child gets its **own** `MOBILEGL_LOG_FILE_PATH` (`<lane log>.poison-child`) because `MG_Util/Debug/Log.cpp` opens the log with `fopen(path, "w")` and a shared path would truncate the file the parent is about to read.
7. **The arming assertion searches the whole log file**, not only the bytes appended after the case started (the `UnlocatedIoBlockScenario` idiom the brief cites). Two tree facts: the log is truncated at each process's first write (`fopen(…, "w")`), so the whole file *is* this process's; and the arming line is latched at the first fill, which may be the harness bring-up rather than the case's own draw. The divergence checks (`Fatal{PipeVerifyDiffer`, `Fatal{UnmigratedPipeInput`) stay scoped to the appended bytes.
8. **A third case, `PoisonOmissionScenario.TheSequenceThePoisonControlsRun`**, is the child's body (it skips in every process without the re-exec marker). Hence 10 new ctest names in the pull build rather than 8.
9. **`trace_cases.py` has no "existing self-checks"** to extend (the brief's C.4 wording). The `verify` flag is validated in `load_trace_case_manifest` instead — non-boolean, or `verify` on a case excluded from CI, is a `ValueError` in every consumer including the cmake emitter.
10. **`--fail-on-added-bytes N` is defined as ".text grew by more than N"**, and `--fail-on-symbol-set-change` as "added or removed is non-empty" (renamed-only folds and resized do not count; `--threshold` already governs resizes).
11. **`retrace-verify` artifacts are named `retrace-verify-result-*`**, which `remove-artifact-clutter`'s `retrace-result-*` sweep does not match, so a failed verify retrace keeps its images. The brief only asked for a `-verify` suffix.
12. **`build-linux-verify` has no `needs:`** (it is a copy of `build-linux`, which has none) and builds with `MOBILEGL_BUILD_BENCHMARK=OFF` — the verify lanes need no benchmark target.
13. **`monolith-symbol-report`** builds the baseline in a `git worktree` of `inputs.baseline_sha` (so `fetch-depth: 0` on the checkout) and spells the MG_Remote check as `if nm … | grep -q MG_Remote; then exit 1; fi` rather than the brief's `grep -q … && exit 1`, which under GitHub's `bash -e` fails the step on the *healthy* path.

## Where the tree contradicted the brief's numbers

| brief | tree (measured) |
|---|---|
| "the 742-entry `integration-verify` lane"; "752 = 2 × (371 + 3 new cases) + 2 + 2" | 406 cases per backend → **816** entries (812 ambient + 4 controls). The pull build's `integration-gpu` set is **878**, not 742. |
| C.4: `ctest -N -L integration-gpu \| sed 's/\.Verify\././' \| sort -u` "parity with the pull registration set" | holds exactly once `VerifyCorrupted.`/`PoisonOmitted.` are folded too: 878 = 878, `comm` empty both ways. |
| "40 trace + 79 desktop cases"; D.4 "retrace (77)" | `--ci --format github-test-matrix` = **77** entries (39 CI cases × 2 minus the DirectVulkan-only `iterationrp`). The verify subset is 16, as predicted. |
| `MGPipeHostSpan.h`, `MGL_ITEST_TIMEOUT` etc. line numbers | `set(MGL_ITEST_TIMEOUT 120)` is at `:393` and `include(GoogleTest)` at `:395`, as the brief says; `run_trace_case.cmake` is 129 lines, as the brief says. |
| brief's ctest-name regex `'^\s+Test #'` | matches only test numbers ≥ 1000 (ctest pads smaller ones); every count above uses `grep -cE '^ *Test *#[0-9]+:'`. Package A recorded the same. |

## Unfinished / known dependencies on package A

- **`nm -D … | grep -q MGPipeVerifyInputs`** (in `build-linux-verify`'s "The verify library really carries the comparator" step and in `retrace-verify`'s unpack step) is the contract name from D8/D13 and **does not exist until core `c4`**; the contract tree exports only `MGPipeInputsFieldEqual` / `MGPipeApplyVerifyCorruption`. Both steps will fail in CI until `p1/core` c4 lands. Deliberate: the check is the point.
- **`pipe-gates` runs `python3 scripts/gen_pipe.py --self-test`**, which core `c5` adds (brief C.1 step 5). The step is red until then.
- **The four control lanes and `PipeVerifyArmingScenario.Armed` are red on this tree by design** (no filler, no comparator). They go green with core `c2`+`c4` merged; the brief defers E's full functional verification to the integrated tree (D.3).
- **Not run here:** the full `ctest -L integration-verify` (816 entries; the ambient `Armed` entries are red by design), any retrace under `MOBILEGL_PIPE_VERIFY` (no trace-replay binary or fixtures budget in this tree — the gate was driven against seven synthetic logs instead), and any CI dispatch (no push).
- **`build-retrace/`** was configured in this tree only to prove the `retrace` label registers; it is untracked and no retrace binary was built.
