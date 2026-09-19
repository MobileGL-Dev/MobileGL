# gl-v1 — the gate mechanism, and two latent crashes

Branch `p5e/gl`, base `7c6f6886`, head `e5b74028`. Ten commits; all eight brief items landed, plus
ID-125 and ID-128 which arrived mid-task.

| # | item | commit |
|---|---|---|
| 1 | barriered stamp asks the server's capability (ID-111) | `8df7861f` |
| 2 | compile-time trip wire on the residual fill (ID-112) | `f94752c0` |
| 3 | admission predicate, generated (ID-116) | `a211756a` |
| — | retiring-phase disjunct (ID-125) | `faf19a6a` |
| — | escalation disjunct (ID-128) | `a0836568` |
| 4 | strict knob becomes admission-aware (ID-117) | `4f492edb` |
| 5 | `ResourceCopyRegion` becomes barriered (ID-118) | `a6ddeb08` |
| 6 | lane reads every log, counts, positive control (ID-119/115) | `bc6e24ff` |
| 7 | Magma's entries get their own lane | `664cbf3e` |
| 8 | amendment 8 marked UNLANDED (ID-120) | `e5b74028` |

## Gate

| step | result | delta and why |
|---|---|---|
| `build` | rc=0 | |
| `unit` | **2254/2254** | +4 split-only cases (one per red-once: ID-111, ID-116, ID-117, ID-128). One rename, no deletions. |
| `isplit` | **117/117** | +3 for the new `TriangleScenario` case (plain split, `SmallRing`, and its own `StrictArming` block); −2 for the DirectVulkan `NamedBlit` pair moving to `integration-magma-split`. |
| `gpu` | **1299/1299** | +5: the same case is discovered by five gpu-labelled blocks and skips loudly in four. |
| `strict` | 49/117, 68 abort | was 7/116. The 68 are exactly `pa`'s and `mv`'s rows — table below. |
| `integration-magma-split` | 0/2 under strict, **2/2 without** | the expected-red step, asserted red *and* asserted on its marker. |
| `gens` | all rc=0 | incl. `--check` and `--self-test` (18 negative + 13 derivation controls). The one `check_doc_citations` line (`CONTRACT-P5.md:524`) is pre-existing in a file I did not touch. |
| `flavours` | **rc=1, not mine** | breaks in `DirectGLES/DirectGLES.cpp:5575,6716` and `Managers.cpp:9081,9344` on `keysMatch`, `ComputeShaderStorageBlockBindingSignatureOf`, `StagedTextureTargetForPipeTarget`. My diff contains **no DirectGLES file**; `git log -S` puts those symbols in `49aab57f` / `72e79020`, both ancestors of my base. ID-124's finding, unchanged. |

## The marker table (this head — base `7c6f6886`, so *before* `pa` and `mv`)

Two independent instruments agreeing exactly: my in-lane `split_log_paths.py markers` (reads the
entries' own private logs) and the integrator's `strict_census3.py` (re-runs each entry under its
own command and environment). 113 of 113 log-declaring entries read; 117 lane entries.

**`Fatal{` — must be only what `pa` and `mv` owe. It is:** `GetProgramForDraw@DrawArrays` 52
(**pa**), `GetBoundVertexArray@DrawArrays` 9 (**mv**), `GetProgramForDispatch@DispatchCompute` 7
(**pa**). Nothing else. `GetFramebufferBindingSlot@Clear` (Magma, 2) has left this table — item 7.

**`Admitted{`, by the generated table:**

| pair | entries | admitted by |
|---|---|---|
| `GetBufferBindingSlot@ReadPixels` | 29 | disjunct 2 (P8/P9/P13) |
| `GetFramebufferBindingSlot@ReadPixels` | 29 | disjunct 1 (`read_pixels` is `kWaitReply`) |
| `GetTransformFeedbackProgram@DrawArrays` | 11 | disjunct 2 (P3b/P4b), also 3 |
| `GetTextureObject@CopyImageSubData` | 6 | disjuncts 1 **and** 2, independently |
| `GetProgramObject@ShaderStorageBlockBinding` | 2 | disjunct 2 (P9) |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | disjunct 1 (`kWaitApplied`) |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | disjuncts 1 and 2 |

**`ADMITTED-ESCALATED` (disjunct 3 only):** `GetProgramForDraw@DrawArrays` 13,
`GetBufferBindingPoint@DrawArrays` 11, `GetBoundVertexArray@DrawArrays` 2.

The first is not a contradiction with the Fatal table: the same pair is Fatal in 52 entries and
admitted in 13. The 13 are draws the client is parked behind because the *payload* escalated them
(open XFB span, or client arrays); the 52 are ordinary draws. That is exactly the distinction
ID-128 exists to draw, and no table indexed by (field, verb) could have drawn it.

### Reconciling with ID-128's census of the merged tree

ID-128 measured 180 entries at `3c13fa4b`; I measure 117 at `7c6f6886`. Differences, all accounted
for, none of them a rule I bent:

- `pa`/`mv` are not in my base, so their three Fatal rows are still here.
- `GetFramebufferBindingSlot@Clear` is absent from my Espryt table **by item 7**, not by admission
  (0/2 red in `integration-magma-split` with its marker asserted).
- `GetBufferBindingSlot@ReadPixels`, `GetProgramObject@ShaderStorageBlockBinding` and
  `GetBufferBindingPoint@DrawArrays` appear in mine and not in ID-128's table. I believe this is
  instrument, not disagreement — both of mine record *every* marker per entry, a first-fault census
  records one — but I cannot check that from here. Flagging rather than assuming.

## Item by item

**1 · ID-111** (`PipeApplier.h/.cpp`). The stamp is now
`MGPipeApplierStampsBarriered(waitRuleLanded, serverPublishesRunAhead, wireSaysBarriered)`, a pure
function; both predicates still computed unconditionally, as arguments rather than short-circuit
arms (ID-103 extended to the capability probe). `ServerPublishesRunAhead` promoted out of the
anonymous namespace, body unchanged, so the red-once can say "this session lacks bit 10" in its own
words (ID-102). **It is a pure function because the brief's red-once needs it to be:**
`kMGPipeP5eClientWaitRuleLanded` is false today, so a case that only drove `ApplyOne` would pass
with or without the conjunct — and keep passing until the integration commit, at which point Magma
SIGSEGVs with the lane green. So the case drives a real `DrawVbo` through `ApplyOne` on a
`SetCapabilityBits(0)` session and asserts the stamp **at the post-flip value of the constant**.
Red-once, deleting `serverPublishesRunAhead &&`:
`../MobileGL/MG_Test/Wire/RemoteClientControls.inc:143: Failure / Expected: true / exited 124`.

**2 · ID-112** (`PipeFill.cpp`, +45 lines after the existing assertion; I did not touch
`EmittedCallSuppliesTheWholeField` itself). The old assertion is keyed on the emitter's
*subsystem*, so it covered five of seven; `GetBoundVertexArray` and `GetBufferBindingPoint` are
outside `kMGPipeP4aFamilySubsystems`. All seven are now named in one list with their own assertion,
a size assertion, and a comment saying where a fired assertion sends you (P7/P8, not this table).
Red-once, deleting the `GetBoundVertexArray` case:
`PipeFill.cpp:2720:23: error: static assertion failed due to requirement
'NoPointerBackedRowIsWhollySupplied()': a pointer-backed residual row stopped being pulled (ID-112).`
**The pre-existing `NoP4aFamilyFieldIsWhollySupplied` stayed silent on that same edit** — the gap
ID-112 names, now measured.

**3 · ID-116** (generator +394, `.inc` +126, `test.yml` hand-kept 13 rows deleted, one unit case +
skip twin). The generator reads `kMGPipeWaitClasses`, `kMGPipeVerbClass` and
`kMGPipeClassFieldMask` and emits `kMGPipeAdmittedPullMask` + `MGPipeBarrierPullAdmitted`, with a
`static_assert` that every admitted bit is a BARRIER-PULLED field of its verb's class;
`--print-admitted` hands CI the same table. Final rule after ID-125/128: `BARRIER_PULLED` ∧ the verb
has a stamp row ∧ in the class mask ∧ (statically barriered ∨ retiring phase ≠ P5e ∨ escalated at
runtime). Static part: **174 pairs across 23 stamped verbs**. Three new negative controls (each
source renamed away must stop the script — a missing source yields a silently **empty** allowlist,
which reads as rigour and is blindness) and nine derivation controls. Red-once, dropping the
wait-class conjunct: `self-test: 3 admission-derivation control(s) did not hold: unbarriering
ReadPixels' op removes every @ReadPixels pair; …` plus the unit case's
`GetBoundVertexArray@DrawArrays is admitted on a verb no barriered op stamps`.

**ID-125.** Implemented as ruled; the phase column is matched as a **token** (`\bP\d+[a-z]*\b`), not
a substring, so a `P5` or a future `P5e2` cannot answer for `P5e`. 111 → 174 pairs. All six
survivors and all three rejections verified (§Rulings for the sixth). **One consequence you should
know:** not one of the seven sticky forwards names P5e (P7, P7/P9, P7/P13, P9), so after ID-125 no
sticky forward can produce a `Fatal{` on a stamped verb — they are all admitted.
`StrictErrorsAlsoPromotesTheStickyForwards` therefore stopped going red. That is the "check, do not
assume" instruction in item 4 paying off. I renamed it
`StrictErrorsReachTheStickyForwardsAndNameThemByField` and restated it around the claim that still
has teeth — the detector *reaches* them (asserted by `rsp` moving and by the marker naming field and
phase), so "did not abort" cannot pass for "never detected". The Fatal half keeps its own case next
door on `GetBoundVertexArray`, a P5e row; `StrictErrorsTurnsABarrierPulledReadIntoANamedAbort` still
goes red, unchanged.

**ID-128.** `PipeApply.h/.cpp` gain `MGPipeApplierCurrentRecordIsBarrieredByEscalation()` and its
setter — a second `thread_local` defaulting **false**, the answer that admits nothing;
`ApplyOne` stamps it as `wireSaysBarriered && MGPipeWaitClassFor(op) == kWaitNone`; the strict arm
reads it as disjunct 3. **The marker says which disjunct** (`ADMITTED` vs `ADMITTED-ESCALATED`), in
the slot the grammar already had for the reason, so `Fatal{UnmigratedPipeInput` filters are
untouched. That is what keeps the lane exact: an `ADMITTED` pair is checked against
`--print-admitted`, an escalated one is not — it is a runtime fact about the payload, and widening
the static list to hold it would forgive the ordinary draw path too. Red-once, dropping
`&& !escalated`: `FieldOwnershipTest.cpp:938: Failure / Actual: false / signal 6` with
`Fatal{UnmigratedPipeInput, "GetBoundVertexArray@DrawArrays"} [BARRIER-PULLED,
MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]`. Its control is
`StrictErrorsTurnsABarrierPulledReadIntoANamedAbort`, the identical pull with the flag at default.

**4 · ID-117** (`PipeInputs.cpp`). Third state exactly as sketched, both existing arms untouched;
`AdmittedBarrierPullOnce` deduped per `(field, verb)` in a static bitset — plain, not atomic, for
the file's existing reason (reachable only on the apply thread; `g_residualPulls` beside it is a
plain `Uint64`). Red-once, making the strict arm unconditional again:
`FieldOwnershipTest.cpp:867: Failure / Actual: false / signal 6`.

**5 · ID-118.** `PipeCalls.def`: `kWaitNone` → **`kWaitApplied`**. Chosen because the row posts no
reply slot (so there is nothing to wait for but the apply, and `gen_pipe.py` refuses a `kWaitReply`
row without a slot); because `copy_framebuffer_to_texture` — same `kBlitOrCopy` class — already
carries that wait; and because `glCopyImageSubData` is zero per frame in the measured scene.
`PipeCatalogueTest`'s census moves in the same commit (9 → 10 applied, offset 24 → 25). Red-once,
reverting the row: the allowlist loses `@CopyImageSubData` and `PipeCatalogueTest` goes red on three
counts. **After ID-125 the pair is admitted by disjunct 2 as well**, so the allowlist alone no
longer sees the revert — I restated the unit assertion to check the wait class itself beside the
admission. Both disjuncts hold independently, as ID-128 asked me to confirm.

**6 · ID-119 / ID-115.** *(a)* `split_log_paths.py` gains `marker_log_paths()` and a `markers` mode
driven from ctest's own map: **113 of 113** log-declaring entries read, against 90 for the old
`split-logs/` grep — the missing 26 were the `F1.` block, both `NamedBlit` pairs, the four `Ct.`
entries and `PersistentMapArm`, i.e. the readback population the allowlist is about. The CI step
resets those logs first (a stale file reads as this run's evidence) and runs the census in **both**
arms. The **two-sided ratchet** is `markers` + `Harness/strict-expected-markers.txt`: unadmitted
marker fails; `ADMITTED` outside `--print-admitted` fails; a pair not in the expected set fails;
**and an expected pair that no longer appears fails**, so the lane cannot rot green.
*(b)* The old `rsp` check grepped for a counter no entry in that directory emits, testing a sentence
true of every possible implementation (the unbarriered arm is `[[noreturn]]` and runs before
`++g_residualPulls`). Replaced by an assertion where the counter is: `DirectGLES.Split.StrictArming.`
(`MOBILEGL_PIPE_STATS=1`, `PERIOD=1`, own log path, `RESOURCE_LOCK`), asserting
`rsp == 0 || rsp >= draws` — the device evidence's shape. Measured window: `draws=1 … rsp=3 vbs=3`.
*(c)* New push-only counter `vbs` (`ServerVerbBoundaries`) at `MGPipeServerStampVerbBoundary` behind
`Enabled()`; the same entry asserts `vbs > 0`. Deliberately not `rsp`: `rsp` reaches zero when the
phase *succeeds*, so a control built on it would fail on the day the debt is paid. The entry skips
loudly in the monolith arms and in any lane that is not the counting one
(`MGITEST_STRICT_ARMING_LANE=1`); without that second guard it is discovered by `TriangleScenario.*`
in three other blocks and asserts on a summary line nobody enabled. The CI step also fails if the
arming entry did not run — a positive control that can be skipped away is not one.

**7 · Magma's lane.** *Why the split and not admitting the row:* `FieldOwnership.def` carries **one**
retiring-phase string for both backends, so any rule admitting `GetFramebufferBindingSlot@Clear` for
Magma admits it for Espryt, where it is *this phase's* debt — turning exactly the regression fb's
handle arm prevents into a warning line. The split costs two entries; the admission costs the gate.
A per-backend phase column is a `FieldOwnership.def` schema change, far outside this package.
**Finding:** `ctest -L` takes a **regex**, so `-L integration-split` matches a label spelled
`integration-split-magma`. I built the briefed spelling first and measured it —
`ctest -N -L integration-split` still listed both DirectVulkan entries, i.e. the split looked done
in every listing and had not happened. The label is `integration-magma-split`, which no existing
`-L` matches. The step asserts red **and** the named marker, so "expected red" cannot decay into
"did not run".

**8 · ID-120.** Struck through in place with a blockquote naming P7/P13 and the argument; code
untouched. **Measured, not inherited:** ID-120 says 10 pairs would change; on this head it is **23**
(`--print-admitted | grep -c '^GetBufferBindingPointCount@'`). The 10 predates ID-125's widening;
the note records the command as well as the number.

## G1 / monolith impact

`vbs` and its line are inside `#if MOBILEGL_PIPE_PUSH`; `PipeInputs.cpp` and `PipeApply.cpp` are
compiled only under `MOBILEGL_PIPE_PUSH` (the header gains declarations only, which emit no symbol);
`PipeFieldOwnership.inc` is included only from `PipeInputs.h`, which its banner keeps out of the
pull build's include closure, and nothing there odr-uses the new table; `PipeFill.cpp`'s trip wire is
`constexpr` + `static_assert`. **The one shared-code delta is `PipeCalls.def`'s wait class**, and
that column is read only on a server publishing `kCapRunAheadApply` — inert until the integration
commit. The `flavours` step could not serve as the empirical G1 check because it is already red at
my base (above), so this argument is by construction rather than by build.

G2/G14: the one new non-fork unit case has its skip twin beside the other seven; the new integration
case skips loudly in the monolith arms with its reason recorded; the `Wire/` suite is registered only
under `MOBILEGL_BUILD_DISAGGREGATED`, so its case needs none.

**Kimi audit rows:** not applicable — I migrate no field family and retired no apply-thread read of
client memory. Every row I touched is left **barriered**; that is the whole of items 3-5.

## Seams, assumptions, rulings I need

1. **The expected-marker file must be refreshed by the wave-3 integration commit.** It carries three
   `ADMITTED-ESCALATED` rows that `pa`/`mv` may retire; the ratchet will name them. That failure is
   the ratchet working, not breaking.
2. **`MG_IntegrationTest/CMakeLists.txt` merge** (per your note): my new block is at the **very end
   of the file**, after the `NamedBlit` loop, and is the only thing there; my other change in that
   file is *inside* the `NamedBlit` loop (the `LABELS` variable). `pa`'s `StrictProgramArm` and
   `mv`'s tiers are in the big disaggregated section above, so the three should not overlap.
3. **`test.yml`**: I rewrote the strict step's whole body from `ready=` down, and added one step
   before "Upload strict lane logs".
4. **RULING WANTED — "a verb with no stamp row admits nothing".** That restriction is mine, not
   ID-125's, and it is the difference between **174** and **491** pairs. The table is read only from
   `CountBarrierPull`, reachable only after `MGPipeServerStampVerbBoundary`, and the server stamps
   only `MGP_VERB_OP_LIST`'s ops — so a bit for any other verb is an allowlist row nothing can
   exercise and nobody can retire. I believe it is right; please confirm.
5. **RULING WANTED — the lane count.** ID-128 defines done as 180/180. With item 7 the Espryt lane
   cannot hold the two Magma entries, so the merged number is `180 − 2 + 1 = 179` on
   `integration-split` plus `2` on `integration-magma-split`. I did **not** adjust the admission rule
   to reach 180; wanting 180 on one label means admitting `GetFramebufferBindingSlot@Clear` globally,
   which I have argued against in item 7.
6. I assumed `pa` does not touch `EmittedCallSuppliesTheWholeField` (ID-112 forbids it); if it did,
   item 2's assertion is what says so.
7. Housekeeping: the `flavours` step left a gitignored `build-push/` in my tree. `git status` is
   clean. I ran the census to `~/w7/p5e-logs/strict-census-gl/` and `strict-census3-gl/`; the shared
   `strict-census/` is untouched.

## Is "the strict lane is hard green" now a statement a script can evaluate?

**Yes — the mechanism is complete, and every part of it is exercised on this branch.** One step now
decides all five things the phrase has to mean: (1) every entry completed (ctest rc); (2) no `Fatal{`
marker anywhere in the lane — over the log set ctest itself declares, so the 26 entries the old
check could not see are in it; (3) every `Admitted{` marker is one the tables admit, compared against
the same table the C++ answers from, so shell and runtime cannot drift; (4) no admitted pair appeared
and none silently vanished — the two-sided ratchet; (5) strict was actually armed on something that
draws — `vbs > 0` on an inproc drawing entry, and the step fails if that entry did not run.

**What is missing is not mechanism. It is two rows of debt and one flip:**

- On my head the script evaluates to **RED, correctly**, naming three rows in three lines:
  `GetProgramForDraw@DrawArrays` (52), `GetBoundVertexArray@DrawArrays` (9),
  `GetProgramForDispatch@DispatchCompute` (7) — `pa`'s and `mv`'s, both now merged upstream.
- **The post-flip arm has never executed.** It is gated on `kMGPipeP5eRunAheadReady`, still false, so
  the `no-fatal` call is code no CI run has taken. I exercised it by hand; CI has not.
- **The expected-marker set is a measurement, and it is mine, not the merged tree's.** The first
  integration run will fail the ratchet on the rows the merge retires. That is the design — but it
  means "hard green" is not evaluable by a fresh clone until that refresh lands.
- **One place I read a ruling as intent rather than text.** ID-119 says `rsp` "must equal the
  admitted-marker tally". It cannot: markers are deduped per `(field, verb)` per process and `rsp`
  counts every pull, so count-equality is not a true statement about either number. I implemented the
  *set* equality (which pairs `rsp` counts — that is what the ratchet checks) and the *shape*
  (`rsp == 0 || rsp >= draws`). If you meant literal count equality, the dedupe has to go, and an
  852-draw frame writes 852 lines.
