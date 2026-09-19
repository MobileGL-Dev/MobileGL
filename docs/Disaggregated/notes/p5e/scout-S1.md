# S1 — VAO / vertex input / index, under run-ahead

Head `2fde7034`. Paths under `MobileGL/`. Arm assumed throughout: `MOBILEGL_PIPE_PUSH` with bits 7
and 8 set (`ResourceSubsystemEnabled()` / `VertexInputSubsystemEnabled()`, `Managers.h:911-918`),
`EsprytSlotTablesEnabled()`, `Transport != Monolith`. The E note's a.1 is the starting map; the
site set below is re-resolved and differs from it in three places, marked **[new]**.

## 1. The live reads of this family, at 2fde7034

Legend as the E note's: **live** = frontend object/array dereference; **id** = `GetLifetimeId()` +
a probe of `MGPipeSlots()`; **mint** = a write into the client allocator; **rec** = record arm.
"hit" / "miss" is the memo state of the site's own memo.

| # | site | what it dereferences | kind | when |
|---|---|---|---|---|
| 1 | `DirectGLES.cpp:4772` `ResolveVaoTwin(currentVAO)` → `:1617-1618` | `g_backendVertexArrayObjects.Find(vao.get())` → `SlotTables.h:411-414` → `HandleOf` `:436-449`: `stateObj->GetLifetimeId()`, then the table's one-entry front memo (`:444`) or `MGPipeSlots().FindByLifetimeId` (`:445-446`); a miss takes `GetOrCreate(vao)` `:215-266`, which is `MGPipeSlots().Acquire` `:247-248` | live + id (mint on a first sight) | every draw; the `GetLifetimeId()` read runs on the memo hit too |
| 2 | `DirectGLES.cpp:4775` | `currentVAO->GetConfigVersion()` | live | every draw, unconditional |
| 3 | `SyncVaoAttributeBuffersByHandle` memo hit, `:733` | `IsBufferDrawCleanByHandle(entry.handle, entry.resource, entry.frontend)`; under a transport `Managers.cpp:3148-3150` does NOT dereference `frontend` | none (handle-only) | per distinct VBO, only when `memo->vboCleanEpoch != bufferEpoch` (`:730`) |
| 4 | same, dirty repair `:735-736` | `currentVAOObject->GetAttribute(entry.attribIndex).Buffer` → `EnsureBufferResource(obj)` → `HandleOfBuffer` (`Managers.cpp:3444`, `:2922-2941`, scope `:2938`) | live + id | on a dirty probe |
| 5 | same, memo miss `:744-772` | `GetAllAttributes()` (32 `VertexAttribute`, each a `SharedPtr<BufferObject>` + 10 scalars), `attrib.Buffer`, `EnsureBufferResource(bufferObject)` `:764`, `HandleOfBuffer(bufferKey)` `:769` | live + id (twice per distinct buffer) | memo key miss |
| 6 | IBO arm `:896-925` | `currentVAOObject->GetIndexBufferBindingSlot().GetBoundObject()` `:897`; `HandleOfBuffer(possibleIBO.get())` `:907`; `EnsureBufferResource(possibleIBO)` `:917` | live + id | every indexed draw; `:897` and `:907` run on the hit too |
| 7 | `SyncCurrentVAO` `:1641-1652` → `SyncToBackend` (`Managers.cpp:4881-4892`) → `SyncToBackendFromApplier` (`:5160-5350`) | nothing frontend — every field is `st.BoundVertexElements` / `rec->Attributes[]` / `st.VertexBuffers[]` / `st.IndexBuffer.Res` (`:5220-5345`) | rec | every draw |
| 8 | `SyncCurrentVertexAttributeValues` `:1662-1719` | `MGB_CTX->GetBoundVertexArray()` `:1668`; `program->GetActiveAttributeLocationMask()` `:1672` (S4); `vao->GetConfigVersion()` `:1683`; miss: `vao->GetAttribute(location).Enabled` `:1688`; `program->GetAttribType(location)` `:1701`,`:1715` (S4) | live | `:1668,:1672,:1683` every draw with a program; `:1688` on memo miss |
| 9 | `:1700` | `MGB_CTX->GetCurrentVertexAttribute(location)` | rec (`set_vertex_attrib_defaults`, RECORD_SUPPLIED — `PipeFill.cpp:2304`, `:2594-2660`) | pending-mask draws only |
| 10 | `DrawArrays` `:6724-6735` (scope `:6729`), `MultiDrawArrays` `:6767-6782` (scope `:6775`) | `g_backendVertexArrayObjects.Find(currentVAO.get())`, then `SyncClientSideAttributesForDrawArrays(currentVAO, first, count)` (`Managers.cpp:5364-5468`): `GetAllAttributes()`, and **`attrib.Offset` dereferenced as a raw CLIENT POINTER** `:5382,:5430` | live + id + **client memory** | every `glDrawArrays` / `glMultiDrawArrays` with a VAO bound |
| 11 | **[new]** `BoundElementArrayBuffer()` `:6447-6452` / `BoundElementArrayBufferId()` `:6456-6461`, reached from `ScopedRestartIndexSubstitution` `:6591`, `:6687` | `MGB_CTX->GetBoundVertexArray()`, `GetIndexBufferBindingSlot().GetBoundObject()`, `EnsureBufferResource(ibo)` → `HandleOfBuffer` | live + id | every `DrawElements`-family call whose `ResolveRestartSubstitution` (`:6464-6480`) is not `None`; also `MultiDraw.cpp:45,:905` |
| 12 | **[new]** `:1646` `g_backendVertexArrayObjects.CollectGarbageIfNeeded()` | no-op on the slot arm (`Managers.h:558-563`) | — | — |

`EnsureBufferResourceForHandle` (`Managers.cpp:3193-3435`) holds the `SharedPtr` but reads nothing
from it under a transport: the shadow base is `resource->hostBytes` (`:3208-3222`),
`SyncPersistentMappedRange` is skipped (`:3334-3341`), definedness comes from the descriptor plus
staged coverage (`:3376-3385`). `(nullptr, h)` is already the live shape at `DirectGLES.cpp:953`.

Scope sites this family owns: `DirectGLES.cpp:1609`, `:6729`, `:6775` — 3 of the 20; plus every
`HandleOfBuffer` reached from rows 4, 5, 6, 11 (`Managers.cpp:2938`).

## 2. What the records and the applier already carry

- `create_vertex_elements` / `bind_vertex_elements` / `delete_vertex_elements` —
  `PipeCalls.def:151-153`, all `kCtxCso`, therefore already published without a wait
  (`ClientSession.cpp:915-922`). Emitter `VertexInputEmit.h:158-189` (`EmitVertexElements`) and
  `:352-397` (`EmitCreate`, **all 32 attributes and all 32 binding points**, `MGPVertexAttribWire`
  `MGPipeTypes.h:497-508`). Applier record `MGPipeVertexElementsRecord` (`PipeApply.h:389-400`):
  `Attributes[32]`, `BindingPoints[32]`, server-owned `ContentSerial` bumped on every applied
  create, including a re-create on the same handle.
- `set_vertex_buffers` — `PipeCalls.def:168` (`kCtxState`, `kVarTail`), emitter
  `VertexInputEmit.h:200-247`, record `MGPVertexBuffers` / `MGPVertexBuffer`
  (`MGPipeTypes.h:805-836`). Applier: `VertexBuffers[32]`, `VertexBufferStart/Count`,
  `VertexFetchBaseInstance`, `VertexBuffersSerial` (`PipeApply.h:598-616`), bumped at
  `PipeApply.cpp:2376-2377`.
- `set_index_buffer` — `PipeCalls.def:169`, emitter `VertexInputEmit.h:255-270`, record
  `MGPIndexBuffer` (`MGPipeTypes.h:839-845`). Applier `IndexBuffer` + `IndexBufferSerial`
  (`PipeApply.h:620-622`), bumped at `PipeApply.cpp:2391-2392`.
- `set_vertex_attrib_defaults` — `PipeCalls.def:177`, `PipeFill.cpp:2594-2660`; all three views
  verbatim since P5c rv, so `GetCurrentVertexAttribute` is RECORD_SUPPLIED.
- Client-side dirty rules: bit 8 `NewVertexElements` = `mix(lifetimeId, configVersion)`
  (`Tracker.h:294-298`); bit 9 `NewVertexBuffers` = `mix(mix(AnyVaoAttributeGeneration,
  vaoIdentity), pendingBaseInstance)` (`Tracker.h:542-543`); bit 10 `NewIndexBuffer` =
  `mix(vaoIdentity, mix(indexSlotVersion, boundLifetimeId))` (`Tracker.h:558-566`). All three emit
  at the validate point, order elements → buffers → index (`PipeFill.cpp:3225-3233`).
- Twin lifetime, already handle-shaped: `ReleaseTwinsForWireObjectDeath(handle,
  VertexElementsCso)` → `g_backendVertexArrayObjects.ReleaseByHandle(handle)`
  (`Managers.cpp:352-353`), plus the client's own `MGPipeEmitVertexElementsDestroyAndFree`
  (`PipeFill.cpp:1050`) and `NoteRecordDestroyed` (`VertexInputEmit.h:319-329`).
- `SyncToBackendFromApplier` (`Managers.cpp:5160-5350`) is the existence proof: the whole driver-VAO
  configuration is already produced from records with zero frontend reads, and its dirty gate is
  three server-owned serials plus the backend's own `g_bufferBackendIdGeneration`.

So the records cover the family in full **except** client-memory vertex arrays (§7).

## 3. The gap — what does not exist yet

**G1. Nothing.** For rows 1–9 and 11 there is no missing field: every value the server needs is
already in `MGPipeApplierState`. This family's P5e work is a re-key and a re-source, not a record
addition. That is the headline and it is what makes S1 cheap next to S2/S3/S5.

**G2. Client-memory vertex arrays have no wire form.** `EmitVertexBuffers` publishes
`Res = kMGPipeNullHandle` for them by design (`VertexInputEmit.h:196-199`, `:211-213`) and the
SERVER uploads the bytes by dereferencing the client pointer (`Managers.cpp:5372-5466`). **The
brief's "client-side arrays are already refused under split" is NOT confirmed — they are not
refused.** `EmitTables.cpp` refuses only client INDEX arrays for the multi-draw forms
(`:651` `CLIENT_INDICES`) and `CLIENT_COMMANDS` / `UNBOUND_PARAMETER` (`:682,:685`); nothing
refuses a client vertex array. Options, in P5e order:
  - **(a) refuse**, `Fatal{UnmigratedVerb, "DrawArrays+CLIENT_ARRAYS"}` raised on the GL thread in
    `EmitDrawRecord`'s array arm, gated on the run-ahead caps bit so the lockstep arm is unchanged;
  - **(b) stage**, a new tail on `draw_vbo` shaped exactly like `kDrawHasUserIndices`
    (`MGPipeTypes.h:1430`, codec `PipeWireCodec.cpp:1985-1998`): a per-attribute
    `{BindingIndex, MGHostSpan}` array staged whole from the draw's known `[first, first+count)`
    range. This is P8's `HostResolve.cpp` territory and is the honest answer.
  P5e should take (a) and name (b) as P8's, because (a) is two lines and makes visible a path that
  was never split-clean; (b) is a record and belongs with the index-array flattening it mirrors.

**G3. One pin, not a field.** The twin key below is only 1:1 while the vertex-elements CSO stays
IDENTITY-addressed (`VertexInputEmit.h:19-28`, D-G1). If P7 ever content-addresses it, two frontend
VAOs share a handle — and therefore one driver VAO name and its 64 scratch buffer ids
(`Managers.h:1204-1300`). P5e must state that as a requirement of the key and pin it in a test, not
leave it as a performance remark.

## 4. The retirement design

**4.1 The twin key is `st.BoundVertexElements`.** The handle `ResolveVaoTwin` resolves today is
`MGPipeSlots().Acquire(VertexElementsCso, vao->GetLifetimeId())` (`SlotTables.h:247-248`) — the
SAME handle `EmitVertexElements` mints (`VertexInputEmit.h:172`) and the applier stores as
`BoundVertexElements` (`PipeApply.h:594`). So:

```
BackendVertexArrayObject* ResolveVaoTwinByHandle() {          // split arm, no scope, no allocator
    const MGPipeHandle cso = MGPipeApplier().BoundVertexElements;
    if (MGPipeHandleIsNull(cso)) return nullptr;              // == "no VAO bound"
    auto& twin = g_backendVertexArrayObjects.GetOrCreate(cso); // SlotTables.h:294-340
    if (!twin) twin = MakeShared<BackendVertexArrayObject>();
    return twin.get();
}
```
`PrepareForDraw:4770-4772` stops reading `GetBoundVertexArray()`; the null arm at `:4816-4821`
(`BindBackendVAOId(0)`) is reached from the null handle instead. No behaviour change: one driver VAO
per frontend VAO, exactly as now. `GetOrCreate(handle)`'s backward-generation refusal
(`SlotTables.h:331-339`) is the ABA answer, and `ReleaseByHandle` (`SlotTables.h:394-408`) the death
answer — both already written and already called from `Managers.cpp:352`.

**4.2 The buffer walk reads the applier, not the VAO.** `SyncVaoAttributeBuffersByHandle`
(`:699-790`) keeps its shape and loses its frontend argument:
  - miss walk `:744-772` → iterate `i` in `[st.VertexBufferStart, +st.VertexBufferCount)`, skip
    `MGPipeHandleIsNull(st.VertexBuffers[i].Res)`, dedupe on `Res` (a `{slot,gen}` compare replaces
    the pointer compare at `:754-760`), `EnsureBufferResourceForHandle(nullptr, Res)`;
  - `entry.handle = Res` directly — `HandleOfBuffer` at `:769` deleted;
  - dirty repair `:735-736` → `EnsureBufferResourceForHandle(nullptr, entry.handle)`;
  - probe `:733` → `IsBufferDrawCleanByHandle(entry.handle, entry.resource, nullptr)`; the
    `frontend` parameter is already dead under a transport (`Managers.cpp:3148-3150`) and
    `ResolvedDrawBuffers::Entry::frontend` (`Managers.h:1223`) leaves the split arm, which also
    retires the dangling raw pointer the struct's own note warns about (`:1224-1239`);
  - `attribIndex` (`Managers.h:1241`) becomes the BINDING index, kept only for diagnostics.
  Memo key is unchanged and already server-owned: `{elementsHandle, elementsSerial, buffersSerial}`
  (`Managers.h:1263-1265`), i.e. `{BoundVertexElements, rec->ContentSerial, VertexBuffersSerial}`.

**4.3 The IBO arm.** `:896-925` → `st.IndexBuffer.Res` and `st.IndexBufferSerial`. The identity
compare `memo->iboHandle == iboHandle` stays (the handle is now read from the applier, not probed);
the extra `HandleOfBuffer` at `:907` goes. Add `iboSerial` to the memo so a rebind that moved the
serial re-ensures without a frontend read — the client shutter (`Tracker.h:558-566`) already fires
on the slot version AND the bound lifetime id, so a redundant rebind is correctly suppressed.

**4.4 The two config-version reads.** `:4775` is DEAD on this arm already (`SyncNeccessaryBuffers`
uses `vaoConfigVersion` only in the legacy branch, `:831-833`): delete it under the transport check.
`:1683` is the key of `PendingAttribValueMask` (`Managers.h:1286-1290`); re-key it on
`{elementsHandle, elementsSerial, activeMask}` and read `rec->Attributes[location].Enabled`
(`PipeApply.h:394`) at `:1688`. Every configuration change re-creates the record and bumps
`ContentSerial` (`PipeApply.h:396-399`), so the key is strictly stronger than the wrapping Uint16.

**4.5 Clean / needs-re-sync, answered from server-owned serials only.** After 4.2–4.4 the family's
four questions are: `BoundVertexElements` + `ContentSerial` (configuration), `VertexBuffersSerial`
(the buffer set), `IndexBufferSerial` (the element slot), `CurrentBufferMutationEpoch()` +
`syncedChangeSerial` vs `record->Serial` (buffer content, `Managers.cpp:3147-3153`). The epoch is a
backend atomic (`Managers.cpp:794`) bumped only from backend bodies, i.e. apply-thread memory under
a transport — that must be pinned, not assumed (§7).

**4.6 Scopes.** `DirectGLES.cpp:1609`, `:6729`, `:6775` are deleted with the sites they wrap; the
`HandleOfBuffer` scope entries this family reaches (`Managers.cpp:2938`) lose their last
vertex-input caller. After that, `GetBoundVertexArray` (`FieldOwnership.def:72`) has no Espryt
draw-path reader left except the two client-array arms and the restart substitution (§4.7), so its
row's retiring phase moves from "P8" to "P5e (Espryt)".

**4.7 The restart substitution.** `BoundElementArrayBuffer()` (`:6447-6452`) →
`st.IndexBuffer.Res`; `BoundElementArrayBufferId()` (`:6456-6461`) →
`FindBufferResourceForHandle(st.IndexBuffer.Res)` (resolve only, exactly as
`SyncToBackendFromApplier:5330` does). `:6591`'s read in the substitution body takes the same.

## 5. Monolith / G1

Every edit above is inside an existing `#if MOBILEGL_PIPE_PUSH` arm that is already selected by
`VertexInputSubsystemEnabled()` / `ResourceSubsystemEnabled()` (`Managers.h:911-918`) — the
functions touched (`SyncVaoAttributeBuffersByHandle`, `SyncToBackendFromApplier`,
`BindAttributeBufferByHandle`) do not exist in the pull build at all. `ResolveVaoTwin` is the one
exception: its three arms (`:1611-1622` slot, `:1623-1636` legacy memo) stay, and the handle arm is
added as a fourth guarded by `Transport != Monolith`, so `MOBILEGL_PIPE_PUSH` under
`Transport=monolith` keeps `Find(vao.get())` byte for byte. `ResolvedDrawBuffers`'s `frontend` and
`configVersion` members stay declared for the legacy arm (they are already outside the
`MOBILEGL_PIPE_PUSH` block at `Managers.h:1223,:1251`), so no mangled name and no struct layout the
pull build sees is changed. G2/G14: no new name enters the pull build's symbol set; the new
`ResolveVaoTwinByHandle` is `MOBILEGL_BUILD_DISAGGREGATED`-only.

## 6. Red-once

1. **Twin key.** `SanityTest.cpp:4083-4084` already pins that `VertexElementsCso` resolves through
   the handle arm. Add: with the twin keyed on `BoundVertexElements`, revert `ResolveVaoTwin` to
   `Find(vao.get())` with the scope at `:1609` removed → `Fatal{RoleViolation, "MGPipeSlots"}`
   naming `HandleOf` (`SlotAllocator.cpp:22-38`). The scope deletion is what makes the revert loud.
2. **The buffer walk.** New `MG_Test` case beside `VertexInputEmit`: publish `set_vertex_buffers`
   for buffers A,B, then repoint the frontend VAO's attribute 0 at C WITHOUT re-emitting, then run
   the sync — the walk must ensure A,B. Revert 4.2 and it ensures C; the case names
   `st.VertexBuffers`.
3. **The index arm.** Same shape on `set_index_buffer` / `st.IndexBuffer.Res`, with the frontend
   element slot rebound after publish. Revert 4.3 → red naming `IndexBuffer.Res`.
4. **The attrib-value memo.** Revert 4.4's key to `GetConfigVersion()` under run-ahead and the
   strict lane aborts `Fatal{UnmigratedPipeInput, "GetBoundVertexArray@draw_vbo"}`
   (`PipeInputs.h:137-170`, `MGPipeStickyForwardPull`) — this is P5e-3's detector doing the work.
5. **Client arrays.** With §3 G2 (a) in place: a `glDrawArrays` with an enabled client-memory
   attribute under run-ahead is a named refusal, and reverting the refusal is red by
   `DoublePrecisionScenario` / a new client-array scenario under `integration-split-strict`.
6. **Integration, unchanged suites that must stay green under run-ahead:**
   `VertexAttribBindingScenario`, `VertexArrayEnableDisableScenario`, `PrimitiveRestartScenario`,
   `ResidentIndexScenario`, `MultiDrawScenario`, `DrawParametersScenario`, `DoublePrecisionScenario`,
   `HandleRecycleScenario`, `ObjectSubsystemControlScenario` (the `0x0ff` / `0x17f` A/B at
   `Managers.h:897-905` must keep its verdicts).

## 7. Risks and unsettled

- **R1 (blocking, owned by S6/S8).** Client-memory vertex arrays, §3 G2. Until refused or staged,
  the apply thread dereferences a raw client pointer per `glDrawArrays` and run-ahead makes the
  bytes movable. It also GENERATES and fills driver buffers on the apply thread
  (`Managers.cpp:5392-5399,:5455-5457`) — harmless, but it means `SyncClientSideAttributesForDrawArrays`
  cannot simply be deleted.
- **R2.** `CurrentBufferMutationEpoch()` (`Managers.cpp:794`, `Managers.h`'s site enumeration) is the
  only non-serial input left in 4.5. It is an atomic and every bump found is in a backend body, but
  P5e must pin "no GL-thread bump under a transport" — a client-side bump would make the memo's
  clean stamp a cross-thread read.
- **R3.** `SyncCurrentVertexAttributeValues` cannot go run-ahead clean on S1's work alone:
  `:1672`, `:1701`, `:1715` read the frontend program. S1's edits are a no-op for safety until S4
  lands. Sequence S4's program twin before or with this function's edit.
- **R4.** The `set_vertex_buffers` window: `Count` is "highest ENABLED attribute + 1"
  (`VertexInputEmit.h:203-207`) while `create_vertex_elements` carries all 32
  (`VertexInputEmit.h:352-366`). An attribute enabled in the record but outside the window resolves
  `VertexBufferForAttributeIndex == nullptr` (`Managers.cpp:4795-4809`) and is skipped at `:5262`.
  Unreachable today because both walks read the same `Enabled` bits in the same validate step; under
  run-ahead they still do (both are client-side, same step) — but it is the same class as S2's
  caveat A8 and should be pinned by a test rather than argued.
- **R5.** `EnsureBufferResourceForHandle`'s `hostBytes` refresh (`Managers.cpp:3370`) is a
  self-assignment under a transport (`:3217-3220` returns `resource->hostBytes`). Passing `nullptr`
  changes nothing — confirmed by reading, not measured. The fp64 narrowing
  (`SyncFloat64AttributeAsFloat32ByHandle`) reads `resource->hostBytes`, i.e. the server staged
  store, and is already server-owned.
- **R6.** `g_bufferBackendIdGeneration` (`Managers.cpp:5172-5174`) and the reset rule
  `MGPipeApplierReset` must ADVANCE `VertexBuffersSerial`/`IndexBufferSerial` rather than zero them
  — the long note at `Managers.cpp:5176-5191` says the twin's whole gate depends on it. Under
  run-ahead an `applier_reset` is a waited control record (P5e-2), so this holds; if any package
  makes `applier_reset` fire-and-forget, this gate breaks first.
- **R7, not settled.** Whether `ResolvedDrawBuffers` should survive at all once the walk is over
  `st.VertexBuffers[Start..+Count)`: that array is already a compact, deduped, handle-keyed list
  bounded by `Count` (1–3 entries on the Minecraft workload, not 32 cold slots). The memo may be
  pure cost after 4.2. Measure before keeping it.
