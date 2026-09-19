# S3 - framebuffers, attachments, images (P5e scouting, head `2fde7034`)

Paths under `MobileGL/` unless they start with `docs/`. Kind legend as the E note's: **live** =
dereference of a frontend object/array; **id** = `GetLifetimeId()` + a client-allocator probe;
**mint** = an allocator WRITE; **rec** = already answered from a record.

## 1. The live reads of this family at `2fde7034`

`GetFramebufferBindingSlot` and `GetImageTextureBinding` are both still BARRIER_PULLED
(`MG_Pipe/FieldOwnership.def:85-88`), filled every kDraw (`MG_Impl/Pipe/PipeFill.cpp:175-184`)
into `PipeInputs.h:164-165`, read through `PipeInputs.h:628-651`. The backend's door is
`GetFramebufferBindingSlotChecked` (`DirectGLES/DirectGLES.cpp:179-197`) - under
`EsprytSlotTablesEnabled()` a straight `MGB_CTX->GetFramebufferBindingSlot` (`:182`).

### 1.1 Framebuffers (a.5)

| site | dereferenced | kind | when |
|---|---|---|---|
| `SyncNeccessaryTextures` draw-FBO list `:2405-2409` | slot, `GetBoundObject()`, `GetVersion()`, `currentFBO->GetObjectVersion()` | live | **every draw**, memo hit or miss |
| same, memo hit `:2424`, `:2437`, `:2441-2447` | `g_fboTextureSyncListFbo == currentFBO.get()` (identity half is a POINTER COMPARE, `:2162-2165`), `PairingsIntact` -> `entry.slot->get()` on borrowed attachment slots (`:2127-2131`, `:2134-2139`), `IsDrawSyncClean(entry.slot->get())`, `Sync{TextureParams,BuiltinSampler,Mipmaps}ToBackend(*entry.slot)` | live | every draw on a hit |
| same, miss `:2452-2457` | `currentFBO->GetAllAttachmentObjects()`, `attachment.GetTexture()`, `SyncTextureObjectToBackend(textureObject)` | live, id, mint | rebuild |
| `SyncReadFramebufferTextureAttachments` `:2246-2259`, `:2278`, `:2285`, `:2290-2296`, `:2303-2308` | read slot, `GetBoundObject()`, `readFBO.get() == drawFbo`, both versions, then the draw list's shape verbatim | live, id, mint | **every draw** (push build) |
| `FramebufferRecordMatchesBinding` `:3017-3033` | `GetFramebufferBindingSlotChecked(target).GetBoundObject()` `:3019`, compare vs `pDefaultFramebufferInfo->defaultFBO` `:3021`; the `HandleOf` half `:3032` is transport-gated off `:3030` | live | every `SyncCurrentFBO`, both targets |
| `SyncCurrentFBOByRecord` `:3058-3274` | `:3178-3179` slot + `GetBoundObject()`; `:3228` `NoteStateForHandle(record.Fbo, currentFBO)`; `:3264` `SyncReadBufferToBackend(currentFBO)`; `:3269` `SyncToBackend(currentFBO, target)` | live (identity + the object handed on) | per target, only when `FramebufferSerial` moved (`:3122`) |
| `BackendFramebufferObject::SyncToBackend` `Managers.cpp:10044-10429` record arm | `stateFBOObject->GetExternalIndex()` (logs), **`GetAllAttachmentObjects()` `:10283`**, **`GetAllFramebufferAttachmentVersions()` `:10284`**, `attachmentObject.IsEmpty()` `:10310`/`:10328`, the attachment object handed to `SyncAttachmentSurface` `:10338` | live | every walk the record hash re-arms (`:10277-10281`) |
| `SyncAttachmentSurface` `Managers.cpp:9585-9742` | `attachment.GetTexture()` `:9617`, `SyncMipmapsToBackend(textureObject)` `:9657`; `attachment.GetRenderbuffer()` `:9698`, **the renderbuffer cross-check `HandleOf(renderbufferObject.get())` `:9711` is NOT transport-gated** (the texture one at `:9629-9632` is), `BackendRenderbufferObject::SyncToBackend(renderbufferObject)` `:9732` | live, id | per re-armed point |
| `BackendRenderbufferObject::SyncToBackend` `Managers.cpp:13550-13584` | `g_backendRenderbufferObjects.HandleOf(stateRBOObject.get())` `:13574` inside the scope `:13570`; the four VALUES are already record-supplied `:13535-13548` | id | per attached RBO sync |
| `SyncReadBufferToBackend` `Managers.cpp:9834-9953` record arm `:9853-9936` | only `GetExternalIndex()` in logs; `m_pushedSyncHandle` is the key `:9856-9862` | rec | - |
| `SyncCurrentFBO` pre-handle arm `:3294-3377` | scope `:3303`, slot/`GetBoundObject` `:3305-3306`, versions `:3317-3318`, `defaultFBO` compare `:3336`, `Find(currentFBO.get())` `:3358`/`:3367`, `GetOrCreate(currentFBO)` `:3369` | live, id, mint | only when the record arm declines |
| `BindCurrentFBO` `:4514-4623` | record arm `:4528-4564` reads NOTHING frontend (`FindByHandle(record.Fbo)` `:4543`); decline `:4568-4622`: slot, `GetBoundObject`, `defaultFBO` `:4581`, scope `:4585`, `Find(currentFBO.get())` `:4592` | rec / live+id | per bind |
| `ForceBindCurrentFBO` `:4716-4754` | `:4720-4721` slot + `GetBoundObject()` **before** the handle arm `:4725-4730`, and `:4734-4738` stamp `GetVersion()/GetObjectVersion()/.get()` **after it** | live | every DSA clear, both blits |
| `SyncCurrentProgram` broadcast count `:4347-4410` | record arm `:4353-4382`; decline `:4386-4389`: slot, versions, `GetDrawBuffers()` | rec / live | per draw |
| `BlitFramebuffer` bound arm `:8398-8410` | `GetFramebufferBindingSlot(Read/Draw).GetBoundObject()` `:8408-8409` | live | per bound blit |
| `BlitFramebuffer` named arm `:8335-8382` | `StateForHandle` `:8361` + `pDefaultFramebufferInfo->defaultFBO` `:8358-8359`; then `ForceBindCurrentFBO` x2 `:8375-8376` | rec + live via the note | per named blit |
| `BlitLayeredDestinationAspects` `:8182-8320` | `GetDrawBuffers()` `:8210`, `GetReadBuffer()` `:8227`, `GetAttachment()` `:8241-8242`, `IsTexture/GetTextureLayer/IsLayered/GetTexture/GetFormat/GetSamples` `:8244-8263`, `SyncTextureObjectToBackend` `:8294-8295` | live, id, mint | both blit arms |
| `Clear` `:6180-6192` | `SyncNeccessaryTextures()` + `SyncCurrentFBO()` + `BindCurrentFBO(Draw)` - all of the above | live | per clear |
| `GetReadColorAttachment` `Managers.cpp:9395-9405`, `IsFixedPointFallbackReadAttachment` `:9773-9796` | read slot, `GetReadBuffer()`, `GetAttachment()`, format | live | readbacks, off the draw path |

### 1.2 Images (a.4)

| site | dereferenced | kind | when |
|---|---|---|---|
| `SyncImageTextureBindingsForDraw` `:2902-2914` | `g_imageUnitHighWaterMark == 0` early-out `:2903` | - | zero reads on a Minecraft draw |
| `SyncImageTextureBindings` `:2783-2867` | record arm `:2829-2854` reads `st.ShaderImageStart/Count`; the walk is `[0, max(Start+Count, highWaterMark))` | rec | per sweep |
| `SyncImageTextureBinding` `:2646-2749` | `MGB_CTX->GetImageTextureBinding(unit)` `:2650` (LIVE, unconditional); `IsWritableImageBufferTexture` `:2651` -> `binding.Texture->GetStorageType()` `:2523-2525`; `imageBinding.Texture` `:2652-2660`; `SyncTextureObjectToBackend(.., true)` `:2660`; `Texture->GetTarget()` `:2682`, `:2723`; `Texture->GetFormat()` `:2724`, `:2736`; **`imageBinding.Access` always frontend `:2744`** | live, id, mint, rec (4 of 5 values `:2670-2681`) | per swept unit |
| `ResolveShaderImageRecord` `:2582-2643` | scope `:2588`; `g_backendTextureObjects.HandleOf(boundTexture)` `:2634` | id | per swept unit |
| `MarkWritableImageBufferTexturesGpuWritten` `:2763-2781` | gate `:2764`; `GetImageTextureBinding` `:2767`; `Texture.get()` cast to `TextureObjectBuffer` `:2772-2773`; `GetBufferBindingSlot().GetBoundObject()` `:2774`; `MarkBufferGpuWritten` -> `HandleOfBuffer` (`Managers.cpp:2947-2978`, `:2922-2941`, scope `:2938`) | live, id | **every draw and dispatch** when any writable buffer image exists (`PrepareForDraw:4804`) |
| `BoundImageUnitFormat` `Managers.cpp:10847-10850` | live image binding's `Format`, reached per draw from `!twin->ImageUnitFormatsStillMatch()` (`DirectGLES.cpp:4472` -> `Managers.cpp:11227-11238`, `:11231`) and at bake `:11071` | live | only for programs with format-less image uniforms |

## 2. What the records and applier state already carry

- `MGPFramebufferState` (`MG_Pipe/MGPipeTypes.h:750-782`, 304 B): `Fbo`, `Color[8]`, `Depth`,
  `Stencil`, `ReadSurface` (RESOLVED, per object, under every Target), `DrawBuffers[8]` (Int8
  index, -1 = None), `Width/Height/Layers/Samples`, `FixedSampleLocations`, `IsDefault`,
  `Complete`, `Target`, `ContentHash`. `MGPSurface` (`:628-655`): `Res`, `InternalFormat`
  (inline), `Kind`, `Layered`, `Level`, `Layer`, `UploadTarget`, `TextureTarget` (added by ID-12
  DV-5 so the four cross-object masks fall out with no lookup, `:636-653`).
- Applier: `FramebufferRecords` keyed by `Fbo.Slot` with the generation checked, and NOT
  cleared by `MGPipeApplierReset` (`MG_Pipe/PipeApply.h:512-524`); `BoundFramebuffer[2]`
  `:646`; `FramebufferSerial`, advanced on every applied record including `Named` `:647-652`;
  `StaleFramebufferRecordLookups` `:653-663`; `FramebufferRecordFor` `:777`; the backend's
  door `BoundFramebufferRecord` (`DirectGLES.cpp:238-241`). Blit workspace
  `VerbBlitReadFbo/DrawFbo/NamedConsumed` `PipeApply.h:733-735`.
- Emitter `MG_Impl/Pipe/FramebufferEmit.h`: `EmitFramebufferState` `:261-321` (both bindings, one
  `Both` record when shared), `EmitFramebufferByName` `:340-389` (every DSA entry point),
  `BuildFramebufferState` `:480-559`, `SurfaceOf` `:579-605`, `FillGeometry` `:610-638`,
  `ContentHash` `:216-238`, the two wire-width refusals `:489-520`, `NoteFramebufferDied`
  `:401-405`. Dirty rule: `NEW_FRAMEBUFFER` off `m_anyAttachmentGeneration` + object/slot version
  (`docs/Disaggregated/ARCHITECTURE.md:165`).
- `object_death` (`MG_Pipe/PipeCalls.def:259-265`) is the framebuffer family's wire delete; client
  `MG_Remote/Client/WireTables.cpp:569`, server `MG_Remote/Server/PipeApplier.cpp:988-1013` ->
  `ReleaseTwinsForWireObjectDeath`. Textures and renderbuffers ride the same opcode.
- Twin table already handle-capable: `FindByHandle` (`DirectGLES/SlotTables.h:420-426`),
  `GetOrCreateByHandle` (`Managers.h` ~453, used at `DirectGLES.cpp:3197`, `:4687`), `LiveGenAt`
  `:347-351`, `NoteStateForHandle`/`StateForHandle` `:362-378`, `ReleaseByHandle` `:394-405`;
  `m_pushedSyncHandle` (`Managers.h:1857`) keys the twin's record lookup off the caller's handle
  under a transport (`Managers.cpp:10095-10101`).
- Images: `MGPImageView` (`MGPipeTypes.h:877-886`, 24 B, no pad) = `{Res, Unit, InternalFormat,
  Layer, Level, Layered, Access}`; `MGPShaderImages` `:888-892`; applier `BoundShaderImages`,
  `ShaderImageStart/Count`, `ShaderImagesSerial` (`PipeApply.h:683-686`). Emitter
  `MG_Impl/Pipe/ImageEmit.h:91-144`.
- **`Access` IS encoded already**: `MGPipeEncodeImageAccess` (`ImageEmit.h:163-176`), 0/1/2 for
  `GL_READ_ONLY`/`WRITE_ONLY`/`READ_WRITE`, asserting on anything else. The server comment saying
  the encoding "does not exist at the contract commit" (`DirectGLES.cpp:2662-2669`) is STALE.
- Buffer-texture backing: `MGPResourceDesc::BufferForTexBuffer` (`MGPipeTypes.h:357`) and
  `Target == MGPipeResourceTarget::TexBuffer` (`:188`, `:226-227`) already cross; renderbuffers
  get the same discriminated descriptor (`MG_Impl/Pipe/TextureEmit.h:269-274`, `:851-882`).

## 3. The gap - what does not exist yet

1. **A server decode of `MGPImageView::Access`** - one pure function beside
   `ResolveShaderImageRecord` plus a Table 0 line pinning 0/1/2. No wire or client change;
   `DirectGLES.cpp:2744` is the only reader.
2. **No per-surface extent in `MGPSurface`** - but none is needed: the RBO descriptor already
   carries format/width/height/samples (`Managers.cpp:13535-13548`). What is missing is a
   handle-taking `SyncToBackendByHandle(MGPipeHandle)` so `surface.Res` replaces `HandleOf(object)`
   at `:13574`; same shape for `SyncMipmapsToBackend` (S2's file).
3. **No record says which attachment POINTS exist.** The walk at `Managers.cpp:10283-10296` uses
   the frontend's 41-point array for membership and for the per-point version memo. No new field:
   `BuildFramebufferState` refuses any record whose framebuffer holds a point at or above the wire
   width (`FramebufferEmit.h:489-501`) and the FRONT/BACK tokens exist only on the default
   framebuffer (`:527-532`), so the record's 11 surfaces ARE the total point set. The rule needs
   contract text, because the detach (`:10310`) and the N-6 refusal (`Managers.cpp:9601-9606`)
   both read `attachmentObject.IsEmpty()` today.
4. **The two attachment texture-sync lists are keyed on a frontend pointer**
   (`DirectGLES.cpp:2424`, `:2278`) with entries BORROWING the attachment's own `SharedPtr` slot
   (`:2127-2131`). No new record field - `record.Fbo` + `ContentHash` is already a complete key
   (`:2158-2169`) and the surfaces carry the texture handles - but the entry becomes
   `{MGPipeHandle Res, BackendTextureObject*}` and `PairingsIntact` a handle compare.
5. **The image sweep's gate is a FRONTEND counter** - `keys.samplingGeneration` (`:2904-2906`),
   deliberately so (`:2890-2901`), and the server cannot read it under run-ahead. P5c rv already
   moved the three texture shutters to APPLIER_DERIVED serials (`PipeApply.h:696-700`); the gate
   must be re-keyed onto `ShaderImagesSerial` + the texture params/content serials, and the "a
   texture bound only to an image unit is re-minted INSIDE this sweep" property re-argued (§7).
6. **No handle form of `MarkBufferGpuWritten`** (`Managers.h:1005` takes the object) - needed:
   `MarkBufferGpuWrittenByHandle(MGPipeHandle)` that skips `HandleOfBuffer`.
7. **`ForceBindCurrentFBO` reads the binding slot on both sides of its own handle arm**
   (`:4720-4721`, `:4734-4738`); under run-ahead it must be handle-only and stamp nothing
   frontend.

## 4. The retirement design

**Key.** The framebuffer twin is ALREADY handle-keyed at its record-driven entries
(`GetOrCreateByHandle(record.Fbo)` `:3197`, `FindByHandle(record.Fbo)` `:4543`,
`GetOrCreateByHandle(fbo)` `:4687`). P5e makes that the ONLY key: under a live wire the
`Find(currentFBO.get())` / `GetOrCreate(currentFBO)` arms at `:3358`, `:3367-3369`, `:4592`,
`Managers.cpp:10101`, `:9711`, `:13574` go, and `NoteStateForHandle`/`StateForHandle` go with them
- the twin's sync body takes the RECORD, so there is no frontend object to reach.
**`SyncCurrentFBO` becomes `SyncCurrentFBOByRecord` only:** the pre-handle arm (`:3294-3377`) is
unreachable and a decline is `Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot"}` rather than
a fallback, which is P5e-3's detector. `FramebufferRecordMatchesBinding` (`:3017-3033`) loses its
remaining half - the record was resolved from `BoundFramebuffer[target]`, so it IS that binding by
construction (`:3025-3030` already says so) - and becomes monolith-only.

**`BackendFramebufferObject::SyncToBackend(record, asTarget)`.** A signature change, not a body
change: `pushedRecord` becomes the parameter (`Managers.cpp:10082-10111` collapses), the draw
buffers are `DecodePushedDrawBuffers` output (`:10118-10119`, already) and the four masks the
record's surfaces (`:10189-10214`, already); the walk at `:10283-10296` becomes a walk over
`Color[0..7] + Depth + Stencil` (just `Color[0]` when `IsDefault`).
`m_syncedFrontendAttachmentVersions` is replaced by the record hash alone (`:10277-10281`) - the
"over-firing is free" second gate at `:10269-10276` hedged against a frontend attachment moving
without the record, which is unrepresentable once the record is the only statement. The re-entry
on `g_attachmentBackendIdGeneration` (`:10426-10428`) stays; it is server-owned.

**`SyncAttachmentSurface(glFBOTarget, surface, glBackendAttachment)`** - the `attachmentObject`
parameter goes. The empty point is `Kind == None || MGPipeHandleIsNull(Res)` (`:9588-9589`) and
the detach at `:10310` takes the SAME test, so N-6's "record empty, frontend still holding" hole
(`:9593-9606`) becomes unrepresentable. Both cross-checks (`:9629-9632`, `:9711-9718`) become
monolith-only - the renderbuffer one is not transport-gated today and must be, in the same edit.
Storage goes through `SyncMipmapsToBackendByHandle` / `SyncToBackendByHandle(surface.Res)`.

**"Is it clean" is already server-owned:** `SyncedFramebufferSerialIsCurrent(target,
st.FramebufferSerial)` (`:3035-3040`, `:3122`) plus `g_attachmentBackendIdGeneration` and
`g_backendContextGeneration`. Nothing new for the FBO twin. The two attachment texture lists key
on `(record.Fbo, ContentHash, contextId, g_backendContextGeneration)` with `PairingsIntact` as
`entry.Res == surface.Res`; `IsDrawSyncClean` is S2's to re-key.

**`BindCurrentFBO` / `ForceBindCurrentFBO`.** The record arm at `:4528-4564` becomes the only arm.
`ForceBindCurrentFBO` becomes: read `st.BoundFramebuffer[target]`, call
`SyncAndBindFramebufferByHandle` (`:4670-4713`, handle-only once `StateForHandle` gives way to the
record), invalidate `g_fboSyncedSerials[target]` (`:4750-4752`); the three frontend stamps at
`:4734-4738` are deleted under a live wire.

**Named blit.** `BlitLayeredDestinationAspects` takes two `const MGPFramebufferState&`: draw
buffers from `DrawBuffers[]`, the read buffer from `ReadSurface` (matched against `Color[]` as
`SyncReadBufferToBackend:9900-9912` does), `IsTexture` from `Kind`, layer/level/layered/format
from the surface, samples from `record.Samples`, the two textures by `FindByHandle(surface.Res)`.
That retires `StateForHandle` `:8361` and the `defaultFBO` pointer `:8358-8359` (use
`kMGPipeDefaultFramebuffer`). `ReadFbo`/`DrawFbo` are already in the workspace
(`PipeApply.h:733-735`); the bound arm's `:8408-8409` becomes
`FramebufferRecordFor(st.BoundFramebuffer[t])`.

**Images.** `SyncImageTextureBinding(unit)` becomes `SyncImageTextureBinding(const MGPImageView&)`:
`Res` is the texture (twin by `FindByHandle`), `Level/Layer/Layered/InternalFormat` are already
the record's (`:2670-2681`), `Access` is `MGPipeDecodeImageAccess(view.Access)`, and the two
texture-property reads (`GetTarget()` `:2682`/`:2723`, `GetFormat()` `:2724`/`:2736`) come off the
**twin**, which owns its target and storage format after tx's sync - not off a new wire field
(`MGPImageView` is 24 B with no pad). `ResolveShaderImageRecord`'s identity test (`:2634`) and its
I5 seam log go: no second answer is left. `g_imageUnitHighWaterMark`, `g_writableImageBufferUnits`
and `g_imageSweepValid` are already server-owned (`:2519-2547`, `:2877-2880`) and stay.
`MarkWritableImageBufferTexturesGpuWritten` becomes: per tracked unit take
`st.BoundShaderImages[unit]`, require `Access != 0` and the descriptor's `Target == TexBuffer`,
call `MarkBufferGpuWrittenByHandle(desc.BufferForTexBuffer)` - no frontend read, no
`HandleOfBuffer`, and `MGPipeReverseAnnouncementScope` loses one of its two draw-path callers.

## 5. Monolith / G1

Every site above already has a `MOBILEGL_PIPE_PUSH` record arm and a pre-handle arm, and the
split-only decisions are already spelled as `MG_Config::Transport != Monolith` (`:3030`, `:3226`,
`:4725`, `Managers.cpp:9629`, `:10097`). P5e keeps that shape: the pre-handle arms stay compiled
and stay the monolith path; what changes under a live wire is that a decline becomes Fatal
instead of a fallback. The pull build sees no token move - the handle-taking forms live inside
`#if MOBILEGL_PIPE_PUSH`, and the frontend-object overloads of `SyncToBackend` /
`SyncMipmapsToBackend` / `SyncAttachmentObject` are kept verbatim for it. `SyncToBackend` gains an
OVERLOAD rather than a changed signature, so the `#else` arms at `Managers.cpp:10120-10122` and
`:10348-10350` are textually unchanged (the discipline `MGB_RBO_*` uses at `:13534-13548`).
G2/G14: no new name enters `MGPipeTypes.h`; the new public names are `MGPipeDecodeImageAccess`,
`SyncToBackendByHandle`, `SyncMipmapsToBackendByHandle`, `MarkBufferGpuWrittenByHandle`.

## 6. Red-once evidence

| arm reverted | goes red as |
|---|---|
| `SyncAttachmentSurface` takes the frontend attachment again | `Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot"}` on the strict lane at the first attached draw (`SnormAttachmentScenario`, `ThreeChannelAttachmentScenario`) |
| the draw-FBO list key back to the pointer compare (`:2424`) | `Fatal{RoleViolation, "MGPipeSlots"}` from `HandleOf` inside the rebuild (`SlotTables.h:441`) - the scope is gone by then |
| `MGPipeDecodeImageAccess` mapped 1<->2 | `FormatlessImageBakeScenario` / `ImageLoadStoreSsoScenario` red (a `writeonly` image read back) - and a new unit case in `MG_Test/Pipe/ImageEmitTest.cpp` beside `TheApplicationsFormatAndAccessTravelUnrecast` (`:452`) pinning the three constants on BOTH sides |
| `MarkWritableImageBufferTexturesGpuWritten` off the descriptor's `BufferForTexBuffer` | `BufferTextureScenario` reads the pre-dispatch shadow |
| the named blit's aspect plan back on the frontend objects | `CopyImageLayeredScenario`, `LayeredAttachmentShapeScenario` |
| the FBO twin's per-point version memo kept as a second gate | `CrossFrameBufferScenario` / `P4aSeamAuditScenario` F-3 (a re-storaged attached RBO) - it must stay green WITHOUT the memo, which is the proof the record hash is sufficient |
| `record.Color[]` used as membership while a point above the wire width exists | `FramebufferEmitTest.AnAttachmentPointAboveTheWireWidthIsRefusedNotTruncated` (`:558`) is the existing guard; add its twin on the SERVER side |

Exit gate: unit (`MG_Test/Pipe/FramebufferEmitTest.cpp`, `MG_Test/Pipe/ImageEmitTest.cpp`) +
`integration-split` + `integration-split-strict` green with the framebuffer/sampler bits set, and
the seam greps (`DirectGLES.cpp:3086-3106`, `:2607-2610`, `:2636-2638`) silent.

## 7. Risks and what I could not settle

1. **The image sweep's gate is the one genuinely frontend-owned thing here.** `:2890-2901` argues
   a server-owned epoch is bumped AFTER the gate has declined, because a texture bound only to an
   image unit is re-minted inside the sweep. That survives the re-key only if the new gate mixes
   the applier's texture content/params serials (`PipeApply.h:696-700`), never a backend re-mint
   counter. I could not prove it without S2's serial design. **Ruling needed.**
2. **A8 is still open and P5e makes it load-bearing.** The image walk unions the record window
   with `g_imageUnitHighWaterMark` (`:2811-2821`) because nothing pins the window's membership.
   Under run-ahead the mark is still server-owned, so the union survives - but if a later package
   narrows the walk to the window, an image established eagerly and never revisited is stranded
   on a deleted driver name. Keep the union and say so in the contract.
3. **`Complete` and the frontend's `CheckCompleteness`.** `MGPFramebufferState::Complete`
   (`MGPipeTypes.h:762-770`) is deliberately the frontend-only answer.
   `glCheckFramebufferStatus` still answers from the frontend on the client, so nothing on the
   apply path reads it - but any P5e code tempted to use `Complete` as a "may I draw" gate would
   be reading the weaker answer. Do not.
4. **`ForceBindCurrentFBO`'s pre-handle stamps** (`:4734-4738`) are also read by the legacy
   `SyncCurrentFBO` arm; deleting them under a live wire leaves that memo permanently invalid in
   a mixed build. Skip the stamp and the legacy arm together under the transport test, as
   `:4750-4752` already does for the serial memo.
5. **Default-framebuffer geometry.** `FillGeometry` (`FramebufferEmit.h:610-638`) takes the extent
   from the first non-empty attachment, `BackLeft` for the default one (`:531-532`). I did not
   verify that an EGL surface resize re-emits the record: the shutter is
   `m_anyAttachmentGeneration` + the object version (`ARCHITECTURE.md:165`), and a resize moving
   neither would be suppressed. **S6/S7 should check** - under run-ahead there is no other
   extent source.
6. **`SyncReadFramebufferTextureAttachments` is deliberately NOT gated on the framebuffer
   subsystem bit** (`:2227-2230`), so its retirement changes behaviour in BOTH arms of the object
   A/B. The A/B baseline moves with it; tell the integrator.
7. **Whether a blit is a run-ahead op is unsettled.** Both arms end in `ForceBindCurrentFBO` x2,
   which restores state that is all server-owned once the design above lands, so I see no reason
   it must wait. The readbacks (`GetReadColorAttachment`, `IsFixedPointFallbackReadAttachment`)
   are S6's: waited today by their reply slot, and their frontend reads should stay refused
   rather than migrated.
