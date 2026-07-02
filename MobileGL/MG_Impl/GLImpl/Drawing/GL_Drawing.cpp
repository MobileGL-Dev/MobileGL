// MobileGL - MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Drawing.h"
#include <Config.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/EGLState/Core.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
    static Bool ValidateCurrentProgramForExecution(const char* functionName) {
        const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
        if (!currentProgram) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "There is no current program object."));
            return false;
        }

        if (!currentProgram->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "The current program object is not linked."));
            return false;
        }

        return true;
    }

    static Bool ValidateCurrentProgramForCompute(const char* functionName) {
        if (!ValidateCurrentProgramForExecution(functionName)) return false;

        const auto& currentProgram = MG_State::pGLContext->GetCurrentProgram();
        if (currentProgram->GetShaderIndexByStage(ShaderStage::Compute) < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "The current program object has no compute shader stage."));
            return false;
        }

        return true;
    }

    static Bool ValidatePrimitiveModeForBackend(const char* functionName, GLenum mode) {
        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "No active backend object."));
            return false;
        }

        if (activeBackendObject->GetBackendType() == BackendType::DirectVulkan && mode == GL_LINE_LOOP) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    "Primitive mode GL_LINE_LOOP is not supported by the DirectVulkan backend."));
            return false;
        }

        const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (vao && vao->GetExternalIndex() == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Default vertex array object cannot be used for drawing."));
            return false;
        }

        return true;
    }

    void Clear_Backend(GLbitfield mask) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.Clear(mask);
    }

    void DrawElements_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElements(mode, count, type, indices);
    }

    void MultiDrawElements_Backend(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                   GLsizei drawcount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElements(mode, count, type, indices, drawcount);
    }

    void MultiDrawElementsBaseVertex_Backend(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                             GLsizei drawcount, const GLint* basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsBaseVertex(mode, count, type, indices, drawcount,
                                                                          basevertex);
    }

    void DrawArrays_Backend(GLenum mode, GLint first, GLsizei count) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArrays(mode, first, count);
    }

    void DrawElementsBaseVertex_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                        GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }

    void MultiDrawElementsIndirect_Backend(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount,
                                           GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsIndirect(mode, type, indirect, drawcount, stride);
    }

    void MultiDrawArraysIndirect_Backend(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawArraysIndirect(mode, indirect, drawcount, stride);
    }

    void MultiDrawElementsIndirectCount_Backend(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                                GLsizei maxdrawcount, GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsIndirectCount(mode, type, indirect, drawcount,
                                                                             maxdrawcount, stride);
    }

    void MultiDrawArraysIndirectCount_Backend(GLenum mode, const void* indirect, GLintptr drawcount,
                                              GLsizei maxdrawcount, GLsizei stride) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawArraysIndirectCount(mode, indirect, drawcount, maxdrawcount,
                                                                           stride);
    }

    void DrawRangeElementsBaseVertex_Backend(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                             const void* indices, GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawRangeElementsBaseVertex(mode, start, end, count, type, indices,
                                                                          basevertex);
    }

    void DrawRangeElements_Backend(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                   const void* indices) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawRangeElements(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance_Backend(GLenum mode, GLsizei count, GLenum type,
                                                             const void* indices, GLsizei instancecount,
                                                             GLint basevertex, GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseVertexBaseInstance(
            mode, count, type, indices, instancecount, basevertex, baseinstance);
    }

    void DrawElementsInstancedBaseVertex_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                 GLsizei instancecount, GLint basevertex) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount,
                                                                              basevertex);
    }

    void DrawElementsInstancedBaseInstance_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                   GLsizei instancecount, GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstancedBaseInstance(mode, count, type, indices,
                                                                                instancecount, baseinstance);
    }

    void DrawElementsInstanced_Backend(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                       GLsizei instancecount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsInstanced(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect_Backend(GLenum mode, GLenum type, const void* indirect) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawElementsIndirect(mode, type, indirect);
    }
    void DrawArraysInstancedBaseInstance_Backend(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                                 GLuint baseinstance) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysInstancedBaseInstance(mode, first, count, instancecount,
                                                                              baseinstance);
    }

    void DrawArraysInstanced_Backend(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysInstanced(mode, first, count, instancecount);
    }

    void DrawArraysIndirect_Backend(GLenum mode, const void* indirect) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.DrawArraysIndirect(mode, indirect);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ) {
        auto dispatchCompute = MG_Backend::gBackendFunctionsTable.GL.DispatchCompute;
        if (!dispatchCompute) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Backend does not support compute dispatch."));
            return;
        }
        if (!ValidateCurrentProgramForCompute(__func__)) return;
        dispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void DispatchComputeIndirect(GLintptr indirect) {
        auto dispatchComputeIndirect = MG_Backend::gBackendFunctionsTable.GL.DispatchComputeIndirect;
        if (!dispatchComputeIndirect) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support indirect compute dispatch."));
            return;
        }
        if (!ValidateCurrentProgramForCompute(__func__)) return;
        dispatchComputeIndirect(indirect);
    }

    void MemoryBarrier(GLbitfield barriers) {
        auto memoryBarrier = MG_Backend::gBackendFunctionsTable.GL.MemoryBarrier;
        if (!memoryBarrier) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Backend does not support memory barriers."));
            return;
        }
        memoryBarrier(barriers);
    }

    void MemoryBarrierByRegion(GLbitfield barriers) {
        auto memoryBarrierByRegion = MG_Backend::gBackendFunctionsTable.GL.MemoryBarrierByRegion;
        if (!memoryBarrierByRegion) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support regional memory barriers."));
            return;
        }
        memoryBarrierByRegion(barriers);
    }

    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElementsIndirect_Backend(mode, type, indirect, drawcount, stride);
    }

    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawArraysIndirect_Backend(mode, indirect, drawcount, stride);
    }

    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
        auto multiDrawElementsIndirectCount = MG_Backend::gBackendFunctionsTable.GL.MultiDrawElementsIndirectCount;
        if (!multiDrawElementsIndirectCount) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support indirect-parameter indexed draws."));
            return;
        }
        MultiDrawElementsIndirectCount_Backend(mode, type, indirect, drawcount, maxdrawcount, stride);
    }

    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride) {
        auto multiDrawArraysIndirectCount = MG_Backend::gBackendFunctionsTable.GL.MultiDrawArraysIndirectCount;
        if (!multiDrawArraysIndirectCount) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support indirect-parameter array draws."));
            return;
        }
        MultiDrawArraysIndirectCount_Backend(mode, indirect, drawcount, maxdrawcount, stride);
    }

    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawRangeElementsBaseVertex_Backend(mode, start, end, count, type, indices, basevertex);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawRangeElements_Backend(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseVertexBaseInstance_Backend(mode, count, type, indices, instancecount, basevertex,
                                                            baseinstance);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseVertex_Backend(mode, count, type, indices, instancecount, basevertex);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseInstance_Backend(mode, count, type, indices, instancecount, baseinstance);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstanced_Backend(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsIndirect_Backend(mode, type, indirect);
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArraysInstancedBaseInstance_Backend(mode, first, count, instancecount, baseinstance);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArraysInstanced_Backend(mode, first, count, instancecount);
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArraysIndirect_Backend(mode, indirect);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsBaseVertex_Backend(mode, count, type, indices, basevertex);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArrays_Backend(mode, first, count);
    }

    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                           GLsizei drawcount) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElements_Backend(mode, count, type, indices, drawcount);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElementsBaseVertex_Backend(mode, count, type, indices, drawcount, basevertex);
    }

    void Clear(GLbitfield mask) {
        Clear_Backend(mask);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElements_Backend(mode, count, type, indices);
    }

} // namespace MobileGL::MG_Impl::GLImpl
