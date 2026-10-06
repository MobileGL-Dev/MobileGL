// MobileGL - MobileGL/MG_Impl/GLImpl/Program/GL_ShaderObjectsARB.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL_ARB_shader_objects: the entry points whose meaning is not just "the core call under another
// name". A handle IS a shader or program name of the core's one shared shader/program name space
// (GL 4.6 §7.1 / §7.3): glCreateShaderObjectARB and glCreateProgramObjectARB hand out exactly what
// glCreateShader / glCreateProgram would, so every core call takes an ARB handle and every ARB call
// takes a core name. What this file adds is the part of the extension that asks "which kind of
// object is this handle" - deleting, querying and reading the info log of either kind through one
// entry point - and the one query of the current program by handle.

#include "GL_Program.h"
#include "../Getter/GL_Getter.h"

#include <MG_State/GLState/Core.h>

#include <string>

namespace MobileGL::MG_Impl::GLImpl {

    namespace {
        // The ARB tokens that have no core spelling. The rest of GetObjectParameter's pnames ARE the
        // core values (OBJECT_DELETE_STATUS_ARB == GL_DELETE_STATUS ... 0x8B80-0x8B88, and
        // ARB_vertex_shader's two attribute counts 0x8B89 / 0x8B8A).
        constexpr GLenum kProgramObjectArb = 0x8B40;
        constexpr GLenum kShaderObjectArb = 0x8B48;
        constexpr GLenum kObjectTypeArb = 0x8B4E;
        constexpr GLenum kObjectSubtypeArb = 0x8B4F;

        enum class ObjectKind { None, Program, Shader };

        ObjectKind KindOf(GLuint obj) {
            if (obj == 0) return ObjectKind::None;
            if (IsProgram(obj)) return ObjectKind::Program;
            if (IsShader(obj)) return ObjectKind::Shader;
            return ObjectKind::None;
        }

        void Fail(ErrorCode code, const char* func, const std::string& what) {
            MG_State::pGLContext->RecordError(code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", func, what));
        }
    } // namespace

    // DeleteObjectARB deletes whichever kind the handle names, with the core's deferred-deletion
    // rules (a program in use, a shader still attached). Zero is ignored, as for glDeleteShader /
    // glDeleteProgram; any other name that is not a shader or program object is INVALID_VALUE.
    void DeleteObjectARB(GLuint obj) {
        if (obj == 0) return;
        switch (KindOf(obj)) {
        case ObjectKind::Program: DeleteProgram(obj); return;
        case ObjectKind::Shader: DeleteShader(obj); return;
        case ObjectKind::None: break;
        }
        Fail(ErrorCode::InvalidValue, __func__, std::to_string(obj) + " is not a shader or program object.");
    }

    // GetHandleARB(PROGRAM_OBJECT_ARB) is the program object in use - GL_CURRENT_PROGRAM, 0 when
    // none. It is the only pname the extension defines.
    GLuint GetHandleARB(GLenum pname) {
        if (pname != kProgramObjectArb) {
            Fail(ErrorCode::InvalidEnum, __func__, "pname must be GL_PROGRAM_OBJECT_ARB.");
            return 0;
        }
        GLint current = 0;
        GetIntegerv(GL_CURRENT_PROGRAM, &current);
        return static_cast<GLuint>(current);
    }

    namespace {
        // The ARB spec's table of pnames, per kind. An unknown pname is INVALID_ENUM; a known one the
        // object's kind does not have (COMPILE_STATUS of a program, LINK_STATUS of a shader, SUBTYPE
        // of a program) is INVALID_OPERATION; a handle that names no shader or program is
        // INVALID_VALUE. True when `*out` holds the answer (every pname here is a single value).
        bool QueryObjectParameter(GLuint obj, GLenum pname, GLint* out, const char* func) {
            bool programParam = false, shaderParam = false;
            switch (pname) {
            case kObjectTypeArb:
            case GL_DELETE_STATUS:
            case GL_INFO_LOG_LENGTH: programParam = shaderParam = true; break;
            case kObjectSubtypeArb:
            case GL_COMPILE_STATUS:
            case GL_SHADER_SOURCE_LENGTH: shaderParam = true; break;
            case GL_LINK_STATUS:
            case GL_VALIDATE_STATUS:
            case GL_ATTACHED_SHADERS:
            case GL_ACTIVE_UNIFORMS:
            case GL_ACTIVE_UNIFORM_MAX_LENGTH:
            case GL_ACTIVE_ATTRIBUTES:
            case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: programParam = true; break;
            default: Fail(ErrorCode::InvalidEnum, func, "pname is not a GetObjectParameterARB parameter."); return false;
            }
            const ObjectKind kind = KindOf(obj);
            if (kind == ObjectKind::None) {
                Fail(ErrorCode::InvalidValue, func, std::to_string(obj) + " is not a shader or program object.");
                return false;
            }
            if ((kind == ObjectKind::Program && !programParam) || (kind == ObjectKind::Shader && !shaderParam)) {
                Fail(ErrorCode::InvalidOperation, func, "pname is not a parameter of this kind of object.");
                return false;
            }
            if (pname == kObjectTypeArb) {
                *out = static_cast<GLint>(kind == ObjectKind::Program ? kProgramObjectArb : kShaderObjectArb);
            } else if (pname == kObjectSubtypeArb) {
                GetShaderiv(obj, GL_SHADER_TYPE, out);
            } else if (kind == ObjectKind::Program) {
                GetProgramiv(obj, pname, out);
            } else {
                GetShaderiv(obj, pname, out);
            }
            return true;
        }
    } // namespace

    void GetObjectParameterivARB(GLuint obj, GLenum pname, GLint* params) {
        GLint value = 0;
        if (QueryObjectParameter(obj, pname, &value, __func__) && params != nullptr) *params = value;
    }

    void GetObjectParameterfvARB(GLuint obj, GLenum pname, GLfloat* params) {
        GLint value = 0;
        if (QueryObjectParameter(obj, pname, &value, __func__) && params != nullptr) *params = static_cast<GLfloat>(value);
    }

    void GetInfoLogARB(GLuint obj, GLsizei maxLength, GLsizei* length, GLchar* infoLog) {
        switch (KindOf(obj)) {
        case ObjectKind::Program: GetProgramInfoLog(obj, maxLength, length, infoLog); return;
        case ObjectKind::Shader: GetShaderInfoLog(obj, maxLength, length, infoLog); return;
        case ObjectKind::None: break;
        }
        Fail(ErrorCode::InvalidValue, __func__, std::to_string(obj) + " is not a shader or program object.");
    }

} // namespace MobileGL::MG_Impl::GLImpl
