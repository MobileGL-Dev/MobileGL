# TASK-pa — the program family leaves the frontend on the draw and dispatch paths

Read `PACKAGE-PREAMBLE-P5E.md` first. Your tree is `~/w7/p5e-pa`, branch `p5e/pa`, base `7c6f6886`.
Your slug for the gate driver is `pa`.

Your package is the largest single item left in P5e: **68 of the 109 red entries in the strict
lane**, and the one that fires once per draw on the device. Until it lands, no draw can be
unbarriered, so the client keeps waiting for the apply thread at every draw and the split arm runs
at roughly two thirds of the monolith frame rate.

## 1 What is measured, so you do not have to re-derive it

Per-entry marker census at your base (`~/w7/p5e-logs/strict-census/census2.tsv`):

| marker | entries |
|---|---|
| `Fatal{UnmigratedPipeInput, "GetProgramForDraw@DrawArrays"}` | 61 |
| `Fatal{UnmigratedPipeInput, "GetProgramForDispatch@DispatchCompute"}` | 7 |

Those 68 are yours. The other markers belong to other packages or are allowlisted; do not touch them.

Device, same base, Redmi, Minecraft 26.3-rc-3, ~849 draws/frame, GPU pinned, 30 s window,
interleaved so the thermal drift is visible in both arms:

| arm | fps p50 | client GL thread ms/frame | apply thread ms/frame |
|---|---|---|---|
| monolith | 267.9 / 195.1 | 3.898 / 4.781 | — |
| inproc | 166.4 / 143.5 | 6.098 / 6.958 | 5.305 / 5.653 |

## 2 The finding that makes this package small: the wire already carries everything

`MGPipeShaderCsoRecord` (`MobileGL/MG_Pipe/PipeApply.h:391`) already holds

- `SharedPtr<const MG_State::GLState::ProgramArchive> Archive` — filled by package pg, once per
  link, under a transport only, null under monolith BY DESIGN (ruling ID-81). A
  `ProgramArchive` (`MG_State/GLState/ProgramState/ProgramArtifactsCodec.h:100`) holds
  `LinkArtifacts Link`, `SpirvArtifacts Spirv` and `LinkedStages`.
- the three post-link mutable tails `BlockBindings`, `SamplerUnits`, `StorageOverrides`, with
  `Signature` and `BindingsSerial`.

And a program-pipeline **composite gets its own record** — the archive comment says so explicitly:
"a program-pipeline composite and its stage programs are three records over one link's artefacts".

Now the key identity: every frontend accessor the two remaining readers call is a **pure function of
`LinkArtifacts`**. `ProgramObject::Artifacts()` (`ProgramState/ProgramObject.h:1342`) returns
`const LinkArtifacts&`, and each of `GetMaxUniformLocation` (`:161`), `GetUniformType` (`:303`),
`GetActiveAttributeLocationMask` (`:662`), `GetAttribType` (`:712`) and
`GetUniformSamplerOrImageUnitIndex` (`:857`) is a thin wrapper over it.

**So this package adds nothing to the wire, changes no record, and regenerates no catalogue.** It
redirects readers from `program->Artifacts()` to `record->Archive->Link`. If you find yourself
designing a new wire row, stop and report instead — you have probably found something this brief
missed, and the integrator wants to hear it before you build it.

## 3 The sites

All in `MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp` unless said otherwise.

### 3.1 The two unconditional accessor calls — these ARE the marker

| site | code | note |
|---|---|---|
| `:6006` in `PrepareForDraw` | `const auto& currentProgram = MGB_CTX->GetProgramForDraw();` | no arm test at all |
| `:6746` in `BindCurrentTextures()` (the no-argument overload) | `BindCurrentTextures(TextureImpl::CaptureDrawTextureSyncKeys(), MGB_CTX->GetProgramForDraw());` | same |

Merely CALLING the accessor trips the check — `MG_Backend/MGPipe/PipeInputs.h:659` runs
`MGP_INPUT_CHECK(MGPipeInputField::GetProgramForDraw)` before returning. So arm-selecting the call
is the last step, not the first: it is only legal once every consumer can be served without it.

`PrepareForCompute`'s `GetProgramForDispatch` is the same shape; find it and treat it identically.

### 3.2 The consumers that still genuinely need the object

**(a) `ResolveAndBindUnitTextures` (`:6090`, reached from `BindCurrentTextures` at `:6719`).**
Reads `GetMaxUniformLocation`, `GetUniformSamplerOrImageUnitIndex`, `GetUniformType` (see the
lambda at `:6175-6183`). Note that the memo KEY above it is ALREADY handle-only — tx2 landed that
under ruling ID-86 (`:6644-6663`: `drawProgram`, `shaderCsoSerial`, `bindingsSerial`,
`samplerViewsSerial`, `contextSerial`). It is only the **miss path** that still reads the frontend.
Also note `:6668` computes `programKey` from `currentProgram.get()` unconditionally; on the handle
arm that value is unused for entry selection (`:6671-6678` takes the `byHandle` branch) but it is
still a read of the pointer, so it has to go too.

**(b) `SyncCurrentVertexAttributeValues` (`:2243`, called from `:6070`).** Reads
`GetActiveAttributeLocationMask` (`:2293`) and `GetAttribType`. **The code already names this work
and says it is yours**: its comment at `:2258-2268` reads "WHAT IS NOT RETIRED HERE, and it is
S4/pg's not vi's: the two program reads below (GetActiveAttributeLocationMask, GetAttribType). They
are frontend object rows of the PROGRAM family and stay until pg carries the attribute mask and
types in the ShaderCso record." The archive carries them; you are the package that reads them from
there.

**(c) `BindCurrentProgramWithResources` (`:6756`)** already takes `handleArm` at `:6763` and selects
at `:6774`, but still receives the object as a parameter. Check every use of that parameter under
the handle arm and make it null-safe in the sense of §5, not merely unused.

Sites `:7374` and `:7446` are already behind `if (ProgramHandleArm())` — pg did those. Read them:
they are the model for the shape the integrator expects, including `FindShaderCsoRecord` and
`ResolveProgramTwin`.

## 4 The design you are asked to land

A single arm-selected accessor for the current draw (or dispatch) program's link artefacts, e.g.

- `const MG_State::GLState::LinkArtifacts* CurrentDrawLinkArtifacts()` that, under
  `ProgramHandleArm()` (`Managers.h:714`, `Transport != Monolith && ProgramSubsystemEnabled()`),
  resolves `MGPipeApplier().DrawProgram` to its `MGPipeShaderCsoRecord` and returns
  `&record->Archive->Link`, and under the monolith arm returns `&program->Artifacts()`.

Then both readers in §3.2 take that, and the two calls in §3.1 become arm-selected. Whether the
helper returns a pointer, a small view struct, or is spelled as two overloads is yours to choose;
argue it in your report. Requirements on whatever you choose:

1. **The arm is decided by `MG_Config::Transport`, never inferred from a pointer being null.**
   This is ruling ID-81 and the phase has already paid for breaking it once: fix1 (`66621767`) and
   rulings ID-107 / ID-109 / ID-110 exist because a seam read "a handle was noted" as "the record
   arm is selected". Those are different statements. Read `CURRENT_STAGE_PROGRESS.md` §2.5 before
   you write the arm selection.
2. **A null archive on the handle arm is a named refusal, not a fall-back to the frontend.** Copy
   the shape of `RefuseNullFrontendTextureOffTheHandleArm` (`Managers.cpp`, search for it) and of
   `MGB_TEXTURE_RECORD_ARM_SELECTED()`. Quietly reaching back into the frontend would hide a
   missing record behind a picture that still looks right, which is what the subsystem A/B exists
   to expose.
3. **The pull build must not gain a call.** Guard so the preprocessed text of the pull flavour is
   unchanged (the `#else` arms of the existing macros are the precedent). G1 is a gate.
4. **The monolith arm keeps its bytes.** The push-monolith build is what the phone ships; it keeps
   the frontend arms token for token.

## 5 What must still work

- **Program pipelines / separate shader objects.** `GLContext::GetProgramForDraw`
  (`MG_State/GLState/Core.cpp:609`) returns either the plainly bound program or a cached COMPOSITE
  flattened from the bound pipeline's stage programs. Confirm from the emitter that the composite
  is the thing `set_draw_program` names and that pg archives it, then prove it: the lane has
  `ProgramPipelineScenario` and `ImageLoadStoreSsoScenario`. If the composite turns out NOT to have
  a record, stop and report — that is a design gap, not something to paper over.
- **Relink.** `create_shader_state` is re-issued on the same handle at every link that moves
  `GetLinkVersion`, including a FAILED relink (`LinkStatus` distinguishes them). The archive is
  replaced with it. Make sure a draw after a relink reads the new archive, and that a draw on an
  unlinked program declines exactly as it does today.
- **The dispatch twin**, with `AtomicCounterScenario` and `UnboundImageDescriptorScenario` as its
  lane evidence (those are 7 of your entries).

## 6 Red-once (required, quoted verbatim in your report)

1. Revert your arm selection so the handle arm takes the frontend path: the strict lane must go red
   again with `GetProgramForDraw@DrawArrays`, and the entry count must return to 61.
2. Make the archive lookup return null on the handle arm: your named refusal must fire, quoted from
   the entry's own log (`MOBILEGL_LOG_FILE_PATH`, because `MGLOG_F` does not reach ctest's captured
   stdout — see `strict_census2.sh` for the working recipe).
3. Put both back and show green.

## 7 Gate before you report

`build` rc=0 · `unit` 2250/2250 · `isplit` 116/116 · **`gpu` 1294/1294** (this step is new, ID-109:
it is the monolith runtime arm, and it is how the phase catches an ID-81 violation) · `gens` all
rc=0 · `strict`: report the FULL per-entry marker census using `strict_census2.sh`, not just the
pass count. Your package succeeds when `GetProgramForDraw@DrawArrays` and
`GetProgramForDispatch@DispatchCompute` are gone from that table and nothing new has appeared.

## 8 Report

`~/w7/notes/p5e/reports/pa-v1.md`. Include: the read-site inventory you actually found (if it
differs from §3, say so — this brief was written from a reading, not from a full audit), the design
and why, the composite/relink evidence, the three red-onces verbatim, the before/after marker
census, the gate table, and anything you were asked to do that you did not do, with the reason.
