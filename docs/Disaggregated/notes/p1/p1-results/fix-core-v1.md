# P1 lane fixes — package `fix-core` (F1 + F2)

Tree: WSL `~/w7/p1-fix-core`, branch `p1/fix-core`, branched from the integrated
`feat/disaggregated` @ `97b997d5`. Not pushed.

## Commits

| sha | subject |
|---|---|
| `80a6b390` | `[Fix] (Pipe): fill the capability set on a texture op and a dispatch, and the shader blit's viewport, provoking vertex and buffer bindings` |
| `6b681c4a` | `[Fix] (Pipe, State): refresh a pushed PipeInputs field when the frontend moves it inside a verb` |
| `9bd6d394` | `[Test] (Pipe): pin the push-on-mutation shape - a frontend write inside a verb refreshes the pushed field, and only its value` |

Files touched: `MobileGL/MG_Pipe/FillPoints.def`,
`MobileGL/MG_Pipe/generated/PipeFillPoints.inc` (regenerated),
`MobileGL/MG_Pipe/PipeMutation.h` (new), `MobileGL/MG_Impl/Pipe/PipeFill.cpp`,
`MobileGL/MG_State/GLState/TextureState/TextureState.h`,
`MobileGL/MG_Test/Pipe/PipeInputsTest.cpp`. No backend file, no other package's file.

---

## F1 — missing fill rows

### What the lane proved

`~/w7/retrace-out/p1-verify/` aborted four cases with
`Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@GenerateMipmap"}` (3) and
`"IsCapabilityEnabled@DispatchCompute"` (1), all `DirectVulkan`.

Root cause, traced in the tree: `VulkanRenderer::MaterializePendingClearForTexture`
(`VulkanRenderer.cpp:7865`) calls `VkClearManager::PreCompensateSrgbClearColor`, which reads
`IsCapabilityEnabled(FramebufferSrgb)` (`VkClearManager.cpp:58`). It is reached from
`GenerateMipmap` (`:11000`, a `kTextureOp` verb) and from `DispatchCompute` through
`PrepareStorageImageTextures` (`:5763`, a `kDispatch` verb). `kDraw`, `kClear`, `kBlitOrCopy`
and `kReadback` already named the field; those two classes did not.

### The audit that followed

Not stopped at the two rows. A static verb-class → field reachability pass over `MG_Backend`
(name-based call graph, roots taken from both `GetBackendFunctions()` tables;
`scratchpad/wf4/fix-core/reach.py`) produced candidate rows with their call paths, and each
candidate was then hand-verified against the tree — the pass merges same-name functions, so
most candidates were collisions (`Clear`, `SyncToBackend`, `GetOrCreatePipeline` on
`PipelineFactory` vs `VulkanRenderer`) and were dropped. Two real families survived:

| class | rows added | verified path |
|---|---|---|
| `kDispatch` | `IsCapabilityEnabled` | `DispatchCompute` → `PrepareStorageImageTextures` → `MaterializePendingClearForTexture` → `PreCompensateSrgbClearColor` |
| `kTextureOp` | `IsCapabilityEnabled` | `GenerateMipmap` (`VulkanRenderer.cpp:11000`) → the same |
| `kTextureOp` | `GetFramebufferBindingSlot`, `GetBufferBindingPoint` | `GenerateMipmap` → `GenerateDepthMipmapWithShader` (`:11069`) → `BindProgramUniformBuffers` (`:4755`) → `ResolveSamplerDescriptor` (`UniformManager.cpp:555`, the feedback-loop check) / `ResolveStorageBufferDescriptor` (`:1194`) |
| `kBlitOrCopy` | `GetViewportIndexed`, `GetDepthRangeIndexed` | `BlitNamedFramebuffer` → `TryBlitToDefaultFramebufferWithShader` (`:8671`) → `ApplyGLViewportState` (`:8502`) → `ComputeGLViewport` |
| `kBlitOrCopy` | `GetProvokingVertexMode` | same → `GetOrCreateBlitPipeline` (`:8511`) → `SelectProvokingVertexMode` (`:4434`) |
| `kBlitOrCopy` | `GetBufferBindingPoint` | same → `BindProgramUniformBuffers` (`:8576`) → the buffer-block resolvers |

Every added row carries its derivation in a comment in `FillPoints.def`. The commit also
unfolds the `kReadback` transform-feedback rows that `9087f133` landed on one 1200-column
line; no row changed there.

---

## F2 — the backend mutates frontend state inside a verb

### Option chosen: **push on mutation** (the findings' preferred option)

Justification:

- It keeps the comparator's invariant — "the pushed block equals the live context at every
  read" — literally true, so the push build's semantics stay equal to the pull build's. The
  fallback (a volatile-in-verb class) would instead have made the push build read the
  boundary value where the pull build reads the moved one, i.e. it would have *kept* the
  behavioural difference and hidden it.
- The compare-at-read arm is the only comparator arm that is real in P1 (the entry compare is
  tautological until P2's tracker fills the first arm, per brief D8). The fallback's mechanism
  is precisely "skip compare-at-read for this field", which would blind the P1 gate on the one
  field that found the bug.
- It is the shape P2's tracker needs anyway (findings, F2 bullet 1).

### Mechanism

`MobileGL/MG_Pipe/PipeMutation.h` (new, leaf layer that both `MG_State` and `MG_Impl` may
include) declares `MGP_NOTE_MUTATION(Field)`. In the pull build the macro is `((void)0)` and
the header includes nothing. Under `MOBILEGL_PIPE_PUSH` it calls
`MGPipeNoteFrontendMutation(field)`, defined in `MG_Impl/Pipe/PipeFill.cpp` next to the filler
whose `MGPipeFillAccess::CopyField` it reuses. The notice refreshes one field's **value** when

1. a context is live,
2. a verb has been filled (`m_currentVerb != kVerbCount`),
3. the field is not sticky (a forwarded field has no storage), and
4. the field is in the current verb class's `kMGPipeClassFieldMask`.

It never touches the poison stamp, so a stamp `MOBILEGL_PIPE_POISON_OMIT` withheld stays
withheld (negative control B survives) and a field the verb never filled stays
`Fatal{UnmigratedPipeInput}` on the next read instead of being healed by an unrelated write.

### The enumeration asked for

**Step 1 — what a backend can write into the frontend.** `scout-pull-sites.md` §2.6 lists the
sites; re-derived live on this tree by matching every non-const `MG_State` method declaration
against every call in `MG_Backend/**` (`scratchpad/wf4/fix-core/scan.py`). Nothing is reached
through `MGB_CTX` itself: the only non-read accessors the backends call are `RecordError`,
`InvalidateCompileEnv` and `ValidateProgramName`, all three F-class/sticky with no storage.
Everything else is a method call on an object the backend obtained by handle or created itself.

| written frontend object | mutators the backends call | pushed field it can move |
|---|---|---|
| `SamplerObject` | `SetMinFilter`, `SetMagFilter`, `SetMipmapMode`, `SetWrapS/T/R`, `SetLodRange`, `SetCompareMode`, `SetSamplerCompareFunc` (`VulkanRenderer.cpp:4270-4276`) — each ends in `SamplerObject::BumpVersion()` (`SamplerObject.cpp:34`) | **`GetSamplingResolutionGeneration`** |
| `ITextureObject` | `AllocateStorage`, `MarkStorageDirty`, `TruncateMipmapLevels`, `SetInternalFormat`, `SetSamples`, `SetFixedSampleLocations` (`UniformManager.cpp:1484-1498,1625-1635` fallback-texture creation; `DirectGLES.cpp:6326-6327`) — each ends in `TextureObjectBase::BumpShapeVersion()` (`TextureObject.cpp:37`); `SetInternalFormat` on a context default texture additionally bumps the bind generation (`TextureObject.cpp:96`) | **`GetSamplingResolutionGeneration`**, **`GetTextureBindGeneration`** |
| texture unit / texture state | any bind path (`TextureState::NoteUnitTouched`), `TextureUnit::SetSamplerObject`, `TextureState::MarkTextureObjectForDeletion` | **`GetTextureBindGeneration`**, **`GetMaxTouchedTextureUnit`** |
| `BufferObject` | `MarkGpuWritten`, `SetBackendResource`, `EnsureGpuResidentStorage`, `WritebackFromBackend`, `SyncPersistentMappedRange`, `SyncGpuWrites` | none — buffers are reached through the O-class binding slots/points, which are live pointers into the context, so a content or resource change is visible to the backend without any copy |
| `ProgramObject` | `SetDrawID`, `SetBaseVertex`, `SetBaseInstance`, `SetViewportPassMask`, `AttachShader`, `Link`, `MarkUBOContentDirty`, `SetBackendHashMemo` | none — `GetProgramForDraw`/`ForDispatch`/`TransformFeedbackProgram` are O-class `SharedPtr` copies; the pointee is shared, so its mutations are seen live and the handle itself never moves inside a verb |
| `VertexArrayObject` | `SetBackendHashMemo`, `SetBackendStateMemo`, `SetBackendAuxMemo` | none — same reason (`GetBoundVertexArray` is O-class) |
| `GLContext` | `RecordError`, `InvalidateCompileEnv` | none — both are sticky F-class with no storage |

**Step 2 — the frontend mutators that bump a pushed value.** Only six `PipeInputs` fields are
*derived* counters rather than plain state copies: `GetSamplingResolutionGeneration`,
`GetTextureBindGeneration`, `GetMaxTouchedTextureUnit`, `GetTextureContextId` (immutable per
context), `GetRenderStateParametersVersion` / `GetPipelineStateVersion` (moved only by
`RenderState`'s setters, which no backend calls — the scan finds no `RenderState` mutator in
`MG_Backend`, and `DirectGLES`'s `ScopedEmulationDrawState` / `ScopedRestartIndexSubstitution`
save and restore *native* GL state, never frontend state), plus the transform-feedback
counters (`AddTransformFeedback*`, `EndTransformFeedback` — `GLContext` mutators no backend
calls).

**Step 3 — the intersection, i.e. what is handled.** Three fields:

| field | hook site |
|---|---|
| `GetSamplingResolutionGeneration` | `TextureState::BumpSamplingResolutionGeneration()` |
| `GetTextureBindGeneration` | `TextureState::BumpTextureBindGeneration()` and the `bindingChanged` branch of `TextureState::NoteUnitTouched()` |
| `GetMaxTouchedTextureUnit` | the high-water branch of `TextureState::NoteUnitTouched()` |

The hooks sit on the **counters**, not on the individual writers, so every path in the table
above (and any writer added later) is covered by three notices rather than forty.

---

## Verification (all run in `~/w7/p1-fix-core`)

Build directories configured to the brief's C recipe: `build-linux` (pull),
`build-push` (`MOBILEGL_PIPE_PUSH=ON`), `build-verify` (`MOBILEGL_PIPE_VERIFY=ON`,
`MOBILEGL_ITEST_REQUIRE_GPU=ON`); `CCACHE_BASEDIR=/home/swung/w7`; all three build clean.

**(a) pull build symbol-identical**

```
python3 scripts/symbol_report.py --before ~/w7/p1-before-libMobileGL.so \
        --after build-linux/libMobileGL.so --threshold 0
  .text 10792579 -> 10792579 (+0, +0.000%)
  .data/.bss/.rodata/Total all +0
  27799 -> 27799 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
  Removed (0) / Added (0) / Resized (0) / Renamed only (0)
```

**(b) generator**

```
python3 scripts/gen_pipe.py --check     -> "generated files are up to date"
                                           71 calls, 69 verify payloads, 63 fields (7 sticky),
                                           69 verbs, 9 classes, 0 UNMAPPED
python3 scripts/gen_pipe.py --self-test -> "6 negative-control trip(s), positive control OK"
```

**(c) `ctest -L unit`**

| build | result |
|---|---|
| `build-linux` | **100% tests passed, 0 failed out of 1485** |
| `build-push` | 11 failed out of 1485 |
| `build-verify` | 11 failed out of 1485 |

The 11 in push/verify are **exactly** the F3 list owned by the other package
(`DirectGLESSanity.{BindsAMultisampleTextureDespiteTheDefaultMipmapFilter,
BindingZeroClearsPreviousNativeTextureBinding}`, `DirectGLESTextureSync.UnitMemoRefusesToDrive
ATwinFromAnotherTexture`, the seven `FramebufferTest.*`, `TextureTest.StorePackedWordsToClient
CopiesWordsVerbatimUnderPackParams`) — they construct a `GLContext` by hand and call a backend
helper with no verb in flight, and they are not mine to fix.

**(d) `ctest -L integration-verify` in `build-verify`**

```
ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4
  exit=0
  100% tests passed, 0 tests failed out of 818
  grep -c "Fatal{UnmigratedPipeInput" -> 0
  grep -c "Fatal{PipeVerifyDiffer"    -> 0
```

The eight `DirectVulkan.Verify.{UnboundImageDescriptorScenario,SampledSetStalenessScenario}.*`
aborts of F2 are gone. The four poison/verify control entries
(`DirectGLES|DirectVulkan.{VerifyCorrupted.PipeVerifyArmingScenario.CorruptedFieldIsReported,
PoisonOmitted.PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb}`) still pass, i.e. the gate
can still go red for its reason.

**(e) full 79-case retrace under verify**

```
cp .../wf2/retrace_gate.py ~/w7/retrace_gate.py            # byte-identical to the one already there
MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py \
  --tree ~/w7/p1-fix-core --lib ~/w7/p1-fix-core/build-verify/libMobileGL.so \
  --out ~/w7/retrace-out/fix-core -j 4
  79 tests selected of 79
  passed 79 / 79; failed: []
```

```
cd ~/w7/retrace-out/fix-core
ls *.log                                                   -> 79
grep -l "armed, zero divergences, zero unmigrated reads"   -> 79
grep -l "Fatal{" *.log                                     -> 0
grep -r "Fatal{|UnmigratedPipeInput|PipeVerifyDiffer" .    -> 0 each
```
Sample summary line: `-- MGPipe verify: improved-transparency-minecraft-26.3 DirectGLES armed,
zero divergences, zero unmigrated reads`.

**(f) `ctest -N` name diff**

```
ctest --test-dir build-linux -N | grep -E '^\s+Test +#[0-9]+: ' \
  | sed -E 's/^ *Test +#[0-9]+: //' | sort > ~/w7/fc-ctest-names.txt
comm -23 ~/w7/p1-before-ctest-names.txt ~/w7/fc-ctest-names.txt | wc -l   -> 0   (removed)
comm -13 ~/w7/p1-before-ctest-names.txt ~/w7/fc-ctest-names.txt | wc -l   -> 29  (added)
```
26 of the 29 came in with the merged P1 packages; the 3 new ones are this package's.

**Extra: include-closure gate**

```
python3 scripts/check_include_closure.py -> 3 probes, 0 skipped, 0 problem(s)
```

**Extra: falsification of the new tests.** With `MGPipeNoteFrontendMutation`'s body
short-circuited and `build-verify` rebuilt,
`PipeInputsTest.AFrontendMutationInsideAVerbRefreshesThePushedField` and
`...DoesNotDivergeAtRead` both FAIL (the second by `SIGABRT` on
`Fatal{PipeVerifyDiffer, "GetSamplingResolutionGeneration@DrawArrays", where=read}` — the lane
failure reproduced in a unit test); `...TheMutationNoticeRefreshesTheValueButNotTheStamp` stays
green, which is correct for a guard case. The edit was reverted and the build restored before
committing (`git diff` clean on that file).

---

## Deviations

1. **Tree path.** The brief names the tree `~/w7/fix-core`; the worktree that exists is
   `~/w7/p1-fix-core` on branch `p1/fix-core` (`git worktree list` at `97b997d5`). Tree wins
   for facts; everything was done there.
2. **Build directories did not exist.** The brief says the tree "has three configured build
   directories". `build-linux`, `build-push` and `build-verify` were absent; they were
   configured from the brief's C recipe (matching `~/w7/p1-core`'s `CMakeCache.txt` flags:
   Release, `/usr/sbin/clang++`, ccache, INFO, `MOBILEGL_BUILD_TEST=ON`,
   `MOBILEGL_BUILD_INTEGRATION_TEST=ON`, `MOBILEGL_BUILD_BENCHMARK=OFF`,
   `MOBILEGL_BUILD_TRACE_REPLAY=OFF`, mesa EGL vendor, lvp ICD;
   `MOBILEGL_ITEST_REQUIRE_GPU=ON` only for verify).
3. **A fourth file under `MG_State`.** The brief allows `MG_State/**` "only if F2's chosen
   mechanism needs a frontend hook" — it does; `TextureState.h` is the only `MG_State` file
   touched, and only by three macro statements plus one include that expands to nothing in the
   pull build (`.text` delta 0 confirms it).
4. **`ctest -L integration-verify` counts 818 entries**, not the brief's 742. The label's
   membership grew with the merged P1 packages; nothing was excluded.
5. **Reformatting inside `FillPoints.def`.** The `kReadback` transform-feedback rows that
   `9087f133` landed on a single 1200-column line were unfolded to the file's one-row-per-line
   shape. No row was added, removed or moved between classes by that edit.
6. **The static reachability pass is a helper, not a gate.** `scratchpad/wf4/fix-core/reach.py`
   over-approximates through same-name functions; it was used to generate candidates, each of
   which was then confirmed or rejected by reading the tree. It is not wired into CI.

## Unfinished / known gaps

- **F3 is not fixed here** (it is the other package's): 11 `ctest -L unit` entries still fail
  in `build-push` and `build-verify` on this branch, and they will keep failing until
  `p1/fix-tests` lands. `build-linux` is 1485/1485.
- **`GetFramebufferBindingSlotFast`** (`DirectGLES.cpp:1611,1905,2743,2858,2933`) still
  bypasses the accessor once its cache is warm, so those five reads are neither poison-checked
  nor compare-at-read verified. That is the known P2 gap recorded in brief D4, untouched here.
- **The notice is single-threaded**, exactly like the filler it shares state with: both write
  `gPipeInputs` with no synchronisation. Unchanged risk profile, but worth naming before P2
  puts a tracker behind the same global.
- **`GetBufferBindingPoint` on `kTextureOp`/`kBlitOrCopy` is a "may-read" row**, not one the
  lane exercised: the helper programs those two verbs draw with declare no SSBO today, so the
  resolver that reads the binding points is reachable in code but not hit by the 79 traces or
  the 818 integration entries. It was added because the path exists; if the table is ever
  trimmed for fill cost, that is the first row to re-examine.
- Nothing was pushed, and `docs/Disaggregated/MEASUREMENTS.md` was not edited (not in this
  package's file list); the F2 enumeration above is the material an integrator would land there.
