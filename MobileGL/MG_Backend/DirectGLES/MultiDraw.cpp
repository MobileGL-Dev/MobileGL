// MobileGL - MobileGL/MG_Backend/DirectGLES/MultiDraw.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "MultiDraw.h"
#include "Managers.h"
#include <MG_State/GLState/Core.h>
#include <cstring>
#include <limits>

namespace MobileGL::MG_Backend::DirectGLES::MultiDrawImpl {
    using MG_Config::GLESMultiDrawMode;

    namespace {
        // ---------------------------------------------------------------------------
        // Batch shape
        // ---------------------------------------------------------------------------

        SizeT IndexTypeSize(GLenum type) {
            switch (type) {
            case GL_UNSIGNED_BYTE: return 1;
            case GL_UNSIGNED_SHORT: return 2;
            case GL_UNSIGNED_INT: return 4;
            default: return 0;
            }
        }

        // The index value this batch restarts on, compared at 32 bits against the zero-extended
        // source index. Normally the all-ones value of the source type, which is what
        // GL_PRIMITIVE_RESTART_FIXED_INDEX and GLES both restart on; with desktop
        // GL_PRIMITIVE_RESTART it is instead whatever glPrimitiveRestartIndex named. The rebased
        // tier turns whichever it is into 0xFFFFFFFF in its widened stream, which is what the
        // driver restarts on.
        //
        // No truncation, deliberately, and the same rule ResolveRestartSubstitution applies: a
        // restart index the source type cannot hold simply matches nothing, so returning it
        // verbatim is already "this batch restarts nowhere".
        Uint32 RestartSentinelFor(GLenum type) {
            if (ResolveRestartSubstitution(type) != RestartSubstitutionKind::None) {
                return MG_State::pGLContext->GetPrimitiveRestartIndex();
            }
            return MG_Util::FixedRestartIndexForGLType(type);
        }

        Bool RestartActive() {
            return MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestart) ||
                   MG_State::pGLContext->IsCapabilityEnabled(CapabilityInput::PrimitiveRestartFixedIndex);
        }

        // Vertices per primitive for the modes whose sub-draws may be concatenated into a
        // single draw without changing the primitive stream. Zero for strip/loop/fan modes
        // (concatenation would weld one sub-draw's last primitive to the next sub-draw's
        // first) and for GL_PATCHES, whose primitive size is dynamic tessellation state.
        Uint32 ConcatenablePrimitiveSize(GLenum mode) {
            switch (mode) {
            case GL_POINTS: return 1;
            case GL_LINES: return 2;
            case GL_TRIANGLES: return 3;
            case GL_LINES_ADJACENCY: return 4;
            case GL_TRIANGLES_ADJACENCY: return 6;
            default: return 0;
            }
        }

        // Beyond this an emulated batch would ask for a scratch allocation measured in
        // hundreds of megabytes (and the scratch ring never shrinks again); decline and let
        // a per-sub-draw tier handle it instead of trying and failing inside the driver.
        constexpr SizeT kMaxFlattenedIndices = SizeT{1} << 24;

        // The flattening dispatch is one invocation per output index. ES 3.1 only
        // guarantees 65535 work groups per dimension, and exceeding it makes
        // glDispatchCompute an INVALID_VALUE no-op - which would leave the draw reading an
        // uninitialised index buffer rather than failing visibly. Cap the tier there
        // instead of querying: 4.19M indices is far past any real multi-draw batch, and
        // beyond it the per-sub-draw tiers are the better answer anyway.
        constexpr SizeT kComputeWorkGroupSize = 64;
        constexpr SizeT kMaxComputeWorkGroups = 65535;
        constexpr SizeT kMaxComputeFlattenedIndices = kMaxComputeWorkGroups * kComputeWorkGroupSize;

        Uint BoundDrawIndirectBufferId() {
            const auto& indirect =
                MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
            if (!indirect) return 0;
            const auto* resource = BufferImpl::EnsureBufferResource(indirect);
            return resource ? resource->id : 0;
        }

        const SharedPtr<MG_State::GLState::BufferObject>& BoundIndexBuffer() {
            static const SharedPtr<MG_State::GLState::BufferObject> none;
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) return none;
            return vao->GetIndexBufferBindingSlot().GetBoundObject();
        }

        // The GL name PrepareForDraw left on GL_ELEMENT_ARRAY_BUFFER, i.e. what a tier
        // that swaps in a scratch index buffer has to put back. Restoring the exact name
        // matters beyond tidiness: the VAO twin memoises that it already synced this
        // index binding and will not re-issue it on the next draw.
        Uint BoundIndexBufferId() {
            const auto& ibo = BoundIndexBuffer();
            if (!ibo) return 0;
            const auto* resource = BufferImpl::EnsureBufferResource(ibo);
            return resource ? resource->id : 0;
        }

        // ---------------------------------------------------------------------------
        // Scratch GL objects
        //
        // All of them belong to the ES context and are abandoned (not deleted) when it
        // dies, exactly like XfbImpl's scatter buffer: the names are the dead context's
        // to reclaim, and deleting them would target whatever the successor context
        // handed out for the same name.
        // ---------------------------------------------------------------------------

        struct ScratchBuffer {
            Uint id = 0;
            SizeT capacity = 0;
            SizeT cursor = 0; // ring buffers only: next free byte
        };

        ScratchBuffer g_indirectCommands; // synthesized DrawElementsIndirectCommand array
        ScratchBuffer g_rebasedIndices;   // CPU-rebased index stream
        ScratchBuffer g_drawInfo;         // compute tier: per-sub-draw descriptors
        ScratchBuffer g_flattenedIndices; // compute tier: flattened index stream

        Uint g_computeProgram = 0;
        Bool g_computeProgramFailed = false;
        GLint g_uElementSize = -1;
        GLint g_uDrawCount = -1;
        GLint g_uTotalIndices = -1;

        // Reused staging, so a steady stream of batches allocates nothing.
        Vector<DrawElementsIndirectCommand> g_commandStaging;
        Vector<Uint32> g_indexStaging;
        Vector<Uint32> g_drawInfoStaging;
        Vector<GLint> g_zeroBaseVertices;

        // Everything below stages through GL_ARRAY_BUFFER, the manager-wide staging target
        // (BufferImpl::TempBufferTarget); binding it disturbs no VAO state.
        Bool EnsureScratchName(ScratchBuffer& buffer) {
            if (buffer.id != 0) return true;
            GLuint id = 0;
            g_GLESFuncs.glGenBuffers(1, &id);
            if (id == 0) return false;
            buffer.id = id;
            buffer.capacity = 0;
            buffer.cursor = 0;
            return true;
        }

        // Whole-buffer upload, for the two buffers that are read from offset 0 because they
        // are bound as storage blocks. Respecifies rather than sub-updates: glBufferData
        // orphans the previous store, so the upload never waits on a dispatch still reading
        // the old contents out of the same name.
        Bool UploadScratch(ScratchBuffer& buffer, SizeT bytes, const void* data) {
            if (bytes == 0) return true;
            if (!EnsureScratchName(buffer)) return false;
            BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, buffer.id);
            // Grow in powers of two so a batch that creeps up in size stops respecifying.
            SizeT capacity = buffer.capacity == 0 ? bytes : buffer.capacity;
            while (capacity < bytes) capacity *= 2;
            g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(capacity), nullptr,
                                     GL_STREAM_DRAW);
            buffer.capacity = capacity;
            buffer.cursor = 0;
            if (data) {
                g_GLESFuncs.glBufferSubData(BufferImpl::TempBufferTarget, 0, static_cast<GLsizeiptr>(bytes), data);
            }
            return true;
        }

        // Ring upload, for the buffers whose consumers can address a byte offset (indirect
        // commands and rewritten index streams). Respecifying per batch is what an
        // orphan-every-time scheme costs, and on a desktop-class driver that allocation
        // dominated the tiers that use these buffers - a multi-draw of 32 sub-draws stages
        // 640 bytes and paid for a fresh store to hold them. Bump-allocating instead means
        // one respecify per wrap; every byte between two wraps is written exactly once, so
        // nothing in flight is overwritten, and the wrap itself orphans.
        constexpr SizeT kRingAlignment = 16; // >= 4, so both command and uint32-index offsets stay legal
        constexpr SizeT kMinRingBytes = 1u << 16;

        Bool UploadScratchRing(ScratchBuffer& buffer, SizeT bytes, const void* data, SizeT& outOffset) {
            outOffset = 0;
            if (bytes == 0) return true;
            if (!EnsureScratchName(buffer)) return false;
            BufferImpl::BindBufferId(BufferImpl::TempBufferTarget, buffer.id);

            const SizeT aligned = (bytes + kRingAlignment - 1) & ~(kRingAlignment - 1);
            if (buffer.capacity < aligned) {
                SizeT capacity = buffer.capacity == 0 ? kMinRingBytes : buffer.capacity;
                while (capacity < aligned) capacity *= 2;
                g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(capacity), nullptr,
                                         GL_STREAM_DRAW);
                buffer.capacity = capacity;
                buffer.cursor = 0;
            } else if (buffer.cursor + aligned > buffer.capacity) {
                g_GLESFuncs.glBufferData(BufferImpl::TempBufferTarget, static_cast<GLsizeiptr>(buffer.capacity),
                                         nullptr, GL_STREAM_DRAW);
                buffer.cursor = 0;
            }

            outOffset = buffer.cursor;
            if (data) {
                g_GLESFuncs.glBufferSubData(BufferImpl::TempBufferTarget, static_cast<GLintptr>(outOffset),
                                            static_cast<GLsizeiptr>(bytes), data);
            }
            buffer.cursor += aligned;
            return true;
        }

        // ---------------------------------------------------------------------------
        // Tier resolution
        // ---------------------------------------------------------------------------

        // Best-first, and measured rather than assumed. MobileGlues orders its own Auto
        // multiindirect -> indirect -> basevertex; on both ES drivers available here that
        // is backwards, because staging a command buffer per batch costs more than the
        // driver entries it saves. mc_sodium_multidraw (132 batches x 32 sub-draws),
        // ns/op, median of three:
        //
        //                    NVIDIA ES 3.2   Mesa llvmpipe ES 3.2
        //   ext                  n/a               19300
        //   basevertex           2500              25200
        //   multiindirect        5700              27600
        //   drawelements         5600              28700
        //   indirect             5800              31000
        //
        // Ring-allocating the command staging (instead of respecifying per batch) was
        // tried first and moved the indirect tiers by less than noise, so the cost is the
        // indirect draw path itself, not the upload. Only "ext" - a real multi-draw entry
        // point rather than an indirect one - actually beats replaying the sub-draws.
        //
        // The compute tier is deliberately absent from the ladder: it rewrites the
        // primitive stream rather than replaying it, and it measured slowest of all here,
        // so it stays opt-in behind the env knob (the same call MobileGlues makes - its
        // Auto never selects Compute either).
        constexpr GLESMultiDrawMode kAutoLadder[] = {
            GLESMultiDrawMode::Ext,      GLESMultiDrawMode::BaseVertex,   GLESMultiDrawMode::MultiIndirect,
            GLESMultiDrawMode::Indirect, GLESMultiDrawMode::DrawElements,
        };

        Bool SupportsTier(GLESMultiDrawMode tier) {
            return IsTierSupported(g_GLESCapabilities, g_GLESFuncs, tier);
        }

        GLESMultiDrawMode g_resolvedTier = GLESMultiDrawMode::Auto;
        Bool g_tierResolved = false;
        String g_tierResolution;

        void ResolveTierOnce() {
            if (g_tierResolved) return;
            g_tierResolved = true;
            g_resolvedTier =
                ResolveTier(g_GLESCapabilities, g_GLESFuncs, MG_Config::Features.EsprytMultiDrawMode,
                            &g_tierResolution);
            MGLOG_D("DirectGLES multi-draw: %s", g_tierResolution.c_str());
        }

        // Which tiers have already announced themselves, one bit per GLESMultiDrawMode.
        // The resolution line above says which tier was CHOSEN; this says which one a
        // batch actually went through, and the two differ whenever a batch's shape
        // demotes it. Worth a line each: a multi-draw path that resolves to a tier and
        // then quietly runs a different one is exactly how "the batch drew nothing"
        // hides.
        Uint32 g_announcedTiers = 0;

        void NoteTierExecuted(GLESMultiDrawMode tier) {
            const Uint32 bit = 1u << static_cast<Uint32>(tier);
            if (g_announcedTiers & bit) return;
            g_announcedTiers |= bit;
            MGLOG_D("DirectGLES multi-draw: first batch executed via tier \"%s\"", TierName(tier));
        }

        // The tier this particular batch can actually take. A tier is demoted here when
        // the batch's own shape - not the driver - rules it out; the compute tier keeps
        // its remaining feasibility checks inside its implementation, where the data it
        // has to walk is already in hand.
        GLESMultiDrawMode ResolveTierForBatch(Bool programReadsDrawID, Bool perSubDrawBaseVertex,
                                              Bool hasIndexBuffer, Bool arbitraryRestart) {
            ResolveTierOnce();
            GLESMultiDrawMode tier = g_resolvedTier;

            // Desktop GL_PRIMITIVE_RESTART restarts on an application-chosen index; the driver
            // only ever restarts on the all-ones value. Every tier but the rebased one hands
            // the application's own index data to the driver, which would then see no restarts
            // at all and weld the primitives together. The rebased tier is the one that
            // REWRITES the stream, and RestartSentinelFor already tells it which value to
            // translate, so it is the only tier this batch can take.
            if (arbitraryRestart) {
                return GLESMultiDrawMode::DrawElements;
            }

            // Batched tiers issue one driver entry for the whole batch, so the emulated
            // gl_DrawID uniform can only hold one value across every sub-draw. A program
            // that reads gl_DrawID gets an unrolled tier, which feeds each sub-draw its
            // own index (the spec's value); nothing else observes the difference. The
            // emulated gl_BaseVertex is one uniform for the same reason, so a batch whose
            // sub-draws carry their own base vertices unrolls too - even the Ext tier,
            // which hands the driver the whole basevertex array, can only leave ONE value
            // in the uniform the shader reads.
            const Bool batched = tier == GLESMultiDrawMode::Ext || tier == GLESMultiDrawMode::MultiIndirect ||
                                 tier == GLESMultiDrawMode::Compute;
            if (batched && (programReadsDrawID || perSubDrawBaseVertex)) {
                tier = SupportsTier(GLESMultiDrawMode::BaseVertex) ? GLESMultiDrawMode::BaseVertex
                                                                   : GLESMultiDrawMode::DrawElements;
            }

            // The indirect tiers describe each sub-draw as an element offset into the
            // bound element array buffer. A client-memory index array has no such buffer,
            // and indirect draws are not defined without one.
            if (!hasIndexBuffer &&
                (tier == GLESMultiDrawMode::MultiIndirect || tier == GLESMultiDrawMode::Indirect)) {
                tier = SupportsTier(GLESMultiDrawMode::BaseVertex) ? GLESMultiDrawMode::BaseVertex
                                                                   : GLESMultiDrawMode::DrawElements;
            }
            return tier;
        }

        // ---------------------------------------------------------------------------
        // Index rewriting, shared by the two tiers that fold base vertices into indices
        // ---------------------------------------------------------------------------

        // Both of those tiers emit GL_UNSIGNED_INT regardless of the source type. Keeping
        // the source width would be wrong, not merely tight: GL adds baseVertex to the
        // index at full precision, so a GL_UNSIGNED_SHORT index plus a base vertex past
        // 65535 addresses a vertex the source type cannot spell. Widening also gives the
        // rewritten stream a restart sentinel (0xFFFFFFFF) that survives the rebase.
        void RebaseIndices(const Uint8* source, SizeT sourceIndexCount, SizeT indexSize, Int32 baseVertex,
                           Bool restartActive, Uint32 restartSentinel, Uint32* out) {
            const Uint32 baseVertexBits = static_cast<Uint32>(baseVertex);
            for (SizeT i = 0; i < sourceIndexCount; ++i) {
                Uint32 value = 0;
                switch (indexSize) {
                case 1: value = source[i]; break;
                case 2: {
                    Uint16 narrow = 0;
                    std::memcpy(&narrow, source + i * 2, sizeof(narrow));
                    value = narrow;
                    break;
                }
                default: std::memcpy(&value, source + i * 4, sizeof(value)); break;
                }
                // Unsigned wraparound is the defined behaviour for a negative base vertex.
                out[i] = (restartActive && value == restartSentinel) ? 0xFFFFFFFFu : value + baseVertexBits;
            }
        }

        // CPU-readable bytes of one sub-draw's indices, from the frontend shadow of the
        // bound index buffer or straight from the client array. Null when the sub-draw
        // would read outside the buffer.
        const Uint8* ResolveSubDrawIndices(const SharedPtr<MG_State::GLState::BufferObject>& indexBuffer,
                                           const Uint8* indexBufferBytes, SizeT indexBufferSize, const void* indices,
                                           SizeT indexCount, SizeT indexSize) {
            if (!indexBuffer) {
                return static_cast<const Uint8*>(indices);
            }
            if (!indexBufferBytes) return nullptr;
            const SizeT byteOffset = reinterpret_cast<SizeT>(indices);
            const SizeT byteEnd = byteOffset + indexCount * indexSize;
            if (byteEnd > indexBufferSize || byteEnd < byteOffset) return nullptr;
            return indexBufferBytes + byteOffset;
        }

        // ---------------------------------------------------------------------------
        // Tier: Ext - one glMultiDrawElementsBaseVertexEXT
        // ---------------------------------------------------------------------------

        Bool RunExt(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices, GLsizei drawcount,
                    const GLint* basevertex) {
            if (!SupportsTier(GLESMultiDrawMode::Ext)) return false;
            const GLint* baseVertices = basevertex;
            if (!baseVertices) {
                // glMultiDrawElements: every base vertex is 0, but the entry point still
                // wants an array. One permanently-zero vector serves every such batch.
                if (g_zeroBaseVertices.size() < static_cast<SizeT>(drawcount)) {
                    g_zeroBaseVertices.resize(static_cast<SizeT>(drawcount), 0);
                }
                baseVertices = g_zeroBaseVertices.data();
            }
            g_GLESFuncs.glMultiDrawElementsBaseVertexEXT(mode, count, type, indices, drawcount, baseVertices);
            NoteTierExecuted(GLESMultiDrawMode::Ext);
            return true;
        }

        // ---------------------------------------------------------------------------
        // Tiers: MultiIndirect / Indirect - synthesized indirect commands
        // ---------------------------------------------------------------------------

        Bool RunIndirect(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                         GLsizei drawcount, const GLint* basevertex, Bool batched, Bool feedDrawID,
                         Bool feedBaseVertex) {
            if (!SupportsTier(batched ? GLESMultiDrawMode::MultiIndirect : GLESMultiDrawMode::Indirect)) return false;
            const SizeT indexSize = IndexTypeSize(type);
            if (indexSize == 0) return false;
            // Indirect commands address indices as an element offset into the bound element
            // array buffer, and an indirect draw is not defined without one.
            const auto& indexBuffer = BoundIndexBuffer();
            if (!indexBuffer) return false;

            g_commandStaging.resize(static_cast<SizeT>(drawcount));
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT byteOffset = reinterpret_cast<SizeT>(indices[i]);
                // firstIndex counts elements, so an offset that is not a whole number of
                // them cannot be expressed as a command at all.
                if (byteOffset % indexSize != 0) return false;
                auto& command = g_commandStaging[static_cast<SizeT>(i)];
                command.count = count[i] > 0 ? static_cast<Uint32>(count[i]) : 0u;
                command.instanceCount = 1;
                command.firstIndex = static_cast<Uint32>(byteOffset / indexSize);
                command.baseVertex = basevertex ? basevertex[i] : 0;
                command.baseInstance = 0;
            }

            const SizeT commandBytes = g_commandStaging.size() * sizeof(DrawElementsIndirectCommand);
            SizeT commandBase = 0;
            if (!UploadScratchRing(g_indirectCommands, commandBytes, g_commandStaging.data(), commandBase)) {
                return false;
            }

            // Every synthesized command carries baseInstance 0. Say so through the direct
            // path, which also clears the indirect-params word index a preceding real
            // indirect draw may have left pointing into its own command buffer.
            SetCurrentBaseInstance(0);

            const Uint previousIndirectBinding = BoundDrawIndirectBufferId();
            BufferImpl::BindBufferId(GL_DRAW_INDIRECT_BUFFER, g_indirectCommands.id);
            if (batched) {
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glMultiDrawElementsIndirectEXT(mode, type, reinterpret_cast<const void*>(commandBase),
                                                               drawcount, 0);
                });
            } else {
                for (GLsizei i = 0; i < drawcount; ++i) {
                    if (feedDrawID) SetCurrentDrawID(static_cast<Uint32>(i));
                    if (feedBaseVertex) SetCurrentBaseVertex(basevertex ? basevertex[i] : 0);
                    const SizeT commandOffset = commandBase + static_cast<SizeT>(i) * sizeof(DrawElementsIndirectCommand);
                    ForEachViewportRoutingPass([&] {
                        g_GLESFuncs.glDrawElementsIndirect(mode, type, reinterpret_cast<const void*>(commandOffset));
                    });
                }
                if (feedDrawID) SetCurrentDrawID(0);
                if (feedBaseVertex) SetCurrentBaseVertex(0);
            }
            BufferImpl::BindBufferId(GL_DRAW_INDIRECT_BUFFER, previousIndirectBinding);
            NoteTierExecuted(batched ? GLESMultiDrawMode::MultiIndirect : GLESMultiDrawMode::Indirect);
            return true;
        }

        // ---------------------------------------------------------------------------
        // Tier: BaseVertex - the per-sub-draw replay
        // ---------------------------------------------------------------------------

        Bool RunBaseVertexLoop(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                               GLsizei drawcount, const GLint* basevertex, Bool feedDrawID, Bool feedBaseVertex) {
            if (!SupportsTier(GLESMultiDrawMode::BaseVertex)) return false;
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] <= 0) continue;
                if (feedDrawID) SetCurrentDrawID(static_cast<Uint32>(i));
                if (feedBaseVertex) SetCurrentBaseVertex(basevertex ? basevertex[i] : 0);
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawElementsBaseVertex(mode, count[i], type, indices[i],
                                                         basevertex ? basevertex[i] : 0);
                });
            }
            if (feedDrawID) SetCurrentDrawID(0);
            if (feedBaseVertex) SetCurrentBaseVertex(0);
            NoteTierExecuted(GLESMultiDrawMode::BaseVertex);
            return true;
        }

        // ---------------------------------------------------------------------------
        // Tier: DrawElements - base vertices folded into a scratch index stream
        // ---------------------------------------------------------------------------

        Bool RunRebasedDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                    GLsizei drawcount, const GLint* basevertex, Bool feedDrawID,
                                    Bool feedBaseVertex) {
            const SizeT indexSize = IndexTypeSize(type);
            if (indexSize == 0) return false;

            SizeT total = 0;
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] > 0) total += static_cast<SizeT>(count[i]);
            }
            if (total == 0) return true;
            if (total > kMaxFlattenedIndices) return false;

            const auto& indexBuffer = BoundIndexBuffer();
            const Uint8* indexBufferBytes = nullptr;
            SizeT indexBufferSize = 0;
            if (indexBuffer) {
                // The shadow is the source of truth for CPU reads, but a persistent map or
                // a shader write may have moved past it since the last sync.
                indexBuffer->SyncPersistentMappedRange();
                indexBuffer->SyncGpuWrites();
                indexBufferBytes = indexBuffer->MappedData();
                indexBufferSize = indexBuffer->GetSize();
            }

            const Bool restartActive = RestartActive();
            const Uint32 restartSentinel = RestartSentinelFor(type);
            // Widening to GL_UNSIGNED_INT gives a UBYTE/USHORT source a sentinel it can never
            // spell, so those batches are lossless. A UINT source that already uses 0xFFFFFFFF as
            // a real vertex index while restarting on a different one is the one shape 32 bits
            // cannot express - the same corner the single-draw substitution reports.
            if (restartActive && indexSize == 4 && restartSentinel != 0xFFFFFFFFu) {
                MGLOG_E_ONCE("GL_PRIMITIVE_RESTART with restart index %u over GL_UNSIGNED_INT multi-draw indices: "
                             "any index that is already 0xFFFFFFFF will restart too, because the rewritten stream "
                             "has no wider sentinel to move to.",
                             restartSentinel);
            }
            g_indexStaging.resize(total);
            SizeT cursor = 0;
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] <= 0) continue;
                const SizeT subDrawCount = static_cast<SizeT>(count[i]);
                const Uint8* source = ResolveSubDrawIndices(indexBuffer, indexBufferBytes, indexBufferSize, indices[i],
                                                            subDrawCount, indexSize);
                if (!source) {
                    MGLOG_E_ONCE("DirectGLES multi-draw (drawelements tier): sub-draw %d reads outside the bound index "
                            "buffer; skipping the batch",
                            i);
                    return false;
                }
                RebaseIndices(source, subDrawCount, indexSize, basevertex ? basevertex[i] : 0, restartActive,
                              restartSentinel, g_indexStaging.data() + cursor);
                cursor += subDrawCount;
            }

            SizeT indexBase = 0;
            if (!UploadScratchRing(g_rebasedIndices, total * sizeof(Uint32), g_indexStaging.data(), indexBase)) {
                return false;
            }

            const Uint previousIndexBinding = BoundIndexBufferId();
            BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, g_rebasedIndices.id);
            cursor = 0;
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] <= 0) continue;
                if (feedDrawID) SetCurrentDrawID(static_cast<Uint32>(i));
                // The base vertex is folded into the rewritten index stream here, so the
                // driver sees none - but gl_BaseVertex still has to report the value the
                // application passed for this sub-draw.
                if (feedBaseVertex) SetCurrentBaseVertex(basevertex ? basevertex[i] : 0);
                ForEachViewportRoutingPass([&] {
                    g_GLESFuncs.glDrawElements(mode, count[i], GL_UNSIGNED_INT,
                                               reinterpret_cast<const void*>(indexBase + cursor * sizeof(Uint32)));
                });
                cursor += static_cast<SizeT>(count[i]);
            }
            if (feedDrawID) SetCurrentDrawID(0);
            if (feedBaseVertex) SetCurrentBaseVertex(0);
            BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, previousIndexBinding);
            NoteTierExecuted(GLESMultiDrawMode::DrawElements);
            return true;
        }

        // ---------------------------------------------------------------------------
        // Tier: Compute - the whole batch flattened into one rebased index stream
        // ---------------------------------------------------------------------------

        // One index per invocation. The sub-draw an output slot belongs to is found by
        // binary search over the inclusive prefix sums of the sub-draw counts, which is
        // why the descriptors are sorted by construction. Sub-draws with a zero count
        // repeat the previous prefix sum and are therefore skipped by the search.
        //
        // Three storage blocks, not the five the shape suggests: ES 3.1 only guarantees
        // four per compute stage, so the per-sub-draw descriptors share one buffer.
        constexpr const char* kFlattenComputeSource = R"(#version 310 es
layout(local_size_x = 64) in;

uniform uint uElementSize;
uniform uint uDrawCount;
uniform uint uTotalIndices;

layout(std430, binding = 0) readonly buffer SourceIndices { uint sourceWords[]; };
layout(std430, binding = 1) readonly buffer DrawInfo { uint drawInfo[]; };
layout(std430, binding = 2) writeonly buffer FlatIndices { uint flatIndices[]; };

uint ReadSourceIndex(uint element) {
    if (uElementSize == 4u) {
        return sourceWords[element];
    }
    if (uElementSize == 2u) {
        uint word = sourceWords[element >> 1u];
        return (word >> ((element & 1u) * 16u)) & 0xFFFFu;
    }
    uint word = sourceWords[element >> 2u];
    return (word >> ((element & 3u) * 8u)) & 0xFFu;
}

void main() {
    uint outIndex = gl_GlobalInvocationID.x;
    if (outIndex >= uTotalIndices) {
        return;
    }

    uint low = 0u;
    uint high = uDrawCount - 1u;
    while (low < high) {
        uint mid = low + (high - low) / 2u;
        if (drawInfo[mid * 3u + 2u] > outIndex) {
            high = mid;
        } else {
            low = mid + 1u;
        }
    }

    uint localIndex = outIndex - (low == 0u ? 0u : drawInfo[(low - 1u) * 3u + 2u]);
    // Unsigned wraparound is the defined behaviour for a negative base vertex. No
    // restart sentinel handling: the tier declines outright while restart is enabled.
    flatIndices[outIndex] = ReadSourceIndex(localIndex + drawInfo[low * 3u]) + drawInfo[low * 3u + 1u];
}
)";

        struct FlattenedStream {
            Uint bufferId = 0;
            SizeT indexCount = 0;
        };

        Bool EnsureComputeProgram() {
            if (g_computeProgram != 0) return true;
            if (g_computeProgramFailed) return false;
            g_computeProgramFailed = true; // cleared again only on a complete success

            const GLuint shader = g_GLESFuncs.glCreateShader(GL_COMPUTE_SHADER);
            if (shader == 0) {
                MGLOG_E_ONCE("DirectGLES multi-draw (compute tier): glCreateShader(GL_COMPUTE_SHADER) failed");
                return false;
            }
            const char* source = kFlattenComputeSource;
            g_GLESFuncs.glShaderSource(shader, 1, &source, nullptr);
            g_GLESFuncs.glCompileShader(shader);
            GLint status = GL_FALSE;
            g_GLESFuncs.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                char log[1024] = {};
                g_GLESFuncs.glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                MGLOG_E_ONCE("DirectGLES multi-draw (compute tier): index-flattening shader failed to compile: %s", log);
                g_GLESFuncs.glDeleteShader(shader);
                return false;
            }

            const GLuint program = g_GLESFuncs.glCreateProgram();
            if (program == 0) {
                MGLOG_E_ONCE("DirectGLES multi-draw (compute tier): glCreateProgram failed");
                g_GLESFuncs.glDeleteShader(shader);
                return false;
            }
            g_GLESFuncs.glAttachShader(program, shader);
            g_GLESFuncs.glLinkProgram(program);
            g_GLESFuncs.glDeleteShader(shader);
            g_GLESFuncs.glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status != GL_TRUE) {
                char log[1024] = {};
                g_GLESFuncs.glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                MGLOG_E_ONCE("DirectGLES multi-draw (compute tier): index-flattening program failed to link: %s", log);
                g_GLESFuncs.glDeleteProgram(program);
                return false;
            }

            g_computeProgram = program;
            g_uElementSize = g_GLESFuncs.glGetUniformLocation(program, "uElementSize");
            g_uDrawCount = g_GLESFuncs.glGetUniformLocation(program, "uDrawCount");
            g_uTotalIndices = g_GLESFuncs.glGetUniformLocation(program, "uTotalIndices");
            g_computeProgramFailed = false;
            MGLOG_D("DirectGLES multi-draw: index-flattening compute program ready (id %u)", program);
            return true;
        }

        // Builds the flattened stream, or leaves `out` empty when this batch's shape rules
        // the tier out. Runs BEFORE PrepareForDraw - see the call site - so it may leave
        // the compute program current and the first storage points unbound; the
        // preparation that follows re-establishes both.
        void FlattenWithCompute(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                GLsizei drawcount, const GLint* basevertex, FlattenedStream& out) {
            if (!SupportsTier(GLESMultiDrawMode::Compute)) return;
            const SizeT indexSize = IndexTypeSize(type);
            if (indexSize == 0) return;

            // Merging sub-draws into a single draw only reproduces the original primitive
            // stream for list-shaped modes: a strip, loop or fan would gain primitives
            // spanning the seam between two sub-draws.
            const Uint32 primitiveSize = ConcatenablePrimitiveSize(mode);
            if (primitiveSize == 0) return;

            // Primitive restart defeats the whole-multiple-of-a-primitive argument below,
            // even for a list mode. A restart ends the current primitive, so a sub-draw of
            // six GL_TRIANGLES indices with a restart after the third emits ONE triangle
            // and drops the two leftover vertices - and once concatenated those leftovers
            // find a third vertex in the next sub-draw and become a triangle that GL never
            // draws. Splicing separator sentinels into the flattened stream could fix it,
            // at the cost of a per-sub-draw offset the prefix-sum layout does not carry;
            // declining is the honest trade for a tier that is already opt-in.
            if (RestartActive()) return;

            // The shader reads the source indices as a storage buffer, so there has to be
            // a real buffer to read - a client-memory index array has none.
            const auto& indexBuffer = BoundIndexBuffer();
            if (!indexBuffer) return;

            // A dispatch inside an open capture span is not legal, and the span would also
            // observe one merged draw rather than the batch it asked for.
            if (XfbImpl::IsCaptureSpanOpen()) return;

            auto* sourceResource = BufferImpl::EnsureBufferResource(indexBuffer);
            if (!sourceResource || sourceResource->id == 0) return;
            const SizeT sourceSize = indexBuffer->GetSize();
            // std430 addresses the source as uint[]; a tail shorter than a word is not
            // reachable, so a narrow index type needs a word-multiple buffer.
            if (indexSize < 4 && (sourceSize % 4) != 0) return;

            g_drawInfoStaging.resize(3 * static_cast<SizeT>(drawcount));
            SizeT total = 0;
            for (GLsizei i = 0; i < drawcount; ++i) {
                const SizeT subDrawCount = count[i] > 0 ? static_cast<SizeT>(count[i]) : 0;
                // GL drops a trailing partial primitive per sub-draw; concatenation would
                // instead splice it onto the next sub-draw's first vertices.
                if (subDrawCount % primitiveSize != 0) return;
                const SizeT byteOffset = reinterpret_cast<SizeT>(indices[i]);
                if (byteOffset % indexSize != 0) return;
                if (subDrawCount != 0) {
                    const SizeT byteEnd = byteOffset + subDrawCount * indexSize;
                    if (byteEnd > sourceSize || byteEnd < byteOffset) return;
                }
                total += subDrawCount;
                if (total > kMaxComputeFlattenedIndices) return;
                const SizeT slot = 3 * static_cast<SizeT>(i);
                g_drawInfoStaging[slot] = static_cast<Uint32>(byteOffset / indexSize);
                g_drawInfoStaging[slot + 1] = static_cast<Uint32>(basevertex ? basevertex[i] : 0);
                g_drawInfoStaging[slot + 2] = static_cast<Uint32>(total);
            }
            if (total == 0) return; // nothing to draw; the ordinary tiers no-op just as well

            if (!EnsureComputeProgram()) return;
            if (!UploadScratch(g_drawInfo, g_drawInfoStaging.size() * sizeof(Uint32), g_drawInfoStaging.data())) {
                return;
            }
            if (!UploadScratch(g_flattenedIndices, total * sizeof(Uint32), nullptr)) return;

            BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, 0, sourceResource->id);
            BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, 1, g_drawInfo.id);
            BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, 2, g_flattenedIndices.id);

            g_GLESFuncs.glUseProgram(g_computeProgram);
            PrgramImpl::g_lastUsedBackendProgramId = g_computeProgram;
            if (g_uElementSize >= 0) g_GLESFuncs.glUniform1ui(g_uElementSize, static_cast<GLuint>(indexSize));
            if (g_uDrawCount >= 0) g_GLESFuncs.glUniform1ui(g_uDrawCount, static_cast<GLuint>(drawcount));
            if (g_uTotalIndices >= 0) g_GLESFuncs.glUniform1ui(g_uTotalIndices, static_cast<GLuint>(total));

            g_GLESFuncs.glDispatchCompute(
                static_cast<GLuint>((total + kComputeWorkGroupSize - 1) / kComputeWorkGroupSize), 1, 1);
            g_GLESFuncs.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);

            // Hand the storage points back to their GL default. PrepareForDraw re-syncs
            // only the points the app has actually touched, so leaving a scratch buffer on
            // an untouched point would keep it visible to the next shader that declares one.
            for (Uint point = 0; point < 3; ++point) {
                BufferImpl::BindBufferBaseCached(GL_SHADER_STORAGE_BUFFER, point, 0);
            }

            NoteTierExecuted(GLESMultiDrawMode::Compute);
            out.bufferId = g_flattenedIndices.id;
            out.indexCount = total;
        }
    } // namespace

    // -------------------------------------------------------------------------------
    // Public surface
    // -------------------------------------------------------------------------------

    Bool IsTierSupported(const MG_External::GLESCapabilities& caps, const MG_External::GLESFunctionsTable& funcs,
                         GLESMultiDrawMode tier) {
        const Bool esAtLeast31 =
            caps.GLESVersion.Major > 3 || (caps.GLESVersion.Major == 3 && caps.GLESVersion.Minor >= 1);
        switch (tier) {
        case GLESMultiDrawMode::Ext:
            return caps.SupportsMultiDrawElementsBaseVertex;
        case GLESMultiDrawMode::MultiIndirect:
            return caps.SupportsMultiDrawIndirect && esAtLeast31 && funcs.glDrawElementsIndirect != nullptr;
        case GLESMultiDrawMode::Indirect:
            return esAtLeast31 && funcs.glDrawElementsIndirect != nullptr;
        case GLESMultiDrawMode::BaseVertex:
            return caps.SupportsDrawElementsBaseVertex;
        case GLESMultiDrawMode::DrawElements:
            // Plain glDrawElements over a rewritten index stream: ES 2 core, so this is
            // the floor every other tier can fall back to.
            return true;
        case GLESMultiDrawMode::Compute:
            // Three storage blocks, which is inside the four ES 3.1 guarantees per stage.
            return caps.SupportsComputeShader && caps.MaxComputeShaderStorageBlocks >= 3 &&
                   funcs.glBindBufferBase != nullptr;
        case GLESMultiDrawMode::Auto:
            break;
        }
        return false;
    }

    GLESMultiDrawMode ResolveTier(const MG_External::GLESCapabilities& caps,
                                  const MG_External::GLESFunctionsTable& funcs, GLESMultiDrawMode requested,
                                  String* explanation) {
        const auto bestAuto = [&]() {
            for (const GLESMultiDrawMode tier : kAutoLadder) {
                if (IsTierSupported(caps, funcs, tier)) return tier;
            }
            return GLESMultiDrawMode::DrawElements;
        };

        GLESMultiDrawMode resolved = GLESMultiDrawMode::DrawElements;
        String line;
        if (requested == GLESMultiDrawMode::Auto) {
            resolved = bestAuto();
            line = String("auto -> ") + TierName(resolved);
        } else if (IsTierSupported(caps, funcs, requested)) {
            resolved = requested;
            line = String("MOBILEGL_ESPRYT_MULTIDRAW_MODE=") + TierName(requested) + " -> " + TierName(resolved);
        } else {
            resolved = bestAuto();
            line = String("MOBILEGL_ESPRYT_MULTIDRAW_MODE=") + TierName(requested) +
                   " requested but unsupported by this driver -> " + TierName(resolved);
        }

        if (explanation) {
            String supported;
            for (const GLESMultiDrawMode tier : kAutoLadder) {
                if (!IsTierSupported(caps, funcs, tier)) continue;
                if (!supported.empty()) supported += ", ";
                supported += TierName(tier);
            }
            if (IsTierSupported(caps, funcs, GLESMultiDrawMode::Compute)) {
                supported += supported.empty() ? "compute (opt-in)" : ", compute (opt-in)";
            }
            *explanation = line + " (driver supports: " + supported + ")";
        }
        return resolved;
    }

    const char* TierName(GLESMultiDrawMode tier) {
        switch (tier) {
        case GLESMultiDrawMode::Auto: return "auto";
        case GLESMultiDrawMode::Ext: return "ext";
        case GLESMultiDrawMode::MultiIndirect: return "multiindirect";
        case GLESMultiDrawMode::Indirect: return "indirect";
        case GLESMultiDrawMode::BaseVertex: return "basevertex";
        case GLESMultiDrawMode::DrawElements: return "drawelements";
        case GLESMultiDrawMode::Compute: return "compute";
        }
        return "unknown";
    }

    GLESMultiDrawMode ResolvedTier() {
        ResolveTierOnce();
        return g_resolvedTier;
    }

    String DescribeTierResolution() {
        ResolveTierOnce();
        return g_tierResolution;
    }

    void OnBackendContextDestroyed() {
        g_indirectCommands = {};
        g_rebasedIndices = {};
        g_drawInfo = {};
        g_flattenedIndices = {};
        g_computeProgram = 0;
        g_computeProgramFailed = false;
        g_uElementSize = -1;
        g_uDrawCount = -1;
        g_uTotalIndices = -1;
    }

    void DrawElementsBatch(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount, const GLint* basevertex) {
        if (drawcount <= 0 || !count || !indices) return;
        // Read before any GL work, because it decides the tier below: a desktop restart index
        // the driver does not know about can only be honoured by the tier that rewrites the
        // index stream (see ResolveTierForBatch). A restart index this index type cannot hold
        // needs no rewrite at all - nothing can match it - but it does need the driver's own
        // fixed-index restart held off for the batch, which is what the scope below does.
        const RestartSubstitutionKind restartKind = ResolveRestartSubstitution(type);
        const Bool arbitraryRestart = restartKind == RestartSubstitutionKind::RewriteIndices;
        const ScopedSuppressedPrimitiveRestart restartCapOverride(restartKind);

        const Bool hasIndexBuffer = BoundIndexBuffer() != nullptr;

        // The compute tier dispatches BEFORE the draw state is established: doing it
        // afterwards would mean unpicking the program, SSBO and index bindings
        // PrepareForDraw just made, and a dispatch inside an open transform feedback
        // span is not legal at all. On success it hands back a flattened index stream.
        // A batch whose sub-draws carry their own base vertices cannot be flattened either
        // when the program reads gl_BaseVertex: one draw call leaves one uniform value.
        // Asked conservatively because this decision precedes PrepareForDraw - see
        // CurrentProgramMayNeedPerSubDrawBuiltins. Flattening is the irreversible half:
        // once the batch is one draw the values are gone, whereas declining to flatten only
        // costs the unrolled tier.
        FlattenedStream flattened;
        if (ResolvedTier() == GLESMultiDrawMode::Compute &&
            !CurrentProgramMayNeedPerSubDrawBuiltins(basevertex != nullptr)) {
            FlattenWithCompute(mode, count, type, indices, drawcount, basevertex, flattened);
        }

        PrepareForDraw(DrawSyncBit::IndexBuffer);

        if (flattened.indexCount != 0) {
            const Uint previousIndexBinding = BoundIndexBufferId();
            BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, flattened.bufferId);
            ForEachViewportRoutingPass([&] {
                g_GLESFuncs.glDrawElements(mode, static_cast<GLsizei>(flattened.indexCount), GL_UNSIGNED_INT, nullptr);
            });
            BufferImpl::BindBufferId(GL_ELEMENT_ARRAY_BUFFER, previousIndexBinding);
            return;
        }

        // Now that PrepareForDraw has synced the program, both questions have real answers;
        // the tier choice and the per-sub-draw feeds use those, not the guess above.
        const Bool feedDrawID = CurrentProgramReadsDrawID();
        const Bool feedBaseVertex = basevertex != nullptr && CurrentProgramReadsBaseVertex();
        const GLESMultiDrawMode tier =
            ResolveTierForBatch(feedDrawID, feedBaseVertex, hasIndexBuffer, arbitraryRestart);

        Bool drawn = false;
        switch (tier) {
        case GLESMultiDrawMode::Ext:
            drawn = RunExt(mode, count, type, indices, drawcount, basevertex);
            break;
        case GLESMultiDrawMode::MultiIndirect:
            drawn = RunIndirect(mode, count, type, indices, drawcount, basevertex, /*batched=*/true, feedDrawID,
                                feedBaseVertex);
            break;
        case GLESMultiDrawMode::Indirect:
            drawn = RunIndirect(mode, count, type, indices, drawcount, basevertex, /*batched=*/false, feedDrawID,
                                feedBaseVertex);
            break;
        case GLESMultiDrawMode::BaseVertex:
            drawn = RunBaseVertexLoop(mode, count, type, indices, drawcount, basevertex, feedDrawID, feedBaseVertex);
            break;
        case GLESMultiDrawMode::DrawElements:
            drawn = RunRebasedDrawElements(mode, count, type, indices, drawcount, basevertex, feedDrawID,
                                           feedBaseVertex);
            break;
        case GLESMultiDrawMode::Compute:
            // Its pre-pass ran above; reaching here means it declined this batch's shape.
            break;
        case GLESMultiDrawMode::Auto:
            break; // resolution never yields Auto
        }

        // Every tier above may decline a batch whose shape it cannot express. The two
        // below are the floor: a base-vertex replay where the driver has one, and the
        // rewritten index stream where it does not. Both are safe for any batch these
        // entry points can receive - except that the base-vertex replay hands the
        // application's own indices to the driver, which cannot restart on a desktop
        // restart index, so that batch has only the rewriting floor.
        if (!drawn && !arbitraryRestart) {
            drawn = RunBaseVertexLoop(mode, count, type, indices, drawcount, basevertex, feedDrawID, feedBaseVertex);
        }
        if (!drawn) {
            drawn = RunRebasedDrawElements(mode, count, type, indices, drawcount, basevertex, feedDrawID,
                                           feedBaseVertex);
        }
        if (!drawn) {
            MGLOG_E_ONCE("DirectGLES multi-draw: no usable tier for a %d sub-draw batch (mode 0x%x, type 0x%x); "
                    "the batch was dropped",
                    drawcount, mode, type);
        }
    }
} // namespace MobileGL::MG_Backend::DirectGLES::MultiDrawImpl
