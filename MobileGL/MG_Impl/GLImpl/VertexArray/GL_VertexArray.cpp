// MobileGL - MobileGL/MG_Impl/GLImpl/VertexArray/GL_VertexArray.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_VertexArray.h"
#include "Validators.h"
#include <MG_Impl/GLImpl/Buffer/Validators.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToMG/DataTypeConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        static bool ValidateCurrentVertexAttribIndex(GLuint index, const char* funcName) {
            if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return false;
            if (index == 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                                 "Generic vertex attribute 0 current value cannot be modified."));
                return false;
            }
            return true;
        }

        static bool TryGetVertexAttribute(GLuint index, const MG_State::GLState::VertexAttribute** outAttr) {
            if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return false;

            auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "No vertex array object is bound."));
                return false;
            }

            *outAttr = &vao->GetAttribute(index);
            return true;
        }

        static bool IsCurrentVertexAttribQuery(GLenum pname) {
            return pname == GL_CURRENT_VERTEX_ATTRIB;
        }

        static bool ValidateVertexBindingIndex(GLuint bindingindex, const char* funcName) {
            if (bindingindex >= MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIB_BINDINGS) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                                 "bindingindex exceeds GL_MAX_VERTEX_ATTRIB_BINDINGS."));
                return false;
            }
            return true;
        }

        static SharedPtr<MG_State::GLState::VertexArrayObject> GetBoundVertexArrayOrError(const char* funcName) {
            auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName, "No vertex array object is bound."));
            }
            return vao;
        }

        static bool ValidateVertexAttribPname(GLenum pname) {
            switch (pname) {
            case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
            case GL_VERTEX_ATTRIB_ARRAY_SIZE:
            case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
            case GL_VERTEX_ATTRIB_ARRAY_TYPE:
            case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
            case GL_CURRENT_VERTEX_ATTRIB:
            case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
            case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
            case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            case GL_VERTEX_ATTRIB_ARRAY_POINTER:
                return true;
            default:
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "Unsupported vertex attrib pname: " + std::to_string(pname)));
                return false;
            }
        }
    } // namespace

    SharedPtr<MG_State::GLState::VertexArrayObject> GetNamedVertexArrayObject_State(GLuint vaobj,
                                                                                   const char* caller) {
        if (!VertexArrayImpl::ValidateVertexArrayName(vaobj)) return nullptr;
        if (!VertexArrayImpl::ValidateVertexArrayObject(vaobj)) return nullptr;
        return MG_State::pGLContext->GetVertexArrayObject(vaobj);
    }

    SharedPtr<MG_State::GLState::BufferObject> GetVertexArrayBufferObject_State(GLuint buffer, const char* caller) {
        if (!BufferImpl::ValidateBufferName(buffer, true)) return nullptr;
        if (buffer == 0) return nullptr;

        auto& bufferObject = MG_State::pGLContext->GetBufferObject(buffer);
        if (!bufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Buffer object {} does not exist.", buffer)));
            return nullptr;
        }
        return bufferObject;
    }

    void DisableVertexAttribArray_State(GLuint index) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl",
                                                                           "EnableVertexAttribArray_State",
                                                                           "No vertex array object is bound."));
            return;
        }

        vao->DisableAttribute(index);
    }

    void EnableVertexAttribArray_State(GLuint index) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl",
                                                                           "EnableVertexAttribArray_State",
                                                                           "No vertex array object is bound."));
            return;
        }

        vao->EnableAttribute(index);
    }

    // TODO: implement GL_BGRA support
    void VertexAttribIPointer_State(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        if (!VertexArrayImpl::ValidateVertexAttribPointerParams(index, size, dataType, stride)) return;

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "VertexAttribPointer_State",
                                                                          "No vertex array object is bound."));
            return;
        }

        auto& vboSlot = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex);
        auto& vbo = vboSlot.GetBoundObject();

        auto offset = reinterpret_cast<SizeT>(pointer);

        vao->SetAttributeFormat(index, size, dataType, false, stride, offset, true);
        vao->BindAttributeBuffer(index, vbo);
    }

    // TODO: implement GL_BGRA support
    void VertexAttribPointer_State(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                   const void* pointer) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        if (!VertexArrayImpl::ValidateVertexAttribPointerParams(index, size, dataType, stride)) return;

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "VertexAttribPointer_State",
                                                                          "No vertex array object is bound."));
            return;
        }

        auto& vboSlot = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::Vertex);
        auto& vbo = vboSlot.GetBoundObject();

        SizeT offset = reinterpret_cast<SizeT>(pointer);

        vao->SetAttributeFormat(index, size, dataType, normalized, stride, offset, false);
        vao->BindAttributeBuffer(index, vbo);
    }

    void BindVertexArray_State(GLuint array) {
        if (array == 0) {
            MG_State::pGLContext->BindVertexArray(0);
            return;
        }

        if (!VertexArrayImpl::ValidateVertexArrayName(array)) return;

        if (!MG_State::pGLContext->ValidateVertexArrayObject(array)) {
            MG_State::pGLContext->CreateVertexArrayObject(array);
        }

        MG_State::pGLContext->BindVertexArray(array);
    }

    void DeleteVertexArrays_State(GLsizei n, const GLuint* arrays) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteVertexArrays_State", "n must be non-negative."));
            return;
        }

        for (GLsizei i = 0; i < n; ++i) {
            GLuint vao = arrays[i];
            if (vao == 0) continue;

            if (!VertexArrayImpl::ValidateVertexArrayName(vao)) continue;

            if (MG_State::pGLContext->GetBoundVertexArray() &&
                MG_State::pGLContext->GetBoundVertexArray() == MG_State::pGLContext->GetVertexArrayObject(vao)) {
                MG_State::pGLContext->BindVertexArray(0);
            }

            MG_State::pGLContext->MarkVertexArrayForDeletion(vao);
        }
    }

    void GenVertexArrays_State(GLsizei n, GLuint* arrays) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenVertexArrays_State", "n must be non-negative."));
            return;
        }

        Vector<Uint> vaos;
        MG_State::pGLContext->GenVertexArrayNames(n, vaos);
        Memcpy(arrays, vaos.data(), n * sizeof(GLuint));
    }

    void CreateVertexArrays_State(GLsizei n, GLuint* arrays) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateVertexArrays_State", "n must be non-negative."));
            return;
        }

        Vector<Uint> vaos;
        MG_State::pGLContext->GenVertexArrayNames(n, vaos);
        for (GLsizei i = 0; i < n; ++i) {
            MG_State::pGLContext->CreateVertexArrayObject(vaos[i]);
            arrays[i] = vaos[i];
        }
    }

    void DisableVertexArrayAttrib_State(GLuint vaobj, GLuint index) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "DisableVertexArrayAttrib_State");
        if (!vao) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        vao->DisableAttribute(index);
    }

    void EnableVertexArrayAttrib_State(GLuint vaobj, GLuint index) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "EnableVertexArrayAttrib_State");
        if (!vao) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        vao->EnableAttribute(index);
    }

    void VertexArrayElementBuffer_State(GLuint vaobj, GLuint buffer) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayElementBuffer_State");
        if (!vao) return;
        auto bufferObject = GetVertexArrayBufferObject_State(buffer, "VertexArrayElementBuffer_State");
        if (buffer != 0 && !bufferObject) return;
        vao->GetIndexBufferBindingSlot().Bind(bufferObject);
    }

    static void VertexBufferBinding_State(const SharedPtr<MG_State::GLState::VertexArrayObject>& vao,
                                          GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride,
                                          const char* caller) {
        if (!ValidateVertexBindingIndex(bindingindex, caller)) return;
        if (offset < 0 || stride < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "offset and stride must be non-negative."));
            return;
        }
        auto bufferObject = GetVertexArrayBufferObject_State(buffer, caller);
        if (buffer != 0 && !bufferObject) return;

        vao->SetBindingBuffer(bindingindex, bufferObject, static_cast<SizeT>(offset), stride);
    }

    void VertexArrayVertexBuffer_State(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset,
                                       GLsizei stride) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayVertexBuffer_State");
        if (!vao) return;
        VertexBufferBinding_State(vao, bindingindex, buffer, offset, stride, "VertexArrayVertexBuffer_State");
    }

    void VertexArrayVertexBuffers_State(GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers,
                                        const GLintptr* offsets, const GLsizei* strides) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayVertexBuffers_State");
        if (!vao) return;
        for (GLsizei i = 0; i < count; ++i) {
            if (!buffers) {
                VertexBufferBinding_State(vao, first + i, 0, 0, 16, "VertexArrayVertexBuffers_State");
            } else {
                VertexBufferBinding_State(vao, first + i, buffers[i], offsets ? offsets[i] : 0,
                                          strides ? strides[i] : 16, "VertexArrayVertexBuffers_State");
            }
        }
    }

    static void VertexAttribFormatSeparate_State(const SharedPtr<MG_State::GLState::VertexArrayObject>& vao,
                                                 GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                                 GLuint relativeoffset, Bool isInteger, const char* caller) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(attribindex)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        if (!VertexArrayImpl::ValidateVertexAttribPointerParams(attribindex, size, dataType, 0)) return;

        vao->SetAttributeFormatSeparate(attribindex, size, dataType, normalized, isInteger, relativeoffset);
    }

    void VertexArrayAttribFormat_State(GLuint vaobj, GLuint attribindex, GLint size, GLenum type,
                                       GLboolean normalized, GLuint relativeoffset) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayAttribFormat_State");
        if (!vao) return;
        VertexAttribFormatSeparate_State(vao, attribindex, size, type, normalized, relativeoffset, false,
                                         "VertexArrayAttribFormat_State");
    }

    void VertexArrayAttribIFormat_State(GLuint vaobj, GLuint attribindex, GLint size, GLenum type,
                                        GLuint relativeoffset) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayAttribIFormat_State");
        if (!vao) return;
        VertexAttribFormatSeparate_State(vao, attribindex, size, type, GL_FALSE, relativeoffset, true,
                                         "VertexArrayAttribIFormat_State");
    }

    void VertexArrayAttribBinding_State(GLuint vaobj, GLuint attribindex, GLuint bindingindex) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayAttribBinding_State");
        if (!vao) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(attribindex)) return;
        if (!ValidateVertexBindingIndex(bindingindex, "VertexArrayAttribBinding_State")) return;
        vao->SetAttributeBinding(attribindex, bindingindex);
    }

    void VertexArrayBindingDivisor_State(GLuint vaobj, GLuint bindingindex, GLuint divisor) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayBindingDivisor_State");
        if (!vao) return;
        if (!ValidateVertexBindingIndex(bindingindex, "VertexArrayBindingDivisor_State")) return;
        vao->SetBindingDivisor(bindingindex, divisor);
    }

    GLboolean IsVertexArray_State(GLuint array) {
        if (array == 0) return GL_FALSE;
        return MG_State::pGLContext->ValidateVertexArrayObject(array) ? GL_TRUE : GL_FALSE;
    }

    void VertexAttribDivisor_State(GLuint index, GLuint divisor) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "VertexAttribDivisor_State",
                                                                          "No vertex array object is bound."));
            return;
        }

        vao->SetAttributeDivisor(index, divisor);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void VertexAttrib1f(GLuint index, GLfloat x) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeFloat(index, {x, 0.0f, 0.0f, 1.0f});
    }

    void VertexAttrib1fv(GLuint index, const GLfloat* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib1f(index, v[0]);
    }

    void VertexAttrib2f(GLuint index, GLfloat x, GLfloat y) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeFloat(index, {x, y, 0.0f, 1.0f});
    }

    void VertexAttrib2fv(GLuint index, const GLfloat* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib2f(index, v[0], v[1]);
    }

    void VertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeFloat(index, {x, y, z, 1.0f});
    }

    void VertexAttrib3fv(GLuint index, const GLfloat* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib3f(index, v[0], v[1], v[2]);
    }

    void VertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeFloat(index, {x, y, z, w});
    }

    void VertexAttrib4fv(GLuint index, const GLfloat* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib4f(index, v[0], v[1], v[2], v[3]);
    }

    void VertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeInt(index, {x, y, z, w});
    }

    void VertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) {
        if (!ValidateCurrentVertexAttribIndex(index, __func__)) return;
        MG_State::pGLContext->SetCurrentVertexAttributeUint(index, {x, y, z, w});
    }

    void VertexAttribI4iv(GLuint index, const GLint* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribI4i(index, v[0], v[1], v[2], v[3]);
    }

    void VertexAttribI4uiv(GLuint index, const GLuint* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
    }

    void VertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w) {
        constexpr float kInv255 = 1.0f / 255.0f;
        VertexAttrib4f(index, x * kInv255, y * kInv255, z * kInv255, w * kInv255);
    }

    void VertexAttrib4Nubv(GLuint index, const GLubyte* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib4Nub(index, v[0], v[1], v[2], v[3]);
    }

    void VertexAttrib4ubv(GLuint index, const GLubyte* v) {
        if (!v) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttrib4f(index, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]),
                       static_cast<GLfloat>(v[3]));
    }

    void GetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        if (!ValidateVertexAttribPname(pname)) return;

        if (IsCurrentVertexAttribQuery(pname)) {
            const auto& current = MG_State::pGLContext->GetCurrentVertexAttribute(index);
            params[0] = current.floatValue[0];
            params[1] = current.floatValue[1];
            params[2] = current.floatValue[2];
            params[3] = current.floatValue[3];
            return;
        }

        const MG_State::GLState::VertexAttribute* attr = nullptr;
        if (!TryGetVertexAttribute(index, &attr)) return;

        switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
            params[0] = attr->Enabled ? 1.0f : 0.0f;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE:
            params[0] = static_cast<GLfloat>(attr->Size);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
            params[0] = static_cast<GLfloat>(attr->Stride);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE:
            params[0] = static_cast<GLfloat>(MG_Util::ConvertDataTypeToGLEnum(attr->Type));
            return;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
            params[0] = attr->Normalized ? 1.0f : 0.0f;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
            params[0] = attr->Buffer ? static_cast<GLfloat>(attr->Buffer->GetExternalIndex()) : 0.0f;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
            params[0] = attr->IsInteger ? 1.0f : 0.0f;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            params[0] = static_cast<GLfloat>(attr->Divisor);
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported float vertex attrib pname: " + std::to_string(pname)));
            return;
        }
    }

    void GetVertexAttribiv(GLuint index, GLenum pname, GLint* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        if (!ValidateVertexAttribPname(pname)) return;

        if (IsCurrentVertexAttribQuery(pname)) {
            const auto& current = MG_State::pGLContext->GetCurrentVertexAttribute(index);
            params[0] = current.intValue[0];
            params[1] = current.intValue[1];
            params[2] = current.intValue[2];
            params[3] = current.intValue[3];
            return;
        }

        const MG_State::GLState::VertexAttribute* attr = nullptr;
        if (!TryGetVertexAttribute(index, &attr)) return;

        switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
            params[0] = attr->Enabled ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE:
            params[0] = attr->Size;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
            params[0] = attr->Stride;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE:
            params[0] = static_cast<GLint>(MG_Util::ConvertDataTypeToGLEnum(attr->Type));
            return;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
            params[0] = attr->Normalized ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
            params[0] = attr->Buffer ? static_cast<GLint>(attr->Buffer->GetExternalIndex()) : 0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
            params[0] = attr->IsInteger ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            params[0] = static_cast<GLint>(attr->Divisor);
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported integer vertex attrib pname: " + std::to_string(pname)));
            return;
        }
    }

    void GetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer) {
        if (!pointer) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "pointer cannot be null."));
            return;
        }
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname must be GL_VERTEX_ATTRIB_ARRAY_POINTER."));
            return;
        }

        auto& vao = MG_State::pGLContext->GetBoundVertexArray();
        if (!vao) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "No vertex array object is bound."));
            return;
        }

        const auto& attr = vao->GetAttribute(index);
        *pointer = reinterpret_cast<void*>(attr.Offset);
    }

    void GetVertexAttribIiv(GLuint index, GLenum pname, GLint* params) {
        GetVertexAttribiv(index, pname, params);
    }

    void GetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        if (!ValidateVertexAttribPname(pname)) return;

        if (IsCurrentVertexAttribQuery(pname)) {
            const auto& current = MG_State::pGLContext->GetCurrentVertexAttribute(index);
            params[0] = current.uintValue[0];
            params[1] = current.uintValue[1];
            params[2] = current.uintValue[2];
            params[3] = current.uintValue[3];
            return;
        }

        GLint signedParams[4] = {};
        GetVertexAttribiv(index, pname, signedParams);
        params[0] = static_cast<GLuint>(signedParams[0]);
        params[1] = static_cast<GLuint>(signedParams[1]);
        params[2] = static_cast<GLuint>(signedParams[2]);
        params[3] = static_cast<GLuint>(signedParams[3]);
    }

    void CreateVertexArrays(GLsizei n, GLuint* arrays) {
        CreateVertexArrays_State(n, arrays);
    }

    void DisableVertexArrayAttrib(GLuint vaobj, GLuint index) {
        DisableVertexArrayAttrib_State(vaobj, index);
    }

    void EnableVertexArrayAttrib(GLuint vaobj, GLuint index) {
        EnableVertexArrayAttrib_State(vaobj, index);
    }

    void VertexArrayElementBuffer(GLuint vaobj, GLuint buffer) {
        VertexArrayElementBuffer_State(vaobj, buffer);
    }

    void VertexArrayVertexBuffer(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
        VertexArrayVertexBuffer_State(vaobj, bindingindex, buffer, offset, stride);
    }

    void VertexArrayAttribFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                 GLuint relativeoffset) {
        VertexArrayAttribFormat_State(vaobj, attribindex, size, type, normalized, relativeoffset);
    }

    void VertexArrayAttribIFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
        VertexArrayAttribIFormat_State(vaobj, attribindex, size, type, relativeoffset);
    }

    void VertexArrayAttribBinding(GLuint vaobj, GLuint attribindex, GLuint bindingindex) {
        VertexArrayAttribBinding_State(vaobj, attribindex, bindingindex);
    }

    void VertexArrayBindingDivisor(GLuint vaobj, GLuint bindingindex, GLuint divisor) {
        VertexArrayBindingDivisor_State(vaobj, bindingindex, divisor);
    }

    void VertexArrayVertexBuffers(GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers,
                                  const GLintptr* offsets, const GLsizei* strides) {
        VertexArrayVertexBuffers_State(vaobj, first, count, buffers, offsets, strides);
    }

    void BindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
        auto vao = GetBoundVertexArrayOrError("BindVertexBuffer");
        if (!vao) return;
        VertexBufferBinding_State(vao, bindingindex, buffer, offset, stride, "BindVertexBuffer");
    }

    void BindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                           const GLsizei* strides) {
        auto vao = GetBoundVertexArrayOrError("BindVertexBuffers");
        if (!vao) return;
        for (GLsizei i = 0; i < count; ++i) {
            if (!buffers) {
                VertexBufferBinding_State(vao, first + i, 0, 0, 16, "BindVertexBuffers");
            } else {
                VertexBufferBinding_State(vao, first + i, buffers[i], offsets ? offsets[i] : 0,
                                          strides ? strides[i] : 16, "BindVertexBuffers");
            }
        }
    }

    void VertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset) {
        auto vao = GetBoundVertexArrayOrError("VertexAttribFormat");
        if (!vao) return;
        VertexAttribFormatSeparate_State(vao, attribindex, size, type, normalized, relativeoffset, false,
                                         "VertexAttribFormat");
    }

    void VertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
        auto vao = GetBoundVertexArrayOrError("VertexAttribIFormat");
        if (!vao) return;
        VertexAttribFormatSeparate_State(vao, attribindex, size, type, GL_FALSE, relativeoffset, true,
                                         "VertexAttribIFormat");
    }

    void VertexAttribBinding(GLuint attribindex, GLuint bindingindex) {
        auto vao = GetBoundVertexArrayOrError("VertexAttribBinding");
        if (!vao) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(attribindex)) return;
        if (!ValidateVertexBindingIndex(bindingindex, "VertexAttribBinding")) return;
        vao->SetAttributeBinding(attribindex, bindingindex);
    }

    void VertexBindingDivisor(GLuint bindingindex, GLuint divisor) {
        auto vao = GetBoundVertexArrayOrError("VertexBindingDivisor");
        if (!vao) return;
        if (!ValidateVertexBindingIndex(bindingindex, "VertexBindingDivisor")) return;
        vao->SetBindingDivisor(bindingindex, divisor);
    }

    void VertexAttribDivisor(GLuint index, GLuint divisor) {
        VertexAttribDivisor_State(index, divisor);
    }

    GLboolean IsVertexArray(GLuint array) {
        return IsVertexArray_State(array);
    }

    void DisableVertexAttribArray(GLuint index) {
        DisableVertexAttribArray_State(index);
    }

    void EnableVertexAttribArray(GLuint index) {
        EnableVertexAttribArray_State(index);
    }

    void VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
        VertexAttribIPointer_State(index, size, type, stride, pointer);
    }

    void VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                             const void* pointer) {
        VertexAttribPointer_State(index, size, type, normalized, stride, pointer);
    }

    void BindVertexArray(GLuint array) {
        BindVertexArray_State(array);
    }

    void DeleteVertexArrays(GLsizei n, const GLuint* arrays) {
        DeleteVertexArrays_State(n, arrays);
    }

    void GenVertexArrays(GLsizei n, GLuint* arrays) {
        GenVertexArrays_State(n, arrays);
    }
} // namespace MobileGL::MG_Impl::GLImpl
