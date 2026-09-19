# S4 - programs / shader state (E note a.2) at head `2fde7034`

Paths under `MobileGL/`. `DirectGLES.cpp` / `Managers.*` = `MG_Backend/DirectGLES/`.
Kinds: **live** = dereference of the frontend `ProgramObject`; **id** = `GetLifetimeId()` +
client-allocator probe; **mint** = `MGPipeSlots().Acquire`; **rec** = record-only arm.
"hit" = runs on every draw, memo hit included.

## 1. The exact live reads of this family

### 1.1 The row itself
| site | read | kind | hit |
|---|---|---|---|
| `DirectGLES.cpp:4781` `PrepareForDraw` | `MGB_CTX->GetProgramForDraw()`, the BARRIER_PULLED SharedPtr row (`MG_Pipe/FieldOwnership.def:92`) | live | yes |
| `:6126` `PrepareForCompute` | `GetProgramForDispatch()` (`FieldOwnership.def:100`) | live | dispatch |
| `:5308`, `:5679` `GetCurrentBackendProgram`, `:5734` `CurrentProgramMayNeedPerSubDrawBuiltins` | three more `GetProgramForDraw()` pulls in the same entry point; `:5680` link/SPIR-V status, `:5736` `GetLinkVersion()` | live | per draw-parameter update / per multi-draw |

### 1.2 `SyncCurrentProgram` (`:4321`, scope `:4329`)
| site | read | kind | hit |
|---|---|---|---|
| `:4339` | `GetLinkStatus()`, `GetSpirvStatus()` | live | yes |
| `:4350-4380` / `:4383-4404` | broadcast count: record arm off `BoundFramebufferRecord(Draw)` (`:238`) / decline arm reads the draw-FBO slot (S3's) | rec / live | yes / latch off |
| `:4415-4416` | `g_backendProgramObjects.Find(currentProgram.get())` -> `HandleOf` (`SlotTables.h:436-449`), miss -> `GetOrCreate(const StatePtr&)` (`:209-252`); legacy arm repeats it at `:4427-4429` | id, mint | yes |
| `:4437-4439` | `GetBackendProgramId()`, `GetLinkVersion()`, `GetImageUnitVersion()` | live | yes |
| `:4463-4464` | `ComputeShaderStorageBlockBindingSignature(*currentProgram)` -> `Managers.cpp:10819-10842`, walks `GetShaderStorageBlockBindingOverrides()` (an `UnorderedMap<String,Int>` inside `LinkArtifacts`) | live | yes |
| `:4472` | `ImageUnitFormatsStillMatch()` -> `Managers.cpp:10846+` -> `GetImageTextureBinding` (S3's row) | live | yes |
| `:4501-4506` | `GetPatchVertices()/GetPatchDefault{Outer,Inner}Level()` - VALUE rows in the block, not object memory | - | yes |
| `:4508` | `twin->SyncToBackend(currentProgram)` -> `Managers.cpp:11922-12971`: ~40 accessors, incl. `GetGeneratedSpirv` `:12028`, `GetLinkedShaderStages` `:12027`, `GetLinkedShaderSnapshot` `:12049` (GL-thread-owned vector; `MGLOG_D` only), `GetTransformFeedbackVaryings` `:12067,:12711`, `ReseedShaderStorageBlockBindings` `:12925` (`Managers.cpp:10809-10817`), `CacheResourceLocations` `:12918` (`:12995-13078`: `GetActiveUniformBlocksCount`, `GetUniformBlockName`, `GetMaxUniformLocation`, `GetUniformName`, `GetUniformType`, `GetUniformLocation`) | live | rebuild only - cold, but on the apply thread |
| `:4509-4510` | the per-draw stash is keyed by the RAW FRONTEND POINTER (`:4097-4098`) | live | yes |

### 1.3 `BindCurrentTextures` (`:5245`)
`:5257` memo entry selected by `currentProgram.get()`; `:5279` `GetLifetimeId()`, `:5281`
`GetBackendStateVersion()`, `:5282` `GetLinkStatus()` - **live on every draw, memo hit
included**; re-read at `:5295-5297` on a store. Miss -> `ResolveAndBindUnitTextures` `:4834`,
whose `sampledTargetForUnit` `:4845-4857` reads `GetLinkStatus` / `GetMaxUniformLocation` /
`GetUniformSamplerOrImageUnitIndex` / `GetUniformType` (live, alias conflict only).

### 1.4 `BindCurrentProgramWithResources` (`:5317`, scope `:5328`)
| site | read | kind | hit |
|---|---|---|---|
| `:5320` | `GetLinkStatus()`, `GetSpirvStatus()` | live | yes |
| `:5332-5339` | stash compare against `currentProgram.get()`; fallback `Find(currentProgram.get())` | id | fallback |
| `:5349` | `GetUBOSize() > 0 && HasGlobalUboBlock()` | live | yes |
| `:5365` -> `ResolveGlobalConstantsRecord` `:4230-4318` | scope `:4236`, `HandleOf(program)` `:4242`, `FindShaderCsoRecord` `:4157`, `GetUBOSize()` `:4280,:4297,:4305,:4313` | id + live | yes |
| `:5371-5373` | `record->GlobalConstantsVersion` ELSE `GetUBOContentVersion()`; `GetUBOSize()` always | rec / live | yes |
| `:5379-5382` | `MGB_UBO_BYTES` = record bytes ELSE `currentProgram->MapUBO()` (`MG_State/GLState/ProgramState/ProgramObject.h:734`, the live `globalUboScratch` every `glUniform*` writes) | rec / live | no record only |
| `:5425,:5430` | `GetUBOSize()` on the `glBufferSubData` fallback | live | no-ring devices |
| `:5448,:5463` | twin's `GetUniformBlockBackendIndices()`, then `currentProgram->GetUniformBlockBinding(i)` (`ProgramObject.h:1029` = `Artifacts().uniformBlockBinding[i]`, moved post-link) - the point read at `:5464` is S5's | live | per named block per draw |
| `:5504-5506` | `SyncAtomicCounterBuffers(twin.GetAtomicCounterBindings(), ...)` -> `:617-660` (S5's rows) | twin | counter programs |
| `:5522` | `GetBackendStateVersion()` - the sampler-pass memo key | live | yes |
| `:5544-5546` | on a miss, `GetUniformSamplerOrImageUnitIndex(frontendLocation)` per sampler binding (`ProgramObject.h:857`); `:5561-5596` are S2's unit/sampler reads | live | miss |

### 1.5 Off the steady path, still apply-thread
`GetBackendProgramId` `:6151-6178`: `ValidateProgramName` `:6152` + `GetProgramObject` `:6157`
(sticky BARRIER_PULLED, `FieldOwnership.def:149,155`), scope `:6166`, `Find`/`GetOrCreate`
`:6168-6170`, `SyncToBackend` `:6174`. `ShaderStorageBlockBinding` `:10020-10042`: the same
two forwards `:10022-10023`, scope `:10029`, `Find` `:10031`, `GetLinkVersion()` `:10038`.

## 2. What records and applier state ALREADY carry

* `MGPipeApplierState::DrawProgram / DispatchProgram / BoundShaderCso` + `ProgramBindingSerial`
  (`MG_Pipe/PipeApply.h:691-695`), written by `MGPipeApplyBindShaderState`
  (`PipeApply.cpp:2919`), `...SetDrawProgram` (`:2980`), `...SetDispatchProgram` (`:2995`),
  cleared by `...DeleteShaderState` (`:2941`) and `MGPipeApplierReset`. Catalogue:
  `MG_Pipe/PipeCalls.def:162-164,180-181`.
* `MGPipeShaderCsoRecord` (`PipeApply.h:370-380`): `Gen/Live`, `MGPProgramDesc Desc`
  (`MG_Pipe/MGPipeTypes.h:585-597` - `Cso`, `StageMask`, `GlobalUboSize`,
  `ReservedNumSamplesOffset`, `SpirvStatus`, `NativeFloat64`, `PointSizeDemoted`,
  `EnableSpirvValidation`, `MGPBlobRef Spirv[6]`, `MGPBlobRef Reflection`), `GlobalConstants`
  + `GlobalConstantsVersion` + `GlobalConstantsSerial`, `Serial`. Two tables, ordinary and
  composite band (`PipeApply.h:503,511`). `Serial` moves on every applied
  `create_shader_state` including a re-issue on the same handle - that is how a RELINK
  travels - and a re-issue clears the block and bumps `GlobalConstantsSerial`
  (`PipeApply.cpp:2895-2914`).
* Emitters: `MG_Impl/Pipe/ProgramEmit.h` `EmitShaderState` `:105-159`, `AcquireShaderCso`
  `:212-292`, `EmitGlobalConstants` `:169-209`; driven from `MG_Impl/Pipe/PipeFill.cpp:3162`
  (`NewShader`/`NewShaderBindings`) and `:3188` (`NewGlobalConstants`). The shutters
  (`MG_Impl/Pipe/Tracker.h:364-371` plain, `:419-448` pipeline) ALREADY mix
  `GetBackendStateVersion`, `GetBlockBindingVersion`, `GetImageUnitVersion`,
  `GetUniformWriteSetVersion` and the identity into bit `NewShaderBindings`.
* **A server-owned clean key that exists and is never read:** `m_syncedShaderCsoSerial`
  (`Managers.h:2415,2517`), stamped from the record at `Managers.cpp:12944-12966`. Nothing in
  the tree calls `GetSyncedShaderCsoSerial()` at this head - P4a's D-B3 landed the producer
  without the consumer. P5e is its first caller.
* Death: `object_death` covers ShaderCso by handle (`Managers.cpp:328-345`);
  `delete_shader_state` is emitted at `PipeFill.cpp:1807-1827`.
* Handle-keyed twin machinery already on this registry (`Managers.cpp:10757`
  `TwinRegistry<ProgramObject, BackendProgramObjectImpl, MGPipeKind::ShaderCso>`):
  `FindByHandle` (`Managers.h:429`), `GetOrCreateByHandle` (`:453`), `ReleaseByHandle`
  (`:505`), `NoteStateForHandle`/`StateForHandle` (`:473-484`).

## 3. The gap

**G-A. The archive is not server-owned.** `MGPipeApplyCreateShaderState` stores the
DESCRIPTOR and deliberately not the artefacts (`PipeApply.cpp:2915-2918`); all seven
`MGPBlobRef`s are `Size 0` and `LinkArtifacts`/`SpirvArtifacts` ride beside the record as
companion pointers (`ProgramEmit.h:230-260`). So every read in 1.2's `SyncToBackend` row is a
read of `ProgramObject::m_artifacts`/`m_spirv`, which `Link()` REPLACES in place
(`ProgramObject.h:1104`, `ProgramObject.cpp:348-356,518-522`). *Fix:* fill the refs - the
client encodes once per link with the existing codec
(`MG_State/GLState/ProgramState/ProgramArtifactsCodec.h`; the verify build already pins the
round trip at `PipeApply.cpp:1171-1202`) into `SEG_STAGE`, the applier decodes into
record-owned `LinkArtifacts`/`SpirvArtifacts`. **No new fields** - `Desc.Reflection` and
`Desc.Spirv[6]` simply stop declaring 0.

**G-B. Three POST-LINK mutable reflection fields have no wire form.** They live inside
`LinkArtifacts`, so an archive snapshot alone is wrong:
| field | setter | counter | server reader |
|---|---|---|---|
| `uniformBlockBinding[i]` | `SetUniformBlockBinding` `ProgramObject.h:1017-1027` | `m_backendStateVersion`, `m_blockBindingVersion` | `:5463` |
| `uniformSamplerOrImageUnitIndex[loc]` | `SetUniformSamplerOrImageUnitIndex` `:830-850` | `m_backendStateVersion` (+`m_imageUnitVersion` for images) | `:4851`, `:5546` |
| `shaderStorageBlockBinding[name]` | `SetShaderStorageBlockBinding` `:1035-1048` | `m_blockBindingVersion` ONLY | `:4464`, `Managers.cpp:10811,10821` |

*Fix:* one new record **`set_program_bindings`**, `kCtxState`, `kVarTail|kHostSpan`, head
`MGPProgramBindings { MGPipeHandle Cso; Uint64 Signature; Uint32 BlockBindingCount,
SamplerUnitCount, StorageOverrideCount, Pad; }` and three tails: `Int32 BlockBindings[]`
(dense, GL uniform-block index order), `{Uint32 Location, Int32 Unit}[]` (sparse), and
`{MGHostSpan Name, Int32 Binding}[]` for the storage overrides - a name span because the
override map is name-keyed BY DESIGN (`ProgramObject.h:1030-1034`: three index spaces, one
coordinate). `Signature` is computed CLIENT-side with the same commutative hash as
`Managers.cpp:10832-10841`, so the rebuild clause is a `Uint64` compare and the name tail is
touched only on a rebuild. Record gains `BlockBindings`, `SamplerUnits`, `StorageOverrides`,
`Signature`, `BindingsSerial`; a re-issued create clears all of them for
`GlobalConstants`' reason (`PipeApply.cpp:2903-2909`). Dirty rule: bit `NewShaderBindings`
already exists and its shutter already reads all three counters; the emitter is a fourth
method on `MGPipeProgramEmitterInstance()`, latched on
`(Cso, backendStateVersion, blockBindingVersion)`, emitted BEFORE `EmitShaderState` (a
rebuild inside the verb must already see the bindings).

**G-C. `MapUBO()` has no run-ahead answer.** `:5380/:5382` runs when
`ResolveGlobalConstantsRecord` declines (bit 12 off, sentinel, short block) and is a torn
read of `globalUboScratch`. Under a live wire with run-ahead that arm is
`Fatal{UnmigratedVerb,"set_global_constants"}`, not a fall-back.

**G-D. Frontend-pointer keys.** The stash (`:4097-4098`) and both memos (`:5257`, `:5279`)
key on the object address; re-key on `{slot,gen}` + server serials. No record needed.

## 4. The retirement design

1. **Twin key = the ShaderCso handle.** `SyncCurrentProgram` takes an `MGPipeHandle` on the
   run-ahead arm; `PrepareForDraw` reads `MGPipeApplier().DrawProgram`, `PrepareForCompute`
   `.DispatchProgram` (`PipeApply.h:691-693`). Twin = `GetOrCreateByHandle(h)`
   (`Managers.h:453`). The `Find`/`GetOrCreate(StatePtr)` pairs at `:4415-4416`, `:5337`,
   `:6168`, `:10031` and `ResolveGlobalConstantsRecord`'s `HandleOf` `:4242` go, and with
   them scopes `:4236, :4329, :5328, :6166, :10029` - five of the twenty S7 must retire.
2. **The record replaces the object in `SyncToBackend`**, whose signature becomes
   `(MGPipeHandle, const MGPipeShaderCsoRecord&)`: artefact reads answered from the record's
   owned archive (G-A), binding reads from its tails (G-B). `GetLinkedShaderSnapshot`
   `:12049` is a debug log and is deleted on this arm; `GetExternalIndex()` in messages
   becomes the handle.
3. **"Clean" from server-owned serials.** The nine-clause condition `:4437-4507` keeps its
   clause count and moves its inputs: `GetLinkVersion` -> `record.Serial` vs
   `twin->GetSyncedShaderCsoSerial()` (the stamp that already exists);
   `GetImageUnitVersion` -> `record.Serial` + `BindingsSerial` (a `glUniform1i` on an image
   uniform moves `m_imageUnitVersion`, a `NewShaderBindings` shutter input, so the bindings
   record re-fires); `ComputeShaderStorageBlockBindingSignature` -> `record.Signature`;
   `GetLinkStatus/GetSpirvStatus` -> `Desc.SpirvStatus` plus a new explicit `Desc.LinkStatus`
   byte (today "linked" is only implied by the create existing, which is not the same
   statement); `ImageUnitFormatsStillMatch` and the patch clauses are S3's / value rows.
4. **Memos.** `BindCurrentTextures`' key (`:5274-5283`) becomes
   `{handle.Slot, handle.Gen, record.Serial, BindingsSerial}`; the sampler-pass memo's
   `backendStateVersion` (`:5522`) becomes `BindingsSerial`; `sampledTargetForUnit`
   (`:4845-4857`) reads the sampler-unit tail plus the archive's uniform types.
5. **The stash** (`:4097-4098,:4509-4510,:5332-5335,:5678-5695`) holds
   `{MGPipeHandle, BackendProgramObjectImpl*}` and consumers compare against
   `MGPipeApplier().DrawProgram` instead of `GetProgramForDraw().get()`.
6. `GetProgramObject` / `ValidateProgramName` keep their P9 row: both call sites (`:6151`,
   `:10020`) are GL-thread entry points, not run-ahead ops (7.2).
7. After this the family's only remaining readers are `GetProgramForDraw:92` and
   `GetProgramForDispatch:100`, which flip to FATAL under run-ahead (P5e-3) - the strict
   lane's expected red for this family becomes a gate.

## 5. Monolith / G1

* The pull build compiles none of it: every new site sits under `MOBILEGL_PIPE_PUSH`, and the
  registry alias already drops the kind parameter there (`Managers.h:626-640`), so mangled
  names stay pre-P2. The record, the applier fields and the emitter method are push-only.
  **G1 byte-identity holds by construction.**
* Push + `Transport=monolith` keeps today's behaviour: the run-ahead arm is selected by a
  *transport + caps* predicate (`ProgramRunAheadArm()` beside `ProgramSubsystemEnabled()`),
  false under monolith and false for DirectVulkan (P5e-4). Under monolith `SyncCurrentProgram`
  keeps the frontend overload verbatim, and with it `MapUBO`, `GetBackendProgramId` and
  `ShaderStorageBlockBinding`.
* Two overloads, not an `#if` inside one body - the frontend one should be visibly the
  monolith-glue half, as `GetOrCreate(const StatePtr&)` is (`SlotTables.h:236-244`).
* G2/G14: one new `PipeCalls.def` row, no new dirty bit, so `gen_pipe_dirty_surface` needs
  only the new emitter's shutter row.

## 6. Red-once

1. **The bindings record is consumed.** Empty the applier's block-binding tail and run
   `UniformInitializerScenario` / `Glsl420DeclarationScenario`: a named UBO lands on the
   wrong point -> wrong pixels + `RefusedObjectCalls`. Unit: a `ProgramEmitTest` case beside
   `MG_Test/Pipe/ProgramEmitTest.cpp:251` ("a glUniformBlockBinding after the link travels on
   its own record").
2. **The archive is server-owned.** Run the run-ahead arm with the blob refs left at `Size 0`
   and the companion pointers removed: the existing trip wire "the record declares no blobs
   and carries no artefacts" (`PipeApply.cpp:2858-2871`) fires `Fatal{ProtocolCorruption}`.
3. **No frontend probe survives.** Delete the five scopes of 4.1 without re-keying:
   `MGPipeRefuseAllocatorFromApplyThread` (`MG_Impl/Pipe/SlotAllocator.cpp:22-38`) aborts
   `Fatal{RoleViolation,"MGPipeSlots"}` on the first `integration-split` draw.
4. **`GetProgramForDraw` is never pulled.** `integration-split-strict`
   (`MOBILEGL_IPC_STRICT_ERRORS=1`, `ConfigLoader.cpp:382`) with the row still
   BARRIER_PULLED: `StrictBarrierPullFatal` (`MG_Backend/MGPipe/PipeInputs.cpp:60-66`) names
   `GetProgramForDraw@draw_vbo`. Green after the re-key = P5e-1 for this family.
5. **Relink / identity under run-ahead:** `RelinkStageSetScenario`, `PostLinkAttachScenario`,
   `ProgramPipelineScenario`, `AsyncCompileScenario` with the client a present ahead;
   `CsoContentAddressingScenario` + `HandleRecycleScenario` for the {slot,gen} ABA.
6. **`MapUBO` refusal:** force bit 12 off under run-ahead and assert G-C's
   `Fatal{UnmigratedVerb,"set_global_constants"}` rather than a torn upload.

## 7. Risks and what I could not settle

1. **Archive cost per link.** MC + shader packs link hundreds of programs, some mid-frame.
   `EncodeProgramArtifacts` over a full `LinkArtifacts`
   (`MG_State/GLState/ProgramState/ProgramArtifacts.h:160-342`: several `String` maps, a
   `std::set<String>`, seven index vectors) runs only in the verify build today. Needs a
   measurement before the package is sized. The cheap alternative - the client publishes an
   immutable `SharedPtr<const LinkArtifacts>` per link and the record pins it - keeps inproc
   fast but makes inproc a different code path from a real split, which is against P5e's
   point. **Integrator ruling.**
2. **A FAILED relink of a bound program** keeps the previous executable running, and the
   emitter's re-issue rule (`ProgramEmit.h:220-226`, keyed on `GetLinkVersion`) fires anyway
   because `BumpLinkObservableVersions` runs in `Link()`'s prologue
   (`ProgramObject.cpp:348-356`). Whether the applier keeps the PREVIOUS record live or holds
   a dead-but-bound one is unsettled. It must NOT be modelled as an `object_death` - that
   would collide with S7's "the twin dies at object_death".
3. **Composite programs.** `GetProgramForDraw` flattens a bound pipeline into a client-built
   composite (`MG_State/GLState/Core.cpp:609-628`, `MG_Impl/Pipe/CompositeResolver.h`); the
   flattening stays on the GL thread at emit and the band handle is fine. What is unsettled
   is the uniform mirror (`Core.cpp:590-606`): it rewrites the composite's block bindings
   every draw, so `set_program_bindings` may fire per draw for a pipeline workload unless the
   equality bail-out at `ProgramObject.h:1018-1022` absorbs it. Unmeasured.
4. **Sampler-unit tail size.** `uniformSamplerOrImageUnitIndex` is indexed by LOCATION and
   `maxUniformLocation` can be in the thousands. The sparse encoding assumes few sampler
   locations - true for every program `CacheResourceLocations` walks
   (`Managers.cpp:13040-13077`, which skips image uniforms outright) - but a large sampler
   array is the counter-example. Needs a declared bound and a counted refusal.
5. **Boundary with S5.** `:5463` (`GetUniformBlockBinding`) is mine, `:5464` onward (the
   binding point) is S5's, and neither lands alone: `set_shader_buffers` and
   `set_program_bindings` belong to ONE package or the UBO loop keeps a live read either way.
   Same split at `:5504-5506` (the gate is program state, the body is binding points).
6. **Codec completeness.** `GetUniformType` / `GetMaxUniformLocation` on the alias path
   (`:4849-4852`) are covered by G-A only if the archive's `uniformIndexInTProgram` and type
   tables survive the codec. `ProgramArtifactsCodec.cpp:242` already carries a libc++-branch
   caveat about a member the codec skips; that member must be named before the design may
   claim "every reflection question is answered from the archive". Unverified at this head.
