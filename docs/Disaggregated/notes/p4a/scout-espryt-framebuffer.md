# Scout — Espryt's framebuffer / renderbuffer / texture surface that P4a converts

Tree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated`
@ `37da3c3a` (P3a docs commit). Every line number below was opened in this tree. Paths are repo-relative
from `MobileGL/` unless stated.

Files that carry essentially all of it:

| file | lines | what P4a touches |
|---|---|---|
| `MG_Backend/DirectGLES/DirectGLES.cpp` | 11142 | `SyncCurrentFBO`, `BindCurrentFBO`, `ForceBindCurrentFBO`, `SyncAndBindFramebufferObject`, the two texture sync lists + the unit-bindings epoch, image units, readbacks, blits, CopyImage, GenerateMipmap, the scoped state guards |
| `MG_Backend/DirectGLES/Managers.cpp` | 10698 | `BackendFramebufferObject`, `BackendTextureObject`, `BackendRenderbufferObject`, `ScratchFBOImpl`, `PixelStoreImpl`, the unpack ring staging, the death-notice consumer |
| `MG_Backend/DirectGLES/Managers.h` | 2260 | the twin classes' members and the `TwinRegistry` alias / `StateBackendObjectRegistry` two-arm template |
| `MG_Backend/DirectGLES/SlotTables.h` | 577 | `BackendSlotTable` — the `{slot,gen}` table, both `GetOrCreate` overloads, `ReleaseByHandle`, the holder list |
| `MG_Backend/DirectGLES/Utils.cpp/.h` | 2405 / 543 | format caveat/widening predicates, `ReadbackImpl` client-store walks |
| `MG_Pipe/MGPipeTypes.h`, `MG_Pipe/PipeCalls.def` | — | the payloads P4a must fill (§7) |

---

## 0. The contract P4a is filling in (already written, all stubs today)

`MG_Pipe/PipeCalls.def` rows that P4a owns (none are new; only their emitters/appliers are):

- `:113` `X(SetFramebufferState, MGPFramebufferState, kCtxState, kNone)`
- `:118` `X(SetSamplerViews, MGPSamplerViews, kCtxState, kVarTail)`
- `:119` `X(BindSamplerStates, MGPSamplerStates, kCtxState, kVarTail)`
- `:120` `X(SetShaderImages, MGPShaderImages, kCtxState, kVarTail)`
- `:133` `X(SetTextureParams, MGPTextureParams, kCtxObject, kNone)`
- `:140` `X(GenerateMipmap, MGPMipPlan, kCtxObject, kNone)`
- `:141` `X(GetTextureImage, MGPReadbackInfo, kCtxObject, kReplySlot)`
- `:139` `X(ResourceCopyRegion, MGPCopyRegion, kCtxObject, kNone)`
- `:142/:143` `X(Blit, MGPBlit, kCtxVerb, kNone)`, `X(Clear, MGPClear, kCtxVerb, kNone)`
- `:144` `X(ReadPixels, MGPReadbackInfo, kCtxVerb, kReplySlot)`
- CSO create/delete: `:105-108` `CreateSamplerState` / `DeleteSamplerState` / `CreateSamplerView` / `DeleteSamplerView`.

Payload shapes (`MG_Pipe/MGPipeTypes.h`), with the fields that decide P4a's work:

- `MGPResourceDesc` `:150-175`, 88 B. **One create/respecify shape for buffers, every texture target and
  renderbuffers** — `Target`, `StorageKind` (== `TextureStorageType`), `BindMask`, `InternalFormat`,
  `Width/Height/Depth`, `ArrayLayers/Levels/Samples`, `FixedSampleLocations`, `Immutable`,
  `HasDefinedContent`, **`ImageBindableHint` (`:166`, "client-side everImageBound; pre-emptive
  allocation")**, `ViewOf` (`:173`, texture-view storage owner), `BufferForTexBuffer` + `BufOffset/BufSize`
  (`:174-175`) for a buffer texture. So texture creation reuses P3a's `resource_create/respecify`.
- `MGPSurface` `:342-352`, 24 B: `Res`, `InternalFormat` **inline** (so the cross-object masks fall out with
  no lookup), `Kind` (Texture|Renderbuffer|None), `Layered`, `Level`, `Layer`, `UploadTarget`.
- `MGPFramebufferState` `:354-371`, 304 B: `Fbo` (`kMGPipeDefaultFramebuffer` for the default FBO),
  `Color[8]`, `Depth`, `Stencil`, **`ReadSurface` — "the RESOLVED read surface, not an index. This is what
  structurally closes the read-buffer-shared-FBO defect class" (`:359-361`)**, `DrawBuffers[8]` as
  `Int8` attachment indices with **-1 = NONE**, `Width/Height/Layers/Samples`,
  `FixedSampleLocations/IsDefault/Complete`, and `ContentHash` (`:369`) doing two jobs: the server's
  render-pass memo key and the **client-side emission suppressor**.
  *Note for the implementer:* `Color[8]` and `DrawBuffers[8]` are **8**, while the frontend today carries
  `FramebufferObject::MAX_DRAW_BUFFERS` draw buffers and `Color0..Color31` attachment points
  (`Managers.h:1579-1581` computes `MAX_COLOR_ATTACHMENT_SLOTS` as `Color31-Color0+1` = 32). The
  attachment-relocation table (§1.4) is 32 wide. This is a real width mismatch P4a has to rule on.
- `MGPSamplerView` `:288-303`, 36 B: "ONLY the view restrictions. Everything a `glTexParameter` writes lives
  on `set_texture_params` instead, **because a texture that is only an FBO attachment, only an image binding
  or only a `glCopyImageSubData` endpoint has no sampler view to hang it on**" (`:288-291`). Carries
  `InternalFormat` (aliasing format for `glTextureView`), `Target`, `MinLevel/NumLevels/MinLayer/NumLayers`,
  `Samples`, `FixedSampleLocations`.
- `MGPTextureParams` `:306-318`, 32 B: `Res`, `BaseLevel/MaxLevel`, `Swizzle[4]`, `DepthStencilMode`,
  **`ForceResync` — "Mirrors `m_forceTextureParamsResync`" (`:312-313`)**, `MinLod/MaxLod/LodBias`.
- `MGPBoundView` `:428-434` / `MGPSamplerViews` `:436-440`; `MGPImageView` `:443-452` /
  `MGPShaderImages` `:454-458`; `MGPSamplerStates` `:442-446`.
- `MGPCopyRegion` `:708-716` (64 B), `MGPBlit` `:718-727` (56 B), `MGPClear` `:729-739` (48 B),
  `MGPMipPlan` `:741-745` (16 B), `MGPReadbackInfo` `:748-756` (64 B, `Res` null for `read_pixels`:
  "the bound read surface answers").

`MGPTextureParams` has **no border colour and no filter/wrap/compare/anisotropy**; those live in
`MGPSamplerDesc` (`:283-287`, a blob of `SamplerParameters`) — but Espryt today pushes filters/wrap/LOD/
anisotropy **onto the texture object** through `SyncBuiltinSamplerToBackend` (§2.4). That mapping
(built-in sampler → sampler CSO, or → an extra params field) is P4a's second contract ruling.

---

## 1. How Espryt resolves the current draw/read framebuffer today

### 1.1 The FBO twin and its table

`FramebufferImpl::BackendFramebufferObject` — `Managers.h:1537-1610`.
Members (all of them are the server-side state P4a must relocate):

| member | line | meaning |
|---|---|---|
| `m_backendFBOId` | `Managers.h:1561` | driver FBO name, `glGenFramebuffers` in the ctor (`Managers.cpp:7015`) |
| `m_contextGeneration` | `:1562` | ES context generation the id belongs to; dtor only deletes when it matches (`Managers.cpp:7035`) |
| `m_frontendDrawBuffers[MAX_DRAW_BUFFERS]` | `:1570-1571` | the app's draw-buffer array verbatim (holes / reversals allowed) |
| `m_backendDrawBuffers[MAX_DRAW_BUFFERS]` | `:1577` | the ES-legal compacted array actually handed to `glDrawBuffers` |
| `m_backendColorSlots[32]` | `:1593` | **the permutation**: where frontend `COLOR_ATTACHMENTn` physically lives as a backend `COLOR_ATTACHMENTm` |
| `m_frontendReadBuffer` / `m_backendReadBuffer` | `:1600-1601` | read-buffer memo |
| `m_syncedFrontendAttachmentVersions` | `:1604` | per-attachment-point memo, keyed on FRONTEND attachment versions |
| `m_syncedBackendIdGeneration` | `:1609` | `g_attachmentBackendIdGeneration` at the last attachment walk |

Table: `extern TwinRegistry<FramebufferObject, BackendFramebufferObject, MGPipeKind::Framebuffer>
g_backendFramebufferObjects;` — declared `Managers.h:1612-1613`, defined `Managers.cpp:7754-7755`.
`TwinRegistry` is the alias at `Managers.h:547-553` that swallows the kind in the pull build;
`StateBackendObjectRegistry` (`Managers.h:303-540`) dispatches to `m_slotTable`
(`BackendSlotTable`, `Managers.h:538`) when `EsprytSlotTablesEnabled()` (`SlotTables.h:132-135`),
otherwise to the legacy `UnorderedMap<StateObject*, Entry>` (`Managers.h:319`).

The other two tables of this family:
- `TextureImpl::g_backendTextureObjects` — `Managers.h:1521-1522`, defined `Managers.cpp:7001`.
- `RenderbufferImpl::g_backendRenderbufferObjects` — `Managers.h:2257-2258`, defined `Managers.cpp:10695-10696`.
- (`SamplerImpl::g_backendSamplerObjects` — `Managers.h:2230-2231`, defined `Managers.cpp:10583`.)

All four still **mint** their handle inside `MG_Backend` off a frontend `GetLifetimeId()`
(`SlotTables.h:231-232`) — that is the debt `SlotTables.h:70-77` records against itself and that
P4a discharges for these kinds, exactly as P3a's seventh table (`BufferImpl::BackendBufferResourceTable`,
`Managers.h:690-692`) already does with `GetOrCreate(MGPipeHandle)` (`SlotTables.h:278-326`) and
`ReleaseByHandle` (`SlotTables.h:350-361`).

### 1.2 The frontend binding slot read

`GetFramebufferBindingSlotChecked(FramebufferTarget)` — `DirectGLES.cpp:173-193`. On the handle arm it is a
plain `MGB_CTX->GetFramebufferBindingSlot(target)` (`:176`); on the legacy arm it goes through the raw
pointer cache `g_fbSlotCache` / `g_fbSlotCacheContext` (`:156-157`, `:179-188`), which the comment at
`:159-172` explains was a **poison/verify bypass** and is therefore kept only for the pull control.
Five call sites: `:1944`, `:2242`, `:3080`, `:3210`, `:3300`.

### 1.3 `SyncCurrentFBO` — `DirectGLES.cpp:2229-2315`

Sequence, verbatim:

1. `:2233-2235` — three `CollectGarbageIfNeeded()` ticks (framebuffer, texture, renderbuffer). These are
   no-ops on the handle arm (`Managers.h:477-491`).
2. `:2237` — walks `{Draw, Read}` in that order; `lastUpdatedFBO` carries the draw object forward.
3. **The four-part memo**, `:2254-2266`:
   `slotVersion` (`slot.GetVersion()`), `objectVersion` (`currentFBO->GetObjectVersion()`), the raw
   `currentPtr` (identity only), and `g_attachmentBackendIdGeneration`. All four stamped together by
   `StampSyncedFBO` (`:2221-2227`) into `g_fboSyncedSlotVersions` / `g_fboSyncedObjectVersions` /
   `g_fboSyncedObjects` / `g_fboSyncedBackendIdGenerations` (declared `Managers.h:1690-1710`,
   defined `Managers.cpp:7756-7762`). The comment at `:2245-2253` records the bug that fixed:
   leaving the slot version to `ForceBindCurrentFBO` alone made the early-out never fire and a
   Minecraft-shaped frame re-ran the whole attachment walk on all 5495 draws.
4. `:2268-2271` — no FBO bound at all → `MGLOG_E_ONCE`, continue.
5. **Default-framebuffer branch** `:2273-2284`: nothing to sync *except* resetting
   `g_alphaWidenedDrawBufferMask` / `g_integerColorDrawBufferMask` to 0 for the DRAW target, then stamp.
6. **Same-FBO-as-draw skip** `:2286-2302`: when the READ target names the object already synced as DRAW,
   the attachment/draw-buffer work is skipped but `SyncReadBufferToBackend` is still called
   (`:2295-2298`), through a `Find` on the twin table. This is the `read-buffer shared FBO` fix
   (`draw_buffers_1`); `MGPFramebufferState::ReadSurface` is the structural replacement.
7. `:2304-2310` — resolve-or-create the twin, materialise it if empty, `SyncToBackend(currentFBO, target)`.

### 1.4 `BackendFramebufferObject::SyncToBackend` — `Managers.cpp:7521-7737`

- `:7533` `Bind(asTarget)` first.
- Draw buffers `:7537-7585`: memcmp against `m_frontendDrawBuffers`; **applied only when
  `asTarget == Draw`** (`:7551`) — the comment `:7544-7550` records the Minecraft 26.x OIT bug where a
  READ-only sync would land `glDrawBuffers` on the wrong framebuffer and falsely stamp the memo.
  `FramebufferAttachmentType::None` → `GL_NONE` (`:7558-7560`) — **that is the "draw-buffer None"
  handling**; default-framebuffer FRONT/BACK tokens pass through unremapped (`:7564-7569`);
  everything else is forced to `GL_COLOR_ATTACHMENT0 + i` (`:7571`), and `nEffectiveBuffers` is the
  highest non-None index + 1 (`:7573`).
- `RecomputeBackendColorSlots` — `Managers.cpp:7432-7519`. Three passes: draw-buffer slot `s` pins
  attachment `a` to backend point `s` (`:7453-7474`); survivors keep identity (`:7479-7486`); displaced
  attachments are parked on the lowest free point (`:7492-7504`). Returns "moved", and each moved
  attachment has its version memo forced to `~0` (`:7515-7516`). When anything moved the read-buffer memo
  is dropped (`:7580-7582`). `GetBackendAttachmentType` (`:7739-7752`) is the single reader.
- Draw-target-only mask recompute `:7587-7619`: `snormClampOutputMask`, `unormClampOutputMask`,
  `g_alphaWidenedDrawBufferMask`, `g_integerColorDrawBufferMask` (globals declared `Managers.h:1650`,
  `:1659`, defined `Managers.cpp:7308-7309`; the shader-side masks go into
  `PrgramImpl::g_snormFallbackClampOutputMask` / `g_unormFallbackClampOutputMask`).
- Read buffer `:7623-7625` → `SyncReadBufferToBackend` (`Managers.cpp:7410-7430`), which binds as READ
  first (`:7427`) then `glReadBuffer`.
- **Attachment walk** `:7627-7724`: re-arm every point when `m_syncedBackendIdGeneration !=
  g_attachmentBackendIdGeneration` (`:7632-7636`); per point compare
  `m_syncedFrontendAttachmentVersions[i] != attachmentVersions[i]` (`:7650`); an empty colour point below
  `MaxColorAttachments` is explicitly detached with `glFramebufferRenderbuffer(..., 0)` (`:7659-7666`) —
  the comment `:7651-7658` says removing that detach breaks the permutation invariant.
- **Re-entrancy** `:7726-7736`: the walk itself can re-mint a texture id, so `SyncToBackend` recurses until
  `g_attachmentBackendIdGeneration` is quiescent.

`SyncAttachmentObject` — `Managers.cpp:7142-7216`: the attachment id/layer/level resolution.
- Texture branch `:7145-7194`: find-or-create the texture twin, **`SyncMipmapsToBackend` only**
  (`:7161` — see §2.6, the parameter gap), then three shapes:
  layered → `glFramebufferTexture(target, att, id, level)` (`:7162-7165`);
  3D / 2DArray / 1DArray / CubeMapArray / 2DMSArray → `glFramebufferTextureLayer(..., level, layer)`
  (`:7166-7177`); otherwise bind + `glFramebufferTexture2D` with the cube-face target
  (`:7178-7194`, the cube-face bind rule at `:7184-7190`).
- Renderbuffer branch `:7195-7214`: find-or-create, `SyncToBackend`, `Bind()`,
  `glFramebufferRenderbuffer`.
- An empty attachment returns `true` and issues nothing (`:7215`).

Read-side helpers on the frontend READ binding: `GetReadColorAttachment` (`Managers.cpp:7280-7291`),
`IsFixedPointFallbackReadAttachment` (`:7385-7408`), `IsAlphaWidenedFallbackReadAttachment` (`:7377-7383`),
`IsAlphaWidenedColorAttachment` (`:7293-7306`), `ComputeAlphaWidenedDrawBufferMask` (`:7357-7372`),
`IsIntegerColorAttachment` (`:7344-7355`), `IsSnormFallbackAttachment` / `IsUnormFallbackAttachment`
(`:7246-7276`).

### 1.5 Binding: `BindCurrentFBO` / `ForceBindCurrentFBO` / `SyncAndBindFramebufferObject`

- `BindCurrentFBO(target)` — `DirectGLES.cpp:3206-3260`. **No fast path on the slot version** — the
  comment `:3211-3216` records `KHR-GL32.packed_pixels` reading out of the previous subtest's framebuffer.
  Handle arm: a plain `Find` (`:3229-3232`); legacy arm: the 64-slot `g_fboTwinLookupMemo`
  (`DirectGLES.cpp:140-141`, used `:3237-3243`). Default FBO → `BindFramebufferId(..., 0)` **through the
  shadow** (`:3253-3259`) — *"a raw bind here would leave the shadow claiming the previous user FBO,
  false-skipping its next re-bind"*. **This is the default-FBO-bind trap from the readback-state-guards
  work**; its siblings are `SyncAndBindFramebufferObject`'s early return (`:3267-3280`) and
  `NoteFramebufferIdDeleted` (`Managers.cpp:7096-7105`).
- `SyncAndBindFramebufferObject(fbo, target, forceSync)` — `DirectGLES.cpp:3262-3294`; used by the DSA
  clears and `BlitNamedFramebuffer`. `forceSync` → `InvalidateSyncedState()` (`Managers.cpp:7119-7140`).
- `ForceBindCurrentFBO(target)` — `DirectGLES.cpp:3296-3308`: sync+bind then stamp all four memo arrays by
  hand.

Driver-binding shadow: `g_driverFBOBindings` / `g_driverFBOBindingKnown` (`Managers.cpp:7051-7056`),
`BindFramebufferId` (`:7058-7079`), `CurrentFramebufferBinding` (`:7081-7094`, the one `glGetIntegerv`
cold pin), `NoteFramebufferIdDeleted` (`:7096-7105`), `InvalidateFramebufferBindingCache`
(`:7107-7117` — also clears the three synced-FBO arrays; called from `DirectGLES.cpp:5597`, `:6112`,
`:6211`, `:10133`, `:10577`, `:11110`).

Call sites that drive the pair (`SyncCurrentFBO` then `BindCurrentFBO`): `PrepareForDraw`
(`DirectGLES.cpp:3359` + `:3363`), `Clear` (`:4511` + `:4518`), `BlitFramebuffer` (`:6433`, `:6442-6443`),
`CopyTexImage2D` (`:7176` + `:7187`), `CopyTexSubImage2D` (`:7270` + `:7282`), `ClearBufferfi/fv/iv/uiv`
(`:7866/7912/7925/7941` + `:7869/7915/7931/7944`), the four `ClearNamedFramebuffer*`
(`:7969`, `:7986`, `:8006`, `:8026` via `ForceBindCurrentFBO`), `ReadPixels` (`:9510` + `:9513`),
`GetTexImage` (`:9681`), the multisample-resolve fallback (`:6507-6508`).

### 1.6 The fragColor MRT broadcast

`PrgramImpl` `DirectGLES.cpp:3037-3057`: the memo `g_broadcastMemoFbo` / `g_broadcastMemoSlotVersion` /
`g_broadcastMemoObjectVersion` / `g_broadcastMemoValid` / `g_broadcastMemoCount`, **keyed exactly like
`SyncCurrentFBO`'s trio** and cleared by `InvalidateBroadcastMemo()` (`:3055-3057`, called from
`DestroyEGLContext` `:11113`). Derivation `DirectGLES.cpp:3079-3102`: walk `drawFBO->GetDrawBuffers()`,
`enabledDrawBuffers = i+1` for every non-`None` slot, `g_fragColorBroadcastCount = max(count,1)`.
It is **read from the frontend, deliberately not from the backend FBO sync** (`:3076-3078`), because a
program compiled against a stale count would not be relinked until the following draw. It then enters the
program-staleness condition at `:3154` (`twin->GetFragColorBroadcastCount() != g_fragColorBroadcastCount`),
i.e. **the draw-buffer array is a shader-compilation input**. P4a must keep that ordering when the FBO
state becomes a pushed `MGPFramebufferState`.

### 1.7 The other emulations that live in this surface

- **Alpha-widened three-channel attachments** (`Managers.h:1642-1659`): the stored alpha is pinned at 1.0
  — clears write 1.0 (`SubstituteWidenedClearAlpha`, `Managers.h:1670-1680`, used
  `DirectGLES.cpp:7917-7920`, `:7933-7936`, `:7946-7949`, and for DSA at `:7961-7963` via
  `IsWidenedNamedDrawBuffer` `:7894-7907`), draws get their alpha write mask forced off
  (`RenderStateImpl::SyncRenderState(forColorClear)`, contract at `DirectGLES.h:209-226`, the applied
  mask memo `g_syncedColorMaskAlphaWidenMask` at `DirectGLES.cpp:2346`), and readback overwrites alpha
  (`IsAlphaWidenedFallbackReadAttachment` → `forceOpaqueAlpha`, `DirectGLES.cpp:9533-9543`).
  `g_integerColorDrawBufferMask` exists because `glClearBufferfv` on an integer buffer is
  `INVALID_OPERATION` (`Managers.h:1652-1658`).
- **SNORM / UNORM caveat formats**: `IsSnormFallbackAttachment` / `IsUnormFallbackAttachment`
  (`Managers.cpp:7246-7276`) → the two shader clamp masks; readback clamp
  `applyFixedPointReadClamp` (`DirectGLES.cpp:9542`, the clamp itself `:9204-9231`).
- **Layered attachment shapes**: `attachmentObject.IsLayered()` (5 reads),
  `GetTextureLayer()` (1), `GetTextureLevel()` (6), `GetTextureUploadTarget()` (4) — the three-way branch
  at `Managers.cpp:7162-7194`; the layered-blit substitute at `DirectGLES.cpp:6265-6421`.
- **Blit LOD / layered blit**: `BlitFramebuffer` `DirectGLES.cpp:6423-…`; the non-zero destination array
  layer is served by `glCopyImageSubData` (`:6288`, `:6354-6413`) because a driver may ignore the layer
  (`:6387`). The multisample replicate path builds its own programs (`:5889-5910`, `:6105-6191`) and the
  aspect split is at `:6218-6258`. `EmulateTextureLodBias` (`Utils.cpp:1749-1830`, `Utils.h:527-536`)
  is the shader-side LOD-bias emulation, invoked `Managers.cpp:9855`, gated by
  `ShouldAvoidExplicitLodBiasOnAngleLlvmpipe()` (`Managers.cpp:94-97`).
- **D24S8 dual-mode sampling**: `GL_DEPTH_STENCIL_TEXTURE_MODE` is forwarded per texture
  (`Managers.cpp:6956-6977`; cache `m_cacheDepthStencilTextureMode`, `Managers.h:1501`), and the *second*
  half — one D24S8 sampled through its own name as `STENCIL_INDEX` and through a `glTextureView` as
  `DEPTH_COMPONENT` in one pass (Better Clouds) — is why `GL_ARB_texture_view` is only advertised behind
  a host extension **and** `MG_Config::Features.EsprytEnableTextureView`
  (`BackendObject_DirectGLES.cpp:1177-1203`; the known break is `SyncTextureViewToBackend` normalising the
  view's ES internalformat independently of the storage, `:1189-1196`).
  The depth/stencil **readback** emulation is `DepthStencilSamplingReadImpl`
  (`DirectGLES.cpp:8117-8578`, entered from `:8617` and `:8721`), with `ScopedDepthBlitState` at `:6985`.

---

## 2. The texture object twin

### 2.1 The class

`TextureImpl::BackendTextureObject` — `Managers.h:1341-1517`. Public surface `:1343-1417`; the members
that are the real server state:

| member | line | note |
|---|---|---|
| `m_backendTextureId` | `1422` | driver name |
| `m_bufferImageSplitViewId` | `1441` | second buffer-texture name for `glBindImageTexture` (the rg32f→r32f split) |
| `m_viewSourceBackendTextureId` | `1447` | for a `glTextureView`: the ES name it was made a view OF |
| `m_contextGeneration` | `1455` | |
| `m_isInitialized`, `m_imageBindableStorageRequired`, `m_backendStorageImmutable` | `1456-1458` | |
| `m_bufferTextureUnsupportedReported` | `1463` | one-shot report latch |
| `m_prevTextureInfo` (`StateTextureBasicInfo`, `Managers.h:1320-1338`) | `1464` | format/w/h/d/levels/bufferExternalIndex/samples/fixedSampleLocations |
| `m_syncedContentVersion` | `1468` | |
| `m_syncedShapeContextId`, `m_syncedShapeGeneration`, `m_syncedShapeParamsVersion` | `1481-1483` | the cheap-gate trio |
| `m_cacheSamplerParameters`, `m_cacheLodRange` | `1484-1485` | |
| `m_cacheBorderColor` / `I` / `UI` / `Form` | `1490-1493` | **all four**, because none alone identifies the driver's border colour |
| `m_cacheSwizzleParams` | `1494-1495` | |
| `m_cacheDepthStencilTextureMode` | `1501` | |
| `m_syncedSamplerVersion`, `m_syncedTextureParamsVersion` | `1502-1503` | |
| `m_forceTextureParamsResync`, `m_forceSamplerResync` | `1507`, `1516` | the two re-mint overrides (`MGPTextureParams::ForceResync` mirrors the first) |

`IsDrawSyncClean(t, contextId, samplingGeneration)` — `Managers.h:1399-1417`: the aggregate gate that is
*exactly* the conjunction of the three per-draw sync calls' own early-outs (documented `:1383-1394`; it
must never be more permissive).

Unit binding shadow: `g_boundTexturesCache[192][TextureTargetCount]` (`Managers.h:1530-1532`, defined
`Managers.cpp:6998-7000`), `g_activeTextureUnit` (`Managers.cpp:6997`), `ActivateTextureUnit`
(`Managers.cpp:6980-6986`), `UnbindTexture` (`:6988-6995`), `Bind(target, unit=TempTextureUnit=0)`
(`Managers.cpp:4729-4743`, `TempTextureUnit` at `Managers.h:1340`).

### 2.2 `SyncTextureObjectToBackend` — `DirectGLES.cpp:1501-1577`

Find-or-create (`:1507-1512`), then a **by-value SharedPtr copy** of the twin
(`:1514-1526`) because the sync can re-enter (a texture view syncs its storage texture first) — on the
slot arm the copy is kept only to hold the twin alive. Then, in order:
`RequireImageBindableStorage` if asked (`:1528-1530`), `SyncTextureParamsToBackend` (`:1531`),
`SyncBuiltinSamplerToBackend` (`:1532`), `SyncMipmapsToBackend` (`:1533`), and — if the storage sync
re-minted the name — **both parameter pushes again** (`:1541-1544`, `NeedsParameterResync()`
`Managers.h:1397`). Handle arm re-resolves once (`:1546-1566`); legacy arm re-resolves and repairs
(`:1568-1576`).

### 2.3 The per-draw work lists and the unit-bindings epoch

`DirectGLES.cpp:1579-1990` is the block P4a rewrites. In order:

- **The identity snapshot.** `UnitBindingsSnapshot` on the push arm holds **lifetime ids, not weak_ptrs**
  (`:1594-1622`; `LifetimeIdOf` `:1624-1630`), with the legacy weak_ptr fields kept under
  `MOBILEGL_PIPE_LEGACY_MEMOS` (`:1616-1621`) and the arm chosen at **runtime** via
  `MGB_UNIT_BINDINGS_HANDLE_ARM` (`:1632-1636`, `#undef` at `:1694`). Pull-build copy of the same two
  functions at `:1696-1727`. `CaptureUnitBindings` `:1638-1664`, `UnitBindingsUnchanged` `:1666-1693`.
  The design note at `:1579-1593` says explicitly what the snapshot does **not** cover (completeness
  flips, high-water mark, context identity, ES context generation, the program keys).
  **This is the ~115-line derivation the task names.**
- **`CurrentUnitBindingsEpoch(maxTouchedUnit)`** — `:1746-1773`, over
  `g_observedUnitBindings` / `…ContextId` / `…Generation` / `…MaxUnit` / `g_unitBindingsEpoch`
  (`:1730-1734`). Gate: `(contextId, bindGeneration, maxTouchedUnit)`; on a miss it re-walks and bumps
  the epoch only when the bindings really moved. Stats gate `Gate::EsprytUnitBindingsEpoch` (`:1757`, `:1762`).
- **`UnitTextureSyncEntry`** — `:1801-1805`: `{const SharedPtr<ITextureObject>* slot,
  ITextureObject* texture, BackendTextureObject* backend}`. **Entries borrow, never own**
  (rationale `:1783-1800`), and `PairingsIntact` (`:1808-1813`) is the structural net under the keys
  (a DSA by-name slot swap that never reached the bind generation would otherwise drive texture A's twin
  from texture B's state).
- **`g_unitTextureSyncList`** + keys `:1814-1820`
  (`Valid`, `ContextId`, `MaxUnit`, `ContextGeneration`, `Epoch`, `SamplingGeneration`).
- **`g_fboTextureSyncList`** + keys `:1825-1830`
  (`Fbo`, `SlotVersion`, `ObjectVersion`, `ContextId`, `ContextGeneration`).
- **`DrawTextureSyncKeys`** `:1839-1844` and `CaptureDrawTextureSyncKeys()` `:1846-1858`
  (`contextId`, `maxTouchedUnit`, `samplingGeneration`, `unitBindingsEpoch`).
- **`SyncNeccessaryTextures(keys)`** `:1860-1988`.
  Unit list hit path `:1879-1901` (per entry: `IsDrawSyncClean` then the three syncs);
  rebuild `:1902-1930` (skips `IsUndefinedDefaultTexture`, `:1918`).
  **FBO list `:1944-1987` walks the DRAW slot only** (`:1944`), over
  `currentFBO->GetAllAttachmentObjects()` with `attachment.IsTexture()` (`:1970-1976`); a null draw FBO
  clears the list (`:1984-1987`). Stats gate `Gate::EsprytTextureSyncList` (`:1889`, `:1904`).
  No-arg wrapper `:1990` used by every non-draw entry point.

### 2.4 `SyncTextureParamsToBackend` / `SyncBuiltinSamplerToBackend`

- `SyncBuiltinSamplerToBackend` — `Managers.cpp:6671-6780`. Gate `m_syncedSamplerVersion == version &&
  !m_forceSamplerResync` (`:6684`). Multisample targets: cache and return without any GL
  (`:6706-6709`). Then min/mag filter (`:6732-6744`, via `ResolveBackendMinFilter`), wrap S/T
  (`:6749-6750`), wrap R only when `SupportsWrapR` (`:6751-6755`), compare func/mode (`:6756-6757`),
  min/max LOD (`:6758-6765`), max anisotropy behind `SupportsTextureFilterAnisotropy` (`:6766-6775`).
  **All of it via `glTexParameter*` on the texture object, after `Bind(target)` at `:6711`.**
- `SyncTextureParamsToBackend` — `Managers.cpp:6782-6978`. Gate
  `m_syncedTextureParamsVersion == version && !m_forceTextureParamsResync` (`:6794`);
  multisample refresh-but-skip at `:6821-6825`; `Bind(target)` `:6827`; base/max level `:6837-6847`;
  swizzle with the alpha-widening substitution `:6852-6864` and the image-widening substitution
  `:6865-6879`; border colour (all three forms + form, `:~6900-6954`); `GL_DEPTH_STENCIL_TEXTURE_MODE`
  `:6956-6977`.

Both `Bind()` on `TempTextureUnit` (unit 0), which is why `BindCurrentTextures` has to re-establish the
real unit bindings afterwards (`DirectGLES.cpp:3687-3693`).

### 2.5 `SyncMipmapsToBackend` — `Managers.cpp:5672-…`

- Texture-view short-circuit `:5683-5686` → `SyncTextureViewToBackend`.
- **Cheap gate** `:5700-5707`: `m_isInitialized && shapeContextId && ctx live && shapeContextId ==
  GetTextureContextId() && shapeGeneration == GetSamplingResolutionGeneration() && contentVersion ==
  GetContentVersion() && shapeParamsVersion == GetTextureParamsVersion() && StorageType == Mipmap`.
- `IsComplete() || HasAnyDefinedMipmapLevel()` bail `:5734-5738`.
- **Second fast path** `:5751-5778`: content version unchanged + a `StateTextureBasicInfo` probe equal to
  `m_prevTextureInfo` → re-stamp the cheap gate and return without the scratch bind.
- `Bind(target)` `:5780`; `currentTextureInfo` built `:5785-5792`.
- `TextureStorageType::Mipmap` `:5794-6503`:
  - `needsRegeneration` `:5800`; immutable storage forces `RecreateBackendTexture()` `:5801-5804`.
  - `imageWidening` = `GetImageBindableStorageWidening(format)` only when `m_imageBindableStorageRequired`
    (`:5810-5813`).
  - **`canAppendMipmaps`** `:5815-5827` (append-only level growth) → the `glTexImage2D/3D` append loop
    `:5833-5910`.
  - Full regeneration `:5912-…`: `ApplyImageBindableStorageWidening` `:5920`; multisample clamp
    `:5923-5931`.
  - **The per-level upload loop** `:6184-6500` — see §2.7.
- `TextureStorageType::Buffer` `:6504-…`: **calls `BufferImpl::EnsureBufferResource(buffer)` at
  `:6520`** (the one place a texture sync pulls a buffer twin — and therefore the one texture path that
  reaches `SyncPersistentMappedRange`, see §5), then `CallTexBuffer` / `CallTexBufferRange`
  (`:6589-6598`), and mints/points `m_bufferImageSplitViewId` for the image split (`:6606-6630`,
  rationale `:6533-6549`).

`SyncTextureViewToBackend` — `Managers.cpp:5579-5670`: syncs the storage owner first (`:5600-5601`,
by-value SharedPtr for the same rehash reason), short-circuits when
`m_viewSourceBackendTextureId == storageBackendTextureId` (`:5613-5616`), otherwise
`RecreateBackendTexture()` (`:5620`, which also bumps `g_attachmentBackendIdGeneration`) and calls
`ResolveTextureViewEntryPoint()` (`:5630-5634`). `StampViewSyncKeys` `:5569-5577`.

### 2.6 Where an attachment-only / image-unit / CopyImage texture gets — or misses — its parameters

**This is the exact answer the task asks for.**

| how the texture is reached | code path | gets `SyncTextureParamsToBackend` + `SyncBuiltinSamplerToBackend`? |
|---|---|---|
| bound to a sampler unit ≤ high-water mark | `SyncNeccessaryTextures` unit list, `DirectGLES.cpp:1898-1900` (hit) / `:1919-1920` → `SyncTextureObjectToBackend` (miss) | **yes** |
| attachment of the **DRAW** framebuffer | `SyncNeccessaryTextures` FBO list, `DirectGLES.cpp:1963-1965` (hit) / `:1974-1975` (rebuild) | **yes** |
| attachment of the **READ** framebuffer only | `SyncCurrentFBO` `:2304-2310` → `SyncToBackend` → `SyncAttachmentObject` `Managers.cpp:7142-7194`, which calls **`SyncMipmapsToBackend` at `:7161` and nothing else** | **NO** — storage is synced, parameters are not. `SyncNeccessaryTextures`'s attachment list reads only `GetFramebufferBindingSlotChecked(FramebufferTarget::Draw)` (`DirectGLES.cpp:1944`) |
| bound to an image unit | `SyncImageTextureBinding` → `SyncTextureObjectToBackend(imageBinding.Texture, /*imageBindable=*/true)` (`DirectGLES.cpp:2071`) | **yes**, and `RequireImageBindableStorage` additionally forces `m_forceTextureParamsResync = true` (`Managers.cpp:4787`) so the swizzle override lands |
| a `glCopyImageSubData` endpoint | `MakeGLESCopyImageEndpoint` → `SyncTextureObjectToBackend(endpoint.Texture)` (`DirectGLES.cpp:7588`); renderbuffer endpoint → `SyncRenderbufferObjectToBackend` (`:7547-7562`, `:7567`) | **yes** |
| `GenerateMipmap` target | `DirectGLES.cpp:7433` / `:7436` → `SyncTextureObjectToBackend` | **yes** |
| `GetTexImage` source | `DirectGLES.cpp:9678` `SyncNeccessaryTextures()` — i.e. only if it is unit-bound or a DRAW attachment; then a bare `Find` at `:9693` | **only incidentally** |

So today parameters ride on the *unit binding* and on the *draw* attachment set. `MGPTextureParams`
(`MGPipeTypes.h:306-318`) is per-`Res` and independent of any view, which is the fix — but the
implementer must not assume the current behaviour is complete: the READ-attachment-only case is a
pre-existing gap that P4a either preserves (bug-for-bug) or closes deliberately, and the choice must be
written down. The corresponding negative control is easy: a texture attached only as READ with a
non-default swizzle / `DEPTH_STENCIL_TEXTURE_MODE`, read back with `glReadPixels`.

### 2.7 Upload shapes — whole box vs rect vs jobs, and the PBO unpack ring

Per dirty (uploadTarget, level) in `Managers.cpp:6185-6499`:

1. Conversion fallbacks first, each of which takes the level **off** the sub-rect path by pointing
   `uploadData` at a fresh buffer: `PrepareFallbackUpload` (`:6215-6217`),
   `PreparePackedNormUpload` (`:6218-6220`), `PrepareImageWidenedUpload` (`:6225-6227`, defined `:5442`).
2. `subRectEligible` — `:6241-6246`: `uploadData == mipData` (unconverted) **and** a non-empty dirty
   region that does not cover the whole level **and** whole-texel byte size **and** the backend upload
   size equals the shadow texel size. `bpp = byteSize / texelCount` (`:6247`).
3. Union box (`GetStorageDirtyRegion`, `:6237`) vs **scatter rects** (`GetStorageDirtyRects`, `:6271-6273`,
   bounded by `MipmapStorage::kMaxDirtyRects`). **`dirtyRectCount` is forced to 0 whenever the unpack ring
   is available** (`:6280-6282`) — the comment records the measured +6 ms/frame Mali cliff from ~100
   one-rect jobs; "One box, one job".
4. **Unpack-ring staging plan**, decided once for the level (`:6290-6353`): three shapes matching the
   three branches — N rects (`:6310-6322`), one box (`:6323-6331`), whole level verbatim (`:6332-6353`).
   `StageBlocksIntoUnpackRing` (`Managers.cpp:4947-5001`, `UnpackStagingBlock` `:4935-4945`) repacks
   **tightly**, which is why the ring path issues no `glPixelStorei` at all — the surrounding
   `ScopedDefaultUnpackState` already holds ROW_LENGTH/IMAGE_HEIGHT at 0 (`:6296-6302`).
   `UnpackRingPixelOffset` `:5003-5006`.
5. `ringStaged` → `BindPixelUnpackBufferId(UnpackRingBufferId())` `:6357-6359`, restored to 0 at
   `:6492-6497`.
6. **Stats** `:6360-6395`: `ByteClass::StageTexture`, `CallClass::TextureUploadEmissions`,
   `TextureUploadRectEmissions` / `TextureUploadBoxEmissions`, `TextureUploadJobs` (rects → N jobs,
   box → 1). *"the box/rect split is counted separately from the bytes on purpose: SSIM is blind to it"*.
7. The dispatch `:6396-6491`: 2D/CubeMap `glTexSubImage2D` in three shapes (`:6399-6431`),
   3D/2DArray/CubeMapArray `glTexSubImage3D` in three shapes (`:6438-6485`).

The ring itself is `BufferImpl`'s: `g_unpackRing` (`Managers.cpp:866-872`), `kUnpackRingInitialBytes`
4 MiB / `kUnpackRingMaxBytes` 64 MiB / alignment 64 (`:803-810`), `UnpackRingAvailable`
(`:3466-3469`, honours `MG_Config::Features.EsprytDisableUnpackRing`), `UnpackRingAllocate` (`:3471-3476`),
`UnpackRingMappedPtr`/`BufferId`/`MaxBytes` (`:3478-3480`), `UnpackRingOnPresent` (`:3482`),
`ResetRingForNewContext` (`:2577`). `ScopedDefaultUnpackState` `Managers.cpp:4851-4880`.

Widening helpers: `GetWidenableClientComponentCount` / `IsIntegerWidenableFormat` /
`PrepareChannelWidenedUpload` / `PreparePackedIntWidenedUpload` (`Managers.h:1292-1318`);
`ShouldUseCaveatTextureFormat`, `BackendTextureFormatAddsAlpha`, `GetImageBindableStorageWidening`
(`Utils.h:63`, `:69`, `:130-161`; `Utils.cpp:245`, `:270`, `:280-…`).
Target mapping: `MapToBackendTextureTarget` (`Managers.h:1243-1253`), `GetBackendUploadSize`
(`:1273-1278` — 1D arrays move layers from y to z).

### 2.8 Image units and the format recast

`SyncImageTextureBinding(unit)` — `DirectGLES.cpp:2057-2135`: high-water mark `g_imageUnitHighWaterMark`
(`:2055`), `TrackWritableImageBufferUnit` (`:2062`, counter at `:2040-2047`), unbind path `:2066-2069`,
`SyncTextureObjectToBackend(tex, true)` `:2071`, layered/layer resolution via
`SupportsLayeredImageBinding` (`:2072-2074`, rationale `:1992-1999`), the buffer split view
(`:2120-2124`), and the **bind-format recast** (`:2126-2132`, rationale `:2075-2083`: a `GL_RG32F` bind
is `INVALID_VALUE` on 19/26 non-core formats on Adreno and 25 on both Malis).
`SyncImageTextureBindings()` `:2169-2179`; the per-draw gate
`SyncImageTextureBindingsForDraw(keys)` `:2202-2214`, keyed on `(contextId, samplingGeneration,
g_backendContextGeneration)` with the deliberate note (`:2195-2201`) that the key is the frontend
sampling-resolution generation and **not** the backend re-mint counter.
`MarkWritableImageBufferTexturesGpuWritten()` `:2149-2167`.

### 2.9 Re-mint and the FBO coupling

`RecreateBackendTexture()` — `Managers.cpp:4790-4840`: `ScratchFBOImpl::NoteTextureIdDeleted`
(`:4792`), **`++FramebufferImpl::g_attachmentBackendIdGeneration`** (`:4797`), delete under the matching
context generation (`:4798-4800`), scrub `g_boundTexturesCache` (`:4801-4807`), regen (`:4810`), reset
`m_isInitialized` / `m_backendStorageImmutable` / `m_prevTextureInfo` (`:4818-4820`), reset **every**
parameter cache and set both force flags (`:4829-4839`).
`RequireImageBindableStorage` — `Managers.cpp:4752-4788`: sticky, clears `m_isInitialized`,
**re-marks every already-uploaded level dirty** (`:4769-4779`), sets `m_forceTextureParamsResync`
(`:4787`). `g_attachmentBackendIdGeneration` is defined at `Managers.cpp:7763`.
Destructor `Managers.cpp:4693-4727` (note the `/sdcard/MG/exp_leak_texture_deletes` experiment latch at
`:4711-4718` — an existing temp-experiment hook in the destructor).

### 2.10 CopyImage

`CopyImageSubData` — `DirectGLES.cpp:7696-…`; endpoint resolution `MakeGLESCopyImageEndpoint`
`:7564-7603` (renderbuffer at `:7566-7573`, texture at `:7587-7602`, with the explicit *no* axis remap
for 1D arrays at `:7592-7598`); `GLESCopyImageEndpoint` at `:7530-7542`;
`GetCopyImageEndpointFormat` `:7605-7608`; `CanMirrorCopyImageShadow` `:7613-7617` and the CPU-shadow
mirror below it (`:7619-…`, `:7790` "the CPU shadow is what glGetTexImage answers from, so it has to
follow the same move"). The layered-blit substitute reuses `glCopyImageSubData` at
`DirectGLES.cpp:6288`, `:6406`.

### 2.11 GenerateMipmap

`GenerateMipmap(target)` — `DirectGLES.cpp:7417-7462`.
`EnsureGenerateMipmapStorageAllocated` (`:6707-6752`) for R11F_G11F_B10F / depth-only / RGB16F / RGB32F;
`GenerateThreeChannelFloatMipmapOnCpu` (`:7432-7435`, filtered on the CPU so dirty levels ride down with
the sync); `GenerateDepthTexture2DMipmap` (`:7439`), `GenerateColorTexture2DMipmap` (`:7444`);
`ScopedDetachedTextureFramebufferAttachments` (`:7453`) then `ScopedCompleteFramebufferBinding` (`:7455`)
then `glGenerateMipmap` (`:7460`).

---

## 3. The renderbuffer twin

`RenderbufferImpl::BackendRenderbufferObject` — `Managers.h:2235-2255`. Members:
`m_backendRBOId`, `m_contextGeneration`, `m_isInitialized`, `m_cacheInternalFormat`, `m_cacheWidth`,
`m_cacheHeight`, `m_cacheSamples` (`:2248-2254`).

- ctor `Managers.cpp:10585-10597` (`glGenRenderbuffers`, stamps `g_backendContextGeneration`).
- dtor `Managers.cpp:10599-10612`; no binding shadow exists (`:10606-10607` — `Bind()` always issues).
- `Bind()` `Managers.cpp:10614-10619` — unconditional `glBindRenderbuffer`.
- `SyncToBackend` `Managers.cpp:10621-10693`: **storage respecify is memoised on the four-field cache**
  (`:10634-10640`); otherwise `Bind()`, `GenerateRenderbufferFormatInfo` (`:10650`),
  `ClampSamplesToBackendSupport` for the multisample form (`:10664-10668`) or
  `glRenderbufferStorage` (`:10670-10671`), and an explicit `GL_OUT_OF_MEMORY` probe that reports
  `ErrorCode::OutOfMemory` to the application (`:10673-10683`) — the deferred-allocation model means the
  error lands on whatever entry point triggered the sync (`:10652-10658`).
- Table `Managers.cpp:10695-10696`. Reached from `SyncAttachmentObject` (`Managers.cpp:7195-7213`),
  from `SyncRenderbufferObjectToBackend` (`DirectGLES.cpp:7547-7562`, used by CopyImage), and ticked by
  `SyncCurrentFBO` (`DirectGLES.cpp:2235`).
- **Lifetime id / death**: `RenderbufferObject` raises `NotifyStateObjectDestroyed`; the consumer arm is
  `Managers.cpp:208-210`.

Frontend members read: `GetInternalFormat` (7 sites), `GetExternalIndex` (4), `GetWidth`, `GetHeight`,
`GetSamples` (2 each).

---

## 4. Readback paths, and what a server without the frontend address space needs

### 4.1 `ReadPixels` — `DirectGLES.cpp:9488-9636`

`IsLegacyNativeReadPixelsFormat` / `…Type` / `IsSupportedDepthStencilReadPixelsPair`
(`:9455-9486`) decide `useNativeReadback`; `GetReadbackChannelMapping` + `GetReadbackDstPixelSize`
decide `convertible` (`:9497-9499`). Then:
`SyncNeccessaryTextures()` `:9507`, `SyncCurrentFBO()` `:9510`, `BindCurrentFBO(Read)` `:9513`,
`ScopedPackState packParamsScope(PackStateFromContext())` `:9516`,
`glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)` `:9518-9524`.
`packSwapBytes` `:9530`, `forceOpaqueAlpha = IsAlphaWidenedFallbackReadAttachment()` `:9533`,
`nativeFastPair` `:9537-9539`; conversion path `:9540-9550`; depth/stencil helpers `:9555-9560+`;
the pack-PBO resolution `GLuint packBufferId` `:9575-9585` (**via
`BufferImpl::EnsureBufferResource(pixelPackBufferObject)` at `:9577`**), `ScopedPixelPackBuffer` `:9589`,
`glReadPixels` `:9593`, the retry-on-error via conversion `:9594-9617`, and the
**PBO → frontend shadow writeback** `:9618-9634` (`WritebackFromBackend` `:9626` +
`BumpBufferMutationEpoch()` `:9628`).

### 4.2 `GetTexImage` — `DirectGLES.cpp:9655-…`

`IsNativeGetTexImagePair` `:9640-9653`; `SyncNeccessaryTextures()` `:9678`; `SyncCurrentFBO()` `:9681`;
resolves the texture off the **active unit's binding slot** (`:9683-9691`) and does a bare
`g_backendTextureObjects.Find` (`:9693-9702`) — it does **not** create a twin, it bails.
Then the cross-FBO resolve barrier note (`:9704-9709`), widening probes at `:9810`, `:9818`,
the temp-FBO + `ReadPixelsViaFormatConversion` route (`:9933`, `:9956`), the pack-PBO resolution
`:10001-10011` and the PBO writeback `:10029-10043`.

**The shadow fallback**: `GetTexImageViaShadowConversion` — `DirectGLES.cpp:9244-9300+`. Valid *only*
while the CPU shadow is authoritative, which holds for non-renderable formats (`:9246-9247`). Reads
`textureMipmapObject->MapMipmapData(uploadTarget, level)` (`:9268`), takes the raw-packed shortcut when
`IsRawPackedPixelTransfer` (`:9277-9287`, so an RGB9_E5 shared exponent is not canonicalised), else
`DecodeShadowDataToWideRGBA` (`:9292-9297`) then `ReadbackImpl::StoreWideRowsToClient`.

`ReadPixelsViaFormatConversion` — `DirectGLES.cpp:9015-9242`; the fixed-point read clamp `:9204-9231`.
`ReadbackImpl` client-store walks: `Utils.cpp:1973-2404`, `StoreWideRowsToClient` `Utils.cpp:2362`,
`StorePackedWordsToClient` `Utils.cpp:2387` (declarations `Utils.h:188-246`); the PBO writeback inside
the shared row walk is `Utils.cpp:2343-2352`.
`StoreReadbackRowsToClient` (depth/stencil destination walk) — `DirectGLES.cpp:8040-8073`, PBO writeback
`:8062`, epoch bump `:8068`.

### 4.3 The scoped driver-state guards (the readback-state-guards architecture)

`DirectGLES.cpp:6545-6644` and `:6795-6900+`:
`ScopedFramebufferBinding` `:6555-6573` (saves/restores through `CurrentFramebufferBinding` +
`BindFramebufferId`, never `glGetIntegerv`), `ScopedPackState` `:6576-6588`, `PackStateFromContext`
`:6592-6596`, `ScopedPixelPackBuffer` `:6601-6607` (always returns the binding to 0),
`ScopedScissorDisable` `:6612-6626` (reads the render-state shadow), `TempFBOBinder` `:6631-6644`,
`ScopedCompleteFramebufferBinding` `:6795-6803`,
`ScopedDetachedTextureFramebufferAttachments` `:6805-…` — **the one direct-iteration site over a twin
table**: push arm uses `g_backendFramebufferObjects.ForEachLive` (`:6877-6884`), legacy arm walks
`begin()/end()` and must test `stateRef.expired()` by hand (`:6885-6895`), pull arm keeps the pre-P2 walk
verbatim (`:6896-6899`). `SlotTables.h:61-68` says the per-entry `weak_ptr` survives **only** for this
caller. `ScopedDepthBlitState` `:6985`.
`PixelStoreImpl` (PACK shadow): `Managers.h:1792-1806`, `Managers.cpp:8004-8054`
(`ApplyPackState` `:8019`).
`ScratchFBOImpl`: `Managers.h:1734-1781`, `Managers.cpp:7767-8002`
(`EnsureId` `:7833`, `EnsureColorAttachment2D` `:7847`, `EnsureCompleteTinyFramebufferId` `:7960`,
`NoteTextureIdDeleted` `:7984`).

### 4.4 What a server without the frontend address space needs

Enumerated so the implementer does not have to re-derive it:

1. **A destination that is not a client pointer.** Every readback today writes either into the
   application's `void* pixels` or into a frontend `BufferObject`'s CPU shadow via
   `WritebackFromBackend` (four sites, §5). `MGPReadbackInfo` (`MGPipeTypes.h:748-756`) already carries
   `DstOffset`/`DstSize` and is `kReplySlot`, so the reply slot is the destination — but the
   **pack-PBO** case is different: the driver writes into the *backend* buffer store and the frontend
   shadow then has to be refreshed. That refresh is `WritebackFromBackend` + `BumpBufferMutationEpoch`,
   both frontend calls, and is P4a's hardest readback item.
2. **PACK pixel-store parameters as payload.** `PackStateFromContext` (`DirectGLES.cpp:6592-6596`) and
   the row walks read `MGB_CTX->GetPixelStoreParameters(false)` directly
   (`DirectGLES.cpp:9530`, `:8042`, `Utils.cpp:~2300`). `SetPixelPackState`/`MGPPixelPackState` exists
   in the catalogue (`PipeCalls.def:126`) — P4a has to actually push it, including the four ES-less
   fields (`IMAGE_HEIGHT`, `SKIP_IMAGES`, `SWAP_BYTES`, `LSB_FIRST`; `Managers.h:1788-1791` says they are
   honoured on the CPU from frontend state).
3. **The resolved read surface**, not the read-buffer enum: `MGPFramebufferState::ReadSurface`.
   The three helpers `IsFixedPointFallbackReadAttachment`, `IsAlphaWidenedFallbackReadAttachment`,
   `GetReadColorAttachment` (`Managers.cpp:7280-7408`) all currently walk
   `MGB_CTX->GetFramebufferBindingSlot(Read)`; with the inline `InternalFormat` on `MGPSurface`
   (`MGPipeTypes.h:344`) they become pure functions of the pushed record.
4. **The texture CPU shadow.** `GetTexImageViaShadowConversion` reads `MapMipmapData`
   (`DirectGLES.cpp:9268`) and the CopyImage mirror writes it (`DirectGLES.cpp:~7790`,
   `MarkStorageDirty`). A split server has no shadow. Either the shadow stays client-side and the server
   answers only from the driver, or a mirror is added — this is the texture analogue of P8's index host
   mirror and should be recorded, not solved, in P4a.
5. **Error reporting.** `BackendRenderbufferObject::SyncToBackend` calls `MGB_CTX->RecordError`
   (`Managers.cpp:10677-10682`); `GenerateMipmap` calls `RecordGLError` (`DirectGLES.cpp:7461`). Both
   need the reverse channel.

---

## 5. `SyncPersistentMappedRange` / `SyncGpuWrites` sites still touching textures

Full census in `MG_Backend/DirectGLES` (the grep is exhaustive):

| site | file:line | texture-related? |
|---|---|---|
| `drawBuffer->SyncPersistentMappedRange()` (indirect) | `DirectGLES.cpp:301`, `:5117-5118`, `:5219-5220` | no (indirect buffers) |
| `indexBuffer->SyncPersistentMappedRange(); SyncGpuWrites()` | `DirectGLES.cpp:4863-4864`, `MultiDraw.cpp:511-512` | no |
| `bufferObject->SyncPersistentMappedRange()` inside the ensure path | `Managers.cpp:2799`, `:2976` (D-N note `:2703-2718`) | **reached from the texture path**: `SyncMipmapsToBackend`'s buffer-texture branch calls `BufferImpl::EnsureBufferResource(buffer)` at `Managers.cpp:6520` |
| `bufferObject->SyncGpuWrites()` | `Managers.cpp:4424`, `:4549` (fp64 narrowing; ID-15 M-3 is the RULED P3a deviation) | no |
| `MarkBufferGpuWritten` / `MarkGpuWritten` for writable image **buffer textures** | `DirectGLES.cpp:2162-2164` (from `MarkWritableImageBufferTexturesGpuWritten`, `:2149-2167`) | **yes — the only texture-driven GPU-write announcement** |
| SSBO / atomic-counter marks | `DirectGLES.cpp:506-510`, `:556-558` | no |
| `WritebackFromBackend` (pack PBO) | `DirectGLES.cpp:8062`, `:9626`, `:10036`, `Utils.cpp:2343` | **yes — these are the readback writebacks**, each paired with `BumpBufferMutationEpoch()` (`:8068`, `:9628`, `:10038`, `Utils.cpp:2351`) |
| `WritebackFromBackend` (XFB capture) | `DirectGLES.cpp:1017`, `:1125` | no |
| `Ops_ReadbackFromGpu` | `Managers.cpp:1726-1761`, tracked wrapper `:1828-1829`, table row `:2300` | no |

So for P4a: **one** texture-owned `SyncPersistentMappedRange` reach (the buffer-texture backing store, via
`EnsureBufferResource`), **one** texture-owned GPU-write mark (writable image buffer textures), and
**four** readback writebacks that are texture/framebuffer-triggered but land on buffers.
`MarkBufferGpuWritten` is `Managers.cpp:2491-2520` (declared `Managers.h:753-763`); the
`BufferImpl::CurrentBufferMutationEpoch` / `BumpBufferMutationEpoch` contract is documented at length in
`Managers.h:558-593` — note it explicitly lists *"every pack-PBO `WritebackFromBackend` site
(glReadPixels/glGetTexImage)"* (`Managers.h:579`) as a bumping path.

---

## 6. Tests and traces

### 6.1 Unit tests (label `unit`)

- **`MG_Test/SanityTest.cpp`** (4378 lines) — the slot-table suite `DirectGLESSlotTable`:
  `ARecycledSlotIsANewHandleAndTheStaleOneResolvesToNothing` `:3316`,
  `RepeatedLookupsOfALiveObjectKeepOneHandle` `:3358`,
  `ANegativeLookupIsNotCachedAcrossAnotherHoldersAcquire` `:3387`,
  `FindNeverMutatesTheTable` `:3416`,
  `AWholeTableSavesResetsAndRestores` `:3450`,
  `TwoTablesOfTheSameKindShareOneSlotAndKeepTheirOwnTwin` `:3491`,
  `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` `:3537`,
  `AnnouncedDeathKeepsObjectChurnFromAccumulatingWithoutASweep` `:3596`,
  `GetOrCreateToleratesANullStateObject` `:3635`,
  `AnAnnouncedDeathReturnsTheSlotWithoutASweep` `:3662`,
  `EveryReKeyedObjectClassAnnouncesItsOwnDeath` `:3711`,
  `TheHandleArmInstallsTheDeathNoticeConsumer` `:3777`,
  `TheTwinRegistryCasesInThisBinaryRunOnTheHandleArm` `:3795`,
  **`EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` `:3878`** — drives Texture, Framebuffer,
  Renderbuffer, SamplerCso, ShaderCso, VertexElementsCso through
  `ExpectTheHandleArmDrivesThisKind` (`:3888-3899`) and then the seventh (Buffer) table by hand
  (`:3908-3958`). **This is the case P4a extends** for the kinds it converts to `GetOrCreate(handle)`.
  `ASavedCopyOfARealRegistryDropsTheTwinOnTheSameNotice` `:3963`,
  `AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane` `:4004`,
  `TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem` `:4061`.
  Fixture `ScopedDirectGLESTextureBindings` `:218-260` — **the second live holder of kind Texture**
  (saves `g_backendTextureObjects` by value at `:224`, resets with `= {}` at `:228`, restores at `:242`),
  used at `:353`, `:404`, `:2699`, `:3065`. `EsprytSlotArmEnvironment` `:90-103`.
- **`MG_Test/Framebuffer/FramebufferTest.cpp`** — 60+ `FramebufferTest.*` cases; the ones P4a's
  framebuffer record must keep green: `NamedFramebufferTextureAttachesWithoutChangingBindings` `:244`,
  `FramebufferTextureBumpsAttachmentVersionOnlyOnce` `:289`,
  `ReadPixelsAllowsPersistentMappedPixelPackBuffer` `:310`,
  `NamedFramebufferDrawBuffersDoNotModifyDefaultFramebuffer` `:446`,
  `DefaultFramebufferProvidesTextureAttachmentsForFrontAndBackAliases` `:492`,
  `FramebufferTexture3DAttachesSliceWithLayerTracking` `:825`,
  `ThreeChannelColorAttachmentsAreCompleteThroughTheWidenedSubstitution` `:961`,
  `TwoAttachmentCompositeFramebufferMatchesIrisComplementaryPass` `:972`,
  **`WidenedDrawBufferIsIdentifiedPerDrawBufferSlotNotPerAttachmentPoint` `:1194`**,
  `DrawIntoAWidenedDrawBufferReachesTheDriverWithAlphaWritesMaskedOff` `:1227`,
  `ClearIntoAWidenedDrawBufferKeepsAlphaWritableAndSubstitutesOne` `:1263`,
  plus the `PackedReadbackEncodeTest.*` block `:645-769` (pure readback encoders) and
  `ReadPixelsRejectsIntegerFormatMismatchWithReadBuffer` `:785`.
- **`MG_Test/Texture/TextureTest.cpp`** — 192 cases. Directly load-bearing for P4a:
  `DepthStencilTextureModeIsBackendVisibleThroughTheParamsVersion` `:2609`,
  `ShadowReadbackRefusesALayoutItCannotProduceInsteadOfOverrunningTheBuffer` `:1017`,
  `ShadowReadbackRefusesAPackStateItCannotHonour` `:1058`,
  `GetTexImageReadsALevelWhoseLowerLevelsWereNeverDefined` `:1971`,
  `GetTexImageOfOneCubeFacePacksIntoAOneFacePixelPackBuffer` `:2096`,
  `CopyImageSubDataRejectsALevelTheTextureDoesNotHave` `:2531`,
  `TextureStorage2DMultisampleTracksNamedObjectState` `:1921`,
  the border-colour trio `:610`, `:656`, `:698`, and the compressed-upload/PBO block `:2657-2812`.
  `MG_Test/Texture/TextureViewTest.cpp`, `MG_Test/Texture/VkClearManagerTest.cpp`.
- `MG_Test/Backend/DirectGLES/` holds only `BaseInstanceInjectionTest.cpp`,
  `EsslShaderPassTest.cpp`, `ViewportIndexRoutingTest.cpp` — **there is no backend-side framebuffer or
  texture unit test today**; P4a's emitter tests belong in `MG_Test/Pipe/` beside
  `ResourceEmitTest.cpp` and `VertexInputEmitTest.cpp` (the P3a shape), which is also where the
  `PipeCatalogueTest.cpp` size pins live.

### 6.2 Integration scenarios (label `integration-gpu` / `integration-verify`)

Registered in `MG_IntegrationTest/CMakeLists.txt`; the framebuffer/texture/renderbuffer set with its
case names:

- **`HandleRecycleScenario`** (`:979-1015+`, four+ pinned lanes): `TheReproducerRecyclesEveryName`,
  `AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput`,
  `ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents`,
  **`ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin`** (`Scenarios/HandleRecycleScenario.cpp:869`),
  **`AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin`** (`:920`),
  `DestroyedVertexArraysReturnTheirVertexElementsSlots`.
  P4a's G8 analogue: add a renderbuffer case and a sampler case here.
- `LayeredAttachmentShapeScenario` (8 cases: `LayeredThreeDColorAttachmentReachesEverySlice`,
  `NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice`,
  `LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace`,
  `LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace`,
  `LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace`,
  `LayeredCubeMapClearMaterialisedBySamplingReachesEveryFace`,
  `LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer`,
  `CubeMapFaceReadbackAnswersTheFaceItWasAskedFor`) — **the direct gate on `MGPSurface`'s
  `Layered`/`Level`/`Layer`/`UploadTarget`.**
- `LayeredTextureReadbackScenario` (2), `LayeredAttachmentBarrierScenario`.
- `ThreeChannelAttachmentScenario` (`ThreeChannelColorAttachmentsReportComplete`,
  `WidenedAttachmentReadsBackOpaqueWhileItsNeighbourKeepsItsAlpha`,
  `DstAlphaBlendingSeesOneInAWidenedAttachment`) — the alpha-widening triple.
- `SnormAttachmentScenario`, `RenderbufferBlendFormatScenario` (3 cases — the only renderbuffer-specific
  scenario), `DepthStencilReadbackScenario` + `…MatrixScenario` + `…AttachmentShapeScenario`
  (one lane, `TEST_FILTER "DepthStencilReadback*Scenario.*"` at `:639`),
  `PackedWordReadbackScenario`, `PixelStoreSweepScenario`,
  `ClearThenReadPixelsScenario` (5 cases, incl. `ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear`
  and `AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation`),
  `ClearTexImageUndefinedLevelZeroScenario`,
  `CopyImageLevelRangeScenario` / `CopyImageLayeredScenario` / `CopyImagePacked16Scenario` (own lane `:806`),
  `TextureViewScenario` (8 cases, incl. `BetterCloudsCoveragePipeline` and `CoherencyIsBidirectional`),
  `BufferTextureScenario`, `ImageSizeAfterRespecScenario`
  (`ADrawSeesTheNewSizeOfARespecifiedImageTexture` — the `RecreateBackendTexture` /
  `g_attachmentBackendIdGeneration` gate), `SampledSetStalenessScenario` (2 cases — the
  sampling-resolution-generation gate), `ImageTargetKindScenario`, `ImageLoadStoreSsoScenario`,
  `NonCoreImageFormatScenario`, `FormatlessImageBakeScenario`, `ImageFormatQualifierScenario`,
  `UnboundImageDescriptorScenario`, `IntegerBorderColorScenario` (carries a known ASan
  stack-buffer-overflow, ID-21 follow-up), `FragmentOutputArrayIndexScenario`,
  `OrientationScenario`, `FragCoordOriginScenario`, `IterationRP*Scenario` ×3, `GuiBatchScenario`.
- `ResourceSubsystemControlScenario.ClearingTheP3aBitsStopsTheEmissionsAndNotThePixels` — the G12
  template P4a copies for its own bits; `CsoContentAddressingScenario`; `PipeVerifyArmingScenario`.

### 6.3 Traces most sensitive to this surface

From `tools/trace_replay/trace_cases.json` (40 cases; 79 desktop arms):

- **`improved-transparency-minecraft-26.3`** — the OIT scratch-clear-FBO / draw-buffers-`None` /
  read-buffer bug family lives here (`Managers.cpp:7544-7550`). First trace to break on a draw-buffer or
  read-buffer regression.
- **`minecraft-1.21.4-fabric-iris-*-in-world`** (17 shader packs: bsl, complementary-reimagined,
  complementary-unbound, sundial-lite, iterationt, iterationrp, photon-v1.1/v1.3b, mellow, nostalgia,
  bliss, chocapic-v6-lite, super-duper-vanilla, makeup-ultrafast, derivative-main, …) — heavy MRT,
  attachment relocation and `glDrawBuffers` permutations; `TwoAttachmentCompositeFramebufferMatchesIrisComplementaryPass`
  is the unit-test shadow of this. `iterationrp` has no DirectGLES arm (79 = 40×2 − 1).
- **`minecraft-1.21.4-rd12-odinlite-in-world`** — the VAO/buffer-heavy fixture that cost P3a +30%/+27%
  (ID-19/ID-24); also the one that aborts on Xiaomi/Magma pre-existing (ID-23). Texture-upload traffic
  is the other half of its frame.
- **`minecraft-1.21.4-fabric-sodium-in-world`** — sub-ms frames, the per-call overhead canary.
- **`minecraft-1.21.1-neoforge-create-instancing-in-world`** (2-frame fixture, not a benchmark) and
  **`create-indirect`** (pre-existing `dev` red, BRIEF-P3A.md's first caveat — keep it in the desktop
  SSIM corpus, out of the device A/B).
- `minecraft-1.21.4-fabric-{rei,xaero-minimap,xaero-world-map,journeymap,modernui}-*` and
  `…-normal-world` — GUI/atlas-heavy, i.e. the `glTexSubImage` union-box vs scatter-rect shape and the
  unpack ring; a shape regression there is invisible to SSIM and shows only in
  `TextureUploadBoxEmissions` / `TextureUploadRectEmissions` / `TextureUploadJobs`
  (`Managers.cpp:6385-6394`).
- `minecraft-1.17-main-menu-854`, `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854`, `OpenRA` — default
  framebuffer / blit-to-default paths.

---

## 7. Working notes for the P4a implementer

1. **Seven→ten tables.** P3a converted only the Buffer kind to a call-carried handle
   (`Managers.h:690-692`). Framebuffer, Texture, Renderbuffer (and Sampler) still mint inside
   `MG_Backend` through `TwinRegistry::GetOrCreate(StatePtr)` (`Managers.h:326-372`) and still depend on
   the shared death notice (`Managers.cpp:199-245`). The buffer family's shape — client mints →
   `GetOrCreate(handle)` → `ResourceDestroy` retires → **client** frees the slot (`SlotTables.h:337-361`,
   `Managers.cpp:233-243`) — is the template. Note the `GetOrCreate(handle)` asymmetry: forward
   generations recycle, **backward generations are refused** (`SlotTables.h:301-321`).
2. **`ForEachLive` is the blocker for dropping `Entry::stateRef` on Framebuffer.**
   `ScopedDetachedTextureFramebufferAttachments` (`DirectGLES.cpp:6805-6899`) is the only
   direct-iteration site and it needs the frontend `FramebufferObject` to read its attachment array. A
   handle-keyed FBO table has no `stateRef` (`SlotTables.h:266-269`), so that walk has to be re-expressed
   over the applier's `MGPFramebufferState` records before the FBO table can lose its `StatePtr`.
3. **The `Color[8]` / `DrawBuffers[8]` vs 32 mismatch** (§0) has to be ruled on before any emitter is
   written; `MAX_COLOR_ATTACHMENT_SLOTS` is 32 at `Managers.h:1579-1581` and `RecomputeBackendColorSlots`
   is 32-wide (`Managers.cpp:7443-7448`).
4. **`ContentHash` suppression on `SetFramebufferState`** must not swallow the `fragColorBroadcastCount`
   input (§1.6): the broadcast count is read from the frontend *before* the FBO sync precisely so a
   program relinks in the same draw. If the FBO record is suppressed, the broadcast count must still be
   derivable — either it becomes a field of the record or it stays a separate emission.
5. **`ForceResync` on `MGPTextureParams`** (`MGPipeTypes.h:312-313`) is already the wire spelling of
   `m_forceTextureParamsResync`; there is **no** wire spelling of `m_forceSamplerResync`
   (`Managers.h:1508-1516`), and the failure mode it guards is not mis-filtering but an *incomplete*
   texture that samples `(0,0,0,1)`. That gap is P4a's third contract ruling.
6. **G5-style byte-identity.** P3a's G5 protects eleven buffer-pool/ring functions
   (ID-11/ID-13/ID-15). The P4a analogue candidates, if the phase wants one, are the unpack-ring
   staging (`StageBlocksIntoUnpackRing`, `Managers.cpp:4947-5001`), `RecomputeBackendColorSlots`
   (`Managers.cpp:7432-7519`) and `ScratchFBOImpl`'s attachment shadow (`Managers.cpp:7767-8002`) —
   all three are dense, well-tested and have nothing to gain from being rewritten.
7. **Exit order (ID-18/ID-21).** `EnsureProcessTeardownSentinel` / `InProcessTeardown`
   (`Managers.cpp:171-180`) gate every twin destructor (`:3556`, `:4693`, `:7026`, `:8085`, `:10432`,
   `:10600`), and `OnFrontendStateObjectDestroyed` drops notices during teardown (`:200`). Any new
   MGPipe singleton P4a adds must be heap-constructed and intentionally leaked, like the four
   `fde5fda3` made never-destroyed.
