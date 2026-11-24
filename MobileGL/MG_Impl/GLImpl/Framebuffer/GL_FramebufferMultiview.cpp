// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/GL_FramebufferMultiview.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL_OVR_multiview: the two attaching entry points, the attachment queries and the draw-time rules.
// A multiview attachment is a FramebufferAttachmentObject of a 2D array texture whose layer is the
// base view index and whose view count is non-zero; the rest of the framebuffer code reads it
// through FramebufferObject::GetViewCount() and HasViewTargetMismatch().
#include "GL_Framebuffer.h"
#include "Validators.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Impl/GLImpl/Texture/Validators.h>
#include <MG_Impl/Pipe/FramebufferEmit.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_State/GLState/ProgramState/ProgramObject.h>
#include <MG_Util/Converters/GLToMG/FramebufferEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl {
    namespace {
        void RecordMultiviewError(ErrorCode code, const char* functionName, String detail) {
            MG_State::pGLContext->RecordError(
                code, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", functionName, Move(detail)));
        }

        const MG_Backend::DynamicBackendParameters& ActiveBackendLimits() {
            static const MG_Backend::DynamicBackendParameters kNoBackend{};
            return MG_Backend::pActiveBackendObject ? MG_Backend::pActiveBackendObject->GetDynamicParameters()
                                                    : kNoBackend;
        }

        // The checks of the spec that do not depend on the framebuffer object, in its order:
        // FramebufferTextureLayer's own errors other than the layer ones, then numViews against
        // MAX_VIEWS_OVR, then the texture-dependent ones. Returns the texture to attach, or null
        // with `ok` telling a detach (texture 0) apart from a refusal.
        SharedPtr<MG_State::GLState::ITextureObject> ValidateMultiviewTexture(const char* functionName, GLuint texture,
                                                                              GLint level, GLint baseViewIndex,
                                                                              GLsizei numViews, Bool& ok) {
            ok = false;
            const Int maxViews = GetMaxViews_State();
            if (numViews < 1 || numViews > maxViews) {
                RecordMultiviewError(
                    ErrorCode::InvalidValue, functionName,
                    std::format("numViews {} is outside [1, GL_MAX_VIEWS_OVR = {}].", numViews, maxViews));
                return nullptr;
            }
            if (texture == 0) {
                ok = true;
                return nullptr;
            }
            const auto& textureObject = MG_State::pGLContext->GetTextureObject(texture);
            if (!textureObject) {
                RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                     std::format("Texture object {} is not valid.", texture));
                return nullptr;
            }
            if (textureObject->GetTarget() != TextureTarget::Texture2DArray) {
                RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                     "A multiview attachment must be a two-dimensional array texture.");
                return nullptr;
            }
            if (!FramebufferImpl::ValidateFramebufferTextureLevel(functionName, textureObject, level)) return nullptr;
            if (baseViewIndex < 0) {
                RecordMultiviewError(ErrorCode::InvalidValue, functionName, "baseViewIndex must be non-negative.");
                return nullptr;
            }
            const Int maxLayers = ActiveBackendLimits().MaxArrayTextureLayers;
            if (static_cast<Int64>(baseViewIndex) + numViews > maxLayers) {
                RecordMultiviewError(ErrorCode::InvalidValue, functionName,
                                     std::format("baseViewIndex {} + numViews {} exceeds GL_MAX_ARRAY_TEXTURE_LAYERS "
                                                 "({}).",
                                                 baseViewIndex, numViews, maxLayers));
                return nullptr;
            }
            ok = true;
            return textureObject;
        }

        // Shared tail of both entry points once the framebuffer object is known. Returns whether
        // the framebuffer changed.
        Bool AttachMultiview(const char* functionName,
                             const SharedPtr<MG_State::GLState::FramebufferObject>& framebufferObject,
                             GLenum attachment, GLuint texture, GLint level, GLint baseViewIndex, GLsizei numViews) {
            if (!IsMultiviewSupported_State()) {
                RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                     "GL_OVR_multiview is not supported by the active backend.");
                return false;
            }
            const Bool depthStencil = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
            const FramebufferAttachmentType attachmentType =
                depthStencil ? FramebufferAttachmentType::Depth
                             : MG_Util::ConvertGLEnumToFramebufferAttachmentType(attachment);
            if (!FramebufferImpl::ValidateFramebufferAttachmentType(attachmentType)) return false;
            if (!FramebufferImpl::ValidateFramebufferTextureAttachmentPoint(functionName, framebufferObject,
                                                                            attachmentType)) {
                return false;
            }
            Bool ok = false;
            const auto textureObject =
                ValidateMultiviewTexture(functionName, texture, level, baseViewIndex, numViews, ok);
            if (!ok) return false;

            const FramebufferAttachmentType stencil = FramebufferAttachmentType::Stencil;
            if (!textureObject) {
                framebufferObject->Detach(attachmentType);
                if (depthStencil) framebufferObject->Detach(stencil);
                return true;
            }
            const auto attach = [&](FramebufferAttachmentType type) {
                framebufferObject->AttachTexture(type, textureObject, TextureUploadTarget::Texture2DArray, level,
                                                 baseViewIndex, /*layered=*/false, numViews);
            };
            attach(attachmentType);
            if (depthStencil) attach(stencil);
            return true;
        }
    } // namespace

    Int GetMaxViews_State() {
        return std::max<Int>(ActiveBackendLimits().MaxViews, 0);
    }

    Bool IsMultiviewSupported_State() {
        return MG_Backend::pActiveBackendObject != nullptr && GetMaxViews_State() > 0;
    }

    void FramebufferTextureMultiviewOVR_State(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                              GLint baseViewIndex, GLsizei numViews) {
        if (target == GL_FRAMEBUFFER) target = GL_DRAW_FRAMEBUFFER;
        const FramebufferTarget framebufferTarget = MG_Util::ConvertGLEnumToFramebufferTarget(target);
        if (!FramebufferImpl::ValidateFramebufferTarget(framebufferTarget)) return;
        if (!TextureImpl::ValidateTextureName(texture, true)) return;
        const auto& framebufferObject =
            MG_State::pGLContext->GetFramebufferBindingSlot(framebufferTarget).GetBoundObject();
        AttachMultiview(__func__, framebufferObject, attachment, texture, level, baseViewIndex, numViews);
    }

    void NamedFramebufferTextureMultiviewOVR_State(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level,
                                                   GLint baseViewIndex, GLsizei numViews) {
        const auto framebufferObject = GetNamedFramebufferObject_State(framebuffer, __func__);
        if (!framebufferObject) return;
        if (AttachMultiview(__func__, framebufferObject, attachment, texture, level, baseViewIndex, numViews)) {
            // A DSA mutator has no validate point, so the record goes out now (see
            // PipePublishFramebufferByName in GL_Framebuffer.cpp).
            MG_Pipe::MGPipeFramebufferEmitterInstance().EmitFramebufferByName(*framebufferObject);
        }
    }

    Bool ValidateMultiviewDraw(const MG_State::GLState::ProgramObject& program, const char* functionName) {
        // Without backend multiview no framebuffer can be multiview, so a single-view program
        // needs no framebuffer walk on every draw.
        if (program.GetMultiviewViewCount() == 1 && !IsMultiviewSupported_State()) return true;
        const auto& drawFramebuffer =
            MG_State::pGLContext->GetFramebufferBindingSlot(FramebufferTarget::Draw).GetBoundObject();
        const Int framebufferViews = drawFramebuffer ? drawFramebuffer->GetViewCount() : 1;
        const Int programViews = program.GetMultiviewViewCount();
        // GL_OVR_multiview 11.1.3.11 / 16.X: the program's num_views (1 without the qualifier)
        // must be the draw framebuffer's view count.
        if (framebufferViews != programViews) {
            RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                 std::format("The program renders {} view(s) but the draw framebuffer has {}.",
                                             programViews, framebufferViews));
            return false;
        }
        if (!drawFramebuffer || !drawFramebuffer->IsMultiview()) return true;
        // A multiview draw framebuffer takes no geometry or tessellation stage and no active,
        // unpaused transform feedback.
        if (program.HasLinkedShaderStage(ShaderStage::Geometry) ||
            program.HasLinkedShaderStage(ShaderStage::TessControl) ||
            program.HasLinkedShaderStage(ShaderStage::TessEval)) {
            RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                 "A multiview draw cannot run a geometry or tessellation stage.");
            return false;
        }
        if (MG_State::pGLContext->IsTransformFeedbackActive() && !MG_State::pGLContext->IsTransformFeedbackPaused()) {
            RecordMultiviewError(ErrorCode::InvalidOperation, functionName,
                                 "A multiview draw cannot capture transform feedback.");
            return false;
        }
        return true;
    }

    namespace FramebufferImpl {
        Bool TryAnswerMultiviewAttachmentQuery(const MG_State::GLState::FramebufferAttachmentObject* attachmentObject,
                                               GLenum pname, GLint* params, const char* caller) {
            if (pname != GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR &&
                pname != GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR) {
                return false;
            }
            if (!IsMultiviewSupported_State()) return false;
            const Bool nothingAttached =
                attachmentObject == nullptr || attachmentObject->IsEmpty() || !attachmentObject->IsValid();
            if (!nothingAttached && !attachmentObject->IsTexture()) {
                RecordMultiviewError(ErrorCode::InvalidEnum, caller,
                                     std::format("pname {:#x} is only defined for a texture attachment.", pname));
                return true;
            }
            // Zero for a texture attachment that is not multiview, as the spec answers for one
            // that is not a two-dimensional array texture; FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER
            // already reads the base view index.
            const Bool multiview = !nothingAttached && attachmentObject->IsMultiview();
            if (pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR) {
                *params = multiview ? static_cast<GLint>(attachmentObject->GetNumViews()) : 0;
            } else {
                *params = multiview ? static_cast<GLint>(attachmentObject->GetTextureLayer()) : 0;
            }
            return true;
        }
    } // namespace FramebufferImpl

    void FramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                        GLint baseViewIndex, GLsizei numViews) {
        FramebufferTextureMultiviewOVR_State(target, attachment, texture, level, baseViewIndex, numViews);
    }

    void NamedFramebufferTextureMultiviewOVR(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level,
                                             GLint baseViewIndex, GLsizei numViews) {
        NamedFramebufferTextureMultiviewOVR_State(framebuffer, attachment, texture, level, baseViewIndex, numViews);
    }
} // namespace MobileGL::MG_Impl::GLImpl
