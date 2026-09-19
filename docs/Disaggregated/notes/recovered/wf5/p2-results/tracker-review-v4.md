# Adversarial review — P2 package B `p2/tracker`, round 4 (`~/w7/p2-tracker` @ `9ff6061c`)

Reviewer's brief: refute the claim that `tracker-review-v3.md`'s major is closed and that nothing
regressed. Everything below is my own run in `~/w7/p2-tracker`, not a quotation from
`tracker-v4.md`. The tree was left exactly as found: `git status --porcelain` empty before and
after, `git rev-parse HEAD` still `9ff6061c65709ebe1bd1976e99cfea3fad9a02ef` on branch
`p2/tracker`, no source file edited. Every negative control and every counterfactual below was run
**in memory** against a loaded copy of the script; `06f9adfd`'s copy of the script was materialised
under `/tmp/rev-tk4-shadow/`, never inside the tree. Reproduction driver:
`scratchpad/wf5/rev-tk4/repro_major.py` (read-only, run from `~/w7/p2-tracker`).

**Verdict: NOT APPROVED — 2 majors.**

Round 3's major is closed *in its narrow form* — I re-ran its own reproduction and the outcome
changed for the stated reason, not by routing around (§2). Every gate this tree can reach is green
and I reproduced all of them, including both long retraces (§1). What blocks approval is that the
same round-4 commit (a) leaves the write analysis an under-approximation through a second
construct that is neither modelled nor declined, so the gate **still prints a false statement about
`RenderState.cpp`** — the exact thing round 3 refuted — and (b) collapsed the under-firing check for
`NEW_PATCH_STATE`, one of the five bits P2 emits for, from 5 legal carriers to 35, so a false row on
any of the 30 `RenderState` setters is now green where it was red one commit ago. Neither is
disclosed; `tracker-v4.md` §2.3 records (b) as "the 31 extra acceptances are the struct-valued
members the analysis could not see before".

---

## 1. What I re-ran, and what it printed

Round 4 is 2 files (`MobileGL/MG_Pipe/DirtySurface.def` +85/−22, `scripts/gen_pipe_dirty_surface.py`
+500/−54) and **compiles nothing**: `cmake --build {build-linux,build-push,build-verify} -j 12` →
`ninja: no work to do.` in all three, and the only occurrence of the string `DirtySurface.def`
outside the file itself is a comment (`MobileGL/MG_Impl/Pipe/PipeFill.cpp:926`). So the binaries
below are the ones round 3 measured, and I re-measured them rather than taking that on trust.

| gate | command | observed |
|---|---|---|
| **G1** | `python3 scripts/symbol_report.py --before ~/w7/p2-before-libMobileGL.so --after build-linux/libMobileGL.so --threshold 0` | rc 0 — `27799 -> 27799 defined symbols: 0 added, 0 removed, 4 resized, 0 renamed`; `.text 10792579 -> 10792739 (+160, +0.001%)`; both files 19100448 bytes. Resized: `RenderState::RenderState()` +148, `SetCapability` +77, `IsCapabilityEnabled` +29, `_GLOBAL__sub_I_DirectGLES.cpp` −9 — the contract's four. **Round 4 adds zero pull-build delta.** |
| **G2** | `ctest --test-dir <d> -N \| grep -E '^[[:space:]]*Test[[:space:]]+#[0-9]+:' \| sed -E 's/^ *Test +#[0-9]+: //' \| LC_ALL=C sort` | `build-linux 2407`, `build-push 2407`, **zero-line diff** (`build-verify` 3225, superset) |
| **G14** | `LC_ALL=C comm -23 <sorted baseline> <build-linux names>` | **0** of 2363 baseline names removed; 44 added |
| **G5** | `git show {refs/tags/p2/contract,HEAD}:…/DirectGLES.cpp \| awk '/namespace RenderStateImpl/,…' \| sha256sum` | `d8fd1c48716056c536752fde09db3a9e5aa8494b70404798bfc15d11220efe27` on both sides **and** equal to `~/w7/p2-before-syncrenderstate.sha` |
| **G3** | `python3 ~/w7/retrace_gate.py --tree ~/w7/p2-tracker --lib build-push/libMobileGL.so --out ~/w7/retrace-out/rev-tk4-push -j 4` | rc 0 — `passed 79 / 79; failed: []` |
| **G4** | `ctest --test-dir build-verify -L integration-verify --no-tests=error -j 4` | `100% tests passed, 0 tests failed out of 818` |
| **G4** | `MOBILEGL_PIPE_VERIFY=1 … --lib build-verify/libMobileGL.so --out ~/w7/retrace-out/rev-tk4-verify -j 4` | rc 0 — `passed 79 / 79; failed: []`; over the 79 `<case>/<backend>/output/mobilegl.log`: `Fatal{` **0**, not-armed (`MGPipe: verify armed`) **0**, `PipeVerifyDiffer` **0**, `PipeResidualDiverged` **0**, `UnmigratedPipeInput` **0** |
| **G9** | `gen_pipe_dirty_surface.py --check` | rc 0 — `73 mutators, all mapped, no stale rows; 45 render-state answers derived from RenderState.cpp and matching` / `8 other bit answers derived from their shutter in Tracker.h (under-firing only); 0 declined; 24 rows carry a prose answer …` / the new footprint line (`1265 function bodies across 82 files … 0 … tainted … 347 members written outside those roots`) |
| **G9** | `… --self-test` | rc 0 — `10 negative controls, all tripped; positive control OK` |
| **G9** | `… --summary` | rc 0, 81 lines — unchanged shape, E's `--summary` CI step still works |
| **G13** | `gen_pipe.py --check` / `--self-test` | rc 0 / rc 0 (`7 negative-control trip(s), positive control OK`); `inventory 477 rows … 0 UNMAPPED`; `generated files are up to date` |
| **G13** | `check_include_closure.py` | rc 0 — `4 probes, 0 skipped, 0 problem(s)` |
| **G13** | `grep -rc 'pGLContext' MobileGL/MG_Backend \| grep -v ':0$'` | empty; no stdio in any file of the package diff |
| unit | `ctest -L unit -j 8` in build-linux / build-push / build-verify | `100% tests passed, 0 tests failed out of 1529` ×3 |
| integration | `ctest -L integration-gpu -j 4` in build-push / build-linux, and `MOBILEGL_PIPE_PUSH=0` in build-push | `878 / 878` ×3, no flake |
| G6/G7, G8/G12, G11 | — | not reachable from this tree (packages A / E / device). Correctly not claimed. |

**Ownership and discipline.** `git diff --name-only refs/tags/p2/contract..HEAD` → 26 files, the
same set round 3 cleared; round 4 touches only `MG_Pipe/DirtySurface.def` and
`scripts/gen_pipe_dirty_surface.py`, both **B** rows of C.5. `git diff --summary
refs/tags/p2/contract..HEAD` prints four `create mode 100644` lines (the three header-only files
and the `.def`) and **no `mode change` line**. 15 commits, every subject a single-line
`[Type] (Scope): …`, bodies are `- ` bullets, `git log --format=%B` contains no `Co-Authored-By`
and no other attribution. Nothing pushed.

---

## 2. Round-3 MAJOR 1, re-run — closed in its narrow form, for the stated reason

I re-ran round 3's own reproduction against the tip rather than trusting `tracker-v4.md` §2.3.

```
$ python3 scratchpad/wf5/rev-tk4/probe1.py        # loads the tree's script, mutates dicts in memory
 function names known: 936
 moved['SetPixelStoreParam'] MEM: ['MEM:m_pixelStorePackParameters', 'MEM:m_pixelStoreUnpackParameters']
 NEW_PIXEL_PACK shutter: ['MEM:m_pixelStorePackParameters', 'MEM:m_pixelStoreUnpackParameters'] resolved=True
 names that may legally carry NEW_PIXEL_PACK: 1 ['SetPixelStoreParam']
 shipped row for SetPixelStoreParam: kPulledPartialShutter|NEW_PIXEL_PACK
```

against `0 []` at `06f9adfd` (I re-derived that from `06f9adfd`'s own script in the shadow root,
not from the previous review). The thing that was wrong is demonstrably right:

* the writes are now *read*, not worked around — `MEMBER_ROOTED_WRITE_RE`
  (`scripts/gen_pipe_dirty_surface.py:332-334`) records `MEM:m_x` for `m_x.f`/`m_x[i].f`/`m_x->f`,
  and `macro_table`/`expand_macros` (`:427-506`) perform the `##` pasting, so
  `RenderState.cpp:827-855`'s sixteen writes appear as the two real members instead of
  `FIELD:paramNameTail`;
* the false `--check` message about `RenderState.cpp:829-833` is gone, and the bit is no longer
  un-nameable (`0 -> 1`, and it is the right function);
* `DirtySurface.def:213` no longer tells a P3a reader that the mutator behind the shipped
  `set_pixel_pack_state` has no shutter; the row is checked like any other bit answer, and I
  confirmed the check bites: swapping the named bit for `NEW_SO_TARGETS` or moving the answer to a
  different mutator both go red (§3 probe output);
* `--self-test`'s positive control discriminates — at `06f9adfd` `NEW_PIXEL_PACK` had **0** legal
  carriers, so the control would have failed there;
* controls 6 and 7 (the two shipped historical defects) still trip, and control 8
  (`kPulledPartialShutter` naming no bit) trips: `X(SetNamedTransformFeedbackBinding,
  kPulledPartialShutter)` → `BAD answer … has to NAME the bits`.

What is **not** closed is the class claim the fix makes about itself. `DirtySurface.def:49-62` and
`gen_pipe_dirty_surface.py:263-290` now assert that the derivation "DECLINES rather than answers
whenever it cannot say it read every writer", with exactly three decline reasons. Two further
write constructs are neither modelled nor declined, and one of them is live in the same file the
original false verdict was about.

---

## 3. Majors

### MAJOR 1 — the write analysis is still an under-approximation: a member-rooted write bound to a **reference** records no `MEM:` token, so the gate still states an absence that is false about `RenderState.cpp`

**The claim under refutation.** `MobileGL/MG_Pipe/DirtySurface.def:56-62`:

> `So the derivation now DECLINES rather than answers whenever it cannot say it read every`
> `writer: a body carrying a construct it does not model (an unexpandable token paste), anything`
> `that reaches such a body, and any shutter member written outside MG_State/GLState +`
> `MG_Impl/Pipe at all.`

and `scripts/gen_pipe_dirty_surface.py:277-283`, whose `modelled` list is
`++m_x / m_x = / m_x op=`, `m_x.f`, `m_x[i].f`, `m_x->f`, a bare `.f =`, `MGP_NOTE_AGGREGATE`,
calls into the roots, and expandable macros.

**A fourth shape exists and is missing from both lists: the member-rooted lvalue bound to a
reference.** `MEMBER_ROOTED_WRITE_RE` (`:332-334`) requires the member name and the assignment to
be in the *same* expression. `RenderState::SetBlendEquation`
(`MobileGL/MG_State/GLState/RenderState/RenderState.cpp:561-573`) does not write that way:

```cpp
561  void RenderState::SetBlendEquation(BlendEquation color, BlendEquation alpha) {
563      for (auto& blendState : m_parameters.BlendStates) {
567          blendState.ColorEquation = color;
568          blendState.AlphaEquation = alpha;
```

`m_parameters` never touches an assignment operator, so `written_tokens` (`:578-611`) records
`FIELD:ColorEquation`/`FIELD:AlphaEquation` and **no `MEM:m_parameters`** — the identical failure
mode round 3 refuted (`FIELD` recorded, `MEM` not, while `resolve_reader` (`:643-664`) resolves the
shutter's accessor to `MEM:m_parameters`). It is not a token paste, so `unmodelled_sites`
(`:511-522`) raises no taint; it is inside the analysed roots, so `writers_outside` (`:551-575`)
raises none either. The gate therefore answers, and its answer is false about the file:

```
$ python3 scratchpad/wf5/rev-tk4/repro_major.py     # read-only, dicts mutated in memory
moved['SetBlendEquation'] MEM tokens: ['MEM:m_bufferBindingPointBase', 'MEM:m_bufferBindingSlot',
                                       'MEM:m_pipelineStateVersion', 'MEM:m_touchedBindingPointCount',
                                       'MEM:m_version']
   UNDER-FIRING answer NEW_PATCH_STATE for SetBlendEquation - it writes nothing NEW_PATCH_STATE's
   shutter reads (shutter: PatchDefaultInnerLevel, PatchDefaultOuterLevel, PatchVertices,
   m_parameters), so a mutation through it publishes nothing
```

`SetBlendEquation` writes `m_parameters` on every mutating path. The sentence is untrue of
`RenderState.cpp:563-568`, and it is produced by the same "cannot read it ⇒ it is not there"
substitution the commit says it removed.

**Scope, enumerated rather than asserted** (`scratchpad/wf5/rev-tk4/probe6.py`, which expands the
macros exactly as the script does and then asks which bodies write `m_parameters` only through a
reference):

```
RenderState.cpp setters that write m_parameters ONLY through a reference alias and are
NOT credited with MEM:m_parameters by the write analysis: 7
   SetBlendFunc, SetBlendFuncIndexed, SetBlendEquation, SetBlendEquationIndexed,
   SetStencilFunc, SetStencilMask, SetStencilOp
```

Seven of `RenderState.cpp`'s setters, plus the same shape wherever it recurs
(`MobileGL/MG_State/GLState/VertexArrayState/VertexArrayState.cpp:65` binds `auto& vao =
m_vertexArrays[index];`, and `m_vertexArrays.push_back/.resize/.reserve` at `:24,62,63` and
`m_currentProgram.reset()` at `ProgramState.cpp:70` are member mutations the analysis reads as
nothing at all — those three happen to be redundant today because the same bodies also carry a
direct write, which is luck, not a property).

**Why it is a major and not a nit.**

1. `ROADMAP.md:7` — 每个门必须能因它存在的理由变红. The gate's only failing verdict is an absence
   claim, and it still emits that claim over code it demonstrably has not read. It is the same
   defect, one construct over, in the same file, in the same gate, in the commit that says it
   removed it.
2. It is **undeclared**: `tracker-v4.md` §6 adds deviations 16 and 17 and §7.4 lists the
   remaining limits as "a callee outside the roots contributes nothing" and "a `FIELD` token is not
   scoped to a type", both said to *widen*. This one narrows, which is the direction that
   manufactures a false red.
3. It is not only prospective. It is what makes the gate's verdicts on the seven listed setters
   arbitrary today (`SetClearColor` accepted, `SetBlendEquation` rejected, for the same false
   claim — see MAJOR 2), and P3a is told to build shutters from this file.

**Minimum to clear.** Either model the shape — resolve a reference initialised from a
member-rooted lvalue and credit its writes to that member — or add it to the decline list: a body
that binds a reference (or takes the address of, or calls a mutating member function on) a shutter
member, and cannot be read further, has to come out DECLINED, with a self-test control that fails
if a future edit turns that back into a verdict. Either way `DirtySurface.def:56-62` and
`gen_pipe_dirty_surface.py:277-290` must stop claiming a completeness they do not have.

### MAJOR 2 — the same commit made `NEW_PATCH_STATE`'s under-firing check unfalsifiable for the whole `RenderState` setter family: 5 legal carriers → 35, and a false row that was red at `06f9adfd` is green at HEAD

`NEW_PATCH_STATE` is one of the five bits P2 emits for (`MobileGL/MG_Impl/Pipe/Tracker.h:90-93`),
and its shutter is built at `Tracker.h:282-292` out of `render.PatchVertices` /
`render.PatchDefaultOuterLevel` / `render.PatchDefaultInnerLevel`, where `render` is the whole
`RenderStateParameters`. `resolve_reader` therefore resolves it to `MEM:m_parameters` **plus** the
three `FIELD:` tokens. Before round 4 that `MEM:m_parameters` was inert, because no writer was ever
credited with it. Round 4's (correct) `MEMBER_ROOTED_WRITE_RE` now credits it to every setter that
writes any byte of `m_parameters` in one expression — which is most of `RenderState.cpp`. The two
halves of the check now meet for reasons that have nothing to do with the patch state:

```
$ python3 scratchpad/wf5/rev-tk4/repro_major.py
  function names that may legally carry NEW_PATCH_STATE: 06f9adfd=5  HEAD=35
  X(SetClearColor, NEW_RENDER_STATE|NEW_PATCH_STATE)
     HEAD     -> NO PROBLEM - --check is green on it
     06f9adfd -> ['UNDER-FIRING answer NEW_PATCH_STATE for SetClearColor - it writes noth...']
  X(SetScissorBox, NEW_RENDER_STATE|NEW_PATCH_STATE)
     HEAD     -> NO PROBLEM - --check is green on it
     06f9adfd -> ['UNDER-FIRING answer NEW_PATCH_STATE for SetScissorBox - it writes noth...']
  X(SetViewportIndexed, NEW_RENDER_STATE|NEW_PATCH_STATE)
     HEAD     -> NO PROBLEM - --check is green on it
     06f9adfd -> ['UNDER-FIRING answer NEW_PATCH_STATE for SetViewportIndexed - it writes...']
```

`glClearColor` does not move the patch trio; `Tracker.h:282-292` compares seven floats and an
integer and nothing else. The full old-vs-new verdict diff over the shipped 73 rows × the 16
non-render bits (`scratchpad/wf5/rev-tk4/probe3.py`) is:

```
total flips old->new: 31        Counter({('REJ', 'ACC'): 31})
REJ -> ACC:  SetPixelStoreParam <- NEW_PIXEL_PACK          (the intended fix)
             30 x Set<anything> <- NEW_PATCH_STATE          (the collapse)
```

Consequences, all present rather than prospective:

1. **A test that cannot fail.** For 30 of the 73 rows — and they are precisely the population that
   could plausibly carry a wrong `NEW_PATCH_STATE` — the under-firing check has no failing input.
   The bit's own negative control does not exist: `--self-test`'s control 7 uses
   `SetCurrentVertexAttributeInt ← NEW_PIXEL_PACK`, which still trips only because that shutter's
   members are narrow. Point it at `NEW_PATCH_STATE` and any `RenderState` setter and it goes green.
2. **A verdict that is now unsupported.** `--check`'s `8 other bit answers derived from their
   shutter in Tracker.h` counts the three shipped patch rows (`DirtySurface.def:214,215,219`) as
   derived-and-matching; after this commit those three verify for writing *any* byte of
   `m_parameters`, so they would verify identically if `SetPatchVertices` wrote only `ClearColor`.
   Five of the eight are independently supported; three carry no information.
3. **Undisclosed, and mis-described.** `tracker-v4.md` §2.3 reports the flips as
   "the 31 extra acceptances are the struct-valued members the analysis could not see before".
   `--check`'s new footprint line (`gen_pipe_dirty_surface.py:1162-1168`) names the FIELD-token
   coarseness as "the one place the analysis stays coarse" and says it "only ever WIDENS what a
   mutator is credited with writing" — the coarseness that actually dominates is on the **reader**
   side (a shutter that resolves to a whole struct), it is not named anywhere, and its effect is to
   widen what counts as *moving the shutter*, which is what disarms the check. The commit message's
   own bullet about the `BitwiseEqual` window says the pixel store no longer leaks into
   `NEW_PATCH_STATE`'s reader set; `MEM:m_parameters` is the bigger leak and it stayed.

I verified the narrowing half of that window fix is real and did not damage anything else: over all
18 bits, `NEW_PATCH_STATE` is the **only** shutter whose reader set changed this round, and it lost
exactly `m_pixelStorePack/UnpackParameters` (`scratchpad/wf5/rev-tk4/probe7.py`).

**Minimum to clear.** Make the patch shutter's reader set as narrow as the bit is — resolve
`render.<Field>` to a member+field pair rather than letting the `render` local also contribute the
bare `MEM:m_parameters`, or require a `FIELD:` match when the shutter names one — and add the
`NEW_PATCH_STATE` analogue of control 7 so the collapse cannot come back silently. If it is decided
that the coarseness is acceptable, then the three patch rows must stop being counted as "derived"
and the footprint line must name this as the place the analysis is coarse.

---

## 4. Minors

1. **`writers_outside()` shares the write analysis's blind spots** (`gen_pipe_dirty_surface.py:551-575`).
   It scans with `MEMBER_WRITE_RE` and `MEMBER_ROOTED_WRITE_RE` only, so an outside writer that
   mutates through a reference alias, `memcpy`, or a mutating member function (`m_x.push_back(…)`,
   `m_x.reset()`) is invisible to the containment scan too. The scan is the mechanism that is
   supposed to make the absence claim safe over unread code; it inherits the same gap it protects
   against. (I checked the current tree: only `MEM:m_pipelineStateVersion` has an outside writer
   after the declaration exclusion — `MG_Pipe/PipeApply.cpp` — and it is a render bit, so nothing
   declines today.)
2. **`macro_table()` collects `#define`s only under the two roots.** A token-pasting macro defined
   in `MG_Util`/`MG_Pipe`/`MG_Common` and invoked inside `MG_State/GLState` would be neither
   expanded nor recorded as refused, so no taint. Today the frontier is clean — the macro-shaped
   invocations inside analysed bodies are only `MOBILEGL_ASSERT`, `MGLOG_*`, `MGP_NOTE_AGGREGATE`,
   `MGP_NOTE_MUTATION` and `XXH64` (`probe1.py` §C) — but the decline machinery's coverage claim
   rests on that staying true.
3. **`expand_macros(…, rounds=4)` (`:507-509`) gives up silently.** A nest deeper than four leaves
   no `##` in the text, so `unmodelled_sites` sees nothing and the body is answered, not declined.
4. **The decline path never fires on the real tree** (`--check`: `0 declined`, `0` tainted bodies),
   so controls 9 and 10 are the only things exercising it, against synthetic dictionaries. That is
   the right shape, but it means a regression in the real decline plumbing is invisible to `--check`.
5. **`--check`'s "8 other bit answers" is 8 (mutator, bit) pairs**, not 8 rows — round 3's minor 2,
   still open — and after MAJOR 2 three of the eight are vacuous, so the honest count of
   independently supported non-render answers is **5**. One word plus MAJOR 2's fix would make the
   line say what it means.
6. **Round 3's §A command defects reproduce for me too**, so §A and D.3 still need correcting
   before the integrator runs the five-part gate: `retrace_gate.py` has no `--ssim` option;
   `grep -E '^\s+Test #'` matches 1408 of this tree's 2407 names and both sides must be sorted and
   compared under `LC_ALL=C`; G4's verify logs are at `<out>/<case>/<backend>/output/mobilegl.log`
   and the arming line is `MGPipe: verify armed`, not `MGPipe verify:`.
7. **Round 3's minors 3, 4, 5 and 8 are open and correctly declared open** in `tracker-v4.md` §4:
   the tracker keys context identity on a raw heap address (`Tracker.h:192-195`);
   `MGPipeNoteAggregate` resolves through `LiveContext()` rather than `this`
   (`PipeFill.cpp:518-543`); three emission globals survive `ResetTheServerSideSingletons()`;
   `MGPipeCsoCache` frees slots only in `Reset()`. None was touched this round, as stated.
8. **No lane started skipping this round** (nothing was compiled). The skip lists I saw are the
   deliberate ones: `build-linux` skips the two `SetHashSuppressorTest` cases and `build-push` skips
   the two verify-only `PipeInputsTest` cases — commit `7993d711`'s "skip instead of vanish", which
   is what keeps G2's `2407 = 2407` true. Recorded so the integrator does not read the skip list as
   a regression.
9. **Still outstanding and correctly recorded by B**: G6/G7 are package A's, G8/G12 package E's,
   G11 the device's; G9 is not a CI gate until E lands `pipe-gates`; G1's admitted resize set is
   four symbols, not D15's three; `D14`/D.4.3's `T1 − T2` no longer isolates the tracker;
   `MGPipeWidenedCounter` cannot see a change of exactly 65536; `s_hashForTest` is a live function
   pointer in production; the three header-only files still want their `list(APPEND)` line.

---

## 5. Bottom line

The machinery is sound and I verified all of it independently at `9ff6061c`: G1 (0/0/0, four
attributed resizes, byte-identical file size), G2 (2407 = 2407, zero-line diff), G3 (79/79 push),
G4 (818/818 plus 79/79 verify with a clean log corpus), G5 (byte-identical `RenderStateImpl`,
equal to the pre-P2 baseline), G9, G13 and G14 all reproduce; three unit lanes are 1529/1529; three
integration-gpu arms are 878/878 with no flake; ownership, mode bits and commit discipline are
clean; and round 3's major is genuinely fixed for the reason it was raised — the writes are read
now, the false message is gone, the bit is nameable by exactly the right function, and the row it
forced has been re-decided rather than routed around.

What blocks approval is that the same commit did not close the *class* of the defect and opened a
second one in the same gate. It still converts "this script cannot read that construct" into "that
mutator writes nothing" — for the seven `RenderState` setters that write through a reference, about
the same file, in the same words — while its header now claims it declines instead. And by teaching
the writer side to see `m_parameters`, without narrowing a reader side that resolves a whole struct,
it disarmed the under-firing check for `NEW_PATCH_STATE` across 30 of the 73 rows: a row saying
`glClearColor` publishes the patch state was red one commit ago and is green now. Nothing renders
wrong, no lane can go red for either, and the result file reports the second one as a gain — which
is exactly why they are worth another round.
