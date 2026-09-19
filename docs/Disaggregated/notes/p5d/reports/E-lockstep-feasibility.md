# E - can the draw barrier be deferred by one verb? (research note, no code)

Head `85362a85`, worktree `MobileGL-p5d-E`, DirectGLES, `MOBILEGL_TRANSPORT=inproc`, push mask
`0x1fff` (every subsystem bit on, so every record arm in the backend is the arm that runs).
Every `file:line` below is at that head. Paths are under `MobileGL/` unless they start with `docs/`.

## 0. The lockstep as it is wired today (the thing (c) would move)

1. `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp:544-550` - `DrawElements_Backend` runs `MGP_FILL(DrawElements)`
   (= `MGPipeValidateForVerb`, `MG_Impl/Pipe/PipeFill.cpp:2781`) and only then calls the backend table
   slot, which under a transport is the client emit table.
2. `PipeFill.cpp:2789-2790` (`RefusePipeInputsTouchWhileApplierOwnsIt`), `:2814` (withdraw the server
   stamp), `:3059-3104` step 4: for every field in the kDraw mask not supplied by a record,
   `MGPipeFillAccess::CopyField` (`:88-330`). The object-class rows copy either a **SharedPtr**
   (VAO `:113`, dispatch program `:216`, draw program `:219`, XFB program `:262`) or a **raw pointer
   into the live GLContext arrays** (15 buffer slots `:119-121`, 15 binding-point row bases `:127-129`,
   2 framebuffer slots `:174-178`, image-unit base `:183`, texture-unit base `:246`). Storage:
   `MG_Backend/MGPipe/PipeInputs.h:807-815`.
3. `MG_Remote/Client/EmitTables.cpp:529-553` `EmitIndexedDraw` -> `ReadDrawBindings` (`:459-482`, reads
   the VAO element slot + the two indirect slots on the GL thread) -> `EmitDrawRecord` (`:484-525`):
   `BeforeDrawVerb` (`:222-226`: `PushDrawConsumers`, deferred-destroy drain, `MarkGpuWritesForDraw`)
   then `EmitAndWaitTails`.
4. `MG_Remote/Client/ClientSession.cpp:814-990`: encode, `PublishAndNotify` (`:871`); DrawVbo is
   `kCtxVerb`, so the BatchWaits early return at `:899-905` does not apply; `WaitForAppliedBudget`
   (`:916-917`), then `DrainEventRing` (`:951` - "the one instant at which the apply thread is known
   not to be inside the applier").
5. Server: `MG_Remote/Server/PipeApplier.cpp:1043-1080` `ApplyOne` = `ScopedApplierEntry` (`:1057`),
   `StampVerbBoundary` (`:1061` -> `PipeInputs.cpp:103-130`: every BARRIER_PULLED field's `FilledGen`
   is set to 0, so each server read of one is counted as `rsp` by `MGPipeInputUnfreshRead`
   `:137-170`), `DecodeAndApply` -> `OnDrawVbo` (`:430`) -> `gl.DrawElements` ->
   `MG_Backend/DirectGLES/DirectGLES.cpp:6705-6716` -> `PrepareForDraw` (`:4762-4830`), then
   `LeaveApplier`. `MG_Remote/Transport/SessionRings.h:378-394`: apply -> `++appliedSeq` ->
   `PublishApplied` -> ring. The client is runnable again only after step 5 has fully returned.

What the barrier protects is stated at `PipeInputs.h:904-910`: `m_filled` (`FilledGen[63]`,
`CurrentVerbSerial`), `m_serverStampedVerb` and the `rsp` counter are plain, not atomic, because
"the verb barrier leaves at most one of {GL thread, apply thread} runnable". `CONTRACT-P5C.md:35-43`
(rule E) says "the barrier held" is no longer an accepted mechanism - and then `:157-201` (§3.1) and
`:410-437` (§5.4) grant exactly the object-class family and the two scopes a named exemption from it.

## (a) Every server-side read that reaches client-owned memory through the object rows

Legend: **live** = a dereference of the frontend array/object the row points at; **id** = a
`GetLifetimeId()` read plus a probe of the CLIENT slot allocator (`MGPipeSlots().FindByLifetimeId`,
`DirectGLES/SlotTables.h:436-449`, inside `MGPipeFrontendKeyedRegistryScope` or
`MGPipeReverseAnnouncementScope`; `MG_Impl/Pipe/SlotAllocator.cpp:22-38` is the guard the scope
disarms); **mint** = `GetOrCreate(const StatePtr&)` -> `MGPipeSlots().Acquire` (`SlotTables.h:244-248`),
a WRITE into the client allocator; **rec** = the same site has a record arm that reads no frontend
memory and is the arm that runs on this workload.

### a.1 `GetBoundVertexArray` (SharedPtr copy, `PrepareForDraw` `:4770`)
| site | what is dereferenced | kind |
|---|---|---|
| `ResolveVaoTwin` `:1602-1633` (scope `:1609`) | `g_backendVertexArrayObjects.Find(vao.get())` -> `SlotTables.h:411-414` -> `HandleOf` `:436-449`: `vao->GetLifetimeId()` + allocator probe; `GetOrCreate(vao)` on a miss `:1618` | id, mint |
| `:4776` | `currentVAO->GetConfigVersion()` | live |
| `SyncVaoAttributeBuffersByHandle` `:699-790` | memo hit `:719-737`: per entry `IsBufferDrawCleanByHandle(entry.handle, entry.resource, entry.frontend)` (`Managers.cpp:3109-3153`; under a transport `:3143-3145` does NOT touch `frontend`), dirty repair `:731` `currentVAOObject->GetAttribute(i).Buffer` -> `EnsureBufferResource`; memo miss `:741-780`: `GetAllAttributes()` (32 x `{Enabled, Buffer SharedPtr, ...}`), `attrib.Buffer.get()`, `EnsureBufferResource(bufferObject)` (`Managers.cpp:3448-3455` -> `HandleOfBuffer` `:2922-2936`, scope `:2933`: `GetLifetimeId` + allocator probe), `HandleOfBuffer(bufferKey)` `:767` | live, id |
| IBO arm `:823-861` | `currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject()` (SharedPtr in the VAO), `HandleOfBuffer(possibleIBO.get())`, `EnsureBufferResource(possibleIBO)` | live, id |
| `SyncCurrentVAO` `:1637-1649` -> `SyncToBackend` (`Managers.cpp:4881-4891`) -> `SyncToBackendFromApplier` (`:5160+`) | nothing: "the frontend VAO is not read at all"; the index arm resolves `st.IndexBuffer.Res` only (`:5320-5331`) | rec |
| `SyncCurrentVertexAttributeValues` `:1659-1700` | `GetBoundVertexArray()` again `:1669`, `vao->GetConfigVersion()` `:1682`, memo miss `vao->GetAttribute(location).Enabled` `:1686` | live |
| `DrawArrays` `:6718-6738` (scope `:6729`), `MultiDrawArrays` `:6775` | `Find(currentVAO.get())` + `SyncClientSideAttributesForDrawArrays(currentVAO, ...)` | id, live |
| `EnsureBufferResourceForHandle` `Managers.cpp:3193-3446` | under a transport the object is HELD, not read: base `:3207-3222` = `resource->hostBytes`, `SyncPersistentMappedRange` skipped `:3329-3341`, content bit from the descriptor `:3376-3381`; it still takes the SharedPtr by reference | held only |

### a.2 `GetProgramForDraw` (SharedPtr copy, `:4781`; the client-side `GetProgramForDraw` join is `MG_State/GLState/Core.cpp:609-628` and runs at fill, not on the server)
| site | what is dereferenced | kind |
|---|---|---|
| `SyncCurrentProgram` `:4321-4400` (scope `:4329`) | `GetLinkStatus()/GetSpirvStatus()` `:4339`; `g_backendProgramObjects` probes; broadcast count from the framebuffer record when `g_fboRecordsTrusted` `:4350-4372`, else `GetFramebufferBindingSlotChecked(Draw)` + `drawFBO->GetObjectVersion()/GetDrawBuffers()` `:4384-4395` | live, id, rec |
| `BindCurrentTextures` `:5245-5305` | `currentProgram.get()` as key, `GetLifetimeId()`, `GetBackendStateVersion()`, `GetLinkStatus()` `:5279-5282`; miss -> `ResolveAndBindUnitTextures` `:4836+`: `GetMaxUniformLocation/GetUniformSamplerOrImageUnitIndex/GetUniformType` `:4845-4855` (only on an alias conflict) | live |
| `BindCurrentProgramWithResources` `:5317-5600` (scope `:5328`) | `GetLinkStatus/GetSpirvStatus` `:5320`, registry `Find` fallback `:5336`, `GetUBOSize` `:5347,:5371`, `MapUBO()`/`GetUBOContentVersion()` ONLY when no ShaderCso record `:5364-5387` (record arm `:5364-5368`), `GetUniformBlockBinding(i)` `:5463`, `GetBackendStateVersion` `:5514`, sampler pass miss `GetUniformSamplerOrImageUnitIndex` `:5537-5539` | live, rec (UBO bytes) |
| `SyncAtomicCounterBuffers` `:600-660` | `GetBufferBindingPointCount` (sticky forward -> `PipeFill.cpp:1861-1865`, live context) - only for programs with counters | live |

### a.3 `GetTextureUnitObject` (raw base of `Array<TextureUnit,192>`; a unit is 11 `BindingSlot<ITextureObject>` + one `SharedPtr<SamplerObject>`, `MG_State/GLState/TextureState/TextureUnit.h:15-26`, `MG_Util/Types.h:181-201`)
| site | what is dereferenced | kind |
|---|---|---|
| `CaptureDrawTextureSyncKeys` `:2188-2211` -> `CurrentUnitBindingsEpoch` `:2044-2098` | record arm `:2048-2056` reads two applier serials (`SamplerViewsSerial`, `SamplerStatesSerial`); walk arm `:2073-2097` -> `CaptureUnitBindings/UnitBindingsUnchanged` `:1867-1920`: per unit `GetAllBindingSlots()[i].GetBoundObject()->GetLifetimeId()`, `GetSamplerObject()` | rec; walk = live |
| `SyncNeccessaryTextures` `:2333-2480`, memo HIT `:2342-2362` | `PairingsIntact` `:2134-2139`: `entry.slot->get()` where `entry.slot` is `const SharedPtr<ITextureObject>*` INTO the frontend unit's slot (`:2127-2131`, borrowed at `:2374`); then `entry.backend->IsDrawSyncClean(entry.slot->get(), ...)` `:2356` - the texture object's versions. **This runs on every draw even when every memo hits.** | live |
| same, memo MISS `:2372-2385` | `GetTextureUnitObject(index)` per unit, every slot's `GetBoundObject()`, `IsUndefinedDefaultTexture`, `SyncTextureObjectToBackend(textureObject)` (`:1725-1760`, scope `:1734`: `Find` id / `GetOrCreate` mint, then params/sampler/mips sync of the object) | live, id, mint |
| `ResolveAndBindUnitTextures` `:4861-4880` (memo miss of `BindCurrentTextures`) | per unit `GetAllBindingSlots()`, `GetBoundObject()`, twin `Find` (scope `:4926`) | live, id |
| sampler pass `:5562-5582` | `GetTextureUnitObject(unit).GetSamplerObject()`, `GetBindingSlot(Texture2D).GetBoundObject()`, `samplerObject->GetLodBias()`, `texture2D->GetFormat()/GetSamplerObject()` | live |
| `BindCurrentUnitSamplers` `:5051-5170` | record arm `:5111-5150` reads `st.BoundSamplerStates[]` only; decline arm `:5157` `GetTextureUnitObject(unit).GetSamplerObject()` | rec |

### a.4 `GetImageTextureBinding` (raw base of `Array<ImageTextureBinding,192>`, `TextureState.h:18-27`: SharedPtr + 5 scalars)
| site | what is dereferenced | kind |
|---|---|---|
| `SyncImageTextureBindingsForDraw` gate `:2870-2900+` | `g_imageUnitHighWaterMark == 0` early-out (`:2893`): zero reads on a draw with no image unit ever bound | - |
| `SyncImageTextureBinding` `:2636-2748` | `imageBinding.Texture` (SharedPtr) `:2650,:2660`, `ResolveShaderImageRecord` (`:2582-2635`, scope `:2588`; identity test `view.Res != HandleOf(boundTexture)` `:2626`), Level/Layer/Layered/Format from the record when it matches `:2670-2676`, **`imageBinding.Access` always frontend** `:2745` (`:2663-2669`: the record's `Access` byte has no written encoding) | live, id, rec (4 of 5 fields) |
| `MarkWritableImageBufferTexturesGpuWritten` `:2764-2785` | gated `g_writableImageBufferUnitCount == 0`; else `imageBinding.Texture.get()` -> `TextureObjectBuffer::GetBufferBindingSlot().GetBoundObject()` -> `MarkBufferGpuWritten` (`Managers.cpp:2947`, id) | live, id |

### a.5 `GetFramebufferBindingSlot` (raw pointers to the two `BindingSlot<FramebufferObject>`)
| site | what is dereferenced | kind |
|---|---|---|
| `SyncCurrentFBO` `:3277-3330` | `SyncCurrentFBOByRecord` first `:3292`; decline (scope `:3303`): slot `:3305`, `GetBoundObject()/GetVersion()`, `currentFBO->GetObjectVersion()`, attachment walk | rec |
| `BindCurrentFBO` `:4514-4600` | record arm `:4527-4560` (`FindByHandle(record.Fbo)`); decline `:4568-4590`: slot, `GetBoundObject()`, compare with `pDefaultFramebufferInfo->defaultFBO` `:4577`, `Find(currentFBO.get())` (scope `:4585`) | rec |
| `SyncNeccessaryTextures` draw-FBO list `:2405-2470` and `SyncReadFramebufferTextureAttachments` `:2246-2331` | **always** `GetFramebufferBindingSlotChecked(target)`, `GetBoundObject()`, `GetVersion()`, `currentFBO->GetObjectVersion()`, and on a list rebuild `currentFBO->GetAllAttachmentObjects()` / `attachment.GetTexture()` (borrowed SharedPtr slots); the record only replaces the VERSION half of the key, "THE IDENTITY HALF IS STILL A POINTER COMPARE" `:2159-2168` | live |
| `Clear` `:6180-6190`, `BlitFramebuffer` bound arm `:8380-8418` | the three functions above, plus `GetFramebufferBindingSlot(Read/Draw).GetBoundObject()` `:8407-8410` for the layered-blit workaround; the named arm `:8330-8378` reads handles and the twin table's `StateForHandle` note | live / rec |

### a.6 `GetBufferBindingSlot`, `GetBufferBindingPoint`, the rest
- `GetBufferBindingSlot` (15 pointers): on the inproc DRAW path there is **no dereference** - the
  indirect arm reads `MGPipeApplier().VerbIndirectBuffer` (`:941-960`; P5c hd §3.5), the
  `:962` slot read is the monolith `else`. Readers are readback (`:10232` etc.) and the monolith
  `ReadIndirectCommandBytes` `:401`. The row is still FILLED every draw (15 x `GetBufferBindingSlot`
  in `CopyField` `:119-121` - the `GetBindingSlot` 1.73% in the profile).
- `GetBufferBindingPoint` (15 row bases into `Array<BindingSlotRange1D,84>`): `SyncBufferBindingPoints
  (ShaderStorage)` `:477-521` and `MarkShaderStorageBuffersGpuWritten` `:585-598` (count is
  record-supplied; loop is over 0 entries on Minecraft), the UBO loop `:5463-5480` (1-3 points per
  program on Minecraft: `point.GetBoundObject()`, `point.GetRange()` - which itself reads
  `object->GetSize()` when no explicit range, `Types.h:218-224` -, `GetBufferResource`,
  `IsBufferDrawClean` -> `HandleOfBuffer` id, `EnsureBufferResource`), atomic counters `:628`, XFB
  `:1407-1420,:569` (only with a pending span). All **live + id**.
- `GetProgramForDispatch`: `PrepareForCompute` `:6126` (dispatch only; same shape as a.2).
- `GetTransformFeedbackProgram`: `StartPendingTransformFeedback` `:1381-1420` at the tail of every
  draw (`:4829`) but returns at `:1386` unless a span is pending.
- Sticky forwards on the draw path: `GetBufferBindingPointCount` (`:621`, atomic counters only).
  `GetProgramObject/ValidateProgramName` (`:6152-6157`), `GetTextureObject` (CopyTex),
  `HasOpenTransformFeedbackSpan`, `RecordError` (`:8804`) are off the steady draw path.

### a.7 The two exemption scopes
`MGPipeFrontendKeyedRegistryScope` (20 sites: `:1609, :1734, :2588, :3303, :4236, :4329, :4585,
:4926, :4993, :5328, :5689, :6166, :6729, :6775, :8837, :9161, :9216, :9695, :10029, :11884`) wraps a
`Find` / `HandleOf` / `GetOrCreate(const StatePtr&)` on one of the five frontend-keyed twin tables;
each is `stateObj->GetLifetimeId()` + `MGPipeSlots().FindByLifetimeId` (`SlotTables.h:436-449`),
and a miss on `GetOrCreate` is `MGPipeSlots().Acquire` (`:244-248`). `MGPipeReverseAnnouncementScope`
(`Managers.cpp:2933`) wraps the same probe for every buffer (`HandleOfBuffer`), reached from every
`EnsureBufferResource` / `IsBufferDrawClean` / `MarkBufferGpuWritten`. Both counters are
`thread_local` (`SlotAllocator.cpp:41-70`) - the 7.4% apply-thread emutls and 3.0% `HandleOf<ITextureObject>`
in the profile are these probes. On the steady Minecraft draw (memos hitting) the scope sites that
still execute per draw are `:1609` (VAO twin), `:4329` (program), `:5328` (program resources),
`:2933` for each UBO point, and `:1734`/`:4926` only on memo misses.

## (b) Which of those reads are UNSAFE if the client runs draw N+1's GL calls before the server applied draw N

Between `PublishAndNotify` for N and `MGPipeValidateForVerb` for N+1 the GL thread runs arbitrary
entry points: `glBindVertexArray`, `glBindBuffer(Base/Range)`, `glActiveTexture`/`glBindTexture`,
`glBindSampler`, `glUseProgram`, `glBindFramebuffer`, `glVertexAttribPointer`, `glEnable/DisableVertexAttribArray`,
`glBufferData/SubData`, `glTexParameter*`, `glUniform*`, `glLinkProgram`, `glDelete*`, plus the client's
own pre-verb work (`PushDrawConsumers` `PersistentMapTracker.cpp:704-745`, `MarkGpuWritesForDraw`
`GpuWritePending.cpp:126-138`, `ReadDrawBindings`). Four classes of hazard:

1. **The raw-pointer rows point INTO live arrays, so every deref reads draw N+1's binding, and the
   read races the client's write.** `BindingSlot::Bind` is a non-atomic `shared_ptr` move plus
   `++m_version` (`Types.h:189-195`); `BindingSlotRange1D::SetRange` is a two-word store (`:229-232`).
   Concretely unsafe: `PairingsIntact` / `IsDrawSyncClean(entry.slot->get())` (`:2136, :2356` - on
   EVERY draw, memo hit or not), the unit walks (`:1871, :2374, :4861, :5562`), the UBO point reads
   (`:5464-5466`, incl. `GetRange()` -> `obj->GetSize()` while `glBufferData` may resize), the
   framebuffer slot reads (`:2405-2410, :2246, :3305, :4568`) and `GetAllAttachmentObjects()` while
   `glFramebufferTexture` mutates the container, image bindings (`:2650-2660, :2745`). A torn
   `shared_ptr` read is UB; a texture the client just `glDeleteTextures`'d while the server holds
   only a borrowed slot pointer is a use-after-free (the borrow rules at `:2103-2126` are written
   on the barrier). **Safe**: `GetBufferBindingSlot` (no draw-path reader under a transport).
2. **The SharedPtr rows pin identity but not internals.** VAO: `GetConfigVersion` (`:4776,:1682`),
   `GetAllAttributes()/GetAttribute(i).Buffer` (`:731, :743-760, :1686`), the element slot (`:824`)
   move under `glVertexAttribPointer`/`glBindBuffer(ELEMENT_ARRAY)` on the same VAO - wrong draw AND a
   `shared_ptr` race. Program: `MapUBO()` (`:5385`, only without a ShaderCso record) copies the
   scratch `glUniform*` writes into - a torn UBO upload; `GetUniformSamplerOrImageUnitIndex`,
   `GetUniformBlockBinding`, `GetBackendStateVersion`, link/SPIR-V status move under
   `glUniform1i`/`glLinkProgram` (`Core.cpp:609-628` joins the pending link on the CLIENT at fill,
   so a relink issued after publish N is exactly the window the join exists to close). Objects reached
   through the rows: `GetSize` (`glBufferData`), texture params/shape for `IsDrawSyncClean` and
   `SyncTextureParamsToBackend` (`glTexParameter`), `GetLodBias` (`glSamplerParameter`), FBO draw
   buffers/attachments. Only `GetLifetimeId()` is immutable and safe.
3. **The client allocator is client memory, mutated by every create/delete.** Every `HandleOf` /
   `Find` in both scopes (`SlotTables.h:445-446`, `Managers.cpp:2935`) probes `MGPipeSlots()`; a
   `GetOrCreate` miss WRITES it (`:248`). `glGen*`/`glDelete*` between two draws make this a data
   race on a hash container whatever draw the probe "means" - and it is unsolved by any snapshot of
   the pointer rows. This is rule E's own sentence at `SlotAllocator.cpp:30-35`, currently
   suspended by the two scopes on the strength of the barrier.
4. **`gPipeInputs` itself is one block.** N+1's fill rewrites the pointer rows, `CurrentVerbSerial`
   and `FilledGen[]` (`PipeFill.cpp:2806, :3068-3103`) while N's stamp (`PipeInputs.cpp:110-127`)
   and N's reads are in flight; `RefusePipeInputsTouchWhileApplierOwnsIt` (`ClientSession.cpp:998-1022`)
   is disarmed under `BatchWaits=1` on the argument that "the pull-verbs' own wait keeps their
   pulled reads fenced" (`:1006-1012`, `:889-898`) - deferring the draw wait removes precisely that
   fence. `DrainEventRing` (`:951`) and the `OnBufferWriteback`/`OnGpuWritten` callbacks that write
   frontend objects also rest on it.

Net: on the inproc draw path, every object-class read except `GetBufferBindingSlot` is unsafe under
one-verb pipelining; the record arms (a.1 `SyncToBackendFromApplier`, a.3 epoch + sampler bind,
a.5 `SyncCurrentFBOByRecord` + `BindCurrentFBO`) are the only parts already safe.

## (c) The minimal "snapshot at fill" scheme

Two layers have to be snapshotted, not one:

**Layer 1 - the block.** `gPipeInputs` becomes a ring of >= 2 `PipeInputs` (63 fields; the value rows
are record-supplied and need no copy, but `m_filled`, `m_currentVerb`, `m_serverStampedVerb` are per
slot). The three doors (`MGPipeFillAccess` `PipeFill.cpp:69`, `MGPipeStampAccess` `PipeInputs.cpp:95`,
`MGPipeApplyAccess`), the generated poison (`PipeFilled.inc`), the verify comparator
(`PipeFill.cpp:470-510`) and the stamp all address ONE block today. Cost per draw: 4 SharedPtr copies
(8 atomic RMW) + 34 pointers (272 B) - trivial - but the pointers are useless as a snapshot: they
name live arrays (b.1).

**Layer 2 - what the pointers point at.** The closure the server dereferences per draw, if copied
by value (upper bound, everything a.1-a.6 reaches on a memo miss):

| family | per draw | bytes | refcount ops (inc+dec) |
|---|---|---|---|
| texture units 0..maxTouchedUnit (Minecraft ~4-8) | 12 SharedPtr per unit (11 slots + sampler) + versions | 8 x ~320 B = ~2.5 KB | ~96-192 |
| image units 0..highWaterMark | `ImageTextureBinding` x n (0 on Minecraft) | 0-40 B x n | 0-2n |
| UBO points per program block (1-3) | SharedPtr + `Range1D` + explicit flag | ~40 B each | 2-6 |
| SSBO / atomic / XFB points | 0 on Minecraft | 0 | 0 |
| FBO slots x2 + attachment SharedPtrs | 2 SharedPtr + versions (+ n attachments for Iris) | ~64 B (+24n) | 4 (+2n) |
| VAO: identity + config version + (memo miss) 32 attributes + IBO | 1 SharedPtr + 4 B; miss: 32 x ~48 B + 1 | ~24 B / ~1.6 KB | 2 / 2+2k |
| program: identity + 6-10 scalars, `MapUBO` bytes only without ShaderCso | 1 SharedPtr + ~64 B | ~80 B | 2 |
| **total, steady (memos hit)** | | **~3 KB** | **~110-210** |

At 852 draws/frame: ~2.5 MB/frame of memcpy (same order as the round-1 persistent-map push that
was worth cutting) and ~95k-180k lock-prefixed RMW/frame; at 20-40 ns each that is **2-4 ms per
15.6 ms frame on the GL thread** - comparable to the wait it would remove - and it does NOT fix
(b.2) (object internals still live), (b.3) (allocator probes) or the borrowed-slot lists
(`:2127-2131`), which would have to be rebuilt to point at the snapshot rather than at the unit.

A **handle-only snapshot** (lifetime id / `{slot,gen}` + version per entry, no SharedPtr) is
~1.5 KB/draw, zero refcount ops, ~1 us/draw. But the server can only consume it if every twin
lookup is BY HANDLE (`FindByHandle`, `SlotTables.h:420-425`) and `IsDrawSyncClean` compares
server-owned serials - which is the G6 rekey (ROADMAP.md:71) plus the `set_shader_buffers` emission
(CONTRACT-P5C.md:169-176). So the only snapshot that is both minimal and SAFE is not a snapshot:
it is the retirement of the frontend-keyed path.

**Where the client would wait.** Today: `ClientSession.cpp:916-917`, immediately after `:871`.
Under one-verb pipelining: `EmitDrawRecord` publishes N and returns; the GL thread runs its
inter-draw work; at the top of `MGPipeValidateForVerb(N+1)` (`PipeFill.cpp:2781-2790`, where the
`RefusePipeInputsTouch...` guard already stands) it waits `appliedSeq >= seq(N)` BEFORE the residual
fill writes the next block (or before it recycles slot N of the ring), then drains events. The
reply-slot rows (`:876-879`) and `PushDrawConsumers`' `resource_subdata` records (ordered before
N+1 on the ring, `EmitTables.cpp:215-219`) keep their shape. Overlap gained = min(client inter-draw
work, server apply of N).

## (d) What the pushed records already cover, so the server COULD stop reading the live tables

| record(s), applier state (`MG_Pipe/PipeApply.h`) | draw-path consumer that already uses it | live read that remains and what would replace it |
|---|---|---|
| `bind/create_vertex_elements` -> `VertexElementsCsos`, `BoundVertexElements` (`:487,:594`); `set_vertex_buffers` -> `VertexBuffers[32]{Res,...}`, `VertexBuffersSerial` (`:598-616`); `set_index_buffer` -> `IndexBuffer.Res` (`:620`) | `SyncToBackendFromApplier` (`Managers.cpp:5160+`, reads nothing frontend); memo KEY of `SyncVaoAttributeBuffersByHandle` (`:703-716`); index resolve `:5320-5331` | the ensure WALK (`:731, :741-780`) and the IBO arm (`:823-861`) still take the VAO's attribute/element SharedPtrs to reach `HandleOfBuffer`; under a transport `EnsureBufferResourceForHandle(nullptr, h)` needs no object (`:3193-3222`, the indirect arm at `:953` already calls it that way) - walk `st.VertexBuffers[Start..+Count].Res` and `st.IndexBuffer.Res` instead. `ResolveVaoTwin`'s twin is keyed by VAO lifetime id, the CSO handle is content-addressed (`SetHashSuppressor`) - the twin needs a rekey (P3b/P4b). `SyncCurrentVertexAttributeValues` can read `rec->Attributes[i].Enabled` |
| `set_sampler_views` -> `BoundSamplerViews[unit] = {View, Texture, Unit}` (`MGPipeTypes.h:856-861`), `SamplerViewsSerial`; `bind_sampler_states` -> `BoundSamplerStates[]` (`PipeApply.h:673-681`); tx staged shadow + `set_texture_params` | `CurrentUnitBindingsEpoch` record arm (`:2032-2056`), `BindCurrentUnitSamplers` record arm (`:5111-5150`) | `SyncNeccessaryTextures`' unit list (`:2372-2385`, and its per-draw `PairingsIntact`/`IsDrawSyncClean` on borrowed slots), `ResolveAndBindUnitTextures` (`:4861-4880`), the sampler pass (`:5562-5582`). One view per unit is already the RESOLVED sampled target, so the 11-slot alias resolution (`:4864-4880`) collapses; `IsDrawSyncClean` would key on the applier's texture serials. Caveat A8 (`:2816-2822, :5127-5133`): nothing pins the window to cover `[0, maxTouchedUnit]` |
| `set_shader_images` -> `BoundShaderImages[unit] = {Res, Unit, InternalFormat, Layer, Level, Layered, Access}` (`MGPipeTypes.h:877-886`) | `SyncImageTextureBinding` takes Level/Layer/Layered/Format from the record (`:2670-2676`); the window/mark union (`:2837-2842`) is server-owned | `Access` is carried but deliberately unread (`:2663-2669`: encoding undefined) -> define it; the identity test `HandleOf(boundTexture)` (`:2626`) -> `FindByHandle(view.Res)`; `MarkWritableImageBufferTexturesGpuWritten` needs the TexBuffer's backing-buffer handle (not in `MGPImageView`) |
| `set_framebuffer_state` -> `FramebufferRecords[h] = {Fbo, Color[8]/Depth/Stencil surfaces, DrawBuffers, IsDefault, ...}` (`MGPipeTypes.h:750-760`), `BoundFramebuffer[2]`, `FramebufferSerial` (`PipeApply.h:639-654`) | `SyncCurrentFBOByRecord` (`:3292`), `BindCurrentFBO` record arm (`:4527-4560`), broadcast count (`:4350-4372`), the ContentHash half of both texture-list keys | the identity half of `g_fboTextureSyncList` / read list (`:2405-2410, :2246-2258`) and their attachment borrows -> `FindByHandle(record.Fbo)` + the record's surface handles; `BlitLayeredDestinationAspects` (`:8407-8410`) -> the twins' `StateForHandle` note as the named arm already does (`:8355-8362`); `pDefaultFramebufferInfo` (G6) |
| `set_draw_program` -> `DrawProgram`, `bind_shader_state` -> `BoundShaderCso`, ShaderCso records with `GlobalConstants` bytes + version (`PipeApply.h:503,:689-693`) | UBO bytes and version (`:5364-5368`), program twin caches block indices / sampler bindings at link (`:5449, :5537`) | link/SPIR-V status, `GetLifetimeId`, `GetBackendStateVersion`, `GetUBOSize`, `GetUniformBlockBinding(i)`, `GetUniformSamplerOrImageUnitIndex(loc)` - the reflection/binding table (ROADMAP.md:32 "program registry 重键", ARCHITECTURE 3.2 "explicitly-not-ported") |
| `set_context_values` (P5c rv) -> the 8 value rows | `SyncBufferBindingPoints` count, `maxTouchedUnit`, XFB flags | - (RECORD_SUPPLIED) |
| `set_shader_buffers` / `set_stream_output_targets` (catalogued, **not emitted until P4b**, CONTRACT-P5C.md:169-176) | nothing | the 84-entry `GetBufferBindingPoint` rows: the UBO loop `:5463-5480` (1-3 per Minecraft draw), SSBO/atomic/XFB. **The one family with no producer at all** |

Also structurally: `EnsureBufferResourceForHandle` already reads nothing from the object under a
transport (a.1 last row) - the frontend argument survives only so `HandleOfBuffer(obj)` can find
the handle. Every consumer that has the handle in the applier (`VertexBuffers[].Res`,
`IndexBuffer.Res`, `VerbIndirectBuffer`) could pass `(nullptr, h)` today; UBO/SSBO/atomic/XFB
cannot until `set_shader_buffers` crosses.

So the records are ~80% there in SHAPE (vertex / sampler / image / framebuffer / program handles
all cross) and ~0% there in the IDENTITY path (every twin table is still frontend-keyed, every
buffer ensure still goes through `HandleOfBuffer(obj)`); plus one family (`GetBufferBindingPoint`)
has no wire form yet.

## (e) Cost / benefit and recommendation

**Benefit bound.** One-verb pipelining overlaps the client's non-wait work with the server's apply.
Profile at 56a77348: client thread 39.5% of cycles, `WaitForApplied` 27.9 self / 34.2 incl, so the
client's overlappable work is ~5-12% of process cycles; the apply thread's real work (`DrainRing`)
is 29%. Best case, with the frame server-bound: ~10-20% of the critical path, ~1.5-3 ms of a
15.6 ms frame, i.e. 64 -> ~72-80 fps - and only if the snapshot costs nothing. The gap to monolith
(110-118) is the apply thread's own work (49% of it is the idle-poll mutex, package T) and the
per-draw guard/TLS/validate overhead (D, C): those cut both sides one-for-one and carry no barrier
change. After T/D/C land the client's non-wait share should be re-measured; if it falls under ~10%
the pipelining gain is inside the noise of a paired A/B.

**Cost.** (1) The full-copy snapshot (c) costs 2-4 ms/frame at 852 draws in refcount traffic alone and
is still incorrect ((b.2), (b.3)); the handle-only snapshot is cheap but presupposes the G6 rekey.
(2) The block itself has to be versioned/double-buffered: every stamp, fill, poison and verify
site assumes one `gPipeInputs` - this is exactly what `P5D-INPROC-PERFORMANCE.md:103-106` assigns to
P11 ("`gPipeInputs` 版本化/双缓冲"); note ROADMAP.md:37's P11 row (persistent map / adoption) does
not yet list that item - a doc gap to close whichever way this goes. (3) The 20 frontend-keyed
scope sites + `HandleOfBuffer` + `set_shader_buffers` = ROADMAP.md:32's P3b/P4b row verbatim.
(4) R-1's contract text (`ClientSession.cpp:889-898`, `PipeInputs.h:904-910`, ARCHITECTURE.md:528-530),
the R-1 negative control, `RefusePipeInputsTouchWhileApplierOwnsIt`, the strict lane's expected-red
and `rsp` accounting all have to be re-derived. (5) With the registries still frontend-keyed, the
7.4% emutls / 3% `HandleOf` on the apply thread get worse, not better, under any snapshot.

**Recommendation.** Do not take the deferred draw barrier into P5d round 4. The reads in (a) show
that on the inproc draw path every object-class row except `GetBufferBindingSlot` is dereferenced
live, and (b) shows that most of those dereferences are data races the moment the client moves on -
the racy identity path (twin tables keyed by frontend lifetime id, `HandleOfBuffer` through the
client allocator) cannot be snapshotted, only retired, and one family (`GetBufferBindingPoint`) has
no wire form at all. The cheap-and-safe subset that round 4 CAN take, because it needs no barrier
change and is split-only: (i) under a transport, drive `SyncVaoAttributeBuffersByHandle`'s walk
and the IBO arm off `st.VertexBuffers[].Res` / `st.IndexBuffer.Res` with
`EnsureBufferResourceForHandle(nullptr, h)`, removing the per-draw VAO attribute reads and their
`HandleOfBuffer` probes (also cuts emutls); (ii) skip the 15-slot `GetBufferBindingSlot` copy in the
kDraw residual fill when the wire is live, since no draw-path site reads it under a transport
(package C's residual-fill territory - coordinate rather than duplicate); (iii) after T/D/C,
re-profile the client's non-wait share. Sequence the real thing as P3b/P4b (handle-keyed twins,
`set_shader_buffers`) followed by P11's versioned `gPipeInputs`; the deferred barrier is one
`WaitForApplied` move (`ClientSession.cpp:917` -> `PipeFill.cpp:2790`) once both are in, and is
not worth a package before then.
