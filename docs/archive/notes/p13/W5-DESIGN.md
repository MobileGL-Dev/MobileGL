# P13 W5 design: the record arm without MG_Remote (ID-P13-1 / A2)

Paths are relative to ROOT = `C:\Users\yello\AndroidStudioProjects\FoldCraftLauncher\MobileGL\.claude\worktrees\p13`. Line numbers are at HEAD `88a11b81` (W4d). Apply changes by symbol name.

## 0. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | The no-MG_Remote library is the DISAGG library minus the transport. A DISAGG-guarded block counts as record-core unless it uses the transport: names a transport symbol, or depends on a session, apply thread/role, T0/AHB adoption, shared images, a server-owned window, the server-side caps source, latch domains, codec/ring, or a split-only diagnostic. | Today the default runs the other way: push compiles only what split compiles outside `#if MOBILEGL_BUILD_DISAGGREGATED`. In 884 guarded blocks, ~80% are data plane (§3.1). |
| D2 | Temporary second macro `MOBILEGL_BUILD_RECORD_ARM`: plain CMake variable (not option(), not cache). Equals DISAGG until the flip, forced 1 at the flip, removed by unifdef before W5 closes. | Each reclassification commit leaves both trees with identical .text (free exact negative control). Flip is one bisectable commit. No stale-cache trap (W3a lesson, HANDOFF §2). |
| D3 | Moved implementation → namespace `MobileGL::MG_Record`; interfaces/seams → `MobileGL::MG_Pipe`. Directories follow link-ratchet partition: server-side under `MG_Backend/Record/`, client-side under `MG_Impl/Pipe/Verb/` and `MG_State/.../BufferState/`. | No moved symbol contains "MG_Remote" so `runtime_mode_proof.py:41` stays meaningful; no new PARTITION rows (`link_ratchet.py:109-146`); matches D12's mg_backend. |
| D4 | Every transport question from the record core goes through a function-pointer seam; MG_Remote installs the hook; the no-hook default is the monolith answer. | House pattern: `MG_Pipe/PipeSessionFail.h`, `MGPipeSetApplyThreadProbe` (`MG_Backend/MGPipe/PipeInputs.h:956`, installed `ClientSession.cpp:1878`), `g_stagedBucketResolver` (`StagedShadow.h:68-84`), `MGPipeServerArm()` constexpr-false stub (`PipeInputs.h:950-965`). |
| D5 | Move `FatalFamilies.def` and `MGFatalFamily` enum to MG_Pipe; MG_Remote keeps only wire projection `FatalCodeForFamily`. | Record-core deaths need the family vocabulary; `MG_Remote/FatalFamily.h:16` includes `protocol_generated.h` (flatbuffers include path only when DISAGG, `CMakeLists.txt:768-776`). |
| D6 | `runtime_mode_proof --mode monolith` gains a positive half: MG_Record symbols > 0 AND MG_Remote == 0. | Otherwise a W5 that silently stopped compiling the record arm into FCL shape would still pass. |

## 1. Inventory: what the record arm takes from MG_Remote today
Method: grep `MG_Remote::[A-Za-z_:0-9]+` and `#include.*MG_Remote/` over MG_Backend, MG_Impl, MG_State, MG_Pipe, MG_Util, `MobileGL/*.cpp|h`; transitive closure by hand through `EmitTables.cpp` (verb port) and `PipeApplier.cpp` (ServerVerbSink). Classes: RC = record-core (must exist without MG_Remote); T = transport (stays, session-only); S = seam (hook, default = monolith answer).

### 1.1 Referenced from outside MG_Remote
- `Server::StagedShadowStore` (+CoverageAdd/Has), `StagedShadow.h` — `Managers.cpp:24,1272-1274`; `VkBufferManager.cpp:18,464,478,798,833` — RC → `MG_Backend/Record/StagedShadowStore.h`, `MG_Record::`
- `ServerStagedTexture()`, `StagedTextureStore::{KeyForHandle,KeyForTwinAddress,ClientTexelBox}`, `StagedTexture{MipExtent,UploadExtent,RunImageOffset,ShrinkingAxisCount}` — `DirectGLES.cpp:33,11464-12922` (11 sites); `Managers.cpp:25,3135-10724` (~35); `WireTextureReadback.inc:355-356`; `VkTextureManager.cpp:25,2079-3154`; `VulkanRenderer.cpp:50,1718-1719`; `WireFramebuffer.inc:1703-1722`; `MG_Pipe/PipeApply.cpp:51,1439-2693` — RC → `MG_Backend/Record/StagedTextureStore.h`
- `Client::InstallMonolithVerbPort`, `EmitTables.h` — `Init.cpp:28,192` — RC → `MG_Impl/Pipe/Verb/VerbPort.h`
- `Client::PersistentMapTracker::{Instance,PushIsArmed,OnServerRole,BlockBytes}` — `BufferObject.cpp:20,63-64,100,422-480,514,933`; `BufferObject.h:203` (comment) — RC (OnServerRole → S) → `MG_State/GLState/BufferState/PersistentMapTracker.{h,cpp}`
- `Client::MarkEndTransformFeedbackCaptureTargets`, `MarkReadPixelsPackBuffer`, `GpuWritePending.h` — `GL_Drawing.cpp:16,1393`; `GL_Framebuffer.cpp:20`; `PipeFill.cpp:52`; `TextureEmit.h:73` — RC → `MG_Impl/Pipe/Verb/GpuWriteSet.{h,cpp}`
- `Client::{BufferWritebackSliceBytes, AwaitBufferWriteback}` — `BufferObject.cpp:595,622` — S (defaults 0 / return) → `MG_Pipe/PipeClientSeam.h`
- `Client::BufferWritebackIsReachable` — `BufferObject.cpp:628` — RC (pure: `GpuWritePending.cpp:184-187`) → GpuWriteSet
- `Client::MGPipeStageChunkBytes` — `PipeFill.cpp:1081`; `TextureEmit.h:568` — S (default 0) → PipeClientSeam
- `MG_Remote::SessionLatch`, `MGFatalFamily::ProtocolCorruption` — `DirectGLES.cpp:3219-3220` (inside a DataArmIsRecord() arm) — S → `MG_Pipe::MGPipeRecordLatch`; family RC → `MG_Pipe/PipeFatalFamily.h`
- `Server::ServerLoopInstance().Backend()` (caps source) — `Utils.cpp:56`; `DirectVulkan.cpp:1045-1046`; `VertexInputStateFactory.cpp:215-216,467,614-615`; `VulkanRenderer.cpp:712` — S (K semantics) → `MG_Backend::ApplyRoleBackend()` seam installed by ServerLoop
- `Server::ServerLoop::OnApplyThread`, `Detail::g_applyThreadKey` — `Managers.cpp:243,3317-3319`; `DirectVulkan.cpp:107,1828`; `SlotAllocator.cpp:26,38,63`; `MipmapStorage.cpp:70`; `PipeApply.cpp:1787` — T (role guards; stay DISAGG)
- `Transport::AdoptT0::*` — `Managers.cpp:1354,2839,2974,2977,3043`; `VkBufferManager.cpp:20,824,874,876,1043`; `VkBufferObject.cpp:12,134` — T
- `Server::SharedImages::*`, `SharedImageRegistry.h` — `DirectGLES.cpp:35,18112`; `VkTextureManager.cpp:27,2155-3023`; `VulkanRenderer.h:28,780`; `VulkanRenderer.cpp:53`; `WireSharedImage.inc`, `WireYuvImage.inc` — T (stubs needed §3.4)
- `Client::EmitObjectDeathRecord`, `WireTables.h` — `Managers.cpp:30,274`; `DirectVulkan.cpp:27,83,108` — T (only under Transport != Monolith)
- `Client::{ClientSession,RunsAsTheServerRole,CapsMirrorInstance,ContextValuesWireLive,EmitApplierResetRecord,ClientWireRecordsEmitted,Send*ContextFrame,EmitBindContextRecord,ContextFrameEmit,BackendObject_Remote,ClientSessionInstance}`; `Server::{ServerSession::Active}`; `MGCapsServerConsumes`; `Client::AdoptTierIsEmulate` — `PipeFill.cpp` (33 refs); `EGLImpl.cpp` (18); `GL_Getter.cpp:3013-3062`; `ResourceTracker.h:607`; `PipeApply.cpp:1735-1786,2734`; `MG_Backend/Init.cpp:162-400` — T
- `Client::{ResetServerProbeCache,ConfiguredServerAvailable}` — `Init.cpp:111,253` — T
- `Wire::kSegEvent`, `Transport::LinkSegment::Event` — `ResourceTracker.h:602,610` — T
- `Client/SlotCaps.h` (header-only macros) — `GL_Drawing.cpp:23`, `GL_Getter.cpp:46`, `GL_Query.cpp:19`, `GL_Sync.cpp:17`, `GL_Texture.cpp:41`, all unguarded — T-shim already safe without DISAGG (`SlotCaps.h:138-149`); optional move to `MG_Impl/GLImpl/SlotCaps.h` (S9)

### 1.2 Transitive closure of the verb port (`MG_Remote/Client/EmitTables.cpp`)
- `VerbChannel`, `ChannelFor`, `ApplyOnMonolithPort`, `MonolithVerbSink`, `MonolithReplySink` (256-437): depends on ClientSession (EmitAndWait*, MaxReplyBytes, RequireReadPixelsReplyFits, Encoder().StageBytes), Wire::{WireTail,ReplySink}, Server::ServerVerbSink — RC once the session is behind a `VerbSession` interface
- `RequireSession` (222-234): ClientSession::Active, SessionFail(NoClientSession) — T (becomes the resolver MG_Remote installs)
- `BeforeReadOnlyVerb` (239-242): PushPersistentMapsBeforeVerb (tracker), MGPipeDrainDeferredDestroys (`PipeMutation.h:216`, DISAGG-gated) — RC (+ drain gets no-op non-DISAGG stub)
- The 51 ported emitters (InstallMonolithVerbPort list 2325-2384): Clear ×9, Draw ×20, Dispatch ×2, Blit ×2, CopyTex ×2, CopyImage, GenerateMipmap, ReadPixels, GetTex(ture)Image ×2, BindImageTexture, ShaderStorageBlockBinding, XFB ×6 (443-1760 + TextureReadbackEmit.inc): OwnedDrawInputs, GpuWriteSet, PersistentMapTracker, MGPipeTextureEmitterInstance, MGPipeFramebufferEmitter, ResourceTracker, E2 knobs (g_dropDrawEmission, 100-181) — RC
- `RequireReadbackReplyComplete` (1112-1140): ClientSession::DeviceLost, ConfirmLossAfterDecline, SessionFail(ReplyError/ReadbackDeclined/ReplyStatusInvalid/ReadbackReplyShort) — RC + S (DeviceLost/ConfirmLoss) + record-fail seam
- `EmitMemoryBarrier*`, `EmitPatchParameteri` (1546-1560, 1759): VerbChannel — RC (not ported; harmless to move)
- `EmitPresent`, fences (1908-2024, RememberFenceSignaled, poll escalation Ipc.PollEscalate), QueryEmit.inc, AnswerGetIntegeri_v, AnswerIsTimerQuerySupported, class-C table, `BuildRemoteEmitTable` (2170-2281), ServerSetEGLSwapInterval (2244), EmitFinishWait, RemoteEmitTable — T
- Planners/readback helpers: PlanDrawInfo/Range/Indirect, RemoteIndexSizeFor, PlanReadbackBands, Scatter*, TightReadbackByteCount, ReadbackReplyIsComplete (2398-2589) — pure, RC
- `UnmigratedVerbFatal` (91-96): SessionFail(UnmigratedVerb) — RC via record-fail seam

### 1.3 Transitive closure of ServerVerbSink (`MG_Remote/Server/PipeApplier.{h,cpp}`)
- SetBackend (105-117): ReleaseQueries/Fences (T); ServerStagedTexture().SetDeviceLimits (RC) — split with virtual OnBackendChanged()
- Table (119-133): none — RC
- OnClear, OnBlit (286-391): Wire::WireProtocolFatal(At) — RC
- OnGetTextureImage, OnReadPixels, ReadBoundFramebufferTight, ReadTextureImageTight, LandReadbackInBuffer, On*ToBuffer (459-830): Wire::ReplySink, SessionFail(ProtocolCorruption) (493), SessionLatched() (595), SessionLatch(...) (745-779) — RC via seams
- OnDrawVbo + anon helpers (832-1136): Wire::CheckDrawUserIndices (897), WireProtocolFatal(At) (934, 1061), SessionLatch (799, 873, 881) — RC; move CheckDrawUserIndices (`PipeWireCodec.cpp:640`, pure) to MG_Pipe
- OnLaunchGrid … OnCopyFramebufferToTexture (1138-1458): WireProtocolFatal (1382) — RC
- OnMapPersistent (144-147): ServerSession::AdoptStoreT0 — T
- Fences, ReportSignaledFences, QueryServer.inc (135-284): ServerSession (251), WireProtocolFatal — T (not on the port)
- OnPresent (401-457): ServerSession credit, SessionLatch — T
- OnApplierReset, OnSharedImage, OnBindContext, OnObjectDeath (1460-1775): ServerSession, ServerLoopInstance, SharedImages, Transport::AdoptT0 — T
- ReplyPool, PipeApplier (70-99, 1776-1948): rings, decoder, ClientSession::ScopedApplierEntry (1828) — T
- Interface Wire::WireVerbSink / ReplySink / WireTail / QueryResultReply (`PipeWireCodec.h:241-245, 483-490, 511-516, 518-701`): pure virtuals — RC → `MG_Pipe/PipeVerbSink.h`

Header-only stores: StagedShadow.h depends only on FatalFunnel.h:43 (SessionFail(StageSnapshotTooNarrow) at :175). StagedTextureStore.h depends on StagedShadow.h:103, FatalFunnel.h:106 (8× SessionFail, 441-919), Config.h (RecordArmAliasesFrontend :310, Transport :1030). Both RC once the fail seam exists.
Client hooks: GpuWritePending.cpp:126-182 (rows 0-5, counters) pure frontend RC; :189-272 (slice/chunk/await) S. PersistentMapTracker.cpp needs Ipc.PersistentBlockKb (:703) and Ipc.PersistentHashSuppress (:1052) RC knobs; ServerLoop::OnApplyThread (:717) S (use MGPipeServerArm()); ClientSession::DeviceLost (:760) S; AdoptTierIsEmulate (:1261-1290) T — keep in MG_Remote under the same symbol name (it's in the ratchet baseline).

### 1.4 DISAGG-only declarations outside MG_Remote the record arm needs (must be unguarded)
- Applier verb handles: VerbIndirectBuffer…ClearVerbHandles `MG_Pipe/PipeApply.h:876-904`
- Respecified-level extents: `PipeApply.h:1312-1335`, `MGPipeTypes.h:466-485`
- ShadowAllocationBytesFor `PipeResource.h:56-84` (tracker needs it)
- OwnedDrawInputs.h:12 (DISAGG)
- Config data-arm predicates (`Config.h:739-767`), MonolithTakesRecordArm / InitDataArm (`ConfigLoader.cpp:41,462-488,737`)
- Magma wire arm members `VulkanRenderer.h:632-916` and all Wire*.inc except WireSharedImage.inc / WireYuvImage.inc
- Magma depth-resolve probe WireDepthResolveProbe.cpp (today in MG_Remote source list `CMakeLists.txt:675-679`); POST hooks `DriverBugProbes.cpp:14,2482`, `DriverPost.cpp:3031`
- Fatal family enum values `MGPipeFatalFamily::{ProtocolCorruption,DeviceLost}` (`PipeSessionFail.h:57-63`)

## 2. Target layout
### 2.1 Files
- `MobileGL/MG_Pipe/PipeVerbSink.h` (SERVER header): MGPipeVerbSink, MGPipeReplySink, MGPipeVerbTail, MGPipeQueryResultReply (from PipeWireCodec.h:241-245, 483-701). PipeWireCodec.h keeps `using WireVerbSink = MG_Pipe::MGPipeVerbSink;` etc. so the decoder doesn't change.
- `MobileGL/MG_Pipe/FatalFamilies.def` (moved), `MG_Pipe/PipeFatalFamily.h` (new): MG_Pipe::MGFatalFamily, FatalFamilyName, MGFatalFamilyCount. MG_Remote/FatalFamily.h keeps FatalCodeForFamily + `namespace MG_Remote { using MG_Pipe::MGFatalFamily; using MG_Pipe::FatalFamilyName; }`.
- `MobileGL/MG_Pipe/PipeSessionFail.h` (extended) + defaults in `MG_Pipe/PipeApply.cpp` next to MGPipeSessionFail (:141): `[[noreturn]] MGPipeRecordFail(MGFatalFamily, fmt, ...)`, `bool MGPipeRecordLatch(...)`, `bool MGPipeRecordLatched()`, `MGPipeProtocolFatal(what, detail)` / `MGPipeProtocolFatalAt(...)` (same line text as PipeWireCodec.cpp:416-423), `MGPipeInstallRecordFailHooks(fail, latch, latched)`. Defaults: format into 512 bytes, MGLOG_F plus a stderr echo (as FatalFunnel.cpp:105-110, so death tests in push tree still match), then abort. Latch defaults to fail; latched defaults to false.
- `MobileGL/MG_Pipe/PipeClientSeam.h` (+ defaults in PipeFill.cpp or small `MG_Impl/Pipe/Verb/ClientSeam.cpp`): hooks StageChunkBytes() (0), WritebackSliceBytes() (0), AwaitWriteback(BufferObject&) (return), SessionDeviceLost() (false), ConfirmLossAfterDecline(ms) (false). BufferObject forward-declared.
- `MobileGL/MG_Backend/Record/StagedShadowStore.h`, `StagedTextureStore.h` (SERVER/core): MG_Record:: stores; SessionFail → MGPipeRecordFail.
- `MobileGL/MG_Backend/Record/RecordVerbSink.{h,cpp}` (SERVER/core): MG_Record::RecordVerbSink : MG_Pipe::MGPipeVerbSink with every RC method from §1.3, tallies, LastDrawRecord. Must not include Transport/, Wire/, <unistd.h>, ClientSession.h.
- `MobileGL/MG_Backend/Record/ApplyRoleBackend.{h,cpp}`: `MG_Backend::BackendObject* ApplyRoleBackend()`: hook result under a transport, else pActiveBackendObject. ServerLoop installs the hook.
- `MobileGL/MG_Impl/Pipe/Verb/VerbPort.{h,cpp}`, `TextureReadbackEmit.inc` (moved) (FRONTEND): MG_Record::VerbSession (abstract: EmitAndWait, EmitAndWaitTails, MaxReplyBytes, RequireReadPixelsReplyFits, StageBytes, ConfirmLossAfterDecline), SetVerbSessionResolver, VerbChannel, all VerbChannel emitters, planners, readback helpers, E2 knobs, ApplyOnMonolithPort, AssignVerbEmitters(table) (all VerbChannel slots), InstallMonolithVerbPort(table) (the 51), MonolithVerbPortInstalled(). On the port VerbChannel keeps the session pointer null so monolith pays no virtual call.
- `MobileGL/MG_Impl/Pipe/Verb/GpuWriteSet.{h,cpp}` (FRONTEND): GpuWritePending.cpp:28-187 (enum, rows, counters, GpuWriteSetIsClientSide, BufferWritebackIsReachable).
- `MobileGL/MG_State/GLState/BufferState/PersistentMapTracker.{h,cpp}` (FRONTEND): whole tracker except AdoptTierIsEmulate. OnServerRole() → MG_Pipe::MGPipeServerArm(); DeviceLost → client seam; knobs → MG_Config::RecordArm. In MG_State because BufferObject.cpp calls it and MG_State includes nothing from MG_Impl today.
What stays in MG_Remote and shrinks:
- Client/EmitTables.cpp: BuildRemoteEmitTable calls MG_Record::AssignVerbEmitters(table) after the class-C block; keeps fences, queries, Present, SwapInterval, class A/C, per-package static_asserts (2128-2168), EmitFinishWait, RemoteEmitTable; defines `ClientVerbSession final : MG_Record::VerbSession` over ClientSession&; the resolver (formerly RequireSession) installed in BuildRemoteEmitTable.
- Client/GpuWritePending.cpp: keeps AwaitBufferWriteback, BufferWritebackSliceBytes, MGPipeStageChunkBytes[For] and installs them into PipeClientSeam (static registration).
- Server/PipeApplier.{h,cpp}: ServerVerbSink : public MG_Record::RecordVerbSink; overrides OnMapPersistent, fences, queries, OnPresent, OnApplierReset, OnObjectDeath, OnBindContext, OnSharedImage, OnBackendChanged(). ReplyPool and PipeApplier unchanged.
- FatalFunnel.cpp: registers record-fail hooks statically (namespace-scope initializer calling MGPipeInstallRecordFailHooks(&SessionFail-adapter, &SessionLatch-adapter, &SessionLatched)); hook globals must be constant-initialized. Static (not role-init) because both roles call SessionFail directly today, unit cases included; keeps SessionFaultCount() and SessionFault publish byte-identical in every DISAGG process. Existing MGPipeSessionFail (four families, server-role install) untouched; merging = W7 hygiene.
- WireTables: EmitObjectDeathRecord, RunsAsTheServerRole, EmitApplierResetRecord, ContextValuesWireLive stay (all T).
Naming rule: rename namespace only (MG_Remote::Server::/Client:: → MG_Record::), keep identifiers (ServerStagedTexture, RemoteDrawBindings, PlanDrawInfo…) so test suites keep names (G14); change is a sed. No forwarding headers under MG_Remote/ for product code; temporary forwarding header for Wire test sources OK until S8.
Port install site: move the InstallMonolithVerbPort call from `MobileGL/Init.cpp:191-193` to top of MG_Impl::Init() (`MG_Impl/Init.cpp:23`), right after MG_Backend::Init() (`Init.cpp:185,197`). Same behaviour; removes the Init.cpp (SHARED) → FRONTEND edge W7's server-only link target would trip on.

### 2.2 CMakeLists.txt
- After flatbuffers guard (:538-553): `set(MOBILEGL_BUILD_RECORD_ARM ${MOBILEGL_BUILD_DISAGGREGATED})` (normal variable). Always emit `-DMOBILEGL_BUILD_RECORD_ARM=0|1` next to :741-743.
- New block `if (MOBILEGL_BUILD_RECORD_ARM) list(APPEND SOURCE_FILES ...)` between the push list (:556-578) and the MG_Remote list (:580-681): MG_Backend/Record/RecordVerbSink.cpp, MG_Backend/Record/ApplyRoleBackend.cpp, MG_Impl/Pipe/Verb/VerbPort.cpp, MG_Impl/Pipe/Verb/GpuWriteSet.cpp, MG_State/GLState/BufferState/PersistentMapTracker.cpp, MG_Backend/DirectVulkan/Renderer/WireDepthResolveProbe.cpp (moved out of :679).
- MG_Remote files at :632, :646-647, :649 stay, smaller.
- S7: `set(MOBILEGL_BUILD_RECORD_ARM ON)` unconditionally. S8: delete the variable and definition; fold the block into SOURCE_FILES (:308).
- flatbuffers/glvnd include dirs (:768-776) stay DISAGG-only = compile-time proof the record core doesn't reach the protocol header.

## 3. Guard strategy
### 3.1 Counts (opening lines `^\s*#\s*(if|elif|ifdef|ifndef).*MOBILEGL_BUILD_DISAGGREGATED`)
MG_Backend 631 (613 `#if MOBILEGL_BUILD_DISAGGREGATED` blocks); MG_Impl 119 (118); MG_State 87 (86); MG_Pipe 39 (38); MG_Util 17 (17); MobileGL/*.cpp|h 12 (12); MG_Test 71; MG_IntegrationTest 20 (+ ~30 `if (MOBILEGL_BUILD_DISAGGREGATED)` in its CMake).
Other spellings: 13 `#if !MOBILEGL_BUILD_DISAGGREGATED`: DirectGLES.cpp:8553,11148; DirectVulkan.cpp:141,204,226; VkSamplerManager.cpp:560; VulkanRenderer.cpp:1485,4837,5410; VulkanRenderer.h:574,586,2149; ProgramArtifactsCodec.cpp:392. 6 compound: Managers.cpp:2680, VkBufferManager.cpp:22, EGLImpl.cpp:620 (&& __ANDROID__); VulkanRenderer.cpp:71,15343; Managers.cpp:3923 and PipeApply.cpp:1247 (VERIFY && !DISAGG). 1 diagnostic-shape guard PipeInputs.h:30.
Block classification (nesting-aware walk, comments excluded):
- A no transport token: 369 blocks, 8,713 lines (MG_Backend 304 / Impl 27 / Pipe 11 / State 21 / Util 4 / top 2)
- F neither transport nor record tokens: 263, 1,592 (176/31/14/32/8/2)
- E transport semantics, no MG_Remote symbol: 105, 1,254 (55/17/4/22/5/2)
- B names an MG_Remote symbol only: 46, 658 (15/23/2/3/0/3)
- C names MG_Remote AND record terms: 65, 9,464 (37/16/5/6/0/1)
- D transport semantics and record terms: 36, 3,155 (26/4/2/2/0/2)
Transport token list: ClientSession, ServerSession, ServerLoop, Transport::, MG_Config::Ipc, SharedImage, AdoptT0/T0/externalAhb, RunsAsTheServerRole, CapsMirror, SessionLatch, ServerRole, SplitRoles, TransportMode::Spawn/InProcess, ServerOwnedWindow, ServerSetContextLive, DeviceLost, RoleSplit, ApplyThread, MGPipeServerArm, MGPipeSessionLive, ClientBlock, EndedServerSession.
Sampling: F in DirectGLES.cpp: 43 blocks, 6 transport (:1183,:1429 T0 retires; :16128,:17054 window extent; :17024 swap interval; :18006 server-context-live), 37 handle-arm memos/XFB role keys/epochs → F ≈ 85% record. E mostly role guards that stay (MipmapStorage.cpp 18 Refuse…FromApplyThread; PipeFill.cpp 21).
Expected: ~690 blocks move to RECORD_ARM (A 369 + ~224 F + record halves of C/D), ~250 stay DISAGG (B 46 + ~100 E + ~40 F + transport halves of C/D).

### 3.2 Mechanics
1. Script (extend HANDOFF §3's pyunifdef.py): emit reviewed CSV {file, open line, close line, class, rule hit}; rewrite class A and reviewed-record F blocks `#if MOBILEGL_BUILD_DISAGGREGATED` → `#if MOBILEGL_BUILD_RECORD_ARM`; rewrite every `#if !MOBILEGL_BUILD_DISAGGREGATED` → `#if !MOBILEGL_BUILD_RECORD_ARM` (frontend-arm variants of no-MG_Remote build, vanish at flip); rewrite `VERIFY && !DISAGG` (2 sites) → `VERIFY && !RECORD_ARM`; leave B, E, transport-classified F; skip `MG_Pipe/generated/` (decoder hook from gen_pipe.py:994,1039; transport) and C/D (hand pass).
2. Bias: when in doubt classify as record (wrong-record block that names transport fails to compile in push tree at S7 = loud net; wrong-transport only loses a record path in FCL = quiet).
3. Exceptions staying DISAGG on purpose: PipeInputs.h:30 (MOBILEGL_PIPE_POISON armed by DISAGG — FCL must not pay per-read poison checks); Ipc.StrictErrors / Ipc.Audit uses; m_serverStampedVerb (PipeInputs.h:760); compound `&& __ANDROID__` / `!_WIN32` guards (T0 / sync_file).

### 3.3 Idioms
- Nested guard: outer `#if MOBILEGL_BUILD_RECORD_ARM`, inner `#if MOBILEGL_BUILD_DISAGGREGATED` around transport lines. Examples: Managers.cpp:1257-1376 (ServerStaged() and RequireStagedCoverageForPendingRanges record; T0 retire machinery transport); VkBufferManager.cpp:243-1211 (AdoptT0 at 824-1043); BufferObject.cpp:443-530.
- Transport ternary: split `cond = Transport != Monolith ? MG_Remote::X() : recordExpr;` into DISAGG-only wireX and unguarded `wireX || recordExpr`. Examples: PipeFill.cpp:3549-3566 (contextValuesWireLive); the six caps reads become ApplyRoleBackend().
- Stubs: in VulkanRenderer.h `#if !MOBILEGL_BUILD_DISAGGREGATED` inline no-ops for AcquireNotedSharedImages(), AcquireSharedImage(...), VkTextureManager::HasNotedSharedImageUses(); WireDraw.inc:252,814,891 and WireFramebuffer.inc:168 stay unguarded. Same for MGPipeDrainDeferredDestroys (PipeMutation.h:216). Precedent PipeInputs.h:960-965.
- Role guards (`if (Transport != Monolith && !OnApplyThread())`): leave under DISAGG (no-ops in monolith).

### 3.4 Hand-attention (C/D)
- VkTextureManager.cpp:1869-3266, VkTextureManager.h:607-682 (1,393 / 74 lines, 36 SharedImage refs): handle-keyed texture arm → record; shared-image import and YUV stay DISAGG.
- VulkanRenderer.h:151-216, 632-916: m_wire* record; SharedImage*/m_sharedImageReads (:727-805) DISAGG — split members, add stubs.
- WireDraw.inc, WireFramebuffer.inc (whole-file guard) → RECORD_ARM using stubs. WireSharedImage.inc, WireYuvImage.inc stay DISAGG.
- VulkanRenderer.cpp:15343-15458 device extensions: AHB-import extension DISAGG; renderpass2 depth resolve, stencil export, robustness2 → record (plan §1.2 device row). Device smoke deferred.
- VkBufferManager.cpp:243-1211 T0 import: nested guard; Ipc.WireDeferredMb (:1114) → MG_Config::RecordArm.WireDeferredMb.
- VertexInputStateFactory.cpp:396-534: :467 reads ServerLoopInstance().Backend() INSIDE the record arm → ApplyRoleBackend(). Also fixes a latent W4 divergence: on monolith the server loop has no backend so native fp64 is never taken there.
- UniformManager.cpp:3153-3207, VulkanRenderer.cpp:705-716, DirectVulkan.cpp:1038-1052, Utils.cpp:49-70 caps-source ternaries → ApplyRoleBackend().
- DirectGLES.cpp:18107-18982, Managers.cpp:8274-8433 shared images: stay DISAGG.
- Managers.cpp:231-283, 354-464, 3281-3334 object-death record, twin drop per ended session, epoch pin: stay DISAGG (transport K).
- DirectGLES.cpp:3212-3225 MG_Remote::SessionLatch in record arm → MG_Pipe::MGPipeRecordLatch.
- MG_Pipe/PipeApply.cpp:122-268 MGPipeSessionLatch seam impl: unguard seam bodies so record-core callers link without MG_Remote; DeviceLost/ProtocolCorruption enum values in PipeSessionFail.h:57-63 lose their guard.
- PipeApply.cpp:1428-1485, 2692 staged-texture uses → record. PipeApply.cpp:1735, 1786, 2734 (ServerSession, ClientSession, AdoptTierIsEmulate) stay DISAGG.
- MG_Backend/MGPipe/PipeInputs.cpp:50-597, PipeInputs.h:889-1009 role/dual block: stay DISAGG with stubs where record code calls them.
- Config.h:465-767, ConfigLoader.cpp:449-725: §4.
- EGLImpl.cpp, MG_Backend/Init.cpp:70-366, PipeFill.cpp:355-494, 999-1065, SlotAllocator.*, MipmapStorage.cpp: stay DISAGG.
- DirectVulkan.cpp:141-379, VulkanRenderer.cpp:1485-1578, 4837-4908, VulkanRenderer.h:574-592, 2149 (`!DISAGG`): deleted at S8 (plan §1.1 g_programResourceCaches item; replacement = what DISAGG already uses, LinkArtifacts).

### 3.5 Test guards
- Unit: move StagedTextureStoreTest (27), StagedShadowTest (5), StagedShadowProductionTest (8), StagedTextureProductionTest (4), RemoteDrawPlan (7), RemoteReadback (10), PersistentMapWritability/FaultOwnership (6) from MG_Test/Wire/ (DISAGG-only, MG_Test/CMakeLists.txt:111-116) into MG_Test/Record/, registered under `if (MOBILEGL_BUILD_RECORD_ARM)`, unconditional after S8; suite names unchanged.
- Other unit files: SplitBufferTest.cpp, ResourceEmitTest.cpp, TextureEmitTest.cpp, SanityTest.cpp use Ipc.PersistentBlockKb and MG_Remote headers; reclassify their 71 guards with the same script.
- Integration: split dataArmIsRecord out of SplitRuntimePeek (DISAGG-only via MGITEST_SPLIT_RUNTIME_PEEK, MG_IntegrationTest/CMakeLists.txt:1936-1940) into a RecordArmPeek compiled under RECORD_ARM. Six scenarios branch on it: CopyImageStoreReadbackScenario.cpp:133, DescriptorPoolGrowthScenario.cpp:113, GenerateMipmapServerScenario.cpp:137,178, GpuWrittenDrawInputScenario.cpp:241, WireIndirectDispatchScenario.cpp:76, WireIndirectDrawScenario.cpp:93 — otherwise they'd assert the frontend arm in the push tree after the flip.
- Skip trap: `mgl_itest_probe_for_symbol(... "MobileGL/MG_Remote/Client" "IsLivePersistentMap")` (MG_IntegrationTest/CMakeLists.txt:1964-1977) silently turns PersistentCoherentMapScenario's membership assertion into a skip when the tracker moves. Repoint in the same commit.

### 3.6 Scripts policing guards / MG_Remote paths
- scripts/ci/runtime_mode_proof.py:32-44, self-test :124-148: monolith = MG_Remote == 0 AND MG_Record > 0 (anchor on MG_Record::RecordVerbSink's vtable, survives LTO); disaggregated: both > 0. Fixtures for "monolith without MG_Record" and "disaggregated without MG_Record" (both red). Land at S7.
- scripts/link_ratchet.py:109-146 PARTITION, :184-187 REQUIRED_FLAGS: no rows needed. Re-run at S4: symbol list should be unchanged (baseline referees keep names: MG_Remote::Client::AdoptTierIsEmulate, EmitObjectDeathRecord, ClientSession::* …). If it moves, rebaseline in same commit naming the cause. Optional rows MG_Backend/Record/ → SERVER.
- scripts/check_include_closure.py PROBES: add probe `record-core` over MG_Backend/Record/StagedTextureStore.h, MG_Pipe/PipeVerbSink.h, MG_Impl/Pipe/Verb/VerbPort.h, MG_Pipe/PipeFatalFamily.h; Forbidden MobileGL/MG_Remote/ and 3rdparty/flatbuffers/. Text mode blind to #if (docstring :28-35) so even a guarded MG_Remote include reds → seams are the only option. Bump --expect-probes 4 → 5 in test.yml:2395; MG_Test/Purity/CMakeLists.txt:10 passes no --expect-probes.
- scripts/ci/fatal_census.py: FAMILIES_DEF (:113) → MG_Pipe/FatalFamilies.def; Rule-2 regexes (:176,:182) add `(?<![A-Za-z0-9_])MGPipeRecord(Fail|Latch)\s*\(`; FUNNEL_SITES["MobileGL/MG_Pipe/PipeApply.cpp"] anchor `void MGPipeRecordFail(`; SCAN_ROOTS (:97-103) already cover; --write-baseline per commit.
- scripts/ci/ph_latch_sites.py:28 LATCH_CALL, :38-43 FILES, UNREACHABLE row ("PipeApplier.cpp", 'Fatal{ReadbackDeclined, "ReadPixelsToBuffer"}'): add MGPipeRecordLatch; add "RecordVerbSink.cpp": ROOT/"MobileGL/MG_Backend/Record/RecordVerbSink.cpp"; re-key row; update file column of MG_Test/Wire/PeerLatchTest.cpp kRows for moved sites. ApplyOne MECHANICS entry stays.
- scripts/ci/wire_declines_audit.py:85 PP_DISAGGREGATED: accept MOBILEGL_BUILD_RECORD_ARM as second fail-open guard during S2-S7 + self-test fixture; after S8 Wire*.inc carry no guard.
- scripts/ci/retired_switches.py RULES: at S8 add rule: a conditional naming MOBILEGL_BUILD_RECORD_ARM is red.
- espryt_memo_purity.py: no change. link_seam_purity.py, protocol_revision_pin.py, gen_pipe.py, gen_protocol.py: no change.
- scripts/ci/skip_census.py + scripts/data/skip_census_baseline.json: rewrite baseline at S7 (push-tree skips shrink, names grow; allowed by G14); name moved Wire suites in commit message.

## 4. Config and FCL build
- Config.h: move `extern Bool MonolithTakesRecordArm;` and new `struct RecordArmTable { Uint32 PersistentBlockKb = 64; Uint32 PersistentHashSuppress = 1; Uint32 WireDeferredMb = 64; }; extern RecordArmTable RecordArm;` out of DISAGG block; drop those three fields from IpcTable. Non-DISAGG #else (:758-767) becomes `inline Bool DataArmIsRecord() { return MonolithTakesRecordArm; }`, `inline Bool RecordArmAliasesFrontend() { return MonolithTakesRecordArm; }`, Transport still constexpr Monolith. Stage behind `#if MOBILEGL_BUILD_RECORD_ARM` (S5) so push tree keeps constexpr false until the flip.
- ConfigLoader.cpp: define MonolithTakesRecordArm and RecordArm outside guard (:32-42); move InitDataArm() (:462-488) out of guard and call unconditionally after InitFeatures() (:731-737), keeping "ignored under a transport" inside DISAGG; parse MOBILEGL_IPC_PERSISTENT_BLOCK_KB, MOBILEGL_IPC_PERSISTENT_HASH_SUPPRESS, MOBILEGL_IPC_WIRE_DEFERRED_MB (now :567-578) in new unconditional InitRecordArm(), register as accepted env names in both shapes; InitIpc summary line (:702-703) prints them in DISAGG. Callers: PersistentMapTracker (:703,:1052), VkBufferManager.cpp:1114, SplitBufferTest.cpp:67-90,473, SanityTest.cpp:4595-4694.
- Side benefit: MOBILEGL_PIPE_DATA_ARM=frontend available in FCL until W6 (device escape hatch, already scoped W4-W6, Config.h:746-750).
- Gradle/FCL: no change (build.gradle:46,76-77 passes only DISAGG/INPROC; FCL defaults DISAGG OFF). Do NOT add an option() (Gradle .cxx cache keeps old value, HANDOFF §2). android-plugin/*.kts and anland scripts unchanged. FCL picks the record arm up after S7; device smoke deferred.

## 5. Risks
1. Link ratchet: keep transport-named referees under current names; no new PARTITION rows. Watch whether RecordVerbSink.cpp.o newly references a FRONTEND symbol PipeApplier.cpp.o only reached via ClientSession::ScopedApplierEntry; must not include ClientSession.h (include-closure probe enforces). W7 note: InstallMonolithVerbPort from MG_Impl::Init, not SHARED Init.cpp.
2. G-gates: G1 retired (ID-P13-4); G2/G14 = skip census; moved unit suites keep Suite.Case names; push lanes gain names, lose skips; IsLivePersistentMap probe path is a silent-skip trap (§3.5).
3. Death tests / fault counters: record-fail default must echo to stderr; in DISAGG hooks must be statically registered (ServerLoopTest, PeerLatchTest, FatalFamilyTest read SessionFaultCount() without role init).
4. Verify tree (build-linux-verify: no DISAGG, MOBILEGL_PIPE_VERIFY): after S7 runs the monolith record arm under the comparator — never run before (integration-verify-split is inproc only). Run integration-verify locally at S7. The two VERIFY && !DISAGG pins (Managers.cpp:3923, PipeApply.cpp:1247) must be gone by then.
5. Portability: PersistentMapTracker.cpp:25-31 includes <sys/mman.h>, <ucontext.h>, <sys/syscall.h> unconditionally; non-DISAGG list still supports WIN32 (CMakeLists.txt:707-712) and APPLE. Guard mprotect arm with `#if defined(__linux__) || defined(__ANDROID__)`, fall back to hash arm (MprotectArmAvailableForTest models "unavailable", :840).
6. FCL process is a JVM: tracker's SIGSEGV handler chains by design (PersistentMapTracker.cpp:54-61,308-328), installs lazily after the JVM — first exposure to FCL's HotSpot; device smoke deferred. Confirm a knob turns the mprotect arm off (none visible; consider one at S7). Memory: shadows round to whole pages (PipeResource.h:56-84); measure RSS on MC 26.3 corpus (record only, ID-P13-2).
7. FCL APK binary size: ~20k guarded lines become compiled (A ≈ 8.7k + ~1.35k F + ~10k record halves of C/D) + ~6.5k moved from MG_Remote − ~0.6k !DISAGG arms ≈ half of MG_Remote's ~46k → ~+8% source lines for FCL lib. Release has ThinLTO + --gc-sections (CMakeLists.txt:150-169). Measure at S0 and S7 (Release aarch64 .text of push lib, split lib, post-flip push lib); record, not a gate.
8. Per-draw cost in FCL: port planning/sink, page-rounded shadows, loss of monolith-only TrySetupDrawFastPath (plan W4a risk). Recorded, not gated (ID-P13-2).
9. Transport-negative control stays meaningful: push lib still has no transport parser; retrace_pull_library_control.sh must red for the same sentence; re-run at S7.
10. Magma: DirectVulkan integration runs only in CI (note: user now allows local DV integration runs), so S3/S7 Magma correctness (shared-image stubs, device extensions) rests on CI; split Magma commits so a red bisects.

## 6. Steps (each keeps DISAGG split tree and no-MG_Remote push tree green)
Local loop (HANDOFF §3): build_split.sh, build_push.sh, itest.sh, one suite at a time, then skip census vs previous step.
- S0: measurements only — record .text, nm counts (MG_Remote, MGPipeApply), push JUnit set and skip list, ratchet report, fatal census for split/push/verify trees. Commit the classification CSV as the W5 inventory under docs/Disaggregated/notes/p13/. Tests: none.
- S1: seams, additive — MG_Pipe/PipeFatalFamily.h + moved .def (MG_Remote/FatalFamily.h keeps projection + aliases); MGPipeRecordFail/Latch/Latched/ProtocolFatal with static registration in FatalFunnel.cpp; MG_Pipe/PipeVerbSink.h + PipeWireCodec.h aliases; PipeClientSeam.h, ApplyRoleBackend; RecordArmTable; script updates fatal_census (path, regex, funnel), ph_latch_sites regex. Push gains only seam symbols. Tests: both trees ctest -L unit; new seam unit cases (default death writes Fatal{…} to stderr; DISAGG hook moves SessionFaultCount); fatal_census.py --self-test and run; ph_latch_sites.py --self-test; runtime_mode_proof --mode monolith (MG_Remote = 0).
- S2: add MOBILEGL_BUILD_RECORD_ARM (= DISAGG). Script rewrite of class A, reviewed F, !DISAGG and VERIFY&&!DISAGG guards (§3.2). One commit per directory: MG_State, MG_Impl, MG_Pipe, MG_Util, DirectGLES, DirectVulkan, top-level, tests. wire_declines_audit accepts the new guard. Invariant: .text and nm byte-identical to S1 in both trees. Tests: objdump -d / sha of .text diff empty for push, split, verify; wire_declines_audit.py --self-test; unit.
- S3: hand-split C/D (§3.4), add stubs, transport-ternary idiom; caps reads → ApplyRoleBackend() (installed by ServerLoop). Invariant: push byte-identical to S1. Tests: split tree full unit, integration-gpu monolith and inproc, split/spawn/tcp, retrace (230/236). Magma via CI (or locally now that it's allowed).
- S4: code motion out of MG_Remote, still compiled only when RECORD_ARM — stores → MG_Backend/Record/; PipeApplier → RecordVerbSink + ServerVerbSink; EmitTables → VerbPort + MG_Remote remote table and ClientVerbSession; GpuWritePending → GpuWriteSet + seam impl; tracker → MG_State; WireDepthResolveProbe.cpp to RECORD_ARM list; port install → MG_Impl::Init; test relocation to MG_Test/Record/; IsLivePersistentMap probe path; new include-closure probe (--expect-probes 5); ph_latch_sites FILES and PeerLatchTest rows. Invariant: push byte-identical to S1. Tests: split tree full as S3 + link_ratchet.py (baseline unchanged or named rebaseline), check_include_closure.py --mode both --require-all --expect-probes 5, fatal_census --write-baseline diff reviewed, ph_latch_sites.py; nm of split lib: MG_Record > 0.
- S5: Config.h / ConfigLoader.cpp predicates and InitDataArm / InitRecordArm keyed on RECORD_ARM (§4). Push byte-identical. Tests: unit; split monolith integration-gpu (MOBILEGL_PIPE_DATA_ARM=frontend and default).
- S6: integration harness — RecordArmPeek; the six scenarios read it; integration CMake `if (MOBILEGL_BUILD_DISAGGREGATED)` blocks registering monolith record-arm cases reclassified to RECORD_ARM. Push unchanged (RECORD_ARM = 0). Tests: split integration-gpu; split_coverage.py / spawn_lane_parity.py unchanged.
- S7 flip: set(MOBILEGL_BUILD_RECORD_ARM ON) unconditionally; fix compile errors (= the misclassifications); runtime_mode_proof positive half (D6); skip-census baseline rewrite (shrinking). Push now RUNS the record arm. Tests: push tree full unit; integration-gpu monolith both arms (default and MOBILEGL_PIPE_DATA_ARM=frontend); retrace monolith 230/236; retrace_pull_library_control.sh vs push lib (same red sentence); runtime_mode_proof build --mode monolith (MG_Remote = 0, MG_Record > 0); nm/.text measurement. Verify tree: unit + integration-verify. Split tree: full. CI: Magma integration, build-linux-monolith-control job.
- S8: unifdef -DMOBILEGL_BUILD_RECORD_ARM=1 over the tree (also deletes !RECORD_ARM arms: g_programResourceCaches, Magma hidden GL programs, VERIFY pins). Drop CMake variable; fold record sources into SOURCE_FILES; test CMake unconditional; retired_switches.py rule; wire_declines_audit back to one guard. Push .text identical to S7. Tests: all S7 + retired_switches.py, ratchet, include closure.
- S9 (optional, can slip to W7): Release --print-gc-sections audit of record-core dead code; move MG_Remote/Client/SlotCaps.h → MG_Impl/GLImpl/; depfile check that no file under MobileGL/MG_Remote/ appears in the push tree's deps (zero MG_Remote at include level, not just symbol level); record size and RSS deltas in README. Test: the new check's self-test (inject one include → red).
Not in W5: deleting monolith frontend arms or Transport K branches (W6); OBJECT library targets / server-only link target (W7).

## 7. Open points for the integrator
1. Static registration of record-fail hooks in FatalFunnel.cpp vs role-init registration (§2.1).
2. RecordArmTable vs making IpcTable unconditional (lower churn but leaks transport knobs into FCL).
3. Whether the mprotect-arm kill switch for FCL bring-up (risk 6) belongs in W5 S7.

Critical files: MG_Remote/Client/EmitTables.cpp, MG_Remote/Server/PipeApplier.cpp, Config.h, CMakeLists.txt, MG_Pipe/PipeSessionFail.h (all under the worktree's MobileGL/).

## 8. Integrator decisions (2026-10-06)
1. Static registration in `FatalFunnel.cpp`. The hook globals are constant-initialized to the monolith defaults and replaced at namespace-scope init, so every DISAGG process (unit cases with no role init included) counts faults exactly as today.
2. `RecordArmTable`. `IpcTable` stays DISAGG-only; no transport knob reaches the FCL library.
3. Yes: S7 adds an env switch that forces the tracker's hash arm instead of the mprotect / SIGSEGV arm, logged at startup, with a unit case proving the knob reaches the tracker. The FCL process is a JVM, so the chained SIGSEGV handler is a real bring-up risk.
