# p05-value-types — result v1

Tree `~/w7/p05-value-types`, branch `p05/value-types`, base `feat/disaggregated @ 6672778b`.
Build dir `build-linux/` (Release, `/usr/sbin/clang++` clang 22, ccache, tests + integration tests ON).
Logs: `~/w7/logs/p05vt-*.log`, `~/w7/logs/nm-*.raw`, `~/w7/logs/probe_vt_H.txt`, `~/w7/logs/p203-*.log`.

## Commits (one, as the brief asks)

- `COMMIT_HASH` `[Refactor] (Pipe, State): extract MGPipeValueTypes.h - move the render-state, sampler and vertex value types and their enums out of MG_State/GLState so MG_Pipe no longer reaches RenderState.h; pure move, member order and namespaces unchanged`

Files: `MobileGL/MG_Pipe/MGPipeValueTypes.h` (new, 550 lines), `MobileGL/MG_Pipe/MGPipeTypes.h`, `MobileGL/MG_Pipe/generated/PipeVerify.inc`, `MobileGL/MG_State/GLState/RenderState/RenderState.{h,cpp}`, `MobileGL/MG_State/GLState/SamplerState/SamplerObject.h`, `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h`, `MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h`, `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp`, `scripts/gen_pipe.py`. Nothing under `MG_Backend/`, `MG_Impl/`, `MG_Util/` touched. Not pushed.

## What was done (B.1, in order)

1. `MGPipeValueTypes.h` created by a Python script (`scratchpad/wf3/p05-value-types/cut.py`) that slices the exact line ranges the brief names, with the anchor line of every range asserted before the cut: `RenderState.h:15-189` (11 enums, D2), `:191-370` (4 structs; `:263`/`:273` respelled to `kMGMaxDrawBuffers`), `SamplerObject.h:14-96` (6 enums + `SamplerParameters`), `VertexArrayObject.h:17-70` (3 vertex types, re-indented by one level because they now sit in `namespace MG_State::GLState` inside `namespace MobileGL` instead of three nested namespaces; namespace unchanged, D3; `class BufferObject;` forward-declared). `inline constexpr Uint kMGMaxDrawBuffers = 8;` (D4). Macro guard + `#pragma once`; includes exactly `<Includes.h>`, `<MG_Util/Math/VectorTypes.h>`, `<cstddef>`, `<type_traits>`. Trip wires as in the brief layout; the two "measure" numbers came out as `SamplerParameters` 100 and `VertexAttributeVersion` 6 (the build accepted them first time; `is_standard_layout_v<RenderStateParameters>` holds, so that assertion is kept).
2. Forwarding edits: `RenderState.h` now includes `<Includes.h>` + `<MG_Pipe/MGPipeValueTypes.h>` only; `RenderState.cpp` gained the direct `FramebufferObject.h` include (it spells `FramebufferObject::MAX_DRAW_BUFFERS` six times); `SamplerObject.h` and `VertexArrayObject.h` include the value header and keep only their classes; `FramebufferObject.h` includes the value header and defines `MAX_DRAW_BUFFERS = MobileGL::kMGMaxDrawBuffers`.
3. `MGPipeTypes.h:33` → `#include "MGPipeValueTypes.h"`, `:32` kept (D1), debt comment rewritten to say the MG_State half is repaid and `DynamicBackendParameters` is what still keeps gate A off `MGPipeTypes.h`; `using` comment updated. `gen_pipe.py:163-165` and the MEMCMP FALLBACK comment (`:410-414`, the source of `PipeVerify.inc:232-238`) reworded to P1; `python3 scripts/gen_pipe.py` run and the regenerated `.inc` committed (comment-only diff).
4. Full build: **zero lost-transitive-include failures** — no consumer `.cpp` needed a direct include (the candidates all reach `FramebufferObject.h` through `Core.h`, as the brief predicted).
5. Tests `PipeCatalogue.ValueTypeLayoutsArePinned` and `PipeCatalogue.ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` added to `PipeCatalogueTest.cpp`; **no include added** (`offsetof` reaches it through `MGPipe.h → MGPipeTypes.h → MGPipeValueTypes.h → <cstddef>`).

## Verification (every command, actual result)

| # | command | result |
|---|---|---|
| 1 | `cmake --build build-linux -j 16` (three times: after the cut, after the tests, after a comment reword) | exit 0 each time; first build 237 steps, `libMobileGL.so` + all test binaries link |
| 2 | `ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure -j 8` | **100% tests passed, 0 failed out of 1460** (log `p05vt-unit.log`; rerun after the final rebuild: see `p05vt-unit2.log`, UNIT2_RESULT) |
| 3 | new tests present in the run | `#1053 PipeCatalogue.ValueTypeLayoutsArePinned Passed`, `#1054 PipeCatalogue.ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail Passed` |
| 4 | `ctest -N` name lists, `comm -23 before after` (`~/w7/pipe/build-linux` vs mine) | before 1327 / after 1329 names; **lost: none**; added: the two tests above |
| 5 | ODR grep (`^\s*struct (RenderStateParameters\|…)\b \| ^\s*enum class (BlendFactor\|BorderColorForm\|CapabilityInput)\b` over `MobileGL/`) | 11 hits, **all in `MobileGL/MG_Pipe/MGPipeValueTypes.h`** (lines 32, 168, 208, 219, 229, 239, 441, 447, 476, 517, 525); no second definition anywhere |
| 6 | `grep -n "MG_State/\|MG_Backend/\|MG_Impl/\|MG_Remote/" MGPipeValueTypes.h` | **empty** (after rewording my own PURITY comment, which had matched with a slash spelling) |
| 7 | `grep -n "#include" RenderState.h` | `10:#include <Includes.h>`, `11:#include <MG_Pipe/MGPipeValueTypes.h>` only |
| 8 | closure probe: `clang++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -H -E` on `#include <MG_Pipe/MGPipeValueTypes.h>` | 654 closure lines, **0 under `MobileGL/MG_State/`, `MG_Backend/`, `MG_Impl/`, `MG_Remote/`** |
| 9 | same flags, `-fsyntax-only` on the probe TU | exit 0 (header is self-contained) |
| 10 | `git diff --stat 6672778b -- MobileGL/MG_Backend` | **empty** |
| 11 | `grep -rc "pGLContext" MobileGL/MG_Backend \| sort` before (pipe) vs after | **identical** |
| 12 | `python3 scripts/gen_pipe.py --check` | `generated files are up to date`, exit 0 |
| 13 | symbol report — `scripts/symbol_report.py` does not exist in this tree (it is the include-gate package's deliverable), so the equivalent was done by hand: `nm --defined-only -S` on `~/w7/pipe/build-linux/libMobileGL.so` vs `build-linux/libMobileGL.so`, keyed on mangled name, comparing type+size | 30511 → 30511 defined symbols; **0 added / 0 removed / 0 resized; 0 symbols changed address**; `size --format=sysv`: `.text` 10792579 B on both sides, Total 17209819 on both |
| 14 | attribution of every differing byte between the two `.so` files (`cmp -l`, mapped to sections) | 31 bytes total, length equal: 20 B in `.note.gnu.build-id`; 7 B in `.rodata` = the `GIT@a4baa9d` → `GIT@6672778` build stamp string (the reference build carries an older configure-time stamp); 4 B in `.rodata` = SPIRV-Tools' `Timestamp: 2026-09-05T22:05:41` → `22:36:04` string. `.text`, `.rodata` otherwise, `.data`, `.data.rel.ro`, `.eh_frame`, `.init_array`, `.dynsym`, `.dynstr`, `.gnu.hash` byte-identical (`objcopy --only-section` + `cmp`). Section table identical. **Zero code bytes differ.** |
| 15 | `ctest -L integration-gpu -j 8` (as configured) | 99%: 4 failed of 868 — three `*IsActuallyArmedWhenTheEnvironmentPinsItOn*` (the known `-j` flake) + `DirectVulkan.IterationRPProgram203Scenario.FixedCompleteInputProducesFixedCompleteGoldenOutput`; `--rerun-failed -j 1`: the three flakes pass, Program203 still fails (1 texel at (0,0), actual `(0x357a,0x4b65)` vs golden `(0x3a74,0x4821)`) — **diagnosed below as a configure difference of the build dir, not a regression** |
| 16 | `ctest -L integration-gpu -j 8` with `MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1 MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1 MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1` exported (what CI's integration-gpu job does, per `MG_IntegrationTest/CMakeLists.txt:286-291`), then `--rerun-failed -j 1` | ITEST2_RESULT |

### Program203 diagnosis (item 15)

- Passes 3/3 in `~/w7/pipe/build-linux`, fails 3/3 in mine, deterministically, same wrong bits every time.
- `.text` of `libMobileGL.so` **and** of `MobileGLIntegrationTest` are byte-identical between the two trees.
- `ctest -N -V` shows the reference registers the test with `ENVIRONMENT … VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json; MOBILEGL_MAGMA_FIX_ITERATIONRP_SUBGROUP_SCRATCH=1; MOBILEGL_MAGMA_DERIVE_NUM_SUBGROUPS=1; MOBILEGL_MAGMA_ITERATIONRP_FIX_BARRIER=1`; mine registers only `MOBILEGL_BACKEND_TYPE=DirectVulkan`.
- Cause: `MG_IntegrationTest/CMakeLists.txt:284-298` attaches the three repairs only when `MOBILEGL_ITEST_VK_ICD MATCHES "lvp_icd|lavapipe"`. `~/w7/pipe/build-linux/CMakeCache.txt` has `MOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json`; **`~/w7/p05-value-types/build-linux/CMakeCache.txt` has `MOBILEGL_ITEST_VK_ICD=` (empty)** — the package worktree was configured without the ICD pin. The only ICD on the machine is lavapipe, so the test runs on lavapipe either way, just without the repairs forced on — exactly the "Program 203 misses its golden output" case the CMake comment describes.
- Proof: my binary run by hand with the three variables → `mismatches=0/262656`, PASSED; the reference binary run by hand without them → `mismatches=1/262656`, the identical wrong bits, FAILED.
- I did not reconfigure `build-linux/` (the brief handed it to me as-is); I ran the lane with the variables exported instead (item 16). **Integrator note:** the other two p05 worktrees were presumably configured the same way and will show the same single failure on this test under a plain `ctest -L integration-gpu`; `cmake -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json build-linux` on each fixes the registration.

## Deviations from the brief

1. **Deleted the blank separator line after each cut region** (`RenderState.h:371`, `VertexArrayObject.h:71`) in addition to the ranges the brief names, so the forwarding headers do not keep a stray empty line right after the opening `namespace` brace. Whitespace only.
2. **The value header's leading comment is not the brief's two-line PURITY text verbatim**: I spelled the forbidden directories without a trailing slash (`MG_State, MG_Impl, MG_Backend or MG_Remote`) because the brief's own text-purity grep (`grep -n "MG_State/\|…"`) matched the brief's suggested wording; the grep is now empty as the brief requires. Added a short paragraph saying what the header is and why the MG_State headers forward to it.
3. **`MGPipeTypes.h` debt comment / `gen_pipe.py` comments are my wording**, following the brief's content instructions (MG_State half repaid; `DynamicBackendParameters` remains and keeps gate A off `MGPipeTypes.h`; field lists in P1). `gen_pipe.py:165` was wrapped onto two lines to stay under the file's line width.
4. **`ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` pins two more offsets than the brief lists** (`RenderState` at 0, `PatchVertices` at 1208) and `ValueTypeLayoutsArePinned` also checks `ColorMasks`' extent and the trivially-copyable/standard-layout predicates at runtime. Additions only.
5. **No `scripts/symbol_report.py`** in this tree (B.3 deliverable, not yet landed): the nm comparison was done by hand as the task allows; every byte of difference is attributed (item 14).
6. **`ctest -L integration-gpu` was run twice**: once as configured (item 15, one non-regression failure explained above) and once with CI's three environment variables exported (item 16). No test file, CMake file or build configuration was changed to get there.
7. `scripts/check_include_closure.py --mode both --probe value-header` (the "once p05/include-gate exists" line) was not run: the script is not in this tree. The equivalent manual `-H` probe (items 8-9) is green.

## Unfinished

UNFINISHED_TEXT
