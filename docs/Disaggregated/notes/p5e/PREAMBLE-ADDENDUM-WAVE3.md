# Addendum to `PACKAGE-PREAMBLE-P5E.md` for wave 3 (pa / mv / gl), 2026-09-18

The preamble is still correct except where this file overrides it. Read both.

## 1 The contract is landed, the draft is gone

`CONTRACT-P5E-draft.md` was landed by c0e as `MobileGL/MG_Remote/CONTRACT-P5E.md` **in your tree**.
Read the landed file, not the draft. `INTEGRATOR-DECISIONS-P5E.md` now runs to **ID-110** and still
overrides both where they differ.

## 2 The gate has a sixth step, and it is not optional

```
bash ~/w7/notes/tools/p5e_gate.sh <slug> gpu        # ctest -L integration-gpu, ~10 min at -j 4
```

`integration-split` entries ALL export `MOBILEGL_TRANSPORT=inproc`. The push build has a **second
server arm** (`Transport == Monolith`) selected at runtime, and until 2026-09-18 it had no gated
entry at all. A P5e package landed a null dereference on it under 2250 green unit entries and 113
green split entries: the game SIGSEGV'd at startup on the phone, and `ctest -L integration-gpu` on
the same head was **1157/1294 — 137 SEGFAULTs across 38 scenarios**. See rulings ID-107 / ID-109 /
ID-110, fix commit `66621767`, and `docs/Disaggregated/CURRENT_STAGE_PROGRESS.md` §2.5.

**Run `gpu` before you report. 1294/1294 or explain.**

## 3 The arm-selection rule, stated as a checklist

Ruling ID-81: handle arms are selected by `MG_Config::Transport != Monolith`; the push-monolith
build keeps its frontend arms token for token. Apply this to your own diff:

- **"A handle was noted" is NOT the statement "the record arm is selected."** They are different
  claims and conflating them is exactly what cost the phase 137 scenarios. A seat that is safe only
  because every writer of a note happens to be transport-gated is safe *by a chain*, not by its own
  statement. State the transport test where you rely on it. `MGB_TEXTURE_RECORD_ARM_SELECTED()`
  (`MG_Backend/DirectGLES/Managers.cpp`, ID-110) is the worked example.
- **Never infer the arm from a pointer being null.** Decide with `MG_Config::Transport` (or your
  family's selector, which must itself reduce to it) and pass the pointer as glue.
- **A null/missing record on the handle arm is a NAMED refusal, never a quiet fall-back to the
  frontend.** `RefuseNullFrontendTextureOffTheHandleArm` (`Managers.cpp`) is the shape: it aborts
  with `Fatal{RoleViolation, "<name>"}` and folds to a constant in the pull build so no call is
  added there.
- **If you pass a null object into a body that can still read one, you have written the fix1 bug.**
  Either the body cannot reach a frontend read on that arm, or the refusal fires first.

## 4 Reading the strict lane properly

`ctest` does NOT carry `MGLOG_F` into its captured stdout, and the lane's entries write no log file,
so `p5e_gate.sh <slug> strict` can only give you a pass count. **The pass count is not your
deliverable.** Produce the per-entry marker census:

```
bash ~/w7/notes/p5e/strict_census2.sh ~/w7/p5e-<slug>
```

It re-runs each red entry alone against the test binary with `MOBILEGL_LOG_FILE_PATH` set and writes
`~/w7/p5e-logs/strict-census/census2.tsv` (marker TAB entry). Copy it to a per-slug path first if
you do not want to clobber the shared one. Report the table before and after your change.

**One known blind spot in that script**, which you must not repeat as a finding: it derives the
gtest filter from the last two dot-components of the ctest entry name, so entries with a third
segment (`Ct.`, `F1.`, `SmallRing.`, `PersistentMapArm.`, `NamedBlit.`) are re-run WITHOUT the extra
`ENVIRONMENT` their lane block sets. `DirectGLES.Split.PersistentMapArm.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares`
is the one entry whose marker the census failed to capture for this reason. If your family touches
such an entry, take its marker under its own lane environment (`ctest -V -N -R '^<entry>$'` prints
the block's environment).

## 5 The baseline you are changing, measured

Head `7c6f6886`. `integration-split` 116/116 green; under `MOBILEGL_IPC_STRICT_ERRORS=1`, 7 pass and
109 abort with:

| marker | entries | owner |
|---|---|---|
| `GetProgramForDraw@DrawArrays` | 61 | **pa** |
| `GetFramebufferBindingSlot@ReadPixels` | 21 | allowlist (readback) |
| `GetBoundVertexArray@DrawArrays` | 9 | **mv** |
| `GetProgramForDispatch@DispatchCompute` | 7 | **pa** |
| `GetTextureObject@CopyImageSubData` | 6 | allowlist (retires P7) |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | allowlist (retires P9) |
| `GetTextureUnitObject@CopyTexImage2D` | 2 | allowlist (CopyTex) |
| (census artifact, §4) | 1 | — |

Touch only your own rows. If your change removes or adds a row that is not yours, that is a finding
for the report, not something to quietly absorb.

## 6 Device numbers, for context only — you do not run the device

Redmi, Minecraft 26.3-rc-3, ~849 draws/frame, GPU pinned, fan on, 30 s windows, interleaved:

| arm | fps p50 | client GL thread ms/frame | apply thread ms/frame |
|---|---|---|---|
| monolith ×3 | 267.9 / 195.1 / 254.2 | 3.898 / 4.781 / 4.694 | — |
| inproc ×2 | 166.4 / 143.5 | 6.098 / 6.958 | 5.305 / 5.653 |

The split arm's client thread costs about 1.5× the monolith arm's single thread, and the device
stats line shows the client entering the doorbell wait about 916 times per frame against 849 draws.
That is the lockstep your package is clearing the way to remove. It is context, not a target: your
deliverable is the marker table and the gate.
