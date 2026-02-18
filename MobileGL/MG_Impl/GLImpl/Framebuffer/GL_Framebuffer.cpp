// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GL_Framebuffer.h"
#include "Validators.h"
#include "Config.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Metrics/TextureMetrics.h>
#include <MG_Impl/GLImpl/Texture/Validators.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    void BlitFramebuffer_Backend(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                 GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
        MG_Backend::gBackendFunctionsTable.GL.BlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1,
                                                              mask, filter);
    }

    void SampleMaski_State(GLuint maskNumber, GLbitfield mask) {
        // TODO: implement
    }

    void RenderbufferStorageMultisample_State(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                              GLsizei height) {
        // TODO: implement
    }

    void RenderbufferStorage_State(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(rbTarget)) return;
        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(rbTarget);
        auto renderbufferObject = bindingSlot.GetBoundObject();
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "RenderbufferStorage_State",
                                             "Renderbuffer target is bound to no renderbuffer object."));
            return;
        }
        TextureInternalFormat format = MG_Util::ConvertGLEnumToTextureInternalFormat(internalformat);
        if (!TextureImpl::ValidateTextureInternalFormat(format)) return;
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "RenderbufferStorage_State",
                                                                      "Width and height must be non-negative."));
            return;
        }
        renderbufferObject->AllocateStorage({width, height});
        renderbufferObject->SetInternalFormat(format);
    }

    GLboolean IsRenderbuffer_State(GLuint renderbuffer) {
        return MG_State::pGLContext->ValidateRenderbufferName(renderbuffer);
    }

    GLboolean IsFramebuffer_State(GLuint framebuffer) {
        return MG_State::pGLContext->ValidateFramebufferName(framebuffer);
    }

    void GetFramebufferAttachmentParameteriv_State(GLenum target, GLenum attachment, GLenum pname, GLint* params) {
        // TODO: implement
    }

    void GenRenderbuffers_State(GLsizei n, GLuint* renderbuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GenRenderbuffers_State", "n must be non-negative"));
            return;
        }
        auto renderbufferNames = MG_State::pGLContext->GenRenderbufferNames(n);
        Memcpy(renderbuffers, renderbufferNames.data(), sizeof(GLuint) * static_cast<SizeT>(n));
    }

    void GenFramebuffers_State(GLsizei n, GLuint* framebuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GenFramebuffers_State", "n must be non-negative"));
            return;
        }
        auto framebuffersNames = MG_State::pGLContext->GenFramebufferNames(n);
        Memcpy(framebuffers, framebuffersNames.data(), sizeof(GLuint) * static_cast<SizeT>(n));
    }

    void FramebufferTextureLayer_State(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
        // TODO: implement
    }

    void FramebufferTexture3D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                                    GLint zoffset) {
        // TODO: implement
    }

    void FramebufferTexture2D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }

        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
            FramebufferTexture2D_State(target, GL_DEPTH_ATTACHMENT, textarget, texture, level);
            FramebufferTexture2D_State(target, GL_STENCIL_ATTACHMENT, textarget, texture, level);
        }

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);

        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferTexture2D_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (texture == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto textureObject = MG_State::pGLContext->GetTextureObject(texture);
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferTexture2D_State",
                                             std::format("Texture object {} is not valid.", texture)));
            return;
        }

        framebufferObject->AttachTexture(attachmentType, textureObject, textureObject ? level : 0);
    }

    void FramebufferTexture1D_State(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
        // TODO: implement
    }

    void FramebufferTexture_State(GLenum target, GLenum attachment, GLuint texture, GLint level) {
        // TODO: implement
    }

    void FramebufferRenderbuffer_State(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                       GLuint renderbuffer) {
        if (target == GL_FRAMEBUFFER) {
            target = GL_DRAW_FRAMEBUFFER;
        }

        FramebufferAttachmentType attachmentType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        RenderbufferTarget rbTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(renderbuffertarget);
        if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return;
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer)) return;
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferRenderbuffer_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return;
        }

        if (renderbuffer == 0) {
            framebufferObject->Detach(attachmentType);
            return;
        }

        auto renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "FramebufferRenderbuffer_State",
                                             std::format("Renderbuffer object {} is not valid.", renderbuffer)));
            return;
        }

        framebufferObject->AttachRenderbuffer(attachmentType, renderbufferObject);
    }

    void DrawBuffers_State(GLsizei n, const GLenum* bufs) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`n` is less than 0."));
            return;
        } else if (n > MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__, "`n` is greater than `GL_MAX_DRAW_BUFFERS`."));
            return;
        }

        // Get bound framebuffer
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw);
        auto fbo = bindingSlot.GetBoundObject();
        bool isDefaultFBO = (fbo == FramebufferImpl::pDefaultFramebufferInfo->defaultFBO);

        static int existenceMap[(SizeT)FramebufferAttachmentType::FramebufferAttachmentTypeCount] = {-1};
        std::fill(existenceMap, existenceMap + (SizeT)FramebufferAttachmentType::FramebufferAttachmentTypeCount, -1);

        for (GLsizei i = 0; i < n; ++i) {
            auto attType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(bufs[i]);

            // ------------------- Check validity begin ------------------------
            if (attType == FramebufferAttachmentType::Unknown) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::format("bufs[{}] = {} is not an accepted value.", i,
                                                             MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            if (isDefaultFBO && attType >= FramebufferAttachmentType::Color0 &&
                attType <= FramebufferAttachmentType::Color31) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format(
                            "FBO is default FBO, but bufs[{}] = {} is one of the `GL_COLOR_ATTACHMENTn` tokens.", i,
                            MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            if (!isDefaultFBO && attType >= FramebufferAttachmentType::FrontLeft &&
                attType <= FramebufferAttachmentType::BackRight) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum,
                    MakeShared<GenericErrorInfo>(
                        "MG_Impl/GLImpl", __func__,
                        std::format("FBO is not default FBO, but bufs[{}] = {} is anything other than `GL_NONE` or "
                                    "one of the `GL_COLOR_ATTACHMENTn` tokens.",
                                    i, MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            if (attType != FramebufferAttachmentType::None && existenceMap[(SizeT)attType] >= 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::format("a symbolic constant other than `GL_NONE` appears "
                                                             "more than once in bufs. bufs[{}] == bufs[{}] == {}.",
                                                             i, existenceMap[(SizeT)attType],
                                                             MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }

            existenceMap[(SizeT)attType] = i;

            if ((SizeT)attType >
                (SizeT)FramebufferAttachmentType::Color0 + MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", __func__,
                                                 std::format("bufs[{}] == {} indicates a color buffer that does "
                                                             "not exist in the current GL context.",
                                                             i, MG_Util::ConvertGLEnumToString(bufs[i]))));
                return;
            }
            // ------------------------- Check validity end ----------------------------------
            fbo->SetDrawBuffer(i, attType);
        }
        for (GLsizei i = n; i < MG_State::GLState::FramebufferObject::MAX_DRAW_BUFFERS; ++i) {
            fbo->SetDrawBuffer(i, FramebufferAttachmentType::None);
        }
    }

    void DrawBuffer_State(GLenum buf) {
        if (buf == GL_NONE) {
            DrawBuffers_State(0, nullptr);
        } else {
            static GLenum bufs[] = {buf};
            DrawBuffers_State(1, bufs);
        }
    }

    void ReadBuffer_State(GLenum mode) {
        auto attType = MG_Util::ConvertGLEnumToFramebufferAttachmentType(mode);

        // ------------------- Check validity begin ------------------------
        if (attType == FramebufferAttachmentType::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>(
                    "MG_Impl/GLImpl", __func__,
                    std::format("`mode` = {} is not an accepted value.", MG_Util::ConvertGLEnumToString(mode))));
            return;
        }

        // Get bound framebuffer
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read);
        auto fbo = bindingSlot.GetBoundObject();
        fbo->SetReadBuffer(attType);
    }

    void DeleteRenderbuffers_State(GLsizei n, const GLuint* renderbuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteRenderbuffers_State", "n must be non-negative."));
            return;
        }

        if (!renderbuffers) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteRenderbuffers_State",
                                                                      "Renderbuffer names array cannot be null."));
            return;
        }

        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint bufferName = renderbuffers[i];
            if (bufferName == 0) continue;
            if (!FramebufferImpl::ValidateRenderbufferName(bufferName)) continue;
            MG_State::pGLContext->MarkRenderbufferObjectForDeletion(bufferName);
        }
    }

    void DeleteFramebuffers_State(GLsizei n, const GLuint* framebuffers) {
        if (n < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteFramebuffers_State", "n must be non-negative."));
            return;
        }

        if (!framebuffers) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "DeleteFramebuffers_State",
                                                                           "Framebuffer names array cannot be null."));
            return;
        }

        for (SizeT i = 0; i < static_cast<SizeT>(n); ++i) {
            Uint bufferName = framebuffers[i];
            if (bufferName == 0) continue;
            if (!FramebufferImpl::ValidateFramebufferName(bufferName)) continue;
            MG_State::pGLContext->MarkFramebufferObjectForDeletion(bufferName);
        }
    }

    GLenum CheckFramebufferStatus_State(GLenum target) {
        FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return GL_FRAMEBUFFER_UNDEFINED;

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        auto framebufferObject = bindingSlot.GetBoundObject();
        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "CheckFramebufferStatus_State",
                                             "Framebuffer target is bound to no framebuffer object."));
            return GL_FRAMEBUFFER_UNDEFINED;
        }

        // TODO: distinguish GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT and GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
        // TODO: GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER, GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER,
        //       GL_FRAMEBUFFER_UNSUPPORTED, GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE,
        //       GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS
        return framebufferObject->CheckCompleteness() ? GL_FRAMEBUFFER_COMPLETE
                                                      : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    }

    void BindRenderbuffer_State(GLenum target, GLuint renderbuffer) {
        if (!FramebufferImpl::ValidateRenderbufferName(renderbuffer)) return;
        RenderbufferTarget renderbufferTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(renderbufferTarget)) return;

        auto renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);
        if (!renderbufferObject) {
            MG_State::pGLContext->CreateRenderbufferObject(renderbuffer);
            renderbufferObject = MG_State::pGLContext->GetRenderbufferObject(renderbuffer);
        }

        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(renderbufferTarget);
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

        auto framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
        if (!framebufferObject) {
            MG_State::pGLContext->CreateFramebufferObject(framebuffer);
            framebufferObject = MG_State::pGLContext->GetFramebufferObject(framebuffer);
        }

        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget);
        bindingSlot.Bind(framebufferObject);
    }

    void GetRenderbufferParameteriv_State(GLenum target, GLenum pname, GLint* params) {
        if (!params) return;

        RenderbufferTarget renderbufferTarget = MG_Util::ConvertGLEnumToRenderbufferTarget(target);
        if (!FramebufferImpl::ValidateRenderbufferTarget(renderbufferTarget)) return;
        auto& bindingSlot = MG_State::pGLContext->GetRenderbufferBindingSlot(renderbufferTarget);
        auto renderbufferObject = bindingSlot.GetBoundObject();
        if (!renderbufferObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "GetRenderbufferParameteriv_State",
                                             "Renderbuffer target is bound to no renderbuffer object."));
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
                MakeShared<GenericErrorInfo>(
                    "MG_Impl/GLImpl", "GetRenderbufferParameteriv_State",
                    std::format("pname {} is not an accepted value.", MG_Util::ConvertGLEnumToString(pname))));
            return;
        }
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

    void ReadPixels_State(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        TextureInputFormat textureInputFormat = MG_Util::ConvertGLEnumToTextureInputFormat(format);
        TexturePixelDataType texturePixelDataType = MG_Util::ConvertGLEnumToTexturePixelDataType(type);

        // Check width/height
        if (width < 0 || height < 0) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidValue,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                           "Width and height must be non-negative"));
            return;
        }

        // Validate format
        if (!TextureImpl::ValidateTextureInputFormat(textureInputFormat)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Invalid format"));
            return;
        }

        // Validate type
        if (!TextureImpl::ValidateTexturePixelDataType(texturePixelDataType)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Invalid pixel data type"));
            return;
        }

        // Get bound framebuffer
        auto& bindingSlot = MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Read);
        auto framebufferObject = bindingSlot.GetBoundObject();

        if (!framebufferObject) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                                              MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                           "No framebuffer bound to read target"));
            return;
        }

        // Check framebuffer completeness
        if (!framebufferObject->CheckCompleteness()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidFramebufferOperation,
                MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State", "Framebuffer is incomplete"));
            return;
        }

        // Check for required buffers
        if (textureInputFormat == TextureInputFormat::StencilIndex) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No stencil buffer for stencil index format"));
                return;
            }
        } else if (textureInputFormat == TextureInputFormat::DepthComponent) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Depth).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No depth buffer for depth component format"));
                return;
            }
        } else if (textureInputFormat == TextureInputFormat::DepthStencil) {
            if (!framebufferObject->GetAttachment(FramebufferAttachmentType::Depth).IsValid() ||
                !framebufferObject->GetAttachment(FramebufferAttachmentType::Stencil).IsValid()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "No depth/stencil buffer for depth-stencil format"));
                return;
            }

            // Validate type for depth/stencil
            if (texturePixelDataType != TexturePixelDataType::UnsignedInt248 &&
                texturePixelDataType != TexturePixelDataType::Float32UnsignedInt248Rev) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidEnum, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                         "Invalid type for depth-stencil format"));
                return;
            }
        }

        // Check PBO state
        const auto& pixelPackBufferObject =
            MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();

        if (pixelPackBufferObject) {
            // Check if PBO is mapped
            if (pixelPackBufferObject->IsMapped()) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation, MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                                              "Pixel pack buffer is currently mapped"));
                return;
            }

            // Check alignment
            const SizeT typeSize = MG_Util::GetTexturePixelDataTypeSize(texturePixelDataType);
            if (reinterpret_cast<uintptr_t>(pixels) % typeSize != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "Pixel data not aligned for pixel pack buffer"));
                return;
            }
        }

        // Check multisampling
        if (framebufferObject->GetAttachment(FramebufferAttachmentType::Color0).IsRenderbuffer()) {
            auto rbo = framebufferObject->GetAttachment(FramebufferAttachmentType::Color0).GetRenderbuffer();
            if (rbo && rbo->GetSamples() > 1) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeShared<GenericErrorInfo>("MG_Impl/GLImpl", "ReadPixels_State",
                                                 "ReadPixels not supported for multisampled framebuffers"));
                return;
            }
        }
    }

    void ReadPixels_Backend(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        MG_Backend::gBackendFunctionsTable.GL.ReadPixels(x, y, width, height, format, type, pixels);
    }

    /* @INSERTION_POINT:FUNCTION_IMPLEMENTATION@ */
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
        ReadPixels_State(x, y, width, height, format, type, pixels);
        ReadPixels_Backend(x, y, width, height, format, type, pixels);
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
        ClearBufferfi_Backend(buffer, drawbuffer, depth, stencil);
    }

    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
        ClearBufferfv_Backend(buffer, drawbuffer, value);
    }

    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {
        ClearBufferuiv_Backend(buffer, drawbuffer, value);
    }

    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {
        ClearBufferiv_Backend(buffer, drawbuffer, value);
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

    void GenFramebuffers(GLsizei n, GLuint* framebuffers) {
        GenFramebuffers_State(n, framebuffers);
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

    void FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
        FramebufferRenderbuffer_State(target, attachment, renderbuffertarget, renderbuffer);
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
        DefaultFramebufferInfo* pDefaultFramebufferInfo;
    } // namespace FramebufferImpl
} // namespace MobileGL::MG_Impl::GLImpl
