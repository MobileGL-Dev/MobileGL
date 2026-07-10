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

namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl {
    Uint GetMaxVertexAttribs() {
        constexpr Uint capacity = static_cast<Uint>(MG_State::GLState::VertexArrayObject::MAX_VERTEX_ATTRIBS);
        if (!MG_Backend::pActiveBackendObject) return capacity;

        const Int backendLimit = MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxVertexAttribs;
        if (backendLimit <= 0) return capacity;
        return std::min(static_cast<Uint>(backendLimit), capacity);
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
} // namespace MobileGL::MG_Impl::GLImpl::VertexArrayImpl
