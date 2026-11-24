// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/Validators.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Validators.h"
#include "GL_Framebuffer.h"
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
        const auto limit =
            static_cast<SizeT>(MG_Backend::pActiveBackendObject
                                   ? MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxColorAttachments
                                   : static_cast<Int>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS));
        if (colorIndex >= limit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl/FramebufferImpl", caller,
                    std::format("Color attachment {} is beyond GL_MAX_COLOR_ATTACHMENTS ({}).", colorIndex, limit)));
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

    Bool ValidateFramebufferParameterPname(GLenum pname, Bool isDefaultFramebuffer, Bool forSetter,
                                           const char* caller) {
        Bool isDefaultParameter = false;
        switch (pname) {
        case GL_FRAMEBUFFER_DEFAULT_WIDTH:
        case GL_FRAMEBUFFER_DEFAULT_HEIGHT:
        case GL_FRAMEBUFFER_DEFAULT_LAYERS:
        case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
        case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
            isDefaultParameter = true;
            break;
        case GL_DOUBLEBUFFER:
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        case GL_IMPLEMENTATION_COLOR_READ_TYPE:
        case GL_SAMPLES:
        case GL_SAMPLE_BUFFERS:
        case GL_STEREO:
            // Queryable only; glFramebufferParameteri sets none of these.
            if (forSetter) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                                 std::format("pname {} is not settable on a framebuffer.",
                                                             MG_Util::ConvertGLEnumToString(pname))));
                return false;
            }
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl/FramebufferImpl", caller,
                    std::format("pname {} is not a framebuffer parameter.", MG_Util::ConvertGLEnumToString(pname))));
            return false;
        }

        // The default framebuffer has no DEFAULT_* state of its own - its shape comes from the
        // surface - so those names are accepted enums it simply cannot answer or accept.
        if (isDefaultFramebuffer && isDefaultParameter) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                             std::format("pname {} does not apply to the default framebuffer.",
                                                         MG_Util::ConvertGLEnumToString(pname))));
            return false;
        }
        return true;
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
        if (!ValidateReadFramebufferIsSingleView(framebufferObject.get(), caller)) return false;

        const FramebufferAttachmentType readBuffer = framebufferObject->GetReadBuffer();
        if (readBuffer == FramebufferAttachmentType::None || !framebufferObject->GetAttachment(readBuffer).IsValid()) {
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
            isMultisampled =
                target == TextureTarget::Texture2DMultisample || target == TextureTarget::Texture2DMultisampleArray;
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

    Bool ValidateReadFramebufferIsSingleView(const MG_State::GLState::FramebufferObject* framebufferObject,
                                             const char* caller) {
        // GL_OVR_multiview 9.3 / 18.2 / 18.3: reading, copying or blitting out of a read
        // framebuffer with more than one view is INVALID_FRAMEBUFFER_OPERATION.
        if (!framebufferObject || framebufferObject->GetViewCount() <= 1) return true;
        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidFramebufferOperation,
            MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl/FramebufferImpl", caller,
                                         "The read framebuffer has more than one view (GL_OVR_multiview)."));
        return false;
    }

    Bool ValidateBoundReadFramebufferIsSingleView(const char* caller) {
        const auto& framebufferObject =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read).GetBoundObject();
        return ValidateReadFramebufferIsSingleView(framebufferObject.get(), caller);
    }

    // GL 4.6 core 9.2.8 conditions that depend only on the framebuffer and the attachment
    // point. Shared, because glFramebufferTexture / 1D / 2D / 3D / TextureLayer are aliases of
    // one another in that section and a CTS case that walks the family must not get five
    // different answers - which is exactly what happened when these lived in one helper that
    // only two of the five went through.
    Bool ValidateFramebufferTextureAttachmentPoint(
        const char* functionName, const SharedPtr<MG_State::GLState::FramebufferObject>& framebufferObject,
        FramebufferAttachmentType attachmentType) {
        // "An INVALID_OPERATION error is generated if COLOR_ATTACHMENTm is used with m greater
        // than or equal to MAX_COLOR_ATTACHMENTS."
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, functionName)) return false;
        // "An INVALID_OPERATION error is generated if zero is bound to target." MobileGL keeps
        // a real FramebufferObject for framebuffer 0, so a null test can never see this - the
        // object is always there, and framebuffer 0 has to be recognized by identity instead,
        // the same comparison DrawBuffers_State makes. Without this an attach onto the default
        // framebuffer silently REPLACED its color attachment, permanently desynchronizing it
        // from what the swapchain keeps publishing.
        const auto& defaultFramebufferInfo = FramebufferImpl::pDefaultFramebufferInfo;
        if (!framebufferObject || (defaultFramebufferInfo && framebufferObject == defaultFramebufferInfo->defaultFBO)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    "No framebuffer object is bound to the target; the default framebuffer's attachments "
                    "cannot be named."));
            return false;
        }
        return true;
    }

    // The other half of 9.2.8: "level must be greater than or equal to zero", and for a
    // texture with immutable storage it "must be smaller than the number of levels the texture
    // has". Split from the attachment-point half because the caller only has a texture object
    // once the detach (texture == 0) case is behind it.
    Bool ValidateFramebufferTextureLevel(const char* functionName,
                                         const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         GLint level) {
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "Texture level must be non-negative."));
            return false;
        }
        if (!textureObject || !textureObject->IsImmutable()) {
            // A mutable texture has no level bound here: a level it has not specified yet is
            // not an error, it just leaves the framebuffer incomplete.
            return true;
        }
        // GetAddressableLevelCount(), NOT GetImmutableLevels(): for a VIEW the latter is
        // deliberately the ORIGINAL texture's count (GL 4.6 core 8.18 defines
        // TEXTURE_IMMUTABLE_LEVELS on a view that way), which is far too large a bound - a
        // two-level view onto a ten-level texture would accept level 5 and attach an image
        // nothing can draw into.
        const Uint levelBound = textureObject->GetAddressableLevelCount();
        if (static_cast<Uint>(level) >= levelBound) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    std::format("Texture level {} is beyond the {} level(s) this texture has.", level, levelBound)));
            return false;
        }
        return true;
    }
} // namespace MobileGL::MG_Impl::GLImpl::FramebufferImpl
