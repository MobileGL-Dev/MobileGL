# ra2-v1 — make run-ahead actually correct

Tree `~/w7/p5e-ra2`, branch `p5e/flip`, base `985ae62d`, head **`efe58eac`** (three commits).

## 1 Verdict (TASK-ra2 §3.2)

**Fixable inside P5e, and fixed. The P11 deferral was not needed for this defect.** With the flip
still thrown the lane that must be green is green — `integration-split` **179/179**, from 112/181
on the base — and the two lanes that must be red are red for their named reasons. Every one of the
four fields / eleven verbs / 70 markers ID-132 measured is gone, and no wait was added to the
shipping path to get there.

### 1.1 The most reusable thing in this package

**A wait added to satisfy a lane is a real cost on a real path. The check on any escalation is
"which arm pays for it", not "which lane goes green".**

ID-133 escalated a plain multi-draw so that a pull in Espryt's indirect multi-draw tier became
legal. It turned 18 entries green and it charged **every `glMultiDraw*` on the `auto` → `ext`
default tier** with a per-batch rendezvous — because there is one draw opcode and the tier is
chosen server-side per batch, so the narrowest predicate both roles could compute could not name
the arm that actually needed it. The lane could not see that; only asking who pays could. ID-136
then retired the pull instead (§4.1) and the escalation went away with it. Both halves landed in
one commit, so neither intermediate state was ever a head.

That question generalises past this phase: every escalation is a rendezvous, every rendezvous is
paid by some arm, and the lane that motivated it is usually not the arm that pays.

**ID-132 was right about the object and wrong about the mechanism, and that distinction is the
most useful thing in this package.** The ruling suspected `gPipeInputs`' *field values* racing.
They do not: the values are already partitioned — the client's fill writes only fields no record
supplies (`SuppliedFieldMask`), the applier writes only record-supplied ones. What races is the
**metadata**: the four shared scalars `CurrentVerbSerial`, `FilledGen[63]`, `m_currentVerb` and
`m_serverStampedVerb`, which both roles write and which no partition covers. That is why a
P5e-sized fix existed at all, and it is the sentence P11's scope should be written in (§5).

Three things landed on top of the diagnosis: the fix (§3), the indirect-binding pull's retirement
(§4.1, ID-133 escalated then ID-136 retired instead) and ID-134's lane (§4.2). Two of ID-133's
premises did not survive measurement; they are corrected in §4.1 and they are what ID-136 acted
on.

Commits: `2d03565f` (the fix), `f2865480` (ID-133 / ID-134 / the contract), `efe58eac` (ID-136).

## 2 The diagnosis, with evidence

### 2.1 What the marker's verb proved before any instrumentation

Of ID-132's 70 red entries, 54 abort with the **plain** poison (no `[BARRIER-PULLED]` bracket).
For a `kRecordSupplied` / `kApplierDerived` field that branch of `MGPipeInputUnfreshRead`
(`MG_Backend/MGPipe/PipeInputs.cpp:311-345`) is reachable **only when `serverStamped == false`** —
with the stamp up those two ownerships take the `Fatal{RoleViolation}` arm instead, and
`GetTextureContextId`'s accessor (`PipeInputs.h:524`) short-circuits to
`MGPipeApplierContextSerial()` before the check can fire. All 54 are on those two ownerships, so
all 54 read the block with the **server's own stamp withdrawn**.

And the verb in the marker is the *client's*. The applier can only ever write a verb that has a
row in `MGP_VERB_OP_LIST` (`FieldOwnership.def:256-276`). `DrawElements`,
`DrawElementsBaseVertex`, `DrawElementsIndirect`, `DrawElementsInstancedBaseVertex`,
`MultiDrawElementsBaseVertex`, `GetIntegeri_v` and `GetSyncStatus` have no row — and 40 of the 54
name one of those. Nothing but the GL thread can have put them there.

### 2.2 The interleaving, verbatim

Two temporary probes (fill site and `MGPipeInputUnfreshRead`; both removed before the commit), one
run of `DirectGLES.Split.MultiDrawTierDrawElements.MultiDrawScenario.PlainBatchMatchesUnrolledDraws`,
consecutive lines of `~/w7/p5e-logs/ra2-P/…PlainBatchMatchesUnrolledDraws.01.log:580-585`:

```
[MobileGLIntegra/ERROR]: RA2-PROBE fill verb=DrawElements wireop=81 waitclass=-1 applierInside=1
[MobileGLIntegra/ERROR]: RA2-PROBE fill verb=DrawElements wireop=81 waitclass=-1 applierInside=1
[mgl-srv-apply/ERROR]:   RA2-PROBE unfresh field=IsCapabilityEnabled verb=DrawElements serverStamped=0 ownership=RECORD-SUPPLIED
[mgl-srv-apply/FATAL]:   MGPipe: Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@DrawElements"}
```

- **writer**: the GL thread, in `MGPipeValidateForVerb`'s residual fill, with `applierInside=1` —
  the apply thread is inside `PipeApplier::ApplyOne` for an earlier record.
- **the write that did it**: `MGPipeServerClearVerbBoundary()` (`PipeFill.cpp:3343`), setting
  `m_serverStampedVerb = false`; hence `serverStamped=0` at the reader. Two more in the same
  window: `++CurrentVerbSerial` (un-freshens every stamp the applier made) and `SetVerb` (renames
  the verb the abort prints).
- **reader**: the apply thread, mid-`DecodeAndApply` of an unbarriered `draw_vbo`, reading
  `IsCapabilityEnabled`.
- **why four fields over eleven verbs, flakily**: the field is whichever the applier touches next
  after the window opens.

### 2.3 Why the existing guard never fired

`RefusePipeInputsTouchWhileApplierOwnsIt`'s run-ahead arm was `if (isBarrieredFill) return;`.
`isBarrieredFill` is the caller's own sentence *"this touch is the residual fill of a record this
thread is about to park behind"* — a claim about the **future**. The order at the validate point
is fill → emit → park, so at the instant of the write the applier is still draining the backlog.
The exemption was unconditional, so the guard was unfireable on exactly the class it was written
for. That is the answer to the brief's question.

### 2.4 A second, independent defect underneath it

`ClientVerbIsBarriered` inverts `MGP_VERB_OP_LIST`, which joins the **whole draw family through
one row** (`DrawVbo -> DrawArrays`). So the inverse answers `kOpCount` for nineteen draw verbs,
and the file's default — *"an op with no verb row answers BARRIERED: fill it, wait for it"* —
filled for each. **The fill happens and the wait does not**: the wait is decided per *record* by
`MGPipeBarriered(op, …)` at the publish, and the record is a `draw_vbo` (`kWaitNone`).
`wireop=81 waitclass=-1` in the probe above is that, measured.

## 3 The fix (commit `2d03565f`)

| file | hunks | what |
|---|---|---|
| `MobileGL/MG_Impl/Pipe/PipeFill.cpp` | `397-429`, `439-472`, `706-713`, `3392-3402`, `3718-3731` | the kDraw fallback in `ClientVerbIsBarriered` (+ a `static_assert` tying it to the generated row); new `QuiesceApplierBeforeFill`; its three call sites — validate-point phase 1, step 4, `MGPipeNoteFrontendMutation`. |
| `MobileGL/MG_Remote/Client/ClientSession.cpp` | `1319-1352` | the run-ahead arm becomes `isBarrieredFill && !ApplyThreadIsInsideApplier()`; the Fatal names which shape fired. |
| `MobileGL/MG_Remote/Client/ClientSession.h` | `211-222` | the block's rationale updated, not deleted. |
| `MobileGL/MG_Test/Wire/RemoteClientTest.cpp` | `2300-2303`, `2314-2376` | the two regression cases and a note on the existing green control. |

**Two waits per filling verb, and that is the shape of the block, not belt-and-braces.** The
validate point writes `gPipeInputs` in two phases that straddle publication: the serial bump,
`MGPipeServerClearVerbBoundary()` and `SetVerb` **before** the fourteen emitters, and step 4's
63-field walk **after** them — and the records those emitters published are records whose apply
reads the block. One wait cannot cover both halves.

Everything added reduces to `MG_Config::Transport != Monolith` through `WaitForApplyToCatchUp`'s
own first test (`m_runAheadArmed && m_started`), stated rather than inferred (ID-81). Under
lockstep, monolith and `MOBILEGL_IPC_RUN_AHEAD=0` every added line is a call that returns; the
refusal is a named `Fatal{RoleViolation, "gPipeInputs"}`, never a fall-back (ID-110).

### 3.1 Controls and red-onces

**A/B control (TASK-ra2 §2).** Same binary, same records, entry's own ctest `ENVIRONMENT`, knob
layered on last (`.p5e/one_entry.py`), on base `985ae62d`: default arm **2/6 runs red**
(`Fatal{UnmigratedPipeInput, "GetRenderStateParametersVersion@DrawElements"}`);
`MOBILEGL_IPC_RUN_AHEAD=0` **0/8 red**. The defect is in the wait rule, not in a record's content.
(`MOBILEGL_IPC_VERB_BARRIER=0` was not used — ID-114.)

**Control C1 — which half is load-bearing.** With the forced wait in place but the kDraw fallback
reverted, `isplit` is **161/181 as well**. So the wait is the correctness fix and the fallback is
the **performance** half: without it every indexed draw takes a forced drain and run-ahead is
lockstep by another name on the draw path. Measured the other way round — fallback alone, no wait
— the lane is 133/181 (`~/w7/p5e-logs/ra2-census-E1`).

**Red-once A** (guard arm back to `if (isBarrieredFill) return;`), 1 of 48 fails:
`RemoteGuards.ClientBarrieredFillWhileTheApplierIsInsideUnderRunAheadIsFatalByName`, `Value of:
DiedOfAbort(child) Actual: false Expected: true  exited 0` — and the green control
`ClientPipeInputsTouchInsideABarrieredFillUnderRunAheadIsAllowed` keeps passing, so the pair is a
rule and not "abort unconditionally".

**Red-once B** (kDraw fallback deleted), 1 of 48 fails, `signal 6`:
`Fatal{RoleViolation, "gPipeInputs"} - the GL thread touched gPipeInputs (MGPipeValidateForVerb)
on a RUN-AHEAD session in a barriered fill that did not first wait for the applier to catch up`.

**Where the regression test lives** (TASK-ra2 §3.4): both cases are in
`MG_Test/Wire/RemoteClientTest.cpp`, in the ordinary **`unit`** lane, and `StartRunAheadSession`
arms run-ahead in the child — so they run with the flip's behaviour on **any** head, including
heads where `kMGPipeP5eRunAheadReady` is false. The strict lane runs under lockstep and cannot see
this class by construction. The probe is the guard call itself and the in-applier flag is raised
on the client thread before it (ID-102).

## 4 The two rulings (commit `f2865480`)

### 4.1 ID-133 then ID-136 — escalated, then retired instead

ID-133 landed as ruled (commit `f2865480`): `MGPipeBarriered` gained a fourth clause — a
`draw_vbo` with `NumDraws > 1` that is **not** `kDrawIsIndirect` is barriered — with the client's
verb-keyed half in `ClientVerbIsBarriered` and a row in contract §2.1. The 18 entries went green.
**ID-136 then withdrew all of it and retired the pull instead** (commit `efe58eac`). Both halves
of that withdrawal are in one commit, by ruling, so no head ever carried one without the other.
What follows is why, because the two premise corrections are the substance.

**First: I did NOT measure the default differently from mv.** `MultiDraw.cpp:391`'s `kAutoLadder`
is `{Ext, BaseVertex, MultiIndirect, Indirect, DrawElements}`, first supported wins, and
`ResolveTierForBatch` can only DEMOTE a batched tier toward BaseVertex/DrawElements — never
promote into an indirect one. My own confirmation is in the gate itself: the **un-prefixed**
`MultiDrawScenario.*` entries were green throughout, and only the `MultiDrawTierIndirect.` /
`MultiDrawTierMultiIndirect.` lanes failed. `auto` → `ext`, and the default tier never reaches the
read.

**But there is no indirect multi-draw OP to escalate.** All twenty draw entry points collapse onto
one opcode, `draw_vbo` (`MGPDrawInfo`'s own header says so; `PipeCalls.def:261` is the single
row), and the tier is resolved on the SERVER, per batch, from driver caps the client does not
hold. So the ruling's granularity does not exist on the wire, and the narrowest predicate both
roles can compute is `NumDraws > 1 && !kDrawIsIndirect`. **Consequence the ruling's cost argument
did not anticipate: this is not confined to the opt-in tier lanes.** A plain `glMultiDraw*` now
takes a wait on *every* tier including the `auto` → `ext` default. `kDrawIsIndirect` is excluded
precisely so Sodium's `glMultiDrawElementsIndirect` — which resolves its buffer from the handle in
the record's second tail and pulls nothing — keeps running ahead.

**Second, and this is the one worth acting on: the pull is not a data dependency.** Backtrace of
the aborting thread (`gdb --batch`, entry run under its own ctest command and `ENVIRONMENT`):

```
#4  MG_Pipe::(anonymous namespace)::CountBarrierPull(...)
#5  MG_Backend::DirectGLES::MultiDrawImpl::(anonymous namespace)::RunIndirect(...)
#6  MG_Backend::DirectGLES::MultiDrawImpl::DrawElementsBatch(...)
#7  MG_Remote::Server::ServerVerbSink::OnDrawVbo(...)
#10 MG_Remote::Server::PipeApplier::ApplyOne(...)
```

and the site is `MultiDraw.cpp:87`:

```cpp
Uint BoundDrawIndirectBufferId() {
    const auto& indirect = MGB_CTX->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
    ...
}
```

called once at `:584` as `const Uint previousIndirectBinding = BoundDrawIndirectBufferId();`,
immediately before the tier binds **its own scratch buffer** (`BindBufferId(GL_DRAW_INDIRECT_BUFFER,
g_indirectCommands.id)`) and restored at `:603`. **The tier does not want the client's indirect
buffer; it wants to put back whatever GL name was bound.** That is a question the server can answer
from its own binding record, and it is the same question mv already retired one function lower —
`ResolveBoundIndexBuffer` (`:167`) has a stated handle arm; `BoundDrawIndirectBufferId`, twenty
lines above it, was missed.

**ID-136: done, and the escalation withdrawn with it.** `BoundDrawIndirectBufferId` now has the
handle arm, in mv's own idiom rather than a second one beside it:

- the arm is `MG_Config::Transport != MG_Config::TransportMode::Monolith`, **stated** (ID-81) and
  never inferred from a null handle (ID-110);
- a null `MGPipeApplier().VerbIndirectBuffer` is *"this verb bound no indirect buffer"* — the same
  `0` the monolith arm returns for an empty slot, and explicitly **not** the arm test;
- a missing backend resource is a **named refusal**,
  `Fatal{RoleViolation, "multidraw-indirect-buffer-arm"}`, a sibling of mv's
  `RefuseMissingIndexBufferRecord` placed beside it, never a quiet fall-back to the frontend;
- the monolith body below is unchanged token for token, and the function moved down beside
  `BoundIndexBufferId` — the same question about the other target, now with the same two arms.

**Why a handle arm and not a migration.** The tier never reads a byte of the client's indirect
buffer. With a transport, the only other writer of this process's `GL_DRAW_INDIRECT_BUFFER` is
`DirectGLES.cpp`'s `DrawSyncBit::IndirectBuffer` arm, which binds from `VerbIndirectBuffer` and
nothing else — so *"what was bound"* **is** *"what the verb's record named"*, and restoring it is
a question the server answers about itself.

`MG_Backend/DirectGLES/MultiDraw.cpp` is **mv's file per ID-113**; I edited it under ID-136, which
overrode that boundary for this one change so the two halves could not land apart. The commit
message says so, and the boundary has not otherwise moved.

### 4.2 ID-134 — the client-array lane

`DirectGLES.Split.ClientVertexArrayScenario.*` now carry the single label
**`integration-clientarrays-split`** (`MG_IntegrationTest/CMakeLists.txt`), and
`.github/workflows/test.yml` gains an expected-red step shaped like gl's Magma one: it asserts the
**marker**, not merely a failure, and it fails both when the lane goes green with run-ahead armed
and when it goes red without `Fatal{UnmigratedVerb, "(Multi)DrawArrays+CLIENT_ARRAYS"}` in the
entries' private logs (they already had private, distinct log paths via `SplitLogPaths.cmake.in`).

**ID-131 heeded by counting, not by assuming**, on the built tree:

| `ctest -N -L …` | before | after |
|---|---|---|
| `integration-split` | 181 | **179** |
| `integration-gpu` | 1359 | **1357** |
| `integration-clientarrays-split` | — | **2** |
| `integration-magma-split` | 2 | 2 |

`integration-clientarrays-split` contains neither `integration-split` nor `integration-gpu` as a
substring, and the drop is exactly two on each.

**One deliberate deviation from "an expected-red step", and it is not hedging.** The step branches
on whether run-ahead is actually ARMED, read from the client's own log line. `kMGPipeP5eRunAheadReady`
is a **build** constant: on a head where it is false no environment can arm run-ahead, ID-82's
refusal is unreachable, and the entries are legitimately green — they are still vi's positive
control for the lockstep arm. A step that demanded red unconditionally would fail on
`feat/disaggregated` today for the one reason that is not a defect. With the flip landed for good
the green branch is dead and its comment says to delete it.

### 4.3 The contract

`CONTRACT-P5E.md` §3.5 amended to what the code does, with the reason the old formulation could
not be checked (a guard whose exemption cannot be false on the class it exists for reads as
coverage and is not); §2.1 carries escalation (iii) with the "why the record and not the tier"
argument and the withdrawal condition. **And §3.2 is marked UNLANDED** — nobody had noticed: it
says "the server's stamp is server-private … never into the shared block", which describes
applier-owned storage, and `MGPipeServerStampVerbBoundary` still writes `m_filled` /
`m_currentVerb` / `m_serverStampedVerb` into `gPipeInputs` itself. **That gap is what made the
race possible**: the contract already specified the P11 shape and the phase shipped the shared one.

## 5 What P11 still owes, and why the halves cannot be split

The fix works by re-establishing the quiescent window the lockstep barrier used to give for free.
It does **not** make the block safe to share: `gPipeInputs` is still one copy with unpartitioned
metadata, and the price of the current answer is a forced drain at every filling verb.

**Smallest sufficient form, both parts required:**

- **(a) per-role stamp state** — the applier gets its own `{CurrentVerbSerial, FilledGen[],
  m_currentVerb, m_serverStampedVerb}`, i.e. contract §3.2 actually landed, so the GL thread
  cannot withdraw the server's stamp at all;
- **(b) a generation on the ~15 `BARRIER_PULLED` values the applier still reads**, keyed to the
  record's produce-generation.

**(a) without (b) is worse than today, and this is the load-bearing paragraph.** Today a read of a
value the client has since moved aborts loudly on the withdrawn stamp. Give the applier its own
stamp and that read succeeds — against whatever the client's latest fill happens to have left in
the shared value. A loud `Fatal{UnmigratedPipeInput}` becomes a silent stale read, which is the
failure mode this campaign has twice paid for. So: both, or (b) discharged by retirement
(P7/P8/P9/P13) first and then (a) alone suffices. The values are already partitioned by
`SuppliedFieldMask`, which is why (b) is small — it is 15 rows, not the 63-field block.

## 6 Gate (head `efe58eac`, flip thrown, `.p5e/p5e_gate_frozen.sh` — a frozen copy per ID-127)

| step | base `985ae62d` | head `efe58eac` |
|---|---|---|
| `build` | rc=0 | rc=0 |
| `unit` | 2254/2254 | **2256/2256** (+2 new cases) |
| `isplit` (`-L integration-split`) | 112/181 | **179/179 — hard green** |
| `gpu` (`-L integration-gpu`) | 1290/1359 | **1357/1357 — hard green** |
| `integration-clientarrays-split` | — | **2/2 red by design**, `Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}` and `"MultiDrawArrays+CLIENT_ARRAYS"` |
| `integration-magma-split` | 2 expected-red | unchanged, not touched |
| `gens` | rc=0 | rc=0; doc citations rc=0 with 3 pre-existing problems, all in `CONTRACT-P5.md`, none introduced here |
| `flavours` | rc=0 | rc=0 — `pull` 915 TUs (push=absent), `push` 929 TUs (push=1), effective-macro assertion held |
| census (`.p5e/census_flip.py`, per-entry own command + own ENVIRONMENT) | 70 red / 17 pairs / 4 fields | **179/179 green, 0 red, zero `Fatal{UnmigratedPipeInput}`** |

The two expected-red lanes, run as themselves:

```
integration-clientarrays-split   rc=8  0% passed, 2 failed of 2
  Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}
  Fatal{UnmigratedVerb, "MultiDrawArrays+CLIENT_ARRAYS"}
  run-ahead ARMED present in both private logs
integration-magma-split          rc=8  0% passed, 2 failed of 2
  Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot@Clear"}
```

**The one `Fatal{` the census still prints is `RoleViolation{g_applier}` at `rc=0`** — it is
`Ct.CtWireScenario.TheDirectApplierResetCallOnTheGLThreadIsRoleViolation`, a death test asserting
its own abort. The census reports markers from passing entries too, which is why it appears.

### 6.1 The admitted set is ID-128's eight rows — which is how ID-136 checks out

Under `MOBILEGL_IPC_STRICT_ERRORS=1`, with `Fatal{` at zero:

| n | pair | why admitted |
|---|---|---|
| 140 | `GetFramebufferBindingSlot@ReadPixels` | ID-116 disjunct 1 |
| 140 | `GetBufferBindingSlot@ReadPixels` | ID-116 disjunct 1 |
| 11 | `GetBufferBindingPoint@DrawArrays` | ID-125 disjunct 2 |
| 11 | `GetTransformFeedbackProgram@DrawArrays` | ID-125 disjunct 2 |
| 6 | `GetTextureObject@CopyImageSubData` | ID-125 disjunct 2 / ID-118 |
| 2 | `GetProgramObject@ShaderStorageBlockBinding` | ID-125 disjunct 2 |
| 2 | `ValidateProgramName@ShaderStorageBlockBinding` | ID-125 disjunct 2 |
| 2 | `GetTextureUnitObject@CopyTexImage2D` | ID-116 disjunct 1 |

**The ninth row ID-133 introduced is gone, and its absence is the test.** Under the escalation the
census showed a ninth pair, `GetBufferBindingSlot@DrawArrays` (18), tagged:

```
Admitted{UnmigratedPipeInput, "GetBufferBindingSlot@DrawArrays"} [BARRIER-PULLED, ADMITTED,
retires in P8 (indirect), P9 (readback), P13 (transfer)]
```

`ADMITTED` rather than `ADMITTED-ESCALATED`, i.e. the **static** table answered and no admission
rule was widened (`gen_pipe_field_ownership.py --self-test` reports the same 174 derived pairs
throughout). That row existed only because the escalation had moved the pull onto the barriered
arm where the allowlist already covered it. With ID-136 the pull **does not happen at all**, so
the row disappears rather than being forgiven — which is the difference between retiring a read
and legalising it, and the reason to prefer the former.

G1: **not verified locally** (ID-123) — the gate compares no symbols. Every line added is inside
`#if MOBILEGL_BUILD_DISAGGREGATED` or in a split-only translation unit, and both non-split
flavours build clean with the effective-macro assertion held.

Kimi audit cross-check: ra2 owns no field family, so there are no `kimi-audit.md` rows to retire,
barrier or report missing. The four fields ID-132 named are untouched; what moved is who may write
the block and when, plus one wait class.

### 6.2 Red-once for both halves of ID-136

**Retiring the pull alone leaves the lane green, so the escalation is provably unnecessary** —
that is this head: escalation (iii) is gone from `MGPipeBarriered`, from `ClientVerbIsBarriered`
and from contract §2.1, and `isplit` is 179/179 with the strict census back to eight admitted rows
(§6.1). Nothing on the lane needs it.

**Reverting the handle arm puts the marker back**, `if (false && …)` on the new arm, 3/3 runs,
deterministic:

```
Fatal{UnmigratedPipeInput, "GetBufferBindingSlot@DrawArrays"} [BARRIER-PULLED, UNBARRIERED,
the client did not fill it, retires in P8 (indirect), P9 (readback), P13 (transfer)]
```

So the arm is necessary and, with the escalation withdrawn, sufficient.

## 7 What I did not do

- **The device measurement of the forced wait's cost** — the coordinator's, not mine, per their
  message. The lane runs in ~12 s either way and cannot see it. My falsifiable expectation, now
  that ID-136 has removed the multi-draw rendezvous: draws no longer fill, so the client's forced
  waits should fall to the Clear / readback / blit / XFB / query count and nothing on the steady
  draw path — including plain `glMultiDraw*`, which after ID-136 is an ordinary unbarriered draw
  again.
- **Anything in P11's shape.** §5 states what it must provide instead.
- **Any other part of mv's file.** ID-136's override covers `BoundDrawIndirectBufferId` and the
  refusal beside it; nothing else in `MultiDraw.cpp` was touched.

Artefacts: `~/w7/p5e-logs/ra2-{A,B,P,red1}` (controls, probes, red-onces),
`~/w7/p5e-logs/ra2-census-{E1,E2,E3,final,final-strict}`, scripts in `~/w7/p5e-ra2/.p5e/`.
