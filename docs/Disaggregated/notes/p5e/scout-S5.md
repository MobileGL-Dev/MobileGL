# S5 — buffer binding points and the remaining rows (P5e scouting report)

Head `2fde7034`. Paths under `MobileGL/` unless they start with `docs/`. `DirectGLES.cpp` =
`MG_Backend/DirectGLES/DirectGLES.cpp`, `Managers.cpp` = `MG_Backend/DirectGLES/Managers.cpp`.
Kinds: **live** = frontend deref; **id** = lifetime id + client allocator probe; **rec** = record.

## 1. The family's live reads at this head

`GetBufferBindingPoint` is one row (`FieldOwnership.def:80-81`, BARRIER_PULLED, "P3b/P4b, P7")
whose storage is four raw bases into `Array<Array<BindingSlotRange1D,84>,4>`
(`PipeInputs.h:809`, filled `PipeFill.cpp:126-131`, `BufferState.h:29`, `:84-86`).

| site | reads | kind | when |
|---|---|---|---|
| `SyncBufferBindingPoints` `DirectGLES.cpp:491-531`, called `:979` (draw) / `:988-989` (compute) | count `GetTouchedBufferBindingPointCount` `:493`; `GetBufferBindingPoint(target,i)` `:510`; `point.GetBoundObject()` `:511`; `EnsureBufferResource(obj)` `:517`; `point.GetRange()` `:523` → `obj->GetSize()` (`Types.h:220-229`); `obj->GetSize()` `:524,:527-528` | rec (count, P5c rv); live + id (body) | count on EVERY draw; body 0 iterations on Minecraft (touched SSBO count 0) |
| `MarkShaderStorageBuffersGpuWritten` `:600-615`, called `:980`, `:990` | count `:601`; point + `GetBoundObject()` `:604-605`; `MarkBufferGpuWritten(obj)` `:606` → `HandleOfBuffer` (`Managers.cpp:2922-2941`, scope `:2933`) | rec; live + id | every draw AND every dispatch |
| UBO loop in `BindCurrentProgramWithResources` `:5447-5497` | `currentProgram->GetUniformBlockBinding(i)` `:5463`; `GetBufferBindingPoint(Uniform,binding)` `:5464`; `GetBoundObject()` `:5465`; `GetRange()` `:5466`; `GetBufferResource(bufferObj.get())` `:5472`; `IsBufferDrawClean(bufferObj.get(),…)` `:5475` → `HandleOfBuffer` (`Managers.cpp:3155-3159`); `EnsureBufferResource` `:5478` | live + id | **1–3 per draw on Minecraft, memo hit or miss** — `drawCleanEpoch` (`:5457,:5473-5479`) short-circuits the STORAGE work only, never the point read or the bind |
| `SyncAtomicCounterBuffers` `:617-673`, called `:5504-5506` | `GetBufferBindingPointCount(AtomicCounter)` `:621` (**sticky forward**, `PipeFill.cpp:1870-1874`); point `:628`; `GetBoundObject()` `:630`; `EnsureBufferResource` `:636`; `GetRange`/`GetSize` `:642-650`; `MarkBufferGpuWritten` `:658` | live + id | only for programs with counter blocks |
| `SyncTransformFeedbackBindingPoints` `:562-592`, called `:1420` | point `:569`; `GetBoundObject()` `:570`; `EnsureBufferResource` `:574`; `GetRange`/`GetSize` `:583-589` | live + id | only on a pending span |
| `StartPendingTransformFeedback` `:1381-1510`, called `:4829` | returns at `:1386` with no pending span; else `GetTransformFeedbackProgram()` `:1387` (`FieldOwnership.def:106`), `program->GetTransformFeedbackBufferCount()` `:1406`, point walk `:1407-1417`, and `xfb.targets.push_back({bufferObject,…})` `:1417` **holds the frontend `SharedPtr` past the verb** | live | tail of EVERY draw (one bool test in steady state) |
| `ReadbackCapturedRanges` `:1118-1160` (from `EndTransformFeedback` `:1527`) | under a transport still `HandleOfBuffer(target.buffer.get())` `:1138` | live + id | per span close |
| `GetBufferBindingSlot` `PipeInputs.h:808`, filled `PipeFill.cpp:116-124`, in kDraw's mask (`FillPoints.def:162`) | 15 pointers copied at every kDraw fill; **no inproc draw-path reader** — `:948-957` is the record arm (`MGPipeApplier().VerbIndirectBuffer`), `:961-966` the monolith `else` | cost only | every draw |
| `RecordError` | already a `kEventGlError` post under a transport (`PipeFill.cpp:1915-1931`); no frontend write from the apply thread | rec | — |
| `HasOpenTransformFeedbackSpan`, `GetProgramObject`, `ValidateProgramName`, `GetTextureObject` | sticky forwards, off the steady draw path (`FieldOwnership.def:147-165`) | — | — |

Every `id` above runs inside `MGPipeReverseAnnouncementScope` — CONTRACT-P5C §3.1's **first
named exemption**, granted verbatim because "`set_shader_buffers` and
`set_stream_output_targets` sit in the catalogue but are P4b's to emit"
(`CONTRACT-P5C.md:157-176`, guard `MG_Impl/Pipe/SlotAllocator.cpp:22-38`). This family IS that
exemption; retiring it retires the scope.

## 2. What the records and the applier already carry

- **The counts, and only the counts.** `MGPContextValues::TouchedBufferBindingPointCount[15]`
  (`MGPipeTypes.h:1692-1705`), emitted by `EmitContextValues` (`PipeFill.cpp:2679-2704`),
  RECORD_SUPPLIED since P5c rv. `:493`, `:601` and the client's own sweep
  (`GpuWritePending.cpp:61`, `:77`) all read it.
- **`set_shader_buffers` is catalogued and dead.** `PipeCalls.def:174`, `MGPShaderBuffers`
  (`MGPipeTypes.h:910-919`), `MGPBufferRange` (`:901-906`), wire op 38
  (`generated/PipeWire.inc:110,:211,:310,:698-704`), thunk (`PipeThunks.inc:168`), table slot
  (`PipeTables.inc:62`), codec layout + bounds + host-span honesty pass
  (`PipeWireCodec.cpp:204`, `:648-661`, `:1778-1801`) — and then `return false`: **no route row
  (`PipeRoute.h`/`.cpp` have none), no `Mono_`/`Wire_` binding, no
  `MGPipeApplySetShaderBuffers`, no applier state, no emitter.** `PipeCatalogueTest.cpp:170-171`
  and `:242-243` pin both slots null. `set_stream_output_targets` is identical
  (`PipeCalls.def:175`, `MGPipeTypes.h:922-928`, `PipeWireCodec.cpp:1805`).
- **The dirty bits exist and own nothing.** `MGPipeDirty::NewConstBuffers/NewShaderBuffers/
  NewSoTargets` (`Tracker.h:97-99`), `MGPipeSubsystemForDirty` answers 0 for all three
  (`Tracker.h:204-209`), pinned by `TrackerTest.cpp:385-387`. `SetHashSuppressor.h:65-66` marks
  both slots "P4b".
- **The buffer twin is already handle-capable.** `IsBufferDrawCleanByHandle`
  (`Managers.cpp:3109-3153`) reads the frontend only under monolith (`:3143-3145`);
  `EnsureBufferResourceForHandle(nullptr, h)` (`Managers.cpp:3193-3222`) needs no object and is
  already called that way at `DirectGLES.cpp:953`.
- **The GPU-write set already moved sides.** `MarkShaderStorageBindings`
  (`GpuWritePending.cpp:55-66`), `MarkAtomicCounterBindings` (`:68-81`),
  `MarkTransformFeedbackTargets` (`:37-53`), driven from `MarkGpuWritesForDraw` (`:145-151`) in
  `BeforeDrawVerb`. `GpuWritePending.h:9-55` is the ruling. The backend's `:606`, `:658`,
  `:2775` are a **duplicate** under a transport.

## 3. The gap

**3.1 The shutter is not merely unowned, it is wrong.** Bits 15/16/17 read
`ctx.GetAnyBufferChangeGeneration()` (`Tracker.h:525`, `:636-639`) — the buffer CONTENT
aggregate (`BufferState.h:69-72`). `glBindBufferBase/Range` mutate the point through a returned
reference (`GL_Buffer.cpp:1501-1520`, `:1624-1650`: `point.Bind()`, `point.SetRange()`), which
moves `BindingSlot::m_version` (`Types.h:189-194`) and nothing the tracker reads. So
`glBindBufferBase(UNIFORM,1,A); draw; glBindBufferBase(UNIFORM,1,B); draw` fires no bit — the
same defect class as P4a's `glBindSampler` hole (`Tracker.h:612-621`).
**And the mutation is invisible to the dirty-surface gate**: `MUTATOR_PREFIXES`
(`scripts/gen_pipe_dirty_surface.py:87-90`) has no `Touch`, and the point write is not a
`pGLContext->` call at all, so `BindBufferBase_State`/`BindBufferRange_State` have no row in
`DirtySurface.def` (83 rows; the nearest is `SetNamedTransformFeedbackBinding` →
`kPulledEveryVerb`, `:295`).

**3.2 New frontend state (push-build only).** `BufferState::NoteBindPointChanged(target)` beside
`NoteBufferChanged` (`BufferState.h:69-75`), one `Uint64` per `BufferBindPointTargets` entry,
bumped by the two entry points above and by `SetNamedTransformFeedbackBinding`. `#if
MOBILEGL_PIPE_PUSH` so the pull build's object does not resize (G1) — the licence
`TextureState::NoteImageUnitTouched` took at P5d r3 (`GpuWritePending.cpp:96-108`).

**3.3 Shutters.** bit 15 `Mix(bindPointGen[Uniform], programIdentity)` (the emitted window is
resolved FOR THE PROGRAM IN USE, 3.4/7.2 — `UseProgram`'s row `DirtySurface.def:311` already
carries this pattern); bit 16 `Mix(bindPointGen[ShaderStorage], bindPointGen[AtomicCounter])`;
bit 17 `Mix(bindPointGen[TransformFeedback], ctx.GetTransformFeedbackGeneration())`. The content
aggregate LEAVES all three — it is the resource family's business, and mixing it in fires on any
`glBufferSubData` anywhere. That is exactly the narrowing bit 10 already took
(`Tracker.h:545-553`).

**3.4 The record.** One `MGPShaderBuffers` + `MGPBufferRange[Count]` per **Class**
(`Uniform=0 | ShaderStorage=1 | AtomicCounter=2`, `MGPipeTypes.h:911`). `Start = 0`;
`Count` = the class's window (7.2) clamped to a new `kMGPipeMaxBufferBindingPoints`, pinned
against `BufferState.h:29`'s 84 in `PipeFill.cpp` where both are visible — the shape
`kMGPipeMaxVertexAttribs` is pinned with at `PipeFill.cpp:2093-2095`.
Entry: `Res = MGPipeSlots().Acquire(MGPipeKind::Buffer, obj->GetLifetimeId())` or null;
`Offset/Size` from `point.GetRange()` resolved CLIENT-side, with `Size = kMGPipeWholeBuffer`
(`MGPipeTypes.h:406`) for a base binding so the SERVER re-resolves against its own descriptor
(7.3). `WritableMask` (`MGPipeTypes.h:914`) set per entry for ShaderStorage/AtomicCounter — it
is what replaces the backend mark's membership question.
`HostSpanCount = 0` **always** for Espryt: `kCapNeedsHostUboBytes` is 0 for the whole of P5 by
ruling (`CONTRACT-P5.md:64`, `MG_Backend/Init.cpp:150`, `PipeWireCodec.cpp:1786`) and the bit is
Magma's named-UBO ring (`docs/Disaggregated/ARCHITECTURE.md:113`, `:121`, `:140`). The codec's
honesty pass (`PipeWireCodec.cpp:1783-1799`) stays the guard that says so if it ever is not.
**XFB is NOT this record**: `set_stream_output_targets` carries a `Generation`
(`MGPipeTypes.h:922-928`) this payload has no field for, and capture points are span-scoped
state latched at Begin. Keep the catalogue's split.

**3.5 Hash + suppressor.** `XXH64` over the tail with `Class/Start/Count` mixed —
`MGPipeShaderImageSetContentHash`'s shape (`ImageEmit.h:53-58`). `SetHashSuppressor.h:65` has
**one** slot for the call while the record is per class; three classes sharing it would make
each emission cancel the others'. Split into three slots (see 7.1).

**3.6 Subsystem bit.** Bit 13 is free (`MGPipe.h:105-109`: "bits 13..62 reserved"). Add
`kMGPipeSubsystemBufferBindings = 1ull << 13`; the three dirty bits leave
`MGPipeSubsystemForDirty`'s `default: return 0` arm (`Tracker.h:204-209`);
`SubsystemForEmitter` (`PipeFill.cpp:1989-2028`) gains a `SetShaderBuffers` row with the pairing
`static_assert`s beside P4a's (`PipeFill.cpp:2104-2126`). `TrackerTest.cpp:385-387` is rewritten,
not deleted.

**3.7 Coverage / ownership.** `Coverage.def`'s EMITTED list gains
`GetBufferBindingPoint → SetShaderBuffers`; SHAPE-ONLY like P4a's six (`Coverage.def:199-213`),
landing in `EmittedCallSuppliesTheWholeField`'s FALSE arm — the field is a pointer no applier can
produce. `FieldOwnership.def:80-81` stays BARRIER_PULLED until the four Espryt consumers are
gone, then becomes RECORD_SUPPLIED. `GetBufferBindingPointCount` (`:147-148`, `:179-180`) is
answered from the applier's `Count[Class]` and its forward becomes FATAL under split — the
`InvalidateCompileEnv` shape (`FieldOwnership.def:163-165`).

## 4. The retirement design (server shape)

**4.1 Applier state**, beside the three unit sets (`PipeApply.h:673-686`):
`Array<MGPBufferRange, kMGPipeMaxBufferBindingPoints> BoundShaderBuffers[3]`, `Uint32 Start[3]`,
`Count[3]`, `WritableMask[3]`, and ONE `Uint64 ShaderBuffersSerial` advanced on every applied
record and **never zeroed by `MGPipeApplierReset`** — `VertexBuffersSerial`'s rule, stated at
`PipeApply.h:609-616`. Entry point `MGPipeApplySetShaderBuffers(hdr, ranges)` validating
`Class < 3` and `Start + Count <= capacity`; the codec's bounds gate already runs
(`PipeWireCodec.cpp:648-661`).

**4.2 `SyncBufferBindingPoints` → `SyncBufferBindingPointsByRecord(Class, glTarget)`.** Count
from `st.Count[Class]`; per entry `EnsureBufferResourceForHandle(nullptr, entry.Res)`; the
whole-vs-range test becomes `entry.Offset == 0 && entry.Size == kMGPipeWholeBuffer`, so
`obj->GetSize()` disappears from the file. The clean probe is
`IsBufferDrawCleanByHandle(entry.Res, resource, nullptr)`, which
`Managers.cpp:3109-3153` already accepts with a null frontend. **No twin key is introduced**: a
binding point is not an object, it names one by handle. "Is it clean" is answered from
`resource->syncedChangeSerial` vs `ResourceRecordOf(res)->Serial` (`Managers.cpp:3151`) — both
server-owned — plus `ShaderBuffersSerial` for the SET.

**4.3 The GPU-write marks are deleted under a live wire.** `:606`, `:658` and `:2775`
(`MarkWritableImageBufferTexturesGpuWritten` `:2763-2781`). The client already owns the set
(2., `GpuWritePending.h:9-55`), and under run-ahead the server's walk is over client memory, so
keeping it is the one thing that cannot work. `WritableMask` survives as the server's record of
which points P9's narrowing channel will name.

**4.4 The UBO loop** `:5463-5480`: `GetUniformBlockBinding(i)` is **S4's** (the reflection
archive). Given the binding index, `point` becomes `st.BoundShaderBuffers[Uniform][binding]` in
the SAME index space — `Start` is 0 and a binding at or above `Count` means "nothing bound",
which is what the frontend array's default says too. `GetBufferResource(bufferObj.get())` `:5472`
becomes `FindBufferResourceForHandle(entry.Res)`.

**4.5 `SyncAtomicCounterBuffers`**: `pointCount` from `st.Count[AtomicCounter]`; the `glBindings`
list stays the backend program's (server-owned since link).

**4.6 XFB stays LOCKSTEP.** `StartPendingTransformFeedback` `:1381`,
`SyncTransformFeedbackBindingPoints` `:562`, the `targets` vector `:1417` and
`ReadbackCapturedRanges`' `HandleOfBuffer` `:1138` are not migrated by this package, because
(i) `GetTransformFeedbackProgram` is an object row no scout migrates, (ii) `xfb.targets` HOLDS
frontend `SharedPtr`s across two verbs, which rule C forbids an applier entry point outright,
and (iii) `ROADMAP.md:32` puts "XFB scatter 搬到 client" in P3b/P4b. Cost on the Espryt steady
path is zero: `:1386` returns on every draw. **The wait rule S6 needs from me:** a draw waits
`appliedSeq == seq` iff `ctx.IsTransformFeedbackActive()` — the flag already crosses in
`MGPContextValues` (`MGPipeTypes.h:1698`) and is client state, so the client can decide without
asking. Same for a dispatch whose program declares atomic counters, until 4.5 lands.

**4.7 `GetBufferBindingSlot`**: drop the 15-pointer copy from the kDraw residual fill under a
live wire (`PipeFill.cpp:116-124`, a class-and-transport test in the walk at `:3252-3269`). No
inproc draw-path reader exists; kReadback/kBlitOrCopy/kTextureOp keep it. **Not** a
`Coverage.def` row — that would claim the whole 15-target field is supplied, the half-truth
`Coverage.def:62-70` already refuses.

## 5. Monolith / G1

- Everything new is `#if MOBILEGL_PIPE_PUSH` and gated on
  `(PipePush & kMGPipeSubsystemBufferBindings)`, so the **pull build is byte-identical** and the
  new `BufferState` counter does not resize the pull build's object.
- Under `Transport=monolith` the record still crosses and the applier still stores it; the
  BACKEND keeps the frontend walk behind `MG_Config::Transport != Monolith`, the exact
  `if (…) else` shape `DirectGLES.cpp:948-966` already uses for the indirect buffer. The push
  build under monolith therefore renders through the same code as today.
- The residual-fill skip for `GetBufferBindingPoint` is gated on a live wire, so under monolith
  the pointer rows are still copied and `MOBILEGL_PIPE_VERIFY`'s comparator
  (`PipeFill.cpp:470-510`) still compares them.
- G2/G14: no new backend entry point and no new exported symbol —
  `SyncBufferBindingPointsByRecord` is a file-local function inside `BufferImpl`.

## 6. Red-once

- **R-S5-a (record).** Revert the handle arm to the frontend walk under a live wire:
  `SsboArrayDynamicIndexScenario` / `StorageBufferRegrowScenario` / `AtomicCounterScenario` under
  `integration-split-strict` must abort with
  `Fatal{UnmigratedPipeInput, "GetBufferBindingPoint@DrawVbo"}` — the server stamp withdraws the
  row (`MG_Backend/MGPipe/PipeInputs.cpp:155-175`), message shape
  `PoisonOmissionScenario.cpp:85-88`.
- **R-S5-b (shutter).** `TrackerTest`: `glBindBufferBase(UNIFORM,1,A); Update();
  glBindBufferBase(UNIFORM,1,B); Update();` must set bit 15 on the second `Update`. Against
  `Tracker.h:636` today it does not — that is the red.
- **R-S5-c (the sticky forward).** `rsp` for `GetBufferBindingPointCount` must be non-zero on
  `AtomicCounterScenario` before and **0** after, with the forward aborting.
- **R-S5-d (no double marking).** Delete the backend marks (4.3) and
  `ProducerMarkCount(GpuWriteProducer::ShaderStorageBinding)` (`GpuWritePending.h`) must stay
  non-zero while `SsboArrayLengthScenario`'s readback still sees the shader's writes.
- **R-S5-e (catalogue).** `PipeCatalogueTest.cpp:170-171`, `:242-243` invert; the inversion is
  itself the proof the route landed.
- Quick gate: unit + `integration-split` + `integration-split-strict` on the four SSBO/atomic
  scenarios and the three XFB ones; an A/B with bit 13 cleared must reproduce today's picture.

## 7. Risks and what I could not settle

1. **One suppressor slot, three classes** (`SetHashSuppressor.h:65`). Needs a ruling: three
   slots (my recommendation — the per-family A/B and fire tallies stay meaningful) or a
   `(slot, Class)` key.
2. **The emitted window.** Minecraft reaches UBOs through the PROGRAM's block bindings `:5463`,
   not through the touched high-water mark. A high-water window makes `Count` 84 for a program
   that binds block 0 at GL point 83 — 2 KB per emission (suppressed while nothing moves, but
   paid on the first emission after any bind). A program-derived sticky window
   (`ImageEmit.h:88-97`'s shape) is cheaper and is why bit 15 mixes program identity in.
   **Unsettled:** whether a narrow window covers `SyncComputeBuffers`' frontend-indexed Uniform
   pass (`:988`), which exists precisely because compute does NOT go through the per-program
   remap (`:967-978`).
3. **`GetRange()` resolution moves to the client** (`Types.h:220-229`). A base binding resolved
   at emit freezes the size; GL resolves at use. Hence `Size = kMGPipeWholeBuffer` and a
   server-side re-resolve. That token needs a table-0 row in CONTRACT-P5E; without it a
   `glBufferData` between emission and apply silently binds the old extent under run-ahead.
4. **The dirty-surface gate will go red** the moment `Touch` joins `MUTATOR_PREFIXES`
   (`gen_pipe_dirty_surface.py:87-90`): every `Touch*` mutator in `MG_Impl/GLImpl` and
   `MG_State/GLState` then needs a row. I did not enumerate them.
5. **Reset discipline.** `MGPipeApplierReset` clears the three windows but ADVANCES
   `ShaderBuffersSerial`; the emitter's latch must reset with it, in `PipeFill.cpp:3129-3143`'s
   list, or the first emission after a make-current is suppressed as unchanged.
6. **Capacity on the wire.** 84 (the frontend's, `BufferState.h:29`) or the ES-clamped
   `MaxUniformBufferBindings` (`DirectGLES.cpp:500-504` clamps at the backend)? Keeping 84 on the
   wire and clamping at the backend preserves today's behaviour exactly; a client-side clamp
   would read a device capability from the wrong side.
7. **`MarkWritableImageBufferTexturesGpuWritten`** `:2763-2781` straddles S3 and S5: its unit
   walk is an image row, its `textureBuffer->GetBufferBindingSlot().GetBoundObject()` `:2775` is
   a buffer row. The client twin already covers it (`GpuWritePending.cpp:83-115`), so the server
   site is simply deleted under a live wire — but that deletion must be ONE edit with S3's.
8. **Conflict to flag for S8:** S6 will want "everything but reply rows is fire-and-forget".
   4.6 says XFB spans and (until 4.5 lands) counter-carrying dispatches are exceptions, decided
   from `MGPContextValues`' `IsTransformFeedbackActive` on the CLIENT. If S6's design instead
   asks the caps snapshot or the server, the two disagree.
