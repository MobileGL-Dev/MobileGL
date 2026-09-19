# P3a package B, rework round 2 — the client (`client-v2`)

Branch `p3a/client`, worktree `/home/swung/w7/p3a-client`, **rebased onto `refs/heads/feat/disaggregated`
(`12e6bfcf`, wire v2 + the dev merge)** and then four new commits on top. **Not pushed.**

| step | hash | message (single line, empty body, no attribution) |
|---|---|---|
| b1' | `a6851a26` | *(rebased, unchanged)* `[Feat] (Pipe, State): mint a {slot, gen} handle for every buffer object …` |
| b2' | `13e4b6f4` | *(rebased, unchanged)* `[Feat] (Pipe): push the bound VAO's format, its vertex buffers with an explicit baseInstance …` |
| b3' | `f656620d` | *(rebased, unchanged)* `[Feat] (Pipe): widen the dirty-surface scan to MG_State …` |
| b4' | `5df05545` | *(rebased, ONE conflict resolved by union — §1)* `[Test] (Pipe): pin the buffer and vertex-input emitters …` |
| **r1** | `2da442a7` | `[Fix] (Pipe): stop the tracker's context Reset eating the pending base instance the same call is about to read, and clear it on the validate point's no-context exit too` |
| **r2** | `b6706362` | `[Feat] (Pipe, GLImpl): set the pending base instance from the three glDraw*BaseInstance entry points, through a pull-safe macro beside MGP_FILL` |
| **r3** | `d1f684a7` | `[Fix] (Pipe, State): look the resource handle up on the content paths instead of minting it, publish the bind mask from the two draw-time emitters, pair the destroy with the create's own latch, and state the record-lifetime rule the applier now holds` |
| **r4** | `dfaa8729` | `[Test] (Pipe): pin the base instance across a make-current and both of the validate point's exits, the record surviving one, the draw-time bind mask, the create-destroy pairing latch, and the bind-mask table against literals` |

`git diff --shortstat 12e6bfcf..HEAD` → **15 files changed, 2843 insertions(+), 69 deletions(-)**. The only file
outside C.5's grant is `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp`, **+3 lines, exactly the three ID-10 grants**
(`:662`, `:685`, `:715` — each immediately above the `MGP_FILL` the review named at `:662/:684/:713`).
`MG_Test/Pipe/TrackerTest.cpp` is C.5's "nobody" file, as at `client-v1` (B-DEV-5), and the review's own rework
item 1 names a case in it.

---

## 1. The rebase, and how the conflict was resolved

`git rebase refs/heads/feat/disaggregated` replayed b1-b3 cleanly. **b4 conflicted in
`MG_Test/Pipe/ResourceEmitTest.cpp`, not in `VertexInputEmitTest.cpp`** — the opposite of what the task
predicted, and worth recording:

* `VertexInputEmitTest.cpp` **auto-merged**. Wire's rewrite of the placeholder case (`:67-110`, the two serial
  assertions now "moved forward" 43/44 and the two vacuous `…empty()` assertions gone) and B's seven appended
  cases do not overlap textually. Nothing was chosen; both sides are present.
* `ResourceEmitTest.cpp` conflicted as one block-vs-block hunk (`<<<<<<<` at :118, `=======` at :1325,
  `>>>>>>>` at :1613), because at `39722687` the file held exactly ONE case and both packages appended to that
  same point. **Resolved by union, in that order: wire's whole block first, then B's whole block**, keeping
  every `#if !MOBILEGL_PIPE_PUSH` / pull-skip arm from both sides (the two use different pull-skip idioms —
  wire per-case, B a macro list — and they compose without collision).

**Every test name from both sides survives**, verified by enumerating the resolved file and by `ctest -N`:

```
wire's 17   ACreateMarksTheSlotLiveAndCarriesItsDescriptor, ARespecifyReplacesTheDescriptorAndOnlyAMutation
            MovesTheSerial, ADestroyDropsTheRecordAndAStaleGenerationResolvesToNothing, NoResourcePathInThis
            PhaseLeavesHostWritesLive, TheSubDataRangeEncodingRefusesExactlyAtItsTwoBounds, EveryResourceCall
            DispatchesByHandleThroughTheInstalledTableOnly, MapPersistentCountsEveryAttemptWhetherItMintsOr
            Declines, AWriteOutsideTheDeclaredStorageIsRefusedNamingTheResource, TheObjectRecordsSurviveAMake
            CurrentAndOnlyTheWorkingStateIsReset, ACallOnARecordTheApplierDoesNotHaveIsCountedRatherThan
            SilentlyDropped, AVertexElementsBlobRoundTripsAndAShrinkLeavesNothingOfTheOneBeforeIt, AVertex
            ElementsRecordThatDoesNotDescribeItsOwnBlobIsRefusedNamingIt, TheVertexBufferWindowIsBoundedAndIts
            EntriesLandWhereItSays, SetIndexBufferMovesOnlyItsOwnSerialAndTheBindingFollowsTheHandle, Every
            ContentCallsOwnBoundsGateRefusesAndNamesTheResource, TheLiveHostWritesWireFiresOnTheCallA
            PersistentMapProducerWouldSetItOn, ASlotOutsideTheRecordTablesBoundIsRefusedRatherThanAllocated
B's 4       EveryBufferTargetSetsItsBindMaskBit, ABindMaskBitIsStickyAcrossARespecifyThatDoesNotRebind,
            ADestroyedBufferReleasesItsSlotAndAStaleHandleResolvesToNothing,
            AWholeBufferSubDataBeyondTheRecordBoundIsSplitIntoContiguousRecords
c0's 1      TheResourceOpTableIsUnregisteredUntilABackendInstallsOne   (common prefix, never in the conflict)
```

One mechanical consequence of the union was carried into b4' itself: `MGPipeBuildSubDataRecord` gained a
parameter in r3, so the splitter case's call at `ResourceEmitTest.cpp:1574` passes it (r3, not b4').

**`feat/disaggregated` moved again while this round ran**, to `b9eaa474` — two version bumps
(`42d43af2` the plugin version to 26.09, `b9eaa474` `CoreVersion` to 26.9). Neither touches a file this
package edits, so the second rebase is a fast-forward with no conflict; every reading below was taken against
`12e6bfcf`, which is the tree the task named and the tree `~/w7/p3a-before-libMobileGL.so` was captured at
(`5cb826b0`, its ancestor). A `CoreVersion` bump does move `.rodata`, so **G1 must be re-taken with a baseline
re-captured at `b9eaa474`** if the integrator lands those two underneath rather than on top.

**A second consequence of the rebase, taken in r4:** `MG_Test/Pipe/TrackerTest.cpp`'s
`ResetTheServerSideSingletons()` (`:250`) now also calls `MGPipeApplierReleaseObjectRecords()` and
`MGPipeVertexInputEmitterInstance().Reset()/ResetCounters()`. Wire's §6 item 2 requires the first
(`MGPipeApplierReset` no longer empties the applier); the second is B's own (the emitter's latches describe an
applier that is about to be emptied). Nothing in that fixture asserted `Resources.empty()`, so this is
robustness for a by-hand run rather than a repair.

---

## 2. Per-finding disposition

### 2.1 Critical

| finding | disposition | where |
|---|---|---|
| **B-C1** `Reset()` zeroes the pending base instance the same call is about to read | **FIXED, one line deleted** — `Tracker.h`'s `Reset()` (`:378-396`) no longer clears `m_pendingBaseInstance`, with the reason written above it (`:368-377`): everything else `Reset()` clears is a latch describing what the server was told; this one is **this call's argument**, written one statement before `MGP_FILL` and not yet read. Both verb-scoped clears are kept. | `Tracker.h:368-396` |
| **B-C2** no re-publication path for resource records after `MGPipeApplierReset()` | **RESOLVED PER ID-12, and the claim replaced by the rule** — no re-publication is added and none may be; `ResourceTracker.h:471-492` now states the actual rule in place of "absence as policy" (below). | `ResourceTracker.h:471-493` |

**B-C1's second half (the review's rework item 1, and B-M1's hole).** The review asked to *"move
`PipeFill.cpp:1446`'s clear above the `ctx == nullptr` return at `:1371`, or record why not"*. Moving it there
would clear the value **before** `EmitVertexBuffers` reads it, which is the bug in the other direction. What
landed instead is the same guarantee on that exit only: `PipeFill.cpp:1440-1449` clears the pending value on
the no-live-context early return, so the property *"consumed by exactly the verb whose entry point set it, and
0 at every other `Update`"* holds on **both** of `MGPipeValidateForVerb`'s exits. That is now the whole
production guarantee, and both the tracker's accessor comment (`Tracker.h:447-452`) and `MGPipeLeaveVerb`'s
(`PipeFill.cpp:849-860`) say so in those words: **no GL entry point calls `MGPipeLeaveVerb`** — `grep` finds
`MG_Test/ScopedPipeVerb.h` and `MG_Test/Pipe/TrackerTest.cpp` and nothing else — so `client-v1` §4 B-DEV-3's
*"neither of them relies on the other being called"* was wrong and is withdrawn. The clear this package relies
on is `PipeFill.cpp:1527`'s, at the end of step 3.

**B-C2, the rule as it now reads** (`ResourceTracker.h:471-492`, beside `ResetForTest`, replacing
*"Never called by the library: a context change does not invalidate a handle"*):

> A buffer handle and the applier record it names are **share-group object state**. A GL object lives in a
> share group, not in a context, so a make-current changes neither. `MGPipeApplierReset()` is a make-current
> and deliberately keeps its `Resources` / `VertexElementsCsos`; the **only** things that drop a record are the
> object's own death signal — `resource_destroy`, which `~BufferObject` raises through
> `MGPipeEmitResourceDestroyAndFree`, and `delete_vertex_elements` — and
> `MGPipeApplierReleaseObjectRecords()`, which is the served context's teardown and is deliberately wired to
> nothing in the monolith. So this tracker needs no re-publication path on a fresh context **and must not have
> one**: re-emitting `resource_create` for a record the applier still holds would move its `Serial` for
> nothing. What the client owes instead is the destroy, which `~BufferObject` already emits in the fixed
> emit-then-free order (D-L).

The one asymmetry the review pointed at is now explained rather than repeated: the vertex-input emitter's
`Reset()` **stays** in the `FreshlyPrimed` arm, and `PipeFill.cpp:1484-1492` no longer says it is there
"because the applier's records were dropped" (they are not). It is there because the emitter's *other* latch,
`m_boundHandle`, mirrors `MGPipeApplierState::BoundVertexElements`, which `MGPipeApplierReset` **does** clear —
without the reset the bind after a make-current would be suppressed as unchanged and the server would draw with
no vertex elements bound. Re-creating an unchanged configuration alongside it is a bounded over-fire; a dropped
bind is not.

Pinned by `ResourceEmit.ABufferCreatedBeforeAMakeCurrentStillLandsItsSubDataAfterOne`
(`ResourceEmitTest.cpp:1686`), which is the client-side half of wire's applier-side case: a **real
`BufferObject`** created and respecified under ctxA, then `MGPipeApplierReset()` + a fresh `GLContext`, then a
real `glBufferSubData` — asserting the record is still `Live`, that the serial moved, that
`RefusedResourceCalls == 0`, and that `CreateCount() == 1` (i.e. the client did **not** grow a re-publication
path).

### 2.2 Major

| finding | disposition | where |
|---|---|---|
| **B-M1** the three `MGPipeSetPendingBaseInstance` call sites | **LANDED (ID-10)**, plus the `MGPipeLeaveVerb` correction above | `GL_Drawing.cpp:662, :685, :715`; `PipeFill.h:114-124` |
| **B-M2** `Acquire` mutating the global slot allocator on the off-thread `SubData` path | **FIXED — the mint stays on the GL thread at creation only**; all five content emitters now look the handle up | `PipeFill.cpp:581-596, :649, :670, :691, :705, :727` |
| **B-M3** `hostBytes` base-vs-base+offset | **NOT B'S** — handed on verbatim, §5 | — |
| **B-M4** the sampled `BindMask`'s DSA hole, and the C.1 misquotation | **FIXED both halves**, by the primary mechanism the review named | `VertexInputEmit.h:219, :267`; `ResourceTracker.h:363-408, :516-522` |
| **B-M5** the absent `UnmapPersistent` producer | **RECORDED, with the reason, beside the `MapPersistent` emitter** | `PipeFill.cpp:718-726` |

**B-M2.** Which of the two options the review allowed: **"make the mint happen on the GL thread at creation
only."** It already did — `MGPipeMintResourceHandle` is called unconditionally from the `BufferObject`
constructor — so on every content path the handle exists and a lookup is not merely safe but strictly correct.
`MGPipeEmitResource{SubData,FlushRange,Readback}`, `MGPipeEmitBufferSubDataResident` and
`MGPipeEmitMapPersistent` now go through one helper, `ContentHandleFor` (`PipeFill.cpp:581-596`), which calls
`MGPipeResourceTracker::Find` (const; no free-list pop, no map insert, no `m_bySlot.resize`) and answers a null
handle with `MGLOG_E_ONCE` naming the call and the GL buffer, then returns — rather than handing
`kMGPipeNullHandle` to the applier, which would count as a refusal with no way back to the cause.
`MGPipeEmitMapPersistent` returns `nullptr` on that path, which every one of its three call sites already
treats as "the backend declined" (`BufferObject.cpp:600-606`, `:650-658`). `Acquire` now survives in exactly
two places, both GL-thread by construction: the constructor's mint, and create/respecify.

**B-M4(a), the record corrected.** `client-v1` §4 B-DEV-2 said the bind entry points are *"`GL_Buffer.cpp`'s,
which C.5 assigns to no package and **C.1 does not list for this one**"*. That last clause is **false**:
`BRIEF-P3A.md:1130` lists `MobileGL/MG_State/GLState/BufferState/BufferState.{h,cpp}` for this package with
the note *"the sticky `everBoundAs` mask's bump points at the buffer bind entry points (D-A3), all
`#if MOBILEGL_PIPE_PUSH`"*, and C.5 gives B `MG_State/GLState/BufferState/**`. What is true is that the brief
pointed at a file that turns out not to contain the entry points: `BufferState` only *vends*
`BindingSlot<BufferObject>&` / `BindingSlotRange1D&`, and the `.Bind()` calls are `GL_Buffer.cpp`'s
(`BindBuffer_State`, `BindBufferBase_State`, `BindBufferRange_State`), which C.5 assigns to nobody. The
substance of the deviation survives; the sentence did not, and the corrected version is now written at
`ResourceTracker.h:376-386`.

**B-M4(b), the hole closed at the place that matters.** The real widening is a **transient** bind, and the DSA
idiom makes it the common case. `MGPipeVertexInputEmitter::EmitVertexBuffers` and `EmitIndexBuffer` already
resolve, at every draw, exactly the attribute buffers and the element-slot buffer, so both now call
`MGPipeResourceTrackerInstance().NoteBoundAs(handle, BufferTarget::Vertex | ::Index)`
(`VertexInputEmit.h:219`, `:267`). `NoteBoundAs` (`ResourceTracker.h:363-374`) grows the table rather than
dropping the note, and is GL-thread by construction (the validate point). **Any buffer ever drawn from carries
its `ARRAY_BUFFER` / `ELEMENT_ARRAY` bit for the rest of its life**, whether or not it was bound at a storage
op. `ResourceEmit.ADrawTimeIndexBindingPublishesElementArrayEvenWhenTheRespecifyCannotSeeIt`
(`ResourceEmitTest.cpp:1729`) drives exactly the review's sequence, with a negative control first (a respecify
with nothing bound publishes an empty mask), and goes red with the two calls removed (§4 M2).

**B-M4(b), the half NOT taken, and why.** The review also asked for `RefreshBindMask` from the other four
resource emitters. It is **not** taken, and the reason is B-M2's: `RefreshBindMask` walks the whole context's
binding state and writes the tracker, and one of those emitters (`resource_subdata`) is on the path D-A2
preserves as reachable **off the render thread**. Extra sampling can only widen a sticky union, but not at the
price of a context-wide read from the wrong thread — fixing one thread-safety finding by creating a larger one
is not a trade this package will make. The reasoning is written at `ResourceTracker.h:400-408`. With
`NoteBoundAs` in place the residue is strictly smaller than what the review's version would have left: a buffer
that is bound, **never drawn from**, and never re-specified afterwards. That is the P8 `GL_Buffer.cpp` item,
unchanged, and it is now the only thing on the list.

**B-M4 secondary, `BindEpoch`.** The sentence the review asked for is at `ResourceTracker.h:516-522`: the epoch
does not see the 84×4 indexed binding points, and that is sound **only** because `BindBufferBase_State` /
`BindBufferRange_State` also bind the generic slot (`GL_Buffer.cpp:1531`); if that stops being true,
`CONSTANT` / `SHADER_BUFFER` / `ATOMIC` / `STREAM_OUTPUT` start being missed silently, and the repair is to
fold `GetTouchedBufferBindingPointCount` into the epoch.

**B-M5.** Recorded, with the reason, at `PipeFill.cpp:718-726`: `BufferBackendOps` has seven hooks and none is
an unmap; `PipeResource::ReleasePersistentMap()` tells the backend nothing today and it learns from the
`Respecify` that follows; emitting `unmap_persistent` would be **new behaviour**, which D-J forbids. So the
client emits none, deliberately, and the applier's `RefusedResourceCalls` stays at 0 for it. The producer lands
with the phase that gives the backend an unmap hook.

### 2.3 Minors

| # | disposition | where |
|---|---|---|
| m1 | **taken** — `MGPipePendingBaseInstance()` now has a caller: `TrackerShippedEmitter.ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance` asserts on it three times (the setter took, the value was consumed, the no-context exit cleared it). Its declaration comment names that case. | `PipeFill.h:72-74`; `TrackerTest.cpp:862` |
| m2 | **taken, both directions** — `MOBILEGL_ASSERT(bindingIndex < 256)` and `attrib.Size in [0,255]` before the two `Uint8` narrowings, and `MOBILEGL_ASSERT(attrib.Stride >= 0)` before the signed→unsigned one, each with the failure it prevents named (binding 256 wrapping to 0; a negative stride arriving as a ~4 GiB fetch distance) | `VertexInputEmit.h:83-90`, `:224-226` |
| m3 | **taken** — `SourceIsVerbatimLevelShadow` is a **parameter** of `MGPipeBuildSubDataRecord`, `true` on `resource_subdata` (the client's own shadow) and `false` on `buffer_subdata_resident` (the application's staging store, or `FillSubData`'s locally expanded pattern) | `ResourceTracker.h:212-233`; `PipeFill.cpp:656`, `:678` |
| m4 | **taken** — both walk lambdas now `if (!MGPipeBuildSubDataRecord(...)) return;` rather than discarding the `Bool`, with the comment saying the pre-pass makes it unreachable today | `PipeFill.cpp:654-658`, `:675-680` |
| m5 | **taken** — `MGPipeClientOnGpuWritten` asserts the announced shape (exactly one range, non-null) and returns on `rangeCount == 0` rather than marking a whole buffer written; the resolve-to-null arm is now as loud as its sibling | `ResourceTracker.h:594-615` |
| m6 | **taken** — a `LiteralBindMaskFor` oracle spelled as raw bit positions read off `MGPipeTypes.h`'s documented order (no `default:` arm, so a new `BufferTarget` is a build break in both places); the emitted mask is compared to the **literal**, the table is compared to the literal for the bindable targets, and a second loop compares the table to the literal for **all 15** enumerators | `ResourceEmitTest.cpp:1417-1462`, `:1484-1490`, `:1505-1512` |
| m7 | **NOT taken** — §6 | — |
| m8 | **taken** — the `.def` row now says the publisher is the backend's (`Managers.cpp`'s `OnFrontendStateObjectDestroyed` consumer, package espryt), and that until it lands the row states the design and not the tree | `DirtySurface.def:295-298` |
| m9 | **taken** — the bit-7/bit-8 coupling is stated where an operator reads the constant: bit 8 without bit 7 sends handles the server cannot resolve and every such call lands in `RefusedResourceCalls`; neither `0x1ff` nor `0x7f` is in that arm | `PipeFill.cpp:978-988` |
| m10 | **taken** — a store of ≥ 4 GiB is `MGLOG_E_ONCE` + `MOBILEGL_ASSERT` naming the buffer and the size, with the reason (a silent narrowing makes the applier's range gate report `Fatal{ProtocolCorruption}` for a corruption that is really a truncation here); the 32-bit `Width` is the contract's, so the value is still narrowed after the line | `ResourceTracker.h:174-190` |
| m11 | **settled, and it agrees with wire** — wire's ONE Blob rule (0 = not declared; non-zero must be exact) makes filling optional. B **fills it, exactly**: `Blob.Seg = kMGHostSpanSegNone`, `Blob.Size` = the piece's own byte length, which is precisely what `SubDataBoxFault`'s `!= 0 && != MGPipeSubDataBufferSize` gate checks. The rule and the choice are written at the builder. | `ResourceTracker.h:218-224` |
| m12 | **taken** — `MGPipeResourceTracker::Entry` gains `Published`, set at the create (`NotePublished`) and read at the destroy (`WasPublished`); `MGPipeEmitResourceDestroyAndFree` now **returns** whether the destroy went out and `~BufferObject` uses that answer instead of asking `MGPipeResourceSubsystemEnabled()` a second time. That closes both directions: a live applier record on a slot about to be re-handed-out, and a legacy backend object nobody releases. | `ResourceTracker.h:328-352`, `:505`; `PipeFill.cpp:623-626`, `:745-762`; `PipeMutation.h:116-125`; `BufferObject.cpp:55-60` |

---

## 3. What changed in the deviations `client-v1` declared

Only two of the nine move; the rest stand as the review accepted them.

* **B-DEV-2 (sampled `BindMask`)** — the C.1 citation is corrected (§2.2 B-M4(a)), the hole is re-described as
  the *transient* bind rather than "a bind after the last resource emission", and the fix for the two bits
  anything keys on is now **in this package** rather than handed on. What is handed on is smaller and stated:
  the three `GL_Buffer.cpp` binders, for the seven bits nothing keys on yet.
* **B-DEV-3 (bit 9's shutter)** — the mechanism is unchanged and still correct; the claim that the two clears
  are independent is **withdrawn** (§2.1). There is one production clear, on two exits.
* **B-DEV-1, -4, -5, -6, -7, -8, -9** — unchanged. B-DEV-7's coordination item (`kMGPipeResourceTargetBuffer
  == 0`) is still open for one look from `espryt`; nothing in `wire-v2` contradicts it, and
  `ResourceEmit.EveryBufferTargetSetsItsBindMaskBit` asserts `created.Target == 0` on every target.

Two **new** deviations this round:

**R1 — the three call sites go through a macro, `MGP_SET_BASE_INSTANCE`, not a bare call.** The review's line
was `MG_Pipe::MGPipeSetPendingBaseInstance(baseinstance);` and warned to *"check that when the lines land"*.
Checked: `PipeFill.h`'s whole declaration block is inside `#if MOBILEGL_PIPE_PUSH` (`:16-113`), so a bare call
does not merely fail to **link** in a pull build, it fails to **compile**. The macro
(`PipeFill.h:114-124`, `((void)0)` in the pull arm) is the shape `MGP_FILL` already has, one line per site,
carrying the **raw** `baseinstance`. G1 is 0/0/0/0, which is the proof the pull text did not move.

**R2 — `RefreshBindMask` is deliberately not called from the content emitters** (§2.2 B-M4(b)). This is the one
place this round declines a reviewer instruction, and the reason is another of the review's own findings.

---

## 4. Mutation evidence — 4 mutations, 4 reds, measured

Each applied to the rebased tree, `build-push` rebuilt, the suites run, the file restored with
`git checkout --` (the tree was verified clean after every one).

| # | mutation | verdict | case that went red |
|---|---|---|---|
| M1 | **B-C1 reverted**: `m_pendingBaseInstance = 0;` put back into `Tracker.h`'s `Reset()` | **RED**, 2/2 failed | `TrackerWalk.ABaseInstanceSurvivesTheFirstWalkOnAFreshContext` **and** `TrackerShippedEmitter.ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance` |
| M2 | **B-M4 reverted**: both `NoteBoundAs` calls commented out of `VertexInputEmit.h` | **RED**, 1/1 failed | `ResourceEmit.ADrawTimeIndexBindingPublishesElementArrayEvenWhenTheRespecifyCannotSeeIt` |
| M3 | **B-M1's hole reopened**: the clear removed from the validate point's no-live-context exit | **RED**, 1/1 failed | `TrackerShippedEmitter.ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance` |
| M4 | **m12 reverted**: the destroy gated on `MGPipeResourceSubsystemEnabled()` again instead of the create's latch | **RED**, 1/1 failed | `ResourceEmit.ADestroyFollowsTheCreateEvenIfTheOpTableWasUnregisteredMeanwhile` |

No mutation survived. The restored tree is `100% tests passed, 0 failed out of 70` on
`ctest -R 'ResourceEmit\.|VertexInputEmit\.|Tracker'`.

---

## 5. For the integrator and for package C (espryt)

1. **B-M3, unchanged and still live — `hostBytes` is `shadow + offset` on two of the three content calls.**
   `espryt-v1.md` §1 e1 latches *"the client's shadow base"*; the client sends, per D-A2 and correctly:
   * `MGPipeEmitResourceRespecify` → `buffer.MappedData()` — the **true base** (`PipeFill.cpp:640`);
   * `MGPipeEmitResourceSubData` → `base + at` where `base = buffer.MappedData()` — **base + offset**
     (`PipeFill.cpp:650`, `:659`);
   * `MGPipeEmitResourceFlushRange` → `buffer.MappedData() + offset` — **base + offset**
     (`PipeFill.cpp:701`);
   * `MGPipeEmitBufferSubDataResident` → the application's staging store, which `espryt-v1.md` D10 already and
     correctly refuses to latch.

   So after any `glBufferSubData(b, 4096, …)` Espryt's `hostBytes` is `shadow + 4096`, and the next pool reseed
   or full re-upload reads 4096 bytes past the store's end. **B is brief-conformant and does not change**;
   espryt's latch is the side that moves — `hostBytes = bytes - MGPipeSubDataBufferOffset(record)` /
   `bytes - record.Offset`, or drop the latch on the two ranged calls and keep it only on respecify.
   `espryt-review-v1.md`'s C-2 is a **different** defect (a cached base dangling after adoption/resize) and its
   fix does not touch this one. The shape that catches it is a `StreamedArenaScenario` recycle plus a
   `CrossFrameBufferScenario` flush at a non-zero offset.
2. **`kMGPipeResourceTargetBuffer == 0`** (B-DEV-7) still wants one confirming look from espryt.
3. **`MGPVertexBuffer::BindingIndex` is the attribute index for every entry** (`VertexInputEmit.h:222`), which
   is what `espryt-v1.md` §6 item 14 says `VertexBufferForBindingIndex`'s fast path assumes. Confirmed.
4. **`OnGpuWritten` is announced as one `kMGPipeWholeBuffer` range** — the client now asserts that shape rather
   than ignoring it (m5). A backend that announces zero ranges gets nothing marked, deliberately.
5. **The resident dispatch gate** (wire §6 item 7) is already the member, not the subsystem:
   `BufferObject::LandBytesIntoResidentStore` asks `MGPipeResourceOpsHaveSubDataResident()`
   (`BufferObject.cpp:435`), which reads `ops->SubDataResident != nullptr`.
6. **`map-persistent-roundtrips` is still 0 on this tree** and becomes non-zero the moment espryt registers the
   table (G10). `MOBILEGL_PIPE_PUSH=0x7f` vs `0x1ff` is still emission-only (G12) until then.
7. **The three `MGP_SET_BASE_INSTANCE` sites are now the gate ID-10 describes**: espryt's
   `DrawParameters` / `VertexAttribBinding` base-instance scenarios must be red without them and green with
   them, and the integrator checks both directions once on the finished tree. Package C may now delete
   `g_pendingFetchBaseInstance` without losing the emulation's input.

---

## 6. Left undone, and what it costs

1. **m7 — `set_index_buffer` has no content suppressor.** Bit 10's shutter mixes `vaoIdentity`, which includes
   `GetConfigVersion()`, so any VAO reconfiguration re-emits an identical `MGPIndexBuffer` and moves the
   applier's `IndexBufferSerial` — the memo espryt's `m_syncedIndexSerial` early-out reads. **Correctness is
   fine (over-fire only); the cost is a measurable per-draw one for a reconfiguring app.** Not taken because
   latching `m_lastIndex` changes emission counts that `TrackerWalk.TheIndexBufferBit*` and espryt's memo cases
   both observe, and ID-6 makes performance a recorded item rather than a gated one. The fix is a latch plus a
   skip and belongs with the phase that measures it.
2. **The `GL_Buffer.cpp` bind hook** — three one-line `NoteBoundAs` calls in `BindBuffer_State`,
   `BindBufferBase_State`, `BindBufferRange_State`, for the seven `BindMask` bits nothing keys on yet. Now the
   *only* residue of B-DEV-2: a buffer bound, never drawn from, and never re-specified afterwards. Still
   nobody's file; still invisible in monolith.
3. **`delete_vertex_elements` and the VAO CSO slot free** (B-DEV-8) — package C's, by the ownership split. On
   this tree VAO CSO slots are never freed.
4. **`RefreshBindMask` from the content emitters** (R2) — declined with a reason, not forgotten.
5. **The `MOBILEGL_PIPE_POISON_OMIT` caselist sweep** — declared off the critical path; the `FillPoints.def`
   comment still says the verdict lands with the G15 caselist run and every row stays.
6. **The review's §7 items that are the integrator's, not a package's**: the `Blob.Size` off-by-one negative
   control on the merged tree (§7.3), and the `TheXIsActuallyArmedWhenTheEnvironmentPinsItOn` family's race on
   shared log files under concurrent load (`MG_IntegrationTest/CMakeLists.txt:1040`). This round's lanes were
   run **serially, with no competing load**, and were clean first time — see §7.

The review's §7.2 (mirror-image applier assertions on every case) is **partly** discharged: the new cases all
read `MGPipeApplier()` directly (`VertexFetchBaseInstance`, `VertexBufferCount`, `Resources[slot].Live/Serial/
Desc.Width`, `RefusedResourceCalls`), and wire's own nine cases do the same from the other side. Turning
`EveryAttributeFieldSurvivesTheWireConversion` into a full round trip through
`MGPipeVertexElementsRecord::Attributes[]` is **not** done: G7's negative control is defined over the
conversion, and adding a second oracle to the same case would make a scripted field-drop fail twice for one
reason. It is a second `EXPECT` beside an existing one whenever the integrator wants it.

---

## 7. Verification transcript

All in `/home/swung/w7/p3a-client` at `dfaa8729`, `CCACHE_BASEDIR=/home/swung/w7`, on the rebased tree.

**G1 — pull build symbol identity, against `~/w7/p3a-before-libMobileGL.so` (the `5cb826b0` baseline, ID-9)**

```
python3 scripts/symbol_report.py --before ~/w7/p3a-before-libMobileGL.so --after build-linux/libMobileGL.so \
        --threshold 0 --fail-on-symbol-set-change --fail-on-added-bytes 0                        rc 0
symbol-report: before: /home/swung/w7/p3a-before-libMobileGL.so (19114360 bytes on disk)
symbol-report: after : build-linux/libMobileGL.so             (19114360 bytes on disk)
symbol-report: .text 10806323 -> 10806323 (+0, +0.000%)   .data +0   .bss +0   .rodata +0
symbol-report: Total 17226471 -> 17226471 (+0)
symbol-report: 27811 -> 27811 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
symbol-report: 27072 -> 27072 normalised names, 27072 unchanged
```

**0 / 0 / 0 / 0, and the two libraries are byte-identical in size.** `~/w7/p3a-before-head.txt` reads
`5cb826b01e00dd28a6e62c1aab65e3edc50e5758`, which is this branch's base, so the baseline problem `client-v1`
and `wire-v2` both raised is closed by the rebase. P3a's admitted-resize set stays empty.

**Three builds** (`cmake --build <dir> -j 24`): `build-linux` rc **0**, `build-push` rc **0**,
`build-verify` rc **0**.

**Three unit runs** (`ctest --test-dir <dir> -L unit -j 12 --no-tests=error`):

```
build-linux    rc=0   100% tests passed, 0 failed out of 1617
build-push     rc=0   100% tests passed, 0 failed out of 1617
build-verify   rc=0   100% tests passed, 0 failed out of 1617
```

(1599 at `12e6bfcf` + b4's 13 + this round's 5.)

**Names — G2's name half, and G14**

```
ctest -N: build-linux 2537, build-push 2537, build-verify 3369
diff <linux names> <push names>                                    -> EMPTY (rc 0)
comm -23 <p3a-before-ctest-names.txt> <linux names>                -> 0 removed
comm -13 ...                                                       -> 35 added
```

The baseline is `p3a-before-ctest-names.txt`, 2502 names taken at `5cb826b0` (which already carries the
contract's 5). The 35 added are therefore **wire's 17** (all `ResourceEmit.*`), **b4's 13**
(4 `ResourceEmit`, 7 `VertexInputEmit`, 2 `TrackerWalk`) and **this round's 5**
(`ResourceEmit.ABufferCreatedBeforeAMakeCurrentStillLandsItsSubDataAfterOne`,
`ResourceEmit.ADrawTimeIndexBindingPublishesElementArrayEvenWhenTheRespecifyCannotSeeIt`,
`ResourceEmit.ADestroyFollowsTheCreateEvenIfTheOpTableWasUnregisteredMeanwhile`,
`TrackerWalk.ABaseInstanceSurvivesTheFirstWalkOnAFreshContext`,
`TrackerShippedEmitter.ABaseInstancedDrawAfterAMakeCurrentPublishesItsOwnBaseInstance`).
**Nothing removed, in any of the three builds.**

**Generators and closure**

```
python3 scripts/gen_pipe.py --check                        rc 0  "generated files are up to date"
                                                                 71 calls, 71 verify payloads,
                                                                 63 PipeInputs fields (7 sticky, 34 emitted)
                                                                 inventory 477 rows, 0 UNMAPPED
git diff --exit-code -- MobileGL/MG_Pipe/generated          rc 0
python3 scripts/gen_pipe_dirty_surface.py --check           rc 0  "75 mutators, all mapped, no stale rows;
                                                                 45 render-state answers derived and matching;
                                                                 10 other answers derived; 0 COARSE; 0 UNDECIDED"
python3 scripts/gen_pipe_dirty_surface.py --self-test       rc 0  "21 negative controls, all tripped;
                                                                 positive controls OK"
python3 scripts/check_include_closure.py                    rc 0  "4 probes, 0 skipped, 0 problem(s)"
                                                                 mutation-header OK 84 headers, 0 forbidden
python3 scripts/check_include_closure.py --mode both --compiler clang++ --self-test --require-all   rc 0
                                                                 "6 negative-control trip(s), parser checks OK"
```

(`--compiler clang++-20` does not exist on this host; `clang++` is the substitution the contract and client
packages both recorded.)

**G13 / layering**: `grep -c 'MG_State' MG_Pipe/PipeApply.h` → **0**; `grep -rc 'pGLContext' MG_Backend | grep
-v ':0$'` → empty.

**The four lanes** — run **serially, with no competing load**, which is the protocol `client-v1` established
after chasing the `TheXIsActuallyArmedWhenTheEnvironmentPinsItOn` race. All four green first time, no re-runs:

```
ctest --test-dir build-push  -L integration-gpu -j 8                         rc 0  100% passed, 0 failed of 920
MOBILEGL_PIPE_PUSH=0    ctest --test-dir build-push -L integration-gpu -j 8   rc 0  100% passed, 0 failed of 920
MOBILEGL_PIPE_PUSH=0x7f ctest --test-dir build-push -L integration-gpu -j 8   rc 0  100% passed, 0 failed of 920
ctest --test-dir build-verify -L integration-verify -j 4                     rc 0  100% passed, 0 failed of 832
```

With the applier bodies real and **no backend registered**, every emission still falls through: nothing
registers `MGPipeResourceOps` anywhere in the tree outside `MG_Test/Pipe/ResourceEmitTest.cpp`, so
`MGPipeResourceSubsystemEnabled()` is false in all three arms and the observable is unchanged. That is what
makes the `0` and `0x7f` arms identical to the default one on this tree, and what makes G12 still an
emission-only A/B until espryt lands.

**The retrace gate, verify-armed** — `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree
~/w7/p3a-client --lib ~/w7/p3a-client/build-verify/libMobileGL.so --out ~/w7/retrace-out/p3a-client -j 4`:

```
retrace rc=0
PASS MobileGLTraceReplay.minecraft-1.21.4-main-menu.DirectVulkan            rc=0   3s ssim=0.997009
PASS MobileGLTraceReplay.minecraft-1.21.4-rd12-odinlite-in-world.DirectGLES rc=0  63s ssim=1.0
PASS MobileGLTraceReplay.minecraft-1.21.4-rd12-odinlite-in-world.DirectVulkan rc=0 79s ssim=0.999991
PASS MobileGLTraceReplay.minecraft-1.21.4-startup.DirectGLES               rc=0   1s ssim=1.0
PASS MobileGLTraceReplay.minecraft-1.21.4-startup.DirectVulkan             rc=0   1s ssim=1.0
passed 79 / 79; failed: []

grep -l 'Fatal{'          ~/w7/retrace-out/p3a-client/*.log   -> empty
grep -L 'MGPipe verify:'  ~/w7/retrace-out/p3a-client/*.log   -> empty   (all 79 armed)
ls ~/w7/retrace-out/p3a-client/*.log | wc -l                  -> 79
```

This is again the strongest single result in the document, and this time it is on a tree where the applier's
bodies are **real**: with the emission live in a verify build, whose compare-at-read re-reads every field from
the live context at every backend read, the whole corpus replays with zero divergence and zero unmigrated
reads on both backends. `~/w7/retrace-out/p3a-client` was deleted afterwards. No
`Fatal{UnmigratedPipeInput,"F@V"}` appeared, so no `FillPoints.def` row was owed and none was added.

**Ownership**: `git diff --stat 12e6bfcf..HEAD` names `MG_Impl/Pipe/{PipeFill.h,PipeFill.cpp,Tracker.h,
ResourceTracker.h,VertexInputEmit.h,SetHashSuppressor.h}`, `MG_Pipe/{PipeMutation.h,DirtySurface.def,
FillPoints.def}`, `MG_State/GLState/BufferState/BufferObject.cpp`, `scripts/gen_pipe_dirty_surface.py`, the
three `MG_Test/Pipe/*.cpp`, and `MG_Impl/GLImpl/Drawing/GL_Drawing.cpp` (**+3 lines, the ID-10 grant**). No
`MG_Backend`. `git status` clean.

---

## 8. Intermediate logs and working state

Deleted at the end of the round: `~/w7/p3a-client2-lanes.log`, `~/w7/p3a-client2-lane-*.log`,
`~/w7/p3a-client2-retrace.log`, `~/w7/client2-lanes-inner.sh`, and `~/w7/retrace-out/p3a-client/`. No mutation
harness worktree was created — every mutation was applied in place and reverted with `git checkout --`.

`git status` in `/home/swung/w7/p3a-client` is clean at `dfaa8729`. The trace fixtures are materialised (real
archives, not LFS pointers) and hidden with `git update-index --assume-unchanged`, which is the state the next
run of the retrace gate needs.
