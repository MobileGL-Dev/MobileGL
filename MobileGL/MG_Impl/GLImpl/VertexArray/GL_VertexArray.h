// MobileGL - MobileGL/MG_Impl/GLImpl/VertexArray/GL_VertexArray.h
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
    void VertexAttrib1f(GLuint index, GLfloat x);
    void VertexAttrib1fv(GLuint index, const GLfloat* v);
    void VertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
    void VertexAttrib2fv(GLuint index, const GLfloat* v);
    void VertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
    void VertexAttrib3fv(GLuint index, const GLfloat* v);
    void VertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void VertexAttrib4fv(GLuint index, const GLfloat* v);
    void VertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
    void VertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
    void VertexAttribI4iv(GLuint index, const GLint* v);
    void VertexAttribI4uiv(GLuint index, const GLuint* v);
    void VertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
    void VertexAttrib4Nubv(GLuint index, const GLubyte* v);
    void VertexAttrib4ubv(GLuint index, const GLubyte* v);
    void GetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params);
    void GetVertexAttribiv(GLuint index, GLenum pname, GLint* params);
    void GetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer);
    void GetVertexAttribIiv(GLuint index, GLenum pname, GLint* params);
    void GetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params);
    void CreateVertexArrays(GLsizei n, GLuint* arrays);
    void DisableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void EnableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void VertexArrayElementBuffer(GLuint vaobj, GLuint buffer);
    void VertexArrayVertexBuffer(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void VertexArrayAttribFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                 GLuint relativeoffset);
    void VertexArrayAttribIFormat(GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void VertexArrayAttribBinding(GLuint vaobj, GLuint attribindex, GLuint bindingindex);
    void VertexArrayBindingDivisor(GLuint vaobj, GLuint bindingindex, GLuint divisor);
    void VertexArrayVertexBuffers(GLuint vaobj, GLuint first, GLsizei count, const GLuint* buffers,
                                  const GLintptr* offsets, const GLsizei* strides);
    void BindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void BindVertexBuffers(GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                           const GLsizei* strides);
    void VertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset);
    void VertexAttribIFormat(GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset);
    void VertexAttribBinding(GLuint attribindex, GLuint bindingindex);
    void VertexBindingDivisor(GLuint bindingindex, GLuint divisor);
    void VertexAttribDivisor(GLuint index, GLuint divisor);
    GLboolean IsVertexArray(GLuint array);
    void DisableVertexAttribArray(GLuint index);
    void EnableVertexAttribArray(GLuint index);
    void VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
    void VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                             const void* pointer);
    void BindVertexArray(GLuint array);
    void DeleteVertexArrays(GLsizei n, const GLuint* arrays);
    void GenVertexArrays(GLsizei n, GLuint* arrays);
} // namespace MobileGL::MG_Impl::GLImpl
