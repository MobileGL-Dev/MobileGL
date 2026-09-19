# TASK-ra2 — make run-ahead actually correct

Read `PACKAGE-PREAMBLE-P5E.md` and `PREAMBLE-ADDENDUM-WAVE3.md` first. Your tree is `~/w7/p5e-ra2`,
branch `p5e/flip`, head `985ae62d`. Your slug for the gate driver is `ra2`.

**Your head already has the flip thrown.** Both `kMGPipeP5eRunAheadReady` (`MG_Backend/Init.cpp`)
and `kMGPipeP5eClientWaitRuleLanded` (`MG_Pipe/PipeApply.h`) are `true`. That is the whole point:
everything below reproduces on your head the moment you build it.

## 1 What is measured, and what it is not

The phase's strict lane is HARD GREEN on the integration branch (`72bc4268`): 181/181, zero
`Fatal{`, ten `Admitted{` rows all owned by later phases. That lane runs under LOCKSTEP, where
`PipeApplier::ApplyOne` stamps every record barriered, **so it cannot see anything that only happens
once the client stops waiting.** It measures remaining debt. It does not measure whether run-ahead
is correct. Your package is what measures that.

With the flip thrown, on your head:

| gate | result |
|---|---|
| `build` | rc=0 |
| `unit` | 2254/2254 |
| `gens` | all rc=0 |
| `flavours` | rc=0 |
| `isplit` | **112 / 181** |
| `gpu` | **1290 / 1359** |

The 69-70 failures are **flaky**: 71 at `-j 4`, 68 at `-j 1`, and a single entry run alone passes
and then aborts on a repeat (`ctest -R '<entry>' --repeat until-fail:12`).

A census over all 181 entries, each under its OWN command and `ENVIRONMENT` taken from
`ctest --show-only=json-v1` (`~/w7/notes/p5e/strict_census3.py`, and
`~/w7/notes/p5e/catch_flip_abort.py` to catch one entry's abort text), gives **70 red on 17 distinct
`<field>@<verb>` pairs over just FOUR fields**:

| count | field |
|---|---|
| 27 | `GetRenderStateParametersVersion` |
| 25 | `GetTextureContextId` |
| 16 | `GetBufferBindingSlot` |
| 2 | `IsCapabilityEnabled` |

across eleven verbs: `Clear`, `DrawArrays`, `DrawElements`, `DrawElementsBaseVertex`,
`DrawElementsIndirect`, `DrawElementsInstancedBaseVertex`, `EndTransformFeedback`, `GetIntegeri_v`,
`GetSyncStatus`, `MultiDrawElementsBaseVertex`, `ReadPixels`.

**Two populations, and the split matters:**

- **16** abort on the UNBARRIERED arm — the marker carries `[BARRIER-PULLED, UNBARRIERED, …]`.
- **54** abort with NO `[BARRIER-PULLED]` bracket at all. That is the plain unfresh-read abort for a
  field whose ownership is `kRecordSupplied` — the freshness stamp says it was never filled. Example
  verbatim: `MGPipe: Fatal{UnmigratedPipeInput, "GetTextureContextId@ReadPixels"}`.

The census outputs are at `~/w7/p5e-logs/census-flipped/` (per-entry logs plus `census3.json`).

## 2 The hypothesis you must TEST, not assume

One field at one verb, consistently, would be a missing migration. **Four fields scattered over
eleven verbs, flaky run to run, and including verbs that are barriered** (`ReadPixels`,
`GetSyncStatus`) is the signature of a RACE.

The obvious candidate: `gPipeInputs` is shared mutable state whose mutual exclusion WAS the
lockstep. Under run-ahead the client still fills it for BARRIERED verbs
(`MGPipeValidateForVerb`, `MG_Impl/Pipe/PipeFill.cpp:3219`, `fillOwed = barriered`) while the apply
thread may still be draining EARLIER unbarriered records and reading the same block. The client's
fill and the server's `MGPipeServerStampVerbBoundary` (`MG_Backend/MGPipe/PipeInputs.cpp:183`) then
overlap, and whichever writes `FilledGen[]` last decides whether the other's read is fresh.

This is consistent with something the project already wrote down: P5d's own research concluded that
retiring this barrier needs **`gPipeInputs` versioning or double-buffering**, and deferred it to P11
after P3b/P4b's handle-keyed twin tables (see the P11 row in `docs/Disaggregated/ROADMAP.md` and
P5d's report). If that conclusion is right, the honest outcome of your package may be "run-ahead
cannot land before that work", and saying so with evidence is a complete and acceptable result.

**Do not start from the hypothesis.** Start from the failure. Instrument, get a reproducer smaller
than a scenario if you can, and establish WHICH writer clobbers WHICH read, with evidence. If the
hypothesis is wrong, say what is actually happening.

Two facts that will help you rule things in or out:
- `RefusePipeInputsTouchWhileApplierOwnsIt` (`MG_Remote/Client/ClientSession`) already exists as the
  guard for exactly this class, and it takes an `isBarrieredFill` argument. Find out whether it
  fires, and if it does not, why not.
- `MOBILEGL_IPC_RUN_AHEAD=0` at runtime turns the client's waiting back on with identical server
  code and identical records (`MobileGL/Config.h`, the `RunAhead` comment). That is your A/B
  control: if a failing entry passes under it on your head, the defect is in the wait rule and not
  in a record's content. Run it. Do NOT use `MOBILEGL_IPC_VERB_BARRIER=0`, which is the first
  conjunct of `RunAheadArmed()` and turns run-ahead off as a side effect.

## 3 What to deliver

In order of value:

1. **The diagnosis**, with evidence. Which writer, which reader, which field, what the interleaving
   is. A verbatim reproducer.
2. **The verdict**: is this fixable inside P5e, or does it need the `gPipeInputs` versioning that
   P5d deferred to P11? Answer with the argument, not with a preference. If the answer is "needs
   P11", say exactly what P11 has to provide and what the smallest version of it would be.
3. **The fix**, if it is P5e-sized. Every arm you add follows ID-81 and ID-110: the arm is decided
   by `MG_Config::Transport` or a family selector that reduces to it, STATED not inferred, and a
   missing record is a named refusal, never a quiet fall-back.
4. **A regression test that fails without the fix.** The strict lane cannot catch this class by
   construction (it runs under lockstep), so whatever you add must run with the flip thrown. Say
   where it lives and how the gate reaches it.

## 4 Gate

`build` · `unit` · `isplit` · `gpu` · `gens` · `flavours`, all on your tree, plus the census
(`strict_census3.py`). The bar for "fixed" is **`isplit` 181/181 and `gpu` 1359/1359 with the flip
still thrown**, and the census showing no `Fatal{` that is not one of the ten admitted rows.

If you cannot reach that bar, report the exact remaining set rather than reverting the flip to make
the numbers look green — the flip is the thing under test.

## 5 Report

`~/w7/notes/p5e/reports/ra2-v1.md`. Lead with the verdict in §3.2, because that is the decision the
integrator has to make. Then the diagnosis with its evidence, then what you changed, then the gate
table, then what you did not do and why.
