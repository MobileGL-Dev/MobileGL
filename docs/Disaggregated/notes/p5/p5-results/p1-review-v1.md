# p1 — adversarial review, v1

Reviewer's worktree: p1's own `~/w7/p5-p1` (read-only except one reverted perturbation, §E4 below;
`git status` clean at the end, verified). Branch `p5/p1`, `388364d5..6fc1bcc8`, base
`feat/disaggregated @ 5175f9cc`. Logs kept: `~/w7/p5-p1rev-{base,igpu,perturb}.log` and the three
scripts that produced them (`~/w7/p5-p1rev-{base,igpu,perturb}.sh`).

**Count: 0 BLOCKER / 5 MAJOR / 8 MINOR / 4 QUESTION.** Every number in `p1-v1.md` §3 reproduced.

---

## MAJOR

### M-1. One of the eleven negative controls is vacuous, and the gate that lets it be vacuous is the self-test's own design

`scripts/gen_pipe_field_ownership.py:511`, control #4 ("a BARRIER_PULLED row with no retiring
phase"), is supposed to blank `GetActiveTextureUnit`'s phase and prove that
`build()`'s `if cls == "BARRIER_PULLED" and retires.strip() in ("", "-")` stops the generator.
Its replacement string is `r"\1 \"-\","`, a **raw** string, so the backslashes survive into the
substitution and the row is rewritten as

```
X(GetActiveTextureUnit,               BARRIER_PULLED, \"-\",
```

`ROW_RE` no longer matches it, the row disappears from the table entirely, and the generator exits
with control **#1**'s message. Reproduced (the self-test with `expect_trip` instrumented to print
each control's actual exit):

```
TRIP  a field in NO class (GetActiveTextureUnit's row removed)   :: ... 1 field(s) in NO class ... GetActiveTextureUnit
TRIP  a BARRIER_PULLED row with no retiring phase                :: ... 1 field(s) in NO class ... GetActiveTextureUnit
```

Control #4 is a duplicate of control #1 under a different name. **The "every BARRIER-PULLED row
names the phase that retires it" guard — the one thing that makes `rsp` sizable per phase — has no
negative control at all**, and the report's headline "11 negative controls, all trip" is 10.

The enabler is `expect_trip` (`:448-455`): it catches any `SystemExit` and asks nothing about which
one. `gen_pipe_dirty_surface.py`, the file p1's report says it copied the discipline from, asserts
the *problem string* of each control (`:1574` `p.startswith("UNMAPPED")`, `:1581` `"STALE row"`,
`:1587` `"BAD answer"`, …). p1 dropped that half.

**Fix:** make `expect_trip` take the substring the control must produce and fail when the message is
someone else's; then fix #4's replacement (`"\\1 \"-\","` or a non-raw literal). Re-run — #4 will
then genuinely be red for its own reason, and any other control that has silently drifted will
surface with it.

### M-2. The stamp map cannot detect an *omitted* row, and §8.5 of the report says it can

`check_verb_ops` (`scripts/gen_pipe_field_ownership.py:205-222`) validates that each row's op is in
`PipeCalls.def` and each row's verb is in `FillPoints.def`, and that no op has two rows. It never
asks the other question: *is there an op that is a verb boundary and has no row?* Three wire ops
share an **exact name** with a verb in `FillPoints.def` and are not in `MGP_VERB_OP_LIST`:

```
ops whose NAME is also a verb name: ['GenerateMipmap', 'GetTextureImage', 'Clear', 'ReadPixels', 'MemoryBarrier']
of those, unmapped: ['GenerateMipmap', 'GetTextureImage', 'MemoryBarrier']
```

(`ResourceCopyRegion` → `CopyImageSubData`/`CopyTexSubImage2D` and `LaunchGrid` → `DispatchCompute`
are the same shape with a renamed op.) Today this is **latent**: CONTRACT §7 puts exactly five slots
in class B and all five are accounted for, and every one of the above is class C
(`Fatal{UnmigratedVerb}`), so no record reaches an applier without a stamp in P5.

What is *not* latent is p1-v1.md §8.5: *"the generator refuses a row whose op or verb does not
exist, so the failure mode is a red generator rather than a silently absent stamp."* That is exactly
backwards for the direction that matters. The phase that first emits `generate_mipmap` will get
**no stamp**, the previous verb's `m_currentVerb`, `FilledGen[]` and class mask will still be armed,
and its backend reads will be judged against the wrong verb — a kTextureOp field outside kDraw's
mask aborts, and a kDraw field inside it reads *fresh* while holding the previous verb's value.
That second half is silent.

**Fix:** five lines in `check_verb_ops` — for every op whose name is also a verb name (or, better,
for every op the emit table's class B carries), require a row or an explicit exemption row with a
reason, exactly as `Present` has one in prose. Add the negative control.

### M-3. R-7.1's `--check` runs in no CI job, on this branch or anywhere else in the phase

`.github/workflows/test.yml` on `p5/p1` runs `gen_pipe.py --check/--self-test` (`:1616`, `:1624`)
and `gen_pipe_dirty_surface.py --check/--self-test` (`:1664-1665`). It does not mention
`gen_pipe_field_ownership.py`. p1 flags this in §7 and assigns it to t1 — but `p5/t1 @ bb14c639`
does not have it either (`git show p5/t1:.github/workflows/test.yml | grep gen_pipe` → the same four
lines). So as the phase stands, **the generated table is committed and nothing diffs it**.

This is not cosmetic. The build-level `static_assert` only catches `kUnclassified`
(`PipeFieldOwnership.inc:256`); a hand edit that *changes* a class compiles clean — I proved that in
§E4 control C below, where a forced RECORD-SUPPLIED→FATAL move built with rc=0. The generator's
`--check` is the only thing that catches a drifted class, and it is nowhere in CI. E4 cannot be
closed until those two lines land.

### M-4. The stamp asserts freshness from the *table*, not from the records that actually arrived

`MGPipeServerStampVerbBoundary` (`PipeInputs.cpp:103-131`) stamps `FilledGen[i] = CurrentVerbSerial`
for every field that is RECORD-SUPPLIED or APPLIER-DERIVED **and** in the verb's class mask —
unconditionally, *before* the record's applier runs, and without any reference to which records this
verb actually carried. Under monolith the stamp meant "the filler published this for THIS verb"
(`PipeInputs.h:684-690`, the `MGPipeApplyAccess` comment that says the applier must not stamp).
Under split it now means "the table says a record *could* have supplied this".

Concrete failure: c1's emitter for `set_dynamic_state` drops one chunk member, or the hash
suppression that already exists (`PipeFill.cpp:1917-1923` for `set_sampler_views`) suppresses a
record whose value did change. The server reads the field, `MGPipeInputFieldIsFresh` answers **true**
because the stamp said so, the backend gets the *previous verb's* value, `rsp` stays 0 and nothing
aborts. That is precisely the class of bug the per-verb generation poison was built to catch, and
under split it is now unobservable for all 33 RECORD-SUPPLIED + APPLIER-DERIVED fields. The only
remaining oracle is E2's SSIM, which sees pixels and not state.

I am labelling this a **design finding, not a coding error** — the contract asked p1 to say "what is
stamped and for which verb" and p1 answered in those terms. But the integrator should know that E4's
`FATAL`/`rsp` half is now much stronger than the poison's original half, and that the report does not
say so anywhere. The stronger arrangement — have `MGPipeApplyAccess`'s writers stamp the field they
write, and let the verb boundary only *withdraw* — costs one line per applier write site
(`PipeApply.cpp:1336, 1364, 1373, 1377, 1436, 1512`, `MGPipeDeriveRenderStateFields`) and would make
a dropped emitter abort instead of render wrong. Worth a ruling before v1 places the call.

### M-5. `MGPipeServerClearVerbBoundary` is told to v1 as optional; under spawn nothing else clears it

Both callers of `MGPipeServerClearVerbBoundary()` are in `MG_Impl/Pipe/PipeFill.cpp` — the **client**
role (`:1639` in `MGPipeLeaveVerb`, `:2419` in `MGPipeValidateForVerb`). p1-v1.md §2 tells v1: *"the
client's own `MGPipeValidateForVerb` and `MGPipeLeaveVerb` already call it (landed here), so v1 does
**not** have to"*.

That holds only for inproc, where both roles share one process and one `gPipeInputs`. In a spawn
server (P6, and the deployment the whole phase exists for) MG_Impl is not in the process at all, so
after the very first stamp `m_serverStampedVerb` is latched **true for the life of the server**. Then:

- `PipeInputs.h:255-259`'s documented contract ("a read outside a server verb is judged exactly as it
  is in monolith") is false — every read anywhere is judged against the last verb's mask;
- `MGPipeStickyForwardPull` (`PipeInputs.cpp:167-172`) stops being a no-op outside a verb, so
  `InvalidateCompileEnv` reached from backend initialisation on a later context — which is exactly
  the case `PipeInputs.h:636-639` wrote the sticky exemption for — is counted, and under
  `MOBILEGL_IPC_STRICT_ERRORS=1` **aborts**. p1's §1.5 claims the opposite ("the hook is a no-op
  outside a server-stamped verb, which is exactly what keeps `InvalidateCompileEnv` reachable from
  backend initialisation"); that claim is true only before the first stamp.

**Fix:** the sentence handed to v1 has to become mandatory — `PipeApplier` clears on leaving the
applier — and the test that pins it (`NothingIsCountedOutsideAServerStampedVerb`) has to reach that
path rather than relying on `MGPipeValidateForVerb`.

---

## MINOR

- **m-1.** `AServerStampMakesRecordSuppliedFieldsFreshAndWithdrawsTheRest`
  (`FieldOwnershipTest.cpp:300-324`) recomputes `answerable` at `:313-315` out of
  `kMGPipeFieldOwnership[i]` — the same array it is testing. It cannot detect a wrong class, only a
  stamp that disagrees with the array. Proved: under §E4's control C (GetClearColor forced to FATAL)
  this test **Passed** while two others went red.
- **m-2.** The unpack-half abort is `Fatal{UnmigratedPipeInput, "GetPixelStoreParameters@ReadPixels"}`
  (`FieldOwnershipTest.cpp:485`) — byte-identical to what a genuinely stale *pack* read would print.
  The whole argument of deviation §6.1 is that a future unpack reader gets "a named abort on its
  first read instead of a stale struct"; the name does not distinguish the two halves.
  `MGPipeInputArgumentRead` (`PipeInputs.cpp:155-165`) has `arg0` in hand and drops it.
- **m-3.** `MGPipeVerbForWireOp(DrawVbo) → DrawArrays` is right for the class mask (all 20 draw verbs
  are `kDraw`) but wrong for the message: every `glDrawElements*`/`glMultiDraw*` under split will
  abort or strict-abort naming `@DrawArrays`. The verb the client already knows is discarded by the
  stamp at `PipeInputs.cpp:106`.
- **m-4.** `MGP_INPUT_CHECK_ARG` (`PipeInputs.h:89-95`) calls `MGPipeInputArgumentRead` *and then*
  `MGP_INPUT_CHECK`. Today the only argument row narrows to `kFatal`, so nothing double-counts; the
  day a row narrows to `BARRIER_PULLED`, `CountBarrierPull` fires in both paths for one read.
- **m-5.** `g_residualPulls` (`PipeInputs.cpp:55`) is a plain `Uint64` written on the apply thread and
  read by whoever calls `MGPipeResidualPullCount()`. Correct only because the verb barrier makes at
  most one thread runnable — the same reasoning `CONTRACT §4` writes down for `gPipeInputs` and which
  p1's own header does not repeat here. Under E1's negative control
  (`MOBILEGL_IPC_VERB_BARRIER=0`, which must go red) it is a data race on the counter *and* on
  `m_serverStampedVerb`/`FilledGen[]`.
- **m-6.** `PipeApplier::m_residualPulls` (c0's `Server/PipeApplier.h:99`) is left dead by p1's §2
  snippet, which forwards to the process-wide counter instead. Either c0's member goes or the report
  should say it is vestigial, so v1 does not wire both.
- **m-7.** Report §3.1 says the forced supplied→FATAL move makes
  `ARecordSuppliedFieldIsReadableAfterAServerStamp` red because *"the read aborts instead of
  completing"*. It does not: the `ASSERT_EQ` at `:343-344` fires first and returns. Red by name
  either way, but the mechanism described is not the mechanism.
- **m-8.** Report §1.5/§6.3 cite `generated/PipeFilled.inc:419` for the "never filled" test and
  `:422`/`:421` for the sticky arm. In this tree they are `:420` and `:421`. Trivial, but §6.3's
  whole argument rests on the ordering of those two lines and the integrator will go look.

---

## QUESTION (judgement calls, not defects)

- **q-1. `texture-remint-pull` is the one of the five emulation sites with no documented exclusion.**
  BRIEF §4's "not on the reduced path" list names compute, XFB, indirect, copy-region,
  `GetTextureImage`, `GenerateMipmap` — which covers four of the five sites. `Managers.cpp:5334` is
  reached from the image-bindable re-mint transition (`glBindImageTexture` on a texture that already
  has non-image-bindable storage), and nothing in the brief says OpenRA cannot reach it. Under
  deviation §6.4 the consequence of being wrong is an **abort in E2's inproc arm**, not a degraded
  frame. Somebody should say out loud whether that risk is accepted.
- **q-2. The transport-keyed arm is inert in a server that does not inherit the env.**
  `PipeApply.cpp:2866` reads `MG_Config::Transport`, parsed per process from `MOBILEGL_TRANSPORT`
  (`Config.h:440-452`). Under inproc that is the right process. Under spawn (P6) the five sites run
  in the **server**, and if the child is not given `MOBILEGL_TRANSPORT` its `Transport` is `Monolith`
  and the teeth are gone in exactly the process they were grown for. Build-keyed arming does not have
  that failure mode; p1's reason for rejecting it (build-split's own monolith lanes) is real, but a
  third option — key on "am I inside a server role" rather than on either — exists and is not
  discussed.
- **q-3. `Coverage.def:173-181` cuts against deviation §6.1.** p1 cites `Coverage.def:62-69`
  ("THE ROW STAYS ONE ROW") as the tree's own ruling. The paragraph about *this* field, eleven lines
  down, ends: *"**Until the field is split**, the whole of it keeps going through the fill loop and
  the pack half is simply written twice."* The tree anticipates the split the contract asked for. The
  argument-keyed form is defensible and strictly stronger at runtime (FATAL beats silently-served),
  but the report presents the precedent as one-sided when the same file says both things.
- **q-4. Records that are not verb boundaries run their appliers under the previous verb's stamp.**
  `set_framebuffer_state`, `set_sampler_views`, `resource_subdata` and friends apply between two
  stamps with `m_currentVerb` and the class mask still holding the previous verb's values. I checked
  `MGPipeApplySetFramebufferState` (`PipeApply.cpp:2230+`) and the appliers I sampled only touch
  `gPipeInputs` through `MGPipeApplyAccess` (no `MGP_INPUT_CHECK`), so nothing trips today. It is
  worth one sentence in the rule that v1 inherits, because the day an applier calls back into the
  backend it will be judged against a verb it does not belong to.

---

## What I verified and it held

Everything in `p1-v1.md` §3 reproduced on a fresh rebuild of all four directories
(`~/w7/p5-p1rev-base.log`, `~/w7/p5-p1rev-igpu.log`):

| claim | re-run result |
|---|---|
| G1 vs `~/w7/p5-before-libMobileGL.so`, `--threshold 0` | `.text 10806323 → 10806323 (+0)`; `.data/.bss/.rodata` all `+0`; `27811 → 27811 defined symbols, 0 added / 0 removed / 0 resized / 0 renamed` |
| G1 purity | `nm` pull `MG_Remote` = **0**, split = **188** |
| `ctest -L unit` | pull **1791**, push **1791**, verify **1791**, split **1862** — 0 failed in all four |
| `ctest -L integration-gpu`, `MOBILEGL_TRANSPORT=monolith` | build-push **1117/1117**, build-split **1117/1117** |
| push vs split integration-gpu names | 1117 / 1117, **0 diff lines** |
| G14 vs `p5-before-ctest-names.txt` | **0 removed**, +6 added, and the six are the six arithmetic cases by name |
| `gen_pipe_field_ownership.py --check` | up to date |
| `gen_pipe_field_ownership.py --self-test` | 11 trips, positive control OK (but see M-1 for what one of them is worth) |

**E4's three controls, actually performed** (`~/w7/p5-p1rev-perturb.log`; the tree was restored and
`git status` was empty afterwards, and the restored build re-ran 20/20 green):

- *a field in NO class* — `kMGPipeFieldOwnership[GetClearColor] = kUnclassified` in the generated
  header → `cmake --build build-split --target FieldOwnershipTest` **rc=1**,
  `PipeFieldOwnership.inc:256: error: static assertion failed … a PipeInputs field is in none of the
  four ownership classes (CONTRACT-P5 table 2, R-7.1)`. The build failure R-7.1 asks for is real.
- *supplied → FATAL in the hand half* — adding `X(GetClearColor, FATAL, "-", …)` to
  `MGP_FIELD_OWNERSHIP_LIST` → `gen_pipe_field_ownership: GetClearColor is in TWO classes - the
  derivation says RECORD_SUPPLIED and FieldOwnership.def says FATAL`.
- *the same move forced through the generated header* — builds clean (see M-3), and **two named tests
  go red**: `FieldOwnershipTest.TheClassSizesPartitionTheFieldSet` and
  `FieldOwnershipTest.ARecordSuppliedFieldIsReadableAfterAServerStamp`. E4's "a named test must go
  red" is satisfied.

Checked by reading, and correct:

- **F-1 is right and the contract is wrong about the count.** All six `MGB_CTX->GetPixelStoreParameters`
  sites in the tree pass `false`: `DirectGLES.cpp:7924`, `:9399`, `:10893`, `:11272`,
  `Utils.cpp:2302`, `VulkanRenderer.cpp:10980`. There is no seventh, and no backend site passes
  `true`. Making unpack FATAL therefore cannot fire on any existing lane — and in fact cannot fire at
  all outside a server stamp, because `MGPipeInputArgumentRead` returns immediately when
  `!serverStamped` (`PipeInputs.cpp:156`).
- **F-2 is right.** `kMGPipeEmittedFieldCount = 40`; `EmittedCallSuppliesTheWholeField` refuses 9; only
  **8** of those 9 are in the emitted list (`GetPixelStoreParameters` is `kNone` in
  `kMGPipeFieldEmittedBy` and was never among the 40). 40 − 8 = **32**. The contract's 31 is wrong.
- **The `Present` claim is right**, and I checked the source rather than the report: `FillPoints.def:21`
  says verbatim that Present and SetSwapInterval "go through BackendObject virtuals and read no
  frontend state, so they are not verbs here"; there is no `MGPipeVerb::Present` in
  `MGP_FILL_VERB_LIST`; and CONTRACT §7's class B is exactly the five slots, of which the other four
  all have stamp rows.
- **§4's debt table reproduces exactly.** Recomputing against the *generated* `kMGPipeClassFieldMask`
  (which OR's the seven sticky into every class — `PipeFillPoints.inc:279`): kClear 25/14, kDraw
  54/26, kReadback 24/18, kBlitOrCopy 29/18, kTextureOp 17/16, kProgramOp 18/14, and
  kClear ∪ kDraw ∪ kReadback = **27** distinct BARRIER-PULLED fields (20 non-sticky + 7 sticky).
- **The sticky-exemption mechanism works as described, in both directions.**
  `PipeFilled.inc:420` tests `gen == 0` before `:421` tests sticky, so the stamp's zeroing wins; and
  the client's fill re-arms them (`PipeFill.cpp:2615`, `if (filled.FilledGen[i] == 0) … = 1`), so the
  withdrawal is per-verb rather than permanent.
- **`InvalidateCompileEnv` from backend initialisation is not re-poisoned** — for P5. The seven still
  carry no `MGP_INPUT_CHECK`, and `MGPipeStickyForwardPull` returns before doing anything when
  `!ServerStampedVerb()`. (The P6 caveat is M-5.)
- **`rsp` is wired where it will be reached.** `ResidualPulls` is inside `#if MOBILEGL_PIPE_PUSH`
  (`PipeStats.h:116`…`:182`), so G1 is safe; and `MOBILEGL_PIPE_POISON` is armed by
  `MOBILEGL_BUILD_DISAGGREGATED` (`PipeInputs.h:21-26`), so `MGP_INPUT_CHECK` — the only thing that
  can call `CountBarrierPull` for the 56 checked accessors — is compiled into **every** split build,
  including Release/INFO. Counting happens at exactly one place (`PipeInputs.cpp:71`).
- **No accessor bypasses the check.** The seven direct `MGPipeInputPoisonFatalForVerb` calls in
  `PipeInputs.h` (`:536, :564, :575, :587, :595, :603, :626`) are all *after* `MGP_INPUT_CHECK`, on a
  null/out-of-range value, not alternatives to it. So no BARRIER-PULLED field can abort where it
  should have been counted.
- **Shared-file risk is still latent.** `p5/b1` is still at the base commit and `p5/w1 @ c1b5c2a0`
  touches none of `PipeApply.cpp`, `PipeStats.{h,cpp}`, `MG_Test/Pipe/CMakeLists.txt`,
  `PipeInputs.h`, `PipeFill.cpp`. p1's §7 conflict forecast stands; nothing has collided yet.

---

## Verdict

**Let it in, with M-1, M-2 and M-3 as conditions and M-5 as a correction to the text v1 is working
from.** The runtime code is sound — I could not make the stamp, the four-way verdict, `rsp`, the
strict lane or the pixel-store narrowing misbehave, and every acceptance number is real. What is
weak is the *instrumentation of the instrumentation*: one negative control proves nothing, the
generator cannot see an omitted stamp row, and the `--check` that R-7.1 makes the whole mechanism
rest on is in no CI job in the entire phase. M-4 is a ruling the integrator should make before v1
places the call, not a reason to hold the branch.
