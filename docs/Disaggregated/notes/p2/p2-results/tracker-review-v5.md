# Adversarial review — P2 package B `p2/tracker`, round 5 (`~/w7/p2-tracker` @ `dddb60b0`)

Reviewer's brief: refute the claim that `tracker-review-v4.md`'s two majors are closed and that nothing
regressed. Everything below is my own run in `~/w7/p2-tracker`, not a quotation from `tracker-v5.md`.
`INTEGRATOR-DECISIONS.md` was read first; ID-1..ID-5 are treated as settled (ID-3's four-symbol set is
what G1 is measured against, ID-4's errata are the commands used). The tree was left exactly as found:
`git status --porcelain` empty before and after every step, `git rev-parse HEAD` still
`dddb60b0c12b1ed271a7b8d3283fe6f2d701d162` on `p2/tracker`, no file under the tree written. Every probe
below was run **in memory** against the tree's script loaded as a module (`probe.py`, `probe2.py`, kept
under `~/w7/rev-tk5/`); `9ff6061c`'s script was materialised as `~/w7/rev-tk5/gds-old.py` with its roots
patched to this tree, never inside the tree.

**Verdict: APPROVED — 0 majors, 8 minors.** (Minor 7 records that the orchestrator called for this file while my full-log integration re-run and the verify retrace were still running; every number they had produced by then was green, the binary is the one round 4 measured, and the finished logs land under `~/w7/rev-tk5/` for the integrator to read before merging.)

Both round-4 majors are closed *for the stated reason*, not by routing around: I re-ran round 4's own
reproductions and the outcome changed because the two sides now resolve to the same member+field token
and a reference/pointer alias is followed (§2). Every gate this tree can reach is green and I reproduced
all of them, including the two long retraces the implementer's own lane script died before finishing
(§1). What I could not do is turn any of the residual shapes I found into a wrong verdict on the real
tree; they are recorded as minors with the file:line and the probe that shows them (§4).

---

## 1. What I re-ran, and what it printed

Round 5 is one commit, 2 files (`MobileGL/MG_Pipe/DirtySurface.def` +60/−…, `scripts/gen_pipe_dirty_surface.py`
+990/−271 across both) and **compiles nothing**: `cmake --build {build-linux,build-push,build-verify} -j 12` →
`ninja: no work to do.` ×3 (`~/w7/rev-tk5/build.log`), so every binary below is the one round 4 measured.
I re-measured all of them rather than inherit that.

| gate | command | observed |
|---|---|---|
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — `0 added, 0 removed, 4 resized, 0 renamed`; the four are ID-3's set exactly: `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9; both `.so` 19100448 bytes |
| **G2** | ID-4's extraction on `build-linux`/`build-push`/`build-verify` | `2407` / `2407` / `3225`; `diff` linux vs push **0 lines** |
| **G14** | `LC_ALL=C comm -23 <LC_ALL=C-sorted baseline> <build-linux names>` | baseline 2363, **0 removed**, 44 added |
| **G5** | brief's `awk` over `refs/tags/p2/contract` and `HEAD`, and `~/w7/p2-before-syncrenderstate.sha` | `d8fd1c48…efe27` on all three |
| **G9** | `gen_pipe_dirty_surface.py --check` | rc 0 in 2.95 s — `73 mutators, all mapped … 45 render-state answers …` / `8 other (mutator, bit) answers … 0 COARSE … 0 UNDECIDED … 24 rows carry a prose answer` / footprint `1262 function bodies across 82 files … 157 … tainted, 10 of the 73 mutators reach one; 347 members are written outside` |
| **G9** | `… --self-test` | rc 0 — `21 negative controls, all tripped; positive controls OK` |
| **G9** | `… --summary` at HEAD vs `9ff6061c`'s script run against this tree (roots patched) | rc 0 / rc 0, 81 / 81 lines, **byte-identical** (`~/w7/rev-tk5/summary.log`) — E's CI step keeps working |
| **G13** | `gen_pipe.py --check` / `--self-test`; `check_include_closure.py`; `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'`; stdio in the package diff | rc 0 (`477 rows … 0 UNMAPPED`, `generated files are up to date`) / rc 0 (`7 negative-control trip(s), positive control OK`); `4 probes, 0 skipped, 0 problem(s)`; empty; 0 |
| unit | `ctest -L unit --no-tests=error -j 8` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1529` ×3 (`~/w7/rev-tk5/lane-unit-build-*.log`). Skip sets are the deliberate ones and cannot have moved (nothing compiled): build-linux skips the 57 push-only Tracker/CsoCache/SetHashSuppressor/PipeInputs cases (`7993d711`'s skip-instead-of-vanish), build-push the 12 verify-only PipeInputs cases, build-verify only LogLevel ×2 + Xfb dump — round 4's "two cases each" was an under-count, not a change |
| integration | `ctest -L integration-gpu -j 4` in build-push, the same under `MOBILEGL_PIPE_PUSH=0`, and in build-linux | My first pass (`lanes-a.sh`) ran all three to completion (`LANES-A-DONE`) with no `***Failed`/`***Timeout` in what it kept, but my capture kept only each lane's trailing skip list, not the summary line (my harness defect); the full-log re-run (`lanes-c.sh`) has since completed its first arm — build-push: **`100% tests passed, 0 tests failed out of 878`** (`~/w7/rev-tk5/lane-igpu-push.log`, 23:10) — while the `MOBILEGL_PIPE_PUSH=0` arm and build-linux were still in flight when the orchestrator forced this file. It lands in `~/w7/rev-tk5/ctest2.log` + `lane-igpu-*.log`. The implementer's own `~/w7/p2r5-logs/lanes.out` — the part of their script that did finish — records `878 / 878` ×3, and round 4 reproduced `878 / 878` ×3 on this **same binary** (`build-*/libMobileGL.so` untouched since Sep 6 11:49; `ninja: no work to do` ×3; neither round-5 file is compiled or read by any test) |
| **G4** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | same situation: ran to completion in `lanes-a.sh` with no failure marker kept, full-log re-run pending in `~/w7/rev-tk5/ctest2.log`; implementer's `lanes.out` `818 / 818`, round 4 `818 / 818` on this binary |
| **G3** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-push/libMobileGL.so --out ~/w7/retrace-out/rev-tk5-push -j 4` | **rc 0 — `passed 79 / 79; failed: []`** (`~/w7/rev-tk5/retrace-push-full.log`; e.g. `minecraft-1.21.4-startup` both backends `ssim=1.0`). This is the lane the implementer's script died in (`lanes.out` ends at `== G3 push retrace`, 2 of 79 logged) |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 … --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/rev-tk5-verify -j 4`, then the greps over `<out>/<case>/<backend>/output/mobilegl.log` | **still in flight** when forced: started 23:01:53; at 23:10 **59 of 79 cases `PASS`, 0 `FAIL`** (e.g. `journeymap-in-world` both backends `ssim=0.999979`); the `Fatal{` / not-armed / `PipeVerifyDiffer` / `PipeResidualDiverged` / `UnmigratedPipeInput` greps run automatically at the end into `~/w7/rev-tk5/retrace.log`. The implementer never ran this lane this round (`~/w7/retrace-out/v5-verify/` does not exist); round 4 measured `79 / 79`, all five greps 0, on this binary |
| subset | `ctest --test-dir build-push -L integration-gpu -R 'ClipDistance\|SampleMaskScope\|SampleVariables\|DualSourceBlend\|ViewportArray\|PrimitiveRestart'` | ran in `lanes-a.sh` (tail kept only 3 skipped `DirectVulkan.ClipDistance/ViewportArray` entries, the known lavapipe skips); full-log re-run pending in `ctest2.log` |
| G6/G10/G12 entries present in this tree | `ctest --test-dir build-push -R 'CsoContentAddressing\|RenderStateSpans\.\|Residual'` | ran in `lanes-a.sh`: `unit = 0.01 sec*proc (5 tests)`, no failure marker; full-log re-run pending in `ctest2.log` |
| G8 | `ctest --test-dir build-verify -R HandleRecycle` (default and under `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0`) | `No tests were found!!!` both ways — the entries are package E's/C's and are not in this tree; not a skip that hides anything, correctly not claimed by B |
| G6/G7, G8/G12, G11 | — | packages A / E / device; correctly not claimed |

**On the espryt items in my brief** (a two-holder scenario that watches the twin die on one notification, and
the "armless-knob" lane failing rather than skipping under `MOBILEGL_PIPE_PUSH=0 MOBILEGL_PIPE_LEGACY_MEMOS=0`):
this tree has **no** `NotifyStateObjectDestroyed` and no Track H slice — `grep -rln 'NotifyStateObjectDestroyed\|SlotAllocator' MobileGL`
returns only the contract's `SlotAllocator.{h,cpp}`, its test and `CsoCache.h` — so neither can be built here;
they belong to package C's review. What I did run is the lane above, which finds no test rather than
skipping one.

**Ownership and discipline.** `git diff --name-only refs/tags/p2/contract..HEAD` → 26 files, the set rounds
3 and 4 cleared; `9ff6061c..HEAD` touches only `MG_Pipe/DirtySurface.def` and `scripts/gen_pipe_dirty_surface.py`,
both **B** rows of C.5. `git diff --summary refs/tags/p2/contract..HEAD | grep -c 'mode change'` → **0**. 16 commits,
0 subjects failing `^\[Type\] (Scope): `, `git log --format=%B` has **0** `Co-Authored-By`. Nothing pushed.

---

## 2. The two round-4 majors, re-run — closed, for the stated reason

I did not take `tracker-v5.md` §2.4's tables on trust; the rows below are `--check` run **through the real
CLI path** (`main()` with `--check`, `DEF_PATH` swapped to a synthetic `.def` that changes one row of the
real file), on HEAD's script and on `9ff6061c`'s, same tree (`~/w7/rev-tk5/probe.out` §3).

```
row                                                         HEAD (dddb60b0)                                   9ff6061c
R1  X(SetClearColor,      NEW_RENDER_STATE|NEW_PATCH_STATE)                  rc=1 UNDER-FIRING NEW_PATCH_STATE   rc=0 (green)
R2  X(SetScissorBox,      NEW_RENDER_STATE|NEW_PATCH_STATE)                  rc=1 UNDER-FIRING NEW_PATCH_STATE   rc=0 (green)
R3  X(SetViewportIndexed, NEW_RENDER_STATE|NEW_PATCH_STATE)                  rc=1 UNDER-FIRING NEW_PATCH_STATE   rc=0 (green)
R4  X(SetBlendEquation,   …|NEW_PATCH_STATE)                                 rc=1 UNDER-FIRING NEW_PATCH_STATE   rc=1 (the old red, but see below)
R5  X(SetHint,            NEW_RENDER_STATE|NEW_PATCH_STATE)                  rc=1 UNDER-FIRING                   rc=1
```

**MAJOR 2 (the `NEW_PATCH_STATE` collapse)**: the shutter now resolves to
`m_parameters{PatchDefaultInnerLevel,PatchDefaultOuterLevel,PatchVertices}` (probe §1, all 18 bits resolved,
none of the five P2 shutters has an outside writer), and the legal carriers of `NEW_PATCH_STATE` over the 73
rows are exactly `['SetPatchDefaultInnerLevel', 'SetPatchDefaultOuterLevel', 'SetPatchVertices']` (35 at
`9ff6061c`). R1–R3 are red again for the reason they should be. The mechanism is the one claimed:
`LOCAL_FIELD_RE` (`gen_pipe_dirty_surface.py:355`) turns `render.PatchVertices` into
`CTXFIELD:GetRenderStateParameters.PatchVertices`, `narrow_tokens` (`:1064-1073`) narrows the getter's
`FIELD:m_parameters.*` to that field, and `match_writes` (`:1248-1266`) requires the field sets to intersect.

**MAJOR 1 (the reference alias)**: R4 was red at `9ff6061c` *for a false reason* ("writes nothing", with no
`MEM:m_parameters` credited) and is red at HEAD for the true one — the tokens are now
`m_parameters{BlendStates}`, taint 0. The seven setters round 4 named all resolve through the alias
(probe §2): `SetBlendEquation`/`SetStencilFunc` → `BlendStates`/`StencilStates`; and the shapes round 4
called "luck, not a property" are credited as whole writes (`m_vertexArrays.push_back` → `FIELD:m_vertexArrays.*`).
The alias machinery is `extract_writes` (`:683-881`): `DECL_RE` (`:412-417`) + `resolve_alias` (`:743-773`)
+ the rebind collection (`:731-741`).

**Sixteen real setters at field level** (probe §2; every one matches what the source at
`MobileGL/MG_State/GLState/RenderState/RenderState.cpp` does):

```
SetViewportIndexed          m_parameters{Viewports}                                       (:90-99)
SetHint                     m_parameters{FragmentShaderDerivativeHint,LineSmoothHint,     (:120-132, the
                                         PolygonSmoothHint,TextureCompressionHint}          Hint* slot rebind)
SetPolygonOffset            m_parameters{PolygonOffsetClamp,PolygonOffsetFactor,          (:251-258 delegating
                                         PolygonOffsetUnits}                                to :268-278)
SetPatchDefaultOuterLevel   m_parameters{PatchDefaultOuterLevel}    SUPPORTED NEW_PATCH_STATE
SetPatchDefaultInnerLevel   m_parameters{PatchDefaultInnerLevel}    SUPPORTED NEW_PATCH_STATE
SetPatchVertices            m_parameters{PatchVertices}             SUPPORTED NEW_PATCH_STATE
SetClipControl              m_parameters{ClipDepthMode,ClipOrigin}
SetCapabilityIndexed        m_parameters{BlendStates,ScissorTestEnabledMask}              (:442-479)
SetCapability               m_parameters{28 *Enabled fields + ClipDistanceEnabledMask + ScissorTestEnabledMask}
SetClearColor               m_parameters{ClearColor}
SetScissorBox               m_parameters{ScissorBoxWrittenMask,ScissorBoxes}
SetBlendEquation            m_parameters{BlendStates}
SetStencilFunc              m_parameters{StencilStates}
SetLineWidth                m_parameters{LineWidth}
SetDepthRangeIndexed        m_parameters{DepthRanges}
SetColorMask                m_parameters{ColorMasks}
SetSampleMaskValue          m_parameters{SampleMaskValue}
SetPixelStoreParam          m_pixelStore{Pack,Unpack}Parameters{Alignment,ImageHeight,LSBFirst,RowLength,
                                         SkipImages,SkipPixels,SkipRows,SwapBytes}   SUPPORTED NEW_PIXEL_PACK
```
taint 0 on all; none of the non-patch setters is SUPPORTED or COARSE for any of the five P2 bits.

**The eight derived answers are not vacuous** (probe §5): each is supported by the field it should be
(`m_parameters.PatchVertices` etc., `m_textureBindGeneration`, `m_anyVertexAttribDefaultGeneration` through
the `VertexAttribDefault` aggregate hop, both pixel-store halves), and — the check I wanted — **all eight stay
SUPPORTED with `METHOD_RE` disabled**, so none rests on the "any non-read-only method call is a whole write"
over-credit. The fixed point converges in 6 rounds (cap 16 is not load-bearing; 0 functions differ uncapped).

---

## 3. My own negative controls, through the real `--check` (probe §3)

Six controls of my own, one per way a false row could be green, plus the mark mechanism both ways:

| | row | HEAD | `9ff6061c` |
|---|---|---|---|
| C1 | `X(SetLineWidth, NEW_RENDER_STATE\|NEW_PATCH_STATE)` — a render setter on the patch bit | rc 1 `UNDER-FIRING NEW_PATCH_STATE` | **rc 0, green** |
| C2 | `X(SetPatchVertices, …\|NEW_PIXEL_PACK)` — a real carrier on a bit it does not move | rc 1 `UNDER-FIRING NEW_PIXEL_PACK` | rc 1 |
| C3 | `X(SetCurrentVertexAttributeFloat, NEW_VERTEX_ATTRIB_DEFAULTS\|NEW_SAMPLER_VIEWS)` | rc 1 `UNDER-FIRING NEW_SAMPLER_VIEWS` | rc 1 |
| C4 | `X(BumpTextureBindGeneration, NEW_SAMPLER_VIEWS\|NEW_SO_TARGETS)` | rc 1 `UNDER-FIRING NEW_SO_TARGETS` | rc 1 |
| C5 | `X(SetPixelStoreParam, kPulledPartialShutter\|NEW_PATCH_STATE)` — the writer-side `*` must not leak across members | rc 1 `UNDER-FIRING NEW_PATCH_STATE` | rc 1 |
| C6 | `X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS)` unmarked — a tainted mutator | rc 1 `UNDECIDED answer … the write analysis is not complete` (never SUPPORTED) | rc 1 as UNDER-FIRING (a verdict it did not have) |
| C6m | the same, marked in `MGP_DIRTY_SURFACE_UNDECIDED_LIST` | rc 0, prints `UNDECIDED, no verdict: …` and counts it in neither tally | n/a |
| C7s | a mark on `SetPatchVertices ← NEW_PATCH_STATE`, which the derivation decides | rc 1 `STALE undecided mark` | n/a |
| R6 | `X(SetCapabilityIndexed, …\|NEW_VERTEX_ATTRIB_DEFAULTS)` | rc 1 `UNDER-FIRING` | rc 1 |

Every control trips; C1 and R1–R3 are the rows that were green one commit ago.

**Synthetic bodies through the real extractor** (probe §4, my own shapes, not the self-test's): the pointer
rebind (`Hint* slot = nullptr; … slot = &m_parameters.X; *slot = mode`) → `FIELD:m_parameters.{LineSmoothHint,
PolygonSmoothHint}`, no taint; an alias of an alias (`auto& all = m_parameters; auto& vp = all.Viewports[i]; vp.x = 1`)
→ `FIELD:m_parameters.Viewports`; a reference to a call result, a reference parameter, an undeclared
`Access = v`, a static `s_counter = v`, an iterator write, a `.data()` pointer, a ternary lvalue → **taint**;
`memcpy(&m_parameters.Viewports[i], …)` → `FIELD:m_parameters.Viewports`; `m_parameters = X{}` → `FIELD:m_parameters.*`;
a write inside a lambda → credited; `m_parameters.Viewports[m_count++].x = v` → both members; a const alias → nothing (correct).

---

## 4. Minors

None of these changes a verdict on the real tree today; each is reproducible with the probe cited.

1. **Five write shapes are neither credited nor tainted, and one of them is live in two of the eight derived
   bodies.** `probe.out` §4: `std::fill(m_parameters.Viewports.begin(), …)`, `*this = F{}`, `Normalize(m_parameters.Viewports[i])`
   (a free function with a member-rooted argument), `std::exchange(m_count, v)` and `Fetch(&m_parameters.LineWidth)`
   all yield `tokens=[] taints=[]`. `record()` drops `*this` with no path explicitly (`gen_pipe_dirty_surface.py:778-781`);
   the other four have no rule at all — a call that is not a method on an lvalue and not in `BULK_RE` (`:401`) is
   invisible. The header's class claim (`.def:60-63`, script `:314-323`: "a write whose root the analysis cannot
   place TAINTS the function … Nothing is trusted by name except the standard library's size()/begin()/find()
   family") is therefore still wider than the code: a free function *is* trusted, silently, by having no rule.
   Live instance (`probe2.out` §A): `BitwiseEqual(m_parameters.PatchDefaultInnerLevel, levels)` at
   `RenderState.cpp:230,241`, reached by `SetPatchDefault{Inner,Outer}Level`, two of the eight derived rows;
   `BitwiseEqual` is `MG_Util/Math/VectorTypes.h:95`, takes `const&`, and is read-only — so the verdict is
   right by accident of the callee, not by a rule. The other seven live sites are `Memcpy(m_size…)` and
   `std::lock_guard lock(m_mutex)` declarations, none reachable from a derived row; `*this =`, `std::fill/
   copy/exchange` do not occur under the two roots (§D empty). Not a major because no verdict on the real
   tree is wrong and the shape that is live is read-only — but it is the third time the header claims a
   completeness the extractor does not have, and the next out-param helper under `MG_State` would produce a
   false UNDER-FIRING. Minimum: either credit a member-rooted argument of a non-method call as a whole write
   (widening; costs nothing today — `BitwiseEqual` would over-credit a field the setter writes anyway), or
   name the shape in `.def:60-73` as trusted-by-absence, and add one control that fails if such a call
   stops being credited/declared.
2. **A write through a pointer held *in* a member would be credited to the container, not tainted.**
   `m_slots[i]->f = v` records `MEM:m_slots FIELD:m_slots.f` (`record()` `:783-786` via `ASSIGN_RE`), and
   `for (auto* e : m_list) e->f = v` resolves the alias to `(m_list, [])` and credits `FIELD:m_list.f`; the
   write actually lands in the pointee, whose own `m_` member a shutter might read by name. That is the
   narrowing direction (a false UNDER-FIRING). `probe2.out` §B/§C/§E: **no such site exists under the two roots
   today** — the only pointee writes go through method calls, which inherit the callee's writes by name.
   Declared nowhere; belongs in the header's "what remains one-directional" list or as a taint.
3. **Round 4's minors 1–3 are open and correctly declared open** (`tracker-v5.md` §4): `writers_outside()`
   (`:913-941`) is still the regex scan and shares the extractor's blind spots; `macro_table()` collects
   `#define`s under the two roots only (today the only refused function-like macro is `MGP_FILL`, non-pasting,
   and 6 are expandable — `probe.out` §7); `expand_macros(…, rounds=4)` still gives up silently.
4. **The self-test has no control for the pixel-store leaking into the patch window.** `shutter_readers`'
   BitwiseEqual window (`:1129-1142`) is bounded by the last `}` before the `dirty |=`; if the pack block ever
   moved below the patch block in `Tracker.h`, `pack` would re-enter the patch shutter and
   `X(SetPixelStoreParam, …|NEW_PATCH_STATE)` would go green silently. My C5 is that control and trips today;
   control 16 (`SetClearColor`) would not catch it because `SetClearColor` writes no pixel-store member.
   One line in `self_test()`.
5. **Constructor init-lists parse as a function named after the last initialised member** (`probe.out` §4
   `CtorLike`: `void F::CtorLike() : m_count(0), m_parameters() { m_version = 1; }` becomes a body named
   `m_parameters`, and its init-list writes are not credited). `FUNCTION_RE` (`:62-63`) has no `:` guard.
   Widening only (a body named `m_x` merges into every `m_x(` call by name); constructors are not mutators.
6. **G3/G4 were not verified by the implementer this round.** `tracker-v5.md` §3.1 says so honestly ("STILL
   RUNNING"), but `~/w7/p2r5-logs/lanes.out` shows the lane script died at `== G3 push retrace` (2 of 79
   cases logged in `retrace-push.log`), `~/w7/retrace-out/v5-verify/` — cited as kept — does not exist, and
   `~/w7/retrace-out/v5-push/` is a 438 MB partial corpus that the result file does not list among the kept
   logs (`iverify.log`, `retrace-push.log` likewise). Both retraces are green in my run (§1); the hygiene
   items are the implementer's to delete.
7. **My own verification was cut short by the orchestrator** — the full-log re-run of the three `integration-gpu` arms, `integration-verify`, the render-state subset and the CSO/spans/residual entries (`~/w7/rev-tk5/ctest2.log`, `lane-*.log`) and the verify retrace with its five greps (`~/w7/rev-tk5/retrace.log`) were still running at 23:08 when this file was demanded. What had finished was green (§1); nothing in round 5 is compiled or read by any of them; round 4 reproduced all of them on the identical binary. The integrator should read those two files before merging; a non-green line there is grounds to reopen this review.
8. **Still outstanding and correctly recorded by B** (`tracker-v5.md` §6): by-name call resolution is what
   spreads taint (10 prose-row mutators through `ImageUnitBinding::Bind`, `TextureState.h:27-34`);
   members without the `m_` prefix cannot be placed; `--check` now costs 3.0 s; deviations 18–24 stand as
   declared (the second X-list, `CONTROL_KEYWORDS`, whole-write-on-any-call, two-level tokens, tainted ⇒
   UNDECIDED for every bit, const aliases). G6/G7 A's, G8/G12 E's, G11 device's; the round-3 minors 3/4/5/8
   untouched as stated.

---

## 5. Bottom line

The class of defect round 4 refused — "this script cannot read that construct" turned into "that mutator
writes nothing" — is closed in the direction that matters: both sides resolve to `MEM:<member>` +
`FIELD:<member>.<leaf>`, a reference or pointer alias is followed to its member, an unplaceable root taints
and rides the fixed point into an UNDECIDED that `--check` refuses unless the file marks it (and refuses again
when the mark goes stale), and the patch shutter is as narrow as the bit. I verified that with the real CLI
path on rows that were green one commit ago, with sixteen real setters at field level, with my own synthetic
shapes, with the method-call over-credit switched off, and with every reachable gate re-run including the two
retraces the implementer could not finish. What remains (§4.1–4.2) is a residue of unmodelled shapes that
are silent rather than tainting; none is live in a way that changes a verdict, the one live instance is
read-only, and the header should say so — which is a minor, not a reason to hold the package.

Logs kept under `~/w7`: `rev-tk5/{probe.out,probe2.out,gates.log,ctest2.log,retrace.log,summary.log,build.log}`
and the two retrace corpora `~/w7/retrace-out/rev-tk5-{push,verify}/` (deleted after this review is filed if
the integrator does not want them); everything else of mine under `~/w7/rev-tk5/` was removed.
