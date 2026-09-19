# BRIEF-P5E — the package plan for retiring the lockstep on the Espryt draw path

Contract: `CONTRACT-P5E-draft.md` (same directory; becomes `MG_Remote/CONTRACT-P5E.md` at c0e).
Base `feat/disaggregated @ 2fde7034`. Paths under `MobileGL/`. Eight packages; seven of them can be
implemented in parallel worktrees once c0e and id have landed. Every line-range below is at the
base head and names the ONLY region the package may edit in a shared file; a package that needs a
line outside its range asks the integrator, who either widens the range or lands the edit in c0e.

## 0 The shape of the landing, in one paragraph

Nothing on Minecraft's steady draw path can go fire-and-forget until EVERY per-draw family reads
records instead of the frontend: `PrepareForDraw` touches the VAO (vi), the program (pg), the unit
textures and samplers (tx2), the bound FBO and its attachment lists (fb) and the UBO points (sb) on
every draw, hit or miss. So the smallest first landing that lets the client stop waiting at draws
is c0e + id + {vi, sb, pg, tx2, fb} + ra, with the caps bit flipped in ONE integration commit after
all of them. What makes it small is what is left barriered and trailing (§5): XFB, client vertex
arrays, CopyTex / GetTexImage / readbacks, `set_storage_block_binding`, the texture half of
`resource_subdata`, `set_texture_params`' reply — none of which is on the steady path. The
contract's barriered predicate (§2.1) is what lets those keep P5C's semantics while draws run
ahead, and the unconditional detector (§3.3) is what makes a missed site a named abort instead of
a wrong picture.

## 1 Dependency order

```
c0e  (the contract, the wire rows, the knobs; inert)          -- lands FIRST, no behaviour change
 └─ id  (registry rekey, by-handle resolvers, the guard)      -- lands SECOND, lockstep unchanged
     ├─ vi   (VAO / VBO / IBO)                       ┐
     ├─ sb   (set_shader_buffers, binding points)    │  parallel worktrees, any order,
     ├─ pg   (program twin, archive, bindings)       │  each rebased on id
     ├─ tx2  (textures, samplers, unit windows)      │
     └─ fb   (FBO, attachments, images, blit)        ┘
 └─ ra  (the wait rule, credit, gPipeInputs, flow control)  -- parallel from day one on c0e;
                                                                its LAST commit is the integration
                                                                commit: kMGPipeP5eRunAheadReady=true
```
Cross-package couplings that must be known before branching: pg's `BlockBindings` tail is what sb's
UBO loop indexes with (`DirectGLES.cpp:5463` vs `:5464`); tx2's `SyncMipmapsToBackendByHandle` /
`SyncTextureToBackendByHandle` are what fb's attachment sync and image sync call; fb's reverse
index is what tx2's `ScopedDetachedTextureFramebufferAttachments` needs; sb's deletion of the
backend GPU-write marks and fb's deletion of `MarkWritableImageBufferTexturesGpuWritten` are one
edit split across two files. Each such seam is a declared signature in c0e (a header declaration
with a body that asserts "not landed") so both sides compile from day one.

## 2 Packages

### c0e — the contract and the wire (integrator; lands first)
**Scope.** `MG_Remote/CONTRACT-P5E.md`; `MG_Pipe/PipeCalls.def` (the `WaitClass` column on every row,
`SetProgramBindings` opcode 80 appended); `scripts/gen_pipe_wire.py` (or whichever generator emits
`PipeWire.inc`) + regenerated `MG_Pipe/generated/*.inc` (`MGPipeWaitClassFor`, the new op's thunk /
table slot / codec layout, route row null); `MG_Pipe/MGPipeTypes.h` (`kCapRunAheadApply`,
`MGPProgramBindings` + tail PODs, `MGPProgramDesc::LinkStatus`, `kDrawClientArrays`,
`kMGPipeMaxBufferBindingPoints`); `MG_Pipe/MGPipeValueTypes.h` (`MGPipeImageAccess` enum, encode/
decode); `MG_Pipe/MGPipe.h` (bit 13, default mask `0x3fff`); `MG_Impl/Pipe/SetHashSuppressor.h`
(four slots); `MG_Impl/Pipe/Tracker.h:97-99, 204-209` (bits 15-17 → subsystem 13); `Config.h`
`IpcTable::RunAhead/PresentCredit`; `ConfigLoader.cpp:356-403`; `MG_Pipe/PipeApply.h` declarations
of every cross-package seam (`MGPipeApplierCurrentRecordIsBarriered()`, `MGPipeBarriered(...)`, the
sb/pg applier fields, the by-handle sync signatures) with asserting bodies; `MG_Pipe/FieldOwnership.def`
re-annotations; `MG_Pipe/Coverage.def` rows; `MG_Test/Pipe/PipeCatalogueTest.cpp` (sizes, opcode
80, the null route pins); `MG_Test/Pipe/TrackerTest.cpp:385-387` (bit 13); `docs/Disaggregated/
ROADMAP.md` row; `MG_Backend/Init.cpp` (`kMGPipeP5eRunAheadReady = false`, the DirectGLES arm that
will publish bit 10). **Red-once.** The ABI fingerprint (`SessionRings.h:498-524`) changes with the
new opcode and `PipeCatalogueTest` pins the new sizes; `MagmaPipeIdentityTest` pins no bit 10 on
DirectVulkan. **Quick gate.** Unit green; `integration-split` green (nothing behaves differently);
G1 0/0/0/0.

### id — identity (S7's spine; lands second)
**Scope.** `MG_Backend/DirectGLES/SlotTables.h` (composite band `m_band`; `ForEachLive` re-typed as
`fn(MGPipeHandle, const BackendPtr&)`; the frontend-keyed members kept as monolith glue, each
asserting `!MGPipeApplierIsUnbarrieredApply()` in split builds); `Managers.h:309-640` (registry
forwarding, `ResolveVaoTwin(handle)` / `ResolveProgramTwin(handle)` / `ResolveTextureTwin(handle)`
declarations in the `ResolveSamplerCsoTwin` shape, `AdoptTwinByHandle` as the body);
`Managers.cpp:188-372` (teardown, `OnFrontendStateObjectDestroyed` — delete the NoSession hop
`:224-246` and the scope at `:256`; `ReleaseTwinsForWireObjectDeath`); `MG_Impl/Pipe/SlotAllocator.{h,cpp}`
(the guard: exemption iff scope AND barriered record; `MGPipeFrontendKeyedRegistryScope` kept as a
class until the family packages delete their sites; `MGPipeReverseAnnouncementScope` →
`MagmaP7AllocatorDebtScope`, backend-kind-keyed); `MG_Backend/DirectVulkan/VulkanRenderer.cpp:4554,
4634, 8986` + `MG_Impl/Pipe/ResourceTracker.h:723` (the rename only); `MG_Remote/Server/PipeApplier.cpp`
`ApplyOne` sets `CurrentRecordBarriered` from `MGPipeBarriered` (the one line ra also touches —
id owns it, ra rebases); `MG_Test/SanityTest.cpp:3499-3960, 4061, 4241, 4378, 4898` (cases moved to
the legacy arm or rewritten by handle; new `ACompositeHandleDoesNotGrowTheOrdinaryTable`);
`MG_Test/Wire/RemoteClientTest.cpp:1866-1913` (`...IsAllowed` deleted; new
`...FromTheApplyThreadIsFatalEvenAtTheOldDebtSites` posting `HandleOf` through `RunOnApplyThread`
with the barriered flag forced false by a test hook). **Red-once.** With the hook forcing
"unbarriered", `HandleOf` from the apply thread aborts `Fatal{RoleViolation, "MGPipeSlots"}`; put
the exemption back → the case is green → red by expectation. **Quick gate.** Unit + `integration-split`
green (lockstep behaviour unchanged: every record is barriered until ra). **New names (G2/G14):**
`ResolveVaoTwin/ResolveProgramTwin/ResolveTextureTwin(MGPipeHandle)`, `MagmaP7AllocatorDebtScope`,
`MGPipeApplierCurrentRecordIsBarriered` — all `MOBILEGL_BUILD_DISAGGREGATED`.

### vi — VAO / vertex input / index (S1)
**Scope.** `DirectGLES.cpp:699-925` (`SyncVaoAttributeBuffersByHandle`, the IBO arm), `:1602-1719`
(`ResolveVaoTwin` handle arm, `SyncCurrentVAO`, `SyncCurrentVertexAttributeValues`), `:4770-4776`
(the VAO lines of `PrepareForDraw` ONLY), `:6447-6480` (restart helpers), `:6718-6782`
(`DrawArrays`/`MultiDrawArrays` arms: monolith-only); `Managers.h:1204-1300` (`ResolvedDrawBuffers`:
`frontend` leaves the split arm, `iboSerial` added), `:1286-1290`; `Managers.cpp:4881-4892`, `:5160-5350`
(`SyncToBackendFromApplier` — signature only), `:5364-5468` (`SyncClientSideAttributesForDrawArrays`:
monolith-only); `MG_Remote/Client/EmitTables.cpp:484-525, 640-690` (`kDrawClientArrays` + the
run-ahead refusal beside `CLIENT_INDICES`); `MG_Test/Pipe/VertexInputEmitTest.cpp` + a
`SanityTest.cpp` sibling beside `:4083`. Trailing inside vi: `ResolvedDrawBuffers` deletion after a
measurement. **Red-once.** (1) Revert `ResolveVaoTwin` to `Find(vao.get())` → `Fatal{RoleViolation,
"MGPipeSlots"}` on the first `integration-split` draw once ra flips the bit; before that, the id
test hook. (2) Publish `set_vertex_buffers` for A,B, repoint the frontend VAO at C without
re-emitting, sync → A,B ensured; the revert ensures C. (3) Same on `set_index_buffer`. (4)
`glDrawArrays` with a client array under run-ahead → the named refusal; revert → red under
`integration-split-strict` (`DoublePrecisionScenario` if it uses client arrays, else a new scenario).
**Quick gate.** Unit + `integration-split` green; `VertexAttribBindingScenario`,
`VertexArrayEnableDisableScenario`, `PrimitiveRestartScenario`, `ResidentIndexScenario`,
`MultiDrawScenario`, `DrawParametersScenario`, `HandleRecycleScenario`, `ObjectSubsystemControlScenario`
(the `0x0ff`/`0x17f` A/B keeps its verdicts). **New names:** `ResolveVaoTwinByHandle` (or the id
resolver), `SyncVaoAttributeBuffersByHandle`'s new signature.

### sb — buffer binding points (S5)
**Scope.** `MG_State/GLState/BufferState/BufferState.h:69-75` (bind-point generations, push-only);
`MG_Impl/GLImpl/Buffer/GL_Buffer.cpp:1501-1520, 1624-1650` + `SetNamedTransformFeedbackBinding` (the
bumps); `MG_Impl/Pipe/Tracker.h:520-560, 630-640` (bits 15/16/17 shutters); new
`MG_Impl/Pipe/ShaderBufferEmit.h`; `MG_Impl/Pipe/PipeFill.cpp` (`SubsystemForEmitter` `:1989-2028`
row + pairing asserts `:2104-2126`, the validate order `:3225-3233`, the reset list `:3129-3143`,
the `kMGPipeMaxBufferBindingPoints`/84 pin); `MG_Pipe/PipeApply.{h,cpp}` (`MGPipeApplySetShaderBuffers`,
the applier fields declared by c0e); `MG_Pipe/PipeRoute.{h,cpp}` + `MG_Remote/Client/WireTables.cpp`
(route rows for op 38); `MG_Remote/Server/PipeApplier.cpp` (the one sink method `OnSetShaderBuffers`);
`DirectGLES.cpp:477-673` (`SyncBufferBindingPointsByRecord`, `MarkShaderStorageBuffersGpuWritten`
deleted under transport, `SyncAtomicCounterBuffers`), `:979-990` (call lines), `:5464-5497` (the
point half of the UBO loop; `:5463` is pg's); `scripts/gen_pipe_dirty_surface.py:87-90` + `DirtySurface.def`
rows; `MG_Test/Pipe/TrackerTest.cpp`, new `ShaderBufferEmitTest.cpp`, `PipeCatalogueTest.cpp:170-171,
242-243` (invert); `MG_IntegrationTest` A/B with bit 13 cleared. **Red-once.** (a) Revert the handle
walk under a live wire → `Fatal{UnmigratedPipeInput, "GetBufferBindingPoint@DrawVbo"}` on
`SsboArrayDynamicIndexScenario` / `StorageBufferRegrowScenario` / `AtomicCounterScenario` under
strict. (b) `TrackerTest`: `glBindBufferBase(UNIFORM,1,A); Update(); glBindBufferBase(UNIFORM,1,B);
Update()` sets bit 15 on the second update — red against `Tracker.h:636` today. (c)
`GetBufferBindingPointCount`'s `rsp` non-zero before, 0 after, on `AtomicCounterScenario`. (d)
Delete the backend marks and `ProducerMarkCount(ShaderStorageBinding)` stays non-zero while
`SsboArrayLengthScenario`'s readback sees the writes. **Quick gate.** Unit + `integration-split` +
strict on the four SSBO/atomic scenarios and the three XFB ones; the bit-13-cleared A/B reproduces
today's picture. **New names:** `MGPipeApplySetShaderBuffers`, `MGPipeShaderBufferEmitterInstance`,
`SyncBufferBindingPointsByRecord` (file-local), `BufferState::NoteBindPointChanged`.

### pg — programs (S4)
**Scope.** `MG_Impl/Pipe/ProgramEmit.h` (archive encode per link via `ProgramArtifactsCodec` into
SEG_STAGE; `EmitProgramBindings`; `LinkStatus`; the failed-relink rule); `MG_State/GLState/ProgramState/
ProgramArtifactsCodec.{h,cpp}` (completeness: name `:242`'s skipped member); `MG_Pipe/PipeApply.{h,cpp}`
(`MGPipeApplyCreateShaderState` adopts the blobs into record-owned artefacts — the companion
pointers go under a transport; `MGPipeApplySetProgramBindings`); `MG_Pipe/PipeRoute.*` +
`WireTables.cpp` (op 80 route); `PipeApplier.cpp` (`OnSetProgramBindings`; trailing:
`set_storage_block_binding` by `ShaderCso`); `DirectGLES.cpp:4097-4110` (the stash), `:4230-4318`
(`ResolveGlobalConstantsRecord(handle)`, `MapUBO` refusal), `:4320-4510` (`SyncCurrentProgram(handle)`
incl. the broadcast decline arm `:4383-4404` → Fatal under transport), `:4781` (`PrepareForDraw`'s
program line), `:5317-5560` except `:5464-5497` (sb) and `:5561-5632` (tx2), `:5670-5740`
(`GetCurrentBackendProgram`), `:6120-6130` (`PrepareForCompute`'s program line), `:10018-10042`
(`ShaderStorageBlockBinding`: monolith body kept; trailing handle arm); `Managers.h:2338-2541`;
`Managers.cpp:10757-13080` (`SyncToBackend(h, record)`, `CacheResourceLocations`,
`ReseedShaderStorageBlockBindings`, the signature); `MG_Test/Pipe/ProgramEmitTest.cpp` (beside `:251`).
**Red-once.** (1) Empty the applier's block-binding tail → `UniformInitializerScenario` /
`Glsl420DeclarationScenario` wrong pixels + `RefusedObjectCalls`. (2) Leave the blob refs at `Size 0`
with the companions removed → `Fatal{ProtocolCorruption}` (`PipeApply.cpp:2858-2871`). (3) Delete
the five scopes without re-keying → `Fatal{RoleViolation, "MGPipeSlots"}` (after ra). (4) Strict lane
names `GetProgramForDraw@draw_vbo` with the row still pulled; green after. (5) Force bit 12 off
under run-ahead → `Fatal{UnmigratedVerb, "set_global_constants"}`. **Quick gate.** Unit +
`integration-split`; `RelinkStageSetScenario`, `PostLinkAttachScenario`, `ProgramPipelineScenario`,
`AsyncCompileScenario`, `CsoContentAddressingScenario`, `HandleRecycleScenario`; the archive cost
measured (links/frame, bytes/link, ms on the GL thread) on MC + a shader pack and reported before
the package is declared done. **New names:** `MGPipeApplySetProgramBindings`, `EmitProgramBindings`,
`SyncCurrentProgramByHandle`.

### tx2 — textures and samplers (S2)
**Scope.** `DirectGLES.cpp:1723-1806` (`SyncTextureToBackendByHandle`), `:1867-2098` (unit
epoch/capture: monolith-only; decline → refusal), `:2127-2140` (`UnitTextureSyncEntry` re-shape,
push-only under `#if`), `:2321-2391` (the unit half), `:4834-4948` (`ResolveAndBindUnitTextures`),
`:4985-5026`, `:5051-5170` (`BindCurrentUnitSamplers`, decline → refusal), `:5245-5305`
(`BindCurrentTextures` with ruling 6's key), `:5561-5632` (the sampler pass; the mint deleted),
`:8725-8760` + `:9560-9600` (`GenerateMipmap` by `VerbMipRes`), `:9161, 9216` (mip shape `HandleOf`
→ handle), trailing `:9714-9735` (CopyTex endpoint) and `:11884` (`GetTexImage`); `Managers.cpp:5816-5975`
(`RequireImageBindableStorage` entry refusal), `:6902-6985`, `:7049-7400` (`SyncMipmapsToBackend(h,
rec)`), `:8472-8560`, `:8712-8900`, `:13184-13290` (sampler identity family deleted under
transport), `:13457-13486`; `Managers.h:1626-1660` (`IsDrawSyncCleanByRecord`), `:1785-1812`,
`:2678-2700`; `MG_Backend/DirectGLES/MipmapStorage.cpp:39-51` (guard list widened);
`PipeApplier.cpp` (the window checks at draw/dispatch dispatch — ONE function `CheckUnitWindows(st)`
called from `OnDrawVbo`/`OnLaunchGrid`); `MG_Test/Wire/RemoteClientTest.cpp:1938-1978`;
`MG_Test/SanityTest.cpp:3075-3144` (+ the ABA sibling); `SampledSetStalenessScenario`,
`TextureParamsWithoutASamplerViewScenario` kept. **Red-once.** (1) Revert any handle arm → the
widened guard aborts by accessor name. (2) Revert the window arm of `ResolveAndBindUnitTextures` →
strict lane `Fatal{UnmigratedPipeInput, "GetTextureUnitObject@DrawVbo"}`. (3) ABA: recycle a slot to
`{s, g+1}` while the old entry is in the work list → the twin is re-minted; skip the Gen reset → red.
(4) Narrow `SamplerEmit.h:735-737`'s count by one → `Fatal{ProtocolCorruption, "SetSamplerViews.Count"}`.
(5) `GenerateMipmap` with the row `kWaitNone` and the unit walk restored → `Fatal{UnmigratedPipeInput,
"GetActiveTextureUnit@...}` (`F1WireScenario.GenerateMipmap*Pixels`). **Quick gate.** Unit +
`integration-split`; `PipeSlotPeek` asserts zero registry-scope entries per frame for this family.
**New names:** `SyncTextureToBackendByHandle`, `SyncMipmapsToBackendByHandle`,
`ResolvePushedTextureParams/BuiltinSampler(MGPipeHandle)`, `IsDrawSyncCleanByRecord`.

### fb — framebuffers, attachments, images, blit (S3)
**Scope.** `DirectGLES.cpp:179-241` (the doors), `:2140-2180` (list keys), `:2227-2331`
(`SyncReadFramebufferTextureAttachments`), `:2405-2470` (the draw-FBO list), `:2519-2914` (images:
`SyncImageTextureBinding(const MGPImageView&)`, `ResolveShaderImageRecord` deleted,
`MarkWritableImageBufferTexturesGpuWritten` deleted under transport, the sweep gate), `:3017-3377`
(`SyncCurrentFBO*`), `:4514-4754` (`BindCurrentFBO`, `SyncAndBind*`, `ForceBindCurrentFBO`),
`:6180-6192` (`Clear`), `:8182-8420` (both blit arms), `:8837-8935` (the detach walk + reverse index),
`:9688-9740` (`SyncRenderbufferObjectToBackend` by handle); `Managers.cpp:9395-9405, 9585-9742,
9834-9953, 10044-10429, 10847-10850, 13530-13584`; `Managers.h:1857, 1943-1970, 2819-2830`
(reverse index texture → FBO slots); `MG_Test/Pipe/FramebufferEmitTest.cpp` (server twin of `:558`),
`MG_Test/Pipe/ImageEmitTest.cpp` (beside `:452`: the three constants on both sides), a new
default-framebuffer-resize re-emit case. **Red-once.** (1) `SyncAttachmentSurface` takes the
frontend attachment again → strict `Fatal{UnmigratedPipeInput, "GetFramebufferBindingSlot"}` on
`SnormAttachmentScenario`. (2) The draw-FBO list key back to the pointer compare → `Fatal{RoleViolation,
"MGPipeSlots"}` (after ra). (3) `MGPipeDecodeImageAccess` mapped 1↔2 → `FormatlessImageBakeScenario` /
`ImageLoadStoreSsoScenario` red + the unit case. (4) The named blit back on the frontend objects →
`CopyImageLayeredScenario`, `LayeredAttachmentShapeScenario`. (5) The per-point version memo kept as
a second gate → `CrossFrameBufferScenario` / `P4aSeamAuditScenario` F-3 must stay green WITHOUT it.
**Quick gate.** Unit + `integration-split` with the framebuffer/sampler bits set; the seam greps
(`DirectGLES.cpp:3086-3106, 2607-2610, 2636-2638`) silent; the object A/B re-baselined for
`SyncReadFramebufferTextureAttachments`. **New names:** `SyncToBackendByHandle` (RBO/FBO overloads),
`MarkBufferGpuWrittenByHandle` (declared; body deleted under transport with sb), the reverse index.

### ra — the transport mechanics (S6; parallel from day one; lands last)
**Scope.** `MG_Remote/Client/ClientSession.{h,cpp}` (`RunAheadArmed`, `EmitAndWaitTails` §2.3,
`ReleaseFillPins`, `DrainEventRing` at every park exit, `Finish`, `RefusePipeInputsTouch...` §3.5,
the stats line `credit-waits`); `MG_Remote/Client/EmitTables.cpp:977-1053` (`EmitPresent` credit +
`FrameSerial`) and the `Finish` slot; `MG_Remote/Client/BackendObject_Remote.cpp` (wait before every
`Server*` EGL forwarder); `MG_Remote/Client/WireTables.cpp:332-352` + `MG_Pipe/PipeRoute.h:270-276`
(`wantReply` for the buffer half); `MG_Remote/Client/CapsMirror.*` (latch); `MG_Impl/Pipe/PipeFill.cpp:84-86,
546-556, 1955+, 2962-3306` (§3.1, §3.4); `MG_Backend/MGPipe/PipeInputs.{h,cpp}` (server-private
stamp, §3.2-3.3); `MG_Remote/Server/PipeApplier.cpp` (`OnPresent` credit return, `StampVerbBoundary`,
`MGPipeBarriered` body; `ApplyOne`'s flag line is id's); `MG_Remote/Server/ServerSession.{h,cpp}`
(present credit, event producer flow control); `MG_Remote/Server/ServerLoop.cpp:375-392` (park
predicate); `MG_Remote/Transport/EventRing.h`, `SessionRings.h`; `MG_Backend/Init.cpp` (flip
`kMGPipeP5eRunAheadReady` — the integration commit); `MG_Impl/Pipe/PipeMutation.h:216-236` (the
unbarriered-enqueue finding); `.github/workflows/test.yml:1140-1206` (the strict lane's allowlist
step); `docs/Disaggregated/ARCHITECTURE.md` §11, §14, §17; `MG_Test/Wire/{RemoteClientTest,
SessionTest, ServerLoopTest}.cpp`. **Red-once.** (1) Encode present without `WaitForPresentAck` →
the `SessionTest.cpp:522-549` twin in `RemoteClientTest` parks a credit-1 client behind a stalled
apply thread; without the wait the third present publishes and `Waits()` is unchanged → red by
count. (2) Leave one `CopyField` in an unbarriered fill → `Fatal{RoleViolation, "gPipeInputs"}` by
name (`RemoteGuards...FillWhileTheApplierOwnsIt`'s shape with `RunAhead=1`). (3) Count instead of
Fatal under an unbarriered record → the strict lane's rewritten step turns from green to red on the
first pulled row. (4) Keep `Fatal{EventRingOverflow}` → a unit case publishing 300 KiB of
`kEventGpuWritten` behind an unwaited sequence aborts; with flow control it completes. (5) Publish
bit 10 for Magma → `MagmaPipeIdentityTest` red. (6) `MOBILEGL_IPC_RUN_AHEAD=0` renders identically
(SSIM); `MOBILEGL_IPC_VERB_BARRIER=0` under run-ahead is the documented red. **Quick gate (before the
flip).** Unit + `integration-split` green with the bit unpublished (every branch inert);
`RemoteGuards` strict arms green. **New names:** `MGPipeBarriered`, `MGPipeWaitClassFor` (c0e),
`ClientSession::RunAheadArmed/ReleaseFillPins`, `MGPipeServerStampVerbBoundary`'s new signature,
`IpcTable::RunAhead/PresentCredit`.

## 3 The integration gate (the commit that flips `kMGPipeP5eRunAheadReady`)

1. Unit green under both `MOBILEGL_IPC_STRICT_ERRORS` arms.
2. `integration-split` green (111+ entries plus the new ones), broad inproc census with zero
   regressions vs `2fde7034`, the 79 traces with no new first blocker.
3. `integration-split-strict` GREEN as a hard lane: the only admitted `[BARRIER-PULLED, ...]` markers
   are the §7 allowlist (`<field>@<verb>` for readbacks, XFB, CopyTex, `set_storage_block_binding`);
   any other marker fails the lane. Every scenario summary shows `rsp` = 0 on unbarriered records.
4. The three negative controls behave as documented: `RUN_AHEAD=0` identical SSIM; `VERB_BARRIER=0`
   red at a barriered row; `RUN_AHEAD=1` on Magma logs once and runs lockstep.
5. Every package's red-once re-run on the integrated tree (a revert of any one handle arm goes red
   in the strict lane by field and verb).
6. G1 0/0/0/0, G2, G14, G5; the push build under `Transport=monolith` renders the four A/B traces
   with unchanged SSIM.
7. `MOBILEGL_IPC_AUDIT=1` clean (no `0xDD` read) with the client a present ahead — the pmap push
   without a reply is the new consumer of retired staging.

## 4 The device exit (Redmi, MC 26.3 VD12, 30 s in-world window)

Arms: monolith; inproc lockstep (`MOBILEGL_IPC_RUN_AHEAD=0`); run-ahead credit 1; run-ahead credit 2.
Per arm: fps; per-thread CPU ms/frame (client GL thread, `mgl-srv-apply`); `cliwait/clipark`,
`credit-waits`, `srv/srvpark` from the wire ledger and the stats line (`ClientSession.cpp:1161-1189`,
`ServerLoop.cpp:410-414`); `simpleperf` on both threads — the exit condition is that
`WaitForApplied` self on the client falls from 27.9% to the present-credit wait alone and
`DrainRing`'s real work is unchanged; the per-op wait tally must show NO `kWaitApplied` row and no
reply row firing per draw on the steady path (what remains per frame — the texture subdata for the
lightmap, `set_texture_params` replies, fences — is listed by op in the report as the trailing
inventory); `rsp` = 0 on unbarriered records; SSIM of the four A/B traces vs monolith unchanged.
The P5D report's method section (`P5D-INPROC-PERFORMANCE.md:58-71`) is the template; the bench
scripts the brief named are not on this machine and are re-created beside it. Recorded, and
GATED on: fps ≥ lockstep's, client-thread ms/frame < lockstep's, SSIM unchanged.

## 5 What trails (after the integration commit, each its own small package)

| item | owner | why it can trail |
|---|---|---|
| `GenerateMipmap` `kWaitApplied` → `kWaitNone` (if tx2 lands the `VerbMipRes` arm late) | tx2 | not on the steady path |
| `set_storage_block_binding` by `ShaderCso` | pg | after-link only |
| CopyTex endpoint / `GetTexImage` / readbacks by handle (the barriered scope sites `6166, 8837, 9161, 9216, 10029, 11884`) | tx2 / fb | barriered rows keep P5C's semantics (P8/P9 phases) |
| `resource_subdata` texture half without a reply (D-D5 accept-by-construction) | ra + tx2 | one wait per frame for the lightmap today |
| `set_texture_params` without a reply (the default-texture self-heal moved client-side) | tx2 | creation-time traffic |
| client vertex arrays staged (`{BindingIndex, MGHostSpan}` tail) | P8 | refused under run-ahead |
| XFB migration (`set_stream_output_targets`, the program row, the targets) | P3b/P4b/P9 | lockstep by escalation (i) |
| `ResolvedDrawBuffers` deletion | vi | measurement first |
| composite pipelines' uniform mirror vs `set_program_bindings` fire rate | pg | unmeasured; pipeline workloads only |
| program-derived `set_shader_buffers` window | sb | narrowing, not correctness |
| `MOBILEGL_IPC_EVENT_KB` (ring size under SSBO-heavy `kEventGpuWritten`) | ra | Iris-class workloads |
| `Flush` as the doorbell point (ARCHITECTURE §11.6); credit-2 latency study | ra | measurement |
| thread placement | — | out of scope by instruction; the per-thread numbers survive it |

## 6 The rulings the integrator makes (conflicts between scout designs, with the resolution)

| # | conflict | resolution (contract §9) |
|---|---|---|
| 1 | S7: handle arms run under `Transport=monolith` too (family bit selects); S1–S5: `Transport != Monolith` keeps the frontend arms under monolith | S1–S5's shape. The push-monolith build keeps its bytes and behaviour, the verify comparator keeps its frontend arm, and `RUN_AHEAD=0` becomes a pure wait-rule A/B on identical server code. |
| 2 | Brief/S7 assumed client vertex arrays are refused under split; S1 read the code: they are not (`EmitTables.cpp:651-685`) | Verified S1 is right. Refuse under `RunAheadArmed` with `kDrawClientArrays`; lockstep unchanged; staging is P8's. |
| 3 | S5: XFB/atomic waits decided from client state at emit; S6: a static per-op wait table | The wire-derived predicate `MGPipeBarriered(op, payload, st)` — static column + XFB-active from `MGPContextValues` + the draw flag — computed identically on both sides. |
| 4 | S6 §4(6): a WAITED op may no longer read the frontend; S1–S5 keep readback/CopyTex/GetTexImage sites barriered | S6's argument is wrong for a single-context client parked in its own wait; barriered records keep P5C's semantics (rule F is scoped to unbarriered records). This is what keeps the first landing small. |
| 5 | S4 §4.6: `ShaderStorageBlockBinding` is a GL-thread entry; S6: `set_storage_block_binding` is a `kCtxVerb` applied on the apply thread through `GetProgramObject(GlName)` | Verified S6 (`PipeCalls.def:243`, `PipeApplier.cpp:805-808`). `kWaitApplied` now; pg's trailing item resolves by `ShaderCso`. |
| 6 | S2: `BindCurrentTextures` memo on `SamplerViewsSerial` alone; S4: on the program handle + serials | Both (a relink with unchanged resolution still needs the twin's new unit assignments). |
| 7 | S3 R1: the image sweep gate needs S2's serial design | `(ShaderImagesSerial, TextureShutterSerial, ContextSerial, g_backendContextGeneration)`; keep the union walk. |
| 8 | S4 R1: encode the archive per link vs pin a `SharedPtr` | Encode (rule B); measure; lazy encode at first bind as the fallback. |
| 9 | S4 R2: a failed relink of a bound program | Re-issue with `LinkStatus = 0` iff the frontend reports it unlinked; pg verifies the frontend's actual behaviour; never `object_death`. |
| 10 | S5 R2: high-water vs program-derived window | High-water (`TouchedBufferBindingPointCount`, already carried); 84 on the wire, backend clamps. |
| 11 | S5 R1: one suppressor slot for three classes | Three slots. |
| 12 | S7 R1: Magma's four apply-thread allocator sites vs an unconditional guard | One renamed scope, backend-kind-keyed; Magma is lockstep and every record is barriered there. |
| 13 | S6 §3(9)/S7: the deferred-destroy queue "loses its producer" | It does not (barriered fills pin objects); the queue stays live; an unbarriered enqueue is the finding. |
| 14 | S6 §3(3)/S2: `GenerateMipmap` waits until the unit read is gone | Verified (`ClientSession.cpp:900-914`; `VerbMipRes` already exists at `DirectGLES.cpp:8736`). tx2 flips the row. |
| 15 | S3 R5: does an EGL resize re-emit the default-framebuffer record | Verified yes (`ClientSession.cpp:210-221` → `TextureObject.cpp:84-102` → `Tracker.h:594`); fb pins it. |
| 16 | S3 vs the brief: `Access` "has no encoding" | Verified S3: `MGPipeEncodeImageAccess` exists (`ImageEmit.h:163-176`); the server comment is stale; the decode moves to `MGPipeValueTypes.h`. |
| 17 | S6 §7: `resource_subdata`'s texture half | Buffer half `kWaitNone` in ra; texture half keeps its reply; trailing. |
| 18 | present credit default | 1. |
| 19 | S1 R4 / S2 R1 (A8): the var-tail windows are not pinned | The window rule in §5.3 with the sink check at every draw/dispatch; `set_vertex_buffers`' window is pinned by a test in vi (same class, same validate step). |

Open items that need a ruling the scouts could not settle and this brief does not: (a) whether
`DoublePrecisionScenario` uses client arrays under split (decides whether vi's refusal needs a new
scenario as its red-once); (b) the archive cost number (ruling 8's fallback trigger); (c) the NoSession
server-only fixtures that lose death delivery when id deletes the hop (`grep RunOnApplyThread
MG_Test/Wire`) — id enumerates them in its first commit and the integrator rules keep/rewrite.
