# P1 package C — p1-magma — result v1

Tree `~/w7/p1-magma`, branch `p1/magma`, branched from tag `p1/contract` (= `bf86b1ed`, the contract commit).
Scripts used: `scratchpad/wf4/magma/00_tree.sh` … `10_pushsmoke.sh`; logs under `~/w7/p1-magma-verify-*`.

## Commits (not pushed)

| hash | subject |
|---|---|
| `fad8636cb6b423e27c815ca2e4ed2b6ef8613e92` | `[Refactor] (Magma): route every frontend read through MGB_CTX - 164 arrow sites sed'd, 49 non-arrow lines converted (43 asserts keep their meaning as MGB_CTX_LIVE); pull build byte-identical` (+ `- ` bullets: per-file counts, D9 categories, D10 untouched, the G1 proof) |

`git diff --stat p1/contract HEAD`: 8 files changed, 217 insertions(+), 210 deletions(-) — 210 rewritten lines (the brief's 210) + 7 `#include <MG_Pipe/PipeInputsSwitch.h>` lines.

## What was converted (all under `MobileGL/MG_Backend/DirectVulkan/`)

| file | `MGB_CTX->` (arrow occurrences) | `MGB_CTX_LIVE` | include placed |
|---|---|---|---|
| `DirectVulkan.cpp` | 12 | 36 (34 asserts + `:1236` + the `:1284` ternary) | after `Core.h` (`:13`) |
| `BackendObject_DirectVulkan.cpp` | 2 | 2 (`:388`, `:786` guards) | after `Core.h` (`:15`) |
| `Renderer/UniformManager.cpp` | 14 | 9 (asserts) | after `Core.h` (`:13`) |
| `Renderer/VulkanRenderer.cpp` | 130 (127 lines) | 2 (`:11267` → `!MGB_CTX_LIVE`, `:12766`) | after `Core.h` (`:17`) |
| `Renderer/VkClearManager.cpp` | 1 | 0 | after `Core.h` (`:16`) |
| `Renderer/VkRenderPassManager.cpp` | 3 | 0 | after its include block (`:16`, after `TextureMetrics.h`; never included `Core.h`) |
| `Renderer/VkTextureManager.cpp` | 2 | 0 | after `Core.h` (`:14`) |
| `Renderer/VertexInputStateFactory.h` | — | — | comment `:133` → "they live on the frontend context's VAOs" |

Total 164 arrow occurrences on 161 lines + 49 non-arrow lines = 213 occurrences / 210 lines, exactly D1/C.3. The old line numbers of every removed non-arrow line were extracted from the diff and equal the D9 lists verbatim (DirectVulkan.cpp 337…1008 + 1236; UniformManager 812…1968; VulkanRenderer 11267, 12766; BackendObject 388, 786).

Rewrites used (D9): `MG_State::pGLContext->` → `MGB_CTX->`; `MOBILEGL_ASSERT(MG_State::pGLContext, ` → `MOBILEGL_ASSERT(MGB_CTX_LIVE, `; `MG_State::pGLContext != nullptr` → `MGB_CTX_LIVE` (covers the 9 UniformManager asserts, `DirectVulkan.cpp:1236`, `VulkanRenderer.cpp:12766`); `MG_State::pGLContext == nullptr` → `!MGB_CTX_LIVE`; `if (MG_State::pGLContext) {` → `if (MGB_CTX_LIVE) {`; `MG_State::pGLContext ? ` → `MGB_CTX_LIVE ? ` (`:1284`). No other qualified spelling (`::MG_State::pGLContext`, `MobileGL::MG_State::pGLContext`) existed (checked before the sed).

D10: the 12 `SyncPersistentMappedRange` + 3 `SyncGpuWrites` Magma sites are untouched (token counts 13 / 3 before and after, identical to `~/w7/pipe`). `PipeStats` `AddCalls` literals untouched (no non-`MGB_CTX` line changed besides the 7 includes and the comment — verified by filtering the diff).

## Verification commands and actual output

All in WSL, `~/w7/p1-magma`, `CCACHE_BASEDIR=/home/swung/w7`, build flags = `wsl_p1_tree.sh` (Release/INFO, clang, ccache, tests+itests on, lavapipe ICD, `MOBILEGL_BUILD_BENCHMARK=OFF`).

1. Tree creation `wsl_p1_tree.sh magma p1/contract`: `build-linux build rc=0 [955/955]`, `build-push build rc=0 [959/959]`.
2. `grep -rc "pGLContext" MobileGL/MG_Backend/DirectVulkan | grep -v ":0$"` → **empty** (all 43 files report 0).
3. `cmake --build build-linux -j 12` (after `touch` of the 8 files, to prove the objects come from the converted sources): rc=0, 21 objects rebuilt. Same for `build-push`: rc=0, 21 objects rebuilt.
4. `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --markdown ~/w7/p1-magma-verify-symbols.md --json ~/w7/p1-magma-verify-symbols.json`:
   ```
   .text 10792579 -> 10792579 (+0, +0.000%)   .data +0   .bss +0   .rodata +0   Total 17209819 -> 17209819 (+0)
   27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
   27060 -> 27060 normalised names, 27060 unchanged
   ```
   Run twice (before and after the touch-rebuild): identical. `nm --defined-only` name-set diff: 0 lines. (The `.so` sha256 differs — build path / build-id — sections and symbols do not.) **G1: 0/0/0/0, .text delta 0; no attribution needed.**
5. `ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure` → `100% tests passed, 0 tests failed out of 1466`.
6. `ctest --test-dir build-linux -L integration-gpu -R '^DirectVulkan\.' --no-tests=error -j 4 --output-on-failure` → 425/427 first pass; the two failures, `DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn` and `DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`, both "quirk pinned ON but the log never reported engaging it". Rerun serially (`-j 1`) twice → **both Passed** each time (0.2–0.3 s). The PrimGenReroute one is also the single entry in the baseline's own failure list `~/w7/pipe-int-itest-failures.txt` (same `-j 4` run shape); the PointSizeDemotion sibling passed in the baseline's run. Both are the "read the log file appended after the case started" scenarios and race under `-j 4`; not a Magma-conversion effect. **Lane green by name: 427/427.**
7. `ctest --test-dir build-push -L unit --no-tests=error` → 1466/1466 (push build compiles, links, and its unit set is green).
8. `ctest -N` name diff: `ctest --test-dir build-linux -N | grep -E '^\s+Test\s+#' | sed 's/^ *Test *#[0-9]*: //' | sort` → 2334 names; `comm -23 ~/w7/p1-before-ctest-names.txt -` → **empty**, `comm -13` → empty.
9. `python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all` → rc=0, `3 probes, 0 skipped, 0 problem(s)` (the new backend→`MG_Pipe/PipeInputsSwitch.h` include does not trip the include gate).
10. Push arm really selected: `nm -C build-push/libMobileGL.so | grep -c "MG_Pipe::PipeInputs::"` = 13 vs 0 in `build-linux`; `VulkanRenderer.cpp.o` in `build-push` references `gPipeInputs` (2).
11. Informational only (not a P1-C gate): `ctest --test-dir build-push -L integration-gpu -R '^DirectVulkan\.' -I 1,12 -j 4` → 12/12 `Subprocess aborted`. Expected on the contract tree: the contract's `MGPipeFillForVerb` only bumps the serial and copies no field (C.2 step 4 predicts this run is red until `p1/core` c2+ lands). `p1/core` currently has **no commit beyond `p1/contract`**, so the "rerun after rebasing onto p1/core c2+" half could not be done in this package.

## Where the tree contradicted the brief's numbers

None. Every count re-measured on the tree equals the brief: per-file `pGLContext` counts (4/47/23/1/1/3/2/129 = `~/w7/p1-before-pglcontext.txt`), arrow occurrences (12/2/14/1/3/2/130), the 49 non-arrow line numbers, the three double-occurrence lines (`VulkanRenderer.cpp:9214,10719,10963`), the D10 site count (12 + 3), the Core.h include line numbers used for D3 placement.

One brief-vs-brief detail: the C.3 header says "49 non-arrow lines" and the D9 table for C sums to 35 + 2 + 9 + 2 + 1 = 49 including the comment line — consistent; `:1284` is counted as an arrow line (it takes both the sed and the `MGB_CTX_LIVE ?` rewrite) and is not double-counted.

The brief's `ctest -N` recipe (`grep -E '^\s+Test #'`) misses tests #1–#999 on this ctest (they print as `Test  #999`, padded); the baseline file `~/w7/p1-before-ctest-names.txt` has all 2334, so the diff was taken with `Test\s+#` (same `sed`). Noted so the integrator's D.1 step does not report a false 999-name loss.

## Deviations from the brief

None in the code. Process notes:
- The edits were applied while the initial `wsl_p1_tree.sh` build was still running (after `git worktree add`, before `build-linux/` existed); to remove any doubt about stale objects, all 8 files were `touch`ed and both configurations rebuilt (21 objects each) before the final symbol report and test runs.
- The commit message carries the prescribed subject plus `- ` bullets (per the task's commit rules), not the single-line form of D13.

## Unfinished / for the integrator

- The push-build DirectVulkan lane on a tree that actually fills (`p1/core` c2+) has not been run — `p1/core` had no commit past the contract at the time of this result. After the D.1 rebase, `cmake --build build-push && ctest --test-dir build-push -L integration-gpu -R '^DirectVulkan\.' -j 4` is the first thing to run on this branch.
- The two `*ArmedWhenTheEnvironmentPinsItOn` scenarios are `-j 4`-flaky on lavapipe (baseline included); if the integrator's D.1 lane run trips on one, a serial rerun of that entry is the check.
