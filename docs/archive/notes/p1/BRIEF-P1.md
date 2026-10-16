# P1 implementation brief — PipeInputs strangler, per-verb fill points, poison generations, MOBILEGL_PIPE_VERIFY comparator and its CI mode

Tree: `feat/disaggregated @ 087685d1` (worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`; WSL `~/w7/pipe` is a worktree of the same repo at `087685d1`, branch `feat/disaggregated`, with a configured `build-linux/` — Release, `/usr/sbin/clang++` (clang 22.1.6; there is no `clang++-20` in WSL), ccache, `MOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO`, `MOBILEGL_BUILD_TEST=ON`, `MOBILEGL_BUILD_INTEGRATION_TEST=ON`, `MOBILEGL_BUILD_DISAGGREGATED=OFF`, `MOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json`, `MOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json`, `MOBILEGL_ITEST_REQUIRE_GPU=OFF`; `build-retrace/` exists; 94 fixture files under `tools/trace_replay/fixtures/`; 28 cores). `~/w7/base` is `dev@81b17c0b` (the performance anchor, untouched). The P0.5 tooling in `scratchpad/wf2/` (`wsl_p05_trees.sh`, `wsl_integrate_p05.sh`, `retrace_gate.py`, `wsl_retrace_baseline.sh`) is the shape every WSL step below copies with `p05`→`p1`.

Every `file:line` below was re-opened at `087685d1` (the scouts were written at `5635e33f`; the one commit between is CI-only, `.github/workflows/test.yml`, and every scout line number still holds). Corrections to the scouts are marked **[correction]**; deliberate deviations from the design documents are marked **[deviation]** and carry their reason.

**Scout verification summary.** The pull-site catalogue (`scout-pull-sites.md` §1) was cross-checked mechanically against the live tree (`scratchpad/wf4/xcheck.py`): all 335 `pGLContext` occurrences on 332 lines in 12 files match the catalogue line-for-line and accessor-for-accessor (the script's one "DIFF" for `VulkanRenderer.cpp` is the parser swallowing the §5 sync tables that follow that heading; 158 − 26 = 132 = live). Spot-reads of 40+ sites across both backends confirmed the `enclosing function` / `verb path` columns with one exception, **[correction]** `VkRenderPassManager.cpp:1111` sits inside `VkRenderPassManager::GetOrCreateRenderPass` (the `ConvertTextureInternalFormatToVkEnum` the catalogue names at `:1044` is `MG_Util::`'s, a different function that reads nothing), so all three `VkRenderPassManager.cpp` reads are render-pass creation — reached from draws, clears and blits only. No `pGLContext` use exists in `MG_Backend` outside the 12 catalogued files (`Init.cpp`, `BackendObject.*`, `BackendObjects.h` and every other `.h` are clean); no other `MG_State::` global is reached (`MG_State::pGLContext` is the only one, 335/335). Other corrections: `scout-docs-spec.md` says "~34 `MOBILEGL_ASSERT`" — the live count is **43** (34 in `DirectVulkan.cpp` + 9 in `UniformManager.cpp`); `scout-pull-sites.md` cites `MGPipeHostSpan.h:119-154` — the file is 57 lines, `struct MGHostSpan` is `:28-37`; `scout-test-infra.md` B2/B4 gate the poison on `MOBILEGL_DEBUG`, which does not exist anywhere in the tree (D2 below). The MG_Impl boundary is **83 call statements over 69 distinct table entries**, not "~93 sites" (the 91-line grep counts null-checks and captured-pointer lines; §B.7). `Present` and `SetSwapInterval` are never called through `gBackendFunctionsTable` from `MG_Impl` — EGL goes through `BackendObject` virtuals (`EGLImpl.cpp:178,448`) — and neither backend's present path reads `pGLContext`, so they are not fill points.

---

## A. Goal and acceptance gate

`docs/Disaggregated/ROADMAP.md:17`, verbatim:

> **P1** `PipeInputs` 替换与 verify harness | 10–13 | `MG_Backend/MGPipe/PipeInputs.h`（Espryt 32 / Magma 55 访问器）；`sed` 293 处 + 58 行非箭头清单逐条转换（显式交付物）；逐 verb 类填充点（G5 表，~93 个边界站点）；逐 verb 世代 poison；G4 影子比对器 + 第三种 CI 模式；20 处 `SyncPersistentMappedRange` + 6 处 `SyncGpuWrites` 的逐站点归属表 | pull 构建 `nm --defined-only` 不变、`.text` 差异逐行归因（空守卫/三元重写推迟到 P2）；40 trace + 全部集成测试在 `MOBILEGL_PIPE_VERIFY=1` 下零分歧；故意损坏一个快照字段能让 verify 变红；故意在 `glGenerateMipmap` 的填充表漏一个字段能在**那条 verb** 上触发 poison Fatal | P0.5

Binding context: `ARCHITECTURE.md:325-340` (the `PipeInputs`/`MGB_CTX` sketch), `:342-347` (phase A/B/C table — P1 is phase A plus the phase-B scaffolding: verify harness, poison, fill points), `:348-349` (fill points per verb class; poison is a per-verb generation, `Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}`; gate C greps `pGLContext`, not `pGLContext->`), `:501` (gate 13.2-②: two state models in one address space, `SnapshotFromGLContext()`, field-wise, first differing field + draw serial, third CI mode 5–10× slower, never shipped, survives P13), `:369` (P13 keeps `MOBILEGL_PIPE_VERIFY` with the `MG_State` include it needs), `:553` (`MG_Backend/MGPipe/` is created in P1), `:583` (`MOBILEGL_PIPE_VERIFY` is a **CMake** option, planned P1). `ROADMAP.md:7`: **每个门必须能因它存在的理由变红**; Windows is not a correctness gate. `ROADMAP.md:38`: day 25 is the P1 exit, "verify harness 逐 draw 逐字段证明推送等价于拉取", not a GO/NO-GO. `ROADMAP.md:46`: the first GO/NO-GO item is P1's proof.

Decoded into checkable statements:

| # | statement | how it is checked |
|---|---|---|
| G1 | The **pull build** (`MOBILEGL_PIPE_PUSH=OFF`, `MOBILEGL_PIPE_VERIFY=OFF`, Release/INFO, LTO off) has an identical `nm --defined-only` symbol set before and after P1, and a `.text` byte delta of **zero**; if not zero, every differing symbol is attributed to one of the 58 converted non-arrow lines (D9). | `scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --json` → `added == removed == resized == renamed == 0`, `.text` delta 0 (C.integrator). |
| G2 | Every backend read of frontend state goes through `MGB_CTX` (335 occurrences → 0 `pGLContext` tokens in `MG_Backend/**` except inside the pull arm of the one switch header, which lives in `MG_Pipe/`). | `grep -rc "pGLContext" MobileGL/MG_Backend | grep -v ":0$"` is empty. |
| G3 | In the **verify build** (`MOBILEGL_PIPE_VERIFY=ON` ⇒ `MOBILEGL_PIPE_PUSH=ON`), with `MOBILEGL_PIPE_VERIFY=1` in the environment, the 40-trace retrace (79 desktop cases: 40 × 2 backends minus `iterationrp × DirectGLES`; `create-indirect` waived on device only) and **every** integration entry (the 742-entry `integration-verify` lane) run with zero `Fatal{PipeVerifyDiffer` and zero `Fatal{UnmigratedPipeInput` and the arming line present in every log. | `retrace_gate.py` under `MOBILEGL_PIPE_VERIFY=1` against the verify `.so`; `ctest -L integration-verify --no-tests=error`; `run_trace_case.cmake`'s arming/differ assertions (E). |
| G4 | Negative control A: `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters` turns a green verify run red, naming that field and the verb serial. | unit (A), lane (E), CI step (E). |
| G5 | Negative control B: `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit` makes `glGenerateMipmap` abort with `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}` on **that** call, and no other verb is affected (a sibling field on the same verb stays fresh; the preceding draw does not abort). | unit (A), fork/waitpid scenario per backend (E). |
| G6 | Every one of the 20 `SyncPersistentMappedRange` and 6 `SyncGpuWrites` sites has a row in the ownership table (D10) stating the §5.7 reconcile class it must reproduce; the `*IndirectCount` rows record today's behaviour verbatim (open question #15, not fixed in P1). | the table is in this brief; the integrator lands it in `docs/Disaggregated/MEASUREMENTS.md`. |
| G7 | Existing test names unchanged (additions only); `pipe-gates`, `include-graph-check`, `test`, `integration`, `retrace` all green; the new generated file is regenerated by `gen_pipe.py` and diffed by CI. | `ctest -N` name diff (`comm -23` empty); `gh workflow run test.yml --ref feat/disaggregated`. |
| G8 | The gate itself can go red for its reason: the arming assertion fails a lane whose library never armed; `--no-tests=error` fails a lane whose label matched nothing; the two negative controls are always-on CI steps. | E. |

---

## B. Fixed design decisions (every package follows these; nothing here is re-decided inside a package)

### D1 — Counts, measured at `087685d1`

| what | value |
|---|---|
| `pGLContext` occurrences / lines in `MG_Backend/**` | **335 / 332** (3 lines hold two: `VulkanRenderer.cpp:9214,10719,10963`) |
| arrow sites `pGLContext->` | **277** occurrences on 274 lines: `DirectGLES.cpp` 91, `Managers.cpp` 15, `MultiDraw.cpp` 5, `Utils.cpp` 2, `BackendObject_DirectVulkan.cpp` 2, `DirectVulkan.cpp` 12, `UniformManager.cpp` 14, `VkClearManager.cpp` 1, `VkRenderPassManager.cpp` 3, `VkTextureManager.cpp` 2, `VulkanRenderer.cpp` 127 (130 occurrences) |
| non-arrow lines | **58**: 43 `MOBILEGL_ASSERT`, 7 null guards, 5 `!= nullptr` outside asserts, 1 `== nullptr`, 1 `.get()`, 1 comment (D9 lists every line) |
| distinct accessors read | **62** (Espryt 32, Magma 56) + one `.get()` |
| `PipeInputs` field set | **63** = the 61 rows of `Coverage.def:30-97` + `GetBoundTransformFeedbackLifetimeId` + `HasOpenTransformFeedbackSpan` (D13). `GetBoundTransformFeedbackName` (`Coverage.def:34`) is read by no backend; it stays as a field so the vendored inventory row `backend_read_inventory.md:594` keeps its mapping (`kMGPipeInventoryUnmapped == 0` is asserted at `PipeCatalogueTest.cpp:213-214`), and is marked dead in the header. |
| MG_Impl fill points | **83** call statements over **69** verbs (every function-pointer member of `GLFunctionsTable`, `BackendObject.h:117-289`); `CompileEnv.cpp:135,137` (`MG_Util`) also calls `GL.GetIntegeri_v` and gets no fill point (the entry reads no frontend state on either backend) |
| `SyncPersistentMappedRange` / `SyncGpuWrites` | 20 / 6, exactly as catalogued (D10) |

ROADMAP's "293 / 32+55 / ~93" are the vendored-inventory numbers; the integrator corrects them in the docs (C.5). `MEASUREMENTS.md:82`'s "61 个 PipeInputs 字段" becomes 63.

### D2 — Switches and their spellings

| switch | kind | default | meaning |
|---|---|---|---|
| `MOBILEGL_PIPE_PUSH` | CMake `option()` + compile definition `-DMOBILEGL_PIPE_PUSH=1` (in `MOBILEGL_COMPILE_DEF`, `CMakeLists.txt:523-525` shape) | OFF | `MGB_CTX` expands to `&gPipeInputs`; the fill points and the filler are compiled; `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp` and `MobileGL/MG_Impl/Pipe/PipeFill.cpp` are appended to `SOURCE_FILES`. OFF is the pull build and must be byte-identical to today. |
| `MOBILEGL_PIPE_VERIFY` | CMake `option()` + `-DMOBILEGL_PIPE_VERIFY=1`; **forces `MOBILEGL_PIPE_PUSH` ON** (`set(MOBILEGL_PIPE_PUSH ON)` as a normal variable in the configure that sees VERIFY, with a `message(STATUS)`) | OFF | compiles the second snapshot, the entry comparator, the compare-at-read hook and the two negative-control knobs. Never shipped. |
| `MOBILEGL_PIPE_POISON` | derived preprocessor macro, defined **once**, in `MobileGL/MG_Backend/MGPipe/PipeInputs.h`: `#if MOBILEGL_PIPE_PUSH && (MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG || MOBILEGL_BUILD_DISAGGREGATED || MOBILEGL_PIPE_VERIFY)` → `#define MOBILEGL_PIPE_POISON 1` else `0` | — | the per-verb generation stamps and the read-side `Fatal{UnmigratedPipeInput}` check. **[correction]** the docs' `MOBILEGL_DEBUG` does not exist; the repo's debug gate is `MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG` (`Defines.h:104-115`). The verify CI build is Release/INFO with `MOBILEGL_BUILD_DISAGGREGATED=OFF`, so the third arm is what arms the poison there without dragging `MG_Remote` in. |
| runtime `MOBILEGL_PIPE_PUSH` (`Features.PipePush`, `Config.h:326`, `ConfigLoader.cpp:245`) | Uint64 bitmask | 0 | **not consumed in P1.** The P1 filler fills every field in a verb's mask regardless of the bitmap; P2 makes the filler yield per bit to the tracker. Bit 63 stays reserved for the CSO negative control. Same name for the macro and the env var is deliberate (`ARCHITECTURE.md:334,367`): the macro selects the arm, the bitmap selects subsystems inside it. |
| runtime `MOBILEGL_PIPE_VERIFY` (`Features.PipeVerify`, `Config.h:332`, `ConfigLoader.cpp:246`) | Bool | 0 | arms the comparator at run time in a verify build. Ignored (with one `MGLOG_W_ONCE`) in a build where `MOBILEGL_PIPE_VERIFY` is not compiled in — that warning is what the arming assertion turns into red. |
| runtime `MOBILEGL_PIPE_VERIFY_FATAL` | Bool, new `Features.PipeVerifyFatal`, parsed `QueryEnvQuirkOverride(...) != ForceOff` (tri-state, like `PipeLegacyMemos`, `ConfigLoader.cpp:251-252`) | 1 | first divergence aborts (`MGLOG_F` + `std::abort()`); `0` logs and counts, for triage and for the lane that must survive to read its own log. |
| runtime `MOBILEGL_PIPE_VERIFY_CORRUPT` | String, new `Features.PipeVerifyCorrupt` (`QueryEnvVariable`) | "" | `<FieldName>` from `kMGPipeInputFieldNames[]`; perturbs that field in the **snapshot** arm before the entry compare. Unknown name → `Fatal{PipeVerifyBadKnob}`. |
| runtime `MOBILEGL_PIPE_POISON_OMIT` | String, new `Features.PipePoisonOmit` | "" | `<Verb>:<FieldName>` (`kMGPipeVerbNames[]` × `kMGPipeInputFieldNames[]`); the filler skips the **stamp** (not the value) of that field for that verb. Unknown name → `Fatal{PipeVerifyBadKnob}`. |

The three new `Features` members and their three parser lines are wrapped in `#if MOBILEGL_PIPE_PUSH` in `Config.h` and `ConfigLoader.cpp` so the pull build's `Features` object is unchanged in size (G1). `ConfigLoader.cpp:242-244`: no allow-list edit is needed for `MOBILEGL_*` names.

### D3 — `MGB_CTX` and the switch header (exact text; A creates it, B and C include it)

`MobileGL/MG_Pipe/PipeInputsSwitch.h` — lives under `MG_Pipe/`, never under `MG_Backend/`, so the pull arm's `pGLContext` spelling is outside what purity gate C greps (`ARCHITECTURE.md:500,349`):

```cpp
// MobileGL - MobileGL/MG_Pipe/PipeInputsSwitch.h
// (banner lines 2-7 as in MGPipe.h:2-7)
#pragma once
#ifndef MOBILEGL_MG_PIPE_INPUTS_SWITCH_H   // belt and braces: reachable as <MG_Pipe/..> and <..> (CMakeLists.txt:531,535)
#define MOBILEGL_MG_PIPE_INPUTS_SWITCH_H
// The strangler switch (ARCHITECTURE.md 9.2). Every backend read of frontend state is spelled
// MGB_CTX->Accessor(...). Pull arm: the live GLContext, so the pull build is the tree before P1
// token for token. Push arm: the PipeInputs block the frontend fills at every verb boundary.
// The pull arm is the ONLY place under MobileGL/ outside MG_State and MG_Impl that may spell
// pGLContext; purity gate C greps MG_Backend/ for that token.
#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/MGPipe/PipeInputs.h>
#define MGB_CTX (&::MobileGL::MG_Pipe::gPipeInputs)
#define MGB_CTX_LIVE (::MobileGL::MG_Pipe::gPipeInputs.IsLive())
#define MGB_CTX_IDENTITY (::MobileGL::MG_Pipe::gPipeInputs.ContextIdentity())
#else
#include <MG_State/GLState/Core.h>
#define MGB_CTX (::MobileGL::MG_State::pGLContext)
#define MGB_CTX_LIVE (::MobileGL::MG_State::pGLContext != nullptr)
#define MGB_CTX_IDENTITY (static_cast<const void*>(::MobileGL::MG_State::pGLContext.get()))
#endif
#endif
```

`MG_State::pGLContext` is `extern UniquePtr<GLState::GLContext>& pGLContext;` (`Core.h:596`), so `MGB_CTX->X()` in the pull arm is exactly today's `MG_State::pGLContext->X()`, and `MGB_CTX_LIVE` is `UniquePtr::operator bool`'s definition spelled out. Backend TUs include `<MG_Pipe/PipeInputsSwitch.h>` immediately after their `MG_State/GLState/Core.h` include (`DirectGLES.cpp:18`, `MultiDraw.cpp:11`, `Utils.cpp:19`, `BackendObject_DirectVulkan.cpp:14`, `DirectVulkan.cpp:12`, `UniformManager.cpp:12`, `VkClearManager.cpp:15`, `VkTextureManager.cpp:13`, `VulkanRenderer.cpp:16`; `Managers.cpp` gets it after its `Managers.h` include, `VkRenderPassManager.cpp` after its own include block — neither includes `Core.h` directly).

### D4 — `PipeInputs`: one struct, three field classes, types identical to today's reads

`MobileGL/MG_Backend/MGPipe/PipeInputs.h`, `namespace MobileGL::MG_Pipe`, `struct PipeInputs` + `inline PipeInputs gPipeInputs{};` (the single global the docs name, `ARCHITECTURE.md:331`; inline variable, no `.cpp` needed for the definition). Includes: `<MG_Pipe/MGPipe.h>` (field ids, verb enum, poison helpers), `<MG_State/GLState/Core.h>` (the frontend types the accessors return — allowed: P13 keeps the `MG_State` include for the verify arm, `ARCHITECTURE.md:369`; the header spells **no** `pGLContext` token). Every accessor has the **same name, parameters and return type** as its `GLContext` counterpart (`Core.h:66-506`), so the `sed` is type-neutral (phase A rule, `ARCHITECTURE.md:328`). Three storage classes:

- **V (value)** — copied out of `GLContext` at fill time by calling the same accessor (`m_x = ctx->GetX(...)`; no derivation logic is re-implemented in `PipeInputs`, which is what keeps the copy semantically identical by construction). Accessor returns the copy (by value or `const&`, whichever `Core.h` uses).
- **O (object reference)** — a `SharedPtr` copy, or a raw pointer to the live `GLContext`-owned slot/array for the accessors that return a non-const reference into the context. Same return type as today; identity is what phase C turns into a handle.
- **F (forwarded)** — argument-keyed lookups and reverse-channel calls. Declared in `PipeInputs.h`, **defined out of line in `MobileGL/MG_Impl/Pipe/PipeFill.cpp`** (the client side, where `pGLContext` may be spelled), forwarding to the live context. Sticky (D6).

| field (id from `kMGPipeInputFieldNames`) | class | storage in `PipeInputs` | accessor signature (= `Core.h`) | fill |
|---|---|---|---|---|
| GetActiveTextureUnit | V | `Int m_activeTextureUnit` | `Int GetActiveTextureUnit() const` | `ctx->GetActiveTextureUnit()` |
| GetBlendColor | V | `FloatVec4 m_blendColor` | `const FloatVec4& GetBlendColor() const` | |
| GetBlendEquationIndexed | V | `BlendEquation m_blendEquation[kMGMaxDrawBuffers][2]` | `void GetBlendEquationIndexed(Uint, BlendEquation&, BlendEquation&) const` | loop i<8 |
| GetBlendFuncIndexed | V | `BlendFactor m_blendFunc[kMGMaxDrawBuffers][4]` | `void GetBlendFuncIndexed(Uint, BlendFactor&×4) const` | loop i<8 |
| GetBoundTransformFeedbackName | V | `Uint m_boundTransformFeedbackName` | `Uint GetBoundTransformFeedbackName() const` | **dead field**: filled, never read (D1) |
| GetBoundVertexArray | O | `SharedPtr<VertexArrayObject> m_boundVertexArray` | `const SharedPtr<VertexArrayObject>& GetBoundVertexArray()` | copy |
| GetBufferBindingSlot | O | `BindingSlot<BufferObject>* m_bufferBindingSlot[BufferTargetCount]` | `BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget)` | `&ctx->GetBufferBindingSlot(t)` for every `t` in `GlobalBufferTargets` (`BufferState.h:15`); others null; accessor asserts non-null |
| GetBufferBindingPoint | O | `BindingSlotRange1D<BufferObject>* m_bufferBindingPointBase[BufferTargetCount]` | `BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget, Uint)` | `&ctx->GetBufferBindingPoint(t, 0)` for `t` in `BufferBindPointTargets` (`BufferState.h:20`); storage is `Array<Array<..., 84>, N>` (`BufferState.h:73`) so `base[index]` is the live slot |
| GetBufferBindingPointCount | F | — | `SizeT GetBufferBindingPointCount(BufferTarget) const` | forward (constexpr table) |
| GetTouchedBufferBindingPointCount | V | `SizeT m_touchedBindingPointCount[BufferTargetCount]` | `SizeT GetTouchedBufferBindingPointCount(BufferTarget) const` | loop over `BufferBindPointTargets` |
| GetClampReadColor | V | `GLenum` | `GLenum GetClampReadColor() const` | |
| GetClearColor / GetClearDepth / GetClearStencil | V | `FloatVec4` / `Float` / `Uint32` | as `Core.h:272-276` | |
| GetColorMaskIndexed | V | `BoolVec4 m_colorMask[kMGMaxDrawBuffers]` | `BoolVec4 GetColorMaskIndexed(Uint) const` | loop |
| GetCullFaceMode | V | `CullFaceMode` | | |
| GetCurrentVertexAttribute | V | `CurrentVertexAttributeValue m_currentVertexAttribute[VertexArrayObject::MAX_VERTEX_ATTRIBS]` (32, `VertexArrayObject.h:24`) | `const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint) const` | loop |
| GetDepthFunc / GetDepthMask | V | `DepthTestFunc` / `Bool` | | |
| GetDepthRangeIndexed | V | `FloatVec2 m_depthRange[RenderStateParameters::MAX_VIEWPORTS]` | `const FloatVec2& GetDepthRangeIndexed(Uint) const` | loop 16 |
| GetFramebufferBindingSlot | O | `BindingSlot<FramebufferObject>* m_framebufferBindingSlot[FramebufferTargetCount]` | `BindingSlot<FramebufferObject>& GetFramebufferBindingSlot(FramebufferTarget)` | both targets |
| GetImageTextureBinding | O | `ImageTextureBinding* m_imageTextureBindingBase` | `ImageTextureBinding& GetImageTextureBinding(Int)` (+ const overload) | `&ctx->GetImageTextureBinding(0)`; storage `Array<ImageTextureBinding, 192>` (`TextureState.h:129`) |
| GetLineWidth / GetLogicOp / GetMaxTouchedTextureUnit / GetMinSampleShadingValue | V | `Float` / `LogicOperation` / `Int` / `Float` | | |
| GetPatchDefaultInnerLevel / GetPatchDefaultOuterLevel / GetPatchVertices | V | `FloatVec2` / `FloatVec4` / `Uint` | `const FloatVec2&` / `const FloatVec4&` / `Uint` | |
| GetPipelineStateVersion / GetRenderStateParametersVersion | V | `Uint` / `Uint` | | |
| GetPixelStoreParameters | V | `PixelStoreParameters m_pixelStore[2]` (`[0]`=pack, `[1]`=unpack) | `PixelStoreParameters GetPixelStoreParameters(Bool isUnpack) const` | both |
| GetPolygonModeFront / GetPolygonOffsetFactor / GetPolygonOffsetUnits / GetPrimitiveRestartIndex / GetProvokingVertexMode | V | `GLenum` / `Float` / `Float` / `Uint32` / `ProvokingVertexMode` | | |
| GetProgramForDispatch / GetProgramForDraw / GetTransformFeedbackProgram | O | `SharedPtr<ProgramObject>` ×3 | `const SharedPtr<ProgramObject>& ...()` | copy (`GetProgramForDraw()` is called once per fill; it has a guarded static, `DirectGLES.cpp:2965-2968`) |
| GetProgramObject | F | — | `const SharedPtr<ProgramObject>& GetProgramObject(Uint)` | forward |
| GetRenderStateParameters | V | `RenderStateParameters m_renderState` (1168 B) | `const RenderStateParameters& GetRenderStateParameters() const` | memcpy-by-assignment |
| GetSamplingResolutionGeneration / GetTextureBindGeneration / GetTextureContextId | V | `Uint64` ×3 | | |
| GetScissorBox | V | `IntVec4` | `const IntVec4& GetScissorBox() const` | |
| GetStencilState | V | `StencilFaceState m_stencil[2]` | `const StencilFaceState& GetStencilState(StencilFace) const` | both faces |
| GetTextureObject | F | — | `const SharedPtr<ITextureObject>& GetTextureObject(Uint)` | forward |
| GetTextureUnitObject | O | `TextureUnit* m_textureUnitBase` | `TextureUnit& GetTextureUnitObject(Int)` | `&ctx->GetTextureUnitObject(0)`; storage `Array<TextureUnit, MAX_TEXTURE_IMAGE_UNITS=192>` (`TextureState.h:41,128`) |
| GetTransformFeedbackCapturedVertices / GetTransformFeedbackGeneration / GetTransformFeedbackPausedPrimitiveCounter / GetBoundTransformFeedbackLifetimeId | V | `Uint64` ×4 | as `Core.h:334,349,357,441` | |
| GetViewport | V | `IntVec4` | `IntVec4 GetViewport() const` | |
| GetViewportIndexed | V | `FloatVec4 m_viewport[MAX_VIEWPORTS]` | `const FloatVec4& GetViewportIndexed(Uint) const` | loop 16 |
| IsCapabilityEnabled | V | `Bool m_capability[CapabilityInputCount]` (37 entries, `MGPipeValueTypes.h:168-204`) | `Bool IsCapabilityEnabled(CapabilityInput) const` | loop over all caps (`FramebufferSrgb` copies today's constant `false`, `MEASUREMENTS.md:84` — no change of value) |
| IsCapabilityEnabledIndexed | V | `Bool m_blendIndexed[kMGMaxDrawBuffers]`, `Bool m_scissorIndexed[MAX_VIEWPORTS]` | `Bool IsCapabilityEnabledIndexed(CapabilityInput, Uint) const` | `Blend` ×8, `ScissorTest` ×16; any other cap → `Fatal{UnmigratedPipeInput}` with the cap name (no backend asks; `VulkanRenderer.cpp:5437` asks `Blend`) |
| IsTransformFeedbackActive / IsTransformFeedbackPaused | V | `Bool` ×2 | | |
| HasOpenTransformFeedbackSpan | F | — | `Bool HasOpenTransformFeedbackSpan(Uint64) const` | forward |
| InvalidateCompileEnv / ValidateProgramName / RecordError | F | — | `void InvalidateCompileEnv()` / `Bool ValidateProgramName(Uint) const` / `void RecordError(ErrorCode, UniquePtr<ErrorInfo>)` | forward (`RecordError` forwards even when no context is live? no: `IsLive()` false → drop with `MGLOG_E_ONCE`; today's guarded sites (`Managers.cpp:8750`) never reach it without a context) |

Non-field members: `const void* m_contextIdentity` (the live `GLContext` address at fill; `ContextIdentity()` serves `MGB_CTX_IDENTITY`), `Bool m_live` (`IsLive()`), `MGPipeVerb m_currentVerb`, and under `MOBILEGL_PIPE_POISON` an `MGPipeFilledState m_filled` (`PipeFilled.inc:293-296`, `CurrentVerbSerial` + `FilledGen[63]`). Estimated size ≈ 3.5 KB (well under the docs' ~20 KB). `static_assert(sizeof(PipeInputs) < 20 * 1024)`.

Every accessor body is: `MGP_INPUT_CHECK(MGPipeInputField::X);` (poison, D6) → `MGP_INPUT_VERIFY_READ(MGPipeInputField::X, idx0, idx1);` (compare-at-read, D8) → return the storage. Both macros expand to nothing when their switch is off, so a plain `MOBILEGL_PIPE_PUSH` build's accessor is a load.

Known gap, closed in P2 (Espryt 0b deletes `g_fbSlotCache`, `ROADMAP.md:18`): the five reads through `GetFramebufferBindingSlotFast` (`DirectGLES.cpp:1611,1905,2743,2858,2933`) bypass the accessor once the cache is warm, so they are not poison-checked; the entry compare still covers `GetFramebufferBindingSlot`.

### D5 — Files created, and where the `pGLContext` spelling is allowed

| path | owner | contents | may spell `pGLContext`? |
|---|---|---|---|
| `MobileGL/MG_Pipe/PipeInputsSwitch.h` | A | D3 | only in the `#else` arm |
| `MobileGL/MG_Pipe/FillPoints.def` | A | D7 source of truth | no |
| `MobileGL/MG_Pipe/generated/PipeFillPoints.inc` | A (generated) | verb enum, class tables, masks | no |
| `MobileGL/MG_Backend/MGPipe/PipeInputs.h` | A | D4 struct, accessors, `gPipeInputs`, `MOBILEGL_PIPE_POISON`, the two accessor macros | **no** |
| `MobileGL/MG_Backend/MGPipe/PipeInputs.cpp` | A | `MGPipeInputPoisonFatal` wrapper with verb name, per-field equality (`MGPipeInputsFieldEqual`), corruption injector | **no** |
| `MobileGL/MG_Impl/Pipe/PipeFill.h` | A | `MGP_FILL(Verb)` macro; declarations of `MGPipeFillForVerb`, `SnapshotFromGLContext` | no (macro expands to nothing under pull) |
| `MobileGL/MG_Impl/Pipe/PipeFill.cpp` | A | the filler (non-verify branch), `SnapshotFromGLContext` (verify branch), the F-class forwarders, `IsLive`, compare-at-read hook, entry compare, knob parsing | yes (client side) |

**[deviation]** `ARCHITECTURE.md:553` lists `MGPipeImpl_DirectGLES.cpp` / `MGPipeImpl_DirectVulkan.cpp` as `[P1+]`. P1 creates neither: nothing backend-specific exists in `PipeInputs` until a backend installs an `MGPipeContext` table (P5). The directory `MG_Backend/MGPipe/` is created with `PipeInputs.h/.cpp`; the integrator records this in the docs.

### D6 — Poison: per-verb generation, seven sticky fields, the Fatal

- `MGPipeFillForVerb(MGPipeVerb v)` (`PipeFill.cpp`) does, in order: `++m_filled.CurrentVerbSerial` (starts at 1 so `FilledGen == 0` means never filled); `m_currentVerb = v`; if `pGLContext == nullptr` → `m_live = false`, return; else `m_live = true`, `m_contextIdentity = ctx`, then for every field in `kMGPipeClassFieldMask[kMGPipeVerbClass[v]]`: copy per D4 and `m_filled.FilledGen[f] = CurrentVerbSerial` — **unless** `(v, f)` equals the parsed `MOBILEGL_PIPE_POISON_OMIT` pair, in which case the value is still copied and only the stamp is skipped (an omission indistinguishable from a forgotten row). Cost is irrelevant in P1 (verify build) and is replaced by the tracker in P2.
- `MGP_INPUT_CHECK(f)` under `MOBILEGL_PIPE_POISON`: `if (!MGPipeInputFieldIsFresh(m_filled, f)) MGPipeInputPoisonFatalForVerb(f, m_currentVerb);` → `MGPipeInputPoisonFatal(f, kMGPipeVerbNames[v])` (`PipeFilled.inc:298-302`: `MGLOG_F("MGPipe: Fatal{UnmigratedPipeInput, \"%s@%s\"}")` + `std::abort()`; before the first verb the name is `"<none>"`). `MGLOG_F` is live at every log level (`Log.h:87-92`) and this is not `MOBILEGL_ASSERT` on purpose (inert in INFO builds, `Defines.h:113-115`).
- **Sticky = exactly the seven F-class fields**: `RecordError`, `InvalidateCompileEnv`, `ValidateProgramName`, `GetProgramObject`, `GetTextureObject`, `HasOpenTransformFeedbackSpan`, `GetBufferBindingPointCount`. Argument (goes into `Coverage.def` as the reason string): each takes an argument that is not verb state — a GL name, a lifetime id, a target — i.e. it is a lookup or a reverse-channel write, not a state read; there is no value the filler could copy and no verb whose fill could make it stale; phase C replaces them with handle tables/callbacks. They are stamped once (`FilledGen = 1`) by the first fill that sees a live context and pass `MGPipeInputFieldIsFresh` through the `Sticky → FilledGen != 0` branch (`PipeFilled.inc:304-307`). **None of the version/generation accessors is sticky** (`GetTextureContextId`, `GetTextureBindGeneration`, `GetSamplingResolutionGeneration`, `GetTransformFeedbackGeneration`, `GetPipelineStateVersion`, `GetRenderStateParametersVersion` change under verbs and are precisely what the poison must protect; scout R5).
- `kMGPipeInputFieldSticky[]` stops being all-`false`: `Coverage.def` gains `MGP_COVERAGE_STICKY_LIST(X)` with seven `X(Accessor, "reason")` rows; `gen_filled` emits `true` for those and `sys.exit`s on a name that is not an accessor.
- `IsCapabilityEnabledIndexed` with a cap other than `Blend`/`ScissorTest`, and `GetBufferBindingSlot`/`GetBufferBindingPoint` with a target the fill left null, are also `MGPipeInputPoisonFatal` (the message names the field and the verb; the cap/target goes in a preceding `MGLOG_E`).

### D7 — Fill points: 69 verbs, 9 classes, one `.def`, one generated `.inc`, one macro

`MobileGL/MG_Pipe/FillPoints.def` (hand-maintained, A):

```
// X(Verb, Class) — one row per function-pointer member of MG_Backend::GLFunctionsTable (BackendObject.h:117-289),
// in declaration order. gen_pipe.py parses that struct and refuses a row set that is not exactly its member set.
#define MGP_FILL_VERB_LIST(X) \
    X(DrawArrays, kDraw) X(DrawElements, kDraw) ... X(GetGpuTimestampNs, kQuery)
// (must be followed by an empty line: gen_pipe.py's block regexes end at a blank line)

// X(Class) — the nine verb classes (ARCHITECTURE.md:153 names eight; kProgramOp is split out because
// ShaderStorageBlockBinding is the one non-draw verb that syncs Espryt's render state and textures).
#define MGP_FILL_CLASS_LIST(X) \
    X(kDraw) X(kDispatch) X(kClear) X(kBlitOrCopy) X(kTextureOp) X(kReadback) X(kXfbSpan) X(kProgramOp) X(kQuery)

// X(Class, Field) — the may-read table. A field named here is filled and stamped at every verb of the class;
// a read of a field NOT named here is Fatal{UnmigratedPipeInput, "Field@Verb"} in a poison build.
#define MGP_FILL_FIELD_LIST(X) \
    X(kDraw, GetBoundVertexArray) ...
```

Verb → class (69 rows; the count and the membership are the contract, verified by a `gen_pipe.py` parse of `struct GLFunctionsTable`):

| class | verbs |
|---|---|
| `kDraw` (20) | DrawArrays, DrawElements, DrawElementsBaseVertex, MultiDrawArrays, MultiDrawElements, MultiDrawElementsBaseVertex, MultiDrawElementsIndirect, MultiDrawArraysIndirect, MultiDrawElementsIndirectCount, MultiDrawArraysIndirectCount, DrawRangeElementsBaseVertex, DrawRangeElements, DrawElementsInstancedBaseVertexBaseInstance, DrawElementsInstancedBaseVertex, DrawElementsInstancedBaseInstance, DrawElementsInstanced, DrawElementsIndirect, DrawArraysInstancedBaseInstance, DrawArraysInstanced, DrawArraysIndirect |
| `kDispatch` (2) | DispatchCompute, DispatchComputeIndirect |
| `kClear` (9) | Clear, ClearBufferfi, ClearBufferfv, ClearBufferiv, ClearBufferuiv, ClearNamedFramebufferfv, ClearNamedFramebufferfi, ClearNamedFramebufferiv, ClearNamedFramebufferuiv |
| `kBlitOrCopy` (5) | BlitFramebuffer, BlitNamedFramebuffer, CopyTexImage2D, CopyTexSubImage2D, CopyImageSubData |
| `kTextureOp` (2) | GenerateMipmap, BindImageTexture |
| `kReadback` (3) | ReadPixels, GetTexImage, GetTextureImage |
| `kXfbSpan` (6) | BeginTransformFeedback, EndTransformFeedback, PauseTransformFeedback, ResumeTransformFeedback, BindTransformFeedback, DeleteTransformFeedback |
| `kProgramOp` (1) | ShaderStorageBlockBinding |
| `kQuery` (21) | BeginOcclusionQuery, EndOcclusionQuery, BeginTimeElapsedQuery, EndTimeElapsedQuery, QueryCounterTimestamp, BeginXfbPrimitivesQuery, EndXfbPrimitivesQuery, IsQueryResultAvailable, GetQueryResult64, DeleteBackendQuery, IsTimerQuerySupported, GetGpuTimestampNs, FenceSync, ClientWaitSync, WaitSync, GetSyncStatus, DeleteSync, MemoryBarrier, MemoryBarrierByRegion, PatchParameteri, GetIntegeri_v |

Class → may-read fields (from the verified reachability, `scout-pull-sites.md` §4.2, union over both backends; the seven sticky fields are implicit everywhere and are not listed). **The verify lane is the oracle: a `Fatal{UnmigratedPipeInput}` found there is fixed by adding the row to this table, never by marking the field sticky.**

| class | fields |
|---|---|
| `kDraw` | GetBoundVertexArray, GetProgramForDraw, GetBufferBindingSlot, GetBufferBindingPoint, GetTouchedBufferBindingPointCount, GetTextureUnitObject, GetTextureContextId, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetSamplingResolutionGeneration, GetImageTextureBinding, GetCurrentVertexAttribute, GetRenderStateParameters, GetRenderStateParametersVersion, GetPipelineStateVersion, GetViewport, GetViewportIndexed, GetDepthRangeIndexed, GetScissorBox, IsCapabilityEnabled, IsCapabilityEnabledIndexed, GetBlendColor, GetBlendFuncIndexed, GetBlendEquationIndexed, GetColorMaskIndexed, GetLogicOp, GetDepthFunc, GetDepthMask, GetStencilState, GetCullFaceMode, GetPolygonModeFront, GetPolygonOffsetFactor, GetPolygonOffsetUnits, GetLineWidth, GetMinSampleShadingValue, GetProvokingVertexMode, GetPatchVertices, GetPatchDefaultOuterLevel, GetPatchDefaultInnerLevel, GetPrimitiveRestartIndex, GetFramebufferBindingSlot, IsTransformFeedbackActive, IsTransformFeedbackPaused, GetTransformFeedbackProgram, GetTransformFeedbackGeneration, GetBoundTransformFeedbackLifetimeId, GetTransformFeedbackCapturedVertices |
| `kDispatch` | GetProgramForDispatch, GetBufferBindingSlot, GetBufferBindingPoint, GetTouchedBufferBindingPointCount, GetTextureUnitObject, GetTextureContextId, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetSamplingResolutionGeneration, GetImageTextureBinding, GetFramebufferBindingSlot, GetPatchVertices, GetPatchDefaultOuterLevel, GetPatchDefaultInnerLevel (Espryt `SyncCurrentProgram` → `AttachPassthroughTessControlStage`, `Managers.cpp:7193-7204`) |
| `kClear` | GetRenderStateParameters, GetRenderStateParametersVersion, GetViewport, IsCapabilityEnabled, GetFramebufferBindingSlot, GetClearColor, GetClearDepth, GetClearStencil, GetScissorBox, GetColorMaskIndexed, GetDepthMask, GetStencilState, GetTextureUnitObject, GetTextureContextId, GetSamplingResolutionGeneration, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetImageTextureBinding |
| `kBlitOrCopy` | GetFramebufferBindingSlot, IsCapabilityEnabled, GetScissorBox, IsTransformFeedbackActive, IsTransformFeedbackPaused, GetRenderStateParameters, GetRenderStateParametersVersion, GetViewport, GetActiveTextureUnit, GetTextureUnitObject, GetTextureContextId, GetSamplingResolutionGeneration, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetImageTextureBinding, GetColorMaskIndexed, GetDepthMask, GetStencilState |
| `kTextureOp` | GetActiveTextureUnit, GetTextureUnitObject, GetImageTextureBinding, GetTextureContextId, GetSamplingResolutionGeneration, GetTextureBindGeneration, GetMaxTouchedTextureUnit |
| `kReadback` | GetPixelStoreParameters, GetBufferBindingSlot, GetFramebufferBindingSlot, GetActiveTextureUnit, GetTextureUnitObject, GetClampReadColor, IsCapabilityEnabled, GetRenderStateParameters, GetRenderStateParametersVersion, GetViewport, GetTextureContextId, GetSamplingResolutionGeneration, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetImageTextureBinding |
| `kXfbSpan` | GetTransformFeedbackProgram, GetBufferBindingPoint, GetTouchedBufferBindingPointCount, GetTransformFeedbackCapturedVertices, IsTransformFeedbackActive, IsTransformFeedbackPaused, GetTransformFeedbackGeneration, GetBoundTransformFeedbackLifetimeId |
| `kProgramOp` | GetRenderStateParameters, GetRenderStateParametersVersion, GetViewport, IsCapabilityEnabled, GetFramebufferBindingSlot, GetTextureUnitObject, GetTextureContextId, GetSamplingResolutionGeneration, GetTextureBindGeneration, GetMaxTouchedTextureUnit, GetImageTextureBinding |
| `kQuery` | GetTransformFeedbackPausedPrimitiveCounter (`DirectVulkan.cpp:1237,1284`) — every other verb in the class reads nothing; the fill is a serial bump |

`gen_pipe.py` G5b (`gen_fill_points`) emits `generated/PipeFillPoints.inc`: `enum class MGPipeVerb : Uint8 { DrawArrays, ..., kVerbCount }` (69), `kMGPipeVerbNames[]`, `enum class MGPipeVerbClass : Uint8` (9), `kMGPipeVerbClassNames[]`, `kMGPipeVerbClass[kMGPipeVerbCount]`, `struct MGPipeFieldMask { Uint64 Words[2]; }` + `kMGPipeClassFieldMask[9]` with the seven sticky fields OR'ed in, `MGPipeFieldMaskHas(mask, field)`, and `static_assert(kMGPipeVerbCount == 69)`. It is included from `MGPipe.h` after `PipeFilled.inc` (`MGPipe.h:86`) — the banner's "included from MG_Pipe/MGPipe.h inside namespace MobileGL::MG_Pipe" stays true.

`MobileGL/MG_Impl/Pipe/PipeFill.h`:

```cpp
#pragma once
#if MOBILEGL_PIPE_PUSH
#include <MG_Pipe/MGPipe.h>
namespace MobileGL::MG_Pipe {
    void MGPipeFillForVerb(MGPipeVerb verb);            // PipeFill.cpp
}
#define MGP_FILL(Verb) ::MobileGL::MG_Pipe::MGPipeFillForVerb(::MobileGL::MG_Pipe::MGPipeVerb::Verb)
#else
#define MGP_FILL(Verb) ((void)0)
#endif
```

Placement rule (A applies it at the 83 statements of §B.7): `MGP_FILL(X);` is the statement **immediately before** the call through the table, after every early return the call is behind (the `ConditionalRenderDiscardsCommand()` check in the `*_Backend` wrappers, `GL_Drawing.cpp:529`; the `if (!f) { RecordError; return; }` null checks; the `if (const auto f = table.GL.X) {` guards). A verb whose table entry is null on this backend never bumps. For a call inside a loop (`GL_Query.cpp:915`, `GL_Sync.cpp:229`) the fill is inside the loop body before the call. In the pull build the macro is `((void)0)` (G1).

### D8 — The comparator: what it compares, where it runs, what it prints

Two mechanisms, both compiled only under `MOBILEGL_PIPE_VERIFY` and active only when `Features.PipeVerify` is set:

1. **Entry compare, once per verb** ("每 draw 比一次"): at the end of `MGPipeFillForVerb`, `SnapshotFromGLContext(gPipeSnapshot, mask)` fills a second, file-static `PipeInputs` from the live context (the "verify branch" that survives P13), applies `MOBILEGL_PIPE_VERIFY_CORRUPT` to **the snapshot** (perturb one field: flip a bool, `+1` a scalar, `^0x5A` the first byte of a struct, swap a pointer for `nullptr`), then `MGPipeVerifyInputs(gPipeInputs, gPipeSnapshot, mask, &field)` compares every field in the mask with `MGPipeInputsFieldEqual(field, a, b)` (`PipeInputs.cpp`: V via G4's `MGPipeFieldEqual` — bitwise floats, field-wise structs; O by identity). **Honest statement of what this proves in P1**: both arms come from the same context at the same instant, so it is tautological except under the CORRUPT knob (negative control A) — its oracle role starts in P2 when the tracker fills the first arm. It is nevertheless the mechanism the gate names, and P1 lands it complete.
2. **Compare-at-read, on every accessor**: `MGP_INPUT_VERIFY_READ(f, i0, i1)` calls `MGPipeVerifyReadHook(f, i0, i1)` (`PipeFill.cpp`), which re-reads the same accessor with the same indices from the live context and compares against the stored value. This is the arm that is **real in P1**: it catches any field whose value differs between the verb boundary (what push reads) and the moment of the read (what pull reads) — a frontend counter bumped by the backend mid-verb, a texture re-mint bumping `SamplingResolutionGeneration` between `TrySetupDrawFastPath` and `SetupDraw` (`VulkanRenderer.cpp:6304` vs `:6999`), an error recorded by a helper. Zero divergence here is the semantic statement "push equals pull for every value any backend read in the 40 traces and 808 integration entries".

Reporting: `MGLOG_F("MGPipe: Fatal{PipeVerifyDiffer, \"%s@%s\", verb=%llu, where=%s}", field, verbName, serial, "entry"|"read")` then `std::abort()` unless `PipeVerifyFatal` is off (then a relaxed-atomic counter, summarised at teardown with `MGLOG_E`). Arming, once, at the first fill under verify: `MGLOG_I("MGPipe: verify armed - %u fields, %u verbs, fatal=%d")` (an `MGLOG_I` with a stated reason, per `PipeStats.cpp:224-231`; the lanes grep for `MGPipe: verify armed`). Knob acknowledgements: `MGLOG_I("MGPipe: verify corruption armed - %s")`, `MGLOG_I("MGPipe: poison omission armed - %s@%s")`. Never `MGLOG_D` (compiled out at INFO, `Log.h:59-64`); never stdio (`test.yml:862-869`).

G4 growth (A):
- `PipeFields.def` gains `MGP_FIELDS_RenderStateParameters` (all 67 members of `MGPipeValueTypes.h` `struct RenderStateParameters`, in declaration order, `Viewports` … `ScissorBoxes`), `MGP_FIELDS_PixelStoreParameters` (8), `MGP_FIELDS_PerBufferBlendState` (7), `MGP_FIELDS_StencilFaceState` (7), `MGP_FIELDS_DynamicBackendParameters` (every member of `BackendObject.h:316-…`, `Pad`-named members excluded), `MGP_FIELDS_MGHostSpan` (`Ptr`, `Seg`, `Size`, `Offset`; `Pad0` excluded), all appended to `MGP_VERIFY_PAYLOAD_LIST` → `kMGPipeVerifiedPayloadCount` 63 → **69**. `MEMCMP_FALLBACK_TYPES` (`gen_pipe.py:167-172`) becomes empty and the fallback branch in `gen_verify` becomes `static_assert(sizeof(T) == 0, "no field list in PipeFields.def")` — a future memcmp fallback is a compile error, not a padding false positive.
- Two overloads added to `gen_verify`'s emitted `MGPipeFieldEqual`, declared **before** the generic template: `template <class T, SizeT N> Bool MGPipeFieldEqual(const Array<T, N>&, const Array<T, N>&)` (element-wise; `Array` is `std::array`, `Types.h:57`), and `template <class D, class T, SizeT N> Bool MGPipeFieldEqual(const VecBase<D, T, N>& a, const VecBase<D, T, N>& b)` (element-wise over `data`, so `FloatVec4`/`FloatVec2` compare **bitwise** — `VecBase::operator==` at `VectorTypes.h:41` is IEEE `==`, under which a NaN patch level would differ from itself; the docs' stated policy is bitwise, `ARCHITECTURE.md:159`). Partial ordering picks these over `const T&`.
- Coverage assertion ("the comparator's coverage is itself asserted", `PipeFields.def:15-17`): `gen_pipe.py` gains `check_field_lists_cover_struct_members()`: for every payload in `MGP_VERIFY_PAYLOAD_LIST`, locate `struct <Name> {` in `MGPipeTypes.h`, `MGPipeValueTypes.h`, `MGPipeHostSpan.h`, `MG_Backend/BackendObject.h` (comments/strings masked, `gen_pipe_dirty_surface.py:46-85` shape), collect the direct data members (regex over one-member-per-line bodies; skip `static`, `constexpr`, member functions, nested types; a member named `Pad\d*` is padding), and `sys.exit` on any member absent from the `F(...)` list or any `F(...)` name that is not a member. Runs in both modes (`--check` included) and therefore in `pipe-gates`. Negative control inside `gen_pipe.py --self-test` (new flag): a canned struct/field-list pair that must trip.

### D9 — The 58 non-arrow lines: exact rewrite per category, and why the pull build does not move

| category | count | lines | rewrite | pull-build effect |
|---|---|---|---|---|
| `MOBILEGL_ASSERT(MG_State::pGLContext, "...")` | 34 | `DirectVulkan.cpp` 337,343,349,355,362,369,376,383,389,394,445,451,496,523,567,582,616,622,631,638,644,650,656,741,746,752,758,817,842,865,971,977,1001,1008 | `MOBILEGL_ASSERT(MGB_CTX_LIVE, "...")` | none (macro is empty at INFO, `Defines.h:113-115`) |
| `MOBILEGL_ASSERT(MG_State::pGLContext != nullptr, ...)` | 9 | `UniformManager.cpp` 812,837,992,1152,1266,1651,1806,1888,1968 | `MOBILEGL_ASSERT(MGB_CTX_LIVE, ...)` | none |
| bare guard `if (MG_State::pGLContext) {` | 5 | `Managers.cpp` 3644,3844,8750; `BackendObject_DirectVulkan.cpp` 388,786 | `if (MGB_CTX_LIVE) {` | `UniquePtr::operator bool` is `get() != nullptr`; identical codegen expected |
| compound guard | 2 | `Managers.cpp` 3773 (`&& MG_State::pGLContext &&`), 4735 (`if (MG_State::pGLContext && ...`) | `MGB_CTX_LIVE` in place | as above |
| ternary condition `MG_State::pGLContext != nullptr` | 3 | `Managers.cpp` 7192,7200,7203 (their `?` arms 7193,7201,7204 are arrow lines and take the sed) | `MGB_CTX_LIVE` | as above |
| `!= nullptr` in a condition | 2 | `DirectVulkan.cpp` 1236; `VulkanRenderer.cpp` 12766 | `MGB_CTX_LIVE` | as above |
| `== nullptr` | 1 | `VulkanRenderer.cpp` 11267 | `!MGB_CTX_LIVE` | as above |
| `.get()` raw capture + `decltype` alias | 1 (+ arrow line 143) | `DirectGLES.cpp` 143-155 | `:143` → `decltype(MGB_CTX->GetFramebufferBindingSlot(FramebufferTarget::Draw))`; `:144` → `static const void* g_fbSlotCacheContext = nullptr;`; `:147` → `const void* ctx = MGB_CTX_IDENTITY;`; `:150` → `g_fbSlotCache[i] = &MGB_CTX->GetFramebufferBindingSlot(static_cast<FramebufferTarget>(i));` | a `const void*` compare instead of `GLContext*`: identical codegen expected; if `.text` moves, this is the first suspect |
| comment | 1 | `VertexInputStateFactory.h:133` | "they live on the frontend context's VAOs" | none |
| ternary with arrow on one line | (arrow line) | `DirectVulkan.cpp` 1284 | `MGB_CTX_LIVE ? MGB_CTX->GetTransformFeedbackPausedPrimitiveCounter() : 0` | as above |

**[deviation]** `ARCHITECTURE.md:344` says the ~34 asserts are *deleted*. They are converted instead: identical in every INFO build (G1 is unaffected), preserves DEBUG-build parity, and gate C is satisfied equally (no `pGLContext` token remains). Under push, `MGB_CTX_LIVE` is always true once a context exists, which changes the guarded sites' semantics only in the push arm — that is the "空守卫/三元重写推迟到 P2" the gate names: P1 keeps them textually equivalent, P2 gives them real meaning.

Expected G1 result: `.text` delta **0**, symbols 0/0/0/0. The only code-bearing rewrites are the 13 guard/compare lines and the `.get()` block, and each is a like-for-like pointer test.

### D10 — `SyncPersistentMappedRange` (20) + `SyncGpuWrites` (6): per-site ownership table

Reconcile classes are `ARCHITECTURE.md:233-238` (§5.7): **R0** none; **R1** EBO/max-index scan = `SyncPersistentMappedRange` **+** `SyncGpuWrites`; **R2** `*IndirectCount` count = **only** `SyncPersistentMappedRange` (open question #15, `ROADMAP.md:88` — recorded, not fixed); **R3** server-side restart rewrite / multi-draw flatten = server reads its mirror, GPU-writer visibility by `OnGpuWritten`; **R4** UBO host payload, the immovable consumer (D-B8, `ARCHITECTURE.md:294`). In P1 every site stays exactly where it is (the `sed` touches none of these lines); the table records the class each site must reproduce when P8 moves the reconcile to the client.

| # | site (verified) | receiver | verb | today | class |
|---|---|---|---|---|---|
| P1 | `DirectGLES.cpp:263` | drawBuffer | draw indirect (CPU fallback `ResolveIndirectCommandBytes`) | SPMR only | R2 |
| P2 | `DirectGLES.cpp:4468` (+ G1 `:4469`) | indexBuffer | draw, restart substitution | SPMR + SGW | R1 |
| P3/P4 | `DirectGLES.cpp:4722,4723` | drawBuffer, parameterBuffer | `MultiDrawElementsIndirectCount` | SPMR only | R2 |
| P5/P6 | `DirectGLES.cpp:4824,4825` | drawBuffer, parameterBuffer | `MultiDrawArraysIndirectCount` | SPMR only | R2 |
| P7 | `Managers.cpp:1569` | bufferObject | buffer op (`EnsureBufferResource`) | SPMR | R0 (client array/staging: application memory, no GPU writer) |
| P8 | `MultiDraw.cpp:510` (+ G3 `:511`) | indexBuffer | multi-draw flatten `RunRebasedDrawElements` | SPMR + SGW | R3 |
| P9 | `DirectVulkan.cpp:280` | drawBuffer | draw indirect (CPU fallback) | SPMR only | R2 |
| P10 | `DirectVulkan.cpp:471` | parameterBuffer | `MultiDrawArraysIndirectCount` | SPMR only | R2 |
| P11 | `DirectVulkan.cpp:795` | indexBufferShared | `BuildClosedLineLoopIndices` (index read) | SPMR only | R1 (client pointer source → R0 when the indices are client memory) |
| P12 | `UniformManager.cpp:2023` | bufferObject | `ResolveUniformBufferPayload` | SPMR | **R4** — immovable |
| P13 | `VkBufferManager.cpp:628` | bufferObject | `AcquireResidentSlice` | SPMR | R0 |
| P14 | `VkBufferManager.cpp:679` | bufferObject | `AcquireStreamedSlice` | SPMR | R0 |
| P15 | `VulkanRenderer.cpp:3434` (+ G4 `:3433`) | indexBufferShared | `TryComputeMaxIndexFromHostBytes` | SPMR + SGW | R1 |
| P16 | `VulkanRenderer.cpp:3513` | bufferObject | `TryBindResolvedVertexBindings` | SPMR | R0 |
| P17 | `VulkanRenderer.cpp:3828` (+ G5 `:3827`) | sourceBufferShared | fp64 narrowing in `UploadAndBindVertexBuffers` | SPMR + SGW | R1 (a GPU-written vertex source) |
| P18 | `VulkanRenderer.cpp:7137` | indirectBuffer | `DispatchComputeIndirect` | SPMR only | R2 |
| P19/P20 | `VulkanRenderer.cpp:12132,12133` | drawBuffer, parameterBuffer | `MultiDrawElementsIndirectCount` | SPMR only | R2 |
| G2 | `Managers.cpp:2643` | bufferObject | `SyncFloat64AttributeAsFloat32` | SGW | R1 |
| G6 | `VulkanRenderer.cpp:4161` | indexBufferShared | `UploadAndBindIndexBuffer`, restart branch | SGW | R3 |

Split: SPMR 8 Espryt / 12 Magma; SGW 3 / 3. Comment-only mentions (not sites): `Managers.cpp:1461,2681`, `VulkanRenderer.h:1214`, `DirectGLES.cpp:459,509`. The integrator lands this table in `docs/Disaggregated/MEASUREMENTS.md` as a new §6.

### D11 — Deferred out of P1 (recorded, with the doc lines to correct)

| item | why not P1 | doc to fix |
|---|---|---|
| `gen_pipe_dirty_surface.py` mapping file + `--check` gate | `ROADMAP.md:18` puts "首轮映射成门" in **P2**; the table cells are the authority (scout R2) | `ARCHITECTURE.md:172,568`, `test.yml:871-874` comment, `scripts/gen_pipe_dirty_surface.py:21-22` say P1 → P2 (integrator) |
| `GetBufferBindingSlot` rows split by target in `Coverage.def` (`:36-40`) | needs the re-vendored inventory carrying the target argument; the extractor lives in MobileGL-CS, not here | note in `Coverage.def:36-40` ("when the inventory is re-vendored") |
| re-vendoring `scripts/data/backend_read_inventory.md` | same; D13's live cross-check is the stop-gap gate | `MEASUREMENTS.md:82` |
| `DynamicBackendParameters` fixed-width members (`MGPipeTypes.h:32`, scout R6) | not in the P1 row; the field list (D8) covers verify without it | — |
| semantic rewrite of the 13 guard/ternary sites | gate text: "推迟到 P2" | — |
| `MGPipeImpl_*.cpp` | D5 | `ARCHITECTURE.md:553` |
| `PipeStats` `AccessorCalls` tallies (`PipeStats.cpp:16-100`) | unchanged: the sites still call the same accessors through `MGB_CTX`; `RecordDrawPayloadBytes` stays unwired until a generator emits records | — |

### D12 — Coverage.def and the live accessor cross-check (A)

`Coverage.def` gains two rows after `:97`: `X(GetBoundTransformFeedbackLifetimeId, SetStreamOutputTargets)` and `X(HasOpenTransformFeedbackSpan, SetStreamOutputTargets)` (the D21 XFB counter-slot rekey's reads, `VulkanRenderer.cpp:11219,11236`; the calls they map to are the same as `GetTransformFeedbackGeneration`'s); `GetBoundTransformFeedbackName` keeps its row with a `/* dead: no backend reads it since D21; kept for inventory row 594 */` comment. `gen_pipe.py` gains `scan_live_accessors()`: walk `MobileGL/MG_Backend/**.{cpp,h}` (comments masked), collect every `\b(?:MGB_CTX|pGLContext)\s*->\s*(\w+)` name, `sys.exit` if a name has no `Coverage.def` row, and print (informational) rows no backend reads. Runs in both modes, so `pipe-gates` catches the next drift the day it lands. Zero-row accessors are already fine for `gen_coverage` (`gen_pipe.py:513-579` only iterates inventory rows).

### D13 — Namespaces, naming, generated-file discipline

Pipe code: `namespace MobileGL::MG_Pipe` (`MGPipe.h:26`); `PipeInputs`, `gPipeInputs`, `MGPipeVerb`, `MGPipeVerbClass`, `MGPipeFieldMask`, `kMGPipe*` constants, `MGPipeFillForVerb`, `SnapshotFromGLContext`, `MGPipeVerifyInputs`, `MGPipeVerifyReadHook`, `MGPipeInputsFieldEqual`. Generated files: `MobileGL/MG_Pipe/generated/*.inc`, LF, written through `write()` inside `main()` (`gen_pipe.py:619-652`) so `--check` and the CI `git diff -- MobileGL/MG_Pipe/generated` see them. Banner: `MGPipe.h:1-7`. Commits: `[Type] (Scope): description`, single line, no `Co-Authored-By`. Logging per D8. Test names: additions only.

---

## C. Packages

Four packages on four branches, four WSL worktrees. **Branch points:** `p1/core` from `feat/disaggregated@087685d1`. `p1/core`'s **first commit is the contract** (`c1`, tagged `p1/contract`): the switch header, `PipeInputs.h` with every accessor, `PipeFill.h`, `FillPoints.def` + regenerated `PipeFillPoints.inc`, the CMake options, `PipeInputs.cpp`/`PipeFill.cpp` with the forwarders and a `MGPipeFillForVerb` that only bumps (no field copies yet), `Config.h`/`ConfigLoader.cpp` knobs. `p1/espryt`, `p1/magma`, `p1/verify` branch from `p1/contract`, so their push builds compile and link while A finishes. `c1` is byte-neutral for the pull build (headers nobody includes, sources behind an OFF option).

WSL tree creation (copy `scratchpad/wf2/wsl_p05_trees.sh` → `wsl_p1_trees.sh`; `p05`→`p1`, slugs `core espryt magma verify`; the three non-core trees are created after `p1/contract` exists with `git worktree add -q ~/w7/p1-$slug -b p1/$slug p1/contract`). Each tree configures three build directories:

```
# pull (the G1 build; flags identical to ~/w7/pipe/build-linux)
cmake -S . -B build-linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_BUILD_TYPE=Release -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO -DMOBILEGL_BUILD_TEST=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON -DMOBILEGL_ITEST_EGL_VENDOR=/usr/share/glvnd/egl_vendor.d/50_mesa.json -DMOBILEGL_ITEST_VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
# push
cmake -S . -B build-push   ... same ... -DMOBILEGL_PIPE_PUSH=ON
# verify (A and E, and the integrator)
cmake -S . -B build-verify ... same ... -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON
```

Before anything else the integrator captures the baseline: `cp ~/w7/pipe/build-linux/libMobileGL.so ~/w7/p1-before-libMobileGL.so` (confirm `git -C ~/w7/pipe log -1` is `087685d1` and the cache flags above), `ctest --test-dir ~/w7/pipe/build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort > ~/w7/p1-before-ctest-names.txt`, `grep -rc "pGLContext" MobileGL/MG_Backend | sort > ~/w7/p1-before-pglcontext.txt` (must equal the D1 per-file counts).

### C.1 Package A — `p1-core` (tree `~/w7/p1-core`, branch `p1/core`)

**Files**

| action | file | what |
|---|---|---|
| CREATE | `MobileGL/MG_Pipe/PipeInputsSwitch.h` | D3 verbatim |
| CREATE | `MobileGL/MG_Pipe/FillPoints.def` | D7: three macros, 69 verb rows in `GLFunctionsTable` declaration order, 9 classes, the field rows; blank line after each macro |
| CREATE | `MobileGL/MG_Backend/MGPipe/PipeInputs.h`, `PipeInputs.cpp` | D4, D6, D8 |
| CREATE | `MobileGL/MG_Impl/Pipe/PipeFill.h`, `PipeFill.cpp` | D7, D8; `PipeFill.cpp` includes `<MG_State/GLState/Core.h>`, `<MG_Backend/MGPipe/PipeInputs.h>`, `<Config.h>` |
| MODIFY | `MobileGL/MG_Pipe/Coverage.def` | D12 rows; `MGP_COVERAGE_STICKY_LIST(X)` with the seven D6 rows and reasons; note at `:36-40` |
| MODIFY | `MobileGL/MG_Pipe/PipeFields.def` | D8: six new field lists + payload-list entries |
| MODIFY | `scripts/gen_pipe.py` | parse `FillPoints.def` (`parse_fill_points()`, blank-line-terminated blocks, `sys.exit` on: a verb not in `GLFunctionsTable`, a table member without a row, a verb in two classes, a class with no verbs, an unknown field, a duplicate (class, field)); parse `MGP_COVERAGE_STICKY_LIST`; `gen_filled` emits real sticky flags; `gen_fill_points` → `PipeFillPoints.inc` written via `write()` in `main()` between the `PipeFilled.inc` and `PipeCoverage.inc` lines (`:650-651`); `gen_verify` overloads + empty `MEMCMP_FALLBACK_TYPES` + `static_assert` fallback; `check_field_lists_cover_struct_members()`; `scan_live_accessors()`; `--self-test` (canned negative controls for the struct-member check and the verb-set check; `trips == 0 → error`, `check_include_closure.py:541-543` shape); summary line gains "N verbs, M classes" |
| MODIFY | `MobileGL/MG_Pipe/generated/*.inc` | regenerate and commit (7 files change: `PipeFilled.inc` count 63 + sticky, `PipeVerify.inc`, new `PipeFillPoints.inc`, the banner of any file whose sources line changes) |
| MODIFY | `MobileGL/MG_Pipe/MGPipe.h` | `#include "generated/PipeFillPoints.inc"` after `:86` with a G5b comment |
| MODIFY | `CMakeLists.txt` | after `:24`: `option(MOBILEGL_PIPE_PUSH "Backends read frontend state through the MGPipe PipeInputs block instead of MG_State::pGLContext (ARCHITECTURE.md 9.2 phase A)" OFF)`, `option(MOBILEGL_PIPE_VERIFY "Compile SnapshotFromGLContext() and the G4 per-verb shadow comparator; implies MOBILEGL_PIPE_PUSH; never shipped" OFF)`; after `:451`: `if (MOBILEGL_PIPE_VERIFY AND NOT MOBILEGL_PIPE_PUSH) message(STATUS ...) set(MOBILEGL_PIPE_PUSH ON) endif()`; `if (MOBILEGL_PIPE_PUSH) list(APPEND SOURCE_FILES MobileGL/MG_Backend/MGPipe/PipeInputs.cpp MobileGL/MG_Impl/Pipe/PipeFill.cpp) endif()`; after `:525`: the two `list(APPEND MOBILEGL_COMPILE_DEF -DMOBILEGL_PIPE_PUSH=1 / -DMOBILEGL_PIPE_VERIFY=1)` |
| MODIFY | `MobileGL/Config.h:332` region, `MobileGL/ConfigLoader.cpp:246` region | D2's three knobs under `#if MOBILEGL_PIPE_PUSH` |
| MODIFY | `MobileGL/MG_Impl/GLImpl/{Drawing/GL_Drawing.cpp, Framebuffer/GL_Framebuffer.cpp, Texture/GL_Texture.cpp, Getter/GL_Getter.cpp, Program/GL_Program.cpp, Query/GL_Query.cpp, Sync/GL_Sync.cpp}` | `#include <MG_Impl/Pipe/PipeFill.h>` after each TU's last `MG_State`/`MG_Backend` include; the 83 `MGP_FILL(X);` statements of §B.7 |
| MODIFY | `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp` | `:225` 61 → 63; new tests below; if the verify-only tests need it, `#if MOBILEGL_PIPE_VERIFY … #else GTEST_SKIP() << "verify not compiled in" #endif` (a visible skip, not a vanishing test) |
| MODIFY | `MobileGL/MG_Test/Pipe/CMakeLists.txt` | only if a `PipeInputs`-linking test target is added: `PipeInputsTest` linking `${LINK_LIBRARIES}` (the static library), registered `LABELS unit` |

**Ordered steps**

1. `c1` (contract): switch header; `FillPoints.def` + generator growth + regenerate; `PipeInputs.h` complete (all 63 accessors, storage, macros expanding to nothing until step 3); `PipeInputs.cpp` (equality, corruption injector); `PipeFill.h`; `PipeFill.cpp` with `IsLive`, `ContextIdentity`, the seven forwarders, and `MGPipeFillForVerb` = serial bump + `m_currentVerb` only; CMake options and conditional sources; `Config.h`/`ConfigLoader.cpp` knobs; `Coverage.def` + `PipeFields.def` edits; `PipeCatalogueTest.cpp` count. Build pull (identical), push, verify; commit; **tag `p1/contract`** and tell the other packages. Message: `[Feat] (Pipe): land the P1 contract - PipeInputsSwitch.h with MGB_CTX, the 63-field PipeInputs block with type-identical accessors, FillPoints.def and its G5b generator, the MOBILEGL_PIPE_PUSH/VERIFY options and the three verify knobs; pull build unchanged`.
2. `c2`: the filler proper (per-class copies, stamps, omission knob), poison checks in the accessors (`MGP_INPUT_CHECK`), sticky stamping on first live fill. Message: `[Feat] (Pipe): fill PipeInputs per verb class from GLContext and stamp per-verb generations - a read of a field the verb did not fill is Fatal{UnmigratedPipeInput}`.
3. `c3`: the 83 fill points in `MG_Impl`. Message: `[Feat] (Impl): call MGP_FILL before every GLFunctionsTable entry - 83 statements over 69 verbs, a no-op in the pull build`.
4. `c4`: `SnapshotFromGLContext`, entry compare, compare-at-read, arming line, `CORRUPT`/`FATAL` knobs. Message: `[Feat] (Pipe): the MOBILEGL_PIPE_VERIFY shadow comparator - a per-verb entry compare over the fill set and a compare-at-read in every accessor, first differing field and verb serial, fatal by default`.
5. `c5`: G4 field lists for the six value types, the two overloads, the struct-member coverage check, `scan_live_accessors`, `--self-test`; regenerate. Message: `[Feat] (Pipe): give the six value structs G4 field lists and assert the lists cover their members - the memcmp fallback is now a compile error, floats in vector types compare bitwise`.
6. `c6`: unit tests. Message: `[Test] (Pipe): pin the P1 poison and verify shapes - 63 fields, 69 verbs, the seven sticky fields, an omitted GenerateMipmap field leaves exactly that field stale, a corrupted snapshot names its field, reading an unfilled field aborts with SIGABRT`.

**Tests to add** (`PipeCatalogueTest.cpp`; the fork/waitpid one follows `HeadlessGL.cpp:343-388` / `FdPassingTest.cpp:62-132`, never `EXPECT_DEATH` — the tree has none):
- `PipeInputFieldsStartUnfilled`: 63.
- `VerbTableIsTheFunctionTable`: `kMGPipeVerbCount == 69`; every class non-empty; the seven sticky bits are set in every class mask.
- `StickyFieldsAreExactlyTheSeven`: iterate `kMGPipeInputFieldSticky[]`.
- `OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale` (scout B4 layer 1, verbatim shape): fill twice with a fake context (`MG_State::pGLContext = MakeUnique<GLContext>()`, the `SanityTest.cpp:907` idiom), once clean, once with `SetPoisonOmission("GenerateMipmap", "GetActiveTextureUnit")`; assert `GetTextureUnitObject` fresh and `GetActiveTextureUnit` stale, and that a `DrawArrays` fill afterwards leaves `GetActiveTextureUnit` stale (not in `kDraw`'s mask) while `GetBoundVertexArray` is fresh.
- `ReadingAnOmittedFieldAbortsNamingTheVerb` (`#if MOBILEGL_PIPE_POISON` else `GTEST_SKIP`): fork; child fills `GenerateMipmap` with the omission then calls `gPipeInputs.GetActiveTextureUnit()`; parent expects `WIFSIGNALED && WTERMSIG == SIGABRT`; sibling test without the omission expects `_exit(0)`. Log assertion of the exact `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}` string via `MOBILEGL_LOG_FILE_PATH` set in the test's own `main()` (`LogLevelTest` shape, `MG_Test/Util/CMakeLists.txt:19-22`) — if that needs a separate target, `PipeInputsTest` links `GTest::gtest`.
- `CorruptedSnapshotFieldIsNamedWithItsSerial` (`#if MOBILEGL_PIPE_VERIFY`): clean arm true; `ApplyVerifyCorruption(snapshot, "GetRenderStateParameters")` → false, field string equal, serial equal to the fill serial.
- `FloatVectorsCompareBitwise`: `MGPipeFieldEqual(FloatVec4{NaN,...}, same bits)` true; `-0.0f` vs `0.0f` false; `Array<PerBufferBlendState, 8>` differing in element 3 → `MGPipeVerify(ResidualValueBlock…)` names `RenderState`.
- `SixValueStructsHaveFieldLists`: `kMGPipeVerifiedPayloadCount == 69u`.

**Constraints**: `PipeInputs.h` and `PipeInputs.cpp` never spell `pGLContext` (`grep -c pGLContext MobileGL/MG_Backend/MGPipe/*` == 0). `MG_Backend/DirectGLES/**` and `MG_Backend/DirectVulkan/**` are **not touched** (B and C own them). `PipeCalls.def` untouched. No renames of `PipeStats` counter names (`PipeStatsTest.cpp:260-279`). No `MGLOG_D` in verify reporting.

**Verification (WSL, `~/w7/p1-core`)**
```
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0   # 0/0/0/0, .text +0
grep -rc "pGLContext" MobileGL/MG_Backend | sort | diff - ~/w7/p1-before-pglcontext.txt    # empty (A converts no site)
grep -c "pGLContext" MobileGL/MG_Backend/MGPipe/PipeInputs.h MobileGL/MG_Backend/MGPipe/PipeInputs.cpp   # 0 0
grep -c "MGP_FILL(" MobileGL/MG_Impl/GLImpl/*/*.cpp | awk -F: '{s+=$2} END {print s}'   # 83
cmake --build build-push --parallel 28 && ctest --test-dir build-push -L unit --no-tests=error
cmake --build build-verify --parallel 28 && ctest --test-dir build-verify -L unit --no-tests=error
# the poison is real: with the sites still on pGLContext (B/C not merged) nothing reads gPipeInputs, so run the
# integration suite in build-push to prove the fill points and filler are inert-correct (all green, no aborts):
ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4
ctest --test-dir build-linux -N | grep -E '^\s+Test #' | sed 's/^ *Test *#[0-9]*: //' | sort | comm -23 ~/w7/p1-before-ctest-names.txt -   # empty
```

### C.2 Package B — `p1-espryt` (tree `~/w7/p1-espryt`, branch `p1/espryt`, from `p1/contract`)

**Files**: `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`, `Managers.cpp`, `MultiDraw.cpp`, `Utils.cpp` (nothing else; `Managers.h`, `Utils.h`, `DirectGLES.h`, `BackendObject_DirectGLES.cpp` have no sites).

**Site list it owns** — 122 lines = 113 arrow lines (113 occurrences) + 9 non-arrow:

| file | arrow lines (sed `MG_State::pGLContext->` → `MGB_CTX->`) | non-arrow (D9) |
|---|---|---|
| `DirectGLES.cpp` (91 + 1) | 143,261,357,370,429,462,465,474,481,518,662,892,987,1007,1244,1275,1373,1386,1414,1415,1515,1517,1518,1580,1728,1820,2026,2054,2083,2166,2842,2844,2846,2957,2968,3048,3212,3362,3482,3580,3662,3712,3821,3822,3826,4062,4088,4093,4156,4215,4380,4396,4397,4402,4442,4570,4609,4680,4711,4712,4782,4813,4814,4973,5014,5259,5260,6044,6045,6107,6108,6185,6375,6699,6700,6794,6795,6932,6933,7357,7358,7554,7562,8544,8774,9041,9084,9194,9197,9420,9510 | 147 (`.get()`, with the 143-155 block rewrite of D9) |
| `Managers.cpp` (15 + 8) | 3645,3646,3774,3775,3845,3846,4736,4737,5355,5460,6230,7193,7201,7204,8751 | 3644,3773,3844,4735 (guards), 7192,7200,7203 (ternary conditions), 8750 (guard) |
| `MultiDraw.cpp` (5) | 45,51,52,87,95 | — |
| `Utils.cpp` (2) | 2297,2301 | — |

**Ordered steps**
1. Add `#include <MG_Pipe/PipeInputsSwitch.h>` (D3 placement) to the four TUs.
2. Mechanical sed over the four files: `sed -i 's/MG_State::pGLContext->/MGB_CTX->/g'` — then confirm the per-file occurrence counts above (`grep -c 'MGB_CTX->'`), and that the only remaining `pGLContext` tokens are the 9 non-arrow lines.
3. Convert the 9 non-arrow lines per D9 (the `DirectGLES.cpp:143-155` block by hand).
4. Build pull; symbol report; build push; run.
5. One commit: `[Refactor] (Espryt): route every frontend read through MGB_CTX - 113 arrow sites sed'd, 9 non-arrow lines converted (the fb-slot cache keys on MGB_CTX_IDENTITY); pull build byte-identical`.

**Constraints**: no reordering, no reformatting beyond the touched lines; the `PipeStats` `AddCalls` literals near the sites (`DirectGLES.cpp:1416-2969`, `Managers.cpp:783-4433`, `MultiDraw.cpp:176-754`) are unchanged; the 8 + 3 sync sites of D10 are untouched; no new `pGLContext` spelling anywhere.

**Verification (WSL, `~/w7/p1-espryt`)**
```
grep -rc "pGLContext" MobileGL/MG_Backend/DirectGLES | grep -v ":0$"       # empty
grep -c "MGB_CTX" MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp MobileGL/MG_Backend/DirectGLES/Managers.cpp MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp MobileGL/MG_Backend/DirectGLES/Utils.cpp   # ≥ 92+23+5+2 (occurrences per file: DirectGLES 91 arrow + 4 in the cache block + 1 alias; Managers 15+8; MultiDraw 5; Utils 2)
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error
python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0   # 0/0/0/0, .text +0 expected; any delta attributed to D9 lines in the commit message
ctest --test-dir build-linux -L integration-gpu -R '^DirectGLES\.' --no-tests=error -j 4                                 # by-name green
cmake --build build-push --parallel 28 && ctest --test-dir build-push -L integration-gpu -R '^DirectGLES\.' --no-tests=error -j 4   # links; runs (on the contract tree the filler does not copy yet, so under push this run is expected to hit Fatal{UnmigratedPipeInput} at the first read - that is the poison working; rerun after rebasing onto p1/core c2+ and it must be green)
```

### C.3 Package C — `p1-magma` (tree `~/w7/p1-magma`, branch `p1/magma`, from `p1/contract`)

**Files**: `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp`, `BackendObject_DirectVulkan.cpp`, `Renderer/UniformManager.cpp`, `Renderer/VulkanRenderer.cpp`, `Renderer/VkClearManager.cpp`, `Renderer/VkRenderPassManager.cpp`, `Renderer/VkTextureManager.cpp`, `Renderer/VertexInputStateFactory.h` (comment only).

**Site list it owns** — 213 lines = 164 arrow occurrences on 161 lines + 49 non-arrow:

| file | arrow lines | non-arrow (D9) |
|---|---|---|
| `DirectVulkan.cpp` (12 + 35) | 270,273,278,402,465,533,586,716,786,910,1237,1284 (1284 also gets the `MGB_CTX_LIVE ?` rewrite) | 337,343,349,355,362,369,376,383,389,394,445,451,496,523,567,582,616,622,631,638,644,650,656,741,746,752,758,817,842,865,971,977,1001,1008 (asserts), 1236 (`!= nullptr`) |
| `BackendObject_DirectVulkan.cpp` (2 + 2) | 389,787 | 388,786 (guards) |
| `UniformManager.cpp` (14 + 9) | 506,555,782,821,847,1016,1189,1194,1295,1662,1850,1938,2013,2018 | 812,837,992,1152,1266,1651,1806,1888,1968 (asserts) |
| `VkClearManager.cpp` (1) | 57 | — |
| `VkRenderPassManager.cpp` (3) | 613,965,1111 | — |
| `VkTextureManager.cpp` (2) | 809,952 | — |
| `VulkanRenderer.cpp` (127 lines / 130 occurrences + 2) | 463,468,523,556,557,570,649,650,1243,1247,1303,2771,2988,3448,3449,3926,4060,4815,4816,4817,4828,4940,4984,5172,5173,5175,5178,5180,5181,5187,5199,5200,5202,5287,5288,5294,5298,5312,5317,5318,5354,5355,5407,5435,5436,5437,5442,5858,5919,5943,6019,6043,6091,6105,6110,6121,6122,6130,6304,6434,6464,6471,6472,6512,6526,6532,6650,6669,6830,6831,6972,6973,6999,7036,7088,7132,7206,7209,7257,7260,7268,7269,7270,7277,7300,7327,7340,7369,7373,7389,7410,7439,7481,7512,7514,7526,7583,7601,7609,7628,8586,8587,8619,8620,9214(×2),9222,9933,10688,10689,10719(×2),10963(×2),11219,11236,11268,11274,11277,11316,11347,11370,12018,12096,12106,12112,12197,12207,12277,12767 | 11267 (`== nullptr`), 12766 (`!= nullptr`) |
| `VertexInputStateFactory.h` (0 + 1) | — | 133 (comment) |

Steps, constraints and verification exactly as B, with `DirectVulkan` in place of `DirectGLES`, the seven TUs getting the switch include (D3 placement), and one commit: `[Refactor] (Magma): route every frontend read through MGB_CTX - 164 arrow sites sed'd, 49 non-arrow lines converted (43 asserts keep their meaning as MGB_CTX_LIVE); pull build byte-identical`. The 12 + 3 sync sites of D10 are untouched. `grep -rc "pGLContext" MobileGL/MG_Backend/DirectVulkan | grep -v ":0$"` is empty.

### C.4 Package E — `p1-verify` (tree `~/w7/p1-verify`, branch `p1/verify`, from `p1/contract`)

The third CI mode and the negative controls. Everything E codes against is a contract fixed above: option names (D2), env knobs (D2), log lines (D8), the `MGPipeVerb`/field vocabularies (D7/D4), `MGLOG_F` + `std::abort()` = SIGABRT.

**Files**

| action | file | what |
|---|---|---|
| MODIFY | `MobileGL/MG_IntegrationTest/CMakeLists.txt` | after `:393`: `set(MGL_ITEST_VERIFY_TIMEOUT 900)`; `if (MOBILEGL_PIPE_VERIFY)` block with two ambient registrations `DirectGLES.Verify.` / `DirectVulkan.Verify.` (env `MOBILEGL_PIPE_VERIFY=1` + `MOBILEGL_LOG_FILE_PATH=${CMAKE_CURRENT_BINARY_DIR}/pipe-verify-<backend>.log` + **`${MGL_ITEST_COMMON_ENV}` / `${MGL_ITEST_VULKAN_ENV}` appended** (`:339-344` rule), `LABELS integration-gpu integration-verify`, `TIMEOUT ${MGL_ITEST_VERIFY_TIMEOUT}`, joined with `mgl_itest_join_environment` (`:307-317`)); two corrupted-field lanes `DirectGLES.VerifyCorrupted.` / `DirectVulkan.VerifyCorrupted.` (`TEST_FILTER "PipeVerifyArmingScenario.CorruptedFieldIsReported"`, env adds `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters MOBILEGL_PIPE_VERIFY_FATAL=0`, own log path); two omission lanes `DirectGLES.PoisonOmitted.` / `DirectVulkan.PoisonOmitted.` (`TEST_FILTER "PoisonOmissionScenario.*"`, env adds `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit`, own log path). Both scenario files added to the `add_executable` list (`:51-130`) |
| CREATE | `MobileGL/MG_IntegrationTest/Scenarios/PipeVerifyArmingScenario.cpp` | `Armed`: skips unless `AmbientQuirkFromEnvironment("MOBILEGL_PIPE_VERIFY") == On` (`ScenarioFixture.h:56-65`) and the log path is set (`UnlocatedIoBlockScenario.cpp:354-360` idiom, only bytes written after the case started); draws once; asserts `MGPipe: verify armed` present and no `Fatal{PipeVerifyDiffer`. `CorruptedFieldIsReported`: skips unless `MOBILEGL_PIPE_VERIFY_CORRUPT` is set; draws once; asserts `Fatal{PipeVerifyDiffer, "GetRenderStateParameters@` present |
| CREATE | `MobileGL/MG_IntegrationTest/Scenarios/PoisonOmissionScenario.cpp` | `OmittedFieldAbortsOnThatVerb`: skips unless `MOBILEGL_PIPE_POISON_OMIT` is set; `fork()` **before** any scenario-local GPU work (`HeadlessGL.cpp:343-388`, `_exit` never `exit`, `:365-368`); child: create a 2-level texture, `glDrawArrays` once (must not abort), `glGenerateMipmap` (must abort), `_exit(0)`; parent: `WIFSIGNALED && WTERMSIG == SIGABRT`, log carries `Fatal{UnmigratedPipeInput, "GetActiveTextureUnit@GenerateMipmap"}` and no `@DrawArrays`. `WithoutOmissionCompletes`: same child without the knob → `WIFEXITED && WEXITSTATUS == 0` (registered in the same lane by filter; the env var is present, so this case clears it? No — env is process-wide: the clean arm runs in the ambient `Verify.` lanes instead, where the scenario asserts the knob is unset) |
| MODIFY | `tools/trace_replay/run_trace_case.cmake` | after `:129`: `if(DEFINED ENV{MOBILEGL_PIPE_VERIFY} AND NOT "$ENV{MOBILEGL_PIPE_VERIFY}" STREQUAL "0")` → `FATAL_ERROR` if `mobilegl.log` missing, if it lacks `MGPipe: verify armed`, or if it contains `Fatal\{PipeVerifyDiffer` / `Fatal\{UnmigratedPipeInput` (scout B2.2 text) |
| MODIFY | `tools/trace_replay/CMakeLists.txt:373-379` | add `LABELS retrace` to both `set_tests_properties` (the lane is otherwise selectable only by regex) |
| MODIFY | `tools/trace_replay/trace_cases.json`, `trace_cases.py` | `"verify": true` on eight cases: the five 180 s cases plus `minecraft-1.21.4-in-world`, `minecraft-1.21.4-fabric-sodium-in-world`, `improved-transparency-minecraft-26.3` (exact names from the file; E lists them in the commit); `--format github-verify-matrix` = `github_test_matrix(cases with verify)`; unit-check in `trace_cases.py`'s existing self-checks |
| MODIFY | `scripts/symbol_report.py` | implement `--fail-on-added-bytes N` (`:287-288` is reserved) and add `--fail-on-symbol-set-change`; default behaviour unchanged; `--self-test` covers both |
| MODIFY | `.github/workflows/test.yml` | (a) `build-linux-verify` (copy of `build-linux:12-147` with `BUILD_DIR=build-verify`, its own ccache key `${{ runner.os }}-test-${{ github.job }}-ccache-v1`, configure adds `-DMOBILEGL_PIPE_VERIFY=ON` and keeps `MOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO`, artifact `mobilegl-linux-runtime-verify`, `timeout-minutes: 120`); (b) `integration-verify` (`needs: build-linux-verify`; the `integration:205-296` shape incl. the path-normalisation step over `build-verify`, `ctest --output-on-failure -L integration-verify --no-tests=error`, core-dump upload, `timeout-minutes: 180`) plus the two negative-control steps: `MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters ctest -L integration-verify -R 'DirectGLES\.Verify\..*ClearThenReadPixels' --no-tests=error` must **fail** (`if ctest ...; then echo "::error::..."; exit 1; fi`), and `MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit ctest -L integration-verify -R 'DirectGLES\.Verify\..*Mipmap' --no-tests=error` must fail (job env reaches the process: no `ENVIRONMENT` property names these, `test.yml:274-288` precedent); (c) `retrace-verify` (`needs: [build-linux-verify, build-retrace, trace-cases, trace-fixtures]`, matrix from a new `trace-cases` output `verify-matrix` = `--format github-verify-matrix`, `max-parallel: 4`, `timeout-minutes: 240`; unpack the **verify** runtime so the verify `.so` lands at `build-linux/libMobileGL.so` (the path frozen into `CTestTestfile.cmake`), `test -f` + `nm -D build-linux/libMobileGL.so | grep -q MGPipeVerifyInputs`, `export MOBILEGL_PIPE_VERIFY=1` next to the per-case exports (`:700-723`), `ctest -V --no-tests=error --timeout 10800 -R '^MobileGLTraceReplay\.<case>\.<backend>$'`, artifacts suffixed `-verify`); one inverted `OpenRA` step with `MOBILEGL_PIPE_VERIFY_CORRUPT`; (d) `remove-artifact-clutter:787` `needs: [retrace-summary, retrace-verify]`; (e) `monolith-symbol-report` (`workflow_dispatch` input `baseline_sha`, default `087685d1`): two pull builds with identical flags (`-DMOBILEGL_BUILD_TEST=OFF -DMOBILEGL_BUILD_BENCHMARK=OFF -DMOBILEGL_BUILD_INTEGRATION_TEST=OFF -DMOBILEGL_BUILD_TRACE_REPLAY=OFF -DMOBILEGL_BUILD_DISAGGREGATED=OFF -DMOBILEGL_PIPE_PUSH=OFF -DMOBILEGL_PIPE_VERIFY=OFF -DMOBILEGL_ENABLE_LTO=OFF`, clang-20), `symbol_report.py --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0 --markdown --json`, upload the markdown, and `nm --defined-only build-sym-head/libMobileGL.so | grep -q MG_Remote && exit 1` (`ARCHITECTURE.md:506`); (f) the `pipe-gates` dirty-surface comment `:871-872` → "in P2"; (g) `pipe-gates` gains `python3 scripts/gen_pipe.py --self-test` after `:851`. Triggers (`:3-9`) unchanged (P0.5 D10). |

**Constraints**: every new `ENVIRONMENT` list appends `${MGL_ITEST_COMMON_ENV}`/`${MGL_ITEST_VULKAN_ENV}`; the 11 filtered mode lanes are not doubled; no `ENVIRONMENT` property ever names `MOBILEGL_PIPE_VERIFY_CORRUPT`/`MOBILEGL_PIPE_POISON_OMIT` on the ambient `Verify.` lanes (the CI negative-control steps rely on job env reaching them); the verify lanes are registered only under `if (MOBILEGL_PIPE_VERIFY)` so `--no-tests=error` makes a mis-configured lane red; `MG_Test/**`, `MobileGL/**` outside `MG_IntegrationTest/`, `CMakeLists.txt` (root) untouched.

**Verification (WSL, `~/w7/p1-verify`)**
```
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/test.yml'))"
python3 tools/trace_replay/trace_cases.py --ci --format github-verify-matrix | python3 -c "import json,sys; m=json.load(sys.stdin); print(len(m['include']))"   # 16 (8 cases x 2, minus any ci_backends restriction)
python3 scripts/symbol_report.py --self-test
cmake -S . -B build-verify ... -DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON && cmake --build build-verify --parallel 28
ctest --test-dir build-verify -N -L integration-verify | grep -c 'Test #'                      # 742 + 2 + 2 (the 371 x 2 ambient entries plus the four control entries)
ctest --test-dir build-verify -N -L integration-gpu | sed 's/\.Verify\././' | sort -u | wc -l  # parity with the pull registration set
# On the contract tree the library never arms (the comparator lands in p1/core c4), so the arming case must SKIP-or-FAIL, never pass:
ctest --test-dir build-verify -R 'DirectGLES\.Verify\.PipeVerifyArmingScenario' --output-on-failure   # expected red (REQUIRE_GPU on) - the lane is falsifiable
```
Full functional verification of E happens on the integrated tree (D.3).

### C.5 File-ownership table (no two packages edit the same file)

| file / glob | A core | B espryt | C magma | E verify | integrator |
|---|---|---|---|---|---|
| `MobileGL/MG_Pipe/{PipeInputsSwitch.h, FillPoints.def, Coverage.def, PipeFields.def, MGPipe.h, generated/*.inc}` | owner | – | – | – | – |
| `scripts/gen_pipe.py` | owner | – | – | – | – |
| `MobileGL/MG_Backend/MGPipe/**` (new) | owner | – | – | – | – |
| `MobileGL/MG_Impl/Pipe/**` (new), `MobileGL/MG_Impl/GLImpl/{Drawing,Framebuffer,Texture,Getter,Program,Query,Sync}/*.cpp` | owner | – | – | – | – |
| root `CMakeLists.txt`, `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | owner | – | – | – | – |
| `MobileGL/MG_Test/Pipe/**` | owner | – | – | – | – |
| `MobileGL/MG_Backend/DirectGLES/{DirectGLES.cpp, Managers.cpp, MultiDraw.cpp, Utils.cpp}` | – | owner | – | – | – |
| `MobileGL/MG_Backend/DirectVulkan/{DirectVulkan.cpp, BackendObject_DirectVulkan.cpp, Renderer/UniformManager.cpp, Renderer/VulkanRenderer.cpp, Renderer/VkClearManager.cpp, Renderer/VkRenderPassManager.cpp, Renderer/VkTextureManager.cpp, Renderer/VertexInputStateFactory.h}` | – | – | owner | – | – |
| `MobileGL/MG_IntegrationTest/**`, `tools/trace_replay/{run_trace_case.cmake, CMakeLists.txt, trace_cases.json, trace_cases.py}`, `scripts/symbol_report.py`, `.github/workflows/test.yml` | – | – | – | owner | – |
| `docs/Disaggregated/*.md`, `scripts/gen_pipe_dirty_surface.py` (comment), `test.yml:871-872` comment (E lands it) | – | – | – | – | owner |
| everything else | nobody | nobody | nobody | nobody | nobody |

The contract (`p1/contract`) is the only shared code, and it is a single commit all three other branches descend from; the integrator's rebase of B/C/E onto the final `p1/core` therefore never sees an edit on both sides of one file.

---

## D. Integration order and what the integrator runs

D.0 Preconditions: baseline captures of §C (`~/w7/p1-before-*`); `~/w7/pipe` clean at `087685d1`; fixture binaries hidden as in `wsl_p05_trees.sh`.

D.1 Order: **core → espryt → magma → verify.** Copy `scratchpad/wf2/wsl_integrate_p05.sh` → `wsl_integrate_p1.sh` (`p05`→`p1`); per slug: rebase `p1/<slug>` onto `feat/disaggregated` in its worktree, `git merge --ff-only`, then in `~/w7/pipe`:
```
cmake --build build-linux --parallel 28 && ctest --test-dir build-linux -L unit --no-tests=error --output-on-failure
python3 scripts/gen_pipe.py --check && python3 scripts/gen_pipe.py --self-test
python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0 --markdown ~/w7/p1-symbol-<slug>.md   # 0/0/0/0 and .text +0 after every merge
```
After espryt and magma additionally: `grep -rc "pGLContext" MobileGL/MG_Backend | grep -v ":0$"` → empty (G2); `ctest --test-dir build-linux -L integration-gpu --no-tests=error -j 4` green by name (`comm -23 ~/w7/p1-before-ctest-names.txt` empty).

D.2 Push build on the integrated tree (`~/w7/pipe/build-push`, `-DMOBILEGL_PIPE_PUSH=ON`): unit + `integration-gpu` green — no `Fatal{UnmigratedPipeInput}` is possible here (the poison is off in a Release/INFO push build without VERIFY), so this run proves only that the filler and accessors are functionally equivalent under pull semantics.

D.3 Verify build (`~/w7/pipe/build-verify`, `-DMOBILEGL_PIPE_VERIFY=ON -DMOBILEGL_ITEST_REQUIRE_GPU=ON`):
```
ctest --test-dir build-verify -L unit --no-tests=error
ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4 --output-on-failure   # 746 entries green: G3 half one
grep -c "MGPipe: verify armed" build-verify/MobileGL/MG_IntegrationTest/pipe-verify-*.log        # > 0 in both
MOBILEGL_PIPE_VERIFY_CORRUPT=GetRenderStateParameters ctest --test-dir build-verify -L integration-verify -R 'DirectGLES\.Verify\..*ClearThenReadPixels' ; echo rc=$?   # non-zero: G4
MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit ctest --test-dir build-verify -L integration-verify -R 'Mipmap' ; echo rc=$?   # non-zero: G5
ctest --test-dir build-verify -R 'PoisonOmitted\.|VerifyCorrupted\.' --no-tests=error                # the four control entries green (they assert the red)
# 40 traces (79 desktop cases) under verify - the second half of G3:
cp scratchpad/wf2/retrace_gate.py ~/w7/retrace_gate.py
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/pipe --lib ~/w7/pipe/build-verify/libMobileGL.so --out ~/w7/retrace-out/p1-verify -j 4
grep -L "MGPipe: verify armed" ~/w7/retrace-out/p1-verify/*/mobilegl.log      # empty: every case armed
grep -l "Fatal{" ~/w7/retrace-out/p1-verify/*/mobilegl.log                      # empty
```
A `Fatal{UnmigratedPipeInput, "F@V"}` here is a missing row in `FillPoints.def`'s class table: add it (never sticky), regenerate, rerun. A `Fatal{PipeVerifyDiffer, ..., where=read}` is a real push/pull semantic divergence: record it in `MEASUREMENTS.md` with the site and decide (plan owner) whether the pull value or the boundary value is the intended one; it is not silenced.

D.4 CI: `gh workflow run test.yml --ref feat/disaggregated` and, once green, `gh workflow run test.yml --ref feat/disaggregated -f baseline_sha=087685d1` for `monolith-symbol-report`; record both run URLs. Expected: `pipe-gates`, `include-graph-check`, `test`, `integration`, `integration-verify`, `retrace` (77), `retrace-verify` (16), `monolith-symbol-report` green.

D.5 Docs (integrator-owned, one commit; `scripts/check_doc_citations.py docs/Disaggregated/*.md` must stay clean): `README.md:3` status → P1 landed with the hash; `ROADMAP.md:17` ✅ with "277 处（274 行）+ 58 行", "Espryt 32 / Magma 56（63 字段）", "83 个填充点 / 69 个 verb"; `ROADMAP.md:18` unchanged (dirty-surface stays P2); `ARCHITECTURE.md:172,568` "P1 起成为门/P1 成门" → P2; `:344` counts; `:349` add `MOBILEGL_PIPE_POISON`'s definition and the `MOBILEGL_DEBUG` correction; `:553` note that `MGPipeImpl_*.cpp` land with the first installed table; `:583` `MOBILEGL_PIPE_VERIFY` → 已落地, add `MOBILEGL_PIPE_PUSH` (CMake) row and the three runtime knobs at `:588-597`; `MEASUREMENTS.md`: new §6 (the D10 table), §4 "61 字段" → 63 / "63 verify payload" → 69, the G1 symbol-report summary lines, the verify-lane wall times, and the P1 `.text` attribution (expected: "0 bytes; 58 lines converted, none code-bearing"). Then `[Merge]` to `dev` per the milestone flow (P1 is not one of the five full-CTS boundaries, `ROADMAP.md:34`, but the `dev` merge still triggers the full `gl44to46` run on both devices — schedule it, off the critical path).

---

## E. Risks and mitigations

| risk | mitigation |
|---|---|
| **A fill table that is too small turns the verify lane red for a reason that is not a bug.** The class masks in D7 are derived statically from `scout-pull-sites.md` §4.2; a helper reached through a path the reachability trace missed reads a field its class does not list. | This is exactly what the lane exists to find, and the fix is mechanical (add the row, regenerate). The rule in D7 — never sticky — keeps the fix honest. Budget one day of D.3 iteration for it; each Fatal names field and verb. |
| **A read outside any verb.** `InvalidateCompileEnv` at backend init (`BackendObject_DirectVulkan.cpp:389,787`) and `RecordError` from sampler sync run with no verb; texture-sync helpers (`Managers.cpp:3645-4737`) are reached only from verbs (callers verified: `DirectGLES.cpp:1325,1567,1632`, `Managers.cpp:5234`, all under `SyncNeccessaryTextures`/FBO sync); Magma's render-pass and view helpers are reached only from draw/clear/blit (`GetOrCreateRenderPass` callers `VulkanRenderer.cpp:6363,6833,6846,7196,7229,8491`); `Present` reads nothing. | The seven sticky fields cover the two genuine out-of-verb cases. If the lane finds another (`@<none>` in the Fatal), the answer is a fill point at that boundary (an EGL virtual in `EGLImpl.cpp`), never a sticky flag. |
| **`.text` moves in the pull build.** The 13 guard/compare rewrites and the `.get()` block are like-for-like, but a compiler may schedule a `const void*` compare differently. | `symbol_report.py --threshold 0` after every merge (D.1); any delta is attributed line by line in the commit message and in `MEASUREMENTS.md`; the gate text allows an attributed delta, not an unexplained one. |
| **The verify lane is green because it never armed.** A `MOBILEGL_PIPE_VERIFY=1` against a library without the option is a no-op that looks green. | `run_trace_case.cmake` fails a verify run whose log lacks the arming line; `PipeVerifyArmingScenario` fails the itest lane the same way; `--no-tests=error` fails a lane whose registration was skipped; the retrace-verify job asserts the swapped `.so` exports `MGPipeVerifyInputs`. |
| **The entry compare is tautological in P1 and someone reads "zero divergence" as more than it is.** | D8 states it; `MEASUREMENTS.md` records which arm proved what; the compare-at-read arm and the poison are the P1 semantic content, and the CORRUPT control keeps the entry compare falsifiable until P2 gives it a real first arm. |
| **Verify cost.** 5–10× on 742 itest entries and 79 traces; the 1800 s trace cases already sit above ctest's 1500 s default. | `TIMEOUT 900` on the verify lanes; `--timeout 10800` on retrace-verify; `timeout-minutes` on every new job (the file has none today); per-push CI runs the 8-case verify subset, the full 79 runs at the P1 exit on WSL (D.3) and on `workflow_dispatch`; `-j 4` locally (each case unpacks a multi-GB trace). |
| **`std::abort()` inside a scenario kills the whole `MobileGLIntegrationTest` process.** | ctest runs one case per process (`gtest_discover_tests`), so a Fatal reds exactly one entry and leaves a core; the omission scenario forks first (`HeadlessGL.cpp:323-343`). |
| **The `GetFramebufferBindingSlotFast` cache bypasses the accessor** for five Espryt reads. | Known and recorded (D4); the entry compare still covers the field; P2's Espryt 0b deletes the cache. |
| **`Coverage.def` and the stale vendored inventory disagree** (a 62-accessor tree, a 61-row table, a dead row). | D12's `scan_live_accessors()` is a `pipe-gates` gate from `c5` on; the dead row is labelled; re-vendoring is a recorded P2 item. |
| **Two `PipeInputs` field lists drift from `RenderStateParameters` when P2 adds `FramebufferSrgb`/`DepthClamp` storage** (`ROADMAP.md:18`). | D8's struct-member coverage check makes a member without an `F(...)` entry a `pipe-gates` failure; `MGL_RESIDUAL_BLOCK_SIZE` and the value-header `static_assert` catch the size. |
| **MSVC / Android.** `fork()`-based tests and the itest lanes are POSIX; the push/verify options are desktop-only in P1. | Windows is not a correctness gate (`ROADMAP.md:7`); the new sources compile on MSVC (no POSIX in `PipeInputs`/`PipeFill`); the fork tests are `#if !_WIN32` with a visible skip; `apk.yml` is unaffected (options OFF). |
| **A/B/C/E land against an outdated contract.** | The contract is one commit, tagged; A must not change any signature in `PipeInputs.h`, `PipeFill.h` or `PipeInputsSwitch.h` after `p1/contract` without telling B/C/E (a rebase-time compile error is the backstop). |

---

## B.7 (appendix) — the 83 fill-point statements at `087685d1`

Insert `MGP_FILL(<Verb>);` immediately before each statement (D7 placement):

- `GL_Drawing.cpp`: 530 Clear; 538 DrawElements; 547 MultiDrawElements; 556 MultiDrawElementsBaseVertex; 565 DrawArrays; 573 MultiDrawArrays; 582 DrawElementsBaseVertex; 591 MultiDrawElementsIndirect; 599 MultiDrawArraysIndirect; 608 MultiDrawElementsIndirectCount; 618 MultiDrawArraysIndirectCount; 628 DrawRangeElementsBaseVertex; 638 DrawRangeElements; 648 DrawElementsInstancedBaseVertexBaseInstance; 658 DrawElementsInstancedBaseVertex; 668 DrawElementsInstancedBaseInstance; 678 DrawElementsInstanced; 686 DrawElementsIndirect; 694 DrawArraysInstancedBaseInstance; 703 DrawArraysInstanced; 711 DrawArraysIndirect; 742 DispatchCompute (`dispatchCompute(numGroupsX, …)`); 794 DispatchComputeIndirect; 815 PatchParameteri; 885 MemoryBarrier; 907 MemoryBarrier (`TextureBarrier`); 919 MemoryBarrierByRegion; 1241 BeginTransformFeedback; 1323 EndTransformFeedback; 1331 FenceSync; 1332 ClientWaitSync; 1334 DeleteSync; 1352 PauseTransformFeedback; 1366 ResumeTransformFeedback; 1571 DeleteTransformFeedback; 1602 BindTransformFeedback — **36**
- `GL_Framebuffer.cpp`: 619 BlitFramebuffer; 632 BlitNamedFramebuffer; 643 ClearNamedFramebufferfv; 653 ClearNamedFramebufferfi; 663 ClearNamedFramebufferiv; 673 ClearNamedFramebufferuiv; 2732 ClearBufferfi; 2738 ClearBufferfv; 2744 ClearBufferuiv; 2750 ClearBufferiv; 2997 ReadPixels — **11**
- `GL_Texture.cpp`: 1079 ReadPixels; 1622 GenerateMipmap; 4027 CopyTexSubImage2D; 4043 CopyImageSubData; 4464 CopyTexImage2D; 5074 GetTexImage; 6456 GetTextureImage; 6660 BindImageTexture — **8**
- `GL_Getter.cpp`: 1176 GetIntegeri_v; 1356 GetGpuTimestampNs; 2266 GetGpuTimestampNs — **3**
- `GL_Program.cpp`: 3401 ShaderStorageBlockBinding — **1**
- `GL_Query.cpp`: 167, 274, 320, 444, 591, 915 DeleteBackendQuery; 181 EndTimeElapsedQuery; 261, 301 GetQueryResult64; 289 IsQueryResultAvailable; 425, 607 EndOcclusionQuery; 523 BeginXfbPrimitivesQuery; 530 BeginOcclusionQuery; 534 BeginTimeElapsedQuery; 582 EndXfbPrimitivesQuery; 646 QueryCounterTimestamp; 775 IsTimerQuerySupported — **18**
- `GL_Sync.cpp`: 59 FenceSync; 97 ClientWaitSync; 122 WaitSync; 142 DeleteSync; 177 GetSyncStatus; 229 DeleteSync — **6**

Not fill points: the 8 null-check/capture lines the 91-line grep also matches (`GL_Drawing.cpp:1010,1031,1255,1329`, `GL_Query.cpp:218,471,545,768`, `GL_Texture.cpp:6455,6715`, `ProgramInterface.h:27`), `CompileEnv.cpp:135,137` (D1), and the EGL virtuals (`EGLImpl.cpp:178,267,284,347,448`).
