// MobileGL - MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectVulkan.h"
#include "DirectVulkanResourceState.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_State/GLState/Core.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
#include "MG_State/GLState/ErrorState/ErrorInfo.h"
#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_Util/Converters/GLToMG/TextureEnumConverter.h"
#include "MG_Util/Metrics/PipeStats.h"
#include "MG_Util/Metrics/TextureMetrics.h"
#include "MG_Util/Miscellany/IndexGenerator.h"
#if MOBILEGL_BUILD_DISAGGREGATED
#include <Config.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Pipe/PipeApply.h>
// P7 wave 2 package C (CONTRACT-P7 §5.5): Magma's own death-notice table. Both headers are
// push-only and reached here for the same reason DirectGLES/Managers.cpp reaches them - the
// notice is declared by the frontend and answered by whichever backend is running.
#include <MG_Remote/Client/WireTables.h>
#include <MG_State/GLState/StateObjectDeathNotice.h>
#endif
#include <atomic>
#include <bit>
#include <cstring>
#include <spirv_reflect.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // Per Magma session (MagmaSession.h); the process-wide session is leak-at-exit storage, see
    // GlobalObjects.cpp.
    RendererSlot pVulkanRenderer;

    namespace {
        // Generation of the live VulkanRenderer instance, mirroring
        // DirectGLES's g_syncContextGeneration. BackendObject_DirectVulkan
        // bumps it (BumpRendererGeneration) wherever pVulkanRenderer is reset
        // or recreated. Fence and timer-query handles are stamped with the
        // generation they were created under: a stale stamp means the frame
        // serials and query-pool slots the handle refers to belong to a
        // destroyed renderer and must never be dereferenced against the
        // current one (a new renderer restarts its frame-serial counter and
        // reuses pool indices). Atomic because handles may be polled from a
        // thread other than the EGL thread that recreates the renderer.
        //
        // One per Magma session, drawn from one process-wide source: a handle stamped by one
        // session's renderer must read as stale against every other session's, never equal.
        std::atomic<Uint64> g_rendererGenerationSource{1};
        struct RendererGeneration {
            std::atomic<Uint64> value{g_rendererGenerationSource.fetch_add(1, std::memory_order_acq_rel)};
        };
        SessionLocal<RendererGeneration> g_rendererGeneration;
    } // namespace

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace {
        // P7 wave 2 package C, CONTRACT-P7 §5.5: MAGMA'S MIRROR OF g_glesStateObjectDeathOps
        // (DirectGLES/Managers.cpp:335), and the one arm of that table Magma has any work in.
        //
        // WHAT THE NOTICE IS FOR HERE, and it is not what it is for on Espryt. Espryt's arm
        // destroys a BACKEND TWIN keyed by lifetime id; Magma keeps no such twin - its two
        // client-minted kinds (VertexElementsCso, Buffer) are keyed on {slot, gen} by
        // MagmaPipeIdentityTables and every other kind is still reached from its frontend
        // object. So the switch Espryt runs under `#else` has nothing to do on this backend
        // and this table is exactly the EMIT arm: with a transport up and off the apply
        // thread, the death crosses as the `object_death` record
        // (MG_Remote::Client::EmitObjectDeathRecord, CONTRACT-P5C §5.2).
        //
        // WHY THAT MATTERS, measured rather than argued (package L's probe,
        // notes/p7/magma-two-process-first-run.md §6): under SPAWN the client process has no
        // DirectVulkan backend at all, so InstallClientWireTables' own emitter already
        // answered the notice and CtWireScenario's two death cases PASSED. Under INPROC the
        // server role's backend is a thread of this process, Transport is Split rather than
        // Spawn, and WireTables' install condition deliberately stands aside for "the
        // backend's own dispatcher" - which on Magma did not exist. Both cases therefore
        // FAILED on inproc with `deaths` stuck at 0. This table is that dispatcher.
        //
        // FRAMEBUFFER IS WHY THE GAP WAS NOT MERELY COSMETIC. The client-side SLOT is freed
        // backend-neutrally by P4a's per-kind helpers (MG_Impl/Pipe/PipeFill.cpp's
        // NotifyAndFree frees whatever the notice does), so PipeSlotPeek never saw a leak.
        // What leaked was the SERVER's twin: a framebuffer has NO wire delete opcode at all
        // (BRIEF-P4A D-I2), so `object_death` is its only death delivery, and without a
        // consumer here the server's record stayed Live for the life of the session.
        void OnMagmaFrontendStateObjectDestroyed(MG_Pipe::MGPipeKind kind, Uint64 lifetimeId,
                                                 MG_Pipe::MGPipeHandle /*handle*/) {
            // Monolith has no record to emit and no server twin to tell.
            if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
            // Raised ON the apply thread: the server role destroying a frontend object it
            // created itself (Magma's hidden blit / depth-mipmap resources). The client's
            // allocator is a client-thread surface (CONTRACT-P5C §3.1) and EmitObjectDeathRecord
            // probes it, so this arm stays silent exactly as Espryt's does.
            if (MG_Remote::Server::ServerLoop::OnApplyThread()) return;
            (void)MG_Remote::Client::EmitObjectDeathRecord(kind, lifetimeId);
        }

        const MG_State::GLState::StateObjectDeathOps g_magmaStateObjectDeathOps = {
            .OnDestroyed = OnMagmaFrontendStateObjectDestroyed,
        };
    } // namespace

    void InstallStateObjectDeathOps() {
        // Installed from BackendObject_DirectVulkan::Initialize(), i.e. step 1 of
        // InitServerRoleCommon (ServerLoop::CreateBackend calls Initialize), which is before
        // the client session starts and therefore before any frontend object can die.
        // Unconditional, exactly as Espryt's ResolveEsprytSlotTablesArm is: only one backend
        // is live at a time, and InstallClientWireTables' own "whoever installed first keeps
        // the notice" guard is what keeps the remote client from stomping this.
        MG_State::GLState::SetStateObjectDeathOps(&g_magmaStateObjectDeathOps);
    }

    Bool StateObjectDeathOpsInstalled() {
        return MG_State::GLState::GetStateObjectDeathOps() == &g_magmaStateObjectDeathOps;
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED

    Uint64 GetRendererGeneration() {
        return g_rendererGeneration->value.load(std::memory_order_acquire);
    }

    void BumpRendererGeneration() {
        g_rendererGeneration->value.store(g_rendererGenerationSource.fetch_add(1, std::memory_order_acq_rel),
                                          std::memory_order_release);
    }

    namespace {

        struct DrawElementsIndirectCommand {
            Uint32 count = 0;
            Uint32 instanceCount = 0;
            Uint32 firstIndex = 0;
            Int32 baseVertex = 0;
            Uint32 baseInstance = 0;
        };

        struct DrawArraysIndirectCommand {
            Uint32 count = 0;
            Uint32 instanceCount = 0;
            Uint32 first = 0;
            Uint32 baseInstance = 0;
        };



        // The verb's handles identify server stores; never inspect a client binding.
        //
        // P8-D: THE GPU READS THE WORDS (VulkanRenderer::DrawWireIndirectNative, WireDraw.inc), as
        // on the monolith arm. The CPU expansion below is kept for the one shape the monolith arm
        // also expands - a COUNT form the device cannot issue natively (no VK_KHR_draw_indirect_count,
        // or maxdrawcount > 1 without multiDrawIndirect: VulkanRenderer.cpp's
        // MultiDrawElementsIndirectCount) - where ReadWireBuffer orders GPU-produced command/count
        // bytes before the expansion.
        void DrawWireIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                              GLsizei stride, Bool indexed, Bool counted = false, GLintptr countOffset = 0) {
            if (drawcount <= 0 || countOffset < 0) return;
            if (pVulkanRenderer->DrawWireIndirectNative(mode, type, reinterpret_cast<Uint64>(indirect), drawcount,
                    stride, indexed, counted, static_cast<Uint64>(countOffset)))
                return;
            auto& buffers = pVulkanRenderer->GetWireBufferManager();
            const auto& state = MG_Pipe::MGPipeApplier();
            // P8-D: this call reads its words on the CPU; the waits those reads take are its own.
            struct ExpansionCount {
                VkBufferManager& buffers;
                const Uint64 waitsBefore = buffers.GetWireHostWaitCount();
                ~ExpansionCount() {
                    if (!MG_Util::PipeStats::Enabled()) return;
                    MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireIndirectCpuExpansions, 1);
                    MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireHostWaitsIndirect,
                                                 buffers.GetWireHostWaitCount() - waitsBefore);
                }
            } expansionCount{buffers};
            if (counted) {
                Uint32 count = 0;
                if (!buffers.ReadWireBuffer(state.VerbIndirectParameterBuffer,
                        static_cast<Uint64>(countOffset), sizeof(count), &count)) return;
                drawcount = static_cast<GLsizei>(std::min<Uint32>(count, static_cast<Uint32>(drawcount)));
                if (!drawcount) return;
            }
            const SizeT commandSize = indexed ? sizeof(DrawElementsIndirectCommand) : sizeof(DrawArraysIndirectCommand);
            if (stride == 0) stride = static_cast<GLsizei>(commandSize);
            if (stride < static_cast<GLsizei>(commandSize)) return;
            const Uint64 byteCount = static_cast<Uint64>(stride) * (drawcount - 1) + commandSize;
            if (byteCount > std::numeric_limits<SizeT>::max()) return;
            Vector<Uint8> bytes(static_cast<SizeT>(byteCount));
            if (!buffers.ReadWireBuffer(state.VerbIndirectBuffer, reinterpret_cast<Uint64>(indirect),
                    byteCount, bytes.data())) return;
            if (indexed) {
                const SizeT indexSize = MG_Util::GetGLTypeSize(type);
                if (indexSize != 1 && indexSize != 2 && indexSize != 4) return;
                Vector<DrawIndexedCmdParam> params(static_cast<SizeT>(drawcount));
                MultiDrawIndexedCmd payload{};
                payload.mode = mode;
                payload.indexBufferView.indexType = type;
                payload.drawCount = static_cast<Uint32>(drawcount);
                payload.pParams = params.data();
                for (GLsizei i = 0; i < drawcount; ++i) {
                    DrawElementsIndirectCommand command{};
                    Memcpy(&command, bytes.data() + static_cast<SizeT>(i) * stride, sizeof(command));
                    params[i] = {command.count, command.instanceCount, command.firstIndex,
                                 command.baseVertex, std::bit_cast<Int32>(command.baseInstance)};
                    const Uint64 end = (static_cast<Uint64>(command.firstIndex) + command.count) * indexSize;
                    if (end > std::numeric_limits<SizeT>::max()) return;
                    payload.indexBufferView.indexByteSize = std::max<SizeT>(payload.indexBufferView.indexByteSize,
                                                                          static_cast<SizeT>(end));
                }
                pVulkanRenderer->MultiDrawElements(payload);
            } else {
                Vector<DrawCmdParam> params(static_cast<SizeT>(drawcount));
                for (GLsizei i = 0; i < drawcount; ++i) {
                    DrawArraysIndirectCommand command{};
                    Memcpy(&command, bytes.data() + static_cast<SizeT>(i) * stride, sizeof(command));
                    params[i].vertexCount = command.count;
                    params[i].instanceCount = command.instanceCount;
                    params[i].firstVertex = command.first;
                    params[i].firstInstance = command.baseInstance;
                }
                MultiDrawCmd payload{};
                payload.mode = mode;
                payload.drawCount = static_cast<Uint32>(drawcount);
                payload.pParams = params.data();
                pVulkanRenderer->MultiDrawArrays(payload);
            }
        }

    } // namespace

    // P7 wave 2 package C, OQ-8 (CONTRACT-P7 §5.3): THE MONOLITH CONSUMER READS THE ARCHIVE
    // TOO, so one published list serves both of this backend's arms.
    //
    // THE TWO ARMS BELOW ARE NOT TWO ANSWERS. ProgramTest's
    // TheArchivesStorageBlockOrderIsTheOneSpirvReflectProduces runs the OLD arm's algorithm -
    // SPIRV-Reflect over the real modules, sorted and deduplicated exactly as
    // GetProgramResourceCache does it - over a multi-stage program with an arrayed SSBO and an
    // atomic-counter block, and asserts element-for-element equality with what the archive
    // published. That equivalence is what makes an #if here a build-time SELECTION rather than
    // a behavioural fork, and it is the reason the case exists.
    //
    // WHY AN #if AT ALL, AND WHAT IT COSTS. LinkArtifacts::storageBlocks is
    // MOBILEGL_BUILD_DISAGGREGATED-only, because one more Vector member changes the struct's
    // size, its implicit destructor and its move constructor in the PULL build whose .text G1
    // pins byte-for-byte (0xa52203). So the pull build keeps `g_programResourceCaches`, the
    // SPIRV-Reflect rebuild and `ClearProgramResourceCaches`, verbatim; only this build drops
    // them - and with them the rehash hazard the ordering comment below the block-binding
    // setter documents. Retiring the pull half is the day the member stops needing its guard,
    // which is a G1/P13 decision and not a wave-2 one; recorded in notes/p7/magma-c.md.
    void ClearProgramResourceCaches() {
        // Nothing to clear: there is no cache in this build. Kept as an entry point so
        // BackendObject_DirectVulkan's two EGL-teardown call sites stay statement for
        // statement what they are in the pull build.
    }

    GLuint GetShaderStorageBlockIndex(const MG_State::GLState::ProgramObject& program, const String& name) {
        const auto& blocks = program.GetLinkReflection().storageBlocks;
        const auto find = [&blocks](const String& key) {
            return std::find_if(blocks.begin(), blocks.end(),
                [&](const MG_State::GLState::StorageBlockReflection& block) { return block.name == key; });
        };
        auto it = find(name);
        if (it == blocks.end()) {
            // Archive names are normalised, so an arrayed block that GL enumerates per element
            // - "B[0]", "B[1]" - is one entry here, spelled "B". Same second chance the cache
            // gave, for the same callers.
            const auto bracket = name.rfind('[');
            if (bracket == String::npos || name.empty() || name.back() != ']') return GL_INVALID_INDEX;
            it = find(name.substr(0, bracket));
            if (it == blocks.end()) return GL_INVALID_INDEX;
        }
        return static_cast<GLuint>(std::distance(blocks.begin(), it));
    }

    GLuint GetShaderStorageBlockBinding(const MG_State::GLState::ProgramObject& program, GLuint blockIndex) {
        const auto& blocks = program.GetLinkReflection().storageBlocks;
        if (blockIndex >= blocks.size()) {
            return 0;
        }
        // THE OVERRIDE IS APPLIED ON READ, where the cache used to apply it on rebuild and
        // patch it in place on rebind. Same answer by a shorter route: the program is the
        // authoritative record of a rebound block (it is what GL_BUFFER_BINDING reports), the
        // archive carries the DECLARED binding only, and asking the program here means a
        // rebind can no longer be lost to a cache rebuild that happened to race a state-version
        // bump.
        const Int rebound = program.GetShaderStorageBlockBindingOverride(blocks[blockIndex].name);
        if (rebound >= 0) return static_cast<GLuint>(rebound);
        return blocks[blockIndex].binding;
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferfi called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::ClearBufferfi called with null GL context");
        pVulkanRenderer->ClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferfv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::ClearBufferfv called with null GL context");
        pVulkanRenderer->ClearBufferfv(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferuiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::ClearBufferuiv called with null GL context");
        pVulkanRenderer->ClearBufferuiv(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClearBufferiv called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::ClearBufferiv called with null GL context");
        pVulkanRenderer->ClearBufferiv(buffer, drawbuffer, value);
    }

    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
        DrawWireIndirect(mode, type, indirect, drawcount, stride, true);
        return;
    }
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        DrawWireIndirect(mode, 0, indirect, drawcount, stride, false);
        return;
    }
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
        DrawWireIndirect(mode, type, indirect, maxdrawcount, stride, true, true, drawcount);
        return;
    }
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride) {
        DrawWireIndirect(mode, 0, indirect, maxdrawcount, stride, false, true, drawcount);
        return;
    }
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        (void)start;
        (void)end;
        DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        (void)start;
        (void)end;
        DrawElements(mode, count, type, indices);
    }
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsInstancedBaseVertexBaseInstance called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DrawElementsInstancedBaseVertexBaseInstance called with null GL context");

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = instancecount;
        payload.params.firstIndex = 0;
        payload.params.vertexOffset = basevertex;
        payload.params.firstInstance = static_cast<Int32>(baseinstance);
        pVulkanRenderer->DrawElements(payload);
    }
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, basevertex, 0);
    }
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, 0, baseinstance);
    }
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instancecount, 0, 0);
    }
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        DrawWireIndirect(mode, type, indirect, 1, 0, true);
        return;
    }
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArraysInstancedBaseInstance called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DrawArraysInstancedBaseInstance called with null GL context");

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.vertexCount = count;
        payload.params.instanceCount = instancecount;
        payload.params.firstVertex = first;
        payload.params.firstInstance = baseinstance;
        pVulkanRenderer->DrawArrays(payload);
    }
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        DrawArraysInstancedBaseInstance(mode, first, count, instancecount, 0);
    }
    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        DrawWireIndirect(mode, 0, indirect, 1, 0, false);
        return;
    }
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyTexImage2D called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::CopyTexImage2D called with null GL context");
        pVulkanRenderer->CopyTexSubImage2D(target, level, 0, 0, x, y, width, height);
    }
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyTexSubImage2D called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::CopyTexSubImage2D called with null GL context");
        pVulkanRenderer->CopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
    }
    void CopyImageSubData(const CopyImageEndpoint& src,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const CopyImageEndpoint& dst,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::CopyImageSubData called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::CopyImageSubData called with null GL context");
        pVulkanRenderer->CopyImageSubData(src, srcTarget, srcLevel, srcX, srcY, srcZ,
                                          dst, dstTarget, dstLevel, dstX, dstY, dstZ,
                                          srcWidth, srcHeight, srcDepth);
    }
    void GenerateMipmap(GLenum target) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GenerateMipmap called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::GenerateMipmap called with null GL context");
        pVulkanRenderer->GenerateMipmap(target);
    }

    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DispatchCompute called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DispatchCompute called with null GL context");
        pVulkanRenderer->DispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void DispatchComputeIndirect(GLintptr indirect) {
        // P8-SV: THE GPU READS THE GROUP COUNTS on the wire arm too - VulkanRenderer's
        // DispatchComputeIndirect takes its transport branch into DispatchWireComputeIndirect
        // (WireDraw.inc), which issues vkCmdDispatchIndirect from the verb's store. The CPU read
        // that was here (ReadWireBuffer, a whole-GPU wait once a shader had written the store) is gone.
        pVulkanRenderer->DispatchComputeIndirect(indirect);
        return;
    }

    void MemoryBarrier(GLbitfield barriers) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MemoryBarrier called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::MemoryBarrier called with null GL context");
        pVulkanRenderer->MemoryBarrier(barriers);
    }

    void MemoryBarrierByRegion(GLbitfield barriers) {
        MemoryBarrier(barriers);
    }

    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format) {
        (void)unit;
        (void)texture;
        (void)level;
        (void)layered;
        (void)layer;
        (void)access;
        (void)format;
    }

    // The two compute limits are the only indexed pnames a backend genuinely owns: they come
    // from the physical device, and MG_Impl/GLImpl/Getter/GL_Getter.cpp asks for them here so it
    // can raise the answer to the GL required minimum. The same six numbers are carried in
    // DynamicBackendParameters::MaxComputeWorkGroupCount/Size (filled at capability init from
    // the same limits), which is their MGPCaps carrier once this entry retires - the
    // AdvertisedLimitsScenario pins the two against each other. Every other indexed pname names FRONTEND
    // state (the indexed buffer bindings, the per-unit texture/sampler bindings, the image-unit
    // bindings, the viewport rectangles, the indexed capabilities) and is answered there before
    // the table is consulted, so the arms this function used to carry for
    // GL_SHADER_STORAGE_BUFFER_* and GL_IMAGE_BINDING_* were unreachable duplicates - and not
    // even faithful ones: the frontend reports the range glBindBufferRange was ASKED for,
    // verbatim, while these clamped it to the buffer's current storage.
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data) {
        if (!data) return;
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetIntegeri_v called with null VulkanRenderer");
        if (index >= 3) {
            *data = 0;
            return;
        }
        switch (target) {
        case GL_MAX_COMPUTE_WORK_GROUP_COUNT:
            *data = static_cast<GLint>(
                pVulkanRenderer->GetPhysicalDevice().properties.limits.maxComputeWorkGroupCount[index]);
            return;
        case GL_MAX_COMPUTE_WORK_GROUP_SIZE:
            *data = static_cast<GLint>(
                pVulkanRenderer->GetPhysicalDevice().properties.limits.maxComputeWorkGroupSize[index]);
            return;
        default:
            *data = 0;
            return;
        }
    }

    void ShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding) {
        auto& state = MG_Pipe::MGPipeApplier();
        const auto handle = state.VerbStorageBlockProgram;
        if (handle.Slot >= state.ShaderCsos.size() || !state.ShaderCsos[handle.Slot].Live ||
            state.ShaderCsos[handle.Slot].Gen != handle.Gen || !storageBlockName) {
            MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"Magma:storage-block-program-record\"}");
            std::abort();
        }
        auto& record = state.ShaderCsos[handle.Slot];
        auto found = std::find_if(record.StorageOverrides.begin(), record.StorageOverrides.end(),
            [&](const auto& entry) { return entry.Name == storageBlockName; });
        if (found == record.StorageOverrides.end())
            record.StorageOverrides.push_back({storageBlockName, static_cast<Int32>(storageBlockBinding)});
        else found->Binding = static_cast<Int32>(storageBlockBinding);
        ++record.BindingsSerial;
        return;
        // P7 wave 2 package C, OQ-8: NOTHING LEFT TO PATCH, and the hazard goes with it.
        //
        // The frontend has already recorded the new binding on the program, and that record is
        // now what GetShaderStorageBlockBinding reads (it asks
        // GetShaderStorageBlockBindingOverride on every call, over an IMMUTABLE archive list).
        // What stood here was the other half of a mutable cache: resolve the index, take a
        // reference into g_programResourceCaches and patch `binding` in place so an
        // already-built entry need not be thrown away.
        //
        // THE HAZARD THAT CAME WITH IT IS GONE TOO, and it is worth naming because it was a
        // real crash rather than a theoretical one: GetShaderStorageBlockIndex re-entered
        // GetProgramResourceCache, which indexes an OPEN-ADDRESSED map and can therefore
        // insert, and a rehash moves entries - so a reference taken before that call dangled.
        // Binding one program's storage block while another program's entry was still absent
        // from the cache was a reproducible segfault (ProgramPipelineScenario's two
        // storage-block cases, in one process). The ordering above was the fix; having no
        // cache is the retirement. The pull build below keeps both.
        (void)storageBlockBinding;
    }
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ReadPixels called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::ReadPixels called with null GL context");
        pVulkanRenderer->ReadPixels(x, y, width, height, format, type, pixels);
    }
    void Clear(GLbitfield mask) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::Clear called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::Clear called with null GL context");
        pVulkanRenderer->Clear(mask);
    }

    // Vulkan has no LINE_LOOP topology; rewrite the draw as an indexed LINE_STRIP
    // whose synthesized index list revisits the first vertex at the end.
    static void DrawLineLoopAsIndexedStrip(const Vector<Uint32>& closedIndices, GLint basevertex) {
        DrawIndexedCmd payload{};
        payload.mode = GL_LINE_STRIP;
        payload.indexBufferView.indexType = GL_UNSIGNED_INT;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(closedIndices.data());
        payload.indexBufferView.indexByteSize = closedIndices.size() * sizeof(Uint32);
        payload.indexBufferView.forceClientMemory = true;
        payload.params.indexCount = static_cast<Uint32>(closedIndices.size());
        payload.params.instanceCount = 1;
        payload.params.vertexOffset = basevertex;
        pVulkanRenderer->DrawElements(payload);
    }

    // Resolve a DrawElements index list (bound element-array buffer or client
    // memory) into uint32 values with the loop-closing first index appended.
    static Bool BuildClosedLineLoopIndices(GLsizei count, GLenum type, const void* indices,
                                           Vector<Uint32>& outIndices) {
        const SizeT width = MG_Util::GetGLTypeSize(type);
        if ((width != 1 && width != 2 && width != 4) || count < 2) return false;
        const auto& bound = MG_Pipe::MGPipeApplier().IndexBuffer;
        Vector<Uint8> owned;
        const Uint8* bytes = static_cast<const Uint8*>(indices);
        if (!MG_Pipe::MGPipeHandleIsNull(bound.Res)) {
            owned.resize(static_cast<SizeT>(count) * width);
            const Uint64 offset = bound.Offset + reinterpret_cast<Uint64>(indices);
            if (offset < bound.Offset || !pVulkanRenderer->GetWireBufferManager().ReadWireBuffer(
                    bound.Res, offset, owned.size(), owned.data())) return false;
            bytes = owned.data();
        }
        if (!bytes) return false;
        outIndices.resize(static_cast<SizeT>(count) + 1);
        for (GLsizei i = 0; i < count; ++i) {
            outIndices[i] = 0;
            Memcpy(&outIndices[i], bytes + static_cast<SizeT>(i) * width, width);
        }
        outIndices[count] = outIndices[0];
        return true;
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawArrays called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DrawArrays called with null GL context");

        if (mode == GL_LINE_LOOP) {
            if (count < 2) {
                return;
            }
            Vector<Uint32> closedIndices(static_cast<SizeT>(count) + 1);
            for (GLsizei i = 0; i < count; ++i) {
                closedIndices[i] = static_cast<Uint32>(first + i);
            }
            closedIndices[count] = static_cast<Uint32>(first);
            DrawLineLoopAsIndexedStrip(closedIndices, 0);
            return;
        }

        DrawCmd payload{};
        payload.mode = mode;
        payload.params.firstVertex = first;
        payload.params.vertexCount = count;

        pVulkanRenderer->DrawArrays(payload);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElements called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DrawElements called with null GL context");

        if (mode == GL_LINE_LOOP) {
            Vector<Uint32> closedIndices;
            if (BuildClosedLineLoopIndices(count, type, indices, closedIndices)) {
                DrawLineLoopAsIndexedStrip(closedIndices, 0);
            }
            return;
        }

        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = 1;

        // A CLIENT-MEMORY VERTEX ARRAY'S upload is bounded by a scan of this draw's index bytes,
        // and those bytes may be shader-written: the scan reconciles the element buffer's shadow,
        // which WAITS for the GPU. A wait in the middle of the draw's own recording flushes the
        // batch that command buffer belongs to - the handle SetupDraw is holding goes stale, and
        // the next vkCmd* records into a command buffer that is no longer the frame's. Reconciling
        // HERE, before SetupDraw starts recording, keeps the wait out of the recording.

        pVulkanRenderer->DrawElements(payload);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawArrays called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::MultiDrawArrays called with null GL context");
        if (drawcount <= 0) {
            return;
        }

        MultiDrawCmd payload{};
        payload.mode = mode;

        // TODO: allocate draw cmd buf elsewhere
        static thread_local Vector<DrawCmdParam> params;
        params.clear();
        params.resize(drawcount);

        for (GLsizei i = 0; i < drawcount; ++i) {
            auto& param = params[i];
            param.vertexCount = count[i] > 0 ? static_cast<Uint32>(count[i]) : 0;
            param.instanceCount = 1;
            param.firstVertex = first[i] > 0 ? static_cast<Uint32>(first[i]) : 0;
            param.firstInstance = 0;
        }
        payload.drawCount = static_cast<Uint32>(drawcount);
        payload.pParams = params.data();
        pVulkanRenderer->MultiDrawArrays(payload);
    }

    // Shared body of glMultiDrawElements (basevertex == nullptr) and
    // glMultiDrawElementsBaseVertex: identical calls except for the per-draw
    // vertex offset, which VkMultiDrawIndexedInfoEXT / VkDrawIndexedIndirectCommand /
    // vkCmdDrawIndexed all carry natively.
    static void MultiDrawElementsImpl(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                      GLsizei drawcount, const GLint* basevertex) {

        if (drawcount <= 0) {
            return;
        }

        // With no element-array buffer bound, every indices[i] is a client pointer into a
        // separate CPU allocation, not an offset into one shared buffer. The batched payload
        // below cannot express that: it carries ONE index-buffer view for the whole batch and
        // turns each pointer into a firstIndex relative to it. Replay the sub-draws through
        // the single-draw entry point instead - it snapshots each client range into its own
        // transient slice, which is exactly what the unrolled draws this must match do.
        // (The batch used to be built this way; the shared-view rewrite that added
        // MultiDrawIndexedCmd left the client-memory shape addressing a view whose byte
        // offset is a hardcoded 0, so UploadAndBindIndexBuffer saw a null client pointer,
        // declined the whole batch and painted nothing.)
        const Bool noIndexBuffer = MG_Pipe::MGPipeHandleIsNull(MG_Pipe::MGPipeApplier().IndexBuffer.Res);
        if (noIndexBuffer) {
            for (GLsizei i = 0; i < drawcount; ++i) {
                if (count[i] <= 0) {
                    continue;
                }
                DrawElementsBaseVertex(mode, count[i], type, indices[i],
                                       basevertex != nullptr ? basevertex[i] : 0);
            }
            return;
        }

        MultiDrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;

        // Loop-invariant: the index type is fixed for the whole multi-draw, so resolve
        // its byte size once instead of twice per sub-draw (a cross-TU switch that
        // showed up in per-frame profiles of sodium-style 132x32 multi-draws). Index
        // sizes are 1/2/4, so the per-sub-draw offset division below reduces to a
        // shift - the hardware divide was the hottest instruction of this loop.
        const SizeT indexSize = MG_Util::GetGLTypeSize(type);
        if (indexSize == 0) {
            MGLOG_E_ONCE("MultiDrawElements skipped: unsupported index type 0x%x", type);
            return;
        }
        const Uint32 indexSizeShift = static_cast<Uint32>(std::countr_zero(indexSize));

        // TODO: allocate draw cmd buf elsewhere
        static thread_local Vector<DrawIndexedCmdParam> params;
        params.clear();
        params.resize(drawcount);

        for (GLsizei i = 0; i < drawcount; ++i) {
            if (count[i] == 0) {
                continue;
            }

            // TODO: this index view needs a redesign, now there's a lotta redundant uploads

            payload.indexBufferView.indexByteOffset = 0;
            payload.indexBufferView.indexByteSize =
                    std::max(reinterpret_cast<SizeT>(indices[i]) + count[i] * indexSize,
                             payload.indexBufferView.indexByteSize);

            auto& param = params[i];

            param.indexCount = count[i];
            param.instanceCount = 1;
            param.firstIndex = reinterpret_cast<SizeT>(indices[i]) >> indexSizeShift;
            param.vertexOffset = basevertex != nullptr ? basevertex[i] : 0;
            param.firstInstance = 0;
        }
        payload.drawCount = drawcount;
        payload.pParams = params.data();
        pVulkanRenderer->MultiDrawElements(payload);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::DrawElementsBaseVertex called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::DrawElementsBaseVertex called with null GL context");
        if (mode == GL_LINE_LOOP) {
            Vector<Uint32> closedIndices;
            if (BuildClosedLineLoopIndices(count, type, indices, closedIndices)) {
                DrawLineLoopAsIndexedStrip(closedIndices, basevertex);
            }
            return;
        }
        DrawIndexedCmd payload{};
        payload.mode = mode;
        payload.indexBufferView.indexType = type;
        payload.indexBufferView.indexByteOffset = reinterpret_cast<SizeT>(indices);
        payload.indexBufferView.indexByteSize = count * MG_Util::GetGLTypeSize(type);
        payload.params.indexCount = count;
        payload.params.instanceCount = 1;
        payload.params.firstIndex = 0;
        payload.params.vertexOffset = basevertex;
        payload.params.firstInstance = 0;
        pVulkanRenderer->DrawElements(payload);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::MultiDrawElementsBaseVertex called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::MultiDrawElementsBaseVertex called with null GL context");
        MultiDrawElementsImpl(mode, count, type, indices, drawcount, basevertex);
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BlitFramebuffer called with null VulkanRenderer");
        MOBILEGL_ASSERT(MG_Pipe::gPipeInputs.IsLive(), "DirectVulkan::BlitFramebuffer called with null GL context");
        pVulkanRenderer->BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    }

    namespace {
        // Backend fence handle: the queue-submission index captured at fence
        // creation (see VulkanRenderer::GetSyncPointSubmitIndex). The fence is
        // signaled once that submission's VkFence has been observed signaled,
        // so completion tracks the GPU itself rather than the frame-count
        // inference; MC 1.21.5's fence-paced ring buffers depend on this to
        // recycle their space instead of growing without bound.
        struct VulkanSyncObject {
            Uint64 submitIndex = 0;
            // Renderer generation the index was issued under (see
            // g_rendererGeneration). A stale generation reports the fence
            // signaled: renderer destruction waits for device idle, so the
            // old renderer's GPU work is long complete, and the index must
            // not be compared against the new renderer's restarted counter.
            Uint64 rendererGeneration = 0;
            // P13 W4: status polls answered "not yet" while the batch was still unsubmitted.
            mutable Uint32 unsubmittedPolls = 0;
        };
    } // namespace

    BackendSyncHandle FenceSync() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::FenceSync called with null VulkanRenderer");
        return new VulkanSyncObject{pVulkanRenderer->GetSyncPointSubmitIndex(), GetRendererGeneration()};
    }

    GLenum ClientWaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        const auto* sync = static_cast<VulkanSyncObject*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::ClientWaitSync called with null VulkanRenderer");
        if (sync == nullptr || sync->rendererGeneration != GetRendererGeneration()) {
            return GL_ALREADY_SIGNALED;
        }
        if (pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex)) {
            return GL_ALREADY_SIGNALED;
        }
        // GL_SYNC_FLUSH_COMMANDS_BIT: flush regardless of timeout, so a
        // zero-timeout poll loop makes progress across calls - but only when
        // the sync's batch is still unsubmitted; flushing for an already
        // submitted fence cannot advance it and would split the frame's
        // render pass on every poll.
        if ((flags & GL_SYNC_FLUSH_COMMANDS_BIT) != 0) {
            pVulkanRenderer->FlushForSyncPoint(sync->submitIndex);
        }
        if (timeout == 0) {
            return pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex) ? GL_ALREADY_SIGNALED
                                                                             : GL_TIMEOUT_EXPIRED;
        }
        // Blocking wait: flush even without the flush bit - the sync's batch
        // can only be submitted from this thread, so waiting on an unflushed
        // fence would otherwise burn the full timeout with no chance of
        // success.
        return pVulkanRenderer->WaitForSubmitIndex(sync->submitIndex, timeout, /*flushIfPending=*/true)
                   ? GL_CONDITION_SATISFIED
                   : GL_TIMEOUT_EXPIRED;
    }

    void WaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        // Server-side waits are implicit: the single graphics queue executes
        // submissions in order, so later GPU work already observes everything
        // recorded before the fence.
        (void)handle;
        (void)flags;
        (void)timeout;
    }

    void DeleteSync(BackendSyncHandle handle) {
        delete static_cast<VulkanSyncObject*>(handle);
    }

    Bool GetSyncStatus(BackendSyncHandle handle) {
        const auto* sync = static_cast<VulkanSyncObject*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetSyncStatus called with null VulkanRenderer");
        if (sync == nullptr || sync->rendererGeneration != GetRendererGeneration()) {
            return true;
        }
        // Pure status read (glGetSynciv must not flush).
        if (pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex)) return true;
        // P13 W4: A STATUS POLL THAT CAN NEVER BE ANSWERED. On the record arm a frame's draws stay
        // in one command buffer (and one render pass) until something submits it, so a client that
        // only polls - glGetSynciv in a loop, no flush bit, no swap - waits for work nothing will
        // ever hand the GPU. Under split the apply thread submits when it goes idle (the server's
        // fence report, one ClientWaitSync(FLUSH, 0)); monolith has no idle moment, so a poller that
        // keeps asking the same unsubmitted fence is that moment. The threshold keeps an ordinary
        // once-a-frame status read from splitting the frame it is polled in.
        constexpr Uint32 kPollsBeforeIdleFlush = 64;
        if (MG_Config::RecordArmAliasesFrontend() && ++sync->unsubmittedPolls >= kPollsBeforeIdleFlush) {
            sync->unsubmittedPolls = 0;
            pVulkanRenderer->FlushForSyncPoint(sync->submitIndex);
            return pVulkanRenderer->IsSubmitIndexComplete(sync->submitIndex);
        }
        return false;
    }

    namespace {
        // Backend timer-query handle: a TIME_ELAPSED span holds a begin and an
        // end timestamp record; a GL_TIMESTAMP one-shot holds only `end`. The
        // records are shared (SharedPtr) with the owning pool's pending list,
        // so deleting the query while results are still in flight is safe.
        struct VulkanTimerQuery {
            enum class Kind : Uint8 { Timer, Occlusion, XfbWritten, XfbGenerated };
            Kind kind = Kind::Timer;
            SharedPtr<VkTimerQueryManager::TimestampRecord> begin;
            SharedPtr<VkTimerQueryManager::TimestampRecord> end;
            // Kind::Occlusion - pool slots recorded between Begin/End; summed at result time.
            Vector<Uint32> occlusionSlots;
            // Kind::XfbGenerated - reroute-pool slots for the span's XFB-INACTIVE
            // draws, where the renderer's reroute is armed (the affected driver's
            // stream query counts nothing without an open capture; see
            // VulkanRenderer::BeginXfbQueryForDraw). Summed alongside the stream
            // slots above, which keep the span's XFB-active draws.
            Vector<Uint32> rerouteSlots;
            // Renderer generation the records were written under (see
            // g_rendererGeneration). A stale generation resolves as available
            // with a final zero result: the records' pool indices and frame
            // serials refer to a destroyed renderer and must never be handed
            // to the current one. DeleteBackendQuery only frees the wrapper
            // (and, via the SharedPtrs, the records), never pool slots, so
            // stale queries are always safe to delete.
            Uint64 rendererGeneration = 0;
            // Kind::XfbGenerated - the frontend's paused-draw primitive counter when the
            // query began. On the affected drivers VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT
            // counts only what the capture saw, so a draw made while the span was paused is
            // invisible to it - but GL_PRIMITIVES_GENERATED counts what the last vertex
            // processing stage emitted regardless. The delta closes that gap at result time.
            Uint64 pausedPrimitiveSnapshot = 0;
            // ...unless the GPU already counted those paused draws when the span opened -
            // through the reroute pool (VulkanRenderer::BeginXfbQueryForDraw reroutes every
            // draw with no open capture, paused ones included) or, where the probe measured
            // the stream query as counting capture-less draws, through the stream slot the
            // paused draw still takes. Adding the CPU delta on top would count them twice,
            // and the CPU counter is the weaker source anyway: only 3 of the ~15 draw entry
            // points write it and it answers 0 for GL_PATCHES.
            Bool pausedPrimitivesCountedByGpu = false;
        };
    } // namespace

    Bool IsTimerQuerySupported() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // The first caps snapshot is sent during the transport handshake,
        // before eglMakeCurrent creates the server's Vulkan renderer.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith && !pVulkanRenderer) return false;
#endif
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::IsTimerQuerySupported called with null VulkanRenderer");
        return pVulkanRenderer->IsTimerQuerySupported();
    }

    BackendQueryHandle BeginTimeElapsedQuery() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginTimeElapsedQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->IsTimerQuerySupported()) {
            return nullptr;
        }
        auto begin = pVulkanRenderer->WriteTimerQueryTimestamp();
        if (!begin) {
            // Pool exhausted this frame; the frontend falls back on a null handle.
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->begin = std::move(begin);
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    void EndTimeElapsedQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndTimeElapsedQuery called with null VulkanRenderer");
        if (query == nullptr) {
            return;
        }
        if (query->rendererGeneration != GetRendererGeneration()) {
            // The span began under a renderer that has since been destroyed;
            // never write into the new renderer's pools on its behalf. The
            // query resolves as available with a zero result.
            return;
        }
        // May be null on pool exhaustion; the query then reads back as 0.
        query->end = pVulkanRenderer->WriteTimerQueryTimestamp();
    }

    BackendQueryHandle QueryCounterTimestamp() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::QueryCounterTimestamp called with null VulkanRenderer");
        if (!pVulkanRenderer->IsTimerQuerySupported()) {
            return nullptr;
        }
        auto record = pVulkanRenderer->WriteTimerQueryTimestamp();
        if (!record) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->end = std::move(record);
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    Bool IsQueryResultAvailable(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        // Degraded/stale handles report available; GetQueryResult64 then
        // resolves them with a final zero result.
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::IsQueryResultAvailable called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return true;
        }
        if (query->begin && !pVulkanRenderer->IsTimerQueryResultReady(*query->begin)) {
            return false;
        }
        if (query->end && !pVulkanRenderer->IsTimerQueryResultReady(*query->end)) {
            return false;
        }
        return true;
    }

    Bool GetQueryResult64(BackendQueryHandle handle, Bool wait, Uint64* outNanoseconds) {
        *outNanoseconds = 0;
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::GetQueryResult64 called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            // The records belong to a destroyed renderer: no real value can
            // ever be produced, so resolve with a final 0.
            return true;
        }
        if (query->kind == VulkanTimerQuery::Kind::Occlusion) {
            Uint64 samples = 0;
            if (!pVulkanRenderer->ResolveOcclusionQueryResult(query->occlusionSlots, samples)) {
                return false;
            }
            query->occlusionSlots.clear(); // slots are recycled by the resolve
            *outNanoseconds = samples;
            return true;
        }
        if (query->kind == VulkanTimerQuery::Kind::XfbWritten ||
            query->kind == VulkanTimerQuery::Kind::XfbGenerated) {
            Uint64 primitives = 0;
            if (!pVulkanRenderer->ResolveXfbQueryResult(query->occlusionSlots, query->rerouteSlots,
                                                        query->kind == VulkanTimerQuery::Kind::XfbGenerated,
                                                        primitives)) {
                return false;
            }
            if (query->kind == VulkanTimerQuery::Kind::XfbGenerated &&
                !query->pausedPrimitivesCountedByGpu &&
#if MOBILEGL_BUILD_DISAGGREGATED
                MG_Config::Transport == MG_Config::TransportMode::Monolith &&
#endif
                MG_Pipe::gPipeInputs.IsLive()) {
                primitives += MG_Pipe::gPipeInputs.GetTransformFeedbackPausedPrimitiveCounter() -
                              query->pausedPrimitiveSnapshot;
            }
            *outNanoseconds = primitives;
            return true;
        }
        // With wait, mirrors ClientWaitSync: a query ended this frame cannot
        // complete until Present submits the commands, so the wait refuses to
        // block on the current unsubmitted serial. Returning false keeps the
        // handle alive in the frontend; the query stays readable once a later
        // Present submits the frame.
        const auto ensureReady = [&](VkTimerQueryManager::TimestampRecord& record) {
            return wait ? pVulkanRenderer->WaitForTimerQueryResult(record)
                        : pVulkanRenderer->IsTimerQueryResultReady(record);
        };
        if (query->begin && query->end) {
            if (!ensureReady(*query->begin) || !ensureReady(*query->end)) {
                return false;
            }
            *outNanoseconds = pVulkanRenderer->GetTimerQueryElapsedNs(*query->begin, *query->end);
            return true;
        }
        if (query->end) {
            if (!ensureReady(*query->end)) {
                return false;
            }
            *outNanoseconds = pVulkanRenderer->GetTimerQueryTimestampNs(*query->end);
            return true;
        }
        // TIME_ELAPSED span that never got its end timestamp (pool
        // exhaustion): nothing further can arrive, resolve with a final 0.
        return true;
    }

    void DeleteBackendQuery(BackendQueryHandle handle) {
        delete static_cast<VulkanTimerQuery*>(handle);
    }

    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated) {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginXfbPrimitivesQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->StartXfbQueryCapture(generated ? 1u : 0u)) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->kind = generated ? VulkanTimerQuery::Kind::XfbGenerated : VulkanTimerQuery::Kind::XfbWritten;
        query->rendererGeneration = GetRendererGeneration();
        query->pausedPrimitiveSnapshot =
#if MOBILEGL_BUILD_DISAGGREGATED
            MG_Config::Transport == MG_Config::TransportMode::Monolith &&
#endif
            MG_Pipe::gPipeInputs.IsLive() ? MG_Pipe::gPipeInputs.GetTransformFeedbackPausedPrimitiveCounter() : 0;
        // Read AFTER StartXfbQueryCapture, which is where a failed reroute-pool creation
        // disarms: the answer is then what this span will actually do for every draw.
        query->pausedPrimitivesCountedByGpu = generated && pVulkanRenderer->ArePausedDrawsGpuCounted();
        return query;
    }

    void EndXfbPrimitivesQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndXfbPrimitivesQuery called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return;
        }
        pVulkanRenderer->StopXfbQueryCapture(
            query->kind == VulkanTimerQuery::Kind::XfbGenerated ? 1u : 0u, query->occlusionSlots,
            query->rerouteSlots);
    }

    BackendQueryHandle BeginOcclusionQuery() {
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::BeginOcclusionQuery called with null VulkanRenderer");
        if (!pVulkanRenderer->StartOcclusionQueryCapture()) {
            return nullptr;
        }
        auto* query = new VulkanTimerQuery{};
        query->kind = VulkanTimerQuery::Kind::Occlusion;
        query->rendererGeneration = GetRendererGeneration();
        return query;
    }

    void EndOcclusionQuery(BackendQueryHandle handle) {
        auto* query = static_cast<VulkanTimerQuery*>(handle);
        MOBILEGL_ASSERT(pVulkanRenderer, "DirectVulkan::EndOcclusionQuery called with null VulkanRenderer");
        if (query == nullptr || query->rendererGeneration != GetRendererGeneration()) {
            return;
        }
        pVulkanRenderer->StopOcclusionQueryCapture(query->occlusionSlots);
    }

    Int64 GetGpuTimestampNs() {
        // Vulkan cannot synchronously sample the GPU clock: timestamps only
        // exist as vkCmdWriteTimestamp results read back later, and
        // VK_EXT_calibrated_timestamps is not wired up. Returning 0 tells the
        // frontend GL_TIMESTAMP getter to fall back.
        return 0;
    }

    void Present() {
        // A served session's renderer is gone once its client released its resources; a present
        // that still reaches it has nothing to show and is dropped rather than dereferenced.
        if (!pVulkanRenderer) {
            MGLOG_E_ONCE("DirectVulkan: a present reached a session whose renderer is already released; dropped");
            return;
        }
        pVulkanRenderer->Present();
        // THE frame boundary for the MGPipe counters, at the backend entry point rather
        // than inside VulkanRenderer::Present: that function has an early return for the
        // no-usable-swapchain case, and a suspended frame is still a frame the counters
        // must close.
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::OnPresent();
        }
    }

    namespace {
        // Per session: each client asks for its own interval.
        SessionLocal<Optional<Int>> g_requestedSwapInterval;
    } // namespace

    void SetSwapInterval(Int interval) {
        *g_requestedSwapInterval = interval;
        if (pVulkanRenderer) {
            pVulkanRenderer->SetSwapInterval(interval);
        }
    }

    Optional<Int> GetRequestedSwapInterval() {
        return *g_requestedSwapInterval;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    void ForgetRequestedSwapInterval() {
        g_requestedSwapInterval->reset();
    }
#endif

    DescriptorPoolCensus GetDescriptorPoolCensus() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // P11 M2: UNDER A TRANSPORT THE POOLS ARE THE SERVER RENDERER'S, and its apply thread
        // grows, rewinds and trims them while the test thread would be reading - a census taken
        // after a swap the client ran ahead of, or mid-burst, walks a set cache BeginFrame may be
        // clearing. Inproc, the server is a thread of this process: the read is posted to it as
        // the test probe, which runs between drain batches, so it sees every record published
        // before it and none half-applied - without waiting on the GPU, which is the point of the
        // trim case (it gives a burst back with other slots' work in flight). Spawn and tcp keep
        // their pools in another process; the client has no renderer and answers unavailable.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            !MG_Remote::Server::ServerLoop::OnApplyThread()) {
            if (MG_Config::Transport != MG_Config::TransportMode::InProcess) return DescriptorPoolCensus{};
            DescriptorPoolCensus census{};
            const MobileGLResult result = MG_Remote::Server::ServerLoopInstance().RunProbeOnApplyThreadForTesting(
                +[](void* user) -> MobileGLResult {
                    *static_cast<DescriptorPoolCensus*>(user) =
                        pVulkanRenderer ? pVulkanRenderer->GetDescriptorPoolCensus() : DescriptorPoolCensus{};
                    return MOBILEGL_OK;
                },
                &census);
            return result == MOBILEGL_OK ? census : DescriptorPoolCensus{};
        }
#endif
        return pVulkanRenderer ? pVulkanRenderer->GetDescriptorPoolCensus() : DescriptorPoolCensus{};
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
