# CONTRACT-P5C — the wire contract for zeroing the inproc split's direct cross-role memory access

Authority: this file, beside `CONTRACT-P5.md` (table 0's encodings, the byte carriers, field
ownership, role/thread ownership, the knobs, R-1…R-17) and `CONTRACT-P5B.md` (the 25 migrated
class-C slots). Where it disagrees with either, this file is newer and wins, and §7 lists
every such place.

**How to change it.** Package c0c's file, edited by the integrator first. A P5c package (tx /
ev / hd / ct / rv / gt) that needs a row changed goes through the integrator; the packages
compile against the rows below from day one, which is the whole reason the file exists.

Base: `feat/disaggregated @ 11ac3de6` (the P5c plan commit; code-identical to the audit head
`a79a0af6`). Every `file:line` below was read at that commit. The audit this contract
discharges is `~/w7/notes/p5c/p5c-audit-v1.md` (59 sites); ROADMAP.md "P5c 计划" tabulates
them as T1–T5, B1–B4, R1–R5, C1, G1–G6, A1.

---

## §0 What P5c is, and the one rule above every row

P5/P5b put verb records, `SEG_STAGE` blobs, replies and caps snapshots on the wire. The audit
proved the two roles still read and write each other's memory directly at 59 sites, correct
only because the verb barrier and the shared address space happen to hold. P6 replaces how
the rings are delivered; it cannot replace a read of memory that does not exist in the other
process. **P5c makes `inproc` honest: between the two roles, nothing crosses except
`SEG_CMD` / `SEG_STAGE` / `SEG_REPLY` / `SEG_EVENT` / the caps snapshot / control frames —
so that P6 is a transport swap and nothing else.**

Rules A (a content record declares its bytes), B (no host pointer crosses) and C (an applier
entry point may not hold a pointer past its return) are CONTRACT-P5 §0's and bind every row
here unchanged. Rule D (a verb crosses AS THE CALL) is CONTRACT-P5B §0's and is NOT relaxed:
P5c changes WHERE the sink resolves the call's objects from (the record's handles, §3), not
what the record carries. P5c adds one rule:

**Rule E — a role may not name the other role's memory.** With an active transport, the
apply thread resolves every object from a handle the record carried (`§3`), every byte from
`SEG_STAGE` adopted into server-owned storage (`§2`), and every reverse notification is a
`SEG_EVENT` record (`§4`); the GL thread reaches server state only through replies, events
and the caps snapshot. A violation is `Fatal{RoleViolation, "<surface>"}` and the two guard
layers that raise it are §6's. "The barrier held" and "same address space" are no longer
accepted as mechanisms anywhere in this file.

Refusal vocabulary is CONTRACT-P5B's, unchanged: `Fatal{ProtocolCorruption, "<row>.<field>"}`
for a record the codec cannot prove, `Fatal{UnmigratedVerb, "<GL slot>[+<QUALIFIER>]"}` for a
shape no sink implements, a null backend slot DECLINES (`return false`). P5c adds one family:
`Fatal{RoleViolation, "<surface>"}` (§6) and `Fatal{EventRingOverflow}` (§4.4).

---

## §1 Table 0 additions — encodings the P5c rows introduce

| field | the ruling | zero means | who reads it |
|---|---|---|---|
| **reverse-channel `MGPBlobRef`** | An `OnBufferWriteback` blobref on the wire names **SEG_EVENT**: `Seg = Wire::kSegEvent` (4, `PipeWireCodec.h:62`), `Offset` = the byte offset of the inline payload inside the SEG_EVENT mapping (`EventRingConsumer::OffsetInSegment`, `EventRing.h:182-185`), `Size` = the head's `Size`, cross-checked against the record's tail. **Never a host address** — rule B binds the reverse direction exactly as it binds `MGHostSpan` (R-2). Under monolith the blobref keeps its P3a shape (`Seg = kMGHostSpanSegNone`, `Offset` IS the mapped address); the two shapes are distinguished by `Seg`, not by the transport knob. | `Size == 0` with a live resource is legal (a zero-length writeback) | the client consumer (`ResourceTracker.h`, §4.3) |
| **`MGPObjectDeath` reuses `MGPHandleOnly`** | `{Handle, Kind, Pad0}` (16 B, `MGPipeTypes.h:93-98`). `Kind` is the `MGPipeKind` of the dead frontend object, widened to `Uint32` exactly as the existing rows widen it. One payload for all seven kinds the death switch handles (`Managers.cpp:224-257`): Texture, Framebuffer, Renderbuffer, SamplerCso, ShaderCso, SamplerViewCso, VertexElementsCso. | `Handle` null = "the object never crossed" — the client emits NOTHING in that case (§5.2), so a null handle arriving is `Fatal{ProtocolCorruption, "ObjectDeath.Handle"}` | `ServerVerbSink::OnObjectDeath` |
| **`MGPApplierReset::ContextSerial`** | `Uint64`. The client-side context's make-current serial at the edge that primed it. P5c has exactly one context per session, so the value is ASSERTED equal to the session's, not dispatched on; P6's multi-context shape reads it for real. | 0 = the first make-current | `ServerVerbSink::OnApplierReset` |
| **`MGPContextValues`** | The rv field table (§5.3), one fixed-width POD, `MGP_ASSERT_POD`-pinned. Whole-record hash-suppressed like every other `set_*` call: it crosses only when one of its source values moved since the last emission. There is no dirty mask in the payload — a suppressed record means "nothing moved", never "field invalid". | — | `MGPipeApplySetContextValues`, the server accessors it feeds |
| **texture per-level extent, server-side** | `max(1, base_extent >> level)` computed from the descriptor's `Width/Height/Depth` and `Levels` (`MGPResourceDesc`, `MGPipeTypes.h:295-360`). The client does NOT emit per-level extents; the derivation is the mip chain's definition and two honest ends compute the same number. | — | tx's `SyncMipmapsToBackend` (§2.2) |
| **`EventKind` = 4: `kEventGlError`** | `EventRing.h`'s enum gains `kEventGlError = 4` beside the existing three (:46-51). Head `EventGlErrorHead { Uint32 Code; Uint32 MessageBytes; }` (8 B) followed by the NUL-terminated message inline, `MessageBytes = strlen + 1`, **capped at 1024** (a longer message is truncated at the producer; the cap is a static_assert, not a check). `Code` is the frontend `ErrorCode`, widened. | `MessageBytes == 0` is `Fatal{ProtocolCorruption, "kEventGlError"}` (the NUL travels, rule A's twin) | `ClientSession::DrainEventRing` |
| **`MGPSurfaceInfo` zero-extent semantics** | `Width == 0 && Height == 0` is the FORMAT-ONLY publication (the DirectGLES shape: the placeholder's extent is deliberately left alone, only the depth/stencil `InternalFormat` moves); a non-zero extent is the SWAPCHAIN shape (storage at the extent, then format). `Samples`/`Layers` are informational only (1/1 from Vulkan, 0/0 from GLES; the placeholder attachment model has no MSAA) and the consumer ignores them. | see left | the surface-changed consumer |

---

## §2 tx — the server's texture staged shadow

The texture half of `resource_subdata` stages the level bytes into `SEG_STAGE` today and
`ApplyTextureUpload` drops the pointer (`PipeApply.cpp:989-1036`: the gate, the accumulation
and the serial — the `bytes` parameter is used only by the non-null gate at :1006-1008).
Espryt then re-reads the client's `MipmapStorage` at sync time (Managers.cpp:6940, :7129,
:7198, :7315), reads per-level shape from the client object (:7314, :7332,
`DirectGLES.cpp:8502-8507`, :8528-8529), and falls back to the client for dirty regions with
no pending upload (Managers.cpp:7360, :7371). Magma WRITES the client's level storage
(`VulkanRenderer.cpp:1562-1609`: `AllocateStorage` + `MarkStorageDirty(..., false)`).
**tx ends all of it: the staged bytes are adopted, and the server's sync reads nothing but
its own shadow and the descriptor.**

### 2.1 `StagedTextureStore` — ownership and coverage

`MobileGL/MG_Remote/Server/StagedTextureStore.h`, the texture twin of `StagedShadow.h`
(R-11), and held to its four rulings:

1. **Keyed by the wire handle the record carried** (`KeyForHandle` = `{slot, gen}` under a
   tag bit) — AMENDED from "the twin's address" at tx's landing, for two reasons that
   together left no honest alternative: constructing a texture twin calls `glGenTextures`
   (Managers.cpp:5496), so a twin-address key either forces twin minting at adopt time (a
   driver name burnt for a texture that may never draw, and a GL context required in the
   R-16 unit case — buffer twins mint no GL and can be created lazily) or strands levels
   adopted before the first sync; and the handle is rule E's cross-role identity anyway,
   `{slot, gen}`-exact across a slot recycle. Magma's T5 path, which has no wire handle in
   its identity tables, keys by twin address under a SECOND tag bit, so the two key spaces
   can never collide. Every event that ends a key's life — respecify, destroy
   (`resource_destroy`'s texture arm, delivered precisely because the store is
   handle-keyed), context death — has a call site to drop it from.
2. **Coverage is the WHOLE STAGED RUN per (uploadTarget, level)** — AMENDED from "the
   region set" at tx's landing: a texture `resource_subdata` always stages the entire
   level shadow (`Blob.Size` IS the declared byte count), so the run is the honest unit;
   the region set stays what it always was — the dirty SHAPE the pending set carries.
   Region-precise coverage would `Fatal` on the legal
   texStorage→small-box `glTexSubImage`→conversion-fallback flow, whose sync reads the
   whole level. A sync that reaches a level no record covered is
   `Fatal{StageSnapshotTooNarrow, "<site>"}` — the same words as the buffer half, and for the
   same reason (`StagedShadow.h:96-116`: moving zeroes into the store is silent data loss,
   not a missing optimisation).
3. **`copies` is a constructor parameter**, false reproducing the monolith expression
   character for character (the client shadow answers, nothing is allocated). G1's pull-build
   byte-identity is preserved the same way R-11 preserved it.
4. **A header with diagnostics**, not a block inside Managers.cpp, so a unit case builds one
   store of each kind and asserts the difference (R-16 — `ServerLoopTest.cpp:616-760` is the
   shape the tests copy).

`PendingUpload` (`PipeApply.h:289-295`) gains NO byte pointer: the record's bytes move to the
store at apply time, and the pending set keeps its job of naming which (target, level, box)
is dirty-by-record. The store, not the pending set, is the byte owner; a pending upload with
no store coverage is the `Fatal{StageSnapshotTooNarrow}` case, not a silent re-read.

### 2.2 What the sync path reads after tx

| the read today | after tx |
|---|---|
| level texels from client `MipmapStorage` (`MapMipmapData`) | the store's adopted bytes |
| per-level extent from the client object | §1's derivation from the descriptor |
| texture target / levels count from the client object | the descriptor (`Target`, `Levels`; the `MGB_STORAGE_LEVELS` handle arm already reads `pushedStorage->Desc.Levels`, Managers.cpp:6651-6652) |
| dirty region, level HAS a pending upload | the record's regions (unchanged — already record-supplied, Managers.cpp:7358-7369) |
| dirty region, level has NO pending upload (GPU-generated mips, re-derivation) | **the server's own dirty mark on the shadow** (§2.3) — the client is never asked |
| level EXISTS at all (`Levels = N` says nothing about a sparse chain) | the store's DEFINED-NESS, fed by the respecify hook: a named level redefines that one, an immutable whole-resource respecify defines the whole chain (all six cube faces included, GL 4.6 core 8.19), a mutable whole-resource respecify defines nothing. Without this the §1 derivation would define `{0,0,0}`-extent holes into existence and change FBO/sampling completeness — added at tx's landing, the contract's own omission |

### 2.3 GPU-generated mips (T5) — who owns "dirty" now

A GPU-side generation (Magma's `GenerateMipmap`, Espryt's re-derivation arms) dirties the
SERVER's shadow, not the client's. The two backends land this in their own shapes, both
final: **Magma**'s `EnsureGenerateMipmapStorageAllocated` is replaced by a store operation
that marks the level Defined at the §1 extent and GpuDirty-in-shadow (the store answers
"exists" and "needs upload" for it; the client object is never touched). **Espryt**
generates the levels ON THE DRIVER, so no server shadow allocation is needed at all — the
levels are already where a sync would put them, and the shadow's staleness is the correct
state ruled below. Either way the client's `TextureObjectMipmap` is not touched from the
apply thread — `AllocateStorage` and `MarkStorageDirty` on it are §6 layer-1 surfaces.

The client shadow for such levels is stale and that is CORRECT: the two readers that could
observe the staleness are both named refusals under split — `glGetTexImage` served from the
client shadow (class C, P9) and `texture-remint-pull` (`Fatal{UnmigratedEmulation}`). This is
i1's copy-image-shadow-mirror ruling (CONTRACT-P5B §2) applied to the mip chain, and tx
reports which scenarios reach either Fatal.

### 2.4 The audit gate (E-P5c #2)

`MOBILEGL_IPC_AUDIT=1` fills retired staging with `0xDD` (R-11). After tx the texture bytes
are adopted before retire, so the `0xDD` fill covers them: **reverting the adoption to
pointer-dropping MUST turn one named texture scenario red on the four A/B traces.** That
revert is tx's red-once (R-16), and it is the gate that makes "the server consumes the staged
bytes" a checked fact rather than a design intention.

---

## §3 hd — sinks and twins resolve by the handle the record carried

The handle-keyed `GetOrCreate(MGPipeHandle)` (`SlotTables.h:278-326`) exists since P3a and
has deliberately no caller at the contract commit (:275-277). **P5c is its first caller.**
The rules:

1. **With an active transport, `MGPipeSlots()` is a client-only surface.** `Acquire`,
   `FindByLifetimeId`, `Free` called from the apply thread are `Fatal{RoleViolation,
   "MGPipeSlots"}` (§6 layer 1). The minting `GetOrCreate(const StatePtr&)`
   (`SlotTables.h:209-252`) and the death switch's `DestroyByLifetimeId` arms are
   monolith-only; split paths call the handle forms (`GetOrCreate(handle)`,
   `ReleaseByHandle`, :350-361).
   **THE NAMED EXEMPTION, ruled at the tx/ev/hd merge:** the sites whose handle-carrying
   records the client does not EMIT yet. `set_shader_buffers` and
   `set_stream_output_targets` sit in the catalogue but are P4b's to emit
   (`SetHashSuppressor.h` says so), so the buffer binding-point ensures they would feed
   (`EnsureBufferResource`→`HandleOfBuffer`, Managers.cpp:3199, and its SSBO/UBO/atomic/
   XFB/PBO callers) and the GPU-written announcement (`MarkBufferGpuWritten`'s handle
   answer, `MGPipeAnnounceBufferGpuWritten` for Magma) have no record handle to resolve
   from in P5c. Those probes run inside `MGPipeReverseAnnouncementScope`
   (`SlotAllocator.h`) — one read-only lifetime-id read, scoped, named and greppable,
   retired by P4b's emission (Espryt) and P7's server-side binding table (Magma). Every
   other apply-thread allocator access (`Acquire`, `Free`, and `FindByLifetimeId` outside
   the scope) stays Fatal with no exemption. The audit's B2 row is correspondingly
   re-scoped: what hd retires is every site a record DOES name; the rest is this scope.
   **THE SECOND NAMED EXEMPTION, ruled the same day:** the G6 frontend-keyed twin registry
   — every `HandleOf`/`Find`/`GetOrCreate(const StatePtr&)` probe the texture / sampler /
   sampler-view / VAO / program resolvers make, and the mailbox-delivered death switch
   until ct's record replaces the hop (ct keeps a documented NoSession server-only-fixture
   arm, and that arm is inside the scope too). These run inside
   `MGPipeFrontendKeyedRegistryScope` (`SlotAllocator.h`), the executable form of §5.4's
   pinned object-surface list: wrapping a site names the debt in code, an unwrapped probe
   still aborts, and the guard's death tests pin exactly that. Retires with the G6 row's
   own phases (P3b/P4b rekeys; the death switch's scope dies when the mailbox hop does).
2. **The records already carry the handles.** `MGPBlit::ReadFbo/DrawFbo`, `MGPMipPlan::Res`,
   `MGPCopyFromFramebuffer::Dst`, `MGPDrawIndirect::Buffer` / `ParameterBuffer`,
   `MGPGridInfo::IndirectBuffer` — hd's edit is to USE them, not to add fields. A site that
   needs an object no record names is a missing handle and stops at the integrator, not a
   quietly-added lifetime-id probe.
3. **Named blit.** `ServerVerbSink::OnBlit` resolves `ReadFbo`/`DrawFbo` through the FBO
   twin table by handle — AMENDED in shape at hd's landing: the named backend ENTRY's
   signature takes frontend `SharedPtr<FramebufferObject>` and the shared function table
   cannot grow a handle parameter without moving G1, so the record's handles reach the
   backend through a split-only verb-handle workspace in `MGPipeApplierState` (the sink
   writes it before dispatch, the backend reads it within the same verb), and
   `BlitFramebuffer` takes its named arm when the workspace names a pair: twins resolved
   by `GetOrCreateByHandle`, frontend objects reached through the twin table's state note
   (`NoteStateForHandle`/`StateForHandle` — a server-side note seeded by the record-driven
   sync, object-class, P3b/P4b retires it with the twin tables). A backend that does not
   consume the workspace declines LOUDLY (Magma until P7) — a blit that silently landed on
   the bound FBO would be a wrong picture, not a missing feature. The client's
   `ScopedBlitBindings` (`EmitTables.cpp:741-762`) is deleted from the split path — it exists
   only to stage values for the server's binding-slot read, and the read is gone. The
   record's `MGPBlit` payload is unchanged.
4. **CopyTex** (`DirectGLES.cpp:8560-8563`, :8655-8658) resolves the destination from
   `MGPCopyFromFramebuffer::Dst`; the `GetTextureUnitObject` unit-slot read is a layer-1
   surface at these two sites.
5. **Indirect buffers.** Draw-indirect / parameter / dispatch-indirect buffer objects come
   from the record's handle fields (CONTRACT-P5B §2 d1/i1 already put them there); the
   `GetBufferBindingSlot(DrawIndirect)` read at `DirectGLES.cpp:6702-6703` and its dispatch
   twin are layer-1 surfaces at those sites.
6. **`HasDefinedContent` is the descriptor's bit OR the staged coverage set** — AMENDED at
   hd's landing, because this contract's premise was wrong: the bit does NOT move only at a
   storage-defining call. The streaming idiom (`glBufferData(size, NULL)` then
   `glBufferSubData`) defines content through subdata with NO new descriptor
   (Managers.cpp:3130-3146 says so, `ServerLoopTest`'s streaming cases pin it), and a pure
   descriptor read would orphan live bytes at the next respecify. The answer is
   `Desc.HasDefinedContent || the staged coverage set is non-empty` — the coverage set IS
   "content records delivered since the last orphan", pointwise equivalent to the
   frontend's flag and purely server-side. `bufferObject->HasDefinedContent()`
   is a layer-1 surface.
7. **caps.** Magma's four reads of the client mirror (`DirectVulkan.cpp:713-715`,
   `VulkanRenderer.cpp:667-676`, `VertexInputStateFactory.cpp:249-250`, :502-503) read
   `ServerLoopInstance().Backend()->GetDynamicParameters()` — the server's own backend, the
   same source Espryt's `ActiveBackendFormatCaps` already prefers (`Utils.cpp:45-48`).
   `Utils.cpp:50`'s fallback arm to `pActiveBackendObject` is **refused** with an active
   transport (`Fatal{RoleViolation, "caps-mirror"}`): the server's backend exists whenever
   the session does, and a silent read of the mirror would hide a bring-up ordering defect.
8. **The legacy buffer arm is a named refusal under transport** (B3). `MappedData()` /
   `IsMapped()` / `GetChangeSerial()` / `SyncPersistentMappedRange()` on the frontend
   `BufferObject` (Managers.cpp:1012-1022, :1119-1121, :2944-2949, :3281-3327) take
   `Fatal{RoleViolation, "buffer-legacy-arm"}` from the apply thread — the same shape as
   `Fatal{PipeLegacyMemosDisabled}`, closing the arm that a cleared subsystem bit 7 currently
   leaves reachable with one `MGLOG_D`.

---

## §4 ev — SEG_EVENT is the only reverse channel

The segment, the ring, the three event heads and the full consumer exist
(`EventRing.h`, `ClientSession.cpp:176-267`); production has **zero producers**
(`ServerSession::PublishEvents`, :484-492, has no caller). ev wires the three producers and
retires every direct call into the client. The ten `MGPipeCallbacks` members stay ten; P5c
arms three and a half (writeback, gpu-written, surface-changed, and gl-error as the fourth
kind) — the remaining six keep CONTRACT-P5's P9 phase.

### 4.1 Producer ownership (Table 3 amendment)

With an active transport, the three reverse entries of `gMGPipeCallbacks` are installed by
the SERVER session (producer callbacks that Reserve + Publish into SEG_EVENT), never by the
client; `MGPipeInstallClientResourceCallbacks` (`ResourceTracker.h:606-613`) is
monolith-only. A second installation over a live entry is `Fatal{RoleViolation,
"callback-double-install"}` — the "never over an entry a backend already claimed" comment
becomes a check. The apply thread calling a client-installed callback (the R1/R2 shape) is a
layer-1 violation; the GL thread calling a server-installed producer is layer 2.

`DrainEventRing` calls the client consumers BY NAME (`MGPipeClientOnBufferWriteback` /
`MGPipeClientOnGpuWritten`, `ResourceTracker.h:553-601`), not through `gMGPipeCallbacks` —
the global table is a producer-side surface under split. The drain point is unchanged:
inside `EmitAndWait` after the barrier (`ClientSession.cpp:836`), the one instant the apply
thread is known to be outside the applier.

### 4.2 The three (+1) producers

| event | producer site (today's direct call) | payload |
|---|---|---|
| `kEventBufferWriteback` | `Ops_H_Readback`'s writeback (Managers.cpp:2339-2343): the server copies the mapped bytes INTO the event record's inline tail instead of casting the pointer into `MGPBlobRef::Offset` | `EventBufferWritebackHead` + inline bytes |
| `kEventGpuWritten` | `MarkBufferGpuWritten` (Managers.cpp:2722-2753) and Magma's three bypasses (`UniformManager.cpp:1075`, :1231, `VulkanRenderer.cpp:11618`) — Magma's direct `bufferObject->MarkGpuWritten()` calls are ROUTED THROUGH the callback, the bypass is deleted | `EventGpuWrittenHead` + `EventRange[RangeCount]` |
| `kEventSurfaceChanged` | `PublishDefaultFramebufferDepthStencilFormat` (`DirectGLES.cpp:11519-11610`) and the swapchain twin (`SwapchainObject.cpp:276-333`): the backend fills `MGPSurfaceInfo` and posts; it no longer calls `AllocateStorage`/`SetInternalFormat` on `pDefaultFramebufferInfo` | `EventSurfaceChangedHead` |
| `kEventGlError` | `PipeInputs::RecordError` (`PipeFill.cpp:1764-1772`) posts with `ErrorCode` + message (§1). **Ordering is P9's** (CONTRACT-P5 table 2 row `RecordError`): the event is observed at the next drain point, which preserves per-thread program order of error-then-read but not cross-verb interleaving; that is the accepted P5c shape and the contract says so rather than discovering it in P6. | `EventGlErrorHead` + inline message |

`PipeInputs::InvalidateCompileEnv` (`PipeFill.cpp:1753-1756`) is DELETED with an active
transport: R-12's caps re-publication (`CapsMirror.cpp:78-80`) already invalidates the
compile environment on the client, and the forward is a write into the frontend with no wire
shape. Its FieldOwnership.def row moves to FATAL under split (§5.4).

### 4.3 The consumer's segment arm (and the guard that has to die first)

`MGPipeClientOnBufferWriteback`'s monolith guard (`ResourceTracker.h:560-564`) currently
REJECTS any `Seg != kMGHostSpanSegNone` with `MGLOG_E_ONCE` — wiring a producer without
removing it would drop every writeback event on arrival. The replacement arm:

```
Seg == kSegEvent          -> resolve via the session's SegmentTable entry for kSegEvent
                             (installed at session start, ServerSession.cpp:333-335 and its
                             client twin); bounds-checked against the announced segment size
Seg == kMGHostSpanSegNone -> monolith only; with an active transport it is
                             Fatal{ProtocolCorruption, "OnBufferWriteback.Seg"} (rule B,
                             reverse direction)
anything else             -> Fatal{ProtocolCorruption, "OnBufferWriteback.Seg"}
```

The surface-changed consumer (stub at `ClientSession.cpp:240-256` today) APPLIES the
`MGPSurfaceInfo` to the client-owned `pDefaultFramebufferInfo` — the allocate/format writes
happen on the GL thread against client memory, which is where R3's ownership always was.

### 4.4 Overflow (P5c's ruling, P9 owns the policy)

All four events are **lossless** in P5c. `Reserve` returning nullptr is
`Fatal{EventRingOverflow}`, not a drop: the ring is 256 KiB, events are drained at every verb
barrier, and a full ring under lockstep means a producer burst no measured workload has.
`CountDrop` and the `eventRingFull` latch stay built and stay unused-by-policy, exactly as
`EventRing.h:14-16` rules — P9 decides between waiting and dropping; P5c makes overflow a
defect instead. The exit gate `eventDropped == 0` at drain points (E-P5c #3) is what checks
this row.

### 4.5 Round-trip red-once

Each of the three P5 events gets a unit round-trip beside `SessionTest.cpp:747-823` (the
fixture already drives one producer and one consumer over real segments), and ONE integration
scenario where reverting a producer to the raw-pointer shape turns the run red (E-P5c #3).
`kEventGlError` gets the unit round-trip only; its ordered-error sibling is P9's.

---

## §5 ct + rv — the two control records and the residual-value record

### 5.1 `applier_reset` — opcode 77, appended

`X(ApplierReset, MGPApplierReset, kScreen, kNone)`, appended after
`CopyFramebufferToTexture` (PipeCalls.def:233; opcode = position = 77).
`struct MGPApplierReset { Uint64 ContextSerial; };`, `MGP_ASSERT_POD(MGPApplierReset, 8)`.

- **Producer:** the GL thread, at the `tracker.FreshlyPrimed()` edge
  (`PipeFill.cpp:2654-2667`), with an active transport ONLY. The emit happens BEFORE the
  block's client-side resets (CsoCache, hash suppressor, vertex-input emitter — all client
  surfaces, they stay); the record's barrier is what orders the server's reset against every
  verb that follows.
- **Sink:** `ServerVerbSink::OnApplierReset(const MGPApplierReset&)` →
  `MGPipeApplierReset()` (`PipeApply.cpp:1202-1300`) after asserting `ContextSerial` against
  the session's. `g_applier` stays server-private (`PipeApply.cpp:409`).
- **The GL thread calling `MGPipeApplierReset()` directly with an active transport is
  `Fatal{RoleViolation, "g_applier"}`** (G2, layer 2). Monolith keeps the direct call —
  G1's byte-identity is untouched because the emit table exists only under split.

### 5.2 `object_death` — opcode 78, appended

`X(ObjectDeath, MGPHandleOnly, kCtxObject, kNone)` — the framebuffer family's FIRST wire
delete opcode (G3). Emitted by the GL thread from
`OnFrontendStateObjectDestroyed` (Managers.cpp:206-256) with an active transport:

1. Look the dying object's handle up in the CLIENT's own allocator
   (`MGPipeSlots().FindByLifetimeId` — legal: client surface, client thread).
2. **No handle → emit nothing.** The server never saw the object, so there is no twin to
   kill; this replaces the mailbox's unconditional delivery.
3. Handle → `EmitAndWait(ObjectDeath, {handle, kind})`. The wait keeps the death's
   ordering against in-flight verbs that name the handle, which is the only property the
   blocking `RunOnApplyThread` provided and the only one P5c keeps.

Sink: `ServerVerbSink::OnObjectDeath(const MGPHandleOnly&)` → the per-kind release
(`ReleaseByHandle`, `SlotTables.h:350-361`; the SamplerViewCso arm's idempotent
secondary path, Managers.cpp:239-256, is keyed by the view's handle the same way). The
mailbox hop (Managers.cpp:208-222) is deleted under split; it stays for monolith, where it
never fired anyway (the guard at :208 is transport-gated). **AMENDED at ct's landing:** a
documented NoSession fallback arm delivers a death to a server-only fixture's loop (a
transport configured with no client session is not a real split); with the §3.1
exemption, that arm's `DestroyByLifetimeId` probes ride inside the registry scope.

### 5.3 `set_context_values` — opcode 79, the rv field table

`X(SetContextValues, MGPContextValues, kCtxState, kNone)`. One POD, emitted at validate when
any covered value moved (the tracker's value-class dirty accounting already watches all of
them — "值类零新增记账", ARCHITECTURE §5.2), hash-suppressed as a whole. The fields and what
each retires:

| payload field | PipeInputs accessor it feeds | retires (FieldOwnership.def row) |
|---|---|---|
| `Uint32 ActiveTextureUnit` | `GetActiveTextureUnit` | BARRIER_PULLED "P3b/P4b" → RECORD_SUPPLIED |
| `Uint32 MaxTouchedTextureUnit` | `GetMaxTouchedTextureUnit` | BARRIER_PULLED "P3b/P4b" → RECORD_SUPPLIED (the hash-suppressed set's high-water mark rides THIS record's unsuppressed value) |
| `Uint32 TouchedBufferBindingPointCount[15]` (indexed by BufferTarget, the Coverage.def 15) | `GetTouchedBufferBindingPointCount(target)` | BARRIER_PULLED "P3b/P4b" → RECORD_SUPPLIED |
| `Uint8 IsTransformFeedbackActive`, `Uint8 IsTransformFeedbackPaused` | the two accessors | BARRIER_PULLED "P3b/P4b (Espryt), P7 (Magma)" → RECORD_SUPPLIED |
| `Uint64 TransformFeedbackGeneration`, `Uint64 BoundTransformFeedbackLifetimeId`, `Uint64 TransformFeedbackCapturedVertices` | the three accessors | BARRIER_PULLED → RECORD_SUPPLIED |

- **`GetCurrentVertexAttribute` does NOT ride this record.** Its problem is the applier's
  cross-view conversion (FieldOwnership.def:77-82), so `MGPVertexAttribDefaults` grows to
  carry the frontend's CONVERTED value (all three views as the frontend computed them), the
  same amendment shape as P5b's `MGPCopyRegion` 64 → 72 B (CONTRACT-P5B §2 i1). The row
  becomes RECORD_SUPPLIED; `set_vertex_attrib_defaults` keeps its opcode.
- **The three shutters** (`GetSamplingResolutionGeneration`, `GetTextureBindGeneration`,
  `GetTextureContextId`) carry NO wire field: they move to APPLIER_DERIVED, answered from the
  applier's own Serial — which is what their rows have said since P5 ("a shutter, not a
  value", FieldOwnership.def:94-102). rv's edit makes the accessor DO it.
- **`GetTransformFeedbackProgram` stays BARRIER_PULLED** (object-class:
  `SharedPtr<ProgramObject>`; the twin tables are P3b/P4b/P7's). rv's exit line "值类行为 0"
  (E-P5c #4) is checked against the value-class list pinned in `FieldOwnershipTest`, and the
  object-class rows are re-annotated with their retiring phase in the same edit so the test
  can pin that they are ALL the remaining rows.

### 5.4 FieldOwnership.def amendments (one list, made by c0c)

The generator's four classes are unchanged. c0c edits rows; rv/tx/hd land the code:

- The §5.3 value rows → RECORD_SUPPLIED (derived, via Coverage.def's emitted list gaining
  `SetContextValues` rows; the derivation's contradiction rule is what proves the edit
  landed).
- The three shutters → APPLIER_DERIVED ("the applier's Serial").
- `GetCurrentVertexAttribute` → RECORD_SUPPLIED (the amended payload).
- `InvalidateCompileEnv` (field + forward rows) → FATAL with mechanism "DELETED under split
  by P5c ev; R-12's caps re-publication replaced it" — the class is stated unconditionally
  because the table only ever describes the split server's reads (under monolith there is no
  server to read), so "under split" would be noise in a column that means it everywhere;
- **The texture family is NOT a set of new field rows** — a ruling, because the texel /
  per-level extent / dirty reads are reads of frontend OBJECT internals reached through
  `GetTextureUnitObject` / the `GetTextureObject` sticky forward, not `MGPipeInputField`
  enum values, and a row naming a non-field stops the generator by design. What pins them
  instead: (a) `GetTextureUnitObject`'s existing row is re-annotated — the tx-retired reads
  leave its site list, what remains is the unit-object POINTER reads (P3b/P4b/P7); (b) the
  object-surface list (texel bytes, per-level extent, dirty region, `HasDefinedContent`,
  `MappedData`/`IsMapped`/`GetChangeSerial`) is pinned verbatim in `FieldOwnershipTest` as
  the layer-1 surface set, and the §6 guard is what enforces it. This is the honest form of
  ROADMAP's "把纹理家族补进 FieldOwnership.def": the .def's mechanism covers fields, and the
  family's object reads get a pinned list plus a guard that can go red.

---

## §6 gt — the role guards (semantics, two layers)

With an active transport, in a split build only (`MOBILEGL_BUILD_DISAGGREGATED`; the pull
build's bytes do not move, G1):

**Layer 1 — apply thread may not touch frontend surfaces.** `Fatal{RoleViolation,
"<surface>"}` raised inside: `MGPipeSlots()` (all three entry points); the frontend
`BufferObject` legacy accessors (§3.8); the frontend `TextureObjectMipmap` /
`MipmapStorage` mutators and maps (`AllocateStorage`, `MarkStorageDirty`, `MapMipmapData`
and the storage-dirty queries); `pDefaultFramebufferInfo`'s attachment writes; the
client-caps-mirror fallback (`Utils.cpp:50`); any BARRIER-PULLED accessor whose row is no
longer BARRIER_PULLED after §5 (the stamp mechanism's `MGPipeInputUnfreshRead` already
aborts these — gt's edit is that it aborts as RoleViolation with the surface named, and the
VALUE-class set is empty at exit). Implementation surface: `ServerLoop::OnApplyThread()`
gated checks at the named entries; compiled out entirely in pull builds.

**Layer 2 — GL thread may not touch server surfaces.** `Fatal{RoleViolation, "<surface>"}`
raised on: direct `MGPipeApplierReset()` (§5.1); `ServerLoop::Backend()` /
`MGPipeGetResourceOps()` reads AFTER session start (the bring-up window before
`ClientSessionInstance().Start()`, `MG_Backend/Init.cpp:100-247`, is the documented exception — it is
single-threaded and pre-dates the roles); a write into `gPipeInputs` outside the residual
fill, and any client-side read of a server-stamped field outside a barrier wait.

**A1 is ruled: `InBarrierWait()` is WIRED, not deleted.** CONTRACT-P5 table 3's sentence —
"the client checks it when it touches `gPipeInputs` outside a barrier (`InBarrierWait` /
`ApplyThreadIsInsideApplier`)" — is currently half-true: `ApplyThreadIsInsideApplier` fires
at emit (`ClientSession.cpp:748`), `InBarrierWait` (`:880`) has zero readers. gt makes the
sentence true: the layer-2 `gPipeInputs` check consults `InBarrierWait()` exactly as the
contract describes. Deleting the sentence instead was considered and rejected: the check is
the only client-side half of the gPipeInputs single-writer rule, and P6 needs it armed, not
forgotten.

**Red-once (R-16), per layer, mandatory:** each layer is switched off in one named
integration scenario and the run MUST go red with the layer's Fatal — this is E-P5c #1's
"关掉任一层守卫必须能在一条具名用例上变红", and it is what makes the guard a gate rather
than a comment. CI gains one `MOBILEGL_IPC_STRICT_ERRORS=1` + guards-armed
`integration-split` lane.

**`rsp` at exit.** `ResidualPulls` enters the per-frame stats line and is MEASURED on the
four A/B traces; the remaining reads are exactly the object-class rows pinned by
`FieldOwnershipTest` (§5.3), value rows = 0. The known blind spot — sticky / non-verb
forwards bypassing the stamp counter (ROADMAP debt "rsp=35 只是下界") — is closed by gt:
the sticky-forward pull path (`MGPipeStickyForwardPull`, `PipeInputs.cpp:180-186`) counts
into `rsp` under the same verb stamp, so a read that escapes the stamp escapes nothing.

---

## §7 Amendments to CONTRACT-P5 / P5B (the complete list)

1. **Table 0** gains §1's six rows (reverse-channel blobref, ObjectDeath's payload reuse,
   ApplierReset::ContextSerial, MGPContextValues, server-side level extent, kEventGlError).
2. **Table 1** gains three rows: `applier_reset` (77), `object_death` (78),
   `set_context_values` (79) — all appended, opcode = position, never inserted.
3. **Table 2** (FieldOwnership.def): the §5.4 amendments; BARRIER_PULLED's value-class
   membership goes to zero and `FieldOwnershipTest` pins the remainder.
4. **Table 3**: the three reverse `MGPipeCallbacks` entries move from "client installs" to
   §4.1's producer ruling; `g_applier` gains "reset only via `applier_reset` under
   transport"; the `InBarrierWait` sentence becomes true (§6); the bring-up window is named
   as the one legal cross-role read.
5. **CONTRACT-P5B §2 f1's named blit**: "降为现有 bound backend 调用" is superseded by
   §3.3 — the sink resolves `ReadFbo`/`DrawFbo` from the record and calls the named entry;
   `ScopedBlitBindings` is deleted from the split path. P5b's "P5b 留下的 inproc 依赖"
   list loses that row.
6. **`MGPVertexAttribDefaults` grows** (§5.3) — the second payload-size amendment after
   P5b's `MGPCopyRegion`; the ABI fingerprint (`SessionRings.h:498-524`) is what catches a
   desync, and `PipeCatalogueTest` pins the new size.
7. **`EventKind` gains `kEventGlError = 4`** — the first event-kind addition; the kind
   space is append-only for the same reason the opcode space is.

## §8 Exit gates this contract serves (E-P5c, restated for the packages)

1. Guards armed: `integration-split` (107) green, broad inproc census with zero regressions
   vs `348d22a4`, 79 traces with no new first blocker; each guard layer red-once (§6).
2. `MOBILEGL_IPC_AUDIT=1`'s `0xDD` covers texture staged bytes on the four A/B traces;
   reverting adoption goes red (§2.4).
3. Three events round-trip; `eventDropped == 0` at drain points; producer→raw-pointer
   revert goes red (§4.5).
4. `rsp` measured per frame on the four A/B traces; remaining pulls are exactly the pinned
   object-class rows (§5.3, §6).
5. Redmi four-arm retest recorded (barrier tax re-measured after writeback / gpu-written
   move to events) — recorded, not gated.
6. G1 0/0/0/0, G2, G14, G5 as usual.

Explicitly NOT P5c (ROADMAP "留给后续阶段的"): object-class BARRIER_PULLED rows and the
frontend-keyed twin registry (P3b/P4b, P7); XFB scatter / `OnXfbScatterReady` /
`OnTexturePullRequest` / ordered `OnGlError` (P9); the EGL forwarders' control plane (P6);
copy-image shadow mirror and CopyTex texel writeback (P8/P9); `s_synced` /
`g_syncedRenderStateParameters` context-generation resets (P6).
