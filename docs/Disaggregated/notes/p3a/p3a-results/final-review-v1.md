# P3a final adversarial review — the integrated tree `5cb826b0..3e298c9a` on `feat/disaggregated`

Reviewer: final integrator-side review, 2026-09-08. Scope: the whole integrated diff, cross-package seams only —
each package's own review already ran. Read-only against `//wsl.localhost/Arch/home/swung/w7/pipe` (a gate is
running there; nothing was built or run in it) and the Windows worktree
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg` at the same commit `3e298c9a`.
Diff extraction: `<scratchpad>/wsl/review-final.sh` → `<scratchpad>/wsl/diff/` (37 files, +9273 / -230).

## VERDICT: **REWORK**

Two criticals, both of them exactly the class this review exists to find — one is a defect no per-package
reviewer could see because it only exists at the client × *non-Espryt backend* seam, and the other is a gate
that is green while protecting code the shipping build does not compile. Neither is expensive to fix (C-1 is
one line plus an integrator ruling; C-2 is a script/ID amendment plus one line of `-R`). Everything else on the
tree is in very good shape: the handle lifecycle, the base-instance path, the epoch ordering and the
serial/version coherence all hold under the adversarial sequences I could construct (§6), G13 purity is clean,
the contract payload is in the base, and CI was not narrowed.

---

## 1. Critical

### C-1 — Under any backend that does not install `StateObjectDeathOps`, every VAO leaks a `VertexElementsCso` slot **and** a ~1.3 KB applier record, for the life of the process, on the shipped default mask

**Where.**
- mint: `MobileGL/MG_Impl/Pipe/VertexInputEmit.h:171` — `MGPipeSlots().Acquire(MGPipeKind::VertexElementsCso, lifetimeId)`, called from `EmitVertexElements` at every validate point.
- record: `MobileGL/MG_Pipe/PipeApply.cpp:1202-1203` `RecordAt(g_applier.VertexElementsCsos, desc.Cso.Slot, kMGPipeMaxVertexElementsSlots)`; the record is `MobileGL/MG_Pipe/PipeApply.h:141-152` — `Array<MGPVertexAttribWire,32>` (24 B each) + `Array<MGPVertexBindingPointWire,32>` (16 B each) = **1280 B + header per slot**.
- the only free: `MobileGL/MG_Backend/DirectGLES/SlotTables.h:416-429` `OnFrontendObjectDestroyed` → `MGPipeSlots().Free(kKind, handle)`, reached from `MG_State/GLState/VertexArrayState/VertexArrayObject.cpp:53`'s `NotifyStateObjectDestroyed` **only when `g_stateObjectDeathOps` is installed**.
- the only installer: `MobileGL/MG_Backend/DirectGLES/Managers.cpp:314` `SetStateObjectDeathOps(&g_glesStateObjectDeathOps)`, inside Espryt's `EsprytSlotTablesEnabled()`. `grep -rn "SetStateObjectDeathOps" MobileGL/` returns that one call site and the declaration.
- no global escape hatch: `grep -rn "MGPipeSlots()\.Reset"` outside `MG_Test/` is **empty**.

**Failure scenario.** DirectVulkan/Magma with the new default `MOBILEGL_PIPE_PUSH = kMGPipeSubsystemsMigratedAtP3a = 0x1ff` (`MobileGL/ConfigLoader.cpp:257`, `MobileGL/MG_Pipe/MGPipe.h:95`). The client's vertex-input emission gate is backend-agnostic — `wants()` is `MGPipeSubsystemForDirty(bit) & pushMask` (`MG_Impl/Pipe/PipeFill.cpp:1462-1466`) and bits 5/9/10 now map to `kMGPipeSubsystemVertexInput` (`MG_Impl/Pipe/Tracker.h:145-148`) — so `EmitVertexElements` runs on every draw under Magma too. Each new `VertexArrayObject` takes a slot; `~VertexArrayObject` raises the death notice into a null `g_stateObjectDeathOps` and returns; the slot is never freed and `g_applier.VertexElementsCsos` never shrinks and never goes `Live = false` (there is **no** `delete_vertex_elements` producer anywhere in the library — `grep` finds only `PipeApply.cpp`'s definition and `MG_Test`). Sodium/Create churn VAOs per chunk section: 10 k VAOs ≈ 13 MB of applier records plus 10 k `SlotState` + 10 k `UnorderedMap` nodes, monotonically, on a platform with an LMK. Past 65536 slots (`kMGPipeMaxVertexElementsSlots`, `PipeApply.h:113`) `RecordAt` returns null and **every** `create_vertex_elements` trips `Fatal{ProtocolCorruption}` permanently — fatal on a verify build.

`MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:243-250` documents this exact anti-pattern as the reason Magma built its own age-reclaimed `MagmaPipeIdentityTable` instead of using `MGPipeSlots`: *"an allocator here would grow by one `SlotState` plus one map node per object EVER created, for the life of the process, on a platform with an LMK."* P3a re-introduces precisely that, from the client side, for a kind Magma deliberately kept out of the allocator.

**Refutation attempted, three ways, all failed.**
1. *"Espryt frees it, so this is Espryt's business."* Espryt does free it, and unconditionally — `OnFrontendObjectDestroyed` returns the slot *"whether or not any holder still had a twin at it"* (`SlotTables.h:407-410`). So the client's and espryt's own note that "on this tree VAO CSO slots are never freed" (`client-v2.md:308-309`, `espryt-v3.md:605-607`) is **wrong for Espryt and right for Magma** — both packages assigned the item to package C, and package C is the one backend where it already works. Nobody looked at the backend where it does not.
2. *"Magma is untouched in P3a, so this cannot be P3a's."* The mint is not in `MG_Backend/DirectVulkan/**`; it is in the client, which runs under every backend. Before this diff `MGPipeSlots()` held **zero** `VertexElementsCso` slots under Magma.
3. *"It is bounded by a shipped A/B."* No: `0x1ff` is the push default for both backends, and `MG_IntegrationTest/CMakeLists.txt:954, :1055, :1060` now pin `0x1ff` on three DirectVulkan lane families as well (raised from `0x7f` by gates' M1).

**Fix (one of):** (a) install the death notice for the `VertexElementsCso` kind from a backend-neutral place — a `MGPipeSlots().Free` in the client, driven by a `delete_vertex_elements` the client emits from `~VertexArrayObject` (this is `client-v2.md` §6.3 / `espryt-v3.md` §8's open item, and it is also the missing producer for the applier record); or (b) gate the client's vertex-input emission on the backend actually consuming it. (a) is the right one and closes three open rows at once.

### C-2 — G5's tenth row is green while the **push build compiles no `FlushPendingRangesNow` at all**; the ladder the shipping build runs is outside the gate

**Where.** `MobileGL/MG_Backend/DirectGLES/Managers.cpp`: `#if MOBILEGL_PIPE_PUSH` at `:902`, `#else` at `:1197`, `#endif` at `:1433`.
- `FlushPendingRangesFrom(GLESBufferResource&, const Uint8* hostBase, SizeT frontendSize)` — `:1051-1125`, **push arm**, the only three-tier ladder a push build has. Both call sites in a push build reach it: `:1717-1724` (the legacy call site, `#if PUSH → FlushPendingRangesFrom`) and `:2044` (`Ops_H_Readback`).
- `FlushPendingRangesNow(GLESBufferResource&, BufferObject&)` — `:1316-1385`, **inside the `#else`**, byte-identical to `5cb826b0:1004-1073`. A push build never compiles it.
- `scripts/p3a_untouched_regions.sh:76` lists the ten names including `FlushPendingRangesNow`; the extractor is preprocessor-blind (`:180-223`) and hashes the pull-arm text.

**Why this is "green for the wrong reason", not a nit.** `BRIEF-P3A.md`'s own risk row for this function is *"`FlushPendingRangesNow`'s three-tier drain silently changes tier because its caller now supplies bytes differently … The function is in G5's byte-identical set"*, and `scripts/p3a_untouched_regions.sh:39-41` states the threat model verbatim: *"A push arm that re-spells the three tiers beside an untouched pull arm satisfies G1 and defeats this gate's reason to exist — two ladders drift."* That is exactly the shape on the tree; the defence is keyed on the **name**, so a copy that was *renamed* walks past it. A future tier-threshold or map-access-bit edit made in `FlushPendingRangesFrom` — the only ladder the shipping build runs — regresses MC 26.3's p99 with G5 still green, and ID-6 makes the perf number recorded rather than gated, so nothing catches it.

It also **violates ID-13 as written**: *"`FlushPendingRangesNow` is defined exactly once, outside any `#if`, byte-identical to 5cb826b0."* It is defined once, but inside the `#else`. `espryt-v3.md:75-106, :519-520` (D12) raised precisely this and said *"It wants an explicit integrator ruling"* — **no ruling was recorded** (confirmed against `INTEGRATOR-DECISIONS.md`, which stops at ID-13's original text).

**Refutation attempted.** The author's argument at `Managers.cpp:1127-1138` — that a push build then contains exactly one ladder and both arms call it, where a forwarder would leave two definitions and make the extractor exit 2 — is sound *as far as it goes*, and I verified the ladders are behaviourally equivalent **today**: the three textual differences are the `hostBase == nullptr` early return (`:1066`, unreachable from the legacy call site, which passes `MappedData()`), `frontendSize` vs `bufferObject.GetSize()` (`:1070` vs `:1330`, the same value at that call site), and `UploadRangeFrom(resource, hostBase, …)` vs `UploadRangeNow(resource, bufferObject, …)` — and in the push arm `UploadRangeNow` is itself a one-line forwarder onto `UploadRangeFrom` (`:993-995`). So **there is no behavioural defect on this tree.** What is broken is the gate's coverage from here on, which is what G5 exists for.

**Fix (both halves).** (1) Amend `scripts/p3a_untouched_regions.sh` so the tenth row hashes the ladder the **push** build compiles: add `FlushPendingRangesFrom` to the list and capture its baseline sha at this commit, keeping `FlushPendingRangesNow` for the pull build. (2) Record the amendment as an ID (ID-14) superseding ID-13's "outside any `#if`" sentence, since the tree deliberately chose the two-arm shape. Both are minutes of work; without them the row is decorative for every build that ships.

---

## 2. Major

### M-1 — A `BufferObject` constructed while the resource op table is unregistered never gets an applier record, and unlike the legacy arm the handle arm has no repair

`MG_State/GLState/BufferState/BufferObject.cpp:44-45`: the handle is minted unconditionally but the create is
gated on `MGPipeResourceSubsystemEnabled()` = *bit 7 **and** `MGPipeGetResourceOps() != nullptr`*
(`MG_Impl/Pipe/PipeFill.cpp:593-596`). Espryt's consumer gate is bit-7-only
(`MG_Backend/DirectGLES/Managers.h:715-718`). The two disagree across a register/unregister boundary:
`UnregisterBufferBackendOps` nulls the table (`Managers.cpp:2466-2468`), called from `OnBackendContextDestroyed`
← `DirectGLES.cpp:11104 DestroyEGLContext()`; the re-register is at `MakeCurrent` (`DirectGLES.cpp:10568`).

A buffer born in that window latches `Published = false` (`ResourceTracker.h:342-350`), so no record exists;
every later `resource_respecify` is refused (`PipeApply.cpp:967-968`, counted into `RefusedResourceCalls`), and
`EnsureBufferResourceForHandle` reads `ResourceRecordOf(res) == nullptr → size = 0 → return resource`
(`Managers.cpp:2717-2721`) with **no diagnostic** — the store stays empty for the object's whole life and the
draw fetches through `id == 0`. The legacy arm recovers from the same window: `EnsureBufferResource` twins
lazily off the frontend object and full-uploads from the shadow.

*Refutation attempted:* "no GL call happens without a current context" — `CanTouchGLNow()` exists precisely
because frontend calls reach this code with no current backend context, and D-A2 explicitly preserves
`NotifySubData` as reachable off the render thread. The window is narrow, not empty.

**Fix:** in `MGPipeEmitResourceRespecify` (`PipeFill.cpp:629`), if `!tracker.WasPublished(handle)` emit the
create first (the descriptor is already built two lines below). Three lines, and it makes the create/destroy
latch self-healing in both directions.

### M-2 — `GLESBufferResource::hostBytes` is a raw pointer written off the render thread with no synchronisation

`Managers.h:673` `const Uint8* hostBytes = nullptr;` — a plain pointer. It is written at
`Managers.cpp:1921` (`Ops_H_SubData`) and `:1969` (`Ops_H_FlushRange`) **before** the `CanTouchGLNow()` test,
i.e. on the arm D-A2 keeps reachable off the render thread, and read on the render thread at `:2005`, `:2013`,
`:2044` (`Ops_H_Readback`), `:2639`/`:2741` (the ensure path) and `:4476` (the fp64 narrowing).

Everything else on that same struct and that same path is synchronised on purpose: `pendingRanges` /
`pendingResidentWrites` under `resource->pendingMutex`, `syncedChangeSerial` an `std::atomic` read with
`memory_order_acquire` (`:2581`, `:2610`). `hostBytes` is the one member the off-thread path touches with
neither. It is new in P3a — the legacy arm never cached a base, it re-read `bufferObject->MappedData()` on the
render thread at every use. espryt-review's C-2 fixed the *lifetime* of this pointer; nobody looked at its
*visibility*.

**Fix:** write it inside the `pendingMutex` scope, or only on the `CanTouchGLNow()` arm — the ensure path
republishes it on the render thread anyway (`:2741`, the `3c55e027` fix), so the off-thread store buys nothing.

### M-3 — the handle arm **drops** one of D-N's eleven `SyncGpuWrites` sites rather than keeping it, and the deviation was never ratified

`Managers.cpp:4456-4464`, in `SyncFloat64AttributeAsFloat32ByHandle`: *"the legacy arm opens with
`bufferObject->SyncGpuWrites()` … on this arm a 64-bit array whose SOURCE buffer was written by a shader and
not yet pulled back narrows stale bytes."* D-N's wording is *"**No move** of `SyncPersistentMappedRange` /
`SyncGpuWrites` off their 8 + 3 Espryt sites"* — a site deleted on the arm that ships is not a site kept. This
is a behavioural difference between the two arms **under the default mask**, and G2 requires the push and pull
integration lanes to be green name-for-name, so it survives only because no scenario covers the shape.

The closure table confirms nothing closed it: `espryt-review-v1.md:493-495` asked package D for
`DoublePrecisionScenario` **with an adopted source** and `StorageBufferRegrow` under ASan, and said in as many
words that *"a clean run of today's `DoublePrecisionScenario` does not discharge this"* — **NO CLAIM FOUND**.
`espryt-v3.md:362-366` reports `DoublePrecisionScenario` green, which is the run the review pre-emptively
refused as evidence.

**Fix:** an explicit integrator ruling (D7 → P8, recorded in `INTEGRATOR-DECISIONS.md` and in
`MEASUREMENTS.md`'s P3a section as a known behavioural delta), plus the named case package D still owes, or a
statement that the shape is unreachable in the corpus.

### M-4 — the stale-comment promotion condition at `Managers.cpp:2276-2282` is now met and the promotion did not happen

The comment says `NoArm` stays a warning rather than a stop because *"bits 7 and 8 are clear in every `.Handles`
itest lane today (`MG_IntegrationTest/CMakeLists.txt` pins `MOBILEGL_PIPE_PUSH=0x7f` …), which is
contract-review item 11 and package D's to re-pin … the stop can be promoted the moment those lanes carry an
explicit `0x1ff` arm."* Gates' M1 did exactly that: `MG_IntegrationTest/CMakeLists.txt:930` now pins `0x1ff`
beside `MOBILEGL_PIPE_LEGACY_MEMOS=0` for the Handles-arm knobs, and the four CSO lanes went to `0x1ff` /
`0x8000000000000 1ff`. So the sentence is false and the safety promotion it gated is unmade: a build with bit 8
clear and `LEGACY_MEMOS=0` still runs with **no vertex-input arm at all**, logging once
(`Managers.cpp:2343-2350`) and drawing through an unconfigured driver VAO.

**Fix:** correct the comment and promote `PipeSubsystemArmVerdict::NoArm` to the same `std::abort()` that
`ResolveEsprytSlotTablesArm` uses for bit 5 (`Managers.cpp:294-298`) — this is also `espryt-v2` D14's
"wants an explicit integrator ruling", still unruled.

### M-5 — four cross-package rows closed by nobody, carried into the phase exit

Not defects on this tree, but they are the "left for X that nobody closed" the review was asked to name; see
§5 for the full table. In severity order:

| row | declared | owner as assigned | reality |
|---|---|---|---|
| `delete_vertex_elements` producer + VAO CSO slot free | `client-v2.md:308-309`, `espryt-v3.md:605-607` | package C | **is C-1 above**; both packages misattributed it to the one backend where it already works |
| contract-review **m4** — the three pairing `static_assert`s use inconsistent right-hand sides | `contract-review-v1.md:136-142`, re-assigned to B by `wire-v2.md:194-201` | package B | B never mentions it; I confirmed the asymmetry survives at `PipeFill.cpp:933-947` (two of the three compare against `kMGPipeSubsystemVertexInput` directly rather than against `SubsystemForEmitter(...)`). Cosmetic, but it is the trap C.5 exists to prevent |
| gates **m10** — the DirectGLES buffer ABA has no positive control | `gates-v2.md:244, :369-371` | package C | never mentioned by C. Compounded: there **is no DirectGLES `.AbaControl` lane** (`MG_IntegrationTest/CMakeLists.txt:959-1011` registers six lanes; the buffer kind's corruption evidence is Magma-only) |
| wire **n6** — which counter G10 asserts against | `wire-v2.md:383-386` | package D | no document closes it, but **the code does**: `StorageBufferRegrowScenario` reads `PipeStats`' `mpr` out of the lane log (`Harness/PipeStatsWindow.h:26-28, :62-73`), not the applier's per-context member. Close it on the record |

---

## 3. Minor

| # | finding | file:line |
|---|---|---|
| m1 | Comment says the three vertex emitters *"still resolve to false today — their dirty bits map to no subsystem"*; they are mapped. | `MG_Impl/Pipe/PipeFill.cpp:1509-1512` vs `MG_Impl/Pipe/Tracker.h:145-148` |
| m2 | G5 doc drift after the tenth function landed: *"Extract the nine bodies"*, and `test.yml`'s *"nine named bodies" / "nine bodies extracted" / "three canned controls"*. | `scripts/p3a_untouched_regions.sh:266`; `.github/workflows/test.yml` (G5 step comments) |
| m3 | G7 attributes the trip with `grep -q IsBgra` over the **whole** ctest log rather than over the failing assertion, and traps only `EXIT` (no `INT`/`TERM`), so a Ctrl-C during the rebuild re-enters `repair`'s own build. Exact today (`IsBgra` appears once, at `MG_Test/Pipe/VertexInputEmitTest.cpp:266`); brittle to a future case name. | `scripts/p3a_vertex_input_negative_control.sh:174, :219` |
| m4 | `p3a_untouched_regions.sh` exits 1 under Git Bash on Windows: Python text-mode stdout emits CRLF and the `awk -v n="$name"` lookup then misses every row. Linux CI unaffected; `newline='\n'` fixes it. | `scripts/p3a_untouched_regions.sh` (python emit block) |
| m5 | G8's DirectGLES `.Handles` arm degrades to a **SKIP**, not a red, if `MGPipeResourceOps` is renamed — the arming marker is a CMake content grep. Honest (visible skip with a reason) but not a gate. | `MG_IntegrationTest/CMakeLists.txt:459-467`; `Scenarios/HandleRecycleScenario.cpp:390-407` |
| m6 | `MGPipeApplierReset` zeroes `MapPersistentRoundtrips` at every make-current while `PipeStats`' `mpr` is process-wide; the two observables disagree across a context switch. Latent (G10/G12 read `PipeStats`'), but this is what wire n6 asked to have written down. | `MG_Pipe/PipeApply.cpp:631` vs `MG_Impl/Pipe/PipeFill.cpp:735-737` |
| m7 | `MGPipeSlots()` is now mutated from `BufferObject`'s constructor and destructor, i.e. from whatever thread issues `glGenBuffers`/`glDeleteBuffers`. The "GL thread by construction" justification is stated but not enforced; two contexts of one share group on two threads is a torn free list. New for the `Buffer` kind. | `MG_Impl/Pipe/PipeFill.cpp:603-611, :741-763`; `ResourceTracker.h:287-299` |
| m8 | `ResolvedDrawBuffers::Entry::resource` is still a raw twin pointer that can dangle after `ReleaseByHandle`; both consumers check identity through `FindByHandle` first, so nothing is dereferenced, but the invariant is not written at the member (espryt-v3 §8's own carry-forward). | `MG_Backend/DirectGLES/Managers.h:979-1019` |

---

## 4. What I attacked and found sound (so the next reviewer does not repeat it)

**Handle lifecycle, end to end.** `BufferObject` ctor mints unconditionally and latches `Published`
(`BufferObject.cpp:44-45`, `ResourceTracker.h:342-350`) → `MGPipeApplyResourceCreate` starts the record over
rather than editing it, so a recycled slot cannot inherit a field (`PipeApply.cpp:938-956`) → espryt's
`GetOrCreate(handle)` refuses a **backwards** generation and resets the twin on a **forward** one
(`SlotTables.h:315-325`), with a release-build voice at `Managers.cpp:2368-2374` because `MOBILEGL_ASSERT`
compiles out at INFO → `~BufferObject` → `MGPipeEmitResourceDestroyAndFree` emits then frees, in that order
(`PipeFill.cpp:741-763`) → `Ops_H_Destroy` takes the twin **out** of the table before deciding pool/delete/defer
(`Managers.cpp:2084-2093`, `SlotTables.h:350-361`). I walked: (a) a handle whose twin was never created —
`ReleaseByHandle` returns an empty `BackendPtr` and `Ops_OnDestroy` fast-outs on `!resource`
(`Managers.cpp:1741`); (b) destroy while the ops table is unregistered — the applier clears the record, the
backend is not called, the slot is recycled, and the next `GetOrCreate` resets the orphaned twin whose
destructor skips the driver because `g_backendContextGeneration` moved; (c) the create/destroy latch
asymmetry — that is M-1, the only hole; (d) the process-teardown sentinel — armed on the handle overload too
(`SlotTables.h:299`), and the buffer's death path is byte-for-byte the legacy `Ops_OnDestroy`, so no new
teardown hazard. **The old twin is gone before the new create lands** in every ordering I could build.

**Base instance.** Three sites, all after `ConditionalRenderDiscardsCommand()` and immediately before `MGP_FILL`
(`GL_Drawing.cpp:662, :685, :715`); `Reset()` deliberately does **not** clear the pending value
(`Tracker.h:368-392`, B-C1 fixed); the value is mixed into bit 9's shutter so a base-instance-only change still
reaches the emitter (`Tracker.h:285-286`); it is a `ContentHash` input so the suppressor cannot eat it
(`VertexInputEmit.h:123-130, :233`); and it is cleared on **both** validate-point exits — the no-live-context
early return (`PipeFill.cpp:1443`) and the end of step 3 (`:1527`), the latter unconditionally, outside every
`wants()` gate. `DrawElementsIndirect` and the multi-draw tiering never set it, so they see 0; a plain draw
after a base-instanced one sees 0 and re-fires the shutter. The server resolves the shift
(`Managers.cpp:4043-4045`, `BaseInstanceByteShiftWire` at `:3666-3669`) and the client never learns the answer.

**Persistent map / readback / epoch.** The five-step order is implemented and named in place
(`Managers.cpp:2058-2081`): writeback → unmap → `syncedChangeSerial` stamp → epoch bump in
`Ops_H_ReadbackTracked`, and `MGPipeApplyResourceReadback` moves **no** serial (`PipeApply.cpp:1051-1053`).
The `hostBytes` base-vs-base+offset mismatch (client-review B-M3, ID-12 "espryt must change") **is closed by
reading both ends**: the client sends `base + offset` (`PipeFill.cpp:651-659`, `:701`) and espryt subtracts it
back — `hostBytes = bytes - offset` at `Managers.cpp:1921` and `bytes - start` at `:1969`. `OnBufferWriteback`'s
shape agrees on both sides: espryt passes `record.Offset` as the destination and `mapped` as the address
(`:2069-2072`), the client writes `WritebackFromBackend({addr, size}, offset)` (`ResourceTracker.h:582-585`).
`OnGpuWritten` is one whole-buffer range on both sides, asserted rather than assumed (`Managers.cpp:2436-2437`,
`ResourceTracker.h:599-602`), and the client's handler sets **both** `m_hasDefinedContent` and
`m_gpuWritePending` via `MarkGpuWritten()` (espryt-review m8's contract point — holds).
`Ops_H_MapPersistent` (`Managers.cpp:2102-2181`) is D-E-conformant: the two permitted substitutions plus one
**declared** third statement (`hostBytes = nullptr` at `:2170`, because `AdoptPersistentMap` frees the shadow) —
that third change is sound and is the C-2 fix, but it is a third change against D-E's "exactly two", so it
belongs in the phase-exit record against `ARCHITECTURE.md:466`.

**Serial / version coherence — one adversarial sequence per memo, all held.**
- `m_syncedVertexBuffersSerial`: A→B→A make-current with a stable per-activation call count would land on a
  stamped value if the reset zeroed the serial. `MGPipeApplierReset` does `++` (`PipeApply.cpp:645-646`). ✔
- `m_syncedElementsSerial` + `m_syncedElementsHandle`: after `MGPipeApplierReleaseObjectRecords` a re-created CSO
  at the same `{slot, gen}` restarts `ContentSerial` at 1, which would match a twin that had only ever seen one
  configuration. Rescued because that function **also** advances both global serials (`:658-659`), forcing
  `attributesDirty`. ✔ (The rescue is stated at `Managers.cpp:4020-4034`, which correctly names the rule as
  living in another package.)
- `m_syncedIndexSerial`: the two element-array restore scopes put the driver id back without touching the
  serial, which is right — the serial records what the *applier* said (`Managers.cpp:4188-4194`). ✔
- The fp64 stream memo: re-keyed on `{sourceHandle, applier Serial}` and never trusted for a persistently
  mapped source (`Managers.cpp:4508-4513`); a destroy drops the record whole and keeps `Gen`, and the successor
  gets `Gen+1`, so the handle compare covers slot recycling. ✔
- Suppressor reserved 0: a computed 0 is remapped to 1 (`SetHashSuppressor.h:62`), `Invalidate` writes 0, and
  `InvalidateAll()` is called in the `FreshlyPrimed` block **beside** `MGPipeApplierReset()` and the emitter's
  own `Reset()` (`PipeFill.cpp:1479-1494`) — without that trio a make-current would suppress the re-bind and
  the server would draw with no vertex elements bound. ✔
- The client's per-handle create/bind latch is keyed on `{slot, Gen, ConfigVersion}` (`VertexInputEmit.h:174-186,
  :304-308`), so ping-ponging two VAOs re-binds but never re-creates. ✔
- Records survive a make-current, per-draw state does not: `MGPipeApplierReset` clears the working half and
  keeps `Resources`/`VertexElementsCsos` (`PipeApply.cpp:607-631`), and the rule is stated in three places that
  agree (`PipeApply.h:196-219`, `ResourceTracker.h:465-492`, `Managers.cpp:4020-4034`). ✔

**Applier window bounds.** `MGPipeApplySetVertexBuffers` polices `Start + Count` (`PipeApply.cpp:1289-1303`);
the applier deliberately does not clear entries outside the window, and espryt bounds every read by
`VertexBufferStart/Count` **and** re-checks `BindingIndex == attributeIndex`
(`Managers.cpp:3647-3661` — the `13b380fe` fix; `glVertexAttribBinding(2,3)` was fetching the wrong entry).
The 32-slot walk (not `rec->AttributeCount`) is what keeps the disable arm alive after a shrinking
configuration, and it is independent of the client because `MGPipeApplyCreateVertexElements` zeroes both arrays
before unpacking (`:1225-1226`). ✔

**Subsystem masks.** Default `0x1ff` (`ConfigLoader.cpp:257`); bit 8 requires bit 7 and is refused **by name**
with a released `MGLOG_E` at `Managers.cpp:2326-2332`; `MOBILEGL_PIPE_LEGACY_MEMOS=0` compiles and the
`SyncToBackend` armless path says so (`Managers.cpp:3746-3750`). G13 is clean: `pGLContext` appears zero times
under `MG_Backend/`, and no `MG_State` type appears in the `MGPipeResourceOps` block. The contract payload
(`MGPVertexBuffers` 24 B, `F(BaseInstance)` in `PipeFields.def:99-100`) is already in the `5cb826b0` base, so
`git diff --stat -- MG_Pipe/generated PipeCalls.def PipeFields.def` over this range is legitimately empty.

**The gates, on the finished tree** (read; the cheap ones traced, none run in the shared tree):
G5 green and *correctly* green against `44c2b5cf` for the nine original functions, with a real `--self-test`
whose negative control for the tenth entry is non-vacuous — but see **C-2** for what the tenth entry protects.
G7 patches `IsBgra` at the one site that exists, restores + rebuilds on every exit path (trap armed *before* the
patcher), keeps its logs out of the repo, and its exit codes match the brief; it must be green-before and
compile-after, so an exit 0 for a different reason is well fenced (see m3 for the residue).
G8 has five `TEST_F`s and six registered lanes, the `.AbaControl` arm asserts the corruption with an explicit
"if this ever goes green-by-being-correct the reproducer has stopped reproducing" (`:287-297`), the new buffer
case at `:767` uses different colours at the same size/layout so a broken re-key reads red, and an unknown arm
`FAIL()`s instead of falling through — the two soft spots are m5 and M-5's missing DirectGLES `.AbaControl`.
G10 asserts 3 roundtrips for 3 storage definitions against 12 for per-draw, over a real `EndFrame`-delimited
`PipeStats` window, with `found` / `>= 0` guards that separate "no stats channel" from "counter read zero", and
its sibling `LargeArenaAdoption` pins `== 1` in a different window, so a stuck-at-3 counter cannot pass both.
G12 tests both directions in one case (`on` → 2, `off` → 0) **and** asserts identical pixels on both arms —
"the counters moved and the picture did not" — with `FAIL()` on an unknown lane.
G9's scan root, the `DirtySurface.def` regeneration and the `MG_State` widening are in the diff and clean.

**CI.** The TEMPORARY `feat/disaggregated` push trigger is present and untouched (`test.yml:9-12`). Nothing was
narrowed: the only `-R` change **broadens** `'HandleRecycle|CsoContentAddressing'` to
`'HandleRecycle|CsoContentAddressing|ResourceSubsystem|MapPersistentRoundtrip'` in `build-verify` (`:614-615`),
the only job that unpacks a push build; the singular `MapPersistentRoundtrip` deliberately catches both the
lane prefix and `…AnAdoptionCostsExactlyOneMapPersistentRoundtrip`. `VertexInputEmit` is a `unit` suite and runs
via `test.yml:570` in `build-verify`, which is the only configuration where its emission cases compile. Two new
G5 steps, branch-scoped to `feat/disaggregated` or `workflow_dispatch`, so they retire with the trigger. No
`LABELS`, `add_test`, `gtest_discover` or matrix entry was removed anywhere in the diff.

**Ownership-table "nobody" files that were touched.** `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp` — exactly the three
one-line `MGP_SET_BASE_INSTANCE` calls ID-10 granted and nothing else; the macro is pull-safe
(`PipeFill.h:117-123`) so the three sites compile in a pull build too. `MG_Test/Buffer/BufferTest.cpp` — the
integrator landed espryt-v3 §7's measured fix verbatim (`83302ca2`; `ScopedBackendOps` at `:1658-1675` now
scopes the pipe table alongside `BufferBackendOps`), which closes the 26 red cases; correct and minimal, and it
is itself evidence that the `Published` latch (client m12) was necessary. `MG_Test/Pipe/TrackerTest.cpp` —
touched by B with B-DEV-5's justification.

---

## 5. Closure table — every "left for X" across the ten result/review files

Legend: **CLOSED** = a later file claims it with a `file:line` I could check; **DECLINED** = declined with a
reason on the record; **DEFERRED** = explicitly a later phase's; **OPEN** = no claim found anywhere.
Rows marked ⚑ are the ones I verified against the code myself.

### 5.1 Closed, and verified where marked

| id | declared | closed at |
|---|---|---|
| contract-review **M1** (Coverage.def omits DispatchIndirect/Query) | `contract-review-v1.md:23-58` | `wire-v2.md:35, :103-111` — `Coverage.def:40-58` |
| contract-review **M2** ⚑ (`GetOrCreate(handle)` adopts a stale generation) | `:60-108` | `espryt-v2.md:126-145` → verified live at `SlotTables.h:315-325`, `Managers.cpp:2368-2374` |
| contract-review m1/m2/m3(A)/m6 | `:112-154` | `wire-v2.md:40-42`; `client-v2.md:200` |
| contract-review must-know 9 ⚑ (`Start+Count` policing) | `:337-340` | `PipeApply.cpp:1289-1303` |
| contract-review must-know 10/11 | `:341-347` | `client-v2.md:200`; `gates-v2.md:54-97` ⚑ (`CMakeLists.txt:930, 1045-1060`) |
| wire-review **C1b** ⚑ (refusals countable) | `wire-review-v1.md:496-497` | `PipeApply.h:220-229`, `PipeApply.cpp:476-490` |
| wire-review **C2** ⚑ (serials advance, never zero) | `:499-502` | `PipeApply.cpp:645-646`, `:658-659` |
| wire-review M-B/M-C/M-D/M-E, n1, n7 | `:505-514, :284-288, :304-310` | `wire-v2.md:36-44`; `client-v2.md:283-285` ⚑ (`BufferObject.cpp:435`) |
| client-review **B-C1** ⚑ (Reset ate the pending base instance) | `client-review-v1.md:22-56` | `Tracker.h:368-392`; the substitute clear at `PipeFill.cpp:1443` |
| client-review **B-C2** ⚑ (record re-publication) | `:58-97` | resolved per ID-12; rule at `ResourceTracker.h:465-492`, `PipeApply.h:196-219` |
| client-review **B-M1** ⚑ (the three ID-10 sites) | `:103-119` | `GL_Drawing.cpp:662, :685, :715` |
| client-review **B-M2** ⚑ (Acquire on content paths) | `:134-159` | `PipeFill.cpp:581-596` — content paths use `Find` |
| client-review **B-M3** ⚑ (hostBytes base vs base+offset) | `:161-178` | espryt was already conformant: `Managers.cpp:1921`, `:1969` subtract |
| client-review B-M4(a)/(b-NoteBoundAs)/secondary, m1-m6, m8-m12 | `:187-256` | `client-v2.md:151-210` |
| espryt-review **C-1** ⚑ (draw-clean vs a live host map) | `espryt-review-v1.md:38-101` | `Managers.cpp:2577` — the probe asks `frontend->IsMapped()` again |
| espryt-review **C-2** ⚑ (dangling hostBytes) | `:103-160` | `Managers.cpp:2170`, `:2637-2641`, `:2741` — **but see M-2 for the visibility half** |
| espryt-review **M-2** ⚑ (walk bounded by AttributeCount) | `:191-213` | `Managers.cpp:4054-4067` — all 32 slots |
| espryt-review **M-3** ⚑ (bit 8 without bit 7) | `:215-233` | `Managers.cpp:2326-2332` |
| espryt-review **M-5** ⚑ (memo depends on wire's C2) | `:254-281` | `Managers.cpp:4020-4034` |
| espryt-review m2 ⚑ (readback clamp past the offset) | `:291-294` | `Managers.cpp:2036-2040` |
| gates-review M1-M4, m1-m6 ⚑ | `gates-review-v1.md:25-210` | `gates-v2.md:54-210`; ID-11's ten at `scripts/p3a_untouched_regions.sh:76-77` |
| espryt-v3 §7 ⚑ (26 red `BufferTest` cases) | `espryt-v3.md:526-587` | **integrator landed it** as `83302ca2` — `MG_Test/Buffer/BufferTest.cpp:1658-1675` |
| wire **n6** (which counter G10 asserts) ⚑ | `wire-v2.md:383-386` | no document, but the code answers: `Harness/PipeStatsWindow.h:26-28` reads `PipeStats`' `mpr`. **Close it on the record** |

### 5.2 Declined with a reason (accepted)

wire n2/n3/n4/n5/n8/n9 (`wire-v2.md:393-401`); espryt-review m4 — `FlushPendingRangesFrom`'s extra null check
(`espryt-v2.md:216`, restated `espryt-v3.md:99-101`) — **note this is now one of C-2's three divergences**;
client-review B-M4(b)'s `RefreshBindMask` half and client m7 (`client-v2.md:173-181, :297-303`); gates m7/m9
(`gates-v2.md:241, :243`); contract-review m5.

### 5.3 Deferred to a later phase (accepted)

wire R1 → P5 (`MGPipeApplierReleaseObjectRecords` has no production caller); client B-M5 → the phase that gives
the backend an unmap hook (`PipeFill.cpp:718-726`) ⚑ — recorded correctly; espryt D7 → P8 (**but see M-3**);
espryt §5's three frontend reads → P5/P8; the vertex-elements CSO content-addressing → P7;
`PipeResource::m_backend` deletion → P13.

### 5.4 OPEN — no claim found anywhere in the ten

| # | row | declared | owner as assigned | my verdict |
|---|---|---|---|---|
| 1 | `delete_vertex_elements` producer + VAO CSO slot free | `client-v2.md:308-309`, `espryt-v3.md:605-607` | package C | ⚑ **This is C-1.** Misattributed: Espryt already frees; Magma does not. **Must fix** |
| 2 | espryt D12 / ID-13 — a push build has no `FlushPendingRangesNow` | `espryt-v3.md:75-106, :519-520` | integrator | ⚑ **This is C-2.** **Must rule + amend the script** |
| 3 | espryt-review post-rebase 3 — named C-2 cases (adopted `DoublePrecision`, ASan `StorageBufferRegrow`) | `espryt-review-v1.md:493-495` | package D | ⚑ still owed; the run espryt-v3 reports is the one the review pre-refused. Feeds M-3 |
| 4 | espryt-review post-rebase 2 — package D's itest for C-1 | `:489-492` | package D | still owed |
| 5 | espryt-review post-rebase 4 — `VertexArrayEnableDisableScenario` with a **shrinking** enabled set | `:496-497` | package D | the only gate that can see M-2's fix; in the lane regex but no shrinking-config claim |
| 6 | espryt-review post-rebase 6 — `LEGACY_MEMOS=0` + `PUSH=0x7f` must name a verdict | `:500-501` | integrator/D | ⚑ related to **M-4** |
| 7 | espryt D14 — the armless verdict diagnoses and does not stop | `espryt-v2.md:272-273` | integrator | ⚑ **M-4**; the promotion condition is now met |
| 8 | contract-review **m4** — the three pairing `static_assert`s | `contract-review-v1.md:136-142` → B | package B | ⚑ **M-5**; asymmetry survives at `PipeFill.cpp:933-947` |
| 9 | gates **m10** — the DirectGLES buffer ABA positive control | `gates-v2.md:244, :369-371` | package C | ⚑ **M-5**; and there is no DirectGLES `.AbaControl` lane at all |
| 10 | client §6.2 — the three `GL_Buffer.cpp` `NoteBoundAs` binder hooks | `client-v2.md:304-307` | **nobody** | ⚑ real but bounded: the two bits anything keys on are covered by `NoteBoundAs` at `VertexInputEmit.h:219, :267`. Carry to P4a |
| 11 | espryt m6 / post-rebase 10 — record the per-draw `FindByLifetimeId` cost in D.4 | `espryt-review-v1.md:306-312, :508-509` | package D | measurement, not a defect |
| 12 | espryt §4.9 — D.4.6's peak-RSS reading needs `retrace_gate.py` to grow | `espryt-v3.md:436-441` | package D | ⚑ **this is the measurement that would have caught C-1**; do it before the device runs |
| 13 | gates §5.2/§5.3/§5.4/§5.6/§5.7, gates m8, §6.10, §6.11 — re-capture `p3a-before-untouched.sha` with the ten-line listing; run G7 on a tree carrying B; the four counting entries must stop skipping; `build-bench`; `gh workflow run test.yml` | `gates-v2.md:353-371`; `gates-review-v1.md:214-216, :363-365` | integrator | ⚑ **all still owed**; G7 in particular has never been run on a tree carrying B |
| 14 | client-review §7.3 — the `Blob.Size` off-by-one control in a verify build | `client-review-v1.md:408-410` | integrator | still owed |
| 15 | client-review §7.7 — G10 non-zero and G12 behavioural on the merged tree | `:423-425` | integrator | ⚑ now provable: the lanes exist and are CI-reachable (`test.yml:614-615`). Run once and record |
| 16 | client-review §7.8 / espryt §7 — the `…ActuallyArmedWhenTheEnvironmentPinsItOn` shared-log race | `MG_IntegrationTest/CMakeLists.txt:1040` | integrator/D | still open; pre-existing, `-j 8` only |
| 17 | contract-review m3 (Magma half), m7; contract §5.5; wire §6.9; espryt §1, §2, §4.7 | various | integrator (docs/tooling) | doc + tooling corrections, none load-bearing; espryt §2's "the G5 script self-`cd`s, so a foreign-cwd run is a false pass" is worth taking |
| 18 | espryt §8 — the `ResolvedDrawBuffers::Entry::resource` invariant sentence | `espryt-v3.md:601-603` | a later pass | **m8** |
| 19 | client §6.5 — the `MOBILEGL_PIPE_POISON_OMIT` caselist sweep | `client-v2.md:311-312` | G15 | off the critical path, as declared |
| 20 | client-review §7.2 residue — `EveryAttributeFieldSurvivesTheWireConversion` as a full round trip | `:402-407` | integrator | declined with a reason by B (a second oracle would make G7's field-drop fail twice); accept |
| 21 | gates §1 — gates' own G1/G14 are measured against `5cb826b0`, not the final tree | `gates-v2.md:44-48` | integrator | ⚑ **must re-measure before the verdict**: G1 is a hard gate and the number on record predates espryt and gates |

---

## 6. The exact fix list

**Blocking (REWORK):**

1. **C-1.** Give `VertexElementsCso` a backend-neutral death path. Preferred shape: the client emits
   `delete_vertex_elements` and calls `MGPipeSlots().Free(MGPipeKind::VertexElementsCso, handle)` from
   `~VertexArrayObject` under `#if MOBILEGL_PIPE_PUSH`, mirroring `MGPipeEmitResourceDestroyAndFree`
   (`PipeFill.cpp:741-763`) — emit first, free second, latch "was it published" the same way. That closes
   `client-v2` §6.3, `espryt-v3` §8's carry-forward and the applier's missing `delete_vertex_elements` producer
   in one edit, and it makes Espryt's existing death notice a redundant second path rather than the only one.
   Add a case: create N VAOs, destroy them, assert `MGPipeSlots().HighWater(VertexElementsCso)` /
   `LiveCount` does not grow — and run it on the DirectVulkan lane, which is where it fails today.
2. **C-2.** Add `FlushPendingRangesFrom` to `scripts/p3a_untouched_regions.sh:76`
   (`EXPECTED_FUNCTION_COUNT=11`), capture its baseline sha at `3e298c9a`, and add it to
   `SELF_TEST_FUNCTIONS`. Record an **ID-14** that supersedes ID-13's "outside any `#if`" sentence with the
   two-arm shape the tree actually chose and the reason (`Managers.cpp:1127-1138`). Fix the "nine" wording in
   the script and `test.yml` while there (m2).
3. **M-1.** In `MGPipeEmitResourceRespecify` (`PipeFill.cpp:629`), emit the create first when
   `!tracker.WasPublished(handle)`.
4. **M-2.** Move the `hostBytes` store in `Ops_H_SubData` (`Managers.cpp:1921`) and `Ops_H_FlushRange`
   (`:1969`) inside the `pendingMutex` scope, or restrict it to the `CanTouchGLNow()` arm.
5. **M-4.** Correct `Managers.cpp:2276-2282` and promote `NoArm` to a stop for both P3a families, now that the
   lanes carry an explicit `0x1ff` arm.
6. **Re-measure G1 and G14 on `3e298c9a`** before the phase verdict (closure row 21). The numbers on record
   are gates' at `5cb826b0`; G1 is a hard gate with an empty admitted-resize set and it has not been read on
   the finished tree.
7. **Run G7 once on this tree** (`bash scripts/p3a_vertex_input_negative_control.sh build-push`) — closure row
   13. It has never been executed on a tree carrying package B, and it is the negative control for G6.

**Non-blocking, but before the `dev` merge:**

8. **M-3.** Record the fp64 `SyncGpuWrites` arm-divergence as a ruled deviation in
   `INTEGRATOR-DECISIONS.md` and `MEASUREMENTS.md`'s P3a section, or land package D's adopted-source
   `DoublePrecisionScenario` case (closure rows 3-5).
9. **M-5.** Close contract-review m4 (`PipeFill.cpp:933-947`), record wire n6 as answered by the code, and
   either add a DirectGLES `.AbaControl` lane or write down that the buffer ABA's corruption evidence is
   Magma-only.
10. Minors m1, m3, m4, m5, m6, m7, m8 as written above; and grow `retrace_gate.py`'s peak-RSS reading
    (closure row 12) — it is the measurement that would have caught C-1 on its own.
11. Remove the TEMPORARY `feat/disaggregated` trigger (`test.yml:9-12`) **at** the `dev` merge, not before —
    the two G5 steps are branch-scoped to it and retire together, which is intentional
    (`test.yml`, G5 step `if:`).
