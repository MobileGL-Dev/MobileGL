// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_ProgramPipeline.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_ProgramPipeline.h"

#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        void RecordPipelineError(ErrorCode code, const char* function, String message) {
            MG_State::pGLContext->RecordError(
                code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", function, Move(message)));
        }

        // GL 4.6 core 7.4 asks only that the name came from GenProgramPipelines and has not been
        // deleted - so a name that was reserved and never bound is legal here, and the command
        // MATERIALIZES it rather than rejecting it.
        //
        // Requiring a bound object instead is what broke every separable-program conformance case
        // across three families: the CTS reserves a name, calls glUseProgramStages three times and
        // only then binds, which is the order the spec's own example uses. Each of those calls
        // failed with INVALID_OPERATION, so the stage programs were never recorded - the pipeline
        // stayed empty, GetProgramForDraw flattened nothing and the draw painted nothing, and the
        // rejected calls' error was left in the queue for the harness to find. One cause, both
        // symptoms.
        const SharedPtr<MG_State::GLState::ProgramPipelineObject>* TryGetPipeline(GLuint pipeline,
                                                                                 const char* function) {
            const auto& object = MG_State::pGLContext->MaterializeProgramPipelineObject(pipeline);
            if (!object) {
                RecordPipelineError(ErrorCode::InvalidOperation, function,
                                    std::format("Program pipeline {} does not exist.", pipeline));
                return nullptr;
            }
            return &object;
        }

        Bool ValidatePipelineCount(GLsizei n, const char* function) {
            if (n < 0) {
                RecordPipelineError(ErrorCode::InvalidValue, function, "n must be non-negative.");
                return false;
            }
            return true;
        }

        // GL 4.6 core table 7.1 maps each stage bit onto a shader stage.
        Bool TryResolveStageBit(GLbitfield bit, ShaderStage& outStage) {
            switch (bit) {
            case GL_VERTEX_SHADER_BIT: outStage = ShaderStage::Vertex; return true;
            case GL_TESS_CONTROL_SHADER_BIT: outStage = ShaderStage::TessControl; return true;
            case GL_TESS_EVALUATION_SHADER_BIT: outStage = ShaderStage::TessEval; return true;
            case GL_GEOMETRY_SHADER_BIT: outStage = ShaderStage::Geometry; return true;
            case GL_FRAGMENT_SHADER_BIT: outStage = ShaderStage::Fragment; return true;
            case GL_COMPUTE_SHADER_BIT: outStage = ShaderStage::Compute; return true;
            default: return false;
            }
        }

        constexpr GLbitfield kAllStageBits = GL_VERTEX_SHADER_BIT | GL_TESS_CONTROL_SHADER_BIT |
                                            GL_TESS_EVALUATION_SHADER_BIT | GL_GEOMETRY_SHADER_BIT |
                                            GL_FRAGMENT_SHADER_BIT | GL_COMPUTE_SHADER_BIT;
    } // namespace

    void GenProgramPipelines(GLsizei n, GLuint* pipelines) {
        if (!ValidatePipelineCount(n, __func__)) return;
        if (n == 0 || !pipelines) return;

        static thread_local Vector<GLuint> names;
        MG_State::pGLContext->GenProgramPipelineNames(static_cast<Uint>(n), names);
        Memcpy(pipelines, names.data(), static_cast<SizeT>(n) * sizeof(GLuint));
    }

    void CreateProgramPipelines(GLsizei n, GLuint* pipelines) {
        if (!ValidatePipelineCount(n, __func__)) return;
        if (n == 0 || !pipelines) return;

        static thread_local Vector<GLuint> names;
        MG_State::pGLContext->GenProgramPipelineNames(static_cast<Uint>(n), names);
        for (GLsizei i = 0; i < n; ++i) {
            pipelines[i] = names[static_cast<SizeT>(i)];
            MG_State::pGLContext->CreateProgramPipelineObject(names[static_cast<SizeT>(i)]);
        }
    }

    void DeleteProgramPipelines(GLsizei n, const GLuint* pipelines) {
        if (!ValidatePipelineCount(n, __func__)) return;
        if (!pipelines) return;

        for (GLsizei i = 0; i < n; ++i) {
            // Deleting zero, an unknown name, or a name that was only reserved is silently ignored.
            MG_State::pGLContext->MarkProgramPipelineForDeletion(pipelines[i]);
        }
    }

    void BindProgramPipeline(GLuint pipeline) {
        if (pipeline != 0 && !MG_State::pGLContext->ValidateProgramPipelineName(pipeline)) {
            RecordPipelineError(ErrorCode::InvalidOperation, __func__,
                                std::format("Program pipeline name {} is not valid.", pipeline));
            return;
        }
        MG_State::pGLContext->BindProgramPipelineObject(pipeline);
    }

    GLboolean IsProgramPipeline(GLuint pipeline) {
        return MG_State::pGLContext->IsProgramPipelineObject(pipeline) ? GL_TRUE : GL_FALSE;
    }

    void GetProgramPipelineiv(GLuint pipeline, GLenum pname, GLint* params) {
        const auto* pipelineObject = TryGetPipeline(pipeline, __func__);
        if (!pipelineObject || !params) return;

        const auto stageProgramName = [&](ShaderStage stage) -> GLint {
            const auto& program = (*pipelineObject)->GetStageProgram(stage);
            return program ? static_cast<GLint>(program->GetExternalIndex()) : 0;
        };

        switch (pname) {
        case GL_ACTIVE_PROGRAM: {
            const auto& active = (*pipelineObject)->GetActiveProgram();
            *params = active ? static_cast<GLint>(active->GetExternalIndex()) : 0;
            break;
        }
        case GL_VERTEX_SHADER: *params = stageProgramName(ShaderStage::Vertex); break;
        case GL_TESS_CONTROL_SHADER: *params = stageProgramName(ShaderStage::TessControl); break;
        case GL_TESS_EVALUATION_SHADER: *params = stageProgramName(ShaderStage::TessEval); break;
        case GL_GEOMETRY_SHADER: *params = stageProgramName(ShaderStage::Geometry); break;
        case GL_FRAGMENT_SHADER: *params = stageProgramName(ShaderStage::Fragment); break;
        case GL_COMPUTE_SHADER: *params = stageProgramName(ShaderStage::Compute); break;
        case GL_VALIDATE_STATUS: *params = (*pipelineObject)->GetValidateStatus() ? GL_TRUE : GL_FALSE; break;
        case GL_INFO_LOG_LENGTH: {
            // GL counts the null terminator, and reports 0 rather than 1 for an empty log.
            const auto& log = (*pipelineObject)->GetInfoLog();
            *params = log.empty() ? 0 : static_cast<GLint>(log.length()) + 1;
            break;
        }
        default:
            RecordPipelineError(ErrorCode::InvalidEnum, __func__,
                                std::format("pname {} is not a program pipeline parameter.",
                                            MG_Util::ConvertGLEnumToString(pname)));
            break;
        }
    }

    void GetProgramPipelineInfoLog(GLuint pipeline, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
        const auto* pipelineObject = TryGetPipeline(pipeline, __func__);
        if (!pipelineObject) return;
        if (bufSize < 0) {
            RecordPipelineError(ErrorCode::InvalidValue, __func__, "bufSize must be non-negative.");
            return;
        }
        if (bufSize == 0 || !infoLog) {
            if (length) *length = 0;
            return;
        }

        const auto& log = (*pipelineObject)->GetInfoLog();
        const auto copied = std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(log.length()));
        if (copied > 0) Memcpy(infoLog, log.data(), static_cast<SizeT>(copied));
        infoLog[copied] = '\0';
        if (length) *length = copied;
    }

    void UseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program) {
        if (stages != GL_ALL_SHADER_BITS && (stages & ~kAllStageBits) != 0) {
            RecordPipelineError(ErrorCode::InvalidValue, __func__, "stages names a bit that is not a shader stage.");
            return;
        }
        const auto* pipelineObject = TryGetPipeline(pipeline, __func__);
        if (!pipelineObject) return;

        SharedPtr<MG_State::GLState::ProgramObject> programObject;
        if (program != 0) {
            if (!MG_State::pGLContext->ValidateProgramName(program)) {
                RecordPipelineError(ErrorCode::InvalidValue, __func__,
                                    std::format("{} is not the name of a program object.", program));
                return;
            }
            programObject = MG_State::pGLContext->GetProgramObject(program);
            if (!programObject) {
                RecordPipelineError(ErrorCode::InvalidValue, __func__,
                                    std::format("{} is not the name of a program object.", program));
                return;
            }
            if (!programObject->GetLinkStatus()) {
                RecordPipelineError(ErrorCode::InvalidOperation, __func__,
                                    std::format("Program {} has not been linked successfully.", program));
                return;
            }
        }

        const GLbitfield selected = stages == GL_ALL_SHADER_BITS ? kAllStageBits : stages;
        for (GLbitfield bit = 1; bit != 0 && bit <= kAllStageBits; bit <<= 1) {
            if ((selected & bit) == 0) continue;
            ShaderStage stage = ShaderStage::Unknown;
            if (!TryResolveStageBit(bit, stage)) continue;
            // program == 0 clears the stage, which is what a null program reference means here.
            (*pipelineObject)->SetStageProgram(stage, programObject);
        }
    }

    void ActiveShaderProgram(GLuint pipeline, GLuint program) {
        const auto* pipelineObject = TryGetPipeline(pipeline, __func__);
        if (!pipelineObject) return;

        if (program == 0) {
            (*pipelineObject)->SetActiveProgram(nullptr);
            return;
        }
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            RecordPipelineError(ErrorCode::InvalidValue, __func__,
                                std::format("{} is not the name of a program object.", program));
            return;
        }
        auto programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) {
            RecordPipelineError(ErrorCode::InvalidValue, __func__,
                                std::format("{} is not the name of a program object.", program));
            return;
        }
        if (!programObject->GetLinkStatus()) {
            RecordPipelineError(ErrorCode::InvalidOperation, __func__,
                                std::format("Program {} has not been linked successfully.", program));
            return;
        }
        (*pipelineObject)->SetActiveProgram(programObject);
    }

    void ValidateProgramPipeline(GLuint pipeline) {
        const auto* pipelineObject = TryGetPipeline(pipeline, __func__);
        if (!pipelineObject) return;
        // Nothing here can fail today: MobileGL links each stage program on its own, so there is no
        // cross-stage interface to re-check at validation time. The log stays empty, which GL allows.
        (*pipelineObject)->SetValidateStatus(true);
    }
} // namespace MobileGL::MG_Impl::GLImpl
