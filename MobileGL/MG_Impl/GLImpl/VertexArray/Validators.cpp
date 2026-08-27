// MobileGL - MobileGL/MG_Impl/GLImpl/VertexArray/Validators.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Validators.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/MGToGL/DataTypeConverter.h>
#include <MG_Util/Converters/MGToStr/DataTypeConverter.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>

namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl {
    Uint GetMaxVertexAttribs() {
        // Shared with reflection's limit and with gl_MaxVertexAttribs; see ResolveMaxVertexAttribs.
        const Bool hasBackend = MG_Backend::pActiveBackendObject != nullptr;
        const Int backendLimit =
            hasBackend ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxVertexAttribs : 0;
        return static_cast<Uint>(MG_Util::ShaderTranspiler::ResolveMaxVertexAttribs(hasBackend, backendLimit));
    }

    Uint GetMaxVertexAttribBindings() {
        return GetMaxVertexAttribs();
    }

    Uint GetMaxVertexAttribRelativeOffset() {
        return 2047;
    }

    Uint GetMaxVertexAttribStride() {
        return 2048;
    }

    Bool ValidateVertexArrayName(Uint index) {
        Bool isValid = MG_State::pGLContext->ValidateVertexArrayName(index);
        if (!isValid) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateVertexArrayName",
                                             std::format("Vertex array name {} is not valid.", index)));
            return false;
        }
        return true;
    }

    Bool ValidateVertexArrayObject(Uint index) {
        if (!MG_State::pGLContext->ValidateVertexArrayObject(index)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateVertexArrayObject",
                                             std::format("Vertex array object {} does not exist.", index)));
            return false;
        }
        return true;
    }

    Bool ValidateVertexAttributeIndex(Uint index) {
        const Uint maxVertexAttribs = GetMaxVertexAttribs();
        if (index >= maxVertexAttribs) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateVertexAttributeIndex",
                    std::format("Attribute index {} exceeds maximum of {}.", index, maxVertexAttribs - 1)));
            return false;
        }
        return true;
    }

    Bool ValidateVertexAttribPointerParams(Uint index, SizeT size, DataType type, Int stride) {
        if (size < 1 || size > 4) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateVertexAttribPointerParams",
                    std::format("Invalid size {} for attribute {}. Must be 1-4.", size, index)));
            return false;
        }

        if (type == DataType::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateVertexAttribPointerParams",
                    std::format("Invalid type {} for attribute {}.", MG_Util::ConvertDataTypeToString(type), index)));
            return false;
        }

        if (stride < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateVertexAttribPointerParams",
                    std::format("Negative stride {} is not allowed for attribute {}.", stride, index)));
            return false;
        }

        return true;
    }

    Bool ValidateVertexAttribFormat(Uint index, GLint sizeRaw, GLenum glType, DataType type, Bool normalized,
                                    Int stride, Bool integerPath) {
        constexpr const char* fn = "ValidateVertexAttribFormat";
        // GL_UNSIGNED_INT_10F_11F_11F_REV is a three-component float-path-only packing that has no
        // DataType of its own, so it has to be recognised by name before the conversion below turns
        // it into Unknown and reports the wrong error (GL 4.6 core 10.3.2).
        if (glType == GL_UNSIGNED_INT_10F_11F_11F_REV) {
            if (integerPath) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("GL_UNSIGNED_INT_10F_11F_11F_REV is not an integer-path type (attribute {}).",
                                    index)));
                return false;
            }
            if (sizeRaw != 3) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("GL_UNSIGNED_INT_10F_11F_11F_REV requires size 3 (attribute {}).", index)));
                return false;
            }
        }
        if (type == DataType::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", fn,
                                             std::format("Invalid type for attribute {}.", index)));
            return false;
        }

        const bool isPacked = (type == DataType::Int2101010Rev || type == DataType::Uint2101010Rev);
        // The packed 2_10_10_10 types are float-normalizing only; glVertexAttribIPointer never accepts them.
        if (integerPath && isPacked) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", fn,
                    std::format("glVertexAttribIPointer does not accept packed 2_10_10_10 types (attribute {}).",
                                index)));
            return false;
        }

        // The integer path takes exactly the six signed/unsigned integer types (GL 4.6
        // core 10.3.2): BYTE, UNSIGNED_BYTE, SHORT, UNSIGNED_SHORT, INT, UNSIGNED_INT.
        // A blacklist could not express that: GL_FLOAT, GL_HALF_FLOAT,
        // GL_DOUBLE and GL_FIXED all convert to a perfectly valid DataType, so they slipped
        // through and were recorded as integer attributes.
        if (integerPath) {
            switch (type) {
            case DataType::Int8:
            case DataType::Uint8:
            case DataType::Int16:
            case DataType::Uint16:
            case DataType::Int32:
            case DataType::Uint32:
                break;
            default:
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("Type is not an integer vertex attribute type (attribute {}).", index)));
                return false;
            }
        }

        if (sizeRaw == static_cast<GLint>(GL_BGRA)) {
            // GL_BGRA is a float-path-only size: it needs GL_UNSIGNED_BYTE or a 2_10_10_10 type and
            // normalized == GL_TRUE. On the integer path it is simply an out-of-range size.
            if (integerPath) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("GL_BGRA size is not valid for glVertexAttribIPointer (attribute {}).", index)));
                return false;
            }
            if (!(type == DataType::Uint8 || isPacked)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("GL_BGRA size requires GL_UNSIGNED_BYTE or a 2_10_10_10 type (attribute {}).",
                                    index)));
                return false;
            }
            if (!normalized) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("GL_BGRA size requires normalized == GL_TRUE (attribute {}).", index)));
                return false;
            }
        } else {
            // Size-range (INVALID_VALUE) takes precedence over the packed-size-must-be-4 rule.
            if (sizeRaw < 1 || sizeRaw > 4) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("Invalid size {} for attribute {}. Must be 1-4 or GL_BGRA.", sizeRaw, index)));
                return false;
            }
            if (isPacked && sizeRaw != 4) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", fn,
                        std::format("A 2_10_10_10 type requires size 4 or GL_BGRA (attribute {}).", index)));
                return false;
            }
        }

        if (stride < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", fn,
                                             std::format("Negative stride {} for attribute {}.", stride, index)));
            return false;
        }
        return true;
    }

    Bool ValidateVertexAttribLFormat(Uint index, GLint size, GLenum type) {
        constexpr const char* fn = "ValidateVertexAttribLFormat";
        if (size < 1 || size > 4) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", fn,
                    std::format("Invalid size {} for attribute {}. Must be 1-4.", size, index)));
            return false;
        }
        // GL 4.6 core 10.3.2: the long form takes GL_DOUBLE and nothing else.
        if (type != GL_DOUBLE) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", fn,
                    std::format("Type 0x{:X} is not GL_DOUBLE (attribute {}).", type, index)));
            return false;
        }
        return true;
    }

    Bool ValidateVertexAttribRelativeOffset(Uint relativeOffset) {
        const Uint limit = GetMaxVertexAttribRelativeOffset();
        if (relativeOffset > limit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateVertexAttribRelativeOffset",
                    std::format("relativeoffset {} exceeds GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET ({}).", relativeOffset,
                                limit)));
            return false;
        }
        return true;
    }
} // namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl
