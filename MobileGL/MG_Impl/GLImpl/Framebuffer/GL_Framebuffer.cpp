// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Framebuffer.h"
#include "Validators.h"
#include <MG_Util/Texture/TextureFormatProcessor.h>
#include "Config.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_Impl/GLImpl/Texture/Validators.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        // GL only requires support for framebuffers whose depth and stencil attachments
        // are the same image; anything else may be reported GL_FRAMEBUFFER_UNSUPPORTED.
        // DirectVulkan cannot form two separate attachments at all, and the real ES
        // drivers behind DirectGLES answer UNSUPPORTED for it too - so saying COMPLETE
        // and then rendering into a framebuffer the driver refuses produced silently
        // empty results (KHR-GL3x.packed_depth_stencil.verify_mixed_attachments).
        Bool ActiveBackendRejectsDistinctDepthStencil() {
            auto* activeBackend = MG_Backend::pActiveBackendObject.get();
            if (activeBackend == nullptr) {
                return false;
            }
            if (activeBackend->GetBackendType() == BackendType::DirectVulkan) {
                return true;
            }
            return !activeBackend->GetDynamicParameters().SupportsDistinctDepthStencilAttachments;
        }

        Bool HasDistinctCompleteDepthStencilTextureAttachments(
            const MG_State::GLState::FramebufferObject& framebufferObject) {
            if (framebufferObject.GetExternalIndex() == 0) {
                return false;
            }

            const auto& depthAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Depth);
            const auto& stencilAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Stencil);
            if (!depthAttachment.IsComplete() || !stencilAttachment.IsComplete() ||
                !depthAttachment.IsTexture() || !stencilAttachment.IsTexture()) {
                return false;
            }

            return depthAttachment.GetTexture().get() != stencilAttachment.GetTexture().get() ||
                   depthAttachment.GetTextureUploadTarget() != stencilAttachment.GetTextureUploadTarget() ||
                   depthAttachment.GetTextureLevel() != stencilAttachment.GetTextureLevel();
        }

        // Mirrors the renderer-side gate: distinct depth/stencil renderbuffers (or a
        // renderbuffer paired with a texture) cannot form one Vulkan depth-stencil
        // attachment, and GL permits reporting such framebuffers as UNSUPPORTED.
        Bool HasDistinctCompleteDepthStencilRenderbufferAttachments(
            const MG_State::GLState::FramebufferObject& framebufferObject) {
            if (framebufferObject.GetExternalIndex() == 0) {
                return false;
            }

            const auto& depthAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Depth);
            const auto& stencilAttachment = framebufferObject.GetAttachment(FramebufferAttachmentType::Stencil);
            if (!depthAttachment.IsComplete() || !stencilAttachment.IsComplete()) {
                return false;
            }
            if (depthAttachment.IsRenderbuffer() && stencilAttachment.IsRenderbuffer()) {
                return depthAttachment.GetRenderbuffer().get() != stencilAttachment.GetRenderbuffer().get();
            }
            return (depthAttachment.IsRenderbuffer() || stencilAttachment.IsRenderbuffer()) &&
                   (depthAttachment.IsTexture() || stencilAttachment.IsTexture());
        }

        Bool HasUnsupportedDistinctDepthStencilAttachments(
            const MG_State::GLState::FramebufferObject& framebufferObject) {
            // TODO: Keep this in sync with DirectVulkan renderbuffer support as color renderbuffer rendering lands.
            return HasDistinctCompleteDepthStencilTextureAttachments(framebufferObject) ||
                   HasDistinctCompleteDepthStencilRenderbufferAttachments(framebufferObject);
        }

        Bool HasDefinedAttachment(const MG_State::GLState::FramebufferObject& framebufferObject) {
            for (const auto& attachment : framebufferObject.GetAllAttachmentObjects()) {
                if (attachment.IsValid() && !attachment.IsEmpty()) {
                    return true;
                }
            }
            return false;
        }

        // Whether the backend can actually attach this color format to a framebuffer. Preferred
        // source of truth is the backend's probed format-capability cache (real glCheckFramebufferStatus
        // probes, so extensions like EXT_render_snorm are respected). Formats a probe-less backend
        // cannot answer for fall back to a conservative static list of formats no ES driver renders to:
        // shared-exponent, SNORM, three-channel norm16/float32/sRGB and three-channel integer formats.
        // Desktop GL treats those as texture-only too (not in the GL 3.3 required-renderable list), so
        // reporting GL_FRAMEBUFFER_UNSUPPORTED for them is legal.
        //
        // `capabilityTargetIndex` is the row of the cache the attachment actually lives in;
        // kFormatCapabilityTargetCount asks about the format in general. Asking per target matters
        // because a capability recorded for one of them says nothing about the others: DirectGLES
        // decides each target's substitution against that target's own probe, and a buffer texture
        // never gets one at all. This is also where the three-channel widening becomes visible to
        // the application - a GL_RGB8_SNORM colour attachment on a driver with no renderable
        // three-channel format answers COMPLETE because the backend stores it as GL_RGBA16F and
        // recorded FramebufferRenderable in CaveatCaps.
        Bool IsColorInternalFormatRenderable(TextureInternalFormat format, SizeT capabilityTargetIndex) {
            const SizeT formatIndex = static_cast<SizeT>(format);
            if (MG_Backend::pActiveBackendObject && formatIndex < MG_Backend::kFormatCapabilityFormatCount) {
                const auto& cache = MG_Backend::pActiveBackendObject->GetFormatCapabilities();
                const SizeT sentinelFormat = static_cast<SizeT>(TextureInternalFormat::RGBA8);
                Bool cachePopulated = false;
                for (SizeT targetIndex = 0; targetIndex < MG_Backend::kFormatCapabilityTargetCount && !cachePopulated;
                     ++targetIndex) {
                    cachePopulated = MG_Backend::HasFormatCapability(cache.FullCaps[targetIndex][sentinelFormat],
                                                                     MG_Backend::FormatCapability::Creatable);
                }
                if (cachePopulated) {
                    const Bool singleTarget = capabilityTargetIndex < MG_Backend::kFormatCapabilityTargetCount;
                    const SizeT firstTarget = singleTarget ? capabilityTargetIndex : 0;
                    const SizeT lastTarget =
                        singleTarget ? capabilityTargetIndex + 1 : MG_Backend::kFormatCapabilityTargetCount;
                    for (SizeT targetIndex = firstTarget; targetIndex < lastTarget; ++targetIndex) {
                        if (MG_Backend::HasFormatCapability(cache.FullCaps[targetIndex][formatIndex],
                                                            MG_Backend::FormatCapability::FramebufferRenderable) ||
                            MG_Backend::HasFormatCapability(cache.CaveatCaps[targetIndex][formatIndex],
                                                            MG_Backend::FormatCapability::FramebufferRenderable)) {
                            return true;
                        }
                    }
                    return false;
                }
            }
            switch (format) {
            case TextureInternalFormat::RGB9E5:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10: // stored as RGB16
            case TextureInternalFormat::RGB12: // stored as RGB16
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::SRGB8:
                return false;
            default:
                return true;
            }
        }

        Bool HasNonRenderableColorAttachment(const MG_State::GLState::FramebufferObject& framebufferObject) {
            const auto& attachments = framebufferObject.GetAllAttachmentObjects();
            for (SizeT i = 0; i < attachments.size(); ++i) {
                const auto type = static_cast<FramebufferAttachmentType>(i);
                if (type < FramebufferAttachmentType::Color0 || type > FramebufferAttachmentType::Color31) {
                    continue;
                }
                const auto& attachment = attachments[i];
                if (!attachment.IsValid()) continue;
                TextureInternalFormat format = TextureInternalFormat::Unknown;
                SizeT capabilityTargetIndex = MG_Backend::kFormatCapabilityTargetCount;
                if (attachment.IsTexture() && attachment.GetTexture()) {
                    format = attachment.GetTexture()->GetFormat();
                    capabilityTargetIndex =
                        MG_Backend::GetFormatCapabilityTargetIndex(attachment.GetTexture()->GetTarget());
                } else if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
                    format = attachment.GetRenderbuffer()->GetInternalFormat();
                    capabilityTargetIndex = MG_Backend::GetRenderbufferFormatCapabilityTargetIndex();
                }
                if (format != TextureInternalFormat::Unknown &&
                    !IsColorInternalFormatRenderable(format, capabilityTargetIndex)) {
                    return true;
                }
            }
            return false;
        }

        void RecordUnsupportedFramebufferTextureAttachmentError(const char* functionName, const char* detail) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, detail));
        }

        GLint ClassifyAttachmentComponentType(TextureInternalFormat internalFormat,
                                              FramebufferAttachmentType attachmentType) {
            // A stencil value is an unsigned integer index regardless of the depth half
            // of a packed format.
            if (attachmentType == FramebufferAttachmentType::Stencil) return GL_UNSIGNED_INT;
            switch (internalFormat) {
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth32FStencil8:
                return GL_FLOAT;
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA32I:
                return GL_INT;
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2UI:
                return GL_UNSIGNED_INT;
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return GL_SIGNED_NORMALIZED;
            case TextureInternalFormat::Unknown:
                return GL_NONE;
            default:
                return GL_UNSIGNED_NORMALIZED;
            }
        }

        // GL 4.6 core 9.2.3: which attachment names an attachment query accepts depends on whether
        // it is looking at the default framebuffer or at a framebuffer object, and a name outside
        // its list is INVALID_ENUM - not the INVALID_OPERATION that a well-formed but unattachable
        // name gets. On success the default framebuffer's names are rewritten to the attachment
        // point that backs them.
        Bool ResolveAttachmentQueryName(Bool isDefaultFramebuffer, GLenum& attachment, const char* caller) {
            const auto reject = [&]() {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        std::format("Attachment {} is not queryable on this framebuffer.",
                                    MG_Util::ConvertGLEnumToString(attachment))));
                return false;
            };

            if (isDefaultFramebuffer) {
                switch (attachment) {
                case GL_FRONT:
                case GL_FRONT_LEFT:
                case GL_FRONT_RIGHT:
                case GL_BACK:
                case GL_BACK_LEFT:
                case GL_BACK_RIGHT:
                    attachment = GL_COLOR_ATTACHMENT0;
                    return true;
                case GL_DEPTH:
                    attachment = GL_DEPTH_ATTACHMENT;
                    return true;
                case GL_STENCIL:
                    attachment = GL_STENCIL_ATTACHMENT;
                    return true;
                default:
                    return reject();
                }
            }

            switch (attachment) {
            case GL_DEPTH_ATTACHMENT:
            case GL_STENCIL_ATTACHMENT:
            case GL_DEPTH_STENCIL_ATTACHMENT:
                return true;
            default:
                break;
            }
            // Every COLOR_ATTACHMENTi token is a name; one past MAX_COLOR_ATTACHMENTS is a
            // well-formed name that this framebuffer has no point for, which is INVALID_OPERATION
            // and is left to the attachment-point lookup below.
            if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31) return true;
            return reject();
        }

        // The TEXTURE_* parameters only exist while the attached object is a texture (GL 4.6 core
        // 9.2.3); asking for one of them about a renderbuffer is INVALID_ENUM. An empty attachment
        // point is a different rule and is deliberately passed through here.
        Bool ValidateAttachmentQueryPname(const MG_State::GLState::FramebufferAttachmentObject* attachmentObject,
                                          GLenum pname, const char* caller) {
            switch (pname) {
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
            case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
            case GL_FRAMEBUFFER_ATTACHMENT_LAYERED:
                break;
            default:
                return true;
            }

            const Bool nothingAttached =
                attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid();
            if (nothingAttached || attachmentObject->IsTexture()) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is only defined for a texture attachment.",
                                MG_Util::ConvertGLEnumToString(pname))));
            return false;
        }

        // Handles the format-derived pnames shared by GetFramebufferAttachmentParameteriv
        // and its DSA variant. Returns true when pname was one of them.
        Bool TryAnswerAttachmentFormatQuery(const MG_State::GLState::FramebufferAttachmentObject* attachmentObject,
                                            FramebufferAttachmentType attachmentType, Bool depthStencilAlias,
                                            GLenum pname, GLint* params, const char* caller) {
            switch (pname) {
            case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
            case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
            case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
                break;
            default:
                return false;
            }

            if (pname == GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE && depthStencilAlias) {
                // The depth and stencil components have different types, so the combined
                // attachment name has no single answer.
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        "GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE cannot be queried on "
                        "GL_DEPTH_STENCIL_ATTACHMENT."));
                return true;
            }

            if (attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid()) {
                // With OBJECT_TYPE == GL_NONE only OBJECT_TYPE and OBJECT_NAME may be queried.
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                                 "No image is attached to the queried attachment point."));
                return true;
            }

            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            if (attachmentObject->IsTexture() && attachmentObject->GetTexture()) {
                internalFormat = attachmentObject->GetTexture()->GetFormat();
            } else if (attachmentObject->IsRenderbuffer() && attachmentObject->GetRenderbuffer()) {
                internalFormat = attachmentObject->GetRenderbuffer()->GetInternalFormat();
            }
            const auto sizes = MG_Util::GetComponentSizesForInternalFormat(internalFormat);

            switch (pname) {
            case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE:
                *params = sizes.Red;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
                *params = sizes.Green;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE:
                *params = sizes.Blue;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
                *params = sizes.Alpha;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE:
                *params = sizes.Depth;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
                *params = sizes.Stencil;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING:
                *params = (internalFormat == TextureInternalFormat::SRGB8 ||
                           internalFormat == TextureInternalFormat::SRGB8Alpha8)
                    ? GL_SRGB
                    : GL_LINEAR;
                break;
            case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
                *params = ClassifyAttachmentComponentType(internalFormat, attachmentType);
                break;
            }
            return true;
        }

        Bool ResolveRepresentableFramebufferTextureUploadTarget(const MG_State::GLState::ITextureObject& textureObject,
                                                                TextureUploadTarget& outUploadTarget,
                                                                Bool& outLayered) {
            outLayered = false;
            switch (textureObject.GetTarget()) {
            case TextureTarget::Texture1D:
                outUploadTarget = TextureUploadTarget::Texture1D;
                return true;
            case TextureTarget::Texture2D:
                outUploadTarget = TextureUploadTarget::Texture2D;
                return true;
            case TextureTarget::TextureRectangle:
                outUploadTarget = TextureUploadTarget::TextureRectangle;
                return true;
            case TextureTarget::Texture2DMultisample:
                outUploadTarget = TextureUploadTarget::Texture2DMultisample;
                return true;
            // Everything below attaches as a LAYERED attachment (GL 4.6 core 9.2.8): glFramebufferTexture
            // on one of these binds the whole texture, not one image, and the upload target named here is
            // only the representative the attachment model records - the first face for a cube map, the
            // whole array otherwise. DirectGLES routes a layered attachment to glFramebufferTexture, which
            // is exactly this.
            case TextureTarget::Texture2DArray:
                outUploadTarget = TextureUploadTarget::Texture2DArray;
                outLayered = true;
                return true;
            case TextureTarget::Texture1DArray:
                outUploadTarget = TextureUploadTarget::Texture1DArray;
                outLayered = true;
                return true;
            case TextureTarget::Texture2DMultisampleArray:
                outUploadTarget = TextureUploadTarget::Texture2DMultisampleArray;
                outLayered = true;
                return true;
            case TextureTarget::Texture3D:
                outUploadTarget = TextureUploadTarget::Texture3D;
                outLayered = true;
                return true;
            case TextureTarget::TextureCubeMap:
                outUploadTarget = TextureUploadTarget::CubeMapPositiveX;
                outLayered = true;
                return true;
            case TextureTarget::TextureCubeMapArray:
                outUploadTarget = TextureUploadTarget::CubeMapArray;
                outLayered = true;
                return true;
            default:
                outUploadTarget = TextureUploadTarget::Unknown;
                return false;
            }
        }

        void AttachFramebufferTextureWithUploadTarget(const char* functionName, GLenum target, GLenum attachment,
                                                      GLuint texture, GLint level,
                                                      TextureUploadTarget textureUploadTarget, Bool layered = false) {
            if (target == GL_FRAMEBUFFER) {
                target = GL_DRAW_FRAMEBUFFER;
            }

            if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
                AttachFramebufferTextureWithUploadTarget(functionName, target, GL_DEPTH_ATTACHMENT, texture, level,
                                                        textureUploadTarget);
                AttachFramebufferTextureWithUploadTarget(functionName, target, GL_STENCIL_ATTACHMENT, texture, level,
                                                        textureUploadTarget);
                return;
            }

            const FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
            const FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
            if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
            if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
            if (!TextureImpl::ValidateTextureName(texture, true)) return;

            auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
            auto& framebufferObject = bindingSlot.GetBoundObject();
            if (!framebufferObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 "Framebuffer target is bound to no framebuffer object."));
                return;
            }

            if (texture == 0) {
                framebufferObject->Detach(attachmentType);
                return;
            }

            auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
            if (!textureObject) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                                 std::format("Texture object {} is not valid.", texture)));
                return;
            }

            const auto expectedTextureTarget = MG_Util::ConvertTextureUploadTargetToTextureTarget(textureUploadTarget);
            if (expectedTextureTarget == TextureTarget::Unknown ||
                textureObject->GetTarget() != expectedTextureTarget) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", functionName,
                        std::format("Attachment target {} does not match texture {} target {}.",
                                    MG_Util::ConvertTextureUploadTargetToString(textureUploadTarget), texture,
                                    MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
                return;
            }

            framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, level, 0, layered);
        }
    } // namespace

    void BlitFramebuffer_Backend(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                 GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        MG_Backend::gBackendFunctionsTable.GL.BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1,
                                                              mask, filter);
    }

    void BlitNamedFramebuffer_Backend(const SharedPtr<MG_State::GLState::FramebufferObject>& readFramebuffer,
                                      const SharedPtr<MG_State::GLState::FramebufferObject>& drawFramebuffer,
                                      GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                      GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        auto blitNamedFramebuffer = MG_Backend::gBackendFunctionsTable.GL.BlitNamedFramebuffer;
        if (!blitNamedFramebuffer) {
            MGLOG_E_ONCE("glBlitNamedFramebuffer skipped: backend does not implement explicit framebuffer blit.");
            return;
        }
        blitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1,
                             dstY1, mask, filter);
    }

    void ClearNamedFramebufferfv_Backend(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                         GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        auto clearNamedFramebufferfv = MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfv;
        if (!clearNamedFramebufferfv) {
            MGLOG_E_ONCE("glClearNamedFramebufferfv skipped: backend does not implement explicit framebuffer clear.");
            return;
        }
        clearNamedFramebufferfv(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfi_Backend(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                         GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        auto clearNamedFramebufferfi = MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferfi;
        if (!clearNamedFramebufferfi) {
            MGLOG_E_ONCE("glClearNamedFramebufferfi skipped: backend does not implement explicit framebuffer clear.");
            return;
        }
        clearNamedFramebufferfi(framebuffer, buffer, drawbuffer, depth, stencil);
    }

    void ClearNamedFramebufferiv_Backend(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                         GLenum buffer, GLint drawbuffer, const GLint* value) {
        auto clearNamedFramebufferiv = MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferiv;
        if (!clearNamedFramebufferiv) {
            MGLOG_E_ONCE("glClearNamedFramebufferiv skipped: backend does not implement explicit framebuffer clear.");
            return;
        }
        clearNamedFramebufferiv(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferuiv_Backend(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                          GLenum buffer, GLint drawbuffer, const GLuint* value) {
        auto clearNamedFramebufferuiv = MG_Backend::gBackendFunctionsTable.GL.ClearNamedFramebufferuiv;
        if (!clearNamedFramebufferuiv) {
            MGLOG_E_ONCE("glClearNamedFramebufferuiv skipped: backend does not implement explicit framebuffer clear.");
            return;
        }
        clearNamedFramebufferuiv(framebuffer, buffer, drawbuffer, value);
    }

    void SampleMaski_State(GLuint maskNumber, GLbitfield mask) {
        if (maskNumber != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "SampleMaski_State",
                                             "Only sample mask word 0 is currently supported."));
            return;
        }

        MG_State::pGLContext->SetSampleMaskValue(static_cast<Uint32>(mask));
    }

    Int GetMaxRenderbufferSize_State() {
        if (MG_Backend::pActiveBackendObject == nullptr) {
            return std::numeric_limits<Int>::max();
        }
        return MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxRenderbufferSize;
    }

    Int GetMaxRenderbufferSamples_State() {
        if (MG_Backend::pActiveBackendObject == nullptr) {
            return std::numeric_limits<Int>::max();
        }
        return std::max(MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxSamples, 1);
    }

    // GL_MAX_SAMPLES is the ceiling over all formats; an integer format has its own, lower
    // one (GL_MAX_INTEGER_SAMPLES) and GL 4.6 core 9.2.4 makes exceeding it INVALID_OPERATION.
    // The multisample TEXTURE path already resolves the limit per format
    // (GL_Texture.cpp, GetMaxTextureSamplesForFormat); renderbuffers only ever compared
    // against GL_MAX_SAMPLES, so on a driver where the two differ - Adreno reports
    // GL_MAX_SAMPLES 4 and GL_MAX_INTEGER_SAMPLES 1 - an integer renderbuffer accepted a
    // sample count the format cannot deliver, and said GL_NO_ERROR about it.
    Int GetMaxRenderbufferSamplesForFormat_State(TextureInternalFormat format) {
        if (MG_Backend::pActiveBackendObject == nullptr) {
            return std::numeric_limits<Int>::max();
        }
        const auto& dynamicParameters = MG_Backend::pActiveBackendObject->GetDynamicParameters();

        GLenum normalizedInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(format);
        GLenum normalizedFormat = GL_RGBA;
        GLenum normalizedType = GL_UNSIGNED_BYTE;
        MG_Util::TextureFormatProcessor::NormalizePixelFormat(normalizedInternalFormat,
                                                              PixelFormatNormalizeOptionBit::None,
                                                              &normalizedInternalFormat, &normalizedFormat,
                                                              &normalizedType);
        const Bool isIntegerFormat = normalizedFormat == GL_RED_INTEGER || normalizedFormat == GL_RG_INTEGER ||
                                     normalizedFormat == GL_RGB_INTEGER || normalizedFormat == GL_RGBA_INTEGER;
        if (!isIntegerFormat) {
            return GetMaxRenderbufferSamples_State();
        }
        return std::max(dynamicParameters.MaxIntegerSamples, 1);
    }

    Bool ValidateRenderbufferStorageSize_State(GLsizei width, GLsizei height, const char* caller) {
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Width and height must be non-negative."));
            return false;
        }

        const Int maxRenderbufferSize = GetMaxRenderbufferSize_State();
        if (width > maxRenderbufferSize || height > maxRenderbufferSize) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("Width and height must not exceed GL_MAX_RENDERBUFFER_SIZE ({}).",
                                maxRenderbufferSize)));
            return false;
        }
        return true;
    }

    Bool ValidateRenderbufferStorageSamples_State(GLsizei samples, TextureInternalFormat format, const char* caller) {
        if (samples < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "Sample count must be non-negative."));
            return false;
        }

        // TODO: Resolve the remaining per-internalformat renderbuffer sample limits once
        // glGetInternalformativ is backed; integer formats are handled below.
        const Int maxSamples = GetMaxRenderbufferSamplesForFormat_State(format);
        if (samples > maxSamples) {
            // GL 4.6 core 9.2.4 makes asking for more samples than the format supports
            // INVALID_OPERATION, not INVALID_VALUE - the count is well formed, this format just
            // cannot deliver it. Only a negative count is INVALID_VALUE.
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("Sample count {} exceeds this format's sample limit ({}).", samples, maxSamples)));
            return false;
        }
        return true;
    }

    void RenderbufferStorageMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                              GLsizei height) {
        constexpr const char* kCaller = "RenderbufferStorageMultisample_State";
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(rbTarget)) return;

        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(rbTarget);
        auto& renderbufferObject = bindingSlot.GetBoundObject();
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", kCaller,
                                             "Renderbuffer target is bound to no renderbuffer object."));
            return;
        }

        TextureInternalFormat format = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!TextureImpl::ValidateTextureInternalFormat(format)) return;

        if (!ValidateRenderbufferStorageSamples_State(samples, format, kCaller)) return;
        if (!ValidateRenderbufferStorageSize_State(width, height, kCaller)) return;

        renderbufferObject->AllocateStorage({width, height});
        renderbufferObject->SetInternalFormat(format);
        renderbufferObject->SetSamples(samples);
    }

    void AllocateRenderbufferStorage_State(const SharedPtr<MG_State::GLState::RenderbufferObject>& renderbufferObject,
                                           GLenum internalformat, GLsizei width, GLsizei height, const char* caller) {
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "No renderbuffer object is available."));
            return;
        }
        TextureInternalFormat format = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!TextureImpl::ValidateTextureInternalFormat(format)) return;
        if (!ValidateRenderbufferStorageSize_State(width, height, caller)) return;
        renderbufferObject->AllocateStorage({width, height});
        renderbufferObject->SetInternalFormat(format);
        renderbufferObject->SetSamples(0);
    }

    void RenderbufferStorage_State(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(rbTarget)) return;
        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(rbTarget);
        auto& renderbufferObject = bindingSlot.GetBoundObject();
        AllocateRenderbufferStorage_State(renderbufferObject, internalformat, width, height, "RenderbufferStorage_State");
    }

    GLboolean IsRenderbuffer_State(GLuint renderbuffer) {
        return MG_State::pGLContext->ValidateRenderbufferObject(renderbuffer);
    }

    GLboolean IsFramebuffer_State(GLuint framebuffer) {
        return MG_State::pGLContext->ValidateFramebufferObject(framebuffer);
    }

    void GetFramebufferAttachmentParameteriv_State(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
        if (params == nullptr) return;
        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }

        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto& framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetFramebufferAttachmentParameteriv_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (!ResolveAttachmentQueryName(framebufferObject->IsDefaultFramebuffer(), attachment,
                                        "GetFramebufferAttachmentParameteriv_State")) {
            return;
        }

        const Bool depthStencilAlias = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
        FramebufferAttachmentType attachmentType = depthStencilAlias
            ? FramebufferAttachmentType::Depth
            : MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;

        Bool depthStencilMismatch = false;
        const auto* attachmentObject = [&]() -> const MG_State::GLState::FramebufferAttachmentObject* {
            if (!depthStencilAlias) {
                return &framebufferObject->GetAttachment(attachmentType);
            }

            const auto& depthAttachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Depth);
            const auto& stencilAttachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil);
            const Bool depthLive = depthAttachment.IsValid() && !depthAttachment.IsEmpty();
            const Bool stencilLive = stencilAttachment.IsValid() && !stencilAttachment.IsEmpty();
            if (depthLive && stencilLive) {
                const Bool sameObject = depthAttachment.IsTexture() == stencilAttachment.IsTexture() &&
                    (!depthAttachment.IsTexture() || depthAttachment.GetTexture() == stencilAttachment.GetTexture()) &&
                    (!depthAttachment.IsRenderbuffer() ||
                     depthAttachment.GetRenderbuffer() == stencilAttachment.GetRenderbuffer());
                depthStencilMismatch = !sameObject;
            } else {
                // GL_DEPTH_STENCIL_ATTACHMENT means "both halves"; a lone half does not answer it.
                depthStencilMismatch = depthLive != stencilLive;
            }
            if (depthLive) return &depthAttachment;
            if (stencilLive) return &stencilAttachment;
            return nullptr;
        }();

        if (depthStencilMismatch) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetFramebufferAttachmentParameteriv_State",
                                             "GL_DEPTH_STENCIL_ATTACHMENT query with different depth and stencil "
                                             "attachment images."));
            return;
        }

        if (!ValidateAttachmentQueryPname(attachmentObject, pname, "GetFramebufferAttachmentParameteriv_State")) return;

        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
            if (attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid()) {
                *params = GL_NONE;
            } else if (attachmentObject->IsTexture()) {
                *params = GL_TEXTURE;
            } else if (attachmentObject->IsRenderbuffer()) {
                *params = GL_RENDERBUFFER;
            } else {
                *params = GL_NONE;
            }
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
            if (attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid()) {
                *params = 0;
            } else if (attachmentObject->IsTexture()) {
                const auto& textureObject = attachmentObject->GetTexture();
                *params = textureObject ? static_cast<GLint>(textureObject->GetExternalIndex()) : 0;
            } else if (attachmentObject->IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject->GetRenderbuffer();
                *params = renderbufferObject ? static_cast<GLint>(renderbufferObject->GetExternalIndex()) : 0;
            } else {
                *params = 0;
            }
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid())
                ? static_cast<GLint>(attachmentObject->GetTextureLevel())
                : 0;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
            if (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid()) {
                const GLenum glUploadTarget =
                    MG_Util::ConvertTextureUploadTargetToGLEnum(attachmentObject->GetTextureUploadTarget());
                switch (glUploadTarget) {
                case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
                case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
                case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
                case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
                    *params = static_cast<GLint>(glUploadTarget);
                    break;
                default:
                    *params = 0;
                    break;
                }
            } else {
                *params = 0;
            }
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid())
                ? static_cast<GLint>(attachmentObject->GetTextureLayer())
                : 0;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_LAYERED:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid() &&
                       attachmentObject->IsLayered())
                ? GL_TRUE
                : GL_FALSE;
            break;
        default:
            if (TryAnswerAttachmentFormatQuery(attachmentObject, attachmentType, depthStencilAlias, pname, params,
                                               "GetFramebufferAttachmentParameteriv_State")) {
                return;
            }
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "GetFramebufferAttachmentParameteriv_State",
                    std::format("pname {} is not an accepted value.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void GenRenderbuffers_State(GLsizei n, GLuint* renderbuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenRenderbuffers_State", "n must be non-negative"));
            return;
        }
        Vector<GLuint> renderbufferNames;
        MG_State::pGLContext->GenRenderbufferNames(n, renderbufferNames);
        Memcpy(renderbuffers, renderbufferNames.data(), sizeof(GLuint) * static_cast<SizeT>(n));
    }

    void CreateRenderbuffers_State(GLsizei n, GLuint* renderbuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateRenderbuffers_State", "n must be non-negative"));
            return;
        }
        if (n > 0 && !renderbuffers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateRenderbuffers_State",
                                             "Renderbuffer output pointer cannot be null."));
            return;
        }

        Vector<GLuint> renderbufferNames;
        MG_State::pGLContext->GenRenderbufferNames(static_cast<Uint>(n), renderbufferNames);
        for (GLsizei i = 0; i < n; ++i) {
            renderbuffers[i] = renderbufferNames[i];
            MG_State::pGLContext->CreateRenderbufferObject(renderbufferNames[i]);
        }
    }

    SharedPtr<MG_State::GLState::RenderbufferObject> GetNamedRenderbufferObject_State(GLuint renderbuffer,
                                                                                     const char* caller) {
        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer, false)) return nullptr;

        auto& renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Renderbuffer object {} does not exist.", renderbuffer)));
            return nullptr;
        }
        return renderbufferObject;
    }

    void NamedRenderbufferStorage_State(GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height) {
        auto renderbufferObject = GetNamedRenderbufferObject_State(renderbuffer, "NamedRenderbufferStorage_State");
        if (!renderbufferObject) return;
        AllocateRenderbufferStorage_State(renderbufferObject, internalformat, width, height,
                                          "NamedRenderbufferStorage_State");
    }

    void NamedRenderbufferStorageMultisample_State(GLuint renderbuffer, GLsizei samples, GLenum internalformat,
                                                   GLsizei width, GLsizei height) {
        auto renderbufferObject =
            GetNamedRenderbufferObject_State(renderbuffer, "NamedRenderbufferStorageMultisample_State");
        if (!renderbufferObject) return;

        TextureInternalFormat format = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!TextureImpl::ValidateTextureInternalFormat(format)) return;
        if (!ValidateRenderbufferStorageSamples_State(samples, format, "NamedRenderbufferStorageMultisample_State"))
            return;
        if (!ValidateRenderbufferStorageSize_State(width, height, "NamedRenderbufferStorageMultisample_State")) return;

        renderbufferObject->AllocateStorage({width, height});
        renderbufferObject->SetInternalFormat(format);
        renderbufferObject->SetSamples(samples);
    }

    void GenFramebuffers_State(GLsizei n, GLuint* framebuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GenFramebuffers_State", "n must be non-negative"));
            return;
        }
        Vector<GLuint> framebuffersNames;
        MG_State::pGLContext->GenFramebufferNames(n, framebuffersNames);
        Memcpy(framebuffers, framebuffersNames.data(), sizeof(GLuint) * static_cast<SizeT>(n));
    }

    void CreateFramebuffers_State(GLsizei n, GLuint* framebuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateFramebuffers_State", "n must be non-negative"));
            return;
        }
        if (n > 0 && !framebuffers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CreateFramebuffers_State",
                                             "Framebuffer output pointer cannot be null."));
            return;
        }

        Vector<GLuint> framebufferNames;
        MG_State::pGLContext->GenFramebufferNames(static_cast<Uint>(n), framebufferNames);
        for (GLsizei i = 0; i < n; ++i) {
            framebuffers[i] = framebufferNames[i];
            MG_State::pGLContext->CreateFramebufferObject(framebufferNames[i]);
        }
    }

    SharedPtr<MG_State::GLState::FramebufferObject> GetNamedFramebufferObject_State(GLuint framebuffer,
                                                                                   const char* caller) {
        if (!FramebufferImpl::ValidateFramebufferName(framebuffer, false)) return nullptr;

        auto& framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Framebuffer object {} does not exist.", framebuffer)));
            return nullptr;
        }
        return framebufferObject;
    }

    // Attaches a single layer/slice of a 3D or array texture. The attachment model stores the layer
    // index; the DirectGLES backend attaches it with glFramebufferTextureLayer.
    static void AttachFramebufferTextureLayer(const char* functionName, GLenum target, GLenum attachment,
                                              GLuint texture, GLint level, GLint layer,
                                              TextureUploadTarget textureUploadTarget) {
        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            AttachFramebufferTextureLayer(functionName, target, GL_DEPTH_ATTACHMENT, texture, level, layer,
                                          textureUploadTarget);
            AttachFramebufferTextureLayer(functionName, target, GL_STENCIL_ATTACHMENT, texture, level, layer,
                                          textureUploadTarget);
            return;
        }

        const FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        const FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto& framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }
        if (layer < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, "Layer must be non-negative."));
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, level, layer,
                                         /*layered=*/false);
    }

    void FramebufferTextureLayer_State(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
        if (texture == 0) {
            const TextureUploadTarget detachTarget = TextureUploadTarget::Texture2D;
            AttachFramebufferTextureWithUploadTarget(__func__, target, attachment, texture, level, detachTarget);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        switch (textureObject->GetTarget()) {
        case TextureTarget::Texture3D:
            textureUploadTarget = TextureUploadTarget::Texture3D;
            break;
        case TextureTarget::Texture2DArray:
            textureUploadTarget = TextureUploadTarget::Texture2DArray;
            break;
        case TextureTarget::Texture2DMultisampleArray:
            textureUploadTarget = TextureUploadTarget::Texture2DMultisampleArray;
            break;
        case TextureTarget::Texture1DArray:
            textureUploadTarget = TextureUploadTarget::Texture1DArray;
            break;
        case TextureTarget::TextureCubeMapArray:
            textureUploadTarget = TextureUploadTarget::CubeMapArray;
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "FramebufferTextureLayer requires a 3D, array, 2D multisample "
                                             "array, or cube map array texture."));
            return;
        }
        // The same backend question the DSA twin asks. GL 4.6 core 9.2.8 makes the two entry points
        // equivalent, so they have to decline in the same places - leaving this one ungated is what
        // let an unrepresentable attachment reach the renderer, and it also refused cube map arrays
        // that GL requires it to accept.
        {
            const auto& layerLimits = MG_Backend::pActiveBackendObject
                                          ? MG_Backend::pActiveBackendObject->GetDynamicParameters()
                                          : MG_Backend::DynamicBackendParameters{};
            const TextureTarget layeredTarget = textureObject->GetTarget();
            if ((layer != 0 || layeredTarget == TextureTarget::TextureCubeMapArray) &&
                !layerLimits.SupportsPerLayerFramebufferAttachment(layeredTarget)) {
                RecordUnsupportedFramebufferTextureAttachmentError(
                    __func__, "This backend does not resolve a framebuffer attachment's layer onto its image.");
                return;
            }
        }
        AttachFramebufferTextureLayer(__func__, target, attachment, texture, level, layer, textureUploadTarget);
    }

    void FramebufferTexture3D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                                    GLint zoffset) {
        if (texture == 0) {
            TextureUploadTarget textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(textarget);
            if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
            AttachFramebufferTextureWithUploadTarget(__func__, target, attachment, texture, level, textureUploadTarget);
            return;
        }

        if (textarget != GL_TEXTURE_3D) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "FramebufferTexture3D requires GL_TEXTURE_3D."));
            return;
        }
        AttachFramebufferTextureLayer(__func__, target, attachment, texture, level, zoffset,
                                      TextureUploadTarget::Texture3D);
    }

    void FramebufferTexture2D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }

        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            FramebufferTexture2D_State(target, GL_DEPTH_ATTACHMENT, textarget, texture, level);
            FramebufferTexture2D_State(target, GL_STENCIL_ATTACHMENT, textarget, texture, level);
            return;
        }

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);

        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        if (texture != 0) {
            textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(textarget);
            if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        }

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto& framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferTexture2D_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferTexture2D_State",
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }

        const auto expectedTextureTarget = MG_Util::ConvertTextureUploadTargetToTextureTarget(textureUploadTarget);
        if (expectedTextureTarget == TextureTarget::Unknown ||
            textureObject->GetTarget() != expectedTextureTarget) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "FramebufferTexture2D_State",
                    std::format("Attachment target {} does not match texture {} target {}.",
                                MG_Util::ConvertGLEnumToString(textarget),
                                texture,
                                MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, textureObject ? level : 0);
    }

    void FramebufferTexture1D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        if (texture != 0) {
            textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(textarget);
            if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        } else {
            textureUploadTarget = TextureUploadTarget::Texture1D;
        }
        AttachFramebufferTextureWithUploadTarget(__func__, target, attachment, texture, level, textureUploadTarget);
    }

    void FramebufferTexture_State(GLenum target, GLenum attachment, GLuint texture, GLint level) {
        if (texture == 0) {
            AttachFramebufferTextureWithUploadTarget(__func__, target, attachment, texture, level,
                                                     TextureUploadTarget::Texture2D);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }

        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        Bool layered = false;
        if (!ResolveRepresentableFramebufferTextureUploadTarget(*textureObject, textureUploadTarget, layered)) {
            RecordUnsupportedFramebufferTextureAttachmentError(
                __func__,
                "Layered or multi-image framebuffer texture targets are not fully represented by the current framebuffer attachment model.");
            return;
        }

        AttachFramebufferTextureWithUploadTarget(__func__, target, attachment, texture, level, textureUploadTarget,
                                                 layered);
    }

    void NamedFramebufferTexture_State(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            NamedFramebufferTexture_State(framebuffer, GL_DEPTH_ATTACHMENT, texture, level);
            NamedFramebufferTexture_State(framebuffer, GL_STENCIL_ATTACHMENT, texture, level);
            return;
        }

        auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferTexture_State");
        if (!framebufferObject) return;

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, "NamedFramebufferTexture_State")) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedFramebufferTexture_State",
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "NamedFramebufferTexture_State",
                                             "Texture level must be non-negative."));
            return;
        }

        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        Bool layered = false;
        if (!ResolveRepresentableFramebufferTextureUploadTarget(*textureObject, textureUploadTarget, layered)) {
            RecordUnsupportedFramebufferTextureAttachmentError(
                "NamedFramebufferTexture_State",
                "Layered or multi-image framebuffer texture targets are not fully represented by the current framebuffer attachment model.");
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, level, 0, layered);
    }

    void NamedFramebufferTextureWithUploadTarget_State(const char* functionName, GLuint framebuffer, GLenum attachment,
                                                       GLuint texture, GLint level,
                                                       TextureUploadTarget textureUploadTarget) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            NamedFramebufferTextureWithUploadTarget_State(functionName, framebuffer, GL_DEPTH_ATTACHMENT, texture,
                                                         level, textureUploadTarget);
            NamedFramebufferTextureWithUploadTarget_State(functionName, framebuffer, GL_STENCIL_ATTACHMENT, texture,
                                                         level, textureUploadTarget);
            return;
        }

        auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, functionName);
        if (!framebufferObject) return;

        const FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, functionName)) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName,
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }

        const auto expectedTextureTarget = MG_Util::ConvertTextureUploadTargetToTextureTarget(textureUploadTarget);
        if (expectedTextureTarget == TextureTarget::Unknown ||
            textureObject->GetTarget() != expectedTextureTarget) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", functionName,
                    std::format("Attachment target {} does not match texture {} target {}.",
                                MG_Util::ConvertTextureUploadTargetToString(textureUploadTarget),
                                texture,
                                MG_Util::ConvertTextureTargetToString(textureObject->GetTarget()))));
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, level);
    }

    void NamedFramebufferTexture1D_State(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                         GLint level) {
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Texture1D;
        if (texture != 0) {
            textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(textarget);
            if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        }
        NamedFramebufferTextureWithUploadTarget_State(__func__, framebuffer, attachment, texture, level,
                                                      textureUploadTarget);
    }

    void NamedFramebufferTexture2D_State(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                         GLint level) {
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Texture2D;
        if (texture != 0) {
            textureUploadTarget = MG_Util::ConvertGLEnumToTextureUploadTarget(textarget);
            if (!TextureImpl::ValidateTextureUploadTarget(textureUploadTarget)) return;
        }
        NamedFramebufferTextureWithUploadTarget_State(__func__, framebuffer, attachment, texture, level,
                                                      textureUploadTarget);
    }

    void NamedFramebufferTexture3D_State(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                         GLint level, GLint zoffset) {
        static_cast<void>(framebuffer);
        static_cast<void>(attachment);
        static_cast<void>(textarget);
        static_cast<void>(texture);
        static_cast<void>(level);
        static_cast<void>(zoffset);
        RecordUnsupportedFramebufferTextureAttachmentError(
            __func__,
            "3D framebuffer texture slice attachments are not represented by the current framebuffer attachment model.");
    }

    void NamedFramebufferTextureLayer_State(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level,
                                            GLint layer) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            NamedFramebufferTextureLayer_State(framebuffer, GL_DEPTH_ATTACHMENT, texture, level, layer);
            NamedFramebufferTextureLayer_State(framebuffer, GL_STENCIL_ATTACHMENT, texture, level, layer);
            return;
        }

        auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, __func__);
        if (!framebufferObject) return;

        const FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, __func__)) return;

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        // Unlike NamedFramebufferTexture, this entry point answers INVALID_OPERATION - not
        // INVALID_VALUE - for a name that is not a texture, so the name check below is the object
        // lookup rather than TextureImpl::ValidateTextureName.
        auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Texture level must be non-negative."));
            return;
        }
        if (layer < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Layer must be non-negative."));
            return;
        }

        // GL 4.6 core 9.2.8: only the layered targets have layers to select, and the largest layer
        // each one admits comes from a different implementation limit.
        const auto& limits = MG_Backend::pActiveBackendObject
            ? MG_Backend::pActiveBackendObject->GetDynamicParameters()
            : MG_Backend::DynamicBackendParameters{};
        TextureUploadTarget textureUploadTarget = TextureUploadTarget::Unknown;
        Int layerLimit = 0;
        switch (textureObject->GetTarget()) {
        case TextureTarget::Texture3D:
            textureUploadTarget = TextureUploadTarget::Texture3D;
            layerLimit = limits.Max3DTextureSize;
            break;
        case TextureTarget::Texture1DArray:
            textureUploadTarget = TextureUploadTarget::Texture1DArray;
            layerLimit = limits.MaxArrayTextureLayers;
            break;
        case TextureTarget::Texture2DArray:
            textureUploadTarget = TextureUploadTarget::Texture2DArray;
            layerLimit = limits.MaxArrayTextureLayers;
            break;
        case TextureTarget::Texture2DMultisampleArray:
            textureUploadTarget = TextureUploadTarget::Texture2DMultisampleArray;
            layerLimit = limits.MaxArrayTextureLayers;
            break;
        case TextureTarget::TextureCubeMapArray:
            textureUploadTarget = TextureUploadTarget::CubeMapArray;
            // A cube map array is an array texture whose layers happen to be cube faces, so its
            // layer index is bounded by GL_MAX_ARRAY_TEXTURE_LAYERS like any other array's.
            layerLimit = limits.MaxArrayTextureLayers;
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "NamedFramebufferTextureLayer requires a 3D, array, 2D multisample "
                                             "array, or cube map array texture."));
            return;
        }

        if (layer >= layerLimit) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             std::format("Layer {} is beyond the limit ({}) for this texture target.",
                                                         layer, layerLimit)));
            return;
        }

        // Everything above is the spec's error set and is answered for every target and every layer
        // on every backend. Whether the attachment can actually be honoured is a backend question:
        // DirectGLES hands the layer to glFramebufferTextureLayer and renders to it, while
        // DirectVulkan maps a GL layer onto a Vulkan array layer with no notion of a 3D depth slice,
        // so a slice lands outside the image and the renderer asserts on the clear. Letting it
        // through there would only move the failure downstream, so it is declined instead - layer
        // zero always works, being the plain first-slice attachment.
        // ...and it is a DIFFERENT question per target: a 2D/2D-multisample array layer is a Vulkan
        // array layer, a 3D layer is a z slice, and a cube map array needs a cube-compatible image
        // before it has any layer to name. Ask the backend about this texture's target rather than
        // guessing from one blanket flag.
        const TextureTarget layeredTextureTarget = textureObject->GetTarget();
        const Bool backsThisTargetsLayers = limits.SupportsPerLayerFramebufferAttachment(layeredTextureTarget);
        // Layer zero of a 3D or array texture is the plain first-slice attachment every backend can
        // already express, so it stays legal even where per-layer selection is not backed. A cube map
        // array has no such fallback: layer zero is still one face of one cube inside a
        // cube-compatible image, so it needs the same support layer 5 does.
        const Bool needsPerLayerSupport =
            layer != 0 || layeredTextureTarget == TextureTarget::TextureCubeMapArray;
        if (needsPerLayerSupport && !backsThisTargetsLayers) {
            RecordUnsupportedFramebufferTextureAttachmentError(
                __func__, "This backend does not resolve a framebuffer attachment's layer onto its image.");
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureUploadTarget, level, layer,
                                         /*layered=*/false);
    }

    void FramebufferRenderbuffer_State(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                       GLuint renderbuffer) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            FramebufferRenderbuffer_State(target, GL_DEPTH_ATTACHMENT, renderbuffertarget, renderbuffer);
            FramebufferRenderbuffer_State(target, GL_STENCIL_ATTACHMENT, renderbuffertarget, renderbuffer);
            return;
        }

        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(renderbuffertarget);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, "FramebufferRenderbuffer_State")) return;
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!FramebufferImpl::ValidateRenderbufferTarget(rbTarget)) return;
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto& framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferRenderbuffer_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (renderbuffer == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer, false)) return;
        auto& renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferRenderbuffer_State",
                                             std::format("Renderbuffer object {} is not valid.", renderbuffer)));
            return;
        }

        framebufferObject->AttachRenderbuffer(attachmentType, renderbufferObject);
    }

    void NamedFramebufferRenderbuffer_State(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget,
                                            GLuint renderbuffer) {
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            NamedFramebufferRenderbuffer_State(framebuffer, GL_DEPTH_ATTACHMENT, renderbuffertarget, renderbuffer);
            NamedFramebufferRenderbuffer_State(framebuffer, GL_STENCIL_ATTACHMENT, renderbuffertarget, renderbuffer);
            return;
        }

        auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferRenderbuffer_State");
        if (!framebufferObject) return;

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(renderbuffertarget);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, "NamedFramebufferRenderbuffer_State"))
            return;
        if (!FramebufferImpl::ValidateRenderbufferTarget(rbTarget)) return;

        if (renderbuffer == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer, false)) return;
        auto renderbufferObject =
            GetNamedRenderbufferObject_State(renderbuffer, "NamedFramebufferRenderbuffer_State");
        if (!renderbufferObject) return;

        framebufferObject->AttachRenderbuffer(attachmentType, renderbufferObject);
    }

    void DrawBuffersForFramebuffer_State(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo, Bool isDefaultFBO,
                                         GLsizei n, const GLenum* bufs, Bool allowDefaultFBOAliases) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`n` is less than 0."));
            return;
        } else if (n > MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`n` is greater than `GL_MAX_DRAW_BUFFERS`."));
            return;
        }
        if (!fbo) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "Framebuffer object is null."));
            return;
        }

        static int existenceMap[(SizeT)FramebufferAttachmentType::FramebufferAttachmentTypeCount] = {-1};
        std::fill(existenceMap, existenceMap + (SizeT)FramebufferAttachmentType::FramebufferAttachmentTypeCount, -1);

        // GL 4.6 core 17.4.1: BACK names both back buffers at once, so DrawBuffers only takes it as
        // the whole list. DrawBuffer, which names one buffer, is exempt.
        if (!allowDefaultFBOAliases && n != 1) {
            for (GLsizei i = 0; i < n; ++i) {
                if (bufs[i] != GL_BACK) continue;
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 "GL_BACK may only appear in bufs when n is one."));
                return;
            }
        }

        for (GLsizei i = 0; i < n; ++i) {
            // FRONT, LEFT, RIGHT and FRONT_AND_BACK each stand for more than one buffer, so
            // DrawBuffers rejects them outright - on a framebuffer object as well as on the default
            // framebuffer. LEFT, RIGHT and FRONT_AND_BACK are not attachment names at all and fall
            // out of the conversion below; only FRONT needs saying here.
            if (!allowDefaultFBOAliases && bufs[i] == GL_FRONT) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format("glDrawBuffers cannot use the multi-buffer name {}.",
                                    MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            auto attType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(bufs[i]);

            // ------------------- Check validity begin ------------------------
            if (attType == FramebufferAttachmentType::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::format("bufs[{}] = {} is not an accepted value.", i,
                                                             MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            // A name that is well formed but belongs to the other kind of framebuffer is
            // INVALID_OPERATION, not INVALID_ENUM: the enum is accepted, this framebuffer just has
            // no such buffer.
            if (isDefaultFBO && attType != FramebufferAttachmentType::None &&
                (attType < FramebufferAttachmentType::FrontLeft || attType > FramebufferAttachmentType::BackRight)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format("FBO is default FBO, but bufs[{}] = {} is not `GL_NONE` or one of the default "
                                    "framebuffer color buffer tokens.",
                                    i, MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            if (!isDefaultFBO && attType != FramebufferAttachmentType::None &&
                (attType < FramebufferAttachmentType::Color0 || attType > FramebufferAttachmentType::Color31)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format("FBO is not default FBO, but bufs[{}] = {} is anything other than `GL_NONE` or "
                                    "one of the `GL_COLOR_ATTACHMENTn` tokens.",
                                    i, MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            if (attType != FramebufferAttachmentType::None && existenceMap[(SizeT)attType] >= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::format("a symbolic constant other than `GL_NONE` appears "
                                                             "more than once in bufs. bufs[{}] == bufs[{}] == {}.",
                                                             i, existenceMap[(SizeT)attType],
                                                             MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            existenceMap[(SizeT)attType] = i;

            if (!FramebufferImpl::ValidateColorAttachmentInRange(attType, __func__)) return;
            // ------------------------- Check validity end ----------------------------------
            fbo->SetDrawBuffer(i, attType);
        }
        for (GLsizei i = n; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            fbo->SetDrawBuffer(i, FramebufferAttachmentType::None);
        }
    }

    void DrawBuffers_State(GLsizei n, const GLenum* bufs) {
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
        auto& fbo = bindingSlot.GetBoundObject();
        const bool isDefaultFBO = (fbo == FramebufferImpl::pDefaultFramebufferInfo->defaultFBO);
        DrawBuffersForFramebuffer_State(fbo, isDefaultFBO, n, bufs, false);
    }

    void DrawBuffer_State(GLenum buf) {
        if (buf == GL_NONE) {
            DrawBuffers_State(0, nullptr);
        } else {
            auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
            auto& fbo = bindingSlot.GetBoundObject();
            const bool isDefaultFBO = (fbo == FramebufferImpl::pDefaultFramebufferInfo->defaultFBO);
            const GLenum bufs[] = {buf};
            DrawBuffersForFramebuffer_State(fbo, isDefaultFBO, 1, bufs, true);
        }
    }

    void ReadBufferForFramebuffer_State(const SharedPtr<MG_State::GLState::FramebufferObject>& fbo, Bool isDefaultFBO,
                                        GLenum src, const char* caller) {
        auto attType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(src);

        // ------------------- Check validity begin ------------------------
        if (attType == FramebufferAttachmentType::Unknown) {
            // LEFT, RIGHT and FRONT_AND_BACK are table 17.4 names - accepted enums - that stand for
            // more than one buffer, so ReadBuffer cannot select them. That makes them
            // INVALID_OPERATION rather than the INVALID_ENUM a name outside the table gets.
            const Bool isMultiBufferName = src == GL_LEFT || src == GL_RIGHT || src == GL_FRONT_AND_BACK;
            MG_State::pGLContext->RecordError(
                isMultiBufferName ? ErrorCode::InvalidOperation : ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("`src` = {} does not name a single readable colour buffer.",
                                MG_Util::ConvertGLEnumToString(src))));
            return;
        }

        if (!fbo) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "No framebuffer bound to read target."));
            return;
        }

        // Naming a buffer the other kind of framebuffer has is INVALID_OPERATION rather than
        // INVALID_ENUM (GL 4.6 core 17.4.1) - the name is accepted, this framebuffer just has no
        // such colour buffer.
        if (isDefaultFBO && attType != FramebufferAttachmentType::None &&
            (attType < FramebufferAttachmentType::FrontLeft || attType > FramebufferAttachmentType::BackRight)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("Default framebuffer read buffer {} is not valid.", MG_Util::ConvertGLEnumToString(src))));
            return;
        }
        if (!isDefaultFBO && attType != FramebufferAttachmentType::None &&
            (attType < FramebufferAttachmentType::Color0 || attType > FramebufferAttachmentType::Color31)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             std::format("Framebuffer object read buffer {} is not valid.",
                                                         MG_Util::ConvertGLEnumToString(src))));
            return;
        }
        if (!isDefaultFBO && !FramebufferImpl::ValidateColorAttachmentInRange(attType, caller)) return;
        fbo->SetReadBuffer(attType);
    }

    void ReadBuffer_State(GLenum mode) {
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read);
        auto& fbo = bindingSlot.GetBoundObject();
        const Bool isDefaultFBO = (fbo == FramebufferImpl::pDefaultFramebufferInfo->defaultFBO);
        ReadBufferForFramebuffer_State(fbo, isDefaultFBO, mode, __func__);
    }

    void NamedFramebufferDrawBuffers_State(GLuint framebuffer, GLsizei n, const GLenum* bufs) {
        // Zero names the default framebuffer, whose accepted buffer names are a different set - so
        // it cannot go through the framebuffer-object lookup, which rejects the name outright.
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferDrawBuffers_State");
        if (!framebufferObject) return;
        DrawBuffersForFramebuffer_State(framebufferObject, framebuffer == 0, n, bufs, false);
    }

    void NamedFramebufferDrawBuffer_State(GLuint framebuffer, GLenum buf) {
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferDrawBuffer_State");
        if (!framebufferObject) return;

        if (buf == GL_NONE) {
            DrawBuffersForFramebuffer_State(framebufferObject, framebuffer == 0, 0, nullptr, true);
        } else {
            const GLenum bufs[] = {buf};
            DrawBuffersForFramebuffer_State(framebufferObject, framebuffer == 0, 1, bufs, true);
        }
    }

    void NamedFramebufferReadBuffer_State(GLuint framebuffer, GLenum src) {
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferReadBuffer_State");
        if (!framebufferObject) return;
        ReadBufferForFramebuffer_State(framebufferObject, framebuffer == 0, src,
                                       "NamedFramebufferReadBuffer_State");
    }

    SharedPtr<MG_State::GLState::FramebufferObject> GetFramebufferObjectForNamedClear(GLuint framebuffer,
                                                                                      const char* caller) {
        return framebuffer == 0 ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
                                : GetNamedFramebufferObject_State(framebuffer, caller);
    }

    Bool ValidateNamedClearfv_State(GLenum buffer, GLint drawbuffer, const GLfloat* value, const char* caller) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "value pointer cannot be null."));
            return false;
        }

        switch (buffer) {
        case GL_COLOR:
            if (drawbuffer < 0 ||
                drawbuffer >= static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "color drawbuffer index is out of range."));
                return false;
            }
            return true;
        case GL_DEPTH:
            if (drawbuffer != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "depth clear requires drawbuffer 0."));
                return false;
            }
            return true;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("buffer {} is not accepted for glClearNamedFramebufferfv.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
    }

    Bool ValidateNamedClearfi_State(GLenum buffer, GLint drawbuffer, const char* caller) {
        if (buffer != GL_DEPTH_STENCIL) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("buffer {} is not accepted for glClearNamedFramebufferfi.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
        if (drawbuffer != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "depth/stencil clear requires drawbuffer 0."));
            return false;
        }
        return true;
    }

    void ClearNamedFramebufferfv_State(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        auto framebufferObject = GetFramebufferObjectForNamedClear(framebuffer, "ClearNamedFramebufferfv_State");
        if (!framebufferObject) return;
        if (!ValidateNamedClearfv_State(buffer, drawbuffer, value, "ClearNamedFramebufferfv_State")) return;
        ClearNamedFramebufferfv_Backend(framebufferObject, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfi_State(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth,
                                      GLint stencil) {
        auto framebufferObject = GetFramebufferObjectForNamedClear(framebuffer, "ClearNamedFramebufferfi_State");
        if (!framebufferObject) return;
        if (!ValidateNamedClearfi_State(buffer, drawbuffer, "ClearNamedFramebufferfi_State")) return;
        ClearNamedFramebufferfi_Backend(framebufferObject, buffer, drawbuffer, depth, stencil);
    }

    // Which buffers the integer clears accept is narrower than the float one, and differs between
    // them: signed values can clear COLOR or STENCIL, unsigned only COLOR (GL 4.6 core 17.4.3.1).
    // Only the colour buffer is indexed; a stencil clear names the single stencil buffer, so any
    // drawbuffer other than 0 is out of range rather than merely unused.
    Bool ValidateNamedClearIntegerv_State(GLenum buffer, GLint drawbuffer, const void* value, Bool allowStencil,
                                          const char* caller) {
        if (!value) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "value pointer cannot be null."));
            return false;
        }

        if (buffer == GL_COLOR) {
            if (drawbuffer < 0 ||
                drawbuffer >= static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "color drawbuffer index is out of range."));
                return false;
            }
            return true;
        }

        if (buffer == GL_STENCIL && allowStencil) {
            if (drawbuffer != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "stencil clear requires drawbuffer 0."));
                return false;
            }
            return true;
        }

        MG_State::pGLContext->RecordError(
            ErrorCode::InvalidEnum,
            MakeUnique<GenericErrorInfo>(
                "MG_Impl/GLImpl", caller,
                std::format("buffer {} is not accepted for this clear.", MG_Util::ConvertGLEnumToString(buffer))));
        return false;
    }

    void ClearNamedFramebufferiv_State(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint* value) {
        auto framebufferObject = GetFramebufferObjectForNamedClear(framebuffer, "ClearNamedFramebufferiv_State");
        if (!framebufferObject) return;
        if (!ValidateNamedClearIntegerv_State(buffer, drawbuffer, value, true, "ClearNamedFramebufferiv_State")) {
            return;
        }
        ClearNamedFramebufferiv_Backend(framebufferObject, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferuiv_State(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint* value) {
        auto framebufferObject = GetFramebufferObjectForNamedClear(framebuffer, "ClearNamedFramebufferuiv_State");
        if (!framebufferObject) return;
        if (!ValidateNamedClearIntegerv_State(buffer, drawbuffer, value, false, "ClearNamedFramebufferuiv_State")) {
            return;
        }
        ClearNamedFramebufferuiv_Backend(framebufferObject, buffer, drawbuffer, value);
    }

    // glInvalidateFramebuffer and its three siblings only grant the implementation permission to
    // throw the named attachments' contents away - "become undefined" is satisfied by keeping them
    // - so MobileGL validates the call and leaves the contents alone. Discarding is a bandwidth
    // optimisation that would need a backend dependency; it can be added later without changing
    // what any of these entry points promise. The conformance tests exercise the validation, which
    // is the part that was missing.
    Bool ValidateInvalidateAttachments_State(const SharedPtr<MG_State::GLState::FramebufferObject>& framebuffer,
                                             GLsizei numAttachments, const GLenum* attachments, const char* caller) {
        if (numAttachments < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "numAttachments cannot be negative."));
            return false;
        }
        if (numAttachments > 0 && attachments == nullptr) {
            return false;
        }

        // Which tokens name an attachment depends on which framebuffer is affected: the default one
        // has buffers, a framebuffer object has attachment points (GL 4.6 core 9.2.7).
        const Bool isDefaultFramebuffer = framebuffer->IsDefaultFramebuffer();
        for (GLsizei at = 0; at < numAttachments; ++at) {
            const GLenum attachment = attachments[at];
            if (isDefaultFramebuffer) {
                switch (attachment) {
                // The by-name forms name the default framebuffer's buffers the way
                // glClearNamedFramebuffer does, with COLOR standing for the colour buffer, while
                // the target forms use the individual FRONT_LEFT/BACK_RIGHT tokens. Both spellings
                // reach here, so both are accepted (GL 4.6 core 17.4.4).
                case GL_COLOR:
                case GL_FRONT_LEFT:
                case GL_FRONT_RIGHT:
                case GL_BACK_LEFT:
                case GL_BACK_RIGHT:
                case GL_DEPTH:
                case GL_STENCIL:
                    continue;
                default:
                    MG_State::pGLContext->RecordError(
                        ErrorCode::InvalidEnum,
                        MakeUnique<GenericErrorInfo>(
                            "MG_Impl/GLImpl", caller,
                            std::format("{} does not name a buffer of the default framebuffer.",
                                        MG_Util::ConvertGLEnumToString(attachment))));
                    return false;
                }
            }

            if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT ||
                attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
                continue;
            }
            if (attachment < GL_COLOR_ATTACHMENT0 || attachment > GL_COLOR_ATTACHMENT31) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeUnique<GenericErrorInfo>(
                        "MG_Impl/GLImpl", caller,
                        std::format("{} does not name an attachment point of a framebuffer object.",
                                    MG_Util::ConvertGLEnumToString(attachment))));
                return false;
            }
            // A COLOR_ATTACHMENTm token past the limit is a well-formed enum naming a point that
            // does not exist, which is INVALID_OPERATION rather than INVALID_ENUM.
            const auto attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
            if (!FramebufferImpl::ValidateColorAttachmentInRange(attachmentType, caller)) return false;
        }
        return true;
    }

    Bool ValidateInvalidateSubRegion_State(GLsizei width, GLsizei height, const char* caller) {
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "width and height cannot be negative."));
            return false;
        }
        return true;
    }

    SharedPtr<MG_State::GLState::FramebufferObject> GetFramebufferObjectForInvalidate_State(GLenum target,
                                                                                           const char* caller) {
        const FramebufferTarget framebufferTarget =
            MG_Util::ConvertGLEnumToFramebufferTarget(target == GL_FRAMEBUFFER ? GL_DRAW_FRAMEBUFFER : target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return nullptr;
        auto framebufferObject = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget).GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Framebuffer target is bound to no framebuffer object."));
        }
        return framebufferObject;
    }

    void InvalidateNamedFramebufferData_State(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments) {
        auto framebufferObject =
            GetFramebufferObjectForNamedClear(framebuffer, "InvalidateNamedFramebufferData_State");
        if (!framebufferObject) return;
        ValidateInvalidateAttachments_State(framebufferObject, numAttachments, attachments,
                                            "InvalidateNamedFramebufferData_State");
    }

    void InvalidateNamedFramebufferSubData_State(GLuint framebuffer, GLsizei numAttachments,
                                                 const GLenum* attachments, GLint x, GLint y, GLsizei width,
                                                 GLsizei height) {
        (void)x;
        (void)y;
        auto framebufferObject =
            GetFramebufferObjectForNamedClear(framebuffer, "InvalidateNamedFramebufferSubData_State");
        if (!framebufferObject) return;
        if (!ValidateInvalidateAttachments_State(framebufferObject, numAttachments, attachments,
                                                 "InvalidateNamedFramebufferSubData_State")) {
            return;
        }
        ValidateInvalidateSubRegion_State(width, height, "InvalidateNamedFramebufferSubData_State");
    }

    void InvalidateFramebuffer_State(GLenum target, GLsizei numAttachments, const GLenum* attachments) {
        auto framebufferObject = GetFramebufferObjectForInvalidate_State(target, "InvalidateFramebuffer_State");
        if (!framebufferObject) return;
        ValidateInvalidateAttachments_State(framebufferObject, numAttachments, attachments,
                                            "InvalidateFramebuffer_State");
    }

    void InvalidateSubFramebuffer_State(GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x,
                                        GLint y, GLsizei width, GLsizei height) {
        (void)x;
        (void)y;
        auto framebufferObject = GetFramebufferObjectForInvalidate_State(target, "InvalidateSubFramebuffer_State");
        if (!framebufferObject) return;
        if (!ValidateInvalidateAttachments_State(framebufferObject, numAttachments, attachments,
                                                 "InvalidateSubFramebuffer_State")) {
            return;
        }
        ValidateInvalidateSubRegion_State(width, height, "InvalidateSubFramebuffer_State");
    }

    void DeleteRenderbuffers_State(GLsizei n, const GLuint* renderbuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteRenderbuffers_State", "n must be non-negative."));
            return;
        }

        if (!renderbuffers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteRenderbuffers_State",
                                                                      "Renderbuffer names array cannot be null."));
            return;
        }

        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint bufferName = renderbuffers[i];
            if (bufferName == 0) continue;
            // GL 3.3 core 4.4.2: unknown names are silently ignored on delete; the shared bind-path
            // validator would record INVALID_OPERATION instead.
            if (!MG_State::pGLContext->ValidateRenderbufferName(bufferName)) continue;
            MG_State::pGLContext->MarkRenderbufferObjectForDeletion(bufferName);
        }
    }

    void DeleteFramebuffers_State(GLsizei n, const GLuint* framebuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteFramebuffers_State", "n must be non-negative."));
            return;
        }

        if (!framebuffers) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteFramebuffers_State",
                                                                           "Framebuffer names array cannot be null."));
            return;
        }

        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint bufferName = framebuffers[i];
            if (bufferName == 0) continue;
            // GL 3.3 core 4.4.1: unknown names are silently ignored on delete; the shared bind-path
            // validator would record INVALID_OPERATION instead.
            if (!MG_State::pGLContext->ValidateFramebufferName(bufferName)) continue;
            MG_State::pGLContext->MarkFramebufferObjectForDeletion(bufferName);
        }
    }

    GLenum CheckFramebufferStatus_State(GLenum target) {
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return GL_FRAMEBUFFER_UNDEFINED;

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto& framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CheckFramebufferStatus_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return GL_FRAMEBUFFER_UNDEFINED;
        }

        // TODO: distinguish GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT and GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
        // TODO: GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER, GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER,
        //       additional GL_FRAMEBUFFER_UNSUPPORTED cases, GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE,
        //       GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS
        if (!framebufferObject->CheckCompleteness()) {
            return HasDefinedAttachment(*framebufferObject) ?
                GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
                GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
        }
        if (HasNonRenderableColorAttachment(*framebufferObject)) {
            return GL_FRAMEBUFFER_UNSUPPORTED;
        }
        if (ActiveBackendRejectsDistinctDepthStencil() &&
            HasUnsupportedDistinctDepthStencilAttachments(*framebufferObject)) {
            return GL_FRAMEBUFFER_UNSUPPORTED;
        }
        return GL_FRAMEBUFFER_COMPLETE;
    }

    GLenum CheckNamedFramebufferStatus_State(GLuint framebuffer, GLenum target) {
        if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "CheckNamedFramebufferStatus_State",
                                             std::format("target {} is not accepted.",
                                                         MG_Util::ConvertGLEnumToString(target))));
            return GL_FRAMEBUFFER_UNDEFINED;
        }

        auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, "CheckNamedFramebufferStatus_State");
        if (!framebufferObject) return GL_FRAMEBUFFER_UNDEFINED;

        if (!framebufferObject->CheckCompleteness()) {
            return HasDefinedAttachment(*framebufferObject) ?
                GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
                GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
        }
        if (HasNonRenderableColorAttachment(*framebufferObject)) {
            return GL_FRAMEBUFFER_UNSUPPORTED;
        }
        if (ActiveBackendRejectsDistinctDepthStencil() &&
            HasUnsupportedDistinctDepthStencilAttachments(*framebufferObject)) {
            return GL_FRAMEBUFFER_UNSUPPORTED;
        }
        return GL_FRAMEBUFFER_COMPLETE;
    }

    // GL_ARB_framebuffer_no_attachments plus the queryable framebuffer state of GL 4.6 core 9.2.3.
    // The DEFAULT_* half is real state on the framebuffer object; the rest is derived from the
    // attachments, and matches what glGetIntegerv answers for the bound framebuffer.
    void GetFramebufferParameteriv_Object(const SharedPtr<MG_State::GLState::FramebufferObject>& framebufferObject,
                                          GLenum pname, GLint* params, const char* caller) {
        if (params == nullptr) return;
        if (!FramebufferImpl::ValidateFramebufferParameterPname(pname, framebufferObject->IsDefaultFramebuffer(),
                                                                /*forSetter=*/false, caller)) {
            return;
        }

        const auto resolveSampleCount = [&]() {
            GLint maxSamples = 0;
            for (const auto& attachment : framebufferObject->GetAllAttachmentObjects()) {
                if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
                    maxSamples = std::max(maxSamples, static_cast<GLint>(attachment.GetRenderbuffer()->GetSamples()));
                } else if (attachment.IsTexture() && attachment.GetTexture()) {
                    maxSamples = std::max(maxSamples, static_cast<GLint>(attachment.GetTexture()->GetSamples()));
                }
            }
            return maxSamples;
        };

        switch (pname) {
        case GL_FRAMEBUFFER_DEFAULT_WIDTH:
            *params = framebufferObject->GetDefaultWidth();
            break;
        case GL_FRAMEBUFFER_DEFAULT_HEIGHT:
            *params = framebufferObject->GetDefaultHeight();
            break;
        case GL_FRAMEBUFFER_DEFAULT_LAYERS:
            *params = framebufferObject->GetDefaultLayers();
            break;
        case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
            *params = framebufferObject->GetDefaultSamples();
            break;
        case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
            *params = framebufferObject->GetDefaultFixedSampleLocations() ? GL_TRUE : GL_FALSE;
            break;
        case GL_SAMPLES:
            *params = resolveSampleCount();
            break;
        case GL_SAMPLE_BUFFERS:
            *params = resolveSampleCount() > 0 ? 1 : 0;
            break;
        case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
            const auto readBuffer = framebufferObject->GetReadBuffer();
            TextureInternalFormat internalFormat = TextureInternalFormat::Unknown;
            if (readBuffer != FramebufferAttachmentType::None) {
                const auto& attachment = framebufferObject->GetAttachment(readBuffer);
                if (attachment.IsTexture() && attachment.GetTexture()) {
                    internalFormat = attachment.GetTexture()->GetFormat();
                } else if (attachment.IsRenderbuffer() && attachment.GetRenderbuffer()) {
                    internalFormat = attachment.GetRenderbuffer()->GetInternalFormat();
                }
            }
            if (internalFormat == TextureInternalFormat::Unknown) {
                *params = 0;
                break;
            }
            const GLenum glInternalFormat = MG_Util::ConvertTextureInternalFormatToGLEnum(internalFormat);
            GLenum normalizedInternalFormat = glInternalFormat;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
            MG_Util::TextureFormatProcessor::NormalizePixelFormat(glInternalFormat, PixelFormatNormalizeOptionBit::None,
                                                                  &normalizedInternalFormat, &format, &type);
            *params = static_cast<GLint>(pname == GL_IMPLEMENTATION_COLOR_READ_FORMAT ? format : type);
            break;
        }
        case GL_DOUBLEBUFFER:
            // Only the window-system surface is ever double buffered; a framebuffer object never is.
            *params = framebufferObject->IsDefaultFramebuffer() ? GL_TRUE : GL_FALSE;
            break;
        case GL_STEREO:
            // Stereo surfaces are not exposed, matching what glGetIntegerv reports.
            *params = GL_FALSE;
            break;
        default:
            break;
        }
    }

    void FramebufferParameteri_Object(const SharedPtr<MG_State::GLState::FramebufferObject>& framebufferObject,
                                      GLenum pname, GLint param, const char* caller) {
        if (!FramebufferImpl::ValidateFramebufferParameterPname(pname, framebufferObject->IsDefaultFramebuffer(),
                                                                /*forSetter=*/true, caller)) {
            return;
        }
        if (pname != GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS && param < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "Framebuffer default parameters must be non-negative."));
            return;
        }

        switch (pname) {
        case GL_FRAMEBUFFER_DEFAULT_WIDTH:
            framebufferObject->SetDefaultWidth(param);
            break;
        case GL_FRAMEBUFFER_DEFAULT_HEIGHT:
            framebufferObject->SetDefaultHeight(param);
            break;
        case GL_FRAMEBUFFER_DEFAULT_LAYERS:
            framebufferObject->SetDefaultLayers(param);
            break;
        case GL_FRAMEBUFFER_DEFAULT_SAMPLES:
            framebufferObject->SetDefaultSamples(param);
            break;
        case GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS:
            framebufferObject->SetDefaultFixedSampleLocations(param != GL_FALSE);
            break;
        default:
            break;
        }
    }

    void GetFramebufferAttachmentParameteriv_Object(
        const SharedPtr<MG_State::GLState::FramebufferObject>& framebufferObject, GLenum attachment, GLenum pname,
        GLint* params, const char* caller) {
        if (params == nullptr) return;

        if (!ResolveAttachmentQueryName(framebufferObject->IsDefaultFramebuffer(), attachment, caller)) return;

        const Bool depthStencilAlias = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
        FramebufferAttachmentType attachmentType = depthStencilAlias
            ? FramebufferAttachmentType::Depth
            : MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;

        Bool depthStencilMismatch = false;
        const auto* attachmentObject = [&]() -> const MG_State::GLState::FramebufferAttachmentObject* {
            if (!depthStencilAlias) {
                return &framebufferObject->GetAttachment(attachmentType);
            }

            const auto& depthAttachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Depth);
            const auto& stencilAttachment = framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil);
            const Bool depthLive = depthAttachment.IsValid() && !depthAttachment.IsEmpty();
            const Bool stencilLive = stencilAttachment.IsValid() && !stencilAttachment.IsEmpty();
            if (depthLive && stencilLive) {
                const Bool sameObject = depthAttachment.IsTexture() == stencilAttachment.IsTexture() &&
                    (!depthAttachment.IsTexture() || depthAttachment.GetTexture() == stencilAttachment.GetTexture()) &&
                    (!depthAttachment.IsRenderbuffer() ||
                     depthAttachment.GetRenderbuffer() == stencilAttachment.GetRenderbuffer());
                depthStencilMismatch = !sameObject;
            } else {
                // GL_DEPTH_STENCIL_ATTACHMENT means "both halves"; a lone half does not answer it.
                depthStencilMismatch = depthLive != stencilLive;
            }
            if (depthLive) return &depthAttachment;
            if (stencilLive) return &stencilAttachment;
            return nullptr;
        }();

        if (depthStencilMismatch) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "GL_DEPTH_STENCIL_ATTACHMENT query with different depth and stencil "
                                             "attachment images."));
            return;
        }

        if (!ValidateAttachmentQueryPname(attachmentObject, pname, caller)) return;

        switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
            if (attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid()) {
                *params = GL_NONE;
            } else if (attachmentObject->IsTexture()) {
                *params = GL_TEXTURE;
            } else if (attachmentObject->IsRenderbuffer()) {
                *params = GL_RENDERBUFFER;
            } else {
                *params = GL_NONE;
            }
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
            if (attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid()) {
                *params = 0;
            } else if (attachmentObject->IsTexture()) {
                const auto& textureObject = attachmentObject->GetTexture();
                *params = textureObject ? static_cast<GLint>(textureObject->GetExternalIndex()) : 0;
            } else if (attachmentObject->IsRenderbuffer()) {
                const auto& renderbufferObject = attachmentObject->GetRenderbuffer();
                *params = renderbufferObject ? static_cast<GLint>(renderbufferObject->GetExternalIndex()) : 0;
            } else {
                *params = 0;
            }
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid())
                ? static_cast<GLint>(attachmentObject->GetTextureLevel())
                : 0;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
            *params = 0;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid())
                ? static_cast<GLint>(attachmentObject->GetTextureLayer())
                : 0;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_LAYERED:
            *params = (attachmentObject != nullptr && attachmentObject->IsTexture() && attachmentObject->IsValid() &&
                       attachmentObject->IsLayered())
                ? GL_TRUE
                : GL_FALSE;
            break;
        default:
            if (TryAnswerAttachmentFormatQuery(attachmentObject, attachmentType, depthStencilAlias, pname, params,
                                               caller)) {
                return;
            }
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not an accepted value.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void GetNamedFramebufferAttachmentParameteriv_State(GLuint framebuffer, GLenum attachment, GLenum pname,
                                                       GLint* params) {
        // Zero names the default framebuffer here rather than being rejected: the DSA queries take
        // it in place of a binding (GL 4.6 core 9.2.3).
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "GetNamedFramebufferAttachmentParameteriv_State");
        if (!framebufferObject) return;
        GetFramebufferAttachmentParameteriv_Object(framebufferObject, attachment, pname, params,
                                                  "GetNamedFramebufferAttachmentParameteriv_State");
    }

    // The by-target forms resolve the binding; the by-name forms take zero as the default
    // framebuffer, exactly as the other DSA framebuffer entry points do (GL 4.6 core 9.2.3).
    void GetFramebufferParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        if (target == GL_FRAMEBUFFER) target = GL_DRAW_FRAMEBUFFER;
        const auto framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        auto& framebufferObject = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget).GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "GetFramebufferParameteriv_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }
        GetFramebufferParameteriv_Object(framebufferObject, pname, params, "GetFramebufferParameteriv_State");
    }

    void FramebufferParameteri_State(GLenum target, GLenum pname, GLint param) {
        if (target == GL_FRAMEBUFFER) target = GL_DRAW_FRAMEBUFFER;
        const auto framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        auto& framebufferObject = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget).GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferParameteri_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }
        FramebufferParameteri_Object(framebufferObject, pname, param, "FramebufferParameteri_State");
    }

    void GetNamedFramebufferParameteriv_State(GLuint framebuffer, GLenum pname, GLint* params) {
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "GetNamedFramebufferParameteriv_State");
        if (!framebufferObject) return;
        GetFramebufferParameteriv_Object(framebufferObject, pname, params, "GetNamedFramebufferParameteriv_State");
    }

    void NamedFramebufferParameteri_State(GLuint framebuffer, GLenum pname, GLint param) {
        auto framebufferObject = framebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(framebuffer, "NamedFramebufferParameteri_State");
        if (!framebufferObject) return;
        FramebufferParameteri_Object(framebufferObject, pname, param, "NamedFramebufferParameteri_State");
    }

    void BlitNamedFramebuffer_State(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0,
                                   GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                   GLbitfield mask, GLenum filter) {
        auto readObject = readFramebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(readFramebuffer, "BlitNamedFramebuffer_State");
        auto drawObject = drawFramebuffer == 0
            ? FramebufferImpl::pDefaultFramebufferInfo->defaultFBO
            : GetNamedFramebufferObject_State(drawFramebuffer, "BlitNamedFramebuffer_State");
        if (!readObject || !drawObject) return;

        BlitNamedFramebuffer_Backend(readObject, drawObject, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1,
                                     mask, filter);
    }

    void BindRenderbuffer_State(GLenum target, GLuint renderbuffer) {
        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer, true)) return;
        RenderbufferTarget renderbufferTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(renderbufferTarget)) return;

        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(renderbufferTarget);
        if (renderbuffer == 0) {
            bindingSlot.Bind(nullptr);
            return;
        }

        Bool doesRenderbufferCreated = MG_State::pGLContext->ValidateRenderbufferObject(renderbuffer);
        if (!doesRenderbufferCreated) {
            MG_State::pGLContext->CreateRenderbufferObject(renderbuffer);
        }
        auto& renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);

        bindingSlot.Bind(renderbufferObject);
    }

    void BindFramebuffer_State(GLenum target, GLuint framebuffer) {
        if (target == GL_FRAMEBUFFER) {
            BindFramebuffer_State(GL_DRAW_FRAMEBUFFER, framebuffer);
            BindFramebuffer_State(GL_READ_FRAMEBUFFER, framebuffer);
            return;
        }

        if (!FramebufferImpl::ValidateFramebufferName(framebuffer)) return;
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;

        Bool doesFramebufferCreated = MG_State::pGLContext->ValidateFramebufferObject(framebuffer);
        if (!doesFramebufferCreated) {
            MG_State::pGLContext->CreateFramebufferObject(framebuffer);
        }
        auto& framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        bindingSlot.Bind(framebufferObject);
    }

    void GetRenderbufferParameterivForObject_State(const SharedPtr<MG_State::GLState::RenderbufferObject>&
                                                       renderbufferObject,
                                                   GLenum pname, GLint* params, const char* caller) {
        if (!params) return;

        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "No renderbuffer object is available."));
            return;
        }

        switch (pname) {
        case GL_RENDERBUFFER_WIDTH:
            *params = static_cast<GLint>(renderbufferObject->GetWidth());
            break;
        case GL_RENDERBUFFER_HEIGHT:
            *params = static_cast<GLint>(renderbufferObject->GetHeight());
            break;
        case GL_RENDERBUFFER_INTERNAL_FORMAT:
            *params = MG_Util::ConvertTextureInternalFormatToGLEnum(renderbufferObject->GetInternalFormat());
            break;
        case GL_RENDERBUFFER_RED_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetRedSize());
            break;
        case GL_RENDERBUFFER_GREEN_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetGreenSize());
            break;
        case GL_RENDERBUFFER_BLUE_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetBlueSize());
            break;
        case GL_RENDERBUFFER_ALPHA_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetAlphaSize());
            break;
        case GL_RENDERBUFFER_DEPTH_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetDepthSize());
            break;
        case GL_RENDERBUFFER_STENCIL_SIZE:
            *params = static_cast<GLint>(renderbufferObject->GetStencilSize());
            break;
        case GL_RENDERBUFFER_SAMPLES:
            *params = static_cast<GLint>(renderbufferObject->GetSamples());
            break;
        default:
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", caller,
                    std::format("pname {} is not an accepted value.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
    }

    void GetRenderbufferParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        RenderbufferTarget renderbufferTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(renderbufferTarget)) return;
        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(renderbufferTarget);
        auto& renderbufferObject = bindingSlot.GetBoundObject();
        GetRenderbufferParameterivForObject_State(renderbufferObject, pname, params, "GetRenderbufferParameteriv_State");
    }

    void GetNamedRenderbufferParameteriv_State(GLuint renderbuffer, GLenum pname, GLint* params) {
        auto renderbufferObject =
            GetNamedRenderbufferObject_State(renderbuffer, "GetNamedRenderbufferParameteriv_State");
        if (!renderbufferObject) return;
        GetRenderbufferParameterivForObject_State(renderbufferObject, pname, params,
                                                 "GetNamedRenderbufferParameteriv_State");
    }

    void ClearBufferfi_Backend(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        MG_Backend::gBackendFunctionsTable.GL.ClearBufferfi(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv_Backend(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        MG_Backend::gBackendFunctionsTable.GL.ClearBufferfv(buffer, drawbuffer, value);
    }

    void ClearBufferuiv_Backend(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        MG_Backend::gBackendFunctionsTable.GL.ClearBufferuiv(buffer, drawbuffer, value);
    }

    void ClearBufferiv_Backend(GLenum buffer, GLint drawbuffer, const GLint* value) {
        MG_Backend::gBackendFunctionsTable.GL.ClearBufferiv(buffer, drawbuffer, value);
    }

    Bool ValidateClearBufferDrawbuffer_State(GLenum buffer, GLint drawbuffer, const char* caller) {
        if (buffer == GL_COLOR) {
            if (drawbuffer < 0 ||
                drawbuffer >= static_cast<GLint>(MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller, "color drawbuffer index is out of range."));
                return false;
            }
            return true;
        }

        if (drawbuffer != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "depth, stencil, and depth/stencil clears require drawbuffer 0."));
            return false;
        }
        return true;
    }

    Bool ValidateClearBufferfv_State(GLenum buffer, GLint drawbuffer) {
        if (buffer != GL_COLOR && buffer != GL_DEPTH) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateClearBufferfv_State",
                    std::format("buffer {} is not accepted for glClearBufferfv.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
        return ValidateClearBufferDrawbuffer_State(buffer, drawbuffer, "ValidateClearBufferfv_State");
    }

    Bool ValidateClearBufferiv_State(GLenum buffer, GLint drawbuffer) {
        if (buffer != GL_COLOR && buffer != GL_STENCIL) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateClearBufferiv_State",
                    std::format("buffer {} is not accepted for glClearBufferiv.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
        return ValidateClearBufferDrawbuffer_State(buffer, drawbuffer, "ValidateClearBufferiv_State");
    }

    Bool ValidateClearBufferuiv_State(GLenum buffer, GLint drawbuffer) {
        if (buffer != GL_COLOR && buffer != GL_STENCIL) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateClearBufferuiv_State",
                    std::format("buffer {} is not accepted for glClearBufferuiv.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
        return ValidateClearBufferDrawbuffer_State(buffer, drawbuffer, "ValidateClearBufferuiv_State");
    }

    Bool ValidateClearBufferfi_State(GLenum buffer, GLint drawbuffer) {
        if (buffer != GL_DEPTH_STENCIL) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "ValidateClearBufferfi_State",
                    std::format("buffer {} is not accepted for glClearBufferfi.",
                                MG_Util::ConvertGLEnumToString(buffer))));
            return false;
        }
        return ValidateClearBufferDrawbuffer_State(buffer, drawbuffer, "ValidateClearBufferfi_State");
    }

    Bool ReadPixels_State(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        // Check width/height
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                           "Width and height must be non-negative"));
            return false;
        }

        // Validate format
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Invalid format"));
            return false;
        }

        // Validate type
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Invalid pixel data type"));
            return false;
        }

        // Get bound framebuffer
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read);
        auto& framebufferObject = bindingSlot.GetBoundObject();

        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                           "No framebuffer bound to read target"));
            return false;
        }

        // Check framebuffer completeness (including formats the ES pipeline cannot attach)
        if (!framebufferObject->CheckCompleteness() || HasNonRenderableColorAttachment(*framebufferObject)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidFramebufferOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Framebuffer is incomplete"));
            return false;
        }

        // Check for required buffers
        if (textureInputFormat == TextureInputFormat::StencilIndex) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No stencil buffer for stencil index format"));
                return false;
            }
        } else if (textureInputFormat == TextureInputFormat::DepthComponent) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Depth).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No depth buffer for depth component format"));
                return false;
            }
        } else if (textureInputFormat == TextureInputFormat::DepthStencil) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Depth).IsValid() ||
                !framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No depth/stencil buffer for depth-stencil format"));
                return false;
            }

            // Validate type for depth/stencil
            if (texturePixelDataType != TexturePixelDataType::UnsignedInt248 &&
                texturePixelDataType != TexturePixelDataType::Float32UnsignedInt248Rev) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                         "Invalid type for depth-stencil format"));
                return false;
            }
        } else {
            const FramebufferAttachmentType readBuffer = framebufferObject->GetReadBuffer();
            if (readBuffer == FramebufferAttachmentType::None ||
                !framebufferObject->GetAttachment(readBuffer).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No color buffer for color format"));
                return false;
            }

            // GL 3.3 section 4.3.1: GL_INVALID_OPERATION if format is an integer format and the read
            // buffer is not an integer format, or vice versa (GL CTS packed_pixels expects the error
            // for every *_INTEGER readback from a normalized attachment).
            const auto& readAttachment = framebufferObject->GetAttachment(readBuffer);
            TextureInternalFormat attachmentFormat = TextureInternalFormat::Unknown;
            if (readAttachment.IsTexture() && readAttachment.GetTexture()) {
                attachmentFormat = readAttachment.GetTexture()->GetFormat();
            } else if (readAttachment.IsRenderbuffer() && readAttachment.GetRenderbuffer()) {
                attachmentFormat = readAttachment.GetRenderbuffer()->GetInternalFormat();
            }
            if (attachmentFormat != TextureInternalFormat::Unknown &&
                TextureImpl::IsIntegerColorInputFormat(textureInputFormat) !=
                    TextureImpl::IsIntegerColorInternalFormat(attachmentFormat)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "Integer-ness of format does not match the read buffer"));
                return false;
            }
        }

        // Packed-type/format pairing (GL CTS packed_pixels: e.g. GL_RED with GL_UNSIGNED_SHORT_5_6_5 must
        // raise an error instead of reaching the backend). Shared with the TexImage/GetTexImage validators;
        // runs after the depth-stencil branch above so DEPTH_STENCIL with a wrong type keeps GL_INVALID_ENUM.
        if (!TextureImpl::ValidateClientFormatTypePairing(textureInputFormat, texturePixelDataType)) {
            return false;
        }

        // Packed-type/format pairing (GL CTS packed_pixels: e.g. GL_RED with GL_UNSIGNED_SHORT_5_6_5 must
        // raise an error instead of reaching the backend). Shared with the TexImage/GetTexImage validators;
        // runs after the depth-stencil branch above so DEPTH_STENCIL with a wrong type keeps GL_INVALID_ENUM.
        if (!TextureImpl::ValidateClientFormatTypePairing(textureInputFormat, texturePixelDataType)) {
            return false;
        }

        // Check PBO state
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        if (pixelPackBufferObject) {
            // Persistent mappings remain legal GPU transfer destinations.
            if (pixelPackBufferObject->IsMapped() &&
                !(pixelPackBufferObject->GetMappingAccess() & BufferMappingAccessBit::Persistent)) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                              "Pixel pack buffer is currently mapped"));
                return false;
            }

            // Check alignment
            const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(texturePixelDataType);
            if (reinterpret_cast<uintptr_t>(pixels) % typeSize != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "Pixel data not aligned for pixel pack buffer"));
                return false;
            }
        }

        // Check multisampling
        const FramebufferAttachmentType readBuffer = framebufferObject->GetReadBuffer();
        if (readBuffer != FramebufferAttachmentType::None && framebufferObject->GetAttachment(readBuffer).IsRenderbuffer()) {
            auto& rbo = framebufferObject->GetAttachment(readBuffer).GetRenderbuffer();
            if (rbo && rbo->GetSamples() > 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "ReadPixels not supported for multisampled framebuffers"));
                return false;
            }
        }

        return true;
    }

    void ReadPixels_Backend(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MG_Backend::gBackendFunctionsTable.GL.ReadPixels(x, y, width, height, format, type, pixels);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        if (!ReadPixels_State(x, y, width, height, format, type, pixels)) return;
        ReadPixels_Backend(x, y, width, height, format, type, pixels);
    }

    // Bytes glReadPixels would write for this rectangle under the current GL_PACK_* state
    // (GL 4.6 core 18.2.8): rows are padded to GL_PACK_ALIGNMENT and laid out GL_PACK_ROW_LENGTH
    // wide, and the skip parameters offset the first texel. The last row is not padded - nothing
    // follows it to align - which is what makes a tightly-sized destination legal.
    static SizeT ComputePackedReadSizeInBytes(GLsizei width, GLsizei height, GLenum format, GLenum type) {
        const SizeT bytesPerPixel =
            MG_Util::GetInputBytesPerPixel(MG_Util::ConvertGLEnumToTextureInputFormat(format),
                                           MG_Util::ConvertGLEnumToTexturePixelDataType(type));
        if (bytesPerPixel == 0 || width <= 0 || height <= 0) return 0;

        const auto packParam = [](PixelStoreParam param) {
            return static_cast<SizeT>(std::max(0, MG_State::pGLContext->GetPixelStoreParam(param)));
        };
        const SizeT rowLengthInPixels =
            packParam(PixelStoreParam::PackRowLength) != 0
                ? packParam(PixelStoreParam::PackRowLength)
                : static_cast<SizeT>(width);
        const SizeT alignment = std::max<SizeT>(1, packParam(PixelStoreParam::PackAlignment));

        const SizeT unalignedRowBytes = rowLengthInPixels * bytesPerPixel;
        const SizeT paddedRowBytes = ((unalignedRowBytes + alignment - 1) / alignment) * alignment;
        const SizeT skipBytes = packParam(PixelStoreParam::PackSkipRows) * paddedRowBytes +
                                packParam(PixelStoreParam::PackSkipPixels) * bytesPerPixel;

        return skipBytes + paddedRowBytes * (static_cast<SizeT>(height) - 1) +
               static_cast<SizeT>(width) * bytesPerPixel;
    }

    // glReadnPixels is glReadPixels with a bound on how much it may write (GL 4.6 core 18.2.8,
    // originally GL_ARB_robustness). It is identical in every other respect, so it validates and
    // reads through exactly the same path once the destination is known to be big enough.
    void ReadnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei bufSize,
                     void* data) {
        if (bufSize < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "bufSize must be non-negative."));
            return;
        }
        if (!ReadPixels_State(x, y, width, height, format, type, data)) return;
        if (ComputePackedReadSizeInBytes(width, height, format, type) > static_cast<SizeT>(bufSize)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                             "the data required for this read does not fit in bufSize."));
            return;
        }
        ReadPixels_Backend(x, y, width, height, format, type, data);
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        if (!ValidateClearBufferfi_State(buffer, drawbuffer)) return;
        ClearBufferfi_Backend(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        if (!ValidateClearBufferfv_State(buffer, drawbuffer)) return;
        ClearBufferfv_Backend(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        if (!ValidateClearBufferuiv_State(buffer, drawbuffer)) return;
        ClearBufferuiv_Backend(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        if (!ValidateClearBufferiv_State(buffer, drawbuffer)) return;
        ClearBufferiv_Backend(buffer, drawbuffer, value);
    }

    void InvalidateNamedFramebufferData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments) {
        InvalidateNamedFramebufferData_State(framebuffer, numAttachments, attachments);
    }

    void InvalidateNamedFramebufferSubData(GLuint framebuffer, GLsizei numAttachments, const GLenum* attachments,
                                           GLint x, GLint y, GLsizei width, GLsizei height) {
        InvalidateNamedFramebufferSubData_State(framebuffer, numAttachments, attachments, x, y, width, height);
    }

    void InvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments) {
        InvalidateFramebuffer_State(target, numAttachments, attachments);
    }

    void InvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments, GLint x, GLint y,
                                  GLsizei width, GLsizei height) {
        InvalidateSubFramebuffer_State(target, numAttachments, attachments, x, y, width, height);
    }

    void ClearNamedFramebufferiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint* value) {
        ClearNamedFramebufferiv_State(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferuiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint* value) {
        ClearNamedFramebufferuiv_State(framebuffer, buffer, drawbuffer, value);
    }

    void SampleMaski(GLuint maskNumber, GLbitfield mask) {
        SampleMaski_State(maskNumber, mask);
    }

    void RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                        GLsizei height) {
        RenderbufferStorageMultisample_State(target, samples, internalformat, width, height);
    }

    void RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
        RenderbufferStorage_State(target, internalformat, width, height);
    }

    GLboolean IsRenderbuffer(GLuint renderbuffer) {
        return IsRenderbuffer_State(renderbuffer);
    }

    GLboolean IsFramebuffer(GLuint framebuffer) {
        return IsFramebuffer_State(framebuffer);
    }

    void GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
        GetFramebufferAttachmentParameteriv_State(target, attachment, pname, params);
    }

    void GenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
        GenRenderbuffers_State(n, renderbuffers);
    }

    void CreateRenderbuffers(GLsizei n, GLuint* renderbuffers) {
        CreateRenderbuffers_State(n, renderbuffers);
    }

    void NamedRenderbufferStorage(GLuint renderbuffer, GLenum internalformat, GLsizei width, GLsizei height) {
        NamedRenderbufferStorage_State(renderbuffer, internalformat, width, height);
    }

    void NamedRenderbufferStorageMultisample(GLuint renderbuffer, GLsizei samples, GLenum internalformat,
                                             GLsizei width, GLsizei height) {
        NamedRenderbufferStorageMultisample_State(renderbuffer, samples, internalformat, width, height);
    }

    void GetNamedRenderbufferParameteriv(GLuint renderbuffer, GLenum pname, GLint* params) {
        GetNamedRenderbufferParameteriv_State(renderbuffer, pname, params);
    }

    void GenFramebuffers(GLsizei n, GLuint* framebuffers) {
        GenFramebuffers_State(n, framebuffers);
    }

    void CreateFramebuffers(GLsizei n, GLuint* framebuffers) {
        CreateFramebuffers_State(n, framebuffers);
    }

    void FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
        FramebufferTextureLayer_State(target, attachment, texture, level, layer);
    }

    void FramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                              GLint zoffset) {
        FramebufferTexture3D_State(target, attachment, textarget, texture, level, zoffset);
    }

    void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        FramebufferTexture2D_State(target, attachment, textarget, texture, level);
    }

    void FramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        FramebufferTexture1D_State(target, attachment, textarget, texture, level);
    }

    void FramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
        FramebufferTexture_State(target, attachment, texture, level);
    }

    void NamedFramebufferTexture(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level) {
        NamedFramebufferTexture_State(framebuffer, attachment, texture, level);
    }

    void NamedFramebufferTexture1D(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                   GLint level) {
        NamedFramebufferTexture1D_State(framebuffer, attachment, textarget, texture, level);
    }

    void NamedFramebufferTexture2D(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                   GLint level) {
        NamedFramebufferTexture2D_State(framebuffer, attachment, textarget, texture, level);
    }

    void NamedFramebufferTexture3D(GLuint framebuffer, GLenum attachment, GLenum textarget, GLuint texture,
                                   GLint level, GLint zoffset) {
        NamedFramebufferTexture3D_State(framebuffer, attachment, textarget, texture, level, zoffset);
    }

    void NamedFramebufferTextureLayer(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level,
                                      GLint layer) {
        NamedFramebufferTextureLayer_State(framebuffer, attachment, texture, level, layer);
    }

    void NamedFramebufferDrawBuffer(GLuint framebuffer, GLenum buf) {
        NamedFramebufferDrawBuffer_State(framebuffer, buf);
    }

    void NamedFramebufferDrawBuffers(GLuint framebuffer, GLsizei n, const GLenum* bufs) {
        NamedFramebufferDrawBuffers_State(framebuffer, n, bufs);
    }

    void NamedFramebufferReadBuffer(GLuint framebuffer, GLenum src) {
        NamedFramebufferReadBuffer_State(framebuffer, src);
    }

    void ClearNamedFramebufferfv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        ClearNamedFramebufferfv_State(framebuffer, buffer, drawbuffer, value);
    }

    void ClearNamedFramebufferfi(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        ClearNamedFramebufferfi_State(framebuffer, buffer, drawbuffer, depth, stencil);
    }

    void FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
        FramebufferRenderbuffer_State(target, attachment, renderbuffertarget, renderbuffer);
    }

    void NamedFramebufferRenderbuffer(GLuint framebuffer, GLenum attachment, GLenum renderbuffertarget,
                                      GLuint renderbuffer) {
        NamedFramebufferRenderbuffer_State(framebuffer, attachment, renderbuffertarget, renderbuffer);
    }

    void DrawBuffer(GLenum buf) {
        DrawBuffer_State(buf);
    }

    void DrawBuffers(GLsizei n, const GLenum* bufs) {
        DrawBuffers_State(n, bufs);
    }

    void ReadBuffer(GLenum src) {
        ReadBuffer_State(src);
    }

    void DeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers) {
        DeleteRenderbuffers_State(n, renderbuffers);
    }

    void DeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
        DeleteFramebuffers_State(n, framebuffers);
    }

    GLenum CheckFramebufferStatus(GLenum target) {
        return CheckFramebufferStatus_State(target);
    }

    GLenum CheckNamedFramebufferStatus(GLuint framebuffer, GLenum target) {
        return CheckNamedFramebufferStatus_State(framebuffer, target);
    }

    void GetFramebufferParameteriv(GLenum target, GLenum pname, GLint* params) {
        GetFramebufferParameteriv_State(target, pname, params);
    }

    void FramebufferParameteri(GLenum target, GLenum pname, GLint param) {
        FramebufferParameteri_State(target, pname, param);
    }

    void GetNamedFramebufferParameteriv(GLuint framebuffer, GLenum pname, GLint* params) {
        GetNamedFramebufferParameteriv_State(framebuffer, pname, params);
    }

    void NamedFramebufferParameteri(GLuint framebuffer, GLenum pname, GLint param) {
        NamedFramebufferParameteri_State(framebuffer, pname, param);
    }

    void GetNamedFramebufferAttachmentParameteriv(GLuint framebuffer, GLenum attachment, GLenum pname, GLint* params) {
        GetNamedFramebufferAttachmentParameteriv_State(framebuffer, attachment, pname, params);
    }

    void BlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1,
                              GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                              GLenum filter) {
        BlitNamedFramebuffer_State(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1,
                                   dstY1, mask, filter);
    }

    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {
        BlitFramebuffer_Backend(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    }

    void BindRenderbuffer(GLenum target, GLuint renderbuffer) {
        BindRenderbuffer_State(target, renderbuffer);
    }

    void BindFramebuffer(GLenum target, GLuint framebuffer) {
        BindFramebuffer_State(target, framebuffer);
    }

    void GetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params) {
        GetRenderbufferParameteriv_State(target, pname, params);
    }

    namespace FramebufferImpl {
        // Leak-at-exit storage; see GlobalObjects.cpp.
        UniquePtr<DefaultFramebufferInfo>& pDefaultFramebufferInfo = *new UniquePtr<DefaultFramebufferInfo>();
    } // namespace FramebufferImpl
} // namespace MobileGL::MG_Impl::GLImpl
