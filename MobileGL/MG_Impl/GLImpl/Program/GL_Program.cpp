// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Program.h"
#include "Config.h"
#include "MG_Util/Converters/GLToStr/GLEnumConverter.h"
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/GLToMG/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>

namespace MobileGL {
    namespace MG_Impl::GLImpl {
        static bool CheckShaderNameValidity(Uint shader) {
            if (shader == 0 || !MG_State::pGLContext->ValidateShaderName(shader)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(shader) + " is not a valid name."));
                return false;
            }
            return true;
        }

        static SharedPtr<MG_State::GLState::ShaderObject> TryToGetShaderObject(Uint shader) {
            if (!CheckShaderNameValidity(shader)) return nullptr;

            auto shaderObject = MG_State::pGLContext->GetShaderObject(shader);
            if (!shaderObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(shader) + " is not a shader object."));
                return nullptr;
            }
            return shaderObject;
        }

        static bool CheckProgramNameValidity(GLuint program) {
            if (!MG_State::pGLContext->ValidateProgramName(program)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) + " is not a valid name."));
                return false;
            }
            return true;
        }

        static SharedPtr<MG_State::GLState::ProgramObject> TryToGetProgramObject(GLuint program) {
            if (!CheckProgramNameValidity(program)) return nullptr;

            auto programObject = MG_State::pGLContext->GetProgramObject(program);
            if (!programObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) + " is not a program object."));
                return nullptr;
            }
            return programObject;
        }

        void CopyStr(GLsizei bufSize, GLsizei* length, GLchar* dst, const char* src, GLsizei srcLength) {
            auto sz = std::min(bufSize - 1, srcLength);
            if (length) *length = sz;

            if (bufSize == 0) return;

            Memcpy(dst, src, sz);
            dst[sz] = '\0';
        }

        void AttachShader_State(GLuint program, GLuint shader) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;
            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;
            if (!programObject->AttachShader(shaderObject)) {
                MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                                  MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
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
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "index " + std::to_string(index) +
                                                     " is greater than or equal to `GL_MAX_VERTEX_ATTRIBS`."));
                return;
            }

            if (strncmp(name, "gl_", 3) == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "name " + std::string(name) +
                                                     " starts with the reserved prefix `gl_`."));
                return;
            }

            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;

            MGLOG_D("%s: loc %02d = \"%s\"", __func__, index, name);
            programObject->SetExplicitVertexInLocation(index, name);
        }

        void CompileShader_State(GLuint shader) {
            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;
            shaderObject->Compile();
        }

        GLuint CreateProgram_State(void) {
            return MG_State::pGLContext->CreateProgram();
        }

        GLuint CreateShader_State(GLenum type) {
            auto shaderId = MG_State::pGLContext->CreateShader(MG_Util::ConvertGLEnumToShaderStage(type));
            if (shaderId == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`shaderType` is not an accepted value."));
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
            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;

            auto count = programObject->DetachShader(shaderObject);
            if (count <= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Shader is not attached to program."));
                return;
            }
        }

        void GetActiveAttrib_State(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                                   GLenum* type, GLchar* name) {
            if (bufSize < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "bufSize " + std::to_string(bufSize) + " is less than 0."));
                return;
            }
            auto programObject = TryToGetProgramObject(program);
            if (!programObject || !programObject->GetLinkStatus()) return;
            auto attribCount = programObject->GetActiveAttributesCount();
            if (index >= attribCount) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        "index " + std::to_string(index) +
                            " is greater than or equal to the number of active attribute variables in " +
                            std::to_string(program) + "."));
                return;
            }
            if (type != nullptr) *type = programObject->GetAttribType(index);
            if (bufSize == 0) return;
            auto& attribName = programObject->GetAttribName(index);
            CopyStr(bufSize, length, name, attribName.c_str(), attribName.length());
        }

        void GetActiveUniform_State(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size,
                                    GLenum* type, GLchar* name) {
            if (bufSize < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "bufSize " + std::to_string(bufSize) + " is less than 0."));
                return;
            }
            auto programObject = TryToGetProgramObject(program);
            if (!programObject || !programObject->GetLinkStatus()) return;
            auto uniformCount = programObject->GetUniformCount();
            if (index >= uniformCount) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        "index " + std::to_string(index) +
                            " is greater than or equal to the number of active uniform variables in " +
                            std::to_string(program) + "."));
                return;
            }

            if (type != nullptr) *type = programObject->GetUniformType(index);
            if (bufSize == 0) return;
            auto& uniformName = programObject->GetUniformName(index);
            CopyStr(bufSize, length, name, uniformName.c_str(), uniformName.length());
        }

        void GetAttachedShaders_State(GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders) {
            if (maxCount < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "maxCount " + std::to_string(maxCount) + " is less than 0."));
                return;
            }
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;
            const auto& s = programObject->GetAttachedShaders();
            GLsizei c = std::min((GLsizei)s.size(), maxCount);
            if (count) *count = c;
            for (GLsizei i = 0; i < c; ++i) {
                shaders[i] = s[i]->GetExternalIndex();
            }
        }

        GLint GetAttribLocation_State(GLuint program, const GLchar* name) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return -1;
            if (strncmp(name, "gl_", 3) == 0) return -1;
            if (!programObject->GetLinkStatus()) return -1;
            return programObject->GetAttributeLocation(name);
        }

        void GetProgramiv_State(GLuint program, GLenum pname, GLint* params) {
            auto programObject = TryToGetProgramObject(program);
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
                *params = log.length();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            }
            case GL_ATTACHED_SHADERS: {
                const auto& attachedShaders = programObject->GetAttachedShaders();
                *params = attachedShaders.size();
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
                *params = programObject->GetActiveAttributesMaxLength();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            case GL_ACTIVE_UNIFORMS:
                *params = programObject->GetUniformCount();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            case GL_ACTIVE_UNIFORM_MAX_LENGTH:
                *params = programObject->GetUniformMaxLength();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            case GL_ACTIVE_UNIFORM_BLOCKS: // GL >= 3.1
                *params = programObject->GetActiveUniformBlocksCount();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH: // ditto.
                *params = programObject->GetActiveUniformBlocksMaxNameLength();
                MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
                break;
            case GL_COMPUTE_WORK_GROUP_SIZE: // GL >= 4.3

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
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "pname " + std::to_string(pname) + " is not an accepted value."));
                return;
            }
        }

        void GetProgramInfoLog_State(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;

            const auto& log = programObject->GetInfoLog();
            CopyStr(bufSize, length, infoLog, log.c_str(), log.length());
        }

        void GetShaderiv_State(GLuint shader, GLenum pname, GLint* params) {
            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;

            switch (pname) {
            case GL_SHADER_TYPE:
                *params = MG_Util::ConvertShaderStageToGLEnum(shaderObject->GetShaderStage());
                break;
            case GL_DELETE_STATUS:
                *params = shaderObject->GetDeleteStatus();
                break;
            case GL_COMPILE_STATUS:
                *params = shaderObject->GetCompileStatus();
                break;
            case GL_INFO_LOG_LENGTH:
                *params = shaderObject->GetInfoLog().length();
                break;
            case GL_SHADER_SOURCE_LENGTH:
                *params = shaderObject->GetShaderSource().length();
                break;
            default:
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "pname " + std::to_string(pname) + " is not an accepted value."));
                return;
            }
        }

        void GetShaderInfoLog_State(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog) {
            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;

            const auto& log = shaderObject->GetInfoLog();
            CopyStr(bufSize, length, infoLog, log.c_str(), log.length());
        }

        void GetShaderSource_State(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* source) {
            if (bufSize < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "bufSize " + std::to_string(bufSize) + " is less than 0."));
            }

            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;

            auto& src = shaderObject->GetShaderSource();
            CopyStr(bufSize, length, source, src.c_str(), src.length());
        }

        GLint GetUniformLocation_State(GLuint program, const GLchar* name) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return -1;
            auto loc = programObject->GetUniformLocation(name);
            MGLOG_D("%s: loc %02d = %s", __func__, loc, name);
            return loc;
        }

        void GetUniform_State(GLuint program, GLint location, void* params) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;

            if (!programObject->GetLinkStatus()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) + " has not been successfully linked."));
                return;
            }

            // Check if location is valid
            if (location < 0 || location > programObject->GetMaxUniformLocation()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " does not correspond to a valid uniform variable location "
                                                     "for the specified program object."));
                return;
            }

            // Check if the location corresponds to an active uniform
            const auto& uniformName = programObject->GetUniformName(location);
            if (uniformName.empty()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " does not correspond to a valid uniform variable location "
                                                     "for the specified program object."));
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
                        Memcpy((char*)params + ttype->getMatrixCols() * sizeof(float) * i,
                               pBase + 4 * sizeof(float) * i, ttype->getMatrixCols() * sizeof(float));
                    }
                }
            }
            // TODO: handle 1i variant as texture unit
        }

        void GetUniformfv_State(GLuint program, GLint location, GLfloat* params) {
            GetUniform_State(program, location, params);
        }

        void GetUniformiv_State(GLuint program, GLint location, GLint* params) {
            GetUniform_State(program, location, params);
        }

        GLboolean IsProgram_State(GLuint program) {
            /* FIXME: Handle situations that:
             * A program object marked for deletion with glDeleteProgram but still in use as part of current
             * rendering state is still considered a program object and glIsProgram will return GL_TRUE.
             */
            return CheckProgramNameValidity(program);
        }

        GLboolean IsShader_State(GLuint shader) {
            /* FIXME: Handle situations that:
             * A shader object marked for deletion with glDeleteShader but still attached to a program object is still
             * considered a shader object and glIsShader will return GL_TRUE.
             */
            return CheckShaderNameValidity(shader);
        }

        void LinkProgram_State(GLuint program) {
            auto programObject = TryToGetProgramObject(program);
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
            programObject->Link(!allowVSOnlyPrograms);
        }

        void ShaderSource_State(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
            if (count < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "count " + std::to_string(count) + " is less than 0."));
                return;
            }

            auto shaderObject = TryToGetShaderObject(shader);
            if (!shaderObject) return;

            std::string src;
            for (GLsizei i = 0; i < count; i++) {
                src += (length == nullptr || length[i] <= 0) ? string[i] : std::string(string[i], length[i]);
            }
            shaderObject->SetShaderSource(Move(src));
        }

        void UseProgram_State(GLint program) {
            MGLOG_D("UseProgram_State: program=%u", program);

            if (program == 0) {
                MG_State::pGLContext->UseProgram(0);
                return;
            }

            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;
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
                                "Uniform size mismatch, expected at least %zu bytes, got %zu bytes.",
                                ItemCount * sizeof(T), size);
                MGLOG_D("%s: program = %d, location = %d, byteOffset = %d", __func__, programObject.GetExternalIndex(),
                        location, offset + byteOffsetInsideUniform);
                Memcpy((char*)programObject.MapUBO() + offset + byteOffsetInsideUniform, value, ItemCount * sizeof(T));
            } else {
                auto* ttype = programObject.GetUniformTType(location);
                if (ttype->isTexture() || ttype->isImage()) {
                    programObject.SetUniformSamplerOrImageUnitIndex(location, *value);
                }
            }
        }

        template <GLsizei ItemCount, typename T>
        void Uniformv_State(GLint location, GLsizei count, T* value) {
            if (location == -1) return;

            auto programObject = MG_State::pGLContext->GetCurrentProgram();
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
                return;
            }

            if (location > programObject->GetMaxUniformLocation() || location < -1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " is an invalid uniform location for the current program "
                                                     "object and location " +
                                                     std::to_string(location) + " is not equal to -1."));
                return;
            }

            for (GLint offset = 0; offset < count; offset++) {
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

        void UniformMatrix2fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
            // For 2x2 matrices, we have 4 elements per matrix
            // If transpose is GL_TRUE, we need to transpose the matrix data
            if (location == -1) return;

            auto programObject = MG_State::pGLContext->GetCurrentProgram();
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
                return;
            }

            if (location > programObject->GetMaxUniformLocation() || location < -1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " is an invalid uniform location for the current program "
                                                     "object and location " +
                                                     std::to_string(location) + " is not equal to -1."));
                return;
            }

            // For matrix uniforms, we handle each matrix individually
            for (GLint i = 0; i < count; i++) {
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

            auto programObject = MG_State::pGLContext->GetCurrentProgram();
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
                return;
            }

            if (location > programObject->GetMaxUniformLocation() || location < -1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " is an invalid uniform location for the current program "
                                                     "object and location " +
                                                     std::to_string(location) + " is not equal to -1."));
                return;
            }

            // For matrix uniforms, we handle each matrix individually
            // Handle padding in mat3 correctly!!
            for (GLint i = 0; i < count; i++) {
                if (transpose == GL_TRUE) {
                    // Transpose the matrix before uploading
                    GLfloat transposedMatrix[9];
                    TransposeMatrix3x3(value + i * 9, transposedMatrix);
                    for (int row = 0; row < 3; ++row) {
                        Uniform_State<3>(*programObject, location + i, transposedMatrix + row * 3,
                                         row * 4 * sizeof(float));
                    }
                } else {
                    // No transpose needed, directly copy the matrix data
                    for (int row = 0; row < 3; ++row) {
                        Uniform_State<3>(*programObject, location + i, value + i * 9 + row * 3,
                                         row * 4 * sizeof(float));
                    }
                }
            }
        }

        void UniformMatrix4fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
            // For 4x4 matrices, we have 16 elements per matrix
            // If transpose is GL_TRUE, we need to transpose the matrix data
            if (location == -1) return;

            auto programObject = MG_State::pGLContext->GetCurrentProgram();
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
                return;
            }

            if (location > programObject->GetMaxUniformLocation() || location < -1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "location " + std::to_string(location) +
                                                     " is an invalid uniform location for the current program "
                                                     "object and location " +
                                                     std::to_string(location) + " is not equal to -1."));
                return;
            }

            // For matrix uniforms, we handle each matrix individually
            for (GLint i = 0; i < count; i++) {
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

        GLuint GetUniformBlockIndex_State(GLuint program, const GLchar* uniformBlockName) {
            const auto& programObject = TryToGetProgramObject(program);
            if (!programObject) return GL_INVALID_INDEX;
            if (!programObject->GetLinkStatus()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
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
                MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                                  MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                               "Program object" +
                                                                                   std::to_string(program) +
                                                                                   " that has been linked."));
                return;
            }
            if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>(
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
                MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                                  MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                               "Program object" +
                                                                                   std::to_string(program) +
                                                                                   " that has been linked."));
                return;
            }
            if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                            " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                            "not the index of an active uniform block in program" +
                            std::to_string(program) + "."));
                return;
            }
            switch (pname) {
            case GL_UNIFORM_BLOCK_DATA_SIZE: {
                *params = programObject->GetUBOSizeAt(uniformBlockIndex);
                MGLOG_D("%s: GL_UNIFORM_BLOCK_DATA_SIZE = %d", __func__, *params);
                break;
            }
            case GL_UNIFORM_BLOCK_NAME_LENGTH: {
                *params = programObject->GetUniformBlockName(uniformBlockIndex).length() + 1;
                MGLOG_D("%s: GL_UNIFORM_BLOCK_NAME_LENGTH = %d", __func__, *params);
                break;
            }
            case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
                // TODO: deduct global ubo?
                *params = programObject->GetActiveUniformBlocksCount();
                MGLOG_D("%s: GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS = %d", __func__, *params);
                break;
            }
            case GL_UNIFORM_BLOCK_BINDING: {
                // TODO
                MGLOG_D("%s: GL_UNIFORM_BLOCK_BINDING = <TODO>", __func__, *params);
            }
            case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
            case GL_UNIFORM_BLOCK_REFERENCED_BY_COMPUTE_SHADER:
            default:
                MGLOG_E("%s: unknown pname = %p %s", __func__, pname, MG_Util::ConvertGLEnumToString(pname).c_str());
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                         "pname " + std::to_string(pname) +
                                                                             " is not one of the accepted tokens."));
                break;
            }
        }

        void GetActiveUniformBlockName_State(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei* length,
                                             GLchar* uniformBlockName) {
            auto programObject = TryToGetProgramObject(program);
            if (!programObject) return;
            if (!programObject->GetLinkStatus()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) +
                                                     " is not a program object that has been linked."));
                return;
            }
            if (!programObject->IsActiveUniformBlock(uniformBlockIndex)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                            " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                            "not the index of an active uniform block in program."));
                return;
            }
            const auto& name = programObject->GetUniformBlockName(uniformBlockIndex);
            CopyStr(bufSize, length, uniformBlockName, name.c_str(), name.length());
            MGLOG_D("%s: \"%s\" at uniformBlockIndex %02d, length = %d", __func__, uniformBlockName, uniformBlockIndex,
                    *length);
        }

        void BindFragDataLocation_State(GLuint program, GLuint colorNumber, const char* name) {
            auto programObject = TryToGetProgramObject(program);
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) + " is not the name of a program object."));
                return;
            }
            if (strncmp(name, "gl_", 3) == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "name " + std::string(name) +
                                                     " starts with the reserved prefix `gl_`."));
                return;
            }
            // TODO: Emit error "if `colorNumber` is greater than or equal to `GL_MAX_DRAW_BUFFERS`"

            MGLOG_D("%s: loc %02d = \"%s\"", __func__, colorNumber, name);
            programObject->SetExplicitFragmentOutLocation(colorNumber, name);
        }

        GLint GetFragDataLocation_State(GLuint program, const char* name) {
            auto programObject = TryToGetProgramObject(program);
            if (programObject == nullptr) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) + " is not the name of a program object."));
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

        void UniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
            UniformMatrix2fv_State(location, count, transpose, value);
        }

        void UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
            UniformMatrix3fv_State(location, count, transpose, value);
        }

        void UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
            UniformMatrix4fv_State(location, count, transpose, value);
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

        void ValidateProgram(GLuint program) {
            ValidateProgram_State(program);
        }
    } // namespace MG_Impl::GLImpl
} // namespace MobileGL
