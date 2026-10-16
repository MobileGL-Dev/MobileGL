# P5b final documentation draft — Codex

State: prepared, uncommitted; waiting for DEVICE_DONE + final sourcehead and final host/trace counts.
Base: 348d22a4b9c15cc571af840dedc4b4edc8c53dbb.
Worktree: /home/swung/w7/p5b-docs-final-codex, branch p5b/docs-final-codex.
Files: docs/Disaggregated/README.md, ROADMAP.md, MEASUREMENTS.md only.

The draft records migrated slots A=2/B=54/C=15; separates package measurements, the fixed-head
one-time host gate, census, phase review/targeted repairs, and final device identity/results.
Host JUnit selected/passed/skipped counts are recorded individually; its live-runner shell
anomaly is retained. Integration census is 1267 selected: 811 passed,203 skipped,62 aborted,
191 failed; baseline 432 passes all retained, no removed old names. Trace results remain pending.
The real-workload profile is stage 256 MiB across traces/device arms; production default 32 MiB
and large-single-blob boundary are explicit. P5b remains in progress, P6 remains after its exit.

The shared joint report's CODEX CENSUS markers are untouched. Only CODEX HOST DOCS was appended.
No builds, tests, new reviews, adb calls or integration tree changes were made for this task.
git diff --check passed on the draft. No commit until the integrator's final evidence signal.
