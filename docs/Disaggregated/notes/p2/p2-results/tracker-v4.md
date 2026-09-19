# P2 package B — `p2/tracker` — result, rework round 4

Tree `~/w7/p2-tracker`, branch `p2/tracker`, branched from the contract commit `9c6a8a25`
(the `p2/contract` tag). Not pushed. Supersedes `tracker-v3.md`; everything that file says
still holds except §2's MAJOR 2 evidence and §7's deviation 15, both corrected below.

Round-4 scope: `tracker-review-v3.md`'s single major. One commit sits on top of `06f9adfd`.
The eleven minors of that review are **not** closed this round (the task named the major);
§4 records what each of them still says and which of them this commit happened to answer.

---

## 1. Commits

Rounds 1–3 (`9c6a8a25..06f9adfd`, unchanged): `f52dd262`, `9c9df515`, `8d000f7e`, `a0c44c4f`,
`e54e7399`, `e49f0ea7`, `b3daa404`, `7993d711`, `57b85f73`, `80b861b8`, `d5d4e757`, `dc452c02`,
`4e8f7179`, `06f9adfd`.

Round 4 (`06f9adfd..HEAD`, new):

| sha | subject | closes |
|---|---|---|
| `9ff6061c` | `[Fix] (Pipe, DirtySurface): read the writes that go through a member's field and the ones the preprocessor pastes together, and decline the rows the write analysis cannot answer - its "UNDER-FIRING" verdicts were an absence proof it did not have, and one of them put a false answer in the map for a bit P2 already ships` | **MAJOR 1** |

Round-4 diffstat: 2 files, +508 / −76.

```
MobileGL/MG_Pipe/DirtySurface.def |  85 ++++--
scripts/gen_pipe_dirty_surface.py | 499 ++++++++++++++++++++++++++++++++-----
```

Both are B's own rows in C.5. Whole package: 26 files, `git diff --summary p2/contract..HEAD`
shows **0 mode changes**; every subject matches `^\[Type\] (Scope): `; `git log --format=%B`
contains no `Co-Authored-By`. `git status --porcelain` empty before and after every run below.

---

## 2. MAJOR 1 — the derivation gate claimed an absence proof it did not have

**Confirmed, in all three of its parts, and fixed.**

### 2.1 What was actually wrong

1. **The member-field hole.** `written_tokens()` collected `MEM:` tokens only from
   `MEMBER_WRITE_RE`, which requires the member name to be *immediately* followed by the
   assignment. A write through a member's field (`m_foo.bar = v`) matched only
   `FIELD_WRITE_RE` and recorded `FIELD:bar`, never `MEM:m_foo`. The reader side
   (`resolve_reader`) resolves an accessor to `MEM:m_foo`. For **any struct-valued member**
   the two halves could therefore never meet, so the analysis under-approximated in exactly
   the direction its own "over-approximation" claim forbids, and an absence claim built on it
   is not a claim it can make.
2. **The token-paste hole.** `RenderState.cpp:829-833`'s `SET_PIXEL_STORE_PARAM` writes
   `m_pixelStore##paramNameHead##Parameters.paramNameTail`. The file was read raw, so the
   member does not exist as a token at all and the "field" the analysis recorded was the
   **macro parameter name**. Reproduced at `06f9adfd`:
   `moved['SetPixelStoreParam'] == ['FIELD:paramNameTail']`. The same hole hid
   `SET_CAPABILITY`'s `m_parameters.capability##Enabled` writes.
3. **The consequence in the deliverable.** Over the 932 function names the analysis knew,
   **zero** could carry `NEW_PIXEL_PACK` — the gate did not merely fail to check bit 2, it
   forbade it — and the row forced by that, `X(SetPixelStoreParam, kPulledEveryVerb)`, carries
   an answer documented as *"no shutter exists, and none is needed yet"* about the only
   mutator behind the shipped `set_pixel_pack_state`.

### 2.2 What landed (`9ff6061c`)

* **`MEMBER_ROOTED_WRITE_RE`** records `MEM:m_x` **and** `FIELD:f` for `m_x.f`, `m_x[i].f`,
  `m_x->f` and nested lvalues. The assignment suffix is shared by all three write patterns
  (`ASSIGN`), so `+=`, `|=`, `<<=`, `++` and `--` count and `==` / `!=` / `<=` / `>=` cannot.
* **The preprocessor is run.** `macro_table()` collects the function-like `#define`s under the
  two analysed roots; `expand_macros()` blanks the directives (so a macro body defined *inside*
  a function is no longer read as that function's code — that is where `FIELD:paramNameTail`
  came from), substitutes parameters and performs `##` pasting. A macro is **refused** when
  expanding it could corrupt the brace matching every body is found by (unbalanced body),
  when it is variadic, over-long, defined twice with different bodies, or modelled directly
  (`MGP_NOTE_AGGREGATE`, whose hop is still read out of `MGPipeNoteAggregate`'s own switch).
* **The gate now declines instead of answering** wherever it cannot say it read every writer.
  Three decline reasons, all printed by `--check` with the site:
  * a body that still contains `##` after expansion, or that invokes a refused **token-pasting**
    macro. The taint is a `TAINT:<site>` token, so it rides the same call-graph fixed point the
    writes ride and a mutator that merely *reaches* an unreadable body is declined too;
  * a shutter member with a write-shaped occurrence **outside** the two analysed roots
    (`writers_outside()` scans all 589 `.h/.cpp` under `MobileGL/`; a file that *declares* a
    member of that name is writing its own, which is how `MG_Backend/MGPipe/PipeInputs.h`'s
    mirror of half of GLState's member names is kept from declining everything);
  * the pre-existing "the shutter reads something this script cannot resolve to a member".
* **`--check` prints its own footprint**: how many bodies and files the absence claim rests on,
  how many are tainted, how many members are written outside the roots, and the one place the
  analysis stays coarse (a `FIELD` token is matched by name and not scoped to a type — which
  only ever *widens* what a mutator is credited with writing, the safe direction here).
* **One precision fix found on the way**: the `BitwiseEqual` bits' shutter window now also
  starts at the last `}` before the `dirty |=`. Without it the pack block's trailing
  `m_pack = pack;` leaked into `NEW_PATCH_STATE`'s reader set (`pack` is a walk local, so it
  expanded to `ctx.GetPixelStoreParameters`) and the patch bit read as though it compared the
  pixel store.
* **The header of `DirtySurface.def` and the script's docstring now say what is true**: what
  the write analysis models, what it declines, and that its remaining one-directionality is the
  *presence* direction.

### 2.3 The verdict changing, and the negative control, in one run

`scratchpad/wf5/p2-tracker-v4/verdict.py` loads `06f9adfd`'s script (through a shadow repo
root under `/tmp`, so nothing is written into the tree) and the tip's, and asks both the same
questions:

```
BEFORE (06f9adfd)
  function names known to the write analysis: 932
  moved['SetPixelStoreParam'] MEM tokens: []
  moved['SetPixelStoreParam'] FIELD tokens: ['FIELD:paramNameTail']
  X(SetPixelStoreParam, NEW_PIXEL_PACK) -> UNDER-FIRING: ... it writes nothing NEW_PIXEL_PACK's
      shutter reads (shutter: m_pixelStorePackParameters, m_pixelStoreUnpackParameters) ...
  function names that may legally carry NEW_PIXEL_PACK: 0 []
  NEGATIVE CONTROLS, still red:
    SetNamedTransformFeedbackBinding   <- NEW_SO_TARGETS         UNDER-FIRING
    SetCurrentVertexAttributeInt       <- NEW_PIXEL_PACK         UNDER-FIRING
    SetPixelStoreParam                 <- NEW_SO_TARGETS         UNDER-FIRING
    BumpTextureBindGeneration          <- NEW_GLOBAL_CONSTANTS   UNDER-FIRING

AFTER  (working tree)
  function names known to the write analysis: 936
  moved['SetPixelStoreParam'] MEM tokens: ['MEM:m_pixelStorePackParameters',
                                           'MEM:m_pixelStoreUnpackParameters']
  moved['SetPixelStoreParam'] FIELD tokens: ['FIELD:Alignment', 'FIELD:ImageHeight',
      'FIELD:LSBFirst', 'FIELD:RowLength', 'FIELD:SkipImages', 'FIELD:SkipPixels',
      'FIELD:SkipRows', 'FIELD:SwapBytes']
  X(SetPixelStoreParam, NEW_PIXEL_PACK) -> ACCEPTED
  function names that may legally carry NEW_PIXEL_PACK: 1 ['SetPixelStoreParam']
  NEGATIVE CONTROLS, still red:
    SetNamedTransformFeedbackBinding   <- NEW_SO_TARGETS         UNDER-FIRING
    SetCurrentVertexAttributeInt       <- NEW_PIXEL_PACK         UNDER-FIRING
    SetPixelStoreParam                 <- NEW_SO_TARGETS         UNDER-FIRING
    BumpTextureBindGeneration          <- NEW_GLOBAL_CONSTANTS   UNDER-FIRING
```

`0 -> exactly 1`, and it is the right one. The discriminating power is not bought with false
acceptance: the full cross product (73 rows × the 16 non-render bits, run in memory) is
**1104 rejected, 64 accepted, 0 declined**, against 1135 / 33 / 0 at `06f9adfd` — the 31 extra
acceptances are the struct-valued members the analysis could not see before.

### 2.4 The row, re-decided on its merits

`X(SetPixelStoreParam, kPulledEveryVerb)` → **`X(SetPixelStoreParam, kPulledPartialShutter|NEW_PIXEL_PACK)`**.

Under the file's own "every path that MUTATES" rule the bare bit is still wrong: bit 2 is a
byte compare of the **pack** half alone (`Tracker.h:275-280`) and eight of the sixteen arms
write the unpack half, so a shutter narrowed to bit 2 would under-fire for half the setter.
What was wrong was the *other* half of the answer — telling a P3a reader that no shutter
exists. `kPulledPartialShutter` is a new documented answer, and the only one that may be
joined with a bit: it says "the pull is what holds on every mutating path; these bits move on
some of them". The named bit is checked exactly like any other bit answer, so a dead one is a
red gate; which rows *need* the answer stays a human judgement, because the derivation's
"it does move that shutter" direction over-approximates and can refute a named bit but cannot
find the rows that should have named one. Both facts are in the row's comment and in the
header. `SetNamedTransformFeedbackBinding` keeps `kPulledEveryVerb` — round 3's finding that
it moves no shutter on any path stands, and control 6 still proves the gate sees it.

### 2.5 `--self-test`, grown to match

`7 negative controls` → **`10 negative controls, all tripped; positive control OK`**:

| # | control | asserts |
|---|---|---|
| 8 | `kPulledPartialShutter` naming no bit | `BAD answer ... has to NAME the bits` |
| 9 | a mutator whose reachable text carries an unmodelled construct | it is **DECLINED** and produces **no** problem |
| 10 | a shutter member written outside the analysed roots | it is **DECLINED** and produces **no** problem |
| + | positive control: `SetPixelStoreParam` ← `NEW_PIXEL_PACK` | neither a problem nor a decline — it goes red the day the pasted writes go unread again |

Controls 9 and 10 are the ones that matter for this major: they fail the self-test if the gate
ever again converts "this script cannot read that" into "that mutator writes nothing".

---

## 3. Verification — every command and its actual output

All at `9ff6061c` in `~/w7/p2-tracker`. All three build dirs reported `ninja: no work to do.`
(the fix touches one script and one `.def`; neither is compiled, and no `#include` of
`DirtySurface.def` exists anywhere in the tree), so every binary below is the one round 3's
review measured, and §3.2–3.4 are re-runs on it rather than on a fresh build.

### 3.1 The package's own verification block (brief §C.1), in order

| command | output |
|---|---|
| `python3 scripts/gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` / `8 other bit answers derived from their shutter in Tracker.h (under-firing only); 0 declined; 24 rows carry a prose answer (kExplicitDestroy, kImmediate, kNoBackendRead, kPulledEveryVerb, kReverseChannel, kUnpublishedDestroy) that no derivation checks` / `the under-firing half read 1265 function bodies across 82 files under MobileGL/MG_State/GLState + MobileGL/MG_Impl/Pipe, macros expanded first; 0 of those bodies carry a construct it does not model …; 347 members are written outside those roots and any shutter that names one is declined; a FIELD token is matched by name and not scoped to a type, so it only ever WIDENS …` |
| `python3 scripts/gen_pipe_dirty_surface.py --self-test` | rc 0 — `10 negative controls, all tripped; positive control OK (SetPixelStoreParam's sixteen token-pasted writes are read)` |
| `python3 scripts/gen_pipe_dirty_surface.py --summary` | rc 0, 81 lines — unchanged shape, so E's still-`--summary` CI step keeps working |
| `python3 scripts/gen_pipe.py --check` | rc 0 — `inventory 477 rows … 0 UNMAPPED`, `generated files are up to date` |
| `cmake --build build-linux -j 12` + `symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text 10792579 -> 10792739 (+160, +0.001%)`; both files 19100448 bytes |
| `cmake --build build-push -j 12` + `ctest --test-dir build-push -L unit --no-tests=error` | rc 0 — `100% tests passed, 0 tests failed out of 1529` |
| `ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | rc 0 — `878 / 878` |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu --no-tests=error -j 4` | rc 0 — `878 / 878` (the all-pull arm) |
| `cmake --build build-verify -j 12` + `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | rc 0 — `818 / 818` |
| `MOBILEGL_PIPE_VERIFY=1 python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/v4-verify -j 4` | rc 0 — `passed 79 / 79; failed: []`, lowest SSIM **0.993475** |
| the brief's `grep -l 'Fatal{' <out>/*/mobilegl.log` | matches nothing **because the path is wrong** — see §5.1; at the real path `<out>/<case>/<backend>/output/mobilegl.log`: 79 logs, `Fatal{` **0** |

### 3.2 Section-A gates this tree can reach on its own

| gate | command | output |
|---|---|---|
| **G1** | as above | 0 added / 0 removed / 0 renamed, 4 resized — the contract's four (`RenderState::RenderState()`, `SetCapability`, `IsCapabilityEnabled`, `_GLOBAL__sub_I_DirectGLES.cpp`). **Round 4 adds zero pull-build delta**, as it must: it compiles nothing. |
| **G2** | `ctest -N` name sets from `build-linux` and `build-push`, extracted with `grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:' \| sed -E 's/^ *Test +#[0-9]+: //' \| LC_ALL=C sort` | `2407` and `2407`, **zero-line diff** |
| **G3** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-push/libMobileGL.so --out ~/w7/retrace-out/v4-push -j 4` | rc 0 — `passed 79 / 79; failed: []`; three lowest SSIM `0.993475  0.993475  0.995426` |
| **G4** | §3.1's verify retrace + the log corpus | 79 logs; `Fatal{` **0**; not-armed (`MGPipe: verify armed`) **0**; `PipeVerifyDiffer` **0**; `PipeResidualDiverged` **0**; `UnmigratedPipeInput` **0** |
| **G5** | `git show {p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G9** | above | green, and now green for a reason it can support |
| **G10** (desktop half) | `MOBILEGL_LOG_FILE_PATH=… MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=1 build-verify/…/MobileGLIntegrationTest --gtest_filter='*CrossFrameBufferScenario*'` | 13/13 pass; window 1 `resid=8.00 … cso[csom=1 csob=1]`, every later window `resid=0.00` |
| **G13** | `gen_pipe.py --check` / `--self-test` (`7 negative-control trip(s), positive control OK`), `check_include_closure.py` (`4 probes, 0 skipped, 0 problem(s)`), `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` (empty) | all green |
| **G14** | `comm -23 <sorted baseline> <build-linux names>` (LC_ALL=C on both sides **and** on `comm`) | **0** of 2363 baseline names removed; 44 added, all `Tracker*` / `CsoCacheTest` / `SetHashSuppressorTest` / the two A stubs |

Not reachable from this tree: **G6/G7** (package A's `MGPipeRenderStateSpans`), **G8/G12**
(package E), **G11** (device). Not claimed.

### 3.3 Test lanes

| lane | result |
|---|---|
| `ctest --test-dir build-linux -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-push -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-verify -L unit -j 8` | rc 0 — **1529 / 1529** |
| `ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** |
| `MOBILEGL_PIPE_PUSH=0 ctest --test-dir build-push -L integration-gpu -j 4` | rc 0 — **878 / 878** |
| `ctest --test-dir build-linux -L integration-gpu -j 4` | rc 0 — **878 / 878**, first try, no flake |
| `ctest --test-dir build-verify -L integration-verify -j 4` | rc 0 — **818 / 818** |

The applier-repair `MGLOG_W_ONCE` is still one line in **79 of 79** verify logs and **79 of 79**
push logs, as round 3 established.

### 3.4 The residual block's firing rate, measured (review v3 minor 1)

```
MOBILEGL_PIPE_STATS=1 MOBILEGL_PIPE_STATS_PERIOD=60 python3 ~/w7/retrace_gate.py \
  --lib build-push/libMobileGL.so --only 'minecraft-1.21.4-fabric-iris-bsl-in-world' …
MGPipe stats: frames=120 window=60 draws=2293 draws/f=38.22 … resid=197.07 … cso[csom=8 csob=1415]
```

197.07 B/frame ÷ 8 B × 60 frames = **1478 residual blocks per 60 frames, 0.64 per draw**. That
number, not `CrossFrameBufferScenario`'s `resid=8.00`, is the one that belongs beside the
`resid=` byte class in `MEASUREMENTS.md`. Both cases passed (SSIM 0.997496 / 0.997324). This
reproduces the reviewer's own measurement exactly; the code is unchanged this round.

---

## 4. The eleven minors of `tracker-review-v3.md`

Not in this round's scope, and recorded so the integrator is not told they are gone:

1. **the residual firing rate** — measured and recorded, §3.4 above. The `MEASUREMENTS.md` line
   is the integrator's.
2. **"7" is 7 (mutator, bit) pairs, not 7 rows** — now **8** pairs and the wording is unchanged,
   so the ambiguity stands. `--check`'s new footprint line says how many bodies and files the
   claim rests on, which is the same class of disclosure, but the word "pairs" is still absent.
3. **the tracker's context identity is a raw heap address** (`m_context != &ctx`) — open. A
   monotonic `GLContext` serial is the fix; not made this round.
4. **`MGPipeNoteAggregate` resolves through `LiveContext()`** rather than `this` — open.
5. **three emission globals survive the fixture reset** — open.
6. **`MGPipeVertexAttribDefaultsLastHeader()` is a production observable** — accepted, declared.
7. **the applier-repair warning lands in every device log** — accepted, hand-off §6.
8. **`MGPipeCsoCache` frees its slots only in `Reset()`** — open.
9. **`FillPoints.def`'s verdict is a keep-all with reasons** — correct scope, recorded.
10. **the outstanding list** — restated in §6.
11. **the three broken §A commands** — re-reproduced, §5.1.

---

## 5. Where the tree contradicts the brief

Rounds 1–3's list stands (`tracker-v2.md` §6, `tracker-v3.md` §6).

### 5.1 Three commands in §A still do not work as written (re-reproduced this round)

1. **G3 / C.2 / C.3's `--ssim 0.99`** — `retrace_gate.py --help` lists exactly
   `--tree --lib --out -j --only`; passing `--ssim 0.99` exits non-zero having run nothing.
   The threshold is internal.
2. **G2 / G14's `grep -E '^\s+Test #'`** — drops every test numbered under 1000.
   `'^[[:space:]]*Test[[:space:]]+#[0-9]+:'` is what matches all 2407, and both sides must be
   sorted **and compared** under `LC_ALL=C` — with the locale collation `comm` warns
   `file 1 is not in sorted order` and its answer is not trustworthy.
3. **G4's log glob and arming string** — the logs are at
   `<out>/<case>/<backend>/output/mobilegl.log`, not `<out>/<case>/mobilegl.log`, and the
   arming line is `MGPipe: verify armed`, not `MGPipe verify:`. Fixing only the path would
   turn a true green into a false red.

**§A and D.3 must be corrected before the integrator runs the five-part gate.**

### 5.2 The G10 stats command needs a log file on desktop

The stats line is `MGLOG_I`, so it does not reach `ctest -V`'s captured output; it needs
`MOBILEGL_LOG_FILE_PATH` and the integration binary run directly with a `--gtest_filter`.

---

## 6. Deviations from the brief

Rounds 1–3's fifteen are unchanged and still declared (header-only
`Tracker`/`CsoCache`/`SetHashSuppressor`; the sixth aggregate generation; `Core.{h,cpp}`;
coarse shutters for bits 5–17; the `Vector` scan in the CSO cache; `s_hashForTest`; the
residual block emitted after the fill; the runtime derivation probe; five stale comment
references; `|`-joined `.def` answers; `GLContext`'s push-only class array;
`MGPipeVertexAttribDefaultRepairCount`; `MGPipeVertexAttribDefaultsLastHeader`;
`SetPatchVertices`' answer class; and 15, `kPulledEveryVerb` carrying a row whose mutator
moves a real shutter on some paths). New in round 4:

16. **`kPulledPartialShutter` joins a prose answer with a bit, which D16 and this file's own
    former rule forbid.** It is one row (`SetPixelStoreParam`) and it **supersedes deviation
    15**, which was the wrong way to record the same fact: dropping the bit told a P3a reader
    that the mutator behind a shipped call has no shutter. The joined form is the only
    machine-readable way to say "the pull is the guarantee, and this bit moves on some of the
    paths"; `--check` rejects a `kPulledPartialShutter` that names no bit, rejects a named bit
    the mutator cannot move, and still rejects every other prose answer that does not stand
    alone.
17. **The derivation now reads outside its two roots.** `writers_outside()` opens all 589
    `.h/.cpp` under `MobileGL/` — not to derive anything from them, but to refuse to claim an
    absence over code it has not read. `--check` costs 1.5 s instead of 0.5 s.

---

## 7. Unfinished, and the hand-off

1. **Package A must teach `MGPipeApplySetVertexAttribDefaults` to switch on
   `MGPAttribValue::ValueClass`**; until then every push/verify process logs one
   `MGLOG_W_ONCE` about the mirror it cannot reproduce. No edit here is needed when it lands.
2. **The 26 derived render-state mirrors are still pulled on this branch**; the residual
   block's trip wire is still half an oracle while `MGPipeDeriveRenderStateFields` is a stub.
   Both become independent with A's `c1`.
3. **`kPulledEveryVerb` is still an unchecked absence claim.** The analysis can now refute a
   named bit, but pointing it at the seven `kPulledEveryVerb` rows as a gate would be wrong
   today: `SetNamedTransformFeedbackBinding` "moves" three bits' shutters by the coarse
   `FIELD:`/call-union route while moving none of them in fact, so the check would be red for
   its own imprecision rather than for the file being wrong. Recorded, not shipped.
4. **The write analysis is complete only over the constructs `--check` now names.** A callee
   whose body lies outside `MG_State/GLState + MG_Impl/Pipe` contributes nothing; a `FIELD`
   token is not scoped to a type. Both only widen, so neither can manufacture a false
   UNDER-FIRING through the fixed point, and the containment scan is what closes the member
   half — but it is a textual analysis and the file says so rather than implying otherwise.
5. **G9 is not a CI gate until package E lands** the `pipe-gates` step; `test.yml` is E's file
   and still runs `--summary` under an "Informational" comment.
6. **`MEASUREMENTS.md` items the integrator owns**: the admitted resized-symbol set is four
   symbols, not D15's three; D14/D.4.3's `T1 − T2` no longer isolates the tracker; the
   `*.IsActuallyArmedWhenTheEnvironmentPinsItOn` family belongs in the known-flake list by
   name; and §3.4's residual firing rate belongs beside the `resid=` byte class.
7. Unchanged from rounds 2–3: per-bit fire rates are not printed (`FormatWindowLine` is A's),
   the six other `SetHashSuppressor` slots are unwired, `MGPipeWidenedCounter` cannot see a
   change of exactly 65536, `s_hashForTest` is a live function pointer in production, the
   three header-only files want their `list(APPEND)` line, and there is no device work here
   (G11, D.4).
