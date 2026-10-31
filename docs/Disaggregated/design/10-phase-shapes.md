# Landing shapes of each phase (formerly ARCHITECTURE §17)

> Design detail. The design points and the index of all sections are in [`../ARCHITECTURE.md`](../ARCHITECTURE.md). Section numbers follow the original `ARCHITECTURE.md`; an `ARCHITECTURE.md §N` in a code comment is found here by that number.

## 17. Phase landing shapes (index)

The shapes, deviations and rationale text for each phase's landing are in the notes; the table below lists only the rules that still apply.

| Original section | Topic | Rules that still apply | Full text |
|---|---|---|---|
| 17.1 | verb barrier and honest same-address-space transport (P5) | The same address space must not become a bypass (R-2): the encoder always writes `nullptr` for `MGHostSpan::Ptr`; replies use the record sequence number as the slot id (R-3); the 120 s wait watchdog only judges deadlock, it is not a performance gate | git history |
| 17.2 | Three backend slot classes, caps, tight readback (P5) | A answers locally / B emits / C refuses by name, never falling back to the monolith (R-4); capabilities read only from `CallMask` (R-8); `ReadPixels` is always tight on the wire, and the client scatters by its own pack state | same as above |
| 17.3 | The server role, shadow and exit (P5) | The apply thread holds the native context for its lifetime; rare EGL operations enter the apply thread through the control mailbox; staged shadow only hands over server-owned copies; bounded exit order | same as above |
| 17.4 | lane isolation (P5) | Each split entry has a private log; the E1 vs E3(a) comparison is a permanent case (`RemoteWaitBoundaryControl.AnUnwaitedPublishIsSeenAsNoWait`, `PersistentCoherentMapScenario.ThePushIsTheOnlyRouteAWriteTakesToTheServer`), the lane is not rerun | same as above |
| 17.5 | class-C verb migration (P5b) | The migration order is decided by the dynamic census; one migration = one emitter + one sink body; records carry GL call arguments verbatim, and unmeasured forms are refused by name (rule D) | git history |
| 17.6 | Shared-address-space access reduced to zero (P5c) | Server-side texture staged shadow; `SEG_EVENT` is the only reverse channel; sink / twin resolve by record handle; `applier_reset` / `object_death` control records; role guard `Fatal{RoleViolation}` | git history |
| 17.7 | run-ahead (P5e) | Rule F: applying an unbarriered record reads no client memory (a violation is a named Fatal, independent of the strict switch); `gPipeInputs` is server-role memory; `RunAheadArmed()` latches at the first caps adoption and can only be turned off, not on; `RUN_AHEAD=0` is the A/B control | git history |
