# S2 — textures and samplers under run-ahead (P5e scouting report)

Head `2fde7034`. Paths under `MobileGL/`. "The record arm" = `MOBILEGL_PIPE_PUSH` +
`TextureResourceSubsystemEnabled()` / `SamplerSubsystemEnabled()`.
**Headline.** The SAMPLER half is already run-ahead-clean on its record arm, and
`SamplerImpl::ResolveSamplerCsoTwin` (`Managers.cpp:13457-13486`) is the exact shape the TEXTURE
half needs: handle in, record first, `GetOrCreateByHandle`, serial-gated sync, no frontend touch,
no allocator probe. The texture half is one step behind — its three sync bodies already read the
record and the staged store, but every one re-enters through
`g_backendTextureObjects.HandleOf(frontendObject)` and the per-draw work list borrows frontend
binding slots. **No new record and no new wire field on the draw path**; one signature, four
line-level substitutions, one ruling.

## 1. The live reads at `2fde7034`

**live** = frontend dereference; **id** = `GetLifetimeId()` + client allocator probe; **mint** =
`Acquire`, a WRITE into it; **rec** = reads nothing frontend; **hit** = runs on every steady draw.

### 1.1 The per-draw keys — already server-owned, nothing to do
`CaptureDrawTextureSyncKeys` `DirectGLES.cpp:2188-2211` reads `GetTextureContextId` `:2189`,
`GetMaxTouchedTextureUnit` `:2203`, `GetSamplingResolutionGeneration` `:2204` — **rec, hit**: the
two generations are APPLIER_DERIVED (`FieldOwnership.def:126-131`), answered from
`MGPipeApplierTextureShutterSerial()`/`ContextSerial()` under a server stamp
(`PipeInputs.h:490-527`); the mark is RECORD_SUPPLIED via `set_context_values` (`Coverage.def:260`,
`MGPipeTypes.h:1694`). `CurrentUnitBindingsEpoch`'s record arm `:2044-2071` ->
`UnitBindingsEpochFromRecords` `:2003-2042` reads the two counts and the two serials — **rec, hit**.

### 1.2 `SyncNeccessaryTextures`, unit half (`:2321-2391`)
| site | read | kind |
|---|---|---|
| `:2346` `PairingsIntact(g_unitTextureSyncList)` | per entry `entry.slot->get()`, a `const SharedPtr<ITextureObject>*` borrowed INTO the frontend unit's slot (`:2127-2131`, borrowed at `:2381`) | **live, hit** |
| `:2356` `IsDrawSyncClean(entry.slot->get(),...)` -> `Managers.h:1641-1658` | `GetTextureParamsVersion`, `GetContentVersion`, `GetSamplerObject()->GetVersion()`, `GetStorageType()`; then `:2358-2361` hands `*entry.slot` to the three syncs | **live, hit** |
| `:2374-2382` memo MISS | `GetTextureUnitObject(index)`/unit, every `GetAllBindingSlots()` slot's `GetBoundObject()`, `IsUndefinedDefaultTexture`, `SyncTextureObjectToBackend(obj)` | live, id, mint |

### 1.3 `SyncTextureObjectToBackend` (`:1723-1806`) and the three sync bodies
| site | read | kind |
|---|---|---|
| `:1734` scope, `:1736` `Find(obj)`, `:1737` `GetOrCreate(obj)`, and the re-resolve `:1781`,`:1797` | `HandleOf` -> `GetLifetimeId` + `MGPipeSlots().FindByLifetimeId` (`SlotTables.h:430-450`); miss mints (`:244-250`) | id, **mint** |
| `:1757-1758` `RequireImageBindableStorage(obj)` -> `Managers.cpp:5816-5975` | `HandleOf` `:5884`, `GetTarget` `:5888`, `GetUploadTargets`/`GetMipmapTexelSize`/`GetMipmapByteSize`/**`MarkStorageDirty`** `:5931-5940` — the last is ALREADY `Fatal{RoleViolation,"texture-legacy-arm"}` from the apply thread (`MipmapStorage.cpp:39-51`) | id, live |
| `:1760` -> `SyncTextureParamsToBackend` `Managers.cpp:8803+` | prologue `ResolvePushedTextureParams` `:8712-8743` = scope `:8717` + `HandleOf` `:8719`; body `:8876-8877` `GetTarget()` **on both arms**; `:8841,:8874` `GetExternalIndex` (logs) | id, **live** |
| `:1761` -> `SyncBuiltinSamplerToBackend` `:8554+` | prologue `ResolvePushedBuiltinSampler` `:8472-8551` = scope `:8481` + `HandleOf` `:8483`; body `:8620-8621` `GetTarget()` on both arms | id, **live** |
| `:1762` -> `SyncMipmapsToBackend` `:7049+` | `:7060` `IsTextureView()` BEFORE the record is resolved; `:7087` scope + `:7089` `HandleOf`; `:8223-8231` the Buffer arm's `GetBufferBindingSlot().GetBoundObject()`; `GetExternalIndex` in ~14 log lines. Everything else on the staged arm is the record + `ServerStagedTexture()` (`:6902-6985`) | id, **live** (4 sites) |

### 1.4 `BindCurrentTextures` / `ResolveAndBindUnitTextures`
- `:5277-5284` memo key: `currentProgram->GetLifetimeId/GetBackendStateVersion/GetLinkStatus` —
  **live, hit** (S4's rows; this memo is the consumer). `:5290`'s `memcmp` vs
  `g_boundTexturesCache` is server memory — rec.
- `:4861-4948` `ResolveAndBindUnitTextures` (memo MISS) — **live, id**:
  `GetTextureUnitObject(unit)` `:4860`, two passes over `GetAllBindingSlots()` `:4876`,
  `GetBoundObject`, `IsUndefinedDefaultTexture` `:4884`, `GetExternalIndex` `:4886`, `GetTarget`
  `:4890`, the alias arbiter `sampledTargetForUnit` `:4845-4857`
  (`GetMaxUniformLocation`/`GetUniformSamplerOrImageUnitIndex`/`GetUniformType`), `GetSamplerObject`
  `:4914-4916`, `SamplesAsIncompleteTexture` `:4918`, scope `:4926` + `Find(obj)` `:4928`, unbind
  walk `:4945-4947` (`GetTarget` per slot).

### 1.5 The samplers
- `BindCurrentUnitSamplers` gate `:5051-5059` (keys + `g_boundSamplersCache`) and record arm
  `:5106-5151` (`st.SamplerStateStart/Count/BoundSamplerStates[]`, `UnbindSampler`,
  `ResolveSamplerCsoTwin`) — **rec, hit; already clean**.
- decline arm `:5152-5169`: `GetTextureUnitObject(unit).GetSamplerObject()` `:5157` ->
  `ResolveUnitSamplerBackend` `:4988-5026` = scope `:4993` + `HandleOf` `:4999` — live, id.
- sampler pass in `BindCurrentProgramWithResources` `:5562-5596` — **live**:
  `GetTextureUnitObject(unit)` `:5562`, `GetSamplerObject` `:5563`,
  `GetBindingSlot(Texture2D).GetBoundObject()` `:5564-5565`, `samplerObject->GetLodBias()` `:5573`,
  `boundTexture->GetSamplerObject()->GetLodBias()` `:5580`, `texture2D->GetSamplerObject()` `:5591`,
  `texture2D->GetFormat()` `:5595`.
- `:5609-5620` CSO-twin arm — rec. `:5624-5632` else arm: `ResolveUnitSamplerBackend` (id) then
  `g_backendSamplerObjects.GetOrCreate(samplerObject)` `:5626` — **mint**, the only one left here.
- **Off the steady draw path (waited ops under P5e-2):** `SyncTextureObjectToBackend` from
  `CopyImageSubData` `:8294-8295`, `GenerateMipmap` `:9575,:9578`, the copy-endpoint resolve
  `:9735`, the texture ops `:10046+`, readbacks `:11695,:11866`; `Find(texture.get())` at
  `:8512,:8839,:9305,:9419,:11886`.

## 2. What the records and applier state ALREADY carry

| need | carrier | citation |
|---|---|---|
| which texture each unit samples, PROGRAM-RESOLVED, one per unit, no alias contest, with `IsUndefinedDefaultTexture` + `SamplesAsIncompleteTexture` already applied (entry -> null) | `st.BoundSamplerViews[unit]={View,Texture,Unit}`, `SamplerViewStart/Count/Serial` | `PipeApply.h:673-676`, `MGPipeTypes.h:856-862`, emitter `SamplerEmit.h:733-789`, the two drops `:760-765` |
| the frontend TARGET to bind, aliasing format, view restrictions, samples | `SamplerViewCsos[View].View.{Target,InternalFormat,MinLevel..NumLayers,Samples,FixedSampleLocations}` | `MGPipeTypes.h:524-535`, `SamplerEmit.h:899-916` |
| which sampler CSO each unit carries, and its 15 values (`lodBias`, filters, `mipmapMode`, `compareMode`, border) with a server-owned `Serial` | `st.BoundSamplerStates[]` + `SamplerCsos[h].Params` | `PipeApply.h:335-341`, `:678-681`, `MGPipeValueTypes.h:462-486` |
| a texture's params, built-in sampler CSO, the two resync bits; and its storage shape, target, format, levels, immutability, `ImageBindableHint`, `ViewOf`, texbuffer backing | `TextureResources[h].Params`+`ParamsSerial`, `.Desc` | `PipeApply.h:265-281`, `MGPipeTypes.h:295-356`, `:548-566` |
| "did storage/texels move", and the twin's own "what did I last sync" | `.Serial` + `PendingUploads` + `ServerStagedTexture()` defined/GPU-dirty; `m_syncedResourceSerial`, `m_syncedParamsSerial`, `m_syncedBuiltinSamplerSerial`+`m_syncedBuiltinSampler` | `PipeApply.h:240-318`, `Managers.cpp:6902-6985`, `Managers.h:1785-1812` |
| texture death by handle on the apply thread, no allocator; the handle-keyed adopt/recycle rule | `object_death` -> `ReleaseTwinsForWireObjectDeath` -> `ReleaseByHandle`; `GetOrCreate(MGPipeHandle)`/`FindByHandle` | `Managers.cpp:328-337`, `SlotTables.h:278-341`, `:420-425`, `:488-506` |

The brief's two questions: **yes**, `object_death` covers textures since P5c ct
(`Managers.cpp:331-332`); and the default texture (name 0) DOES have a record —
`TextureEmit.h:806-833` self-heals a create+respecify when its first `set_texture_params` is
refused, written for exactly that case.

## 3. The gap

- **G-S2-1 (a signature, not a field).** No `SyncTextureToBackendByHandle(MGPipeHandle)`; every
  entry takes `const SharedPtr<ITextureObject>&` and re-derives the handle with `HandleOf`. Needed:
  one handle-taking entry point + handle-taking prologues
  (`ResolvePushedTextureParams(MGPipeHandle)`, `ResolvePushedBuiltinSampler(MGPipeHandle)`).
- **G-S2-2 (four live reads inside the handle arm, all with an existing carrier).**
  `Managers.cpp:8876-8877` and `:8620-8621` `GetTarget()` -> `Desc.Target` (via
  `StagedTextureTargetForPipeTarget`, `:6905`); `:7060` `IsTextureView()` -> `Desc.ViewOf`
  (`MGPipeTypes.h:356`), record resolved first; `:8223-8231` -> `Desc.BufferForTexBuffer`
  (already read at `:8240`). Plus **G-S2-4**: ~20 log lines print `GetExternalIndex()`;
  `Desc.GlNameForDiag` (`MGPipeTypes.h:349`) is the declared diagnostics carrier. Mechanical.
- **G-S2-3 (no handle-arm clean gate).** `IsDrawSyncClean(const ITextureObject*,...)`
  (`Managers.h:1641-1658`) has no record-keyed sibling, so the memo-HIT path is frontend-bound
  every draw. Needs no new state — see §4.
- **G-S2-5 (RULING).** `RequireImageBindableStorage`'s re-dirty (`Managers.cpp:5930-5962`) reaches
  back into the client's storage model and already aborts under split; `ImageBindableHint` is the
  prevention and P9 owns the pull, so P5e should delete the frontend argument and leave the split
  arm a NAMED refusal at the entry, not build half a terminator.
- **G-S2-6 (narrowing, needs the integrator's word).** Driving the unit list off
  `BoundSamplerViews` syncs only the program-resolved texture per unit, where today the walk syncs
  every slot of every touched unit. The union of (views + image units + the FBO attachment lists +
  the waited texture ops) still covers every texture anything reads, so this narrows WORK, not
  coverage — but it is a behaviour delta against the monolith arm and must be named under G1.

## 4. The retirement design

**Twin key.** `g_backendTextureObjects` stays one table; every draw-path resolution becomes
`GetOrCreateByHandle(view.Texture)` / `FindByHandle(view.Texture)`. `HandleOf`, `Find(StateObject*)`
and `GetOrCreate(const StatePtr&)` become unreachable from the apply thread, so the existing
`MGPipeRefuseAllocatorFromApplyThread` guards (`SlotTables.h:255-266`, `:436-441`) stop being
scope-exempted here, and this family's `MGPipeFrontendKeyedRegistryScope` sites —
`DirectGLES.cpp:1734`, `:4926`, `:4993`, `Managers.cpp:5882`, `:7087`, `:8481`, `:8717` — are
deleted, not moved. **Sync shape** (the `ResolveSamplerCsoTwin` template,
`Managers.cpp:13457-13486`):
```
BackendTextureObject* SyncTextureToBackendByHandle(MGPipeHandle res, Bool imageBindable) {
    const auto* rec = PipeTextureRecordForHandle(res);       // record FIRST; null -> named decline
    auto* slot = rec ? g_backendTextureObjects.GetOrCreateByHandle(res) : nullptr; // adopt/recycle by Gen
    if (!slot) return nullptr;                               // backward Gen -> named decline
    if (!*slot) *slot = MakeShared<BackendTextureObject>();
    (*slot)->SyncTextureParamsToBackend(res, *rec); (*slot)->SyncBuiltinSamplerToBackend(res, *rec);
    (*slot)->SyncMipmapsToBackend(res, *rec);
    if ((*slot)->NeedsParameterResync()) { /* params; builtin */ }
    return slot->get();
}
```
The by-value twin copy + second `Find` (`:1745-1755`, `:1781-1805`) go with the map arm: a slot
entry is an array element, so the nested-`glTextureView` relocation hazard is answered by
re-indexing, not by a refcount.
**"Is it clean" from server-owned serials.** `IsDrawSyncCleanByRecord(res, rec)` is EXACTLY the
conjunction of the three handle-arm prologues' own early-outs — the relation
`Managers.h:1626-1640` already states for the frontend version:
```
m_isInitialized && m_syncedResourceSerial != 0 && m_syncedResourceSerial == rec->Serial   // :7133-7139
&& rec->PendingUploads.empty()
&& m_syncedParamsSerial == rec->ParamsSerial && rec->Params.ForceResync == 0 && !m_forceTextureParamsResync
&& m_syncedBuiltinSampler == rec->Params.BuiltinSampler                                   // :8735-8741
&& m_syncedBuiltinSamplerSerial == PipeSamplerCsoRecordForHandle(rec->Params.BuiltinSampler)->Serial
&& rec->Params.SamplerResync == 0 && !m_forceSamplerResync                                // :8539-8548
&& (TextureStorageType)rec->Desc.StorageKind == TextureStorageType::Mipmap
```
No new member, no new field. The `contextId`/`samplingGeneration` arguments disappear: the
applier's `ContextSerial` and `TextureShutterSerial` (`PipeApply.h:697-717`) are what the keys
already carry.

**The unit work list and the bind.** `UnitTextureSyncEntry` (`:2127-2132`) becomes
`{MGPipeHandle Res, BackendTextureObject* backend}`; the memo key becomes `(ContextSerial,
SamplerViewsSerial, SamplerViewStart, SamplerViewCount, g_backendContextGeneration)` —
`SamplerViewsSerial` moves iff the resolved set moved, which the emitter's content-hash suppressor
(`SamplerEmit.h:692-697`) makes sound — and **`PairingsIntact` is deleted**: no borrowed slot is
left to go stale. The rebuild walks `st.BoundSamplerViews[Start..Start+Count)` calling
`SyncTextureToBackendByHandle`. `ResolveAndBindUnitTextures` collapses to one pass over the same
window: `FindByHandle(entry.Texture)`, target from `SamplerViewCsos[entry.View].View.Target`, bind;
then unbind every backend target the window did not claim, driven by the fixed `TextureTarget` enum
plus `g_boundTexturesCache` (server memory) instead of `GetAllBindingSlots()`. The alias arbiter
`sampledTargetForUnit` `:4845-4857` DIES — the client already arbitrated (`SamplerEmit.h:751-757`).

**The sampler pass** `:5562-5596`. The unit's effective sampler becomes `st.BoundSamplerStates[unit]`
if non-null, else `TextureResources[view.Texture].Params.BuiltinSampler`; its values come from the
CSO record (`lodBias`, `minFilter`, `magFilter`, `mipmapMode`, `compareMode` all present,
`MGPipeValueTypes.h:462-486`), and `texture2D->GetFormat()` becomes
`SamplerViewCsos[view.View].View.InternalFormat`. The raw-depth-fetch substitution itself STAYS on
the server (`docs/Disaggregated/ARCHITECTURE.md` §5.5 assigns it there by name). `:5624-5632`'s
`GetOrCreate(samplerObject)` is deleted; the CSO-twin arm `:5609-5620` is the only arm left.
Finally the two decline arms — `BindCurrentUnitSamplers` `:5152-5169` and
`CurrentUnitBindingsEpoch` `:2073-2098` — become named refusals under a live wire instead of
falls-back to `GetTextureUnitObject`, and with them gone
`CaptureUnitBindings`/`UnitBindingsUnchanged`/`g_observedUnitBindings` (`:1867-1957`) are
monolith/legacy-only text.

## 5. Monolith / G1

Every edit sits inside `#if MOBILEGL_PIPE_PUSH` and, for the run-ahead arms, behind
`MG_Config::Transport != MG_Config::TransportMode::Monolith` — the idiom already at
`DirectGLES.cpp:3226`, `:4725`, `:6534`. The pull build keeps `:1936-1957`, `:2073-2098`,
`:2340-2391`, `:4861-4948`, `:5152-5169`, `:5624-5632` token for token, the D-P rule this family
already follows (`Managers.cpp:8613-8618`, `:8791-8800` spell the `#if/#else` arm split and why a
shared prologue is a G1 failure). Three obligations: (1) `BackendTextureObject`'s pull-build layout
must not move — every member the new gate needs is already inside `#if MOBILEGL_PIPE_PUSH`
(`Managers.h:1785-1812`), so the admitted-resize set stays empty; (2) `UnitTextureSyncEntry`'s
re-shape is push-only, two structs under one name selected by `#if`, which
`g_fboTextureSyncListRecordKeyed` (`:2157-2171`) already does; (3) the push build under
`Transport=monolith` must KEEP the frontend walk — no server to answer from — so the run-ahead arm
is a THIRD arm, like `MGB_STAGED_TEXTURE_LIVE` (`:6902-6903`). `MOBILEGL_PIPE_VERIFY` forces the
lockstep, so the comparator keeps seeing both.

## 6. Red-once

1. **Widen the pinned guard list.** `MG_Test/Wire/RemoteClientTest.cpp:1938-1941` deliberately
   EXEMPTS the shape reads — "GetMipmapTexelSize / GetMipmapByteSize / GetMipmapLevelCount /
   GetUploadTargets / GetTarget / IsComplete ... the per-draw binding walk reads them every draw".
   P5e adds those six plus `GetSamplerObject`/`GetTextureParamsVersion`/`GetContentVersion` to
   `RefuseLegacyTextureArmFromApplyThread` (`MipmapStorage.cpp:39-51`) and its
   `MGL_TEXTURE_GUARD_TEST` rows `:1942-1978`. Revert the handle arm -> each aborts by name.
2. **`Fatal{UnmigratedPipeInput,"GetTextureUnitObject@DrawVbo"}`** under
   `MOBILEGL_IPC_STRICT_ERRORS=1` (`PipeInputs.cpp:71-77`, `:191-224`), where the row is
   BARRIER_PULLED and the read today only counts as `rsp`: revert `ResolveAndBindUnitTextures`'
   window arm -> strict lane red at the named field+verb. **3.** And an integration assertion over
   `PipeSlotPeek` (`MG_IntegrationTest/Harness/PipeSlotPeek.h`) that
   `MGPipeFrontendKeyedRegistryScope` entries for this family are **0** over a frame; the mint is
   already `Fatal{RoleViolation,"MGPipeSlots"}` (`SlotTables.h:255-266`).
4. **`DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture`**
   (`MG_Test/SanityTest.cpp:3075-3144`) is the `PairingsIntact` pin: keep it for monolith/pull,
   and give it a handle-arm sibling whose hazard is ABA — recycle the slot to `{slot, gen+1}`
   while the old entry is in the work list, assert the twin is re-minted
   (`SlotTables.h:316-340`'s forward/backward rule answers it).
5. **`TextureParamsWithoutASamplerViewScenario`** stays green (the D10/G9 pin), i.e. the view
   window must not become the ONLY path into `SyncTextureParamsToBackend`; and
   **`SampledSetStalenessScenario`** — a texture that becomes complete with no rebind must
   re-enter the sampled set, which under the new key is `SamplerViewsSerial` moving; revert
   `Tracker.h:605-610`'s `textureParams` mix -> red.

## 7. Risks and what I could not settle

- **R1 — the window is not pinned (A8).** Everything in §4 reads
  `[SamplerViewStart, +SamplerViewCount)`. `SamplerEmit.h:735-737` emits `Start=0,
  Count=maxTouched+1`, but `PipeApply.h:666-669` says only "the last set as received" and does not
  require coverage; under run-ahead a too-narrow window silently drops a sync, and a decline is
  what run-ahead cannot take. **P5e needs a contract sentence**: the window MUST cover
  `[0, MaxTouchedTextureUnit]`, narrower is `Fatal{ProtocolCorruption,"SetSamplerViews.Count"}` —
  not a server-side re-derivation.
- **R2 — the program key (conflict with S4).** `BindCurrentTextures`' memo `:5277-5284` reads the
  frontend `ProgramObject`; my design deletes the alias arbiter but not that key. **Ruling needed:**
  key it on `st.DrawProgram` + `ProgramBindingSerial` (S4's rows), or on `SamplerViewsSerial` alone
  — which already mixes the program-resolved opaque units (`Tracker.h:605-610`)? I lean to the
  latter (one fewer cross-family key) but could not rule out that a relink with unchanged
  resolution must still re-bind.
- **R3 — `RequireImageBindableStorage` under run-ahead.** §3 G-S2-5. It aborts today, but three
  frames deep at a `MarkStorageDirty` guard rather than as a named refusal at the entry; run-ahead
  turns a decline into a hard stop. How often `ImageBindableHint` fails to prevent it is unmeasured.
- **R4 — the twin's mutable flags.** `m_forceTextureParamsResync`/`m_forceSamplerResync` are
  server-set and read-not-cleared from the wire (D-E2) — server memory, safe — but the new gate
  reads BOTH them and the wire bits, so "neither side clears the other's" becomes load-bearing for
  a gate, not just for a push. One contract sentence.
- **R5 — buffer textures.** `Managers.cpp:8223-8231` still hands the frontend `BufferObject` to
  `EnsureBufferResourceForHandle` (D-A4's `frontend` argument). `Desc.BufferForTexBuffer` names the
  buffer; whether `frontend` may be `nullptr` under split is **S1/S5's call** — the E note says the
  transport arm reads nothing from the object (`Managers.cpp:3199+`), which suggests yes.
- **R6 — unmeasured.** This family's share of the apply thread at `2fde7034` is unknown; the E
  note's 3.0% `HandleOf<ITextureObject>` is at `85362a85`, pre-round-3. Re-measure before sizing.
