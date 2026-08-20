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
#include "../Getter/GL_Getter.h"

namespace MobileGL::MG_Impl::GLImpl {
    static Bool ValidateProgramForExecution(const SharedPtr<MG_State::GLState::ProgramObject>& currentProgram,
                                            const char* functionName) {
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

    static Bool ValidateCurrentProgramForExecution(const char* functionName) {
        return ValidateProgramForExecution(MG_State::pGLContext->GetProgramForDraw(), functionName);
    }

    // A dispatch resolves its program through the DISPATCH accessor: with a pipeline bound
    // that is the pipeline's compute stage program, not the graphics composite a draw would
    // build - which no longer contains a compute stage to find at all.
    static Bool ValidateCurrentProgramForCompute(const char* functionName) {
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDispatch();
        if (!ValidateProgramForExecution(currentProgram, functionName)) return false;

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
        // A paused span captures nothing, so a draw made while paused contributes to
        // PRIMITIVES_GENERATED but not to TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN.
        if (MG_State::pGLContext->IsTransformFeedbackPaused()) {
            MG_State::pGLContext->AddTransformFeedbackPausedPrimitives(CountPrimitivesForDraw(mode, count));
            return;
        }
        Uint64 primitives = CountPrimitivesForDraw(mode, count);
        if (primitives == 0) return;
        MG_State::pGLContext->AddTransformFeedbackInputPrimitives(primitives);

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
            // A geometry stage writes what it emits, not what the draw assembled, and the
            // amplification factor lives in the shader. Record that this span contained such
            // a draw so the transform feedback queries keep their backend result for it.
            if (program->GetShaderIndexByStage(ShaderStage::Geometry) >= 0) {
                MG_State::pGLContext->AddTransformFeedbackGeometryCaptureDraw();
            }
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
        // Only draws that get this far are in the written counter at all. The instanced and
        // indirect entry points never call this function, so a span that contains one is NOT
        // fully accounted, and the queries must be able to tell: they compare this counter's
        // delta against zero before standing in for the backend's own result.
        MG_State::pGLContext->AddTransformFeedbackAccountedCaptureDraw();
    }

    // Every primitive mode a draw command accepts (GL 4.6 core table 10.1, plus
    // GL_PATCHES for the tessellation pipeline). Anything else is GL_INVALID_ENUM.
    static Bool IsAcceptedPrimitiveMode(GLenum mode) {
        switch (mode) {
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_LOOP:
        case GL_LINE_STRIP:
        case GL_LINES_ADJACENCY:
        case GL_LINE_STRIP_ADJACENCY:
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
        case GL_TRIANGLES_ADJACENCY:
        case GL_TRIANGLE_STRIP_ADJACENCY:
        case GL_PATCHES:
            return true;
        default:
            return false;
        }
    }

    // The `mode` INVALID_ENUM in isolation, so a draw entry point can raise it BEFORE any of the
    // state-dependent INVALID_OPERATIONs below. GL 4.6 core 10.4 makes a bad mode INVALID_ENUM
    // unconditionally, while "no current program" is not even a spec-listed draw error - it is
    // MobileGL's own null-dereference guard - so it must never shadow the enum check
    // (KHR-GL31.api.coverage calls glDrawArraysInstanced/glDrawElementsInstanced with mode
    // GL_POINTS-1 against a bare context and pins GL_INVALID_ENUM).
    static Bool ValidatePrimitiveModeEnum(const char* functionName, GLenum mode) {
        if (IsAcceptedPrimitiveMode(mode)) return true;

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "mode is not an accepted primitive type."));
        return false;
    }

    static Bool ValidatePrimitiveModeForBackend(const char* functionName, GLenum mode) {
        if (!ValidatePrimitiveModeEnum(functionName, mode)) {
            return false;
        }

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

        // A geometry stage only accepts the primitive types that decompose into its declared
        // input primitive (GL 4.6 core 11.3.1); anything else is INVALID_OPERATION. GL_PATCHES
        // is the tessellation pipeline's input and reaches the geometry stage already
        // converted, so it is not constrained here.
        const auto& currentProgram = MG_State::pGLContext->GetProgramForDraw();
        const GLenum gsInput = currentProgram ? currentProgram->GetGeometryInputType() : GL_NONE;
        if (gsInput != GL_NONE && mode != GL_PATCHES) {
            Bool compatible = false;
            switch (gsInput) {
            case GL_POINTS:
                compatible = mode == GL_POINTS;
                break;
            case GL_LINES:
                compatible = mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP;
                break;
            case GL_LINES_ADJACENCY:
                compatible = mode == GL_LINES_ADJACENCY || mode == GL_LINE_STRIP_ADJACENCY;
                break;
            case GL_TRIANGLES:
                compatible = mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
                break;
            case GL_TRIANGLES_ADJACENCY:
                compatible = mode == GL_TRIANGLES_ADJACENCY || mode == GL_TRIANGLE_STRIP_ADJACENCY;
                break;
            default:
                break;
            }
            if (!compatible) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", functionName,
                        "Primitive mode is incompatible with the geometry shader's input primitive type."));
                return false;
            }
        }

        // While transform feedback is active the draw's primitive type must match
        // the feedback primitive mode (GL 3.3 core 13.2.2). With a geometry shader
        // the constraint moves to the shader's output primitive type instead, so
        // the draw mode itself is unconstrained here. A paused span is exempt: it
        // captures nothing, so there is nothing for the mode to be incompatible with
        // (GL 4.6 core 13.2.3).
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            !MG_State::pGLContext->IsTransformFeedbackPaused() &&
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

    // Byte size of the command structures the indirect draws read (GL 4.6 core 10.3.10).
    constexpr SizeT kDrawArraysIndirectCommandBytes = 4 * sizeof(Uint32);
    constexpr SizeT kDrawElementsIndirectCommandBytes = 5 * sizeof(Uint32);

    // Shared preconditions of every *Indirect draw: `indirect` is a byte offset into the
    // buffer bound to GL_DRAW_INDIRECT_BUFFER, must be 4-byte aligned, and the whole
    // command has to lie inside that buffer.
    static Bool ValidateIndirectDrawSource(const char* functionName, const void* indirect, SizeT commandBytes) {
        const auto offset = reinterpret_cast<uintptr_t>(indirect);
        if (offset % 4 != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "indirect offset must be a multiple of 4."));
            return false;
        }

        const auto& buffer = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        if (!buffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "No buffer is bound to GL_DRAW_INDIRECT_BUFFER."));
            return false;
        }

        if (offset + commandBytes > buffer->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "The indirect command extends past the end of the bound "
                                             "GL_DRAW_INDIRECT_BUFFER."));
            return false;
        }
        return true;
    }

    // Index type accepted by the DrawElements family (GL 4.6 core 10.3.9).
    static Bool ValidateDrawElementsIndexType(const char* functionName, GLenum type) {
        switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_UNSIGNED_SHORT:
        case GL_UNSIGNED_INT:
            return true;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "type is not an accepted index type."));
            return false;
        }
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
        // GL 4.6 core 19: each num_groups_* must be within GL_MAX_COMPUTE_WORK_GROUP_COUNT
        // for its dimension. GetIntegeri_v already floors that at the spec minimum.
        const GLuint numGroups[3] = {numGroupsX, numGroupsY, numGroupsZ};
        for (GLuint dimension = 0; dimension < 3; ++dimension) {
            GLint maxGroups = 0;
            GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, dimension, &maxGroups);
            if (numGroups[dimension] > static_cast<GLuint>(std::max(maxGroups, 0))) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "num_groups exceeds GL_MAX_COMPUTE_WORK_GROUP_COUNT for dimension " +
                                                     std::to_string(dimension) + "."));
                return;
            }
        }
        dispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void DispatchComputeIndirect(GLintptr indirect) {
        // Argument and binding validation runs FIRST. Both are properties of the call and of GL
        // state, so a context whose backend cannot dispatch at all must still report the
        // argument error the spec names rather than masking every one of them with
        // "unsupported" - which is what put GL_INVALID_OPERATION where
        // KHR-GL43.compute_shader.api-indirect expects GL_INVALID_VALUE.
        //
        // GL 4.6 core 19: `indirect` is a byte offset into GL_DISPATCH_INDIRECT_BUFFER -
        // negative or misaligned is INVALID_VALUE, nothing bound is INVALID_OPERATION.
        if (indirect < 0 || (indirect % 4) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "indirect must be non-negative and a multiple of 4."));
            return;
        }
        const auto& indirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DispatchIndirect).GetBoundObject();
        if (!indirectBuffer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "No buffer is bound to GL_DISPATCH_INDIRECT_BUFFER."));
            return;
        }
        // ...and the same INVALID_OPERATION covers "the command would source data beyond the end
        // of the bound buffer object" (GL 4.6 core 19): the dispatch reads three uints starting
        // at `indirect`.
        constexpr SizeT kDispatchIndirectCommandSize = 3 * sizeof(Uint32);
        if (static_cast<SizeT>(indirect) + kDispatchIndirectCommandSize > indirectBuffer->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("indirect ({}) + 12 bytes runs past the end of the {}-byte buffer bound to "
                                "GL_DISPATCH_INDIRECT_BUFFER.",
                                indirect, indirectBuffer->GetSize())));
            return;
        }
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

    void PatchParameteri(GLenum pname, GLint value) {
        if (pname != GL_PATCH_VERTICES) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "pname must be GL_PATCH_VERTICES."));
            return;
        }
        GLint maxPatchVertices = 32;
        GetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVertices);
        if (value <= 0 || value > maxPatchVertices) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "value must be in [1, GL_MAX_PATCH_VERTICES]."));
            return;
        }
        MG_State::pGLContext->SetPatchVertices(static_cast<Uint>(value));
        if (const auto patchParameteri = MG_Backend::gBackendFunctionsTable.GL.PatchParameteri) {
            patchParameteri(pname, value);
        }
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
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElementsIndirect_Backend(mode, type, indirect, drawcount, stride);
    }

    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawArraysIndirect_Backend(mode, indirect, drawcount, stride);
    }

    // ARB_indirect_parameters / GL 4.6 core 10.4: `drawcount` is a byte offset into the buffer
    // bound to PARAMETER_BUFFER and holds one uint draw count. Three errors have to be raised
    // before the call reaches a backend, and none of them was
    // (KHR-GL46.indirect_parameters_tests.MultiDraw{Arrays,Elements}IndirectCount):
    //   * drawcount not a multiple of four                                  INVALID_VALUE
    //   * nothing bound to PARAMETER_BUFFER, or the uint at `drawcount`
    //     lies past its end                                                 INVALID_OPERATION
    //   * maxdrawcount commands from `indirect` run past the end of the
    //     buffer bound to DRAW_INDIRECT_BUFFER                              INVALID_OPERATION
    static Bool ValidateIndirectCountDraw(GLintptr indirect, GLintptr drawcount, GLsizei maxdrawcount,
                                          GLsizei stride, SizeT commandSize, const char* funcName) {
        if (drawcount < 0 || (drawcount % 4) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             "drawcount must be non-negative and a multiple of four."));
            return false;
        }
        const auto& parameterBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Parameter).GetBoundObject();
        if (!parameterBuffer ||
            static_cast<SizeT>(drawcount) + sizeof(Uint32) > parameterBuffer->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             "No buffer is bound to GL_PARAMETER_BUFFER, or drawcount runs past "
                                             "the end of the one that is."));
            return false;
        }
        if (maxdrawcount < 0 || stride < 0 || indirect < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             "indirect, maxdrawcount and stride must all be non-negative."));
            return false;
        }
        const SizeT effectiveStride = stride != 0 ? static_cast<SizeT>(stride) : commandSize;
        const auto& indirectBuffer =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::DrawIndirect).GetBoundObject();
        // A zero maxdrawcount sources nothing, so it cannot run past anything.
        const SizeT requiredBytes =
            maxdrawcount == 0 ? 0
                              : static_cast<SizeT>(indirect) +
                                    static_cast<SizeT>(maxdrawcount - 1) * effectiveStride + commandSize;
        if (!indirectBuffer || requiredBytes > indirectBuffer->GetSize()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             "maxdrawcount commands would be sourced from beyond the end of the "
                                             "buffer bound to GL_DRAW_INDIRECT_BUFFER."));
            return false;
        }
        return true;
    }

    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride) {
        // Argument validation before the backend-availability check: see DispatchComputeIndirect.
        // DrawElementsIndirectCommand: count, instanceCount, firstIndex, baseVertex, baseInstance.
        if (!ValidateIndirectCountDraw(reinterpret_cast<GLintptr>(indirect), drawcount, maxdrawcount, stride,
                                       5 * sizeof(Uint32), __func__)) {
            return;
        }
        // The only two draw entry points that were missing this. Every backend draw path
        // dereferences GetProgramForDraw() unconditionally, so "no current program" has to be
        // stopped here or it is a null dereference rather than the INVALID_OPERATION the spec
        // asks for - reachable through a bound pipeline that supplies no graphics stage.
        //
        // AFTER the argument checks, unlike the sibling draw entry points, and deliberately:
        // the argument rules here are properties of the call rather than of GL state, and
        // NegativeApiErrorsTest.IndirectParameterDrawsCheckBothBuffers pins the INVALID_VALUE
        // they produce for a call made with no program bound. Same precedence decision, and
        // the same reason, as DispatchComputeIndirect above.
        if (!ValidateCurrentProgramForExecution(__func__)) return;
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
        // Argument validation before the backend-availability check: see DispatchComputeIndirect.
        // DrawArraysIndirectCommand: count, instanceCount, first, baseInstance.
        if (!ValidateIndirectCountDraw(reinterpret_cast<GLintptr>(indirect), drawcount, maxdrawcount, stride,
                                       4 * sizeof(Uint32), __func__)) {
            return;
        }
        // See MultiDrawElementsIndirectCount, including why this one goes last.
        if (!ValidateCurrentProgramForExecution(__func__)) return;
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
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawRangeElementsBaseVertex_Backend(mode, start, end, count, type, indices, basevertex);
    }

    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawRangeElements_Backend(mode, start, end, count, type, indices);
    }

    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseVertexBaseInstance_Backend(mode, count, type, indices, instancecount, basevertex,
                                                            baseinstance);
    }

    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseVertex_Backend(mode, count, type, indices, instancecount, basevertex);
    }

    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstancedBaseInstance_Backend(mode, count, type, indices, instancecount, baseinstance);
    }

    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawElementsInstanced_Backend(mode, count, type, indices, instancecount);
    }

    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        if (!ValidateDrawElementsIndexType(__func__, type)) return;
        if (!ValidateIndirectDrawSource(__func__, indirect, kDrawElementsIndirectCommandBytes)) return;
        DrawElementsIndirect_Backend(mode, type, indirect);
    }

    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArraysInstancedBaseInstance_Backend(mode, first, count, instancecount, baseinstance);
    }

    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        DrawArraysInstanced_Backend(mode, first, count, instancecount);
    }

    void DrawArraysIndirect(GLenum mode, const void* indirect) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        if (!ValidateIndirectDrawSource(__func__, indirect, kDrawArraysIndirectCommandBytes)) return;
        DrawArraysIndirect_Backend(mode, indirect);
    }

    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        AccountTransformFeedbackPrimitives(mode, count);
        DrawElementsBaseVertex_Backend(mode, count, type, indices, basevertex);
    }

    void DrawArrays(GLenum mode, GLint first, GLsizei count) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        AccountTransformFeedbackPrimitives(mode, count);
        DrawArrays_Backend(mode, first, count);
    }

    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
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
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElements_Backend(mode, count, type, indices, drawcount);
    }

    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
        if (!ValidateCurrentProgramForExecution(__func__)) return;
        if (!ValidatePrimitiveModeForBackend(__func__, mode)) return;
        MultiDrawElementsBaseVertex_Backend(mode, count, type, indices, drawcount, basevertex);
    }

    void Clear(GLbitfield mask) {
        Clear_Backend(mask);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        if (!ValidatePrimitiveModeEnum(__func__, mode)) return;
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
        const auto& program = MG_State::pGLContext->GetProgramForDraw();
        if (!program || !program->GetLinkStatus() || program->GetTransformFeedbackVaryingCount() == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "No program with transform feedback varyings is active."));
            return;
        }
        // Every capture buffer slot the program's mode uses must have a buffer bound. A slot
        // of stride 0 - two consecutive gl_NextBuffer entries - captures nothing and so needs
        // no binding.
        const SizeT usedBufferCount = program->GetTransformFeedbackBufferCount();
        for (SizeT i = 0; i < usedBufferCount; ++i) {
            if (program->GetTransformFeedbackStride(static_cast<Uint32>(i)) == 0) continue;
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
        if (const auto beginXfb = MG_Backend::gBackendFunctionsTable.GL.BeginTransformFeedback) {
            beginXfb(primitiveMode);
        }
    }

    // Vulkan transform feedback captures triangle strips in plain (i, i+1, i+2)
    // vertex order, but GL decomposes odd strip triangles as (i+1, i, i+2)
    // (GL 4.6 table 10.1). With the geometry stage's statically-known strip
    // lengths the captured records are reordered in place: swap the first two
    // vertex records of every odd triangle within each emitted strip.
    static void FixupGsStripCaptureOrder(const SharedPtr<MG_State::GLState::ProgramObject>& program,
                                         Uint64 inputPrimitives) {
        // Only Vulkan-order captures need this. A backend that runs the capture on its
        // own GL/ES driver (it owns the span, hence the EndTransformFeedback entry) has
        // already produced GL's vertex order, and reordering it again would corrupt it.
        if (MG_Backend::gBackendFunctionsTable.GL.EndTransformFeedback != nullptr) {
            return;
        }
        if (program == nullptr || !program->HasGsTriangleStripCaptureFixup() || inputPrimitives == 0) {
            return;
        }
        const auto& stripTriangles = program->GetGsStripTriangles();

        // Global triangle indices whose leading vertex pair must swap.
        Vector<Uint64> swapTriangles;
        Uint64 triangleBase = 0;
        for (Uint64 input = 0; input < inputPrimitives; ++input) {
            for (const Uint32 stripLength : stripTriangles) {
                for (Uint32 t = 1; t < stripLength; t += 2) {
                    swapTriangles.push_back(triangleBase + t);
                }
                triangleBase += stripLength;
            }
        }
        if (swapTriangles.empty()) {
            return;
        }

        for (SizeT bufferIndex = 0; bufferIndex < program->GetTransformFeedbackBufferCount(); ++bufferIndex) {
            const Uint32 stride = program->GetTransformFeedbackStride(static_cast<Uint32>(bufferIndex));
            if (stride == 0) continue;
            const auto& bindingPoint =
                MG_State::pGLContext->GetBufferBindingPoint(BufferTarget::TransformFeedback,
                                                            static_cast<Uint>(bufferIndex));
            const auto& buffer = bindingPoint.GetBoundObject();
            if (buffer == nullptr) continue;
            const Range1D range = bindingPoint.GetRange();
            const Uint8* mapped = buffer->MappedData();
            if (mapped == nullptr) continue;
            // The geometry stage amplifies, so the CPU vertex counter does not bound
            // the capture; the binding range's whole-triangle capacity does.
            const Uint64 rangeBytes = range.end > range.start ? static_cast<Uint64>(range.end - range.start) : 0;
            const Uint64 capturedTriangles = std::min<Uint64>(triangleBase, (rangeBytes / stride) / 3);

            // Observed Vulkan capture order for odd strip triangles is (i, i+2, i+1)
            // (winding preserved by swapping the trailing pair); GL wants
            // (i+1, i, i+2), which is one rotation away: (a,b,c) -> (c,a,b).
            Vector<Uint8> scratch(stride);
            for (const Uint64 triangle : swapTriangles) {
                if (triangle >= capturedTriangles) break;
                const SizeT v0Offset = static_cast<SizeT>(range.start) + static_cast<SizeT>(triangle * 3) * stride;
                const SizeT v1Offset = v0Offset + stride;
                const SizeT v2Offset = v1Offset + stride;
                Memcpy(scratch.data(), mapped + v2Offset, stride);
                buffer->WritebackFromBackend({const_cast<Uint8*>(mapped) + v1Offset, stride}, v2Offset);
                buffer->WritebackFromBackend({const_cast<Uint8*>(mapped) + v0Offset, stride}, v1Offset);
                buffer->WritebackFromBackend({scratch.data(), stride}, v0Offset);
            }
        }
    }

    void EndTransformFeedback(void) {
        if (!MG_State::pGLContext->IsTransformFeedbackActive()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Transform feedback is not active."));
            return;
        }
        const auto capturedProgram = MG_State::pGLContext->GetTransformFeedbackProgram();
        const Uint64 inputPrimitives = MG_State::pGLContext->GetTransformFeedbackInputPrimitives();
        // Closed while the capture state is still active: a backend that captures
        // through its own driver reads the capture program and buffer bindings here.
        if (const auto endXfb = MG_Backend::gBackendFunctionsTable.GL.EndTransformFeedback) {
            endXfb();
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
        FixupGsStripCaptureOrder(capturedProgram, inputPrimitives);
    }

    void PauseTransformFeedback(void) {
        if (!MG_State::pGLContext->IsTransformFeedbackActive() ||
            MG_State::pGLContext->IsTransformFeedbackPaused()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Transform feedback is not active, or is already paused."));
            return;
        }
        MG_State::pGLContext->SetTransformFeedbackPaused(true);
        if (const auto pauseXfb = MG_Backend::gBackendFunctionsTable.GL.PauseTransformFeedback) {
            pauseXfb();
        }
    }

    void ResumeTransformFeedback(void) {
        if (!MG_State::pGLContext->IsTransformFeedbackActive() ||
            !MG_State::pGLContext->IsTransformFeedbackPaused()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Transform feedback is not paused."));
            return;
        }
        MG_State::pGLContext->SetTransformFeedbackPaused(false);
        if (const auto resumeXfb = MG_Backend::gBackendFunctionsTable.GL.ResumeTransformFeedback) {
            resumeXfb();
        }
    }

    void GenTransformFeedbacks(GLsizei n, GLuint* ids) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "n must be non-negative."));
            return;
        }
        if (n == 0 || ids == nullptr) return;
        Vector<Uint> names;
        MG_State::pGLContext->GenTransformFeedbackNames(static_cast<Uint>(n), names);
        Memcpy(ids, names.data(), static_cast<SizeT>(n) * sizeof(GLuint));
    }

    void CreateTransformFeedbacks(GLsizei n, GLuint* ids) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "n must be non-negative."));
            return;
        }
        if (n == 0 || ids == nullptr) return;
        Vector<Uint> names;
        MG_State::pGLContext->GenTransformFeedbackNames(static_cast<Uint>(n), names);
        // Unlike glGenTransformFeedbacks, the names are objects immediately: there is no bind step
        // to create them from (GL 4.6 core 13.2.1).
        for (const Uint name : names) {
            MG_State::pGLContext->CreateTransformFeedbackObject(name);
        }
        Memcpy(ids, names.data(), static_cast<SizeT>(n) * sizeof(GLuint));
    }

    namespace {
        // Shared front half of the by-name transform feedback entry points: the object has to exist
        // (INVALID_OPERATION otherwise) before anything else about the call is looked at.
        Bool ValidateNamedTransformFeedback(GLuint xfb, const char* functionName) {
            if (!MG_State::pGLContext->IsTransformFeedbackObject(xfb)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 std::to_string(xfb) + " is not a transform feedback object."));
                return false;
            }
            return true;
        }

        Bool ValidateTransformFeedbackBufferIndex(GLuint index, const char* functionName) {
            if (index >= MG_State::GLState::GLContext::MAX_TRANSFORM_FEEDBACK_BUFFERS) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 "index exceeds GL_MAX_TRANSFORM_FEEDBACK_BUFFERS."));
                return false;
            }
            return true;
        }

        // A capture binding may not be changed while the object is capturing (GL 4.6 core 13.2.2).
        Bool ValidateNamedTransformFeedbackNotActive(GLuint xfb, const char* functionName) {
            if (MG_State::pGLContext->IsNamedTransformFeedbackActive(xfb)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 "The transform feedback object is capturing."));
                return false;
            }
            return true;
        }

        SharedPtr<MG_State::GLState::BufferObject> ResolveTransformFeedbackBuffer(GLuint buffer,
                                                                                 const char* functionName) {
            if (buffer == 0) return nullptr;
            if (!MG_State::pGLContext->ValidateBufferName(buffer)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 std::to_string(buffer) + " is not a buffer object."));
                return nullptr;
            }
            return MG_State::pGLContext->GetBufferObject(buffer);
        }
    } // namespace

    void TransformFeedbackBufferBase(GLuint xfb, GLuint index, GLuint buffer) {
        if (!ValidateNamedTransformFeedback(xfb, __func__)) return;
        if (!ValidateTransformFeedbackBufferIndex(index, __func__)) return;
        if (!ValidateNamedTransformFeedbackNotActive(xfb, __func__)) return;
        if (buffer != 0 && !MG_State::pGLContext->ValidateBufferName(buffer)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(buffer) + " is not a buffer object."));
            return;
        }
        MG_State::pGLContext->SetNamedTransformFeedbackBinding(xfb, index,
                                                               ResolveTransformFeedbackBuffer(buffer, __func__), {},
                                                               false);
    }

    void TransformFeedbackBufferRange(GLuint xfb, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        if (!ValidateNamedTransformFeedback(xfb, __func__)) return;
        if (!ValidateTransformFeedbackBufferIndex(index, __func__)) return;
        if (!ValidateNamedTransformFeedbackNotActive(xfb, __func__)) return;
        if (offset < 0 || size <= 0 || (offset % 4) != 0 || (size % 4) != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "offset and size must be non-negative multiples of 4."));
            return;
        }
        if (buffer != 0 && !MG_State::pGLContext->ValidateBufferName(buffer)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(buffer) + " is not a buffer object."));
            return;
        }
        auto bufferObject = ResolveTransformFeedbackBuffer(buffer, __func__);
        const Range1D range{static_cast<SizeT>(offset), static_cast<SizeT>(offset) + static_cast<SizeT>(size)};
        MG_State::pGLContext->SetNamedTransformFeedbackBinding(xfb, index, bufferObject, range,
                                                               bufferObject != nullptr);
    }

    void GetTransformFeedbackiv(GLuint xfb, GLenum pname, GLint* param) {
        if (!ValidateNamedTransformFeedback(xfb, __func__)) return;
        if (!param) return;
        switch (pname) {
        case GL_TRANSFORM_FEEDBACK_ACTIVE:
            *param = MG_State::pGLContext->IsNamedTransformFeedbackActive(xfb) ? GL_TRUE : GL_FALSE;
            return;
        case GL_TRANSFORM_FEEDBACK_PAUSED:
            *param = MG_State::pGLContext->IsNamedTransformFeedbackPaused(xfb) ? GL_TRUE : GL_FALSE;
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname must be GL_TRANSFORM_FEEDBACK_ACTIVE or _PAUSED."));
            return;
        }
    }

    void GetTransformFeedbacki_v(GLuint xfb, GLenum pname, GLuint index, GLint* param) {
        if (!ValidateNamedTransformFeedback(xfb, __func__)) return;
        if (pname != GL_TRANSFORM_FEEDBACK_BUFFER_BINDING) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname must be GL_TRANSFORM_FEEDBACK_BUFFER_BINDING."));
            return;
        }
        if (!ValidateTransformFeedbackBufferIndex(index, __func__)) return;
        if (!param) return;
        const auto binding = MG_State::pGLContext->GetNamedTransformFeedbackBinding(xfb, index);
        *param = binding.Buffer ? static_cast<GLint>(binding.Buffer->GetExternalIndex()) : 0;
    }

    void GetTransformFeedbacki64_v(GLuint xfb, GLenum pname, GLuint index, GLint64* param) {
        if (!ValidateNamedTransformFeedback(xfb, __func__)) return;
        if (pname != GL_TRANSFORM_FEEDBACK_BUFFER_START && pname != GL_TRANSFORM_FEEDBACK_BUFFER_SIZE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname must be GL_TRANSFORM_FEEDBACK_BUFFER_START or _SIZE."));
            return;
        }
        if (!ValidateTransformFeedbackBufferIndex(index, __func__)) return;
        if (!param) return;
        const auto binding = MG_State::pGLContext->GetNamedTransformFeedbackBinding(xfb, index);
        // glTransformFeedbackBufferBase leaves both at zero; only the range form sets them
        // (GL 4.6 core table 23.48).
        if (!binding.Buffer || !binding.HasExplicitRange) {
            *param = 0;
            return;
        }
        *param = (pname == GL_TRANSFORM_FEEDBACK_BUFFER_START)
                     ? static_cast<GLint64>(binding.Range.start)
                     : static_cast<GLint64>(binding.Range.end - binding.Range.start);
    }

    void DeleteTransformFeedbacks(GLsizei n, const GLuint* ids) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "n must be non-negative."));
            return;
        }
        if (ids == nullptr) return;
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint id = ids[i];
            // Unknown names and 0 are silently ignored; an object whose capture span is
            // still open is not (GL 4.6 core 13.2.1).
            if (id == 0 || !MG_State::pGLContext->ValidateTransformFeedbackName(id)) continue;
            if (id == MG_State::pGLContext->GetBoundTransformFeedbackName() &&
                MG_State::pGLContext->IsTransformFeedbackActive()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Cannot delete a transform feedback object whose capture is active."));
                continue;
            }
            if (const auto deleteXfb = MG_Backend::gBackendFunctionsTable.GL.DeleteTransformFeedback) {
                deleteXfb(id);
            }
            MG_State::pGLContext->MarkTransformFeedbackObjectForDeletion(id);
        }
    }

    void BindTransformFeedback(GLenum target, GLuint id) {
        if (target != GL_TRANSFORM_FEEDBACK) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "target must be GL_TRANSFORM_FEEDBACK."));
            return;
        }
        // A running capture pins its object; only a paused one may be swapped out.
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            !MG_State::pGLContext->IsTransformFeedbackPaused()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Transform feedback is active and not paused."));
            return;
        }
        if (!MG_State::pGLContext->ValidateTransformFeedbackName(id)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(id) + " is not a transform feedback object name."));
            return;
        }
        MG_State::pGLContext->BindTransformFeedbackObject(id);
        if (const auto bindXfb = MG_Backend::gBackendFunctionsTable.GL.BindTransformFeedback) {
            bindXfb(id);
        }
    }

    GLboolean IsTransformFeedback(GLuint id) {
        // Name 0 is the default object, and a name glGenTransformFeedbacks handed out only
        // becomes the name of an object once it has been bound.
        return MG_State::pGLContext->IsTransformFeedbackObject(id) ? GL_TRUE : GL_FALSE;
    }

    // glDrawTransformFeedback[Stream][Instanced]: replays the vertices the named object
    // captured in its last completed span, as if by glDrawArraysInstanced with that count
    // (GL 4.6 core 10.3.7).
    static void DrawTransformFeedbackImpl(const char* functionName, GLenum mode, GLuint id, GLuint stream,
                                          GLsizei instancecount) {
        if (!ValidateCurrentProgramForExecution(functionName)) return;
        if (!ValidatePrimitiveModeForBackend(functionName, mode)) return;
        if (instancecount < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "instancecount must be non-negative."));
            return;
        }
        // "id is not the name of a transform feedback object" has to mean the same thing here
        // as it does to glIsTransformFeedback, and the two predicates are not interchangeable:
        // a name glGenTransformFeedbacks handed out is only reserved until it is first bound,
        // and only the bind turns it into an object (GL 4.6 core 13.2.1). ValidateTransformFeedbackName
        // answers the reservation question - the right one for glBindTransformFeedback, which is
        // what turns a reserved name into an object - so using it here let a generated-but-unbound
        // name through to the completed-span check below and raised INVALID_OPERATION where the
        // spec asks for INVALID_VALUE. Name 0 is the default object and always drawable.
        if (id != 0 && !MG_State::pGLContext->IsTransformFeedbackObject(id)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             std::to_string(id) + " is not a transform feedback object name."));
            return;
        }
        // GL_MAX_VERTEX_STREAMS is 1, so stream 0 is the only one that exists.
        if (stream != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "stream must be less than GL_MAX_VERTEX_STREAMS."));
            return;
        }
        // Drawing from an object whose capture is currently open is legal and deliberate:
        // it is how a transform feedback result is fed straight back into the next span
        // (ARB_transform_feedback2 lists no such restriction).
        if (!MG_State::pGLContext->HasTransformFeedbackCompletedSpan(id)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "glEndTransformFeedback has never been called for this object."));
            return;
        }

        const Uint64 vertices = MG_State::pGLContext->GetTransformFeedbackRecordedVertices(id);
        if (vertices == 0) return;
        const auto count = static_cast<GLsizei>(vertices);
        AccountTransformFeedbackPrimitives(mode, count);
        if (instancecount == 1) {
            DrawArrays_Backend(mode, 0, count);
        } else {
            DrawArraysInstanced_Backend(mode, 0, count, instancecount);
        }
    }

    void DrawTransformFeedback(GLenum mode, GLuint id) {
        DrawTransformFeedbackImpl(__func__, mode, id, 0, 1);
    }

    void DrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei instancecount) {
        DrawTransformFeedbackImpl(__func__, mode, id, 0, instancecount);
    }

    void DrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream) {
        DrawTransformFeedbackImpl(__func__, mode, id, stream, 1);
    }

    void DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei instancecount) {
        DrawTransformFeedbackImpl(__func__, mode, id, stream, instancecount);
    }

} // namespace MobileGL::MG_Impl::GLImpl
