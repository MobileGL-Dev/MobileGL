# Package sb — `set_shader_buffers`: the binding-point tables cross the wire (P5e wave 2)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-89,
ID-93 are yours), `BRIEF-P5E.md` (§0, §1, §2 "sb", §5, §6 rulings 3, 10, 11), `CONTRACT-P5E.md`
§5.6 (and §2.1 for the barriered predicate's XFB input), your scout report `scout-S5.md` (all of it),
and the binding-point / XFB rows of `kimi-audit.md` (rows 111-124) — report per row: retired / left
barriered / not found. Notes: `//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `sb`; your tree is `~/w7/p5e-sb` (branch `p5e/sb`, based on the landed id head, built).
c0e has already landed the row's `WaitClass`, subsystem bit 13, the three suppressor slots, the
applier field declarations (`BoundShaderBuffers[3]`, `Start/Count/WritableMask[3]`,
`ShaderBuffersSerial`) and `MGPipeApplySetShaderBuffers`'s declaration with an asserting body — you
write the bodies.

## What "done" means for this family

`set_shader_buffers` (`MGPShaderBuffers`, opcode 38, `kCtxState`, catalogued since P5 and never
emitted) is EMITTED for the first time: one record per class (Uniform=0, ShaderStorage=1,
AtomicCounter=2), the window is the touched high-water mark per target, capacity 84 on the wire,
the backend clamps to the driver's limit. On the server, `SyncBufferBindingPoints`,
`SyncAtomicCounterBuffers`, `MarkShaderStorageBuffersGpuWritten` and the point half of the UBO loop
in `BindCurrentProgramWithResources` read the applier's arrays instead of
`MGB_CTX->GetBufferBindingPoint(...)`, resolving every buffer with
`EnsureBufferResourceForHandle(nullptr, handle)`. The backend's GPU-write marks (which reach into
client objects) are deleted under a transport — the client's own `MarkGpuWritesForDraw` already
covers them (verify, and say so).

## Scope

Client: `MG_State/GLState/BufferState/BufferState.h` (bind-point generations, push-only),
`MG_Impl/GLImpl/Buffer/GL_Buffer.cpp` (the bumps at every bind-point mutator, including the named
XFB binding), `MG_Impl/Pipe/Tracker.h` (bits 15/16/17 shutters), new
`MG_Impl/Pipe/ShaderBufferEmit.h`, `MG_Impl/Pipe/PipeFill.cpp` (`SubsystemForEmitter` row, the
pairing asserts, the validate order, the reset list, the `kMGPipeMaxBufferBindingPoints`/84 pin).
Wire/apply: `MG_Pipe/PipeApply.{h,cpp}` (`MGPipeApplySetShaderBuffers`), `MG_Pipe/PipeRoute.{h,cpp}`
+ `MG_Remote/Client/WireTables.cpp` (route rows), `MG_Remote/Server/PipeApplier.cpp`
(`OnSetShaderBuffers` only). Backend: `DirectGLES.cpp` `SyncBufferBindingPoints` →
`SyncBufferBindingPointsByRecord`, `MarkShaderStorageBuffersGpuWritten`, `SyncAtomicCounterBuffers`,
their call lines, and the POINT half of the UBO loop (the `GetUniformBlockBinding` line above it is
pg's — do not touch it; coordinate through the report). Gates: `scripts/gen_pipe_dirty_surface.py`
+ `DirtySurface.def` rows. Tests: `TrackerTest.cpp`, new `ShaderBufferEmitTest.cpp`,
`PipeCatalogueTest.cpp` (invert the "not emitted" pins).

## Rulings that bind you

- **ID-104 (found by c0e after the brief was written, and it is a WIRE change you own):**
  `MGPShaderBuffers::WritableMask` is a `Uint32` while the window capacity is 84 points, so the
  mask can only describe the first 32. **Widen the field so the mask covers the declared window**
  (`Uint32 WritableMask[3]`, or `Uint64` + `Uint32` — your choice, say why); do NOT narrow the
  window, which would change what applications can bind. The POD grows; that is fine (opcode 80
  already moved the ABI fingerprint this phase) — pin the new size in `PipeCatalogueTest`. This is
  a named exemption to the file-ownership rule: you edit `MG_Pipe/MGPipeTypes.h` (c0e's file) in
  the same commit as the emitter, and nobody else touches that struct.
- **ID-106:** `MG_Backend/Init.cpp`'s `ConsumedSubsystemsFor` does NOT yet carry bit 13 — c0e
  deliberately withheld it (no emitter existed, and withholding is the safe direction). **You add
  it in the same commit that sets `ShaderBufferEmit.h`'s wired constant**, or the client's R-8
  liveness gate withholds the whole family and your emission never runs.
- ID-89 / rulings 10, 11: high-water window (`TouchedBufferBindingPointCount` per target), 84 on the
  wire, backend clamps; three suppressor slots (one per class).
- `kMGPipeWholeBuffer` as the wire token for a base binding needs its contract row — c0e landed the
  table-0 row; make the emitter use it so a `glBufferData` between emission and apply cannot bind a
  stale extent.
- XFB stays lockstep (contract §5.7): the XFB capture points are NOT part of your emission unless
  your scout's design says they are — if you emit them, the barriered predicate must keep XFB draws
  barriered regardless.

## Red-once (execute, revert, quote)

1. Revert the handle walk under a live wire → `Fatal{UnmigratedPipeInput, "GetBufferBindingPoint@DrawVbo"}`
   under `strict` on `SsboArrayDynamicIndexScenario` / `StorageBufferRegrowScenario` / `AtomicCounterScenario`.
2. `TrackerTest`: `glBindBufferBase(UNIFORM,1,A); Update(); glBindBufferBase(UNIFORM,1,B); Update()`
   must set bit 15 on the second update — red against today's tracker.
3. `GetBufferBindingPointCount`'s `rsp` non-zero before your change, 0 after, on `AtomicCounterScenario`.
4. Delete the backend marks and show `ProducerMarkCount(ShaderStorageBinding)` still non-zero while
   `SsboArrayLengthScenario`'s readback sees the writes.

## Gate

`build`, `unit`, `isplit`, `gens` (the dirty-surface generator must stay green — you are adding
mutators), `one` on the four SSBO/atomic scenarios and the three XFB ones, `strict` (record the
markers that disappeared). Also run the subsystem A/B with bit 13 cleared and confirm it reproduces
today's picture — that is the arm that proves the emission is what the server consumes.
