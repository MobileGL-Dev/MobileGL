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
