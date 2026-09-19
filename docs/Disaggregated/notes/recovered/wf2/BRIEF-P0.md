# P0 implementation brief (MobileGL disaggregation, plan "MGPipe")

You are one implementer among several working IN PARALLEL on phase P0 of the plan in
`C:/Users/geekerwan/AndroidStudioProjects/FoldCraftLauncher/MobileGL-disagg/docs/Disaggregated/PLAN.md`
(that file is being consolidated by another agent right now; if it is missing or mid-rewrite, read
`PLAN-B-MGPipe.md` in the same directory - same content, older numbering). Read the plan's §0 (decisions D-B1..D-B8),
§4 (interface MGPipe), §6.2/§6.3 (PipeInputs, residual block), §7 (reverse channel), §8 (inherited transport design),
§10.3 (gates) and §11 P0 before touching code. Cite plan sections in your commit body.

## Where the code lives and how you build it (WSL "Arch", 28 cores)

- Reference tree: `~/w7/pipe` (branch `feat/disaggregated`, warm `build-linux` with clang+ccache, unit + integration tests ON,
  lavapipe/llvmpipe ICD). DO NOT edit `~/w7/pipe` or `~/w7/base` - they are read-only references.
- Make YOUR OWN tree: `cp -a ~/w7/pipe ~/w7/p0-<slug>` (a few minutes; keeps the warm build dir), then work only there.
  Single-writer rule: never touch another slug's tree.
- Run WSL commands from the Windows Bash tool as:
  `MSYS_NO_PATHCONV=1 wsl.exe -d Arch -- bash -lc 'bash /mnt/c/<path-to-a-script>.sh' 2>&1 | tr -d '\0'`
  Put every non-trivial command sequence into a script file under your scratchpad directory (inline quoting through wsl.exe
  gets mangled). Your scratchpad is `C:/Users/GEEKER~1/AppData/Local/Temp/claude/C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL/eb1a694b-6a85-4ee7-810d-692eabf8fda6/scratchpad/wf2/p0-<slug>/` (create it; Windows path `/mnt/c/Users/geekerwan/AppData/Local/Temp/claude/...` from WSL).
- You may read/edit tree files from Windows through the UNC path `//wsl.localhost/Arch/home/swung/w7/p0-<slug>/...` with the
  Read/Edit/Write tools, or edit inside WSL with python/sed from a script. Keep LF line endings.
- Build: `cd ~/w7/p0-<slug> && export CCACHE_BASEDIR=/home/swung/w7 && cmake --build build-linux -j 24 2>&1 | tail -30`
  (reconfigure with the same cmake line as `~/w7/prep.sh` only if you changed CMake files: copy that line, keep `-B build-linux`).
- Tests: `cd build-linux && ctest -L unit -j 12 --output-on-failure` (428 unit tests must stay green) and, when your change
  touches a backend or MG_State, `ctest -L integration-gpu -j 8` (367 scenarios; compare the pass/fail NAME set with the
  baseline recorded in `~/w7/pipe-itest-baseline.txt` if present - a failure that is already in the baseline is not yours).
  Per the user's rule, run REPRESENTATIVE tests locally, not everything, unless your package says otherwise.
- Android (only if your package needs it): `~/w7/pipe/build-android` is warm; NDK `/home/swung/android-sdk/ndk/27.3.13750724`,
  SDK `~/android-sdk`, gradle `~/gradle-8.10.2`, JDK `/usr/lib/jvm/java-21-openjdk`.
- Devices (only if your package needs them): `35d0befa` (Xiaomi 24129PN74C, Adreno 830) and `3B159D009VZ00000` (Oppo PLG110,
  Mali). Both are SHARED with another running campaign. Protocol: acquire an atomic mkdir lock BEFORE any adb use:
  `LOCK="C:/Users/GEEKER~1/AppData/Local/Temp/claude/C--Users-geekerwan-AndroidStudioProjects-FoldCraftLauncher-MobileGL/8ab6b3cb-c783-4826-9fd7-39ff3498af5b/scratchpad/w7/locks/<serial>"; until mkdir "$LOCK" 2>/dev/null; do sleep 60; done; echo "p0-<slug> $(date)" > "$LOCK/owner"` ... `rm -rf "$LOCK"` when done. Hold it at most 25 minutes per acquisition. If a lock is held for more than 60 minutes (check `cat "$LOCK/owner"`), do NOT break it: report "blocked by device lock" in your result and finish the non-device parts. Always pass `-s <serial>` to adb. Wake+unlock before launching apps (`adb -s X shell input keyevent KEYCODE_WAKEUP; input keyevent 82`). Read device logs from `/sdcard/MG/latest.log`, not logcat.

## Rules

- Commit on a branch named `p0/<slug>` in YOUR tree (`git checkout -b p0/<slug>`), one or a few focused commits.
  Commit message format (mandatory): first line `[Type] (Scope): short lowercase description`; blank line; then `- ` bullets
  with the rationale, plan section citations and evidence. Types: Feat / Fix / Test / Docs / Build. NEVER add Co-Authored-By
  or any AI attribution trailer.
- Do not push anywhere. The coordinator fetches `p0/<slug>` from your tree by path.
- The default ALL target must build; unit tests must be green; no per-draw `fprintf`/`printf` instrumentation may be committed
  (a CI grep gate for that is one of the P0 deliverables).
- Logging: `MGLOG_D` for non-critical lines (compiled out in INFO builds), never `MGLOG_I` for per-frame noise.
- Do not modify `3rdparty/` contents except adding the flatbuffers submodule where your package says so.
- Do not touch the fixtures under `tools/trace_replay/fixtures/` (they are real binaries in the tree; `git checkout` would
  turn them into LFS pointers).
- When you are done, return: the branch name and tree path, the list of commits (hash + subject), what you tested (exact
  commands and results), anything you could not finish and why, and any deviation from the plan with justification.
