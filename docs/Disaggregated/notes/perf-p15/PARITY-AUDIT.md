# P15 S0.1: parity audit of the deleted frontend-arm mechanisms

**Question.** P13 W4 moved monolith onto the record/wire arm, and W6e / `de6ab167` then deleted the
frontend arm as dead code. Which performance mechanisms did that arm have, and does the wire arm
have an equivalent?

**Sources:**
- `origin/dev` and the pre-W6 tree `88a11b81` (the frontend arm);
- the HEAD wire arm;
- [PARITY-SEED.md](PARITY-SEED.md), the regex-extracted list of deleted definitions;
- the profiles in PLAN-P15.md §2.2: FCL MC 1.21.5 and rd12, render thread, ms/frame.

**Verdicts:**
- **port**: measured as missing, and the port is planned;
- **have**: the wire arm has an equivalent;
- **skip**: not shown in any profile; reconsider only if one shows it.

## Magma (DirectVulkan)

| # | mechanism (dev / 88a11b81) | wire arm at HEAD | profile evidence | verdict, cost |
|---|---|---|---|---|
| M1 | **Clears.** `VkClearManager`: a glClear inside the active pass on the same attachments became `ClearAttachmentsOnActiveRenderPass`; otherwise a pending clear keyed (texture identity, level, layer) turned the next pass's load op into CLEAR, materialised before reads, blits and copies | `ClearWireFramebuffer`: ends the draw pass; per surface, a new view + render pass + framebuffer, its own pass, then retire | FCL 0.42-0.46 ms (dev ~0.05); rd12 ~0.27 ms. Dev's per-draw `HasPendingClear` probe cost ~0.6 ms/frame on rd12 | **port, in two steps.** (a) The in-pass half: the clear takes the draw pass a draw to the same framebuffer would take (`PrepareWireDrawPass`) and records `vkCmdClearAttachments` in it. No new state, nothing deferred, so no read path has to materialise anything. (b) LOAD_OP_CLEAR only if a GPU-side reason appears; CPU-wise (a) removes the object churn and the pass break. Small cost, low risk **Update 2026-10-09 (cut 1c):** (a) covered same-framebuffer clears only. Minecraft 1.21.5 clears through framebuffers of their own (a depth-only one for the shared depth texture, one per colour target) and draws through another framebuffer holding the same images. Every such clear then began a LOAD_OP_CLEAR pass that did nothing but clear: 11.5 passes/frame against dev's 4.1. Dev handled the cross-framebuffer case with its texture-keyed pending clears, deleted as dead code in P13 W6. Cut 1c restores it, keyed on the image itself (the storage resource, level and layers; never a GL name): a clear of images the open pass holds goes in-pass, a whole-image clear of other images waits on the image (folded into the next pass that draws into it as LOAD_OP_CLEAR, or run at the first other use through SyncTextureResourceByHandle), and the default framebuffer stays excluded (the known openra Adreno in-pass issue). See MONOLITH-DIFF.md. |
| M2 | **Render pass + framebuffer cache.** `VkRenderPassManager::GetOrCreateRenderPass`, hash-keyed `RenderPassEntry` (render pass, framebuffer), garbage-collected at present | `m_wireDrawPassCaches`: exact-key reuse within one frame slot, emptied when the slot's frame begins (`ClearWireDrawPassCache`). Every frame rebuilds each framebuffer's pass, views and VkFramebuffer | FCL: vkCreateFramebuffer in SetupWireDraw 0.04, `~RenderPassEntry` 0.04-0.05, plus views | **port**: keep entries across frames while the key's image epochs hold, and retire through the submit-indexed list when an image goes away. Medium cost, medium risk (view lifetime against image release) |
| M3 | **Blit resources.** `InitializeBlitResources`, `TryBlitToDefaultFramebufferWithShader` (persistent pipeline and pool) | `BlitWireColorImage`: persistent pipelines, but per blit a descriptor **pool** + set, 2 views and a framebuffer, all retired | FCL 0.13 ms (vkCreateDescriptorPool 0.084 + destroy) vs dev 0.02 | **port**: one pool per frame slot, reset when the slot's frame begins; a full pool falls back to the old path. Views and framebuffer are next if they show up. Small cost, low risk |
| M4 | **Buffer sub-data.** `VkBufferManager::OnSubData`: a busy resident buffer took a staged copy (`StagedRangeCopy`, as now). A *streamed* buffer (`AcquireStreamedSlice`) updated only the CPU shadow, and the next draw streamed the whole buffer into a fresh arena slice, so no copy and no pass break | `WriteWireBuffer`: busy → `StagedWireRangeCopy`, which ends the pass and records two full barriers; idle → host memcpy. No streamed/renamed buffers on the wire | FCL 0.28 ms (11 sub-data calls/frame, ~25 µs each), and each one splits the pass on the GPU. WBUF probe: 4.5 host + 6.7 staged writes/frame; ~5.6 of the staged ones hit one 288-byte store | **ported**: write renaming for stores of 64 KB or less with a host shadow; the rename goes through the same orphan release as glBufferData, which every bind memo already handles (VkBuffer compare + destroy epoch) |
| M5 | **Draw fast path.** `TrySetupDrawFastPath` / `SetupDrawSnapshot` (4 snapshots on program lifetime id) | P14 wire memos: pass continuation (`WirePassMemoHolds`), unit sampler memo, pipeline / vertex / index bind dedupe, vertex input on its record | FCL SetupWireDraw 0.55 ≈ dev SetupDraw 0.54 | **have** |
| M6 | **Uniform rebinds.** `UniformManager` `FastRebindMemo`, `SampledBindingsUnchanged`, `CollectSampledTextures` walk-skip | P14 descriptor-set rebind with fresh offsets, walk-skip | small | **have** |
| M7 | **Vertex input.** `VertexInputStateFactory` per-VAO memos (`VaoBackendMemos`, memoised hash) | vertex input kept on its record (`ResolveWireVertexInput`) | small | **have** |
| M8 | **Texture sync.** `VkTextureManager` `SyncedTextureMemoEntry`, `DrawSyncScope` | P14 direct-mapped texture fronts, record-serial checks | small | **have** |
| M9 | **Vertex streams.** `UploadAndBindVertexBuffers`, `ConvertedVertexStream` cache (per frame, pointer + changeSerial), `LookupVaoDrawMemo` | wire vertex slices + bind shadow, but NO converted-stream cache: every draw re-converted the whole remaining stream | FCL: none (no conversion). BSL: `ConvertWireVertexStream` 49 ms/frame, Sodium multi-draws over one arena (Magma 19.5 fps vs dev 64.7) | **ported** (corrected from "have"): per-frame cache keyed on a monotonic store content serial, then kept across frames in `VkBufferManager` |
| M10 | **Depth mipmap / MSAA resolve / YUV**: per-op descriptor pools existed in dev too | same per-op pools | not in FCL/rd12 profiles | **skip** |
| M11 | **Command-buffer reset/end** | larger, more fragmented recordings | FCL +0.16 ms | re-measure after M1-M4 (follows from the pass count) |

## Espryt (DirectGLES)

| # | mechanism | wire/record arm at HEAD | profile evidence | verdict |
|---|---|---|---|---|
| E1 | `StateBackendObjectRegistry` (pointer-keyed twins, GC) | twins keyed on applier serials / lifetime ids (P14) | backend +0.09 ms on FCL | have |
| E2 | `SyncCurrentProgram`, `BindCurrentProgramWithResources` | `SyncCurrentProgramByHandle` + P14 memos | rd12 +0.05/+0.07 | covered by S3 (backend draw-delta) |
| E3 | `SyncVaoAttributeBuffersByHandle` | `SyncVaoAttributeBuffersByRecord`, `AdoptUnchangedBufferSets` | rd12 +0.09 | covered by S2/S3 |
| E4 | `BackendFramebufferObject::SyncToBackend` (dirty-bit sync) | record-serial sync, clean-attachment skip (P14) | small | have |
| E5 | clear / blit (host GL) | same host calls | FCL clear 0.16 vs 0.13, blit 0.125 vs 0.11 | have (no Espryt port; user confirmed) |
| E6 | buffer sub-data (`Ops_SubDataTracked`) | `Ops_H_SubDataTracked` via record | 0.024 vs 0.007 | skip |

Espryt's FCL gap is the client half of the draw path (+0.39 ms), which stages S1-S3 address.

## Order of ports (S0.2)

1. M1a: in-pass clear.
2. M3: blit frame pools.
3. M2: cross-frame pass cache.
4. M4: sub-data renaming.
5. Re-measure M11.

Every step: FCL presenting-thread CPU before and after, in the same session, with the MobileGlues
and dev reference rows.
