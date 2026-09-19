# P5e — the authoritative fact sheet for the doc update

Everything below is measured or landed. Do not restate anything that is not here; if a doc you are
editing asserts something this sheet contradicts, this sheet wins and you say so in your report.

Head: **`5c007b22`** on `feat/disaggregated`, pushed to origin. Tree `~/w7/p5e-int`.
Rulings: `~/w7/notes/p5e/INTEGRATOR-DECISIONS-P5E.md`, **ID-80 … ID-136**.
Contract: `MobileGL/MG_Remote/CONTRACT-P5E.md` (landed by c0e, amended by gl and ra2).

## 1 What the phase set out to do, and where it stands

Retire the lockstep on the Espryt draw path: the client publishes an unbarriered record and walks
away instead of waiting for the apply thread. **Run-ahead is ARMED** — both
`kMGPipeP5eRunAheadReady` (`MG_Backend/Init.cpp`) and `kMGPipeP5eClientWaitRuleLanded`
(`MG_Pipe/PipeApply.h`) are `true`. The phase is NOT closed: see §7.

## 2 The twelve packages

| wave | package | what it did |
|---|---|---|
| 1 | **c0e** | the contract and the wire: `PipeCalls.def`'s fifth column `WaitClass` + `MGPipeWaitClassFor`, `set_program_bindings` (opcode 80), `kCapRunAheadApply` (bit 10, DirectGLES arm only), `kDrawClientArrays`, subsystem bit 13, `MGPipeImageAccess`, the two knobs, the cross-package seams |
| 1 | **id** | identity: registries rekeyed on `{slot, gen}`, by-handle resolvers, the allocator guard; ID-96's answer was the empty set |
| 2 | **vi** | vertex input / VAO / index |
| 2 | **sb** | buffer binding points; `MGPShaderBuffers::WritableMask` widened (ID-104) |
| 2 | **pg** | programs: the per-link archive into SEG_STAGE, `set_program_bindings`' three tails |
| 2 | **tx2** | textures and samplers, the staged store's by-handle reads |
| 2 | **fb** | framebuffers, attachments, images, blit |
| 2 | **ra** | the transport mechanics: the wait rule, present credit, `gPipeInputs` as server-role memory |
| 2.5 | **fix1** | the monolith-arm regression the device found (ID-107); see §5 |
| 3 | **pa** | the program family leaves the frontend on the draw and dispatch paths |
| 3 | **mv** | multi-draw's frontend VAO read, plus gated entries for all five multi-draw tiers |
| 3 | **gl** | the gate mechanism itself, plus two latent crashes |
| 3 | **ra2** | the race the flip exposed, and the indirect-binding pull |

## 3 The strict lane became a hard gate

`GetProgramForDraw@DrawArrays` 62 → 0 (**no wire change**: the record already carried the per-link
`ProgramArchive`; what was missing were readers that still asked the frontend object),
`GetProgramForDispatch@DispatchCompute` 7 → 0, `GetBoundVertexArray@DrawArrays` 9 → 0.

The admission rule is **three disjuncts**, and none was designed up front — each was forced by a
real entry failing:

1. the verb's wire op is statically barriered (ID-116);
2. the field's retiring phase does not name this phase (ID-125);
3. the record was barriered by ESCALATION rather than by its class (ID-128) — XFB-active draws and
   client-array draws, both of which the contract puts outside P5e.

The allowlist is **generated** by `scripts/gen_pipe_field_ownership.py`, not hand-kept: the 13-row
copy `ra` had landed in CI was wrong in BOTH directions (it omitted `GetFramebufferBindingSlot@ReadPixels`,
21 entries, and carried `GetTextureObject@CopyImageSubData`, which was not admissible). An admitted
pull is **loud but not fatal** (ID-117): same marker grammar, tag `Admitted{` instead of `Fatal{`,
deduped per pair. The lane is a **two-sided ratchet** — a pair that appears and is not expected
fails, and an expected pair that stops appearing fails too, so the lane cannot rot green.

The ratchet earned itself on the merge: it named `GetProgramForDraw@DrawArrays` as no longer
appearing (pa retired it) and `GetBufferBindingSlot@DrawArrays` as newly appearing (mv's indirect
tiers forced a row mv had itself NAMED as latent before it could fire; it retires in P8).

## 4 Gate at the head

| gate | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2256 / 2256** |
| `integration-split` | **179 / 179** |
| `integration-gpu` | **1357 / 1357** |
| `flavours` (ID-124, new) | rc=0 — pull 915 TUs (push define absent), push 929 TUs (push=1) |
| generators / include-closure / dirty-surface / doc citations | all rc=0 (the `CONTRACT-P5.md:524` ambiguity is the standing pre-existing one) |
| per-entry strict census | **179/179 green, zero `Fatal{`**, `Admitted{` exactly the eight expected rows |
| expected-red lanes | `integration-clientarrays-split` 2 entries on `(Multi)DrawArrays+CLIENT_ARRAYS`; `integration-magma-split` 2 on `GetFramebufferBindingSlot@Clear` — both asserted by name |

Coverage grew: unit 2250 → 2256, `integration-split` 116 → 179, `integration-gpu` 1294 → 1357, and
build flavours from **one** (split) to **three** (pull, push, split). The 65 new lane entries all
cover arms or tiers that previously had **no gated entry at all**.

## 5 Three defect classes the phase found, and what each teaches

**(a) An ungated runtime arm (ID-107/109/110).** The push build has two server arms selected at
runtime; every `integration-split` entry exported `MOBILEGL_TRANSPORT=inproc`, and split-only
scenarios skip themselves off the split runtime, so the arm the phone ships on had zero gated
entries. `44f91c74` was **1157/1294 on `integration-gpu`: 137 SEGFAULTs across 38 scenarios**, while
unit and the split lane were green. Fixed by `66621767`. `gpu` is now a standing gate step.
The code-shape lesson: **"a handle was noted" is not the statement "the record arm is selected"**.

**(b) Ungated build flavours (ID-124).** The gate configured only the split flavour. Three of four
flavours were broken. And the first version of the new `flavours` step reported a clean 982-TU
"pull" build that was a **second copy of the split build** — `-DMOBILEGL_PIPE_PUSH=OFF` beside an
option that implies it is silently overridden, and the CMake cache still read OFF. The step now
asserts the **effective compile define** out of `compile_commands.json`. Lesson: **the evidence for
an arm must include proof that it was that arm.**

**(c) A guard that could never fire (ID-132/135).** Throwing the flip dropped `isplit` to 112/181
and `gpu` to 1290/1359, **flaky** — 70 red on four fields across eleven verbs. One field at one verb
consistently is a missing migration; four fields scattered and flaky is a race. Root cause:
`gPipeInputs`' **field values** are already partitioned by `SuppliedFieldMask` and do not race; the
**metadata scalars** do. It stayed invisible because
`RefusePipeInputsTouchWhileApplierOwnsIt`'s run-ahead arm exempted any `isBarrieredFill` on a claim
about the FUTURE ("the client is about to park") while the real order is fill → emit → park. The fix
makes the claim a tested fact. A second defect fell out: `MGP_VERB_OP_LIST` joins the whole draw
family through one row, so nineteen indexed-draw verbs answered "barriered", filled, and never
parked. Contract §3.2 turns out to have **specified per-role stamp storage and never been landed**;
that gap is what made the race possible.

**The methodological finding (ID-132), which is the most reusable thing in the phase:** a hard-green
strict lane is necessary and **structurally not sufficient**. That lane runs under lockstep, where
`ApplyOne` stamps every record barriered, so it cannot see anything that only happens once the
client stops waiting. It measures remaining debt, not whether run-ahead is correct. The phase exit
needs a separate post-flip lane.

## 6 Device measurements (Redmi `2f7cbe2e`, MC 26.3-rc-3, world `test`, DirectGLES)

### 6.1 The measurement correction that has to be stated first

The first matrix was **not CPU-clock-pinned** and is void: the **monolith arm's own** per-frame CPU
moved 4.43 → 7.07 ms between two runs of the same arm, and the slow run was **cooler** (53.5 °C vs
58.9). The `walt` governor had taken policy0 from 2745 to 748 MHz. `tools/device_bench/pin_device.sh`
does not know this serial and **refuses to guess** — correct, and why it did not half-apply silently.
`~/w7/notes/p5e/pin_redmi.sh` pins this device's own nodes, write order **min→hw floor, max→target,
min→target**, and **reads back `scaling_cur_freq`** to verify; checked again after every session.

### 6.2 Render distance 12 — both arms hit the panel

Clocks pinned, GPU pinned, fan on, arms interleaved, 30 s windows, two runs each:

| arm | fps p50 | fps max | client ms/frame | apply ms/frame |
|---|---|---|---|---|
| monolith | 115.4 / 115.6 | 117.5 / 117.2 | 6.25 / 6.45 | — |
| inproc run-ahead | 118.0 / 117.6 | 118.6 / 118.5 | 6.87 / 6.92 | 4.35 / 4.43 |
| inproc lockstep (`RUN_AHEAD=0`) | 111.6 | 113.7 | 8.16 | 6.53 |

Monolith and run-ahead both reach the **120 Hz panel**; lockstep is the only arm that cannot. So
this distance answers "can it reach the ceiling", not "which is faster".

### 6.3 Render distance 32 — the comparable round

`renderDistance:32` (backed up, restored afterwards). **~3550 draws/frame**, 4.2× the VD12 load, and
**nobody near a ceiling** (the game's own `enableVsync:false`, `maxFps:260`; measured max 37–71).
Settle raised to 75–150 s because the world streams far longer at this distance. Five runs per arm.

| arm | n | fps p50 median | p50 range | client ms/frame median | apply ms/frame |
|---|---|---|---|---|---|
| monolith | 5 | 58.5 | 56.3 – 67.9 (±10%) | 16.19 | — |
| **inproc run-ahead** | 5 | **59.9** | 56.8 – 61.1 (±4%) | **15.72** | 11.1 – 12.1 |
| inproc lockstep | 2 | 35.5 | 34.5 – 36.5 | 26.17 | 19.7 – 20.5 |

**Parity.** Run-ahead is marginally ahead on both medians and the difference sits well inside
monolith's own spread; what IS outside the noise is that the split arm is **half as variable**.
`mono1`'s 67.9 is the only outlier in the set (first run after install).

**Against the lockstep it replaces: p50 +69%, client CPU −40%, apply CPU −43%.** It is worth far
more under heavy load, as expected — the rendezvous count tracks the draw count, about 900 per frame
at VD12 and about 3600 at VD32.

Both arms are **client-thread saturated (~94%)**, so that column is each one's real bottleneck and
they compare directly.

### 6.4 Why run-ahead was worth doing, from the profile (taken at `7c6f6886`, pre-flip)

Symbolized `simpleperf cpu-cycles`, client GL thread: `SessionProducer::WaitForAppliedOrEventBacklog`
**31.58% SELF time**; the next symbol is 3.72%. The counters showed the client entering the doorbell
wait about **916 times per frame** against ~849 draws, 99.8% of them resolved by spinning.
`MOBILEGL_IPC_SPIN_US=0` (park every time) collapses the frame rate to **20.9**. So the cost is the
**rendezvous itself**, not the spin tuning — which is why the fix is "stop having one per draw"
rather than "make each one cheaper". After the wait, the client thread is a flat tail of 1–3%
functions: there is no second lever.

## 7 What the phase has NOT done

- **BRIEF-P5E §3 items 4 and 5** — the three negative controls and the per-package red-once re-runs.
  Item 5's evidence form was **not executable** while every candidate was red before the revert, so
  ID-121 re-sequenced it to after the lane went green. It has not been run since.
- `scripts/ci/split_negative_controls.sh`'s E1 control **fails its own check** and demands a
  `Fatal{BarrierViolation}` that cannot fire at `MOBILEGL_IPC_BATCH_WAITS`'s default of 1 (ID-122).
- **G1** is still CI's to assert; the phase gate builds the flavours but does not do symbol identity
  (ID-123).
- Contract §8 amendment 8 (`GetBufferBindingPointCount`'s sticky forward) is marked **UNLANDED**
  (ID-120); contract §3.2's per-role stamp storage is likewise unlanded and is what let (c) happen.
- **Above the panel ceiling at VD12, which arm is faster** is unanswered: this device cannot hold a
  higher clock. VD32 answers the general question instead.

## 8 The next opportunity, which is not a defect

At VD32 the apply thread sits at **11–12 ms** while the client is at **15.7 ms**. The two sides are
**unbalanced**, and moving work from client to apply would lower the bottleneck directly. This was
meaningless under lockstep, where the two costs simply added; run-ahead is what makes it a lever.
It belongs to a later phase.

## 9 Rulings worth indexing (ID-80 … ID-136; the full text is in the notes)

| ID | one line |
|---|---|
| 81 | handle arms only under `Transport != Monolith`; the push-monolith build keeps its frontend arms |
| 84 | rule F is scoped to UNBARRIERED records; barriered ones keep P5C semantics |
| 90 | Magma stays lockstep for all of P5e; `kCapRunAheadApply` is never published on DirectVulkan |
| 103 | the barriered stamp is forced `true` until the client's wait rule lands; the predicate runs anyway |
| 107/109/110 | the ungated monolith runtime arm, the `gpu` gate step, and stating the record-arm selector instead of inferring it |
| 111 | the stamp must ask the server's OWN capability, or flipping the flag kills Magma |
| 112 | a compile-time trip wire on the residual fill's seven rows |
| 116/117/125/128 | the admission rule: generated, three disjuncts, admitted pulls loud but not fatal |
| 119/115 | the `rsp` pin reworded to something checkable; the lane needs a positive control |
| 121 | §3.5's red-once evidence form re-sequenced to after the lane is green |
| 124 | three of four build flavours were broken; the arm check on the effective compile define |
| 129 | a verb with no stamp row admits nothing |
| 131 | `ctest -L` is a REGEX — a new label must not contain an existing one, and prove it by counting |
| 132/135 | strict hard green is necessary and structurally not sufficient; right about the object, wrong about the mechanism |
| 136 | a wait added to satisfy a lane is a real cost on a real path: ask **which arm pays**, not which lane goes green |
