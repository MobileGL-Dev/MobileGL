# P0.5 implementation brief — value header + artifacts header + include-closure gate

Tree: `feat/disaggregated @ 6672778b` (worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`; WSL `~/w7/pipe` and the three package worktrees `~/w7/p05-value-types` / `~/w7/p05-program-artifacts` / `~/w7/p05-include-gate`, branches `p05/<slug>`, all at `6672778b`, all with a configured `build-linux/` — Release, `/usr/sbin/clang++` (clang 22; there is no `clang++-20` in WSL), ccache, `MOBILEGL_BUILD_TEST=ON`, `MOBILEGL_BUILD_INTEGRATION_TEST=ON`, `MOBILEGL_BUILD_DISAGGREGATED=OFF`, log level INFO).
Every `file:line` below was re-opened at `6672778b`; the scouts were written at `6e0e3df3` and all their line numbers still hold. Corrections to the scouts are marked **[correction]**.

---

## A. Goal and acceptance gate (verbatim)

`docs/Disaggregated/ROADMAP.md:16`:

> **P0.5** 值头与制品头抽取 | 6–9 | `MG_Pipe/MGPipeValueTypes.h`（`RenderStateParameters`、`SamplerParameters`、`PixelStoreParameters`、`VertexAttribute`… 不 include `MG_State/GLState`）；`MG_State/GLState/ProgramState/ProgramArtifacts.h`（五个反射类型，不 include `ShaderObject.h`/`SpvcSession.h`，更新 7 个 includer）；`Visit()` 归档 + `sizeof` 绊线；CI `-H` include 闭包断言 | **全套测试逐名不变（纯搬移）；两条闭包断言绿且人为加回一个 `MG_State` include 能变红；`nm`/`.text` 变化可逐符号归因** | P0；**P1 与 P7 的硬前置**

`docs/Disaggregated/ARCHITECTURE.md:260` is the manifest: value header = `MAX_DRAW_BUFFERS`, `PerBufferBlendState`, `StencilFaceState`, `PixelStoreParameters`, `RenderStateParameters`, `SamplerParameters`, `BorderColorForm`, `VertexAttribute`, `VertexBufferBindingPoint`, "它不 include `MG_State/GLState` 任何东西"; artifacts header = `TypeFacts`, `ResourceReflection`, `XfbVarying`, `LinkArtifacts`, `SpirvArtifacts`, "只 include `<Includes.h>` 与容器". `ARCHITECTURE.md:259`: archive = `Visit()` + `static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`, one field table for both directions. `ROADMAP.md:7`: **每个门必须能因它存在的理由变红**; Windows 机器不是正确性门. `ROADMAP.md:34`: P0.5 is one of the five boundaries that owes a full `gl44to46` caselist run (CI/device, off the critical path).

Decoded: (1) no behaviour change, every existing test name still present and green (adding tests is fine); (2) two closure probes green + a negative control that goes red; (3) every `nm --defined-only -S` delta on `libMobileGL.so` explainable per symbol.

---

## B. Package split

### B.0 Decisions shared by all three packages (fixed here; do not re-decide inside a package)

| # | decision | why |
|---|---|---|
| D1 | **`DynamicBackendParameters` does NOT move.** `MGPipeTypes.h:32` keeps `#include <MG_Backend/BackendObject.h>`; the compositional assertion `MGPipeTypes.h:144-145` stays. | Not in the manifest; it has `SizeT` fields (`BackendObject.h:317,322,545`) and two member functions taking `TextureTarget` (`:487-497`) that force `TextureEnum.h`; "fixed-width members" is a type change, not a move. Escalate to the plan owner as a P1/P7 item. Consequence: probe A asserts `MGPipeValueTypes.h`, not `MGPipeTypes.h`. |
| D2 | **All eleven enums `RenderState.h:15-189` move**, including `StencilFace`, `PixelStoreParam`, `CapabilityInput`. | A split (some enums here, some there) is the maintenance trap; namespace `MobileGL` is unchanged so no spelling changes anywhere. |
| D3 | **`VertexAttribute`, `VertexBufferBindingPoint`, `VertexAttributeVersion` keep namespace `MobileGL::MG_State::GLState`** (the value header reopens it and forward-declares `class BufferObject`). **[correction** to scout-value-types §7.2, which recommended re-homing them in `MobileGL` + a `using`]. | Zero mangled-name churn for the value-types package, so the symbol report attributes 100 % of P0.5 churn to the artifacts de-nesting. Gate A is a path check; namespaces are irrelevant to it. Re-homing is a P13 cosmetic. |
| D4 | `MAX_DRAW_BUFFERS` becomes `inline constexpr Uint kMGMaxDrawBuffers = 8;` in `namespace MobileGL` (value header); `FramebufferObject::MAX_DRAW_BUFFERS` is **defined from it** (`= MobileGL::kMGMaxDrawBuffers`), so the 80 existing spellings keep working and cannot drift. `RenderStateParameters::MAX_VIEWPORTS` moves with its struct unchanged. No other new constants (**[correction]**: scout-value-types §6 `kMGMaxViewports`/`kMGMaxVertexAttribs*` are dropped). | Single definition; no `static_assert`-in-a-.cpp indirection. |
| D5 | **`LinkArtifacts::program` (`SharedPtr<glslang::TProgram>`) and `uniformInitialValues` (`Vector<glslang::TIntermediate::TUniformInitializer>`) move VERBATIM.** No `UniformInitializer` replacement in P0.5 (**[correction]** to scout-program-artifacts §6.2 item 5; recorded as the P7 follow-up). | Pure move. Both are symbol-free: `shared_ptr`'s deleter is type-erased at construction (only `ProgramLinkTask.cpp` constructs one) and `TUniformInitializer` (`include/glslang/MachineIndependent/localintermediate.h:627-636`) is an aggregate of `std::string`/`std::vector`/an unscoped enum with implicit inline special members. `<Includes.h>:80-84` makes both types complete on every side anyway. The gate pins the count of `glslang::` tokens in the header at exactly 2 so nothing new sneaks in. |
| D6 | `ProgramArtifacts.h` includes exactly `<Includes.h>` and `<set>` (`std::set<String>` at `ProgramObject.h:1331,1336`; `<set>` is in no header on the path today). | `ARCHITECTURE.md:260`. |
| D7 | `sizeof` tripwires: exact literal for POD types on every ABI (`TypeFacts` 44, `PixelStoreParameters` 28, `PerBufferBlendState` 28, `StencilFaceState` 28, `RenderStateParameters` 1168 — `MEASUREMENTS.md:86`, `ARCHITECTURE.md:188`, consistent with `MGL_RESIDUAL_BLOCK_SIZE 1248` at `MGPipeTypes.h:535`); **per-STL** numbers for container-bearing structs, asserted only under `__GLIBCXX__ && !_GLIBCXX_DEBUG` (the CI toolchain) with a `_LIBCPP_VERSION` (NDK) branch pinned by the integrator; MSVC unasserted. | `sizeof(std::string/std::set)` differs between libstdc++ and libc++ (`ARCHITECTURE.md:259` assumes one number; it cannot be one number). |
| D8 | **No `PipeFields.def` field lists for the value types in P0.5** (they need `std::array<struct>` support in `gen_pipe.py`'s comparator; `std::array::operator==` is unconstrained, so the `requires (x == y)` branch in `PipeVerify.inc:230` would hard-error); only the two comments that promise them "in P0.5" (`scripts/gen_pipe.py:163-165`, generated into `generated/PipeVerify.inc:232-238`) are re-worded to say P1. | Pure move; `MOBILEGL_PIPE_VERIFY` is not live until P1. |
| D9 | Gate skip semantics: a probe whose header does not exist prints `SKIP` and is counted; `--require-all` turns a SKIP into a failure. The negative control does not depend on either new header. CI and the ctest run **without** `--require-all` until the integrator flips it after all three packages land. | The task's "tolerate a not-yet-existing header" requirement, reconciled with `ROADMAP.md:7` (an all-skip run must not be a pass once the headers exist). |
| D10 | `.github/workflows/test.yml:3-8` triggers are NOT changed (`feat/disaggregated` is not a push trigger). The integrator runs `gh workflow run test.yml --ref feat/disaggregated` and records the run URL. | Adding the branch to the trigger list also fires the retrace jobs on every push; that is the plan owner's call, not this phase's. |

**Header paths and probe TUs (fixed contract; the gate package codes against these before the other two land):**

| probe | header (repo-relative) | probe TU text (exact) | forbidden path prefixes (repo-relative, after `normpath`) |
|---|---|---|---|
| `value-header` | `MobileGL/MG_Pipe/MGPipeValueTypes.h` | `#include <MG_Pipe/MGPipeValueTypes.h>\n` | `MobileGL/MG_State/`, `MobileGL/MG_Impl/`, `MobileGL/MG_Backend/`, `MobileGL/MG_Remote/` |
| `artifacts-header` | `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h` | `#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>\n` | `MobileGL/MG_State/GLState/ProgramState/ShaderObject.h`, `MobileGL/MG_Util/ShaderTranspiler/`, `MobileGL/Config.h`, `MobileGL/MG_Backend/`, `MobileGL/MG_State/GLState/BufferState/`, `MobileGL/MG_State/GLState/ProgramState/Shader` (prefix; catches `ShaderStage.h`, `ShaderCompileTask.h`, `ShaderCompileAdoptionMap.h`, `ShaderPreprocessCache.h`, `ShaderSourceKey.h`); text limit: `glslang::` ≤ 2 occurrences |
| `wire-header` (bonus, green today) | `MobileGL/MG_Remote/Transport/ITransport.h` | `#include <MG_Remote/Transport/ITransport.h>\n` | `MobileGL/Includes.h` (`WireLog.h:9-24` states the rule; `ITransport.h:33,35` include only `../Protocol/mg_protocol_base.h` and `<cstdint>`) |

The forbidden sets deliberately exclude glslang / spirv-cross / vulkan: `MobileGL/Includes.h:53,56,59,80-84,130` pulls `ska`, `xxhash`, `spirv_cross_c.h`, five glslang headers and `vulkan.h` unconditionally, and both new headers are allowed `<Includes.h>`. A "no glslang in the closure" assertion is unsatisfiable by construction; P7's criterion is `nm -D | grep glslang` on the server binary, a symbol gate.

---

### B.1 Package `p05-value-types`  (tree `~/w7/p05-value-types`, branch `p05/value-types`)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_Pipe/MGPipeValueTypes.h` | the value header (layout below) |
| MODIFY | `MobileGL/MG_State/GLState/RenderState/RenderState.h` | replace `:11-12` with `#include <MG_Pipe/MGPipeValueTypes.h>`; delete `:15-370` (keep `:14 namespace MobileGL {`); `namespace MG_State { namespace GLState { class RenderState … } }` (`:372-538`) untouched |
| MODIFY | `MobileGL/MG_State/GLState/RenderState/RenderState.cpp` | add `#include <MG_State/GLState/FramebufferState/FramebufferObject.h>` after `:9` — **[correction]**: no scout saw that `RenderState.cpp:460,488,524,542,572,586` spell `MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS` while the .cpp includes only `RenderState.h`, `Log.h`, `Types.h` (`:9-11`) |
| MODIFY | `MobileGL/MG_State/GLState/SamplerState/SamplerObject.h` | replace `:11` (`VectorTypes.h`) with `#include <MG_Pipe/MGPipeValueTypes.h>`; delete `:14-97` (keep `:13 namespace MobileGL {`; `class SamplerObject` from `:99` unchanged — it uses `SamplerParameters`, `FloatVec4/IntVec4/UintVec4`, `BorderColorForm`, all now supplied by the value header) |
| MODIFY | `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h` | add `#include <MG_Pipe/MGPipeValueTypes.h>` after `:12`; delete `:17-70` (`VertexAttribute`, `VertexBufferBindingPoint`, `VertexAttributeVersion`); keep `:11 "../BufferState/BufferObject.h"` (the class at `:72+` uses `BufferObject` 6 times) |
| MODIFY | `MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h` | add `#include <MG_Pipe/MGPipeValueTypes.h>` after `:13`; `:106` → `static constexpr Uint MAX_DRAW_BUFFERS = MobileGL::kMGMaxDrawBuffers;` |
| MODIFY | `MobileGL/MG_Pipe/MGPipeTypes.h` | `:33` → `#include "MGPipeValueTypes.h"`; keep `:32` (D1); rewrite the debt comment `:24-31`: the `MG_State` half is repaid, `MGPCaps`/`DynamicBackendParameters` remains (D1) and is what still keeps gate A off `MGPipeTypes.h`; `:36-40` `using` lines unchanged except the comment `:37-38` ("now live in MGPipeValueTypes.h") |
| MODIFY | `scripts/gen_pipe.py:163-165` + regenerate `MobileGL/MG_Pipe/generated/PipeVerify.inc` | comment only: "field lists of their own in P1 (P0.5 moved the types)"; run `python3 scripts/gen_pipe.py` and commit the regenerated `.inc` (CI `pipe-gates` diffs it, `test.yml:819-827`) |
| MODIFY | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | add the tests in "Tests" below; **must not gain any include** (`:17-18` are `"Includes.h"` + `<MG_Pipe/MGPipe.h>`; adding `RenderState.h` there would defeat the gate) |
| MODIFY (only if the build demands) | any `.cpp` that relied on `RenderState.h` transitively supplying `FramebufferObject.h`/`TextureObject.h`/`RenderbufferObject.h`/`SamplerObject.h` | add the direct include **to that .cpp**, immediately after its existing `MG_State/GLState/...` include; never re-add anything to `RenderState.h`. Candidates: the TUs including the four `MG_Util/Converters/*/RenderStateEnumConverter.h` or `MG_Util/Texture/PixelStoreProcessor.h` (`DirectGLES.cpp`, `VulkanRenderer.cpp`, `GL_Buffer.cpp`, `GL_Getter.cpp`, `GL_RenderState.cpp`, `GL_Texture.cpp`, `TextureTest.cpp`) — all also include `Core.h`, whose `:20 FramebufferState.h:12` keeps `FramebufferObject.h`, so most likely none. List every such file in the commit message. |

**`MGPipeValueTypes.h` layout** (one `namespace MobileGL` block, dependency order; every moved region is a verbatim cut, comments included):

```
// banner: copy MGPipeTypes.h:1-7, path = MobileGL/MG_Pipe/MGPipeValueTypes.h
#pragma once
#ifndef MOBILEGL_MG_PIPE_VALUE_TYPES_H        // belt and braces: this file is reachable both as
#define MOBILEGL_MG_PIPE_VALUE_TYPES_H        // <MG_Pipe/…> and <…> (CMakeLists.txt:531,535)
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>         // includes only <Includes.h> + <cstring> (VectorTypes.h:11,13)
#include <cstddef>                             // offsetof
#include <type_traits>
// PURITY: nothing from MG_State/, MG_Impl/, MG_Backend/, MG_Remote/ — scripts/check_include_closure.py
// probe "value-header" (ROADMAP P0.5; ARCHITECTURE.md:260, :501 gate A). Adding one turns CI red.
namespace MobileGL {
    // GL_MAX_DRAW_BUFFERS as MobileGL advertises it. FramebufferObject::MAX_DRAW_BUFFERS is defined
    // from this constant, so the two cannot drift.
    inline constexpr Uint kMGMaxDrawBuffers = 8;

    // RenderState.h:15-189 verbatim (11 enums, D2)
    // RenderState.h:191-370 verbatim, with :263 and :273 respelled
    //   Array<PerBufferBlendState, kMGMaxDrawBuffers> BlendStates;
    //   Array<BoolVec4, kMGMaxDrawBuffers> ColorMasks;
    //   MEMBER ORDER UNCHANGED (DirectGLES.cpp:2070-2081 offsetof spans; PipeSpanTable.inc:34-59 names 24 members)
    // SamplerObject.h:14-97 verbatim (6 enums incl. BorderColorForm : Uint8, SamplerParameters)

    namespace MG_State::GLState {
        class BufferObject;                    // BufferObject.h:61 forward-declares it the same way
        // VertexArrayObject.h:17-70 verbatim: VertexAttribute, VertexBufferBindingPoint, VertexAttributeVersion
        // (SharedPtr<BufferObject> members: NOT trivially copyable by design; MGPipeTypes.h:247-253 carries them as a blob)
    }

    // ---- trip wires (new; none existed for these before) ----
    static_assert(std::is_trivially_copyable_v<PixelStoreParameters> && sizeof(PixelStoreParameters) == 28);
    static_assert(std::is_trivially_copyable_v<PerBufferBlendState> && sizeof(PerBufferBlendState) == 28);
    static_assert(std::is_trivially_copyable_v<StencilFaceState> && sizeof(StencilFaceState) == 28);
    static_assert(std::is_trivially_copyable_v<RenderStateParameters>);
    static_assert(std::is_standard_layout_v<RenderStateParameters>);            // offsetof legality
    static_assert(sizeof(RenderStateParameters) == 1168, "RenderStateParameters changed size; MGL_RESIDUAL_BLOCK_SIZE and the Espryt spans depend on it");
    static_assert(offsetof(RenderStateParameters, BlendStates) < offsetof(RenderStateParameters, LogicOp));
    static_assert(std::tuple_size_v<decltype(RenderStateParameters::BlendStates)> == kMGMaxDrawBuffers);
    static_assert(std::is_trivially_copyable_v<SamplerParameters> && sizeof(SamplerParameters) == /*measure: expected 100*/);
    static_assert(std::is_trivially_copyable_v<MG_State::GLState::VertexAttributeVersion> && sizeof(MG_State::GLState::VertexAttributeVersion) == 6);
} // namespace MobileGL
#endif
```
Measure a number before pinning it: compile once with `== 1`; clang/gcc print `note: expression evaluates to 'N == 1'`. If `is_standard_layout_v<RenderStateParameters>` fails (it should not: all members public, `Vec4` adds no data to `VecBase`, `VectorTypes.h:17,161`), drop that one assertion and say so in the commit — do not touch the vector types.

**Ordered steps**
1. Create the header by cutting the regions above (cut, never copy; the ODR grep in step 6 proves it).
2. Apply the five `MG_State` header edits and the `RenderState.cpp` include.
3. Apply the `MGPipeTypes.h` edit; `gen_pipe.py` comment + regenerate.
4. Build: `cmake --build ~/w7/p05-value-types/build-linux --parallel $(nproc)`. Resolve any lost-transitive-include failure by adding the direct include at the consumer (table above).
5. Add the tests; rebuild; run.
6. Verification commands (below) all green; commit as ONE commit: `[Refactor] (Pipe, State): extract MGPipeValueTypes.h - move the render-state, sampler and vertex value types and their enums out of MG_State/GLState so MG_Pipe no longer reaches RenderState.h; pure move, member order and namespaces unchanged`.

**Constraints**
- Byte-for-byte identical behaviour: no member reorder (Espryt spans `DirectGLES.cpp:2070-2081, 2660-2683`; `PipeSpanTable.inc:34-59` names 24 members by string for P2's `offsetof` table), no enum underlying-type change (`BorderColorForm : Uint8` is load-bearing for `SamplerParameters` tail padding), no `SizeT`→fixed-width widening, no renames, no namespace changes (D3), no reformatting beyond the re-indentation the cut itself needs (do not run `scripts/format_code.sh` over the touched files unless it is a no-op on the untouched ones).
- `MobileGL/MG_Backend/**` is not touched: all 51 `RenderStateParameters` uses in `DirectGLES.cpp`, the 17 in `VulkanRenderer.cpp`, the `Managers.h:1087,1825` `SamplerParameters` shadows, `Managers.h:744,788` `VertexAttribute`/`VertexAttributeVersion` compile unchanged (P1's `sed` counts at `ARCHITECTURE.md:344` depend on this — verify `grep -rc "pGLContext" MobileGL/MG_Backend | sort` is identical before/after).
- No `#include <MG_State/…>` or `<MG_Backend/…>` in the new header, ever; MG_Pipe never includes MG_State back.
- Existing test names unchanged; only additions.

**Tests to add** (`MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp`, `TEST(PipeCatalogue, …)`, no new includes):
- `ValueTypeLayoutsArePinned`: runtime `EXPECT_EQ` twins of the `sizeof`/`offsetof` static assertions above (so the numbers show up in `ctest` output on every platform, including ones where a static assertion is skipped).
- `ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail`: `offsetof(ResidualValueBlock, Pack) == sizeof(RenderStateParameters)` and `offsetof(ResidualValueBlock, CapabilityBits) == 1200` — pins that the move did not alter the carrier.

**Verification (WSL, in `~/w7/p05-value-types`)**
```
cmake --build build-linux --parallel $(nproc)
ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
ctest --test-dir build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort > /tmp/vt-after.txt
ctest --test-dir ~/w7/pipe/build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort > /tmp/vt-before.txt
comm -23 /tmp/vt-before.txt /tmp/vt-after.txt            # must print nothing (no test name lost)
# ODR: exactly one definition each, all in the new header
grep -rn "^\s*struct \(RenderStateParameters\|PixelStoreParameters\|PerBufferBlendState\|StencilFaceState\|SamplerParameters\|VertexAttribute\|VertexBufferBindingPoint\|VertexAttributeVersion\)\b\|^\s*enum class \(BlendFactor\|BorderColorForm\|CapabilityInput\)\b" MobileGL
# purity (text)
grep -n "MG_State/\|MG_Backend/\|MG_Impl/\|MG_Remote/" MobileGL/MG_Pipe/MGPipeValueTypes.h     # empty
grep -n "#include" MobileGL/MG_State/GLState/RenderState/RenderState.h                          # Includes.h + MGPipeValueTypes.h only
# closure (compiler; header must be self-contained)
printf '#include <MG_Pipe/MGPipeValueTypes.h>\n' > /tmp/probe_vt.cpp
clang++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -H -E -o /dev/null /tmp/probe_vt.cpp 2>&1 | grep -E '^\.+ ' | grep -c 'MobileGL/MG_State/\|MobileGL/MG_Backend/'   # 0
clang++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -fsyntax-only /tmp/probe_vt.cpp
# once p05/include-gate exists (rebase onto it, or copy the script in): python3 scripts/check_include_closure.py --mode both --probe value-header
# backend read sites untouched
git diff --stat 6672778b -- MobileGL/MG_Backend                                                   # empty
# symbol report expectation for this package alone: 0 added / 0 removed / 0 resized / 0 renamed (see B.3 symbol_report.py)
python3 scripts/symbol_report.py --before ~/w7/pipe/build-linux/libMobileGL.so --after build-linux/libMobileGL.so
python3 scripts/gen_pipe.py --check
```

---

### B.2 Package `p05-program-artifacts`  (tree `~/w7/p05-program-artifacts`, branch `p05/program-artifacts`)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h` | the five types + `kInvalidUniformOffset` + `VisitFields` tables + tripwires (layout below) |
| MODIFY | `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h` | commit 1: add `#include "ProgramArtifacts.h"` after `:11`; delete `:41-104` and insert the alias block; `:545-547` → `static constexpr Uint kInvalidUniformOffset = MobileGL::MG_State::GLState::kInvalidUniformOffset;` (keep the comment); delete `:1144-1171` (XfbVarying + its 2-line comment) and `:1173-1449` (the LinkArtifacts comment block, `LinkArtifacts`, the SpirvArtifacts comment block, `SpirvArtifacts`). Commit 2: delete `:14 #include <MG_Util/ShaderTranspiler/SpvcSession.h>` (the header names no `spvc_*`/`SpvReflect*`/`SpvcMetadata` symbol — verified by grep, the include line is the only match) |
| CREATE | `MobileGL/MG_Test/Program/ProgramArtifactsTest.cpp` | tests below |
| MODIFY | `MobileGL/MG_Test/Program/CMakeLists.txt` | append a `ProgramArtifactsTest` stanza copied from the complete `AsyncCompileTest` stanza (`add_executable`, `target_include_directories` with `${MGL_ROOT}/include ${MGL_ROOT}/MobileGL`, `target_link_libraries(... GTest::gtest_main ${LINK_LIBRARIES})`, its `gtest_discover_tests(... DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)`) |
| MODIFY (commit 2, only if the build demands) | any `.cpp` that got `<spirv_reflect.h>`/`SpvcSession.h` transitively via `ProgramObject.h` | add `#include <MG_Util/ShaderTranspiler/SpvcSession.h>` **immediately after that TU's `ProgramObject.h` include line**. Candidates: `UniformManager.cpp`, `VulkanRenderer.cpp`, `ProgramInterface.cpp`, `ProgramFactory.h`, `ProgramTranslationCache.h/.cpp`, `ProgramLinkTask.h/.cpp`, `ProgramObject.cpp` (the other 17 `SpvcSession.h` includers already include it themselves) |

Includers of `ProgramObject.h` **[correction]**: 8 non-self files, in two spellings — `MG_Backend/DirectVulkan/Renderer/ProgramFactory.h:13`, `UniformManager.cpp:13`, `VulkanRenderer.cpp:17`, `MG_Impl/GLImpl/Program/ProgramInterface.cpp:11`, `ProgramLinkTask.h:11`, `ProgramTranslationCache.h:11` (as `<MG_State/GLState/ProgramState/ProgramObject.h>`), `ProgramPipelineObject.h:11`, `ProgramState.h:12` (as `"ProgramObject.h"`), plus `ProgramObject.cpp:9`. The roadmap's "7" is `ShaderObject.h`'s count. **None of the 8 needs an edit**: the in-class aliases keep every spelling (`ProgramObject::LinkArtifacts`, `ProgramObject::TypeFacts`, `ProgramObject::kInvalidUniformOffset` at `VulkanRenderer.cpp:4659,8553`, `GL_Program.cpp:1249,1304,1522`, `Core.cpp:513-514`, `ProgramSpirvTask.cpp:353,357,454`, the tests) compiling unchanged. Narrowing `ProgramTranslationCache.h`/`ProgramInterface.cpp` to the new header is a P7 follow-up, not P0.5.

**`ProgramArtifacts.h` layout**

```
// banner (copy from ProgramObject.h:1-7, path adjusted)
#pragma once
#include <Includes.h>   // String/Vector/Array/UnorderedMap/SharedPtr + GL enums. DEBT NOTE (MGPipeTypes.h:24-31 style):
                        // Includes.h:80-84 still pulls glslang and :59 spirv_cross_c.h; this header is glslang-free BY SYMBOL
                        // (what P7's `nm -D | grep glslang` measures), not by preprocessed text. A textually glslang-free
                        // closure needs MG_Util/Types.h split off Includes.h (Types.h:11 includes it back) - out of scope.
#include <set>          // std::set<String> (LinkArtifacts); NOT provided by Includes.h
// PURITY: no ShaderObject.h, no SpvcSession.h, nothing under MG_Util/ShaderTranspiler/, no Config.h, no MG_Backend/.
// scripts/check_include_closure.py probe "artifacts-header". `glslang::` appears exactly twice below (D5) and the gate pins that.

namespace MobileGL::MG_State::GLState {
    // ProgramObject.h:545-547 -> namespace scope (SpirvArtifacts::reservedNumSamplesOffset defaults to it)
    inline constexpr Uint kInvalidUniformOffset = ~0u;

    // ProgramObject.h:41-72 verbatim            struct TypeFacts
    // ProgramObject.h:74-99 verbatim            struct ResourceReflection
    // ProgramObject.h:101-104 verbatim          using UniformReflection/BlockReflection/PipeInputReflection/PipeOutputReflection
    // ProgramObject.h:1144-1171 verbatim        struct XfbVarying
    // ProgramObject.h:1173-1393 verbatim        (comment block + struct LinkArtifacts; the I5 paragraph :1184-1188 gets one added
    //                                            sentence: "m_artifacts lives in ProgramObject and is private there; the type being
    //                                            namespace-scope changes nothing about that gate")
    // ProgramObject.h:1395-1449 verbatim        (comment block + struct SpirvArtifacts)

    // ---- the archive field tables (ARCHITECTURE.md:259): ONE table per type, serving both directions ----
    // Visitor contract: v(const char* name, Field&). A visitor recurses into TypeFacts/XfbVarying/ResourceReflection
    // by calling VisitFields on the element itself; the tables never recurse.
    template <class Self, class V> requires std::same_as<std::remove_const_t<Self>, TypeFacts>
    void VisitFields(Self& a, V&& v) { v("isArray", a.isArray); … v("basicType", a.basicType); }            // 20
    …ResourceReflection (14, `type` visited as a value)… XfbVarying (11)…
    …LinkArtifacts: every member EXCEPT `program` (52 of 53) - `program` is null for every archived instance by
       construction (ProgramTranslationCache.h:42-50) and must never be serialized…
    …SpirvArtifacts (8)…

    // ---- trip wires ----
    static_assert(std::is_trivially_copyable_v<TypeFacts> && sizeof(TypeFacts) == 44);        // 13 Bool + 3 pad + 7 x 4
#if defined(__GLIBCXX__) && !defined(_GLIBCXX_DEBUG) && (SIZE_MAX == UINT64_MAX)
#define MGL_RESOURCEREFLECTION_SIZE <measure>
#define MGL_XFBVARYING_SIZE         <measure>
#define MGL_LINKARTIFACTS_SIZE      <measure>
#define MGL_SPIRVARTIFACTS_SIZE     <measure>
#elif defined(_LIBCPP_VERSION) && (SIZE_MAX == UINT64_MAX) && defined(MGL_ARTIFACT_SIZES_LIBCXX_PINNED)
    // integrator pins these from the NDK build (see C.4); until then this branch is inert
#endif
#ifdef MGL_LINKARTIFACTS_SIZE
    static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE,
                  "LinkArtifacts changed size: add the field to VisitFields(LinkArtifacts) (and its serializer when one exists), then update this number");
    … same for the other three …
#endif
}
```
`VisitFields` is a free constrained template rather than a member so the moved struct bodies stay verbatim, and a single table serves `const` and non-`const` (`Self` deduces either).

**`ProgramObject.h` alias block** (replaces `:41-104`, stays in the public section; a fully qualified RHS is mandatory — an unqualified `TypeFacts` would self-reference):
```
        using TypeFacts            = MobileGL::MG_State::GLState::TypeFacts;
        using ResourceReflection   = MobileGL::MG_State::GLState::ResourceReflection;
        using UniformReflection    = ResourceReflection;
        using BlockReflection      = ResourceReflection;
        using PipeInputReflection  = ResourceReflection;
        using PipeOutputReflection = ResourceReflection;
        using XfbVarying           = MobileGL::MG_State::GLState::XfbVarying;
        using LinkArtifacts        = MobileGL::MG_State::GLState::LinkArtifacts;
        using SpirvArtifacts       = MobileGL::MG_State::GLState::SpirvArtifacts;
```
`MAX_UNIFORM_LOCATIONS` (`:39`, glslang-bound) and the inline accessors that spell `glslang::ElmRowMajor` (`:489,:502`) / `glslang::TQualifier::layoutLocationEnd` (`:1476`) stay in `ProgramObject.h`.

**Ordered steps**
1. Commit 1 (pure move): create the header (types + `kInvalidUniformOffset` only), apply the `ProgramObject.h` cuts + aliases + re-export; build; add the test file + CMake stanza (alias/POD tests only); run. Message: `[Refactor] (Program): extract ProgramArtifacts.h - move TypeFacts, ResourceReflection, XfbVarying, LinkArtifacts and SpirvArtifacts to namespace scope with in-class aliases so every existing spelling compiles unchanged; pure move`.
2. Commit 2: drop `ProgramObject.h:14`; build; fix consumers per the table; message `[Refactor] (Program): stop ProgramObject.h including SpvcSession.h - it names no spirv-cross or SPIRV-Reflect symbol`.
3. Commit 3: the `VisitFields` tables + tripwires + the field-count tests. Message `[Feat] (Program): add the reflection-archive field tables and sizeof trip wires to ProgramArtifacts.h`.
(Three commits so the symbol report attributes churn to commit 1 only, and any `SpvcSession.h` fallout is bisectable.)

**Constraints**
- Struct bodies verbatim (D5): both glslang-typed members stay; no field added/removed/reordered/re-typed; comments move with their structs.
- Do not leave a second definition behind (the classic failure: `GLState::TypeFacts` and `GLState::ProgramObject::TypeFacts` would both compile and silently split the type). The ODR grep below must show exactly one `struct` per name.
- I5 (`ProgramObject.h:1622-1631`): `m_artifacts`/`m_spirv` stay private; `Artifacts()`/`Spirv()` remain the only readers. Nothing in this package touches `ProgramObject.cpp`, `ProgramLinkTask.*`, `ProgramSpirvTask.*`, `MG_Backend/**` (except commit 2's include lines).
- Existing test names unchanged; `MG_Test/Program/*` test binaries untouched except the new one.

**Tests** (`ProgramArtifactsTest.cpp`; first include is `<MG_State/GLState/ProgramState/ProgramArtifacts.h>`, then `<gtest/gtest.h>`, then — for the alias checks only — `<MG_State/GLState/ProgramState/ProgramObject.h>`):
- `AliasesAreTheSameTypes`: `static_assert(std::is_same_v<ProgramObject::LinkArtifacts, LinkArtifacts>)` ×5, and `ProgramObject::kInvalidUniformOffset == kInvalidUniformOffset`.
- `TypeFactsIsPodOf44Bytes`.
- `VisitFieldsCoversEveryMember` (commit 3): counting visitor → `TypeFacts` 20, `ResourceReflection` 14, `XfbVarying` 11, `LinkArtifacts` 52, `SpirvArtifacts` 8; `const` and non-`const` instances count the same.
- `SizesArePinnedOnThisToolchain`: `EXPECT_EQ(sizeof(LinkArtifacts), MGL_LINKARTIFACTS_SIZE)` under the same `#if`, plus an unconditional `RecordProperty("sizeof_LinkArtifacts", …)` (and the other three) so the number is visible in every `ctest -V` log on every platform (this is how the integrator obtains the libc++ numbers).

**Verification (WSL, in `~/w7/p05-program-artifacts`)**
```
cmake --build build-linux --parallel $(nproc)
ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
# test-name diff vs ~/w7/pipe exactly as in B.1 (comm -23 must be empty)
grep -rn "^\s*struct \(TypeFacts\|ResourceReflection\|XfbVarying\|LinkArtifacts\|SpirvArtifacts\)\b" MobileGL     # 5 hits, all in ProgramArtifacts.h
grep -n "#include" MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h                                     # <Includes.h> and <set> only
grep -c "glslang::" MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h                                    # 2 (D5)
printf '#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>\n' > /tmp/probe_pa.cpp
clang++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -H -E -o /dev/null /tmp/probe_pa.cpp 2>&1 | grep -E '^\.+ ' | grep -E 'ShaderObject.h|SpvcSession.h|MG_Util/ShaderTranspiler/|MobileGL/Config.h|MobileGL/MG_Backend/|BufferState/'   # empty
clang++ -std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -fsyntax-only /tmp/probe_pa.cpp
git diff --stat 6672778b -- MobileGL/MG_Backend MobileGL/MG_Impl                                                 # empty after commit 1 (commit 2 may add include lines)
# symbol report expectation: 0 added / 0 removed / 0 resized after --strip-scope; all churn is ProgramObject::X -> X renames
python3 scripts/symbol_report.py --before ~/w7/pipe/build-linux/libMobileGL.so --after build-linux/libMobileGL.so \
   --strip-scope 'MobileGL::MG_State::GLState::ProgramObject::' --only-names 'TypeFacts,ResourceReflection,XfbVarying,LinkArtifacts,SpirvArtifacts'
```

---

### B.3 Package `p05-include-gate`  (tree `~/w7/p05-include-gate`, branch `p05/include-gate`)

Compiles and passes against the base tree (probes A/B `SKIP`, C `OK`, self-test `OK`) and needs nothing from the other two packages.

**Files**

| action | file | what |
|---|---|---|
| CREATE | `scripts/check_include_closure.py` | the closure gate (spec below) |
| CREATE | `scripts/symbol_report.py` | the nm/`.text` attribution (spec below) |
| CREATE | `MobileGL/MG_Test/Purity/CMakeLists.txt` | ctest wiring (below) |
| MODIFY | `MobileGL/MG_Test/CMakeLists.txt` | after `:90 add_subdirectory(Pipe)`: a 2-line comment in the `:88-89` voice + `add_subdirectory(Purity)` |
| MODIFY | `.github/workflows/test.yml` | new job `include-graph-check` inserted after `flatc-check` (after `:323`, before `:325 benchmark`); name reserved by `ARCHITECTURE.md:568` |

**`scripts/check_include_closure.py` — specification**
- `REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))` (`gen_pipe.py:37` idiom); `argparse` with `description=__doc__`; every stdout line prefixed `include-closure: `; failures end with a GitHub `::error::` line and `sys.exit(1)` (`test.yml:829-846` house style).
- `PROBES` module-level table exactly as in B.0 (Name, Header, Tu, Forbidden, Allow=[] — policy: an allow entry needs a `Reason` string printed in the summary and a PR justification, `test.yml:831-835` — TextLimits, Why).
- Flags: `--mode {text,clang,both}` (default `text`), `--compiler PATH` (default order: `$CXX`, `clang++-20`, `clang++`, `c++`, `g++`; print which one was used), `--compile-commands PATH` (optional: take any entry whose `file` is under `MobileGL/`, keep `-D/-I/-isystem/-std`, drop `-o/-c/-MD/-MF/-MT` and the input), `--probe NAME` (repeatable), `--self-test`, `--require-all`, `--json PATH`.
- Default clang flags when no compile-commands (measured to reproduce the full-flag closure exactly, 666 lines): `-std=gnu++23 -Iinclude -IMobileGL -IMobileGL/MG_Pipe -I3rdparty/xxHash -I3rdparty/Vulkan-Headers/include -H -E -o /dev/null`, cwd = `REPO_ROOT`. Before running clang mode, check `include/ska/flat_hash_map.hpp`, `3rdparty/xxHash/xxhash.h`, `3rdparty/Vulkan-Headers/include/vulkan/vulkan.h` exist; if not, fail with `run from a full checkout: git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers` (never silently downgrade).
- `-H` parsing: stderr only; accept only lines matching `^(\.+) (.*)$` (g++ appends a "Multiple include guards may be useful for:" trailer that must be discarded); depth = dot count, so the chain to a violation is reconstructed by walking back to depth−1; `path = os.path.normpath(os.path.join(cwd, path))` before prefix matching (today's output contains `TextureState/../SamplerState/SamplerObject.h`); paths outside the repo are ignored.
- Text mode: transitive walk of literal `#include` lines (there are no macro-driven includes under `MobileGL/`: 3377 includes, all literal). Resolution: `"…"` → including file's directory first; then `include/`, `MobileGL/`, `MobileGL/MG_Pipe/`, `3rdparty/xxHash/`, `3rdparty/Vulkan-Headers/include/`; unresolved = leaf. Blind to `#if`; the compiler mode is the arbiter. In `--mode both` a disagreement between the modes' violation sets is itself a failure that prints both sets.
- Second assertion per probe in clang mode: `-fsyntax-only` on the probe TU (self-contained header), reported on its own line.
- Missing header (D9): `include-closure: <probe> SKIP  header not present yet: <path>`; counted in the summary; with `--require-all` a SKIP is a failure.
- Summary lines (always): `include-closure: <probe> OK|SKIP|FORBIDDEN <n> headers in closure, <m> forbidden (mode=…, cxx=…)`, then `include-closure: <k> probes, <s> skipped, <p> problem(s)`. On a violation print the chain, one hop per line, indented.
- Assert that the manifest contains both `value-header` and `artifacts-header` — deleting a probe is red.
- `--self-test` (always run by the ctest and the CI job; must not touch any tracked file — synthesize TUs in `tempfile.mkdtemp()`):
  1. Negative control independent of P0.5: TU `#include <MG_Pipe/MGPipeHandles.h>\n#include <MG_State/GLState/RenderState/RenderState.h>\n` checked with the `value-header` forbidden list → must report ≥1 violation with `RenderState.h` at depth 1.
  2. When `MGPipeValueTypes.h` exists: TU `#include <MG_Pipe/MGPipeValueTypes.h>\n#include <MG_State/GLState/RenderState/RenderState.h>\n` → must trip (the roadmap's literal "人为加回一个 MG_State include 能变红").
  3. When `ProgramArtifacts.h` exists: TU with `#include <MG_State/GLState/ProgramState/ShaderObject.h>` added → must trip on the `artifacts-header` list; and a canned copy of the header with a third `glslang::` token must trip the text limit.
  4. Parser unit checks: a canned g++ `-H` transcript with trailer lines parses to the dot lines only; a `../`-spelled path normalises and matches.
  5. Each of 1-3 runs in every enabled mode. Zero violations anywhere → `::error:: negative control did not trip: the closure gate is not checking anything`, exit 1.

**`MobileGL/MG_Test/Purity/CMakeLists.txt`**
```
cmake_minimum_required(VERSION 3.14)
# The P0.5 include-closure assertions (ROADMAP P0.5; ARCHITECTURE.md:501 gate A): no MobileGL
# compilation, no GL context. Text mode only here, because this CTestTestfile also runs on the
# `test` job's runner (test.yml:149-203), which has no submodules; the compiler-backed run is the
# include-graph-check job and the developer's local `--mode both`.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    add_test(NAME MobileGLPurity.IncludeClosure
             COMMAND ${Python3_EXECUTABLE} ${MGL_ROOT}/scripts/check_include_closure.py --mode text --self-test)
    # ctest -L unit (test.yml:198-203). NO ENVIRONMENT property: it replaces the job env (ARCHITECTURE.md:567).
    set_tests_properties(MobileGLPurity.IncludeClosure PROPERTIES LABELS unit)
else()
    message(STATUS "MobileGLPurity.IncludeClosure not registered: no python3 interpreter (Windows is not a correctness gate)")
endif()
```
The integrator later appends `--require-all` (C.3). The `test` job's path rewrite (`test.yml:185-196`) only touches the cmake binary path; the script's absolute path is the same on both runners.

**CI job** (insert after `test.yml:323`):
```
  # P0.5 interface-purity gate A (ARCHITECTURE.md:501): the two extracted headers' include closure,
  # asserted on `-H` output because `nm --undefined-only` is blind to "included but not called".
  # Needs a preprocessor and three header submodules, no CMake configure, no glslang sources -
  # independent of build-linux for the same reason pipe-gates is.
  include-graph-check:
    name: Include-closure purity gate
    runs-on: ubuntu-latest
    steps:
      - name: Checkout repo
        uses: actions/checkout@v6
      - name: Check out the three header submodules the closure needs
        # ska/flat_hash_map.hpp, xxhash.h and vulkan/vulkan.h are the only submodule headers
        # Includes.h reaches; glslang and spirv_cross are vendored under include/.
        run: git submodule update --init include/ska 3rdparty/xxHash 3rdparty/Vulkan-Headers
      - name: Install clang
        run: sudo apt-get update && sudo apt-get install -y clang-20
      - name: Include-closure assertions and negative control
        run: python3 scripts/check_include_closure.py --mode both --compiler clang++-20 --self-test
```
(`--require-all` appended by the integrator, C.3.)

**`scripts/symbol_report.py` — specification** (informational; `ARCHITECTURE.md:507`; name `monolith-symbol-report` from `:568`; no nm/size tooling exists in the tree today)
- `--before <.so>` `--after <.so>` `[--nm nm] [--size size] [--cxxfilt c++filt] [--markdown out.md] [--json out.json] [--strip-scope PREFIX]* [--rename-map OLD=NEW]* [--only-names A,B,…] [--threshold 0] [--self-test]`.
- Per side: `nm --defined-only -S <so>` → `{mangled: (type, size)}`; demangle via `c++filt` for the normalised key; apply `--strip-scope`/`--rename-map` textual substitutions to the demangled name (this folds `ProgramObject::LinkArtifacts` → `LinkArtifacts` everywhere, including template arguments such as `std::vector<ProgramObject::ResourceReflection>`); `size --format=sysv <so>` for `.text`/`.data`/`.bss`/Total.
- Buckets sorted by |Δ|: removed, added, resized, renamed-only (same normalised name, byte-identical size — the pure-move signature), unchanged (count). `--only-names` restricts the listing to symbols whose demangled text contains one of the names.
- Prints `symbol-report: .text B -> A (+d, +p%)` and `symbol-report: N -> M defined symbols: a added, r removed, s resized, n renamed`, then a Markdown table; always exit 0; `--fail-on-added-bytes N` reserved for a future hard gate.
- Docstring guard rails: same `CMAKE_BUILD_TYPE` on both sides (visibility presets differ, `CMakeLists.txt:578-598`), `MOBILEGL_ENABLE_LTO` OFF on both (`:108,137-146`), same compiler; print both `.so` paths and sizes in the header.
- `--self-test`: two tiny canned `nm -S` transcripts → expected buckets.

**Verification (WSL, in `~/w7/p05-include-gate`, base tree — no new headers yet)**
```
python3 scripts/check_include_closure.py --mode both --self-test                 # value-header SKIP, artifacts-header SKIP, wire-header OK, self-test OK, exit 0
python3 scripts/check_include_closure.py --mode both --self-test --require-all; echo exit=$?   # exit 1, message names the two missing headers
python3 scripts/check_include_closure.py --mode text --self-test                 # exit 0 (this is what the ctest runs)
python3 scripts/check_include_closure.py --mode both --compiler g++ --self-test  # trailer-line handling, exit 0
# red-for-its-reason, by hand: temporarily add `#include <MG_State/GLState/RenderState/RenderState.h>` to
# MobileGL/MG_Remote/Transport/ITransport.h -> probe wire-header FORBIDDEN with the printed chain; revert.
cmake -S . -B build-linux && ctest --test-dir build-linux -R MobileGLPurity --output-on-failure
ctest --test-dir build-linux -L unit -N | grep -c MobileGLPurity                  # 1 (the label reaches ctest -L unit)
python3 scripts/symbol_report.py --self-test
python3 scripts/symbol_report.py --before ~/w7/pipe/build-linux/libMobileGL.so --after build-linux/libMobileGL.so   # 0/0/0/0 on an unchanged tree
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/test.yml'))"     # syntax only
```
Commits: `[CI] (Purity): add scripts/check_include_closure.py - the -H include-closure gate for the P0.5 headers with an always-on negative control, wired as a unit ctest and the include-graph-check job` and `[Tooling] (Purity): add scripts/symbol_report.py - per-symbol nm/.text attribution between two libMobileGL.so builds`.

---

### B.4 File-ownership table (the guarantee that the three trees never touch the same file)

| file / glob | value-types | program-artifacts | include-gate | integrator |
|---|---|---|---|---|
| `MobileGL/MG_Pipe/MGPipeValueTypes.h` (new) | owner | – | – | – |
| `MobileGL/MG_Pipe/MGPipeTypes.h`, `scripts/gen_pipe.py`, `MobileGL/MG_Pipe/generated/*.inc` | owner | – | – | – |
| `MobileGL/MG_State/GLState/{RenderState/RenderState.h, RenderState/RenderState.cpp, SamplerState/SamplerObject.h, VertexArrayState/VertexArrayObject.h, FramebufferState/FramebufferObject.h}` | owner | – | – | – |
| `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | owner | – | – | – |
| `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h` (new), `ProgramObject.h` | – | owner | – | – |
| `MobileGL/MG_Test/Program/ProgramArtifactsTest.cpp` (new), `MobileGL/MG_Test/Program/CMakeLists.txt` | – | owner | – | – |
| `scripts/check_include_closure.py`, `scripts/symbol_report.py` (new), `MobileGL/MG_Test/Purity/**` (new) | – | – | owner | – |
| `MobileGL/MG_Test/CMakeLists.txt`, `.github/workflows/test.yml` | – | – | owner | flips `--require-all` after all land |
| `MobileGL/MG_Backend/**`, `MobileGL/MG_Impl/**`, `MobileGL/MG_Util/**`, other `MG_State/**` | include-line additions only, only if the build demands, placed after the TU's existing `MG_State/GLState/...` include | include-line additions only (commit 2), placed after the TU's `ProgramObject.h` include line | – | – |
| `docs/Disaggregated/*.md` | – | – | – | owner |
| `tools/**`, `android-plugin/**`, root `CMakeLists.txt` | nobody | nobody | nobody | nobody |

The one shared-file possibility is the "include-line additions" row: the two packages add at different anchors of a TU, so the hunks do not overlap; the integrator resolves an adjacent-hunk conflict by keeping both lines.

---

## C. Integration order and what the integrator runs

C.0 Preconditions: all three branches green on their own verification lists; `~/w7/pipe` clean at `6672778b` (fixture binaries hidden as in `wsl_integrate.sh`); capture the base library: `cp ~/w7/pipe/build-linux/libMobileGL.so ~/w7/p05-before-libMobileGL.so` (Release, `/usr/sbin/clang++`, built at `6672778b` — verify with `git -C ~/w7/pipe log -1` and `grep CMAKE_BUILD_TYPE ~/w7/pipe/build-linux/CMakeCache.txt`); capture `ctest -N` names and `grep -rc "pGLContext" MobileGL/MG_Backend | sort > ~/w7/p05-before-pglcontext.txt`.

C.1 Order: **include-gate → value-types → program-artifacts.** The gate first, so probes A and B flip from SKIP to OK as each header lands and the integrator runs the real gate on each merge. The p05 trees are worktrees of `~/w7/pipe` (`git worktree list` shows them; branches are local), so the P0 helper's fetch step is unnecessary; per slug:
```
cd ~/w7/pipe && git checkout -q feat/disaggregated
git checkout -q p05/<slug> && git rebase feat/disaggregated            # conflict => abort, report, stop
git checkout -q feat/disaggregated && git merge --ff-only p05/<slug>
cmake --build build-linux --parallel $(nproc) && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
python3 scripts/check_include_closure.py --mode both --self-test       # after value-types: A OK; after artifacts: A+B OK
```
(`scratchpad/wf2/wsl_integrate.sh` is the P0 shape of this loop; it hard-codes `p0/` and `~/w7/p0-<slug>` and fetches — copy it to `wsl_integrate_p05.sh` with the prefix changed and the fetch removed rather than editing it.)

C.2 After all three: `python3 scripts/check_include_closure.py --mode both --self-test --require-all` (exit 0); `ctest -N` name diff against the C.0 capture (`comm -23` empty; additions listed in the merge message); `git diff --stat 6672778b -- MobileGL/MG_Backend` shows include-line additions only (or nothing); `grep -rc "pGLContext" MobileGL/MG_Backend | sort | diff - ~/w7/p05-before-pglcontext.txt` empty; `python3 scripts/gen_pipe.py --check`.

C.3 Flip the ratchet (one commit, integrator-owned): append `--require-all` to the ctest COMMAND in `MobileGL/MG_Test/Purity/CMakeLists.txt` and to the CI step in `test.yml`; message `[CI] (Purity): require both P0.5 closure probes now that the headers exist`.

C.4 Symbol report: `python3 scripts/symbol_report.py --before ~/w7/p05-before-libMobileGL.so --after build-linux/libMobileGL.so --strip-scope 'MobileGL::MG_State::GLState::ProgramObject::' --markdown ~/w7/p05-symbol-report.md`. Expected: `.text` unchanged (the library gains no code: the `VisitFields` templates are uninstantiated outside the test binary, all moved types are declarations); 0 added / 0 removed / 0 resized after normalisation; N renamed. Anything else is attributed line by line in the report before the `dev` merge. Paste the two summary lines into the merge commit. Android/libc++ sizes: build the plugin APK once (`gradle -p android-plugin :app:assemblePluginRelease`, see `apk.yml:78`) with a temporary `static_assert(sizeof(LinkArtifacts) == 1)` (and the other three) to read the values from the NDK clang notes, then pin the `_LIBCPP_VERSION` branch of `ProgramArtifacts.h` and define `MGL_ARTIFACT_SIZES_LIBCXX_PINNED` there; message `[Test] (Program): pin the libc++ artifact sizes`.

C.5 CI and docs: `gh workflow run test.yml --ref feat/disaggregated` (and `apk.yml` if dispatchable) — record run URLs; `include-graph-check`, `pipe-gates`, `test` green. Then docs (integrator-owned): `docs/Disaggregated/README.md:3` status → P0.5 landed with the commit hash; `ROADMAP.md:16` ✅ with the "7 includers" wording corrected to "8 includers, none edited (in-class aliases)"; `ARCHITECTURE.md:260` note that `DynamicBackendParameters` stayed (D1) and its `:563` stale citation `MG_Test/CMakeLists.txt:93-95` → `:100-103`. Then `[Merge]` to `dev` per the milestone flow, and schedule the full `gl44to46` caselist (`ROADMAP.md:34`) on the two devices per the Wave-7 protocol — device time, not on the integration critical path.

---

## D. Risks and mitigations

| risk | mitigation |
|---|---|
| **ODR — a struct copied, not moved.** Two same-named definitions in `namespace MobileGL` (value types) compile per-TU and silently break Espryt's `offsetof` shadow if they drift; for the artifacts, `GLState::TypeFacts` vs `GLState::ProgramObject::TypeFacts` compile as two different types. | The per-package ODR greps (exactly one `struct X` per name) are mandatory verification, and the alias `static_assert(is_same_v<…>)` in `ProgramArtifactsTest.cpp` catches the artifacts case at compile time. |
| **Include cycles.** MG_State now includes MG_Pipe (`RenderState.h`, `SamplerObject.h`, `VertexArrayObject.h`, `FramebufferObject.h` → `MGPipeValueTypes.h`). | `MGPipeValueTypes.h` includes only `<Includes.h>`/`VectorTypes.h` and forward-declares `BufferObject`; the gate makes any MG_State include in it red; MG_Pipe is "永远进构建" and below MG_State (`ARCHITECTURE.md:550-561`), so the direction is the intended one. `ProgramArtifacts.h` includes nothing from `ProgramState/`, so `ProgramObject.h → ProgramArtifacts.h` cannot cycle. |
| **Lost transitive includes.** `RenderState.h` stops dragging `FramebufferObject.h → TextureObject.h → RenderbufferObject.h → SamplerObject.h …` (8 headers) into the 73 `Core.h` consumers and its 5 direct includers; `ProgramObject.h` (commit 2) stops dragging `SpvcSession.h`/`<spirv_reflect.h>`. | Only the compiler enumerates the fallout; the rule is "add the direct include at the consumer, never at the forwarding header", one anchor per package (B.4), listed in the commit message. `RenderState.cpp` is the one known case (fixed in the package). |
| **MSVC / Android not built locally.** `test.yml` builds Linux only (clang-20, `build-linux`); `apk.yml` builds the NDK APK; neither runs on `feat/disaggregated` pushes; the Windows box is not a correctness gate. | Nothing in P0.5 is platform-specific: no `SizeT` widening (D1/D5), no language feature beyond C++20 constraints (`requires std::same_as`; fine on MSVC 19.3x and NDK r27 clang), `offsetof` on `RenderStateParameters` is already compiled on all three toolchains by `DirectGLES.cpp`. Container-bearing `sizeof` assertions are libstdc++-only until the integrator pins libc++ (C.4); MSVC is unasserted. The integrator's `workflow_dispatch` of `apk.yml` is the Android build gate; a `cmake --build` in the Windows worktree before the `dev` merge is cheap and recommended, not required. |
| **glslang types leaking by value in the archive.** `LinkArtifacts::program` and `uniformInitialValues` keep glslang types (D5). | Symbol-free by construction (type-erased deleter; aggregate with inline implicit members); the gate pins `glslang::` at exactly 2 occurrences in `ProgramArtifacts.h`, so a third leak is red; `VisitFields(LinkArtifacts)` excludes `program` and the future deserializer must assert it null (`ProgramTranslationCache.h:42-50` already asserts that at insert). The `UniformInitializer` re-typing is the recorded P7 follow-up, together with narrowing `ProgramTranslationCache.h`/`ProgramInterface.cpp` to the new header and moving `MAX_UNIFORM_LOCATIONS`' glslang RHS behind a client-side assert. |
| **`PipeInputs` (P1) needs.** `ARCHITECTURE.md:328`: accessors return "exactly the types the backend reads today"; `:344`: 293 `pGLContext->` sites + 58 non-arrow lines are `sed`'d mechanically; `ResidualValueBlock` needs `offsetof` computability; `MOBILEGL_PIPE_VERIFY` keeps its `MG_State` includes (`:369`). | D3 keeps every type's qualified name; the value header keeps standard layout and asserts it; `MG_Backend/**` is diff-empty apart from possible include lines; the `pGLContext` count is captured before/after (C.0/C.2); the value header coexists with `MG_State` includes (no mutual exclusion). `PipeFields.def` field lists for the value types are P1 work (D8), and `MEMCMP_FALLBACK_TYPES` in `gen_pipe.py:166-171` stays as is until then. |
| **`RenderStateParameters` is about to change in P2** (`FramebufferSrgb`/`DepthClamp` storage, `ROADMAP.md:18,78`), moving `sizeof` off 1168. | After P0.5 every 1168-dependent number lives in exactly two places: the value header's assertions and `MGL_RESIDUAL_BLOCK_SIZE` (`MGPipeTypes.h:535`); the test twins name both. P2 updates both in one commit. |
| **A gate that is green because it never ran.** `feat/disaggregated` is not a trigger branch (`test.yml:3-8`); a mislabelled ctest never runs under `ctest -L unit`. | `LABELS unit` verified by `ctest -L unit -N`; the ctest runs in every local build; the integrator dispatches the workflow and records the URL (D10); `--self-test` is always on, so a broken parser cannot pass silently; `--require-all` after integration makes SKIP red. |
| **`#pragma once` with two include spellings** (`<MG_Pipe/X.h>` and `<X.h>`, `CMakeLists.txt:531,535`) on case-insensitive/symlinked Windows worktrees. | The macro guard in `MGPipeValueTypes.h`. |
| **`nm` attribution looks catastrophic** because de-nesting renames every mangled name mentioning the five artifact types (including every `std::vector<…>` instantiation). | `symbol_report.py --strip-scope` folds them into the renamed-only bucket with byte-identical sizes; the value-types package contributes zero churn by D3, so the two packages are separable in the report by construction. |
