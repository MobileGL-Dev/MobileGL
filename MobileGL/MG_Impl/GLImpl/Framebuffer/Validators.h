// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/Validators.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/FramebufferState/FramebufferObject.h>

namespace MobileGL::MG_Impl::GLImpl::FramebufferImpl {
    Bool ValidateFramebufferTarget(FramebufferTarget target);
    Bool ValidateFramebufferName(Uint index, Bool allowZero = true);
    Bool ValidateFramebufferAttachmentType(FramebufferAttachmentType attachment);
    // GL_COLOR_ATTACHMENTn is a token per n up to 31, but only the first GL_MAX_COLOR_ATTACHMENTS of
    // them name an attachment point of a framebuffer object; the rest are INVALID_OPERATION for the
    // attaching entry points (GL 4.6 core 9.2.7). Non-colour attachments pass through unchanged.
    Bool ValidateColorAttachmentInRange(FramebufferAttachmentType attachment, const char* caller);
    Bool ValidateRenderbufferTarget(RenderbufferTarget target);
    Bool ValidateRenderbufferName(Uint index, Bool allowZero = true);
    // The read-framebuffer preconditions the CopyTexSubImage family shares (GL 4.6 core 8.6): the
    // read framebuffer must be complete, its read buffer must name a real attachment, and it must
    // not be multisampled. Incompleteness is INVALID_FRAMEBUFFER_OPERATION, the other two are
    // INVALID_OPERATION.
    Bool ValidateReadFramebufferForCopy(const char* caller);
    // The pname sets of glGet/FramebufferParameteri (GL 4.6 core 9.2.3). Order matters and is part
    // of the contract: a name outside the table is INVALID_ENUM, and only then is a name that the
    // DEFAULT framebuffer does not answer INVALID_OPERATION. Testing the framebuffer kind first
    // would turn GL_FRAMEBUFFER_DEFAULT_WIDTH on framebuffer zero into the wrong error.
    Bool ValidateFramebufferParameterPname(GLenum pname, Bool isDefaultFramebuffer, Bool forSetter,
                                           const char* caller);
} // namespace MobileGL::MG_Impl::GLImpl::FramebufferImpl
