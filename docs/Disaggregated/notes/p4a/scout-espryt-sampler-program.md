# P4a scout — Espryt's sampler / image / program surface

Tree read: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg` at
`feat/disaggregated@37da3c3a` (P3a closed, ID-24). Every line number below was opened in that tree.
Paths are repo-relative under `MobileGL/` unless written out in full.

House shape assumed from `~/w7/notes/p3a/BRIEF-P3A.md` §A/§C and `INTEGRATOR-DECISIONS.md` ID-1..24:
a phase is 4-5 packages (contract → wire → client → backend → gates), the contract commit is tagged
and frozen, G1 (pull build symbol-identical, admitted-resize set EMPTY) is the hardest gate, and every
backend edit lives inside `#if MOBILEGL_PIPE_PUSH`.

---

## 0. The one-paragraph orientation

P3a converted **one** family (buffers) to a handle-addressed op table and **one** CSO (vertex
elements) to an applier record. P4a's sampler/program half is structurally *easier* in one way and
*harder* in three:

* **Easier**: the payloads already exist and are already in the verify comparator's list.
  `MGPSamplerDesc` (`MG_Pipe/MGPipeTypes.h:283-287`), `MGPSamplerView` (`:293-304`),
  `MGPTextureParams` (`:307-318`), `MGPProgramDesc` (`:323-335`), `MGPBoundView`/`MGPSamplerViews`
  (`:432-444`), `MGPSamplerStates` (`:447-451`), `MGPImageView`/`MGPShaderImages` (`:453-468`),
  `MGPGlobalConstants` (`:507-513`). All twelve are already in `MGP_VERIFY_PAYLOAD_LIST`
  (`MG_Pipe/PipeFields.def:314-336`) with their `MGP_FIELDS_*` tables (`:74-88`, `:108-134`).
  The catalogue rows exist too and are `kNone`/`kHasBlob`/`kVarTail` today
  (`MG_Pipe/PipeCalls.def:104-110`, `:117-119`, `:127`, `:134-135`).
* **Harder 1**: sampler state has **two** carriers in MobileGL (a `SamplerObject` bound to a unit,
  and a `SamplerObject` *owned by every texture object*), and Espryt pushes them through **two
  different driver entry-point families** (`glSamplerParameter*` vs `glTexParameter*`), with two
  independent caches. §1.
* **Harder 2**: the shader CSO's wire form is SPIR-V + the whole `LinkArtifacts`+`SpirvArtifacts`
  archive (`ARCHITECTURE.md:259-266`), but the archive **has no serializer yet** — only the field
  tables. §3.9.
* **Harder 3**: three of the four things Espryt's program twin is keyed on are **not program state
  at all** (draw-FBO clamp masks, fragColor broadcast count, live image-unit formats, patch
  parameters). That is the `st_variant`-style lazy specialisation `ARCHITECTURE.md:264` names, and
  it is what stops `create_shader_state` from being self-contained. §3.4.

---

## 1. Samplers

### 1.1 `SamplerParameters` and `BorderColorForm` — the value type

`MG_Pipe/MGPipeValueTypes.h:462-486`. 15 members, `sizeof == 100`, trivially copyable
(`:615` static_assert; pinned again in `MG_Test/Pipe/PipeCatalogueTest.cpp:131` and `:138`):

```
wrapS/wrapT/wrapR   SamplerWrapMode (enum class, 4 B each)   default Repeat
minFilter           SamplerFilterMode                        default Nearest
magFilter           SamplerFilterMode                        default Linear
mipmapMode          SamplerMipmapMode                        default Linear
minLod/maxLod       Float                                    -1000 / +1000
lodBias             Float                                    0
maxAnisotropy       Float                                    1
compareFunc         SamplerCompareFunc                       LessEqual (NOT Never - table 23.18)
compareMode         SamplerCompareMode                       None
borderColor         FloatVec4                                (0,0,0,0)
borderColorI        IntVec4
borderColorUI       UintVec4
borderColorForm     BorderColorForm : Uint8                  Float
```

`BorderColorForm` is declared at `:449-460` with the rationale verbatim: all three representations
are *always* numerically populated, so the value alone cannot say which driver entry point to use.
`MGPipeTypes.h:279-282` says the struct "crosses byte for byte INCLUDING borderColorForm".

> **TRAP (P4a must fix in the contract commit).** `SamplerParameters` has **3 bytes of trailing
> padding** (96 bytes of members + 1 byte `borderColorForm` = 97, padded to 100). There is **no
> `MGP_FIELDS_SamplerParameters`** in `PipeFields.def` (grep: only `RenderStateParameters` at `:233`
> and `PixelStoreParameters` at `:253` of the value structs), and `SamplerParameters` is **not** in
> `MGP_VERIFY_PAYLOAD_LIST`. `MGPSamplerDesc::Parameters` is an `MGPBlobRef`, so under
> `MOBILEGL_PIPE_VERIFY` the blob is compared as bytes and the padding false-differs; and any
> content hash taken over those 100 bytes hashes uninitialised padding, which would mint a fresh
> sampler CSO per call. This is exactly the shape the P3a brief's G7 negative control exists for on
> the vertex side. Add the field table + the verify-list row, and hash/compare field-wise.

### 1.2 The `SamplerCso` slot table (P2)

`MG_Backend/DirectGLES/Managers.h:2228-2229`:

```cpp
extern TwinRegistry<MG_State::GLState::SamplerObject, BackendSamplerObject, MG_Pipe::MGPipeKind::SamplerCso>
    g_backendSamplerObjects;
```

defined at `Managers.cpp:10583`. `TwinRegistry` is the alias at `Managers.h:1188-1195` that swallows
the kind parameter in the pull build (G1); the push arm holds a
`BackendSlotTable<SamplerObject, BackendSamplerObject, SamplerCso>` member at `Managers.h:538`, and
`StateBackendObjectRegistry::GetOrCreate` dispatches to it at `Managers.h:325-332` when
`EsprytSlotTablesEnabled()` (`SlotTables.h:132-135`).

The handle-keyed overloads P4a needs already exist and are **unused for this kind**:
`BackendSlotTable::GetOrCreate(MGPipeHandle)` (`SlotTables.h:278-326` — forward Gen = recycle+reset,
**backward Gen = refuse**, `kMaxHandleSlot = 1<<20` at `:148`), `FindByHandle` (`:376-382`),
`ReleaseByHandle` (`:350-361`), `LiveGenAt` (`:331-335`).

**Death today is the shared notice, not a call.** `Managers.cpp:199-247`
`OnFrontendStateObjectDestroyed` switches on kind; `MGPipeKind::SamplerCso` →
`SamplerImpl::g_backendSamplerObjects.DestroyByLifetimeId(lifetimeId)` (`Managers.cpp:211-213`),
which forwards to `BackendSlotTable::OnFrontendObjectDestroyed` (`Managers.h:442-446`,
`SlotTables.h:416-430`). The raiser is `SamplerObject`'s push-only destructor
(`MG_State/GLState/SamplerState/SamplerObject.h:18-24` — declared only under `MOBILEGL_PIPE_PUSH`
so the pull build keeps its implicit dtor, G1). The ops table is installed at `Managers.cpp:247-250`
+ `:326`. **P4a decision owed**: does `SamplerCso` follow buffers (its own `DeleteSamplerState` call,
client frees the slot — the `default:` comment at `Managers.cpp:236-246` spells the ordering rule) or
does it keep the shared notice? The catalogue already has `DeleteSamplerState`
(`PipeCalls.def:105`), and the P3a `VertexElementsCso` case (`Managers.cpp:225-236`) is the
precedent for the **hybrid**: the client speaks the whole death *and* the backend keeps a notice
consumer for the driver object, with a documented double-release that is a no-op.

### 1.3 `BackendSamplerObject` — the twin

Declared `Managers.h:2205-2223`, defined `Managers.cpp:10416-10584`.

| member | line | note |
|---|---|---|
| `m_backendSamplerId` | `Managers.h:2218` | `glGenSamplers` in ctor `Managers.cpp:10417-10429` |
| `m_contextGeneration` | `:2219` | vs `g_backendContextGeneration`; dtor refuses to delete a foreign name |
| `m_isInitialized` | `:2220` | |
| `m_cacheSamplerParameters` | `:2221` | the whole 100-byte struct, as the last-pushed shadow |
| `m_syncedSamplerVersion` | `:2222` | `Uint16`, vs `SamplerObject::GetVersion()` |

`SyncToBackend` (`Managers.cpp:10451-10556`): version early-out at `:10461-10465`; then a
per-field diff against `m_cacheSamplerParameters`:

* min-filter + mipmap-mode fused, through `ResolveBackendMinFilter(params, ShouldAvoidSamplerMipmapMinFilterOnAngleLlvmpipe())`
  (`Managers.cpp:10482-10490`; the resolver is `Managers.cpp:100-117` and collapses the four
  `*_MIPMAP_*` filters to `GL_NEAREST`/`GL_LINEAR` on ANGLE-on-llvmpipe);
* mag filter `:10491-10497`;
* `SYNC_SAMPLER_PARAM_IF_CHANGED` macro (`:10475-10480`) for wrapS/T/R, compareFunc, compareMode
  (`:10498-10502`);
* minLod / maxLod `:10503-10510`;
* maxAnisotropy behind `g_GLESCapabilities.SupportsTextureFilterAnisotropy`, cache updated even when
  the call is skipped `:10511-10517`;
* **border colour**, `:10518-10553`: gated on `SupportsTextureBorderClamp`, branches on
  `borderColorForm` to `glSamplerParameterIiv` / `Iuiv` / `fv`, with the entry-point null checks;
  the redundancy filter compares **all four** (float, int, uint, form) `:10518-10521`.
* **`lodBias` is never pushed here.** ES has no `GL_TEXTURE_LOD_BIAS`; it is folded into the shader
  as a uniform — see §3.5.

`Bind(unit)` (`:10558-10566`) dedups against `g_boundSamplersCache[unit]` and calls `glBindSampler`.
`UnbindSampler(unit)` (`:10575-10581`) is the symmetric zero-bind. Destructor
(`:10431-10449`) scrubs **every** row of `g_boundSamplersCache` that names `this` before deleting —
the recycled-heap-address defence that `{slot,gen}` is supposed to retire.

### 1.4 The unit shadow

`Array<BackendSamplerObject*, TextureState::MAX_TEXTURE_IMAGE_UNITS> g_boundSamplersCache`
— declared `Managers.h:2226-2227`, defined `Managers.cpp:10582`. `MAX_TEXTURE_IMAGE_UNITS` is **192**
(`MGPipeTypes.h:429-431` states it: "one Array of MAX_TEXTURE_IMAGE_UNITS = 192", merged unit space,
no stage dimension — which is why `PipeCalls.def:194-196` deliberately drops the stage dimension of
`set_sampler_views`).

This shadow is a **raw twin pointer per unit** and is read by three memos as a proof-of-driver-state
(§1.5, §1.6). Under a split it cannot be a pointer; it becomes the applier's own per-unit
`MGPipeHandle` array. That is the single largest mechanical change on the sampler side.

### 1.5 Per-unit sampler walk — `BindCurrentUnitSamplers`

`MG_Backend/DirectGLES/DirectGLES.cpp:3593-3626`, called last from `BindCurrentTextures`
(`DirectGLES.cpp:3752`).

Two memos stack here:

1. **`UnitSamplerLookupMemo`** (`DirectGLES.cpp:3522-3535`), one row per unit (192 rows). Push arm
   stores `MGPipeHandle frontendHandle` (`:3525`); legacy arm stores a `WeakPtr<SamplerObject>`
   under `MOBILEGL_PIPE_LEGACY_MEMOS` (`:3528-3530`). `ResolveUnitSamplerBackend`
   (`:3537-3572`): push arm probes `g_backendSamplerObjects.HandleOf(sampler)` then `FindByHandle`
   (`:3543-3556`); legacy arm uses `OwnerEquals` + `Find` (`:3558-3568`). **A miss is never cached**
   (`:3550-3553`) because the program pass creates the twin later in the same draw.
   `SlotTables.h:556-561` records that this caller *thrashes* the table's single-entry
   lifetime-id memo (a different sampler per unit) — a known, unpaid perf item.
2. **The walk memo** (`DirectGLES.cpp:3588-3593`): `g_unitSamplerWalkContextId`,
   `…Epoch`, `…MaxUnit`, `…ContextGeneration`, `…Valid`, plus
   `g_unitSamplerWalkRows` — a **full 192-entry copy of `g_boundSamplersCache`**. The replay gate
   (`:3597-3604`) is keys-equal **AND** `memcmp` of the first `maxTouchedUnit+1` rows. The
   invalidation argument is written out at `:3575-3587`.

The walk itself (`:3605-3616`): for every unit ≤ `maxTouchedUnit`, take
`MGB_CTX->GetTextureUnitObject(unit).GetSamplerObject()`; bind its twin, or `UnbindSampler(unit)`.

### 1.6 `SamplerPassMemo` — the program-side sampler pass

Declared `Managers.h:1913-1924` (with the six-bullet invalidation enumeration at `:1895-1912`),
member at `Managers.h:2117`, accessor `Managers.h:2001`.

```cpp
struct SamplerPassMemo {                      // Managers.h:1913
    static constexpr SizeT kMaxEntries = 16;  // :1914
    Bool valid; Uint8 count;
    Uint64 contextId, unitBindingsEpoch, samplingGeneration;
    Uint32 backendStateVersion;  Uint textureContextGeneration;
    Array<Uint8, 16> units;                       // :1922
    Array<BackendSamplerObject*, 16> rows;        // :1923 - driver-state proof
};
```

Consumed in `BindCurrentProgramWithResources` at `DirectGLES.cpp:3922-4038`: the clean test is the
five keys (`:3936-3941`) plus a per-entry row compare against `g_boundSamplersCache`
(`:3942-3950`). A pass touching >16 units, or an out-of-range unit, simply never memoises
(`:3956-3966`). Reset on relink at `Managers.cpp:9258`.

The pass body (`:3954-4030`) does five things per `SamplerUniformBinding`:

1. resolve the frontend unit — `currentProgram->GetUniformSamplerOrImageUnitIndex(frontendLocation)`
   (`:3958-3960`);
2. `glUniform1i(backendLocation, unit)` if it moved (`:3971-3974`);
3. the **LOD-bias uniform** (`:3982-4000`) — sampler-object bias wins, else the *texture's own*
   sampler's bias, looked up through `SamplerUniformTextureTarget(uniformType)`
   (`DirectGLES.cpp:3553`ff mapping table, i.e. `DirectGLES.cpp` right after
   `NeedsRawDepthFetchSampler`);
4. the **raw-depth-fetch substitution** (`:4001-4010`);
5. otherwise `SyncToBackend` + `Bind(unit)` on the unit's sampler object, creating the twin if
   `ResolveUnitSamplerBackend` missed (`:4011-4026`), or `UnbindSampler(unit)` (`:4027-4029`).

**This is the only site in Espryt that ever creates a `BackendSamplerObject`**
(`DirectGLES.cpp:4014-4020`) — there is no eager `glGenSamplers` at `glGenSamplers` time.
`create_sampler_state` under P4a inverts that: the CSO is minted and emitted at the frontend call,
and the draw path only binds. Say so in the brief; it changes when `glGenSamplers` runs on the
driver and therefore what `MGLOG_E_ONCE("Failed to generate sampler object.")` at
`Managers.cpp:10424` can report.

### 1.7 The raw-depth-fetch sampler

`DirectGLES.cpp:61-62` (two file-static `SharedPtr`s), `:207-219` `GetRawDepthFetchSampler()`,
`:221-235` `NeedsRawDepthFetchSampler()`.

It **constructs a frontend `MG_State::GLState::SamplerObject(0)`** inside the backend
(`:209-214`) purely to reuse `BackendSamplerObject::SyncToBackend`'s signature — nearest/nearest,
no mipmaps, compare mode off, compare func Always. `ARCHITECTURE.md:322` names it explicitly:

> Espryt 的小号同类：`g_rawDepthFetchSamplerState` → 后端原生 sampler。

so P4a is expected to replace it with a backend-native sampler (a `glGenSamplers` id + a literal
parameter push), not to route it through a CSO. `NeedsRawDepthFetchSampler` reads
`IsDepthFormatInternalFormat(textureFormat)` and three sampler params (compareMode, minFilter,
mipmapMode, magFilter) — under a split those come from the **resolved sampler view + the sampler
CSO's parameters**, both of which the applier will hold. `ARCHITECTURE.md:206` already assigns this
post-processing to the **server**, acting on the already-resolved set:

> 两处后端特定后处理留在 server、作用于已解析集合：Espryt 的 raw-depth-fetch sampler 替换、Magma 的 feedback-loop 检测。

### 1.8 Texture-object sampling state — the *second* carrier

Every `ITextureObject` owns a `SamplerObject` (`GetSamplerObject()`), and Espryt pushes it with
`glTexParameter*` in `BackendTextureObject::SyncBuiltinSamplerToBackend`
(`Managers.cpp:6671-6781`):

* version gate on `samplerObject->GetVersion()` **plus** `m_forceSamplerResync`
  (`:6683-6690`; the flag's rationale is `Managers.h:1508-1519` — a driver re-mint loses every
  pushed parameter, and an incomplete texture samples (0,0,0,1));
* multisample targets take **no** parameters at all — the cache is stamped and the function returns
  (`:6706-6710`);
* min/mag filter through the same `ResolveBackendMinFilter`, but with `IsAngleLlvmpipeRenderer()`
  rather than the sampler-object predicate (`:6732-6744`) — **an asymmetry to preserve**;
* `SYNC_TEX_SAMPLER_PARAM_IF_CHANGED` (`:6719-6731`) for wrapS/T, wrapR **only when
  `SupportsWrapR(targetInternal)`** (`:6752-6757`), compareFunc, compareMode;
* minLod/maxLod/maxAnisotropy `:6758-6776`;
* **no border colour here.** The texture's border colour is pushed from
  `SyncTextureParamsToBackend` instead (`Managers.cpp:6911-6954`), reading
  `stateTextureObject->GetBorderColorForm()/GetBorderColor()/…I()/…UI()` and comparing against four
  separate members `m_cacheBorderColor{,I,UI,Form}` (`Managers.h:1488-1493`). That split matters:
  the border colour is *sampler* state per GL 4.6 table 23.18 (`MGPipeValueTypes.h:478-483`) but
  Espryt pushes it on the **texture params** version, not the sampler version.

So a P4a `MGPSamplerDesc` covers the *sampler object* half; the texture's own sampling state is
`set_texture_params` (`MGPTextureParams`, `MGPipeTypes.h:307-318`) **plus** a second sampler CSO or
an inline parameter block — and today `MGPTextureParams` carries `BaseLevel/MaxLevel/Swizzle/
DepthStencilMode/ForceResync/MinLod/MaxLod/LodBias` and **no filter, no wrap, no border colour, no
compare mode**. That is a real gap between the payload and what
`SyncBuiltinSamplerToBackend` + the border block actually push. Resolve it in the contract commit —
either widen `MGPTextureParams` (it is 32 B today, `MGP_ASSERT_POD` at `:318`) or give every
texture object an implicit sampler CSO.

### 1.9 Seamless cube maps — a non-item, state it so nobody looks for it

`GL_TEXTURE_CUBE_MAP_SEAMLESS` is **not** sampler state in this codebase. It is
`RenderStateParameters::TextureCubeMapSeamlessEnabled` (`MGPipeValueTypes.h:305`, packed into the
alignment hole so `sizeof(RenderStateParameters)` stays 1168 — `:292-303`), lives in the **P1
pipeline chunk** (`MG_Pipe/MGPipeRenderStateSpans.h:64-67`), and therefore already crosses as part
of `create_render_state`/`bind_render_state` since P2. `PipeApply.cpp:72` and `:370` carry it in the
capability list. **`grep -rn CubeMapSeamless MG_Backend/` returns nothing** — no backend reads it
(ES 3.0+ cube sampling is unconditionally seamless), and the frontend answers `glIsEnabled` from
`RenderState.cpp:340` / `:420`. Nothing for P4a to do.

### 1.10 Complete list of sampler-state reads from backend code

| site | file:line | reads |
|---|---|---|
| `DirectGLES.cpp:227` | raw-depth probe | `samplerObject->GetAllSamplerParameters()` (compareMode, minFilter, mipmapMode, magFilter) |
| `DirectGLES.cpp:3607` | unit walk | `GetTextureUnitObject(unit).GetSamplerObject()` |
| `DirectGLES.cpp:3986` | LOD bias | `samplerObject->GetLodBias()` |
| `DirectGLES.cpp:3993-3995` | LOD bias fallback | `boundTexture->GetSamplerObject()->GetLodBias()` |
| `DirectGLES.cpp:4004-4008` | raw-depth gate | unit sampler, else `texture2D->GetSamplerObject()`; `texture2D->GetFormat()` |
| `DirectGLES.cpp:3468-3474` | completeness | `textureUnit.GetSamplerObject()` else `textureObject->GetSamplerObject()`, then `MG_State::GLState::SamplesAsIncompleteTexture` (`MG_State/GLState/TextureState/TextureObject.cpp:489-512`) |
| `Managers.cpp:6705` | texture built-in sampler | `samplerObject->GetAllSamplerParameters()` |
| `Managers.cpp:6683` | version gate | `samplerObject->GetVersion()` |
| `Managers.cpp:10473` | sampler object | `stateSamplerObject->GetAllSamplerParameters()` |
| `Managers.cpp:10461`,`:10471`,`:10502` | ids/versions | `GetVersion()`, `GetExternalIndex()` |
| `Managers.cpp:6925`,`:6932`,`:6937`,`:6942` | border | `GetBorderColorForm/I/UI/…` on the **texture object** |
| `Managers.cpp:4838` | reset | `m_cacheSamplerParameters = SamplerParameters{}` (on backend texture re-mint) |

`DirectVulkan` uses the same helpers (`MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:515`,
`:828`, `:857`, `:1676`) — a reminder that `SamplesAsIncompleteTexture`/`IsUndefinedDefaultTexture`
resolution is shared and, per `ARCHITECTURE.md:206`, moves **client-side** in P4a
("~40 行搬迁"), leaving only the two backend-specific post-processings on the server.

---

## 2. Images (`set_shader_images`' counterpart)

### 2.1 The payload that exists

`MGPImageView` (`MGPipeTypes.h:453-462`): `Res, Unit, InternalFormat, Layer, Level:Uint16,
Layered:Uint8, Access:Uint8` (24 B). `MGPShaderImages` (`:464-468`): `Start, Count, ContentHash`
var-tail header. Fields tables `PipeFields.def:117-121`.
Catalogue row `PipeCalls.def:119` — `kCtxState, kVarTail`.
Coverage row `Coverage.def:85` maps `GetImageTextureBinding → SetShaderImages`.
Dirty bit `MGPipeDirty::NewShaderImages` (`MG_Impl/Pipe/Tracker.h:79`), computed at
`Tracker.h:311-312` as `mix(mix(textureContent, textureParams), programImageUnitVersion)` — already
live, just not emitted (`kMGPipeDirtyEmittedAtP3a` at `Tracker.h:104-107` stops at bit 10).

### 2.2 The single funnel: `SyncImageTextureBinding`

`DirectGLES.cpp:2057-2135`. Everything about an image unit passes through here — the file says so at
`:2019-2021` ("`glBindImageTextures` is a frontend loop over `glBindImageTexture`, and the whole-sweep
`SyncImageTextureBindings` goes through it too"). It:

* maintains `g_writableImageBufferUnits[192]` + `g_writableImageBufferUnitCount`
  (`:2029-2031`, tracker `:2038-2054`, predicate `:2033-2036`);
* maintains `g_imageUnitHighWaterMark` (`:2055`, bumped `:2063-2065`);
* unbinds with `glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8)` `:2066-2069`;
* calls `SyncTextureObjectToBackend(texture, /*imageBindableStorageRequired=*/true)` `:2071` —
  **this is the call that forces immutable storage and can re-mint the ES texture name**;
* normalises `layered`/`layer` through `SupportsLayeredImageBinding` (`:1992-2013`, and note the
  rule: it asks the **backend** target after `MapToBackendTextureTarget`, and `layer` is forced to 0
  for a non-layerable target — Adreno took a stray layer index literally);
* rewrites the bind **format**: buffer textures take the *split* view
  (`GetImageBindableBufferSplitFormat`, `backendTexture->GetBufferImageSplitViewId()`,
  `:2126-2138`), everything else takes the *widening*
  (`GetImageBindableStorageWidening`, `:2139-2145`);
* finally `glBindImageTexture(unit, bindTextureId, Level, layered, layer, Access, bindFormat)` `:2133`.

`MarkWritableImageBufferTexturesGpuWritten` (`DirectGLES.cpp:2147-2171`) is the GPU-write
announcement for buffer textures on writable image units; under push it already routes through
`BufferImpl::MarkBufferGpuWritten` (`:2166-2169`), i.e. the P3a reverse channel.

`SyncImageTextureBindings` (`:2173-2185`) sweeps `min(192, MaxImageUnits)` units.

### 2.3 The image sweep memo

`DirectGLES.cpp:2190-2193` (`g_imageSweepContextId`, `g_imageSweepSamplingGeneration`,
`g_imageSweepBackendContextGeneration`, `g_imageSweepValid`), gate
`SyncImageTextureBindingsForDraw` at `:2202-2214`. Two properties P4a must keep:

* the `g_imageUnitHighWaterMark == 0` early-out (`:2203`) is what makes every Minecraft draw pay one
  integer test;
* the gate key is deliberately the **frontend** sampling-resolution generation and **not** a
  backend re-mint counter, because a texture bound *only* to an image unit is re-minted *inside* the
  sweep (`:2195-2201`). A server-side epoch would be bumped after the gate had already declined.

Called from the draw path at `DirectGLES.cpp:3355`.

### 2.4 The photon canary — image binding semantics

Memory `flywheel-indirect-lessons` item 5 (and item 2) applies directly, and its mechanism is in
this tree:

* `RebindImageUniformsToFrontendUnits` (`Managers.cpp:602-651`) rewrites every image uniform
  declaration's `layout(binding = N)` to the **frontend-tracked unit**
  (`GetUniformLocation` → `GetUniformSamplerOrImageUnitIndex`, `:627-635`), because glslang
  auto-assigns a binding when desktop GL source omits one while the app picks the unit with
  `glUniform1i` (illegal on ES image uniforms). Called at `Managers.cpp:9800`, and the ordering
  constraints against `BakeImageFormatQualifiers` / `RemapImageArrayElementUnits` /
  `SplitReadWriteImageUniforms` / `RemoveLayoutBinding` are spelled out at `:9801-9851`.
* `IsImageUniformType` (`Managers.h:1826-1867`) is the **33-entry** contiguous block
  `GL_IMAGE_1D`..`GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY`; the header records that the earlier
  15-entry list caused two bugs (a non-baked non-core format losing the stage, and an image uniform
  assigned with `glUniform1i`).
* `CacheResourceLocations` skips image uniforms entirely (`Managers.cpp:10316-10321`).

**The canary itself** (from the memory note, still the rule): real-device Flywheel testing does NOT
cover Iris packs; `photon-v1.3b` on llvmpipe retrace is what catches an image/SSBO binding-semantics
regression. `MEASUREMENTS`/roadmap caveat: photon is broken on Adreno (memory
`photon-broken-on-adreno`) — run it on llvmpipe, not on the phone. P4a moves the image-unit
resolution to `set_shader_images` records and the *format bake* stays server-side (the format the
shader was built against is live `glBindImageTexture` state, §3.4), so this is precisely the change
the canary exists for. **Recommend the brief pin `photon-v1.3b` desktop retrace as a named G3b
case for P4a.**

### 2.5 Format-less image bake (the fourth staleness key)

`ImageFormatBakeInputs` (`Managers.h:2170-2199`) + `CollectImageFormatBakeInputs`
(`Managers.cpp:8327-8390`): walks the program's uniform reflection
(`GetMaxUniformLocation`/`GetUniformName`/`GetUniformType`/`GetUniformTypeFacts`, `:8346-8351`),
resolves each format-less image's unit (`GetUniformSamplerOrImageUnitIndex`, `:8377`), reads the
**live** bound format (`BoundImageUnitFormat`, `Managers.cpp:8155-8161`) and mixes a signature
(`MixImageUnitFormat`, `:8163`). Stored on the twin as `m_formatlessImageUnits` +
`m_imageUnitFormatSignature` (`Managers.h:2118-2119`, written `Managers.cpp:9287-9288`) and
re-checked per draw by `ImageUnitFormatsStillMatch()` (`Managers.cpp:8544-8547`, declared
`Managers.h:2030`), consumed in the rebuild condition at `DirectGLES.cpp:3164`.
`FormatlessImageBakeScenario` and `NonCoreImageFormatScenario` are its itests.

---

## 3. Programs

### 3.1 The `ShaderCso` slot table and the composite band

`Managers.h:2130-2131` / `Managers.cpp:8065`:
`TwinRegistry<ProgramObject, BackendProgramObjectImpl, MGPipeKind::ShaderCso> g_backendProgramObjects`.

Slot space: `MGPipeHandles.h:84-92` reserves the **top 1/16** of the `ShaderCso` space
(`kMGPipeShaderCsoSlotLimit = 1<<20`, `kMGPipeShaderCsoCompositeSlotBase = limit - limit/16`) for
**program-pipeline composites**, with `MGPipeIsCompositeShaderSlot(slot)` at `:90-92`. That band is
allocated and asserted but **nothing mints into it yet** — P4a's composite resolver does.

Resolution today, `PrgramImpl::SyncCurrentProgram` at `DirectGLES.cpp:3104-3128`: push arm
`g_backendProgramObjects.Find` → `GetOrCreate` (`:3106-3113`); legacy arm goes through
`g_programTwinLookupMemo` (`DirectGLES.cpp:134-135`, an 8-entry `TwinLookupMemo` declared at
`DirectGLES.cpp:93`), `:3115-3127`. A per-draw stash `g_currentDrawFrontendProgram` /
`g_currentDrawBackendProgram` is written at `:3202-3203` and read at `DirectGLES.cpp:3775-3778`
and `:4046-4052`.

### 3.2 The rebuild condition — nine clauses, and where each one comes from

`DirectGLES.cpp:3148-3200`. `twin->SyncToBackend(currentProgram)` runs when **any** of:

| clause | line | source of truth |
|---|---|---|
| no backend program id | 3148 | |
| `GetSyncedLinkVersion() != GetLinkVersion()` | 3150 | program state |
| `GetSyncedImageUnitVersion() != GetImageUnitVersion()` | 3151 | program state |
| snorm clamp mask ≠ `g_snormFallbackClampOutputMask` | 3152 | **draw FBO** |
| unorm clamp mask ≠ `g_unormFallbackClampOutputMask` | 3153 | **draw FBO** |
| `GetFragColorBroadcastCount() != g_fragColorBroadcastCount` | 3154 | **draw FBO**, memoised at `:3078-3101` |
| SSBO binding signature ≠ `ComputeShaderStorageBlockBindingSignature(*program)` | 3155-3156 | program state |
| `!ImageUnitFormatsStillMatch()` | 3164 | **live image-unit state** |
| patch-vertices/outer/inner mismatch (bitwise compare, NaN-safe — `:3183-3186`) | 3188-3196 | **context state** (`MGB_CTX->GetPatchVertices()` etc.) |

The comment at `:3187-3199` explains why the `gl_PerVertex` member set needs **no** clause on this
backend (it mirrors the neighbouring stages' emitted ESSL) while DirectVulkan does.

`GetSyncedLinkVersion()` call sites: `DirectGLES.cpp:3150`, `:4110`, `:7857`; written once at
`Managers.cpp:10234`; declared `Managers.h:2004`.

### 3.3 The `BackendProgramObjectImpl` surface

Class `Managers.h:1873-2122`. Public accessors that P4a's `MGPProgramDesc`/applier must reproduce
or replace: `GetIndirectParamsBinding` (`:1959`), `GetBackendProgramId` (`:1960`),
`IsBackendProgramUsable` (`:1964`), `GetBackendGlobalUBOId` (`:1965`),
`GetSnormFallbackClampOutputMask`/`GetUnormFallbackClampOutputMask` (`:1966-1967`),
`GetFragColorBroadcastCount` (`:1968`), `GetShaderStorageBlockBindingSignature` (`:1973`),
`GetAtomicCounterBindings` + `GetAtomicCounterEsslBindingTop` (`:1979-1980`),
`GetPassthroughTessControl{PatchVertices,OuterLevel,InnerLevel}` (`:1986-1998`),
`HasGlobalUboBlock` / `GetUniformBlockBackendIndices` / `GetSamplerUniformBindings` (`:1997-1999`),
`GetLastUploadedGlobalUboVersion`/`Set…` (`:2000-2001`), `GetGlobalUboBackendBlockSize` (`:1999`),
`GetGlobalUboRingAllocation` (`:2000`), `GetSamplerPassMemo` (`:2001`),
`ReadsDrawID`/`ReadsBaseVertex`/`RoutesViewportIndex` (`:1946-1958`),
`SetBaseInstance`/`SetBaseInstanceWordIndex`/`SetDrawID`/`SetBaseVertex`/`SetViewportPassMask`
(`:1939-1952`). Private state at `:2066-2121`.

`SamplerUniformBinding` (`Managers.h:1880-1891`): `frontendLocation, backendLocation, uniformType,
lastAssignedUnit, lodBiasLocation, lastAssignedLodBias`. Built once per link in
`CacheResourceLocations` (`Managers.cpp:10264-10345`), which also does the array-element
subscripting fix (`SubscriptUniformNameForElement`, `Managers.cpp:10245-10256` — reflection names an
array after its first element at every location it spans, so `goku[7]` needed its own
`glGetUniformLocation`).

### 3.4 `SyncToBackend` — what it reads from the `ProgramObject`

`Managers.cpp:9230-10240`. The complete read set with line numbers (the P4a archive must supply
every one of these, or the call must carry it):

```
9241 GetExternalIndex        9248 GetLinkStatus / GetSpirvStatus
9268 GetShaderStorageBlockBindingOverrides   (-> SpvcSession::SetShaderStorageBlockBinding)
9335 GetLinkedShaderStages   9336 GetGeneratedSpirv        9347 PointSizeDemoted
9357 GetLinkedShaderSnapshot 9366 GetSpirvValidationEnabled
9375 GetTransformFeedbackVaryings           10017 GetTransformFeedbackVaryingCount
10067 GetTransformFeedbackBufferMode        10217 GetUBOSize
10234 GetLinkVersion         10235 GetImageUnitVersion
10290 GetActiveUniformBlocksCount           10295 GetUniformBlockName
10310 GetMaxUniformLocation  10312 GetUniformName          10314 GetUniformType
 8119 GetShaderStorageBlockBindingOverrides (signature)
 8346-8351 GetMaxUniformLocation/GetUniformName/GetUniformType/GetUniformTypeFacts
 8377 GetUniformSamplerOrImageUnitIndex     8506/8513 GetUniformLocation / UniformLocationsAliasSameUniform
  622/627 (RebindImageUniformsToFrontendUnits) GetUniformLocation / GetUniformSamplerOrImageUnitIndex
```

`ARCHITECTURE.md:264` names the **eight extra inputs** the backend program depends on beyond the
artefacts, and the list above matches: draw-FBO snorm/unorm clamp masks, fragColor broadcast count,
storage-block binding signature, atomic-counter set, live image formats, patch parameters (+ Magma's
default-FB height and XFB layout). `create_shader_state` publishes the **artefacts**; the server
specialises at verb time. That is the design; nothing in this tree contradicts it.

### 3.5 Sampler/LOD-bias/UBO/atomic-counter binding at draw time

`BindCurrentProgramWithResources` (`DirectGLES.cpp:3765-4041`), in order:

1. `backendProgram.Use()` (`:3781`; the impl is `Managers.cpp:10347-10390`, with the
   `m_rebindAfterRelink` rule — a relink replaces the executable behind an unchanged GL name);
2. **global UBO / default uniform block** (`:3786-3856`): `GetUBOSize()`, `GetUBOContentVersion()`,
   `MapUBO()`; preferred path is the shared persistent-mapped **UBO ring**
   (`UboRingAvailable/UboRingAllocate/UboRingMappedPtr/UboRingBufferId`, `:3805-3831`) with a
   per-program `UboRingAllocation` keyed `{contentVersion, ringGeneration, frameSerial, offset}`
   (`Managers.h:2003`, `BufferImpl::UboRingAllocation`), and a `glBufferSubData` fallback
   (`:3833-3852`). Bytes are accounted to `PipeStats::ByteClass::StageUboGlobal`
   (`MG_Util/Metrics/PipeStats.h:55-57`). **This is `set_global_constants`'
   (`MGPGlobalConstants{ShaderCso, Version, Blob}`, `MGPipeTypes.h:507-513`, "covers the DEFAULT
   UNIFORM BLOCK only (D6)") counterpart** — the ring is on `ARCHITECTURE.md:315`'s
   do-not-touch list, so the P4a shape is: the client emits the block image + version, the server
   keeps the ring exactly as-is;
3. **named UBOs** (`:3857-3910`): `blockBackendIndices` from the link cache,
   `currentProgram->GetUniformBlockBinding(i)`, `MGB_CTX->GetBufferBindingPoint(Uniform, binding)`,
   then the P3a buffer clean-probe (`GetBufferResource`/`IsBufferDrawClean`/`EnsureBufferResource`,
   `:3884-3894`) and `BindBufferBaseCached`/`BindBufferRangeCached`. This is
   `set_shader_buffers(Uniform)` — **P4b, not P4a** (dirty bits 15-17, `Managers.h:717-723` says so
   explicitly);
4. **atomic counters** (`:3912-3921`) via `SyncAtomicCounterBuffers(bindings, esslBindingTop)`;
5. the sampler pass (§1.6).

### 3.6 The SPIR-V → ESSL transpile: what is per-program and what is per-context

* **`SpvcSession` is per STAGE, per transpile, and stack-allocated**:
  `MG_Util/ShaderTranspiler/SpvcSession.h:89-105` (move-only, non-copyable), constructed at
  `Managers.cpp:9019-9020` inside `TranspileSpirvToEssl`. Nothing persists it. Its mutators that
  carry program state: `SetShaderStorageBlockBinding` (`SpvcSession.h:120`, called
  `Managers.cpp:9037-9039`), `SetAtomicCounterBlockBindings` (`SpvcSession.h:135`, called
  `Managers.cpp:9060-9061`, and **both halves are memo state** — the top is key, the returned
  bindings are payload, `:9051-9059`), `SetVertexAttribLocation` (`:109`),
  `DropDefaultFragmentOutputColorIndex` (`:154`), `RelaxReadWriteExclusiveStorageBuffers` (`:175`),
  `ReflectTransformFeedbackCaptures` (`:189`), `StripTransformFeedbackDecorations` (`:201`),
  `SetEntryPoint` (`:202`), `SetSpecializationConstants` (`:210`).
* **The L2 ESSL translation cache is per PROCESS**: `GetEsslTranslationCache()`
  (`MG_Util/ShaderTranspiler/TranslationCache.h:698`), consulted at `Managers.cpp:9664-9720`.
  Key inputs `EsslTranslationKeyInputs` (`TranslationCache.h:635-689`) mix **driver capability bits**
  (`viewportIndexLoweringArmed`, `supportsNoperspectiveInterpolation`,
  `supportsExtendedImageFormats`, the four sample-count limits) with **per-program/per-context**
  inputs (`xfbCaptureBlockNames`, `glFormatByUniformName`, `storageBlockBindingOverrides`,
  the two block-rename maps, the two location-strip flags, `atomicCounterEsslBindingTop`,
  `esslVersion`, `enableSpirvValidation`). Payload `EsslTranslationResult` (`:618-632`) carries the
  ESSL **plus** `flattenedXfbBlockNames` **plus** `atomicCounterGlBindings` — the header records
  why dropping either is a silent-wrong-output bug. `Managers.cpp:8556-8558` states the invariant:
  *every* read inside `TranspileSpirvToEssl` must appear in `BuildEsslTranslationKey`.
  `MG_Test/ShaderTranspiler/TranslationCacheTest.cpp` is its unit test.
* **`InvalidateCompileEnv()` is a forwarded (F-class) `PipeInputs` accessor**
  (`MG_Backend/MGPipe/PipeInputs.h:573`), i.e. already a reverse call rather than a state read.

`ARCHITECTURE.md:258-260`: glslang stays entirely on the **client**, SPIRV-Cross
(`TranspileSpirvToEssl`) entirely on the **server**, file-level cut. No `MOBILEGL_IPC_PROGRAM`
switch, no server-side compile pool.

### 3.7 What a shader CSO on the wire must carry

`MGPipeTypes.h:320-335`:

```cpp
struct MGPProgramDesc {           // 192 B
    MGPipeHandle Cso;
    Uint32 StageMask;             // == GetLinkedShaderStages()
    Uint32 GlobalUboSize;
    Uint32 ReservedNumSamplesOffset;
    Uint8  SpirvStatus, NativeFloat64, PointSizeDemoted, EnableSpirvValidation;
    MGPBlobRef Spirv[6];          // per stage
    MGPBlobRef Reflection;        // the whole LinkArtifacts + SpirvArtifacts archive
};
```

`ARCHITECTURE.md:259` enumerates what the archive **must** cover: the four `ResourceReflection`
vectors (each with its `TypeFacts`), `uniformSamplerOrImageUnitIndex`, `uniformBlockBinding`,
`shaderStorageBlockBinding` (by name), `explicitOpaqueUniformBindings`,
`xfbVaryings/xfbStrides/xfbPackedStride/xfbNeedsScatteredCapture`, `computeLocalSize`, the GS/TCS/TES
facts, `usesReservedNumSamples`, `uniformOffsets`; and `XfbVarying` carries two spellings (GL name
+ block instance/member/element).

### 3.8 `ProgramArtifacts.h` — the archive as it stands

`MG_State/GLState/ProgramState/ProgramArtifacts.h`, 576 lines, extracted at P0.5. Purity is asserted
by `check_include_closure.py`'s `artifacts-header` probe (`:16-19`), and the header notes that it is
glslang-free **by symbol**, not textually (`:10-15`).

* `TypeFacts` (`:31-59`, 20 fields, POD, `sizeof == 44` pinned at `:552`);
* `ResourceReflection` (`:63-86`, 14 fields, `sizeof == 128` on libstdc++);
* `XfbVarying` (`:95-121`, 11 fields, `sizeof == 128`);
* `LinkArtifacts` (`:160-345`, **58 members**, `sizeof == 1056`), including the live
  `SharedPtr<glslang::TProgram> program` at `:165` that is **null for every archived instance by
  construction** and is the one member `VisitFields` deliberately omits (`:471-474`);
* `SpirvArtifacts` (`:365-402`, 8 fields, `sizeof == 88`): `generatedSpirv`,
  `enableSpirvValidation`, `uniformOffsets`, `globalUboScratch`, `reservedNumSamplesOffset`,
  `spirvStatus`, `nativeFloat64`, `pointSizeDemoted`.
* **The visitor tables**: `VisitFields(TypeFacts)` `:408-434`, `(ResourceReflection)` `:436-453`,
  `(XfbVarying)` `:455-469`, `(LinkArtifacts)` `:471-535` (57 fields), `(SpirvArtifacts)` `:537-547`.
  Contract at `:404-407`: `v(const char* name, Field&)`, `Field` const when `Self` is const, so **one
  table serves both directions**.
* **Trip wires** `:549-575`: `sizeof` static_asserts, pinned **per STL** — libstdc++ numbers are in
  the tree, the libc++/NDK branch at `:565-566` is **inert until the integrator pins it**
  (`MGL_ARTIFACT_SIZES_LIBCXX_PINNED`). `MG_Test/Program/ProgramArtifactsTest.cpp` records every
  `sizeof` as a ctest property on every platform.

> **The serializer does not exist.** Grep finds no writer/reader over these tables — only the tables
> and the size asserts. Every `VisitFields` comment says "(and its serializer when one exists)".
> **P4a's contract package owns writing it**, and the NDK `sizeof` pin at `:565-566` is a
> gate the integrator must close in the same phase, or an Android build silently has no trip wire.

### 3.9 Program-pipeline composites (`ProgramPipelineScenario`)

`GLContext::GetProgramForDraw()` (`MG_State/GLState/Core.h:190`, `Core.cpp:609`) already flattens a
pipeline into one hidden composite `ProgramObject` **entirely in the frontend**, which is why
`ARCHITECTURE.md:212` says the tracker pushes **one** handle out of the composite band and the
server never learns it is a composite. Two signatures decide the work
(`MG_State/GLState/ProgramState/ProgramPipelineObject.h`):

* `ComputeDrawProgramSignature` (`:97-107`) — `{lifetimeId, GetLinkVersion()}` per graphics stage;
  the header (`:75-85`) records that keying on `GetBackendStateVersion()` instead made the SSO
  conformance loop rebuild the composite (glslang + SPIR-V + spirv-opt) **on every draw**;
* `ComputeUniformMirrorVersions` (`:125-141`) — the per-stage refresh gate, mixing
  `backendStateVersion<<32 | uboContentVersion` and `blockBindingVersion<<32 | uniformWriteSetVersion`.

`DirectGLES.cpp:3157-3163` records the accident this removed: `glUniform1i` on an image uniform used
to bump the composite's cache key and therefore produce a whole new twin.

`ProgramPipelineScenario.cpp:1-28` lists the sixteen conformance cases (compute_shader.*,
shader_image_load_store.advanced-sso-*, shader_storage_buffer_object.basic-syntaxSSO /
basic-noBindingLayout) that depend on the flattening. `MG_Test/Program/ProgramPipelineCompositeTest.cpp`
is the unit-level pin.

### 3.10 The "CompositeResolver"

**It does not exist in the tree.** `grep -rn CompositeResolver MobileGL/` returns nothing; the only
two mentions are `docs/Disaggregated/ARCHITECTURE.md:562` (the P2+ file list for `MG_Impl/Pipe/`,
which today holds `CsoCache.h, PipeFill.cpp, PipeFill.h, ResourceTracker.h, SetHashSuppressor.h,
SlotAllocator.{h,cpp}, Tracker.h, VertexInputEmit.h`) and `ROADMAP.md:20` (P4a's own row).

From `ARCHITECTURE.md:212` and `MGPipeHandles.h:84-92`, what it composes is **program + program**:
the pipeline's per-stage `ProgramObject`s → one `ShaderCso` handle out of the reserved top-1/16 band,
with the pipeline cache's eviction releasing the slot, bumping `gen` and emitting
`delete_shader_state`. It is **not** a framebuffer/sampler composer — those are separate
(`set_framebuffer_state`, `bind_sampler_states`). Its side benefit, stated at `:212`: the blocking
`JoinLinkAndSpirv()` leaves the server's draw path. Model it on `MG_Impl/Pipe/CsoCache.h`
(`kMGPipeCsoCacheCapacity = 64` at `:53`, `Acquire` at `:74`, `Counters` at `:57-69`, LRU eviction,
and the `kMGPipeBehaviourNoCsoContentAddressing` negative control at `:30-37`).

---

## 4. Emulations that touch programs — which stay frontend-side

`ARCHITECTURE.md:216` is the rule: *驱动表达不了的变换在 tracker 里 lowering，硬件/驱动强加的变换在
driver 里 lowering*, and the per-emulation table is `:221-243`.

| emulation | code | verdict |
|---|---|---|
| **fragColor broadcast** | `PrgramImpl::BroadcastLegacyFragColor` (`Utils.h:260`, `Utils.cpp:506`), called `Managers.cpp:9854`; the count is `g_fragColorBroadcastCount` (`Managers.h:2129`) memoised from the draw FBO's `GetDrawBuffers()` at `DirectGLES.cpp:3078-3101`, snapshotted onto the twin at `Managers.cpp:9261` | **SERVER.** It is a post-emission ESSL rewrite (`ARCHITECTURE.md:315` do-not-touch: "SPIRV-Cross 会话与 post-emission ESSL 重写"), keyed on framebuffer state the server already holds after `set_framebuffer_state`. |
| **noperspective** | `ShaderCompiler::EmulateNoPerspectiveForEssl`, `Managers.cpp:8789-8792`, armed on `!g_GLESCapabilities.SupportsNoperspectiveInterpolation` (`:8788`); the arming bit is L2 key material (`TranslationCache.h:661`) | **SERVER.** Driver-capability-armed SPIR-V pass; the client cannot know the arming. |
| **fp64 demotion** | `DemoteFloat64Pass`, decided **per program at link** and recorded as `SpirvArtifacts::nativeFloat64` (`ProgramArtifacts.h:378-390`); read by `BackendObject.h:496`, `Managers.cpp:3522`/`:3924`, DirectVulkan `VertexInputStateFactory.cpp:253`/`:494`, and by the frontend uniform path `GL_Program.cpp:1319`/`:1643`/`:1784` | **CLIENT (already).** The demotion happens before the archive exists; the flag rides in `MGPProgramDesc::NativeFloat64` (`MGPipeTypes.h:329`) so the server knows the layout. The **vertex-side** fp64 narrowing is server-side (`ARCHITECTURE.md:233`), and ID-15's M-3 ruled the P3a `SyncGpuWrites` gap on that stream a deviation — **P4a inherits that open item**. `DoublePrecisionScenario` is the gate. |
| **XFB scatter** | `XfbImpl` in `DirectGLES.cpp:895-1230`: `XfbCaptureTarget` `:895`, `scattered/scatterProgram/scatterCapacityVertices` `:922-924`, `g_scatterBufferId` `:930`, `ReadbackCapturedRanges` `:987`, `BindScatterCaptureBuffer` `:1032`, `ScatterCapturedRecords` `:1068-1140`, armed at `StartPendingTransformFeedback` `:1210` off `program->NeedsScatteredTransformFeedbackCapture()` (`ProgramObject.h:1227` → `xfbNeedsScatteredCapture`) | **CLIENT** — `ARCHITECTURE.md:239` and §8.5. The read-modify-write moves to the client. Note `Managers.h:576-580`: `ReadbackCapturedRanges` and the scatter path are two of the backend-initiated shadow writebacks that bump the buffer-mutation epoch **without** an op — P3a's epoch contract already names them, and a P4a move must not orphan those bumps. |
| **texture LOD bias** | `EmulateTextureLodBias` (`Utils.h:536`, called `Managers.cpp:9855`) + the per-draw `glUniform1f` at `DirectGLES.cpp:3982-4000` | **SERVER** (post-emission rewrite), but the *value* is sampler state and must arrive with the sampler CSO / sampler view. `MGPTextureParams::LodBias` exists (`MGPipeTypes.h:317`) but `SamplerParameters::lodBias` (`MGPipeValueTypes.h:470`) is the one the pass reads. |
| **image format bake / widen / split / array remap** | §2.5, plus `BakeImageFormatQualifiers` (`Utils.h:313`), `RemapImageArrayElementUnits` (`:365`), `SplitReadWriteImageUniforms` (`:519`), `RemoveLayoutBinding` (`:314`) | **SERVER.** `ARCHITECTURE.md:235`: image-bindable 存储加宽/拆分 → server. |
| **pass-through TCS synthesis** | `AttachPassthroughTessControlStage` (`Managers.h:2041-2044`), `BuildPassthroughTessControlEssl` (`Utils.h:408`) | **SERVER**, keyed on `set_patch_state` (already P2). |

Everything in this table except fp64-demotion and XFB scatter stays on the server, which is why the
shader CSO's payload is SPIR-V + archive and **not** ESSL.

---

## 5. Tests

### 5.1 Integration scenarios that already exercise this surface

All rows are in `MG_IntegrationTest/CMakeLists.txt:56-135`.

*Samplers / border colour*: `IntegerBorderColorScenario` (`:127`) — the `borderColorForm` scenario;
its header (`Scenarios/IntegerBorderColorScenario.cpp:9-33`) documents the exact Espryt failure
(1132396544 = the bits of 255.0f read through an integer sampler) and the Vulkan
`VK_EXT_custom_border_color` requirement. **ID-21 records a pre-existing ASan stack-buffer-overflow
in this scenario** — a test bug, and P4a will be running it a lot.
Also `SampledSetStalenessScenario` (`:65`), `TextureViewScenario` (`:114`),
`ClearTexImageUndefinedLevelZeroScenario` (`:128`).

*Images*: `ImageLoadStoreSsoScenario` (`:88`), `ImageTargetKindScenario` (`:89`),
`ImageFormatQualifierScenario` (`:90`), `NonCoreImageFormatScenario` (`:91`),
`ImageSizeAfterRespecScenario` (`:92`), `FormatlessImageBakeScenario` (`:100`),
`UnboundImageDescriptorScenario` (`:126`), `AtomicCounterScenario` (`:119`).

*Programs*: `ProgramPipelineScenario` (`:87`), `PostLinkAttachScenario` (`:99`),
`RelinkStageSetScenario` (`:124`), `AsyncCompileScenario` (`:61`, with two pinned-config lanes at
`:679-753`), `PipelineFailureScenario` (`:68`), `SpirvShaderBinaryScenario` (`:123`),
`UniformInitializerScenario` (`:82`), `Glsl420DeclarationScenario` (`:94`),
`IoBlockNameCollisionScenario` (`:95`), `UnlocatedIoBlockScenario` (`:96`, plus a location-strip
pinned-on lane at `:647-670`), `PointSizeDemotionScenario` (`:109`, pinned lanes `:814-834`),
`SsboDeclarationFormScenario` (`:93`), `SsboArrayLengthScenario` (`:80`),
`SsboArrayDynamicIndexScenario` (`:121`), `DoublePrecisionScenario` (`:81`),
`ViewportArrayScenario` (`:79` + negative control `:768`), the five XFB scenarios (`:104-108`),
`TessellationDrawModeScenario`/`GeometryDrawModeScenario` (`:97-98`).

*The P2/P3a machinery scenarios P4a extends*: `HandleRecycleScenario` (`:133`, six pinned arms at
`:979-1024`), `CsoContentAddressingScenario` (`:134`, four lanes `:1086-1113`),
`ResourceSubsystemControlScenario` (`:135`, two lanes `:1160-1169`),
`PipeVerifyArmingScenario` (`:131`), `PoisonOmissionScenario` (`:132`).

`HandleRecycleScenario` today covers exactly two kinds — buffer
(`ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents`, header `:14`) and VAO
(`AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput`, `:568`). **P4a's G8
analogue is: add a texture case, a sampler case and a program case, each with `.Handles`/`.Legacy`/
`.AbaControl` arms**, and record the red-before evidence on the contract tree exactly as P3a did.

### 5.2 Unit tests

`MG_Test/Pipe/`: `PipeCatalogueTest.cpp` (payload sizes — `sizeof(SamplerParameters)==100` at
`:131`, trivially-copyable at `:138`), `CsoCacheTest.cpp`, `TrackerTest.cpp`, `SlotAllocatorTest.cpp`,
`PipeInputsTest.cpp`, `RenderStateSpansTest.cpp`, and P3a's two emitter tests
`ResourceEmitTest.cpp` / `VertexInputEmitTest.cpp` (the shape P4a's `SamplerEmitTest` /
`ProgramEmitTest` / `ImageEmitTest` should copy; note ID-14 — `ResourceEmitTest`'s `ApplierGuard`
is the fixture-scoping pattern, and `MG_Test/Buffer/BufferTest.cpp` needed the same treatment).

`MG_Test/ShaderTranspiler/`: `SpirvPassTest.cpp`, `TranslationCacheTest.cpp`,
`WidenImageFormatsTest.cpp`, `FlattenAtomicCounterBlockTest.cpp`, `FlattenXfbInterfaceBlocksTest.cpp`,
`UniquifyIoBlockNamesTest.cpp`, `StripIoBlockLocationsTest.cpp`, `LowerViewportIndexTest.cpp`,
`DemoteFloat64Test.cpp`, `DemotePointSizeTest.cpp`, `LegalizeResourceArrayIndexTest.cpp`,
`ClampMultisampleFetchTest.cpp`, `GlslangCaptureTest.cpp`, `DeriveNumSubgroupsTest.cpp`,
`EmulateSubgroupsTest.cpp`, `FixIterationRP*Test.cpp`, `FlattenFloat64StorageBlockTest.cpp`.
`EmulateNoPerspectiveForEssl` is tested from `MG_Test/Program/ProgramUtilTest.cpp:1344`, `:1385`,
`:1439`, `:1465` (not from the ShaderTranspiler directory).

`MG_Test/Program/`: `ProgramArtifactsTest.cpp` (**the archive's size-and-field pin — P4a's serializer
test belongs beside it**), `ProgramPipelineCompositeTest.cpp`, `ProgramInterfaceTest.cpp`,
`ProgramTest.cpp`, `ProgramUtilTest.cpp`, `AsyncLinkTest.cpp`, `AsyncSpirvPhaseTest.cpp`
(reads `GetLinkVersion()` at `:695`/`:701`), `AsyncCompileTest.cpp`, `AsyncTeardownTest.cpp`,
`OptimisticStatusTest.cpp`, `ParallelShaderCompileTest.cpp`, `ShaderCompileAdoptionTest.cpp`,
`TessellationLinkTest.cpp`, `XfbBlockVaryingTest.cpp`, `XfbFrontendOrderInvarianceTest.cpp`.

### 5.3 `PipeInputs` accessors for programs

`MG_Backend/MGPipe/PipeInputs.h`. O-class storage: `m_programForDispatch`, `m_programForDraw`,
`m_transformFeedbackProgram` (`:683-685`), listed in `MGP_INPUT_STORAGE_LIST` at `:108-109`, `:122`,
with accessors at `:534-548`. F-class (forwarded, sticky, no poison check — the declared exception
at `:558-568`): `GetProgramObject(Uint index)` `:570`, `ValidateProgramName(Uint index)` `:574`,
plus `GetTextureObject` `:571` and `InvalidateCompileEnv` `:573`.
`Coverage.def:99-101` maps `GetProgramForDispatch→SetDispatchProgram`,
`GetProgramForDraw→SetDrawProgram`, `GetProgramObject→CreateShaderState`, and `:127`/`:146-149`
records `GetProgramObject`/`ValidateProgramName` as client-resolved name lookups.

`gPipeInputs` is **leak-at-exit storage** (`PipeInputs.h:688-700`, ID-18/ID-21): its O-class
`SharedPtr`s reach `~BufferObject` → the slot allocator at exit. P4a adds **more** frontend
`SharedPtr` members if it is careless — sampler/program handles must be `MGPipeHandle`, not
`SharedPtr`, or the exit-order closure has to be re-litigated.

---

## 6. Traps and open items for the P4a brief

1. **`SamplerParameters` has 3 bytes of trailing padding and no field table.** §1.1. Contract commit
   must add `MGP_FIELDS_SamplerParameters` + the `MGP_VERIFY_PAYLOAD_LIST` row, and the sampler CSO's
   content hash must be field-wise. A memcmp-based hash mints a fresh CSO per call.
2. **`MGPTextureParams` cannot express what `SyncBuiltinSamplerToBackend` pushes.** §1.8 — no filter,
   wrap, compare mode or border colour. Decide in the contract: widen the payload (it is 32 B with
   an `MGP_ASSERT_POD`) or give each texture an implicit sampler CSO.
3. **The archive has no serializer, and the libc++/NDK `sizeof` pin is inert.**
   `ProgramArtifacts.h:565-566`. Both are contract-package work; the NDK pin must land in the same
   phase or the Android build has no trip wire on a 58-member struct.
4. **`g_boundSamplersCache` is a raw twin pointer per unit and is load-bearing for three memos.**
   §1.4-1.6. It becomes a per-unit handle array in the applier; the `memcmp` replay proofs
   (`DirectGLES.cpp:3597-3604`, `:3942-3950`) have to be re-derived against that.
5. **The sampler twin is created lazily, only from the program pass** (`DirectGLES.cpp:4014-4020`).
   `create_sampler_state` inverts the timing of `glGenSamplers`.
6. **Three of the program twin's nine staleness keys are not program state.** §3.2/§3.4. The
   `st_variant` specialisation is the design (`ARCHITECTURE.md:264`); a brief that treats
   `create_shader_state` as self-contained will produce a per-draw rebuild.
7. **`g_rawDepthFetchSamplerState` constructs a frontend `SamplerObject` inside `MG_Backend`**
   (`DirectGLES.cpp:209`). `ARCHITECTURE.md:322` names its removal as P4a work. The purity gate G13
   (`grep -rc pGLContext MG_Backend` empty) does not catch it — it is a type use, not `pGLContext`.
8. **The image sweep's gate must stay keyed on the frontend generation** (`DirectGLES.cpp:2195-2201`)
   — a server-side epoch is bumped too late by construction.
9. **`SlotTables.h` still mints handles from inside `MG_Backend`** for the six P2 kinds
   (`:70-77` records the debt; `GetOrCreate(StatePtr)` at `:209-252` calls
   `MGPipeSlots().Acquire`). P3a discharged it for `Buffer` only. P4a discharges it for
   `Texture`, `Framebuffer`, `SamplerCso`, `ShaderCso` — four of the remaining five — and should say
   whether `Renderbuffer` follows in the same phase (its `resource_*` conversion is in the
   `ROADMAP.md:20` cell) or is left for later.
10. **`ForEachLive` needs `Entry::stateRef`, and a handle-keyed entry has none** (`SlotTables.h:266-269`,
    `:443-463`). The one direct-iteration site is `ScopedDetachedTextureFramebufferAttachments`, which
    walks **texture** twins (`SlotTables.h:61-64`) — i.e. exactly a kind P4a converts. That iteration
    has to be re-expressed before the texture table goes handle-keyed, or it silently walks nothing.
11. **The photon canary.** §2.4 — pin `photon-v1.3b` (llvmpipe retrace, **not** Adreno) as a named
    P4a case; it is the only thing that has ever caught an image-binding-semantics regression.
12. **`ProgramPipelineObject`'s composite band is allocated but unused** (`MGPipeHandles.h:84-92`).
    The composite resolver mints into it, and the pipeline cache's eviction is what frees the slot,
    bumps `gen` and emits `delete_shader_state` (`ARCHITECTURE.md:212`).
13. **Inherited from P3a**: ID-15 M-3 (the fp64 stream's dropped `SyncGpuWrites`) is still a ruled
    deviation; ID-21's `IntegerBorderColorScenario` ASan stack-buffer-overflow is still open and sits
    squarely in P4a's sampler test set; ID-19's per-draw vertex-input emission cost (+699/+422
    ns/draw) is on the optimisation list and P4a's per-unit sampler/image emission is the same shape
    of risk on VAO/texture-heavy frames (rd12 was +30%/+27%).
14. **Subsystem bits.** `MGPipe.h:72-95` has bits 0-8 used and
    `kMGPipeSubsystemsMigratedAtP3a = 0x1ff`. P4a needs its own bits (framebuffer / texture+sampler /
    program are three natural ones) and a `kMGPipeSubsystemsMigratedAtP4a`, plus the arm-resolution
    treatment at `Managers.cpp:2352-2400` (`ClassifyPipeSubsystemArm`, `StopOnArmlessPipeSubsystem`)
    — including the **dependency refusal** pattern (bit 8 requires bit 7, `Managers.cpp:2395-2400`):
    a program/sampler bit will require the texture bit for the same reason.
15. **Dirty bits.** `Tracker.h:70-83` already computes `NewShader`, `NewShaderBindings`,
    `NewGlobalConstants`, `NewFramebuffer`, `NewSamplerViews`, `NewSamplers`, `NewShaderImages`
    (values at `:246-262` and `:305-312`) — P4a only has to **emit** them and extend
    `kMGPipeDirtyEmittedAtP3a` (`:104-107`) into a `…AtP4a`. `MGPipeSubsystemForDirty` (`:128`ff)
    is where each new bit's gate goes.
16. **Registration.** `MGPipeSetResourceOps` is installed from `BufferImpl::RegisterBufferBackendOps`
    (`Managers.cpp:2529-2551`, called `BackendObject_DirectGLES.cpp:849` and
    `DirectGLES.cpp:10568`) — ID-8's accepted deviation. P4a's new op tables should get their own
    registration entry point rather than piling onto the buffer one.
