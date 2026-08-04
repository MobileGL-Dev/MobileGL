// MobileGL - MobileGL/MG_Impl/GLImpl/Drawing/GL_Drawing.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    /* @INSERTION_POINT:FUNCTION_DECLARATION@ */
    void BeginTransformFeedback(GLenum primitiveMode);
    void EndTransformFeedback(void);
    void PauseTransformFeedback(void);
    void ResumeTransformFeedback(void);
    void GenTransformFeedbacks(GLsizei n, GLuint* ids);
    void DeleteTransformFeedbacks(GLsizei n, const GLuint* ids);
    void BindTransformFeedback(GLenum target, GLuint id);
    GLboolean IsTransformFeedback(GLuint id);
    void DrawTransformFeedback(GLenum mode, GLuint id);
    void DrawTransformFeedbackInstanced(GLenum mode, GLuint id, GLsizei instancecount);
    void DrawTransformFeedbackStream(GLenum mode, GLuint id, GLuint stream);
    void DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint id, GLuint stream, GLsizei instancecount);
    void DispatchCompute(GLuint numGroupsX, GLuint numGroupsY, GLuint numGroupsZ);
    void DispatchComputeIndirect(GLintptr indirect);
    void PatchParameteri(GLenum pname, GLint value);
    void MemoryBarrier(GLbitfield barriers);
    void MemoryBarrierByRegion(GLbitfield barriers);
    void MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawcount, GLsizei stride);
    void MultiDrawElementsIndirectCount(GLenum mode, GLenum type, const void* indirect, GLintptr drawcount,
                                        GLsizei maxdrawcount, GLsizei stride);
    void MultiDrawArraysIndirectCount(GLenum mode, const void* indirect, GLintptr drawcount,
                                      GLsizei maxdrawcount, GLsizei stride);
    void DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                     const void* indices, GLint basevertex);
    void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
    void DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                     GLsizei instancecount, GLint basevertex, GLuint baseinstance);
    void DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                         GLsizei instancecount, GLint basevertex);
    void DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                           GLsizei instancecount, GLuint baseinstance);
    void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount);
    void DrawElementsIndirect(GLenum mode, GLenum type, const void* indirect);
    void DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instancecount,
                                         GLuint baseinstance);
    void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    void DrawArraysIndirect(GLenum mode, const void* indirect);
    void DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);
    void DrawArrays(GLenum mode, GLint first, GLsizei count);
    void MultiDrawArrays(GLenum mode, const GLint* first, const GLsizei* count, GLsizei drawcount);
    void MultiDrawElements(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                           GLsizei drawcount);
    void MultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                     GLsizei drawcount, const GLint* basevertex);
    void Clear(GLbitfield mask);
    void DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
} // namespace MobileGL::MG_Impl::GLImpl
