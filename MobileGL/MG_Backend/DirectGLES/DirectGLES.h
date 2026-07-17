// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLES.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Backend/BackendObject.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>
#include <MG_State/GLState/TextureState/TextureState.h>
#include <MG_State/GLState/SamplerState/SamplerObject.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

#define CallAndCheck(operation)                                                                                        \
    MGLOG_D("Call GLES func: %s", #operation);                                                                         \
    operation Utils::CheckGLESError();

namespace MobileGL::MG_Backend::DirectGLES {
    // Re-establishes the frontend texture-unit bindings on the native ES context.
    // Content uploads use scratch bindings, so draws and dispatches call this after
    // texture synchronization.
    void BindCurrentTextures();
    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
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
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride);
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
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
    void ClearNamedFramebufferfv(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, const GLfloat* value);
    void ClearNamedFramebufferfi(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                 GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
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
    void CopyImageSubData(const SharedPtr<MG_State::GLState::ITextureObject>& srcTexture,
                          GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                          const SharedPtr<MG_State::GLState::ITextureObject>& dstTexture,
                          GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                          GLsizei srcWidth, GLsizei srcHeight, GLsizei srcDepth);
    void GenerateMipmap(GLenum target);
    const GLubyte* GetString(GLenum name);
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels);
    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
    void DispatchComputeIndirect(GLintptr indirect);
    void MemoryBarrier(GLbitfield barriers);
    void MemoryBarrierByRegion(GLbitfield barriers);
    void BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access,
                          GLenum format);
    void GetIntegeri_v(GLenum target, GLuint index, GLint* data);
    void GetInteger64i_v(GLenum target, GLuint index, GLint64* data);
    void GetProgramiv(GLuint program, GLenum pname, GLint* params);
    void GetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params);
    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name);
    void GetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length,
                                GLchar* name);
    void GetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                              const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params);
    GLint GetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name);
    GLint GetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name);
    void ShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding);
    Bool InitWindowSurface(NativeWindowType window);
    Bool InitPbufferSurface(EGLint width, EGLint height);
    Bool MakeCurrent();
    Bool ReleaseCurrent();
    // True when the backend ES context is current on the calling thread, i.e.
    // immediate buffer ops may issue GL calls right now.
    Bool IsBackendContextCurrentOnThisThread();
    // GL fence sync objects, backed by native ES fences. FenceSync returns null
    // (the frontend then falls back to an always-signaled sync) when the calling
    // thread does not own the ES context. Waits/queries degrade to "signaled" in
    // the same situation, and handles created under a since-destroyed ES context
    // are always treated as signaled.
    BackendSyncHandle FenceSync();
    GLenum ClientWaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void WaitSync(BackendSyncHandle sync, GLbitfield flags, GLuint64 timeout);
    void DeleteSync(BackendSyncHandle sync);
    Bool GetSyncStatus(BackendSyncHandle sync);
    // True when GL_EXT_disjoint_timer_query and every entry point the timer
    // hooks below need are present. Also gates the E_GL_ARB_timer_query
    // advertisement in BackendObject_DirectGLES::InitCapabilities, and is
    // registered as the GLFunctionsTable::IsTimerQuerySupported hook: a pure
    // capability read needs no current ES context, and it stays false until
    // the ES capabilities have been filled in.
    Bool AreTimerQueriesSupported();
    // GL timer-query objects, backed by GL_EXT_disjoint_timer_query. The
    // creators return null (the frontend then falls back to an immediately
    // available zero result) when the calling thread does not own the ES
    // context or the extension/entry points are missing, and handles created
    // under a since-destroyed ES context are always treated as complete with
    // a zero result (mirrors the fence-sync handles above).
    BackendQueryHandle BeginTimeElapsedQuery();
    void EndTimeElapsedQuery(BackendQueryHandle query);
    BackendQueryHandle QueryCounterTimestamp();
    Bool IsQueryResultAvailable(BackendQueryHandle query);
    // Returns true when a final value landed in *outNanoseconds (a zero for
    // null or stale-generation handles IS final: the frontend may cache it
    // and release the handle). Returns false only when the calling thread
    // does not own the ES context, so the value is genuinely unobtainable
    // right now; the handle stays alive and readable later.
    Bool GetQueryResult64(BackendQueryHandle query, Bool wait, Uint64* outNanoseconds);
    void DeleteBackendQuery(BackendQueryHandle query);
    Int64 GetGpuTimestampNs();
    void Present();
    // Frame-completion watermarks for the buffer-storage pool: CurrentFrameSerial()
    // is bumped once per Present(); CompletedFrameSerial() is the newest frame whose
    // GPU work has provably finished (advanced by polling a one-fence-per-frame ring).
    // A buffer retired during frame N is safe to recycle once CompletedFrameSerial() >= N.
    Uint64 CurrentFrameSerial();
    Uint64 CompletedFrameSerial();
    // Applies (or defers until the window surface exists) the app-requested
    // eglSwapInterval on the native EGL surface.
    void SetSwapInterval(Int interval);
    void SetEGLFuncsTable(const MG_External::EGLFunctionsTable& eglFuncs);
    void SetGLESFuncsTable(const MG_External::GLESFunctionsTable& glesFuncs);
    void SetGLESCapabilities(const MG_External::GLESCapabilities& capabilities);
    void DestroyEGLContext();

    extern MG_External::EGLFunctionsTable g_EGLFuncs;
    extern MG_External::GLESFunctionsTable g_GLESFuncs;
    extern MG_External::GLESCapabilities g_GLESCapabilities;
} // namespace MobileGL::MG_Backend::DirectGLES
