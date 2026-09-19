# c1-review-v3 — adversarial review of package c1, round 3 (`p5/c1` head `5682f429`)

Reviewer: same-family (Claude), the client role. Worktree `~/w7/p5-c1`, reverted and clean at the
end; `p5/c1` is still `5682f429` and `p5/v1` is still `ca92308b` — nothing was committed to either.
Every perturbation below was run in that worktree and reverted. The joint was measured on a scratch
branch `p5/c1rev3-joint` created in that worktree and left there; it touches no package ref.
Temp files `~/w7/p5-c1rev3-*`.

**Counts: 8 CLOSED, 6 PARTIAL, 1 NOT CLOSED. New findings: 0 blockers, 5 majors, 4 minors.**

Round 3's *code* is good: I could not break a single one of the runtime fixes, and the joint with
v1's round 2 is the number the phase has been waiting for (`integration-split` 21/21, inproc census
0 segfaults). What round 3 did **not** do is gate several of those fixes. Five of the fourteen
claimed items ship no control at all, and three ship a control that cannot observe the production
code it names — measured, not argued: I deleted the production wiring of M2, M3, M4 and B1 and the
2030-case unit lane stayed **100% green** in every case. R-16 §13 is explicit that a gate without an
executed "I made it red once" line counts as unverified, and c1-v3.md claims one for B1 that does
not exist.

---

## 1. The items

### 1. B1 — the five by-address delete sites — **PARTIAL** (code CLOSED, gate NOT CLOSED)

**The routing is fixed.** `git grep -n '&MGPipeApply' 5682f429 -- MobileGL/MG_Impl/` returns
**only two comment lines** (`PipeFill.cpp:1530`, `:1537`) and no code. The five sites are
`&MGPipeRouteDeleteSamplerView` (`:1562`), `&MGPipeRouteResourceDestroy` (`:1583`, `:1624`),
`&MGPipeRouteDeleteSamplerState` (`:1661`), `&MGPipeRouteDeleteShaderState` (`:1682`), and
`EmitDeleteIfPublished`'s parameter is renamed `route`. Round 2's corroborating symptom is gone:
`MGPipeRouteDeleteSamplerView` / `DeleteShaderState` / `DeleteSamplerState` now have callers.

**The grep gate does not exist.** c1-v3.md §1 B1 says "Gate: the red-check refuses any
`&MGPipeApply` under `MG_Impl/` (grep, now 0)", and `PipeFill.cpp:1537` repeats it in shipped
source. I read all 272 lines of `~/w7/p5-c1-r3-redcheck.py`:

```
$ grep -n 'MGPipeApply\|grep\|MG_Impl' ~/w7/p5-c1-r3-redcheck.py
31:FILL = "MobileGL/MG_Impl/Pipe/PipeFill.cpp"
```

That is the only hit — a path constant. There is no grep, no gate, and **no B1 case**: the
runner's nine cases are B3, codex4, M2, M3, codex6, ID49, ID47, M8, ID47b. Nor is the gate in
`scripts/`, `CMakeLists.txt`, or `.github/workflows/test.yml` (its two `MGPipeApply` mentions are
an unrelated `nm` check at `:868`).

**The claimed red-once line was never run, and nothing catches the revert.** c1-v3 says "Red once:
revert one site to `&MGPipeApplyResourceDestroy`". I ran exactly that:

```
=== B1   one delete site reverted to &MGPipeApplyResourceDestroy ===
  ctest -L unit: 100% tests passed, 0 tests failed out of 2030
  *** NO TEST NAMES IT -- the lane stays 100% green ***
```

So round 2's fix requirement #1 ("plus a grep gate for `&MGPipeApply` under `MG_Impl/`, because the
next one will be missed the same way") is **not met**, and B3's new test — which round 2 called "the
gate that would have caught B1" — does not catch it, because B1 is a direct call in `PipeFill` that
never goes through the table B3 observes. → **new finding N-1**.

### 2. B3 / codex 9 — the client arm is observed — **CLOSED**

`PipeRouting.TheInstalledClientArmIsWireAndNotMonolithAndEveryRoutedRowMoved` installs the CLIENT
tables and counts cells that differ from `MGPipeMonolithScreen()/Context()/Escapes()`: 33 generated
+ 4 escapes, and `MGPipeInstalledArm() == kClientWire`. It reads the pointers back rather than
constructing them, so a dropped assignment reads *equal* and is caught.

c1's red-check drops `gMGPipeContext.SetVertexBuffers` and reports RED. **My own perturbation took
the half c1's did not** — an escape row:

```
=== B3e  one ESCAPE row dropped from InstallClientWireTables ===
  (gMGPipeRouteEscapes.ResourceRespecify commented out)
  ctest -L unit: 99% tests passed, 1 tests failed out of 2030
     RED  PipeRouting.TheInstalledClientArmIsWireAndNotMonolithAndEveryRoutedRowMoved
```

Red on its own message. The fork control
`AClientWireRowWithNoSessionRefusesByNameRatherThanApplying` adds the behavioural half
(`Fatal{NoClientSession, "ResourceDestroy"}`, which a monolith adapter would not produce).

*Scope note (minor N-6):* the case asserts a **count**, not per-row identity, so a dropped row
combined with an accidentally-overwritten unrouted row still reads 33.

### 3. M1 / ID-53 — `Fatal{BarrierViolation, "<slot>"}` — **CLOSED** (for c1's half)

The check is `ClientSession.cpp:707`, reads `g_applyThreadInsideApplier`, and reports through
`MGLOG_F`. On c1's own head it is inert — `git grep 'ScopedApplierEntry'` finds only the
definitions in `ClientSession.h:132-136`, zero call sites — and c1 says so rather than claiming
otherwise. That is the honest statement, and c1 correctly assigns the arm to v1's `ApplyOne`.

**It is armed by v1's bracket and it is readable.** On my joint scratch branch with the bracket
applied, one split entry run by hand with a private log:

```
$ MOBILEGL_IPC_VERB_BARRIER=0 MOBILEGL_LOG_FILE_PATH=~/w7/p5-c1rev3-m1-barrier.log \
    ./MobileGLIntegrationTest --gtest_filter='ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels'
--- private log: size=6926 ---
BarrierViolation lines in the FILE: 1
[09:35:50] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} -
the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of
them runnable, which is what keeps one process-wide gPipeInputs legal
```

With the barrier ON the same entry passes and the file holds no Fatal. So c1's choice of `MGLOG_F`
is vindicated: ID-53's E1 can read the string from the private file once j0's lane plumbing lands.

**Not c1's, but the integrator must see it:** the violation fires in the forked **pre-flight
child**, so `ScenarioFixture.h:111` skips the entry and the whole lane reads green (§5 below).
That is ID-62's N-1, still live on this joint, and assigned to j0.

**Placement point (codex 7).** c1 left the check at emission and argues the second check at the
first `gPipeInputs` write belongs to p1's `MGPipeValidateForVerb`. The file is p1's, so the
assignment is right; c1 marks it a design argument rather than asserting it. Accept.

### 4. M2 / codex 11 — status and exact extent — **PARTIAL**

Production is right: `EmitAndWait` gained `Uint64* replySizeOut`, and both the fast and bounce paths
call `RequireReadbackReplyComplete(status, replySize, tight)` with a named Fatal per mode —
`Fatal{ReplyError,"ReadPixels"}`, `Fatal{ReadbackDeclined,"ReadPixels"}`,
`Fatal{ReadbackReplyShort,"ReadPixels <got> < <expected>"}`. The bounce path checks **before** the
scatter, so a short reply never reaches the application pointer.

**The control drives the predicate, not the wiring.**
`RemoteReadback.AReplyIsScatteredOnlyWhenItIsOkAndExactlyTheReadsExtent` calls
`ReadbackReplyIsComplete(...)` directly. I deleted **both** `RequireReadbackReplyComplete(...)` call
sites from `EmitReadPixels` — the entire production wiring:

```
=== M2a  both RequireReadbackReplyComplete CALLS deleted (call-site wiring) ===
  ctest -L unit: 100% tests passed, 0 tests failed out of 2030
  *** NO TEST NAMES IT ***
```

The verifier's §11 perturbation (post Status=OK with `DstSize - oneRowBytes` from the server sink
and require a named refusal) was not re-run by c1 and is not in the runner. → **N-3**.

### 5. M3 / codex 10 — one tight-size function — **PARTIAL**

**10a, the shared body: closed.** `EmitReadPixels` now computes `const Uint64 tight =
TightReadbackBytes(width, height, format, type);` and the test-facing `TightReadbackByteCount`
forwards to the same function, so a `+16` inside `TightReadbackBytes` moves both (c1's red-check
perturbs exactly that and reports RED).

**But codex 10's own confirm recipe still passes.** codex 10 said: "Change only production
`info.DstSize = tight` to `tight + 16`: `DstSizeIsTheTightExtentAndNeverThePackedOne` still passes."
That test asserts only on `TightReadbackByteCount(...)`; it never reaches the emitter. I ran the
recipe:

```
=== M3a  production info.DstSize = tight + 16 (codex 10's OWN recipe) ===
  ctest -L unit: 100% tests passed, 0 tests failed out of 2030
  *** NO TEST NAMES IT ***
```

So the number the emitter actually puts on the wire is still unobserved; only the helper is. → **N-3**.

**10b, the capacity half: closed.** `session.RequireReadPixelsReplyFits(...)` is called at
`EmitTables.cpp:368`, before emission, with the tight extent. c1's `ID47b` control multiplies the
tight extent by 100000, drives the split lane with the init patch and observes
`Fatal{ReplyTooLarge` at the CLIENT (rc=-6, baseline restored green) — an actual oversized emission
attempt, which is what codex 10 asked for. I did not independently re-run the "delete the call"
variant; c1's account (the abort moves to the server's `Post`, so the client-side assertion goes
absent) is consistent with `ReplySlot.h`'s structure.

### 6. M4 / codex 5 — ERROR is not a decline — **PARTIAL**

**Scope is right.** The reply-owning rows are the `kReplySlot` set in `generated/PipeWire.inc`:
`ResourceCreate(2)`, `ResourceRespecify(3)`, `MapPersistent(5)`, `SetTextureParams(47)`,
`ResourceSubData(48)`, `ResourceReadback(52)`, `ReadPixels(58)`. The two escapes that own a slot
(`ResourceRespecify`, `MapPersistent`) now abort `Fatal{ReplyError, "resource_respecify"}` /
`"map_persistent"` on `status == 2`; `ResourceFlushRange` and `CreateShaderState` own no slot and
pass `nullptr` for status, so "the two escapes" is the correct set.
`Wire_ResourceReadback` forwards status into `MGPipePostReply` and the mailbox aborts — also correct.

**`SessionWait::ShutDown`: resolved by making it DECLINED.** `ClientSession.cpp` now sets
`*statusOut = kStatusDeclined` on the dead-doorbell arm, so the "teardown legitimately reaches
here" comment is true for the acceptance rows rather than contradicted a frame later.

**The escape rule has no control.** `PipeRouting.AnErrorStatusIsNotFoldedIntoAcceptedOrRefused`
drives `MGPipeTakeReplyBool` over a hand-minted slot; it never invokes either escape. I deleted
**both** `if (status == 2)` blocks — the whole of M4's change:

```
############ (1) M4 escape half: delete both ERROR->Fatal blocks ############
  ctest -L unit: 100% tests passed, 0 tests failed out of 2038
  *** NO TEST NAMES IT -- the escape ERROR rule has no control ***
```

(2038 rather than 2030 because this ran on the joint branch, which adds v1's `ServerLoopTest`.)
→ **N-4**.

**New, from M4's own change (minor N-7):** because ShutDown now answers DECLINED, a teardown that
races a readback takes `RequireReadbackReplyComplete`'s decline arm and aborts
`Fatal{ReadbackDeclined, "ReadPixels"}`. M4 restored the comment's promise for the acceptance rows
and broke it for the readback row.

### 7. M5 — `Fatal{InitialBytesNotCarried}` role-aware — **CLOSED** (code), no control

Both split-only branches in `PipeFill.cpp` (`:757` respecify, `:877` flush follow-up) now also
require `!MG_Remote::Client::RunsAsTheServerRole()`. `RunsAsTheServerRole()` was moved out of
`WireTables.cpp`'s anonymous namespace and declared in `WireTables.h`, so `PipeFill` needs no server
header — and the non-disaggregated builds are unaffected (`build-linux`/`build-push`/`build-verify`
unit lanes 1816/1816 each, and G1 `.text +0`).

**The server never reaches it on the joint:** `Fatal{InitialBytesNotCarried}` appears **0 times** in
my joint inproc census (1149 entries), and the split lane is 21/21.

No red-once line and no control — see N-5.

### 8. M6 / codex 8 — the red-check runner — **CLOSED**

I executed all three perturbations the charter names, against a **stubbed copy**
(`~/w7/p5-c1rev3-probe-a.py`) whose `apply`/`restore`/`build`/`run` are overridden, so the real tree
was never touched. The runner's verdict is `went_red = expect in out` where `expect` is a
case-specific `[  FAILED  ] Suite.Name` or a named `Fatal{...}`; `rc` is printed but not used.

| perturbation | required | measured |
|---|---|---|
| every case returns an UNRELATED failure (`"UNRELATED_FAILURE\nSegmentation fault"`, rc −11) | must NOT count as red | **9 × STILL-GREEN, 0 RED**, EXIT=1 |
| every case returns an ALL-GREEN gtest run (`[  PASSED  ] 1 test.`, rc 0) | must exit non-zero | **9 × STILL-GREEN, 0 RED**, EXIT=1 |
| build fails after preflight | HARNESS failure, never a red | **9 × HARNESS-FAIL, 0 RED**, EXIT=1 |

Round 2's runner scored 20/20 RED and exit 0 on the first of those. All four of M6's sub-claims
hold: own string; build break is a harness failure; restore + rebuild + re-baseline covers the two
integration cases (`apply([(INIT, None, None)])` before the baseline run and restore after);
`sys.exit(1)` when `not_red or harness_failures`.

*The runner is still an untracked file outside the repo*, so none of this runs in CI — the same
structural point as N-1.

### 9. M7 — what arms the lane — **CLOSED**

`WireTables.h:62-70` now states the truth: the harness arms on the encoder's `EmitSeq`
(`Harness/SplitRuntimePeek.cpp:50`, `Harness/ScenarioFixture.h:85`), the counter is the routed
ordinal, and re-pointing the harness is t1's file. Verified by grep — the only readers of
`ClientWireRecordsEmitted()` are `PipeFill.cpp:766` and `:768` (its own `InitialBytesNotCarried`
self-check) plus the definition and declaration:

```
5682f429:MobileGL/MG_Impl/Pipe/PipeFill.cpp:766:  const Uint64 before = MG_Remote::Client::ClientWireRecordsEmitted();
5682f429:MobileGL/MG_Impl/Pipe/PipeFill.cpp:768:  if (MG_Remote::Client::ClientWireRecordsEmitted() == before) {
5682f429:MobileGL/MG_Remote/Client/WireTables.cpp:591: Uint64 ClientWireRecordsEmitted() { return g_emitted; }
5682f429:MobileGL/MG_Remote/Client/WireTables.h:70:   Uint64 ClientWireRecordsEmitted();
```

Correcting a false claim in a header rather than asserting a fact c1 cannot make true is the right
call.

### 10. M8 / ID-57 / codex 2 — the pack-PBO destination — **CLOSED on behaviour, PARTIAL on the control**

**ID-57's own control passes.** The ruling names it exactly: "a bound 64-byte pack PBO and an
offset-16 read under inproc dies with that string". c1 did not run it — its red-check instead
replaces the probe with `if (true)`, which is R-16's "a probe may not be armed against a stub"
(§13). I wrote the real control (a temporary `TEST_F` in `ClearThenReadPixelsScenario.cpp`, since
reverted) binding a real 64-byte `GL_PIXEL_PACK_BUFFER` and reading 1×1 RGBA8 at offset 16:

```
############ inproc (SPLIT) ############
rc = -6  (SIGABRT)
   [ RUN      ] ClearThenReadPixelsScenario.ReviewerPackPboOffsetRead
   [reviewer] PACK PBO bound (64 bytes); calling glReadPixels at offset 16
   Fatal lines in the private log: 1
     [09:39:08] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{UnmigratedVerb, "ReadPixels+PACK_BUFFER"}
```

The `[reviewer] glReadPixels RETURNED` line never prints, so the refusal precedes the emission and
the offset is never dereferenced — codex 2's SIGSEGV at address 16 is gone.

**The monolith arm passes too** — the second half of ID-57's control ("the same call under monolith
reads back through the buffer as today"). Same probe, `MOBILEGL_TRANSPORT=monolith`, after a clear
to opaque green:

```
############ (2) ID-57 monolith arm ############
  rc = 0
   [       OK ] ClearThenReadPixelsScenario.ReviewerPackPboOffsetReadMonolith (1 ms)
   [reviewer] monolith PBO bytes at offset 16: 0 255 0 255
```

(0,255,0,255) came back **through** the pack buffer at offset 16, so the monolith path is
untouched — as G1's `.text +0` and the split-only emit table already implied, now measured.

The residual gap is that c1's *shipped* control cannot distinguish "the probe expression is right"
from "the Fatal is reachable": if `GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject()`
were wrong, `if (true)` would still be red. → **N-5 (grouped)**.

### 11. codex 6, codex 4, codex 12

**codex 6 — `PACK_SKIP_IMAGES` ignored for a 2-D read — CLOSED.** Both
`ReadbackPackStateIsTight` (the `SkipImages` term is gone) and
`ScatterTightReadbackIntoPackState` (the `SkipImages * imageRows * strideBytes` offset is gone)
stop consulting the image-level parameters, matching `honorPackImageParams=false` at
`DirectGLES.cpp:10905`. `RemoteReadback.PackSkipImagesIsIgnoredForATwoDimensionalRead` asserts a
4×3 RGBA8 read with `SKIP_IMAGES=1, IMAGE_HEIGHT=3` lands at bytes 0–47, that 48–95 keep the
`0xEE` sentinel, and that the fast path takes it. The runner's red-once re-adds the term and the
case goes RED.

**codex 4 — teardown refuses by name — CLOSED for the routed rows.** `UninstallClientWireTables()`
now raises `g_clientTablesUninstalled` instead of reinstalling the monolith adapters;
`RequireSession` reads it first and aborts `Fatal{ClientTablesUninstalled, "<row>"}` before any ring
access; `ReinstallMonolithAfterTeardown()` runs at the very end of **both** Stop paths (the
`!m_started` early return and the full path), which also closes minor m2.
`PipeRouting.ARoutedCallDuringTeardownRefusesByNameNotRunsTheApplier` drives it and the runner's
red-once reverts Uninstall to `MGPipeInstallMonolithTables()` → RED.
**But the five class-B verbs are not covered** → **N-2 (major)**, below.

**codex 12 — caps adopted on every make-current — NOT CLOSED as a gate.**
`BackendObject_Remote::MakeEGLCurrent` now pumps the control plane and calls
`RefreshFormatCapabilities()` after a successful non-release make-current, which is the right shape
and is correctly skipped for a release-current. But there is **no control anywhere**:
`git grep -l 'MakeEGLCurrent' 5682f429 -- MobileGL/MG_Test/ MobileGL/MG_IntegrationTest/` returns
**nothing**, the runner has no codex-12 case, and c1-v3.md §1 codex 12 ships **no "Red once" line** —
it only restates the verifier's before-measurement (`gen 3→3`). Under R-16 §13's fourth bullet that
is an unverified gate. → **N-5 (grouped)**.

### 12. The six minors, and m1 deferred

- **m2** — closed by codex 4 (above), including the failed-`Start` early-return path.
- **m6** — the `ImageHeight` half is closed by codex 6 (it is no longer consulted). The
  `0 < ROW_LENGTH < width` overlap is now a written comment saying the client reproduces GL 8.4.4
  verbatim rather than clamping, and noting the fast path rejects any `ROW_LENGTH != width`. That
  is exactly the "say so" the round-2 review asked for. Accept.
- **m3, m4, m5** — left as documented debts with reasons. Accept; all three are coverage/ownership
  statements, not code defects, and m5 (`gen_pipe.py` is c0's) is correctly outside R-17's grant.
- **m1 (the four escape-row generated thunks point at null members) — deferring it is a judgment
  the integrator should accept.** What m1 is: `MGP_ResourceRespecify`, `MGP_MapPersistent`,
  `MGP_ResourceFlushRange`, `MGP_CreateShaderState` in `generated/PipeThunks.inc` call through a
  null `gMGPipeScreen`/`gMGPipeContext` member, and `PipeCatalogueTest` *pins* those rows null —
  it pins the trap rather than closing it. The cost of the alternative is real and c1 states it
  correctly: installing a `Fatal{RowIsAnEscape}` marker makes those cells non-null, which moves
  `PipeCatalogueTest`'s 33/4/37 partition — a gate the integrator reads — for code that has **zero
  callers** (the routes go through `gMGPipeRouteEscapes`, and the catalogue test pins
  `escapesInstalled == 4`). Trading a read gate's arithmetic for a diagnostic on dead code is a bad
  trade at the end of a phase. c1 marks it rather than refuting it and says precisely what would
  close it. **Accept the deferral**; it belongs in the P6 generator pass with the catalogue-test
  update, not in a fix round.

### 13. The `BlobMissing` classification — **CLOSED**

`set_dynamic_state` carries an OPTIONAL blob. `EmitRenderState` sends a version-only update with
`ChunkMask == 0` and zero bytes; the round-2 generic `MGP_WIRE_BLOB` wrapper staged it through
`StageRequired`, which Fatals on a zero count. `Wire_SetDynamicState` is now hand-written and uses
`StageOptional`, like the two sub-data rows; the decoder already took the `ChunkMask == 0` arm.
The classification c1 gives is correct: **it was a c1 round-2 regression** that turned a legal
header-only update into a blob-shape abort and **masked the honest class-C death**.

Confirmed on my joint census: `grep -c 'BlobMissing'` over all 1149 entries → **0**. And the probed
line, which is the answer to "because the slot dies as `Fatal{UnmigratedVerb}` (R-4)":

```
=== CrossFrameBufferScenario.*
    rc=134  first Fatal: Fatal{UnmigratedVerb, "DrawElements"}
```

So the slot now dies at its honest R-4 death, not a blob-shape complaint. c1 is also right that this
is not unit-red-checkable without a session plus an empty-chunk draw; the census probe is the
appropriate evidence and c1 says so.

### 14. The joint numbers — measured, §5 below

### 15. G1 / G5 / generators / push lane — **CLOSED, every number exact**

- **G1**: `.text 10806611 -> 10806611 (+0, +0.000%)`, `.data/.bss/.rodata +0`, total
  `17227007 -> 17227007 (+0)`, `27814 -> 27814 defined symbols: 0 added, 0 removed, 0 resized,
  0 renamed`, rc 0. Corroboration: `nm --defined-only build-linux/libMobileGL.so | grep -ic
  MG_Remote` → **0**, `PipeRoute` → **0**, `build-split` → **606**. Every round-3 change is
  push/disaggregated-only.
- **G5**: `p3a_untouched_regions.sh ff2994d9 HEAD` rc 0 ("the 11 pool / deferred-release / ring /
  flush-drain functions are byte-identical"); `p4a_untouched_regions.sh` rc 0 ("the 17 … regions are
  byte-identical"). Both print the pre-existing ID-41 `FlushPendingRangesFrom` note, which is
  explicitly "not a verdict in either direction". No worktree writes.
- **Generators**: `gen_pipe.py --check` rc 0 "generated files are up to date"; `--self-test` rc 0,
  **9** negative-control trips + positive control; `gen_pipe_field_ownership.py --check` rc 0;
  `check_include_closure.py` rc 0, **4** probes / **0** problems.
- **`ctest -L unit`** re-run by me on all five build dirs: `build-split` **2030/2030**,
  `build-verify-split` **2030/2030**, `build-push` **1816/1816**, `build-verify` **1816/1816**,
  `build-linux` **1816/1816**. 100% in every lane.
- **push `integration-gpu`**: `MOBILEGL_TRANSPORT=monolith ctest -L integration-gpu -j 8` on
  `build-push` → **"100% tests passed, 0 tests failed out of 1128"**, name-for-name.
- **G2/G14 names**: baseline `~/w7/p5-before-ctest-names.txt` **2902**; push **2944**; split
  **3179**; push-vs-baseline **0 removed / 42 added**; split-vs-push **0 removed / 235 added**, of
  which **21** are `DirectGLES.Split.`. Exact.
  *Methodology note (minor N-9):* the `ctest -N | grep -oP '(?<=: ).*'` recipe also matches ctest's
  own `Total Tests: N` footer, injecting one spurious name per list (raw 2945/3180). c1's saved
  lists have the same artefact; it cancels out of every delta, but the recipe should gain
  `grep -v '^Total Tests'`.

---

## 2. New findings, most severe first

### N-1 — major — B1's fix ships no gate and no executed control, and both the report and the shipped source claim one that does not exist

**File:line.** `~/w7/p5-c1-r3-redcheck.py` (whole file, no `&MGPipeApply` grep and no B1 case);
`MobileGL/MG_Impl/Pipe/PipeFill.cpp:1537` ("the grep gate scripts/../p5-c1 redcheck refuses any
`&MGPipeApply` under MG_Impl/"); `c1-v3.md` §1 B1.
**Violates.** R-16 §13 bullet 4 (a gate with no executed red-once line is unverified); ID-58's fix
list item 1; R-17/R-4.
**Claim.** The routing fix is correct but ungated. Nothing in the repo or in the harness refuses
`&MGPipeApply` under `MG_Impl/`, and no test fails when a site is reverted.
**Failure scenario.** The next entry point taken by address — the same shape that produced B1 —
lands on the applier on the GL thread under split and on a silent no-op under spawn, and every lane
stays green exactly as it did in round 2. Measured: reverting the texture site to
`&MGPipeApplyResourceDestroy` leaves `ctest -L unit` at 2030/2030.
**How to confirm.** The perturbation above; and `grep -c 'MGPipeApply' ~/w7/p5-c1-r3-redcheck.py`.
**Fix.** Put the grep in the repo, not in an untracked script — a CMake/CTest case or a line in
`.github/workflows/test.yml` next to the existing `nm` check — and correct `PipeFill.cpp:1537`.

### N-2 — major — codex 4's teardown refusal does not cover the five class-B verbs, so the use-after-free window it was written for is still open on half the table

**File:line.** `MobileGL/MG_Remote/Client/EmitTables.cpp:119` (`RequireSession`, a *second*
function in a *different* TU); `MobileGL/MG_Remote/Client/WireTables.cpp:88`
(`g_clientTablesUninstalled`, in that file's anonymous namespace) and `:97` (the only check);
`MobileGL/MG_Remote/Client/ClientSession.cpp:578-668`.
**Violates.** R-4; CONTRACT §4 table 3's teardown order.
**Claim.** `EmitTables.cpp` has its own `RequireSession` that only checks
`ClientSession::Active() == nullptr`. It cannot see `g_clientTablesUninstalled`. `Stop()` clears
`g_active` only at the very end (`ClientSession.cpp:664`), after the drain, `Shutdown()`, the join,
and after `m_producer.Detach()` / `m_shm.Close()` / the transport resets have freed the rings.
**Failure scenario.** A `Clear`, `DrawArrays`, `BlitFramebuffer`, `ReadPixels` or `Present` arriving
during that window gets a live `ClientSession&` and proceeds into `EmitAndWait` on rings being
freed — a use-after-free, and precisely the window codex 4 named, left open on the class-B half
while the routed half now refuses by name.
**How to confirm.** Mirror `ARoutedCallDuringTeardownRefusesByNameNotRunsTheApplier` for a class-B
slot: install the tables, `UninstallClientWireTables()`, then drive the `Clear` emit slot; require
`Fatal{ClientTablesUninstalled, "Clear"}`. Today it will not abort.
**Fix.** Give the flag a single owner both TUs read (declare it beside `RunsAsTheServerRole()` in
`WireTables.h`, or move the check into a shared `RequireSession`).

### N-3 — major — M2's and M3's production wiring is unobserved; deleting it leaves the 2030-case lane 100% green

**File:line.** `MobileGL/MG_Remote/Client/EmitTables.cpp:391` and `:403`
(the two `RequireReadbackReplyComplete` calls); `:356` (`info.DstSize = tight`);
`MobileGL/MG_Test/Wire/RemoteClientTest.cpp` (`AReplyIsScatteredOnlyWhenItIsOkAndExactlyTheReadsExtent`,
`DstSizeIsTheTightExtentAndNeverThePackedOne`).
**Violates.** R-16 §13 bullets 1 and 4; ID-58's fix list item 4.
**Claim.** Both new readback rules are tested as **pure predicates**. Nothing observes that the
emitter calls them, or that the number it puts in `info.DstSize` is the tight extent.
**Failure scenario.** Measured, twice: deleting both `RequireReadbackReplyComplete(...)` calls →
2030/2030 green; `info.DstSize = tight + 16` (codex 10's own recipe) → 2030/2030 green. A future
edit that drops either is invisible, which is the exact shape codex 10 and 11 were written about.
**How to confirm.** The two perturbations above.
**Fix.** One integration control that drives a real `ReadPixels` under inproc and observes the
emitted `MGPReadbackInfo::DstSize`, plus the verifier's §11 short-reply perturbation at the server
sink requiring `Fatal{ReadbackReplyShort`.

### N-4 — major — M4's escape rule has no control

**File:line.** `MobileGL/MG_Remote/Client/WireTables.cpp:404` and `:451`;
`MobileGL/MG_Test/Wire/RemoteClientTest.cpp` (`AnErrorStatusIsNotFoldedIntoAcceptedOrRefused`).
**Violates.** R-16 §13 bullets 1 and 4; R-5.
**Claim.** The only ERROR control mints a reply slot by hand and drives `MGPipeTakeReplyBool` —
it never invokes `Wire_Escape_ResourceRespecify` or `Wire_Escape_MapPersistent`, which are the two
rows M4 actually changed. The runner has no M4 case.
**Failure scenario.** Measured: deleting both `if (status == 2)` blocks leaves `ctest -L unit` at
**2038/2038, 0 failed**. A future edit that folds ERROR back into `false`/`nullptr` — the exact
defect codex 5 found — is invisible, and a transport fault again reads as "the server said no".
**How to confirm.** The perturbation above.
**Fix.** A fork control per escape: post Status=2 on the escape's own sequence through the installed
route and require `Fatal{ReplyError, "resource_respecify"}` / `"map_persistent"`.

### N-5 — major — five of round 3's fourteen items ship no executed red-once line, and three more arm their control against a stub

**Violates.** R-16 §13 bullets 3 ("a probe may not be armed against a stub; symbol existing ≠
implementation existing") and 4 ("a gate without that line is treated as unverified").
**The audit**, item by item:

| item | control shipped | red-once executed |
|---|---|---|
| B1 | **none** | **no** (claimed, absent — N-1) |
| B3 | yes (count) + fork | yes |
| M1 | none (inert on c1's head, admitted) | n/a — armed only by v1's bracket |
| M2 | predicate only | yes, but not the wiring (N-3) |
| M3 | helper only | yes, but not `info.DstSize` (N-3) |
| M4 | **none for the escapes** | **no** (N-4) |
| M5 | **none** | **no** |
| M6 | meta | yes (I re-executed all three) |
| M7 | n/a (a corrected comment) | n/a |
| M8 | forced `if (true)` probe | yes, but against a stub |
| codex 4 | yes | yes |
| codex 6 | yes | yes |
| codex 12 | **none anywhere** | **no** |
| BlobMissing | census probe only | no (stated) |

M1, M5 and the `BlobMissing` classification are defensible — c1 gives an honest reason for each and
the owner of the arm is another package. **B1, M4 and codex 12 are not**: all three are c1-owned,
all three are one fork test away, and all three are reported as fixed without the line the brief
requires.

### N-6 — minor — B3's case asserts a count, not identity

33 differing cells + 4 differing escapes. A dropped routed row combined with an accidentally
overwritten unrouted row still sums to 33. Asserting the routed rows by name (or asserting the
complement is unchanged) closes it. Not urgent — the realistic defect is a dropped row, which is
caught.

### N-7 — minor — M4's ShutDown→DECLINED makes a teardown that races a readback abort by a new name

`RequireReadbackReplyComplete`'s decline arm is a `Fatal{ReadbackDeclined, "ReadPixels"}`, so the
"teardown legitimately reaches here" comment is now true for the acceptance rows and false for
`ReadPixels`. Arguably right (a readback has no pixels to return and must not hand back stale
bytes), but it should be *said*: the comment at the ShutDown arm claims teardown no longer aborts.

### N-8 — minor — the joint merge with v1 round 2 is not conflict-free

`git merge p5/v1` from `5682f429` conflicts in `MobileGL/MG_Test/Wire/CMakeLists.txt`: c1 refactored
the three per-suite blocks into one `foreach (wiretest IN ITEMS RemoteClientTest ServerLoopTest
SessionHandshakeTest)` loop, and v1 independently appended its own `ServerLoopTest` block. The
resolution is trivial — take c1's loop, which already registers v1's target with identical include
dirs, link libraries, MSVC option and `LABELS unit`, so nothing is lost — but ID-59 recorded the
previous joint as conflict-free and ID-61's `jm` package assumes a clean merge order. Worth one
line in the merge runbook. (c1's own comment in that file predicts exactly this conflict.)

### N-9 — minor — the G2/G14 name recipe counts ctest's own footer

See item 15. Cancels out of every delta; fix the recipe.

---

## 3. Claims in `c1-v3.md` the code does not support

1. **§1 B1: "Gate: the red-check refuses any `&MGPipeApply` under `MG_Impl/` (grep, now 0)."**
   **False.** There is no such grep in `~/w7/p5-c1-r3-redcheck.py`, nor anywhere in the repo. The
   same false statement is shipped in source at `PipeFill.cpp:1537`. (N-1)
2. **§1 B1: "Red once: revert one site to `&MGPipeApplyResourceDestroy` → …"** The runner has no B1
   case, so this line was never executed; and when I execute it, no test names it. (N-1)
3. **§Intro: "every gate ships its red-once line."** Five items ship none (B1, M4's escape half, M5,
   codex 12, and the `BlobMissing` classification, the last stated openly). (N-5)
4. **§1 M3: the implication that one shared body closes codex 10a.** It closes the helper half;
   codex 10's own recipe (`info.DstSize = tight + 16`) still passes. (N-3)
5. **§1 M8 / §2: the M8 control.** It forces the probe to `if (true)`; it does not bind a pack PBO,
   so it cannot show the probe expression detects a real binding. The behaviour is nevertheless
   correct — I verified it with the control ID-57 actually specified. (N-5)
6. **§4: "Joint `integration-split` under inproc … 14 / 21."** This is c1's **own head** plus the
   `Init.cpp` one-liner, not a joint with v1 at all (`git merge-base --is-ancestor c9d84e33
   5682f429` → NO). Labelling it "joint" is misleading; the real joint is 21/21 (§5). The seven were
   correctly attributed to v1's flush seam.
7. **§1 codex 12: no red-once line, and no test anywhere names `MakeEGLCurrent`.** The change looks
   right; it is unverified. (N-5)

Everything else in `c1-v3.md` that I checked was accurate, including the three §3 report
corrections (the 1128/1149/19/7 tangle, "after an ATTEMPTED caps pump", and
`ClientWireRecordsEmitted`), the census triage, and all of §4's self-test numbers.

---

## 4. Verified as claimed — do not re-run

- The five address-of sites, and the three `MGPipeRoute*` delete functions now having callers.
- The `kReplySlot` row set, and that `ResourceFlushRange` / `CreateShaderState` own no slot.
- `RunsAsTheServerRole()` moved to `WireTables.h`; `PipeFill` includes no server header.
- `ReinstallMonolithAfterTeardown()` on **both** Stop paths (closes m2).
- All of §4's self-tests: G1 0/0/0/0, five unit lanes, push 1128/1128, G5, the four generator
  checks, G2/G14 name deltas. Every number exact (item 15).

---

## 5. The joint with v1's round 2 — measured

Scratch branch `p5/c1rev3-joint` = `5682f429` + `git merge p5/v1` (`ca92308b`, which contains v1
round 2 `c9d84e33`; the only commits on top are `c3e736a3`/`39292958`, already in c1) + the two
merge-time hunks (`Init.cpp` direct construction; `ScopedApplierEntry` at the top of
`PipeApplier::ApplyOne`) + the one-file conflict resolution of N-8. Joint head `45dff8b9`. Build
rc 0.

| lane | result |
|---|---|
| `integration-split`, inproc | **21 / 21** — "100% tests passed, 0 tests failed out of 21" (19 run + 2 `PersistentMapArm` counting entries skipped) |
| `integration-split`, inproc + `MOBILEGL_IPC_AUDIT=1` | **21 / 21**, same two skips |
| `integration-split`, inproc + `MOBILEGL_IPC_VERB_BARRIER=0` | **"100% tests passed"** — but **all 21 SKIPPED**, 0 aborted, 0 failed, `BarrierViolation` 0 times in ctest output |
| inproc `integration-gpu` census (1149) | **426 passed / 185 skipped / 511 aborted / 27 failed / 0 segfault**; "53% tests passed, 538 tests failed out of 1149" |

**c1's flush-seam attribution is confirmed:** the seven `PersistentCoherentMapScenario` entries that
were red on c1's own head are green once v1's `05bcb734` is in. **The three segfaults c1 saw are
gone** (v1's neutral-pack `61d6cd8e`): `PixelStoreSweepScenario` now exits rc 0 with no Fatal.

**The barrier-off lane is a false green** — ID-62's N-1, live here. The Fatal *does* fire, in the
forked pre-flight child (item 3 above), so `ScenarioFixture.h:111` skips every entry and ctest
reports success. It is j0's control defect, not c1's, and c1's string is the thing that makes the
attribution possible at all.

**First failure per family, probed with a private `MOBILEGL_LOG_FILE_PATH`:**

| family | first diagnostic | attribution |
|---|---|---|
| `CrossFrameBufferScenario` | `Fatal{UnmigratedVerb, "DrawElements"}` | class-C by design (CONTRACT §7) |
| `P4aFinalFixScenario` (6) | `Fatal{UnmigratedVerb, "GenerateMipmap"}` | class-C by design |
| `CopyImagePacked16Scenario` (6) | `Fatal{UnmigratedVerb, "CopyImageSubData"}` | class-C by design |
| `DepthStencilReadbackMatrixScenario` (3) | `Fatal{UnmigratedVerb, "CopyTexImage2D"}` + assertion at `:530` | class-C + P4b/P7 debt |
| `LayeredAttachmentShapeScenario` (14) | no Fatal; assertion at `LayeredAttachmentShapeScenario.cpp:507` | P4b/P7 debt (wrong-but-non-fatal) |
| `PixelStoreSweepScenario` | rc 0, no Fatal — **passes** | was v1's ID-49 segfault; fixed |
| `HandleRecycleScenario` (5) | assertion failure, no Fatal | shared/P4b debt — these are the entries the `BlobMissing` fix let run past `set_dynamic_state` |
| `PrimitivesGeneratedNoXfbScenario` (3) | assertion failure | P4b/P7 debt |
| `TextureParamsWithoutASamplerViewScenario` (1) | assertion failure | P4b/P7 debt |

`BlobMissing` **0**; `InitialBytesNotCarried` **0**; `BarrierViolation` 0 outside the deliberate
barrier-off probe. **None of the 538 is c1's.**

---

## 6. Verdict

**MERGEABLE**, with N-1/N-2/N-4 booked as follow-ups — **not** a package split.

ID-58's contingency is "if round 3's reviews still find a **blocker**, the client package is split".
I found none. Every runtime fix in round 3 survives adversarial perturbation:

- B1's routing is complete and the split lane no longer reaches the applier on the GL thread;
- B3's gate is real and goes red on its own message, including the escape half c1's own control
  did not exercise;
- M1's string is armed by v1's bracket and readable from a private log — the thing ID-53 needs;
- M8 refuses a **real** bound pack PBO before any dereference, which is ID-57's own control and
  which c1 never ran;
- the readback rules, the escape ERROR rule, the role gate and the teardown refusal are all
  correct where I could drive them;
- M6's runner, the one thing round 2 got structurally wrong, is now sound under all three
  perturbations codex 8 specified;
- and the joint is the number the phase was waiting for: **`integration-split` 21/21**, audit
  21/21, census **0 segfaults**, nothing in the tail attributable to c1.

What is wrong is the **gating**, and it is wrong in a way that is cheap to fix and expensive to
leave: N-1 (B1 ungated, and claimed gated in both the report and shipped source), N-2 (the class-B
half of the teardown window), N-3/N-4 (production wiring for M2/M3/M4 that deletes clean).
Under R-16 §13 those are unverified gates, not unverified behaviour — I verified the behaviour
myself. Splitting the package would not help: the two halves ID-58 names (routing/catalogue vs
readback/mailbox) each contain some of these, and every one of them is a test-side addition of a few
lines, not a redesign.

**Required before the joint merge is called done (all small, all c1's):**

1. **N-1** — move the `&MGPipeApply` grep into the repo (a CTest case or a `test.yml` step beside
   the existing `nm` check), add the B1 case to the runner, and correct the false sentence at
   `PipeFill.cpp:1537` and in `c1-v3.md` §1 B1.
2. **N-2** — one owner for `g_clientTablesUninstalled` that `EmitTables.cpp`'s `RequireSession`
   also reads, plus the class-B mirror of the teardown control.
3. **N-4** — a fork control per escape for the Status=2 rule.
4. **N-3** — an integration control that observes the emitted `DstSize` and the short-reply refusal.
5. **codex 12** — any control at all, or say plainly in the report that it is unverified.

**Accepted as judgment calls:** m1's deferral (§1.12), M1's and M5's missing controls (the arm
belongs to another package and c1 says so), codex 7's placement point going to p1, and the
`BlobMissing` classification resting on the census probe.

**For the integrator, not c1:** N-8 (the one-file merge conflict) and ID-62's N-1 (the barrier-off
lane is a false green because the pre-flight child dies — j0's).
