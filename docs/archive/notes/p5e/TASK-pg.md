# Package pg — programs: the twin and every reflection answer come from records (P5e wave 2)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-85,
ID-87, ID-88 are yours), `BRIEF-P5E.md` (§0, §1, §2 "pg", §5, §6 rulings 5, 6, 8, 9),
`CONTRACT-P5E.md` §5.5, your scout report `scout-S4.md` (all of it), and the program rows of
`kimi-audit.md` (rows 91-110) — report per row: retired / left barriered / not found. Notes:
`//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `pg`; your tree is `~/w7/p5e-pg` (branch `p5e/pg`, based on the landed id head, built).
c0e landed `SetProgramBindings` (opcode 80) with its codec/route/thunk, `MGPProgramDesc::LinkStatus`,
the `MGPipeShaderCsoRecord` fields (`BlockBindings`, `SamplerUnits`, `StorageOverrides`,
`BindingsSerial`, `LinkStatus`) and `MGPipeApplySetProgramBindings`'s asserting declaration; id landed
`ResolveProgramTwin(MGPipeHandle)`.

## What "done" means for this family

The program twin is keyed by the `ShaderCso` handle; `SyncCurrentProgram`, `BindCurrentTextures`'
memo key, `BindCurrentProgramWithResources` and `PrepareForCompute`'s program line answer every
question — link status, SPIR-V status, UBO size and bytes, block bindings, sampler unit indices,
storage-block overrides, backend state version — from the record and the archived reflection, never
from the frontend `ProgramObject`. The five frontend-keyed scope sites of this family are gone.

## Scope

`MG_Impl/Pipe/ProgramEmit.h` (the artefact archive encoded per link through `ProgramArtifactsCodec`
into SEG_STAGE; `EmitProgramBindings`; `LinkStatus`; the failed-relink rule),
`MG_State/GLState/ProgramState/ProgramArtifactsCodec.{h,cpp}` (completeness — name the member the
codec skips at its libc++ caveat and either carry it or refuse by name), `MG_Pipe/PipeApply.{h,cpp}`
(`MGPipeApplyCreateShaderState` adopts the blobs into record-owned artefacts under a transport;
`MGPipeApplySetProgramBindings`), `MG_Pipe/PipeRoute.*` + `MG_Remote/Client/WireTables.cpp` (op 80
route), `MG_Remote/Server/PipeApplier.cpp` (`OnSetProgramBindings`), `DirectGLES.cpp` (the stash,
`ResolveGlobalConstantsRecord(handle)`, the `MapUBO` refusal, `SyncCurrentProgram(handle)` incl. the
broadcast decline arm → Fatal under transport, `PrepareForDraw`'s program line,
`BindCurrentProgramWithResources` EXCEPT the binding-point half (sb's) and the sampler pass (tx2's),
`GetCurrentBackendProgram`, `PrepareForCompute`'s program line, `ShaderStorageBlockBinding`'s
monolith body kept), `Managers.h` + `Managers.cpp` (`SyncToBackend(handle, record)`,
`CacheResourceLocations`, `ReseedShaderStorageBlockBindings`), `MG_Test/Pipe/ProgramEmitTest.cpp`.

## Rulings that bind you

- ID-87 / ruling 8: ENCODE the archive per link (a `SharedPtr` pin is refused by rule B). **Measure**
  links/frame, bytes/link and GL-thread ms on Minecraft + a shader pack before you declare done; if
  the steady cost exceeds ~0.2 ms/frame, fall back to lazy encode at first bind and say so.
- ID-88 / ruling 9: a failed relink of a bound program re-issues `create_shader_state` with
  `LinkStatus = 0` **iff the frontend reports it unlinked** — verify the frontend's actual behaviour
  in `ProgramObject::Link` rather than assuming; never `object_death`.
- ID-85 / ruling 5: `set_storage_block_binding` is `kWaitApplied` for now (c0e set the column);
  resolving it by `ShaderCso` is your TRAILING item, not this package.
- Ruling 6: `BindCurrentTextures`' memo key is BOTH tx2's `SamplerViewsSerial` and your
  `{DrawProgram slot/gen, ShaderCso.Serial, BindingsSerial}`. tx2 owns the line; agree the shape
  through your reports (state exactly what you expect of it).

## Red-once (execute, revert, quote)

1. Empty the applier's block-binding tail → `UniformInitializerScenario` / `Glsl420DeclarationScenario`
   wrong pixels (or a named refusal).
2. Leave the archive blob refs at `Size 0` with the companion pointers removed → `Fatal{ProtocolCorruption}`.
3. Delete the five scopes without re-keying → the id guard's `Fatal{RoleViolation, "MGPipeSlots"}`
   (id's hook forces "unbarriered").
4. `strict` names `GetProgramForDraw@draw_vbo` before, and does not after.
5. Force the global-constants subsystem bit off under run-ahead → `Fatal{UnmigratedVerb, "set_global_constants"}`.

## Gate

`build`, `unit`, `isplit`, `gens`; `one` on `RelinkStageSetScenario|PostLinkAttachScenario|ProgramPipelineScenario|AsyncCompileScenario|CsoContentAddressingScenario|HandleRecycleScenario`;
`strict` (record which markers of this family disappeared). The archive cost measurement is part of
the gate — report the numbers, not an argument.
