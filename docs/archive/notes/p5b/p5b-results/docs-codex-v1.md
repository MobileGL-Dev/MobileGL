# P5 / P5b docs close-out, Codex

Date: 2026-09-16. Branch `p5b/docs-codex`, independent worktree `/home/swung/w7/p5b-docs-codex`, base `2deca994`.
Commit: `dab238938eb55eec993d90ce1f5a9b753d8712bb` — `[Docs] (Disaggregated): record P5 final gate stop and scoped control evidence`.
Only `docs/Disaggregated/{README,ROADMAP,MEASUREMENTS}.md` changed: 102 insertions / 32 deletions. No source, integration worktree, main progress note, device, build or test changes. No repeated tests or adversarial review.

## What changed

- README and ROADMAP now distinguish landed P5 code, the historical reduced IPC-frame milestone, the stopped final-head gate, and the P5b real-workload exit. P5.5 is renamed P5b. P6 remains after P5b.
- MEASUREMENTS §27 retains historical joint readings and adds x2, v1-r3 and r2 corrections. Old 511 aborts are 505 class-C plus six P5 StageSnapshotTooNarrow defects fixed by v1-r3. The 27 wrong answers are 22 texture readbacks, three queries, one inspection and one FBO/RBO ReadPixels lifetime path; the last pixel causality remains unisolated.
- §28 replaces maxrec pending with measured 784 B SetVertexAttribDefaults on reduced/OpenRA, the default 4 MiB cap and the SmallRing ledger. Old x2 ringwaits was reclaim evidence, not an actual wait; r2's 1/8 MiB wait/wrap controls are separately attributed. Existing rsp is a lower bound and inproc RSS remains one shared-process measurement.
- §30 records Redmi A/B runner `eec0e836`, APK source `e61d0012`, 40 groups / 27 completed / 13 failed; all ten split groups abort before benchmark. Five concise rows preserve useful frame-time readings. No barrier-tax estimate is invented; full tables and per-thread data remain in ab-v1.md.
- New §31 records `37fc4fdb` final gate: builds/G1/unit/reduced ran, E1 own diagnostics passed, then E3(a) 4 pixel failures + 2 designed skips was correctly rejected. Part 2 and Part 4 were not reached. Package, historical joint and APK evidence is not relabeled final merged-head proof.
- r2 corrections #5–13 and package acceptance are summarized, including exact library restore, four own E3 pixel/private-log reds, all-skip rejection, REQUIRE_GPU, fresh/nonempty census, production G5 pins and the current-name c1f 15/15 mutation run. No new G1 measurement is claimed for r2. ROADMAP names r1's four remaining corrections, non-verb rsp, illegal reply status and tree-external runner propagation.

## Evidence read

Under `notes/p5/p5-results`: joint-v1.md, x2-v1.md, x2m-v1.md, j0-v3.md, v1-v3.md, ab-v1.md, ap-v1.md, apkci-v1.md, p5-close-codex-review.md. Under `notes/p5b/p5b-results`: r2-v1.md. Also `/home/swung/w7/p5-final-gate.log` (741 lines at documentation time), which pins its own source HEAD on line 1 and ends with E3(a)'s selected-skip failure.

Final-gate log SHA-256: `51d15bd11d06851fa9f7934eeac1a162f8db45c8b81dc345dd76e77701c7531c`.

## Validation and handoff

`git diff --check` clean; UTF-8/replacement-character and final-newline checks on all three edited files passed. Changes were checked against the cited report tables and the final gate's stopping point. Committer identity before commit: `Swung0x48 <swung0x48@outlook.com>`; single-line commit without trailers; final tree clean. No binary testing was needed for this documentation-only change.

Integrator can cherry-pick `dab23893` onto the evolving merge head. This report does not claim r1 is currently fixed or a later integration gate is complete; those need their own report/head. Existing §26 historical numbers and architecture text are retained. The main progress MD is owned by the integrator and was not edited.
