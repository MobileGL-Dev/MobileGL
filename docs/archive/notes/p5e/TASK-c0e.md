# Package c0e — the contract and the wire (P5e; lands first, inert)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md`,
`BRIEF-P5E.md` (§0, §1, §2 "c0e", §3, §6), `CONTRACT-P5E-draft.md` (all of it — you are landing it),
then the scout reports your rows come from (`scout-S6.md` for the wait classes / knobs / caps bit,
`scout-S5.md` for `set_shader_buffers`' encoding and bit 13, `scout-S4.md` for `set_program_bindings`
/ `LinkStatus`, `scout-S3.md` for the image `Access` decode, `scout-S1.md` for `kDrawClientArrays`,
`scout-S7.md` for the seam declarations). All are in `//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.
Your slug is `c0e`; your tree is `~/w7/p5e-c0e` (built, on branch `p5e/c0e` at `2fde7034`).

## Deliverables (all inert: no behaviour changes; unit + integration-split + gens stay green; G1 0/0/0/0)

1. `MobileGL/MG_Remote/CONTRACT-P5E.md` — the draft, edited into its landed form: apply the 19 rulings
   as decided in `INTEGRATOR-DECISIONS-P5E.md` (they mostly match the draft's recommendations),
   make every `file:line` resolve at the HEAD of your branch (run `gens`; the doc-citation lint covers
   `MG_Remote/CONTRACT-P5*.md`), remove "draft" wording, keep the voice of CONTRACT-P5C.
2. `MobileGL/MG_Pipe/PipeCalls.def`: the `WaitClass` column on every row exactly as CONTRACT-P5E §2.2
   (kWaitNone / kWaitApplied / kWaitReply / kWaitPresent / kWaitByPredicate…), `SetProgramBindings`
   (opcode 80) appended in S4's shape. Extend the generator(s) that read the def (`scripts/gen_pipe.py`
   and whatever emits the wire/route/thunk tables — find them) so that `MGPipeWaitClassFor(op)` is
   generated, the new op has its codec layout / thunk / table slot / route row (null route, refused
   by name until pg lands), and every `--check` / `--self-test` stays green with the regenerated
   `MobileGL/MG_Pipe/generated/*.inc` committed. The ABI fingerprint (`SessionRings.h`) must change
   with the new opcode; pin it.
3. `MobileGL/MG_Pipe/MGPipeTypes.h`: `kCapRunAheadApply` (bit 10, MGPCapBit), `MGPProgramBindings`
   + its tail PODs, `MGPProgramDesc::LinkStatus` in the spare bytes, `kDrawClientArrays` draw flag,
   `kMGPipeMaxBufferBindingPoints` (84; pinned against `BufferState.h`'s constant where both are
   visible), `MGP_ASSERT_POD` sizes; `MGPipeValueTypes.h`: `MGPipeImageAccess` enum with
   encode/decode (the client encode in `ImageEmit.h` already exists — move/alias, do not duplicate).
4. `MobileGL/MG_Pipe/MGPipe.h`: subsystem bit 13 (`kMGPipeSubsystemBufferBindings`), default mask
   `0x3fff`; `MG_Impl/Pipe/SetHashSuppressor.h`: the three `SetShaderBuffers` class slots;
   `MG_Impl/Pipe/Tracker.h`: bits 15-17 mapped to subsystem 13 (`MGPipeSubsystemForDirty`);
   `TrackerTest.cpp`'s bit table; `MG_Pipe/FieldOwnership.def` re-annotations (retiring phase P5e
   on the rows the brief names); `MG_Pipe/Coverage.def` rows as the contract lists.
5. `MobileGL/Config.h` / `ConfigLoader.cpp`: `IpcTable::RunAhead` (`MOBILEGL_IPC_RUN_AHEAD`, default
   1, forced 0 under `PipeVerify`, logged in the IPC line), `IpcTable::PresentCredit`
   (`MOBILEGL_IPC_PRESENT_CREDIT`, default 1, range 1..8).
6. Seam declarations in `MobileGL/MG_Pipe/PipeApply.h` (+ `.cpp` bodies that assert "not landed" by
   name, never silently succeed): `MGPipeApplierCurrentRecordIsBarriered()`,
   `MGPipeBarriered(op, payload, applierState)` (the static column half implemented here; the
   XFB/client-array halves read the fields the contract names), the sb applier fields
   (`BoundShaderBuffers[3]`, `Start/Count/WritableMask[3]`, `ShaderBuffersSerial`) and
   `MGPipeApplySetShaderBuffers` declaration, the pg fields on `MGPipeShaderCsoRecord`
   (`BlockBindings`, `SamplerUnits`, `StorageOverrides`, `BindingsSerial`, `LinkStatus`) and
   `MGPipeApplySetProgramBindings` declaration, the by-handle sync signatures the family packages
   implement (`SyncTextureToBackendByHandle`, `SyncMipmapsToBackendByHandle`, `ResolveVaoTwin(handle)`,
   `ResolveProgramTwin(handle)`, `ResolveTextureTwin(handle)`, the FBO/RBO `SyncToBackendByHandle`
   overloads) — declared where the brief says (Managers.h ranges) with asserting bodies, so every
   package compiles against the same names from day one.
7. `MobileGL/MG_Backend/Init.cpp`: `kMGPipeP5eRunAheadReady = false` and the DirectGLES arm that
   will publish bit 10 only when it is true (Magma never); `MagmaPipeIdentityTest` pins no bit 10.
8. `MG_Test/Pipe/PipeCatalogueTest.cpp`: sizes, opcode 80, the null route pin, the wait-class table
   (every row's class asserted against the contract table by name).
9. `docs/Disaggregated/ROADMAP.md`: the P5e row (status "进行中", the package list, the exit gate)
   inserted after P5d; `CURRENT_STAGE_PROGRESS.md` §1 row + §6 next-step line. Keep the citation lint green.

## Red-once (execute, revert, record)
- Flip one row's `WaitClass` in the def without regenerating → `gen_pipe.py --check` red.
- Change the `MGPProgramBindings` POD size → `PipeCatalogueTest` red.
- Publish bit 10 on the Magma arm → `MagmaPipeIdentityTest` red.

## Gate
`build`, `unit`, `isplit`, `gens` green; `strict` unchanged vs base (record its marker list in the
report as the baseline the later packages must shrink). Commit in sensible slices (contract; def +
generators + generated; types; knobs; seams + tests; docs).
