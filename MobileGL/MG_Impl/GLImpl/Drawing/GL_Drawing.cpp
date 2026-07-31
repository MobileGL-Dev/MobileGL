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

    // Primitives a draw of `count` vertices in `mode` assembles (0 for
    // incomplete primitives). Used for the CPU-side transform feedback
    // primitive accounting.
    static Uint64 CountPrimitivesForDraw(GLenum mode, GLsizei count) {
        if (count <= 0) return 0;
        switch (mode) {
        case GL_POINTS: return static_cast<Uint64>(count);
        case GL_LINES: return static_cast<Uint64>(count / 2);
        case GL_LINE_STRIP: return count >= 2 ? static_cast<Uint64>(count - 1) : 0;
        case GL_LINE_LOOP: return count >= 2 ? static_cast<Uint64>(count) : 0;
        case GL_TRIANGLES: return static_cast<Uint64>(count / 3);
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN: return count >= 3 ? static_cast<Uint64>(count - 2) : 0;
        default: return 0;
        }
    }

    // Accumulate the transform feedback primitive counter for a captured draw.
    // Draws without a geometry stage write exactly the primitives they assemble,
    // clamped by the capture buffers' remaining capacity (a full buffer stops
    // recording whole primitives, which is what PRIMITIVES_WRITTEN reports).
    // Geometry amplification is not modelled here.
    static void AccountTransformFeedbackPrimitives(GLenum mode, GLsizei count) {
        if (!MG_State::pGLContext->IsTransformFeedbackActive()) return;
        Uint64 primitives = CountPrimitivesForDraw(mode, count);
        if (primitives == 0) return;

        Uint64 verticesPerPrimitive = 1;
        switch (mode) {
        case GL_LINES:
        case GL_LINE_STRIP:
        case GL_LINE_LOOP:
            verticesPerPrimitive = 2;
            break;
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
            verticesPerPrimitive = 3;
            break;
        default:
            break;
        }

        const auto& program = MG_State::pGLContext->GetTransformFeedbackProgram();
        if (program != nullptr) {
            // Capacity in captured vertices = the tightest bound buffer.
            Uint64 capacityVertices = ~0ull;
            for (SizeT i = 0; i < program->GetTransformFeedbackBufferCount(); ++i) {
                const Uint32 stride = program->GetTransformFeedbackStride(static_cast<Uint32>(i));
                if (stride == 0) continue;
                const auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback,
                                                                                static_cast<Uint>(i));
                const Range1D range = point.GetRange();
                const Uint64 bytes = range.end > range.start ? static_cast<Uint64>(range.end - range.start) : 0;
                capacityVertices = std::min<Uint64>(capacityVertices, bytes / stride);
            }
            if (capacityVertices != ~0ull) {
                const Uint64 usedVertices = MG_State::pGLContext->GetTransformFeedbackCapturedVertices();
                const Uint64 remainingVertices = capacityVertices > usedVertices ? capacityVertices - usedVertices : 0;
                primitives = std::min<Uint64>(primitives, remainingVertices / verticesPerPrimitive);
            }
        }
        MG_State::pGLContext->AddTransformFeedbackPrimitives(primitives);
        MG_State::pGLContext->AddTransformFeedbackCapturedVertices(primitives * verticesPerPrimitive);
    }

    static Bool ValidatePrimitiveModeForBackend(const char* functionName, GLenum mode) {
        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "No active backend object."));
            return false;
        }

        const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (vao && vao->GetExternalIndex() == 0 && !MG_State::IsRelaxedSemanticsActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Default vertex array object cannot be used for drawing in core profile."));
            return false;
        }

        // While transform feedback is active the draw's primitive type must match
        // the feedback primitive mode (GL 3.3 core 13.2.2). With a geometry shader
        // the constraint moves to the shader's output primitive type instead, so
        // the draw mode itself is unconstrained here.
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            !(MG_State::pGLContext->GetTransformFeedbackProgram() &&
              MG_State::pGLContext->GetTransformFeedbackProgram()->GetShaderIndexByStage(ShaderStage::Geometry) >= 0)) {
            const GLenum feedbackMode = MG_State::pGLContext->GetTransformFeedbackPrimitiveMode();
            Bool compatible = false;
            switch (feedbackMode) {
            case GL_POINTS:
                compatible = mode == GL_POINTS;
                break;
            case GL_LINES:
                compatible = mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP;
                break;
            case GL_TRIANGLES:
                compatible = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
                break;
            default:
                break;
            }
            if (!compatible) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", functionName,
                        "Primitive mode is incompatible with the active transform feedback primitive mode."));
                return false;
            }
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

    void MultiDrawArrays_Backend(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        MG_Backend::gBackendFunctionsTable.GL.MultiDrawArrays(mode, first, count, drawcount);
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
        AccountTransformFeedbackPrimitives(mode, count);
        DrawElementsBaseVertex_Backend(mode, count, type, indices, basevertex);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        AccountTransformFeedbackPrimitives(mode, count);
        DrawArrays_Backend(mode, first, count);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        if (drawcount < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "drawcount must be non-negative."));
            return;
        }
        MultiDrawArrays_Backend(mode, first, count, drawcount);
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
        AccountTransformFeedbackPrimitives(mode, count);
        DrawElements_Backend(mode, count, type, indices);
    }

    void BeginTransformFeedback(GLenum primitiveMode) {
        if (primitiveMode != GL_POINTS && primitiveMode != GL_LINES && primitiveMode != GL_TRIANGLES) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "primitiveMode must be GL_POINTS, GL_LINES or GL_TRIANGLES."));
            return;
        }
        if (MG_State::pGLContext->IsTransformFeedbackActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Transform feedback is already active."));
            return;
        }
        const auto& program = MG_State::pGLContext->GetCurrentProgram();
        if (!program || !program->GetLinkStatus() || program->GetTransformFeedbackVaryingCount() == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "No program with transform feedback varyings is active."));
            return;
        }
        // Every capture buffer slot the program's mode uses must have a buffer bound.
        const SizeT usedBufferCount = program->GetTransformFeedbackBufferCount();
        for (SizeT i = 0; i < usedBufferCount; ++i) {
            const auto& point = MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback,
                                                                            static_cast<Uint>(i));
            if (point.GetBoundObject() == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        "Transform feedback buffer binding point " + std::to_string(i) + " has no buffer bound."));
                return;
            }
        }
        MG_State::pGLContext->BeginTransformFeedback(primitiveMode, program);
    }

    void EndTransformFeedback(void) {
        if (!MG_State::pGLContext->IsTransformFeedbackActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Transform feedback is not active."));
            return;
        }
        MG_State::pGLContext->EndTransformFeedback();
        // Captured results must be visible to MapBuffer/GetBufferSubData after
        // End; the capture targets are host-coherent GPU memory, so completing
        // the GPU work is all that is required.
        auto& backendGL = MG_Backend::gBackendFunctionsTable.GL;
        if (backendGL.FenceSync && backendGL.ClientWaitSync) {
            if (auto sync = backendGL.FenceSync()) {
                backendGL.ClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, ~0ull);
                if (backendGL.DeleteSync) {
                    backendGL.DeleteSync(sync);
                }
            }
        }
    }

} // namespace MobileGL::MG_Impl::GLImpl
