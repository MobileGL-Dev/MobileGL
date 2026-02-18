// MobileGL - MobileGL/MG_Util/Converters/MGToGL/FramebufferEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FramebufferEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        GLenum ConvertFramebufferTargetToGLEnum(FramebufferTarget target) {
            switch (target) {
            case FramebufferTarget::Draw:
                return GL_DRAW_FRAMEBUFFER;
            case FramebufferTarget::Read:
                return GL_READ_FRAMEBUFFER;
            default:
                return GL_DRAW_FRAMEBUFFER;
            }
        }

        GLenum ConvertFramebufferAttachmentTypeToGLEnum(FramebufferAttachmentType type) {
            if (static_cast<Int>(type) >= static_cast<Int>(FramebufferAttachmentType::Color0) &&
                static_cast<Int>(type) <= static_cast<Int>(FramebufferAttachmentType::Color31)) {
                return GL_COLOR_ATTACHMENT0 +
                       (static_cast<GLenum>(type) - static_cast<GLenum>(FramebufferAttachmentType::Color0));
            }

            switch (type) {
            case FramebufferAttachmentType::Depth:
                return GL_DEPTH_ATTACHMENT;
            case FramebufferAttachmentType::Stencil:
                return GL_STENCIL_ATTACHMENT;
            case FramebufferAttachmentType::FrontLeft:
                return GL_FRONT_LEFT;
            case FramebufferAttachmentType::FrontRight:
                return GL_FRONT_RIGHT;
            case FramebufferAttachmentType::BackLeft:
                return GL_BACK_LEFT;
            case FramebufferAttachmentType::BackRight:
                return GL_BACK_RIGHT;
            default:
                return GL_NONE;
            }
        }

        GLenum ConvertRenderbufferTargetToGLEnum(RenderbufferTarget target) {
            switch (target) {
            case RenderbufferTarget::Renderbuffer:
                return GL_RENDERBUFFER;
            default:
                return GL_RENDERBUFFER;
            }
        }

    } // namespace MG_Util
} // namespace MobileGL