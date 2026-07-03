// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Program.h"
#include "Config.h"
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>
#include <MG_Backend/BackendObjects.h>

namespace MobileGL::MG_Impl::GLImpl {
    static GLint BoolToGLInt(bool value) {
        return value ? GL_TRUE : GL_FALSE;
    }

    static bool CheckShaderNameValidity(Uint shader) {
        if (shader == 0 || !MG_State::pGLContext->ValidateShaderName(shader)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(shader) + " is not a valid name."));
            return false;
        }
        return true;
    }

    static const SharedPtr<MG_State::GLState::ShaderObject>& TryToGetShaderObject(Uint shader) {
        static const SharedPtr<MG_State::GLState::ShaderObject> nullShaderObject = nullptr;
        if (!CheckShaderNameValidity(shader)) return nullShaderObject;

        auto& shaderObject = MG_State::pGLContext->GetShaderObject(shader);
        if (!shaderObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(shader) + " is not a shader object."));
            return nullShaderObject;
        }
        return shaderObject;
    }

    static bool CheckProgramNameValidity(GLuint program) {
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " is not a valid name."));
            return false;
        }
        return true;
    }

    static const SharedPtr<MG_State::GLState::ProgramObject>& TryToGetProgramObject(GLuint program) {
        static const SharedPtr<MG_State::GLState::ProgramObject> nullProgramObject = nullptr;
        if (!CheckProgramNameValidity(program)) return nullProgramObject;

        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " is not a program object."));
            return nullProgramObject;
        }
        return programObject;
    }

    static const SharedPtr<MG_State::GLState::ProgramObject>& TryToGetLinkedProgramForInterfaceQuery(GLuint program,
                                                                                                     const char* caller) {
        static const SharedPtr<MG_State::GLState::ProgramObject> nullProgramObject = nullptr;
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            const ErrorCode error = MG_State::pGLContext->ValidateShaderName(program)
                ? ErrorCode::InvalidOperation
                : ErrorCode::InvalidValue;
            MG_State::pGLContext->RecordError(
                error,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::to_string(program) + " is not a linked program object."));
            return nullProgramObject;
        }

        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::to_string(program) + " is not a linked program object."));
            return nullProgramObject;
        }
        return programObject;
    }

    static bool IsProgramInterfaceEnum(GLenum programInterface) {
        switch (programInterface) {
        case GL_UNIFORM:
        case GL_UNIFORM_BLOCK:
        case GL_PROGRAM_INPUT:
        case GL_PROGRAM_OUTPUT:
        case GL_BUFFER_VARIABLE:
        case GL_SHADER_STORAGE_BLOCK:
        case GL_ATOMIC_COUNTER_BUFFER:
        case GL_TRANSFORM_FEEDBACK_VARYING:
        case GL_VERTEX_SUBROUTINE:
        case GL_TESS_CONTROL_SUBROUTINE:
        case GL_TESS_EVALUATION_SUBROUTINE:
        case GL_GEOMETRY_SUBROUTINE:
        case GL_FRAGMENT_SUBROUTINE:
        case GL_COMPUTE_SUBROUTINE:
        case GL_VERTEX_SUBROUTINE_UNIFORM:
        case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
        case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
        case GL_GEOMETRY_SUBROUTINE_UNIFORM:
        case GL_FRAGMENT_SUBROUTINE_UNIFORM:
        case GL_COMPUTE_SUBROUTINE_UNIFORM:
            return true;
        default:
            return false;
        }
    }

    static bool IsSubroutineUniformInterface(GLenum programInterface) {
        switch (programInterface) {
        case GL_VERTEX_SUBROUTINE_UNIFORM:
        case GL_TESS_CONTROL_SUBROUTINE_UNIFORM:
        case GL_TESS_EVALUATION_SUBROUTINE_UNIFORM:
        case GL_GEOMETRY_SUBROUTINE_UNIFORM:
        case GL_FRAGMENT_SUBROUTINE_UNIFORM:
        case GL_COMPUTE_SUBROUTINE_UNIFORM:
            return true;
        default:
            return false;
        }
    }

    static bool ValidateProgramInterfaceivQuery(GLenum programInterface, GLenum pname) {
        bool valid = IsProgramInterfaceEnum(programInterface);
        switch (pname) {
        case GL_ACTIVE_RESOURCES:
            break;
        case GL_MAX_NAME_LENGTH:
            valid = valid && programInterface != GL_ATOMIC_COUNTER_BUFFER;
            break;
        case GL_MAX_NUM_ACTIVE_VARIABLES:
            valid = programInterface == GL_UNIFORM_BLOCK || programInterface == GL_ATOMIC_COUNTER_BUFFER ||
                    programInterface == GL_SHADER_STORAGE_BLOCK;
            break;
        case GL_MAX_NUM_COMPATIBLE_SUBROUTINES:
            valid = IsSubroutineUniformInterface(programInterface);
            break;
        default:
            valid = false;
            break;
        }
        if (!valid) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported program interface query."));
        }
        return valid;
    }

    static bool ValidateNamedProgramResourceInterface(GLenum programInterface, const char* caller) {
        if (!IsProgramInterfaceEnum(programInterface) || programInterface == GL_ATOMIC_COUNTER_BUFFER) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Unsupported named program resource interface."));
            return false;
        }
        return true;
    }

    static Int GetKnownProgramResourceCount(const SharedPtr<MG_State::GLState::ProgramObject>& programObject,
                                            GLenum programInterface) {
        switch (programInterface) {
        case GL_UNIFORM:
            return programObject->GetUniformCount();
        case GL_UNIFORM_BLOCK:
            return programObject->GetActiveUniformBlocksCount();
        case GL_PROGRAM_INPUT:
            return programObject->GetActiveAttributesCount();
        case GL_PROGRAM_OUTPUT:
            return programObject->GetActiveFragmentOutputCount();
        default:
            return -1;
        }
    }

    void CopyStr(GLsizei bufSize, GLsizei* length, GLchar* dst, const char* src, GLsizei srcLength) {
        if (bufSize <= 0) {
            if (length) *length = 0;
            return;
        }

        auto sz = std::min(bufSize - 1, srcLength);
        Memcpy(dst, src, sz);
        dst[sz] = '\0';
        if (length) *length = sz;
    }

    bool RecordInvalidUniformLocationError(const char* functionName, GLint location, const String& targetDescription) {
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                         "location " + std::to_string(location) +
                                             " does not correspond to a valid uniform variable location for " +
                                             targetDescription + "."));
        return false;
    }

    GLint GetOpaqueUniformUnitLimit(const glslang::TType* type) {
        const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
        if (type && type->isImage()) return dynamicParameters.MaxImageUnits;
        if (type && type->isTexture()) return dynamicParameters.MaxCombinedTextureImageUnits;
        return 0;
    }

    bool ValidateOpaqueUniformUnit(const char* functionName, const glslang::TType* type, GLint unit) {
        const GLint limit = GetOpaqueUniformUnitLimit(type);
        if (unit < 0 || unit >= limit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Opaque uniform unit is out of range."));
            return false;
        }
        return true;
    }

    bool ValidateShaderStorageBlockBinding(GLuint binding) {
        SizeT maxBindingCount = MG_State::pGLContext->GetBufferBindingPointCount(BufferTarget::ShaderStorage);
        if (MG_Backend::pActiveBackendObject) {
            const Int backendCount =
                MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBufferBindings;
            maxBindingCount = std::min(maxBindingCount, static_cast<SizeT>(std::max(backendCount, 0)));
        }

        if (binding < maxBindingCount) {
            return true;
        }

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidValue,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                         "Shader storage block binding is out of range."));
        return false;
    }

    void AttachShader_State(GLuint program, GLuint shader) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;
        if (!programObject->AttachShader(shaderObject)) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                           std::to_string(shader) +
                                                                               " is already attached to " +
                                                                               std::to_string(program) + "."));
            return;
        }
    }

    void BindAttribLocation_State(GLuint program, GLuint index, const GLchar* name) {
        if (index >= MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "index " + std::to_string(index) +
                                                 " is greater than or equal to `GL_MAX_VERTEX_ATTRIBS`."));
            return;
        }

        if (strncmp(name, "gl_", 3) == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "name " + std::string(name) + " starts with the reserved prefix `gl_`."));
            return;
        }

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        MGLOG_D("%s: loc %02d = \"%s\"", __func__, index, name);
        programObject->SetExplicitVertexInLocation(index, name);
    }

    void CompileShader_State(GLuint shader) {
        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;
        shaderObject->Compile();
    }

    GLuint CreateProgram_State() {
        return MG_State::pGLContext->CreateProgram();
    }

    GLuint CreateShader_State(GLenum type) {
        auto shaderId = MG_State::pGLContext->CreateShader(MG_Util::ConvertGLEnumToShaderStage(type));
        if (shaderId == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`shaderType` is not an accepted value."));
            return 0;
        }
        return shaderId;
    }

    void DeleteProgram_State(GLuint program) {
        if (!CheckProgramNameValidity(program)) return;
        MG_State::pGLContext->MarkProgramForDeletion(program);
    }

    void DeleteShader_State(GLuint shader) {
        if (!CheckShaderNameValidity(shader)) return;
        MG_State::pGLContext->MarkShaderForDeletion(shader);
    }

    void DetachShader_State(GLuint program, GLuint shader) {
        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        auto count = programObject->DetachShader(shaderObject);
        if (count <= 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Shader is not attached to program."));
            return;
        }
    }

    void GetActiveAttrib_State(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                               GLenum* type, GLchar* name) {
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "bufSize " + std::to_string(bufSize) + " is less than 0."));
            return;
        }
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) return;
        auto attribCount = programObject->GetActiveAttributesCount();
        if (index >= attribCount) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "index " + std::to_string(index) +
                        " is greater than or equal to the number of active attribute variables in " +
                        std::to_string(program) + "."));
            return;
        }
        if (size != nullptr) *size = programObject->GetActiveAttribArraySize(index);
        if (type != nullptr) *type = programObject->GetActiveAttribType(index);
        if (bufSize == 0) return;
        auto& attribName = programObject->GetActiveAttribName(index);
        CopyStr(bufSize, length, name, attribName.c_str(), (GLsizei)attribName.length());
    }

    void GetActiveUniform_State(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                                GLenum* type, GLchar* name) {
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "bufSize " + std::to_string(bufSize) + " is less than 0."));
            return;
        }
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) return;
        auto uniformCount = programObject->GetUniformCount();
        if (index >= uniformCount) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "index " + std::to_string(index) +
                        " is greater than or equal to the number of active uniform variables in " +
                        std::to_string(program) + "."));
            return;
        }
        if (size != nullptr) *size = programObject->GetActiveUniformArraySize(index);
        if (type != nullptr) *type = programObject->GetActiveUniformType(index);
        if (bufSize == 0) return;
        auto& uniformName = programObject->GetActiveUniformName(index);
        CopyStr(bufSize, length, name, uniformName.c_str(), (GLsizei)uniformName.length());
    }

    void GetUniformIndices_State(GLuint program, GLsizei uniformCount, const GLchar* const* uniformNames,
                                 GLuint* uniformIndices) {
        if (uniformCount < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "uniformCount " + std::to_string(uniformCount) + " is less than 0."));
            return;
        }
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) return;
        if (uniformCount == 0 || uniformNames == nullptr || uniformIndices == nullptr) return;

        for (GLsizei i = 0; i < uniformCount; ++i) {
            const char* uniformName = uniformNames[i];
            if (uniformName == nullptr) {
                uniformIndices[i] = GL_INVALID_INDEX;
                continue;
            }

            const Int uniformIndex = programObject->GetActiveUniformIndex(uniformName);
            uniformIndices[i] = uniformIndex >= 0 ? static_cast<GLuint>(uniformIndex) : GL_INVALID_INDEX;
        }
    }

    void GetAttachedShaders_State(GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders) {
        if (maxCount < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "maxCount " + std::to_string(maxCount) + " is less than 0."));
            return;
        }
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        const auto& s = programObject->GetAttachedShaders();
        GLsizei c = std::min((GLsizei)s.size(), maxCount);
        if (count) *count = c;
        for (GLsizei i = 0; i < c; ++i) {
            shaders[i] = s[i]->GetExternalIndex();
        }
    }

    GLint GetAttribLocation_State(GLuint program, const GLchar* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return -1;
        if (strncmp(name, "gl_", 3) == 0) return -1;
        if (!programObject->GetLinkStatus()) return -1;
        return programObject->GetAttributeLocation(name);
    }

    void GetProgramiv_State(GLuint program, GLenum pname, GLint* params) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        switch (pname) {
        case GL_DELETE_STATUS:
            *params = programObject->GetDeleteStatus();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_LINK_STATUS:
            *params = programObject->GetLinkStatus();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_VALIDATE_STATUS:
            *params = programObject->GetValidateStatus();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_INFO_LOG_LENGTH: {
            const auto& log = programObject->GetInfoLog();
            *params = log.empty() ? 0 : static_cast<GLint>(log.length()) + 1;
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        }
        case GL_ATTACHED_SHADERS: {
            const auto& attachedShaders = programObject->GetAttachedShaders();
            *params = (GLint)attachedShaders.size();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        }
        case GL_ACTIVE_ATOMIC_COUNTER_BUFFERS:
            *params = programObject->GetActiveAtomicCounterCount();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_ATTRIBUTES:
            *params = programObject->GetActiveAttributesCount();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
            *params = programObject->GetActiveAttributesMaxLength() + 1;
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_UNIFORMS:
            *params = (GLint)programObject->GetUniformCount();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_UNIFORM_MAX_LENGTH:
            *params = programObject->GetUniformMaxLength() + 1;
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_UNIFORM_BLOCKS: // GL >= 3.1
            *params = programObject->GetActiveUniformBlocksCount();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: // ditto.
            *params = programObject->GetActiveUniformBlocksMaxNameLength() + 1;
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_COMPUTE_WORK_GROUP_SIZE: { // GL >= 4.3
            if (!programObject->GetLinkStatus() || programObject->GetShaderIndexByStage(ShaderStage::Compute) < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) +
                                                     " is not a linked program object with a compute shader."));
                return;
            }
            params[0] = static_cast<GLint>(programObject->GetComputeLocalSize(0));
            params[1] = static_cast<GLint>(programObject->GetComputeLocalSize(1));
            params[2] = static_cast<GLint>(programObject->GetComputeLocalSize(2));
            MGLOG_D("%s: %s = (%d, %d, %d)", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), params[0],
                    params[1], params[2]);
            break;
        }

        case GL_PROGRAM_BINARY_LENGTH:

        case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
        case GL_TRANSFORM_FEEDBACK_VARYINGS:
        case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
        case GL_GEOMETRY_VERTICES_OUT:
        case GL_GEOMETRY_INPUT_TYPE:
        case GL_GEOMETRY_OUTPUT_TYPE:
        default:
            MGLOG_D("%s: %s", __func__, MG_Util::ConvertGLEnumToString(pname).c_str());
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname " + std::to_string(pname) + " is not an accepted value."));
            return;
        }
    }

    void GetProgramInfoLog_State(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        const auto& log = programObject->GetInfoLog();
        CopyStr(bufSize, length, infoLog, log.c_str(), (GLsizei)log.length());
    }

    void GetShaderiv_State(GLuint shader, GLenum pname, GLint* params) {
        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;

        switch (pname) {
        case GL_SHADER_TYPE:
            *params = (GLint)MG_Util::ConvertShaderStageToGLEnum(shaderObject->GetShaderStage());
            break;
        case GL_DELETE_STATUS:
            *params = shaderObject->GetDeleteStatus();
            break;
        case GL_COMPILE_STATUS:
            *params = shaderObject->GetCompileStatus();
            break;
        case GL_INFO_LOG_LENGTH:
            *params = shaderObject->GetInfoLog().empty() ? 0 : (GLint)shaderObject->GetInfoLog().length() + 1;
            break;
        case GL_SHADER_SOURCE_LENGTH:
            *params = shaderObject->GetShaderSource().empty() ? 0 : (GLint)shaderObject->GetShaderSource().length() + 1;
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname " + std::to_string(pname) + " is not an accepted value."));
            return;
        }
    }

    void GetShaderInfoLog_State(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;

        const auto& log = shaderObject->GetInfoLog();
        CopyStr(bufSize, length, infoLog, log.c_str(), (GLsizei)log.length());
    }

    void GetShaderSource_State(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "bufSize " + std::to_string(bufSize) + " is less than 0."));
        }

        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;

        auto& src = shaderObject->GetShaderSource();
        CopyStr(bufSize, length, source, src.c_str(), (GLsizei)src.length());
    }

    GLint GetUniformLocation_State(GLuint program, const GLchar* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return -1;
        auto loc = programObject->GetUniformLocation(name);
        MGLOG_D("%s: loc %02d = %s", __func__, loc, name);
        return loc;
    }

    void GetUniform_State(GLuint program, GLint location, void* params) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " has not been successfully linked."));
            return;
        }

        // Check if location is valid
        if (!programObject->IsValidUniformLocation(location)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "location " + std::to_string(location) +
                        " does not correspond to a valid uniform variable location for the specified program object."));
            return;
        }

        auto isOpaque = programObject->IsUniformOpaqueAtLocation(location);
        if (!isOpaque) {
            // TODO: probably handle int/float differences
            auto offset = programObject->GetUniformOffset(location);
            auto size = programObject->GetUniformSizesInBytes(location);
            char* pUBO = (char*)programObject->MapUBO();
            auto* ttype = programObject->GetUniformTType(location);

            if (!ttype->isMatrix() || ttype->getMatrixCols() != 3)
                Memcpy(params, pUBO + offset, size);
            else {
                // TODO: we only deal with mat3 yet, deal with other types later
                // assuming float here, which may not be the case
                auto* pBase = pUBO + offset;
                for (int i = 0; i < ttype->getMatrixRows(); i++) {
                    Memcpy((char*)params + ttype->getMatrixCols() * sizeof(float) * i, pBase + 4 * sizeof(float) * i,
                           ttype->getMatrixCols() * sizeof(float));
                }
            }
        }
        // TODO: handle 1i variant as texture unit
    }

    template <typename T>
    void GetUniformScalar_State(GLuint program, GLint location, T* params) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " has not been successfully linked."));
            return;
        }

        if (!programObject->IsValidUniformLocation(location)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "location " + std::to_string(location) +
                        " does not correspond to a valid uniform variable location for the specified program object."));
            return;
        }

        if (programObject->IsUniformOpaqueAtLocation(location)) {
            const Int unit = std::max(programObject->GetUniformSamplerOrImageUnitIndex(location), 0);
            *params = static_cast<T>(unit);
            return;
        }

        auto offset = programObject->GetUniformOffset(location);
        auto size = programObject->GetUniformSizesInBytes(location);
        char* pUBO = static_cast<char*>(programObject->MapUBO());
        auto* ttype = programObject->GetUniformTType(location);

        if constexpr (std::is_same_v<T, GLfloat>) {
            if (ttype->isMatrix() && ttype->getMatrixCols() == 3) {
                auto* pBase = pUBO + offset;
                for (int i = 0; i < ttype->getMatrixRows(); i++) {
                    Memcpy(reinterpret_cast<char*>(params) + ttype->getMatrixCols() * sizeof(GLfloat) * i,
                           pBase + 4 * sizeof(GLfloat) * i, ttype->getMatrixCols() * sizeof(GLfloat));
                }
                return;
            }
        }

        Memcpy(params, pUBO + offset, size);
    }

    void GetUniformfv_State(GLuint program, GLint location, GLfloat* params) {
        GetUniformScalar_State(program, location, params);
    }

    void GetUniformiv_State(GLuint program, GLint location, GLint* params) {
        GetUniformScalar_State(program, location, params);
    }

    void GetUniformuiv_State(GLuint program, GLint location, GLuint* params) {
        GetUniformScalar_State(program, location, params);
    }

    GLboolean IsProgram_State(GLuint program) {
        /* FIXME: Handle situations that:
         * A program object marked for deletion with glDeleteProgram but still in use as part of current
         * rendering state is still considered a program object and glIsProgram will return GL_TRUE.
         */
        if (program == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateProgramName(program) ? GL_TRUE : GL_FALSE;
    }

    GLboolean IsShader_State(GLuint shader) {
        /* FIXME: Handle situations that:
         * A shader object marked for deletion with glDeleteShader but still attached to a program object is still
         * considered a shader object and glIsShader will return GL_TRUE.
         */
        if (shader == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateShaderName(shader) ? GL_TRUE : GL_FALSE;
    }

    void LinkProgram_State(GLuint program) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        MGLOG_D("%s: linking program %d", __func__, program);

        static Bool allowVSOnlyPrograms;
        static Bool initialized = false;
        if (!initialized) {
            const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
            if (!activeBackendObject) {
                MGLOG_E("activeBackendObject is not initialized!");
                return;
            }
            const auto& rendererInfo = activeBackendObject->GetRendererInfo();
            allowVSOnlyPrograms = (Int)rendererInfo.StaticBackendCapability.AllowVSOnlyPrograms;
        }
        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (activeBackendObject) {
            programObject->SetMaxFragmentOutputColorNumber(activeBackendObject->GetDynamicParameters().MaxDrawBuffers);
        }
        programObject->Link(!allowVSOnlyPrograms);
    }

    void ShaderSource_State(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "count " + std::to_string(count) + " is less than 0."));
            return;
        }

        auto& shaderObject = TryToGetShaderObject(shader);
        if (!shaderObject) return;

        std::string src;
        for (GLsizei i = 0; i < count; i++) {
            if (!string[i]) {
                continue;
            }
            src += (length && length[i] >= 0) ? std::string(string[i], length[i]) : std::string(string[i]);
        }
        shaderObject->SetShaderSource(Move(src));
    }

    void UseProgram_State(GLuint program) {
        MGLOG_D("UseProgram_State: program=%u", program);

        if (program == 0) {
            MG_State::pGLContext->UseProgram(0);
            return;
        }

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }
        MG_State::pGLContext->UseProgram(program);
    }

    template <GLsizei ItemCount, typename T>
    void Uniform_State(MG_State::GLState::ProgramObject& programObject, GLuint location, T* value,
                       SizeT byteOffsetInsideUniform = 0) {
        if (!programObject.IsUniformOpaqueAtLocation(location)) {
            MGLOG_D("%s: program = %d, location = %d, maxLocation = %d", __func__, programObject.GetExternalIndex(),
                    location, programObject.GetMaxUniformLocation());
            auto size = programObject.GetUniformSizesInBytes(location);
            auto offset = programObject.GetUniformOffset(location);
            MOBILEGL_ASSERT(size >= ItemCount * sizeof(T),
                            "Uniform size mismatch, expected at least %zu bytes, got %zu bytes.", ItemCount * sizeof(T),
                            size);
            MGLOG_D("%s: program = %d, location = %d, byteOffset = %d", __func__, programObject.GetExternalIndex(),
                    location, offset + byteOffsetInsideUniform);
            Memcpy((char*)programObject.MapUBO() + offset + byteOffsetInsideUniform, value, ItemCount * sizeof(T));
        } else {
            auto* ttype = programObject.GetUniformTType(location);
            if (!ttype->isTexture() && !ttype->isImage()) return;
            if constexpr (!std::is_same_v<std::remove_cv_t<T>, GLint> || ItemCount != 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Opaque uniforms can only be set with Uniform1i/Uniform1iv."));
                return;
            }
            if (!ValidateOpaqueUniformUnit(__func__, ttype, *value)) return;
            MGLOG_D("%s: program = %d, opaque uniform location = %d, name = '%s', unit = %d", __func__,
                    programObject.GetExternalIndex(), location, programObject.GetUniformName(location).c_str(),
                    static_cast<Int>(*value));
            programObject.SetUniformSamplerOrImageUnitIndex(location, *value);
        }
    }

    template <GLsizei ItemCount, typename T>
    void Uniformv_State(GLint location, GLsizei count, T* value) {
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetCurrentProgram();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        for (GLint offset = 0; offset < count; offset++) {
            if (!programObject->IsValidUniformLocation(location + offset)) {
                RecordInvalidUniformLocationError(__func__, location + offset, "the current program object");
                return;
            }
            Uniform_State<ItemCount>(*programObject, location + offset, value + offset * ItemCount);
        }
    }

    template <GLsizei ItemCount, typename T>
    void ProgramUniformv_State(GLuint program, GLint location, GLsizei count, T* value) {
        if (location == -1) return;

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }

        for (GLint offset = 0; offset < count; offset++) {
            if (!programObject->IsValidUniformLocation(location + offset)) {
                RecordInvalidUniformLocationError(__func__, location + offset,
                                                  "program " + std::to_string(program));
                return;
            }
            Uniform_State<ItemCount>(*programObject, location + offset, value + offset * ItemCount);
        }
    }

    // Helper function to transpose a 2x2 matrix
    void TransposeMatrix2x2(const GLfloat* input, GLfloat* output) {
        // Input matrix is in column-major order (OpenGL default)
        // [0  2]
        // [1  3]
        //
        // Output matrix should be in row-major order if transpose is true
        // [0  1]
        // [2  3]
        output[0] = input[0]; // 0,0 element stays the same
        output[1] = input[2]; // 0,1 element becomes 1,0
        output[2] = input[1]; // 1,0 element becomes 0,1
        output[3] = input[3]; // 1,1 element stays the same
    }

    // Helper function to transpose a 3x3 matrix
    void TransposeMatrix3x3(const GLfloat* input, GLfloat* output) {
        // Input matrix is in column-major order (OpenGL default)
        // [0  3  6]
        // [1  4  7]
        // [2  5  8]
        //
        // Output matrix should be in row-major order if transpose is true
        // [0  1  2]
        // [3  4  5]
        // [6  7  8]
        output[0] = input[0]; // 0,0 element stays the same
        output[1] = input[3]; // 0,1 element becomes 1,0
        output[2] = input[6]; // 0,2 element becomes 2,0
        output[3] = input[1]; // 1,0 element becomes 0,1
        output[4] = input[4]; // 1,1 element stays the same
        output[5] = input[7]; // 1,2 element becomes 2,1
        output[6] = input[2]; // 2,0 element becomes 0,2
        output[7] = input[5]; // 2,1 element becomes 1,2
        output[8] = input[8]; // 2,2 element stays the same
    }

    // Helper function to transpose a 4x4 matrix
    void TransposeMatrix4x4(const GLfloat* input, GLfloat* output) {
        // Input matrix is in column-major order (OpenGL default)
        // [0   4   8  12]
        // [1   5   9  13]
        // [2   6  10  14]
        // [3   7  11  15]
        //
        // Output matrix should be in row-major order if transpose is true
        // [0   1   2   3]
        // [4   5   6   7]
        // [8   9  10  11]
        // [12 13  14  15]
        output[0] = input[0];   // 0,0 element stays the same
        output[1] = input[4];   // 0,1 element becomes 1,0
        output[2] = input[8];   // 0,2 element becomes 2,0
        output[3] = input[12];  // 0,3 element becomes 3,0
        output[4] = input[1];   // 1,0 element becomes 0,1
        output[5] = input[5];   // 1,1 element stays the same
        output[6] = input[9];   // 1,2 element becomes 2,1
        output[7] = input[13];  // 1,3 element becomes 3,1
        output[8] = input[2];   // 2,0 element becomes 0,2
        output[9] = input[6];   // 2,1 element becomes 1,2
        output[10] = input[10]; // 2,2 element stays the same
        output[11] = input[14]; // 2,3 element becomes 3,2
        output[12] = input[3];  // 3,0 element becomes 0,3
        output[13] = input[7];  // 3,1 element becomes 1,3
        output[14] = input[11]; // 3,2 element becomes 2,3
        output[15] = input[15]; // 3,3 element stays the same
    }

    void Uniform1fv_State(GLint location, GLsizei count, const GLfloat* value) {
        Uniformv_State<1>(location, count, value);
    }

    void Uniform2fv_State(GLint location, GLsizei count, const GLfloat* value) {
        Uniformv_State<2>(location, count, value);
    }

    void Uniform3fv_State(GLint location, GLsizei count, const GLfloat* value) {
        Uniformv_State<3>(location, count, value);
    }

    void Uniform4fv_State(GLint location, GLsizei count, const GLfloat* value) {
        Uniformv_State<4>(location, count, value);
    }

    void Uniform1iv_State(GLint location, GLsizei count, const GLint* value) {
        Uniformv_State<1>(location, count, value);
    }

    void Uniform2iv_State(GLint location, GLsizei count, const GLint* value) {
        Uniformv_State<2>(location, count, value);
    }

    void Uniform3iv_State(GLint location, GLsizei count, const GLint* value) {
        Uniformv_State<3>(location, count, value);
    }

    void Uniform4iv_State(GLint location, GLsizei count, const GLint* value) {
        Uniformv_State<4>(location, count, value);
    }

    void Uniform1uiv_State(GLint location, GLsizei count, const GLuint* value) {
        Uniformv_State<1>(location, count, value);
    }

    void UniformMatrix2fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        // For 2x2 matrices, we have 4 elements per matrix
        // If transpose is GL_TRUE, we need to transpose the matrix data
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetCurrentProgram();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        // For matrix uniforms, we handle each matrix individually
        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "the current program object");
                return;
            }
            if (transpose == GL_TRUE) {
                // Transpose the matrix before uploading
                GLfloat transposedMatrix[4];
                TransposeMatrix2x2(value + i * 4, transposedMatrix);
                Uniform_State<4>(*programObject, location + i, transposedMatrix);
            } else {
                // No transpose needed, directly copy the matrix data
                Uniform_State<4>(*programObject, location + i, value + i * 4);
            }
        }
    }

    void UniformMatrix3fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        // For 3x3 matrices, we have 9 elements per matrix
        // If transpose is GL_TRUE, we need to transpose the matrix data
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetCurrentProgram();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        // For matrix uniforms, we handle each matrix individually
        // Handle padding in mat3 correctly!!
        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "the current program object");
                return;
            }
            if (transpose == GL_TRUE) {
                // Transpose the matrix before uploading
                GLfloat transposedMatrix[9];
                TransposeMatrix3x3(value + i * 9, transposedMatrix);
                for (int row = 0; row < 3; ++row) {
                    Uniform_State<3>(*programObject, location + i, transposedMatrix + row * 3, row * 4 * sizeof(float));
                }
            } else {
                // No transpose needed, directly copy the matrix data
                for (int row = 0; row < 3; ++row) {
                    Uniform_State<3>(*programObject, location + i, value + i * 9 + row * 3, row * 4 * sizeof(float));
                }
            }
        }
    }

    void UniformMatrix4fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        // For 4x4 matrices, we have 16 elements per matrix
        // If transpose is GL_TRUE, we need to transpose the matrix data
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetCurrentProgram();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        // For matrix uniforms, we handle each matrix individually
        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "the current program object");
                return;
            }
            if (transpose == GL_TRUE) {
                // Transpose the matrix before uploading
                GLfloat transposedMatrix[16];
                TransposeMatrix4x4(value + i * 16, transposedMatrix);
                Uniform_State<16>(*programObject, location + i, transposedMatrix);
            } else {
                // No transpose needed, directly copy the matrix data
                Uniform_State<16>(*programObject, location + i, value + i * 16);
            }
        }
    }

    void UniformMatrixNonSquarefv_State(const char* caller, GLint location, GLsizei count) {
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetCurrentProgram();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "There is no current program object."));
            return;
        }

        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(caller, location + i, "the current program object");
                return;
            }
            if (programObject->IsUniformOpaqueAtLocation(location + i)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Opaque uniforms cannot be set with matrix Uniform calls."));
                return;
            }
        }

        // TODO: Implement non-square matrix uniform uploads for non-opaque uniforms.
    }

    void ProgramUniformMatrix2fv_State(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                       const GLfloat* value) {
        if (location == -1) return;

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }

        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "program " + std::to_string(program));
                return;
            }
            if (transpose == GL_TRUE) {
                GLfloat transposedMatrix[4];
                TransposeMatrix2x2(value + i * 4, transposedMatrix);
                Uniform_State<4>(*programObject, location + i, transposedMatrix);
            } else {
                Uniform_State<4>(*programObject, location + i, value + i * 4);
            }
        }
    }

    void ProgramUniformMatrix3fv_State(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                       const GLfloat* value) {
        if (location == -1) return;

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }

        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "program " + std::to_string(program));
                return;
            }
            if (transpose == GL_TRUE) {
                GLfloat transposedMatrix[9];
                TransposeMatrix3x3(value + i * 9, transposedMatrix);
                for (int row = 0; row < 3; ++row) {
                    Uniform_State<3>(*programObject, location + i, transposedMatrix + row * 3, row * 4 * sizeof(float));
                }
            } else {
                for (int row = 0; row < 3; ++row) {
                    Uniform_State<3>(*programObject, location + i, value + i * 9 + row * 3, row * 4 * sizeof(float));
                }
            }
        }
    }

    void ProgramUniformMatrix4fv_State(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                       const GLfloat* value) {
        if (location == -1) return;

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }

        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(__func__, location + i, "program " + std::to_string(program));
                return;
            }
            if (transpose == GL_TRUE) {
                GLfloat transposedMatrix[16];
                TransposeMatrix4x4(value + i * 16, transposedMatrix);
                Uniform_State<16>(*programObject, location + i, transposedMatrix);
            } else {
                Uniform_State<16>(*programObject, location + i, value + i * 16);
            }
        }
    }

    void ProgramUniformMatrixNonSquarefv_State(const char* caller, GLuint program, GLint location, GLsizei count) {
        if (location == -1) return;

        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;

        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "program " + std::to_string(program) + " is not linked."));
            return;
        }

        for (GLint i = 0; i < count; i++) {
            if (!programObject->IsValidUniformLocation(location + i)) {
                RecordInvalidUniformLocationError(caller, location + i, "program " + std::to_string(program));
                return;
            }
            if (programObject->IsUniformOpaqueAtLocation(location + i)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Opaque uniforms cannot be set with matrix Uniform calls."));
                return;
            }
        }

        // TODO: Implement non-square matrix uniform uploads for non-opaque uniforms.
    }

    GLuint GetUniformBlockIndex_State(GLuint program, const GLchar* uniformBlockName) {
        const auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return GL_INVALID_INDEX;
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) +
                                                 " is not a program object that has been linked."));
            return GL_INVALID_INDEX;
        }

        const auto& index = programObject->GetUniformBlockIndex(uniformBlockName);
        return index;
    }

    void UniformBlockBinding_State(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
        const auto& programObject = TryToGetProgramObject(program);
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Program object" + std::to_string(program) + " that has been linked."));
            return;
        }
        if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                        " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                        "not the index of an active uniform block in program" +
                        std::to_string(program) + "."));
            return;
        }
        programObject->SetUniformBlockBinding(uniformBlockIndex, uniformBlockBinding);
    }

    void GetActiveUniformBlockiv_State(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint* params) {
        const auto& programObject = TryToGetProgramObject(program);
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Program object" + std::to_string(program) + " that has been linked."));
            return;
        }
        if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                        " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                        "not the index of an active uniform block in program" +
                        std::to_string(program) + "."));
            return;
        }
        switch (pname) {
        case GL_UNIFORM_BLOCK_DATA_SIZE: {
            *params = (GLint)programObject->GetUBOSizeAt(uniformBlockIndex);
            MGLOG_D("%s: GL_UNIFORM_BLOCK_DATA_SIZE = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_NAME_LENGTH: {
            *params = (GLint)programObject->GetUniformBlockName(uniformBlockIndex).length() + 1;
            MGLOG_D("%s: GL_UNIFORM_BLOCK_NAME_LENGTH = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
            *params = programObject->GetUniformBlockActiveUniformCount(uniformBlockIndex);
            MGLOG_D("%s: GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_BINDING: {
            *params = static_cast<GLint>(programObject->GetUniformBlockBinding(uniformBlockIndex));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_BINDING = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangVertex));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER:
            *params =
                BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangTessControl));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER:
            *params =
                BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangTessEvaluation));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangGeometry));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangFragment));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_COMPUTE_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(uniformBlockIndex, EShLangCompute));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_COMPUTE_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
            GLint uniformIndexCount = 0;
            for (Uint uniformIndex = 0; uniformIndex < programObject->GetUniformCount(); ++uniformIndex) {
                if (programObject->GetActiveUniformBlockIndex(uniformIndex) != static_cast<Int>(uniformBlockIndex)) {
                    continue;
                }
                params[uniformIndexCount++] = static_cast<GLint>(uniformIndex);
            }
            MGLOG_D("%s: GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES count = %d", __func__, uniformIndexCount);
            break;
        }
        default:
            MGLOG_E("%s: unknown pname = %p %s", __func__, pname, MG_Util::ConvertGLEnumToString(pname).c_str());
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname " + std::to_string(pname) + " is not one of the accepted tokens."));
            break;
        }
    }

    void GetActiveUniformBlockName_State(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length,
                                         GLchar* uniformBlockName) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) +
                                                 " is not a program object that has been linked."));
            return;
        }
        if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                        " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                        "not the index of an active uniform block in program."));
            return;
        }
        const auto& name = programObject->GetUniformBlockName(uniformBlockIndex);
        CopyStr(bufSize, length, uniformBlockName, name.c_str(), (GLsizei)name.length());
        MGLOG_D("%s: \"%s\" at uniformBlockIndex %02d, length = %d", __func__, uniformBlockName, uniformBlockIndex,
                length ? *length : 0);
    }

    void BindFragDataLocation_State(GLuint program, GLuint colorNumber, const char* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " is not the name of a program object."));
            return;
        }
        if (name == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "name cannot be null."));
            return;
        }
        const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
        if (colorNumber >= static_cast<GLuint>(dynamicParameters.MaxDrawBuffers)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "colorNumber is greater than or equal to GL_MAX_DRAW_BUFFERS."));
            return;
        }
        if (strncmp(name, "gl_", 3) == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "name " + std::string(name) + " starts with the reserved prefix `gl_`."));
            return;
        }

        MGLOG_D("%s: loc %02d = \"%s\"", __func__, colorNumber, name);
        programObject->SetExplicitFragmentOutLocation(colorNumber, name);
    }

    GLint GetFragDataLocation_State(GLuint program, const char* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " is not the name of a program object."));
            return -1;
        }
        if (name == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "name cannot be null."));
            return -1;
        }
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " has not been linked successfully."));
            return -1;
        }
        return programObject->GetFragmentDataLocation(name);
    }

    void ValidateProgram_State(GLuint program) {
        //            THROW_UNIMPL_EXCEPTION;
    }

    void AttachShader(GLuint program, GLuint shader) {
        AttachShader_State(program, shader);
    }

    void BindAttribLocation(GLuint program, GLuint index, const GLchar* name) {
        BindAttribLocation_State(program, index, name);
    }

    void CompileShader(GLuint shader) {
        CompileShader_State(shader);
    }
    GLuint CreateProgram(void) {
        return CreateProgram_State();
    }

    GLuint CreateShader(GLenum type) {
        return CreateShader_State(type);
    }

    void DeleteProgram(GLuint program) {
        DeleteProgram_State(program);
    }

    void DeleteShader(GLuint shader) {
        DeleteShader_State(shader);
    }

    void DetachShader(GLuint program, GLuint shader) {
        DetachShader_State(program, shader);
    }

    void GetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type,
                         GLchar* name) {
        GetActiveAttrib_State(program, index, bufSize, length, size, type, name);
    }

    void GetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type,
                          GLchar* name) {
        GetActiveUniform_State(program, index, bufSize, length, size, type, name);
    }

    void GetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length,
                              GLchar* uniformName) {
        GetActiveUniform_State(program, uniformIndex, bufSize, length, nullptr, nullptr, uniformName);
    }

    void GetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar* const* uniformNames,
                           GLuint* uniformIndices) {
        GetUniformIndices_State(program, uniformCount, uniformNames, uniformIndices);
    }

    void GetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders) {
        GetAttachedShaders_State(program, maxCount, count, shaders);
    }

    GLint GetAttribLocation(GLuint program, const GLchar* name) {
        return GetAttribLocation_State(program, name);
    }

    void GetProgramiv(GLuint program, GLenum pname, GLint* params) {
        GetProgramiv_State(program, pname, params);
    }

    void GetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
        GetProgramInfoLog_State(program, bufSize, length, infoLog);
    }

    void GetShaderiv(GLuint shader, GLenum pname, GLint* params) {
        GetShaderiv_State(shader, pname, params);
    }

    void GetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
        GetShaderInfoLog_State(shader, bufSize, length, infoLog);
    }

    void GetShaderSource(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
        GetShaderSource_State(shader, bufSize, length, source);
    }

    GLint GetUniformLocation(GLuint program, const GLchar* name) {
        return GetUniformLocation_State(program, name);
    }

    void GetUniformfv(GLuint program, GLint location, GLfloat* params) {
        GetUniformfv_State(program, location, params);
    }

    void GetUniformiv(GLuint program, GLint location, GLint* params) {
        GetUniformiv_State(program, location, params);
    }

    void GetUniformuiv(GLuint program, GLint location, GLuint* params) {
        GetUniformuiv_State(program, location, params);
    }

    GLboolean IsProgram(GLuint program) {
        return IsProgram_State(program);
    }
    GLboolean IsShader(GLuint shader) {
        return IsShader_State(shader);
    }

    void LinkProgram(GLuint program) {
        LinkProgram_State(program);
    }

    void ShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
        ShaderSource_State(shader, count, string, length);
    }

    void UseProgram(GLuint program) {
        UseProgram_State(program);
    }

    void Uniform1f(GLint location, GLfloat v0) {
        Uniform1fv(location, 1, &v0);
    }

    void Uniform2f(GLint location, GLfloat v0, GLfloat v1) {
        GLfloat v[] = {v0, v1};
        Uniform2fv(location, 1, v);
    }

    void Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
        GLfloat v[] = {v0, v1, v2};
        Uniform3fv(location, 1, v);
    }

    void Uniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
        GLfloat v[] = {v0, v1, v2, v3};
        Uniform4fv(location, 1, v);
    }

    void Uniform1i(GLint location, GLint v0) {
        Uniform1iv(location, 1, &v0);
    }

    void Uniform2i(GLint location, GLint v0, GLint v1) {
        GLint v[] = {v0, v1};
        Uniform2iv(location, 1, v);
    }

    void Uniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
        GLint v[] = {v0, v1, v2};
        Uniform3iv(location, 1, v);
    }

    void Uniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
        GLint v[] = {v0, v1, v2, v3};
        Uniform4iv(location, 1, v);
    }

    void Uniform1ui(GLint location, GLuint v0) {
        Uniform1uiv(location, 1, &v0);
    }

    void Uniform2ui(GLint location, GLuint v0, GLuint v1) {
        GLuint v[] = {v0, v1};
        Uniform2uiv(location, 1, v);
    }

    void Uniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2) {
        GLuint v[] = {v0, v1, v2};
        Uniform3uiv(location, 1, v);
    }

    void Uniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
        GLuint v[] = {v0, v1, v2, v3};
        Uniform4uiv(location, 1, v);
    }
    void Uniform1fv(GLint location, GLsizei count, const GLfloat* value) {
        Uniform1fv_State(location, count, value);
    }

    void Uniform2fv(GLint location, GLsizei count, const GLfloat* value) {
        Uniform2fv_State(location, count, value);
    }

    void Uniform3fv(GLint location, GLsizei count, const GLfloat* value) {
        Uniform3fv_State(location, count, value);
    }

    void Uniform4fv(GLint location, GLsizei count, const GLfloat* value) {
        Uniform4fv_State(location, count, value);
    }

    void Uniform1iv(GLint location, GLsizei count, const GLint* value) {
        Uniform1iv_State(location, count, value);
    }

    void Uniform2iv(GLint location, GLsizei count, const GLint* value) {
        Uniform2iv_State(location, count, value);
    }

    void Uniform3iv(GLint location, GLsizei count, const GLint* value) {
        Uniform3iv_State(location, count, value);
    }

    void Uniform4iv(GLint location, GLsizei count, const GLint* value) {
        Uniform4iv_State(location, count, value);
    }

    void Uniform1uiv(GLint location, GLsizei count, const GLuint* value) {
        Uniformv_State<1>(location, count, value);
    }

    void Uniform2uiv(GLint location, GLsizei count, const GLuint* value) {
        Uniformv_State<2>(location, count, value);
    }

    void Uniform3uiv(GLint location, GLsizei count, const GLuint* value) {
        Uniformv_State<3>(location, count, value);
    }

    void Uniform4uiv(GLint location, GLsizei count, const GLuint* value) {
        Uniformv_State<4>(location, count, value);
    }

    void UniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrix2fv_State(location, count, transpose, value);
    }

    void UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrix3fv_State(location, count, transpose, value);
    }

    void UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrix4fv_State(location, count, transpose, value);
    }

    void UniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void UniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void UniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void UniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void UniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void UniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count);
    }

    void ProgramUniform1f(GLuint program, GLint location, GLfloat v0) {
        ProgramUniform1fv(program, location, 1, &v0);
    }

    void ProgramUniform2f(GLuint program, GLint location, GLfloat v0, GLfloat v1) {
        GLfloat v[] = {v0, v1};
        ProgramUniform2fv(program, location, 1, v);
    }

    void ProgramUniform3f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
        GLfloat v[] = {v0, v1, v2};
        ProgramUniform3fv(program, location, 1, v);
    }

    void ProgramUniform4f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
        GLfloat v[] = {v0, v1, v2, v3};
        ProgramUniform4fv(program, location, 1, v);
    }

    void ProgramUniform1i(GLuint program, GLint location, GLint v0) {
        ProgramUniform1iv(program, location, 1, &v0);
    }

    void ProgramUniform2i(GLuint program, GLint location, GLint v0, GLint v1) {
        GLint v[] = {v0, v1};
        ProgramUniform2iv(program, location, 1, v);
    }

    void ProgramUniform3i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2) {
        GLint v[] = {v0, v1, v2};
        ProgramUniform3iv(program, location, 1, v);
    }

    void ProgramUniform4i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
        GLint v[] = {v0, v1, v2, v3};
        ProgramUniform4iv(program, location, 1, v);
    }

    void ProgramUniform1ui(GLuint program, GLint location, GLuint v0) {
        ProgramUniform1uiv(program, location, 1, &v0);
    }

    void ProgramUniform2ui(GLuint program, GLint location, GLuint v0, GLuint v1) {
        GLuint v[] = {v0, v1};
        ProgramUniform2uiv(program, location, 1, v);
    }

    void ProgramUniform3ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2) {
        GLuint v[] = {v0, v1, v2};
        ProgramUniform3uiv(program, location, 1, v);
    }

    void ProgramUniform4ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
        GLuint v[] = {v0, v1, v2, v3};
        ProgramUniform4uiv(program, location, 1, v);
    }

    void ProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
        ProgramUniformv_State<1>(program, location, count, value);
    }

    void ProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
        ProgramUniformv_State<2>(program, location, count, value);
    }

    void ProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
        ProgramUniformv_State<3>(program, location, count, value);
    }

    void ProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat* value) {
        ProgramUniformv_State<4>(program, location, count, value);
    }

    void ProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
        ProgramUniformv_State<1>(program, location, count, value);
    }

    void ProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
        ProgramUniformv_State<2>(program, location, count, value);
    }

    void ProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
        ProgramUniformv_State<3>(program, location, count, value);
    }

    void ProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint* value) {
        ProgramUniformv_State<4>(program, location, count, value);
    }

    void ProgramUniform1uiv(GLuint program, GLint location, GLsizei count, const GLuint* value) {
        ProgramUniformv_State<1>(program, location, count, value);
    }

    void ProgramUniform2uiv(GLuint program, GLint location, GLsizei count, const GLuint* value) {
        ProgramUniformv_State<2>(program, location, count, value);
    }

    void ProgramUniform3uiv(GLuint program, GLint location, GLsizei count, const GLuint* value) {
        ProgramUniformv_State<3>(program, location, count, value);
    }

    void ProgramUniform4uiv(GLuint program, GLint location, GLsizei count, const GLuint* value) {
        ProgramUniformv_State<4>(program, location, count, value);
    }

    void ProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                 const GLfloat* value) {
        ProgramUniformMatrix2fv_State(program, location, count, transpose, value);
    }

    void ProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                 const GLfloat* value) {
        ProgramUniformMatrix3fv_State(program, location, count, transpose, value);
    }

    void ProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                 const GLfloat* value) {
        ProgramUniformMatrix4fv_State(program, location, count, transpose, value);
    }

    void ProgramUniformMatrix2x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    void ProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    void ProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    void ProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    void ProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    void ProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count);
    }

    GLuint GetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName) {
        return GetUniformBlockIndex_State(program, uniformBlockName);
    }

    void UniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
        UniformBlockBinding_State(program, uniformBlockIndex, uniformBlockBinding);
    }

    void GetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint* params) {
        GetActiveUniformBlockiv_State(program, uniformBlockIndex, pname, params);
    }

    void GetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length,
                                   GLchar* uniformBlockName) {
        GetActiveUniformBlockName_State(program, uniformBlockIndex, bufSize, length, uniformBlockName);
    }

    void BindFragDataLocation(GLuint program, GLuint colorNumber, const char* name) {
        BindFragDataLocation_State(program, colorNumber, name);
    }

    GLint GetFragDataLocation(GLuint program, const char* name) {
        return GetFragDataLocation_State(program, name);
    }

    void GetProgramInterfaceiv(GLuint program, GLenum programInterface, GLenum pname, GLint* params) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (!ValidateProgramInterfaceivQuery(programInterface, pname)) return;
        auto getProgramInterfaceiv = MG_Backend::gBackendFunctionsTable.GL.GetProgramInterfaceiv;
        if (!getProgramInterfaceiv) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support program interface queries."));
            return;
        }
        getProgramInterfaceiv(program, programInterface, pname, params);
    }

    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return GL_INVALID_INDEX;
        if (!ValidateNamedProgramResourceInterface(programInterface, __func__)) return GL_INVALID_INDEX;
        if (!name) return GL_INVALID_INDEX;
        auto getProgramResourceIndex = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceIndex;
        if (!getProgramResourceIndex) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support program interface queries."));
            return GL_INVALID_INDEX;
        }
        GLuint index = getProgramResourceIndex(program, programInterface, name);
        const String resourceName = name;
        if (index == GL_INVALID_INDEX && resourceName.length() > 3 &&
            resourceName.compare(resourceName.length() - 3, 3, "[0]") == 0) {
            index = getProgramResourceIndex(program, programInterface,
                                            resourceName.substr(0, resourceName.length() - 3).c_str());
        }
        return index;
    }

    void GetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length,
                                GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (!ValidateNamedProgramResourceInterface(programInterface, __func__)) return;
        const Int resourceCount = GetKnownProgramResourceCount(programObject, programInterface);
        if (resourceCount >= 0 && index >= static_cast<GLuint>(resourceCount)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "index is out of range."));
            return;
        }
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "bufSize must be non-negative."));
            return;
        }
        auto getProgramResourceName = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceName;
        if (!getProgramResourceName) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support program interface queries."));
            return;
        }
        getProgramResourceName(program, programInterface, index, bufSize, length, name);
    }

    void GetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                              const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (propCount < 0 || bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                      "propCount and bufSize must be non-negative."));
            return;
        }
        auto getProgramResourceiv = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceiv;
        if (!getProgramResourceiv) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support program interface queries."));
            return;
        }
        getProgramResourceiv(program, programInterface, index, propCount, props, bufSize, length, params);
    }

    GLint GetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return -1;
        auto getProgramResourceLocation = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceLocation;
        if (!getProgramResourceLocation) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support program interface queries."));
            return -1;
        }
        return getProgramResourceLocation(program, programInterface, name);
    }

    GLint GetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return -1;
        auto getProgramResourceLocationIndex = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceLocationIndex;
        if (!getProgramResourceLocationIndex) return -1;
        return getProgramResourceLocationIndex(program, programInterface, name);
    }

    void ShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) return;
        if (!ValidateShaderStorageBlockBinding(storageBlockBinding)) return;
        auto shaderStorageBlockBinding = MG_Backend::gBackendFunctionsTable.GL.ShaderStorageBlockBinding;
        if (!shaderStorageBlockBinding) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support shader storage block binding."));
            return;
        }
        shaderStorageBlockBinding(program, storageBlockIndex, storageBlockBinding);
    }

    void ValidateProgram(GLuint program) {
        ValidateProgram_State(program);
    }
} // namespace MobileGL::MG_Impl::GLImpl
