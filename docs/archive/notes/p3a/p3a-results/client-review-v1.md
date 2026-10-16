# P3a package B review — the client (`client-review-v1`)

Reviewed: `p3a/client` `73bab3d6 / 1ae6b4f4 / 71805d59 / a0ac79cc`, worktree `/home/swung/w7/p3a-client`, diffed
against `39722687`. Read-only; a private worktree at `a0ac79cc` was used for the scanner experiments in §6 and
removed (`git worktree list` is back to seven, `p3a-client` is clean at `a0ac79cc`).

## VERDICT: **REWORK**

Two critical defects, both latent on this tree and both live the moment the granted `*BaseInstance` call sites
and package `wire` land underneath. Five majors, one of which (the missing call sites) is known and granted.
The emission bodies themselves are, with the exceptions listed, careful and brief-conformant; the failures are
at the seams — draw state parked in a container whose `Reset()` is context-scoped, and a re-publication path
that was built for the CSO half and not for the resource half.

The package's own §5 evidence stands. I re-ran the four generator/closure gates and both scanner directions
myself (§6); all green, and the scanner's new mechanism has a live control.

---

## 1. Critical

### B-C1 — the tracker's context `Reset()` zeroes the pending base instance that the same call is about to read

`MG_Impl/Pipe/Tracker.h:373` — `Reset()` does `m_pendingBaseInstance = 0;`. `Reset()` is called from
`MGPipeTracker::Update` itself at `Tracker.h:194-197`:

```cpp
if (m_context != &ctx) { Reset(); m_context = &ctx; }
```

and the value is read two screens later at `Tracker.h:286` (bit 9's shutter) and at
`MG_Impl/Pipe/PipeFill.cpp:1335` (`tracker.PendingBaseInstance()` → `EmitVertexBuffers`).

**Failure scenario.** `eglMakeCurrent(ctxB); glDrawArraysInstancedBaseInstance(..., baseinstance = 7);`
The granted call site runs `MGPipeSetPendingBaseInstance(7)` immediately before `MGP_FILL`. `MGP_FILL` →
`MGPipeValidateForVerb` → `tracker.Update(*ctxB, ...)` sees `m_context != &ctxB`, calls `Reset()`, and the 7 is
gone before either reader sees it. `FreshlyPrimed` then sets every bit, so `set_vertex_buffers` **does** go out —
carrying `BaseInstance = 0`. Espryt computes `fetchBaseInstance = UseNativeBaseInstance() ? 0 : 0` and, on a
device without `GL_EXT_base_instance`, fetches every instanced array from element 0. One silently mis-shifted
instanced draw per context switch, on the emulation path, with no gate: the desktop retrace corpus is
single-context, `DrawParametersScenario` does not switch contexts between the setter and the draw, and the unit
case that pins the hash (`VertexInputEmit.ABaseInstanceChangeAloneStillEmitsTheVertexBufferSet`) calls
`EmitVertexBuffers(Ctx(), 7)` directly and never touches the tracker.

**Refutation attempted, three ways, all fail.** (a) *"Reset only happens at teardown"* — no; `Tracker.h:194`
fires on every change of the current `GLContext`, which is what `wire-review-v1.md`'s C1 established for the
applier through the same three lines. (b) *"the setter could run after Update"* — it cannot; D-H2.1 and B's own
`PipeFill.h:65-70` put it immediately **before** `MGP_FILL`, and `Update` is inside it. (c) *"a fresh context is
a fresh server, so clearing is right"* — the pending base instance is not context state, it is **this call's
argument**. Everything else `Reset()` clears is a latch describing what the server was told; this one is an
input that has not been read yet. The fact that `Reset()` clears it is the bug, not the mitigation.

**Fix (one line, in B's file):** delete `Tracker.h:373`. The value is already cleared unconditionally at the end
of every verb (`PipeFill.cpp:1446`) and again in `MGPipeLeaveVerb` (`PipeFill.cpp:801`), so it is provably 0 at
every `Update` except the one that immediately follows a setter — which is the one that must see it. Add the
case that would have caught it (§7).

### B-C2 — the resource family has no re-publication path after `MGPipeApplierReset()`; the vertex-input family has one

`PipeFill.cpp:1404-1412`, the `FreshlyPrimed` arm, resets four things: the CSO cache, the applier, the set-hash
suppressor and — B's addition, correctly — `MGPipeVertexInputEmitterInstance().Reset()`. Nothing resets or
re-arms `MGPipeResourceTrackerInstance()`, and `MG_Impl/Pipe/ResourceTracker.h:390-393` states the absence as
policy: *"A unit fixture's per-case reset. **Never called by the library**: a context change does not invalidate
a handle."*

That is true of the handle and false of the **record**. `MGPipeApplierReset()` empties `g_applier.Resources`
(`wire-review-v1.md` C1: *"At each switch `g_applier.Resources` is emptied (`:558`)"*), and the client's only
producer of a record is `MGPipeEmitResourceCreate`, emitted from the `BufferObject` **constructor**
(`MG_State/GLState/BufferState/BufferObject.cpp:44-45`), which a context switch does not re-run.

**Failure scenario.** Share-group buffer `B` created under `ctxA`. `eglMakeCurrent(ctxB)`; first draw →
`FreshlyPrimed` → `MGPipeApplierReset()` empties `Resources`. Now `glBufferSubData(B)`: the client emits
`ResourceSubData` for a handle whose record is no longer `Live`; the applier's `FindResource` returns null and
the call is **dropped** — *"nothing stored, nothing dispatched, no serial"* — with the only trace a
`MOBILEGL_ASSERT` that is inert at `INFO`, which is what both gate builds use. The upload is lost. Every later
draw from `B` renders stale geometry, and `IsBufferDrawCleanByHandle` reports clean because no serial moved.

**Refutation attempted.** (a) *"wire owns the reset, so this is wire's defect"* — wire raised it and explicitly
routed the decision here: *"B (whose `ResourceTracker.h` mints the handles) or the integrator must decide:
re-emit `ResourceCreate` + `ResourceRespecify` on first use after a reset, or take `Resources` /
`VertexElementsCsos` out of the reset … **This … must be settled before `p3a/client` lands**"*
(`wire-v1.md` §5 item 1). `client-v1.md` does not mention the item anywhere — not in §4's deviations, not in
§6's "left undone", not in §7's re-run list. (b) *"not reachable today"* — correct and irrelevant; it is
reachable the moment `wire` + `espryt` are underneath, which is the tree the integrator is building. (c) *"the
CSO precedent covers it"* — it does not, and B proved that by having to add `MGPipeVertexInputEmitterInstance()
.Reset()` for exactly this reason (`PipeFill.cpp:1408-1411`). The asymmetry is the finding: B saw the problem
for the half it could fix from the emitter and did not carry the reasoning to the half whose producer is a
constructor.

**Fix (in B's files, the option wire named "re-emit on first use"):** give `MGPipeResourceTracker` a
`Uint64 m_publishEpoch` and a per-`Entry` `PublishedEpoch`; bump `m_publishEpoch` from the `FreshlyPrimed` arm
beside the emitter reset; in `MGPipeEmitResource{Respecify,SubData,FlushRange,Readback,MapPersistent}` and
`…BufferSubDataResident`, when `entry.PublishedEpoch != m_publishEpoch`, emit `ResourceCreate` (and, for the
non-respecify calls, a `ResourceRespecify` carrying the live `GetSize()/GetUsage()/GetStorageFlags()`) before the
call and stamp the entry. That is the same shape `MGPipeVertexInputEmitter::EmitCreate` already has. If the
integrator instead exempts `Resources` / `VertexElementsCsos` from the reset, B still owes the deletion of
`ResourceTracker.h:390-393`'s claim and a line saying which decision was taken.

---

## 2. Major

### B-M1 — the three `MGPipeSetPendingBaseInstance` call sites (known, granted; here are the exact points)

`MG_Impl/GLImpl/Drawing/GL_Drawing.cpp`. The public entry points at `:1088`, `:1108`, `:1132` delegate to the
`*_Backend` functions, and `MGP_FILL` is in the latter. One line each, **immediately above** the `MGP_FILL`:

| function | file:line of the `MGP_FILL` | line to insert above it |
|---|---|---|
| `DrawElementsInstancedBaseVertexBaseInstance_Backend` (`:655`) | `GL_Drawing.cpp:662` | `MG_Pipe::MGPipeSetPendingBaseInstance(baseinstance);` |
| `DrawElementsInstancedBaseInstance_Backend` (`:678`) | `GL_Drawing.cpp:684` | `MG_Pipe::MGPipeSetPendingBaseInstance(baseinstance);` |
| `DrawArraysInstancedBaseInstance_Backend` (`:707`) | `GL_Drawing.cpp:713` | `MG_Pipe::MGPipeSetPendingBaseInstance(baseinstance);` |

The value is the **raw** `baseinstance` argument, never `EmulatedFetchBaseInstance(baseinstance)`: whether to
emulate the shift is the server's (`wire-v1.md` A1 stores `hdr.BaseInstance` unresolved and hands the decision to
package C). Each site must be inside `#if MOBILEGL_PIPE_PUSH` or behind a pull-build no-op — `MGPipeSetPending
BaseInstance` is declared in `PipeFill.h` outside any guard but defined at `PipeFill.cpp:783`, which is inside
the file's push-only region, so a pull build would fail to link if the call is unguarded. **Check that when the
lines land.**

**What the reset in `MGPipeLeaveVerb` must guarantee — and what it actually guarantees.** `grep` over
`MobileGL/` finds `MGPipeLeaveVerb` called from exactly two places: `MG_Test/ScopedPipeVerb.h:73` and
`MG_Test/Pipe/TrackerTest.cpp:651`. **No GL entry point calls it.** So `PipeFill.cpp:801`'s clear guarantees
only that a unit case which opens a `ScopedPipeVerb` cannot leak a base instance into the next case. The
production guarantee is entirely `PipeFill.cpp:1446`'s unconditional `tracker.ClearPendingBaseInstance()` at the
end of step 3, and the property it must hold is: *the pending value is consumed by exactly the verb whose entry
point set it, and is 0 at every other `Update`*. It holds — with one hole, `PipeFill.cpp:1371`'s
`if (ctx == nullptr) return;`, which skips the clear; a base-instanced draw with no live context would leave the
value standing for the next verb. Harmless in practice (a draw with no context is already a no-op) but it is the
one path the "neither of them relies on the other" claim in `client-v1.md` §4 B-DEV-3 does not cover, and
B-C1's fix makes `Reset()` no longer a second safety net. Move the clear above the `ctx == nullptr` return, or
say why not.

### B-M2 — the content emitters mutate the process-global slot allocator on a path Espryt documents as off-thread

`PipeFill.cpp:622, 656, 669, 696` each open with `MGPipeResourceTrackerInstance().Acquire(buffer)`, which is
`ResourceTracker.h:262-268`: `MGPipeSlots().Acquire(...)` (a `FindByLifetimeId` on a non-atomic map, then a
free-list pop and a map insert on a miss) followed by `m_bySlot.resize(slot + 1)`.

D-A2's `SubData` row preserves *"the off-thread/stale queue"* arm of `Ops_SubData` verbatim, i.e. the design
states that `BufferObject::NotifySubData` is reachable off the render thread. Today that path touches one
`const BufferBackendOps*` read and the backend's own mutex. B adds an unsynchronised insert into a global map
and a possible `Vector` reallocation to it. Two threads in `Acquire` for two different buffers, or one in
`Acquire` while another is in `Free` from `~BufferObject`, is a data race with a torn free list and a dangling
`m_bySlot` span as the outcomes.

**Refutation attempted.** *"Acquire is a lookup for an existing buffer, so it does not mutate"* — the lookup
itself walks a map another thread may be rehashing, and `m_bySlot.resize` is reached whenever the slot index
exceeds the vector, which a concurrent `Acquire` on the other thread can cause between the two statements.
*"contract-review item 13 already accepts non-atomic parity"* — that item is about the `g_resourceOps` pointer,
a single word written only at bring-up; this is a container mutated on the hot content path.

**Fix, and it is cheap and strictly better:** the handle is minted unconditionally in the constructor
(`BufferObject.cpp:44`), so on every content path it already exists. Replace `Acquire(buffer)` with
`Find(buffer)` (`ResourceTracker.h:273`, const, no allocator mutation) in
`MGPipeEmitResource{SubData,FlushRange,Readback}`, `MGPipeEmitBufferSubDataResident` and
`MGPipeEmitMapPersistent`, and treat a null handle as a loud `MGLOG_E_ONCE` + return (which is also the
diagnostic B-C2 needs). `Acquire` then survives only in the constructor path and in the two validate-time
vertex emitters, both of which are GL-thread by construction.

### B-M3 — `hostBytes` disagreement with package C: espryt latches the **shadow base**, the client sends **base + offset**

`espryt-v1.md` §1 e1: *"`GLESBufferResource` gains one push-only member, `const Uint8* hostBytes`: **the client's
shadow base** as the last content-carrying resource call left it. It is what the three-tier flush, the pool
reseed and the full re-upload read on the handle arm"*, and D10: *"(The three content-carrying calls that do
record it — respecify / sub-data / flush-range — all carry the client's own shadow.)"*

The client sends, per D-A2 and correctly: `PipeFill.cpp:623` `base = buffer.MappedData()` then `base + at` per
piece; `PipeFill.cpp:666` `buffer.MappedData() + offset`. Only `MGPipeEmitResourceRespecify`
(`PipeFill.cpp:614`) passes the true base. So after any `glBufferSubData(b, 4096, …)` Espryt's `hostBytes` is
`shadow + 4096`, and the next pool reseed or full re-upload reads 4096 bytes past the store's end.

**Refutation attempted.** *"B is wrong to add the offset"* — no: D-A2 spells `bytes = MappedData() + offset` for
`SubData` and `MappedData() + Offset` for `FlushRange`, and the applier hands the pointer straight to
`ops->SubData(res, record, bytes)` where the record's box carries the offset. B is brief-conformant; **espryt's
latch is the side that must change** (`hostBytes = bytes - MGPipeSubDataBufferOffset(record)`, or drop the latch
on the two ranged calls and keep it only on respecify). Recorded here because it is a merge-time defect that
neither package's own gates can see, and `espryt-v1.md` §6 item 14's confirmation list does not include it.

### B-M4 — the sampled `BindMask` widens the D-A3 hole in exactly the direction the risk register names, and the stated ownership reason misquotes C.1

`ResourceTracker.h:332-380` (`RefreshBindMask`), called only from `MGPipeEmitResourceCreate`
(`PipeFill.cpp:592`) and `MGPipeEmitResourceRespecify` (`PipeFill.cpp:606`).

Two separate problems.

**(a) The stated reason is wrong.** B-DEV-2 says the bind entry points *"are `MG_Impl/GLImpl/Buffer/GL_Buffer.cpp`'s,
which C.5 assigns to no package and **C.1 does not list for this one**"*. C.1's file table **does** list
`MobileGL/MG_State/GLState/BufferState/BufferState.{h,cpp}` with the note *"the sticky `everBoundAs` mask's bump
points at the buffer bind entry points (D-A3), all `#if MOBILEGL_PIPE_PUSH`"*, and C.5 gives B
`MG_State/GLState/BufferState/**`. I checked whether the brief was simply wrong about where the binds are:
`BufferState` (`BufferState.h:38-43`) only *vends* `BindingSlot<BufferObject>&` / `BindingSlotRange1D&`, and the
`.Bind()` calls are at `GL_Buffer.cpp:1463`, `:1518`, `:1531` and in `BindBufferRange_State` (`:1602`) — none of
them B's. So the **substance** of the deviation survives; the **sentence** does not, and the difference matters
because it reads as "no package owned it" when what is true is "the brief pointed at a file that turned out not
to contain the entry points". Correct the record.

**(b) The hole is not the one B describes.** B says sampling *"widens it from 'a bind after the last respecify'
to 'a bind after the last resource emission of any kind'"*. The real widening is a **transient** bind: with
OR-at-bind the mask is correct in client state forever after the bind, and any later respecify publishes it;
with sampling, `glBindBuffer(ELEMENT_ARRAY, ebo); glDrawElements(…); glBindBuffer(ELEMENT_ARRAY, 0);
glNamedBufferSubData(ebo, …); glNamedBufferData(ebo, …)` publishes a respecify whose sample sees no binding at
all, so `ELEMENT_ARRAY` is never set even though a respecify followed. The DSA idiom makes this the common case,
not the corner: `TryAdoptLargeStorage`'s own comment (`BufferObject.cpp:215-217`) names `glNamedBufferSubData` as
what MC 26.3 streams with, and a DSA-defined EBO may never be bound at any emission. `MGPResourceDesc::BindMask
& ELEMENT_ARRAY` is the `kCapNeedsHostIndexBytes` switch; getting it wrong silently disables restart rewriting
and multi-draw flattening under split, is invisible in monolith, and the brief's risk register says this is
*"the one P3a deliverable whose only real gate is a unit test"*.

**Fix, entirely inside B's files and better than the handed-on `GL_Buffer.cpp` hook for the bit that matters:**
`MGPipeVertexInputEmitter::EmitVertexBuffers` and `EmitIndexBuffer` already resolve, at every draw, exactly the
attribute buffers and the element-slot buffer (`VertexInputEmit.h:200`, `:240`). Call
`MGPipeResourceTrackerInstance().NoteBoundAs(handle, BufferTarget::Vertex)` / `…::Index` there. Any buffer ever
drawn from then carries its bit for the rest of its life, whether or not it was bound at a storage op, and the
`GL_Buffer.cpp` hook stays a P8 nicety for the seven bits nothing keys on. Also call `RefreshBindMask` from the
other four emitters, not just create/respecify — the mask is only *published* on create/respecify, so extra
sampling can only widen the sticky union.

Secondary, same function: `BindEpoch` (`ResourceTracker.h:414-425`) sums only the 14 global slot versions plus
the bound VAO's element-slot version and identity. It does not see the 84×4 indexed binding points. That is
sound today only because `BindBufferBase_State` also binds the generic slot (`GL_Buffer.cpp:1531`, with the
comment saying why); if that ever stops being true, `CONSTANT`/`SHADER_BUFFER`/`ATOMIC`/`STREAM_OUTPUT` start
being missed silently. One sentence in the comment, or fold `GetTouchedBufferBindingPointCount` into the epoch.

### B-M5 — `UnmapPersistent` has no producer, and the omission is not recorded

D-A2's row and my review brief both name the pair *"map/unmap persistent → `MapPersistent` / `UnmapPersistent`"*.
`PipeMutation.h:1626-1657` declares eight emitters plus the mint; there is no `MGPipeEmitUnmapPersistent`, and
`grep` finds no client call to `MGPipeApplyUnmapPersistent`. So a catalogue call that `wire` implemented is dead
in P3a and its gate can never go red.

**Refutation attempted, and it succeeds on the substance:** `BufferBackendOps` has seven hooks and none of them
is an unmap; `PipeResource::ReleasePersistentMap()` (`BufferObject.cpp:157`, from `RedefineStorage`) tells the
backend nothing today and the backend learns from the `Respecify` that follows. Emitting `UnmapPersistent` would
be **new behaviour**, which D-J forbids. So the right answer is "no producer in P3a, deliberately" — but
`client-v1.md` §6 does not say it, and a reader of the catalogue cannot tell a deliberate absence from a missed
hook. Add the row to §6 and a sentence beside the `MapPersistent` emitter.

---

## 3. Minor

| # | finding | file:line |
|---|---|---|
| m1 | `MGPipePendingBaseInstance()` has no caller anywhere, and its stated reason — *"Exists for the unit gate, which drives the emitter without a draw entry point to set it"* — is false: no test calls it (`grep` finds only the declaration and the definition). Either use it in the case B-C1 needs, or delete it. | `PipeFill.h:70`, `PipeFill.cpp:787` |
| m2 | contract-review must-know item 10 (*"B must reject an attribute index ≥ 256 before narrowing"*) is not discharged: a bare `static_cast<Uint8>`. Safe only by an invariant of `VertexArrayObject`, which is another package's file — the same "assert rather than assume" rule B applies, correctly, to the staging mirror at `PipeFill.cpp:1288-1300`. Same class: `entry.Stride = static_cast<Uint32>(attrib.Stride)` narrows a signed value with no check (contract-review m6 asks for both directions). | `VertexInputEmit.h:89`, `:206` |
| m3 | `MGPSubData::SourceIsVerbatimLevelShadow = 1` is set unconditionally by the shared builder, including on the `buffer_subdata_resident` path where the bytes are the application's staging store, and on the `FillSubData` path where they are a locally expanded pattern (`BufferObject.cpp:497-506`). The field's documented meaning is *"are these bytes an untransformed level shadow?"*; on those two callers the answer is no. Nothing reads it on the buffer path today. | `ResourceTracker.h:200`, `PipeFill.cpp:645` |
| m4 | `MGPipeBuildSubDataRecord`'s `Bool` return is discarded inside both walk lambdas. Unreachable because the pre-pass at `ResourceTracker.h:235-237` proves every piece encodable first — but if that pre-pass is ever relaxed, a `false` emits a zeroed record (null handle, size 0) rather than refusing. One `if`. | `PipeFill.cpp:626-629`, `:644-647` |
| m5 | `MGPipeClientOnGpuWritten` discards `rangeCount`/`ranges`. `espryt-v1.md` D8 makes the shape a contract point *"the client's `OnGpuWritten` must read it that way"*: one range of `kMGPipeWholeBuffer`, deliberately not zero ranges, because zero will mean a fully narrowed set at P8/P9. B's implementation would mark a whole buffer written for a zero-range announcement. Safe direction today; encode the contract (assert the shape, or name it in the comment). Same function resolves-to-null silently, unlike its sibling at `:461-464`. | `ResourceTracker.h:485-489` |
| m6 | `ResourceEmit.EveryBufferTargetSetsItsBindMaskBit` asserts `respecified.BindMask == MGPipeBindMaskForBufferTarget(target)` — the emitted value against the table under test. It pins the plumbing, not the table, for 14 of the 15 targets; only `Index` gets a literal (`:228`). The risk register calls this case the only real gate on the `ELEMENT_ARRAY` bit. Assert literals per target. | `ResourceEmitTest.cpp:214-216` |
| m7 | `set_index_buffer` has no content suppressor and bit 10's shutter mixes `vaoIdentity`, which includes `GetConfigVersion()`. Any VAO reconfiguration therefore re-emits an identical `MGPIndexBuffer` and moves the applier's `IndexBufferSerial`, which is precisely the memo espryt's `m_syncedIndexSerial` early-out reads. Correctness is fine (over-fire); it is a measurable per-draw cost for a reconfiguring app. Latch `m_lastIndex` and skip the apply when unchanged. | `VertexInputEmit.h:235-245`, `Tracker.h:293-301` |
| m8 | `DirtySurface.def:292-294`'s new comment states that `MarkVertexArrayForDeletion` *"→ `delete_vertex_elements`, published through the death notice"*. By B-DEV-8 this package emits no `delete_vertex_elements`; the row's comment asserts a publication that becomes true only when package C lands. Mark it "(from `espryt`)". | `DirtySurface.def:292-294` |
| m9 | Bits 7 and 8 are documented as an independent per-subsystem A/B, and are not: `EmitVertexBuffers`/`EmitIndexBuffer` name buffer handles by `{slot,gen}` whether or not the resource family created a record for them, so a mask with bit 8 set and bit 7 clear sends the server handles it cannot resolve. Not reachable from `0x1ff` or from G12's `0x7f`; state the coupling where an operator reads it. | `VertexInputEmit.h:200`, `:240` |
| m10 | `desc.Width = static_cast<Uint32>(buffer.GetSize())` truncates silently for a store ≥ 4 GiB; the applier's range gate then aborts a legal write as `Fatal{ProtocolCorruption}`. The 32-bit `Width` is the contract's, not B's, but the truncation should be a refusal or a `MOBILEGL_ASSERT`. | `ResourceTracker.h:180` |
| m11 | `MGPSubData::Blob` is filled (`Seg`, `Size`) while `wire-review-v1.md` M-E leaves it an open one-rule decision (*"either gate `Blob.Size` when non-zero, or state … that B must not fill it"*). Harmless — the applier never reads it — but B and wire must not settle it differently. | `ResourceTracker.h:201-203` |
| m12 | Create/destroy pairing is gated in two different places: the create at the call site (`BufferObject.cpp:45`), the destroy inside the helper (`PipeFill.cpp:699`). A buffer constructed while a table is registered and destroyed after `UnregisterBufferBackendOps()` frees its slot without emitting `ResourceDestroy`, leaving the applier record `Live` and Espryt's twin (a driver buffer id) alive on a slot about to be re-handed-out. Espryt's M2 (`GetOrCreate(MGPipeHandle)` adopting a stale generation) is the only thing standing between that and a cross-buffer id mix-up. Latch the decision on the `Entry` at mint time and use the latched value in both places. | `BufferObject.cpp:45`, `:55-57`; `PipeFill.cpp:695-706` |

---

## 4. Judgement of the nine declared deviations

| dev | verdict |
|---|---|
| **B-DEV-1** declarations in `PipeMutation.h` | **Correct, and the only shape available.** `MG_State` may not include `MG_Impl/Pipe/ResourceTracker.h`; the closure probe still reports `mutation-header OK, 84 headers, 0 forbidden` (I re-ran it). The forward declaration of `BufferObject` is the whole coupling. Accept. |
| **B-DEV-2** sampled `BindMask` | **Accept the mechanism, reject the reasoning and the hole assessment.** See B-M4. |
| **B-DEV-3** bit 9's shutter gains the pending base instance | **Correct and necessary; keep it.** D-H4 read literally does not work, for exactly the reason B gives: neither `GetAnyVaoAttributeGeneration` nor `vaoIdentity` moves on a base-instance-only draw, so the emitter is never reached and the content hash never gets its chance to refuse suppression. The over-fire is bounded to draws that carry one and the first plain draw after. **But** it is what makes B-C1 fatal: the deviation put draw state into the shutter without moving it out of the container whose `Reset()` is context-scoped. Accept with B-C1's fix. |
| **B-DEV-4** what the widened scan found | **Accept.** Both rows are real (`BumpSamplingResolutionGeneration` had no row at all; `NoteUnitTouched` is unreachable by `pGLContext->`), the `kPulledPartialShutter|NEW_SAMPLER_VIEWS` answer is derived and checked, and declining to mint a `kNoteMutation` prose word was the right call — a new prose answer is a row no derivation can back. I verified the mechanism has a live control (§6). |
| **B-DEV-5** `TrackerTest` follows `kMGPipeDirtyEmittedAtP3a` | **Accept.** The name is unchanged (G14), the property is unchanged, the P2 constant was not edited in place, and the three new arms are asserted by name. |
| **B-DEV-6** the splitter is a capped walk, cap as a parameter | **Accept, and it is better than the brief.** D-A2's "the emitter must split" is genuinely unreachable at the record's own bounds — a second piece starts ≥ 2³²−1 past the first, already past the 2³¹−1 offset cap — and refusing whole (all-or-nothing, nothing emitted before the decision) is the only safe answer for a content write. The unit case drives the split at 32 MiB, which is the bound P5 will actually hit. |
| **B-DEV-7** `kMGPipeResourceTargetBuffer` / `MGPipeBindBit` spelled client-side | **Accept, with the coordination item live.** `Target = 0` for the buffer arm matches `MGPipeTypes.h:148`'s documented order and a zero-initialised record. Nothing in `wire-v1.md` or `espryt-v1.md` states a different value; the integrator must still confirm it once, as B asks. |
| **B-DEV-8** no `delete_vertex_elements` here | **Accept.** C.1's emitter list names only three, and the consumer is `MG_Backend`'s. See m8 for the `.def` comment. |
| **B-DEV-9** the two client callbacks are installed over a null slot only | **Accept.** Installing from the mint's one-shot means they exist before any backend can call them, and never over an entry a backend claimed. See m5 for the shape. |

Two things `client-v1.md` claims that I could not sustain: §4 B-DEV-3's *"neither of them relies on the other
being called"* (B-M1 — `MGPipeLeaveVerb` is called by no GL entry point, so in production there is only one
clear, and `PipeFill.cpp:1371`'s early return skips it), and §2's *"the verify lane cannot regress on account of
a skipped field — there is none"*, which is true and which I re-derived independently (§5).

---

## 5. Dimension-by-dimension, where I did not raise a finding

**(1) Emission correctness.** All eight/nine dispatch sites carry exactly one call and fall through unchanged:
`~BufferObject` (`:50-57`), `NotifyRespecify` (`:67-71`), `NotifySubData` (`:83-87`), `NotifyFlushMappedRange`
(`:99-106`), `TryAdoptLargeStorage` (`:233-237`), `SyncGpuWrites` (`:371-379`), `LandBytesIntoResidentStore`
(`:430-452`), `FillSubData`'s nullability probe (`:488-495`), `EnsureGpuResidentStorage` (`:598-604`),
`AcquireMemoryRange`'s persistent arm (`:650-658`). I diffed each push arm against the pull arm below it
statement by statement; the guards (`m_size != 0`, `bytes.size > 0`, `!IsGpuResident && Write && !FlushExplicit`)
are reproduced exactly, and the `size == 0` and decline fall-throughs land on the same code. `NotifyRespecify` is
called after `m_size`, `m_storageFlags`, `m_isImmutableStorage` and `m_hasDefinedContent` are all final
(`:187-189`, `:206-210`), so `Immutable` — the `kNeedsAck` predicate's discriminator — is right for both idioms.
`initialBytes` is `MappedData()` only under `HasDefinedContent`, which preserves the orphaning idiom.
`MGPipeResourceOpsHaveSubDataResident()` discharges `wire-review-v1.md` n7 *before* wire wrote it down — the
client's dispatch gate on the resident path is the member, not the subsystem predicate. The `BufferTarget`
`static_assert` is complete (no `default:`, `kMGPipeBindUnmapped` sentinel, a second assert pinning
`Index ⇒ ELEMENT_ARRAY`).

*"`ResourceSubData` on a buffer with no prior respecify"*: not reachable from a legal GL sequence. A
`glBufferSubData` on an unsized buffer is `INVALID_OPERATION`/`INVALID_VALUE` at the GL layer and never reaches
`NotifySubData`; a zero-size `glBufferData` followed by a non-empty `glBufferSubData` is likewise rejected
upstream (and `NotifySubData` early-returns on `size == 0` at `:82` before any emission). The applier's `Width`
gate is therefore armed against a client bug, not against an application. B-C2 is the one way it fires.

**(2) Vertex input.** The wire conversion is field-for-field (`VertexInputEmit.h:80-91`) and I checked that each
of the eleven copied members is individually detectable by
`EveryAttributeFieldSurvivesTheWireConversion`: the fixture's three configuration families give every field at
least one non-default value across the 32 slots (`Offset` 64+4i, `Stride` 16+i / a binding-model 0, `Type`
cycling six values, `Size` 1..4, `Enabled` alternating, `Normalized` i%3, `IsInteger` i%5, `IsLong` on the two
L-format doubles, `IsBgra` on the tail, `BindingIndex` deliberately `(i+5)%32 ≠ i`). Each assertion is a
separate `EXPECT_EQ` naming both sides, under a `SCOPED_TRACE` carrying the index, so G7's scripted drop answers
by field name — B's hand-run of the `IsBgra` drop is representative, not lucky. `AttributeCount`/
`BindingPointCount` are both 32 and `Blob.Size = 32*24 + 32*16 = 1280` exactly, which is what
`wire-v1.md` §5 item 2's `Fatal{ProtocolCorruption}` gate requires. `Start = 0`, `Count = highest enabled + 1`
means no *enabled* attribute can ever index past `Count`, so wire's "entries outside the window are not cleared"
cannot bite. `BindingIndex == attribute index` matches what `espryt-v1.md` §6 item 14 says
`VertexBufferForBindingIndex`'s fast path assumes. The per-handle latch is per handle and includes `Gen`
(`VertexInputEmit.h:172-174`), so a recycled slot re-creates. The suppressor's reserved-0 rule is the
suppressor's own (`SetHashSuppressor.h:62-63` remaps a computed 0 to 1); the emitter does not need to.
`SetIndexBuffer` is emitted independently of the CSO's configuration version. `SyncClientSideAttributesForDraw
Arrays` is untouched and `Res == kMGPipeNullHandle` for a client array is emitted as designed.

**(3) Tracker.** The three `MGPipeSubsystemForDirty` arms are present and paired
(`Tracker.h:141-148`, asserted by the P2 `static_assert`s with no edit, exactly as the contract commit asked).
Bit 10's narrowing is real and its two cases are well built — in particular the wrap case cycles **three**
buffers, because two would put the version and the bound object on the same parity and the wrap would be
unobservable; that is the one construction that makes the identity half load-bearing.

**(4) Verify-lane soundness.** With bits 7 and 8 in `kMGPipeWiredSubsystems` (`PipeFill.cpp:918-924`), `supplied`
at `PipeFill.cpp:1470` can newly be true only for a field whose `SubsystemForEmitter` returns one of the two.
`kMGPipeSubsystemResources` is returned for no `MGPipeFieldEmitter` (the resource family has no `Coverage.def`
emitted row), and `kMGPipeSubsystemVertexInput` names exactly `GetBoundVertexArray`, for which
`EmittedCallSuppliesTheWholeField` returns `false` (`PipeFill.cpp:964-969`). **No field is newly skipped; P3a
retires no pull.** That discharges contract-review must-know item 3, which said the omission would be silent and
catchable only by G4 — and the 79/79 verify-armed retrace with zero divergence and zero unarmed cases is the
independent confirmation. Ordering: the seven resource hooks emit after the frontend state they read is final
(above); the three vertex emitters run at validate time, after the entry point's state writes.

**(5) G1.** `BufferObject.h` is untouched; no member, no virtual, no base was added; every edit is inside
`#if MOBILEGL_PIPE_PUSH` and the constructor and destructor are out-of-line. `sizeof(BufferObject)` and its
vtable cannot move in a pull build, which is what the 0/0/0/0 reading against the original baseline says. The
one resize B found and fixed during b1 (`SyncGpuWrites`'s dropped `m_size == 0 ||`) is exactly the failure mode
D-K predicts and is the evidence the gate can go red. Per ID-1/ID-8/ID-9 the reading is valid against the
baseline it was taken against; the re-measure is the rebase's.

**(6) The scanner.** I ran all three directions myself in a throwaway worktree at `a0ac79cc`:
inserting a row for a mutator that does not exist → `rc=1`, *"STALE row SetSomethingThatDoesNotExist"*; deleting
the `BumpSamplingResolutionGeneration` row → `rc=1`, *"UNMAPPED mutator BumpSamplingResolutionGeneration"*;
and — the control B did not run — removing the new `MGP_NOTE_MUTATION` attribution from `scan_file` → `rc=1`,
*"STALE row NoteUnitTouched"*, so the **new mechanism itself has a live negative control**. `--check` reports
75 mutators, 0 UNDECIDED, 0 COARSE; `--self-test` trips all 21 canned controls; `--summary` renders and its
known-limits line is down to the two blind spots the `.def` still declares. `gen_pipe.py --check`,
`git diff --exit-code -- MG_Pipe/generated` and `check_include_closure.py` (4 probes, 0 problems) are green.

**(7) Tests.** All eleven names C.1 asks for are present, in the two suites C.1 names, as plain `TEST`s with an
RAII scope — B's reason for refusing `TEST_F` is right and is worth propagating: a fixture files cases under the
fixture's name and would silently move them out of `ctest -R 'VertexInputEmit\.'`, disarming G6 and G7. The pull
build declares the same names and skips, so the G2 name-set identity holds. No vacuous assertions except m6. Two
coverage gaps, both named above: nothing drives the pending base instance through `MGPipeValidateForVerb`
(B-C1), and nothing drives an emission after a `FreshlyPrimed` reset (B-C2).

**(8) Hygiene.** Four commits, single-line messages in `[Type] (Scope): description` form, no bodies, no
attribution lines; b1/b2/b3's messages are the brief's verbatim. Fourteen files, all B's by C.5 except
`MG_Test/Pipe/TrackerTest.cpp`, which C.5 assigns to nobody and which B-DEV-5 justifies. `git status` clean.

---

## 6. Rework list (ordered; 1-3 are blocking)

1. **B-C1.** Delete `Tracker.h:373` (`m_pendingBaseInstance = 0;` inside `Reset()`), leaving the two verb-scoped
   clears. Add `TrackerWalk.ABaseInstanceSurvivesTheFirstWalkOnAFreshContext`: set the pending value, `Walk()`
   against a *different* `GLContext` than the previous case used, assert `PendingBaseInstance() == 7` at the
   emitter and that bit 9 fired. Move `PipeFill.cpp:1446`'s clear above the `ctx == nullptr` return at `:1371`,
   or record why not.
2. **B-C2.** Implement the publish-epoch re-emission described in §1, **or** carry the integrator's decision to
   exempt `Resources` / `VertexElementsCsos` from `MGPipeApplierReset()`; either way delete the claim at
   `ResourceTracker.h:390-393` and add a §6 row. Add `ResourceEmit.AResetApplierGetsTheResourceRepublishedBefore
   TheNextWrite`.
3. **B-M1.** The three `MGPipeSetPendingBaseInstance(baseinstance)` lines at `GL_Drawing.cpp:662`, `:684`, `:713`,
   guarded so the pull build still links, plus one integration assertion that a base-instanced draw reaches the
   emitter with a non-zero value.
4. **B-M2.** `Find` instead of `Acquire` in the five content emitters; null handle → `MGLOG_E_ONCE` + return.
5. **B-M4.** `NoteBoundAs` from `EmitVertexBuffers`/`EmitIndexBuffer`; `RefreshBindMask` from the other four
   resource emitters; correct B-DEV-2's citation of C.1 in the result file; one sentence on `BindEpoch`'s
   dependence on `BindBufferBase_State` also binding the generic slot.
6. **B-M5.** Record the deliberate absence of an `UnmapPersistent` producer, with the reason (no
   `BufferBackendOps` unmap hook exists to convert).
7. **B-M3** is *not* B's to fix — hand it to the integrator and package C verbatim (§2).
8. Minors m1-m12 at B's discretion; m2, m5, m6 and m12 are the ones I would insist on, because each is a
   contract point stated elsewhere that this package is the last chance to encode.

## 7. Post-rebase verification the integrator must insist on

Rebase order is `feat/disaggregated@5cb826b0` → `wire` (post-rework) → `client`. B touches none of the files
ID-9 names as conflict-prone (`Managers.cpp`, `LargeArenaAdoptionScenario.cpp`), so the rebase should be clean
apart from the `ResourceEmitTest.cpp` / `VertexInputEmitTest.cpp` append that C.5 predicts — **resolve by union,
never by choosing a side**.

1. **Re-take both baselines at `5cb826b0`** and re-run G1 (`--threshold 0 --fail-on-symbol-set-change
   --fail-on-added-bytes 0`) and G14 (`ctest -N` name sets, pull vs push vs verify, and `comm -23` against the
   baseline). B's §5 caveat about the replaced baseline is correct and is now moot under ID-9; the reading has
   to be retaken, not inherited.
2. **The mirror-image assertions B's §7.1 owes.** Every `ResourceEmit` / `VertexInputEmit` case gains a second
   `EXPECT` on `MGPipeApplier()`: the stored `MGPipeResourceRecord::Desc` (Width, Usage, StorageFlags, Immutable,
   HasDefinedContent, BindMask), `MGPipeVertexElementsRecord::Attributes[]` / `BindingPoints[]`,
   `MGPipeApplierState::VertexBuffers[]`, `VertexFetchBaseInstance`, and the three `Serial`s.
   `EveryAttributeFieldSurvivesTheWireConversion` becomes a round trip; G6's statement is only then about what
   the backend reads.
3. **The `Blob.Size` gate, exercised.** `wire-review-v1.md` M-B measured that disabling it leaves the suite
   green. With `client` underneath, a deliberate `Blob.Size` off-by-one in `EmitCreate` must produce
   `Fatal{ProtocolCorruption}` in the verify build. Run it once as a negative control.
4. **B-C1's and B-C2's scenarios on a real applier.** A two-context integration case: draw with a base instance
   immediately after a make-current (assert `VertexFetchBaseInstance == 7`), and a `glBufferSubData` on a
   share-group buffer immediately after a make-current (assert the applier's `Serial` moved and, once C1b lands,
   that `RefusedResourceCalls` is 0).
5. **B-M3, before `espryt` is trusted.** Confirm what Espryt latches as `hostBytes` for `Ops_H_SubData` /
   `Ops_H_FlushRange`; if it is the raw `bytes` pointer, it is `shadow + offset` and must be corrected. A
   `StreamedArenaScenario` recycle plus a `CrossFrameBufferScenario` flush at a non-zero offset is the shape that
   catches it.
6. **The two contract points `espryt-v1.md` §6 item 14 lists**, now answerable: `OnGpuWritten` is announced as
   one `kMGPipeWholeBuffer` range (m5 — the client currently ignores the shape), and
   `MGPVertexBuffer::BindingIndex` is the attribute index for every entry (it is; `VertexInputEmit.h:207`).
   Add `kMGPipeResourceTargetBuffer == 0` (B-DEV-7) as a third.
7. **`map-persistent-roundtrips` becomes non-zero** the moment `espryt` registers the table (G10), and
   `MOBILEGL_PIPE_PUSH=0x7f` vs `0x1ff` becomes a behavioural A/B (G12) rather than an emission-only one.
   Both are currently unprovable and both are on the merged tree's critical path.
8. **The four lanes and the verify-armed retrace re-run on the merged tree**, with the note that
   `integration-gpu`'s `TheXIsActuallyArmedWhenTheEnvironmentPinsItOn` family races on shared log files under
   concurrent load (`MG_IntegrationTest/CMakeLists.txt:1040` records the shape); B's chase-down was correct and
   the quiet run is the result to record.
