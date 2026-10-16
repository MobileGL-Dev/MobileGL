# P5e — what is actually left before the strict lane can be a hard gate

Head `7c6f6886`. Measured, not assumed: the per-entry marker census is
`~/w7/p5e-logs/strict-census/census2.tsv` (produced by `strict_census2.sh`, which runs each red
entry alone against the test binary because ctest swallows `MGLOG_F` and the log-file knob is read
before the config loader, so the lane's own entries never write one).

## 1 The census

`integration-split` is 116/116 green. Under `MOBILEGL_IPC_STRICT_ERRORS=1`: 7 pass, 109 abort.

| marker | entries | who retires it | verdict |
|---|---|---|---|
| `GetProgramForDraw@DrawArrays` | 61 | P5e | **must go** |
| `GetFramebufferBindingSlot@ReadPixels` | 21 | P5e row, but a READBACK | allowlist candidate |
| `GetBoundVertexArray@DrawArrays` | 9 | P5e | **must go** |
| `GetProgramForDispatch@DispatchCompute` | 7 | P5e | **must go** |
| `GetTextureObject@CopyImageSubData` | 6 | P7 | allowlist candidate |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | P9 | allowlist candidate |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | P5e row, but CopyTex | allowlist candidate |
| (none captured) | 1 | — | **census artifact**, see §4 |

## 2 The program family (68 of the 109) — and the wire already carries the answer

The important finding: **no wire change is needed.** `MGPipeShaderCsoRecord`
(`MG_Pipe/PipeApply.h:391`) already holds `SharedPtr<const ProgramArchive> Archive` — the whole of
the frontend's `LinkArtifacts` plus the SPIR-V, encoded once per link by pg — and the three
post-link mutable tails (`BlockBindings`, `SamplerUnits`, `StorageOverrides`) with a
`BindingsSerial`. What is missing is not data on the wire; it is **readers that still ask the
frontend object instead of the archive**.

Three sites, all in `MG_Backend/DirectGLES/DirectGLES.cpp`:

| site | what it is | state |
|---|---|---|
| `:6006` `PrepareForDraw` | `const auto& currentProgram = MGB_CTX->GetProgramForDraw();` — **unconditional**, no arm test. Merely calling the accessor trips `MGP_INPUT_CHECK` (`MG_Backend/MGPipe/PipeInputs.h:659`), so this alone is the per-draw marker. | the whole fix |
| `:6746` `BindCurrentTextures()` (the no-arg overload) | same accessor, same problem | same fix |
| `:7374`, `:7446` | already behind `if (ProgramHandleArm())` — pg did these | done |

Two genuine consumers keep it alive under the handle arm:

- **`ResolveAndBindUnitTextures` (`:6090`, reached from `BindCurrentTextures` at `:6719`)** reads
  `GetMaxUniformLocation`, `GetUniformSamplerOrImageUnitIndex`, `GetUniformType` off the frontend
  program. The memo KEY above it is already handle-only (`:6644-6663`, ruling ID-86) — it is the
  miss path that still reads the frontend.
- **`SyncCurrentVertexAttributeValues` (`:2243`, called at `:6070`)** reads
  `GetActiveAttributeLocationMask` and `GetAttribType`. **The code already names this as the
  remaining work and whose it is**: its own comment at `:2258-2268` says "WHAT IS NOT RETIRED HERE,
  and it is S4/pg's not vi's: the two program reads below … stay until pg carries the attribute
  mask and types in the ShaderCso record."

So the package is: give both readers a handle arm that answers from `record->Archive->Link`, then
arm-select the two accessor calls themselves. `GetProgramForDispatch@DispatchCompute` is the same
shape in `PrepareForCompute`.

## 3 `GetBoundVertexArray@DrawArrays` (9) is confined to multi-draw

All nine entries are `MultiDrawScenario.*` plus `IndexedDrawFamilyScenario.MultiDrawElementsBaseVertexPaintsEverySubDraw`.
vi retired the bound-VAO row on the record arm inside `SyncCurrentVertexAttributeValues`
(`:2290`, `fromRecords ? noVao : MGB_CTX->GetBoundVertexArray()`), so this is a DIFFERENT site on
the per-sub-draw path. Find it and give it the same treatment.

## 4 The one entry with no marker is a census artifact, not a finding

`DirectGLES.Split.PersistentMapArm.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`.
The lane prefix `PersistentMapArm` carries EXTRA ctest `ENVIRONMENT` (the arm knob) that the
direct-binary re-run did not reproduce, so the entry took a different path. Every entry with a
third name segment (`Ct.`, `F1.`, `SmallRing.`, `PersistentMapArm.`, `NamedBlit.`) is subject to the
same doubt and its marker must be re-taken under its own lane environment before the table is
trusted.

## 5 The lane cannot go literally green as the knob is written

`CountBarrierPull` (`MG_Backend/MGPipe/PipeInputs.cpp:98`) aborts on EVERY barrier-pulled read when
`StrictErrors` is set, allowlisted or not. So "GREEN as a hard lane" (BRIEF §3.3) cannot mean
116/116 under the knob unchanged. Either the knob becomes allowlist-aware, or the gate becomes a
marker-set comparison. `StrictBarrierPullFatal`'s `why` argument already exists **for this
purpose** — its comment says "the lane's allowlist step reads this line and the two reasons mean
different things to it". Whatever is chosen has to be decided once and mechanised, not kept by hand.

## 6 The performance target, measured

Monolith at this head, Redmi, MC 26.3-rc-3, ~849 draws/frame, GPU pinned, fan on, 30 s window:

| | |
|---|---|
| fps | 254.9 (p50 267.9, min 197.7, max 275.9) |
| GL thread `Thread-17` | **993.7 ms CPU per second** — 99.4% of one core |
| per frame | **3.898 ms** |

Monolith is single-thread CPU-bound at 3.9 ms/frame. **Parity therefore means
`max(client, apply) <= 3.9 ms/frame` on the split arm**, which is a question about balance and
overhead, not a question about total work: the split may spend MORE total CPU than monolith and
still win, as long as neither thread exceeds monolith's single-thread cost. That is the arithmetic
the optimisation phase has to hit.
