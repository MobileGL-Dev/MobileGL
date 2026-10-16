# Adversarial review of `p2/spans` — round 3 (`~/w7/p2-spans` @ `842af233`)

Verdict: **approved — 0 majors, 11 minors.**

Scope reviewed: the full diff `refs/tags/p2/contract (9c6a8a25)..842af233`, per commit, i.e. the ten
commits of `c1`/`c2`/`c3` plus the three rework rounds. The contract commit `c0` itself is another
package's; where a finding lands in `c0` it is marked so.

Tree state on entry and on exit: `git status --porcelain` empty at `842af233`; all three build
directories report `ninja: no work to do`. The one experiment that patched a file (§3) reverted with
`git checkout --` and rebuilt; the tree was re-confirmed clean and 11/11 green afterwards.

---

## 1. What I re-ran independently (command → observed)

All in `~/w7/p2-spans`, `CCACHE_BASEDIR=/home/swung/w7`. Raw logs: `~/w7/rev-spans-gates.txt`,
`~/w7/rev-spans-nc.txt`, `~/w7/rev-spans-nc-ctest.txt`.

| gate | command | observed |
|---|---|---|
| build | `cmake --build build-{linux,push,verify} -j 12` | `ninja: no work to do` ×3, rc 0 — §2 of the result file was produced against this checkout |
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160`, `.data/.bss/.rodata +0`. Resized: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9. See minor 3. |
| **G5** | `awk '/^    namespace RenderStateImpl \{/,/^    \} \/\/ namespace RenderStateImpl/' MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27`, byte-equal to `~/w7/p2-before-syncrenderstate.sha` ✅ |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (rc 1) ✅ |
| **G13** | `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` rc 0 ✅ |
| **G13** | `gen_pipe.py --check` / `--self-test` | `generated files are up to date` rc 0; `7 negative-control trip(s), positive control OK` rc 0 ✅. `--check` prints `63 PipeInputs fields (7 sticky, 33 emitted by a P2 call)` — the 34→33 of minor 1 of the previous round is real and regenerated. |
| unit | `ctest --test-dir <d> -L unit --no-tests=error -j 12` | **100% passed, 0 failed, out of 1504** in `build-linux`, `build-push` and `build-verify` ✅ (the result file's number confirmed) |
| **G6** | `ctest --test-dir build-push -R 'RenderStateSpans\.'` | 11/11 pass; `build-verify` 11/11; `build-linux` 11/11 as visible SKIPs |
| c3 | `ctest -R 'SlotAllocator\.'` ×3 | 6/6, 6/6, 6/6 |
| **G10 (unit half)** | `ctest --test-dir build-verify -R 'Residual' --no-tests=error` | 6/6 pass, and 3 of the 6 are the wire itself (`ResidualTripWire*`) — `-R Residual` no longer reaches only `static_assert` catalogue cases ✅ |
| **G2 (shape)** | names of `build-linux` vs `build-push` | 2382 vs 2382, `diff` empty ✅ |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <names>` | **0 removed**; 19 added (11 `RenderStateSpans.*`, 6 `SlotAllocator.*`, `Tracker`/`CsoCache` placeholders) ✅ |
| ownership | `git diff --name-status p2/contract..HEAD` | 9 files, all A-owned per §C.5; `git diff --name-only … -- tools/trace_replay/fixtures` = 0 ✅ |

Which build takes which trip-wire arm — checked, because D-10's claim depends on it:
`compile_commands.json` gives `build-push` = `MOBILEGL_PIPE_PUSH=1` with `MOBILEGL_PIPE_VERIFY`
absent, and `PipeInputs.h:20-26` derives `MOBILEGL_PIPE_POISON=0` at INFO with
`MOBILEGL_BUILD_DISAGGREGATED=OFF`. So **`build-push` exercises the counted-and-logged arm and
`build-verify` the fork/`SIGABRT`/`Fatal{…}` arm** — both halves of `MGP_TRIP_WIRE_REPORT`
(`PipeApply.cpp:30-40`) actually run under the gate set. The claim is not self-certified.

---

## 2. I re-derived the chunk table and the split rule myself

The claim under test is `MGPipeRenderStateSpans.h:20-22`: *a byte is pipeline iff some public
`RenderState` setter that calls `BumpVersions()` writes it.*

Boundaries recomputed from `MGPipeValueTypes.h`'s declaration order and alignment, against
`MGPipeRenderStateSpans.h:58-98`:

| chunk | range | size | verdict, from `RenderState.cpp` |
|---|---|---|---|
| D0 `[0,264)` | Viewports[16], LineWidth, PointSize | 264 | `SetViewport(:78)`, `SetViewportIndexed(:98)`, `SetLineWidth(:113)`, `SetPointSize(:203)` — all `++m_version` ✔ |
| P0 `[264,292)` | PatchVertices, Outer, Inner | 28 | `:214`, `:233`, `:244` — all `BumpVersions()` ✔ |
| D1 `[292,312)` | PolygonOffset{Factor,Units,Clamp}, ClipOrigin, ClipDepthMode | 20 | `SetPolygonOffsetClamped(:276)`, `SetClipControl(:288)` — `++m_version` ✔ |
| P1 `[312,584)` | BlendStates[8], LogicOp, DepthTestEnabled, DepthFunc, DepthMask, ColorMasks[8], the three D3 bools | 272 | `SetBlendFunc/Equation(+Indexed)`, `SetLogicOp`, `SetCapability(DepthTest)`, `SetDepthFunc/Mask`, `SetColorMask(+Indexed)` — all `BumpVersions()` ✔ |
| D2 `[584,752)` | ClearColor/Depth/Stencil, BlendColor, DepthRanges[16] | 168 | `:711,:722,:733,:744,:760,:775` — `++m_version` ✔ |
| P2 `[752,784)` | SampleCoverageValue/Invert, SampleMaskValue, MinSampleShadingValue, **StencilStates[0].Func** | 20+12? → **20** | `SetSampleCoverage(:791)`, `SetSampleMaskValue(:806)`, `SetMinSampleShadingValue(:820)`, `SetStencilFunc`'s conditional `++m_pipelineStateVersion(:649)` ✔ |
| D3 `[772,784)` | face 0 Ref/ValueMask/WriteMask | 12 | `SetStencilFunc(:648)` / `SetStencilMask(:657)` — `++m_version` only ✔ |
| P3 `[784,800)` | face 0 the three ops + face 1 Func | 16 | `SetStencilOp(:671)` `BumpVersions()`, face-1 Func as above ✔ |
| D4 `[800,812)` | face 1 Ref/ValueMask/WriteMask | 12 | ✔ |
| P4 `[812,840)` | face 1 ops, CullFaceEnabled, CullFaceMode, FrontFaceMode, ProvokingVertexMode | 28 | `SetCullFaceMode(:894)`, `SetFrontFaceMode(:905)`, `SetProvokingVertexMode(:916)`, `SetCapability(CullFace)` — `BumpVersions()` ✔ |
| D5 `[840,868)` | 4 hints, PointFadeThresholdSize, PointSpriteCoordOrigin, ClampReadColor | 28 | `SetHint(:131)`, `:147`, `:157`, `:167` — `++m_version` ✔ |
| P5 `[868,876)` | PolygonModeFront/Back | 8 | `SetPolygonMode(:178)` `BumpVersions()` ✔ |
| D6 `[876,880)` | PrimitiveRestartIndex | 4 | `:192` `++m_version` ✔ |
| P6 `[880,904)` | the 20 capability bools + ScissorTestEnabledMask | 24 | every `SET_CAPABILITY` arm `:313`, `SetCapability(ScissorTest) :360`, `SetCapabilityIndexed(ScissorTest) :460` — `BumpVersions()` ✔ |
| D7 `[904,1168)` | ScissorBoxes[16], ScissorBoxWrittenMask, ClipDistanceEnabledMask | 264 | `SetScissorBox(:948)`, `SetScissorBoxIndexed(:969)`, the ClipDistance arm's deliberate `++m_version` (`:377-381`) ✔ |

Sums: 396 pipeline + 772 dynamic = 1168 = `sizeof(RenderStateParameters)`. **The partition is exact,
sorted and complete, and I found no byte on the wrong side.** The subset hash therefore moves iff
`m_pipelineStateVersion` moves for all 47 public setters; the walk in `RenderStateSpansTest.cpp:349-556`
drives every one of them (I diffed it against `RenderState.h:30-153` — nothing is missing), each with
a vacuity guard (`:340`) so no case can pass by writing the value already stored.

Corollary I confirm independently: **T-2 is right and the brief's D6 table is wrong** — chunk P2 ends
at `StencilStates[0].Ref`, so it contains `StencilStates[0].Func`; the brief's own P2 row claims 20
bytes for four members measuring 16. **T-1 is right** — `ScissorTestEnabledMask` is a `DynamicTailKey`
input *and* pipeline state, and `RenderStateSpansTest.cpp:765-770` asserts the direction that makes a
later demotion loud rather than silent.

---

## 3. Independent negative control: the G6/G7 gate does go red for its own reason

The result file's control moves the P1/D2 boundary (demotes `ColorMasks`). I ran a **different** one,
so the answer is not the implementer's: demote `ScissorTestEnabledMask` out of P6 into D7 by moving
the last boundary from `offsetof(…, ScissorBoxes)` to `offsetof(…, ScissorTestEnabledMask)`
(`MGPipeRenderStateSpans.h:96`), with the two byte-count `static_assert`s (`:190-191`) adjusted to
392/776 so the control still **compiles**, as D19 requires.

```
build rc=0
ctest --test-dir build-push -R 'RenderStateSpans\.' --output-on-failure    → rc 8
 1/11 RenderStateSpans.ChunkTablePartitionsTheBlock ............ ***Failed
 2/11 RenderStateSpans.SetterConsistency ....................... ***Failed
 4/11 RenderStateSpans.DynamicChunksCoverMagmasDynamicTailKey .. ***Failed
73% tests passed, 3 tests failed out of 11
```
with, in the log:
```
RenderStateSpansTest.cpp:287: ScissorTestEnabledMask is named as pipeline state but only 0 of its 4
  bytes are in the pipeline half - the rest have silently left the CSO's identity
RenderStateSpansTest.cpp:342: SetCapability(ScissorTest): the pipeline-subset hash held but
  m_pipelineStateVersion MOVED
RenderStateSpansTest.cpp:342: SetCapabilityIndexed(ScissorTest, 5): … MOVED
RenderStateSpansTest.cpp:342: SetCapabilityIndexed(ScissorTest, 0): … MOVED
RenderStateSpansTest.cpp:766: ScissorTestEnabledMask moved into the dynamic half; …
```
`DerivationMatchesTheFrontendGetters` and `IncrementalChunksKeepEveryDerivedFieldInStep` correctly
stayed **green** (the block is still fully carried, only its halves moved), which is the discrimination
you want. Reverted, rebuilt, 11/11 green, `git status` empty.

So G6/G7's test is not a test that cannot fail, it names the setter, and the membership half
(`IsWhollyPipeline`, added in `ad1238bd`) catches the partial-demotion case the hash cannot see.

---

## 4. The other things I hunted for, and what I found

- **A subset hash that disagrees with the pipeline version for some setter** — none; §2 re-derives all
  47. `MGPipeComputePipelineSubsetHash` (`MGPipeRenderStateSpans.cpp:188-195`) gathers exactly the
  seven pipeline chunks in ascending order and `MGPipeHashPipelineBytes` hashes the same bytes; their
  agreement is asserted at `RenderStateSpansTest.cpp:850`.
- **State that silently changed meaning in the CSO or the dynamic payload** — none. The strongest check
  is `ExpectAssembledBlockIsTheLiveBlock` (`RenderStateSpansTest.cpp:217-224`): a `memcmp` of the whole
  1168-byte assembled block against the live one, driven after the whole-block *and* after every
  incremental step. That is the only cover for the ~25 members with no derived field (the ones Espryt's
  span `memcmp` reads raw), and it is present.
- **A memo that can serve a stale answer** — the applier holds no derived-value memo. The one scoping
  risk is `MGPipeDeriveRenderStateFieldsForChunks`' four guards. I checked each guard's chunk set
  against the members its block reads (`PipeApply.cpp:96-107`): blend loop = chunks(BlendStates|ColorMasks)
  = P1; viewport = D0; depth range = D2; scissor-enable = P6; capability walk = P1|P4|P6|D7 (the 20
  bools in P6, `DepthTestEnabled` and the three new bools in P1, `CullFaceEnabled` in P4, `BlendStates`
  in P1, `ScissorTestEnabledMask` in P6, `ClipDistanceEnabledMask` in D7 — complete because
  `MGP_PLAIN_CAPABILITY_LIST` includes `DepthTest` and `CullFace`). Every guard is computed from the
  boundary table, so a boundary move recomputes it. `IncrementalChunksKeepEveryDerivedFieldInStep`
  drives all 35 capabilities one at a time through the scoped path, which is the case that would catch
  a too-narrow guard.
- **A field marked emitted that nothing writes** — I walked all 33 rows of `MGP_COVERAGE_EMITTED_LIST`
  against `DeriveRenderStateFields`/the seven apply entry points. Every one has a writer. This is the
  check that makes minor 1 of the previous round (`GetPixelStoreParameters`) the right call rather than
  a cosmetic one: with the row present, the *unpack* half would have had no writer at all.
- **A dirty bit never set or never cleared** — the applier's only latched state is
  `ScatteredChunkBits` (`PipeApply.h:77`), set by bind and set_dynamic_state, cleared only by
  `MGPipeApplierReset()`. That is correct for one context and is the substance of minor 8.
- **A handle re-key that breaks on deletion/reuse/share group** — `SlotAllocatorTest.cpp:57-246` covers
  first handout = gen 0, no bump on respecify, bump on the *next handout* rather than on the free
  (so a double free cannot skip a generation), a stale handle failing `IsLive` and failing to free its
  successor's slot, kind independence, density, slot 0, and — the arm `7c2c1456` added — the strictly
  stronger deterministic ABA: re-acquiring the **same lifetime id** after a free cannot reproduce the
  handle. Share groups are safe by construction: `MGPipeSlots()` is keyed on the process-unique
  lifetime id, so a shared object keeps one handle across contexts.
- **A gate green because it never ran** — the previous round's MAJOR 2. All eight apply entry points now
  have a caller (`ce370a3e`), and I confirmed `ctest -R Residual` in `build-verify` reaches the three
  wire cases rather than only the `static_assert` catalogue. Both wires are driven disarmed /
  armed-agreeing / armed-diverging, and both `MGP_TRIP_WIRE_REPORT` arms are compiled in the gate set
  (§1).
- **Hot-path instrumentation the brief forbids** — none committed. Both wires sit on
  `set_patch_state` (24-byte `memcmp`, emitted about once per program) and `set_residual_value_state`;
  D9 already specifies the residual comparison for all builds ("counted and logged otherwise"), so the
  shipped-build arm is sanctioned rather than a deviation. No timer anywhere in `MGPipeValidateForVerb`'s
  reach; nothing new on the per-draw path — the round-2 scoping (`a9bb99a4`) *removed* ~170 stores and
  35 switch dispatches from it.
- **Undeclared deviation from D1-D20** — I found none. D-1 … D-12 in the result file cover every
  departure I could identify, including the two contract-file edits after the tag (`Coverage.def`,
  `MG_Test/Pipe/CMakeLists.txt`).

---

## 5. Minors

1. **A dead CSO on `bind_render_state` is still left to an INFO-inert assertion.**
   `MobileGL/MG_Pipe/PipeApply.cpp:457-461` — `MOBILEGL_ASSERT(record != nullptr, …); if (record == nullptr) return;`.
   At INFO (every gate and every shipped build) that is a silent no-op: the whole pipeline half and
   **both** versions stay at the previous CSO's values while the client believes the new one is bound,
   and under D12 Magma keys its pipeline memo on that handle — a cached `VkPipeline` built for CSO X
   would then draw with CSO W's fixed-function state, with no gate that can see it. `bee07c32`'s own
   message says it stopped "leaving a mixed CSO … to a DEBUG-only assertion"; it did that for
   `create_render_state` and for the attribute tail but not for the bind, which is the more
   consequential of the three. Latent today (nothing emits binds yet), so not a major.
2. **The same for slot 0 and for an unvalidated slot index on create.** `PipeApply.cpp:408-412`:
   `MOBILEGL_ASSERT(desc.Cso.Slot >= kMGPipeFirstAllocatableSlot, …)` is inert at INFO, so a `{0,0}`
   desc creates a live record at slot 0 and a subsequent bind of the *null handle* would scatter it;
   and `RenderStateCsos.resize(desc.Cso.Slot + 1)` sizes the vector from an unchecked wire field
   (harmless in-process, a denial-of-service shape once this file becomes `MG_Remote/Server/PipeApplier`).
3. **G1's `resized` set has a fourth member that no brief line admits.** `_GLOBAL__sub_I_DirectGLES.cpp`
   −9 bytes, beside the three `RenderState` symbols. G1's row says "empty or exactly the one mangled
   `RenderState::RenderState()`"; D15 admits three. The delta is `c0`'s and is declared as D-1, but the
   result file does not say *why* a `DirectGLES.cpp` static initialiser resizes when three `Bool`s are
   added to `RenderStateParameters` — G5 proves the source text did not move, so this is pure codegen.
   The integrator has to either explain it or widen G1's admitted set before the merge gate reads
   `--threshold 0` literally.
4. **A comment describes a short-circuit the code does not have.** `PipeApply.h:52-53`:
   "The last bind, so a rebind of the same handle can be answered without a scatter" —
   `MGPipeApplyBindRenderState` (`PipeApply.cpp:457-474`) always scatters; `BoundRenderStateCso` is
   only read at `PipeApply.cpp:485` to clear it on delete.
5. **A declaration comment went stale with `d1a7c5f1`.** `MGPipeRenderStateSpans.h:272-273` still says
   the subset hash is "seeded with the table version"; since that commit the seed is
   `kMGPipeRenderStateChunkTableVersion ^ BoundaryChecksum()` (`:157-158`).
6. **The mode-bit claim is not true of the whole contract.** Result file D-6 and minor 10 say all four
   files created `0755` are now `0644`; `git ls-tree -r HEAD` shows
   `MobileGL/MG_Impl/Pipe/SlotAllocator.h` and `SlotAllocator.cpp` are still `100755`. (`c0`'s files —
   two more than the four the result file counted.)
7. **`Coverage.def:79` attributes the working block to the wrong call.**
   `X(GetRenderStateParameters, CreateRenderState)` — `create_render_state` writes only the applier's
   CSO record; the block reaches `PipeInputs::m_renderState` from `bind_render_state` and
   `set_dynamic_state` (`PipeApply.cpp:464`, `:492`), which is how the two version rows are already
   spelled. Behaviourally inert (both calls sit in `kMGPipeSubsystemRenderState`, and the skip rule is
   per subsystem), but package B reads this table and the label is misleading.
8. **D-8: the process-global applier, and `MGPipeApplierReset()` with no production caller.**
   `PipeApply.cpp:380` `MGPipeApplierState g_applier{}` vs `BRIEF-P2.md:103`'s "per-context CSO store".
   The argument (the working block it writes into is itself the process-global `gPipeInputs`) is sound,
   but the consequence is real and must not be lost in the hand-off: a second context inherits the
   first's slot space **and its `ScatteredChunkBits`**, so a `set_residual_value_state` that arrives
   before the new context's first bind compares the new context's carried bits against the old
   context's block — `Fatal{PipeResidualDiverged}` and `std::abort()` in a verify build, on a correct
   program. Package B's tracker must call `MGPipeApplierReset()` at context teardown; this is currently
   only a sentence in a result file and a comment in a header.
9. **`set_pixel_pack_state` now removes no pull.** With `GetPixelStoreParameters` back to `kNone`
   (`generated/PipeFilled.inc:355`), the fill loop keeps copying both halves of `m_pixelStore` and the
   applier writes the pack half a second time (`PipeApply.cpp:499-501`). The decision is the right one
   — the alternative leaves the unpack half unwritten under a poison stamp that says it was published —
   but it means the ROADMAP P2 line item `set_pixel_pack_state` currently buys nothing, and splitting
   the field is unscheduled. Worth a line in `MEASUREMENTS.md` rather than only in §6 of the result file.
10. **The non-fatal trip-wire arm has no latch and no rate limit.** `PipeApply.cpp:650-654` reports per
    diverging capability per emission, and `set_residual_value_state` is emitted "whenever the
    capability set changes" (D9) — which is the Blaze3D `glEnable/glDisable(GL_BLEND)` shape, i.e.
    potentially twice per batch. A persistent divergence (minor 8's multi-context case is exactly one)
    would emit up to 35 `MGLOG_E` lines per emission into `/sdcard/MG/latest.log` on the G11 device
    windows. A "report once per capability until `MGPipeApplierReset`" latch would cost one bitmask.
11. **Confirmed, and carried, from the result file's own §5** — `T-5`: `BRIEF-P2.md`'s G2/G14 command
    `ctest -N | grep -E '^\s+Test #'` yields **1383** of the 2382 names on this tree (ctest wraps long
    lines); `sed -n 's/^ *Test *#[0-9]*: //p'` yields 2382, and `~/w7/p2-before-ctest-names.txt` (2363)
    is in the exact form. The integrator must use the `sed` form or G14 will silently compare a
    fifty-eight-percent sample. `T-7` also confirmed: `BRIEF-P2.md` D2's "after any scatter the applier
    calls `MGPipeDeriveRenderStateFields`" is not what the code does (it calls the chunk-scoped form);
    the doc line needs the integrator's pass.

---

## 6. Not run here, and correctly so

`ctest -L integration-gpu` / `-L integration-verify`, the 40-trace retrace under push and verify
(G3/G4), the two-device paired A/B (G11), the microbenchmark and the CSO-content-addressing control
(G12), and `scripts/g7_negative_control.sh` (package E's file, absent on this tree — §3 above is the
hand-run substitute, and it is a *different* control from the implementer's, so the two together cover
both a demotion at an array head and a demotion of a scalar mask). G8 (`HandleRecycleScenario`) and G9
(the dirty-surface gate) belong to packages E and B and do not exist on this tree; G12's counters are
package B's. Nothing in this package's diff can affect them except through the contract, which is
`c0`'s.
