# P3a scout — Espryt's vertex-array surface

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated`.
Every line number below was opened. Paths are repo-relative to `MobileGL/` unless they start with `docs/`.

---

## 0. One-paragraph shape

Espryt has **no vertex-elements CSO**. It has a **per-VAO twin object** (`BackendVertexArrayObject`) that owns
one driver VAO name plus per-attribute scratch buffers, and that twin re-emits `glVertexAttribPointer` /
`glEnable|DisableVertexAttribArray` / `glVertexAttribDivisor` / `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER)`
**into the driver VAO** whenever a version compare says the frontend moved. So the three things P3a wants to
separate — vertex *elements* (format), vertex *buffers* (which store + offset + stride), and the *index* buffer —
are today one fused emit inside `BackendVertexArrayObject::SyncToBackend`, gated by one `Uint32` config version
plus one `Uint16` index-slot version plus one global buffer-id generation. That fusion is the P3a work.

---

## 1. How a draw resolves its vertex input today

### 1.1 The draw prologue

`PrepareForDraw` — `MG_Backend/DirectGLES/DirectGLES.cpp:3163-3231` (decl `Managers.h:105`).
Order of the vertex-relevant steps:

| line | step |
|---|---|
| `DirectGLES.cpp:3169` | `const auto& currentVAO = MGB_CTX->GetBoundVertexArray();` |
| `DirectGLES.cpp:3170-3171` | `vaoTwin = VertexArrayImpl::ResolveVaoTwin(currentVAO)` — **one** twin resolve serves the whole draw |
| `DirectGLES.cpp:3176` | early `vaoConfigVersion = currentVAO->GetConfigVersion()` (deliberate cold-line prefetch) |
| `DirectGLES.cpp:3193-3195` | `BufferImpl::SyncNeccessaryBuffers(currentVAO, vaoTwin, vaoConfigVersion, syncBit & IndexBuffer, syncBit & IndirectBuffer)` |
| `DirectGLES.cpp:3196` | `VertexArrayImpl::SyncCurrentVAO(currentVAO, vaoTwin)` → `vaoTwin->SyncToBackend(...)` |
| `DirectGLES.cpp:3212-3220` | `vaoTwin->Bind()`, else `BindBackendVAOId(0)` |
| `DirectGLES.cpp:3222` | `VertexArrayImpl::SyncCurrentVertexAttributeValues(vaoTwin, currentProgram)` |

`DrawSyncBit` (`Managers.h:74-83`): `None / IndexBuffer(1) / IndirectBuffer(2) / Instancing(4)`.
Note the client-array upload is **not** here — it runs *after* `PrepareForDraw` at the draw entry point (§1.6).

### 1.2 `ResolveVaoTwin` — `DirectGLES.cpp:1236-1268`

Two arms:
* `MOBILEGL_PIPE_PUSH` + `EsprytSlotTablesEnabled()` (`DirectGLES.cpp:1240-1252`): `g_backendVertexArrayObjects.Find(vao.get())`
  → `GetOrCreate(vao)` → `MakeShared<BackendVertexArrayObject>()`. **No lookup memo on this arm.**
* legacy (`DirectGLES.cpp:1254-1266`): `g_vaoTwinLookupMemo.Lookup(vao)` (a 4096-slot Fibonacci-hashed direct-map,
  declared `DirectGLES.cpp:128-129`, class at `DirectGLES.cpp:~80-121`) then the address-keyed registry.

`SyncCurrentVAO` — `DirectGLES.cpp:1270-1283`; it still calls `g_backendVertexArrayObjects.CollectGarbageIfNeeded()`
at `1275` (legacy arm only — the slot arm has no GC, see `SlotTables.h:37-48`).

### 1.3 `SyncNeccessaryBuffers` — `DirectGLES.cpp:572-707`

Resolves the *stores* the draw fetches through. It is the `ResolvedDrawBuffers` memo host.

* `DirectGLES.cpp:601` `const Uint64 bufferEpoch = CurrentBufferMutationEpoch();` (decl `Managers.h:588`, doc `Managers.h:548-587`)
* `DirectGLES.cpp:602-603` `auto* memo = vaoTwin ? &vaoTwin->GetResolvedDrawBuffersMemo() : nullptr;`
* **hit path** `605-621`: memo valid && `configVersion` matches → if `vboCleanEpoch != bufferEpoch`, re-probe each
  distinct entry with `IsBufferDrawClean(entry.frontend, entry.resource)` (`Managers.h:675`), and repair only the dirty
  ones via `EnsureBufferResource(currentVAOObject->GetAttribute(entry.attribIndex).Buffer)` (`DirectGLES.cpp:613-614`).
  Stamp `vboCleanEpoch = allClean ? bufferEpoch : 0` (`619`).
* **miss path** `622-664`: full walk over `GetAllAttributes()` (`629`), skip `!Enabled` / null `Buffer`, dedupe by
  frontend `BufferObject*` into a `MAX_VERTEX_ATTRIBS` scratch (`626-627, 634-642`), `EnsureBufferResource` each
  (`644`), rebuild memo entries (`645-651`), `memo->configVersion = configVersion; memo->valid = true; vboCleanEpoch = 0` (`654-660`).
* **IBO** `666-694`: `currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject()` (`669`), matched on
  **bound-object identity** `memo->iboFrontend == possibleIBO.get()` plus `iboCleanEpoch` (`673-687`) — explicitly not
  covered by the config version.
* **indirect** `696-703`: binds `GL_DRAW_INDIRECT_BUFFER` from `MGB_CTX->GetBufferBindingSlot(BufferTarget::DrawIndirect)`.
* tail `705-706`: `SyncBufferBindingPoints(ShaderStorage, GL_SHADER_STORAGE_BUFFER)` + `MarkShaderStorageBuffersGpuWritten()`.
  UBO binding points are deliberately **not** synced here (comment `696-704`) — the program rebind does it.

### 1.4 `ResolvedDrawBuffers` — `Managers.h:833-869`

```
struct Entry { BufferObject* frontend; GLESBufferResource* resource; Uint8 attribIndex; }  // Managers.h:849-853
Bool valid; Uint32 configVersion; Uint count; Array<Entry, MAX_VERTEX_ATTRIBS> entries;    // 854-857
BufferObject* iboFrontend; GLESBufferResource* iboResource;                                 // 858-859
Uint64 vboCleanEpoch; Uint64 iboCleanEpoch;                                                 // 866-867
```
Accessor `GetResolvedDrawBuffersMemo()` `Managers.h:870`; member `m_resolvedDrawBuffers` `Managers.h:911`.
Key: **VAO config version** for the VBO half, **bound-object identity + epoch** for the IBO half.

`PendingAttribValueMask` — `Managers.h:872-887` (`{valid, configVersion, activeMask, pendingMask}`), accessor
`Managers.h:884`, member `Managers.h:912`; consumed by `SyncCurrentVertexAttributeValues`.

### 1.5 `BackendVertexArrayObject::SyncToBackend` — `Managers.cpp:2419-2647`

This is the function P3a converts into `create/bind_vertex_elements_state` + `set_vertex_buffers` + `set_index_buffer`.

Gate (`Managers.cpp:2437-2470`):
```
currentConfigVersion      = stateVAOObject->GetConfigVersion();                          // 2437
currentIndexBufferVersion = ...GetIndexBufferBindingSlot().GetVersion();  // Uint16      // 2438
currentBufferIdGeneration = BufferImpl::g_bufferBackendIdGeneration;                     // 2446
bufferIdsRemitted         = m_syncedBufferIdGeneration != currentBufferIdGeneration;     // 2447
attributesDirty  = bufferIdsRemitted || !m_hasSyncedConfigVersion || version mismatch    // 2448-2449
indexBufferDirty = bufferIdsRemitted || version mismatch || OBJECT-IDENTITY mismatch     // 2452-2456
baseInstanceDirty= m_syncedFetchBaseInstance != g_pendingFetchBaseInstance               // 2462-2463
emitAttributes   = attributesDirty || baseInstanceDirty || m_hasConvertedFloat64Attribute// 2467
if (!emitAttributes && !indexBufferDirty) return;                                        // 2468-2470
```
Per-attribute walk `2477-2617` over `GetAllAttributeVersions()` / `GetAllAttributes()`:
* `SwitchVersion` → `glEnable/DisableVertexAttribArray` (`2489-2496`)
* `FormatVersion` / `BufferVersion` → format + buffer (`2498-2502`)
* **fp64 narrowing** `2521-2542`: `attrib.IsLong || attrib.Type == Float64` → `SyncFloat64AttributeAsFloat32`; on
  failure the array is **disabled** (Adreno SIGSEGV note `2495-2520`, `KHR-GL43.vertex_attrib_binding.basic-input-case4`)
* **zero-stride** `2545-2560`: `attrib.Stride == 0 && HasVertexBindingApi()` → `SyncZeroStrideAttribute` (binding-point API)
* `BindAttributeBuffer` (`Managers.cpp:2330-2346`) then `glVertexAttribPointer` / `glVertexAttribIPointer` at
  `fetchOffset = attrib.Offset + BaseInstanceByteShift(attrib, fetchBaseInstance)` (`2586-2600`)
* **BGRA refusal probe** `2568-2582` + `2602-2611`: `glGetError` drain, attempt, re-check, disable on refusal
  (`basic-input-case5`). Deliberately the only format that pays a `glGetError` round trip.
* `glVertexAttribDivisor` on `needsSyncFormat` (`2613-2615`)

Index half `2618-2638`: `EnsureBufferResource(indexBufferBinding)` → `BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, id)`,
else `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)`; stamps `m_syncedIndexBufferVersion` + `m_syncedIndexBufferObject` (`2634-2637`).
Stamps at `2640-2646`.

Twin private state — `Managers.h:906-953`:
```
m_resolvedDrawBuffers, m_pendingAttribValueMask                                  // 911-912
Uint m_backendVAOId                                                              // 913
Array<Uint,32> m_clientAttributeBufferIds                                        // 914
Array<Uint,32> m_convertedAttributeBufferIds                                     // 918
Array<ConvertedFloat64Stream,32> m_convertedAttributeStreams                     // 919-920
Bool m_hasConvertedFloat64Attribute                                              // 924
Uint16 m_syncedIndexBufferVersion                                                // 927
const BufferObject* m_syncedIndexBufferObject   (raw, never dereferenced)        // 931
Bool m_hasSyncedConfigVersion; Uint32 m_syncedConfigVersion                      // 937-938
Array<VertexAttributeVersion,32> m_syncedAttributeVersions                       // 939-940
Uint32 m_syncedFetchBaseInstance                                                 // 946
Uint64 m_syncedBufferIdGeneration                                                // 952
```
ctor/dtor `Managers.cpp:2259-2296` (`glGenVertexArrays`; dtor deletes the VAO name and both scratch-buffer arrays,
early-returns under `InProcessTeardown()`).
`Bind()` `Managers.cpp:2323-2327` → `BindBackendVAOId` shadow (`Managers.cpp:2303-2312`), `InvalidateVAOBindingCache`
`2314-2316`, `NoteVAOIdDeleted` `2318-2321`.

### 1.6 Client-array upload path

`BackendVertexArrayObject::SyncClientSideAttributesForDrawArrays(vao, first, count)` — `Managers.cpp:2651-2755`
(decl `Managers.h:831-832`). Selects `attrib.Enabled && !attrib.Buffer` (`2662-2664`), then:
* fp64 client array `2670-2715`: `NarrowDoubleStreamToFloat32` → `glBufferData(GL_STREAM_DRAW)` into
  `m_clientAttributeBufferIds[attribIndex]`, counts `PipeStats::ByteClass::StageVertexClient` (`2701-2704`)
* ordinary `2717-2752`: `uploadSize = (first + count - 1) * stride + elementSize` (`2726`) — this is the exact
  formula ARCHITECTURE.md 5.7 assigns to the client; `glBufferData` then `glVertexAttribPointer(..., nullptr)`;
  bytes counted at `2740-2743`.

Call sites (both **after** `PrepareForDraw`, both re-`Find` the twin instead of reusing the resolve):
* `DrawArrays` — `DirectGLES.cpp:4805-4821` (`Find` at `4814`, call at `4817`)
* `MultiDrawArrays` — `DirectGLES.cpp:4837-4866`, per sub-draw range (`Find` at `4855`, call at `4857`)

There is **no** client-array upload on any `DrawElements*` path — `SyncClientSideAttributesForDrawArrays` is the only
uploader and only `DrawArrays`/`MultiDrawArrays` call it.

### 1.7 fp64 vertex narrowing

`SyncFloat64AttributeAsFloat32` — `Managers.cpp:2757-2874` (decl `Managers.h:889-892`).
* `bufferObject->SyncGpuWrites()` before reading the shadow (`2776`); `MappedData()` / `GetSize()` (`2777-2778`)
* stride-0 = "never advances" → exactly one element converted (`2793-2796`)
* baseInstance folds into `firstElement` (`2800-2806`)
* memo `ConvertedFloat64Stream` keyed on `{sourceLifetimeId, sourceChangeSerial, sourceOffset, sourceStride,
  componentCount, elementCount}` and **never trusted for a persistently mapped buffer** (`2812-2820`)
* emits `StageVertexClient` bytes (`2839-2845`)
* stride-0 output requires the binding API (`2857-2866`), else `glVertexAttribPointer` with `normalized = GL_FALSE`
  deliberately (`2869-2873`, `basic-input-case5`)

### 1.8 `SyncCurrentVertexAttributeValues` — `DirectGLES.cpp:1291-1348`

Feeds `glVertexAttrib4fv / I4iv / I4uiv` for program-active locations with **no enabled array**, from
`MGB_CTX->GetCurrentVertexAttribute(location)` (`1329`). Memo keyed on `{VAO config version, program active-attrib mask}`
(`1311-1323`). This is the read that `set_vertex_attrib_defaults` (already landed at P2) supplies.

### 1.9 Complete list of Espryt reads of frontend vertex state

Every `MG_Backend/DirectGLES` read of `VertexArrayObject` / `VertexAttribute` / `VertexBufferBindingPoint`:

| file:line | read |
|---|---|
| `DirectGLES.cpp:128-129` | `TwinLookupMemo<VertexArrayObject, BackendVertexArrayObject, 12>` (legacy arm) |
| `DirectGLES.cpp:572-573` | `SyncNeccessaryBuffers(SharedPtr<VertexArrayObject>&, BackendVertexArrayObject*, ...)` |
| `DirectGLES.cpp:613-614` | `GetAttribute(entry.attribIndex).Buffer` |
| `DirectGLES.cpp:626-627` | `MAX_VERTEX_ATTRIBS` dedupe scratch |
| `DirectGLES.cpp:629` | `GetAllAttributes()` |
| `DirectGLES.cpp:669` | `GetIndexBufferBindingSlot().GetBoundObject()` |
| `DirectGLES.cpp:1236-1266` | `ResolveVaoTwin` |
| `DirectGLES.cpp:1270-1283` | `SyncCurrentVAO` |
| `DirectGLES.cpp:1291-1348` | `SyncCurrentVertexAttributeValues` (`1312` config version, `1317` `GetAttribute(location).Enabled`) |
| `DirectGLES.cpp:3169-3176` | `GetBoundVertexArray()`, `GetConfigVersion()` |
| `DirectGLES.cpp:4624` | `BoundElementArrayBuffer()` → `GetIndexBufferBindingSlot().GetBoundObject()` (restart path) |
| `DirectGLES.cpp:4814`, `4855` | `g_backendVertexArrayObjects.Find(currentVAO.get())` (client arrays) |
| `Managers.cpp:210` | death-notice case `MGPipeKind::VertexElementsCso` |
| `Managers.cpp:2330` | `BindAttributeBuffer(const VertexAttribute&)` |
| `Managers.cpp:2377` | `BaseInstanceByteShift(const VertexAttribute&, Uint32)` |
| `Managers.cpp:2387` | `SyncZeroStrideAttribute(Uint, const VertexAttribute&)` |
| `Managers.cpp:2437-2438`, `2453` | config version, index-slot version, index bound object |
| `Managers.cpp:2477-2478` | `GetAllAttributeVersions()`, `GetAllAttributes()` |
| `Managers.cpp:2619` | index bound object (emit) |
| `Managers.cpp:2659` | `GetAllAttributes()` (client arrays) |
| `Managers.cpp:2758` | `SyncFloat64AttributeAsFloat32(..., const VertexAttribute&, ...)` |
| `Managers.cpp:2877-2878` | `TwinRegistry<VertexArrayObject, BackendVertexArrayObject, MGPipeKind::VertexElementsCso> g_backendVertexArrayObjects` |
| `Managers.h:826-955` | class + registry decl |
| `BackendObject_DirectGLES.cpp:1391` | `VertexArrayObject::MAX_VERTEX_ATTRIBS` clamp for the advertised limit |
| `MultiDraw.cpp:98` | `GetIndexBufferBindingSlot().GetBoundObject()` |

**Finding.** Neither Espryt nor Magma reads `VertexBufferBindingPoint`, `GetAttributeBindingIndex` or
`GetAttributeRelativeOffset` — grep over `MG_Backend/DirectGLES` and `MG_Backend/DirectVulkan` returns nothing.
The frontend resolves the binding model eagerly into the flat `VertexAttribute` view
(`VertexArrayObject.cpp:229-260` `ResolveAttributeFromBinding`, `262-269` `ResolveAttributesForBinding`,
`146-181` `MirrorPointerIntoBinding`). See §4.2 for why this matters to the "two views" requirement.

---

## 2. After P2's Espryt 0b slot tables

### 2.1 Which kind holds the VAO twin

`MG_Backend/DirectGLES/Managers.cpp:2877-2878`:
```
TwinRegistry<MG_State::GLState::VertexArrayObject, BackendVertexArrayObject,
             MG_Pipe::MGPipeKind::VertexElementsCso> g_backendVertexArrayObjects;
```
decl `Managers.h:954-955`. So **`MGPipeKind::VertexElementsCso` (`MG_Pipe/MGPipeHandles.h:31`) is today the slot
space of the per-VAO twin**, one slot per live frontend `VertexArrayObject`, minted off `GetLifetimeId()`.

`TwinRegistry` is an alias that swallows the kind in a pull build: `Managers.h:543-549`; the class carries
`BackendSlotTable<StateObject, BackendObject, kKind> m_slotTable` only under `MOBILEGL_PIPE_PUSH`
(`Managers.h:299-305`, macro `MGB_TWIN_KIND_PARAM` `Managers.h:293-297`).

`BackendSlotTable` — `MG_Backend/DirectGLES/SlotTables.h:139-...`:
`GetOrCreate` `SlotTables.h:207-243` (`MGPipeSlots().Acquire(kKind, stateObj->GetLifetimeId())` at `225-226`, gen
mismatch resets the twin at `229-233`); `Find` `246-253`; `FindByHandle` `255-262`; `HandleOf` `272-281`;
`OnFrontendObjectDestroyed` (static, per-kind, walks every holder) `295-310`; `ForEachLive` `326-345`;
`ReleaseTwinAt` `352-374`; one-entry lifetimeId→handle memo `m_memoLifetimeId/m_memoHandle` `429-431`
(the comment at `407-428` names `ResolveVaoTwin` as one of the two callers the single-entry memo *does* help).

Arm selection: `ClassifyEsprytSlotArm` (`SlotTables.h:86-98`, impl `Managers.cpp:224-...`),
`EsprytSlotTablesEnabled()` inline latch `SlotTables.h:131-134`,
subsystem bit `kMGPipeSubsystemEsprytSlots = 1ull << 5` (`MG_Pipe/MGPipe.h:77`).

### 2.2 Death announcement

`VertexArrayObject::~VertexArrayObject()` — `MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:42-54`,
raising `NotifyStateObjectDestroyed(MG_Pipe::MGPipeKind::VertexElementsCso, m_lifetimeId)` (`53`).
Declared only under push (`VertexArrayObject.h:26-31`, so the pull build keeps its implicit destructor for G1).
Consumer: `OnFrontendStateObjectDestroyed` — `Managers.cpp:191-217`, VAO case at `209-211`
(`g_backendVertexArrayObjects.DestroyByLifetimeId(lifetimeId)`); ops table `Managers.cpp:219-221`.

### 2.3 Recorded debt P3a inherits

`SlotTables.h:68-78` — the header is under `MG_Backend/` yet it **mints** handles off a frontend
`SharedPtr`'s `GetLifetimeId()`, which `MGPipeHandles.h:13-16` forbids ("minted by the CLIENT"). Its own comment says
the minting and the lifetimeId→handle resolution belong on the client and the backend should receive the handle in
the verb payload — i.e. exactly what P3a is supposed to do for the VAO kind. `check_include_closure.py` does not
probe `MG_Backend` headers, so nothing catches this automatically.

---

## 3. Index-buffer handling (and the P8 host-mirror hooks)

### 3.1 The element-array binding

Owned by the VAO twin (§1.5, `Managers.cpp:2618-2638`). Its freshness key is
`{ BindingSlot::GetVersion() (Uint16, wrapping), bound-object raw pointer identity }` —
`Managers.h:922-931` explains why identity joins the version (`BindingSlot` lives at `MG_Util/Types.h:181-204`,
`m_version` is a `Uint16` bumped only on a real change, `Bind()` at `189-195`, `GetVersion()` at `198`).
`SetIndexBuffer` being independent of the VAO config version (D5, `docs/Disaggregated/ARCHITECTURE.md:99`) matches
what the code already does — the index slot is explicitly *not* covered by `m_configVersion`.

**Trap that any P3a re-emit must preserve:** two paths temporarily swap the element-array binding and must restore
the *exact* previous GL name, because the twin memoised that it already synced it:
* `ScopedRestartIndexSubstitution::~ScopedRestartIndexSubstitution` — `DirectGLES.cpp:4788-4791`
  (previous name captured at `4776` via `BoundElementArrayBufferId()`, `DirectGLES.cpp:4629-4634`)
* `MultiDrawImpl` — `MultiDraw.cpp:101-109` (`BoundIndexBufferId`, comment states the memo hazard verbatim)
  and `MultiDraw.cpp:929-937` (flattened-stream draw restores `previousIndexBinding`).

### 3.2 Primitive-restart rewrite (server-owned at P8, client today)

`docs/Disaggregated/ARCHITECTURE.md:222` assigns restart rewrite to the **server**, reading through the index host
mirror (`ARCHITECTURE.md:383-391`), with `kCapNeedsHostIndexBytes` as the switch (`ARCHITECTURE.md:106`) and
`kMGHostSpanSegFromServerIndexMirror = 0xFFFFFFFF` (`MG_Pipe/MGPipeHostSpan.h:26`) as the "bytes are already yours"
sentinel.

Today, in Espryt:
* `RestartSubstitutionKind ResolveRestartSubstitution(GLenum)` — `DirectGLES.cpp:4636-4653`
  (`None` / `SuppressRestart` when `restartIndex > fixedMax` / `RewriteIndices`)
* `ScopedSuppressedPrimitiveRestart` — `Managers.h:~130-145`, impl `DirectGLES.cpp:4657-4673`
  (toggles `GL_PRIMITIVE_RESTART_FIXED_INDEX` *around* the render-state shadow)
* `ScopedRestartIndexSubstitution` — decl `Managers.h:157-194`, ctor `DirectGLES.cpp:4675-4786`, dtor `4788-4791`
  * `kMaxRestartRewriteBytes = 1<<26` (64 MiB) — `DirectGLES.cpp:4510`
  * EBO source: **whole buffer** is rewritten, never just the draw range (`4691-4721`), so a GPU-resident
    `firstIndex` still addresses the right element
  * **the two reconcile calls P8 must reproduce**: `indexBuffer->SyncPersistentMappedRange();`
    `indexBuffer->SyncGpuWrites();` then `MappedData()` — `DirectGLES.cpp:4709-4713`
  * client-pointer source: only the draw's own range (`4722-4741`)
  * widening `UBYTE→USHORT→UINT` when the source already contains the all-ones index
    (`ContainsFixedRestartIndex` `4553-4560`, `WiderIndexType` `4515-4521`, decision `4746-4764`)
  * `RewriteRestartIndices` `4562-4578`; `UploadRestartScratch` `4580-4617` (whole-buffer `glBufferData` orphan into
    `g_restartIndices`, staged through `BufferImpl::TempBufferTarget`), byte counter
    `PipeStats::ByteClass::StageIndexClient` at `4610-4614`
  * `OnRestartSubstitutionContextDestroyed` `4655-4659`
* Call sites: `DirectGLES.cpp:4174` (indirect, `count=0` → whole-EBO form), `4799`, `4830`, `5099`, `5112`, `5137`,
  `5159`, `5174`, `5191`.

### 3.3 Multi-draw flattening (server-owned at P8, client today)

`MG_Backend/DirectGLES/MultiDraw.{h,cpp}`. Tier ladder documented `MultiDraw.h:15-31`:
`Ext / MultiIndirect / Indirect / BaseVertex / DrawElements / Compute`.

* `DrawElementsBatch` — `MultiDraw.cpp:897-992` (the whole entry point, owns `PrepareForDraw` at `927`)
* `ResolveTierForBatch(programReadsDrawID, perSubDrawBaseVertex, hasIndexBuffer, arbitraryRestart)` —
  `MultiDraw.cpp:294-347`; arbitrary restart forces the rewriting tier (`303-307`); batched tiers demote when the
  program reads `gl_DrawID` / per-sub-draw base vertex (`316-324`)
* `RunRebasedDrawElements` — `MultiDraw.cpp:492-...`; **the CPU index read**:
  `indexBuffer->SyncPersistentMappedRange(); indexBuffer->SyncGpuWrites(); MappedData(); GetSize();`
  — `MultiDraw.cpp:508-514`. `RebaseIndices` `MultiDraw.cpp:353-380` (always widens to `GL_UNSIGNED_INT`).
* `FlattenWithCompute` — `MultiDraw.cpp:690-...`; declines on non-list modes (`701-704`), on active restart
  (`706-717`), and with no real index buffer (`719-722`)
* Ceilings: `kMaxFlattenedIndices = 1<<24` (`MultiDraw.cpp:74`), `kComputeWorkGroupSize = 64` /
  `kMaxComputeWorkGroups = 65535` (`MultiDraw.cpp:81-84`)
* `BoundIndexBuffer` / `BoundIndexBufferId` `MultiDraw.cpp:94-109`, `BoundDrawIndirectBufferId` `86-92`

### 3.4 baseInstance emulation (the thing `set_vertex_buffers`' explicit field retires)

* `g_pendingFetchBaseInstance` — `Managers.cpp:2358`; `SetPendingFetchBaseInstance` / `GetPendingFetchBaseInstance`
  `Managers.cpp:2360-2366`; decl + doc `Managers.h:970-983` (`ScopedFetchBaseInstance` at `976-982`)
* `BaseInstanceByteShift` — `Managers.cpp:2377-2383`: `baseInstance * attrib.Stride`, only for `Divisor != 0`,
  zero for a stride-0 array
* applied at `Managers.cpp:2586` (`fetchOffset`) and inside the fp64 path at `Managers.cpp:2800-2806`
* `EmulatedFetchBaseInstance` — `DirectGLES.cpp:5128-5130` (returns 0 when `GL_EXT_base_instance` exists,
  `UseNativeBaseInstance` `5122-5124`); scopes at `DirectGLES.cpp:5135`, `5172`, `5224`
* **The shift is baked into the driver VAO's attribute offsets**, is draw state, and is deliberately *not* covered by
  any frontend version (`Managers.h:941-946`). Un-shifting on the next non-baseInstance draw is why
  `baseInstanceDirty` exists (`Managers.cpp:2462-2463`).

---

## 4. The MGPipe side as it exists

### 4.1 Catalogue entries (all present, none implemented)

`MG_Pipe/PipeCalls.def`:
```
95:  X(CreateVertexElements,     MGPVertexElements,       kCtxCso,   kHasBlob)
96:  X(BindVertexElements,       MGPHandleOnly,           kCtxCso,   kNone)
97:  X(DeleteVertexElements,     MGPHandleOnly,           kCtxCso,   kNone)
108: X(SetVertexBuffers,         MGPVertexBuffers,        kCtxState, kVarTail)
109: X(SetIndexBuffer,           MGPIndexBuffer,          kCtxState, kNone)
117: X(SetVertexAttribDefaults,  MGPVertexAttribDefaults, kCtxState, kVarTail)   // landed at P2
```
Generated: dispatch-table members `generated/PipeTables.inc:43-45, 55-56`; thunks
`generated/PipeThunks.inc:96-105, 144-149`; wire opcodes `generated/PipeWire.inc:66-68` (20/21/22) and `78-79` (32/33),
wire record `PipeWire.inc:273-279`; field verifier `generated/PipeVerify.inc:62, 148, 376-378`.

`gMGPipeContext` (`MG_Pipe/MGPipe.h:97`) is **never populated** anywhere outside
`MG_Test/Pipe/PipeCatalogueTest.cpp:102`. P2 does not emit through the table — it calls the applier directly
(`MG_Pipe/PipeApply.h:100-121`: `MGPipeApplyCreateRenderState`, `…BindRenderState`, `…DeleteRenderState`,
`…SetDynamicState`, `…SetPixelPackState`, `…SetPatchState`, `…SetVertexAttribDefaults`, `…SetResidualValueState`).
**P3a's applier entry points follow that same shape**, with `MGPipeApplierState` at `PipeApply.h:47-86`.

### 4.2 Payloads

`MG_Pipe/MGPipeTypes.h`:
```
253-259  MGPVertexElements { MGPipeHandle Cso; Uint32 AttributeCount; Uint32 BindingPointCount; MGPBlobRef Blob; }  // 40 B
356-364  MGPVertexBuffer   { MGPipeHandle Res; Uint64 Offset; Uint32 Stride, Divisor, BindingIndex, Pad0; }         // 32 B
367-371  MGPVertexBuffers  { Uint32 Start, Count; Uint64 ContentHash; }                                            // 16 B
374-380  MGPIndexBuffer    { MGPipeHandle Res; Uint64 Offset; Uint32 IndexSize, Pad0; }                             // 24 B
484-488  MGPVertexAttribDefaults { Uint32 Mask, Count; }                                                           //  8 B
714-728  MGPDrawInfo (56 B), 731-735 MGPDrawRange (12 B)
```
Draw flags `MGPipeTypes.h:701-707`: `kDrawHasUserIndices / kDrawPrimitiveRestart / kDrawIndicesAreClient /
kDrawHasIndexRange / kDrawHasXfbCount`.

**The two views** the roadmap means (`docs/Disaggregated/ROADMAP.md:19` "vertex elements 三件（两个视图都带）",
`ARCHITECTURE.md:130`) are the two arrays concatenated in `MGPVertexElements::Blob`:
1. the resolved flat `MG_State::GLState::VertexAttribute[]` — `MG_Pipe/MGPipeValueTypes.h:491-527`
   (`Enabled, Size, Type, Normalized, Stride, Offset, IsInteger, IsLong, IsBgra, Divisor, Buffer`,
   plus query-only `LegacyStride`/`LegacyPointer` at `524-526` which the doc says stay client-side)
2. `VertexBufferBindingPoint[]` — `MGPipeValueTypes.h:530-538` (`Buffer, Offset, Stride = 16 initial, Divisor`)

`VertexAttributeVersion` (3× `Uint16`, size-asserted 6) — `MGPipeValueTypes.h:540-544`, assert `561-562`.

> **Flag for the implementer.** The stated reason both views must travel (pointer stride 0 = element size vs binding
> stride 0 = same element) is *already resolved in the frontend*: `VertexAttribute::Stride` is documented as the
> RESOLVED distance and a surviving 0 can only have come from the binding model (`MGPipeValueTypes.h:496-507`), and
> Espryt consumes exactly that (`Managers.cpp:2545-2560`). Today **no backend reads the binding-point array at all**
> (§1.9). So the second view is either (a) for the `glGetVertexArrayIndexed*` query path, which
> `VertexArrayObject.h:88-104` keeps client-side anyway, or (b) forward-looking. Decide and record it before shipping
> a 2× blob for every VAO in a chunked-renderer workload.

> **Flag: `baseInstance` explicit field.** `ROADMAP.md:19` says "`set_vertex_buffers`（`baseInstance` 显式字段）",
> but `MGPVertexBuffer` (`MGPipeTypes.h:356-364`) has **no** baseInstance member. The only explicit field in the
> current catalogue is `MGPDrawInfo::StartInstance` (`MGPipeTypes.h:719`, `PipeFields.def:189`), which is on the draw
> verb, not on `set_vertex_buffers`. Either the roadmap line means "the fetch shift stops being folded into the
> vertex-buffer offsets and travels as `MGPDrawInfo::StartInstance`" — which is what the payloads support — or
> `MGPVertexBuffer` needs a field, which changes an already-size-asserted POD. Resolve explicitly; `MGPVertexBuffer`
> at 32 B has a `Pad0` at `MGPipeTypes.h:362` that could take it without moving the assert.

### 4.3 CSO caching

`ARCHITECTURE.md:57` sets the client-side content-addressed CSO capacities (render-state 64 / **vertex-elements 1024** /
sampler 256 / sampler-view 4096). `MG_Impl/Pipe/CsoCache.h` implements **only** the render-state cache
(`kMGPipeCsoCacheCapacity = 64`, `CsoCache.h:53`; eviction at `140`). There is no vertex-elements cache yet, and the
negative control bit is `kMGPipeBehaviourNoCsoContentAddressing` (`CsoCache.h:30-37`).

Slot minting: `MG_Impl/Pipe/SlotAllocator.h:39-99` — `FindByLifetimeId` `53`, `Acquire` `55`, `Free` `60`,
`FreeCount` `71`, singleton `MGPipeSlots()` `99`.

---

## 5. What the tracker already pushes for VAOs (P2)

`MG_Impl/Pipe/Tracker.h`:
* dirty enum — `Tracker.h:56-84`; the vertex rows: `NewVertexAttribDefaults` `63`, `NewVertexElements` `65`,
  `NewVertexBuffers` `70`, `NewIndexBuffer` `71`
* names — `Tracker.h:96-114` (`"NEW_VERTEX_ATTRIB_DEFAULTS"` `100`, `"NEW_VERTEX_ELEMENTS"` `101`,
  `"NEW_VERTEX_BUFFERS"` `105`, `"NEW_INDEX_BUFFER"` `106`)
* `kMGPipeDirtyEmittedAtP2` — `Tracker.h:91-97`: only the five value bits, **vertex elements / buffers / index are
  computed and counted but not emitted**; `MGPipeSubsystemForDirty` returns 0 for them (`Tracker.h:122-136`,
  default arm `132-135`)
* the shutters, `MGPipeTracker::Update` — `Tracker.h:178-...`:
```
Tracker.h:210-213  vaoIdentity = vao ? MGPipeMixShutter(vao->GetLifetimeId(), vao->GetConfigVersion()) : 0;
                   now[NewVertexElements] = vaoIdentity;
Tracker.h:243-244  now[NewVertexBuffers]  = MGPipeMixShutter(ctx.GetAnyVaoAttributeGeneration(), vaoIdentity);
Tracker.h:245-248  now[NewIndexBuffer]    = MGPipeMixShutter(buffers, vaoIdentity);   // over-fires on ANY buffer write
Tracker.h:208      now[NewVertexAttribDefaults] = ctx.GetAnyVertexAttribDefaultGeneration();
```
  `MGPipeMixShutter` `Tracker.h:143-146`; `MGPipeWidenedCounter` `Tracker.h:158-176`.
  The over-fire policy is stated at `Tracker.h:27-33` — a rarely-firing bit renders stale and P1's verify
  comparator cannot see it for object-class state.

`FillPoints.def` kDraw rows relevant to vertex state — `MG_Pipe/FillPoints.def:152-199`:
`GetBoundVertexArray` `154`, `GetProgramForDraw` `155`, `GetBufferBindingSlot` `156`,
`GetCurrentVertexAttribute` `165`, `GetPrimitiveRestartIndex` `193`.
(`kDispatch, GetBufferBindingSlot` `204`; `kReadback, GetBufferBindingSlot` `289`.)

`Coverage.def` — accessor→call map: `GetBoundVertexArray → BindVertexElements` (`Coverage.def:36`);
`GetBufferBindingSlot → SetIndirectBuffers` with the note at `37-41` that its rows **split across
set_vertex_buffers / set_index_buffer / set_indirect_buffers / set_shader_buffers** once the inventory is
re-vendored with the target argument (deferred out of P1 — P3a is where that split lands);
`GetCurrentVertexAttribute → SetVertexAttribDefaults` `52`; `GetPrimitiveRestartIndex → DrawVbo` `70`.
Emitted list `Coverage.def:158-192` contains **no** vertex-elements/buffers/index row yet
(`GetCurrentVertexAttribute → SetVertexAttribDefaults` `164` and `GetPrimitiveRestartIndex → SetDynamicState` `178`
are the only vertex-adjacent ones).

`PipeInputs` side — `MG_Backend/MGPipe/PipeInputs.h`: field list rows `79` (`GetBoundVertexArray → m_boundVertexArray`),
`80` (`GetBufferBindingSlot`), `89` (`GetCurrentVertexAttribute`), `107` (`GetPrimitiveRestartIndex`);
accessors `477-481`, `487-493`, `274-283`, `369-372`; storage `641`, `658`, `678-679`;
`kMaxVertexAttribs` `175`. Fill: `MG_Impl/Pipe/PipeFill.cpp:80-82` (bound VAO), `83-91` (buffer binding slots),
`126-129` (current attribs); aggregate note dispatch `PipeFill.cpp:537-538`.

---

## 6. The P2 aggregate generations that cover vertex state

| generation | owner | declaration | bumped from |
|---|---|---|---|
| `VertexArrayObject::m_configVersion` (`Uint32`, per object) | frontend | `VertexArrayObject.h:107` (getter), `:214` (member) | the three `Bump*Version` — `VertexArrayObject.cpp:313-332` |
| per-attribute `VertexAttributeVersion{Format,Buffer,Switch}` (`Uint16`×3) | frontend | `MGPipeValueTypes.h:540-544`; array member `VertexArrayObject.h:207` | same three bumpers |
| `VertexArrayState::m_anyVaoAttributeGeneration` (`Uint64`) | frontend | `VertexArrayState.h:36-42` (`NoteAttributeChanged` / `GetAnyAttributeGeneration`), member `:44` | `MGP_NOTE_AGGREGATE(VaoAttribute)` inside all three bumpers (`VertexArrayObject.cpp:317, 324, 331`) |
| `BufferState::m_anyBufferChangeGeneration` (`Uint64`) | frontend | `BufferState.h:72-78` | every buffer mutator |
| index slot `BindingSlot::m_version` (`Uint16`, **wraps**) | frontend | `MG_Util/Types.h:198, 202` | `BindingSlot::Bind` `Types.h:189-195` |
| `BufferImpl::g_bufferBackendIdGeneration` (`Uint64`) | backend | `Managers.h:701-707` | driver-id re-mint (persistent-map adoption, immutable-store retire) |
| buffer-mutation epoch (`CurrentBufferMutationEpoch()`) | backend | `Managers.h:548-588` | any path that can dirty any buffer |
| `VertexArrayObject::m_lifetimeId` (`Uint64`, never reused) | frontend | `VertexArrayObject.h:70` (`GetLifetimeId`), member `:205`; allocator `:203` | ctor |

### How a server-side VAO object table would be keyed

* Handle: `MGPipeHandle{Slot, Gen}` of kind `VertexElementsCso`, minted client-side off `GetLifetimeId()`
  (`SlotTables.h:225-226`), freed on the destructor notice (`VertexArrayObject.cpp:53`), so the server table is a
  dense array indexed by `Slot` with a `Gen` compare — exactly `BackendSlotTable::Entry` (`SlotTables.h:145-158`)
  and `MGPipeApplierState::RenderStateCsos` (`PipeApply.h:47-51`) already do this shape for render state.
* Freshness inside a slot: today the twin's own `{m_syncedConfigVersion, m_syncedAttributeVersions[32],
  m_syncedIndexBufferVersion + m_syncedIndexBufferObject, m_syncedFetchBaseInstance, m_syncedBufferIdGeneration}`
  (`Managers.h:906-952`). Under P3a the elements half becomes a **content hash** on the client (`CsoCache`-shaped)
  and the buffers/index halves become `MGPVertexBuffers::ContentHash` (`MGPipeTypes.h:370`) + a plain
  `set_index_buffer`, so `m_syncedConfigVersion` / `m_syncedAttributeVersions` / `m_syncedIndexBufferObject` all
  disappear — but `g_bufferBackendIdGeneration` **does not**: it answers "did *I* re-mint the driver id", which is a
  server-side `MGGen` and must never cross the wire (`ARCHITECTURE.md:73-79`).
* `ARCHITECTURE.md:363` records that `SetBackendStateMemo` (the frontend VAO holding a backend heap pointer) is
  deleted outright; the three legacy memos on the frontend object are `VertexArrayObject.h:110-175`
  (`GetBackendHashMemo`/`SetBackendHashMemo` `121-129`, `GetBackendStateMemo`/`Set…` `137-149`,
  `GetBackendAuxMemo`/`Set…` `160-172`, storage `216-225`), all under `MOBILEGL_PIPE_LEGACY_MEMOS`.

---

## 7. Tests

### 7.1 Integration scenarios (`MG_IntegrationTest/Scenarios/`) — gtest `TEST_F`

`VertexAttribBindingScenario.cpp` (758 lines; every case captures the vertex shader's inputs with XFB under
`GL_RASTERIZER_DISCARD`, poison-prefilled buffer — header note `:8-17`):
`FormatAndBindingFeedTheDraw` `241`, `FormatBeforeBufferStillResolves` `269`,
`BindingStrideZeroRepeatsOneElement` `297`, `PointerStrideZeroStaysTightlyPacked` `324`,
`BindingDivisorAppliesToEveryAttributeOnThePoint` `349`,
**`BaseInstanceMovesTheInstancedArraysStartElement` `406`**,
**`BaseInstanceLeavesPerVertexArraysWhereTheyWere` `451`**,
`RelativeOffsetComposesWithBindingOffset` `496`, `InputArrayCaptureProgramFeedsTheDraw` `532`,
`EveryInputArrayElementGetsItsOwnCurrentValue` `617`, `DoubleArrayIsFetchedAtFloat32Precision` `681`,
`NormalizedIsIgnoredForDoubleArrays` `708`, `LongDoubleArrayIsFetchedAtFloat32Precision` `734`.

`VertexArrayEnableDisableScenario.cpp`: `EitherHalfOfTheAttributesInTurn` `231`, `TheEvenHalfAlone` `245`,
`TheOddHalfAlone` `255`.

`MultiDrawScenario.cpp`: `BaseVertexBatchMatchesUnrolledDraws` `225`, `PlainBatchMatchesUnrolledDraws` `253`,
`UnsignedShortBatchMatchesUnrolledDraws` `288`, `UnsignedByteBatchMatchesUnrolledDraws` `317`,
`BaseVertexBeyondIndexTypeRangeMatchesUnrolledDraws` `357`, `ClientSideIndicesBatchMatchesUnrolledDraws` `391`,
`PrimitiveRestartStripBatchMatchesUnrolledDraws` `423`, `PrimitiveRestartUnsignedShortBatchMatchesUnrolledDraws` `456`,
`ZeroCountSubDrawsMatchUnrolledDraws` `493`, `BaseVertexDrawsRejectMalformedArguments` `526`.

`PrimitiveRestartScenario.cpp`: `AnArbitraryRestartIndexDrawsInsteadOfKillingTheProcess` `215`,
`TheFixedIndexValueTakesTheForwardingPath` `236`, `WithoutTheCapTheStripWeldsAcrossTheGap` `263`,
`ChangingTheRestartIndexBetweenDrawsIsHonoured` `281`, `ANonIndexedListTopologyDrawIsUnaffectedByTheCap` `306`,
`ARestartIndexTooLargeForTheIndexTypeRestartsNowhere` `332`, `AnAllOnesVertexIndexSurvivesTheSubstitution` `368`.

`DrawParametersScenario.cpp` (the `gl_BaseInstance`/`gl_DrawID`/`gl_BaseVertex` side of the same emulation):
`DrawArraysReportsAZeroBaseVertexDespiteItsFirst` `213`,
`DrawArraysInstancedBaseInstanceReportsItsBaseInstance` `221`, `APlainDrawAfterABaseInstancedOneSeesZeroAgain` `233`,
`DrawElementsBaseVertexReportsItsBaseVertex` `245`, `DrawElementsAfterABaseVertexDrawReportsZeroAgain` `266`,
`MultiDrawArraysNumbersItsSubDraws` `286`, `MultiDrawElementsIndirectCarriesEveryCommandsParameters` `301`,
`MultiDrawArraysIndirectCountObeysItsParameterBuffer` `327`.

`ResidentIndexScenario.cpp` (element-array + persistent map — the closest thing to a P8 index-mirror gate):
`PersistentCoherentEboWrittenEveryFrame` `186`, `EboAlsoBoundAsVertexBufferLater` `225`,
`EboDeletedAndRecreatedAtTheSameName` `271`, `OneEboTwoVaosMutatedAcrossFrames` `311`, `PromotedDynamicEbo` `351`.

`HandleRecycleScenario.cpp`: `TheReproducerRecyclesEveryName` `440`,
**`AVertexArrayAtARecycledAddressDoesNotInheritItsPredecessorsVertexInput` `483`** (the P2 re-key gate for this kind),
`ATextureAtARecycledAddress…` `573`, `AFramebufferAtARecycledAddress…` `624`.

`DoublePrecisionScenario.cpp` vertex-array cases: `A64BitVertexFormatIsRecordedAndItsArrayIsDroppedAtDraw` `863`,
`AnEnabledLongArrayDoesNotBreakADrawThatIgnoresIt` `916`.

Buffer-family scenarios the roadmap names for P3a: `LargeArenaAdoptionScenario.cpp`
(`SubDataAfterAnInFlightDrawReachesTheNextDraw` `204`, `ReadbackSeesTheLatestCpuWrite` `227`,
`GpuWriteIntoTheArenaIsReadBack` `245`), `StorageBufferRegrowScenario.cpp`
(`AGrownStoreIsVisibleThroughItsExistingIndexedBinding` `124`).

### 7.2 Unit tests

* `MG_Test/VertexArray/VertexArrayTest.cpp` (1366 lines, 46 cases) — frontend state. Cases most load-bearing for
  P3a: `VertexAttributeSetup` `124`, `IndexBufferBinding` `156`, `MultipleAttributes` `221`,
  `BoundVAOPreservesState` `266`, `VertexBindingIndexIsBoundedByTheAdvertisedAttribLimit` `322`,
  `DefaultAttributeBindingIsIdentityAcrossFullCapacity` `338`,
  `General_ElementArrayBufferBindingIsVaoLocalAndZeroUnbinds` `557`,
  `General_ClientSideVertexAttribPointerIsAccepted` `602`, `General_ElementBufferBindingPoint` `750`,
  `ArrayFormat_PackedStored` `1207`, `ArrayFormat_BgraStored` `1228`,
  `ArrayFormat_BindingAndRelativeOffsetAreQueryable` `1324`.
* `MG_Test/VertexArray/VertexAttribBindingStateTest.cpp` (430 lines) — the two-view invariant:
  `DefaultsMatchTheSpecInitialState` `198`,
  **`SeparateFormatSequenceKeepsLegacyStrideAndPointerAtZero` `209`**, `DivisorGoesThroughTheBindingPoint` `318`,
  `BindingApiRejectsTheDefaultVertexArrayInCoreProfile` `370`, `…StillAcceptsTheDefaultVertexArrayWhenRelaxed` `395`,
  `RelaxedSemanticsOverrideReopensTheDefaultVertexArray` `415`.
* `MG_Test/SanityTest.cpp` `DirectGLESSlotTable` suite (20 cases, `3311`–`4044`). VAO-specific:
  `EveryReKeyedObjectClassAnnouncesItsOwnDeath` `3706` (VAO at `3727-3734`),
  `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` `3857` (VAO at `3878-3879`),
  plus `ARecycledSlotIsANewHandleAndTheStaleOneResolvesToNothing` `3311`,
  `OneDeathNoticeDropsTheTwinInEveryHolderOfTheKind` `3532`, `AnAnnouncedDeathReturnsTheSlotWithoutASweep` `3657`,
  `EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping` `4044`.
* `MG_Test/Pipe/TrackerTest.cpp`: `TrackerAggregates.AVertexArrayAttributeMovesOnlyTheVaoAggregate` `153`
  (skip-list row `55`), `AVertexAttribDefaultMovesOnlyItsOwnAggregate` `213`,
  `TrackerWalk.WrapAroundRePushesButNeverMisses` `384`, `TrackerAttribPayload.*` `490-524`.
* `MG_Test/Backend/DirectGLES/BaseInstanceInjectionTest.cpp` — the *shader-side* baseInstance gate (`77-155`),
  not the fetch shift; `MG_Test/SanityTest.cpp:469-609` covers the SPIR-V rewrites.
* `MG_Test/Pipe/SlotAllocatorTest.cpp`, `CsoCacheTest.cpp`, `PipeCatalogueTest.cpp` (pins
  `MGP_CALL_LIST_DOCUMENTED_COUNT = 71`, `PipeCalls.def:69`) — the catalogue/allocator gates P3a must keep green.

---

## 8. Traps an implementer will hit

1. **The twin bakes driver ids into the driver VAO, keyed on FRONTEND versions.** `g_bufferBackendIdGeneration`
   (`Managers.h:701-707`, read `Managers.cpp:2446`) exists solely because a backend-side re-mint moves no frontend
   version. A `set_vertex_buffers` keyed on client-side content hash alone reintroduces the bug commit `d7655247`
   fixed. The generation must stay server-local.
2. **Element-array binding restore.** `ScopedRestartIndexSubstitution` (`DirectGLES.cpp:4788-4791`) and
   `MultiDrawImpl` (`MultiDraw.cpp:101-109`, `929-937`) restore the exact previous GL name *because the twin
   memoised it*. Whatever replaces the memo must keep an equivalent invariant, or the next draw silently draws
   through the scratch buffer.
3. **fp64 streams are content-derived and covered by no version.** `m_hasConvertedFloat64Attribute`
   (`Managers.h:919-924`, set `Managers.cpp:2530`, cleared `2471`) forces a full re-walk each draw while any such
   array is live. A pure content-hash CSO would go stale here.
4. **The zero-stride and BGRA paths are the two Adreno SIGSEGV corners** (`Managers.cpp:2495-2520`, `2568-2611`);
   both must survive the split, and the BGRA probe is the one `glGetError` on the draw path.
5. **Client arrays are resolved after `PrepareForDraw`**, per draw range, and only for `DrawArrays` /
   `MultiDrawArrays` (`DirectGLES.cpp:4805-4821`, `4837-4866`). The push ordering contract (`ARCHITECTURE.md:194`)
   allows this, but the emitted `set_vertex_buffers` for a client-array attribute names a store that does not exist
   until the draw entry point runs.
6. **`SyncNeccessaryBuffers` also does SSBO binding points and `MarkShaderStorageBuffersGpuWritten()`**
   (`DirectGLES.cpp:705-706`) — unrelated to vertex input, and it must not be dragged into `set_vertex_buffers`.
7. **`ResolveVaoTwin`'s single-entry handle memo.** `SlotTables.h:407-428` names `ResolveVaoTwin` as one of the two
   callers a one-entry memo helps, *because* `PrepareForDraw` resolves the same VAO once per draw. Two resolves per
   draw (e.g. adding a second lookup for the elements CSO) would thrash it.
8. **`CollectGarbageIfNeeded` at `DirectGLES.cpp:1275`** is legacy-arm-only; do not add a matching tick on the slot
   arm (`SlotTables.h:37-48`).
