// MobileGL - MobileGL/MG_Backend/DirectVulkan/DirectVulkan.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Backend/BackendObject.h>
#include "MagmaSession.h"
#include "Renderer/VulkanRenderer.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    // The renderer of the calling thread's Magma session (MagmaSession.h): a served session's own on
    // its apply thread, the process-wide one everywhere else. Spelled like the UniquePtr it used to
    // be so the call sites read the same.
    class RendererSlot {
    public:
        UniquePtr<VulkanRenderer>& Slot() const { return m_slot.Get(); }
        VulkanRenderer* get() const { return Slot().get(); }
        VulkanRenderer* operator->() const { return get(); }
        VulkanRenderer& operator*() const { return *get(); }
        explicit operator bool() const { return get() != nullptr; }
        RendererSlot& operator=(UniquePtr<VulkanRenderer>&& renderer) {
            Slot() = std::move(renderer);
            return *this;
        }
        void reset() { Slot().reset(); }
        friend Bool operator==(const RendererSlot& slot, std::nullptr_t) { return slot.get() == nullptr; }
        friend Bool operator!=(const RendererSlot& slot, std::nullptr_t) { return slot.get() != nullptr; }

    private:
        SessionLocal<UniquePtr<VulkanRenderer>> m_slot;
    };
    extern RendererSlot pVulkanRenderer;

    // Generation of the live VulkanRenderer instance, mirroring DirectGLES's
    // g_syncContextGeneration. BackendObject_DirectVulkan bumps it wherever
    // pVulkanRenderer is reset or recreated; fence and timer-query handles
    // stamped with an older generation are stale and resolve as signaled /
    // available with zero results instead of dereferencing the destroyed
    // renderer's frame serials and query-pool slots.
    Uint64 GetRendererGeneration();
    void BumpRendererGeneration();

#if MOBILEGL_BUILD_DISAGGREGATED
    // P7 wave 2 package C (CONTRACT-P7 §5.5). Magma's StateObjectDeathOps table, installed
    // from BackendObject_DirectVulkan::Initialize() so that GetStateObjectDeathOps() is
    // non-null on every Magma role that owns a backend. See the definition for what the one
    // arm does and for the inproc gap it closes. Push-only and disaggregated-only: a pull
    // build declares no notice at all (StateObjectDeathNotice.h) and has nothing to emit.
    void InstallStateObjectDeathOps();
    // True when the notice currently installed is this backend's. Read by MG_Test so the
    // install can be asserted without a death having to happen first.
    Bool StateObjectDeathOpsInstalled();
#endif

    // Drops every cached program-resource reflection entry (CPU-side strings/vectors
    // only, no Vulkan handles). Called at EGL teardown next to the renderer reset;
    // safe because GL calls are serialized in this codebase, and any still-live
    // program rebuilds its entry from the retained generated SPIR-V on demand.
    void ClearProgramResourceCaches();

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, const GLfloat* value);
    void ClearNamedFramebufferiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, const GLint* value);
    void ClearNamedFramebufferuiv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                  GLint drawbuffer, const GLuint* value);
    void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer, GLenum buffer,
                                 GLint drawbuffer, GLfloat depth, GLint stencil);
    void Clear(GLbitfield mask);
    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex);
    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount);
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex);
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride);
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride);
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex);
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance);
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex);
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance);
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect);
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance);
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void DrawArraysIndirect(GLenum mode, const void* indirect);
    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter);
    void BlitNamedFramebuffer(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                              const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                              GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                              GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                              GLbitfield mask, GLenum filter);
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border);
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height);
    void CopyImageSubData(const CopyImageEndpoint& src,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const CopyImageEndpoint& dst,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void GenerateMipmap(GLenum target);
    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
    void DispatchComputeIndirect(GLintptr indirect);
    void MemoryBarrier(GLbitfield barriers);
    void MemoryBarrierByRegion(GLbitfield barriers);
    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format);
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data);
    void ShaderStorageBlockBinding(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding);
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void GetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture, TextureUploadTarget uploadTarget,
                         GLint level, GLenum format, GLenum type, GLsizei bufSize, GLvoid* pixels);
    // GL fence sync objects, mapped onto the renderer's frame-serial busy
    // tracking: a fence captures the frame serial current at creation and is
    // signaled once every command recorded under that serial has completed on
    // the GPU.
    BackendSyncHandle FenceSync();
    GLenum ClientWaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void WaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void DeleteSync(BackendSyncHandle sync);
    Bool GetSyncStatus(BackendSyncHandle sync);
    // GPU timer queries (GL_TIME_ELAPSED spans and GL_TIMESTAMP one-shots),
    // backed by per-frame VkQueryPool timestamp slots. All hooks degrade
    // gracefully: null handles when the renderer is absent, the device lacks
    // timestamp support, or the frame's pool is exhausted.
    // Dynamic support check (GLFunctionsTable::IsTimerQuerySupported): true
    // only while a live renderer exists whose device can actually time.
    Bool IsTimerQuerySupported();
    BackendQueryHandle BeginTimeElapsedQuery();
    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated);
    void EndXfbPrimitivesQuery(BackendQueryHandle query);
    BackendQueryHandle BeginOcclusionQuery();
    void EndOcclusionQuery(BackendQueryHandle query);
    void EndTimeElapsedQuery(BackendQueryHandle query);
    BackendQueryHandle QueryCounterTimestamp();
    Bool IsQueryResultAvailable(BackendQueryHandle query);
    // Returns true when a final value was produced (outNanoseconds set; the
    // frontend may cache it and release the handle), false when the result
    // cannot be obtained yet (e.g. a wait refused because the records' frame
    // serial is the current unsubmitted frame) - the handle then stays
    // readable later.
    Bool GetQueryResult64(BackendQueryHandle query, Bool wait, Uint64* outNanoseconds);
    void DeleteBackendQuery(BackendQueryHandle query);
    // Always 0: Vulkan cannot synchronously sample the GPU clock (timestamps
    // only exist as vkCmdWriteTimestamp results); the frontend falls back.
    Int64 GetGpuTimestampNs();
    void Present();
    // eglSwapInterval. Remembered across renderers: one created for a later window surface
    // starts from the app's last request (GetRequestedSwapInterval) instead of the default.
    void SetSwapInterval(Int interval);
    Optional<Int> GetRequestedSwapInterval();
#if MOBILEGL_BUILD_DISAGGREGATED
    // The swap interval an ended server session asked for is not the next one's, the same rule as
    // DirectGLES::ForgetRequestedSwapInterval: under a transport ~BackendObject_DirectVulkan forgets
    // it, so the next session starts at the default until it asks.
    void ForgetRequestedSwapInterval();
#endif
} // namespace MobileGL::MG_Backend::DirectVulkan
