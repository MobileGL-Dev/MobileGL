# P3a package A, rework round 2 — the applier bodies (`wire-v2`)

Branch `p3a/wire`, worktree `/home/swung/w7/p3a-wire`, on top of `e9b4a263` (the three landed commits are untouched).
**Not pushed.** Three new commits, seven files, +2038 / -85 against the tag `39722687`.

| commit | message (single line, empty body, no attribution) |
|---|---|
| `cdc42379f6...` (`cdc42379`) | `[Fix] (Pipe): account for DispatchIndirect and Query in Coverage.def's target-by-target split, re-sort the new emitted row, and name the P3a default mask in ConfigLoader's include comment` |
| `761faca1...` (`761faca1`) | `[Fix] (Pipe): give both blob-carrying record families one Blob rule, and name the wire views the vertex-elements blob actually carries` |
| `48fe55be...` (`48fe55be`) | `[Fix, Test] (Pipe): a make-current is not a teardown - the applier keeps its object records across one, counts every call refused on a record it does not have, advances the two vertex-input serials instead of restarting them at 0, and bounds the slot it grows a record table on` |

```
 MobileGL/ConfigLoader.cpp                     |    5 +-
 MobileGL/MG_Pipe/Coverage.def                 |   33 +-
 MobileGL/MG_Pipe/MGPipeTypes.h                |   32 +-      (comments only)
 MobileGL/MG_Pipe/PipeApply.cpp                |  683 ++++++-
 MobileGL/MG_Pipe/PipeApply.h                  |  104 ++-
 MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp    | 1232 +++++++++++-
 MobileGL/MG_Test/Pipe/VertexInputEmitTest.cpp |   34 +-
```

**The granted `Tracker.h` waiver was not needed and not used.** The fix lives entirely inside A's files: the call
site at `MG_Impl/Pipe/Tracker.h:192-195` and `MG_Impl/Pipe/PipeFill.cpp:1188-1193` are byte-identical to `e9b4a263`,
`git diff 39722687..HEAD --stat` names no file of B's, and B's rebase sees no edit in `Tracker.h` or `PipeFill.cpp`.
One thing B *does* have to know is in §6.

---

## 1. Per-finding disposition

| finding | disposition | where |
|---|---|---|
| **C1** reset scope + invisible refusal | **fixed** — the records are out of the reset; the refusal is counted, named and driven | `PipeApply.h:202-229`, `:266-300`; `PipeApply.cpp:439-490`, `:597-660` |
| **C2** serials sent backwards | **fixed** — `++` at every reset, never 0 | `PipeApply.cpp:628-646`, `:649-660`; `VertexInputEmitTest.cpp:67-110` |
| **M-A** / contract `M1` `Coverage.def` split | **fixed** — all 15 `BufferTarget` enumerators, in the enum's own spelling | `Coverage.def:40-58` |
| **M-B** `c2`'s zero coverage | **fixed** — 9 new cases; all 8 review mutations measured red (§4) | `ResourceEmitTest.cpp:788-1330` |
| **M-C** `Fatal{PipeLiveHostWrites}` control + `map_persistent` | **fixed** — pinned on `map_persistent` and `resource_readback`, with a negative control and a positive one | `PipeApply.cpp:1049`, `:1110`; `ResourceEmitTest.cpp:1218-1281` |
| **M-D** unbounded `RecordAt` | **fixed** — two named bounds, `Fatal{ProtocolCorruption}`, never a resize | `PipeApply.h:112-113`; `PipeApply.cpp:416-421`, `:906-914`, `:1200-1210` |
| **M-E** the two `Blob` rules | **fixed** — ONE rule, stated on both records and enforced on both | `MGPipeTypes.h:254-265`, `:624-632`; `PipeApply.cpp:516-525`, `:1171-1188` |
| **m1** emitted-row order | **fixed** | `Coverage.def:202` |
| **m2** `Blob` comment names frontend types | **fixed** | `MGPipeTypes.h:270-275` |
| **m3** stale default-mask comment (A's half) | **fixed** | `ConfigLoader.cpp:11-13` |
| **m4** `static_assert` form | **NOT A's** — see §1.9 | `MG_Impl/Pipe/PipeFill.cpp:687-701` |
| **n1** bare `return` at `delete_vertex_elements` | fixed in passing (routed through the resolver) | `PipeApply.cpp:1266-1273` |
| **n2, n3, n4, n5, n8, n9, n10** | recorded, not taken (ID-4 declared minors) | §7 |
| **n6, n7** | carried into §6 for B, as the review asks | §6 |

### 1.1 C1 — the records are per-object, not per-draw, and the refusal is now an observable

**The rule that landed** (`PipeApply.h:200-229`, and the two functions' own comments):

* `MGPipeApplierReset()` — which runs at **every change of the current `GLContext`**, not once per fresh one — clears
  the render-state CSOs, the residual mirror and the vertex-input **working state**, and advances the two global
  serials. It does **not** touch `Resources` or `VertexElementsCsos`.
* `MGPipeApplierReleaseObjectRecords()` (new, `PipeApply.h:279-300`, `PipeApply.cpp:649-660`) clears them. It is the
  **served context's teardown**: under split there is one applier per served context and this is what goes with it.
* Between the two, a record is cleared by its object's own death signal — `resource_destroy`,
  `delete_vertex_elements` — which is what D-L already makes the buffer's death crossing.

**In the monolith `MGPipeApplierReleaseObjectRecords()` is deliberately wired to nothing**, and that is written into
the header. There is one `g_applier` behind every context (`PipeApply.h:286` has always said so), so calling it on
one context's destruction would drop every *other* context's records — C1 in its other direction. The only callers
are the unit fixtures, which is the one place that legitimately means "this applier is going away". The records that
would otherwise accumulate are bounded by the client's dense slot high-water mark and are dropped one at a time by
the death notices, so nothing leaks.

**The observable.** Both refusal paths now go through `ResolveResource` / `ResolveVertexElements`
(`PipeApply.cpp:472-490`), which assert *and* count into `MGPipeApplierState::RefusedResourceCalls` /
`RefusedVertexInputCalls` (`PipeApply.h:220-229`). One place resolves, asserts and counts, so a call that forgets one
of the three cannot exist. `ResourceEmit.ACallOnARecordTheApplierDoesNotHaveIsCountedRatherThanSilentlyDropped`
(`ResourceEmitTest.cpp:859`) pins that a legal sequence leaves both at 0 and walks all nine resource entry points and
both vertex-input ones to 9 and 2. Removing either increment is red (§4, M12/M13).

**Why it stays a defined no-op rather than becoming a `Fatal`, and the exact sequence that requires it.** The review
allowed a no-op only if a legal sequence genuinely needs one. It does — one, and it is written at
`PipeApply.cpp:439-470`:

```
the served context is torn down
  -> MGPipeApplierReleaseObjectRecords()            (the applier goes away with the context)
  -> ~BufferObject / ~VertexArrayObject for every object the context still owns
  -> resource_destroy / delete_vertex_elements, each naming a record the line above dropped
```

Those death notices are legal, unavoidable and unordered with respect to the release, so a wire here would abort a
verify lane on the ordinary shutdown of a context. The counter is what makes the *other* case — an unknown slot, a
stale generation, a genuinely dropped `glBufferSubData` — visible in all three gate builds, where `MOBILEGL_ASSERT`
is inert.

**§3 A2's frequency is corrected** (C1c): it read "at the first validate of a fresh context"; the truth is "at every
change of the current context", and that is now what the code comments say (`PipeApply.cpp:604-608`,
`PipeApply.h:279-292`).

### 1.2 C2 — the two global serials advance

`PipeApply.cpp:628-646` replaces `= 0` with `++`, with the three-row argument (carry over / restart / advance) in
place, and `MGPipeApplierReleaseObjectRecords` does the same (`:658-659`) for the same reason. `PipeApply.h:251-264`
carries the rule beside the members. `VertexInputEmitTest.cpp:67-110`'s comment is rewritten and `:104-110` now pins
"moved forward" (`43`/`44` from `42`/`43`, plus the two strict inequalities); **the case's name is unchanged**, which
is all G14 protects. `ResourceEmit.TheObjectRecordsSurvive…` (`:816-820`) pins the same property with real state
behind it. Restoring `= 0` is red in both suites (§4, M15).

### 1.3 M-A — `Coverage.def`'s split is exhaustive

`Coverage.def:40-58` now lists all **15** `BufferTarget` enumerators in `BufferObject.h:15-33`'s own spelling
(`Vertex`, `Index`, `DrawIndirect`, `Parameter`, …), so the list cross-checks mechanically. `DispatchIndirect` gets
its own bucket with the reason the review asked for — both backends read it at every `glDispatchComputeIndirect` and
**no call carries it**, `MGPIndirectBuffers` being the `DrawIndirect` + `Parameter` pair only — plus the sentence
that a later phase must not read the split as covering it. `Query` gets a one-line bucket. `:63` now says "seven
targets above are still pulled" rather than five. Comment-only: `gen_pipe.py --check` is clean and
`git diff -- MobileGL/MG_Pipe/generated` is empty.

### 1.4 M-B — the `c2` coverage, in `ResourceEmitTest.cpp`

Nine new cases (`ResourceEmitTest.cpp:788-1330`), deliberately **not** in `VertexInputEmitTest.cpp`, whose contents
`C.5` gives to B and which B is appending to now. The file header block at `:646-660` says so in those words.

| case | what it holds | line |
|---|---|---|
| `TheObjectRecordsSurviveAMakeCurrentAndOnlyTheWorkingStateIsReset` | C1's two scopes with live records behind them; the write after the switch lands | `:788` |
| `ACallOnARecordTheApplierDoesNotHaveIsCountedRatherThanSilentlyDropped` | C1's observable, all eleven refusing entry points | `:859` |
| `AVertexElementsBlobRoundTripsAndAShrinkLeavesNothingOfTheOneBeforeIt` | the blob unpack over all 32+32 slots, field by field; the shrink; `ContentSerial` re-create vs recycled slot; no rebind | `:930` |
| `AVertexElementsRecordThatDoesNotDescribeItsOwnBlobIsRefusedNamingIt` | the counts/`Blob.Size` gate in both build arms, plus the "0 is not a fault" half of the Blob rule | `:984` |
| `TheVertexBufferWindowIsBoundedAndItsEntriesLandWhereItSays` | entries land in the window and nowhere else; the raw `BaseInstance`; `Start + Count` at 32 and 33; the null-tail arm | `:1033` |
| `SetIndexBufferMovesOnlyItsOwnSerialAndTheBindingFollowsTheHandle` | D5 independence; null index buffer; bind null / bind dead / delete-clears-the-binding | `:1111` |
| `EveryContentCallsOwnBoundsGateRefusesAndNamesTheResource` | flush range, readback range, `Level`, and the new `MGPSubData::Blob` rule, each with a positive control | `:1166` |
| `TheLiveHostWritesWireFiresOnTheCallAPersistentMapProducerWouldSetItOn` | M-C's negative control (§1.5) | `:1218` |
| `ASlotOutsideTheRecordTablesBoundIsRefusedRatherThanAllocated` | M-D | `:1283` |

Every refusal case uses `ExpectRefusedNaming` (`:912-929`), which takes the file's two existing arms — forked child +
`SIGABRT` + `Fatal{…}` in poison/verify, in-process log read + "nothing moved" in a shipped push build — and picks
the right tag for the arm, so one needle serves both. Comparisons are **by field name**
(`ExpectAttribEq` / `ExpectBindingEq` / `ExpectVertexBufferEq`, `:697-726`), which is what G7's negative control needs
of this family.

### 1.5 M-C — the wire has a control, and it sits on the call P5 attaches its producer to

`PinNoLiveHostWrites` is now called from `MGPipeApplyMapPersistent` (`PipeApply.cpp:1110`, with the reason in place —
D-A4 and `ARCHITECTURE.md:481` name the persistent-map push as exactly where the flag gets set) and from
`MGPipeApplyResourceReadback` (`:1049`, which reads the store the flag describes), beside the three it already had.

`ResourceEmit.TheLiveHostWritesWireFiresOnTheCallAPersistentMapProducerWouldSetItOn` (`ResourceEmitTest.cpp:1218`)
sets `MGPipeApplier().Resources[slot].HasLiveHostWrites = true` **inside a forked child** — so the parent's applier
stays honest for the next drive — and asserts `SIGABRT` plus `Fatal{PipeLiveHostWrites} <call> {slot=8, gen=4}` for
all five calls. Its negative control is the same `map_persistent` with the flag clear: it answers normally and logs
nothing. In a shipped push build the wire is compiled out entirely, so the case is a **visible skip** there rather
than a vacuous pass (`ctest -N` names stay identical: 2504 in both the pull and push trees).

### 1.6 M-D — the slot is bounded before it reaches an allocator

`PipeApply.h:97-113` mints `kMGPipeMaxResourceSlots = 1u << 20` and `kMGPipeMaxVertexElementsSlots = 1u << 16` with
the reason for two numbers rather than one (a resource record is descriptor-sized; a vertex-elements record carries
both unpacked views at ~1.3 KB, so one bound would mean two very different worst cases). `RecordAt`
(`PipeApply.cpp:400-421`) takes the bound and returns `nullptr`; both call sites report
`Fatal{ProtocolCorruption}` with the identity and the bound in the line (`:906-914`, `:1200-1210`) and return. The
tables still grow only to the client's own dense high-water mark, so the bound costs nothing until a record is
already corrupt. The case drives the bound exactly (`slot == kMGPipeMaxResourceSlots` and `0xFFFFFFFE` refused, the
table not grown); the last slot *below* the bound is deliberately not driven, because naming it is a ~90 MB
allocation and the direction that matters is the one that reaches the allocator.

### 1.7 M-E — one Blob rule, stated on both records and enforced on both

> `Blob.Size` is the record's own statement of how many bytes its blob holds, and the applier holds the record to
> that statement **whenever the record makes it**: a non-zero `Blob.Size` that disagrees with the byte length the
> record's other fields describe is `Fatal{ProtocolCorruption}` and the call is refused. A **zero** `Blob.Size` means
> "this record does not declare its blob" — which is what a monolith emission is, because the bytes travel beside the
> record through the entry point's companion `const void*` — and it is not a fault. Either way the bytes read are
> bounded by the record's **other** fields (the two counts here, the destination range there), so the declared length
> is a cross-check and never the safety property.

Written verbatim on `MGPVertexElements` (`MGPipeTypes.h:254-265`) and on `MGPSubData` (`:624-632`), and enforced on
both: `SubDataBoxFault` gains the check (`PipeApply.cpp:516-525`) and `create_vertex_elements`'s gate relaxes from
`!= declared` to `!= 0 && != declared` (`:1187`). Both directions are driven (§4, M5/M9).

**This changes what §5 item 2 told B**: B is no longer *required* to compute
`A*sizeof(MGPVertexAttribWire) + B*sizeof(MGPVertexBindingPointWire)` by hand at the emitter — leaving
`MGPVertexElements::Blob` entirely zero is now legal and inert. If B does fill it, it must be exact. The asymmetry
the review objected to is gone in the direction that removes a verify-build abort from a legal emission rather than
adding one, and the safety argument is unchanged because the counts (not the length) are what bound the unpack.

### 1.8 m1, m2, m3

`m1`: `Coverage.def:202` — `X(GetBoundVertexArray, BindVertexElements)` moved from position 2 to position 4, the
`LC_ALL=C` place after `GetBlendFuncIndexed`. Regenerating leaves `MobileGL/MG_Pipe/generated` byte-identical
(`gen_pipe.py` sorts the emitter enum itself), so this is presentation only, which is what `m1` said.

`m2`: `MGPipeTypes.h:270-275` — the comment now names `MGPVertexAttribWire[AttributeCount]` followed by
`MGPVertexBindingPointWire[BindingPointCount]`, says why the frontend types cannot travel, and points at the one
place the layout is stated (`MGPipeValueTypes.h`'s P3a block).

`m3`: `ConfigLoader.cpp:11-13` — "For `kMGPipeSubsystemsMigratedAtP3a`, the push build's `PipePush` default (the P2
constant beside it is the phase-by-phase control, not the default)".

### 1.9 m4 is **not** in an A file, and neither is m3's other half

The review's M-F table calls all four minors "A-owned". For `m4` that is wrong against `C.5`:
`MG_Impl/Pipe/PipeFill.{h,cpp}` — the `SubsystemForEmitter` / `static_assert` / `kMGPipeWiredSubsystems` block — is
**"owner (contract commit only)" for A and "owner (after the tag)" for B**. The tag is cut, so the three pairing
asserts at `PipeFill.cpp:687-701` are B's to make uniform. The one-line fix is to give the `NewVertexBuffers`
(`:694-697`) and `NewIndexBuffer` (`:698-701`) arms the same right-hand side the `NewVertexElements` arm has,
`SubsystemForEmitter(MGPipeFieldEmitter::BindVertexElements)`, rather than the literal `kMGPipeSubsystemVertexInput`.

`m3`'s `MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h:112-115` half is the integrator's, as the review says: no
package owns `MG_Backend/DirectVulkan/**` in `C.5`.

---

## 2. What the applier's contract now is, in one place (for B and C)

1. **Records are object state.** They survive a make-current. They are cleared by `resource_destroy` /
   `delete_vertex_elements` and by `MGPipeApplierReleaseObjectRecords()`. Nothing else clears them.
2. **`MGPipeApplierReleaseObjectRecords()` must not be wired in the monolith.** One applier serves every context.
3. **The two global serials only ever advance.** `VertexBuffersSerial` and `IndexBufferSerial` are `++`-ed by their
   own calls, by `MGPipeApplierReset()` and by `MGPipeApplierReleaseObjectRecords()`, and are never assigned. A twin
   may safely memo either one; no value can recur. (`Serial` and `ContentSerial` are per record and restart with the
   record, which is safe because the handle's `Gen` moves with it — unchanged from `wire-v1`.)
4. **A refused call is counted**, in `RefusedResourceCalls` / `RefusedVertexInputCalls`, per context. A non-zero
   count in a lane is a dropped call, and a legal sequence leaves it at 0 except during a context teardown.
5. **`handle.Slot` is bounded** by `kMGPipeMaxResourceSlots` / `kMGPipeMaxVertexElementsSlots` (`PipeApply.h:112`),
   which C should reuse when it bounds `handle.Slot` before `BackendSlotTable::EntryAt` (contract-review M2).
6. **The Blob rule** (§1.7) — one rule, both families, `0` means "not declared".

---

## 3. Deviations declared in this round

**R1 — `MGPipeApplierReleaseObjectRecords()` has no production caller.** It is the split-side teardown hook and the
unit fixtures' "empty the applier". Wiring it in the monolith would be wrong (§1.1) and wiring it under split is P5's
server bring-up, not P3a's. It is `push`-only source, so G1 is unaffected; the two fixtures and one case drive it.

**R2 — the review's C1a asked for an `INTEGRATOR-DECISIONS.md` entry.** `docs`/notes ownership is the integrator's
(ID-5, and `C.5` gives `docs/Disaggregated/*.md` to nobody but the integrator), so the decision is written here and
into the two functions' own comments instead. The integrator owes ID-9 the two sentences in §2 items 1-2.

**R3 — the vertex-elements `Blob.Size` gate was RELAXED, not tightened.** §1.7. It is the one place this round
changes a rule rather than adding one, and it changes it in the direction that cannot abort a legal emission.

**R4 — `VertexInputEmitTest.cpp` was edited even though B is appending to it.** The edit is confined to `c0`'s
placeholder case (`:67-110`) and is not optional: at `e9b4a263` that case asserts `VertexBuffersSerial == 0` after a
reset, which is exactly the inversion C2 removes, so leaving it would leave the tree red. No case was added there.
See §6 for the rebase note.

**R5 — the shared G1 baseline has drifted and G1 was run as a parent-vs-HEAD control instead.** §5.

---

## 4. Mutation evidence — 17 mutations, 17 reds, measured

Each mutation was applied to `MobileGL/MG_Pipe/PipeApply.cpp` in this worktree, the two test targets rebuilt, the two
suites run, and the file restored with `git checkout --` (the tree was verified clean before and after every batch;
no other worktree was created). The eight the review measured green at `e9b4a263` are M1-M8.

| # | mutation | verdict | first case to go red |
|---|---|---|---|
| M1 | `create_vertex_elements`: the attribute `memcpy` removed | **RED** | `AVertexElementsBlobRoundTripsAndAShrink…` |
| M2 | `create_vertex_elements`: the binding-point `memcpy` removed | **RED** | `AVertexElementsBlobRoundTripsAndAShrink…` |
| M3 | `set_vertex_buffers`: the entry-copy loop body removed | **RED** | `TheVertexBufferWindowIsBounded…` |
| M4 | `set_vertex_buffers`: the `Start + Count` window gate disabled | **RED** | `TheVertexBufferWindowIsBounded…` |
| M5 | `create_vertex_elements`: the counts/`Blob.Size` gate disabled | **RED** | `AVertexElementsRecordThatDoesNotDescribeItsOwnBlob…` |
| M6 | `resource_flush_range`: `BufferRangeFault` removed | **RED** | `EveryContentCallsOwnBoundsGate…` |
| M7 | `resource_readback`: `BufferRangeFault` removed | **RED** | `EveryContentCallsOwnBoundsGate…` |
| M8 | `SubDataBoxFault`: the `Level` arm removed | **RED** | `EveryContentCallsOwnBoundsGate…` |
| M9 | `SubDataBoxFault`: the new `MGPSubData` Blob rule removed | **RED** | `EveryContentCallsOwnBoundsGate…` |
| M10 | `resource_create`: the slot bound widened by one | **RED** | `ASlotOutsideTheRecordTablesBound…` |
| M11 | `create_vertex_elements`: the slot bound widened by one | **RED** | `ASlotOutsideTheRecordTablesBound…` |
| M12 | the resource refusal counter removed | **RED** | `ACallOnARecordTheApplierDoesNotHave…` |
| M13 | the vertex-input refusal counter removed | **RED** | `ACallOnARecord…` + `SetIndexBufferMovesOnlyItsOwnSerial…` |
| M14 | **C1 reverted**: `MGPipeApplierReset` clears the records again | **RED** | `TheObjectRecordsSurviveAMakeCurrent…` |
| M15 | **C2 reverted**: the two serials zeroed at a reset | **RED** | `TheObjectRecordsSurvive…` + `VertexInputEmit.AResetApplierCarriesNoVertexInputStateOver` |
| M16 | `map_persistent`: the `PipeLiveHostWrites` pin removed | **RED** (verify tree) | `TheLiveHostWritesWireFires…` |
| M17 | `resource_readback`: the `PipeLiveHostWrites` pin removed | **RED** (verify tree) | `TheLiveHostWritesWireFires…` |

M1-M15 were run in `build-push`, M16-M17 in `build-verify` (the wire is compiled out of a shipped push build, and its
case skips visibly there). No mutation survived.

---

## 5. Verification transcript

All in `/home/swung/w7/p3a-wire` at `48fe55be`, `CCACHE_BASEDIR=/home/swung/w7`.

**G1 — and a baseline problem the integrator has to settle.** `~/w7/p3a-before-libMobileGL.so` and
`~/w7/p3a-before-ctest-names.txt` were **re-captured at 03:17 today at `5cb826b0`** ("`[Merge] (dev): bring
dev@9eae9858 into feat/disaggregated - the adopted-store respecify VAO rebind fix (d7655247) and the
persistent-buffer ordering POST probe`"), which is **not an ancestor of `p3a/wire`** (`~/w7/p3a-before-head.txt`
records it). Run against it, `symbol_report` now reports `27811 -> 27799, 0 added, 12 removed, 1 resized` — every
removed symbol is `MG_Util::SelfTest::…PersistentBufferUpdateOrdering…`, i.e. dev code this branch does not have —
and the name comparison shows 15 removals, all `PersistentBufferOrderingProbeTest.*` and
`*.LargeArenaAdoptionScenario.Respecified*`. None of it is P3a's. `wire-v1` ran the same command against the
*previous* baseline and got `27799 -> 27799`.

So G1 was run as the control that actually answers the question — **the pull library at `e9b4a263` versus the pull
library at `48fe55be`**, both built in this worktree:

```
python3 scripts/symbol_report.py --before <pull lib at e9b4a263> --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0            rc 0
symbol-report: .text 10792739 -> 10792739 (+0, +0.000%)   .data +0   .bss +0   .rodata +0
symbol-report: Total 17209979 -> 17209979 (+0)
symbol-report: 27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27060 -> 27060 normalised names, 27060 unchanged
```

The two libraries are the same size to the byte. P3a's admitted-resize set stays empty. **Action for the
integrator:** either re-capture `~/w7/p3a-before-*` at `44c2b5cf` (ID-1's stated base) or rebase every P3a package
onto `5cb826b0` and re-capture there; until then the shared baseline answers a different question than G1 asks.

**Generators and closure**

```
python3 scripts/gen_pipe.py --check       rc 0   "gen_pipe: generated files are up to date"
python3 scripts/gen_pipe.py --self-test   rc 0   "7 negative-control trip(s), positive control OK"
git diff --exit-code -- MobileGL/MG_Pipe/generated                                    rc 0
python3 scripts/check_include_closure.py                          rc 0  "4 probes, 0 skipped, 0 problem(s)"
python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all   rc 0
```

**Three builds** (`cmake --build <dir> -j 24`): `build-linux` rc **0**, `build-push` rc **0**, `build-verify` rc **0**.

**Three unit runs** (`ctest --test-dir <dir> -L unit -j 12 --no-tests=error`):

```
build-linux   rc=0   100% tests passed, 0 failed out of 1588   (9.16 s)
build-push    rc=0   100% tests passed, 0 failed out of 1588   (9.20 s)
build-verify  rc=0   100% tests passed, 0 failed out of 1588   (9.28 s)
```

(1579 at `e9b4a263` + 9 new names.)

**The two P3a suites** (`ctest -R 'ResourceEmit\.|VertexInputEmit\.'`):

```
build-push    19 tests, 100% passed, 1 visible skip  (the PipeLiveHostWrites wire is compiled out here)
build-verify  19 tests, 100% passed, 0 skips        (every forked-abort arm taken)
build-linux   19 tests, 18 visible skips + the range-encoding case
```

**Names (G2's name half, G14)**

```
ctest -N: build-linux 2504, build-push 2504, build-verify 3332
diff <linux names> <push names>                          -> empty (rc 0)
comm -23 <names at e9b4a263> <names at HEAD>             -> empty   (nothing removed)
comm -13 ...                                             -> 9 added, all ResourceEmit.*
```

**G13**: `grep -c 'MG_State' MG_Pipe/PipeApply.h` → **0**; `grep -rc 'pGLContext' MG_Backend | grep -v ':0$'` →
empty.

**Ownership**: `git diff --stat 39722687..HEAD` names exactly `MG_Pipe/{PipeApply.h, PipeApply.cpp, MGPipeTypes.h,
Coverage.def}`, `ConfigLoader.cpp` and the two `MG_Test/Pipe/*EmitTest.cpp` — all A's under `C.5`. No `MG_Impl`, no
`MG_Backend`. Working tree clean; nothing pushed; no other worktree created.

**Not run here, and why**: G2's execution half, G3/G3b/G4's retrace, G5 (the script is D's and is not in the tree;
this package touches no `Managers.cpp` region, so the nine functions are byte-identical by construction), G6-G12,
G15. This package still registers no `MGPipeResourceOps` and emits no call, so it changes no observable and those
gates belong to the integrator's D.1 merges. Behaviour neutrality is unchanged from `wire-v1`: `grep` over
`MobileGL/` for the fourteen entry points and for `MGPipeSetResourceOps` still returns hits only in
`MG_Test/Pipe/ResourceEmitTest.cpp`.

---

## 6. What is left, and for whom

1. **The rebase note for B** (R4). `MG_Test/Pipe/VertexInputEmitTest.cpp:67-110` changed: the placeholder case's
   comment is rewritten and its two serial assertions now read "moved forward" (43/44) instead of 0/0, and the two
   vacuous `…CsoCsos.empty()` / `Resources.empty()` assertions were removed because the records are no longer part of
   that scope. The case's **name** is unchanged. B's `b4` is still an append below `:110`; if it collides it is a
   union, never a choice.
2. **`MGPipeApplierReset` no longer empties the applier** — a fixture that wants an empty one must also call
   `MGPipeApplierReleaseObjectRecords()`, as `ResourceEmitTest.cpp:139-150`'s `ApplierGuard` now does. Any B or C
   test that resets the applier and then asserts `Resources.empty()` needs the second call.
3. **`MGPVertexElements::Blob.Size` may now be left 0** (§1.7), which supersedes `wire-v1` §5 item 2. If B fills it,
   it must be exact. The same rule now applies to `MGPSubData::Blob`, which B must leave 0 or fill exactly.
4. **`resource_subdata` still requires a `Width`** — unchanged from `wire-v1` §5 item 3.
5. **`VertexFetchBaseInstance` is raw** — unchanged, for C.
6. **The serial contract for C**, complete: §2 items 3-5 above.
7. **n7 (cross-package, from the review):** B's resident dispatch must gate on
   `MGPipeGetResourceOps()->SubDataResident != nullptr`, not on `MGPipeResourceSubsystemEnabled()` alone — the
   applier drops a resident write when the member is null, after bumping `Serial`, and
   `BufferObject::LandBytesIntoResidentStore` is unreachable once the call has been emitted. Unreachable in P3a
   (Espryt registers all nine members; Magma registers no table), but it is B's dispatch shape.
8. **n6 (cross-package, from the review):** `MGPipeApplierState::MapPersistentRoundtrips` is **per context** (reset
   by `MGPipeApplierReset`, pinned at `ResourceEmitTest.cpp:585-587`) while `PipeStats`' `mpr` is not. D's
   `StorageBufferRegrowScenario` should assert against **`PipeStats`' `mpr`**, which is the one an operator greps and
   the one that survives a context switch.
9. **Doc corrections owed to the integrator**, unchanged from `wire-v1` §5 item 8, plus: D-G4's reset prose should
   say the records are share-group state (§2 items 1-2), and the review's M-F table should record that `m4` is B's
   file after the tag (§1.9).
10. **The baseline drift** (§5) — integrator.

## 7. Declared minors not taken (ID-4)

`n2` (`unmap_persistent` refuses on a dropped record) — left as is; delivering a release to a backend that has no
record for it is the worse answer, and the refusal is now counted. `n3` (`SubDataBoxFault` holds three of seven
statements) — the other four are inert for a buffer and the helper's comment no longer claims completeness beyond
what it checks. `n4` (`IndexSize` unpoliced), `n5` (`BindingIndex` unpoliced) — both stated as deliberate at
`PipeApply.cpp:1331-1345` and `:1305-1311`; policing either would hand B a contract this package cannot justify.
`n8` (the by-handle assertion is half made), `n9` (`ARespecifyReplaces…` re-pins the ack predicate) — recorded.
`n10` (`PipeApply.h` edited after the tag) — this round edits it again, comment and members both; C's tree branches
from the tag and will see all of it on its rebase, and the integrator still owes C that message.

*Intermediate logs and the mutation harness under `~/w7/p3a-wire-mut/` were deleted at the end of the round.*
