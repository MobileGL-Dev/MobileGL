// MobileGL - MobileGL/MG_Impl/GLImpl/Buffer/Validators.cpp
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
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/MGToGL/BufferEnumConverter.h>
#include <MG_Util/Converters/MGToStr/BufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl::BufferImpl {
    Bool ValidateBufferTarget(BufferTarget target) {
        if (target == BufferTarget::Unknown) {
            using namespace MG_Util;
            String bufferTargetStr = ConvertBufferTargetToString(target);
            String glTargetStr = ConvertGLEnumToString(ConvertBufferTargetToGLEnum(target));
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>(
                                            "MG_Impl/GLImpl/BufferImpl", "ValidateBufferTarget",
                                            std::format("Target {} ({}) is not valid.", bufferTargetStr, glTargetStr)));
            return false;
        }

        if (target == BufferTarget::Index && MG_State::pGLContext->GetBoundVertexArray() == nullptr) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl",
                                                                           "ValidateBufferTarget",
                                                                           "No vertex array object is bound."));
            return false;
        }

        return true;
    }

    Bool ValidateBufferBindingPointTarget(BufferTarget target) {
        if (target != BufferTarget::Uniform && target != BufferTarget::AtomicCounter &&
            target != BufferTarget::TransformFeedback && target != BufferTarget::ShaderStorage) {
            using namespace MG_Util;
            String bufferTargetStr = ConvertBufferTargetToString(target);
            String glTargetStr = ConvertGLEnumToString(ConvertBufferTargetToGLEnum(target));
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>(
                                            "MG_Impl/GLImpl/BufferImpl", "ValidateBufferTarget",
                                            std::format("Target {} ({}) is not valid.", bufferTargetStr, glTargetStr)));
            return false;
        }
        return true;
    }

    Bool ValidateBufferBindingPointIndex(BufferTarget target, Uint index) {
        SizeT pointCount = MG_State::pGLContext->GetBufferBindingPointCount(target);
        if (target == BufferTarget::ShaderStorage && MG_Backend::pActiveBackendObject) {
            const Int backendCount =
                MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxShaderStorageBufferBindings;
            pointCount = std::min(pointCount, static_cast<SizeT>(std::max(backendCount, 0)));
        }

        if (index < pointCount) {
            return true;
        }

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidValue,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl", "ValidateBufferBindingPointIndex",
                                         std::format("Binding point index {} is out of range for target {}.", index,
                                                     MG_Util::ConvertBufferTargetToString(target))));
        return false;
    }

    Bool ValidateBufferName(Uint index, Bool allowZero) {
        if (index == 0) {
            if (allowZero) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl", "ValidateBufferName",
                                                                      "Buffer name 0 is not valid."));
            return false;
        }
        Bool isValid = MG_State::pGLContext->ValidateBufferName(index);
        if (isValid) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl", "ValidateBufferName",
                                         std::format("Buffer name {} is not valid.", index)));
        return false;
    }

    Bool ValidateBufferUsage(BufferUsage usage) {
        if (usage != BufferUsage::Unknown) {
            return true;
        }
        using namespace MG_Util;
        String bufferUsageStr = ConvertBufferUsageToString(usage);
        String glUsageStr = ConvertGLEnumToString(ConvertBufferUsageToGLEnum(usage));
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl/BufferImpl", "ValidateBufferUsage",
                std::format("Usage {} ({}) is not one of the allowable values.", bufferUsageStr, glUsageStr)));
        return false;
    }

    Bool ValidateBufferMappingAccess(Flags<BufferMappingAccessBit> accessBits) {
        if (accessBits == BufferMappingAccessBit::Null) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidEnum,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl",
                                                                           "ValidateBufferMappingAccess",
                                                                           "Access bits cannot be null."));
            return false;
        }

        const auto validBits = BufferMappingAccessBit::Read | BufferMappingAccessBit::Write |
                               BufferMappingAccessBit::InvalidateRange | BufferMappingAccessBit::InvalidateBuffer |
                               BufferMappingAccessBit::FlushExplicit | BufferMappingAccessBit::Unsynchronized |
                               BufferMappingAccessBit::Persistent | BufferMappingAccessBit::Coherent;

        if ((accessBits & validBits) != accessBits) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/BufferImpl", "ValidateBufferMappingAccess",
                                             "Access bits cannot contain invalid flags."));
            return false;
        }

        return true;
    }
} // namespace MobileGL::MG_Impl::GLImpl::BufferImpl
