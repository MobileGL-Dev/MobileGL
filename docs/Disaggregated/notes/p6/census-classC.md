# P6 class-C dynamic census

Date: 2026-09-16. Tree: `/home/swung/w7/p5-joint`, `p5/joint` at
`e61d00123c41d74f98cd06b63768be12d9b6348a`. The checkout was read only.
`build-split/CTestTestfile.cmake` was confirmed before execution. Every process ran with
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0` and `MOBILEGL_TRANSPORT=inproc`.

## Method and evidence

`ctest -L integration-gpu --show-only=json-v1` supplied each entry's exact command, arguments,
environment, working directory and timeout. The wrapper then drove the 1,149 commands with eight
workers and a unique `MOBILEGL_LOG_FILE_PATH`. Exact per-entry status, return code, command, working
directory and first `Fatal{...}` text are in
`/home/swung/w7/p6-census-logs/results.json`; the corresponding `<entry>.log` and `<entry>.out`
files are beside it. Runner source is `/home/swung/w7/notes/tools/p6_census_lane.{sh,py}`.

“Distinct scenario” below means the final `GTestSuite.Case` pair, deliberately collapsing backend,
Split, SmallRing and feature-arm registry variants. Counts are registry entries. Thus the two
columns answer different questions.

| status | entries |
|---|---:|
| passed | 426 |
| skipped | 185 |
| subprocess-aborted | 511 |
| ordinary failed | 27 |
| total | 1,149 |

This exactly reproduces P5's 426/185/511/27. All 511 aborts have a real FATAL-severity first line:
505 `UnmigratedVerb`, six `StageSnapshotTooNarrow`. None of the 27 ordinary failures has a Fatal.

## First unmigrated slot by integration entry

| slot | entries | distinct scenarios | ROADMAP subsystem | approximate next first blocker if this slot is migrated |
|---|---:|---:|---|---|
| `BindImageTexture` | 138 | 71 | P3b/P4b image sweep / texture binding | Usually `DispatchCompute` for image-store cases; otherwise a draw, then `MemoryBarrier` or readback. |
| `BeginTransformFeedback` | 95 | 44 | P9 XFB reverse/scatter (with P3b/P4b client scatter debt) | Usually the draw is already class B; next class C is approximately `EndTransformFeedback` or an XFB query end. |
| `DrawElements` | 56 | 27 | P8 index host mirror / draw protocol breadth | Most integration cases appear to proceed to class-B `ReadPixels`; Minecraft's next distinct class-C slot cannot be known until this one is live. |
| `DispatchCompute` | 49 | 25 | P7/P8 compute command and resource state | Commonly `MemoryBarrier`, then readback/draw. |
| `PatchParameteri` | 43 | 19 | P7/P8 tessellation patch state / draw breadth | Commonly `BeginTransformFeedback`; otherwise the patch draw itself. |
| `CopyImageSubData` | 30 | 14 | P8 CopyImage mirror | `GetTexImage`/`GetTextureImage`, `BindImageTexture`, or a draw, depending on the case. |
| `MemoryBarrier` | 18 | 9 | P7/P8 compute ordering | The GuiBatch sources next issue indexed/multi-draw work, approximately `MultiDrawElementsBaseVertex` or `DrawElements`. |
| `MultiDrawElementsBaseVertex` | 18 | 9 | P8 draw-vbo multi-draw breadth | Usually class-B readback; no later class-C call is visible in the scenario body. |
| `ClearBufferiv` | 12 | 6 | P3b/P4b framebuffer depth; P7 Magma twin | Usually class-B draw/readback; no later class-C call is visible. |
| `ClearBufferuiv` | 8 | 4 | P3b/P4b framebuffer depth; P7 Magma twin | Usually class-B draw/readback; no later class-C call is visible. |
| `ClearBufferfv` | 6 | 3 | P3b/P4b framebuffer depth; P7 Magma twin | — |
| `DrawArraysInstancedBaseInstance` | 4 | 2 | P8 draw protocol breadth | — |
| `DrawElementsBaseVertex` | 4 | 2 | P8 index/draw protocol breadth | — |
| `ShaderStorageBlockBinding` | 4 | 2 | P7/P8 compute/buffer binding | — |
| `CopyTexImage2D` | 3 | 1 | P3b/P4b texture copy; P9 readback | — |
| `MultiDrawArraysIndirectCount` | 2 | 1 | P8 indirect-count resolution | — |
| `MultiDrawArrays` | 2 | 1 | P8 draw protocol breadth | — |
| `MultiDrawElementsIndirect` | 2 | 1 | P8 indirect/index mirror | — |
| `BindTransformFeedback` | 2 | 1 | P9 XFB namespace/scatter | — |
| `MultiDrawElements` | 2 | 1 | P8 draw protocol breadth | — |
| `ClearNamedFramebufferfv` | 2 | 1 | P3b/P4b named FBO state; P7 twin | — |
| `GenerateMipmap` | 2 | 2 | P8 mip plan / CPU fallback | — |
| `DrawArraysInstanced` | 2 | 1 | P8 draw protocol breadth | — |
| `ClearBufferfi` | 1 | 1 | P3b/P4b framebuffer depth-stencil; P7 twin | — |
| `DrawElementsInstancedBaseVertex` | 0 | 0 | P8 index/draw protocol breadth | Trace-only first blocker: two backend runs, one fixture. |

The “next” column is intentionally an approximation from the scenario sources and the calls before
the Fatal. A first-blocker run cannot observe calls after its abort, and repeated calls to the same
slot are treated as solved by migrating that slot.

## Fatal-family census

| first diagnostic family | entries | distinct scenarios | interpretation |
|---|---:|---:|---|
| `UnmigratedVerb` | 505 | 248 | Actual FATAL-severity aborts. |
| `StageSnapshotTooNarrow, "respecify_whole_store"` | 6 | 4 | Actual FATAL-severity aborts; P8/P11 staging/adoption debt, not class C. |
| no Fatal | 27 | 13 | Ordinary assertion failures; the P5 family attribution remains applicable. |

One passing refusal-control log contains the words `Fatal{ProtocolCorruption}` inside an ERROR-level
explanation of what the applier *would* do. It is not a Fatal event and is excluded from the abort
table; this distinction prevents a textual grep from inventing a seventh non-verb abort.

## Trace fixtures under inproc

All 40 cases in `trace_cases.json` are hydrated locally: each required archive and primary golden
exists in the joint fixture directory and is not an LFS pointer. No fetch was performed. The
reference registry has 80 backend entries; 79 are CI-eligible because iterationrp excludes
DirectGLES. Each eligible entry was run alone with a private log by
`p6_census_traces.{sh,py}`. Consolidated evidence is
`/home/swung/w7/retrace-out/p6-census/summary.{json,txt}`; only those two retrace-output files remain.

| case(s) | backend result / first Fatal |
|---|---|
| `OpenRA` | DirectGLES PASS SSIM 1.0; DirectVulkan PASS SSIM 1.0; no Fatal. |
| `improved-transparency-minecraft-26.3` | Both backends: `Fatal{UnmigratedVerb, "DrawElementsInstancedBaseVertex"}`. |
| `minecraft-1.21.4-fabric-iris-iterationrp-in-world` | DirectVulkan only: `Fatal{UnmigratedVerb, "DrawElements"}`. |
| `minecraft-1.21.4-startup`; `minecraft-1.21.4-main-menu`; `minecraft-1.21.11-main-menu`; `minecraft-1.17-main-menu-854`; `minecraft-1.21.4-in-world`; `minecraft-1.21.4-rd12-odinlite-in-world`; `minecraft-1.21.4-fabric-sodium-in-world`; `minecraft-1.21.4-fabric-common-mods-in-world`; `minecraft-1.21.4-fabric-common-mods-inventory`; `minecraft-1.21.4-fabric-rei-inventory`; `minecraft-1.21.4-fabric-xaero-minimap-in-world`; `minecraft-1.21.4-fabric-xaero-world-map-in-world`; `minecraft-1.21.4-fabric-journeymap-in-world`; `minecraft-1.21.4-fabric-modernui-inventory`; `minecraft-1.21.4-fabric-rei-inventory-normal-world`; `minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world`; `minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world`; `minecraft-1.21.4-fabric-journeymap-in-world-normal-world`; `minecraft-1.21.4-fabric-modernui-inventory-normal-world`; `minecraft-1.21.4-fabric-iris-bsl-in-world`; `minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world`; `minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world`; `minecraft-1.21.4-fabric-iris-sundial-lite-in-world`; `minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world`; `minecraft-1.21.4-fabric-iris-complementary-unbound-in-world`; `minecraft-1.21.4-fabric-iris-mellow-in-world`; `minecraft-1.21.4-fabric-iris-nostalgia-in-world`; `minecraft-1.21.4-fabric-iris-bliss-in-world`; `minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world`; `minecraft-1.21.4-fabric-iris-iterationt-in-world`; `minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world`; `minecraft-1.21.4-fabric-iris-photon-v1.1-in-world`; `minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world`; `minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world`; `minecraft-1.21.1-neoforge-create-indirect-in-world`; `minecraft-1.21.1-neoforge-create-instancing-in-world`; `minecraft-1.21.4-fabric-iris-bsl-esc-menu-854` | Both backends: `Fatal{UnmigratedVerb, "DrawElements"}`. |

Trace totals: 2/79 pass; 75 backend entries (38 fixture cases) first-stop at `DrawElements`; two
backend entries (one case) first-stop at `DrawElementsInstancedBaseVertex`.

## Static cross: class-C slots not hit by anything measured

These 39 of the static census's 64 class-C slots appear in neither the integration lane nor the
hydrated traces and are last in the migration order:

`BeginOcclusionQuery`, `BeginTimeElapsedQuery`, `BeginXfbPrimitivesQuery`,
`BlitNamedFramebuffer`, `ClearNamedFramebufferfi`, `ClearNamedFramebufferiv`,
`ClearNamedFramebufferuiv`, `ClientWaitSync`, `CopyTexSubImage2D`, `DeleteBackendQuery`,
`DeleteSync`, `DeleteTransformFeedback`, `DispatchComputeIndirect`, `DrawArraysIndirect`,
`DrawElementsIndirect`, `DrawElementsInstanced`, `DrawElementsInstancedBaseInstance`,
`DrawElementsInstancedBaseVertexBaseInstance`, `DrawRangeElements`,
`DrawRangeElementsBaseVertex`, `EndOcclusionQuery`, `EndTimeElapsedQuery`,
`EndTransformFeedback`, `EndXfbPrimitivesQuery`, `FenceSync`, `GetGpuTimestampNs`,
`GetQueryResult64`, `GetSyncStatus`, `GetTexImage`, `GetTextureImage`,
`IsQueryResultAvailable`, `MemoryBarrierByRegion`, `MultiDrawArraysIndirect`,
`MultiDrawElementsIndirectCount`, `PauseTransformFeedback`, `QueryCounterTimestamp`,
`ResumeTransformFeedback`, `SetSwapInterval`, `WaitSync`.

## Proposed migration order

### Wave 1 — current first blockers

Wave 1 moves the top ten integration blockers plus the trace-only instanced draw blocker. It advances
544 currently aborting backend-entry executions, representing 267 distinct scenario/fixture names,
to their next blocker. Counts below are current first blockers, not promises that the scenario then
passes.

| package owner / slots | scenarios advanced | required applier and client-row seam |
|---|---:|---|
| P3b/P4b + P7/P8 image/compute: `BindImageTexture`, `DispatchCompute`, `CopyImageSubData`, `MemoryBarrier` | 119 (235 lane entries) | Reuse `MGPipeApplySetShaderImages` and `MGPipeApplySetDispatchProgram`; add/finish `MGPipeApplyLaunchGrid`, `MGPipeApplyResourceCopyRegion`, `MGPipeApplyMemoryBarrier`. Replace the `MGR_ASSIGN_UNMIGRATED` rows for `table.GL.BindImageTexture`, `CopyImageSubData`, `MemoryBarrier`, and the hand-written `table.GL.DispatchCompute = &DispatchCompute_Unmigrated` with their emit/client-resolved rows. |
| P9 XFB/tess: `BeginTransformFeedback`, `PatchParameteri` | 63 (138 lane entries) | Reuse `MGPipeApplySetPatchState`; add/finish `MGPipeApplyBeginStreamOutput`. Replace the `table.GL.BeginTransformFeedback` and `table.GL.PatchParameteri` unmigrated rows. |
| P8 indexed/multi-draw: `DrawElements`, `MultiDrawElementsBaseVertex`, `DrawElementsInstancedBaseVertex` | 75 (74 lane + 77 trace backend entries) | Reuse `MGPipeApplySetVertexBuffers` and `MGPipeApplySetIndexBuffer`; add/finish the command-side `MGPipeApplyDrawVbo` (today `DrawVbo` goes directly to `IVerbSink::OnDrawVbo`, and `PipeApply.h` has no command applier). Replace all three `table.GL.*` unmigrated rows with `EmitDrawVbo`-family rows. |
| P3b/P4b + P7 framebuffer clear: `ClearBufferiv`, `ClearBufferuiv` | 10 (20 lane entries) | Add/finish `MGPipeApplyClear` (today `Clear` also goes directly to `IVerbSink::OnClear`). Replace the two `table.GL.ClearBuffer*` unmigrated rows with `EmitClear` adapters. |

The missing command applier names above are deliberately called out: `PipeWireCodec.cpp` says the
five P5 class-B verbs have no `MGPipeApply*`, and its off-reduced-path command cases return false.
A package must either add these named PipeApply seams or explicitly extend the `IVerbSink` pattern;
claiming an existing `PipeApply.h` entry point would be false.

### Wave 2 — measured tail

Migrate the remaining measured slots, still packaged by subsystem: P8 draw variants
(`DrawArraysInstancedBaseInstance`, `DrawElementsBaseVertex`, `MultiDrawArraysIndirectCount`,
`MultiDrawArrays`, `MultiDrawElementsIndirect`, `MultiDrawElements`, `DrawArraysInstanced`);
P3b/P4b/P7 framebuffer (`ClearBufferfv`, `ClearBufferfi`, `ClearNamedFramebufferfv`);
P7/P8 compute (`ShaderStorageBlockBinding`); P8 texture/copy (`CopyTexImage2D`,
`GenerateMipmap`); and P9 XFB (`BindTransformFeedback`). This advances the remaining 38 current
verb-abort entries after Wave 1's measured set.

### Wave 3 — unmeasured static tail

Migrate the 39 “not hit” slots above, grouping query/sync/present under P10, reverse readback under
P9, and residual draw/copy breadth under P8. Their order should not displace any Wave 1 or Wave 2
package unless a newly added real workload produces a first-blocker measurement.
