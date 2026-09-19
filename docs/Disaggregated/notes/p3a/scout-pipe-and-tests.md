# P3a scout — the MGPipe scaffolding and the gates

Tree read: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch
`feat/disaggregated`, HEAD `55d2af9b` ("make the handle-ABA negative control construct its own
collision and defeat the {slot, gen} generation"). Read-only; nothing edited.

What P3a is, per the plan the tree carries (`docs/Disaggregated/ROADMAP.md:19`): handle wave 1 on
Espryt — the seven `BufferBackendOps` become `resource_*` / `buffer_subdata_resident` (may be null) /
`resource_flush_range` / `resource_readback` / `map_persistent`; pool and deferred release move as-is;
vertex elements (all three calls, both backends); `set_vertex_buffers` with an explicit `baseInstance`
field; `set_index_buffer`; the Adreno SIGSEGV workaround stays. Acceptance: the full gate set,
buffer/VAO scenario families (`LargeArenaAdoption`, `StorageBufferRegrow` publishing
`map-persistent-roundtrips`, `VertexAttribBinding`, `MultiDraw`, `PrimitiveRestart`), Create/rd12/26.3/
sodium traces, and MC 26.3 p99 unchanged on Adreno. `ROADMAP.md:64` sets the rebaseline trip wire:
**P3a over 27 days means the narrow-handle-ification premise was wrong.** `ROADMAP.md:34` puts the
full `gl44to46` caselist (~56,271 cases) at P3a, because P3a is one of the five architecture
boundaries.

---

## 1. MG_Pipe: the catalogue and the payloads P3a fills in

### 1.1 `MobileGL/MG_Pipe/PipeCalls.def` — the call catalogue

The wire opcode is the **1-based position in `MGP_CALL_LIST`** (`PipeCalls.def:20-23`): appending is
the only legal edit; reordering is a protocol break; a retired call keeps its slot with a comment.
`MGP_CALL_LIST_DOCUMENTED_COUNT 71` (`:69`) is the authority and `MG_Test/Pipe/PipeCatalogueTest.cpp`
pins the arithmetic against the two generated tables.

The rows P3a lands (opcode = line order within `MGP_CALL_LIST`, which begins at `:74`):

| line | call | payload | class | flags |
|---|---|---|---|---|
| `:75` | `ResourceCreate` | `MGPResourceDesc` | `kScreen` | `kNone` |
| `:76` | `ResourceRespecify` | `MGPResourceDesc` | `kScreen` | `kNone` |
| `:77` | `ResourceDestroy` | `MGPHandleOnly` | `kScreen` | `kNone` |
| `:78` | `MapPersistent` | `MGPHandleOnly` | `kScreen` | `kReplySlot\|kOptional` |
| `:79` | `UnmapPersistent` | `MGPHandleOnly` | `kScreen` | `kOptional` |
| `:95` | `CreateVertexElements` | `MGPVertexElements` | `kCtxCso` | `kHasBlob` |
| `:96` | `BindVertexElements` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| `:97` | `DeleteVertexElements` | `MGPHandleOnly` | `kCtxCso` | `kNone` |
| `:108` | `SetVertexBuffers` | `MGPVertexBuffers` | `kCtxState` | `kVarTail` |
| `:109` | `SetIndexBuffer` | `MGPIndexBuffer` | `kCtxState` | `kNone` |
| `:110` | `SetIndirectBuffers` | `MGPIndirectBuffers` | `kCtxState` | `kNone` |
| `:128` | `ResourceSubData` | `MGPSubData` | `kCtxObject` | `kHasBlob\|kVarTail` |
| `:129` | `BufferSubDataResident` | `MGPSubData` | `kCtxObject` | `kHasBlob\|kOptional` |
| `:130` | `ResourceSubDataComplete` | `MGPSubDataComplete` | `kCtxObject` | `kNone` |
| `:131` | `ResourceFlushRange` | `MGPFlushRange` | `kCtxObject` | `kNone` |
| `:132` | `ResourceReadback` | `MGPReadback` | `kCtxObject` | `kReplySlot` |

**`kNeedsAck` is declared but used by no call in the tree today.** It is defined at
`MG_Pipe/MGPipe.h:42` (`1u << 0`, "the caller must not proceed until the server has acknowledged.
Rare by design"), listed in the `PipeCalls.def:18` flag legend, and referenced once more only as
prose in `MG_Remote/Transport/Doorbell.h:19`. **No `X(...)` row carries it** — verified by
`grep -rn kNeedsAck`. So if P3a wants `glBufferStorage` to be acknowledged, it is *adding the first
use of the flag*, not reusing an existing one: `ResourceCreate` (`:75`) / `ResourceRespecify` (`:76`)
currently carry `kNone`, and the immutable-storage discriminator already exists in the payload as
`MGPResourceDesc::Immutable` and `MGPResourceDesc::StorageFlags` ("glBufferStorage flags",
`MGPipeTypes.h:161-163`). Changing a flag word does not move an opcode, so this is an in-place edit,
but it does change the generated tables and therefore requires a `gen_pipe.py` regeneration + diff.

Deliberately **not** migrated, recorded in the footer `PipeCalls.def:163-177`: `GetIntegeri_v` /
`GetInteger64i_v` (the six compute-workgroup answers live in `MGPCaps::Dynamic`), `GetProgramiv`,
`ShaderStorageBlockBinding`, `set_pixel_unpack_state` ("no such state crosses the line"), a
compressed-format concept, `pipe_transfer`, and a stage dimension on `set_sampler_views`.

### 1.2 `MobileGL/MG_Pipe/MGPipeTypes.h` — the payloads

Every payload is `MGP_ASSERT_POD(T, Size)` (`:44-46`): trivially copyable **and** an exact stated
size. Adding a member is a build break until the number is updated, and the field list in
`PipeFields.def` must be updated in the same commit or `gen_pipe.py` refuses.

- `MGPResourceDesc` — `:150-176`, **88 bytes**. One discriminated descriptor for buffers, every
  texture target and renderbuffers. `Resource` handle, `Target`, `StorageKind`, `BindMask`
  (`:154-157`; the `ELEMENT_ARRAY` bit is the D-B7 switch: with `kCapNeedsHostIndexBytes` the server
  mirrors this resource), `InternalFormat`, `Width/Height/Depth`, `ArrayLayers/Levels/Samples`,
  `FixedSampleLocations`, `Immutable`, `Usage` (`BufferUsage`), `StorageFlags` (glBufferStorage
  flags), `HasDefinedContent` ("false after a NULL-data respecify"), `ImageBindableHint`
  (client-side `everImageBound`, pre-emptive allocation), `GlNameForDiag` (**diagnostics only** —
  `:167-170`: a GL name is never an identity, never a memo key, never part of a content hash),
  `ViewOf`, `BufferForTexBuffer`, `BufOffset`/`BufSize` with `kMGPipeWholeBuffer == ~0ull` (`:177`).
- `MGPSubData` — `:587-599`, **72 bytes**. `Res`, `Target`, `Level`,
  `SourceIsVerbatimLevelShadow` (`:590-592`, replaces the backend's `uploadData == mipData` pointer
  comparison), `UnionBox`, `RegionCount` (`MGPSubRegion[]` var-tail), `Blob`.
  **The UnionBox buffer convention** is `:579-586`: with `Target == Buffer` there is no level and no
  box, so the destination byte range rides in the box — `UnionBox.X` is the byte offset,
  `UnionBox.W` the byte size, `Y = Z = 0`, `H = D = 1`, `Level = 0`, `RegionCount = 0`, `Blob` holds
  exactly `Size` source bytes. That caps one record at a 2^31-1 offset and a 2^32-1 size; **the
  emitter must split a range beyond either**, and at `SEG_STAGE`'s 32 MiB the ring's half-capacity
  bound is the far tighter constraint. The three accessors are the *only* legal spelling:
  `MGPipeSetSubDataBufferRange(record, offset, size)` (`:603-611`, returns **false with the record
  untouched** when the range does not fit — that is the emitter's split signal),
  `MGPipeSubDataBufferOffset` (`:612-616`; reads `UnionBox.X` through `Uint32` so a corrupt negative
  X lands above the encodable bound and the applier's bounds gate refuses it) and
  `MGPipeSubDataBufferSize` (`:617`). Carrying source strides rather than inferring them is
  deliberate (`:562-565`): the old pointer test cannot survive a split.
  `MGPSubRegion` is `:566-573`, 40 bytes, `SrcRowStride`/`SrcSliceStride` where 0 means tightly
  packed. The union box **and** the region list both travel so the *server* picks the upload shape —
  Mali prices texture upload by job count, ~100 sprite rects vs one union box measured +6 ms/frame
  (`:575-577`).
- `MGPVertexBuffer` — `:356-364`, 32 bytes: `Res`, `Offset`, `Stride`, `Divisor`, `BindingIndex`.
  **There is no `baseInstance` field here today**; `ROADMAP.md:19` asks P3a for an explicit one, and
  `MGPDrawInfo` currently carries `StartInstance` (`PipeFields.def:189`).
- `MGPVertexBuffers` — `:367-371`, 16 bytes: `Start`, `Count`, `ContentHash`. Var-tail header;
  `MGPVertexBuffer[Count]` follows. The `ContentHash` is mandatory for **every** `kVarTail` `set_*`
  (`:350-352`): "or 26.2's redundant `glBindSampler` traffic reappears as a variable-length record
  per batch".
- `MGPIndexBuffer` — `:374-380`, 24 bytes: `Res`, `Offset`, `IndexSize`. `:373` states it is **an
  independent call, NOT a subset of the VAO configuration version (D5)**.
- `MGPIndirectBuffers` — `:382-386`, 16 bytes: `DrawIndirect`, `Parameter`.
- `MGPBufferRange` — `:435-440`, 24 bytes: `Res`, `Offset`, `Size`. No inline host span, by design
  (`:429-434`): the named-UBO host bytes needed under `kCapNeedsHostUboBytes` (D-B8) travel as an
  *optional second* var-tail of `MGHostSpan[HostSpanCount]` behind the ranges, so SSBO / atomic /
  XFB ranges do not each pay 32 dead bytes.
- `MGPShaderBuffers` — `:445-454`, 32 bytes: `Class` (Uniform | ShaderStorage | AtomicCounter),
  `Start`, `Count`, `WritableMask`, `HostSpanCount`, `ContentHash`. `HostSpanCount` is 0, or `Count`
  for the Uniform class under `kCapNeedsHostUboBytes`; a range with nothing to ship carries an empty
  span so the two arrays stay index-aligned.
- `MGPVertexElements` — `:253-…`, `kHasBlob`; fields `Cso`, `AttributeCount`, `BindingPointCount`,
  `Blob` (`PipeFields.def:71-72`).
- `MGPFlushRange` — `:629-635`, 32 bytes, carries **the application's real access flags**
  (`Flags<BufferMappingAccessBit>`), not a normalised subset.
- `MGPReadback` — `:637-641`, 24 bytes. `MGPSubDataComplete` — `:621-626`, 24 bytes, the forward
  terminator for a server-initiated texture pull, may carry zero regions.
- Shared primitives: `MGPBlobRef` `:55-61` (24 B; monolith `Seg == kMGHostSpanSegNone` and `Offset`
  is an address into the caller's staging arena), `MGPBox` `:70-74`, `MGPReplySlot` `:78-81`
  ("every server query in this catalogue is async-with-handle; none of them blocks"),
  `MGPHandleOnly` `:93-98` (16 B, `Handle` + widened `Kind`).
- Cap bits P3a cares about, `MGPCapBit` `:108-124`: `kCapResidentSubData` (bit 2),
  `kCapNeedsHostIndexBytes` (bit 7 — arms the index host mirror under split, D-B7),
  `kCapNeedsHostUboBytes` (bit 8, D-B8).

### 1.3 `MobileGL/MG_Pipe/MGPipeHandles.h` — identity

`MGPipeKind` `:24-40`: `None, Buffer, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso,
VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context, KindCount`.
`MGPipeHandle` `:54-61` is `{Uint32 Slot, Uint32 Gen}`, 8 bytes, alignment 4, trivially copyable
(`:63-65`). **Gen increments only when a SLOT IS REUSED — never on a respecify** (`:44-48`), so a
`glBufferData` on a live buffer keeps its handle. Two generations exist and are strictly separate
(`:50-53`): this one is the client's "is this the same GL object"; `MGGen` is the *server's* epoch
for "did I recast my driver object", and **no MGPipe call may require the client to know MGGen**.
Reserved: `kMGPipeNullHandle{0,0}` and `kMGPipeDefaultFramebuffer{0,1}` (`:72-73`),
`kMGPipeFirstAllocatableSlot = 1` (`:81`). ShaderCso has a reserved composite band,
`kMGPipeShaderCsoSlotLimit = 1<<20` and `kMGPipeShaderCsoCompositeSlotBase` = limit − limit/16
(`:88-94`) — not P3a's business but it constrains the allocator.

### 1.4 `MobileGL/MG_Impl/Pipe/SlotAllocator.h` — minting and recycling

`MGPipeSlotAllocator` (`:39-96`), one free list plus a high-water mark **per kind** (`kKindCount` =
`MGPipeKind::KindCount`, `:41`), so slots stay dense and the server's table is an array. API:
`Allocate(kind)` `:47`, `AllocateFor(kind, lifetimeId)` `:49`, `FindByLifetimeId` `:53`,
`Acquire(kind, lifetimeId)` `:55` (find-then-allocate — the ordinary client path),
`Free(kind, handle)` `:60` (**the Gen bump happens on the NEXT handout of the slot, not here**, so
a double free cannot skip a generation), `IsLive` `:62`, `GenOfSlot` `:65`, `LifetimeIdOfSlot` `:66`,
`HighWater` `:69` (what a server-side slot-indexed table must be sized to), `LiveCount`/`FreeCount`
`:70-71`, `Reset()` `:74`. Storage: `SlotState{Gen, Live, EverHandedOut, LifetimeId}` `:77-82`,
`KindState{Vector<SlotState> Slots, Vector<Uint32> FreeList, UnorderedMap<Uint64,Uint32>
ByLifetimeId, LiveCount}` `:84-90`. Process singleton `MGPipeSlots()` `:99`; compiled only under
`MOBILEGL_PIPE_PUSH` (`:34-36`). The lifetimeId→slot map is what keeps a GL name out of every key
(`:30-32`); it has nothing to do with `MG_State`'s `IndexGenerator`, whose LIFO name reuse is the
problem `{slot, gen}` exists to close (`:19-22`). Implementation: `SlotAllocator.cpp` (174 lines);
unit coverage `MG_Test/Pipe/SlotAllocatorTest.cpp` (recycle/gen/stale-free at `:63-102`, `:163`,
`:263`).

### 1.5 The death notice — and the hole P3a has to fill

`MobileGL/MG_State/GLState/StateObjectDeathNotice.h` (`MOBILEGL_PIPE_PUSH` only, `:12`):
`StateObjectDeathOps{ void (*OnDestroyed)(MGPipeKind, Uint64 lifetimeId) }` `:39-43`, a plain
non-atomic pointer `g_stateObjectDeathOps` `:45` written once at backend bring-up,
`SetStateObjectDeathOps` `:47`, and `NotifyStateObjectDestroyed(kind, lifetimeId)` `:55-59`. It
carries `{kind, lifetimeId}` and **not the object**, because by the time the last `SharedPtr` drops
there is nothing left to pass (`:23-26`); one entry point serves all kinds because the backend's
answer is the same for all of them: free the slot, drop the twin (`:26-27`). In a pull build the
header declares nothing (G1 requires a byte-identical symbol set).

Frontend raisers today (`grep NotifyStateObjectDestroyed`) — six, and **Buffer is not one of them**:

- `FramebufferState/FramebufferObject.cpp:36` (Framebuffer)
- `ProgramState/ProgramObject.cpp:43` (ShaderCso)
- `RenderbufferState/RenderbufferObject.cpp:40` (Renderbuffer)
- `SamplerState/SamplerObject.cpp:39` (SamplerCso)
- `TextureState/TextureObject.cpp:40` (Texture)
- `VertexArrayState/VertexArrayObject.cpp:53` (VertexElementsCso)

Backend consumer: `MG_Backend/DirectGLES/Managers.cpp:191-217`
`OnFrontendStateObjectDestroyed(kind, lifetimeId)`, installed as
`g_glesStateObjectDeathOps = {.OnDestroyed = OnFrontendStateObjectDestroyed}` `:219-221`. It drops
out early on `InProcessTeardown()` `:192` (a notice after `exit()` has begun is dropped — the twin
is a *deliberate* leak, `:187-190`), then switches to `<Kind>Impl::g_backend<Kind>Objects.
DestroyByLifetimeId(lifetimeId)`. **`MGPipeKind::Buffer` falls into `default:` with the comment
"Buffer already has its own death signal (`BufferBackendOps::OnDestroy`)"** (`:212-215`).

That existing buffer signal is `BufferBackendOps::OnDestroy(SharedPtr<BackendBufferResource>&&)`
declared at `MG_State/GLState/BufferState/BufferObject.h:103` and called from
`~BufferObject` at `BufferState/BufferObject.cpp:39-43` — it hands over the *resource*, not
`{kind, lifetimeId}`, and it fires only when `m_resource.Backend()` is set. **P3a must decide
whether the buffer slot is freed through the ops table's `OnDestroy` or by adding the seventh
`NotifyStateObjectDestroyed(MGPipeKind::Buffer, m_lifetimeId)` raiser and an arm in the
`Managers.cpp` switch.** The `SlotTables.h` header prose already says "All six re-keyed object
classes raise …" (`DirectGLES/SlotTables.h:38`), i.e. the seventh is exactly the P3a delta.

`DestroyByLifetimeId` is **static** and answered by *every live table of the kind*, not just the
global — a by-value copy held by a fixture or a context reset is a real configuration — and the slot
goes back **once, last**, after every holder has let go (`SlotTables.h:49-59`,
`Managers.cpp:180-185`). That holder-list discipline is load-bearing because the allocator erases
its lifetimeId→slot mapping on `Free`, so a notice resolved twice would find nothing the second time.

Other `SlotTables.h` facts P3a inherits: the key stops being a frontend heap address, so the
`weak_ptr` per entry stops being an identity test and `OwnerEquals` / three `TwinLookupMemo`s /
`UnitSamplerLookupMemo`'s owner compare lose their reason to exist (`:23-28`); the lookup becomes one
bounds check plus one array index, so a returned `BackendPtr*` survives the next `Find` (`:29-32`);
there is **no GC on this arm** — no draw tick, no creation tick, no sweep; the seven
`CollectGarbageIfNeeded` sites in `DirectGLES.cpp` drive the legacy registry only (`:43-47`). The
`weak_ptr` survives for one reason: `ForEachLive()` hands a strong reference to
`ScopedDetachedTextureFramebufferAttachments` (`:61-68`).

**Recorded debt P3a should be aware of, `SlotTables.h:70-77`:** this header is under `MG_Backend/`
and it *mints* handles (`MGPipeSlots().Acquire`) off a frontend `SharedPtr`'s `GetLifetimeId()`,
which contradicts `MGPipeHandles.h:13-16` ("minted by the CLIENT and never by the server"). It is
monolith glue; the minting and the resolution belong on the client and the backend should receive the
handle in the verb payload. **Not covered by any gate** — `check_include_closure.py` does not probe
`MG_Backend` headers.

Magma's side: `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h`. `MagmaPipeIdentityTable` (entry
storage `:369-378`: `m_entries`, `m_freeSlots`, `m_index`, plus a one-entry front memo
`m_lastLifetimeId/m_lastIndex/m_lastHandle`), and `MagmaPipeIdentityTables` `:384-402` owning
`m_vaos{"VertexElementsCso"}` and `m_buffers{"Buffer"}`. `HandleOf(kind, lifetimeId)` `:388-391`
routes `Buffer` to the buffer mint and everything else to the VAO mint — **a VAO is kind
`VertexElementsCso`** (`:386-387`). These are **per renderer, not process-global** (`:381-383`), so
Magma is *not* a holder in Espryt's holder list and does not mint out of `MGPipeSlots()`.
`MagmaPipeSlotIndex(handle)` `:411-417` is `handle.Slot - kMGPipeFirstAllocatableSlot` with an assert
(compiled out at INFO) and a ternary that has effect in a shipped build.
`MagmaPipeSlotTable<T, kChunkEntries=256>` `:427+` grows by appending chunks so **entry addresses
never move** — the draw path holds references across nested calls.
`VertexInputStateFactory.cpp:64` already calls
`m_identity->HandleOf(MGPipeKind::Buffer, attr.Buffer->GetLifetimeId())`, i.e. Magma's vertex-input
hash is already keyed on the buffer handle (commit `43f8b470`).

### 1.6 `PipeApply.{h,cpp}` — the in-process applier

`MG_Pipe/PipeApply.h` (150 lines) / `PipeApply.cpp` (667 lines), compiled **only** under
`MOBILEGL_PIPE_PUSH` (`PipeApply.h:30`). Under split this file becomes
`MG_Remote/Server/PipeApplier`; in the monolith it writes `MG_Backend/MGPipe/PipeInputs`'s
`gPipeInputs` directly, so a call and its effect are one function call apart (`:15-18`).
**The server's per-context working block *is* `PipeInputs::m_renderState`** (`:20-25`) — which is why
DirectGLES' `SyncRenderState` did not change a line, and why the verify comparator is a real oracle
("assembled == live", field by field, at every backend read).

Per-context stores, `MGPipeApplierState` `:48-87`:
- `Vector<MGPipeRenderStateCsoRecord> RenderStateCsos` `:51`, indexed by `MGPipeHandle::Slot`, slot 0
  never live. Each record `:42-46` = `{Uint32 Gen, Bool Live, Array<Uint8,
  kMGPipePipelineChunkBytes> PipelineBytes}` — it keeps the 396 pipeline bytes because an incremental
  `create_render_state` names only the chunks that moved against a `BaseCso`.
- `MGPipeHandle BoundRenderStateCso` `:53` — the last bind, so a rebind of the same handle needs no
  scatter.
- `ResidualValueBlock Residual` + `Bool HasResidual` `:56-57`.
- `Uint32 ScatteredChunkBits` `:77` — the applier's own ledger of which global chunk bits it (rather
  than the fill loop) wrote. Both trip wires arm off it, and the contract is spelled at `:62-76`:
  with the render-state subsystem OFF nothing is scattered, the ledger stays empty and the wires say
  nothing; with it ON the applier is the block's only writer. `set_patch_state`'s own write
  deliberately does **not** enter the ledger — a wire comparing against bytes it wrote itself is a
  tautology (`:74-76`).
- Four observable counters `:83-86`: `ResidualCapabilitiesCompared`, `ResidualDivergences`,
  `PatchCarrierComparisons`, `PatchCarrierDivergences`. Rationale `:79-82`: only a poison or verify
  build aborts, so the shipped push build counts and logs and a unit case can see the wire fire in
  *every* build.
- Singletons `MGPipeApplier()` `:90` (`PipeApply.cpp:393`) and `MGPipeApplierReset()` `:93`.

**Seven apply entry points today, and that is all of them** (`PipeApply.h:99-122`, definitions at
`PipeApply.cpp:407, 457, 476, 490, 499, 503, 561, 607`):
`MGPipeApplyCreateRenderState`, `…BindRenderState`, `…DeleteRenderState`, `…SetDynamicState`,
`…SetPixelPackState`, `…SetPatchState`, `…SetVertexAttribDefaults`, `…SetResidualValueState`.
**Nothing for buffers, resources, vertex elements, vertex buffers or the index buffer exists yet —
that is P3a's server half.**

Plus the derivation step (`PipeApply.h:124-149`): `MGPipeDeriveRenderStateFields(inputs)`
(`PipeApply.cpp:657`) recomputes every `PipeInputs` field that is a pure function of the working
`RenderStateParameters`, and `MGPipeDeriveRenderStateFieldsForChunks(inputs, globalChunkBits)`
(`:664`) is the scoped form the applier actually calls, so a per-frame `glViewport` (dynamic chunk D0
alone) does not pay the 8-wide blend loop, the 16-wide depth-range loop or the 35-arm capability
switch. Every guard's chunk set is computed with `MGPipeRenderStateChunkBitsCovering`, so a boundary
move cannot leave one stale. Trip-wire log tag: `MGP_TRIP_WIRE_TAG(name)` = `"Fatal{" name "}"`,
`PipeApply.cpp:29-31` — that is the marker G4 greps the retrace logs for.

Client-side CSO cache, `MG_Impl/Pipe/CsoCache.h` — the shape P3a's vertex-elements CSO should copy.
Lookup (`:14-25`): (1) widened `m_pipelineStateVersion` did not move → reuse the last handle, **zero
hashing, zero probing** — the steady state; (2) moved → hash the 396 pipeline bytes, probe, and
**confirm with a memcmp** before reuse (a bare 64-bit equality would alias two states onto one CSO —
silent wrong pixels with no gate that could see it; Mesa's `cso_cache` memcmps for the same reason);
(3) miss → mint a slot, emit `create_render_state` with every chunk, bind. Capacity 64
(`kMGPipeCsoCacheCapacity`, `:53`), ~26 KB per context, provisional pending the P13 retune.
Counters `:57-69`: `Mints`, `Binds`, `Hits`, `Collisions`, `Evictions`.

---

## 2. The P2 tracker and the emission step

### 2.1 `MobileGL/MG_Impl/Pipe/Tracker.h` (436 lines, header-only)

Header-only for an ownership reason, not a design one (`:40-44`): the root `CMakeLists.txt` that
would name a new `.cpp` belongs to package A and is frozen behind the `p2/contract` tag. Splitting it
out is one `list(APPEND)` line. **P3a will hit the same wall if it wants `Tracker.cpp`.**

`enum class MGPipeDirty` `:57-80`, 18 bits, one per row of `ARCHITECTURE.md 5.2`:
- value class, **emitted at P2** (`:58-63`): `NewRenderState` 0, `NewPipelineState` 1, `NewPixelPack`
  2, `NewPatchState` 3, `NewVertexAttribDefaults` 4.
- value class, **computed and counted, emitted from P3a on** (`:64-68`): `NewVertexElements` 5,
  `NewShader` 6, `NewShaderBindings` 7, `NewGlobalConstants` 8.
- object class, from P3b/P4b (`:69-78`): `NewVertexBuffers` 9, `NewIndexBuffer` 10, `NewFramebuffer`
  11, `NewSamplerViews` 12, `NewSamplers` 13, `NewShaderImages` 14, `NewConstBuffers` 15,
  `NewShaderBuffers` 16, `NewSoTargets` 17.

Names table `:95-114`, `kMGPipeDirtyEmittedAtP2` `:90-93`, `kMGPipeDirtyCount <= 32` asserted `:83`.
**`ROADMAP.md:19` puts `set_vertex_buffers` and `set_index_buffer` in P3a, but bits 9 and 10 are
labelled P3b/P4b in this enum — reconcile that before wiring, and update `:64-78`'s comments.**

`MGPipeSubsystemForDirty(bit)` `:119-135` maps a bit to its runtime `MOBILEGL_PIPE_PUSH` subsystem
and **returns 0 for bits 5..17** ("no call of their own until P3a/P3b/P4a/P4b, so there is no
subsystem to switch and the residual fill keeps supplying their fields"). Subsystem constants are in
`MG_Pipe/MGPipe.h:72-85`: bit 0 RenderState, 1 PixelPack, 2 PatchState, 3 VertexAttribDefaults,
4 ResidualValues, 5 EsprytSlots (Track H), 6 MagmaVertexInput (Track H); **bits 7..62 reserved,
allocated in ROADMAP order and never reused** (`:79` — an operator's recorded `0x7f` has to keep
meaning what it meant); bit 63 is `kMGPipeBehaviourNoCsoContentAddressing`, the negative control, not
a subsystem. Default for a push build with the knob unset: `kMGPipeSubsystemsMigratedAtP2 = 0x7f`
(`:85`). **P3a's new subsystem bits are 7 and up.**

`MGPipeMixShutter(acc, value)` `:143-146` is the composite shutter hash. Its collision caveat is
explicit (`:137-142`): a collision costs a *missed* fire, which is acceptable **for bits 5..17 and
only for them**, because nothing consumes those bits in P2 — "P3 replaces each with its own exact
shutter as it takes the subsystem over". The five P2 emits for are never composed.
`MGPipeWidenedCounter` `:159-177` widens a `Uint16` at the tracker boundary (`Observe` `:161`);
a decrease is a wrap and adds 65536. The one case it cannot see is stated at `:153-158`: a counter
advancing by exactly a multiple of 65536 inside one verb boundary reads as unchanged.

`MGPipeTracker::Update(ctx, verbClass)` `:188-307`, the dirty walk. A different context calls
`Reset()` first (`:192-195`) so the first walk publishes a complete state. Shutters:
- bits 0/1 `:202-205` — widened render-state and pipeline-state versions.
- bit 4 `:208` — `ctx.GetAnyVertexAttribDefaultGeneration()`.
- `vaoIdentity = MixShutter(vao->GetLifetimeId(), vao->GetConfigVersion())`, 0 when unbound
  (`:210-212`); bit 5 `NewVertexElements` **is exactly this** (`:213`).
- bits 6/7/8 `:219-236` from the current program's `GetLinkVersion` / `GetImageUnitVersion` +
  `GetBackendStateVersion` + `GetBlockBindingVersion` + `GetUniformWriteSetVersion` /
  `GetUBOContentVersion`. `:215-218` — deliberately **not** `GetProgramForDraw`, which joins a
  pending link; the tracker must not force a compile to answer "did the shader move".
- aggregate reads `:239-241`: `GetAnyTextureContentGeneration`, `GetAnyTextureParamsGeneration`,
  `buffers = ctx.GetAnyBufferChangeGeneration()`.
- bit 9 `NewVertexBuffers` = `MixShutter(ctx.GetAnyVaoAttributeGeneration(), vaoIdentity)` `:243-244`.
- bit 10 `NewIndexBuffer` = `MixShutter(buffers, vaoIdentity)` `:248`, with the reason at `:245-247`:
  the index buffer lives in the bound VAO's element slot, P2 has no cheap shutter for that slot
  alone, so it **shares the buffer aggregate and over-fires on any buffer write anywhere. "P3b
  narrows it when it takes the subsystem over."** — the narrowing P3a/P3b owes.
- bits 15/16 `NewConstBuffers`/`NewShaderBuffers` are the raw `buffers` aggregate `:259-260`;
  bit 17 mixes it with the XFB generation `:261-262`.
- bits 2 and 3 are `memcmp` shutters handled outside the loop (`:266-270`, `:275-293`) — bit 3
  because **NaN is a legal `glPatchParameterfv` value and must compare equal to itself**, which float
  equality denies and memcmp grants.
- latch and count `:271-272`, `:299-305`, behind `MG_Util::PipeStats::Enabled()`.

`Reset()` `:313-326` clears the latches but **deliberately not the fire tallies** (`:311-312`): they
are a per-run measurement. `ResetCounters()` `:328`. Readouts: `FireCount(bit[, verbClass])`
`:333-340`, `WalkCount` `:341-349`, `LastDirty()` `:350`, `Primed()` `:351`, `FreshlyPrimed()` `:355`.
**No timer lives here** (`:35-38`): ROADMAP forbids committing hot-path instrumentation; absolute
ns/draw comes from DriverBench, from outside the library.

Design rule P3a must keep (`:27-33`): **every shutter over-fires on purpose.** A bit that fires too
often costs one extra push; a bit that fires too rarely renders stale, and the P1 verify comparator
**cannot see that for object-class state** (it compares those by identity only). Narrowing is P3's
work, paid for with the fire rates this file publishes.

### 2.2 How a new call family gets an emitter — `MG_Impl/Pipe/PipeFill.cpp` (1198 lines)

`MGPipeValidateForVerb(verb)` `:1053-1197` is the validate point, five steps (declared in
`PipeFill.h:21-35`):

1. `:1055-1075` — parse the poison knob, arm verify, bump `filled.CurrentVerbSerial` (starts at 1,
   so a read before the first bump is `Fatal{UnmigratedPipeInput, "<Field>@<none>"}` rather than
   default storage, `:1068-1071`), set verb and context identity. `MG_Config::Features.PipeVerify`
   set without the compiled comparator emits `MGLOG_W_ONCE` `:1061-1064` — a lane's arming assertion
   turns that into red.
2. `:1084-1085` — the dirty walk.
3. `:1087-1128` — emission. The gate closure `wants(bit)` `:1093-1097` is
   `MGPipeSubsystemForDirty(bit) != 0 && (pushMask & subsystem) && (dirty & bit)`. `:1088-1091`
   states the rule: **every gate goes through `MGPipeSubsystemForDirty`, the one map**; naming the
   constants inline would be a second copy of the map in the only path that runs.
   `tracker.FreshlyPrimed()` handling is **before** the per-subsystem gates `:1110-1115` and resets
   `MGPipeCsoCacheInstance()`, `MGPipeApplierReset()`, `MGPipeSetHashSuppressorInstance().
   InvalidateAll()` and `g_residualDue = true` — it used to live inside `EmitRenderState`, which runs
   only when bit 0 is set, so a per-subsystem A/B that cleared bit 0 gave a fresh context a
   never-reset applier (`:1100-1109`). Then the four emitters `:1117-1128`
   (`EmitRenderState` `:982`, `EmitPixelPackState` `:767`, `EmitPatchState` `:777`,
   `EmitVertexAttribDefaults` `:824`); each returns `payloadBytes`.
4. `:1130-1161` — the residual fill. For each field in the verb class's mask: skip sticky fields
   (`:1136-1144`; under poison, stamp them once with `FilledGen = 1`), then compute
   `supplied = subsystem != 0 && (subsystem & kMGPipeWiredSubsystems) && (pushMask & subsystem) &&
   EmittedCallSuppliesTheWholeField(field) && (applierDerives || AppliedWithoutDerivation(field))`
   `:1150-1155`, and `CopyField` only when not supplied `:1156`. **The stamp is written either way**
   (`:1145-1149`) — a stamp says "this verb published this field", which is as true of an emitted
   field as of a copied one, and withholding it would abort every backend read of exactly the fields
   the migration just took over.
4b. `:1162-1186` — the residual value block. `g_residualDue` is armed **outside** the subsystem gate
   `:1163-1170` (whether the capability set may have moved is a fact about the frontend), and the
   block is emitted **after** step 4 `:1171-1174` because its trip wire compares against the
   assembled mirror; it is held rather than dropped at verbs whose class does not read
   `IsCapabilityEnabled` `:1176-1185`.
5. `:1187-1193` `PipeStats::RecordDrawPayloadBytes(payloadBytes)`; `:1194-1196` the verify
   `EntryCompare`.

**The wiring checklist for a new emitter** (from `:621-672`):
- add rows to `Coverage.def`'s `MGP_COVERAGE_EMITTED_LIST` (§2.4);
- add the emitter arm to `SubsystemForEmitter(MGPipeFieldEmitter)` `:624-638` — the map from a
  *field's emitter* to the subsystem;
- add the matching `MGPipeSubsystemForDirty` arm in `Tracker.h`;
- **add the `static_assert` pairing them.** `:640-644` states why: the two maps answer different
  questions (one takes an emitter, one takes a dirty bit) and must agree, because emission is gated
  on one and the residual-fill skip on the other; a divergence would push a call whose field is still
  pulled, or worse skip a field whose call was never emitted. Existing asserts `:645-663`;
- extend `kMGPipeWiredSubsystems` `:669-672` — "it grows one commit at a time, and a field whose
  emitter is not wired here keeps being pulled, so adding a row to `Coverage.def` can never silently
  drop a field on the floor before the call that carries it exists";
- extend `EmittedCallSuppliesTheWholeField` `:693+` if the call supplies a field completely.

Other machinery in this file P3a touches: `MGPipeNoteFrontendMutation` `:498`,
**`MGPipeNoteAggregate(aggregate)` `:518`** (§2.5), the seven F-class forwarders `:568-604`,
`MGPipeLeaveVerb` `:607`, `MGPipeVertexAttribDefaultRepairCount`/`…LastHeader` `:1049-1050`.

### 2.3 The set-hash suppressor — `MG_Impl/Pipe/SetHashSuppressor.h` (85 lines)

Coalescing rule 4 (`:12-13`): **every `kVarTail` `set_*` hashes the resolved set on the client and
does not emit when the hash has not moved.** `MGPipeSuppressorSlot` `:36-45` already has the slots:
`SetVertexBuffers = 0` (**marked P3b**), `SetSamplerViews`, `BindSamplerStates`, `SetShaderImages`,
`SetShaderBuffers`, `SetStreamOutputTargets`, `SetVertexAttribDefaults` (**the one P2 wired**).
`ShouldEmit(slot, contentHash)` `:53-59` latches and returns; **hash 0 is reserved for "never
emitted"** and a computed 0 is remapped to 1 (`:23-25`, `:54`) — one collision in 2^64 costs an extra
emission and never a missed one. `Invalidate(slot)` `:63`, `InvalidateAll()` `:65-67`,
`LastEmitted(slot)` `:71`. Singleton `:80-83`. This is the carrier for the ~175 lines of debounce
that move off the backends in P3b/P4b — Espryt's `UnitBindingsSnapshot` / `CaptureUnitBindings` /
`UnitBindingsUnchanged` and Magma's equivalents (`:15-21`).

### 2.4 `MobileGL/MG_Pipe/Coverage.def` — the emitted list and the semantic-merge trap

`MGP_COVERAGE_ACCESSOR_LIST` `:29-104` joins the 477-row read inventory
(`scripts/data/backend_read_inventory.md`, 57 files) against a pipe call. Three pseudo-calls stand
for rows that never become a forward call (`:19-24`): `kClientResolved`, `kReverseChannel`,
`kStructuralHandle`. Rows matching neither list are UNMAPPED — merely counted today, **zero from P5
on** (`:16-17`).

Buffer/VAO rows:
- `GetBoundVertexArray → BindVertexElements` `:36`
- `GetBufferBindingSlot → SetIndirectBuffers` `:42`, with the caveat `:37-41`: **the accessor is
  polymorphic over `BufferTarget` and its rows really split across `set_vertex_buffers`,
  `set_index_buffer`, `set_indirect_buffers` and `set_shader_buffers`**; the split waits on the
  inventory being re-vendored carrying the target argument (the extractor lives in MobileGL-CS,
  deferred out of P1). It is named for the plan's explicit `DrawIndirect`/`Parameter` replacement.
- `GetBufferBindingPoint` / `GetBufferBindingPointCount` / `GetTouchedBufferBindingPointCount →
  SetShaderBuffers` `:43-45`
- `MGP_COVERAGE_DELTA_LIST` `:128-130`: `handle-ify (wire handle) → kStructuralHandle`,
  **`Buffer ops delta → ResourceRespecify`**. This list is read by `gen_pipe.py` only, never by the
  preprocessor, because the delta kinds are the inventory's free-text labels, not C tokens.

`MGP_COVERAGE_STICKY_LIST` `:116-123` — exactly the seven F-class accessors whose value is valid
across verbs, so the poison's per-verb generation does not apply. The argument is uniform (`:106-115`):
each takes an argument that is not verb state (a GL name, a lifetime id, a target), so there is no
value the filler could copy and no verb whose fill could make it stale. **None of the
version/generation accessors is sticky** — those change under verbs and are precisely what the poison
must protect. A verify-lane `Fatal{UnmigratedPipeInput}` is fixed by a `FillPoints.def` row, **never**
by a sticky row, and `gen_pipe.py` refuses a name that is not an accessor above.

`MGP_COVERAGE_EMITTED_LIST` `:154-187` — which P2 call now *supplies* a `PipeInputs` field so the
residual fill loop can skip it; `gen_pipe.py` turns it into `kMGPipeFieldEmittedBy[]` in
`generated/PipeFilled.inc`. A field with no row here keeps going through the fill loop, **which is
what makes the `MOBILEGL_PIPE_PUSH` bitmask a true per-subsystem A/B rather than an all-or-nothing
switch** (`:135-136`). 33 rows, all render-state / patch / attrib-default. The one row whose call
differs from the accessor list's is `GetPrimitiveRestartIndex` (coverage maps it to `draw_vbo`
because that is where a backend reads it, but the value travels in dynamic chunk D6, so
`set_dynamic_state` supplies it) `:141-143`.

**`GetPixelStoreParameters` is deliberately absent, `:145-153`** — and this is the semantic-merge
trap. The field is `PipeInputs::m_pixelStore[2]` (pack **and** unpack) while `set_pixel_pack_state`
carries the pack half only, permanently (D10). A row here would be a half-truth: with the bitmask
bit set, the unpack half would be written by nothing while its poison stamp said it was published, so
neither the poison nor the verify comparator could see it. Until the field is split, the whole of it
goes through the fill loop and the pack half is written twice.

**How the trap actually fired (commit `bb2a236d`, "drop the emitter arm for set_pixel_pack_state that
spans retired"):** the spans work (`d1a7c5f1`) removed `GetPixelStoreParameters` from the emitted
list, so the generated `MGPipeFieldEmitter` **no longer had a `SetPixelPackState` enumerator** — but
the tracker's subsystem map and its `static_assert`, written against the contract, still named it, and
**the push and verify builds did not compile on the integrated tree.** The fix
(`PipeFill.cpp:651-657`) drops the emitter arm and keeps a `static_assert` on the *dirty bit* instead:
`MGPipeSubsystemForDirty(NewPixelPack) == kMGPipeSubsystemPixelPack`, because the bit is what the
emission gate consults while the field stays in the residual loop by design. **The lesson for P3a:
`Coverage.def`'s emitted list is generated-enum input, and every semantic merge that touches it must
be re-checked against `SubsystemForEmitter`'s switch arms and their static_asserts in `PipeFill.cpp`
and `Tracker.h`. Green on two branches separately is not green on the merge — build push AND verify.**

### 2.5 Fields, verbs, mutation surface

`MobileGL/MG_Pipe/PipeFields.def` — the G4 comparator's field lists, one macro per payload. Padding
is deliberately absent because verify must have **zero** false positives and a padding byte is what
makes a `memcmp` of `RenderStateParameters` falsely differ (`:9-13`). Hand maintained alongside
`MGPipeTypes.h`, `MGPipeValueTypes.h`, `MGPipeHostSpan.h`, `MG_Backend/BackendObject.h`;
`gen_pipe.py` asserts **in both modes, hence in `pipe-gates`**, that every list names exactly the
direct data members of its struct, `Pad<n>` excluded (`:16-19`). Rows relevant to P3a:
`MGP_FIELDS_MGPResourceDesc` `:44-48` (22 fields), `MGP_FIELDS_MGPVertexElements` `:71-72`,
`MGP_FIELDS_MGPVertexBuffer` `:96-97`, `MGP_FIELDS_MGPVertexBuffers` `:99-100`,
`MGP_FIELDS_MGPIndexBuffer` `:102-103`, `MGP_FIELDS_MGPIndirectBuffers` `:105-106`,
`MGP_FIELDS_MGPBufferRange` `:123-124`, `MGP_FIELDS_MGPShaderBuffers` `:126-127`.
`MGP_VERIFY_PAYLOAD_LIST` at `:302-319` is the emission order — **`gen_pipe.py` reads this list to
know what to emit**, so a new payload must be added there too.

`MobileGL/MG_Pipe/FillPoints.def` — `MGP_FILL_VERB_LIST(X)` `:73+`, one row per function-pointer
member of `MG_Backend::GLFunctionsTable` (`BackendObject.h`) **in declaration order**;
`gen_pipe.py` parses that struct and refuses a row set that is not exactly its member set (`:71-72`).
Nine verb classes are in use (`kDraw`, `kClear`, `kBlitOrCopy`, `kTextureOp`, `kReadback`,
`kDispatch`, `kQuery`, `kXfbSpan`, plus the class field masks). Note `:60-64`: retiring a
class-field-mask row would need the poison omission run across the **full CTS caselist on both
devices**, not the desktop corpus — and that is explicitly **recorded as P3a work**.
Formatting constraint: `gen_pipe.py`'s block regexes end at a blank line, so keep the empty line
after each macro (`:67`).

`MobileGL/MG_Pipe/PipeMutation.h` — `MGP_NOTE_MUTATION(Field)` `:79-80` and
`MGP_NOTE_AGGREGATE(Aggregate)` `:81-82`, both `((void)0)` in a pull build `:84-85`.
`enum class MGPipeAggregate` `:56-74`: `VaoAttribute` 0 (any VAO attribute format / buffer / enable
moved), `FramebufferAttachment`, `TextureContent`, `TextureParams`, **`BufferChange`** (any buffer
object contents moved), `VertexAttribDefault`. Counters are members of the owning `MG_State`
container, all under `MOBILEGL_PIPE_PUSH so the pull build's state objects do not change size (G1)`
(`:46-48`); the macro goes through a free function that finds the live `GLContext` because objects
have no back-pointer to their state (`:48-51`). **Monotonic and never reset** — the tracker widens and
compares, never subtracts (`:53-55`). Buffer bump sites are already in place:
`BufferState/BufferObject.cpp:47` (`NotifyRespecify`), `:55` (`NotifySubData`), `:65`
(`NotifyFlushMappedRange`), each also bumping `m_changeSerial`.

`MobileGL/MG_Pipe/DirtySurface.def` — the G9 completeness map. Verdict kinds include `kImmediate`,
`kExplicitDestroy`, `kUnpublishedDestroy`, `kNoBackendRead` (`:75`); `kExplicitDestroy` means the
mutation is published by the `delete_*` / `resource_destroy` call the Track H slice adds (`:98`).
Buffer/VAO rows: `MarkBufferObjectForDeletion → kExplicitDestroy` `:252`,
`MarkVertexArrayForDeletion → kExplicitDestroy` `:260` (and the other four kinds at `:253-259`).
`MGP_DIRTY_SURFACE_UNDECIDED_LIST` is **empty** today (`:285`) — a row can be parked there when the
scanner cannot answer. The TransformFeedbackBinding reasoning at `:239-249` is the worked example of
how a binding that moves no shutter is argued.

`MobileGL/MG_Pipe/MGPipe.h:87-121` shows what a generator run produces and where each `.inc` lands
inside the namespace: G1 `PipeTables.inc` (`:92`), G2 `PipeThunks.inc` (`:101`), G3 `PipeWire.inc`
(wire records, size assertions **and the applier's bounds precondition**, `:104`), G4
`PipeVerify.inc` (`:107`), G5 `PipeFilled.inc` (`:110`), G5b `PipeFillPoints.inc` (`:115`), G6
`PipeCoverage.inc` (`:118`), G7 `PipeSpanTable.inc` (`:121`). All eight are committed in
`MobileGL/MG_Pipe/generated/` — deliberately, so **the build never depends on python**.

---

## 3. The gates as they stand after P2

### 3.1 `scripts/check_include_closure.py` — interface purity

Four probes, `PROBES` at `:73-138`; each carries a `Why` printed in the summary, and `Allow` entries
are deliberately awkward — each needs a Reason printed on every run so a whitelist entry has to be
argued in the PR (`:70-72`). All four have empty `Allow` today.

| probe | header | forbidden | text limits |
|---|---|---|---|
| `value-header` `:74-88` | `MG_Pipe/MGPipeValueTypes.h` | `MG_State/`, `MG_Impl/`, `MG_Backend/`, `MG_Remote/` | — |
| `artifacts-header` `:89-108` | `MG_State/GLState/ProgramState/ProgramArtifacts.h` | `ShaderObject.h`, `MG_Util/ShaderTranspiler/`, `Config.h`, `MG_Backend/`, `BufferState/`, prefix `ProgramState/Shader` | `glslang:: ≤ 2` |
| `mutation-header` `:109-127` | `MG_Pipe/PipeMutation.h` | `MG_Impl/`, `MG_State/GLState/Core.h`, `MG_Remote/` | — |
| `wire-header` `:128-137` | `MG_Remote/Transport/ITransport.h` | `MobileGL/Includes.h` | — (the gate's own canary) |

`REQUIRED_PROBE_NAMES = ("value-header", "artifacts-header")` `:141` — deleting a probe must be red,
not quietly green. Modes `:35-40`: `text` (transitive walk of literal `#include` lines, no compiler),
`clang` (`-H -E` on a one-line probe TU, the ground truth), `both` (runs both and additionally fails
if they disagree about the violations). `--require-all` `:42-45` turns every SKIP (a probe whose
header does not exist yet) into a failure — the ratchet the integrator flips as headers land.
CI invocation, `.github/workflows/test.yml` job `include-graph-check` (`:738`, name "Include-closure
purity gate"): `python3 scripts/check_include_closure.py --mode both --compiler clang++-20
--self-test --require-all`, after `git submodule update --init include/ska 3rdparty/xxHash
3rdparty/Vulkan-Headers` and installing `clang-20 libx11-dev`. It does not depend on `build-linux`.
**`check_include_closure.py` does NOT probe `MG_Backend` headers** — which is exactly why
`SlotTables.h`'s client/server minting debt (§1.5) is uncaught.

### 3.2 `scripts/symbol_report.py --threshold 0` — G1

G1 is spelled `added == removed == resized == renamed == 0` and `.text` delta 0 (`:36-40`); the
canonical invocation is `--threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0` (`:40`).
`--fail-on-symbol-set-change` covers **all four buckets, not just added/removed**: a resize at zero
net delta is what a same-size rewrite produces (`:42-44`), and it always compares at threshold 0
whatever `--threshold` says (`:404-410` — `--threshold` is a *report* control, and a gate reading the
thresholded buckets would quietly weaken itself). `bucket()` `:176-207`; `gate_failures()`
`:251-269`; a `--self-test` (`:294-332`) pins the buckets and the three shapes that used to walk
through (a shrunk `.text` under a zero budget, a resize at zero net delta, …).

The **four admitted resizes** are the integrator's, not the script's — `wsl_p2_gate.sh:24`:
`RenderState::RenderState`, `RenderState::SetCapability`, `RenderState::IsCapabilityEnabled`,
`_GLOBAL__sub_I_DirectGLES.cpp`. The gate script runs
`symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so
--threshold 0 --markdown ~/w7/p2g-symbols.md` (`:25`) and then prints the Resized table (`:26`) for
the operator to compare against that list by eye. The CI counterpart is the `monolith-symbol-report`
job (`test.yml:1359`), **`workflow_dispatch` only**, which builds the baseline in a worktree at
`inputs.baseline_sha`, default `"087685d1"` (`test.yml:14-21` — "P1's G1 says the pull build is
byte-identical to `feat/disaggregated@087685d1`").

The gate script also pins the pull build's `SyncRenderState` body by sha256 of the
`namespace RenderStateImpl` block in `DirectGLES.cpp` against `~/w7/p2-before-syncrenderstate.sha`
(`wsl_p2_gate.sh:27-28`), and asserts `grep -rc pGLContext MobileGL/MG_Backend` is empty (`:23`).

### 3.3 The generator gates — CI job `pipe-gates` (`test.yml:1538-1617`)

`name: MGPipe generators and hygiene gates`, `runs-on: ubuntu-latest`, **deliberately independent of
`build-linux`** — source-level, seconds, and a broken build must not hide a drifted interface
(`:1541-1542`). Steps:

1. `Regenerate the MGPipe interface (G1-G7)` `:1553-1556`: `python3 scripts/gen_pipe.py` then
   `git diff --exit-code -- MobileGL/MG_Pipe/generated`.
2. `The MGPipe generators' checks can still fail` `:1562-1563`: `gen_pipe.py --self-test`. Rationale
   `:1558-1561`: regenerate-and-diff cannot see a check that silently stopped checking — a broken
   gate and a clean tree produce the same green. The controls are at `gen_pipe.py:1207-1246` — seven
   of them, each must trip, and **zero trips is itself an error** (`:1241-1242`), plus a mismatch
   count check `:1243-1244` and a positive control `:1240`. They cover: a struct member with no
   `F(...)`, an `F(...)` that is not a member, a payload with no struct, a verb missing from
   `FillPoints.def`, a verb that is not a `GLFunctionsTable` member, a field row naming a
   non-accessor, and **an emitted row naming a call that does not exist in `PipeCalls.def`**
   (`:1230-1234`) — the enum-generation guard the `bb2a236d` merge trap sits next to.
   The two structural checks it drives are `check_field_lists_cover_struct_members` `:416-421` and
   `check_call_payloads_have_field_lists` `:479-481`, **both of which run in `--check` mode too**.
   `parse_coverage` refuses an emitted field that is not an accessor `:522-523` or listed twice
   `:525`; `gen_emitted_by` `:809-845` refuses an emitted row whose call is not real `:817-819`.
3. `The symbol report's buckets and gates can still fail` `:1566-1567`:
   `symbol_report.py --self-test`.
4. `No stdio instrumentation in MG_Backend or MG_State` `:1576-1583`: greps for
   `fprintf(stderr|stdout)`, bare `printf(`, `puts(`, `std::cout|cerr`. **Zero exceptions today**, and
   any addition needs a PR reason rather than a quiet whitelist entry (`:1569-1575`) — per-draw
   instrumentation has been committed by accident before, once inside a mutex critical section.
5. `MGPipe dirty-surface mapping is complete (G9)` `:1601-1604`:
   `gen_pipe_dirty_surface.py --check` then `--self-test`. `--check` **fails both directions**
   (`:1586-1588`): a mutator the scanner finds with no row in the def, *and* a row naming a mutator
   the scan no longer finds — so a deleted mutator cannot leave a stale row claiming coverage. The
   self-test is not optional (`:1590-1594`): two canned negative controls (a mutator withheld, a row
   naming a function that does not exist). What it does **not** cover, stated in `DirtySurface.def`'s
   header rather than left implicit (`:1596-1600`): a mutation inside a lambda is attributed to the
   enclosing function, a mutation published through a helper reads as deferred, and it scans only
   `MG_Impl/GLImpl`, so the four `MGP_NOTE_MUTATION` sites in `MG_State` are outside it entirely. It
   is a completeness gate over what the scanner can see; **the semantic proof is the verify lane.**
6. `Documentation citation lint` `:1609-1617`: `check_doc_citations.py docs/Disaggregated/*.md`,
   **warning only** (`|| true`) until the documents settle.

Also independent of `build-linux`: `flatc-check` (`test.yml:710`) regenerates
`MG_Remote/Protocol/generated/protocol_generated.h` and `git diff --exit-code`s it.

### 3.4 The verify lane and the retraces

Compile-time: `MOBILEGL_PIPE_VERIFY` is an *option*, and `PipeInputs.h` derives `MOBILEGL_PIPE_POISON`
from it (`test.yml:313-314`, `:375-381`). Job `build-linux-verify` (`:315`) configures with
`-DMOBILEGL_PIPE_VERIFY=ON` (`:396`) and then has a hard artifact check,
**"The verify library really carries the comparator"** `:415-424`: if `libMobileGL.so` defines no
`MGPipeVerifyInputs` it errors with "…every lane that consumes this artifact would run the
comparator-free library and pass having compared nothing". It publishes
`mobilegl-linux-runtime-verify`.

Job `integration-verify` (`:484`) unpacks that runtime and runs:
- `ctest -L integration-verify` (`:524-549`) — 818 entries per the P2 brief.
- `Unit tests on the verify runtime (G6, G10)` `:568-570` — `ctest -L unit`, because
  `MGPipeRenderStateSpans.cpp` / `PipeApply.cpp` are appended to `SOURCE_FILES` only inside the
  `if (MOBILEGL_PIPE_PUSH)` block, and the push-only tests `GTEST_SKIP()` elsewhere (`:555-559`).
- `The handle-ABA and CSO-content-addressing controls (G8, G12)` `:589-600` —
  `ctest -L integration-gpu -R 'HandleRecycle|CsoContentAddressing'`. This is the **only** CI job
  that unpacks a `MOBILEGL_PIPE_PUSH` build (`:572-577`).
- `The verify lanes armed the comparator` `:615+` — catches what ctest cannot: an arming entry that
  *skipped* still reports green (`:612-614`).
- **Negative control A** `:641-656`: `MOBILEGL_PIPE_VERIFY_CORRUPT: GetRenderRateParameters`
  (`GetRenderStateParameters`) — the step passes when **ctest FAILS** (`if ctest …; then error`),
  the only shape that can catch a comparator that stopped comparing (`:633-637`). It counts matched
  tests first `:651` because an empty selection would also exit non-zero under `--no-tests=error`.
  The knob reaches the process through the **job** environment, not a ctest `ENVIRONMENT` property
  (`:637-640`) — the property-overrides-job-env trap.
- **Negative control B** `:670`: an omitted fill point must turn the lane red on that verb
  (`MOBILEGL_PIPE_POISON_OMIT`, `PipeFill.h:46-52`).

Retrace: `trace-cases` (`:961`) emits three matrices from `tools/trace_replay/trace_cases.json` —
**40 cases total, 39 CI-enabled, 77 full matrix entries, 8 verify cases → 16 verify matrix entries**
(computed by running `trace_cases.py` against the tree). `verify: true` is validated where the
manifest is parsed rather than where the matrix is built, so a typo is a loud manifest error in every
consumer (`trace_cases.py:29-37`), and a case marked `verify` but excluded from CI raises (`:35-37`).
Job `retrace-verify` (`:1202`) runs that 16-entry matrix, `max-parallel: 4`, `timeout-minutes: 240`,
needing `build-linux-verify` + `build-retrace` + `trace-cases` + `trace-fixtures`; it unpacks the
verify runtime over `build-retrace`'s frozen absolute library path (`:1259-1261`).

**The 79-case sweep is the integrator's local one, not CI's.** `~/w7/notes/p2/BRIEF-P2.md:37` (G3)
defines it: "79 desktop cases: 40 × 2 minus `iterationrp × DirectGLES`", driven by
`~/w7/retrace_gate.py` — a standalone script that parses the reference `build-retrace`
`CTestTestfile.cmake` for case parameters and per-test `ENVIRONMENT`, rewrites library/fixture/output
paths onto the tree under test, applies the CI per-case env, and runs `cmake -P run_trace_case.cmake`
with bounded parallelism, writing JSON + text summaries; exit 0 only when every selected case passes.
`wsl_p2_gate.sh:52-59` runs it twice (verify then push) and reports "verify armed: N of M" by
grepping for `MGPipe verify:` and lists every log carrying `Fatal{`.

Trigger note: `test.yml:4-12` — `push` on `dev`, `Feat/Backend-Direct-GLES`,
`Feat/Backend-Direct-Vulkan` and **`feat/disaggregated`**, the last marked
"TEMPORARY, remove before merging the MGPipe work into dev … so a phase's landing is not gated on
someone remembering to dispatch the workflow by hand", plus `workflow_dispatch`.

### 3.5 The HandleRecycle lanes and the G7 negative control

`MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp` (`MG_IntegrationTest/CMakeLists.txt:132`) is
registered **four times** — `DirectGLES.HandleRecycle.Handles.` `:877-878`,
`DirectVulkan.HandleRecycle.Handles.` `:886-887`, `DirectGLES.HandleRecycle.Legacy.` `:895-896`,
`DirectVulkan.HandleRecycle.Legacy.` `:904-905` — all filtering `HandleRecycleScenario.*`. The
`Handles` arm needs a backend keyed on `{slot, gen}` (packages C and D, `:326`); the CMake emits
explicit warnings when a configuration will make an arm SKIP (`:418`, `:445`, `:456`, `:459`) and
re-checks a marker before either arm asserts so a hand-forced configuration cannot pass silently
(`:376`). `:814` records that the scenario is meaningful in a pull build too (lifetimeId + weak_ptr
guards), so `ctest -R HandleRecycle` selects sensibly in both.

The arm selection is `ClassifyEsprytSlotArm(subsystemBitSet, legacyMemosEnabled)`
(`Managers.cpp:226-237`): with `MOBILEGL_PIPE_LEGACY_MEMOS`, bit 5 set → `Handles`, else
`PipeLegacyMemos` → `Legacy` else `NoArm`; without it, `Handles` is the only arm.
`ResolveEsprytSlotTablesArm()` `:255-292` resolves **at the first twin lookup, not at bring-up** —
deliberately, and it is the fix for a lane that went green by skipping: bring-up runs inside
`eglMakeCurrent`, which the harness pre-flights in a forked child, and a child that aborts is
reported as "no usable GPU" and skips every scenario (`:261-267`). `NoArm` is
`Fatal{PipeLegacyMemosDisabled, …}` + `std::abort()` `:278-282`, because returning instead would run
the very arm the operator disabled and hand back a green measured on it — "which is exactly the lever
`HandleRecycleScenario`'s arms are selected with" (`:269-277`). The recent fix `55d2af9b` makes the
ABA control construct its own collision and defeat the `{slot, gen}` generation.

**G7 negative control — `scripts/g7_negative_control.sh`.** Not a CI lane; it rebuilds the library
twice and is run by hand and by the integrator at the five-part gate (`:31-32`). It breaks the
pipeline/dynamic split on purpose — moves `ColorMasks` out of pipeline chunk P1 into a dynamic chunk
by inserting boundaries at `ColorMasks` and `FramebufferSrgbEnabled` (`:19-25`) — deliberately a break
the compiler **cannot** catch, since the chunks still ascend, do not overlap and cover
`[0, sizeof(RenderStateParameters))`, so every structural `static_assert` still holds. What breaks is
meaning: `glColorMask` bumps `m_pipelineStateVersion` but no longer moves the pipeline-subset hash,
and `RenderStateSpansTest`'s SetterConsistency has to **name `SetColorMask`**. The four measurement
pins (7/8 chunks, 396/772 bytes) are relaxed by the same patch, because leaving them would turn the
control into a build break — proving the assertions compile rather than that the test still checks
(`:27-29`). Exit codes `:44-51`: **0** = tripped *and* named `SetColorMask` (or, under
`--verify-patch-only`, the patch compiled); **1** = did not answer — either it stayed green on a
demoted member, or it went red without naming `SetColorMask` so the red cannot be attributed
(both are findings about the test, and both leave the tree restored and rebuilt); **2** = could not
run the control at all. Usage: `g7_negative_control.sh <build-dir> [--verify-patch-only]`, and
`<build-dir>` must carry the push-only unit tests since `MGPipeRenderStateSpans.cpp` compiles only
under `MOBILEGL_PIPE_PUSH` (`:37-39`).

### 3.6 `wsl_p2_gate.sh` — the integrator's gate script

`//wsl.localhost/Arch/home/swung/w7/notes/tools/wsl_p2_gate.sh`, 61 lines, run in `~/w7/pipe`.
Usage `wsl_p2_gate.sh [quick]` — `quick` skips the two 79-case retrace sweeps (`:3`).

**The fail-fast rule, `:4` and `:19`:** three builds are made first — `build-linux` (pull),
`build-push` (`-DMOBILEGL_PIPE_PUSH=ON`), `build-verify` (`-DMOBILEGL_PIPE_VERIFY=ON
-DMOBILEGL_ITEST_REQUIRE_GPU=ON`) — and **if any returns non-zero the script prints
`BUILD FAILED - gate aborted (no stale-binary verdicts)`, echoes `P2_GATE_DONE` and exits 1.**
"Results from a stale binary must never look like a verdict." It also prints the first two `error:`
lines of each build log (`:18`). Common flags `:10`: Ninja + clang/clang++ + ccache, `Release`,
`MOBILEGL_LOG_ACTIVE_LEVEL=INFO`, tests and integration tests ON, **`BENCHMARK=OFF`**, mesa EGL
vendor json and lvp Vulkan ICD, `CMAKE_POLICY_VERSION_MINIMUM=3.5`.
`export CCACHE_BASEDIR=/home/swung/w7` `:6`.

Parts:
- **Part 1, interface purity** `:21-29`: `check_include_closure.py`; `pGLContext` must not appear
  anywhere under `MG_Backend`; the G1 symbol report with the four admitted resizes named in the echo
  (`:24`); the `SyncRenderState` sha comparison; `HandleRecycle` under `build-verify`.
- **Part 5, coverage/poison/handle discipline** `:31-34`: `gen_pipe.py --check` and `--self-test`;
  `gen_pipe_dirty_surface.py --check` and `--self-test`; `ctest -R 'RenderStateSpans\.|Residual'` in
  `build-push`.
- **Part 3, behavioural A/B** `:36-48`: G2 — a `ctest -N` **name diff between pull and push must be
  zero lines** (`:39`); G14 — names removed/added versus `~/w7/p2-before-ctest-names.txt` (`:40`);
  `ctest -L unit` in all three builds; `ctest -L integration-gpu` in pull and push with a
  `--rerun-failed` pass; **`MOBILEGL_PIPE_PUSH=0` on the push build** (`:44`); the
  render-state-sensitive subset `ClipDistance|SampleMaskScope|SampleVariables|DualSourceBlend|
  ViewportArray|PrimitiveRestart` (`:45`); `g7_negative_control.sh build-push` (`:46`);
  `CsoContentAddressing` (`:47`); the controls `PoisonOmitted\.|VerifyCorrupted\.|HandleRecycle`
  under `build-verify` (`:48`).
- **Part 2, semantic shadow comparison** `:50-60`: `ctest -L integration-verify` counting `Fatal{`
  lines and the top five `Fatal{Kind, "detail"}` by frequency; then, unless `quick`, the two
  `retrace_gate.py` sweeps described in §3.4, with `MOBILEGL_PIPE_VERIFY=1` on the verify one, and
  the two summaries copied out before the output trees are deleted.
- Terminator: `echo P2_GATE_DONE` `:61`.

Sibling scripts in the same directory: `wsl_p2_bench.sh`, `wsl_p2_tree.sh`, `wsl_integrate_p2.sh`,
`p2_ab.sh`.

---

## 4. Measurement infrastructure P3a inherits — for RECORDING, not gating

### 4.1 `tools/trace_replay/run_android_retrace_local.py --benchmark`

`--benchmark` (`:383-387`) replays each case end to end as a frame-timing benchmark instead of
comparing images. Companions: `--benchmark-repeats` `:389`, `--benchmark-tail-frames` `:395`,
`--benchmark-no-finish` `:401`, `--benchmark-timeout-seconds` `:407-410` ("a benchmark replays the
whole trace, not just up to `target_call`"). `run_benchmark_case` `:312-361` passes `--benchmark`,
`--benchmark-tail-frames`, `--benchmark-finish 0|1`, deletes the stale `benchmark.json` first `:332`,
files each run as `benchmark-run<N>.json` (`read_benchmark` `:214-225`) and prints per-run plus
best-of lines `:344-361`.

`benchmark.json` carries the **whole `frameCpuTimesMs[]` array** on purpose (`cpu_tail` `:265-281`):
the device computes no p99, so p99 is a **host-side reduction over an artefact that already exists**.
The window is the same trailing `tailFrames` the device summarised, so the host numbers sit beside
the device's own rather than describing a different set of frames. `nearest_rank_percentile`
`:247-262` uses nearest rank, not an interpolating percentile, "so that every number printed here is a
frame that was actually observed" and so a host p95 agrees **exactly** with the device's for the same
window; `series_median` `:240-244` recomputes p50 by the device's own median rule so it agrees with
`medianFrameCpuMs` on the same run rather than merely sitting next to it (commit `af20dba6`).
`format_benchmark` `:284-302` prints `frames / total / tail / mean / median / p95 / fps` and then the
CPU half on the same line — **"it is what the disaggregation A/B is actually read on"**, because wall
time under `--benchmark-no-finish` still contains everything the retrace thread waited for
(`:294-296`). Per-frame thread CPU time was added by commit `e9499d38`
("record per-frame thread CPU time beside wall time so a paired A/B can be read as CPU cost").

### 4.2 `//wsl.localhost/Arch/home/swung/w7/notes/tools/p2_ab.sh` — the paired A/B driver

`p2_ab.sh <serial> [case ...]`, run from **Windows Git Bash** with `adb.exe` on PATH (`:6`).
Design (`:2`): `{pull, push} APK × {no-finish, finish} × cases × backends`, reboot-clean once, pinned
for the whole session with the pin **verified and retried**, one device-lock hold per run. Outputs go
to a Windows-local scratchpad dir because **Git Bash cannot create directories on `//wsl.localhost`**
(`:3-5`, and I hit exactly that: `mkdir //wsl.localhost/... → Read-only file system`, though the
`Write` tool and shell redirection into an existing directory both work); `copy_ab.sh` moves them into
`~/w7/notes/p2/ab/<serial>/`. **Do not export `MSYS_NO_PATHCONV`/`MSYS2_ARG_CONV_EXCL` globally here**
(`:7-8`): the runner passes `/c/...` paths to Windows python and relies on MSYS converting them.

Defaults `:10-16`: runner tree `…/MobileGL-disagg` ("must be at the integrated commit"), APKs read
over UNC from `~/w7/notes/p2/apk/trace-{pull,push}.apk`, pin helper from
`~/w7/notes/p2/devices/pin_device.sh`, package `top.mobilegl.plugin.trace`, four default cases:
`minecraft-1.21.4-in-world`, `minecraft-1.21.4-fabric-iris-bsl-in-world`,
`improved-transparency-minecraft-26.3`, `minecraft-1.21.4-startup`.
Session start `:24-35`: reboot, wait for device, poll `sys.boot_completed`, poll for root (24 × 5 s),
sleep 20, wake + unlock keyevent, uninstall the package, then **pin up to 3 times with a
`check` after each and exit 43 if none verified**. Device lock is a `mkdir` mutex with a 900 s timeout
and an owner file, exit 42 on timeout `:20-21`; `trap … EXIT` unlocks and unpins `:22`.
Per-run `:39-52`: `pin check` before and after, install the arm, `--benchmark-no-finish` for the
`nofinish` mode, then
`ANDROID_SERIAL=$SER python tools/trace_replay/run_android_retrace_local.py --case … --backend …
--benchmark $extra --benchmark-repeats 3 --benchmark-tail-frames 200 --env MOBILEGL_PIPE_STATS=1
--env MOBILEGL_PIPE_STATS_PERIOD=120`, copies `benchmark*.json` / `mobilegl.log` / `result.json` out
of `.trace-work/android-retrace-result/*-<backend>/`, and prints a one-line summary carrying
`rc`, `pin=<before>/<after>`, elapsed seconds, the `cpu mean=… p99=…` extract, and the count of
`MGPipe stats: frames` windows in the device log. Terminator `AB_DONE <serial>`.
`7a2e2561` is the commit that lets the trace APK be built in the push shape for this A/B.

### 4.3 `MobileGL/MG_Benchmark/Driver/run_driver_bench.sh` — DriverBench

`./run_driver_bench.sh native|espryt|magma [<libMobileGL.so>] [bench args…]` (`:2-5`). The bench
**dlopens exactly one EGL provider** through `DRIVERBENCH_EGL_LIB` — the system `libEGL.so.1` for
native, or the given `libMobileGL.so` for a backend — with **no `LD_LIBRARY_PATH` shadowing**, so
MobileGL's own loader still finds the real driver underneath (`:6-9`). Vendor libraries are pinned
explicitly (`:11-15`): a bare `libEGL.so.1` on a glvnd system picks whatever
`eglGetDisplay(EGL_DEFAULT_DISPLAY)` resolves first, which is Mesa/llvmpipe — "a software rasteriser
silently replacing the GPU under a benchmark"; override `MGL_EGL_VENDOR` / `MGL_VK_ICD` to test
another driver (defaults are the NVIDIA json/ICD, `:19-20`). `espryt` sets
`MOBILEGL_BACKEND_TYPE=DirectGLES`; `magma` sets `DirectVulkan` plus `VK_ICD_FILENAMES` (`:30-38`).
Binary override: `DRIVERBENCH_BIN` (`:18`). Sources: `Driver/DriverBench.c`,
`Driver/DriverBenchCases.inc`, and the on-device entry `MG_Util/SelfTest/DriverBenchJni.cpp`.
`Tracker.h:35-38` names DriverBench as the **only** source of absolute ns/draw, because it times whole
frames from outside the library and the tree forbids committing hot-path timers. Note that
`wsl_p2_gate.sh` configures with `-DMOBILEGL_BUILD_BENCHMARK=OFF`, so the gate build does not carry it.

### 4.4 `tools/device_bench/pin_device.sh` — the pin helper

`pin_device.sh <serial> pin|unpin|check` (`:2`). It exists because `bench.sh`'s
`pin_freqs()`/`unpin_freqs()` are MediaTek-legacy-only (`/proc/ppm` + `/proc/gpufreq`) and **neither
campaign device has those paths — on both of them `bench.sh`'s pin writes into nothing and exits 0**,
the silent "the run looks pinned but is not" failure the `PROFILE_VERIFIED` guard was built to catch
(`:4-9`). The prescribed sequence (`:12-15`): `pin`, then `bench.sh … --no-pin`, then `check ||
"PIN DRIFTED - discard this run"`, then `unpin`. All three actions print live big/little/GPU
frequencies, governors and the gate temperature, so the output of `pin` and `unpin` is itself the
before/after evidence (`:17-18`).

**`check` is deliberately three-valued** (`:20-30`), because "no pin at all" and "a pin that slipped"
are different facts: **0 PINNED** (every pinned node at its pin and every live frequency equal to it),
**1 DRIFT** (partly pinned, or a live frequency left its pin — *the dangerous state: a run overlapping
this is not comparable, discard it*), **2 UNPINNED** (no node at a pin this script set — the correct
state to leave a device in, and still non-zero so `pin_device.sh X check && measure` cannot silently
measure unpinned). UNPINNED is asserted against **our** pins, not the stock range, because ColorOS
moves `policy4`'s max on its own within seconds of a release and an exact-stock comparison reported
DRIFT on a correctly unpinned device (`:28-30`). Stock values are hardcoded from a 2026-09-07 reading
rather than sampled at pin time, because a restore reading "stock" off an already-pinned device would
make the pin permanent (`:32-36`). Git Bash hygiene: `export MSYS_NO_PATHCONV=1
MSYS2_ARG_CONV_EXCL='*'` so MSYS does not rewrite `/sys/...` and `/proc/...` arguments (`:39-40`).
Landed by `2d690754`; `13d7e32b` restored the executable bit on it and `profile.sh`.
Siblings in `tools/device_bench/`: `bench.sh`, `profile.sh`, `session.sh`, `devices/`, `README.md`.

**None of §4 gates anything.** The A/B numbers are recorded evidence; the only performance-shaped
acceptance criterion `ROADMAP.md:19` states for P3a is "MC 26.3 在 Adreno 上 p99 不变" — p99 unchanged,
read off `frameCpuTimesMs` through the host-side reduction in §4.1.

---

## 5. Short checklist for a P3a implementer

1. Flags/opcodes: edit `PipeCalls.def` in place for `kNeedsAck` (first use in the tree); **never**
   reorder. Regenerate and commit `MG_Pipe/generated/*.inc`.
2. Payloads: add the `baseInstance` field to `MGPVertexBuffer` → update its `MGP_ASSERT_POD` size,
   its `MGP_FIELDS_*` list, and (for a *new* payload) `MGP_VERIFY_PAYLOAD_LIST`.
3. Buffer sub-data: encode only through `MGPipeSetSubDataBufferRange` and **handle its `false`
   return by splitting**; the tighter real bound is the ring's half capacity, not 2^32.
4. Tracker: give bits 5 / 9 / 10 exact shutters, allocate subsystem bits **7 and up** in ROADMAP
   order, and wire `MGPipeSubsystemForDirty` + `SubsystemForEmitter` + the pairing `static_assert`
   + `kMGPipeWiredSubsystems` in the same commit. Reconcile bits 9/10's "P3b" comments with
   `ROADMAP.md:19`.
5. Suppressor: `MGPipeSuppressorSlot::SetVertexBuffers` already exists — use it; hash 0 is reserved.
6. Death: decide `BufferBackendOps::OnDestroy` vs. a seventh `NotifyStateObjectDestroyed` raiser, and
   respect the holder-list "resolve once, free the slot last" discipline.
7. Applier: add the new apply entry points and their per-context stores beside
   `MGPipeApplierState`; anything the applier writes into the working block must either enter the
   ledger or have a stated reason not to (the `set_patch_state` tautology precedent).
8. Residual: `MGL_RESIDUAL_BLOCK_SIZE` is **8** (`MGPipeTypes.h:546`), one `Uint64 CapabilityBits`
   (`:521-533`). It only ever goes **down**; shrinking the block without lowering the number, or
   growing it at all, is a build break (`:535-539`). **P3a must not add anything to
   `ResidualValueBlock`** — buffer, VAO, vertex-buffer and index-buffer state gets a real call, and if
   any of it is not ready it stays in the per-verb residual *fill loop* (`kMGPipeFieldEmittedBy` /
   `CopyField`), which is a different mechanism from this block.
9. Gates before pushing: `gen_pipe.py --check --self-test`, `gen_pipe_dirty_surface.py --check
   --self-test`, `check_include_closure.py --mode both --self-test --require-all`,
   `symbol_report.py --self-test`, and the full `wsl_p2_gate.sh` (build push **and** verify — that is
   where the `bb2a236d` merge trap surfaced).
