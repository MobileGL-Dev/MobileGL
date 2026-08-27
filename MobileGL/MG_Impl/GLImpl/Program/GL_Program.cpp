// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_Program.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Program.h"
#include "ProgramInterface.h"
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
    // The flattened uniform type these helpers used to take as a raw glslang::TType*
    // pointing into the TProgram's pool allocator. See ProgramObject::TypeFacts.
    using TypeFactsRef = const MG_State::GLState::ProgramObject::TypeFacts&;
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

    // The four non-location interface queries validate the NAME only: GL 4.6 imposes the
    // successful-link requirement on GetProgramResourceLocation/LocationIndex alone, and
    // requires the others to report a program that has never linked as one with zero active
    // resources. Being stricter leaves a stray GL_INVALID_OPERATION behind that aborts the
    // caller's next subcase.
    static const SharedPtr<MG_State::GLState::ProgramObject>& TryToGetProgramForInterfaceQuery(GLuint program,
                                                                                               const char* caller) {
        static const SharedPtr<MG_State::GLState::ProgramObject> nullProgramObject = nullptr;
        if (!MG_State::pGLContext->ValidateProgramName(program)) {
            const ErrorCode error = MG_State::pGLContext->ValidateShaderName(program)
                ? ErrorCode::InvalidOperation
                : ErrorCode::InvalidValue;
            MG_State::pGLContext->RecordError(
                error,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::to_string(program) + " is not a program object."));
            return nullProgramObject;
        }
        auto& programObject = MG_State::pGLContext->GetProgramObject(program);
        if (!programObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::to_string(program) + " is not a program object."));
            return nullProgramObject;
        }
        return programObject;
    }

    static bool IsProgramInterfaceEnum(GLenum programInterface) {
        return ProgramInterface::IsInterfaceEnum(programInterface);
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
            // Neither buffer interface has resource names. GL_TRANSFORM_FEEDBACK_BUFFER only
            // became reachable here when IsInterfaceEnum grew the GL 4.4 interfaces, so it
            // needs the same exclusion GL_ATOMIC_COUNTER_BUFFER already had.
            valid = valid && programInterface != GL_ATOMIC_COUNTER_BUFFER &&
                    programInterface != GL_TRANSFORM_FEEDBACK_BUFFER;
            break;
        case GL_MAX_NUM_ACTIVE_VARIABLES:
            valid = programInterface == GL_UNIFORM_BLOCK || programInterface == GL_ATOMIC_COUNTER_BUFFER ||
                    programInterface == GL_SHADER_STORAGE_BLOCK ||
                    programInterface == GL_TRANSFORM_FEEDBACK_BUFFER;
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
        if (!ProgramInterface::IsNamedInterface(programInterface)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Unsupported named program resource interface."));
            return false;
        }
        return true;
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

    GLint GetOpaqueUniformUnitLimit(const TypeFactsRef type) {
        const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();
        if (type.isImage) return dynamicParameters.MaxImageUnits;
        if (type.isTexture) return dynamicParameters.MaxCombinedTextureImageUnits;
        return 0;
    }

    bool ValidateOpaqueUniformUnit(const char* functionName, const TypeFactsRef type, GLint unit) {
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

    // GL 4.6 core 7.6.3: INVALID_VALUE when uniformBlockBinding >= MAX_UNIFORM_BUFFER_BINDINGS.
    // The storage-block twin below has always had this check; the uniform one never did, and the
    // value it stores is used as a RAW SUBSCRIPT into the state layer's fixed indexed-binding
    // array on every draw and dispatch (DirectGLES's per-program UBO rebind, DirectVulkan's
    // descriptor resolve, whose only guard is a MOBILEGL_ASSERT that compiles away in release).
    // An out-of-range binding therefore did not merely go unreported - it read past the array and
    // dereferenced whatever SharedPtr it found there.
    bool ValidateUniformBlockBinding(GLuint binding) {
        // Exactly what glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS) advertises: the state
        // layer's array width, which the getter clamps to as well.
        const SizeT maxBindingCount = MG_State::pGLContext->GetBufferBindingPointCount(BufferTarget::Uniform);
        if (binding < maxBindingCount) {
            return true;
        }

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidValue,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl", __func__,
                std::format("Uniform block binding {} is not less than GL_MAX_UNIFORM_BUFFER_BINDINGS ({}).", binding,
                            maxBindingCount)));
        return false;
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
        // GL 4.2 / ARB_shader_atomic_counters adds this one to the accepted set. Leaving it
        // out did not merely lose the answer: the leftover GL_INVALID_ENUM is what made
        // KHR-GL43.shader_atomic_counters.basic-program-query force a FAIL.
        case GL_UNIFORM_ATOMIC_COUNTER_BUFFER_INDEX:
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
            case GL_UNIFORM_ATOMIC_COUNTER_BUFFER_INDEX:
                // Index into the GL_ACTIVE_ATOMIC_COUNTER_BUFFERS list, -1 for every uniform
                // that is not an atomic counter (GL 4.6 core table 7.6).
                params[i] = programObject->GetActiveUniformAtomicCounterBufferIndex(idx);
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
            // Counter BUFFERS, not counters, and glslang's own getNumAtomicCounters() answers
            // neither: the relaxed parse has already turned every atomic_uint into a plain uint
            // member of a synthesized storage block by the time it builds its reflection, so it
            // reports zero. The interface-query model recovers the buffers from those blocks and
            // is what glGetProgramInterfaceiv(GL_ATOMIC_COUNTER_BUFFER, GL_ACTIVE_RESOURCES)
            // already answers - the two queries are required to agree.
            *params = ProgramInterface::GetActiveResourceCount(*programObject, GL_ATOMIC_COUNTER_BUFFER);
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
            // Uniform blocks only. GetActiveUniformBlocksCount() is the internal block space,
            // which also carries the storage blocks and the synthesized atomic counter blocks.
            *params = programObject->GetGlUniformBlockCount();
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
            // "a linked program object with a compute shader" is one whose EXECUTABLE has the
            // stage: the local size below is a link artifact, so an attached-but-not-yet-linked
            // compute shader would answer this query with the previous link's (absent) value
            // instead of the INVALID_OPERATION GL 4.6 core 7.13 asks for.
            if (!programObject->GetLinkStatus() || !programObject->HasLinkedShaderStage(ShaderStage::Compute)) {
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

        // The geometry and tessellation link properties (GL 4.6 core table 23.35). Same shape as
        // GL_COMPUTE_WORK_GROUP_SIZE above, and for the same reason: "a linked program object
        // with a geometry shader" is one whose EXECUTABLE has the stage, so an
        // attached-but-not-yet-linked shader must give INVALID_OPERATION rather than the previous
        // link's value. The geometry three used to be listed here only to fall through into the
        // INVALID_ENUM default, and the tessellation five were not listed at all.
        case GL_GEOMETRY_VERTICES_OUT:
        case GL_GEOMETRY_INPUT_TYPE:
        case GL_GEOMETRY_OUTPUT_TYPE:
        case GL_GEOMETRY_SHADER_INVOCATIONS: {
            if (!programObject->GetLinkStatus() || !programObject->HasLinkedShaderStage(ShaderStage::Geometry)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::to_string(program) +
                                                     " is not a linked program object with a geometry shader."));
                return;
            }
            switch (pname) {
            case GL_GEOMETRY_VERTICES_OUT: *params = programObject->GetGeometryVerticesOut(); break;
            case GL_GEOMETRY_INPUT_TYPE: *params = static_cast<GLint>(programObject->GetGeometryInputType()); break;
            case GL_GEOMETRY_OUTPUT_TYPE: *params = static_cast<GLint>(programObject->GetGeometryOutputType()); break;
            default: *params = programObject->GetGeometryShaderInvocations(); break;
            }
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        }
        case GL_TESS_CONTROL_OUTPUT_VERTICES: {
            if (!programObject->GetLinkStatus() || !programObject->HasLinkedShaderStage(ShaderStage::TessControl)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::to_string(program) +
                            " is not a linked program object with a tessellation control shader."));
                return;
            }
            *params = programObject->GetTessControlOutputVertices();
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        }
        case GL_TESS_GEN_MODE:
        case GL_TESS_GEN_SPACING:
        case GL_TESS_GEN_VERTEX_ORDER:
        case GL_TESS_GEN_POINT_MODE: {
            if (!programObject->GetLinkStatus() || !programObject->HasLinkedShaderStage(ShaderStage::TessEval)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::to_string(program) +
                            " is not a linked program object with a tessellation evaluation shader."));
                return;
            }
            switch (pname) {
            case GL_TESS_GEN_MODE: *params = static_cast<GLint>(programObject->GetTessGenMode()); break;
            case GL_TESS_GEN_SPACING: *params = static_cast<GLint>(programObject->GetTessGenSpacing()); break;
            case GL_TESS_GEN_VERTEX_ORDER:
                *params = static_cast<GLint>(programObject->GetTessGenVertexOrder());
                break;
            default: *params = programObject->GetTessGenPointMode() ? GL_TRUE : GL_FALSE; break;
            }
            MGLOG_D("%s: %s = %d", __func__, MG_Util::ConvertGLEnumToString(pname).c_str(), *params);
            break;
        }
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

    // MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS: while the compile job is still in flight -
    // and, via the latch below, for the rest of that node's life once any query was
    // answered this way - GL_COMPILE_STATUS reads GL_TRUE and the info log reads empty,
    // WITHOUT joining. The latch (TakeOptimisticCompileAnswer) is what makes the three
    // sites tell ONE story: without it, a job settling between an application's info-log
    // read and its status read would produce the torn pair "GL_FALSE with an empty log",
    // and an application that aborts on that never reaches the link join that carries the
    // real diagnostic. A failure hidden here still fails the program link, with the
    // compile log quoted in the program info log (ProgramLinkTask::ConsumeShaders), which
    // is where the serial compile-then-check applications this exists for do their error
    // handling.
    static Bool AnswerCompileOptimistically(const SharedPtr<MG_State::GLState::ShaderObject>& shaderObject) {
        return MG_Util::Async::OptimisticShaderStatusActive() && shaderObject->TakeOptimisticCompileAnswer();
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
            if (AnswerCompileOptimistically(shaderObject)) {
                *params = GL_TRUE;
                break;
            }
            *params = shaderObject->GetCompileStatus();
            break;
        case GL_INFO_LOG_LENGTH:
            // Not cosmetic: LWJGL's one-argument glGetShaderInfoLog convenience overload
            // sizes its buffer from this query, so a joining answer here would defeat the
            // non-joining GetShaderInfoLog below.
            if (AnswerCompileOptimistically(shaderObject)) {
                *params = 0;
                break;
            }
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

        // See AnswerCompileOptimistically: an in-flight compile reads as an empty log. The
        // cost is a lost compile WARNING (a successful compile whose log the application
        // reads exactly once, now, and never after the join) - accepted as part of the
        // opt-in.
        if (AnswerCompileOptimistically(shaderObject)) {
            CopyStr(bufSize, length, infoLog, "", 0);
            return;
        }

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
    // there. Returns false when there is nothing here to unpack.
    //
    // A DOUBLE matrix is declined not because it is laid out differently - it is not, the
    // demotion makes a dmat4 a mat4 in the shader and a mat4-shaped slot here - but because it
    // is ROUTED differently: the caller's component-by-component EbtDouble branch has to widen
    // each float back to the queried type, and it undoes the same padding itself.
    // Float matrices only, in both senses: a DOUBLE matrix never comes through here, whether its
    // program was demoted (components are floats, the query is not) or kept its doubles (the
    // column stride is a dvec4's, and the caller's converting branch already walks it component
    // by component with the right one).
    Bool TryGatherFloatMatrixColumns(const TypeFactsRef ttype, const char* pBase, void* params) {
        if (!ttype.isMatrix || ttype.isDouble) return false;
        const Int columns = ttype.matrixCols;
        const Int rows = ttype.matrixRows;
        for (Int column = 0; column < columns; ++column) {
            Memcpy(static_cast<char*>(params) + static_cast<SizeT>(column) * rows * sizeof(GLfloat),
                   pBase + static_cast<SizeT>(column) * 4 * sizeof(GLfloat), rows * sizeof(GLfloat));
        }
        return true;
    }

    // Bytes a uniform actually occupies in the global UBO. It is the tight GL type size for
    // everything except a matrix, whose padded columns make it wider, and a `double` on a
    // program whose modules were demoted, where it is half. The rule itself lives on
    // ProgramObject, because the pipeline composite's uniform refresh needs the same one and
    // two copies of a layout rule is one too many.
    SizeT UniformStorageSpanInBytes(const TypeFactsRef ttype, SizeT tightSize, const Bool nativeFloat64) {
        return MG_State::GLState::ProgramObject::UniformStorageSpanInBytes(ttype, tightSize, nativeFloat64);
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
            const auto& ttype = programObject->GetUniformTypeFacts(location);
            const Bool nativeFloat64 = programObject->UsesNativeFloat64();
            const SizeT span = UniformStorageSpanInBytes(ttype, size, nativeFloat64);
            if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + span > programObject->GetUBOSize()) {
                MGLOG_E_ONCE("%s: uniform at program %u location %d has no backing storage; returning nothing", __func__,
                        program, location);
                return;
            }

            if (!TryGatherFloatMatrixColumns(ttype, pUBO + offset, params)) {
                // Never more than the uniform actually occupies. `size` is the GL type size,
                // which on a DEMOTED program is twice a `double` uniform's storage - its 64-bit
                // floats were narrowed before the module reached a backend, so the slot holds
                // floats. The typed entry points (glGetUniformdv and friends) go through
                // GetUniformScalar_State, which converts component by component; this raw
                // copy has no type to convert with, so it is bounded rather than converted.
                Memcpy(params, pUBO + offset, std::min<SizeT>(size, span));
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
        const auto& ttype = programObject->GetUniformTypeFacts(location);
        const Bool nativeFloat64 = programObject->UsesNativeFloat64();
        const SizeT span = UniformStorageSpanInBytes(ttype, size, nativeFloat64);
        if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
            offset + span > programObject->GetUBOSize()) {
            MGLOG_E_ONCE("%s: uniform at program %u location %d has no backing storage; returning nothing", __func__,
                    program, location);
            return;
        }

        if constexpr (std::is_same_v<T, GLfloat>) {
            if (TryGatherFloatMatrixColumns(ttype, pUBO + offset, params)) return;
        }

        // A double-precision uniform is the one case where the stored component type can differ
        // from the DECLARED one for a non-opaque uniform: on a DEMOTED program the shader's
        // 64-bit floats were narrowed to 32 before the module reached the backend
        // (ShaderTranspiler::DemoteFloat64Pass), so what is in the global UBO is a float per
        // component, laid out exactly like the float-typed twin of this uniform - std140
        // 16-byte column stride for a matrix included. Reading it as a GLdouble would return
        // two components reinterpreted as one. A program that KEPT its doubles stores real ones
        // at the dvec4 column stride instead, so the width and the stride both move; everything
        // else about this walk is the same. Read component by component either way and let GL's
        // conversion rules (7.6: round to nearest for the integer queries) apply; the value
        // widens back to the queried type, having lost precision - where it lost any - at the
        // glUniform*d that stored it and not here.
        if (ttype.isDouble) {
            const Int columns = ttype.isMatrix ? ttype.matrixCols : 1;
            const Int rows = ttype.isMatrix ? ttype.matrixRows
                                               : (ttype.isVector ? ttype.vectorSize : 1);
            // A non-matrix is one tightly packed run and never reaches the stride at all.
            const SizeT columnStride =
                MG_State::GLState::ProgramObject::UniformMatrixColumnStride(ttype, nativeFloat64);
            const SizeT componentSize = nativeFloat64 ? sizeof(GLdouble) : sizeof(GLfloat);
            for (Int column = 0; column < columns; ++column) {
                for (Int row = 0; row < rows; ++row) {
                    GLdouble component = 0.0;
                    if (nativeFloat64) {
                        Memcpy(&component, pUBO + offset + column * columnStride + row * componentSize,
                               sizeof(GLdouble));
                    } else {
                        GLfloat narrow = 0.0f;
                        Memcpy(&narrow, pUBO + offset + column * columnStride + row * componentSize,
                               sizeof(narrow));
                        component = static_cast<GLdouble>(narrow);
                    }
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

        // Read fresh every link, never latched in a static: the capability is
        // per-backend, and a latch would freeze it across a backend teardown +
        // re-initialization (the previous function-static memo here never even set
        // its own initialized flag, so it re-read every call anyway - this makes
        // the always-fresh behavior the stated one). A struct-field read per
        // glLinkProgram costs nothing.
        const auto& activeBackendObject = MG_Backend::pActiveBackendObject;
        if (!activeBackendObject) {
            MGLOG_E_ONCE("activeBackendObject is not initialized!");
            return;
        }
        const Bool allowVSOnlyPrograms =
            activeBackendObject->GetRendererInfo().StaticBackendCapability.AllowVSOnlyPrograms;
        programObject->SetMaxFragmentOutputColorNumber(activeBackendObject->GetDynamicParameters().MaxDrawBuffers);
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
            // Record the write for the pipeline composite's uniform mirror, which copies only
            // the locations a stage program has actually been written to (see
            // ProgramObject::MarkUniformWrittenAtLocation). Here rather than further down
            // because every exit below is still a write as far as GL is concerned: the
            // buffered-write detour returns early, the bytes-equal dedupe returns early, and
            // even the no-backing-storage bail is a uniform the application addressed. This is
            // the funnel EVERY glUniform* and glProgramUniform* entry point reaches, once per
            // LOCATION - so an array element write marks that element and nothing else. On a
            // program that can never be a pipeline stage - the monolithic glUseProgram path,
            // which is where the thousands of calls per frame are - this is one bool branch.
            programObject.MarkUniformWrittenAtLocation(location);
            // Everything up to and including the clamp is phase-A data (the uniform's GL type
            // decides its size), so it is answered without joining anything.
            const SizeT size = programObject.GetUniformSizesInBytes(location);
            SizeT writeSize = ItemCount * sizeof(T);
            if (size < writeSize) {
                // Metadata bug: degrade to a clamped copy instead of killing the process.
                MGLOG_E_ONCE("%s: uniform size mismatch at program %u location %u: expected at least %zu bytes, got %zu "
                        "bytes; clamping",
                        __func__, programObject.GetExternalIndex(), location, ItemCount * sizeof(T), size);
                writeSize = size;
            }
            // The uniform shadow's LAYOUT is phase-B data, so a write that lands while the
            // SPIR-V job is still running is recorded and replayed at its publish instead of
            // joining it. This is the hot path for a shaderpack that sets its uniforms
            // immediately after glLinkProgram. BufferUniformWrite declines (and we fall
            // through, joining) only past its size budget.
            if (programObject.IsSpirvPending() &&
                programObject.BufferUniformWrite(location, byteOffsetInsideUniform, value, writeSize)) {
                return;
            }
            const Uint offset = programObject.GetUniformOffset(location);
            char* pUBO = static_cast<char*>(programObject.MapUBO());
            const SizeT uboSize = programObject.GetUBOSize();
            if (pUBO == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + byteOffsetInsideUniform + writeSize > uboSize) {
                // Should not happen: linking gives every settable uniform backing
                // storage. Log and drop the write instead of faulting.
                MGLOG_E_ONCE("%s: uniform at program %u location %u has no backing storage (ubo=%p offset=%u size=%zu "
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
            const auto& ttype = programObject.GetUniformTypeFacts(location);
            if (!ttype.isTexture && !ttype.isImage) return;
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

    // Whether the program a uniform write is about to land in stores 64-bit floats at their
    // declared width. Answered off the PROGRAM, never off the live backend: it describes the
    // modules that were actually built for it, and a backend with native fp64 still demotes a
    // program whose vertex stage declares a Float64 input (see ProgramSpirvTask::GenerateSpirv).
    // Nullptr - no current program, or a name that is not a program - answers false and lets the
    // callee record the same error it always did.
    Bool CurrentProgramUsesNativeFloat64() {
        if (MG_State::pGLContext == nullptr) return false;
        const auto& programObject = MG_State::pGLContext->GetProgramForUniform();
        return programObject != nullptr && programObject->UsesNativeFloat64();
    }

    Bool NamedProgramUsesNativeFloat64(GLuint program) {
        const auto& programObject = TryToGetProgramObject(program);
        return programObject != nullptr && programObject->GetLinkStatus() && programObject->UsesNativeFloat64();
    }

    // glUniform*d / glUniformMatrix*dv. On a DEMOTED program neither needs a layout of its own:
    // the transpile chain narrowed every 64-bit float in the shader to 32
    // (ShaderTranspiler::DemoteFloat64Pass) and the global UBO is laid out by reflecting that
    // demoted module, so a double uniform's storage IS a float uniform's - same offset, same
    // 4-byte components, same std140 column padding for matrices. Narrowing here, at the one
    // place the 64-bit value enters, and then handing the bytes to the ordinary float upload
    // path is what keeps the two in step; a separate double-shaped layout there would write
    // 8-byte components into 4-byte slots and silently address the wrong ones.
    //
    // The narrowing is the same static_cast the demoted shader's own arithmetic performs, so the
    // value the shader reads is the value glUniform*d was given, at float precision.
    //
    // On a program that KEPT its doubles the reverse is true and for the same reason: its global
    // UBO really does hold 8-byte components, so narrowing would leave a float bit pattern in the
    // low half of a double slot - which is not a precision loss but a garbage value. The 64-bit
    // values go through unchanged then, and the upload path is width-agnostic (it is templated on
    // the component type and bounded by the uniform's own slot span).
    //
    // Note TryToGetProgramObject / GetProgramForUniform run TWICE on this path, once for the
    // width question and once inside the call below. That is a lookup and a join on an entry
    // point no shader pack uses; the alternative is duplicating both functions' whole validation
    // sequence here, which is the thing that must not drift.
    template <GLsizei ItemCount>
    void UniformvNarrowed_State(GLint location, GLsizei count, const GLdouble* value) {
        if (value == nullptr || count <= 0) {
            // Same shape as the float entry points: the location validation still runs, and a
            // null pointer is left to fault exactly where glUniform*fv would.
            Uniformv_State<ItemCount>(location, count, reinterpret_cast<const GLfloat*>(value));
            return;
        }
        if (location != -1 && CurrentProgramUsesNativeFloat64()) {
            Uniformv_State<ItemCount>(location, count, value);
            return;
        }
        Vector<GLfloat> narrowed(static_cast<SizeT>(count) * ItemCount);
        for (SizeT i = 0; i < narrowed.size(); ++i) narrowed[i] = static_cast<GLfloat>(value[i]);
        Uniformv_State<ItemCount>(location, count, narrowed.data());
    }

    template <GLsizei ItemCount>
    void ProgramUniformvNarrowed_State(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        if (value == nullptr || count <= 0) {
            ProgramUniformv_State<ItemCount>(program, location, count, reinterpret_cast<const GLfloat*>(value));
            return;
        }
        if (location != -1 && NamedProgramUsesNativeFloat64(program)) {
            ProgramUniformv_State<ItemCount>(program, location, count, value);
            return;
        }
        Vector<GLfloat> narrowed(static_cast<SizeT>(count) * ItemCount);
        for (SizeT i = 0; i < narrowed.size(); ++i) narrowed[i] = static_cast<GLfloat>(value[i]);
        ProgramUniformv_State<ItemCount>(program, location, count, narrowed.data());
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

    // glUniformMatrix*dv / glProgramUniformMatrix*dv on a program that KEPT its doubles. Same
    // walk as UniformMatrixfv_Object down to the last branch, and deliberately a copy of it
    // rather than a template over the component type: the two differ in exactly one number that
    // is not derivable from the component type alone - std140 pads a double matrix's column out
    // to a dvec4 (32 bytes) unless the column is a dvec2, which is already 16 - and folding that
    // into the float version would put a per-call branch on the hot glUniformMatrix4fv path
    // Minecraft calls thousands of times a frame for a case no shader pack ever takes.
    template <typename Program>
    void UniformMatrixdvNative_Object(Program& programObject, GLint location, GLsizei count, GLboolean transpose,
                                      const GLdouble* value, Int columns, Int rows,
                                      const String& ownerDescription) {
        const SizeT columnStride = rows <= 2 ? 2 * sizeof(GLdouble) : 4 * sizeof(GLdouble);
        const SizeT componentCount = static_cast<SizeT>(columns) * static_cast<SizeT>(rows);
        GLdouble column[4] = {};
        for (GLint matrix = 0; matrix < count; ++matrix) {
            if (matrix > 0 && !programObject.UniformLocationsAliasSameUniform(location, location + matrix)) break;
            if (!programObject.IsValidUniformLocation(location + matrix)) {
                RecordInvalidUniformLocationError("glUniformMatrixdv", location + matrix, ownerDescription);
                return;
            }
            if (programObject.IsUniformOpaqueAtLocation(location + matrix)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "glUniformMatrixdv",
                                                 "Opaque uniforms cannot be set with matrix Uniform calls."));
                return;
            }
            const GLdouble* source = value + static_cast<SizeT>(matrix) * componentCount;
            for (Int c = 0; c < columns; ++c) {
                for (Int r = 0; r < rows; ++r) {
                    column[r] = transpose == GL_TRUE ? source[r * columns + c] : source[c * rows + r];
                }
                const SizeT byteOffset = static_cast<SizeT>(c) * columnStride;
                switch (rows) {
                case 2: Uniform_State<2>(programObject, location + matrix, column, byteOffset); break;
                case 3: Uniform_State<3>(programObject, location + matrix, column, byteOffset); break;
                default: Uniform_State<4>(programObject, location + matrix, column, byteOffset); break;
                }
            }
        }
    }

    // glUniformMatrix*dv / glProgramUniformMatrix*dv. On a DEMOTED program this narrows to the
    // float form and hands it straight over: after DemoteFloat64Pass a `dmat4` uniform is a
    // `mat4` in the shader and a mat4-shaped slot in the global UBO, columns padded to a vec4
    // and all. Everything else about the call - transpose handling, the array-element walk, the
    // opaque-uniform refusal - is then the one implementation both spellings share. A program
    // that kept its doubles gets the same walk at double width and the wider column stride.
    template <typename Program>
    void UniformMatrixdv_Object(Program& programObject, GLint location, GLsizei count, GLboolean transpose,
                                const GLdouble* value, Int columns, Int rows) {
        if (value == nullptr || count <= 0) return;
        if (programObject.UsesNativeFloat64()) {
            UniformMatrixdvNative_Object(programObject, location, count, transpose, value, columns, rows,
                                         "the current program object");
            return;
        }
        const SizeT componentCount = static_cast<SizeT>(columns) * static_cast<SizeT>(rows);
        Vector<GLfloat> narrowed(static_cast<SizeT>(count) * componentCount);
        for (SizeT i = 0; i < narrowed.size(); ++i) narrowed[i] = static_cast<GLfloat>(value[i]);
        UniformMatrixfv_Object(programObject, "glUniformMatrixdv", location, count, transpose, narrowed.data(),
                               columns, rows, "the current program object");
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

        // GetGlUniformBlockIndex, not GetUniformBlockIndex: the latter answers in the internal
        // block space, which also resolves storage blocks and the synthesized atomic counter
        // blocks. Neither is a uniform block (GL 4.6 core 7.6), so both are GL_INVALID_INDEX here.
        const auto index = programObject->GetGlUniformBlockIndex(uniformBlockName);
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
        if (!ValidateUniformBlockBinding(uniformBlockBinding)) return;
        if (!programObject->IsActiveGlUniformBlock(uniformBlockIndex)) {
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
        // The GL_UNIFORM_BLOCK index space skips the storage and atomic counter blocks the
        // block-keyed tables still carry; translate before touching them.
        const Uint blockIndex = static_cast<Uint>(programObject->BlockIndexFromGlUniformBlock(uniformBlockIndex));
        MGLOG_D("UBB prog=%u idx=%u binding=%u", program, uniformBlockIndex, uniformBlockBinding);
        programObject->SetUniformBlockBinding(blockIndex, uniformBlockBinding);
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
        if (!programObject->IsActiveGlUniformBlock(uniformBlockIndex)) {
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
        // The GL_UNIFORM_BLOCK index space skips the storage and atomic counter blocks the
        // block-keyed tables still carry; every accessor below is indexed by the block space.
        const Uint blockIndex = static_cast<Uint>(programObject->BlockIndexFromGlUniformBlock(uniformBlockIndex));
        switch (pname) {
        case GL_UNIFORM_BLOCK_DATA_SIZE: {
            *params = (GLint)programObject->GetUBOSizeAt(blockIndex);
            MGLOG_D("%s: GL_UNIFORM_BLOCK_DATA_SIZE = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_NAME_LENGTH: {
            *params = (GLint)programObject->GetUniformBlockName(blockIndex).length() + 1;
            MGLOG_D("%s: GL_UNIFORM_BLOCK_NAME_LENGTH = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
            *params = programObject->GetUniformBlockActiveUniformCount(blockIndex);
            MGLOG_D("%s: GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_BINDING: {
            *params = static_cast<GLint>(programObject->GetUniformBlockBinding(blockIndex));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_BINDING = %d", __func__, *params);
            break;
        }
        case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangVertex));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER:
            *params =
                BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangTessControl));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_CONTROL_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER:
            *params =
                BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangTessEvaluation));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_TESS_EVALUATION_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangGeometry));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_GEOMETRY_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangFragment));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_COMPUTE_SHADER:
            *params = BoolToGLInt(programObject->IsUniformBlockReferencedByStage(blockIndex, EShLangCompute));
            MGLOG_D("%s: GL_UNIFORM_BLOCK_REFERENCED_BY_COMPUTE_SHADER = %d", __func__, *params);
            break;
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
            // Member entries of an arrayed block are recorded against the first instance;
            // every instance of the array reports that shared member set (matches
            // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, which scans with the same owner index).
            //
            // Both sides of the comparison are BLOCK indices: GetUniformBlockMemberOwnerIndex
            // answers in that space, so the scan uses GetActiveUniformOwnerBlockIndex rather
            // than the GL_UNIFORM_BLOCK-space GetActiveUniformBlockIndex.
            const Int ownerIndex = static_cast<Int>(programObject->GetUniformBlockMemberOwnerIndex(blockIndex));
            GLint uniformIndexCount = 0;
            for (Uint uniformIndex = 0; uniformIndex < programObject->GetUniformCount(); ++uniformIndex) {
                if (programObject->GetActiveUniformOwnerBlockIndex(uniformIndex) != ownerIndex) {
                    continue;
                }
                params[uniformIndexCount++] = static_cast<GLint>(uniformIndex);
            }
            MGLOG_D("%s: GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES count = %d", __func__, uniformIndexCount);
            break;
        }
        default:
            MGLOG_D("%s: unknown pname = %p %s", __func__, pname, MG_Util::ConvertGLEnumToString(pname).c_str());
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
        if (!programObject->IsActiveGlUniformBlock(uniformBlockIndex)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    "uniformBlockIndex " + std::to_string(uniformBlockIndex) +
                        " is greater than or equal to the value of `GL_ACTIVE_UNIFORM_BLOCKS` or is "
                        "not the index of an active uniform block in program."));
            return;
        }
        const auto& name = programObject->GetUniformBlockName(
            static_cast<Uint>(programObject->BlockIndexFromGlUniformBlock(uniformBlockIndex)));
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
        UniformvNarrowed_State<1>(location, 1, v);
    }

    void Uniform1dv(GLint location, GLsizei count, const GLdouble* value) {
        UniformvNarrowed_State<1>(location, count, value);
    }

    void ProgramUniform1d(GLuint program, GLint location, GLdouble v0) {
        const GLdouble v[] = {v0};
        ProgramUniformvNarrowed_State<1>(program, location, 1, v);
    }

    void ProgramUniform1dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformvNarrowed_State<1>(program, location, count, value);
    }
    void Uniform2d(GLint location, GLdouble v0, GLdouble v1) {
        const GLdouble v[] = {v0, v1};
        UniformvNarrowed_State<2>(location, 1, v);
    }

    void Uniform2dv(GLint location, GLsizei count, const GLdouble* value) {
        UniformvNarrowed_State<2>(location, count, value);
    }

    void ProgramUniform2d(GLuint program, GLint location, GLdouble v0, GLdouble v1) {
        const GLdouble v[] = {v0, v1};
        ProgramUniformvNarrowed_State<2>(program, location, 1, v);
    }

    void ProgramUniform2dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformvNarrowed_State<2>(program, location, count, value);
    }
    void Uniform3d(GLint location, GLdouble v0, GLdouble v1, GLdouble v2) {
        const GLdouble v[] = {v0, v1, v2};
        UniformvNarrowed_State<3>(location, 1, v);
    }

    void Uniform3dv(GLint location, GLsizei count, const GLdouble* value) {
        UniformvNarrowed_State<3>(location, count, value);
    }

    void ProgramUniform3d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2) {
        const GLdouble v[] = {v0, v1, v2};
        ProgramUniformvNarrowed_State<3>(program, location, 1, v);
    }

    void ProgramUniform3dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformvNarrowed_State<3>(program, location, count, value);
    }
    void Uniform4d(GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3) {
        const GLdouble v[] = {v0, v1, v2, v3};
        UniformvNarrowed_State<4>(location, 1, v);
    }

    void Uniform4dv(GLint location, GLsizei count, const GLdouble* value) {
        UniformvNarrowed_State<4>(location, count, value);
    }

    void ProgramUniform4d(GLuint program, GLint location, GLdouble v0, GLdouble v1, GLdouble v2, GLdouble v3) {
        const GLdouble v[] = {v0, v1, v2, v3};
        ProgramUniformvNarrowed_State<4>(program, location, 1, v);
    }

    void ProgramUniform4dv(GLuint program, GLint location, GLsizei count, const GLdouble* value) {
        ProgramUniformvNarrowed_State<4>(program, location, count, value);
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
        auto& programObject = TryToGetProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (!ValidateProgramInterfaceivQuery(programInterface, pname)) return;
        if (!params) return;
        switch (pname) {
        case GL_ACTIVE_RESOURCES:
            *params = ProgramInterface::GetActiveResourceCount(*programObject, programInterface);
            return;
        case GL_MAX_NAME_LENGTH:
            *params = ProgramInterface::GetMaxNameLength(*programObject, programInterface);
            return;
        case GL_MAX_NUM_ACTIVE_VARIABLES:
            *params = ProgramInterface::GetMaxNumActiveVariables(*programObject, programInterface);
            return;
        default:
            // GL_MAX_NUM_COMPATIBLE_SUBROUTINES: the subroutine interfaces are always empty
            // here (glslang refuses `subroutine` when generating SPIR-V), so zero it is.
            *params = 0;
            return;
        }
    }

    GLuint GetProgramResourceIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetProgramForInterfaceQuery(program, __func__);
        if (!programObject) return GL_INVALID_INDEX;
        if (!ValidateNamedProgramResourceInterface(programInterface, __func__)) return GL_INVALID_INDEX;
        if (!name) return GL_INVALID_INDEX;
        return ProgramInterface::GetResourceIndex(*programObject, programInterface, name);
    }

    void GetProgramResourceName(GLuint program, GLenum programInterface, GLuint index, GLsizei bufSize, GLsizei* length,
                                GLchar* name) {
        auto& programObject = TryToGetProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (!ValidateNamedProgramResourceInterface(programInterface, __func__)) return;
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "bufSize must be non-negative."));
            return;
        }
        String resourceName;
        if (!ProgramInterface::GetResourceName(*programObject, programInterface, index, resourceName)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "index is out of range."));
            return;
        }
        CopyStr(bufSize, length, name, resourceName.c_str(), static_cast<GLsizei>(resourceName.length()));
    }

    void GetProgramResourceiv(GLuint program, GLenum programInterface, GLuint index, GLsizei propCount,
                              const GLenum* props, GLsizei bufSize, GLsizei* length, GLint* params) {
        // Every early-out below reports "nothing was written", and it has to say so before it can
        // take one: callers legitimately leave *length uninitialised and then loop to it. The CTS
        // does exactly that (gl4cProgramInterfaceQueryTests.cpp:2172 declares `GLsizei length;` and
        // walks `for (i = 0; i < length; ++i)` over a 1000-entry stack array), so an untouched
        // *length turned every error path here into a stack overrun inside the caller -
        // KHR-GL43.program_interface_query.subroutines-vertex read 0x20202020 entries and died on
        // both backends. The success path overwrites this with the real count.
        if (length) *length = 0;

        auto& programObject = TryToGetProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        if (!ProgramInterface::IsInterfaceEnum(programInterface)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Unsupported program interface."));
            return;
        }
        if (propCount <= 0 || bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                                      "propCount must be positive and bufSize "
                                                                      "non-negative."));
            return;
        }
        if (props == nullptr) return;
        // Both prop checks run BEFORE any value is produced: a property this command does
        // not know at all is INVALID_ENUM, one it knows but the interface does not carry is
        // INVALID_OPERATION (GL 4.6 Table 7.2). The two are deliberately different errors.
        for (GLsizei i = 0; i < propCount; ++i) {
            if (!ProgramInterface::IsResourceProp(props[i])) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "prop is not a valid property name."));
                return;
            }
            if (!ProgramInterface::InterfaceSupportsProp(programInterface, props[i])) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "prop is not supported for this program interface."));
                return;
            }
        }

        Vector<GLint> values;
        for (GLsizei i = 0; i < propCount; ++i) {
            if (!ProgramInterface::GetResourceProp(*programObject, programInterface, index, props[i], values)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "index is out of range."));
                return;
            }
        }
        if (params == nullptr) return;
        const GLsizei written = static_cast<GLsizei>(std::min<SizeT>(values.size(), static_cast<SizeT>(bufSize)));
        for (GLsizei i = 0; i < written; ++i) params[i] = values[i];
        if (length) *length = written;
    }

    GLint GetProgramResourceLocation(GLuint program, GLenum programInterface, const GLchar* name) {
        // Unlike the four queries above, this one and GetProgramResourceLocationIndex really
        // do require a successful link (GL 4.6 §7.3.1.3).
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return -1;
        if (!ProgramInterface::InterfaceHasLocations(programInterface)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Program interface has no locations."));
            return -1;
        }
        return ProgramInterface::GetResourceLocation(*programObject, programInterface, name);
    }

    GLint GetProgramResourceLocationIndex(GLuint program, GLenum programInterface, const GLchar* name) {
        auto& programObject = TryToGetLinkedProgramForInterfaceQuery(program, __func__);
        if (!programObject) return -1;
        if (programInterface != GL_PROGRAM_OUTPUT) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "GetProgramResourceLocationIndex only accepts GL_PROGRAM_OUTPUT."));
            return -1;
        }
        return ProgramInterface::GetResourceLocationIndex(*programObject, programInterface, name);
    }

    // GL 4.6 §7.7. Every property this reports is one the GL_ATOMIC_COUNTER_BUFFER interface
    // already carries, so this is a rename of glGetProgramResourceiv's props onto the older
    // entry point's - and the two are required to agree, which is only true while both read the
    // same model. It was a silent stub: it wrote nothing, raised nothing, and left every probe
    // reading its own uninitialised output.
    static Bool TryMapActiveAtomicCounterBufferProp(GLenum pname, GLenum& outProp) {
        switch (pname) {
        case GL_ATOMIC_COUNTER_BUFFER_BINDING:
            outProp = GL_BUFFER_BINDING;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_DATA_SIZE:
            outProp = GL_BUFFER_DATA_SIZE;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTERS:
            outProp = GL_NUM_ACTIVE_VARIABLES;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTER_INDICES:
            outProp = GL_ACTIVE_VARIABLES;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_VERTEX_SHADER:
            outProp = GL_REFERENCED_BY_VERTEX_SHADER;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_TESS_CONTROL_SHADER:
            outProp = GL_REFERENCED_BY_TESS_CONTROL_SHADER;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_TESS_EVALUATION_SHADER:
            outProp = GL_REFERENCED_BY_TESS_EVALUATION_SHADER;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_GEOMETRY_SHADER:
            outProp = GL_REFERENCED_BY_GEOMETRY_SHADER;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_FRAGMENT_SHADER:
            outProp = GL_REFERENCED_BY_FRAGMENT_SHADER;
            return true;
        case GL_ATOMIC_COUNTER_BUFFER_REFERENCED_BY_COMPUTE_SHADER:
            outProp = GL_REFERENCED_BY_COMPUTE_SHADER;
            return true;
        default:
            return false;
        }
    }

    void GetActiveAtomicCounterBufferiv(GLuint program, GLuint bufferIndex, GLenum pname, GLint* params) {
        auto& programObject = TryToGetProgramForInterfaceQuery(program, __func__);
        if (!programObject) return;
        GLenum prop = GL_NONE;
        if (!TryMapActiveAtomicCounterBufferProp(pname, prop)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname is not an active atomic counter buffer property."));
            return;
        }
        Vector<GLint> values;
        if (!ProgramInterface::GetResourceProp(*programObject, GL_ATOMIC_COUNTER_BUFFER, bufferIndex, prop, values)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "bufferIndex is not an active atomic counter buffer index."));
            return;
        }
        if (params == nullptr) return;
        // GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTER_INDICES is the only multi-value property
        // here, and the caller sized its array from _ACTIVE_ATOMIC_COUNTERS.
        for (SizeT i = 0; i < values.size(); ++i) params[i] = values[i];
    }

    // GL 4.6 §7.6.2: <storageBlockIndex> is an active shader storage block index of <program>
    // - that is, exactly what glGetProgramResourceIndex(GL_SHADER_STORAGE_BLOCK) returned.
    // Since wave 2 that index is the interface-query layer's, so this is where the one index
    // space the application sees gets turned into whatever the backend's is; the backends are
    // handed the block NAME and do their own lookup. Getting this wrong is silent: the call
    // succeeds and rebinds a DIFFERENT buffer.
    void ShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex, GLuint storageBlockBinding) {
        auto& programObject = TryToGetProgramObject(program);
        if (!programObject || !programObject->GetLinkStatus()) return;
        if (!ValidateShaderStorageBlockBinding(storageBlockBinding)) return;
        String blockName;
        if (!ProgramInterface::GetResourceName(*programObject, GL_SHADER_STORAGE_BLOCK, storageBlockIndex,
                                               blockName)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "storageBlockIndex is not an active shader storage block index."));
            return;
        }
        // Recorded before the backend call, and independently of whether a backend is even
        // present: this is the state GL_BUFFER_BINDING reports, and it is also what reseeds a
        // backend's own reflection cache after any rebuild.
        programObject->SetShaderStorageBlockBinding(blockName, storageBlockBinding);
        auto shaderStorageBlockBinding = MG_Backend::gBackendFunctionsTable.GL.ShaderStorageBlockBinding;
        if (!shaderStorageBlockBinding) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Backend does not support shader storage block binding."));
            return;
        }
        shaderStorageBlockBinding(program, blockName.c_str(), storageBlockBinding);
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
