// MobileGL - MobileGL/MG_Pipe/MGPipeRenderStateSpans.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Definitions behind MGPipeRenderStateSpans.h and the two chunk arrays that
// generated/PipeSpanTable.inc declares (ARCHITECTURE.md §5.3). The pipeline half holds every
// byte a BumpVersions() setter writes, which covers all fixed-function state Magma's pipeline
// build reads and more; the extra bytes cost nothing because the subset is hashed only when
// m_pipelineStateVersion moves. Render-pass facts (color attachment count, rasterization
// samples and the effective sample mask derived from them) are not in RenderStateParameters,
// so Magma keeps renderPassHash beside the CSO handle in its pipeline memo key.
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>

#include <cstring>

namespace MobileGL::MG_Pipe {
    namespace {
        // Half-local chunk index -> global chunk index. The halves alternate, so this is
        // arithmetic rather than a table.
        constexpr SizeT GlobalPipelineChunk(SizeT halfIndex) {
            return halfIndex * 2 + 1;
        }
        constexpr SizeT GlobalDynamicChunk(SizeT halfIndex) {
            return halfIndex * 2;
        }

        const Uint8* BytesOf(const RenderStateParameters& params) {
            return reinterpret_cast<const Uint8*>(&params);
        }
        Uint8* BytesOf(RenderStateParameters& params) {
            return reinterpret_cast<Uint8*>(&params);
        }

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

        Uint32 ChunksThatMoved(const RenderStateParameters& a, const RenderStateParameters& b, SizeT halfCount,
                               SizeT (*toGlobal)(SizeT)) {
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
        Gather(params, AllChunks(kMGPipePipelineChunkCount), dst, kMGPipePipelineChunkCount, GlobalPipelineChunk);
    }

    void MGPipeScatterPipelineBytes(const void* src, RenderStateParameters& dst) {
        Scatter(src, AllChunks(kMGPipePipelineChunkCount), dst, kMGPipePipelineChunkCount, GlobalPipelineChunk);
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
        return static_cast<Uint64>(XXH64(bytes, kMGPipePipelineChunkBytes, kMGPipeRenderStateChunkTableSeed));
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
