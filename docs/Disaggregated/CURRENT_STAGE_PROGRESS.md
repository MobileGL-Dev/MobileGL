# Current phase: P13 closed (2026-10-07); next phase P14

> **Update 2026-10-07**. This page holds only a summary; plans and rulings are in git history. The previous phase P8 closed on 09-30.

## Goal

Retire the pull path: the monolith (the library embedded in FCL) also runs on the record arm fixed in P8, and the old "backend reads the frontend directly" path and all its compile-time / run-time switches are deleted.

## Completion

| Item | Status |
|---|---|
| W1 CI foundation (skip census, zero `MG_Remote` symbol check, runtime shape proof) | ✅ |
| W2–W3 default flip and macro removal (`MOBILEGL_PIPE_PUSH` / `LEGACY_MEMOS` deleted, bits 0–13 fixed on) | ✅ |
| W4–W5 the monolith switches to the record arm, the record arm moves out of `MG_Remote` | ✅ |
| W6 delete the backend's frontend-object arm (Espryt / Magma), `# P13` debts zeroed | ✅ |
| W7 seven OBJECT modules + server-only link check | ✅ |
| W8 recorder golden (the first run caught and fixed two monolith push-content defects) | ✅ |
| W9 cache capacity re-tuning | moved to the performance round after P13 (user 10-07) |
| W10 documentation and close-out | ✅; FCL on-device smoke passes on both backends |

## Next steps

**P14**: state ownership (multiple sessions / multiple contexts / share groups in one process), design in [`design/11-state-ownership.md`](design/11-state-ownership.md). The performance round (baseline `dev` build, pinned clocks, FCL MC + anland glmark2, both backends) is also queued after P13.

**Awaiting user decisions**: the "needs a ruling" entries in [`DEBTS.md`](DEBTS.md); FCL's silent death is left to the user (details in git history).
