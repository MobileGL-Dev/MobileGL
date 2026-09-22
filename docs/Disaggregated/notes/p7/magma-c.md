# P7 wave 2 package C — Magma object death, resident sub-data, the reflection archive, the vertex-layout split

Base: `p7/magma-c` off `feat/disaggregated @ a686131e` (pipe HEAD at branch time).
Build: `build-split` = `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON`, ICD pinned to
`/usr/share/vulkan/icd.d/lvp_icd.json` (the same lavapipe ICD `~/w7/pipe/build-split` pins; the
worktree tool leaves `MOBILEGL_ITEST_VK_ICD` empty and CMake warns about it).

Contract rows: CONTRACT-P7 §5.5 (`StateObjectDeathOps`), §5.4 (OQ-10), §5.3 (OQ-8), §3.2
(`vertex-layout`, `vertex-format-conversion`). Discipline: rule I (no `std::abort()` on a Magma
wire arm), rule J / R-16 (every fix red once on a two-process arm first).

---

## Slice 1 — Magma `StateObjectDeathOps` (§5.5)

**What landed.** `MG_Backend/DirectVulkan/DirectVulkan.cpp` gains
`g_magmaStateObjectDeathOps` and `InstallStateObjectDeathOps()`, installed from
`BackendObject_DirectVulkan::Initialize()` — i.e. from `ServerLoop::CreateBackend`, step 1 of
`InitServerRoleCommon`, which both Magma server roles (inproc and the spawn/TCP child) run and
which is before any client session exists. Guarded `#if MOBILEGL_BUILD_DISAGGREGATED &&
MOBILEGL_PIPE_PUSH`, so the pull build compiles nothing new (G1).

**The one arm, and why it is only one.** Espryt's `g_glesStateObjectDeathOps` has two jobs: emit
the wire `object_death` record off the apply thread, and otherwise destroy a backend twin keyed
by lifetime id. Magma has no lifetime-id-keyed twin at all — its two client-minted kinds
(`VertexElementsCso`, `Buffer`) are keyed on `{slot, gen}` by `MagmaPipeIdentityTables` and the
other six are still reached from their frontend objects — so the Magma table is exactly the EMIT
arm and nothing else.

**Correction to the plan's wording (worth the integrator's attention).** Plan §1.3 and the task
brief describe the gap as a client SLOT leak provable with `PipeSlotPeek`. It is not, and has not
been since P4a: `MG_Impl/Pipe/PipeFill.cpp`'s `NotifyAndFree` frees the client slot
unconditionally and backend-neutrally, so `PipeSlotPeek` never saw a Magma leak and does not see
a change now. What leaked was the **server's twin**. A framebuffer has no wire delete opcode at
all (BRIEF-P4A D-I2), so `object_death` is its only death delivery; with no consumer installed
under Magma inproc, the server's record stayed `Live` for the life of the session. The
`CtWireScenario` framebuffer case now reads BOTH numbers — the server's `deaths` tally and the
client's `PipeSlotKind::Framebuffer` live count — so a future regression in either half is
attributable.

**Cleanup of the Magma special cases.** The two executable ones are the `CtWireScenario` skip
guards (`:153` / `:209`), both deleted. Everything else the audit counted is comment text, and it
is reworded rather than deleted because the underlying argument survives: `PipeSlotPeek.h`,
`MagmaPipeArms.h` (the age-sweep-instead-of-death-notice paragraph — still true, because the new
table never touches the per-renderer identity tables), `PipeMutation.h`, `PipeFill.cpp`,
`VertexArrayObject.cpp`, `DirectGLES/Managers.cpp`, `HandleRecycleScenario.cpp`,
`WireTables.cpp`.

**The install-order question, answered rather than assumed.** `SetStateObjectDeathOps` is a bare
last-writer-wins pointer with no stacking, so two installers could in principle race.
They cannot meet: Magma's runs only in a process that OWNS a DirectVulkan backend, and
`InstallClientWireTables`' arm runs only under `Transport == Spawn`, which is by construction the
process that owns no backend (`InitSplitRoles` builds a `BackendObject_Remote` there). The
red-once below measures exactly that split of responsibility.

**Proof (green).** `ctest -R "DirectVulkan\.(Split|Spawn|Tcp)\.Ct\..*Death"` — 6/6 pass
(`integration-magma-all-{split,spawn,tcp}`), plus the two `TcpServer` fixtures. DirectGLES
control `ctest -R "DirectGLES\.(Split|Spawn|Tcp)\.Ct\..*Death"` — 6/6 pass, unchanged.

**RED-ONCE (executed).** `BackendObject_DirectVulkan::Initialize()`'s call changed to
`if (false) InstallStateObjectDeathOps();`, rebuilt, lane re-run:

```
1/8 DirectVulkan.Split.Ct.CtWireScenario.TextureDeath...        ***Failed
2/8 DirectVulkan.Spawn.Ct.CtWireScenario.TextureDeath...            Passed
4/8 DirectVulkan.Tcp.Ct.CtWireScenario.TextureDeath...              Passed
5/8 DirectVulkan.Split.Ct.CtWireScenario.FramebufferDeath...    ***Failed
6/8 DirectVulkan.Spawn.Ct.CtWireScenario.FramebufferDeath...        Passed
7/8 DirectVulkan.Tcp.Ct.CtWireScenario.FramebufferDeath...          Passed
75% tests passed, 2 tests failed out of 8

CtWireScenario.cpp:264: Failure
Expected: (afterCounters.deaths) > (deathsBefore), actual: 0 vs 0
```

Restored and rebuilt: 8/8 green again. This is precisely the shape package L's temporary probe
predicted (`notes/p7/magma-two-process-first-run.md` §6): spawn and tcp were already carried by
`InstallClientWireTables`' emitter, and **inproc was the only hole** — which is the hole this
table closes.

---

## Slice 2 — OQ-10 `kCapResidentSubData` (§5.4)

**What landed.** `InitServerRoleCommon` ORs `kCapResidentSubData` when
`g_resourceOpsAtStep2->SubDataResident != nullptr` — the pointer that function already pinned at
step 2 and that step 5 already refuses to see swapped. Never from `ActiveBackendType` (ID-39).
`rsd=` joins the `emit[]` bracket on the PipeStats summary line, counted at
`MGPipeEmitBufferSubDataResident` once per record. `SplitRuntimePeek` gains
`residentSubDataCap`, read through the same `CapsMirrorInstance().HasCap()` accessor the
frontend's `MGPipeResourceOpsHaveSubDataResident` reads.

### BLOCKER FOR THE INTEGRATOR: §5.4's white-box case is not achievable at this tree

§5.4 asks for a case in `integration-magma-buffers` and one in `integration-split` proving that
the emitted record is `BufferSubDataResident` (opcode 49). **Neither can exist today, and the
reason is R-6 rather than anything this package did or did not do.** The chain:

| step | site | verdict under split |
|---|---|---|
| the resident emitter runs only from a resident store | `BufferObject.cpp` `UploadSubData` → `if (m_resource.IsGpuResident())` | — |
| residency comes only from `AdoptPersistentMap` | `PipeResource.h:181`, the only writer of `m_gpuMapped` | — |
| adoption needs a non-null `map_persistent` answer | `BufferObject.cpp` `TryAdoptLargeStorage` / `EnsureGpuResidentStorage` / `AcquireMemoryRange` | — |
| the answer is a decline | `PipeApply.cpp:2331` "**A SPLIT BUILD RUNS AT TIER T2 AND DECLINES EVERY ACQUISITION, ALWAYS**" | always |
| the tier gate has no other implemented value | `PersistentMapTracker.cpp:1033` `AdoptTierIsEmulate` — T0/T1 are P11's and are a named `Fatal{UnimplementedAdoptTier}` | always |

So under any transport no buffer is ever GPU-resident, `LandBytesIntoResidentStore` is
unreachable, and opcode 49 cannot cross. The capability is therefore wired **ahead of its
consumer**, deliberately: when P11 lands a tier that adopts, the bit is already correct and
already derived from the table, and `rsd=` is the counter that will show the records. The
plan's phrasing ("both backends fall back to the in-place memcpy at `BufferObject.cpp:~711-716`")
is one step optimistic — under split the code does not reach that fallback either, it takes the
plain shadow `Memcpy` in `UploadSubData` above it.

**What is proved instead, and it is both halves of the bit.**

- *Server half* — `CapsMirrorTest.MagmaTransportPublishesRealBufferConsumersWithoutRunAhead`
  (`RemoteClientTest.cpp`) now asserts
  `(mask & kCapResidentSubData) != 0  ==  (ops->SubDataResident != nullptr)`. The two sides are
  compared to each other rather than both to `true`, so the case also covers a backend WITHOUT
  the arm on the day one exists.
- *Client half* — the new `CtWireScenario.TheServerPublishesTheResidentSubDataCapabilityFromIts
  OwnTable`, registered through the `ctCase` list so tier 2 replays it: green on
  `DirectGLES.{Split,Spawn,Tcp}` (`integration-{split,spawn,tcp}`) and
  `DirectVulkan.{Split,Spawn,Tcp}` (`integration-magma-all-*`), 6/6.
- *Negative half* — `CapsMirrorTest.APlaceholderMirrorConsumesNothing` keeps the
  no-snapshot-withholds-the-bit reading, which stopped being vacuous the moment anything set it.

**RED-ONCE (executed, both levels).** `capBits |= MG_Pipe::kCapResidentSubData;` → `capBits |= 0;`:

```
RemoteClientTest.cpp:692: Failure
Expected equality of these values:
  (server.CallMask() & static_cast<Uint64>(kCapResidentSubData)) != 0
    Which is: false
    Which is: true

25% tests passed, 6 tests failed out of 8
  DirectGLES.{Split,Spawn,Tcp}.Ct....ResidentSubDataCapability...   ***Failed
  DirectVulkan.{Split,Spawn,Tcp}.Ct....ResidentSubDataCapability... ***Failed
CtWireScenario.cpp:262: Failure
Value of: runtime.residentSubDataCap
  Actual: false
```

Restored: 11/11 unit and 6/6 integration green again.

**One incidental finding, recorded because it cost a cycle.** The first cut of the capability
case did no GL work and failed on `ScenarioFixture.h:90` — an armed split lane refuses a case
whose encoder ordinal did not move, because "emitted nothing" and "resolved the transport and
then put nothing on the wire" are the same observation. The case now clears and reads back
first. This is the same F1 arming rule package L hit on tier 3.
