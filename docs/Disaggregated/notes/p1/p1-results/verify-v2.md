# P1 package E (p1-verify) — v2, the rework of the three majors and eight minors

Tree `~/w7/p1-verify`, branch `p1/verify`, base `p1/contract` = `bf86b1ed`. Four **new** commits on top of
v1's four; nothing rebased, nothing pushed. `git status` clean apart from the four untracked build
directories (`build-linux`, `build-push`, `build-verify`, `build-retrace`).

Review answered: `scratchpad/wf4/p1-results/verify-review-v1.md`. Scripts and raw outputs of this round:
`scratchpad/wf4/verify2/s0*.sh` … `s19.sh`.

## Commits

| sha | subject |
|---|---|
| `62783e4f` | `[Test] (Pipe): the integration-verify lanes and their two always-on negative controls` (v1) |
| `c4170e56` | `[Test] (Retrace): make a retrace under MOBILEGL_PIPE_VERIFY prove it armed, and give the lane a label` (v1) |
| `09804491` | `[Feat] (Tooling): give symbol_report.py the two hard gates G1 needs, with the report written first` (v1) |
| `1a5252b6` | `[CI] (Pipe): the third CI mode - a verify build, its integration and retrace lanes, and the two negative controls as always-on steps` (v1) |
| **`6060690a`** | `[Test] (Pipe): give the arming assertion a lane and a log of its own, and stop the poison child calling a sequence it never ran a success` |
| **`01300e6a`** | `[Fix] (Tooling): gate symbol_report on all four buckets and on a .text that moved in either direction` |
| **`ce26029c`** | `[Test] (Retrace): scan a verify retrace's log for the third MGPipe Fatal too` |
| **`eec8cf2c`** | `[CI] (Pipe): read the verify library's static symtab, grep only the arming lane's log, and stop the two retrace lanes overwriting each other's evidence` |

`git diff --stat p1/contract..HEAD`: 9 files, +1738 / −14 — the same nine files as v1 (E's row of C.5); no file
outside it was touched in either round.

## What each finding got

### M1 — `nm -D` can never find an MGPipe symbol (fixed, `eec8cf2c`)

Confirmed on this tree, and the reviewer's diagnosis is exact:

```
nm --defined-only build-verify/libMobileGL.so | wc -l      -> 30557
nm -D --defined-only build-verify/libMobileGL.so | wc -l   -> 11930
MGPipeFillForVerb    static:1   dynamic:0
MGPipeVerifyInputs   static:0   dynamic:0      (does not exist until core c4)
```

Both gates (`build-linux-verify` "The verify library really carries the comparator", `retrace-verify`
"Unpack the VERIFY runtime as the library under test") now use `nm --defined-only`, name in their error
text what a miss means, and the build-side one first counts the defined symbols and refuses an artifact
that looks stripped — otherwise a stripped library would fail the greps for a third, silent reason. The
comment at each site says why `-D` is wrong (hidden visibility in every non-Debug configuration,
`CMakeLists.txt:600-604`), so nobody re-adds it. **The v1 result file's wording was wrong** and is corrected
below (see "Dependencies on package A"): `MGPipeFillForVerb` exists today and `-D` did not find it either.

### M2 — "Every verify process really armed" (fixed, `6060690a` + `eec8cf2c`)

Reproduced verbatim: after running the last ambient entry
(`DirectGLES.Verify.PoisonOmissionScenario.WithoutOmissionCompletes`, rc 0) the shared lane log is 317 lines
with **0** arming lines, because that parent forks/execve()s/waits and issues no verb. The step would have
red a healthy lane, and in any case spoke about the last writer.

Fix, in two halves:

* `PipeVerifyArmingScenario.Armed` moved into a lane of its own — `DirectGLES.VerifyArming.` /
  `DirectVulkan.VerifyArming.`, `TEST_FILTER PipeVerifyArmingScenario.Armed`, own
  `pipe-verify-arming-<backend>.log`, marker `MGITEST_PIPE_ARMING_LANE=1` (a harness variable; the library
  never reads it). The case skips wherever the marker is unset, so it no longer runs in the ambient lane at
  all. This is the shape every other log-reading scenario in the file already uses, and it also closes
  **minor 2** (the `-j 4` truncation race): nothing else writes that path.
* the CI step is now "The verify lanes armed the comparator", greps only those two logs, requires one per
  backend, and states what it proves — arming is a property of (library, environment), and these two
  processes share both with their ~400 ambient siblings; a per-process census is not available through a
  log every process truncates. The false claim is gone from the step name, the step text, the CMake comment
  and the scenario's header comment.

Driven against real and synthetic logs (`s06.sh`): contract-tree logs (no comparator) → rc 1 naming the log;
same logs with the arming line appended → rc 0; one backend's log removed → rc 1 naming the count.

### M3 — `symbol_report.gate_failures` ignored `resized`/`renamed` and shrinks (fixed, `01300e6a`)

`gate_failures(added, removed, resized, renamed, text_delta, fail_on_added_bytes, fail_on_symbol_set_change,
fail_on_text_delta=False)`:

* `--fail-on-symbol-set-change` fires on any of the four buckets;
* `--fail-on-added-bytes 0` means byte-identical (a shrink fails it); a positive budget keeps the old
  one-sided meaning; `--fail-on-text-delta` is the explicit spelling; a negative budget is a `parser.error`;
* the gates always read **threshold-0** buckets, whatever `--threshold` says (a gate that read the
  thresholded resize list would weaken itself the day someone raised the threshold), and the run prints that
  it did when `--threshold` is non-zero;
* the self-test drives the three shapes that used to pass. The reviewer's counterexample now answers:
  `gate_failures([],[],[],[],-4096,0,True)` → `['--fail-on-added-bytes 0: .text moved by -4096 bytes …']`.

The CI invocation is unchanged (`--threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0`) — it is
the brief's spelling and it now means what G1 says.

### Minors

| # | disposition |
|---|---|
| 1 | **Fixed** (`6060690a`). The child exits `HasFailure() ? 1 : 0` and checks `glGetError()` after the mipmap; the parent's message explains that status 1 is the child's own assertion. A/B proven below. |
| 2 | **Fixed** by M2's dedicated arming lane (own log, one process). No `RUN_SERIAL` was needed and the ambient lane keeps `-j 4`. |
| 3 | **Fixed** (`eec8cf2c`). `remove-artifact-clutter` now selects `retrace (` **and** `retrace verify (` jobs and strips both prefixes, longest first (verified on both job-name shapes). |
| 4 | **Fixed** (`eec8cf2c`). The control copies the verified `OpenRA/` output to `${RUNNER_TEMP}` first, captures ctest's status, restores the good output, and only then decides — so the restore happens on both paths and `Upload actual image` ships the verified run. |
| 5 | **Documented, not implemented** (`eec8cf2c`, in the step comment, and here). The second `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` pass is 186 entries under `-L integration-verify` on this tree; at the 5-10x the comparator costs that is ~23% more wall time on a lane already budgeted at 180 min, and C.4 does not ask for it. "Every integration entry" is not "every configuration": the staged-copy tier stays covered by `integration`, unverified. |
| 6 | **Fixed/declared** (`eec8cf2c`). `build-linux-verify` regains `Show installed toolchain`; the deliberate Release-always divergence is now written into the Configure step's comment (a Debug build flips visibility and arms `MOBILEGL_PIPE_POISON` through a different `#if` arm, i.e. it would measure a different library). |
| 7 | **Fixed** (`ce26029c`), and more than a comment: `Fatal{PipeVerifyBadKnob` is in the regex. D2's abort is exactly what `MOBILEGL_PIPE_VERIFY_FATAL=0` — the supported triage configuration — takes away, so relying on it was a hole. |
| 8 | **Fixed here.** The corrected local G5 line for the integrator is in "For the integrator" below. |
| 9 | **Fixed here** (see M1 and "Dependencies on package A"). |

## Verification — every command and its actual output

WSL, `~/w7/p1-verify`, this round. (v1's table still holds for everything untouched; these are re-runs.)

| # | command | result |
|---|---|---|
| 1 | `cmake --build build-{verify,linux,push} -j 12` | rc 0 ×3 (scenario + CMake changes rebuilt). |
| 2 | `ctest -L unit --no-tests=error -j 12` in all three builds | `100% tests passed, 0 tests failed out of 1466` ×3. |
| 3 | `python3 scripts/gen_pipe.py --check` | rc 0 — `63 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes`, `0 UNMAPPED`, up to date. |
| 4 | `symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` | rc **0**. `.text 10792579 -> 10792579 (+0)`, `27799 -> 27799: 0 added, 0 removed, 0 resized, 0 renamed`. G1 holds with the *stricter* gate. |
| 5 | `symbol_report.py --self-test` | `OK (2 canned transcripts, 5 buckets, 3 gates)`, rc 0. |
| 6 | `gate_failures([],[],[],[],-4096,0,True)` / resized-only / renamed-only / clean | 1 reason / 1 / 1 / `[]` — the reviewer's three counterexamples now fail, a clean pair still passes. |
| 7 | `ctest --test-dir build-linux -N` vs `~/w7/p1-before-ctest-names.txt` | **removed 0, added 10** (the five new cases × 2 backends). Additions only (G7). |
| 8 | `ctest --test-dir build-verify -N -L integration-verify` | **818** = 812 ambient + 2 `VerifyArming.` + 2 `VerifyCorrupted.` + 2 `PoisonOmitted.` (was 816; the two arming entries are the delta). |
| 9 | `ctest --test-dir build-linux -L integration-verify --no-tests=error` | `Total Tests: 0`, rc **8** — G8 half two still holds. |
| 10 | `ctest -N -L integration-gpu` folded (`s/\.(Verify\|VerifyArming\|VerifyCorrupted\|PoisonOmitted)\./\./`) | verify **878** unique names, pull **878**, `comm` empty both ways. |
| 11 | `ctest --test-dir build-verify -R '…Arming…\|VerifyCorrupted\.\|PoisonOmitted\.\|WithoutOmissionCompletes'` | `DirectGLES.Verify.…Armed` ***Skipped**, `DirectGLES.Verify.…CorruptedFieldIsReported` ***Skipped**, both `VerifyArming.…Armed` ***Failed**, both `VerifyCorrupted.` ***Failed**, both `PoisonOmitted.` ***Failed**, both ambient `WithoutOmissionCompletes` Passed (0.45/0.41 s). **The arming case skips or fails; it never passes on the contract tree.** |
| 12 | the arming step body vs the real `pipe-verify-arming-*.log` | rc 1, `::error::…pipe-verify-arming-DirectGLES.log carries no arming line` (correct: no comparator until core c4). Both logs exist, 318/330 lines, 0 arming lines. |
| 13 | the same body with the arming line appended to both logs | rc 0, `arming line present in all 2 arming-lane log(s)`. |
| 14 | the same body with one backend's log deleted | rc 1, `::error::found 1 pipe-verify-arming-*.log (expected one per backend)`. |
| 15 | the old failure mode: run only the last ambient entry, then look at the shared log | entry rc 0; `pipe-verify-DirectGLES.log` = 317 lines, **0** arming lines — M2 reproduced, and the new step no longer reads that file. |
| 16 | **A/B on minor 1**: ablate the child's first `ASSERT_NE(program, 0u)` **and** restore v1's `_exit(0)` | both `WithoutOmissionCompletes` **Passed** — the vacuous green. |
| 17 | same ablation with the fix in place (`_exit(HasFailure()?1:0)`) | both **Failed**; ablation reverted, both Passed again (0.42/0.40 s). |
| 18 | `run_trace_case.cmake`'s verify block driven with `cmake -P` against 8 synthetic logs | unset→0 · `=0`→0 · armed→0 · no arming line→1 · missing log→1 · `Fatal{PipeVerifyDiffer`→1 · `Fatal{UnmigratedPipeInput`→1 · **`Fatal{PipeVerifyBadKnob`→1** (was 0). |
| 19 | `nm` census on `build-verify/libMobileGL.so` | 30557 static / 11930 dynamic defined symbols; `MGPipeFillForVerb` static 1, dynamic 0. M1 reproduced and the fix's premise measured. |
| 20 | Windows `python -c "import yaml,sys; yaml.safe_load(…)"` on a copy of `test.yml` | parses. 17 jobs; `build-linux-verify` 16 steps / timeout 120, `integration-verify` 12 steps / needs `build-linux-verify` / 180, `retrace-verify` 13 steps / needs `[build-linux-verify, build-retrace, trace-cases, trace-fixtures]` / 240, `remove-artifact-clutter` needs `[retrace-summary, retrace-verify]`. BOM preserved. |
| 21 | `bash -n` on all 33 `run:` blocks of the six touched jobs | 0 syntax errors. |
| 22 | `remove-artifact-clutter`'s prefix stripping, on both job-name shapes | `retrace (DirectGLES, OpenRA)` → `OpenRA`; `retrace verify (DirectVulkan, minecraft-1.21.4-startup)` → `minecraft-1.21.4-startup`. |
| 23 | `trace_cases.py --format {cmake,names,fixture-files,github-test-matrix}` HEAD vs the contract script, same directory and manifest | **byte-identical** ×4 (21145 / 1928 / 46 / 6422 bytes), and `--ci --format github-test-matrix` identical too. `github-verify-matrix` = **16**, `github-test-matrix` = **77**. |
| 24 | `ctest --test-dir build-retrace -N -L retrace` | **80** (the label from v1's `c4170e56` still registers). |
| 25 | `grep -rc pGLContext MobileGL/MG_Backend \| sort \| diff - ~/w7/p1-before-pglcontext.txt` | only the two `:0` lines for the contract's new `MGPipe/PipeInputs.{h,cpp}`. E converts no site (G2 is B and C's). |
| 26 | env-knob census in the generated `*.cmake` | `818 MOBILEGL_PIPE_VERIFY=1`, `2 MOBILEGL_PIPE_VERIFY_CORRUPT`, `2 MOBILEGL_PIPE_VERIFY_FATAL=0`, `2 MOBILEGL_PIPE_POISON_OMIT`, `2 MGITEST_PIPE_ARMING_LANE=1`. No ambient entry names a control knob — the ENVIRONMENT-property trap stays avoided, and the new marker is on the arming lane only. |

## Deviations from the brief (v1's, still standing, plus this round's)

v1's deviations 1-3, 5-9, 11-13 are unchanged and still apply; the ones this round touched or added:

1. *(v1 #4, restated)* CI negative control B targets
   `DirectGLES\.Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes`, not the brief's
   `DirectGLES\.Verify\..*Mipmap`: nothing in the tree matches `Mipmap`, and with an empty selection
   `--no-tests=error` exits non-zero, which the `if ctest …; then error` shape would read as success. Both
   controls count their selection with `ctest -N` first.
2. *(v1 #10, superseded)* `--fail-on-added-bytes N` is "grew by more than N" **only for N > 0**; `0` means
   "byte-identical, in either direction", `--fail-on-symbol-set-change` covers all four buckets, and both
   gates read threshold-0 buckets. v1's claim that "`--threshold` already governs resizes" was wrong —
   `--threshold` only shortens the report.
3. **New.** `PipeVerifyArmingScenario.Armed` runs in a dedicated `VerifyArming.` lane per backend and skips
   in the ambient `Verify.` lanes; the brief's C.4 registers only six lanes and expects the arming case to
   ride the ambient ones. The lane count is therefore 8, and `integration-verify` is 818 rather than 816.
   Reason: the shared lane log cannot be read soundly by a lane member (M2, minor 2).
4. **New.** The arming CI step proves "the arming lane's process armed", not "every verify process armed".
   The honest per-process statement is not obtainable from a log every process truncates; the step, its
   name and the CMake comment all say so.
5. **New.** `integration-verify` still does not run `integration`'s second
   `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1` pass (minor 5) — 186 entries at 5-10x, and C.4 does not ask
   for it. Recorded in the step comment so a green lane is not read as "every configuration".
6. **New.** `run_trace_case.cmake` scans for three Fatal families, not the brief's two
   (`Fatal{PipeVerifyBadKnob` added).
7. **New.** `build-linux-verify` keeps Release under `ACTIONS_STEP_DEBUG` where `build-linux` switches to
   Debug, and this is now declared rather than implied by "copy of `build-linux:12-147`".

## Where the tree contradicted the brief's numbers

Unchanged from v1, with the lane count updated:

| brief | tree (measured) |
|---|---|
| "the 742-entry `integration-verify` lane"; "752 = 2 × (371 + 3 new) + 2 + 2" | **818** = 2 × 406 ambient + 2 arming + 2 corrupted + 2 poison-omitted. The pull build's `integration-gpu` set is 878. |
| C.4's `-L integration-gpu` fold "parity with the pull registration set" | holds once `Verify.`/`VerifyArming.`/`VerifyCorrupted.`/`PoisonOmitted.` are all folded: 878 = 878, `comm` empty both ways. |
| "40 trace + 79 desktop cases"; D.4 "retrace (77)" | `--ci --format github-test-matrix` = 77; `github-verify-matrix` = 16, as predicted. |
| brief's ctest-name regex `'^\s+Test #'` | matches only numbers ≥ 1000 (ctest pads); every count here uses `grep -cE '^ *Test *#[0-9]+:'`. |
| `LABELS integration-gpu integration-verify` (C.4 spelling) | must be `LABELS "integration-gpu\;integration-verify"` or the second label is silently dropped (v1 deviation 1, measured again: 818 under the label). |
| C.4 "after `:393`" for the verify block | must sit after `include(GoogleTest)` at `:395`; the block is at the end of the file. |

## For the integrator

* D.3's G5 line as written in the brief (`-R 'Mipmap'`) selects **nothing** on this tree and prints rc=0.
  The line that works locally is
  `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit ctest --test-dir build-verify -L integration-verify -R 'DirectGLES\.Verify\.PoisonOmissionScenario\.WithoutOmissionCompletes' --no-tests=error ; echo rc=$?`
  (expect non-zero once the poison is armed).
* D.3's arming grep should read the arming lane's logs:
  `grep -c "MGPipe: verify armed" build-verify/MobileGL/MG_IntegrationTest/pipe-verify-arming-*.log` (> 0 in
  both). The ambient `pipe-verify-<backend>.log` holds only the lane's last process and must not be used as
  evidence.
* D.3's lane count is 818, not 752.

## Dependencies on package A, and what is still unfinished

* `nm --defined-only … | grep -q MGPipeVerifyInputs` (both jobs) is red until core **c4** lands the
  comparator, and **that is the only reason left**: with the `-D` removed the check now measures what it
  claims, and `MGPipeFillForVerb` — which exists today — is found. (v1's wording implied `-D` would start
  working at c4; it would not have.)
* `pipe-gates` runs `python3 scripts/gen_pipe.py --self-test`, which core **c5** adds; red until then
  (`unrecognized arguments: --self-test`, rc 2, re-confirmed).
* The six control/arming entries are red and the two ambient arming entries skip on this tree by design (no
  filler, no comparator). They go green with core c2+c4 merged; C.4 defers E's functional verification to
  the integrated tree (D.3).
* **Not run here**: the full 818-entry lane, any retrace under `MOBILEGL_PIPE_VERIFY` (no trace binary or
  fixtures in this tree — the gate was driven against synthetic logs), and any CI dispatch (no push). The CI
  steps were exercised by extracting and running their exact bodies locally (rows 12-14, 21, 22).
* `build-retrace/` remains a throwaway configure used only to prove the `retrace` label registers.
