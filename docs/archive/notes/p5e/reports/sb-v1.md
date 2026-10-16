# sb — `set_shader_buffers`: the binding-point tables cross the wire (P5e wave 2)

Branch `p5e/sb`, head **b004e5dc**, base `f6cfcbd3` (wave 1). 25 files, +1708/-92, two commits:
`1ebe6abe` (wire + applier + route + client emitter + gates + tests) and `b004e5dc` (the four
Espryt consumers).

## Gate

| step | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2216/2216** (2206 at base; +10 new names, each with a pull-build skip twin) |
| `isplit` | **111/111** |
| `gens` | all 7 rc=0; `check_doc_citations` rc=0 with 3 pre-existing warnings in `CONTRACT-P5.md` |
| `one` SSBO/atomic | `DirectGLES.Split.(AtomicCounter\|ProgramPipeline)` 5/5; `DirectGLES.(SsboArrayLength\|SsboArrayDynamicIndex\|StorageBufferRegrow)` 6/6 |
| `one` XFB | `Split\..*Xfb` **11/11** |
| A/B bit 13 cleared | `MOBILEGL_PIPE_PUSH=0x1fff` on `integration-split`: **111/111** — today's picture reproduced |
| `strict` | 3/111 pass, identical to the base (red by design, CONTRACT-P5C §6) |

**Strict marker table, before and after — IDENTICAL, nine rows, same counts:**
`GetFramebufferBindingSlot@Clear` 45 · `GetTextureUnitObject@Clear` 22 ·
`GetBoundVertexArray@DrawArrays` 14 · `GetImageTextureBinding@BindImageTexture` 13 ·
`GetTextureObject@CopyImageSubData` 6 · `GetProgramForDispatch@DispatchCompute` 3 ·
`ValidateProgramName@ShaderStorageBlockBinding` 2 · `GetTextureUnitObject@GenerateMipmap` 2 ·
`GetTextureUnitObject@ReadPixels` 1.
**No `GetBufferBindingPoint@*` marker exists at this head, before or after.** Every split
scenario that binds an indexed buffer aborts on an EARLIER row — pg's `GetProgramForDispatch`
or the allowlisted `ValidateProgramName` — so my family never gets to abort first. That is the
baseline document's own "lower bound" property, and it is why red-once (a) had to be executed
in a different, equivalent form (below). The A/B at `0x1fff` on the **unit** lane has two
failures, `DirectGLESTextureSync.{UnitMemoRefusesToDriveATwinFromAnotherTexture,
AnAttachmentOnlyTexturesParametersReachTheDriverWithNoSamplerView}` (SanityTest.cpp:3112) —
NOT this package's: they fail identically with the P5e tracker shutter reverted, and they are
texture-unit-memo cases. The arm the gate asks for (`integration-split`) is green.

## What changed, per file

**Wire / apply** — `MGPipeTypes.h` (`MGPShaderBuffers::WritableMask` → `Uint32[3]`, POD 32→40,
`kMGPipeShaderBufferWritableMaskWords`, `MGPipeShaderBufferMaskHas/Set`); `PipeApply.h`
(`ShaderBufferWritableMask[3][3]`); `PipeApply.cpp` (`MGPipeApplySetShaderBuffers` bodied,
`ClearShaderBufferWindows`, both resets); `PipeRoute.{h,cpp}` + `Client/WireTables.cpp` (op 38,
both arms); `Wire/PipeWireCodec.cpp` (the decode arm applies; Class/Count bounded in the layout
arm); `CapsCodec.cpp` (one static_assert).

**Client** — new `MG_Impl/Pipe/ShaderBufferEmit.h`; `BufferState.h` (`NoteBindPointChanged` /
`GetBindPointGeneration`, push-only); `GLState/Core.{h,cpp}` (the two forwarders, and the DSA
`SetNamedTransformFeedbackBinding` bump); `GL_Buffer.cpp` (the bumps in `BindBufferBase_State` /
`BindBufferRange_State`); `Tracker.h` (bits 15/16/17 rewritten); `PipeFill.cpp` (wired constant,
`P5eFamilyIsLive`, the two adapters, the validate order, the reset list); `MG_Backend/Init.cpp`
(consumer bit 13, ID-106).

**Backend** — `DirectGLES.cpp`: `ResolveBufferBindingSubsystemArm` /
`BindingPointsComeFromRecords` / `SyncBufferBindingPointsByRecord` (all file-local), the two
call sites, `MarkShaderStorageBuffersGpuWritten`'s deletion under a transport,
`SyncAtomicCounterBuffers`' record arm, the UBO loop's point half.

**Gates / tests** — `gen_pipe_dirty_surface.py` (`Touch` prefix, control 6 re-pointed);
`DirtySurface.def` (two rows + the undecided mark); `MG_Test/Pipe/CMakeLists.txt`; new
`ShaderBufferEmitTest.cpp` (8 cases); `TrackerTest.cpp` (2); `PipeCatalogueTest.cpp`,
`PipeWireCodecTest.cpp`, `RemoteClientTest.cpp` (the pins that invert).

**ID-104:** `Uint32 WritableMask[3]`, not `Uint64 + Uint32`. 96 ≥ 84 either way, but ONE word
type means one shift width, so the set and the test are a single expression each rather than a
low half and a high half that have to agree — and the applier's per-class copy goes through the
same two helpers as the payload's. POD 32 → 40, pinned in `PipeCatalogueTest`.

## kimi-audit rows 111–124

| row | site | verdict |
|---|---|---|
| 111 | `SyncBufferBindingPoints` point + `GetBoundObject` | **RETIRED** → `SyncBufferBindingPointsByRecord` |
| 112 | same: ensure probe, `GetRange`, `obj->GetSize` | **RETIRED** → `EnsureBufferResourceForHandle(nullptr, Res)`, the record's extent, `resource->storageSize` |
| 113 | `MarkShaderStorageBuffersGpuWritten` | **RETIRED BY DELETION** under a transport; the client owns the set |
| 114 | `SyncTransformFeedbackBindingPoints` | **LEFT BARRIERED** (XFB, §5.7) |
| 115 | `SyncAtomicCounterBuffers` (count, point, ensure, range, mark) | **RETIRED**; its `MarkBufferGpuWritten` deleted |
| 116 | `SyncBoundBuffer` `GetBufferBindingSlot` | **NOT MINE** — the generic slot, `FieldOwnership.def:78` (P8/P9/P13); untouched |
| 117 | UBO loop point + `GetBoundObject` + `GetRange` | **RETIRED** → `BoundShaderBuffers[Uniform][binding]` |
| 118 | UBO loop `GetBufferResource` / `IsBufferDrawClean` / `EnsureBufferResource` | **RETIRED** → the three by-handle twins |
| 119–124 | `StartPendingTransformFeedback`, `ReadbackCapturedRanges`, `ScatterCapturedRecords` | **LEFT BARRIERED** (XFB, §5.7) |

Not found as written: nothing. `MGPipeFrontendKeyedRegistryScope` at `Managers.cpp:2938`
(`HandleOfBuffer`) is NOT retired by this package — the draw path no longer reaches it for
binding points, but `EnsureBufferResource` / `IsBufferDrawClean` still do for the monolith arm
and for other families.

## Red-once, executed, reverted, quoted verbatim

**(b) the shutter** — reverted bits 15/16/17 to the pre-P5e content aggregate, rebuilt, ran:
```
../MobileGL/MG_Test/Pipe/TrackerTest.cpp:615: Failure
Expected: (m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewConstBuffers)) != (0u), actual: 0 vs 0
NEW_CONST_BUFFERS did not fire when a uniform binding point moved onto another buffer - the emitted window would still name the first one
../MobileGL/MG_Test/Pipe/TrackerTest.cpp:624: Failure
Expected: (m_tracker.LastDirty() & MGPipeDirtyBit(MGPipeDirty::NewConstBuffers)) != (0u), actual: 0 vs 0
NEW_CONST_BUFFERS did not fire for an unbind
../MobileGL/MG_Test/Pipe/TrackerTest.cpp:652: Failure
Expected equality of these values:
  m_tracker.LastDirty() & family
    Which is: 229376
  0u
    Which is: 0
a write to a buffer bound to no binding point republished the binding-point sets
```
Both directions in one revert: the old shutter UNDER-fires on a rebind and an unbind and
OVER-fires on an unrelated `glBufferSubData` (229376 = bits 15|16|17). Restored → 2/2 pass.

**(a) the record arm.** The brief's form is not executable at this head (see the strict table
above, and §"rulings"). Executed the equivalent that IS available: keep the server's record arm
and withhold the CLIENT's emission (`kMGPipeWiredBufferBindingSubsystem` back to 0), so the
server's window is empty and nothing else can fill it:
```
0% tests passed, 5 tests failed out of 5
  3464..3466 DirectGLES.Split.AtomicCounterScenario.* (Subprocess aborted)
  3467..3468 DirectGLES.Split.ProgramPipelineScenario.* (Subprocess aborted)
[08:18:24] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{UnimplementedWritebackWait} - a ClientSession is
active and buffer 1 still has an outstanding GPU write after its readback was emitted.
```
Restored → 5/5 pass. Also ran the mirror (`BindingPointsComeFromRecords()` → `false`, the
frontend walk under a live wire): green picture, marker table unchanged — the finding in §2 of
"rulings".

**(c) the sticky forward.** `rsp` is a process-wide residual-pull tally on the apply thread's
stats line, not a per-field one, so the measurable form is the delta on
`DirectGLES.Split.AtomicCounterScenario` (`MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1`):
```
=== AFTER (record arm live) ===   rsp values: 2 x rsp=196, 1 x rsp=588   (max 588)
=== BEFORE (frontend walk)  ===   rsp values: 2 x rsp=199, 1 x rsp=597   (max 597)
```
9 and 3 pulls per case removed — `GetBufferBindingPointCount` plus the per-point
`GetBufferBindingPoint` reads of the counter walk. It does not reach 0 because other families
still pull on the same thread.

**(d) no double marking.** The backend marks are deleted; the client's are the only producer.
Withheld `MarkShaderStorageBindings` + `MarkAtomicCounterBindings` on the client:
```
0% tests passed, 3 tests failed out of 3
../MobileGL/MG_IntegrationTest/Scenarios/AtomicCounterScenario.cpp:186: Failure
Expected equality of these values:   Which is: 5   Which is: 13
../MobileGL/MG_IntegrationTest/Scenarios/AtomicCounterScenario.cpp:189: Failure
Expected equality of these values:   Which is: 100   Which is: 116
```
Restored → 3/3 pass, and `SplitBufferTest`'s existing
`ProducerMarkCount(ShaderStorageBinding) == 1` / `AtomicCounterBinding == 1` pins stay green
(24/24). A revert of the backend DELETION cannot go red by construction — it is a duplicate —
so this is the honest form of (d).

**(e) the catalogue inverts.** `PipeCatalogue.{UninstalledTablesAreAllNull,
ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull}` now assert `SetShaderBuffers != nullptr`; installed rows
34 → 35, the escape total 38 → 39, `PipeRouting...EveryRoutedRowMoved` 34 → 35.

## G1 / G2 / G14

Every new client symbol is under `MOBILEGL_PIPE_PUSH` (`BufferState`'s two members and the two
`GLContext` forwarders included, so the pull build's objects do not resize) and every new server
arm under `MOBILEGL_BUILD_DISAGGREGATED`, with the frontend arm kept token for token for
`Transport=monolith` (ruling 1). The only shared-code deltas are `MGPShaderBuffers`' size (a
push-only payload) and two `static_assert`s. 10 new ctest names, all with pull-build skip twins.

## Seams I assumed

- **pg** owns `currentProgram->GetUniformBlockBinding(i)` (`DirectGLES.cpp`, the line above my
  arm). I index `BoundShaderBuffers[Uniform][binding]` with its result and did not touch it; if
  pg moves the binding source to `MGPipeShaderCsoRecord::BlockBindings`, `binding` keeps its
  meaning and my arm is unaffected.
- **fb** owns the third GPU-write mark, `MarkWritableImageBufferTexturesGpuWritten`
  (`DirectGLES.cpp`, kimi row S3). I deleted my two under a transport and left it; the brief
  calls the pair "one edit split across two files".
- **ra** flips `kMGPipeP5eClientWaitRuleLanded`. `SetShaderBuffers` is `kWaitNone` in c0e's
  static column, so it goes fire-and-forget on that day with no change here.
- **tx2 / fb** own the rows that abort ahead of mine in the strict lane.

## Rulings I need

1. **`GetBufferBindingPointCount`'s forward → FATAL under split (§5.6, §8.8) CONFLICTS WITH §6.**
   `MG_Backend/DirectVulkan/Renderer/UniformManager.cpp:1200` and `:2030` are live Magma
   readers, `FieldOwnership`'s class is unconditional, and §6 keeps Magma's P5C semantics — so
   the flip would abort a split Magma server. **I left both rows `BARRIER_PULLED`** and removed
   Espryt's only caller instead (the measured `rsp` drop above). A backend-keyed class would be
   a new mechanism and an integrator ruling.
2. **Red-once (a)'s scenarios do not exist in the split lane.** `SsboArrayDynamicIndexScenario`,
   `StorageBufferRegrowScenario` and `SsboArrayLengthScenario` carry only `integration-gpu`;
   there is no `DirectGLES.Split.` twin. In the split scenarios that DO bind indexed buffers the
   strict lane aborts on pg's row first, so `Fatal{UnmigratedPipeInput,
   "GetBufferBindingPoint@DrawVbo"}` is unreachable until tx2/fb/pg land. Re-run this red on the
   integrated tree (BRIEF §3.5) — it should be reachable there.
3. **The sink named in the brief does not exist.** `MG_Remote/Server/PipeApplier.cpp`'s
   `ServerVerbSink` has methods for the five class-B VERBS only; a `kCtxState` row's sink is the
   decode arm in `MG_Remote/Wire/PipeWireCodec.cpp`. I extended scope to two arms of that file
   (the decode case and the layout bound). pg will touch the neighbouring `SetProgramBindings`
   case — separate `case` labels, so the merge is textual only.
4. **`PipeFill.cpp` needed a conjunct outside the named ranges.** `P4aFamilyHasItsConsumer` asks
   R-8 for the RESOURCE bit and answers `true` for any non-P4a family, so ID-106's consumer bit
   13 would have had no reader. I added `P5eFamilyIsLive` (bit 13's own R-8 question + bit 13
   requires bit 7) beside it and one conjunct in `FamilyIsLive` and in `wants()`.
5. **`CONTRACT-P5E.md` §5.6 still says the `WritableMask` question is "an integrator ruling sb
   must ask for rather than pick".** ID-104 answered it; the landed contract text wants that
   paragraph replaced at landing. I did not edit c0e's file.
6. `Fatal{ProtocolCorruption, "SetShaderBuffers.Class"}` is spelled in the file's existing shape
   (`Fatal{ProtocolCorruption} SetShaderBuffers.Class - …`, as `ApplyUnitWindow` spells its
   own), not with the contract's comma-and-quotes. Say if the contract's literal form is wanted.

## What I did not do

- The program-derived `set_shader_buffers` window (BRIEF §5, trailing): the window is the
  touched high-water mark, per ruling 10. A program binding block 0 at GL point 83 still pays an
  84-entry record on its first emission after a bind.
- `set_stream_output_targets` stays unemitted and XFB stays lockstep (§5.7). Dirty bit 17 is
  computed, shutters correctly on the new generation, and names the subsystem.
- `FieldOwnership.def:80-81` stays `BARRIER_PULLED` with phase "P5e (Espryt unbarriered), P7
  (Magma)": Magma still reads `GetBufferBindingPoint`, so the four consumers are not all gone.
- No `PipeStats::CallClass` row for binding-point emissions — that would grow the counter arrays
  and the stats line for every build. The emitter keeps its own per-class tally
  (`EmissionCount(cls)`), which is what the unit suite reads.
