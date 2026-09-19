# P5e audit — apply-thread reads of client-owned memory on the DirectGLES inproc draw path

Repo head `2fde7034` (`feat/disaggregated`). Read-only static audit; no build, no run.

Legend — paths: `D` = `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp`, `M` = `MobileGL/MG_Backend/DirectGLES/Managers.cpp`, `ST` = `MobileGL/MG_Backend/DirectGLES/SlotTables.h`, `PA` = `MobileGL/MG_Remote/Server/PipeApplier.cpp`, `AP` = `MobileGL/MG_Pipe/PipeApply.cpp/.h`, `MT` = `MobileGL/MG_Pipe/MGPipeTypes.h`, `PI` = `MobileGL/MG_Backend/MGPipe/PipeInputs.h`, `MD` = `MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp`.
Kinds: `LIVE` = dereference of a client object/field; `PROBE` = `GetLifetimeId()` + `MGPipeSlots().FindByLifetimeId`; `MINT` = `MGPipeSlots().Acquire(...)`; `ROW-O` = `gPipeInputs` BARRIER_PULLED row holding a raw pointer/SharedPtr into the live `GLContext`; `ROW-V` = BARRIER_PULLED copied-value row.
Arm: `rec` = runs when `MG_Config::Transport != Monolith` with all `MOBILEGL_PIPE_PUSH` bits; `leg` = legacy/monolith arm only; `both` = no transport branch.
Freq: `draw` = every draw/dispatch; `memo` = only on memo miss / clean-probe open; `rare` = config change / error / special-feature path.
"Dup" = pushed record or `MGPipeApplierState` field (`AP:PipeApply.h`) already carrying the same information; `none` = nothing pushed covers it.

Sink map (all sinks are in `PA`; bodies read no `MG_State` — every client read is one level down in the callee): `OnDrawVbo` PA:430 → the 19 draw entries (`D:6705`-`D:7295`), all of which call `PrepareForDraw` (D:4762) first; `OnClear` PA:168 → `Clear` D:6180 / `ClearBufferfi` D:10045 / `ClearBufferfv` D:10091 / `ClearBufferiv` D:10104 / `ClearBufferuiv` D:10120; `OnBlit` PA:232 → `BlitFramebuffer` D:8322; `OnLaunchGrid` PA:670 → `DispatchCompute` D:9610 / `DispatchComputeIndirect` D:9618 → `PrepareForCompute` D:6118; `OnGenerateMipmap` PA:915 → `GenerateMipmap` D:9559; `OnCopyFramebufferToTexture` PA:931 → `CopyTexImage2D` D:9256 / `CopyTexSubImage2D` D:9373. `AP` (`PipeApply.cpp`, 38 `MGPipeApply*` entries) contains **zero** client-owned reads — no `pGLContext`, no `MGPipeSlots()` probe, no `MGB_CTX->`.

## VAO / vertex

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 1 | D:4770 | `PrepareForDraw` | `MGB_CTX->GetBoundVertexArray()` — SharedPtr to client VAO | ROW-O | draw | both | `BoundVertexElements` (AP:594) |
| 2 | D:1617 → ST:436-446 | `VertexArrayImpl::ResolveVaoTwin` | `Find(vao.get())` → `HandleOf` → `MGPipeSlots().FindByLifetimeId` | PROBE | draw (memo hit at ST:444 skips) | rec (slot arm) | `BoundVertexElements` |
| 3 | D:1618 → ST:247-248 | `ResolveVaoTwin` | `GetOrCreate(vao)` → `MGPipeSlots().Acquire` | MINT | rare | rec | none (first-touch only) |
| 4 | D:1626, D:1629-1634 | `ResolveVaoTwin` | legacy memo/registry keyed on frontend heap address + `stateRef` weak_ptr | LIVE | draw | leg | none |
| 5 | D:4775 | `PrepareForDraw` | `currentVAO->GetConfigVersion()` | LIVE | draw | both | none (server has `VertexBuffersSerial`/`ContentSerial` only) |
| 6 | D:1653 → M:4881 | `SyncCurrentVAO` | client VAO handed to `SyncToBackend` | LIVE handoff | draw | both; handle arm diverts to `SyncToBackendFromApplier` M:5160 (zero client reads) | `VertexElementsCso` record (AP:487) |
| 7 | M:4919-5104 | `BackendVertexArrayObject::SyncToBackend` | `GetConfigVersion`, index slot, `GetAllAttributeVersions`/`GetAllAttributes`, per-attrib `Enabled/Divisor/IsBgra/Offset/Stride/Type/...` | LIVE | draw | leg only | `MGPVertexAttribWire`/`MGPVertexBindingPointWire` (MT:389-400) |
| 8 | M:5479-5544 | `SyncFloat64AttributeAsFloat32` | `attrib.*`, `bufferObject->MappedData()` — client shadow bytes; `NarrowDoubleStreamToFloat32` reads them | LIVE | rare | leg | none |
| 9 | D:1669 | `SyncCurrentVertexAttributeValues` | `GetBoundVertexArray()` | ROW-O | draw | both | `BoundVertexElements` |
| 10 | D:1683, D:1688, D:1700 | `SyncCurrentVertexAttributeValues` | `vao->GetConfigVersion()`, `vao->GetAttribute(location).Enabled`; `GetCurrentVertexAttribute(location)` | LIVE + ROW-V | draw / memo | both | `SetVertexAttribDefaults` (AP:1600) for :1700; none for :1683/:1688 |
| 11 | D:6449, D:6451, D:6459 | `BoundElementArrayBuffer` | `GetBoundVertexArray()`, VAO index slot `GetBoundObject()`, `EnsureBufferResource` | ROW-O + LIVE + PROBE | per indexed draw | both | `IndexBuffer` (AP:620) |
| 12 | D:6724, D:6731 | `DrawArrays` | `GetBoundVertexArray()` + registry `Find(currentVAO.get())` → the one true per-draw `MGPipeSlots().FindByLifetimeId` from the apply thread | ROW-O + PROBE | every DrawArrays | both | `BoundVertexElements` |
| 13 | D:6768, D:6777 | `MultiDrawArrays` | same pair, per sub-draw | ROW-O + PROBE | per sub-draw | both | `BoundVertexElements` |
| 14 | M:5372-5449 | `SyncClientSideAttributesForDrawArrays` | `GetAllAttributes()`, `attrib.Offset` reinterpreted as client-memory pointer, client array bytes uploaded | LIVE | VAOs with client-side arrays | both | none (client-memory vertex data) |
| 15 | MD:96 | `BoundIndexBuffer` | `MGB_CTX->GetBoundVertexArray()` | ROW-O | per multi-draw element call | both | `BoundVertexElements` |

## Buffers

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 16 | D:897 | `SyncNeccessaryBuffers` | `currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject()` | LIVE | every indexed draw | both | `IndexBuffer` (AP:620) |
| 17 | D:907 → M:2922-2941 | `SyncNeccessaryBuffers` | `HandleOfBuffer(possibleIBO.get())` → `MGPipeSlots().FindByLifetimeId` | PROBE | every indexed draw | rec | `IndexBuffer.Res` |
| 18 | D:911 → M:3109-3151 | `IsBufferDrawCleanByHandle` | server-only under split; `frontend->IsMapped()` at M:3147 is monolith-only | LIVE (monolith) | memo | both | resource record `Serial`/`HasLiveHostWrites` |
| 19 | D:915, D:932 | `SyncNeccessaryBuffers` | `EnsureBufferResource(possibleIBO)` | PROBE | rare | rec | `IndexBuffer.Res` |
| 20 | D:926, D:928, D:934 | `SyncNeccessaryBuffers` | `memo->iboFrontend == possibleIBO.get()` — client address compare | LIVE | every indexed draw | leg | `IndexBuffer.Res` |
| 21 | D:842 | `SyncNeccessaryBuffers` | `currentVAOObject->GetAttribute(entry.attribIndex).Buffer` | LIVE | rare (clean-probe fail) | both | `VertexBuffers` window (AP:598-600) |
| 22 | D:857-861 | `SyncNeccessaryBuffers` | `GetAllAttributes()`, per-attrib `Enabled`/`Buffer` | LIVE | memo | leg-memo path | `VertexElementsCso` + `VertexBuffers` |
| 23 | D:864, D:874 | `SyncNeccessaryBuffers` | stored raw client `BufferObject*`, `EnsureBufferResource(bufferObject)` | LIVE + PROBE | memo | both | buffer handle in `VertexBuffers` |
| 24 | D:736, D:746-753, D:763, D:766-770 | `SyncVaoAttributeBuffersByHandle` | memo-miss full walk: `GetAllAttributes`, `attrib.Buffer`, stores client pointer + `HandleOfBuffer` | LIVE + PROBE | memo | rec | `VertexBuffers`/`VertexElementsCso` |
| 25 | M:3158 | `IsBufferDrawClean` | `HandleOfBuffer(frontend)` | PROBE | every probe | rec | resource record |
| 26 | M:3163, M:3175, M:3179, M:3180 | `IsBufferDrawClean` | `frontend->GetBackendResource/IsMapped/GetSize/GetChangeSerial` | LIVE | every probe | leg | resource record |
| 27 | M:3444 | `EnsureBufferResource` | `HandleOfBuffer(bufferObject.get())` | PROBE | per bound VBO/IBO/SSBO/UBO/atomic/XFB/PBO per draw | rec | the handle in each pushing record |
| 28 | M:3452 | `EnsureBufferResource` | `bufferObject->GetExternalIndex()` | LIVE | error arm only | both | none |
| 29 | M:3221, M:3340, M:3393 | `EnsureBufferResourceForHandle` | `bufferObject->MappedData()/SyncPersistentMappedRange()/HasDefinedContent()` — **monolith-only**; under an active transport this function is client-free | LIVE | — | leg | `ServerStaged()` shadow + resource record |
| 30 | M:2953 | `MarkBufferGpuWritten` | `HandleOfBuffer(bufferObject.get())` before the reverse announcement | PROBE | per SSBO per draw | rec | none (reverse direction) |
| 31 | M:2950 | `MarkBufferGpuWritten` | `bufferObject->MarkGpuWritten()` — client **write** | LIVE | per SSBO per draw | leg | none |
| 32 | M:3083 | `GetBufferResource` | `bufferObject->GetBackendResource()` (called per UBO per draw from D:5472) | LIVE | draw | both | none |
| 33 | D:962, D:5987, D:6077, D:6511, D:6930-6931, D:7082-7083 | indirect/restart helpers | client `GetBufferBindingSlot(DrawIndirect/Parameter)` + `MappedData()` — all behind `Transport == Monolith`; record arms (D:373, D:951-956, D:6534-6536, D:6889-6927, D:7043-7079) read applier handles + server staged shadows only | ROW-O + LIVE | indirect draws | leg | `VerbIndirectBuffer`/`VerbIndirectParameterBuffer` (AP:742-743) |
| 34 | M:5364-5468 | see #14 | client-side attribute bytes | LIVE | rare | both | none |

## Textures / samplers

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 35 | D:1899-1913 | `UnitBindingsUnchanged` | `GetTextureUnitObject(unit)` (raw `TextureUnit*` into client array), `GetAllBindingSlots()`, `GetBoundObject()->GetLifetimeId()`, sampler `GetLifetimeId()` | ROW-O + LIVE | memo | rec walk arm | `BoundSamplerViews`/`BoundSamplerStates` windows (AP:673-681) |
| 36 | D:1871-1885 | `CaptureUnitBindings` | same tables, captures lifetime ids | ROW-O + LIVE | memo | rec | same |
| 37 | D:1934-1940, D:1947-1953 | legacy twins of the above | same + `WeakPtr` owner checks | LIVE | memo | leg | same |
| 38 | D:2346 → D:2136, D:2356, D:2359-2361 | `SyncNeccessaryTextures` served path | `entry.slot->get()` — borrowed pointer **into a client binding slot**; `*entry.slot` ITextureObject handed to twin sync | LIVE | **every draw, per list entry** | both | `BoundSamplerViews` |
| 39 | D:2374-2376 | `SyncNeccessaryTextures` rebuild | `GetTextureUnitObject(index)`, slots, `GetBoundObject()` | ROW-O | memo | both | `BoundSamplerViews` |
| 40 | D:2379-2381 | `SyncNeccessaryTextures` | `IsUndefinedDefaultTexture(...)`, probe + `SyncTextureObjectToBackend` mint | LIVE + PROBE/MINT | memo | both | texture resource handle |
| 41 | D:2405-2406, D:2408-2409 | `SyncNeccessaryTextures` draw-FBO half | `GetFramebufferBindingSlotChecked(Draw).GetBoundObject()`, slot `GetVersion()`, `currentFBO->GetObjectVersion()` | ROW-O + LIVE | **every draw** | both | `BoundFramebuffer[Draw]` + `FramebufferRecord` (AP:646,524) |
| 42 | D:2438-2447 | `SyncNeccessaryTextures` | replay of memoised `*entry.slot` through `IsDrawSyncClean` | LIVE | memo | both | `BoundSamplerViews` |
| 43 | D:2450-2459 | `SyncNeccessaryTextures` | `currentFBO->GetAllAttachmentObjects()`, `attachment.GetTexture()` + mint | LIVE + MINT | memo | both | `MGPFramebufferState` (MT:750) |
| 44 | D:2246-2247, D:2252, D:2258-2259 | `SyncReadFramebufferTextureAttachments` | read-FBO slot `GetBoundObject()`, pointer compare, versions | ROW-O + LIVE | every draw (PUSH) | both | `BoundFramebuffer[Read]` |
| 45 | D:2285, D:2290-2296 | same | `PairingsIntact` + replay to `SyncTextureParamsToBackend`/`SyncBuiltinSamplerToBackend`/`SyncMipmapsToBackend` | LIVE | memo | both | sampler-view/params records |
| 46 | D:2303-2308 | same | `readFBO->GetAllAttachmentObjects()`, `GetTexture()` + mint | LIVE + MINT | memo | both | `MGPFramebufferState` |
| 47 | D:1736, D:1738 | `SyncTextureObjectToBackend` | `g_backendTextureObjects.Find(textureObject.get())` / `GetOrCreate(textureObject)` | PROBE + MINT | per texture sync | both | texture resource handle |
| 48 | D:1781, D:1790, D:1797-1799 | `SyncTextureObjectToBackend` | re-resolution tail Find/GetOrCreate | PROBE + MINT | per call | both | same |
| 49 | D:1758, D:1760-1762, D:1771-1772 | `SyncTextureObjectToBackend` | client SharedPtr handed into `BackendTextureObject::Sync*` (deep reads at M:5816/7049/8554/8802) | LIVE | per sync | both | pushed params/mipmap records |
| 50 | D:4861, D:4876-4877 | `ResolveAndBindUnitTextures` | `GetTextureUnitObject(unit)`, `GetAllBindingSlots()`, `GetBoundObject()` | ROW-O | memo | both | `BoundSamplerViews` |
| 51 | D:4883, D:4885, D:4888 | same | `IsUndefinedDefaultTexture`, `GetExternalIndex()`, `GetTarget()` | LIVE | memo | both | none |
| 52 | D:4914-4918 | same | `textureUnit.GetSamplerObject()` ×2, `textureObject->GetSamplerObject()`, `SamplesAsIncompleteTexture` | LIVE | memo | both | `BoundSamplerStates` |
| 53 | D:4928, D:4945-4946 | same | `Find(textureObject.get())`; clear pass reads `bindingSlot.GetTarget()` | PROBE + LIVE | memo | both | view handle |
| 54 | D:5157, D:5159-5160 | `BindCurrentUnitSamplers` legacy arm | `GetTextureUnitObject(unit).GetSamplerObject()` | ROW-O + LIVE | memo | leg | `BoundSamplerStates` |
| 55 | D:4999, D:5003, D:5015, D:5018 | `ResolveUnitSamplerBackend` | `HandleOf(samplerObject.get())` probe, `OwnerEquals` weak memo, `Find` | PROBE + LIVE | per non-CSO sampler per draw | rec | `BoundSamplerStates` |
| 56 | D:5626 | `BindCurrentProgramWithResources` | `g_backendSamplerObjects.GetOrCreate(samplerObject)` | MINT | rare | leg-in-rec fallback | `BoundSamplerStates` |
| 57 | D:5633-5637, D:5639 | same | `SyncToBackend(samplerObject, ...)` — client SamplerObject | LIVE | memo | both | `SamplerCsos` record (AP:501) |
| 58 | D:5573, D:5577-5580, D:5591-5595 | same | `GetLodBias()`, binding-slot reads, `texture2D->GetSamplerObject()/GetFormat()` | LIVE | memo | both | none (LOD-bias/depth-fetch fixups) |
| 59 | D:9564-9566, D:9568-9570 | `GenerateMipmap` | `GetTextureUnitObject(unitIndex)` (client array), slot `GetBoundObject()`, `texture->GetFormat()` | ROW-O + LIVE | every mipmap | both (rec arm too) | `MGPMipPlan.Res` (MT:1382) covers identity only |
| 60 | D:9162, D:9217 | `GenerateDepth/ColorTexture2DMipmap` | `HandleOf(texture.get())` | PROBE | rare | rec | `VerbMipRes` (AP:739) |
| 61 | D:9484-9548 | `GenerateThreeChannelFloatMipmapOnCpu` | `GetFormat`, `AsMipmapTexture`, `GetMipmapLevelCount/TexelSize`, `MapMipmapData` — client shadow bytes read+written, `MarkStorageDirty` | LIVE | rare | both | none |
| 62 | D:9580, D:9584-9585, D:9591, D:9603 | `GenerateMipmap` | `texture->GetFormat()/GetTarget()` | LIVE | every mipmap | both | resource record `Desc` |
| 63 | D:8839, D:8861-8897 | `ScopedDetachedTextureFramebufferAttachments` | `Find(texture.get())` probe; **unmemoised walk of every live twin's client FBO**: `GetAllAttachmentObjects`, `GetTexture`, `GetTextureUploadTarget`, `IsLayered`, `GetTextureLevel` | PROBE + LIVE | rare (per mipmap) | both | none |
| 64 | D:8472 | `UpdateTextureBindingAtTarget` | `GetTextureUnitObject(unit)` | ROW-O | every copy-tex | both | `MGPCopyFromFramebuffer.Dst` (MT:1639) covers the handle |
| 65 | D:9302-9312, D:9416+ | `CopyTexImage2D`/`CopyTexSubImage2D` legacy arm | unit binding slot `GetBoundObject()`, `Find(textureObject.get())`, `GetFormat()` | ROW-O + LIVE + PROBE | every copy-tex | leg | `VerbCopyTexDst` (AP:737) |
| 66 | M:5884, M:7089, M:8483, M:8719 | texture `HandleOf` probes | `g_backendTextureObjects.HandleOf` → allocator probe | PROBE | memo/feature paths | rec | resource handle |
| 67 | M:9261, M:9318 | `SyncAttachmentObject` | `GetOrCreate(stateTextureObject)` / `GetOrCreate(renderbufferObject)` | MINT | FBO attach walk | leg | attachment record |

## Framebuffers / attachments

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 68 | D:3019, D:3021, D:3032 | `FramebufferRecordMatchesBinding` (from `SyncCurrentFBO` rec arm D:3112-3113) | `GetFramebufferBindingSlotChecked(target).GetBoundObject()` — **client slot, every record-arm draw, per target**; `pDefaultFramebufferInfo->defaultFBO`; `HandleOf(bound.get())` at D:3032 is monolith-gated | ROW-O + LIVE | draw | rec | `BoundFramebuffer` + `FramebufferSerial` (AP:646,652) |
| 69 | D:3178-3179, D:3186 | `SyncCurrentFBO` rec arm | `slot.GetBoundObject()` on serial miss; `currentFBO` handoff | ROW-O + LIVE | memo | rec | `FramebufferRecords` (AP:524) |
| 70 | D:3227-3228 | `SyncCurrentFBO` rec arm | `NoteStateForHandle(record.Fbo, currentFBO)` — **stores the client FBO into the twin table** | LIVE | per handle sync | rec | — |
| 71 | D:3269 → M:10044 | `SyncCurrentFBO` rec arm | `backendObj->SyncToBackend(currentFBO, target)` walks the frontend FBO | LIVE | memo | rec | `MGPFramebufferState` |
| 72 | D:3305-3306, D:3317-3319, D:3336, D:3349, D:3358, D:3360, D:3367-3369, D:3373 | `SyncCurrentFBO` legacy arm | slot + `GetBoundObject()`, versions, `defaultFBO`, `Find`/`GetOrCreate` mint, `SyncReadBufferToBackend`, `SyncToBackend` | ROW-O + LIVE + PROBE/MINT | draw | leg | same records |
| 73 | D:4568, D:4580-4581, D:4592, D:4600, D:4602 | `BindCurrentFBO` legacy arm (record arm D:4528-4565 is client-free: applier record + `FindByHandle`) | slot, `GetBoundObject()`, `defaultFBO`, `Find`, weak-owner memo | ROW-O + LIVE + PROBE | draw | leg | `BoundFramebufferRecord` |
| 74 | D:4720-4721, D:4734-4736 | `ForceBindCurrentFBO` | `GetFramebufferBindingSlotChecked(target).GetBoundObject()` + versions + `fbo.get()` — **unconditional on both arms**; memoises the frontend pointer at D:4736 | ROW-O + LIVE | every call (clear/blit/DSA) | both | `BoundFramebuffer` |
| 75 | D:4630, D:4646-4647, D:4655 | `SyncAndBindFramebufferObject` | `defaultFBO` compare, `Find`/`GetOrCreate` mint, `SyncToBackend` | LIVE + PROBE/MINT | rare | both | `MGPFramebufferState` |
| 76 | D:4700, D:4705 | `SyncAndBindFramebufferByHandle` | `StateForHandle(fbo)` returns the **recorded client FBO** (server-side weak note from #70); `SyncToBackend(stateObj, target)` walks it | LIVE | rare (named blit) | rec | handle itself is server |
| 77 | D:8357-8359 | `BlitFramebuffer` named arm | `FramebufferImpl::pDefaultFramebufferInfo->defaultFBO` | LIVE | rare | rec | none |
| 78 | D:8407-8410 | `BlitFramebuffer` bound arm | `GetFramebufferBindingSlot(Read/Draw).GetBoundObject()` ×2 — **both client FBOs, every bound blit** | ROW-O + LIVE | every bound blit | both | none (`MGPBlit` null-handle form carries no FBO identity) |
| 79 | D:8210-8302 | `BlitLayeredDestinationAspects` | `GetDrawBuffers()`, `GetReadBuffer()`, `GetAttachment(...)`, `IsLayered/GetTextureLevel/GetTextureLayer`, `GetTexture()`, `GetFormat()/GetSamples()`, `GetTarget()` + `SyncTextureObjectToBackend` probe/mint | LIVE + PROBE/MINT | every blit | both | none (attachment detail not in `MGPBlit`) |
| 80 | D:6284, D:6288-6291 | `Clear` | `GetFramebufferBindingSlot(Draw).GetBoundObject()` + `GetExternalIndex/GetObjectVersion/GetDrawBuffers()` — **debug-log only** | ROW-O + LIVE | dbg | both | — |
| 81 | M:10054, M:10119, M:10121, M:10216, M:10283-10284 | `BackendFramebufferObject::SyncToBackend` | `stateFBOObject->GetExternalIndex/GetDrawBuffers/GetAttachment/GetAllAttachmentObjects/GetAllFramebufferAttachmentVersions` | LIVE | per FBO sync | both (entered from #71/#75/#76) | `MGPFramebufferState` |
| 82 | M:9938, M:9872, M:9886 | `SyncReadBufferToBackend` | `GetReadBuffer()`, `GetExternalIndex()` | LIVE | per FBO sync | both | `MGPFramebufferState` |
| 83 | M:9862, M:10101 | same / `SyncToBackend` | `HandleOf(stateFBOObject)` — monolith branch only | PROBE | per sync | leg | FBO handle |
| 84 | D:4386-4399 | `SyncCurrentProgram` (FBO half) | `GetFramebufferBindingSlotChecked(Draw)`, `slot.GetBoundObject()/GetVersion()`, `drawFBO->GetObjectVersion()`, `drawFBO->GetDrawBuffers()` | ROW-O + LIVE | draw (memo-guarded at D:4390) | both | `BoundFramebufferRecord(Draw)` + `MGPFramebufferState.DrawBuffers` |

## Images

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 85 | D:2650-2653 | `SyncImageTextureBinding` | `GetImageTextureBinding(unit)` (client array), `binding.Texture`, `IsWritableImageBufferTexture` (`Texture->GetStorageType()`) | ROW-O + LIVE | memo/rare | both | `BoundShaderImages` window (AP:683-686) |
| 86 | D:2660, D:2671 | same | `SyncTextureObjectToBackend(imageBinding.Texture, true)` probe/mint; `imageBinding.Texture.get()` identity via `HandleOf` D:2634 | PROBE/MINT + LIVE | memo | both | `MGPImageView` |
| 87 | D:2672-2675 | same | legacy arm reads `imageBinding.Layered/Layer/Level/Format` (rec arm reads the `MGPImageView` record) | LIVE | memo | leg | `MGPImageView` |
| 88 | D:2682, D:2723-2724, D:2736, D:2744 | same | `GetTarget()`, `GetFormat()`, `imageBinding.Access` | LIVE | memo | both | `MGPImageView` |
| 89 | D:2767, D:2773-2774 | `MarkWritableImageBufferTexturesGpuWritten` | `GetImageTextureBinding(unit)`, downcast to `TextureObjectBuffer*`, `GetBufferBindingSlot().GetBoundObject()` | ROW-O + LIVE | every draw when writable image-buffer units exist | both | `BoundShaderImages` |
| 90 | M:10849 | `BoundImageUnitFormat` (via `ImageUnitFormatsStillMatch`, D:4472) | `MGB_CTX->GetImageTextureBinding(unit).Format` | ROW-O | programs with format-less image units | both | `MGPImageView.Format` |

## Programs / shader state

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 91 | D:4781 | `PrepareForDraw` | `MGB_CTX->GetProgramForDraw()` — SharedPtr to client ProgramObject | ROW-O | draw | both | `DrawProgram` (AP:691) |
| 92 | D:6126 | `PrepareForCompute` | `MGB_CTX->GetProgramForDispatch()` | ROW-O | every dispatch | both | `DispatchProgram` (AP:692) |
| 93 | D:4339 | `SyncCurrentProgram` | `currentProgram->GetLinkStatus()/GetSpirvStatus()` | LIVE | draw | both | `ShaderCsos` record (AP:503) |
| 94 | D:4415-4416 | `SyncCurrentProgram` | `g_backendProgramObjects.Find(currentProgram.get())` → `HandleOf` probe; `GetOrCreate(currentProgram)` mint | PROBE + MINT | draw | rec | `DrawProgram` handle |
| 95 | D:4425-4429 | `SyncCurrentProgram` | `g_programTwinLookupMemo.Lookup(currentProgram)`; address-keyed `Find`/`GetOrCreate` mint | LIVE + MINT | draw | leg | `DrawProgram` |
| 96 | D:4457-4459 | `SyncCurrentProgram` | `currentProgram->GetLinkVersion()/GetImageUnitVersion()` | LIVE | draw | both | `ShaderCso` serials |
| 97 | D:4463-4464 → M:10821 | `SyncCurrentProgram` | `ComputeShaderStorageBlockBindingSignature(*currentProgram)` → `ProgramObject::GetShaderStorageBlockBindingOverrides()` | LIVE | draw (early-out on empty map) | both | none (overrides not pushed) |
| 98 | D:4507 | `SyncCurrentProgram` | `twin->SyncToBackend(currentProgram)` — full client ProgramObject handoff | LIVE | memo | both | `CreateShaderState` record |
| 99 | D:4242, D:4274-4313 | `ResolveGlobalConstantsRecord` | `HandleOf(program)` probe; `program->GetUBOSize()` compares | PROBE + LIVE | every draw with a default UBO | rec | `SetGlobalConstants` record (AP:3010) |
| 100 | D:5257, D:5279-5282 | `BindCurrentTextures` | `programKey = currentProgram.get()`; `GetLifetimeId()`, `GetBackendStateVersion()`, `GetLinkStatus()` | LIVE | draw | both | `DrawProgram` |
| 101 | D:5320, D:5333, D:5337 | `BindCurrentProgramWithResources` | `GetLinkStatus()/GetSpirvStatus()`, `currentProgram.get()`, `Find` probe | LIVE + PROBE | draw | both | `DrawProgram` |
| 102 | D:5349, D:5372-5373 | same | `GetUBOSize()`, `GetUBOContentVersion()` | LIVE | every draw with UBO | both | `ShaderCso.Desc.GlobalUboSize` + `GlobalConstantsSerial` |
| 103 | D:5380, D:5382, D:5425-5430 | same | `currentProgram->MapUBO()` — client shadow bytes (legacy fallback of the ring path); `GetUBOSize()` for upload | LIVE | ring-miss only | both | `GlobalConstants` bytes |
| 104 | D:5463 | same | `currentProgram->GetUniformBlockBinding(i)` | LIVE | per UBO per draw | both | `ShaderCso.Desc` |
| 105 | D:5522, D:5546 | same | `GetBackendStateVersion()`, `GetUniformSamplerOrImageUnitIndex(location)` | LIVE | memo | both | `ShaderCso` records |
| 106 | D:1672, D:1701 | `SyncCurrentVertexAttributeValues` | `program->GetActiveAttributeLocationMask()`, `program->GetAttribType(location)` | LIVE | draw | both | none |
| 107 | D:5679, D:5683-5692 | `GetCurrentBackendProgram` | `GetProgramForDraw()` + stash-miss `Find(currentProgram.get())` probe | ROW-O + PROBE | per indirect sub-draw | both | `DrawProgram` |
| 108 | D:5733-5740 | `CurrentProgramMayNeedPerSubDrawBuiltins` | `currentProgram->GetLinkVersion()` | LIVE | per sub-draw query | both | `ShaderCso` serial |
| 109 | D:6157, D:10023, D:6152, D:10022 | `GetBackendProgramId` / `ShaderStorageBlockBinding` | `GetProgramObject(...)`, `ValidateProgramName(...)` — frontend name-table probe | LIVE | API-driven, not per draw | both | none |
| 110 | M:12954 | `BackendProgramObjectImpl::SyncToBackend` | `HandleOf(stateProgramObject)` | PROBE | per program sync/relink | rec | `ShaderCso` handle |

## Binding-point tables

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 111 | D:510-511 | `SyncBufferBindingPoints` | `GetBufferBindingPoint(target, i)` — raw pointer into the live GLContext array; `point.GetBoundObject()` SharedPtr to client BufferObject | ROW-O + LIVE | per touched SSBO per draw; per UBO+SSBO per dispatch | **both — no arm switch** | `MGPShaderBuffers` record (MT:910) |
| 112 | D:517, D:524, D:526, D:529-530 | same | `EnsureBufferResource(obj)` probe; `point.GetRange()`; `obj->GetSize()` | PROBE + LIVE | per bound point | both | buffer handle + range in record |
| 113 | D:602, D:605 | `MarkShaderStorageBuffersGpuWritten` | touched count; `GetBufferBindingPoint(ShaderStorage,i).GetBoundObject()` | ROW-V + ROW-O/LIVE | **every draw** | both | `MGPShaderBuffers` |
| 114 | D:569-570, D:575, D:583, D:585, D:588-589 | `SyncTransformFeedbackBindingPoints` | TF binding point, `GetBoundObject()`, ensure probe, `GetRange()`, `GetSize()` | ROW-O + LIVE + PROBE | rare (capture span open) | both | none |
| 115 | D:621, D:628, D:630, D:636, D:642-648, D:658-660 | `SyncAtomicCounterBuffers` | count; `GetBufferBindingPoint(AtomicCounter, ...)`, `GetBoundObject()`, ensure, `GetRange()`, `GetSize()`, `MarkBufferGpuWritten` | ROW-V/O + LIVE + PROBE | rare (programs with atomic counters) | both | none |
| 116 | D:669, D:675 | `SyncBoundBuffer` | `GetBufferBindingSlot(target).GetBoundObject()`, ensure probe | ROW-O + LIVE + PROBE | monolith indirect paths | leg | `VerbIndirectBuffer`/`VerbDispatchIndirectBuffer` |
| 117 | D:5464-5466 | `BindCurrentProgramWithResources` | `GetBufferBindingPoint(Uniform, binding)`, `GetBoundObject()`, `GetRange()` | ROW-O + LIVE | per UBO per draw | both | none (UBO bindings not pushed as records) |
| 118 | D:5472-5479 | same | `GetBufferResource(bufferObj.get())`, `IsBufferDrawClean`, `EnsureBufferResource(bufferObj)` | LIVE + PROBE | per UBO (epoch-stamped) | both | buffer handle |

## XFB

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 119 | D:1387 | `StartPendingTransformFeedback` | `MGB_CTX->GetTransformFeedbackProgram()` | ROW-O | span start | both | none (`TransformFeedback*` context values are pushed, the program row is not) |
| 120 | D:1405, D:1407-1415, D:1417 | same | `program->GetTransformFeedbackBufferCount()`, TF binding points, `GetBoundObject()`, ensure probe, `GetRange()`, `GetSize()`; pushes the client `SharedPtr<BufferObject>` into `xfb.targets` | LIVE + ROW-O + PROBE | span start | both | none |
| 121 | D:1427, D:1434, D:1439, D:1463, D:1497 | same | `NeedsScatteredTransformFeedbackCapture()`, `GetTransformFeedbackStride/PackedStride/BufferMode()` | LIVE | span start | both | none |
| 122 | D:1138, D:1295 | `ReadbackCapturedRanges` / `ScatterCapturedRecords` | `HandleOfBuffer(target.buffer.get())` | PROBE | span end | rec | buffer handle (carried in `xfb.targets`, but stored as SharedPtr) |
| 123 | D:1174, D:1191, D:1310, D:1342 | same | `IsBackendPersistentMapped()`, `WritebackFromBackend(...)`, `MappedData()` | LIVE | span end | leg | none |
| 124 | D:1247, D:1282, D:1312 | `ScatterCapturedRecords` | `program->GetTransformFeedbackPackedStride/Stride/Varyings()` | LIVE | span end | both | none |

## Misc

| # | site | function | what is read | kind | freq | arm | dup |
|---|---|---|---|---|---|---|---|
| 125 | PA:742-743 | `ServerVerbSink::OnResourceCopyRegion` | `gPipeInputs.GetTextureObject(SrcGlName/DstGlName)` — BARRIER_PULLED sticky-forward frontend name-table probe returning `SharedPtr<ITextureObject>` (`FieldOwnership.def:151`) | ROW-O | per `CopyImageSubData` | both | `MGPCopyRegion.Src/Dst` handles (MT:1302); the GL names are the declared P5b debt |
| 126 | PA:591 | `OnDrawVbo` | `MGPipeHostBytes(*userIndices)` — SEG_STAGE bytes under split; client pointer under monolith | ROW-V (split: server bytes) | `kDrawHasUserIndices` arm only | both | record tail |
| 127 | D:11761, D:11765, D:12192, D:12196, D:10232, D:11214, D:11451 | readback helpers (`ReadPixels`, `GetTexImage`, `StoreReadbackRowsToClient`, ...) | pack-PBO `GetBufferBindingSlot` + ensure | ROW-O + PROBE | readback paths | both | `MGPReadbackInfo` (MT:1388) |
| 128 | MD:909, MD:930 | `MultiDrawImpl::DrawElementsBatch` | `BoundIndexBuffer()` (client EBO), `BoundIndexBufferId()` | LIVE | per multi-draw | both | `IndexBuffer` (AP:620) |

## (a) Scope construction sites

`MGPipeFrontendKeyedRegistryScope` (exempts `MGPipeSlots()` from the apply-thread `Fatal{RoleViolation}`, `MobileGL/MG_Impl/Pipe/SlotAllocator.cpp:29`). Production sites: D:1609 (`ResolveVaoTwin`, VAO twin resolve, every draw) · D:1734 (`SyncTextureObjectToBackend`) · D:2588 (`ResolveShaderImageRecord`, image-seam identity) · D:3303 (`SyncCurrentFBO`, legacy half) · D:4236 (`ResolveGlobalConstantsRecord`) · D:4329 (`SyncCurrentProgram`, whole function) · D:4585 (`BindCurrentFBO`) · D:4926 (`ResolveAndBindUnitTextures`) · D:4993 (`ResolveUnitSamplerBackend`) · D:5328 (`BindCurrentTextures`) · D:5689 (`GetCurrentBackendProgram`) · D:6166 (`GetBackendProgramId`) · D:6729 (`DrawArrays`) · D:6775 (`MultiDrawArrays`) · D:8837 (`ScopedDetachedTextureFramebufferAttachments`) · D:9161 / D:9216 (mipmap depth/color) · D:9695 (`SyncRenderbufferObjectToBackend`) · D:10029 (`ShaderStorageBlockBinding`) · D:11884 (`GetTexImage`) · M:256 (`OnFrontendStateObjectDestroyed`, NoSession arm only) · M:4544 (`HandleOfSamplerViewForTexture`) · M:5882 (`RequireImageBindableStorage`) · M:7087 (`SyncMipmapsToBackend`) · M:8481 (`ResolvePushedBuiltinSampler`) · M:8717 (`ResolvePushedTextureParams`) · M:9256 / M:9310 (`SyncAttachmentObject`, legacy arm) · M:9709 (`SyncAttachmentSurface` RBO cross-check) · M:10382 / M:10403 (debug-only verification block in `BackendFramebufferObject::SyncToBackend`) · M:12949 (`BackendProgramObjectImpl::SyncToBackend`) · M:13273 (`BackendSamplerObject::SyncToBackend`) · M:13570 (`BackendRenderbufferObject::SyncToBackend`). Tests: `MobileGL/MG_Test/Wire/RemoteClientTest.cpp:1883, :1903`.

`MGPipeReverseAnnouncementScope`: `MobileGL/MG_Impl/Pipe/ResourceTracker.h:723` (`MGPipeAnnounceBufferGpuWritten`) · M:2938 (`BufferImpl::HandleOfBuffer`) · test at `RemoteClientTest.cpp:1885`. Definitions: `MobileGL/MG_Impl/Pipe/SlotAllocator.h:199-233`.

## (b) Frontend objects/pointers held across records by the apply thread

| site | storage | what |
|---|---|---|
| D:67 | `g_rawDepthFetchSamplerState` | SharedPtr to a frontend `SamplerObject` created and retained by the backend for process life |
| D:1027, D:1054 (in `g_xfbObjects` D:1064) | `XfbObjectState::targets[].buffer`, `scatterProgram` | SharedPtrs to frontend `BufferObject`/`ProgramObject` pinned across a capture span |
| D:117-121; instances D:138, D:140, D:146 | `TwinLookupMemo` ×3 (VAO/program/FBO) | raw frontend key + `WeakPtr` owner (legacy arm) |
| D:1847/1849, D:1926/1928 (in `g_observedUnitBindings` D:1959) | `UnitBindingsSnapshot::legacySlotObjects`/`legacySamplerObject` | WeakPtrs to frontend textures/samplers |
| D:2127-2131 (in `g_unitTextureSyncList` D:2140) | `UnitTextureSyncEntry` | borrowed pointer into a frontend binding slot + raw `ITextureObject*` |
| D:2151-2152, D:2235-2236 | `g_fboTextureSyncList`, `g_readFboTextureSyncList` + raw `FramebufferObject*` keys | draw/read FBO attachment sync lists |
| D:4097-4098 | `g_currentDrawFrontendProgram`/`g_currentDrawBackendProgram` | per-draw program/twin stash |
| D:4107 | `g_broadcastMemoFbo` | raw `const FramebufferObject*` memo key |
| D:4978-4985 | `g_unitSamplerLookupMemos` | per-unit `WeakPtr` frontend sampler + handle key |
| D:5220-5235 | `g_resolvedTextureBindingMemos` | frontend `ProgramObject` as `const void*` key + bound-texture copies |
| D:5588-5592 | `rawDepthSamplerObject` | pointer into frontend `TextureUnit` storage (transient per sampler pass) |
| `Managers.h:1223/1254/1278` | `BackendVertexArrayObject::ResolvedDrawBuffers::Entry::frontend`, `iboFrontend` | raw client `BufferObject*` per-VAO memo |
| `Managers.h:1378` | `BackendVertexArrayObject::m_syncedIndexBufferObject` | raw `BufferObject*` |
| `Managers.h:2054` / M:10453 | `g_fboSyncedObjects[2]` | raw `FramebufferObject*` per target (compare-only) |
| `Managers.h:320-324`; registries at 1423/1818/1943/2540/2678/2819 | legacy `StateBackendObjectRegistry::m_entries` | raw frontend address key + `StateWeakPtr` |
| ST:156-167, ST:362-378 | `BackendSlotTable::Entry::stateRef`; `NoteStateForHandle`/`StateForHandle` | per-handle frontend `WeakPtr`/SharedPtr note |
| PI:807, PI:812-814 | `m_boundVertexArray`, `m_programForDispatch`, `m_programForDraw`, `m_transformFeedbackProgram` | SharedPtrs to frontend objects inside `gPipeInputs` (BARRIER_PULLED rows) |
| PI:808-811, PI:815 | `m_bufferBindingSlot[15]`, `m_bufferBindingPointBase[15]`, `m_framebufferBindingSlot[2]`, `m_imageTextureBindingBase`, `m_textureUnitBase` | raw pointers into frontend tables inside `gPipeInputs` |

## (c) `MGB_CTX->` accessors used from `MG_Backend/DirectGLES` whose `FieldOwnership.def` row is BARRIER_PULLED

Under `MOBILEGL_PIPE_PUSH`, `MGB_CTX` = `&gPipeInputs` (`MobileGL/MG_Pipe/PipeInputsSwitch.h:17-27`); BARRIER_PULLED object rows dereference the live GLContext through the pointer bases at PI:807-815.

| accessor | call sites |
|---|---|
| `GetFramebufferBindingSlot` / `GetFramebufferBindingSlotChecked` | D:182, D:197, D:6284, D:8408-8409; M:9397, M:9775 |
| `GetBufferBindingSlot` | D:401, D:669, D:962, D:6852, D:6930-6931, D:7009, D:7082-7083, D:7263, D:7312, D:10232, D:11214, D:11451, D:11761, D:12192; `Utils.cpp:2353`; MD:88 |
| `GetBufferBindingPoint` | D:510, D:569, D:605, D:628, D:1407, D:5464 |
| `GetBufferBindingPointCount` | D:621 |
| `GetBoundVertexArray` | D:1669, D:4770, D:6449, D:6724, D:6768; MD:96 |
| `GetTextureUnitObject` | D:1871, D:1899, D:1934, D:1947, D:2374, D:4861, D:5157, D:5562, D:8472, D:9302, D:9416, D:9564, D:11874 |
| `GetImageTextureBinding` | D:2650, D:2767; M:10849 |
| `GetProgramForDraw` | D:4781, D:5308, D:5679, D:5734 |
| `GetProgramForDispatch` | D:6126 |
| `GetTransformFeedbackProgram` | D:1387 |
| `GetProgramObject` / `ValidateProgramName` | D:6157, D:10023 / D:6152, D:10022 |
| `RecordError` | D:8804; M:13668 |

(`GetTouchedBufferBindingPointCount`, `GetActiveTextureUnit`, `GetCurrentVertexAttribute`, `GetPrimitiveRestartIndex`, `GetRenderStateParameters*`, `GetViewport`, `IsCapabilityEnabled`, `IsTransformFeedbackActive/Paused`, `GetPatch*`, `GetTransformFeedbackCapturedVertices`, `GetMaxTouchedTextureUnit` are RECORD-SUPPLIED; `GetTextureContextId`/`GetTextureBindGeneration`/`GetSamplingResolutionGeneration`/`GetPixelStoreParameters(false)` are APPLIER_DERIVED — server-owned, not listed.)

## What would have to change for the apply thread to read only server-owned memory

**VAO/vertex.** The bound-VAO row (`GetBoundVertexArray`) must become a handle read of `BoundVertexElements` plus the `VertexElementsCso` record; `GetConfigVersion()` compares must be replaced by the applier's own `VertexBuffersSerial`/`IndexBufferSerial`/`ContentSerial`. The `DrawArrays`/`MultiDrawArrays` entry-path probe (#12/#13) and `SyncClientSideAttributesForDrawArrays` (#14) have no pushed equivalent for client-side arrays — those draws would need the client-array bytes staged with the record or the path refused under split. The legacy memo/registry arm (#4, #7, #8) dies with the legacy arm.

**Buffers.** Every `HandleOfBuffer` probe on the hot path (#17, #25, #27, #30) is redundant with handles already present in `IndexBuffer`, the `VertexBuffers` window, and the shader-buffer records; the sync lists must key on handles and drop the stored raw frontend pointers. `GetBufferResource`/`GetBackendResource` (#32) needs a handle-keyed twin lookup. The reverse announcement `MarkBufferGpuWritten` (#30/#31) must consume the record-side handle instead of probing the client allocator per SSBO per draw.

**Textures/samplers.** The memoised sync lists must stop borrowing pointers into client binding slots (#38); the unit windows already exist as `BoundSamplerViews`/`BoundSamplerStates` records, so the per-draw key check, the rebuild walk (#35-#40), and `ResolveAndBindUnitTextures` (#50-#53) can all run on handle arrays. Frontend-keyed `Find`/`GetOrCreate` (#47-#48, #55, #66) becomes `FindByHandle`/`GetOrCreateByHandle`. `GenerateMipmap`'s unit/slot resolution (#59) must take the texture from `VerbMipRes` only; the CPU-mip shadow path (#61) and the unkeyed all-twin detach sweep (#63) need staged shadow bytes and a handle-keyed affected-FBO list respectively.

**Framebuffers/attachments.** The record arm of `SyncCurrentFBO` must stop validating the pushed record against the live binding slot (#68) — the record's own `Gen`/serial is the truth; the slot read at #69, the `NoteStateForHandle` SharedPtr stash (#70), and the `SyncToBackend(currentFBO)` walk (#71, #81, #82) must be replaced by applying `MGPFramebufferState` directly. `ForceBindCurrentFBO` (#74) needs a handle-keyed force-bind. The bound-form blit (#78, #79) must carry FBO handles and attachment descriptors in `MGPBlit` (or a companion record), since today nothing pushed identifies the two framebuffers or their attachments.

**Images.** `GetImageTextureBinding` rows (#85-#90) are already duplicated by the `BoundShaderImages` window (`MGPImageView` records); the apply path needs to read the window + serial and drop the live binding read, the `binding.Texture` dereference, and the `HandleOf` identity check, including the format-less-image `BoundImageUnitFormat` lookup.

**Programs/shader state.** The program rows (#91, #92) become `DrawProgram`/`DispatchProgram` handle reads; twin identity resolves by handle, eliminating #94/#95. Version checks (#96, #102, #108) map to `ShaderCso` serials; the per-draw `GetShaderStorageBlockBindingOverrides` signature (#97) and the attribute mask/type reads (#106) need to be baked into the `ShaderCso` record at create/relink time. `MapUBO` fallback bytes (#103) already have the `GlobalConstants` record as the pushed arm. The name-table probes (#109) are API-driven and can stay latency-bound or be handle-translated at emit.

**Binding-point tables.** This family has no transport branch at all: `SyncBufferBindingPoints`, `MarkShaderStorageBuffersGpuWritten`, `SyncAtomicCounterBuffers`, and the per-UBO loop read the live GLContext arrays on every draw/dispatch (#111-#118). The SSBO side is partly covered by `MGPShaderBuffers`; UBO and atomic-counter bindings need equivalent pushed windows (handle + range per index) so the loops iterate applier state, and `EnsureBufferResource` becomes handle-keyed throughout.

**XFB.** Capture setup (#119-#121) reads the TF program row and TF binding points live; the record arm needs the TF program handle and the captured-target list pushed at `BindStreamOutput`/begin, with `xfb.targets` storing handles instead of client SharedPtrs. The span-end readback probes (#122) then resolve by handle; the legacy writeback arm (#123) is monolith-only and unaffected.

**Misc.** `OnResourceCopyRegion`'s sticky-forward name-table probe (#125) must resolve `MGPCopyRegion.Src/Dst` handles instead of the GL names. Readback pack-PBO reads (#127) need the pack buffer handle in `MGPReadbackInfo`-class records. The multi-draw EBO reads (#128) fold into the `IndexBuffer` handle. The two deliberate leaks (`gPipeInputs`, PI:818-835; `MGPipeSlots()`, `SlotAllocator.cpp:370-381`) and every frontend-pointer memo in list (b) must be retired or converted to handle keys before the R-1 barrier can be removed.
