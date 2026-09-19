# x2m — merge x2 against j0 rounds 2–3

Branch `p5/x2m`, merge commit `bd11ab18`, based on `feat/disaggregated@f5614995` and merging
`p5/x2@579118a1`. Identity immediately before commit was
`Swung0x48 <swung0x48@outlook.com>`. The commit message is one line with no attribution trailer.
No LFS fetch and no adb access occurred.

## Conflict resolutions

### `MobileGL/MG_IntegrationTest/Harness/split_log_paths.py` — one hunk

Kept j0 round 3's result/JUnit classification unchanged: skipped, missing, duplicate, not-run, or
non-failing selected entries cannot satisfy a control. In `evidence`, retained j0's stronger E1
contract that every selected private log must contain the expected Fatal, and added x2's optional
control label plus its named E3(a) diagnostic. Successful evidence is printed for every selected
entry; labeled failures name E3(a), while unlabeled E1 failures retain j0's exact established text.

### `scripts/ci/redcheck_control_smoke_test.sh` — two conflict regions

The setup/trap backs up and restores all four mutated files: j0's helper, split control and pull
retrace control, plus x2's draw-drop control. `mktemp` failure remains fatal.

The mutation combines x2's seven evidence-check perturbations across split/pull/drop-draw with j0's
separate ID-62 skipped-selection perturbation. The runner requires the named unrelated split,
unrelated pull-retrace, high-SSIM, missing-arm-line, zero-drop and missing-private-Fatal failures,
then restores all three control scripts before mutating only the helper's skip check. j0's generalized
line rewrite already preserves both `if` and `elif`; that subsumes x2's narrower `elif !` matcher fix,
so the j0 repair is the retained implementation.

### `scripts/ci/testdata/split_private_log_smoke.sh` — two hunks

Kept both suites as distinct modes: x2's `e3-no-private` case and j0's skipped/notrun/missing
selection, partial-Fatal and wrong-Fatal cases. The message dispatch retains x2's named E3(a)
private-log failure and j0's exact full-line matching for selection-classification failures.

### `scripts/ci/testdata/stub_ctest.sh` — two hunks

Kept the union of mode documentation and behavior. E1 emits the correct private Fatal for x2's
`e3-no-private` and j0's selection/partial cases, while `wrong-fatal` remains deliberately wrong.
The command-result switch keeps j0's skip/not-run shapes and x2's distinct E3(a) pixel-only failure
whose private library diagnostic is absent.

## Verification

Both Release Clang/Ninja builds completed with `CCACHE_BASEDIR=/home/swung/w7` and
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`.

| Gate | Result |
|---|---|
| `control_smoke_test.sh` | 16 core + 11 private-log cases, 0 failed |
| `redcheck_control_smoke_test.sh` | `P5_T1_CONTROL_SMOKE_REDCHECK_OK`; all named evidence mutations and ID-62 skip mutation caught |
| `split_log_paths.py check ctest build-split` | 21 entries, 21 distinct private paths |
| split `ctest -L unit`, REQUIRE_GPU=1 | 2073/2073, 0 failed |
| split inproc `ctest -L integration-split -j 4`, REQUIRE_GPU=1 | 22 selected, 0 failed; 20 passed and 2 expected skips |
| SmallRing ledger | `maxrec=784`, `cap=524288`, `cmdbytes=1314880`, `ringwraps=1`, `ringpads=0`, `ringwaits=1`, `emitseq=24323` |
| push `ctest -L integration-gpu -j 4` | 1128/1128, 0 failed |
| OpenRA DirectGLES baseline, inproc | 1/1 pass; `ssim=1.000000`, `mismatchPixels=0` |
| OpenRA with `MOBILEGL_IPC_E2_DROP_DRAW=1` | expected red, 0/1; `ssim=0.000036`, `mismatchPixels=295296`; 758 DrawVbo records dropped |

OpenRA evidence is retained in `~/w7/p5-x2m-e2-baseline{,.out}` and
`~/w7/p5-x2m-e2-dropdraw{,.out}`. Test transcripts are retained as
`~/w7/p5-x2m-{unit,split,gpu,redcheck}.log`; `~/w7/p5-x2m-submodlinks.sh` is the retained worktree
link toggle.

## G1

This worktree has no `build-linux`, so no new symbol-report run is claimed. The merged x2 report's
measured G1 was 0 added / 0 removed / 0 resized / 0 renamed and `.text +0`. The merge diff was
checked: every addition in the only pull-built touched files, `PipeStats.{h,cpp}`, is inside
`#if MOBILEGL_PIPE_PUSH`; all remaining implementation changes are under `MG_Remote/`,
`MG_IntegrationTest/`, `MG_Test/Wire/`, or CI scripts and therefore push/split guarded.

The worktree was left clean with submodule links off.
