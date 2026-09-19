# Package vi — VAO / vertex input / index from records (P5e wave 2)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-81,
ID-82, ID-95 are yours), `BRIEF-P5E.md` (§0, §1, §2 "vi", §5, §6 rulings 1, 2, 19),
`CONTRACT-P5E.md` §4 and §5.1 (landed by c0e in `MobileGL/MG_Remote/`; the draft in the notes dir is
the same text), your scout report `scout-S1.md` (all of it), and the VAO / buffer rows of
`kimi-audit.md` (rows 1-34) — your report must say, per row, retired / left barriered / not found.
All notes are in `//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `vi`; your tree is `~/w7/p5e-vi` (branch `p5e/vi`, based on the landed id head, already
configured and built). The twin resolvers `ResolveVaoTwin(MGPipeHandle)` and the by-handle registry
API are id's and already in your base — use them, do not re-declare.

## What "done" means for this family

On the DirectGLES draw path under an active transport, nothing in the VAO / vertex-input / index
family reads the frontend: the twin resolves from `st.BoundVertexElements`, the attribute walk and
its dirty repair run over `st.VertexBuffers[Start..Start+Count)` and the CSO record's `Attributes[]`,
the IBO arm resolves `st.IndexBuffer.Res`, every buffer ensure is
`EnsureBufferResourceForHandle(nullptr, handle)`, and the clean stamp uses the server-owned
`{elementsHandle, elementsSerial, buffersSerial}` (+ the new `iboSerial`) instead of the VAO's
config version. The three frontend-keyed scope sites of this family (`DirectGLES.cpp:1609, :6729,
:6775` at the P5d base — re-resolve) are gone.

## Scope (the only regions you may edit; anything else is the integrator's)

`MG_Backend/DirectGLES/DirectGLES.cpp`: `SyncVaoAttributeBuffersByHandle` + the IBO arm of
`SyncNeccessaryBuffers`; `ResolveVaoTwin` / `SyncCurrentVAO` / `SyncCurrentVertexAttributeValues`;
the VAO lines of `PrepareForDraw` only; the restart helpers (`BoundElementArrayBuffer`/`...Id`);
the `DrawArrays` / `MultiDrawArrays` client-array arms (monolith-only after your change).
`MG_Backend/DirectGLES/Managers.h`: `ResolvedDrawBuffers` (`Entry::frontend` leaves the split arm,
`iboSerial` added) and `PendingAttribValueMask`'s key. `Managers.cpp`: `SyncToBackendFromApplier`
(signature/entry only — its body is already record-only and is your existence proof),
`SyncClientSideAttributesForDrawArrays` (monolith-only). `MG_Remote/Client/EmitTables.cpp`: the
`kDrawClientArrays` flag and the run-ahead refusal beside the existing `CLIENT_INDICES` refusal.
Tests: `MG_Test/Pipe/VertexInputEmitTest.cpp`, a `SanityTest.cpp` sibling, and the scenario work below.

## Rulings that bind you

- ID-82 / ruling 2: client-memory vertex arrays are **not** refused today (your scout verified it).
  Refuse them when run-ahead is armed — `kDrawClientArrays` on the draw record, a named
  `Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}`-shape refusal on the client. Lockstep keeps
  today's behaviour exactly. Answer the open question yourself: does any existing scenario
  (`DoublePrecisionScenario`?) exercise a client array under split — if none does, add one so the
  refusal has a red-once.
- ID-81 / ruling 1: handle arms only under `Transport != Monolith`; the push-monolith build keeps
  its frontend arms and its bytes.
- ID-95 / ruling 19: `set_vertex_buffers`' window must cover the attributes the CSO declares; pin it
  with a test in the same validate step (a narrower window is `Fatal{ProtocolCorruption, "<row>.Count"}`).

## Red-once (execute each, revert, quote the evidence line in the report)

1. Revert `ResolveVaoTwin` to the frontend lookup → the id guard's `Fatal{RoleViolation, "MGPipeSlots"}`
   (use id's test hook to force "unbarriered" — run-ahead is not armed yet).
2. Publish `set_vertex_buffers` for A,B, repoint the frontend VAO at C without re-emitting, sync →
   A,B must be ensured; the revert ensures C.
3. The same on `set_index_buffer` / `st.IndexBuffer.Res`.
4. `glDrawArrays` with a client array under the run-ahead flag → the named refusal.

## Gate

`build`, `unit`, `isplit`, `gens` green; `one` on `VertexAttribBindingScenario|VertexArrayEnableDisableScenario|PrimitiveRestartScenario|ResidentIndexScenario|MultiDrawScenario|DrawParametersScenario|HandleRecycleScenario|ObjectSubsystemControlScenario`;
`strict` — record which `UnmigratedPipeInput` markers of this family disappeared vs the c0e baseline
in your report (that list is the phase's progress metric). Also answer, with evidence: does any
GL-thread path bump `CurrentBufferMutationEpoch` under a transport (your scout's open question 2) —
pin the answer with an assertion or a test rather than an argument.

Trailing (do NOT do): deleting `ResolvedDrawBuffers` (needs a measurement first), staging client
arrays (P8).
