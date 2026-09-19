# P2 package B — `p2/tracker` — result, rework round 2

Tree `~/w7/p2-tracker`, branch `p2/tracker`, branched from the contract commit `9c6a8a25`
(the `p2/contract` tag). Not pushed. This file supersedes `tracker-v1.md`.

Round-2 scope: both majors and every minor of `tracker-review-v1.md`. Nothing from round 1 was
reverted; the four new commits sit on top of `7993d711`.

---

## 1. Commits

Round 1 (`9c6a8a25..7993d711`, unchanged):

| sha | subject (abbreviated) |
|---|---|
| `f52dd262` | `[Feat] (State): give the frontend six aggregate generations …` |
| `9c9df515` | `[Feat] (Pipe): compute the per-verb dirty mask at the validate point …` |
| `8d000f7e` | `[Feat] (Pipe): mint render-state CSOs on the pipeline subset …` |
| `a0c44c4f` | `[Feat] (Pipe): push pixel-pack, patch and vertex-attribute-default state …` |
| `e54e7399` | `[Feat] (Pipe): carry what has no call of its own in the residual value block …` |
| `e49f0ea7` | `[Feat] (Pipe): map every frontend mutator onto the aggregate generation …` |
| `b3daa404` | `[Test] (Pipe): pin the tracker's shutters and the CSO cache's content addressing …` |
| `7993d711` | `[Test] (Pipe): give the pull build the same ctest names as the push build …` |

Round 2 (`7993d711..dc452c02`, new):

| sha | subject | closes |
|---|---|---|
| `57b85f73` | `[Fix] (Pipe): derive the render-state answers of the dirty-surface map from RenderState.cpp instead of believing them, and correct the two rows that named a publisher which does not always fire` | **MAJOR 1**, minor 10 |
| `80b861b8` | `[Fix] (Pipe): count the render-state CSO binds the cache has always declared and never incremented` | minor 3 |
| `d5d4e757` | `[Fix] (Pipe): carry the class a vertex-attribute default was written through, and stop the applier's lossy write from being observable` | **MAJOR 2**, minors 1, 2, 5, 9, 11, 12 |
| `dc452c02` | `[Docs] (Pipe): say what actually fails to publish a program, a pipeline and a shader dying, instead of naming a scope` | minor 10 (sharpened) |

Round-2 diffstat: 9 files, +746 / −148.

```
MobileGL/MG_Impl/Pipe/CsoCache.h        |   9 +-
MobileGL/MG_Impl/Pipe/PipeFill.cpp      | 159 ++++++--
MobileGL/MG_Impl/Pipe/PipeFill.h        |  13 +
MobileGL/MG_Impl/Pipe/Tracker.h         |  34 +
MobileGL/MG_Pipe/DirtySurface.def       | 242 +++++++------
MobileGL/MG_State/GLState/Core.cpp      |  11 +
MobileGL/MG_State/GLState/Core.h        |  39 +
MobileGL/MG_Test/Pipe/TrackerTest.cpp   | 209 +++++++++-
scripts/gen_pipe_dirty_surface.py       | 178 +++++++--
```

Whole package: 26 files, +2857 / −78. Every round-2 file is one B owns in C.5 except
`MG_State/GLState/Core.{h,cpp}`, which the table gives to **nobody** (round-1 deviation 3).
No file under `MG_Backend/`, no `CMakeLists.txt`, no A/E file was touched.

---

## 2. MAJOR 1 — the dirty-surface map's render-state answers

**What was wrong.** `X(SetCapability, NEW_PIPELINE_STATE)` and
`X(SetStencilFunc, NEW_PIPELINE_STATE)` named a publisher that does not fire on every path:
`RenderState.cpp:377-381`'s ClipDistance0..7 arms do `++m_version` with an explicit
"Deliberately NOT BumpVersions()", and `:641-649` makes `++m_pipelineStateVersion` conditional
on `Func` moving. `--check` validated row existence and answer vocabulary only, so both rows
survived a green gate.

**What landed** (`57b85f73`):

1. **The rule is restated and the two rows corrected.** A row now lists *every* publisher that
   fires on *every* path, joined with `|`; a publisher that fires on some paths only must not
   appear. `SetCapability` and `SetStencilFunc` are `NEW_RENDER_STATE`. The 18 setters that
   call `BumpVersions()` on every path carry `NEW_RENDER_STATE|NEW_PIPELINE_STATE`, and the
   patch trio carries `NEW_PATCH_STATE|NEW_RENDER_STATE|NEW_PIPELINE_STATE` (it calls
   `BumpVersions()` too, and the trio really does travel in pipeline chunk P0).
2. **`--check` now derives the answer instead of accepting it.**
   `render_state_publishers()` reads `RenderState.cpp`, walks every `RenderState::Set*` body
   and computes which counter moves on every path — `BumpVersions()` moves both, a bare
   `++m_version` moves only `NEW_RENDER_STATE`, a body with both kinds of path always-fires
   only `NEW_RENDER_STATE`, and a body with no bump inherits from the setter it delegates to
   (`SetPolygonOffset` → `SetPolygonOffsetClamped`). A row that claims a publisher the
   derivation does not find is `UNDER-FIRING`; one that omits a publisher it does find is
   `MISSING publisher`. 45 rows are checked this way.
3. **Two more self-test negative controls**, one per direction, built from the two defects
   that were actually in the file.

Evidence (all re-run):

| command | output |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `5 negative controls, all tripped` |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0 — still works (E's CI step is unbroken) |
| end-to-end control, file edited to the OLD answer (`SetStencilFunc` ← `…|NEW_PIPELINE_STATE`) | rc 1 — `UNDER-FIRING answer NEW_PIPELINE_STATE for SetStencilFunc … (derived: NEW_RENDER_STATE)` |
| end-to-end control, other direction (`SetDepthMask` ← `NEW_RENDER_STATE`) | rc 1 — `MISSING publisher NEW_PIPELINE_STATE for SetDepthMask … (derived: NEW_PIPELINE_STATE|NEW_RENDER_STATE)` |

The derived table was cross-checked by hand against the review's own setter-by-setter list
(§1.2 of `tracker-review-v1.md`); it agrees on all 45, including the two disputed rows.

---

## 3. MAJOR 2 — `set_vertex_attrib_defaults`' payload

**What was wrong.** `PipeFill.cpp` hard-coded `value.ValueClass = 0` and always sent the
**float** view's four words. `GLContext` converts numerically between the three views, so those
bytes cannot reproduce the value (`glVertexAttrib4f(loc, 1.5f, …)` → frontend `intValue` 1,
pushed word `0x3FC00000`), and every `glVertexAttribI4i/ui` default was wrong. No gate could
see it, because the residual fill re-pulls the field at every `kDraw` verb *after* emission —
while at a non-`kDraw` verb nothing put the value back at all.

**What landed** (`d5d4e757`):

1. **The class is recorded and carried.** `GLContext::GetCurrentVertexAttributeClass(index)`
   (push-only) says which of the three views the last `glVertexAttrib*` write filled directly;
   `MGPipeFillAttribValue` (Tracker.h) puts that class in `MGPAttribValue::ValueClass` and that
   class's own four words in `Data`.
2. **Where the class lives, and why not in the value.** It is a parallel array on `GLContext`,
   not a member of `CurrentVertexAttributeValue`: **the tree refused the obvious shape** —
   `MG_Backend/MGPipe/PipeInputs.cpp:96-99` compares that storage with one `memcmp` behind
   `static_assert(sizeof(...) == 3*4*4, "…grew a member; update the comparator")`, and
   `MG_Backend/**` is a file class B must not touch (C.1). The first attempt broke
   `build-verify` on exactly that assertion. The class need not be mirrored anyway: it only
   decides how the three views are *rebuilt*, so two writes that leave the views identical
   rebuild identically whichever class they carried — which is also why the emitter's
   suppressing `memcmp` over the three views alone stays correct.
3. **The applier's lossy write is no longer observable.** `MGPipeApplySetVertexAttribDefaults`
   (package **A**'s `PipeApply.cpp:224-226`) still `memcpy`s the four words into all three
   views. So after the call the emitter compares the mirror against the frontend's value byte
   for byte over the attributes the call named and, when they differ, copies the field itself
   and warns once — the same self-healing shape as `ApplierDerivesRenderStateFields`. That
   closes the window the old code left wrong and stops by itself once A's applier switches on
   `ValueClass`.
4. **It is testable.** `MGPipeVertexAttribDefaultRepairCount()` (PipeFill.h) is the observable:
   reading the storage in that window would be the poison violation `FillPoints.def` forbids,
   so the count is what the test asserts.
5. **The row is recorded as shape-only.** The comment on
   `EmittedCallSuppliesTheWholeField` now says the pull for `GetCurrentVertexAttribute` is
   blocked on A teaching the applier to honour `ValueClass`; the "payload bytes are real"
   claim of `tracker-v1.md` §4.2 is withdrawn (see §7, blocking follow-up 1).

New tests (7), all in `MG_Test/Pipe/TrackerTest.cpp`, with pull-build SKIP twins:

- `TrackerAttribPayload.AFloatWriteCarriesTheFloatBitsAndNamesItsClass`
- `TrackerAttribPayload.AnIntWriteCarriesTheIntWordsAndNamesItsClass`
- `TrackerAttribPayload.AUintWriteCarriesTheUintWordsAndNamesItsClass`
- `TrackerAttribPayload.TheSameNumbersWrittenThroughADifferentClassAreADifferentValue`
- `TrackerShippedEmitter.APushedAttributeDefaultTheApplierCannotReproduceIsRepaired`
- `TrackerShippedEmitter.{ABlendToggleThroughTheValidatePointMintsTwoCsos,
  TheSteadyStateThroughTheValidatePointEmitsNothing, AViewportThroughTheValidatePointMintsNoCso}`

Negative controls run for these, not just asserted:

| control | result |
|---|---|
| restore the old payload (`ValueClass = 0`, always the float view) | **3 of the 4 `TrackerAttribPayload` cases go red** |
| disable the render-state emission at the validate point | `TrackerShippedEmitter.ABlendToggle…` goes red (the other two are "nothing was minted" assertions and stay green, which is why both now `ASSERT` that the priming draw minted and bound exactly one CSO first) |

---

## 4. The minors

| # | verdict |
|---|---|
| 1 `MGPipeSubsystemForDirty` had no production caller | **fixed.** Step 3 gates every emission through `wants(bit)` → `MGPipeSubsystemForDirty`, and five `static_assert`s tie that map to `SubsystemForEmitter`, the field-side map it must agree with. The four hand-written subsystem names are gone. |
| 2 unit tests exercised a re-implementation | **fixed.** `TrackerShippedEmitter` drives `MGPipeValidateForVerb` itself and reads the real singletons (`MGPipeCsoCacheInstance().GetCounters()`, `MGPipeTrackerInstance().LastDirty()`). `TrackerWalk` is kept as the cheap shutter-level fixture. |
| 3 `Counters::Binds` never incremented | **fixed** in `Acquire`, which has exactly one caller and is followed by a bind every time; the comment says so. `TrackerShippedEmitter` now asserts on it. |
| 4 residual trip wire is a tautology on this branch | **recorded in the code** (`EmitResidualValueState`) and in §5 below: the semantic half is deferred to the merged tree; what this branch proves is emission, size and suppression. |
| 5 `Staged()` latched on either bit | **fixed.** The mirror is advanced only by the `NewRenderState` branch, and the invariant it used to rely on (another package's `BumpVersions`) is now a `MOBILEGL_ASSERT` rather than an assumption. |
| 6 G9 is only half a gate until E lands | **recorded**, §5. `.github/workflows/test.yml` is E's file and was not touched. |
| 7 four resized symbols, not three | **recorded**, §5. All four are the contract commit's; B's own pull-build delta is still zero. |
| 8 D14 / D.4.3's T1−T2 no longer isolate the tracker | **recorded**, §5, for `MEASUREMENTS.md`. |
| 9 the derivation probe is a one-field sample | **recorded in the code**, at the probe. |
| 10 three `kExplicitDestroy` rows name a mechanism nothing builds | **fixed**: they answer `kUnpublishedDestroy`, a new documented answer. The reason given is the tree's, not the brief's — see §6.2. |
| 11 `MGPipeWidenedCounter` can miss a change of exactly 65536 | **recorded at the code**, as the review asked; not fixed (it would mean widening MG_State's counters, which ARCHITECTURE.md 5.2 rules out). |
| 12 test-seam / reset asymmetry | **fixed** for the reset half: `ResetTheServerSideSingletons()` resets applier + cache + tracker + suppressor together in both fixtures' `SetUp`/`TearDown`. `s_hashForTest` stays (declared deviation 6). |
| 13 the brief's G2/G14 grep is broken | **recorded**, §6.1; the corrected pattern is used throughout. |
| 14 `retrace_gate.py` has no `--ssim` | **recorded**, §6.1 (reproduced: `unrecognized arguments: --ssim 0.99`). |
| 15 G4's log greps inspected zero files | **fixed in this run**: corrected path *and* corrected arming string, numbers in §5. |
| 16 header-only Tracker/CsoCache/SetHashSuppressor | **stands** as declared deviation 1; §7 asks the integrator to land the one `list(APPEND)` line. |
| lane flake | **reproduced and re-established**, §5. |

---

## 5. Verification — every command and its actual output

All commands run in `~/w7/p2-tracker` at `dc452c02`. Three other packages' lanes were running
on the same machine for part of this (see the flake row).

### 5.1 Generators and gates

| command | output |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `5 negative controls, all tripped` |
| `python3 scripts/gen_pipe.py --check` | rc 0 — `63 PipeInputs fields (7 sticky, 34 emitted by a P2 call)`, `generated files are up to date` |
| `python3 scripts/gen_pipe.py --self-test` | rc 0 — `7 negative-control trip(s), positive control OK` |
| `python3 scripts/check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |

### 5.2 Section-A gates reachable from this tree

| gate | command | output |
|---|---|---|
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160 (+0.001%)`. Identical to round 1 — **round 2 adds zero pull-build delta**; the four resized are the contract's (minor 7). |
| **G2** | `ctest -N` name sets, pattern `'^ +Test +#[0-9]+: '` | `build-linux 2404`, `build-push 2404`, **zero-line diff** |
| **G3** | `retrace_gate.py --lib build-push/libMobileGL.so -j 4` | rc 0 — **79 / 79**, lowest SSIM **0.993475** (two cases), then 0.995426 |
| **G4** | `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818 / 818** |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py --lib build-verify/libMobileGL.so -j 4` | rc 0 — **79 / 79**, lowest SSIM 0.997009 |
| **G4** | log evidence on the CORRECTED path `<out>/<case>/<backend>/output/mobilegl.log` and the CORRECTED arming string `MGPipe: verify armed` | 79 logs; `Fatal{` **0**; unarmed **0**; `PipeVerifyDiffer` **0**; `PipeResidualDiverged` **0**; `UnmigratedPipeInput` **0** |
| **G5** | `git show {p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48…0efe27` on both sides and equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G9** | above | green, and now a gate over the *answers* as well as the rows |
| **G10** (desktop half) | `MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 … ctest -R DirectGLES.CrossFrameBufferScenario` | 13/13 pass; `resid=8.00` in one window and `resid=0.00` in the others; `cso[csom=1 csob=1]` once then `csom=0 csob=0` — the block goes out once and is then suppressed |
| **G13** | above | clean |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <build-linux names>` | empty (0 of 2363 baseline names removed) |

Not reachable here, unchanged from round 1: **G6/G7** (package A + E), **G8/G12** (package E),
**G11** (device, the integrator).

### 5.3 Test lanes

| lane | result |
|---|---|
| `ctest --test-dir build-linux -L unit -j 8` | rc 0 — **1526 / 1526** |
| `ctest --test-dir build-push -L unit -j 8` | rc 0 — **1526 / 1526** |
| `ctest --test-dir build-verify -L unit -j 8` | rc 0 — **1526 / 1526** |
| `ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** (the all-pull arm, first try) |
| `ctest --test-dir build-linux -L integration-gpu -j 4` | first run rc 8 — **4 failures**, all `*.IsActuallyArmedWhenTheEnvironmentPinsItOn`; **re-run of the whole lane rc 0, 878 / 878**, and all four pass standalone (`ctest -R IsActuallyArmedWhenTheEnvironmentPinsItOn` → 10/10). This is the **pull** build, where this package changes nothing at all (G1), with three other packages' lanes running on the same machine — the flake family `tracker-v1.md` §4.4 and the review's last minor both name. |
| `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818 / 818** |

Unit count went 1518 → 1526: +7 new cases, +1 the round-1 `SetHashSuppressor` name that the
pull build had been missing until `build-linux` was rebuilt in this round (caught by the G2
name diff, fixed by the rebuild, not by an edit).

---

## 6. Where the tree contradicted the brief (round 2 additions)

Round 1's list (`tracker-v1.md` §4) still stands: `MGPipeDeriveRenderStateFields` is a stub on
this tag (§4.1), the two `Coverage.def` rows cannot retire their pull (§4.2 — now half-fixed,
see §3), `MGPipeApplyDeleteRenderState` takes `MGPHandleOnly`. New this round:

### 6.1 Three commands in section A do not work as written

1. **G2/G14's grep.** `grep -E '^\s+Test #'` matches only four-digit numbers, so it silently
   dropped ~1000 of 2404 names. Corrected pattern used here: `'^ +Test +#[0-9]+: '` with
   `sed -E 's/^ *Test +#[0-9]+: //'`.
2. **G3/C.2/C.3's `--ssim 0.99`.** `retrace_gate.py` has no such flag; reproduced —
   `retrace_gate.py: error: unrecognized arguments: --ssim 0.99`, and its `--help` lists only
   `--tree --lib --out -j --only`. The threshold is internal. As written the G3 command exits
   non-zero having run nothing.
3. **G4's log greps.** The logs are at `<out>/<case>/<backend>/output/mobilegl.log`, not
   `<out>/<case>/mobilegl.log`, **and** the arming line the code emits (`PipeFill.cpp:407`) is
   `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`, not `MGPipe verify:`. Fixing only the
   path would turn a true green into a false red. Both corrections are used in §5.2.

### 6.2 D13's "six kinds" versus the tree

The review's minor 10 says D13 scopes explicit destroy to six kinds excluding programs,
pipelines and shaders. D13's *prose* says six kinds, but the `Core.cpp` line ranges it cites
(`346-352` at the base ref) cover **both** `MarkProgramForDeletion` and
`MarkShaderForDeletion`, and the tree has a `StateBackendObjectRegistry` for `ProgramObject`.
So the brief is not self-consistent here and the "outside the six" reason is not safe to write
down. The reason that *is* safe, and the one the def now gives, is the tree's:
`PipeCalls.def` has no per-object handle for a program, a program pipeline or a shader —
`resource_destroy` and the `delete_*` family name resources and CSOs — so nothing publishes
their death and their DirectGLES twins are still reclaimed by the backend's own registry
teardown. Answer: `kUnpublishedDestroy`, documented as a hole.

### 6.3 The mirrored value's size is asserted in a file B may not edit

`MG_Backend/MGPipe/PipeInputs.cpp` compares `CurrentVertexAttributeValue` with one `memcmp`
under `static_assert(sizeof(...) == 3 * 4 * 4)`. That assertion is right (it is what keeps a
NaN attribute equal to itself and has no padding to false-differ on) and it is what forced
MAJOR 2's discriminator to live beside the array rather than inside the value. Recorded because
it is a real constraint on anyone who later wants the class mirrored: it needs A's or the
integrator's hand on that file, and the argument in §3.2 says it is not needed.

---

## 7. Deviations from the brief

Round 1's nine deviations are unchanged and still declared: header-only
`Tracker/CsoCache/SetHashSuppressor` (1), the sixth aggregate generation (2), `Core.{h,cpp}`
(3), coarse shutters for bits 5–17 (4), `Vector` scan in the CSO cache (5), `s_hashForTest`
(6), the residual block emitted after the fill and held rather than dropped (7), the runtime
derivation probe (8), five stale `MGPipeFillForVerb` comment references in A's files (9).

New in round 2:

10. **`DirtySurface.def` rows may carry several answers.** D16 describes one answer per row.
    A single answer cannot be both true and informative for the render-state family (the
    coarsest true answer for all 45 setters is `NEW_RENDER_STATE`, which erases the pipeline
    half that D6 and P3a need). The `|` notation plus a derived check keeps both. The file's
    own rule paragraph is rewritten to match.
11. **`GLContext` grew a push-only parallel array** (`m_currentVertexAttributeClasses`) and one
    push-only accessor. `Core.{h,cpp}` is "nobody" in C.5 (deviation 3 already covers editing
    it); this is called out separately because it is *state*, not just a facade. All of it is
    `#if MOBILEGL_PIPE_PUSH`; G1 is byte-for-byte unchanged.
12. **`MGPipeVertexAttribDefaultRepairCount()` is a counter in production code** read only by a
    test. It is not on any hot path (incremented inside the repair branch of an emission that
    only happens when an attribute default moved) and it is the only way to observe the repair
    without a poisoned read.

**Blocking follow-up for the integrator (not a deviation, a hand-off):**

1. **Package A must teach `MGPipeApplySetVertexAttribDefaults` to switch on
   `MGPAttribValue::ValueClass`** and reproduce `GLContext`'s conversions
   (`MG_State/GLState/Core.cpp:205-250`). Until then `GetCurrentVertexAttribute` keeps being
   pulled, the client repairs the mirror after every such call, and the row must not be treated
   as retiring its pull. The wire half is done and the class is now truthful.

---

## 8. Unfinished

1. **The 26 derived render-state mirrors are still pulled on this branch** (`tracker-v1.md`
   §4.1). Self-healing: the one-shot probe flips the moment A's `c1` is in the tree. The
   integrator's re-run of `integration-verify` + the verify retrace on the merged tree is what
   proves them — and note the probe samples **one** derivation (minor 9), so a partial `c1`
   is caught by the verify lane, not by the probe.
2. **The residual block's trip wire is only half an oracle here** (minor 4). It becomes
   independent when A's `c1` lands.
3. **The attribute-default repair never fired in the retrace corpus** (`attrib repairs: 0`
   across all 79 verify logs): no trace in the corpus moves a `glVertexAttrib*` default, so the
   coverage for that path is the unit test, not the corpus. Worth one integration entry with a
   `glVertexAttrib4f` in it when package E next touches the lane.
4. **`MEASUREMENTS.md` items the integrator owns**, restated from the review: the admitted
   resized-symbol set has to be widened to the four the contract actually produces (minor 7);
   D14/D.4.3's T1/T2 must be restated before the run, because the dirty walk is unconditional
   and only emission is behind the bitmask, so `T2` already contains the tracker cost
   (minor 8); the `*.IsActuallyArmedWhenTheEnvironmentPinsItOn` family belongs in the known-
   flake list by name (it fails under lane parallelism in the **pull** build too).
5. **G9 is not a CI gate until package E lands** the `pipe-gates` step; `test.yml` still runs
   `--summary` under an "Informational … becomes a gate in P2" comment (minor 6).
6. **Per-bit fire rates are still not printed** (`FormatWindowLine` is A's `PipeStats.cpp`),
   the six other `SetHashSuppressor` slots are still unwired (D11's own scope), and there is no
   device work here (G11, D.4).
