# P1 package A (p1-core) — c1, the contract commit

Tree `~/w7/p1-core`, branch `p1/core`, base `feat/disaggregated @ 087685d1`.

## Commit

- `bf86b1ede6f9a43b4114fe2603f68a0bc2aa233c` — `[Feat] (Pipe): land the P1 contract - PipeInputsSwitch.h with MGB_CTX, the 63-field PipeInputs block with type-identical accessors, FillPoints.def and its G5b generator, the MOBILEGL_PIPE_PUSH/VERIFY options and the three verify knobs; pull build unchanged`
- Tag: `p1/contract` (annotation-less tag on that commit; `git describe --tags` → `p1/contract`). Not pushed.
- 15 files, +1834 / −30. Working tree clean apart from the three untracked build directories.

| action | file |
|---|---|
| CREATE | `MobileGL/MG_Pipe/PipeInputsSwitch.h` (D3 verbatim, 28 lines) |
| CREATE | `MobileGL/MG_Pipe/FillPoints.def` (69 verb rows in `GLFunctionsTable` declaration order, 9 classes, 138 (class, field) rows) |
| CREATE | `MobileGL/MG_Pipe/generated/PipeFillPoints.inc` (generated: `MGPipeVerb` 69, `kMGPipeVerbNames`, `MGPipeVerbClass` 9, `kMGPipeVerbClassNames`, `kMGPipeVerbClass[]`, `MGPipeFieldMask{Words[2]}`, `MGPipeFieldMaskHas`, `kMGPipeClassFieldMask[9]` with the 7 sticky bits OR'ed in, `static_assert(kMGPipeVerbCount == 69)`) |
| CREATE | `MobileGL/MG_Backend/MGPipe/PipeInputs.h` (D4: all 63 accessors — 49 V, 7 O, 7 F — with GLContext-identical signatures; `gPipeInputs` inline; `MOBILEGL_PIPE_POISON` derived once; `MGP_INPUT_CHECK` / `MGP_INPUT_VERIFY_READ`; `MGP_INPUT_STORAGE_LIST` + `VisitStorage`; `static_assert(sizeof < 20 KiB)`) |
| CREATE | `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp` (`MGPipeVerbName`, `MGPipeInputPoisonFatalForVerb`, `MGPipeFindInputField` / `MGPipeFindVerb`; under `MOBILEGL_PIPE_VERIFY`: `MGPipeInputsFieldEqual`, `MGPipeApplyVerifyCorruption`) |
| CREATE | `MobileGL/MG_Impl/Pipe/PipeFill.h` (D7 `MGP_FILL(Verb)`, `((void)0)` under pull) |
| CREATE | `MobileGL/MG_Impl/Pipe/PipeFill.cpp` (`PipeInputs::IsLive`, the seven F-class forwarders, `MGPipeFillForVerb` = serial bump + verb + identity + one-time sticky stamps; no field copies) |
| MODIFY | `MobileGL/MG_Pipe/Coverage.def` (D12: `GetBoundTransformFeedbackLifetimeId`, `HasOpenTransformFeedbackSpan` rows after `RecordError`; dead-row comment on `GetBoundTransformFeedbackName`; the `:36-40` note reworded to "when the inventory is re-vendored"; `MGP_COVERAGE_STICKY_LIST(X)` with the seven D6 rows and reasons) |
| MODIFY | `MobileGL/MG_Pipe/generated/PipeFilled.inc` (63 fields, 7 `true` sticky flags with reasons, `kMGPipeInputStickyFieldCount = 7`) |
| MODIFY | `MobileGL/MG_Pipe/MGPipe.h` (`#include "generated/PipeFillPoints.inc"` after `PipeFilled.inc`, G5b comment) |
| MODIFY | `scripts/gen_pipe.py` (`parse_function_table()`, `parse_fill_points()`, sticky-list parsing in `parse_coverage()`, `gen_filled` real sticky flags, `gen_fill_points()`, `PipeFillPoints.inc` written between `PipeFilled.inc` and `PipeCoverage.inc`, summary line "… 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes") |
| MODIFY | `CMakeLists.txt` (options after `:24`; VERIFY⇒PUSH block + conditional `SOURCE_FILES` append after the disaggregated fallback `endif()` at `:452`; the two `MOBILEGL_COMPILE_DEF` appends after `:525`) |
| MODIFY | `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` (D2 `PipeVerifyFatal` / `PipeVerifyCorrupt` / `PipePoisonOmit` under `#if MOBILEGL_PIPE_PUSH`) |
| MODIFY | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` (`:225` 61u → 63u) |

Not touched (per brief): `MG_Backend/DirectGLES/**`, `MG_Backend/DirectVulkan/**`, `PipeCalls.def`, `PipeFields.def`, `MG_Impl/GLImpl/**`, `tools/trace_replay/fixtures/**`.

## Verification (all run in WSL on `~/w7/p1-core`; logs under `~/w7/p1-core-verify-*.log`, `~/w7/p1-core-build-*-c1.log`)

Tree creation: `bash wsl_p1_tree.sh core feat/disaggregated verify` → worktree on `p1/core @ 087685d1`, submodules ok, `build-linux` / `build-push` / `build-verify` all rc=0 at the base (955/955).

Before any edit (sanity of the tooling): `symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` → `.text 10792579 -> 10792579 (+0)`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`.

After the edits, in this order:

1. `python3 scripts/gen_pipe.py` → wrote `PipeFilled.inc`, `PipeFillPoints.inc`; `python3 scripts/gen_pipe.py --check` → `gen_pipe: 71 calls (11 screen, 60 context), 63 verify payloads, 63 PipeInputs fields (7 sticky), 69 verbs, 9 classes` / `inventory 477 rows … 0 UNMAPPED` / `generated files are up to date`, rc=0.
2. `cmake --build build-linux -j 20` (reconfigured itself on the `CMakeLists.txt` change; `MOBILEGL_PIPE_PUSH=1` occurs 0 times in `build-linux/compile_commands.json`) → rc=0.
   `python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` →
   `.text 10792579 -> 10792579 (+0, +0.000%)`, `.data +0`, `.bss +0`, `.rodata +0`, `Total +0`, `27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed`, `27060 normalised names unchanged`; file bytes identical (19100448). `nm --defined-only build-linux/libMobileGL.so | grep -c MGPipe` = 0, same as the baseline. **G1 holds: the pull build is symbol-identical with a .text delta of 0** (the new `.cpp` files are not listed in the pull build — D5/D13: appended to `SOURCE_FILES` only under `MOBILEGL_PIPE_PUSH`; the new headers are included by nobody in the pull build; `Config.h`'s three members are behind `#if MOBILEGL_PIPE_PUSH`; the `MGPipe.h` include of `PipeFillPoints.inc` adds only `inline constexpr` tables that no pull TU odr-uses).
3. `cmake --build build-push -j 20` → rc=0, links (`MOBILEGL_PIPE_PUSH=1` on all 496 compile commands; `nm -C build-push/libMobileGL.so` exports `MGPipeFillForVerb`, `PipeInputs::IsLive`, `gPipeInputs`). Configure log: `MobileGL: PipeInputs push ON, appending the MGPipe fill sources`.
4. `cmake --build build-verify -j 20` → rc=0, links (`MOBILEGL_PIPE_PUSH=1` and `MOBILEGL_PIPE_VERIFY=1` on all 496; exports `MGPipeInputsFieldEqual`, `MGPipeApplyVerifyCorruption`). Configure log: `MobileGL: MOBILEGL_PIPE_VERIFY=ON forces MOBILEGL_PIPE_PUSH ON for this configure`.
5. `ctest -L unit --no-tests=error -j 12`: build-linux `100% tests passed, 0 tests failed out of 1466`; build-push `100% … out of 1466`; build-verify `100% … out of 1466`.
6. `(cd build-linux && ctest -N | grep -E '^\s+Test\s+#[0-9]+:' | sed 's/^ *Test *#[0-9]*: //' | sort)` → 2334 names; `comm -23 ~/w7/p1-before-ctest-names.txt -` → **0 removed**, `comm -13` → 0 added. (Note: the brief's regex `'^\s+Test #'` only matches test numbers ≥ 1000 — ctest pads smaller numbers with extra spaces — and would report 999 spurious removals; the baseline file itself holds all 2334 names, so the diff above uses the padding-tolerant regex.)
7. `grep -rc "pGLContext" MobileGL/MG_Backend | sort | diff - ~/w7/p1-before-pglcontext.txt` → only the two new files appear, both `:0` (`MG_Backend/MGPipe/PipeInputs.h:0`, `PipeInputs.cpp:0`); every catalogued file's count is unchanged (A converts no site). `grep -c pGLContext` on the two MGPipe files → `0 0`.
8. `grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp` → 0 (fill points are c3).
9. Integration smoke in the push and verify libraries (`ctest -L integration-gpu -R 'ClearThenReadPixels|GenerateMipmap|Mipmap' -j 4`): build-push `100% tests passed, 0 failed out of 10`, build-verify `100% … out of 10`, zero `Fatal{` in either log — the inert filler and the unarmed poison do not disturb a run in which no backend site reads `gPipeInputs` yet.

Not run (not in the task's proof list; long): the full `ctest -L integration-gpu` in build-push (brief C.1 step 1 lists it for A's own tree — recommended after c3 when the fill points exist, since at c1 nothing calls `MGPipeFillForVerb`).

## Deviations from the brief (and brief-vs-tree facts)

1. **`MGP_INPUT_CHECK` is live at c1** (under `MOBILEGL_PIPE_POISON`) rather than "expanding to nothing until step 3" (C.1 step 1 wording). Reason: B's and C's verification text expects a push run on the contract tree to hit `Fatal{UnmigratedPipeInput}` at the first read ("that is the poison working"); with an empty check the first read of an O-class field would be a null dereference instead. The check costs nothing in a non-poison push build (`((void)0)`), and c2 now only has to add the filler copies. `MGP_INPUT_VERIFY_READ` expands to nothing as specified (c4 arms it).
2. **Sticky stamping is in c1** (three lines in `MGPipeFillForVerb`: the seven sticky fields get `FilledGen = 1` on the first live fill) rather than c2. It is generation bookkeeping, not a field copy, and it keeps the F-class forwarders usable under poison from the contract on.
3. **`MGPipeInputsFieldEqual` and `MGPipeApplyVerifyCorruption` are in c1** (`PipeInputs.cpp`, `#if MOBILEGL_PIPE_VERIFY`), per C.1 step 1's "`PipeInputs.cpp` (equality, corruption injector)"; the comparator proper (`SnapshotFromGLContext`, entry compare, compare-at-read hook, arming line, knob parsing) is NOT in c1, as the task instruction requires. Both are one call of `PipeInputs::VisitStorage` over `MGP_INPUT_STORAGE_LIST` (56 storage rows + 7 forwarded = 63, static_asserted) instead of two 63-way switches. `IsCapabilityEnabledIndexed`'s two arrays are one `IndexedCapabilities` struct so the list stays one row per field.
4. **`PipeFields.def` is untouched in c1.** C.1 step 1 says "`Coverage.def` + `PipeFields.def` edits" but step 5 (c5) owns the six G4 field lists / overloads / coverage check, and the task instruction for c1 does not list `PipeFields.def`. Deferred to c5. Consequence: `kMGPipeVerifiedPayloadCount` stays 63 at c1 and `RenderStateParameters` / `PixelStoreParameters` still go through the memcmp fallback inside `MGPipeInputsFieldEqual` (both arms are copied by assignment into zero-initialised globals, so padding is equal; c5 removes the fallback anyway).
5. **`PipeInputs::IsLive()` forwards to the live context** (defined in `PipeFill.cpp`, returns `pGLContext != nullptr`) instead of returning a fill-time `m_live` flag; `m_live` is still recorded at fill for the filler's own use. Reason: D5 lists `IsLive` among `PipeFill.cpp`'s contents and D9 says `MGB_CTX_LIVE` "is always true once a context exists" under push, which a fill-time flag cannot promise before the first verb (e.g. `InvalidateCompileEnv` at backend init, `BackendObject_DirectVulkan.cpp:389,787`). `ContextIdentity()` is the fill-time address as specified.
6. **`GetBufferBindingPointCount` is not `constexpr`** in `PipeInputs` (GLContext's is): it is an out-of-line forwarder per D4's F class. No backend uses it in a constant expression.
7. **`MGPipeFieldMask::Words[2]`** as the brief says, generated as `max(2, ceil(fields/64))` so a 129th field grows the mask instead of silently truncating (`static_assert(kMGPipeInputFieldCount <= 2 * 64)` in the `.inc`).
8. **Two extra `gen_pipe.py` checks beyond the brief's list**: verb rows must be in `GLFunctionsTable` declaration order (not just the same set), so `MGPipeVerb`'s numbering is the table's by construction; a class listed twice in `MGP_FILL_CLASS_LIST` is refused. `--self-test`, `scan_live_accessors()` and `check_field_lists_cover_struct_members()` are c5, as the brief's step 5 says.
9. **Tree-vs-brief line facts**: `CMakeLists.txt` "after :451" — the disaggregated fallback's `endif()` is at `:452`; the block was inserted after it (before the `if (MOBILEGL_BUILD_DISAGGREGATED)` sources block at `:454`). The `MOBILEGL_COMPILE_DEF` appends went after the `endif()` at `:525` as stated. The brief's ctest-name regex issue is item 6 above.
10. **Accessor namespaces**: `BufferTarget` and `FramebufferTarget` live in `namespace MobileGL` (not `MG_State::GLState`, where the brief's text implies all frontend types sit); the header's `using` aliases spell them accordingly. Everything else (`VertexArrayObject`, `ProgramObject`, `ITextureObject`, `BufferObject`, `FramebufferObject`, `TextureUnit`, `ImageTextureBinding`, `CurrentVertexAttributeValue`) is `MG_State::GLState::`; value types are `MobileGL::`.
11. `IsCapabilityEnabled` with a cap ≥ `CapabilityInputCount` returns `false` (defensive bound; GLContext would index its own table); indexed getters mirror `RenderState`'s bounds handling (`MOBILEGL_ASSERT` + element 0 / early return; `GetCurrentVertexAttribute` mirrors `Core.cpp:246-253` with `MGLOG_E_ONCE` + a static default).

## Unfinished / next for A

- c2: per-class copies and stamps in `MGPipeFillForVerb` (mask = `kMGPipeClassFieldMask[kMGPipeVerbClass[verb]]`), `MOBILEGL_PIPE_POISON_OMIT` parsing.
- c3: the 83 `MGP_FILL(X);` statements (§B.7).
- c4: `SnapshotFromGLContext`, entry compare, `MGPipeVerifyReadHook` behind `MGP_INPUT_VERIFY_READ`, arming/knob log lines, `PipeVerifyFatal`.
- c5: `PipeFields.def` field lists, `MGPipeFieldEqual` overloads, `check_field_lists_cover_struct_members`, `scan_live_accessors`, `--self-test`.
- c6: the unit tests listed in C.1.
- Full `ctest -L integration-gpu` in build-push (after c3).
