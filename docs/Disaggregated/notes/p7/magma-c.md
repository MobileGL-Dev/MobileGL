# P7 wave 2 package C — Magma object death, resident sub-data, the reflection archive, the vertex-layout split

## For the integrator, first

Nine things this package changes that live outside its own files or outside its own slices.
`CONTRACT-P7.md` is the integrator's file (§0 "怎么改"), so none of these edit it.

1. **§12's debt row is retired.** "Magma 两进程 client 在 `StateObjectDeathOps` 落地前
   `ObjectDeaths=0`；`CtWireScenario` 的死亡用例在 Magma 臂按名 skip" — the table landed, both
   skip guards are deleted, and the two cases are green on `DirectVulkan.{Split,Spawn,Tcp}`.
   §2.3's "Magma 自己的 `StateObjectDeathOps` 表（wave 2-C）落地前…按名 skip" can go with it.
2. **§3.1's pinned census moves: 15 sites / 4 files → 13 / 3.** `Renderer/WireDraw.inc` leaves
   the list entirely. `scripts/ci/fatal_census.py` is unchanged (79 / 43 / 3 / 0) — the funnels
   were already converted in wave 0, so retiring a `MagmaWireFatal` CALL moves no abort count.
3. **§3.2 says "the other six" for `vertex-layout`; there are seven.** `offset-overflow`
   post-dates the audit. It is masked with the rest; the reasoning is in slice 4 below.
4. **§5.4's white-box case (opcode 49 on the wire) is not achievable at this tree**, and the
   blocker is R-6, not this package. The full chain is in slice 2. The capability is wired
   ahead of its consumer on purpose; `rsd=` on the PipeStats line is what will show the records
   the day P11 lands an adoption tier.
5. **§5.3's `g_programResourceCaches` deletion is achieved in the disaggregated build only.**
   The archive member has to be `#if MOBILEGL_BUILD_DISAGGREGATED` for G1, so the pull build
   keeps the cache. One-line change the day that pin is retired. P13.
6. **A family-word question.** `buffer-window` is now a permanent protocol error going through
   `MagmaWireFatal`, whose family is `UnmigratedVerb`. The honest word is `ProtocolCorruption`;
   `MG_Pipe::MGPipeFatalFamily` carries only two words and its own comment says a third is
   where the choice gets argued. §3.3 keeps the funnels on `UnmigratedVerb` deliberately so the
   refusal census stays comparable to P5b. Flagged, not decided.
7. **Gating lane counts grow** (grow-only, G14-legal): `integration-magma-split` 73 → 75,
   `-spawn` 52 → 54, `-tcp` 54 → 56, `integration-split` 186 → 187, `integration-spawn`
   102 → 103, `ctest -L unit` 2402 → 2403.
8. **One ratchet symbol changes bucket** without changing the total (186, monotone green):
   `p3b-p4b-espryt` 14 → 13 and `both-backends` 60 → 61, because slice 3's monolith consumer
   makes `DirectVulkan.cpp` reference a frontend symbol only `DirectGLES` referenced before.
   `p7-magma` — P7's own target bucket — is unchanged at 101. Nothing in this package was
   aimed at gate 4; the §4.2 path to 101 → 0 is package B's `(B')`.
9. **The worktree tool leaves `MOBILEGL_ITEST_VK_ICD` empty** and CMake warns about it; every
   Magma entry then runs whatever ICD the loader finds first. This package reconfigured
   `build-split` with `/usr/share/vulkan/icd.d/lvp_icd.json`, the pin `~/w7/pipe/build-split`
   already carries. Worth adding to `p7_worktree.sh` so the next package does not have to
   notice it.


Base: `p7/magma-c` off `feat/disaggregated @ 4f2d4c4e` (pipe HEAD at branch time).
Build: `build-split` = `-DMOBILEGL_BUILD_DISAGGREGATED=ON -DMOBILEGL_BUILD_DISAGGREGATED_INPROC=ON
-DMOBILEGL_PIPE_PUSH=ON -DMOBILEGL_BUILD_INTEGRATION_TEST=ON`, ICD pinned to
`/usr/share/vulkan/icd.d/lvp_icd.json` (the same lavapipe ICD `~/w7/pipe/build-split` pins; the
worktree tool leaves `MOBILEGL_ITEST_VK_ICD` empty and CMake warns about it).

Contract rows: CONTRACT-P7 §5.5 (`StateObjectDeathOps`), §5.4 (OQ-10), §5.3 (OQ-8), §3.2
(`vertex-layout`, `vertex-format-conversion`). Discipline: rule I (no `std::abort()` on a Magma
wire arm), rule J / R-16 (every fix red once on a two-process arm first).

## Gates, on the four slices together

| gate | base (`4f2d4c4e`) | after | |
|---|---|---|---|
| `ctest -L unit` | 2402 | **2403 / 2403** | +1 = the OQ-8 order case |
| `integration-magma-split` | 73 (71 P + 2 S) | **75 / 75, 0 skipped** | +2 vertex-layout; the 2 skips were the death cases |
| `integration-magma-spawn` | 52 (50 + 2) | **54 / 54, 0 skipped** | |
| `integration-magma-tcp` | 54 (52 + 2) | **56 / 56, 0 skipped** | |
| `integration-split` | 186 | **187 / 187** | +1 = the OQ-10 capability case |
| `integration-spawn` | 102 | **103 / 103** | |
| `integration-magma-full-split` | 453 P / 57 S / 0 red | **513 entries, 0 red, 55 skipped → 458 P / 55 S** | better on both counts |
| `scripts/ci/fatal_census.py` | 79 / 43 / 3 / 0 | **79 / 43 / 3 / 0** | unchanged, as expected |
| `scripts/ci/spawn_lane_parity.py` | green | **green** | all tiers equal after normalisation |
| `grep -rn "@P7"` | 15 sites / 4 files | **13 / 3** | `WireDraw.inc` leaves the list |
| `link_ratchet.py --assert-monotone` | 186 | **186, "unchanged"** | green |
| **G1** (`build-linux`, pull) | `.text` `0xa52203` | **`.text` `0xa52203` (10822147, +0)** | `.data`/`.bss`/`.rodata`/total all +0; 27837 → 27837 defined symbols, **0 added / 0 removed / 0 resized / 0 renamed**; the `.so` is byte-identical on disk (19143824) |

**Ratchet buckets.** AFTER: `p7-magma` **101**, `p3b-p4b-espryt` **13**, `both-backends` **61**,
`p6-core` **11**, total **186**. CONTRACT-P7 §4.1 records the `8bb6309a` baseline as
101 / 14 / 60 / 11. So one symbol sits in `both-backends` that the baseline recorded under
`p3b-p4b-espryt`; the total, the monotone assertion and **P7's own target bucket (101)** are all
unchanged. HONESTY NOTE: I did not separately re-measure the buckets at `4f2d4c4e`, so I cannot
attribute that single move to this package rather than to the two commits between the baseline
and this branch point. Nothing here was aimed at gate 4 — §4.2's path from 101 to 0 is package
B's `(B')`. (An attempt to identify the mover by rebuilding one object at the base failed to
compile and was abandoned; the clean way is `--bucket --verbose-buckets` on a base build.)

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

---

## Slice 4 — the `vertex-layout` split and `vertex-format-conversion` (§3.2)

**What landed.** `BuildWireVertexInput` answered eight conditions with one `false`, and the
caller answered that with a P7-marked `MagmaWireFatal` — a session kill where the monolith arm
masks the attribute out and declines the draw. Split by what the reason is ABOUT:

| reason | before | after |
|---|---|---|
| `buffer-window` | `false` → P7-marked Fatal | `false` → named Fatal `Magma:vertex-layout-buffer-window` (no P7 marker: it is a permanent protocol error, not an unmigrated shape) |
| `attribute-shape`, `native-fp64-format`, `format-map`, `native-format-feature`, `converted-format-feature`, `element-size`, `offset-overflow` | `false` → Fatal | `MGLOG_E_ONCE` + `unsupportedAttribMask \|= 1<<loc` + `continue`, then the monolith arm's own pre-flight in `WireDraw.inc` declines the draw when a masked bit is one the program reads |
| `vertex-format-conversion` | P7-marked Fatal | `MGLOG_E_ONCE` + `return false` (decline, rule I (a)) |

`unsupportedAttribMask` now enters the wire layout hash, for the reason the monolith entry's
hash already carries it: two VAOs can otherwise produce identical bindings/attributes/divisors
and differ only in which enabled attribute was masked, and a blind hash would serve one the
other's pipeline. It was safe to omit only while the mask was always zero.

**Count correction.** CONTRACT-P7 §3.2 says "the other six". There are SEVEN non-`buffer-window`
return sites in the tree: `offset-overflow` post-dates the audit's count and is an arithmetic
guard rather than a shape one. It is masked with the rest — an attribute whose base offsets
cannot be added is one Vulkan cannot fetch, the monolith arm never computes that sum at all,
and masking is strictly safer than a Fatal for a number no application can reach on purpose.

**Family-word question for the integrator.** `buffer-window` now goes through `MagmaWireFatal`,
whose family is `UnmigratedVerb`. The honest word for "the record's own numbers are impossible"
is `ProtocolCorruption`. `MG_Pipe::MGPipeFatalFamily` carries only `UnmigratedVerb` and
`RoleViolation` and its own comment says a third "costs a row here and an arm in the adapter -
which is the point, because that arm is where the choice gets argued". Not taken here: it is a
cross-package edit (`MG_Pipe/PipeSessionFail.h` + the `MG_Remote` adapter) and §3.3 records that
the three Magma funnels keep `UnmigratedVerb` deliberately, because renaming invalidates the
refusal census kept since P5b. Flagged rather than decided.

**Census.** `grep -rn "@P7" MobileGL/ --include=*.cpp --include=*.inc --include=*.h`:
**15 sites / 4 files → 13 sites / 3 files.** `Renderer/WireDraw.inc` leaves the list entirely
(both of its marked refusals are retired). `scripts/ci/fatal_census.py` is unchanged at
`79 abort sites over 20 files, 43 distinct family words, 3 refusal words, 0 unmarked` — the
funnels were already converted in wave 0, so retiring a `MagmaWireFatal` call moves no
`std::abort()` count. (Note for anyone editing near this row: the census counts the marker
literally, so a COMMENT that spells it raises the number. The first cut of this slice pushed
15 → 16 that way.)

**New scenario.** `Scenarios/MagmaVertexLayoutScenario.cpp`, DirectVulkan only — Espryt hands
`GL_FIXED` to a driver that supports it, so the same case under the same name would assert a
different fact there. `DataType::Fixed32` has no arm in `ToVkVertexFormat` at all, which is
what makes it the one unmappable shape reachable on host lavapipe; CONTRACT-P7 §2.5 records
that no other P7-marked refusal is (they need real hardware), so without this shape the
retirement would have had no host gate.

- `AnUnmappableVertexFormatDeclinesTheDrawInsteadOfEndingTheSession` — the program READS the
  `GL_FIXED` array, so the draw declines and the framebuffer keeps the clear colour.
- `AnUnreadUnmappableArrayDoesNotStopTheRestOfTheDraw` — the array is enabled and unmappable
  but unread, so the draw lands. This is what stops the first case being satisfied by
  "decline every draw that mentions GL_FIXED".

Registered on `DirectVulkan.{Split,Spawn,Tcp}.VertexLayout.` AND picked up by the ambient
monolith `DirectVulkan.` discovery, so the A/B is direct: the same two assertions on the wire
arm and on the monolith arm.

**Proof (green).** 8/8 — monolith `DirectVulkan.` ×2, `Split` ×2, `Spawn` ×2, `Tcp` ×2.

**RED-ONCE (executed).** `format-map` put back on the protocol verdict
(`return reject("format-map")`):

```
70% tests passed, 3 tests failed out of 10
#4238 DirectVulkan.Split.VertexLayout...Unmappable...  Subprocess aborted***Exception
  MGPipe: Fatal{UnmigratedVerb, "Magma:vertex-layout-buffer-window"}
#4240 DirectVulkan.Spawn.VertexLayout...Unmappable...  Subprocess aborted
#4242 DirectVulkan.Tcp.VertexLayout...Unmappable...    Subprocess aborted
#3408 DirectVulkan.MagmaVertexLayoutScenario...Unmappable...   Passed   <- monolith control
```

The three split arms KILL THE PROCESS and the monolith arm stays green, which is exactly the
divergence this slice closes. Restored: 10/10.

**Lane arithmetic.** The three new `Split` entries carry `integration-magma-split`, so that
gating lane grows 73 → 75 (and `-spawn` 52 → 54, `-tcp` 54 → 56). Growth only; G14's name
sets are grow-only by rule.

---

## Slice 3 — OQ-8: one reflection archive serves the storage-block consumers (§5.3)

**What landed.**

- `LinkArtifacts` gains `Vector<StorageBlockReflection> storageBlocks` (`{name, binding,
  dataSize}`), **`#if MOBILEGL_BUILD_DISAGGREGATED` only** — see G1 below.
- `ProgramLinkTask::SnapshotStorageBlockIndexSpace()` fills it in phase A, right after
  `SnapshotGlslangReflection`.
- Its `VisitFields` row and its own three-field table join the codec;
  `kProgramArtifactsCodecVersion` 2 → 3; `ProgramArtifactsSchemaFingerprint()` and therefore
  `wireFingerprint` move with them, which is expected and is what makes a mixed-version pair
  refuse at the handshake rather than at the first program.
- `MagmaProgramSource::EnsureStorageBlocks` is **deleted**, with `m_storageBlocks`,
  `m_storageBlocksReady`, the local `StorageBlock` struct and `#include <spirv_reflect.h>`.
- `DirectVulkan.cpp`'s monolith consumer reads the archive too, under `#if` symmetry.

**Why phase A and not phase B.** The obvious home is where the SPIR-V exists, and there is
exactly one function that sees both finished halves — `ProgramSpirvTask::RunBody`'s L1 cache
insert — but its writes reach only the TRANSLATION CACHE. Phase A's `artifacts` has already
been moved into the `ProgramObject` by then (`ProgramObject.cpp`'s `JoinPendingLink`), and
phase B is structurally forbidden from touching it. A field filled there would therefore be
present on a cache HIT and absent on every cold link — the worst of both, and invisible until
a second identical program linked. So the fill is glslang-derived, in phase A, and the
equivalence below is what makes that safe.

**The equivalence is measured, not asserted.** The consumers speak the index space
SPIRV-Reflect produces, and the archive now produces it from glslang's reflection instead.
`ProgramTest.TheArchivesStorageBlockOrderIsTheOneSpirvReflectProduces` runs the OLD algorithm —
SPIRV-Reflect over the real modules, sorted by (normalised name, binding), deduplicated across
stages — over a multi-stage program with an arrayed SSBO and an atomic-counter block, and
compares element for element. Measured reference on this tree:

```
{ "Blocks", "VertexOnly", "gl_AtomicCounterBlock_0", "FragmentOnly" }
```

which exercises all three of the cases that could have diverged: the instance array collapses
to one entry (glslang reflects per element, SPIR-V carries one descriptor with a count), the
synthesized atomic-counter block IS in the backend's space although GL's own
`GL_SHADER_STORAGE_BLOCK` enumeration excludes it, and `FragmentOnly` lands last because the
cross-stage dedupe walks stages in linked order.

**MEASUREMENT — `spvReflectCreateShaderModule` on the Magma wire draw path, per frame.**
Same binary, same workload, same window (`frames=1 window=1 draws=1 acc=18`, identical bytes),
`DirectVulkan` × inproc, `StorageBufferRegrowScenario` + the pipeline storage-block case, with
a temporary `MGLOG_I("OQ8_REFLECT_CALL")` at the call site:

| | reflect calls | frames | draws |
|---|---|---|---|
| BEFORE (`HEAD`'s per-draw `EnsureStorageBlocks`) | **4** | 1 | 1 |
| AFTER (reads the archive) | **0** | 1 | 1 |

The AFTER number is 0 structurally, not incidentally: the call site is gone and
`MagmaProgramSource.h` no longer includes `spirv_reflect.h` at all, which is the gate a future
edit would have to break on purpose. (A `MagmaProgramSource` is a borrowed view constructed
once per wire draw, so its `mutable` memo was cold every time it was read — that is why the
per-draw cost existed at all.)

**The monolith half: DONE, under `#if` symmetry — with one caveat the integrator should see.**
The task allowed deferring this if it could not be done without moving pull `.text`. It can,
and it is: `GetShaderStorageBlockIndex` / `GetShaderStorageBlockBinding` /
`ClearProgramResourceCaches` and the block-binding setter each have a disaggregated arm that
reads the archive and a `#else` arm that is the previous code verbatim. In the disaggregated
build that retires `g_programResourceCaches`, `ProgramResourceCache`, `StorageBlockResource`,
`BufferVariableResource`, `AddBufferVariablesRecursive`, `NormalizeDescriptorName`,
`GetProgramResourceCache` **and the rehash hazard** — the open-addressed-map dangling reference
that was a reproducible segfault in `ProgramPipelineScenario`'s two storage-block cases.

Two things are worth stating plainly:

1. **The pull build keeps all of it.** The archive member is disaggregated-only because one
   more `Vector` in `LinkArtifacts` changes its size, its implicit destructor and its move
   constructor, and G1 pins the pull build's `.text` byte for byte. So §5.3's "`g_programResource
   Caches` 随之删" is achieved in the disaggregated build and NOT in the pull one. Retiring the
   pull half is a one-line change (drop the guard on the member) the day that pin is retired or
   the member is moved — a P13 / integrator call, not a wave-2 one.
2. **Nothing outside `DirectVulkan.cpp` read any of the retired machinery.** Measured:
   `BufferVariableResource`, `bufferVariables`, `activeVariables` and `StorageBlockResource`
   have no reference anywhere else in the tree, and `dataSize` was written and never read —
   `GL_BUFFER_VARIABLE` and `GL_ACTIVE_VARIABLES` are answered by
   `MG_Impl/GLImpl/Program/ProgramInterface.cpp` out of glslang's own reflection, which is a
   different index space. So the two arms differ in exactly the two functions their callers
   use and in nothing else.

One genuine behaviour improvement rides along: the monolith `GetShaderStorageBlockBinding` now
asks `GetShaderStorageBlockBindingOverride` on every read, over an immutable list, where it
used to read a binding patched into a mutable cache. Same answer by a shorter route, and a
rebind can no longer be lost to a cache rebuild that raced a state-version bump.

**Proof (green).** `ctest -L unit` 2403/2403 (2402 before this slice; +1 is the new case).
`DirectVulkan.*(ProgramPipeline|StorageBufferRegrow)` 60/60 — the **monolith** `DirectVulkan.`
arm and the `Split` / `Spawn` / `Tcp` arms together, which is what says both consumers were
switched.

**G1 after the archive change:** `.text` `0xa52203` = 10822147 → 10822147 (+0), 27837 → 27837
defined symbols, **0 added / 0 removed / 0 resized / 0 renamed**, and the file is byte-identical
on disk (19143824 bytes both sides). The `#if` guards did their job; an unguarded call site
caught it once mid-slice (the pull build failed to compile, which is the loud version).

**RED-ONCE (executed).** `SnapshotStorageBlockIndexSpace` made to publish nothing:

```
ProgramTest.cpp:525: Failure
    Which is: {}
    Which is: { "Blocks", "VertexOnly", "gl_AtomicCounterBlock_0", "FragmentOnly" }
...
58% tests passed, 25 tests failed out of 60
  3091 DirectVulkan.ProgramPipelineScenario.ComputeAndGraphicsStagesShareOnePipeline  (SEGFAULT)
  3092 DirectVulkan.ProgramPipelineScenario.AStageProgramsStorageBlockBindingReaches... (Failed)
  3282 DirectVulkan.StorageBufferRegrowScenario...                                      (Failed)
  4179 DirectVulkan.Split.Buffers.StorageBufferRegrowScenario...                        (Failed)
  4198 DirectVulkan.Spawn.Buffers.StorageBufferRegrowScenario...                        (Failed)
  4217 DirectVulkan.Tcp.Buffers.StorageBufferRegrowScenario...                          (Failed)
  4469 DirectVulkan.Split.ProgramPipelineScenario...                                    (Failed)
```

The monolith arm reds beside the three transports, which is the evidence that the monolith
consumer really was switched and not merely edited. Restored: 61/61.
