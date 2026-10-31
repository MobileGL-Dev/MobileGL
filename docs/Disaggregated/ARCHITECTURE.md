# Design and architecture (index)

> This page holds only the design points and where each chapter lives; the full design of each part is in [`design/`](design/). Section numbers §1–§17 and appendices A / B are still in use; `ARCHITECTURE.md §N` written in code comments maps to a file in the table below. The original wire contracts for each phase are in git history; where they conflict with the design text, the newer contract wins.

## In one sentence

The backend already maintains a state machine that tracks GLES / Vulkan; all that is missing is an explicit statement of "what I was told". MGPipe has the frontend push state changes to the backend as **records** before each command that does work; the backend recognizes only these records and does not read frontend memory, so a thread, a process, or a network cable can sit between the two.

## Key design points

1. **One call catalog generates everything**: `PipeCalls.def` has one call per line; the wire number is the line order and is append-only; the two function pointer tables, the codec, and the validator are all generated from it.
2. **Objects are addressed by handle `{slot, gen}`**: allocated by the frontend, never sent back by the backend, so creating an object needs no round trip.
3. **State is pushed once before a command**, not by every GL setter; whole blocks cross the wire and are deduplicated by content, and unchanged data is not sent.
4. **One backend implementation, three topologies**: direct calls in one thread / two threads in one process / two processes (possibly across machines).
5. **Reverse messages are named callbacks** (errors, GPU writeback, window size…), over an event channel ordered like commands.
6. **Shaders cross the wire as SPIR-V + reflection information**: compiled in the frontend, translated and specialized by state in the backend.
7. **The transport has two layers**: control plane (handshake, EGL control) and data plane (commands, bulk data), each independently choosing local shared memory or a byte stream / TCP, and they can be mixed; upper-layer code does not distinguish link types.
8. **The client runs ahead (run-ahead)**: only commands that need an answer wait; present credit paces it.
9. **Errors do not crash**: when the server crashes or disconnects, the client enters a "device lost" state; external connections need a token, and malformed input is rejected rather than making the server exit.
10. **Provable migration**: every gate must be able to turn red; the monolith build's binary stays byte-identical throughout the split (G1).

## Where the chapters live

| § | Content | File |
|---|---|---|
| 1–2 | Boundary: two tables, three topologies; object model: handles, two generations, CSO | [`design/01-boundary-and-objects.md`](design/01-boundary-and-objects.md) |
| 3–4 | Call catalog, generators G1–G8, capability bits; record and payload format | [`design/02-catalogue-and-records.md`](design/02-catalogue-and-records.md) |
| 5–7 | Frontend tracker (when to push, what to push, how to deduplicate); texture upload; shaders | [`design/03-frontend.md`](design/03-frontend.md) |
| 8 | Reverse channel: seven callbacks, ordering, errors and ack, texture re-mint (server-local, no pull), XFB | [`design/04-reverse-channel.md`](design/04-reverse-channel.md) |
| 9–10 | Backend rework (`PipeInputs`, memo re-key, A/B arms); server-side applier | [`design/05-backend-and-server.md`](design/05-backend-and-server.md) |
| 11 | Transport: shared segment, ring, doorbell, control plane, backpressure, wait rules, events, two axes | [`design/06-transport.md`](design/06-transport.md) |
| 12–13 | Persistent map and large-buffer adoption; readback, round-trip list, five-part verification gate | [`design/07-memory-readback-verification.md`](design/07-memory-readback-verification.md) |
| 14–15 | Frame pacing and threads; process, handshake, EGL, Android (including the on-screen presentation server), crash handling | [`design/08-runtime-and-platform.md`](design/08-runtime-and-platform.md) |
| 16, Appendix A, Appendix B | Source directory and build options; all switches; boundary counters | [`design/09-build-switches-counters.md`](design/09-build-switches-counters.md) |
| 17 | The shape each phase landed in and the rules still in force (originals in git history) | [`design/10-phase-shapes.md`](design/10-phase-shapes.md) |
| — | State ownership: multiple sessions / multiple contexts / share groups (prerequisite for the anland unified server) | [`design/11-state-ownership.md`](design/11-state-ownership.md) |
