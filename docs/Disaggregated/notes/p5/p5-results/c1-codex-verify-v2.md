# c1 cross-family review, round 2 — VERIFICATION

Executes the ten findings of `p5-results/c1-codex-review-v2.md` that are c1's — **2, 4, 5, 6, 7, 8,
9, 10, 11, 12**. Findings 1 and 3 are v1's open items (ID-55 item 12, ID-56 blockers 1 and 3) and
were not assigned here; §D records where they turned up anyway.

**Worktree:** `~/w7/p5-verify`, branch `p5/verify`, detached at **`41992649`** (`p5/c1`'s head),
`build-split`, ccache, `-j 14`. `~/w7/p5-verify-submodlinks.sh off` before every git command and
`on` before every build. `build-split/CTestTestfile.cmake` present (580 bytes, 3174 entries).
**Nothing was fixed and nothing was committed.** Every perturbation is a local, uncommitted edit,
reverted with the driver's own saved-text restore, and `git status --short` was verified empty
between findings — printed as `(clean)` in every log below. Final state: clean, detached at
`41992649`, init patch reverted.

**The init patch.** Every integration entry under `inproc` needs c1's documented
`MG_Backend/Init.cpp` direct-construction one-liner (`~/w7/p5-c1-r2-redcheck.py`, `init_patch`),
reproduced verbatim in `~/w7/p5-verify3-initpatch.py` / `p5-verify3-drive.py:init_patch`. **It was
applied as a local, uncommitted edit for every integration run in this document, and reverted
after each one.** Runs marked "unit" do not use it.

**Driver and artefacts.** `~/w7/p5-verify3-drive.py` (apply → build → run → revert → `git status`),
`~/w7/p5-verify3-probes.py` (the temporary read-only probes), per-finding scripts
`~/w7/p5-verify3-f*.py`, logs `~/w7/p5-verify3-*.log`. Temp files all carry the `p5-verify3-`
prefix. No GitHub LFS fetch, no adb device, no other `~/w7/p5-*` worktree touched.

---

## The unperturbed baselines, green first

**Unit lane** — `cd ~/w7/p5-verify/build-split && ctest -L unit -j 14`
(`~/w7/p5-verify3-baseline-unit.log`):

```
100% tests passed, 0 tests failed out of 2025
UNIT_RC=0
```

**Integration-split, with the init patch** — `ctest -L integration-split -j 8`
(`~/w7/p5-verify3-isplit-inproc.log`):

```
67% tests passed, 7 tests failed out of 21
The following tests FAILED:
	3161 - DirectGLES.Split.PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw (Subprocess aborted)
	3162 - DirectGLES.Split.PersistentCoherentMapScenario.AWriteAfterAFrameBoundaryReachesTheNextFramesDraw (Subprocess aborted)
	3163 - DirectGLES.Split.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares (Subprocess aborted)
	3164 - DirectGLES.Split.PersistentMapArm.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares (Subprocess aborted)
	3172 - DirectGLES.Split.SmallRing.PersistentCoherentMapScenario.TwoWritesThroughTheCoherentPointerEachReachTheirOwnDraw (Subprocess aborted)
	3173 - DirectGLES.Split.SmallRing.PersistentCoherentMapScenario.AWriteAfterAFrameBoundaryReachesTheNextFramesDraw (Subprocess aborted)
	3174 - DirectGLES.Split.SmallRing.PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares (Subprocess aborted)
```

**c1-v2.md §6's 14 / 21 reproduces exactly**, and the seven are exactly the seven it names. Probed
with `MOBILEGL_LOG_FILE_PATH`, entry 3162's own diagnostic is c1-v2.md §6's, verbatim:

```
[Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{ProtocolCorruption} resource_flush_range {slot=1, gen=0, glName=1}: a non-empty flush carries no bytes (offset=0, size=120, storage=120 bytes)
```

---

## Finding 2 — a pack-PBO offset is used as a client CPU destination — **CONFIRMED (blocker)**

ID-57 already rules the P5 behaviour (refuse by name). The job here was to confirm that **this head
dereferences the offset**. It does, and the proof is a SIGSEGV rather than an inference.

**Source.** `EmitReadPixels` has no PBO branch at all. `MobileGL/MG_Remote/Client/EmitTables.cpp`:

```
291:            info.DstOffset = 0;
292:            info.DstSize = tight;
322:                                    pixels, tight, &status);      // the tight fast path
331:                                bounce.data(), tight, &status);   // the bounce path
```

`info.DstOffset` is the literal `0` on every path, so the server never learns the PBO offset; the
GL `pixels` argument is handed to `ClientSession::EmitAndWait` as `replyOut`, and `EmitAndWait`
memcpys the reply into it. The frontend permits the offset: `GL_Framebuffer.cpp:3055-3078` checks
only that the PBO is not non-persistently mapped and that `pixels` is aligned to
`GetTexturePixelDataTypeSize(type)` — **1 byte for `GL_UNSIGNED_BYTE`**, so offset 16 passes.

**Command.** `python3 ~/w7/p5-verify3-f2.py` and the monolith arm in `~/w7/p5-verify3-f6-f4c.py`.
The probe (temporary `TEST_F` in the Split lane's `ClearThenReadPixelsScenario`, `P.F2_CASE`):
clear to (0.25, 0.5, 0.75, 1.0), bind a 64-byte `GL_PIXEL_PACK_BUFFER`, then
`glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void*)16)`.

**Unperturbed control** — the scaffolding alone, existing case, inproc + init patch:

```
--- run --gtest_filter=ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels  => rc=0 ---
[  PASSED  ] 1 test.
```

**Split (inproc), the offset-16 read — `rc = -11` (SIGSEGV):**

```
--- run --gtest_filter=ClearThenReadPixelsScenario.VerifyF2PackPboOffsetIsNotACpuDestination  => rc=-11 ---
[ RUN      ] ClearThenReadPixelsScenario.VerifyF2PackPboOffsetIsNotACpuDestination
F2: probeLive=1
F2: 64-byte GL_PIXEL_PACK_BUFFER bound, glError=0
F2: about to glReadPixels(0,0,1,1,RGBA,UNSIGNED_BYTE, (void*)16)
```

The process never reaches the next line (`F2: RETURNED from glReadPixels`). The reply's four bytes
were written to CPU address 16. **No `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}` and no other
named refusal** — ID-57's required diagnostic does not exist on this head.

**Monolith control, the identical call** (`MOBILEGL_TRANSPORT=monolith`, no `MGITEST_SPLIT_LANE`):

```
--- run --gtest_filter=ClearThenReadPixelsScenario.VerifyF2PackPboOffsetIsNotACpuDestination  => rc=0 ---
[       OK ] ClearThenReadPixelsScenario.VerifyF2PackPboOffsetIsNotACpuDestination (2 ms)
F2: probeLive=0
F2: RETURNED from glReadPixels; glError=0
F2: PBO[16..19] = 40 80 bf ff (0xEE = untouched)
```

`40 80 bf ff` is the cleared colour (64, 128, 191, 255) at PBO offset 16. So the monolith arm is
correct today and the split arm dereferences the offset — precisely ID-57's premise.

**Minimal fix shape.** In `EmitReadPixels`, before `RequireReadPixelsReplyFits` and before any use
of `pixels`: if `MG_State::pGLContext` reports a bound `BufferTarget::PixelPack`, take R-4's class-C
refusal — `Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}` — under `MG_Config::Transport !=
Monolith` only, so the monolith path is byte-identical (G2/G14).
**Red-once control.** ID-57's own: a bound 64-byte pack PBO and the offset-16 read under inproc must
die with that string (assert it from a private `MOBILEGL_LOG_FILE_PATH`, see finding 7), and the
same call under monolith must still read back `40 80 bf ff` at offset 16 through
`glGetBufferSubData`. Delete the refusal and the inproc half must SIGSEGV again.

---

## Finding 4 — `Stop` installs driver-reaching adapters while the transport is still active — **CONFIRMED (major)**

**Source.** `MobileGL/MG_Remote/Client/WireTables.cpp:487-493`:

```cpp
    void UninstallClientWireTables() {
        // PUTS THE MONOLITH ARM BACK rather than nulling the rows. ...
        MG_Pipe::MGPipeInstallMonolithTables();
    }
```

No transport, role or session guard of any kind. `ClientSession::Stop()` calls it as its **first**
statement, before `m_started`, `g_active`, the server, the applier, the rings or the transports go
away.

**Command.** `python3 ~/w7/p5-verify3-f4-f9b-f12.py` and `~/w7/p5-verify3-f6-f4c.py` (the
`F4c` variant), inproc + init patch, probe `P.F4C_CASE`.

**First attempt** (`glBufferData` *with* data) aborted at `rc = -6`, and the diagnostic — readable
only because the probe set `MOBILEGL_LOG_FILE_PATH` — was **not** a refusal of the forbidden path:

```
[Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{InitialBytesNotCarried, "resource_respecify"} - the respecify crossed with initialBytes = nullptr (R-13.3) and the resource_subdata records that were supposed to follow it emitted NOTHING, so 16 bytes of initial content exist on no side of the wire
```

That is c1's own **content** self-check firing by accident: the split-only follow-up walk still runs
(it is gated on `Transport != Monolith`, not on which table is installed), its `ResourceSubData`
records now go to the monolith adapter, so `ClientWireRecordsEmitted()` does not move. It fires
**only** for a respecify that carries initial bytes.

**The decisive run** uses mutations that carry none (`glBufferData(..., nullptr, ...)` then
`glDeleteBuffers`), so the numbers can be read:

```
--- run --gtest_filter=ClearThenReadPixelsScenario.VerifyF4cUninstallThenAMutationWithNoInitialBytes  => rc=0 ---
[       OK ] ClearThenReadPixelsScenario.VerifyF4cUninstallThenAMutationWithNoInitialBytes (0 ms)
[  PASSED  ] 1 test.
F4c: BEFORE uninstall wireRecords=15 emitSeq=15
F4c: Uninstall returned; sessionActive=1
F4c: AFTER  uninstall wireRecords=15 emitSeq=15
F4c: VERDICT wireOrdinalMoved=0 emitSeqMoved=0 survivedWithNoRefusal=1
```

`sessionActive=1` — the session, the server and its applier are all still live. The wire ordinal
**did not move**; `MGPipeApply*` ran on the calling GL thread; the test **passed**. That is exactly
the forbidden path, taken silently. Finding 9's part 2 (below) is the same effect from the other
direction: a `glDeleteBuffers` through the monolith adapter, delta 0, everything green.

**The failed-`Start` → `Stop` half — CONFIRMED at source.** `ClientSession::Stop()` (`:578-586`)
calls `UninstallClientWireTables()` **before** `if (!m_started) { ... }`, so every one of Start's
failure paths leaves the process with monolith adapters installed while `MG_Config::Transport` is
still `inproc`. The resulting table state is byte-for-byte the state F4c measured, and the
split-only follow-up walks keep running against it. No separate runtime probe was built: forcing a
Start failure needs a transport fault injector this head does not have.

**Minimal fix shape.** `UninstallClientWireTables()` should install a **teardown** table, not the
monolith one: every row a named `Fatal{SessionTornDown, "<row>"}` (R-4's class-C shape) under
`Transport != Monolith`, and `MGPipeInstallMonolithTables()` only when the transport really is
monolith. That keeps the "not a null row" property c1 wanted while refusing rather than executing.
**Red-once control.** F4c's own probe, asserted: after `UninstallClientWireTables()` on a live
session, one routed resource mutation must die naming `SessionTornDown`; deleting the guard must
turn it back into `wireOrdinalMoved=0 … PASSED`.

---

## Finding 5 — the two acceptance escapes convert ERROR into an ordinary decline — **CONFIRMED (major)**

**Source.** The two escapes read `status` straight out of `EmitAndWait` and never mint or take a
reply slot. `MobileGL/MG_Remote/Client/WireTables.cpp`:

```cpp
// Wire_Escape_ResourceRespecify
            if (status == 1) ++g_declined;
            return status == 0;                 // Status 2 (ERROR) -> false, same as DECLINED

// Wire_Escape_MapPersistent
            if (status == 0) { MGLOG_F("... Fatal{UnexpectedMapAccept ...}"); std::abort(); }
            return nullptr;                     // Status 2 (ERROR) -> nullptr, same as DECLINED
```

`EmitAndWait` itself only *logs* ERROR (`MGLOG_E("... answered ERROR")`, `ClientSession.cpp:773`)
and returns. `Fatal{ReplyError}` exists in exactly two production places, both in the **mailbox**
the *generated* rows use — `MG_Pipe/PipeRoute.cpp:137` (`MGPipeTakeReplyBool`) and `:151`
(`MGPipeTakeReplyPointer`) — and the escapes reach neither.

**Command.** `python3 ~/w7/p5-verify3-f5-f10b-f11.py`, inproc + init patch, each perturbation
posting **the correct sequence** with `ReplySink::kStatusError`.

**(a) `ResourceRespecify` answered ERROR** — `PipeWireCodec.cpp`, the respecify case, `PostReply(op,
seq, ReplySink::kStatusError, nullptr, 0)` in place of `noteAcceptance(...)`:

```
--- run --gtest_filter=TriangleScenario.AVboBackedTriangleReachesReadPixels  => rc=0 ---
[  PASSED  ] 1 test.
---- F5a ResourceRespecify=ERROR : Fatal/ERROR lines ----
   (NO 'Fatal{' line at all)
   ReplyError present: False
```

**(b) `MapPersistent` answered ERROR** — same file, `kStatusDeclined` → `kStatusError`:

```
--- run --gtest_filter=PersistentCoherentMapScenario.TheMapLandsInTheArmItsLaneDeclares  => rc=-6 ---
   [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{ProtocolCorruption} resource_flush_range {slot=1, gen=0, glName=1}: a non-empty flush carries no bytes (offset=0, size=120, storage=120 bytes)
   ReplyError present: False
```

The abort is the pre-existing flush seam, byte-identical to the unperturbed baseline above. ERROR
produced **no observable difference at all** from DECLINED.

**(c) the comparison the claim makes — a GENERATED acceptance row answered ERROR** (`noteAcceptance`
posts `kStatusError`):

```
--- run --gtest_filter=TriangleScenario.AVboBackedTriangleReachesReadPixels  => rc=1 ---
[  FAILED  ] TriangleScenario.AVboBackedTriangleReachesReadPixels
   [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{ReplyError, "resource_create"} - the row answered ERROR, which is not an acceptance answer; folding it into accepted or refused would make a transport fault look like a resource decision
   ReplyError present: True
```

So the review's claim is exact: the generated wrappers abort, the two escapes decline.

**Minimal fix shape.** Give both escapes the mailbox's rule rather than a copy of it: after
`EmitAndWait`, `if (status == 2) { MGLOG_F("MGPipe: Fatal{ReplyError, \"resource_respecify\"} …");
std::abort(); }` (and `"map_persistent"`). Better still, route them through
`MGPipeTakeReplyBool`/`MGPipeTakeReplyPointer` by minting a slot and posting the wire status into
it, the way `Wire_ResourceCreate` already does — one mechanism, not two.
**Red-once control.** Perturbations (a) and (b) as executed, each asserting
`Fatal{ReplyError, "<row>"}` from a private log file; deleting the new check must make the Triangle
entry pass again (a) and leave (b) indistinguishable from the flush-seam abort.

---

## Finding 6 — `ReadPixels` incorrectly applies `PACK_SKIP_IMAGES` — **CONFIRMED (major)**

**Source.** `EmitTables.cpp:595-621`, and the comment states the behaviour as deliberate:

```cpp
        // SKIP_IMAGES is in the parameter set and is meaningless for a 2D read, so it is
        // applied as GL defines it (whole images of ROW_LENGTH x IMAGE_HEIGHT) rather than
        // ignored ...
        auto* out = static_cast<Uint8*>(destination) +
                    static_cast<Uint64>(pack.SkipImages) * imageRows * strideBytes + ...
```

and `ReadbackPackStateIsTight` (`:97-104`) rejects the fast path on `pack.SkipImages != 0`. The
monolith conversion path is the opposite: `DirectGLES.cpp:10904` passes
`/*honorPackImageParams=*/false`.

**Command.** `python3 ~/w7/p5-verify3-f6-f4c.py` (unit; no init patch). The review's exact probe —
`SkipImages=1` on an otherwise neutral pack state, 4×3 RGBA8, 96-byte sentinel destination — added
as a temporary case in `MG_Test/Wire/RemoteClientTest.cpp` driving the exported
`ScatterTightReadbackIntoPackState` and `ReadbackPackStateIsTightForTest`, i.e. the functions the
emitter itself calls.

**Unperturbed:** `ctest`-equivalent `RemoteClientTest --gtest_filter=RemoteReadback.*` → `rc=0`,
`[  PASSED  ] 4 tests.`

**With the case added:**

```
--- run --gtest_filter=*VerifyF6SkipImagesOnATwoDimensionalRead*  => rc=1 ---
F6: ReadbackPackStateIsTight = 0 (0 => the SCATTER runs)
F6: destination bytes touched = 48, first = 48, last = 95
[  FAILED  ] RemoteReadback.VerifyF6SkipImagesOnATwoDimensionalRead
```

**Bytes 48–95 receive the reply; bytes 0–47 keep their sentinel** — exactly the claim. An
application that supplied the 48-byte destination GL requires is overrun by 48 bytes. Re-run over
the whole suite: `RemoteReadback.*` → `4 PASSED`, this one `FAILED` — the four existing cases are
blind to it.

**Minimal fix shape.** Drop `SkipImages`/`ImageHeight` from the scatter's destination arithmetic and
from `ReadbackPackStateIsTight`'s rejection list — one term in `out` and one clause in the
predicate — matching `honorPackImageParams=false`.
**Red-once control.** The case above as written (require bytes 0–47 to hold the reply and 48–95 the
sentinel); putting the `SkipImages` term back must turn it red, and
`TheFastPathIsTakenExactlyWhenTheScatterWouldChangeNothing` must be extended to drive the predicate
with `SkipImages=1` so the two halves stay coupled.

---

## Finding 7 — R-1's assertion is inert on this head and checks too late even after the bracket — **CONFIRMED (major)**

### (i) Inert on this head

```
$ cd ~/w7/p5-verify && git grep -nE 'ScopedApplierEntry|NoteApplyThreadEnteredApplier|ApplyThreadIsInsideApplier' 41992649 -- MobileGL
41992649:MobileGL/MG_Remote/CONTRACT-P5.md:342: ... (the contract's prose)
41992649:MobileGL/MG_Remote/Client/ClientSession.cpp:692:        if (ApplyThreadIsInsideApplier()) {
41992649:MobileGL/MG_Remote/Client/ClientSession.cpp:801:    Bool ClientSession::ApplyThreadIsInsideApplier() {
41992649:MobileGL/MG_Remote/Client/ClientSession.cpp:804:    void ClientSession::NoteApplyThreadEnteredApplier() {
41992649:MobileGL/MG_Remote/Client/ClientSession.h:114:        static Bool ApplyThreadIsInsideApplier();
41992649:MobileGL/MG_Remote/Client/ClientSession.h:123-130: (the declarations and ScopedApplierEntry itself)
```

Every hit is a **declaration, a definition, or the single consumer**. `ScopedApplierEntry` has no
use anywhere outside its own definition, and **no production applier is bracketed**.
`PipeApplier::ApplyOne` (`:295-325`) calls `StampVerbBoundary` and `LeaveApplier()`, and
`LeaveApplier()` is `MG_Pipe::MGPipeServerClearVerbBoundary()` (`:342`) — p1's *verb-boundary* flag,
a different flag from `g_applyThreadInsideApplier`. So `ApplyThreadIsInsideApplier()` is permanently
`false` and the check at `:692` never fires.

The check itself works — it is only never armed. Raising the flag by hand on the head as committed:

```
--- run --gtest_filter=...VerifyF7aTheFlagIsUpAtEmission  => rc=-6 ---
F7a: flag before = 0
F7a: flag raised = 1; now emitting a verb...
   [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "ResourceRespecify"} - the apply thread is inside the applier while the GL thread is emitting. ...
```

### (ii) The placement claim, with the bracket applied

The bracket hunk from `~/w7/p5-v1-joint-seam.diff` (the `PipeApplier.cpp` `#include` plus
`const Client::ClientSession::ScopedApplierEntry insideApplier;` at the top of `ApplyOne`) was
applied locally. Command: `python3 ~/w7/p5-verify3-f7b.py`.

With the bracket in place and the flag **up at emission**, the check fires as before
(`Fatal{BarrierViolation, "ResourceRespecify"}`, `rc=-6`). With the flag **up during the
residual-fill window and down again before emission** — the exact ordering the review describes,
made deterministic:

```
--- run --gtest_filter=...VerifyF7bTheFlagWasUpDuringTheFillAndDownAtEmission  => rc=0 ---
F7b: flag UP during the residual-fill window = 1
F7b: flag DOWN before emission = 0; now emitting...
F7b: the emission-time check did NOT fire - a fill that raced the applier and finished before EmitAndWait is unobserved
[  PASSED  ] 1 test.
---- F7 (iii) bracket + flag down at emission : Fatal lines ----
   (NO 'Fatal{' line at all)
```

`MGP_FILL(ReadPixels)` is at `MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp:3097`, one line before
the table call at `:3098`, so a real racing fill lands inside exactly that window. The only check is
at `ClientSession.cpp:692`, inside `EmitAndWait`, which the emitter reaches afterwards.

**PARTIAL on one sub-point:** no two-thread harness was built that blocks a *real* applier mid-apply
while the GL thread fills. The raise/lower sandwich reproduces the ordering deterministically but
with the flag driven by the test, not by `PipeApplier::ApplyOne`. Everything else is CONFIRMED.

**(iii) The bracket is safe to add** (R-17.3): with it applied, the lane stays green —
`ClearThenReadPixelsScenario.*` → `5 PASSED`, `TriangleScenario.*` → `2 PASSED`, no `Fatal{`.

**(iv) The log-destination sub-claim — CONFIRMED.** `MGL_ITEST_GLES_SPLIT_ENVIRONMENT`
(`MG_IntegrationTest/CMakeLists.txt:1854-1857`, used by all three `DirectGLES.Split.`
`gtest_discover_tests` blocks at `:1914`, `:1923`, `:1932`) carries `MOBILEGL_BACKEND_TYPE`,
`MOBILEGL_TRANSPORT`, `MOBILEGL_IPC_SERVER_PATH`, `MGITEST_SPLIT_LANE` and the capability/common
env — and **no `MOBILEGL_LOG_FILE_PATH`**, although fourteen other lanes in the same file have one.
Observed throughout this verification: every split-lane abort printed **nothing** to stderr; every
`Fatal{...}` quoted in this document was readable only because the probe set the variable itself.
ID-53 ruling (1) has not landed for these lanes.

**Minimal fix shape.** (a) Land the `ScopedApplierEntry` bracket (v1's file, the integrator's at the
merge). (b) Move the check to the residual write: an `MGP_FILL`-side assertion — either inside
`MGPipeInputsBeginFill` or a `ClientSession::RequireApplierNotInside("<verb>")` called from
`MGP_FILL` — so the flag is read where `gPipeInputs` is actually written, keeping the `EmitAndWait`
check as a second line. (c) Give every `DirectGLES.Split.*` entry its own `MOBILEGL_LOG_FILE_PATH`
per ID-53.
**Red-once control.** Delete the bracket → the arming case must stop firing; keep the bracket but
move the check back to emission only → the fill-window case must go from red to green. Both are
executable with the raise/lower probe above.

---

## Finding 8 — "20 / 20 RED" does not establish twenty failures for their own reasons — **CONFIRMED (major)**

**Commands.** `python3 ~/w7/p5-verify3-f8-patch.py && python3 ~/w7/p5-verify3-f8-redcheck-copy.py`
— a COPY of `~/w7/p5-c1-r2-redcheck.py` with `ROOT` pointed at `~/w7/p5-verify` and **`run()`
returning `("UNRELATED_FAILURE", 1)` for every case**, exactly as proposed. `build()` was also
stubbed to `(True, "")` so that no case could be scored red through the `BUILD-BREAK (also a red)`
arm instead of through the verdict predicate; the review itself notes no project build is needed.
The edit/restore machinery ran for real and the worktree was verified clean afterwards.

```
preflight: all five targets build clean before any control runs
RED   R1 the install really fills the escape table
RED   R2 the install is reachable from a header, not an orphan .o
RED   R3 a missing answer Fatals rather than defaulting
RED   R4 a missing answer is not silently false
RED   R5 two outstanding answers are a barrier fault
RED   R6 ERROR is not folded into an acceptance answer
RED   R7 DECLINED is false and is COUNTED
RED   R8 DECLINED is not folded into accepted
RED   ID47-a the boundary is the pool's cap, not 'any pool at all'
RED   ID47-b the emitter really asks the helper, with its own number
RED   ID49-a DstSize is the TIGHT extent, not the packed one
RED   ID49-b the scatter honours SKIP_ROWS / SKIP_PIXELS
RED   ID49-c the scatter writes the ROW, never the whole stride
RED   ID49-d the neutral-pack fast path really equals the scatter
RED   ID31 the acceptance answer really rides the slot
RED   ID31-b the slot's status agrees with the applier's answer
RED   R8 the liveness gate reads the mirror, not the op table
RED   R17 the role gate keeps the apply thread off the wire
RED   v1-seam m_started is set before g_active
RED   R13.3 the respecify's bytes really follow as sub-data
--- restoring
CONTROLS_THAT_DID_NOT_GO_RED=none
P5_C1_R2_REDCHECK_DONE
SCRIPT_EXIT_CODE=0
```

**All twenty controls scored RED on a stubbed runner that never ran anything**, and the harness
reported its headline result, `CONTROLS_THAT_DID_NOT_GO_RED=none`. The predicate is
`red = rc != 0 or "[  FAILED  ]" in out or "PASSED  ] 0 tests" in out`, plus `BUILD-BREAK` and
`TimeoutExpired` — no case asserts its own diagnostic.

**Second half — "does the script exit non-zero when `failures` is non-empty?" No.**
`python3 ~/w7/p5-verify3-f8b-patch.py && python3 ~/w7/p5-verify3-f8b-redcheck-copy.py`, with
`run()` returning `("[  PASSED  ] 20 tests", 0)` so every case scores STILL GREEN:

```
CONTROLS_THAT_DID_NOT_GO_RED=['R1 the install really fills the escape table', 'R2 ...', ... all twenty ...]
P5_C1_R2_REDCHECK_DONE
SCRIPT_EXIT_CODE=0
```

Twenty listed failures and **exit code 0**. There is no `sys.exit` after the final `print`. The
restore loop also excludes `ITEST` (`for target, binary in {PCT, RCT, PWC, SAN}`) and only prints
its results.

**Minimal fix shape.** Add a per-case `expect` string to the `CASES` tuples and make the verdict
`red = (rc != 0 or "[  FAILED  ]" in out) and expect in out`; drop the bare-`rc != 0` and
`BUILD-BREAK` arms from "red" (a build break is a harness failure, not a control going red); include
`ITEST` in the restore loop; and end with `sys.exit(1 if failures else 0)`.
**Red-once control.** The two runs above, kept as the harness's own self-test: stub `run()` to an
unrelated failure and the script must report every case as NOT red and exit non-zero.

---

## Finding 9 — the 33+4 catalogue test cannot detect one missing client-wire installation — **CONFIRMED (major)**

**Source.** `MG_Test/Pipe/PipeCatalogueTest.cpp:150-196` calls `MGPipeInstallMonolithTables()`
itself and counts `gMGPipeScreen` / `gMGPipeContext` / `gMGPipeRouteEscapes`. It never calls
`InstallClientWireTables()`, so nothing in it can observe that table at all.

**Command.** `python3 ~/w7/p5-verify3-f9-f10a.py` (unit) and `~/w7/p5-verify3-f4-f9b-f12.py`
(integration). Perturbation: delete **only**
`gMGPipeScreen.ResourceDestroy = &Wire_ResourceDestroy;` from `InstallClientWireTables`.

**Unperturbed:** `PipeCatalogueTest --gtest_filter=*` → `rc=0`, `[  PASSED  ] 32 tests.`

**Perturbed:**

```
--- run --gtest_filter=*ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull*  => rc=0 ---
[       OK ] PipeCatalogue.ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull (0 ms)
[  PASSED  ] 1 test.
--- run --gtest_filter=*  => rc=0 ---
[  PASSED  ] 32 tests.
```

Green, unchanged, whole suite included.

**Part 2 — does anything notice at runtime?** With the same deletion still in place, the init patch
applied, and a split entry that destroys a resource:

```
--- run --gtest_filter=ClearThenReadPixelsScenario.VerifyF9bADestroyedResourceWithTheRowDeleted  => rc=0 ---
[  PASSED  ] 1 test.
F9b: destroy: wireRecords 16 -> 16 (delta 0), emitSeq 16 -> 16 (delta 0)
F9b: VERDICT destroyReachedTheWire=0
--- run --gtest_filter=ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels  => rc=0 --- [  PASSED  ] 1 test.
--- run --gtest_filter=TriangleScenario.*  => rc=0 --- [  PASSED  ] 2 tests.
```

**Control, same probe, row still installed:**

```
F9b: destroy: wireRecords 16 -> 17 (delta 1), emitSeq 16 -> 17 (delta 1)
F9b: VERDICT destroyReachedTheWire=1
```

So the deletion is a clean, silent, fully green regression: the destroy runs the monolith applier on
the GL thread, no record reaches the wire, and every lane entry still passes.

**Minimal fix shape.** Give `PipeCatalogueTest` a case that calls `InstallClientWireTables()` and
asserts, row by row, that each of the 33 + 4 pointers differs from the monolith table's — or, the
cheaper and stronger form the review asks for, a control that **invokes** the row through the
installed split table and requires `ClientWireRecordsEmitted()` to advance and the opcode to appear
on the wire. `ClientWireRecordsEmitted()` is read by no test at this head (confirmed by grep); the
F9b probe above is that control, ready to be asserted.
**Red-once control.** Delete any one assignment in `InstallClientWireTables` → the per-row
comparison must name that row, and the destroy probe's `destroyReachedTheWire` must go 1 → 0.

---

## Finding 10 — the readback red-checks miss deletion of the mechanisms they protect — **CONFIRMED (major), both halves**

### (a) The tight-size test does not observe production

**Command.** `python3 ~/w7/p5-verify3-f9-f10a.py` (unit). Perturbation: production
`EmitTables.cpp:292`, `info.DstSize = tight;` → `info.DstSize = tight + 16;`.

**Unperturbed:** `RemoteReadback.*` → `rc=0`, `[  PASSED  ] 4 tests.`

**Perturbed:**

```
--- run --gtest_filter=*DstSizeIsTheTightExtentAndNeverThePackedOne*  => rc=0 ---
[       OK ] RemoteReadback.DstSizeIsTheTightExtentAndNeverThePackedOne (0 ms)
[  PASSED  ] 1 test.
--- run --gtest_filter=RemoteReadback.*  => rc=0 ---
[  PASSED  ] 4 tests.
```

The named case stays green with the emitted `DstSize` 16 bytes wrong, because it only exercises
`TightReadbackByteCount`, the test-facing wrapper; production multiplies width × height × bpp
independently at `:290-292`. ID49-a's reported red therefore proves nothing about the emitted value.

### (b) The capacity perturbation cannot detect deletion of the pre-check

**Command.** `python3 ~/w7/p5-verify3-f5-f10b-f11.py`, inproc + init patch. Perturbation: delete
**only** the production `session.RequireReadPixelsReplyFits(...)` call (`EmitTables.cpp:304-307`).

```
--- run --gtest_filter=ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels  => rc=0 ---
[  PASSED  ] 1 test.
--- run --gtest_filter=ClearThenReadPixelsScenario.*  => rc=0 --- [  PASSED  ] 5 tests.
--- run --gtest_filter=TriangleScenario.*  => rc=0 --- [  PASSED  ] 2 tests.
---- F10b : Fatal lines ----  (NO 'Fatal{' line at all)
```

ID47-b's named scenario — `ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels`
— is a small read that fits the pool, so with the call gone it simply passes. The perturbation
ID47-b actually performs (`tight * 1000`) only proves the helper is reachable *when it is called*.

**Minimal fix shape.** Replace ID49-a's mutation with the production one (`info.DstSize = tight + 16`)
and give `DstSizeIsTheTightExtentAndNeverThePackedOne` a way to read the **emitted** record's
`DstSize` — a test-visible last-record peek on the encoder, or the existing wire decode path — so
the test observes what crossed rather than a helper. For (b), add the control the review names: an
**oversized** emission attempt (a read whose tight extent exceeds `MaxReplyBytes`) through the
installed split table, requiring `Fatal{ReplyTooLarge, "ReadPixels …"}` **before** the record
ordinal advances; deleting the call must then leave the read unrefused.
**Red-once control.** Exactly the two perturbations executed here: `tight + 16` must turn the
tight-size case red, and deleting `RequireReadPixelsReplyFits` must turn the oversized-emission case
red. Both are green today.

---

## Finding 11 — short successful replies and failed reads become plausible pixels — **CONFIRMED (major)**

**Source.** `ClientSession::EmitAndWait` checks only for an **oversized** reply
(`if (replyOut != nullptr && replySize > replyBytes) … Fatal{ReplyTooLarge}`, `:781-787`). There is
no minimum-extent check, and `EmitReadPixels` passes `&status` but never inspects it
(`EmitTables.cpp:313-333`).

**Command.** `python3 ~/w7/p5-verify3-f11n.py`, inproc + init patch. The server sink perturbed at
`MG_Remote/Server/PipeApplier.cpp:199`, keeping the same sequence. The probe is a 4×3 RGBA8 read
into a 0xCD-filled destination after a clear to (0.25, 0.5, 0.75, 1.0).

> **Why neutral pack state.** The review's non-neutral variant cannot isolate finding 11 on this
> head: with `ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2` the **unperturbed control already returns
> zeros and dies `-11`**, because the server half of ID-49 is absent (review blocker 1, v1's) and
> the backend honours the client's pack state into a tight scratch. Measured, see §D. The neutral
> read takes the emitter's zero-copy fast path — the reply lands straight in the application's
> pointer — so only the reply's extent and status vary.

**Unperturbed control:**

```
--- run ...VerifyF11nAShortReplyOnTheTightPath  => rc=0 ---  [  PASSED  ] 1 test.
F11n: row 0 bytes  0.. 3 = 40 80 bf ff
F11n: row 1 bytes 16..19 = 40 80 bf ff
F11n: row 2 bytes 32..35 = 40 80 bf ff
F11n: of the 48 destination bytes, 0 are still the sentinel 0xCD
```

**Perturbed — `Status=OK` with `info.DstSize - oneRowBytes`, same sequence:**

```
--- run ...VerifyF11nAShortReplyOnTheTightPath  => rc=0 ---  [  PASSED  ] 1 test.
F11n: row 0 bytes  0.. 3 = 40 80 bf ff
F11n: row 1 bytes 16..19 = 40 80 bf ff
F11n: row 2 bytes 32..35 = cd cd cd cd
F11n: of the 48 destination bytes, 16 are still the sentinel 0xCD
---- F11n short OK reply : Fatal lines ----  (NO 'Fatal{' line at all - no named refusal)
```

The application receives its own stale bytes as the third row of pixels, the call returns normally,
`glGetError()` is 0 and the test **passes**.

**Perturbed — `Status=DECLINED`, zero payload, same sequence:**

```
--- run ... => rc=0 ---  [  PASSED  ] 1 test.
F11n: row 0 bytes  0.. 3 = cd cd cd cd
F11n: row 1 bytes 16..19 = cd cd cd cd
F11n: row 2 bytes 32..35 = cd cd cd cd
F11n: of the 48 destination bytes, 48 are still the sentinel 0xCD
(NO 'Fatal{' line at all - no named refusal)
```

**Perturbed — `Status=ERROR`, zero payload, same sequence:** identical output, 48/48 sentinel,
`rc=0`, `[  PASSED  ] 1 test.`, no Fatal. **The ReadPixels status is ignored entirely.**

**Minimal fix shape.** Two lines, both in `ClientSession::EmitAndWait`'s reply block: require
`replySize == replyBytes` for a reply-owning row that offered a destination —
`Fatal{ReplyShort, "<row>"}` — and refuse to deliver on a non-OK status for `ReadPixels`; plus, in
`EmitReadPixels`, branch on `status` before scattering or returning. The exact-extent rule is
CONTRACT §2 row 23's own ("never truncated").
**Red-once control.** The three perturbations executed here, each asserting its own diagnostic from
a private log file and asserting the destination is **untouched**; removing the extent check must
restore `16 of 48 stale, PASSED`.

---

## Finding 12 — a repeated successful `MakeEGLCurrent` does not adopt its new caps — **CONFIRMED (major)**

**Source.** `Server::ServerMakeEGLCurrent` (`MG_Remote/Server/ServerLoop.cpp:618-628`) republishes a
snapshot on **every** call once the session is accepted. `BackendObject_Remote::MakeEGLCurrent`
(`BackendObject_Remote.cpp:206-213`) forwards and then calls the base, and
`MG_Backend::BackendObject::MakeEGLCurrent` (`BackendObject.cpp:341-347`) calls `InitCapabilities()`
**only** `if (!m_backendCapabilitiesInitialized)`. `BackendObject_Remote::InitCapabilities`
(`:106-140`) is the only thing on that path that calls `session->PumpControlPlane()`.

**Command.** `python3 ~/w7/p5-verify3-f4-f9b-f12.py`, inproc + init patch. The probe repeats
`eglMakeCurrent` on the already-initialised surface and reads the mirror generation and a public cap
getter with **no** `PumpControlPlane` and **no** `Present`.

```
--- run ...VerifyF12RepeatedMakeCurrentAdoptsItsSnapshot  => rc=1 ---
F12: BEFORE repeat  mirrorGeneration=3 GL_MAX_TEXTURE_SIZE=16384
F12: AFTER  repeat   ok=1 mirrorGeneration=3 GL_MAX_TEXTURE_SIZE=16384  (NO PumpControlPlane, NO Present)
F12: EXPLICIT PumpControlPlane() adopted=1 mirrorGeneration=4
F12: VERDICT repeatAdoptedIt=0 snapshotWasWaiting=1
F12: after TWO more repeats gen=4, then pump adopted=2 gen=6
```

The make-current **succeeded** (`ok=1`), a snapshot **was** published (the explicit pump adopted
exactly one and moved the generation 3 → 4), and neither the mirror generation nor the public
`GL_MAX_TEXTURE_SIZE` moved at the time of the call. The accumulation half is confirmed too: two
further repeats leave the generation at 4 and the next pump adopts **2** at once (4 → 6).

*(The gtest verdict `rc=1` is the Split fixture's own arming assertion in `TearDown` — this probe
emits no wire records, so `EmitSeq` does not move. It is not a failure of the measurement; the
numbers above are the result.)*

**Minimal fix shape.** In `BackendObject_Remote::MakeEGLCurrent`, after the base call succeeds and
when the surface was already initialised, adopt what the forwarder just published — a
`session->PumpControlPlane()` plus `RefreshFormatCapabilities()`, i.e. the tail of
`InitCapabilities()` factored out — so that "a second CapsSnapshot arrival IS the invalidation"
(c0's R-12 ruling) holds at the make-current that caused it.
**Red-once control.** The probe above, asserted: `mirrorGeneration` must increase across a repeated
successful make-current and a following explicit pump must adopt **0**. Removing the new pump must
restore `repeatAdoptedIt=0 snapshotWasWaiting=1`.

---

## §C. The two report discrepancies

### (i) `p5-c1-r2-igpu-split-mono.log` says 19 failed of 1149; c1-v2.md §6 says 7 — **the log is right, and §6's sentence is wrong twice over**

```
$ sed -n '2301p' ~/w7/p5-c1-r2-igpu-split-mono.log
98% tests passed, 19 tests failed out of 1149
```

The failure list (`:2504-2523`) is **nineteen entries, every one of them a `DirectGLES.Split.*`
entry**, each labelled `integration-gpu integration-split`. The two Split entries that are *not* in
it are `#3157` and `#3168`, the two `AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation`
cases, which passed. The label summary in the same log confirms the overlap:

```
integration-gpu      = 323.60 sec*proc (1149 tests)
integration-split    =   5.80 sec*proc (21 tests)
```

**Yes, the `Split.` entries force inproc**, and that is why they are in a monolith-labelled run.
`MG_IntegrationTest/CMakeLists.txt:1854-1857` builds `MGL_ITEST_GLES_SPLIT_ENVIRONMENT` with
`"MOBILEGL_TRANSPORT=inproc"` and the three `gtest_discover_tests` blocks at `:1908`, `:1917`,
`:1926` attach it as a ctest `ENVIRONMENT` property together with
`LABELS "integration-gpu;integration-split"`. A ctest `ENVIRONMENT` property overrides the parent
environment, so those 21 entries run inproc whatever the surrounding invocation says — and `-L
integration-gpu` selects them because they carry that label too.

So, precisely:

* **19 is the correct count for that log.** All 19 failures are Split entries run **without** the
  init patch — which is the state c1-v2.md §6 itself describes two paragraphs earlier ("Without it
  the lane is **19 failed / 21** on this head").
* **The 1128 non-Split `integration-gpu` entries were entirely green** under monolith in that run.
* **7 is the count you get with the init patch applied**, and it is a count of Split entries, not of
  monolith ones. Measured here: `ctest -L integration-split` with the init patch gives exactly
  `7 tests failed out of 21`, the seven `PersistentCoherentMapScenario` entries.

§6's sentence "The same seven are the only `integration-gpu` failures in `build-split` under
monolith (1149, 7 failed)" is wrong in two ways: the saved log records 19, not 7; and the failures
it counts are not monolith failures at all — they are inproc Split entries that happen to share the
`integration-gpu` label. The correct statement is: *"under monolith transport the 1128 non-Split
integration-gpu entries are green; the 21 Split entries force inproc and, with the init patch, seven
of them fail on the `resource_flush_range` seam."*

### (ii) Is there a full `inproc` `integration-gpu` lane log for this head? — **No. One was run.**

`~/w7/p5-c1-*` contains `p5-c1-r2-igpu-push.log` (push build), `p5-c1-r2-igpu-split-mono.log`
(build-split, monolith invocation, the log above) and `p5-c1-r2-isplit-inproc.log` (the 21-entry
`integration-split` lane only). **No full inproc `integration-gpu` log exists**, confirming the
review's "what I could not verify" note. It was therefore run once — see §C-census below.

---

## §C-census. The full `inproc` `integration-gpu` lane, run once

```
$ cd ~/w7/p5-verify/build-split          # init patch applied locally
$ MOBILEGL_TRANSPORT=inproc ctest -L integration-gpu -j 8
49% tests passed, 583 tests failed out of 1149      (40 s wall, ctest rc=8)
```
Log: `~/w7/p5-verify3-igpu-inproc.log`. Parsed by `~/w7/p5-verify3-census.py` / `-census2.py`.

| verdict | count |
|---|---|
| Passed | **386** |
| Skipped | **180** |
| Subprocess aborted | **558** |
| Failed (assertion) | **23** |
| SegFault | **2** |
| Timeout | 0 |
| **total** | **1149** |

Aborted + Failed + SegFault = 583 = ctest's failure count. By lane:

| lane | total | passed | skipped | aborted | failed / segfault |
|---|---|---|---|---|---|
| `DirectGLES` | 526 | 172 | 72 | 270 | 10 / 2 |
| `DirectVulkan` | 487 | 144 | 68 | 265 | 10 / 0 |
| `DirectVulkan.HandleRecycle` | 72 | 32 | 32 | 6 | 2 / 0 |
| `DirectGLES.HandleRecycle` | 36 | 23 | 8 | 4 | 1 / 0 |
| `DirectGLES.Split` | 10 | 7 | 0 | 3 | 0 / 0 |
| `DirectGLES.Split.SmallRing` | 10 | 7 | 0 | 3 | 0 / 0 |
| `DirectVulkan.AsyncCompile` | 5 | 0 | 0 | 5 | 0 / 0 |
| `DirectGLES.MapPersistentRoundtrips` | 2 | 1 | 0 | 1 | 0 / 0 |
| `DirectGLES.Split.PersistentMapArm` | 1 | 0 | 0 | 1 | 0 / 0 |

**This is the expected shape, not a regression.** Under `MOBILEGL_TRANSPORT=inproc` every ordinary
entry takes the split emit table, and BRIEF §12 C-3 rules that only **six** of 71 slots are
implemented; the other 65 are `Fatal{UnmigratedVerb}`. The census is therefore mostly a measurement
of the reduced path's size. The seven Split failures are the known flush seam (3 + 3 + 1 across the
three Split lanes).

**First failure per lane, with one probed log line** (`MOBILEGL_LOG_FILE_PATH` set per probe,
`~/w7/p5-verify3-probe.sh`; note that *nothing* reaches stderr without it):

| lane | first non-passing entry | its own diagnostic |
|---|---|---|
| `DirectGLES` | `CrossFrameBufferScenario.VertexBufferSubData` | `MGPipe: Fatal{UnmigratedVerb, "DrawElements"}` |
| `DirectVulkan` | `CrossFrameBufferScenario.VertexBufferSubData` | `MGPipe: Fatal{UnmigratedVerb, "DrawElements"}` |
| `DirectVulkan.AsyncCompile` | `XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone` | `MGPipe: Fatal{UnmigratedVerb, "BeginTransformFeedback"}` |
| `DirectGLES.HandleRecycle` | `Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin` | `MGPipe: Fatal{BlobMissing, "SetDynamicState"} - the row's decoder requires a declared blob and the call site handed over 0 bytes at 0x7ffe2d5d48a0. Under monolith the companion pointer carries them; under split they have to be staged, and there is nothing to stage` |
| `DirectGLES.Split` (all three) | `PersistentCoherentMapScenario.AWriteAfterAFrameBoundaryReachesTheNextFramesDraw` | `MGPipe: Fatal{ProtocolCorruption} resource_flush_range {slot=1, gen=0, glName=1}: a non-empty flush carries no bytes (offset=0, size=120, storage=120 bytes)` |

The `Fatal{BlobMissing, "SetDynamicState"}` line is the one genuinely new diagnostic the census
surfaces: a `kHasBlob` row reaching the decoder with `Size = 0` on the ordinary (non-Split) lanes
under inproc. It is contract §2 table 1 row 4's known "declared real, nothing reads it" gap seen from
the split side; it is **not** one of the twelve findings and needs an integrator ruling on ownership.

**The 2 SegFaults and 23 assertion failures are worth naming** because two of them are ID-49's own:

```
Exception: SegFault    DirectGLES.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters
Exception: SegFault    DirectGLES.ForcedDepthStencilEmulation.DepthStencilReadbackMatrixScenario.DepthReadbackHonoursThePackPixelStoreParameters
```

These are exactly the two SEGFAULTs ID-49 cites as the shape of the missing server half.

---

## §D. Blocker 1 (v1's) turned up while verifying finding 11 — reported, not assigned

Building finding 11's probe in the review's own non-neutral pack state
(`ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2`, 4×3 RGBA8) produced a broken **unperturbed control**:

```
--- F11 CONTROL (unperturbed server reply) => rc=-11 ---
F11: row 0 first pixel at byte  40 = 00 00 00 00
F11: row 1 first pixel at byte  72 = 00 00 00 00
F11: row 2 first pixel at byte 104 = 00 00 00 00
```

Zeros and a SIGSEGV with no perturbation at all. This is review blocker 1 (ID-56 blocker 1, v1's
open half of ID-49): `ServerVerbSink::OnReadPixels` (`PipeApplier.cpp:163-202`) resizes
`m_readbackScratch` to `info.DstSize` — ID-49's **tight** 48 bytes — and then calls the backend,
which under inproc reads the client's own pack state through `PackStateFromContext()` and writes
rows at 40, 72 and 104…119. There is no neutral-pack scope anywhere in that function. The census's
two `DepthReadbackHonoursThePackPixelStoreParameters` SegFaults are the same defect.

It is recorded here only because it blocks any non-neutral readback verification on this head; it is
v1's item and was not perturbed or fixed.

---

## Verdict table

| # | finding | verdict | the decisive evidence |
|---|---|---|---|
| 2 | pack-PBO offset used as a CPU destination (blocker) | **CONFIRMED** | offset-16 read under inproc → `rc=-11`, never returns; same call under monolith returns `PBO[16..19] = 40 80 bf ff`; `info.DstOffset` is literal `0` |
| 4 | `Stop` reinstalls driver-reaching adapters on a live transport | **CONFIRMED** | after `UninstallClientWireTables()` with `sessionActive=1`: `wireOrdinalMoved=0 emitSeqMoved=0`, test PASSED, no refusal. Failed-`Start` half confirmed at source |
| 5 | the two escapes turn ERROR into an ordinary decline | **CONFIRMED** | respecify ERROR → `rc=0`, no `Fatal{`; MapPersistent ERROR → indistinguishable from baseline; generated row ERROR → `Fatal{ReplyError, "resource_create"}` |
| 6 | `PACK_SKIP_IMAGES` applied to a 2-D read | **CONFIRMED** | `bytes touched = 48, first = 48, last = 95` on a 96-byte destination; fast path predicate = 0 |
| 7 | R-1's assertion inert, and checks too late after the bracket | **CONFIRMED** (one sub-point PARTIAL) | zero production uses of `ScopedApplierEntry`; with the bracket, flag up during the fill window and down at emission → check silent, PASSED. No real two-thread applier race harness |
| 8 | "20/20 RED" does not establish twenty attributed failures | **CONFIRMED** | stubbed `run()` → 20/20 RED, `CONTROLS_THAT_DID_NOT_GO_RED=none`, exit 0; all-green stub → 20 failures listed, **exit 0** |
| 9 | the 33+4 catalogue test cannot see a missing client-wire row | **CONFIRMED** | row deleted → `PipeCatalogueTest` 32/32 green; split destroy `wireRecords 16→16 (delta 0)` vs control `16→17`, all lane entries green |
| 10 | the readback red-checks miss the mechanisms they protect | **CONFIRMED** (a and b) | `DstSize = tight + 16` → named case still green; `RequireReadPixelsReplyFits` deleted → ID47-b's scenario + 5 + 2 entries all green |
| 11 | short / failed replies become plausible pixels | **CONFIRMED** | one row short → `16 of 48` destination bytes stale, PASSED, no Fatal; DECLINED and ERROR with zero payload → `48 of 48` stale, PASSED, no Fatal |
| 12 | a repeated `MakeEGLCurrent` does not adopt its new caps | **CONFIRMED** | `gen 3 → 3` across a successful repeat, explicit pump `adopted=1 → gen 4`; two more repeats → pump `adopted=2 → gen 6` |

**Ten of ten confirmed**, one with a single sub-point at PARTIAL (finding 7's true concurrent race).

## For the integrator

1. **Finding 7 (c) is an ID-53 gap, not a c1 finding:** `MGL_ITEST_GLES_SPLIT_ENVIRONMENT` sets no
   `MOBILEGL_LOG_FILE_PATH`, so every `DirectGLES.Split.*` abort in CI is unattributed. ID-53 ruling
   (1) has not landed for these three blocks. Owner unclear (t1's lane plumbing vs the joint-merge
   package).
2. **`Fatal{BlobMissing, "SetDynamicState"}`** (census, `DirectGLES.HandleRecycle` and its family)
   is a `kHasBlob` row reaching the decoder with `Size = 0` on the ordinary lanes under inproc. Not
   one of the twelve findings; needs an owner.
3. **Findings 5 and 11 both argue for one mechanism, not two:** the escapes and `EmitReadPixels`
   should read their answers through the same mailbox rules the generated rows already obey. Worth
   deciding before c1's round 3 splits the fix across two files.
