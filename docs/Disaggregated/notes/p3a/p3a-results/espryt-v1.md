# P3a package C — Espryt (`espryt-v1`)

Branch `p3a/espryt`, worktree `/home/swung/w7/p3a-espryt`, parent `39722687` (`p3a/contract`, tag). **Not pushed.**
Six commits, oldest first:

| # | hash | message |
|---|---|---|
| e1 | `9ae4ec099dddaef0e6d6a1a4df9f551aa67f6f6e` | `[Refactor] (Espryt): give the buffer resource its own {slot, gen} table instead of hanging it off the frontend object` |
| e2 | `de13c5886cd6d36287a26ecfd644561d2a6f77a7` | `[Refactor] (Espryt): take the buffer ops by handle and payload instead of by frontend object reference` |
| e3 | `855272e5012d35126357297a2a3557bd4026a452` | `[Refactor] (Espryt): answer a readback through the reverse channel and bump the mutation epoch after it, never before` |
| e4 | `469f78b85f8bd3a4bd6bbfe6af90c664c20c5bd9` | `[Refactor] (Espryt): drive the driver VAO from the pushed vertex-elements record and retire the wrapping index-slot version and its identity patch` |
| e5 | `a9a1d0eb156b61cea8aff4847e44eb7d75c6bc8f` | `[Refactor] (Espryt): key the narrowed fp64 vertex stream on the buffer's handle instead of its lifetime id` |
| e6 | `203120fa54a004023a79f05aba72947ee62c0bb2` | `[Test] (Espryt): extend the slot-table suite to the buffer kind and pin that its death crosses as resource_destroy` |

Files touched, all inside C.2's list: `MG_Backend/DirectGLES/{SlotTables.h, Managers.h, Managers.cpp, DirectGLES.cpp}`
and `MG_Test/SanityTest.cpp`. **`MultiDraw.cpp` and `Utils.cpp` were not touched** — nothing in them needed to move
(their element-array restores and the XFB/PBO writeback shapes are D-J's "must not change" rows, and their
`EnsureBufferResource` call sites keep their behaviour through the dispatcher). Nothing outside the list was touched;
in particular `BackendObject_DirectGLES.cpp` was NOT edited even though C.2 step e2 names `:849` as a bring-up site —
see deviation **D3**.

---

## 1. Per-step summary

### e1 — the seventh slot table

* `Managers.h`, `namespace BufferImpl`, all inside `#if MOBILEGL_PIPE_PUSH`:
  * `using BackendBufferResourceTable = BackendSlotTable<BufferObject, GLESBufferResource, MGPipeKind::Buffer>` and
    `extern BackendBufferResourceTable g_backendBufferResources`. The `StateObject` parameter is `BufferObject` only
    because the template names one: **no member that touches it is instantiated** (`Find(StateObject*)`, `HandleOf`,
    `ForEachLive`, `OnFrontendObjectDestroyed` are never called on this table), which is what lets a handle-keyed kind
    live in the same class template. `EnsureProcessTeardownSentinel()` is armed by the table's own
    `GetOrCreate(MGPipeHandle)` — the contract's overload — i.e. at its first insertion, not from a destructor.
  * `ResolveResourceSubsystemArm()` / `ResolveVertexInputSubsystemArm()` plus the two **inline latched**
    `ResourceSubsystemEnabled()` / `VertexInputSubsystemEnabled()` (bit 7 / bit 8). Latched for the same reason
    `EsprytSlotTablesEnabled()` is: the two arms hold `GLESBufferResource` in **different containers** (the frontend
    object's `PipeResource::m_backend` vs this table), so a mid-run flip would strand every resource already built and
    leak the driver ids it owns. Inline for the same reason too — both are consulted per draw.
  * `GetOrCreateBufferResourceForHandle` / `FindBufferResourceForHandle` (neither touches `MGPipeSlots()`), and
    `HandleOfBuffer(const BufferObject*)`, named in place as **monolith glue** for the sites P3a deliberately does not
    migrate.
  * `GLESBufferResource` gains one push-only member, **`const Uint8* hostBytes`**: the client's shadow base as the last
    content-carrying resource call left it. It is what the three-tier flush, the pool reseed and the full re-upload read
    on the handle arm instead of asking a frontend object for `MappedData()`.
* `SlotTables.h` gains **`BackendPtr ReleaseByHandle(MGPipeHandle)`** — the death half of the contract's
  `GetOrCreate(MGPipeHandle)`, for a kind whose announcement is its own destroy call. It hands the twin *out* rather
  than destroying it in place, because the caller still has to decide pool / delete / defer, and every one of those has
  to be reached with the entry already retired. It frees no slot: the client does that after the call returns (D-L).

### e2 — the nine ops by handle

* Nine `Ops_H_*` bodies + seven `*Tracked` wrappers duplicated for the new table, **including
  `Ops_H_MapPersistentTracked`'s bump-even-on-decline**. `Create` and `UnmapPersistent` get no wrapper (the legacy table
  had seven ops; `Create` defines nothing and `UnmapPersistent` is emitted by nothing).
* `const MG_Pipe::MGPipeResourceOps g_glesResourceOps` registered from `RegisterBufferBackendOps()` and cleared in
  `UnregisterBufferBackendOps()`, **unconditionally**, exactly as `BufferBackendOps` is — the subsystem bit is the
  frontend's dispatch predicate, so bit 7 clear registers a table nobody calls and the A/B stays a configuration
  question rather than a bring-up-order one.
* Applier reads, all through three file-local accessors: `ResourceRecordOf(res)` (Live + Gen checked),
  `ResourceSerialOf(res)`, `ResourceWidthOf(res)`.
* **Shared bodies, one per helper, so the two arms cannot drift**: `RespecifyStorageWith(resource, size, usage,
  initialData, syncedSerial)`, `UploadRangeFrom(resource, hostBase, start, end)`,
  `FlushPendingRangesFrom(resource, hostBase, frontendSize)` (the whole three-tier ladder, unchanged),
  `DrainResidentWritesNow(resource)` and `StorageMatchesSize(resource, size)`. The object-shaped names
  (`RespecifyStorageNow`, `UploadRangeNow`, `FlushPendingRangesNow`, `StorageMatches`, the two-argument
  `DrainResidentWritesNow`) survive as thin forwarders. **The whole block is `#if MOBILEGL_PIPE_PUSH` / `#else`**: the
  pull build compiles the pre-P3a text verbatim — see deviation **D1**.
* `EnsureBufferResourceForHandle(bufferObject, res)` beside the legacy `EnsureBufferResource`, dispatched from it.
  A **null handle on the handle arm is a loud `MGLOG_E_ONCE` and a null return, never a fall-back to the legacy arm**:
  quietly twinning off the frontend object would hide a missing emission behind a working picture, which is what the
  subsystem A/B exists to expose. (This is what makes the default-mask arm fail visibly on this tree — §4.)
* `IsBufferDrawCleanByHandle(res, resource)` beside `IsBufferDrawClean`, dispatched from it. **The five reads become
  four applier reads with identical semantics**: identity → the slot table's twin at this handle (not
  `GetBackendResource()`); size → `record.Desc.Width` (not `GetSize()`); freshness → `record.Serial` vs
  `syncedChangeSerial` (not `GetChangeSerial()`); map state → `record.HasLiveHostWrites` (not `IsMapped()`).
  `HasLiveHostWrites` is pinned false by a `MOBILEGL_PIPE_VERIFY`-only `MOBILEGL_ASSERT`, so P5 cannot land a silent
  semantic change under it.

### e3 — the reverse channel

* `Ops_H_Readback` answers through `gMGPipeCallbacks.OnBufferWriteback(res, offset, {Offset=(address), Size,
  Seg=kMGHostSpanSegNone})` instead of `bufferObject.WritebackFromBackend`, in the fixed order **writeback → unmap →
  serial stamp → `BumpBufferMutationEpoch()`** (the bump is in `Ops_H_ReadbackTracked`, i.e. strictly after the other
  three; the ordering rule and why it must never move earlier are written into the function). A missing reverse channel
  is an `MGLOG_E_ONCE`, not a silent drop.
* `BufferImpl::MarkBufferGpuWritten(bufferObject)` replaces the three `MarkGpuWritten` sites
  (`DirectGLES.cpp:500 / :544 / :2012`). On the handle arm it announces `OnGpuWritten(res, 1, &{0,
  kMGPipeWholeBuffer})`; on the legacy arm it calls `MarkGpuWritten` as before. Each call site is
  `#if MOBILEGL_PIPE_PUSH / #else` so the pull build's text is unchanged.
  **Integration point for package `client`:** the whole-resource announcement is spelled as **one range of
  `kMGPipeWholeBuffer`**, deliberately not as "zero ranges" — a zero count is the shape a fully *narrowed*
  announcement will legitimately have once P8/P9 build the client's conservative set, and the two must not be the same
  record. The client's `OnGpuWritten` must read it that way.

### e4 — the VAO half

* `BackendVertexArrayObject::SyncToBackendFromApplier()` beside the legacy `SyncToBackend` body, dispatched at the top
  of `SyncToBackend`. It takes **no argument** and reads no frontend type.
* **Gate, re-keyed** (`Managers.h`, new members all `#if MOBILEGL_PIPE_PUSH`):

  | retired (now `#if MOBILEGL_PIPE_LEGACY_MEMOS`) | replaced by |
  |---|---|
  | `m_hasSyncedConfigVersion` + `m_syncedConfigVersion` | `m_hasSyncedElements` + `m_syncedElementsHandle` + `m_syncedElementsSerial` vs `BoundVertexElements` + `rec.ContentSerial` |
  | `Array<VertexAttributeVersion,32> m_syncedAttributeVersions` | **deleted** — the applier's `Attributes[]` *is* what was last pushed, so the per-attribute compare has nothing left to prove; a moved `ContentSerial` re-emits the whole walk |
  | `m_syncedIndexBufferVersion` (wrapping `Uint16`) + `m_syncedIndexBufferObject` (raw identity patch) | `m_syncedIndexSerial` vs `IndexBufferSerial` — one monotone `Uint64`, no wrap, nothing to patch. **This is the Track H re-key.** |
  | `baseInstanceDirty` as a gate input | gone: a base-instance change is a `ContentHash` input, so it moves `VertexBuffersSerial`. `m_syncedFetchBaseInstance` **survives** as the record of what was last *emitted*. |
  | — | **`m_syncedVertexBuffersSerial` (new, beyond D-G4's table)** vs `VertexBuffersSerial`; see deviation **D5** |
  | `m_syncedBufferIdGeneration` | **unchanged**, server-local, still the only thing that catches a driver-id re-mint no counter on either side moves |
  | `m_hasConvertedFloat64Attribute` | **unchanged** — a narrowed stream is derived from buffer *content*, which no serial covers |

* Per-attribute walk from `rec->Attributes[i]` and `st.VertexBuffers[]`, with
  `VertexBufferForBindingIndex(st, bindingIndex)` (positional slot first, window scan behind it) and
  `BaseInstanceByteShiftWire(stride, divisor, baseInstance)` — the divisor comes from `MGPVertexBuffer::Divisor`,
  because the wire attribute deliberately does not carry it. **Every branch survives**: the enable/disable block, the
  fp64 narrowing with its Adreno disable, the zero-stride binding-API path
  (`SyncZeroStrideAttributeByHandle`), `BindAttributeBufferByHandle` (driver id from the slot table),
  the BGRA refusal probe, `glVertexAttribPointer`/`IPointer` at the shifted `fetchOffset`, `glVertexAttribDivisor`.
* `VertexArrayImpl::BackendUsesNativeBaseInstance()` (reads `g_GLESCapabilities.SupportsBaseInstance`) is applied to
  `st.VertexFetchBaseInstance` in the applier arm. It is **idempotent** with whatever `wire` resolves, so the answer is
  the same whichever side resolves first.
* `ResolvedDrawBuffers` **re-keyed** (all new fields push-only): `Entry` gains `MGPipeHandle handle`; the struct gains
  `elementsHandle`, `elementsSerial`, `buffersSerial` and `iboHandle`. `SyncNeccessaryBuffers` grows a push branch
  (`SyncVaoAttributeBuffersByHandle`) whose memo validity is `{BoundVertexElements, ContentSerial, VertexBuffersSerial}`
  instead of the frontend configuration version, and whose clean probe is `IsBufferDrawCleanByHandle(entry.handle, …)`.
  The IBO half re-keys `iboFrontend` → `iboHandle` the same way. `vboCleanEpoch`/`iboCleanEpoch` against
  `CurrentBufferMutationEpoch()` and the hit/miss/repair structure are unchanged.
* `g_pendingFetchBaseInstance`, `SetPendingFetchBaseInstance`, `GetPendingFetchBaseInstance`, `ScopedFetchBaseInstance`
  and the three scopes at `DirectGLES.cpp:5150 / :5187 / :5239` are **removed from the handle arm** and kept compiled
  under `MOBILEGL_PIPE_LEGACY_MEMOS` — see deviation **D2**. `EmulatedFetchBaseInstance` / `UseNativeBaseInstance`
  survive; the applier arm's copy of the decision is `BackendUsesNativeBaseInstance()`.

### e5 — the fp64 stream key, and the D-L comment

* `ConvertedFloat64Stream::sourceLifetimeId` (**`ARCHITECTURE.md`'s "`ConvertedVertexStreamKey` 的 `sourcePin`"**) moves
  under `#if MOBILEGL_PIPE_LEGACY_MEMOS`; the push arm gains `MGPipeHandle sourceHandle`. `sourceChangeSerial` is the
  applier's server-owned `Serial` on the handle arm. `sourceOffset` / `sourceStride` / `componentCount` /
  `elementCount` are unchanged, and the memo is still **never trusted for a persistently mapped buffer**.
* `SyncFloat64AttributeAsFloat32ByHandle` enables the memo on that key (e4 landed the function with the memo
  deliberately disabled so the key change is this commit's whole subject).
* `Managers.cpp`'s `default:` arm comment now reads *"Buffer death crosses as ResourceDestroy (P3a)"*, with the fixed
  order (`resource_destroy` → `MGPipeSlots().Free`) and the reason written in place.

### e6 — SanityTest

No new test name (G2's name sets stay identical, G14 has nothing to compare against). Two existing cases grew arms:

* `DirectGLESSlotTable.EveryReKeyedObjectClassAnnouncesItsOwnDeath` now also builds a `BufferObject` and asserts it
  raises **no** `StateObjectDeathNotice` — the falsifiable form of D-L. Both firing would free the slot twice.
* `DirectGLESSlotTable.EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm` now drives the seventh table through
  the family's own walk: client mints → `GetOrCreate(handle)` → `FindByHandle` → `ReleaseByHandle` (resource_destroy)
  → `MGPipeSlots().Free` → re-acquire, asserting the slot comes back, `Gen` moves, the stale handle resolves to
  nothing, and the successor does **not** inherit the predecessor's storage.
* `PixelPackBindingCacheSkipsRedundantBindsAndRestsAtZero` untouched and passing.

---

## 2. Track H census — every deleted/retired member and re-keyed memo

Deleted from the handle arm (kept compiled under `MOBILEGL_PIPE_LEGACY_MEMOS`, which a pull build forces ON, so
`sizeof` moves nowhere and G1 holds):

1. `BackendVertexArrayObject::m_hasSyncedConfigVersion`
2. `BackendVertexArrayObject::m_syncedConfigVersion`
3. `BackendVertexArrayObject::m_syncedAttributeVersions` (`Array<VertexAttributeVersion, 32>`) — **the largest single
   deletion of the phase**
4. `BackendVertexArrayObject::m_syncedIndexBufferVersion` (wrapping `Uint16`)
5. `BackendVertexArrayObject::m_syncedIndexBufferObject` (raw frontend pointer, the wrap-hole identity patch)
6. `ConvertedFloat64Stream::sourceLifetimeId` — **`ConvertedVertexStreamKey`'s `sourcePin`**
7. `VertexArrayImpl::g_pendingFetchBaseInstance` + `SetPendingFetchBaseInstance` + `GetPendingFetchBaseInstance` +
   `ScopedFetchBaseInstance` (the ambient process global and its three draw-site scopes)
8. `GLESBufferResource`'s residence in `PipeResource::m_backend` — the resource moves into the seventh slot table.
   `SetBackendResource` / `m_backend` / `BackendBufferResource` are **not deleted** (D-K), they simply stop being
   written under push.

Re-keyed memos:

| memo | was | is |
|---|---|---|
| `BackendVertexArrayObject`'s sync gate | config version + 32 per-attribute version triples | `{elementsHandle, elementsSerial}` + `VertexBuffersSerial` |
| the twin's index memo | wrapping `Uint16` + raw `BufferObject*` | `m_syncedIndexSerial` (`Uint64`) |
| `ResolvedDrawBuffers::configVersion` | frontend VAO config version | `{elementsHandle, elementsSerial, buffersSerial}` |
| `ResolvedDrawBuffers::Entry::frontend` | raw `BufferObject*` | `+ MGPipeHandle handle` (the probe's identity) |
| `ResolvedDrawBuffers::iboFrontend` | raw `BufferObject*` | `+ MGPipeHandle iboHandle` |
| `ConvertedFloat64Stream` | frontend lifetime id + frontend change serial | buffer `{slot, gen}` + applier `Serial` |
| `GLESBufferResource::syncedChangeSerial` | mirrors `BufferObject::GetChangeSerial()` | mirrors `MGPipeResourceRecord::Serial` (server-owned `MGGen`) |

Unchanged on purpose (D-J / D-A4): `g_bufferBackendIdGeneration`, `m_hasConvertedFloat64Attribute`,
`m_syncedFetchBaseInstance` (as the *emitted* record), the `BackendSlotTable` single-entry `m_memoLifetimeId` /
`m_memoHandle` front memo, `TempBufferTarget` and its redundant-bind cache.

---

## 3. Evidence

### 3.1 `AcquirePersistentMap` — the two permitted changes only (D-E)

Statement-level diff of `Ops_AcquirePersistentMap@44c2b5cf` against `Ops_H_MapPersistent@HEAD` (comments and blank
lines stripped; nothing else in the function differs):

```
-void* Ops_AcquirePersistentMap(BufferObject& bufferObject) {
+void* Ops_H_MapPersistent(MG_Pipe::MGPipeHandle res, Uint64 size, const void* seedBytes) {
-const SizeT size = bufferObject.GetSize();
-auto* resource = static_cast<GLESBufferResource*>(bufferObject.GetBackendResource().get());
-if (!resource) { auto created = MakeShared<GLESBufferResource>(); resource = created.get();
-                 bufferObject.SetBackendResource(std::move(created)); }
+auto* resource = GetOrCreateBufferResourceForHandle(res);
+if (!resource) return nullptr;
-const void* initial = bufferObject.MappedData();
+const void* initial = seedBytes;
-MGLOG_E_ONCE("Ops_AcquirePersistentMap: ...")
+MGLOG_E_ONCE("Ops_H_MapPersistent: ...")
-resource->storageSize = size;
+resource->storageSize = static_cast<SizeT>(size);
-resource->syncedChangeSerial = bufferObject.GetChangeSerial();
+resource->syncedChangeSerial = ResourceSerialOf(res);
```

That is: **(1)** the signature, **(2)** the three reads through the object (`GetSize()` → `size`, `MappedData()` →
`seedBytes`, `GetBackendResource()`/`SetBackendResource()` → the slot table's `GetOrCreate(res)`), plus the two
consequences that follow from them mechanically — the `Uint64 → SizeT` narrowing of the parameter and the
`GetChangeSerial()` → server-owned `Serial` substitution D-A2's table names for every hook. The four-way capability
gate, the stale-generation wipe **before** the new stamp, the idempotency hit, the fresh-id sequence with
`++g_bufferBackendIdGeneration`, the locally defined `0x0040/0x0080/0x0100` constants, `immutableStorage = true` set as
soon as the store exists, the `MGLOG_E_ONCE` decline and every success stamp are byte-identical. The legacy
`Ops_AcquirePersistentMap` itself is **untouched**.

### 3.2 G5 — the nine functions are byte-identical

Brace-extracted from `Managers.cpp` at `44c2b5cf` and at `HEAD`, sha256 (first 16 hex) of each body:

```
OK  IsPoolable                     01200228771b2bb3 == 01200228771b2bb3
OK  EnrollIntoPool                 7a55c6a95d1970db == 7a55c6a95d1970db
OK  AcquireFromPool                f8dd2b3c22efe640 == f8dd2b3c22efe640
OK  TrimBufferPool                 cb607a511958cf8f == cb607a511958cf8f
OK  ClearBufferPool                068fff7ad1e01e8b == 068fff7ad1e01e8b
OK  ProcessDeferredBufferReleases  497b6b793b3451cd == 497b6b793b3451cd
OK  CreateRingStorage              b648500e8fe072f0 == b648500e8fe072f0
OK  RingAvailable                  f62a79ad5ee22bc6 == f62a79ad5ee22bc6
OK  RingAllocate                   efc1c1b7dee277de == efc1c1b7dee277de
```

`git diff 44c2b5cf -- MG_Backend/DirectGLES/Managers.cpp` contains no hunk inside any of the nine; the only lines
mentioning them are new forward declarations inside the push arm and new *calls* (`AcquireFromPool` from
`EnsureBufferResourceForHandle`, `RingAllocate`/`RingAvailable` from the shared flush/drain cores).
`scripts/p3a_untouched_regions.sh` is package D's and does not exist yet; the check above is its stand-in and the
integrator should re-run the real script after D lands.

---

## 4. Verification transcript

`CCACHE_BASEDIR=/home/swung/w7`, all in `/home/swung/w7/p3a-espryt`.

### 4.1 Per-commit: pull symbol identity, push build, unit

`python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so
--threshold 0 --fail-on-symbol-set-change`

| commit | symbol report | build-push | `ctest -L unit` build-linux | `ctest -L unit` build-push |
|---|---|---|---|---|
| e1 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | run, green | run, green |
| e2 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | `100% tests passed, 0 failed out of 1571` | `100% … 1571` |
| e3 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | `100% … 1571` | `100% … 1571` |
| e4 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | `100% … 1571` | `100% … 1571` |
| e5 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | `100% … 1571` | `100% … 1571` |
| e6 | `0 added, 0 removed, 0 resized, 0 renamed` | rc 0 | `100% … 1571` | `100% … 1571` |

`.text 10792739 -> 10792739 (+0, +0.000%)`, `Total 17209979 -> 17209979 (+0)` at every one of the six.
**Honesty note:** at e1 the two unit runs were executed but their summary lines were cut off by a `tail` in the run
script; every later commit prints the summary and e1's tree is a strict subset of e2's. Everything from e2 on is
quoted verbatim.

**An intermediate e2 state resized five pull symbols and was fixed before the commit.** The first cut of the shared
helper cores rewrote `RespecifyStorageNow`, `UploadRangeNow`, `FlushPendingRangesNow` and `DrainResidentWritesNow` in
place, which is pull-visible: `1 added, 1 removed, 5 resized` (`RespecifyStorageNow +16`, `FlushPendingRangesNow +14`,
`UploadRangeNow +14`, `Ops_SubDataTracked −13`, `Ops_FlushMappedRangeTracked −12`; the add/remove pair was
`DrainResidentWritesNow` losing its unused parameter). The fix is deviation **D1**.

### 4.2 Purity and shape

```
grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'      -> empty
grep -n  'MG_State' MobileGL/MG_Pipe/PipeApply.h               -> no hit
```

`grep -n 'g_pendingFetchBaseInstance\|ScopedFetchBaseInstance' MG_Backend/DirectGLES/*.{h,cpp}` is **not empty**, and
every remaining hit is inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm (`Managers.h:1159-1172`, `Managers.cpp:3320-3333` and
the legacy `SyncToBackend` body, `DirectGLES.cpp:5271-5276`'s macro). Same for
`m_syncedConfigVersion|m_syncedAttributeVersions|m_syncedIndexBufferObject|m_hasSyncedConfigVersion|m_syncedIndexBufferVersion`
(`Managers.h:1079-1100`, one `#if MOBILEGL_PIPE_LEGACY_MEMOS … #endif` block; the only other hits are the prose in the
replacement block's comment). See deviation **D2**.

`MOBILEGL_PIPE_LEGACY_MEMOS=0` push build compiles: `clang++ -fsyntax-only` with build-push's own command line and the
macro flipped, on both `Managers.cpp` and `DirectGLES.cpp` — **rc 0 for both**. (No standard build directory configures
that combination, so this is the only place it is checked.)

`ctest -N` name sets: build-linux 2487, build-push 2487, `diff` empty. Against `~/w7/p3a-before-ctest-names.txt`:
0 removed, 5 added — and all five are **the contract's** (`PipeCatalogue.*` ×3, `ResourceEmit.*`,
`VertexInputEmit.*`). This package adds no test name and removes none (G14).

### 4.3 The two legacy integration arms (both green)

```
MOBILEGL_PIPE_PUSH=0    ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
    rc 0   100% tests passed, 0 tests failed out of 461
MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
    rc 0   100% tests passed, 0 tests failed out of 461
```

**Both were re-run against a freshly built HEAD `build-push`.** An earlier pass of these lanes was taken against a
`build-push` left over from a base-commit excursion; the numbers above replace it. (`ROADMAP.md:7`'s stale-binary rule,
the hard way.)

Two entries — `UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn` and
`PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn` — failed in one narrowly filtered
`build-linux` run and pass in every full lane. `build-linux/libMobileGL.so` is **symbol- and byte-identical to
`$BASE`**, so they cannot be this package's; they look filter- or concurrency-sensitive and are recorded here rather
than chased.

### 4.4 The default-mask arm — expected red, and why

```
MOBILEGL_PIPE_PUSH=0x1ff ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES
    rc 8   56% tests passed, 202 tests failed out of 461
```

The same lane at `39722687` (base, `build-push` rebuilt at it) is `100% tests passed, 0 failed out of 461`, so the 202
are this package's switch-over meeting an applier that is still a stub. **Every one of them is explained by a missing
record**, and the two diagnostics this package emits say so by name. Running one failing case directly
(`MOBILEGL_LOG_FILE_PATH=… MOBILEGL_PIPE_PUSH=0x1ff MobileGLIntegrationTest
--gtest_filter='*OrientationScenario.OffscreenPassRendersUpright*'`, rc 1):

```
[ERROR]: MGPipe: buffer 1 has no resource handle - the resource family is switched over but
         nothing emitted resource_create for it
[ERROR]: MGPipe: no vertex-elements record is bound, so the driver VAO cannot be configured -
         nothing emitted create_vertex_elements/bind_vertex_elements
```

i.e. `MGPipeSlots()` holds no `Buffer` slot (nothing calls the client's `ResourceCreate` emitter) and
`MGPipeApplierState::{VertexElementsCsos, BoundVertexElements}` are empty (`MGPipeApplyCreateVertexElements` /
`…BindVertexElements` are no-op stubs at `c0`). Failures by scenario, top of the list: `ImageTargetKind` 24,
`VertexAttribBinding` 13, `Orientation` 11, `DoublePrecision` 8, `SsboDeclarationForm` 7, `TessellationXfbCapture` 6,
`DrawParameters` 6, `XfbRepeatedCapture` 5, `XfbAfterClipDistance` 5, `ProgramPipeline` 5, `PointSizeDemotion` 5+3,
`XfbCaptureBufferReuse` 4, `UnwrittenPositionOutput` 4, `UnboundImageDescriptor` 4, `PrimitiveRestart` 4, … — that is
"every scenario that draws or touches a buffer", which is the expected blast radius of a VAO that cannot be configured
and an `EnsureBufferResource` that returns null. **No failure was found that is not explained by a missing record**, so
nothing here is treated as a defect. `MGB_CTX` was **not** re-read in the handle arm to paper over it.

---

## 5. Deviations, each with its reason

**D1 — the shared helper cores are `#if MOBILEGL_PIPE_PUSH` / `#else`, and the pull build compiles the pre-P3a text
verbatim.** Refactoring `RespecifyStorageNow` / `UploadRangeNow` / `FlushPendingRangesNow` /
`DrainResidentWritesNow` / `StorageMatches` in place is **pull-visible** and resized five symbols (§4.1). G1 admits an
empty resize set, so the pull arm keeps the original text and the push arm carries the parameterised cores plus thin
forwarders. Consequence, and it is the good half: **inside a push build there is exactly one three-tier flush, one
respecify and one range upload**, shared by the legacy and the handle arm, so E's "the tier silently changes" risk has
no second copy to drift into. The `#else` copy is frozen by G1 itself and retires with the pull path at P13.

**D2 — `g_pendingFetchBaseInstance` and friends are retired, not deleted.** C.2's verification line asks for
`grep -n 'g_pendingFetchBaseInstance\|ScopedFetchBaseInstance' … # empty`. That is not achievable together with G1:
`SetPendingFetchBaseInstance` and `GetPendingFetchBaseInstance` are declared in `Managers.h` and defined in
`Managers.cpp`, so they are **real symbols in the pull build** and deleting them is `2 removed` — a straight G1 break.
It would also leave the legacy arm (bit 8 clear) with no base-instance emulation at all, which `ARCHITECTURE.md:367`
requires to keep working through P3a/P4a. They are therefore inside `#if MOBILEGL_PIPE_LEGACY_MEMOS`, exactly like the
five retired twin memos, and the handle arm neither declares nor reads them. The grep's rule should read as it does for
the twin memos two lines below it: *every remaining hit must be inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm.*

**D3 — `MGPipeSetResourceOps` is installed from `RegisterBufferBackendOps()`, not from the two bring-up call sites.**
C.2 step e2 names `BackendObject_DirectGLES.cpp:849` and `DirectGLES.cpp:10402`, but `BackendObject_DirectGLES.cpp` is
**not in C.2's file list** and hard rule (4) forbids touching it. Both named sites call
`BufferImpl::RegisterBufferBackendOps()`, so installing there covers both with one edit, in the file that owns the
table, and pairs it with `UnregisterBufferBackendOps()` exactly as `BufferBackendOps` is paired today.

**D4 — e2 necessarily carries the `OnBufferWriteback` substitution; e3 carries `OnGpuWritten` and the ordering.**
A handle-shaped readback has no `BufferObject` to call `WritebackFromBackend` on, so the reverse-channel call cannot be
deferred past the commit that introduces the handle-shaped op (D-A2's own table says the substitution is part of the
hook conversion). e3 is therefore the three `MarkGpuWritten` sites, the written-down epoch-order rule and the
missing-channel diagnostic. Both commits are individually coherent; the split is the brief's, one step over.

**D5 — the twin gained `m_syncedVertexBuffersSerial`, which D-G4's table does not list.** `set_index_buffer` and
`set_vertex_buffers` are independent calls, and `set_vertex_buffers` carries the buffer identities, offsets and
divisors this twin **bakes into the driver VAO**. A set that moved while the elements CSO did not (a rebind of the same
format onto a different buffer, or a base-instance-only change — which is a `ContentHash` input precisely so it is not
suppressed) has to re-emit them. Keying only on `ContentSerial` would drop exactly the case `baseInstanceDirty` used to
catch. It is a third monotone server-owned `Uint64`, no wrap and no identity patch, so it costs the census nothing.

**D6 — `EnsureBufferResource` keeps its `SharedPtr<BufferObject>` parameter on the handle arm.** C.2's "must not
break" list says the sixteen call sites keep their behaviour "taking a handle where they took a `SharedPtr`". Full
handle shape is not reachable in P3a: `BufferObject::SyncPersistentMappedRange()` at the old `Managers.cpp:1700` is one
of the eleven sites **D-N explicitly keeps where they are** (the per-site attribution table is P8's to execute). So
`EnsureBufferResourceForHandle(bufferObject, res)` is handle-shaped in every respect except that one call, which is
named in a comment at the call. The sixteen call sites are unchanged and dispatch inside `EnsureBufferResource`. No
`MG_State` type appears in any *new* handle-arm signature (`MGPipeResourceOps`' nine, `SyncToBackendFromApplier`,
`SyncFloat64AttributeAsFloat32ByHandle`, `IsBufferDrawCleanByHandle`, `Ops_H_*`, `BindAttributeBufferByHandle`,
`SyncZeroStrideAttributeByHandle`, `ResourceWidthForHandle`, `ResourceSerialForHandle`,
`GetOrCreateBufferResourceForHandle`, `FindBufferResourceForHandle`).

**D7 — one behavioural difference between the arms, and it is confined to fp64 vertex arrays.** The legacy
`SyncFloat64AttributeAsFloat32` opens with `bufferObject->SyncGpuWrites()` (the other D-N-retained site,
`Managers.cpp:2774`). A handle has no inverse map to a frontend object — by design, the server has none — so
`SyncFloat64AttributeAsFloat32ByHandle` cannot make that call, and a `GL_DOUBLE` array whose **source buffer was
written by a shader and not yet pulled back** narrows stale bytes on the handle arm. It is written into the function.
P8 closes it by moving the pull to the client. `DoublePrecisionScenario` is the gate that would see it, and it must be
re-run after the rebase (§6).

**D8 — the whole-resource `OnGpuWritten` announcement is one `kMGPipeWholeBuffer` range, not zero ranges.** See e3.
This is a contract point the client half has to agree with and is called out for the integrator.

**D9 — `Ops_H_UnmapPersistent` is a no-op.** P3a emits it from nowhere; the donation is permanent for the life of the
store and is ended by the respecify / destroy paths, which already retire the immutable id. Giving it a body that
unmapped a live coherent store is the one thing D-B4 forbids. The entry exists so the transport has both halves.

**D10 — `Ops_H_ResidentSubData` does not record `hostBytes`.** Its `bytes` are the application's staging store, valid
for the duration of the call only, which is exactly why they are copied. Recording them as the shadow base would leave
a dangling pointer for the next drain. (The three content-carrying calls that *do* record it —
respecify / sub-data / flush-range — all carry the client's own shadow.)

---

## 6. What the post-rebase verification must cover

After `git rebase feat/disaggregated` (contract + wire + client) in `/home/swung/w7/p3a-espryt`, in this order:

1. **Rebuild all three build dirs before judging anything.** A verdict from a stale `build-push` cost this package a
   round already (§4.3).
2. `symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0
   --fail-on-symbol-set-change --fail-on-added-bytes 0` → `0/0/0/0`.
3. `ctest -L unit` in build-linux, build-push and build-verify.
4. `ctest -N` name sets: build-linux == build-push; nothing removed against `~/w7/p3a-before-ctest-names.txt`.
5. **The default-mask arm, which is the whole point of the rebase**:
   `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 8 -R DirectGLES` (no env pin, i.e. `0x1ff`)
   must be green. Any residual failure is now a real defect, not a missing record: the two diagnostics
   (`has no resource handle`, `no vertex-elements record is bound`) name which record is still missing, and
   `MOBILEGL_LOG_FILE_PATH=… ./build-push/MobileGL/MG_IntegrationTest/MobileGLIntegrationTest --gtest_filter=…` is how
   to read them.
6. Both legacy arms again: `MOBILEGL_PIPE_PUSH=0` and `MOBILEGL_PIPE_PUSH=0x7f`, same lane, both green.
7. **The buffer/VAO family by name** (C.2's last regex), under the default mask:
   `-R 'LargeArenaAdoption|StorageBufferRegrow|VertexAttribBinding|MultiDraw|PrimitiveRestart|CrossFrameBuffer|ResidentIndex|BufferTexture|AtomicCounter|XfbCaptureBufferReuse|PackedWordReadback|DoublePrecision|VertexArrayEnableDisable|DrawParameters'`.
   Specifically watch: **`DoublePrecisionScenario`** (D7's stale-narrowing gap),
   **`VertexAttribBindingScenario.BaseInstanceMovesTheInstancedArraysStartElement` / `…LeavesPerVertexArraysWhereTheyWere`**
   and all eight `DrawParametersScenario` cases (the retired ambient global, D2/D-H2),
   **`PrimitiveRestartScenario` + `ResidentIndexScenario` + `MultiDrawScenario`** (the two element-array restore scopes
   against `m_syncedIndexSerial`), **`CrossFrameBufferScenario` + `StreamedArenaScenario`** (the Mali WAR-stall
   queue-only `SubData` and `FlushPendingRangesFrom`'s tier ladder), **`LargeArenaAdoptionScenario` +
   `StorageBufferRegrowScenario`** (`Ops_H_MapPersistent`, `mpr`, and
   `InvalidateIndexedBufferBindingShadowsForId` on an extent move), **`AtomicCounterScenario` +
   `BufferTextureScenario` + `PackedWordReadbackScenario` + `XfbCaptureBufferReuseScenario`** (the reverse channel and
   the epoch order).
8. `MOBILEGL_ESPRYT_DISABLE_INVALIDATE_FLUSH=1 ctest --test-dir build-push -L integration-gpu -j 8` — D.3's named
   step. This package rewrote that kill switch's caller.
9. `ctest --test-dir build-verify -L integration-verify` and the verify retrace sweep: `grep -l 'Fatal{'` empty,
   `grep -L 'MGPipe verify:'` empty. A `Fatal{ProtocolCorruption}` here is a malformed `CreateVertexElements` blob or
   an out-of-range `MGPSubData` box.
10. `ctest --test-dir build-verify -R HandleRecycle` (G8), including package D's new buffer arm.
11. **The retrace sweep**: `python3 ~/w7/retrace_gate.py --tree ~/w7/p3a-espryt --lib build-push/libMobileGL.so --out
    ~/w7/retrace-out/p3a-espryt -j 4`, and the named `--only` set of G3b. The two `coherent_as_flush: true` Create
    fixtures must take the same buffer path under push as under pull.
12. `bash scripts/p3a_untouched_regions.sh 44c2b5cf HEAD` once package D lands it (§3.2 is the stand-in), and
    `--self-test`.
13. Re-check `grep -rc 'pGLContext' MobileGL/MG_Backend | grep -v ':0$'` empty and the
    `MOBILEGL_PIPE_LEGACY_MEMOS=0` syntax check of `Managers.cpp` + `DirectGLES.cpp` (§4.2) — the rebase brings in the
    client's headers and can reopen it.
14. Two contract points to confirm against the merged `client`: the `OnGpuWritten` whole-resource range shape (D8) and
    that `MGPVertexBuffer::BindingIndex` is the attribute index for every entry (D-H3), which
    `VertexBufferForBindingIndex` assumes on its fast path.

Intermediate logs this package created live in the WSL `/tmp` of the run shell and in
`<scratchpad>/wsl/*.sh`; nothing was written under `~/w7/p3a-espryt-*.log`.
