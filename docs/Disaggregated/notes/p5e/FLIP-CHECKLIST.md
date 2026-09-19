# P5e wave 3 — integration and the flip, as an executable checklist

Base `7c6f6886`. Integration tree `~/w7/p5e-int` (branch `p5e-wave2-landed`, fetched to the Windows
worktree `MobileGL-disagg` via remote `wslpipe`, pushed to `origin/feat/disaggregated`).
`G` below = `bash /home/swung/w7/notes/tools/p5e_gate.sh int <step>`.

Rulings this implements: ID-111 … ID-123.

## Phase A — land the three packages

| package | tree | scope | brief |
|---|---|---|---|
| `pa` | `~/w7/p5e-pa` | the program family, 69 of 109 red entries, the per-draw pull | `TASK-pa.md` |
| `mv` | `~/w7/p5e-mv` | `MultiDraw.cpp`'s bound-VAO read, 9 entries (ID-113) | `TASK-mv.md` |
| `gl` | `~/w7/p5e-gl` | the gate mechanism + two latent crashes (ID-111/112/115/116/117/118/119/120) | `TASK-gl.md` |

Merge order: `gl` first (it changes the strict knob and the generator, so the other two want to be
measured under it), then `pa`, then `mv`. Expect conflicts only where `pa` and `gl` both touch
`PipeInputs.*`; `gl` was told to stay out of `DirectGLES.cpp` and `MultiDraw.cpp`.

After each merge: `G build`, `G unit`, `G isplit`, `G gpu`, `G gens`. **`gpu` is not optional** —
it is the push-monolith runtime arm and the only thing that would catch an ID-81 violation
(ID-107/109; the last one cost 137 SEGFAULTs across 38 scenarios).

## Phase B — make the lane's claim evaluable, then measure it

1. **Re-take the census on the corrected harness.** The current census re-runs each entry under a
   FIXED environment, which is wrong for 14 of 109 (the `PersistentMapArm.` and `SmallRing.` and
   `NamedBlit.` prefixes carry extra `ENVIRONMENT`). Drive the environment from
   `ctest -L integration-split -N -V` per entry. Until that is done, any marker table is provisional.
2. **The census is a first-fault histogram, not an inventory.** The 62 entries that abort on
   `GetProgramForDraw@DrawArrays` will not go green when that row lands; they will advance to their
   next pull. Budget a second census immediately after `pa` merges, before claiming any distance to
   hard green.
3. Baselines to record BEFORE the flip so the deltas are attributable (all measured green at
   `7c6f6886` by the controls scout, re-confirm after the merges):
   - `MOBILEGL_IPC_RUN_AHEAD=0 ctest --test-dir build-split -L integration-split -j 4` → 116/116
   - `MOBILEGL_IPC_AUDIT=1 ctest --test-dir build-split -L integration-split -j 4` → 116/116
   - `MOBILEGL_IPC_STRICT_ERRORS=1 ctest -L unit` → 2250/2250
   - the default arm's split logs carrying "run-ahead requested … running lockstep"

## Phase C — the things BRIEF §3 asks for that do not exist or are broken

| BRIEF item | real state | action |
|---|---|---|
| §3.4(a) `RUN_AHEAD=0` identical picture | control exists and is green; SSIM harness does not | host-side pixel equality on the lane's readback scenarios is cheaper and stronger than SSIM; write that |
| §3.4(b) `VERB_BARRIER=0` red at a barriered row | `scripts/ci/split_negative_controls.sh` **fails its own check** and demands a `Fatal{BarrierViolation}` that cannot fire at `BATCH_WAITS=1` | ID-122: fix, re-specify, or delete with a named reason. Do not leave it |
| §3.4(c) Magma logs once and runs lockstep | already true and already in every split entry's private log | assert it, no new fixture |
| §3.5 per-package red-once | **not executable** — every candidate is red before the revert (ID-121) | re-sequence after the lane is green; evidence form becomes "the table gains exactly this one pair" |
| §3.6 G1 | the gate configures only `build-split`; no pull flavour exists (ID-123) | integrator runs one `-DMOBILEGL_PIPE_PUSH=OFF` configure+build; packages must write "not verified locally", never "G1 green" |
| §3.7 `MOBILEGL_IPC_AUDIT=1` clean | measured 116/116 today | re-run post-flip, when a present-ahead client is the new consumer of retired staging |

## Phase D — the flip, in one commit

Flip together, never separately:

- `kMGPipeP5eRunAheadReady` — `MG_Backend/Init.cpp:98`
- `kMGPipeP5eClientWaitRuleLanded` — `MG_Pipe/PipeApply.h:969`

and in the SAME commit, ID-111's one-line fix to the barriered stamp
(`MG_Remote/Server/PipeApplier.cpp:1180-1182`), if `gl` has not already landed it. Without it the
Vulkan backend stamps every draw unbarriered while its client is still lockstep and dies on the first
draw.

## Phase E — the device matrix

Four arms, interleaved, 30 s windows, GPU pinned per arm (the pin is lost across app restarts), fan
on, and the core each thread landed on recorded:

| arm | what it answers |
|---|---|
| monolith | the parity target |
| inproc, `MOBILEGL_IPC_RUN_AHEAD=0` | the paired A/B control: identical server code, identical records, the only difference is whether the client waits |
| inproc, run-ahead, `PRESENT_CREDIT=1` | the shipping configuration |
| inproc, run-ahead, `PRESENT_CREDIT=2` | ID-92's measurement arm |

**Do not use `MOBILEGL_IPC_VERB_BARRIER=0` as the control** (ID-114): it is the first conjunct of
`RunAheadArmed()`, so it turns run-ahead off as a side effect, and it aborts within seconds with
`Fatal{UnmigratedPipeInput, "IsCapabilityEnabled@ClientWaitSync"}`.

Acceptance, in order of what actually decides it:

1. `cli` per frame falls from ~916 to the handful of records that stay barriered by ID-84.
2. `rsp` stops scaling with `draws` (it was exactly the draw count under inproc, 0 under monolith).
3. client thread CPU ms/frame falls from 6.1 toward the projected ~3.0 (`PERF-PROFILE-7c6f6886.md` §5).
4. fps, last, and only compared within one interleaved session.

## Phase F — docs

`CURRENT_STAGE_PROGRESS.md` §2.5, `MEASUREMENTS.md` §10, `ROADMAP.md`'s P5e row, and a P5e report.
The perf story belongs in the report, not scattered: `PERF-BASELINE-7c6f6886.md` and
`PERF-PROFILE-7c6f6886.md` are its raw material.
