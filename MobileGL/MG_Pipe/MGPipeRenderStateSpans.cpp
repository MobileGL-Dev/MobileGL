// MobileGL - MobileGL/MG_Pipe/MGPipeRenderStateSpans.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The definitions behind MGPipeRenderStateSpans.h and behind the two arrays
// generated/PipeSpanTable.inc has declared since P0. Compiled ONLY under
// MOBILEGL_PIPE_PUSH (CMakeLists.txt appends it to SOURCE_FILES there), which is how the
// pull build gains no symbol from the split - a declaration emits nothing.
//
// PROVENANCE OF THE PIPELINE HALF. It began as the enumeration
// VulkanRenderer::ComputePipelineStateHash carried above itself, which was the contract
// that function had without being able to say so; it moves here because this file is now
// that contract. Verbatim, from VulkanRenderer.cpp at feat/disaggregated@48268068:
//
//   Value hash over every fixed-function GL state the pipeline payload reads that
//   the memo key's other fields (mode, program hash, vertex-input hash, render-pass
//   hash, transform flags) do not already pin down. Enumerated against the payload
//   build in GetOrCreatePipeline - any new GL-state read there must be added here:
//     - capability bits: CullFace, DepthTest, PolygonOffsetFill (mode gating rides
//       the memo's mode key), RasterizerDiscard, ColorLogicOp, StencilTest,
//       PrimitiveRestart(+FixedIndex), SampleShading, SampleMask, plus the depth write mask
//     - patch vertices, polygon mode, cull face mode, depth func, logic op,
//       min sample shading, the glSampleMaski word
//     - front/back stencil ops + compare funcs (ref/mask are dynamic state)
//     - per draw buffer up to the render pass's colour span: indexed blend enable,
//       blend factors/equations, indexed colour write mask (broadcast from index 0
//       when the device lacks independentBlend - the same read the payload does)
//   FBO-derived payload inputs (attachment presence/formats/draw-buffer gating) are
//   pinned by the render-pass hash key, exactly as the version-keyed memo relied on.
//
// P2's pipeline half is a strict SUPERSET of that list. It adds SampleCoverageValue,
// SampleCoverageInvert, FrontFaceModeSetting, ProvokingVertexModeSetting,
// ScissorTestEnabledMask, PolygonModeBack, the eleven capability bools the hash never read
// (DebugOutput, DebugOutputSynchronous, Dither, LineSmooth, PolygonOffsetLine,
// PolygonOffsetPoint, PolygonSmooth, SampleAlphaToCoverage, SampleAlphaToOne, SampleCoverage,
// ProgramPointSize) and the three capabilities P2 gave storage to (FramebufferSrgb,
// DepthClamp, TextureCubeMapSeamless). All of them are written by a setter that calls
// BumpVersions(), so under the header's rule they are pipeline. The alternative - demoting
// those setters to ++m_version - would change MG_State semantics in the PULL build for the
// sake of the push path. Growing the subset costs nothing measurable: the hash runs only
// when m_pipelineStateVersion moves, which is exactly when Magma recomputed
// ComputePipelineStateHash before.
//
// The render-pass facts are deliberately NOT here. ComputePipelineStateHash's signature is
// (colorAttachmentCount, rasterizationSamples) and it folds ResolveEffectiveSampleMask, so
// it was never a pure function of RenderStateParameters; a CSO handle cannot replace it on
// its own and Magma keeps renderPassHash as a separate memo-key component.
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>

#include <cstring>

namespace MobileGL::MG_Pipe {
    namespace {
        // Half-local chunk index -> global chunk index. The halves alternate, so this is
        // arithmetic rather than a table.
        constexpr SizeT GlobalPipelineChunk(SizeT halfIndex) { return halfIndex * 2 + 1; }
        constexpr SizeT GlobalDynamicChunk(SizeT halfIndex) { return halfIndex * 2; }

        const Uint8* BytesOf(const RenderStateParameters& params) {
            return reinterpret_cast<const Uint8*>(&params);
        }
        Uint8* BytesOf(RenderStateParameters& params) { return reinterpret_cast<Uint8*>(&params); }

        SizeT BlobBytes(Uint32 chunkMask, SizeT halfCount, SizeT (*toGlobal)(SizeT)) {
            SizeT total = 0;
            for (SizeT i = 0; i < halfCount; ++i) {
                if ((chunkMask & (1u << i)) == 0) continue;
                total += MGPipeRenderStateChunkAt(toGlobal(i)).Length;
            }
            return total;
        }

        void Gather(const RenderStateParameters& params, Uint32 chunkMask, void* dst, SizeT halfCount,
                    SizeT (*toGlobal)(SizeT)) {
            Uint8* out = static_cast<Uint8*>(dst);
            const Uint8* src = BytesOf(params);
            for (SizeT i = 0; i < halfCount; ++i) {
                if ((chunkMask & (1u << i)) == 0) continue;
                const MGPStateChunk chunk = MGPipeRenderStateChunkAt(toGlobal(i));
                std::memcpy(out, src + chunk.Offset, chunk.Length);
                out += chunk.Length;
            }
        }

        void Scatter(const void* src, Uint32 chunkMask, RenderStateParameters& dst, SizeT halfCount,
                     SizeT (*toGlobal)(SizeT)) {
            const Uint8* in = static_cast<const Uint8*>(src);
            Uint8* out = BytesOf(dst);
            for (SizeT i = 0; i < halfCount; ++i) {
                if ((chunkMask & (1u << i)) == 0) continue;
                const MGPStateChunk chunk = MGPipeRenderStateChunkAt(toGlobal(i));
                std::memcpy(out + chunk.Offset, in, chunk.Length);
                in += chunk.Length;
            }
        }

        Uint32 ChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b,
                               SizeT halfCount, SizeT (*toGlobal)(SizeT)) {
            const Uint8* left = BytesOf(a);
            const Uint8* right = BytesOf(b);
            Uint32 mask = 0;
            for (SizeT i = 0; i < halfCount; ++i) {
                const MGPStateChunk chunk = MGPipeRenderStateChunkAt(toGlobal(i));
                if (std::memcmp(left + chunk.Offset, right + chunk.Offset, chunk.Length) != 0) {
                    mask |= 1u << i;
                }
            }
            return mask;
        }

        constexpr Uint32 AllChunks(SizeT halfCount) {
            return halfCount >= 32 ? ~Uint32{0} : static_cast<Uint32>((Uint64{1} << halfCount) - 1);
        }
    } // namespace

    // The two arrays generated/PipeSpanTable.inc declares. Every entry is
    // MGPipeRenderStateChunkAt(), so a boundary can only be written once.
    const MGPStateChunk kMGPipePipelineChunks[kMGPipePipelineChunkCount] = {
        MGPipeRenderStateChunkAt(GlobalPipelineChunk(0)), MGPipeRenderStateChunkAt(GlobalPipelineChunk(1)),
        MGPipeRenderStateChunkAt(GlobalPipelineChunk(2)), MGPipeRenderStateChunkAt(GlobalPipelineChunk(3)),
        MGPipeRenderStateChunkAt(GlobalPipelineChunk(4)), MGPipeRenderStateChunkAt(GlobalPipelineChunk(5)),
        MGPipeRenderStateChunkAt(GlobalPipelineChunk(6)),
    };
    static_assert(sizeof(kMGPipePipelineChunks) / sizeof(kMGPipePipelineChunks[0]) == kMGPipePipelineChunkCount,
                  "kMGPipePipelineChunks lost an entry");

    const MGPStateChunk kMGPipeDynamicChunks[kMGPipeDynamicChunkCount] = {
        MGPipeRenderStateChunkAt(GlobalDynamicChunk(0)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(1)),
        MGPipeRenderStateChunkAt(GlobalDynamicChunk(2)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(3)),
        MGPipeRenderStateChunkAt(GlobalDynamicChunk(4)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(5)),
        MGPipeRenderStateChunkAt(GlobalDynamicChunk(6)), MGPipeRenderStateChunkAt(GlobalDynamicChunk(7)),
    };
    static_assert(sizeof(kMGPipeDynamicChunks) / sizeof(kMGPipeDynamicChunks[0]) == kMGPipeDynamicChunkCount,
                  "kMGPipeDynamicChunks lost an entry");

    void MGPipeGatherPipelineBytes(const RenderStateParameters& params, void* dst) {
        Gather(params, AllChunks(kMGPipePipelineChunkCount), dst, kMGPipePipelineChunkCount,
               GlobalPipelineChunk);
    }

    void MGPipeScatterPipelineBytes(const void* src, RenderStateParameters& dst) {
        Scatter(src, AllChunks(kMGPipePipelineChunkCount), dst, kMGPipePipelineChunkCount,
                GlobalPipelineChunk);
    }

    SizeT MGPipePipelineChunkBlobBytes(Uint32 chunkMask) {
        return BlobBytes(chunkMask, kMGPipePipelineChunkCount, GlobalPipelineChunk);
    }

    void MGPipeGatherPipelineChunks(const RenderStateParameters& params, Uint32 chunkMask, void* dst) {
        Gather(params, chunkMask, dst, kMGPipePipelineChunkCount, GlobalPipelineChunk);
    }

    void MGPipeScatterPipelineChunks(const void* src, Uint32 chunkMask, RenderStateParameters& dst) {
        Scatter(src, chunkMask, dst, kMGPipePipelineChunkCount, GlobalPipelineChunk);
    }

    SizeT MGPipeDynamicChunkBlobBytes(Uint32 chunkMask) {
        return BlobBytes(chunkMask, kMGPipeDynamicChunkCount, GlobalDynamicChunk);
    }

    void MGPipeGatherDynamicChunks(const RenderStateParameters& params, Uint32 chunkMask, void* dst) {
        Gather(params, chunkMask, dst, kMGPipeDynamicChunkCount, GlobalDynamicChunk);
    }

    void MGPipeScatterDynamicChunks(const void* src, Uint32 chunkMask, RenderStateParameters& dst) {
        Scatter(src, chunkMask, dst, kMGPipeDynamicChunkCount, GlobalDynamicChunk);
    }

    Uint32 MGPipeDynamicChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b) {
        return ChunksThatMoved(a, b, kMGPipeDynamicChunkCount, GlobalDynamicChunk);
    }

    Uint32 MGPipePipelineChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b) {
        return ChunksThatMoved(a, b, kMGPipePipelineChunkCount, GlobalPipelineChunk);
    }

    Uint64 MGPipeHashPipelineBytes(const void* bytes) {
        return static_cast<Uint64>(
            XXH64(bytes, kMGPipePipelineChunkBytes, kMGPipeRenderStateChunkTableSeed));
    }

    Uint64 MGPipeComputePipelineSubsetHash(const RenderStateParameters& params) {
        // 396 bytes on the stack. A streaming XXH64_state_t would allocate; gathering first
        // is also what CsoCache wants, because the same bytes are what a hash hit memcmps
        // against before the handle is reused.
        Uint8 gathered[kMGPipePipelineChunkBytes];
        MGPipeGatherPipelineBytes(params, gathered);
        return MGPipeHashPipelineBytes(gathered);
    }
} // namespace MobileGL::MG_Pipe
