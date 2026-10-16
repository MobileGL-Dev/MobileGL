# Track H, slice 1 — scout report

Worktree: `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`, branch `feat/disaggregated` @ `e7a6a72f`.
All paths below are relative to that root unless spelled absolutely. Every line number was opened.

Scope per ROADMAP P2 (`docs/Disaggregated/ROADMAP.md:18`): *"第一片 Track H：Espryt 0b（`SlotAllocator` + 6 个 registry → slot 数组 + 删 `TwinLookupMemo`×3/`OwnerEquals`/`g_fbSlotCache`/GC）与 Magma 子系统 4（`VertexInputStateFactory`/`VaoDrawMemo` 重键，删前端 VAO 里的后端裸指针）；`MOBILEGL_PIPE_LEGACY_MEMOS`"*.

---

## 1. Espryt 0b — `StateBackendObjectRegistry` and the memos layered on it

### 1.1 The template itself

`MobileGL/MG_Backend/DirectGLES/Managers.h:271-395` — `template <typename StateObject, typename BackendObject> class StateBackendObjectRegistry`.

* Key type: **raw `StateObject*`** (the frontend heap address). `using BackendMap = UnorderedMap<StateObject*, Entry>;` at `Managers.h:285`. `UnorderedMap` here is the ska/flat open-addressed map (see the erase-relocation warnings quoted below), *not* `std::unordered_map`.
* `struct Entry { BackendPtr backend; StateWeakPtr stateRef; }` — `Managers.h:281-284`. The `weak_ptr` is the ABA defence: a recycled heap address is caught because the snapshot pins the dead object's control block. Comment at `Managers.h:277-280` says a separate liveness map was tried and removed (second hash lookup on a path the draw runs ~10×).
* `GetOrCreate(const StatePtr&)` — `Managers.h:289-323`. Calls `EnsureProcessTeardownSentinel()` (`Managers.h:295`), runs a *deferred* `CollectGarbage()` when `m_creationTick >= kCreationGCInterval` **before** taking the entry reference (`Managers.h:300-303`; the comment at `296-299` explains that erasing after taking the reference would invalidate it because the map is open-addressed), then `m_entries[stateObj.get()]`, ticks `m_creationTick` only on a genuinely new key (`Managers.h:305-318`), resets `entry.backend` when `entry.stateRef.expired()` (`Managers.h:319-322`), re-seeds `entry.stateRef = stateObj`, returns `BackendPtr&`.
* `Find(StateObject*)` — `Managers.h:334-345`. **Find erases**: on an expired `stateRef` it calls `m_entries.erase(entryIt)` and returns null (`Managers.h:339-342`). The doc comment at `Managers.h:326-333` is explicit that an erase relocates the *rest of the probe cluster*, so a `BackendPtr*` returned by `Find` is invalidated by any later registry call — callers must copy the `SharedPtr` out. `const` overload at `Managers.h:347-349` `const_cast`s and therefore also erases.
* Iteration: `begin()/end()` at `Managers.h:351-354` — used by the FBO scan in §1.3.
* GC: `CollectGarbageIfNeeded()` `Managers.h:356-364` (ticks `m_gcTick`, collects every `kGCInterval = 1024`), `CollectGarbageNow()` `Managers.h:366`, private `CollectGarbage()` `Managers.h:369-391` (two-pass: gather `staleKeys` where `stateRef.expired()`, then erase; re-entrancy guarded by `m_isCollecting`).
* Constants/members: `kGCInterval = 1024` (`Managers.h:394`), `kCreationGCInterval = 64` (`Managers.h:396`), `m_entries`, `m_gcTick`, `m_creationTick`, `m_isCollecting` (`Managers.h:397-400`).
* Teardown interlock: `InProcessTeardown()` / `EnsureProcessTeardownSentinel()` declared `Managers.h:59-60`, documented `Managers.h:44-58`, defined `Managers.cpp:161-170` (a `std::atexit` handler latched via `std::call_once`; a registry destructor hook was tried and is explicitly wrong because tests destroy temporary registry instances mid-run). Twin destructors also compare `g_backendContextGeneration` (`Managers.h:64-70`) so a twin outliving its ES context does not `glDelete*` a recycled name.

### 1.2 The six instances

| # | Instance | Declared | Defined | Maps |
|---|---|---|---|---|
| 1 | `VertexArrayImpl::g_backendVertexArrayObjects` | `Managers.h:803-804` | `Managers.cpp:2747-2748` | `MG_State::GLState::VertexArrayObject*` → `SharedPtr<BackendVertexArrayObject>` |
| 2 | `TextureImpl::g_backendTextureObjects` | `Managers.h:1124-1125` | `Managers.cpp:5075` | `ITextureObject*` → `SharedPtr<BackendTextureObject>` |
| 3 | `FramebufferImpl::g_backendFramebufferObjects` | `Managers.h:1215-1216` | `Managers.cpp:5828-5829` | `FramebufferObject*` → `SharedPtr<BackendFramebufferObject>` |
| 4 | `PrgramImpl::g_backendProgramObjects` | `Managers.h:1733-1734` | `Managers.cpp:6139` | `ProgramObject*` → `SharedPtr<BackendProgramObjectImpl>` |
| 5 | `SamplerImpl::g_backendSamplerObjects` | `Managers.h:1833-1834` | `Managers.cpp:8657` | `SamplerObject*` → `SharedPtr<BackendSamplerObject>` |
| 6 | `RenderbufferImpl::g_backendRenderbufferObjects` | `Managers.h:1860-1861` | `Managers.cpp:8769-8770` | `RenderbufferObject*` → `SharedPtr<BackendRenderbufferObject>` |

Note: the key of #2 is the polymorphic base `ITextureObject`, so texture **views** (`TextureObjectView`) and ordinary textures share one keyspace — the handle kind `Texture` must cover both (`MGPipeHandles.h:27`).

### 1.3 Every lookup / insert / erase / GC site

**`DirectGLES.cpp` (all sites, with enclosing function):**

| Line | Enclosing function | Op |
|---|---|---|
| 1209-1210 | `VertexArrayImpl::ResolveVaoTwin` (`1202`) | VAO `Find` then `GetOrCreate`, creates twin if slot null, stores into `g_vaoTwinLookupMemo` (`1214`) |
| 1223 | `VertexArrayImpl::SyncCurrentVAO` (`1216`) | VAO `CollectGarbageIfNeeded` |
| 1306-1308 | `TextureImpl::SyncTextureObjectToBackend` (`1300`) | texture `Find`/`GetOrCreate`; copies the twin out by value (`1321`) *because the sync can re-enter via `SyncTextureViewToBackend` and relocate the map* |
| 1340-1342 | same function, tail | re-resolve after the nested sync; restores the twin if a collection took the slot |
| 1533 | `TextureImpl::SyncNeccessaryTextures` (`1529`) | texture `CollectGarbageIfNeeded` |
| 1898-1900 | `FramebufferImpl::SyncCurrentFBO` (`1894`) | GC ×3: FBO, texture, renderbuffer |
| 1960 | `SyncCurrentFBO`, draw==read fast path | FBO `Find` → `SyncReadBufferToBackend` |
| 1969-1971 | `SyncCurrentFBO`, main path | FBO `Find`/`GetOrCreate` + create |
| 2728-2729 | `PrgramImpl::SyncCurrentProgram` (`2724`) | GC ×2: program, sampler |
| 2771-2777 | `SyncCurrentProgram` | program `Find`/`GetOrCreate` + create + `g_programTwinLookupMemo.Store` |
| 2876-2879 | `BindCurrentFBO` (`2856`) | FBO `Find` (memo-miss fallback) + `g_fboTwinLookupMemo.Store` |
| 2917-2922 | `SyncAndBindFramebufferObject` (`2897`) | FBO `Find`/`GetOrCreate` + create |
| 3112 | `ResolveAndBindUnitTextures` (`3025`) | texture `Find` |
| 3170 | `ResolveUnitSamplerBackend` (`3164`) | sampler `Find`, memoised (see §1.5) |
| 3387 | `BindCurrentProgramWithResources` (`3373`) | program `Find` |
| 3622 | `BindCurrentProgramWithResources` | sampler `GetOrCreate` (this is where a unit's sampler twin is first created — the reason `ResolveUnitSamplerBackend` must never latch a miss) |
| 3671 | `GetCurrentBackendProgram` (`3663`) | program `Find` |
| 4101-4103 | `GetBackendProgramId` (`4089`) | program `Find`/`GetOrCreate` |
| 4574 | `DrawArrays` (`4566`) | VAO `Find` |
| 4615 | `MultiDrawArrays` (`4599`) | VAO `Find` |
| 6127-6130 | `UpdateTextureBindingAtTarget` (`6105`) | texture `Find`/`GetOrCreate` |
| 6407 | `ScopedDetachedTextureFramebufferAttachments` ctor (`6401`) | texture `Find` |
| 6413-6425 | same ctor | **full iteration** over `g_backendFramebufferObjects`, dereferencing `it->first` (the raw key) only after checking `it->second.stateRef.expired()` — the one site that reads the registry's internals directly |
| 6705 | `CopyTexImage2D` (`6678`) | texture `Find` |
| 6800 | `CopyTexSubImage2D` (`6771`) | texture `Find` |
| 7064-7067 | `SyncRenderbufferObjectToBackend` (`7060`) | renderbuffer `Find`/`GetOrCreate` + create |
| 7363 | `ShaderStorageBlockBinding` (`7357`) | program `Find` |
| 9206 | `GetTexImage` (`9168`) | texture `Find` |

**`Managers.cpp`:**

| Line | Context | Op |
|---|---|---|
| 5222-5228 | `BackendFramebufferObject::SyncAttachmentObject` (`5216`) | texture `Find`/`GetOrCreate` + create |
| 5273-5279 | same function, renderbuffer branch | renderbuffer `Find`/`GetOrCreate` + create |
| 5771 | attachment verification asserts (debug) | texture `Find` |
| 5788 | same | renderbuffer `Find` |

**`MG_Test/SanityTest.cpp`** (test-only, but it *copies and restores whole registry instances*, so a slot-array rewrite has to keep that possible):
`160` (copy-construct `previousRegistry`), `164` (`= {}` reset), `178` (restore), `195` (`decltype` member) — the `ScopedDirectGLESTextureBindings` fixture at `145-197`; `321`, `365` `GetOrCreate`; `3031`, `3051` `Find` in the sampled-set staleness test.

### 1.4 `TwinLookupMemo` ×3 and `OwnerEquals`

* `OwnerEquals` — `DirectGLES.cpp:63-66`, a free template: `!snapshot.owner_before(current) && !current.owner_before(snapshot)`. Rationale comment `60-62` (raw addresses lie; a held `weak_ptr` pins the control block).
* `TwinLookupMemo<StateObject, BackendObject, kIndexBits>` — `DirectGLES.cpp:83-116`. `Lookup` (`86-92`) probes `m_slots[IndexFor(ptr)]`, requires `slot.key == stateObj.get() && slot.twin && OwnerEquals(slot.owner, stateObj)`. `Store` (`94-99`). `struct Slot { StateObject* key; WeakPtr<StateObject> owner; BackendObject* twin; }` (`102-106`). `IndexFor` is Fibonacci hashing of the address, top `kIndexBits` (`108-113`). Backing store `Array<Slot, 1<<kIndexBits>` (`115`). The 20-line safety argument at `68-82` is the thing a handle rewrite must reproduce: it rests entirely on "a live state object's entry is never erased nor its twin replaced".
* Three instances:
  * `g_vaoTwinLookupMemo`, 12 bits = 4096 slots, `DirectGLES.cpp:123-124` (sizing rationale `118-122`: Minecraft cycles hundreds of section VAOs/frame).
  * `g_programTwinLookupMemo`, 8 bits = 256 slots, `DirectGLES.cpp:125-126`.
  * `g_fboTwinLookupMemo`, 6 bits = 64 slots, `DirectGLES.cpp:131-132` (rationale `127-130`).
* Use sites: `1206` + `1214` (VAO), `2769` + `2777` (program), `2874` + `2879` (FBO).
* Other `OwnerEquals` callers, **outside** the three memos — these die with it and must be re-expressed:
  * `DirectGLES.cpp:1392` and `1394` — `UnitBindingsUnchanged`, comparing the `UnitBindingsSnapshot` (`1366-1370`: `Array<WeakPtr<ITextureObject>, TextureTargetCount> slotObjects` + `WeakPtr<SamplerObject> samplerObject`) captured by `CaptureUnitBindings` (`1372-1384`) against the live units. Global snapshot store `g_observedUnitBindings` / `g_observedUnitBindingsContextId` at `1399-1400`.
  * `DirectGLES.cpp:3167` — `ResolveUnitSamplerBackend` (§1.5).

### 1.5 `UnitSamplerLookupMemo` (the fourth weak-ptr memo; ARCHITECTURE §9.5 lists it among the 11 deletions)

`DirectGLES.cpp:3157-3161` — `struct UnitSamplerLookupMemo { WeakPtr<SamplerObject> frontend; BackendSamplerObject* backend; }`, array `g_unitSamplerLookupMemos` per texture image unit (`3162-3163`). `ResolveUnitSamplerBackend` at `3165-3177`. Contract comment `3148-3156`: a **miss is never cached**, because the sampler twin is created later in the same draw by the program pass (`DirectGLES.cpp:3622`). The downstream walk memo lives at `3195-3200` (`g_unitSamplerWalkContextId`, `…Epoch`, `…MaxUnit`, `…ContextGeneration`, `…Valid`, `…Rows`).

### 1.6 `g_fbSlotCache` / `g_fbSlotCacheContext` and the P1-accessor bypass

`DirectGLES.cpp:134-157`:

```
143  using FbBindingSlot =
144      std::remove_reference_t<decltype(MGB_CTX->GetFramebufferBindingSlot(FramebufferTarget::Draw))>;
145  static const void* g_fbSlotCacheContext = nullptr;
146  static Array<FbBindingSlot*, SizeT(FramebufferTarget::FramebufferTargetCount)> g_fbSlotCache = {};
147  static inline FbBindingSlot& GetFramebufferBindingSlotFast(FramebufferTarget target) {
148      const void* ctx = MGB_CTX_IDENTITY;
149      if (ctx != g_fbSlotCacheContext) { ... refill all slots from *MGB_CTX ... }
156      return *g_fbSlotCache[SizeT(target)];
157  }
```

Why it exists (comment `134-142`): the frontend getter linear-scans, and the draw path asks several times per draw. The frontend getter it replaces is `GLContext::GetFramebufferBindingSlot` at `MG_State/GLState/Core.cpp:1180`, which forwards to `FramebufferState::GetFramebufferBindingSlot` — a **linear scan over `m_bindingSlots`** at `MG_State/GLState/FramebufferState/FramebufferState.cpp:48-57`.

**The P1 bypass, precisely.** Under the push arm `MGB_CTX` is `&gPipeInputs` (`MG_Pipe/PipeInputsSwitch.h:19`) and `MGB_CTX_IDENTITY` is `gPipeInputs.ContextIdentity()` (`PipeInputsSwitch.h:21` → `MG_Backend/MGPipe/PipeInputs.h:191`, backed by `m_contextIdentity` at `PipeInputs.h:614`, written in `PipeFill.cpp:270`). The accessor `PipeInputs::GetFramebufferBindingSlot` at `PipeInputs.h:508-517` carries `MGP_INPUT_CHECK` (per-verb poison, `PipeInputs.h:44-49`) and `MGP_INPUT_VERIFY_READ` (`PipeInputs.h:62-63`) and returns `*m_framebufferBindingSlot[index]` — a **pointer into the live `GLContext`**, filled per verb at `MG_Impl/Pipe/PipeFill.cpp:123-128` from `FillPoints.def` rows `156, 175, 188, 203, 247, 252, 284` (kDraw, kDispatch, kClear, kBlitOrCopy, kTextureOp, kReadback, kProgramOp).
The fast getter runs the checked accessor **once per context change** and then hands out the cached raw pointer forever, so on every later call the per-verb poison stamp and the verify read-hook are skipped. A verb that legitimately never fills `GetFramebufferBindingSlot` cannot be caught at these five call sites, and a verify build compares the field only when the slow accessor is used.

**Its five callers** (all in `DirectGLES.cpp`):
1. `1613` — inside `TextureImpl::SyncNeccessaryTextures` (`1529`), reads the Draw slot.
2. `1907` — `FramebufferImpl::SyncCurrentFBO` (`1894`), loop over {Draw, Read}.
3. `2745` — `PrgramImpl::SyncCurrentProgram` (`2724`), Draw slot.
4. `2860` — `BindCurrentFBO` (`2856`).
5. `2935` — `ForceBindCurrentFBO` (`2931`).

Remaining **unc­ached** `MGB_CTX->GetFramebufferBindingSlot` reads (they keep the poison and stay correct, but a handle rewrite touches them too): `DirectGLES.cpp:4217`, `6046`, `6047`; `Managers.cpp:5356`, `5461`; Magma: `UniformManager.cpp:556`, `VulkanRenderer.cpp:2989, 5188, 5408, 6020, 6111, 6465, 7261, 7629, 8587, 8588, 9223, 9934`.

### 1.7 The FBO-identity comparisons the reserved handle `{0,1}` retires

`DirectGLES.cpp:2874` (`currentFBO != pDefaultFramebufferInfo->defaultFBO`), `2898` (same in `SyncAndBindFramebufferObject`), `1939` (`currentFBO == …->defaultFBO` in `SyncCurrentFBO`), plus `stateFBO->IsDefaultFramebuffer()` in the FBO scan at `6420`. `MGPipeHandles.h:67-73` reserves `kMGPipeDefaultFramebuffer{0,1}` exactly for these.

### 1.8 What the replacement has to provide

A `SlotAllocator` (planned home `MobileGL/MG_Impl/Pipe/`, `ARCHITECTURE.md:552`) plus per-kind dense arrays must supply, at minimum:

1. **Client-minted `{slot, gen}` per kind** with free list + high-water mark, `gen++` on slot reuse only (`MGPipeHandles.h:19-22, 44-48`; `ARCHITECTURE.md:36-37`), first allocatable slot 1 (`MGPipeHandles.h:81`), debug assert on `gen` wrap.
2. **`lifetimeId → slot` map on the client** (`ARCHITECTURE.md:40`) — the frontend keeps `GetLifetimeId()` as its own identity; GL names never enter a key (`GlNameForDiag` only).
3. **Twin storage indexed by slot** replacing all six `BackendMap`s — this is what makes `Find` O(1) with no probe cluster, and removes the "reference into the map is invalidated by the next call" hazard that `SyncTextureObjectToBackend` (`DirectGLES.cpp:1310-1345`) currently pays a by-value copy + a second `Find` to survive.
4. **Explicit destroy** so the six GCs (`CollectGarbageIfNeeded` at `DirectGLES.cpp:1223, 1533, 1898, 1899, 1900, 2728, 2729`) and the `weak_ptr` liveness probe disappear. Today **nothing tells the backend an object died** except buffers (§4).
5. An **iterable live set per kind** for the one scan site (`DirectGLES.cpp:6413-6425`) — a dense array iterates naturally, but a "which slots are live" bit needs to exist.
6. **Default-framebuffer identity** as `{0,1}` to retire the four `pDefaultFramebufferInfo->defaultFBO` compares (§1.7).
7. A **slot-indexed framebuffer binding table** replacing `g_fbSlotCache`, so `GetFramebufferBindingSlotFast` becomes an ordinary indexed read of pushed state rather than a cached pointer into frontend storage. This is where the P1 bypass gets closed rather than preserved.
8. Test-fixture support for save/reset/restore of a whole kind's table (`SanityTest.cpp:160-195`).
9. `MOBILEGL_PIPE_LEGACY_MEMOS` (default `true`, `MobileGL/Config.h:356-360`) keeps the registries and `TwinLookupMemo`s compiled alongside the new path for the A/B window; both arms must live behind the same `PipeInputs` interface (`ARCHITECTURE.md:367`).

**Every file that changes for Espryt 0b:** `MobileGL/MG_Backend/DirectGLES/Managers.h` (registry template, 6 externs, twin classes' destructor/context-generation contract), `MobileGL/MG_Backend/DirectGLES/Managers.cpp` (6 definitions + 4 use sites), `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` (~40 sites above, plus `OwnerEquals`, 3 `TwinLookupMemo`s, `UnitSamplerLookupMemo`, `UnitBindingsSnapshot`, `g_fbSlotCache`), new `MobileGL/MG_Impl/Pipe/SlotAllocator.*`, `MobileGL/MG_Pipe/MGPipeHandles.h` (if kinds/reserved bands move), `MobileGL/Config.h` (`PipeLegacyMemos` gating), `MobileGL/MG_Test/SanityTest.cpp` (fixture).

---

## 2. Magma subsystem 4 — `VertexInputStateFactory`, `VaoDrawMemo`, and the frontend VAO's backend pointers

### 2.1 `VertexInputStateFactory`

Header `MobileGL/MG_Backend/DirectVulkan/Renderer/VertexInputStateFactory.h`; class at `:18`, `using HashType = Uint64` at `:20`, `struct BackendVertexInputState` at `:31` (`hash` `:32`, `layoutHash` `:40`), cache `UnorderedMap<HashType, UniquePtr<BackendVertexInputState>> m_cache` at `:123`, `m_frameBoundaryCounter` `:125`, `m_evictionEpoch = ++s_evictionEpochSource` `:141`. Owned by the renderer: `VulkanRenderer.h:889`, constructed `VulkanRenderer.cpp:3197-3198`.

* **Key = a content hash of the VAO, not a pointer.** `ComputeHash` at `VertexInputStateFactory.cpp:15-53`: `XXH64` over `CacheVersion` then, per enabled attribute, `Enabled/Size/Type/Normalized/Stride/Offset/IsInteger/IsLong/IsBgra/Divisor` and — crucially — `const Uint64 bufferKey = attr.Buffer ? attr.Buffer->GetLifetimeId() : 0;` at `:48`. The 10-line comment at `:37-47` records that this used to be the buffer's **heap address** and that the recycled address is what let a transform-feedback capture read a destroyed VAO's slice. `ARCHITECTURE.md:363` calls for exactly this `lifetimeId` to be replaced by the handle's `gen` mixed into every server-side content hash.
* Where the key comes from: the `VertexArrayObject&` is the context's bound VAO — `MGB_CTX->GetBoundVertexArray()` (e.g. `DirectGLES.cpp:2947` on the Espryt side; on Magma it arrives as the `vao` argument of `UploadAndBindVertexBuffers`, `VulkanRenderer.cpp:3581`). `attr.Buffer` is a `SharedPtr<BufferObject>` held by the frontend VAO.
* `GetOrComputeHash` `VertexInputStateFactory.cpp:55-63` — reads/writes the VAO's own hash memo.
* `GetOrCreateVertexInputState(vao)` `:65-86` — VAO state-memo fast path (`:71-76`), else `GetOrCreateVertexInputState(vao, hash)` and then stamps **all three** VAO memos (`:78` state, `:83-84` aux).
* `GetOrCreateVertexInputState(vao, hash)` `:88-…` — `m_cache.find(hash)` `:89`, builds otherwise.
* Eviction: `OnFrameBoundary()` `:298-324` — sweeps every 256 boundaries, retires entries idle > 1024 boundaries, and on **every** erase does `m_evictionEpoch = ++s_evictionEpochSource` (`:318`), which is what invalidates every VAO's `SetBackendStateMemo` pointer. Erasure is deliberately confined to the frame boundary (`:303-307`).

### 2.2 `VaoDrawMemo`

`VulkanRenderer.h:1244-1264`. Fields: `vaoKey` (raw `const VertexArrayObject*`, `:1245`), `vaoLifetimeId` (`:1252`, with the 6-line comment `:1246-1251` saying the pointer alone is not an identity), `contentHash` (`:1255`), `layoutFactsValid` (`:1256`), `layoutHash` (`:1261`), `layoutAuxMasks` (`:1262`), `bindings` (`ResolvedVertexBindings`, `:1263`). Table `m_vaoDrawMemoTable` sized `kVaoDrawMemoSlotCount = 2048` (`:1271-1272`), never rehashed or swept (`:1265-1270`); `m_currentDrawResolvedEntry` `:1279` is a pointer into it valid only within one draw (`:1275-1278`).

`LookupVaoDrawMemo` — `VulkanRenderer.cpp:3540-3579`. **Key = `(address, lifetimeId)`**: the address (`>>4`, Fibonacci-mixed, `:3547-3548`) picks a 2-way slot pair `index` / `index^1`; the match requires `vaoKey == vao && vaoLifetimeId == vao->GetLifetimeId()` (`:3553-3561`); a miss recycles the slot whose `bindings.frameSerial` is older (`:3564-3568`) and resets `contentHash`, `layoutFactsValid`, `bindings.frameSerial/indexFrameSerial/indexBuffer` (`:3569-3577`).

Readers/writers: `VulkanRenderer.cpp:3619-3647` (`UploadAndBindVertexBuffers`: probes the memo before the factory resolve, `:3622` reads the VAO hash memo, `:3624`/`:3640` lookup), `:3649-3652` (stamps `layoutAuxMasks`, `contentHash`, `layoutHash`, `layoutFactsValid`), `:6197-6220` (the `TrySetupDrawFastPath` layout-facts read; note the comment `:6190-6193` that the slot "only ever answers for THIS object" because of the lifetime id).

The related snapshot `SetupDrawSnapshot` (`VulkanRenderer.h:958-…`) already carries the same doubled identities that handles collapse: `programLifetimeId` `:963`, `vao` (raw pointer) `:965` + `vaoLifetimeId` `:971` + `vaoConfigVersion` `:972`, `drawFbo` `:973` + `drawFboLifetimeId` `:978` + `fboVersion` `:979`. `ARCHITECTURE.md:363` names this as one of the 7 re-keys ("~14 探测字段与两个有损求和 → 三个 handle + 两个 server 纪元 + dirty mask").

### 2.3 The backend raw pointers stored in the frontend `VertexArrayObject`

`MobileGL/MG_State/GLState/VertexArrayState/VertexArrayObject.h`:

| API | Lines | Storage |
|---|---|---|
| `GetBackendHashMemo(Uint64&)` | `106-110` | `m_backendHashMemo` `:190`, `m_backendHashMemoVersion` `:191` |
| `SetBackendHashMemo(Uint64)` | `111-114` | same |
| `GetBackendStateMemo(const void*&, Uint64&)` | `121-126` | `m_backendStateMemo` (**`const void*` into the factory's heap entry**) `:192`, `m_backendStateMemoEpoch` `:193`, `m_backendStateMemoVersion` `:194` |
| `SetBackendStateMemo(const void*, Uint64)` | `127-131` | same |
| `GetBackendAuxMemo(Uint64&, Uint64&)` | `140-145` | `m_backendAuxMemo0/1` `:195-196`, `m_backendAuxMemoVersion` `:197` |
| `SetBackendAuxMemo(Uint64, Uint64)` | `146-150` | same |

All six are `const` methods over `mutable` members, all guarded by `m_configVersion` (`:189`, exposed at `:101`, bumped in `VertexArrayObject.cpp:299, 305, 311` and elsewhere). `GetLifetimeId()` `:66`, `m_lifetimeId` `:176`, allocator `VertexArrayObject.cpp:19-21` (atomic, starts at 1 — `:14-17` explains a zero-initialized memo slot must never match a live id).

**Every writer and reader across both backends** (this is the complete set; `grep` over `MobileGL/` finds no others):

* `SetBackendHashMemo` / `GetBackendHashMemo` on the VAO — reader `VertexInputStateFactory.cpp:58`, writer `:60`; readers `VulkanRenderer.cpp:3622`, `6197`, `6214`.
* `SetBackendStateMemo` / `GetBackendStateMemo` — reader `VertexInputStateFactory.cpp:72`, writer `:78`. **No other site.** This is the one `ARCHITECTURE.md:284` says is deleted outright ("`SetBackendStateMemo`（前端 VAO 里存后端堆裸指针）直接删除").
* `SetBackendAuxMemo` — writer `VertexInputStateFactory.cpp:83-84`. **There is no live reader left**: the values were moved into `VaoDrawMemo::layoutHash` / `layoutAuxMasks` (see the comment `VulkanRenderer.h:1257-1259`, "the exact values `GetBackendAuxMemo` used to serve, moved here"). The `Set` is dead weight today; `ARCHITECTURE.md:284` routes `SetBackendHashMemo/AuxMemo` to "server 侧 per-slot 字段".
* **DirectGLES/Espryt uses none of the three.** The Espryt VAO twin tracks its own `m_syncedBufferIdGeneration` etc. (`Managers.h:795-800`) instead.
* Adjacent but out of this slice: `ProgramObject::GetBackendHashMemo(Uint flags, Uint64&)` / `SetBackendHashMemo` — `ProgramObject.h:798-826`, 4-slot flag-keyed ring (`kBackendHashMemoSlotCount = 4` `:1408`, `m_backendHashMemoSlots` `:1414`, round-robin `:824`); its only user is `ProgramFactory.cpp:3446-3448`.

### 2.4 What the re-key must produce

* `VertexInputStateFactory::ComputeHash`'s `bufferKey` becomes the buffer handle's `{slot, gen}` (`ARCHITECTURE.md:363`), so the hash is meaningful across the wire and no frontend `SharedPtr<BufferObject>` is dereferenced in the backend.
* `VaoDrawMemo` keys on `vaoHandle.slot` (direct index, no Fibonacci mix, no 2-way probe) with `vaoHandle.gen` as the validity compare — `vaoKey` and `vaoLifetimeId` both go away.
* `SetBackendStateMemo` deleted; the eviction-epoch dance (`VertexInputStateFactory.cpp:78`, `:318`, `VertexInputStateFactory.h:141`) goes with it, since a slot-indexed table has stable entries.
* `SetBackendAuxMemo` / `SetBackendHashMemo` become server-side per-slot fields; the frontend VAO loses `m_backendHashMemo*`, `m_backendStateMemo*`, `m_backendAuxMemo*` (`VertexArrayObject.h:190-197`).
* Files that change: `VertexInputStateFactory.{h,cpp}`, `VulkanRenderer.{h,cpp}` (memo struct + `LookupVaoDrawMemo` + `UploadAndBindVertexBuffers` + `TrySetupDrawFastPath` + `SetupDrawSnapshot`), `MG_State/GLState/VertexArrayState/VertexArrayObject.h`, and — because `MOBILEGL_PIPE_LEGACY_MEMOS` must keep both arms — `MobileGL/Config.h:356-360`.

---

## 3. The handle scheme the design wants

`MobileGL/MG_Pipe/MGPipeHandles.h` exists and is P0-landed (98 lines total):

* `enum class MGPipeKind : Uint8` — `:24-40`: `None=0, Buffer=1, Texture, Renderbuffer, Framebuffer, Xfb, RenderStateCso, VertexElementsCso, SamplerCso, SamplerViewCso, ShaderCso, Fence, Query, Context, KindCount`. Mirrors `ARCHITECTURE.md:38`.
* `struct MGPipeHandle { Uint32 Slot; Uint32 Gen; }` — `:54-61`, with `operator==` `:58-60`; `static_assert`s for size 8 / align 4 / trivially copyable at `:63-65`.
* Semantics comment `:44-53`: `Gen` bumps **only on slot reuse**, never on respecify; `2^32` recycles to wrap, documented not defended in release, debug allocator asserts. Two generations exist and are strictly separate — `MGPipeHandle::Gen` is the client's "is this still the same GL object", `MGGen` is the server's "did I recast my driver object", and **no MGPipe call may require the client to know `MGGen`** (`ARCHITECTURE.md:44-49`).
* Reserved handles `:67-77`: `kMGPipeNullHandle{0,0}`, `kMGPipeDefaultFramebuffer{0,1}` (explicitly to retire the four `pDefaultFramebufferInfo->defaultFBO` compares), `MGPipeHandleIsNull`. First allocatable slot `kMGPipeFirstAllocatableSlot = 1` at `:81`.
* ShaderCso composite band `:83-97`: `kMGPipeShaderCsoSlotLimit = 1<<20`, `kMGPipeShaderCsoCompositeSlotBase = limit - limit/16`, `MGPipeIsCompositeShaderSlot`, plus a `static_assert` that the band does not swallow ordinary slots.
* **Not yet present anywhere in the tree**: any `SlotAllocator`, any handle table, any use of `MGPipeHandle` outside this header — `grep SlotAllocator` hits only `ARCHITECTURE.md:552` and `ROADMAP.md:18`.

Design points from `docs/Disaggregated/ARCHITECTURE.md`:

* §2.1 `:33-40` — 8-byte POD in a register pair; **client mints, server never returns a handle**, hence zero creation round trips (deviation D1 from gallium); slots dense **per kind** so the server table is an array not a hash map; explicitly unrelated to `IndexGenerator`, whose LIFO name reuse is the problem handles close; GL name survives only as `GlNameForDiag` in `MGPResourceDesc` and never enters a memo key or content hash; `GetLifetimeId()` stays client-side and the client maintains `lifetimeId → slot`.
* §2.3 `:53-63` — which existing backend cache each CSO kind corresponds to; notably `VertexElementsCso` ↔ `VertexInputStateFactory::m_cache`, `SamplerCso` ↔ `BackendSamplerObject`, `Framebuffer`/`Xfb` ↔ `BackendFramebufferObject`. CSOs are content-addressed **client-side** with per-class `ska::flat_hash_map<xxHash, MGPipeHandle>` and LRU caps (render-state 64 / vertex-elements 1024 / sampler 256 / sampler-view 4096; shader follows `ProgramObject` lifetime).
* §9.5 `:363` — the accounting this slice pays into: **11 memos deleted** (the 6 registries' same-address `weak_ptr` + GC, `TwinLookupMemo`×3 + `OwnerEquals`, `UnitSamplerLookupMemo`'s `WeakPtr` test, `SetBackendStateMemo`, `VkTextureManager::TextureIdentity` liveness probe, `ConvertedVertexStreamKey::sourcePin`), 2 moved to client debounce, **7 re-keyed** (incl. `StampSyncedFBO`'s 4-tuple → `ContentHash` + server-private `attachmentRemintEpoch`, and `VertexInputStateFactory::ComputeHash`'s `lifetimeId` → `gen`), 1 unchanged (D18). Two latent bugs already landed separately: `m_xfbCounterSlotByObject` keyed on the raw GL name (`bd2b4158`) and `RenderbufferObject` missing `GetLifetimeId()` (`9c7339b2`).
* §10.1 `:375` — server side is `MG_Remote/Server/PipeObjectTables`: **per-kind slot arrays, not an object graph**; the server holds no buffer copy, no texels, no frontend object graph. `PipeApplier` decodes → updates object tables + `PipeInputs` → calls backend function pointers, with a debug assert that no `SharedPtr` or raw frontend pointer crosses the applier boundary. Directory placeholder for the client half: `ARCHITECTURE.md:552` (`MG_Impl/Pipe/` — Tracker, SlotAllocator, CsoCache, HostResolve, CompositeResolver, all `[P2+]`).
* §13 gate `:501` — beyond the three purity gates there is a debug assertion "every backend memo key is `{slot, gen}`, never a raw frontend pointer", **backed by `HandleRecycleScenario`, which must be RED on at least one backend before the re-key**.
* Coverage plumbing that already anticipates handles: `MobileGL/MG_Pipe/Coverage.def:23-24` defines `kStructuralHandle` ("the row is a SIGNATURE carrying `SharedPtr<MG_State...>`, which becomes an `MGPipeHandle` parameter"), and `Coverage.def:128` maps the inventory delta `handle-ify (wire handle)` to it. `ARCHITECTURE.md:80` counts 167 structural-handle rows out of 477.

---

## 4. Lifetime hooks available today

### 4.1 `GetLifetimeId()` per frontend class

| Class | Declaration | Allocator |
|---|---|---|
| `BufferObject` | `MG_State/GLState/BufferState/BufferObject.h:215` (rationale `:210-214`) | `BufferObject.cpp:22` |
| `FramebufferObject` | `FramebufferState/FramebufferObject.h:159` (rationale `:153-158`) | `FramebufferObject.cpp:20` |
| `ProgramObject` | `ProgramState/ProgramObject.h:1262` | `ProgramObject.cpp:27` |
| `RenderbufferObject` | `RenderbufferState/RenderbufferObject.h:52` (rationale `:46-51`) | `RenderbufferObject.cpp:23` |
| `SamplerObject` | `SamplerState/SamplerObject.h:57`, defined `SamplerObject.cpp:253`; id assigned in the ctor init-list `SamplerObject.cpp:25` | `SamplerObject.cpp:20` |
| `ITextureObject` (pure virtual) | `TextureState/TextureObject.h:83`; override on `TextureObjectBase` `:161`, defined `TextureObject.cpp:306`; ctor `TextureObject.cpp:41` | `TextureObject.cpp:24` |
| `VertexArrayObject` | `VertexArrayState/VertexArrayObject.h:66` (rationale `:57-65`) | `VertexArrayObject.cpp:19-21` |

Each class has its **own** counter starting at 1, so ids are only comparable within a type (stated at `MG_Test/State/ObjectLifetimeIdTest.cpp:52-55`). Consumer example outside the backends: `ProgramPipelineObject.h:101` builds a pipeline signature from stage programs' lifetime ids.

**Gaps for a per-kind slot allocator:** the transform-feedback object is a plain struct in a map and has *no* heap identity — its recycled-name hazard was fixed differently (see `MG_Test/State/TransformFeedbackLifetimeIdTest.cpp:10-23`, D21, commit `bd2b4158`). Query and fence objects have no `GetLifetimeId()` either, though `MGPipeKind` reserves `Xfb`, `Fence`, `Query`.

### 4.2 Deletion paths

Frontend GL entry points → `GLContext::Mark*ForDeletion`:

| GL entry | `MG_Impl` call | `GLContext` | State container |
|---|---|---|---|
| `glDeleteBuffers` | `GL_Buffer.cpp:532` | `Core.cpp:116-140` | `BufferState.cpp:53` |
| `glDeleteVertexArrays` | `GL_VertexArray.cpp:392` | `Core.cpp:167-169` | `VertexArrayState.cpp:78-104` |
| `glDeleteTextures` | `GL_Texture.cpp:3849` | `Core.cpp:279-307` | `TextureState.cpp:118-155` |
| `glDeleteProgram` | `GL_Program.cpp:611` | `Core.cpp:346-348` | `ProgramState.cpp:28` |
| `glDeleteShader` | `GL_Program.cpp:618` | `Core.cpp:350-352` | `ProgramState.cpp:132` |
| `glDeleteFramebuffers` | `GL_Framebuffer.cpp:2261` | `Core.cpp:1188-1190` | `FramebufferState.cpp:59-71` |
| `glDeleteRenderbuffers` | `GL_Framebuffer.cpp:2236` | `Core.cpp:1252-1254` | `RenderbufferState.cpp:51` |
| `glDeleteSamplers` | `GL_Sampler.cpp:329` | `Core.cpp:1213-1225` | `SamplerState.cpp:31` |
| `glDeleteProgramPipelines` | `GL_ProgramPipeline.cpp:97` | `Core.cpp:1399` | — |
| `glDeleteTransformFeedbacks` | `GL_Drawing.cpp:1609` | `Core.cpp:1437` | — |

What deletion actually does (representative bodies): `TextureState::MarkTextureObjectForDeletion` (`TextureState.cpp:118-155`) unbinds from every touched unit's binding slot and image binding, calls `BumpTextureBindGeneration()` (`:142`, with the comment that a cached sampled set may hold the raw pointer), erases from `m_textureObjects` and returns the name to `m_indexGenerator`. `FramebufferState::MarkFramebufferObjectForDeletion` (`FramebufferState.cpp:59-71`) rebinds each slot to FBO 0 and erases. `VertexArrayState::MarkVertexArrayForDeletion` (`VertexArrayState.cpp:78-104`) handles the detached-bound case and nulls the slot.

**Crucially: dropping the container's `SharedPtr` is the *only* signal.** A still-bound object keeps living, and nothing calls into the backend. The backend learns of the death lazily, when the registry's `weak_ptr` is found expired by `Find` (`Managers.h:339-342`) or by a `CollectGarbage` sweep. The `GetOrCreate` comment at `Managers.h:306-314` says this out loud: *"Nothing tells the backend that a texture or renderbuffer was DELETED — the twin, and the driver storage it owns, lives until a collection"*, and quantifies the resulting ~100 CTS cases' worth of dead gigabyte-sized objects held at once.

### 4.3 The one existing explicit death notification

Buffers only: `BufferBackendOps` (`MG_State/GLState/BufferState/BufferObject.h:76-103`) with `void (*OnDestroy)(SharedPtr<BackendBufferResource>&&)` at `:103`; installed via `SetBufferBackendOps` (`BufferObject.h:126`, `BufferObject.cpp:26-28`); fired from `BufferObject`'s destructor path at `BufferObject.cpp:39-41`. The other ops in the same table (`Respecify` `:46`, `SubData` `:55`, `FlushMappedRange` `:64`, `AcquirePersistentMap` `:186/499/546`, `ReadbackFromGpu` `:320`, `ResidentSubData` `:365/397`) are the model an object-death callback would follow. `SetBackendResource` (`BufferObject.cpp:607`) is slated for deletion once the server owns the resource table (`ARCHITECTURE.md:284`).

Secondary "the driver object went away" signals the backends already use, which handles do **not** replace and which must survive: `g_backendContextGeneration` (`Managers.h:64-70`, bumped once per `DestroyEGLContext`), `g_attachmentBackendIdGeneration` (used in `StampSyncedFBO`, `DirectGLES.cpp:1891`), `BufferImpl::g_bufferBackendIdGeneration` (`Managers.h:800`), and `InProcessTeardown()`.

---

## 5. Tests that would catch a mistake

### 5.1 What the docs name vs. what exists

`HandleRecycleScenario` (`ROADMAP.md:18`, `ARCHITECTURE.md:501`) **does not exist yet** — `grep` finds it only in those two documents. The gate is stated as "green after, and red before the re-key on at least one backend", so it must be written as a genuine reproducer first. Integration scenarios live in `MobileGL/MG_IntegrationTest/Scenarios/` (77 files), are gtest fixtures deriving from `MGITest::ScenarioTest` (`Harness/ScenarioFixture.h:117`, with the `Ready()` / GPU-skip contract documented at `:120-135`), and must be added to the source list in `MobileGL/MG_IntegrationTest/CMakeLists.txt` (scenario entries start at `:55`).

Likewise **share groups are not implemented at all**: the only mention in code is `MobileGL/MG_Backend/DirectGLES/Managers.h:1080` ("a texture is owned by exactly one context (share groups are not implemented)"), and `ARCHITECTURE.md:18` confirms v1 has one screen / one context / one flow. A "share-group" test can only be a multi-context / context-recreation test today (§5.3).

### 5.2 Existing unit tests that pin the invariants this slice rewrites

* `MobileGL/MG_Test/State/ObjectLifetimeIdTest.cpp` — the canonical "an address is not an identity" test. `ProbeLifetimeIdAcrossAddressReuse` `:57-…` allocates/frees 64 times through a `void* volatile` sink (`:44`, to defeat new/delete elision) and asserts the id differs at a repeated address; `:109` distinct-id assertion; cases at `:115` (VAO), `:125` (Buffer), `:135`, `:139`. It **skips rather than passes** when the allocator never repeats (`:120-121, :130`). A handle allocator's `gen` needs the analogous test.
* `MobileGL/MG_Test/State/TransformFeedbackLifetimeIdTest.cpp` — the name-recycling analogue (D21); `context.MarkTransformFeedbackObjectForDeletion(name)` at `:72` and `:135`; header comment `:10-23` describes exactly the failure mode a slot table must not reintroduce, plus the "open span vs closed span vs gone" contract a bounded slot table must answer.
* `MobileGL/MG_Test/SanityTest.cpp`:
  * `ScopedDirectGLESTextureBindings` `:153-197` — saves/clears/restores `g_backendTextureObjects` wholesale; a slot-array rewrite must keep this shape or the fixture must be rewritten with it.
  * `:2575-2596` `DirectGLESStateGuards.ScratchFBOTextureDeletionForcesFullScrub` — "attached texture id dies → must scrub and re-attach", the ES-does-not-auto-detach rule.
  * `:2650-2665` — a texture twin whose context died must not delete a foreign recycled name (bumps `g_backendContextGeneration` around the reset).
  * `:2765-2773` and `:2786-2790` — the same for framebuffer and renderbuffer twins; `:2757-2762` also pins that the bind shadow follows ES reverting deleted-framebuffer bindings to 0.
  * `:3020-3062` — the sampled-set staleness test: syncs `resident`, silently swaps `foreign` onto unit 0's binding slot, re-runs the draw's fill (`ScopedPipeVerb draw` `:3029`, `draw.Renew()` `:3046`), and asserts `foreign` got its **own** twin (`:3051-3055`) and that nothing re-specified `resident`'s storage. This is the closest existing analogue of a handle-reuse scenario and it exercises the texture registry `Find` directly.
* `MobileGL/MG_Test/Buffer/BufferTest.cpp:137-171`, `MG_Test/Framebuffer/FramebufferTest.cpp:154-227` — name recycling at the `IndexGenerator` level (`GenBuffers`/`GenFramebuffers`/`GenRenderbuffers` return the recycled name). These are the frontend behaviours the handle allocator must *not* inherit (`MGPipeHandles.h:20-22`).
* `MG_Test/Program/ProgramPipelineCompositeTest.cpp:449-459` — composite program identity is stable across units via `GetLifetimeId()`; relevant to the reserved ShaderCso composite band.
* `MG_Test/Program/XfbFrontendOrderInvarianceTest.cpp:27` — references the recycled-heap-address binding memo as its motivation.

### 5.3 Existing integration scenarios closest to the four named cases

* **Handle reuse / deletion-while-bound**: `Scenarios/XfbCaptureBufferReuseScenario.cpp` (header `:9-27`: one capture buffer reused across spans, with respecify-between-spans and immutable-storage variants, plus a fresh-buffer-per-span negative control) and `Scenarios/CrossFrameBufferScenario.cpp` (header `:9-30`: the shipped stale-slice bug that `VaoDrawMemo`/the EBO memo caused — mutate a buffer after a frame boundary, prove the pixels show the new content).
* **Framebuffer slot**: `Scenarios/CrossFrameBufferScenario.cpp` (draw/read FBO interplay) and, on the unit side, `SanityTest.cpp:2575` scratch-FBO scrub. The read-buffer-shared-FBO defect class the design says handles kill by construction is described at `ARCHITECTURE.md:134`.
* **Pipe machinery negative controls already wired**: `Scenarios/PipeVerifyArmingScenario.cpp` and `Scenarios/PoisonOmissionScenario.cpp` (the latter forks a child process, `:61-69`, to observe a `Fatal{UnmigratedPipeInput}`) — these are the templates for a `HandleRecycleScenario` that must be red before the re-key.
* **VAO-shaped coverage** that the Magma re-key can regress: `Scenarios/VertexArrayEnableDisableScenario.cpp`, `Scenarios/VertexAttribBindingScenario.cpp`, `Scenarios/MultiDrawScenario.cpp`, `Scenarios/ResidentIndexScenario.cpp`, `Scenarios/SampledSetStalenessScenario.cpp`.

### 5.4 Suggested shape of the missing `HandleRecycleScenario`

To be red before the re-key it has to reproduce, through public GL, the ABA the current keys only *paper over*: delete a VAO (or texture/FBO) that a backend memo slot holds, immediately create a replacement that the allocator is very likely to place at the same address **and** give a byte-identical configuration (so the content hash matches too), then draw and assert the pixels come from the new object. The `ObjectLifetimeIdTest` volatile-sink trick (`:44`) and the `CrossFrameBufferScenario` red/green quad readback are the two halves to borrow. Because the current code defends this with `lifetimeId` + `weak_ptr`, the "red before" arm needs those defences disabled — which is exactly what `MOBILEGL_PIPE_LEGACY_MEMOS=0` (`Config.h:356-360`) is for.

---

## 6. Sequencing notes for the implementer

1. `MOBILEGL_PIPE_LEGACY_MEMOS` (`Config.h:360`, default `true`) is a **compile-time** switch by design (`ARCHITECTURE.md:367`): after phase C the `MOBILEGL_PIPE_PUSH` bitmap is *not* a valid A/B any more, because a re-key bug is present in both arms. Both implementations must sit under the same `PipeInputs` interface.
2. The registry's "reference into the map dies on the next call" hazard (`Managers.h:326-333`) has already produced one real defect fix in `SyncTextureObjectToBackend` (`DirectGLES.cpp:1310-1345`). A dense slot array removes the hazard; the by-value copy and second `Find` should be deleted **in the same change**, not left as harmless.
3. `Find` erasing on expiry (`Managers.h:339-342`) means the current code has no read-only lookup. If the new table keeps a `Find` that mutates, the FBO scan at `DirectGLES.cpp:6413` (which iterates while other code may run) needs re-auditing.
4. `EnsureProcessTeardownSentinel()` is called from `GetOrCreate` (`Managers.h:295`) — i.e. the first registry insertion is what arms the atexit guard. If insertion moves to a `SlotAllocator`, the arming site moves with it; the comment at `Managers.h:52-57` explains why a destructor hook is wrong.
5. Purity gate C greps `MG_Backend/` for the token `pGLContext` (`ARCHITECTURE.md:501`, `PipeInputsSwitch.h:15-16`); nothing in this slice may reintroduce it, and the `g_fbSlotCache` rewrite is the one place tempted to.
