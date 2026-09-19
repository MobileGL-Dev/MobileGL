# Scout: `MobileGL/MG_Pipe/MGPipeValueTypes.h` (P0.5 value-header extraction)

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated` @ `6e0e3df3`.
All paths below are relative to that worktree root unless absolute. Every line number was opened and read.

---

## 0. What the tree already says the task is

The debt is already written down in three places; the implementer should treat these as the spec:

- `MobileGL/MG_Pipe/MGPipeTypes.h:24-33` — the in-file P0.5 debt note:
  > "two payloads reach into headers this directory is eventually forbidden to see - `MGPCaps` embeds MG_Backend's `DynamicBackendParameters`, and `ResidualValueBlock` embeds MG_State's `RenderStateParameters` and `PixelStoreParameters` … P0.5 extracts `MGPipeValueTypes.h` and both includes below go away; until then purity gate A (section 10.3) cannot be armed for this header."
- `docs/Disaggregated/ARCHITECTURE.md:260` — names the exact intended member list:
  > 同批抽取 `MG_Pipe/MGPipeValueTypes.h`（`MAX_DRAW_BUFFERS`、`PerBufferBlendState`、`StencilFaceState`、`PixelStoreParameters`、`RenderStateParameters`、`SamplerParameters`、`BorderColorForm`、`VertexAttribute`、`VertexBufferBindingPoint`），它不 include `MG_State/GLState` 任何东西
- `docs/Disaggregated/ROADMAP.md:16` — P0.5 row, 6–9 天, acceptance: 全套测试逐名不变（纯搬移）；两条闭包断言绿且人为加回一个 `MG_State` include 能变红；`nm`/`.text` 变化可逐符号归因. Blocking prerequisite for **P1 and P7**.

Note the doc list **does not** include `DynamicBackendParameters`, but `MGPipeTypes.h:140-145` says the caps block *does* move ("until P0.5 moves the caps block into MGPipeValueTypes.h with fixed-width members"). That contradiction must be resolved by the implementers — see §7.4.

---

## 1. Current definition of each type

### 1.1 `MobileGL::PixelStoreParameters`
- **Definition**: `MobileGL/MG_State/GLState/RenderState/RenderState.h:191-200`. Namespace `MobileGL` (not `MG_State::GLState`).
- Members (all fixed-width, no pointer, no `SizeT`):
  ```
  Bool SwapBytes = false;   Bool LSBFirst = false;
  Int RowLength = 0; Int ImageHeight = 0; Int SkipPixels = 0;
  Int SkipRows = 0;  Int SkipImages = 0;  Int Alignment = 4;
  ```
- Dependencies: `Bool` = `bool` (`MobileGL/MG_Util/Types.h:33`), `Int` = `Int32` = `int32_t` (`MG_Util/Types.h:27,29`). Nothing else.
- **Size asserted at 28** by `MobileGL/MG_Pipe/MGPipeTypes.h:494` (`sizeof(MGPPixelPackState) == 28`, and `MGPPixelPackState` is exactly `{ PixelStoreParameters Pack; }`, `MGPipeTypes.h:487-489`). Comment at `:491-493` states the layout is "two Bools, two bytes of padding, six Ints".
- **Trivially copyable**: yes — asserted at `MGPipeTypes.h:490` via `MGPPixelPackState`.

### 1.2 `MobileGL::PerBufferBlendState`
- **Definition**: `MG_State/GLState/RenderState/RenderState.h:202-210`.
- Members: `Bool Enabled`; `BlendFactor SrcFactorRGB/DstFactorRGB/SrcFactorAlpha/DstFactorAlpha`; `BlendEquation ColorEquation/AlphaEquation`.
- Enum deps: `BlendFactor` (`RenderState.h:15-38`, 18 enumerators incl. the four dual-source `Src1*`, plus `BlendFactorCount`, `Unknown = -1`), `BlendEquation` (`RenderState.h:40-48`). Both are plain `enum class` (underlying `int`), so 28 bytes with 3 pad bytes after `Enabled`.

### 1.3 `MobileGL::StencilFaceState`
- **Definition**: `RenderState.h:212-220`.
- Members: `DepthTestFunc Func`, `Int Ref`, `Uint32 ValueMask`, `Uint32 WriteMask`, `StencilOperation FailOp/PassDepthFailOp/PassDepthPassOp`.
- Enum deps: `DepthTestFunc` (`RenderState.h:71-82`), `StencilOperation` (`RenderState.h:84-95`).

### 1.4 `MobileGL::RenderStateParameters` — the big one
- **Definition**: `MG_State/GLState/RenderState/RenderState.h:222-370` (149 lines, heavily commented; the comments are load-bearing — several record CTS failures and must move with the struct).
- **Constant it owns**: `static constexpr Uint MAX_VIEWPORTS = 16;` at `RenderState.h:227`. 74 references tree-wide across 17 files (`MG_Backend/DirectGLES/{BackendObject_DirectGLES,DirectGLES,Managers}.cpp`, `MG_Backend/DirectVulkan/{BackendObject_DirectVulkan,Renderer/VulkanRenderer.cpp,Renderer/VulkanRenderer.h}`, `MG_Impl/GLImpl/{Getter/GL_Getter,RenderState/GL_RenderState}.cpp`, `MG_IntegrationTest/Scenarios/{AdvertisedLimits,ViewportArray}Scenario.cpp`, `MG_State/GLState/RenderState/RenderState.{h,cpp}`, `MG_Test/Backend/DirectGLES/ViewportIndexRoutingTest.cpp`, `MG_Test/BackendLoader/BackendLoaderTest.cpp`, `MG_Test/State/RenderStateTest.cpp`, `MG_Util/BackendLoaders/OpenGL/Loader.cpp`, `MG_Util/Converters/GLToStr/GLEnumConverter.cpp`). Since it is a *member* of the struct, it moves with it for free — spelling stays `RenderStateParameters::MAX_VIEWPORTS`.
- **External constant it depends on**: `MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS` (`MG_State/GLState/FramebufferState/FramebufferObject.h:106`, `static constexpr Uint MAX_DRAW_BUFFERS = 8;`), used at `RenderState.h:263` (`Array<PerBufferBlendState, ...MAX_DRAW_BUFFERS> BlendStates`) and `RenderState.h:273` (`Array<BoolVec4, ...MAX_DRAW_BUFFERS> ColorMasks`). **This single reference is what drags the whole `FramebufferObject.h` subtree into `RenderState.h`** — see §2. 80 references tree-wide across 24 files.
- Member list in declaration order (offsets matter — see §4.1):
  | # | member | type | notes |
  |---|---|---|---|
  | 1 | `Viewports` | `Array<FloatVec4, 16>` | 256 B, offset 0 |
  | 2 | `LineWidth`, `PointSize` | `Float` ×2 | |
  | 3 | `PatchVertices` | `Uint` | |
  | 4 | `PatchDefaultOuterLevel` | `FloatVec4` | hashed bitwise by Magma |
  | 5 | `PatchDefaultInnerLevel` | `FloatVec2` | |
  | 6 | `PolygonOffsetFactor/Units/Clamp` | `Float` ×3 | |
  | 7 | `ClipOrigin`, `ClipDepthMode` | `GLenum` ×2 | defaults `GL_LOWER_LEFT` / `GL_NEGATIVE_ONE_TO_ONE` |
  | 8 | `BlendStates` | `Array<PerBufferBlendState, MAX_DRAW_BUFFERS>` | **Espryt span boundary: `offsetof(...,BlendStates)`** |
  | 9 | `LogicOp` | `LogicOperation` | **Espryt span boundary: `offsetof(...,LogicOp)`** |
  | 10 | `DepthTestEnabled`, `DepthFunc`, `DepthMask` | | |
  | 11 | `ColorMasks` | `Array<BoolVec4, MAX_DRAW_BUFFERS>` | |
  | 12 | `ClearColor`, `ClearDepth`, `ClearStencil`, `BlendColor` | | |
  | 13 | `DepthRanges` | `Array<FloatVec2, 16>` | |
  | 14 | `SampleCoverageValue/Invert`, `SampleMaskValue`, `MinSampleShadingValue` | | |
  | 15 | `StencilStates` | `Array<StencilFaceState, 2>` | [0]=Front, [1]=Back |
  | 16 | `CullFaceEnabled`, `CullFaceModeSetting`, `FrontFaceModeSetting`, `ProvokingVertexModeSetting` | | |
  | 17 | 4× hint `GLenum` (`LineSmoothHint`, `PolygonSmoothHint`, `TextureCompressionHint`, `FragmentShaderDerivativeHint`) | | |
  | 18 | `PointFadeThresholdSize`, `PointSpriteCoordOrigin` | | |
  | 19 | `ClampReadColor` | `GLenum` | |
  | 20 | `PolygonModeFront/Back` | `GLenum` ×2 | |
  | 21 | `PrimitiveRestartIndex` | `Uint32` | |
  | 22 | 20 capability `Bool`s (`ColorLogicOpEnabled` … `ProgramPointSizeEnabled`) | `RenderState.h:325-344` | |
  | 23 | `ScissorTestEnabledMask` | `Uint32` | 16 bits, one per viewport |
  | 24 | `ScissorBoxes` | `Array<IntVec4, 16>` | |
  | 25 | `ScissorBoxWrittenMask`, `ClipDistanceEnabledMask` | `Uint32` ×2 | comments at `:352-369` explicitly say they are placed *after* `LogicOp` so Espryt's tail-span memcmp sees them |
- **Enums it depends on**, all defined in `RenderState.h` above the struct, all `enum class` with an `Unknown = -1` and a `...Count` sentinel:
  `BlendFactor` `:15-38` · `BlendEquation` `:40-48` · `LogicOperation` `:50-69` · `DepthTestFunc` `:71-82` · `StencilOperation` `:84-95` · `StencilFace` `:97-102` · `PixelStoreParam` `:104-127` · `CullFaceMode` `:129-135` · `FrontFaceMode` `:137-142` · `ProvokingVertexMode` `:144-149` · `CapabilityInput` `:151-189`.
  (`StencilFace`, `PixelStoreParam` and `CapabilityInput` are *not* members of any value struct — they are the setter-API vocabulary of `class RenderState` and of the four converter headers. See §7.3 for whether they move.)
- **Vector types**: `FloatVec2/FloatVec4/IntVec4/BoolVec4` from `MG_Util/Math/VectorTypes.h:213-224`, all `Vec2/Vec3/Vec4<T> : VecBase<...>` (`VectorTypes.h:16-92, 99-212`). `VecBase` has a user-provided default ctor and initializer-list ctor but **implicit copy ctor/assign/dtor**, so `is_trivially_copyable_v` is true while `is_trivially_default_constructible_v` is false. `GL_*` enum constants come from the GL headers pulled by `<Includes.h>`.
- **Trivially copyable**: yes — asserted in the backend, `MG_Backend/DirectGLES/DirectGLES.cpp:2068-2069`.
- **Size**: not directly asserted anywhere. It is pinned *indirectly* by `#define MGL_RESIDUAL_BLOCK_SIZE 1248` + `static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE)` (`MGPipeTypes.h:535-538`). Working backwards through `ResidualValueBlock`'s layout (`MGPipeTypes.h:515-524`: `RenderStateParameters` + `PixelStoreParameters`(28) + `Uint64` + `Uint32`+`Uint32` + `Float[4]` + `Float[2]` + `Uint32[2]`, block alignment 8) gives **`sizeof(RenderStateParameters) == 1168`** on the LP64/LLP64 targets — *arithmetic inference from the assertion, not a measured value; verify with a compile before relying on it.*
- Also asserted: `MG_Test/Pipe/PipeCatalogueTest.cpp:113` — `EXPECT_GE(sizeof(ResidualValueBlock), sizeof(RenderStateParameters) + sizeof(PixelStoreParameters))`, and `:110-111` re-assert `MGL_RESIDUAL_BLOCK_SIZE` at both compile and run time.

### 1.5 `MobileGL::SamplerParameters`
- **Definition**: `MG_State/GLState/SamplerState/SamplerObject.h:72-97`. Namespace `MobileGL`.
- Members: `SamplerWrapMode wrapS/wrapT/wrapR`, `SamplerFilterMode minFilter/magFilter`, `SamplerMipmapMode mipmapMode`, `Float minLod(-1000)/maxLod(1000)/lodBias/maxAnisotropy`, `SamplerCompareFunc compareFunc`, `SamplerCompareMode compareMode`, `FloatVec4 borderColor`, `IntVec4 borderColorI`, `UintVec4 borderColorUI`, `BorderColorForm borderColorForm`.
- Enum deps, all in the same header: `SamplerFilterMode` `:14-19` · `SamplerMipmapMode` `:21-27` · `SamplerWrapMode` `:29-36` · `SamplerCompareMode` `:38-43` · `SamplerCompareFunc` `:45-55` · `BorderColorForm : Uint8` `:66-70`.
- Trivially copyable: yes (all members are scalars/enums/`Vec4`), but **no static_assert exists today** anywhere in the tree.
- **No size assertion exists.** `MGPipeTypes.h:255-263` carries it as an opaque blob (`MGPSamplerDesc { MGPipeHandle Cso; MGPBlobRef Parameters; }`, `MGP_ASSERT_POD(MGPSamplerDesc, 32)`) and the comment says "Carried as a blob until P0.5 gives it a value header."

### 1.6 `MobileGL::MG_State::GLState::VertexAttribute` — **NOT a POD**
- **Definition**: `MG_State/GLState/VertexArrayState/VertexArrayObject.h:17-51`. Note the *deeper namespace*: `MobileGL::MG_State::GLState`, unlike all the others which sit directly in `MobileGL`.
- Members: `Bool Enabled`, `int Size`, `DataType Type`, `Bool Normalized`, `int Stride` (resolved, 0 is meaningful — see the CTS note at `:19-28`), `SizeT Offset`, `Bool IsInteger`, `Bool IsLong`, `Bool IsBgra`, `Uint Divisor`, **`SharedPtr<BufferObject> Buffer`**, `int LegacyStride`, `SizeT LegacyPointer`.
- **BLOCKER**: `SharedPtr<BufferObject> Buffer` (`VertexArrayObject.h:39`) makes this **not trivially copyable** and forces `#include "../BufferState/BufferObject.h"` (`VertexArrayObject.h:11`). It also uses `SizeT`, which `MGPipeTypes.h:140-143` explicitly calls out as ABI-dependent and unacceptable in a wire type.
- Enum dep: `DataType` (`MG_Util/Types.h:129-145`) — free, `Types.h` is already inside `<Includes.h>`.
- `SharedPtr` = `std::shared_ptr` (`MG_Util/Types.h:41-42`); `SizeT` = `std::size_t` (`Types.h:55`); `Array` = `std::array` (`Types.h:56-57`).

### 1.7 `MobileGL::MG_State::GLState::VertexBufferBindingPoint` — **NOT a POD**
- **Definition**: `VertexArrayObject.h:58-64`. Members: `SharedPtr<BufferObject> Buffer`, `SizeT Offset`, `int Stride = 16` (initial VERTEX_BINDING_STRIDE is 16, not 0), `Uint Divisor`. Same blocker.
- Neighbour worth knowing: `VertexAttributeVersion` (`VertexArrayObject.h:66-70`, three `Uint16`) is consumed by `MG_Backend/DirectGLES/Managers.h:788`; it is trivially copyable and is a candidate to ride along.
- The `MAX_VERTEX_ATTRIBS = 32` / `MAX_VERTEX_ATTRIB_BINDINGS = 32` constants live on `class VertexArrayObject` (`VertexArrayObject.h:76-77`), not on the value struct — 98 references tree-wide. `MGPipeTypes.h:243-245` speaks of "the resolved `VertexAttribute[32]`", so the header needs its own copy of the 32.

### 1.8 `MobileGL::MG_Backend::DynamicBackendParameters`
- **Definition**: `MG_Backend/BackendObject.h:316-549` (233 lines). Namespace `MobileGL::MG_Backend`.
- ~90 scalars. **Non-fixed-width members that block a wire type**: `SizeT UniformBufferOffsetAlignment` `:317`, `SizeT ShaderStorageBufferOffsetAlignment` `:322`, `SizeT MaxShaderStorageBlockSize` `:545`.
- **It is not a pure aggregate**: it has two member functions — `static constexpr Uint32 PerLayerFramebufferAttachmentBit(TextureTarget)` `:487-492` and `Bool SupportsPerLayerFramebufferAttachment(TextureTarget) const` `:494-497`. Both take `TextureTarget`, which is why `BackendObject.h:11` includes `MG_State/GLState/TextureState/TextureEnum.h` — **an MG_Backend header reaching into MG_State**, and the only MG_State include in that file.
- Also carries `GpuVendorKind GpuVendor` (enum defined `BackendObject.h:303-315`), `Int MaxComputeWorkGroupCount[3]` / `MaxComputeWorkGroupSize[3]` `:392-393` (the six indexed limits that MGPCaps exists to carry), two `GLenum` provoking-vertex fields `:445-446`.
- Trivially copyable: yes — asserted indirectly at `MGPipeTypes.h:139` (`is_trivially_copyable_v<MGPCaps>`, and `MGPCaps` embeds it by value at `:131`).
- Size asserted *compositionally*, not literally: `MGPipeTypes.h:144-145`, `sizeof(MGPCaps) == sizeof(DynamicBackendParameters) + 8 + 24 + 24`, with the comment at `:140-143` explaining why a literal is impossible today ("`DynamicBackendParameters` still carries `SizeT` fields, so its literal size is ABI-dependent until P0.5 moves the caps block into `MGPipeValueTypes.h` with fixed-width members").

---

## 2. Transitive include closure of each host header (what a naive move drags along)

Computed by walking `#include` lines from each header, resolving relative-then-root, over the real tree.

**Baseline — `MobileGL/Includes.h` (the umbrella every MG_Pipe header already pulls, `Includes.h:1-149`):**
- Project headers reached (5): `Defines.h`, `MG_Util/Debug/Log.h`, `MG_Util/GLExtensions.h`, `MG_Util/PlatformStubs.h`, `MG_Util/Types.h`.
- External headers reached (56): the whole libstdc++ set (`<bit> <map> <array> <ctime> <mutex> <queue> <regex> <atomic> <bitset> <cctype> <chrono> <cstdio> <format> <memory> <random> <string> <thread> <vector> <cassert> <climits> <cstdlib> <cstdarg> <cstring> <numeric> <expected> <iostream> <optional> <algorithm> <stdexcept> <functional> <string_view> <unordered_map>` `<stacktrace>`), plus `ska/flat_hash_map.hpp` (`:53`), `xxhash.h` (`:56`), **`spirv_cross/spirv_cross_c.h` (`:59`)**, `EGL/egl.h` `GL/gl.h` `GL/glcorearb.h` `GL/glext.h` `GLES3/gl32.h` (`:65-76`), **`glslang/Include/Types.h`, `glslang/Public/ShaderLang.h`, `glslang/SPIRV/GlslangToSpv.h`, `glslang/Include/intermediate.h`, `glslang/MachineIndependent/localintermediate.h` (`:80-84`)**, `vulkan/vulkan.h` (`:130`), `tracy/Tracy.hpp` (`:141`), plus platform (`dlfcn.h pthread.h unistd.h android/log.h android/native_window.h windows.h processthreadsapi.h signal.h`).

> **Critical framing for the brief**: `<Includes.h>` *already* brings glslang, SPIRV-Cross and Vulkan into every MG_Pipe header. So `MGPipeValueTypes.h` cannot be "glslang-free" while it includes `<Includes.h>`, and P7's `nm -D | grep glslang` criterion is about the *linked server binary*, not this header's textual closure. The purity gate that P0.5 actually arms is **gate A: no `MG_State/GLState` include reachable from `MG_Pipe/`**.

**Delta each host header adds on top of `Includes.h`:**

| host header | extra project headers dragged in |
|---|---|
| `MG_State/GLState/RenderState/RenderState.h` | **10**: `MG_Util/Math/VectorTypes.h`, `MG_State/GLState/FramebufferState/FramebufferObject.h`, `MG_State/GLState/RenderbufferState/RenderbufferObject.h`, `MG_State/GLState/SamplerState/SamplerObject.h`, `MG_State/GLState/TextureState/TextureObject.h`, `MG_State/GLState/TextureState/TextureEnum.h`, `MG_State/GLState/TextureState/TextureTypes.h`, `MG_State/GLState/TextureState/MipmapStorage.h`, `MG_State/GLState/TextureState/MipmapUploadTargetArray.h` |
| `MG_State/GLState/SamplerState/SamplerObject.h` | **1**: `MG_Util/Math/VectorTypes.h` |
| `MG_State/GLState/VertexArrayState/VertexArrayObject.h` | **3**: `MG_State/GLState/BufferState/BufferObject.h`, `MG_State/GLState/BufferState/PipeResource.h`, `MG_Util/Math/VectorTypes.h` (+`<new>` externally) |
| `MG_Backend/BackendObject.h` | **1**: `MG_State/GLState/TextureState/TextureEnum.h` |

Chain that produces `RenderState.h`'s big closure: `RenderState.h:12` → `FramebufferState/FramebufferObject.h:12,13` → `TextureState/TextureObject.h` and `RenderbufferState/RenderbufferObject.h` → `SamplerState/SamplerObject.h`, `TextureTypes.h`, `MipmapStorage.h`, `MipmapUploadTargetArray.h`, `TextureEnum.h`. **The only reason `RenderState.h` needs `FramebufferObject.h` at all is the two `MAX_DRAW_BUFFERS` uses at `:263` and `:273`.**

No `spirv-cross`, `glslang` or other third-party header is reached by any of these four *beyond what `Includes.h` already pulls*. There is no `SPIRV-Reflect`/`SpvcSession`/`ShaderObject` in these closures (that is the sibling `ProgramArtifacts.h` extraction, a different scout's problem).

---

## 3. Includers of each host header

Whole-tree counts (`*.h *.cpp *.hpp *.inc`, textual `#include` match):

### `MG_State/GLState/RenderState/RenderState.h` — 8 includers
| file | in |
|---|---|
| `MobileGL/MG_Pipe/MGPipeTypes.h:33` | **MG_Pipe** |
| `MobileGL/MG_State/GLState/Core.h:15` | MG_State (the umbrella; 73 files include `GLState/Core.h`) |
| `MobileGL/MG_Test/State/RenderStateTest.cpp` | test |
| `MobileGL/MG_Util/Converters/GLToMG/RenderStateEnumConverter.h:11` | MG_Util |
| `MobileGL/MG_Util/Converters/MGToGL/RenderStateEnumConverter.h:11` | MG_Util |
| `MobileGL/MG_Util/Converters/MGToStr/RenderStateEnumConverter.h:11` | MG_Util |
| `MobileGL/MG_Util/Converters/MGToVk/RenderStateEnumConverter.h:11` | MG_Util |
| `MobileGL/MG_Util/Texture/PixelStoreProcessor.h:11` | MG_Util (wants `PixelStoreParameters` only) |
- **MG_Backend includers: none directly** — both backends reach `RenderStateParameters` through `MG_State/GLState/Core.h`.

### `MG_State/GLState/SamplerState/SamplerObject.h` — 7 includers
| file | in |
|---|---|
| `MobileGL/MG_Backend/DirectGLES/DirectGLES.h` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectGLES/Managers.h` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.h:14` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp` | **MG_Backend** |
| `MobileGL/MG_Impl/GLImpl/Sampler/Validators.h` | MG_Impl |
| `MobileGL/MG_State/GLState/TextureState/TextureObject.h` | MG_State |
| `MobileGL/MG_State/GLState/TextureState/TextureUnit.h` | MG_State |
- MG_Pipe includers: none directly (reached transitively via `RenderState.h`).

### `MG_State/GLState/VertexArrayState/VertexArrayObject.h` — 5 includers
| file | in |
|---|---|
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h` | **MG_Backend** |
| `MobileGL/MG_Impl/GLImpl/VertexArray/Validators.h` | MG_Impl |
| `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.cpp` | MG_State |
| `MobileGL/MG_Test/State/ObjectLifetimeIdTest.cpp` | test |
| `MobileGL/MG_Util/SelfTest/DriverPost.cpp` | MG_Util |
- MG_Pipe includers: none.

### `MG_Backend/BackendObject.h` — 9 includers
| file | in |
|---|---|
| `MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectGLES/DirectGLES.h` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectVulkan/BackendObject_DirectVulkan.cpp` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.h` | **MG_Backend** |
| `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.h` | **MG_Backend** |
| `MobileGL/MG_IntegrationTest/Harness/BackendCapsPeek.cpp` | itest |
| `MobileGL/MG_Pipe/MGPipeTypes.h:32` | **MG_Pipe** |
| `MobileGL/MG_Util/SelfTest/DriverPost.h` | MG_Util |
| `MobileGL/MG_Util/ShaderTranspiler/CompileEnv.h` | MG_Util |

### MG_Pipe's own headers — blast radius is tiny
- `MG_Pipe/MGPipe.h` is included by exactly **2** files: itself and `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:18`.
- `MG_Pipe/MGPipeTypes.h` is included by `MGPipe.h:15`, `MGPipeCallbacks.h:13`, and itself.
- No `MG_Backend`, `MG_Impl`, `MG_State` or `MG_Remote` file includes any MG_Pipe header yet. **A change confined to MG_Pipe therefore cannot break the backends** — the risk is entirely in what the extraction does to the *MG_State* headers the backends do read.

---

## 4. How the two backends consume these types

### 4.1 Espryt (DirectGLES) — the three-segment memcmp, `MG_Backend/DirectGLES/DirectGLES.cpp`
- Shadow: `static RenderStateParameters g_syncedRenderStateParameters;` at `:1984`, guarded by `g_hasSyncedRenderState`; `SyncRenderState(Bool forColorClear)` begins `:2022`; called from `:2994` and `:4121`.
- Rationale comment `:2057-2067`; then the assertions and the spans:
  ```
  :2068  static_assert(std::is_trivially_copyable_v<RenderStateParameters>,
  :2069                "span memcmp/memcpy below treats the parameter block as raw bytes");
  :2070  constexpr SizeT kBlendSpanBegin = offsetof(RenderStateParameters, BlendStates);
  :2071  constexpr SizeT kBlendSpanEnd   = offsetof(RenderStateParameters, LogicOp);
  ```
  Three spans (`:2074-2081`): head `[0, kBlendSpanBegin)`, blend `[kBlendSpanBegin, kBlendSpanEnd)`, tail `[kBlendSpanEnd, sizeof(RenderStateParameters))`.
- Write-back at `:2660-2683`: three conditional `memcpy`s over the same three spans, plus the dual-source-declined patch at `:2676-2678` which re-writes `g_syncedRenderStateParameters.BlendStates[i]` for declined draw buffers.
- **Hard constraints this places on any move**: (a) `offsetof` requires standard-layout — the struct must stay a plain aggregate with a single access specifier and no virtual/base; (b) `BlendStates` must stay the first member of the "blend" region and `LogicOp` the first member after it — i.e. **member order is load-bearing and must not be reordered by the move**; (c) the capability `Bool`s, `ScissorTestEnabledMask`, `ScissorBoxes`, `ScissorBoxWrittenMask` and `ClipDistanceEnabledMask` must stay *after* `LogicOp` (the comments at `:352-369` of `RenderState.h` say so explicitly, citing `KHR-GL43.viewport_array.scissor_zero_dimension`); (d) padding is *deliberately copied*, so any change that alters padding changes behaviour subtly (it only costs an extra field-wise pass, but it invalidates the "steady state is exact" claim at `DirectGLES.cpp:2062-2065`).
- ~51 further per-field reads of `g_syncedRenderStateParameters` in the same function (`:2101` `SYNC_CAPABILITY` macro, `:2132`, `:2149`, `:2179-2180`, `:2196`, `:2411-2417`, `:2425`, `:2454`, `:2507`, `:2516-2537`, `:2591-2649`).
- `SamplerParameters`: Espryt keeps two per-object shadows `SamplerParameters m_cacheSamplerParameters;` at `MG_Backend/DirectGLES/Managers.h:1087` and `:1825`, diffed **field by field** (`Managers.cpp:4778-4850+`, macro at `:4793-4796`), reset with `= SamplerParameters{}` at `Managers.cpp:2911`, and read in `ResolveBackendMinFilter(const SamplerParameters&, ...)` at `Managers.cpp:90`. **No memcmp, no offsetof, no sizeof.**
- `VertexAttribute`: consumed by const-ref only — `Managers.h:744`, `Managers.cpp:2199, 2246, 2256, 2627`. `Managers.h:788` holds `Array<VertexAttributeVersion, VertexArrayObject::MAX_VERTEX_ATTRIBS>`.
- `PixelStoreParameters`: used in `DirectGLES.cpp` and `MG_Backend/DirectGLES/Utils.cpp`, by value/field, no layout assumption.

### 4.2 Magma (DirectVulkan) — `MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp`
- `Uint64 VulkanRenderer::ComputePipelineStateHash(Uint32 colorAttachmentCount, VkSampleCountFlagBits)` at `:4820-4900+`; declared `VulkanRenderer.h:849-850`. Callers at `:4989` and `:6338`.
- It takes **one bulk `const RenderStateParameters&`** (`:4828`, comment `:4822-4827` explains it replaces ~17 accessor calls) and then reads **named fields only**:
  `CullFaceEnabled, DepthTestEnabled, PolygonOffsetFillEnabled, RasterizerDiscardEnabled, ColorLogicOpEnabled, StencilTestEnabled, PrimitiveRestartEnabled, PrimitiveRestartFixedIndexEnabled, DepthMask, SampleShadingEnabled` (`:4830-4839`), `MinSampleShadingValue` (bit-cast, `:4855`), `SampleMaskValue` (via `ResolveEffectiveSampleMask`, `:4817`), `PatchVertices` (`:4867`), `PatchDefaultOuterLevel[0..3]` / `PatchDefaultInnerLevel[0..1]` (bit-cast, `:4874-4881`), `PolygonModeFront`, `CullFaceModeSetting`, `DepthFunc`, `LogicOp` (`:4883-4886`), `StencilStates[]` (`:4889-4895`, "[0] is Front, [1] is Back"), `BlendStates[i]` and `ColorMasks[...]` (`:4896-4900+`), with `MOBILEGL_ASSERT(colorAttachmentCount <= p.BlendStates.size(), ...)` at `:4892-4894`.
- Memo key `PipelineMemoEntry` (`VulkanRenderer.h:818-837`) stores only the resulting `Uint64 pipelineStateHash`; `kPipelineMemoSize = 8` at `:838`.
- **Magma makes NO `sizeof`, `offsetof`, or memcmp assumption on any of these types.** A tree-wide grep for `sizeof(RenderStateParameters)|offsetof(RenderStateParameters|sizeof(PixelStoreParameters)|sizeof(SamplerParameters)|sizeof(VertexAttribute)` finds hits only in `DirectGLES.cpp:2070,2071,2081,2682`, `MGPipeTypes.h:492` (comment) and `MG_Test/Pipe/PipeCatalogueTest.cpp:113`.
- `SamplerParameters` on Magma: `VkSamplerManager` (`MG_Backend/DirectVulkan/Renderer/VkSamplerManager.h`) never touches the struct — it takes `const MG_State::GLState::SamplerObject&` (`:45, 89, 95, 101`) and builds a `Uint64 BuildSamplerKey(...)` (`:89`) plus a `ResolvedBorderColor` (`:71-76`). It only includes `SamplerObject.h` at `:14`. So the *class*, not the value struct, is Magma's sampler input.
- `VertexAttribute` on Magma: `VertexInputStateFactory` (`.h` includes `VertexArrayObject.h`); `.cpp:124, 183, 371` read fields and `GetDynamicParameters().SupportsFloat64VertexAttributes`.

### 4.3 `PipeStats` and MGPipe size assertions mentioning these types
- No `PipeStats` reference to any of the four value types (grep of `MG_Util/PipeStats*` is empty). `PipeStats` only appears in `SyncRenderState`'s gate accounting (`DirectGLES.cpp:2040-2051`, `Gate::EsprytRenderState`, `CallClass::AccessorCalls`).
- MGPipe assertions that transitively pin these types:
  - `MGPipeTypes.h:139` `is_trivially_copyable_v<MGPCaps>` → pins `DynamicBackendParameters`.
  - `MGPipeTypes.h:144-145` `sizeof(MGPCaps) == sizeof(DynamicBackendParameters) + 8 + 24 + 24`.
  - `MGPipeTypes.h:490` `is_trivially_copyable_v<MGPPixelPackState>` and `:494-495` `sizeof(MGPPixelPackState) == 28` → pins `PixelStoreParameters`.
  - `MGPipeTypes.h:525` `is_trivially_copyable_v<ResidualValueBlock>` and `:535-538` `sizeof(...) == MGL_RESIDUAL_BLOCK_SIZE` (1248) → pins `RenderStateParameters` + `PixelStoreParameters` jointly.
  - `generated/PipeWire.inc:449-455` (`MGPWireRec_SetPixelPackState`) and `:481-487` (`MGPWireRec_SetResidualValueState`) re-assert 8-byte-aligned record sizes derived from those payloads; `:121-127` does the same for `MGPWireRec_GetCaps`.
  - `MG_Test/Pipe/PipeCatalogueTest.cpp:110-113, 120-125`.
- `generated/PipeVerify.inc:232-238` — the memcmp fallback in `MGPipeFieldEqual`, whose comment names exactly the P0.5 types:
  > "Only reached by the payload members that are still MG_State / MG_Backend value structs (`RenderStateParameters`, `PixelStoreParameters`, `DynamicBackendParameters`) and by `MGHostSpan`. Those are exactly the types P0.5 moves into `MGPipeValueTypes.h`, at which point they get field lists of their own and this branch stops being reachable from any payload."
  The generator source of that text is `scripts/gen_pipe.py:413`.
- `generated/PipeSpanTable.inc:34-67` — `kMGPipePipelineStateMembers[24]` names `RenderStateParameters` members **by string**, taken from what `ComputePipelineStateHash` hashes; `kMGPipePipelineChunks[]` / `kMGPipeDynamicChunks[]` are `extern` and will be filled by `MG_Pipe/MGPipeRenderStateSpans.cpp` in P2 using `offsetof`. **Renaming any of those 24 members breaks P2 silently** (the strings are only checked at P2 time).

---

## 5. Existing MG_Pipe header include lines

| file | include lines |
|---|---|
| `MG_Pipe/MGPipe.h` | `:10 #include <Includes.h>` · `:12 "MGPipeCallbacks.h"` · `:13 "MGPipeHandles.h"` · `:14 "MGPipeHostSpan.h"` · `:15 "MGPipeTypes.h"` · then in-namespace `.def`/`.inc` textual includes at `:65, 68, 77, 80, 83, 86, 89, 92` |
| `MG_Pipe/MGPipeCallbacks.h` | `:10 <Includes.h>` · `:12 "MGPipeHandles.h"` · `:13 "MGPipeTypes.h"` |
| `MG_Pipe/MGPipeHandles.h` | `:10 <Includes.h>` only |
| `MG_Pipe/MGPipeHostSpan.h` | `:10 <Includes.h>` only |
| `MG_Pipe/MGPipeTypes.h` | `:10 <Includes.h>` · `:12 "MGPipeHandles.h"` · `:13 "MGPipeHostSpan.h"` · **`:32 <MG_Backend/BackendObject.h>`** · **`:33 <MG_State/GLState/RenderState/RenderState.h>`** (the two P0.5 debts) |
| `MG_Pipe/generated/PipeVerify.inc` | `:25 "../PipeFields.def"` |

`MGPipeTypes.h:35-40` opens `namespace MobileGL::MG_Pipe` and pulls the three names in with `using`:
```
:36    using MG_Backend::DynamicBackendParameters;
:39    using MobileGL::PixelStoreParameters;
:40    using MobileGL::RenderStateParameters;
```
with the comment at `:37-38`: "Both live directly in namespace `MobileGL` today; P0.5 moves them into `MG_Pipe/MGPipeValueTypes.h`."

**What `<Includes.h>` brings**: see §2 baseline. Practically: `Bool/Int/Uint/Int32/Uint32/Int64/Uint64/Float/SizeT/String/Vector<T>/Array<T,N>/SharedPtr<T>/DataType/Range1D` (`MG_Util/Types.h`), `MOBILEGL_ASSERT` and the `MGLOG_*` macros (`MG_Util/Debug/Log.h`), all GL/GLES/EGL enums and `GLenum`, `vulkan.h`, glslang, SPIRV-Cross C API, ska, xxHash, and 30-odd libstdc++ headers. So `MGPipeValueTypes.h` needs **only** `<Includes.h>` plus `<MG_Util/Math/VectorTypes.h>` (which is *not* in the umbrella — verify: `Includes.h` does not include it; `RenderState.h:11`, `SamplerObject.h:11`, `VertexArrayObject.h` all include it explicitly).

**Build wiring**: `CMakeLists.txt:529-535` puts both `${CMAKE_SOURCE_DIR}/MobileGL` and `${CMAKE_SOURCE_DIR}/MobileGL/MG_Pipe` on the include path, so the new header is spellable as both `<MG_Pipe/MGPipeValueTypes.h>` and `<MGPipeValueTypes.h>`. `MobileGL/MG_Test/Pipe/CMakeLists.txt:8-15` repeats those two paths for `PipeCatalogueTest`. No new CMake entry is needed for a header-only addition; a future `MGPipeRenderStateSpans.cpp` (P2) would need one.

---

## 6. Proposed contents of `MobileGL/MG_Pipe/MGPipeValueTypes.h`

Header order below is dependency order; the file is one `namespace MobileGL { ... }` block with a nested `namespace MG_State::GLState { ... }` for the two vertex types (see §7.2 on why).

```
// header banner (copy the 8-line LGPL block from MGPipeTypes.h:1-7)
#pragma once
#include <Includes.h>
#include <MG_Util/Math/VectorTypes.h>
// NOTHING from MG_State/GLState or MG_Backend. Purity gate A (plan section 10.3)
// asserts that; adding one back must turn it red.

namespace MobileGL {
  // --- 1. shared constants -------------------------------------------------
  inline constexpr Uint kMGMaxDrawBuffers = 8;   // canonical home of MAX_DRAW_BUFFERS
  inline constexpr Uint kMGMaxViewports   = 16;  // mirrored as RenderStateParameters::MAX_VIEWPORTS
  inline constexpr Uint kMGMaxVertexAttribs        = 32;
  inline constexpr Uint kMGMaxVertexAttribBindings = 32;

  // --- 2. render-state enums (verbatim from RenderState.h:15-189) ----------
  enum class BlendFactor { ... };          // RenderState.h:15-38
  enum class BlendEquation { ... };        // :40-48
  enum class LogicOperation { ... };       // :50-69
  enum class DepthTestFunc { ... };        // :71-82
  enum class StencilOperation { ... };     // :84-95
  enum class StencilFace { ... };          // :97-102     (see §7.3)
  enum class PixelStoreParam { ... };      // :104-127    (see §7.3)
  enum class CullFaceMode { ... };         // :129-135
  enum class FrontFaceMode { ... };        // :137-142
  enum class ProvokingVertexMode { ... };  // :144-149
  enum class CapabilityInput { ... };      // :151-189    (see §7.3)

  // --- 3. render-state value structs (verbatim, MEMBER ORDER UNCHANGED) ----
  struct PixelStoreParameters { ... };     // RenderState.h:191-200
  struct PerBufferBlendState  { ... };     // :202-210
  struct StencilFaceState     { ... };     // :212-220
  struct RenderStateParameters { ... };    // :222-370, with :263 and :273 rewritten
                                           //   to use kMGMaxDrawBuffers
  // --- 4. sampler enums + value struct (verbatim from SamplerObject.h) -----
  enum class SamplerFilterMode { ... };    // SamplerObject.h:14-19
  enum class SamplerMipmapMode { ... };    // :21-27
  enum class SamplerWrapMode   { ... };    // :29-36
  enum class SamplerCompareMode{ ... };    // :38-43
  enum class SamplerCompareFunc{ ... };    // :45-55
  enum class BorderColorForm : Uint8 { Float, Int, Uint };  // :66-70
  struct SamplerParameters { ... };        // :72-97

  namespace MG_State::GLState {
    // --- 5. vertex value structs -----------------------------------------
    // BufferObject is NOT visible here. See §7.1 for the three options; the
    // recommendation is a forward declaration + SharedPtr, which keeps the
    // move byte-for-byte but leaves the types non-POD, with the POD wire
    // shapes (MGPVertexAttributeWire / MGPVertexBindingWire) defined in
    // MGPipeTypes.h next to MGPVertexElements.
    class BufferObject;                     // fwd decl only
    struct VertexAttribute { ... };         // VertexArrayObject.h:17-51
    struct VertexBufferBindingPoint { ... };// :58-64
    struct VertexAttributeVersion { ... };  // :66-70 (trivially copyable; ride-along)
  }

  // --- 6. new trip wires (none exist today for these) ----------------------
  static_assert(std::is_trivially_copyable_v<PixelStoreParameters>);
  static_assert(sizeof(PixelStoreParameters) == 28);
  static_assert(std::is_trivially_copyable_v<PerBufferBlendState>);
  static_assert(std::is_trivially_copyable_v<StencilFaceState>);
  static_assert(std::is_trivially_copyable_v<RenderStateParameters>);
  static_assert(std::is_standard_layout_v<RenderStateParameters>);  // offsetof legality,
                                                                     // DirectGLES.cpp:2070-2071
  static_assert(offsetof(RenderStateParameters, BlendStates) < offsetof(RenderStateParameters, LogicOp));
  static_assert(std::is_trivially_copyable_v<SamplerParameters>);
} // namespace MobileGL
```

Whether `DynamicBackendParameters` joins this header is an open decision — see §7.4.

### What stays behind, as thin forwarding includes
- **`MG_State/GLState/RenderState/RenderState.h`**: delete `:11 #include <MG_Util/Math/VectorTypes.h>` and `:12 #include <MG_State/GLState/FramebufferState/FramebufferObject.h>`; add `#include <MG_Pipe/MGPipeValueTypes.h>`. Delete lines `:14-370` (all enums + the four structs) and keep only `namespace MG_State::GLState { class RenderState {...}; }` at `:372-538`. Every existing spelling (`MobileGL::RenderStateParameters`, `MobileGL::BlendFactor`, …) is unchanged, so this is a pure move. **The `FramebufferObject.h` include disappearing is the single biggest win** — it removes 8 headers from the closure of a header 73 files reach through `Core.h`.
- **`MG_State/GLState/SamplerState/SamplerObject.h`**: replace `:11 #include <MG_Util/Math/VectorTypes.h>` with `#include <MG_Pipe/MGPipeValueTypes.h>`; delete `:14-97`; keep `class SamplerObject` from `:99` on.
- **`MG_State/GLState/VertexArrayState/VertexArrayObject.h`**: add `#include <MG_Pipe/MGPipeValueTypes.h>`; delete `:17-70`; keep `#include "../BufferState/BufferObject.h"` at `:11` (still needed by `class VertexArrayObject` and, for the `SharedPtr` members to be usable, by every TU that copies a `VertexAttribute`).
- **`MG_State/GLState/FramebufferState/FramebufferObject.h:106`**: change `static constexpr Uint MAX_DRAW_BUFFERS = 8;` to `static constexpr Uint MAX_DRAW_BUFFERS = MobileGL::kMGMaxDrawBuffers;` (needs `#include <MG_Pipe/MGPipeValueTypes.h>` at the top, or leave the literal and add a `static_assert(MAX_DRAW_BUFFERS == kMGMaxDrawBuffers)` in a .cpp — the latter avoids a new MG_State→MG_Pipe include direction and is probably cleaner). All 80 existing `FramebufferObject::MAX_DRAW_BUFFERS` spellings keep working either way.
- **`MG_Pipe/MGPipeTypes.h`**: delete `:32-33` (the two debt includes) and `:36-40` (the three `using`s), add `#include "MGPipeValueTypes.h"` plus `using MobileGL::PixelStoreParameters; using MobileGL::RenderStateParameters;` — or nothing at all if the payloads are respelled `MobileGL::RenderStateParameters`. Update the debt comment at `:24-33` to record that the gate is now armed. `:140-145` (the compositional `MGPCaps` assertion) becomes a literal only if `DynamicBackendParameters` also moves.
- **`MG_Pipe/PipeFields.def`**: add `MGP_FIELDS_RenderStateParameters`, `MGP_FIELDS_PixelStoreParameters`, `MGP_FIELDS_PerBufferBlendState`, `MGP_FIELDS_StencilFaceState` (and `MGP_FIELDS_DynamicBackendParameters` if it moves), so `MGPipeFieldEqual`'s memcmp fallback (`generated/PipeVerify.inc:232-238`) stops being reachable from `ResidualValueBlock`/`MGPPixelPackState`/`MGPCaps`. That requires a `MGPipeHasFieldVerifier<T>` specialization each — `gen_pipe.py` emits those from the `.def`, so regenerate and commit `generated/*.inc`.

---

## 7. Risks, ODR hazards, and open decisions

### 7.1 `VertexAttribute` cannot become a POD without a redesign (highest risk)
`SharedPtr<BufferObject> Buffer` (`VertexArrayObject.h:39`) and `VertexBufferBindingPoint::Buffer` (`:59`) are `std::shared_ptr`. Three options, in increasing cost:
1. **Move as-is with a forward-declared `BufferObject`** (recommended for P0.5, "pure move"). The header stays free of `MG_State/GLState` includes, every consumer keeps compiling because `BufferObject.h` is already in their TU, and the types stay non-trivially-copyable — which is fine, because `MGPipeTypes.h:247-253` already carries them as a **blob**, not inline. Risk: a TU that includes only `MGPipeValueTypes.h` and then copies a `VertexAttribute` gets an incomplete-type error. Mitigate with a `static_assert` comment and by NOT adding a POD assertion for these two.
2. **Split into a POD wire twin** (`MGPVertexAttributeWire` with `Uint64 BufferHandle` instead of the `SharedPtr`, fixed-width `Uint64 Offset` instead of `SizeT`) defined in `MGPipeTypes.h`, leaving the frontend struct in `MGPipeValueTypes.h`. This is what `MGPVertexElements`'s blob actually needs and is probably the right end state, but it is a *design* change, not a move, and violates the P0.5 "纯搬移 / 全套测试逐名不变" acceptance criterion. Defer to P2/P3.
3. Template on the buffer handle type — rejected; it would break `offsetof`/`sizeof` reasoning and every `const VertexAttribute&` signature (`Managers.h:744`, `Managers.cpp:2199,2246,2256,2627`, `VertexInputStateFactory.cpp`).

### 7.2 Namespace hazard — `VertexAttribute` is nested one level deeper
`RenderStateParameters`, `PixelStoreParameters` and `SamplerParameters` live directly in `MobileGL`; `VertexAttribute` and `VertexBufferBindingPoint` live in `MobileGL::MG_State::GLState` (`VertexArrayObject.h:14-16`). If the new header reopens `namespace MobileGL::MG_State::GLState`, then **`MG_Pipe` defines names inside `MG_State`** — cosmetically wrong for a purity gate that is about MG_State, and it will confuse any grep-based gate. Two defensible answers: (a) keep the nesting and note it (zero call-site churn, ~100 `MG_State::GLState::VertexAttribute` spellings survive); (b) move them to bare `MobileGL` and add `namespace MG_State::GLState { using MobileGL::VertexAttribute; using MobileGL::VertexBufferBindingPoint; }` back in `VertexArrayObject.h`. (b) is cleaner and still name-transparent; the `using` alias makes `MG_State::GLState::VertexAttribute` keep resolving. **Recommend (b).**

### 7.3 Which enums move — the "vocabulary vs. value" cut
`StencilFace` (`RenderState.h:97-102`), `PixelStoreParam` (`:104-127`) and `CapabilityInput` (`:151-189`) are **not members of any value struct**. They are the argument vocabulary of `class RenderState`'s setters (`RenderState.h:431-434, 456-460, 495-497`) and of the four `MG_Util/Converters/*/RenderStateEnumConverter.h`. Leaving them behind in `RenderState.h` is legal but forces those four converter headers to keep including `RenderState.h` — which after the split is cheap, since `RenderState.h`'s closure will have collapsed. Moving them is also legal and makes the converters includable from `MGPipeValueTypes.h` alone. **Either works; the ARCHITECTURE.md list does not name them.** Whichever is chosen, it must be uniform — a *split* (some enums here, some there) is what creates the worst maintenance trap.

### 7.4 `DynamicBackendParameters` — contradiction between the two specs
`ARCHITECTURE.md:260` omits it from the `MGPipeValueTypes.h` list; `MGPipeTypes.h:140-143` says P0.5 moves it "with fixed-width members". "With fixed-width members" is **not a pure move** — it changes the three `SizeT` fields (`BackendObject.h:317, 322, 545`) to `Uint64` and would change `sizeof`, breaking `MGPipeTypes.h:144-145` on purpose. It also drags a second problem: `DynamicBackendParameters` has two member functions taking `TextureTarget` (`BackendObject.h:487-497`), which is why `BackendObject.h:11` includes `MG_State/GLState/TextureState/TextureEnum.h` — so moving it either drags `TextureEnum.h` into MG_Pipe or forces those two helpers to be free functions left behind in `BackendObject.h`. **Recommendation**: in P0.5 do the pure structural move only (leave `DynamicBackendParameters` where it is, keep `MGPipeTypes.h:32`'s `BackendObject.h` include, and note that gate A is armed for MG_State but not yet for MG_Backend), or move it *verbatim* including the `SizeT`s and keep the compositional assertion. Widening the fields is a separate, testable change. Escalate this to the plan owner rather than silently picking.

### 7.5 ODR / one-definition risks
- **Double definition**: the move must *delete* the originals, not duplicate them. Duplicating `RenderStateParameters` in two headers under the same name in namespace `MobileGL` is an ODR violation that neither compiler will diagnose across TUs and that would silently break `DirectGLES.cpp`'s `offsetof` shadow if the two ever drift. Grep after the move: `grep -rn "struct RenderStateParameters\|struct PixelStoreParameters\|struct SamplerParameters\|struct VertexAttribute\|struct VertexBufferBindingPoint\|struct PerBufferBlendState\|struct StencilFaceState" MobileGL/` must return exactly one hit each, in `MGPipeValueTypes.h`.
- **`MAX_DRAW_BUFFERS` divergence**: if the constant is duplicated (a `kMGMaxDrawBuffers = 8` in the new header and a literal `8` left at `FramebufferObject.h:106`), the two can drift and `RenderStateParameters::BlendStates` would silently stop matching `FramebufferObject`'s attachment arrays. Pin with a `static_assert` in a .cpp that sees both.
- **`#pragma once` + two include paths**: `MG_Pipe` is on the include path twice (`CMakeLists.txt:531` and `:535`), so the same file is reachable as `<MG_Pipe/MGPipeValueTypes.h>` and `<MGPipeValueTypes.h>`. `#pragma once` is path-canonicalizing on GCC/Clang/MSVC so this is safe in practice, but on Windows with mixed case or symlinked worktrees it has bitten before. **Recommend adding a belt-and-braces include guard macro alongside `#pragma once`** for this one header, since it is the only header spellable two ways that also defines types with size assertions.
- **`MG_Test/Pipe/PipeCatalogueTest.cpp` include set**: it includes `"Includes.h"` and `<MG_Pipe/MGPipe.h>` only (`:17-18`) and asserts on `RenderStateParameters`/`PixelStoreParameters` at `:113`. If `MGPipeTypes.h` stops including `RenderState.h`, this test still compiles only because the new header supplies them — good, that is exactly the gate. If someone "fixes" the test by adding a `RenderState.h` include, the gate is defeated; forbid it in review.
- **Enum-to-`GLenum` implicit conversions**: none of these enums are `enum class` with a fixed underlying type except `BorderColorForm : Uint8`. Moving them cannot change their underlying type unless someone "tidies" it — do not. `BorderColorForm`'s `: Uint8` is load-bearing for `SamplerParameters`'s tail padding.
- **Member reordering**: forbidden. `DirectGLES.cpp:2070-2071` reads `offsetof(..., BlendStates)` and `offsetof(..., LogicOp)`, and `generated/PipeSpanTable.inc:34-59` names 24 members by string for P2's `offsetof` table. A reorder that keeps `sizeof` at 1168 would pass every existing assertion and silently corrupt Espryt's span diffing.

### 7.6 Files that must change their `#include` lines
Header edits (the move itself):
1. `MobileGL/MG_State/GLState/RenderState/RenderState.h` — drop `:11`, `:12`; add `<MG_Pipe/MGPipeValueTypes.h>`; delete `:14-370`.
2. `MobileGL/MG_State/GLState/SamplerState/SamplerObject.h` — replace `:11`; delete `:14-97`.
3. `MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h` — add the new include; delete `:17-70`; add the `using` aliases if §7.2(b) is chosen.
4. `MobileGL/MG_State/GLState/FramebufferState/FramebufferObject.h:106` — pin `MAX_DRAW_BUFFERS` against the new constant.
5. `MobileGL/MG_Pipe/MGPipeTypes.h` — drop `:32-33`, rework `:36-40`, add `"MGPipeValueTypes.h"`, update the debt comment `:24-33` and (conditionally) `:140-145`.
6. `MobileGL/MG_Pipe/PipeFields.def` — add the four/five new field lists; then rerun `python3 scripts/gen_pipe.py` and commit `MG_Pipe/generated/PipeVerify.inc` (and any other regenerated `.inc`).

Includers that *could* be narrowed (optional, and each one narrowed is a place the purity gate gets stronger; none is required for the build to pass, because all five headers keep their old names and namespaces):
7. `MobileGL/MG_Util/Texture/PixelStoreProcessor.h:11` — wants `PixelStoreParameters` only; swap `RenderState.h` → `MGPipeValueTypes.h`.
8-11. `MobileGL/MG_Util/Converters/{GLToMG,MGToGL,MGToStr,MGToVk}/RenderStateEnumConverter.h:11` — want the enums only; swap if §7.3 moves the enums.
12. `MobileGL/MG_Backend/DirectVulkan/Renderer/VkSamplerManager.h:14` — needs `class SamplerObject`, so it must keep `SamplerObject.h`. **No change.**
13. `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h` — needs `class VertexArrayObject`. **No change.**
14. `MobileGL/MG_Impl/GLImpl/Sampler/Validators.h`, `MG_Impl/GLImpl/VertexArray/Validators.h` — check whether they need the class or only the value struct.

Files that need **no** change and must be verified unchanged (the "纯搬移" proof): all 51 `RenderStateParameters` uses in `MG_Backend/DirectGLES/DirectGLES.cpp`, all 17 in `MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp`, the 11 in `MG_State/GLState/RenderState/RenderState.cpp`, and the 73 includers of `MG_State/GLState/Core.h`.

### 7.7 Suggested verification, cheapest first
1. `grep -rn "MG_State/GLState" MobileGL/MG_Pipe/` must be empty (gate A). Add the deliberate-red check by re-adding one include.
2. `g++ -H -fsyntax-only` (or `clang -H`) on a TU that includes only `<MG_Pipe/MGPipeValueTypes.h>` — the printed tree must contain no `MG_State/` path. This is the "CI `-H` 闭包断言" ROADMAP.md:16 asks for.
3. Compile-only check that `sizeof(RenderStateParameters)` is unchanged before/after — add a temporary `static_assert(sizeof(RenderStateParameters) == 1168)` (confirm the real number first; 1168 above is inferred arithmetic from `MGL_RESIDUAL_BLOCK_SIZE`, not measured).
4. `PipeCatalogueTest` (`MG_Test/Pipe/`) must stay green without gaining an include.
5. `nm`/`.text` diff per ROADMAP.md:16 — a pure move should produce zero symbol changes.
