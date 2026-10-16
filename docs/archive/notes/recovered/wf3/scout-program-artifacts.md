# Scout report — `MG_State/GLState/ProgramState/ProgramArtifacts.h` (P0.5)

Worktree root (all paths below are relative to it unless absolute):
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`
Branch `feat/disaggregated`. Read-only scout; nothing was modified.

Every file:line below was opened and read.

---

## 0. The authoritative list of the five types

`docs/Disaggregated/ARCHITECTURE.md:260` (section 7, "Shader state = SPIR-V + 反射归档"), verbatim
naming of the five:

> P0.5 把 `TypeFacts`、`ResourceReflection`、`XfbVarying`、`LinkArtifacts`、`SpirvArtifacts` 抽到
> `MG_State/GLState/ProgramState/ProgramArtifacts.h`（只 include `<Includes.h>` 与容器），更新 7 个
> includer，加 CI `-H` 闭包断言。

`docs/Disaggregated/ROADMAP.md:16` (P0.5 row) repeats it: "`ProgramArtifacts.h`（五个反射类型,不
include `ShaderObject.h`/`SpvcSession.h`,更新 7 个 includer);`Visit()` 归档 + `sizeof` 绊线;CI `-H`
include 闭包断言". Exit gate: "全套测试逐名不变(纯搬移);两条闭包断言绿且人为加回一个 `MG_State`
include 能变红;`nm`/`.text` 变化可逐符号归因". Dependency line: "P0; **P1 与 P7 的硬前置**".

So the five are, in dependency order:

1. `TypeFacts`
2. `ResourceReflection` (+ its four aliases)
3. `XfbVarying`
4. `LinkArtifacts`
5. `SpirvArtifacts`

Other supporting facts from the docs:

- `ARCHITECTURE.md:258` — `CreateShaderState`'s payload is per-stage SPIR-V + the archive
  (`LinkArtifacts` + `SpirvArtifacts`, **whole structs**), not source. glslang lives entirely on the
  client, SPIRV-Cross (`TranspileSpirvToEssl`) entirely on the server, split at file granularity.
- `ARCHITECTURE.md:259` — archive mechanism is `Visit()` + a `sizeof` tripwire
  (`static_assert(sizeof(LinkArtifacts) == MGL_LINKARTIFACTS_SIZE)`), one field table serving both
  serialization directions. Required coverage listed explicitly: the four `ResourceReflection`
  vectors (each carrying `TypeFacts`), `uniformSamplerOrImageUnitIndex`, `uniformBlockBinding`,
  `shaderStorageBlockBinding` (by name), `explicitOpaqueUniformBindings`,
  `xfbVaryings/xfbStrides/xfbPackedStride/xfbNeedsScatteredCapture`, `computeLocalSize`, the
  GS/TCS/TES facts, `usesReservedNumSamples`, `uniformOffsets`. `XfbVarying` carries two spellings
  (GL name + block instance/member/element).
- `ARCHITECTURE.md:133` — `MGPProgramDesc` is 192 bytes: per-stage SPIR-V blob x6 + reflection blob
  + `StageMask`/`GlobalUboSize`/`ReservedNumSamplesOffset` + four state bytes.
- `ARCHITECTURE.md:260` last sentence — without this step, P7's `nm -D | grep glslang` criterion is
  unreachable.
- `ROADMAP.md` P7 row — gate is literally `nm -D libMobileGLServer.so | grep glslang` empty.

---

## 1. Current definitions — file:line, members, dependencies

All five live **inside `class ProgramObject`** in
`MobileGL/MG_State/GLState/ProgramState/ProgramObject.h` (1803 lines total; class opens at :25,
closes at :1802). They are therefore spelled `ProgramObject::TypeFacts` etc. everywhere.

### 1.1 `TypeFacts` — ProgramObject.h:44–72 (doc comment :41–43)

Flattened `glslang::TType`. All scalar POD; no heap member.

| line | member | type |
|---|---|---|
| 45 | `isArray` | `Bool` |
| 48 | `isSizedArray` | `Bool` |
| 49 | `isMatrix` | `Bool` |
| 50 | `isVector` | `Bool` |
| 51 | `isOpaque` | `Bool` |
| 52 | `isTexture` | `Bool` |
| 53 | `isImage` | `Bool` |
| 54 | `isDouble` | `Bool` (`getBasicType() == EbtDouble`) |
| 55 | `isVoid` | `Bool` (hidden block members) |
| 56 | `isBuffer` | `Bool` (`qualifier.storage == EvqBuffer`) |
| 57 | `isPatch` | `Bool` |
| 58 | `hasIndex` | `Bool` |
| 59 | `hasFormat` | `Bool` |
| 60 | `vectorSize` | `Int` |
| 61 | `matrixCols` | `Int` |
| 62 | `matrixRows` | `Int` |
| 63 | `layoutIndex` | `Int` |
| 64 | `layoutFormat` | `Uint` |
| 68 | `layoutMatrix` | `Int` (widened `glslang::TLayoutMatrix`) |
| 71 | `basicType` | `Int` (widened `glslang::TBasicType`) |

**Already glslang-free by type** — the widening to `Int`/`Uint` was done deliberately (comments at
:65–70 say so). `Bool`/`Int`/`Uint` come from `MG_Util/Types.h:33,29,30`.

### 1.2 `ResourceReflection` — ProgramObject.h:76–99 (doc comment :74–75); aliases :101–104

Flattened `glslang::TObjectReflection`; used for uniforms, blocks, pipe inputs and pipe outputs
alike.

| line | member | type |
|---|---|---|
| 77 | `name` | `String` (= `std::string`, `MG_Util/Types.h:21`) |
| 78 | `glDefineType` | `GLenum` |
| 79 | `offset` | `Int` = -1 |
| 82 | `size` | `Int` (raw `TObjectReflection::size`) |
| 86 | `index` | `Int` = -1 (TPROGRAM block index owning it) |
| 87 | `counterIndex` | `Int` = -1 |
| 88 | `arrayStride` | `Int` |
| 89 | `topLevelArraySize` | `Int` |
| 90 | `topLevelArrayStride` | `Int` |
| 91 | `binding` | `Int` = -1 |
| 92 | `location` | `Int` = -1 |
| 95 | `stages` | `Uint32` (an `EShLanguageMask`, widened) |
| 98 | `arraySize` | `GLint` = 1 |
| 99 (`type;`) | `type` | `TypeFacts` |

Aliases, ProgramObject.h:101–104:
```
using UniformReflection    = ResourceReflection;   // :101
using BlockReflection      = ResourceReflection;   // :102
using PipeInputReflection  = ResourceReflection;   // :103
using PipeOutputReflection = ResourceReflection;   // :104
```
Dependencies: `String`, `GLenum`/`GLint` (GL headers via `Includes.h:68–73`), `TypeFacts`. **No
glslang type by value.**

### 1.3 `XfbVarying` — ProgramObject.h:1146–1171 (doc comment :1144–1145)

| line | member | type |
|---|---|---|
| 1147 | `name` | `String` (GL spelling, incl. `"Block.member"`) |
| 1148 | `type` | `GLenum` = `GL_FLOAT` |
| 1149 | `size` | `GLint` = 1 |
| 1150 | `bufferIndex` | `Uint32` |
| 1151 | `offsetBytes` | `Uint32` |
| 1152 | `byteSize` | `Uint32` |
| 1155 | `packedOffsetBytes` | `Uint32` |
| 1166 | `blockInstanceName` | `String` |
| 1167 | `blockName` | `String` |
| 1168 | `blockMemberIndex` | `Int` = -1 |
| 1171 | `blockMemberElement` | `Int` = -1 |

**No glslang type.** This is the "two spellings" struct `ARCHITECTURE.md:259` calls out.

### 1.4 `LinkArtifacts` — ProgramObject.h:1210–1393 (doc comment block :1173–1209)

53 members. Only **two** carry a glslang type:

- **:1215 `SharedPtr<glslang::TProgram> program;`** — live only between `LinkProgram()` and the end
  of `DoReflection`; null for an L1-memo-served link; must not be dereferenced outside
  `DoReflection` (comment :1211–1214).
- **:1282 `Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;`**

Full member list (line: declaration):

```
1215  SharedPtr<glslang::TProgram> program;              <-- glslang
1218  Vector<UniformReflection>    uniformReflection;
1219  Vector<BlockReflection>      blockReflection;
1220  Vector<PipeInputReflection>  pipeInputReflection;
1221  Vector<PipeOutputReflection> pipeOutputReflection;
1227  Bool  lastStageIsFragment = false;
1228  Array<GLuint, 3> computeLocalSize{};
1231  UnorderedMap<String, Int>  uniformIndexByName;
1234  Vector<String>  attribs;
1235  Vector<GLenum>  attribTypes;
1238  UnorderedMap<String, Uint> linkedFragDataLocation;
1239  UnorderedMap<String, Uint> linkedFragDataIndex;
1245  Vector<Int> glUniformIndexToTProgram;
1246  Vector<Int> tProgramUniformIndexToGl;
1247  Vector<Int> glBlockIndexToTProgram;
1248  Vector<Int> tProgramBlockIndexToGl;
1269  Vector<Int> glUniformBlockIndexToBlock;
1270  Vector<Int> blockIndexToGlUniformBlock;
1275  UnorderedMap<String, Int> linkedExplicitUniformLocations;
1282  Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;   <-- glslang
1283  UnorderedMap<String, Uint> uniformLocations;
1291  Vector<Uint64> writtenUniformLocationBits;
1292  Vector<Uint64> writtenUniformIndexBits;
1293  Vector<Uint>   writtenUniformIndices;
1296  Vector<Int> uniformIndexInTProgram;
1298  Vector<Int> uniformSamplerOrImageUnitIndex;
1303  UnorderedMap<String, Uint> explicitOpaqueUniformBindings;
1313  UnorderedMap<String, Uint> uniformBlockIndexByName;
1314  Vector<Int> uniformBlockBinding;
1325  UnorderedMap<String, Int> shaderStorageBlockBinding;
1331  std::set<String> storageBlocksWithoutBinding;      <-- needs <set>
1336  std::set<String> uniformBlocksWithoutBinding;      <-- needs <set>
1338  Uint activeUniformCount = 0;
1346  Bool usesReservedNumSamples = false;
1347  Uint maxUniformLocation = 0;
1348  Int  uniformNameMaxLength = 0;
1349  Int  attribInNameMaxLength = 0;
1350  Int  uniformBlockNameMaxLength = 0;
1352  String infoLog;
1353  Bool linkStatus = false;
1357  Vector<XfbVarying> xfbVaryings;
1363  Vector<String>  xfbInterfaceNames;
1364  Vector<Uint32>  xfbStrides;
1365  Vector<Uint32>  gsStripTriangles;
1366  Bool   gsStripCaptureFixup = false;
1367  GLenum gsInputPrimitive = GL_NONE;
1371  Int    tcsOutputVertices = 0;
1380  GLenum gsOutputPrimitive = GL_NONE;
1381  Int    gsMaxVertices = 0;
1382  Int    gsInvocations = 0;
1385  GLenum tessGenMode = GL_NONE;
1386  GLenum tessGenSpacing = GL_NONE;
1387  GLenum tessGenVertexOrder = GL_NONE;
1388  Bool   tessGenPointMode = false;
1389  GLenum xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
1390  Int    xfbVaryingNameMaxLength = 0;
1391  Bool   xfbNeedsScatteredCapture = false;
1392  Uint32 xfbPackedStride = 0;
```

Membership rule stated at :1177–1180: "exactly the field list `ResetLinkArtifacts()` clears (plus
the four it forgot to — `infoLog`, `linkedFragDataLocation/Index` and the geometry strip-capture
pair)". Access rule (invariant I5) at :1187–1192: `m_artifacts` is private and reachable only via
`ProgramObject::Artifacts()`, which calls `EnsureLinkJoined()` first.

`std::set` is used at :1331 and :1336 but **`ProgramObject.h` never includes `<set>`** — it arrives
transitively (`Includes.h` includes `<map>` at :17 but not `<set>`; `<set>` appears explicitly only
in `MG_Util/ShaderTranspiler/glslang/TMglGlslIoResolver.h:15`, `ShaderCompiler.h:16`,
`ShaderSourceProcessor.h:10`, `SpirvPasses/FlattenXfbInterfaceBlocksPass.h:15`,
`SpirvPasses/UniquifyIoBlockNamesPass.h:16`, `TranslationCache.h:15`). **The new header must include
`<set>` itself** or it will fail to compile once the transitive path is cut.

### 1.5 `SpirvArtifacts` — ProgramObject.h:1409–1449 (doc comment :1395–1408)

| line | member | type |
|---|---|---|
| 1410 | `generatedSpirv` | `Vector<Vector<unsigned>>` |
| 1411 | `enableSpirvValidation` | `Bool` |
| 1414 | `uniformOffsets` | `Vector<Uint>` |
| 1415 | `globalUboScratch` | `Vector<Uint8>` |
| 1420 | `reservedNumSamplesOffset` | `Uint` = **`kInvalidUniformOffset`** |
| 1427 | `spirvStatus` | `Bool` |
| 1436 | `nativeFloat64` | `Bool` |
| 1448 | `pointSizeDemoted` | `Bool` |

**No glslang, no spirv-cross, no SPIRV-Reflect type.** One external dependency:
`kInvalidUniformOffset`, declared at **ProgramObject.h:547** as
`static constexpr Uint kInvalidUniformOffset = ~0u;` — i.e. a `ProgramObject` static member declared
*before* `SpirvArtifacts` but *outside* it. Moving `SpirvArtifacts` out requires moving (or
mirroring) this constant. Its readers, all of which spell it `ProgramObject::kInvalidUniformOffset`:
`MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:4659` and `:8553`;
`MG_Impl/GLImpl/Program/GL_Program.cpp:1249,1304,1522`; `MG_State/GLState/Core.cpp:513,514`;
`MG_State/GLState/ProgramState/ProgramObject.cpp:198,273`; plus in-header uses at
ProgramObject.h:565, :811, :1420.

### 1.6 `TUniformInitializer` — the one type that must be re-declared

`include/glslang/MachineIndependent/localintermediate.h:627–636` (vendored/forked glslang; this
struct is a MobileGL fork addition, see commit `e5846569c`):

```cpp
struct TUniformInitializer {
    std::string name;
    TBasicType basicType = EbtVoid;
    int vectorSize = 1;
    int matrixCols = 0;
    int matrixRows = 0;
    int arraySize = 1;
    std::vector<long long> intValues;
    std::vector<double>    floatValues;
};
```

It is a plain aggregate — no pool allocator, no `TString`. `ProgramTranslationCache.h:44–47`
independently states this: "the only member that ever pointed into glslang-owned memory was
`program` itself, and `TUniformInitializer` / `XfbVarying`, which look like glslang types, are
`std::string` + `std::vector` aggregates."

Producers/consumers of `uniformInitialValues`:
- `MG_State/GLState/ProgramState/ProgramLinkTask.cpp:612–618` (dedupe + push_back)
- `MG_State/GLState/ProgramState/ProgramObject.cpp:148` (`ApplyUniformInitialValues`, reads it)
- `MG_State/GLState/ProgramState/ProgramObject.cpp:356` (`ResetLinkArtifacts` clears it)

**`ProgramObject.cpp:180–183` compares `init.basicType` against `glslang::EbtFloat`,
`glslang::EbtFloat16`, `glslang::EbtDouble`, `glslang::EbtInt`, `glslang::EbtUint`,
`glslang::EbtBool`.** Re-typing `basicType` to `Int` keeps this compiling only because those
enumerators implicitly convert; if the new type is to be glslang-free, that `.cpp` (which is
client-side and may keep including glslang) stays as-is, but the *header* must not name
`glslang::TBasicType`.

---

## 2. Transitive include closure of `ProgramObject.h` today

Direct includes, `ProgramObject.h:10–14`:
```
:10  #include <Includes.h>
:11  #include "ShaderObject.h"
:13  #include <MG_Util/Metrics/BufferMetrics.h>
:14  #include <MG_Util/ShaderTranspiler/SpvcSession.h>
```

Level 2:
- `MG_State/GLState/ProgramState/ShaderObject.h:10–13` →
  `<Includes.h>`, `ShaderStage.h` (no includes at all), `ShaderCompileTask.h`,
  `ShaderCompileAdoptionMap.h`.
- `MG_Util/ShaderTranspiler/SpvcSession.h:10–12` → `<Includes.h>`, **`<spirv_reflect.h>`**,
  `MG_Util/ShaderTranspiler/Types.h`.
- `MG_Util/Metrics/BufferMetrics.h:10` → `<Includes.h>` only.

Level 3:
- `ShaderCompileTask.h:10–15` → `<Includes.h>`, `MG_Util/Async/JobNode.h`,
  `MG_Util/ShaderTranspiler/CompileEnv.h`, `MG_Util/ShaderTranspiler/Types.h`,
  **`MG_State/GLState/BufferState/BufferState.h`**, `ProgramState/ShaderPreprocessCache.h`.
- `ShaderCompileAdoptionMap.h:10–11` → `<Includes.h>`, `ProgramState/ShaderSourceKey.h`.
- `MG_Util/ShaderTranspiler/Types.h:10–11` → `<Includes.h>`, `CompileEnv.h`.

**`Includes.h` is the elephant.** `MobileGL/Includes.h:79–83`, unconditional:
```
#include <glslang/Include/Types.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Include/intermediate.h>
#include <glslang/MachineIndependent/localintermediate.h>
```
plus `Includes.h:56` `<spirv_cross/spirv_cross_c.h>`, `:53` `<ska/flat_hash_map.hpp>`, `:55`
`<xxhash.h>`, `:63–74` EGL/GL/GLES headers, `:130` `<vulkan/vulkan.h>`, `:148` `MG_Util/Debug/Log.h`,
`:149` `MG_Util/Types.h` (which at :11 includes `<Includes.h>` back and at :12 `GLExtensions.h`).

**Consequence to flag loudly for the implementers:** the ARCHITECTURE wording "只 include
`<Includes.h>` 与容器" does **not** by itself remove glslang from the preprocessed text of
`ProgramArtifacts.h`, because `Includes.h` drags glslang in unconditionally. What it removes is the
*by-value/by-symbol* dependency (`SharedPtr<glslang::TProgram>` needs `~TProgram`;
`Vector<TUniformInitializer>` needs the type complete) — which is exactly what the P7 gate
(`nm -D | grep glslang`, a **link/symbol** gate) measures. Two options for the header, both worth
recording in the design note:
- (a) include `<Includes.h>` as the docs say and rely on the type-level purge; the `-H` closure
  assertion then asserts the absence of `ShaderObject.h` / `SpvcSession.h` / `spirv_reflect.h`, not
  the absence of glslang.
- (b) include only `MG_Util/Types.h` + `<set>` + GL headers, which would be genuinely glslang-free —
  but `MG_Util/Types.h:11` itself includes `<Includes.h>`, so this is not achievable without
  splitting `Types.h` too. **(a) is the only reachable shape today**; say so in the header comment.

Where `Includes.h` gets its type aliases (`MG_Util/Types.h`): `String` :21, `Int8..Uint64` :23–32,
`Bool` :33, `Float`/`Double` :34–35, `StringView` :36, `Vector` :37–38, `Pair` :39–40, `SharedPtr`
:41–42, `UniquePtr` :43–44, `WeakPtr` :45–46, `SizeT` :55, `Array` :56–57,
`UnorderedMap = ska::flat_hash_map` :87–88, `Optional` :117, `Data = Vector<Uint8>` :123.

---

## 3. Every includer — verified counts

### 3.1 `ProgramObject.h` — 8 non-self includers (9 with its own .cpp)

| # | file:line | what it uses |
|---|---|---|
| 1 | `MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.h:13` | `ProgramObject` by reference in `ComputeHash` (:474), `GetOrCreateProgram` (:475), `VkProgramObject::program` pointer (:583), `stages` doc note (:590). Names **no** artifact type. |
| 2 | `MobileGL/MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:13` | `const ProgramObject&` parameter on ~15 resolvers (:232, :250, :465, :751, :808, :835, :862, :986, :1146, :1260, :1643, :1712, :1759, :1802, :1884). Accessors only; names no artifact type. |
| 3 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:17` | `ProgramObject::kInvalidUniformOffset` (:4659, :8553); constructs internal blit/depth-mipmap `ProgramObject`s (:4232–4253, :4312–4316). |
| 4 | `MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp:11` | **the heaviest consumer.** `ProgramObject::TypeFacts` (:84, :187, :198, :435), `ProgramObject::ResourceReflection` (:93), `ProgramObject::BlockReflection` (:176, :251), `ProgramObject::LinkArtifacts` (:228, :265, :328, :437, :528). Fields read off `LinkArtifacts`: `uniformReflection`, `blockReflection`, `pipeInputReflection`, `pipeOutputReflection`, `lastStageIsFragment` (only those five). |
| 5 | `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.h:11` | `ProgramObject::LinkArtifacts artifacts;` (:80) and `SpirvHandoff::reflection` (:118); `SharedPtr<const ProgramObject::SpirvArtifacts> cachedSpirv` (:147); `SharedPtr<const ProgramObject::LinkArtifacts> linkArtifactsForCache` (:151). Also holds `Vector<SharedPtr<glslang::TShader>> shaders` (:108) — genuinely glslang, stays client-side. |
| 6 | `MobileGL/MG_State/GLState/ProgramState/ProgramPipelineObject.h:11` | pipeline composite; holds `SharedPtr<ProgramObject>` per stage. |
| 7 | `MobileGL/MG_State/GLState/ProgramState/ProgramState.h:12` | the object table. **This is the fan-out node**: `MG_State/GLState/Core.h:16` includes it, so every `Core.h` includer (all of `MG_Impl`, both backends' `Managers.h`/`DirectVulkan.cpp`) reaches `ProgramObject.h` transitively. |
| 8 | `MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.h:11` | `ProgramTranslationResult { ProgramObject::LinkArtifacts link; ProgramObject::SpirvArtifacts spirv; }` (:50–51). |
| (self) | `MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp:9` | — |

**The roadmap's "7 个 includer" does not reproduce as the ProgramObject.h includer count (8 non-self,
9 with the .cpp).** It *does* reproduce exactly as the `ShaderObject.h` includer count (7, below), so
the number is either a miscount or was written against `ShaderObject.h`. Implementers should plan
for **8 includers of `ProgramObject.h`** plus the files that only *name* the types (§3.4).

### 3.2 `ShaderObject.h` — exactly 7 includers

| # | file:line | uses |
|---|---|---|
| 1 | `MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.h:14` | `ShaderObject` for stage/source |
| 2 | `MobileGL/MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp:18` | builds internal shaders (:4232 ff.) |
| 3 | `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:11` | `SharedPtr<ShaderObject>` attach lists, `LinkedShaderRef` (:150–154) |
| 4 | `MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp:9` | self |
| 5 | `MobileGL/MG_Util/Converters/GLToMG/ProgramEnumConverter.h:11` | `ShaderStage` conversion |
| 6 | `MobileGL/MG_Util/Converters/MGToGL/ProgramEnumConverter.h:11` | ditto |
| 7 | `MobileGL/MG_Util/ShaderTranspiler/ShaderSourceProcessor.h:13` | source preprocessing |

### 3.3 `SpvcSession.h` — 18 includers

`MobileGL/MG_Backend/DirectVulkan/Renderer/ProgramFactory.cpp:13`;
`MobileGL/MG_Benchmark/ShaderCache/TranslationCacheBench.cpp:60`;
`MobileGL/MG_Benchmark/Transpile/TranspileProfile.cpp:43`;
`MobileGL/MG_State/GLState/ProgramState/ProgramObject.h:14`;
`MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.cpp:14`;
`MobileGL/MG_Test/ShaderTranspiler/DemoteFloat64Test.cpp:18`;
`.../DemotePointSizeTest.cpp:17`; `.../FlattenXfbInterfaceBlocksTest.cpp:18`;
`.../LegalizeResourceArrayIndexTest.cpp:17`; `.../LowerViewportIndexTest.cpp:29`;
`.../StripIoBlockLocationsTest.cpp:17`; `.../TranslationCacheTest.cpp:39`;
`.../UniquifyIoBlockNamesTest.cpp:19`; `.../WidenImageFormatsTest.cpp:34`;
`MobileGL/MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.cpp:10`;
`MobileGL/MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h:11`;
`MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.h:11`;
`MobileGL/MG_Util/ShaderTranspiler/SpvcSession.cpp:9`.

**`ProgramObject.h:14` is the only one of the 18 that is a `ProgramObject.h` include** — dropping it
costs nothing to the other 17. Grep confirms `ProgramObject.h` names no `spvc_*` / `SpvReflect*` /
`SpvcMetadata` symbol in its own body; the include is dead weight there (verify with a build before
deleting — `ProgramObject.cpp` may rely on it transitively).

There are **no includers of any of the three headers outside `MobileGL/`** (checked the whole
worktree; `Testing/`, `tools/`, `3rdparty/` have none).

### 3.4 Files that *name* one of the five types (these need `ProgramArtifacts.h`, or keep getting it
via `ProgramObject.h`)

- `MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp:27–28` (`using TypeFactsRef = const ...::ProgramObject::TypeFacts&`), then :243, :250, :1197, :1213, :1246, :1301, :1545.
- `MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp` — see §3.1 row 4.
- `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.cpp:48–49` (`MakeTypeFacts`), :79–81 (`MakeResourceReflection`), :94, :133, :866, :872, :876–877, :987, :991, :1114, :1122, :1302, :1331, :1383, :1439, :1539, :1730, :1758, :1768, :1789, :1796, :1857, :1944, :1960, :2038.
- `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.h:80, :114, :118, :145, :147, :148, :151, :203`.
- `MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.h:46`; `ProgramSpirvTask.cpp:127, :196, :234, :349`.
- `MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp:198, :207, :273, :332–343, :356, :499–501, :686`.
- `MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.h:21, :34, :38–39, :46, :50–51`; `ProgramTranslationCache.cpp:39–40` (`sizeof(ProgramObject::ResourceReflection)`).
- `MobileGL/MG_Util/ShaderTranspiler/TranslationCache.h:54, :429, :442` — **comments only**; MG_Util deliberately must not depend on MG_State (`ProgramTranslationCache.h:38–39`).
- `MobileGL/MG_Pipe/MGPipeTypes.h:296` — comment on `MGPProgramDesc` naming the archive.
- `MobileGL/MG_Test/Program/AsyncSpirvPhaseTest.cpp:336` — comment.
- `MobileGL/MG_Backend/DirectGLES/Managers.cpp:6424, :6440` — uses `TypeFacts` through `const auto&`, never spelling the name.

---

## 4. How the two backends read the artifacts today

**Neither backend names `LinkArtifacts`, `SpirvArtifacts`, `ResourceReflection` or `XfbVarying`
anywhere.** Both go exclusively through `ProgramObject` accessors, which are what the `PipeInputs`
work of P1 replaces. This is good news for P0.5: moving the type declarations does not touch backend
code at all.

### 4.1 DirectGLES (Espryt) — reads the **front-end tables**

Reaches `ProgramObject.h` transitively: `DirectGLES/Managers.h:17` → `MG_State/GLState/Core.h:16` →
`ProgramState/ProgramState.h:12` → `ProgramObject.h`. (`DirectGLES.h:10–15` and `Managers.h:10–18`
contain no direct include.)

Call sites (file:line → accessor → underlying artifact field):

*XFB (all `LinkArtifacts`)*
- `DirectGLES.cpp:890` `GetTransformFeedbackPackedStride()` → `xfbPackedStride`
- `DirectGLES.cpp:925, :1034` `GetTransformFeedbackStride(i)` → `xfbStrides[i]`
- `DirectGLES.cpp:931`, `Managers.cpp:7448`, `Managers.cpp:8092` `GetTransformFeedbackVaryings()` → `xfbVaryings`
- `DirectGLES.cpp:1005` `GetTransformFeedbackBufferCount()`
- `DirectGLES.cpp:1027` `NeedsScatteredTransformFeedbackCapture()` → `xfbNeedsScatteredCapture`
- `DirectGLES.cpp:1039, :1063` packed stride again (scatter buffer bind)
- `DirectGLES.cpp:1097`, `Managers.cpp:8140, :8155, :8219` `GetTransformFeedbackBufferMode()` → `xfbBufferMode`
- `Managers.cpp:8090` `GetTransformFeedbackVaryingCount()`

*Attributes / uniforms*
- `DirectGLES.cpp:1247` `GetActiveAttributeLocationMask()`
- `DirectGLES.cpp:1276, :1290` `GetAttribType(location)` → `attribTypes`
- `DirectGLES.cpp:3036`, `Managers.cpp:6419, :6572` `GetMaxUniformLocation()` → `maxUniformLocation`
- `DirectGLES.cpp:3038, :3564`, `Managers.cpp:468, :6450, :6589` `GetUniformSamplerOrImageUnitIndex(loc)` → `uniformSamplerOrImageUnitIndex`
- `DirectGLES.cpp:3039`, `Managers.cpp:6423, :6576` `GetUniformType(loc)`
- `Managers.cpp:463, :465, :6579` `GetUniformLocation(name)` → `uniformLocations`
- `Managers.cpp:6421, :6574` `GetUniformName(loc)` → `uniformReflection[...].name`
- **`Managers.cpp:6424` `GetUniformTypeFacts(loc)` → `uniformReflection[...].type` — the only direct
  `TypeFacts` read in either backend** (comment at :6440: "From the OWNED TypeFacts, not from a live
  TType"). It is bound with `const auto&`, so the type name never appears.

*Blocks / SPIR-V / status*
- `DirectGLES.cpp:2734, :3033, :3336, :3374, :3663, :4071`, `Managers.cpp:7321` `GetLinkStatus()` / `GetSpirvStatus()` → `linkStatus` / `spirvStatus`
- `DirectGLES.cpp:3397, :3402, :3444, :3449`, `Managers.cpp:8290, :8293` `GetUBOSize()` → `globalUboScratch.size()`
- `DirectGLES.cpp:3481` `GetUniformBlockBinding(i)` → `uniformBlockBinding`
- `Managers.cpp:6192, :6202, :7341` `GetShaderStorageBlockBindingOverrides()` → `shaderStorageBlockBinding`
- `Managers.cpp:7408` `GetLinkedShaderStages()`; `:7409` `GetGeneratedSpirv()` → `generatedSpirv`;
  `:7430` `GetLinkedShaderSnapshot()`; `:7439` `GetSpirvValidationEnabled()` → `enableSpirvValidation`
- version/identity: `GetLinkVersion` (`DirectGLES.cpp:2798, :3715, :7368`), `GetImageUnitVersion`
  (:2799), `GetBackendStateVersion` (:3335, :3350, :3540), `GetLifetimeId` (:3333, :3349),
  `GetExternalIndex` (many, `Managers.cpp:7212` ff.). These live on `ProgramObject`, **not** in the
  artifacts — they must stay behind.

`ARCHITECTURE.md:310` additionally notes Espryt's `ScatterCapturedRecords` is a read-modify-write on
the *client* shadow and that under split the client keeps the varying/stride tables from the
reflection archive.

### 4.2 DirectVulkan (Magma) — re-derives with **SPIRV-Reflect**

`MG_Backend/DirectVulkan/DirectVulkan.cpp:22` `#include <spirv_reflect.h>`.

The re-derivation lives in `GetProgramResourceCache(const ProgramObject&)` at
**`DirectVulkan.cpp:161–2xx`** (the docs' "around line 161" is exact — the function opens at :161,
the cache lookup at :162). Shape:

- Caches keyed by GL program name: `UnorderedMap<GLuint, ProgramResourceCache> g_programResourceCaches;`
  at `DirectVulkan.cpp:103`; cleared at `:305` (`ClearProgramResourceCaches`); insertion hazard note
  at `:728`.
- `struct BufferVariableResource` `:51–57`, `struct StorageBlockResource` `:58–63`,
  `struct ProgramResourceCache` `:65–81` (`programLifetimeId`, `backendStateVersion`,
  `blockBindingVersion`, `Vector<StorageBlockResource> storageBlocks`,
  `Vector<BufferVariableResource> bufferVariables`).
- Validity key `:171–174`: `GetLifetimeId()` + `GetBackendStateVersion()` +
  `GetBlockBindingVersion()`. On a block-binding-only bump (`:176–190`) it re-applies
  `program.GetShaderStorageBlockBindingOverride(block.name)` **by name** instead of re-reflecting —
  i.e. `LinkArtifacts::shaderStorageBlockBinding` is the authority and spirv-reflect is the
  structure.
- The reflect run itself `:196–...`: `program.GetGeneratedSpirv()` → per module
  `spvReflectCreateShaderModule(spirv.size() * sizeof(Uint), spirv.data(), &module)` (:205),
  `spvReflectEnumerateDescriptorBindings` (:216, :222).
- `NormalizeDescriptorName` `:118–131` strips a `"[0]"` suffix off `type_description->type_name`.
- `AddBufferVariablesRecursive` `:133–159` walks `SpvReflectBlockVariable::members` recursively to
  build the `GL_BUFFER_VARIABLE` list with names/offsets/sizes.

This is exactly the "three consumers of one archive" open question at `ROADMAP.md:81`: Espryt reads
the front-end tables, Magma runs SPIRV-Reflect, and `DirectVulkan.cpp` reflects a second time for
`glGetProgramResource*`. **P0.5 changes none of it** — but note that the archive as shipped
(`LinkArtifacts` alone) does not contain buffer-variable / storage-block reflection, which is why
Magma re-derives; the server can keep doing exactly that from the shipped SPIR-V.

Magma's other `ProgramObject` reads (`UniformManager.cpp`, `ProgramFactory.h/.cpp`,
`VulkanRenderer.cpp`) are all accessor calls; see §3.1 rows 1–3.

---

## 5. Existing `Visit()` / archive / serialization patterns to follow

**There is no `Visit()`, `Archive()`, `Serialize()` or `ForEachField()` in the tree today**
(`grep -rn "Visit(\|ForEachField\|Archive(\|Serialize(\|Deserialize("` over `MobileGL/` returns
nothing). The `Visit()` archive is genuinely new. The two prior-art patterns to imitate:

### 5.1 The X-macro field table — `MG_Pipe/PipeFields.def`

`MobileGL/MG_Pipe/PipeFields.def:9–18` states the contract: one macro per payload in
`MGPipeTypes.h`, listing only the fields that carry **meaning** (padding deliberately absent, because
`MOBILEGL_PIPE_VERIFY` must have zero false positives). Hand-maintained alongside the struct; a field
added without a `.def` entry makes the comparator blind to it.

Shape (`PipeFields.def:83–85`):
```c
#define MGP_FIELDS_MGPProgramDesc(F) \
    F(Cso) F(StageMask) F(GlobalUboSize) F(ReservedNumSamplesOffset) F(SpirvStatus) F(NativeFloat64) \
    F(PointSizeDemoted) F(EnableSpirvValidation) F(Spirv) F(Reflection)
```
and a roll-up list at `PipeFields.def:227` enumerating every payload. Consumed by
`scripts/gen_pipe.py` (docstring at `scripts/gen_pipe.py:9–33`), which emits
`MobileGL/MG_Pipe/generated/{PipeWire,PipeVerify,PipeTables,PipeThunks,PipeFilled,PipeSpanTable,PipeCoverage}.inc`.
Outputs are committed; CI regenerates and fails on a diff. `gen_pipe.py` refuses a catalogue payload
with no field list.

**Recommendation:** the `Visit()` archive is the C++-template analogue of this — but because
`LinkArtifacts` members are `String`/`Vector`/`UnorderedMap`/`std::set` rather than scalars, a
generated `.inc` is less natural than a member-template. The prior art to cite is the
*discipline* (one field table, both directions, a gate that goes red on a missed field), not the
macro mechanics.

### 5.2 The sizeof tripwire — `MG_Pipe/MGPipeTypes.h`

Two idioms, both worth copying:

**(a) The POD assertion macro**, `MGPipeTypes.h:43–45`:
```c
#define MGP_ASSERT_POD(T, Size)                                                                    \
    static_assert(std::is_trivially_copyable_v<T>, #T " must be trivially copyable");              \
    static_assert(sizeof(T) == (Size), #T " changed size; update the wire format and this assertion")
```
used e.g. `MGP_ASSERT_POD(MGPProgramDesc, 192);` at `MGPipeTypes.h:308`. Rationale at
`MGPipeTypes.h:20–22`: "Sizes are asserted rather than merely documented because the wire records
generated from these structs are memcpy'd; a field silently changing width is a protocol break that
no test would otherwise see."

**(b) The named-constant ratchet**, `MGPipeTypes.h:527–539` — this is the exact shape
`MGL_LINKARTIFACTS_SIZE` should take:
```c
#define MGL_RESIDUAL_BLOCK_SIZE 1248
    static_assert(sizeof(ResidualValueBlock) == MGL_RESIDUAL_BLOCK_SIZE,
                  "the residual value block changed size; lower MGL_RESIDUAL_BLOCK_SIZE if a field "
                  "retired, and do not raise it");
```
with a comment block (:527–535) explaining the direction of travel and what makes it a build break.
`MGPipeTypes.h:504–507` adds the caveat that a heterogeneous POD **also** needs member-by-member
`offsetof` assertions, since padding differs across ABIs and the monolith verify harness is blind to
it (both sides are one TU).

**Caveat for `LinkArtifacts`:** `sizeof(LinkArtifacts)` is **not ABI-stable and not
trivially-copyable** — it contains `std::string`, `std::vector`, `ska::flat_hash_map` and `std::set`,
whose sizes differ between libstdc++/libc++/MSVC and between debug/release STL configurations.
`MGL_LINKARTIFACTS_SIZE` therefore cannot be a single literal the way `MGL_RESIDUAL_BLOCK_SIZE` is;
it is a **same-build tripwire** ("did somebody add a field without touching `Visit()`"), not a wire
size. Options: (i) assert per-toolchain values behind `#if`, (ii) assert only under a canonical CI
toolchain (`#if defined(__GLIBCXX__) && !defined(_GLIBCXX_DEBUG)`), or (iii) drop the byte count and
assert a **field count** the `Visit()` template computes at compile time. State whichever you pick in
the header — the ARCHITECTURE wording assumes (i)/(ii).

### 5.3 The `-H` include-closure gate

`ARCHITECTURE.md:425` describes purity gate A: `MG_Remote/Transport/` headers must not include the
front-end umbrella, "纯度门 A 断言 `-H` 输出". **No such script exists yet** — `scripts/` contains
only `check_doc_citations.py`, `format_code.sh`, `gen_pipe.py`, `gen_pipe_dirty_surface.py`,
`gen_protocol.py`, `update-source-file-header.py`. The P0.5 implementer writes the first one, and
`ROADMAP.md:16` requires it be demonstrably red when a `MG_State` include is added back.

Related counter-example already in the tree: `MGPipeTypes.h:24–31` documents its own P0.5 debt in the
file header ("P0.5 DEBT, recorded here so it is impossible to miss") for `#include
<MG_Backend/BackendObject.h>` (:32) and `#include <MG_State/GLState/RenderState/RenderState.h>`
(:33). **Follow that convention**: any include `ProgramArtifacts.h` cannot yet drop gets an explicit
debt paragraph in the file header.

---

## 6. Proposed contents of `ProgramArtifacts.h`

File: `MobileGL/MG_State/GLState/ProgramState/ProgramArtifacts.h`
Namespace: `MobileGL::MG_State::GLState` (**top-level, not nested in `ProgramObject`**).

### 6.1 Includes

```cpp
#pragma once
#include <Includes.h>   // String/Vector/UnorderedMap/Array + GL enums; see debt note below
#include <set>          // std::set<String>, used by LinkArtifacts - NOT in Includes.h
```
Nothing else. Explicitly **not**: `ShaderObject.h`, `SpvcSession.h`, `ProgramLinkTask.h`,
`BufferMetrics.h`, `<spirv_reflect.h>`.

File-header debt paragraph (mandatory, mirroring `MGPipeTypes.h:24–31`): `<Includes.h>` still pulls
glslang (`Includes.h:79–83`), spirv-cross C (`:56`) and Vulkan (`:130`). This header is glslang-free
**by type and by symbol**, which is what the P7 `nm -D | grep glslang` gate measures; a textually
glslang-free closure additionally requires splitting `MG_Util/Types.h` (which itself includes
`<Includes.h>` at :11) and is out of P0.5 scope.

### 6.2 Types, in dependency order

1. **`kInvalidUniformOffset`** — move `static constexpr Uint kInvalidUniformOffset = ~0u;`
   (ProgramObject.h:547) out to a namespace-scope `inline constexpr Uint kInvalidUniformOffset = ~0u;`
   in this header, and leave `static constexpr Uint kInvalidUniformOffset = kInvalidUniformOffset;`…
   no — leave a **`using`-style re-export** in `ProgramObject`:
   `static constexpr Uint kInvalidUniformOffset = MobileGL::MG_State::GLState::kInvalidUniformOffset;`
   so the 9 existing spellings `ProgramObject::kInvalidUniformOffset` (§1.5) keep compiling
   unchanged. This is required: `SpirvArtifacts::reservedNumSamplesOffset` (:1420) uses it as a
   default member initializer.

2. **`struct TypeFacts`** — verbatim move of ProgramObject.h:44–72, comments included. Zero changes;
   already POD, already glslang-free.

3. **`struct ResourceReflection`** — verbatim move of :76–99, plus the four aliases :101–104 moved to
   namespace scope.

4. **`struct XfbVarying`** — verbatim move of :1146–1171. Zero changes.

5. **`struct UniformInitializer`** — **NEW**, the de-glslang'd replacement for
   `glslang::TIntermediate::TUniformInitializer` (`include/glslang/MachineIndependent/localintermediate.h:627–636`):
   ```cpp
   struct UniformInitializer {
       String name;
       Int    basicType  = 0;   // widened glslang::TBasicType, same convention as TypeFacts::basicType
       Int    vectorSize = 1;
       Int    matrixCols = 0;
       Int    matrixRows = 0;
       Int    arraySize  = 1;
       Vector<Int64>  intValues;    // was std::vector<long long>
       Vector<Double> floatValues;  // was std::vector<double>
   };
   ```
   `Int64` = `MG_Util/Types.h:31`, `Double` = `:35`. The widening of `basicType` to `Int` matches the
   precedent already set by `TypeFacts::basicType` (ProgramObject.h:71) and
   `TypeFacts::layoutMatrix` (:68).

6. **`struct LinkArtifacts`** — moved from :1210–1393, with **exactly two member re-types**:
   - `:1215 SharedPtr<glslang::TProgram> program;` → **stays in `ProgramObject.h`**, see §6.3.
   - `:1282 Vector<glslang::TIntermediate::TUniformInitializer> uniformInitialValues;` →
     `Vector<UniformInitializer> uniformInitialValues;`
   All other 51 members move verbatim.

7. **`struct SpirvArtifacts`** — verbatim move of :1409–1449, once `kInvalidUniformOffset` is in
   scope (item 1).

8. **The archive** — `Visit()` as a member template on each of the five, plus the tripwires:
   ```cpp
   template <class V> void TypeFacts::Visit(V&& v) { v(isArray); v(isSizedArray); /* 20 fields */ }
   ```
   with a matching `static_assert` per struct. The `Visit()` must be `const`-overloaded (or take
   `auto&& self`, C++23 deducing-this — the project is C++23, see `Includes.h:47`
   `__cplusplus >= 202302L`) so one field list serves both write (const) and read (mutable), which is
   the "一份字段表服务序列化两个方向" requirement of `ARCHITECTURE.md:259`.

### 6.3 What must **stay** in `ProgramObject.h`

- `ProgramObject::MAX_UNIFORM_LOCATIONS` (:39) — `static_cast<Int>(glslang::TQualifier::layoutLocationEnd)`.
  Named by `MG_Impl/GLImpl/Getter/GL_Getter.cpp:1945`, `ProgramLinkTask.cpp:1251`. Stays glslang-bound;
  it is not one of the five types. (If P7 later needs it server-side, replace the RHS with the literal
  and `static_assert` against glslang on the client — flag but do not do this in P0.5.)
- **`LinkArtifacts::program`** — the `SharedPtr<glslang::TProgram>`. Two options; pick (A):
  - **(A) Keep it inside `LinkArtifacts` but change nothing about the header's purity by making the
    field's type a forward-declared incomplete `glslang::TProgram`.** `SharedPtr<T>` of an incomplete
    `T` is legal to declare, copy and move; only construction and destruction need completeness, and
    both happen in `ProgramLinkTask.cpp` (the only writer: :646, :678, :764, :846, :877) and
    `ProgramObject.cpp:499–501` (whole-struct reset). **But** `LinkArtifacts`'s own implicit
    destructor must destroy the `shared_ptr`, and that is instantiated wherever a `LinkArtifacts` is
    destroyed — including in `ProgramInterface.cpp` and `ProgramTranslationCache.cpp`. A
    `shared_ptr`'s destructor does **not** require `T` complete (the deleter was type-erased at
    construction), so this works: `namespace glslang { class TProgram; }` at the top of
    `ProgramArtifacts.h` is sufficient. **Verify by compiling with a hand-edited `Includes.h` that
    hides glslang**, because this is the single highest-risk claim in this plan.
  - (B) Move `program` out of `LinkArtifacts` onto `ProgramLinkTask` and pass it beside the
    artifacts. Cleaner for the server, but touches ~30 call sites in `ProgramLinkTask.cpp`
    (§1.4 readers) and `ProgramSpirvTask.cpp:127, :234` — a bigger diff than "纯搬移" allows.
    Record as the P7 follow-up if (A)'s incomplete-type trick fails on any toolchain.
- Everything else: the ~120 accessors, `Artifacts()`/`Spirv()` join gates (:1682–1698),
  `EnsureLinkJoined`/`EnsureSpirvJoined` (:1633, :1648), `ResetLinkArtifacts` declaration (:1457,
  defined `ProgramObject.cpp:332`), `LinkedShaderRef` (:150–154), `PendingUniformWrite` (:1658–1663),
  `BackendHashMemoSlot` (:1767–1771), and all `m_*` members (:1712–1801).

### 6.4 Forwarding shape

`ProgramObject.h` keeps compiling every existing spelling (`ProgramObject::LinkArtifacts`,
`ProgramObject::TypeFacts`, …) via in-class `using` re-exports, so **no other file changes at all**
in P0.5 (this is what makes the exit gate "全套测试逐名不变(纯搬移)" achievable):

```cpp
// ProgramObject.h, replacing lines 41-104, 1144-1449
#include <MG_State/GLState/ProgramState/ProgramArtifacts.h>
...
class ProgramObject {
public:
    using TypeFacts            = MobileGL::MG_State::GLState::TypeFacts;
    using ResourceReflection   = MobileGL::MG_State::GLState::ResourceReflection;
    using UniformReflection    = ResourceReflection;
    using BlockReflection      = ResourceReflection;
    using PipeInputReflection  = ResourceReflection;
    using PipeOutputReflection = ResourceReflection;
    using XfbVarying           = MobileGL::MG_State::GLState::XfbVarying;
    using UniformInitializer   = MobileGL::MG_State::GLState::UniformInitializer;
    using LinkArtifacts        = MobileGL::MG_State::GLState::LinkArtifacts;
    using SpirvArtifacts       = MobileGL::MG_State::GLState::SpirvArtifacts;
    static constexpr Uint kInvalidUniformOffset = MobileGL::MG_State::GLState::kInvalidUniformOffset;
    ...
```
`ProgramObject.h` also **drops `#include <MG_Util/ShaderTranspiler/SpvcSession.h>` (:14)** — it names
no spvc/SpvReflect symbol (verified by grep). Keep `#include "ShaderObject.h"` (:11) and
`#include <MG_Util/Metrics/BufferMetrics.h>` (:13); both are genuinely used by `ProgramObject`
itself. Removing :14 is the one purity win available to `ProgramObject.h` in this phase; the glslang
dependency stays because `MAX_UNIFORM_LOCATIONS` and `SharedPtr<glslang::TProgram>` are there.

Note the alias `using TypeFacts = ...::TypeFacts;` inside a class whose enclosing namespace already
declares `TypeFacts` is legal (member alias shadows), but the qualified RHS is mandatory or it
self-references. Spell it fully qualified as above.

### 6.5 Where the `Visit()` archive's two directions live

Serializer/deserializer bodies do **not** belong in `ProgramArtifacts.h` (that would drag whichever
byte-stream helper into every `Core.h` consumer). Put them where `MGPProgramDesc::Reflection`'s blob
is built and parsed — the natural home is a new `MG_Pipe/` or `MG_Remote/` TU that includes
`ProgramArtifacts.h` and nothing else from `MG_State`. `ProgramArtifacts.h` carries only the field
tables (`Visit`) and the tripwires.

---

## 7. Risk list

**R1 — `Includes.h` is not glslang-free (HIGH, design-level).** `Includes.h:79–83` pulls all five
glslang headers unconditionally. Following the ARCHITECTURE literally ("只 include `<Includes.h>` 与
容器") yields a header that is glslang-free *by type* but not *by preprocessed text*. Decide and
document which gate the `-H` assertion enforces before writing it, or the gate will be written to a
criterion the header cannot meet. See §2.

**R2 — `sizeof(LinkArtifacts)` is not a stable number.** `String`/`Vector`/`ska::flat_hash_map`/
`std::set` sizes vary by STL and by `_GLIBCXX_DEBUG`. `MGL_LINKARTIFACTS_SIZE` cannot be a bare
literal the way `MGL_RESIDUAL_BLOCK_SIZE` (`MGPipeTypes.h:536`) is. Also note the project builds on
Windows/MSVC (this worktree), Android/NDK-libc++ and Linux/libstdc++ — three different answers. See
§5.2 for the three mitigation options.

**R3 — `SharedPtr<glslang::TProgram>` by value in `LinkArtifacts` (ProgramObject.h:1215).** This is
the single member that references glslang by value in the archived types. The incomplete-type plan
(§6.3 option A) is sound in principle but must be compile-verified on all three toolchains; MSVC in
particular has historically been strict about `shared_ptr<Incomplete>` in aggregates with implicit
special members. Fallback is §6.3 option B. Related: the server will deserialize a `LinkArtifacts`
whose `program` is always null — assert that at the deserializer, matching the existing insert-time
assertion documented at `ProgramTranslationCache.h:42–43` ("`link.program` is null by construction …
Asserted at insert").

**R4 — `glslang::TIntermediate::TUniformInitializer` re-typing breaks a comparison.**
`MG_State/GLState/ProgramState/ProgramObject.cpp:180–183` compares `init.basicType` against
`glslang::EbtFloat` / `EbtFloat16` / `EbtDouble` / `EbtInt` / `EbtUint` / `EbtBool`. With
`basicType` widened to `Int` this still compiles (enum→int promotion) but the file keeps a glslang
dependency; that is fine (it is a client TU) as long as the *header* does not. Also
`ProgramLinkTask.cpp:612–618` builds the vector via `std::find_if` on `.name` and copies the whole
struct — a plain type substitution, no field-order sensitivity.

**R5 — `<set>` (MEDIUM, will bite immediately).** `LinkArtifacts` uses `std::set<String>` at :1331
and :1336 and no header on the path includes `<set>` deliberately. Add it explicitly. See §1.4.

**R6 — `kInvalidUniformOffset` ordering (MEDIUM).** Declared at ProgramObject.h:547, *used* as a
default member initializer at :1420. Moving `SpirvArtifacts` without moving the constant is an
immediate compile error, and moving the constant without a re-export breaks 9 call sites across three
modules (§1.5). Handle both halves in one commit.

**R7 — ODR / aliasing (LOW but real).** Injecting `using TypeFacts = ...::TypeFacts;` inside
`ProgramObject` while a namespace-scope `TypeFacts` exists in the same namespace is fine, but
`ProgramLinkTask.cpp:48` writes
`static MobileGL::MG_State::GLState::ProgramObject::TypeFacts MakeTypeFacts(...)` — the
fully-qualified-through-the-class spelling. That resolves through the alias unchanged. Watch for any
place that forward-declares one of these types instead of including the header: grep found none, but
re-check after the move. Also: **do not leave a second definition behind** — the classic P0.5 failure
is copying the struct into the new header and forgetting to delete it from `ProgramObject.h`, which
compiles (different scopes: `GLState::TypeFacts` vs `GLState::ProgramObject::TypeFacts`) and silently
splits the type in two.

**R8 — pImpl / join-gate boundary (LOW, but the invariant to protect).** `LinkArtifacts` and
`SpirvArtifacts` become namespace-scope types, so anyone can now *declare* one without going through
`ProgramObject::Artifacts()`. The I5 invariant (ProgramObject.h:1187–1192, :1622–1631) is protected by
`m_artifacts` being **private**, not by the type being nested — so the invariant survives the move.
Say so in a comment at the top of `LinkArtifacts` in the new header, because the current comment
explains I5 in terms of "the member below is private", and that member no longer lives next to the
struct.

**R9 — `ProgramTranslationCache.cpp:40` takes `sizeof(ProgramObject::ResourceReflection)`.** It feeds
a byte budget, not an allocator ("Approximate on purpose", `ProgramTranslationCache.cpp:46–48`), so a
size change is harmless — but it is a second place that must keep compiling after the alias move.

**R10 — the "7 includers" figure is wrong.** Verified: `ProgramObject.h` has **8** non-self includers
(9 with `ProgramObject.cpp`); `ShaderObject.h` has exactly 7; `SpvcSession.h` has 18. Budget P0.5
against the real numbers (§3), and note that if `ProgramObject.h` drops its `SpvcSession.h` include
(§6.4) the `SpvcSession.h` count falls to 17 while every remaining includer is a
ShaderTranspiler/test/benchmark TU that legitimately wants spirv-cross.

**R11 — fan-out.** `MG_State/GLState/Core.h:16` → `ProgramState.h:12` → `ProgramObject.h` means both
backends and all of `MG_Impl` recompile on any `ProgramObject.h` touch, and *nothing in either
backend directly includes it*. That is the reason the extraction buys real build-time and symbol
separation later — and the reason the `-H` closure assertion should be run against
`MG_Backend/DirectGLES/Managers.cpp` and `MG_Backend/DirectVulkan/DirectVulkan.cpp` (the two big
transitive consumers), not only against `ProgramArtifacts.h` itself.

---

## 8. Quick reference — file inventory

| path | why it matters |
|---|---|
| `MobileGL/MG_State/GLState/ProgramState/ProgramObject.h` | all five types live here today (1803 lines) |
| `MobileGL/MG_State/GLState/ProgramState/ProgramObject.cpp` | `ResetLinkArtifacts` :332, `ApplyUniformInitialValues` :143–215, `kInvalidUniformOffset` uses :198/:273 |
| `MobileGL/MG_State/GLState/ProgramState/ShaderObject.h` | 319 lines; 7 includers; the glslang/`ShaderCompileTask` path |
| `MobileGL/MG_Util/ShaderTranspiler/SpvcSession.h` | 18 includers; pulls `<spirv_reflect.h>` :11 |
| `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.h` | `artifacts` :80, `SpirvHandoff` :92–158 |
| `MobileGL/MG_State/GLState/ProgramState/ProgramLinkTask.cpp` | sole writer of `LinkArtifacts.program`; `MakeTypeFacts` :48, `MakeResourceReflection` :79 |
| `MobileGL/MG_State/GLState/ProgramState/ProgramSpirvTask.h` | `artifacts` :46 |
| `MobileGL/MG_State/GLState/ProgramState/ProgramTranslationCache.h` | `ProgramTranslationResult` :49–52 — the existing "whole archive as a value" precedent |
| `MobileGL/MG_Impl/GLImpl/Program/ProgramInterface.cpp` | only consumer that names `LinkArtifacts` outside `ProgramState/` |
| `MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp` | `TypeFactsRef` alias :27–28 |
| `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`, `Managers.cpp` | Espryt accessor call sites (§4.1) |
| `MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp` | SPIRV-Reflect re-derivation :51–305 |
| `MobileGL/MG_Pipe/MGPipeTypes.h` | `MGPProgramDesc` :296–308; the two tripwire idioms :43–45 and :527–539; the P0.5 debt-note convention :24–33 |
| `MobileGL/MG_Pipe/PipeFields.def` | the X-macro field-table prior art :9–18, :83–85, :227 |
| `MobileGL/MG_Pipe/PipeCalls.def:102` | `X(CreateShaderState, MGPProgramDesc, kCtxCso, kHasBlob)` |
| `scripts/gen_pipe.py` | the generator that consumes the `.def` files; `--check` mode is the CI gate |
| `MobileGL/Includes.h` | :79–83 glslang, :56 spirv-cross, :17 `<map>` (no `<set>`), :149 `MG_Util/Types.h` |
| `MobileGL/MG_Util/Types.h` | every alias the new header uses (:21–123) |
| `include/glslang/MachineIndependent/localintermediate.h:627–636` | `TUniformInitializer` — the struct to re-declare |
| `docs/Disaggregated/ARCHITECTURE.md:256–264` | section 7 |
| `docs/Disaggregated/ROADMAP.md:16` | P0.5 row |
