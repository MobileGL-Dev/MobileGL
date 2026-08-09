// MobileGL - MobileGL/MG_Impl/GLImpl/VertexArray/GL_VertexArray.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_VertexArray.h"
#include "Validators.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Buffer/Validators.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToMG/DataTypeConverter.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // GL 3.3 Core signed normalized fixed-point -> float (spec §2.1.2 Eq 2.2):
        //   f = (2c + 1) / (2^b - 1)
        // This maps the FULL signed range [-2^(b-1), 2^(b-1)-1] onto exactly [-1, 1] (so -128 -> -1.0
        // and 127 -> +1.0 with no clamp), and consequently cannot represent 0 exactly (0 -> 1/(2^b-1)).
        // NOTE: GL 4.2 later switched signed normalization to f = max(c/(2^(b-1)-1), -1); do NOT use that
        // form here -- it is not GL 3.3 Core. Unsigned normalization (f = c/(2^b-1)) is unchanged.
        // The bit width fixes the arithmetic type: 8/16-bit stay exact in int/float, but the 32-bit forms
        // must use double because 2*INT_MAX overflows int32 and neither 2^32-1 nor 2^31-1 is float-exact.
        constexpr GLfloat NormalizeSignedByte(GLbyte c) {   // b = 8,  divisor 2^8 - 1  = 255
            return (2 * static_cast<int>(c) + 1) / 255.0f;
        }
        constexpr GLfloat NormalizeSignedShort(GLshort c) { // b = 16, divisor 2^16 - 1 = 65535
            return (2 * static_cast<int>(c) + 1) / 65535.0f;
        }
        constexpr GLfloat NormalizeSignedInt(GLint c) {     // b = 32, divisor 2^32 - 1 (double!)
            return static_cast<GLfloat>((2.0 * static_cast<double>(c) + 1.0) / 4294967295.0);
        }
        constexpr GLfloat NormalizeUnsignedShort(GLushort c) { // b = 16
            return static_cast<GLfloat>(c) / 65535.0f;
        }
        constexpr GLfloat NormalizeUnsignedInt(GLuint c) {     // b = 32 (double!)
            return static_cast<GLfloat>(static_cast<double>(c) / 4294967295.0);
        }
        // (b = 8 unsigned normalization is VertexAttrib4Nub's  x * (1/255).)

        // Sign-extend a `bits`-wide two's-complement field held in the low bits of `field`.
        constexpr GLint SignExtendField(GLuint field, int bits) {
            const GLuint signBit = 1u << (bits - 1);
            return (field & signBit) ? static_cast<GLint>(field | (~0u << bits)) : static_cast<GLint>(field);
        }

        // Decode one GL_INT_/GL_UNSIGNED_INT_2_10_10_10_REV packed word into four float components.
        // The _REV layout packs x in bits [0..9], y in [10..19], z in [20..29], w in [30..31]; x/y/z
        // are 10-bit fields and w is a 2-bit field. Signed fields are two's-complement, and normalized
        // conversion uses the GL 3.3 (2c+1)/(2^b-1) form (matching NormalizeSigned* above), NOT the
        // GL 4.2 clamp form.
        Array<GLfloat, 4> DecodePacked2101010(GLuint value, bool signedType, bool normalized) {
            const GLuint fx = value & 0x3FFu;
            const GLuint fy = (value >> 10) & 0x3FFu;
            const GLuint fz = (value >> 20) & 0x3FFu;
            const GLuint fw = (value >> 30) & 0x3u;
            if (signedType) {
                const GLint sx = SignExtendField(fx, 10);
                const GLint sy = SignExtendField(fy, 10);
                const GLint sz = SignExtendField(fz, 10);
                const GLint sw = SignExtendField(fw, 2);
                if (normalized) {
                    return {(2 * sx + 1) / 1023.0f, (2 * sy + 1) / 1023.0f, (2 * sz + 1) / 1023.0f,
                            (2 * sw + 1) / 3.0f};
                }
                return {static_cast<GLfloat>(sx), static_cast<GLfloat>(sy), static_cast<GLfloat>(sz),
                        static_cast<GLfloat>(sw)};
            }
            if (normalized) {
                return {fx / 1023.0f, fy / 1023.0f, fz / 1023.0f, fw / 3.0f};
            }
            return {static_cast<GLfloat>(fx), static_cast<GLfloat>(fy), static_cast<GLfloat>(fz),
                    static_cast<GLfloat>(fw)};
        }

        static bool ValidateCurrentVertexAttribIndex(GLuint index, const char* funcName) {
            // GL 3.3 core 2.7: VertexAttrib* sets the current value of ANY generic attribute,
            // including index 0 - only an out-of-range index is an error (INVALID_VALUE).
            // "Attribute 0 is immutable" was legacy immediate-mode lore; rejecting it broke GL
            // CTS's per-case state reset, which writes vertexAttrib4f(0, 0,0,0,1) after every case.
            static_cast<void>(funcName);
            return VertexArrayImpl::ValidateVertexAttributeIndex(index);
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

        // The two ARB_vertex_attrib_binding per-attribute queries. They do not live on the
        // resolved VertexAttribute (which is the flat, already-combined view) but on the VAO's
        // binding-point mapping, so they need the object, not the attribute.
        static bool TryGetVertexAttribBindingQuery(GLuint index, GLenum pname, GLint& out) {
            if (pname != GL_VERTEX_ATTRIB_BINDING && pname != GL_VERTEX_ATTRIB_RELATIVE_OFFSET) return false;
            const auto& vao = MG_State::pGLContext->GetBoundVertexArray();
            if (!vao) {
                out = 0;
                return true;
            }
            out = pname == GL_VERTEX_ATTRIB_BINDING ? static_cast<GLint>(vao->GetAttributeBindingIndex(index))
                                                    : static_cast<GLint>(vao->GetAttributeRelativeOffset(index));
            return true;
        }

        // The stride a pointer-style call gives its binding point: the argument when it is non-zero,
        // otherwise the tightly packed element size (GL 4.6 core 10.3.2). A packed 2_10_10_10 or
        // 10F_11F_11F attribute is one 32-bit word regardless of its component count.
        static int EffectiveVertexStride(GLsizei stride, GLint size, GLenum type) {
            if (stride != 0) return static_cast<int>(stride);
            switch (type) {
            case GL_INT_2_10_10_10_REV:
            case GL_UNSIGNED_INT_2_10_10_10_REV:
            case GL_UNSIGNED_INT_10F_11F_11F_REV:
                return 4;
            default:
                break;
            }
            return static_cast<int>(size * MG_Util::GetGLTypeSize(type));
        }

        // glBindVertexBuffers / glVertexArrayVertexBuffers take a range of binding points, and a
        // range that runs past the last one is INVALID_OPERATION rather than the INVALID_VALUE a
        // single out-of-range index gets (GL 4.6 core 10.3.1).
        static bool ValidateVertexBindingRange(GLuint first, GLsizei count, const char* funcName) {
            if (count < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName, "count must be non-negative."));
                return false;
            }
            if (static_cast<Uint64>(first) + static_cast<Uint64>(count) >
                VertexArrayImpl::GetMaxVertexAttribBindings()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                                 "first + count exceeds GL_MAX_VERTEX_ATTRIB_BINDINGS."));
                return false;
            }
            return true;
        }

        static bool ValidateVertexBindingIndex(GLuint bindingindex, const char* funcName) {
            if (bindingindex >= VertexArrayImpl::GetMaxVertexAttribBindings()) {
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
            // Core since GL 4.1 (ARB_vertex_attrib_64bit). It was rejected while no attribute could
            // ever be long; now that IsLong is real state the pname has to be accepted.
            case GL_VERTEX_ATTRIB_ARRAY_LONG:
            case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            case GL_VERTEX_ATTRIB_ARRAY_POINTER:
            // ARB_vertex_attrib_binding (core since GL 4.3). The binding-point view is real
            // state on the VAO (GetAttributeBindingIndex / GetAttributeRelativeOffset), so
            // both of its per-attribute queries are answerable.
            case GL_VERTEX_ATTRIB_BINDING:
            case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
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
        // Name zero is not a vertex array object in a core profile: it names the default vertex
        // array, which the by-name (direct state access) entry points never accept. MobileGL keeps a
        // real object at index 0 for the compatibility paths, so the generic name validation below
        // would otherwise let it through (GL 4.6 core 10.3.1).
        if (vaobj == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Vertex array name 0 is not a vertex array object."));
            return nullptr;
        }
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

    void VertexAttribIPointer_State(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        // Integer path: never normalized, never BGRA/packed (the validator rejects those).
        if (!VertexArrayImpl::ValidateVertexAttribFormat(index, size, type, dataType, false, stride, true)) return;

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

        vao->SetAttributeFormat(index, size, dataType, false, stride, offset, true, false);
        vao->BindAttributeBuffer(index, vbo);
        vao->MirrorPointerIntoBinding(index, vbo, offset, EffectiveVertexStride(stride, size, type));
    }

    void VertexAttribPointer_State(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                   const void* pointer) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        if (!VertexArrayImpl::ValidateVertexAttribFormat(index, size, type, dataType, normalized == GL_TRUE, stride, false))
            return;

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

        // GL_BGRA is a 4-component reversed-order format; store 4 components and mark it BGRA so the
        // backend can pick the reversed VkFormat / pass GL_BGRA through to a GLES driver.
        const bool isBgra = (size == static_cast<GLint>(GL_BGRA));
        const int effectiveSize = isBgra ? 4 : size;
        vao->SetAttributeFormat(index, effectiveSize, dataType, normalized, stride, offset, false, isBgra);
        vao->BindAttributeBuffer(index, vbo);
        vao->MirrorPointerIntoBinding(index, vbo, offset, EffectiveVertexStride(stride, effectiveSize, type));
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

            // GL 3.3 core 2.10: unknown names are silently ignored on delete; the shared bind-path
            // validator would record INVALID_OPERATION instead.
            if (!MG_State::pGLContext->ValidateVertexArrayName(vao)) continue;

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
        if (static_cast<Uint>(stride) > VertexArrayImpl::GetMaxVertexAttribStride()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "stride exceeds GL_MAX_VERTEX_ATTRIB_STRIDE."));
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
        if (!ValidateVertexBindingRange(first, count, "VertexArrayVertexBuffers_State")) return;
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
        static_cast<void>(caller);
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(attribindex)) return;

        DataType dataType = MG_Util::ConvertGLEnumToDataType(type);
        // The separate-format entry points take the same size/type rules as the pointer ones,
        // GL_BGRA included, so they need the full format validation rather than the pointer-only
        // subset - that one reports GL_BGRA as an out-of-range size.
        if (!VertexArrayImpl::ValidateVertexAttribFormat(attribindex, size, type, dataType, normalized == GL_TRUE, 0,
                                                         isInteger))
            return;
        if (!VertexArrayImpl::ValidateVertexAttribRelativeOffset(relativeoffset)) return;

        const Bool isBgra = (size == static_cast<GLint>(GL_BGRA));
        vao->SetAttributeFormatSeparate(attribindex, isBgra ? 4 : size, dataType, normalized, isInteger,
                                        relativeoffset, isBgra);
    }

    // The long (64-bit) attribute format: the values reach the shader as doubles, unconverted
    // (GL 4.6 core 10.3.2). ValidateVertexAttribLFormat has already pinned type to GL_DOUBLE, so the
    // recorded DataType is always Float64 - what IsLong adds is that this is the *unconverted* form,
    // as opposed to VertexAttribFormat(GL_DOUBLE), which asks for a float conversion.
    //
    // Whether the backend can feed it is detected, not assumed: DirectVulkan needs shaderFloat64,
    // and DirectGLES can never have it at all. A backend without it declines here, loudly - GL error
    // plus a log line naming the reason - rather than accepting state no draw could honour and
    // rendering garbage. The matching startup POST row is in MG_Util/SelfTest/DriverPost.cpp.
    static void VertexAttribLFormatSeparate_State(const SharedPtr<MG_State::GLState::VertexArrayObject>& vao,
                                                  GLuint attribindex, GLint size, GLenum type,
                                                  GLuint relativeoffset) {
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(attribindex)) return;
        if (!VertexArrayImpl::ValidateVertexAttribLFormat(attribindex, size, type)) return;
        if (!VertexArrayImpl::ValidateVertexAttribRelativeOffset(relativeoffset)) return;

        if (!MG_Backend::pActiveBackendObject ||
            !MG_Backend::pActiveBackendObject->GetDynamicParameters().SupportsFloat64VertexAttributes) {
            MGLOG_I("VertexAttribLFormat: attribute %u asked for a 64-bit (GL_DOUBLE) format, but this "
                    "backend has no double-precision vertex attribute support - see the "
                    "\"64-bit vertex attributes\" / \"shaderFloat64\" POST row for what that costs",
                    attribindex);
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "VertexAttribLFormat",
                                             "64-bit vertex attributes are not supported by this backend."));
            return;
        }

        vao->SetAttributeFormatSeparate(attribindex, size, MG_Util::ConvertGLEnumToDataType(type),
                                        /*normalized: */ false, /*isInteger: */ false, relativeoffset,
                                        /*isBgra: */ false, /*isLong: */ true);
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

    // Shared body for glVertexAttribP{1,2,3,4}ui(v). These set the CURRENT generic vertex attribute
    // value (they are the packed members of the immediate VertexAttrib* family, not the array-format
    // path), so they take the float current-value funnel. The single packed word is always fully
    // decoded, but only the first `componentCount` components are written; the rest keep the generic
    // attribute defaults (0, 0, 0, 1). type must be one of the two 2_10_10_10_REV packed enums.
    static void VertexAttribP_Common(GLuint index, GLenum type, GLboolean normalized, GLuint value,
                                     int componentCount, const char* funcName) {
        if (!ValidateCurrentVertexAttribIndex(index, funcName)) return;

        bool signedType;
        if (type == GL_INT_2_10_10_10_REV) {
            signedType = true;
        } else if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
            signedType = false;
        } else {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", funcName,
                                             "glVertexAttribP*ui type must be GL_INT_2_10_10_10_REV or "
                                             "GL_UNSIGNED_INT_2_10_10_10_REV; got " +
                                                 MG_Util::ConvertGLEnumToString(type) + "."));
            return;
        }

        const Array<GLfloat, 4> decoded = DecodePacked2101010(value, signedType, normalized == GL_TRUE);
        Array<GLfloat, 4> out = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < componentCount; ++i) out[i] = decoded[i];
        MG_State::pGLContext->SetCurrentVertexAttributeFloat(index, out);
    }

    void VertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
        VertexAttribP_Common(index, type, normalized, value, 1, __func__);
    }
    void VertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
        VertexAttribP_Common(index, type, normalized, value, 2, __func__);
    }
    void VertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
        VertexAttribP_Common(index, type, normalized, value, 3, __func__);
    }
    void VertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value) {
        VertexAttribP_Common(index, type, normalized, value, 4, __func__);
    }

    // The *uiv forms dereference a pointer to a SINGLE packed GLuint (never an array of N words).
    void VertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribP_Common(index, type, normalized, value[0], 1, __func__);
    }
    void VertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribP_Common(index, type, normalized, value[0], 2, __func__);
    }
    void VertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribP_Common(index, type, normalized, value[0], 3, __func__);
    }
    void VertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));
            return;
        }
        VertexAttribP_Common(index, type, normalized, value[0], 4, __func__);
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

    // ---- Stubbed glVertexAttrib* current-value setters, funnelled into the primitives above -------
    // These add ONLY a null-pointer guard: index validation (incl. the index-0 rejection) is inherited
    // from VertexAttrib4f / VertexAttribI4i / VertexAttribI4ui via ValidateCurrentVertexAttribIndex, so
    // the funnels must not re-validate it. Component fill matches the primitives: unspecified middle
    // components are 0, unspecified w is 1 (integer 1 for the I* forms).
#define MG_ATTRIB_NULL_GUARD(ptr)                                                                                      \
    if (!(ptr)) {                                                                                                      \
        MG_State::pGLContext->RecordError(                                                                            \
            ErrorCode::InvalidValue,                                                                                   \
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "value pointer cannot be null."));               \
        return;                                                                                                        \
    }

    // Family A -- GLdouble, value-preserving narrowing to float.
    void VertexAttrib1d(GLuint index, GLdouble x) { VertexAttrib4f(index, static_cast<GLfloat>(x), 0.0f, 0.0f, 1.0f); }
    void VertexAttrib2d(GLuint index, GLdouble x, GLdouble y) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f, 1.0f);
    }
    void VertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z), 1.0f);
    }
    void VertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z),
                       static_cast<GLfloat>(w));
    }
    void VertexAttrib1dv(GLuint index, const GLdouble* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttrib1d(index, v[0]); }
    void VertexAttrib2dv(GLuint index, const GLdouble* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttrib2d(index, v[0], v[1]); }
    void VertexAttrib3dv(GLuint index, const GLdouble* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttrib3d(index, v[0], v[1], v[2]);
    }
    void VertexAttrib4dv(GLuint index, const GLdouble* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttrib4d(index, v[0], v[1], v[2], v[3]);
    }

    // Family A -- GLshort, value-preserving (sign kept), NOT normalized.
    void VertexAttrib1s(GLuint index, GLshort x) { VertexAttrib4f(index, static_cast<GLfloat>(x), 0.0f, 0.0f, 1.0f); }
    void VertexAttrib2s(GLuint index, GLshort x, GLshort y) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), 0.0f, 1.0f);
    }
    void VertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z), 1.0f);
    }
    void VertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w) {
        VertexAttrib4f(index, static_cast<GLfloat>(x), static_cast<GLfloat>(y), static_cast<GLfloat>(z),
                       static_cast<GLfloat>(w));
    }
    void VertexAttrib1sv(GLuint index, const GLshort* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttrib1s(index, v[0]); }
    void VertexAttrib2sv(GLuint index, const GLshort* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttrib2s(index, v[0], v[1]); }
    void VertexAttrib3sv(GLuint index, const GLshort* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttrib3s(index, v[0], v[1], v[2]);
    }
    void VertexAttrib4sv(GLuint index, const GLshort* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttrib4s(index, v[0], v[1], v[2], v[3]);
    }

    // Family A -- 4-component *v with no scalar sibling, value-preserving. NOT the normalized 4N* forms.
    void VertexAttrib4bv(GLuint index, const GLbyte* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]),
                       static_cast<GLfloat>(v[3]));
    }
    void VertexAttrib4iv(GLuint index, const GLint* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]),
                       static_cast<GLfloat>(v[3]));
    }
    void VertexAttrib4uiv(GLuint index, const GLuint* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]),
                       static_cast<GLfloat>(v[3]));
    }
    void VertexAttrib4usv(GLuint index, const GLushort* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, static_cast<GLfloat>(v[0]), static_cast<GLfloat>(v[1]), static_cast<GLfloat>(v[2]),
                       static_cast<GLfloat>(v[3]));
    }

    // Family B -- normalized 4N* forms (GL 3.3 Core Eq 2.1/2.2 via the Normalize* helpers).
    void VertexAttrib4Nbv(GLuint index, const GLbyte* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, NormalizeSignedByte(v[0]), NormalizeSignedByte(v[1]), NormalizeSignedByte(v[2]),
                       NormalizeSignedByte(v[3]));
    }
    void VertexAttrib4Nsv(GLuint index, const GLshort* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, NormalizeSignedShort(v[0]), NormalizeSignedShort(v[1]), NormalizeSignedShort(v[2]),
                       NormalizeSignedShort(v[3]));
    }
    void VertexAttrib4Niv(GLuint index, const GLint* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, NormalizeSignedInt(v[0]), NormalizeSignedInt(v[1]), NormalizeSignedInt(v[2]),
                       NormalizeSignedInt(v[3]));
    }
    void VertexAttrib4Nusv(GLuint index, const GLushort* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, NormalizeUnsignedShort(v[0]), NormalizeUnsignedShort(v[1]),
                       NormalizeUnsignedShort(v[2]), NormalizeUnsignedShort(v[3]));
    }
    void VertexAttrib4Nuiv(GLuint index, const GLuint* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttrib4f(index, NormalizeUnsignedInt(v[0]), NormalizeUnsignedInt(v[1]), NormalizeUnsignedInt(v[2]),
                       NormalizeUnsignedInt(v[3]));
    }

    // Family C -- pure integer forms. Signed -> VertexAttribI4i (sign-extend), unsigned ->
    // VertexAttribI4ui (zero-extend). w defaults to the integer 1 / 1u. Never touches the float view.
    void VertexAttribI1i(GLuint index, GLint x) { VertexAttribI4i(index, x, 0, 0, 1); }
    void VertexAttribI2i(GLuint index, GLint x, GLint y) { VertexAttribI4i(index, x, y, 0, 1); }
    void VertexAttribI3i(GLuint index, GLint x, GLint y, GLint z) { VertexAttribI4i(index, x, y, z, 1); }
    void VertexAttribI1ui(GLuint index, GLuint x) { VertexAttribI4ui(index, x, 0u, 0u, 1u); }
    void VertexAttribI2ui(GLuint index, GLuint x, GLuint y) { VertexAttribI4ui(index, x, y, 0u, 1u); }
    void VertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z) { VertexAttribI4ui(index, x, y, z, 1u); }
    void VertexAttribI1iv(GLuint index, const GLint* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttribI1i(index, v[0]); }
    void VertexAttribI2iv(GLuint index, const GLint* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttribI2i(index, v[0], v[1]); }
    void VertexAttribI3iv(GLuint index, const GLint* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttribI3i(index, v[0], v[1], v[2]);
    }
    void VertexAttribI1uiv(GLuint index, const GLuint* v) { MG_ATTRIB_NULL_GUARD(v) VertexAttribI1ui(index, v[0]); }
    void VertexAttribI2uiv(GLuint index, const GLuint* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttribI2ui(index, v[0], v[1]);
    }
    void VertexAttribI3uiv(GLuint index, const GLuint* v) {
        MG_ATTRIB_NULL_GUARD(v) VertexAttribI3ui(index, v[0], v[1], v[2]);
    }
    void VertexAttribI4bv(GLuint index, const GLbyte* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttribI4i(index, static_cast<GLint>(v[0]), static_cast<GLint>(v[1]), static_cast<GLint>(v[2]),
                        static_cast<GLint>(v[3]));
    }
    void VertexAttribI4sv(GLuint index, const GLshort* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttribI4i(index, static_cast<GLint>(v[0]), static_cast<GLint>(v[1]), static_cast<GLint>(v[2]),
                        static_cast<GLint>(v[3]));
    }
    void VertexAttribI4ubv(GLuint index, const GLubyte* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttribI4ui(index, static_cast<GLuint>(v[0]), static_cast<GLuint>(v[1]), static_cast<GLuint>(v[2]),
                         static_cast<GLuint>(v[3]));
    }
    void VertexAttribI4usv(GLuint index, const GLushort* v) {
        MG_ATTRIB_NULL_GUARD(v)
        VertexAttribI4ui(index, static_cast<GLuint>(v[0]), static_cast<GLuint>(v[1]), static_cast<GLuint>(v[2]),
                         static_cast<GLuint>(v[3]));
    }
#undef MG_ATTRIB_NULL_GUARD

    void GetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        // GL_CURRENT_VERTEX_ATTRIB is context state and returns before TryGetVertexAttribute, so the
        // index bound has to be enforced up front or an out-of-range index reads past the array.
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
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
        case GL_VERTEX_ATTRIB_ARRAY_LONG:
            params[0] = attr->IsLong ? 1.0f : 0.0f;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            params[0] = static_cast<GLfloat>(attr->Divisor);
            return;
        case GL_VERTEX_ATTRIB_BINDING:
        case GL_VERTEX_ATTRIB_RELATIVE_OFFSET: {
            GLint value = 0;
            TryGetVertexAttribBindingQuery(index, pname, value);
            params[0] = static_cast<GLfloat>(value);
            return;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported float vertex attrib pname: " + std::to_string(pname)));
            return;
        }
    }

    // The double query mirrors GetVertexAttribfv exactly (it is the other float-domain getter): the
    // current value is read from the float view, and float -> double widening is lossless.
    void GetVertexAttribdv(GLuint index, GLenum pname, GLdouble* params) {
        if (!params) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "params pointer cannot be null."));
            return;
        }
        // GL_CURRENT_VERTEX_ATTRIB is context state and returns before TryGetVertexAttribute, so the
        // index bound has to be enforced up front or an out-of-range index reads past the array.
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        if (!ValidateVertexAttribPname(pname)) return;

        if (IsCurrentVertexAttribQuery(pname)) {
            const auto& current = MG_State::pGLContext->GetCurrentVertexAttribute(index);
            params[0] = static_cast<GLdouble>(current.floatValue[0]);
            params[1] = static_cast<GLdouble>(current.floatValue[1]);
            params[2] = static_cast<GLdouble>(current.floatValue[2]);
            params[3] = static_cast<GLdouble>(current.floatValue[3]);
            return;
        }

        const MG_State::GLState::VertexAttribute* attr = nullptr;
        if (!TryGetVertexAttribute(index, &attr)) return;

        switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
            params[0] = attr->Enabled ? 1.0 : 0.0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE:
            params[0] = static_cast<GLdouble>(attr->Size);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
            params[0] = static_cast<GLdouble>(attr->Stride);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE:
            params[0] = static_cast<GLdouble>(MG_Util::ConvertDataTypeToGLEnum(attr->Type));
            return;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
            params[0] = attr->Normalized ? 1.0 : 0.0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
            params[0] = attr->Buffer ? static_cast<GLdouble>(attr->Buffer->GetExternalIndex()) : 0.0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
            params[0] = attr->IsInteger ? 1.0 : 0.0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_LONG:
            params[0] = attr->IsLong ? 1.0 : 0.0;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            params[0] = static_cast<GLdouble>(attr->Divisor);
            return;
        case GL_VERTEX_ATTRIB_BINDING:
        case GL_VERTEX_ATTRIB_RELATIVE_OFFSET: {
            GLint value = 0;
            TryGetVertexAttribBindingQuery(index, pname, value);
            params[0] = static_cast<GLdouble>(value);
            return;
        }
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "Unsupported double vertex attrib pname: " + std::to_string(pname)));
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
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
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
        case GL_VERTEX_ATTRIB_ARRAY_LONG:
            params[0] = attr->IsLong ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            params[0] = static_cast<GLint>(attr->Divisor);
            return;
        case GL_VERTEX_ATTRIB_BINDING:
        case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
            TryGetVertexAttribBindingQuery(index, pname, params[0]);
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
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
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

    // glGetVertexArrayiv reports exactly one thing (GL 4.6 core table 23.4): which buffer the
    // named vertex array takes its indices from. Everything else about a vertex array is
    // per-attribute and belongs to the indexed queries below.
    void GetVertexArrayiv(GLuint vaobj, GLenum pname, GLint* param) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, __func__);
        if (!vao || !param) return;
        if (pname != GL_ELEMENT_ARRAY_BUFFER_BINDING) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname must be GL_ELEMENT_ARRAY_BUFFER_BINDING."));
            return;
        }
        const auto& indexBuffer = vao->GetIndexBufferBindingSlot().GetBoundObject();
        *param = indexBuffer ? static_cast<GLint>(indexBuffer->GetExternalIndex()) : 0;
    }

    void GetVertexArrayIndexediv(GLuint vaobj, GLuint index, GLenum pname, GLint* param) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, __func__);
        if (!vao || !param) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        const auto& attr = vao->GetAttribute(index);
        switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
            *param = attr.Enabled ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_SIZE:
            *param = static_cast<GLint>(attr.Size);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
            *param = static_cast<GLint>(attr.Stride);
            return;
        case GL_VERTEX_ATTRIB_ARRAY_TYPE:
            *param = static_cast<GLint>(MG_Util::ConvertDataTypeToGLEnum(attr.Type));
            return;
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
            *param = attr.Normalized ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
            *param = attr.IsInteger ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_LONG:
            *param = attr.IsLong ? GL_TRUE : GL_FALSE;
            return;
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
            *param = static_cast<GLint>(attr.Divisor);
            return;
        case GL_VERTEX_ATTRIB_RELATIVE_OFFSET:
            *param = static_cast<GLint>(vao->GetAttributeRelativeOffset(index));
            return;
        case GL_VERTEX_ATTRIB_BINDING:
            *param = static_cast<GLint>(vao->GetAttributeBindingIndex(index));
            return;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "pname is not an accepted indexed vertex array query."));
            return;
        }
    }

    // Only GL_VERTEX_BINDING_OFFSET needs 64 bits. Its `index` names a vertex buffer binding
    // point directly (GL 4.6 core 10.3.1), not an attribute - unlike every pname the 32-bit
    // indexed query above accepts, which is why this one does not go through an attribute's
    // binding index.
    void GetVertexArrayIndexed64iv(GLuint vaobj, GLuint index, GLenum pname, GLint64* param) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, __func__);
        if (!vao || !param) return;
        if (!VertexArrayImpl::ValidateVertexAttributeIndex(index)) return;
        if (pname != GL_VERTEX_BINDING_OFFSET) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "pname must be GL_VERTEX_BINDING_OFFSET."));
            return;
        }
        *param = static_cast<GLint64>(vao->GetBindingPoint(index).Offset);
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
        if (!ValidateVertexBindingRange(first, count, "BindVertexBuffers")) return;
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

    void VertexAttribLFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
        auto vao = GetBoundVertexArrayOrError("VertexAttribLFormat");
        if (!vao) return;
        VertexAttribLFormatSeparate_State(vao, attribindex, size, type, relativeoffset);
    }

    void VertexArrayAttribLFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset) {
        auto vao = GetNamedVertexArrayObject_State(vaobj, "VertexArrayAttribLFormat");
        if (!vao) return;
        VertexAttribLFormatSeparate_State(vao, attribindex, size, type, relativeoffset);
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
