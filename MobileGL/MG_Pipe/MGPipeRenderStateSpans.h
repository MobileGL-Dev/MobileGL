// MobileGL - MobileGL/MG_Pipe/MGPipeRenderStateSpans.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include "MGPipeTypes.h"
#include "MGPipeValueTypes.h"

// G7: the pipeline/dynamic split of RenderStateParameters, written in EXACTLY ONE PLACE
// (ARCHITECTURE.md 5.3, D-B1).
//
// The rule that decides the split, and it is the only rule:
//
//     A byte of RenderStateParameters is in the PIPELINE half if and only if some public
//     RenderState setter that calls BumpVersions() writes it. Every other byte is in the
//     DYNAMIC half. There is no third set.
//
// That makes the G7 invariant - the pipeline-subset hash moves IF AND ONLY IF
// m_pipelineStateVersion moves - true by CONSTRUCTION rather than by inspection, and it is
// what MG_Test/Pipe/RenderStateSpansTest.cpp walks every setter to confirm.
//
// The chunks alternate: chunk 0 is dynamic, chunk 1 is pipeline, and so on, so the whole
// table is 16 BOUNDARIES rather than 15 hand-written ranges. Every boundary is an offsetof
// or a sizeof - never a literal - because a python guess at a layout it cannot see is
// exactly the drift the setter-consistency test exists to catch. 8 dynamic chunks + 7
// pipeline chunks = 15, and both counts fit the Uint32 ChunkMask of MGPRenderStateDesc and
// MGPDynamicState with room to spare.
//
// Note the two splits are ORTHOGONAL and coexist (ARCHITECTURE.md 5.3): DirectGLES'
// head [0, 312) / blend [312, 536) / tail [536, 1168) spans cut ACROSS this table, and
// nothing about them changes. StencilFaceState is deliberately NOT reordered - reordering
// would move Espryt's shadow bytes for no gain.
namespace MobileGL::MG_Pipe {

    namespace MGPipeRenderStateChunkDetail {
        using RSP = RenderStateParameters;
        using SFS = StencilFaceState;

        inline constexpr SizeT kStencilFace0 = offsetof(RSP, StencilStates);
        inline constexpr SizeT kStencilFace1 = kStencilFace0 + sizeof(SFS);
        // The pipeline half of one stencil face is [Func, Ref) + [FailOp, end); the dynamic
        // half is [Ref, FailOp) - Ref and ValueMask are VK_DYNAMIC_STATE_STENCIL_REFERENCE /
        // _COMPARE_MASK and WriteMask is _WRITE_MASK, which is why glStencilFunc changing only
        // the reference must not evict a cached pipeline (RenderState.cpp SetStencilFunc).
        inline constexpr SizeT kFaceDynamicBegin = offsetof(SFS, Ref);
        inline constexpr SizeT kFaceDynamicEnd = offsetof(SFS, FailOp);
    } // namespace MGPipeRenderStateChunkDetail

    // 15 chunks, 16 boundaries, strictly ascending, [0, sizeof(RenderStateParameters)).
    inline constexpr SizeT kMGPipeRenderStateChunkCount = 15;

    inline constexpr Array<SizeT, kMGPipeRenderStateChunkCount + 1> kMGPipeRenderStateChunkBoundaries = {
        // D0 dynamic: Viewports[16], LineWidth, PointSize
        SizeT{0},
        // P0 pipeline: PatchVertices, PatchDefaultOuterLevel, PatchDefaultInnerLevel
        offsetof(RenderStateParameters, PatchVertices),
        // D1 dynamic: PolygonOffsetFactor/Units/Clamp, ClipOrigin, ClipDepthMode
        offsetof(RenderStateParameters, PolygonOffsetFactor),
        // P1 pipeline: BlendStates[8], LogicOp, DepthTestEnabled, DepthFunc, DepthMask,
        //              ColorMasks[8], FramebufferSrgbEnabled, DepthClampEnabled,
        //              TextureCubeMapSeamlessEnabled
        offsetof(RenderStateParameters, BlendStates),
        // D2 dynamic: ClearColor, ClearDepth, ClearStencil, BlendColor, DepthRanges[16]
        offsetof(RenderStateParameters, ClearColor),
        // P2 pipeline: SampleCoverageValue, SampleCoverageInvert, SampleMaskValue,
        //              MinSampleShadingValue, StencilStates[0].Func
        offsetof(RenderStateParameters, SampleCoverageValue),
        // D3 dynamic: StencilStates[0].{Ref, ValueMask, WriteMask}
        MGPipeRenderStateChunkDetail::kStencilFace0 + MGPipeRenderStateChunkDetail::kFaceDynamicBegin,
        // P3 pipeline: StencilStates[0].{FailOp, PassDepthFailOp, PassDepthPassOp},
        //              StencilStates[1].Func
        MGPipeRenderStateChunkDetail::kStencilFace0 + MGPipeRenderStateChunkDetail::kFaceDynamicEnd,
        // D4 dynamic: StencilStates[1].{Ref, ValueMask, WriteMask}
        MGPipeRenderStateChunkDetail::kStencilFace1 + MGPipeRenderStateChunkDetail::kFaceDynamicBegin,
        // P4 pipeline: StencilStates[1].{FailOp, PassDepthFailOp, PassDepthPassOp},
        //              CullFaceEnabled, CullFaceModeSetting, FrontFaceModeSetting,
        //              ProvokingVertexModeSetting
        MGPipeRenderStateChunkDetail::kStencilFace1 + MGPipeRenderStateChunkDetail::kFaceDynamicEnd,
        // D5 dynamic: the four hints, PointFadeThresholdSize, PointSpriteCoordOrigin,
        //             ClampReadColor
        offsetof(RenderStateParameters, LineSmoothHint),
        // P5 pipeline: PolygonModeFront, PolygonModeBack
        offsetof(RenderStateParameters, PolygonModeFront),
        // D6 dynamic: PrimitiveRestartIndex
        offsetof(RenderStateParameters, PrimitiveRestartIndex),
        // P6 pipeline: the 20 capability bools ColorLogicOpEnabled..ProgramPointSizeEnabled,
        //              ScissorTestEnabledMask
        offsetof(RenderStateParameters, ColorLogicOpEnabled),
        // D7 dynamic: ScissorBoxes[16], ScissorBoxWrittenMask, ClipDistanceEnabledMask
        offsetof(RenderStateParameters, ScissorBoxes),
        sizeof(RenderStateParameters),
    };

    // Chunk 0 is dynamic and they alternate, which is not a coincidence: every boundary above
    // is a transition between a run of BumpVersions()-written members and a run of
    // ++m_version-only members, so two adjacent chunks of the same half would mean a boundary
    // that separates nothing.
    constexpr Bool MGPipeRenderStateChunkIsPipeline(SizeT index) { return (index % 2) == 1; }

    constexpr MGPStateChunk MGPipeRenderStateChunkAt(SizeT index) {
        return MGPStateChunk{static_cast<Uint16>(kMGPipeRenderStateChunkBoundaries[index]),
                             static_cast<Uint16>(kMGPipeRenderStateChunkBoundaries[index + 1] -
                                                 kMGPipeRenderStateChunkBoundaries[index])};
    }

    namespace MGPipeRenderStateChunkDetail {
        constexpr SizeT CountHalf(Bool pipeline) {
            SizeT count = 0;
            for (SizeT i = 0; i < kMGPipeRenderStateChunkCount; ++i) {
                if (MGPipeRenderStateChunkIsPipeline(i) == pipeline) ++count;
            }
            return count;
        }
        constexpr SizeT BytesOfHalf(Bool pipeline) {
            SizeT bytes = 0;
            for (SizeT i = 0; i < kMGPipeRenderStateChunkCount; ++i) {
                if (MGPipeRenderStateChunkIsPipeline(i) == pipeline) {
                    bytes += MGPipeRenderStateChunkAt(i).Length;
                }
            }
            return bytes;
        }
    } // namespace MGPipeRenderStateChunkDetail

    inline constexpr SizeT kMGPipePipelineChunkCount = MGPipeRenderStateChunkDetail::CountHalf(true);
    inline constexpr SizeT kMGPipeDynamicChunkCount = MGPipeRenderStateChunkDetail::CountHalf(false);
    // The CSO's content-addressed identity is exactly this many bytes; CsoCache stores them
    // per entry and memcmps them on a hash hit.
    inline constexpr SizeT kMGPipePipelineChunkBytes = MGPipeRenderStateChunkDetail::BytesOfHalf(true);
    inline constexpr SizeT kMGPipeDynamicChunkBytes = MGPipeRenderStateChunkDetail::BytesOfHalf(false);

    // Seeds MGPipeComputePipelineSubsetHash, so a chunk-table change invalidates every
    // persisted key rather than silently aliasing an old one. BUMP IT whenever a boundary,
    // an ordering or the halves' membership moves.
    inline constexpr Uint64 kMGPipeRenderStateChunkTableVersion = 1;

    // ---- the trip wires. A mistake in the table is a build break, here. ----
    static_assert(kMGPipeRenderStateChunkBoundaries[0] == 0,
                  "the chunk table must start at byte 0 of RenderStateParameters");
    static_assert(kMGPipeRenderStateChunkBoundaries[kMGPipeRenderStateChunkCount] ==
                      sizeof(RenderStateParameters),
                  "the chunk table must cover RenderStateParameters to its last byte");
    static_assert(kMGPipePipelineChunkCount == 7);
    static_assert(kMGPipeDynamicChunkCount == 8);
    static_assert(kMGPipePipelineChunkCount + kMGPipeDynamicChunkCount == kMGPipeRenderStateChunkCount);
    static_assert(kMGPipePipelineChunkBytes + kMGPipeDynamicChunkBytes == sizeof(RenderStateParameters),
                  "the two halves must partition the block exactly - no gap, no overlap");
    static_assert(kMGPipeRenderStateChunkCount <= 32,
                  "a chunk index has to fit the Uint32 ChunkMask of MGPRenderStateDesc/MGPDynamicState");

    // Sorted, non-overlapping and complete: because every chunk is [b[i], b[i+1]) the only
    // way to violate that is a non-ascending boundary, so this is the whole check.
    constexpr Bool MGPipeRenderStateChunkBoundariesAscend() {
        for (SizeT i = 0; i < kMGPipeRenderStateChunkCount; ++i) {
            if (!(kMGPipeRenderStateChunkBoundaries[i] < kMGPipeRenderStateChunkBoundaries[i + 1])) {
                return false;
            }
            if (kMGPipeRenderStateChunkBoundaries[i + 1] > 0xffffu) return false;
        }
        return true;
    }
    static_assert(MGPipeRenderStateChunkBoundariesAscend(),
                  "the chunk boundaries must strictly ascend and fit MGPStateChunk's Uint16 fields");

    // The measured sizes. They are DERIVED above; these two assertions only pin what the P2
    // brief and MEASUREMENTS.md quote, so a table change that moves them is loud.
    static_assert(kMGPipePipelineChunkBytes == 396, "the pipeline subset is 396 bytes");
    static_assert(kMGPipeDynamicChunkBytes == 772, "the dynamic subset is 772 bytes");

    // ---- global chunk bits, so nothing downstream hand-maintains a second table ----

    // The GLOBAL chunk indices (bit i is chunk i of the 15) whose byte range overlaps
    // [offset, offset + size). It falls straight out of the boundary table, which is the
    // whole point: the applier scopes its derivation by the chunks a scatter actually moved
    // (D5/D8), and a hand-written member -> chunk mapping is exactly the second table that
    // would go stale the first time a boundary moves.
    constexpr Uint32 MGPipeRenderStateChunkBitsCovering(SizeT offset, SizeT size) {
        Uint32 bits = 0;
        for (SizeT i = 0; i < kMGPipeRenderStateChunkCount; ++i) {
            const SizeT begin = kMGPipeRenderStateChunkBoundaries[i];
            const SizeT end = kMGPipeRenderStateChunkBoundaries[i + 1];
            if (offset < end && begin < offset + size) bits |= Uint32{1} << i;
        }
        return bits;
    }

    // The wire masks are HALF-LOCAL (bit i of MGPRenderStateDesc::ChunkMask is pipeline chunk
    // i); these widen them to the global indices the boundary table is written in. The
    // halves alternate with chunk 0 dynamic, so the two conversions are arithmetic.
    constexpr Uint32 MGPipeGlobalChunkBitsOfPipelineMask(Uint32 pipelineMask) {
        Uint32 bits = 0;
        for (SizeT i = 0; i < kMGPipePipelineChunkCount; ++i) {
            if (((pipelineMask >> i) & 1u) != 0) bits |= Uint32{1} << (i * 2 + 1);
        }
        return bits;
    }
    constexpr Uint32 MGPipeGlobalChunkBitsOfDynamicMask(Uint32 dynamicMask) {
        Uint32 bits = 0;
        for (SizeT i = 0; i < kMGPipeDynamicChunkCount; ++i) {
            if (((dynamicMask >> i) & 1u) != 0) bits |= Uint32{1} << (i * 2);
        }
        return bits;
    }
    inline constexpr Uint32 kMGPipeAllGlobalChunks =
        static_cast<Uint32>((Uint64{1} << kMGPipeRenderStateChunkCount) - 1);

    // The two conversions must agree with MGPipeRenderStateChunkIsPipeline, and together they
    // must cover the table exactly - a widening that dropped or doubled a chunk would make
    // the applier's scoping silently wrong rather than loud.
    namespace MGPipeRenderStateChunkDetail {
        inline constexpr Uint32 kAllPipelineHalfBits =
            static_cast<Uint32>((Uint64{1} << kMGPipePipelineChunkCount) - 1);
        inline constexpr Uint32 kAllDynamicHalfBits =
            static_cast<Uint32>((Uint64{1} << kMGPipeDynamicChunkCount) - 1);
        inline constexpr Uint32 kWidenedPipeline = MGPipeGlobalChunkBitsOfPipelineMask(kAllPipelineHalfBits);
        inline constexpr Uint32 kWidenedDynamic = MGPipeGlobalChunkBitsOfDynamicMask(kAllDynamicHalfBits);
    } // namespace MGPipeRenderStateChunkDetail
    static_assert((MGPipeRenderStateChunkDetail::kWidenedPipeline &
                   MGPipeRenderStateChunkDetail::kWidenedDynamic) == 0,
                  "the two half-local -> global widenings must not overlap");
    static_assert((MGPipeRenderStateChunkDetail::kWidenedPipeline |
                   MGPipeRenderStateChunkDetail::kWidenedDynamic) == kMGPipeAllGlobalChunks,
                  "the two half-local -> global widenings must cover the whole chunk table");
    static_assert(MGPipeRenderStateChunkBitsCovering(0, sizeof(RenderStateParameters)) == kMGPipeAllGlobalChunks,
                  "every chunk must be covered by the whole block");

    // ---- the operations everything else is written against ----

    // The 396 pipeline bytes of `params`, in ascending chunk order, into `dst`.
    void MGPipeGatherPipelineBytes(const RenderStateParameters& params, void* dst);
    // The inverse: `src` is kMGPipePipelineChunkBytes bytes in the same order.
    void MGPipeScatterPipelineBytes(const void* src, RenderStateParameters& dst);
    // Incremental create_render_state: only the pipeline chunks named by `chunkMask` (bit i
    // is pipeline chunk i, 0-based within the pipeline half), concatenated ascending.
    SizeT MGPipePipelineChunkBlobBytes(Uint32 chunkMask);
    void MGPipeGatherPipelineChunks(const RenderStateParameters& params, Uint32 chunkMask, void* dst);
    void MGPipeScatterPipelineChunks(const void* src, Uint32 chunkMask, RenderStateParameters& dst);

    // set_dynamic_state: bit i of `chunkMask` is dynamic chunk i, 0-based within the dynamic
    // half; the blob is those chunks concatenated in ascending order.
    SizeT MGPipeDynamicChunkBlobBytes(Uint32 chunkMask);
    void MGPipeGatherDynamicChunks(const RenderStateParameters& params, Uint32 chunkMask, void* dst);
    void MGPipeScatterDynamicChunks(const void* src, Uint32 chunkMask, RenderStateParameters& dst);
    // Which dynamic chunks differ between two blocks - the chunk-level suppressor's answer.
    Uint32 MGPipeDynamicChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b);
    // Which pipeline chunks differ - the incremental-create mask against a base CSO.
    Uint32 MGPipePipelineChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b);

    // XXH64 over the seven pipeline chunks in ascending order, seeded with the table version.
    // Runs ONLY when m_pipelineStateVersion moved, i.e. never in the steady state.
    Uint64 MGPipeComputePipelineSubsetHash(const RenderStateParameters& params);
    // The same hash over already-gathered bytes (CsoCache holds them, so it does not re-gather).
    Uint64 MGPipeHashPipelineBytes(const void* bytes);
} // namespace MobileGL::MG_Pipe
