// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Program.h"
#include "Config.h"
#include <cmath>
#include <limits>
#include <MG_Impl/GLImpl/VertexArray/Validators.h>
#include <MG_State/GLState/Core.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/ProgramEnumConverter.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/Converters/SPIRVCrossToGL/SpvcTypeConverter.h>
#include <MG_Util/Async/ShaderCompilePool.h>
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
            // Programs and shaders share one name space: a name that exists but
            // belongs to a shader is INVALID_OPERATION, a name GL never handed
            // out is INVALID_VALUE (GL 3.3 core 2.11.x).
            const ErrorCode error = MG_State::pGLContext->ValidateShaderName(program)
                ? ErrorCode::InvalidOperation
                : ErrorCode::InvalidValue;
            MG_State::pGLContext->RecordError(
                error,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) +
                                                 (error == ErrorCode::InvalidOperation ? " is not a program object."
                                                                                       : " is not a valid name.")));
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

    // The GL_UNIFORM interface and glGetActiveUniform(s)iv are the same query in two
    // spellings, so they answer from the same place - the frontend reflection. The backend
    // program is not that place: it does not exist at all for a program whose types its
    // shading language cannot express (a double-precision uniform has no ESSL form), and
    // the interface queries would then describe a program with no uniforms.
    //
    // Writes the GL_UNIFORM value of `prop` for active uniform `index`; false for a prop
    // the reflection does not model, which the caller forwards to the backend instead.
    Bool GetUniformResourceProp(const SharedPtr<MG_State::GLState::ProgramObject>& programObject, Uint index,
                                GLenum prop, GLint* out) {
        switch (prop) {
        case GL_TYPE:
            *out = static_cast<GLint>(programObject->GetActiveUniformType(index));
            return true;
        case GL_ARRAY_SIZE:
            *out = programObject->GetActiveUniformArraySize(index);
            return true;
        case GL_NAME_LENGTH:
            *out = static_cast<GLint>(programObject->GetActiveUniformName(index).length() + 1);
            return true;
        case GL_BLOCK_INDEX:
            *out = programObject->GetActiveUniformBlockIndex(index);
            return true;
        case GL_OFFSET:
            *out = programObject->GetActiveUniformOffset(index);
            return true;
        case GL_ARRAY_STRIDE:
            *out = programObject->GetActiveUniformArrayStride(index);
            return true;
        case GL_MATRIX_STRIDE:
            *out = programObject->GetActiveUniformMatrixStride(index);
            return true;
        case GL_IS_ROW_MAJOR:
            *out = programObject->GetActiveUniformIsRowMajor(index);
            return true;
        case GL_LOCATION:
            // A block member has no location; GetUniformLocation already reports -1 for one.
            *out = programObject->GetUniformLocation(programObject->GetActiveUniformName(index));
            return true;
        default:
            return false;
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
        if (index >= VertexArrayImpl::GetMaxVertexAttribs()) {
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

    // glMaxShaderCompilerThreadsKHR / glMaxShaderCompilerThreadsARB - one implementation,
    // because GL_KHR_parallel_shader_compile and GL_ARB_parallel_shader_compile define the
    // same entry point with the same semantics and GetProcAddress.cpp maps both spellings.
    //
    // The three cases the extension defines, and what each means here:
    //
    //   count == 0            "no compiler threads": compilation must happen on the
    //                         application's thread. Everything already in flight is joined
    //                         first, so that after this call returns NOTHING is outstanding
    //                         and every GL_COMPLETION_STATUS_KHR reads GL_TRUE - which is
    //                         the observable the extension actually specifies. The pool
    //                         keeps its worker threads (this is not teardown); what changes
    //                         is that AsyncShaderCompileActive() now says no, so
    //                         glCompileShader/glLinkProgram run their bodies inline.
    //   count == 0xFFFFFFFF   "implementation maximum": the pool's full thread count.
    //   otherwise             a concurrency budget, clamped to the thread count - asking for
    //                         more threads than exist cannot conjure any.
    //
    // A nonzero count is also what LIFTS a previous zero: the suspension lasts exactly until
    // the application asks for threads again, and nothing else re-arms it (no implicit
    // restore at eglInitialize, at a context switch or at a join). An application that turned
    // compiler threads off keeps them off until it says otherwise.
    //
    // Legal - and a no-op beyond bookkeeping - while MOBILEGL_ASYNC_SHADER_COMPILE is off:
    // compilation is already inline, and the call must not fail just because MobileGL had
    // nothing to suspend.
    void MaxShaderCompilerThreadsKHR_State(GLuint count) {
        namespace Async = MG_Util::Async;
        if (count == 0) {
            MGLOG_D("%s: count = 0; joining all pending shader work and compiling inline", __func__);
            Async::SetAsyncShaderCompileSuspended(true);
            // Suspend BEFORE joining, not after. The post-condition this call owes the
            // application is "nothing is in flight when I return", and only this order
            // guarantees it: with the latch already set, anything the join itself causes to
            // be compiled runs inline and is therefore already settled when the join ends.
            // Joining first would leave a window in which a fresh enqueue is still legal.
            if (MG_State::pGLContext) MG_State::pGLContext->JoinAllPendingShaderWork();
            return;
        }

        Async::ShaderCompilePool& pool = Async::ShaderCompilePool::Get();
        const Uint threadCount = pool.GetThreadCount();
        const Uint requested = count == 0xFFFFFFFFu ? threadCount : std::min<Uint>(count, threadCount);
        pool.SetMaxConcurrency(requested);
        Async::SetAsyncShaderCompileSuspended(false);
        MGLOG_D("%s: count = %u; concurrency = %u of %u threads", __func__, count, requested, threadCount);
    }

    GLuint CreateProgram_State() {
        return MG_State::pGLContext->CreateProgram();
    }

    GLuint CreateShader_State(GLenum type) {
        // GL 4.6 core 7.1: shaderType is an enum, so an unrecognised one is INVALID_ENUM (it
        // used to be documented as INVALID_VALUE). The check has to happen HERE: the state
        // layer hands out a name for ShaderStage::Unknown just as happily as for a real
        // stage, so the old "shaderId == 0 means bad type" test could never fire and an
        // unknown shaderType silently produced a usable shader name and no error at all.
        const ShaderStage stage = MG_Util::ConvertGLEnumToShaderStage(type);
        if (stage == ShaderStage::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`shaderType` is not an accepted value."));
            return 0;
        }
        return MG_State::pGLContext->CreateShader(stage);
    }

    void DeleteProgram_State(GLuint program) {
        // "If program is zero, it is silently ignored" (GL 4.6 core 7.3) - unlike every
        // other program entry point, where 0 is a name GL never handed out.
        if (program == 0) return;
        if (!CheckProgramNameValidity(program)) return;
        MG_State::pGLContext->MarkProgramForDeletion(program);
    }

    void DeleteShader_State(GLuint shader) {
        // Same silent-zero rule as glDeleteProgram (GL 4.6 core 7.1).
        if (shader == 0) return;
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
        // A shader flagged with glDeleteShader lives on while attached; this detach may
        // have been its last GL-visible attachment.
        MG_State::pGLContext->ReleaseShaderNameIfOrphaned(shader);
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

    void GetActiveUniformsiv_State(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname,
                                   GLint* params) {
        if (uniformCount < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "uniformCount " + std::to_string(uniformCount) + " is less than 0."));
            return;
        }

        // Program-name resolution with the correct two-error split: a live shader name is
        // GL_INVALID_OPERATION, a never-generated name is GL_INVALID_VALUE. glGetActiveUniformsiv has
        // no "not linked" error, so unlike TryToGetLinkedProgramForInterfaceQuery there is no
        // link-status check here; an unlinked program simply has zero active uniforms (handled below).
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            const ErrorCode error = MG_State::pGLContext->ValidateShaderName(program) ? ErrorCode::InvalidOperation
                                                                                      : ErrorCode::InvalidValue;
            MG_State::pGLContext->RecordError(
                error, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                    std::to_string(program) + " is not a program object."));
            return;
        }
        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) return;

        switch (pname) {
        case GL_UNIFORM_TYPE:
        case GL_UNIFORM_SIZE:
        case GL_UNIFORM_NAME_LENGTH:
        case GL_UNIFORM_BLOCK_INDEX:
        case GL_UNIFORM_OFFSET:
        case GL_UNIFORM_ARRAY_STRIDE:
        case GL_UNIFORM_MATRIX_STRIDE:
        case GL_UNIFORM_IS_ROW_MAJOR:
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname " + std::to_string(pname) + " is not an accepted value."));
            return;
        }

        if (uniformCount == 0) return;
        if (uniformIndices == nullptr || params == nullptr) return;

        // Every index must be < the number of active uniforms, checked before any write so params is
        // left untouched on error. GetUniformCount() is 0 for an unlinked program, which is also the
        // spec-mandated GL_INVALID_VALUE path for querying an unlinked program (no separate error).
        const Uint activeUniforms = programObject->GetUniformCount();
        for (GLsizei i = 0; i < uniformCount; ++i) {
            if (uniformIndices[i] >= activeUniforms) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "uniformIndices[" + std::to_string(i) +
                                                     "] = " + std::to_string(uniformIndices[i]) +
                                                     " is greater than or equal to the number of active uniforms."));
                return;
            }
        }

        for (GLsizei i = 0; i < uniformCount; ++i) {
            const Uint idx = uniformIndices[i];
            switch (pname) {
            case GL_UNIFORM_TYPE:
                params[i] = static_cast<GLint>(programObject->GetActiveUniformType(idx));
                break;
            case GL_UNIFORM_SIZE:
                params[i] = programObject->GetActiveUniformArraySize(idx);
                break;
            case GL_UNIFORM_NAME_LENGTH:
                params[i] = static_cast<GLint>(programObject->GetActiveUniformName(idx).length() + 1);
                break;
            case GL_UNIFORM_BLOCK_INDEX:
                params[i] = programObject->GetActiveUniformBlockIndex(idx);
                break;
            case GL_UNIFORM_OFFSET:
                params[i] = programObject->GetActiveUniformOffset(idx);
                break;
            case GL_UNIFORM_ARRAY_STRIDE:
                params[i] = programObject->GetActiveUniformArrayStride(idx);
                break;
            case GL_UNIFORM_MATRIX_STRIDE:
                params[i] = programObject->GetActiveUniformMatrixStride(idx);
                break;
            case GL_UNIFORM_IS_ROW_MAJOR:
                params[i] = programObject->GetActiveUniformIsRowMajor(idx);
                break;
            default:
                break;
            }
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
        case GL_TRANSFORM_FEEDBACK_VARYINGS:
            *params = static_cast<GLint>(programObject->GetTransformFeedbackVaryingCount());
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
            *params = static_cast<GLint>(programObject->GetTransformFeedbackBufferMode());
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
            *params = programObject->GetTransformFeedbackVaryingMaxLength();
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

        // GL_KHR_parallel_shader_compile. THIS CASE MUST NOT JOIN - it is the one program
        // query whose entire purpose is to answer without waiting, and routing it through
        // any of ProgramObject's Artifacts() accessors (the join gate, invariant I5) would
        // block the caller and make the extension a lie: an application polling it would
        // serialize itself on the very link it is trying to overlap. IsLinkComplete() is the
        // node-direct reader that exists for exactly this.
        //
        // No link at all reads GL_TRUE, which is what the extension requires: the query
        // means "is anything still outstanding", not "has this program ever been linked".
        case GL_COMPLETION_STATUS_KHR:
            *params = programObject->IsLinkComplete() ? GL_TRUE : GL_FALSE;
            break;

        case GL_PROGRAM_BINARY_LENGTH:
            // No program binary format is exposed, so a program never has a retrievable
            // binary and its length is zero (ARB_get_program_binary).
            *params = 0;
            break;
        case GL_PROGRAM_BINARY_RETRIEVABLE_HINT:
            *params = programObject->GetBinaryRetrievableHint() ? GL_TRUE : GL_FALSE;
            break;
        case GL_PROGRAM_SEPARABLE:
            *params = programObject->GetSeparable() ? GL_TRUE : GL_FALSE;
            break;

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
        // GL_KHR_parallel_shader_compile. THIS CASE MUST NOT JOIN - see the identical case in
        // GetProgramiv_State. GL_COMPILE_STATUS two cases up deliberately DOES join (it has
        // to: it reports the outcome); this one reports whether there is an outcome yet, and
        // reading it through Compiled() would defeat the whole extension.
        case GL_COMPLETION_STATUS_KHR:
            *params = shaderObject->IsCompileComplete() ? GL_TRUE : GL_FALSE;
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

    // A float matrix lives in the global UBO under std140 rules - one 16-byte-aligned column
    // vector per column - while the value glGetUniform* must return is tightly packed
    // columns * rows floats. Only mat4 is the same either way; every other shape needs the
    // padding undone, and the readback has to undo exactly what UniformMatrixfv_Object put
    // there. Returns false when `ttype` is not a float matrix (nothing to unpack).
    Bool TryGatherFloatMatrixColumns(const glslang::TType* ttype, const char* pBase, void* params) {
        if (ttype == nullptr || !ttype->isMatrix() || ttype->getBasicType() == glslang::EbtDouble) return false;
        const Int columns = ttype->getMatrixCols();
        const Int rows = ttype->getMatrixRows();
        for (Int column = 0; column < columns; ++column) {
            Memcpy(static_cast<char*>(params) + static_cast<SizeT>(column) * rows * sizeof(GLfloat),
                   pBase + static_cast<SizeT>(column) * 4 * sizeof(GLfloat), rows * sizeof(GLfloat));
        }
        return true;
    }

    // Bytes a uniform actually occupies in the global UBO. It is the tight GL type size for
    // everything except a float matrix, whose padded columns make it wider.
    SizeT UniformStorageSpanInBytes(const glslang::TType* ttype, SizeT tightSize) {
        if (ttype != nullptr && ttype->isMatrix() && ttype->getBasicType() != glslang::EbtDouble) {
            return static_cast<SizeT>(ttype->getMatrixCols()) * 4 * sizeof(GLfloat);
        }
        return tightSize;
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
            const SizeT span = UniformStorageSpanInBytes(ttype, size);
            if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + span > programObject->GetUBOSize()) {
                MGLOG_E("%s: uniform at program %u location %d has no backing storage; returning nothing", __func__,
                        program, location);
                return;
            }

            if (!TryGatherFloatMatrixColumns(ttype, pUBO + offset, params)) {
                Memcpy(params, pUBO + offset, size);
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
        const SizeT span = UniformStorageSpanInBytes(ttype, size);
        if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
            offset + span > programObject->GetUBOSize()) {
            MGLOG_E("%s: uniform at program %u location %d has no backing storage; returning nothing", __func__,
                    program, location);
            return;
        }

        if constexpr (std::is_same_v<T, GLfloat>) {
            if (TryGatherFloatMatrixColumns(ttype, pUBO + offset, params)) return;
        }

        // A double-precision uniform is the one case where the stored component type can
        // differ from the queried one for a non-opaque uniform, and the difference is not
        // just a reinterpretation: it is twice as wide, so a raw copy would overrun the
        // caller's buffer as well as return nonsense. Read component by component and let
        // GL's conversion rules (7.6: round to nearest for the integer queries) apply.
        if (ttype->getBasicType() == glslang::EbtDouble) {
            const Int columns = ttype->isMatrix() ? ttype->getMatrixCols() : 1;
            const Int rows = ttype->isMatrix() ? ttype->getMatrixRows()
                                               : (ttype->isVector() ? ttype->getVectorSize() : 1);
            // The slot the linker handed out is exactly `columns` columns wide, so it also
            // states the column stride - which for a double matrix is not a float's 16 bytes.
            const SizeT columnStride = columns > 0 ? size / static_cast<SizeT>(columns) : size;
            for (Int column = 0; column < columns; ++column) {
                for (Int row = 0; row < rows; ++row) {
                    GLdouble component = 0.0;
                    Memcpy(&component, pUBO + offset + column * columnStride + row * sizeof(GLdouble),
                           sizeof(component));
                    if constexpr (std::is_integral_v<T>) {
                        // Rounded to the nearest integer and clamped into the queried type's
                        // range, so a negative double read through glGetUniformuiv is 0
                        // rather than its two's complement.
                        const GLdouble rounded = std::nearbyint(component);
                        const GLdouble lowest = static_cast<GLdouble>(std::numeric_limits<T>::lowest());
                        const GLdouble highest = static_cast<GLdouble>(std::numeric_limits<T>::max());
                        params[column * rows + row] = static_cast<T>(std::clamp(rounded, lowest, highest));
                    } else {
                        params[column * rows + row] = static_cast<T>(component);
                    }
                }
            }
            return;
        }

        Memcpy(params, pUBO + offset, size);
    }

    void GetUniformdv_State(GLuint program, GLint location, GLdouble* params) {
        GetUniformScalar_State(program, location, params);
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
        // Deletion-flagged names stay valid while the object is still GL-visible (program in
        // use, shader attached), so name validity is exactly the Is* answer.
        if (program == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateProgramName(program) ? GL_TRUE : GL_FALSE;
    }

    GLboolean IsShader_State(GLuint shader) {
        if (shader == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateShaderName(shader) ? GL_TRUE : GL_FALSE;
    }

    void LinkProgram_State(GLuint program) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        MGLOG_D("%s: linking program %d", __func__, program);

        // Relinking the program an active transform feedback captures from would
        // invalidate its varyings mid-capture (GL 3.3 core 2.11.3).
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            MG_State::pGLContext->GetTransformFeedbackProgram().get() == programObject.get()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "The program used by active transform feedback cannot be relinked."));
            return;
        }

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

        // The program in use may not change while transform feedback is active - unless
        // the capture is paused, which is exactly what ARB_transform_feedback2 added the
        // pause for (GL 4.6 core 7.3).
        if (MG_State::pGLContext->IsTransformFeedbackActive() &&
            !MG_State::pGLContext->IsTransformFeedbackPaused()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "The current program cannot change while transform feedback is active."));
            return;
        }

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
            const SizeT size = programObject.GetUniformSizesInBytes(location);
            const Uint offset = programObject.GetUniformOffset(location);
            char* pUBO = static_cast<char*>(programObject.MapUBO());
            const SizeT uboSize = programObject.GetUBOSize();
            SizeT writeSize = ItemCount * sizeof(T);
            if (size < writeSize) {
                // Metadata bug: degrade to a clamped copy instead of killing the process.
                MGLOG_E("%s: uniform size mismatch at program %u location %u: expected at least %zu bytes, got %zu "
                        "bytes; clamping",
                        __func__, programObject.GetExternalIndex(), location, ItemCount * sizeof(T), size);
                writeSize = size;
            }
            if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + byteOffsetInsideUniform + writeSize > uboSize) {
                // Should not happen: linking gives every settable uniform backing
                // storage. Log and drop the write instead of faulting.
                MGLOG_E("%s: uniform at program %u location %u has no backing storage (ubo=%p offset=%u size=%zu "
                        "uboSize=%zu); dropping write",
                        __func__, programObject.GetExternalIndex(), location, static_cast<void*>(pUBO), offset,
                        writeSize, uboSize);
                return;
            }
            MGLOG_D("%s: program = %d, location = %d, byteOffset = %d", __func__, programObject.GetExternalIndex(),
                    location, offset + byteOffsetInsideUniform);
            // Apps re-set identical uniform values constantly (Minecraft re-uploads the same
            // matrices and sampler indices every frame), and any content-version move makes both
            // backends re-upload the whole UBO on the next draw. Every glUniform entry point
            // funnels its final bytes through here - after any transpose/stride conversion, with
            // the exact destination range known - and the scratch is zero-filled at link (matching
            // the GL zero defaults), so a bytes-equal write can be dropped without moving the
            // version.
            if (std::memcmp(pUBO + offset + byteOffsetInsideUniform, value, writeSize) == 0) return;
            Memcpy(pUBO + offset + byteOffsetInsideUniform, value, writeSize);
            programObject.MarkUBOContentDirty();
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

        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        for (GLint offset = 0; offset < count; offset++) {
            if (offset > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + offset)) {
                // GL 3.3 §2.11.4: values for elements beyond the end of the uniform
                // array are ignored. Never step onto a neighboring uniform's location.
                break;
            }
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
            if (offset > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + offset)) {
                // Values for elements beyond the end of the uniform array are ignored.
                break;
            }
            if (!programObject->IsValidUniformLocation(location + offset)) {
                RecordInvalidUniformLocationError(__func__, location + offset,
                                                  "program " + std::to_string(program));
                return;
            }
            Uniform_State<ItemCount>(*programObject, location + offset, value + offset * ItemCount);
        }
    }

    // glUniform*d / glUniformMatrix*dv. The vector forms need nothing beyond the shared
    // upload template - it is already typed on the component - but a matrix does: the
    // column stride the linker used for a double matrix is not the 16 bytes a float one
    // gets. It is not guessed here; the slot the uniform was given is exactly `columns`
    // columns wide, so dividing states the stride the rest of the pipeline agreed on.
    template <typename Program>
    void UniformMatrixdv_Object(Program& programObject, GLint location, GLsizei count, GLboolean transpose,
                                const GLdouble* value, Int columns, Int rows) {
        const SizeT slotSize = programObject.GetUniformSizesInBytes(location);
        const SizeT columnStride = columns > 0 ? slotSize / static_cast<SizeT>(columns) : slotSize;
        const SizeT componentCount = static_cast<SizeT>(columns) * static_cast<SizeT>(rows);
        Vector<GLdouble> column(static_cast<SizeT>(rows));
        for (GLint matrix = 0; matrix < count; ++matrix) {
            if (matrix > 0 && !programObject.UniformLocationsAliasSameUniform(location, location + matrix)) break;
            if (!programObject.IsValidUniformLocation(location + matrix)) {
                RecordInvalidUniformLocationError(__func__, location + matrix, "the current program object");
                return;
            }
            const GLdouble* source = value + matrix * componentCount;
            for (Int c = 0; c < columns; ++c) {
                for (Int r = 0; r < rows; ++r) {
                    column[r] = transpose == GL_TRUE ? source[r * columns + c] : source[c * rows + r];
                }
                Uniform_State<1>(programObject, location + matrix, column.data(), c * columnStride);
                for (Int r = 1; r < rows; ++r) {
                    Uniform_State<1>(programObject, location + matrix, column.data() + r,
                                     c * columnStride + r * sizeof(GLdouble));
                }
            }
        }
    }

    // glUniformMatrix*fv / glProgramUniformMatrix*fv, every shape (square and non-square).
    // A float matrix sits in the global UBO under std140 rules: each of its `columns`
    // column vectors starts on its own 16-byte boundary no matter how many rows it has, so
    // the only shape that may be written as one contiguous block is mat4. Writing a matNxM
    // as N*M packed floats puts every column after the first at the wrong byte offset.
    template <typename Program>
    void UniformMatrixfv_Object(Program& programObject, const char* caller, GLint location, GLsizei count,
                                GLboolean transpose, const GLfloat* value, Int columns, Int rows,
                                const String& ownerDescription) {
        // std140: a column vector of a float matrix is padded out to a vec4.
        constexpr SizeT kColumnStride = 4 * sizeof(GLfloat);
        const SizeT componentCount = static_cast<SizeT>(columns) * static_cast<SizeT>(rows);
        GLfloat column[4] = {};
        for (GLint matrix = 0; matrix < count; ++matrix) {
            if (matrix > 0 && !programObject.UniformLocationsAliasSameUniform(location, location + matrix)) {
                // GL 3.3 2.11.4: values for elements beyond the end of the uniform array
                // are ignored. Never step onto a neighboring uniform's location.
                break;
            }
            if (!programObject.IsValidUniformLocation(location + matrix)) {
                RecordInvalidUniformLocationError(caller, location + matrix, ownerDescription);
                return;
            }
            if (programObject.IsUniformOpaqueAtLocation(location + matrix)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "Opaque uniforms cannot be set with matrix Uniform calls."));
                return;
            }
            if (value == nullptr) return;
            const GLfloat* source = value + static_cast<SizeT>(matrix) * componentCount;
            for (Int c = 0; c < columns; ++c) {
                for (Int r = 0; r < rows; ++r) {
                    column[r] = transpose == GL_TRUE ? source[r * columns + c] : source[c * rows + r];
                }
                const SizeT byteOffset = static_cast<SizeT>(c) * kColumnStride;
                switch (rows) {
                case 2: Uniform_State<2>(programObject, location + matrix, column, byteOffset); break;
                case 3: Uniform_State<3>(programObject, location + matrix, column, byteOffset); break;
                default: Uniform_State<4>(programObject, location + matrix, column, byteOffset); break;
                }
            }
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
        // A mat2 is NOT four contiguous floats in the global UBO: std140 pads each column
        // vector out to 16 bytes, so column 1 starts at byte 16, not byte 8.
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        UniformMatrixfv_Object(*programObject, __func__, location, count, transpose, value, 2, 2,
                               "the current program object");
    }

    void UniformMatrix3fv_State(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        // For 3x3 matrices, we have 9 elements per matrix
        // If transpose is GL_TRUE, we need to transpose the matrix data
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        // For matrix uniforms, we handle each matrix individually
        // Handle padding in mat3 correctly!!
        for (GLint i = 0; i < count; i++) {
            if (i > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + i)) {
                // Values for elements beyond the end of the uniform array are ignored.
                break;
            }
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

        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }

        // For matrix uniforms, we handle each matrix individually
        for (GLint i = 0; i < count; i++) {
            if (i > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + i)) {
                // Values for elements beyond the end of the uniform array are ignored.
                break;
            }
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

    void UniformMatrixNonSquarefv_State(const char* caller, GLint location, GLsizei count, GLboolean transpose,
                                        const GLfloat* value, Int columns, Int rows) {
        if (location == -1) return;

        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "There is no current program object."));
            return;
        }

        UniformMatrixfv_Object(*programObject, caller, location, count, transpose, value, columns, rows,
                               "the current program object");
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

        UniformMatrixfv_Object(*programObject, __func__, location, count, transpose, value, 2, 2,
                               "program " + std::to_string(program));
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
            if (i > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + i)) {
                // Values for elements beyond the end of the uniform array are ignored.
                break;
            }
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
            if (i > 0 && !programObject->UniformLocationsAliasSameUniform(location, location + i)) {
                // Values for elements beyond the end of the uniform array are ignored.
                break;
            }
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

    void ProgramUniformMatrixNonSquarefv_State(const char* caller, GLuint program, GLint location, GLsizei count,
                                               GLboolean transpose, const GLfloat* value, Int columns, Int rows) {
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

        UniformMatrixfv_Object(*programObject, caller, location, count, transpose, value, columns, rows,
                               "program " + std::to_string(program));
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
        MGLOG_D("GBI prog=%u name='%s' -> %d", program, uniformBlockName ? uniformBlockName : "(null)", (Int)index);
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
        MGLOG_D("UBB prog=%u idx=%u binding=%u", program, uniformBlockIndex, uniformBlockBinding);
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
            // Member entries of an arrayed block are recorded against the first instance;
            // every instance of the array reports that shared member set (matches
            // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, which scans with the same owner index).
            const Int ownerIndex = static_cast<Int>(programObject->GetUniformBlockMemberOwnerIndex(uniformBlockIndex));
            GLint uniformIndexCount = 0;
            for (Uint uniformIndex = 0; uniformIndex < programObject->GetUniformCount(); ++uniformIndex) {
                if (programObject->GetActiveUniformBlockIndex(uniformIndex) != ownerIndex) {
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

    void BindFragDataLocationIndexed_State(GLuint program, GLuint colorNumber, GLuint index, const char* name) {
        auto& programObject = TryToGetProgramObject(program);
        // TryToGetProgramObject already recorded the error for a bad handle (GL_INVALID_VALUE for an
        // unknown name, GL_INVALID_OPERATION for a non-program object); do not record a second one.
        if (!programObject) return;
        if (name == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "name cannot be null."));
            return;
        }
        // index selects the single (0) or dual-source (1) color; it must be 0 or 1.
        if (index > 1) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "index must be 0 or 1."));
            return;
        }
        const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
        // colorNumber is bounded by GL_MAX_DRAW_BUFFERS for index 0, and by
        // GL_MAX_DUAL_SOURCE_DRAW_BUFFERS (which MobileGL reports as 1) for index 1.
        const GLuint colorNumberLimit =
            (index == 0) ? static_cast<GLuint>(dynamicParameters.MaxDrawBuffers) : 1u;
        if (colorNumber >= colorNumberLimit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "colorNumber exceeds the applicable draw-buffer limit."));
            return;
        }
        if (strncmp(name, "gl_", 3) == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "name " + std::string(name) + " starts with the reserved prefix `gl_`."));
            return;
        }

        MGLOG_D("%s: loc %02d index %u = \"%s\"", __func__, colorNumber, index, name);
        programObject->SetExplicitFragmentOutLocation(colorNumber, name);
        programObject->SetExplicitFragmentOutIndex(index, name);
    }

    // glBindFragDataLocation is glBindFragDataLocationIndexed with color index 0.
    void BindFragDataLocation_State(GLuint program, GLuint colorNumber, const char* name) {
        BindFragDataLocationIndexed_State(program, colorNumber, 0, name);
    }

    GLint GetFragDataLocation_State(GLuint program, const char* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return -1; // TryToGetProgramObject already recorded the error.
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

    GLint GetFragDataIndex_State(GLuint program, const char* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return -1; // TryToGetProgramObject already recorded the error.
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
        // Returns the color index bound by glBindFragDataLocationIndexed (0 by default), or -1 if name
        // is not an active user-defined output. Note: the index is tracked for reflection but is not
        // yet plumbed into dual-source blend rendering, and shader-side layout(index=) is not reflected.
        return programObject->GetFragmentDataIndex(name);
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

    void MaxShaderCompilerThreadsKHR(GLuint count) {
        MaxShaderCompilerThreadsKHR_State(count);
    }

    // GL_ARB_parallel_shader_compile's spelling of the same entry point.
    void MaxShaderCompilerThreadsARB(GLuint count) {
        MaxShaderCompilerThreadsKHR_State(count);
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

    void GetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname,
                             GLint* params) {
        GetActiveUniformsiv_State(program, uniformCount, uniformIndices, pname, params);
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
    void Uniform1d(GLint location, GLdouble v0) {
        const GLdouble v[] = {v0};
        Uniformv_State<1>(location, 1, v);
    }

    void Uniform1dv(GLint location, GLsizei count, const GLdouble* value) {
        Uniformv_State<1>(location, count, value);
    }

    void ProgramUniform1d(GLuint program, GLint location, GLdouble v0) {
        const GLdouble v[] = {v0};
        ProgramUniformv_State<1>(program, location, 1, v);
    }

    void ProgramUniform1dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformv_State<1>(program, location, count, value);
    }
    void Uniform2d(GLint location, GLdouble v0, GLdouble v1) {
        const GLdouble v[] = {v0, v1};
        Uniformv_State<2>(location, 1, v);
    }

    void Uniform2dv(GLint location, GLsizei count, const GLdouble* value) {
        Uniformv_State<2>(location, count, value);
    }

    void ProgramUniform2d(GLuint program, GLint location, GLdouble v0, GLdouble v1) {
        const GLdouble v[] = {v0, v1};
        ProgramUniformv_State<2>(program, location, 1, v);
    }

    void ProgramUniform2dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformv_State<2>(program, location, count, value);
    }
    void Uniform3d(GLint location, GLdouble v0, GLdouble v1, GLdouble v2) {
        const GLdouble v[] = {v0, v1, v2};
        Uniformv_State<3>(location, 1, v);
    }

    void Uniform3dv(GLint location, GLsizei count, const GLdouble* value) {
        Uniformv_State<3>(location, count, value);
    }

    void ProgramUniform3d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2) {
        const GLdouble v[] = {v0, v1, v2};
        ProgramUniformv_State<3>(program, location, 1, v);
    }

    void ProgramUniform3dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformv_State<3>(program, location, count, value);
    }
    void Uniform4d(GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3) {
        const GLdouble v[] = {v0, v1, v2, v3};
        Uniformv_State<4>(location, 1, v);
    }

    void Uniform4dv(GLint location, GLsizei count, const GLdouble* value) {
        Uniformv_State<4>(location, count, value);
    }

    void ProgramUniform4d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3) {
        const GLdouble v[] = {v0, v1, v2, v3};
        ProgramUniformv_State<4>(program, location, 1, v);
    }

    void ProgramUniform4dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformv_State<4>(program, location, count, value);
    }
    void UniformMatrix2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 2);
    }

    void ProgramUniformMatrix2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 2);
    }
    void UniformMatrix3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 3);
    }

    void ProgramUniformMatrix3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 3);
    }
    void UniformMatrix4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 4);
    }

    void ProgramUniformMatrix4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 4);
    }
    void UniformMatrix2x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 3);
    }

    void ProgramUniformMatrix2x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 3);
    }
    void UniformMatrix2x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 4);
    }

    void ProgramUniformMatrix2x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 2, 4);
    }
    void UniformMatrix3x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 2);
    }

    void ProgramUniformMatrix3x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 2);
    }
    void UniformMatrix3x4dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 4);
    }

    void ProgramUniformMatrix3x4dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 3, 4);
    }
    void UniformMatrix4x2dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 2);
    }

    void ProgramUniformMatrix4x2dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 2);
    }
    void UniformMatrix4x3dv(GLint location, GLsizei count, GLboolean transpose, const GLdouble* value) {
        if (location == -1) return;
        auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        if (programObject == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "There is no current program object."));
            return;
        }
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 3);
    }

    void ProgramUniformMatrix4x3dv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value) {
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
        UniformMatrixdv_Object(*programObject, location, count, transpose, value, 4, 3);
    }
    void GetUniformdv(GLuint program, GLint location, GLdouble* params) {
        GetUniformdv_State(program, location, params);
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
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 2, 3);
    }

    void UniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 3, 2);
    }

    void UniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 2, 4);
    }

    void UniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 4, 2);
    }

    void UniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 3, 4);
    }

    void UniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
        UniformMatrixNonSquarefv_State(__func__, location, count, transpose, value, 4, 3);
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
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 2, 3);
    }

    void ProgramUniformMatrix3x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 3, 2);
    }

    void ProgramUniformMatrix2x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 2, 4);
    }

    void ProgramUniformMatrix4x2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 4, 2);
    }

    void ProgramUniformMatrix3x4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 3, 4);
    }

    void ProgramUniformMatrix4x3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                   const GLfloat* value) {
        ProgramUniformMatrixNonSquarefv_State(__func__, program, location, count, transpose, value, 4, 3);
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

    void BindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const char* name) {
        BindFragDataLocationIndexed_State(program, colorNumber, index, name);
    }

    GLint GetFragDataLocation(GLuint program, const char* name) {
        return GetFragDataLocation_State(program, name);
    }

    GLint GetFragDataIndex(GLuint program, const char* name) {
        return GetFragDataIndex_State(program, name);
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
        if (programInterface == GL_UNIFORM) {
            if (pname == GL_ACTIVE_RESOURCES) {
                *params = static_cast<GLint>(programObject->GetUniformCount());
                return;
            }
            if (pname == GL_MAX_NAME_LENGTH) {
                // Stored as the bare length; GL_MAX_NAME_LENGTH counts the terminator.
                *params = programObject->GetUniformMaxLength() + 1;
                return;
            }
        }
        getProgramInterfaceiv(program, programInterface, pname, params);
    }

    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return GL_INVALID_INDEX;
        if (!ValidateNamedProgramResourceInterface(programInterface, __func__)) return GL_INVALID_INDEX;
        if (!name) return GL_INVALID_INDEX;
        if (programInterface == GL_UNIFORM) {
            const Int uniformIndex = programObject->GetActiveUniformIndex(name);
            return uniformIndex < 0 ? GL_INVALID_INDEX : static_cast<GLuint>(uniformIndex);
        }
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
        if (programInterface == GL_UNIFORM) {
            // Same index space GetProgramResourceIndex answers in, and the range check above
            // already used it.
            const String& uniformName = programObject->GetActiveUniformName(index);
            CopyStr(bufSize, length, name, uniformName.c_str(), static_cast<GLsizei>(uniformName.length()));
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
        if (programInterface == GL_UNIFORM) {
            if (index >= programObject->GetUniformCount()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "index is out of range."));
                return;
            }
            if (props == nullptr || params == nullptr) return;
            GLsizei written = 0;
            for (GLsizei i = 0; i < propCount && written < bufSize; ++i) {
                GLint value = 0;
                if (!GetUniformResourceProp(programObject, index, props[i], &value)) {
                    // GL_ATOMIC_COUNTER_BUFFER_INDEX and the GL_REFERENCED_BY_* stage props are
                    // not modelled here; ask the backend, which indexes resources by name.
                    auto backendGetIndex = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceIndex;
                    auto backendGetiv = MG_Backend::gBackendFunctionsTable.GL.GetProgramResourceiv;
                    if (backendGetIndex && backendGetiv) {
                        const GLuint backendIndex = backendGetIndex(program, GL_UNIFORM,
                                                                   programObject->GetActiveUniformName(index).c_str());
                        if (backendIndex != GL_INVALID_INDEX) {
                            GLsizei one = 0;
                            backendGetiv(program, GL_UNIFORM, backendIndex, 1, &props[i], 1, &one, &value);
                        }
                    }
                }
                params[written++] = value;
            }
            if (length) *length = written;
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

    // ARB_get_program_binary with no supported binary format (GL_NUM_PROGRAM_BINARY_FORMATS
    // is 0, which the extension explicitly allows). The three entry points below are what an
    // application - and dEQP's function loader - reach through the extension; without it
    // glProgramParameteri is not exposed in a 4.0 context at all.
    void ProgramParameteri(GLuint program, GLenum pname, GLint value) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (pname != GL_PROGRAM_BINARY_RETRIEVABLE_HINT && pname != GL_PROGRAM_SEPARABLE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "pname is not an accepted value."));
            return;
        }
        if (value != GL_TRUE && value != GL_FALSE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value must be GL_TRUE or GL_FALSE."));
            return;
        }
        if (pname == GL_PROGRAM_SEPARABLE) {
            programObject->SetSeparable(value == GL_TRUE);
            return;
        }
        programObject->SetBinaryRetrievableHint(value == GL_TRUE);
    }

    // GL 4.6 core 7.3: glCreateShaderProgramv is defined as the exact sequence below, so it
    // is written as that sequence rather than as a private shortcut - every error it can
    // raise is one of theirs, raised at the point they would raise it.
    GLuint CreateShaderProgramv(GLenum type, GLsizei count, const GLchar* const* strings) {
        // GL 4.6 core 7.3: a negative count is INVALID_VALUE and is checked before anything
        // is created, so a bad count never leaks a shader name. An unrecognised type is
        // INVALID_ENUM, which CreateShader_State raises below.
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "count must be non-negative."));
            return 0;
        }

        const GLuint shader = CreateShader_State(type);
        if (shader == 0) return 0;

        ShaderSource_State(shader, count, strings, nullptr);
        CompileShader_State(shader);

        const GLuint program = CreateProgram_State();
        if (program != 0) {
            const auto& shaderObject = MG_State::pGLContext->GetShaderObject(shader);
            const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
            // The program is separable whether or not the shader compiled: a failed
            // compile leaves an unlinked but otherwise well-formed separable program.
            if (programObject) programObject->SetSeparable(true);
            if (shaderObject && programObject && shaderObject->GetCompileStatus()) {
                AttachShader_State(program, shader);
                // Not LinkProgram_State: that injects a default fragment shader into a
                // program that has none, which is exactly wrong for a separable
                // vertex-stage program - the pipeline supplies the real one.
                programObject->Link(false);
                // glDetachShader defers the removal to the next link, so the program keeps
                // the shader object it was built from while no longer reporting it attached.
                DetachShader_State(program, shader);
            }
            if (shaderObject && programObject && !shaderObject->GetInfoLog().empty()) {
                programObject->AppendInfoLog(shaderObject->GetInfoLog());
            }
        }
        DeleteShader_State(shader);
        return program;
    }

    void GetProgramBinary(GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary) {
        (void)binaryFormat;
        (void)binary;
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "bufSize must be non-negative."));
            return;
        }
        if (length) *length = 0;
        // GL_PROGRAM_BINARY_LENGTH is always zero here, which the spec makes an error to ask for.
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "The program has no retrievable binary."));
    }

    void ProgramBinary(GLuint program, GLenum binaryFormat, const void* binary, GLsizei length) {
        (void)binaryFormat;
        (void)binary;
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (length < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "length must be non-negative."));
            return;
        }
        // No format is supported, so every binary is rejected - and the program's link status
        // has to read FALSE afterwards.
        programObject->MarkLinkFailedByProgramBinary();
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "binaryFormat is not a supported format."));
    }

    void TransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar* const* varyings, GLenum bufferMode) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (bufferMode != GL_INTERLEAVED_ATTRIBS && bufferMode != GL_SEPARATE_ATTRIBS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "bufferMode is not a valid capture mode."));
            return;
        }
        if (count < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "count must be non-negative."));
            return;
        }
        // GL 3.3 core: SEPARATE_ATTRIBS count may not exceed the separate-attrib limit.
        if (bufferMode == GL_SEPARATE_ATTRIBS && count > 4) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "count exceeds GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS."));
            return;
        }
        Vector<String> names;
        names.reserve(static_cast<SizeT>(count));
        for (GLsizei i = 0; i < count; ++i) {
            names.emplace_back(varyings != nullptr && varyings[i] != nullptr ? varyings[i] : "");
        }
        // ARB_transform_feedback3's special names only mean anything in an interleaved
        // capture, and gl_NextBuffer cannot advance past the last capture buffer.
        constexpr Uint maxTransformFeedbackBuffers = 4;
        Uint nextBufferCount = 0;
        for (const String& name : names) {
            const Bool isNextBuffer = name == "gl_NextBuffer";
            const Bool isSkipComponents = name.size() == 18 && name.compare(0, 17, "gl_SkipComponents") == 0 &&
                                          name[17] >= '1' && name[17] <= '4';
            if (!isNextBuffer && !isSkipComponents) continue;
            if (bufferMode != GL_INTERLEAVED_ATTRIBS) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "'" + name + "' requires GL_INTERLEAVED_ATTRIBS."));
                return;
            }
            if (isNextBuffer && ++nextBufferCount >= maxTransformFeedbackBuffers) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "More gl_NextBuffer entries than "
                                                 "GL_MAX_TRANSFORM_FEEDBACK_BUFFERS allows."));
                return;
            }
        }
        programObject->SetTransformFeedbackVaryings(Move(names), bufferMode);
    }

    void GetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLsizei* size,
                                     GLenum* type, GLchar* name) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject) return;
        if (!programObject->GetLinkStatus()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::to_string(program) + " has not been successfully linked."));
            return;
        }
        const auto* varying = programObject->GetTransformFeedbackVarying(index);
        if (varying == nullptr) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "index is not an active transform feedback varying of the program."));
            return;
        }
        if (size != nullptr) *size = varying->size;
        if (type != nullptr) *type = varying->type;
        GLsizei written = 0;
        if (name != nullptr && bufSize > 0) {
            written = std::min<GLsizei>(bufSize - 1, static_cast<GLsizei>(varying->name.size()));
            Memcpy(name, varying->name.data(), static_cast<SizeT>(written));
            name[written] = '\0';
        }
        if (length != nullptr) *length = written;
    }
} // namespace MobileGL::MG_Impl::GLImpl
