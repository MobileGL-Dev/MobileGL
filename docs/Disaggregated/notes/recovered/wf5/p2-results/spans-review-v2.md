# Adversarial review — package A continuation `p2/spans` (v2, round 2)

Tree `~/w7/p2-spans` @ `7c2c1456`, six commits on top of the contract `9c6a8a25` (tag `p2/contract`).
Reviewed against `BRIEF-P2.md` §A (G1–G14), §B (D1–D20), §C.0 and §C.5, and against
`p2-results/spans-v2.md`.

**Verdict: NOT APPROVED — 2 majors.**

Everything the package claims about the *chunk table* and the *setter walk* is true and I
re-derived it independently. The majors are both in `a9bb99a4`, the round-2 "Fix" commit, and both
concern the two trip wires it added to `MG_Pipe/PipeApply.cpp`: one of them is a regression that can
`std::abort()` a correct context, and neither of them is reachable from any caller or test in any
build, so nothing in the package could have noticed.

Tree left exactly as found: `git status --porcelain` and `git diff --stat` both empty after every
experiment below (each negative control was restored and rebuilt; see §4).

---

## 1. What I re-ran, and what it actually produced

All in `~/w7/p2-spans`, `CCACHE_BASEDIR=/home/swung/w7`.

| gate / command | observed | verdict |
|---|---|---|
| `cmake --build build-{linux,push,verify} -j 12` | rc 0, rc 0, rc 0 (build-linux relinked `RenderStateSpansTest`, build-verify did 57 steps — the tree was **not** fully up to date at HEAD when I arrived, i.e. the result file's table was produced before the last restore-rebuild) | builds OK, see minor m9 |
| **G1** `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`. Resized = `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 | **holds**; the 4th symbol is D-1, already declared |
| **G1 attribution** same, `~/w7/p2-contract/build-linux/libMobileGL.so` → `build-linux/…` | `0 added, 0 removed, 0 resized, 0 renamed`, `.text +0` | **confirmed** — c1..c4 move the pull build by nothing; all four resizes belong to `c0` |
| **G5** `awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, byte-equal to `~/w7/p2-before-syncrenderstate.sha` | **holds** |
| **G13** `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (rc 1) | **holds** |
| **G13** `python3 scripts/check_include_closure.py` | rc 0, `4 probes, 0 skipped, 0 problem(s)` | **holds** |
| **G13** `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0, `7 negative-control trip(s), positive control OK` | **holds** |
| **G9** `gen_pipe_dirty_surface.py --check` | `error: unrecognized arguments: --check` (rc 2) | package B's, correctly out of scope; `--summary` rc 0 |
| `ctest -L unit` build-linux / build-push / build-verify | rc 0 / 0 / 0, **100% passed of 1498** in each | **holds** |
| `RenderStateSpansTest` push / verify / pull | 5 PASSED / 5 PASSED / 5 SKIPPED; `MOBILEGL_PIPE_VERIFY=1` also 5 PASSED | **holds** (`CMakeLists.txt:469-471` forces `PIPE_PUSH` ON under `PIPE_VERIFY`, so the `CMakeCache` `MOBILEGL_PIPE_PUSH:BOOL=OFF` in build-verify is not what it looks like) |
| `SlotAllocatorTest` push / verify / pull | 6 PASSED / 6 PASSED / 1 passed + 5 skipped | **holds** |
| **G2 shape** `names build-linux` vs `names build-push` (`sed -n 's/^ *Test *#[0-9]*: //p'`) | 2376 vs 2376, `diff` empty | **holds** |
| **G14** baseline `comm -23` tree | **0 removed**, 13 added (5 `RenderStateSpans.*`, 6 `SlotAllocator.*`, 2 placeholders) | **holds** |
| M1's grep blind spot | `ctest -N \| grep -E '^\s+Test #' \| wc -l` → **1377** of 2376 | **confirmed**; `BRIEF-P2.md:36,48,54,755,792` must be corrected as the result file says, and `~/w7/p2-before-ctest-names.txt` is correct and must be kept |

I also re-derived the chunk table by hand from the declarations in `MGPipeValueTypes.h` and every
`BumpVersions()` site in `RenderState.cpp`. **The partition is exact and the split rule is obeyed**:
D0 `[0,264)`, P0 `[264,292)`, D1 `[292,312)`, P1 `[312,584)`, D2 `[584,752)`, P2 `[752,772)`,
D3 `[772,784)`, P3 `[784,800)`, D4 `[800,812)`, P4 `[812,840)`, D5 `[840,868)`, P5 `[868,876)`,
D6 `[876,880)`, P6 `[880,904)`, D7 `[904,1168)` — 396 + 772 = 1168, and every boundary is a member
offset so no byte and no padding byte is unassigned. Setters I re-derived myself against the rule
"pipeline iff a `BumpVersions()` setter writes it": `SetClipControl` (`RenderState.cpp:283-289`,
`++m_version` only → D1 ✓), `SetPrimitiveRestartIndex` (`:189-193`, `++m_version` → D6 ✓),
`SetSampleCoverage` (`:786-792`, `BumpVersions` → P2 ✓, and `MGPipeTypes.h`'s old comment is
correctly fixed), `SetStencilFunc` (`:637-650`, `++m_pipelineStateVersion` conditional on `Func` →
`Func` in P2/P3, `Ref`/`ValueMask` in D3/D4 ✓), `SetScissorBox` (`:926-949`, `++m_version` incl. the
`ScissorBoxWrittenMask` transition → D7 ✓), `SetCapability(ScissorTest)` (`:356-362`, `BumpVersions`
→ `ScissorTestEnabledMask` in P6 ✓), `SetCapability(ClipDistance*)` (`:363-383`, deliberately
`++m_version` → D7 ✓). `generated/PipeSpanTable.inc`'s 44 `kMGPipePipelineStateMembers` names match
that derivation exactly. No disagreement found.

---

## 2. Majors

### MAJOR 1 — `a9bb99a4` made the residual trip wire **strictly weaker**, not "unchanged in strength": it now compares against a field the fill table does not publish for `kDispatch` or `kTextureOp`, and can `abort()` a correct context

`spans-v2.md` §1 (m9) and the commit body of `a9bb99a4` both state:

> `set_residual_value_state`'s trip wire compares the carried bits against the WORKING BLOCK instead
> of `PipeInputs::m_capability`. … **The check is unchanged in strength and now has no ordering
> contract at all.**

Both halves of that sentence are false.

**The code.** `MobileGL/MG_Pipe/PipeApply.cpp:518-521`:

```cpp
const RenderStateParameters& working = MGPipeApplyAccess::RenderState(gPipeInputs);
for (SizeT i = 0; i < kCapabilityCount; ++i) {
    const Bool carried = ((block.CapabilityBits >> i) & 1ull) != 0;
    const Bool assembledBit = MGPipeApplyAccess::DeriveCapability(working, static_cast<CapabilityInput>(i));
```

`MGPipeApplyAccess::RenderState` (`PipeApply.cpp:104`) returns `inputs.m_renderState` **raw**.

**Why it is weaker.** The may-read table decides which fields a verb class publishes:

```
$ grep -n 'GetRenderStateParameters\b' MobileGL/MG_Pipe/FillPoints.def
128:    X(kDraw,        GetRenderStateParameters)
184:    X(kClear,       GetRenderStateParameters)
208:    X(kBlitOrCopy,  GetRenderStateParameters)
257:    X(kReadback,    GetRenderStateParameters)
280:    X(kProgramOp,   GetRenderStateParameters)
$ grep -n 'IsCapabilityEnabled' MobileGL/MG_Pipe/FillPoints.def
135:    X(kDraw,        IsCapabilityEnabled)
136:    X(kDraw,        IsCapabilityEnabledIndexed)
179:    X(kDispatch,    IsCapabilityEnabled)
187:    X(kClear,       IsCapabilityEnabled)
204:    X(kBlitOrCopy,  IsCapabilityEnabled)
246:    X(kTextureOp,   IsCapabilityEnabled)
256:    X(kReadback,    IsCapabilityEnabled)
283:    X(kProgramOp,   IsCapabilityEnabled)
```

`IsCapabilityEnabled` → `m_capability` is published at **7** of the nine classes; `GetRenderStateParameters`
→ `m_renderState` is published at **5**. The commit moved the oracle from the field with the wider
publication set to the field with the narrower one. `kDispatch` and `kTextureOp` are exactly the two
classes it lost.

**A failure the change creates.** `MOBILEGL_PIPE_PUSH` is a per-subsystem bitmask (`D14`), and D5's
skip rule is per-field-per-bit — that is the stated reason the bitmask is "a true per-subsystem A/B"
(`BRIEF-P2.md:182`). Run the residual subsystem on and the render-state subsystem off
(`MOBILEGL_PIPE_PUSH=0x10`) on a verify build, and:

1. `glDrawArrays` — `kDraw` publishes both `m_renderState` and `m_capability` from `GLContext`
   (`MG_Impl/Pipe/PipeFill.cpp:158-161`, `:233`).
2. `glDisable(GL_DITHER)`.
3. `glDispatchCompute` — `kDispatch` publishes `m_capability` (fresh, `Dither=false`) but **not**
   `m_renderState`, which still holds the last draw's block with `DitherEnabled=true`.
4. The tracker sees the capability set move and emits `set_residual_value_state` with `Dither=false`.
5. `PipeApply.cpp:521` asks `DeriveCapability(working, Dither)` → `true`; `PipeApply.cpp:523-526`
   fires `MGLOG_F("… Fatal{PipeResidualDiverged, \"Dither\"} …"); std::abort();`

That is an abort on a perfectly correct context, produced by the commit that claims to have removed
exactly this class of false positive. The pre-`a9bb99a4` form (`m_capability`) is correct at step 5,
because `kDispatch` publishes it.

**The "no ordering contract at all" half is also false.** Grep the writers:

```
$ grep -rn 'm_capability\[i\] =\|MGPipeScatterPipelineBytes(record\|MGPipeScatterDynamicChunks(chunkBytes' MobileGL --include=*.cpp
MobileGL/MG_Pipe/PipeApply.cpp:265   inputs.m_capability[i] = DeriveCapability(...)
MobileGL/MG_Pipe/PipeApply.cpp:383   MGPipeScatterPipelineBytes(record->PipelineBytes.data(), ...RenderState(inputs));
MobileGL/MG_Pipe/PipeApply.cpp:409   MGPipeScatterDynamicChunks(chunkBytes, dyn.ChunkMask, ...RenderState(inputs));
MobileGL/MG_Impl/Pipe/PipeFill.cpp:233  dst.m_capability[i] = ctx.IsCapabilityEnabled(...)
```

Once the render-state subsystem bit is on, `m_renderState` is written **only** by the two scatters at
`:383` / `:409` and `m_capability` **only** by the derivation at `:265` — and the derivation runs from
those same two scatters. The two arrays therefore become valid at exactly the same instant. A residual
block emitted before the first `bind_render_state` of a context finds `m_renderState` as stale as it
would have found `m_capability` (zero-initialised globals: `DitherEnabled=false`, `MultisampleEnabled=false`,
against a fresh context's `true`/`true`). The ordering contract m9 says was removed is still there,
undeclared, on top of the new class contract.

**And the poison cannot see it.** `MGP_INPUT_CHECK` (`MG_Backend/MGPipe/PipeInputs.h:43-52`) is the
mechanism that turns "this verb class does not publish that field" into
`Fatal{UnmigratedPipeInput}`. It lives on the *accessors*. `MGPipeApplyAccess::RenderState` reaches
`inputs.m_renderState` through the friend struct and never runs it. This is the same shape as the P1
`g_fbSlotCache` poison bypass the brief calls out at D13 and asks P2 to close.

**Fix directions** (not applied): revert the oracle to `m_capability`; or keep the working block but
arm the wire only when the applier itself last scattered (`g_applier.BoundRenderStateCso != null`
*and* a per-context "the block is current for this verb" flag); or add `kDispatch`/`kTextureOp` rows
for `GetRenderStateParameters` to `FillPoints.def` (B's file, so it must be declared as a contract).
Whatever is chosen, the claim in `spans-v2.md` must be corrected before the integrator reads it as
settled.

---

### MAJOR 2 — both trip wires `a9bb99a4` added have **no caller and no test in any build**, so neither can go red for the reason it exists

```
$ for f in MGPipeApplySetResidualValueState MGPipeApplySetPatchState MGPipeApplySetPixelPackState \
           MGPipeApplySetVertexAttribDefaults MGPipeApplyDeleteRenderState MGPipeDeriveRenderStateFields; do
    echo "--- $f"; grep -rn "$f" MobileGL --include=*.cpp --include=*.h | grep -v 'MG_Pipe/PipeApply'; done
--- MGPipeApplySetResidualValueState
--- MGPipeApplySetPatchState
--- MGPipeApplySetPixelPackState
--- MGPipeApplySetVertexAttribDefaults
--- MGPipeApplyDeleteRenderState
--- MGPipeDeriveRenderStateFields
(nothing)
```

Three of the applier's eight entry points *are* driven by `RenderStateSpansTest`
(`MGPipeApplyCreateRenderState` at `:532`/`:765`, `MGPipeApplyBindRenderState` at `:538`/`:771`,
`MGPipeApplySetDynamicState` at `:545`/`:795`). The five that are not include **exactly the two the
round-2 commit was written to fix**:

- `MGPipeApplySetPatchState`'s `Fatal{PipePatchCarriersDiffer}` wire (`PipeApply.cpp:424-460`) — m8;
- `MGPipeApplySetResidualValueState`'s `Fatal{PipeResidualDiverged}` wire (`PipeApply.cpp:497-532`) — m9.

`ctest --test-dir build-verify -R 'Residual' -N` lists only `PipeCatalogue.ResidualBlockSizeIsPinned`,
`PipeCatalogue.ResidualBlockIsExactlyItsTwoValueStructsPlusPatchTail` and an unrelated
`DriverBugProbes.*` — i.e. G10's `ctest -R Residual` reaches the `static_assert`s, never the wire.

`ROADMAP.md:7`, quoted at `BRIEF-P2.md:29`: **每个门必须能因它存在的理由变红**. These two gates cannot
change colour at all — in the push build the residual wire compiles to the `MGLOG_E` arm and the patch
wire to nothing, and in the verify build neither function is ever entered. The package demonstrably
*can* drive the applier directly (it does so for three calls, three ways, including the
base-inherit branch), so "this package cannot reach it" is not available as a reason here; the same
fixture that drives `MGPipeApplyBindRenderState` would drive both wires, in both directions, in a
dozen lines.

This is also what let MAJOR 1 land: a case that applied a residual block at a `ScopedPipeVerb(Dispatch)`
after a `ScopedPipeVerb(DrawArrays)` would have aborted on the spot.

`spans-v2.md` §2's verification table lists these two items as delivered ("**m8** … now landed",
"**m9** … The check is unchanged in strength") without recording that neither is executed by any of the
listed commands. §5 "Unfinished" says only that `MGPipeDeriveRenderStateFields*` has no *production*
caller and that "the applier is reached only from the unit tests" — which is true of three entry
points and false of five.

---

## 3. Minors

1. **`GetPixelStoreParameters` is marked emitted but only half of it has a carrier.**
   `Coverage.def:66` → `generated/PipeFilled.inc:357` `MGPipeFieldEmitter::SetPixelPackState`, while
   `PipeInputs.h:654` is `PixelStoreParameters m_pixelStore[2]` (pack **and** unpack) and
   `PipeFill.cpp:158-161` fills both. `MGPipeApplySetPixelPackState` (`PipeApply.cpp:414-416`) writes
   `m_pixelStore[0]` only, by design (D10, `MGPipeTypes.h:490-492`). The moment B applies D5's rule
   ("skip any field whose entry is non-`kNone` and whose subsystem bit is on") the unpack half stops
   being written by anything — and its poison stamp will say it was published, so neither the poison
   nor the verify comparator can see it. Latent today: every backend read is
   `GetPixelStoreParameters(false)` (5 in DirectGLES, 1 in DirectVulkan); the `true` readers are all
   frontend (`MG_Impl/GLImpl/Texture/GL_Texture.cpp`). Cheapest fix is a `kNone` for this accessor, or
   splitting the field. Originates in `c0`, still package A's file.
2. **`MOBILEGL_ASSERT` is DEBUG-only, so `MGPipeApplyCreateRenderState`'s incremental path silently
   mixes two CSOs in the shipped push/verify builds.** `PipeApply.cpp:358-363`: on a dead `BaseCso`,
   `FindCso` returns null, the assert is compiled out (`Defines.h:104-115`, INFO ⇒ no-op), and
   `if (base != nullptr)` leaves `record.PipelineBytes` holding *whatever the recycled slot's previous
   occupant left there*, on top of which the delta chunks are scattered. The brand-new branch is safe
   (`record.PipelineBytes = {}` unconditionally). m8 rejected `MOBILEGL_ASSERT` for the patch wire for
   exactly this reason; the same reasoning was not applied here.
3. **Same DEBUG-only issue in `MGPipeApplySetVertexAttribDefaults`** (`PipeApply.cpp:474-494`): a `Mask`
   bit at or above `PipeInputs::kMaxVertexAttribs` is silently dropped by the loop bound, and the three
   `consumed`/`Location` consistency assertions vanish in an INFO build, so a malformed tail
   desynchronises the attribute writes without a word.
4. **Undeclared deviation from D2: the CSO store is process-global, not per context.** D2
   (`BRIEF-P2.md:103`) says "The applier owns a **per-context** CSO store". The tree has one
   `MGPipeApplierState g_applier{}` (`PipeApply.cpp:319`), and `MGPipeApplierReset()` has no caller
   outside the unit fixtures — so two contexts share one slot space and one `BoundRenderStateCso`
   arming flag, and a context teardown leaves live records. `PipeApply.h:60` documents the intent
   ("Under split there is one per served context") but `spans-v2.md` §4 does not list it among D-1…D-7.
   (`c0` origin, A-owned.)
5. **The patch trip wire carries the same class contract as MAJOR 1, undeclared.**
   `PipeApply.cpp:441-443` reads `working.PatchVertices` / `PatchDefault{Outer,Inner}Level`;
   `FillPoints.def:152-154, 180-182` publish the patch trio for `kDraw` and `kDispatch` only. D-5
   declares an *ordering* contract (bind before `set_patch_state`) but not the class one: a
   `set_patch_state` emitted at a `kClear` / `kTextureOp` / `kReadback` verb compares against bytes the
   class never publishes, while the arming condition `!MGPipeHandleIsNull(g_applier.BoundRenderStateCso)`
   is a global that stays set from the first bind of the process onward.
6. **`SetterConsistency` never touches chunk P3.** `SetStencilOp` is driven on `Back` only
   (`RenderStateSpansTest.cpp:444`) and `SetStencilFunc` on `Front` only (`:429-442`), so face 0's
   three ops and face 1's `Func` — the whole of P3 `[784,800)` — are never written by the setter walk;
   only `ChunkTablePartitionsTheBlock`'s per-face loop (`:260-270`) covers them. Two more lines close
   it. Similarly `SetHint` is driven for one of four targets and `SetStencilMask` for one of two faces.
7. **D-7's kept whole-block entry point is dead.** `MGPipeDeriveRenderStateFields`
   (`PipeApply.cpp:534-539`) has no caller anywhere — not production, not test. It is declared, but the
   integrator should note that D2's sentence "after any scatter the applier calls
   `MGPipeDeriveRenderStateFields`" is now false of the landed code and needs correcting in the doc
   pass alongside D6's printed table (T-2) and the `ARCHITECTURE.md` §5.3 line T-1 asks for.
8. **`kMGPipeRenderStateChunkTableVersion` is a promise, not a gate.**
   `MGPipeRenderStateSpans.h:141` seeds the subset hash with it and the comment says "BUMP IT whenever
   a boundary … moves", but nothing enforces that: my §4 control moved a boundary, changed the two
   byte-count assertions, and built clean with the version still at 1. A `static_assert` keying the
   version off, say, a checksum of the boundary array would make the promise real.
9. **The tree at HEAD was not fully built when the result file's table was written.** My first
   `cmake --build` did real work in `build-linux` (relinked `RenderStateSpansTest`) and 57 steps in
   `build-verify`, from a clean `git status`. The numbers all reproduce, but §2's table was produced
   against a state that no longer matched the checkout; re-run it after the final restore.
10. **D-6's mode changes ride in a code commit.** `git diff 9c6a8a25..HEAD --summary` shows the three
    `100755 => 100644` changes inside `a9bb99a4`, which is the commit `spans-v2.md` §D-6 says it wanted
    to keep free of unrelated churn; `MGPipeRenderStateSpans.cpp` is still `100755`.

---

## 4. Negative controls I ran myself (tree restored, clean)

**(a) G7 proper — demote `ColorMasks` and D3's three bools by moving the P1/D2 boundary from
`ClearColor` to `ColorMasks`**, with the two byte-count assertions updated to 361/807 as a legitimate
table change would:

```
build rc=0
ColorMasks is named as pipeline state but only 0 of its 32 bytes are in the pipeline half …
FramebufferSrgbEnabled is named as pipeline state but only 0 of its 1 bytes are in the pipeline half …
DepthClampEnabled …  TextureCubeMapSeamlessEnabled …
[  FAILED  ] RenderStateSpans.ChunkTablePartitionsTheBlock
SetCapability(DepthClamp): the pipeline-subset hash held but m_pipelineStateVersion MOVED
SetCapability(FramebufferSrgb): …   SetCapability(TextureCubeMapSeamless): …
SetColorMask: …   SetColorMaskIndexed: …   SetColorMaskIndexed(0): …
[  FAILED  ] RenderStateSpans.SetterConsistency
[  PASSED  ] 3 tests.  [  FAILED  ] 2 tests
```

Matches the result file exactly. Restored, rebuilt, 5 PASSED, `git status` clean.

**(b) m3's control C2 — point the viewport derivation guard at `DepthRanges` instead of `Viewports`**
(`PipeApply.cpp:79`):

```
build rc=0
viewport 0
../MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp:103: step 1: viewports only (dynamic chunk D0)
[  FAILED  ] RenderStateSpans.IncrementalChunksKeepEveryDerivedFieldInStep
[  PASSED  ] 4 tests.  [  FAILED  ] 1 test
```

So `IncrementalChunksKeepEveryDerivedFieldInStep` is a genuine oracle for the chunk-scoped derivation,
as claimed, and the four guards' chunk sets are correct — I checked each guarded block's reads against
its `MGPipeRenderStateChunkBitsCovering` constant by hand and found no block that reads a chunk its
guard omits (`kChunksBlendLoop` = {P1}, `kChunksViewportLoop` = {D0}, `kChunksDepthRangeLoop` = {D2},
`kChunksScissorEnableLoop` = {P6}, `kChunksCapabilityWalk` = {P1, P4, P6, D7}).

Restored, rebuilt, 5 PASSED, `git status --porcelain` and `git diff --stat` both empty.

---

## 5. Claims in `spans-v2.md` I checked and found true

- M1 / T-5 withdrawal: correct. The brief's `grep -E '^\s+Test #'` drops 999 of 2376 names; the
  baseline artefact itself is exact (0 in either direction) and must be kept.
- T-1 (`ScissorTestEnabledMask` is a `DynamicTailKey` input **and** pipeline state): correct, and
  `DynamicChunksCoverMagmasDynamicTailKey` asserts it in the direction that makes a later demotion
  loud (`RenderStateSpansTest.cpp:700-705`).
- T-2 (`StencilStates[0].Func` is at the end of P2, not in P3): correct; the file's `static_assert`s
  hold and the brief's own printed P2 row (20 bytes for four members that measure 16) was internally
  inconsistent, so "the file wins" is the right call.
- T-3, T-4, T-6: correct.
- D-1 (the fourth resized symbol is `c0`'s): independently confirmed by the contract-to-HEAD symbol
  report, `0/0/0/0`.
- m1/m2's control (a head-of-array demotion of `BlendStates[0].Enabled` is now caught): the
  `IsWhollyPipeline` assertion at `RenderStateSpansTest.cpp:240` and the index-0 setter cases at
  `:396-399, 409-412, 417-418, 452-453, 460, 477` do make that reachable; I did not re-run this
  control, having reproduced (a) and (b).
- m10: correct and correctly stated — `MOBILEGL_PIPE_VERIFY=1` on the unit binary leaves the
  comparator disarmed, and the header no longer claims otherwise.
- `MOBILEGL_PIPE_POISON` in the two new `#if`s is a real macro, defined at
  `MG_Backend/MGPipe/PipeInputs.h:16-26` and in scope because `PipeApply.cpp:17` includes that header
  before line 424. Not a finding.
- The `CompositeShaderBandIsNeverHandedOut` DEBUG skip keys off
  `MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG`, which is *exactly* the macro
  `MOBILEGL_ASSERT` is gated on (`Defines.h:104`), so D-4's narrowing is sound; the exhaustion arm
  really does run in the INFO builds every gate uses (983 039 handouts, 15 ms).

---

## 6. What would clear this review

1. Fix or revert the residual trip wire's oracle, and correct the m9 claim in `spans-v2.md` (MAJOR 1).
2. Give `MGPipeApplySetResidualValueState` and `MGPipeApplySetPatchState` unit cases that go red for
   the reason each exists — at minimum: a residual block that disagrees on one capability aborts under
   verify, an agreeing one does not, and the same pair for the patch trio including a NaN outer level
   that must compare equal to itself (MAJOR 2). Driving them at a `ScopedPipeVerb(Dispatch)` as well as
   a `DrawArrays` is what pins MAJOR 1 shut.
3. Decide minor 1 (unpack's carrier) before package B lands D5's skip, since after that no gate in the
   tree can see it.
