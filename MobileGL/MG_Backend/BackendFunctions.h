// MobileGL - MobileGL/MG_Backend/BackendFunctions.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include <MG_Util/Damage/Damage.h>
#include <MG_Pipe/MGPipeHandles.h>

namespace MobileGL::MG_State::GLState {
    class FramebufferObject;
    class ITextureObject;
    class RenderbufferObject;
} // namespace MobileGL::MG_State::GLState

namespace MobileGL::MG_Backend {
    // One endpoint of a glCopyImageSubData. GL 4.6 core 18.3.2 accepts GL_RENDERBUFFER
    // alongside the ten whole-image texture targets, and a renderbuffer name lives in a
    // namespace of its own - so an endpoint is a sum type, not an ITextureObject. At most
    // one of the two pointers is set; neither is set when the name named nothing, which is
    // the INVALID_VALUE the frontend validator reports.
    struct CopyImageEndpoint {
        SharedPtr<MG_State::GLState::ITextureObject> Texture;
        SharedPtr<MG_State::GLState::RenderbufferObject> Renderbuffer;
        MG_Pipe::MGPipeHandle TextureHandle = MG_Pipe::kMGPipeNullHandle;
        MG_Pipe::MGPipeHandle RenderbufferHandle = MG_Pipe::kMGPipeNullHandle;

        Bool IsRenderbuffer() const {
            if (!MG_Pipe::MGPipeHandleIsNull(RenderbufferHandle)) return true;
            return Renderbuffer != nullptr;
        }
        Bool Exists() const {
            if (!MG_Pipe::MGPipeHandleIsNull(TextureHandle)) return true;
            if (!MG_Pipe::MGPipeHandleIsNull(RenderbufferHandle)) return true;
            return Texture != nullptr || Renderbuffer != nullptr;
        }
    };

    // Opaque backend fence-sync handle, created by GLFunctionsTable::FenceSync
    // and released by GLFunctionsTable::DeleteSync.
    using BackendSyncHandle = void*;

    // Opaque backend timer-query handle, created by
    // GLFunctionsTable::BeginTimeElapsedQuery / QueryCounterTimestamp and
    // released by GLFunctionsTable::DeleteBackendQuery.
    using BackendQueryHandle = void*;

    struct GLFunctionsTable {
        void (*DrawArrays)(GLenum mode, GLint first, GLsizei count);
        void (*DrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
        void (*DrawElementsBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);
        void (*MultiDrawArrays)(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
        void (*MultiDrawElements)(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                  GLsizei drawcount);
        void (*MultiDrawElementsBaseVertex)(GLenum mode, const GLsizei* count, GLenum type,
                                            const GLvoid* const* indices, GLsizei drawcount, const GLint* basevertex);
        void (*MultiDrawElementsIndirect)(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                          GLsizei stride);
        void (*MultiDrawArraysIndirect)(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
        void (*MultiDrawElementsIndirectCount)(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                               GLsizei maxdrawcount, GLsizei stride);
        void (*MultiDrawArraysIndirectCount)(GLenum mode, const void* indirect, GLintptr drawcount,
                                             GLsizei maxdrawcount, GLsizei stride);
        void (*DrawRangeElementsBaseVertex)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                            const void* indices, GLint basevertex);
        void (*DrawRangeElements)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                  const void* indices);
        void (*DrawElementsInstancedBaseVertexBaseInstance)(GLenum mode, GLsizei count, GLenum type,
                                                            const void* indices, GLsizei instancecount,
                                                            GLint basevertex, GLuint baseinstance);
        void (*DrawElementsInstancedBaseVertex)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                GLsizei instancecount, GLint basevertex);
        void (*DrawElementsInstancedBaseInstance)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                  GLsizei instancecount, GLuint baseinstance);
        void (*DrawElementsInstanced)(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                      GLsizei instancecount);
        void (*DrawArraysInstancedBaseInstance)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                                GLuint baseinstance);
        void (*DrawArraysInstanced)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
        void (*DrawElementsIndirect)(GLenum mode, GLenum type, const void* indirect);
        void (*DrawArraysIndirect)(GLenum mode, const void* indirect);
        void (*Clear)(GLbitfield mask);
        void (*ClearBufferfi)(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void (*ClearBufferfv)(GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void (*ClearBufferuiv)(GLenum buffer, GLint drawbuffer, const GLuint* value);
        void (*ClearBufferiv)(GLenum buffer, GLint drawbuffer, const GLint* value);
        void (*ClearNamedFramebufferfv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                        GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void (*ClearNamedFramebufferfi)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                        GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void (*ClearNamedFramebufferiv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                        GLenum buffer, GLint drawbuffer, const GLint* value);
        void (*ClearNamedFramebufferuiv)(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                         GLenum buffer, GLint drawbuffer, const GLuint* value);
        void (*BlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
        void (*BlitNamedFramebuffer)(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                                     const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                                     GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                     GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
        void (*CopyTexImage2D)(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                               GLsizei height, GLint border);
        void (*CopyTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                  GLsizei width, GLsizei height);
        // The by-name form: the destination is `texture`, whatever the units hold. `target` is
        // the one the record carries (the texture's own 2D-class target).
        void (*CopyTextureSubImage2D)(const SharedPtr<MG_State::GLState::ITextureObject>& texture, GLenum target,
                                      GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                                      GLsizei height);
        void (*CopyImageSubData)(const CopyImageEndpoint& src, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY,
                                 GLint srcZ, const CopyImageEndpoint& dst, GLenum dstTarget, GLint dstLevel, GLint dstX,
                                 GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
        void (*GenerateMipmap)(GLenum target);
        // The by-name form, also used once the bound-target form has resolved its texture.
        void (*GenerateTextureMipmap)(const SharedPtr<MG_State::GLState::ITextureObject>& texture, GLenum target);
        void (*ReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
        void (*GetTexImage)(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
        void (*GetTextureImage)(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                                TextureUploadTarget uploadTarget, GLint level, GLenum format, GLenum type,
                                GLsizei bufSize, GLvoid* pixels);
        void (*DispatchCompute)(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
        void (*DispatchComputeIndirect)(GLintptr indirect);
        void (*MemoryBarrier)(GLbitfield barriers);
        void (*MemoryBarrierByRegion)(GLbitfield barriers);
        void (*BindImageTexture)(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer,
                                 GLenum access, GLenum format);
        // The ONLY indexed query that is genuinely a backend one, and only for the pnames
        // MG_Impl/GLImpl/Getter/GL_Getter.cpp does not already own. Every indexed pname that
        // names FRONTEND state - the indexed buffer bindings, the per-unit texture/sampler
        // bindings, the image-unit bindings, the viewport rectangles, the indexed capabilities
        // - is answered in GL_Getter::GetIntegeri_v and never reaches this entry; the
        // 64-bit and float/double widths are derived there from the same answer, which is why
        // no GetInteger64i_v/GetFloati_v/GetDoublei_v table entry exists. In practice this
        // leaves GL_MAX_COMPUTE_WORK_GROUP_COUNT / _SIZE (also asked directly by
        // MG_Util/ShaderTranspiler/CompileEnv.cpp) plus whatever pname the frontend has no
        // case for at all.
        void (*GetIntegeri_v)(GLenum target, GLuint index, GLint* data);
        // There is deliberately NO GetProgramiv entry: glGetProgramiv describes the program
        // the APPLICATION wrote - link status, the transform-feedback mode, the compute local
        // size - all of which are frontend link artifacts on ProgramObject, and
        // MG_Impl/GLImpl/Program/GL_Program.cpp answers every one of them from there. Asking a
        // backend would mean asking about a DIFFERENT program (a SPIRV-Cross-generated ESSL
        // one, or a SPIR-V module), in a namespace the application never sees.
        // The GL program interface (glGetProgramInterfaceiv / glGetProgramResource*) is NOT
        // a backend query: it describes the program the application wrote, in the
        // application's namespace, which neither backend program is in. It is answered
        // entirely by MG_Impl/GLImpl/Program/ProgramInterface from the frontend reflection.
        // Takes the block's GL NAME, not glShaderStorageBlockBinding's index. The index
        // the application passes is the frontend interface-query enumeration's, and no
        // backend shares that index space: DirectVulkan enumerates SPIR-V descriptor
        // bindings and DirectGLES asks a real driver about SPIRV-Cross-generated ESSL.
        // The name is the one coordinate all three agree on, so the frontend resolves the
        // index against its own enumeration and each backend maps the name to its own.
        void (*ShaderStorageBlockBinding)(GLuint program, const GLchar* storageBlockName, GLuint storageBlockBinding);
        // GL fence sync objects. All entries are optional (may be null); the
        // frontend then falls back to always-signaled sync semantics.
        // FenceSync may itself return null when the backend cannot create a
        // fence right now (e.g. the calling thread does not own the backend
        // context); the frontend treats such a sync as always signaled.
        BackendSyncHandle (*FenceSync)();
        GLenum (*ClientWaitSync)(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
        void (*WaitSync)(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
        void (*DeleteSync)(BackendSyncHandle sync);
        Bool (*GetSyncStatus)(BackendSyncHandle sync); // true = signaled
        // GL timer-query objects (GL_ARB_timer_query). All entries are
        // optional (may be null); the frontend then falls back to zero
        // results and reports GL_QUERY_COUNTER_BITS == 0.
        // BeginTimeElapsedQuery / QueryCounterTimestamp may themselves
        // return null when the backend cannot create a query right now;
        // the frontend treats such a query as immediately available with
        // a zero result.
        // Dynamic support check: true only when the live backend can
        // actually time at the moment of the call (extension / entry
        // points / timestamp valid bits are known then, not at table
        // init). Gates the advertised GL_QUERY_COUNTER_BITS.
        Bool (*IsTimerQuerySupported)();
        BackendQueryHandle (*BeginTimeElapsedQuery)();            // starts a TIME_ELAPSED span
        void (*EndTimeElapsedQuery)(BackendQueryHandle query);    // ends the span
        BackendQueryHandle (*QueryCounterTimestamp)();            // glQueryCounter(GL_TIMESTAMP) one-shot
        Bool (*IsQueryResultAvailable)(BackendQueryHandle query); // non-blocking
        // Returns true when a final value was produced (*outNanoseconds
        // written; the frontend may cache it and release the handle).
        // Returns false when the result could not be obtained YET - e.g.
        // a Vulkan wait that refuses to block on a not-yet-submitted
        // frame serial - in which case the frontend must keep the handle
        // and leave the query readable later.
        Bool (*GetQueryResult64)(BackendQueryHandle query, Bool wait, Uint64* outNanoseconds);
        void (*DeleteBackendQuery)(BackendQueryHandle query);
        // GL_SAMPLES_PASSED occlusion queries (optional; null = unsupported,
        // the frontend then rejects the target). Results/deletion flow through
        // GetQueryResult64 / DeleteBackendQuery like timer queries.
        BackendQueryHandle (*BeginOcclusionQuery)();
        void (*EndOcclusionQuery)(BackendQueryHandle query);
        // Transform feedback primitive queries backed by real GPU query pools
        // (optional; null = frontend falls back to CPU accounting).
        BackendQueryHandle (*BeginXfbPrimitivesQuery)(Bool generated);
        void (*EndXfbPrimitivesQuery)(BackendQueryHandle query);
        // Whether GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN should be answered from the
        // frontend's own accounting wherever that accounting is exact - a capture with no
        // geometry stage - instead of from the query above. Set by DirectGLES, whose result
        // is whatever the ES driver's PRIMITIVES_WRITTEN counter says: Adreno reports twice
        // the written count for a vertex-only capture that follows a large render pass,
        // where the desktop-exact answer is the one the frontend already computed. Defaults
        // to false, so a backend that never sets it keeps using its GPU result.
        Bool PrefersCpuXfbPrimitiveAccounting = false;
        // Transform feedback capture spans, for backends whose own GL/ES driver
        // performs the capture (DirectGLES). Both optional; null means the backend
        // drives capture from its draw recording instead (DirectVulkan). End is
        // called while the frontend capture state is still active, so the backend
        // can still see the capture program and buffer bindings.
        // GL_PATCH_VERTICES; ES 3.2 spells it the same way.
        void (*PatchParameteri)(GLenum pname, GLint value);
        void (*BeginTransformFeedback)(GLenum primitiveMode);
        void (*EndTransformFeedback)();
        // ARB_transform_feedback2. A backend that leaves these null keeps the single
        // implicit capture span the frontend has always modeled; the frontend state
        // (paused flag, per-object bindings) is tracked either way.
        void (*PauseTransformFeedback)();
        void (*ResumeTransformFeedback)();
        void (*BindTransformFeedback)(GLuint name);
        void (*DeleteTransformFeedback)(GLuint name);
        Int64 (*GetGpuTimestampNs)(); // glGetInteger64v(GL_TIMESTAMP); 0 if unsupported
    };
    struct GlobalBackendFunctionsTable {
        GLFunctionsTable GL;
        void (*Present)();
        // Optional: applies the app-requested eglSwapInterval to the native
        // presentation path (null = backend keeps its own pacing policy).
        void (*SetSwapInterval)(Int interval);
    };

    // THE DAMAGE OF THE PRESENT BEING APPLIED on this thread: the server's apply thread sets it
    // around the backend's Present (from the present record), and a backend that can hand damage
    // to its native present reads it there. Full whenever nothing set it.
    const MG_Util::Damage::Region& CurrentPresentDamage();
    void SetCurrentPresentDamage(const MG_Util::Damage::Region& damage);

    extern GlobalBackendFunctionsTable gBackendFunctionsTable;
} // namespace MobileGL::MG_Backend
