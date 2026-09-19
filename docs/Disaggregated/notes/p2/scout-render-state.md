# Scout: render state → CSO + dynamic state (P2 groundwork)

Worktree `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch
`feat/disaggregated` @ `e7a6a72f`. Every line number below was opened in this worktree at that
commit. Paths are repo-relative to that root unless absolute.

---

## 1. `RenderStateParameters` as it is today

File: `MobileGL/MG_Pipe/MGPipeValueTypes.h`, `struct RenderStateParameters` at **line 239**,
closing brace **line 387**. It moved here from `MG_State/GLState` in P0.5; the MG_State headers
now include this file, so every old spelling still compiles (header comment lines 17-25).

### 1.1 Trip wires around it (`MGPipeValueTypes.h:539-544`)

```
539  static_assert(std::is_trivially_copyable_v<RenderStateParameters>);
540  static_assert(std::is_standard_layout_v<RenderStateParameters>);   // offsetof legality
541  static_assert(sizeof(RenderStateParameters) == 1168, "...MGL_RESIDUAL_BLOCK_SIZE and the Espryt spans depend on it");
543  static_assert(offsetof(RenderStateParameters, BlendStates) < offsetof(RenderStateParameters, LogicOp));
544  static_assert(std::tuple_size_v<decltype(RenderStateParameters::BlendStates)> == kMGMaxDrawBuffers);
```

Supporting element sizes, also asserted in the same file: `PerBufferBlendState` = 28
(`:537`), `StencilFaceState` = 28 (`:538`), `PixelStoreParameters` = 28 (`:536`).
`kMGMaxDrawBuffers = 8` at `:30`; `MAX_VIEWPORTS = 16` is a `static constexpr` member at
`:244` (no storage).

Vector sizes (from `MobileGL/MG_Util/Math/VectorTypes.h:16-18` — `VecBase` is a bare
`Array<T,N>`, `:215-224` for the aliases): `FloatVec4`/`IntVec4` = 16, `FloatVec2` = 8,
`BoolVec4` = 4 (`Bool` is 1 byte), and enum classes default to a 4-byte underlying type.

### 1.2 Every member, in declaration order, with computed offset/size

Offsets below are derived by hand from the above sizes and align-4 layout; they sum to exactly
**1168**, which is the `static_assert` at `:541`, so the derivation is self-checking. **P2 must
still compute them with `offsetof` in `MGPipeRenderStateSpans.cpp`** — `scripts/gen_pipe.py:72-74`
explicitly refuses to guess them in python.

| # | Member | Decl line | Off | Size | Class today | Notes |
|---|--------|-----------|-----|------|-------------|-------|
| 1 | `Array<FloatVec4,16> Viewports` | 255 | 0 | 256 | **dynamic** | float state since GL 4.1; index 0 is what `glViewport`/`GetIntegerv` address |
| 2 | `Float LineWidth` | 256 | 256 | 4 | **dynamic** | `VK_DYNAMIC_STATE_LINE_WIDTH` |
| 3 | `Float PointSize` | 257 | 260 | 4 | **dynamic** | Espryt head span, ES 1.x only |
| 4 | `Uint PatchVertices` | 259 | 264 | 4 | **pipeline** | in G7 list |
| 5 | `FloatVec4 PatchDefaultOuterLevel` | 265 | 268 | 16 | **pipeline** | in G7 list; baked into synthesized TCS |
| 6 | `FloatVec2 PatchDefaultInnerLevel` | 266 | 284 | 8 | **pipeline** | in G7 list |
| 7 | `Float PolygonOffsetFactor` | 267 | 292 | 4 | **dynamic** | `VK_DYNAMIC_STATE_DEPTH_BIAS` |
| 8 | `Float PolygonOffsetUnits` | 268 | 296 | 4 | **dynamic** | idem |
| 9 | `Float PolygonOffsetClamp` | 272 | 300 | 4 | *neither* | **no backend reads it** (grep: 0 hits under `MG_Backend/`) |
| 10 | `GLenum ClipOrigin` | 276 | 304 | 4 | *neither* | getter-only; only reader is `MG_Impl/GLImpl/Getter/GL_Getter.cpp:1541` |
| 11 | `GLenum ClipDepthMode` | 277 | 308 | 4 | *neither* | idem, `GL_Getter.cpp:1544` |
| 12 | `Array<PerBufferBlendState,8> BlendStates` | 280 | **312** | 224 | **pipeline** | span boundary: `offsetof(BlendStates)` = Espryt `kBlendSpanBegin` |
| 13 | `LogicOperation LogicOp` | 281 | **536** | 4 | **pipeline** | span boundary: `offsetof(LogicOp)` = Espryt `kBlendSpanEnd` |
| 14 | `Bool DepthTestEnabled` | 284 | 540 | 1 | **pipeline** | |
| 15 | `DepthTestFunc DepthFunc` | 285 | 544 | 4 | **pipeline** | |
| 16 | `Bool DepthMask` | 286 | 548 | 1 | **pipeline** | |
| 17 | `Array<BoolVec4,8> ColorMasks` | 290 | 549 | 32 | **pipeline** | align 1; note the odd offset |
| 18 | `FloatVec4 ClearColor` | 293 | 584 | 16 | **dynamic** | |
| 19 | `Float ClearDepth` | 294 | 600 | 4 | **dynamic** | |
| 20 | `Uint32 ClearStencil` | 295 | 604 | 4 | **dynamic** | |
| 21 | `FloatVec4 BlendColor` | 296 | 608 | 16 | **dynamic** | `VK_DYNAMIC_STATE_BLEND_CONSTANTS` |
| 22 | `Array<FloatVec2,16> DepthRanges` | 303 | 624 | 128 | **dynamic** | rides `VkViewport::min/maxDepth` |
| 23 | `Float SampleCoverageValue` | 304 | 752 | 4 | *see §2 conflict* | Espryt-only; `SetSampleCoverage` bumps the **pipeline** version but the field is NOT in the G7 list |
| 24 | `Bool SampleCoverageInvert` | 305 | 756 | 1 | idem | idem |
| 25 | `Uint32 SampleMaskValue` | 306 | 760 | 4 | **pipeline** | in G7 list |
| 26 | `Float MinSampleShadingValue` | 310 | 764 | 4 | **pipeline** | in G7 list |
| 27 | `Array<StencilFaceState,2> StencilStates` | 311 | 768 | 56 | **split** | `.Func`/`.FailOp`/`.PassDepthFailOp`/`.PassDepthPassOp` = pipeline; `.Ref`/`.ValueMask`/`.WriteMask` = dynamic — the one member that straddles the split |
| 28 | `Bool CullFaceEnabled` | 314 | 824 | 1 | **pipeline** | |
| 29 | `CullFaceMode CullFaceModeSetting` | 315 | 828 | 4 | **pipeline** | |
| 30 | `FrontFaceMode FrontFaceModeSetting` | 316 | 832 | 4 | *Espryt-only* | Magma never reads it (see §4.4) |
| 31 | `ProvokingVertexMode ProvokingVertexModeSetting` | 317 | 836 | 4 | *unhashed pipeline* | Magma reads it at `VulkanRenderer.cpp:12768` but it is not in the hash |
| 32 | `GLenum LineSmoothHint` | 320 | 840 | 4 | *neither* | no backend reader |
| 33 | `GLenum PolygonSmoothHint` | 321 | 844 | 4 | *neither* | |
| 34 | `GLenum TextureCompressionHint` | 322 | 848 | 4 | *neither* | |
| 35 | `GLenum FragmentShaderDerivativeHint` | 323 | 852 | 4 | *neither* | |
| 36 | `Float PointFadeThresholdSize` | 326 | 856 | 4 | *neither* | no backend reader |
| 37 | `GLenum PointSpriteCoordOrigin` | 327 | 860 | 4 | *neither* | |
| 38 | `GLenum ClampReadColor` | 330 | 864 | 4 | **dynamic** | read at `VulkanRenderer.cpp:2772`; Coverage.def maps it to `SetDynamicState` |
| 39 | `GLenum PolygonModeFront` | 334 | 868 | 4 | **pipeline** | in G7 list |
| 40 | `GLenum PolygonModeBack` | 335 | 872 | 4 | *neither* | never read by a backend (Espryt pushes `GL_FRONT_AND_BACK` with the **front** value at `DirectGLES.cpp:2513`) |
| 41 | `Uint32 PrimitiveRestartIndex` | 339 | 876 | 4 | *per-verb* | Coverage.def maps it to `DrawVbo`, not to a CSO |
| 42 | `Bool ColorLogicOpEnabled` | 342 | 880 | 1 | **pipeline** | |
| 43 | `Bool DebugOutputEnabled` | 343 | 881 | 1 | *neither* | |
| 44 | `Bool DebugOutputSynchronousEnabled` | 344 | 882 | 1 | *neither* | |
| 45 | `Bool DitherEnabled` | 345 | 883 | 1 | *Espryt-only* | `SYNC_CAPABILITY(Dither, …)` `DirectGLES.cpp:2112` |
| 46 | `Bool LineSmoothEnabled` | 346 | 884 | 1 | *neither* | |
| 47 | `Bool MultisampleEnabled` | 347 | 885 | 1 | **pipeline** | in G7 list (feeds `ResolveEffectiveSampleMask`) + Espryt `:2113` |
| 48 | `Bool PolygonOffsetFillEnabled` | 348 | 886 | 1 | **pipeline** | |
| 49 | `Bool PolygonOffsetLineEnabled` | 349 | 887 | 1 | *neither* | |
| 50 | `Bool PolygonOffsetPointEnabled` | 350 | 888 | 1 | *neither* | |
| 51 | `Bool PolygonSmoothEnabled` | 351 | 889 | 1 | *neither* | |
| 52 | `Bool PrimitiveRestartEnabled` | 352 | 890 | 1 | **pipeline** | |
| 53 | `Bool PrimitiveRestartFixedIndexEnabled` | 353 | 891 | 1 | **pipeline** | |
| 54 | `Bool RasterizerDiscardEnabled` | 354 | 892 | 1 | **pipeline** | |
| 55 | `Bool SampleAlphaToCoverageEnabled` | 355 | 893 | 1 | *Espryt-only* | `SYNC_CAPABILITY` `:2114`; Magma has **no** `alphaToCoverageEnable` at all |
| 56 | `Bool SampleAlphaToOneEnabled` | 356 | 894 | 1 | *neither* | not even in Espryt's macro list |
| 57 | `Bool SampleCoverageEnabled` | 357 | 895 | 1 | *Espryt-only* | `SYNC_CAPABILITY` `:2115` |
| 58 | `Bool SampleMaskEnabled` | 358 | 896 | 1 | **pipeline** | in G7 list |
| 59 | `Bool SampleShadingEnabled` | 359 | 897 | 1 | **pipeline** | in G7 list |
| 60 | `Bool StencilTestEnabled` | 360 | 898 | 1 | **pipeline** | |
| 61 | `Bool ProgramPointSizeEnabled` | 361 | 899 | 1 | *neither* | |
| 62 | `Uint32 ScissorTestEnabledMask` | 367 | 900 | 4 | *conflict* | comment `:362-367` says it **does** bump the pipeline version (Espryt turns it into a real `glEnable`), yet it is a **dynamic** input on Magma (`DynamicTailKey::scissorEnabled`) and is absent from the G7 list |
| 63 | `Array<IntVec4,16> ScissorBoxes` | 368 | 904 | 256 | **dynamic** | |
| 64 | `Uint32 ScissorBoxWrittenMask` | 380 | 1160 | 4 | **dynamic** | deliberately beside `ScissorBoxes` so it lands in Espryt's tail span (`:376-379`) |
| 65 | `Uint32 ClipDistanceEnabledMask` | 386 | 1164 | 4 | **dynamic** | `RenderState.cpp:374-378` documents "deliberately NOT `BumpVersions()`" |

Total = **1168** ✓.

The three Espryt spans therefore are: **head `[0, 312)`**, **blend `[312, 536)`**,
**tail `[536, 1168)`**.

### 1.3 The `FramebufferSrgb` / `DepthClamp` storage hole

`CapabilityInput::DepthClamp` (`MGPipeValueTypes.h:182`) and `CapabilityInput::FramebufferSrgb`
(`:185`) exist in the enum and in all three converters
(`MG_Util/Converters/GLToMG/RenderStateEnumConverter.cpp:261,267`;
`MGToGL/…:261,267`; `MGToStr/…:243,249`) — but **`RenderStateParameters` has no
`FramebufferSrgbEnabled` / `DepthClampEnabled` field at all.**

Consequence chain:
- `RenderState::SetCapability` (`MobileGL/MG_State/GLState/RenderState/RenderState.cpp:308-385`)
  falls through to `default: break;` at `:381-382` for both — the `glEnable` is silently swallowed,
  no version bumps.
- `RenderState::IsCapabilityEnabled` (`:387-432`) falls to `default: return false;` at `:429-430`.
- Under push, `MG_Impl/Pipe/PipeFill.cpp:229-235` copies the whole capability array including
  FramebufferSrgb — the comment at `:230` says it "copies today's constant false".
- **Six backend read points are hard-constant `false` today**, all for `FramebufferSrgb`
  (`DepthClamp` has zero backend readers):
  1. `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:2168` (the sRGB-write block in SyncRenderState)
  2. `MobileGL/MG_Backend/DirectVulkan/Renderer/VkClearManager.cpp:58`
  3. `MobileGL/MG_Backend/DirectVulkan/Renderer/VkRenderPassManager.cpp:614`
  4. `…/VkRenderPassManager.cpp:965-968`
  5. `…/VkRenderPassManager.cpp:1112`
  6. `MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp:953`
- This is called out as an open question in `MobileGL/MG_Pipe/generated/PipeSpanTable.inc:24-26`
  and in `docs/Disaggregated/ARCHITECTURE.md:189` (the doc's proposal: give both real storage and
  put `FramebufferSrgb` on the **pipeline** side, because it changes the interpretation of the
  attachment/blend). `docs/Disaggregated/ROADMAP.md:18` lists "补 `FramebufferSrgb`/`DepthClamp`
  存储" as P2 scope.

Note the size trap: adding two `Bool`s changes `sizeof(RenderStateParameters)` away from 1168 and
breaks `MGPipeValueTypes.h:541`, `MGL_RESIDUAL_BLOCK_SIZE` (`MG_Pipe/MGPipeTypes.h:536`, which
"only ever goes DOWN" — a growth is a deliberate build break, `:529-535`), and
`MG_Test/Pipe/PipeCatalogueTest.cpp:111-112`. There are 7 free padding bytes after
`ProgramPointSizeEnabled` at offsets 900… no — offsets 880-899 are 20 packed bools with **no**
padding, and 549+32=581 → 584 leaves 3 free bytes after `ColorMasks`. Placing the two new bools
in that 3-byte hole (i.e. declaring them right after `ColorMasks` and before `ClearColor`) keeps
`sizeof` at 1168 but would move them into the tail span and change no offsets — worth checking
against the field-order load-bearing note in `ARCHITECTURE.md:183`.

### 1.4 The mirror field list

`MobileGL/MG_Pipe/PipeFields.def:230-247` — `MGP_FIELDS_RenderStateParameters(F)` names all 65
members in the same order. `gen_pipe.py` asserts every direct data member appears here
(`PipeFields.def:224-228`), so **a new member without a row here fails the pipe gates**. That is
the enforcement point for the FramebufferSrgb/DepthClamp addition.

---

## 2. Frontend `RenderState` — every public setter and which version it bumps

Files: `MobileGL/MG_State/GLState/RenderState/RenderState.h` (180 lines),
`MobileGL/MG_State/GLState/RenderState/RenderState.cpp` (973 lines).

State: `Uint16 m_version` (`RenderState.h:164`), `Uint16 m_pipelineStateVersion` (`:171`,
with the rationale comment at `:165-170`), `RenderStateParameters m_parameters` (`:172`),
plus two `PixelStoreParameters` (`:175-176`) that live **outside** the parameter block.
`BumpVersions()` is the private inline at `RenderState.h:159-162` — `++m_version;
++m_pipelineStateVersion;`.

Readers: `GetVersion()` `:20`/`.cpp:53`, `GetPipelineStateVersion()` `:22`/`.cpp:57`,
`GetAllParameters()` `:23`/`.cpp:61`.

Constructor `RenderState::RenderState()` `.cpp:37-51` — all-true `ColorMasks`, `(0,1)`
`DepthRanges`; the viewport/scissor all-zero default deliberately means "never written".

### 2.1 Group A — setters that bump **both** versions (`BumpVersions()` / `SET_CAPABILITY`)

This is the list the G7 consistency test must enumerate on the "pipeline subset hash MUST move"
side.

| Setter | .cpp lines |
|---|---|
| `SetPolygonMode(front, back)` | 174-179 |
| `SetPatchVertices` | 210-215 |
| `SetPatchDefaultOuterLevel` | 229-234 (bitwise-equal guard, `:225-228`) |
| `SetPatchDefaultInnerLevel` | 240-245 |
| `SetCapability` — the 22 `SET_CAPABILITY` arms | macro 309-314, arms 317-338 |
| `SetCapability(Blend)` | 339-348 |
| `SetCapability(ScissorTest)` | 353-359 |
| `SetCapabilityIndexed(ScissorTest, i)` | 442-453 |
| `SetCapabilityIndexed(Blend, i)` | 455-469 |
| `SetBlendFunc` | 497-513 |
| `SetBlendFuncIndexed` | 523-539 |
| `SetBlendEquation` | 553-565 |
| `SetBlendEquationIndexed` | 572-584 |
| `SetLogicOp` | 595-600 |
| `SetDepthFunc` | 607-612 |
| `SetDepthMask` | 618-623 |
| `SetStencilOp` | 652-664 |
| `SetColorMask` | 671-681 |
| `SetColorMaskIndexed` | 688-692 |
| `SetSampleCoverage` | 778-784 |
| `SetSampleMaskValue` | 794-799 |
| `SetMinSampleShadingValue` | 805-813 (explicit comment: Vulkan bakes `minSampleShading`) |
| `SetCullFaceMode` | 882-887 |
| `SetFrontFaceMode` | 893-898 |
| `SetProvokingVertexMode` | 904-909 |

The 22 `SET_CAPABILITY` names, in source order (`.cpp:317-338`): `ColorLogicOp`, `DebugOutput`,
`DebugOutputSynchronous`, `DepthTest`, `CullFace`, `Dither`, `LineSmooth`, `Multisample`,
`PolygonOffsetFill`, `PolygonOffsetLine`, `PolygonOffsetPoint`, `PolygonSmooth`,
`PrimitiveRestart`, `PrimitiveRestartFixedIndex`, `RasterizerDiscard`, `SampleAlphaToCoverage`,
`SampleAlphaToOne`, `SampleCoverage`, `SampleMask`, `SampleShading`, `StencilTest`,
`ProgramPointSize`.

### 2.2 Group B — setters that bump **`m_version` only**

| Setter | .cpp lines |
|---|---|
| `SetViewport` (broadcasts all 16) | 69-79 |
| `SetViewportIndexed` | 90-99 |
| `SetLineWidth` | 109-114 |
| `SetHint` | 120-132 |
| `SetPointFadeThresholdSize` | 144-148 |
| `SetPointSpriteCoordOrigin` | 154-158 |
| `SetClampReadColor` | 164-168 |
| `SetPrimitiveRestartIndex` | 189-193 |
| `SetPointSize` | 199-204 |
| `SetPolygonOffset` (delegates to Clamped) | 251-258 |
| `SetPolygonOffsetClamped` | 268-277 |
| `SetClipControl` | 283-289 |
| `SetCapability(ClipDistance0..7)` | 360-380 (explicit "deliberately NOT BumpVersions", `:374-377`) |
| `SetStencilMask` | 644-650 |
| `SetClearColor` | 699-704 |
| `SetClearDepth` | 710-715 |
| `SetClearStencil` | 721-726 |
| `SetBlendColor` | 732-737 |
| `SetDepthRange` (broadcasts all 16) | 745-753 |
| `SetDepthRangeIndexed` | 759-768 |
| `SetScissorBox` | 918-941 (also flips `ScissorBoxWrittenMask`; the transition itself counts as a change, `:925-939`) |
| `SetScissorBoxIndexed` | 947-962 |

### 2.3 Group C — the one conditional setter

`SetStencilFunc(face, func, ref, mask)` `.cpp:629-642`: always `++m_version` (`:640`), and
`++m_pipelineStateVersion` **only if `Func` moved** (`:636`, `:641`). Comment `:633-635`: `Ref`
and `ValueMask` are `VK_DYNAMIC_STATE_STENCIL_REFERENCE`/`_COMPARE_MASK`. This is the setter that
proves the split is per-field, not per-setter — a G7 test that walks setters must special-case it
(drive it twice: once changing only `Ref`, once changing `Func`).

### 2.4 Group D — setters that bump **nothing**

`SetPixelStoreParam` `.cpp:820-848` (macro `:821-825`). Pixel-store state is not in the parameter
block and moves neither counter; it becomes `set_pixel_pack_state` (Coverage.def).

### 2.5 Conflicts the G7 test will surface immediately

The G7 invariant per `docs/Disaggregated/ARCHITECTURE.md:186` and
`generated/PipeSpanTable.inc:15-20` is: **pipeline-subset hash moves ⟺ `m_pipelineStateVersion`
moves.** Against today's tree that invariant is **already false in at least six ways**, all in the
"version moved but hash did not" direction:

1. `SetSampleCoverage` (`.cpp:778-784`) bumps the pipeline version, but neither
   `SampleCoverageValue` nor `SampleCoverageInvert` is in `kMGPipePipelineStateMembers`.
2. `SetFrontFaceMode` (`.cpp:893-898`) bumps it; `FrontFaceModeSetting` is not in the list, and
   (see §4.4) Magma does not read it at all.
3. `SetProvokingVertexMode` (`.cpp:904-909`) bumps it; `ProvokingVertexModeSetting` is not in the
   list. `Coverage.def:74-77` already flags this ("Not in ComputePipelineStateHash today even
   though Vulkan makes it pipeline state; recorded here so the G7 chunk table has to answer for
   it before it freezes").
4. `SetCapability(ScissorTest)` / `SetCapabilityIndexed(ScissorTest, …)` bump it;
   `ScissorTestEnabledMask` is not in the list and is a **dynamic** input on Magma
   (`DynamicTailKey::scissorEnabled`).
5. Eleven of the 22 `SET_CAPABILITY` names are absent from the list: `DebugOutput`,
   `DebugOutputSynchronous`, `Dither`, `LineSmooth`, `PolygonOffsetLine`, `PolygonOffsetPoint`,
   `PolygonSmooth`, `SampleAlphaToCoverage`, `SampleAlphaToOne`, `SampleCoverage`,
   `ProgramPointSize`.
6. `SetPolygonMode(front, back)` bumps it even when only `PolygonModeBack` moved;
   `PolygonModeBack` is not in the list (and no backend reads it).

Directionally the other way there is nothing: every one of the 24 listed members is written only
by a Group-A setter (`StencilStates` is the exception, and its dynamic third is written by
`SetStencilMask`/`SetStencilFunc`'s `Ref`, which correctly do **not** bump the pipeline version).

**Implication for P2**: either the chunk table grows past the 24 hashed members (making the
pipeline subset a superset of what Magma hashes), or several Group-A setters must be demoted to
`++m_version`. Demoting is behaviour-changing for Espryt, which pushes `Dither`,
`SampleAlphaToCoverage`, `SampleCoverage` and the scissor-test enable through
`SYNC_CAPABILITY` — but Espryt gates on `m_version`, not the pipeline version, so demotion is
safe there. Decide this before the chunk table freezes.

---

## 3. Espryt's consumer — `SyncRenderState`

File: `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` (10639 lines total).
`namespace RenderStateImpl` spans **1983-2688**; `void SyncRenderState(Bool forColorClear)`
is **2024-2687** (664 lines of body). `docs/Disaggregated/ROADMAP.md:47` and
`ARCHITECTURE.md:188` state the contract as "`SyncRenderState`（693 行）一行未动" — the exact count
differs by how the file-scope shadows are counted, but the contract is unambiguous: **nothing in
1983-2688 may move**. Declaration: `DirectGLES.h:225`.

### 3.1 The file-scope shadow state (1984-2017)

```
1984  static Uint16 g_syncedRenderStateVersion = 0;
1985  static Bool   g_hasSyncedRenderState = false;
1986  static RenderStateParameters g_syncedRenderStateParameters;    // the byte shadow
1987  static IntVec4 g_syncedBackendViewport   = (-1,-1,-1,-1);      // RESOLVED viewport
1990  static IntVec4 g_syncedBackendScissorBox = (-1,-1,-1,-1);      // RESOLVED scissor
1993  static Bool g_syncedSrgbFramebufferWrites = true;
2003  static Bool g_forceFullRenderStateResync = true;               // see 1994-2002
2011  static Uint32 g_syncedColorMaskAlphaWidenMask = 0;             // NOT derivable from the block, 2006-2010
2016  static Array<PerBufferBlendState, MAX_DRAW_BUFFERS> g_dualSourceDeclinedBlendStates;
2018  void InvalidateSyncedRenderState()                             // 2018-2023
```

`InvalidateSyncedRenderState` callers: `DirectGLES.cpp:3902`, `DirectGLES.cpp:10079`
(MakeCurrent), `MG_Test/Framebuffer/FramebufferTest.cpp:1171,1179`.

### 3.2 The version early-out (2028-2046)

`Uint16 currentRenderStateVersion = MGB_CTX->GetRenderStateParametersVersion();` at **2028** —
this is the **only** MGPipe accessor call on the hit path. `forceFullPush` is read and cleared at
2029-2030. `appliedWidenMask` / `colorMaskWidenDirty` 2035-2036 (these are *not* frontend state;
they come from `FramebufferImpl::g_alphaWidenedDrawBufferMask` and from `forColorClear`). The
early-out itself is 2037-2046, and it is "Gate 1 of section 2.3.1"
(`PipeStats::Gate::EsprytRenderState`, 2041-2044).

**Under a CSO scheme the server must keep this shape**: the working `RenderStateParameters` block
and a `Uint16` that increases whenever any chunk is scattered in. `MGPBindRenderState` carries
both counters (`MGPipeTypes.h:225-230`: `Version`, `PipelineVersion`), which is exactly what feeds
line 2028.

### 3.3 The three-segment memcmp (2070-2083)

```
2070  static_assert(std::is_trivially_copyable_v<RenderStateParameters>, "span memcmp/memcpy below treats the parameter block as raw bytes");
2072  constexpr SizeT kBlendSpanBegin = offsetof(RenderStateParameters, BlendStates);   // 312
2073  constexpr SizeT kBlendSpanEnd   = offsetof(RenderStateParameters, LogicOp);       // 536
2074  currentBytes = (const unsigned char*)&parameters;
2075  syncedBytes  = (const unsigned char*)&g_syncedRenderStateParameters;
2076  headSpanDirty  = !g_hasSyncedRenderState || memcmp(cur, syn, kBlendSpanBegin) != 0;
2078  blendSpanDirty = !g_hasSyncedRenderState || memcmp(cur+Begin, syn+Begin, End-Begin) != 0;
2081  tailSpanDirty  = !g_hasSyncedRenderState || memcmp(cur+End,   syn+End,   sizeof(RSP)-End) != 0;
```

Rationale comment 2058-2069 (the Blaze3D per-batch `GL_BLEND` toggle). Note 2064-2066: memcmp can
false-DIFFER on padding but never false-match, and the write-back below makes the shadow
byte-identical including padding.

The `offsetof(BlendStates) < offsetof(LogicOp)` assertion in `MGPipeValueTypes.h:543` exists
solely to keep these two constants ordered.

### 3.4 The gated blocks, in file order

| Lines | Gate | What it pushes |
|---|---|---|
| 2085-2097 | *ungated* | resolved viewport (`MGB_CTX->GetViewport()` at 2085, surface-size fallback 2086-2092, shadow compare 2093-2097) |
| 2101-2139 | `tailSpanDirty` | `SYNC_CAPABILITY` macro (2102-2109) × 11 caps (2110-2120) + scissor-test bit 0 (2131-2138) |
| 2141-2161 | `tailSpanDirty && SupportsClipDistance` | `ClipDistanceEnabledMask` → 8 `glEnable/glDisable` |
| 2163-2175 | *ungated* | `GL_FRAMEBUFFER_SRGB` — reads `MGB_CTX->IsCapabilityEnabled(FramebufferSrgb)` at **2168** (constant false today) |
| 2177-2187 | `tailSpanDirty` | primitive restart → `GL_PRIMITIVE_RESTART_FIXED_INDEX` |
| 2196-2410 | `blendSpanDirty` | the whole blend block, incl. the dual-source-decline path 2200-2264 |
| 2412-2422 | `tailSpanDirty` | `glDepthFunc`, `glDepthMask`, `glDepthRangef(DepthRanges[0])` |
| 2424-2448 | `tailSpanDirty` | stencil: `glStencilFuncSeparate` / `glStencilMaskSeparate` / `glStencilOpSeparate` × 2 faces |
| 2450-2505 | `tailSpanDirty \|\| colorMaskWidenDirty` | colour mask (broadcast vs. `glColorMaski`), widen-mask doctoring 2462-2468 |
| 2507-2515 | `tailSpanDirty` | polygon mode (NV/ANGLE ext; front value only) |
| 2517-2532 | `tailSpanDirty` | clear colour/depth/stencil + `glBlendColor` |
| 2534-2543 | `tailSpanDirty` | `glCullFace`, `glFrontFace` |
| 2545-2585 | `tailSpanDirty` | resolved scissor (`ScissorBoxWrittenMask` bit 0 → surface fallback 2571-2577; shadow compare 2580-2584). The long comment 2545-2569 is the `scissor_zero_dimension` history |
| 2587-2596 | `tailSpanDirty` | `glLogicOp` (null-checked) |
| 2598-2603 | `headSpanDirty` | `glPolygonOffset` |
| 2605-2609 | `headSpanDirty` | `glLineWidth` |
| 2611-2617 | `headSpanDirty` | `glPointSize` (null-checked) |
| 2619-2625 | `tailSpanDirty` | `glSampleCoverage` |
| 2627-2632 | `tailSpanDirty` | `glSampleMaski` |
| 2634-2655 | `tailSpanDirty` | sample shading enable + `glMinSampleShading` |

`SYNC_CAPABILITY`'s 11 GL caps (2110-2120): `DepthTest`, `ColorLogicOp`, `Dither`, `Multisample`,
`SampleAlphaToCoverage`, `SampleCoverage`, `SampleMask`, `PolygonOffsetFill`, `RasterizerDiscard`,
`StencilTest`, `CullFace`. Note `SampleAlphaToOne` is **absent**.

### 3.5 The write-back (2657-2686)

```
2657  g_syncedRenderStateVersion = currentRenderStateVersion;
2662  syncedBytesMut = (unsigned char*)&g_syncedRenderStateParameters;
2663  if (headSpanDirty)  memcpy(syncedBytesMut,          currentBytes,          kBlendSpanBegin);
2666  if (blendSpanDirty) memcpy(syncedBytesMut+Begin,    currentBytes+Begin,    End-Begin);
2677     …then re-stamp the dual-source-DECLINED draw buffers from g_dualSourceDeclinedBlendStates (2677-2680)
2682  if (tailSpanDirty)  memcpy(syncedBytesMut+End,      currentBytes+End,      sizeof(RSP)-End);
2686  g_hasSyncedRenderState = true;
```

Comment 2658-2661 explains the byte copy (padding included) and 2669-2676 explains why the
declined blend states are re-stamped.

### 3.6 What must NOT move, precisely

- The `const RenderStateParameters&` binding at **2056** — the server must hand `SyncRenderState`
  a reference to a whole 1168-byte block, not chunks.
- The `Uint16` version read at **2028** and the early-out at **2037-2046**.
- The two `offsetof` constants at **2072-2073** and the three memcmps at **2076-2083**.
- The memcpy write-back at **2662-2685**.
- The two *resolved* shadows (`g_syncedBackendViewport` 1987, `g_syncedBackendScissorBox` 1990) —
  those are backend-derived values, unrelated to the CSO split.

The Espryt head/blend/tail split and the pipeline/dynamic split are **orthogonal** and coexist —
`ARCHITECTURE.md:188` states this explicitly. In particular a pipeline chunk (`BlendStates`,
`ColorMasks`, `StencilStates`) and a dynamic chunk (`ScissorBoxes`, `ClearColor`,
`StencilStates.Ref`) both land inside Espryt's **tail** span, and `StencilStates` lands in the
tail for both halves.

### 3.7 `SyncRenderState` call sites (Espryt)

`DirectGLES.cpp` 2996, 4123 (`forColorClear = mask & GL_COLOR_BUFFER_BIT`), 6031, 6066, 6693,
6787, 7380, 7426, 7439, 7455, 7471, 7491, 7508, 7528; plus
`MG_Test/Framebuffer/FramebufferTest.cpp` 1238, 1271, 1283, 1323, 1356.

---

## 4. Magma's consumer

File: `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp` unless noted.

### 4.1 `ComputePipelineStateHash` — 4821-4910

Signature `(Uint32 colorAttachmentCount, VkSampleCountFlagBits rasterizationSamples) const`;
declaration `VulkanRenderer.h:849-850`. Mixer `CombinePipelineStateWord` at **4776-4778**
(boost-style, seed `0x243F6A8885A308D3` at 4848). Enumeration comment **4780-4794** — this is the
list `scripts/gen_pipe.py:75-99` was copied from, and it names itself as the contract
("any new GL-state read there must be added here").

One bulk fetch at **4829** (`const RenderStateParameters& p = MGB_CTX->GetRenderStateParameters();`,
rationale 4823-4828 — replaced ~17 accessor calls).

Fields hashed, in order:

| Lines | Field(s) |
|---|---|
| 4831-4840 | `capabilityBits` bits 0-9: `CullFaceEnabled`, `DepthTestEnabled`, `PolygonOffsetFillEnabled`, `RasterizerDiscardEnabled`, `ColorLogicOpEnabled`, `StencilTestEnabled`, `PrimitiveRestartEnabled`, `PrimitiveRestartFixedIndexEnabled`, `DepthMask`, `SampleShadingEnabled` |
| 4846-4847 | bit 10 = **effective** sample-mask enable, via `ResolveEffectiveSampleMask` |
| 4852-4856 | `MinSampleShadingValue`, hashed by **bits** (`memcpy`), not value |
| 4863 | `ResolveEffectiveSampleMask(...)` word (i.e. `SampleMaskValue` gated) |
| 4864 | `PatchVertices` |
| 4871-4878 | `PatchDefaultOuterLevel[0..3]`, `PatchDefaultInnerLevel[0..1]`, all `bit_cast` |
| 4879 | `PolygonModeFront` |
| 4880 | `CullFaceModeSetting` |
| 4881 | `DepthFunc` |
| 4882 | `LogicOp` |
| 4885-4891 | `StencilStates[0]` then `[1]`: `FailOp \| PassDepthPassOp<<16 \| PassDepthFailOp<<32 \| Func<<48` — **`Ref`/`ValueMask`/`WriteMask` deliberately excluded** |
| 4895-4908 | per draw buffer `i < colorAttachmentCount`: `BlendStates[i].Enabled`, `ColorMasks[independentBlend ? i : 0]` rgba, then the four factors and two equations |

`ResolveEffectiveSampleMask` is **4813-4819** and reads
`IsCapabilityEnabled(Multisample)` (4816), `IsCapabilityEnabled(SampleMask)` (4817) and
`GetRenderStateParameters().SampleMaskValue` (4818). Comment 4795-4812 explains the GL-vs-Vulkan
rule; both the hash and the payload go through it so they cannot disagree (4811-4812).

**Not hashed but pipeline-relevant** (already flagged in `PipeSpanTable.inc:27-29`):
`ProvokingVertexModeSetting`, `FrontFaceModeSetting`, `ClipOrigin`, `ClipDepthMode`.

### 4.2 `GetOrCreatePipeline` — 4951 onward; the memo

- Signature 4951-4958: `(mode, program, programObj, transformFlags, vao, renderPassEntry, primitiveRestartEnable)`.
- `vertexLayoutHash` 4976-4977, `renderPassHash` 4978.
- **Version → hash gate** 4985-4995: `MGB_CTX->GetPipelineStateVersion()` at **4985**, then the
  cached-hash key `(version, colorAttachmentCount, sampleCount)` (4986-4994). Comment 4979-4984 is
  the "glViewport must not evict a good VkPipeline" argument.
- **Memo probe** 4997-5013: linear scan comparing `mode`, `programHash`, `vertexInputHash`,
  `renderPassHash`, `pipelineStateHash`, `primitiveRestartEnable`, `transformFlags`. Gate 5 stats
  at 5005-5009.
- **Memo storage**: `VulkanRenderer.h:818-841` — `struct PipelineMemoEntry` (818-837),
  `kPipelineMemoSize = 8` (838), `m_pipelineMemo[8]` (839), `m_pipelineMemoCount` (840),
  `m_pipelineMemoNext` (841, round-robin). Cached hash: `m_pipelineStateHashVersion` (855),
  `m_pipelineStateHashColorCount` (856), `m_pipelineStateHashSampleCount` (860),
  `m_pipelineStateHash` (861), `m_pipelineStateHashValid` (862).
  `InvalidatePipelineMemo()` at 881-885 clears all three.
  Insertion at `VulkanRenderer.cpp:5665-5675`.
- Payload build reads (all `MGB_CTX->`): capabilities 5173-5182 (`CullFace`, `DepthTest`,
  `PolygonOffsetFill`+`DrawModeUsesPolygonFill`, `RasterizerDiscard`, `ColorLogicOp`+
  `m_logicOpFeatureEnabled`, `StencilTest`); FBO depth/stencil gating 5186-5199; stencil faces
  5200-5201; `GetPolygonModeFront()` 5203; the payload initializer 5276-5332 with
  `IsCapabilityEnabled(SampleShading)` 5288, `GetMinSampleShadingValue()` 5289,
  `ResolveEffectiveSampleMask` 5291, `GetPatchVertices()` 5295, `GetCullFaceMode()` 5299,
  **`.frontFace = VK_FRONT_FACE_CLOCKWISE` hardcoded at 5301**, `GetDepthMask()` 5313,
  `GetDepthFunc()` 5318, `GetLogicOp()` 5319; the tessellation defaults 5355-5356; the per-colour-
  attachment blend/mask loop 5429-5448 (`GetBlendFuncIndexed` 5436, `GetBlendEquationIndexed` 5437,
  `IsCapabilityEnabledIndexed(Blend, i)` 5438, `GetColorMaskIndexed` 5443). PipeStats constant of
  15 reads at 5258-5275 (with its own inventory).
- `ResolvePrimitiveRestartEnable` 4936-4949 — bulk fetch at 4941, reads
  `PrimitiveRestartFixedIndexEnabled`, `PrimitiveRestartEnabled`, `PrimitiveRestartIndex`. It is a
  **draw** input, not a CSO input (the answer depends on whether the draw is indexed and on the
  index type; see `VulkanRenderer.h:830-835`).
- `PipelineFactory::GetOrCreatePipeline` (the second, hash-keyed level) is
  `MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp:252-…`, declared
  `PipelineFactory.h:132`; frame-boundary bookkeeping at `PipelineFactory.h:184`.

### 4.3 `ApplyDynamicDrawStateTail` — 5902-6004, and its shadow

`struct DynamicStateShadow` is the anonymous-namespace type at `VulkanRenderer.cpp:300-397`,
instance `static DynamicStateShadow g_dynamicStateShadow;` at **398**.

- Per-state shadows 300-317 (viewport, scissor, blend constants, depth bias, line width, the six
  stencil words).
- Whole-tail gate fields 328-332: `dynamicTailValid`, `dynamicTailParamsVersion`,
  `dynamicTailExtentX/Y`, `dynamicTailIsDefaultFbo`. Rationale 318-327.
- **`struct DynamicTailKey` 355-395** with `operator==` 372-394. This is the ready-made shape of
  the dynamic chunk set: `viewport[4]`, `depthRange[2]`, `blendColor[4]`, `polygonOffsetFactor`,
  `polygonOffsetUnits`, `lineWidth`, `stencilValueMask[2]`, `stencilWriteMask[2]`, `stencilRef[2]`,
  `scissorBox[4]`, `extentX`, `extentY`, `preTransform`, `scissorEnabled`, `isDefaultFbo`.
- **The complete input inventory is spelled out at 341-354**, one line per reader:
  `ApplyGLViewportState` → `Viewports[0]`, `DepthRanges[0]`; `ApplyBlendConstants` → `BlendColor`;
  `ApplyPolygonOffsetState` → `PolygonOffsetUnits`, `PolygonOffsetFactor`; `ApplyLineWidthState`
  → `LineWidth`; `ApplyStencilState` → `StencilStates[0..1].{ValueMask, WriteMask, Ref}`;
  scissor rect → `ScissorTestEnabledMask` bit 0, `ScissorBoxes[0]`. The caveat at 351-354:
  `ApplyLineWidthState` also clamps against device limits, which are not in the key.

Function body:
- multi-viewport arm 5905-5916 (delegates to `ApplyMultiViewportDynamicState` 5878-5900, which
  invalidates the single-element shadow at 5896-5899).
- **version gate** 5920-5930: `MGB_CTX->GetRenderStateParametersVersion()` at **5920**, compared
  with extent + `isDefaultFbo`; Gate 6 stats 5925-5928.
- **value gate**: one bulk fetch at **5944**, key built 5945-5976, compared 5977-5982 (a hit
  re-arms the cheap version gate at 5980).
- the six applies 5983-5997, shadow update 5998-6003.

The five `Apply*` helpers (each with its own per-state shadow early-out):
`ComputeGLViewport` 451-505 (`GetViewportIndexed(index)` 464, `GetDepthRangeIndexed(index)` 469),
`ApplyGLViewportState` 507-521, `ApplyBlendConstants` 523-543 (`GetBlendColor()` 524),
`ApplyPolygonOffsetState` 556-568 (`GetPolygonOffsetUnits()` 557, `GetPolygonOffsetFactor()` 558),
`ApplyLineWidthState` 570-589 (`GetLineWidth()` 571, device clamp 572-581),
`ApplyStencilState` 649-677 (`GetStencilState(Front/Back)` 650-651 → six `vkCmdSetStencil*`
671-676). Scissor helpers: `MakeClampedScissorRect` 591-604,
`MapScissorRectToDefaultFramebuffer` 609-617, `MakeDefaultFramebufferScissorRect` 619-647,
`ComputeGLScissorRect` (…-5869, reads `ScissorTestEnabledMask` 5860 and `ScissorBoxes[index]` 5866).

The declared Vulkan dynamic states are exactly eight:
`MobileGL/MG_Backend/DirectVulkan/Renderer/PipelineFactory.cpp:390-397` — `VIEWPORT`, `SCISSOR`,
`BLEND_CONSTANTS`, `DEPTH_BIAS`, `LINE_WIDTH`, `STENCIL_COMPARE_MASK`, `STENCIL_WRITE_MASK`,
`STENCIL_REFERENCE`. Note `PipelineFactory.cpp:485` warns against adding
`VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE`.

### 4.4 Magma facts worth flagging

- **`FrontFaceModeSetting` is never read by Magma.** `.frontFace` is hardcoded
  `VK_FRONT_FACE_CLOCKWISE` at 5301 (and 4427, 4538); a repo-wide grep for `GetFrontFaceMode` /
  `FrontFaceModeSetting` finds only `DirectGLES.cpp:2539-2542`, `GL_Getter.cpp:1632`,
  `Core.cpp:1143-1144`, `Core.h:296` and `RenderState.{h,cpp}`. The winding is folded into
  `ConvertCullFaceModeToVkEnum(mode, invertClockwise)` at 5299 instead. The claim in
  `PipeSpanTable.inc:28-29` that `FrontFaceModeSetting` is "handled elsewhere in the payload path"
  is **not backed by a read site** — verify before relying on it.
- Magma implements **no** `alphaToCoverageEnable` / `alphaToOneEnable` / `depthClampEnable`
  (grep across `PipelineFactory.{h,cpp}` and `VulkanRenderer.cpp` is empty). So
  `SampleAlphaToCoverageEnabled` is Espryt-only and `SampleAlphaToOneEnabled` is dead everywhere.

### 4.5 Other Magma readers of the version / block

- `TrySetupDrawFastPath` (SetupDrawSnapshot gate): `GetPipelineStateVersion()` at **6122**, then a
  bulk fetch at **6131** to check only `DepthTestEnabled || StencilTestEnabled` against
  `snap.drawUsesDepthStencil` (6131-6135); the cached-hash refresh repeats at 6335-6343 and the
  memo scan at 6347-6353. Snapshot write at **6973** (`snap.renderStateVersion = MGB_CTX->GetPipelineStateVersion()`).
- `VulkanRenderer.cpp:4061` — index-rewrite path, bulk fetch for
  `PrimitiveRestartEnabled`/`FixedIndex`/`PrimitiveRestartIndex`.
- `VulkanRenderer.cpp:12019` — multi-draw merge granularity, same two restart bits.
- Clear path 7269-7271 (`GetClearColor/Depth/Stencil`), 7207/7210/8621 (`GetScissorBox`),
  7301/7390/7411/7527/7584 (`GetColorMaskIndexed`), 7328/7370/7513/7602 (`GetDepthMask`),
  7341/7374/7515/7610 (`GetStencilState`), 7258/7440 (`RasterizerDiscard`),
  7278/7482/8620 (`ScissorTest`), 6831-6832 (`DepthTest`/`StencilTest`), 3449-3450 (restart caps),
  2772 (`GetClampReadColor`), 12768 (`GetProvokingVertexMode`).

---

## 5. How both backends read render state through the P1 accessors

The switch: `MobileGL/MG_Pipe/PipeInputsSwitch.h:19-26`. Push arm `MGB_CTX` =
`&MobileGL::MG_Pipe::gPipeInputs` (`:19`); pull arm = `MG_State::pGLContext` (`:24`).
The push-arm struct is `MobileGL/MG_Backend/MGPipe/PipeInputs.h`, the global at `:682`.

Accessor → storage-field table in `PipeInputs.h:84-126` (`X(Accessor, member)` rows). The
render-state ones: `GetClearColor`→`m_clearColor` (84), `GetColorMaskIndexed`→`m_colorMask` (87),
`GetDepthRangeIndexed`→`m_depthRange` (92), `GetPipelineStateVersion`→`m_pipelineStateVersion`
(102), `GetPixelStoreParameters`→`m_pixelStore` (103), `GetRenderStateParameters`→`m_renderState`
(111), `GetRenderStateParametersVersion`→`m_renderStateParametersVersion` (112),
`GetScissorBox`→`m_scissorBox` (114), `GetStencilState`→`m_stencil` (115),
`GetViewport`→`m_viewport` (123), `GetViewportIndexed`→`m_viewportIndexed` (124),
`IsCapabilityEnabled`→`m_capability` (125), `IsCapabilityEnabledIndexed`→`m_capabilityIndexed` (126).

Accessor bodies (each `MGP_INPUT_CHECK` + `MGP_INPUT_VERIFY_READ` then a plain field read):
`GetClearColor` 249-253, `GetClearDepth` 254-258, `GetClearStencil` 259-…,
`GetColorMaskIndexed` 264-…, `GetDepthRangeIndexed` 294-…, `GetPipelineStateVersion` 338-342,
`GetRenderStateParametersVersion` 343-347, `GetPixelStoreParameters` 348-…,
`GetRenderStateParameters` 378-…, `GetScissorBox` 388-…, `GetStencilState` 393-…,
`GetViewport` 428-432, `GetViewportIndexed` 433-441, `IsCapabilityEnabled` 442-447,
`IsCapabilityEnabledIndexed` 452-…, `GetBlendColor` 204-208, `GetBlendEquationIndexed` 209-218,
`GetBlendFuncIndexed` 219-231, `GetClampReadColor` 244-248.
Constants: `kCapabilityCount` 173, `kMaxViewports` 174, `kStencilFaceCount` 176,
`struct IndexedCapabilities { Bool Blend[8]; Bool ScissorTest[16]; }` 180-183.

Fill side: `MobileGL/MG_Impl/Pipe/PipeFill.cpp` — `GetClearColor` 90-91, `GetClearDepth` 93-94,
`GetClearStencil` 96-97, `GetColorMaskIndexed` 99-101 (loop), `GetDepthRangeIndexed` 118-120
(loop over 16), `GetRenderStateParameters` 183-184 (whole-block copy),
`GetRenderStateParametersVersion` 186-187, `GetScissorBox` 192-193,
`GetStencilState` 195-197 (both faces), `GetViewport` 221-222, `GetViewportIndexed` 224-227,
`IsCapabilityEnabled` 229-234 (all caps incl. the constant-false FramebufferSrgb, note at 230),
`IsCapabilityEnabledIndexed` 236-243 (Blend ×8, ScissorTest ×16). Cost note at 448.

Verb fill points: `MobileGL/MG_Pipe/FillPoints.def` — `GetRenderStateParameters` +
`GetRenderStateParametersVersion` are filled for `kDraw` (128-129), `kClear` (184-185),
`kBlitOrCopy` (208-209), `kReadback` (257-258), `kProgramOp` (280-281).

### 5.1 Complete backend call-site inventory (`grep MGB_CTX-><accessor>` under `MG_Backend/`)

| Accessor | n | Sites |
|---|---|---|
| `GetRenderStateParameters` | 11 | GLES 2056, 3828, 4158; VK 4061, 4818, 4829, 4941, 5859, 5944, 6131, 12019 |
| `GetRenderStateParametersVersion` | 2 | GLES 2028; VK 5920 |
| `GetPipelineStateVersion` | 3 | VK 4985, 6122, 6973 |
| `IsCapabilityEnabled` | 29 | GLES 2168, 3824, 4398, 4399; `DirectGLES/MultiDraw.cpp` 52, 53; `VkClearManager.cpp` 58; `VkRenderPassManager.cpp` 614, 966, 1112; `VkTextureManager.cpp` 953; VK 3449, 3450, 4816, 4817, 5173, 5174, 5176, 5179, 5181, 5182, 5288, 6831, 6832, 7258, 7278, 7440, 7482, 8620 |
| `IsCapabilityEnabledIndexed` | 1 | VK 5438 |
| `GetViewport` | 1 | GLES 2085 |
| `GetViewportIndexed` | 1 | VK 464 |
| `GetScissorBox` | 3 | VK 7207, 7210, 8621 |
| `GetDepthRangeIndexed` | 1 | VK 469 |
| `GetStencilState` | 8 | VK 650, 651, 5200, 5201, 7341, 7374, 7515, 7610 |
| `GetColorMaskIndexed` | 6 | VK 5443, 7301, 7390, 7411, 7527, 7584 |
| `GetClearColor` / `GetClearDepth` / `GetClearStencil` | 1 each | VK 7269 / 7270 / 7271 |
| `GetBlendColor` | 1 | VK 524 |
| `GetBlendFuncIndexed` / `GetBlendEquationIndexed` | 1 each | VK 5436 / 5437 |
| `GetLogicOp` / `GetDepthFunc` | 1 each | VK 5319 / 5318 |
| `GetDepthMask` | 5 | VK 5313, 7328, 7370, 7513, 7602 |
| `GetCullFaceMode` | 1 | VK 5299 |
| `GetProvokingVertexMode` | 1 | VK 12768 |
| `GetLineWidth` | 1 | VK 571 |
| `GetPatchVertices` | 3 | GLES 2844; `DirectGLES/Managers.cpp` 7194; VK 5295 |
| `GetPatchDefaultOuterLevel` | 3 | GLES 2846; `Managers.cpp` 7202; VK 5355 |
| `GetPatchDefaultInnerLevel` | 3 | GLES 2848; `Managers.cpp` 7205; VK 5356 |
| `GetPolygonOffsetFactor` / `GetPolygonOffsetUnits` | 1 each | VK 558 / 557 |
| `GetPolygonModeFront` | 1 | VK 5203 |
| `GetMinSampleShadingValue` | 1 | VK 5289 |
| `GetPrimitiveRestartIndex` | 3 | GLES 4404, 4444; `MultiDraw.cpp` 46 |
| `GetClampReadColor` | 1 | VK 2772 |
| `GetPixelStoreParameters` | 6 | GLES 6187, 7556, 9043, 9422; `DirectGLES/Utils.cpp` 2302; VK 10690 |

Zero backend call sites: `GetClipOrigin`, `GetClipDepthMode`, `GetHint`, `GetPointSize`,
`GetPointFadeThresholdSize`, `GetPointSpriteCoordOrigin`, `GetPolygonOffsetClamp`,
`GetPolygonModeBack`, `GetFrontFaceMode`, `GetSampleCoverageValue`, `GetSampleCoverageInvert`,
`GetSampleMaskValue` — all of these are reached (when at all) only through the bulk
`GetRenderStateParameters()` field reads, or not at all.

---

## 6. What already exists for the CSO / spans

### 6.1 The member list (G7) — the only thing P0 pinned

`MobileGL/MG_Pipe/generated/PipeSpanTable.inc` (68 lines, generated).
- Header comment 9-32, including the D-B1 history at 15-20 and the "deliberately absent" list at
  22-29 (FramebufferSrgb/DepthClamp storage hole; ProvokingVertexModeSetting;
  FrontFaceModeSetting/ClipOrigin/ClipDepthMode).
- `kMGPipePipelineStateMembers[]` **34-59**, 24 names:
  `CullFaceEnabled`, `DepthTestEnabled`, `PolygonOffsetFillEnabled`, `RasterizerDiscardEnabled`,
  `ColorLogicOpEnabled`, `StencilTestEnabled`, `PrimitiveRestartEnabled`,
  `PrimitiveRestartFixedIndexEnabled`, `DepthMask`, `SampleShadingEnabled`, `MultisampleEnabled`,
  `SampleMaskEnabled`, `SampleMaskValue`, `MinSampleShadingValue`, `PatchVertices`,
  `PatchDefaultOuterLevel`, `PatchDefaultInnerLevel`, `PolygonModeFront`, `CullFaceModeSetting`,
  `DepthFunc`, `LogicOp`, `StencilStates`, `BlendStates`, `ColorMasks`.
- `kMGPipePipelineStateMemberCount = 24` at 60, self-check 61-62.
- **`extern const MGPStateChunk kMGPipePipelineChunks[];` 66 and
  `extern const MGPStateChunk kMGPipeDynamicChunks[];` 67 — declared, NEVER DEFINED.** No
  `MGPipeRenderStateSpans.cpp` exists in the tree (grep confirms: only the forward declaration
  `struct MGPipeRenderStateSpans;` at `MG_Pipe/MGPipe.h:61`, and the comments at
  `MGPipe.h:57-60`, `MGPipeTypes.h:83-85`).
- Generator: `scripts/gen_pipe.py` — `PIPELINE_STATE_MEMBERS` list at **75-99**, provenance comment
  **65-74** (copied from `VulkanRenderer.cpp:4805-4906` at `dev@81b17c0b`, explicitly refusing to
  compute offsets in python), emitter `gen_span_table()` **1040-1072**, written at **1166**.
- Only current consumer: `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:354-358`
  (`TEST(PipeCatalogue, PipelineSubsetMembersArePinned)` — asserts count 24, first `CullFaceEnabled`,
  last `ColorMasks`). **The G7 setter-consistency test does not exist yet.**

### 6.2 Wire types (all defined, none used yet)

`MobileGL/MG_Pipe/MGPipeTypes.h`:
- `struct MGPStateChunk { Uint16 Offset; Uint16 Length; }` **86-89**, `MGP_ASSERT_POD(…, 4)` 90;
  comment 83-85 states the split "is defined exactly once, in `MGPipeRenderStateSpans`".
- `struct MGPRenderStateDesc { MGPipeHandle Cso; MGPipeHandle BaseCso; Uint32 ChunkMask; Uint32 Pad0; MGPBlobRef Blob; }`
  **215-221**, size 48 (222). Comment 213-214: carries ONLY the pipeline subset's chunk bytes;
  `ChunkMask` allows an incremental create against `BaseCso`.
- `struct MGPBindRenderState { MGPipeHandle Cso; Uint16 Version; Uint16 PipelineVersion; }`
  **225-229**, size 12 (230). Comment 224: "Steady state: 12 bytes on the wire, no hashing, no blob."
- `struct MGPDynamicState { Uint32 ChunkMask; Uint16 Version; Uint16 Pad0; MGPBlobRef Blob; }`
  **236-241**, size 32 (242). Comment **232-235** enumerates the dynamic half verbatim: "viewport,
  scissor, depth range, blend colour, line width, polygon offset, stencil ref/write mask, clear
  values, sample coverage, hints and the point-size family".

Note the discrepancy: that comment puts **sample coverage** and **hints** in the dynamic half,
while `SetSampleCoverage` bumps the pipeline version (§2.5 item 1). One of the two has to change.

### 6.3 Calls and handles

`MobileGL/MG_Pipe/PipeCalls.def`:
```
 92  X(CreateRenderState,  MGPRenderStateDesc,  kCtxCso,   kHasBlob)
 93  X(BindRenderState,    MGPBindRenderState,  kCtxCso,   kNone)
 94  X(DeleteRenderState,  MGPHandleOnly,       kCtxCso,   kNone)
106  X(SetDynamicState,    MGPDynamicState,     kCtxState, kHasBlob)
```
Class taxonomy comment 15-50 (`kCtxCso` = 13 create/bind/delete entries).
`MGPipeHandleKind::RenderStateCso` at `MobileGL/MG_Pipe/MGPipeHandles.h:31` (alongside
`VertexElementsCso` 32, `SamplerCso` 33, `SamplerViewCso` 34, `ShaderCso` 35).
Call-class enum `kCtxCso` at `MGPipe.h:31`.

Field lists for the comparators: `PipeFields.def:62-63` (`MGP_FIELDS_MGPRenderStateDesc`),
`:65-66` (`MGP_FIELDS_MGPBindRenderState`); both are in `MGP_VERIFY_PAYLOAD_LIST` at
`PipeFields.def:298-299`.

### 6.4 The residual carrier that the CSO retires

`MGPipeTypes.h:516-525` — `struct ResidualValueBlock` whose **first member is
`RenderStateParameters RenderState;` (517) with the comment "until create/bind_render_state +
set_dynamic_state land"**, plus `PixelStoreParameters Pack` (518), `CapabilityBits` (519),
`PatchVertices` (520), `PatchOuter[4]` (522), `PatchInner[2]` (523).
`#define MGL_RESIDUAL_BLOCK_SIZE 1248` at **536** with the ratchet comment 527-535 (only ever goes
DOWN; P13 asserts 0). Carrier call `X(SetResidualValueState, MGPResidualValueState, kCtxState,
kHasBlob)` at `PipeCalls.def:126`; field list `PipeFields.def:147-148`.
**Landing the render-state CSO must delete `RenderState` from this struct and lower 1248 by 1168 →
80** (and the `PixelStoreParameters Pack` removal is `set_pixel_pack_state`, separate).
Test that will break: `MG_Test/Pipe/PipeCatalogueTest.cpp:111-112`.

### 6.5 Coverage map (which call answers which read)

`MobileGL/MG_Pipe/Coverage.def` already routes every render-state accessor:
- → `CreateRenderState`: `GetBlendEquationIndexed` (32), `GetBlendFuncIndexed` (33),
  `GetColorMaskIndexed` (50), `GetCullFaceMode` (51), `GetDepthFunc` (53), `GetDepthMask` (54),
  `GetLogicOp` (59), `GetMinSampleShadingValue` (61), `GetPolygonModeFront` (67),
  `GetProvokingVertexMode` (77, with the "must answer for it before it freezes" note at 74-76),
  `GetRenderStateParameters` (78), `GetStencilState` (82), `IsCapabilityEnabled` (93),
  `IsCapabilityEnabledIndexed` (94).
- → `BindRenderState`: `GetPipelineStateVersion` (65), `GetRenderStateParametersVersion` (79).
- → `SetDynamicState`: `GetBlendColor` (31), `GetClampReadColor` (46), `GetClearColor` (47),
  `GetClearDepth` (48), `GetClearStencil` (49), `GetDepthRangeIndexed` (55), `GetLineWidth` (58),
  `GetPolygonOffsetFactor` (68), `GetPolygonOffsetUnits` (69), `GetScissorBox` (80),
  `GetViewport` (91), `GetViewportIndexed` (92).
- → `SetPatchState`: `GetPatchDefaultInnerLevel` (62), `GetPatchDefaultOuterLevel` (63),
  `GetPatchVertices` (64) — **note: the patch trio is a separate call, not part of the render-state
  CSO, even though all three ARE in `kMGPipePipelineStateMembers`.** That is a live inconsistency
  the chunk table must resolve.
- → `SetPixelPackState`: `GetPixelStoreParameters` (66). → `DrawVbo`: `GetPrimitiveRestartIndex` (70).

### 6.6 Missing entirely

Grep across `MobileGL/`, `scripts/`, `docs/` finds **no** `CsoCache`, no
`ComputePipelineSubsetHash` / `MGPipeComputePipelineSubsetHash`, no
`MGPipeRenderStateSpans.{h,cpp}`, and no implementation of `kMGPipePipelineChunks` /
`kMGPipeDynamicChunks`. `CsoCache` (64 entries, keyed on the pipeline subset) and
`MGPipeComputePipelineSubsetHash()` are named only in the plan:
`docs/Disaggregated/ROADMAP.md:18` (P2 row) and `docs/Disaggregated/ARCHITECTURE.md:186`.
`MobileGL/Config.h:324` mentions "client-side content addressing of CSOs, which is the negative
control the CSO …" — the A/B switch the ROADMAP asks for.

### 6.7 The plan's own design constraints (for reference, `docs/Disaggregated/ARCHITECTURE.md:183-190`)

- Whole-block, not three CSOs: `RenderStateParameters` is a trivially-copyable POD whose **field
  order is load-bearing** (`ScissorBoxWrittenMask`, `ClipDistanceEnabledMask` are deliberately in
  the tail); splitting into blend/depth-stencil/rasterizer CSOs would need a hand-maintained
  ~150-field table with no trip wire (`:183`).
- Subset identity, not whole-block content addressing: hashing the whole block would make
  `glViewport`/`glScissor`/`glBlendColor`/`glClearColor` mint a new CSO every call and flush the
  server's pipeline memo — the regression `RenderState.h:165-170` records (`:184`).
- Dynamic subset enumerated at `:185`.
- The split lives in exactly one place: `MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` chunk table +
  `MGPipeComputePipelineSubsetHash()`, shared by client and both backends; G7 walks every public
  `RenderState` setter (`:186`).
- Server side: one working `RenderStateParameters` (1168 B) per context; `bind` and
  `set_dynamic_state` each scatter their chunks into it. Espryt gets an unchanged
  `const RenderStateParameters&`; Magma's pipeline memo is rekeyed on `cso.slot` and the dynamic
  tail still goes through `ApplyDynamicDrawStateTail` (`:188`).
- Client fetch order (`:187`): pipeline version unchanged → reuse the last CSO handle, zero hashing;
  changed → xxHash the pipeline subset (~25-30 words, what Magma already hashes) → CSO map probe →
  hit sends a 12-byte bind, miss sends a create of the changed chunks then a bind; `m_version`
  changed but subset unchanged → `set_dynamic_state` only (~200 B).

---

## 7. Open questions the implementers must answer before the chunk table freezes

1. **The six-way G7 invariant violation** in §2.5. Either grow the pipeline subset beyond Magma's
   24 hashed members, or demote the offending setters to `++m_version`. `SetSampleCoverage`,
   `SetProvokingVertexMode`, `SetFrontFaceMode`, `SetCapability(ScissorTest)`, the 11 unlisted
   capabilities, and `SetPolygonMode`'s back-face half.
2. **`StencilStates` straddles the split** (pipeline: `Func`/`FailOp`/`PassDepthFailOp`/
   `PassDepthPassOp`; dynamic: `Ref`/`ValueMask`/`WriteMask`). A byte-range chunk table cannot put
   one array in both halves without sub-member chunks. `StencilFaceState` is 28 bytes laid out
   `Func(0) Ref(4) ValueMask(8) WriteMask(12) FailOp(16) PassDepthFailOp(20) PassDepthPassOp(24)`
   — so per face the pipeline half is `[0,4)` ∪ `[16,28)` and the dynamic half is `[4,16)`. Four
   chunks for the two faces, or reorder `StencilFaceState` (which changes nothing's size but does
   move Espryt's shadow bytes, which is fine — Espryt compares against its own shadow).
3. **The patch trio** is `SetPatchState` in Coverage.def but is in the pipeline member list. Pick one.
4. **`FramebufferSrgb`/`DepthClamp` storage**, and the `sizeof == 1168` /
   `MGL_RESIDUAL_BLOCK_SIZE` / `PipeFields.def` triple that must be updated with it.
5. **`ScissorTestEnabledMask` is both** — it bumps the pipeline version (for Espryt's `glEnable`)
   and is a dynamic-tail input on Magma. Under the plan's model it is dynamic, and the Espryt
   `glEnable` is served by the working block Espryt reads anyway, so demoting its version bump is
   likely correct — but check `MGPipeValueTypes.h:362-367`, which asserts the opposite.
6. **~14 members no backend reads at all** (§1.2 "neither"). They still have to live somewhere in
   the working block for `glGet`, but they need not be in either chunk set if the client answers
   their getters locally (`kClientResolved`). That would shrink the wire payload meaningfully.
