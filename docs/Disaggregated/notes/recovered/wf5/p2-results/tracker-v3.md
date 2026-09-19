# P2 package B — `p2/tracker` — result, rework round 3

Tree `~/w7/p2-tracker`, branch `p2/tracker`, branched from the contract commit `9c6a8a25`
(the `p2/contract` tag). Not pushed. Supersedes `tracker-v2.md`; everything that file says
about rounds 1–2 still holds except where §2 below corrects it.

Round-3 scope: the three majors and all ten minors of `tracker-review-v2.md`. Nothing from
rounds 1–2 was reverted; two new commits sit on top of `dc452c02`.

---

## 1. Commits

Rounds 1–2 (`9c6a8a25..dc452c02`, unchanged): `f52dd262`, `9c9df515`, `8d000f7e`, `a0c44c4f`,
`e54e7399`, `e49f0ea7`, `b3daa404`, `7993d711`, `57b85f73`, `80b861b8`, `d5d4e757`, `dc452c02`.

Round 3 (`dc452c02..HEAD`, new):

| sha | subject | closes |
|---|---|---|
| `4e8f7179` | `[Fix] (Pipe): re-arm the residual block on any render-state move and republish every vertex-attribute default on a fresh context` | **MAJOR 1**, **MAJOR 3**, minors 1, 4, 5, 6 |
| `06f9adfd` | `[Fix] (Pipe): derive the dirty-surface map's object-class and value-class answers too, and correct the two rows that named a shutter their mutator never moves` | **MAJOR 2**, minors 8, 9 |

Round-3 diffstat: 5 files, +594 / −56.

```
MobileGL/MG_Impl/Pipe/PipeFill.cpp    | 119 ++++++--
MobileGL/MG_Impl/Pipe/PipeFill.h      |   9 +
MobileGL/MG_Pipe/DirtySurface.def     |  43 ++-
MobileGL/MG_Test/Pipe/TrackerTest.cpp | 107 ++++++-
scripts/gen_pipe_dirty_surface.py     | 372 ++++++++++++++++++++++++---
```

All five are B's own rows in C.5. Whole package: 26 files, +3395 / −78. No `MG_Backend/**`,
no `CMakeLists.txt`, no A/E file touched this round (round 1's `MG_State/GLState/Core.{h,cpp}`
edits stand as declared deviation 3, "nobody" in C.5). Single-line `[Type] (Scope): …`
subjects, `- ` bullets, zero attribution lines.

---

## 2. The three majors

### MAJOR 1 — the residual block's shutter

**Confirmed and fixed.** `g_residualDue` was set only on `FreshlyPrimed || NEW_PIPELINE_STATE`
(`PipeFill.cpp:1096-1098` at `dc452c02`), while `CapabilityInput::ClipDistance0..7` are 8 of
the 35 bits the block carries and `RenderState::SetCapability`'s clip-distance arm is
deliberately `++m_version` only. The comment above the emitter asserted the opposite as its
justification.

What landed (`4e8f7179`):

* the arming is now `NEW_RENDER_STATE | NEW_PIPELINE_STATE` — the coarsest always-true
  shutter, and the same answer `DirtySurface.def`'s derivation gives `SetCapability`;
* it moved **outside** the `kMGPipeSubsystemResidualValues` gate: whether the capability set
  may have moved is a fact about the frontend, not about which subsystems this build pushes,
  so a per-subsystem A/B that turns the block off no longer loses the record that one is owed;
* `FreshlyPrimed` arms it in the new fresh-context block (below), which is also where
  `MGPipeApplierReset()` clears the applier's residual mirror — the two now cannot disagree;
* the false comment is replaced with the fact and the trade (over-firing costs one 35-bit
  loop on a verb that already moved render state; under-firing renders stale).

Pinned by `TrackerShippedEmitter.AClipDistanceEnableReArmsTheResidualBlock`, which poisons
`MGPipeApplier().Residual`, asserts the premise (`glEnable(GL_CLIP_DISTANCE0)` moves
`m_version` and not `m_pipelineStateVersion` — asserted, not assumed, so the test says so if
that ever changes), and requires the re-emitted block to carry the bit.

**Cost, for `MEASUREMENTS.md`.** The block now goes out on every verb where render state
moved, not only where the pipeline version moved: 8 bytes plus a 35-way `IsCapabilityEnabled`
loop plus the applier's 35-bit compare. In the `CrossFrameBufferScenario` window trace this
changes nothing measurable (§5.5: `resid=8.00` in window 1, `0.00` after, exactly as in round
2, because nothing there moves render state after the priming draw). A blend-toggle workload
pays it once per toggled draw. I did not add a content-level suppressor (skip when the 35 bits
are unchanged) on purpose: the trip wire's job is to catch an *assembled* block that drifts
from the frontend's answer, and that drift does not move the carried bits, so suppressing on
them would disarm the oracle for exactly the case D9 built it for.

### MAJOR 2 — two `.def` rows, and a gate that could not see them

**Both confirmed and fixed, and the gate widened rather than merely documented.**

* `X(SetPixelStoreParam, NEW_PIXEL_PACK)` → **`kPulledEveryVerb`**. The setter writes both
  halves (eight `Pack` arms, eight `Unpack` arms) and the tracker's bit 2 is a byte compare of
  the pack half alone, so eight of sixteen arms move nothing that bit reads. What is true on
  every path is the pull: `GetPixelStoreParameters` is one of the two `Coverage.def` rows an
  emitted call does not supply completely, so the residual fill copies both halves at every
  verb of the class. The comment on the row says all of that.
* `X(SetNamedTransformFeedbackBinding, NEW_SO_TARGETS)` → **`kPulledEveryVerb`**. False on
  *every* path: the mutator binds a `BufferState` binding point or writes a saved-bindings
  entry, while the bit mixes the buffer-**content** aggregate with the transform-feedback
  generation. It reaches the backend like every other buffer binding point, through
  `GetBufferBindingPoint` in the class's may-read mask.
* **`--check` now derives the other 28 rows' answers too.** `gen_pipe_dirty_surface.py`
  reads `Tracker.h`'s `Update()` for what each bit's shutter READS (locals expanded, and the
  two `BitwiseEqual` bits taken from the block that builds the compared value), resolves those
  accessors through `MG_State`'s getters to the members behind them, computes what every
  mutator transitively WRITES as a fixed point over `MG_State/GLState` + `MG_Impl/Pipe`
  (expanding `MGP_NOTE_AGGREGATE` through `MGPipeNoteAggregate`'s own `switch` rather than
  assuming the hop), and fails a row naming a bit whose shutter its mutator moves on no path.
  The enumerator spelling (`MGPipeDirty::NewSoTargets`) and the row spelling
  (`NEW_SO_TARGETS`) are paired **by position** out of `Tracker.h`, so the enum and
  `kMGPipeDirtyNames` drifting apart is itself a gate failure.
* It is **one-directional by construction** and the file and `--check` both say so: the write
  analysis over-approximates (a call name resolves to every body of that name, a write inside
  an `if` counts), so it can prove absence and not presence. Absence is the under-firing
  direction. There is deliberately no MISSING check on this family.
* `--check` now prints its own coverage, including what it did **not** check:
  `45 render-state answers derived …; 7 other bit answers derived from their shutter in
  Tracker.h (under-firing only); 0 declined; 25 rows carry a prose answer (kExplicitDestroy,
  kImmediate, kNoBackendRead, kPulledEveryVerb, kReverseChannel, kUnpublishedDestroy) that no
  derivation checks`, and it lists every row it had to decline (none today).

**The gate demonstrably sees the two defects.** Before the rows were corrected, the new
derivation printed exactly, and only, them:

```
dirty-surface: UNDER-FIRING answer NEW_SO_TARGETS for SetNamedTransformFeedbackBinding - it
  writes nothing NEW_SO_TARGETS's shutter reads (shutter: m_anyBufferChangeGeneration,
  m_transformFeedbackGeneration), so a mutation through it publishes nothing
dirty-surface: UNDER-FIRING answer NEW_PIXEL_PACK for SetPixelStoreParam - it writes nothing
  NEW_PIXEL_PACK's shutter reads (shutter: m_pixelStorePackParameters,
  m_pixelStoreUnpackParameters), so a mutation through it publishes nothing
dirty-surface: 2 problem(s); …   (main() returned 1)
```

Two more self-test negative controls make that permanent (one object-class, one value-class):
`--self-test` is now `7 negative controls, all tripped`.

The review's own in-memory demonstration no longer passes: `SetNamedTransformFeedbackBinding
← NEW_VERTEX_ATTRIB_DEFAULTS`, `SetPixelStoreParam ← NEW_SO_TARGETS`,
`BumpTextureBindGeneration ← NEW_GLOBAL_CONSTANTS` and `SetCurrentVertexAttributeInt ←
NEW_FRAMEBUFFER` are all UNDER-FIRING under the new check.

### MAJOR 3 — `set_vertex_attrib_defaults` published nothing across a context change

**Confirmed and fixed.** `Tracker::Reset()` sets the staging mirror to `AttribDefaults{}`
(the GL defaults) and a fresh `GLContext` holds the same, so the per-attribute diff was empty
on the one walk that must publish a complete state, while `MGPipeApplierReset()` left
`gPipeInputs.m_currentVertexAttribute` holding the **previous** context's values — cancelling,
two lines later, the `InvalidateAll()` written for exactly that case.

What landed (`4e8f7179`): `EmitVertexAttribDefaults(ctx, freshlyPrimed)` sends all 32 when the
tracker is freshly primed — the arm `EmitRenderState` already had, and the same shape as
`EmitPixelPackState`/`EmitPatchState`, which send whole values. `MGPipeVertexAttribDefaultsLastHeader()`
(PipeFill.h) is the observable: `Count`/`Mask` of the last call that actually went out, which
is the only way to see this without a poisoned read of `m_currentVertexAttribute`.

Pinned by `TrackerShippedEmitter.AFreshContextRepublishesEveryVertexAttributeDefault`
(fresh context ⇒ `Count == 32`, `Mask == 0xFFFFFFFF`; a steady context ⇒ `Count == 1`).

**This closed round-2 minor 2 as a side effect, with corpus evidence.** The repair path used
to have unit coverage only — the reviewer found 0 occurrences of the applier warning across
79 verify logs. Now the fresh-context republish carries the GL default `{0,0,0,1}`, whose
float bit pattern (`0x3F800000` in component 3) is not the int view the frontend holds, so
the applier's class-blind `memcpy` cannot reproduce it, the client detects that and repairs
the mirror — **once per process, in all 79 verify logs and all 79 push logs** (§5.4). The
repair is now exercised by the whole retrace corpus and the whole integration lane, not just
by a unit test.

Consequence the integrator must know: **every push/verify run now logs one
`MGLOG_W_ONCE` line** — `MGPipe: MGPipeApplySetVertexAttribDefaults does not reproduce the
carried value on this build (it ignores MGPAttribValue::ValueClass) - the client is keeping
m_currentVertexAttribute authoritative`. It is once per process, it is true, and it stops by
itself the day package A honours `ValueClass` (§7 hand-off 1). I kept it at W rather than
demoting to D because a mirror the applier cannot reproduce is exactly what a warning is for.

### The three fixes' negative controls, run

Each fix was reverted in the working tree, the test binary rebuilt, the new case run, and the
tree restored (`scratchpad/wf5/p2-tracker/r3_neg.py`):

| control | result |
|---|---|
| residual armed on the pipeline version only | `AClipDistanceEnableReArmsTheResidualBlock` **TRIPPED (red)** |
| no freshly-primed arm in `set_vertex_attrib_defaults` | `AFreshContextRepublishesEveryVertexAttributeDefault` **TRIPPED (red)** |
| applier/cache reset back inside the render-state gate | `AFreshContextResetsTheApplierWithTheRenderStateSubsystemOff` **TRIPPED (red)** |

---

## 3. The ten minors

| # | verdict |
|---|---|
| 1 the repair test asserted the defect | **fixed.** It now asserts the invariant: the call went out naming exactly the attribute that moved (`Count == 1`, `Mask == 1`), `repairs <= before + 1`, and a second identical walk neither re-emits nor re-repairs. It stays green the day A honours `ValueClass`. |
| 2 the repair path had unit coverage only | **fixed by MAJOR 3's fix, with evidence**: the warning now appears once in each of the 79 verify logs and 79 push logs (§5.4). Still true that no fixture *moves* a `glVertexAttrib*` default mid-trace; the fresh-context republish is what exercises the path. |
| 3 `build-verify` was stale at `dc452c02` | **accepted and acted on**: all three build dirs were rebuilt from scratch of the current tip before any lane in §5 was run, and the numbers below are from those binaries. My verify retrace's lowest SSIM is **0.993475**, equal to the push arm and to the reviewer's own run — `tracker-v2.md` §5.2's `0.997009` was indeed produced from a binary that was not the tip. **Do not copy `tracker-v2.md` §5 into `MEASUREMENTS.md`; use §5 here.** |
| 4 the fresh-context reset sat inside the render-state subsystem gate | **fixed.** `MGPipeCsoCacheInstance().Reset()`, `MGPipeApplierReset()`, `InvalidateAll()` and the residual arming now happen in one block before any per-subsystem gate. Pinned by `AFreshContextResetsTheApplierWithTheRenderStateSubsystemOff`, which clears bit 0 and keeps bits 1–3 (the A/B D14 invites). |
| 5 the residual gate names a subsystem constant directly | **fixed as far as it can be**: it still tests `kMGPipeSubsystemResidualValues` directly, because the residual block has no dirty bit to route through `MGPipeSubsystemForDirty` — that is what makes it the residue. The exception is now stated at the code and *asserted*: a `constexpr` `static_assert` that **no** `MGPipeDirty` bit maps onto `kMGPipeSubsystemResidualValues`, so the day one does, the double gate is a build break rather than a silent second switch. |
| 6 the `MOBILEGL_ASSERT` is compiled out in Release | **recorded at the code**: the comment now says the assert is a debug/verify alarm and that the *behaviour* is safe in every build regardless — it is the narrowing of the mirror advance to the `NEW_RENDER_STATE` branch, which is unconditional, that makes it safe; a broken invariant in a shipping build costs a re-send, never a false claim. |
| 7 `SetPatchVertices` changed answer class without a declaration | **declared**, §6 deviation 14. |
| 8 `render_state_publishers()` unions across bodies of one name | **fixed**: the fold is now `set.intersection` over the bodies of a name (per-body resolution, then intersect). No answer changes today (45 names / 45 bodies); it is the fold that stays right when one name has two bodies. |
| 9 a conditional `BumpVersions()` derives as unconditional | **fixed in the header's wording**: "every path" now reads "every path that MUTATES" — a redundant-write guard has no mutation to carry, so it does not make its publisher conditional; a publisher reached on only some *mutating* paths is what must not be named. |
| 10 the outstanding list | **restated**, §7/§8: G9 is not a CI gate until E lands; G1's admitted resize set is four symbols; D14/D.4.3's T1−T2 no longer isolates the tracker; `MGPipeWidenedCounter` cannot see a change of exactly 65536; `s_hashForTest` is a live function pointer; the three header-only files still want their `list(APPEND)` line. |
| 11 three commands in brief §A are broken | **re-reproduced this round**, §6.1. |

---

## 4. What is *not* fixed, and why

* **`kPulledEveryVerb` is a prose answer no derivation checks.** Both corrected rows now carry
  it, and so do 23 others. Checking it would mean deriving "this field is in its verb class's
  may-read mask and no emitted call supplies it", which is `Coverage.def` + `gen_pipe.py` +
  `EmittedCallSuppliesTheWholeField` — A's files and E's gate, not B's. `--check` prints the
  count of prose rows so the gap is visible in the gate's own output rather than implied.
* **The object-class derivation cannot prove a publisher fires on every path**, only that it
  fires on none. Stated in the file, in the script's docstring and in `--check`'s output.
* **No content suppressor on the residual block** — see MAJOR 1's cost note.

---

## 5. Verification — every command and its actual output

All commands run in `~/w7/p2-tracker` at `06f9adfd`, after `cmake --build` of all three build
dirs (build-linux 67 targets, build-push 51, build-verify 73 — the tip is what every number
below was produced from). `git status --porcelain` is empty before and after.

### 5.1 Generators and gates

| command | output |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` + `7 other bit answers derived from their shutter in Tracker.h (under-firing only); 0 declined; 25 rows carry a prose answer (…) that no derivation checks` |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `7 negative controls, all tripped` |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0 — unchanged shape, E's CI step still works |
| `python3 scripts/gen_pipe.py --check` | rc 0 — `inventory 477 rows … 0 UNMAPPED`, `generated files are up to date` |
| `python3 scripts/gen_pipe.py --self-test` | rc 0 — `7 negative-control trip(s), positive control OK` |
| `python3 scripts/check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty |

### 5.2 Section-A gates reachable from this tree

| gate | command | output |
|---|---|---|
| **G1** | `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text +160 (+0.001%)`; file size identical. The four resized are the contract's (`RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9). **Round 3 adds zero pull-build delta.** |
| **G2** | `ctest -N` name sets, `'^ +Test +#[0-9]+: '` | `build-linux 2407`, `build-push 2407`, **zero-line diff** (2404 + the 3 new cases and their pull SKIP twins) |
| **G3** | `retrace_gate.py --tree ~/w7/p2-tracker --lib …/build-push/libMobileGL.so --out ~/w7/retrace-out/r3-push -j 4` | rc 0 — **79 / 79**, `failed: []`, lowest SSIM **0.993475** (two cases), then 0.995426 |
| **G4** | `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818 / 818** |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 retrace_gate.py … --lib …/build-verify/libMobileGL.so --out ~/w7/retrace-out/r3-verify -j 4` | rc 0 — **79 / 79**, lowest SSIM **0.993475** |
| **G4** | log evidence at `<out>/<case>/<backend>/output/mobilegl.log`, arming string `MGPipe: verify armed` | 79 logs; `Fatal{` **0**; unarmed **0**; `PipeVerifyDiffer` **0**; `PipeResidualDiverged` **0**; `UnmigratedPipeInput` **0** |
| **G5** | `git show {p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48…0efe27` on both sides and equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G9** | above | green, and now a gate over the object-class and value-class answers as well |
| **G10** (desktop half) | `MOBILEGL_LOG_FILE_PATH=… MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 MobileGLIntegrationTest --gtest_filter='*CrossFrameBufferScenario*'` | 13/13 pass; window 1 `resid=8.00 … cso[csom=1 csob=1]`, windows 2+ `resid=0.00 … csom=0 csob=0` |
| **G13** | above | clean |
| **G14** | `comm -23 ~/w7/p2-before-ctest-names.txt <build-linux names>` | empty (0 of 2363 baseline names removed) |

Not reachable here: **G6/G7** (package A), **G8/G12** (package E), **G11** (device).

### 5.3 Test lanes

| lane | result |
|---|---|
| `ctest --test-dir build-linux -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-push -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-verify -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** (all-pull arm) |
| `ctest --test-dir build-linux -L integration-gpu -j 4` | rc 0 — **878 / 878**, first try, no flake |
| `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818 / 818** |

Unit count 1526 → 1529: the three new `TrackerShippedEmitter` cases (and their pull SKIP
twins, which is why G2 stays name-for-name identical).

### 5.4 The applier-repair warning, now visible in the corpus

`grep -l 'does not reproduce the carried value' …/output/mobilegl.log`: **79 of 79** verify
logs and **79 of 79** push logs, exactly one occurrence each (`MGLOG_W_ONCE`). Before this
round: 0 of 79. See MAJOR 3.

### 5.5 G10's stats windows, verbatim (first two)

```
MGPipe stats: frames=1 window=1 draws=1 draws/f=1.00 acc=16 acc/draw=16.00
  bytes/f[buf=184.00 … resid=8.00] tex[…] cso[csom=1 csob=1] gates[ers=1/1 etl=1/1 eub=1/1 …]
MGPipe stats: frames=2 window=1 draws=1 draws/f=1.00 acc=14 acc/draw=14.00
  bytes/f[buf=0.00 … resid=0.00] tex[…] cso[csom=0 csob=0] gates[ers=2/0 etl=2/0 eub=2/0 …]
```

**Note for whoever repeats this**: the stats line does **not** reach `ctest -V`'s captured
output. It is `MGLOG_I`, so on desktop it needs `MOBILEGL_LOG_FILE_PATH`; run the integration
binary directly with `--gtest_filter='*CrossFrameBufferScenario*'` (the ctest entry names are
not the gtest suite names).

---

## 6. Where the tree contradicted the brief

Rounds 1–2's list stands (`tracker-v2.md` §6): `MGPipeDeriveRenderStateFields` is a stub on
this tag; the two `Coverage.def` rows cannot retire their pull; `MGPipeApplyDeleteRenderState`
takes `MGPHandleOnly`; D13's "six kinds" is not self-consistent with the `Core.cpp` ranges it
cites; `MG_Backend/MGPipe/PipeInputs.cpp`'s `static_assert(sizeof(...) == 3*4*4)` forces the
attribute-value class to live beside the array. New or re-confirmed this round:

### 6.1 Three commands in section A still do not work as written (re-reproduced)

1. **G3/C.2/C.3's `--ssim 0.99`** — `retrace_gate.py: error: unrecognized arguments: --ssim
   0.99`, with `--tree`/`--lib`/`--out` all supplied. As written the gate exits non-zero
   having run nothing. The threshold is internal.
2. **G2/G14's `grep -E '^\s+Test #'`** — matches 1408 of 2407 names in this tree (it drops
   every test numbered under 1000). Corrected pattern used here: `'^ +Test +#[0-9]+: '` with
   `sed -E 's/^ *Test +#[0-9]+: //'`.
3. **G4's log glob and arming string** — logs are at `<out>/<case>/<backend>/output/mobilegl.log`
   (the brief's `<out>/<case>/mobilegl.log` matches nothing) and the arming line is
   `MGPipe: verify armed - 63 fields, 69 verbs, fatal=1`, not `MGPipe verify:` (0 files match
   the brief's string). Fixing only the path would turn a true green into a false red.

**§A and D.3 must be corrected before the five-part gate is run.**

### 6.2 The G10 stats command needs a log file on desktop

Recorded in §5.5. `tracker-v2.md` and `tracker-review-v2.md` both present a `ctest`-shaped
command; what actually produces the line is the direct binary run with
`MOBILEGL_LOG_FILE_PATH`.

---

## 7. Deviations from the brief

Rounds 1–2's twelve deviations are unchanged and still declared (header-only
`Tracker/CsoCache/SetHashSuppressor`; the sixth aggregate generation; `Core.{h,cpp}`; coarse
shutters for bits 5–17; `Vector` scan in the CSO cache; `s_hashForTest`; the residual block
emitted after the fill; the runtime derivation probe; five stale comment references in A's
files; `|`-joined `.def` answers; `GLContext`'s push-only class array;
`MGPipeVertexAttribDefaultRepairCount`). New in round 3:

13. **`MGPipeVertexAttribDefaultsLastHeader()` is a second observable in production code** —
    one 8-byte global written only when a `set_vertex_attrib_defaults` actually goes out. It
    is the only way to see "the fresh context republished all 32" and "one moved attribute
    published exactly one": reading `m_currentVertexAttribute` back at a verb whose class does
    not carry it is the poison violation `FillPoints.def` forbids. Not on any hot path.
14. **`X(SetPatchVertices, …)` answers `NEW_PATCH_STATE|NEW_RENDER_STATE|NEW_PIPELINE_STATE`,
    not D16's supplied `kImmediate`** (round-2 change, undeclared until now — round-2 review
    minor 7). It *is* also an immediate publish point, but it has a real bit and the bit is
    the more useful answer: `set_patch_state` carries it whatever the caller does next. The
    row's comment says so, and the render-state half of the answer is derived and checked.
15. **`kPulledEveryVerb` now carries two rows whose mutator does move *a* real shutter on
    *some* paths** (`SetPixelStoreParam`'s eight pack arms). D16 describes one answer per row
    and the file forbids mixing a prose answer with a bit; under the file's own rule the
    coarsest true answer is the pull. The information that is lost — "the pack half also moves
    NEW_PIXEL_PACK" — is in the row's comment, where no shutter builder can mistake it for a
    licence to narrow.

---

## 8. Unfinished, and the hand-off

1. **Package A must teach `MGPipeApplySetVertexAttribDefaults` to switch on
   `MGPAttribValue::ValueClass`** and reproduce `GLContext`'s conversions
   (`MG_State/GLState/Core.cpp:205-250`). Until then `GetCurrentVertexAttribute` keeps being
   pulled, the client repairs the mirror after every such call, and **every push run logs the
   one-time warning of §5.4**. No edit here is needed when it lands: the repair stops firing,
   `TrackerShippedEmitter.APushedAttributeDefaultTheApplierCannotReproduceIsRepaired` stays
   green (it asserts `repairs <= 1`, not `== 1`), and the `Coverage.def` row can then retire
   its pull.
2. **The 26 derived render-state mirrors are still pulled on this branch**; the one-shot
   derivation probe flips when A's `c1` lands, and the probe samples one derivation, so the
   verify lane is what proves the rest.
3. **The residual block's trip wire is still half an oracle here** — while
   `MGPipeDeriveRenderStateFields` is a stub, the mirror it compares against is filled by the
   residual fill from the same accessor. It becomes independent with A's `c1`.
4. **No fixture in the corpus *moves* a `glVertexAttrib*` default mid-trace.** The repair path
   is now exercised by the fresh-context republish in all 158 retrace runs, but an integration
   entry with a `glVertexAttrib4f` in it would still be worth one line of E's lane.
5. **`MEASUREMENTS.md` items the integrator owns**: the admitted resized-symbol set is four
   symbols, not D15's three; D14/D.4.3's T1−T2 no longer isolates the tracker (the dirty walk
   is unconditional, only emission is behind the bitmask); the
   `*.IsActuallyArmedWhenTheEnvironmentPinsItOn` family belongs in the known-flake list by
   name (it did not flake this round, but it did in round 2 under lane parallelism, in the
   pull build); and the residual block's new firing rate (MAJOR 1's cost note) belongs beside
   the `resid=` byte class.
6. **G9 is not a CI gate until package E lands** the `pipe-gates` step; `test.yml` is E's file
   and still runs `--summary` under an "Informational" comment. The two new derivations are
   only as load-bearing as that step.
7. Unchanged from round 2: per-bit fire rates are not printed (`FormatWindowLine` is A's),
   the six other `SetHashSuppressor` slots are unwired (D11's own scope), `MGPipeWidenedCounter`
   cannot see a change of exactly 65536, `s_hashForTest` is a live function pointer in
   production code, the three header-only files want their `list(APPEND)` line, and there is
   no device work here (G11, D.4).
