# P5 end-of-phase adversarial review

Review target: **37fc4fdb0f6bad35de1d7913c75087376f81fc85**, branch `feat/disaggregated`; diff base **ff2994d9**. HEAD was unchanged at the last check. This range also contains the P5b c0b contract/stubs and APK-CI change; it is not a P5-only range.

Read-only review. No build, CMake, Ninja, CTest, mutation campaign, git-ref change, or device operation was performed. The confirmation perturbations below are proposed, not executed. “Source-confirmed” means the defect follows from the inspected implementation; it does not mean a new runtime reproduction. Repository locations are relative to `/home/swung/w7/pipe` at the pinned HEAD. External evidence/runner locations have no repository HEAD and are identified separately.

## Findings, most severe first

### 1. Coherent-map draws can replace transported bytes with client memory on the server — blocker

**Locations:** `MobileGL/MG_Backend/DirectGLES/Managers.cpp:3087`; `MobileGL/MG_State/GLState/BufferState/BufferObject.cpp:388`; `MobileGL/MG_Remote/Client/PersistentMapTracker.cpp:115`; `MobileGL/MG_Remote/Client/WireTables.cpp:317`.

**Rule:** R-2/R-11; ID-52's server-shadow-only ruling; CONTRACT §4 table 3's client-exclusive trackers; E5/R-16.

**Claim:** The apply thread still executes a client producer that reads `MappedData()` and invokes SubData through the monolith adapter, bypassing the wire.

**Failure scenario:** A coherently mapped VBO is pushed before DrawArrays, but its staged bytes are corrupted or its client push is removed. During the server draw, `EnsureBufferResourceForHandle` calls `SyncPersistentMappedRange`; `PushIsArmed()` checks only transport, so this enters `PushBlocksFor` again. `PushMappedSpanBlock → NotifySubData` changes the frontend serial/aggregate and emits from the frontend shadow (`PipeFill.cpp:785`). `Wire_ResourceSubData` detects the server role and directly calls the monolith adapter. The server shadow can therefore be repaired from the client's correct bytes, hiding the missing/corrupt transport. This also gives the apply thread writes to client-owned tracker/frontend state; serialization is not role ownership. The retained backend sync sites are named as P8 debt, but their ability to defeat the P5 honesty oracle is not an acceptable consequence of that deferral.

The new corrupt-shadow test uses an **unmapped** BufferObject (`ServerLoopTest.cpp:1637`), so its sync call early-outs and does not cover this path. E3(a)'s knob disables both the client and server copies of the same producer, so its red does not distinguish them.

**Confirm:** In an isolated follow-up, suppress only `PushPersistentMapsBeforeVerb()` on the client, leaving `SyncPersistentMappedRange` intact, and run the two coherent-pointer draw cases. Add an apply-thread assertion at `PushMappedSpanBlock` to expose the bypass directly. Pixel behavior under this perturbation is **unverified** here.

### 2. Framebuffer death still invokes driver work on the client thread — major

**Locations:** `MobileGL/MG_Impl/Pipe/PipeFill.cpp:1631`, `:1513`; `MobileGL/MG_Backend/DirectGLES/Managers.cpp:211`, `:8638`, `:8715`.

**Rule:** R-1; CONTRACT §4 server/context ownership; ID-65's unisolated FBO/RBO disposition.

**Claim:** Moving native-context ownership to the apply thread left the frontend framebuffer-death callback as a driver-call path on the client.

**Failure scenario:** Create/bind/clear an FBO, then unbind and delete it before the next draw. Framebuffer deletion has no wire delete: `NotifyAndFree` calls `OnFrontendStateObjectDestroyed`, which destroys the backend twin. Its destructor changes the process-wide FBO binding cache and calls native `glDeleteFramebuffers` without checking/forwarding to the context owner. On the client thread the native deletion does not perform the expected unbind in the server's context, while `NoteFramebufferIdDeleted` can claim binding zero. A subsequent bind-to-zero can then be suppressed against an incorrect cache.

The saved reduced-verb failure is real: `~/w7/p5-joint-evidence/probe-10.out:10` reports black, and `:13` reports 11,589/11,625 wrong pixels. The scenario uses Clear, DrawArrays and **ReadPixels**, not GetTexImage (`P4aFinalFixScenario.cpp:397`). Exact causal attribution of all those pixels to this cache path remains **unverified**, but the wrong-thread driver path itself is source-confirmed. Calling this merely P4b/P7 texture-readback debt is unsupported.

**Confirm:** Instrument native `glDeleteFramebuffers` with an owner-thread assertion and execute `P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact` under inproc; then forward the death callback to the apply thread and compare the pixels.

### 3. Tight ReadPixels drops PACK_SWAP_BYTES — major

**Locations:** `MobileGL/MG_Remote/Server/PipeApplier.cpp:243`; `MobileGL/MG_Remote/Client/EmitTables.cpp:159`, `:478`, `:888`.

**Rule:** ID-49; CONTRACT table 1 row 23; G2's preserved readback semantics.

**Claim:** Neutralizing the server pack state also disables byte swapping, and neither client return path restores that behavior.

**Failure scenario:** Read a multibyte component with `GL_PACK_SWAP_BYTES=GL_TRUE`, for example RGBA/UNSIGNED_SHORT from a known non-symmetric color. `neutralPack{}` resets `SwapBytes=false`; the client tightness predicate ignores SwapBytes and the scatter is only memcpy. Monolith explicitly honors this flag through its conversion path (`DirectGLES.cpp:10893`). Split returns native-order words instead of swapped words, without a diagnostic.

**Confirm:** Add a one-pixel UNSIGNED_SHORT read with SwapBytes toggled and compare the two output byte arrays under monolith/inproc. Use a component other than 0 or 65535. Runtime reproduction is **unverified** here.

### 4. Applied-before-retired scheduling can abort valid uploads as RingOverrun — major

**Locations:** `MobileGL/MG_Remote/Transport/SessionRings.h:393`; `MobileGL/MG_Remote/Server/ServerLoop.cpp:533`; `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:779`.

**Rule:** R-1/R-9; legal delayed retirement must not become loss of an otherwise valid record.

**Claim:** The client resumes on appliedSeq, but staging exhaustion performs only one immediate retirement poll and then aborts; it does not wait for retirement.

**Failure scenario:** The consumer applies a large upload and publishes appliedSeq, then is descheduled before the drain's `RetireThrough`. The client wakes and stages another individually valid blob that cannot coexist with the previous allocation. `StageAllocate` polls the still-old retiredSeq once and raises `Fatal{RingOverrun, "SEG_STAGE"}`. No oversized record or unavailable long-term capacity is necessary. The analogous command-ring risk exists because ring reclamation also waits for the end of a drain batch.

**Confirm:** Pause the server immediately after publishing appliedSeq and before RetireThrough; issue two successive uploads each larger than half SEG_STAGE but smaller than its full capacity. The second should wait for reclamation, not abort. This interleaving is **unverified** by execution here.

### 5. The shipped CI stops on the deliberately indebted census before reaching E1/E3 controls — major

**Location:** `.github/workflows/test.yml:1010`; later controls at `:1102`.

**Rule:** ID-65's explicit reduced-path/debt disposition; ID-66; E1/E3/E6 and R-16.

**Claim:** CI still requires the entire inproc integration-gpu lane to pass and match monolith, so the accepted class-C/wrong-answer debts prevent the later negative controls from running.

**Failure scenario:** The known 505 verb aborts and 27 ordinary failures produce a nonzero CTest exit in the workflow's fail-fast shell. The subsequent status comparison and later default-success steps are skipped. Even allowing that command to return would encounter a parity requirement incompatible with the recorded disposition. The historical joint runner's success is not evidence that this workflow executes its controls.

**Confirm:** Inspect the first `integration-split` CI job on the pinned revision: the broad inproc step's failure must be followed by skipped E1/E3 steps. Alternatively execute the step sequence on an isolated configured tree with the known census baseline.

### 6. The E2 draw-drop control runs against the pull library left by the preceding control — major

**Locations:** `scripts/ci/retrace_pull_library_control.sh:85`, `:98`; `.github/workflows/test.yml:1949`, `:1976`.

**Rule:** E2/R-16; ID-65/69.

**Claim:** The pull-library control restores images but never restores the split library before the workflow runs the new draw-drop control.

**Failure scenario:** The first negative control copies PULL_LIBRARY over FROZEN_LIBRARY and succeeds because transport identification rejects it. The following draw-drop step reuses that frozen path. Pull ignores the drop knob and cannot emit the required dropped-record line; the new control fails for the wrong library. The x2/x2m standalone split-library runs and stub tests do not exercise this sequence.

**Confirm:** In an isolated retrace workspace, run the two workflow control steps in order and inspect the frozen library between them; it will still be byte-identical to PULL_LIBRARY.

### 7. E3(a)'s final merged selection necessarily includes intentional skips — major

**Locations:** `scripts/ci/split_negative_controls.sh:182`; `MobileGL/MG_IntegrationTest/Harness/split_log_paths.py:57`; `MobileGL/MG_IntegrationTest/Scenarios/PersistentCoherentMapScenario.cpp:453`.

**Rule:** ID-62's own-entry red requirement; E3(a)/R-16.

**Claim:** The merged validator rejects every skip, but its E3(a) filter still includes the two non-counting instances of TheMapLandsInTheArmItsLaneDeclares, which deliberately skip.

**Failure scenario:** After the four behavioral entries fail for the missing persistent push, the default and SmallRing map-arm entries skip because they lack the counting marker. The helper exits with “the knob killed the pre-flight” even though these are intentional body skips. The old joint explicitly measured 4 red/2 skip; x2m reports the same two expected baseline skips and does not report executing the final merged live E3 control.

A second attribution gap remains at `split_negative_controls.sh:148`: E3's pixel diagnostic is searched in the **combined** output. Once the selection is repaired, one case's legitimate pixel failure can supply the reason for another selected case's unrelated failure.

**Confirm:** Execute the final merged control with a healthy baseline, or feed its result validator a manifest/JUnit containing these six names with four failures and the two documented skips. It must reject that result.

### 8. “ringwaits” measures a reclaim attempt, and SmallRing does not assert a wait — major

**Locations:** `MobileGL/MG_Remote/Wire/PipeWireCodec.cpp:801`; `MobileGL/MG_IntegrationTest/Harness/WireLedgerChecks.h:119`, `:139`.

**Rule:** BRIEF E3(e); ID-65's back-pressure proof; R-16.

**Claim:** The reported wait is neither a wait nor proof that the producer was blocked by the consumer, and the gate only asserts command-ring wrap.

**Failure scenario:** Staging fills with already-retired allocations because reclamation is lazy. An immediate reclaim increments `m_stageReclaimWaits` even though the consumer finished earlier and the producer never waited. SmallRing can pass with stageReclaimWaits zero: that field is only a JUnit property. Increasing command traffic to wrap does not establish staging pressure or a wait.

**Confirm:** Remove only `++m_stageReclaimWaits` and run the SmallRing triangle control. Its wrap/byte assertions remain unchanged and can pass with ringwaits=0. For the measurement itself, retire allocations before requesting the next blob: the “wait” still increments.

### 9. Losing the split implementation still turns the baseline into a successful all-skip run — major

**Locations:** `scripts/ci/split_negative_controls.sh:95`; `MobileGL/MG_IntegrationTest/Harness/ScenarioFixture.h:140`.

**Rule:** R-16; ID-29/32; completed-phase E1.

**Claim:** Bring-up-era disarming remains accepted after the phase is declared landed.

**Failure scenario:** A regression makes the runtime split predicate false while the harness remains usable. Every Split entry skips; the control script prints a warning and exits zero before testing either knob. REQUIRE_GPU only rejects unusable GPU setup; it does not convert the split predicate's skip into a failure. This is a false green for this gate, regardless of whether another lane later catches the regression.

**Confirm:** Perturb `ImplementedVerbCount()` to return zero while retaining the build/session. The split baseline and its control script must reject the loss of implementation; the current all-skip branch accepts it.

### 10. The new EGL-backed unit gates are still optional in CI — major

**Locations:** `.github/workflows/test.yml:963`; `MobileGL/MG_Test/Wire/ServerLoopTest.cpp:1205`.

**Rule:** R-16; ID-69's explicitly outstanding CI item.

**Claim:** The split unit step does not set REQUIRE_GPU, so server production-wiring controls can disappear into skips.

**Failure scenario:** EGL fixture bring-up fails in the unit process. The macro skips the coverage, shadow and native ownership tests, and CTest reports success. Later integration steps setting REQUIRE_GPU do not prove those unit-only assertions executed. This is a **known but still unclosed** phase debt, not a newly discovered historical omission.

**Confirm:** Make the EGL fixture's BringUp return a named failure and invoke the unit step with its exact CI environment. The relevant cases should fail; they currently skip.

### 11. The census runner can report passes with zero executed tests and reuse stale Fatal evidence — major

**External location:** `/home/swung/w7/notes/tools/p6_census_lane.py:43`, `:54`, `:59`, `:62`.

**Rule:** R-16; ID-68/P5b's census-driven migration gate.

**Claim:** The runner classifies process status without proving the selected test ran, and reads private log paths that it never clears.

**Failure scenario:** A stale manifest's gtest filter matches zero tests; gtest exits zero and the runner records “passed.” A launch failure that does not open the library log can leave an earlier run's Fatal attached to the new result. The skip-substring test takes precedence over a failing return code; SIGSEGV is folded into “aborted.” Thus fewer counted aborts need not mean more migrated rendering. This is a runner defect, **not evidence that the retained 426/185/511/27 dataset is fabricated**.

**Confirm:** Run the runner against an isolated one-entry manifest whose command selects `DefinitelyMissingSuite.DefinitelyMissingCase`; require a harness error rather than “passed.” Separately retain a prior private log and use a command that fails before library initialization.

### 12. G5's pin self-test does not exercise production pin selection — minor

**Locations:** `scripts/p3a_untouched_regions.sh:405`, `:535`, `:555`; corresponding direct helper calls in `scripts/p4a_untouched_regions.sh:769`, `:792`.

**Rule:** ID-41; R-16.

**Claim:** The self-test invokes apply_pinned_shas directly rather than proving the production baseline extraction actually invokes it.

**Failure scenario:** Remove the production pin-substitution call while retaining the helper. Both pin self-test arms still perform their own substitution and pass. A comparison whose two refs share a drifted body can then pass without consulting the reviewed pin. Existing old-base comparisons may go red for a different reason; that does not rescue the self-test's stated coverage.

**Confirm:** Delete only the apply_pinned_shas call in production extract_baseline, leaving self-test calls untouched, and execute --self-test in an isolated copy. The control should fail but its inputs/results are unchanged.

### 13. c1g renamed the test but left c1f's shipped mutation runner targeting the removed name — minor

**Location:** `MobileGL/MG_Test/Wire/c1f_redcheck.py:72`; new cases in `RemoteClientControls.inc`.

**Rule:** ID-67; R-16 reproducibility.

**Claim:** The committed codex12-repeat-skip mutation cannot complete a green baseline at HEAD.

**Failure scenario:** The runner selects `RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent`, which c1g replaced with identical-tuple and different-tuple cases. Gtest runs zero matching tests, and the runner correctly rejects the missing OK line. Its 15/15 historical result therefore cannot be repeated with this script at HEAD.

**Confirm:** Search the test source for the exact name at runner line 72; it is absent. Update the mutation to target the different-tuple case before rerunning it.

## Claims in reports/docs that code or logs do not support

### Current overclaims and stale dispositions

- **`x2-v1.md:7`: “All three obligations are closed.”** R-10's numeric evidence and the isolated draw-drop result do not close final workflow integration; findings 5–8 remain. ID-69's “asserts `ringwraps=1 ringwaits=1`” overstates the code: only wrap is asserted.
- **`ServerLoopTest.cpp:1624`: “the server's staged copy is the draw's ONLY base”; `v1-v3.md`'s corresponding E5 closure.** The unmapped fixture does not cover finding 1's coherent-map producer re-entry.
- **`SlotCaps.h:60`: “the frontend's own CPU readback, which is exact.”** The retained 22 texture-readback failures contradict this general statement after server rendering. ID-65 waives them to later phases; it does not make the fallback exact.
- **`v1-v3.md` §7: “none is v1's.”** `joint-v1.md` §4 explicitly says the FBO/RBO fix site was not isolated and does not prove that exclusion. Finding 2 identifies an outstanding context-ownership path. The 27 failures cannot all be described as P4b/P7 texture readback: three are queries, one is inspection, and one is this deletion path.
- **`~/w7/notes/p6/census-classC.md:71`: six StageSnapshotTooNarrow aborts are “P8/P11 staging/adoption debt.”** ID-69 and `v1-v3.md` correct this: those six were v1's P5 round-2 overbroad refusal of orphan-plus-partial-upload. HEAD contains the HasDefinedContent fix (`Managers.cpp:2018`, `:3147`). The old census measured **505 class-C + 6 P5 defects**, not 511 class-C. The reported post-fix census is 432/185/505/27; it is not the old census rerun at this review HEAD.
- **`MEASUREMENTS.md:674`, `:694`, `:726`: abort attribution unmeasured, max-record pending, A/B pending.** These are stale as a phase-close account: census, x2/x2m, and ab reports now exist. Historical joint numbers should remain explicitly historical, with the final dispositions added.
- **`ab-v1.md:11`: “This is a concrete P5 blocker.”** The Minecraft class-C aborts are the explicitly ruled P5b exit work under ID-66/68/69, not evidence that P5 promised those workloads. They do leave device barrier cost unmeasured.
- **`ARCHITECTURE.md:628` / ROADMAP's P5 completion presentation:** no completed final-head full-gate transcript was available. `~/w7/p5-final-gate.log` was **zero bytes** when inspected. The joint gate and package quick gates cannot be relabeled as execution of the final combined workflow, particularly with findings 5–7.

### MEASUREMENTS §§26–30: numerical provenance and limits

The following identifies evidence, not a new execution. All `p5-joint-*` and `p5j-*` paths below are under `/home/swung/w7`.

| Published figures | Evidence inspected / limit |
|---|---|
| §26: 4 closure probes/0 problems; remote symbols 0/610; text 10,806,611 unchanged; symbols 27,814 unchanged and 0/0/0/0 | `p5-joint-gate.log:15`, `:19`, `:21`, `:22`. Historical joint only. |
| §26: both G5 comparisons; generator 9, dirty-surface 27, ownership 15; emitter/CSO 185; verify controls 4 | `p5-joint-gate.log:23`, `:24`, `:27`, `:30`, `:32`, `:33`, `:34`. These are run summaries, not proof of every self-test's production sensitivity. |
| §26: G2 difference 0; G14 0 removed/42 added | `p5-joint-gate.log:36`, `:37`. Does not describe all additions up to review HEAD. |
| §26: unit 1816 ×3, split 2038, Wire 58, integration 1128/1128/1149 | `p5-joint-gate.log:78` through `:85`. CTest success totals include skips. |
| §26/27: 426 pass/185 skip/511 abort/27 fail/0 segfault | `p5-joint-evidence/census-status.json:3` through `:9`; independent external census results reproduce the four nonzero counts. Historical attribution corrected above. |
| §26: 21 split entries, 19 run/2 skip; persistent 2/2, pmap 2160 versus 0, mpr 1 | `split-repeat-1.xml:3`, `:4`, `:6`; `p5-joint-gate.log:710` through `:712`. |
| §26/27: verify 930/930, zero Fatal; OpenRA 2/2 and SSIM 1.0; retrace push/verify 79/79, armed 79 | `p5-joint-gate.log:714` through `:723`; `p5-joint-evidence/gate-openra/summary.json:5`, `:10`. No final-head rerun inferred. |
| §26 landed quick-gate 2038/22/1128 and G1 zero | Traceable to ID-66; independently matched final quick-gate raw transcript **unverified** in this review. |
| §27: E1 14 reds with private Fatal | `p5-joint-gate.log:555`, retained `controls-private/`, and joint report Appendix B. Not execution of final merged CI. |
| §27: E3(a) 6 selected/4 red/2 skip | `p5-joint-gate.log:623` onward and joint report §3. Its `:708` “6 ... red” summary is not literally correct. |
| §27: ID-42 green/red/restored, three reds | `id42-green.xml:3`/4/6 = 3/0/1; `id42-red.xml:3`/4/6 = 3/3/0; restored = 3/0/1. This supports the scoped membership “VERIFIED,” not wire-only coherent-map correctness. |
| §27: audit 19 pass/2 skip; strict 19 abort/2 skip | `audit.xml:3`/4/6 = 21/0/2; `strict.xml:3`/4/6 = 21/19/2, with named library diagnostics retained in `strict-private/`. |
| §27: 27 ordinary failures and family split 14+3+5+3+1+1; 8 repeated ×3 | Traceable to joint report §4 and `probes.json`; FBO probe personally traced above. Every rerun and every family's complete causal exclusion is **unverified**; an assertion transcript is not root-cause proof. |
| §28: Triangle frame/window 1/1 and 2/1, pmap 0, mpr 0, rsp 35 | `p5-joint-probe-52.log:577`, `:578`. |
| §28: persistent frame/window 1/1 and 2/1, pmap 600, mpr 1 then 0, rsp 35 | `p5-joint-probe-53.log:577`, `:578`. Finding 1 prevents interpreting pmap as exclusively transported bytes. |
| §28: all six RSS numbers, 58,990,592 mapped/role and 117,981,184 combined | `p5-joint-probe-52.log:11`, `:13`, `:581`; corresponding lines of `probe-53.log`. These are shared-process samples, not two independent peaks or an RSS slope. |
| §28: 8/32/16 MiB + 256 KiB and 16-byte reply header | Contract/SessionRings/ReplySlot geometry; joint `additional.xml` records the smoke test. Physical RSS is a different measurement. |
| §29: 10/10 confirmed; later review counts; 81/58/59 identity counts | Traceable to ID-40/46/48/52/58/62/64 and their reports. Independent replay of every historical perturbation and identity rewrite is **unverified**. |
| §30: four-arm protocol and trailing 200 frames | Protocol, not a measured P5 barrier result. `ab-v1.md` reports 27/40 completed groups and no split benchmark; its raw device directory was not audited here. |

Later report evidence inspected: `p5-c1f-redcheck-final.log:681` has `FAILED_CONTROLS=[]`, but its old codex-12 name at `:625` confirms finding 13's revision mismatch. `p5-v1-r3-redcheck.log:118` reports 36 perturbations; that does not test finding 1's mapped-buffer variant. `p5-x2m-redcheck.log:113` reports the smoke mutation campaign, not the live workflow sequence. `p5-x2m-e2-dropdraw/summary.json:5` gives SSIM 0.000036 and its private `OpenRA/DirectGLES/output/mobilegl.log:1101` records 758 draws dropped. The reported maxrec=784 and SmallRing 1,314,880-byte ledger are traceable to x2/x2m reports; a new numeric measurement at review HEAD is **unverified**.

## Debts present in code that no inspected ID or ROADMAP row specifically names

These are concrete omissions, not new phase assignments:

- The coherent-map **server-side re-entry into the client producer**, including frontend aggregate/serial writes and wire-bypass accounting, is stronger than the named P8 debt “retain the 21 sync sites” (finding 1).
- Client-side **multibyte pack conversion after tight readback** is missing; the ID-49 scatter description covers layout but never accounts for SwapBytes (finding 3).
- A **retirement-aware allocation wait/retry policy** is absent despite a barrier that waits only for application (finding 4).
- The **combined control lifecycle**—restoring the frozen library and selecting only cases that can produce each expected red—is not captured by the package-local control debts (findings 5–7).
- `PipeApplier.cpp:484` explicitly admits sticky forwards during non-verb records are outside rsp accounting; `PipeInputs.cpp:184` returns before counting/strict checking when no verb is stamped. The code and earlier report name this limitation, but the inspected ID/ROADMAP account does not give this specific gap an owner/retirement gate. Consequently rsp=35 is a lower bound, not the complete frontend-dependency census.
- The reply pool accepts arbitrary status integers (`ReplySlot.h:302`), while ReadPixels rejects only 1 and 2 before checking extent (`EmitTables.cpp:359`). An exact-sized status=3 answer is accepted, although the helper `ReadbackReplyIsComplete` would reject it. No inspected disposition names this protocol-validation gap. Production emission of such a status is **unverified**; confirm with the existing adversarial reply sink posting status=3.

The six historical staging aborts, context-loss restaging, PACK-PBO refusal, oversized readback, GetCaps carrier, ABI sizing, 22 texture-readback failures, three query failures, and inspection-forwarder gap are already named somewhere in the record. They should not be repackaged as newly unnamed debts.

## What I could not verify and why

- No new runtime reproduction, sanitizer run, mutation, or build identity check was permitted. Proposed confirmations must run later in an authorized isolated checkout. In particular, findings 1–4 need their specified focused runtime probes; finding 2's exact pixel-cause attribution remains qualified.
- No completed full gate for **37fc4fdb** was available; the final-gate file was empty. Package logs belong to their stated revisions. I did not infer binary equivalence from a current source checkout.
- The 20,314-line historical report collection was used for decisions, claims, prior findings and evidence references; this review does **not** claim an independent reconstruction of every historical “verified” assertion or every APK/device datum. Any such assertion not tied to inspected evidence above remains **unverified by this review**.
- APK proof/signature/build settings and device pin/fan/temperature/repeat figures were read as package reports, not independently remeasured. The required final-head APK rebuild is not demonstrated by `ap-v1.md`, which explicitly builds `e61d0012`.
- No N-frame RSS slope or device inproc barrier-cost measurement exists in the inspected evidence. The A/B's split arms abort before benchmarking; no timing can be derived from them.
- G1/G2/G14 historical summaries are not final-head binary verdicts. I found no basis to claim a newly measured pull-codegen regression, nor to claim final-head byte identity without running the forbidden checks.
- The post-fix 432/185/505/27 census is reported on earlier package/integration heads. Its reproduction at the pinned review HEAD, and exhaustive causal disposition of all 27 wrong answers, remain **unverified**. Inspecting the old 511 Fatal lines cannot settle either question.

