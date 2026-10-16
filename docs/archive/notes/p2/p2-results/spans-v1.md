# P2 package A continuation — `p2/spans` (c1, c2, c3)

Tree: `~/w7/p2-spans`, branch `p2/spans`, branched from the contract commit `9c6a8a25` (tag/branch
`p2/contract`). Build dirs: `build-linux` (pull), `build-push`, `build-verify`. Nothing pushed.

**Note on tree creation.** `wsl_p2_tree.sh spans p2/contract verify` fails: `p2/contract` exists both
as a branch (checked out in `~/w7/p2-contract`) and as a tag, so `git worktree add` refuses with
`fatal: ambiguous object name: 'p2/contract'`. The tree was created with the SHA instead:
`wsl_p2_tree.sh spans 9c6a8a25 verify`. Same commit; worth telling the other packages.

---

## 1. Commits

| sha | subject |
|---|---|
| `810850b13a8f3894d4e916b8e3df4a5a6469303f` | `[Feat] (Pipe): derive every render-state PipeInputs field from the assembled working block instead of pulling it again from GLContext` |
| `eec92cd221eda13b25b1f374b2021c2c832d112f` | `[Test] (Pipe): walk every RenderState setter and assert the pipeline-subset hash moves exactly when the pipeline version does` |
| `02b970e9c1b60d53775d578ecca8c222842b1ead` | `[Test] (Pipe): pin the slot allocator's identity contract - gen moves only on reuse and a recycled address never reproduces a handle` |

Files touched, all inside package A's ownership rows in C.5:

- `MobileGL/MG_Pipe/PipeApply.cpp` (c1)
- `MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp` (c2)
- `MobileGL/MG_Test/Pipe/SlotAllocatorTest.cpp` (c3)

No file owned by another package was touched. `git status --porcelain` is empty.

### c1 — `MGPipeDeriveRenderStateFields`

The 29 derivations of D5, transcribed one by one from the `RenderState` getter of the same name
(`MG_State/GLState/RenderState/RenderState.cpp`); `GLContext`'s accessors are one-line forwards to
those, verified by reading `Core.cpp`. The body lives in `MGPipeApplyAccess`, the struct
`PipeInputs.h:618` already names as a friend, so no new friend and no per-field accessor. Two are
not field copies and were transcribed exactly:

- `GetViewport()` — viewport 0 rounded with `std::lround` (added `<cmath>`), matching
  `RenderState.cpp`'s comment about `glGetIntegerv` rounding to nearest.
- `IsCapabilityEnabled` — the 35-way switch including `Blend -> BlendStates[0].Enabled`,
  `ScissorTest -> ScissorTestEnabledMask & 1` and the `ClipDistance0..7` run; plus the indexed twin
  for `Blend[8]` / `ScissorTest[16]`.

The derived set is exactly the 29 rows of `MGP_COVERAGE_EMITTED_LIST` minus the five non-derived
carriers (`GetCurrentVertexAttribute`, `GetPixelStoreParameters`, `GetRenderStateParameters`,
`GetPipelineStateVersion`, `GetRenderStateParametersVersion`) — 34 − 5 = 29, which matches.

### c2 — `RenderStateSpansTest.cpp`, D19's four cases

`ChunkTablePartitionsTheBlock`, `SetterConsistency`, `DerivationMatchesTheFrontendGetters`,
`DynamicChunksCoverMagmasDynamicTailKey`. The four names are the same four in every build; in a
pull build each is a visible `GTEST_SKIP`, never a vanishing test.

`ChunkTablePartitionsTheBlock` builds its member table from `PipeFields.def`'s
`MGP_FIELDS_RenderStateParameters` — the same list `gen_pipe.py` checks against the struct — with
`offsetof`/`sizeof`, so a member added to `RenderStateParameters` lands in the test automatically
and must be classified.

`SetterConsistency` drives every public `RenderState` setter (45 setters, the 25 `SET_CAPABILITY`
names, `Blend`, `ScissorTest`, both indexed capabilities, the 8 clip distances, `SetStencilFunc`
twice, `SetPolygonMode` front-only and back-only, `SetScissorBox` across the
`ScissorBoxWrittenMask` transition, and `SetPixelStoreParam` as the must-move-nothing case). Each
case also asserts `m_version` moved — the vacuity guard.

### c3 — `SlotAllocatorTest.cpp`, six cases

`ReservedHandlesAreWhatMGPipeHandlesSaysTheyAre` (the only non-push-only case, the surviving claim
of the contract's placeholder), `GenMovesOnlyOnSlotReuse`, `FreedSlotComesBackBeforeHighWaterGrows`,
`SlotZeroIsNeverHandedOut`, `LifetimeIdSurvivesARecycledAddress`,
`CompositeShaderBandIsNeverHandedOut`.

---

## 2. Verification — every command and its actual result

All run in `~/w7/p2-spans` at `02b970e9`, `CCACHE_BASEDIR=/home/swung/w7`.

| command | result |
|---|---|
| `python3 scripts/gen_pipe.py --check` | rc 0 — "generated files are up to date", 71 calls / 63 PipeInputs fields (34 emitted by a P2 call) / 69 verbs / 9 classes, 0 UNMAPPED |
| `python3 scripts/gen_pipe.py --self-test` | rc 0 — "7 negative-control trip(s), positive control OK" |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0 (informational at this branch point; `--check`/`--self-test` are package B's) |
| `python3 scripts/check_include_closure.py` | rc 0 — "4 probes, 0 skipped, 0 problem(s)"; wire-header closure 2 headers, 0 forbidden |
| `cmake --build build-linux -j 12` | rc 0 |
| `cmake --build build-push -j 12` | rc 0 |
| `cmake --build build-verify -j 12` | rc 0 |
| `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1497** |
| `ctest --test-dir build-push -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1497** |
| `ctest --test-dir build-verify -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1497** |
| `RenderStateSpansTest` (push / verify) | 4/4 PASSED in both |
| `RenderStateSpansTest` (pull) | 0 passed, **4 SKIPPED** with "push not compiled in" |
| `SlotAllocatorTest` (push / verify) | 6/6 PASSED in both; `CompositeShaderBandIsNeverHandedOut` 11 ms |
| `SlotAllocatorTest` (pull) | 1 passed, **5 SKIPPED** |
| `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — **added 0 / removed 0 / renamed 0**; resized 4 (see deviation D-1) |
| `symbol_report.py --before ~/w7/p2-contract/build-linux/libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — **added 0 / removed 0 / renamed 0 / resized 0**: my three commits move the pull build by nothing at all |
| G5: `sha256` of `namespace RenderStateImpl` in `DirectGLES.cpp` | `d8fd1c48…0efe27`, **equal** to `~/w7/p2-before-syncrenderstate.sha` |
| G13: `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| G2 shape: `diff` of `ctest -N` names, `build-linux` vs `build-push` | rc 0, 0 lines — name-for-name identical |
| G14: `ctest -N` names removed vs `~/w7/pipe/build-linux` (the untouched reference tree) | **0 removed, 12 added** (1364 → 1376) |
| G14: same, `build-verify` vs `~/w7/pipe/build-verify` | **0 removed** |

The 12 added names are the 4 `RenderStateSpans.*`, the 6 `SlotAllocator.*`, and the two placeholders
the contract commit created for packages B (`Tracker.*`, `CsoCache.*`).

### G7's negative control, run manually

`scripts/g7_negative_control.sh` does not exist on this branch — C.5 gives it to package E, so I did
not create it. The equivalent was run by hand
(`scratchpad/wf5/spans/g7_manual_negcontrol.sh`, kept for the integrator): move the P1/D2 boundary
in `MGPipeRenderStateSpans.h` from `offsetof(ClearColor)` to `offsetof(ColorMasks)` so `ColorMasks`
and the three new capability bools fall in the dynamic half — the partition stays complete, so it
still compiles once the two byte-count `static_assert`s are adjusted (396→361, 772→807).

```
build rc=0
ctest-equivalent rc=1 (non-zero is the PASS for a negative control)
SetColorMask: the pipeline-subset hash held but m_pipelineStateVersion MOVED
SetColorMaskIndexed: the pipeline-subset hash held but m_pipelineStateVersion MOVED
[  FAILED  ] RenderStateSpans.SetterConsistency
revert build rc=0 / after revert rc=0 / git status clean
```

So G7's test goes red for exactly the reason it exists, and names the setter. Package E can write
`g7_negative_control.sh` against this recipe.

---

## 3. Where the tree contradicted the brief

**T-1 — `ScissorTestEnabledMask` is a `DynamicTailKey` input AND is pipeline state.** D19 asks for a
test that *every* field `DynamicTailKey` reads lies inside `kMGPipeDynamicChunks`, "the two
exceptions being `extentX`/`extentY`/`preTransform`/`isDefaultFbo`". The tree has a third:
`DynamicTailKey::scissorEnabled` reads `ScissorTestEnabledMask` bit 0
(`MG_Backend/DirectVulkan/Renderer/VulkanRenderer.cpp`, the inventory comment above `struct
DynamicTailKey`), and that member is **pipeline** state under D6's rule, because
`SetCapability(ScissorTest)` and `SetCapabilityIndexed(ScissorTest, i)` both call `BumpVersions()`.
It is harmless — `BumpVersions()` moves `m_version` too, so `MGPDynamicState::Version` still moves
on a scissor-enable change and `ApplyDynamicDrawStateTail` still re-runs — and the test asserts it
**the other way round** (`EXPECT_FALSE(IsWhollyDynamic(...))` plus
`EXPECT_TRUE(IsWhollyPipeline(...))`), so a later table edit that demotes the mask is loud rather
than silent. The integrator may want a line in `ARCHITECTURE.md` §5.3.

**T-2 — the chunk table in the tree is not the table in D6.** D6's table puts `StencilStates[0].Func`
in chunk P3 with the face-0 ops; the landed `MGPipeRenderStateSpans.h` puts it at the end of P2. The
brief itself says "the file must derive them, and if it disagrees the file is right", and the file's
`static_assert`s (396 / 772 / partition) hold, so nothing was changed. Recorded because the D6 table
as printed is not what compiled.

**T-3 — four of D5's 29 fields are not `kDraw` fields.** `MG_Pipe/FillPoints.def` gives
`GetClearColor`/`GetClearDepth`/`GetClearStencil` to `kClear` and `GetClampReadColor` to `kReadback`
alone; D5 calls all 29 "`kDraw` `PipeInputs` fields". Reading a clear value under `DrawArrays` is
`Fatal{UnmigratedPipeInput}` — and the verify build's poison caught exactly that on the first run of
`DerivationMatchesTheFrontendGetters` (`Fatal{UnmigratedPipeInput, "GetClampReadColor@DrawArrays"}`,
`ctest` reported it as `(Subprocess aborted)`). The poison is right and the test was wrong; the case
now runs in **three verb phases** — `DrawArrays`, `Clear`, `ReadPixels` — each of which mutates the
context *after* its own fill and then applies, so each phase's `ASSERT_NE` still proves the block
disagreed before the apply. No test name changed and the poison was not weakened.

**T-4 — the field names in D5 are not the tree's.** `m_minSampleShading` is
`m_minSampleShadingValue`; `m_patchInner`/`m_patchOuter` are
`m_patchDefaultInnerLevel`/`m_patchDefaultOuterLevel`. Cosmetic; the code uses the tree's names.

**T-5 — `~/w7/p2-before-ctest-names.txt` does not correspond to any build dir of the base ref.** It
has 2363 names, of which **999 are absent from the untouched reference tree too**
(`~/w7/pipe/build-linux` lists 1364, `~/w7/pipe/build-verify` 2182; the 999 missing names are the
same set for both, e.g. all of `AsyncCompileTest.*` and `AsyncLinkTest.*`). So G14 as written
(`comm -23 ~/w7/p2-before-ctest-names.txt -` against `build-linux`) reports 999 "removals" on a tree
with no P2 change at all. G14 was therefore checked against the reference tree's own `ctest -N`
output instead: **0 removed** in both `build-linux` and `build-verify`. The integrator should
re-capture the baseline (and say which build dir it comes from) before running G14 for real.

**T-6 — `wsl_p2_tree.sh <slug> p2/contract` is ambiguous** (branch and tag of the same name). See
the header note.

---

## 4. Deviations from the brief

**D-1 — G1's admitted resize set needs a fourth symbol, and it belongs to the contract commit.**
D15 and C.0 admit exactly `RenderState::{RenderState, SetCapability, IsCapabilityEnabled}`. The pull
build also resizes `_GLOBAL__sub_I_DirectGLES.cpp`, **−9 bytes**. Attribution:
`DirectGLES.cpp:1986` holds `static RenderStateParameters g_syncedRenderStateParameters;`, and D3's
three new NSDMI members change the dynamic-initialisation code the compiler emits for that static —
the three `false` bytes fill the `[581, 584)` padding hole, so the initialiser folds into fewer
stores. It is the contract commit's delta, not this package's: `symbol_report.py` between
`~/w7/p2-contract/build-linux/libMobileGL.so` and mine reports **0 added / 0 removed / 0 renamed /
0 resized**. `namespace RenderStateImpl`'s source text is unchanged (G5 hash equal), so this is not
a `SyncRenderState` change. The contract commit is tagged and four packages branch from it, so it
was not amended; the integrator should either widen G1's admitted set to these four symbols or note
the attribution in the merge commit.

**D-2 — c2 and c3 delete the contract commit's placeholder test cases.** `RenderStateSpans.`
`PlaceholderUntilTheOwningPackageFillsThisIn` and the `SlotAllocator` one are gone, replaced by the
real cases; the `SlotAllocator` placeholder's only live claim survives verbatim as
`ReservedHandlesAreWhatMGPipeHandlesSaysTheyAre`. This is what the contract's own file headers ask
for ("STUB, and deliberately one … the package which owns its CONTENTS"), and neither name is in
the pre-P2 baseline, so G14 is not affected. Recorded because it is technically a test-name removal
inside P2.

**D-3 — `scripts/g7_negative_control.sh` was not created**, because C.5 gives it to package E. The
manual equivalent was run and is reported above; the script lives at
`scratchpad/wf5/spans/g7_manual_negcontrol.sh` for E to lift from.

**D-4 — the applier's `CompositeShaderBandIsNeverHandedOut` case skips in a DEBUG build.** Walking
the ShaderCso slot space to `kMGPipeShaderCsoCompositeSlotBase` (983 040 slots, ~16 MB, 11 ms) trips
the allocator's own "slot space is exhausted" `MOBILEGL_ASSERT`, which is live and correct in a
DEBUG build. The case therefore `GTEST_SKIP`s there and is checked in the INFO builds every gate
runs. Stated rather than silently avoided.

---

## 5. Unfinished / out of scope for this package

- The gates this tree cannot reach alone and that the integrator owns: **G2** (only its *shape* —
  pull-vs-push name-for-name — was checked here; the two `-L integration-gpu` runs need a GPU),
  **G3** (40-trace SSIM), **G4** (retrace under verify), **G8** (`HandleRecycleScenario`, package E),
  **G9** (`gen_pipe_dirty_surface.py --check/--self-test`, package B), **G10**'s device
  `MOBILEGL_PIPE_STATS` window, **G11** (two-device A/B), **G12** (the microbenchmark and the
  `CsoContentAddressing` ctest entry, packages B and E), **G14**'s `gh workflow run`.
- `MGPipeDeriveRenderStateFields` has no production caller yet: the applier is only reached from the
  new unit test until package B's tracker emits `create/bind_render_state` and `set_dynamic_state`.
  That is the intended landing order (D12: "an unmodified Magma is already correct under push"), and
  it is why c1 cannot change any retrace pixel on this branch.
- `MEASUREMENTS.md` lines for the measured chunk sizes (396 / 772, per-chunk 264/28/20/272/168/…)
  are the integrator's per C.5 (`docs/Disaggregated/*.md` is integrator-only).
