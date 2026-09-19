# c1 adversarial review, round 2

Reviewed `p5/c1@41992649`, including c1 commit `02f15bb6`. Source citations below are at that branch; the red-check script and logs are external artifacts. This was a read-only review: no builds, tests, perturbations, source edits, or ref changes were performed. Failure scenarios are established by source inspection unless explicitly marked unverified; proposed perturbations remain unexecuted.

## Findings, most severe first

### 1. Tight client allocation is paired with a server that still applies non-neutral pack state — blocker

**File:line:** `MobileGL/MG_Remote/Server/PipeApplier.cpp:192`; `MobileGL/MG_Remote/Client/EmitTables.cpp:292`.  
**Violates:** ID-49; CONTRACT §2 row 23.

**Claim:** This head sends a tight `DstSize` but still lets the backend write the application's packed layout into the server scratch allocation.

**Failure scenario:** A first 4×3 RGBA8 read with `PACK_ROW_LENGTH=8, SKIP_ROWS=1, SKIP_PIXELS=2` allocates 48 bytes; the backend writes rows at offsets 40, 72, and 104, ending at byte 120. `OnReadPixels` has no neutral-pack scope, and DirectGLES `ReadPixels` installs `PackStateFromContext()` at `DirectGLES.cpp:10879`. Even when earlier reads have enlarged the scratch allocation enough to hide the overflow, the first 48 bytes are not a tight reply, so client scattering is wrong. This is the outstanding v1 half of ID-49, present in the reviewed tree.

**How to confirm:** With the documented Init hook applied in an authorized verification checkout, perform that exact first read using a sentinel destination and a guarded/ASan server scratch allocation. It must expose the server overrun before evaluating client scatter.

### 2. A pack-PBO offset is used as a client CPU destination — blocker

**File:line:** `MobileGL/MG_Remote/Client/EmitTables.cpp:321` and `:332`.  
**Violates:** CONTRACT §3's PBO producer requirement; R-2 and R-4.

**Claim:** `EmitReadPixels` has no PBO destination branch and treats the GL `pixels` argument as a dereferenceable application pointer.

**Failure scenario:** Bind a sufficiently large pixel-pack buffer and call a 1×1 RGBA8 read with `pixels=(void*)16`. The frontend permits an aligned PBO offset (`GL_Framebuffer.cpp:3055`), but the tight client path copies the reply to address 16. Offset zero instead supplies no reply buffer and cannot receive a nonempty reply. The server also loses the actual PBO offset: `info.DstOffset` is always zero, while `OnReadPixels` passes its scratch address to a backend that can still see the bound PBO. b1's subsequent `MarkReadPixelsPackBuffer()` does not repair either destination.

**How to confirm:** In an authorized patched-head scenario, bind a 64-byte pack PBO, issue the offset-16 read, and inspect the PBO through the ordinary buffer readback path. Require correct bytes and no CPU access to address 16.

### 3. Every nonempty decoded flush still reaches a mandatory-null rejection — blocker

**File:line:** `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:1777`; `MobileGL/MG_Pipe/PipeApply.cpp:1873`.  
**Violates:** CONTRACT §2 row 20 / §6.3; R-13.2.

**Claim:** The new preceding subdata records cannot make a nonempty wire flush pass the existing applier guard.

**Failure scenario:** After the preceding subdata has staged the 120-byte coherent buffer, the decoder calls the flush applier with `nullptr`; the guard aborts before `g_resourceOps->FlushRange` can consult server-owned storage. This is the acknowledged seam, with corrected branch-relative codec line numbers.

**How to confirm:** `wsl.exe -d Arch -- bash -lc "cd ~/w7/pipe && git show p5/c1:MobileGL/MG_Pipe/PipeApply.cpp | sed -n '1868,1894p'"`

### 4. Stop installs driver-reaching adapters while the transport is still active — major

**File:line:** `MobileGL/MG_Remote/Client/ClientSession.cpp:585`; `MobileGL/MG_Remote/Client/WireTables.cpp:482`.  
**Violates:** R-4/R-17; CONTRACT §4 server-exclusive applier ownership.

**Claim:** Teardown replaces the wire routes with unconditional monolith adapters before draining or joining, with no transport/role guard on those adapters.

**Failure scenario:** At this point `m_started`, `g_active`, the server, and its applier are still live. A routed resource call after uninstall runs `MGPipeApply*` on the caller instead of failing or emitting. The same bypass remains after a failed Start calls Stop while configuration still selects inproc. Thus the stated protection against a GL call arriving during teardown explicitly permits the forbidden path.

**How to confirm:** Start a live session, call `UninstallClientWireTables()`, then issue one routed resource mutation on the GL thread; observe unchanged wire ordinal and a direct applier call. Require a named refusal instead. No concurrent GL API call is needed for this probe.

### 5. The two acceptance escapes convert ERROR into ordinary decline — major

**File:line:** `MobileGL/MG_Remote/Client/WireTables.cpp:342` and `:391`; `ClientSession.cpp:773`.  
**Violates:** R-5; CONTRACT reply header status distinction; c1-v2 §4 R-17.1.

**Claim:** `ResourceRespecify` and `MapPersistent` bypass the mailbox's `ReplyError` enforcement.

**Failure scenario:** Stamp the correct sequence with Status=2. `EmitAndWait` logs ERROR and returns; respecify returns false, and MapPersistent returns nullptr, indistinguishable to their callers from DECLINED. The generated acceptance wrappers instead pass Status=2 into `MGPipeTakeReplyBool`, which aborts. A dead doorbell similarly leaves the initialized ERROR status and takes these escape declines without reading a reply.

**How to confirm:** Perturb the server reply for each escape to Status=2 while preserving its sequence. Drive each through its installed client route and require `Fatal{ReplyError, "<row>"}`; current source returns normally. The existing mailbox ERROR test never invokes either escape.

### 6. ReadPixels incorrectly applies PACK_SKIP_IMAGES — major

**File:line:** `MobileGL/MG_Remote/Client/EmitTables.cpp:612`.  
**Violates:** ID-49's requirement to reproduce the application's GL readback layout.

**Claim:** The scatter applies image skips to a two-dimensional ReadPixels operation, where image-height/image-skip state is ignored.

**Failure scenario:** A 4×3 RGBA8 read with otherwise neutral pack state and `PACK_SKIP_IMAGES=1` should write bytes 0–47. The fast-path predicate rejects it and the scatter writes bytes 48–95 instead. An application supplying the required 48-byte destination is overrun. The preexisting DirectGLES conversion path explicitly uses `honorPackImageParams=false` at `DirectGLES.cpp:10905`.

**How to confirm:** Add `SkipImages=1` to a sentinel scatter case with a 96-byte destination; require bytes 0–47 to contain the reply and bytes 48–95 to retain their sentinel. Then exercise the same state through GL.

### 7. R-1's assertion is inert on this head and checks too late even after the proposed bracket — major

**File:line:** `MobileGL/MG_Remote/Client/ClientSession.cpp:692`; `MobileGL/MG_Remote/Server/PipeApplier.cpp:305`.  
**Violates:** R-1; CONTRACT §4 `gPipeInputs` row; ID-53.

**Claim:** No production applier brackets execution with c1's flag, and the client's sole check is at emission rather than at the preceding residual-state write.

**Failure scenario:** With the barrier disabled, the flag remains false while two threads access shared pipe state. Adding only `ScopedApplierEntry` does not observe a client fill that races the applier and finishes after the applier has cleared its flag but before `EmitAndWait` checks it. `MGP_FILL(ReadPixels)`, for example, occurs before the table call at `GL_Framebuffer.cpp:3097`. Ordinary split lane environments also lack the private log destination needed to attribute this MGLOG_F abort.

**How to confirm:** `wsl.exe -d Arch -- bash -lc "cd ~/w7/pipe && git grep -nE 'ScopedApplierEntry|NoteApplyThreadEnteredApplier|ApplyThreadIsInsideApplier' p5/c1 -- MobileGL"`  
For the remaining placement defect, an authorized probe should hold the server inside apply while the client reaches its first residual write, then release it before emission; require the assertion at the write itself.

### 8. “20/20 RED” does not establish twenty failures for their own reasons — major

**File:line:** external `/home/swung/w7/p5-c1-r2-redcheck.py:253`–`:276` (not tracked at p5/c1).  
**Violates:** R-16; ID-46/ID-50.

**Claim:** The runner accepts any nonzero exit, any failed-test marker, build failure, or timeout, without asserting a case-specific diagnostic.

**Failure scenario:** A broken integration setup or unrelated SIGSEGV satisfies all four integration perturbations. Preflight only builds the unpatched baseline; it does not first run each scenario with its necessary `init_patch`. Restore excludes the integration target, merely prints other restore failures, and the script does not exit unsuccessfully when `failures` is nonempty. Its log retains only “RED”, losing attribution.

**How to confirm:** In an authorized copy of the harness, make `run()` return `("UNRELATED_FAILURE", 1)` for every case. Its verdict predicate still accepts every case as red, and the final status remains successful. No project build is needed to demonstrate that predicate defect.

### 9. The 33+4 catalogue test cannot detect one missing client-wire installation — major

**File:line:** `MobileGL/MG_Test/Pipe/PipeCatalogueTest.cpp:162`; `MobileGL/MG_Remote/Client/WireTables.cpp:439`.  
**Violates:** R-16/R-17.

**Claim:** The advertised routing partition test installs and counts the monolith table, never the client-wire table.

**Failure scenario:** Delete one assignment in `InstallClientWireTables`; the earlier monolith pointer remains nonnull, the arm flag still says client wire, and the catalogue test observes its explicitly reinstalled monolith table. The aggregate encoder ordinal can still advance through other resource rows and class-B verbs. No `MG_Test` or `MG_IntegrationTest` reference to `ClientWireRecordsEmitted` was found at this head.

**How to confirm:** Delete only `gMGPipeScreen.ResourceDestroy = &Wire_ResourceDestroy;`; `PipeCatalogue.ExactlyTheRoutedRowsAreInstalledAndTheRestAreStillNull` remains structurally unchanged and green. A control must invoke that row through the installed split table and observe its opcode on the wire.

### 10. The readback red-checks miss deletion of the production mechanisms they claim to protect — major

**File:line:** `MobileGL/MG_Remote/Client/EmitTables.cpp:281`, `:304`, `:590`; `MobileGL/MG_Test/Wire/RemoteClientTest.cpp:463`.  
**Violates:** R-16; ID-47/ID-49.

**Claim:** The tight-size test exercises a helper unused by the emitter, and the capacity integration perturbation does not prove that removing the pre-check is detected.

**Failure scenario:** The ID49-a mutation changes `TightReadbackBytes`, reached only through the test-facing `TightReadbackByteCount`; production independently multiplies width, height, and bpp at line 281. Its reported red can therefore occur without changing any emitted `DstSize`. ID47-b multiplies the helper argument by 1000 in a small-read success scenario; deleting the helper call leaves that scenario successful.

**How to confirm:** Change only production `info.DstSize = tight` to `tight + 16`: `RemoteReadback.DstSizeIsTheTightExtentAndNeverThePackedOne` still passes. Separately delete only the production `RequireReadPixelsReplyFits` call: the named small-read scenario still fits the pool. The required control is an actual oversized emission attempt that observes the client's own refusal before the record ordinal advances.

### 11. Short successful replies and failed reads can become plausible pixels — major

**File:line:** `MobileGL/MG_Remote/Client/ClientSession.cpp:781`; `MobileGL/MG_Remote/Client/EmitTables.cpp:330`.  
**Violates:** CONTRACT §2 row 23's exact reply extent; R-4's refusal requirement.

**Claim:** The client checks only for oversized replies, ignores ReadPixels status, and scatters the entire requested extent even when fewer bytes arrived.

**Failure scenario:** A correctly stamped OK reply carrying one row of a three-row read passes `ReplySlotPool::Read` and `EmitAndWait`. The tight path leaves the remaining application bytes stale; the bounce path copies the unfilled portion of its initialized buffer into the application. A zero-payload ERROR or DECLINED likewise reaches the unconditional scatter. The ordinary pool stamp check cannot detect this because the sequence is correct.

**How to confirm:** Perturb the server sink to post Status=OK with `info.DstSize - oneRowBytes` while retaining the same sequence. Drive a non-neutral pack read and require a named short-reply refusal and untouched destination; current client code accepts and scatters it.

### 12. A repeated successful MakeEGLCurrent does not adopt its newly published caps — major

**File:line:** `MobileGL/MG_Remote/Client/BackendObject_Remote.cpp:212`; `MobileGL/MG_Backend/BackendObject.cpp:341`.  
**Violates:** R-12; the EGL forwarder's caps-republication contract.

**Claim:** After initial capability setup, the virtual queues a fresh snapshot through v1's forwarder but returns without pumping it.

**Failure scenario:** On an already initialized surface, `ServerMakeEGLCurrent` publishes a snapshot (`ServerLoop.cpp:621`), but the client's base MakeEGLCurrent skips `InitCapabilities`. A getter or shader compile before the next Present sees the prior mirror and prior compile-environment memo. Repeated make-current calls without Present can also accumulate snapshots. Runtime impact when actual device caps change is unverified; the missed adoption is visible in control flow.

**How to confirm:** After initial make-current, change a test server cap and repeat make-current on the initialized surface; immediately assert that mirror generation and a public cap getter reflect the new snapshot, without manually calling `PumpControlPlane` or Present.

## Claims in c1-v2.md that the code or logs do not support

- **§6: “The same seven are the only integration-gpu failures in build-split under monolith (1149, 7 failed).”** The available `~/w7/p5-c1-r2-igpu-split-mono.log:2301` says **“19 tests failed out of 1149”**. Its failure list includes ClearThenReadPixels and Triangle, not just persistent maps. The named Split entries override transport to inproc despite the surrounding monolith invocation. A later seven-failure full-lane run is unverified; no such saved log was located.

- **§6: “That is the only thing between 14/21 and 21/21.”** The saved `p5-c1-r2-isplit-inproc.log` records 14 passed and seven aborts, but contains no per-abort Fatal text. Source proves the flush incompatibility; it does not prove that fixing it exposes no later failure. ID-49's server half is also absent from this head (finding 1), so the statement cannot establish broader inproc correctness.

- **§4: “ERROR is refused as an acceptance answer at all.”** False for the two escapes (finding 5).

- **§8: “I made every gate red once” / “20 / 20 RED.”** The log supports twenty runner verdicts, not twenty attributed production failures. Findings 8–10 identify the acceptance predicate and two misdirected proofs.

- **§6: installation is “after the first CapsSnapshot.”** `ClientSession.cpp:526` merely warns if pumping adopts zero snapshots and continues to installation at line 574. The ordering is after an attempted pump, not a successful first adoption.

- **§7: the distinction is visible through “ClientWireRecordsEmitted()”.** That counter is used by the respecify self-check, but the advertised 33+4 test and integration probes do not read it. An overall encoder ordinal cannot identify a single row left on the monolith adapter.

## What I could not verify and why

- All proposed runtime reproductions and deletion controls remain **unverified by execution**, as required by the read-only/no-build instruction. Each finding gives the specific operation or perturbation needed in an authorized verifier checkout. The documented `init_patch` is necessary to reach the remote backend on this committed head.
- The complete `integration-gpu` lane under **inproc** is unverified. The listed artifacts contain the 21-entry inproc split log and the full monolith-labelled log, but no full inproc-lane log. Locating an archived full inproc run and reading its per-test diagnostics would settle its additional failure census.
- End-to-end stale-reply rejection across pool reuse was not exercised. A settling probe should post/read sequence n, reuse its slot with n+8, then issue a later reply-owning record with its server post suppressed; require `ReplyMissing`, not the prior result. Ring wrap and session restart should be separate variants.
- Exhaustive texture byte-count coverage across mip levels, cube faces, compressed storage, and non-default UNPACK conversion is unverified. The new count comes from the level shadow's `GetMipmapByteSize(uploadTarget, level)`, rather than raw user unpack memory; that alone does not prove every conversion path. A settling probe must upload distinct per-level/per-face bytes through public GL calls with those states and compare the decoded staged extent and rendered result.
- Generator regeneration/checks and G1 binary equivalence were not rerun. The reviewed generator/table/thunk changes cannot substitute for a fresh byte comparison.
- No per-EGL-virtual deletion controls were present in the twenty-case harness; its `m_started` scenario is also accepted without an attributed diagnostic. Per-virtual forwarding and lifecycle behavior therefore remain unverified beyond source tracing.

