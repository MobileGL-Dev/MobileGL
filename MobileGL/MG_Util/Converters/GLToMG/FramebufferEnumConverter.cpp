// MobileGL - MobileGL/MG_Util/Converters/GLToMG/FramebufferEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FramebufferEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        FramebufferTarget ConvertGLEnumToFramebufferTarget(GLenum framebufferTarget) {
            switch (framebufferTarget) {
            case GL_FRAMEBUFFER:
            case GL_DRAW_FRAMEBUFFER:
                return FramebufferTarget::Draw;
            case GL_READ_FRAMEBUFFER:
                return FramebufferTarget::Read;
            case GL_UNKNOWN_MGL:
            default:
                return FramebufferTarget::Unknown;
            }
        }

        FramebufferAttachmentType ConvertGLEnumToFramebufferAttachmentType(GLenum attachment) {
            if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT31) {
                return static_cast<FramebufferAttachmentType>(static_cast<SizeT>(FramebufferAttachmentType::Color0) +
                                                              (attachment - GL_COLOR_ATTACHMENT0));
            }

            switch (attachment) {
            case GL_NONE:
                return FramebufferAttachmentType::None;
            case GL_DEPTH_ATTACHMENT:
                return FramebufferAttachmentType::Depth;
            case GL_STENCIL_ATTACHMENT:
                return FramebufferAttachmentType::Stencil;
            case GL_FRONT_LEFT:
                return FramebufferAttachmentType::FrontLeft;
            case GL_FRONT_RIGHT:
                return FramebufferAttachmentType::FrontRight;
            case GL_BACK_LEFT:
                return FramebufferAttachmentType::BackLeft;
            case GL_BACK_RIGHT:
                return FramebufferAttachmentType::BackRight;
            default:
                return FramebufferAttachmentType::Unknown;
            }
        }

        RenderbufferTarget ConvertGLEnumToRenderbufferTarget(GLenum renderbufferTarget) {
            switch (renderbufferTarget) {
            case GL_RENDERBUFFER:
                return RenderbufferTarget::Renderbuffer;
            case GL_UNKNOWN_MGL:
            default:
                return RenderbufferTarget::Unknown;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL