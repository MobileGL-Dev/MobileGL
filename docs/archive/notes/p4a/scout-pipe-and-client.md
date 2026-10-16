# P4a scout — the MGPipe scaffolding and the P3a client conventions

Tree read: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch
`feat/disaggregated` @ `37da3c3a` (P3a docs; landing hash `fde5fda3`). Every line number below was opened.
Repo-relative paths are under `MobileGL/` unless the path starts with `scripts/`, `.github/` or `~/w7/`.

Read first: `~/w7/notes/p3a/INTEGRATOR-DECISIONS.md` (ID-1..ID-24) and `BRIEF-P3A.md` §A (lines 108-207) and §C
(1012-1365). P4a's roadmap row is `docs/Disaggregated/ROADMAP.md:20`.

---

# PART 1 — The MG_Pipe scaffolding P4a builds on

## 1.1 `MG_Pipe/PipeCalls.def` — the rows P4a implements

The file is 183 lines. `MGP_CALL_LIST_DOCUMENTED_COUNT 71` at `PipeCalls.def:75`; **the wire opcode is the 1-based
position in `MGP_CALL_LIST`** (`:29`), so P4a **may not reorder, insert into a group, or retire a row**. A new call is
appended at the very end (the three appended rows at `:160-166` are the precedent). Class tokens are unscoped
(`MGPipe.h:29-37`: `kScreen kCtxCso kCtxState kCtxObject kCtxVerb kCtxQuery`); flag tokens at `MGPipe.h:39-55`
(`kNone=0, kNeedsAck=1<<0, kHasBlob=1<<1, kVarTail=1<<2, kHostSpan=1<<3, kReplySlot=1<<4, kOptional=1<<5`).

Opcode = line position within `MGP_CALL_LIST` starting at `PipeCalls.def:80`. P4a's rows, with opcode:

| # | row | file:line | payload | class | flags |
|---|---|---|---|---|---|
| 2 | `ResourceCreate` | `:81` | `MGPResourceDesc` | `kScreen` | `kNone` |
| 3 | `ResourceRespecify` | `:82` | `MGPResourceDesc` | `kScreen` | `kNeedsAck` |
| 4 | `ResourceDestroy` | `:83` | `MGPHandleOnly` | `kScreen` | `kNone` |
| 17 | `CreateRenderState` | `:98` | `MGPRenderStateDesc` | `kCtxCso` | `kHasBlob` |
| 23 | `CreateSamplerState` | `:104` | `MGPSamplerDesc` | `kCtxCso` | `kNone` |
| 24 | `DeleteSamplerState` | `:105` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| 25 | `CreateSamplerView` | `:106` | `MGPSamplerView` | `kCtxCso` | `kNone` |
| 26 | `DeleteSamplerView` | `:107` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| 27 | `CreateShaderState` | `:108` | `MGPProgramDesc` | `kCtxCso` | `kHasBlob` |
| 28 | `BindShaderState` | `:109` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| 29 | `DeleteShaderState` | `:110` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| 31 | `SetFramebufferState` | `:113` | `MGPFramebufferState` | `kCtxState` | `kNone` |
| 35 | `SetSamplerViews` | `:117` | `MGPSamplerViews` | `kCtxState` | `kVarTail` |
| 36 | `BindSamplerStates` | `:118` | `MGPSamplerStates` | `kCtxState` | `kVarTail` |
| 37 | `SetShaderImages` | `:119` | `MGPShaderImages` | `kCtxState` | `kVarTail` |
| 40 | `SetGlobalConstants` | `:122` | `MGPGlobalConstants` | `kCtxState` | `kHasBlob` |
| 44 | `SetDrawProgram` | `:126` | `MGPHandleOnly` | `kCtxState` | `kNone` |
| 45 | `SetDispatchProgram` | `:127` | `MGPHandleOnly` | `kCtxState` | `kNone` |
| 47 | `SetTextureParams` | `:133` | `MGPTextureParams` | `kCtxObject` | `kNone` |
| 48 | `ResourceSubData` | `:134` | `MGPSubData` | `kCtxObject` | `kHasBlob\|kVarTail` |
| 50 | `ResourceSubDataComplete` | `:136` | `MGPSubDataComplete` | `kCtxObject` | `kNone` |
| 53 | `ResourceCopyRegion` | `:139` | `MGPCopyRegion` | `kCtxObject` | `kNone` |
| 54 | `GenerateMipmap` | `:140` | `MGPMipPlan` | `kCtxObject` | `kNone` |
| 55 | `GetTextureImage` | `:141` | `MGPReadbackInfo` | `kCtxObject` | `kReplySlot` |
| 56 | `Blit` | `:143` | `MGPBlit` | `kCtxVerb` | `kNone` |
| 57 | `Clear` | `:144` | `MGPClear` | `kCtxVerb` | `kNone` |
| 58 | `ReadPixels` | `:145` | `MGPReadbackInfo` | `kCtxVerb` | `kReplySlot` |

Notes that bind P4a:
- **No `BindSamplerState` / `SetSamplerView` singular calls exist**; `PipeCalls.def:53-57` explains why: those two
  binds live only as the array forms under `kCtxState`, so the CSO group holds 13 rather than 15.
- **`CreateVertexElements` is `kHasBlob` and `BindVertexElements` is `MGPHandleOnly`** (`:101-103`) — the shape
  `CreateShaderState`/`BindShaderState` copies.
- The `kNeedsAck` legend (`:18-24`) says the flag means *records of this call MAY require an ack*, with the per-record
  predicate `MGPipeResourceRespecifyNeedsAck(desc)` deciding. Texture/renderbuffer respecify travels on the same
  `ResourceRespecify` row and inherits that predicate — `desc.Immutable != 0` (`MGPipeTypes.h:681-683`), which for a
  texture means `glTexStorage*`. **If P4a wants a different answer for textures, the predicate is where it changes,
  not the flag word.**
- `PipeCalls.def:169-183` is the explicit-deletions footer: `GetIntegeri_v`/`GetInteger64i_v`, `GetProgramiv`,
  `ShaderStorageBlockBinding` (folded into `MGPProgramDesc`'s reflection archive), `set_pixel_unpack_state`, a
  compressed-format concept, `pipe_transfer`, and **the stage dimension of `set_sampler_views`** (the texture unit
  space is merged, not per-stage).

## 1.2 `MG_Pipe/MGPipeTypes.h` — payloads and their asserts

Every payload carries `MGP_ASSERT_POD(T, Size)` (`MGPipeTypes.h:44-46`: trivially copyable + exact `sizeof`). The
macro is `#undef`ed at `:880`. Sizes are asserted because wire records are `memcpy`d (`:21-23`).

P4a's payloads, verbatim from the header:

- **`MGPResourceDesc`** `:150-176`, `MGP_ASSERT_POD(..., 88)` at `:177`; `kMGPipeWholeBuffer = ~0ull` at `:178`.
  Fields: `Resource`; `Uint8 Target` (`Buffer | Tex1D..TexCubeArray | Tex2DMS.. | Renderbuffer | TexBuffer`);
  `Uint8 StorageKind` (`== TextureStorageType`); `Uint16 BindMask` (12 bits, see §2.1); `Uint32 InternalFormat`
  ("already resolved to an uncompressed fallback by the client"); `Width, Height, Depth`;
  `Uint16 ArrayLayers, Levels, Samples`; `Uint8 FixedSampleLocations, Immutable`; `Uint32 Usage`
  (`BufferUsage`); `Uint32 StorageFlags`; `Uint8 HasDefinedContent`; `Uint8 ImageBindableHint`
  ("client-side everImageBound; pre-emptive allocation"); `Uint32 GlNameForDiag` (**diagnostics only, never an
  identity/memo key/hash input** — `:171-174`); `MGPipeHandle ViewOf` (storage owner for a texture view);
  `MGPipeHandle BufferForTexBuffer`; `Uint64 BufOffset, BufSize`.
  **The texture half of this struct is already fully specified and unused** — P3a wrote only the buffer fields
  (`ResourceTracker.h:160-162` pins `kMGPipeResourceTargetBuffer = 0` and
  `kMGPipeResourceStorageKindBuffer = TextureStorageType::Buffer`, and says the contract package "minted no enum for
  the first list"). **P4a must mint the `Target` enum**, because `Buffer` being 0 is currently justified only by "it
  is the leading member".
- **`MGPSurface`** `:343-352`, `MGP_ASSERT_POD(..., 24)`: `Res`, `Uint32 InternalFormat` (inline "so the four
  cross-object masks fall out at push time with no lookup"), `Uint8 Kind` (`Texture | Renderbuffer | None`),
  `Uint8 Layered`, `Uint16 Level`, `Uint32 Layer`, `Uint16 UploadTarget`, `Uint16 Pad0`.
- **`MGPFramebufferState`** `:355-377`, `MGP_ASSERT_POD(..., 304)`: `MGPipeHandle Fbo`
  (`kMGPipeDefaultFramebuffer` for the default FBO), `MGPSurface Color[8]`, `Depth`, `Stencil`,
  **`MGPSurface ReadSurface` — the RESOLVED read surface, not an index** (`:359-361`, "structurally closes the
  read-buffer-shared-FBO defect class"; that defect is in memory as `read-buffer shared FBO`),
  `Int8 DrawBuffers[8]` (attachment index, `-1` = NONE), `Uint16 Width, Height, Layers, Samples`,
  `Uint8 FixedSampleLocations, IsDefault, Complete, Pad0`, `Uint32 Pad1`, **`Uint64 ContentHash`** with two jobs
  (`:369-373`): the server's render-pass memo key **and the client's emission suppressor** — "an unchanged hash means
  this record is not sent at all. **The same pattern is mandatory for every `kVarTail` `set_*` below**, or 26.2's
  redundant `glBindSampler` traffic reappears as a variable-length record per batch."
- **`MGPSamplerDesc`** `:283-286`, `MGP_ASSERT_POD(..., 32)`: `Cso`, `MGPBlobRef Parameters`.
  `:279-282`: `SamplerParameters` crosses byte for byte **including `borderColorForm`**, without which the backend
  cannot pick between `glSamplerParameterIiv`/`fv` or the `VkBorderColor` families. `SamplerParameters` is
  `MGPipeValueTypes.h:462`, `sizeof == 100` asserted at `MGPipeValueTypes.h:615`. Note the row carries **no**
  `kHasBlob` flag in `PipeCalls.def:104` even though the payload holds an `MGPBlobRef` — an asymmetry P4a should
  either fix in the flag word or document (compare `PipeApply.h:67-68`, which says `resource_respecify` deliberately
  does *not* carry `kHasBlob` because `MGPResourceDesc` has no `MGPBlobRef` member; here the member exists).
- **`MGPSamplerView`** `:293-303`, `MGP_ASSERT_POD(..., 36)`: `Cso`, `Texture`, `Uint32 InternalFormat` (aliasing
  format for `glTextureView`), `Uint8 Target`, `Uint8 Pad0[3]`, `Uint16 MinLevel, NumLevels, MinLayer, NumLayers`,
  `Uint16 Samples`, `Uint8 FixedSampleLocations`, `Uint8 Pad1`. `:288-292`: **only the view restrictions** — anything
  a `glTexParameter` writes goes on `set_texture_params`, because a texture that is only an FBO attachment, only an
  image binding, or only a `glCopyImageSubData` endpoint has no sampler view to hang it on. (That sentence is the
  design source of the ROADMAP's mandatory new scenario.)
- **`MGPTextureParams`** `:307-317`, `MGP_ASSERT_POD(..., 32)`: `Res`, `Uint16 BaseLevel, MaxLevel`,
  `Uint8 Swizzle[4]`, `Uint8 DepthStencilMode`, `Uint8 ForceResync` (mirrors `m_forceTextureParamsResync` — "the
  widened-channel carrier needs a swizzle override that the frontend params version does not move for"),
  `Uint8 Pad0[2]`, `Float MinLod, MaxLod, LodBias`. **Per texture OBJECT, independent of any view.**
- **`MGPProgramDesc`** `:323-335`, `MGP_ASSERT_POD(..., 192)`: `Cso`, `Uint32 StageMask` (`== GetLinkedShaderStages()`),
  `Uint32 GlobalUboSize`, `Uint32 ReservedNumSamplesOffset`, `Uint8 SpirvStatus`, `Uint8 NativeFloat64`,
  `Uint8 PointSizeDemoted`, `Uint8 EnableSpirvValidation`, `MGPBlobRef Spirv[6]` (per stage), `MGPBlobRef Reflection`
  ("the whole `LinkArtifacts` + `SpirvArtifacts` archive; P0.5 extracts those types out of `ProgramObject.h`").
- **`MGPBoundView`** `:432-437`, `MGP_ASSERT_POD(..., 24)`: `View`, `Texture`, `Uint32 Unit`, `Uint32 Pad0`.
  `:428-431`: no stage dimension, `TextureState::m_textureUnits` is one array of `MAX_TEXTURE_IMAGE_UNITS = 192`.
- **`MGPSamplerViews`** `:440-444`, `MGP_ASSERT_POD(..., 16)`: `Uint32 Start, Count; Uint64 ContentHash`.
  Var-tail = `MGPBoundView[Count]`.
- **`MGPSamplerStates`** `:447-451`, `MGP_ASSERT_POD(..., 16)`: same shape; var-tail is
  `MGPipeHandle[Count]` of sampler CSOs (`:446`).
- **`MGPImageView`** `:453-462`, `MGP_ASSERT_POD(..., 24)`: `Res`, `Uint32 Unit`, `Uint32 InternalFormat`,
  `Uint32 Layer`, `Uint16 Level`, `Uint8 Layered`, `Uint8 Access`.
- **`MGPShaderImages`** `:464-469`, `MGP_ASSERT_POD(..., 16)`: `Uint32 Start, Count; Uint64 ContentHash`.
- **`MGPGlobalConstants`** `:507-513`, `MGP_ASSERT_POD(..., 40)`: `ShaderCso`, `Uint32 Version`, `Uint32 Pad0`,
  `MGPBlobRef Blob`. `:506`: **default uniform block only (D6)**.
- **`MGPSubData`** `:636-648`, `MGP_ASSERT_POD(..., 72)`: `Res`, `Uint16 Target, Level`,
  `Uint8 SourceIsVerbatimLevelShadow` (replaces the backend's `uploadData == mipData` pointer comparison),
  `MGPBox UnionBox`, `Uint32 RegionCount` (`MGPSubRegion[]` in the tail), `MGPBlobRef Blob`.
  `:611-613`: **carries the union box AND the region list so the SERVER picks the upload shape** — "Mali prices
  texture upload by JOB COUNT: ~100 sprite rects against one union box measured +6 ms/frame". The **buffer half**
  convention (`:615-618` and the helpers `MGPipeSetSubDataBufferRange` `:651-660`, `MGPipeSubDataBufferOffset`
  `:661-666`, `MGPipeSubDataBufferSize` `:667`) is exactly what the texture path does **not** use: with a texture
  target, `Level`, `RegionCount` and the real box come back into play, and `SubDataBoxFault`'s buffer-only
  refusals (`PipeApply.cpp:510-531`) must be branched on `Target`.
- **`MGPSubRegion`** `:607-614`, `MGP_ASSERT_POD(..., 40)`: `Int32 X,Y,Z; Uint32 W,H,D; Uint64 SrcOffset;
  Uint32 SrcRowStride` (0 = tightly packed), `Uint32 SrcSliceStride`. `:602-605`: source strides are **carried, not
  inferred** from a pointer comparison.
- **`MGPSubDataComplete`** `:686-691`, `(..., 24)`; **`MGPCopyRegion`** `:708-716`, `(..., 64)`;
  **`MGPBlit`** `:718-727`, `(..., 56)`; **`MGPClear`** `:729-739`, `(..., 48)` (one discriminated record replacing
  `glClear`, four `glClearBuffer*`, four `glClearNamedFramebuffer*`); **`MGPMipPlan`** `:741-745`, `(..., 16)`;
  **`MGPReadbackInfo`** `:748-756`, `(..., 64)` (`Res` null for `read_pixels`: the bound read surface answers).
- **`MGPSurfaceInfo`** `:872-878`, `(..., 24)` — the reverse-channel `OnSurfaceChanged` payload.

Two blanket rules P4a inherits:
- **THE ONE BLOB RULE** stated at `MGPipeTypes.h:270-285` (on `MGPVertexElements`) and again at `:619-631`
  (on `MGPSubData`): `Blob.Size` is the record's own statement of its blob length; a **non-zero** size that
  disagrees with what the record's other fields describe is `Fatal{ProtocolCorruption}` and the call is refused; a
  **zero** size means "this record does not declare its blob" (a monolith emission, bytes travelling beside the
  record through the entry point's companion `const void*`) and is **not** a fault. The bytes read are always
  bounded by the record's *other* fields. The implementation of both halves lives at `PipeApply.cpp:521-527` and
  `PipeApply.cpp:390-402`.
- `kMGPipeMaxVertexAttribs = 32` at `:378`, pinned against `VertexArrayObject::MAX_VERTEX_ATTRIBS` in the one TU
  that sees both — `MG_Impl/Pipe/PipeFill.cpp:1035-1038`. **P4a needs the same shape for the unit/attachment/image
  bounds** (there is no `kMGPipeMaxTextureUnits`, `kMGPipeMaxColorAttachments` or `kMGPipeMaxImageUnits` today;
  `MGPFramebufferState` hard-codes 8 colours and 8 draw buffers).

## 1.3 `MG_Pipe/PipeFields.def` — the verify comparator's field lists

Purpose at `PipeFields.def:9-19`: one macro per payload listing the fields that carry MEANING; **padding is
deliberately absent** (a `Pad<n>` member is padding and is not listed) because `MOBILEGL_PIPE_VERIFY` must have zero
false positives. `gen_pipe.py` asserts — in both modes, hence in `pipe-gates` — **that every list names exactly its
struct's direct data members**; a member added without a row here fails the CI job.

P4a's existing rows (all already complete, none need adding unless a payload gains a member):
`MGP_FIELDS_MGPResourceDesc` `:44-48`; `MGP_FIELDS_MGPSamplerDesc` `:74-75`; `MGP_FIELDS_MGPSamplerView` `:77-79`;
`MGP_FIELDS_MGPTextureParams` `:81-83`; `MGP_FIELDS_MGPProgramDesc` `:85-87`; `MGP_FIELDS_MGPSurface` `:89-90`;
`MGP_FIELDS_MGPFramebufferState` `:92-94`; `MGP_FIELDS_MGPBoundView` `:108-109`; `MGP_FIELDS_MGPSamplerViews`
`:111-112`; `MGP_FIELDS_MGPSamplerStates` `:114-115`; `MGP_FIELDS_MGPImageView` `:117-118`;
`MGP_FIELDS_MGPShaderImages` `:120-121`; `MGP_FIELDS_MGPGlobalConstants` `:132-133`; `MGP_FIELDS_MGPSubRegion`
`:156-157`; `MGP_FIELDS_MGPSubData` `:159-160`; `MGP_FIELDS_MGPSubDataComplete` `:162-163`;
`MGP_FIELDS_MGPCopyRegion` `:171-172`; `MGP_FIELDS_MGPBlit` `:174-176`; `MGP_FIELDS_MGPClear` `:178-180`;
`MGP_FIELDS_MGPMipPlan` `:182-183`; `MGP_FIELDS_MGPReadbackInfo` `:185-186`; `MGP_FIELDS_MGPSurfaceInfo` `:223-224`.

`MGP_VERIFY_PAYLOAD_LIST` at `:315-331` is **what `gen_pipe.py` reads to know what to emit**; every payload above is
already in it. The tail of the file (`:226-311`) holds the *value* structs the comparator must see into
(`RenderStateParameters`, `PixelStoreParameters`, `PerBufferBlendState`, `StencilFaceState`,
`DynamicBackendParameters`, `MGHostSpan`) plus **P3a's two wire views** `MGP_FIELDS_MGPVertexAttribWire` `:306-308`
and `MGP_FIELDS_MGPVertexBindingPointWire` `:310-311`. **If P4a carries `SamplerParameters` as a typed value rather
than a blob, that is the pattern**: a `MGP_FIELDS_SamplerParameters` row plus a `P(SamplerParameters)` entry.

## 1.4 `MG_Pipe/Coverage.def` — which accessor each call answers, and the emitted-list discipline

Header `:9-25`: `gen_pipe.py` joins `scripts/data/backend_read_inventory.md` (477 rows, 57 files) against
`MGP_COVERAGE_ACCESSOR_LIST` and `MGP_COVERAGE_DELTA_LIST`, writes `generated/PipeCoverage.inc`. Three pseudo-calls
stand for read points that never become a forward call: `kClientResolved`, `kReverseChannel`, `kStructuralHandle`.
Unmapped rows are counted in P0 and must be **zero from P5 onward**.

**`MGP_COVERAGE_ACCESSOR_LIST`** (`:29-132`) — the P4a-relevant rows already present:

| accessor | call | line |
|---|---|---|
| `GetActiveTextureUnit` | `SetSamplerViews` | `:30` |
| `GetFramebufferBindingSlot` | `SetFramebufferState` | `:84` |
| `GetImageTextureBinding` | `SetShaderImages` | `:85` |
| `GetMaxTouchedTextureUnit` | `SetSamplerViews` | `:88` |
| `GetProgramForDispatch` | `SetDispatchProgram` | `:99` |
| `GetProgramForDraw` | `SetDrawProgram` | `:100` |
| `GetProgramObject` | `CreateShaderState` | `:101` |
| `GetSamplingResolutionGeneration` | `SetSamplerViews` | `:109` |
| `GetTextureBindGeneration` | `SetSamplerViews` | `:112` |
| `GetTextureContextId` | `SetSamplerViews` | `:113` |
| `GetTextureObject` | `SetSamplerViews` | `:114` |
| `GetTextureUnitObject` | `SetSamplerViews` | `:115` |

**No accessor row names `SetTextureParams`, `CreateSamplerState`, `CreateSamplerView`, `BindSamplerStates` or
`SetGlobalConstants`** — those five calls have no backend *read point* in the inventory, they carry object
descriptors. That is the shape P4a inherits, not a hole to fill by inventing accessors: `gen_pipe.py` refuses a name
that is not an accessor.

**`MGP_COVERAGE_STICKY_LIST`** `:144-151` — the seven F-class forwarded accessors, no per-verb generation.
Two are P4a's: `GetProgramObject` and `GetTextureObject`, both *"keyed by GL name: an object lookup, not verb
state"*. `:141-142`: the verify lane's `Fatal{UnmigratedPipeInput}` is fixed by a `FillPoints.def` row, **never** by
a row here.

**`MGP_COVERAGE_DELTA_LIST`** `:156-158` — read by `gen_pipe.py` only: `handle-ify (wire handle) →
kStructuralHandle`, `Buffer ops delta → ResourceRespecify`.

**`MGP_COVERAGE_EMITTED_LIST`** `:198-232` — the discipline P4a copies. The rules stated in the comment block
`:160-197`:
- every name must be an accessor in the accessor list and every call a real call in `PipeCalls.def`;
- `gen_pipe.py` turns it into `kMGPipeFieldEmittedBy[]` in `generated/PipeFilled.inc` — see
  `generated/PipeFilled.inc:306-314`, where the `MGPipeFieldEmitter` enum today is exactly
  `{kNone, BindRenderState, BindVertexElements, CreateRenderState, SetDynamicState, SetPatchState,
  SetVertexAttribDefaults}`. **Adding a row mints a new enumerator, which `PipeFill.cpp`'s `SubsystemForEmitter`
  switch must then handle** (that switch is exhaustive over the enum, `PipeFill.cpp:957-976`);
- a field with no row keeps going through the fill loop, "which is what makes the `MOBILEGL_PIPE_PUSH` bitmask a
  true per-subsystem A/B";
- `GetPixelStoreParameters` is **deliberately absent** (`:173-181`): the field is both halves of
  `m_pixelStore[2]` and `set_pixel_pack_state` carries pack only; a row would be a half-truth;
- **P3a added exactly one row**, `GetBoundVertexArray → BindVertexElements` (`:202`), and `:183-197` explains that a
  row may land **before** the call that carries it exists, because `kMGPipeWiredSubsystems` is the guard. The final
  paragraph (`:191-197`) is written *for the commit that wires it* and is the model P4a should copy: the row is
  shape-only until the backend's twin resolution reads the applier's state instead of the object, and
  **`PipeFill.cpp`'s `EmittedCallSuppliesTheWholeField` arm is where that is decided, deliberately rather than
  silently by the row's presence.**

## 1.5 `MG_Pipe/MGPipeHandles.h` — the kinds

`MGPipeKind` at `:24-40`: `None=0, Buffer=1, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso,
VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context, KindCount`.
**P4a's kinds are `Texture`, `Renderbuffer`, `Framebuffer`, `SamplerCso`, `SamplerViewCso`, `ShaderCso`.**

- `MGPipeHandle{Uint32 Slot; Uint32 Gen;}` `:54-61`, `sizeof == 8`, `alignof == 4` asserted `:63-65`.
  **`Gen` increments only when a slot is REUSED, never on a respecify** (`:44-48`).
- `kMGPipeNullHandle{0,0}` `:72`; **`kMGPipeDefaultFramebuffer{0,1}`** `:73` — kind `Framebuffer` slot 0 gen 1, which
  exists "so the four `pDefaultFramebufferInfo->defaultFBO` identity comparisons in DirectGLES retire into an
  ordinary handle compare" (`:69-71`). This is P4a's single most load-bearing reserved value.
- `MGPipeHandleIsNull` `:75-77`; `kMGPipeFirstAllocatableSlot = 1` `:81`.
- **The ShaderCso composite band** `:83-97`: `kMGPipeShaderCsoSlotLimit = 1<<20`,
  `kMGPipeShaderCsoCompositeSlotBase = limit - (limit >> 4)` (top 1/16), `MGPipeIsCompositeShaderSlot(slot)` `:92-94`,
  and a `static_assert` that the band cannot swallow the ordinary slots `:96-97`. A composite is minted client-side
  out of the stage programs bound to a pipeline object and **the server never learns it is a composite**.
  The allocator honours the band at `MG_Impl/Pipe/SlotAllocator.cpp:19-24` (`SlotIsAllocatable` refuses the band for
  kind `ShaderCso`) and asserts exhaustion at `:57-60`.

## 1.6 `MG_Pipe/PipeApply.{h,cpp}` after P3a

### `MGPipeResourceOps` (`PipeApply.h:74-91`)
```
struct MGPipeResourceOps {
    void  (*Create)        (MGPipeHandle res, const MGPResourceDesc& desc);
    void  (*Respecify)     (MGPipeHandle res, const MGPResourceDesc& desc, const void* initialBytes);
    void  (*SubData)       (MGPipeHandle res, const MGPSubData& record, const void* bytes);
    void  (*SubDataResident)(MGPipeHandle res, const MGPSubData& record, const void* bytes); // kOptional, may be null
    void  (*FlushRange)    (MGPipeHandle res, const MGPFlushRange& record, const void* bytes);
    void  (*Readback)      (MGPipeHandle res, const MGPReadback& record);
    void  (*Destroy)       (MGPipeHandle res);
    void* (*MapPersistent) (MGPipeHandle res, Uint64 size, const void* seedBytes);
    void  (*UnmapPersistent)(MGPipeHandle res);
};
void MGPipeSetResourceOps(const MGPipeResourceOps*);   // :90
const MGPipeResourceOps* MGPipeGetResourceOps();       // :91
```
`:52-56`: **the SECOND backend op table, beside `BufferBackendOps`**; a null table means "this backend has not taken
the resource family over" and the frontend dispatches the old way — *"which is what lets the client half land on its
own"*. `:58-61`: **no frontend type appears here** (gate G13 greps `PipeApply.h` for `MG_State`).
`:63-68`: the companion `const void*` is the client's own shadow base, zero-copy, and `resource_respecify`
deliberately does **not** carry `kHasBlob` because a `kHasBlob` record must own an `MGPBlobRef` and `MGPResourceDesc`
has none.
**P4a's texture/renderbuffer `resource_*` calls travel on this same table.** The signatures are already
handle+record+bytes and carry no buffer-specific type, so the table likely needs **no new members** for
create/respecify/subdata/destroy; what it lacks is any hook for `GenerateMipmap`, `ResourceCopyRegion`,
`GetTextureImage`, `Blit`, `Clear`, `SetTextureParams` and the sampler/program/framebuffer CSOs — those are new
tables or new members and that is a contract decision.

### The applier's records and state (`PipeApply.h:96-274`, `PipeApply.cpp`)
- Bounds: `kMGPipeMaxResourceSlots = 1<<20` and `kMGPipeMaxVertexElementsSlots = 1<<16` at `PipeApply.h:112-113`,
  with the argument at `:97-111`: *a slot at or above these is `Fatal{ProtocolCorruption}` — the same verdict as any
  other record that would make the server act outside its own storage — and never a resize.* The tables grow only to
  the client's own dense high-water mark. `RecordAt` implements it at `PipeApply.cpp:404-425` (returns null above the
  limit, `resize(slot+1)` otherwise).
- `MGPipeRenderStateCsoRecord` `:42-46` (Gen, Live, 396 pipeline bytes).
- `MGPipeResourceRecord` `:117-132`: `Uint32 Gen`, `Bool Live`, `MGPResourceDesc Desc` (the last create/respecify,
  verbatim — "the backend reads its Width / Usage / StorageFlags / HasDefinedContent instead of asking the frontend
  object"), `Uint64 Serial` (**server-owned MGGen-class, `++` on every mutation; no MGPipe call may require the
  client to provide or know one**), `Bool HasLiveHostWrites` — *always false in P3a, written by nobody*, pinned by a
  verify-only trip wire (`PipeApply.cpp:533-547`, `PinNoLiveHostWrites`).
- `MGPipeVertexElementsRecord` `:141-152`: Gen/Live/counts/two unpacked views/`Uint64 ContentSerial`.
- `MGPipeApplierState` `:154-274`. **The two halves and their different lives are the single most important P3a
  ruling for P4a** (`:196-216`): *object records* (`Vector<MGPipeResourceRecord> Resources` `:217`,
  `Vector<MGPipeVertexElementsRecord> VertexElementsCsos` `:218`) describe **share-group** objects and outlive a
  make-current; *working state* (`BoundVertexElements` `:236`, `VertexBuffers`/`Start`/`Count` `:240-242`,
  `VertexFetchBaseInstance` `:250`, `IndexBuffer` `:262`) is per-context and is cleared. **P4a's framebuffer/sampler/
  program/image bindings are working state; its texture/renderbuffer/sampler-CSO/program-CSO records are object
  records.**
- **Refusal counters** `:220-229`: `Uint64 RefusedResourceCalls`, `Uint64 RefusedVertexInputCalls`. The rationale at
  `:220-227` and again at `PipeApply.cpp:441-473`: `MOBILEGL_ASSERT` compiles out at INFO — which all three gate
  builds and every shipped build are — so *"a no-op nobody can see is a dropped call nobody can see"*; the counters
  are the observable in **every** build. The two resolvers that assert **and** count are `ResolveResource`
  `PipeApply.cpp:478-486` and `ResolveVertexElements` `:488-494`, with `kResourceRefusalNote` at `:475-476`
  (*"the record is not this applier's; the call is dropped, not applied"*). **P4a adds its own counter(s) in the
  same shape.** The one *legal* refusal sequence is documented at `PipeApply.cpp:452-462`: teardown →
  `MGPipeApplierReleaseObjectRecords()` → `~Object` → death notices naming records already dropped.
- Serials that **only advance, never zero**: `VertexBuffersSerial` `:251-258`, `IndexBufferSerial` `:263-264`, with
  the three-way argument spelled out at `PipeApply.cpp:651-665` (carry over → wrong immediately; restart at 0 → walks
  back through values already stamped into a twin that outlived the switch; advance → safe direction). Both
  `MGPipeApplierReset` (`PipeApply.cpp:664-665`) and `MGPipeApplierReleaseObjectRecords` (`:677-678`) `++` them.
- `MapPersistentRoundtrips` `:266-273` plus the long note at `PipeApply.cpp:636-650`: this member is **per applier and
  zeroed at every make-current**, while `PipeStats`' process-wide `mpr` (emitted from the client) is what the gates
  read through `MG_IntegrationTest/Harness/PipeStatsWindow.h`; nothing outside `MG_Test` reads the member.

### The three lifecycle functions
- `MGPipeApplier()` `PipeApply.h:277` / `PipeApply.cpp:596`.
- **`MGPipeApplierReset()`** `PipeApply.h:279-291`, body `PipeApply.cpp:601-666`: **a make-current, not a teardown**;
  runs on every change of the current `GLContext`; drops render-state CSOs, the residual mirror and the vertex-input
  working state; **does NOT drop the object records** (`PipeApply.cpp:617-627`: dropping them "made every
  `glBufferSubData` after a context switch resolve to nothing and be dropped, with the only trace an assertion that
  compiles out at INFO"); **does not clear the op table** (`:626-627`).
- **`MGPipeApplierReleaseObjectRecords()`** `PipeApply.h:293-300`, body `PipeApply.cpp:667-678`: the served context's
  teardown; **deliberately wired to NOTHING in the monolith** — one applier behind every context, so calling it on
  one context's destruction would drop every other context's records. A record is cleared by its object's own death
  signal. The only caller in the tree is the unit fixture `ApplierGuard`
  (`MG_Test/Pipe/ResourceEmitTest.cpp:146-157`), whose comment says a test fixture *"is the one caller in the tree
  that legitimately means 'this applier is going away'"*.

### The never-destroyed singletons (ID-18, ID-21)
- `MGPipeApplierState& g_applier = *new MGPipeApplierState{};` at `PipeApply.cpp:384`, with the reason at `:380-383`:
  destroy notices are raised from `~BufferObject` / `~VertexArrayObject`, released by exit handlers that run *after*
  this TU's globals are gone.
- `MGPipeResourceTrackerInstance()` — `new`, never deleted, `MG_Impl/Pipe/ResourceTracker.h:554-560`.
- `MGPipeVertexInputEmitterInstance()` — `MG_Impl/Pipe/VertexInputEmit.h:428-440`, with the concrete UAF it closes.
- `MGPipeSetHashSuppressorInstance()` — `MG_Impl/Pipe/SetHashSuppressor.h:88-94`; `MGPipeCsoCacheInstance()` —
  `MG_Impl/Pipe/CsoCache.h:196-202`; `MGPipeSlots()` — `MG_Impl/Pipe/SlotAllocator.cpp` (the rule's origin, cited by
  every other one). **The rule stated in the comments is "every MGPipe process singleton", not only the ones on
  today's death paths** — P4a's new singletons (a texture tracker, an FBO emitter) inherit it verbatim.
- The other half of the closure is ID-21's `6515c8e6`: leak-at-exit storage for the four static `PipeInputs` holders.
  Proof recipe: `GLIBC_TUNABLES=glibc.malloc.tcache_count=0` on both lanes (ID-18, ID-20, ID-21).

### The 23 apply entry points, with exact line numbers
`PipeApply.h:309-329` declares the seven P2 ones; bodies `PipeApply.cpp:680, 730, 749, 763, 772, 776, 834, 880`.
P3a's nine resource entry points: declared `PipeApply.h:347-380`, bodies at `PipeApply.cpp:951`
(`ResourceCreate`), `:984` (`ResourceRespecify`), `:1010` (`ResourceSubData`), `:1016` (`BufferSubDataResident`),
`:1022` (`ResourceFlushRange`), `:1051` (`ResourceReadback`), `:1088` (`ResourceDestroy`), `:1113` (`MapPersistent`),
`:1142` (`UnmapPersistent`); the section header at `:931-949` is worth copying verbatim as a template — *"what the
applier owns here is IDENTITY, EXTENT AND ORDER — NOT CONTENT"*, and each body does three things in this order:
resolve the handle against the record, check the record against its own declared extent, then move the record and
hand the call to the backend.
P3a's five vertex-input entry points: declared `PipeApply.h:386-404`, bodies `PipeApply.cpp:1173, 1263, 1279, 1303,
1349`; the section header `:1157-1171` says **these five dispatch to nobody** — the backend reads them at its own
draw-time sync out of the applier's working state, which is what the serials are for.
The derivation step `MGPipeDeriveRenderStateFields` / `...ForChunks` is `PipeApply.h:420` / `:431`.

Body-level idioms P4a should copy:
- **A create starts the record over rather than editing it** — `PipeApply.cpp:960-967`: a recycled slot's record must
  not contribute one field. Generation is taken from the handle and nothing else survives. `Serial` stays 0 (a create
  is not a mutation) so a fresh backend twin starting at 0 agrees without either side publishing anything (`:968-171`).
- **Serial moves BEFORE the backend is told** — `ApplyBufferWrite`, `PipeApply.cpp:578-582`: the backend stamps its
  own synced serial from inside the hook, so a bump afterwards leaves the twin one mutation behind.
- **A readback moves no serial** — `PipeApply.cpp:1062-1075`, and the writeback precedes the epoch bump.
- **Destroy drops the record whole and keeps the generation** — `:1092-1095`; the *client* frees the slot afterwards,
  in that order.
- **A re-create on the same handle keeps the serial and counts up; a different identity starts over** —
  `PipeApply.cpp:1200-1204`; and it **does not rebind** (`:1223-1226`).
- **A dead handle leaves the previous binding untouched** — `MGPipeApplyBindVertexElements`, `:1263-1278`;
  the null handle is legal and means "nothing bound".
- **The var-tail window is the bound**: `Start + Count > kMGPipeMaxVertexAttribs` is `Fatal{ProtocolCorruption}`,
  and entries outside the declared window are **not** cleared — "this record is 'the last set as received'"
  (`PipeApply.cpp:1303-1324`). Every P4a `kVarTail` set inherits this.

## 1.7 `MG_Pipe/MGPipeCallbacks.h` — the reverse channel

Ten callbacks in `struct MGPipeCallbacks` (`:27-51`), count asserted at `:55-57` (`kMGPipeCallbackCount = 10`,
`sizeof == 10 * sizeof(void(*)())`) — *"an eleventh cannot be added without touching the transport's
reverse-channel record table"*. `inline MGPipeCallbacks gMGPipeCallbacks{}` at `:60`, null-initialised: a backend
that installs nothing sends nothing.

The six that are P4a's business: `OnGlError` `:29`; `OnTextureWriteback(res, box, bytes)` `:33`;
**`OnTexturePullRequest(res, target, firstLevel, levelCount, pullSerial)`** `:37-38` — "the one new stall class in
this design (D-B6): the server recast a texture and needs its texels back. The client answers with zero or more
`ResourceSubData` records terminated by `ResourceSubDataComplete` carrying the same `pullSerial`";
`OnMipLevelsGenerated(res, base, count)` `:41` — **shape only, never bytes**; `OnSurfaceChanged(info)` `:44` —
"retires the layering inversion where the swapchain writes into MG_Impl's `pDefaultFramebufferInfo`";
`OnCapsInvalidated()` `:45`. `:24-25`: their **order is a correctness requirement**, not an optimisation.

The client's half of the channel is installed additively and never over an entry a backend already claimed —
`MGPipeInstallClientResourceCallbacks()` at `ResourceTracker.h:623-630` (`if (... == nullptr) ... = &Client...`).

---

# PART 2 — The P3a client conventions to copy

## 2.1 `MG_Impl/Pipe/ResourceTracker.h` (632 lines, header-only, all inside `#if MOBILEGL_PIPE_PUSH`)

The file header `:12-40` states the shape: where it runs (**the ONE exception to push-at-validate** — the seven
`BufferBackendOps` hooks already dispatch at the GL call, so their pipe calls are emitted from the same
`BufferObject` dispatchers, not from `MGPipeValidateForVerb`); what lives here; **header-only for the ownership
reason** (the root `CMakeLists.txt` that would name a new `.cpp` belongs to the contract package); and **no timer and
no per-call record copy on a hot path** — the observables are written only by create/respecify, and the hot call is
observed through pure builders instead.

- **The BindMask table** `:60-149`. `enum MGPipeBindBit : Uint16` `:67-86` gives `MGPResourceDesc::BindMask`'s twelve
  bits, `kMGPipeBindElementArray = 1<<11` being the D-B7 switch. `kMGPipeBindUnmapped = 0x10000u` `:92` is a
  sentinel that is *not* a legal mask value; `MGPipeBindMaskForBufferTarget` `:96-134` has **no `default:` arm on
  purpose**, and `MGPipeEveryBufferTargetIsMapped()` + `static_assert` `:136-149` make adding a `BufferTarget` a
  build break. **P4a inherits `kMGPipeBindSampler` (1<<5), `kMGPipeBindShaderImage` (1<<6),
  `kMGPipeBindRenderTarget` (1<<7), `kMGPipeBindDepthStencil` (1<<8)`, which nothing sets today**, and
  `:64-66` says explicitly: *"They are spelled HERE rather than in MGPipeTypes.h because that header is the contract
  package's and the mask has, so far, exactly one producer: this file. The integrator moves them beside the field
  when a second producer appears (P4a's texture family)."* **That move is a P4a contract action.**
- **Payload builders are pure** `:164-235`, so a unit case can assert field by field.
  `MGPipeBuildResourceDesc(buffer, handle, bindMask, storageDefined)` `:171-205`, including the narrowing alarm at
  `:180-194` (a store ≥ 4 GiB cannot be declared, and truncating silently would make the applier's range gate name a
  corruption that is really a narrowing here). `MGPipeBuildSubDataRecord` `:225-235` fills `Blob.Seg =
  kMGHostSpanSegNone` and `Blob.Size = size` — *"Leaving it 0 would be legal too; declaring it correctly is the
  stronger of the two."*
- **The range splitter** `:237-276`: `kMGPipeSubDataMaxRecordOffset = 0x7FFFFFFF`, `kMGPipeSubDataMaxRecordSize =
  0xFFFFFFFF`, `MGPipeForEachSubDataRecordRange(offset, size, piece, maxChunk)`. Two properties are load-bearing:
  **contiguous and ascending** (`:238-242`) — splitting into overlapping or reordered pieces changes what the
  backend's queue-and-drain sees and the Mali WAR-stall fix depends on that queue being exactly the writes the
  application made. **Every piece is proved encodable before the first one is emitted** (`:263-268`): a half-emitted
  range is a partial content write landed as if it were whole. `maxChunk` is a parameter *"exercised at a reachable
  value by the unit gate, so that lowering it is one argument rather than a new code path written under pressure"*.
- **`class MGPipeResourceTracker`** `:282-550`:
  `Acquire(buffer)` `:292-299` — **minting is NOT gated on a backend having registered `MGPipeResourceOps`**: the
  handle is client state and `set_vertex_buffers` names it whether or not the resource family is on; only the CALLS
  are gated. `Find` `:303-305` never mints. `Resolve(handle)` `:313-320` is the reverse-channel inverse: a **raw**
  pointer is exact (a `WeakPtr` would be wrong at construction/destruction), and it double-checks
  `MGPipeSlots().GenOfSlot(...) == handle.Gen`. `Retire(handle)` `:325-329` — **the caller frees the slot
  afterwards, in that order**, because `Free` erases the lifetimeId→slot mapping.
  **`NotePublished` / `WasPublished`** `:331-350` — the create/destroy **latch**: the create is gated at its call site
  and the destroy inside the emitter, so the two ask the same question at two different moments; a buffer created
  while a table was registered and destroyed after `UnregisterBufferBackendOps()` would take the second answer, free
  its slot, and leave the applier's record `Live` on a slot about to be re-handed-out. **P4a needs one latch per
  kind it mints.**
  `BindMask` `:355-358` (sticky, ORed, never cleared); `NoteBoundAs(handle, target)` `:369-374` — closes the sampling
  window from the *draw* side; `RefreshBindMask(ctx, buffer, handle)` `:407-445` — samples the frontend's live
  binding state at create/respecify, with the **recorded deviation** at `:379-399` (the brief pointed at
  `BufferState.{h,cpp}` but the `.Bind()` calls are in `GL_Buffer.cpp`, which C.5 assigns to nobody, so the mask is
  sampled instead of hooked) and the honest statement of the residual hole. The `BindEpoch` shutter `:522-534` and
  its stated blind spot `:516-521` (it does not see the 84×4 indexed binding points, sound only because
  `BindBufferBase/Range_State` also bind the generic slot). **All of that is the template for a texture bind mask.**
  Observables for unit cases `:447-463`: `LastDesc()`, `CreateCount()`, `RespecifyCount()`, `DestroyCount()`,
  `MapPersistentCount()`, `NoteDesc(desc, isCreate)`, `NoteDestroy()`, `NoteMapPersistent()`.
  **`ResetForTest()` and its 20-line rule** `:465-498`: *"A buffer handle and the applier record it names are
  SHARE-GROUP OBJECT STATE… this tracker needs no re-publication path on a fresh context and must not have one:
  re-emitting `resource_create` for a record the applier still holds would move its `Serial` for nothing."* This is
  ID-12's B-C2 resolved, and P4a must state the same rule for its own trackers rather than leaving absence as policy.
- **The client's half of the reverse channel** `:562-630`: `MGPipeClientOnBufferWriteback` `:570-589` (monolith: `Seg`
  is `kMGHostSpanSegNone` and `Offset` **is** the address of the backend's mapped bytes) and
  `MGPipeClientOnGpuWritten` `:596-618`, whose shape is a **contract point**: *one range covering
  `kMGPipeWholeBuffer`, deliberately not zero ranges*, because zero will mean "fully narrowed" at P8/P9. The
  backend's producer side is `MG_Backend/DirectGLES/Managers.cpp:2491-2521`, which also records the other half of
  the contract (the client's `OnGpuWritten` must set **both** `m_hasDefinedContent` and `m_gpuWritePending`, or the
  next respecify orphans and drops shader-written contents).

## 2.2 `MG_Impl/Pipe/VertexInputEmit.h` (444 lines)

Header `:11-38`: these three emit at the **validate point**, from `MGPipeValidateForVerb`'s step 3, in the fixed
order **elements → buffers → index**; the CSO is **identity-addressed, not content-addressed** (D-G1, a recorded
deviation), one handle per frontend VAO minted off its lifetime id, `create_vertex_elements` **re-issued on the same
handle** whenever the configuration moves. It also states what the unit gate reads and why the conversion is a pure
function per field (that is what makes G7 able to name a dropped field).

- **Wire conversion** `:59-116`: `MGPipeBuildVertexAttribWire(attrib, bindingIndex)` `:82-107` and
  `MGPipeBuildVertexBindingPointWire(point)` `:111-116`. The comment `:60-81` enumerates, field by field, what the
  wire form carries and **what it deliberately omits and why** (Divisor travels in `MGPVertexBuffer`; `LegacyStride`/
  `LegacyPointer` are query answers and stay client-side; `Buffer` identity travels in `set_vertex_buffers`; a
  surviving `Stride == 0` is meaningful; `IsLong` is separate from `Type == Float64`). It asserts rather than assumes
  across the three narrowing casts `:87-93`. **This is the exact form a `MGPSamplerParametersWire` or a
  texture-descriptor conversion should take.**
- **ContentHash** `:118-133`: `MGPipeVertexBufferSetContentHash(entries, start, count, baseInstance)` — `XXH64` over
  the entries, then `MGPipeMixShutter` of `start`, `count` and `baseInstance`. The comment `:118-125` states the hard
  requirement: **the hash must cover every input the record carries**, because the set is suppressed on an unchanged
  hash. P4a's `MGPFramebufferState`, `MGPSamplerViews`, `MGPSamplerStates` and `MGPShaderImages` all carry a
  `ContentHash`; the same rule applies to each.
- **`class MGPipeVertexInputEmitter`** `:135-425`:
  `kAttribs`/`kBindings` with a `static_assert` against `kMGPipeMaxVertexAttribs` `:139-143`.
  **`EmitVertexElements(ctx)`** `:158-193` implements the three arms verbatim (`:145-156`): no VAO → bind the null
  handle; the bound VAO changed → (re)create if its configuration moved *since this handle last published one*, then
  bind; same VAO, configuration moved → create on the same handle and **do not rebind**. The latch is **per handle,
  in a slot-indexed table**, so ping-ponging between two VAOs re-binds but never re-creates.
  **`EmitVertexBuffers(ctx, baseInstance)`** `:195-248`: one entry per attribute slot, `Start = 0`,
  `Count` = highest enabled attribute + 1; a client-memory array is `Res == kMGPipeNullHandle` and that is *how the
  server learns the attribute is client-sourced*; it calls `NoteBoundAs(entry.Res, BufferTarget::Vertex)` at every
  draw (`:216-222`, the bit that survives the DSA idiom); it goes through
  `MGPipeSetHashSuppressorInstance().ShouldEmit(MGPipeSuppressorSlot::SetVertexBuffers, hash)` `:230-233` and
  **returns 0 bytes when suppressed**.
  **`EmitIndexBuffer(ctx)`** `:250-273`: emitted unconditionally (no suppressor slot), `Offset`/`IndexSize` left 0
  because there is no draw at the validate point.
  Unit observables `:275-290`, **none of which costs a copy** — the emitter builds into the same arrays it hands the
  applier.
  **`RecordIsPublished(handle)`** `:292-311` and **`NoteRecordDestroyed(handle)`** `:313-327` — the **record half** of
  the latch, deliberately kept OUT of `Reset()` because object records are precisely what `MGPipeApplierReset` does
  not clear. The comment `:293-303` records the trap: *a slot can exist with no record behind it*, because a backend
  that keys its twins on the handle mints the slot itself (`BackendSlotTable::GetOrCreate → MGPipeSlots().Acquire`)
  whether or not the subsystem ever asked the client to emit — **which is exactly what a `MOBILEGL_PIPE_PUSH=0x7f`
  lane runs**. `delete_*` on such a handle is a refused call and the resolver asserts. **Every P4a kind whose slot a
  backend twin table mints has this same trap.**
  **`Reset()`** `:329-343` — the per-context half only (`Published`, `Gen`, `ConfigVersion`, the bound handle);
  re-creating an unchanged configuration is a bounded over-fire, forgetting that the applier holds one would leak the
  record and its slot at the object's death.
  `EmitCreate` `:359-406`: **all 32 of each, deliberately** (the record declares both counts and the applier refuses
  one whose counts do not describe its blob; G6 is stated over all 32 slots); blob is attributes-then-binding-points,
  ascending and contiguous; `Blob.Seg = kMGHostSpanSegNone`, `Blob.Offset = 0`, `Blob.Size = kAttribBytes +
  kBindingBytes`; sets `latch.RecordLive/RecordGen` — **the one producer of the record half** (`:400-403`).
  Storage `:408-424` is fixed-size `Array`s, one blob buffer, one entries buffer; no allocation per draw.

## 2.3 `MG_Impl/Pipe/PipeFill.cpp` — the validate point and its discipline

- **The resource emitters** (the exception to push-at-validate) `:602-798`:
  `MGPipeResourceSubsystemEnabled()` `:602-605` = `(Features.PipePush & kMGPipeSubsystemResources) != 0 &&
  MGPipeGetResourceOps() != nullptr` — **both halves matter** (`PipeMutation.h:100-105`).
  `MGPipeResourceOpsHaveSubDataResident()` `:607-610`. `MGPipeMintResourceHandle` `:612-624` — **unconditional in a
  push build**. `MGPipeEmitResourceCreate` `:622-637`. `MGPipeEmitResourceRespecify` `:638-683`, including the
  **self-healing create** at `:648-673` (the create's gate and the consumer's gate disagree across a
  register/unregister boundary; a buffer born in that window latched `Published = false`, so every later respecify
  was refused and the twin drew through id 0 with no diagnostic anywhere). `MGPipeEmitResourceSubData` `:684-704`
  drives the splitter. `MGPipeEmitMapPersistent` `:762-775` is the `mpr` counting site.
  **`MGPipeEmitResourceDestroyAndFree`** `:777-799` and **`MGPipeEmitVertexElementsDestroyAndFree`** `:801-849` —
  the two death paths, both returning "did the call go out"; the second has the three-position ordering argument
  (`:836-848`): applier record dropped **first**, the backend death notice raised **second** (it resolves the handle
  through the allocator, and a backend told after the `Free` could no longer find its twin), the slot goes back
  **last**, and the double free is a proven no-op.
  Declarations are in `MG_Pipe/PipeMutation.h:106-153`, deliberately so `MG_State` sees a declaration and never the
  client's tracker (`PipeMutation.h:79-92`; the closure gate's mutation-header probe enforces it).
- **Subsystem mapping and the pairing asserts** `:955-1038`: `SubsystemForEmitter(MGPipeFieldEmitter)` `:957-976`
  (exhaustive switch over the generated enum); five `static_assert`s pairing it against
  `MGPipeSubsystemForDirty` `:986-1002`; **P3a's three** at `:1004-1030`, with the explicit lesson (`:1016-1023`):
  *all three compare against `SubsystemForEmitter`, not against the constant* — pinning the dirty half to a constant
  asks a weaker question and would let an emitter row move onto another subsystem unnoticed. The escape hatch that
  existed at the contract commit (`MGPipeSubsystemForDirty(...) == 0 ||`) **was removed once the dirty half was
  mapped** (`:1009-1015`) — P4a will need the same hatch, and must remove it in the same commit that maps its bits.
  The vertex-attrib capacity pin is `:1032-1038`.
- **`kMGPipeWiredSubsystems`** `:1071-1076` = RenderState | PixelPack | PatchState | VertexAttribDefaults |
  **Resources | VertexInput**. The comment `:1040-1070` is the model: *"a field whose emitter is not wired here keeps
  being pulled - so adding a row to `Coverage.def` can never silently drop a field on the floor before the call that
  carries it exists"*; each bit is added **by the commit that gives its own emitters their bodies**; and it states
  which bits retire a pull (**neither of P3a's does**). It also records the one-directional A/B trap (`:1062-1069`):
  **bit 8 without bit 7 sends the server handles it cannot resolve and every one of those lands in
  `RefusedResourceCalls`; bit 7 without bit 8 is fine.** Neither `0x1ff` nor `0x7f` is in that arm.
- **`EmittedCallSuppliesTheWholeField(field)`** `:1116-1124`, returning **false** for
  `GetPixelStoreParameters`, `GetCurrentVertexAttribute`, `GetBoundVertexArray`. The 60-line argument `:1078-1115`
  is the one to reread when P4a decides its own rows — in particular the `GetBoundVertexArray` paragraph
  (`:1094-1115`): the field's storage is a `SharedPtr<VertexArrayObject>`, the call carries an 8-byte handle, so the
  row is **emitted-and-still-pulled**; *"what retires the pull is not a better applier - it is P8"*. **Every P4a
  candidate (`GetTextureObject`, `GetProgramObject`, `GetFramebufferBindingSlot`, `GetImageTextureBinding`) has the
  same pointer-storage problem** and will land in the same arm unless P4a also changes what the backend reads.
  `AppliedWithoutDerivation(field)` follows at `:1126+`.
- **The validate point** `MGPipeValidateForVerb(verb)` `:1505-1687`. Step order: knob parse / verify arm / poison
  serial `:1507-1521`; set verb + identity `:1522-1524`; **the no-context early return that still clears the pending
  base instance** `:1525-1535`; step 2 the dirty walk `:1541-1543`; step 3 the emission gate
  `const auto wants = [&](MGPipeDirty bit){ subsystem != 0 && (pushMask & subsystem) && (dirty & bit) }` `:1550-1555`
  — *"Every gate below goes through `MGPipeSubsystemForDirty`, the ONE map"*; **the `FreshlyPrimed` arm** `:1570-1585`
  (`CsoCache.Reset()`, `MGPipeApplierReset()`, `SetHashSuppressor.InvalidateAll()`,
  `VertexInputEmitter.Reset()`, `g_residualDue = true`) with the P3a paragraph `:1577-1584` explaining that the
  emitter's reset is needed **not** because the applier dropped its records but because the emitter's bound-handle
  latch mirrors `BoundVertexElements`, which `MGPipeApplierReset` does clear, and that the **resource tracker is
  deliberately NOT reset here**; the four P2 emitters `:1587-1598`; **P3a's vertex segment in the fixed order**
  `:1600-1616`; `tracker.ClearPendingBaseInstance()` `:1620`; step 4 the residual fill `:1623-1655` (with the
  `supplied` predicate at `:1643-1647` = subsystem non-zero **and** wired **and** in the push mask **and**
  `EmittedCallSuppliesTheWholeField` **and** (`applierDerives || AppliedWithoutDerivation`)); step 4b the residual
  value block `:1656-1678`; `PipeStats::RecordDrawPayloadBytes` `:1679-1686`; the verify `EntryCompare` `:1683-1685`.
- **The base-instance macro** `MG_Impl/Pipe/PipeFill.h:114-123`: `MGP_FILL(Verb)` and
  `MGP_SET_BASE_INSTANCE(BaseInstance)`, the latter a macro **because the three call sites compile in the pull build
  too**, where `MGPipeSetPendingBaseInstance` is neither declared nor defined. Declaration + the rules at
  `PipeFill.h:46-73`. The three sites are `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp:662, 685, 715` (ID-10's grant),
  one line each immediately above the `MGP_FILL`. Setter/getter/clear on the tracker:
  `Tracker.h:453-455`, member `:477`.

## 2.4 `MG_Impl/Pipe/Tracker.h` — the dirty bits, and which are P4a's

`enum class MGPipeDirty : Uint32` at `:57-84`, **the comment table is the roadmap**:

| bit | name | line | comment says | subsystem today |
|---|---|---|---|---|
| 0 | `NewRenderState` | `:60` | `RenderState::m_version` → `set_dynamic_state` | RenderState |
| 1 | `NewPipelineState` | `:61` | → `create/bind_render_state` | RenderState |
| 2 | `NewPixelPack` | `:62` | → `set_pixel_pack_state` | PixelPack |
| 3 | `NewPatchState` | `:63` | → `set_patch_state` | PatchState |
| 4 | `NewVertexAttribDefaults` | `:64` | → `set_vertex_attrib_defaults` | VertexAttribDefaults |
| 5 | `NewVertexElements` | `:67` | → `create/bind_vertex_elements` | VertexInput (P3a) |
| 6 | **`NewShader`** | `:67` | the current program's link version | **0 — P4a** |
| 7 | **`NewShaderBindings`** | `:68` | image units, block bindings, uniform write set | **0 — P4a/P4b** |
| 8 | **`NewGlobalConstants`** | `:69` | the default-uniform-block image | **0 — P4a** |
| 9 | `NewVertexBuffers` | `:75` | → `set_vertex_buffers` (P3a) | VertexInput |
| 10 | `NewIndexBuffer` | `:76` | → `set_index_buffer` (P3a) | VertexInput |
| 11 | **`NewFramebuffer`** | `:76` | — | **0 — P4a** |
| 12 | **`NewSamplerViews`** | `:77` | — | **0 — P4a** |
| 13 | **`NewSamplers`** | `:78` | — | **0 — P4a** |
| 14 | **`NewShaderImages`** | `:79` | — | **0 — P4a** |
| 15 | `NewConstBuffers` | `:80` | — | 0 — P4b |
| 16 | `NewShaderBuffers` | `:81` | — | 0 — P4b |
| 17 | `NewSoTargets` | `:82` | — | 0 — P4b |

(The line-number collisions above are because two enumerators share a comment line; the enum order is exactly as
listed.) `kMGPipeDirtyCount <= 32` asserted `:87`. The file header `:22-26` says P2 emitted for bits 0..4, **P3a adds
5, 9 and 10**, "the rest are still computed, latched and counted so the per-bit fire rate is a measurement rather
than a plan, with their fields going through the residual fill until P3b/P4a/P4b". **So P4a's fire rates already
exist as a measurement** — `FireCount(bit)` / `FireCount(bit, verbClass)` / `WalkCount()` at `:395-410`.

Phase constants that are never edited in place (`:93-104`): `kMGPipeDirtyEmittedAtP2` `:96-99`,
`kMGPipeDirtyEmittedAtP3a` `:102-104` (= P2 | 5 | 9 | 10). **P4a adds `kMGPipeDirtyEmittedAtP4a` beside them.**
`kMGPipeDirtyNames[]` `:106-127`. `MGPipeSubsystemForDirty(bit)` `:130-155` — P3a's three arms at `:145-149`, and
`default: return 0` at `:150-154` for "no call of its own until P3b/P4a/P4b".

**The shutters P4a will narrow** — all already computed in `Update()`:
- `NewShader` `:249-262`: `MixShutter(program->GetLifetimeId(), program->GetLinkVersion())`, deliberately **not**
  `GetProgramForDraw` (which joins a pending link; the tracker must not force a compile to answer "did the shader
  move").
- `NewShaderBindings` `:253-258`: a four-way mix of `GetImageUnitVersion`, `GetBackendStateVersion`,
  `GetBlockBindingVersion`, `GetUniformWriteSetVersion`.
- `NewGlobalConstants` `:259`: `MixShutter(lifetimeId, GetUBOContentVersion())`.
- `NewFramebuffer` `:303-306`: `MixShutter(GetAnyFramebufferAttachmentGeneration(),
  m_framebufferBind.Observe(GetFramebufferBindingSlot(Draw).GetVersion()))`.
- `NewSamplerViews` `:307-308`: `MixShutter(textureContent, GetTextureBindGeneration())`.
- `NewSamplers` `:309-310`: `MixShutter(textureParams, GetSamplingResolutionGeneration())`.
- `NewShaderImages` `:311-312`: `MixShutter(MixShutter(textureContent, textureParams), programImages)`.

The composite-collision argument `:157-171` (each mix is 64→64, ~2^-64 per pair, inputs an application cannot steer)
and the **over-fire-is-free / under-fire-is-fatal** rule `:28-35` are what P4a's narrowing arguments must be written
against. `MGPipeWidenedCounter` `:181-205` is how a wrapping `Uint16` crosses the tracker boundary
(**never widened in `MG_State`**), with its one stated blind spot (`:187-192`: an advance of exactly 65536 between
walks reads as unchanged).
`Reset()` `:365-393` — clears every latch, **deliberately not `m_pendingBaseInstance`** with the 12-line reason.

## 2.5 `MG_Impl/Pipe/SetHashSuppressor.h` — the seven slots

`enum class MGPipeSuppressorSlot : Uint32` `:44-53`:
`SetVertexBuffers = 0` (**P3a — wired, and its hash includes `BaseInstance`**), `SetSamplerViews` (**P3b**),
`BindSamplerStates` (**P3b**), `SetShaderImages` (**P4b**), `SetShaderBuffers` (**P4b**),
`SetStreamOutputTargets` (**P4b**), `SetVertexAttribDefaults` (**P2 — wired**), `Count`.

**Note the phase labels in the enum disagree with `ROADMAP.md:20`**, which puts `set_sampler_views`,
`bind_sampler_states` **and** `set_shader_images` in P4a. Three of P4a's four `kVarTail` sets therefore already have
slots (2, 3, 5 by index) and **`SetFramebufferState` has none** — it is not `kVarTail`, but it does carry a
`ContentHash` whose second job `MGPipeTypes.h:369-373` states is exactly the client's emission suppressor. **P4a must
either add a `SetFramebufferState` slot or suppress it another way, and update the phase labels it touches.**

Mechanics: `ShouldEmit(slot, contentHash)` `:61-67` — **hash 0 is reserved for "never emitted"** and a computed 0 is
remapped to 1 (`:31-33`: one collision in 2^64 costs an extra emission and never a missed one);
`Invalidate(slot)` `:71`; `InvalidateAll()` `:73-75` (called from the `FreshlyPrimed` arm);
`LastEmitted(slot)` `:79-81` exposed for the unit test that pins the reserved value. The file header `:12-33` is the
whole rule, including that this is the carrier for the ~175 lines of debounce that move off the backends
(Espryt's `UnitBindingsSnapshot` / `CaptureUnitBindings` / `UnitBindingsUnchanged`) **in P3b and P4b**.

## 2.6 `MG_Pipe/DirtySurface.def` — the rows P4a's mutators land in

The mechanism (`:9-27`, `:150-155`): `scripts/gen_pipe_dirty_surface.py` scans **`MG_Impl/GLImpl` AND
`MG_State/GLState`** (the second root is P3a's widening) for every `pGLContext->` mutator call and every
`MGP_NOTE_MUTATION` site, and `--check` fails **both directions**. Answer vocabulary at `:103-149`:
`NEW_*`, `kImmediate`, `kReverseChannel`, `kNoBackendRead`, `kExplicitDestroy`, `kUnpublishedDestroy`,
`kPulledEveryVerb`, `kPulledPartialShutter`. The rule at `:29-45`: **a row lists every publisher that fires on
EVERY path that mutates**, because a shutter built from this file would otherwise under-fire; a redundant-write
guard does not make a publisher conditional, but a publisher reached on only *some* mutating paths must not be named.
The derivation is checked mechanically for every `NEW_*` answer (`:46-60`) with `MGP_DIRTY_SURFACE_UNDECIDED_LIST`
(`:326-330`, empty today) as the only escape, and a mark that outlives its reason is itself a red gate.

Rows P4a owns or will touch:
- `X(SetActiveTextureUnit, kImmediate)` `:164`.
- `X(BumpTextureBindGeneration, NEW_SAMPLER_VIEWS)` `:248`.
- **`X(BumpSamplingResolutionGeneration, NEW_SAMPLERS)`** `:257` — a P3a addition found only by the widened scan root
  (`:249-256`).
- **`X(NoteUnitTouched, kPulledPartialShutter|NEW_SAMPLER_VIEWS)`** `:270` — the other P3a addition, and the reason
  `MGP_NOTE_MUTATION` became a recognised publish mechanism (`:258-269`): every texture and sampler bind routes
  through it, it moves two pushed fields with two different answers (`GetTextureBindGeneration` moves only on the
  `bindingChanged` arm; `GetMaxTouchedTextureUnit` has **no shutter at all** and is copied at every verb).
- The destroy family `:288-297` / `:296-306`: `MarkTextureObjectForDeletion`, `MarkFramebufferObjectForDeletion`,
  `MarkRenderbufferObjectForDeletion`, `MarkSamplerObjectForDeletion`, `MarkVertexArrayForDeletion`,
  `MarkBufferObjectForDeletion` are `kExplicitDestroy`; **`MarkProgramForDeletion`,
  `MarkProgramPipelineForDeletion`, `MarkShaderForDeletion` are `kUnpublishedDestroy`** `:301-305`, i.e. a **known
  hole**: "programs, program pipelines and shaders have no per-object handle on the wire at all in P2". **P4a's
  shader CSO is what closes that hole, and those three rows are what must change.**
- `X(SetNamedTransformFeedbackBinding, kPulledEveryVerb)` `:284` — with the note that narrowing is P3b's.

## 2.7 Subsystem bits — `MG_Pipe/MGPipe.h:66-95`

```
bit 0 kMGPipeSubsystemRenderState            :72
bit 1 kMGPipeSubsystemPixelPack              :73
bit 2 kMGPipeSubsystemPatchState             :74
bit 3 kMGPipeSubsystemVertexAttribDefaults   :75
bit 4 kMGPipeSubsystemResidualValues         :76
bit 5 kMGPipeSubsystemEsprytSlots            :77   (Track H, Espryt 0b)
bit 6 kMGPipeSubsystemMagmaVertexInput       :78   (Track H, Magma subsystem 4)
bit 7 kMGPipeSubsystemResources              :84   (P3a)
bit 8 kMGPipeSubsystemVertexInput            :85   (P3a)
// bits 9..62 reserved for the later phases, allocated in ROADMAP order   :86
bit 63 kMGPipeBehaviourNoCsoContentAddressing :90  (a BEHAVIOUR, not a subsystem)
kMGPipeSubsystemsMigratedAtP2  = 0x7f    :94
kMGPipeSubsystemsMigratedAtP3a = 0x1ff   :95
```
`:66-70`: **bits are allocated in ROADMAP order and never reused — "an operator's recorded `0x7f` has to keep
meaning what it meant"**; a clear subsystem bit means "keep pulling", valid only while
`MOBILEGL_PIPE_LEGACY_MEMOS` compiles the pre-handle arm beside it (`ARCHITECTURE.md:367` keeps that arm through
**P3a/P4a**). `:80-83` justifies why P3a took **two** bits rather than one: separate A/Bs, "a buffer path that
regressed and a vertex path that regressed are different findings".

**P4a therefore takes bits 9 and upward, in ROADMAP order, and adds
`kMGPipeSubsystemsMigratedAtP4a` beside `:95` without editing `0x7f` or `0x1ff`.** ROADMAP's P4a row names five
groupings (framebuffer state; sampler CSO; sampler view + texture params; the three unit-array sets; shader CSO +
draw/dispatch program + global constants + CompositeResolver; texture/renderbuffer `resource_*`), so the honest split
is likely 3-4 bits (framebuffer, texture-and-sampler, program, texture resources) — the P3a precedent argues for
splitting wherever two regressions would be different findings, and against splitting into an arm that is
non-functional on its own (`PipeFill.cpp:1062-1069`'s bit-8-without-bit-7 trap: **P4a's texture `resource_*` bit is
to its sampler-view bit what bit 7 is to bit 8**, and the same paragraph has to be written for it).

## 2.8 Espryt's slot tables — the seventh is P3a's

`MG_Backend/DirectGLES/SlotTables.h:138` defines `BackendSlotTable<StateObject, BackendObject, kKind>`; the
holder list (`:420-436`, `s_firstHolder` `:541-543`) is what makes `DestroyByLifetimeId` static-and-answered-by-every-
live-table. `TwinRegistry` instances, one per kind:
`VertexElementsCso` `Managers.h:1181` / `Managers.cpp:4673`; `Texture` `Managers.h:1521` / `Managers.cpp:7001`;
`Framebuffer` `Managers.h:1612` / `Managers.cpp:7754`; `ShaderCso` `Managers.h:2130` / `Managers.cpp:8065`;
`SamplerCso` `Managers.h:2230` / `Managers.cpp:10583`; `Renderbuffer` `Managers.h:2257` / `Managers.cpp:10695`.
The **seventh**, P3a's, is the buffer table at `Managers.h:691`
(`BackendSlotTable<BufferObject, GLESBufferResource, MGPipeKind::Buffer>`).
**`SamplerViewCso` has no twin registry at all** — it is the one P4a kind with no existing backend table.

The death dispatcher `OnFrontendStateObjectDestroyed(kind, lifetimeId)` is `Managers.cpp:198-245`: arms for
`Texture` `:202`, `Framebuffer` `:205`, `Renderbuffer` `:208`, `SamplerCso` `:211`, `ShaderCso` `:214`,
`VertexElementsCso` `:217-231` (P3a's C-1 note: this is now the *second*, redundant path — the client speaks the
whole death), and the `default:` arm `:232-243` recording that **buffer death crosses as `ResourceDestroy`**, so no
seventh raiser was added. The op table is installed/uninstalled at `Managers.cpp:2529-2557`
(`RegisterBufferBackendOps` / `UnregisterBufferBackendOps`), **unconditionally in a push build**, because the
subsystem bit is the *frontend's* dispatch predicate — "a build with bit 7 clear registers a table nobody calls and
the A/B stays a pure configuration question rather than a bring-up-order one" (`:2532-2537`).

---

# PART 3 — The gates after P3a

## 3.1 `scripts/p3a_untouched_regions.sh` (467 lines) — the byte-identical set

**Eleven** functions in `MobileGL/MG_Backend/DirectGLES/Managers.cpp` (`SOURCE_PATH` at `:85`):
`FUNCTIONS` `:87` = `IsPoolable EnrollIntoPool AcquireFromPool TrimBufferPool ClearBufferPool
ProcessDeferredBufferReleases CreateRingStorage RingAvailable RingAllocate FlushPendingRangesNow`;
`PINNED_FUNCTIONS` `:91` = `FlushPendingRangesFrom` with `PINNED_BASELINE_REF=3e298c9a` `:92` and
`PINNED_SHA_FlushPendingRangesFrom=37fc94ff…` `:93`; `EXPECTED_FUNCTION_COUNT=11` `:95`;
`SELF_TEST_FUNCTIONS="ClearBufferPool FlushPendingRangesNow FlushPendingRangesFrom"` `:104`.

The shape P4a copies (all from the header `:1-84`):
- **Why a file diff cannot say it**: the file is rewritten by design, so the claim is about *bodies*, extracted and
  hashed on their own (`:5-11`).
- **Each row is annotated with what the function is and why it matters** (`:13-30`).
- **The extraction is preprocessor-blind and mask-first** (`:53-64`): comments and string/char/raw-string literals
  are replaced by spaces of the same length, the *definition* is found as the one `<name> (` whose closing paren is
  followed (past qualifiers) by `{`, the body is brace-matched in the masked text and **hashed from the ORIGINAL
  text** — so a comment change inside one of these functions is a difference too, deliberately.
- **Exactly one definition must be found per name; zero or two is exit 2, never a silent pass** (`:66-68`).
- Three usages (`:70-76`): `<ref-a> <ref-b>` compares, `<ref>` prints the baseline list, `--self-test` proves the
  comparison can go red. **stdout is always the sha list** so the baseline capture is a plain redirect; everything
  else goes to stderr (`:78-79`).
- **Both arguments are git refs** — an uncommitted edit is invisible by design (`:81-82`).
- Exit codes `:84`-ish: 0 identical / 1 moved (naming the first in the fixed order) / 2 could not run.
- ID-15's ruling is transcribed at `:32-42`: two preprocessor arms, both hashed, the pull ladder against `BASELINE`
  and the push ladder against a **pinned** sha, because that one was born in P3a and has no body at the base ref.
  **P4a's own byte-identical set will have the same two shapes** (functions that predate the phase → compared against
  the base ref; functions born in the phase → pinned sha captured at the commit where they were reviewed).

## 3.2 `scripts/p3a_vertex_input_negative_control.sh` (278 lines) — G7 for the vertex conversion

`HEADER=MobileGL/MG_Impl/Pipe/VertexInputEmit.h` `:80`, `FIELD=IsBgra` `:81`, `TEST_NAME='VertexInputEmit\.'` `:82`.
The design (`:1-64`): the break is chosen so **the compiler cannot see it** — the struct still has the member, still
24 bytes, its four `static_assert`s still hold, `PipeFields.def` still names it and the generated comparator still
compares it; what breaks is the **value**. `IsBgra` is picked because it has the least other coverage (`Size` stays
4 for `GL_BGRA`, so a dropped `IsBgra` does not even change the attribute's size). The patch is applied **by regex,
not by an exact line**, because the header belongs to another package (`:25-29`); if the field is not assigned at
all, that is exit 2, never a pass. **Restore is not enough — the rebuild is part of the contract** (`:38-49`):
every exit goes through `repair()` (restore, rebuild, re-run), which also runs from the `EXIT` trap, and a failed
repair downgrades the verdict to 2. Logs live in `<build-dir>/p3a-g7-logs/`, inside the build tree so nothing is
left in the repository (`:51-53`). Exit codes `:55-62`: 0 tripped **and named the field**; 1 the control did not
answer (green with the field dropped, **or** red without naming it — both findings about the test); 2 could not run.
**Not a CI lane** — it rebuilds the library twice (`:31-32`).

`scripts/g7_negative_control.sh` is P2's sibling for the pipeline/dynamic split: it demotes `ColorMasks` out of
pipeline chunk P1 by inserting two boundaries, expects `RenderStateSpansTest` to go red **naming `SetColorMask`**,
relaxes the four measurement pins so the patch stays a semantic break rather than a build break (`:33-36`), and has a
`--verify-patch-only` mode (`:41-45`) that checks the mechanism without claiming the control passed.

## 3.3 The itest lanes and their `MOBILEGL_PIPE_PUSH` pins

`MobileGL/MG_IntegrationTest/CMakeLists.txt` (1389 lines). The scenarios are at
`Scenarios/{HandleRecycle, CsoContentAddressing, ResourceSubsystemControl}Scenario.cpp` `:133-135`.

- **The Handles-arm knobs** `:948-955`: `MGL_ITEST_HANDLES_ARM_KNOBS = "MOBILEGL_PIPE_LEGACY_MEMOS=0"
  "MOBILEGL_PIPE_PUSH=0x1ff"`, `MGL_ITEST_ABA_ARM_KNOBS = "MOBILEGL_PIPE_HANDLE_ABA_CONTROL=1"`, empty in a pull
  build. **The mask is the PHASE DEFAULT, not a hand-picked bit** (`:936-947`): it was `0x7f` and is now `0x1ff`,
  because *"pinning it at `0x7f` after P3a would leave the Handles arm asserting the P2 shape while the buffer and
  vertex-input handles it is supposed to be about stayed switched off - a lane that still passes and no longer
  measures the key that ships"*. **P4a must raise all of these to its own phase constant** — this is closed
  contract-review item 11 (`:1120-1128`) and it is stated for all three lane families.
- Six `HandleRecycle` lanes `:975-1030`: `DirectGLES.HandleRecycle.Handles.`, `DirectVulkan.HandleRecycle.Handles.`,
  `DirectGLES.HandleRecycle.Legacy.` (`MOBILEGL_PIPE_PUSH=0`), `DirectVulkan.HandleRecycle.Legacy.`,
  `DirectVulkan.HandleRecycle.AbaControl.` (`PUSH=0` + the ABA knob), `DirectVulkan.HandleRecycle.AbaControlHandles.`
  (the handles knobs + the ABA knob). The last one exists because *"the guard P2 SHIPS is the generation: a control
  that only defeated the retired guards would be green forever"* (`HandleRecycleScenario.cpp:87-93`).
- Four `CsoContentAddressing` lanes `:1067-1105` and their env `:1130-1146`: `0x1ff` for On,
  **`0x80000000000001ff`** for Off (bit 63 + the phase default) — the mask moves with the phase, the control is bit
  63, and the two must not be confused (`:1112-1128`).
- Two `ResourceSubsystemControl` lanes `:1147-1175`: On = `0x1ff`, **Off = `0x7f`** — "the two phase constants, not
  hand-picked bits". DirectGLES only, because Magma registers no `MGPipeResourceOps` (`:1121-1124`).
- Two `MapPersistentRoundtrips` counting lanes `:1177-1210`, each selecting **one** case by `TEST_FILTER` with a
  **private `MOBILEGL_LOG_FILE_PATH`**, plus `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1`. The private-log
  rule is stated three times (`:1035-1046`, `:1126-1140`, `Harness/PipeStatsWindow.h:19-25`): the library opens the
  log `fopen(path,"w")`, so **every process in a lane truncates it**, and two readers in one lane race under
  `ctest -j` with a failure that looks exactly like "the counter was never emitted".
- Two `LargeArenaAdoption` A/B lanes `:1212-1243`: `ResourceSubsystemOn.` (`0x1ff`) and `ResourceSubsystemOff.`
  (`0x7f`) over the whole scenario.
- **Everything is registered in EVERY build, including the pull build, so `ctest -L integration-gpu` stays
  name-for-name identical between pull and push (G2)**; in a pull build `MGITEST_PIPE_PUSH_BUILD` is absent and the
  cases **SKIP saying so** (`:362-378`, `:408-506`, `:1141-1146`). The CMake side greps the backend sources for the
  subsystem constant and the knob name and passes the answer in as `MGITEST_HANDLE_REKEY_<backend>` /
  `MGITEST_HANDLE_ABA_IMPLEMENTED`, with `CMAKE_CONFIGURE_DEPENDS` so the answer cannot go stale
  (`HandleRecycleScenario.cpp:98-103`).
- The verify block `:1246-1389`: `MGL_ITEST_VERIFY_TIMEOUT 900`; the ambient `DirectGLES.Verify.` /
  `DirectVulkan.Verify.` re-registrations labelled `"integration-gpu;integration-verify"`; the **arming** lane with
  its own log and `MGITEST_PIPE_ARMING_LANE=1`; negative control A (`MOBILEGL_PIPE_VERIFY_CORRUPT=
  GetRenderStateParameters` with `MOBILEGL_PIPE_VERIFY_FATAL=0` so the process survives and can read its own report
  back) and negative control B (`MOBILEGL_PIPE_POISON_OMIT=GenerateMipmap:GetActiveTextureUnit`, the scenario forks
  so the `abort()` is a datum in `waitpid()`).

## 3.4 `HandleRecycleScenario` — the arms, and which kinds already have ABA controls

`Scenarios/HandleRecycleScenario.cpp`. Three arms `enum class Arm {Handles, Legacy, AbaControl}` `:141-145`,
selected by the harness marker `MGITEST_HANDLE_ARM` (`:146-152`; **the library never reads it**). The arm
descriptions are at `:74-93`, the skip policy at `:95-113` (*"never a silently-deleted registration and never a green
that means 'the thing I test does not exist yet'"*).

Seven cases:
| line | case | kind |
|---|---|---|
| `:521` | `TheReproducerRecyclesEveryName` | — |
| `:568` | `AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` | VertexElementsCso |
| `:677` | (continuation case, VAO/vertex-input) | VertexElementsCso |
| `:768` | `ABufferAtARecycledAddressDoesNotInheritItsPredecessorsContents` | Buffer (P3a) |
| **`:869`** | **`ATextureAtARecycledAddressDoesNotInheritItsPredecessorsTwin`** | **Texture** |
| **`:920`** | **`AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin`** | **Framebuffer** |
| `:1015` | `DestroyedVertexArraysReturnTheirVertexElementsSlots` (the leak test) | VertexElementsCso |

**The texture and framebuffer cases already exist but are NOT ABA controls**: `:865-868` says in as many words *"The
`AbaControl` knob does not steer this path, so this case expects the correct pixels in EVERY arm — stated explicitly
rather than by omission"*, and the assertion is
`ExpectPixelsFor(m_arm, /*armExpectsCorruption=*/false, ...)` `:903-904`. The framebuffer case's readback is
deliberately **not** through the framebuffer under test (`:913-918`): it uses `glGetTexImage` on the replacement's own
attachment, so "the clear went somewhere else" is visible. **P4a's kinds (`Texture`, `Framebuffer`, `Renderbuffer`,
`SamplerCso`, `SamplerViewCso`, `ShaderCso`) need `armExpectsCorruption=true` controls of their own**, i.e. the
`MOBILEGL_PIPE_HANDLE_ABA_CONTROL` knob has to defeat the identity half of the memo keys P4a introduces
(the Magma side of that knob is `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h`'s
`MagmaPipeAbaControlDefeatsIdentity`, per ID-1). **Renderbuffer, SamplerCso, SamplerViewCso and ShaderCso have no
case at all today.**

## 3.5 The leak test and the `PipeSlotPeek` harness

`Harness/PipeSlotPeek.h` (44 lines) + `.cpp`. `enum class PipeSlotKind { Buffer, VertexElementsCso }` `:29-32` —
*"Mirrors `MG_Pipe::MGPipeKind` for exactly the kinds a scenario has a reason to count, so that the enum does not
travel through this header and the GL headers together"*. **P4a extends this enum for each kind it mints.**
`bool PeekPipeSlotLiveCount(kind, unsigned*)` / `PeekPipeSlotHighWater(kind, unsigned*)` `:41-42`, both returning
**false, touching nothing**, where the allocator is out of reach — a pull build has none, and on Android the module
links the shipping `libMobileGL.so` built `-fvisibility=hidden`. **`:37-39`: a caller that gets false must SKIP
rather than pass — "could not look" is not "did not leak".** It is its own translation unit because the scenario
sources include the GL headers with prototypes (`:19-21`).

The leak case `DestroyedVertexArraysReturnTheirVertexElementsSlots` `HandleRecycleScenario.cpp:1015-1110`:
skips unless `m_arm == Arm::Handles` (`:1019-1025`: the client mints the slot only when the subsystem is on);
skips if the peek is out of reach (`:1027-1032`); one shared VBO so only the VAO kind's slots churn;
one round = create → **DRAW** (which is what mints the slot and publishes the record) → unbind → delete;
**two warm-up rounds before the baseline is taken** (`:1076-1085`, so what is measured is growth *with* the churn and
not the one-off cost of drawing at all, the default VAO's slot above all); `kChurn = 48`; three assertions
(`:1098-1110`): `liveAfter == liveBefore`, `highWaterAfter == highWaterBefore`, `peakLive - liveBefore <= 1`.
The failure message names the cost (a `SlotState`, a lifetime-id map node, the applier's ~1.3 KB record, and past
`kMGPipeMaxVertexElementsSlots` a permanent `Fatal{ProtocolCorruption}`) and the backend. `:1005-1013` records why
the four ABA cases ran green over the leak: **the observable is the allocator, not pixels** — the leak produces
correct pictures the whole way to the fatal.

## 3.6 The verify lane, `retrace_gate.py` and `wsl_p3a_gate.sh`

- **Arming line** (ID-4 errata in `BRIEF-P3A.md:150-153`): `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`;
  per-case retrace logs carry `MGPipe verify: <case> <backend> armed, zero divergences, zero unmigrated reads`.
  The field count is pinned in `generated/PipeFilled.inc:96` (`kMGPipeInputFieldCount == 63`).
- **ctest name extraction**, mandatory (ID-4): `grep -E "^[[:space:]]*Test[[:space:]]+#[0-9]+:" | sed -E "s/^ *Test
  +#[0-9]+: //" | LC_ALL=C sort` — the naive `^\s+Test #` **drops every id under 1000**.
- **`~/w7/retrace_gate.py`**: `--tree --lib --out [-j] [--only REGEX]` (`:9`, args at `:34-39`).
  **`--only` is a REGEX**, applied as `re.search(a.only, k)` at `:51` — ID-15 records that the gate script once
  passed a comma list and matched 0 cases while reporting success. There is **no `--ssim` and no `--backend`**; the
  threshold is the per-case one in `trace_cases.json`.
- **`~/w7/notes/tools/wsl_p3a_gate.sh`** (70 lines), `usage: wsl_p3a_gate.sh [quick]`, `BASE=44c2b5cf` `:7`.
  Three builds first and **fails fast** (`:20-22`: *"results from a stale binary must never look like a verdict"*).
  `FAMILY` `:12` is the buffer/VAO regex
  (`LargeArenaAdoption|StorageBufferRegrow|VertexAttribBinding|MultiDraw|PrimitiveRestart|CrossFrameBuffer|
  ResidentIndex|BufferTexture|AtomicCounter|XfbCaptureBufferReuse|PackedWordReadback|DoublePrecision|
  VertexArrayEnableDisable|DrawParameters`) — **P4a needs its own FBO/texture/program family regex**.
  `NAMED` `:13` is the G3b regex, with the comment `# retrace_gate.py --only is a regex`.
  Part 1 (purity) `:24-32`: include closure, the `pGLContext` grep, the `MG_State`-in-`PipeApply.h` grep,
  `symbol_report.py --threshold 0`, the Resized table, `p3a_untouched_regions.sh $BASE HEAD`, the `RenderStateImpl`
  sha, `HandleRecycle` under verify.
  Part 5 `:34-38`: `gen_pipe.py --check/--self-test`, `gen_pipe_dirty_surface.py --check/--self-test`,
  `ctest -R 'RenderStateSpans\.|Residual|VertexInputEmit\.|ResourceEmit\.'`, the G5 self-test.
  Part 3 `:40-52`: G2 name diff, G14 removed/added, unit ×3 builds, then **five `integration-gpu` arms** —
  default, `MOBILEGL_PIPE_PUSH=0`, `MOBILEGL_PIPE_PUSH=0x7f`, `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1`, and the
  family subset — plus `g7_negative_control.sh`, `p3a_vertex_input_negative_control.sh`,
  `CsoContentAddressing|ResourceSubsystemControl`, and the verify controls
  (`PoisonOmitted\.|VerifyCorrupted\.|HandleRecycle`).
  Part 2 `:54-68`: `integration-verify`, the Fatal-line census, then the three retrace sweeps (verify / push / named)
  with the armed-count and Fatal-log checks, skipped by `quick`. Prints `P3A_GATE_DONE`.
- Tree/integration helpers are phase-parameterised (ID-3): `wsl_tree.sh p3a <slug> <base> [verify]` and
  `wsl_integrate.sh p3a <slug>…`.

## 3.7 CI (`.github/workflows/test.yml`)

Triggers `:3-21`: pushes to `dev`, the two backend branches, and — **`# TEMPORARY, remove before merging the MGPipe
work into dev`** `:9-12` — `feat/disaggregated`, so a phase's landing is not gated on someone dispatching by hand;
plus `workflow_dispatch` with a `baseline_sha` input defaulting to `087685d1` `:13-21` (**the SYMBOL baseline, not
the phase base ref**).

Jobs `:24-1554`: `build-linux`, `test`, `integration`, `build-linux-verify`, `integration-verify`, `flatc-check`,
`include-graph-check`, `benchmark`, `build-retrace`, `trace-cases`, `trace-fixtures`, `retrace`, `retrace-summary`,
`retrace-verify`, `monolith-symbol-report`, `remove-artifact-clutter`, **`pipe-gates`** `:1554`.

`pipe-gates` (`:1554-1687`), *"deliberately independent of `build-linux`: these are source-level gates, they take
seconds, and a broken build must not hide a drifted interface"*, `env: BASELINE: "44c2b5cf"` with the note
distinguishing it from `baseline_sha` `:1560-1565`, and `fetch-depth: 0` because the G5 gate reads `Managers.cpp` at
`BASELINE` with `git show` `:1567-1572`. Steps:
1. `Regenerate the MGPipe interface (G1-G7)` — `python3 scripts/gen_pipe.py` then
   `git diff --exit-code -- MobileGL/MG_Pipe/generated` `:1579-1583`.
2. `The MGPipe generators' checks can still fail` — `gen_pipe.py --self-test` `:1585-1591`.
3. `The symbol report's buckets and gates can still fail` — `symbol_report.py --self-test` `:1593-1595`.
4. `No stdio instrumentation in MG_Backend or MG_State` `:1597-1612` — one grep alternation covering
   `fprintf(stderr|stdout)`, `printf(`, `puts(`, `std::cout|cerr`, **with no exceptions**.
5. `MGPipe dirty-surface mapping is complete (G9)` — `gen_pipe_dirty_surface.py --check` **and** `--self-test`
   `:1614-1636`.
6. **`The buffer pool, the deferred-release drain and the rings did not move (G5)`** `:1638-1665`, guarded
   `if: github.ref == 'refs/heads/feat/disaggregated' || github.event_name == 'workflow_dispatch'` — *"the question
   is 'did these eleven move since P3a started'… it belongs with the TEMPORARY trigger lines at the top of this file
   and retires with them"*.
7. `The untouched-region gate can still fail (G5)` — the `--self-test`, same guard `:1667-1669`.
8. `Documentation citation lint` — **warning only** (`|| true`) while the documents settle `:1671-1687`.

ID-16 / ID-22 record the two reds that are **not** code signals: GitHub artifact-download `digest-mismatch` on
retrace matrix entries, and an HTTP 504 from `gh api --method DELETE` in "remove artifact clutter" after every test
passed.

---

# PART 4 — The measurement infrastructure as of P3a

All under `~/w7/notes/tools/` (integrator-owned, ID-3).

- **`wsl_build_trace_apks.sh`** (29 lines). `usage: wsl_build_trace_apks.sh <phase> [tree]`; builds the trace-flavour
  APK (arm64-v8a, **release**, debuggable, INFO) twice — pull, then push via `-Pmobilegl.pipePush=ON` — and
  **debug-signs both**, because a WSL build has no `SIGNING_*` keys and the release APK comes out unsigned
  (`adb install` fails `INSTALL_PARSE_FAILED_NO_CERTIFICATES`). It creates `~/.android/debug.keystore` if absent,
  wipes `android-plugin/app/.cxx` between arms, and **prints the lib size plus the `MGPipe` and
  `map-persistent-roundtrips` string counts** for each APK — ID-17's guard, so an -O0 APK can never pass unnoticed
  again (**Release `.text` ≈ 9.4 MB; the P2-era A/B APK's 21.8 MB `.text` was a Debug-flavour build**). Output:
  `~/w7/notes/<phase>/apk/trace-{pull,push}.apk`.
- **`p3a_ab.sh`** (53 lines) — the paired device A/B, run from Windows Git Bash with `adb.exe` on PATH.
  `usage: p3a_ab.sh <serial> [case …]`; default cases: `improved-transparency-minecraft-26.3`,
  `minecraft-1.21.4-rd12-odinlite-in-world`, `minecraft-1.21.4-fabric-sodium-in-world`,
  `minecraft-1.21.1-neoforge-create-instancing-in-world`. Matrix `{pull,push} × {nofinish,finish} × cases ×
  {DirectGLES,DirectVulkan}`. Protocol: **reboot-clean once**, wait for `sys.boot_completed` **and** for root, wake,
  **pin verified and retried up to 3 times** (`pin_device.sh pin` then `check`, exit 43 on failure), then a
  **device-lock hold per run** (`mkdir` lock with a 900 s timeout → exit 42) and `pin-before.txt` / `pin-after.txt`
  around each run. The runner is
  `tools/trace_replay/run_android_retrace_local.py --case … --backend … --benchmark [--benchmark-no-finish]
  --benchmark-repeats 3 --benchmark-tail-frames 200 --env MOBILEGL_PIPE_STATS=1 --env MOBILEGL_PIPE_STATS_PERIOD=120`.
  Outputs go to a **Windows-local** scratchpad dir (Git Bash cannot create directories on `//wsl.localhost`) and are
  copied back by `copy_ab.sh`. Note at the top: **do not export `MSYS_NO_PATHCONV` globally** here.
- **`p3a_ab_7f.sh`** — the same script with four differences (verified by diff): the output root
  (`p3a-ab7f`), the loop restricted to `arm=push` × `mode=nofinish`, the directory suffix `push7f`, and
  **`--env MOBILEGL_PIPE_PUSH=0x7f`**. It is ID-17's supplementary arm: the *same push APK* run with P2's
  subsystems only = the device-side T2.
- **`ab_reduce3.py`** (84 lines) — `usage: ab_reduce3.py <main-root> <7f-root> <out.md>`. Reads the runner's own
  "best of N" line (lowest mean wall frame time repeat) for CPU p50/p99 over the trailing window
  (`runner_best` `:14-24`), the pin verdicts from `pin-before/after.txt` (`pin` `:26-37`), and the **summed `mpr=`**
  out of every `MGPipe stats: frames=` line in `mobilegl.log` (`mpr_total` `:39-49`). Emits a 13-column table:
  pull p50 | P2-only (0x7f) p50 | P3a (0x1ff) p50 | Δ P2 | Δ P3a total | pull p99 | P3a p99 | pins pull/7f/P3a |
  mpr 7f/P3a, with the footnote *"the P3a-only share is the difference of the two"*.
- **`wsl_p3a_bench.sh`** (62 lines) — desktop DriverBench, run on an **idle box (no gate, no builds)**.
  `REPEATS=5`, `DRIVERBENCH_FRAMES=240`, cases `mc_vanilla_draw mc_state_toggle mc_pass_switch`, lavapipe/llvmpipe
  ICDs, `EGL_PLATFORM=surfaceless`. Eleven arms: `native`, and for each of `espryt`/`magma`:
  `-pull`, `-push`, **`-push7f` (`MOBILEGL_PIPE_PUSH=0x7f` = T2)**, `-push0` (`=0`),
  `-nocso` (`=0x80000000000001ff`). Writes `driverbench.csv` (every repeat) and `driverbench.md` (medians +
  **T1 = push(0x1ff) − pull, T2 = push(0x7f) − pull, T1 − T2 = what the phase added**).
- **The recorded numbers** (ID-19, ID-24), which are P4a's comparison baseline:
  desktop `mc_vanilla_draw` ns/draw — Espryt pull 5115 / push(0x1ff) 6162 / push(0x7f) 5463 / push(0) 5693;
  Magma pull 17099 / 17709 / 17287 / 17739. **T1 = +1048 (+20.5%) / +611 (+3.6%); T2 = +349 / +189;
  P3a itself = +699 / +422 ns/draw** — the largest boundary cost so far. Device (Release APKs from `3e298c9a`):
  P2's boundary = +6-12% p50 on both devices and backends; P3a adds ~0 on 26.3 and sodium but **+17-19 points on
  rd12** (Espryt +30%, Magma +27% total); MC 26.3 p99 on Adreno 25.46 → 26.32 ms (+3.4%). `mpr` on device: 26.3 = 8
  per run, sodium 1.
- **Thermal / device traps** (ID-23 and the `fcl-device-testing` memory): rd12 on Magma on the Xiaomi **aborts on
  every arm including pull** (a scudo map error inside a `calloc` from libMobileGL seconds after start, with 6 GB
  free) — pre-existing, excluded from the table, task chip open. **Side effect: the GPU pwrlevel pin resets after the
  crash** (`pin_device` then reports "range 2..5"), so every row taken *after* rd12 on that device carries a **DRIFT**
  verdict on both arms (still paired). Also excluded: `create-instancing` (a 2-frame fixture, not a benchmark) and
  sub-ms sodium rows (noise). The protocol memory adds: CPU big 1.96 / little 1.55 GHz fixed, GPU pinned, a 40 °C
  entry threshold, downclock afterwards, and the harness's `pidof` single-sample kill is on the optimisation list.
- Artefact locations (ID-24): `~/w7/notes/p3a/ab` (60 `benchmark.json`), `ab7f` (15), `ab3-final.md`;
  bench `~/w7/notes/p3a/bench/driverbench.md`; APKs `~/w7/notes/p3a/apk/`.

---

# What P4a has to decide that P3a did not

Collected, each with the file that forces the question:

1. **The `MGPResourceDesc::Target` enum does not exist.** `ResourceTracker.h:154-162` says the contract package
   minted none and that `Buffer` is 0 only because it is the leading member of a comment. Every texture target,
   `Renderbuffer` and `TexBuffer` need real values, in `MG_Pipe`, before a second producer writes the field.
2. **`MGPipeBindBit` must move out of `ResourceTracker.h`.** `ResourceTracker.h:64-66` names P4a's texture family as
   exactly the second producer that triggers the move, and four of the twelve bits
   (`Sampler`, `ShaderImage`, `RenderTarget`, `DepthStencil`) have no writer today.
3. **`kMGPipeMaxResourceSlots` is one bound for all resource kinds** (`PipeApply.h:105-113` argues the two existing
   bounds differ *because the two records differ*). A texture record carrying an 88-byte descriptor plus per-level
   state is a third size class.
4. **`MGPSubData`'s buffer-only gate** (`PipeApply.cpp:510-531`) refuses `Level != 0` and `RegionCount != 0`
   unconditionally; the texture path needs the branch on `Target` plus a real box/level/region validator.
5. **`SamplerViewCso` is the one P4a kind with no `TwinRegistry`** (`Managers.h` has six, `:1181/:1521/:1612/:2130/
   :2230/:2257`, plus the buffer table `:691`), and therefore no backend death arm in
   `OnFrontendStateObjectDestroyed` (`Managers.cpp:198-245`). Its lifetime is client-only, like
   `VertexElementsCso` after C-1.
6. **`kUnpublishedDestroy` for programs/pipelines/shaders** (`DirtySurface.def:301-305`) is exactly what P4a's
   shader CSO closes; three rows change and the gate checks both directions.
7. **`SetFramebufferState` has no `MGPipeSuppressorSlot`** although its `ContentHash`'s stated second job is client
   suppression (`MGPipeTypes.h:369-373`); and the three slots P4a will wire are labelled `P3b`/`P4b` in
   `SetHashSuppressor.h:46-50`, which disagrees with `ROADMAP.md:20`.
8. **The `EmittedCallSuppliesTheWholeField` answer for every P4a accessor is almost certainly "no"** for the same
   pointer-storage reason as `GetBoundVertexArray` (`PipeFill.cpp:1094-1115`); that has to be decided in that
   function, deliberately, and not inherited from a `Coverage.def` row's presence.
9. **`MGPSamplerDesc` carries an `MGPBlobRef` but its `PipeCalls.def:104` row is `kNone`**, unlike every other
   blob-carrying row.
10. **Bit ordering**: P4a's texture-`resource_*` bit is to its sampler-view bit what bit 7 is to bit 8
    (`PipeFill.cpp:1062-1069`) — one direction of the A/B is non-functional and must be documented in the same place.
