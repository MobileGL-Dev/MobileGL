# Adversarial review — package E `p1-verify` (v1)

Tree `~/w7/p1-verify`, branch `p1/verify` = `1a5252b6`, four commits on `p1/contract` (`bf86b1ed`). Tree left exactly as found (only the untracked `build-*` directories the implementer created; no file touched). Scripts and raw outputs: `scratchpad/wf4/verify-review/s0*.sh`, `~/w7/p1-verify-review/`.

**Verdict: NOT approved — 3 majors.** The package's mechanical gates all hold (G1 0/0/0/0 +0, G2 untouched, G7 additions only, `gen_pipe --check` clean, unit 1466/1466 in all three builds, lane counts as claimed, `trace_cases.py` formats byte-identical to the contract, YAML parses, the `run_trace_case.cmake` block does what it says on synthetic logs). What fails is the CI mode itself: two of its always-on steps cannot pass on the integrated tree for reasons unrelated to the comparator, and the G1 job's gate is weaker than G1's check column.

## 1. What was re-run independently (command → observed)

| # | command (in `~/w7/p1-verify`) | observed |
|---|---|---|
| 1 | `git diff --name-only p1/contract..HEAD` | the 9 files of the result file; every one is in E's row of C.5. Nothing under `MG_Backend/**`, `MG_Test/**`, root `CMakeLists.txt`, `MG_Pipe/**` touched. |
| 2 | `grep -rc pGLContext MobileGL/MG_Backend \| sort \| diff - ~/w7/p1-before-pglcontext.txt` | only the two `:0` lines for `MG_Backend/MGPipe/PipeInputs.{h,cpp}` (files the contract commit created after the baseline was captured; both 0). E converts no site and touches no site. |
| 3 | `python3 scripts/gen_pipe.py --check` | rc 0, `63 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `0 UNMAPPED`, up to date. |
| 4 | `cmake --build build-{linux,push,verify} -j 12` | `ninja: no work to do` ×3 (the trees were current). |
| 5 | `symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | rc 0; `.text 10792579 -> 10792579 (+0)`, `27799 -> 27799: 0 added, 0 removed, 0 resized, 0 renamed`. G1 holds for E. |
| 6 | `ctest -L unit --no-tests=error -j 12` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1466` ×3. |
| 7 | `ctest --test-dir build-linux -N` vs `~/w7/p1-before-ctest-names.txt` | removed 0, added 10 (the five new cases × 2 backends). |
| 8 | `ctest --test-dir build-verify -N -L integration-verify` | 816; `VerifyCorrupted.`/`PoisonOmitted.` = #3157-3160; control-A filter selects 5. |
| 9 | `ctest --test-dir build-linux -L integration-verify --no-tests=error` | `Total Tests: 0`, rc 8 (G8 half two holds). |
| 10 | `ctest --test-dir build-retrace -N -L retrace` | 80. |
| 11 | `grep -ho "MOBILEGL_PIPE_[A-Z_]*=…" build-verify/MobileGL/MG_IntegrationTest/*.cmake \| uniq -c` | 816 `PIPE_VERIFY=1`, 2 each of `CORRUPT`, `FATAL=0`, `POISON_OMIT` — the ambient entries name neither control knob. |
| 12 | `trace_cases.py --format {cmake,names,fixture-files,github-test-matrix}` HEAD vs contract, same directory | all four byte-identical; `github-verify-matrix` = 16 (OpenRA first), `github-test-matrix` = 77. |
| 13 | `python3 scripts/symbol_report.py --self-test` | `OK (2 canned transcripts, 5 buckets, 2 gates)`. |
| 14 | Windows `yaml.safe_load` on a copy (BOM kept) | 17 jobs; `retrace-verify` needs/if as stated; `remove-artifact-clutter` needs `[retrace-summary, retrace-verify]`, `if: always()`; dispatch input `baseline_sha`; `trace-cases` outputs incl. `verify-matrix`; `pipe-gates` has both `--self-test` steps. Job-level diffs of `build-linux-verify`/`retrace-verify`/`integration-verify` against their originals show only the declared changes plus the two items in minors 5 and 6. |
| 15 | `run_trace_case.cmake:150-186` extracted and driven with `cmake -P` against 9 synthetic logs | unset→0, `=0`→0, armed→0, no-arming→1, missing log→1, `Fatal{PipeVerifyDiffer`→1, `Fatal{UnmigratedPipeInput`→1, `=true` armed→0, `Fatal{PipeVerifyBadKnob}`→0 (see minor 7). |
| 16 | `ctest --test-dir build-verify -R 'DirectGLES\.Verify\.PipeVerifyArmingScenario\|VerifyCorrupted\.\|PoisonOmitted\.\|Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes'` | `Armed` ***Failed, `CorruptedFieldIsReported` (ambient) ***Skipped, both `WithoutOmissionCompletes` Passed (0.43/0.45 s), the four control entries ***Failed — exactly the contract-tree shape the result file reports. |
| 17 | `python3 scripts/gen_pipe.py --self-test` | `unrecognized arguments: --self-test`, rc 2 — the declared dependency on core c5. |

## 2. Majors

### M1 — Both `nm -D` gates can never pass: the Release library exports no MGPipe symbol

`test.yml:393-394` (`build-linux-verify`, "The verify library really carries the comparator") and `test.yml:1161` (`retrace-verify`, "Unpack the VERIFY runtime") do `nm -D --defined-only libMobileGL.so | grep -q MGPipeVerifyInputs` / `MGPipeFillForVerb`. The library is built with `CXX_VISIBILITY_PRESET hidden` for every non-Debug configuration (root `CMakeLists.txt:600-604`), and the MGPipe functions are plain namespace functions with no export attribute, so they are local (`t`) symbols and absent from the dynamic table. Reproduced on this tree with the symbol that already exists:

```
$ nm -D --defined-only build-verify/libMobileGL.so | grep -c MGPipeFillForVerb
0
$ nm --defined-only build-verify/libMobileGL.so | grep MGPipeFillForVerb
0000000000648d80 t _ZN8MobileGL7MG_Pipe17MGPipeFillForVerbENS0_10MGPipeVerbE
$ nm -D --defined-only build-verify/libMobileGL.so | grep -c MGPipe      # 0 of 11930 exported
0
```

Consequence: `build-linux-verify` fails at its check step on every run, so `integration-verify` (needs it) never runs and every `retrace-verify` matrix entry is skipped by its `if:`. The result file attributes the red to "`MGPipeVerifyInputs` does not exist until core c4"; that is not the failure mode — `MGPipeFillForVerb` exists today and is not found either, and c4 will not change visibility. The fix is one of: `nm --defined-only` (static symtab; the runtime artifact is not stripped, and the job's own MG_Remote check at `test.yml:1301` already uses that spelling), or an exported probe symbol. The step's comment ("a typo'd -D is not an error in CMake") is right about the need; the spelling defeats it.

### M2 — "Every verify process really armed" (`test.yml:504-517`) reds a healthy lane, and checks one process, not every process

The step greps `pipe-verify-*.log` for the arming line after the whole lane ran. Each ambient lane shares ONE log path (`MG_IntegrationTest/CMakeLists.txt:670,674`), and the library opens it with `fopen(path, "w")` at each process's first write (`MG_Util/Debug/Log.cpp:73`), so after 406 serial processes the file holds only the LAST one. The last registered entry of each ambient lane is `PoisonOmissionScenario.WithoutOmissionCompletes` (ctest #2750 / #3156, from `ctest -N -L integration-verify`), and that parent process never reaches a verb: its body is `Ready()` → `fork()+execve()` → `waitpid` → read files (`PoisonOmissionScenario.cpp:344-380`); the fixture `SetUp` brings up a context via `RunEglBringUp` (`HeadlessGL.cpp:205-297`: EGL + `glGetString`), which contains no `GLFunctionsTable` verb; the child writes its own `.poison-child` file. Arming is latched at the first fill (brief D8), so this process's log carries no arming line by construction.

Reproduced on this tree:

```
$ ctest --test-dir build-verify -R '^DirectGLES\.Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes$'   # rc 0
$ wc -l < build-verify/MobileGL/MG_IntegrationTest/pipe-verify-DirectGLES.log
317
$ head -1 …/pipe-verify-DirectGLES.log ; tail -1 …/pipe-verify-DirectGLES.log
[…] Initializing MobileGL...
[…] MobileGL closing...
```

(317 lines of init/teardown; no verb, no fill.) On the integrated tree, therefore, `grep -q "MGPipe: verify armed"` on this file fails and the step exits 1 — a red for a non-reason, immediately after a green 816-entry run. Even with a different last entry the step proves only "the last process armed", not "every process" (the name and the `::error::` text both claim the latter); the per-process proof is `PipeVerifyArmingScenario.Armed`, which already exists. Either drop the step or make it honest (e.g. count processes that wrote, or give the arming entry its own log path and grep that).

### M3 — The G1 gate (`symbol_report.py:244-257`) passes a shrunk `.text` and resized symbols; G1's check column requires all four buckets and the delta to be zero

Brief A/G1: "`added == removed == resized == renamed == 0`, `.text` delta 0". `gate_failures` has no `resized` (or `renamed`) input at all, and `--fail-on-added-bytes N` fires only on `text_delta > N`:

```
$ python3 -c "import sys; sys.path.insert(0,'scripts'); import symbol_report as s
print(s.gate_failures([], [], -4096, 0, True)); print(s.gate_failures.__code__.co_varnames[:5])"
[]
('added', 'removed', 'text_delta', 'fail_on_added_bytes', 'fail_on_symbol_set_change')
```

So `monolith-symbol-report` with `--threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` (`test.yml:1306-1314`) is green for a P1 tree whose pull `.text` shrank, or whose functions were resized with zero net delta (the exact shape the D9 guard rewrites can produce: "if `.text` moves, this is the first suspect"). Deviation 10 says "`--threshold` already governs resizes" — it governs only the report's bucket (`bucket(..., args.threshold)`), never the exit code. A one-line fix each: `text_delta != 0` under `--fail-on-added-bytes 0` (or a `--fail-on-text-delta`), and `resized` in the set-change gate; the self-test then needs the corresponding canned cases. (With `--strip-scope`, renames are additionally folded out of `added`/`removed` at `:193` and escape the gate; the CI invocation does not pass it, so that is a documentation hole only.)

## 3. Minors

1. **The re-exec'd child discards its own assertions.** `PoisonOmissionScenario.cpp:274-281`: `RunSequence(); … _exit(0);` unconditionally. If `ASSERT_NE(program, 0u)` (`:142`) or any earlier fatal assertion fires, `RunSequence` returns before the draw and the mipmap, the child exits 0, and `WithoutOmissionCompletes` passes without the sequence having run (the `"Fatal{"`-absent check is satisfied by any log). Fails safe for CI control B (it would go red), but the G5 sibling entry can pass vacuously. `_exit(::testing::Test::HasFailure() ? 1 : 0)` and a `glGetError()` after the mipmap close it.
2. **Shared per-lane log vs the brief's `-j 4`.** D.3 runs `ctest -L integration-verify -j 4`; with 405 siblings truncating `pipe-verify-<backend>.log`, `Armed`'s whole-file arming grep (`PipeVerifyArmingScenario.cpp:184`) can miss the line during a neighbour's bring-up window (false red), and `LibraryLogSince(before)` (`:192`) reads past EOF after a truncation (Fatal scan vacuous — harmless only because `FATAL=1` aborts anyway). The existing lanes that use this idiom are one-entry filtered lanes. `RUN_SERIAL`/`RESOURCE_LOCK` on the ambient registrations, or a dedicated arming lane with its own log, removes the race; at minimum the result file should tell the integrator to run the arming entries serially.
3. **`remove-artifact-clutter` sweeps a failed verify retrace's fixture.** `test.yml:1364` keeps `trace-fixture-*` only for jobs whose name `startswith("retrace (")`; `retrace verify (…)` failures are not consulted. The `needs:` was extended; the keep-on-failure filter was not (the commit message implies both).
4. **Retrace negative control overwrites the verified run's output.** `test.yml:1188-1200` reruns OpenRA into the same case directory; `Upload actual image` (`:1209`) then ships the corrupted run's images for OpenRA/DirectGLES.
5. **`integration-verify` drops the second `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` pass** the `integration` job runs (`test.yml:290-294`), so the staged-copy upload tier is never exercised under the comparator. Not required by C.4; worth a sentence in the docs so "every integration entry" is not read as "every configuration".
6. **`build-linux-verify` hardcodes Release** where `build-linux` switches to Debug under `ACTIONS_STEP_DEBUG` (the original's `BUILD_TYPE` block). Declared in the step comment and correct (a Debug build would flip visibility and the poison arm), but it is an undeclared difference from "copy of `build-linux:12-147`"; the "Show installed toolchain" step is also dropped.
7. **`run_trace_case.cmake:170-171` scans only `PipeVerifyDiffer|UnmigratedPipeInput`.** `Fatal{PipeVerifyBadKnob}` (a typo'd knob) is not in the regex; it is caught only because D2 makes it abort. Fine if that stays true; a comment would do.
8. **D.3's G5 line as written in the brief (`-R 'Mipmap'` without `--no-tests=error`) prints rc=0 on this tree** because nothing matches (`ctest -N -R Mipmap` → 0). Deviation 4 fixes CI but the result file does not hand the integrator the corrected local line (`-R 'DirectGLES\.Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes' --no-tests=error`).
9. Result-file wording: "`MGPipeVerifyInputs` … does not exist until core c4 … the check is the point" reads as if the check will turn green at c4; per M1 it will not.

## 4. Brief vs tree (facts recorded, tree wins)

- Lane size 816 (not 742/752), pull `integration-gpu` 878 — as the result file measured; the `comm` parity is trivially true (fold of `Verify.`/`VerifyCorrupted.`/`PoisonOmitted.`), not a statement about coverage.
- `LABELS "a\;b"` escaping, `include(GoogleTest)` at `:395`, `'^\s+Test #'` regex — confirmed as the result file states.
- `gen_pipe.py --self-test` does not exist on this tree (rc 2) — the `pipe-gates` step is red until core c5, as declared.
- `retrace` label count 80 (build-retrace registers the non-CI case pair too).

## 5. What was not re-run

The full 816-entry lane, any real retrace under `MOBILEGL_PIPE_VERIFY`, and any CI dispatch — same as the implementer, for the same reasons (no comparator on the contract tree; no push). M1 and M2 are demonstrable without them; M3 is a pure-function property.
