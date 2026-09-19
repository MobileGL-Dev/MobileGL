# Adversarial review — package `p2-spans` (brief C.0, the `p2/spans` continuation c1/c2/c3)

Tree `~/w7/p2-spans` @ `02b970e9`, three commits on top of the contract tag `p2/contract` (`9c6a8a25`).
Everything below was re-run by me; nothing is taken from `spans-v1.md`. The tree was left exactly as
found (`git status --porcelain` empty, `build-{linux,push,verify}` rebuilt and green at HEAD).

**Verdict: NOT APPROVED — 1 major, 10 minors.** The code in c1/c2/c3 is, as far as I can falsify it,
correct: I re-derived the chunk table from `RenderState.cpp` for eight setters, re-ran G7's negative
control in *both* directions, and could not make a derivation, a chunk boundary or a subset hash
disagree. The major is in the package's **reported verification and its recorded tree/brief
contradiction**, not in its code — but it is consequential enough that it must be fixed before the
integrator acts on `spans-v1.md`.

---

## Scope and what I read

Full diff `9c6a8a25..HEAD` (1105 lines, 3 files, 1005 insertions):

- `MobileGL/MG_Pipe/PipeApply.cpp` (c1 `810850b1`, +154/−9) — read in full, plus the whole file
- `MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp` (c2 `eec92cd2`, +610/−12) — read in full
- `MobileGL/MG_Test/Pipe/SlotAllocatorTest.cpp` (c3 `02b970e9`, +241/−9) — read in full

plus, as context for the claims c1–c3 make: `MGPipeRenderStateSpans.{h,cpp}`, `PipeApply.h`,
`MGPipeValueTypes.h`, `RenderState.{h,cpp}`, `PipeInputs.h`, `SlotAllocator.h`, `Coverage.def`,
`generated/PipeFilled.inc`, `generated/PipeSpanTable.inc`, `MG_Test/ScopedPipeVerb.h`,
`VulkanRenderer.cpp`'s `struct DynamicTailKey`.

---

## MAJOR

### M1 — G2's name-for-name check and G14's "no test name removed" check were run over **1376 of 2375** ctest names, and the resulting §3 T-5 "contradiction" is a misdiagnosis that would have the integrator throw away a correct baseline

`spans-v1.md:98-99` reports G14 as "0 removed, 12 added (1364 → 1376)" and `spans-v1.md:162-169`
(T-5) concludes that `~/w7/p2-before-ctest-names.txt` "does not correspond to any build dir of the
base ref", that "999 are absent from the untouched reference tree too", and that "the integrator
should re-capture the baseline (and say which build dir it comes from) before running G14 for real".

All three statements are false. The 999 names are dropped by the *extraction command*, which is the
one the brief prints at `BRIEF-P2.md:36` (G2), `:48` (G14) and `:755` (D.1's after-every-merge check)
and which the package reused verbatim.

**Reproduction** (in `~/w7/p2-spans`):

```
$ ctest --test-dir build-linux -N | tail -1
Total Tests: 2375
$ ctest --test-dir build-linux -N | grep -cE '^\s+Test #'
1376
$ ctest --test-dir build-linux -N | grep 'AsyncCompileTest' | head -1 | cat -A
  Test  #888: AsyncCompileTest.ManyShadersCompileAndLinkCorrectlyWithAsyncOn$
$ ctest --test-dir build-linux -N | grep -E '^\s+Test #' | head -1 | cat -A
  Test #1000: QueryTest.TransformFeedbackQueryTargetsReadTheirOwnCounter$
```

`ctest` right-aligns the id to the width of the largest id in the listing. With 2375 tests every id
below 1000 is printed as `Test  #NNN:` — **two** spaces — so `grep -E '^\s+Test #'` (one literal
space) silently discards exactly the 2375 − 1376 = **999** tests numbered #1..#999.

With a correct extraction (`sed -n 's/^ *Test *#[0-9]*: //p'`):

```
$ names () { ctest --test-dir "$1" -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort; }
build-linux: 2375   build-push: 2375   build-verify: 3193
pipe/build-linux: 2363   pipe/build-verify: 3181

baseline (2363) vs ~/w7/pipe/build-linux : 0 in baseline-not-in-tree, 0 in tree-not-in-baseline
G14 vs baseline, p2-spans/build-linux    : removed 0, added 12
G2 shape, build-linux vs build-push      : IDENTICAL (2375 == 2375, diff empty)
removed vs pipe/build-verify             : 0
```

So `~/w7/p2-before-ctest-names.txt` is **exactly** `~/w7/pipe/build-linux`'s name list, name for
name, and it must be kept. The 12 added names are the 4 `RenderStateSpans.*`, the 6
`SlotAllocator.*`, and the two contract placeholders `Tracker.*` / `CsoCache.*`.

**Why this is a major and not a minor.**

1. The gate outcome the package reports as verified (G2 shape, G14) was computed on 58 % of the
   data. It happens to be the same answer — I re-established it — but as reported it was not
   established, and this is the "gate that is green because it never ran" shape.
2. `spans-v1.md` T-5 is a *recorded* tree/brief disagreement that does not exist. The brief's
   instruction is "the tree wins for facts", and the integrator is told at `BRIEF-P2.md:755` to run
   the same broken command after every merge. Acting on T-5 means discarding a correct 2363-name
   artefact and, very likely, re-capturing it with the same pattern (yielding 1376 names) — which
   would blind G14 to 999 test names for the rest of P2 and for every remaining package.
3. The fix is cheap and belongs in this package's result file: withdraw T-5, keep the baseline, and
   flag the extraction command in `BRIEF-P2.md:36/48/54/755` for the integrator to correct.

---

## MINORS

### m1 — `ChunkTablePartitionsTheBlock` asserts a named pipeline member is *touched* by the pipeline half, not *covered* by it

`RenderStateSpansTest.cpp:137-140` uses `EXPECT_GT(pipelineBytes, 0)` for a member named in
`kMGPipePipelineStateMembers`. D19 (`BRIEF-P2.md:443`) asks it to assert that
`kMGPipePipelineChunks` "covers every member named". The `EXPECT_TRUE(IsWhollyDynamic(...))`
counterpart in the else branch (`:145`) is the strong form; the pipeline branch is not.

I built a negative control for this (CONTROL C: start pipeline chunk P1 four bytes late, i.e. demote
`BlendStates[0].Enabled` and its padding — the `glEnablei(GL_BLEND, 0)` bit — out of the CSO
identity, leaving `BlendStates` a named pipeline member):

```
build rc=0
[  FAILED  ] RenderStateSpans.ChunkTablePartitionsTheBlock
[  PASSED  ] 3 tests   <- RenderStateSpans.SetterConsistency among them
```

So **the G7 gate proper stays green** with a draw-buffer-0 blend enable outside the CSO key; only the
hardcoded `EXPECT_EQ(kMGPipePipelineChunkBytes, 396)` / `EXPECT_EQ(kMGPipeDynamicChunkBytes, 772)`
at `RenderStateSpansTest.cpp:115-116` turned the suite red, and it did so for an incidental reason
(the totals moved). The moment those two constants are legitimately updated — P3 adds pipeline
members and they will be — a partial demotion of a named member becomes invisible to the whole
suite, and its consequence is one CSO handle serving two different pipeline states, i.e. silent
wrong pixels. One-line fix: `EXPECT_TRUE(IsWhollyPipeline(member.Offset, member.Size))` for every
named member except `StencilStates`, which the loop at `:157-167` already handles by hand.

### m2 — every indexed setter is driven at exactly one index, and never at index 0

`SetViewportIndexed`@3 (`:200`), `SetCapabilityIndexed(Blend)`@3 (`:278`),
`SetCapabilityIndexed(ScissorTest)`@5 (`:280`), `SetBlendFuncIndexed`@2 (`:288`),
`SetBlendEquationIndexed`@4 (`:294`), `SetColorMaskIndexed`@6 (`:327`), `SetDepthRangeIndexed`@9
(`:333`), `SetScissorBoxIndexed`@11 (`:349`). Index 0 is the element both backends actually consume
(`IsCapabilityEnabled(Blend)` → `BlendStates[0].Enabled`, `GetScissorBox`/`GetViewport` → `[0]`) and
it is the element a chunk boundary landing at the head of an array demotes first — exactly the case
CONTROL C shows the gate missing. Driving each indexed setter at index 0 as well is a handful of
lines and closes m1's blind spot from the setter side.

### m3 — the derivation is unconditional and whole-block, and it sits on the per-draw path

`MGPipeDeriveRenderStateFields` is called from `MGPipeApplyBindRenderState` (`PipeApply.cpp:317`)
**and** from `MGPipeApplySetDynamicState` (`:338`), and it always recomputes all 29 fields
(`:129-198`): 8 × 8 blend/colour-mask/indexed-blend stores, 16 × 3 viewport/depth-range/
indexed-scissor stores, ~20 scalars, two `FloatVec4`/`StencilFaceState` copies, and a 35-arm switch
dispatched 35 times. A per-frame `glViewport` — the D8 case whose whole point is that it sends
dynamic chunk D0 alone (`BRIEF-P2.md:242`) — now pays the entire derivation. P1 pulled 29 values;
P2 writes roughly 170 and dispatches 35 switch arms to produce the same 29.

Nothing in this package measures that, and the gate it threatens is G11 / D.4.3's pinned
**T1 ≤ 45 ns/draw** (`BRIEF-P2.md:895`). Scoping the derivation by the chunk mask that was actually
scattered — derive only the fields whose backing chunk moved — is not precluded by D2
(`BRIEF-P2.md:105`, "after any scatter the applier calls …") and should be evaluated before device
time is spent, not after. Recording it here because c1 is the commit that introduces the cost and
`spans-v1.md` does not mention it.

### m4 — the round-trip D2's whole argument rests on has no assertion anywhere

D2 (`BRIEF-P2.md:86`) is "the block they read *is* the assembled block", and that is what lets
Espryt's `SyncRenderState` stay untouched. `DerivationMatchesTheFrontendGetters` compares the 29
derived `PipeInputs` fields but never asserts
`memcmp(&gPipeInputs.GetRenderStateParameters(), &ctx.GetRenderStateParameters(), sizeof(RenderStateParameters)) == 0`
after `applyWholeBlock()`. That one line is the only thing that would cover the ~25 members of the
block with **no** derived `PipeInputs` field at all — `SampleCoverageValue`, `SampleCoverageInvert`,
`SampleMaskValue`, `PolygonModeBack`, `PointSize`, `PointFadeThresholdSize`,
`PointSpriteCoordOrigin`, the four hints, `ClipOrigin`, `ClipDepthMode`, `PolygonOffsetClamp`,
`FrontFaceModeSetting`, `ScissorBoxes[1..15]`, `ScissorBoxWrittenMask`, `ClipDistanceEnabledMask`,
`DepthTestEnabled`, `CullFaceEnabled` and the 20 capability bools' raw storage — which is precisely
the set Espryt reads raw through its span memcmp. `Gather`/`Scatter` are exact inverses by
construction (`MGPipeRenderStateSpans.cpp:78-100`, same chunk iteration, same order), so this is a
coverage hole rather than a live bug, but it is the cheapest high-value assertion in the file.

### m5 — the incremental `create_render_state` path has no caller in any test

`applyWholeBlock` (`RenderStateSpansTest.cpp:393-418`) always passes `BaseCso = kMGPipeNullHandle`
and all-ones masks. So `MGPipeApplyCreateRenderState`'s base-inherit + partial-mask branch
(`PipeApply.cpp:287-300`) is never entered, and `MGPipeScatterPipelineChunks`,
`MGPipePipelineChunkBlobBytes`, `MGPipeDynamicChunksThatMoved`, `MGPipePipelineChunksThatMoved` and
`MGPipeHashPipelineBytes` have **no caller anywhere in the tree** outside `MGPipeRenderStateSpans.cpp`
itself (verified by grep over `MobileGL/**`). D7 step 2's miss path (`BRIEF-P2.md:234`) emits exactly
the mask ≠ all-ones shape, and D8's chunk-level suppressor is `MGPipeDynamicChunksThatMoved`. Package
B will be the first caller; A owns the code and could pin it now with a few lines.

### m6 — `SlotAllocator.LifetimeIdSurvivesARecycledAddress` cannot distinguish what its name claims

`MGPipeSlotAllocator::Acquire` takes a `lifetimeId`, never an address (`SlotAllocator.h:55`). Each
loop iteration frees its handle at `SlotAllocatorTest.cpp:213`, so the next `Acquire` reuses slot 1
with `gen + 1` and the handle differs whether or not the heap repeated the address. The ABA
assertion at `:206` is therefore a restatement of `GenMovesOnlyOnSlotReuse`; the real ABA is only
reproduced end to end by package E's `HandleRecycleScenario`. Separately, the case can silently
`GTEST_SKIP` when the allocator never repeats an address (`:219-222`) — it did not skip here
(`Passed` in both build-push and build-verify), but that is machine-dependent and the case name
promises more than it delivers on a machine where it does skip.

### m7 — `CompositeShaderBandIsNeverHandedOut`'s DEBUG skip is broader than its reason

`SlotAllocatorTest.cpp:230-235` (declared as D-4 in `spans-v1.md:203-207`) skips the whole case in a
DEBUG build, including the two arms that do not trip the exhaustion assert: the eight low-slot
handouts (`:239-242`) and the "every other kind is unaffected" arm (`:259-263`). Only the
`while (HighWater < kMGPipeShaderCsoCompositeSlotBase)` walk needs the skip.

### m8 — `MGPipeApplySetPatchState` does not assert the two patch carriers agree (undeclared)

D6 (`BRIEF-P2.md:196`) — "the applier asserts under verify that the two carriers agree. Redundancy
here is a trip wire, not waste" — and D10 (`:270`) — "asserts they match what the pipeline chunk P0
delivered" — both require it. `PipeApply.cpp:345-354` writes the trio and asserts nothing, so a
stale `set_patch_state` silently clobbers what `bind_render_state` scattered from chunk P0. This is
`c0` code and so belongs to the contract reviewer, but C.5 gives `PipeApply.*` to package A and
`spans-v1.md` §4 does not declare it; recorded so it is not lost between the two reviews.

### m9 — the residual trip wire compares against storage only the derivation writes (undeclared, `c0`)

`MGPipeApplySetResidualValueState` (`PipeApply.cpp:394-406`) compares `block.CapabilityBits` against
`PipeInputs::m_capability`, which is written **only** by `DeriveRenderStateFields`. Emitted before
the first `bind_render_state`/`set_dynamic_state` of a context — which is what D9's "the block is
emitted once per context" (`BRIEF-P2.md:263`) describes — it compares the carried bits against
never-derived storage (all false, while `Dither` and `Multisample` default to true) and aborts under
poison/verify. Again `c0`, but it is an ordering contract package B has to honour and nothing in the
tree states it.

### m10 — the "verify comparator is the guard" claim is not exercised by anything this package runs

c1's commit message and `PipeApply.cpp:61-66` both rest the D5 transcription risk on
`MOBILEGL_PIPE_VERIFY`'s compare-at-read. In a unit-test process the comparator never arms:
`ArmVerify` reads `MG_Config::Features.PipeVerify` (`PipeFill.cpp:374-380`), which a process that
never runs the config loader leaves false. Verified:

```
$ MOBILEGL_PIPE_VERIFY=1 ./build-verify/MobileGL/MG_Test/Pipe/RenderStateSpansTest
[  PASSED  ] 4 tests.
```

— all four pass, including the deliberately divergent reads at `RenderStateSpansTest.cpp:477`,
`:553` and `:567`, which a live comparator would report as `Fatal{PipeVerifyDiffer, …, where=read}`.
Harmless (and it is why those cases can be written at all), but the claim must not be read as
evidence: the derivation's oracle is G3/G4 on the retrace and integration-verify lanes, which this
package cannot reach.

---

## What I re-ran and confirmed (so the integrator need not repeat it)

| check | command | result |
|---|---|---|
| builds | `cmake --build build-{linux,push,verify} -j 12` | rc 0, "no work to do" — binaries match HEAD |
| G1 | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | **0 added / 0 removed / 0 renamed**, 4 resized, `.text` +160 |
| G1 attribution (D-1) | same, `~/w7/p2-contract/build-linux/libMobileGL.so` → `build-linux/…` | **0/0/0/0** — c1+c2+c3 move the pull build by nothing. The 4th resize (`_GLOBAL__sub_I_DirectGLES.cpp`, −9) is c0's: base→contract already shows all four. D-1's attribution is correct. |
| G5 | `awk '/namespace RenderStateImpl {/,/} \/\/ namespace RenderStateImpl/' … \| sha256sum` | `d8fd1c48…0efe27`, **equal** to `~/w7/p2-before-syncrenderstate.sha` |
| G13 | `grep -rc pGLContext MobileGL/MG_Backend \| grep -v ':0$'` | empty; no stdio under `MG_Backend`/`MG_State`; no `printf`/`clock_gettime`/`chrono`/`MGLOG_I` added anywhere in the c1..c3 diff |
| generators | `gen_pipe.py --check` / `--self-test` | rc 0; 71 calls / 63 fields (34 emitted) / 69 verbs / 9 classes / 0 UNMAPPED; 7 negative-control trips |
| include closure | `check_include_closure.py` | rc 0 — 4 probes, 0 forbidden; wire-header closure 2 headers |
| unit | `ctest -L unit --no-tests=error -j 8` in all three build dirs | **1497/1497 passed** in each |
| the ten new cases | per build dir | push/verify: 4/4 `RenderStateSpans.*` + 6/6 `SlotAllocator.*` PASS. pull: 4 `RenderStateSpans.*` SKIP, 5 `SlotAllocator.*` SKIP + `ReservedHandles…` PASS. Same ten names in all three. |
| **G7 negative control, direction A** (demote `ColorMasks` + the three new bools to dynamic) | patched `MGPipeRenderStateSpans.h`, rebuilt, ran | build rc 0, suite rc **1**; `SetColorMask` / `SetColorMaskIndexed` / `SetCapability(DepthClamp\|FramebufferSrgb\|TextureCubeMapSeamless)`: "the pipeline-subset hash held but m_pipelineStateVersion MOVED"; `SetterConsistency` + `ChunkTablePartitionsTheBlock` FAILED |
| **G7 negative control, direction B** (promote `ClampReadColor` to pipeline — the direction the package did *not* test) | same shape | build rc 0, suite rc **1**; "`SetClampReadColor`: the pipeline-subset hash MOVED but m_pipelineStateVersion held" and "ClampReadColor is not named as pipeline state but 4 of its bytes are in the pipeline half" |
| restore after each control | `cp` back, rebuild, rerun, `git status --porcelain` | rc 0, suite green, **tree clean** |

**Chunk table, re-derived by hand from `RenderState.cpp` (eight setters, not five):**

| setter | `RenderState.cpp` | version behaviour | member(s) written | chunk | verdict |
|---|---|---|---|---|---|
| `SetSampleCoverage` | :786 | `BumpVersions()` | `SampleCoverageValue`, `SampleCoverageInvert` | P2 | pipeline ✓ |
| `SetClipControl` | :283 | `++m_version` only | `ClipOrigin`, `ClipDepthMode` | D1 | dynamic ✓ |
| `SetStencilFunc` | :637 | `++m_version` always; `++m_pipelineStateVersion` only when `Func` moves (:644) | `Func` / `Ref` / `ValueMask` | P2 / D3 / D3 | ✓ — and the test drives both halves (`:305-318`) |
| `SetScissorBox` | :926 | `++m_version` (incl. the `ScissorBoxWrittenMask` transition, :944) | `ScissorBoxes`, `ScissorBoxWrittenMask` | D7 | dynamic ✓ |
| `SetCapability(ClipDistance0..7)` | :360-380 | `++m_version` only | `ClipDistanceEnabledMask` | D7 | dynamic ✓ |
| `SetCapabilityIndexed(ScissorTest)` | :442 | `BumpVersions()` | `ScissorTestEnabledMask` | P6 | pipeline ✓ |
| `SetMinSampleShadingValue` | :813 | `BumpVersions()` | `MinSampleShadingValue` | P2 | pipeline ✓ |
| `SetPointSize` | :199 | `++m_version` only | `PointSize` | D0 | dynamic ✓ |

I then classified **all 46** public setters mechanically (`BumpVersions` vs `++m_version` per
function body) against the 15-chunk table, and found no member in a pipeline chunk written only by a
`++m_version` setter, nor any member in a dynamic chunk written by a `BumpVersions()` setter. The
partition is exact (`static_assert`s at `MGPipeRenderStateSpans.h:144-174`, boundaries ascend and
sum to `sizeof(RenderStateParameters)`), and all 46 setters declared in `RenderState.h` are driven
by `SetterConsistency`.

**Other things I tried to break and could not:**

- The 29 derivations against their getters, line by line: `GetViewport`'s `std::lround`
  (`RenderState.cpp:81-88` vs `PipeApply.cpp:159-162`), the 35-arm `IsCapabilityEnabled`
  (`:392-437` vs `:74-127`, including `Blend → BlendStates[0].Enabled`,
  `ScissorTest → mask & 1` and the `ClipDistance0..7` run), `GetStencilState`'s face index
  (`GetStencilFaceIndex` Front=0/Back=1, `StencilFace` enum `MGPipeValueTypes.h:114-119`),
  `GetScissorBox` → `ScissorBoxes[0]` unrounded, the indexed loops' bounds
  (`kMGMaxDrawBuffers` = 8 for blend/colour mask, `PipeInputs::kMaxViewports` = 16 =
  `RenderStateParameters::MAX_VIEWPORTS`, `PipeInputs::kCapabilityCount` ==
  `CapabilityInput::CapabilityInputCount`). All faithful.
- `kCapabilityNames` (`PipeApply.cpp:205-241`) against the `CapabilityInput` enum
  (`MGPipeValueTypes.h:168-206`) — the 35 names are in enum order, so the residual `Fatal` line names
  the right capability. The `static_assert` at `:243` only pins the count, but the order is in fact
  right today.
- The derived set vs `MGP_COVERAGE_EMITTED_LIST` (`Coverage.def`) and `kMGPipeFieldEmittedBy[]`
  (`generated/PipeFilled.inc:326-385`): 34 fields carry a P2 emitter; 5 are non-derived carriers
  (`GetCurrentVertexAttribute`, `GetPixelStoreParameters`, `GetRenderStateParameters`,
  `GetPipelineStateVersion`, `GetRenderStateParametersVersion`); the remaining 29 are exactly what
  `DeriveRenderStateFields` writes. No emitted field is left without a carrier.
- `DynamicChunksCoverMagmasDynamicTailKey`'s inventory against the real `struct DynamicTailKey` in
  `VulkanRenderer.cpp`: `viewport[4]`, `depthRange[2]`, `blendColor[4]`, `polygonOffsetFactor`,
  `polygonOffsetUnits`, `lineWidth`, `stencilValueMask[2]`, `stencilWriteMask[2]`, `stencilRef[2]`,
  `scissorBox[4]`, `scissorEnabled` + the four backend facts. The test's 13 rows are complete, and
  T-1 (`scissorEnabled` reads pipeline-half `ScissorTestEnabledMask`) is a correct observation,
  correctly asserted the other way round (`RenderStateSpansTest.cpp:623-628`).
- `DerivationMatchesTheFrontendGetters`' vacuity structure: `ScopedPipeVerb` really does run
  `MGPipeFillForVerb` (`MG_Test/ScopedPipeVerb.h:47-52`), the fill precedes every mutation, and the
  mutated values are chosen so an order swap is caught (blend func index 2 gets four *distinct*
  factors, viewport 0 is `(1.5, 2.5, 63.5, 32.25)` → `(2, 3, 64, 32)`). T-3's three-phase split
  (kDraw / kClear / kReadback) is the right answer to the `FillPoints.def` classes and does not
  weaken the poison.
- `SetterConsistency`'s vacuity guard (`EXPECT_NE(versionBefore, rs.GetVersion())`, `:190`) fires on
  every case, so no case can pass by writing the value already stored — I confirmed this by the two
  negative controls, in which the version expectations still held while the hash expectations broke.

---

## Declared deviations — my assessment

| id | claim | verdict |
|---|---|---|
| D-1 | the 4th resized pull symbol (`_GLOBAL__sub_I_DirectGLES.cpp`, −9) belongs to c0 | **confirmed independently** (contract → spans is 0/0/0/0) |
| D-2 | c2/c3 delete the contract's placeholder cases | **confirmed harmless** — neither name is in the 2363-name baseline; G14 shows 0 removed |
| D-3 | `scripts/g7_negative_control.sh` not created (E owns it) | **correct per C.5**; the manual recipe works — I re-ran it and a second, opposite-direction control |
| D-4 | `CompositeShaderBandIsNeverHandedOut` skips in DEBUG | **accepted**, see m7 for the narrowing |
| T-1 | `ScissorTestEnabledMask` is a `DynamicTailKey` input *and* pipeline state | **correct**, and correctly asserted |
| T-2 | the landed chunk table differs from D6's printed table (`StencilStates[0].Func` in P2, not P3) | **correct**, and the brief says the file wins |
| T-3 | three of D5's "kDraw" fields are kClear/kReadback | **correct** (`FillPoints.def`), well handled |
| T-4 | D5's field names are not the tree's | **correct**, cosmetic |
| **T-5** | the ctest-name baseline is wrong and should be re-captured | **FALSE — see M1.** The baseline is exactly right; the extraction command is wrong. |
| T-6 | `p2/contract` is ambiguous (branch + tag) for `git worktree add` | plausible and worth passing on; not re-tested |

---

## What must happen before this package can be approved

1. **M1**: withdraw T-5 from `spans-v1.md`, keep `~/w7/p2-before-ctest-names.txt`, re-report G2's
   shape and G14 over the full 2375/3193 name lists with a correct extraction, and flag
   `BRIEF-P2.md:36 / :48 / :54 / :755` so the integrator's after-every-merge check and the other four
   packages do not inherit the 999-name blind spot.

The minors are all cheap; m1 and m2 together (one `EXPECT_TRUE(IsWhollyPipeline(...))` and eight
extra index-0 cases) close the one place where I could get the G7 gate to stay green on a real
CSO-identity defect, and m3 is the risk the day-43 number is most exposed to.
