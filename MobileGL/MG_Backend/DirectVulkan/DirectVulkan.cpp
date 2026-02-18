// MobileGL - MobileGL/MG_Backend/DirectVulkan/DirectVulkan.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectVulkan.h"
#include "TmpImpl.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    void Clear(GLbitfield mask) {
        MobileGL::MG_Backend::DirectVulkan::TmpImpl::Clear(mask);
    }

    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
        MobileGL::MG_Backend::DirectVulkan::TmpImpl::DrawElements(mode, count, type, indices);
    }

    void ClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {}
    void ClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {}
    void ClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value) {}
    void ClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value) {}
    void DrawArrays(GLenum mode, GLint first, GLsizei count) {}
    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices, GLint basevertex) {}
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                           GLsizei drawcount) {}
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const GLvoid* const* indices,
                                     GLsizei drawcount, const GLint* basevertex) {}
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride) {}
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride) {}
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex) {}
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices) {}
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance) {}
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex) {}
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance) {}
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount) {}
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {}
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance) {}
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {}
    void DrawArraysIndirect(GLenum mode, const void* indirect) {}
    void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                         GLint dstY1, GLbitfield mask, GLenum filter) {}
    void CopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width,
                        GLsizei height, GLint border) {}
    void CopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width,
                           GLsizei height) {}
    void GenerateMipmap(GLenum target) {}
    void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {}
    void GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid* pixels) {}

    Bool InitWindowSurface(NativeWindowType window) {
        if (!window) {
            MGLOG_E("Cannot initialize Vulkan window surface: invalid window handle");
            return false;
        }
        TmpImpl::InitVulkan(window);
        return true;
    }

    void Present() {
        TmpImpl::Present();
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
