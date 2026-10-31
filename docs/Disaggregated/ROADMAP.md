# Roadmap (index)

> **2026-10-07**: P0 through P7, P12, P9, P10, P11, P8, and **P13** are done — both the split route and the monolith route are complete. P13 (retire the pull path): the monolith also runs only on the record arm; the pull / push builds, the `MOBILEGL_PIPE_PUSH` and `MOBILEGL_PIPE_LEGACY_MEMOS` compile-time switches, the residual value blocks, and the backend's frontend-object arm are all deleted; the library splits into seven modules partitioned by the link ratchet, and in the split build the server half is truly linked on its own; G1 is retired, succeeded by zero `MG_Remote` symbols, the skip census, and the recorder golden. Performance work is queued after it, with the `dev` build as baseline. The next item is **P14** (state ownership). The plan, acceptance results, measured results, and reports for each phase are in git history.

## Goals

The client (the side that runs the application) and the server (the side that runs the driver) can connect over TCP across **different machines and different operating systems**; on the same machine they use two processes + shared memory. The transport has two layers, control plane and data plane, each independently selectable and mixable.

## Two routes

- **Monolith route**: build the interface first, so the monolith benefits too (the backend owns its own state and can move to the render thread). P0 → P0.5 → P1 → P2 → P3a → P4a → P3b/P4b → P7 → P8 → **P13** (complete).
- **Split route**: split the frontend and backend step by step on top of the interface. P5 → P5b → P5c → P5d → P5e → P5f → P6 → P6.5 → Ph → P12 → P9 → P10 → **P11** (complete).

## Phase list

| Phase | What it does | Status |
|---|---|---|
| P0 | Lay the foundation: counters, call catalog and generators, CI gates; verify Android can launch a second process | ✅ 09-05 |
| P0.5 | Extract the data type header shared by frontend and backend, cutting the backend's dependency on frontend headers | ✅ |
| P1 | The backend reads an "input block" instead, and can verify per draw that it matches the frontend's real state | ✅ |
| P2 | The frontend starts actively pushing render state; interface cost measured at +6–12%, decision to continue | ✅ 09-08 |
| P3a | GLES backend's buffers and vertex arrays switch to handles | ✅ 09-08 |
| P4a | GLES backend's framebuffers, textures, samplers, programs switch to handles | ✅ 09-08 |
| P5 | First split into two threads: command queue + backend thread (every command waits) | ✅ 09-16 |
| P5b | Move the remaining commands onto the queue; first on-device run of the target game with a separate backend thread | ✅ 09-16 |
| P5c | The two threads no longer secretly share memory; they exchange only through the queue | ✅ 09-17 |
| P5d | Two-thread performance: in-game 7–13 fps → 103–106 fps | ✅ 09-18 |
| P5e | The client runs ahead and no longer waits for the backend on every command; matches single-threaded under heavy load | ✅ 09-19 |
| P5f | All state passes through the queue, clearing the last obstacle to splitting into processes; the Vulkan backend also runs ahead | ✅ 09-20 |
| P6 | Split into two processes; on device the two-process cost matches two-thread | ✅ 09-22 |
| P6.5 | Transport splits into control plane / data plane; a PC as client and a phone as server run over TCP | first wave ✅, remainder to CI |
| Ph | The server safely accepts external connections: token, quotas, bad data does not crash | required items ✅ (with P7) |
| P3b / P4b | Deepening and wrap-up of the GLES backend | mostly ✅, remainder in parallel |
| P7 | Full Vulkan backend migration; on-device frame check 36/36 passes | ✅ 09-23 |
| **P12** | **The server opens its own window and presents on screen; the client needs no window** | ✅ 09-29: exit gate B (cross-machine TCP) ✅ 09-28, A (FCL + killing the server) ✅ 09-29; contract in git history; G1 holds; remainder into [`DEBTS.md`](DEBTS.md) |
| **P8** | ~~Move the remaining emulation paths to the correct side and complete the protocol~~ → **Close the server-side emulation gaps and split coverage** | ✅ 09-30: re-scoped (6 of the original 10 items were already done as prerequisites); Espryt mip generation / staging copies with GPU writes / driver refusing reads, Magma wire-native indirect and dispatch; 246 cases brought onto the split three arms (`split_coverage.py`); the Create dropped-copy fix on Adreno landed in dev and feat; contract in git history; remainder into [`DEBTS.md`](DEBTS.md) |
| **P9** | **Reverse channel: readback, logs, and re-mint no longer wait synchronously for the client** | ✅ 09-29: PACK-PBO readback fire-and-forget (both backends), graded log forwarding, re-mint without pull (`OnLog` / `OnTexturePullRequest` deleted, callbacks 9 → 7), F1 / F2; contract in git history; G1 holds; event-volume batching not done, no measured need; remainder into [`DEBTS.md`](DEBTS.md) |
| **P10** | **Synchronization objects, queries, and frame pacing** | ✅ 09-29: fence polling answered locally by the server's own report (`kEventFenceSignaled`, `MOBILEGL_IPC_POLL_ESCALATE`; host Magma·tcp −37%, zero round trips for both backends on Adreno 830); `SetSwapInterval` forwarded, class C zeroed; query / XFB / AsyncCompile registered on each split arm; credit 1/2/3 device loopback within noise; contract in git history; G1 holds; remainder into [`DEBTS.md`](DEBTS.md) |
| **P11** | **Same-machine large-buffer zero-copy sharing** | ✅ 09-29: T0 zero-copy import (both backends, on by default, silently falls back to T2 when the self-test fails or on Stream); same-machine external client goes through shared memory via helper + token broker (B1); connections paired by `PairBind` nonce (revision 5); adoption tier fixed at handshake; contract in git history; remainder into [`DEBTS.md`](DEBTS.md) |
| **P13** | **Delete the old "backend reads the frontend directly" path** | ✅ 10-07: the monolith runs only on the record arm; the pull / push builds and the `PIPE_PUSH` / `LEGACY_MEMOS` compile-time switches, the residual value blocks, and the backend's frontend-object arm are deleted; seven OBJECT modules and server-only real linking; link ratchet 172 → 27 named debts; recorder golden; G1 retired; FCL on-device smoke passes on both backends; performance round (baseline `dev`) and remainder into [`DEBTS.md`](DEBTS.md) |
| **P14** | **State ownership: multiple sessions / multiple contexts / share groups in one process** (prerequisite for a unified server; anland's built-in renderer does not isolate per process) | design ✅ [`design/11-state-ownership.md`](design/11-state-ownership.md), implementation pending |

## See also

- Engineering discipline and verification gates: [`guide/discipline.md`](guide/discipline.md)
- Still-open debts: [`docs/Disaggregated/DEBTS.md`](DEBTS.md); open questions: [`docs/Disaggregated/OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md)
