# CI cleanup log

Every removal or replacement of a CI test, gate or lane is logged here with a one-line reason;
decisions to keep a red case (and fix the code or the tooling instead) are logged too, so the
next reader can see why a check still exists. Started in P13 (2026-10-06). The rule: a check is
removed only when it is superseded or no longer relevant (tied to the pull build, or a finished
migration-phase audit), never to hide a real bug. Goldens change only when the GL spec proves
them wrong.

## Removals

| Date | Check | Workflow / job | Reason | Successor |
|---|---|---|---|---|
| 2026-10-06 | `build explicit pull runtime control` (the pull `libMobileGL.so`, `-DMOBILEGL_PIPE_PUSH=OFF`) | Test / `build-linux-monolith-control` | P13 W2 retires the pull build: CMake defaults to push and nothing ships pull (ID-P13-1) | the same job builds the push monolith without MG_Remote (the FCL shape); its `runtime_mode_proof --mode monolith` is the zero-MG_Remote-symbol check |
| 2026-10-06 | `Negative control (pull library)` | Test / `retrace-split` (every leg) | its library was the pull build above | `Negative control (library without a transport)`: same script (renamed `retrace_transport_control.sh`) and evidence sentence, with the push library without MG_Remote; W1 ran both side by side on every leg |
| 2026-10-06 | `DirectGLES.HandleRecycle.Legacy.*`, `DirectVulkan.HandleRecycle.Legacy.*`, `DirectVulkan.HandleRecycle.AbaControl.*` | Test / integration-gpu (`integration-monolith-control`) | they ran `MOBILEGL_PIPE_PUSH=0`, the pre-handle arm, which P13 W3b removed with `MOBILEGL_PIPE_LEGACY_MEMOS` (ID-P13-3) | `HandleRecycle.Handles.*` and `DirectVulkan.HandleRecycle.AbaControlHandles.*` (the ABA control against the handle arm that ships) stay |
| 2026-10-06 | `DirectGLES.ResourceSubsystemControl.Off.*`, `DirectGLES.ResourceSubsystemOff.*`, `DirectGLES.ObjectSubsystemControl.{Off,Refused,RefusedTexture}.*` | Test / integration-gpu | they cleared subsystem bits (masks 0x7f / 0x1ff / 0x9ff / 0x5ff) to run legacy arms; since W3b bits 0-13 are fixed on (ID-P13-3) | one named-refusal control per lane, `DirectGLES.SubsystemMaskRefusal.<Lane>`: the same scenario and mask must go red with `Fatal{PipeSubsystemsFixedOn}` naming that mask (`Harness/RefusedMaskControl.cmake`); the `On` lanes now run 0x3fff |
| 2026-10-06 | `SanityTest`: `DirectGLESSlotTable.{AnArmlessKnobCombinationStopsInsteadOfSkippingTheLane, TheArmlessCasesLeaveTheLogPathAndTheConfigAsTheyFoundThem, EglBringUpUnderTheArmlessKnobPairReturnsInsteadOfStopping}` | Test / unit | they pinned the `MOBILEGL_PIPE_LEGACY_MEMOS=0` + cleared-bit knob pair; the knob is gone and the mask is refused at startup | the SubsystemMaskRefusal controls above |
| 2026-10-06 | `SanityTest`: `DirectGLESTextureSync.AnAttachmentOnlyTexturesParametersReachTheDriverWithNoSamplerView` | Test / unit | **not a behaviour gap.** At 0x3fff the parameter push is record-addressed (`SyncTextureParamsToBackend` reads the texture's applier record; nothing in it consults a sampler view), but the unit lane cannot create that record (`NoP4aConsumer`, as above), so the probe's handle half never had a record to read; its legacy half pinned the pre-handle push that W3b removed | `TextureParamsWithoutASamplerViewScenario` (G9): its four cases take a WHITE-BOX reading of the driver-side parameters before any sample (`TakeTheWhiteBoxReadingBeforeAnySample`, ID-19), at the shipping mask on every lane - the same property, observed where records exist |
| 2026-10-06 | unit `RenderStateSpans.{ResidualTripWireIsSilentUntilTheApplierOwnsTheBytes, ResidualTripWireHoldsAcrossAVerbClassThatDoesNotPublishTheBlock, ResidualTripWireFiresNamingTheCapability}` and PeerLatchTest's `SetResidualValueStateBlobSize` row | Test / unit | P13 W3c deleted ResidualValueBlock and its applier trip wire (op 46 is a retired, declined row) | integration-verify's new `Negative control A2 (corrupted capability)`: the verify comparator must go red on a corrupted `IsCapabilityEnabled`; `PipeWireCodecTest.ResidualValueBlockCrossesAsItsOwnBlob` now pins that op 46 is declined; `TrackerTest...AClipDistanceEnableReArmsTheResidualBlock` now asserts the capability reaches the applier's block |

## Kept after investigation

### `minecraft-1.17-main-menu-854` (Test retrace / retrace-split / retrace-verify, APK retrace), 2026-10-06

- **Symptom**: every lane, both backends, SSIM 0.0186 against the golden; the frame is Minecraft's
  red loading-screen colour. Local repro on WSL llvmpipe: same SSIM, monolith and inproc.
- **Root cause**: the trace was captured on an implementation that gave shaders and programs
  **overlapping names** (shader 1 and program 1 live at once, likewise shader 5 / program 5,
  calls 159-209). GL 4.6 core and ES 3.2 §7.1 put shader and program objects in one shared name
  space, so a conformant implementation never hands out such a pair. Since
  `5a0f13fdf` / `168808d1a` (2026-10-01/02) MobileGL advertises `GL_ARB_shader_objects`, and
  apitrace's glretrace then resolves every shader and program name through one ARB handle map
  (`_handleARB_map`): program 1 overwrote shader 1's entry, `glAttachShader(1, 1)` attached a
  program name (GL_INVALID_OPERATION at call 170), the blit program linked without a vertex
  shader, and nothing after it drew.
- **Classification**: (b) retrace tooling - not a MobileGL bug and not a bad golden. The core
  entry points are typed (`glAttachShader(program, shader)`), so mapping each argument through
  its own type's map is lossless even when the capture's names collide; only the untyped
  `*ObjectARB` handle calls need the shared map.
- **Fix**: MobileGL-Dev/apitrace `45843686` (branch `mobilegl/android-trace-replay`): typed
  `GLprogram` / `GLshader` arguments resolve through `_program_map` / `_shader_map` first and fall
  back to the ARB handle map only for names created by `glCreate*ObjectARB`; typed creators record
  both maps. Submodule bumped in the same MobileGL commit. Local: 1.17 passes on DirectGLES
  (monolith, inproc) and DirectVulkan (lavapipe).
- **Decision**: keep the case. With typed mapping it checks GL-level correctness of MobileGL like
  any other fixture; the namespace overlap is a property of the capture that a spec-portable
  replay can absorb.
- **Side note (not changed)**: MobileGL advertises `GL_ARB_shader_objects` while its
  `glCreateShaderObjectARB` / `glCreateProgramObjectARB` entry points are stubs
  (`MG_Impl/GLImpl/Exporting/Definitions.cpp`). The string was added on purpose for KWin; whether
  to implement the ARB handle entry points is a separate decision.

### `SanityTest` `DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture`, 2026-10-06 (removed in W3b, then RESTORED)

- **First call (wrong)**: retired with the P2-mask probes as "its arm is gone". The orchestrator's
  review asked whether its failure at the shipping mask was a behaviour gap.
- **Finding**: the code it pins is LIVE. The monolith arm (DISAGG builds under
  `MOBILEGL_TRANSPORT=monolith`, and the FCL build) still walks the frontend units with the
  borrowed-slot work list and its `PairingsIntact` check (`UnitTexturesByHandle()` is false on
  monolith). Its red at 0x3fff is not a product gap: at that mask the twin-sync body it observes
  reads applier records, and a unit-lane process cannot create them (`MGPipeApplyResourceCreate`
  declines under `NoP4aConsumer`), so the probe observed nothing.
- **GL-level replacement tried and measured**: `DsaUpdateKeepsTheBoundUnitScenario` (A bound to
  unit 0; B respecified and filled by name; unit 0 must still sample A). Red-checked locally: it
  stays GREEN with `PairingsIntact` disabled, and still green with the frontend's by-name
  bind-generation bump ALSO removed, and with by-name mipmap / readback / parameter / copy calls
  added in the window. The hazard is no longer reachable through public GL: the by-name
  emulation restores the slot before any draw can replay the list. The scenario stays as a GL
  4.6 8.1/8.5 semantics test on every lane, but it is NOT a guard of the pairing check and is not
  claimed as one.
- **Decision**: the probe is restored, pinned to the P2 mask in its own process (the code under
  test is mask-independent; the mask only selects the sync body it observes). Red-checked
  locally: with `PairingsIntact` disabled it goes red at "the replay never reached the texture now
  in the slot" (the stale pairing was replayed instead of the texture the slot holds); restored,
  green. The resident-respecify assertion now runs first so a red names that damage when it
  occurs. It goes with the borrowed-slot list itself when W4c moves monolith to the by-handle unit
  walk.

## Infrastructure fixes (no check removed)

| Date | What was red | Cause | Fix |
|---|---|---|---|
| 2026-10-06 | `Direct{GLES,Vulkan}.PoisonOmitted.PoisonOmissionScenario.OmittedFieldAbortsOnThatVerb` (verify tree, local; P13 W5 S7) | monolith now runs the record arm, which resolves GenerateMipmap's texture from the verb's handle and never reads the omitted `GetActiveTextureUnit` - the control could not go red | the monolith lanes omit `ReadPixels:GetPixelStoreParameters` (the pair the split lane already uses), which the record arm still reads; verified red-for-its-reason on both backends |
| 2026-10-06 | `integration-verify`, 184 aborts `Fatal{PipeVerifyDiffer, "GetPixelStoreParameters@ReadPixels"}` (verify tree, local; P13 W5 S7) | the verify read hook took the live context as the pack oracle inside the monolith verb port's neutral-pack readback window (it already used the applied pack for server-stamped verbs) | `MG_Record::MonolithPortApplyingForVerify()`; the hook uses the applied pack payload in that window too |
| 2026-10-06 | `ResourceEmit.TheLiveHostWritesWireFiresOnTheCallAPersistentMapProducerWouldSetItOn` (verify unit, local; P13 W5 S7) | the always-false `HasLiveHostWrites` pin is lifted wherever the record arm is built (it gives the bit a producer), and every library builds it now | the case skips under `MOBILEGL_BUILD_RECORD_ARM` with the existing reason; `PinLiveHostWritesNamesABuffer` remains the successor |
| 2026-10-06 | every `ctest` invocation in integration / verify / split jobs exited 8 after the tests passed (`integration-verify`, `integration-split*`, `integration (inproc)`, `spawn_lane_parity.py`) | `PipeCatalogueSourceRoot.cmake` (a CTest include file) used `if(... IN_LIST ...)`; CTest scripts run without policy CMP0057 on the pinned CMake 3.31.10, so the include errored | use `list(FIND)` instead (`MobileGL/MG_Test/Pipe/CMakeLists.txt`) |
| 2026-10-06 | `TcpServer.Start.P8aUnlocatedIoBlocks` failed in 0.07 s on `integration (monolith)` | the loopback TCP endpoints (40613, knob lanes 41613-41617) sat inside Linux's ephemeral port range; with thousands of loopback client connections in the lane, an outgoing socket can own the port the supervisor binds | default endpoint moved to 30613 (knob lanes 31613-31617), below 32768 |
| 2026-10-06 | spawn lanes on hosted runners: `no Welcome within 5000 ms` | a cold `eglInitialize` on the hosted software stack took ~7 s, longer than the spawn handshake's Welcome wait | the Welcome wait takes `max(5000, MOBILEGL_IPC_COLD_START_MS)` (`ClientSession.cpp ColdHandshakeBudgetMs`); the initial-caps wait stays 5000 ms, which `InitialCapsStartup` pins |
| 2026-10-06 | `FencePollScenario` on `integration-verify-split` | the test took its `before` counter snapshot after `glFenceSync`, racing the apply thread that answers the fence | snapshot taken before the work and the fence are queued (test bug, not a product bug) |
| 2026-10-06 | `LogForwardChannel.APeerThatStopsReadingCostsWarnLinesNotTheCallersTime` on `integration-split-strict` (run 37433779649) | test race: when the channel's sender thread started late it dequeued its first line after the 16 KiB queue had already filled, so the lossy class recovered mid-flood and closed a first gap (4 drops) with its own notice - two notices, 1880 named vs 1884 counted. The channel did what its contract says (one notice per gap) | the test parks the sender in its held write (`WaitForAttempt`) before the flood, so the flood makes exactly one gap; 100/100 local runs green |
| 2026-10-06 | `TcpLane.EventForfeitPeer` (`EventForfeitPeer.StreamLogFloodWhileControlIsNotReadHoldsNoApplyThreadAndLosesNoError`) on `integration-split` (run 37433779649) | `notices <= 8` was one machine's measurement: every time the socket takes another burst during the flood, the WARN class recovers and its gap closes with a notice; the loaded runner made 14. Every other F2 assertion held (drops named exactly, no ERROR lost, no forward held its thread) | the bound is now `kFloodRecords / 10` (40), still an order of magnitude below the regression it guards (373, one notice per ERROR line) |
| 2026-10-06 | `DirectGLES.SubsystemMaskRefusal.ObjectSubsystemControlRefusedTexture` flaky under `ctest -j` (a W3b control) | the control globbed `<stem>*` for its logs, which also matched the longer-named sibling `...RefusedTexture`; the shorter control deleted the sibling's log while it ran | glob `<stem>.*` (`Harness/RefusedMaskControl.cmake`) |
| 2026-10-06 | `MGPipe generators and hygiene gates` and `trace case matrix` (fatal census) on the W3b/W3c push | W3c's retired `Wire_SetResidualValueState` died through its own `std::abort()` (the census counts server-image aborts that bypass `SessionFail`), and W3b's mechanical `MGB_CTX` rewrite reached a string in `FieldOwnership.def` that the generator copies into `PipeFieldOwnership.inc` | the row dies through `SessionFail(ResidualBlockSize, ...)`; the `.def` text is restored so the generated file is unchanged |
| 2026-10-06 | skip census: `Direct{GLES,Vulkan}.HandleRecycle.Handles.*` and `...AbaControlHandles.*` leak cases pass -> SKIPPED | W3b stopped pinning `MOBILEGL_PIPE_PUSH` on those lanes (bits 0-13 are fixed on), and the scenario's `LanePinnedALiveAllocator` read "no pin" as "no allocator". A real regression in coverage, caught by the census on its first full run | an unpinned HandleRecycle lane runs the full mask and has the allocator; the cases run (and pass) again. The census also retired the `PeerLatchSite/SetResidualValueStateBlobSize` row W3c deleted |
