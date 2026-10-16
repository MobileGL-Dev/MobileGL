# P2 package A continuation — `p2/spans` (v2, rework round 2)

Tree `~/w7/p2-spans`, branch `p2/spans`, from the contract commit `9c6a8a25` (tag `p2/contract`).
Build dirs `build-linux` (pull), `build-push`, `build-verify`. Nothing pushed. `git status --porcelain`
empty at `7c2c1456`.

This file **supersedes `spans-v1.md`**. The review `spans-review-v1.md` raised 1 major and 10 minors;
all 11 are addressed below, and the major's central correction — **T-5 of `spans-v1.md` is withdrawn,
`~/w7/p2-before-ctest-names.txt` is correct and must be kept** — is repeated in §3 where the
integrator will look for it.

---

## 1. Commits

The three v1 commits are unchanged. Three new commits on top:

| sha | subject |
|---|---|
| `810850b1` | `[Feat] (Pipe): derive every render-state PipeInputs field from the assembled working block instead of pulling it again from GLContext` (v1) |
| `eec92cd2` | `[Test] (Pipe): walk every RenderState setter and assert the pipeline-subset hash moves exactly when the pipeline version does` (v1) |
| `02b970e9` | `[Test] (Pipe): pin the slot allocator's identity contract - gen moves only on reuse and a recycled address never reproduces a handle` (v1) |
| `a9bb99a4` | `[Fix] (Pipe): scope the derivation to the chunks a scatter moved, and give the patch trio and the residual block trip wires that do not depend on call order` — **m3, m8, m9, m10** |
| `ad1238bd` | `[Test] (Pipe): require a named pipeline member to be WHOLLY pipeline, drive every indexed setter at index 0, and walk the incremental chunk path` — **m1, m2, m4, m5** |
| `7c2c1456` | `[Test] (Pipe): make the slot allocator's ABA case prove its claim without waiting for the heap, and narrow the DEBUG skip to the arm that needs it` — **m6, m7** |

Files touched across all six, all inside package A's rows of the C.5 ownership table:

- `MobileGL/MG_Pipe/MGPipeRenderStateSpans.h`
- `MobileGL/MG_Pipe/PipeApply.{h,cpp}`
- `MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp`
- `MobileGL/MG_Test/Pipe/SlotAllocatorTest.cpp`

No file owned by another package was touched.

### What each review item became

**M1 — the 999-name blind spot.** No code change; §3 and §2 carry the corrected numbers.
`ctest -N` right-aligns the id to the width of the largest id, so with 2376 tests every id below
1000 prints as `Test  #NNN:` (two spaces) and the brief's `grep -E '^\s+Test #'` drops exactly those.
Every name check in §2 was re-run with `sed -n 's/^ *Test *#[0-9]*: //p'`. **`spans-v1.md`'s T-5 is
false and is withdrawn**; the baseline is exactly `~/w7/pipe/build-linux`'s name list (0 names in
either direction) and must be kept. The extraction command must be corrected at
`BRIEF-P2.md:36` (G2), `:48` (G14), `:54` (the baseline capture), `:755` (D.1's after-every-merge
check) and `:792` (D.3) before any other package or the integrator reuses it.

**m1 — a named pipeline member must be WHOLLY pipeline.** `ChunkTablePartitionsTheBlock` used
`EXPECT_GT(pipelineBytes, 0)`; it now uses `EXPECT_TRUE(IsWhollyPipeline(...))` for every member in
`kMGPipePipelineStateMembers`, with `StencilStates` — the one documented straddler — still checked
face by face by the loop that follows. Negative control below.

**m2 — every indexed setter is also driven at index 0.** Eight new `SetterConsistency` cases:
`SetViewportIndexed(0)`, `SetCapabilityIndexed(Blend, 0)`, `SetCapabilityIndexed(ScissorTest, 0)`,
`SetBlendFuncIndexed(0)`, `SetBlendEquationIndexed(0)`, `SetColorMaskIndexed(0)`,
`SetDepthRangeIndexed(0)`, `SetScissorBoxIndexed(0)`. Each still trips the vacuity guard if it were
handed the value already stored.

**m3 — the derivation is scoped to the chunks a scatter moved.** `MGPipeApplyBindRenderState` and
`MGPipeApplySetDynamicState` now call `MGPipeDeriveRenderStateFieldsForChunks`. The four wide walks
— the 8-wide blend/colour-mask loop, the 16-wide viewport loop, the 16-wide depth-range loop and the
35-arm capability switch — are each guarded by the chunks whose bytes they read. Every guard is
`MGPipeRenderStateChunkBitsCovering(offsetof(member), sizeof(member))` over the members that block
reads, computed from the boundary table (new in `MGPipeRenderStateSpans.h`, with two `static_assert`s
that the two half-local→global widenings partition the table), so **there is no second hand-written
member→chunk map to go stale**. The ~20 scalar copies and the two 28-byte stencil copies stay
unguarded on purpose: they cannot go stale, which confines the risk of the scoping to four guards.
The 25 plain capabilities are now a single `MGP_PLAIN_CAPABILITY_LIST` used both for
`DeriveCapability`'s switch arms and for the capability guard's chunk set, so those two cannot drift.
A per-frame `glViewport` (the D8 case) now recomputes 16 viewport entries plus the scalars instead of
~170 stores and 35 switch dispatches. **Not measured** — see §5.

**m4 — the round trip has an assertion.** `ExpectAssembledBlockIsTheLiveBlock` memcmps
`gPipeInputs.GetRenderStateParameters()` against `ctx.GetRenderStateParameters()` over the whole
1168 bytes, after the whole-block apply in `DerivationMatchesTheFrontendGetters` phase 1 and after
**every** step of the new incremental case. It is the only cover for the ~25 members with no derived
field, which is the set Espryt's `SyncRenderState` reads raw.

**m5 — the incremental `create_render_state` path has a caller.** New case
`RenderStateSpans.IncrementalChunksKeepEveryDerivedFieldInStep`: a full apply, then nine deltas plus
all 35 capabilities one at a time, each emitting only `MGPipePipelineChunksThatMoved` /
`MGPipeDynamicChunksThatMoved` against a staged mirror, through `MGPipePipelineChunkBlobBytes` +
`MGPipeGatherPipelineChunks` + `MGPipeApplyCreateRenderState` with a non-null `BaseCso`. It asserts
the reconstructed record equals the live pipeline half byte for byte and that
`MGPipeHashPipelineBytes(gathered) == MGPipeComputePipelineSubsetHash(block)`. Those five functions
now all have a caller outside `MGPipeRenderStateSpans.cpp`. The case is also **m3's oracle**: a
whole-block apply names every chunk and so cannot tell a correct guard from one that is too narrow.

**m6 — the ABA case proves its claim without waiting for the heap.** The `GTEST_SKIP` is gone. The
address-reuse count is `RecordProperty`'d rather than depended on, and a deterministic arm proves the
strictly stronger form: re-acquiring the **same lifetime id** after a `Free` — the key the map is
actually built on, which `MG_State` never reissues — returns the same slot with `gen + 1`, so the
handle differs; the dead handle stays dead and the new one is live. If the identity key itself
repeating cannot reproduce a handle, no recycled address can.

**m7 — the DEBUG skip is narrowed.** The eight low-slot handouts and the "every other kind is
unaffected" arm now run in every build; only the exhaustion walk, which trips the allocator's own
`MOBILEGL_ASSERT` on purpose, sits behind the skip.

**m8 — `set_patch_state` asserts against chunk P0 (undeclared in v1, now landed).** Under
`MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY` the applier compares the incoming trio, **bitwise**
(a NaN outer level is a legal `glPatchParameterfv` value and must equal itself), against what the
working block already holds, and aborts with `Fatal{PipePatchCarriersDiffer}` on a disagreement. It
is armed only once a CSO has been bound, because a `set_patch_state` that legitimately precedes the
first `bind_render_state` of a context has nothing to agree with yet. **This states an ordering
contract on package B: within a validate, the bind comes first.** `MOBILEGL_ASSERT` was not used —
it is DEBUG-only (`Defines.h:104`), so it would not fire in the verify build D6 names.

**m9 — the residual trip wire no longer depends on call order.**
`MGPipeApplySetResidualValueState` compares the carried bits against
`DeriveCapability(<the working block>, cap)` rather than against `PipeInputs::m_capability`, which
only the derivation writes. A residual block emitted before the first bind of a context — what D9's
"once per context" describes — used to be compared against all-false storage while `Dither` and
`Multisample` default to true, and aborted under poison/verify on a perfectly correct context. The
check is unchanged in strength.

**m10 — the "verify comparator is the guard" claim is corrected, not repeated.** The comment in
`PipeApply.h` now says what is true: `ArmVerify` reads `MG_Config::Features.PipeVerify`, which a unit
test process never sets, so the comparator is the oracle **on the retrace and integration-verify
lanes (G3/G4), which this package cannot reach**; the unit oracle is
`DerivationMatchesTheFrontendGetters` and, since this round, `IncrementalChunksKeepEveryDerivedFieldInStep`.
Re-verified: `MOBILEGL_PIPE_VERIFY=1 ./build-verify/.../RenderStateSpansTest` → `[ PASSED ] 5 tests`,
i.e. the comparator does not arm there.

---

## 2. Verification — every command and its actual result

All in `~/w7/p2-spans` at `7c2c1456`, `CCACHE_BASEDIR=/home/swung/w7`.

| command | result |
|---|---|
| `cmake --build build-{linux,push,verify} -j 12` | rc 0, rc 0, rc 0 |
| `python3 scripts/gen_pipe.py --check` | rc 0 — "generated files are up to date" |
| `python3 scripts/gen_pipe.py --self-test` | rc 0 — "7 negative-control trip(s), positive control OK" |
| `python3 scripts/check_include_closure.py` | rc 0 — "4 probes, 0 skipped, 0 problem(s)" |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0 (informational at this branch point; `--check`/`--self-test` are package B's, G9) |
| `ctest --test-dir build-linux -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1498** |
| `ctest --test-dir build-push -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1498** |
| `ctest --test-dir build-verify -L unit --no-tests=error -j 8` | rc 0 — **100% passed, 0 failed of 1498** |
| `RenderStateSpansTest` (push / verify) | **5/5 PASSED** in both |
| `RenderStateSpansTest` (pull) | 0 passed, **5 SKIPPED** ("push not compiled in") |
| `SlotAllocatorTest` (push / verify) | **6/6 PASSED** in both |
| `SlotAllocatorTest` (pull) | 1 passed, 5 SKIPPED |
| `MOBILEGL_PIPE_VERIFY=1 ./build-verify/…/RenderStateSpansTest` | `[ PASSED ] 5 tests` — the comparator does not arm in a unit process (m10) |
| **G1** `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — **0 added / 0 removed / 0 renamed**, 4 resized, `.text` +160 (see deviation D-1) |
| **G1 attribution** same, `~/w7/p2-contract/build-linux/libMobileGL.so` → `build-linux/…` | **0 added / 0 removed / 0 renamed / 0 resized**, `.text` +0 — all six of this branch's commits move the pull build by nothing |
| **G5** `awk '/namespace RenderStateImpl \{/,/\} \/\/ namespace RenderStateImpl/' DirectGLES.cpp \| sha256sum` | `d8fd1c48…0efe27` — **equal** to `~/w7/p2-before-syncrenderstate.sha` |
| **G13** `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty (rc 1). No stdio added under `MG_Backend`/`MG_State`; no `printf`/`chrono`/`clock_gettime`/`MGLOG_I` in the diff |

### G2 shape and G14, with a CORRECT extraction

`names () { ctest --test-dir "$1" -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort; }`

```
p2-spans/build-linux   total 2376   extracted 2376   (the brief's grep would extract 1377)
p2-spans/build-push    total 2376   extracted 2376
p2-spans/build-verify  total 3194   extracted 3194   (the brief's grep would extract 2195)
pipe/build-linux       total 2363   extracted 2363
pipe/build-verify      total 3181   extracted 3181
~/w7/p2-before-ctest-names.txt: 2363 lines

baseline vs pipe/build-linux : 0 only-in-baseline, 0 only-in-tree   -> the baseline is EXACT
G2 shape, build-linux vs build-push : diff empty (2376 == 2376)
G14, build-linux vs baseline : removed 0, added 13
G14, build-verify vs pipe/build-verify : removed 0, added 13
```

The 13 added names are the 5 `RenderStateSpans.*`, the 6 `SlotAllocator.*` and the two contract
placeholders `Tracker.PlaceholderUntilTheOwningPackageFillsThisIn` /
`CsoCache.PlaceholderUntilTheOwningPackageFillsThisIn`. The 13th vs v1's 12 is this round's
`RenderStateSpans.IncrementalChunksKeepEveryDerivedFieldInStep`.

### Negative controls (all run at `7c2c1456`; tree clean afterwards)

`scripts/g7_negative_control.sh` is package E's per C.5 and was not created. The manual recipes are
kept at `scratchpad/wf5/spans/g7_manual_negcontrol.sh` and `…/negctl.sh`, `…/negctl2.sh` for E to
lift from.

| control | what it changes | result (non-zero rc is the PASS) |
|---|---|---|
| **G7 proper** (the brief's own) | P1/D2 boundary `offsetof(ClearColor)` → `offsetof(ColorMasks)`, so `ColorMasks` + D3's three new bools fall dynamic; 396→361, 772→807 | build rc 0, suite **rc 1**. `ChunkTablePartitionsTheBlock` FAILS naming `ColorMasks`, `FramebufferSrgbEnabled`, `DepthClampEnabled` ("only 0 of its N bytes are in the pipeline half"); `SetterConsistency` FAILS naming `SetCapability(DepthClamp)` / `SetCapability(FramebufferSrgb)` |
| **m1/m2's control** (the one v1 could not see) | P1 starts 4 bytes late, demoting `BlendStates[0].Enabled` — the `glEnablei(GL_BLEND, 0)` bit — while `BlendStates` stays a named pipeline member; both byte-count constants updated as P3 legitimately would | build rc 0, suite **rc 1**. `ChunkTablePartitionsTheBlock`: "BlendStates is named as pipeline state but only 220 of its 224 bytes are in the pipeline half"; `SetterConsistency`: "SetCapabilityIndexed(Blend, 0): the pipeline-subset hash held but m_pipelineStateVersion MOVED". **Under v1 this control left both cases green once the constants were updated.** |
| **m3's control B** | drop `ClipDistanceEnabledMask` from the capability guard | build rc 0, suite **rc 1** — `IncrementalChunksKeepEveryDerivedFieldInStep` and `DerivationMatchesTheFrontendGetters` both FAIL |
| **m3's control C2** | point the viewport guard at `DepthRanges` instead of `Viewports` | build rc 0, suite **rc 1** — `IncrementalChunksKeepEveryDerivedFieldInStep` FAILS at `step 1: viewports only (dynamic chunk D0)`, "viewport 0" |
| (control C, discarded) | drop `ColorMasks` from the blend guard | **no effect, correctly**: `ColorMasks` and `BlendStates` are in the same chunk P1, so the covering computation yields the same constant. Recorded because it is evidence the guards are chunk-granular, not member-granular |
| restore after each | `cp` back, rebuild, rerun | rc 0, suite green, `git status --porcelain` empty |

---

## 3. Where the tree contradicted the brief

**T-5 IS WITHDRAWN.** `spans-v1.md` claimed `~/w7/p2-before-ctest-names.txt` "does not correspond to
any build dir of the base ref" and asked the integrator to re-capture it. That was a misdiagnosis of
the *extraction command*, not of the artefact. The baseline is name-for-name identical to
`~/w7/pipe/build-linux` (0 in either direction). **Keep it.** What must change is the command:
`grep -E '^\s+Test #'` (one literal space) silently drops every test whose id is below 1000, because
`ctest -N` right-aligns the id — 999 of 2376 names in `build-linux`, 999 of 3194 in `build-verify`.
Use `sed -n 's/^ *Test *#[0-9]*: //p'` (no `grep`) at `BRIEF-P2.md:36`, `:48`, `:54`, `:755`, `:792`.
Left uncorrected it blinds G2's shape check and G14 for the rest of P2 and for the other four
packages.

**T-1 — `ScissorTestEnabledMask` is a `DynamicTailKey` input AND is pipeline state.** Unchanged from
v1 and confirmed by the reviewer. D19 names two exceptions to "every `DynamicTailKey` field is
dynamic"; the tree has a third. Harmless — `BumpVersions()` moves `m_version` too, so
`MGPDynamicState::Version` still moves on a scissor-enable change — and the test asserts it the other
way round so a later demotion is loud. The integrator may want a line in `ARCHITECTURE.md` §5.3.

**T-2 — the landed chunk table is not D6's printed table.** D6 puts `StencilStates[0].Func` in P3;
the file puts it at the end of P2. The brief says the file wins; the file's `static_assert`s hold.

**T-3 — four of D5's 29 fields are not `kDraw` fields.** `FillPoints.def` gives the three clear
values to `kClear` and `GetClampReadColor` to `kReadback`. `DerivationMatchesTheFrontendGetters`
therefore runs in three verb phases. Unchanged from v1.

**T-4 — D5's field names are not the tree's** (`m_minSampleShadingValue`,
`m_patchDefault{Inner,Outer}Level`). Cosmetic.

**T-6 — `wsl_p2_tree.sh <slug> p2/contract` is ambiguous**: `p2/contract` exists as both a branch
(checked out in `~/w7/p2-contract`) and a tag, so `git worktree add` refuses with
`fatal: ambiguous object name`. This tree was created with the SHA (`wsl_p2_tree.sh spans 9c6a8a25
verify`). Same commit.

---

## 4. Deviations from the brief

**D-1 — G1's admitted resize set needs a fourth symbol, and it belongs to the contract commit.**
Unchanged from v1 and independently confirmed by the reviewer. D15/C.0 admit
`RenderState::{RenderState, SetCapability, IsCapabilityEnabled}`; the pull build also resizes
`_GLOBAL__sub_I_DirectGLES.cpp` by −9 bytes, because `DirectGLES.cpp:1986`'s
`static RenderStateParameters g_syncedRenderStateParameters` gains three NSDMI members that fill the
`[581,584)` padding hole. `symbol_report.py` between `~/w7/p2-contract/build-linux/libMobileGL.so`
and this tree's is 0/0/0/0, so it is `c0`'s delta, not this branch's. The contract is tagged and four
packages branch from it, so it was not amended: the integrator either widens G1's admitted set to
these four symbols or notes the attribution in the merge commit.

**D-2 — c2 and c3 delete the contract commit's placeholder cases.** Unchanged from v1; neither name
is in the baseline, so G14 shows 0 removed.

**D-3 — `scripts/g7_negative_control.sh` was not created**, because C.5 gives it to package E. The
manual equivalents were run (§2) and are kept in `scratchpad/wf5/spans/`.

**D-4 — `CompositeShaderBandIsNeverHandedOut` still skips its EXHAUSTION ARM in a DEBUG build**, and
only that arm since `7c2c1456`. Walking the ShaderCso slot space to
`kMGPipeShaderCsoCompositeSlotBase` trips the allocator's own live-and-correct
`MOBILEGL_ASSERT`. The other two arms now run in every build.

**D-5 (new) — `set_patch_state`'s trip wire places an ordering contract on package B.** D6 and D10
require the assertion; nothing in the tree said when the two carriers are expected to agree. As
landed it is armed only after a CSO has been bound, i.e. within a validate the tracker must emit
`bind_render_state` before `set_patch_state`. If B needs the other order, the guard condition — not
the assertion — is what moves.

**D-6 (new) — three files changed mode 100755 → 100644.** `MGPipeRenderStateSpans.h`,
`PipeApply.{h,cpp}` were created executable by `c0` (a Windows artefact); editing them over the UNC
path normalised them to 644, which is what every other file under `MobileGL/MG_Pipe/` already is.
`MGPipeRenderStateSpans.cpp` is still 100755 and was deliberately left alone this round rather than
carry an unrelated change in a code commit; the integrator may want to normalise it.

**D-7 (new) — `MGPipeDeriveRenderStateFields(PipeInputs&)` is kept as the whole-block entry point**
even though the applier no longer calls it. D2 says "after any scatter the applier calls
MGPipeDeriveRenderStateFields"; the applier now calls the chunk-scoped form, and the whole-block form
remains as its `kMGPipeAllGlobalChunks` special case for any caller (package B, a server reset) that
wants the unconditional walk. No behaviour of D2 changes: the scoped call derives exactly what the
whole-block call would, which is what §2's controls B and C2 pin.

---

## 5. Unfinished

- **m3 is implemented but NOT measured.** This package has no benchmark: the DriverBench T1/T2
  decomposition and `benchmark.json`'s `frameCpuTimesMs[]` are package E's (G11/G12, C.5), and the
  ceiling is pinned in `MEASUREMENTS.md` by the integrator (D.4.5). The scoping is justified by the
  work it removes (a D0-only scatter no longer runs the 8-wide blend loop, the 16-wide depth-range
  loop, the 16-wide indexed-scissor loop or the 35-arm capability switch) and by its correctness
  controls, not by a number. The first real number can only come from E's microbenchmark on the
  finished tree.
- **`MGPipeDeriveRenderStateFields*` still has no production caller.** The applier is reached only
  from the unit tests until package B's tracker emits `create/bind_render_state` and
  `set_dynamic_state`. That is the intended landing order (D12) and it is why nothing on this branch
  can change a retrace pixel.
- Gates this tree cannot reach alone: **G2**'s two `-L integration-gpu` runs (GPU), **G3** (40-trace
  SSIM), **G4** (retrace under verify — and it is the only oracle for the derivation, m10),
  **G8** (`HandleRecycleScenario`, E), **G9** (`gen_pipe_dirty_surface.py --check/--self-test`, B),
  **G10**'s device `MOBILEGL_PIPE_STATS` window, **G11** (two-device A/B), **G12** (the microbenchmark
  and the `CsoContentAddressing` ctest entry, B and E), **G14**'s `gh workflow run`.
- `MEASUREMENTS.md` lines for the measured chunk sizes (396 / 772, and the per-chunk 264/28/20/272/
  168/20/12/16/12/28/28/8/4/24/264) are the integrator's per C.5.
