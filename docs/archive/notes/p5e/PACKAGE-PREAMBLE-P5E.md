# P5e package preamble (read first; applies to every package agent)

You implement ONE package of P5e ("retire the lockstep on the Espryt draw path"). The plan is
`BRIEF-P5E.md` (your package's section is your scope: the files and LINE RANGES you may edit; a line
outside your range is the integrator's - ask via your report, do not edit it), the rules are
`CONTRACT-P5E-draft.md` (a draft until c0e lands it as `MobileGL/MG_Remote/CONTRACT-P5E.md`; the
integrator's rulings in `INTEGRATOR-DECISIONS-P5E.md` override the draft where they differ), and the
map of the code is your scout report `scout-S?.md` plus the E note and the Kimi audit
(`kimi-audit.md`, an independent list of every apply-thread read of client memory - cross-check
your family's rows against it and say in your report which rows you retired, which you left
barriered, and which you found not to exist).

## Where you work

Your tree is a WSL git worktree: `~/w7/p5e-<slug>` on branch `p5e/<slug>`, reachable from Windows as
`//wsl.localhost/Arch/home/swung/w7/p5e-<slug>/` (read and edit files through that UNC path with the
file tools; `mkdir` does not work over UNC - create directories with a wsl command). Every shell
command runs inside WSL through a script file:
```
MSYS_NO_PATHCONV=1 wsl.exe -d Arch -- bash -lc "cd ~/w7/p5e-<slug> && <command>"
```
(For anything longer than one line, write a `.sh` into your tree's `.p5e/` directory over UNC and
run it with `bash ~/w7/p5e-<slug>/.p5e/<name>.sh`.) Do not touch any other tree, never `~/w7/pipe`,
never the Windows checkouts under `FoldCraftLauncher/`. No adb, no devices. No `git lfs` commands.
Never `git reset --hard` / `git checkout -- .` on trees you did not create.

Build and gate (the split flavour is already configured in `build-split`; the first build after
your edits is incremental with ccache):
```
bash ~/w7/notes/tools/p5e_gate.sh <slug> build          # cmake --build build-split, prints errors
bash ~/w7/notes/tools/p5e_gate.sh <slug> unit           # ctest -L unit (tcache_count=0)
bash ~/w7/notes/tools/p5e_gate.sh <slug> isplit         # ctest -L integration-split (lavapipe)
bash ~/w7/notes/tools/p5e_gate.sh <slug> strict         # the strict lane (MOBILEGL_IPC_STRICT_ERRORS=1)
bash ~/w7/notes/tools/p5e_gate.sh <slug> gens           # gen_pipe / field_ownership / dirty-surface / include-closure / doc citations
bash ~/w7/notes/tools/p5e_gate.sh <slug> one '<ctest -R regex>'   # a subset, with output on failure
```
Each writes `~/w7/p5e-logs/<slug>-<step>.log` and prints the tail. A build takes 1-10 minutes; unit
~1 min; isplit ~10 s; run them in the foreground (they finish inside the tool's timeout) - if a step
exceeds 9 minutes, run it with `run_in_background` and wait for the notification.

## Rules (the same as every phase)

- Commit on your branch as you go (`git -C ~/w7/p5e-<slug> commit`), single-line subject
  `[Scope] (Disaggregated): <what>` with `- ` bullets in the body for the why; NO `Co-Authored-By`,
  no attribution lines. Check `git var GIT_COMMITTER_IDENT` prints `Swung0x48 <swung0x48@outlook.com>`
  before your first commit; if not, `git config user.name Swung0x48 && git config user.email
  swung0x48@outlook.com` in your tree only. Do not push, do not merge.
- Every correctness-bearing change gets a red-once: the brief names them; run the red (revert the
  arm or use the hook), see the named Fatal / red test, put it back, record the exact evidence line
  in your report. Keep tests light otherwise.
- G1: the pull build must not change. Keep every split-only change under `#if MOBILEGL_BUILD_DISAGGREGATED`
  (or transport-gated inside an existing `MOBILEGL_PIPE_PUSH` arm, per ruling 1); name any delta
  to shared code in the report. G2/G14: a test that exists only in the split build gets a skip twin.
- No hot-path instrumentation left in; counters behind `MG_Util::PipeStats::Enabled()` are fine.
- Comments in the file's voice: the WHY, updated not deleted.
- Rework rule: if your own gate stays red after two full attempts on the same defect, stop and
  report the state precisely rather than thrashing.

## Report

Write `~/w7/notes/p5e/reports/<slug>-v1.md` (over UNC: `//wsl.localhost/Arch/home/swung/w7/notes/p5e/reports/`)
BEFORE your final message: what changed per file (with the line ranges you touched), the rows of
your family retired / left barriered / not found, the red-once evidence (verbatim lines), the gate
results (exact pass counts), G1/monolith impact, seams you need from other packages and what you
assumed about them, the rulings you need from the integrator, what you did not do. <= 200 lines.
Your final message is the structured result only (branch, head sha, report path, gate summary,
blockers).
