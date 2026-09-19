---

## 2. The non-`pGLContext` paths by which a backend reaches frontend objects

`pGLContext` is the only `MG_State::` global the backends touch (§0, "other MG_State globals: 0").
Every other reach is one of the five shapes below. P1 cannot succeed by rewriting only the 278
arrow sites: the accessors return **references to live frontend objects**, and the backends then
call ~200 distinct methods on those objects.

### 2.1 The one static cache of a frontend pointer

`MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:142-155` — the only place a raw `GLContext*` is
stored across calls:

```
142:    using FbBindingSlot =
143:        std::remove_reference_t<decltype(MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw))>;
144:    static const MG_State::GLState::GLContext* g_fbSlotCacheContext = nullptr;
145:    static Array<FbBindingSlot*, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fbSlotCache = {};
146:    static inline FbBindingSlot& GetFramebufferBindingSlotFast(FramebufferTarget target) {
147:        MG_State::GLState::GLContext* ctx = MG_State::pGLContext.get();
148:        if (ctx != g_fbSlotCacheContext) { ... &ctx->GetFramebufferBindingSlot(...) ... }
```

It caches **interior pointers into the GLContext object** (the by-value binding-slot members),
keyed on the raw context address. Under P1 this cache has no meaning across a pipe boundary — the
slots are not addressable from the backend at all. Its five call sites are the hot framebuffer
path and each is a `GetFramebufferBindingSlot` read that the arrow-site table does **not** list
(they read through the cache, not through `pGLContext`):

| call site | enclosing function | verb path | what it reads |
|---|---|---|---|
| `DirectGLES/DirectGLES.cpp:1611` | `SyncNeccessaryTextures` | texture op (draw/dispatch) | draw FBO binding slot |
| `DirectGLES/DirectGLES.cpp:1905` | `SyncCurrentFBO` | framebuffer sync | slot for `target` |
| `DirectGLES/DirectGLES.cpp:2743` | `SyncCurrentProgram` | program/uniform sync | draw FBO binding slot |
| `DirectGLES/DirectGLES.cpp:2858` | `BindCurrentFBO` | framebuffer sync | slot for `target` |
| `DirectGLES/DirectGLES.cpp:2933` | `ForceBindCurrentFBO` | framebuffer sync | slot for `target` |

Each of those five also reads the slot's **version** (`BindingSlot::GetVersion()`) at
`DirectGLES.cpp:1614`, `1917`, `2745`, `2936` — a value block P1 must carry alongside the handle.

There are **no** other `GLContext&`/`GLContext*` parameters or local aliases anywhere in
`MG_Backend`: `grep -rn 'GLContext\s*[&*]' --include=*.cpp --include=*.h` returns exactly the one
line 144 above once `pGLContext` matches are excluded.

### 2.2 `SharedPtr<MG_State::GLState::…>` in signatures and members (the "handle-ify" surface)

177 lines. The ones that matter structurally are the **table-level** ones (they are the verb
signatures themselves) and the **member-level** ones (backend containers that own or weakly hold
frontend objects across frames).

Table-level (these five `GLFunctionsTable` entries take frontend objects **by handle today**):

| declaration | file:line |
|---|---|
| `ClearNamedFramebufferfv(const SharedPtr<FramebufferObject>&, …)` | `BackendObject.h:158` |
| `ClearNamedFramebufferfi(const SharedPtr<FramebufferObject>&, …)` | `BackendObject.h:160` |
| `ClearNamedFramebufferiv(const SharedPtr<FramebufferObject>&, …)` | `BackendObject.h:162` |
| `ClearNamedFramebufferuiv(const SharedPtr<FramebufferObject>&, …)` | `BackendObject.h:164` |
| `BlitNamedFramebuffer(const SharedPtr<FramebufferObject>& read, const SharedPtr<FramebufferObject>& draw, …)` | `BackendObject.h:168-169` |
| `GetTextureImage(const SharedPtr<ITextureObject>&, …)` | `BackendObject.h:186` |
| `CopyImageSubData(const CopyImageEndpoint& src, …, const CopyImageEndpoint& dst, …)` where `CopyImageEndpoint` is `{SharedPtr<ITextureObject> Texture; SharedPtr<RenderbufferObject> Renderbuffer;}` | `BackendObject.h:33-38`, `BackendObject.h:176-180` |

Their per-backend mirrors: `DirectGLES/DirectGLES.h:60,62,64,66,70,71`;
`DirectVulkan/DirectVulkan.h:36,38,40,42,76,77,101`;
`DirectVulkan/Renderer/VulkanRenderer.h:184,186,188,190,195,196,256,1400`.

Member-level (backend state that outlives a verb and holds frontend objects):

| holder | file:line | what it holds |
|---|---|---|
| `UniformManager::m_fallbackTexture2D` | `Renderer/UniformManager.h:305` | `SharedPtr<ITextureObject>` — a synthesised frontend texture the backend itself created |
| `UniformManager::m_fallbackMultisampleTextures` | `Renderer/UniformManager.h:308` | `UnorderedMap<Uint32, SharedPtr<ITextureObject>>` |
| `UniformManager::m_unboundStorageImageTextures` | `Renderer/UniformManager.h:317` | `UnorderedMap<Uint64, SharedPtr<ITextureObject>>` |
| `VkClearManager::m_aliveObjects` | `Renderer/VkClearManager.h:167` | `unordered_map<TextureIdentity, WeakPtr<ITextureObject>>` |
| `VkTextureManager::m_aliveObjects` | `Renderer/VkTextureManager.h:725` | `unordered_map<TextureIdentity, WeakPtr<ITextureObject>>` |
| `VkRenderPassManager` attachment records | `Renderer/VkRenderPassManager.h:45,51,330,359` | `WeakPtr<ITextureObject>` / `WeakPtr<RenderbufferObject>` |
| `VulkanRenderer` blit/clear helper programs | `Renderer/VulkanRenderer.h:378,379,380,388` | `SharedPtr<ProgramObject>` + two `SharedPtr<SamplerObject>` the backend **constructs itself** |
| DirectGLES raw-depth-fetch sampler | `DirectGLES/DirectGLES.cpp:55` (`static SharedPtr<SamplerObject> g_rawDepthFetchSamplerState`), built at `DirectGLES.cpp:150-160` | same pattern: backend-authored frontend object |

The last two rows are the P1 trap: these are frontend objects **created by the backend**, so under
disaggregation the server has no frontend to create them in. They need either a server-local
equivalent or a client-side create call.

### 2.3 The twin registries and the owner-keyed memos

`DirectGLES/Managers.h:271` `template <typename StateObject, typename BackendObject> class
StateBackendObjectRegistry` — hash map **keyed on the raw frontend object pointer**
(`using BackendMap = UnorderedMap<StateObject*, Entry>;`, `Managers.h:284`), each `Entry` holding
`{BackendPtr backend; StateWeakPtr stateRef;}` (`Managers.h:280-283`). Liveness is decided by
`entry.stateRef.expired()` (`Managers.h:318`, `Managers.h:339`).

Six instances, all reached by the draw path:

| registry | declared | keyed on |
|---|---|---|
| `VertexArrayImpl::g_backendVertexArrayObjects` | `Managers.h:803-804` | `VertexArrayObject*` |
| `TextureImpl::g_backendTextureObjects` | `Managers.h:1124-1125` | `ITextureObject*` |
| `FramebufferImpl::g_backendFramebufferObjects` | `Managers.h:1215-1216` | `FramebufferObject*` |
| `PrgramImpl::g_backendProgramObjects` | `Managers.h:1733-1734` | `ProgramObject*` |
| `SamplerImpl::g_backendSamplerObjects` | `Managers.h:1833-1834` | `SamplerObject*` |
| `RenderbufferImpl::g_backendRenderbufferObjects` | `Managers.h:1860-1861` | `RenderbufferObject*` |

On top of them, `DirectGLES/DirectGLES.cpp:83-115` `class TwinLookupMemo` — a direct-mapped memo
whose slot is `{StateObject* key; WeakPtr<StateObject> owner; BackendObject* twin;}`
(`DirectGLES.cpp:100-104`), indexed by Fibonacci-hashing the **frontend heap address**
(`DirectGLES.cpp:108-112`), validated by `OwnerEquals` (`DirectGLES.cpp:62-65`, a
`weak_ptr::owner_before` pair test). Three instances: `g_vaoTwinLookupMemo`
(`DirectGLES.cpp:122-123`), `g_programTwinLookupMemo` (`DirectGLES.cpp:124-125`),
`g_fboTwinLookupMemo` (`DirectGLES.cpp:130-131`); looked up at `DirectGLES.cpp:1204`, `2767`,
`2872` and stored at `1212`, `2775`, `2877`.

**Both mechanisms are pointer-identity based and cannot survive a process/thread split.** P1 has to
replace "frontend heap address + `weak_ptr` owner equality" with the wire handle plus a lifetime
id. The good news is that the frontend already exposes exactly that: `GetLifetimeId()` (§2.4).

### 2.4 Lifetime-id and version reads (the memo keys P1 must carry over the wire)

39 `GetLifetimeId()` reads:

| object kind | sites |
|---|---|
| program | `DirectGLES/DirectGLES.cpp:3333`, `3349`; `DirectVulkan/DirectVulkan.cpp:163`; `Renderer/UniformManager.cpp:482`, `696`, `2272`, `2384`, `2732`; `Renderer/VulkanRenderer.cpp:6044`, `6484`, `6524`, `6577`, `6648`, `6962` |
| texture | `Renderer/UniformManager.cpp:656`, `1746`, `1792`; `Renderer/VkClearManager.cpp:155`, `197`, `251`, `368`, `459`; `Renderer/VkRenderPassManager.cpp:642`; `Renderer/VkTextureManager.cpp:235`, `733` |
| sampler | `Renderer/UniformManager.cpp:654`, `1747`, `1793` |
| VAO | `Renderer/VertexInputStateFactory.cpp:48` (via `attr.Buffer`, actually a buffer); `Renderer/VulkanRenderer.cpp:3552`, `6107`, `6384`, `6965` |
| FBO | `Renderer/VkRenderPassManager.cpp:844`, `869`; `Renderer/VulkanRenderer.cpp:6112`, `6968` |
| buffer | `DirectGLES/Managers.cpp:2678`; `Renderer/VertexInputStateFactory.cpp:48` |

36 version reads:

| version | sites | what it gates |
|---|---|---|
| `BindingSlot::GetVersion()` (framebuffer slot) | `DirectGLES/DirectGLES.cpp:1614`, `1917`, `2745`, `2936` | the FBO-sync short-circuit (`g_fboSyncedSlotVersions`) |
| `BindingSlot::GetVersion()` (VAO index-buffer slot) | `DirectGLES/Managers.cpp:2307` | element-array rebind |
| `BufferObject::GetChangeSerial()` | `DirectGLES/Managers.cpp:794`, `1078`, `1151`, `1163`, `1203`, `1238`, `1245`, `1283`, `1468`, `1553`, `1580`, `1581`, `2679`; `Renderer/VkBufferManager.cpp:697`; `Renderer/VulkanRenderer.cpp:3849` | every buffer upload/re-upload decision on both backends |
| `ITextureObject::GetContentVersion()` | `DirectGLES/Managers.cpp:3649`, `3776`, `3826`, `4731`; `Managers.h:1012`; `Renderer/VkTextureManager.cpp:1646`, `1755`; `Renderer/VulkanRenderer.cpp:6293`, `7016` | texture upload short-circuit + the draw-time content-sum memo |
| `SamplerObject::GetVersion()` | `DirectGLES/Managers.cpp:4756`, `8534`; `Managers.h:1016`; `Renderer/UniformManager.cpp:655`; `Renderer/VkSamplerManager.cpp:401` | sampler twin re-sync |
| `ITextureObject::GetShapeVersion()` | `Renderer/VkTextureManager.cpp:1647`, `1756` | image recreate decision |
| `ITextureObject::GetTextureParamsVersion()` | 14 sites, e.g. `DirectGLES/Managers.cpp:3647`, `3777`, `3847` | texture-parameter re-push |
| `ProgramObject::GetBackendStateVersion()` / `GetLinkVersion()` / `GetImageUnitVersion()` / `GetUBOContentVersion()` / `GetBlockBindingVersion()` | `DirectGLES/DirectGLES.cpp:3335`, `3350`, `3540`, `2798`, `2799`, `3715`, `3401`; `Renderer/UniformManager.cpp:2273`; `DirectVulkan/DirectVulkan.cpp:165` | program twin re-sync + UBO re-upload |
| `VertexArrayObject::GetConfigVersion()` | `DirectGLES/DirectGLES.cpp:1258`, `2962`; `DirectGLES/Managers.cpp:2306` | VAO re-emit |
| `FramebufferObject::GetObjectVersion()` | `DirectGLES/DirectGLES.cpp:1615`, `1918`, `2937` | FBO twin re-sync |

### 2.5 `BufferBackendOps` — the reverse (frontend → backend) immediate-op table

Declared `MobileGL/MG_State/GLState/BufferState/BufferObject.h:76-123`; seven function pointers:
`Respecify`, `SubData`, `ResidentSubData`, `FlushMappedRange`, `OnDestroy`, `AcquirePersistentMap`,
`ReadbackFromGpu`. Every one takes `BufferObject&` — a **frontend object by reference**, and the
callee reads the frontend shadow through it.

| backend | table | registered | unregistered |
|---|---|---|---|
| DirectGLES | `g_glesBufferBackendOps` (`DirectGLES/Managers.cpp:1351-1361`, all seven filled, each wrapped in a `…Tracked` shim that bumps the buffer-mutation epoch) | `Managers.cpp:1374-1379` `RegisterBufferBackendOps()`, called from `DirectGLES/BackendObject_DirectGLES.cpp:849` and `DirectGLES/DirectGLES.cpp:10063` | `Managers.cpp:1381-1392`, also from `OnBackendContextDestroyed` (`Managers.cpp:1397`) |
| DirectVulkan | `g_vulkanBufferBackendOps` (`Renderer/VkBufferManager.cpp:106-113`, **six** filled — `ResidentSubData` is left null) | `Renderer/VkBufferManager.cpp:132` inside `VkBufferManager::Initialize` | `Renderer/VkBufferManager.cpp:139-141` inside `Shutdown` |

The epoch the GLES shims maintain (`Managers.cpp:1362-1372`, `CurrentBufferMutationEpoch` /
`BumpBufferMutationEpoch`) is itself a frontend-write-visibility signal the draw path memos read.

### 2.6 Backend → frontend **writes** (not pulls, but the same coupling, and P1 must not miss them)

These are calls the backend makes that mutate a frontend object. They are the mirror image of the
pull sites and a pipe with only a pull direction will silently drop them:

| method on frontend object | sites |
|---|---|
| `BufferObject::MarkGpuWritten()` | `DirectGLES/DirectGLES.cpp:510`, `1828`; `Renderer/UniformManager.cpp:1074`, `1230` |
| `BufferObject::SetBackendResource()` | `DirectGLES/Managers.cpp:1015`, `1482`; `Renderer/VkBufferManager.cpp:297` (+1) |
| `BufferObject::EnsureGpuResidentStorage()` | `Renderer/UniformManager.cpp:1072`, `1228`; `Renderer/VulkanRenderer.cpp:11326` |
| `BufferObject::WritebackFromBackend()` | `DirectGLES/Managers.cpp:1279` |
| `ITextureObject::AllocateStorage()` / `MarkStorageDirty()` / `TruncateMipmapLevels()` / `SetInternalFormat()` / `SetSamples()` / `SetFixedSampleLocations()` | `DirectGLES/DirectGLES.cpp:6326`, `6327`; `Renderer/UniformManager.cpp:1484`, `1489`, `1490`, `1496`, `1497`, `1498`, `1625`, `1628`, `1635` |
| `ProgramObject::SetDrawID()` / `SetBaseVertex()` / `SetBaseInstance()` / `SetViewportPassMask()` | `DirectGLES/DirectGLES.cpp:3683`, `3689`, `3677`, `3813` |
| `ProgramObject::AttachShader()` / `Link()` / `MarkUBOContentDirty()` / `SetBackendHashMemo()` | `Renderer/VulkanRenderer.cpp:4233`, `4234`, `4235`, `4313`, `4315`, `4665`, `8564`; `Renderer/ProgramFactory.cpp:3448` |
| `VertexArrayObject::SetBackendHashMemo()` / `SetBackendStateMemo()` / `SetBackendAuxMemo()` | `Renderer/VertexInputStateFactory.cpp:60`, `78`, `83` |
| `SamplerObject::SetWrapS/T/R`, `SetMinFilter`, `SetMagFilter`, `SetMipmapMode`, `SetLodRange` | `Renderer/VulkanRenderer.cpp:4270-4276` |
| `GLContext::RecordError()` | 6 sites, §3 accessor `RecordError` |
| `GLContext::InvalidateCompileEnv()` | 2 sites, §3 accessor `InvalidateCompileEnv` |

### 2.7 Frontend-object method surface reached through the pulled references

Counted by receiver-name heuristic over `MG_Backend/**` (excludes comment lines). This is the size
of the "handle + value block" problem P1 inherits once the arrow sites are replaced:

| frontend class | distinct methods called | call sites | top methods |
|---|---|---|---|
| `BufferObject` | 16 | 169 | `GetSize` 50, `MappedData` 42, `SyncPersistentMappedRange` 20, `GetChangeSerial` 15, `GetBackendResource` 11, `SyncGpuWrites` 6 |
| `ITextureObject` | 38 | 411 | `GetExternalIndex` 147, `GetTarget` 48, `GetFormat` 47, `GetTextureParamsVersion` 14, `GetStorageType` 11, `IsTextureView` 11, `GetLifetimeId` 10 |
| `ProgramObject` | 56 | 275 | `GetExternalIndex` 69, `GetLifetimeId` 14, `GetLinkStatus` 14, `GetUBOSize` 14, `GetUniformLocation` 12, `GetUniformSamplerOrImageUnitIndex` 10 |
| `FramebufferObject` | 9 | 115 | `GetAttachment` 39, `GetExternalIndex` 19, `IsDefaultFramebuffer` 18, `GetDrawBuffers` 16, `GetReadBuffer` 8, `GetObjectVersion` 7 |
| `SamplerObject` | 26 | 52 | `GetVersion` 5, `GetAllSamplerParameters` 3, then one-per-parameter getters (`GetWrapS/T/R`, `GetMinFilter`, …) |
| `VertexArrayObject` | 12 | 40 | `GetIndexBufferBindingSlot` 11, `GetAttribute` 6, `GetConfigVersion` 6, `GetBackendHashMemo` 4, `GetLifetimeId` 4 |

Total ≈ **1062** method calls on frontend objects across 157 distinct methods. `MGPResourceDesc`,
`MGPProgramDesc`, `MGPSamplerDesc`, `MGPFramebufferState` and `MGPVertexElements` in
`MG_Pipe/PipeFields.def` are the intended replacements; the field lists there are visibly narrower
than this surface, which is the second thing P1 has to reconcile.

---

## 3. (a) The `PipeInputs` accessor surface

Return types are from `MobileGL/MG_State/GLState/Core.h` (the `GLContext` class body starts at
`Core.h:61`). "sites" is the count from §0.2; full site lists are in §1.

### 3.1 Accessors that return a **handle to a frontend object** (must become an `MGPipeHandle`)

| accessor | signature (Core.h) | sites | verb paths served |
|---|---|---|---|
| `GetBufferBindingSlot` | `BindingSlot<BufferObject>& GetBufferBindingSlot(BufferTarget)` — `Core.h:78` | 29 | draw (indirect/parameter), dispatch (indirect), readback (PixelPack), buffer op (`SyncBoundBuffer`) |
| `GetBufferBindingPoint` | `BindingSlotRange1D<BufferObject>& GetBufferBindingPoint(BufferTarget, Uint)` — `Core.h:79` | 9 | buffer op (SSBO/UBO/atomic), xfb, program/uniform sync (descriptor) |
| `GetFramebufferBindingSlot` | `BindingSlot<FramebufferObject>& GetFramebufferBindingSlot(FramebufferTarget)` — `Core.h:473` | 19 (+5 via the fast cache, §2.1) | framebuffer sync, clear, blit, readback, draw, pipeline build |
| `GetTextureUnitObject` | `TextureUnit& GetTextureUnitObject(Int)` — `Core.h:120` | 19 | texture op, program/uniform sync (descriptor) |
| `GetImageTextureBinding` | `ImageTextureBinding& GetImageTextureBinding(Int)` (+ const overload) — `Core.h:121-122` | 7 | texture op (image unit), program/uniform sync (storage image/texel) |
| `GetBoundVertexArray` | `const SharedPtr<VertexArrayObject>& GetBoundVertexArray()` — `Core.h:102` | 12 | vertex sync, draw |
| `GetProgramForDraw` | `const SharedPtr<ProgramObject>& GetProgramForDraw()` — `Core.h:174` | 7 | draw, program/uniform sync |
| `GetProgramForDispatch` | `const SharedPtr<ProgramObject>& GetProgramForDispatch()` — `Core.h:179` | 3 | dispatch |
| `GetProgramObject` | `const SharedPtr<ProgramObject>& GetProgramObject(Uint)` — `Core.h:159` | 3 | program/uniform sync (`ShaderStorageBlockBinding`, `GetBackendProgramId`) |
| `GetTextureObject` | `const SharedPtr<ITextureObject>& GetTextureObject(Uint)` — `Core.h:110` | 1 | texture op (`VkTextureManager::SyncTextureAndGetDescriptor` liveness check) |
| `GetTransformFeedbackProgram` | `const SharedPtr<ProgramObject>& GetTransformFeedbackProgram() const` — `Core.h:329` | 3 | xfb |
| `GetCurrentVertexAttribute` | `const CurrentVertexAttributeValue& GetCurrentVertexAttribute(Uint)` — `Core.h:106` | 2 | vertex sync |

**The four in bold below return a reference to a mutable slot, not a value**, so a `PipeInputs`
accessor cannot simply copy them: `GetBufferBindingSlot`, `GetBufferBindingPoint`,
`GetFramebufferBindingSlot`, `GetTextureUnitObject`. Every call site immediately follows with
`.GetBoundObject()`, `.GetRange()`, `.GetVersion()` or `.GetSamplerObject()`; those are the value
blocks P1 must materialise. `PipeFields.def` already models them as `MGPVertexBuffer`,
`MGPIndexBuffer`, `MGPIndirectBuffers`, `MGPBufferRange`, `MGPShaderBuffers`, `MGPBoundView`,
`MGPSamplerViews`, `MGPImageView`, `MGPFramebufferState`.

### 3.2 Accessors that return a **plain value** (a `PipeInputs` field can hold them verbatim)

| accessor | return type (Core.h line) | sites |
|---|---|---|
| `GetActiveTextureUnit` | `Int` (146) | 8 |
| `GetBlendColor` | `const FloatVec4&` (278) | 1 |
| `GetBlendEquationIndexed` | `void(Uint, BlendEquation&, BlendEquation&)` — **out-params** (255) | 1 |
| `GetBlendFuncIndexed` | `void(Uint, BlendFactor&×4)` — **out-params** (250) | 1 |
| `GetBufferBindingPointCount` | `constexpr SizeT(BufferTarget)` (80) | 3 |
| `GetTouchedBufferBindingPointCount` | `SizeT(BufferTarget)` (86) | 2 |
| `GetClampReadColor` | `GLenum` (235) | 1 |
| `GetClearColor` | `const FloatVec4&` (272) | 1 |
| `GetClearDepth` | `Float` (274) | 1 |
| `GetClearStencil` | `Uint32` (276) | 1 |
| `GetColorMaskIndexed` | `BoolVec4(Uint)` (270) | 6 |
| `GetCullFaceMode` | `CullFaceMode` (294) | 1 |
| `GetDepthFunc` | `DepthTestFunc` (259) | 1 |
| `GetDepthMask` | `Bool` (261) | 5 |
| `GetDepthRangeIndexed` | `const FloatVec2&(Uint)` (282) | 1 |
| `GetLineWidth` | `Float` (211) | 1 |
| `GetLogicOp` | `LogicOperation` (257) | 1 |
| `GetMaxTouchedTextureUnit` | `Int` (126) | 1 |
| `GetMinSampleShadingValue` | `Float` (289) | 1 |
| `GetPatchDefaultInnerLevel` | `const FloatVec2&` (219) | 3 |
| `GetPatchDefaultOuterLevel` | `const FloatVec4&` (217) | 3 |
| `GetPatchVertices` | `Uint` (215) | 3 |
| `GetPixelStoreParameters` | `PixelStoreParameters(Bool isUnpack)` (292) | 6 (all with `isUnpack=false`) |
| `GetPolygonModeFront` | `GLenum` (237) | 1 |
| `GetPolygonOffsetFactor` | `Float` (222) | 1 |
| `GetPolygonOffsetUnits` | `Float` (223) | 1 |
| `GetPrimitiveRestartIndex` | `Uint32` (240) | 3 |
| `GetProvokingVertexMode` | `ProvokingVertexMode` (298) | 1 |
| `GetRenderStateParameters` | `const RenderStateParameters&` (205) | 11 |
| `GetScissorBox` | `const IntVec4&` (300) | 3 |
| `GetStencilState` | `const StencilFaceState&(StencilFace)` (266) | 8 |
| `GetViewport` | `IntVec4` (207) | 1 |
| `GetViewportIndexed` | `const FloatVec4&(Uint)` (209) | 1 |
| `IsCapabilityEnabled` | `Bool(CapabilityInput)` (242) | 29 |
| `IsCapabilityEnabledIndexed` | `Bool(CapabilityInput, Uint)` (244) | 1 |
| `IsTransformFeedbackActive` | `Bool` (325) | 5 |
| `IsTransformFeedbackPaused` | `Bool` (326) | 2 |
| `HasOpenTransformFeedbackSpan` | `Bool(Uint64 lifetimeId)` (450) — **NOT in PipeFilled.inc** | 1 |
| `ValidateProgramName` | `Bool(Uint) const` (157) — pseudo-call `kClientResolved` | 3 |

The 14 distinct `CapabilityInput` values the 30 capability reads ask for (P1's `CapabilityBits`
block, which `PipeFields.def` already names in `MGP_FIELDS_ResidualValueBlock`): `FramebufferSrgb`,
`RasterizerDiscard`, `PrimitiveRestart`, `PrimitiveRestartFixedIndex`, `Multisample`, `SampleMask`,
`CullFace`, `DepthTest`, `PolygonOffsetFill`, `ColorLogicOp`, `StencilTest`, `SampleShading`,
`ScissorTest`, `Blend` (only via `IsCapabilityEnabledIndexed`).

### 3.3 Accessors that return a **version / generation counter** (memo keys)

| accessor | return type (Core.h line) | sites | memo it keys |
|---|---|---|---|
| `GetPipelineStateVersion` | `Uint` (204) | 3 | Vulkan pipeline cache + fast-path snapshot |
| `GetRenderStateParametersVersion` | `Uint` (202) | 2 | GLES render-state shadow, Vulkan dynamic-state tail |
| `GetTextureBindGeneration` | `Uint64` (130) | 5 | per-draw sampled-texture set |
| `GetSamplingResolutionGeneration` | `Uint64` (136) | 9 | mipmap-completeness re-resolve |
| `GetTextureContextId` | `Uint64` (143) | 6 | disambiguates a recreated context at the same address |
| `GetTransformFeedbackGeneration` | `Uint64` (334) | 1 | xfb counter-buffer rebind |
| `GetTransformFeedbackCapturedVertices` | `Uint64` (357) | 1 | GLES scatter-capture record count |
| `GetTransformFeedbackPausedPrimitiveCounter` | `Uint64` (349) | 2 | xfb primitives-written query |
| `GetBoundTransformFeedbackLifetimeId` | `Uint64` (441) — **NOT in PipeFilled.inc** | 1 | Vulkan xfb counter-slot ownership |

### 3.4 Non-read accessors (reverse channel / client-resolved)

| accessor | signature | sites | Coverage.def classification |
|---|---|---|---|
| `RecordError` | `void(ErrorCode, UniquePtr<ErrorInfo>)` — `Core.h:66` | 6 | `kReverseChannel` |
| `InvalidateCompileEnv` | `void()` — `Core.h:506` | 2 | `kClientResolved` |
| `ValidateProgramName` | `Bool(Uint) const` — `Core.h:157` | 3 | `kClientResolved` |

### 3.5 Reconciliation with `PipeFilled.inc`'s 61 fields

| | |
|---|---|
| fields in `PipeFilled.inc` (`generated/PipeFilled.inc:28-90`, asserted at `:92`) | **61** |
| distinct accessors read by the backends on this tree | **61** (+`.get()`) |
| in `PipeFilled.inc` but **never read by a backend** | 1 — `GetBoundTransformFeedbackName` (`PipeFilled.inc:32`) |
| read by a backend but **absent from `PipeFilled.inc`** | 2 — `GetBoundTransformFeedbackLifetimeId`, `HasOpenTransformFeedbackSpan` |
| corrected field count for P1 | **62** |

Additional structural gaps that PipeInputs-as-generated does not model, all of which have real
sites:

1. **`.get()` on the context itself** (`DirectGLES.cpp:147`) has no accessor to map to; the whole
   `GetFramebufferBindingSlotFast` cache (`DirectGLES.cpp:142-155`, 5 call sites) must be deleted
   or rebuilt on handles.
2. **Slot version reads** (`BindingSlot::GetVersion()`, 5 sites, §2.4) ride on
   `GetFramebufferBindingSlot`/`GetBoundVertexArray` and are not separate fields today;
   `MGP_FIELDS_MGPBindRenderState` has a `Version` and `MGP_FIELDS_MGPFramebufferState` has a
   `ContentHash`, but no field carries a framebuffer **binding-slot** version.
3. **`GetBufferBindingSlot` is polymorphic over `BufferTarget`** — `Coverage.def:37-42` already
   flags this ("its rows split across set_vertex_buffers, set_index_buffer, set_indirect_buffers
   and set_shader_buffers once the inventory carries the target argument (P1)"). The five targets
   actually asked for are `DrawIndirect` (13 sites), `Parameter` (4), `PixelPack` (7),
   `DispatchIndirect` (1), plus a runtime `target` parameter at `DirectGLES.cpp:518`
   (`SyncBoundBuffer`, called with `DrawIndirect` @`:664` and `DispatchIndirect` @`:691`).
4. **`GetBufferBindingPoint` is likewise polymorphic**: `TransformFeedback` (3), `ShaderStorage`
   (1), `AtomicCounter` (1), `Uniform` (2), plus a runtime `target`/`bufferTarget` at
   `DirectGLES.cpp:370` and `UniformManager.cpp:1194`.

