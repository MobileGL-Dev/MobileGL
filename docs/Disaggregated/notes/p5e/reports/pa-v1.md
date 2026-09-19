# pa-v1 — the program family leaves the frontend on the draw and dispatch paths

Tree `~/w7/p5e-pa`, branch `p5e/pa`, base `7c6f6886`, head **`dd899510`**.
`53b83469` (MG_Backend, MG_Remote) + `dd899510` (MG_Test). 4 files, +392/−20.

## 1 Read-site inventory (what is actually there)

All five accessor sites the kimi index names are accounted for
(`GetProgramForDraw` D:4781/5308/5679/5734, `GetProgramForDispatch` D:6126):

| site (head line) | kimi | at base | now |
|---|---|---|---|
| `DirectGLES.cpp:6229` `PrepareForDraw` | 91 | unconditional pull | arm-selected (pa) |
| `:7032` `BindCurrentTextures()` no-arg, exported at `DirectGLES.h:25` | index D:5308 | unconditional pull | arm-selected (pa) |
| `:8145` `PrepareForCompute` | 92 | unconditional pull | arm-selected (pa) |
| `:7675` `GetCurrentBackendProgram` | 107 | already gated | pg, untouched |
| `:7747` `CurrentProgramMayNeedPerSubDrawBuiltins` | 108 | already gated | pg, untouched |

The consumers of the hoisted value — **this is where the brief's §3 differs from the tree**:

- **`SyncCurrentVertexAttributeValues` (kimi 106) is the only one that still genuinely needs the
  object**: `GetActiveAttributeLocationMask` + `GetAttribType`.
- **`ResolveAndBindUnitTextures`' reads are on its LEGACY arm.** `sampledTargetForUnit` is declared
  *after* tx2's `if (UnitTexturesByHandle()) { … return; }`, so the handle arm never reaches it.
  §3.2(a) reads as if the miss path still needed serving; it does not — but see §2.
- **`BindCurrentTextures`' memo key (kimi 100)** was already by-handle (ID-86); what was left is the
  unconditional `programKey = currentProgram.get()` load and three `currentProgram ? … : 0` memo
  stores. Retired.
- **`BindCurrentProgramWithResources` (kimi 101–105)** needed no code change: every read is on the
  `:` side of a `handleArm ? <record> :` pick or inside the `else` of `if (handleArm)`. The one pair
  that is *guarded* rather than picked — `globalConstants ? … : currentProgram->{GetUBOContentVersion,MapUBO}()`
  — is unreachable on that arm because the `set_global_constants` refusal above it is `[[noreturn]]`
  **in the same function**; I added that sentence to its comment rather than restructuring pg's lines.

Kimi rows: **retired** 91, 92, 106 and the residue of 100. **Left barriered** 109
(`ValidateProgramName`, P9) and the XFB row D:1387 (§5.7). **Not as the audit has it**: row 106's
`dup` column says "none" — wrong now, the archive carries both fields; that column predates pg.
No row of this family was found not to exist.

## 2 Design

`DrawProgramFromRecords()` (`:6173`) states the arm once, as a **conjunction**:
`ProgramHandleArm() && TextureImpl::UnitTexturesByHandle()`. One hoisted value serves four callees,
so the arm must name every consumer, not this family alone; the second conjunct is
`BindCurrentTextures`'. **The family bits are independently settable A/Bs** (`0x0ff` is supported),
so "the program family is on the handle arm" is *not* "no consumer needs the object" — reading the
first as the second is the shape that cost the phase 137 scenarios (ID-107/109/110). Both conjuncts
reduce to `Transport != Monolith && <family bit>`, so push-monolith is false here and keeps the
frontend hoist token for token. In the **mixed A/B** (program on, textures off) the hoist keeps
pulling and the row stays red — deliberate, smallest, and stated at the selector (ruling R-2).

`SyncCurrentVertexAttributeValues` gains a **second overload** taking `MGPipeHandle cso` (`:2380`,
under `#if MOBILEGL_PIPE_PUSH`), per §5.8's "two overloads, not an `#if` inside one body". It makes
the same three-part test `SyncCurrentProgramByHandle` makes (`record == nullptr ||
Desc.LinkStatus == 0 || Desc.SpirvStatus == 0` → return; ID-88's reason — that is the record-arm
spelling of the frontend's `if (!program) return;`), computes the mask from
`record->Archive->Link.attribs` with ProgramObject's own rule (`min(size,32)`, bit where the name is
non-empty), reads the type from `…->Link.attribTypes[location]` **bounds-checked** (the frontend
form is not, because its mask came off the same instance; here the two vectors arrived over a wire),
and logs `cso.Slot/Gen` rather than a GL name (`ProgramArchiveSource::Identity`'s rule).
**Nothing added to the wire, no record changed, no catalogue regenerated.**

A live, linked record with **no archive** on that arm is `Fatal{RoleViolation, "program-handle-arm"}`,
never a fall-back — the shape of `RefuseNullFrontendTextureOffTheHandleArm`.

Two deliberate deviations from the wave-3 scout note: I did **not** build a `ProgramArchiveSource`
per draw (its `FromRecord` allocates a vector and a map) — the hot path reads `record->Archive->Link`
directly — and I did **not** add the two accessors to `ProgramArchiveSource`, because no caller would
use them. And the overload **copies** vi's memo arm rather than sharing a tail: a shared tail would
rewrite the frontend overload's preprocessed text, which G1 measures at 0/0/0/0
(`SyncCurrentProgramByHandle` / `SyncCurrentVAOFromRecords` are the same precedent).

Also: the ledger at `:6239` now adds `(vertexInputFromRecords ? 0 : 1) + (programFromRecords ? 0 : 1)`
— **zero accessor calls per draw** on the record arm — and the stale comment in
`PipeApplier::OnLaunchGrid` (`PipeApplier.cpp:782`) saying the compute program is pulled is corrected.

## 3 Composite and relink

**Composite.** `ProgramEmit.h:126` takes `drawProgram = ctx.GetProgramForDraw()` (the flattened
composite when a pipeline is bound), `:129` acquires its CSO via `AcquireShaderCso`, which `:635`
routes through `MGPipeSlots().AllocateComposite` for a composite, and `:180`
`MGPipeRouteSetDrawProgram(HandleOnly(drawCso))` names *that* handle — so `set_draw_program` names
the composite and pg archives it like any other CSO. Server side `PipeShaderCsoRecordForHandle`
(`Managers.cpp:4483`) is band-aware. Green lane evidence: `ProgramPipelineScenario.*` (2 split
entries) and `TessellationXfbCaptureScenario.CapturesFromASeparableEvaluationProgramInAPipelineObject`.

**Relink.** The overload holds no pointer across calls — it re-resolves the record and re-reads
`Archive->Link` every draw, and `MGPipeApplyCreateShaderState` replaces `Archive` wholesale — so a
draw after a relink reads the new archive by construction. A failed relink is `Desc.LinkStatus == 0`
(ID-88) and declines before the archive is consulted.

**One behavioural delta between the arms, named not hidden:** for a program the frontend reports
*unlinked*, the monolith arm may still issue `glVertexAttrib*` current values (its guard is
`if (!program)`, not link status) while the record arm returns early. No program is bound in that
state on either arm (`SyncCurrentProgram*` binds 0), so it is unobservable.

**Gap:** the split lane has **no relink scenario**. `RelinkStageSetScenario`, `PostLinkAttachScenario`,
`HandleRecycleScenario`, `ImageLoadStoreSsoScenario` are `DirectGLES.` (monolith-arm) entries only,
covered by `gpu` 1294/1294. Named as debt, not claimed as record-arm evidence.

## 4 Red-once, verbatim

**(1) Arm reverted** (`DrawProgramFromRecords()` → `false`, vertex-attribute call site back on the
frontend overload), rebuilt, full census: lane back to **7 passed / 109 red**, marker back at
**62 + 7** — not 61 + 7, confirming the coordinator's correction in my own tree.

```
[11:48:24] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetProgramForDraw@DrawArrays"} [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]
[11:47:57] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetProgramForDispatch@DispatchCompute"} [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P5e (Espryt unbarriered), P7 (Magma)]
```

The 62nd is `…PersistentMapArm.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`,
taken under its own lane environment (`~/w7/p5e-logs/pa-census-red1/`).

**(2) Archive lookup forced to answer null** on the handle arm
(`if (true /* P5E-PA-RED-ONCE-2 */ || !record->Archive)`), entry
`TriangleScenario.AVboBackedTriangleReachesReadPixels`, log `~/w7/p5e-logs/pa-red2.log`:

```
[11:49:20] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{RoleViolation, "program-handle-arm"} - SyncCurrentVertexAttributeValues needs the active-attribute mask and the attribute types of ShaderCso {1, 0} and that record carries no server-owned archive. The arm is selected by Transport != Monolith AND the program subsystem bit (CONTRACT-P5E §5.8, ID-81), so this record's create_shader_state was applied under a transport and must have adopted one; the frontend ProgramObject is not a fall-back here, it IS the client memory this row retires
```

**(3) Both put back** → `build rc=0`, `unit 2250/2250`, `isplit 120/120`.

**(4) The new pin's own negative control (R-16):** with the arm reverted all four
`DirectGLES.Split.StrictProgramArm.` entries abort — `GetProgramForDraw@DrawArrays` in the MultiDraw
one, `GetProgramForDispatch@DispatchCompute` in the three AtomicCounter ones, each from its own
private log.

## 5 Marker census, before and after

Both tables are from **my own tree** with a corrected census (`.p5e/census_pa.sh`): the gtest filter
and the whole `ENVIRONMENT` come from `ctest -N -V` per entry instead of the last two dot components
plus one hard-coded env block — the blind spot addendum §4 names. "Before" is the red-once-1 build.

| marker | before | after | owner |
|---|---|---|---|
| `GetProgramForDraw@DrawArrays` | **62** | **0** | **pa** |
| `GetProgramForDispatch@DispatchCompute` | **7** | **0** | **pa** |
| `GetFramebufferBindingSlot@ReadPixels` | 19 | 67 | allowlist (readback, P7) |
| `GetTransformFeedbackProgram@DrawArrays` | 0 | 11 | XFB §5.7 — no P5e package retires it |
| `GetBoundVertexArray@DrawArrays` | 9 | 11 | mv |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | allowlist (P7) |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | allowlist (P9) |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | 2 | allowlist (CopyTex) |
| `GetFramebufferBindingSlot@Clear` (DirectVulkan) | 2 | 2 | Magma, ID-90 / P7 |
| **red / lane** | **109 / 116** | **101 / 116** | |

**Eight entries went fully green** — exactly my seven dispatch entries plus
`MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments`. Three rows moved that are **not mine**,
all of them *unmasking* and none of them new reads (my marker fired first at base and hid them):

- `…@ReadPixels` +48: the draw scenarios that read pixels back.
- `GetBoundVertexArray@DrawArrays` +2: both `ClientVertexArrayScenario` entries. **mv's.**
- `GetTransformFeedbackProgram@DrawArrays` +11 (7 `TessellationXfbCaptureScenario`, 4
  `XfbCaptureBufferReuseScenario`): read at `DirectGLES.cpp:1892` from
  `StartPendingTransformFeedback`, the **last** statement of `PrepareForDraw`, which is why nothing
  ever reached it. `kBarrierPulled`, retirement "P3b/P4b (Espryt), P7 (Magma)", and §5.7 calls it
  "an object row no package retires". **Missing from the addendum's allowlist table** (R-4).

`GetFramebufferBindingSlot@Clear` (DirectVulkan `NamedBlit.F1WireScenario.*`) is **identical before
and after in my census**: a census-method artefact of the shared script (those entries' real lane is
Magma; the shared script re-ran them as DirectGLES), not anything I caused. Out of scope (ID-90).

## 6 Gate

| step | result |
|---|---|
| `build` | rc=0 |
| `unit` | **2250 / 2250** |
| `isplit` | **120 / 120** (116 + four new pin entries) |
| `gpu` | **1294 / 1294** (run twice, before and after the pin commit; the pin is `integration-split`-labelled so the count is unchanged) |
| `gens` | all seven rc=0; `check_doc_citations` rc=0 with 3 pre-existing problems in `CONTRACT-P5.md`, a file I did not touch |
| `strict` | 19 passed / 101 failed of 120 (was 7 / 109 of 116); per-entry census §5 |
| `flavours` (ID-124) | **rc=1 — both halves pre-existing, proved below** |

`flavours` printed `pull ARM CHECK FAILED (compile_commands says MOBILEGL_PIPE_PUSH=1)` and
`nodisagg build rc=1 (726 TUs)` with two errors, `ComputeShaderStorageBlockBindingSignatureOf`
(`:5749`) and `keysMatch` (`:6981`). `.p5e/nodisagg_base_tu.sh` compiles the **base revision's**
`DirectGLES.cpp` with the `nodisagg` flavour's own command from its `compile_commands.json` and gets
`rc=1` with **the same two errors and no others**. Cause of the second is in the base text:
`const Bool keysMatch =` sits inside `#if MOBILEGL_BUILD_DISAGGREGATED` (base `:6695`) while
`if (!keysMatch || …)` does not. That is tx2's line, inside the block pa edits; I left it rather
than fixing another package's line under an already-red gate (R-7).

## 7 G1 / monolith impact

Every added token is inside `#if MOBILEGL_PIPE_PUSH` / `#if MOBILEGL_BUILD_DISAGGREGATED`, except
three constructs copied deliberately from **vi's already-cleared shape**: the
`inline Bool DrawProgramFromRecords()` selector with the `#if` inside it (vi's
`VertexInputReadsRecords`), the `const SharedPtr<ProgramObject> noFrontendProgram;` local (vi's
`noFrontendVao`), and the `x ? noFrontend : MGB_CTX->Get…()` ternary — all three constant-fold to
the pre-change expression in a pull build. No other shared-code delta; the push-monolith arm takes
the frontend branch everywhere, token for token. **I did not run `symbol_report.py`** (no pull
baseline here, and the flavour does not build at the base either way); the evidence I do have is
§6's: base and head fail `nodisagg` with the same two pre-existing errors, so my diff added no
pull-build text that fails to compile.

## 8 Rulings wanted

- **R-1** `Fatal{RoleViolation, "program-handle-arm"}` follows `…OffTheHandleArm`'s
  `"texture-handle-arm"`. Confirm the vocabulary (alternative: `Fatal{UnmigratedVerb, "create_shader_state"}`).
- **R-2** The hoist's second conjunct. Keep the mixed A/B pulling, or have tx2 give
  `sampledTargetForUnit` an archive arm so the conjunct can go?
- **R-3** The new `DirectGLES.Split.StrictProgramArm.` lane (4 entries, `integration-split` only,
  private log paths). It exists because **nothing gated my marker** — pg declared this red-once and
  it never landed. mv and gl will want to extend it and all three of us touch
  `MG_IntegrationTest/CMakeLists.txt`, so **expect a conflict** and consider re-homing it to the
  integration commit. Name, labels and scenario choice are yours to overrule.
- **R-4** Add `GetTransformFeedbackProgram@DrawArrays` (11) to the allowlist table in the addendum /
  `STRICT-WORKLIST.md`; §5.7 already rules it out of P5e.
- **R-5** Pre-existing, not mine: on the **dispatch** path `BindCurrentTextures`' by-handle memo key
  uses `st.DrawProgram` although the caller's program is `st.DispatchProgram`. Benign today (the
  by-handle resolution does not consult the program at all, so it only over-keys) but it is a key
  naming the wrong row.
- **R-6** `~/w7/notes/tools/p5e_gate.sh` was edited **while my `gpu` run was reading it** (mtime
  11:55:45, new `flavours` step). The ctest log is unaffected (1294/1294) but the driver printed a
  parse error from the half-written file; I ran `flavours` from `.p5e/p5e_gate_snapshot.sh`. Pin the
  driver for the wave.
- **R-7** Two `flavours` findings that are the integrator's: (a) the `pull` arm cannot exist as
  specified — `MOBILEGL_BUILD_DISAGGREGATED=ON` forces `MOBILEGL_PIPE_PUSH=ON`
  (`CMakeLists.txt:496-500`), so only `nodisagg` is a real pull build; (b) `nodisagg` is red at
  `7c6f6886`. Both are one-line fixes; say the word and pa takes them.

## 9 What I did not do

- No device run, no adb, no push, no merge; no other tree touched.
- No accessors added to `ProgramArchiveSource`, no shared-tail refactor of
  `SyncCurrentVertexAttributeValues` (§2, G1).
- No DirectVulkan `GetProgramForDraw` sites (`VulkanRenderer.cpp:6384/6815/6853/7424/7476`): Magma
  keeps the lockstep for all of P5e (ID-90).
- No relink scenario added to the split lane (§3's gap).
- Left in the tree: `build-nodisagg/` and `build-pull/`, both untracked, created by the `flavours`
  step (which `rm -rf`s them itself on the next run).
