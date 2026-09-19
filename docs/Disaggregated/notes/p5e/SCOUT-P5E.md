# P5e scouting brief: retire the lockstep on the Espryt draw path (client runs ahead of apply)

Repo (READ-ONLY for scouts): `C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg`,
branch `feat/disaggregated`, head `2fde7034` (code head `1f8de61b`). Paths below are under `MobileGL/`
unless they start with `docs/`. Cite `file:line` at this head; the E note's citations are at `85362a85`
and may have moved a few lines in `ClientSession.cpp`, `ServerLoop.*`, `PersistentMapTracker.*`,
`PipeFill.cpp`, `PipeInputs.cpp`, `SlotAllocator.*`, `Doorbell.*` (round 3 touched them) - re-resolve.

## Where we are

P5d round 3 made `inproc` CPU-bound on the client thread at ~9 ms/frame (Redmi, MC 26.3 VD12, ~850
draws/frame), apply thread ~2-3 ms of real work per frame plus spinning. The frame is still
`client work + (per draw) wait for apply + handoff`: every kCtxVerb (draw/clear/blit/dispatch) and
every reply-slot row waits `appliedSeq == seq` (R-1, `MG_Remote/Client/ClientSession.cpp`
EmitAndWaitTails). The reason the wait exists is stated in `MG_Remote/CONTRACT-P5C.md` §0 rule E
and its two named exemptions (§3.1 object-class rows, §5.4 the two scopes): during apply the server
still dereferences client-owned memory through the 15 object-class BARRIER_PULLED rows of
`MG_Pipe/FieldOwnership.def` and through the five frontend-keyed twin registries
(`MG_Backend/DirectGLES/SlotTables.h`, `HandleOf(stateObj)` -> `MGPipeSlots().FindByLifetimeId`,
`GetOrCreate(const StatePtr&)` -> `MGPipeSlots().Acquire`) inside `MGPipeFrontendKeyedRegistryScope`
/ `MGPipeReverseAnnouncementScope`. If the client ran ahead, those reads would race the GL thread's
next verb (bindings move, objects die, the client slot allocator is mutated by glGen/glDelete) and
the shared `gPipeInputs` block (`MG_Backend/MGPipe/PipeInputs.h`, the residual fill in
`MG_Impl/Pipe/PipeFill.cpp` MGPipeValidateForVerb step 4, the server stamp in
`MG_Backend/MGPipe/PipeInputs.cpp`) would be written by both roles.

**The research note** `SCOUT-INPUT-E-lockstep-feasibility.md` (same directory as this brief) already
mapped, per object row, every server-side live read on the DirectGLES inproc draw path (its §(a)),
why each is unsafe under run-ahead (§(b)), why a snapshot is not the answer (§(c)) and which pushed
records already cover the data (§(d)). Its conclusion was "not as a P5d round" - the decision now is
to DO it as its own phase (P5e), because it is the item that lets the two threads overlap.

## The target, stated as invariants (what "done" means)

P5e-1  On DirectGLES under an active transport, the apply of every op that the client publishes
       without waiting reads NOTHING from client-owned memory: no frontend object dereference, no
       `gPipeInputs` BARRIER_PULLED row, no `MGPipeSlots()` probe or mint. Every object is resolved
       from a handle the record (or the applier state built from records) carries, through a
       handle-keyed twin table.
P5e-2  The client waits only at: reply-slot rows (already), the control records that need it
       (`applier_reset`, EGL make-current / surface changes), explicit sync (fence client-wait,
       Finish), and a present credit (the client may be at most N presents ahead; N configurable,
       default to be decided by the scouts - 1 or 2). Everything else - draws, clears, blits,
       dispatches, every set/CSO/object record, persistent-map pushes - is fire-and-forget on the
       in-order ring; SEG_CMD / SEG_STAGE space is the only backpressure.
P5e-3  `gPipeInputs` becomes server-role memory under a live wire: the GL thread does not touch the
       block at all for run-ahead ops (the residual fill and the client-side stamp bookkeeping are
       skipped; the tracker dirty walk and the emitters still run - they produce records). The
       server's stamp + `MGPipeInputUnfreshRead` becomes the detector: any BARRIER_PULLED read
       during a run-ahead apply is `Fatal{UnmigratedPipeInput,...}` (strict), so the strict lane
       (`integration-split-strict`, `MOBILEGL_IPC_STRICT_ERRORS=1`) turns from "expected red" into a
       hard green gate for the migrated verbs.
P5e-4  DirectVulkan (Magma, P7 not done) keeps the lockstep: the run-ahead switch is a backend
       capability the server publishes (caps snapshot / `CapsMirror`), not a client guess.
P5e-5  Monolith and the pull build are byte-identical (G1) or every delta is named; the push
       build under `Transport=monolith` unchanged in behaviour; G2/G14 name-set rules kept.
P5e-6  Evidence: per family a red-once (revert the handle arm -> a named Fatal or a named test red);
       the exit gate is unit + `integration-split` + `integration-split-strict` green for the
       migrated verbs, the four A/B traces rendering under run-ahead on the Redmi with SSIM
       unchanged, and the device numbers (fps + per-thread CPU ms/frame; the client thread
       should stop spending its 30% in WaitForApplied).

## Scout assignments (one agent each; read the code, not just the note; write your report file)

Every report: (1) the exact live reads of your family at head `2fde7034` (file:line, kind: live /
id / mint / rec, and whether it runs on a memo hit or only on a miss), (2) what the records and
applier state ALREADY carry for it (`MG_Pipe/PipeApply.h` MGPipeApplierState, `MG_Pipe/MGPipeTypes.h`
record PODs, `MG_Pipe/PipeCalls.def` rows, the emitters in `MG_Impl/Pipe/*Emit.h` and
`MG_Remote/Client/EmitTables.cpp`), (3) the gap: fields/records that do not exist yet and must be
added (name the record, the fields, the encoding, and the client-side dirty/emit rule), (4) the
retirement design: the server code shape after the change (which functions change, which twin
table key, how "is it clean / does it need re-sync" is answered from server-owned serials),
(5) monolith/G1 impact and how the monolith arm stays byte-identical (`#if`, transport check, or
a separate function), (6) the red-once test(s), (7) risks and the things you could not settle.
<= 250 lines, precise, in the voice of the existing contracts.

### S1 - VAO / vertex input / index (E note a.1)
Sites: `DirectGLES/DirectGLES.cpp` ResolveVaoTwin, PrepareForDraw's config-version read,
SyncVaoAttributeBuffersByHandle (memo hit + miss), the IBO arm, SyncCurrentVertexAttributeValues,
DrawArrays/MultiDrawArrays client-side attribute paths, `Managers.cpp` EnsureBufferResource(obj) ->
HandleOfBuffer. State: BoundVertexElements + VertexElementsCsos (content serial), VertexBuffers[]
{Res,...} + VertexBuffersSerial, IndexBuffer.Res, SetVertexAttribDefaults record. Design question:
the server's VAO twin key - the vertex-elements CSO handle (one GL VAO per CSO, buffers rebound per
draw from VertexBuffers[]) or something else; what happens to `ResolvedDrawBuffers` memo keys;
`EnsureBufferResourceForHandle(nullptr, h)` everywhere; client-side arrays are already refused
under split (confirm).

### S2 - textures and samplers (a.3)
Sites: CaptureDrawTextureSyncKeys / CurrentUnitBindingsEpoch, SyncNeccessaryTextures (memo hit:
PairingsIntact + IsDrawSyncClean through borrowed slot pointers; memo miss: unit walk +
SyncTextureObjectToBackend(frontend)), ResolveAndBindUnitTextures, the sampler pass,
BindCurrentUnitSamplers, `TextureImpl` twin registry (`StateBackendObjectRegistry<ITextureObject,...>`
keyed by frontend), `SyncMipmapsToBackend`, `ResolvePushedTextureParams/BuiltinSampler` (P4a/P5c tx
already read records + the server staged store keyed by handle). State: BoundSamplerViews[unit]
{View, Texture, Unit} + SamplerViewsSerial, BoundSamplerStates[] + SamplerStatesSerial,
set_texture_params, tx `StagedTextureStore`. Design: twin keyed by texture handle; per-twin
"clean" from server-owned serials (params serial, storage/defined-ness serial from tx, sampler-state
handle) instead of `IsDrawSyncClean(frontend)`; the memo re-keyed on (unit -> texture handle +
serials); texture death (`object_death` for textures exists since P5c ct?) and the default-texture
/ builtin-sampler cases.

### S3 - framebuffers, attachments, images (a.4, a.5)
Sites: SyncCurrentFBO/SyncCurrentFBOByRecord, BindCurrentFBO, the draw-FBO texture sync list in
SyncNeccessaryTextures and SyncReadFramebufferTextureAttachments (identity half = pointer compare,
`GetAllAttachmentObjects()` live), Clear/BlitFramebuffer bound arms, `g_fboTextureSyncList`,
SyncImageTextureBinding (Access byte has no encoding), MarkWritableImageBufferTexturesGpuWritten
(buffer-texture backing buffer via frontend). State: FramebufferRecords[handle] {Color[8]/Depth/
Stencil surfaces, DrawBuffers, IsDefault}, BoundFramebuffer[2], FramebufferSerial, BoundShaderImages[]
{Res, Unit, InternalFormat, Layer, Level, Layered, Access}, the TexBuffer backing record (P4a).
Design: framebuffer twin keyed by handle (already? `FindByHandle(record.Fbo)` exists), attachment
lists from the record's surface handles, the image `Access` encoding, the buffer-texture backing
handle on the server.

### S4 - programs / shader state (a.2)
Sites: SyncCurrentProgram, BindCurrentTextures (program-keyed memo), BindCurrentProgramWithResources
(link/spirv status, UBO size, MapUBO without ShaderCso, GetUniformBlockBinding, backend state
version, sampler-pass uniform unit indices), SyncAtomicCounterBuffers, PrepareForCompute's
GetProgramForDispatch, `g_backendProgramObjects` registry, GetProgramObject/ValidateProgramName
sticky rows. State: DrawProgram / DispatchProgram / BoundShaderCso handles, ShaderCso records
(create_shader_state: SPIR-V + full reflection = `ProgramState/ProgramArtifacts.h`), GlobalConstants
bytes + version. Design: program twin keyed by the shader-state handle; every reflection question
answered from the archived artifacts; the "no ShaderCso record" arm (MapUBO) - who takes it and
whether it can be refused under run-ahead; link status as record state.

### S5 - buffer binding points and the remaining rows (a.6 + the tail)
Sites: SyncBufferBindingPoints (UBO/SSBO/atomic/XFB), the UBO loop in BindCurrentProgramWithResources,
MarkShaderStorageBuffersGpuWritten, SyncAtomicCounterBuffers, GetBufferBindingPointCount sticky,
XFB rows (GetTransformFeedbackProgram, HasOpenTransformFeedbackSpan, StartPendingTransformFeedback),
GetBufferBindingSlot (15 pointers, filled every draw, no draw-path reader), RecordError.
State: `set_shader_buffers` (`MGPShaderBuffers`, PipeCalls.def:174, kCtxState, kVarTail|kHostSpan,
CONTRACT-P5C.md:169-176 says catalogued but NOT emitted until P4b). Design: the emission (client
tracker dirty rule for the four binding-point tables - what dirty bit exists, `gen_pipe_dirty_surface`
implications), the applier-side table, the Espryt consumers (UBO points at draw, SSBO/atomic points,
XFB capture points), the named-UBO host span (D-B8 is Magma-only - can the Espryt emission omit
it), `GetBufferBindingPointCount` from the record; what stays lockstep (an open XFB span? dispatch
with atomic counters?) and how the client knows to wait there.

### S6 - transport mechanics of run-ahead (fable)
`MG_Remote/Client/ClientSession.cpp` EmitAndWaitTails / the barrier flags / DrainEventRing /
AwaitBufferWriteback / Stop's drain, `MG_Remote/Transport/SessionRings.h` (appliedSeq, presentAck,
WaitForCmdSpace, stage retirement), `MG_Remote/Server/ServerLoop.cpp` + `PipeApplier.cpp`
(ApplyOne, StampVerbBoundary, LeaveApplier, the deferred-destroy queue from P5d round 1 in
`MG_Pipe/PipeMutation.h`), `MG_Remote/Client/PersistentMapTracker.cpp` (pushes before verbs),
`MG_Remote/Client/GpuWritePending.cpp`, `CapsMirror.*`, `ConfigLoader.cpp` IPC knobs,
`docs/Disaggregated/ARCHITECTURE.md` §11 (reverse channel, present credit `MOBILEGL_IPC_PRESENT_CREDIT`
mentioned as P6+/P10), §17. Design: (1) the exact wait rule per op under run-ahead and the switch
(`MOBILEGL_IPC_RUN_AHEAD`? default, the caps bit Espryt publishes, Magma stays lockstep, the
negative control), (2) present pacing: `WaitForPresentAck` with credit N; what N does to the two rings
(SEG_CMD 8 MiB, SEG_STAGE 32/256 MiB) at ~850 draws + 0.36 MB pmap per frame, (3) where events are
drained now (present, reply rows) and which client operations must force a drain/wait
(MapBuffer readback, GetBufferSubData, glFinish, fence client-wait, glGetError semantics under
run-ahead - name the relaxation), (4) `gPipeInputs` ownership: the client-side skip of the residual
fill + stamp bookkeeping under a live wire (which of `MGPipeValidateForVerb`'s steps stay: the
tracker walk and the emitters must), the server-only stamp, the strict detector, the verify build
(`MOBILEGL_PIPE_VERIFY` forces lockstep - confirm), (5) object lifetime under run-ahead: slot reuse
{slot,gen} vs records still in flight, `object_death` ordering, the deferred-destroy queue, the
client memos that hold frontend->handle maps, `Forget()` of persistent maps while pushes are in
flight in SEG_STAGE, (6) the two scopes and `MGPipeRefuseAllocatorFromApplyThread`: after P5e the
apply thread must never enter them - the guard becomes unconditional Fatal; list every remaining
apply-thread allocator touch that the other scouts' designs do not already retire (readback paths,
CopyTex, mip, blit named arms - are they run-ahead ops or waited ops?), (7) the measurement plan on
the Redmi (bench scripts exist: `~/w7/notes/tools/p5d_bench_{fcl4,all4}.sh`).

### S7 - identity: the five twin registries and the allocator (fable)
`DirectGLES/SlotTables.h` (BackendSlotTable, StateBackendObjectRegistry, HandleOf, FindByHandle,
GetOrCreate(handle) from P5c hd, the holder list / DestroyByLifetimeId, dead-generation rules),
`Managers.cpp` HandleOfBuffer / g_backendBufferResources / OnFrontendStateObjectDestroyed /
EmitObjectDeathRecord (P5c ct), `MG_Impl/Pipe/SlotAllocator.*`, `MG_Impl/Pipe/ResourceTracker.h`,
the client-side lifetime-id -> handle maps, `MG_Pipe/PipeMutation.h` deferred destroys. Deliver:
for each of the five kinds (buffer, VAO, texture, framebuffer, program/shader state) + sampler +
renderbuffer if they have twins: today's key, the record that carries the handle at create/bind,
whether a death record exists, and the exact change to make the registry handle-keyed with no
`GetOrCreate(const StatePtr&)` / `HandleOf(stateObj)` reachable on the apply thread; the twin
creation moment (first record naming the handle) and the twin death moment (object_death / applier
reset); the "recycled slot, new gen" ABA answer; how the 20 scope sites are removed and the two
scopes deleted (or reduced to monolith-only). This is the spine the other scouts hang on - be exact.

### S8 - synthesis (fable; runs after S1-S7)
Read all seven reports and write `CONTRACT-P5E-draft.md` (the rules: run-ahead op classes and
wait points; server-owned `gPipeInputs`; handle-keyed twins; the new records/fields; Magma stays
lockstep; the strict gate) and `BRIEF-P5E.md`: packages with file ownership boundaries that can be
implemented in parallel worktrees, their order/dependencies (which package lands first so the
others build on it - likely S7's registry re-key and S5's record), per-package red-once and quick
gate, the integration gate, the device exit, and the open questions that need the integrator's
ruling. Name conflicts between scout designs explicitly and resolve them (or list them as rulings
needed). <= 500 lines each; write the files before the final message.

## Rules
Read-only: no edits to the repo, no builds, no adb. Reports go to
`C:/Users/geekerwan/AppData/Local/Temp/claude/C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL-disagg/5007453d-cb19-4ebf-96a3-60ba4fb55b47/scratchpad/p5e/scout-<id>.md`
(write the file BEFORE the final message; the final message is the structured result only).
No process narration in the reports. Cite at head `2fde7034`.
