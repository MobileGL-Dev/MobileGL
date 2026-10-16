# Package id — identity: handle-keyed twins, the guard, the scopes (P5e; lands second)

Read in this order, all of it: `PACKAGE-PREAMBLE-P5E.md`, `INTEGRATOR-DECISIONS-P5E.md` (ID-90 and
ID-96 are yours), `BRIEF-P5E.md` (§0, §1, §2 "id", §6 rulings 1, 4, 12, 13), `CONTRACT-P5E-draft.md`
§4 (all of it) and §9, your scout report `scout-S7.md` (all of it), and the rows of `kimi-audit.md`
lists (a) and (b) (the scope construction sites and the frontend pointers held across records).
All in `//wsl.localhost/Arch/home/swung/w7/notes/p5e/`. Your slug is `id`; your tree is `~/w7/p5e-id`
(built, on branch `p5e/id` at `2fde7034`).

Package c0e is being implemented IN PARALLEL on `p5e/c0e` by another agent; it declares
`MGPipeApplierCurrentRecordIsBarriered()` and `MGPipeBarriered(...)` in `MG_Pipe/PipeApply.h`. You do
not wait for it: declare exactly those two names yourself in the same header with the contract's
signatures (a minimal body: barriered = true for every record until ra lands - lockstep is unchanged
by your package), and note in the report that c0e owns the final declaration; the integrator resolves
the textual overlap at landing (one hunk).

## Deliverables (lockstep behaviour unchanged; the registries stop being frontend-keyed on the server)

1. `MG_Backend/DirectGLES/SlotTables.h`: the composite band `m_band`; `ForEachLive` re-typed as
   `fn(MGPipeHandle, const BackendPtr&)`; the frontend-keyed members (`HandleOf(stateObj)`,
   `Find(StateObject*)`, `GetOrCreate(const StatePtr&)`, `stateRef`, `NoteStateForHandle`, the mutable
   memo) kept ONLY as monolith glue, each asserting `!MGPipeApplierIsUnbarrieredApply()` in split
   builds (a named Fatal, not a silent path).
2. `Managers.h:309-640` and the resolvers: `ResolveVaoTwin(MGPipeHandle)`, `ResolveProgramTwin(MGPipeHandle)`,
   `ResolveTextureTwin(MGPipeHandle)` in the shape of `SamplerImpl::ResolveSamplerCsoTwin`
   (`Managers.cpp:13457-13486` at base): handle in, record first, `GetOrCreate(handle)`, no frontend
   touch, no allocator probe; `AdoptTwinByHandle` as the shared body. The family packages will call
   these; if c0e's seam declarations differ in spelling, yours win for these three (tell the report).
3. `Managers.cpp:188-372`: teardown; `OnFrontendStateObjectDestroyed` — delete the NoSession mailbox
   hop (`:224-246`) and the scope at `:256`; `ReleaseTwinsForWireObjectDeath` by handle; the twin
   death moment per CONTRACT-P5E §4.2, the ABA answer §4.3 (a recycled slot with a new gen re-mints;
   an old-gen handle is refused by `FindByHandle`'s gen compare — pin it).
4. `MG_Impl/Pipe/SlotAllocator.{h,cpp}`: the guard `MGPipeRefuseAllocatorFromApplyThread` exempts
   only "inside a scope AND the current record is barriered" (contract §4.4); `MGPipeFrontendKeyedRegistryScope`
   stays as a class (the family packages delete their 20 sites); `MGPipeReverseAnnouncementScope` →
   `MagmaP7AllocatorDebtScope`, backend-kind-keyed (ruling 12), with the four Magma sites renamed
   (`VulkanRenderer.cpp:4554, 4634, 8986`, `ResourceTracker.h:723` at base — re-resolve) and Espryt's
   `HandleOfBuffer` site (`Managers.cpp:2933` at base) kept under the old scope name until vi/sb retire
   it (say so in the report). P5d round 3 already made the scope counters plain (apply-thread-only
   writers) — keep that shape.
5. `MG_Remote/Server/PipeApplier.cpp` `ApplyOne`: set `CurrentRecordBarriered` from `MGPipeBarriered`
   (one line; ra rebases on it).
6. Tests: `MG_Test/SanityTest.cpp` cases the brief lists (moved to the legacy arm or rewritten by
   handle; new `ACompositeHandleDoesNotGrowTheOrdinaryTable`); `MG_Test/Wire/RemoteClientTest.cpp`
   (`...IsAllowed` deleted; new `...FromTheApplyThreadIsFatalEvenAtTheOldDebtSites` posting `HandleOf`
   through `RunOnApplyThread` with the barriered flag forced false by a test hook). ID-96: enumerate
   every NoSession server-only fixture that loses death delivery when the hop is deleted
   (`grep RunOnApplyThread MG_Test/Wire`), rewrite them to drive `object_death` by handle through
   the applier, delete only what cannot be expressed that way, and list each in the report.

## Red-once (execute, revert, record)
With the test hook forcing "unbarriered", `HandleOf` from the apply thread → `Fatal{RoleViolation, "MGPipeSlots"}`;
put the exemption back → the case is green → red by expectation. Skip the Gen reset in the ABA
case → red.

## Gate
`build`, `unit`, `isplit`, `gens`, `strict` — unit and isplit must be exactly base-green (2203 /
111 at base; new names only add). G2/G14: skip twins for split-only cases. Commit in slices
(tables; resolvers; death path; guard + scopes; tests). Report per the preamble.
