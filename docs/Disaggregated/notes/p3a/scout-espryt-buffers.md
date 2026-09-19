# P3a scout — Espryt (MobileGL/MG_Backend/DirectGLES) buffer surface

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated`.
All paths below are repo-relative. Every line number was opened.

---

## 0. Map at a glance

| Thing | Where |
|---|---|
| Ops table type | `MobileGL/MG_State/GLState/BufferState/BufferObject.h:76-124` |
| Ops table registry (global) | `MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:18,27,31` |
| Espryt implementations | `MobileGL/MG_Backend/DirectGLES/Managers.cpp:624-1490` (whole `BufferImpl` anon namespace) |
| Espryt table instance | `Managers.cpp:1482-1490` (`g_glesBufferBackendOps`) |
| Backend resource class | `MobileGL/MG_Backend/DirectGLES/Managers.h:592-644` (`GLESBufferResource`) |
| Storage abstraction | `MobileGL/MG_State/GLState/BufferState/PipeResource.h:62-140` |
| Draw-path entry | `Managers.cpp:1602-1706` (`EnsureBufferResource`), `1579-1601` (`IsBufferDrawClean`) |

Note on the ops-table shape: **every hook receives `BufferObject&` (a frontend heap
reference), not a handle.** That is the single biggest P3a conversion fact — see §2.

---

## 1. `BufferBackendOps` — definition and Espryt's seven hooks

### 1.1 Definition (MG_State side)

`MobileGL/MG_State/GLState/BufferState/BufferObject.h:76` declares `struct BufferBackendOps`
with exactly seven function pointers:

| # | Hook | Signature line |
|---|---|---|
| 1 | `Respecify(BufferObject&)` | `BufferObject.h:80` |
| 2 | `SubData(BufferObject&, SizeT offset, SizeT size)` | `BufferObject.h:82` |
| 3 | `ResidentSubData(BufferObject&, SizeT offset, DataPtr data)` | `BufferObject.h:95` |
| 4 | `FlushMappedRange(BufferObject&, Range1D, Flags<BufferMappingAccessBit> appAccess)` | `BufferObject.h:99-100` |
| 5 | `OnDestroy(SharedPtr<BackendBufferResource>&&)` | `BufferObject.h:103` |
| 6 | `void* AcquirePersistentMap(BufferObject&)` | `BufferObject.h:114` |
| 7 | `ReadbackFromGpu(BufferObject&)` | `BufferObject.h:121` |

The table is a single process-global pointer: `g_bufferBackendOps` at
`BufferObject.cpp:18`, set/read by `SetBufferBackendOps` (`BufferObject.cpp:27`) and
`GetBufferBackendOps` (`BufferObject.cpp:31`). A null table means shadow-only tracking
(unit tests, benchmarks) — see `BufferObject.h:125-127`.

Contract stated in the header and relied on by Espryt: **every op must tolerate
`GetBackendResource() == nullptr`** (`BufferObject.h:70-74`); the lazy draw-time
`EnsureBufferResource` full-uploads from the shadow and thereby covers everything that
happened before the resource existed. Contents are **always read from the shadow**, so
ops carry only ranges and flags — never bytes — with the single exception of
`ResidentSubData`, which carries `DataPtr data` valid **for the duration of the call
only** (`BufferObject.h:85-94`).

Frontend dispatchers that reach these hooks:
`NotifyRespecify` (`BufferObject.cpp:45`), `NotifySubData` (`:53`),
`NotifyFlushMappedRange` (`:63`), `NotifyContentWrite` (`:73`),
`LandBytesIntoResidentStore` (`:371`), `SyncGpuWrites` (`:319`),
`TryAdoptLargeStorage` (`:186`), `EnsureGpuResidentStorage` (`:493`).

### 1.2 Espryt's backend resource (`GLESBufferResource`)

`Managers.h:592-644`. Derives from `MG_State::GLState::BackendBufferResource`
(`PipeResource.h:66-70`). Fields, each of which P3a must decide client-vs-server for:

- `id` (GL buffer name), `storageSize`, `storageInitialized` — `Managers.h:598-600`
- `contextGeneration` — `Managers.h:603`; compared against `g_bufferContextGeneration`
  (`Managers.cpp:646`), bumped in `OnBackendContextDestroyed` (`Managers.cpp:1529`)
- `std::atomic<Uint64> syncedChangeSerial` — `Managers.h:608`; mirrors the FRONTEND's
  `GetChangeSerial()`
- `pendingRespecify` (`Managers.h:614`), `VecRange1D pendingRanges` (`Managers.h:615`),
  `Vector<PendingResidentWrite> pendingResidentWrites` (`Managers.h:624`), guarded by
  `std::mutex pendingMutex` (`Managers.h:625`)
- `drawCleanEpoch` — `Managers.h:631`
- `persistentMapped`, `persistentPtr` — `Managers.h:637-638`
- `immutableStorage` — `Managers.h:644`

### 1.3 Hook 1 — `Ops_Respecify` (`Managers.cpp:1213-1264`)

Wrapper `Ops_RespecifyTracked` at `Managers.cpp:1449-1452` (calls impl then
`BumpBufferMutationEpoch()`).

Reads from `BufferObject`: `GetSize()`, `GetUsage()`, `HasDefinedContent()`,
`MappedData()`, `GetChangeSerial()` — all via `RespecifyStorageNow`
(`Managers.cpp:895-936`).

Behaviour:
- `ResourceOf(bufferObject)` (`Managers.cpp:878-880`) → null ⇒ return (lazy create).
- If `resource->immutableStorage` (`Managers.cpp:1224`): the store came from
  `glBufferStorageEXT` and `glBufferData` cannot respecify it. Clears
  `persistentMapped/persistentPtr`, and if on the context thread deletes the id
  (`NoteBufferIdDeleted` + `glDeleteBuffers`, `Managers.cpp:1229-1234`); otherwise leaves
  it flagged for `EnsureBufferResource` to retire. Sets `pendingRespecify = true` and
  clears both pending queues.
- Off-thread / no id / stale context (`Managers.cpp:1244`): queue `pendingRespecify`.
- Size 0 (`Managers.cpp:1250`): drop storage state, no GL.
- Otherwise `RespecifyStorageNow` (`Managers.cpp:934`):
  - `BindBufferId(TempBufferTarget /* = GL_ARRAY_BUFFER, Managers.h:552 */, id)`
  - `glBufferData(target, size, initialData, usage)`, where `initialData` is
    `bufferObject.MappedData()` **only if `HasDefinedContent()`** — an orphaning
    respecify passes NULL so the driver renames without a stall
    (`Managers.cpp:906-912`).
  - PipeStats `StageBuffer` bytes accounted only when `initialData != nullptr`
    (`Managers.cpp:913-919`).
  - Clears `pendingRespecify`, `pendingRanges`, `pendingResidentWrites`; stamps
    `syncedChangeSerial` (`Managers.cpp:920-925`).
  - If the EXTENT moved, `InvalidateIndexedBufferBindingShadowsForId(resource.id)`
    (`Managers.cpp:933`) — Adreno resolves a whole-buffer indexed binding's extent at
    bind time, so a grown store would keep the old, smaller range.
- **TODO in tree**: orphan-on-respecify as an id-swap is NOT implemented
  (`Managers.cpp:888-894`).

### 1.4 Hook 2 — `Ops_SubData` (`Managers.cpp:1266-1297`)

Tracked wrapper `Managers.cpp:1453-1456`.

- `pendingRespecify` ⇒ return (full re-upload pending).
- No context / no id / stale generation / `!StorageMatches` (`Managers.cpp:939-942`) ⇒
  `pendingRanges.Add({offset, offset+size})` under `pendingMutex`.
- Adopted zero-copy store (`persistentMapped && persistentPtr`, `Managers.cpp:1278`) ⇒
  bytes already there; just stamp `syncedChangeSerial`. No GL.
- `MG_Config::Features.EsprytDisableUploadRing` (negative control) ⇒
  `UploadRangeNow` (immediate `glBufferSubData`, `Managers.cpp:946-955`).
- **Default path**: queue the range only (`Managers.cpp:1295-1296`). This is the Mali
  WAR-stall fix — an immediate `glBufferSubData` parks the calling thread in
  `osup_sync_object_wait` (`Managers.cpp:1284-1292`, and `Managers.h:806-826`).

### 1.5 Hook 3 — `Ops_ResidentSubData` (`Managers.cpp:1304-1312`)

Tracked wrapper `Managers.cpp:1457-1460`. **Issues no GL at all** — thread-agnostic.
Copies the caller's bytes into `resource->pendingResidentWrites` (a
`{offset, Vector<Uint8> bytes}` record, `Managers.h:620-624`) under `pendingMutex`.
Drained by `DrainResidentWritesNow` (`Managers.cpp:1084-1128`) at draw-time sync
(`Managers.cpp:1670`) or at readback (`Managers.cpp:1391`):
- stage into `g_uploadRing` + `glCopyBufferSubData(GL_COPY_READ_BUFFER →
  GL_COPY_WRITE_BUFFER)` (`Managers.cpp:1110-1117`);
- fallback direct `glBufferSubData` (`Managers.cpp:1119-1122`) — legal because the
  adopted store carries `kDynamicStorageBit`.

This hook exists because an in-place host write into the coherent mapping tears the
frames still reading the old bytes (`BufferObject.h:85-94`, `Managers.cpp:1074-1083`).

### 1.6 Hook 4 — `Ops_FlushMappedRange` (`Managers.cpp:1314-1379`)

Tracked wrapper `Managers.cpp:1461-1465`.

- `pendingRespecify` ⇒ return.
- Off-thread / mismatched storage ⇒ queue the range (`Managers.cpp:1321-1325`).
- Adopted store ⇒ stamp serial, return (`Managers.cpp:1334-1337`). The historical
  self-copy here mapped a buffer Espryt keeps persistently mapped — an
  `INVALID_OPERATION` whose fallback WAR-stalled.
- Default (ring enabled) ⇒ queue the range (`Managers.cpp:1344-1348`).
- Kill-switch path only: honours the app's real mapping flags
  (`InvalidateRange`/`InvalidateBuffer`/`Unsynchronized`, `Managers.cpp:1353-1356`),
  optionally `glMapBufferRange(WRITE | INVALIDATE_RANGE? | UNSYNCHRONIZED?)` + `Memcpy`
  + `glUnmapBuffer` (`Managers.cpp:1357-1375`), else `UploadRangeNow`.

### 1.7 Hook 5 — `Ops_OnDestroy` (`Managers.cpp:1421-1447`)

Tracked wrapper `Managers.cpp:1466-1469`. Takes ownership of the
`SharedPtr<BackendBufferResource>` moved out of the dying `BufferObject`.

Three outcomes:
1. Stale `contextGeneration` ⇒ zero `id`, drop (`Managers.cpp:1424-1427`).
2. Context current (`CanTouchGLNow()`, `Managers.cpp:882-884`):
   - `IsPoolable` (`Managers.cpp:700-708`: needs working fences, id != 0,
     `!persistentMapped`, `!immutableStorage`, live generation, initialized storage,
     `storageSize <= kMaxPoolableBufferBytes` = 8 MiB `Managers.cpp:694`) ⇒
     `EnrollIntoPool` (`Managers.cpp:712-745`);
   - else `InvalidateArrayBufferBindingCache` if bound, `ScrubBufferBindingShadowsForId`,
     `glDeleteBuffers` (`Managers.cpp:1434-1441`).
3. Not on the context thread ⇒ push onto `g_deferredBufferReleases`
   (`Managers.cpp:669-676`) under `g_deferredBufferReleasesMutex`, and set the lock-free
   emptiness flag `g_hasDeferredBufferReleases` (`Managers.cpp:673-676`).

**Deferred-release / delayed-free machinery** (all of this must survive P3a):
- Drain: `ProcessDeferredBufferReleases` (`Managers.cpp:1542-1572`), called per draw from
  `SyncNeccessaryBuffers`/`SyncComputeBuffers` (`DirectGLES.cpp:735` region and
  `DirectGLES.cpp:717`). Fast-outs on the atomic flag first.
- Buffer pool (Mesa-style BO recycle), `Managers.cpp:678-762`:
  `struct PooledBuffer {id, size, contextGeneration, retireSerial}` (`Managers.cpp:685-690`),
  `UnorderedMap<SizeT, Vector<PooledBuffer>> g_bufferPool` (`Managers.cpp:691`),
  budget constants `kMaxPoolableBufferBytes` 8 MiB / `kMaxPoolBytes` 64 MiB /
  `kMaxEntriesPerBucket` 32 (`Managers.cpp:694-696`).
  `EnrollIntoPool` stamps `retireSerial = DirectGLES::CurrentFrameSerial() + 1`
  (`Managers.cpp:737-739` — the `+1` is explained there).
  `AcquireFromPool(size)` (`Managers.cpp:748-773`) hands back only entries with
  `retireSerial <= DirectGLES::CompletedFrameSerial()` (`DirectGLES.cpp:10859-10860`).
  `TrimBufferPool` (`Managers.cpp:1882`) is called once per frame from Present
  (`DirectGLES.cpp:10928`); `ClearBufferPool` (`Managers.cpp:1913`) on context loss.
- The pool reseed path in `EnsureBufferResource` (`Managers.cpp:1657-1685`) does a
  whole-buffer `glBufferSubData` from `bufferObject->MappedData()`.

### 1.8 Hook 6 — `Ops_AcquirePersistentMap` (`Managers.cpp:1134-1210`)

Tracked wrapper `Managers.cpp:1470-1476` (bumps the epoch **even on decline**).

- Gate: `CanTouchGLNow()` + `glBufferStorageEXT` + `glMapBufferRange` + `glGenBuffers`
  (`Managers.cpp:1135-1138`); `size == 0` ⇒ null.
- Creates the `GLESBufferResource` if absent (`Managers.cpp:1142-1147`) — the one op that
  calls `bufferObject.SetBackendResource(...)` besides `EnsureBufferResource`.
- Stale generation ⇒ wipe the resource fields BEFORE stamping the new generation
  (`Managers.cpp:1151-1160`), otherwise idempotency would return a dead context's pointer.
- Idempotent hit: `persistentMapped && persistentPtr && storageSize == size`
  (`Managers.cpp:1163-1165`).
- Fresh id required: `NoteBufferIdDeleted`, `++g_bufferBackendIdGeneration`,
  `glDeleteBuffers`, `glGenBuffers` (`Managers.cpp:1169-1179`).
- `glBufferStorageEXT(GL_ARRAY_BUFFER, size, bufferObject.MappedData(),
  GL_MAP_WRITE_BIT | PERSISTENT | COHERENT | DYNAMIC_STORAGE)` — bit constants defined
  locally at `Managers.cpp:1130-1132` (`0x0040/0x0080/0x0100`). Seeds from the shadow,
  which is still live at this point (`Managers.cpp:1179-1185`).
- `immutableStorage = true` set as soon as the store exists, before the map result
  (`Managers.cpp:1188`).
- `glMapBufferRange(0, size, WRITE|PERSISTENT|COHERENT)`; on failure logs
  `MGLOG_E_ONCE` and returns null with `persistentMapped=false`
  (`Managers.cpp:1189-1197`).
- Success: stamps `persistentPtr/persistentMapped/storageSize/storageInitialized`,
  clears both pending queues, stamps `syncedChangeSerial` (`Managers.cpp:1198-1209`).

Frontend adoption then calls `PipeResource::AdoptPersistentMap` (`PipeResource.h:113-117`),
which **drops the CPU shadow**. Espryt itself never calls `EnsureGpuResidentStorage`
(only Magma does: `DirectVulkan/Renderer/UniformManager.cpp:1073,1229`,
`VulkanRenderer.cpp:11620`); Espryt reaches adoption purely through the frontend's
`TryAdoptLargeStorage` (`BufferObject.cpp:186-196`, threshold 16 MiB, kill switch
`MG_Config::Features.DisableLargeBufferAdoption`) and through coherent persistent maps in
`AcquireMemoryRange`.

### 1.9 Hook 7 — `Ops_ReadbackFromGpu` (`Managers.cpp:1382-1419`)

Tracked wrapper `Managers.cpp:1477-1480`.

- Bails on no resource / id 0 / uninitialized storage / off-thread / stale generation
  (`Managers.cpp:1384-1386`).
- **Adopted store branch** (`Managers.cpp:1387-1394`): `DrainResidentWritesNow` then
  `glFinish()`. There is no backend copy to read back — the coherent mapping IS the data.
- Otherwise: `FlushPendingRangesNow` first (`Managers.cpp:1404`) so queued app writes are
  not reverted, then `BindBufferId` + `glMapBufferRange(0, size, GL_MAP_READ_BIT)`,
  `bufferObject.WritebackFromBackend({mapped, size}, 0)` (`Managers.cpp:1410`),
  `glUnmapBuffer`, then stamp `syncedChangeSerial` so the next draw does not re-upload the
  readback over itself (`Managers.cpp:1413-1418`).

### 1.10 Registration lifecycle and the mutation epoch

- `RegisterBufferBackendOps` (`Managers.cpp:1505-1510`) — called from
  `BackendObject_DirectGLES::Initialize` (`BackendObject_DirectGLES.cpp:849`) and from
  every `MakeCurrent` (`DirectGLES.cpp:10402`).
- `UnregisterBufferBackendOps` (`Managers.cpp:1512-1525`) — also clears the pool and the
  deferred-release list.
- `OnBackendContextDestroyed` (`Managers.cpp:1527-1540`) — bumps
  `g_bufferContextGeneration`, invalidates every binding cache, and resets the UBO and
  unpack rings. Called from `DirectGLES.cpp:10938`.
- **Buffer-mutation epoch**: `g_bufferMutationEpoch` (`Managers.cpp:653`),
  `CurrentBufferMutationEpoch`/`BumpBufferMutationEpoch` (`Managers.cpp:1493-1503`). The
  exhaustive list of bump sites and the acquire/release rules is documented at
  `Managers.h:556-590` — read it before touching anything, it is the correctness contract
  for the draw-clean memos.
- `g_bufferBackendIdGeneration` (`Managers.cpp:1502`, declared `Managers.h:706`): bumped
  whenever a LIVE resource's driver id is re-minted (persistent-map adoption at
  `Managers.cpp:1173`, immutable-store retire at `Managers.cpp:1639`), because VAO twins
  bake ids against frontend versions that do not move.

### 1.11 Three persistent-mapped rings + shared machinery

Documented at `Managers.h:729-825`; implemented `Managers.cpp:764-870` (types) and
`1922-2204` (create/allocate/present).

- `PersistentRingStore` (`Managers.cpp:800-819`), `RetiredRingStore` (`:823-827`),
  `RingFrameMark` (`:832-835`), `PersistentRing` (`:839-849`).
- Instances: `g_uboRing` (`Managers.cpp:851`), `g_unpackRing` (`:852-858`),
  `g_uploadRing` (`:867-873`). Caps: 4 MiB initial / 64 MiB max each
  (`Managers.cpp:770-784`).
- `CreateRingStorage` (`Managers.cpp:1930-1983`) — `glBufferStorageEXT` +
  `glMapBufferRange` persistent|coherent; retires the old store onto `ring.retired` with
  `CurrentFrameSerial()+1`.
- `RingAvailable` (`Managers.cpp:1989-2000`) — self-heals a stale context generation via
  `ResetRingForNewContext` (`Managers.cpp:866-876`).
- `RingAllocate` fast path `Managers.cpp:2016-2038`, slow path from `:2040`.
- Exported wrappers `Managers.cpp:2172-2203`. Present-time upkeep is called from
  `DirectGLES.cpp:10925-10928` (`UboRingOnPresent`, `UnpackRingOnPresent`,
  `UploadRingOnPresent`, `TrimBufferPool`).
- Upload-ring gate for the buffer path: `UploadRingUsableNow` (`Managers.cpp:963-967`) —
  kill switch `Features.EsprytDisableUploadRing`, `glCopyBufferSubData` present, ring up.
- `FlushPendingRangesNow` (`Managers.cpp:1004-1072`) is the three-tier drain and the
  single most delicate function in the file:
  1. `glMapBufferRange(WRITE | INVALIDATE_BUFFER|INVALIDATE_RANGE)` + memcpy, taken only
     for a whole-buffer flush or a range `>= kInvalidateRangeMinBytes` (128 KiB,
     `Managers.cpp:975`) — `Managers.cpp:1054-1067`;
  2. upload ring + `glCopyBufferSubData` — `Managers.cpp:1058-1067` (ring branch at
     `:1069-1076` in the listing order shown by the file: `RingAllocate`, `Memcpy`,
     `BindBufferId(GL_COPY_READ_BUFFER/GL_COPY_WRITE_BUFFER)`, copy);
  3. direct `UploadRangeNow`.
  Ranges are flushed **as queued** (`VecRange1D::Add` already merges near-adjacent ones);
  collapsing to the union re-copied whole chunk arenas — `Managers.cpp:1000-1003`.
  Kill switch `Features.EsprytDisableInvalidateFlush` (`Managers.cpp:1017`) forces tier 2.
- Unpack ring consumers (texture uploads, not buffers, but the same allocator):
  `Managers.cpp:3153,3172,3174,4484,4510,4562`.
- UBO ring consumer: `DirectGLES.cpp:3652-3684` (global default-uniform block).

**Observation, low risk**: `OnBackendContextDestroyed` (`Managers.cpp:1537-1538`) resets
`g_uboRing` and `g_unpackRing` but not `g_uploadRing`; `RingAvailable`
(`Managers.cpp:1996-1998`) heals it on first use, so this is benign today but is an
asymmetry worth preserving-or-fixing deliberately in P3a.

---

## 2. Every place Espryt holds or dereferences a frontend `BufferObject`

### 2.1 The twin registry question — answered

**The buffer twin is NOT one of the six `{slot, gen}` slot tables.** The six tables
(`SlotTables.h:1-80` explains the mechanism) are, by their `MGPipeKind` template argument:

| Kind | Declared | Defined |
|---|---|---|
| `VertexElementsCso` (VAO) | `Managers.h:954` | `Managers.cpp:2877` |
| `Texture` | `Managers.h:1275` | `Managers.cpp:5205` |
| `Framebuffer` | `Managers.h:1366` | `Managers.cpp:5958` |
| `ShaderCso` (program) | `Managers.h:1884` | `Managers.cpp:6269` |
| `SamplerCso` | `Managers.h:1984` | `Managers.cpp:8787` |
| `Renderbuffer` | `Managers.h:2011` | `Managers.cpp:8899` |

The `TwinRegistry` alias itself is `Managers.h:544-549`; `StateBackendObjectRegistry` is
`Managers.h:300`.

Buffers instead use **owner-embedded twinning**: the `GLESBufferResource` is owned by the
frontend `BufferObject` through `PipeResource::m_backend`
(`PipeResource.h:127-131`), reached by `GetBackendResource()` /
`SetBackendResource()` (`BufferObject.cpp:611,615`). Espryt's only accesses:

- `Managers.cpp:879` — `ResourceOf(BufferObject&)`, the ops' accessor
- `Managers.cpp:1142`, `1146` — `Ops_AcquirePersistentMap` get + create
- `Managers.cpp:1576` — `GetBufferResource(BufferObject*)` (no GL)
- `Managers.cpp:1582` — `IsBufferDrawClean` identity compare
- `Managers.cpp:1608`, `1613` — `EnsureBufferResource` get + create

Consequence for P3a: there is no existing slot table to extend; the buffer kind's
`{slot, gen}` table has to be introduced, and `SetBackendResource` disappears (matching
`docs/Disaggregated/ARCHITECTURE.md:284`, "`SetBackendResource` 删除（server 拥有资源表）").

### 2.2 Keyed by what, today

| Key | Site | Use |
|---|---|---|
| Raw `BufferObject*` | `DirectGLES.cpp:626` (`syncedBuffers[]` dedupe scratch), `DirectGLES.cpp:635` (`bufferKey`) | per-draw dedupe of the attribute walk |
| Raw `BufferObject*` | `Managers.h:915-923` (`ResolvedDrawBuffers::Entry::frontend`, `iboFrontend`) | VAO draw-buffer memo; pinned by the VAO's attribute `SharedPtr`s |
| Raw `const BufferObject*` | `Managers.cpp:2452`, `2636` (`m_syncedIndexBufferObject`) | IBO identity compare against a wrapping `Uint16` slot version |
| `GetLifetimeId()` | `Managers.cpp:2812-2820` (`stream.sourceLifetimeId`) | fp64 → fp32 converted-stream memo key |
| `GetChangeSerial()` | `Managers.cpp:2814`, `Managers.h:608` (`syncedChangeSerial`) | staleness of converted streams / backend storage |
| `SharedPtr<BufferObject>` | `DirectGLES.cpp:747` (`XfbCaptureTarget::buffer`) | keeps the capture target alive across a span |
| `SharedPtr<BufferObject>` (by value/ref from PipeInputs) | every `EnsureBufferResource(...)` call | see the site list below |

### 2.3 `EnsureBufferResource` / `GetBufferResource` call sites (Espryt)

`Managers.cpp`:
- `2337` `BindAttributeBuffer` (vertex attribute → `GL_ARRAY_BUFFER`)
- `2393` `SyncZeroStrideAttribute` (→ `glBindVertexBuffer`)
- `2622` `BackendVertexArrayObject::SyncToBackend`, index-buffer branch (→ `GL_ELEMENT_ARRAY_BUFFER`)
- `4724` texture-buffer (`TextureStorageType::Buffer`) → `glTexBuffer`/`glTexBufferRange`

`DirectGLES.cpp`:
- `411` `SyncBufferBindingPoints` (indexed UBO/SSBO/XFB points)
- `463` region `SyncTransformFeedbackBindingPoints`
- `526` `SyncAtomicCounterBuffers` (mapped onto reserved SSBO bindings)
- `560` `SyncBoundBuffer` (DrawIndirect / DispatchIndirect)
- `612`, `645` the `ResolvedDrawBuffers` memo repair and full walk
- `681` IBO memo miss
- `4185`, `4247` indirect-draw `mg_IndirectParams` SSBO view
- `4631` `BoundElementArrayBufferId()`
- `9411` `glReadPixels` pack PBO
- `9837` `glGetTexImage` pack PBO

### 2.4 Reads of `BufferObject` members from backend code

| Member | Espryt sites |
|---|---|
| `GetSize()` | `Managers.cpp:897, 1006 (limit), 1139, 1396, 1653, 1698, 1701, 2777, 4789 (via GetBufferRangeSizeInBytes compare), 4795`; `DirectGLES.cpp:299, 432-435 (range clamp), 480-483, 502-506, 7887, 7889, 9456, 9459, 9866, 9869`; `Utils.cpp:2322` |
| `GetUsage()` | `Managers.cpp:903` |
| `GetChangeSerial()` | `Managers.cpp:925, 1207, 1280, 1336, 1377, 1417, 1600, 1684, 1710-1714 (compare), 2814` |
| `GetLifetimeId()` | `Managers.cpp:2813` |
| `MappedData()` (host-visible base — shadow OR coherent map) | `Managers.cpp:911, 952, 1055, 1063, 1184, 1367, 1668, 2775`; `DirectGLES.cpp:301, 4965 region (restart rewrite `source = indexBuffer->MappedData()`), 5031-5038, 5074-5081`; `MultiDraw.cpp:513` |
| `HasDefinedContent()` | `Managers.cpp:911` |
| `IsMapped()` | `Managers.cpp:1594` (`IsBufferDrawClean`) |
| `IsBackendPersistentMapped()` | `Managers.cpp:2815`; `DirectGLES.cpp:851` |
| `GetExternalIndex()` (GL name, diagnostics only) | `Managers.cpp:4711`; `DirectGLES.cpp:9412, 9415, 9838, 9841` |
| `GetBackendResource()` | §2.1 list |
| `MarkGpuWritten()` | `DirectGLES.cpp:500` (`MarkShaderStorageBuffersGpuWritten`), `544` (`SyncAtomicCounterBuffers`), `2012` (`MarkWritableImageBufferTexturesGpuWritten`) |
| `WritebackFromBackend()` | `DirectGLES.cpp:868` (XFB `ReadbackCapturedRanges`), `976` (`ScatterCapturedRecords`), `7896` (`StoreReadbackRowsToClient`), `9460` (`glReadPixels` PBO), `9870` (`glGetTexImage` PBO); `Managers.cpp:1410` (`Ops_ReadbackFromGpu`); `Utils.cpp:2343` (`ReadPixelsViaFormatConversion`) |
| `SyncPersistentMappedRange()` / `SyncGpuWrites()` | §3 |

Note the "dirty ranges" question: Espryt never reads a frontend dirty-range list — the
frontend has none. Ranges arrive only as op arguments and live on the BACKEND resource
(`pendingRanges`).

`MGB_CTX` is the pushed `PipeInputs` block: `MG_Pipe/PipeInputsSwitch.h:19` (push) /
`:24` (pull). The buffer accessors that hand Espryt a `SharedPtr<BufferObject>` are
`PipeInputs::GetBufferBindingSlot` (`MG_Backend/MGPipe/PipeInputs.h:485-495`) and
`GetBufferBindingPoint` (`:496-508`); the fill-point coverage is
`MG_Pipe/FillPoints.def:156-158, 204-206, 269, 286, 289, 310-311` and
`MG_Pipe/Coverage.def:42-45`.

---

## 3. `SyncPersistentMappedRange` and `SyncGpuWrites`

### 3.1 What they do (frontend)

`SyncPersistentMappedRange` — `BufferObject.cpp:291-303`. Returns immediately unless the
buffer is mapped, NOT GPU-resident, PERSISTENT, WRITE, not FLUSH_EXPLICIT, with a
non-empty mapped range. Then it calls `NotifySubData(start, len)` — i.e. it synthesises a
`SubData` op for a write that never went through any GL entry point. For a GPU-resident
(adopted) buffer it is a pure no-op (`BufferObject.cpp:295`).

`SyncGpuWrites` — `BufferObject.cpp:319-328`. If `m_gpuWritePending` (set by
`MarkGpuWritten`, `BufferObject.cpp:314-317`), clears the flag **unconditionally** and
dispatches `ReadbackFromGpu`.

### 3.2 Espryt's share of the 20 `SyncPersistentMappedRange` sites — 8

| Site | Context |
|---|---|
| `DirectGLES.cpp:297` | `ResolveIndirectCommandBytes` — before reading indirect commands from the shadow |
| `DirectGLES.cpp:4710` | restart-substitution: element buffer, immediately followed by `SyncGpuWrites()` at `:4711` |
| `DirectGLES.cpp:4964`, `:4965` | `MultiDrawElementsIndirectCount` — draw + parameter buffer |
| `DirectGLES.cpp:5066`, `:5067` | `MultiDrawArraysIndirectCount` — draw + parameter buffer |
| `Managers.cpp:1700` | `EnsureBufferResource` — the per-draw push, before respecify/flush |
| `MultiDraw.cpp:511` | multi-draw index flattening |

(The other 11 backend sites are Magma's: `DirectVulkan.cpp:281,472,796`,
`UniformManager.cpp:2024`, `VkBufferManager.cpp:628,679`,
`VulkanRenderer.cpp:3542,3621,4016,7431,12426,12427`; plus the definition and the test
sites.)

### 3.3 Espryt's share of the 6 `SyncGpuWrites` backend sites — 3

| Site | Context |
|---|---|
| `DirectGLES.cpp:4711` | restart substitution reads the index shadow |
| `Managers.cpp:2774` | `SyncFloat64AttributeAsFloat32` before narrowing doubles from the shadow |
| `MultiDraw.cpp:512` | multi-draw index flattening |

(Magma's three: `VulkanRenderer.cpp:3541, 4015, 4349`. The frontend's two are
`MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:957, 995` — `glGetBufferSubData` /
`glGetNamedBufferSubData`.)

### 3.4 What a server without the frontend's address space needs instead

Both calls are the backend reaching **backwards** into client memory:

- `SyncPersistentMappedRange` exists only because the app writes into the shadow with no
  API call. A server cannot observe that. The design already names the replacement:
  a client-side tracker `m_livePersistentMaps` pushing block-granular dirty ranges at each
  validate point, over the union of exactly these 20 sites, chunked by
  `MOBILEGL_IPC_PERSISTENT_BLOCK_KB` (default 64) — `ARCHITECTURE.md:482`. Phase 1 is
  conservative (whole mapped span, block-split), Phase 2 exact (64 KiB dirty bits +
  `memcmp`). Also `ARCHITECTURE.md:481`: `ResourceRespecify/SubData` carry a
  `hasLiveHostWrites` bit — the one thing `IsBufferDrawClean` (`Managers.cpp:1594`) needs
  from the map state, with zero new record kinds.
- The five Espryt sites that follow `SyncPersistentMappedRange` with a **CPU read of
  `MappedData()`** (`DirectGLES.cpp:301`, `:4965`, `:5031-5038`, `:5074-5081`,
  `MultiDraw.cpp:513`) are the harder half: they are `*IndirectCount` count resolution,
  restart rewriting and multi-draw flattening. `ARCHITECTURE.md:221` moves
  `*IndirectCount` count resolution to the CLIENT (server receives resolved
  `MGPDrawRange[]`); `ARCHITECTURE.md:106` adds `kCapNeedsHostIndexBytes` so restart
  rewriting / multi-draw flattening get an index mirror on the server side
  (`ARCHITECTURE.md:387`, budget `MOBILEGL_PIPE_INDEX_MIRROR_MB` default 64).
- `SyncGpuWrites` becomes the `OnGpuWritten(res, rangeCount, ranges)` reverse callback
  (`MG_Pipe/MGPipeCallbacks.h:30-31`), and `WritebackFromBackend` becomes
  `OnBufferWriteback(res, offset, bytes)` (`MGPipeCallbacks.h:32`), batched per operation,
  ordered with respect to the epoch bump (`ARCHITECTURE.md:275`, `:288`).
- Espryt's two remaining `SyncGpuWrites` consumers (fp64 narrowing at `Managers.cpp:2774`,
  index flattening at `MultiDraw.cpp:512`) both read `MappedData()` right after — same
  index-mirror / client-side-resolution treatment.

---

## 4. Readback paths and write-back into the frontend

### 4.1 `glGetBufferSubData` (frontend, not Espryt)

`MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:957` and `:995` call `SyncGpuWrites()` then
`DownloadSubData` (`BufferObject.cpp:434`), which reads `m_resource.Bytes()` — the shadow
or the coherent map. Espryt's contribution is only `Ops_ReadbackFromGpu`
(`Managers.cpp:1382`). A non-persistent mapping makes the call an `INVALID_OPERATION`
(`GL_Buffer.cpp:948-955`).

### 4.2 Map-read

`BufferObject::AcquireMemory` (`BufferObject.cpp:466`) and `AcquireMemoryRange`
(`BufferObject.cpp:518`) call `SyncGpuWrites()` (`:467`, `:537`) and hand back a pointer
into `PipeResource::Bytes()`. Espryt never sees a read map directly — the reconciliation
is the `ReadbackFromGpu` op.

### 4.3 Pack-PBO readback (Espryt owns these)

Four distinct writeback shapes, all landing through `WritebackFromBackend` and each
followed by `BumpBufferMutationEpoch()`:

1. `glReadPixels` native + PBO: `DirectGLES.cpp:9406-9468`. Resolves the PBO via
   `EnsureBufferResource` (`:9411`), scopes the driver binding with
   `ScopedPixelPackBuffer` (`DirectGLES.cpp:6435-6441`), `glReadPixels`, then maps the PBO
   read-only and `WritebackFromBackend({pboMappedPtr, size}, 0)` (`:9460`), epoch bump
   (`:9462`).
2. `glGetTexImage` + PBO: `DirectGLES.cpp:9832-9880`, structurally identical (`:9870`).
3. Row-by-row conversion readbacks: `StoreReadbackRowsToClient`
   (`DirectGLES.cpp:7876-7906`) — per-row `WritebackFromBackend` at `:7896`, ONE epoch
   bump for the whole loop at `:7903`. Same shape in
   `Utils.cpp:2297-2354` (`:2343` writeback, `:2352` bump).
4. Transform feedback: `XfbImpl::ReadbackCapturedRanges` (`DirectGLES.cpp:838-878`) maps
   each capture range read-only and writes back at `:868` (+ bump `:872`); persistently
   mapped targets are skipped at `:851`. `ScatterCapturedRecords`
   (`DirectGLES.cpp:918-985`) is a read-modify-write of the frontend shadow: it starts
   from `target.buffer->MappedData()` (`:961`), patches the captured varyings in, writes
   back (`:976`), bumps (`:978`), and re-uploads with `glBufferSubData` (`:980-984`).
   `ARCHITECTURE.md:310` and `MGPipeCallbacks.h:49-51` (`OnXfbScatterReady`) already
   specify how this survives the split — the scatter loop moves to the client.

Driver-side pack/unpack binding shadows that every readback routes through:
`BindPixelPackBufferId` (`Managers.cpp:1739`), `BindPixelUnpackBufferId`
(`Managers.cpp:1748`), `InvalidatePixelBufferBindingCaches` (`Managers.cpp:1757`),
`NoteBufferIdDeleted` (`Managers.cpp:1764`). Resting state is 0; the reason is documented
at `Managers.h:670-682`.

---

## 5. The Adreno SIGSEGV workaround the roadmap says to keep

`ROADMAP.md:19` names it as "Adreno SIGSEGV workaround 保留". `ARCHITECTURE.md:316`
repeats it in the server-local-invariants list ("Adreno 禁用属性 SIGSEGV workaround").
`grep SIGSEGV` in `MobileGL/MG_Backend/DirectGLES/` returns exactly three hits, and they
are **two distinct workarounds plus one cross-reference**:

### 5.1 GL_DOUBLE / IsLong attribute with no buildable stream

`Managers.cpp:2517-2540` (rationale `:2516-2521`, and the forward reference at
`Managers.h:888-895`): becoming long bumps `FormatVersion`, not `SwitchVersion`, so the
enable/disable block above does not re-run and an already-enabled array would stay enabled
with **no pointer and no `GL_ARRAY_BUFFER` binding**. That is not merely
`GL_INVALID_ENUM` — "the Adreno driver dereference[s] null inside the next draw and take[s]
the process with it (SIGSEGV in libGLESv2_adreno,
KHR-GL43.vertex_attrib_binding.basic-input-case4)". Fix: try
`SyncFloat64AttributeAsFloat32` (`Managers.cpp:2757-2874`), and on failure
`glDisableVertexAttribArray(attribIndex)` (`Managers.cpp:2537`) with a `MGLOG_W_ONCE`.
Note the re-enable at `Managers.cpp:2529` is explicitly non-redundant.

### 5.2 GL_BGRA vertex size refused by the driver

`Managers.cpp:2568-2612` (rationale `:2568-2582`). ES has no `GL_BGRA` vertex size; the
rejection "leaves the array ENABLED with no pointer, and the Adreno driver then
dereferences null inside the next draw and kills the process rather than reporting an
error (SIGSEGV in libGLESv2_adreno,
KHR-GL43.vertex_attrib_binding.basic-input-case5)". Mechanism: drain `glGetError` before
the call (`Managers.cpp:2585-2588`), issue `glVertexAttribPointer` with
`GL_BGRA` as the size (`Managers.cpp:2593-2596`), and if `glGetError() != GL_NO_ERROR`
(`Managers.cpp:2603`) disable the array. Deliberately **only** for `attrib.IsBgra` — the
comment says the per-draw sync must not grow a `glGetError` round trip for real formats.

Both live inside `BackendVertexArrayObject::SyncToBackend`, i.e. in P3a's vertex-elements
half, and both depend on being able to observe a DRIVER error at attribute-declaration
time — which under a split means the check has to stay server-side and the disable has to
be a server-local decision, not a client one.

---

## 6. Test coverage that P3a must keep green

### 6.1 `MG_IntegrationTest` scenarios touching buffers on Espryt

Roadmap-named (`ROADMAP.md:19`): `LargeArenaAdoption`, `StorageBufferRegrow` (publishes
`map-persistent-roundtrips`), `VertexAttribBinding`, `MultiDraw`, `PrimitiveRestart`.

Full `TEST_F` names, by file under `MobileGL/MG_IntegrationTest/Scenarios/`:

- `LargeArenaAdoptionScenario.cpp` — `SubDataAfterAnInFlightDrawReachesTheNextDraw` (:204),
  `ReadbackSeesTheLatestCpuWrite` (:227), `GpuWriteIntoTheArenaIsReadBack` (:245). File
  header at `:13` names `TryAdoptLargeStorage` explicitly.
- `ResidentIndexScenario.cpp` — `PersistentCoherentEboWrittenEveryFrame` (:186),
  `EboAlsoBoundAsVertexBufferLater` (:225), `EboDeletedAndRecreatedAtTheSameName` (:271),
  `OneEboTwoVaosMutatedAcrossFrames` (:311), `PromotedDynamicEbo` (:351). Header comment at
  `:40` names `BufferObject::SyncPersistentMappedRange`.
- `StorageBufferRegrowScenario.cpp` — `AGrownStoreIsVisibleThroughItsExistingIndexedBinding`
  (:124). This is the case behind `InvalidateIndexedBufferBindingShadowsForId`
  (`Managers.cpp:933`).
- `CrossFrameBufferScenario.cpp` — `VertexBufferSubData` (:409), `VertexMapWriteUnmap`
  (:414), `VertexPersistentMapFlush` (:419), `VertexPersistentCoherentWrite` (:425),
  `VertexOrphanAndReupload` (:431), `VertexCopyBufferSubData` (:437), `IndexBufferSubData`
  (:446), `IndexMapWriteUnmap` (:451), `IndexPersistentMapFlush` (:456),
  `IndexPersistentCoherentWrite` (:481), `IndexOrphanAndReupload` (:487),
  `IndexCopyBufferSubData` (:493), `PartialStalenessIsCaughtByWholeRegionChecks` (:518);
  plus `StreamedArenaScenario` in the same file: `StreamedVertexDataSurvivesArenaRecycling`
  (:675), `StreamedIndexDataSurvivesArenaRecycling` (:731) — these two are the buffer-pool
  recycle cases.
- `BufferTextureScenario.cpp` — `VertexStageTexelFetchReadsTheBufferAndTracksItsUpdates`
  (:162), `AnImageStoreIntoABufferTextureIsVisibleToTheCpu` (:238),
  `LevelQueriesDescribeTheAttachedBufferRange` (:311). Covers `Managers.cpp:4707-4800`.
- `XfbCaptureBufferReuseScenario.cpp` — `EverySpanIntoABufferObjectOfItsOwn` (:182),
  `EverySpanIntoOneRespecifiedBufferObject` (:215),
  `ARespecificationMayChangeTheCaptureBufferSize` (:243),
  `EverySpanIntoOneImmutableStorageBuffer` (:283).
- `AtomicCounterScenario.cpp` — `DispatchIncrementsTheBoundCounterBuffers` (:172),
  `CountersAccumulateAcrossDispatchesAndFollowAReseed` (:201),
  `SubDataAfterDispatchSurvivesAnImmediateReadback` (:242) — the `MarkGpuWritten` /
  `ReadbackFromGpu` round trip.
- `PrimitiveRestartScenario.cpp` — 7 cases (:215, :236, :263, :281, :306, :332, :368);
  these drive the `MappedData()` index rewrite at `DirectGLES.cpp:4964-4980`.
- `MultiDrawScenario.cpp` — 10 cases (:225 … :526); drive `MultiDraw.cpp:495-525`.
- `PackedWordReadbackScenario.cpp` — :148, :167, :194 (PBO/readback writeback paths).
- `VertexAttribBindingScenario.cpp`, `VertexArrayEnableDisableScenario.cpp`,
  `DoublePrecisionScenario.cpp` — the vertex-elements half incl. the fp64 narrowing and
  the two SIGSEGV workarounds.

### 6.2 Unit tests

- `MobileGL/MG_Test/Buffer/BufferTest.cpp` — 88 `TEST` macros. The ones that exercise the
  ops table directly use mock tables defined in-file: `kZeroCopyMockOps` (:1609),
  `kResidentSubDataMockOps` (:1640), `ScopedBackendOps` RAII (:1651-1654). Directly
  ops-shaped cases: `General_PersistentCoherentZeroCopyStressNoPerDrawReupload` (:1658),
  `General_PersistentCoherentFallbackSyncsPerDrawWhenBackendDeclines` (:1724),
  `General_CoherentAsFlush_*` (:1778, :1809, :1819, :1855, :1890),
  `RedefiningAnAdoptedBufferHandsTheMappingBack` (:1952),
  `RedefiningAnAdoptedBufferAtANewSizeStaysInBounds` (:1990),
  `ImmutableStorageOnAnAdoptedBufferHandsTheMappingBackToo` (:2029),
  `RedefiningAnAdoptedBufferToZeroBytesLeavesItOnTheShadow` (:2058),
  `RedefiningANonAdoptedBufferIsUnchanged` (:2093), the adopted-store map/unmap family
  (:2165, :2204, :2248, :2279, :2316), the `ResidentSubData` family (:2364, :2412, :2557,
  :2616, :2698), `MapBufferRangeAndUnmapBufferReseedAnAdoptedShaderStorageBuffer` (:2451),
  `ANonPersistentWriteMapOfAShadowBackedStoreStillFlushesThroughTheBackend` (:2483),
  `GlBufferSubDataIntoAnAdoptedStoreLandsInPlaceWithoutABackendTransfer` (:2533),
  `GlClearBufferSubDataRepeatsItsPatternThroughAnAdoptedStoreInPlace` (:2589),
  `GlCopyBufferSubDataIntoAnAdoptedStoreLandsAtTheDestinationOffset` (:2665),
  `AnExplicitFlushOfAPersistentMapOfAnAdoptedStoreOnlyPublishesTheChange` (:2733),
  `AZeroLengthExplicitFlushOfAnAdoptedStoreLeavesItUndefined` (:2779),
  `AWriteMapThatDiscardsWhatItMapsDoesNotReconcileAnAdoptedStoreAtMapTime` (:2817),
  `AStorageBindingDoesNotAdoptTheStoreWhileTheApplicationHoldsAMapping` (:2868),
  `RespecifyingAStoreWhileItIsMappedDoesNotLandTheStagedBytesIntoIt` (:2917),
  `RespecifyingAShadowBackedStoreWhileItIsMappedPushesNoRangeDown` (:2943).
- `MobileGL/MG_Test/SanityTest.cpp:2535` —
  `DirectGLESStateGuards.PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero`, the only
  unit test that pokes `BufferImpl` symbols directly (`:2539-2551`).
- `MobileGL/MG_Test/Backend/DirectGLES/` contains **no** buffer tests
  (`BaseInstanceInjectionTest.cpp`, `EsslShaderPassTest.cpp`,
  `ViewportIndexRoutingTest.cpp` only). `MG_Test/SelfTest/` likewise has none.
- No unit test covers `EnsureBufferResource`, `IsBufferDrawClean`, the pool, the rings, or
  the deferred-release list — those are integration-only today. Worth knowing before P3a
  refactors them.

### 6.3 Trace cases the roadmap names (`ROADMAP.md:19` — "Create/rd12/26.3/sodium trace")

Fixtures present in `tools/trace_replay/fixtures/`:

| Roadmap name | Fixture |
|---|---|
| Create ×2 | `minecraft-1.21.1-neoforge-create-indirect-in-world.tgz` (ref `.0000504631.png`) and `minecraft-1.21.1-neoforge-create-instancing-in-world.tgz` (ref `.0000530333.png`) |
| rd12 | `minecraft-1.21.4-rd12-odinlite-in-world.tgz` (ref `.0004660351.png`) |
| 26.3 | `improved-transparency-minecraft-26.3.tgz` (ref `.0002667619.png`) |
| sodium | `minecraft-1.21.4-fabric-sodium-in-world.tgz` (ref `.0000923340.png`) |

`ARCHITECTURE.md:485` records that both Create fixtures carry `coherent_as_flush: true`,
so under split they must take the same buffer path as monolith for a name-by-name compare.
`ROADMAP.md:90` warns that the create-indirect fixture already fails on Adreno 830 at
`dev@81b17c0b` (baseline APK reproduces) — it is on the P3a/P8 acceptance list but must be
fixed on `dev` first. `ROADMAP.md:19` also demands MC 26.3 p99 unchanged on Adreno.

---

## 7. Short list of the traps a P3a implementer will hit

1. Every hook takes `BufferObject&`, and four of them read `MappedData()` — the
   conversion is not "rename seven functions", it is "decide who owns the bytes".
2. `EnsureBufferResource` (`Managers.cpp:1602`) both CREATES the twin and calls back into
   the frontend (`SyncPersistentMappedRange` at `:1700`). That callback is the pull the
   split must invert.
3. `IsBufferDrawClean` (`Managers.cpp:1579`) reads five frontend facts —
   `GetBackendResource()`, `IsMapped()`, `GetSize()`, `GetChangeSerial()` — on the hot
   path, per draw, per buffer. `ARCHITECTURE.md:481` reduces it to one
   `hasLiveHostWrites` bit; the size/serial half still needs a home.
4. The mutation-epoch bump list (`Managers.h:556-590`) is exhaustive by design, and three
   of its entries are backend-initiated writebacks with NO op (`DirectGLES.cpp:872, 978,
   7903, 9462, 9872`; `Utils.cpp:2352`). Under split the bump becomes an ordering rule on
   the reverse channel (`ARCHITECTURE.md:288`).
5. `g_bufferBackendIdGeneration` (`Managers.cpp:1502`) exists purely because VAO twins bake
   driver ids against frontend versions. It stays server-side, but the VAO half of P3a has
   to keep reading it (`Managers.cpp:2446`, `:2649`).
6. `PendingResidentWrite` (`Managers.h:620-624`) is the only place Espryt COPIES app bytes
   into its own storage. `BufferSubDataResident` is flagged `kOptional` in the catalogue
   (`ARCHITECTURE.md:91,94`) — Magma deliberately does not register it.
7. `TempBufferTarget` is `GL_ARRAY_BUFFER` (`Managers.h:552`) and the redundant-bind cache
   for it (`Managers.cpp:631-632`) is invalidated from seven places. Any new code path that
   binds `GL_ARRAY_BUFFER` behind `BindBufferId`'s back silently corrupts it.
