# S7 - identity: the five twin registries and the allocator (head `2fde7034`)

Paths under `MobileGL/`. `DG` = `MG_Backend/DirectGLES/DirectGLES.cpp`, `MG` = `MG_Backend/DirectGLES/Managers.cpp`,
`MH` = `Managers.h`, `ST` = `MG_Backend/DirectGLES/SlotTables.h`, `SA` = `MG_Impl/Pipe/SlotAllocator.{h,cpp}`,
`PF` = `MG_Impl/Pipe/PipeFill.cpp`. Kinds: **id** = `GetLifetimeId()` + `MGPipeSlots().FindByLifetimeId`
(`ST:436-449`, through `MGPipeRefuseAllocatorFromApplyThread` `SA.cpp:257-273`); **mint** = `GetOrCreate(const StatePtr&)`
-> `MGPipeSlots().Acquire` (`ST:215-268`), a WRITE into the client allocator; **live** = a dereference of the frontend
object beyond its lifetime id; **rec** = a handle-keyed arm that already exists beside the site.

## 1. The exact live reads of the family (per kind: today's key, every apply-thread site, hit/miss)

**The mechanism common to all five** (`MH:309-620` `StateBackendObjectRegistry` wrapping `BackendSlotTable`):
`Find(StateObject*)` `ST:411-414` = `HandleOf` (id) + `FindByHandle`; `GetOrCreate(StatePtr)` `ST:215-268` = `Acquire`
(mint) + `RememberHandle` + `entry.stateRef = stateObj` (a frontend `SharedPtr` OWNED BY THE SERVER TABLE, `ST:162,260`);
the per-table memo `m_memoLifetimeId/m_memoHandle` (`ST:649-650`, `mutable`, written from the apply thread on every id);
`OnFrontendObjectDestroyed` `ST:465-485` = id + `MGPipeSlots().Free` (monolith / NoSession fixture only). Every id and
mint on the apply thread is legal today only through `MGPipeFrontendKeyedRegistryScope` / `MGPipeReverseAnnouncementScope`
(`SA.cpp:263-264`); the two depth counters `SA.cpp:303,324`.

| kind / table (key today) | apply-thread sites at `2fde7034` | kind; when |
|---|---|---|
| **Buffer** `g_backendBufferResources` `MG:2757`, `BackendSlotTable<..., Buffer>` `MH:886-888` - ALREADY handle-keyed; the only frontend probe is `HandleOfBuffer` `MG:2925-2941` (scope `:2938`) | callers: `EnsureBufferResource(obj)` `MG:3444-3455`; `IsBufferDrawClean(frontend,res)` `MG:3159-3162` -> `IsBufferDrawCleanByHandle(HandleOfBuffer(frontend),...)`; `MarkBufferGpuWritten` `MG:2945-2951`. Reached per draw from S1 (VAO ensure walk, IBO arm), S5 (UBO/SSBO/atomic/XFB points), image buffer textures | id; memo HIT and miss (IsBufferDrawClean runs on the hit path) |
| **VAO** `g_backendVertexArrayObjects` `MH:1423-1424`, `TwinRegistry<VertexArrayObject, BackendVertexArrayObject, VertexElementsCso>`; key = VAO lifetime id -> VertexElementsCso slot | `ResolveVaoTwin` `DG:1602-1633` (scope `:1609`, `Find` `:1617`, `GetOrCreate` `:1618`) from `PrepareForDraw` `:4772` EVERY draw; `DrawArrays` `:6729-6731`, `MultiDrawArrays` `:6775-6777` (`Find`) | id every draw; mint on first draw of a VAO |
| **Texture** `g_backendTextureObjects` `MH:1818-1819`, key = texture lifetime id -> Texture slot | `SyncTextureObjectToBackend` `DG:1723-1802` (scope `:1734`, `Find` `:1736`, `GetOrCreate` `:1738`, re-resolve `:1781/:1790`) called from the three sync-list rebuilds `:2308,:2381,:2457` (memo MISS), `SyncImageTextureBinding` `:2660` (every image unit), `CopyImageSubData` `:8294-8295`, `GenerateMipmap` `:9575-9578`, CopyTex endpoint `:9735`; `ResolveShaderImageRecord` `:2588/:2634` `HandleOf`; `ResolveAndBindUnitTextures` `:4926-4928` `Find` (memo miss); `ScopedDetachedTextureFramebufferAttachments` `:8837-8839` `Find`; `GetTexImage` `:11884-11886`; mip shape `:9161-9162, :9216-9217` `HandleOf` (transport arm!); CopyTex monolith else-arm `:8512-8515`. `MG`: `:5882-5884` `HandleOf` (image-bindable re-arm), **`:7087-7089` `HandleOf` inside `SyncMipmapsToBackend` - on EVERY texture sync on the handle arm**, `:8481-8483` `ResolvePushedBuiltinSampler`, `:8717-8719` `ResolvePushedTextureParams`, legacy attach `:9256-9261` `Find/GetOrCreate`, `:9630` (monolith-gated), verify arm `:10382-10384` | id on every texture sync (hit or miss of the unit memo); mint on first sync |
| **Renderbuffer** `g_backendRenderbufferObjects` `MH:2819-2820`, key = rb lifetime id | `SyncRenderbufferObjectToBackend` `DG:9688-9710` (scope `:9695`, `Find/GetOrCreate` `:9698-9701`) from CopyTex endpoint `:9714` and attach; `MG:9310-9318` legacy attach; **`MG:9709-9720` `HandleOf` cross-check - NOT monolith-gated, unlike the texture twin at `:9628-9632`**; `MG:13570-13574` `HandleOf` inside `BackendRenderbufferObject::SyncToBackend` (every rb sync); verify arm `:10403-10406` | id per sync; mint on first attach |
| **Framebuffer** `g_backendFramebufferObjects` `MH:1943-1944`, key = FBO lifetime id; default FBO = `kMGPipeDefaultFramebuffer{0,1}` (`MGPipeHandles.h:73`), never a twin | record arm `SyncCurrentFBOByRecord` `DG:3197` `GetOrCreateByHandle` (rec) **but `:3228` `NoteStateForHandle(record.Fbo, currentFBO)` writes a frontend `SharedPtr` into the table**; decline arm `:3303/:3358/:3367-3369` `Find/GetOrCreate`; `:3032` `HandleOf` (monolith-gated `:3031`); `BindCurrentFBO` decline `:4585/:4592/:4602`; `SyncAndBindFramebufferObject` `:4646-4647`; `SyncAndBindFramebufferByHandle` `:4687` (rec) + `:4700` `StateForHandle` (live: the note); named blit `:8361` `StateForHandle`; `ForEachLive` `:8904` (locks every `stateRef`); `MG:9862,:10101` `HandleOf` monolith else-arms (`m_pushedSyncHandle` `MH:1857` under transport) | rec on the steady draw; the note is a live frontend read on every named blit / by-handle bind |
| **Program / ShaderCso** `g_backendProgramObjects` `MH:2540-2541`, key = program lifetime id -> ShaderCso slot (composites: the band, `ProgramEmit.h:411-422`) | `SyncCurrentProgram` `DG:4320-4432` (scope `:4329`, `Find/GetOrCreate` `:4415-4416`) EVERY draw; `ResolveGlobalConstantsRecord` `:4236/:4242` `HandleOf` every draw with UBO bytes; `BindCurrentProgramWithResources` `:5328/:5337` `Find` when the raw-pointer stash `g_currentDrawFrontendProgram` (`:4097,:4509,:5333`) misses; `GetCurrentBackendProgram` `:5689-5691` `Find` (per sub-draw via `:5716-5827`); `GetProgramBackendId` `:6166-6170` `Find/GetOrCreate`; `SetStorageBlockBinding` `:10029-10031`; `MG:12949-12954` `HandleOf` inside `BackendProgramObjectImpl::SyncToBackend` | id every draw; mint on first use |
| **Sampler** `g_backendSamplerObjects` `MH:2678-2679`, `TwinRegistry<SamplerObject, BackendSamplerObject, SamplerCso>` - TWO disjoint families in one table (`MH:2681-2700`): identity twins minted off the SamplerObject's lifetime id, and CSO twins at content-addressed handles (`SamplerEmit.h:427` `Allocate` with no lifetime id) | identity family: `ResolveUnitSamplerBackend` `DG:4989-5013` (scope `:4993`, `HandleOf` `:4999`, `g_unitSamplerLookupMemos`), the sampler-pass decline arm `:5623-5630` **`GetOrCreate(samplerObject)` = a MINT from the apply thread on every unit the record arm does not cover**, `MG:13264-13280` `HandleOf` inside `BackendSamplerObject::SyncToBackend(object)`. CSO family: `ResolveSamplerCsoTwin` `MG:13460-13486` `GetOrCreateByHandle(cso)` from `BindCurrentUnitSamplers` `:5146` and the program pass `:5616` | rec (CSO); id + mint (identity) |
| **SamplerView** `g_backendSamplerViews` `MH:2761-2764` handle-only `BackendSlotTable`; key = texture lifetime id -> SamplerViewCso slot | no backend caller of `HandleOfSamplerViewForTexture` (`MG:4535-4546`, scope `:4544`); its only caller is the harness `MG_IntegrationTest/Harness/PipeApplyPeek.cpp:165` | none on the draw path |

Outside DirectGLES: `MG_Impl/Pipe/ResourceTracker.h:723` (`MGPipeAnnounceBufferGpuWritten`, Magma), `VulkanRenderer.cpp:4554,
:4634, :8986` (Magma hidden resources' destructors + `MGPipeSlots().IsLive` in the blit endpoint resolve). Scope-site census:
20 in `DG` (`:1609 :1734 :2588 :3303 :4236 :4329 :4585 :4926 :4993 :5328 :5689 :6166 :6729 :6775 :8837 :9161 :9216 :9695
:10029 :11884`), 15 in `MG` (`:256 :2938 :4544 :5882 :7087 :8481 :8717 :9256 :9310 :9709 :10382 :10403 :12949 :13273 :13570`),
1 in `ResourceTracker.h`, 3 in Magma, 3 in `MG_Test/Wire/RemoteClientTest.cpp:1883-1903`.

**Corrections to the E note.** (i) The VAO twin key is NOT content-addressed: `VertexInputEmit.h:170-172` mints the
VertexElementsCso slot with `Acquire(VertexElementsCso, vao->GetLifetimeId())`, one CSO per VAO, re-created on the SAME
handle when `ConfigVersion` moves (`:178-179`), so the twin key already equals `MGPipeApplier().BoundVertexElements`.
(ii) The same holds for every kind: Texture `TextureEmit.h:422` / `PF:1495`, Renderbuffer `PF:1499`, Framebuffer
`FramebufferEmit.h:257` / `PF:1503`, ShaderCso `ProgramEmit.h:411-422` / `PF:1507`, SamplerView `SamplerEmit.h:886-887`
all mint off the object's own lifetime id, so `HandleOf(obj)` on the server always answers the handle the records carry.
The rekey is therefore a REPLACEMENT OF THE LOOKUP ARGUMENT, not a re-keying of the table.

## 2. What the records and applier state already carry

- Handle-indexed record tables, Gen-checked on every read (`MG_Pipe/PipeApply.h:486-524`; readers `MG:4325-4360`
  `PipeTextureRecordForHandle` / `PipeRenderbufferRecordForHandle` / `PipeSamplerCsoRecordForHandle` /
  `PipeSamplerViewRecordForHandle` / `PipeShaderCsoRecordForHandle` (band-aware), `PushedFramebufferRecord` `MH:1963`,
  `FindShaderCsoRecord` `DG:4243`; `PipeApply.cpp:444,481,488,505` `!Live || Gen != handle.Gen -> nullptr`).
- The bound handles: `BoundVertexElements` `:594`, `BoundFramebuffer[2]` `:646`, `BoundSamplerViews[u].Texture` `:673`,
  `BoundSamplerStates[u]`, `BoundShaderImages[u].Res`, `DrawProgram/DispatchProgram/BoundShaderCso` `:691-693`,
  `VertexBuffers[].Res`, `IndexBuffer.Res`; the verb workspace `VerbBlitReadFbo/DrawFbo`, `VerbCopyTexDst`, `VerbMipRes`,
  `VerbIndirect*` `:733-755`; `MGPSurface.Res` in `FramebufferRecords`.
- Twin creation by handle already exists per kind: `GetOrCreate(handle)` `ST:294-342` (forward Gen = recycle & reset `:338`,
  backward Gen = refused `:331`), `GetOrCreateByHandle` `MH:407-414`, `AdoptTwinByHandle` `MG:4148-4152`,
  `GetOrCreateBufferResourceForHandle` `MG:2887-2908`, `GetOrCreateSamplerViewForHandle` `MG:4505-4528`,
  `ResolveSamplerCsoTwin` `MG:13474`, `SyncAndBindFramebufferByHandle` `DG:4677-4712`, CopyTex split arm `DG:8487`.
- Twin death by handle already exists for all seven kinds: `object_death` (opcode 78, `MGPHandleOnly` `MGPipeTypes.h:93-98`,
  `PipeCalls.def:265`, `kCtxObject`) emitted by `EmitObjectDeathRecord` `MG_Remote/Client/WireTables.cpp:569-608` (GL thread,
  the client's own allocator) -> `ServerVerbSink::OnObjectDeath` `PipeApplier.cpp:988-1013` -> `ReleaseTwinsForWireObjectDeath`
  `MG:328-372` -> `ReleaseTwinByHandle` `ST:498-507`; Buffer via `resource_destroy` -> `Ops_H_Destroy` `MG:2424-2444`
  `ReleaseByHandle`. Order on the client is fixed (`PF:1623-1626` `NotifyAndFree`: notice -> record out -> `Free`), so the
  death record precedes the slot's next handout on the ring. Under `BatchWaits=1` (default, `Config.h:493`) `object_death`,
  every `create_*/delete_*` (kCtxCso) and every `set_*` (kCtxState) already publish WITHOUT waiting
  (`ClientSession.cpp:915-920`); `EmitAndWait` at `WireTables.cpp:604` is a name only.
- Client-side lifetime-id -> handle maps that STAY client: `MGPipeSlotAllocator::ByLifetimeId` `SA.h:146`,
  `MGPipeResourceTracker::m_bySlot` `ResourceTracker.h:535` (slot -> `BufferObject*` inverse for writebacks), the emitters'
  latches (`VertexInputEmit.h:174`, `SamplerEmit.h:889`, `TextureEmit.h:455-470` `ResolveTexture`), `EmitTables.cpp:1076-1082`
  `PublishedTextureHandle`. All GL-thread; none is touched by this design.

## 3. The gap (what does not exist yet)

No new wire record and no new field is needed for identity. The gaps are server-side shapes:
1. **Per-kind by-handle resolvers on the draw path** are missing for VAO, Program and Texture-at-draw: `ResolveVaoTwin(handle)`,
   `ResolveProgramTwin(handle)`, `ResolveTextureTwin(handle)` (the shape of `ResolveSamplerCsoTwin`). `AdoptTwinByHandle`
   is the body; the release-build voice of the refusal is per kind (`MG:2893-2898`'s shape).
2. **The twin sync bodies take the frontend object** (`MG:4881` VAO, `:7049` mips, `:8802` params, `:10044` FBO, `:11922`
   program, `:13184/:13188` sampler, `:13550` rb) and resolve their own record through `HandleOf(obj)` (`MG:7089, :12954,
   :13574`). Identity's requirement on S1-S5: every body becomes `SyncToBackend(MGPipeHandle, const <Record>&)` and stores
   `m_handle` (the `m_pushedSyncHandle` pattern, `MH:1857`) - the object parameter is deleted, not defaulted to null.
3. **The state note** (`NoteStateForHandle` writer `DG:3228`; readers `:4700`, `:8361`) and **`ForEachLive`'s `stateRef`
   walk** (`DG:8904`, the one direct iteration) are frontend `SharedPtr`s in server memory. S3 owns the replacement: the
   by-handle FBO sync reads `FramebufferRecords[h].State` only; `ScopedDetachedTextureFramebufferAttachments` walks
   `FramebufferRecords` for surfaces naming texture handle T (an O(records) walk, or a reverse index texture -> FBO slots
   maintained at `set_framebuffer_state`).
4. **Raw-pointer memos**: `g_currentDrawFrontendProgram` (`DG:4097`) -> a handle stash; `g_unitSamplerLookupMemos`
   (`DG:4985`) and the sampler identity family (`:4989-5013`, `:5623-5630`, `MG:13264-13280`) are DELETED - a unit whose
   `BoundSamplerStates[u]` is null while the frontend holds a sampler is a missing `bind_sampler_states`, which under
   run-ahead is `Fatal{UnmigratedPipeInput,"SamplerObject@draw"}` (strict), never a mint (S2 to ratify).
5. **`BackendSlotTable` has no composite band.** `EntryAt(handle.Slot)` `ST:581-584` resizes to the slot; a program-pipeline
   composite (slot >= 983040, `SA.cpp:457`, `kMaxHandleSlot = 1<<20` `ST:154`) grows `g_backendProgramObjects` to ~983k
   entries (~40 B each). Today reachable through `GetOrCreate(StatePtr)` for a composite program; after the rekey it is the
   ordinary path. The table gets a `m_band` vector indexed by `slot - base`, as the allocator (`SA.h:144`) and the applier
   (`PipeApply.h:511`) already do.
6. **`MG:9709-9720`** (renderbuffer attach cross-check) lacks the `Transport == Monolith` gate its texture sibling has
   (`MG:9628-9632`): a live id probe under transport today. Fixed by the rekey (the check is deleted; `AdoptTwinByHandle`
   is the whole identity test) - noted so its disappearance is not mistaken for a lost check.
7. The client-side rule that lets the server never miss: the record naming a handle is ON THE RING BEFORE the verb that
   reads it. Already true - `create_*` from the validate point / birth hooks (`PipeMutation.h:996-1184`), `set_*` from the
   tracker walk, `object_death` from the destructor - and the applier's Gen check turns any violation into the loud
   refusal (`PipeApply.cpp:1740-1750`), not a wrong twin.

## 4. The retirement design (server shape after P5e)

**ST (BackendSlotTable), handle-only under `MOBILEGL_PIPE_PUSH`:** delete `GetOrCreate(const StatePtr&)`, `HandleOf`,
`Find(StateObject*)`, `OnFrontendObjectDestroyed`, `NoteStateForHandle/StateForHandle`, `Entry::stateRef`, the memo pair
and `RememberHandle/ForgetHandle`; keep `GetOrCreate(handle)`, `FindByHandle`, `ReleaseByHandle`, `ReleaseTwinByHandle`,
`LiveGenAt`, the holder list, `ForEachLive` re-typed as `fn(MGPipeHandle, const BackendPtr&)`; add the composite band.
`Entry` becomes `{backend, Gen, Live}`. The `StateObject` template parameter survives only as a name. The pull build
never compiles this header (`ST:14-16, :86`), so G1 holds by construction.

**MH (StateBackendObjectRegistry):** the slot arm forwards only the handle members; the legacy `UnorderedMap<StateObject*>`
arm stays exactly as it is under `MOBILEGL_PIPE_LEGACY_MEMOS` (the A/B of ARCHITECTURE 9.6; `CMakeLists.txt:43` default ON).
`DestroyByLifetimeId` becomes legacy-arm-only; the notice consumer `OnFrontendStateObjectDestroyed` `MG:206-320` keeps the
split arm (`EmitObjectDeathRecord`), keeps the monolith switch for the LEGACY arm, and the slot arm's monolith death is the
same `object_death` route applied synchronously (`MGPipeRouteObjectDeath` under `Transport=monolith` = direct apply) - one
death path per arm, the scope at `MG:256` deleted. The NoSession fixture hop (`MG:224-246`) is deleted: a server-role
fixture with no client session gets no deaths (its twins die with the backend at Stop, which is what happens today past
`Running()==false` anyway).

**Twin key = the record's handle, resolved from applier state, never from an object:**
- VAO: `ResolveVaoTwin(st.BoundVertexElements)` in `PrepareForDraw`; `DrawArrays/MultiDrawArrays` client-side attribute paths
  are refused under split already (S1 confirms) - their `Find` becomes the monolith legacy arm only.
- Program: `SyncCurrentProgram(st.DrawProgram)`, `PrepareForCompute(st.DispatchProgram)`, `ResolveGlobalConstantsRecord(handle)`;
  `g_currentDrawBackendProgram` keyed by `{slot,gen}`; `GetProgramBackendId(name)` / `SetStorageBlockBinding(name)` are
  monolith sticky entries (`DG:6152`, `:10022`) - under split they are unreachable (P5b class-C refused or carried as
  `MGPSetStorageBlockBinding.ShaderCso`, `EmitTables.cpp:1267`).
- Texture: `SyncTextureTwin(handle)` called with `BoundSamplerViews[u].Texture`, `BoundShaderImages[u].Res`,
  `FramebufferRecords[h].State.Color[i].Res`, `VerbCopyTexDst`, `VerbMipRes`, `MGPCopyImage` src/dst; `SyncMipmapsToBackend`
  reads the staged store by handle (tx already does, `CONTRACT-P5C.md §2.2`).
- Renderbuffer: `AdoptTwinByHandle(surface.Res)` (already, `MG:9719`) + by-handle `SyncToBackend(handle, record)`.
- Framebuffer: `GetOrCreateByHandle(BoundFramebuffer[t])` / `record.Fbo` (already); the decline arms (`DG:3303-3370`,
  `:4568-4602`) become monolith-legacy-only; `g_fboRecordsTrusted=false` under a transport is `Fatal{ProtocolCorruption}`.
- Sampler: the CSO family only (`ResolveSamplerCsoTwin(st.BoundSamplerStates[u])`); the table is renamed to what it is.
- Buffer: `EnsureBufferResourceForHandle(nullptr, h)` / `IsBufferDrawCleanByHandle(h, res, nullptr)` from every consumer
  that holds `h` (S1/S5 name them); `HandleOfBuffer` and `MarkBufferGpuWritten(obj)` become monolith-only, and the split
  GPU-written announcement takes the handle (`MarkBufferGpuWrittenByHandle`).

**"Is it clean / does it need re-sync" from server-owned serials:** each twin stores `m_handle` and the record serial(s)
it last consumed (`m_syncedResourceSerial`, `m_syncedParamsSerial`, `m_syncedShaderCsoSerial`, `m_syncedRecordHashes`
already exist: `MH:1400-1410, :1800-1812, :2750, :2820`). `FindByHandle` answering null or a different `Gen` = a different
object = full sync; equal handle + equal serials = clean. No frontend version is read anywhere. (S1-S5 own the per-kind
serial sets; the identity invariant is only: the compare is `{handle, serial}`, never `{pointer, version}`.)

**Twin creation moment:** the first apply that resolves the handle through `GetOrCreate(handle)` - a verb, a bind, an
attach - AFTER the record naming it was applied (the applier refuses a bind/verb naming an unknown or stale handle,
`PipeApply.cpp:2266, :2549, :2625, :2683, :2896`). Creation is lazy, exactly as today; no twin is minted at `create_*`.
**Twin death moment:** the apply of `object_death` / `delete_*` / `resource_destroy` (in ring order, after every verb that
named the generation), or the server backend's own destruction at Stop (`MG:188-212` `InProcessTeardown`). `applier_reset`
does not touch twins or object records (`PipeApply.h:800-832`); `MGPipeApplierReleaseObjectRecords` `:841` is wired to
nothing outside tests and stays so.

**The ABA answer ("recycled slot, new gen"):** `Gen` moves only on reuse (`SA.cpp:410-419`); the client frees a slot only
after its death record went out (`PF:1623-1626`); the ring is in order; so at the server every record naming `{s,g}`
is applied before `object_death{s,g}`, which is applied before `create{s,g+1}`. Even with the death skipped (an object
that never crossed emits none, `WireTables.cpp:595-596`), `GetOrCreate({s,g+1})` finds `Live && Gen != g+1` and resets
(`ST:338`); `FindByHandle({s,g})` after the recycle answers null (`:424`); a backward `{s,g-1}` is refused (`:331`).
No wait is involved and none is added: run-ahead changes nothing here. What the client must NOT do is hold a server-side
`SharedPtr` to a frontend object - after P5e the server holds none (no `stateRef`, no state note, no object rows in
`gPipeInputs` under run-ahead), so the P5d deferred-destroy queue (`PipeMutation.h:957-975`, `PF:999-1046`) loses its
producer; it stays as a guard that logs once if it ever enqueues under run-ahead.

**The two scopes:** `MGPipeFrontendKeyedRegistryScope` is deleted (class, 35 Espryt sites, the `SA.cpp:264` exemption, the
`ResourceTracker.h:723` site with it once S5 carries the handle). `MGPipeRefuseAllocatorFromApplyThread` becomes
`Transport != Monolith && OnApplyThread -> Fatal`, unconditional for DirectGLES. `MGPipeReverseAnnouncementScope` is the
Magma question (part 7).

## 5. Monolith / G1

- Pull build: untouched - every symbol above is under `MOBILEGL_PIPE_PUSH` or `MOBILEGL_BUILD_DISAGGREGATED`; the
  `TwinRegistry` alias keeps the two-argument mangled name (`MH:626-634`); G2/G14 unchanged.
- Push build under `Transport=monolith`: the applier is in-process and populated synchronously by the same records
  (`MGPipeRoute* == MGPipeApply*` under monolith), so the handle bodies run under monolith too and read
  `MGPipeApplier()` state that is current at the verb. Behaviour identical; bytes differ. The selection is the FAMILY BIT
  (`XSubsystemEnabled()`, the `0x1fff` mask), not a transport check: bit clear -> the legacy `UnorderedMap` arm (its
  `HandleOf` on the GL thread is legal), bit set -> handle arm. Under a transport the bits are caps-required, so only the
  handle arm is reachable and a legacy-arm entry from the apply thread is the Fatal. The monolith entry points that reach
  the backend WITHOUT a record - `GetProgramBackendId(name)`, `GetTexImage`, `CopyImageSubData`, the sticky forwards -
  keep their object-taking monolith bodies and resolve the handle on the GL thread through the emitter-side maps
  (`PublishedTextureHandle`'s shape), i.e. the resolution moves to the SAME side the mint lives on.
- If the integrator rules "monolith bytes identical" instead, the alternative is `#if MOBILEGL_BUILD_DISAGGREGATED` +
  `Transport != Monolith` around each handle body with the frontend body kept beside it; it keeps 35 sites alive and the
  scope classes with them, and is not recommended.

## 6. Red-once

1. Revert `ResolveVaoTwin`'s handle arm (put `Find(vao.get())` back) with the scope gone: the first draw under
   `integration-split` aborts `Fatal{RoleViolation, "MGPipeSlots"}` naming `HandleOf` (`SA.cpp:265-272`); the same for
   `SyncCurrentProgram` and `SyncTextureTwin`.
2. `RemoteGuards.AnAllocatorProbeInsideAnExemptionScopeOnTheApplyThreadIsAllowed` (`RemoteClientTest.cpp:1881-1895`)
   is deleted with the scope; a new `...FromTheApplyThreadIsFatalEvenAtTheOldDebtSites` posts `HandleOf` through
   `RunOnApplyThread` and expects the abort. `:1866-1879` and `:1901-1913` stay as the control.
3. ABA under run-ahead: `CtWireScenario.TextureDeathCrossesAndTheRecycledSlotAnswersTheNewObject` /
   `FramebufferDeath...` (`CtWireScenario.cpp:120-215`) and `HandleRecycleScenario`'s `aba` arm (`:140-149`) run with the
   draw wait removed; skipping the `ReleaseTwinByHandle` in `OnObjectDeath` must render the dead twin's colour (red).
4. Unit: `DirectGLESSlotTable.AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted` (`SanityTest.cpp:4898`) kept; a new
   `ACompositeHandleDoesNotGrowTheOrdinaryTable` pins the band; the cases that drive `GetOrCreate(StatePtr)` / the death
   notice (`:3499-3960, :4061, :4241, :4378`) are moved onto the legacy arm or rewritten by handle; `ScopedDirectGLESTextureBindings`
  (`:230-262`) keeps its by-value registry copy (holder list unchanged).
5. Strict lane: `integration-split-strict` green for the migrated verbs is the gate that a frontend-keyed resolution cannot
   hide behind a memo hit (P5e-3).

## 7. Risks and what S7 could not settle

- **Magma.** `VulkanRenderer.cpp:4554, :4634, :8986` and `ResourceTracker.h:723` are apply-thread allocator touches P7
  retires; with the guard unconditional they abort on the first Magma frame under `inproc`. Ruling needed: keep ONE scope,
  renamed and compiled for DirectVulkan only (`MagmaP7AllocatorDebtScope`), with the guard's exemption keyed on the
  backend kind; Espryt has no exemption. (P5e-4 keeps Magma lockstep either way.)
- **Sampler identity family deletion** changes what a draw does for a unit the record arm does not cover (`DG:5601-5630`):
  today it mints a twin off the object; after P5e it is a Fatal under strict. S2 must confirm `bind_sampler_states`
  covers `[0, maxTouchedUnit]` (E's caveat A8) before the arm is removed.
- **`ForEachLive` / the detach walk** (`DG:8893-8935`) needs S3's reverse index; without it the by-handle FBO table cannot
  answer "which twins attach T" and the walk silently does nothing (the handle-keyed entries are already invisible to
  `ForEachLive`, `ST:282-285`).
- **NoSession server-only fixtures** (`MG:224-246`, `CONTRACT-P5C.md §5.2` amendment) lose their death delivery; the
  affected fixtures are `ServerLoop` unit cases with no client - to be enumerated by the integrator (grep `RunOnApplyThread`
  users in `MG_Test/Wire`).
- **Composite band in the twin table** (part 3.5) is a pre-existing ~40 MB growth on any program pipeline; it becomes
  the ordinary path after the rekey, so it is a P5e prerequisite, not a follow-up.
- **Ordering of packages.** Every other family's design calls a by-handle resolver; the ST/MH rekey + the per-kind
  resolvers + the band land FIRST (one worktree, no behaviour change under lockstep), then S1-S5 swap their arguments, then
  S6 removes the wait. The guard flip (delete the scope) is the LAST commit of the identity package, because it is the
  red-once for every site that was missed.
- Not verified: whether any `MGPipeApplier()` bound handle can be null at a verb that today resolved a non-null frontend
  object (a family bit clear under transport). The caps-required-bits rule (P5e-4) is what makes that unreachable; S6 to pin.
