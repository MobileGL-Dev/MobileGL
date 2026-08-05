// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/Validators.cpp
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
#include <MG_Util/Converters/MGToGL/FramebufferEnumConverter.h>
#include <MG_Util/Converters/MGToStr/FramebufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl::FramebufferImpl {
    Bool ValidateFramebufferTarget(FramebufferTarget target) {
        if (target == FramebufferTarget::Unknown) {
            using namespace MG_Util;
            String bufferTargetStr = ConvertFramebufferTargetToString(target);
            String glTargetStr = ConvertGLEnumToString(ConvertFramebufferTargetToGLEnum(target));
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>(
                                            "MG_Impl/GLImpl/FramebufferImpl", "ValidateFramebufferTarget",
                                            std::format("Target {} ({}) is not valid.", bufferTargetStr, glTargetStr)));
            return false;
        }
        return true;
    }

    Bool ValidateFramebufferName(Uint index, Bool allowZero) {
        if (index == 0 && !allowZero) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", "ValidateFramebufferName",
                                             "Framebuffer name 0 is not valid in this situation."));
            return false;
        }
        Bool isValid = MG_State::pGLContext->ValidateFramebufferName(index);
        if (isValid) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", "ValidateFramebufferName",
                                         std::format("Framebuffer name {} is not valid.", index)));
        return false;
    }

    Bool ValidateFramebufferAttachmentType(FramebufferAttachmentType attachment) {
        if (attachment == FramebufferAttachmentType::Unknown) {
            using namespace MG_Util;
            String attachmentStr = ConvertFramebufferAttachmentTypeToString(attachment);
            String glAttachmentStr = ConvertGLEnumToString(ConvertFramebufferAttachmentTypeToGLEnum(attachment));
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl/FramebufferImpl", "ValidateFramebufferAttachmentType",
                    std::format("Attachment type {} ({}) is not valid.", attachmentStr, glAttachmentStr)));
            return false;
        }
        return true;
    }

    Bool ValidateColorAttachmentInRange(FramebufferAttachmentType attachment, const char* caller) {
        const auto first = static_cast<SizeT>(FramebufferAttachmentType::Color0);
        const auto index = static_cast<SizeT>(attachment);
        if (index < first) return true;
        const auto colorIndex = index - first;
        const auto limit = static_cast<SizeT>(
            MG_Backend::pActiveBackendObject ? MG_Backend::pActiveBackendObject->GetDynamicParameters()
                                                   .MaxColorAttachments
                                             : static_cast<Int>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS));
        if (colorIndex >= limit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl/FramebufferImpl", caller,
                    std::format("Colour attachment {} is beyond GL_MAX_COLOR_ATTACHMENTS ({}).", colorIndex, limit)));
            return false;
        }
        return true;
    }

    Bool ValidateRenderbufferTarget(RenderbufferTarget target) {
        if (target == RenderbufferTarget::Unknown) {
            using namespace MG_Util;
            String renderbufferTargetStr = ConvertRenderbufferTargetToString(target);
            String glTargetStr = ConvertGLEnumToString(ConvertRenderbufferTargetToGLEnum(target));
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl/FramebufferImpl", "ValidateRenderbufferTarget",
                    std::format("Target {} ({}) is not valid.", renderbufferTargetStr, glTargetStr)));
            return false;
        }
        return true;
    }

    Bool ValidateRenderbufferName(Uint index, Bool allowZero) {
        if (index == 0) {
            // Zero is never a GenRenderbuffers name, so it must not reach the name-table lookup
            // below: where it is allowed (glBindRenderbuffer / FramebufferRenderbuffer detach) it
            // means "unbind", and looking it up would record a bogus INVALID_OPERATION - GL CTS's
            // per-case state reset calls glBindRenderbuffer(GL_RENDERBUFFER, 0) after every case.
            if (allowZero) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", "ValidateRenderbufferName",
                                             "Renderbuffer name 0 is not valid in this situation."));
            return false;
        }
        Bool isValid = MG_State::pGLContext->ValidateRenderbufferName(index);
        if (isValid) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", "ValidateRenderbufferName",
                                         std::format("Renderbuffer name {} is not valid.", index)));
        return false;
    }

    Bool ValidateReadFramebufferForCopy(const char* caller) {
        auto& framebufferObject =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        if (!framebufferObject || !framebufferObject->CheckCompleteness()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidFramebufferOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                             "Read framebuffer is not framebuffer complete."));
            return false;
        }

        const FramebufferAttachmentType readBuffer = framebufferObject->GetReadBuffer();
        if (readBuffer == FramebufferAttachmentType::None ||
            !framebufferObject->GetAttachment(readBuffer).IsValid()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                             "Read buffer names no attachment of the read framebuffer."));
            return false;
        }

        // SAMPLE_BUFFERS is one whenever the read buffer resolves to multisample storage. A
        // multisample texture says so by its target - its sample count can legally be one - while a
        // renderbuffer says so by having been given a non-zero sample count.
        const auto& readAttachment = framebufferObject->GetAttachment(readBuffer);
        Bool isMultisampled = false;
        if (readAttachment.IsRenderbuffer() && readAttachment.GetRenderbuffer()) {
            isMultisampled = readAttachment.GetRenderbuffer()->GetSamples() > 0;
        } else if (readAttachment.IsTexture() && readAttachment.GetTexture()) {
            const auto target = readAttachment.GetTexture()->GetTarget();
            isMultisampled = target == TextureTarget::Texture2DMultisample ||
                             target == TextureTarget::Texture2DMultisampleArray;
        }
        if (isMultisampled) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                             "Cannot copy from a multisampled read framebuffer."));
            return false;
        }

        return true;
    }
} // namespace MobileGL::MG_Impl::GLImpl::FramebufferImpl
