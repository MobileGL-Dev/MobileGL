# P3a package C — adversarial review of the Espryt switch-over (`espryt-review-v1`)

Reviewed `9ae4ec09 … 203120fa` on `p3a/espryt`, parent `39722687` (tag `p3a/contract`), worktree
`/home/swung/w7/p3a-espryt` (**not modified**; no worktree was created — every finding below was reached by opening
the file at `HEAD` and at `$BASE` through `git show 39722687:<path>`, and by reading the frontend code the arms call
into). Line numbers are `HEAD`'s unless a `$BASE` reading is named.

## Verdict

**REWORK.** The shape of the package is right and most of it is very carefully done: the nine op bodies are
recognisably their `Ops_*` counterparts, the readback ordering rule is implemented and written down where it belongs,
the G5 nine are untouched by construction (the diff contains no hunk between `Managers.cpp`'s `IsPoolable` and
`RingAllocate`), `AcquirePersistentMap`'s legacy body is byte-untouched, the pull arm is the pre-P3a text verbatim,
and the commit hygiene is clean.

What forces a rework is four things:

1. **`IsBufferDrawCleanByHandle` drops the live-map test and replaces it with a field nothing ever sets**, so on the
   handle arm a persistently mapped (non-adopted) buffer reads *draw-clean* forever and its host writes stop reaching
   the GPU (**C-1**). The report calls this substitution "identical semantics"; it is the inverse.
2. **`GLESBufferResource::hostBytes` is a cached raw base with no invalidation**, where every legacy path re-asked
   `MappedData()`. The two events that free or move the client's shadow — persistent-map adoption and a shadow
   resize — leave it dangling, and the fp64 narrowing and the pool reseed dereference it (**C-2**).
3. **Contract-review `M2` is entirely unaddressed** — neither fixed in `SlotTables.h` (which `C.5` hands to C at the
   tag) nor guarded at the one call site — and the package report does not mention it at all (**M-1**).
4. **Nothing in this package has ever executed.** Every lane C ran was either a legacy arm (`0` / `0x7f`) or the
   default mask where `EnsureBufferResource` returns `nullptr` for every buffer and `SyncToBackendFromApplier` returns
   at its first line. The 202 failures are correctly attributed to the stub applier (§8), and the corollary is that
   **no line of `Ops_H_*`, `EnsureBufferResourceForHandle`, `IsBufferDrawCleanByHandle` or the applier walk has been
   run once, in any build**. The package's own evidence cannot distinguish "correct" from "never reached".

None of the four needs a new design; C-1 and C-2 are a handful of lines each. The rework list is §"Rework" below.

---

## Critical

### C-1 — `IsBufferDrawCleanByHandle` answers **clean** for a buffer with live host writes, so `SyncPersistentMappedRange()` stops being called

`Managers.cpp:2385-2404` (the handle arm) against `:2405-2434` (the legacy arm, verbatim from `$BASE:1579-1600`),
`MG_State/GLState/BufferState/BufferObject.cpp:291-303` (`SyncPersistentMappedRange`), `Managers.cpp:2530`
(`EnsureBufferResourceForHandle`'s copy of the D-N call), `DirectGLES.cpp:611-620` / `:706-711` (the memo pass that
consumes the probe).

The legacy probe has five questions; the fourth is

```cpp
// A live non-zero-copy map may owe a per-draw SyncPersistentMappedRange push
// (persistent maps mutate the shadow without bumping the change serial).
if (frontend->IsMapped()) return false;                     // Managers.cpp:2428
```

The handle arm substitutes

```cpp
if (record->HasLiveHostWrites) return false;                // Managers.cpp:2400
```

and pins `HasLiveHostWrites` **false** with a `MOBILEGL_PIPE_VERIFY` assert two lines above it (`:2396-2399`), because
D-A4/D-N say P3a has no producer for it. `false` means *clean*. So the two arms give opposite answers on exactly the
state the legacy question exists for.

Failure scenario, and it is the common Minecraft/Flywheel/Sodium shape rather than a corner:

1. The app maps a buffer `GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT` and the store is **not** adopted — under 16 MiB,
   or `DisableLargeBufferAdoption`, or no `EXT_buffer_storage`. The frontend keeps the shadow; `m_isMapped` is true and
   `m_resource.IsGpuResident()` is false, which is precisely `SyncPersistentMappedRange`'s live case.
2. The app writes through the mapped pointer. **No op is emitted, no change serial moves, no epoch is bumped** — that
   is the whole reason the legacy probe asks `IsMapped()` instead of asking a serial.
3. On the handle arm the probe now finds: twin matches, context generation matches, `id != 0`, not
   `persistentMapped` (the *backend* store is an ordinary mutable one), `HasLiveHostWrites == false`, no
   `pendingRespecify`, no `pendingRanges`, `storageSize == Desc.Width`, `syncedChangeSerial == record.Serial`
   → **clean**.
4. `SyncVaoAttributeBuffersByHandle` (`DirectGLES.cpp:611-620`) therefore leaves `allClean == true` and stamps
   `memo->vboCleanEpoch = bufferEpoch`, so the probes are skipped entirely on later draws; and even when an epoch bump
   re-opens them, step 3 answers clean again. `EnsureBufferResource` is never called for that buffer again, and
   `EnsureBufferResourceForHandle`'s `bufferObject->SyncPersistentMappedRange()` (`:2530`) is the **only** per-draw push
   for a vertex/uniform/SSBO persistent map — D-N's other seven sites are the indirect (`DirectGLES.cpp:297, 4964,
   4965, 5066, 5067`) and index (`:4710`, `MultiDraw.cpp:511`) paths. The frame draws the last uploaded bytes, forever,
   with no diagnostic anywhere.

Refutations attempted, all failed:

- *"`persistentMapped` above it covers this."* It covers the **adopted** store only (`resource->persistentMapped` is
  the backend's own coherent donation). The case here is the emulated persistent map, which is exactly what
  `SyncPersistentMappedRange` exists for and what `MOBILEGL_COHERENT_AS_FLUSH` exercises.
- *"The pending-range test catches it."* Circular: a range only appears once `SyncPersistentMappedRange` →
  `NotifySubData` runs, and that only runs from the ensure path the clean verdict skips.
- *"`FLUSH_EXPLICIT` maps publish through `FlushMappedRange`, so the serial moves."* True, and irrelevant: `:297`'s
  guard is `if (m_mappingAccess & FlushExplicit) return;` — the **non**-explicit map is the one this push serves.
- *"It only costs one stale frame."* No: nothing on either side ever moves back to dirty.

Fix: keep a live-host-write input on the handle arm. The cheapest honest one is to keep asking the frontend object
that `EnsureBufferResourceForHandle` already takes (D-N keeps `SyncPersistentMappedRange` in Espryt for all of P3a, so
the object is present at every call site of this probe: `DirectGLES.cpp:614`, `:786`, `Managers.cpp:2411`) — i.e.
`IsBufferDrawCleanByHandle(res, resource, frontend)` with `if (frontend && frontend->IsMapped()) return false;` beside
the `HasLiveHostWrites` test, and a comment saying the frontend read retires when P5 gives `HasLiveHostWrites` a
producer. Do **not** delete the `HasLiveHostWrites` test or its assert; they are right.
Gate to add: an itest that maps a sub-adoption-threshold buffer persistently, writes through the pointer with no GL
call at all, draws, and reads the pixels back — nothing in the named must-not-break set does this today
(`LargeArenaAdoption` and `StorageBufferRegrow` both drive the **adopted** path).

### C-2 — `hostBytes` is a cached raw base into the client's shadow, and two ordinary events free or move that shadow under it

`Managers.h:651-663` (the member and its own warning), written at `Managers.cpp:1837`, `:1885`, `:1933`; dereferenced
at `:1901`, `:1966`, `:1974`, `:1997`, `:2498`, `:2533`, `:2538` and `:4186`. Against
`MG_State/GLState/BufferState/PipeResource.h:100-118` (`ResizeShadow`, `AdoptPersistentMap`).

The comment is right about the rule — *"it may not be dereferenced except while the client's shadow is known to be
live"* — and the code does not keep it. Every legacy path re-read `bufferObject.MappedData()` at the moment of use;
the handle arm reads a pointer recorded by whichever content-carrying call came last. Two events invalidate it and
neither clears it:

- **`PipeResource::AdoptPersistentMap` does `m_shadow->clear(); m_shadow->shrink_to_fit();`** (`PipeResource.h:116-118`),
  i.e. the allocation `hostBytes` points into is *freed*, and the live bytes move to the coherent map.
- **`ResizeShadow` is `reserve(bit_ceil(size)); resize(size)`** (`:102-105`): a grow past the reserved capacity
  reallocates and frees the old block. An **orphaning** respecify (`glBufferData(size, NULL)`) carries
  `HasDefinedContent = 0` and therefore `initialBytes == nullptr` (D-A2), so `Ops_H_Respecify` (`:1836-1838`) does not
  refresh the base it just invalidated.

The reachable consequence is `SyncFloat64AttributeAsFloat32ByHandle`:

```cpp
const Uint8* const sourceBase = resource->hostBytes;         // Managers.cpp:4186
...
const Bool memoHit = stream.valid && convertedBufferId != 0 && !resource->persistentMapped && ...   // :4218
NarrowDoubleStreamToFloat32(sourceBase + attrib.Offset, sourceStride, componentCount, elementCount, converted); // :4235
```

The memo is *deliberately* not trusted for a persistently mapped source (correct, and inherited), so for an adopted
buffer the `!resource->persistentMapped` term makes `memoHit` false **on every draw** and the narrowing reads
`elementCount * componentCount * 8` bytes from the freed shadow, every draw. The legacy arm at `:4056` reads
`bufferObject->MappedData()`, which after adoption is `PipeResource::Bytes()` → the coherent map → the live bytes. So
this is simultaneously a use-after-free and a wrong-data path, and it is not the gap D7 declares (D7 is the missing
`SyncGpuWrites`, a different and lesser problem on the same function).

The second dereference is the pool reseed (`:2489-2498`): `glBufferSubData(TempBufferTarget, 0, poolSize,
resource->hostBytes)` uploads `poolSize` bytes from the cached base, gated only on `hostBytes != nullptr` — a dangling
pointer is not null. Reaching it needs `resource->id == 0` with a pooled entry of exactly `Desc.Width` (`IsPoolable`
caps at 8 MiB), so it is narrower than the fp64 path, but it is the same defect and it is on the adopt→respecify→
regrow path `StorageBufferRegrowScenario` drives.

Same finding, second half, **undeclared**: that `hostBytes != nullptr` term is a change to the **pool decision**, not
to "the source of bytes" that D-F permits. `$BASE:1662-1663` is `(poolSize > 0 && !resource->persistentMapped)`; the
handle arm adds a third condition, so a buffer whose base was never recorded silently stops recycling pool ids. It is
not among the ten declared deviations and it moves `AcquireFromPool` call counts, which `StreamedArenaScenario`'s two
recycle cases observe.

Refutation attempted: *"every content-carrying call refreshes the base, so it is never stale when read."* False for
both events above — adoption is not a content-carrying call, and an orphaning respecify carries no bytes by design.
*"An adopted buffer never reaches those reads."* False for `:4186`: the fp64 path has no `persistentMapped` early-out,
only a memo exclusion that makes the read *more* frequent.

Fix: (a) in `Ops_H_MapPersistent`, set `resource->hostBytes = nullptr` when the map succeeds — the shadow is about to
be released and the coherent pointer is `resource->persistentPtr`, which is what the fp64 narrowing should read for a
`persistentMapped` resource; (b) in `Ops_H_Respecify`, `resource->hostBytes = nullptr` whenever `desc.HasDefinedContent
== 0` (an orphan says the client has no bytes for this store); (c) gate the pool reseed on the applier's own
`record->Desc.HasDefinedContent` as well, so a reseed can never move bytes the client did not just hand over. Each is
one line, and (a) is also what makes `SyncFloat64AttributeAsFloat32ByHandle` read the right memory for an adopted
source.

---

## Major

### M-1 — contract-review `M2` is unaddressed and unmentioned: `GetOrCreate(MGPipeHandle)` still adopts a stale generation, and `EntryAt` still resizes on an unbounded client slot

`SlotTables.h:272-286` is byte-identical to `39722687` (`git diff 39722687 HEAD -- SlotTables.h` adds
**only** `ReleaseByHandle`, `:288-311`); `EntryAt` at `:453-456` still does `m_slots.resize(slot + 1)`;
`GetOrCreateBufferResourceForHandle` (`Managers.cpp:2227-2238`) adds neither a slot bound nor a generation direction
test.

`contract-review-v1` M2 and its must-know item 6 both name this as C's to fix or guard, in a file `C.5` hands to C at
the tag; `INTEGRATOR-DECISIONS.md` ID-8 records it as going "to espryt's e1". `espryt-v1` does not mention M2 anywhere
— not in §1's description of e1, not in §5's deviations, not in §6's post-rebase list. The consequence is unchanged
from the contract review: a `handle.Gen` *behind* the entry's destroys the incumbent live twin
(`entry.backend.reset()`), and because `GLESBufferResource::~GLESBufferResource` is `= default` (`Managers.h:~600`)
that drops a driver buffer id with no `glDeleteBuffers` and no pool enrolment — a leak *and* a resource that loses its
storage with no diagnostic. `FindByHandle` (`:294-301`) refuses the same input, so the two entry points still disagree.

e6's new case cannot go red for it: `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm`
(`SanityTest.cpp:3896-3950`) only reaches `GetOrCreate(second)` **after** `ReleaseByHandle(first)` has set
`entry.Live = false`, which is the one path where the `entry.Live && entry.Gen != handle.Gen` arm cannot run.

Fix, as the contract review specifies: refuse the backwards case in `GetOrCreate(MGPipeHandle)` (return `m_nullTwin`,
or `Fatal{ProtocolCorruption}`), say in the comment which direction each arm covers, and bound `handle.Slot` before
`EntryAt` — plus one `SanityTest` case that stamps a live twin at `{slot, gen=2}`, calls `GetOrCreate({slot, gen=1})`
and asserts the incumbent survived. Note the applier's `RecordAt` has the identical unbounded-resize shape
(`wire-review-v1` M-D); one of the two must bound it and both should say which.

### M-2 — the attribute walk is bounded by `AttributeCount`, so a shrinking configuration leaves arrays enabled that the legacy walk disabled

`Managers.cpp:3794-3796`:

```cpp
const Uint32 attributeCount = std::min<Uint32>(rec->AttributeCount, MG_Pipe::kMGPipeMaxVertexAttribs);
for (Uint attribIndex = 0; attribIndex < attributeCount && emitAttributes; ++attribIndex) {
```

against `$BASE:2479` `for (Uint attribIndex = 0; attribIndex < allAttributes.size() && emitAttributes; …)`, i.e. all
**32** slots, with `glDisableVertexAttribArray` reached for every disabled one.

D-G2 fixes only `AttributeCount <= 32`; nothing in the contract requires the client to emit 32. D-H3 sets the
precedent in the other direction for the *buffer* set ("`Count` = MAX_VERTEX_ATTRIBS **truncated to the highest
enabled attribute + 1**"), so a client that applies the same rule to `MGPVertexElements` is contract-legal and would
make attribute 5 stop being disabled after the VAO disables it — leaving the driver VAO with an enabled array pointing
at a stale buffer, which is the class D-J's first Adreno workaround exists to prevent.

This is free to close and the applier already did the work: `MGPipeApplyCreateVertexElements` zeroes both arrays
before the unpack (`wire-v1` §2.2), so `rec->Attributes[i]` for `i >= AttributeCount` is all-zero, i.e. `Enabled == 0`.
Walk `kMGPipeMaxVertexAttribs` and the disable arm is restored with no dependency on B at all. If the bound is kept
deliberately, it has to be stated as a hard requirement on B (`AttributeCount == 32` always) in the report and pinned
by a `VertexInputEmit` case — today it is neither.

### M-3 — the vertex-input subsystem cannot in fact "run alone", and the header says it can

`Managers.h:690-692` — *"Kept separate because the two bits are separately clearable and the A/B has to be able to run
either one alone."*

With bit 8 set and bit 7 clear: `EnsureBufferResource` (`Managers.cpp:2556`) takes the **legacy** arm and puts the twin
in `PipeResource::m_backend`, so `g_backendBufferResources` stays empty; `BindAttributeBufferByHandle`
(`Managers.cpp:3421-3433`) and `SyncZeroStrideAttributeByHandle` (`:3437-3465`) resolve the driver id only through
`FindBufferResourceForHandle`, get null, log `MGLOG_E_ONCE` and `continue` — **without disabling the array**, so the
draw fetches through whatever pointer the driver VAO last held. Every attribute of every draw, in a configuration the
comment advertises and that G12's A/B is entitled to run.

(The mirror combination, bit 7 set and bit 8 clear, is fine: the legacy VAO walk calls `BindAttributeBuffer` →
`EnsureBufferResource`, which dispatches to the handle arm.)

Fix: either make bit 8 imply bit 7 in `ResolveVertexInputSubsystemArm()` (with a loud line saying so) and correct the
comment, or have the two handle-arm binders fall back to `EnsureBufferResource(attrib.Buffer)` when the table has no
twin. The first is the honest one; a silent fallback is what `EnsureBufferResource`'s own null-handle arm
(`:2557-2566`) deliberately refuses to do.

### M-4 — the two new arms ignore `Features.PipeLegacyMemos`, so "no arm at all" is reachable and silent

`Managers.cpp:2215-2225` — both resolvers read only `MG_Config::Features.PipePush & kMGPipeSubsystem{Resources,
VertexInput}`.

The tree's established rule for a re-keyed family is the three-way `EsprytSlotArmVerdict`
(`SlotTables.h:89-99`): bit clear **and** `PipeLegacyMemos` false is `NoArm`, and `ResolveEsprytSlotTablesArm()`
**stops**; Magma's equivalent raises `Fatal{PipeLegacyMemosDisabled}` (`MagmaPipeArms.h:112-115`). C adds two new
legacy arms with no such classification, so `(bits 7/8 clear, MOBILEGL_PIPE_LEGACY_MEMOS=0)` runs the pre-handle
buffer and VAO paths in silence. That pair is not hypothetical: it is what
`MG_IntegrationTest/CMakeLists.txt:863` pins for every `.Handles` lane today
(`MOBILEGL_PIPE_LEGACY_MEMOS=0;MOBILEGL_PIPE_PUSH=0x7f`), which is contract-review item 11's problem — but item 11 is
about which lane exists, and this is about C's arms not answering the knob at all.

Fix: give bits 7 and 8 the same verdict shape as `ClassifyEsprytSlotArm`, or at minimum log at `MGLOG_E` and stop in
`Resolve*SubsystemArm()` when the bit is clear and `Features.PipeLegacyMemos` is false. Package D's
`ResourceSubsystemControlScenario` is then able to assert it.

### M-5 — the re-keyed memos are correct only if `wire`'s C2 rework lands, and the report never says so

`Managers.cpp:3766-3776` (the twin's gate), `DirectGLES.cpp:601-610` and `:640-648` (the `ResolvedDrawBuffers` key).

`MGPipeApplierReset()` runs at **every** change of the current `GLContext` (`Tracker.h:192-195`, established in
`wire-review-v1` C1) and today sets `VertexBuffersSerial` and `IndexBufferSerial` back to **0**
(`PipeApply.cpp:565`, `:567`), while the twin survives the excursion — `BackendVertexArrayObject` has no
context-generation member and `OnBackendContextDestroyed` runs on destroy, not on make-current. So both counters walk
back through values this twin has already stamped, and `m_syncedVertexBuffersSerial` / `m_syncedIndexSerial` can match
over cleared state.

I checked whether C's gate survives that on its own, and it does **not**: `attributesDirty`
(`Managers.cpp:3768-3772`) is an AND of `{elementsHandle, elementsSerial, buffersSerial}`, and after a reset the
per-record `ContentSerial` also restarts at 1 for a re-created CSO at the **same** `{slot, gen}` (an applier reset is
not a slot recycle, so `Gen` does not move — which is the half `wire-review-v1` waved through as "safe (a recycled slot
moves Gen)"). With `wire`'s C2 fix applied — *advance* the two global serials at reset instead of zeroing them — the
`m_syncedVertexBuffersSerial` term can never match again after a reset and the whole AND is forced dirty, which
rescues the `ContentSerial` half as well. So: **C's design is correct conditional on C2, and unsafe without it**, and
nothing in `espryt-v1` records the dependency.

Fix: state it in the report and at the gate (one comment naming `PipeApply.cpp`'s reset and why the
`VertexBuffersSerial` term is load-bearing), and add it to §6 as a post-rebase precondition: if the integrator lands
`wire` without C2, this package must not be merged.

Related, same area, smaller: after a reset and before the client's first re-emission, `BoundVertexElements` is null, so
`SyncVaoAttributeBuffersByHandle` builds a memo whose key is `{null, 0, 0}` and marks it `valid`
(`DirectGLES.cpp:640-648`). That key never changes, so the memo hits on every later draw for a VAO whose attributes
have moved — the legacy arm's config version caught exactly this. Treat "no bound elements record" as a memo miss.

---

## Minor

- **m1 — the report's D-E evidence is incomplete.** §3.1's statement-level diff renders
  `GetBackendResource()/SetBackendResource()` → "the slot table's `GetOrCreate(res)`", but the substituted call is
  `GetOrCreateBufferResourceForHandle` (`Managers.cpp:2227-2238`), which additionally seeds `pendingRespecify = true`
  on a freshly minted twin — where `Ops_AcquirePersistentMap` (`$BASE:1146`) does not. The outcome is the same (a twin
  with no `storageInitialized` respecifies either way), but a claim of "byte-identical below the signature" must name it.
- **m2 — `Ops_H_Readback` maps at `record.Offset` while clamping the size against `storageSize`**
  (`Managers.cpp:1993-2001`); `$BASE:1404` maps at literal 0. Equivalent under D-A2's whole-buffer contract, but a
  non-zero `Offset` that the applier's `BufferRangeFault` accepts against `Desc.Width` can still run off a smaller
  `storageSize`. Clamp `size` to `storageSize - Offset`.
- **m3 — `Ops_H_FlushRange` lost the `ZoneScopedC` the legacy map arm carries** (`$BASE:1357-1359`). Profiling only.
- **m4 — the shared `FlushPendingRangesFrom` adds `if (hostBase == nullptr) return;` after the queue is drained**
  (`Managers.cpp:1109-1111`), which is a text change the *legacy* arm also runs inside a push build. Unreachable there
  (`MappedData()` is non-null whenever ranges are queued) and defensively right, but it is the one place D1's "one
  body, so the arms cannot drift" is not literally true.
- **m5 — the VAO index bind no longer ensures storage.** `$BASE:2622` calls `EnsureBufferResource(indexBufferBinding)`;
  `Managers.cpp:3862-3866` only `FindBufferResourceForHandle`. Covered in practice because `PrepareForDraw`
  (`DirectGLES.cpp:3193-3196`) runs `SyncNeccessaryBuffers` before `SyncCurrentVAO` — but `SyncNeccessaryBuffers`
  ensures the IBO only when `includeIBO`, so a non-indexed draw whose `IndexBufferSerial` moved logs
  `"No backend buffer found for index buffer binding"` and leaves the binding alone. Harmless (the serial is not
  stamped, so the next indexed draw repairs it), noisy, and worth one line of comment.
- **m6 — new per-draw allocator probes, unmeasured and unmentioned.** `HandleOfBuffer` (`Managers.cpp:2220-2225`) goes
  straight to `MGPipeSlots().FindByLifetimeId`, which is a hash lookup (`MG_Impl/Pipe/SlotAllocator.cpp:97-105`), and
  it now runs inside `EnsureBufferResource` (every SSBO/UBO/atomic/XFB/PBO/IBO bind), inside `IsBufferDrawClean`, and
  once per SSBO per draw through `MarkBufferGpuWritten`. The legacy arm paid a pointer compare. `BackendSlotTable`'s
  own `m_memoLifetimeId`/`m_memoHandle` front memo is never populated on this arm (only the minting overload calls
  `RememberHandle`), so nothing absorbs it. ID-6 makes this recorded-not-gated, but D.4 has to be told to expect it,
  and memoising inside `HandleOfBuffer` is a two-line recovery.
- **m7 — "no `MG_State` type in any new handle-arm signature" is stated more broadly than it holds.** D6's list omits
  `EnsureBufferResourceForHandle` (`Managers.h:733-734`) and `MarkBufferGpuWritten` (`:746`), both of which take
  `SharedPtr<BufferObject>`. G13 only greps `PipeApply.h`, so nothing is broken; the sentence should name its
  exceptions rather than list only the functions that satisfy it.
- **m8 — a contract point the report owes B and does not state.** `MarkGpuWritten` sets **both** `m_hasDefinedContent`
  and `m_gpuWritePending` (`BufferObject.cpp:314-317`). The client's `OnGpuWritten` implementation must set both, or
  the next `ResourceRespecify` carries `HasDefinedContent = 0`, `Ops_H_Respecify` orphans instead of uploading
  (`Managers.cpp:1873-1877`), and the shader-written contents are dropped. D8 states the *range shape*; this is the
  other half.
- **m9 — `MOBILEGL_PIPE_LEGACY_MEMOS=0` was checked with `-fsyntax-only`, which is not the claim.** §4.2's check
  proves the two translation units parse; it does not link, does not cover `MultiDraw.cpp` / `Utils.cpp` /
  `SanityTest.cpp`, and never runs. The greps do support the conclusion (no retired member is referenced outside a
  `MOBILEGL_PIPE_LEGACY_MEMOS` arm — I re-ran them across the whole backend directory), so the *conclusion* stands;
  the *evidence* should say "parses" rather than "compiles".

---

## Dimension-by-dimension

**(1) The nine op bodies vs their `Ops_*` counterparts.** Diffed side by side against `$BASE:1134-1447`.

| pair | verdict |
|---|---|
| `Ops_Respecify` (`$BASE:1213`) / `Ops_H_Respecify` (`:1832`) | Substitutions only: `ResourceOf` → `FindBufferResourceForHandle`, `GetSize()==0` → `desc.Width==0`, `RespecifyStorageNow` → `RespecifyStorageWith(desc.Width, desc.Usage, HasDefinedContent?initialBytes:null, ResourceSerialOf)`. Immutable-store retire, off-thread arm, all five field resets and the `return`s are structurally identical. ✔ (plus the `hostBytes` write — C-2) |
| `Ops_SubData` (`:1266`) / `Ops_H_SubData` (`:1880`) | All four arms preserved in order: `pendingRespecify` early-out, off-thread/size-mismatch queue, adopted zero-copy stamp-and-return, `EsprytDisableUploadRing` immediate upload, queue-only default (the Mali WAR fix). `StorageMatches(obj)` → `StorageMatchesSize(ResourceWidthOf)`. ✔ |
| `Ops_ResidentSubData` (`:1304`) / `Ops_H_ResidentSubData` (`:1911`) | Identical; one added `bytes == nullptr` guard. `hostBytes` deliberately not recorded (D10) — correct, and the reason given is the right one. ✔ |
| `Ops_FlushMappedRange` (`:1314`) / `Ops_H_FlushRange` (`:1927`) | Same five arms; `AccessFlags` re-widened from the record and read unnormalised; the kill-switch map arm gains `&& resource->hostBytes != nullptr`, which only steers a null base into `UploadRangeFrom`'s own null early-out. ✔ (m3) |
| `Ops_ReadbackFromGpu` (`:1382`) / `Ops_H_Readback` (`:1978`) | Both branches survive; `WritebackFromBackend` → `OnBufferWriteback`. ✔ (m2) |
| `Ops_OnDestroy` (`:1417`) / `Ops_H_Destroy` (`:2037`) | The *same function* is called with the twin `ReleaseByHandle` hands out, so the three outcomes are byte-identical by construction. A `ResourceDestroy` for a slot the backend never twinned yields a null and returns — no leak. ✔ |
| `Ops_AcquirePersistentMap` (`:1134`) / `Ops_H_MapPersistent` (`:2055`) | Statement-for-statement equal below the signature: four-way gate, stale-generation wipe **before** the stamp, idempotency hit, `++g_bufferBackendIdGeneration`, `immutableStorage = true` before the map, decline log, all six success stamps. The legacy body is untouched. ✔ (m1) |
| — / `Ops_H_Create` (`:1824`) | No-op, as D-A2's row says. ✔ |
| — / `Ops_H_UnmapPersistent` (`:2129`) | No-op with its reason. ✔ (D9 sound) |

No early-out, ring use, pool interaction or epoch bump differs **inside the nine**. The two behavioural drifts I found
are outside them, in `IsBufferDrawCleanByHandle` (C-1) and `EnsureBufferResourceForHandle` (C-2's pool gate).

**(2) The seven wrappers and epoch discipline.** All seven `*Tracked` shapes are reproduced (`:2141-2176`), including
`Ops_H_MapPersistentTracked`'s **bump on decline** (`:2169-2175`); `Ops_H_Create` and `Ops_H_UnmapPersistent`
correctly bump nothing. `RegisterBufferBackendOps` / `UnregisterBufferBackendOps` keep their bumps and install/clear
`MGPipeSetResourceOps` inside them (`:2291`, `:2306`). The readback order is **writeback → unmap → serial stamp →
`BumpBufferMutationEpoch`** with the bump in the wrapper, i.e. strictly last (`:2010-2035`, `:2161-2164`) —
`ARCHITECTURE.md:288` satisfied, and the rule is written into the function where the next reader will meet it. Serial
stamping reads `record->Serial` inside the hook, which is consistent with the applier bumping before the hook
(`wire-v1` §2.1); every arm that does not stamp leaves the twin dirty, which is the safe direction. ✔

**(3) The slot table.** `ReleaseByHandle` (`SlotTables.h:288-311`) hands the twin out with the entry already retired,
forgets the front memo when the slot matches, frees no slot — matching `ReleaseTwinAt`'s established shape and D-L's
order. No `ForEachLive` sweep over the buffer table exists (item 7 ✔ — and the handle overload writes no `stateRef`,
so one would see nothing anyway). `EnsureProcessTeardownSentinel()` is armed by `GetOrCreate(MGPipeHandle)`
(`SlotTables.h:277`), i.e. at first insertion, not from a destructor ✔. The destroy path frees ids and pool-enrols
exactly as `Ops_OnDestroy` did ✔. **M2/M-1 is the one open hole.** One latent hazard worth recording: after
`ReleaseByHandle` the twin is destroyed, so `ResolvedDrawBuffers::Entry::resource` and `iboResource` become dangling —
where on the legacy arm the frontend's `SharedPtr` kept them alive. Both consumers check identity through
`FindByHandle` before touching the pointer (`Managers.cpp:2386-2388`, `DirectGLES.cpp:784-790`), so nothing is
dereferenced today; the invariant deserves a sentence at the member, because it is now the only thing standing between
the memo and a use-after-free.

**(4) The VAO half.** Gate: `{m_syncedElementsHandle, m_syncedElementsSerial}` + `m_syncedVertexBuffersSerial` +
`m_syncedIndexSerial` + the unchanged `m_syncedBufferIdGeneration` and `m_hasConvertedFloat64Attribute`, with
`m_syncedFetchBaseInstance` kept as the *emitted* record — D-G4's table, plus D5's independent index serial. The walk
is field-for-field the legacy one (enable/disable, fp64 narrowing with the Adreno disable and the explicit re-enable,
zero-stride binding-API path, attribute bind, BGRA drain/probe/disable, `Pointer`/`IPointer` at the shifted offset,
divisor); the per-attribute `needsSync*` skips are gone, which costs redundant GL calls but changes no state.
`BaseInstanceByteShiftWire` (`:3435-3438`) reproduces `BaseInstanceByteShift` (`$BASE:2377-2382`) exactly, divisor
term included, and is applied at the pointer call and inside the fp64 narrowing — the two places the legacy value
reached; `DrawElementsIndirect` and the multi-draw tiering are untouched, as D-H2 requires, and the three former scope
sites now compile to `((void)value)` under `MOBILEGL_PIPE_LEGACY_MEMOS=0` only (`DirectGLES.cpp:5272-5276`).
`ResolvedDrawBuffers` re-keys with the same hit/miss/repair shape and the same `vboCleanEpoch`/`iboCleanEpoch`
semantics ✔. **Stale-record risk: M-5 (reset) and M-2 (`AttributeCount`).** `IsBufferDrawClean`'s reads are equivalent
except for the map state — **C-1**. One cross-package assumption the report does not record: `VertexFetchBaseInstance`
is *sticky* in the applier, where the legacy global was scoped to the draw, so correctness depends on B emitting
`set_vertex_buffers` with `BaseInstance = 0` for the next non-base-instanced draw — which D-H2.3's `ContentHash`
requirement delivers, and which `DrawParametersScenario.APlainDrawAfterABaseInstancedOneSeesZeroAgain` is the gate for.

**(5) D-E / D-F / D-J.** `Ops_AcquirePersistentMap` is untouched (no hunk in the diff touches `$BASE:1134-1210`);
`Ops_H_MapPersistent`'s two permitted changes are the only ones (m1 aside). `git diff 39722687 HEAD --
MobileGL/MG_Backend/DirectGLES/Managers.cpp` places **no hunk** inside any of the nine G5 functions — the two hunks in
that region are an insertion before `RespecifyStorageNow` and a lone `#endif` after the last of the pre-existing
helpers, so G5 holds by construction, script or no script. Both Adreno workarounds are reproduced in the handle walk
with their rationale (`Managers.cpp:3807-3826`, `:3852-3874`); the three rings are untouched;
`InvalidateIndexedBufferBindingShadowsForId` on an extent move survives inside the shared `RespecifyStorageWith`
(`:1006-1008`). ✔

**(6) G1 / D-K.** Every new member is push-only (`hostBytes` `Managers.h:651-663`; `ResolvedDrawBuffers::Entry::handle`
and the four struct fields; the four new twin members) and every retired member is under
`MOBILEGL_PIPE_LEGACY_MEMOS`, which `CMakeLists.txt:476-479` forces ON whenever `MOBILEGL_PIPE_PUSH` is OFF — so the
pull build's `sizeof` and symbol set cannot move, and the reported `0 added / 0 removed / 0 resized / 0 renamed` at all
six commits is consistent with the guards I read. `grep` over the whole backend confirms every remaining reference to
the seven retired names sits inside a `MOBILEGL_PIPE_LEGACY_MEMOS` arm. No `pGLContext`; no `MG_State` type in any
`MGPipeResourceOps` signature (m7 is about a looser claim). ✔ (m9 on the `LEGACY_MEMOS=0` evidence.)

**(7) Legacy-arm parity.** The `#else` copy of the five helpers is the pre-P3a text **unmodified** — the diff proves
it, since neither hunk lands inside the block. In a *push* build the legacy arm runs the parameterised cores through
thin forwarders (`RespecifyStorageNow`, `UploadRangeNow`, `FlushPendingRangesNow`, `StorageMatches`, the two-argument
`DrainResidentWritesNow`), and I compared those statement by statement against `$BASE:895-1118`: the only difference
is m4's added null test. The `0` and `0x7f` lanes being green at HEAD is consistent. ✔

**(8) The 202 default-arm failures.** Attributable to the stub applier, and I could not construct a counter-example.
With no `ResourceCreate` emitted, `HandleOfBuffer` returns the null handle for every buffer, so `EnsureBufferResource`
(`Managers.cpp:2556-2568`) returns `nullptr` before touching any of C's new code; with no
`CreateVertexElements`/`BindVertexElements`, `BoundVertexElementsRecord` returns null and
`SyncToBackendFromApplier` returns at `:3771` before `Bind()`. Those are the only two new failure sources reachable at
`0x1ff` on this tree, they are exactly the two diagnostics quoted in §4.4, and the scenario histogram ("every scenario
that draws or touches a buffer") matches. **But the same argument is the reason this package has no positive
evidence at all**: the 202-failure run proves the two refusals fire and nothing else, and the two green lanes exercise
only the legacy arm. Every finding above is therefore un-contradicted by any run C has done, and the post-rebase gate
is the first execution of the code under review.

**(9) Tests.** No name added or removed (`ctest -N` 2487 == 2487, five additions all `contract`'s) — G14 ✔.
`EveryReKeyedObjectClassAnnouncesItsOwnDeath`'s buffer arm asserts the **absence** of a notice, which is the
falsifiable form of D-L (it goes red if anyone adds a seventh raiser, which is the mistake it guards). It does not go
red if a *notice were dropped* — but that direction is covered for the six kinds by the existing membership assertion,
and for the buffer the equivalent is `resource_destroy` reaching `Ops_H_Destroy`, which no unit test can see and which
`HandleRecycleScenario`'s buffer arm (package D) is the gate for. `EverySwitchedOverKindResolvesItsTwinThroughTheHandleArm`
drives the real walk and its five assertions are each falsifiable (drop `ReleaseByHandle`'s `Live = false` and
`:3908` goes red; drop the `Gen` compare in `FindByHandle` and `:3922` does). Gap: neither case can go red for M-1,
and nothing anywhere exercises an `Ops_H_*` body.

**(10) Commit hygiene.** Six commits, six single-line messages, empty bodies (`git log --format=%b` prints nothing for
all six), author and committer `Swung0x48 <swung0x48@outlook.com>`, no `Co-Authored-By` and no generator line.
`git diff --stat 39722687 HEAD` names exactly `SlotTables.h`, `Managers.h`, `Managers.cpp`, `DirectGLES.cpp`,
`SanityTest.cpp` — five of C.2's seven, nothing outside. e1-e5's messages are byte-identical to C.2's; e6's is C's own
(C.2 gives step 6 no message) and follows the house style. ✔

---

## Judgement on the ten declared deviations

| # | judgement |
|---|---|
| **D1** `#if PUSH / #else` instead of an in-place refactor | **Sound, and the right call.** The measurement behind it is real (five resized pull symbols), G1 admits an empty resize set, and the diff proves the `#else` copy is the original text untouched. The push arm's forwarders are behaviourally equivalent to it statement by statement; the single exception is m4's added null test, which is unreachable on the legacy arm. The claimed benefit — one three-tier ladder inside a push build — is delivered. |
| **D2** `g_pendingFetchBaseInstance` retired, not deleted | **Sound.** Deleting two defined symbols from the pull build is a straight G1 break, and `ARCHITECTURE.md:367` requires the legacy arm to keep working. The re-reading of C.2's `# empty` grep as "every hit inside a `LEGACY_MEMOS` arm" is the same rule the twin-memo grep two lines below already states, and I verified the hits: `Managers.h:1160-1172`, `Managers.cpp:3320-3333`, `DirectGLES.cpp:5271-5276`, all guarded. |
| **D3** `MGPipeSetResourceOps` from `RegisterBufferBackendOps()` | **Sound.** `BackendObject_DirectGLES.cpp` is not in C.2's file list and hard rule (4) forbids it; both named bring-up sites call `RegisterBufferBackendOps`, so one edit covers both, in the file that owns the table, paired with the unregister. Contract-review item 13's constraint (install at bring-up/teardown only) is met — and note it also re-installs on every make-current, which is idempotent because the pointer is the same object. |
| **D4** e2 carries `OnBufferWriteback`, e3 carries `OnGpuWritten` + the ordering | **Sound and unavoidable.** A handle-shaped readback has no object to call `WritebackFromBackend` on; the split is the brief's own, one step over, and both commits are individually coherent. |
| **D5** the new `m_syncedVertexBuffersSerial` | **Sound, and it is load-bearing beyond its stated reason.** D-G4's table drops `baseInstanceDirty` on the argument that a base-instance change moves `VertexBuffersSerial`; without a term for that serial the twin would never see the move. It is also the term that rescues the whole gate across an applier reset — see M-5. It should be described that way rather than as an addition to the census. |
| **D6** `EnsureBufferResourceForHandle` keeps its `SharedPtr<BufferObject>` | **Sound.** D-N keeps `SyncPersistentMappedRange` in Espryt for all of P3a and the call is named at the site. The accompanying claim about "no `MG_State` type in any new handle-arm signature" is overstated (m7), and note that C-1's fix *uses* this parameter, so the argument for keeping it is stronger than the report makes it. |
| **D7** the fp64 `SyncGpuWrites` gap | **The gap is real, correctly declared, and P8's to close — but it is not the only defect on that function.** `SyncFloat64AttributeAsFloat32ByHandle` also reads a cached shadow base that adoption has freed (C-2), which the declaration does not cover and which is worse than the declared gap: stale bytes are at least *valid* bytes. `DoublePrecisionScenario` will only see either of them if its source buffer is shader-written or adopted; as written it likely sees neither, so "`DoublePrecisionScenario` is the gate that would see it" is optimistic and should be checked against the scenario's actual body before it is relied on. |
| **D8** whole-resource `OnGpuWritten` as one `kMGPipeWholeBuffer` range | **Sound, and the right distinction to draw for P8/P9.** Verified against the payload: `MGPRange{0, ~0ull}` matches `MGPipeTypes.h:63-67` and `:177`, and `MGPBlobRef{address, size, kMGHostSpanSegNone, 0}` matches `:55-61`'s field order exactly. The report should add the `HasDefinedContent` half of the same contract (m8). |
| **D9** `Ops_H_UnmapPersistent` is a no-op | **Sound.** D-B4 forbids the alternative, nothing emits it in P3a, and the entry exists so the transport has both halves. |
| **D10** `Ops_H_ResidentSubData` does not record `hostBytes` | **Sound, and it is the one place the lifetime question was asked and answered correctly** — which is what makes C-2 a slip rather than a design error. |

---

## Rework list

**Blocking (before the integrator merges `espryt` in D.1):**

1. **C-1** — restore a live-host-write input to `IsBufferDrawCleanByHandle` (`Managers.cpp:2385-2404`): keep the
   `HasLiveHostWrites` test and its assert, add the frontend `IsMapped()` test through the object the three call sites
   already hold, with a comment saying it retires when P5 gives the field a producer. Add a scenario that writes
   through a non-adopted persistent map with no GL call and draws.
2. **C-2** — clear `hostBytes` at the two events that invalidate it: on a successful `Ops_H_MapPersistent`
   (`Managers.cpp:2055-2127`) and on a `HasDefinedContent == 0` respecify (`:1832-1878`); make
   `SyncFloat64AttributeAsFloat32ByHandle` (`:4186`) read `resource->persistentPtr` when the resource is
   `persistentMapped`; gate the pool reseed (`:2489-2498`) on `record->Desc.HasDefinedContent` as well as on the base
   being non-null, and declare the pool-decision change in the report.
3. **M-1** — close contract-review M2 in `SlotTables.h:272-286` (refuse a backwards `Gen`, bound `handle.Slot` before
   `EntryAt`) and add the `SanityTest` case that goes red for it. Record it in the report.
4. **M-2** — walk `kMGPipeMaxVertexAttribs` in `SyncToBackendFromApplier` (`Managers.cpp:3794-3796`), or state and pin
   `AttributeCount == 32` as a requirement on B.
5. **M-3** — make bit 8 imply bit 7 (or give the two handle-arm binders an ensure fallback) and correct
   `Managers.h:690-692`.
6. **M-4** — give bits 7 and 8 the `EsprytSlotArmVerdict` three-way treatment, or at minimum stop with a named line
   when the bit is clear and `Features.PipeLegacyMemos` is false.
7. **M-5** — record the dependency on `wire`'s C2 in the report and at the gate, and treat "no bound elements record"
   as a memo miss in `SyncVaoAttributeBuffersByHandle` (`DirectGLES.cpp:640-648`).

**Declared minors, non-blocking (ID-4):** m1-m9.

## Post-rebase verification the integrator must insist on

`espryt-v1` §6 is a good list and stands; these are the additions and the ones to refuse to accept as green without
seeing the output:

1. **§6's items 1-6 exactly as written**, and in particular item 5 — the default-mask lane green — is the *first* time
   any of this code executes. Treat a green there as bring-up, not as verification.
2. **A named case for C-1**, run in both arms: a persistently mapped, non-adopted buffer written only through its
   pointer, drawn, read back. If package D cannot add one, `MOBILEGL_COHERENT_AS_FLUSH=1` over the two
   `coherent_as_flush: true` Create fixtures (G3b) is the nearest existing proxy and must be run under push **and**
   pull with a byte-for-byte path comparison, not only an SSIM pass.
3. **A named case for C-2**: `DoublePrecisionScenario` with an **adopted** source buffer (≥ 16 MiB, adoption enabled),
   plus `StorageBufferRegrowScenario` under ASan or with `MOBILEGL_ESPRYT_*` pool tracing, since the pool reseed is
   the second dereference. A clean run of today's `DoublePrecisionScenario` does **not** discharge this.
4. **`VertexArrayEnableDisableScenario` under the default mask** — it is the only gate that can see M-2, and it must be
   run with a configuration that actually shrinks the enabled set.
5. **The three subsystem combinations, not two**: `0x1ff`, `0x7f` (both off) and `0x17f` (bit 8 on, bit 7 off). The
   third is M-3; if it is not made to work it must be made to refuse, and G12's scenario should assert whichever.
6. **`MOBILEGL_PIPE_LEGACY_MEMOS=0` with `MOBILEGL_PIPE_PUSH=0x7f`** — the `.Handles` itest pin — must produce a named
   verdict rather than a silent legacy run (M-4).
7. **Confirm `wire`'s C2 landed** (advance, not zero, at reset) before merging; if it did not, M-5 makes this package
   unsafe on any application that changes contexts.
8. `bash scripts/p3a_untouched_regions.sh 44c2b5cf HEAD` once package D lands it — §3.2's sha stand-in is credible and
   I independently confirmed no hunk lands inside the nine, but the real script and its `--self-test` are the gate.
9. A **real build** with `MOBILEGL_PIPE_LEGACY_MEMOS=0` (not `-fsyntax-only`), covering `MultiDraw.cpp`, `Utils.cpp`
   and the test targets, since the rebase brings in the client's headers (m9).
10. Record the per-draw `FindByLifetimeId` cost in D.4 (m6) so it is a measured number rather than a surprise in the
    P3a-vs-pull A/B.

---

*No file in `/home/swung/w7/p3a-espryt` was modified and no worktree was created. Base readings were taken with
`git show 39722687:<path>` into the scratchpad; those copies were the only artefacts and are the reviewer's to delete.*
