// MobileGL - MobileGL/MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include "MG_State/GLState/FramebufferState/FramebufferObject.h"
#include <Includes.h>
#include <MG_State/GLState/Core.h>

namespace MobileGL {
    namespace MG_Impl::GLImpl {
        /* @INSERTION_POINT:FUNCTION_DECLARATION@ */
        void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
        void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
        void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value);
        void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value);
        void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value);
        void RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                            GLsizei height);
        void RenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
        GLboolean IsRenderbuffer(GLuint renderbuffer);
        void GetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params);
        void GenRenderbuffers(GLsizei n, GLuint* renderbuffers);
        void FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
        void DeleteRenderbuffers(GLsizei n, const GLuint* renderbuffers);
        void BindRenderbuffer(GLenum target, GLuint renderbuffer);
        void SampleMaski(GLuint maskNumber, GLbitfield mask);
        GLboolean IsFramebuffer(GLuint framebuffer);
        void GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params);
        void GenFramebuffers(GLsizei n, GLuint* framebuffers);
        void FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
        void FramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level,
                                  GLint zoffset);
        void FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
        void FramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
        void FramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
        void DrawBuffer(GLenum buf);
        void DrawBuffers(GLsizei n, const GLenum* bufs);
        void ReadBuffer(GLenum src);
        void DeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
        GLenum CheckFramebufferStatus(GLenum target);
        void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                             GLint dstY1, GLbitfield mask, GLenum filter);
        void BindFramebuffer(GLenum target, GLuint framebuffer);

        namespace FramebufferImpl {
            struct DefaultFramebufferInfo {
                SharedPtr<MG_State::GLState::FramebufferObject> defaultFBO;
                SharedPtr<MG_State::GLState::ITextureObject> colorAttachment;
                SharedPtr<MG_State::GLState::ITextureObject> depthAttachment;
                SharedPtr<MG_State::GLState::ITextureObject> stencilAttachment;
            };

            extern DefaultFramebufferInfo* pDefaultFramebufferInfo;
        } // namespace FramebufferImpl
    } // namespace MG_Impl::GLImpl
} // namespace MobileGL
