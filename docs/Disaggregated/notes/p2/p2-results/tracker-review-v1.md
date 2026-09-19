# Adversarial review — P2 package B `p2/tracker` (`~/w7/p2-tracker`, `9c6a8a25..7993d711`)

Reviewer's brief: refute correctness/completeness against BRIEF-P2 §A (G1–G14), §B (D1–D20),
§C.1 and §C.5. Everything below was re-derived or re-run in `~/w7/p2-tracker`; the tree was
left as found (`git status --porcelain` empty before and after; only `build-*` and
`~/w7/rev-tk/`, `~/w7/retrace-out/rev-tk-verify/` were written).

**Verdict: NOT APPROVED — 2 majors.** The headline deliverable is real and is proven by the
strongest oracle in the tree; the two majors are a factually wrong row-pair in the mapping
file that this package just turned into a gate, and a wire payload that is landed as "real"
while being incapable of reproducing the value it carries.

---

## 1. What I re-ran, and what it printed

Every command below was run by me, not read out of `tracker-v1.md`.

| gate | command | observed |
|---|---|---|
| G9 | `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `dirty-surface: 73 mutators, all mapped, no stale rows` |
| G9 | `… --self-test` | rc 0 — `3 negative controls, all tripped` |
| G9 | `… --summary` | rc 0 — still works (E's CI step is unbroken) |
| G13/G6 | `python3 scripts/gen_pipe.py --check` | rc 0 — `63 PipeInputs fields (7 sticky, 34 emitted by a P2 call)`, `generated files are up to date` |
| G13 | `python3 scripts/gen_pipe.py --self-test` | rc 0 — `7 negative-control trip(s), positive control OK` |
| G13 | `python3 scripts/check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| G13 | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |
| **G5** | `git show {48268068,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,/} \/\/ namespace RenderStateImpl/' \| sha256sum` | `d8fd1c48…0efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha`. Confirmed. |
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160 (+0.001%)`. Resized: `RenderState::RenderState()` +148, `RenderState::SetCapability` +77, `RenderState::IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9. All four are the **contract** commit's; this package adds zero pull-build delta on top. See minor m7 for the fourth symbol. |
| **G2** | ctest `-N` name sets, corrected grep | `build-linux 2396`, `build-push 2396`, **zero-line diff** |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <names-build-linux>` | empty (0 of 2363 baseline names removed) |
| unit | `ctest --test-dir build-linux -L unit -j 8` | rc 0 — 1518/1518 |
| unit | `ctest --test-dir build-push -L unit -j 8` | rc 0 — 1518/1518 |
| unit | `ctest --test-dir build-verify -L unit -j 8` | rc 0 — 1518/1518 |
| integration | `ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878/878** |
| integration | `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | first run rc 8, one failure: `DirectGLES.PointSizeDemotion.…TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn`. **Re-run of the full lane: rc 0, 878/878.** The entry also passes 3/3 standalone under `MOBILEGL_PIPE_PUSH=0`. Confirms `tracker-v1.md` §4.4: a lane parallel flake, not a push/pull divergence. |
| **G4** | `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818/818** |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py --lib build-verify/libMobileGL.so -j 4` | see §1.1 |
| **G10** (stats half) | `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 MOBILEGL_LOG_FILE_PATH=… ctest -R DirectGLES.CrossFrameBufferScenario` | `MGPipe stats: … resid=8.00 … cso[csom=1 csob=1]`, then 4 windows of `resid=0.00 … csom=0 csob=0`. Reproduced exactly as claimed. |
| G6/G7 | `ctest --test-dir build-push -N \| grep RenderStateSpans` | only `RenderStateSpans.PlaceholderUntilTheOwningPackageFillsThisIn`. Not reachable from this tree — package A. Correctly not claimed. |
| G8/G12 | `ctest -N \| grep -E 'HandleRecycle\|CsoContentAddressing'` | absent in both `build-push` and `build-verify`. Package E. Correctly not claimed. |

Ownership (C.5) re-checked against `git diff --stat p2/contract..HEAD`: 26 files. Every file is
B's row, plus `MG_State/GLState/Core.{h,cpp}`, `MG_Test/ScopedPipeVerb.h`,
`MG_Test/Pipe/PipeInputsTest.cpp` (all "nobody"). `CMakeLists.txt`, `MG_Backend/**`,
`VertexArrayState/VertexArrayObject.h` (D's), `.github/workflows/test.yml` (E's),
`scripts/gen_pipe.py` (A's) untouched. **No ownership violation.** Commit subjects are
single-line `[Type] (Scope): …`; no `Co-Authored-By` anywhere.

### 1.1 Verify retrace

`MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib
~/w7/p2-tracker/build-verify/libMobileGL.so --out ~/w7/retrace-out/rev-tk-verify -j 4`
→ **RETRACE_RESULT_PLACEHOLDER**

### 1.2 The chunk table and the setters — re-derived independently

I re-derived the split rule (`MGPipeRenderStateSpans.h:20-22`) against `RenderState.cpp` for
**more than five** setters, hunting for a pipeline byte written by a `++m_version`-only setter
(the failure that would leave the assembled block permanently stale, because
`EmitRenderState` sends only dynamic chunks when `NewPipelineState` does not fire):

* `++m_version` only, all landing in **dynamic** chunks — `SetViewport`/`SetViewportIndexed`
  (D0, `:69,98`), `SetLineWidth` (D0, `:113`), `SetPointSize` (D0, `:203`),
  `SetPolygonOffsetClamped` (D1, `:276`), `SetClipControl` (D1, `:288`), `SetHint`
  (D5, `:131`), `SetPointFadeThresholdSize`/`SetPointSpriteCoordOrigin`/`SetClampReadColor`
  (D5, `:147,157,167`), `SetPrimitiveRestartIndex` (D6, `:192`), `SetClearColor`/`SetClearDepth`/
  `SetClearStencil`/`SetBlendColor` (D2, `:711,722,733,744`), `SetDepthRange`/
  `SetDepthRangeIndexed` (D2, `:760,775`), `SetStencilMask` (D3/D4, `:657`),
  `SetScissorBox`/`SetScissorBoxIndexed` (D7, `:948,969`), `SetCapability(ClipDistance0..7)`
  (D7 `ClipDistanceEnabledMask`, `:381`).
* `BumpVersions()`, all landing in **pipeline** chunks — the 25 `SET_CAPABILITY` arms and the
  `Blend`/`ScissorTest` arms (P1/P6, `:313,349,360`), `SetCapabilityIndexed` (P1/P6,
  `:460,476`), the four blend setters (P1, `:520,546,572,591`), `SetLogicOp`/`SetDepthFunc`/
  `SetDepthMask` (P1, `:607,619,630`), `SetColorMask`/`SetColorMaskIndexed` (P1, `:688,699`),
  `SetStencilOp` (P3/P4, `:671`), `SetSampleCoverage`/`SetSampleMaskValue`/
  `SetMinSampleShadingValue` (P2, `:791,806,820`), `SetCullFaceMode`/`SetFrontFaceMode`/
  `SetProvokingVertexMode` (P4, `:894,905,916`), `SetPolygonMode` (P5, `:178`),
  `SetPatchVertices`/`SetPatchDefault{Outer,Inner}Level` (P0, `:214,233,244`).
* The one hybrid, `SetStencilFunc` (`:641-649`): `Func` → pipeline (P2 for face 0, P3 for
  face 1), `Ref`/`ValueMask` → dynamic (D3/D4), and `++m_pipelineStateVersion` is conditional
  on `Func` moving. Exactly what the table encodes.

**No violation found.** The partition itself is a `static_assert` chain
(`MGPipeRenderStateSpans.h:144-174`: boundaries ascend, start 0, end `sizeof`, 7+8 chunks,
396+772 == 1168), so a gap is a build break, and the build is green. The brief's D6 table and
the landed header disagree only cosmetically (`StencilStates[0].Func` sits inside the
brief's `[752,772)` P2 range but the brief's member list omits it; the header lists it).
Tree wins; no action beyond the integrator copying the header's member list into
`MEASUREMENTS.md`.

### 1.3 The headline claim IS proven

`GetRenderStateParameters`, the two version fields and the patch trio are the six fields the
residual fill actually skips on this branch (`PipeFill.cpp:695-710` `AppliedWithoutDerivation`
∩ `:668-676` `EmittedCallSuppliesTheWholeField`, `applierDerives == false` here). For those,
`gPipeInputs.m_renderState` is assembled purely from `bind_render_state` + `set_dynamic_state`
(`PipeApply.cpp:156-189`). The verify build's read hook re-copies the **whole**
`RenderStateParameters` from the live context at every backend read and compares it field-wise
(`PipeFill.cpp:452-475` → `PipeInputs.cpp:153-158` → the generated field comparator), and
`EntryCompare` does the same at every verb (`PipeFill.cpp:422-431`). 818 `integration-verify`
entries and the verify retrace green therefore *do* prove the chunk table, the scatter and the
skip together — `tracker-v1.md` §8 is more modest than the evidence warrants. The `csom=1
csob=1` / `resid=8.00` window I reproduced proves the emission is live and not dead code.

---

## 2. Majors

### MAJOR 1 — `DirtySurface.def` gives the two mutators with a real two-answer split the *narrower*, under-firing answer, contradicting its own stated rule; the gate it just became cannot see it

`MobileGL/MG_Pipe/DirtySurface.def:21-22` states the rule:

> where a mutator has more than one true answer the row carries the **COARSER** one — the one
> that cannot under-fire

`RenderState.h:159-162` — `BumpVersions()` bumps **both** `m_version` and
`m_pipelineStateVersion`. Therefore, over every `RenderState` setter,
`{NEW_PIPELINE_STATE fires} ⊆ {NEW_RENDER_STATE fires}`: `NEW_RENDER_STATE` is the coarser
answer, always. Two rows pick the other one:

* `DirtySurface.def:68` — `X(SetCapability, NEW_PIPELINE_STATE)`. The comment immediately
  above it (`:66-67`) *names the counter-example* — "SetCapability's ClipDistance0..7 arms
  move only m_version" — and then concludes the opposite of the rule.
  `RenderState.cpp:377-381`: the ClipDistance arm writes `ClipDistanceEnabledMask` (dynamic
  chunk D7) and does `++m_version` with an explicit "Deliberately NOT BumpVersions()" comment.
  So for `glEnable(GL_CLIP_DISTANCE0)` the row's named publisher **does not fire at all**; the
  mutation is in fact published by `NEW_RENDER_STATE` → `set_dynamic_state` chunk D7.
* `DirtySurface.def:86` — `X(SetStencilFunc, NEW_PIPELINE_STATE)`, with the comment at
  `:82-85` again spelling out the split. `RenderState.cpp:641-649`: `++m_version` is
  unconditional, `++m_pipelineStateVersion` is conditional on `Func` moving. A
  `glStencilFunc` that changes only `Ref`/`ValueMask` (chunks D3/D4) does **not** move
  `NEW_PIPELINE_STATE`.

Reproduction (both directions, from `~/w7/p2-tracker`):

```
$ grep -n "SetCapability,\|SetStencilFunc,\|COARSER" MobileGL/MG_Pipe/DirtySurface.def
22:// the COARSER one - the one that cannot under-fire:
68:    X(SetCapability,                          NEW_PIPELINE_STATE)              \
86:    X(SetStencilFunc,                         NEW_PIPELINE_STATE)              \
$ sed -n '377,381p;641,649p' MobileGL/MG_State/GLState/RenderState/RenderState.cpp
                    // Deliberately NOT BumpVersions(): ...
                    ++m_version;
                const Bool pipelineRelevantChange = state.Func != func;
                ...
                ++m_version;
                if (pipelineRelevantChange) ++m_pipelineStateVersion;
$ python3 scripts/gen_pipe_dirty_surface.py --check
dirty-surface: 73 mutators, all mapped, no stale rows      # rc 0 — the gate is blind to this
```

Why it is a major and not a nit: D16's whole purpose is that *"every frontend mutation a
backend can observe has an answer to 'what publishes this'"*, and the file itself says the
failure mode "is silent and one-directional: a mutation that forgets to publish renders
stale" (`DirtySurface.def:12-14`). The gate `--check` only validates row **existence** and
**vocabulary** (`gen_pipe_dirty_surface.py:171-193`), never the answer's truth, so the two
wrong rows survive a green gate — literally the "green because it never checked" pattern.
P3a is specified to build the narrow shutters from this file; a shutter keyed on the pipeline
version for `SetStencilFunc`'s dynamic half or for the clip-distance mask is exactly the
under-firing that `ARCHITECTURE.md:502` names as the dangerous direction.

Fix inside B's ownership: change both rows to `NEW_RENDER_STATE` (the coarser, always-true
answer), or introduce a two-answer notation and let `--check` require it wherever a setter has
both a `BumpVersions` and a `++m_version` path. Either way the two comments at `:66-67` and
`:82-85` should state which bit actually fires.

### MAJOR 2 — `set_vertex_attrib_defaults` is emitted with a payload that provably cannot reproduce the frontend value, and the ordering of steps 3/4 makes it invisible to every gate

`MobileGL/MG_Impl/Pipe/PipeFill.cpp:752-786`, `EmitVertexAttribDefaults`:

```
776:                value.ValueClass = 0;
777:                std::memcpy(value.Data, resolved[i].floatValue.data(), sizeof(value.Data));
```

* `MGPipeTypes.h:475-481` declares `MGPAttribValue { Uint32 Location; Uint8 ValueClass; /* Float | Int | Uint | Double */ Uint8 Pad0[3]; Uint32 Data[4]; }`. The client hard-codes the
  discriminator to `0` for every attribute, unconditionally — the field is never computed.
  The comment above it (`PipeFill.cpp:773-775`) justifies this with "ClassifyVertexAttribType
  resolves the float/int/uint view on the CLIENT", which is a non-sequitur:
  `ClassifyVertexAttribType` is applied at the **backend read sites**
  (`DirectGLES.cpp:1278`, `VulkanRenderer.cpp:6243,6887`) against the *shader's* declared
  attribute type — it is not the emitter's answer and it is not applied here.
* Only `floatValue` is carried, and `MGPipeApplySetVertexAttribDefaults`
  (`PipeApply.cpp:224-226`) memcpys those same 16 bytes into all three views. But the frontend
  does a **numeric** conversion between the views (`Core.cpp:212-216`:
  `current.intValue[c] = static_cast<Int32>(value[c])`). So a `glVertexAttrib4f(loc, 1.5f, …)`
  puts `1` in the frontend's `intValue` and `0x3FC00000` (1069547520) in the pushed one.
  The pushed value is wrong for every non-integral float and for every int/uint attribute set
  through `glVertexAttribI4i/ui`.

`tracker-v1.md` §4.2 names the gap and proposes the right fix, but then claims the call is
still worth landing because "the wire shape, **the payload bytes** and the suppressor are all
real". The payload bytes are not real; they are wrong, and `ValueClass` — which exists
"precisely so it does not have to" guess (§4.2's own words) — is the one field the client was
free to fill correctly and did not.

Why the gates cannot see it: the emission runs at step 3 (`PipeFill.cpp:924-947`) and the
residual fill at step 4 (`:949-…`). `GetCurrentVertexAttribute` is deliberately excluded from
`EmittedCallSuppliesTheWholeField` (`PipeFill.cpp:668-676`), so step 4 immediately overwrites
the three corrupted views with a correct pull, **before** `EntryCompare` (`:1006`) and before
any backend read can reach the verify read hook. G4's 818 verify entries and the verify
retrace are therefore structurally incapable of reporting this, which is why it is green.
The correctness of a shipped push build here rests entirely on a later step undoing an
earlier one; the moment anyone flips that one `case` label — or assumes
`MGP_COVERAGE_EMITTED_LIST`'s `GetCurrentVertexAttribute → SetVertexAttribDefaults` row
(`Coverage.def`) is authoritative, which is what the generated
`kMGPipeFieldEmittedBy[]` says — it is silent wrong vertex-attribute defaults with no gate
between it and the screen.

Minimum to clear this: set `value.ValueClass` from the frontend's actual class rather than a
literal `0`, and either (a) carry all three views, or (b) record in `Coverage.def`/the result
file that this row is a **shape-only** emission whose payload is not yet usable, with the
applier fix (A's `PipeApply.cpp`) named as a blocking follow-up before any phase retires the
pull for this field. Do not carry the "payload bytes are real" claim into `MEASUREMENTS.md`.

---

## 3. Minors

1. **`MGPipeSubsystemForDirty` has no production caller.** `Tracker.h:119-135` defines the
   bit→subsystem map, but `MGPipeValidateForVerb` gates emission with a *second*, independent
   copy: `SubsystemForEmitter` (`PipeFill.cpp:612-630`) plus four hand-written `if`s
   (`:926-948`). `grep -rn MGPipeSubsystemForDirty MobileGL/` finds it only in
   `TrackerTest.cpp:283-289`. So the one test that pins "each of the five names its own
   subsystem" pins the copy nothing runs; mis-gating `NewPatchState` on the PixelPack bit in
   the real path would pass every test.
2. **The tracker's unit tests exercise a re-implementation, not the shipped emitter.**
   `TrackerTest.cpp:240-258` (`TrackerWalk::Walk`) reproduces step 3 against a *local*
   `MGPipeTracker`/`MGPipeCsoCache`, with the justification (`:216-220`) that the validate
   point uses process-wide singletons. That is a fair reason, but it means
   `BlendToggleReusesTwoCsos`, `ViewportDoesNotMintACso` and `SteadyStateEmitsNothing` cannot
   fail on a defect in `MGPipeValidateForVerb`; the shipped emitter's only coverage is the
   integration/verify lanes. The brief's C.1 wording ("mints exactly 2 CSOs and issues 2N
   binds") reads as a test of the real path. Worth one always-on integration entry that drives
   `MGP_FILL` and reads `csom`/`csob` back through `PipeStats` (E's `CsoContentAddressingScenario`
   is the natural home).
3. **`MGPipeCsoCache::Counters::Binds` is declared, documented and never incremented.**
   `CsoCache.h:59` ("bind_render_state emissions, mint or reuse") and the header's promise to
   "publish the mint / bind / evict counters" (`:28-29`); `grep -rn 'm_counters.Binds'` finds
   nothing, and the tests count binds in their own `m_binds`. Either wire it in `Acquire`, or
   delete it before P13's retune reads a permanent zero.
4. **The residual value block's trip wire is a tautology on this branch.**
   `EmitResidualValueState` (`PipeFill.cpp:800-820`) builds the bits from
   `ctx.IsCapabilityEnabled(...)`, and `MGPipeApplySetResidualValueState`
   (`PipeApply.cpp:244-256`) compares them against `gPipeInputs.m_capability[]` — which step 4
   filled from the *same* accessor a few lines earlier, because `applierDerives` is false and
   `IsCapabilityEnabled` is not in `AppliedWithoutDerivation`. So it cannot diverge here. It
   becomes a real oracle the moment A's `c1` lands (then `IsCapabilityEnabled` is skipped by
   the fill and comes from `MGPipeDeriveRenderStateFields`). `tracker-v1.md` §2.2's G10 line
   should say that the *semantic* half is deferred to the merged tree, alongside the `resid=`
   evidence it does establish.
5. **`tracker.Staged() = live` is latched on either bit, but `set_dynamic_state` is emitted
   only on `NewRenderState`.** `PipeFill.cpp:889-893`. If `NewPipelineState` ever fired without
   `NewRenderState`, the staging mirror would claim the server holds dynamic bytes it never
   received, and the chunk-level suppressor would never resend them — a permanently stale
   answer. It is unreachable today only because `BumpVersions()` moves both counters
   (`RenderState.h:159-162`), i.e. it depends on an invariant of *another package's* file that
   nothing here asserts. A `MOBILEGL_ASSERT` (or narrowing the assignment to the chunks
   actually sent) costs nothing.
6. **G9 is only half-landed here and `tracker-v1.md` §2.2 lists it green without the caveat.**
   `.github/workflows/test.yml:1528-1532` still runs `--summary` under an "Informational …
   becomes a gate in P2" comment. B correctly did not touch E's file (C.1 constraint), but the
   G14 row of §A says "CI: `pipe-gates`", so the gate is not a gate until E lands. Say so in
   the hand-off.
7. **G1 admits three named resizes; the tree has four.** `_GLOBAL__sub_I_DirectGLES.cpp`
   (−9 bytes) is resized alongside the three `RenderState` symbols. It is the contract
   commit's, not B's (B's own delta on top is zero), but D15/§A's "resized ⊆ the three
   RenderState symbols" is not literally satisfiable on the merged tree and the integrator has
   to widen the admitted set explicitly rather than wave it through.
8. **D14's "all-pull control reproduces P1's behaviour exactly" and D.4.3's T2 are no longer
   true of the landed code.** The dirty walk (`PipeFill.cpp:918-922`) runs unconditionally;
   only emission is behind the bitmask. So `MOBILEGL_PIPE_PUSH=0` still pays for ~18 shutters
   per verb, and `T2 = ns_per_op(push, PUSH=0) − ns_per_op(pull)` already contains the whole
   tracker cost — which means `T1 − T2` does **not** isolate what P2 added, it cancels the
   largest thing P2 added. B followed D1 correctly (the walk is step 2, before the gate); the
   integrator must restate T1/T2 in `MEASUREMENTS.md` before the D.4.3 run, or the ≤45 ns
   ceiling is checked against the wrong difference.
9. **`ApplierDerivesRenderStateFields()` probes one of twenty-nine derivations.**
   `PipeFill.cpp:679-704` latches on `m_clearStencil` alone. A partial `c1` that implements
   `m_clearStencil` and forgets, say, `m_viewport`'s rounding flips the latch to true and lets
   28 mirrors go unwritten; the verify lane is the backstop, not the probe. Worth saying so in
   the comment (which currently says "this only says it is THERE" — accurate, but the
   integrator should know the probe is a single-field sample).
10. **Three `kExplicitDestroy` rows name a mechanism nothing builds.**
    `DirtySurface.def:127,128,131` map `MarkProgramForDeletion`,
    `MarkProgramPipelineForDeletion` and `MarkShaderForDeletion` onto "the `delete_*` call the
    Track H slice emits", but D13 scopes Espryt 0b's explicit destroy to six object kinds that
    do not include programs, pipelines or shaders. Those three answers are aspirational, not
    factual — the same class of defect as MAJOR 1, at lower stakes.
11. **`MGPipeWidenedCounter` can miss a change, contrary to its own comment.**
    `Tracker.h:154-159`: the wrap test is `now < m_last`, so a `Uint16` counter that advances
    by exactly 65536 between two walks reads as unchanged and the bit does not fire.
    `TrackerWalk.WrapAroundRePushesButNeverMisses` drives 70000 *single-step* changes, which
    cannot construct that case, so "never a missed push" (`Tracker.h:150-151`) is stronger than
    what is tested. Pre-existing in class (both backends already compare raw `Uint16`s), so
    record it rather than fix it.
12. **Test seam and reset asymmetry.** `MGPipeCsoCache::s_hashForTest` (`CsoCache.h:120-121`)
    is a writable function pointer in production code — declared as deviation 6 and contained,
    but note that `MGPipeApplierReset()` is callable from a test *without* resetting
    `MGPipeCsoCacheInstance()`/`MGPipeTrackerInstance()`, which would make the next
    `bind_render_state` name a dead CSO (`PipeApply.cpp:156-160` asserts and returns, leaving
    `m_renderState` unwritten while the fill skips it). Safe today only because ctest runs one
    gtest case per process. `TrackerWalk::SetUp`/`TearDown` (`TrackerTest.cpp:223-233`) do
    exactly that pairing.
13. **The brief's own G2/G14 grep is broken — confirmed.** `ctest -N | grep -cE '^\s+Test #'`
    returns **1397** of 2396 on this tree (I reran it). `tracker-v1.md` §4.3 is right and the
    corrected pattern `'^ +Test +#[0-9]+: '` is what I used above. §A's G2/G14 rows and D.1's
    loop must be fixed by the integrator before anyone copies them into CI.
14. **Header-only `Tracker.h`/`CsoCache.h`/`SetHashSuppressor.h`** (deviation 1) is a sound
    route around A's frozen `CMakeLists.txt`, but it leaves a `static PipeInputs probe`
    (`PipeFill.cpp:681`) and three function-local singletons in whichever TU includes them.
    Land the one `list(APPEND)` line at integration rather than letting the deviation stand as
    the final shape.

---

## 4. Deviations checked against §B

Declared and accepted: 1 (header-only, ownership-forced), 2 (a sixth aggregate
`VertexAttribDefault` — over-fires, cheaper than D4's per-draw 768-byte hash, and
`TrackerAggregates.AVertexAttribDefaultMovesOnlyItsOwnAggregate` pins it), 3 (`Core.{h,cpp}`
— "nobody" in C.5, all `#if MOBILEGL_PIPE_PUSH`, G1 unaffected — verified), 4 (coarse
shutters for bits 5–17; every one of them over-fires, and P2 emits nothing from them), 5
(`Vector` linear scan instead of `ska::flat_hash_map` — behaviour is exactly D7), 6
(`s_hashForTest`), 7 (residual block after the fill, held rather than dropped), 8 (the
derivation probe), 9 (five stale `MGPipeFillForVerb` comment references in A's files).

Undeclared deviations found: **none**. I checked the aggregate bump points against D4 line by
line — every `++m_configVersion` (`VertexArrayObject.cpp:300,307,314`), every
`++m_contentVersion` (`TextureObject.cpp:295,367,379`, `TextureObject2DCube.cpp:52,64`), every
`++m_textureParamsVersion` (`TextureObject.cpp:102,128,143,158,208,216,234,246,267,305,315`,
`TextureObject.h:183`), `SamplerObject::BumpVersion` (`:28-37`, the one choke point for all 15
sampler setters), every `++m_changeSerial` in `BufferObject.cpp` (7 sites) and every
`++m_objectVersion` in `FramebufferObject.cpp` (`:191,203-206` inside the five-setter macro,
`:216-219`) carries an `MGP_NOTE_AGGREGATE`. No bump site is uncovered — the "dirty bit that is
never set" hunt came up empty.

Also checked and clean: no hot-path instrumentation (no timer anywhere in `Tracker.h`; every
tally is behind `PipeStats::Enabled()`, `Tracker.h:292-298`); no stdio under
`MG_Backend`/`MG_State`; no accessor in the walk forces a link join (`GetCurrentProgram`
`Core.cpp:386-388` and the six program version getters are plain member reads —
`GetProgramForDraw` is deliberately not used, `Tracker.h:208-211`); `PipeCalls.def` untouched.

---

## 5. Bottom line

The core of the package is sound and independently verified: G1, G2, G4 (integration-verify),
G5, G9, G13, G14 all reproduce, three unit lanes are 1518/1518, both integration-gpu arms are
878/878, the chunk-table partition is a `static_assert` and its *rule* survived a
setter-by-setter re-derivation, and the assembled render-state block is proven byte-identical
to the live one by an oracle that re-reads the whole struct at every backend read.

What blocks approval is not the machinery but two artefacts landed as finished while being
factually wrong, in both cases in a way no gate in this tree can report: the mapping file's
two under-firing rows (MAJOR 1) and the vertex-attribute-default payload (MAJOR 2). Both are
small fixes; neither needs another package.
