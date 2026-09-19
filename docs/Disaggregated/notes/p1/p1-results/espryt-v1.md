# P1 package B — p1-espryt — result v1

Tree `~/w7/p1-espryt`, branch `p1/espryt`, branched from tag `p1/contract` (`bf86b1ed`). Working tree clean after the commit. Nothing pushed.

## Commits (p1/contract..p1/espryt)

| hash | subject |
|---|---|
| `f4ea84c5f786ff556e8c41508072ad3782bee3ca` | `[Refactor] (Espryt): route every frontend read through MGB_CTX - 113 arrow sites sed'd, 9 non-arrow lines converted (the fb-slot cache keys on MGB_CTX_IDENTITY); pull build byte-identical` |

Files: `MobileGL/MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.cpp, MultiDraw.cpp, Utils.cpp}` only (4 files, +129/−124). No other file touched; no fixture binary touched.

## What was done (brief C.2 steps 1–5)

1. `#include <MG_Pipe/PipeInputsSwitch.h>` inserted per D3: `DirectGLES.cpp` after its `Core.h` include (old :18 → new :19), `MultiDraw.cpp` after :11 (→ :12), `Utils.cpp` after :19 (→ :20), `Managers.cpp` after its `Managers.h` include (old :9 → new :10). Includes were inserted after all line-number-addressed edits, so every brief line number was applied against the unshifted file.
2. Mechanical `MG_State::pGLContext->` → `MGB_CTX->` over the four files: 91 + 15 + 5 + 2 = **113 occurrences on 113 lines**, matching the brief's per-file arrow table exactly (verified line-for-line before the sed: DirectGLES.cpp 143…9510, Managers.cpp 3645…8751, MultiDraw.cpp 45,51,52,87,95, Utils.cpp 2297,2301).
3. The 9 non-arrow lines per D9: `Managers.cpp` 3644, 3844, 8750 (`if (MG_State::pGLContext) {` → `if (MGB_CTX_LIVE) {`), 3773 (`&& MG_State::pGLContext &&` → `&& MGB_CTX_LIVE &&`), 4735 (`if (MG_State::pGLContext && ` → `if (MGB_CTX_LIVE && `), 7192/7200/7203 (`MG_State::pGLContext != nullptr` → `MGB_CTX_LIVE`; their `?` arms 7193/7201/7204 took the sed); `DirectGLES.cpp` 143–155 block: `:144` → `static const void* g_fbSlotCacheContext = nullptr;`, `:147` → `const void* ctx = MGB_CTX_IDENTITY;`, `:150` → see deviation 1.
4. Pull build, symbol report, push build, lanes — below.
5. One commit, message per brief plus `- ` bullets.

## Verification (WSL, `~/w7/p1-espryt`; logs under `~/w7/p1-espryt-*`)

| command | result |
|---|---|
| `grep -rc "pGLContext" MobileGL/MG_Backend/DirectGLES \| grep -v ":0$"` | **empty** (all 10 files report 0) |
| `for f in …; do grep -o 'MGB_CTX->' … \| wc -l; done` | DirectGLES.cpp **91**, Managers.cpp **15**, MultiDraw.cpp **5**, Utils.cpp **2** (the brief's "91 15 5 2"; `MGB_CTX` tokens total in DirectGLES.cpp = 93 = 91 arrows + `*MGB_CTX` in the cache refill + `MGB_CTX_IDENTITY`) |
| `grep -c "MGB_CTX_LIVE" MobileGL/MG_Backend/DirectGLES/Managers.cpp` | **8** |
| `grep -c "MGB_CTX_IDENTITY" MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` | 1 |
| `cmake --build build-linux -j 12` | rc=0 (`[49/49] Linking … MobileGLIntegrationTest`) |
| `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --json … --markdown ~/w7/p1-espryt-symbol.md` | `.text 10792579 -> 10792579 (+0)`, `.data/.bss/.rodata +0`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, `27060 normalised names, 27060 unchanged` — **G1: 0/0/0/0, .text +0** |
| `nm --defined-only` name set, before vs after | diff **empty** (27799 = 27799) |
| raw `.text` byte compare (`objcopy -O binary --only-section=.text` + `cmp -l`) | 39 bytes differ in 6 DirectGLES.cpp functions (`SyncMipmapsToBackend`, `SyncBuiltinSamplerToBackend`, `SyncTextureParamsToBackend`, `CopyTexSubImage2D`, `CopyTexImage2D`, `GetTexImage`), function sizes unchanged. Disassembly diff: every difference is a `movl $0xNNN,0x8(%rax)` immediate = a `__LINE__` value stored into an `ErrorInfo` (e.g. `0xf0e`→`0xf0f` = 3854→3855): 23 bytes are +1 (lines after the inserted include), 16 are +2 (DirectGLES.cpp lines after the include *and* the hoisted `auto& live` line). The object built with **only** the include already shows these; the seds and the D9 rewrites add no byte. Not a gate item (G1 is symbol/size based), recorded for completeness. |
| `ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure -j 8` | rc=0, **100% tests passed, 0 failed out of 1466** (on the final `.so`) |
| `ctest --test-dir build-linux -L integration-gpu -R '^DirectGLES\.' --no-tests=error --output-on-failure -j 4` | 441 tests; **439 passed, 2 failed**: `DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn`, `DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn` — both are "text appended to the lane's `MOBILEGL_LOG_FILE_PATH` log after the case started" assertions (`UnlocatedIoBlockScenario.cpp:516`, `PointSizeDemotionScenario.cpp:512`). Re-run of the 10 `ActuallyArmedWhenTheEnvironmentPinsItOn` cases: `-j 1` **10/10 pass**, `-j 4` **10/10 pass**. The same two cases also failed identically in the push lane of a tree that did **not** contain this commit (bare `p1/core` c2, see below), and the pull `.so` is symbol- and size-identical to the baseline, so this is a pre-existing parallel-lane flake of the shared per-lane log file, not a regression of this package. Lane label used: `integration-gpu` (the tree has only `integration-gpu` and `unit`; the `-R '^DirectGLES\.'` selects the DirectGLES entries). |
| `ctest -N` name diff vs `~/w7/p1-before-ctest-names.txt` | `comm -23` **empty**, `comm -13` **empty**, 2334 = 2334 — see "tree vs brief" item 2 for the regex. |
| `cmake --build build-push -j 12` (`MOBILEGL_PIPE_PUSH=ON`) | rc=0 (`[55/55] Linking …`); the DirectGLES.cpp object references `gPipeInputs` and no `pGLContext` (`nm`: 0 / 2) |
| `ctest --test-dir build-push -L unit` on the **contract** tree | 18 failed / 1466 (expected on the contract tree: the c1 filler copies nothing, C.2 step 4 note) |
| `ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\..*ClearThenReadPixels'` on the contract tree | 0/5 pass, as the brief predicts for the contract tree |
| D10: `git diff -U0 … \| grep -c "SyncPersistentMappedRange\|SyncGpuWrites"` on changed lines | **0** — the 8 SPMR + 3 SGW Espryt sites are untouched (they are not on any converted line) |
| `git diff -U0 … \| grep -c AddCalls` on changed lines | 0 — `PipeStats` literals untouched |
| CRLF check on the four files | none |

### Push-build proof against the real filler (throwaway, removed afterwards)

Because `p1/core` had progressed to c2 (`3aa4d8af`, the per-class filler) while this package ran, `p1/espryt` was cherry-picked onto `p1/core@3aa4d8af` in a throwaway worktree (`~/w7/p1-espryt-onto-core`, branch `tmp/p1-espryt-onto-core`; **both deleted after the run**, logs kept as `~/w7/p1-espryt-onto-core-*.log`):

- cherry-pick applied cleanly (disjoint files); `cmake --build build-push` rc=0.
- `ctest -L unit` (push): 18 failed / 1466 — the same 18 as on the contract tree: `DirectGLESSanity.{BindsAMultisampleTextureDespiteTheDefaultMipmapFilter, BindingZeroClearsPreviousNativeTextureBinding}`, `NegativeApiErrorsTest.MultiBindTextures{RejectsBadRangesAndNames, BindsToTheTexturesOwnTarget}` (aborted), `FramebufferTest.{DrawIntoAWidenedDrawBuffer…, ClearIntoAWidenedDrawBuffer…, ApplicationAlphaMaskOff…, DualSourceBlendFactorsReachTheDriver…, DualSourceBlendIsDeclined…, DualSourceFactorsAreDeclinedEvenWithBlendingDisabled, DualSourceFactorsWithBlendingDisabledStillReachACapableDriver}`, `TextureTest.StorePackedWordsToClientCopiesWordsVerbatimUnderPackParams` (aborted), + 6 more of the same families (full list in `~/w7/p1-espryt-onto-core-unit.log`).
- `ctest -L integration-gpu -R '^DirectGLES\.'` (push): **68 passed / 373 failed** (SegFault or `Subprocess aborted`). A single case run with `MOBILEGL_LOG_FILE_PATH` shows the reason: `MGPipe: Fatal{UnmigratedPipeInput, "GetImageTextureBinding@<none>"}` — the verb name is `<none>`, i.e. **no verb was ever filled in the process**. Cause: `p1/core` at c2 (and at its current head `275dd3ed`, c4) carries **0** `MGP_FILL(` statements in `MobileGL/MG_Impl/GLImpl` (c3, the 83 fill points, has not landed yet). So the push lane cannot be green on any tree yet; this is not a package-B defect and not a fill-table gap (those would name a real verb). **The brief's "rerun after rebasing onto p1/core c2+ and it must be green" has to be re-run by the integrator once c3 is on `p1/core`** (D.2).
- The 18 push-build unit failures are the same phenomenon in the unit binaries (backend helpers driven directly by the tests, never through a fill point); with c3 landed most should turn green, but tests that call backend entry points without going through `GLFunctionsTable` will still read `@<none>` and are an A/integrator item (recorded here so it is not a surprise in D.2).

## Deviations from the brief (with reason)

1. **D9 line 150 (`DirectGLES.cpp` cache refill loop).** D9 spells `g_fbSlotCache[i] = &MGB_CTX->GetFramebufferBindingSlot(static_cast<FramebufferTarget>(i));`. Built that way the pull build is **not** byte-identical: `.text +32`, 3 resized symbols (`PrgramImpl::SyncCurrentProgram` 1494→1510, `ForceBindCurrentFBO` 181→190, `BlitNamedFramebuffer` 592→593) — exactly the "first suspect" D9 names. Bisected by rebuilding only the DirectGLES.cpp object per cumulative variant (`~/w7/p1-espryt-bisect.txt`): include alone 0, +91 arrow seds 0, +`const void*` cache (144) 0, **+line 150 via `MGB_CTX->` = +32**, +line 147 `MGB_CTX_IDENTITY` 0. Reason: `g_fbSlotCache[i] = …` stores a pointer, and under clang's TBAA any pointer store may alias the `unique_ptr`'s own pointer, so `MGB_CTX->` inside the loop re-loads `pGLContext` per iteration (the original read through the local `ctx`). Committed shape hoists the dereference once:
   ```cpp
   const void* ctx = MGB_CTX_IDENTITY;
   if (ctx != g_fbSlotCacheContext) {
       auto& live = *MGB_CTX;
       for (SizeT i = 0; i < g_fbSlotCache.size(); ++i) {
           g_fbSlotCache[i] = &live.GetFramebufferBindingSlot(static_cast<FramebufferTarget>(i));
       }
       g_fbSlotCacheContext = ctx;
   }
   ```
   Measured: `.text +0`, 0/0/0/0. Semantics identical in both arms (pull: `GLContext&`; push: `PipeInputs&`, the identity compare still uses `MGB_CTX_IDENTITY`, so a recreated context invalidates the cache under push too). One extra line inside the touched 143–155 block; no `pGLContext` token. If the integrator prefers the literal D9 text, the cost is the attributed +32 above (both variants' numbers are in the bisect file) — the "no reformatting beyond the touched lines" constraint is respected either way.
2. **Line 143** keeps `std::remove_reference_t<decltype(MGB_CTX->GetFramebufferBindingSlot(FramebufferTarget::Draw))>` (D9 abbreviates it as `decltype(MGB_CTX->…)`); it is the sed result and the `remove_reference_t` is required for the alias to be the slot type.
3. **Push-lane green not demonstrated** (see above): impossible on any tree until c3 lands; the brief's step-4 expectation of a `Fatal{UnmigratedPipeInput}` on the contract tree was reproduced instead.

## Where the tree contradicted the brief's numbers

1. None for the site counts: DirectGLES.cpp 92 lines / 91 arrows, Managers.cpp 23 / 15, MultiDraw.cpp 5 / 5, Utils.cpp 2 / 2, 9 non-arrow lines at exactly the listed line numbers; `~/w7/p1-before-pglcontext.txt` per-file numbers match.
2. **Baseline `ctest -N` name file vs the brief's own extraction regex.** `~/w7/p1-before-ctest-names.txt` holds 2334 names; the brief's pipeline `grep -E '^\s+Test #'` applied to the same build lists only 1335 (both in `~/w7/pipe/build-linux` today and in this tree) because with 2334 tests ctest pads the number to four columns (`Test    #1:` … `Test #1000:`), so the single-space regex matches only #1000–#2334. Width-tolerant form used here: `ctest --test-dir build-linux -N | grep -E '^\s*Test\s+#[0-9]+:' | sed -E 's/^\s*Test\s+#[0-9]+: //' | sort` → 2334 names, `comm` both ways empty. The integrator's D.1 `comm -23` step needs the same regex or it reports 999 "lost" names (all of `TextureTest.*`, `ProgramUtilTest.*`, … i.e. tests #1–#999).
3. `SyncCurrentProgram` (the symbol the literal D9 line 150 grows) lives in `DirectGLES.cpp` (`PrgramImpl::SyncCurrentProgram`), not `Managers.cpp`; the brief's `Managers.cpp:7193-7204` reference in D7 is `AttachPassthroughTessControlStage`'s ternaries (converted per D9, byte-neutral).
4. `p1/core` moved during this package: c2 `3aa4d8af`, then c4 `275dd3ed` (comparator) — c3 (83 fill points) not yet present at the time of writing.

## Unfinished

- Push-lane green (`ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.'`) — blocked on `p1/core` c3; re-run at integration (D.2). Everything else in the package's verification list is done.
- The two `ActuallyArmedWhenTheEnvironmentPinsItOn` itests are a pre-existing `-j 4` flake (pass serially and in isolation, fail identically without this commit); not fixed here (`MG_IntegrationTest/**` is package E's file set).
