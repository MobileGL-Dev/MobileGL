# P5e package `id` — identity: handle-keyed twins, the guard, the two scopes (v1)

Branch `p5e/id`, base `2fde7034`, head **`0117d157`**, worktree `~/w7/p5e-id`. Five commits, no
attribution trailers. Lockstep behaviour is unchanged: `MGPipeBarriered` answers `true` for every
record until ra lands, so every site that must one day ask the question already asks it and every
answer is today's.

## 1 What changed, per file (ranges at HEAD)

| file | ranges | what |
|---|---|---|
| `MG_Pipe/PipeApply.h` | `747-763`, `813-844` | `MGPipeApplierState::CurrentRecordBarriered` (inside the existing `#if MOBILEGL_BUILD_DISAGGREGATED` block, so the push-monolith layout does not move); opaque `enum class MGPWireOp : Uint16;`; `MGPipeBarriered(MGPWireOp, const void*, const MGPipeApplierState&)`; `MGPipeApplierCurrentRecordIsBarriered()`. |
| `MG_Pipe/PipeApply.cpp` | `1215-1238` | both bodies. `MGPipeBarriered` returns `true` (today's rule restated, not a stub); the reader answers the field under split, the constant otherwise. |
| `MG_Remote/Server/PipeApplier.cpp` | `1058-1066` | `ApplyOne` stamps the flag from `MGPipeBarriered(...)` before `StampVerbBoundary`/`DecodeAndApply`. |
| `MG_Impl/Pipe/SlotAllocator.h` | `176-206`, `222-272` | `MGPipeApplierIsUnbarrieredApply()`, `MGPipeRefuseFrontendKeyedRegistryFromUnbarrieredApply()`; `MGPipeReverseAnnouncementScope` → `MagmaP7AllocatorDebtScope`; both scope comments re-derived for §4.4. |
| `MG_Impl/Pipe/SlotAllocator.cpp` | `14-22`, `26-86`, `105-140` | the guard (§2 below), the non-allocator refusal, the renamed depth. Adds `#include <MG_Pipe/PipeApply.h>` (declarations only). |
| `MG_Backend/DirectGLES/SlotTables.h` | `61-103`, `176-203`, `218-260`, `292-297`, `336-342`, `405-447`, `464-508`, `588-651`, `665-724`, `761-764` | the composite band (`m_band`, `kHasCompositeBand`, `EntryOrNull`/`IndexOfSlot`/`EntryAt`, `CompositeLiveCount` + two capacity readers); `ForEachLive` re-typed `fn(MGPipeHandle, const BackendPtr&)`; the frontend-keyed half kept as monolith glue, each surface a named Fatal. |
| `MG_Backend/DirectGLES/Managers.h` | `469-489`, `534-548`, `1428-1445`, `1840-1855`, `2579-2595` | `NoteStateForHandle`/`StateForHandle` move `BUILD_DISAGGREGATED` → `PIPE_PUSH`; `ForEachLive` handle-arm-only; the three resolver declarations. |
| `MG_Backend/DirectGLES/Managers.cpp` | `226-264`, `336-358`, `2966-2978`, `4919-4939`, `9160-9181`, `10842-10862` | death path (§3), §4.2/§4.3 written down, `HandleOfBuffer`'s scope, the three resolvers. |
| `MG_Backend/DirectGLES/DirectGLES.cpp` | `8847-8915` | the single `ForEachLive` caller adapted (ruling R2). |
| `MG_Backend/DirectVulkan/.../VulkanRenderer.cpp` | `4554-4559`, `4638-4640`, `8983-8995` | scope rename, Magma's three sites. |
| `MG_Impl/Pipe/ResourceTracker.h` | `720-728` | scope rename, Magma's fourth site. |
| `MG_Test/SanityTest.cpp` | `3413-3418`, `4941-5006`, `5087-5090` | `FakeShaderCsoSlotTable`; `ACompositeHandleDoesNotGrowTheOrdinaryTable`; its G2/G14 skip twin. |
| `MG_Test/Wire/RemoteClientTest.cpp` | `1861-1872`, `1887-1927` | `...IsAllowed` deleted; new `...FromTheApplyThreadIsFatalEvenAtTheOldDebtSites`. |

The three resolvers — `ResolveVaoTwin` / `ResolveTextureTwin` / `ResolveProgramTwin`, each taking
`MGPipeHandle` — are `ResolveSamplerCsoTwin`'s shape exactly: record first (a handle with no record
is a loud null, never an adopted slot), then `AdoptTwinByHandle`, then a twin if the slot is empty.
No frontend touch, no allocator probe, **and no sync** — which serials gate a VAO/texture/program
sync belongs to vi/tx2/pg. `ResolveProgramTwin` resolves through the band-aware
`PipeShaderCsoRecordForHandle` and adopts into `m_band`: after the rekey a composite's bind is the
ordinary path through it, which is why the band had to land in this package.

## 2 The guard and the two scopes (§4.4, ruling 12)

`MGPipeRefuseAllocatorFromApplyThread` exempts a probe **iff a named scope is open AND the record
being applied is barriered**. A named exemption is a debt the CLIENT'S WAIT pays for — behind a
barriered record the client is parked in `WaitForApplied` and its free list and `lifetimeId -> slot`
map stand still; behind an unbarriered one the same probe reads memory the client is concurrently
mutating, so the value is wrong by construction and `MOBILEGL_IPC_STRICT_ERRORS` is not consulted
(rule F). Every record is barriered this phase, so the guard reads as it did at `2fde7034`.
`MagmaP7AllocatorDebtScope` exempts only when `ActiveBackendType == DirectVulkan`; the depth itself
is **not** backend-keyed (the guard reads the kind once, at the decision — keying the counter would
make its meaning depend on a global a test can move between ctor and dtor, `m_counted`'s bug one
level out). `MGPipeRefuseFrontendKeyedRegistryFromUnbarrieredApply` is the non-allocator half, for
`NoteStateForHandle` / `StateForHandle`, which answer from a frontend `SharedPtr` without ever
calling the allocator; same `Fatal{RoleViolation, "MGPipeSlots"}` name, because it is one surface.

## 3 The death path, and ID-96 answered by measurement

The NoSession mailbox hop (`Managers.cpp:224-246` at base) and the scope at `:256` are deleted; that
arm ran `DestroyByLifetimeId`, i.e. a probe **and a `Free`** in the client's allocator from the apply
thread — not a read any barrier could make safe.

**ID-96: the set of fixtures that lose death delivery is EMPTY.** Method: a probe that aborted in
exactly the deleted branch (`EmitObjectDeathRecord` answered `NoSession` **and**
`ServerLoopInstance().Running()`) was run over the whole unit lane (2204 entries) and the whole
`integration-split` lane (111). It **never fired** (`grep -c P5E-ID-PROBE` = 0 and 0), both lanes
green. Analytically: the tree's only server role with no client session is `ServerLoopTest.cpp`'s
`ServerFixture` / `EglServerFixture`; its one case that lets a re-keyed frontend object die off the
apply thread, `ServerLoopEglTest.FrontendFramebufferDeathDeletesOnTheContextOwner`, is already
refused at its FIRST step by P5c's guard, so its `framebuffer.reset()` was unreachable at `2fde7034`
too. **Nothing rewritten, nothing deleted** — integrator to confirm at landing.

§4.2's death moment and §4.3's ABA answer are written into `ReleaseTwinsForWireObjectDeath`'s
comment: the ring ordering is the argument, the Gen compare is the proof (forward re-mints, a stale
handle resolves to null, a backward generation is refused), so even a skipped death is answered by
the forward-Gen reset alone and no wait is added at any delete. Pinned by
`AGenerationBehindTheLiveTwinIsRefusedRatherThanAdopted` and the band case's recycle clause.

## 4 Kimi audit rows

**List (a), scope construction sites.**
- *Retired*: `M:256` (deleted with the mailbox hop).
- *Re-homed*: `VulkanRenderer.cpp:4554, :4634, :8986` and `ResourceTracker.h:723` → the renamed
  `MagmaP7AllocatorDebtScope`. `M:2938` (`BufferImpl::HandleOfBuffer`) →
  `MGPipeFrontendKeyedRegistryScope` (see ruling R3).
- *Left barriered* (unchanged; now exempt only while the record is barriered), vi/sb/pg/tx2/fb's to
  delete with the probes they wrap: `D:1609 :1734 :2588 :3303 :4236 :4329 :4585 :4926 :4993 :5328
  :5689 :6166 :6729 :6775 :8837 :9161 :9216 :9695 :10029 :11884` (20) and `M:4544 :5882 :7087 :8481
  :8717 :9256 :9310 :9709 :10382 :10403 :12949 :13273 :13570` (13).
- *Correction to the audit*: list (a) omits `VulkanRenderer.cpp:4554/:4634/:8986`, which construct
  `MGPipeFrontendKeyedRegistryScope` at base; it attributes Magma's debt to
  `MGPipeReverseAnnouncementScope` alone. All four sites exist; none was "not found". Test sites
  `:1883`/`:1885` went with the deleted `...IsAllowed` case; `:1903` is untouched.

**List (b), frontend pointers held across records.** id owns two rows. `ST:156-167, ST:362-378`
(`Entry::stateRef`, `NoteStateForHandle`/`StateForHandle`) is **left, as named monolith glue** per
ruling 1 / §5.8, each surface now a named Fatal from an unbarriered apply; what *is* retired is the
row's worst form — `ForEachLive` no longer hands a frontend `SharedPtr` out of server memory on
every step (`D:8904`). `Managers.h:320-324` + the six registry globals (legacy `m_entries`, raw
address key) is left: it is the `MOBILEGL_PIPE_LEGACY_MEMOS` arm, unreachable under a transport.
Every other row in list (b) belongs to vi/sb/pg/tx2/fb/ra and is untouched here.

## 5 Red-once evidence (executed, reverted, verbatim)

**(1) §4.4 — the exemption stops exempting an unbarriered apply.** Green first (1/1 passed).
Revert = `if (MGPipeApplierCurrentRecordIsBarriered())` → `if (true)` in `SlotAllocator.cpp`:
```
0% tests passed, 1 tests failed out of 1
  2139 - RemoteGuards.AnAllocatorProbeInsideAnExemptionScopeFromTheApplyThreadIsFatalEvenAtTheOldDebtSites (Failed)
../MobileGL/MG_Test/Wire/RemoteClientControls.inc:139: Failure
Value of: DiedOfAbort(child)   Actual: false   Expected: true   (exited 0)
```
Restored → 1/1 passed.

> **A FALSE GREEN WAS FOUND AND FIXED HERE; other packages should expect the same trap.** The first
> version built `TextureObject2D tex(46)` *on the apply thread*. That constructor MINTS a resource
> handle, so the child aborted with the same `Fatal{RoleViolation, "MGPipeSlots"}` at the mint and
> passed with the guard reverted too. It now builds the texture and the table on the CLIENT thread
> and posts only the probe — the `MGL_TEXTURE_GUARD_TEST` shape.

**(2) §4.3 — skip the Gen reset in the ABA case.** Revert = delete
`if (entry.Live && entry.Gen != handle.Gen) entry.backend.reset();` from `GetOrCreate(handle)`:
```
../MobileGL/MG_Test/SanityTest.cpp:4932: Failure
  next  Which is: (ptr = 0x55884178bd80, value = 4-byte object <EE-FF C0-00>)   vs  nullptr
a successor at a recycled slot inherited its predecessor's twin
../MobileGL/MG_Test/SanityTest.cpp:5001: Failure
  recycled  Which is: (ptr = 0x57dcfaad5c50, value = 4-byte object <5A-00 00-00>)  vs  nullptr
a successor at a recycled composite slot inherited its predecessor's driver program
```
Restored → 2/2 passed.

**(3) the composite band (extra, beyond the task's two).** Revert = `EntryAt` indexes `m_slots`
unconditionally:
```
../MobileGL/MG_Test/SanityTest.cpp:4967: Failure
  table.OrdinaryCapacityForTest()  Which is: 983044   vs  0u
a composite slot grew the ORDINARY table - one program pipeline is ~40 MB of twin entries for a single live program
```
Restored → 1/1 passed. 983044 entries is the number the band exists to prevent.

## 6 Gate

`build` rc=0. **`unit` 2204 / 2204** (base 2203; the one new name is
`ACompositeHandleDoesNotGrowTheOrdinaryTable`, real + skip twin). **`isplit` 111 / 111** (exactly
base). `gens`: `gen_pipe --check/--self-test`, `gen_pipe_field_ownership --check/--self-test`,
`check_include_closure` (4 probes, 0 problems), `gen_pipe_dirty_surface --check/--self-test` all
rc=0; `check_doc_citations` prints `CONTRACT-P5.md:524: 'Core.cpp:39' -> ambiguous`, which is
**pre-existing** in a file this package does not touch. `strict`: 3% passed, 108 failed of 111 —
identical to base.

**Strict-lane markers, before/after** (first marker per entry; `split-logs` cleared before each
run; copies in `~/w7/p5e-logs/split-logs-{before,after}`):

| `field@verb` | BEFORE | AFTER | owner |
|---|---|---|---|
| `GetFramebufferBindingSlot@Clear` | 39 | 39 | fb |
| `GetBoundVertexArray@DrawArrays` | 14 | 14 | vi |
| `GetImageTextureBinding@BindImageTexture` | 13 | 13 | fb |
| `GetTextureUnitObject@Clear` | 9 | 9 | tx2 |
| `GetTextureObject@CopyImageSubData` | 6 | 6 | tx2 (barriered, §7 allowlist) |
| `GetProgramForDispatch@DispatchCompute` | 3 | 3 | pg |
| `ValidateProgramName@ShaderStorageBlockBinding` | 2 | 2 | pg (barriered, §7 allowlist) |
| *(88 entries have a private log; 2 carry no marker)* | 2 | 2 | — |

Unchanged is the expected result: id retires no BARRIER-PULLED field — it lands the spine the
per-family packages retire them with. The table is richer than `STRICT-BASELINE-2fde7034.md`'s
22-entry slice (88 entries now carry a private log) and agrees with it on every shared field, so it
supersedes rather than contradicts it.

**G1 / monolith.** 0/0/0/0 **by construction, not measured**: every new symbol is inside
`#if MOBILEGL_PIPE_PUSH` or `#if MOBILEGL_BUILD_DISAGGREGATED`, `CurrentRecordBarriered` sits inside
the applier state's existing `BUILD_DISAGGREGATED` block, and the `TwinRegistry` alias is untouched.
The p5e gate configures the split flavour only, so neither the pull build nor the `PIPE_PUSH=ON,
BUILD_DISAGGREGATED=OFF` push-monolith build was compiled here. The one deliberate widening in that
direction is `NoteStateForHandle`/`StateForHandle` moving from `BUILD_DISAGGREGATED` to `PIPE_PUSH`
— availability added, never removed — because the re-typed `ForEachLive`'s caller needs them there.

## 7 Seams assumed from c0e

- `MGPipeBarriered(MGPWireOp op, const void* payload, const MGPipeApplierState& st)` and
  `MGPipeApplierCurrentRecordIsBarriered()`, both declared in `MG_Pipe/PipeApply.h`. **c0e owns the
  final declaration**; the integrator resolves the textual overlap at landing (one hunk).
- Two things c0e may spell differently: (a) `Bool CurrentRecordBarriered = true;` on
  `MGPipeApplierState` is the storage the predicate reads and `ApplyOne`'s "one line" writes;
  (b) `MGPWireOp` is **opaquely forward-declared** (`enum class MGPWireOp : Uint16;`) rather than
  pulled in by including `MGPipe.h`, which would have moved that header's include closure.
- My three resolver spellings win per the task. Nothing else is assumed: `MGPipeWaitClassFor`,
  `kCapRunAheadApply`, `kDrawClientArrays` and c0e's other rows are unreferenced here.

## 8 Rulings I need

1. **Range widening.** BRIEF-P5E gives id `Managers.h:309-640` and `Managers.cpp:188-372`. The three
   resolvers are declared beside each family's registry `extern` (`Managers.h:1428, 1840, 2579`) and
   defined beside each registry global (`Managers.cpp:4919, 9160, 10842`), so they read exactly like
   `SamplerImpl::ResolveSamplerCsoTwin` instead of needing three forward declarations at `:640`.
2. **`DirectGLES.cpp:8847-8915` is outside id's scope** (fb's `:8837-8935`). Re-typing `ForEachLive`
   is deliverable 1 and its one caller had to move in the same commit or the build breaks; the
   lambda takes `(MGPipeHandle, twin)` and calls `StateForHandle(fbo)` inside the scope already open
   at `:8837`, which fb replaces with its reverse index. Confirm or re-land in c0e.
3. **"kept under the old scope name"** (TASK-id deliverable 4, `Managers.cpp:2933`): I read "the old
   scope" as the *surviving* one, `MGPipeFrontendKeyedRegistryScope`. The other reading — leave
   `HandleOfBuffer` under the renamed Magma scope — would abort Espryt on every indexed draw, since
   that exemption is now DirectVulkan-keyed. Confirm.
4. **Names beyond BRIEF-P5E's G2/G14 list for id** (all `PIPE_PUSH` / `BUILD_DISAGGREGATED`):
   `MGPipeApplierIsUnbarrieredApply`, `MGPipeRefuseFrontendKeyedRegistryFromUnbarrieredApply`,
   `MGPipeApplierState::CurrentRecordBarriered`, `BackendSlotTable::CompositeLiveCount` /
   `OrdinaryCapacityForTest` / `CompositeCapacityForTest` (last two test-only). Extend the list?
5. **`StateBackendObjectRegistry::ForEachLive` is handle-arm-only now** — the legacy map fallback is
   deleted (a frontend-address key has no handle to hand over, and its one caller has always had its
   own `#if MOBILEGL_PIPE_LEGACY_MEMOS` walk beside the call). A narrowing, not a lost walk.
6. **ID-96's empty answer** (§3) — confirm at landing.

## 9 What I did not do

- **`SanityTest.cpp:3499-3960, :4061, :4241, :4378` were NOT moved onto the legacy arm or rewritten
  by handle.** Deliverable 6 inherits that from scout-S7 §6.4, which assumed S7's design (the
  frontend-keyed members *deleted*). Ruling 1 / §5.8 keeps them as monolith glue, so those cases
  still compile, still exercise the surface they name, and run on the test's own thread where no
  guard can fire; moving them would be churn against a base-green count with no gate behind it.
  Flagged rather than done — a mechanical follow-up on request.
- No device work; no strict-lane allowlist edit (`test.yml` is ra's); no `docs/` or ROADMAP row (c0e
  owns `CONTRACT-P5E.md`). The pull and push-monolith builds were not compiled here (§6).
