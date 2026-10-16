# jm — P5 joint merge and first full gate

**Scratch head: e61d00123c41d74f98cd06b63768be12d9b6348a (p5/joint).** Reduced path: **19 passed / 2 intentional skips / 0 failed, total 21**. E1: all **14** selected private files carry the named barrier Fatal. G1: **0/0/0/0, .text +0**. The full phase is **not all green**: broad inproc is **426 passed / 185 skipped / 511 subprocess-aborted / 27 failed / 0 segfault = 1149**, and OpenRA's clear-drop control remained green.

All current measurements below are from this scratch head. Historical package numbers are explicitly identified as comparison inputs. CTest's “100% tests passed” includes skips; actual statuses are separated wherever JUnit was retained. No integration merge was made.

**Clean handoff is pending coordination:** after all jm measurements and a clean 09:48 check, a concurrent APK job edited android-plugin/build.gradle.kts (mtime 09:49:57). That file is outside jm’s granted edits. The edit is preserved, not reverted; scratch HEAD itself is unchanged.

## 1. Assembly

| Input | Pinned SHA at start |
|---|---|
| integration base | c3e736a3 |
| p5/j0 | b2dcae713f5c81e753fe1ddfd8203b58961d7827 |
| p5/c1 | 5682f429d0c877ae5d50ad723b76253b182c98f6 |
| p5/v1 | c9d84e33c5e51075e9c6ec7374740fb314cc755c |

Merged in that order using the full SHAs. j0 fast-forwarded; git ignored the supplied -m message because no merge commit was created. c1 merge: 2536ffb9207667b3abc19fd2e77d6091d5385f58. v1 merge: 65a403284c9f3906fbad2e59522185d55758b630. **Zero conflicts**, including Wire/CMakeLists.txt; no manual or semantic conflict resolution.

Neither requested seam was already committed. Both are in the single scratch-head commit:

[Feat] (MG_Backend, MG_Remote/Server): construct BackendObject_Remote directly in the split hook and bracket PipeApplier::ApplyOne with ScopedApplierEntry - the two merge-time hunks (ID-59)

Only Init.cpp, PipeApplier.cpp and ServerLoop.cpp changed: direct construction/include, weak factory deletion, and the const ScopedApplierEntry/include before the stamp, with LeaveApplier inside its lifetime. The supplied diff's older client preview fixes were excluded. v1's newer include context required placing the same added include beside PipeInputs.h; hunk intent and added text are unchanged. Identity was Swung0x48 <swung0x48@outlook.com> before commits; no attribution trailers.

Initial gen_pipe regeneration and --check both reported up to date. No generated file changed, so no regeneration commit. gen_pipe --self-test: 9 negative controls tripped, positive control OK. Field ownership --check: up to date. Dirty surface --check: 79 mutators, all mapped, no stale rows; 4 UNDECIDED rows remain explicitly unproved.

Only the merge hunks were committed. ID-42's later temporary source perturbation was restored byte-for-byte and rebuilt. Integration and other package worktrees were not edited or built in; their committed branches were consumed through git. No LFS fetch or adb access.

## 2. Full gate, in script order

Runner: ~/w7/notes/tools/wsl_p5_gate_joint.sh. Main transcript: **~/w7/p5-joint-gate.log**. The copied body changes only default TREE, LOG=p5j, and P5_JOINT_GATE_DONE; BASE remains ff2994d9. Baseline .READY was read as 4595163c7c439641ddb94f8a936eb2ccd5fb8b19.

The launcher p5_joint_launch.sh wraps the gate's git calls with submodlinks off/on. The copied link helper uses its own temporary filename and parses .gitmodules directly, avoiding git while links are on. The first nohup invocation returned before starting; the second, with a one-second startup allowance, ran successfully. The process was polled at approximately 60-second intervals; final process exit and the distinct marker were both observed. **Exit 0 is not an all-green verdict:** the supplied gate prints integration reds without propagating them to its final exit.

### Configure and build

```text
cfg build-linux rc=0
  cfg build-push rc=0
  cfg build-verify rc=0
  cfg build-split rc=0
  build build-linux rc=0
  build build-push rc=0
  build build-verify rc=0
  build build-split rc=0
```

The gate checked all four CTestTestfile.cmake artifacts before believing builds. No build failed.

### Part 1 — interface purity

```text
=== Part 1: interface purity
include-closure: 4 probes, 0 skipped, 0 problem(s)
gate C pGLContext in MG_Backend (must be empty): MobileGL/MG_Backend/MGPipe/PipeInputs.h:1 
G13 MG_State in PipeApply.h (must not be inside the ops block): 37:namespace MobileGL::MG_State::GLState { 40:} // namespace MobileGL::MG_State::GLState 843:    // (uploadTarget, level) as well (MG_State/GLState/TextureState/TextureObject.h), so a 1051:                                      const MG_State::GLState::LinkArtifacts* link, 1052:                                      const MG_State::GLState::SpirvArtifacts* spirv); 
G13b MGPipeUnmigratedEmulation sites in DirectGLES: MobileGL/MG_Backend/DirectGLES/Managers.cpp:2 MobileGL/MG_Backend/DirectGLES/DirectGLES.cpp:4 
G1-split nm MG_Remote  pull=0 (must be 0)  split=610 (must be > 0)
G1 symbols (admitted resizes: NONE for P5):
symbol-report: .text 10806611 -> 10806611 (+0, +0.000%)
symbol-report: 27814 -> 27814 defined symbols: 0 added, 0 removed, 0 resized, 0 renamed
G5 p3a_untouched_regions.sh rc=0 [p3a-untouched] byte-identical between ff2994d9 and HEAD
G5 p4a_untouched_regions.sh rc=0 [p4a-untouched] depth-stencil-sampling / format-caveat regions are byte-identical between ff2994d9 and HEAD
```

The gate C pGLContext hit is a comment at PipeInputs.h:793, not a backend read. The raw G13/G13b output above is retained without inventing another closure verdict.

### Part 5 — coverage and generators

```text
=== Part 5: coverage, poison, handle discipline
gen_pipe: generated files are up to date
gen_pipe: self-test: 9 negative-control trip(s), positive control OK
dirty-surface:   UNDECIDED, no verdict: UseProgram <- NEW_SHADER_IMAGES (the write analysis is not complete for this mutator: DestroyProgramSlot() writes 'attachedShaders', which it never declares - a member without the m_ prefix, a global, or a call result; none of which this script can place)
dirty-surface: the under-firing half read 1608 function bodies across 93 files under MobileGL/MG_State/GLState + MobileGL/MG_Impl/Pipe, macros expanded first, both sides resolved to MEM:<member> + FIELD:<member>.<leaf>; 284 of those bodies carry a write it could not attribute and taint whatever reaches them, 13 of the 79 mutators reach one; 473 members are written outside those roots and any absence claim over one is undecided; a mutating call on a member-rooted lvalue and a call resolved by name both only WIDEN what a mutator is credited with, and a FIELD token is not scoped to a type
dirty-surface self-test: 27 negative controls, all tripped; positive controls OK (SetPixelStoreParam's sixteen token-pasted writes are read at field level, and the seven reference-alias setters of RenderState.cpp resolve to m_parameters' fields)
gen_pipe_field_ownership: generated file is up to date
gen_pipe_field_ownership: self-test: 15 negative-control trip(s), each asserted against its OWN message; harness control OK; positive control OK (63 fields partitioned, 7 sticky forwards, 9 refusals)
emitter/CSO unit suites: 100% tests passed, 0 tests failed out of 185
verify controls (PoisonOmitted/VerifyCorrupted): 100% tests passed, 0 tests failed out of 4
```

### Part 3 — behavioural A/B

```text
G2 pull-vs-push name diff lines: 0
G14 removed vs before: 0 added: 42
build-linux unit: 100% tests passed, 0 tests failed out of 1816
build-push unit: 100% tests passed, 0 tests failed out of 1816
build-verify unit: 100% tests passed, 0 tests failed out of 1816
build-split unit: 100% tests passed, 0 tests failed out of 2038
split Wire suites: 100% tests passed, 0 tests failed out of 58
pull integration-gpu: 100% tests passed, 0 tests failed out of 1128
push integration-gpu: 100% tests passed, 0 tests failed out of 1128
split integration-gpu (monolith): 100% tests passed, 0 tests failed out of 1149
split integration-gpu (inproc):   53% tests passed, 538 tests failed out of 1149
three-arm name+status diff lines (must be 0): 1080
split integration-split: 100% tests passed, 0 tests failed out of 21
  of which skipping (packages c1/s1/v1 not landed yet): 2
```

Negative controls, verbatim:

```text
negative control: -L integration-split is RED in a build without the option, as it must be
split entries - passed: 19, failed: 0, skipped: 2 (ctest exit 0)
negative control E1 (MOBILEGL_IPC_VERB_BARRIER=0) turned 14 selected entries red, and the red carries the scenario's own diagnostic, as it must
negative control E3(a) (MOBILEGL_IPC_PERSISTENT_BLOCK_KB=0) turned 6 selected entries red, and the red carries the scenario's own diagnostic, as it must
  (E1/E3(a) above are scripts/ci/split_negative_controls.sh, the file CI runs)
negative control: a pull library under inproc is RED (1 case(s) named the transport, not the picture)
```

Persistent-map lanes, verbatim:

```text
persistent-map arm lanes: 100% tests passed, 0 tests failed out of 2
  persistent-map-arm-DirectGLES.log: pmap=0.00 mpr=1 
  persistent-map-arm-split-DirectGLES.log: pmap=2160.00 mpr=1
```

### Part 2 — semantic shadow and retraces

```text
=== Part 2: semantic shadow comparison
integration-verify: 100% tests passed, 0 tests failed out of 930
Fatal lines: 0
fixtures hydrated from ~/w7/pipe (no LFS fetch): 92 file(s)
split OpenRA retrace (E2): passed 2 / 2; failed: []
-- MGPipe split: OpenRA DirectGLES transport=inproc, Fatal{ lines in /home/swung/w7/retrace-out/p5-split/OpenRA/DirectGLES/output/mobilegl.log: 0
-- MGPipe split: OpenRA DirectVulkan transport=inproc, Fatal{ lines in /home/swung/w7/retrace-out/p5-split/OpenRA/DirectVulkan/output/mobilegl.log: 0
push retrace:   passed 79 / 79; failed: []
verify retrace: passed 79 / 79; failed: []
  verify armed: 79 of 79; logs with Fatal: 0
P5_JOINT_GATE_DONE
JOINT_GATE_PROCESS_EXIT=0
```

### Interpreting the gate figures

The unit/Wire figures are the gate's CTest summaries; its summ function discarded detailed status lists, so no extra unit skip counts are invented. Retained split-build JUnit gives:

| Transport | Passed | Skipped | Aborted | Failed | Segfault | Total |
|---|---:|---:|---:|---:|---:|---:|
| monolith | 954 | 195 | 0 | 0 | 0 | 1149 |
| inproc | 426 | 185 | 511 | 27 | 0 | 1149 |

Sources: ~/w7/p5j-split-{monolith,inproc}.xml and their igpu logs. Pull, push and split-monolith failure lists are empty. Appendix A copies the full split-inproc list from the gate's referenced sublog; the main gate prints only five failures. Name/status parity is **RED: 1080 diff lines**, despite name parity.

E3(a)'s “6 selected entries red” sentence is imprecise: it measured 4 failures and 2 skips. E1's helper returns after one matching file; jm separately inspected all 14 and found the named Fatal in each. That stronger condition is measured here, not enforced by the helper.

OpenRA summary JSON measures SSIM 1.0 on both backends. Preserved evidence: ~/w7/p5-joint-evidence/gate-openra/ (summaries, runner logs, private library logs), gate-push/ summaries, and ~/w7/p5j-retrace-{split,push,verify}-summary.txt. Push and verify failure lists are empty. Fixtures came only from the local hydrated integration copies; the gate printed 92 copied files.

### Part 4 — device recording

Not run: this package explicitly forbids adb. Device timings and an N-frame RSS slope are not invented desktop measurements.

## 3. Additional exit-gate controls

Runs used build-split, MOBILEGL_TRANSPORT=inproc and GLIBC_TUNABLES=glibc.malloc.tcache_count=0. Scripts: ~/w7/notes/tools/p5_joint_*. Evidence directory: **~/w7/p5-joint-evidence/**.

### E1 / E3(a)

The foreground integration-split -j 4 lane was repeated three times: **each 19 passed / 2 skipped / 0 failed, total 21** (split-repeat-{1,2,3}.{out,xml}). The skips are default and SmallRing TheMapLandsInTheArmItsLaneDeclares, whose bodies require the dedicated counting-lane marker. The dedicated PersistentMapArm entry passes.

An earlier background-launch run measured 16 passed / 1 skipped / 4 SIGHUP failures. It is preserved as split.{out,xml} and split-private/. The signal failures occurred around launcher closure with no recorded Fatal. They are reported as a runner interruption, not silently dropped or assigned to package code. The following CI-control baseline and all three foreground repeats were clean.

CI invocation was read from test.yml: from build-split, MOBILEGL_ITEST_REQUIRE_GPU=1, CONTROL_TMPDIR set, bash ../scripts/ci/split_negative_controls.sh, no positional arguments. jm also supplied the transport and GLIBC_TUNABLES above. Transcript: controls.out; rc=0. Its baseline: 19 pass / 0 fail / 2 skip.

E1: **14 selected / 14 aborted / 14 fresh private files with BarrierViolation**. Example:

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```

Source: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear.log. Appendix B records every selected file.

E3(a): **6 selected / 4 failed / 2 skipped**. There is **no Fatal for block size zero**: PersistentMapTracker.cpp returns without pushing when blockBytes==0. No nonexistent private-file Fatal is quoted. The required evidence is the scenario's pixel assertion:

```text
Value of: WholeSurfaceIs(afterSecond, "red", "the SECOND write through the same mapping, announced by nothing: " "this is exit gate E3(b), and a 'push on the next explicit buffer " "operation' implementation reads back the FIRST write's colour here")
```

Source: ~/w7/p5-joint-evidence/controls/control-output.txt, also included in controls.out. The four failing names are preserved there; controls-private/ preserves the library files.

### ID-42 — emulated-direction red-check VERIFIED

Used the exact DirectGLES.Split.PersistentCoherentMapScenario selection, with GoogleTest XML to preserve the production arm property.

| Phase | Passed | Skipped | Failed | Build |
|---|---:|---:|---:|---|
| original | 2 | 1 | 0 | existing joint binary |
| return false at top of IsLivePersistentMap | 0 | 0 | 3 | succeeded |
| restored byte-for-byte | 2 | 1 | 0 | succeeded |

All three green-phase GoogleTest XML files report persistent_map_arm=emulated. The two behavioral cases executed; the non-counting case intentionally skips. The mutation also failed that last entry's setup, hence three red entries. Required text observed:

> This map landed in the emulated arm immediately after the map and the predicate did not make it a member.

Evidence: id42-{green,red,restored}.{out,xml}, id42-*-gtest/*.xml and id42-{red,restored}-build.out. Final git diff of PersistentMapTracker.cpp is empty. The predicate control went red for its own assertion, then returned green.

### Audit and its R-16 limit

MOBILEGL_IPC_AUDIT=1 integration-split: **19 passed / 2 skipped / 0 failed, total 21**. Evidence: audit.{out,xml} and audit-private/.

Existing ctest entries TheAuditPoisonFillsExactlyTheStagedRunAfterTheApplierReturns and StagedShadowProductionTest.SubDataThroughTheRealOpsTableCopiesAndSurvivesTheSourcePoison both passed in additional.{out,xml}. They test poisoning and the production SubData copy.

The separate v1 M-2 corrupt-staged-upload/draw perturbation is not a shipped ctest entry: **not exercised here**. Audit-green is not promoted into a newly verified R-16 corrupt-draw negative control. Follow-up owner: v1 / integrator.

### Strict errors — recorded residual-input debt

MOBILEGL_IPC_STRICT_ERRORS=1 integration-split: **19 subprocess-aborted / 2 skipped / 0 passed, total 21**. First Fatal in the sorted private-file inventory:

```text
[09:43:13] [Linux mgl-srv-apply/FATAL]: MGPipe: Fatal{UnmigratedPipeInput, "GetTextureContextId@Clear"} [BARRIER-PULLED, MOBILEGL_IPC_STRICT_ERRORS=1, retires in P3b/P4b]
```

Source: /home/swung/w7/p5-joint-evidence/strict-private/DirectGLES.Split.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear.log. Evidence: strict.{out,xml}, strict-private/. This is expected R-7 debt, not a default-lane regression; the diagnostic assigns this field to P3b/P4b.

### E2 clear-drop control — NOT VERIFIED: remained green

The extra run used MOBILEGL_IPC_E2_DROP_CLEAR=1 with inproc, this scratch split library and --only OpenRA.DirectGLES. It passed **1/1, SSIM 1.0, mismatchPixels=0**, threshold 0.99. The private library log proves the knob was armed:

```text
[09:45:15] [Linux mobilegl_trace_/WARN]: MG_Remote client: MOBILEGL_IPC_E2_DROP_CLEAR=1 - E2's NEGATIVE CONTROL is armed and every glClear will be DROPPED on the wire. This arm is expected to fail its SSIM threshold; a lane that stays green with it set is not going through the wire at all
```

Evidence: e2-drop-clear.out and e2-drop-clear/{summary.json,OpenRA.DirectGLES.log,OpenRA/DirectGLES/output/mobilegl.log}. This falsifies the control's expected red, not the measured baseline SSIM. The armed-knob message alone does not prove a Clear was reached/dropped or that dropping it must affect the final snapshot. **No wire bypass is inferred.** Route to c1/t1 and the integrator to establish a load-bearing E2 perturbation.

## 4. Independent census and attribution

Command: MOBILEGL_TRANSPORT=inproc ctest -L integration-gpu -j 8, with --output-on-failure and JUnit. **426 passed / 185 skipped / 511 subprocess-aborted / 27 failed / 0 segfault = 1149.** It exactly matches the gate's independent run. Sources: census.{out,xml}, census-status.json, manifest.json.

Each of the 27 ordinary failures was rerun using the discovered executable/arguments/environment and working directory, with MOBILEGL_LOG_FILE_PATH=~/w7/p5-joint-probe-<n>.log. All 27 returned rc=1; zero Fatal lines appeared in their private files. No segfaulted entry existed to probe. Appendix C quotes a line from every probe and gives both its private library log and GoogleTest transcript.

| Family | Failed entries | Attribution | Evidence and limits |
|---|---:|---|---|
| LayeredAttachmentShapeScenario | 14 | P4b/P7 debt: layered attachments / texture readback | poison-valued texels, including cube-face GetTexImage; no class-C Fatal, so not mislabeled as one |
| DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture | 3 | P4b/P7 texture-shadow readback debt; shared backend conversion follow-up | wrong packed words; GetTexImage, not ID-49's tight ReadPixels |
| HandleRecycleScenario framebuffer case | 5 | P4b/P7 texture-readback debt | HandleRecycleScenario.cpp:1318 reads both attachments via glGetTexImage; split GL_Texture.cpp:6804 routes to client-shadow fallback. Not proof of a recycled-handle bug |
| PrimitivesGeneratedNoXfbScenario | 3 | P7 query/primitive-accounting debt | generated count 0 / missing Magma reroute marker; query/XFB is outside BRIEF §4's reduced path |
| TextureParamsWithoutASamplerViewScenario | 1 | shared backend / inspection harness | native driver query in PipeApplyPeek.cpp:98 has no apply-thread forwarder; caller-thread zeros do not independently prove a server swizzle defect |
| P4aFinalFixScenario renderbuffer/framebuffer deletion | 1 | shared backend FBO/RBO lifetime following transported deletes | clear succeeds, later draw black; exact c1/v1/backend fix site not isolated. Route to shared backend/integrator, not a proved “none v1” claim |

These are triage destinations with stated limits, not fixes or causal claims inferred solely from names. In particular, this run does not independently prove v1 owns none of the wrong answers.

The three query and five framebuffer-recycling entries each received **three additional standalone reruns**, 24 extra runs total. Every rerun returned rc=1. **No flake demonstrated**: v1-v2's standalone-green claim is not reproduced on this joint head.

The 511 aborts were counted, not exhaustively assigned a Fatal category. Two measured samples are class-C by design:

- DirectGLES.CrossFrameBufferScenario.VertexBufferSubData: /home/swung/w7/p5-joint-probe-54.log

```text
[09:48:10] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{UnmigratedVerb, "DrawElements"}
```

- DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone: /home/swung/w7/p5-joint-probe-55.log

```text
[09:48:10] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{UnmigratedVerb, "BeginTransformFeedback"}
```

Those samples do not establish every abort's diagnostic; per-abort probing was not required. The historical “all aborts are class C” claim is not repeated as a measured fact.

Comparison input **c1-v3's historical 420/182/517/27/3** versus this joint: +6 passed, +3 skipped, -6 aborted, unchanged ordinary failures, -3 segfault. The current zero-segfault result and green persistent-map reduced path agree with the intent of v1's newer fixes, but aggregate deltas do not assign individual transitions. Compared with v1-v2's historic family table, jm measures 14 layered failures and 5 recycling failures here and does not reuse its 23-failure headline. Its printed overall census does not sum to this lane's measured total and is not reused.

## 5. R-10, scenario statistics and segment ledger

Two inproc entries ran with MOBILEGL_PIPE_STATS=1 and MOBILEGL_PIPE_STATS_PERIOD=1. These are real nonzero-window library lines, excluding teardown zeros.

| Entry | Frame/window | pmap bytes/frame | mpr in window | rsp in window |
|---|---|---:|---:|---:|
| TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary | 1/1 | 0.00 | 0 | 35 |
| same | 2/1 | 0.00 | 0 | 35 |
| PersistentCoherentMapScenario.AWriteAfterAFrameBoundaryReachesTheNextFramesDraw | 1/1 | 600.00 | 1 | 35 |
| same | 2/1 | 600.00 | 0 | 35 |

Sources: ~/w7/p5-joint-probe-52.log and -53.log, with corresponding probe XML/out in the evidence directory. The gate's separate counting lane pmap=2160.00/mpr=1 covers another workload/window and is not substituted for these measurements.

| Entry | Server accept peakRss | Client handshake peakRss | Client teardown peakRss |
|---|---:|---:|---:|
| Triangle | 11526144 | 11706368 | 141451264 |
| Persistent map | 11464704 | 11649024 | 141422592 |

Units: bytes. The library explicitly says peakRss is the **process's**, shared by both inproc roles. Server prints only at accept here; that early sample is not its independent full-run physical peak. The log measures roleMapped=58990592 per role and allRolesMapped=117981184 once both roles are mapped. No N-frame RSS slope is claimed.

**Maximum record bytes: NO MEASUREMENT for these scenarios.** Neither private/stats log publishes it. MaxRecordBytesSeen exists on the encoder, but its consumers found by source search are codec tests, not scenario telemetry. MaxRecordBytesSeenStaysFarBelowHalfTheRing passed; that is not a numeric maximum for either scenario. Follow-up: w1/s1/t1 telemetry. No value is inferred from structs or configured capacity.

The canonical ledger is pinned by the executed, passing ProtocolSmokeTest.WelcomeCarriesTheFourSegmentAnnouncements: **SEG_CMD 8 MiB / SEG_STAGE 32 MiB / SEG_REPLY 16 MiB / SEG_EVENT 256 KiB**. Source creates and reads back all four sizes, including ID-47's reply size. This announcement ledger is distinct from measured mapped bytes. additional.{out,xml} contains this test, the max-record test and both audit/copy tests: **4 passed, 0 skipped, 0 failed**.

## 6. Exit-gate disposition and handoff

- **E1:** default reduced path clean; all 14 selected private files have the barrier Fatal under knob zero.
- **E2:** baseline 2/2 at SSIM 1.0; pull-library transport control works; **clear-drop SSIM control failed to turn red**. c1/t1/integrator own the remaining R-16 obligation.
- **E3(a/c):** block-zero red for its own pixel assertion; mpr=1 in both counted arms and split pmap nonzero. ID-42 emulated membership VERIFIED through green/red/restored. SmallRing entries ran, but **no back-pressure wait count was measured**: a small configured capacity and marker are not proof of E3(e)'s wait. Follow-up t1/s1.
- **E4:** field generator and own-message negative self-tests pass; strict runtime red names the residual field. rsp is recorded debt, not a default-path failure.
- **E5:** shipped poisoning/copy controls pass and audit lane is clean. The separate corrupt-draw perturbation was **not exercised here**; its R-16 half is not closed.
- **E6:** G1/G2/G14 and monolith/verify/retraces green; broad inproc and status parity red. The integrator must adjudicate reduced-path/debt exclusions; jm has not silently waived them.

No build failure or semantic merge conflict occurred. No package bug was fixed. At the post-ID-42 check (09:48), the restored tree had empty git status --short after submodlinks off, empty diff --check, and empty diff for PersistentMapTracker.cpp. A later final check found a concurrent APK job had edited android-plugin/build.gradle.kts at 09:49:57, after all jm measurements. This edit is not jm’s and is outside jm’s grant; its full file is preserved in evidence/concurrent-apk-build.gradle.kts, the matching HEAD blob in evidence/concurrent-apk-base-build.gradle.kts, and its patch in evidence/concurrent-apk-edit.diff. Clean handoff is pending coordination; current status is M android-plugin/build.gradle.kts. The branch head remains e61d00123c41d74f98cd06b63768be12d9b6348a and feat/disaggregated remains c3e736a3439785f59472b24dcd0cc32f3ea426c6.

Retained evidence/reproducers:
- ~/w7/p5-joint-gate.log, p5-joint-step3.log, p5-joint-followup.log, p5-joint-probes.log.
- ~/w7/p5-joint-probe-1.log through -55.log, all indexed by evidence/probes.json and the appendices.
- ~/w7/p5-joint-evidence/ (run outputs, XML, JSON, private snapshots and small retrace results).
- ~/w7/p5j-* gate sublogs, symbols, name lists, and retrace summaries.
- ~/w7/p5-joint-submodlinks.sh and notes/tools/{wsl_p5_gate_joint.sh,p5_joint_*.sh,p5_joint_*.py}.

These retained root p5-joint-* files are evidence or reproducers cited here; no uncited root temporary file remains. The extra control's expanded OpenRA input trace is deleted after execution; its result, images and logs remain. No glob deletion was used.

## Appendix A — full gate failure lists

Pull: empty. Push: empty. Split-monolith: empty. Split-inproc, copied from ~/w7/p5j-igpu-split-inproc.log (the main gate prints only five):

```text
2050 - DirectGLES.CrossFrameBufferScenario.VertexBufferSubData (Subprocess aborted) integration-gpu
	2051 - DirectGLES.CrossFrameBufferScenario.VertexMapWriteUnmap (Subprocess aborted) integration-gpu
	2052 - DirectGLES.CrossFrameBufferScenario.VertexPersistentMapFlush (Subprocess aborted) integration-gpu
	2053 - DirectGLES.CrossFrameBufferScenario.VertexPersistentCoherentWrite (Subprocess aborted) integration-gpu
	2054 - DirectGLES.CrossFrameBufferScenario.VertexOrphanAndReupload (Subprocess aborted) integration-gpu
	2055 - DirectGLES.CrossFrameBufferScenario.VertexCopyBufferSubData (Subprocess aborted) integration-gpu
	2056 - DirectGLES.CrossFrameBufferScenario.IndexBufferSubData (Subprocess aborted) integration-gpu
	2057 - DirectGLES.CrossFrameBufferScenario.IndexMapWriteUnmap (Subprocess aborted) integration-gpu
	2058 - DirectGLES.CrossFrameBufferScenario.IndexPersistentMapFlush (Subprocess aborted) integration-gpu
	2059 - DirectGLES.CrossFrameBufferScenario.IndexPersistentCoherentWrite (Subprocess aborted) integration-gpu
	2060 - DirectGLES.CrossFrameBufferScenario.IndexOrphanAndReupload (Subprocess aborted) integration-gpu
	2061 - DirectGLES.CrossFrameBufferScenario.IndexCopyBufferSubData (Subprocess aborted) integration-gpu
	2062 - DirectGLES.CrossFrameBufferScenario.PartialStalenessIsCaughtByWholeRegionChecks (Subprocess aborted) integration-gpu
	2063 - DirectGLES.StreamedArenaScenario.StreamedVertexDataSurvivesArenaRecycling (Subprocess aborted) integration-gpu
	2064 - DirectGLES.StreamedArenaScenario.StreamedIndexDataSurvivesArenaRecycling (Subprocess aborted) integration-gpu
	2065 - DirectGLES.ResidentIndexScenario.PersistentCoherentEboWrittenEveryFrame (Subprocess aborted) integration-gpu
	2066 - DirectGLES.ResidentIndexScenario.EboAlsoBoundAsVertexBufferLater (Subprocess aborted) integration-gpu
	2067 - DirectGLES.ResidentIndexScenario.EboDeletedAndRecreatedAtTheSameName (Subprocess aborted) integration-gpu
	2068 - DirectGLES.ResidentIndexScenario.OneEboTwoVaosMutatedAcrossFrames (Subprocess aborted) integration-gpu
	2069 - DirectGLES.ResidentIndexScenario.PromotedDynamicEbo (Subprocess aborted) integration-gpu
	2070 - DirectGLES.MultiDrawScenario.BaseVertexBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2071 - DirectGLES.MultiDrawScenario.PlainBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2072 - DirectGLES.MultiDrawScenario.UnsignedShortBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2073 - DirectGLES.MultiDrawScenario.UnsignedByteBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2074 - DirectGLES.MultiDrawScenario.BaseVertexBeyondIndexTypeRangeMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2075 - DirectGLES.MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2076 - DirectGLES.MultiDrawScenario.PrimitiveRestartStripBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2077 - DirectGLES.MultiDrawScenario.PrimitiveRestartUnsignedShortBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2078 - DirectGLES.MultiDrawScenario.ZeroCountSubDrawsMatchUnrolledDraws (Subprocess aborted) integration-gpu
	2079 - DirectGLES.MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments (Subprocess aborted) integration-gpu
	2081 - DirectGLES.DrawParametersScenario.DrawArraysInstancedBaseInstanceReportsItsBaseInstance (Subprocess aborted) integration-gpu
	2082 - DirectGLES.DrawParametersScenario.APlainDrawAfterABaseInstancedOneSeesZeroAgain (Subprocess aborted) integration-gpu
	2083 - DirectGLES.DrawParametersScenario.DrawElementsBaseVertexReportsItsBaseVertex (Subprocess aborted) integration-gpu
	2084 - DirectGLES.DrawParametersScenario.DrawElementsAfterABaseVertexDrawReportsZeroAgain (Subprocess aborted) integration-gpu
	2085 - DirectGLES.DrawParametersScenario.MultiDrawArraysNumbersItsSubDraws (Subprocess aborted) integration-gpu
	2086 - DirectGLES.DrawParametersScenario.MultiDrawElementsIndirectCarriesEveryCommandsParameters (Subprocess aborted) integration-gpu
	2087 - DirectGLES.DrawParametersScenario.MultiDrawArraysIndirectCountObeysItsParameterBuffer (Subprocess aborted) integration-gpu
	2094 - DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone (Subprocess aborted) integration-gpu
	2095 - DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceEnables (Subprocess aborted) integration-gpu
	2096 - DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadLines8 (Subprocess aborted) integration-gpu
	2097 - DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadPoints1 (Subprocess aborted) integration-gpu
	2098 - DirectGLES.XfbAfterClipDistanceScenario.SkipComponentsCaptureSurvivesEveryClipWorkloadStopPoint (Subprocess aborted) integration-gpu
	2099 - DirectGLES.UnwrittenPositionOutputScenario.ARedeclaredButUnwrittenPositionStillDraws (Subprocess aborted) integration-gpu
	2101 - DirectGLES.UnwrittenPositionOutputScenario.AWrittenRedeclaredPositionStillDraws (Subprocess aborted) integration-gpu
	2102 - DirectGLES.UnwrittenPositionOutputScenario.CapturingAWrittenPositionStillDraws (Subprocess aborted) integration-gpu
	2103 - DirectGLES.UnwrittenPositionOutputScenario.AShaderWithNoPositionBlockStillDraws (Subprocess aborted) integration-gpu
	2109 - DirectGLES.SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenAFilterChangeCompletesTheTexture (Subprocess aborted) integration-gpu
	2110 - DirectGLES.SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture (Subprocess aborted) integration-gpu
	2125 - DirectGLES.AdvertisedLimitsScenario.ImageUnitBindingsAreReportedFromTheFrontendState (Subprocess aborted) integration-gpu
	2130 - DirectGLES.PrimitiveRestartScenario.AnArbitraryRestartIndexDrawsInsteadOfKillingTheProcess (Subprocess aborted) integration-gpu
	2131 - DirectGLES.PrimitiveRestartScenario.TheFixedIndexValueTakesTheForwardingPath (Subprocess aborted) integration-gpu
	2132 - DirectGLES.PrimitiveRestartScenario.WithoutTheCapTheStripWeldsAcrossTheGap (Subprocess aborted) integration-gpu
	2133 - DirectGLES.PrimitiveRestartScenario.ChangingTheRestartIndexBetweenDrawsIsHonoured (Subprocess aborted) integration-gpu
	2135 - DirectGLES.PrimitiveRestartScenario.ARestartIndexTooLargeForTheIndexTypeRestartsNowhere (Subprocess aborted) integration-gpu
	2136 - DirectGLES.PrimitiveRestartScenario.AnAllOnesVertexIndexSurvivesTheSubstitution (Subprocess aborted) integration-gpu
	2155 - DirectGLES.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (Failed) integration-gpu
	2156 - DirectGLES.DepthStencilReadbackMatrixScenario.CopyTexImageFromADepthAttachmentSurvivesAReadBack (Subprocess aborted) integration-gpu
	2171 - DirectGLES.ViewportArrayScenario.EachViewportIndexUsesItsOwnDepthRange (Subprocess aborted) integration-gpu
	2176 - DirectGLES.SsboArrayLengthScenario.WholeBufferBindingsReportTheElementCount (Subprocess aborted) integration-gpu
	2177 - DirectGLES.SsboArrayLengthScenario.RangeBindingsReportTheBoundWindow (Subprocess aborted) integration-gpu
	2178 - DirectGLES.SsboArrayLengthScenario.AReadonlyWriteonlyArrayStillReportsItsLength (Subprocess aborted) integration-gpu
	2179 - DirectGLES.DoublePrecisionScenario.ADoubleUniformReachesTheShaderAtFloatPrecision (Subprocess aborted) integration-gpu
	2180 - DirectGLES.DoublePrecisionScenario.EveryDoubleShapeLandsInItsOwnSlot (Subprocess aborted) integration-gpu
	2181 - DirectGLES.DoublePrecisionScenario.TheTransposeFlagStillTransposes (Subprocess aborted) integration-gpu
	2184 - DirectGLES.DoublePrecisionScenario.ADoubleUniformKeepsItsDeclaredInitializer (Subprocess aborted) integration-gpu
	2185 - DirectGLES.DoublePrecisionScenario.EveryDoubleUniformShapeArrivesWhereTheShaderReadsIt (Subprocess aborted) integration-gpu
	2186 - DirectGLES.DoublePrecisionScenario.TheConformanceUniformShaderAgreesWithEveryValueItWasGiven (Subprocess aborted) integration-gpu
	2190 - DirectGLES.DoublePrecisionScenario.AStorageBlockWithDoublesKeepsTheLayoutItWasBoundWith (Subprocess aborted) integration-gpu
	2191 - DirectGLES.UniformInitializerScenario.AnUnsetUniformReadsItsDeclaredInitializer (Subprocess aborted) integration-gpu
	2192 - DirectGLES.UniformInitializerScenario.AnApplicationWriteBeatsTheInitializer (Subprocess aborted) integration-gpu
	2193 - DirectGLES.UniformInitializerScenario.RelinkingRestoresTheInitializer (Subprocess aborted) integration-gpu
	2194 - DirectGLES.SwizzleAccessRoutineScenario.EveryAccessRoutineFetchesTheSameTexelUnderTheIdentitySwizzle (Subprocess aborted) integration-gpu
	2195 - DirectGLES.SwizzleAccessRoutineScenario.EveryAccessRoutineSeesAReversedSwizzle (Subprocess aborted) integration-gpu
	2196 - DirectGLES.SwizzleAccessRoutineScenario.RepeatedProgramBuildsKeepFetchingTheSameTexel (Subprocess aborted) integration-gpu
	2197 - DirectGLES.SwizzleAccessRoutineScenario.EveryAccessRoutineSeesConstantSwizzleSources (Subprocess aborted) integration-gpu
	2206 - DirectGLES.ProgramPipelineScenario.ComputeAndGraphicsStagesShareOnePipeline (Subprocess aborted) integration-gpu
	2207 - DirectGLES.ProgramPipelineScenario.AStageProgramsStorageBlockBindingReachesThePipelineDraw (Subprocess aborted) integration-gpu
	2208 - DirectGLES.ProgramPipelineScenario.AStorageBlockRebindingHoldsWithoutAPipeline (Subprocess aborted) integration-gpu
	2214 - DirectGLES.ImageLoadStoreSsoScenario.PerElementImageUnitsReachAPipelineDraw (Subprocess aborted) integration-gpu
	2216 - DirectGLES.ImageLoadStoreSsoScenario.ReassigningAnImageUnitBetweenDrawsReachesTheNextDraw (Subprocess aborted) integration-gpu
	2217 - DirectGLES.ImageTargetKindScenario.LoadsTexture1D (Subprocess aborted) integration-gpu
	2218 - DirectGLES.ImageTargetKindScenario.LoadsTexture1DArray (Subprocess aborted) integration-gpu
	2219 - DirectGLES.ImageTargetKindScenario.LoadsTexture2D (Subprocess aborted) integration-gpu
	2220 - DirectGLES.ImageTargetKindScenario.LoadsTexture2DArray (Subprocess aborted) integration-gpu
	2221 - DirectGLES.ImageTargetKindScenario.LoadsTexture3D (Subprocess aborted) integration-gpu
	2222 - DirectGLES.ImageTargetKindScenario.LoadsTextureBuffer (Subprocess aborted) integration-gpu
	2223 - DirectGLES.ImageTargetKindScenario.LoadsTextureCube (Subprocess aborted) integration-gpu
	2224 - DirectGLES.ImageTargetKindScenario.LoadsTextureCubeArray (Subprocess aborted) integration-gpu
	2225 - DirectGLES.ImageTargetKindScenario.LoadsTextureRectangle (Subprocess aborted) integration-gpu
	2226 - DirectGLES.ImageTargetKindScenario.LoadsTexture2DMultisample (Subprocess aborted) integration-gpu
	2227 - DirectGLES.ImageTargetKindScenario.LoadsTexture2DMultisampleArray (Subprocess aborted) integration-gpu
	2228 - DirectGLES.ImageTargetKindScenario.StoresTexture1D (Subprocess aborted) integration-gpu
	2229 - DirectGLES.ImageTargetKindScenario.StoresTexture1DArray (Subprocess aborted) integration-gpu
	2230 - DirectGLES.ImageTargetKindScenario.StoresTexture2D (Subprocess aborted) integration-gpu
	2231 - DirectGLES.ImageTargetKindScenario.StoresTexture2DArray (Subprocess aborted) integration-gpu
	2232 - DirectGLES.ImageTargetKindScenario.StoresTexture3D (Subprocess aborted) integration-gpu
	2233 - DirectGLES.ImageTargetKindScenario.StoresTextureBuffer (Subprocess aborted) integration-gpu
	2234 - DirectGLES.ImageTargetKindScenario.StoresTextureCube (Subprocess aborted) integration-gpu
	2235 - DirectGLES.ImageTargetKindScenario.StoresTextureCubeArray (Subprocess aborted) integration-gpu
	2236 - DirectGLES.ImageTargetKindScenario.StoresTextureRectangle (Subprocess aborted) integration-gpu
	2237 - DirectGLES.ImageTargetKindScenario.StoresTexture2DMultisample (Subprocess aborted) integration-gpu
	2238 - DirectGLES.ImageTargetKindScenario.StoresTexture2DMultisampleArray (Subprocess aborted) integration-gpu
	2239 - DirectGLES.ImageTargetKindScenario.AtomicallyAddsToTexture1D (Subprocess aborted) integration-gpu
	2240 - DirectGLES.ImageTargetKindScenario.AtomicallyAddsToTexture1DArray (Subprocess aborted) integration-gpu
	2241 - DirectGLES.ImageTargetKindScenario.IgnoresLayerForTexture2D (Subprocess aborted) integration-gpu
	2242 - DirectGLES.ImageTargetKindScenario.IgnoresLayerForTexture1D (Subprocess aborted) integration-gpu
	2243 - DirectGLES.ImageTargetKindScenario.IgnoresLayerForTextureRectangle (Subprocess aborted) integration-gpu
	2244 - DirectGLES.ImageTargetKindScenario.IgnoresLayerForTexture2DMultisample (Subprocess aborted) integration-gpu
	2245 - DirectGLES.ImageTargetKindScenario.AllKindsInOneProgram (Subprocess aborted) integration-gpu
	2246 - DirectGLES.ImageFormatQualifierScenario.AFormatlessWriteonlyImageWrites (Subprocess aborted) integration-gpu
	2247 - DirectGLES.ImageFormatQualifierScenario.RebindToADifferentFormatRebuilds (Subprocess aborted) integration-gpu
	2248 - DirectGLES.ImageFormatQualifierScenario.FirstBindAfterLinkRebuilds (Subprocess aborted) integration-gpu
	2249 - DirectGLES.ImageFormatQualifierScenario.ADeclaredFormatStillWins (Subprocess aborted) integration-gpu
	2250 - DirectGLES.NonCoreImageFormatScenario.TwoChannelFloatImageDropsSurplusStoresAndLoadsZeroOne (Subprocess aborted) integration-gpu
	2251 - DirectGLES.NonCoreImageFormatScenario.PackedFloatImageDecodesItsUploadAndDropsSurplusStores (Subprocess aborted) integration-gpu
	2252 - DirectGLES.NonCoreImageFormatScenario.PackedIntegerImageSplitsItsUploadAndKeepsAllFourChannels (Subprocess aborted) integration-gpu
	2253 - DirectGLES.NonCoreImageFormatScenario.SingleChannelUnsignedImageDropsSurplusStoresAndLoadsZeroOne (Subprocess aborted) integration-gpu
	2254 - DirectGLES.NonCoreImageFormatScenario.UnsignedNormalizedImageCarriesItsCodesBothWays (Subprocess aborted) integration-gpu
	2255 - DirectGLES.NonCoreImageFormatScenario.SignedNormalizedImageSignExtendsItsCodesAndClampsAtMinusOne (Subprocess aborted) integration-gpu
	2256 - DirectGLES.NonCoreImageFormatScenario.TenTenTenTwoImageUsesItsOwnPerChannelDenominators (Subprocess aborted) integration-gpu
	2257 - DirectGLES.NonCoreImageFormatScenario.NormalizedImageCarriesEveryLayerFaceOfACubeMapArray (Subprocess aborted) integration-gpu
	2258 - DirectGLES.NonCoreImageFormatScenario.BufferImageAddressesTheApplicationsOwnTexels (Subprocess aborted) integration-gpu
	2259 - DirectGLES.NonCoreImageFormatScenario.ASplitBufferImageStillSamplesWholeTexels (Subprocess aborted) integration-gpu
	2260 - DirectGLES.NonCoreImageFormatScenario.ATwoChannelImageTextureStillSamplesAsRGZeroOne (Subprocess aborted) integration-gpu
	2261 - DirectGLES.ImageSizeAfterRespecScenario.ADrawSeesTheNewSizeOfARespecifiedImageTexture (Subprocess aborted) integration-gpu
	2277 - DirectGLES.IoBlockNameCollisionScenario.DistinctlyNamedBlocksCarryThePayloadThroughFiveStages (Subprocess aborted) integration-gpu
	2278 - DirectGLES.IoBlockNameCollisionScenario.OneBlockNameInBothDirectionsStillCarriesThePayload (Subprocess aborted) integration-gpu
	2279 - DirectGLES.UnlocatedIoBlockScenario.BlocksCarryTheirPayloadThroughFiveStages (Subprocess aborted) integration-gpu
	2280 - DirectGLES.UnlocatedIoBlockScenario.BlocksNamedInBothDirectionsStillMeetWithoutLocations (Subprocess aborted) integration-gpu
	2282 - DirectGLES.TessellationDrawModeScenario.TessellationProgramRejectsNonPatchModes (Subprocess aborted) integration-gpu
	2283 - DirectGLES.TessellationDrawModeScenario.PatchesRejectedWithoutATessellationEvaluationStage (Subprocess aborted) integration-gpu
	2285 - DirectGLES.GeometryDrawModeScenario.PointsInGeometryProgramRejectsNonPointModesOnFeedbackDraws (Subprocess aborted) integration-gpu
	2291 - DirectGLES.FormatlessImageBakeScenario.R8uiBakedFromTheBoundUnitStillReachesTheDriver (Subprocess aborted) integration-gpu
	2292 - DirectGLES.FormatlessImageBakeScenario.R16uiBakedFromTheBoundUnitStillReachesTheDriver (Subprocess aborted) integration-gpu
	2293 - DirectGLES.FormatlessImageBakeScenario.CoreFormatBakedFromTheBoundUnitIsUnaffected (Subprocess aborted) integration-gpu
	2297 - DirectGLES.BufferTextureScenario.AnImageStoreIntoABufferTextureIsVisibleToTheCpu (Subprocess aborted) integration-gpu
	2299 - DirectGLES.VertexAttribBindingScenario.FormatAndBindingFeedTheDraw (Subprocess aborted) integration-gpu
	2300 - DirectGLES.VertexAttribBindingScenario.FormatBeforeBufferStillResolves (Subprocess aborted) integration-gpu
	2301 - DirectGLES.VertexAttribBindingScenario.BindingStrideZeroRepeatsOneElement (Subprocess aborted) integration-gpu
	2302 - DirectGLES.VertexAttribBindingScenario.PointerStrideZeroStaysTightlyPacked (Subprocess aborted) integration-gpu
	2303 - DirectGLES.VertexAttribBindingScenario.BindingDivisorAppliesToEveryAttributeOnThePoint (Subprocess aborted) integration-gpu
	2304 - DirectGLES.VertexAttribBindingScenario.BaseInstanceMovesTheInstancedArraysStartElement (Subprocess aborted) integration-gpu
	2305 - DirectGLES.VertexAttribBindingScenario.BaseInstanceLeavesPerVertexArraysWhereTheyWere (Subprocess aborted) integration-gpu
	2306 - DirectGLES.VertexAttribBindingScenario.RelativeOffsetComposesWithBindingOffset (Subprocess aborted) integration-gpu
	2307 - DirectGLES.VertexAttribBindingScenario.InputArrayCaptureProgramFeedsTheDraw (Subprocess aborted) integration-gpu
	2308 - DirectGLES.VertexAttribBindingScenario.EveryInputArrayElementGetsItsOwnCurrentValue (Subprocess aborted) integration-gpu
	2309 - DirectGLES.VertexAttribBindingScenario.DoubleArrayIsFetchedAtFloat32Precision (Subprocess aborted) integration-gpu
	2310 - DirectGLES.VertexAttribBindingScenario.NormalizedIsIgnoredForDoubleArrays (Subprocess aborted) integration-gpu
	2311 - DirectGLES.VertexAttribBindingScenario.LongDoubleArrayIsFetchedAtFloat32Precision (Subprocess aborted) integration-gpu
	2312 - DirectGLES.XfbCaptureBufferReuseScenario.EverySpanIntoABufferObjectOfItsOwn (Subprocess aborted) integration-gpu
	2313 - DirectGLES.XfbCaptureBufferReuseScenario.EverySpanIntoOneRespecifiedBufferObject (Subprocess aborted) integration-gpu
	2314 - DirectGLES.XfbCaptureBufferReuseScenario.ARespecificationMayChangeTheCaptureBufferSize (Subprocess aborted) integration-gpu
	2315 - DirectGLES.XfbCaptureBufferReuseScenario.EverySpanIntoOneImmutableStorageBuffer (Subprocess aborted) integration-gpu
	2316 - DirectGLES.XfbPrimitiveQueryScenario.ACaptureThatFitsReportsEveryPrimitiveOnBothTargets (Subprocess aborted) integration-gpu
	2317 - DirectGLES.XfbPrimitiveQueryScenario.AnOverflowingVertexOnlyCaptureStopsWritingAtTheBufferCapacity (Subprocess aborted) integration-gpu
	2326 - DirectGLES.XfbRepeatedCaptureScenario.ASecondGeometryCaptureAfterADeqpStateResetStillRecords (Subprocess aborted) integration-gpu
	2327 - DirectGLES.XfbRepeatedCaptureScenario.ACaptureFromAPatchesDrawRecords (Subprocess aborted) integration-gpu
	2328 - DirectGLES.XfbRepeatedCaptureScenario.ACaptureFromAFragmentlessProgramRecords (Subprocess aborted) integration-gpu
	2329 - DirectGLES.XfbRepeatedCaptureScenario.ACaptureFromAnAdjacencyDrawRecords (Subprocess aborted) integration-gpu
	2330 - DirectGLES.XfbRepeatedCaptureScenario.AVertexOnlyAdjacencyCaptureRecords (Subprocess aborted) integration-gpu
	2331 - DirectGLES.XfbRepeatedCaptureScenario.ACaptureListBeginningWithGlNextBufferSparesTheEarlierBuffer (Subprocess aborted) integration-gpu
	2332 - DirectGLES.XfbRepeatedCaptureScenario.ASpanThatNeverDrawsLeavesTheCaptureBufferAlone (Subprocess aborted) integration-gpu
	2333 - DirectGLES.TessellationXfbCaptureScenario.CapturesGlPositionByNameFromTheEvaluationStage (Subprocess aborted) integration-gpu
	2334 - DirectGLES.TessellationXfbCaptureScenario.CapturesGlPositionAndGlPointSizeByNameFromTheEvaluationStage (Subprocess aborted) integration-gpu
	2335 - DirectGLES.TessellationXfbCaptureScenario.TheEvaluationStageSeesTheUserPerVertexBlockOfItsPatch (Subprocess aborted) integration-gpu
	2336 - DirectGLES.TessellationXfbCaptureScenario.CapturesGlPointSizeByNameFromTheGeometryStage (Subprocess aborted) integration-gpu
	2337 - DirectGLES.TessellationXfbCaptureScenario.MapsTheCaptureBufferAfterEachOfTwoPatchDraws (Subprocess aborted) integration-gpu
	2338 - DirectGLES.TessellationXfbCaptureScenario.TheEvaluationStageSeesGlPointSizeAcrossItsPatch (Subprocess aborted) integration-gpu
	2339 - DirectGLES.TessellationXfbCaptureScenario.CapturesFromASeparableEvaluationProgramInAPipelineObject (Subprocess aborted) integration-gpu
	2340 - DirectGLES.PointSizeDemotionScenario.TheValueSurvivesTheFiveStageChainIntoTheCapture (Subprocess aborted) integration-gpu
	2341 - DirectGLES.PointSizeDemotionScenario.TheEvaluationStageOwnsTheCaptureWithoutAGeometryStage (Subprocess aborted) integration-gpu
	2342 - DirectGLES.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue (Subprocess aborted) integration-gpu
	2345 - DirectGLES.VertexArrayEnableDisableScenario.EitherHalfOfTheAttributesInTurn (Subprocess aborted) integration-gpu
	2346 - DirectGLES.VertexArrayEnableDisableScenario.TheEvenHalfAlone (Subprocess aborted) integration-gpu
	2347 - DirectGLES.VertexArrayEnableDisableScenario.TheOddHalfAlone (Subprocess aborted) integration-gpu
	2349 - DirectGLES.CopyImageLevelRangeScenario.LevelOneOfATwoLevelTextureIsAccepted (Subprocess aborted) integration-gpu
	2351 - DirectGLES.CopyImageLevelRangeScenario.AValidLevelZeroCopyStillMovesPixels (Subprocess aborted) integration-gpu
	2352 - DirectGLES.CopyImageLayeredScenario.ArrayToArrayCopiesEverySlice (Subprocess aborted) integration-gpu
	2353 - DirectGLES.CopyImageLayeredScenario.ArrayToArrayHonoursDifferentLayerOffsets (Subprocess aborted) integration-gpu
	2354 - DirectGLES.CopyImageLayeredScenario.VolumeToVolumeHonoursNonZeroZ (Subprocess aborted) integration-gpu
	2355 - DirectGLES.CopyImageLayeredScenario.VolumeToVolumeAtNonZeroMipLevel (Subprocess aborted) integration-gpu
	2356 - DirectGLES.CopyImageLayeredScenario.ArrayToVolumeCopiesEverySlice (Subprocess aborted) integration-gpu
	2357 - DirectGLES.CopyImageLayeredScenario.VolumeToArrayCopiesEverySlice (Subprocess aborted) integration-gpu
	2358 - DirectGLES.CopyImagePacked16Scenario.FlatImageLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	2359 - DirectGLES.CopyImagePacked16Scenario.ArrayMipLevelLandsInFlatImageIntact (Subprocess aborted) integration-gpu
	2360 - DirectGLES.CopyImagePacked16Scenario.RenderbufferLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	2370 - DirectGLES.PackedWordReadbackScenario.ACopiedRgb9E5WordSurvivesInAnR11fG11fB10fDestination (Subprocess aborted) integration-gpu
	2371 - DirectGLES.PackedWordReadbackScenario.ACopyThroughARenderbufferReachesAnRgb9E5Destination (Subprocess aborted) integration-gpu
	2376 - DirectGLES.LayeredAttachmentShapeScenario.LayeredThreeDColorAttachmentReachesEverySlice (Failed) integration-gpu
	2377 - DirectGLES.LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice (Failed) integration-gpu
	2378 - DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace (Failed) integration-gpu
	2379 - DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace (Failed) integration-gpu
	2380 - DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace (Failed) integration-gpu
	2382 - DirectGLES.LayeredAttachmentShapeScenario.LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer (Failed) integration-gpu
	2383 - DirectGLES.LayeredAttachmentShapeScenario.CubeMapFaceReadbackAnswersTheFaceItWasAskedFor (Failed) integration-gpu
	2384 - DirectGLES.LayeredTextureReadbackScenario.GetTexImageReturnsEveryLayerOfA1DArray (Subprocess aborted) integration-gpu
	2385 - DirectGLES.LayeredTextureReadbackScenario.GetTexImageReturnsEveryLayerFaceOfACubeMapArray (Subprocess aborted) integration-gpu
	2386 - DirectGLES.AtomicCounterScenario.DispatchIncrementsTheBoundCounterBuffers (Subprocess aborted) integration-gpu
	2387 - DirectGLES.AtomicCounterScenario.CountersAccumulateAcrossDispatchesAndFollowAReseed (Subprocess aborted) integration-gpu
	2388 - DirectGLES.AtomicCounterScenario.SubDataAfterDispatchSurvivesAnImmediateReadback (Subprocess aborted) integration-gpu
	2389 - DirectGLES.LargeArenaAdoptionScenario.SubDataAfterAnInFlightDrawReachesTheNextDraw (Subprocess aborted) integration-gpu
	2390 - DirectGLES.LargeArenaAdoptionScenario.RespecifiedVertexArenaKeepsVaoBindings (Subprocess aborted) integration-gpu
	2391 - DirectGLES.LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding (Subprocess aborted) integration-gpu
	2393 - DirectGLES.LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack (Subprocess aborted) integration-gpu
	2395 - DirectGLES.SsboArrayDynamicIndexScenario.ReadsAndWritesTheBlockTheIndexNames (Subprocess aborted) integration-gpu
	2396 - DirectGLES.StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding (Subprocess aborted) integration-gpu
	2405 - DirectGLES.RelinkStageSetScenario.RelinkingToAddATessEvalStageRunsTheNewExecutable (Subprocess aborted) integration-gpu
	2406 - DirectGLES.GuiBatchScenario.Baseline (Subprocess aborted) integration-gpu
	2407 - DirectGLES.GuiBatchScenario.ShortIndices (Subprocess aborted) integration-gpu
	2408 - DirectGLES.GuiBatchScenario.TwoBuildersWideIndices (Subprocess aborted) integration-gpu
	2409 - DirectGLES.GuiBatchScenario.TwoBuildersBaseVertex (Subprocess aborted) integration-gpu
	2410 - DirectGLES.GuiBatchScenario.TwoBuildersShortIndicesBaseVertex (Subprocess aborted) integration-gpu
	2411 - DirectGLES.GuiBatchScenario.BlendAndDepth (Subprocess aborted) integration-gpu
	2412 - DirectGLES.GuiBatchScenario.ThreeFramesWithoutRelayout (Subprocess aborted) integration-gpu
	2413 - DirectGLES.GuiBatchScenario.MeshesBlockLeftUnbound (Subprocess aborted) integration-gpu
	2414 - DirectGLES.GuiBatchScenario.FullFidelityWithRegrowth (Subprocess aborted) integration-gpu
	2415 - DirectGLES.UnboundCounterBlockScenario.ADeclaredButUnboundCounterDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2416 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundSamplerBufferDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2417 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundSamplerBufferDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2418 - DirectGLES.UnboundImageDescriptorScenario.ABufferTextureWithNoAttachedBufferDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2419 - DirectGLES.UnboundImageDescriptorScenario.AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2420 - DirectGLES.UnboundImageDescriptorScenario.AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2421 - DirectGLES.UnboundImageDescriptorScenario.AWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2422 - DirectGLES.UnboundImageDescriptorScenario.AWriteonlyImage2DLeftUnboundDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2423 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundSampler2DDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2424 - DirectGLES.UnboundImageDescriptorScenario.ASamplerOnAUnitWhoseDefaultTextureIsIncompleteDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2425 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2426 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundUnsignedSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2427 - DirectGLES.UnboundImageDescriptorScenario.ADeclaredButUnboundSignedSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2428 - DirectGLES.UnboundImageDescriptorScenario.AFormatlessWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2429 - DirectGLES.IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour (Subprocess aborted) integration-gpu
	2430 - DirectGLES.IntegerBorderColorScenario.TexParameterIivBorderColourSurvivesToAnIntegerSampler (Subprocess aborted) integration-gpu
	2431 - DirectGLES.IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler (Subprocess aborted) integration-gpu
	2432 - DirectGLES.IntegerBorderColorScenario.ASignedIntegerBorderIsClampedToTheFormatsRepresentableRange (Subprocess aborted) integration-gpu
	2433 - DirectGLES.IntegerBorderColorScenario.ANegativeBorderOnAnUnsignedFormatClampsToTheFormatsMaximum (Subprocess aborted) integration-gpu
	2434 - DirectGLES.IntegerBorderColorScenario.AnOversizedUnsignedBorderIsClampedToTheFormatsMaximum (Subprocess aborted) integration-gpu
	2469 - DirectGLES.TextureParamsWithoutASamplerViewScenario.AnAttachmentOnlyTexturesSwizzleReachesTheDriver (Failed) integration-gpu
	2470 - DirectGLES.TextureParamsWithoutASamplerViewScenario.AReadAttachmentOnlyTexturesDepthStencilModeReachesTheDriver (Subprocess aborted) integration-gpu
	2471 - DirectGLES.TextureParamsWithoutASamplerViewScenario.AnImageUnitOnlyTexturesSwizzleSurvivesARequireImageBindableStorageRemint (Subprocess aborted) integration-gpu
	2472 - DirectGLES.TextureParamsWithoutASamplerViewScenario.ACopyImageEndpointOnlyTexturesParamsReachTheDriver (Subprocess aborted) integration-gpu
	2482 - DirectGLES.P4aSeamAuditScenario.AProgramSwitchWithEqualImageUnitCountersMovesTheImageWindow (Subprocess aborted) integration-gpu
	2487 - DirectGLES.P4aFinalFixScenario.GenerateMipmapAcrossAnUnrelatedDraw (Subprocess aborted) integration-gpu
	2488 - DirectGLES.P4aFinalFixScenario.GenerateMipmapImmediately (Subprocess aborted) integration-gpu
	2491 - DirectGLES.P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact (Failed) integration-gpu
	2494 - DirectGLES.P4aFinalFixScenario.AFramebufferDeletedAfterADsaClearLeavesItsAttachmentIntact (Subprocess aborted) integration-gpu
	2495 - DirectGLES.P4aFinalFixScenario.AnImageBindAfterAllocationReachesTheApplierAsAMetadataRespecify (Subprocess aborted) integration-gpu
	2512 - DirectVulkan.CrossFrameBufferScenario.VertexBufferSubData (Subprocess aborted) integration-gpu
	2513 - DirectVulkan.CrossFrameBufferScenario.VertexMapWriteUnmap (Subprocess aborted) integration-gpu
	2514 - DirectVulkan.CrossFrameBufferScenario.VertexPersistentMapFlush (Subprocess aborted) integration-gpu
	2515 - DirectVulkan.CrossFrameBufferScenario.VertexPersistentCoherentWrite (Subprocess aborted) integration-gpu
	2516 - DirectVulkan.CrossFrameBufferScenario.VertexOrphanAndReupload (Subprocess aborted) integration-gpu
	2517 - DirectVulkan.CrossFrameBufferScenario.VertexCopyBufferSubData (Subprocess aborted) integration-gpu
	2518 - DirectVulkan.CrossFrameBufferScenario.IndexBufferSubData (Subprocess aborted) integration-gpu
	2519 - DirectVulkan.CrossFrameBufferScenario.IndexMapWriteUnmap (Subprocess aborted) integration-gpu
	2520 - DirectVulkan.CrossFrameBufferScenario.IndexPersistentMapFlush (Subprocess aborted) integration-gpu
	2521 - DirectVulkan.CrossFrameBufferScenario.IndexPersistentCoherentWrite (Subprocess aborted) integration-gpu
	2522 - DirectVulkan.CrossFrameBufferScenario.IndexOrphanAndReupload (Subprocess aborted) integration-gpu
	2523 - DirectVulkan.CrossFrameBufferScenario.IndexCopyBufferSubData (Subprocess aborted) integration-gpu
	2524 - DirectVulkan.CrossFrameBufferScenario.PartialStalenessIsCaughtByWholeRegionChecks (Subprocess aborted) integration-gpu
	2525 - DirectVulkan.StreamedArenaScenario.StreamedVertexDataSurvivesArenaRecycling (Subprocess aborted) integration-gpu
	2526 - DirectVulkan.StreamedArenaScenario.StreamedIndexDataSurvivesArenaRecycling (Subprocess aborted) integration-gpu
	2527 - DirectVulkan.ResidentIndexScenario.PersistentCoherentEboWrittenEveryFrame (Subprocess aborted) integration-gpu
	2528 - DirectVulkan.ResidentIndexScenario.EboAlsoBoundAsVertexBufferLater (Subprocess aborted) integration-gpu
	2529 - DirectVulkan.ResidentIndexScenario.EboDeletedAndRecreatedAtTheSameName (Subprocess aborted) integration-gpu
	2530 - DirectVulkan.ResidentIndexScenario.OneEboTwoVaosMutatedAcrossFrames (Subprocess aborted) integration-gpu
	2531 - DirectVulkan.ResidentIndexScenario.PromotedDynamicEbo (Subprocess aborted) integration-gpu
	2532 - DirectVulkan.MultiDrawScenario.BaseVertexBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2533 - DirectVulkan.MultiDrawScenario.PlainBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2534 - DirectVulkan.MultiDrawScenario.UnsignedShortBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2535 - DirectVulkan.MultiDrawScenario.UnsignedByteBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2536 - DirectVulkan.MultiDrawScenario.BaseVertexBeyondIndexTypeRangeMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2537 - DirectVulkan.MultiDrawScenario.ClientSideIndicesBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2538 - DirectVulkan.MultiDrawScenario.PrimitiveRestartStripBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2539 - DirectVulkan.MultiDrawScenario.PrimitiveRestartUnsignedShortBatchMatchesUnrolledDraws (Subprocess aborted) integration-gpu
	2540 - DirectVulkan.MultiDrawScenario.ZeroCountSubDrawsMatchUnrolledDraws (Subprocess aborted) integration-gpu
	2541 - DirectVulkan.MultiDrawScenario.BaseVertexDrawsRejectMalformedArguments (Subprocess aborted) integration-gpu
	2543 - DirectVulkan.DrawParametersScenario.DrawArraysInstancedBaseInstanceReportsItsBaseInstance (Subprocess aborted) integration-gpu
	2544 - DirectVulkan.DrawParametersScenario.APlainDrawAfterABaseInstancedOneSeesZeroAgain (Subprocess aborted) integration-gpu
	2545 - DirectVulkan.DrawParametersScenario.DrawElementsBaseVertexReportsItsBaseVertex (Subprocess aborted) integration-gpu
	2546 - DirectVulkan.DrawParametersScenario.DrawElementsAfterABaseVertexDrawReportsZeroAgain (Subprocess aborted) integration-gpu
	2547 - DirectVulkan.DrawParametersScenario.MultiDrawArraysNumbersItsSubDraws (Subprocess aborted) integration-gpu
	2548 - DirectVulkan.DrawParametersScenario.MultiDrawElementsIndirectCarriesEveryCommandsParameters (Subprocess aborted) integration-gpu
	2549 - DirectVulkan.DrawParametersScenario.MultiDrawArraysIndirectCountObeysItsParameterBuffer (Subprocess aborted) integration-gpu
	2556 - DirectVulkan.XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone (Subprocess aborted) integration-gpu
	2557 - DirectVulkan.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceEnables (Subprocess aborted) integration-gpu
	2558 - DirectVulkan.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadLines8 (Subprocess aborted) integration-gpu
	2559 - DirectVulkan.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadPoints1 (Subprocess aborted) integration-gpu
	2560 - DirectVulkan.XfbAfterClipDistanceScenario.SkipComponentsCaptureSurvivesEveryClipWorkloadStopPoint (Subprocess aborted) integration-gpu
	2561 - DirectVulkan.UnwrittenPositionOutputScenario.ARedeclaredButUnwrittenPositionStillDraws (Subprocess aborted) integration-gpu
	2562 - DirectVulkan.UnwrittenPositionOutputScenario.CapturingAnUnwrittenPositionStillDraws (Subprocess aborted) integration-gpu
	2563 - DirectVulkan.UnwrittenPositionOutputScenario.AWrittenRedeclaredPositionStillDraws (Subprocess aborted) integration-gpu
	2564 - DirectVulkan.UnwrittenPositionOutputScenario.CapturingAWrittenPositionStillDraws (Subprocess aborted) integration-gpu
	2565 - DirectVulkan.UnwrittenPositionOutputScenario.AShaderWithNoPositionBlockStillDraws (Subprocess aborted) integration-gpu
	2571 - DirectVulkan.SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenAFilterChangeCompletesTheTexture (Subprocess aborted) integration-gpu
	2572 - DirectVulkan.SampledSetStalenessScenario.AQueuedClearIsMaterialisedWhenASamplerObjectCompletesTheTexture (Subprocess aborted) integration-gpu
	2587 - DirectVulkan.AdvertisedLimitsScenario.ImageUnitBindingsAreReportedFromTheFrontendState (Subprocess aborted) integration-gpu
	2592 - DirectVulkan.PrimitiveRestartScenario.AnArbitraryRestartIndexDrawsInsteadOfKillingTheProcess (Subprocess aborted) integration-gpu
	2593 - DirectVulkan.PrimitiveRestartScenario.TheFixedIndexValueTakesTheForwardingPath (Subprocess aborted) integration-gpu
	2594 - DirectVulkan.PrimitiveRestartScenario.WithoutTheCapTheStripWeldsAcrossTheGap (Subprocess aborted) integration-gpu
	2595 - DirectVulkan.PrimitiveRestartScenario.ChangingTheRestartIndexBetweenDrawsIsHonoured (Subprocess aborted) integration-gpu
	2597 - DirectVulkan.PrimitiveRestartScenario.ARestartIndexTooLargeForTheIndexTypeRestartsNowhere (Subprocess aborted) integration-gpu
	2598 - DirectVulkan.PrimitiveRestartScenario.AnAllOnesVertexIndexSurvivesTheSubstitution (Subprocess aborted) integration-gpu
	2617 - DirectVulkan.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (Failed) integration-gpu
	2618 - DirectVulkan.DepthStencilReadbackMatrixScenario.CopyTexImageFromADepthAttachmentSurvivesAReadBack (Subprocess aborted) integration-gpu
	2633 - DirectVulkan.ViewportArrayScenario.EachViewportIndexUsesItsOwnDepthRange (Subprocess aborted) integration-gpu
	2638 - DirectVulkan.SsboArrayLengthScenario.WholeBufferBindingsReportTheElementCount (Subprocess aborted) integration-gpu
	2639 - DirectVulkan.SsboArrayLengthScenario.RangeBindingsReportTheBoundWindow (Subprocess aborted) integration-gpu
	2640 - DirectVulkan.SsboArrayLengthScenario.AReadonlyWriteonlyArrayStillReportsItsLength (Subprocess aborted) integration-gpu
	2641 - DirectVulkan.DoublePrecisionScenario.ADoubleUniformReachesTheShaderAtFloatPrecision (Subprocess aborted) integration-gpu
	2642 - DirectVulkan.DoublePrecisionScenario.EveryDoubleShapeLandsInItsOwnSlot (Subprocess aborted) integration-gpu
	2643 - DirectVulkan.DoublePrecisionScenario.TheTransposeFlagStillTransposes (Subprocess aborted) integration-gpu
	2646 - DirectVulkan.DoublePrecisionScenario.ADoubleUniformKeepsItsDeclaredInitializer (Subprocess aborted) integration-gpu
	2647 - DirectVulkan.DoublePrecisionScenario.EveryDoubleUniformShapeArrivesWhereTheShaderReadsIt (Subprocess aborted) integration-gpu
	2648 - DirectVulkan.DoublePrecisionScenario.TheConformanceUniformShaderAgreesWithEveryValueItWasGiven (Subprocess aborted) integration-gpu
	2652 - DirectVulkan.DoublePrecisionScenario.AStorageBlockWithDoublesKeepsTheLayoutItWasBoundWith (Subprocess aborted) integration-gpu
	2653 - DirectVulkan.UniformInitializerScenario.AnUnsetUniformReadsItsDeclaredInitializer (Subprocess aborted) integration-gpu
	2654 - DirectVulkan.UniformInitializerScenario.AnApplicationWriteBeatsTheInitializer (Subprocess aborted) integration-gpu
	2655 - DirectVulkan.UniformInitializerScenario.RelinkingRestoresTheInitializer (Subprocess aborted) integration-gpu
	2656 - DirectVulkan.SwizzleAccessRoutineScenario.EveryAccessRoutineFetchesTheSameTexelUnderTheIdentitySwizzle (Subprocess aborted) integration-gpu
	2657 - DirectVulkan.SwizzleAccessRoutineScenario.EveryAccessRoutineSeesAReversedSwizzle (Subprocess aborted) integration-gpu
	2658 - DirectVulkan.SwizzleAccessRoutineScenario.RepeatedProgramBuildsKeepFetchingTheSameTexel (Subprocess aborted) integration-gpu
	2659 - DirectVulkan.SwizzleAccessRoutineScenario.EveryAccessRoutineSeesConstantSwizzleSources (Subprocess aborted) integration-gpu
	2662 - DirectVulkan.IterationRPProgram203Scenario.FixedCompleteInputProducesFixedCompleteGoldenOutput (Subprocess aborted) integration-gpu
	2663 - DirectVulkan.IterationRPScratchFixScenario.FixtureShapedReductionSumsEveryInvocation (Subprocess aborted) integration-gpu
	2664 - DirectVulkan.IterationRPScratchFixScenario.WideFixtureShapedReductionSumsEveryInvocation (Subprocess aborted) integration-gpu
	2668 - DirectVulkan.ProgramPipelineScenario.ComputeAndGraphicsStagesShareOnePipeline (Subprocess aborted) integration-gpu
	2669 - DirectVulkan.ProgramPipelineScenario.AStageProgramsStorageBlockBindingReachesThePipelineDraw (Subprocess aborted) integration-gpu
	2670 - DirectVulkan.ProgramPipelineScenario.AStorageBlockRebindingHoldsWithoutAPipeline (Subprocess aborted) integration-gpu
	2676 - DirectVulkan.ImageLoadStoreSsoScenario.PerElementImageUnitsReachAPipelineDraw (Subprocess aborted) integration-gpu
	2677 - DirectVulkan.ImageLoadStoreSsoScenario.AnImageArrayAlongsideAnotherDescriptorKeepsBothBindings (Subprocess aborted) integration-gpu
	2678 - DirectVulkan.ImageLoadStoreSsoScenario.ReassigningAnImageUnitBetweenDrawsReachesTheNextDraw (Subprocess aborted) integration-gpu
	2679 - DirectVulkan.ImageTargetKindScenario.LoadsTexture1D (Subprocess aborted) integration-gpu
	2680 - DirectVulkan.ImageTargetKindScenario.LoadsTexture1DArray (Subprocess aborted) integration-gpu
	2681 - DirectVulkan.ImageTargetKindScenario.LoadsTexture2D (Subprocess aborted) integration-gpu
	2682 - DirectVulkan.ImageTargetKindScenario.LoadsTexture2DArray (Subprocess aborted) integration-gpu
	2683 - DirectVulkan.ImageTargetKindScenario.LoadsTexture3D (Subprocess aborted) integration-gpu
	2684 - DirectVulkan.ImageTargetKindScenario.LoadsTextureBuffer (Subprocess aborted) integration-gpu
	2685 - DirectVulkan.ImageTargetKindScenario.LoadsTextureCube (Subprocess aborted) integration-gpu
	2686 - DirectVulkan.ImageTargetKindScenario.LoadsTextureCubeArray (Subprocess aborted) integration-gpu
	2687 - DirectVulkan.ImageTargetKindScenario.LoadsTextureRectangle (Subprocess aborted) integration-gpu
	2688 - DirectVulkan.ImageTargetKindScenario.LoadsTexture2DMultisample (Subprocess aborted) integration-gpu
	2689 - DirectVulkan.ImageTargetKindScenario.LoadsTexture2DMultisampleArray (Subprocess aborted) integration-gpu
	2690 - DirectVulkan.ImageTargetKindScenario.StoresTexture1D (Subprocess aborted) integration-gpu
	2691 - DirectVulkan.ImageTargetKindScenario.StoresTexture1DArray (Subprocess aborted) integration-gpu
	2692 - DirectVulkan.ImageTargetKindScenario.StoresTexture2D (Subprocess aborted) integration-gpu
	2693 - DirectVulkan.ImageTargetKindScenario.StoresTexture2DArray (Subprocess aborted) integration-gpu
	2694 - DirectVulkan.ImageTargetKindScenario.StoresTexture3D (Subprocess aborted) integration-gpu
	2695 - DirectVulkan.ImageTargetKindScenario.StoresTextureBuffer (Subprocess aborted) integration-gpu
	2696 - DirectVulkan.ImageTargetKindScenario.StoresTextureCube (Subprocess aborted) integration-gpu
	2697 - DirectVulkan.ImageTargetKindScenario.StoresTextureCubeArray (Subprocess aborted) integration-gpu
	2698 - DirectVulkan.ImageTargetKindScenario.StoresTextureRectangle (Subprocess aborted) integration-gpu
	2699 - DirectVulkan.ImageTargetKindScenario.StoresTexture2DMultisample (Subprocess aborted) integration-gpu
	2700 - DirectVulkan.ImageTargetKindScenario.StoresTexture2DMultisampleArray (Subprocess aborted) integration-gpu
	2701 - DirectVulkan.ImageTargetKindScenario.AtomicallyAddsToTexture1D (Subprocess aborted) integration-gpu
	2702 - DirectVulkan.ImageTargetKindScenario.AtomicallyAddsToTexture1DArray (Subprocess aborted) integration-gpu
	2703 - DirectVulkan.ImageTargetKindScenario.IgnoresLayerForTexture2D (Subprocess aborted) integration-gpu
	2704 - DirectVulkan.ImageTargetKindScenario.IgnoresLayerForTexture1D (Subprocess aborted) integration-gpu
	2705 - DirectVulkan.ImageTargetKindScenario.IgnoresLayerForTextureRectangle (Subprocess aborted) integration-gpu
	2706 - DirectVulkan.ImageTargetKindScenario.IgnoresLayerForTexture2DMultisample (Subprocess aborted) integration-gpu
	2707 - DirectVulkan.ImageTargetKindScenario.AllKindsInOneProgram (Subprocess aborted) integration-gpu
	2708 - DirectVulkan.ImageFormatQualifierScenario.AFormatlessWriteonlyImageWrites (Subprocess aborted) integration-gpu
	2709 - DirectVulkan.ImageFormatQualifierScenario.RebindToADifferentFormatRebuilds (Subprocess aborted) integration-gpu
	2710 - DirectVulkan.ImageFormatQualifierScenario.FirstBindAfterLinkRebuilds (Subprocess aborted) integration-gpu
	2711 - DirectVulkan.ImageFormatQualifierScenario.ADeclaredFormatStillWins (Subprocess aborted) integration-gpu
	2712 - DirectVulkan.NonCoreImageFormatScenario.TwoChannelFloatImageDropsSurplusStoresAndLoadsZeroOne (Subprocess aborted) integration-gpu
	2713 - DirectVulkan.NonCoreImageFormatScenario.PackedFloatImageDecodesItsUploadAndDropsSurplusStores (Subprocess aborted) integration-gpu
	2714 - DirectVulkan.NonCoreImageFormatScenario.PackedIntegerImageSplitsItsUploadAndKeepsAllFourChannels (Subprocess aborted) integration-gpu
	2715 - DirectVulkan.NonCoreImageFormatScenario.SingleChannelUnsignedImageDropsSurplusStoresAndLoadsZeroOne (Subprocess aborted) integration-gpu
	2716 - DirectVulkan.NonCoreImageFormatScenario.UnsignedNormalizedImageCarriesItsCodesBothWays (Subprocess aborted) integration-gpu
	2717 - DirectVulkan.NonCoreImageFormatScenario.SignedNormalizedImageSignExtendsItsCodesAndClampsAtMinusOne (Subprocess aborted) integration-gpu
	2718 - DirectVulkan.NonCoreImageFormatScenario.TenTenTenTwoImageUsesItsOwnPerChannelDenominators (Subprocess aborted) integration-gpu
	2719 - DirectVulkan.NonCoreImageFormatScenario.NormalizedImageCarriesEveryLayerFaceOfACubeMapArray (Subprocess aborted) integration-gpu
	2720 - DirectVulkan.NonCoreImageFormatScenario.BufferImageAddressesTheApplicationsOwnTexels (Subprocess aborted) integration-gpu
	2721 - DirectVulkan.NonCoreImageFormatScenario.ASplitBufferImageStillSamplesWholeTexels (Subprocess aborted) integration-gpu
	2722 - DirectVulkan.NonCoreImageFormatScenario.ATwoChannelImageTextureStillSamplesAsRGZeroOne (Subprocess aborted) integration-gpu
	2723 - DirectVulkan.ImageSizeAfterRespecScenario.ADrawSeesTheNewSizeOfARespecifiedImageTexture (Subprocess aborted) integration-gpu
	2739 - DirectVulkan.IoBlockNameCollisionScenario.DistinctlyNamedBlocksCarryThePayloadThroughFiveStages (Subprocess aborted) integration-gpu
	2740 - DirectVulkan.IoBlockNameCollisionScenario.OneBlockNameInBothDirectionsStillCarriesThePayload (Subprocess aborted) integration-gpu
	2741 - DirectVulkan.UnlocatedIoBlockScenario.BlocksCarryTheirPayloadThroughFiveStages (Subprocess aborted) integration-gpu
	2742 - DirectVulkan.UnlocatedIoBlockScenario.BlocksNamedInBothDirectionsStillMeetWithoutLocations (Subprocess aborted) integration-gpu
	2744 - DirectVulkan.TessellationDrawModeScenario.TessellationProgramRejectsNonPatchModes (Subprocess aborted) integration-gpu
	2745 - DirectVulkan.TessellationDrawModeScenario.PatchesRejectedWithoutATessellationEvaluationStage (Subprocess aborted) integration-gpu
	2747 - DirectVulkan.GeometryDrawModeScenario.PointsInGeometryProgramRejectsNonPointModesOnFeedbackDraws (Subprocess aborted) integration-gpu
	2753 - DirectVulkan.FormatlessImageBakeScenario.R8uiBakedFromTheBoundUnitStillReachesTheDriver (Subprocess aborted) integration-gpu
	2754 - DirectVulkan.FormatlessImageBakeScenario.R16uiBakedFromTheBoundUnitStillReachesTheDriver (Subprocess aborted) integration-gpu
	2755 - DirectVulkan.FormatlessImageBakeScenario.CoreFormatBakedFromTheBoundUnitIsUnaffected (Subprocess aborted) integration-gpu
	2759 - DirectVulkan.BufferTextureScenario.AnImageStoreIntoABufferTextureIsVisibleToTheCpu (Subprocess aborted) integration-gpu
	2761 - DirectVulkan.VertexAttribBindingScenario.FormatAndBindingFeedTheDraw (Subprocess aborted) integration-gpu
	2762 - DirectVulkan.VertexAttribBindingScenario.FormatBeforeBufferStillResolves (Subprocess aborted) integration-gpu
	2763 - DirectVulkan.VertexAttribBindingScenario.BindingStrideZeroRepeatsOneElement (Subprocess aborted) integration-gpu
	2764 - DirectVulkan.VertexAttribBindingScenario.PointerStrideZeroStaysTightlyPacked (Subprocess aborted) integration-gpu
	2765 - DirectVulkan.VertexAttribBindingScenario.BindingDivisorAppliesToEveryAttributeOnThePoint (Subprocess aborted) integration-gpu
	2766 - DirectVulkan.VertexAttribBindingScenario.BaseInstanceMovesTheInstancedArraysStartElement (Subprocess aborted) integration-gpu
	2767 - DirectVulkan.VertexAttribBindingScenario.BaseInstanceLeavesPerVertexArraysWhereTheyWere (Subprocess aborted) integration-gpu
	2768 - DirectVulkan.VertexAttribBindingScenario.RelativeOffsetComposesWithBindingOffset (Subprocess aborted) integration-gpu
	2769 - DirectVulkan.VertexAttribBindingScenario.InputArrayCaptureProgramFeedsTheDraw (Subprocess aborted) integration-gpu
	2770 - DirectVulkan.VertexAttribBindingScenario.EveryInputArrayElementGetsItsOwnCurrentValue (Subprocess aborted) integration-gpu
	2771 - DirectVulkan.VertexAttribBindingScenario.DoubleArrayIsFetchedAtFloat32Precision (Subprocess aborted) integration-gpu
	2772 - DirectVulkan.VertexAttribBindingScenario.NormalizedIsIgnoredForDoubleArrays (Subprocess aborted) integration-gpu
	2773 - DirectVulkan.VertexAttribBindingScenario.LongDoubleArrayIsFetchedAtFloat32Precision (Subprocess aborted) integration-gpu
	2774 - DirectVulkan.XfbCaptureBufferReuseScenario.EverySpanIntoABufferObjectOfItsOwn (Subprocess aborted) integration-gpu
	2775 - DirectVulkan.XfbCaptureBufferReuseScenario.EverySpanIntoOneRespecifiedBufferObject (Subprocess aborted) integration-gpu
	2776 - DirectVulkan.XfbCaptureBufferReuseScenario.ARespecificationMayChangeTheCaptureBufferSize (Subprocess aborted) integration-gpu
	2777 - DirectVulkan.XfbCaptureBufferReuseScenario.EverySpanIntoOneImmutableStorageBuffer (Subprocess aborted) integration-gpu
	2778 - DirectVulkan.XfbPrimitiveQueryScenario.ACaptureThatFitsReportsEveryPrimitiveOnBothTargets (Subprocess aborted) integration-gpu
	2779 - DirectVulkan.XfbPrimitiveQueryScenario.AnOverflowingVertexOnlyCaptureStopsWritingAtTheBufferCapacity (Subprocess aborted) integration-gpu
	2780 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan (Failed) integration-gpu
	2782 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.CountsATessellatedPatchWithNoCaptureSpan (Subprocess aborted) integration-gpu
	2783 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.ASpanMixingActiveAndInactiveDrawsAccumulatesBoth (Subprocess aborted) integration-gpu
	2784 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsACpuPricedDrawExactlyOnce (Subprocess aborted) integration-gpu
	2785 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsATessellatedPatchExactlyOnce (Subprocess aborted) integration-gpu
	2786 - DirectVulkan.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsAnInstancedDrawExactlyOnce (Subprocess aborted) integration-gpu
	2788 - DirectVulkan.XfbRepeatedCaptureScenario.ASecondGeometryCaptureAfterADeqpStateResetStillRecords (Subprocess aborted) integration-gpu
	2789 - DirectVulkan.XfbRepeatedCaptureScenario.ACaptureFromAPatchesDrawRecords (Subprocess aborted) integration-gpu
	2790 - DirectVulkan.XfbRepeatedCaptureScenario.ACaptureFromAFragmentlessProgramRecords (Subprocess aborted) integration-gpu
	2791 - DirectVulkan.XfbRepeatedCaptureScenario.ACaptureFromAnAdjacencyDrawRecords (Subprocess aborted) integration-gpu
	2792 - DirectVulkan.XfbRepeatedCaptureScenario.AVertexOnlyAdjacencyCaptureRecords (Subprocess aborted) integration-gpu
	2793 - DirectVulkan.XfbRepeatedCaptureScenario.ACaptureListBeginningWithGlNextBufferSparesTheEarlierBuffer (Subprocess aborted) integration-gpu
	2794 - DirectVulkan.XfbRepeatedCaptureScenario.ASpanThatNeverDrawsLeavesTheCaptureBufferAlone (Subprocess aborted) integration-gpu
	2795 - DirectVulkan.TessellationXfbCaptureScenario.CapturesGlPositionByNameFromTheEvaluationStage (Subprocess aborted) integration-gpu
	2796 - DirectVulkan.TessellationXfbCaptureScenario.CapturesGlPositionAndGlPointSizeByNameFromTheEvaluationStage (Subprocess aborted) integration-gpu
	2797 - DirectVulkan.TessellationXfbCaptureScenario.TheEvaluationStageSeesTheUserPerVertexBlockOfItsPatch (Subprocess aborted) integration-gpu
	2798 - DirectVulkan.TessellationXfbCaptureScenario.CapturesGlPointSizeByNameFromTheGeometryStage (Subprocess aborted) integration-gpu
	2799 - DirectVulkan.TessellationXfbCaptureScenario.MapsTheCaptureBufferAfterEachOfTwoPatchDraws (Subprocess aborted) integration-gpu
	2800 - DirectVulkan.TessellationXfbCaptureScenario.TheEvaluationStageSeesGlPointSizeAcrossItsPatch (Subprocess aborted) integration-gpu
	2801 - DirectVulkan.TessellationXfbCaptureScenario.CapturesFromASeparableEvaluationProgramInAPipelineObject (Subprocess aborted) integration-gpu
	2802 - DirectVulkan.PointSizeDemotionScenario.TheValueSurvivesTheFiveStageChainIntoTheCapture (Subprocess aborted) integration-gpu
	2803 - DirectVulkan.PointSizeDemotionScenario.TheEvaluationStageOwnsTheCaptureWithoutAGeometryStage (Subprocess aborted) integration-gpu
	2804 - DirectVulkan.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue (Subprocess aborted) integration-gpu
	2805 - DirectVulkan.PointSizeDemotionScenario.ACaptureSurvivesAStageThatOnlyReadsThePointSize (Subprocess aborted) integration-gpu
	2807 - DirectVulkan.VertexArrayEnableDisableScenario.EitherHalfOfTheAttributesInTurn (Subprocess aborted) integration-gpu
	2808 - DirectVulkan.VertexArrayEnableDisableScenario.TheEvenHalfAlone (Subprocess aborted) integration-gpu
	2809 - DirectVulkan.VertexArrayEnableDisableScenario.TheOddHalfAlone (Subprocess aborted) integration-gpu
	2811 - DirectVulkan.CopyImageLevelRangeScenario.LevelOneOfATwoLevelTextureIsAccepted (Subprocess aborted) integration-gpu
	2813 - DirectVulkan.CopyImageLevelRangeScenario.AValidLevelZeroCopyStillMovesPixels (Subprocess aborted) integration-gpu
	2814 - DirectVulkan.CopyImageLayeredScenario.ArrayToArrayCopiesEverySlice (Subprocess aborted) integration-gpu
	2815 - DirectVulkan.CopyImageLayeredScenario.ArrayToArrayHonoursDifferentLayerOffsets (Subprocess aborted) integration-gpu
	2816 - DirectVulkan.CopyImageLayeredScenario.VolumeToVolumeHonoursNonZeroZ (Subprocess aborted) integration-gpu
	2817 - DirectVulkan.CopyImageLayeredScenario.VolumeToVolumeAtNonZeroMipLevel (Subprocess aborted) integration-gpu
	2818 - DirectVulkan.CopyImageLayeredScenario.ArrayToVolumeCopiesEverySlice (Subprocess aborted) integration-gpu
	2819 - DirectVulkan.CopyImageLayeredScenario.VolumeToArrayCopiesEverySlice (Subprocess aborted) integration-gpu
	2820 - DirectVulkan.CopyImagePacked16Scenario.FlatImageLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	2821 - DirectVulkan.CopyImagePacked16Scenario.ArrayMipLevelLandsInFlatImageIntact (Subprocess aborted) integration-gpu
	2822 - DirectVulkan.CopyImagePacked16Scenario.RenderbufferLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	2832 - DirectVulkan.PackedWordReadbackScenario.ACopiedRgb9E5WordSurvivesInAnR11fG11fB10fDestination (Subprocess aborted) integration-gpu
	2833 - DirectVulkan.PackedWordReadbackScenario.ACopyThroughARenderbufferReachesAnRgb9E5Destination (Subprocess aborted) integration-gpu
	2838 - DirectVulkan.LayeredAttachmentShapeScenario.LayeredThreeDColorAttachmentReachesEverySlice (Failed) integration-gpu
	2839 - DirectVulkan.LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice (Failed) integration-gpu
	2840 - DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace (Failed) integration-gpu
	2841 - DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace (Failed) integration-gpu
	2842 - DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace (Failed) integration-gpu
	2844 - DirectVulkan.LayeredAttachmentShapeScenario.LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer (Failed) integration-gpu
	2845 - DirectVulkan.LayeredAttachmentShapeScenario.CubeMapFaceReadbackAnswersTheFaceItWasAskedFor (Failed) integration-gpu
	2846 - DirectVulkan.LayeredTextureReadbackScenario.GetTexImageReturnsEveryLayerOfA1DArray (Subprocess aborted) integration-gpu
	2847 - DirectVulkan.LayeredTextureReadbackScenario.GetTexImageReturnsEveryLayerFaceOfACubeMapArray (Subprocess aborted) integration-gpu
	2848 - DirectVulkan.AtomicCounterScenario.DispatchIncrementsTheBoundCounterBuffers (Subprocess aborted) integration-gpu
	2849 - DirectVulkan.AtomicCounterScenario.CountersAccumulateAcrossDispatchesAndFollowAReseed (Subprocess aborted) integration-gpu
	2850 - DirectVulkan.AtomicCounterScenario.SubDataAfterDispatchSurvivesAnImmediateReadback (Subprocess aborted) integration-gpu
	2853 - DirectVulkan.LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding (Subprocess aborted) integration-gpu
	2855 - DirectVulkan.LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack (Subprocess aborted) integration-gpu
	2857 - DirectVulkan.SsboArrayDynamicIndexScenario.ReadsAndWritesTheBlockTheIndexNames (Subprocess aborted) integration-gpu
	2858 - DirectVulkan.StorageBufferRegrowScenario.AGrownStoreIsVisibleThroughItsExistingIndexedBinding (Subprocess aborted) integration-gpu
	2867 - DirectVulkan.RelinkStageSetScenario.RelinkingToAddATessEvalStageRunsTheNewExecutable (Subprocess aborted) integration-gpu
	2868 - DirectVulkan.GuiBatchScenario.Baseline (Subprocess aborted) integration-gpu
	2869 - DirectVulkan.GuiBatchScenario.ShortIndices (Subprocess aborted) integration-gpu
	2870 - DirectVulkan.GuiBatchScenario.TwoBuildersWideIndices (Subprocess aborted) integration-gpu
	2871 - DirectVulkan.GuiBatchScenario.TwoBuildersBaseVertex (Subprocess aborted) integration-gpu
	2872 - DirectVulkan.GuiBatchScenario.TwoBuildersShortIndicesBaseVertex (Subprocess aborted) integration-gpu
	2873 - DirectVulkan.GuiBatchScenario.BlendAndDepth (Subprocess aborted) integration-gpu
	2874 - DirectVulkan.GuiBatchScenario.ThreeFramesWithoutRelayout (Subprocess aborted) integration-gpu
	2875 - DirectVulkan.GuiBatchScenario.MeshesBlockLeftUnbound (Subprocess aborted) integration-gpu
	2876 - DirectVulkan.GuiBatchScenario.FullFidelityWithRegrowth (Subprocess aborted) integration-gpu
	2877 - DirectVulkan.UnboundCounterBlockScenario.ADeclaredButUnboundCounterDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2878 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundSamplerBufferDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2879 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundSamplerBufferDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2880 - DirectVulkan.UnboundImageDescriptorScenario.ABufferTextureWithNoAttachedBufferDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2881 - DirectVulkan.UnboundImageDescriptorScenario.AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2882 - DirectVulkan.UnboundImageDescriptorScenario.AWriteonlyImageBufferLeftUnboundDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2883 - DirectVulkan.UnboundImageDescriptorScenario.AWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2884 - DirectVulkan.UnboundImageDescriptorScenario.AWriteonlyImage2DLeftUnboundDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2885 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundSampler2DDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2886 - DirectVulkan.UnboundImageDescriptorScenario.ASamplerOnAUnitWhoseDefaultTextureIsIncompleteDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2887 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2888 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundUnsignedSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2889 - DirectVulkan.UnboundImageDescriptorScenario.ADeclaredButUnboundSignedSampler2DMSDoesNotLoseTheDraw (Subprocess aborted) integration-gpu
	2890 - DirectVulkan.UnboundImageDescriptorScenario.AFormatlessWriteonlyImage2DLeftUnboundDoesNotLoseTheDispatch (Subprocess aborted) integration-gpu
	2891 - DirectVulkan.IntegerBorderColorScenario.InsideTexelsAreUnaffectedByTheBorderColour (Subprocess aborted) integration-gpu
	2892 - DirectVulkan.IntegerBorderColorScenario.TexParameterIivBorderColourSurvivesToAnIntegerSampler (Subprocess aborted) integration-gpu
	2893 - DirectVulkan.IntegerBorderColorScenario.SamplerParameterIivBorderColourSurvivesToAnIntegerSampler (Subprocess aborted) integration-gpu
	2894 - DirectVulkan.IntegerBorderColorScenario.ASignedIntegerBorderIsClampedToTheFormatsRepresentableRange (Subprocess aborted) integration-gpu
	2895 - DirectVulkan.IntegerBorderColorScenario.ANegativeBorderOnAnUnsignedFormatClampsToTheFormatsMaximum (Subprocess aborted) integration-gpu
	2896 - DirectVulkan.IntegerBorderColorScenario.AnOversizedUnsignedBorderIsClampedToTheFormatsMaximum (Subprocess aborted) integration-gpu
	2944 - DirectVulkan.P4aSeamAuditScenario.AProgramSwitchWithEqualImageUnitCountersMovesTheImageWindow (Subprocess aborted) integration-gpu
	2956 - DirectVulkan.P4aFinalFixScenario.AFramebufferDeletedAfterADsaClearLeavesItsAttachmentIntact (Subprocess aborted) integration-gpu
	2963 - DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureAlone (Subprocess aborted) integration-gpu
	2964 - DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceEnables (Subprocess aborted) integration-gpu
	2965 - DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadLines8 (Subprocess aborted) integration-gpu
	2966 - DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureAfterClipDistanceWorkloadPoints1 (Subprocess aborted) integration-gpu
	2967 - DirectVulkan.AsyncCompile.XfbAfterClipDistanceScenario.SkipComponentsCaptureSurvivesEveryClipWorkloadStopPoint (Subprocess aborted) integration-gpu
	2978 - DirectGLES.ForcedDepthStencilEmulation.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (Failed) integration-gpu
	2979 - DirectGLES.ForcedDepthStencilEmulation.DepthStencilReadbackMatrixScenario.CopyTexImageFromADepthAttachmentSurvivesAReadBack (Subprocess aborted) integration-gpu
	2988 - DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.BlocksCarryTheirPayloadThroughFiveStages (Subprocess aborted) integration-gpu
	2989 - DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.BlocksNamedInBothDirectionsStillMeetWithoutLocations (Subprocess aborted) integration-gpu
	2990 - DirectGLES.UnlocatedIoBlocks.UnlocatedIoBlockScenario.TheEmulationIsActuallyArmedWhenTheEnvironmentPinsItOn (Subprocess aborted) integration-gpu
	3010 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan (Failed) integration-gpu
	3012 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.CountsATessellatedPatchWithNoCaptureSpan (Subprocess aborted) integration-gpu
	3013 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.ASpanMixingActiveAndInactiveDrawsAccumulatesBoth (Subprocess aborted) integration-gpu
	3014 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsACpuPricedDrawExactlyOnce (Subprocess aborted) integration-gpu
	3015 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsATessellatedPatchExactlyOnce (Subprocess aborted) integration-gpu
	3016 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.APausedSpanCountsAnInstancedDrawExactlyOnce (Subprocess aborted) integration-gpu
	3017 - DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn (Failed) integration-gpu
	3018 - DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.FlatImageLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	3019 - DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.ArrayMipLevelLandsInFlatImageIntact (Subprocess aborted) integration-gpu
	3020 - DirectGLES.WidenedPacked16.CopyImagePacked16Scenario.RenderbufferLandsInArrayMipLevelIntact (Subprocess aborted) integration-gpu
	3021 - DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheValueSurvivesTheFiveStageChainIntoTheCapture (Subprocess aborted) integration-gpu
	3022 - DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheEvaluationStageOwnsTheCaptureWithoutAGeometryStage (Subprocess aborted) integration-gpu
	3023 - DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue (Subprocess aborted) integration-gpu
	3024 - DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.ACaptureSurvivesAStageThatOnlyReadsThePointSize (Subprocess aborted) integration-gpu
	3025 - DirectGLES.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn (Subprocess aborted) integration-gpu
	3026 - DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheValueSurvivesTheFiveStageChainIntoTheCapture (Subprocess aborted) integration-gpu
	3027 - DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheEvaluationStageOwnsTheCaptureWithoutAGeometryStage (Subprocess aborted) integration-gpu
	3028 - DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.AGeometryOnlyChainCarriesTheVertexValue (Subprocess aborted) integration-gpu
	3029 - DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.ACaptureSurvivesAStageThatOnlyReadsThePointSize (Subprocess aborted) integration-gpu
	3030 - DirectVulkan.PointSizeDemotion.PointSizeDemotionScenario.TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn (Subprocess aborted) integration-gpu
	3036 - DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin (Failed) integration-gpu
	3072 - DirectGLES.HandleRecycle.Legacy.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin (Failed) integration-gpu
	3090 - DirectVulkan.HandleRecycle.Legacy.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin (Failed) integration-gpu
	3108 - DirectVulkan.HandleRecycle.AbaControl.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin (Failed) integration-gpu
	3126 - DirectVulkan.HandleRecycle.AbaControlHandles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin (Failed) integration-gpu
	3143 - DirectGLES.ResourceSubsystemControl.On.ResourceSubsystemControlScenario.ClearingTheP3aBitsStopsTheEmissionsAndNotThePixels (Subprocess aborted) integration-gpu
	3145 - DirectGLES.MapPersistentRoundtrips.StorageBufferRegrowScenario.NStorageDefinitionsCostNMapPersistentRoundtripsNotOnePerDraw (Subprocess aborted) integration-gpu
	3146 - DirectGLES.MapPersistentRoundtrips.LargeArenaAdoptionScenario.AnAdoptionCostsExactlyOneMapPersistentRoundtrip (Subprocess aborted) integration-gpu
	3147 - DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.SubDataAfterAnInFlightDrawReachesTheNextDraw (Subprocess aborted) integration-gpu
	3148 - DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.RespecifiedVertexArenaKeepsVaoBindings (Subprocess aborted) integration-gpu
	3149 - DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding (Subprocess aborted) integration-gpu
	3151 - DirectGLES.ResourceSubsystemOn.LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack (Subprocess aborted) integration-gpu
	3155 - DirectGLES.ResourceSubsystemOff.LargeArenaAdoptionScenario.RespecifiedIndexArenaKeepsVaoBinding (Subprocess aborted) integration-gpu
	3157 - DirectGLES.ResourceSubsystemOff.LargeArenaAdoptionScenario.GpuWriteIntoTheArenaIsReadBack (Subprocess aborted) integration-gpu
```

## Appendix B — E1 evidence in every selected private file

**DirectGLES.Split.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.ClearThenReadPixelsScenario.ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.ClearThenReadPixelsScenario.AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToASubRectReadback**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToASubRectReadback.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.AMultisampleResolveBlitIntoTheDefaultFramebufferKeepsItsOrientation.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToASubRectReadback**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToASubRectReadback.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.ClearThenReadPixelsScenario.ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.TriangleScenario.AVboBackedTriangleReachesReadPixels**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.TriangleScenario.AVboBackedTriangleReachesReadPixels.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.SmallRing.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.SmallRing.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.TriangleScenario.AVboBackedTriangleReachesReadPixels.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```
**DirectGLES.Split.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary**

File: /home/swung/w7/p5-joint-evidence/controls-private/DirectGLES.Split.TriangleScenario.TheSameVboAndVaoRedrawAcrossAFrameBoundary.log

```text
[09:43:07] [Linux MobileGLIntegra/FATAL]: MGPipe: Fatal{BarrierViolation, "SetTextureParams"} - the apply thread is inside the applier while the GL thread is emitting. R-1 makes at most one of them runnable, which is what keeps one process-wide gPipeInputs legal
```

## Appendix C — every ordinary failure, grouped by family

Each quote is from the GoogleTest output of a run with the named private MOBILEGL_LOG_FILE_PATH. No Fatal appeared in these 27 private logs. Exact discovered command/environment metadata is in manifest.json.

### DepthStencilReadbackMatrixScenario

**DirectGLES.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-1.log

Output: /home/swung/w7/p5-joint-evidence/probe-1.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackMatrixScenario.cpp:530: Failure
Expected equality of these values:
  bad
    Which is: 3072
  0u
    Which is: 0
3072 of 3072 words from glGetTexImage(GL_DEPTH_STENCIL) are wrong (first word 0x0)

[  FAILED  ] DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (0 ms)
```

**DirectVulkan.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-11.log

Output: /home/swung/w7/p5-joint-evidence/probe-11.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackMatrixScenario.cpp:530: Failure
Expected equality of these values:
  bad
    Which is: 3072
  0u
    Which is: 0
3072 of 3072 words from glGetTexImage(GL_DEPTH_STENCIL) are wrong (first word 0x0)

[  FAILED  ] DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (0 ms)
```

**DirectGLES.ForcedDepthStencilEmulation.DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-23.log

Output: /home/swung/w7/p5-joint-evidence/probe-23.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackMatrixScenario.cpp:530: Failure
Expected equality of these values:
  bad
    Which is: 3072
  0u
    Which is: 0
3072 of 3072 words from glGetTexImage(GL_DEPTH_STENCIL) are wrong (first word 0x0)

[  FAILED  ] DepthStencilReadbackMatrixScenario.GetTexImageReadsAPackedDepthStencilTexture (0 ms)
```

### HandleRecycleScenario

**DirectGLES.HandleRecycle.Handles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-32.log

Output: /home/swung/w7/p5-joint-evidence/probe-32.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1369: Failure
Expected equality of these values:
  replacementIsNotGreen
    Which is: 16
  0
the replacement framebuffer's own attachment is not the colour it was cleared to, so the clear reached a framebuffer this one only shares an address with (first texel rgba=0,0,0,0)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1377: Failure
Expected equality of these values:
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-33.log); rc=1 (/home/swung/w7/p5-joint-probe-34.log); rc=1 (/home/swung/w7/p5-joint-probe-35.log).

**DirectGLES.HandleRecycle.Legacy.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-36.log

Output: /home/swung/w7/p5-joint-evidence/probe-36.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1369: Failure
Expected equality of these values:
  replacementIsNotGreen
    Which is: 16
  0
the replacement framebuffer's own attachment is not the colour it was cleared to, so the clear reached a framebuffer this one only shares an address with (first texel rgba=0,0,0,0)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1377: Failure
Expected equality of these values:
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-37.log); rc=1 (/home/swung/w7/p5-joint-probe-38.log); rc=1 (/home/swung/w7/p5-joint-probe-39.log).

**DirectVulkan.HandleRecycle.Legacy.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-40.log

Output: /home/swung/w7/p5-joint-evidence/probe-40.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1369: Failure
Expected equality of these values:
  replacementIsNotGreen
    Which is: 16
  0
the replacement framebuffer's own attachment is not the colour it was cleared to, so the clear reached a framebuffer this one only shares an address with (first texel rgba=0,0,0,0)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1377: Failure
Expected equality of these values:
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-41.log); rc=1 (/home/swung/w7/p5-joint-probe-42.log); rc=1 (/home/swung/w7/p5-joint-probe-43.log).

**DirectVulkan.HandleRecycle.AbaControl.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-44.log

Output: /home/swung/w7/p5-joint-evidence/probe-44.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1369: Failure
Expected equality of these values:
  replacementIsNotGreen
    Which is: 16
  0
the replacement framebuffer's own attachment is not the colour it was cleared to, so the clear reached a framebuffer this one only shares an address with (first texel rgba=0,0,0,0)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1377: Failure
Expected equality of these values:
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-45.log); rc=1 (/home/swung/w7/p5-joint-probe-46.log); rc=1 (/home/swung/w7/p5-joint-probe-47.log).

**DirectVulkan.HandleRecycle.AbaControlHandles.HandleRecycleScenario.AFramebufferAtARecycledAddressDoesNotInheritItsPredecessorsTwin** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-48.log

Output: /home/swung/w7/p5-joint-evidence/probe-48.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1369: Failure
Expected equality of these values:
  replacementIsNotGreen
    Which is: 16
  0
the replacement framebuffer's own attachment is not the colour it was cleared to, so the clear reached a framebuffer this one only shares an address with (first texel rgba=0,0,0,0)

../MobileGL/MG_IntegrationTest/Scenarios/HandleRecycleScenario.cpp:1377: Failure
Expected equality of these values:
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-49.log); rc=1 (/home/swung/w7/p5-joint-probe-50.log); rc=1 (/home/swung/w7/p5-joint-probe-51.log).

### LayeredAttachmentShapeScenario

**DirectGLES.LayeredAttachmentShapeScenario.LayeredThreeDColorAttachmentReachesEverySlice** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-2.log

Output: /home/swung/w7/p5-joint-evidence/probe-2.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_3D colour attachment: layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_3D colour attachment: layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectGLES.LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-3.log

Output: /home/swung/w7/p5-joint-evidence/probe-3.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:660: Failure
Failed
slice 2 texel (0, 0) is (171, 171, 171, 171), expected (50, 180, 3, 255) - the attached slice was not the one written

[  FAILED  ] LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice (39 ms)
[----------] 1 test from LayeredAttachmentShapeScenario (39 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (260 ms total)
```

**DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-4.log

Output: /home/swung/w7/p5-joint-evidence/probe-4.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_CUBE_MAP_ARRAY colour attachment: layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_CUBE_MAP_ARRAY colour attachment: layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-5.log

Output: /home/swung/w7/p5-joint-evidence/probe-5.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered cube-map-array depth attachment (pass 1's colour on a layer means the depth test did not cover it): layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered cube-map-array depth attachment (pass 1's colour on a layer means the depth test did not cover it): layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectGLES.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-6.log

Output: /home/swung/w7/p5-joint-evidence/probe-6.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
a layered draw with the depth and stencil tests DISABLED, into a colour + GL_DEPTH_STENCIL_ATTACHMENT cube-map-array pair (all poison means the attachment pair lost the draw outright, which is what a non-layered depth/stencil attachment beside a layered colour one looks like): layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 183, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
a layered draw with the depth and stencil tests DISABLED, into a colour + GL_DEPTH_STENCIL_ATTACHMENT cube-map-array pair (all poison means the attachment pair lost the draw outright, which is what a non-layered depth/stencil attachment beside a layered colour one looks like): layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 183, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectGLES.LayeredAttachmentShapeScenario.LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-7.log

Output: /home/swung/w7/p5-joint-evidence/probe-7.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
Failed
layered GL_TEXTURE_1D_ARRAY glClear materialised by sampling (unit = layer): unit 0 texel 0 is (171, 171, 171, 171), expected (17, 68, 187, 255) - the poison, so the clear never reached this one

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
Failed
layered GL_TEXTURE_1D_ARRAY glClear materialised by sampling (unit = layer): unit 1 texel 0 is (171, 171, 171, 171), expected (17, 68, 187, 255) - the poison, so the clear never reached this one

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
```

**DirectGLES.LayeredAttachmentShapeScenario.CubeMapFaceReadbackAnswersTheFaceItWasAskedFor** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-8.log

Output: /home/swung/w7/p5-joint-evidence/probe-8.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
Failed
glGetTexImage(GL_TEXTURE_CUBE_MAP_<face>): face +X texel 0 is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever written to this face

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
Failed
glGetTexImage(GL_TEXTURE_CUBE_MAP_<face>): face -X texel 0 is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever written to this face

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.LayeredThreeDColorAttachmentReachesEverySlice** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-16.log

Output: /home/swung/w7/p5-joint-evidence/probe-16.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_3D colour attachment: layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_3D colour attachment: layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-17.log

Output: /home/swung/w7/p5-joint-evidence/probe-17.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:660: Failure
Failed
slice 2 texel (0, 0) is (171, 171, 171, 171), expected (50, 180, 3, 255) - the attached slice was not the one written

[  FAILED  ] LayeredAttachmentShapeScenario.NonLayeredThreeDSliceAttachmentWritesOnlyThatSlice (37 ms)
[----------] 1 test from LayeredAttachmentShapeScenario (37 ms total)

[----------] Global test environment tear-down
[==========] 1 test from 1 test suite ran. (243 ms total)
```

**DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayColorAttachmentReachesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-18.log

Output: /home/swung/w7/p5-joint-evidence/probe-18.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_CUBE_MAP_ARRAY colour attachment: layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered GL_TEXTURE_CUBE_MAP_ARRAY colour attachment: layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthAttachmentGatesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-19.log

Output: /home/swung/w7/p5-joint-evidence/probe-19.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered cube-map-array depth attachment (pass 1's colour on a layer means the depth test did not cover it): layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
layered cube-map-array depth attachment (pass 1's colour on a layer means the depth test did not cover it): layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.LayeredCubeMapArrayDepthStencilAttachmentGatesEveryLayerFace** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-20.log

Output: /home/swung/w7/p5-joint-evidence/probe-20.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
a layered draw with the depth and stencil tests DISABLED, into a colour + GL_DEPTH_STENCIL_ATTACHMENT cube-map-array pair (all poison means the attachment pair lost the draw outright, which is what a non-layered depth/stencil attachment beside a layered colour one looks like): layer 0 texel (0, 0) is (171, 171, 171, 171), expected (10, 200, 183, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
Failed
a layered draw with the depth and stencil tests DISABLED, into a colour + GL_DEPTH_STENCIL_ATTACHMENT cube-map-array pair (all poison means the attachment pair lost the draw outright, which is what a non-layered depth/stencil attachment beside a layered colour one looks like): layer 1 texel (0, 0) is (171, 171, 171, 171), expected (30, 190, 183, 255) - the poison, so nothing was ever rendered into this layer

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:507: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.LayeredOneDArrayClearMaterialisedBySamplingReachesEveryLayer** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-21.log

Output: /home/swung/w7/p5-joint-evidence/probe-21.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
Failed
layered GL_TEXTURE_1D_ARRAY glClear materialised by sampling (unit = layer): unit 0 texel 0 is (171, 171, 171, 171), expected (17, 68, 187, 255) - the poison, so the clear never reached this one

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
Failed
layered GL_TEXTURE_1D_ARRAY glClear materialised by sampling (unit = layer): unit 1 texel 0 is (171, 171, 171, 171), expected (17, 68, 187, 255) - the poison, so the clear never reached this one

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:458: Failure
```

**DirectVulkan.LayeredAttachmentShapeScenario.CubeMapFaceReadbackAnswersTheFaceItWasAskedFor** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-22.log

Output: /home/swung/w7/p5-joint-evidence/probe-22.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
Failed
glGetTexImage(GL_TEXTURE_CUBE_MAP_<face>): face +X texel 0 is (171, 171, 171, 171), expected (10, 200, 3, 255) - the poison, so nothing was ever written to this face

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
Failed
glGetTexImage(GL_TEXTURE_CUBE_MAP_<face>): face -X texel 0 is (171, 171, 171, 171), expected (30, 190, 3, 255) - the poison, so nothing was ever written to this face

../MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentShapeScenario.cpp:542: Failure
```

### P4aFinalFixScenario

**DirectGLES.P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-10.log

Output: /home/swung/w7/p5-joint-evidence/probe-10.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/P4aFinalFixScenario.cpp:420: Failure
Value of: Mostly(image, "white", "the draw after a renderbuffer and its framebuffer died")
  Actual: false (the draw after a renderbuffer and its framebuffer died: region x[2,126] y[2,94] should be all white, but 11589 of 11625 pixels (100%) are not; first offender at (8,2) is black rgba(0,0,0,0))
Expected: true

[  FAILED  ] P4aFinalFixScenario.ARenderbufferAndItsFramebufferDeletedAfterAClearLeaveTheNextDrawIntact (12 ms)
[----------] 1 test from P4aFinalFixScenario (12 ms total)

[----------] Global test environment tear-down
```

### PrimitivesGeneratedNoXfbScenario

**DirectVulkan.PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-12.log

Output: /home/swung/w7/p5-joint-evidence/probe-12.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/PrimitivesGeneratedNoXfbScenario.cpp:295: Failure
Expected equality of these values:
  generated
    Which is: 0
  2u
    Which is: 2
GL_PRIMITIVES_GENERATED must count a draw made while transform feedback is inactive (GL 4.6 core 13.4)

[  FAILED  ] PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan (26 ms)
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-13.log); rc=1 (/home/swung/w7/p5-joint-probe-14.log); rc=1 (/home/swung/w7/p5-joint-probe-15.log).

**DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-24.log

Output: /home/swung/w7/p5-joint-evidence/probe-24.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/PrimitivesGeneratedNoXfbScenario.cpp:295: Failure
Expected equality of these values:
  generated
    Which is: 0
  2u
    Which is: 2
GL_PRIMITIVES_GENERATED must count a draw made while transform feedback is inactive (GL 4.6 core 13.4)

[  FAILED  ] PrimitivesGeneratedNoXfbScenario.CountsADrawMadeWithNoCaptureSpan (24 ms)
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-25.log); rc=1 (/home/swung/w7/p5-joint-probe-26.log); rc=1 (/home/swung/w7/p5-joint-probe-27.log).

**DirectVulkan.PrimGenReroute.PrimitivesGeneratedNoXfbScenario.TheRerouteIsActuallyArmedWhenTheEnvironmentPinsItOn** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-28.log

Output: /home/swung/w7/p5-joint-evidence/probe-28.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/PrimitivesGeneratedNoXfbScenario.cpp:541: Failure
Expected equality of these values:
  generated
    Which is: 0
  1u
    Which is: 1
the pinned-on lane did not even count correctly

../MobileGL/MG_IntegrationTest/Scenarios/PrimitivesGeneratedNoXfbScenario.cpp:544: Failure
```

Three additional runs: rc=1 (/home/swung/w7/p5-joint-probe-29.log); rc=1 (/home/swung/w7/p5-joint-probe-30.log); rc=1 (/home/swung/w7/p5-joint-probe-31.log).

### TextureParamsWithoutASamplerViewScenario

**DirectGLES.TextureParamsWithoutASamplerViewScenario.AnAttachmentOnlyTexturesSwizzleReachesTheDriver** — rc=1.

Private log: /home/swung/w7/p5-joint-probe-9.log

Output: /home/swung/w7/p5-joint-evidence/probe-9.out

```text
../MobileGL/MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp:415: Failure
Expected equality of these values:
  applied.Swizzle[channel]
    Which is: 0
  static_cast<int>(expectedSwizzle[channel])
    Which is: 1
ESPRYT HAS NOT APPLIED THE SWIZZLE YET. Channel 1 of the driver texture (ES name 1) reads 0x0, the application set 0x1, and the applier's record already carries the right value - so the record reached the server and the server has not pushed it. The texture was an attachment of the DRAW framebuffer and nothing else and has NO sampler view (asserted above), which makes this exactly the deferred-to-first-view shape G9 exists to catch: the sample at the end of this case would repair it, and the end-to-end assertion below would then pass on a driver that was told late. That is the half no public-GL case can see.

../MobileGL/MG_IntegrationTest/Scenarios/TextureParamsWithoutASamplerViewScenario.cpp:415: Failure
```

## Appendix D — unmodified scenario telemetry

Probe 52

```text
[09:45:14] [Linux MobileGLIntegra/INFO]: MG_Remote memory[accept/server]: peakRss=11526144 currentRss=11526144 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[09:45:14] [Linux MobileGLIntegra/INFO]: MG_Remote memory[handshake/client]: peakRss=11706368 currentRss=11706368 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
[09:45:15] [Linux mgl-srv-apply/INFO]: MGPipe stats: frames=1 window=1 draws=1 draws/f=1.00 acc=21 acc/draw=21.00 bytes/f[buf=60.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=8.00 csob-blob=100.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=1 csob=1 mpr=0] emit[fbe=1 sve=1 sse=1 sie=0 ctu=0 trp=0 rsp=35] gates[ers=1/1 etl=2/1 eub=2/4 mfp=0/0 mpm=0/0 mdt=0/0]
[09:45:15] [Linux mgl-srv-apply/INFO]: MGPipe stats: frames=2 window=1 draws=1 draws/f=1.00 acc=21 acc/draw=21.00 bytes/f[buf=0.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=0.00 resid=8.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=0 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=35] gates[ers=1/1 etl=3/0 eub=3/3 mfp=0/0 mpm=0/0 mdt=0/0]
[09:45:15] [Linux MobileGLIntegra/INFO]: MG_Remote memory[teardown/client]: peakRss=141451264 currentRss=138625024 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```

Probe 53

```text
[09:45:15] [Linux MobileGLIntegra/INFO]: MG_Remote memory[accept/server]: peakRss=11464704 currentRss=11464704 roleMapped=58990592 allRolesMapped=58990592 (peakRss is the PROCESS's; under inproc both roles share it)
[09:45:15] [Linux MobileGLIntegra/INFO]: MG_Remote memory[handshake/client]: peakRss=11649024 currentRss=11649024 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
[09:45:15] [Linux mgl-srv-apply/INFO]: MGPipe stats: frames=1 window=1 draws=1 draws/f=1.00 acc=21 acc/draw=21.00 bytes/f[buf=120.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=600.00 resid=8.00 csob-blob=100.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=1 csob=1 mpr=1] emit[fbe=1 sve=1 sse=1 sie=0 ctu=0 trp=0 rsp=35] gates[ers=1/1 etl=2/1 eub=2/4 mfp=0/0 mpm=0/0 mdt=0/0]
[09:45:15] [Linux mgl-srv-apply/INFO]: MGPipe stats: frames=2 window=1 draws=1 draws/f=1.00 acc=19 acc/draw=19.00 bytes/f[buf=120.00 tex=0.00 ubog=0.00 ubon=0.00 vtxc=0.00 idxc=0.00 icmd=0.00 pmap=600.00 resid=0.00 csob-blob=0.00] tex[emit=0 box=0 rect=0 jobs=0] cso[csom=0 csob=0 mpr=0] emit[fbe=0 sve=0 sse=0 sie=0 ctu=0 trp=0 rsp=35] gates[ers=2/0 etl=3/0 eub=3/3 mfp=0/0 mpm=0/0 mdt=0/0]
[09:45:15] [Linux MobileGLIntegra/INFO]: MG_Remote memory[teardown/client]: peakRss=141422592 currentRss=138641408 roleMapped=58990592 allRolesMapped=117981184 (peakRss is the PROCESS's; under inproc both roles share it)
```
