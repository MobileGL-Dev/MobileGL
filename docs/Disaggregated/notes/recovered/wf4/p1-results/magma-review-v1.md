# P1 package C — p1-magma — adversarial review v1

Reviewed: tree `~/w7/p1-magma`, branch `p1/magma` @ `fad8636cb6b423e27c815ca2e4ed2b6ef8613e92` (one commit on top of `p1/contract` = `bf86b1ed`). Result file: `scratchpad/wf4/p1-results/magma-v1.md`. Brief: `scratchpad/wf4/BRIEF-P1.md` sections A, B (D1–D13), C.3, C.5, D, E.
Review scripts and the saved patch: `scratchpad/wf4/magma-review/` (`01_state.sh` … `11_check.sh`, `classify.py`, `ppscan.py`, `dbgcompile.py`, `diff.patch`). Logs: `~/w7/p1-magma-review-*`. The tree was left exactly as found (`git status --porcelain` shows only the untracked `build-linux/`, `build-push/`); the only thing I created is my own throwaway worktree `~/w7/p1-magma-review-int` (detached merge of `p1/magma` + `p1/core`, see §5), which is not a package tree.

**Verdict: approved — zero majors, four minors (none code-affecting).**

---

## 1. What I re-ran independently (every gate reproduces)

| check | command (WSL, `~/w7/p1-magma`, `CCACHE_BASEDIR=/home/swung/w7`) | observed |
|---|---|---|
| tree state | `git log --oneline p1/contract..HEAD`; `git status --porcelain` | exactly one commit `fad8636c`; clean apart from `?? build-linux/ build-push/`; both build dirs are Release / INFO / clang / LTO off, `build-push` has `MOBILEGL_PIPE_PUSH=ON`, `build-linux` OFF/OFF |
| G1 symbol report | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --markdown ~/w7/p1-magma-review-symbols.md --json ~/w7/p1-magma-review-symbols.json` (after `cmake --build build-linux -j 12` → `ninja: no work to do`) | `Total 17209819 -> 17209819 (+0)`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, `27060 normalised names, 27060 unchanged` |
| G1 raw | `readelf -S` `.text` size before/after; `nm --defined-only` name-set diff | `0xa4ae83` both; diff `0` lines |
| G2 (owned files) | `grep -rc "pGLContext" MobileGL/MG_Backend \| sort \| diff ~/w7/p1-before-pglcontext.txt -` | the only differences are the eight DirectVulkan files going `4/47/23/1/1/3/2/129 → 0` plus the two contract-created `MG_Backend/MGPipe/PipeInputs.{h,cpp}:0` lines; **every DirectGLES count is unchanged** (`DirectGLES.cpp:92, Managers.cpp:23, MultiDraw.cpp:5, Utils.cpp:2`) — no site outside the package's ownership was touched |
| ownership | `git diff --name-only p1/contract..HEAD` | exactly the eight files of C.5's Magma row; nothing else |
| gen_pipe | `python3 scripts/gen_pipe.py --check` | `generated files are up to date`, rc=0 (`--self-test` does not exist on the contract; it is core c5) |
| unit (pull) | `ctest --test-dir build-linux -L unit -j 12 --no-tests=error` | `100% tests passed, 0 tests failed out of 1466` |
| unit (push) | `ctest --test-dir build-push -L unit -j 12 --no-tests=error` | `100% tests passed, 0 tests failed out of 1466` |
| G7 name diff | `ctest -N \| grep -E "^\s+Test +#" \| sed -E "s/^ *Test +#[0-9]+: //" \| sort` vs `~/w7/p1-before-ctest-names.txt` | `after=2334 before=2334 removed=0 added=0` |
| DV integration lane (pull) | `ctest --test-dir build-linux -L integration-gpu -R '^DirectVulkan\.' -j 4 --no-tests=error --output-on-failure` | `100% tests passed, 0 tests failed out of 427` on the first `-j 4` pass (the implementer's two `*ArmedWhenTheEnvironmentPinsItOn` flakes did not reproduce for me; `~/w7/pipe-int-itest-failures.txt` confirms the PrimGenReroute one flaked on the baseline too) |
| push arm selected | `nm -C build-push/libMobileGL.so \| grep -c "MG_Pipe::PipeInputs::"` vs pull | `13` vs `0`; `ctest --test-dir build-push -N -L integration-gpu -R '^DirectVulkan\.'` → `Total Tests: 427` (the push build registers the lane) |
| include gate | `python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all` | rc=0, `6 negative-control trip(s)`, `3 probes, 0 skipped, 0 problem(s)` |

## 2. Independent classification of every changed line (`classify.py` over `git diff -U0 p1/contract..HEAD`)

The script pairs each removed line with its added line and accepts a pair only if applying the D9 rewrite set verbatim to the old line yields the new line; anything else is reported.

```
categories: {'guard': 2, 'arrow': 160, 'assert': 43, 'nenull': 2, 'arrow+ternary': 1, 'eqnull': 1}
pure additions: 7   (the seven #include <MG_Pipe/PipeInputsSwitch.h> lines)
BAD count: 1        (VertexInputStateFactory.h:133 — the comment rewrite, the one intentional non-mechanical line; text equals D9's wording)
arrow occurrences per file: BackendObject 2, DirectVulkan 12, UniformManager 14, VkClearManager 1, VkRenderPassManager 3, VkTextureManager 2, VulkanRenderer 130 — total 164 on 161 lines
non-arrow lines: 49 (DirectVulkan.cpp 34 asserts + 1236 + 1284; UniformManager 9 asserts; VulkanRenderer 11267, 12766; BackendObject 388, 786) + the comment
```

- Old line numbers of every arrow line and every non-arrow line equal the C.3 / D9 lists **verbatim** (printed by the script; compared list-for-list, including the three double-occurrence lines `VulkanRenderer.cpp:9214,10719,10963`).
- No pure deletions; no reordering; `git diff --check` clean; CR count 0 in all eight files (LF preserved).
- `PipeStats` `AddCalls` literals and every other non-`pGLContext` line untouched (follows from the classification: the only non-rewrite changes are the 7 includes and the comment).
- D10: `SyncPersistentMappedRange` / `SyncGpuWrites` counts identical to `~/w7/pipe` per file (`DirectVulkan.cpp 3/0, UniformManager.cpp 1/0, VulkanRenderer.cpp 6/3, VkBufferManager.cpp 2/0` = 12 + 3).
- Include placement matches D3: line immediately after the single `"MG_State/GLState/Core.h"` include in the six TUs that have one (`DirectVulkan.cpp:13`, `BackendObject_DirectVulkan.cpp:15`, `UniformManager.cpp:13`, `VkClearManager.cpp:16`, `VkTextureManager.cpp:14`, `VulkanRenderer.cpp:17`); `VkRenderPassManager.cpp:16` closes its only include block (`:10-15`) and is followed by the blank line and the namespace.
- The six code-bearing rewrites, verbatim (`git diff -U1`):
  ```
  BackendObject_DirectVulkan.cpp:388,786   -        if (MG_State::pGLContext) {            →  if (MGB_CTX_LIVE) {
  DirectVulkan.cpp:1236  - !query->pausedPrimitivesCountedByGpu && MG_State::pGLContext != nullptr) {  →  && MGB_CTX_LIVE) {
  DirectVulkan.cpp:1284  - MG_State::pGLContext ? MG_State::pGLContext->GetTransformFeedbackPausedPrimitiveCounter() : 0  →  MGB_CTX_LIVE ? MGB_CTX->...() : 0
  VulkanRenderer.cpp:11267  - if (!m_transformFeedbackFeatureEnabled || MG_State::pGLContext == nullptr ||  →  || !MGB_CTX_LIVE ||
  VulkanRenderer.cpp:12766  - return (MG_State::pGLContext != nullptr &&  →  return (MGB_CTX_LIVE &&
  ```
  Operand order, short-circuit order and the accessor called are unchanged in every one; no null check dropped, no `const&` lost (the sed cannot touch the left-hand side of a site and the classification proves nothing else moved).

## 3. Hunts that came back empty

| hunt | method | result |
|---|---|---|
| site left behind a macro / in a shared file | `grep -rn pGLContext MobileGL/MG_Backend` minus DirectGLES; `grep -rn "MG_State::"` in DirectVulkan excluding `MG_State::GLState` | nothing; no other `MG_State::` global is read |
| site inside a preprocessor region the INFO push build never compiled | `ppscan.py`: every `MGB_CTX` line with a non-empty `#if` stack | none — all 210 lines are unconditionally compiled, so the push build's successful compile checks every site |
| DEBUG-level build breaks on the new spelling (`MOBILEGL_ASSERT` becomes real) | `dbgcompile.py`: each of the seven TUs compiled `-fsyntax-only` with `-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_DEBUG` from `compile_commands.json`, in both `build-linux` and `build-push` | 14/14 `rc=0 errors=0` |
| an accessor Magma names that the D12 `scan_live_accessors()` gate (core c5) will reject | set of `MGB_CTX->(\w+)` in DirectVulkan (56 names) `comm -23` the `X(...)` rows of `MG_Pipe/Coverage.def` (65 rows) | empty — every one of the 56 has a row; the seven F-class names are among them, as the brief expects |
| the push-arm guards change meaning (`MGB_CTX_LIVE` at backend init before any fill) | read the contract: `PipeFill.cpp:33 Bool PipeInputs::IsLive() const { return LiveContext() != nullptr; }` | `MGB_CTX_LIVE` under push is literally `pGLContext.get() != nullptr`, so `BackendObject_DirectVulkan.cpp:388/786` (`InvalidateCompileEnv` at init) and the four XFB guards keep today's meaning in both arms; the brief's D9 [deviation] paragraph ("always true once a context exists") is true of the contract as landed |
| lane green because it never ran | `ctest -N` counts in both builds; `LastTest.log` timestamps | 427 entries registered and executed in both arms |
| other packages' trees touched | heads of `~/w7/p1-espryt` (`f4ea84c5`), `~/w7/p1-verify` (`bf86b1ed`), `~/w7/pipe` (`087685d1`, clean apart from its own untracked build dirs) | untouched by this package |

## 4. Where the tree and the brief disagree (and what wins)

- None on counts: every per-file count, line list and the D10 site count re-measured on the tree equals the brief and the result file.
- `ctest -N` recipe: the brief's `grep -E '^\s+Test #'` matches only four-digit test numbers on this ctest (`Test    #1:` / `Test  #999:` / `Test #1000:`). The baseline file `~/w7/p1-before-ctest-names.txt` nevertheless holds all 2334 clean names (verified head/tail and `grep -c '^ *[0-9]*:'` = 0), so the integrator's `wsl_p1_verify_all.sh` (which already uses `Test +#`) is fine; the brief's recipe line in D.1/C is the stale one. The implementer recorded this correctly.
- D13 says commits are single-line; the orchestrator's commit rule (subject + `- ` bullets) wins for this package and the implementer declared it. Subject line equals C.3's prescribed text character for character.

## 5. The one verification the result file could not do: the push-build DV lane on a tree that fills

The pull-build lane is a tautology for this package (the `.so` is byte-identical to the baseline), so the only functional test that can fail because of Magma's converted sites is the push build with core's filler and fill points present. The implementer deferred it because `p1/core` had no commit past the contract at 01:49; by review time `p1/core` was at `275dd3ed` (c2 filler `3aa4d8af`, c3 83 fill points `a196ada4`, c4 comparator `275dd3ed`), touching no Magma-owned file, `git merge-tree` conflict-free. I built a throwaway detached merge (`~/w7/p1-magma-review-int` @ `72a919b2`, `p1/magma` + `p1/core`, same configure flags as `wsl_p1_tree.sh`) and ran:

MERGED_RESULTS_PLACEHOLDER

## 6. Minors (none require a code change in this package)

1. **Push-smoke abort mischaracterised.** The result file (item 11) reports the contract-tree push lane as `12/12 Subprocess aborted` and calls it the expected poison. The actual reason (reproduced: `MOBILEGL_LOG_FILE_PATH=... ctest --test-dir build-push -R '^DirectVulkan\.OrientationScenario\.OffscreenPassRendersUpright$'`) is `[FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@<none>"}` — the **unconditional** null-storage check in `PipeInputs.h:475-478` (`m_bufferBindingSlot[index] == nullptr → MGPipeInputPoisonFatalForVerb`), fired before any fill point ran (`@<none>`; c3 was not on the contract). The D6 per-verb poison (`MGP_INPUT_CHECK`) is compiled out in a Release/INFO push build (`MOBILEGL_PIPE_POISON 0`, `PipeInputs.h:25`), so "the poison working" (C.2 step 4's wording, which C.3 inherits) is not what was observed. Harmless for this package; worth correcting in the brief's C.2/C.3 text so the integrator does not read a `<none>` Fatal as a table gap.
2. **The result file's "Lane green by name: 427/427" rests on a serial rerun of two flaky entries** rather than a clean `-j 4` pass. My `-j 4` pass was 427/427 first time; the baseline's failure list carries the same PrimGenReroute entry. Not a package defect, but the integrator's D.1 lane should keep `--rerun-failed -j 1` in its recipe (it already does).
3. **Commit-message form** deviates from D13's single-line rule (declared; the orchestrator's rule wins). No action.
4. **Cross-package observation, not Magma's:** the contract keeps a `Bool m_live` that `MGPipeFillForVerb` writes (`PipeFill.cpp:84,88`) but `IsLive()` ignores it (`PipeFill.cpp:33`). Magma's six `MGB_CTX_LIVE` sites are correct *because* `IsLive()` forwards; if core ever switches `IsLive()` to `m_live`, `BackendObject_DirectVulkan.cpp:388/786` (backend init, before any verb) would silently skip `InvalidateCompileEnv` under push. Record for the core reviewer / integrator.

## 7. Majors

None.
