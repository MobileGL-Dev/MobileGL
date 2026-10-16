# ra — the wait rule, present credit, server-owned `gPipeInputs` (P5e)

Branch `p5e/ra`, head **48b2f56e**, based on **f6cfcbd3** (landed wave 1). Tree `~/w7/p5e-ra`.
Five commits; the mechanism is complete and **INERT** — `kMGPipeP5eRunAheadReady` is still `false`,
so `RunAheadArmed()` is false everywhere and every gate is exactly as green as the base.

## 1 Per deliverable: what is wired, and what it does when the bit flips

| # | deliverable | wired | at the flip |
|---|---|---|---|
| 1 | the predicate | c0e's `MGPipeBarriered` (all three clauses) is the server's; ra adds the CLIENT's spelling, `ClientVerbIsBarriered(verb, ctx*)` in `PipeFill.cpp` | both roles answer the same for the same record: static `WaitClass` column, XFB-active, (client arrays: vi refuses at emit) |
| 2 | the client wait rule | `EmitAndWaitTails` switches on `MGPipeWaitClassFor(op)` under `RunAheadArmed()`, **replacing** the `BatchWaits` skip; `ReleaseFillPins()` after a barriered apply; present credit in `EmitPresent`→`AcquirePresentCredit`; `OnPresent`→`ReturnPresentCredit`; forced waits before every `Server*` EGL forwarder and in `glFinish`; drains at every wait exit incl. the stage bell | `kWaitNone` rows publish and return; the steady draw path stops waiting; presents pace on a credit of 1 |
| 3 | `resource_subdata` buffer half | `MGPipeSubDataWantsItsReply(record)` — ONE predicate, read by `MGPipeRouteResourceSubData` and by `Wire_ResourceSubData`; the row keeps `kReplySlot`, the server still posts | the 64 KB persistent-map push stops being a hidden round trip per block |
| 4 | `gPipeInputs` ownership | `fillOwed = barriered` in `MGPipeValidateForVerb` gates the serial bump, `SetVerb`, `MGPipeServerClearVerbBoundary` and the whole of step 4; `MGPipeNoteFrontendMutation`/`MGPipeLeaveVerb` return; `CountBarrierPull` aborts unconditionally under an unbarriered record; the layer-2 guard is re-derived | the block is server-role memory for unbarriered records; a missed migration is a named abort, not a wrong picture |
| 5 | event-ring flow control | `ReserveEventOrBlock` (all four event kinds), `Drained()` clears **and rings**, `WaitFor{Applied,PresentAck}OrEventBacklog`, `ServerLoop`'s park gains "not while the ring is full" | a full 256 KiB ring is backpressure, not `Fatal{EventRingOverflow}`; two named refusals remain for the impossible cases |
| 6 | lifetime | the deferred-destroy queue STAYS; an enqueue from an unbarriered apply logs once and is Fatal under strict | ruling 13 (ID-91) pinned; slot reuse needs no wait (the ring orders `{s,g}` → `death` → `{s,g+1}`) |
| 7 | the strict lane | ONE step, TWO arms, and the arm is chosen by grepping `kMGPipeP5eRunAheadReady` out of `Init.cpp` | the lane becomes a hard green gate with §7's `<field>@<verb>` allowlist, in the same commit the runtime flips |
| 8 | docs | ARCHITECTURE §11.6, §11.7, §14, new §17.7, 附 A's knob table | — |

## 2 Files touched (regions, not whole files)

- `MG_Remote/Client/ClientSession.{h,cpp}` — the wait switch in `EmitAndWaitTails`; `RunAheadArmed`/`LatchRunAheadFromCaps` (latched in `PumpControlPlane`, demote-only); `ReleaseFillPins`, `WaitForApplyToCatchUp`, `Finish`, `AcquirePresentCredit`, `PresentCreditWaits`, `LastPublishedSeq`; the re-derived `RefusePipeInputsTouchWhileApplierOwnsIt(surface, isBarrieredFill)`; drains at the cmd-space wait and the stage bell; `credit-waits` on the wire ledger; the latch reset in `Stop`.
- `MG_Remote/Client/EmitTables.cpp` — `EmitPresent` mints `FrameSerial` through `AcquirePresentCredit`.
- `MG_Remote/Client/BackendObject_Remote.cpp` — `WaitForApplyBeforeEglForwarder` before the five `Server*` forwarders (NOT `ServerSwapEGLBuffers`).
- `MG_Remote/Client/WireTables.cpp` — `Wire_ResourceSubData` takes `wantReply` and posts accept-by-construction for the buffer half.
- `MG_Pipe/PipeRoute.h` — `MGPipeSubDataWantsItsReply` + the route's use of it (inside `#if MOBILEGL_PIPE_PUSH`).
- `MG_Remote/Server/PipeApplier.cpp` — `OnPresent` returns the credit and refuses `FrameSerial == 0` on a run-ahead server; `ServerPublishesRunAhead` helper; `#include "ServerSession.h"`.
- `MG_Remote/Server/ServerSession.cpp` — `ReserveEventOrBlock` replaces the four `Reserve` + Fatal sites.
- `MG_Remote/Server/ServerLoop.cpp` — the park predicate's fourth clause and the `DrainRing` gate.
- `MG_Remote/Transport/{EventRing.h,SessionRings.h,Ring.cpp}` — `SetServerDoorbell`, the ring-and-clear `Drained()`, the two `…OrEventBacklog` waits, `EventRingIsFull()`.
- `MG_Remote/Wire/PipeWireCodec.{h,cpp}` — `SetStageWaitHook`, called around the SEG_STAGE retirement park (**named delta**, see §5).
- `MG_Impl/Pipe/PipeFill.cpp` — `ClientRunsAhead`, `WireOpForVerb`, `ClientVerbIsBarriered`, `g_lastFillWasBarriered`, the `fillOwed` gate, `MGPipeReleaseResidualFillPins`, `MGPipeClientFinish`, the deferred-destroy finding, the vertex-attrib-defaults mirror read skipped under run-ahead (§3.4).
- `MG_Backend/MGPipe/PipeInputs.cpp` — `CountBarrierPull`'s unconditional arm; `StrictBarrierPullFatal` gains a reason so the lane can tell the two markers apart.
- `MG_Pipe/PipeMutation.h` (declarations + the ruling-13 comment), `MG_Pipe/PipeApply.h` (the two-constants ruling, §6).
- `MG_Impl/GLImpl/Exporting/Definitions.cpp` — `glFinish` (**named delta**, §5).
- `MG_Test/Wire/{RemoteClientTest,SessionTest}.cpp`, `.github/workflows/test.yml`, `docs/Disaggregated/ARCHITECTURE.md`.

## 3 Red-once evidence (executed, reverted, verbatim)

**1. Present without the credit wait** (`if (m_presentsSent >= credit)` → `if (false)`),
`RemoteRunAhead.ACreditOneClientPaysTheCreditAtEveryPresentAfterTheFirst`:
```
0% tests passed, 1 tests failed out of 1
../MobileGL/MG_Test/Wire/RemoteClientControls.inc:143: Failure
  Actual: false
exited 103
```
(103 = `PresentCreditWaits() != 2` — red by count, which is the only way to test a wait that is
usually already satisfied.)

**2. Leave the fill in for an unbarriered record** (`const Bool fillOwed = barriered;` → `= true`),
`RemoteRunAhead.AnUnbarrieredVerbDoesNotFillPipeInputsAndIsAllowed`:
```
signal 6
[08:09:45] [Linux RemoteClientTes/FATAL]: MGPipe: Fatal{RoleViolation, "gPipeInputs"} - the GL thread touched gPipeInputs (MGPipeValidateForVerb) on a RUN-AHEAD session outside a barriered fill. With the client running a
```

**3. Count instead of Fatal under an unbarriered record** (drop `CountBarrierPull`'s first
clause), `RemoteGuards.AResidualPullUnderAnUnbarrieredRecordIsFatalWithoutTheStrictKnob`:
```
0% tests passed, 1 tests failed out of 1
../MobileGL/MG_Test/Wire/RemoteClientControls.inc:139: Failure
  Actual: false
exited 0
```
(the probe stops aborting; on the lane this is every still-pulling scenario going from a named
abort to a wrong picture with a number beside it.)

**4. Keep `Fatal{EventRingOverflow}`** (`if (!ServerPublishesRunAhead(session)) Fatal…` → always),
`RemoteRunAhead.AnEventBurstBehindAnUnwaitedSequenceFlowControlsInsteadOfAborting`:
```
signal 6
[08:11:31] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{EventRingOverflow} - the producer of kEventGpuWritten could not reserve 1040 bytes on SEG_EVENT. P5c's events are lossless and a full ring under lockstep is a produce
```

**4b. The drain stops ringing the server** (`Drained()`'s `NotifyIfParked` → `(void)wasFull;`),
`SessionTest.DrainingAFullEventRingClearsTheLatchAndRingsTheServer`:
```
../MobileGL/MG_Test/Wire/SessionTest.cpp:661: Failure
    Which is: 0
    Which is: 1
```

**4c. The applied park stops breaking on the ring** (drop the `eventRingFull` clause),
`SessionTest.AFullEventRingWakesAClientParkedOnAppliedSeq`:
```
../MobileGL/MG_Test/Wire/SessionTest.cpp:586: Failure
    Which is: 4-byte object <02-00 00-00>     (TimedOut)
    Which is: 4-byte object <00-00 00-00>     (Reached)
```

**5. Bit 10 for Magma** — c0e already ran and recorded this
(`MagmaPipeIdentityTest.AMagmaServerNeverPublishesTheRunAheadCapBit` fails "Which is: 1024 / 0").
Re-run here green: `MagmaPipeIdentity` 5/5.

**6. NOT RUN, and why.** `MOBILEGL_IPC_RUN_AHEAD=0` renders identically is TRIVIALLY true on this
tree — the caps bit is unpublished, so both arms are the same code path — and the SSIM arm needs
the device. `MOBILEGL_IPC_VERB_BARRIER=0` under run-ahead is likewise unreachable before the flip.
Both belong to the integrator's device exit (BRIEF §4).

## 4 Gate (this tree vs base f6cfcbd3, measured both)

| step | base f6cfcbd3 | p5e/ra 48b2f56e |
|---|---|---|
| build | rc=0 | rc=0 |
| unit | 2206/2206 | **2222/2222** (+16 cases, all new) |
| `integration-split` | 111/111 | 111/111 |
| `integration-split-strict` | 3% passed, 108 failed of 111 | **identical** |
| gens | 7 rc=0 + the pre-existing `CONTRACT-P5.md:524 Core.cpp:39` ambiguity | identical |

Strict marker table, from the 88 per-entry private logs, **before and after are the same table**
(measured by checking out f6cfcbd3, rebuilding and re-running):

| marker | count (base) | count (ra) | owner |
|---|---|---|---|
| `GetFramebufferBindingSlot@Clear` | 39 | 39 | fb |
| `GetBoundVertexArray@DrawArrays` | 14 | 14 | vi |
| `GetImageTextureBinding@BindImageTexture` | 13 | 13 | fb |
| `GetTextureUnitObject@Clear` | 9 | 9 | tx2 |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | P5b debt (allowlisted) |
| `GetProgramForDispatch@DispatchCompute` | 3 | 3 | pg |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | pg (allowlisted) |

**G1: 0 delta.** Every edit to shared code is under `MOBILEGL_BUILD_DISAGGREGATED` or
`MOBILEGL_PIPE_PUSH` (`PipeRoute.h`'s route is at line 270, inside the file's push guard).

## 5 Kimi audit cross-check (my family is list (b), not an object family)

- **RETIRED:** `PI:807, PI:812-814` — the four O-class `SharedPtr` rows (`m_boundVertexArray`,
  the three programs). `ReleaseFillPins` drops them after every barriered apply, and an
  unbarriered verb never re-fills them, so the apply thread can no longer become a frontend
  object's last owner through this block.
- **LEFT BARRIERED:** `PI:808-811, PI:815` — the raw pointer bases into the frontend tables.
  They are written only by a barriered fill; an unbarriered apply that reads through them takes
  §3.3's abort by field and verb. Their retirement is the family packages' (list (c)).
- **NOT FOUND / not mine:** every row of list (c) is an `MGB_CTX->` accessor whose retirement
  belongs to vi/sb/pg/tx2/fb. ra changes not who reads them but what a read costs.

## 6 Rulings I need from the integrator

1. **`kMGPipeP5eClientWaitRuleLanded` flips with `kMGPipeP5eRunAheadReady`, not when ra lands.**
   ID-103 says "ra flips it in the same commit that lands the wait rule". That cannot be right:
   `RunAheadArmed()` needs the server's caps bit, so between ra landing and the bit being
   published the client still blocks after every record — and a `false` stamp there withdraws
   §4.4's exemptions from probes that wait still makes safe, which is the "refusal with no defect
   behind it" ID-103 itself refused. I left BOTH constants `false` and amended the comment at the
   constant. **The integration commit throws both.** (Had I flipped it, `integration-split` would
   be near-totally red on this tree: the family packages have not landed.)
2. **§3.2's "the stamp moves to applier-owned storage" is implemented as an OWNERSHIP rule, not
   as moved storage.** Physically moving `m_filled`/`m_currentVerb`/`m_serverStampedVerb` out of
   `gPipeInputs` means re-pointing the freshness check in all 63 generated accessors
   (`generated/PipeFilled.inc`), which is c0e's file and a much larger blast radius. What ra
   landed is the rule the move existed for: under run-ahead the GL thread writes those fields
   only inside a barriered fill, so the E-note hazard (b.4) — a GL thread withdrawing the
   server's stamp while an apply depends on it — cannot occur. Confirm, or schedule the move.
3. **`SetIdentity` is NOT skipped** (§3.1 lists it). `m_live`/`m_contextIdentity` are not fields
   of the residual model — no ownership row owns them, no record carries them, no server stamp
   can answer them — they are "does this process still have a GL context", which every
   null-context guard reads and only the GL thread knows. Two stores per verb, no stamp.
4. **The §7 allowlist is my best reading, not a measurement.** It cannot be measured before the
   flip (every entry still aborts on an unmigrated row). The integrator should re-derive it from
   the first green run and delete rows rather than widen them.

## 7 Seams assumed from other packages

- **vi:** a draw with a client-memory array is refused at emit under run-ahead (`ruling 2` /
  §5.1). `ClientVerbIsBarriered` therefore does NOT re-compute `kDrawClientArrays` — duplicating
  vi's "does any enabled attribute lack a buffer" walk is exactly the second spelling ruling 3
  replaced. If vi's refusal does not land, a client-array draw would be judged unbarriered and
  its fill skipped. Named in the code comment.
- **tx2:** `GenerateMipmap` stays `kWaitApplied` until the `VerbMipRes` arm lands; the wait rule
  reads the column, so tx2 flipping the row is all that is needed.
- **pg/sb/fb/tx2:** the strict allowlist's rows are theirs to delete.
- **c0e (landed):** `MGPipeWaitClassFor`, `kCapRunAheadApply`, `MGPipeRunAheadCapBitsFor`,
  `IpcTable::RunAhead/PresentCredit`, `MGPipeBarriered`, the barriered-record flag + setter.
- **id (landed):** `ApplyOne`'s stamp line — untouched by ra, as the brief assigns.

## 8 Named deltas and what I did not do

- **`PipeWireCodec.{h,cpp}` (not in my scope list):** the SEG_STAGE retirement park is a CLIENT
  wait and the deadlock argument needs every client wait to drain. The encoder cannot drain
  SEG_EVENT itself, so it gained a `StageWaitHook` the session installs. Null in every standalone
  codec; the park is otherwise byte-identical.
- **`MG_Impl/GLImpl/Exporting/Definitions.cpp` (not in my scope list):** `glFinish` was a pure
  no-op. Under run-ahead "the commands issued so far have completed" is a promise the queue can
  break, and this is the only place an application can ask for it. Two lines, under
  `#if MOBILEGL_BUILD_DISAGGREGATED`, calling `MG_Pipe::MGPipeClientFinish`.
- **The server half of the present credit is not unit-pinned.** The `RemoteClient` fixture has no
  backend object, so `ServerVerbSink::OnPresent` declines before it can reach
  `ReturnPresentCredit`; the cases substitute a peer sink and assert the CLIENT's half. That the
  server returns exactly one credit per swap after `Present()` returns is three lines in
  `PipeApplier::OnPresent` and the device exit's `credit-waits` reading.
- **No device numbers.** The measurement arms of BRIEF §4 (fps, per-thread ms/frame, simpleperf,
  SSIM) are the integrator's; no adb from here.
- **`MOBILEGL_IPC_EVENT_KB`** stays a trailing item: flow control makes 256 KiB correct rather
  than fast, and the ring size is a measurement, not a correctness input.
- **G2/G14 skip twins:** the new public names are all in split-only translation units
  (`MG_Remote/*`, the push-only half of `PipeFill.cpp`) and the new cases live in
  `MG_Test/Wire/*`, which is a split-only binary. No monolith twin is possible or needed; unit
  count moves 2206 → 2222 on the split build only.
