# v1-review-v1 — adversarial review of package v1 (`5e5bf7b9..571cbabb`, `p5/v1`)

Reviewer: Claude (Opus 5), read-only on `~/w7/pipe`, perturbations run and reverted in `~/w7/p5-v1`
(`git status` clean at `571cbabb`, submodule links back `off`), re-runs only in `~/w7/p5-v1-joint`.
Counts: **0 blocker / 7 major / 7 minor**.

Perturbation scripts kept because they are cited: `~/w7/p5-v1rev-patch.py`, `~/w7/p5-v1rev-patch2.py`.

---

## Findings, most severe first

### M-1 — R-11, the package's headline deliverable, has no check that fails when its production wiring is deleted (major)

**Ruling violated:** BRIEF §13 / §11.11 (R-16): "一条断言不得构造它要观察的状态"; "一条探针不得对着桩武装".
Also ID-46's premise (findings 5–10: tests that stay green with the code they name deleted).

**Claim:** every R-16 gate v1 ships for R-11 perturbs `StagedShadowStore` itself, which the test also
constructs; not one of them names the four `Managers.cpp` call sites that are the actual R-11 change.

**Failure scenario:** a later refactor drops `MGL_SERVER_STAGED_ADOPT` from `Ops_H_SubData` — the exact
rule-C violation R-11 exists to fix — and every gate in the tree stays green.

**Confirmed (ran, then reverted):** `python3 ~/w7/p5-v1rev-patch.py r11-subdata-site-removed apply`
(`Managers.cpp:2117-2118` → `resource->hostBytes = static_cast<const Uint8*>(bytes) - offset;`) plus
`require-pending-coverage-removed` (`Managers.cpp:1058` loop body → `continue`, neutering
`RequireStagedCoverageForPendingRanges` at both call sites); `cmake --build build-split -j 24` rc=0;
`ctest -L unit -j 8`:

```
99% tests passed, 2 tests failed out of 1985
	1859 - RingTest.TheTwoFlagSpacesAreDisjointByTranslation (Failed) unit
	1947 - PipeWireCodecTest.NoReplyIsWrittenForARowTheCatalogueGivesNoReplySlot (Failed) unit
```

i.e. exactly the two known reds (ID-43, ID-31) and **all eleven of v1's own cases green**. Reverted;
rebuild + `ctest -R 'ServerLoopTest|StagedShadowTest'` back to 11/11.

**Why the suite cannot see it:** `ServerLoopTest.cpp:509-535` and `:540-559` build their own
`StagedShadowStore`, their own `Vector<Uint8> staged(32, 0xAB)`, and hand-poison it with
`std::fill(staged.begin(), staged.end(), 0xDD)` — w1's poison is simulated, not used. The redcheck's
`r11-copy-removed` and `coverage-widened` both patch `StagedShadow.h`
(`~/w7/p5-v1-redcheck.py`, CASES entries 9 and 10), i.e. the class the case constructs.

**Fix shape:** one case that drives `Ops_H_SubData` (or the `MGL_SERVER_STAGED_ADOPT` expansion) and
asserts `ServerStaged().TrackedResources()`/`IsCovered` moved, plus a redcheck entry anchored in
`Managers.cpp`.

---

### M-2 — the end-to-end R-11 control (`MOBILEGL_IPC_AUDIT=1`, "no 0xDD reaches a draw") cannot distinguish R-11 present from R-11 absent (major)

**Ruling violated:** R-2.5 / R-11 ("对照见 R-2.5 的 0xDD 毒化 … 这是 §1.9 与 class 3 的唯一机械对照"),
BRIEF §13 (a probe may not arm against something that cannot answer).

**Claim:** on the reduced path under `inproc`, nothing ever dereferences `resource->hostBytes`, so the
audit lane is green whether or not the copy exists.

**Code:** `Managers.cpp:2928-2931`

```cpp
const auto liveHostBase = [&resource, &bufferObject]() -> const Uint8* {
    if (bufferObject) return bufferObject->MappedData();
    return resource->hostBytes;
};
```

Both callers of `EnsureBufferResourceForHandle` hand over a live frontend object — `Managers.cpp:3090`
(from `EnsureBufferResource`, which returns early on a null object at `:3075`) and `Managers.cpp:7692`,
whose `buffer` is dereferenced two lines earlier at `:7676`. Under `inproc` the client's
`BufferObject` is in the same process, so the draw-time drain always reads `MappedData()`. The only
readers of the server shadow are `Ops_H_Readback` (`:2267`) and `Ops_H_FlushRange`'s tier-1
invalidating map (`:2211`), and the 21 split entries (clear / blit / VBO triangle / coherent map)
reach neither.

**Corollary, and it is the sharper half:** `MGL_SERVER_STAGED_REQUIRE_PENDING(*resource, hostBase,
size, "ensure_flush_pending")` at `Managers.cpp:3059` is **dead under the only transport P5 ships** —
`hostBase` is `MappedData()`, so `StagedShadowStore::RequireCoverage` returns at `StagedShadow.h:104`
on `it->second.Bytes.data() != hostBase`. v1-v1 §1/§5.2 says the clamp is "checked at its two
server-shadow call sites"; one of the two can never fire.

**How to confirm:** M-1's perturbation already shows the unit lane blind. The audit lane itself I could
not re-test (see "what I could not verify"), but the call-graph above is mechanical: add a counter to
`StagedShadowStore::Adopt` and run the 21 split entries — if it is zero, the audit row proves nothing.

---

### M-3 — two whole-store readers of the same base are not covered by the M-6 refusal (major)

**Ruling violated:** ID-37 / the integrator's M-6 ruling as v1 itself restates it in
`StagedShadow.h:20-26`: "a drain that reaches outside it is Fatal rather than a re-read of whatever
happens to be there."

**Claim:** `StagedShadowStore::Adopt` zero-fills the shadow (`StagedShadow.h:72`,
`shadow.Bytes.resize(needed, 0)`), and two sites upload the *whole* store from that base with no
coverage check, so never-staged bytes are silently uploaded as zeroes instead of being a Fatal.

**Code:**
* `Managers.cpp:2980` — `g_GLESFuncs.glBufferSubData(TempBufferTarget, 0, (GLsizeiptr)poolSize, liveHostBase());`
* `Managers.cpp:970` — `g_GLESFuncs.glBufferData(TempBufferTarget, (GLsizeiptr)size, initialData, usage);`
  reached from `:3055` and `:3065` with `initialData = shadowHasContent ? hostBase : nullptr` (`:3052`).

**Failure scenario (spawn, or any future handle-only ensure):** a buffer receives one
`resource_subdata` for `[0,64)` of a 4 KiB store, then the pool-reuse arm at `:2965-2980` uploads
4 KiB from the shadow — 4032 zero bytes over live content — with no diagnostic. That is precisely the
"silent re-read" shape `Managers.cpp:1126-1129` is the scar tissue for.

**Why v1 missed it:** v1-v1 §5.2 scopes the search to `grep FlushPendingRangesFrom(` ("the other two
pass `MappedData()`"). Both sites above read the same base without going through that function, so the
grep could not find them. The four `FlushPendingRangesFrom` callers are indeed `:1885`, `:2267`,
`:3060`, `:3198` and the two unguarded ones do pass `MappedData()` — the answer to that question is
"yes", but the question was the wrong one.

---

### M-4 — the affinity gate cannot fail for its own reason (major)

**Ruling violated:** CONTRACT §5 (`MOBILEGL_IPC_SERVER_AFFINITY`: "whoever starts the apply thread logs
the **resolved mask**, because an affinity that silently did nothing looks exactly like one that
worked"); BRIEF §13.

**Claim:** `ServerLoopTest.TheAffinityStringResolvesToAMaskAndTheMaskIsLogged` asserts only
`ResolvedAffinityMask() == 0` after setting `ServerAffinity = "off"` and that the literal
`"RESOLVED mask"` appears in the log. Zero is also the answer for `auto` on any box without a cpufreq
tree (`ServerLoop.cpp:65`), for an unrecognised string (`:91`), for an empty mask (`:117`) and for a
failed `sched_setaffinity` (`:118`). So the case is satisfied by the environment, not by the code.

**Confirmed (ran, then reverted):** `python3 ~/w7/p5-v1rev-patch.py affinity-off-is-auto apply` —
`ServerLoop.cpp:86` `if (std::strcmp(raw, "off") == 0) return 0;` →
`... return DetectBigCoreMask();`. Rebuilt `build-split` (rc=0):

```
1/1 Test #1978: ServerLoopTest.TheAffinityStringResolvesToAMaskAndTheMaskIsLogged ...   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

The only branch the case names can be deleted and it stays green. `DetectBigCoreMask()` and the
explicit-numeric arm (`:88-93`) are exercised by no case at all, and the redcheck entry
`resolved-mask-not-logged` only renames the literal the case greps for — a tautology, not a control.

**Fix shape:** set `ServerAffinity` to an explicit mask the box can honour (e.g. `"0x3"`) and assert
`ResolvedAffinityMask() == 0x3` **and** that the logged mask equals it; keep `off` → 0 as the second
half, so the two answers must differ.

---

### M-5 — 28 wrong-answer failures on the split path are undiagnosed, and §8's "what passes" list is wrong (major)

**Ruling:** BRIEF §4 limits the *gate* to the reduced path; it does not license shipping 28 silent
wrong answers without triage, and R-8 ("right by accident under inproc") is the reason the lane exists.

**Claim:** the 28 `(Failed)` entries in `~/w7/p5-v1-joint-inproc.log` are **assertion** failures (wrong
pixels / wrong counts), not `Fatal{UnmigratedVerb}` aborts, and they are introduced by the split path.
v1-v1 §8 attributes only the 506 aborts and says nothing about them.

**Confirmed (ran):** on the joint build, same binary, same entries —

```
$ MOBILEGL_TRANSPORT=monolith ctest -R "DirectGLES.LayeredAttachmentShapeScenario|DirectGLES.HandleRecycle.Legacy.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin|DirectGLES.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture|DirectGLES.P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact"
100% tests passed, 0 tests failed out of 11

$ MOBILEGL_TRANSPORT=inproc ctest -R "...HandleRecycle.Legacy...|DirectGLES.LayeredAttachmentShapeScenario.LayeredThreeDColorAttachmentReachesEverySlice"
LayeredAttachmentShapeScenario.cpp:507: Failure  (x4)
HandleRecycleScenario.cpp:1369: Failure   Expected equality of these values:  Which is: 16
HandleRecycleScenario.cpp:1377: Failure   Expected equality of these values:  Which is: 16
```

Both fail only under `inproc`. Families: `LayeredAttachmentShapeScenario` (14, both backends),
`HandleRecycleScenario.*` (4), `DepthStencilReadbackMatrixScenario` (3),
`PrimitivesGeneratedNoXfbScenario` (3), plus `TextureParamsWithoutASamplerViewScenario`,
`P4aFinalFixScenario`, `PersistentMapArm`. The DirectVulkan half means they are not a DirectGLES
backend quirk.

**Also:** v1-v1 §8 states "What passes is … `HandleRecycle.*`". In the log one `HandleRecycleScenario`
entry passes, **17 skip** and **4 `HandleRecycle.*` variants fail**.

**Note for the integrator:** these are not necessarily v1's — c1 owns the emit table — but they are the
tree v1 proposes to merge, and nobody has owned them.

---

### M-6 — `ShutdownSplitRoles` never stops the apply thread or drops the server backend on any path where `ClientSession::Start` failed early (major)

**Ruling violated:** CONTRACT §4's teardown order (steps 2–3, Kill then bounded join) and charter item 8
("Nothing added in P5 may start a destructor chain that runs after the transport closes").

**Claim:** `MG_Backend/Init.cpp` `ShutdownSplitRoles()` delegates the whole of table 3 to
`ClientSession::Stop()`, and that function's `!m_started` arm (`ClientSession.cpp:345-362`) closes the
shared mapping, closes the `ServerSession` and resets both transports **without ever calling
`ServerLoopInstance().Stop()`**.

**Failure scenario (reachable today on `p5/v1`):** `InitSplitRoles` step 1 succeeds
(`ServerLoop::CreateBackend` → `BackendObject_DirectGLES::Initialize()`, which registers the process-wide
resource op table at `BackendObject_DirectGLES.cpp:849`), step 3 `ClientSession::Start` fails before
`m_started = true` (e.g. `:259` `!m_cmd.Valid()`, `:273` `!m_replies.Valid()`, or a refused `Accept`).
`InitSplitRoles` returns false, `Init()` returns with `pActiveBackendObject = nullptr`. The server's
`BackendObject_DirectGLES` is now permanently alive with `g_resourceOps` pointing into it, and
`ServerLoop::CreateBackend`'s `if (m_backend != nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT`
(`ServerLoop.cpp:148-150`) makes every later split bring-up in that process fail. `MobileGL::Destroy()`
then reaches the same `!m_started` arm and still does not drop it.

**Second scenario (the one to watch at merge):** v1-v1 §9.1 requires c1 to restore `m_started = true;`
*before* `g_active = this;` in step 7. If c1 puts it after `ServerLoop::Start` instead, the same arm
unmaps SEG_CMD (`m_shm.Close()`, `:356`) under a **running** apply thread whose park predicate reads
`control.cmdHead` (`ServerLoop.cpp:257`) — a use-after-free with no join anywhere. Nothing in the tree
enforces the placement; it lives only in v1's prose.

**How to confirm:** no test covers `ShutdownSplitRoles` at all (grep: the identifier appears only in
`MobileGL/Init.cpp`, `BackendObjects.h`, `MG_Backend/Init.cpp`).

**Fix shape:** `ShutdownSplitRoles` calls `Server::ServerLoopInstance().Stop()` itself after
`ClientSession::Stop()` returns — `Stop()` is already idempotent and its `!joinable` arm exists for
exactly this.

---

### M-7 — `RunOnApplyThread`'s "no thread" arm is a silent EGL-on-the-app-thread fallback (major)

**Ruling violated:** R-1 / CONTRACT §4's `pActiveBackendObject` row ("under split it must be a
**blocking** request onto the apply thread"); charter item 2.

**Code:** `ServerLoop.cpp:428`

```cpp
if (!m_running.load(std::memory_order_acquire)) return work(user);
```

**Claim:** the comment says "It is NOT a silent fallback to monolith", and then it is exactly that —
there is no log, no counter and no assertion. If `ServerLoop::Start` refused (`!session.Accepted()`,
`ServerLoop.cpp:184-188`) or `std::thread`'s constructor threw, every one of the twelve forwarders runs
`backend->MakeEGLCurrent(dpy, draw, read, ctx)` **inline on the app thread** (`ServerLoop.cpp:611`),
stamps `g_backendContextOwnerThread` with the app thread, and the phase's entire premise is gone with
no line saying so. Compare `CreateBackend`'s `default:` arm (`:158-164`), which refuses by name.

**Not covered by the redcheck:** `control-runs-on-caller` perturbs the *other* inline arm
(`if (OnApplyThread())`, `:422`).

**Same function, narrower race (minor, folded here):** `ApplyThreadMain` answers a pending request and
notifies at `:310-320`, then stores `m_running = false` at `:322`. A caller that reads `m_running ==
true` at `:428` in that two-statement window posts into a mailbox nobody will pump and blocks forever
on `m_controlDone.wait` (`:446`). `Stop()`'s bounded join has already been satisfied by then, so there
is no `Fatal{ApplyThreadJoinTimeout}` either.

**Fix shape:** `MGLOG_E_ONCE` naming the call and the reason on the `!m_running` arm, plus a counter a
lane can assert is zero; and fold the `m_running = false` store into the same `m_controlMutex` critical
section as the drain-the-mailbox block.

---

### m-1 — `present ↔ eglSwapBuffers 1:1` is asserted to be "structural" and is not (minor)

`PipeApplier.cpp:149-154`: "the swap is the client's EGL call and crosses as the SwapEGLBuffers control
request, **which runs Present on this thread through this same table**. Both paths therefore end here
and the 1:1 is structural."

`ServerSwapEGLBuffers` (`ServerLoop.cpp:635-648`) calls `backend->SwapEGLBuffers` →
`BackendObject_DirectGLES::SwapEGLBuffers` (`BackendObject_DirectGLES.cpp:972-974`) →
`BackendObject::SwapEGLBuffers` (`BackendObject.cpp:369ff`), which validates the current thread's
surface and calls `eglSwapBuffers`. It never calls `Present()`. The only caller of `Present()` is
`ServerVerbSink::OnPresent` (`PipeApplier.cpp:155`). ARCHITECTURE.md:531's 1:1 therefore rests entirely
on c1 emitting exactly one `Present` record per swap, and nothing compares `Verbs().Presents()` to a
swap count. Consequence if it drifts: the frame fence and `TrimBufferPool`'s recycle watermark
(the reason the comment gives for caring) stop tracking frames.

### m-2 — `EXPECT_DEATH(..., "")` — ID-46 finding 10's shape, recurring (minor)

`ServerLoopTest.cpp:594`: `EXPECT_DEATH(store.RequireCoverage(&key, base, 0, 64, "unit_out_of_range"), "")`.
The empty regex accepts any death, including a segfault inside `RequireCoverage`. It is *partly*
compensated by the log grep at `:596` (`Fatal{StageSnapshotTooNarrow, "unit_out_of_range"}`), which is
why this is minor rather than major — but v1-v1 §4's "Every control asserts its own string" is not true
of the matcher, and the compensating grep depends on `MGLOG_F` flushing before `abort()` in a forked
child, which nothing pins.

### m-3 — `ServerLoop::Start` does not reset its own diagnostics; the suite passes only because ctest gives each case a process (minor)

`ServerLoop::Start` (`ServerLoop.cpp:180-198`) resets `m_stopRequested`, `m_exited` and `m_running` but
not `m_drained` / `m_parks`. `ServerLoopTest.cpp:365` asserts `DrainedRecords() == 1u` and `:398`
asserts `== 8u`; both are absolute.

**Confirmed (ran):** `build-split/MobileGL/MG_Test/Wire/./ServerLoopTest` with no filter —

```
../MobileGL/MG_Test/Wire/ServerLoopTest.cpp:398: Failure
Expected equality of these values:
  Server::ServerLoopInstance().DrainedRecords()   Which is: 9
  8u                                              Which is: 8
[  FAILED  ] ServerLoopTest.TheSessionWatermarkAndTheDecoderTallyAgreeAfterEveryRecord
```

Two consequences: a developer running the binary directly gets a red CI never shows, and `ServerLoop`'s
diagnostics are not per-session — which matters the first time a process opens a second session
(context loss, `MobileGL::Initialize` after `Destroy`).

### m-4 — the doorbell claim in the header and in `Fatal{ApplyThreadJoinTimeout}` is wrong (minor)

`ServerLoop.h:24-29` and `ServerLoop.cpp:490-496` both state that `CondVarDoorbell::Kill()` is the only
thing that can break a `kWaitForever` park, and the Fatal tells the operator "this is a lost wakeup and
not a slow thread". `Doorbell::Wait` re-tests `ready()` after every `Park` return
(`Doorbell.h:165-168`), `Stop()` publishes `m_stopRequested` (`:468`) **before** it rings (`:473`), and
`m_stopRequested` is in the predicate (`:258`) — so a plain `Notify` is sufficient for the stop case.
Kill is only load-bearing for a predicate with nothing to see (which is why the
`park-predicate-loses-control` redcheck entry does time out, correctly). The Fatal will mis-diagnose
the next real occurrence.

### m-5 — `MGL_SERVER_STAGED_DROP_ALL()` frees every shadow without nulling the `hostBytes` that name them (minor, latent)

`Managers.cpp:2766` drops every shadow on context death. The other two drop sites pair the `Drop` with
something that makes the stale base unreachable — `Ops_H_Destroy` (`:2309-2311`) retires the twin
itself, and the map-persistent site (`:2401-2402`) nulls `resource->hostBytes` on the very next line.
`DropAll` does neither: the twins survive, and `EnsureBufferResourceForHandle`'s
context-generation block (`:2933-2945`) resets id,
storage and `pendingRanges` but leaves `hostBytes` pointing at freed memory. Today unreachable:
`Ops_H_Readback` returns at `:2238` and `Ops_H_FlushRange` at `:2069-2075` on the generation test, and
`liveHostBase()` prefers `MappedData()` whenever a frontend object exists — which under `inproc` is
always (see M-2). Under spawn it is a use-after-free in `glBufferData(..., initialData, ...)`
(`:970` via `:3052`, `:3055`).

### m-6 — `AssertConsumerMaskIsHonest` runs before the client half exists (minor)

`MG_Backend/Init.cpp`: the check is step 2; `pActiveBackendObject = CreateRemoteBackendObject()` is step
4 and `InitSpecificBackendLibs()` then calls `pActiveBackendObject->Initialize()`. Anything the client's
object registers into `MGPipeSetResourceOps` afterwards is invisible to the check — and a
client-registered `g_resourceOps` is the same infinite-loop shape `PipeApplier.h:103-106` names for
`gBackendFunctionsTable` (the applier would dispatch a resource record back into the emit table).
Re-asserting after step 4 costs one call.

### m-7 — `OnReadPixels` writes the client's answer before the stamp is cleared and before the bracket drops (minor, latent)

`PipeApplier.cpp:198` posts the reply; `:323` calls `LeaveApplier()`; the joint tree's
`ScopedApplierEntry` (v1-v1 §5.1) drops later still. This is safe **only** because `ReplySlot.h:37-42`
rules that the client reaches a slot through `appliedSeq` and never by polling the slot's own stamp —
a sentence in another package's header, asserted nowhere. If any reply path ever waits on the stamp,
the client resumes while the apply-side `gPipeInputs` flag is still up, which is the one thing
CONTRACT §4's `gPipeInputs` row forbids. Cheap defence: post the reply after `LeaveApplier()`, or
assert in `ReplyPool::PostReply` that the stamp is written before the watermark moves.

---

## Claims in `v1-v1.md` the code does not support

1. §8: "**What passes is what the six-verb reduced path plus the deferred handle arm can serve:
   `OrientationScenario` (11/11 DirectGLES), `HandleRecycle.*`, …**" — in
   `~/w7/p5-v1-joint-inproc.log` one `HandleRecycleScenario` entry passes, 17 skip, and four
   `HandleRecycle.*` variants are among the 28 failures (M-5).
2. §4: "**no case constructs the state it observes** (records go in through w1's encoder over a real
   `ShmSegment` session … nothing writes a record field by hand)" — true of the `ServerLoopTest` half,
   false of the `StagedShadowTest` half: `ServerLoopTest.cpp:514-526` builds the store, the bytes and
   the 0xDD by hand; `:543-556` likewise; and `TheAuditPoisonFills…` hand-writes `MGPResourceDesc` and
   `MGPSubData` fields at `:445-466` (b1's ID-42 shape, mitigated by the encoder/decoder being real).
3. §4: "**Every control asserts its own string**" — `EXPECT_DEATH(..., "")` at `ServerLoopTest.cpp:594`
   (m-2).
4. §1/§5.2: "**the three-tier drain's queue is checked at its two server-shadow call sites**" and "the
   three clamp lines are spelled twice" — the `ensure_flush_pending` spelling (`Managers.cpp:3059`)
   can never fire under `inproc`, because `hostBase` there is `MappedData()` and `RequireCoverage`
   returns at `StagedShadow.h:104` (M-2). And two whole-store readers of the same base are unchecked
   (M-3).
5. §2 acceptance row: "**`MOBILEGL_IPC_AUDIT=1` on the reduced path, no 0xDD reaches a draw** … 19/21
   green under audit" — the lane result is real, the inference is not: nothing on that path
   dereferences `hostBytes`, and the whole 1985-case split unit lane stays green with the R-11 copy
   deleted (M-1, M-2).
6. `PipeApplier.cpp:149-154`: "**Both paths therefore end here and the 1:1 is structural**" —
   `SwapEGLBuffers` never reaches `Present()` (m-1).
7. §1 / `ServerLoop.h:24-29` / `ServerLoop.cpp:490`: "**`CondVarDoorbell::Kill()` is the only thing that
   can un-park a `kWaitForever` waiter**" — not for the stop case, because the predicate carries
   `m_stopRequested` and `Stop()` publishes it before ringing (m-4). (The charter says this too; the
   code has since made it untrue, which is the better outcome — but the Fatal still says it.)
8. §5.2: "**Overturned if a THIRD caller ever passes a server shadow (grep `FlushPendingRangesFrom(`)**"
   — the grep is sound (4 callers: `:1885`, `:2267`, `:3060`, `:3198`; the two unguarded ones pass
   `MappedData()`), but it is the wrong question: the unguarded server-shadow readers do not go through
   that function (M-3).
9. §11 "everything else with the `p5-v1-` prefix was deleted" — `~/w7/p5-v1-joint-preflight.log` and the
   other kept files are present as listed; no discrepancy found. (Checked, no finding.)

---

## Verified as claimed (so the integrator does not re-run them)

* **G1.** `python3 scripts/symbol_report.py --before ~/w7/p5-before-libMobileGL.so --after
  ~/w7/p5-v1/build-linux/libMobileGL.so --threshold 0` →
  `.text 10806611 -> 10806611 (+0, +0.000%). 27814 -> 27814 defined symbols: 0 added, 0 removed,
  0 resized, 0 renamed`. `nm --defined-only build-linux/libMobileGL.so | grep -ci MG_Remote` = **0**;
  `grep -ci 'ShutdownSplitRoles\|StagedShadow\|ServerLoop\|PipeApplier'` = **0**; split: MG_Remote 426,
  `ServerLoop` 24. Nothing v1 added is reachable from the pull build.
* **The joint split lane.** `ctest -L integration-split` → 21 entries, 19 pass, 2 skip. Both skips are
  t1's lane gate, not v1's probe declining: `PersistentCoherentMapScenario.cpp:390: Skipped — not the
  counting lane: MGITEST_PMAP_LANE is set only by the *.PersistentMapArm.* entries`.
* **`MOBILEGL_IPC_VERB_BARRIER=0`** → 19 of 21 red (18 `Subprocess aborted`, plus
  `AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation` as a plain `Failed`), matching
  §8's split of one assertion-red and eighteen `Fatal{BarrierViolation}`.
* **`MOBILEGL_IPC_STRICT_ERRORS=1`** → 19 aborts; direct run with a log file gives the string verbatim:
  `[Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetTextureContextId@Clear"}
  [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P3b/P4b]` — on the apply thread, as claimed.
* **No default path skips the barrier wait.** Joint `ClientSession::EmitAndWait` (`ClientSession.cpp:664-698`)
  waits on `appliedSeq` for every verb; only `MOBILEGL_IPC_VERB_BARRIER=0` short-circuits, and even then
  only for rows without `kReplySlot` (`:670-679`). The one other early return is
  `SessionWait::ShutDown` on a dead doorbell (`:683-691`), which is teardown and is named.
* **`appliedSeq` is not batched and the stamp clear is before the publish.**
  `SessionConsumer::ApplyOne` (`SessionRings.h:385-388`) is `apply(view); ++m_appliedSeq;
  AdvanceApplied; PublishApplied`, and `PipeApplier::ApplyOne` does `StampVerbBoundary → DecodeAndApply
  → LeaveApplier` (`PipeApplier.cpp:304-324`), all inside the callback. `m_drained.fetch_add` is inside
  the callback too (`ServerLoop.cpp:373`). I found no third member of the §6 family in v1's code: the
  only post-publish work in `DrainRing` is the tally-agreement Fatal (`:393`) and `RetireThrough`
  (`:407`), and R-9 permits `retiredSeq` to be late. (The Fatal at `:393` is a post-mortem — the client
  has already been released by the time it can fire — but that is a diagnostic, not a race.)
* **The flake sweep's `-j 8` does interleave this family**, on a 28-core box. I restored the historical
  defect (`~/w7/p5-v1rev-patch2.py apply`: `m_drained` tallied once per batch, after the publish) and
  ran v1's own sweep parameters, `ctest -R 'ServerLoopTest|StagedShadowTest' -j 8 --repeat
  until-fail:100`: red in **1.35 s** on both
  `AClearRecordCrossesAndIsStampedAsAVerbBoundary` and
  `TheSessionWatermarkAndTheDecoderTallyAgreeAfterEveryRecord`. Reverted; clean sweep green afterwards.
* **The audit poison is stricter than R-2.5 requires, and a retained pointer really would read 0xDD.**
  `PipeWireCodec.cpp:1132-1135` fills the run from inside `DecodeAndApply` as soon as the applier
  returns — before `appliedSeq`, long before `retiredSeq` — so there is no "stale-but-valid because the
  run is not yet retired" window. The problem is not the poison; it is that nothing on the reduced path
  reads the pointer (M-2).
* **No GL/EGL left on the app thread on the reduced path**, as far as static reading goes: all twelve
  forwarders go through `RunOnApplyThread`; `CreateBackend`'s `Initialize()`
  (`BackendObject_DirectGLES.cpp:844-851`) is `dlsym` + table installs + `RegisterBufferBackendOps`, no
  GL call; the backend is destroyed on the apply thread (`ServerLoop.cpp:301-306`). The two holes are
  M-7's untested `!m_running` arm and the seam v1 itself reports (c1's private trampolines instead of
  the forwarders, §7.4 / §9.2).

---

## What I could not verify, and why

* **Whether `StagedShadowStore::Adopt`'s split arm or `RequireStagedCoverageForPendingRanges` ever
  execute on the joint split lane.** Answering it needs a counter in `~/w7/p5-v1-joint`, which I was
  told not to modify, and `p5/v1` alone cannot arm the lane. This is the single measurement that would
  settle M-1/M-2 either way, and it is one `MGLOG_I_ONCE` plus one re-run.
* **Whether the 28 `inproc` failures are v1's, c1's or shared.** I established that they are new under
  `inproc` and are wrong answers rather than aborts (M-5); root-causing needs edits in the joint tree.
* **`MOBILEGL_IPC_AUDIT=1` with the R-11 copy removed** — same reason.
* **DirectVulkan under split, OpenRA/SPLIT, anything on a device** — out of reach here, and v1 says so
  itself (§5.6, §5.7).
* **Teardown with a populated sync/query registry.** `MobileGL/Init.cpp` runs `ShutdownSplitRoles()` at
  `:53` and `DestroyAllSyncObjects()` / `DestroyAllQueryObjects()` at `:71` / `:76`, whose own comments
  say they run "while the backend function table can still release the backend handles" — under split
  that table is c1's emit table and the session is already closed. Currently unreachable, because the
  client's `glFenceSync` aborts at the emit table (`Fatal{UnmigratedVerb}`) before anything is
  registered, so I could not construct the case. It becomes live the moment P9 migrates fences, and
  §1's "P5 keeps today's order" records the position without recording that dependency.
* **G5 (`p3a_untouched_regions.sh` / `p4a_…`) and G2/G14** — not re-run; ID-41's re-pin is b1's round 2
  and is not on this branch, so a red there would not be v1's.

---

## Verdict

No blocker. **v1 can merge after one fix round**, provided the round closes M-1, M-4 and M-6 (each is a
small, local change) and answers M-2 with the one-line instrumentation above — because M-2 decides
whether M-1 and M-3 are "a gate that could not fail" or "a rule that never ran". M-3, M-5 and M-7 can be
tracked as follow-ups with owners; M-5 needs an owner assigned before P6, not after.
