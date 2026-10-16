# Package A continuation `p2/spans` — result file v3 (rework round 3)

Tree `~/w7/p2-spans`, branch `p2/spans`, HEAD **`842af233`**, ten commits on top of the contract
`9c6a8a25` (tag `p2/contract`). Not pushed. `git status --porcelain` empty at HEAD; every build
directory is fully up to date at HEAD (`cmake --build` reports 0 steps in all three), so §2's
table below was produced against the final checkout — review minor 9.

**This round fixes both majors and nine of the ten minors of `spans-review-v2.md`.** The tenth
(minor 4, the process-global CSO store) is argued rather than fixed and is declared as D-8.

---

## 1. Commits

| commit | subject |
|---|---|
| `810850b1` | `[Feat] (Pipe): derive every render-state PipeInputs field from the assembled working block instead of pulling it again from GLContext` (round 1) |
| `eec92cd2` | `[Test] (Pipe): walk every RenderState setter and assert the pipeline-subset hash moves exactly when the pipeline version does` (round 1) |
| `02b970e9` | `[Test] (Pipe): pin the slot allocator's identity contract - gen moves only on reuse and a recycled address never reproduces a handle` (round 1) |
| `a9bb99a4` | `[Fix] (Pipe): scope the derivation to the chunks a scatter moved, and give the patch trio and the residual block trip wires that do not depend on call order` (round 2) |
| `ad1238bd` | `[Test] (Pipe): require a named pipeline member to be WHOLLY pipeline, drive every indexed setter at index 0, and walk the incremental chunk path` (round 2) |
| `7c2c1456` | `[Test] (Pipe): make the slot allocator's ABA case prove its claim without waiting for the heap, and narrow the DEBUG skip to the arm that needs it` (round 2) |
| **`bee07c32`** | **`[Fix] (Pipe): arm both applier trip wires off the applier's own scatter ledger instead of off a bound handle, and stop leaving a mixed CSO or a malformed attribute tail to a DEBUG-only assertion`** (round 3 — MAJOR 1, minors 2/3/5) |
| **`ce370a3e`** | **`[Test] (Pipe): drive both redundancy trip wires and the four apply entry points nothing in any build reached, and finish the setter walk's stencil faces and hint targets`** (round 3 — MAJOR 2, minor 6, minor 7) |
| **`d1a7c5f1`** | **`[Fix] (Pipe): stop claiming set_pixel_pack_state supplies a field it only half writes, and fold the chunk boundaries into the subset hash's seed`** (round 3 — minors 1 and 8) |
| **`842af233`** | **`[Chore] (Pipe): drop the executable bit from MGPipeRenderStateSpans.cpp`** (round 3 — minor 10, mode-only, no code) |

Files touched this round, all A-owned per `BRIEF-P2.md` §C.5: `MG_Pipe/PipeApply.{h,cpp}`,
`MG_Pipe/MGPipeRenderStateSpans.{h,cpp}`, `MG_Pipe/Coverage.def`,
`MG_Pipe/generated/PipeFilled.inc`, `MG_Test/Pipe/RenderStateSpansTest.cpp`,
`MG_Test/Pipe/CMakeLists.txt`. **No file owned by another package was touched.**

---

## 2. Verification — every command and what it actually printed

All in `~/w7/p2-spans` at `842af233`, `CCACHE_BASEDIR=/home/swung/w7`.

| gate / command | observed |
|---|---|
| `cmake --build build-{linux,push,verify} -j 12` | rc 0, rc 0, rc 0; **0 build steps in each** (the tree at HEAD is fully built — minor 9) |
| **G1** `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text 10792579 -> 10792739 (+160)`. Identical to round 2 — the four resizes are all `c0`'s (`RenderState::{RenderState,SetCapability,IsCapabilityEnabled}` + `_GLOBAL__sub_I_DirectGLES.cpp`), and this round adds nothing to the pull build. |
| **G5** `awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27` — byte-equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (rc 1) |
| **G13** `python3 scripts/check_include_closure.py` | `4 probes, 0 skipped, 0 problem(s)` (rc 0) |
| **G13** `gen_pipe.py --check` / `--self-test` | `generated files are up to date` (rc 0) / `7 negative-control trip(s), positive control OK` (rc 0) |
| `ctest -L unit` build-linux / build-push / build-verify | **100% passed, 0 failed, out of 1504** in each (1498 before this round; +6 cases) |
| `ctest -R 'RenderStateSpans\.'` ×3 builds | 11 / 11 / 11 passed (5 cases before round 2, 5 after, **11 now**) |
| `ctest -R 'SlotAllocator\.'` ×3 builds | 6 / 6 / 6 passed |
| **G10** `ctest --test-dir build-verify -R 'Residual' --no-tests=error` | **6 tests, 100% passed** — and three of the six are now the wire itself: `ResidualTripWireIsSilentUntilTheApplierOwnsTheBytes`, `…HoldsAcrossAVerbClassThatDoesNotPublishTheBlock`, `…FiresNamingTheCapability`. Before this round `-R Residual` reached only the two `static_assert` catalogue cases and an unrelated `DriverBugProbes` entry. |
| **G2 shape** `names build-linux` vs `names build-push` | 2382 vs 2382, `diff` empty |
| **G14** `comm -23 ~/w7/p2-before-ctest-names.txt <names>` | **0 removed**, 19 added (12 `RenderStateSpans.*`, 6 `SlotAllocator.*`, 2 placeholders — 19 because the two placeholders are counted once each) |
| **G7** manual negative control (D19: move the P1/D2 boundary from `ClearColor` to `ColorMasks`, byte counts 396/772 → 361/807) | build rc 0; `ctest -R 'RenderStateSpans\.'` **rc 8, exactly two entries red**: `ChunkTablePartitionsTheBlock` (`ColorMasks … only 0 of its 32 bytes are in the pipeline half`, plus the three D3 bools) and `SetterConsistency` (`SetColorMask`, `SetColorMaskIndexed`, `SetColorMaskIndexed(0)`, `SetCapability(DepthClamp/FramebufferSrgb/TextureCubeMapSeamless)`: *the pipeline-subset hash held but m_pipelineStateVersion MOVED*). Reverted, rebuilt, 11/11 green, `git status` clean. `scripts/g7_negative_control.sh` is package E's and does not exist on this tree. |
| **new: trip-wire negative control A** — delete both wires' arming (the pre-`bee07c32` unconditional shape) | `build-verify` builds; `RenderStateSpansTest` **dies inside `ResidualTripWireIsSilentUntilTheApplierOwnsTheBytes`** with no further output — i.e. `std::abort()` on a correct context, MAJOR 1 exactly. Reverted and rebuilt. |
| **new: trip-wire negative control B** — make every comparison agree (the wire can never fire) | `ResidualTripWireFiresNamingTheCapability` and `PatchCarrierTripWireFiresOnlyWhenTheApplierOwnsChunkP0` both **FAIL**; the other two trip-wire cases still pass. Reverted and rebuilt. |

Not run here and not this package's: `ctest -L integration-gpu` / `-L integration-verify`, the
40-trace retrace (G3/G4), the device runs (G11), the microbenchmark (G12). They are the
integrator's D.3/D.4.

---

## 3. What each review item became

### MAJOR 1 — the residual wire's oracle (fixed, `bee07c32`)

The review is right that `a9bb99a4` moved the oracle from a field published at seven verb
classes to one published at five, that the m9 claim ("unchanged in strength", "no ordering
contract at all") was false in both halves, and that `MOBILEGL_PIPE_PUSH=0x10` +
draw / `glDisable(GL_DITHER)` / dispatch aborts a correct context. **That claim is withdrawn.**

The fix is neither of the first two directions the review offered verbatim; it is the second one
made precise, and it does not touch `FillPoints.def` (package B's file):

`MGPipeApplierState` gains `Uint32 ScatteredChunkBits` — the global chunk bits **this applier has
itself scattered** into `PipeInputs::m_renderState`, written by `bind_render_state` (the whole
pipeline half) and `set_dynamic_state` (the chunks it names), cleared by `MGPipeApplierReset()`.
The residual wire compares capability *i* only when every chunk that capability's answer is read
out of is in the ledger. `set_patch_state`'s own write to the working block deliberately does not
enter the ledger.

Why the ledger and not `m_capability`:

- with the render-state subsystem **off**, the ledger is empty, the wire is silent, and the
  `0x10` repro is dead. That is correct rather than merely convenient: with the subsystem off the
  working block is the per-verb fill loop's, published per class, so disagreeing with it means
  nothing;
- with the subsystem **on**, the applier is the block's only writer (D5's skip rule removes the
  field from the fill loop), so its bytes are current at *every* verb of *every* class — which is
  what case 6b drives, as a draw / capability change / `kDispatch` sequence;
- `m_capability` would be the *weaker* oracle exactly where the review calls it correct: at a
  class that publishes it, the fill loop filled it out of the same `GLContext` the client built
  `CapabilityBits` from, so the comparison is a tautology. The redundancy D9 exists to check is
  between the **carried** bits and the **assembled** block.

The grain is per capability because a bind alone owns the pipeline half while the eight
`ClipDistance` answers come from `ClipDistanceEnabledMask` in dynamic chunk **D7**; case 6a pins
both directions. The source-chunk table is written from the same `MGP_PLAIN_CAPABILITY_LIST` and
the same boundary table `DeriveCapability` reads, so the two cannot drift.

On the `MGP_INPUT_CHECK` bypass: the poison gate answers "did the **filler** publish this field
for this verb". For this read that is the wrong question — the applier reads bytes it wrote
itself, at a verb the filler may legitimately not have published anything for — and the ledger is
a strictly stronger guard for it, because it is false in exactly the cases the poison would have
caught and in more besides. This is now stated in `PipeApply.h` next to the field, and recorded
as **D-9** below.

### MAJOR 2 — no caller, no test (fixed, `ce370a3e`)

All five unreached entry points plus `MGPipeDeriveRenderStateFields` are now driven, and both
wires are driven in three states each (disarmed / armed-and-agreeing / armed-and-diverging), in
**every** build:

| case | what it pins |
|---|---|
| `ResidualTripWireIsSilentUntilTheApplierOwnsTheBytes` | the `0x10` shape says nothing; a bind alone arms some but not all; a `ClipDistance` disagreement is silent until the dynamic half lands; the full 35 are compared after it |
| `ResidualTripWireHoldsAcrossAVerbClassThatDoesNotPublishTheBlock` | the review's own repro: draw → capability change → `kDispatch`. Red on the pre-fix wire (negative control A), green now |
| `ResidualTripWireFiresNamingTheCapability` | the wire fires: SIGABRT + `Fatal{PipeResidualDiverged, "Dither"}` under poison/verify, counted + logged under plain push |
| `PatchCarrierTripWireFiresOnlyWhenTheApplierOwnsChunkP0` | disarmed → silent; armed + agreeing **with a NaN outer level** → silent (a `==` compare would call that a divergence); armed + diverging → `Fatal{PipePatchCarriersDiffer}` / counted |
| `WholeBlockDerivationAgreesWithTheChunkScopedOne` | D-7's kept whole-block entry point, previously dead (minor 7) |
| `TheRemainingApplyEntryPointsReachPipeInputs` | `set_pixel_pack_state`, `set_vertex_attrib_defaults` and `delete_render_state`, each read back through the accessor a backend uses, under the verb class that publishes it |

Both wires now also **run in the shipped push build**, counting and logging where a poison or
verify build aborts (one `MGP_TRIP_WIRE_REPORT`/`MGP_TRIP_WIRE_TAG` pair; only the fatal arm
writes the `Fatal{…}` marker G4 greps for). A wire compiled out of every build a device runs is
not a wire, and the counters are what make the non-fatal arm assertable.

The diverging cases fork and read the wire's line back out of a log file, so the suite now has
its own `main()` and links `GTest::gtest` — `PipeInputsTest`'s shape and its stated reason
(*"Never `EXPECT_DEATH`"*). That is a `MG_Test/Pipe/CMakeLists.txt` edit; the file is A's
(§C.5), and the change is additive for the other three suites, which stay in the `foreach`.

### Minors

1. **`GetPixelStoreParameters` marked emitted (fixed, `d1a7c5f1`).** The row is removed from
   `MGP_COVERAGE_EMITTED_LIST` and the reason is in `Coverage.def`'s prose (`kNone` in
   `generated/PipeFilled.inc:355`). The accessor's row in `MGP_COVERAGE_ACCESSOR_LIST`
   (`Coverage.def:66`) is a *plan* mapping and stays. Emitted count 34 → 33; `gen_pipe --check`
   and `--self-test` green; no test pinned the count. **This is a change to a `c0` contract file
   after the tag**, in the safe direction (a field goes back through B's fill loop rather than
   being skipped) — package B is the only consumer of that table and it has not landed. Declared
   again in §4 so the integrator tells B.
2. **`MOBILEGL_ASSERT` in the incremental create path (fixed, `bee07c32`).** Both arms start from
   a defined base (`{}` or the base record) and report through the trip-wire verdict, so a dead
   `BaseCso` can no longer leave a recycled slot's previous occupant under the delta chunks in an
   INFO build. A brand-new CSO that does not name every chunk is reported the same way.
3. **`MOBILEGL_ASSERT` in `SetVertexAttribDefaults` (fixed, `bee07c32`).** The loop walks the
   mask's 32 bits, consumes a tail entry for every named location (so a named-but-unstorable
   attribute cannot desynchronise the rest), and reports all three faults in every build.
   `PipeInputs::kMaxVertexAttribs` is `VertexArrayObject::MAX_VERTEX_ATTRIBS == 32` today, so the
   out-of-range arm is unreachable; the guard is what keeps that true if the two stop agreeing.
4. **Process-global CSO store (not fixed — declared, D-8).** See §4.
5. **The patch wire's undeclared class contract (fixed, `bee07c32`).** Same ledger; the arming
   condition is now "chunk P0 is the applier's", which covers the ordering contract and the
   verb-class one at once, and it is written down at the wire.
6. **`SetterConsistency` never wrote chunk P3 (fixed, `ce370a3e`).** `SetStencilOp`,
   `SetStencilFunc` and `SetStencilMask` are driven on **both** faces and `SetHint` on all four
   targets.
7. **`MGPipeDeriveRenderStateFields` dead (fixed as far as this package can, `ce370a3e`).** It now
   has a caller — `WholeBlockDerivationAgreesWithTheChunkScopedOne` — and no production caller,
   which is correct: under split a scatter can arrive without a chunk mask. **D2's sentence "after
   any scatter the applier calls `MGPipeDeriveRenderStateFields`" is still false of the landed
   code** (the applier calls the chunk-scoped form) and remains for the integrator's doc pass,
   alongside T-1 and T-2. Carried into §5.
8. **The chunk-table version was a promise, not a gate (fixed, `d1a7c5f1`).** The hash is now
   seeded with `kMGPipeRenderStateChunkTableVersion ^ BoundaryChecksum()`, an FNV-1a fold of the
   16 boundaries, so a moved boundary invalidates every persisted key whether or not anyone
   bumps the version. Deliberately **not** a `static_assert` on the boundaries: that would turn
   G7's negative control — which moves a boundary on purpose and must still compile (D19) — into
   a build break instead of a red test. Re-verified: the G7 control still builds and still fails
   exactly the two cases.
9. **The table was written against a stale checkout (fixed).** §2 above was produced at
   `842af233` with all three build directories reporting 0 steps.
10. **Mode changes riding in a code commit (fixed, `842af233`).** The last `100755` file is now
    `100644` in a mode-only commit. The three that rode inside `a9bb99a4` cannot be un-ridden
    without rewriting a reviewed commit, which is not worth it; recorded here instead.

---

## 4. Deviations from the brief, with reasons

Carried forward from v2 (unchanged, re-stated so this file stands alone):

- **D-1** G1's `resized` set is three `RenderState` symbols plus `_GLOBAL__sub_I_DirectGLES.cpp`
  (−9 bytes), not the one the G1 row names. All four are `c0`'s; contract-to-HEAD is `0/0/0/0`.
- **D-2** `MGPipeTypes.h`'s `MGPDynamicState` comment about sample coverage was corrected in `c0`
  (D6 asks for it).
- **D-3** The 29 derivations are 25 `kDraw` + 3 `kClear` + 1 `kReadback`, checked under three
  separate verb scopes, because the fill table is what says which verb may read which field.
- **D-4** `SlotAllocator.CompositeShaderBandIsNeverHandedOut` skips its exhaustion arm in a DEBUG
  build only (`MOBILEGL_ASSERT` is live there and the band is asserted, not returned).
- **D-5** — **superseded this round.** v2 declared an *ordering* contract on both wires ("the
  bind comes first within a validate"). The real contract is the applier's scatter ledger, which
  subsumes it and the verb-class contract; see MAJOR 1 above and D-9 below.
- **D-6** The four P2 files were created `100755`; all four are now `100644`, the last of them in
  its own commit.
- **D-7** `MGPipeDeriveRenderStateFields` (whole block) is kept beside the chunk-scoped form for
  the split server. It now has a test caller and still no production caller.

New this round:

- **D-8 — the CSO store is process-global, not per context (review minor 4).** `BRIEF-P2.md:103`
  says "per-context CSO store"; the tree has one `MGPipeApplierState g_applier`
  (`PipeApply.cpp`). **Not changed, and the reason is that the thing it writes into is also one
  process-global**: the working block *is* `gPipeInputs` (D2's own decision), a single block for
  the process, and none of the seven apply signatures carries a context identity. A per-context
  slot space over a per-process working block would be the inconsistent shape, not the current
  one. `MGPipeApplierReset()` is the teardown hook and has no production caller yet — **the
  tracker (package B) must call it when a context is destroyed**, or a second context inherits
  the first's slot space and its `ScatteredChunkBits`. Flagged for B and for the integrator; the
  header says so at `PipeApply.h`.
- **D-9 — the applier reads `m_renderState` through the friend accessor, bypassing
  `MGP_INPUT_CHECK`, deliberately.** The poison gate answers "did the filler publish this field
  for this verb"; the applier reads bytes *it* wrote, at a verb the filler may legitimately not
  have published anything for. The guard that is right for this read is the scatter ledger, and
  it is strictly stronger here: it is false in every case the poison would have caught and in the
  cases the poison cannot see (the subsystem being off entirely). Related to, but not the same
  as, the P1 `g_fbSlotCache` bypass D13 asks P2 to close — that one is a *cache* read of state
  the filler owns.
- **D-10 — both trip wires now run in the shipped push build**, not only under poison/verify as
  `BRIEF-P2.md` D6/D9 phrase it ("asserts under verify", "counted and logged otherwise"). D9
  already asks for the counted-and-logged arm; this makes the patch wire behave the same way
  instead of compiling out. Cost: a 24-byte `memcmp` on `set_patch_state` (emitted about once per
  program) and a 35-iteration switch on `set_residual_value_state` (emitted once per context and
  on capability-set changes). Neither is a hot path and neither is instrumentation.
- **D-11 — `MG_Test/Pipe/CMakeLists.txt` changes after the contract tag.** `RenderStateSpansTest`
  moves out of the four-suite `foreach` and links `GTest::gtest` with its own `main()`. A's file
  (§C.5); the other three suites are untouched.
- **D-12 — `Coverage.def`'s emitted list loses `GetPixelStoreParameters` after the contract
  tag** (minor 1). The direction is safe for every other package; B is the only consumer and has
  not landed.

---

## 5. Where the tree contradicted the brief

Carried from v2 and re-confirmed at HEAD:

- **T-1** `ScissorTestEnabledMask` is both a `DynamicTailKey` input and pipeline state; the file
  wins, `DynamicChunksCoverMagmasDynamicTailKey` asserts the direction that makes a later
  demotion loud. `ARCHITECTURE.md` §5.3 needs the sentence.
- **T-2** `StencilStates[0].Func` ends chunk **P2**, not P3; the brief's own P2 row was
  internally inconsistent (20 bytes for four members measuring 16). The file wins.
- **T-3/T-4/T-6** as recorded in v2 (the 44-name `kMGPipePipelineStateMembers`, the nine fill
  classes, the `MGPipeTypes.h` comment).
- **T-5 withdrawn** in v2: `BRIEF-P2.md`'s `grep -E '^\s+Test #'` in G2/G14 drops 999 of 2382
  names because ctest wraps long lines. `~/w7/p2-before-ctest-names.txt` is exact and must be
  kept; the brief's command needs `sed -n 's/^ *Test *#[0-9]*: //p'` (the shape §D.3 already
  uses). Re-confirmed this round.
- **T-7 (new)** `BRIEF-P2.md` D2: *"After any scatter the applier calls
  `MGPipeDeriveRenderStateFields(PipeInputs&)`"*. The landed applier calls
  `MGPipeDeriveRenderStateFieldsForChunks` with the chunks the scatter moved; the whole-block
  form is kept for the split server. The doc line needs correcting in the integrator's pass.

---

## 6. Unfinished

1. **`MGPipeApplierReset()` has no production caller.** Package B's tracker must call it at
   context teardown (D-8). Until it does, the applier's slot space and its scatter ledger
   outlive a context.
2. **The unpack half of `m_pixelStore` has no carrier** and now correctly claims none. Splitting
   the field, or giving unpack its own call, is a later phase's; the decision the review asked
   for is made (minor 1) and it is B-safe.
3. **`MGPipeDeriveRenderStateFields` still has no production caller** — correct for now, but the
   integrator must correct D2's sentence (T-7).
4. **Nothing emits any of these calls yet.** The tracker (B) is what turns the applier into a
   live path; everything above is driven by unit cases and by the retrace/integration lanes once
   B lands.
5. Not run on this tree, by design: `-L integration-gpu` / `-L integration-verify`, the 40-trace
   retrace under push and verify (G3/G4), the device pairs (G11), the microbenchmark and the
   CSO-content-addressing control (G12), `scripts/g7_negative_control.sh` (package E's file — the
   control was run by hand, §2).
