# Texture / program split-scenario census (P3b/P4b wave 2-D, package D2)

`PLAN-PH-P34B-P7.md` §1.2, row **门：纹理 / program 场景 split 覆盖**: "~20 个候选场景…先在
inproc 跑一遍记首阻塞，按 P7/P8/P9 归属决定注册或具名排除".

Head `7ed5da52`, build `~/w7/p7-espryt-d2/build-split` (Release, clang, lavapipe ICD, WSL
desktop). Every candidate was run under `MOBILEGL_TRANSPORT=inproc` first; the ones that passed
were then registered through `mgl_itest_register_split_arms` and re-run under all three
transports, which is the reading in the table.

Package D1 owns `PixelStoreSweep`, `DepthStencilReadbackMatrix`, `PackedWordReadback` and
`LayeredTextureReadback`. They are not in this census.

## Summary

| | registered | excluded by name | blocked, not registered |
|---|---|---|---|
| texture family | 7 scenarios | ~~1 case~~ **0** (package D3 fixed the defect) | 0 |
| program family | 5 scenarios | 9 cases (one scenario's remainder) | 0 |

**12 scenarios newly carry `integration-split` / `-spawn` / `-tcp` entries**, **72 cases per
arm** (the table below sums to 72; an earlier draft said 71 and was simply wrong). With
`TextureViewAliasScenario` (this package's own; 3 of its 4 cases registered by D2, **all 4 since
D3**) and the `TextureUploadShape` gate's one case, this package adds **76 new
`DirectGLES.{Split,Spawn,Tcp}.` entries per arm** — 72 + 3 + 1 — counted from the generated
ctest include files against the pinned `~/w7/p7-before/ctest-names-split.txt` baseline (D3's
un-exclusion makes it 77 on the integration tree, one per arm).

The raw diff against that baseline reports **77** per arm at D2's head, not 76. The extra one is
`DirectGLES.<arm>.Ct.CtWireScenario.TheServerPublishesTheResidentSubDataCapabilityFromItsOwnTable`,
registered by the pre-existing `foreach(ctCase ...)` loop and added by `b56d70177` ("the server
publishes kCapResidentSubData off its own wire resource table") — one of the five P7 commits
between the `p7-before` baseline and this package's base. No file under `CtWireScenario` is
touched here.

An earlier draft of this note said "+37 entries per split arm". That number came from
differencing against `~/w7/pipe/build-split`, which is the integrator's LIVE build directory and
had already moved past this package's base (`7ed5da52` -> `8c5dd6fc`); it matched nothing and is
withdrawn. The split-arm registration count recorded for the tier-2 Magma replay went from 33 to
46.

**Updated by package D3** (`notes/p34b/espryt-d3.md`): the one excluded case is the only row of
this census that has moved, and it moved because the defect behind it was fixed rather than
because the exclusion was widened or narrowed. Everything else below is as D2 measured it.

## Texture family

| scenario | cases | inproc | spawn | tcp | first blocker |
|---|---|---|---|---|---|
| `TextureViewScenario` | 10 | pass | pass | pass | — (needed `MOBILEGL_ESPRYT_ENABLE_TEXTURE_VIEW=1` on the three arm environments AND on the `TcpServer.Start` fixture; without it every case green-skipped) |
| `TextureParamsWithoutASamplerViewScenario` | 4 | pass | pass | pass | — |
| `SampledSetStalenessScenario` | 2 | pass | pass | pass* | one lane-interaction flake, see below |
| `ImageSizeAfterRespecScenario` | 1 | pass | pass | pass | — |
| `ImageTargetKindScenario` | 29 | pass (24 run, 5 skip) | pass | pass | — the 5 skips are `*Texture2DMultisample*` capability skips that also skip under monolith |
| `IntegerBorderColorScenario` | 6 | pass | pass | pass | — |
| `SwizzleAccessRoutineScenario` | 4 | pass | pass | pass | — |
| `TextureViewAliasScenario` (this package) | 4 | 4 pass | 4 pass | 4 pass | ~~`TheOwnerUploadDrainRunsAgainAfterADrawBoundary`, excluded by name~~ — **fixed by package D3**, see below |

### The debt-table 待核 row: is the P6 inspection forwarder debt closed?

`PLAN-PH-P34B-P7.md` §1.3 carries "债务表「22 → P4b/P7 texture readback」…待核" and names
`TextureParamsWithoutASamplerView` as the case that settles it. **It is closed for this half.**
All four G9 cases pass under inproc, spawn and tcp on the current head — the parameter sync does
not depend on a sampler view existing, under a transport any more than under monolith, and the
white-box half (`PipeApplyPeek`) agrees with Espryt's applied value in every arm. The scenario is
now registered in the three arms, so the answer is re-taken on every CI run rather than by
inspection.

### `TextureViewAliasScenario.TheOwnerUploadDrainRunsAgainAfterADrawBoundary` — ~~excluded by name~~ FIXED (package D3)

> **D3's resolution, at the head of D2's own diagnosis because the diagnosis was right.** The
> fix is at the GATE: `IsDrawSyncCleanByRecord`'s two STORAGE clauses resolve through
> `Desc.ViewOf` to the storage record (`PipeTextureStorageRecordForRecord`), and
> `SyncTextureViewToBackendByRecord` stamps the same storage serial, so the stamp and the gate
> name one quantity. The three PARAMETER clauses stay the view's own record's. The publisher-side
> reverse index was rejected: it needs pruning on VIEW death or an owner upload bumps a recycled
> slot's `Serial`, and `VkTextureManager::ResolveWireTextureStorage` already resolves the same way
> on the Magma side, so the index would have given the two backends two answers to one question.
> The case is un-excluded and green on all three arms; `CONTRACT-P5E.md` §5.2 now states the rule
> the code implements. Everything from here to the end of this section is D2's finding as written.

Red on inproc, spawn AND tcp; green under monolith. **Not a lane problem — a product defect this
package found.** After a view has been SAMPLED once, a later `glTexSubImage` through the OWNER's
name never becomes visible through the view.

* client half provably correct: the case's own emitter-counter assertions pass, so the records
  were emitted and the applier accepted them;
* `BackendTextureObject::IsDrawSyncCleanByRecord` (`Managers.cpp:15433`) compares the VIEW's
  `record.Serial` / `record.PendingUploads`;
* an owner-side `resource_subdata` moves only the OWNER's (`MG_Pipe/PipeApply.cpp:1175`);
* nothing propagates a serial bump from a storage owner to the records that view it —
  `MGPResourceDesc::ViewOf` is a forward edge with no reverse index, read only by the
  descriptor-equality helper (`PipeApply.cpp:666`);
* monolith works because the same gate reads `GetContentVersion()` THROUGH the view, which
  `TextureObjectView` forwards to the owner (`TextureObjectView.cpp:100-102`);
* `MG_Remote/CONTRACT-P5E.md:387-395` states the split gate in full and has the same hole.

**Owner**: not P7/P8/P9 — a new finding. The fix is at the gate (`Managers.cpp`, package D1's
file) or at the publisher (`MG_Pipe/PipeApply.cpp`'s `ApplyTextureUpload`, outside D2's
footprint), and the publisher form needs an O(#texture records) scan or a reverse index on
`MGPipeResourceRecord`, plus a `CONTRACT-P5E` amendment. `KHR-GL43.texture_view.coherency` is
exactly this test and reads Pass in the CTS baseline, which is a **monolith** reading.

> **D3**: taken at the gate. `KHR-GL43.texture_view.coherency` still has no SPLIT reading on this
> tree — the CTS baselines are monolith and the split comparison is the device window's (門 5) —
> so the integration case is the only thing standing behind the fix on host hardware. Listed in
> `espryt-d3.md`'s "what remains".

### `SampledSetStalenessScenario` on tcp

`AQueuedClearIsMaterialisedWhenAFilterChangeCompletesTheTexture` aborted once in a full
`ctest -L integration-tcp -j 1` run (182 entries, 1 failure) and passes when run alone and on a
re-run of the whole lane. The tcp lane serialises through one supervisor
(`RESOURCE_LOCK mobilegl-tcp`) that serves one session at a time, so a subprocess abort there is a
session-reuse interaction rather than a property of this scenario. **Recorded as informational,
not excluded**; if it recurs it belongs with the tcp supervisor's own work, not with the texture
family.

## Program family

| scenario | cases | inproc | spawn | tcp | first blocker |
|---|---|---|---|---|---|
| `PostLinkAttachScenario` | 2 | pass | pass | pass | — |
| `RelinkStageSetScenario` | 3 | pass | pass | pass | — |
| `UniformInitializerScenario` | 3 | pass | pass | pass | — |
| `Glsl420DeclarationScenario` | 6 | pass (5 run, 1 skip) | pass | pass | — the skip is `AnArrayOfSamplerArraysIsHonouredOrDeclinedCleanly`, a capability skip that also skips under monolith |
| `IoBlockNameCollisionScenario` | 2 | pass | pass | pass | — |
| `ProgramPipelineScenario` beyond `*StorageBlock*` | 9 | **not registered** | | | **ScenarioFixture's armed-lane rule.** The two storage-block cases are already registered by the P5e i1 block. The other nine are pure name/state cases that draw nothing, and an armed `DirectGLES.Split.` case whose workload produced no record is RED by design (`ScenarioFixture.h:84-97`: the client encoder's record ordinal must move). That is the rule working, not a blocker to retire — registering them would be asserting "records crossed" about cases that have no records to send. |

## DirectVulkan arms: none, deliberately

No `DirectVulkan.Split.` / `.Spawn.` / `.Tcp.` **gating** entry is added for any of these.

It is not an omission and it is not a coverage hole either: `mgl_itest_register_split_arms`
records every call, and the informational tier-2 block replays all 46 of them on DirectVulkan
under `integration-magma-all-{split,spawn,tcp}` — so every scenario in this census DOES get a
Magma × three-transport reading the day it is registered. What it does not get is a **gate**.

Gating them would assert P7 wave 2's exit gate 1 ("集成 + trace 在 Magma push 与 split 下全绿")
from inside a P3b/P4b package, on a backend whose own `@P7` named refusals are still being retired
in waves A/B/C — the vertex-layout split, the blit/copy/mip cluster and the alignment cluster are
all in flight. A red there would be wave 2's to fix and would block this package's lane for
reasons it cannot act on. The tier-2 replay is exactly the right instrument for that: it produces
the red list without making it a gate.

## Gates

Taken on `~/w7/p7-espryt-d2/build-split` (Release, clang, lavapipe ICD) and
`~/w7/p7-espryt-d2/build-linux` (the pull configuration) at the head of this package, after the
review rework. Lane totals are whole-label runs, not filtered subsets.

| gate | reading |
|---|---|
| `ctest -L unit` | 2418 / 2418 |
| `ctest -L integration-split` | 263 / 263 |
| `ctest -L integration-spawn` | 179 / 179 |
| `ctest -L integration-tcp` | 182 / 182 |
| `integration-gpu`, DirectGLES monolith, the touched families | 89 / 89 |
| G1 pull `.text` | 10822147 = `0xa52203` |
| G1 `nm --defined-only` | identical to `~/w7/p7-before/pull-syms.txt`, 30570 symbols |
| G14 ctest name set | 4155 -> 6615; 0 names removed by this package |
| `scripts/ci/spawn_lane_parity.py build-split` | rc 0 |
| `scripts/ci/fatal_census.py` | rc 0, 0 unmarked |
| `scripts/ci/espryt_memo_purity.py --self-test` | PASS, 5 families anchored, 1 allow entry, all consulted |
| `scripts/ci/link_seam_purity.py --self-test` | PASS |
| `scripts/link_ratchet.py --self-test` | OK |

The one name the G14 diff reports as removed,
`CapsMirrorTest.APlaceholderMirrorConsumesNothing`, is absent from the base build too and is
referenced only by a comment in `RemoteClientTest.cpp:590` explaining that it was retired; it
predates this package's base.

Per-slice readings, red-once transcripts and the two findings this package made are in
`espryt-d2.md`.
