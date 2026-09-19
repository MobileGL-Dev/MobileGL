# P3a package A `c1`-`c3` — adversarial review of the applier bodies (`wire-review-v1`)

Reviewed `db0c93bb` / `cede041d` / `e9b4a263` on `p3a/wire`, parent `39722687` (tag `p3a/contract`), worktree
`/home/swung/w7/p3a-wire` (not modified). Everything below was checked by opening the file at `e9b4a263`. A throwaway
`git worktree` at `/home/swung/w7/p3a-review-wire` was used for a clang push build and one negative control, and was
removed at the end.

## Verdict

**REWORK** — a small, fully-specified one. The three commits are well made: the record shapes are right, the serial
order is right and I verified it against the Espryt code it is written for, the dispatch is by handle only, G13 and
G1 hold, and the package is genuinely behaviour-neutral (nothing in the tree outside its own test calls a single one
of the fourteen entry points). Six of the eight declared deviations are sound as written.

What forces a rework round rather than "accept with minors" is three things, none of them large:

1. **The context-reset hole is bigger than the report scopes it, and the applier makes its consequence invisible**
   (C1). `MGPipeApplierReset()` does not run once per fresh context — it runs at **every make-current between two
   live contexts** (`Tracker.h:192-195`). The refusal it produces is a `MOBILEGL_ASSERT` that compiles out at INFO,
   which is the level all three gate builds use, and there is no counter, so a dropped `glBufferSubData` is
   unobservable in every build that ships or gates.
2. **Two of the three server-owned serials are reset to zero by that same call** (C2), which makes an `MGGen` go
   backwards — the exact wrap hazard D-G4 introduces them to retire, now with the identity patch deleted from the
   twin's side.
3. **The contract review's `M1` and its minors `m1`-`m4` are all still open**, in files `C.5` makes A-only, and the
   contract review said `M1` was A's to land on this branch.

Nothing here needs the tag re-cut, nothing here is an ABI or a signature change, and the c2/c3 coverage gap (M-B) is
new test code, not new applier code. The eight new ctest names, the three builds and the G1 zero-resize reading I
have no reason to doubt; where I could re-derive a claim independently I did, and it held.

---

## Findings

### C1 (critical) — `MGPipeApplierReset()` runs at every context switch, so the records `c1`/`c2` define are gone for any object that outlives one; and the refusal that follows is invisible in all three gate builds

`MG_Pipe/PipeApply.cpp:543-569` (the reset), `:427-441` (the verdict comment), `:494-500` and `:867-870`, `:907-910`,
`:938-941`, `:974-978`, `:1001-1006`, `:1027-1032` (the six refusal sites), `MG_Impl/Pipe/PipeFill.cpp:1188-1190`,
`MG_Impl/Pipe/Tracker.h:192-195` and `:352-355`.

The report's §3 A2 argues the no-op verdict from "`MGPipeApplierReset()` runs at the **first validate of a fresh
context**". That is true of `FreshlyPrimed()` but understates when it happens. `MGPipeTracker::Update` opens with

```cpp
if (m_context != &ctx) {          // Tracker.h:192-195
    Reset();
    m_context = &ctx;
}
```

so `Reset()` — and therefore `MGPipeApplierReset()` two frames up at `PipeFill.cpp:1190` — fires on **every change of
the current `GLContext`**, including a make-current back to a context that is still alive and whose objects are all
still there. `Tracker.h:322`'s own comment says as much ("Context teardown, server reset, a unit test's fixture"),
and `PipeApply.cpp:553-557` restates the "fresh context is a fresh server" reading without noticing that a
*returning* context is not fresh.

Failure scenario. A two-context application (a loader/worker context is the common shape, and MobileGL's own async
paths make one) alternates make-current. At each switch `g_applier.Resources` is emptied (`:558`). Every subsequent
`glBufferSubData` / `glBufferStorage` / `glFlushMappedBufferRange` on a share-group buffer created before the switch
reaches `FindResource` (`:415-420`), resolves to `nullptr`, and is **dropped**: no store, no dispatch, no serial. In
a shipped push build and in both gate builds the only trace is a `MOBILEGL_ASSERT` — inert at INFO, which is
`MOBILEGL_LOG_ACTIVE_LEVEL` in `build-push/CMakeCache.txt` — and there is no counter and no `MGLOG_E`. The write
never reaches Espryt, `IsBufferDrawClean` (re-keyed by D-A4 onto `record.Serial` and `record.Desc.Width`) has no
record to consult, and the frame draws stale bytes with nothing anywhere saying so.

Refutations attempted, and why each failed:

- *"The twin re-uploads from the shadow anyway."* Not after D-A4: the twin's inputs become the applier's record, and
  there is no record. The shadow still has the bytes, but nothing tells the backend to go and get them.
- *"This is the CSO store's own precedent — `bind_render_state` no-ops a dead CSO."* The precedent does not carry,
  because `MGPipeCsoCacheInstance().Reset()` runs on the line above (`PipeFill.cpp:1189`): the **client** forgets its
  CSOs at the same instant and re-creates them, so the two sides start over together. There is no client-side
  companion for buffers, and there cannot be one under D-A2: `ResourceCreate` is emitted from the `BufferObject`
  **constructor**, which a context switch does not re-run.
- *"It is unreachable today, nothing emits."* True of behaviour, and it is why the package is behaviour-neutral. It
  is not a refutation of the finding, because the record semantics are precisely what B's `ResourceTracker.h` and C's
  `IsBufferDrawClean` / `SyncToBackend` re-key are being written against right now, and the report's own §5 item 1
  says it must be settled before `p3a/client` lands.

Credit where it is due: A found the hole, named it in the file and escalated it in §5 item 1. What is missing is the
frequency and, more importantly, an **observable**. `ROADMAP.md:7` requires every gate to be able to go red for the
reason it exists; a mutation that is silently dropped in all three builds is the inverse of that.

Rework:
1. The reset's scope is decided **before** `p3a/client` lands, as §5 item 1 asks (re-emit create+respecify on first
   use after a reset, or take `Resources` / `VertexElementsCsos` out of the reset because they describe share-group
   objects). Record the decision in `INTEGRATOR-DECISIONS.md`, not only in a package report.
2. Whichever is chosen, make the refusal countable: a `Uint64 RefusedResourceCalls` (or a small per-call array) in
   `MGPipeApplierState`, bumped on every `FindResource`-returned-null path, with a `ResourceEmit` case pinning that a
   legal sequence leaves it at 0 and a refused one moves it. It costs one increment on a path that already returns.
3. Correct §3 A2's "at the first validate of a fresh context" to "at every change of the current context".

### C2 (critical) — `MGPipeApplierReset()` sends `VertexBuffersSerial` and `IndexBufferSerial` *backwards*, and both the code and the comment that defends it consider only the two wrong options

`MG_Pipe/PipeApply.cpp:565` and `:567` (`= 0`), with `:558-566` clearing the state those serials version;
`MG_Test/Pipe/VertexInputEmitTest.cpp:67-100` (the comment and the case that pin it); `Tracker.h:192-195` again for
the frequency; D-G4's twin table and `MG_Backend/DirectGLES/Managers.cpp:2452-2457` for what is being replaced.

D-G4 replaces `m_syncedIndexBufferVersion` (a wrapping `Uint16`) **plus** `m_syncedIndexBufferObject` (the identity
that closes the wrap hole) with `m_syncedIndexSerial` vs `IndexBufferSerial` — "one monotone `Uint64`, no wrap, no
identity patch". `ARCHITECTURE.md:44-49` defines an `MGGen` as monotone. This one is not: at every context switch it
returns to 0 and counts up again, with nothing beside it.

There are exactly three things a reset can do to a version whose data it is clearing, and only one of them is
right:

| | applier after reset | twin memo | first call after the switch | verdict |
|---|---|---|---|---|
| carry the count over | 42 | 42 | 42 == 42 → **clean** over cleared state | wrong immediately |
| zero it (what landed) | 0 | 42 | 1 != 42 → dirty … until the count climbs back to 42 | wrong later, and reliably |
| **advance it** | 43 | 42 | 43 != 42, and no stamped value can ever recur | right |

`VertexInputEmitTest.cpp:67-74` — landed at `c0`, and the only prose anywhere on the subject — argues only the first
row: *"A reset that carried a previous context's count over would let a twin believe it had synced a configuration
it has never seen."* That is true, and it is why zeroing looks like the answer. It is not: a counter that restarts
at 0 walks back through every value it has already handed out. `VertexInputEmitTest.cpp:94-95` then pins
`VertexBuffersSerial == 0` and `IndexBufferSerial == 0`, so the inversion is asserted, not merely written.

Failure scenario. Contexts A and B alternate. A `BackendVertexArrayObject` of context A survives the excursion —
`OnBackendContextDestroyed` (`Managers.cpp:1527-1540`) runs on destroy, not on make-current, and it does not touch
VAO twins at all, which have **no context-generation member** (`Managers.h:926-952`: `m_syncedConfigVersion`,
`m_syncedIndexBufferVersion`, `m_syncedIndexBufferObject`, `m_syncedBufferIdGeneration`, and nothing else). On the
next activation of A the counter climbs back through the value that twin last stamped; the first `set_index_buffer`
that lands on it reads clean and the driver keeps the previous element-array binding. Because each activation
restarts at 0 and does much the same work, landing on a previously-stamped value is not a coincidence — it is the
normal case for a context whose per-activation call count is stable.

Refutations attempted:

- *"The handle is compared beside the serial."* True for `ContentSerial`, which D-G4 pairs with
  `m_syncedElementsHandle` and whose per-record reset is unavoidable (the records are cleared) and safe (a recycled
  slot moves `Gen`). False for `VertexBuffersSerial` and `IndexBufferSerial`, which D-G4 defines as single
  per-applier `Uint64` compares with the identity patch explicitly deleted on the twin side.
- *"The twin dies with the context."* It does not; see above. `m_syncedBufferIdGeneration` catches a driver-id
  re-mint, not a serial that went backwards.
- *"Just don't reset them, then."* That is the first row of the table and is worse.

Fix (one line each, on this branch): at reset, **advance** the two global serials instead of zeroing them —
`++g_applier.VertexBuffersSerial; ++g_applier.IndexBufferSerial;` — so clearing the state is itself announced;
correct `VertexInputEmitTest.cpp:67-74`'s comment and turn `:94-95`'s two assertions into "moved forward" (the
**name** stays, which is all G14 protects); and add the sentence to §5's "Serial contract — for C".

(The reset body and that case landed at `c0`, and the contract review passed the reset as "clears exactly those
eleven". The three members were inert then; `c1`/`c2` are what give them meaning, which is why this is reviewable
here and why the fix belongs on this branch.)

### M-A (major) — contract-review **M1** is unaddressed: `Coverage.def`'s target split still omits `DispatchIndirect` and `Query`

`MG_Pipe/Coverage.def:41-47` at `e9b4a263`, unchanged from `39722687`. The five buckets still name 13 of the 15
`BufferTarget` enumerators; `DispatchIndirect` (a live backend read at `DirectGLES.cpp:725` and
`VulkanRenderer.cpp:7423`, and carried by no call — `MGPIndirectBuffers` is `DrawIndirect` + `Parameter` only) and
`Query` appear in no bucket, not even the "still pulled" one, while `:48-56` argues the row may stay one row
*because* the split is stated exhaustively here.

`contract-review-v1` classed this as a major and said "M1 is a comment-only fix that A can land on `p3a/wire`", and
that `Coverage.def` is A-only under `C.5`, so no later package may correct it. `git diff 39722687 HEAD --stat` names
three files and this is not one of them. Two comment lines, using the enum's own spelling so the list can be
cross-checked mechanically against `BufferObject.h:15-33`.

### M-B (major) — `c2` shipped 191 lines and two new `Fatal{ProtocolCorruption}` gates with **zero** test coverage in any tree

`PipeApply.cpp:1060-1226` against `MG_Test/Pipe/ResourceEmitTest.cpp` (nine cases, all resource-family) and
`VertexInputEmitTest.cpp` (still `c0`'s placeholder).

Nothing anywhere asserts: the `create_vertex_elements` count / `Blob.Size` refusal (`:1078-1092`), the
`set_vertex_buffers` `Start + Count` window (`:1170-1181`), `ContentSerial`'s re-create-keeps-counting vs
recycled-slot-starts-over rule (`:1104-1110`, `:1127`), the blob unpack itself (`:1113-1124`),
`BindVertexElements`'s null-handle and dead-handle arms (`:1133-1147`), `DeleteVertexElements` clearing a bound
handle (`:1162-1164`), or `SetIndexBuffer`'s independent serial (`:1213-1226`).

Concretely — and this was **measured, not argued** (see Experiment): I applied eight semantic mutations to
`PipeApply.cpp` in a throwaway worktree at `e9b4a263` — both `std::memcpy`s in `MGPipeApplyCreateVertexElements`
removed, `MGPipeApplySetVertexBuffers`' entry-copy loop removed, the `Start + Count` window gate removed, the
`Blob.Size` gate removed, both `BufferRangeFault` calls in `resource_flush_range` and `resource_readback` removed,
and `SubDataBoxFault`'s `Level` arm removed — rebuilt, and `ResourceEmitTest` reported **`[  PASSED  ] 9 tests.`**,
byte for byte what the unmutated build reports. `grep` over `MG_Test/` and `MG_IntegrationTest/` for the five
vertex-input entry points returns nothing, and `VertexInputEmitTest.cpp` still holds `c0`'s single placeholder
(`AResetApplierCarriesNoVertexInputStateOver`, `:75`), so there is no second suite that would have caught it. Two
gates this package introduced cannot go red in the package that introduced them (`ROADMAP.md:7`), and three of the
five gates in `c1` are in the same position.

The report defers these to B (§5 item 7, "the vertex-input applier's own cases — for B"). Unlike the splitter (A6,
which genuinely needs a file that does not exist on this branch), these need no emitter and no client file: they are
direct calls into `MGPipeApplyCreateVertexElements` / `…SetVertexBuffers` / `…SetIndexBuffer`, exactly the shape
`ResourceEmitTest.cpp`'s nine cases already use. The deferral is a choice, and it leaves `c2` unguarded through
D.1's `client` and `espryt` merges.

Rework: four cases, in `ResourceEmitTest.cpp` beside the existing ones or in `VertexInputEmitTest.cpp` (A owns
neither exclusively; `C.5` gives the latter's *contents* to B, so appending A's applier cases to `ResourceEmitTest`
and saying so in the header is the lower-collision choice) —
(a) a blob round trip over all 32 attribute and 32 binding-point slots, plus a shrink that leaves nothing of the
previous configuration; (b) the count/`Blob.Size` refusal in its forked-abort and log arms, like
`AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource`; (c) `Start + Count` at 32 and 33; (d) `ContentSerial`
counting up on a re-create and restarting at 1 on a recycled slot, plus `SetIndexBuffer` moving only
`IndexBufferSerial`.

### M-C (major) — the new `Fatal{PipeLiveHostWrites}` wire (deviation A4) has no negative control, and skips the one call P5 will attach its producer to

`PipeApply.cpp:479-488` (the wire), `:876` / `:520` / `:926` (its three call sites), `ResourceEmitTest.cpp:403-435`.

`ResourceEmit.NoResourcePathInThisPhaseLeavesHostWritesLive` asserts `HasLiveHostWrites == false` after nine calls
on a field that **no code in the tree writes**. Every one of those `EXPECT_FALSE`s passes with all fourteen applier
bodies deleted (only `MGPipeApplyResourceCreate`'s `RecordAt` has to survive, for `RecordOf`'s `EXPECT_GT`). The
true arm of the wire is never taken anywhere, in any build, so `Fatal{PipeLiveHostWrites}` is a gate that cannot go
red — which is the mistake the deviation's own justification says it is avoiding.

It is one line to fix: `MGPipeApplier().Resources[slot].HasLiveHostWrites = true;` (the accessor returns a non-const
reference) followed by the same forked-child assertion `AWriteOutsideTheDeclaredStorage…` already uses.

Second half: `PinNoLiveHostWrites` is called from `resource_respecify`, `ApplyBufferWrite` (both sub-data forms) and
`resource_flush_range`, but **not** from `MGPipeApplyMapPersistent` (`:1001-1025`) — and D-A4 / `ARCHITECTURE.md:481`
name the persistent-map push as exactly where P5 sets the flag. Add the call there (and to `resource_readback`,
which reads the store the flag describes).

### M-D (major) — `RecordAt` resizes a slot-indexed table on an unbounded, client-supplied slot index

`PipeApply.cpp:405-410`, reached from `MGPipeApplyResourceCreate` (`:855`) and `MGPipeApplyCreateVertexElements`
(`:1094`):

```cpp
if (slot >= records.size()) records.resize(static_cast<SizeT>(slot) + 1);
```

The only guard on the way in is `desc.Resource.Slot >= kMGPipeFirstAllocatableSlot` (`:843-846`, `:1061-1064`). A
`Slot` of `0xFFFFFFFE` therefore asks for a four-billion-entry vector of 88-byte records from inside the bounds
gate's own commit. This package polices `Blob.Size`, the destination range, `Level`, `RegionCount`, the null-bytes
case and `Start + Count`; the one number it does not police is the one that reaches an allocator.

`contract-review-v1`'s M2 recorded the identical shape against `BackendSlotTable::EntryAt` (`SlotTables.h:427-430`,
"does `m_slots.resize(slot + 1)` on a **client-supplied** slot index with no bound") and told C to bound-check at the
call site — noting in the same breath that "the applier's blob-bounds gate (`c1`/`c2`) does not sit between the
payload and this call for the resource family". The applier *is* the natural single place to do it: one bound, two
call sites, `Fatal{ProtocolCorruption}` with the same identity in the line as its five siblings.

Refutation attempted, and it failed: *"the slot space is dense, the handle comes out of `MGPipeSlots().Acquire`"* —
that is equally true of `AttributeCount`, `Start`, `Count` and `Blob.Size`, all of which this commit does gate; and
`ARCHITECTURE.md:119` reserves the tag for records that would make the server act outside its own storage, which an
unbounded `resize` does.

### M-E (major) — `MGPSubData::Blob` and `MGPVertexElements::Blob` are given opposite rules, and only one of them is written down

`PipeApply.cpp:1082-1085` (vertex elements: `desc.Blob.Size != declared` → `Fatal{ProtocolCorruption}`) against
`:494-535` and `:907-936` (sub-data, resident sub-data, flush range: `record.Blob` is never read at all), with
`MGPipeTypes.h:602-609` stating the same self-description for the buffer half — *"`Blob` holds exactly `Size` source
bytes"*.

So B is required to fill `MGPVertexElements::Blob.Size` exactly, on pain of an abort in every verify build
(correctly flagged in the report's §5 item 2), and is free to leave `MGPSubData::Blob` entirely zero, on three calls
that actually carry content today. The `c1` deliverable's own words in `C.0` are "the bounds gate that refuses a
blob outside its declared segment"; what landed refuses the *destination* range and never looks at the blob header
on the family that has one.

Refutation attempted: *"segments do not exist in P3a (D-N: no `MGHostSpan`, no `SEG_*`), so there is nothing to check
`Blob` against"* — that argument applies verbatim to `MGPVertexElements::Blob` and was not accepted there. Either
gate `MGPSubData::Blob.Size == MGPipeSubDataBufferSize(record)` when `Blob.Size != 0` (which stays inert while B
leaves it zero and becomes a real gate the moment a transport fills it), or state in the report and in
`MGPipeTypes.h` that the buffer half's `Blob` is deliberately unpoliced until P5/P8 and that B must not fill it.
Silence on one and a `Fatal` on the other is what B cannot act on.

### M-F (major) — contract-review must-know item 9 **is** addressed; the inherited minors `m1`-`m4` are **not**

Item 9 (`MGPipeApplySetVertexBuffers` must police `Start + Count`) is closed: `PipeApply.cpp:1170-1181` faults on
`Uint64{hdr.Start} + Uint64{hdr.Count} > kMGPipeMaxVertexAttribs` and on `Count != 0 && tail == nullptr`, with the
window's own numbers in the line. Good, and the `Uint64` widening means the addition cannot wrap.

The four A-owned minors are all untouched at `e9b4a263`, verified by opening each file:

| item | file:line | state |
|---|---|---|
| `m1` | `MG_Pipe/Coverage.def:187` | `X(GetBoundVertexArray, BindVertexElements)` still at position 2 of the otherwise `LC_ALL=C`-sorted `MGP_COVERAGE_EMITTED_LIST` |
| `m2` | `MG_Pipe/MGPipeTypes.h:257` | `MGPBlobRef Blob; // VertexAttribute[] followed by VertexBufferBindingPoint[]` — the two types that cannot travel, contradicting `MGPipeValueTypes.h:552-559` and `PipeApply.h:307-312` |
| `m3` (A's half) | `ConfigLoader.cpp:11` | include comment still reads "For `kMGPipeSubsystemsMigratedAtP2`, the push build's `PipePush` default" while `:256` uses `…AtP3a` |
| `m4` | `MG_Impl/Pipe/PipeFill.cpp:688-701` | the `NewVertexElements` assert compares against `SubsystemForEmitter(...)`; the `NewVertexBuffers` and `NewIndexBuffer` asserts against the literal `kMGPipeSubsystemVertexInput` |

`m5` (`PipeCatalogueTest.cpp:352-362`'s name) is recorded-only and needs nothing. `m3`'s `MagmaPipeArms.h:112-115`
half is the integrator's (no package owns `MG_Backend/DirectVulkan/**`). All four are one-line edits in files `C.5`
makes A-only, and the contract review requires them closed before D.1's `espryt` merge.

---

## Minor findings

- **n1 — a dead handle at `delete_vertex_elements` is the one refusal with no assertion and no reason in place.**
  `PipeApply.cpp:1152-1154` is a bare `if (record == nullptr) return;`, where its five siblings all carry
  `MOBILEGL_ASSERT(record != nullptr, …, kResourceRefusalNote)`. If the omission is deliberate — a death notice can
  legitimately name a CSO the applier lost at a context switch (C1) — say so; otherwise it is an inconsistency a
  reader will "fix" one way or the other.
- **n2 — `unmap_persistent` refuses on a dropped record** (`:1032-1036`), which is the one call where delivering it
  anyway is the safer answer: it releases a donation. Inert today (never emitted by P3a), worth a line.
- **n3 — `SubDataBoxFault` holds three of the convention's seven statements.** `:460-473` checks `UnionBox.X`,
  `Level` and `RegionCount`; `MGPipeTypes.h:602-605` also fixes `Y = Z = 0` and `H = D = 1`. Inert (nothing reads
  them for a buffer), but the helper's comment claims "the buffer half's convention held to itself".
- **n4 — `set_index_buffer` does not police `IndexSize`** (`:1213-1226`). D-I's three stated reasons cover `Res`
  (may be null) and `Offset` (the draw's); they do not cover `IndexSize`, which the backend uses as an element
  stride and which is `{0, 1, 2, 4}` and nothing else.
- **n5 — a `MGPVertexBuffer` entry's own `BindingIndex` is stored unchecked** (`:1197-1199`). Contract-review item
  10 puts the `>= 256` rejection on B before the `Uint8` narrowing on the attribute side; nothing polices the
  `Uint32` side server-side. Inert while C indexes `VertexBuffers[]` by attribute index (D-H3), which it does.
- **n6 — the applier's `MapPersistentRoundtrips` is per-context; PipeStats' `mpr` is not.** `:568` resets it and
  `ResourceEmitTest.cpp:576-577` pins that ("Per context, like every other member"), while D-B2 puts the counter G10
  actually greps in `PipeStats` at the **client** emitter, where nothing resets it. Two observables of one event
  that diverge across a context switch; say in §5 which one `StorageBufferRegrowScenario` asserts against.
- **n7 — the resident-sub-data drop claims a fallback it cannot deliver.** `:526-533` drops the call when
  `SubDataResident` is null, after `++stored->Serial`, with the comment "the write is landed by its ordinary
  sub-data route instead". That fallback is `BufferObject::LandBytesIntoResidentStore`
  (`MG_State/GLState/BufferState/BufferObject.cpp:371-384`) and it is unreachable once the call has been emitted, so
  the bytes are lost. Unreachable in P3a (Espryt registers all nine members; Magma registers no table at all, so
  `MGPipeResourceSubsystemEnabled()` is false there) — but §5 must tell B that the client's dispatch gate is
  `MGPipeGetResourceOps()->SubDataResident != nullptr`, not `MGPipeResourceSubsystemEnabled()` alone.
- **n8 — the by-handle assertion is half made.** `ResourceEmitTest.cpp:504-526` checks `g_spy.LastHandle` after
  `Create` and after `Destroy` only; `SubData`, `SubDataResident`, `FlushRange`, `Readback` and `MapPersistent` are
  counted but their handle is not asserted, so a dispatch that handed the backend the wrong handle would pass the
  case whose title is "DispatchesByHandle".
- **n9 — `ARespecifyReplaces…` re-pins `MGPipeResourceRespecifyNeedsAck`** (`ResourceEmitTest.cpp:333-334`), which
  `PipeCatalogueTest.ResourceRespecifyAcksOnlyImmutableStorage` already pins per the contract review's D-A5 row.
  Harmless; recorded so nobody deletes the wrong one.
- **n10 — `PipeApply.h` was edited after the `p3a/contract` tag.** Comment-only and declared (A1), and `C.5` makes
  the file A's, so this is not an ownership break. But `C.0` says nothing in `c0` may change after the tag "without
  telling the other three packages", and C's tree is branched **from the tag**: it will not see the corrected
  `VertexFetchBaseInstance` comment — the one that tells C the value is raw — until it rebases. The telling has to
  be an actual message from the integrator, not a line in a result file.

---

## Dimension-by-dimension

**(1) Semantics vs D-A / D-G4 / D-H / D-I.**

- *Record updates.* `resource_create` starts the record over (`:850-857`) rather than editing it, keeps only `Gen`
  from the handle, and leaves `Serial` at 0 — correct, and the reason given (a recycled slot must inherit no
  `Width`, no `Serial`, no `Immutable`) is the right one. `resource_respecify` replaces `Desc` whole (`:878`), which
  is what D-A2's "replaces the stored descriptor" means and what `ARespecifyReplaces…` pins with a shrinking
  immutable store. `resource_destroy` drops the record and keeps `Gen` (`:988-990`), matching D-L's rule that the
  bump belongs to the next handout, and does it **before** the backend hook (`:996`), which is the ordering its
  comment claims. The three content calls store nothing, correctly.
- *Serial rules, and the report's "bump before the backend hook".* **Verified against the code it is written for,
  and it is right.** Espryt stamps its synced serial *inside* the hook at `Managers.cpp:925` (`RespecifyStorageNow`,
  reached from `Ops_Respecify`), `:1282` and `:1294` (`Ops_SubData`'s adopted and kill-switch arms), `:1334`,
  `:1369`, `:1376` (`Ops_FlushMappedRange`) and `:1414` (`Ops_ReadbackFromGpu`), and `IsBufferDrawClean:1599`
  compares that stamp against the frontend serial. Under D-A4 the stamp becomes `record.Serial`, so a bump after the
  hook would leave the twin one mutation behind on every landed write. `PipeApply.cpp:518-522`, `:879`, `:927` all
  bump first. The arms that do *not* stamp (the queue-only sub-data path `:1295-1296`, `Ops_Respecify`'s three early
  returns) leave the twin dirty, which is the safe direction. ✔
- *`ContentSerial`.* `:1104-1110` + `:1127`: an existing record of the same identity keeps counting, a different
  `Gen` starts over, first create lands on 1 so 0 means "never created". Correct for D-G4's
  `m_syncedElementsHandle` + `m_syncedElementsSerial` pair, since the handle half covers the restart. The two
  arrays are zeroed before the unpack (`:1113-1114`), so a shrinking configuration leaves nothing behind — a real
  bug avoided, and untested (M-B).
- *Blob bounds gate and `Fatal{ProtocolCorruption}` identity.* `:1078-1092` faults on both counts `>` 32, on
  `Blob.Size != AttributeCount*24 + BindingPointCount*16`, and on a non-empty blob with no bytes; the line carries
  `{slot, gen}`, both counts, both byte lengths. The tag is `ARCHITECTURE.md:119`'s and is spelled through
  `MGP_TRIP_WIRE_TAG`, so it reads `Fatal{ProtocolCorruption}` only in poison/verify and plain in a shipped push
  build — matching the file's existing five wires exactly. The two sizes are right: `MGPVertexAttribWire` is 24 and
  `MGPVertexBindingPointWire` is 16, pinned at `MGPipeValueTypes.h:623-628`, and the applier uses `sizeof` rather
  than literals, so they cannot drift. **Does the emitter side produce exactly `Blob.Size`?** Nothing produces it
  yet — `MGPipeSetSubDataBufferRange` has no `create_vertex_elements` counterpart, and there is no helper that fills
  `MGPVertexElements::Blob` — so B has to compute `A*sizeof(MGPVertexAttribWire) + B*sizeof(MGPVertexBindingPointWire)`
  by hand at the emitter. That is the asymmetry M-E is about, and the report's §5 item 2 is the right warning but
  only half the rule.
- *`SetVertexBuffers` Start+Count.* Policed (M-F). ✔
- *`VertexFetchBaseInstance` raw storage.* `:1209` stores `hdr.BaseInstance` unresolved. Sound; see deviation A1.
- *`SetIndexBuffer` independence from the VAO config version (D5, `ARCHITECTURE.md:99`).* `:1213-1226` stores the
  record verbatim and bumps only `IndexBufferSerial`; it touches neither `BoundVertexElements` nor any
  `ContentSerial`. ✔ (C2 is about that serial's reset, not its independence.)
- *`DeleteVertexElements` on a bound record.* `:1149-1165` drops the record whole, keeps `Gen`, and clears
  `BoundVertexElements` when it named this handle — using `MGPipeHandle`'s `operator==` (`MGPipeHandles.h:58-60`),
  so both `Slot` and `Gen` must match. ✔ Untested (M-B).
- *`MGPipeApplierReset` clearing everything.* All eleven P3a members are cleared (`:558-568`), and the op table
  deliberately is not, which `TheResourceOpTableIsUnregisteredUntilABackendInstallsOne` pins. The *content* of that
  clearing is C1/C2.

**(2) Behaviour neutrality — yes, and I checked it rather than took it.** `grep` over `MobileGL/` for all fourteen
entry points returns hits only in `MG_Test/Pipe/ResourceEmitTest.cpp`; `MGPipeSetResourceOps` / `MGPipeGetResourceOps`
likewise. So no production path reaches a single body, `g_resourceOps` is `nullptr` in every build, and the frontend
still dispatches `BufferBackendOps` verbatim. No new side effect is therefore *reachable* in a push or verify build.
For when they are reached: there is one allocation, `RecordAt`'s amortised `resize`, on create only, never per draw;
`create_vertex_elements` zeroes 1280 bytes per configuration change, which D-G2 explicitly budgets as
once-per-configuration; `set_vertex_buffers` and `set_index_buffer` allocate nothing; no timer anywhere
(`ROADMAP.md:7`). The stats counter (`:1008`) is a `Uint64` increment in the applier's own state, not a
`PipeStats` call, so it costs nothing when stats are off. On **`Fatal` on legal sequences**: the five bounds wires
can only fire on a record that contradicts itself, which is not a legal sequence — but the *dead-handle* verdict is
the mirror image of that question and is C1: the no-op does hide a genuine protocol error a split build would need
to see, and the report's stated reason (a legal context-reset sequence) is real but far more frequent than stated,
which is why the answer is "keep the no-op, add the counter" rather than "make it Fatal".

**(3) The reverse channel's applier side (D-D) — satisfied.** `MGPipeApplyResourceReadback` (`:938-972`) calls
`ops->Readback` synchronously and does nothing after it returns, so C is free to run the whole of D-D's five steps
inside the hook in order: writeback → unmap → stamp `syncedChangeSerial` → `BumpBufferMutationEpoch()`. Nothing in
the applier can reorder them and nothing bumps an epoch on this side. The applier deliberately does **not** move
`Serial` on a readback, and that is right for the wrong-sounding reason: today `Ops_ReadbackFromGpu:1414` stamps the
frontend serial *after* `WritebackFromBackend` precisely because the writeback moved it
(`BufferObject.cpp:305-311` bumps `m_changeSerial`); under P3a neither side moves, the stamp is a no-op, and the
buffer still reads clean afterwards — the same outcome by a shorter path. The one thing the applier cannot do is
police the ordering, and it does not pretend to.

**(4) Persistent map (D-E, D-B) — correct, and it touches nothing it must not.** `MGPipeApplyMapPersistent`
(`:1001-1025`) counts first and unconditionally (D-B2's "attempts, mint *or* decline"), resolves the handle, moves
no serial and no descriptor, and returns `nullptr` for a decline — with the decline reached three ways (no table,
null member, dead handle), all of which are the caller's existing branch. `MGPipeApplyUnmapPersistent`
(`:1027-1041`) is the pair. The signature it dispatches through is D-A1's verbatim, which is the *only* thing D-E
allows to change about `Ops_AcquirePersistentMap`; nothing here constrains the four-way capability gate, the
stale-generation wipe order, the idempotency hit, `++g_bufferBackendIdGeneration`, `immutableStorage = true`, the
`MGLOG_E_ONCE` decline or the success stamps. `AcquirePersistentMap` semantics are untouched. ✔ (n6 and M-C's second
half are the two loose ends.)

**(5) G1, closure, purity.** `grep -c 'MG_State' MG_Pipe/PipeApply.h` → **0**; `grep -rc 'pGLContext'
MobileGL/MG_Backend | grep -v ':0$'` → empty. No type from `MG_State` appears in `MGPipeResourceOps` or in any of
the fourteen signatures — every parameter is an `MGPipeHandle`, an `MGP*` payload or a `const void*`. `PipeApply.h`
includes exactly what it included at the tag (`Includes.h`, `MGPipeRenderStateSpans.h`, `MGPipeTypes.h`); the `.cpp`
adds no include (`std::memcpy`'s `<cstring>` was already there at `:21`), so `MG_Pipe` gains no dependency on
`MG_Impl` or `MG_State` and the closure result is unchanged by construction. Everything `c1`/`c2` add is inside
`PipeApply.cpp`, which `CMakeLists.txt:482-493` appends to `SOURCE_FILES` only under push; `c3` is a test
executable. The reported `27799 → 27799, 0 resized` is consistent with that and I have no reason to doubt it.

**(6) Tests.** The five things `C.0` asks `c3` for are all pinned, and the range-encoding case pins both bounds
exactly, one byte either side, plus "a refused encoding leaves the record untouched" — which is the property the
splitter needs and is the right thing to keep on this branch (A6). The lifecycle cases are not vacuous: deleting
`FindResource`'s `Live` test or its `Gen` test turns `ADestroyDropsTheRecordAndAStaleGenerationResolvesToNothing`
red at `:377-379` and `:392-395`, and deleting the create's `record = MGPipeResourceRecord{}` turns it red at
`:369-371`. `EveryResourceCallDispatchesByHandleThroughTheInstalledTableOnly` is the strongest case in the file: it
proves the null-table fall-through, the install, the per-hook arrival, the uninstall, and that the records keep
moving through all of it.

Named cases that **would pass on a broken applier**:

- `NoResourcePathInThisPhaseLeavesHostWritesLive` — passes with every body deleted but `create`'s `RecordAt`; see
  M-C.
- `TheSubDataRangeEncodingRefusesExactlyAtItsTwoBounds` — passes with both `c1` and `c2` deleted entirely; it tests
  two inline functions of `MGPipeTypes.h`. Deliberate (A5) and worth having, but it is one of the eight new names
  and it is not a test of this branch.
- `AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource` — covers **one** fault on **one** call. Deleting
  `BufferRangeFault`'s call from `MGPipeApplyResourceFlushRange` (`:915`) and from `MGPipeApplyResourceReadback`
  (`:944`), and `SubDataBoxFault`'s `Level` arm (`:464`), leaves the suite green — measured, see Experiment.
- Everything in `c2`: five entry points, two gates, no assertion anywhere. Measured, see M-B and Experiment.

**(7) Commit hygiene — clean.** Three commits, three single-line messages **byte-identical to `C.0`'s**, empty
bodies (`git log --format='%b'` prints nothing for all three), no `Co-Authored-By` and no generator line, author and
committer `Swung0x48 <swung0x48@outlook.com>`. `git diff --stat 39722687 HEAD` names exactly
`MG_Pipe/PipeApply.{h,cpp}` and `MG_Test/Pipe/ResourceEmitTest.cpp`, all three A's under `C.5`; working tree clean;
nothing pushed. The one nit is n10 (a post-tag edit to a contract file, declared, comment-only).

---

## Judgement on the eight declared deviations

| # | judgement |
|---|---|
| **A1** `VertexFetchBaseInstance` stores the raw value | **Sound, and it is the only resolution available.** D-H2.2 says "Espryt's applier computes `fetchBaseInstance = UseNativeBaseInstance() ? … ` and stores it in `MGPipeApplierState::VertexFetchBaseInstance`", but `MGPipeApplierState` lives in `MG_Pipe`, `UseNativeBaseInstance()` is a `MG_Backend` capability, and G13 greps for exactly that reach. The brief is internally inconsistent and A resolved it the right way: the value is raw, C's applier arm computes `BaseInstanceByteShift` out of each attribute's own stride and divisor, which is equivalent to today's `EmulatedFetchBaseInstance` at `DirectGLES.cpp:5128`. The comment-only header fix is the minimum change. The integrator owes D-H2.2 the correction (report §5 item 8) **and** owes C the message (n10). |
| **A2** dead / unknown handle → defined no-op + debug assert | **The class is right; the implementation and the stated reason are not.** A no-op rather than `Fatal` is correct — a wire that aborts a verify lane on a legal sequence is the failure `ROADMAP.md:7` names. But the reason is stated as a corner case ("a shared store, a worker context") when `Tracker.h:192-195` makes it every make-current, and the refusal has no observable in any of the three gate builds. See C1. |
| **A3** `MGPResourceDesc::Target` not gated | **Sound, and verified.** `MGPipeTypes.h:152`'s value set ("Buffer \| Tex1D.. \| Renderbuffer \| TexBuffer") has no minted enumerator anywhere — `TextureTarget` has no `Buffer` — so a gate here would be a guess handed to B as a contract. The call identifies the family, which is what `PipeCalls.def` already says, and the box convention is what is checked. |
| **A4** a second trip-wire tag, `Fatal{PipeLiveHostWrites}` | **Sound in principle, incomplete in fact.** The reasoning is right (`MOBILEGL_ASSERT` is inert at INFO, which `build-push/CMakeCache.txt` confirms is the gate level, so D-A4's "a `MOBILEGL_PIPE_VERIFY` assertion pins that it is false" needs a real wire). But the wire has no negative control and skips `map_persistent`. See M-C. |
| **A5** one of the eight cases is not push-gated | **Sound.** `MGPipeSetSubDataBufferRange` / `…BufferOffset` / `…BufferSize` are `inline` functions of `MGPipeTypes.h` and exist in a pull TU, so the case runs rather than skips in all three trees. `ctest -N` names stay identical between the pull and push trees either way (G2's name half is about the *name set*, not about which arm runs), and the bound is pinned for the transport that will read it. |
| **A6** the splitter's case is B's | **Sound.** `MG_Impl/Pipe/ResourceTracker.h` does not exist on this branch and `C.5` gives it to B, so the split cannot be driven here. What `c3` pins instead is exactly the splitter's precondition — both bounds, and that a refused encoding leaves the record byte-identical, so the emitter can split against the record it just tried. The hand-off is written into the file header and into §5 item 6. The integrator should record the re-assignment explicitly, since `C.0`'s `c3` line asks for it by name. |
| **A7** G5 could not run | **Sound, and verified independently.** `git diff --stat 39722687 HEAD` names three files and `Managers.cpp` is not among them, so the nine G5 functions are byte-identical by construction, script or no script. |
| **A8** `check_include_closure.py --compiler clang++-20` | **Sound**, already recorded at `c0`; the no-argument form and `--compiler clang++` were both run. |

---

## Experiment — the suite's own negative control, run

A throwaway worktree `/home/swung/w7/p3a-review-wire` at `e9b4a263`, configured to match `p3a-wire`'s own
`build-push` as read out of its `CMakeCache.txt` (`Release`, `/usr/sbin/clang++`, `MOBILEGL_PIPE_PUSH=ON`,
`MOBILEGL_PIPE_VERIFY=OFF`, `MOBILEGL_FORCE_RELEASE_OPT=ON`, `MOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_INFO`,
integration tests and benchmark OFF, `CCACHE_BASEDIR=/home/swung/w7`).

**Baseline** — `ResourceEmitTest` at `e9b4a263`: `[  PASSED  ] 9 tests.`, all nine named cases OK. So the report's
push-arm result reproduces.

**Broken-applier control** — eight semantic mutations, applied together, all inside `PipeApply.cpp`:

| # | mutation |
|---|---|
| 1, 2 | both `std::memcpy`s removed from `MGPipeApplyCreateVertexElements` (`:1119`, `:1123`) — nothing is unpacked |
| 3 | `MGPipeApplySetVertexBuffers`' `for` loop removed (`:1197-1199`) — no entry is ever stored |
| 4 | the `Start + Count` window gate disabled (`:1173`) |
| 5 | the `Blob.Size != declared` gate disabled (`:1084`) |
| 6, 7 | `BufferRangeFault` removed from `MGPipeApplyResourceFlushRange` (`:915`) and `MGPipeApplyResourceReadback` (`:944`) |
| 8 | `SubDataBoxFault`'s `Level != 0` arm removed (`:464`) |

Rebuilt (rc 0) and re-run: **`[  PASSED  ] 9 tests.`** — identical output, every case still OK, including
`AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource` (it drives `resource_subdata`, whose gate mutation 6/7
did not touch). That is the evidence behind M-B and behind the "would pass on a broken applier" list under (6).

The worktree was removed with `git worktree remove --force`, `git worktree prune` run, and its three build logs
deleted. `/home/swung/w7/p3a-wire` is clean at `e9b4a263`; no file in it was modified.

---

## The exact rework list

**Blocking (this branch, before `p3a/client` is merged in D.1):**

1. **C1a** — settle the reset scope with the integrator and record it in `INTEGRATOR-DECISIONS.md`, not only in a
   package report (`PipeApply.cpp:543-569`, `PipeFill.cpp:1188-1190`, `Tracker.h:192-195`).
2. **C1b** — make the refusal countable: a refusal counter in `MGPipeApplierState` bumped on every
   `FindResource`-null path, plus a `ResourceEmit` case pinning 0 on a legal sequence and non-zero on a refused one.
3. **C1c** — correct §3 A2's "the first validate of a fresh context" to "every change of the current context".
4. **C2** — at reset, advance `VertexBuffersSerial` and `IndexBufferSerial` instead of zeroing them
   (`PipeApply.cpp:565`, `:567`); correct `VertexInputEmitTest.cpp:67-74`'s comment and turn `:94-95`'s two
   assertions into "moved forward" (the case's **name** is unchanged, which is all G14 protects); and add the
   sentence to §5's serial contract for C.
5. **M-A** — close contract-review `M1`: two comment lines in `Coverage.def:41-47` putting `DispatchIndirect` and
   `Query` in the still-pulled bucket with their reason, in `BufferObject.h:15-33`'s own spelling.
6. **M-B** — four applier-side cases for `c2` (blob round trip + shrink; the count/`Blob.Size` refusal in both
   build arms; `Start + Count` at 32 and 33; `ContentSerial` re-create vs recycled slot, and `SetIndexBuffer`
   moving only its own serial).
7. **M-C** — a negative control that sets `HasLiveHostWrites` and observes `Fatal{PipeLiveHostWrites}`, and a
   `PinNoLiveHostWrites` call in `MGPipeApplyMapPersistent` (and `…ResourceReadback`).
8. **M-D** — bound `handle.Slot` before `RecordAt` resizes on it (`PipeApply.cpp:405-410`, `:855`, `:1094`), with
   the same `Fatal{ProtocolCorruption}` identity line as its siblings.
9. **M-E** — decide `MGPSubData::Blob`: either gate `Blob.Size` when non-zero, or state in `MGPipeTypes.h` and in
   §5 that the buffer half's `Blob` is unpoliced until P5/P8 and that B must not fill it. One rule, written once,
   for both blob-carrying families.

**Inherited from `contract-review-v1`, A-owned, due before D.1's `espryt` merge:**

10. **m1** — re-sort `Coverage.def:187` into the `LC_ALL=C` order of `MGP_COVERAGE_EMITTED_LIST`.
11. **m2** — `MGPipeTypes.h:257`: name the wire views, not `VertexAttribute[]` / `VertexBufferBindingPoint[]`.
12. **m3** — `ConfigLoader.cpp:11`: `kMGPipeSubsystemsMigratedAtP2` → `…AtP3a`. (`MagmaPipeArms.h:112-115` is the
    integrator's; no package owns `MG_Backend/DirectVulkan/**`.)
13. **m4** — `PipeFill.cpp:688-701`: give all three pairing `static_assert`s the same right-hand side.

**Declared minors, non-blocking** (ID-4): n1-n10 above. n7 and n6 should nonetheless be added to the report's §5
list for B, because they are cross-package and nothing else will say them.

**Report corrections owed by A:** §3 A2's frequency (C1c); §5 item 5's serial contract (C2); §5 gains n6 (the two
roundtrip counters diverge across a context switch) and n7 (B's resident dispatch must gate on the op-table member,
not on the subsystem alone).
