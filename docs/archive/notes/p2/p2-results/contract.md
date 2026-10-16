# P2 package A, commit `c0` — the contract (`p2/contract`)

Tree: `~/w7/p2-contract`, branch `p2/contract`, from `feat/disaggregated@48268068`.

**Commit: `9c6a8a25`** — `[Feat] (Pipe): land the P2 contract - real storage for the three
swallowed capabilities, the render-state chunk table and its subset hash, the in-process
applier, the slot allocator, the subsystem bitmask and the residual ratchet down to 8`
**Tag: `p2/contract` → `9c6a8a25`.** Not pushed, per the brief.

30 files, +1826 / -130.

---

## 1. What landed, against C.0's table

| C.0 row | file | done |
|---|---|---|
| MODIFY (D3.1) | `MobileGL/MG_Pipe/MGPipeValueTypes.h` | three `Bool`s in the `[581, 584)` hole, between `ColorMasks` and `ClearColor` |
| MODIFY (D3.2) | `MobileGL/MG_Pipe/PipeFields.def` | three rows in `MGP_FIELDS_RenderStateParameters`; `MGP_FIELDS_ResidualValueBlock` down to `F(CapabilityBits)` |
| MODIFY (D3.3, D3.4) | `MG_State/GLState/RenderState/RenderState.{h,cpp}` | three `SET_CAPABILITY` + three `RETURN_CAPABILITY` arms in `CapabilityInput` order; `{}` on `m_parameters`, `m_pixelStorePackParameters`, `m_pixelStoreUnpackParameters` |
| MODIFY (D9, D6) | `MobileGL/MG_Pipe/MGPipeTypes.h` | `ResidualValueBlock` → one `Uint64 CapabilityBits`; `MGL_RESIDUAL_BLOCK_SIZE` 1248 → 8; the `MGPDynamicState` comment now says sample coverage is **pipeline**, with the rule that decides it |
| MODIFY (D14) | `MobileGL/MG_Pipe/MGPipe.h` | the seven subsystem bits, the behaviour bit 63, `kMGPipeSubsystemsMigratedAtP2 = 0x7f` |
| CREATE (D6) | `MobileGL/MG_Pipe/MGPipeRenderStateSpans.{h,cpp}` | the chunk table from `offsetof`, the partition `static_assert`s, `MGPipeComputePipelineSubsetHash()`, `VulkanRenderer.cpp:4780-4794`'s enumeration moved in as provenance |
| CREATE (D2, D5) | `MobileGL/MG_Pipe/PipeApply.{h,cpp}` | the apply entry points, the per-context CSO store, `MGPipeDeriveRenderStateFields()` as a stub |
| CREATE (D13) | `MobileGL/MG_Impl/Pipe/SlotAllocator.{h,cpp}` | per-kind `{slot, gen}` allocator, free list + high-water, `lifetimeId → slot` maps, debug wrap assert |
| MODIFY | `scripts/gen_pipe.py` | `PIPELINE_STATE_MEMBERS` → the D6 set (44, declaration order); `gen_span_table()`'s "deliberately absent" block rewritten to record the answers; `ResidualValueBlock` `offsetof` asserts; `kMGPipeFieldEmittedBy[]` from `MGP_COVERAGE_EMITTED_LIST`; one more `--self-test` control |
| MODIFY | `MG_Pipe/generated/*.inc` | `PipeFilled.inc`, `PipeSpanTable.inc`, `PipeWire.inc` regenerated and committed |
| MODIFY (D5) | `MobileGL/MG_Pipe/Coverage.def` | `MGP_COVERAGE_EMITTED_LIST` (34 rows); the `GetProvokingVertexMode` note answered |
| MODIFY (D14) | `CMakeLists.txt` | the three new sources inside `if (MOBILEGL_PIPE_PUSH)`; `option(MOBILEGL_PIPE_LEGACY_MEMOS … ON)`, forced ON with a `message(STATUS)` when `MOBILEGL_PIPE_PUSH=OFF`; `-DMOBILEGL_PIPE_LEGACY_MEMOS=1` |
| MODIFY (D14, D18) | `MobileGL/Config.h`, `MobileGL/ConfigLoader.cpp` | the push-build default `kMGPipeSubsystemsMigratedAtP2`, the bit list in the comment, `Features.PipeHandleAbaControl` |
| MODIFY (D17) | `MG_Util/Metrics/PipeStats.{h,cpp}` | `CallClass::{RenderStateCsoMints, RenderStateCsoBinds}`, names `render-state-cso-mints` / `render-state-cso-binds`, short `csom` / `csob` on the summary line |
| MODIFY | `MG_Test/Util/PipeStatsTest.cpp` | the two names pinned in `CounterNamesAreStable`; `cso[csom=` pinned in `SummaryLineCarriesEveryClassAndGate` |
| CREATE (stubs) | `MG_Test/Pipe/{RenderStateSpansTest, TrackerTest, SlotAllocatorTest, CsoCacheTest}.cpp` | one placeholder `TEST` each, each asserting something already true so the binary is never a silently-green empty lane |
| MODIFY | `MG_Test/Pipe/CMakeLists.txt` | the four registered, `LABELS unit` |
| MODIFY | `MG_Test/Pipe/PipeCatalogueTest.cpp` | residual size 1248 → 8; `PipelineSubsetMembersArePinned` 24 → 44 |

Nothing beyond that table plus the two additions in §4.

## 2. The chunk table, as the compiler computed it

The brief's D6 offsets were **correct in every entry**. Confirmed by a probe program before
writing the table and then by the file's own `static_assert`s.
`sizeof(RenderStateParameters)` is still **1168**; the three new bools sit at **581 / 582 /
583**, exactly the free hole, so no existing member moved (`BlendStates` 312, `LogicOp` 536,
`ColorMasks` 549, `ClearColor` 584, `StencilStates` 768, `ScissorBoxes` 904 all unchanged).

The file expresses the table as **16 boundaries**, not 15 ranges, because the halves
alternate perfectly (chunk 0 dynamic, chunk 1 pipeline, …). Every boundary is an `offsetof`
or a `sizeof`.

| # | half | range | size | members |
|---|---|---|---|---|
| D0 | dynamic | `[0, 264)` | 264 | `Viewports[16]`, `LineWidth`, `PointSize` |
| P0 | pipeline | `[264, 292)` | 28 | `PatchVertices`, `PatchDefaultOuterLevel`, `PatchDefaultInnerLevel` |
| D1 | dynamic | `[292, 312)` | 20 | `PolygonOffsetFactor/Units/Clamp`, `ClipOrigin`, `ClipDepthMode` |
| P1 | pipeline | `[312, 584)` | 272 | `BlendStates[8]`, `LogicOp`, `DepthTestEnabled`, `DepthFunc`, `DepthMask`, `ColorMasks[8]`, **the three new bools** |
| D2 | dynamic | `[584, 752)` | 168 | `ClearColor`, `ClearDepth`, `ClearStencil`, `BlendColor`, `DepthRanges[16]` |
| P2 | pipeline | `[752, 772)` | 20 | `SampleCoverageValue/Invert`, `SampleMaskValue`, `MinSampleShadingValue`, `StencilStates[0].Func` |
| D3 | dynamic | `[772, 784)` | 12 | `StencilStates[0].{Ref, ValueMask, WriteMask}` |
| P3 | pipeline | `[784, 800)` | 16 | `StencilStates[0].{FailOp, PassDepthFailOp, PassDepthPassOp}`, `StencilStates[1].Func` |
| D4 | dynamic | `[800, 812)` | 12 | `StencilStates[1].{Ref, ValueMask, WriteMask}` |
| P4 | pipeline | `[812, 840)` | 28 | `StencilStates[1].{FailOp, …}`, `CullFaceEnabled`, `CullFaceModeSetting`, `FrontFaceModeSetting`, `ProvokingVertexModeSetting` |
| D5 | dynamic | `[840, 868)` | 28 | four hints, `PointFadeThresholdSize`, `PointSpriteCoordOrigin`, `ClampReadColor` |
| P5 | pipeline | `[868, 876)` | 8 | `PolygonModeFront`, `PolygonModeBack` |
| D6 | dynamic | `[876, 880)` | 4 | `PrimitiveRestartIndex` |
| P6 | pipeline | `[880, 904)` | 24 | the 20 capability bools, `ScissorTestEnabledMask` |
| D7 | dynamic | `[904, 1168)` | 264 | `ScissorBoxes[16]`, `ScissorBoxWrittenMask`, `ClipDistanceEnabledMask` |

**7 pipeline chunks / 396 bytes + 8 dynamic chunks / 772 bytes = 1168.** Both totals are
`static_assert`ed in the header, as is the partition (`kMGPipePipelineChunkBytes +
kMGPipeDynamicChunkBytes == sizeof(RenderStateParameters)`) and the strict ascent of the
boundaries.

The split was re-derived from the source before being written: every setter in
`RenderState.cpp` was read and classified by `BumpVersions()` vs `++m_version`, and the
result agrees with D6 in every member, including the two the brief flags —
`SetSampleCoverage` calls `BumpVersions()` (pipeline, so the `MGPipeTypes.h` comment was
wrong and is fixed) and `SetClipControl` does not (dynamic).

`kMGPipePipelineStateMembers` is now **44** names (24 + `SampleCoverageValue`,
`SampleCoverageInvert`, `FrontFaceModeSetting`, `ProvokingVertexModeSetting`,
`ScissorTestEnabledMask`, `PolygonModeBack`, the 11 unhashed capability bools, the 3 new bools).

## 3. Verification — every command and its result

All in `~/w7/p2-contract` on the committed tree.

| gate | command | result |
|---|---|---|
| generators | `python3 scripts/gen_pipe.py --check` | **rc 0**, `generated files are up to date`; `inventory 477 rows … 0 UNMAPPED` |
| generators | `python3 scripts/gen_pipe.py --self-test` | **rc 0**, `7 negative-control trip(s), positive control OK` (6 pre-existing + the new "emitted row naming a call that does not exist") |
| G13 include closure | `python3 scripts/check_include_closure.py` | **rc 0**, `4 probes, 0 skipped, 0 problem(s)`; value-header 66 headers / 0 forbidden, mutation-header 84 / 0 |
| build (pull) | `cmake --build build-linux -j 12` | **rc 0** |
| build (push) | `cmake --build build-push -j 12` | **rc 0** |
| build (verify) | `cmake --build build-verify -j 12` | **rc 0** |
| unit (pull) | `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | **rc 0**, `100% tests passed, 0 failed out of 1489` |
| unit (push) | `ctest --test-dir build-push -L unit --no-tests=error -j 8` | **rc 0**, `100% tests passed, 0 failed out of 1489` |
| unit (verify) | `ctest --test-dir build-verify -L unit --no-tests=error -j 8` | **rc 0**, `100% tests passed, 0 failed out of 1489` |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: **0 added, 0 removed, 4 resized, 0 renamed**`; `.text +160 (+0.001%)`, `.data/.bss/.rodata +0`, file size **identical** (19100448 bytes both sides) |
| **G5** | `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' …/DirectGLES.cpp \| sha256sum` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` — **equal** to `~/w7/p2-before-syncrenderstate.sha` |
| **G14** | `ctest --test-dir build-linux -N \| … \| comm -23 <baseline> -` | **0 removed**; 4 added (the four stub placeholders) |
| G2 (names) | `diff` of `ctest -N` names, build-linux vs build-push | **identical**, 2367 each |
| G13 purity | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | **empty** |
| dirty surface | `python3 scripts/gen_pipe_dirty_surface.py` vs `~/w7/p2-before-dirty-surface.txt` | **unchanged** (byte-identical); `--summary` still works, as B needs until E lands |

### G1's four resizes, all attributed

| symbol | before | after | delta | why |
|---|---|---|---|---|
| `MG_State::GLState::RenderState::RenderState()` | 1700 | 1848 | +148 | D3.4's three `{}` |
| `RenderState::SetCapability(CapabilityInput, bool)` | 850 | 927 | +77 | D3.3's three `SET_CAPABILITY` arms |
| `RenderState::IsCapabilityEnabled(CapabilityInput) const` | 239 | 268 | +29 | D3.3's three `RETURN_CAPABILITY` arms |
| `_GLOBAL__sub_I_DirectGLES.cpp` | 1340 | 1331 | **−9** | **not predicted by D15.** `DirectGLES.cpp:1986` holds `static RenderStateParameters g_syncedRenderStateParameters;` at namespace scope; its dynamic initialiser re-schedules around the three new default-initialised members. A *shrink*, in a static-init thunk, with no interface consequence — the direct and only possible consequence of the struct gaining members |

D15 predicted exactly three; the fourth is named here and in the commit message, which is
what §E's row ("the gate admits an attributed delta, never an unexplained one") asks for.
**The integrator should add this fourth symbol to G1's admitted set for every later merge**,
because it will reappear in every symbol report taken against `~/w7/p2-before-libMobileGL.so`.

Not run (not this package's, or needs the whole tree): G3, G4 (retrace), G6/G7 (the
`RenderStateSpansTest` body is c2 and `scripts/g7_negative_control.sh` is package E's),
G8, G9, G10's device half, G11, G12.

## 4. Deviations

Ordered by how much a reviewer should care.

1. **`PipeStats`' two new `CallClass` values are compiled only under `MOBILEGL_PIPE_PUSH`.**
   D17 says to add them in the contract commit and says nothing about a guard, but
   `PipeStats.cpp` is a *pull-build* file: adding two enumerators there grew
   `g_frameCalls` / `g_totalCalls` / `kCallClassNames` from 48 to 64 bytes, resized
   `FormatWindowLine` (+1414), `ResetCounters`, `EmitSummaryLine`, `AdvanceSummaryWindow`
   and `OnPresent`, and **added two symbols**
   (`…PipeStats::(anonymous namespace)::g_windowBaseCalls.6/.7`) — i.e. `1 added ≠ 0` and
   9 unattributed resizes, a straight G1 failure. Guarding them restores
   `0 added / 0 removed / 0 renamed` and the four attributed resizes above. The counters
   can only ever be non-zero in a push build (a render-state CSO does not exist otherwise),
   so nothing is lost. `PipeStatsTest`'s two new assertions carry the same guard.
   D15's own rule — "none of them touches the pull build" — is what this restores.
2. **`MobileGL/MG_Backend/MGPipe/PipeInputs.h` gained one line: `friend struct
   MGPipeApplyAccess;`.** That file is in nobody's column of C.5 ("everything else |
   nobody"), but D2 is unimplementable without it: `PipeInputs`' storage is `private:` with
   exactly one friend (`MGPipeFillAccess`, defined in `PipeFill.cpp`, which package B owns),
   and the applier has to write `m_renderState`. Defining a second `MGPipeFillAccess` in
   `PipeApply.cpp` would be an ODR violation. A friend declaration emits no symbol, and the
   pull build's symbol set confirms it (0 added). The comment beside it records that the
   applier deliberately does **not** stamp the poison generations — that statement belongs
   to the walk in `PipeFill.cpp`, which is B's to make.
3. **`PipeApply.h` declares an eighth entry point, `MGPipeApplyDeleteRenderState`.** D2 lists
   seven; D7 requires that `CsoCache`'s LRU eviction emit `DeleteRenderState`, and that call
   needs a server side. Package B owns `CsoCache` and does not own `PipeApply`, so without
   this the eviction path either leaks applier slots or forces B to edit my file. It uses
   the existing `DeleteRenderState` opcode (`PipeCalls.def:94`); no opcode was added.
4. **`MGPipeDeriveRenderStateFields()` is a stub, as C.0's ordered steps require** ("stub
   bodies for `PipeApply`'s derivation so B/C/D link"). Its 29 derivations are commit `c1`
   on `p2/spans`. Consequence to flag to package B: `MGPipeApplySetResidualValueState`'s
   trip wire compares `CapabilityBits` against `PipeInputs::m_capability[]`, which only the
   derivation fills — so B must not wire `set_residual_value_state` (its step `b5`) before
   `c1` has landed, or the comparison runs against fill-loop-supplied values that happen to
   be right today but are not the derivation's answer. Nothing in `c0` calls either function.
5. **Two `PipeCatalogueTest` test *bodies* were rewritten under names that no longer
   describe them**, because G14 forbids removing a test name:
   `ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` now pins that those two value
   structs and the patch tail are **gone** and that `CapabilityBits` is at offset 0; and
   `MGPipeVerify`'s nested-struct demonstration moved from `ResidualValueBlock.RenderState`
   (which no longer exists) onto `RenderStateParameters` directly, where it now also pins
   that the comparator names `FramebufferSrgbEnabled`. Both files say so in a comment.
6. **`ConfigLoader.cpp` includes `<MG_Pipe/MGPipe.h>` under `#if MOBILEGL_PIPE_PUSH`.**
   The alternative was to hard-code `0x7f` in a second place. `Config.h` cannot include it
   (layering), and the pull build's translation unit is unchanged.
7. **`kMGPipeFieldEmittedBy[]` is an enum table, not a table of strings.** D5 says "naming,
   per `MGPipeInputField`, the P2 call"; a per-field `strcmp` in the residual fill loop
   would be absurd, so `gen_pipe.py` emits `enum class MGPipeFieldEmitter` (`kNone` plus one
   enumerator per distinct emitting call, sorted for stability), the table, and a parallel
   name array for diagnostics. Mapping an emitter onto its subsystem bit is deliberately
   left to package B — a 7-arm switch beside the code that reads the bitmask, rather than a
   second hard-coded mapping in the generator.
8. **`MGPipe.h`'s `struct MGPipeRenderStateSpans;` forward declaration was removed** and
   replaced by a comment pointing at the real header. It was a P0 placeholder for a type
   that never came into existence under that name and had no users (`grep` confirms: only
   the declaration itself and comments).

### Where the tree disagreed with the brief

Only one, and it was already flagged as such by the brief itself: the D6 table's chunk **P2**
is described in prose as four members but its range `[752, 772)` also covers
`StencilStates[0].Func` (`StencilStates` is at 768). The file derives the boundary from
`offsetof(StencilStates) + offsetof(StencilFaceState, Ref)`, so the range is right and the
prose was one member short. Every numeric offset in D6 was verified correct.

## 5. What the other four packages compile against

Frozen at `p2/contract` (`9c6a8a25`); none of this may change without telling them.

- `MG_Pipe/MGPipeRenderStateSpans.h`: `kMGPipeRenderStateChunkBoundaries[16]`,
  `MGPipeRenderStateChunkIsPipeline()`, `MGPipeRenderStateChunkAt()`,
  `kMGPipe{Pipeline,Dynamic}Chunk{Count,Bytes}`, `kMGPipeRenderStateChunkTableVersion`,
  and the gather / scatter / diff / hash functions
  (`MGPipeGatherPipelineBytes`, `MGPipeScatterPipelineBytes`,
  `MGPipe{Pipeline,Dynamic}ChunkBlobBytes`, `MGPipeGather/Scatter{Pipeline,Dynamic}Chunks`,
  `MGPipe{Dynamic,Pipeline}ChunksThatMoved`, `MGPipeComputePipelineSubsetHash`,
  `MGPipeHashPipelineBytes`). `kMGPipe{Pipeline,Dynamic}Chunks[]` are defined in the `.cpp`
  behind `generated/PipeSpanTable.inc`'s `extern` declarations.
- `MG_Pipe/PipeApply.h`: the eight `MGPipeApply*` entry points,
  `MGPipeDeriveRenderStateFields(PipeInputs&)`, `MGPipeRenderStateCsoRecord`,
  `MGPipeApplierState`, `MGPipeApplier()`, `MGPipeApplierReset()`.
- `MG_Impl/Pipe/SlotAllocator.h`: `MGPipeSlotAllocator` (`Allocate`, `AllocateFor`,
  `FindByLifetimeId`, `Acquire`, `Free`, `IsLive`, `GenOfSlot`, `LifetimeIdOfSlot`,
  `HighWater`, `LiveCount`, `FreeCount`, `Reset`) and `MGPipeSlots()`.
- `MG_Pipe/MGPipe.h`: `kMGPipeSubsystem*` bits 0..6, `kMGPipeBehaviourNoCsoContentAddressing`
  (bit 63), `kMGPipeSubsystemsMigratedAtP2` (`0x7f`).
- `generated/PipeFilled.inc`: `MGPipeFieldEmitter`, `kMGPipeFieldEmittedBy[]`,
  `kMGPipeFieldEmitterNames[]`, `kMGPipeEmittedFieldCount` (34).
- `RenderStateParameters::{FramebufferSrgbEnabled, DepthClampEnabled,
  TextureCubeMapSeamlessEnabled}`; `ResidualValueBlock{ Uint64 CapabilityBits; }`;
  `MGL_RESIDUAL_BLOCK_SIZE == 8`.
- `Features.PipeHandleAbaControl`, the `MOBILEGL_PIPE_LEGACY_MEMOS` CMake option and its
  `-D`, and `CallClass::{RenderStateCsoMints, RenderStateCsoBinds}` (push builds only).

## 6. Artefacts

- `~/w7/p2-out/names-build-{linux,push,verify}.txt` — the `ctest -N` name sets used for G2/G14.
- `~/w7/p2-out/names-before-sorted.txt`, `~/w7/p2-out/removed.txt` (empty).
- `~/w7/p2-out/dirty-surface-after.txt` — byte-identical to the baseline.

**A trap for whoever re-runs G14**: the brief's extraction command
`grep -E '^\s+Test #'` only matches ctest's *four-digit* rows, because ctest pads the
number (`Test   #1:` vs `Test #1234:`); it silently returned 1368 of 2367 names here. Use
`grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:'` and `LC_ALL=C sort` on both sides.
