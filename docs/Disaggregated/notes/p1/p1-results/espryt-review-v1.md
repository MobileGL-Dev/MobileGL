# P1 package B — p1-espryt — adversarial review v1

Reviewed: tree `~/w7/p1-espryt`, branch `p1/espryt`, HEAD `f4ea84c5` (one commit on top of `p1/contract` = `bf86b1ed`). Implementer's result: `p1-results/espryt-v1.md`. Brief: `BRIEF-P1.md` A, B (D1–D13), C.2, C.5, D, E.

Stance: I tried to refute correctness, completeness and the G1/G2/G7 gate claims. Every number below was re-derived by me; logs under `~/w7/p1-espryt-review-*` and `~/w7/p1-espryt-review-int-*`; scripts under `scratchpad/wf4/espryt-review/`. The tree was left exactly as found (`git status`: only `build-linux/`, `build-push/` untracked; `git diff --stat` empty). The one throwaway worktree I created (`~/w7/p1-espryt-review-int`, branch `tmp/p1-espryt-review-int`) was removed afterwards.

## Verdict

**Approved — zero majors.** The package is a textually pure conversion of exactly the 122 lines the brief assigns to it, in exactly the four files it owns, and the pull build is symbol- and size-identical to the `087685d1` baseline. The one deviation from D9 (the hoisted `auto& live = *MGB_CTX;` in the fb-slot cache refill) is declared, justified with a bisect, semantically equivalent in both arms, and is the shape that yields `.text +0` where the literal D9 text does not.

## What I re-ran (commands + observed output)

| check | command (in `~/w7/p1-espryt`) | observed |
|---|---|---|
| diff scope | `git diff --stat p1/contract..HEAD` | 4 files, +129/−124: `DirectGLES.cpp` 190, `Managers.cpp` 47, `MultiDraw.cpp` 11, `Utils.cpp` 5 — nothing outside C.5's B row; no fixture touched |
| textual purity of every changed line | `scratchpad/wf4/espryt-review/diffcheck.py` over `git diff p1/contract..HEAD` (pairs `-`/`+` lines per hunk; a pair is "pure" iff `+` with `MGB_CTX->`→`MG_State::pGLContext->` equals `-`) | 86 hunks, **112 pure arrow substitutions**; the only non-pure pairs are the 4 include insertions, the `:143` decltype line (an arrow inside `decltype`, pure by the same rule), the 143–155 block, and the 8 Managers.cpp D9 lines (`if (MG_State::pGLContext)`→`if (MGB_CTX_LIVE)` ×3, `&& MG_State::pGLContext &&`→`&& MGB_CTX_LIVE &&`, `if (MG_State::pGLContext && `→`if (MGB_CTX_LIVE && `, `!= nullptr`→`MGB_CTX_LIVE` ×3). No accessor renamed, no argument changed, no evaluation order moved, no `const&` binding lost. |
| the touched old-line set equals the brief's site list | `scratchpad/wf4/espryt-review/linecheck.py` (old-side line numbers of every `-` line vs C.2's table + D9's 144/147/150) | `DirectGLES.cpp` removed 94 = brief 94, `Managers.cpp` 23 = 23, `MultiDraw.cpp` 5 = 5, `Utils.cpp` 2 = 2; `removed-not-in-brief []` and `brief-not-removed []` for all four |
| G2 (this package's share) | `grep -rc "pGLContext" MobileGL/MG_Backend/DirectGLES \| grep -v ":0$"` | empty (all 10 files 0) |
| no site outside ownership touched | `grep -rc "pGLContext" MobileGL/MG_Backend \| sort \| diff ~/w7/p1-before-pglcontext.txt -` | only the four DirectGLES lines change (92/23/5/2 → 0) plus the two contract-created `MGPipe/PipeInputs.{h,cpp}:0` rows; every DirectVulkan count identical to the baseline |
| MGB_CTX counts | per-file `grep -o` | arrows 91/15/5/2 (brief: 91 15 5 2); `MGB_CTX` tokens DirectGLES.cpp 93 (= 91 + `*MGB_CTX` + `MGB_CTX_IDENTITY`), `MGB_CTX_LIVE` Managers.cpp 8 (brief: 8), `MGB_CTX_IDENTITY` 1 |
| include placement (D3) | `grep -n` | `DirectGLES.cpp:18` Core.h → `:19` switch; `MultiDraw.cpp:11`→`:12`; `Utils.cpp:19`→`:20`; `Managers.cpp:9` `"Managers.h"` → `:10` switch (Managers.cpp does not include Core.h directly, per D3) |
| CRLF | `grep -c $'\r'` ×4 | 0 0 0 0 |
| gen_pipe | `python3 scripts/gen_pipe.py --check` | rc=0, "generated files are up to date" (63 fields, 7 sticky, 69 verbs, 9 classes) |
| pull build | `cmake --build build-linux -j 12` | rc=0 |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --json … --markdown …` | `.text 10792579 -> 10792579 (+0)`, `.data/.bss/.rodata +0`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, `27060 normalised names, 27060 unchanged` |
| nm set | `nm --defined-only … \| awk '{print $NF}' \| sort` before/after, `diff` | 0 lines |
| raw `.text` bytes | `objcopy -O binary --only-section=.text` ×2, `cmp -l` | sizes equal; **39 bytes differ**, deltas +1 (23 bytes) and +2 (16 bytes); mapped through `nm -S` to exactly the 6 functions the implementer names (`SyncMipmapsToBackend` 9, `SyncBuiltinSamplerToBackend` 8, `CopyTexSubImage2D` 7, `SyncTextureParamsToBackend` 6, `CopyTexImage2D` 5, `GetTexImage` 4) — the `__LINE__` immediates in `ErrorInfo` constructions shifted by the inserted include (+1) and, after `:148`, the hoisted line (+2). Sizes unchanged, so G1 as decoded ("`.text` byte delta of zero" = size) holds; see minor 1 for the wording |
| push build | `cmake --build build-push -j 12` | rc=0; `nm -C …/DirectGLES.cpp.o`: `gPipeInputs` refs 2, `pGLContext` 0 |
| **push arm at DEBUG level** (never built by anyone: both INFO builds compile out `DirectGLES.cpp:4200-…`, which holds the converted site at `:4217`) | `scratchpad/wf4/espryt-review/10_debugpush.sh`: the four TUs' `build-push/compile_commands.json` commands with `-DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_DEBUG` and `-fsyntax-only` (this also arms `MOBILEGL_PIPE_POISON` and the `MGP_INPUT_CHECK` bodies) | `DirectGLES.cpp rc=0 errors=0`, `Managers.cpp 0/0`, `MultiDraw.cpp 0/0`, `Utils.cpp 0/0` (twice each: static + shared targets) |
| G7 names | width-tolerant regex `grep -E '^\s*Test\s+#[0-9]+:'` | 2334 = 2334, `comm -23` empty, `comm -13` empty. The brief's own regex `'^\s+Test #'` yields **1335** on this build (confirmed: ctest pads to `Test    #1:` above 999 tests) — the implementer's "tree vs brief" item 2 is correct and the integrator's D.1 step needs the tolerant form |
| unit, pull | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | rc=0, 1466 (3 skipped by design) |
| unit, push (contract tree) | `ctest --test-dir build-push -L unit --no-tests=error -j 8` | rc=8, 18 failed / 1466 — the same 18 the implementer lists (contract filler copies nothing; expected per C.2 step 4) |
| integration lane, pull | `ctest --test-dir build-linux -L integration-gpu -R '^DirectGLES\.' --no-tests=error -j 4` | **441/441 passed, rc=0**. The implementer's run had 2 failures (`UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn`, `PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`); on the identical binary they pass here, which is consistent with the "shared per-lane log file under `-j 4`" flake they diagnosed (`UnlocatedIoBlockScenario.cpp:362-371`: the case trusts bytes after `LibraryLogSize()` in a file every process of the lane appends to). Pre-existing, not this package's |
| D10 / PipeStats constraints | `git diff -U0 p1/contract..HEAD \| grep -c "SyncPersistentMappedRange\|SyncGpuWrites"` / `\| grep -c AddCalls` | 0 / 0 |
| commit hygiene | `git log -1 --format=%B`; `git branch -r \| grep p1/` | subject exactly the C.2 step-5 text, blank line, four `- ` bullets; no attribution lines; no remote branch (not pushed) |

## Integrated-tree check (throwaway, `p1/core` HEAD `83b16561` = c1…c5 incl. the 83 fill points + `f4ea84c5` cherry-picked as `4c92f727`)

INTEGRATED_SECTION_PLACEHOLDER

## Deviations from the D-decisions, audited

1. **D9 `:150` shape** — declared. Committed: `auto& live = *MGB_CTX;` once, then `&live.GetFramebufferBindingSlot(i)` in the loop. Pull arm: `*MG_State::pGLContext` is `UniquePtr::operator*` → `GLContext&`, the same object the old `ctx` pointed at; behaviour when no context is live is identical to before (both the old `ctx->…` on a null `get()` and the new `*pGLContext` on a null pointer are only reached when the cached identity differs, i.e. never in that state). Push arm: `*(&gPipeInputs)` → `PipeInputs&`, whose `GetFramebufferBindingSlot` (`PipeInputs.h:494-500`) returns the live slot reference the filler stored; the identity key stays `MGB_CTX_IDENTITY` = `gPipeInputs.ContextIdentity()` so a recreated context still invalidates. I verified the committed shape is `.text +0` (above); I did **not** independently rebuild the literal D9 variant — the implementer's `~/w7/p1-espryt-bisect.txt` (v4/v5: +32, `SyncCurrentProgram` 1494→1510, `ForceBindCurrentFBO` 181→190, `BlitNamedFramebuffer` 592→593; v8 = committed: 189968 = baseline) is internally consistent and the TBAA explanation is the standard one for a pointer store next to a `unique_ptr` load. The deviation is the better reading of D9's own "if `.text` moves, this is the first suspect".
2. **`:143` keeps `std::remove_reference_t<…>`** — D9 abbreviated it; the alias must be the slot type, not a reference. Correct.
3. **Push lane not demonstrated in the package's own tree** — correct at the time (c3 had not landed); now demonstrated by me on the integrated throwaway tree, see above.

Undeclared deviations found: **none**.

## Majors

None.

## Minors

1. **Wording "pull build byte-identical" (commit subject, brief-dictated) vs. 39 differing `.text` bytes.** G1 is size/symbol based and holds (0/0/0/0, `.text +0`), and the body of the commit discloses the `__LINE__` immediates, but the integrator's `MEASUREMENTS.md` P1 attribution line ("expected: 0 bytes; 58 lines converted, none code-bearing", brief D.5) should say "sizes identical; 39 bytes of `__LINE__` immediates in 6 `DirectGLES.cpp` functions from the inserted include (+1) and the hoisted cache line (+2)" rather than "0 bytes". Integrator doc item, not a code change.
2. **Push-build unit tests that drive backend helpers directly** (`DirectGLESSanity.*`, `DirectGLESTextureSync.*`, `FramebufferTest.*`, `TextureTest.*` families, 18 on the contract tree) read `gPipeInputs` with no verb ever filled — they bypass `GLFunctionsTable` and therefore every `MGP_FILL`. Whatever their state on the integrated tree (recorded in the section above), this is a design consequence of D7's placement rule, not of this package's textual conversion; the implementer flagged it for A/integrator (D.2 expects "unit + integration-gpu green" under push). Recording so it is tracked.
3. **Brief D.1 / C `ctest -N` regex** (`'^\s+Test #'`) undercounts to 1335 on a 2334-test build; the implementer's tolerant form (`'^\s*Test\s+#[0-9]+:'`) is the one the integrator must use, or `comm -23` reports 999 phantom losses.
4. The two `ActuallyArmedWhenTheEnvironmentPinsItOn` itests are a `-j 4` flake in package E's file set (`MG_IntegrationTest/**`); the ambient lanes share one `MOBILEGL_LOG_FILE_PATH` per lane. Not this package's; noted for E/integrator.

## Where the tree contradicted the brief

- Only the `ctest -N` regex (minor 3). Every site count, line number and per-file total in C.2 matched the tree exactly (line check above).

## Hunted for and not found

- A converted site with a different accessor / dropped null check / moved evaluation / lost `const&`: none (112 pure substitutions + 1 decltype; the only `GetRenderStateParameters()` binding that is not `const auto&` is `DirectGLES.cpp:4158`'s `const FloatVec4& cc = …ClearColor`, a member reference — no 1168-byte copy).
- A site left on `pGLContext` behind a macro: the four files have 0 tokens; the one converted site under a preprocessor conditional (`DirectGLES.cpp:4217`, `LOG_LEVEL_DEBUG` block) compiles under the push arm at DEBUG.
- A site touched in a shared or unowned file: none (diff is the four owned files).
- Reformatting beyond touched lines: none (diff = 122 site lines + 4 includes + 1 hoisted line + the `:150` `&ctx->`→`&live.` line).
- A `PipeStats` literal or D10 sync site on a changed line: none.
