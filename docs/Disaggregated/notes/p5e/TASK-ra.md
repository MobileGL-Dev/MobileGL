# Package ra — the wait rule, present credit, server-owned `gPipeInputs` (P5e; parallel, lands last)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-83,
ID-84, ID-91, ID-92, ID-93 are yours), `BRIEF-P5E.md` (§0, §1, §2 "ra", §3 the integration gate, §4
the device exit, §6 rulings 3, 4, 13, 14, 15, 17, 18), `CONTRACT-P5E.md` §2 and §3 (all of both),
your scout report `scout-S6.md` (all of it), and lists (b) and (c) of `kimi-audit.md`. Notes:
`//wsl.localhost/Arch/home/swung/w7/notes/p5e/`.

Your slug is `ra`; your tree is `~/w7/p5e-ra` (branch `p5e/ra`, based on the landed c0e head — NOT
on the family packages; you are parallel to them and rebase at landing). c0e landed the `WaitClass`
column, `MGPipeWaitClassFor(op)`, the `RunAhead` / `PresentCredit` knobs, `kCapRunAheadApply` (bit
10) and `kMGPipeP5eRunAheadReady = false`; id landed the `CurrentRecordBarriered` line in `ApplyOne`.

## What "done" means

The mechanism exists and is INERT until the integration commit: with `kMGPipeP5eRunAheadReady`
false the server never publishes bit 10, the client never arms run-ahead, and every gate is exactly
as green as today. Your last commit flips that constant — but only after the family packages have
landed and the integrator says so, so structure your branch that way (the flip is its own final
commit, trivially revertable).

1. **The predicate.** `MGPipeBarriered(op, payload, applierState)` (c0e declared it) computed
   identically by both roles: the static `WaitClass` column, plus XFB-active from the context values
   and the `kDrawClientArrays` flag (ruling 3 / ID-83). The server's `ApplyOne` already sets
   `CurrentRecordBarriered` from it (id's line).
2. **The client wait rule.** `EmitAndWaitTails` after `PublishAndNotify`: reply rows wait; barriered
   rows wait; unbarriered rows return. Present pacing through `WaitForPresentAck` with credit
   (default 1, range 1..8; ID-92) — the plumbing exists end to end and has no production caller
   today; `MGPPresent::FrameSerial` minted client-side; `OnPresent` returns the credit. Drains and
   forced waits per contract §2.5 (MapBuffer / GetBufferSubData / glFinish / fence client-wait /
   the EGL forwarders / the glGetError window — name the relaxation in the contract and in code).
3. **`resource_subdata`'s buffer half stops waiting** (ruling 15 / ID-93): the reply Bool is
   discarded at the call site today, so every 64 KB persistent-map block push is a hidden round
   trip. The row keeps its `kReplySlot` on the wire; the client neither waits nor reads. The texture
   half keeps its reply (trailing).
4. **`gPipeInputs` becomes server-role memory for unbarriered records** (contract §3): the client's
   residual fill and its stamp bookkeeping do not run for them (the tracker walk and the emitters
   still run — they produce records); the stamp becomes server-private; `MGPipeInputUnfreshRead`
   becomes an UNCONDITIONAL detector for unbarriered applies (a named Fatal, not a counter), which
   is what turns the strict lane into a hard gate. Barriered records keep P5C's semantics exactly
   (ID-84 / ruling 4).
5. **Event-ring flow control** (contract §2.6): the ring's overflow is a Fatal today on the premise
   that the client drains at every verb — under run-ahead that premise is gone. Make the producer
   block/flow-control instead, and keep a named refusal for the impossible case.
6. **Lifetime under run-ahead** (contract §2.7): slot reuse vs records in flight, `object_death`
   ordering, the deferred-destroy queue (ID-91 / ruling 13: it stays; an unbarriered enqueue is a
   finding — pin it), `Forget()` of a persistent map while its pushes are in SEG_STAGE.
7. **The strict lane** (`.github/workflows/test.yml`): rewritten as a hard green gate with the
   contract §7 allowlist (`<field>@<verb>` for the barriered rows that keep pulling).
8. Docs: `docs/Disaggregated/ARCHITECTURE.md` §11, §14, §17 — the wait rule, the credit, the event
   ring, `gPipeInputs` ownership.

## Red-once (execute, revert, quote)

1. Encode present without `WaitForPresentAck` → the credit-1 client behind a stalled apply thread
   publishes a third present; `Waits()` unchanged → red by count.
2. Leave one `CopyField` in an unbarriered fill → `Fatal{RoleViolation, "gPipeInputs"}` by name.
3. Count instead of Fatal under an unbarriered record → the strict lane turns red on the first
   pulled row (that is the gate's own red-once).
4. Keep the event-ring Fatal → a unit case publishing ~300 KiB of events behind an unwaited
   sequence aborts; with flow control it completes.
5. Publish bit 10 for Magma → `MagmaPipeIdentityTest` red.
6. `MOBILEGL_IPC_RUN_AHEAD=0` renders identically; `MOBILEGL_IPC_VERB_BARRIER=0` under run-ahead is
   the documented red.

## Gate (before the flip)

`build`, `unit`, `isplit`, `gens`, `strict` — all exactly as green as the base, because every branch
is inert with the bit unpublished. Your report must state, per numbered item above, what is wired
and what it does when the bit flips; the integrator runs the device exit.
