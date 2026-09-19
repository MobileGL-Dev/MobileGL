# Scout — the frontend (MG_State/GLState + MG_Impl/GLImpl) as P4a's client emission must read it

Tree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg` @ `37da3c3a`
(feat/disaggregated, P3a closed). Every path below is relative to `MobileGL/` unless it starts
with `docs/`. Every line number was opened in this tree; where a file is large I name the exact
range I read.

Read alongside: `~/w7/notes/p3a/INTEGRATOR-DECISIONS.md` (ID-1..24). The rulings that bind P4a
directly are ID-11/ID-13 (G5 byte-identical functions — none of them are in P4a's files),
ID-12/ID-8b (**the applier keeps object records across make-current; only
`MGPipeApplierReleaseObjectRecords` drops them; the client therefore needs no re-publication on
`FreshlyPrimed`, but must say so as a rule**), ID-18/ID-21 (**leak-at-exit storage: every MGPipe
process singleton is heap-constructed and never destroyed; a new P4a singleton MUST follow**),
and ID-19 (P3a already costs +699/+422 ns/draw; P4a's per-verb emission budget is tight).

---

## 0. The one-paragraph shape

P4a's kinds are **Framebuffer, Texture, Renderbuffer, SamplerCso/SamplerViewCso, ShaderCso**
(`MG_Pipe/MGPipeHandles.h:24-40`). Unlike P3a's buffers, **five of the six already raise a death
notice** and **all six already have a lifetime id**; what none of them has is a client-minted
handle, a client emitter, or an applier record. The frontend versions the emitters need already
exist and are already read by the tracker's shutters (`MG_Impl/Pipe/Tracker.h:303-312`) — bits
11..14 (`NewFramebuffer`, `NewSamplerViews`, `NewSamplers`, `NewShaderImages`) plus bits 6..8
(`NewShader`, `NewShaderBindings`, `NewGlobalConstants`) are **computed, latched and counted
today and emitted by nobody**. P4a is therefore mostly "wire the emitters onto shutters that
already fire", plus four genuinely new pieces of frontend work: the resolved `ReadSurface`, the
framebuffer `Complete` bit, the texture region list on the wire, and the program archive blob.

---

## 1. The object classes: ids, versions, constructors, destructors

### 1.1 Summary table (the hook points)

| kind | class + header | lifetime id | ctor | dtor / death | already raises |
|---|---|---|---|---|---|
| Framebuffer | `FramebufferObject`, `MG_State/GLState/FramebufferState/FramebufferObject.h:105-191` | `m_lifetimeId` `:176`, `GetLifetimeId()` `:165`, allocator `FramebufferObject.cpp:20-24` | `FramebufferObject.cpp:126-135` | `FramebufferObject.cpp:27-37` (`#if MOBILEGL_PIPE_PUSH` only) | `NotifyStateObjectDestroyed(MGPipeKind::Framebuffer, …)` `:36` |
| Texture | `TextureObjectBase : ITextureObject`, `TextureState/TextureObject.h:117-234` | `m_lifetimeId` `:205`, `GetLifetimeId()` decl `:84` / def `TextureObject.cpp:334-336`, allocator `TextureObject.cpp:19,26-28` | `TextureObject.cpp:56-...` | `TextureObject.cpp:31-41` — **virtual, declared on the BASE** so 2D/3D/cube/buffer/view all announce once (`TextureObject.h:120-128`) | `MGPipeKind::Texture` `TextureObject.cpp:40` |
| Renderbuffer | `RenderbufferObject`, `RenderbufferState/RenderbufferObject.h:23-71` | `m_lifetimeId` `:64`, `GetLifetimeId()` `:58`, allocator `RenderbufferObject.cpp:21,24-26` | `RenderbufferObject.cpp:28` | `RenderbufferObject.cpp:31-41` | `MGPipeKind::Renderbuffer` `:40` |
| Sampler | `SamplerObject`, `SamplerState/SamplerObject.h:16-79` | `m_lifetimeId` `:76`, `GetLifetimeId()` `:63`, allocator `SamplerObject.cpp:20-24` | `SamplerObject.cpp:26-27` | `SamplerObject.cpp:30-40` | `MGPipeKind::SamplerCso` `:39` |
| Program | `ProgramObject`, `ProgramState/ProgramObject.h:25-...` | `m_lifetimeId` `:1353`, `GetLifetimeId()` `:1262`, allocator `ProgramObject.cpp:26-30` | inline `ProgramObject.h:55` | `ProgramObject.cpp:32-45` (`CancelLink()` then the notice) | `MGPipeKind::ShaderCso` `ProgramObject.cpp:43` |
| Shader | `ShaderObject`, `ProgramState/ShaderObject.h:24`, dtor `:52` | **NONE** | — | no notice | — |
| ProgramPipeline | `ProgramPipelineObject`, `ProgramState/ProgramPipelineObject.h:19-170` | **NONE** | — | no notice | — (composites are ordinary `ProgramObject`s, §9.4) |
| Buffer (P3a, the model) | `BufferState/BufferObject.cpp` | `g_nextBufferLifetimeId` `:20`, `AllocateLifetimeId` `:23-25` | `:35-47` — **mints the handle unconditionally, emits create only if the subsystem is on** | `:49-65` — `MGPipeEmitResourceDestroyAndFree` returns the latch, and the legacy `OnDestroy` runs only when it says false | emits `resource_destroy` itself (no notice) |
| VAO (P3a, the CSO model) | `VertexArrayState/VertexArrayObject.cpp:41-62` | | | `MGPipeEmitVertexElementsDestroyAndFree(m_lifetimeId)` — **backend-neutral, client-side**, emit → notice → free slot, in that fixed order | |

### 1.2 The two death shapes, and which one each P4a kind takes

`MG_State/GLState/StateObjectDeathNotice.h` (whole file, 62 lines) is the P2 mechanism: one
`StateObjectDeathOps{ void(*OnDestroyed)(MGPipeKind, Uint64 lifetimeId) }` (`:39-43`), a plain
non-atomic global `g_stateObjectDeathOps` (`:45`, written at backend bring-up, read from
destructors — both on the context thread), `SetStateObjectDeathOps` `:47`, and
`NotifyStateObjectDestroyed` `:55-59`. Declares nothing at all in a pull build (`:12`, `:62`).

**The P3a ruling P4a inherits (ID-15 C-1, `MG_Pipe/PipeMutation.h:127-141`):** a death path that
lives only in a backend's death-ops table **is no path at all under a backend that installs
none** — Magma installs none — so every VAO leaked its slot and its ~1.3 KB applier record and
Fatal'd past 65536 slots. The fix was `MGPipeEmitVertexElementsDestroyAndFree`, a **client-side**
helper taking the lifetime id (the object is already gone) that emits `delete_*`, raises the
notice, and frees the slot in that fixed order (`PipeMutation.h:136-141`,
`MG_Impl/Pipe/PipeFill.cpp:801-...`).

**Therefore: for all five P4a kinds the notice at the destructor is NOT sufficient.** Each
destructor must call a client helper of the same shape. The five call sites are the five `#if
MOBILEGL_PIPE_PUSH` destructor bodies listed in 1.1 — they already exist, so P4a edits five
existing lines rather than adding five destructors (which would resize the pull build's symbol
set and break G1).

### 1.3 The per-object version counters P4a's shutters and records need

**Framebuffer** (`FramebufferObject.h`):
- `m_attachmentVersions` `:178`, one `Uint16` per attachment point (`FramebufferAttachmentTypeCount`
  = 41 entries, `:24-71`), read whole via `GetAllFramebufferAttachmentVersions()` `:152-154`.
- `m_objectVersion` `:190`, `GetObjectVersion()` `:156` — bumped by `BumpAttachmentVersion`
  (`FramebufferObject.cpp:231-235`), by `SetReadBuffer` (`:205-210`) and by the five
  `Set Default*` setters (macro `:216-229`). **`SetDrawBuffer` bumps only through
  `BumpAttachmentVersion(buffer)` (`:195-199`) — i.e. it versions the *new* attachment slot, not
  the draw-buffer array**, which is a real quirk: `glDrawBuffers` writing `NONE` over `COLOR0`
  bumps `m_attachmentVersions[None]`, not `[Color0]`.
- `m_drawBuffers` `:180` (`Array<FramebufferAttachmentType, MAX_DRAW_BUFFERS=kMGMaxDrawBuffers>`,
  `:107,113`), `m_readBuffer` `:181`, defaults seeded in the ctor `:126-135`
  (`BackLeft` for name 0, `Color0` otherwise).
- `FramebufferAttachmentObject` `:74-103` is the attachment struct: `SharedPtr<ITextureObject>`
  **or** `SharedPtr<RenderbufferObject>` (`:96-97`), `m_textureUploadTarget` `:98`, `m_textureLevel`
  `:99`, `m_textureLayer` `:100`, `m_layered` `:101`, `m_isValid` `:102`. `IsComplete()`
  `FramebufferObject.cpp:89-99`, `GetSize()` `:101-119` (resolves `Unknown` upload target to
  `GetUploadTargets()[0]`).
- **`FramebufferState`** `FramebufferState/FramebufferState.h:15-45`: `m_framebufferObjects`
  (`UnorderedMap<Uint, SharedPtr<…>>`) `:41`, `m_bindingSlots[Draw|Read]` `:43-44`,
  `NoteAttachmentChanged`/`GetAnyAttachmentGeneration` `:33-34` under `#if MOBILEGL_PIPE_PUSH`
  `:28-40`. `MarkFramebufferObjectForDeletion` `FramebufferState.cpp:59-72` rebinds any slot
  holding the victim to FBO 0 and erases the map entry.

**Texture** (`TextureState/TextureObject.h`): `ITextureObject` `:23-115` is the interface (39
pure virtuals); `TextureObjectBase` `:117-234` the storage-free base; `TextureObjectMipmap`
`:236-303` adds the level API; `TextureObjectWithOneMipmap` `:344-378` holds
`MipmapUploadTargetArray<1> m_textureStorage` `:377`. Concrete leaves: `TextureObject1D/2D/3D.h`,
`TextureObject2DCube.h`, `TextureObjectBuffer.h`, `TextureObjectView.h`.
Three independent counters, all on the base:
- `m_textureParamsVersion` (Uint16) `:213`, `GetTextureParamsVersion()` `:158` — the
  glTexParameter family.
- `m_shapeVersion` (Uint64, **starts at 1**) `:217`, `GetShapeVersion()` `:160`, bumped **only**
  by `BumpShapeVersion()` (`:202`, def `TextureObject.cpp:44-54`), which also calls
  `pGLContext->BumpSamplingResolutionGeneration()` `:53`. Invalidates the completeness memo
  `m_completeMemoShapeVersion[2]` / `m_completeMemoValue[2]` `:220-221`, read by
  `IsMipmapCompleteForFilterCached` `TextureObject.cpp:298-307`.
- `m_contentVersion` (Uint64, **starts at 1**) `:224`, `GetContentVersion()` `:159`, bumped by
  `MarkStorageDirty(dirty=true)` (`TextureObject.cpp:379-386`), `MarkStorageDirtyRegion`
  (`:392-398`) and `BumpContentVersion()` (`:309-312`).
- Other state the descriptor needs: `m_internalFormat` `:207`, `m_sampler` `:208` (**every texture
  owns a `SamplerObject(0)`**, `TextureObject.cpp:58`), `m_swizzleParams` `:209`, `m_levelRange`
  `:211` (`{0,1000}`), `m_immutableLevels` `:212`, `m_depthStencilTextureMode` `:225`,
  `m_viewMinLevel/NumLevels/MinLayer/NumLayers` `:228-231`, `m_samples` `:232`,
  `m_fixedSampleLocations` `:233`.
- Views: `TextureObjectView.h:44-135`, `m_storageOwner` `:134`, `GetViewStorageOwner()` override
  `:49`; the base returns a static null `TextureObject.cpp:338-343`. `IsTextureView()`
  `TextureObject.h:102`. A view is composed onto the ROOT at creation, so one hop always reaches
  storage (`:90-101`) — that is exactly `MGPResourceDesc::ViewOf`.
- Texture buffers: `TextureObjectBuffer.h:16-48` — `m_bufferBindingSlot` `:46`,
  `m_bufferRangeOffset` `:47`, `m_bufferRangeSize` `:48` with `kWholeBuffer = ~0` `:28`,
  `GetBufferRangeSizeInBytes()` `:35-41` **resolves live against the buffer's current size** —
  which is exactly the `kMGPipeWholeBuffer` / "resolved live" contract of
  `MGPipeTypes.h:173-177`.
- `TextureState` `TextureState/TextureState.h:39-173`: `MAX_TEXTURE_IMAGE_UNITS = 192` `:42`,
  `MAX_PER_STAGE_TEXTURE_IMAGE_UNITS = 32` `:47`; `m_textureUnits` `:166`,
  `m_imageTextureBindings` `:167`, `m_textureObjects` `:169`, `m_defaultTextureObjects` `:172`
  (name 0, one per target, immortal). `ImageTextureBinding` `:18-37` = `{Texture, Level, Layered,
  Layer, Access, Format, Version}` with `Bind()` `:27-36` bumping `Version`.
- `TextureUnit` `TextureState/TextureUnit.h:15-26`: `m_slots[TextureTargetCount]` `:24` +
  `m_sampler` `:25`; `SetSamplerObject` `TextureUnit.cpp:28-40` bumps
  `pGLContext->BumpTextureBindGeneration()` `:39`.

**Renderbuffer** — **the outlier: no version at all.** `RenderbufferObject.h:63-70` has
`m_externalIndex`, `m_lifetimeId`, `m_internalFormat`, `m_width`, `m_height`, `m_samples`,
`m_allocated`, `m_componentSizes` and nothing else. `SetInternalFormat`
(`RenderbufferObject.cpp:96-99`), `AllocateStorage` (`:101-105`) and `SetSamples` (`:107-109`)
write the members and **bump nothing and notify nothing**. See §5.2 — this is a real P4a hole.

**Sampler** — `m_version` (Uint16) `SamplerObject.h:77`, `GetVersion()` `:58`;
`BumpVersion()` `:73` (def `SamplerObject.cpp:43-54`) is the **only** writer, called by all 14
setters (`:56-...`), each of which early-outs on an unchanged value. It does three things:
`++m_version`, `pGLContext->BumpSamplingResolutionGeneration()` (`:50`), and
`MGP_NOTE_AGGREGATE(TextureParams)` (`:53`). `m_samplerParameters` `:78` is the whole
`SamplerParameters` POD that `MGPSamplerDesc::Parameters` ships byte-for-byte
(`MGPipeTypes.h:283-287`; `docs/Disaggregated/ARCHITECTURE.md:4.1` insists `borderColorForm`
travels with it). `SamplerState` `SamplerState.h:17-31` is just a map + `IndexGenerator`.

**Program** — five counters, all read by the tracker (`Tracker.h:251-260`):
- `m_linkVersion` (mutable Uint32) `ProgramObject.h:1418`, `GetLinkVersion()` `:793` — bumped
  **only** by `BumpLinkObservableVersions()` (decl `:1340`), called at Link()'s prologue and at
  both publish sites (`ProgramObject.cpp:72`, `:119`).
- `m_backendStateVersion` (mutable) `:1399`, `GetBackendStateVersion()` `:790`.
- `m_blockBindingVersion` `:1401`, `GetBlockBindingVersion()` `:1013`, bumped by
  `SetUniformBlockBinding` `:1020-1027` (and the storage-block setter).
- `m_imageUnitVersion` `:1394`, `GetImageUnitVersion()` `:855`, bumped inside
  `SetUniformSamplerOrImageUnitIndex` **only for image uniforms** `:847-849`.
- `m_uniformWriteSetVersion` `:1393`, `GetUniformWriteSetVersion()` `:601`.
- `m_uboContentVersion` (mutable) `:1417`, `GetUBOContentVersion()` `:740`,
  `MarkUBOContentDirty()` `:741-743` — **`~0u` is reserved as the backends' "never uploaded"
  sentinel and the wrap skips it**.
- `m_artifacts` `:1426` and `m_spirv` `:1429` are private and reachable only through
  `Artifacts()` / `Spirv()`, which join `m_pendingLink` `:1434` / `m_pendingSpirv` `:1438`
  (invariant I5, `:1264-1275`).

---

## 2. The mutation entry points, per family

Frontend GL entry points live in `MG_Impl/GLImpl/<Family>/GL_<Family>.cpp` in a **two-layer
shape**: a `Foo_State(...)` function that mutates `MG_State` and validates, and a public `Foo(...)`
that calls it then dispatches through `MG_Backend::gBackendFunctionsTable.GL.Foo` behind an
`MGP_FILL(Verb)`. P4a's emission sites are the `_State` bodies (mutation) and, for the verbs,
the `MGP_FILL` point.

### 2.1 Framebuffer — `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp` (182 KB), decls
`GL_Framebuffer.h:16-96`

- Attach: `FramebufferTexture{,1D,2D,3D,Layer}` `GL_Framebuffer.h:44-49`,
  `NamedFramebufferTexture{,1D,2D,3D,Layer}` `:50-55`, `FramebufferRenderbuffer` `:34`,
  `NamedFramebufferRenderbuffer` `:35-36`. All land in `FramebufferObject::AttachTexture`
  (`FramebufferObject.cpp:137-142`) / `AttachRenderbuffer` (`:144-148`) / `Detach` (`:150-153`),
  each ending in `BumpAttachmentVersion` (`:231-235`) → `++m_attachmentVersions[type]`,
  `++m_objectVersion`, `MGP_NOTE_AGGREGATE(FramebufferAttachment)`.
- Draw/read buffers: `DrawBuffer` / `DrawBuffers` / `ReadBuffer` `:78-80` and their `Named*`
  forms `:56-58`. Implementations: `DrawBuffersForFramebuffer_State`
  `GL_Framebuffer.cpp:1719-1830`, `DrawBuffers_State` `:1831-1836`, `DrawBuffer_State`
  `:1838-1848`, `ReadBufferForFramebuffer_State` `:1850-1899` (ends at `fbo->SetReadBuffer(attType)`
  `:1898`), `ReadBuffer_State` `:1901-1906`, `NamedFramebufferDrawBuffers_State` `:1908-1916`.
- Default geometry (`ARB_framebuffer_no_attachments`): `FramebufferParameteri` `:71`,
  `NamedFramebufferParameteri` `:73` → the five setters `FramebufferObject.h:146-150`, defined by
  the macro at `FramebufferObject.cpp:216-229`.
- Renderbuffer storage: `RenderbufferStorage` / `RenderbufferStorageMultisample` `:23-25`,
  `NamedRenderbufferStorage{,Multisample}` `:30-32`; bodies
  `RenderbufferStorageMultisample_State` `GL_Framebuffer.cpp:817-842`,
  `AllocateRenderbufferStorage_State` `:844-858`, `RenderbufferStorage_State` `:860-866`.
  **None of them notify anything** (§5.2).
- `BindFramebuffer` `:85`, `BindRenderbuffer` `:38`, `DeleteFramebuffers` `:81`,
  `DeleteRenderbuffers` `:37`, `Blit{,Named}Framebuffer` `:75-77,83-84`, `Invalidate*` `:61-66`,
  `CheckFramebufferStatus` `:82` / `CheckNamedFramebufferStatus` `:69`, `ReadPixels`/`ReadnPixels`
  `:16-18`, `ClearBuffer*` `:19-22` / `ClearNamedFramebuffer*` `:59-60,67-68`.
- `DefaultFramebufferInfo` `GL_Framebuffer.h:87-96` holds the default FBO plus its three synthetic
  attachments; `pDefaultFramebufferInfo->defaultFBO` is the identity every "is this the default"
  test compares against (`GL_Framebuffer.cpp:1834`, `:1844`, `:1904`). This is exactly the four
  comparisons `MGPipeHandles.h:67-73` retires into `kMGPipeDefaultFramebuffer{0,1}`.

### 2.2 Texture — `MG_Impl/GLImpl/Texture/GL_Texture.cpp` (426 KB), decls `GL_Texture.h`

Storage / respecify (each → `MGPResourceDesc` create-or-respecify):
- `TexImage3D_State` `:2352`, `TexImage2D_State` `:2524`, `TexImage1D_State` `:2678` — each
  `AllocateStorage(...)` (`:2473`, `:2621`, `:2734`), optional `UpdateMipmapSubData` (`:2515`,
  `:2668`, `:2754`), then `MarkStorageDirty(..., true)` (`:2518`, `:2674`, `:2758`).
- `TexImage2DMultisample_State` `:2308`, `TexImage3DMultisample_State` `:2264`.
- `CompressedTexImage{1,2,3}D_State` `:4984`, `:4904`, `:4830` (`AllocateStorage` `:4895`/`:4975`,
  `MarkStorageDirty` `:4901`/`:4981`).
- `TexStorage{1,2,3}D` `:6002`, `:6027`, `:6052` — allocate every level with
  `MarkStorageDirty(..., false)` (`:5554`, `:5628`, `:5698`) then `TruncateMipmapLevels`
  (`:5564`, `:5644`, `:5714`). `TexStorage{2,3}DMultisample` `:6110`, `:6121`, shared validator
  `TexStorageMultisample_State` `:6098`.
- DSA `TextureStorage*` / `TextureSubImage*` / `CompressedTextureSubImage*` — `GL_Texture.h:41-60`.
- `TextureView` `:5838` (decl `GL_Texture.h:83`) → `TextureState::CreateTextureViewObject`
  (`TextureState.h:55-58`).
- `TextureBuffer` / `TextureBufferRange` `GL_Texture.h:73-74`.
- The generated-mip storage grow: `GL_Texture.cpp:528-539` (`AllocateStorage`,
  `MarkStorageDirty(false)`, `TruncateMipmapLevels`, then **`BumpContentVersion()` `:539`** —
  the level SET grew without any CPU byte being dirty).

Sub-image (each → `MGPSubData` + regions):
- `TexSubImage3D_State` `:1725` → `MarkStorageDirtyRegion(target, level, {x,y,z}, {w,h,d})`
  `:1820-1821`.
- `TexSubImage2D_State` `:1825` → `MarkStorageDirtyRegion(..., {x,y,0}, {w,h,1})` `:1940`.
- `TexSubImage1D_State` `:1945` → `:2007`.
- `CompressedTexSubImage{1,2,3}D_State` `:4818`, `:4658`, `:4489`.
- `ClearTexImage` `:1102` / `ClearTexSubImage` `:1118` → `MarkStorageDirty(..., true)` `:1006`,
  `:1097`.
- Copy family: `CopyTexImage2D_State` `:4389` / `_Backend` `:4467`, `CopyTexSubImage2D_Backend`
  `:4028`, `CopyTexSubImage3D_State` `:3986`, `CopyTexSubImage1D_State` `:4372`,
  `CopyTexImage1D_State` `:4474`, DSA `CopyTextureSubImage{1,2,3}D` `GL_Texture.h:133-136`.
- `CopyImageSubData` `GL_Texture.h:143`, validator `ValidateCopyImageSubData_State`
  `GL_Texture.cpp:4263`, backend hop `CopyImageSubData_Backend` `:4034` (verb
  `CopyImageSubData`, class `kBlitOrCopy`, `MG_Pipe/FillPoints.def:113`) — the payload is
  `MGPCopyRegion` (`MGPipeTypes.h:708-716`).

Parameters (each → `MGPTextureParams`, or the sampler-owned half):
- `TexParameter{f,i,fv,iv,Iiv,Iuiv}_State` `GL_Texture.cpp:2012`, `:2110`, `:2126`, `:2163`,
  `:2194`, `:2224`; DSA `TextureParameter*` `GL_Texture.h:62-67`.
- They land on `TextureObjectBase::Set*`, each of which bumps `m_textureParamsVersion` and fires
  `MGP_NOTE_AGGREGATE(TextureParams)`: `SetInternalFormat` `TextureObject.cpp:98-119` (also
  `BumpShapeVersion` `:116` and a `BumpTextureBindGeneration` for the default-texture
  Unknown↔defined transition `:108-113`), `SetBorderColor` `:136-145`, `SetBorderColorI`
  `:151-160`, `SetBorderColorUI` `:166-175`, `SetSwizzleParam` `:202-225`, `SetSwizzleParamRGBA`
  `:227-233`, `SetBaseLevel` `:239-252` (+`BumpShapeVersion` `:251`), `SetMaxLevel` `:254-264`
  (+`:263`), `SetImmutableLevels` `:274-284`, `SetSamples` `:318-322`, `SetFixedSampleLocations`
  `:328-332`, `SetDepthStencilTextureMode` **inline in the header** `TextureObject.h:188-193`.
- **TEXTURE_BORDER_COLOR / MIN_FILTER / WRAP_* are NOT texture-object state**: they live on the
  texture's own `SamplerObject` (`TextureObject.cpp:125-179`), so a `glTexParameteri(GL_TEXTURE_
  MIN_FILTER)` runs `SamplerObject::BumpVersion` and fires `MGP_NOTE_AGGREGATE(TextureParams)`
  through *that* path.

Mipmap generation: `GenerateMipmap` `GL_Texture.cpp:6690-6709` (`EnsureGeneratedMipmapStorage
Allocated` `:6707` then `GenerateMipmap_Backend` `:1623`), `GenerateTextureMipmap` `:6711-6720`
(binds temporarily, `WithTemporarilyBoundNamedTexture`), guard `ValidateGenerateMipmapTexture`
`:6675-6688`, auto-gen hook `MaybeAutoGenerateMipmap` `:1628`. Verb `GenerateMipmap`, class
`kTextureOp` (`FillPoints.def:114`), payload `MGPMipPlan` (`MGPipeTypes.h:741-745`).

Binds: `BindTexture_State` `:4995`, `BindTextureUnit` `:6296`, `BindTextures` `:6395`,
`BindImageTexture` `:6612-6670`, `BindImageTextures` `:6418`. **`BindImageTexture` is the one
that is already a verb** (`FillPoints.def:122`, class `kTextureOp`): it writes
`GetImageTextureBinding(unit).Bind(...)` `:6665-6666`, calls `NoteTextureUnitTouched(unit)`
`:6667`, then `MGP_FILL(BindImageTexture)` `:6668` and dispatches `:6669`.
`glBindTexture`/`glBindSampler` are **not** verbs — they route through
`TextureState::NoteUnitTouched` (`TextureState.h:78-100`) only.

Readback: `GetTexImage_State` `:5251` / `_Backend` `:5079`, `GetCompressedTexImage_State` `:3801`,
`GetTextureSubImage` `GL_Texture.h:75-76`, `CopyTextureImageToClientOrPBO_State`
`GL_Texture.cpp:5375`.

### 2.3 Sampler — `MG_Impl/GLImpl/Sampler/GL_Sampler.cpp` (27 KB), decls `GL_Sampler.h:14-29`
`SamplerParameter{f,i,fv,iv,Iiv,Iuiv}` `:15-20`, `BindSampler` `:29`, `BindSamplers` `:28`,
`GenSamplers` `:25`, `CreateSamplers` `:27`, `DeleteSamplers` `:26`, the six `Get*` queries.
All setters funnel to `SamplerObject::Set*` → `BumpVersion()` (§1.3). `glDeleteSamplers` goes
through `GLContext::MarkSamplerObjectForDeletion` (`Core.cpp:1228-1240`), which **unbinds the
sampler from all 192 units first** (`:1232-1237`) so `TextureUnit::SetSamplerObject`
(`TextureUnit.cpp:28-40`) bumps the bind generation.

### 2.4 Program / pipeline — `MG_Impl/GLImpl/Program/GL_Program.cpp` (188 KB) +
`GL_ProgramPipeline.cpp` (12.7 KB) + `ProgramInterface.cpp` (49 KB)
- `UseProgram_State` `GL_Program.cpp:1451-1481` (public `UseProgram` `:2530`) → refuses while XFB
  is active-and-unpaused `:1457-1464`, refuses an unlinked program `:1473-1479`, then
  `pGLContext->UseProgram(program)` `:1480` → `ProgramState::UseProgram` (`Core.cpp:393-395`).
- `LinkProgram_State` `:1395` (public `:2522`), `DeleteProgram_State` `:606` (public `:2434`).
- **The one uniform funnel: `Uniform_State<ItemCount,T>` `GL_Program.cpp:1483-1561`.** Every
  `glUniform*` and `glProgramUniform*` reaches it once per LOCATION (`:1495-1496`). Non-opaque
  arm: `MarkUniformWrittenAtLocation` `:1499` → `BufferUniformWrite` if SPIR-V is pending
  `:1516-1519` → `MapUBO()`/`GetUBOOffset` `:1520-1532` → **bytes-equal dedupe `:1542`** →
  `Memcpy` `:1543` → `MarkUBOContentDirty()` `:1544`. Opaque arm `:1545-1560`:
  `SetUniformSamplerOrImageUnitIndex(location, unit)` `:1559`, which bumps
  `m_imageUnitVersion` only for image uniforms (`ProgramObject.h:847-849`).
  Wrappers: `Uniformv_State` `:1563-1587` (reads `GetProgramForUniform()` `:1567`),
  `ProgramUniformv_State` `:1590`. Public entry points from `:2534` (`Uniform1f`), `:2553`
  (`Uniform1i`), `:2999` (`ProgramUniform1i`), etc.
- `glUniformBlockBinding` → `ProgramObject::SetUniformBlockBinding` `ProgramObject.h:1020-1027`
  (bumps `m_backendStateVersion` **and** `m_blockBindingVersion`).
- `glShaderStorageBlockBinding` is a **verb** (`FillPoints.def:124`, class `kProgramOp` — the
  class exists solely because it syncs Espryt's render state and textures, `:150-151`, `:323-334`).
- Pipelines: `GL_ProgramPipeline.cpp`; frontend state `Core.h:200-215`
  (`GenProgramPipelineNames` `:204`, `CreateProgramPipelineObject` `:205`,
  `BindProgramPipelineObject` `:208` → `Core.cpp:1391`, `MaterializeProgramPipelineObject` `:211`,
  `MarkProgramPipelineForDeletion` `:212`, `GetBoundProgramPipeline` `:215` → `Core.cpp:1429`,
  `m_boundProgramPipeline` `Core.h:660`).

### 2.5 Pixel store — `glPixelStore{i,f}`
`GLContext::SetPixelStoreParam` `Core.cpp:1134-1136` → `RenderState::SetPixelStoreParam`
`RenderState/RenderState.cpp:828-856` (a 16-arm macro switch, Pack ×8 + Unpack ×8).
`GetPixelStoreParameters(Bool isUnpack)` `RenderState.cpp:885-887`. Storage:
`m_pixelStorePackParameters` / `m_pixelStoreUnpackParameters` `RenderState.h:180-181` — **deliberately
outside `RenderStateParameters m_parameters` `:177`, so they are NOT part of the CSO byte range,
and the setter bumps NO version** (`SetPixelStoreParam` never calls `BumpVersions()` `:159-162`).

---

## 3. Which mutation sites already carry a `MGP_NOTE_MUTATION` / `MGP_NOTE_AGGREGATE`

Macros: `MG_Pipe/PipeMutation.h:155-158` (push) / `:160-161` (pull no-ops).
`MGPipeAggregate` enum `:56-74`. Dispatch: `MG_Impl/Pipe/PipeFill.cpp:529-552`
(`MGPipeNoteAggregate`) and `:509` (`MGPipeNoteFrontendMutation`).

**Complete census of the P4a-relevant sites** (grep over `MG_State/` + `MG_Impl/`):

`MGP_NOTE_AGGREGATE(FramebufferAttachment)` — 3 sites, all in
`FramebufferState/FramebufferObject.cpp`: `:209` (`SetReadBuffer`), `:221` (the five default-geometry
setters via macro), `:234` (`BumpAttachmentVersion`, i.e. every attach/detach/draw-buffer write).

`MGP_NOTE_AGGREGATE(TextureParams)` — 13 sites: `TextureObject.cpp:118, 144, 159, 174, 224, 232,
250, 262, 283, 321, 331`; `TextureObject.h:192` (`SetDepthStencilTextureMode`);
`SamplerState/SamplerObject.cpp:53` (`BumpVersion`, the choke point for **all** sampler params).

`MGP_NOTE_AGGREGATE(TextureContent)` — 5 sites: `TextureObject.cpp:311` (`BumpContentVersion`),
`:383` (`MarkStorageDirty`), `:395` (`MarkStorageDirtyRegion`);
`TextureState/TextureObject2DCube.cpp:53, 65` (the cube overrides).

`MGP_NOTE_MUTATION(...)` — **only four sites in the whole tree, all in
`TextureState/TextureState.h`**: `:85` `GetMaxTouchedTextureUnit`, `:98` and `:111`
`GetTextureBindGeneration`, `:133` `GetSamplingResolutionGeneration`. (P3a widened
`gen_pipe_dirty_surface.py`'s scan root to `MG_State/GLState` precisely to bring these inside the
gate — `MG_Pipe/DirtySurface.def:150-153`.)

**Nothing at all fires for**: any `RenderbufferObject` mutation, any `ProgramObject` version bump,
`glUseProgram`, `glBindProgramPipeline`, `glBindImageTexture`'s `ImageTextureBinding::Bind`
(only `NoteUnitTouched` runs), and `SetPixelStoreParam`.

---

## 4. The dirty bits P4a inherits — already computed, emitted by nobody

`MG_Impl/Pipe/Tracker.h`. Enum `MGPipeDirty` `:57-84`; names `:106-125`;
`MGPipeSubsystemForDirty` `:130-154` (**returns 0 for every bit past 10** `:149-153`);
`MGPipeMixShutter` `:170-173`; `MGPipeWidenedCounter` `:186-204`.

Shutters computed in `MGPipeTracker::Update` `:215-361`:

| bit | name | shutter, verbatim | line |
|---|---|---|---|
| 6 | `NewShader` | `Mix(program->GetLifetimeId(), program->GetLinkVersion())` | `:252`, stored `:261` |
| 7 | `NewShaderBindings` | `Mix(Mix(Mix(ImageUnitVersion, BackendStateVersion), BlockBindingVersion), UniformWriteSetVersion)` | `:253-257`, `:262` |
| 8 | `NewGlobalConstants` | `Mix(LifetimeId, UBOContentVersion)` | `:258`, `:263` |
| 11 | `NewFramebuffer` | `Mix(ctx.GetAnyFramebufferAttachmentGeneration(), m_framebufferBind.Observe(ctx.GetFramebufferBindingSlot(Draw).GetVersion()))` | `:303-306` |
| 12 | `NewSamplerViews` | `Mix(textureContent, ctx.GetTextureBindGeneration())` | `:307-308` |
| 13 | `NewSamplers` | `Mix(textureParams, ctx.GetSamplingResolutionGeneration())` | `:309-310` |
| 14 | `NewShaderImages` | `Mix(Mix(textureContent, textureParams), program->GetImageUnitVersion())` | `:311-312` |

`textureContent` / `textureParams` are read at `:266-267`. The program is read from
`ctx.GetCurrentProgram()` `:246` **and deliberately not `GetProgramForDraw()`** — "the tracker must
not force a compile just to answer did the shader move" `:242-245`. **P4a must respect that**: the
emitter, which needs the artifacts, is the thing that joins.

Also: `m_primed` / `FreshlyPrimed()` `:349-350`, `:417-420`; `Reset()` `:378-392` (called from
`Update` whenever `m_context` moves `:219-222`); the fire tallies `:353-359`.
`kMGPipeDirtyEmittedAtP2` `:96-99` and `kMGPipeDirtyEmittedAtP3a` `:102-104` — **P4a adds a
`kMGPipeDirtyEmittedAtP4a` constant rather than editing either** (the rule is stated at `:93-95`).

Subsystem bits: `MG_Pipe/MGPipe.h:72-95`; `kMGPipeSubsystemResources = 1<<7` `:84`,
`kMGPipeSubsystemVertexInput = 1<<8` `:85`, `kMGPipeSubsystemsMigratedAtP3a = 0x1ff` `:95`.
Bit 63 is the CSO content-addressing negative control `:90`. **Bits 9+ are free for P4a.**

---

## 5. The dirty-surface rows P4a's kinds already carry, and the two holes

`MG_Pipe/DirtySurface.def`. Legend `:101-143` (`kImmediate`, `kReverseChannel`, `kNoBackendRead`,
`kExplicitDestroy`, `kUnpublishedDestroy`, `kPulledEveryVerb`, `kPulledPartialShutter|BIT`).
Gate: `scripts/gen_pipe_dirty_surface.py --check` / `--self-test`, scan root now **both**
`MG_Impl/GLImpl` and `MG_State/GLState`, and a `MGP_NOTE_MUTATION` site counts as a publish
mechanism whose "mutator" is the enclosing function (`:16-28`, `:145-155`).

Rows P4a touches:
- `X(BumpTextureBindGeneration, NEW_SAMPLER_VIEWS)` `:248`
- `X(BumpSamplingResolutionGeneration, NEW_SAMPLERS)` `:256` (added at P3a by the widened root)
- `X(NoteUnitTouched, kPulledPartialShutter|NEW_SAMPLER_VIEWS)` `:271` — the bind generation moves
  only on the `bindingChanged` arm; `GetMaxTouchedTextureUnit` has **no shutter at all** `:265-266`
- `X(SetPixelStoreParam, kPulledPartialShutter|NEW_PIXEL_PACK)` `:237` with the reasoning at
  `:224-236`: the setter writes BOTH halves, only the pack half has a bit, so
  `glPixelStorei(GL_UNPACK_ALIGNMENT, 8)` **moves nothing at all**
- `X(SetActiveTextureUnit, kImmediate)` `:164`
- Deaths `:299-307`: `MarkFramebufferObjectForDeletion`, `MarkRenderbufferObjectForDeletion`,
  `MarkSamplerObjectForDeletion`, `MarkTextureObjectForDeletion` are all `kExplicitDestroy`;
  **`MarkProgramForDeletion`, `MarkProgramPipelineForDeletion`, `MarkShaderForDeletion` are
  `kUnpublishedDestroy` `:301-302, :305`** — a recorded hole ("programs, program pipelines and
  shaders have no per-object handle on the wire at all in P2", `:113-124`). **P4a closes the
  program row and must decide explicitly what to do with pipeline and shader** (a pipeline has no
  wire object; a composite does — see §9.4).
- `MGP_DIRTY_SURFACE_UNDECIDED_LIST` `:332` is **empty**; every bit answer above is derived and
  checked in both directions.

### 5.1 Hole A — `glUseProgram` has no row and no publisher
`GLContext::UseProgram` (`Core.cpp:393-395`) writes `ProgramState::m_currentProgram`
(`ProgramState.h:100`). The scanner sees `pGLContext->UseProgram(...)` from
`GL_Program.cpp:1467`/`:1480`, so a row is owed. Bit 6's shutter mixes lifetimeId ⊕ linkVersion
(`Tracker.h:252`), so a switch between two programs DOES move it — the row's honest answer is
`NEW_SHADER`, but **it is not there today**, meaning the `--check` gate will fail the moment the
scan is re-run against a `UseProgram` call site it now recognises. Verify before writing the brief.

### 5.2 Hole B — a renderbuffer respecify publishes nothing
`RenderbufferObject::{SetInternalFormat, AllocateStorage, SetSamples}`
(`RenderbufferObject.cpp:96-109`) bump no version and raise no notice, and
`AllocateRenderbufferStorage_State` (`GL_Framebuffer.cpp:844-858`) adds nothing. Bit 11's shutter
(`Tracker.h:303-306`) is `anyAttachmentGeneration ⊕ drawFboBindVersion` — neither moves when an
**already-attached** renderbuffer is re-storaged. So
`glBindRenderbuffer; glRenderbufferStorage(newsize)` on a renderbuffer that is already an FBO
attachment is invisible to the framebuffer bit. P4a needs either (a) a version + aggregate on
`RenderbufferObject` (matching `TextureObjectBase::BumpShapeVersion`, which does exactly this and
routes through `BumpSamplingResolutionGeneration`), or (b) `resource_respecify` emitted straight
from the storage entry points the way `BufferObject::NotifyRespecify` does
(`BufferObject.cpp:67-79`) — option (b) matches ARCHITECTURE's rule that renderbuffers share
`MGPResourceDesc` (`docs/Disaggregated/ARCHITECTURE.md` §4.1 table).

---

## 6. The pixel-store shadows — two of them, on opposite sides

### 6.1 Frontend: unpack is fully resolved client-side, pack is not
Every texture upload calls
`MG_Util::PixelStoreProcessor::ProcessTexturePixelsDataUnpack(pixels,
pGLContext->GetPixelStoreParameters(true), …)` before touching the level shadow — call sites
`GL_Texture.cpp:905, 1780-1782, 1902, 1991-1992, 2502-2503, 2655-2656, 2749-2750, 6195-6196`
(decl `MG_Util/Texture/PixelStoreProcessor.h:16`, def `PixelStoreProcessor.cpp:858`). The result
is a **tightly packed** buffer copied row by row into the level shadow
(`GL_Texture.cpp:1803-1817`). This is why `docs/Disaggregated/ARCHITECTURE.md:` §3.2's "explicitly
not migrated" list contains `set_pixel_unpack_state` ("前端已在 `glTexImage` 时解析压缩格式、强制默
认 unpack") and why `MGPPixelPackState` is PACK-only (`MGPipeTypes.h:531-542`).
**Consequence for P4a: `MGPSubData`'s bytes are already tight; `MGPSubRegion::SrcRowStride /
SrcSliceStride` (`MGPipeTypes.h:611-612`) describe the LEVEL SHADOW's pitch, not the
application's.**
Pack, by contrast, is read live at readback: `GL_Texture.cpp:5356`, `:5411`,
`DirectGLES.cpp:6593`, `:8043`, `:9530`, `:9909`.

### 6.2 Espryt: two process-wide driver shadows, asymmetrically invalidated — the pollution risk
- **UNPACK**: `ScopedDefaultUnpackState`, `MG_Backend/DirectGLES/Managers.cpp:4851-4918`. Shadow
  statics `s_synced, s_alignment, s_rowLength, s_skipRows, s_skipPixels, s_imageHeight,
  s_skipImages` at `:4911-4917` — **`static inline` class members, i.e. one set per PROCESS, not
  per context**. `EnsureShadowSynced()` `:4874-4891` pins the driver to GL defaults exactly once
  and sets `s_synced = true` `:4878`. **`s_synced` is written at `:4878` and read at `:4875` and
  nowhere else in the file — there is no invalidation path at all.** The class comment `:4842-4850`
  states the invariant it depends on: "The backend unpack state is set ONLY by MobileGL's own
  save/restore helpers … so the shadow stays accurate". Any glPixelStorei on an `UNPACK_*` from
  outside those helpers, or a driver context whose unpack state is not the pinned default,
  desynchronises it permanently. The known outside writer is the ring path's
  `glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x())` at `Managers.cpp:6400` (guarded by
  `!ringStaged`, comment `:6297`).
- **PACK**: `PixelStoreImpl`, declared `MG_Backend/DirectGLES/Managers.h:1783-1806`
  (`struct PackState{Alignment,RowLength,SkipRows,SkipPixels}` `:1793-1802`), defined
  `Managers.cpp:8004-8054` — file-scope `g_packState` / `g_packStateKnown` `:8006-8007`,
  `PinPackState` `:8009-8016`, `ApplyPackState` `:8019-8040`, `CurrentPackState` `:8042-8049`,
  `InvalidatePackStateCache` `:8051-8053`. **This one IS invalidated**, at MakeCurrent
  (`DirectGLES.cpp:10578`) and at context destroy (`:11112`). `ScopedPackState` RAII
  `DirectGLES.cpp:6577-6588`, `PackStateFromContext` `:6592-6593`; users at `:7092, 8477, 8588,
  8683, 9103, 9432`.
- Header note `Managers.h:1789-1791`: `PACK_IMAGE_HEIGHT / SKIP_IMAGES / SWAP_BYTES / LSB_FIRST`
  have **no ES equivalent** and are honoured on the CPU from frontend context state — i.e. the
  boundary must keep carrying the full `PixelStoreParameters` (28 B, `MGPipeTypes.h:538-542`),
  not the four ES-expressible fields.

**P4a's stake**: once texture upload is emitted by handle, Espryt's upload path stops being the
thing that reads the frontend's unpack state (ARCHITECTURE.md:252 says it "改为从描述符取步长,
`UNPACK_ROW_LENGTH` 从 `SrcRowStride/bpp` 设"), so the P4a emitter has to carry the strides that
`ScopedDefaultUnpackState`'s pin currently makes unnecessary — and the asymmetric invalidation
above becomes a correctness dependency rather than a performance one.

---

## 7. `ReadSurface` resolution and framebuffer completeness

`MGPFramebufferState` (`MG_Pipe/MGPipeTypes.h:355-372`, 304 B) demands three things the client
must resolve **before** the record goes out:
- `MGPSurface ReadSurface` `:361` — "The RESOLVED read surface, not an index. This is what
  structurally closes the read-buffer-shared-FBO defect class."
- `Uint8 Complete` `:364`.
- `Uint64 ContentHash` `:370` — both the server's render-pass memo key and the client's emission
  suppressor.
`MGPSurface` itself `:343-353` (24 B): `{Res, InternalFormat, Kind(Texture|Renderbuffer|None),
Layered, Level, Layer, UploadTarget}` — **`InternalFormat` is inline so four cross-object masks
fall out with zero lookups** (`:341-342`).

**Where the client gets each piece:**
- Bound FBO: `ctx.GetFramebufferBindingSlot(FramebufferTarget::Draw|Read)`
  (`Core.h:547`, `FramebufferState.cpp:49-57`); default-FBO identity is
  `FramebufferImpl::pDefaultFramebufferInfo->defaultFBO` (`GL_Framebuffer.h:89, 95`).
- Attachments: `fbo->GetAllAttachmentObjects()` (`FramebufferObject.h:131`,
  `FramebufferObject.cpp:159-161`) or per point `GetAttachment(type)` `:155-157`;
  then `IsTexture()`/`IsRenderbuffer()`/`GetTexture()`/`GetRenderbuffer()`/`GetTextureLevel()`/
  `GetTextureLayer()`/`IsLayered()`/`GetTextureUploadTarget()`
  (`FramebufferObject.cpp:53-87`), format from `ITextureObject::GetFormat()` or
  `RenderbufferObject::GetInternalFormat()`.
- **The read surface**: `fbo->GetReadBuffer()` (`FramebufferObject.h:137`) then
  `fbo->GetAttachment(readBuffer)` — this is the exact two-step every consumer already does:
  frontend `GL_Framebuffer.cpp:2373, 2921, 2992`, `Framebuffer/Validators.cpp:185`,
  `Getter/GL_Getter.cpp:343, 2114`, `Texture/GL_Texture.cpp:4445`; Espryt
  `Managers.cpp:7286, 7391, 7398, 7415`; Magma `VkRenderPassManager.cpp:618`,
  `VulkanRenderer.cpp:1806, 1823, 8755, 9243, 10261-10264`.
  **The defect class it closes** is visible in Espryt's
  `BackendFramebufferObject::SyncReadBufferToBackend` (`Managers.cpp:7410-7430`), whose comment at
  `:7424-7426` says it must `Bind(FramebufferTarget::Read)` first "When this is reached from
  `SyncCurrentFBO`'s 'same FBO as draw' skip path" — the shared-FBO case where the read-buffer
  sync was skipped. With the surface pre-resolved on the client, that whole hazard is
  unrepresentable.
- **`Complete`**: `FramebufferObject::CheckCompleteness()` `FramebufferObject.cpp:163-193` — walks
  the 41 attachment slots, skips `!IsValid()`, requires equal `GetSize()` and per-attachment
  `IsComplete()`, and requires at least one valid attachment. The *GL-visible* status is more
  than that: `CheckFramebufferStatus_State` `GL_Framebuffer.cpp:2265-2296` adds
  `HasDefinedAttachment` (to pick `INCOMPLETE_ATTACHMENT` vs `MISSING_ATTACHMENT` `:2284-2286`),
  `HasNonRenderableColorAttachment` → `UNSUPPORTED` `:2288-2290`, and
  `ActiveBackendRejectsDistinctDepthStencil() && HasUnsupportedDistinctDepthStencilAttachments`
  `:2291-2294` (**this one reads the backend**, so it cannot be part of a purely-client answer —
  see the risk note in §14). `CheckNamedFramebufferStatus_State` `:2298-...` is the same body.
  The TODO block at `:2279-2282` records that DRAW_BUFFER / READ_BUFFER / MULTISAMPLE /
  LAYER_TARGETS incompleteness is not implemented.
- `Width/Height/Layers/Samples/FixedSampleLocations` for a no-attachment FBO come from
  `GetDefaultWidth/Height/Layers/Samples/FixedSampleLocations` (`FramebufferObject.h:141-145`).
- `DrawBuffers[8]` from `GetDrawBuffers()` `:135` (an array of `FramebufferAttachmentType`, so the
  client converts to the `Int8` attachment index / `-1` the payload wants, `MGPipeTypes.h:362`).

---

## 8. Texture sub-data accumulation — the union box and the region list already exist

`MG_State/GLState/TextureState/MipmapStorage.h` is where a P4a `MGPSubData` + `MGPSubRegion[]`
is built from; the frontend structure and the payload were designed against each other.

- `struct MipmapDirtyRegion` `:26-39` — `{IntVec3 lo, hi}` half-open, `Empty()` `:29`,
  `CoversWholeLevel(levelSize)` `:30-33`, `TexelCount()` `:34-38`. Cleared (all zero) while clean
  `:20-25`.
- Union box per level: `m_dirtyRegions` `:119`; `MarkDirtyRegion(level, offset, size)` `:58`
  unions and sets the flag; `GetDirtyRegion(level)` `:60` reads it, meaningful only while
  `IsDirty(level)` `:54`.
- **Behind it, the disjoint rect list**: `kMaxDirtyRects = 96` `:75`, `m_dirtyRects` `:127`
  (per-level `Vector<MipmapDirtyRegion>`), `GetDirtyRects(level, out, maxRects)` `:82`,
  `InsertDirtyRect` `:114`. The design notes `:62-81` are load-bearing for the wire format:
  the union box "stays the source of truth (every write funnels through
  `MarkDirty`/`MarkDirtyRegion` into BOTH representations), backends OPT IN to the list purely as
  an upload-size refinement"; **`GetDirtyRects` returning 0 means "upload the union box instead"
  and covers every reason at once** — tracking unavailable, a single rect, more rects than
  `maxRects`, or a summed area close enough to the box's that one big upload wins `:76-81`.
  96 slots because a 16-slot list forced far-apart merges to >90% of the box's area on
  Minecraft's ~100 sprites/frame `:68-74`. An EMPTY list is the resting state and always means
  "the union box is the whole story" `:120-126`.
- Other level state the descriptor and the blob need: `m_texelSizes` `:116`, `m_data` `:117`
  (the level shadow), `m_isDirty` `:118`, `m_compressedData`/`m_compressedFormats` `:128-129`
  (kept **beside**, not instead of, the uncompressed shadow — rationale `:84-92`),
  `m_requestedCompressedFormats` `:130` (`:99-109`).
- The `ITextureObject` face of all of this: `MarkStorageDirty` `TextureObject.h:252`,
  `MarkStorageDirtyRegion` `:257-262` (**base fallback degrades to whole-level**),
  `GetStorageDirtyRegion` `:264-267` (**base fallback returns the whole level**),
  `GetStorageDirtyRects` `:274-281` (**base fallback returns 0**), `IsStorageDirty` `:253`,
  `MapMipmapData` `:251`, `GetMipmapTexelSize` `:243`, `GetMipmapByteSize` `:244`,
  `GetMipmapLevelCount` `:242`. Concrete impls
  `TextureObject.cpp:379-410` (`TextureObjectWithOneMipmap`), cube overrides
  `TextureObject2DCube.cpp:53, 65`.
- The payload side: `MGPSubRegion` `MGPipeTypes.h:607-614` (`X,Y,Z,W,H,D,SrcOffset,SrcRowStride,
  SrcSliceStride`; **strides CARRIED, not inferred from a pointer comparison** `:603-606`),
  `MGPSubData` `:636-648` (`Res, Target, Level, SourceIsVerbatimLevelShadow, UnionBox,
  RegionCount, Blob`). "Carries the union box AND the region list so the SERVER picks the upload
  shape — the decision belongs on the side that pays the GPU cost. Mali prices texture upload by
  JOB COUNT: ~100 sprite rects against one union box measured +6 ms/frame" `:616-618`.
  The blob rule `:624-635`: a non-zero `Blob.Size` that is not exactly the record's own byte size
  is `Fatal{ProtocolCorruption}`; **zero means "not declared"**, which is what a monolith emission
  is. `MGPSubDataComplete` `:686-691` is the forward terminator for a server-initiated pull.
- Espryt's existing consumer, whose shape `MGPSubRegion` copies: `UnpackStagingBlock`
  `Managers.cpp:4934-4942` (`src, rowBytes, rows, slices, srcRowStride, srcSliceStride, offset`),
  with the ring-staging contract at `:4920-4933` and `:6297`.

---

## 9. Program artifacts, reflection, per-stage SPIR-V, the link version

`MG_State/GLState/ProgramState/ProgramArtifacts.h` (35 KB, 576 lines) is the P0.5 extraction that
makes `MGPProgramDesc`'s reflection blob possible — it includes only `<Includes.h>` (`:10`), never
`ShaderObject.h` or `SpvcSession.h`.

- `kInvalidUniformOffset = ~0u` `:26`.
- `struct TypeFacts` `:31-59` — 20 flattened predicates (13 `Bool` + 7 4-byte scalars),
  **POD of exactly 44 bytes**, asserted `:551-552`.
- `struct ResourceReflection` `:63-86` — one flattened `glslang::TObjectReflection`
  (`name, glDefineType, offset, size, index, counterIndex, arrayStride, topLevelArraySize,
  topLevelArrayStride, binding, location, stages, arraySize, TypeFacts type`); aliased as
  `UniformReflection`/`BlockReflection`/`PipeInputReflection`/`PipeOutputReflection` `:88-91`.
- `struct XfbVarying` `:95-120` — `{name, type, size, bufferIndex, offsetBytes, byteSize,
  packedOffsetBytes, blockInstanceName, blockName, blockMemberIndex, blockMemberElement}`.
- `struct LinkArtifacts` `:160-343` — the whole link output in one movable block; `program`
  (`SharedPtr<glslang::TProgram>`) is **live only between LinkProgram() and the end of
  DoReflection** and is null for an L1-memo-served link `:161-165`; then the four owned
  reflection vectors `:168-171`, `lastStageIsFragment` `:177`, `computeLocalSize` `:178`, …,
  ending at `xfbVaryingNameMaxLength` / `xfbNeedsScatteredCapture` / `xfbPackedStride` `:340-342`.
  Indexed by **TProgram index**, deliberately `:155-158`.
- `struct SpirvArtifacts` `:359-399` — `generatedSpirv` (`Vector<Vector<unsigned>>`, **one module
  per entry of the linked-shader snapshot, in that order**) `:360`, `enableSpirvValidation` `:361`,
  `uniformOffsets` `:364`, `globalUboScratch` `:365`, `reservedNumSamplesOffset` `:370`,
  `spirvStatus` `:377`, `nativeFloat64` `:386`, `pointSizeDemoted` `:398`.
- **The archive**: `VisitFields(Self&, Visitor)` free templates `:401-547`, one per type, the same
  table serving serializer (const) and deserializer (non-const) `:403-406`. This is the
  ready-made writer for `MGPProgramDesc::Reflection`.
- **Trip wires** `:549-575`: `sizeof(TypeFacts)==44`; per-STL pins under `__GLIBCXX__`
  `MGL_RESOURCEREFLECTION_SIZE 128`, `MGL_XFBVARYING_SIZE 128`, `MGL_LINKARTIFACTS_SIZE 1056`,
  `MGL_SPIRVARTIFACTS_SIZE 88` `:558-562`; the libc++ (NDK) branch is **inert until the integrator
  pins it** `:563-565` — that pin is a P4a chore if the archive crosses on device.

`MGPProgramDesc` (`MG_Pipe/MGPipeTypes.h:323-335`, 192 B): `{Cso, StageMask (== the linked stage
set), GlobalUboSize, ReservedNumSamplesOffset, SpirvStatus, NativeFloat64, PointSizeDemoted,
EnableSpirvValidation, MGPBlobRef Spirv[6], MGPBlobRef Reflection}`.

### 9.1 The "as last linked" rule the emitter must obey
`ProgramObject::GetLinkedShaderSnapshot()` `ProgramObject.h:106` returns
`Vector<LinkedShaderRef>` (`:99-103`, `{shader, source, node}`), rebuilt in Link()'s prologue.
`HasLinkedShaderStage(stage)` `:115-120`. **The header states it twice** (`:92-98`, `:121-133`):
`glAttachShader`/`glCompileShader` take effect only at the next link and neither moves
`m_linkVersion`, so **anything keyed on the link generation must consume this snapshot, never the
live attach list** (`GetAttachedShaders()` `:89-90`), and the SPIR-V module list is sized and
indexed by the snapshot, not by the attach list. `StageMask` therefore comes from the snapshot.

### 9.2 The join gate
`Artifacts()`/`Spirv()` join through `EnsureLinkJoined()` (inline `ProgramObject.h:1275`, blocking
half `ProgramObject.cpp:50-83`) and `EnsureSpirvJoined()` (`ProgramObject.cpp:89-122`).
`JoinPendingLink` moves the node out FIRST to avoid re-entry `:55-60`, moves the artifacts
`:66`, and calls `BumpLinkObservableVersions()` `:72` (the *second* bump; the first was at
enqueue `:67-71`). `GLContext::GetProgramForDraw` `Core.cpp:609-628` is join site J1 and calls
`currentProgram->JoinLinkAndSpirv()` `:627` — **both phases, and the comment `:622-626` explains
why phase A alone reopens the hazard**. `GetProgramForDispatch` `Core.cpp:763-...`,
`GetProgramForUniform` `:786-...`.

**P4a's emitter must be the joiner.** ARCHITECTURE.md:198 says `create_shader_state` is emitted
"从编译池的终止 continuation" — i.e. from the compile pool's terminal continuation, not from the
draw — which is the monolith-unreachable async win. That is a design decision for the brief, not
a fact about this tree: today nothing emits it at all.

### 9.3 What the server still specialises (do NOT try to push it)
`docs/Disaggregated/ARCHITECTURE.md:263` lists the eight extra inputs a backend program depends on
(draw-FBO snorm/unorm clamp mask, fragColor broadcast count, storage-block binding signature,
atomic-counter set, live image formats, patch params; Magma adds default-FB height for FragCoord
Y-flip and the XFB layout): `create_shader_state` publishes the **artifacts**, the server
specialises at verb time.

### 9.4 Program pipelines and the composite band
`GLContext::GetProgramForDraw` `Core.cpp:630-...`: when no program is current it takes the bound
pipeline `:631`, **joins every graphics stage program** `:642-645`, computes
`pipeline->ComputeDrawProgramSignature()` `:647` (`ProgramPipelineObject.h:94-98`, an
`Array<Uint64, kGraphicsStageCount*2>` of per-stage lifetimeId+linkVersion), checks
`GetCachedDrawProgram(signature)` `:648-651` (`ProgramPipelineObject.h:149`) and otherwise
**flattens the pipeline into a fresh `MakeShared<ProgramObject>(0u)`** `:661` — deliberately
unnamed (`:658-660`), graphics stages only (`:663-668`: a composite carrying a compute module
SIGSEGVs inside `vkCreateGraphicsPipelines` on Adreno 830).
`RefreshCompositeUniforms` `:649` / `SetMirroredUniformVersions` `:606`
(`ProgramPipelineObject.h:123-148`).
**A composite is an ordinary `ProgramObject` with its own lifetime id**, so it fits the ShaderCso
slot space — and `MGPipeHandles.h:83-97` already reserves the top 1/16 of that space for exactly
these (`kMGPipeShaderCsoSlotLimit = 1<<20` `:88`, `kMGPipeShaderCsoCompositeSlotBase` `:89-90`,
`MGPipeIsCompositeShaderSlot` `:92-94`), and `SlotAllocator.cpp:19-23` refuses the band from the
ordinary allocator. `ProgramPipelineObject` itself has **no lifetime id and no wire object**
(`:19-170`; the only identity is `m_everBound` `:170` / `GetEverBound` `:51`).

`ShaderObject` (`ProgramState/ShaderObject.h:24`, dtor `:52`) has **no lifetime id** — consistent
with `kUnpublishedDestroy` and with the design (shaders never cross).

---

## 10. `PipeInputs` — what already exists for these kinds, and what fills it

`MG_Backend/MGPipe/PipeInputs.h` (736 lines). Storage list
`MGP_INPUT_STORAGE_LIST` `:73-129`; the block `struct PipeInputs` `:158-687`; three storage
classes V/O/F documented `:144-153`; `sizeof(PipeInputs) < 20 KB` asserted `:714`; the
field-count identity `:709-712`.

**P4a-relevant accessors and their storage:**

| accessor | decl | storage member | class |
|---|---|---|---|
| `GetFramebufferBindingSlot(FramebufferTarget)` | `:508-517` | `m_framebufferBindingSlot[kFramebufferTargetCount]` `:681` | O (raw ptr into the live context) |
| `GetImageTextureBinding(Int unit)` ×2 | `:518-533` | `m_imageTextureBindingBase` `:682` | O |
| `GetTextureUnitObject(Int unit)` | `:549-...` | `m_textureUnitBase` `:686` | O |
| `GetProgramForDraw()` | `:539-543` | `m_programForDraw` `:684` | O (`SharedPtr`) |
| `GetProgramForDispatch()` | `:534-538` | `m_programForDispatch` `:683` | O |
| `GetTransformFeedbackProgram()` | `:544-548` | `m_transformFeedbackProgram` `:685` | O |
| `GetActiveTextureUnit()` | `:199-203` | `m_activeTextureUnit` `:629` | V |
| `GetMaxTouchedTextureUnit()` | `:313-317` | `m_maxTouchedTextureUnit` `:647` | V |
| `GetTextureBindGeneration()` | `:398-401` | `m_textureBindGeneration` `:662` | V |
| `GetSamplingResolutionGeneration()` | — | `m_samplingResolutionGeneration` `:661` | V |
| `GetTextureContextId()` | `:403-...` | `m_textureContextId` `:663` | V |
| `GetPixelStoreParameters` | — | `m_pixelStore` (`[0]`=pack, `[1]`=unpack) `:103` | V |
| `GetClampReadColor()` | `:244-248` | `m_clampReadColor` `:635` | V |
| **forwarded (F, sticky)** | `:558-577` | none | `GetProgramObject(Uint)` `:570`, `GetTextureObject(Uint)` `:571`, `ValidateProgramName` `:574`, `GetBufferBindingPointCount` `:569`, `HasOpenTransformFeedbackSpan` `:572`, `InvalidateCompileEnv` `:573`, `RecordError` `:577` |

`kMGPipeForwardedFieldCount = 7` `:135`, asserted equal to the sticky count `:136-137`. The F
accessors carry **no** `MGP_INPUT_CHECK` / `MGP_INPUT_VERIFY_READ` and the reason is stated
`:563-568`.
The two friend doors: `MGPipeFillAccess` `:611` (client filler) and `MGPipeApplyAccess` `:618`
(the applier scattering render-state chunks straight into `m_renderState`).
`gPipeInputs` `:706` — **`inline PipeInputs& gPipeInputs = *new PipeInputs();`**, leak-at-exit,
with the full ID-18 rationale at `:692-705` (its O-class members are `SharedPtr`s to FRONTEND
objects; destroying the block at exit runs `~VertexArrayObject` → `~BufferObject` →
`MGPipeApplyResourceDestroy` → the backend's twin tables). **Any P4a `SharedPtr<ProgramObject>` /
`SharedPtr<ITextureObject>` added here inherits that rule verbatim.**

### 10.1 `FillPoints.def` rows that fill them
`MG_Pipe/FillPoints.def`. Verb list `:79-148` (69 verbs, **must be exactly the member set of
`MG_Backend::GLFunctionsTable` in declaration order** `:17-22`, enforced by `gen_pipe.py`);
class list `:152-153` (nine classes); the may-read table `MGP_FILL_FIELD_LIST` `:158-338`.

P4a's fields, by class:
- `GetFramebufferBindingSlot`: `kDraw` `:200`, `kDispatch` `:219`, `kClear` `:232`,
  `kBlitOrCopy` `:247`, `kTextureOp` `:291`, `kReadback` `:296`, `kProgramOp` `:328`.
- `GetTextureUnitObject`: `kDraw` `:165`, `kDispatch` `:213`, `kClear` `:240`, `kBlitOrCopy` `:256`,
  `kTextureOp` `:278`, `kReadback` `:298`, `kProgramOp` `:329`.
- `GetImageTextureBinding`: `:170, 218, 245, 261, 279, 308, 334`.
- `GetTextureContextId` / `GetTextureBindGeneration` / `GetSamplingResolutionGeneration` /
  `GetMaxTouchedTextureUnit`: the same seven classes (`:166-169, 214-217, 241-244, 257-260,
  280-283, 304-307, 330-333`).
- `GetActiveTextureUnit`: `kBlitOrCopy` `:255`, `kTextureOp` `:277`, `kReadback` `:297`.
- `GetProgramForDraw`: `kDraw` `:161`. `GetProgramForDispatch`: `kDispatch` `:209`.
- `GetPixelStoreParameters`: `kReadback` ONLY `:294`.
- `GetClampReadColor`: `kReadback` `:299`.
- The eight statically over-approximated rows are **all KEPT** with reasons `:30-70`; three of
  them are P4a-adjacent (`kTextureOp/kDispatch + IsCapabilityEnabled` because Magma's
  `GenerateMipmap` and `PrepareStorageImageTextures` materialise a queued clear that reads
  `GL_FRAMEBUFFER_SRGB` `:43-49`; `kTextureOp + GetBufferBindingPoint` / `GetFramebufferBindingSlot`
  for the depth-mipmap shader path `:284-292`). **`P3a STATUS` `:65-70` says the omission sweep
  rides the full gl44to46 caselist run and every row stays until it lands** — so P4a must not
  retire one on desktop evidence either.

### 10.2 `Coverage.def` rows P4a's calls will claim
`MG_Pipe/Coverage.def`. Accessor→call map `:29-132`; sticky list `:144-151`; delta list `:156-158`;
**emitted list `:198-232`** (what a call now SUPPLIES, so the residual fill loop may skip it).
Existing accessor rows for P4a's kinds: `GetFramebufferBindingSlot → SetFramebufferState` `:84`,
`GetImageTextureBinding → SetShaderImages` `:85`, `GetActiveTextureUnit → SetSamplerViews` `:30`,
`GetMaxTouchedTextureUnit → SetSamplerViews` `:88`, `GetSamplingResolutionGeneration →
SetSamplerViews` `:109`, `GetTextureBindGeneration → SetSamplerViews` `:112`,
`GetTextureContextId → SetSamplerViews` `:113`, `GetTextureObject → SetSamplerViews` `:114`,
`GetTextureUnitObject → SetSamplerViews` `:115`, `GetProgramForDraw → SetDrawProgram` `:100`,
`GetProgramForDispatch → SetDispatchProgram` `:99`, `GetProgramObject → CreateShaderState` `:101`,
`GetPixelStoreParameters → SetPixelPackState` `:94`.
**None of them is in the EMITTED list yet.** The two standing warnings for P4a:
- `GetPixelStoreParameters` is **deliberately absent from EMITTED** `:173-181` — the field is
  `m_pixelStore[2]` and `set_pixel_pack_state` carries the pack half only, so a row there would be
  a half-truth that neither the poison nor the verify comparator could see. **P4a must not add it.**
- P3a's own row (`GetBoundVertexArray → BindVertexElements` `:202`) is "shape-only" and the note
  `:191-197` says the decision of whether an emitted call supplies the WHOLE field lives in
  `PipeFill.cpp`'s `EmittedCallSuppliesTheWholeField` (`:1116`), **deliberately rather than
  silently by the row's presence**. Same discipline applies to every P4a row.

---

## 11. The payloads P4a fills (all already declared, `MG_Pipe/MGPipeTypes.h`)

`MGPResourceDesc` `:150-176` (88 B) — one create/respecify shape for buffers, **every texture
target** and renderbuffers: `Target` `:152`, `StorageKind` (== `TextureStorageType`) `:153`,
`BindMask` `:157` (the `ELEMENT_ARRAY` bit is the index-mirror switch `:154-156`),
`InternalFormat` "already resolved to an uncompressed fallback by the client" `:158`,
`Width/Height/Depth` `:159`, `ArrayLayers/Levels/Samples` `:160`,
`FixedSampleLocations/Immutable` `:161`, `Usage`/`StorageFlags` `:162-163`,
`HasDefinedContent` `:164`, `ImageBindableHint` `:165`, `GlNameForDiag` (**diagnostics only** —
a GL name is never an identity `:167-170`), `ViewOf` `:172`, `BufferForTexBuffer` `:173`,
`BufOffset/BufSize` with `kMGPipeWholeBuffer = ~0` `:174, 177`.
`MGPipeResourceRespecifyNeedsAck(desc)` `:680-682` — `desc.Immutable != 0` (rationale `:668-679`).

Other P4a payloads: `MGPSamplerDesc` `:283-287` (32 B); `MGPSamplerView` `:293-304` (36 B, view
restrictions only — rationale `:289-292`: an attachment-only / image-only / CopyImageSubData-only
texture has no sampler view to hang params on); `MGPTextureParams` `:307-318` (32 B, per texture
OBJECT, with `ForceResync` `:314` mirroring Espryt's `m_forceTextureParamsResync`);
`MGPSurface` `:343-353`; `MGPFramebufferState` `:355-372`; `MGPSamplerViews` `:440-444`,
`MGPSamplerStates` `:447-451`, `MGPImageView` `:453-462`, `MGPShaderImages` `:464-468` (all
`{Start, Count, ContentHash}` var-tail headers); `MGPProgramDesc` `:323-335`;
`MGPGlobalConstants` `:507-513`; `MGPSubData`/`MGPSubRegion` `:607-648`; `MGPCopyRegion`
`:708-716`; `MGPMipPlan` `:741-745`; `MGPReadbackInfo` `:748-756`; `MGPClear` `:729-739`;
`MGPBlit` `:718-725`.

Catalogue rows (`MG_Pipe/PipeCalls.def`, `MGP_CALL_LIST_DOCUMENTED_COUNT 71` `:75`):
`CreateSamplerState/DeleteSamplerState` `:104-105`, `CreateSamplerView/DeleteSamplerView`
`:106-107`, `CreateShaderState/BindShaderState/DeleteShaderState` `:108-110`,
`SetFramebufferState` `:113`, `SetSamplerViews` `:117`, `BindSamplerStates` `:118`,
`SetShaderImages` `:119`, `SetGlobalConstants` `:122`, `SetDrawProgram`/`SetDispatchProgram`
`:126-127`, `SetTextureParams` `:133`, `ResourceSubData` `:134`, `ResourceSubDataComplete` `:136`,
`ResourceCopyRegion` `:139`, `GenerateMipmap` `:140`, `GetTextureImage` `:141`, `Blit` `:143`,
`Clear` `:144`, `ReadPixels` `:145`. **Record numbering never churns — the wire opcode is the
1-based position, so a new call is APPENDED to its group** `:26-29`.

---

## 12. The client emission machinery P4a mirrors

- **Slot allocator** `MG_Impl/Pipe/SlotAllocator.{h,cpp}` — `Allocate` `cpp:38-81`,
  `AllocateFor` `:83-95`, `FindByLifetimeId` `:97-105`, `Acquire` `:107-111`, `Free` `:113-130`
  (**stale-gen free is a no-op** `:117-119`), `IsLive` `:132-137`, `HighWater` `:151-153`,
  `Reset` `:161-168`. **Gen moves only on reuse** `cpp:66-75`, `h:24-28`. `MGPipeSlots()`
  `cpp:170-181` is **never destroyed** with the ID-18 rationale spelled out at `:171-179`.
- **CSO cache** `MG_Impl/Pipe/CsoCache.h` — the exact template for a P4a sampler-CSO or
  program-CSO cache: version-first skip `:14-25`, hash + **memcmp confirm** `:84-98`
  (a bare 64-bit equality would alias two states — `:20-24`), `Mint` `:138-172`, LRU `Evict`
  `:174-183` (emits `delete_*` and frees the slot), capacity 64 `:53`, counters `:57-69`,
  `s_hashForTest` seam `:121-128`, `MGPipeCsoCacheInstance()` never destroyed `:197-203`.
- **Set-hash suppressor** `MG_Impl/Pipe/SetHashSuppressor.h` — `MGPipeSuppressorSlot` `:44-53`
  already reserves `SetSamplerViews` (P3b), `BindSamplerStates` (P3b), `SetShaderImages` (P4b),
  `SetShaderBuffers` (P4b), `SetStreamOutputTargets` (P4b). **There is no
  `SetFramebufferState` slot** — P4a adds one (or latches `MGPFramebufferState::ContentHash`
  itself). `ShouldEmit` `:61-67` (**hash 0 is reserved for "never emitted"; a computed 0 remaps to
  1** `:31-33`), `Invalidate`/`InvalidateAll` `:71-75`. The requirement a wired slot puts on its
  hash is stated `:23-29`: it must cover **every** input the record carries, not just the set.
- **Resource tracker** `MG_Impl/Pipe/ResourceTracker.h` — `MGPipeBindMaskForBufferTarget`
  `:96-134` + the exhaustiveness `static_assert` `:136-148`, `MGPipeBuildResourceDesc` `:171-...`,
  `MGPipeBuildSubDataRecord` `:225-...`, **`MGPipeForEachSubDataRecordRange` `:257-...` (the
  splitter for ranges past a single record's bound)**, `class MGPipeResourceTracker` `:282-...`
  (`Acquire` `:292`, `Find` `:303`, `Resolve` `:313`, `Retire` `:325`, `NotePublished` `:342`,
  `BindMask` `:355`, `NoteBoundAs` `:369`, `RefreshBindMask` `:407`, counters `:448-463`,
  `ResetForTest` `:493`), `MGPipeResourceTrackerInstance()` never destroyed `:554-559`, and the
  reverse-channel callbacks `MGPipeClientOnBufferWriteback` `:570-...`,
  `MGPipeClientOnGpuWritten` `:596-...`, `MGPipeInstallClientResourceCallbacks` `:623-...`.
- **Vertex-input emitter** `MG_Impl/Pipe/VertexInputEmit.h` — the CSO-with-blob template:
  wire builders `:78-104`, `:106-121`, content hash `:123-129`,
  `class MGPipeVertexInputEmitter` `:136-...` (`EmitVertexElements` `:157`, `EmitVertexBuffers`
  `:200`, `EmitIndexBuffer` `:256`, `RecordIsPublished` `:307`, `NoteRecordDestroyed` `:319`,
  `Reset` `:340`, `EmitCreate` `:373`, the blob staging array `:413`), instance never destroyed
  `:433-441`.
- **The validate point** `MG_Impl/Pipe/PipeFill.cpp:1504-1620+`: poison serial `:1518-1524`,
  verb + identity `:1525-1527`, the no-context early exit that still clears the pending base
  instance `:1528-1536`, the dirty walk `:1544-1545`, the `wants()` lambda that goes through
  **`MGPipeSubsystemForDirty` and never names a subsystem constant** `:1548-1557`, the
  `FreshlyPrimed()` block `:1570-1585` (CSO cache reset, `MGPipeApplierReset`, suppressor
  invalidate-all, vertex-input emitter reset — **and the note at `:1574-1582` that the resource
  tracker is deliberately NOT reset**), then the per-subsystem emissions `:1587-1615`.
  P4a's emitters go in this ladder.
  Also here: `SubsystemForEmitter` `:957-...` + its pairing `static_assert`s `:984-1032`,
  `kMGPipeWiredSubsystems` `:1071`, `EmittedCallSuppliesTheWholeField` `:1116`,
  `MGPipeNoteAggregate` `:529-552`, and the seven F-class forwarder bodies `:875-906`.
- **Applier** `MG_Pipe/PipeApply.h` — `MGPipeResourceOps` `:74-86` (+`Set/Get` `:90-91`),
  slot bounds `kMGPipeMaxResourceSlots = 1<<20` / `kMGPipeMaxVertexElementsSlots = 1<<16`
  `:112-113` with the "a slot at or above these is `Fatal{ProtocolCorruption}`, never a resize"
  rule `:97-111`, `MGPipeResourceRecord` `:117-132`, `MGPipeVertexElementsRecord` `:141-152`,
  `MGPipeApplierState` `:154-...` — **and the explicit split at `:194-218`: object records
  (`Resources` `:217`, `VertexElementsCsos` `:218`) survive a make-current and are cleared only by
  the object's own death signal or `MGPipeApplierReleaseObjectRecords` `:300`; working state
  (`BoundVertexElements` `:236`, `VertexBuffers` `:240`, …) is cleared by `MGPipeApplierReset`
  `:291`**. Refusal counters `:228-229` exist because "a no-op nobody can see is a dropped call
  nobody can see" `:220-227`.

---

## 13. Unit tests under `MG_Test` that cover these frontend objects

All registered with `LABELS unit` via `gtest_discover_tests` (e.g.
`MG_Test/Framebuffer/CMakeLists.txt:20`, `MG_Test/Texture/CMakeLists.txt:20, 38`).

| file | size | suites | what it pins for P4a |
|---|---|---|---|
| `MG_Test/Framebuffer/FramebufferTest.cpp` | 1669 L, **61 `TEST`** | `FramebufferTest`, `PackedReadbackEncodeTest` | attach/detach, draw/read buffer validation, completeness |
| `MG_Test/Texture/TextureTest.cpp` | 6714 L, **192 `TEST`** | `TextureTest`, `SharedExponentRGB9E5Test` | the whole texture surface; **ID-8 records that P3a's wave1/wave2 both appended at the same point here and collided — resolve by union, keep every name** |
| `MG_Test/Texture/TextureViewTest.cpp` | 551 L | `TextureViewTest` (`:147, 159, 169, 178, 198, 224, 236, 259`, …) | view state, `TEXTURE_VIEW_*`, composition onto the root, "a framebuffer attach is bounded by the view's own level count" `:198` — directly `MGPResourceDesc::ViewOf` / `MGPSamplerView` |
| `MG_Test/Texture/VkClearManagerTest.cpp` | | | the queued-clear materialisation `FillPoints.def:284-289` names |
| `MG_Test/State/ObjectLifetimeIdTest.cpp` | 158 L, 6 `TEST(ObjectLifetimeIdTest,…)` | | **the id invariant P4a's handles rest on**: constructs/destroys on the heap 64× (`:57-58`), publishes through a volatile sink so the new/delete pair cannot be elided (`:42-46`), asserts id ≠ 0 (`:72-73`) and strictly increasing (`:74-76`), and **skips rather than passes quietly if the allocator never repeats an address** (`:21-24`). Covers `BufferObject`, `RenderbufferObject`, `VertexArrayObject` (`:34-36`) — **not** Texture/Framebuffer/Sampler/Program, all of which now have ids. Cheapest P4a test win. |
| `MG_Test/State/RenderStateTest.cpp` | | | render state incl. the pixel-store setters |
| `MG_Test/Program/ProgramTest.cpp` | 4990 L | | link, reflection, uniforms |
| `MG_Test/Program/ProgramArtifactsTest.cpp` | 133 L, 5 `TEST(ProgramArtifacts,…)` | `AliasesAreTheSameTypes` `:28`, `TypeFactsIsPodOf44Bytes` `:44`, `VisitFieldsCoversEveryMember` `:80`, `VisitFieldsPassesConstnessThrough` `:90`, `SizesArePinnedOnThisToolchain` `:115` | **the archive contract `MGPProgramDesc::Reflection` serialises through**; `:115` is where the libc++/NDK numbers get read from (`ProgramArtifacts.h:553-557`) |
| `MG_Test/Program/ProgramPipelineCompositeTest.cpp` | 724 L | `ProgramPipelineCompositeTest` | the composite path of §9.4 |
| `MG_Test/Program/{AsyncCompile,AsyncLink,AsyncSpirvPhase,AsyncTeardown,OptimisticStatus,ParallelShaderCompile}Test.cpp` | | | the join gates a P4a emitter will trip |
| `MG_Test/Program/ProgramInterfaceTest.cpp`, `XfbBlockVaryingTest.cpp`, `TessellationLinkTest.cpp` | | | reflection surface |
| `MG_Test/Pipe/*` | | `PipeCatalogueTest` (585 L — payload sizes and the 71-record arithmetic), `PipeInputsTest` (644 L), `TrackerTest` (917 L), `CsoCacheTest` (200 L), `SlotAllocatorTest` (292 L), `ResourceEmitTest` (1874 L), `VertexInputEmitTest` (635 L), `RenderStateSpansTest` (1368 L), `MagmaPipeIdentityTest` (217 L) | **`ResourceEmitTest` and `VertexInputEmitTest` are the two files a P4a `*EmitTest` is modelled on**; `PipeCatalogueTest` is where every new payload size assertion lands |
| `MG_Test/Util/PipeStatsTest.cpp` | | `CounterNamesAreStable` (`:260`), `SummaryLineCarriesEveryClassAndGate` (`:137`) | any new `PipeStats::CallClass` must be pinned in both |
| `MG_Test/ScopedPipeVerb.h` | | | the fixture helper for driving a verb in a unit test |

Fixture trap to carry forward — **ID-14**: `MG_Test/Buffer/BufferTest.cpp`'s `ScopedBackendOps`
scoped only `BufferBackendOps`, so under a push build 26 of 86 dispatch cases routed into the
pipe; `83302ca2` made it save/null/restore the pipe table too, the way `ResourceEmitTest`'s
`ApplierGuard` scopes the applier. **Any P4a test that installs a backend ops table needs the same
two-scope shape**, and the follow-up (a pipe-shaped mock so the same assertions run over the
handle path) is still open.

---

## 14. Named risks and open questions for the brief

1. **`Complete` cannot be a purely client-side answer today.**
   `CheckFramebufferStatus_State` (`GL_Framebuffer.cpp:2291-2294`) consults
   `ActiveBackendRejectsDistinctDepthStencil()`, and `HasNonRenderableColorAttachment` `:2288`
   reads the backend's probed format-capability cache (the comment at `:101` says so).
   `FramebufferObject::CheckCompleteness()` (`FramebufferObject.cpp:163-193`) is backend-free but
   is a **weaker** answer. Decide explicitly which one `MGPFramebufferState::Complete` carries and
   say so in the payload comment; a client that emits the GL-visible status is reading the backend
   from the client side, which P4a is supposed to be removing.
2. **Renderbuffer respecify publishes nothing** (§5.2) — a real correctness hole, not a
   narrowing item.
3. **`glUseProgram` has no `DirtySurface.def` row** (§5.1); the gate is likely to go red the
   moment P4a touches that path.
4. **Espryt's unpack shadow is never invalidated** (§6.2, `Managers.cpp:4911` vs
   `DirectGLES.cpp:10578`/`:11112`) — once P4a's descriptor carries the strides, the shadow stops
   being a benign optimisation.
5. **`FramebufferObject::SetDrawBuffer` versions the wrong attachment slot**
   (`FramebufferObject.cpp:195-199`): it bumps `m_attachmentVersions[buffer]`, i.e. the value being
   written, not the index being written to. `m_objectVersion` and the aggregate still move, so bit
   11 is safe; a *narrower* P4a shutter built on `m_attachmentVersions` would not be.
6. **The composite band's bookkeeping is unwritten.** `MGPipeHandles.h:83-94` and
   `SlotAllocator.cpp:19-23` reserve and defend the band, but nothing mints from it yet, and a
   composite is created on a cache miss inside `GetProgramForDraw` (`Core.cpp:661`) — a
   `MakeShared` on the draw path, whose death is an ordinary `~ProgramObject`.
7. **`ProgramArtifacts.h`'s libc++ size pins are inert** (`:563-565`). If the archive is to cross
   on device in P4a, the integrator owes those four numbers from an NDK build.
8. **`MGPProgramDesc` is 192 B with six SPIR-V blobs; the ring caps one record at half its
   capacity** (`ARCHITECTURE.md` §4: `RingProducer::MaxRecordBytes()`), so `create_shader_state` is
   named there as one of the two emissions that must be split by the emitter. `MGPipeForEach
   SubDataRecordRange` (`ResourceTracker.h:257`) is the existing splitter shape.
9. **ID-19's cost budget.** P3a already added +699 (Espryt) / +422 (Magma) ns/draw and rd12 moved
   +30%/+27% on device. Every P4a per-verb emission needs a suppressor slot or a version-first
   skip *before* it hashes anything — the `MGPipeCsoCache::Acquire` shape (`CsoCache.h:14-25`),
   not a hash-every-verb shape.
10. **`kUnpublishedDestroy` for pipelines and shaders** (`DirtySurface.def:301-302, 305`) should be
    resolved deliberately: a program pipeline has no wire object and a shader never crosses, so the
    honest P4a outcome may be "the program row becomes `kExplicitDestroy`, the other two stay
    `kUnpublishedDestroy` with an updated reason", not "all three close".
