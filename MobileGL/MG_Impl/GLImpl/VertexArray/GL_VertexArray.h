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
    // Packed current-value setters (GL_INT_/GL_UNSIGNED_INT_2_10_10_10_REV). Decode one packed word
    // into the first 1/2/3/4 components of the generic attribute's float current value.
    void VertexAttribP1ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void VertexAttribP2ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void VertexAttribP3ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void VertexAttribP4ui(GLuint index, GLenum type, GLboolean normalized, GLuint value);
    void VertexAttribP1uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void VertexAttribP2uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void VertexAttribP3uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void VertexAttribP4uiv(GLuint index, GLenum type, GLboolean normalized, const GLuint* value);
    void VertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
    void VertexAttrib4Nubv(GLuint index, const GLubyte* v);
    void VertexAttrib4ubv(GLuint index, const GLubyte* v);
    // Float-domain current-value setters that funnel into VertexAttrib4f. The d/s/bv/iv/uiv/usv
    // forms are value-preserving (only the 4N* forms normalize per GL 3.3 Core Eq 2.1/2.2).
    void VertexAttrib1d(GLuint index, GLdouble x);
    void VertexAttrib2d(GLuint index, GLdouble x, GLdouble y);
    void VertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z);
    void VertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
    void VertexAttrib1dv(GLuint index, const GLdouble* v);
    void VertexAttrib2dv(GLuint index, const GLdouble* v);
    void VertexAttrib3dv(GLuint index, const GLdouble* v);
    void VertexAttrib4dv(GLuint index, const GLdouble* v);
    void VertexAttrib1s(GLuint index, GLshort x);
    void VertexAttrib2s(GLuint index, GLshort x, GLshort y);
    void VertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z);
    void VertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);
    void VertexAttrib1sv(GLuint index, const GLshort* v);
    void VertexAttrib2sv(GLuint index, const GLshort* v);
    void VertexAttrib3sv(GLuint index, const GLshort* v);
    void VertexAttrib4sv(GLuint index, const GLshort* v);
    void VertexAttrib4bv(GLuint index, const GLbyte* v);
    void VertexAttrib4iv(GLuint index, const GLint* v);
    void VertexAttrib4uiv(GLuint index, const GLuint* v);
    void VertexAttrib4usv(GLuint index, const GLushort* v);
    void VertexAttrib4Nbv(GLuint index, const GLbyte* v);
    void VertexAttrib4Nsv(GLuint index, const GLshort* v);
    void VertexAttrib4Niv(GLuint index, const GLint* v);
    void VertexAttrib4Nusv(GLuint index, const GLushort* v);
    void VertexAttrib4Nuiv(GLuint index, const GLuint* v);
    // Pure-integer current-value setters that funnel into VertexAttribI4i / VertexAttribI4ui and
    // write the integer view, never the float one.
    void VertexAttribI1i(GLuint index, GLint x);
    void VertexAttribI2i(GLuint index, GLint x, GLint y);
    void VertexAttribI3i(GLuint index, GLint x, GLint y, GLint z);
    void VertexAttribI1ui(GLuint index, GLuint x);
    void VertexAttribI2ui(GLuint index, GLuint x, GLuint y);
    void VertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z);
    void VertexAttribI1iv(GLuint index, const GLint* v);
    void VertexAttribI2iv(GLuint index, const GLint* v);
    void VertexAttribI3iv(GLuint index, const GLint* v);
    void VertexAttribI1uiv(GLuint index, const GLuint* v);
    void VertexAttribI2uiv(GLuint index, const GLuint* v);
    void VertexAttribI3uiv(GLuint index, const GLuint* v);
    void VertexAttribI4bv(GLuint index, const GLbyte* v);
    void VertexAttribI4sv(GLuint index, const GLshort* v);
    void VertexAttribI4ubv(GLuint index, const GLubyte* v);
    void VertexAttribI4usv(GLuint index, const GLushort* v);
    void GetVertexAttribfv(GLuint index, GLenum pname, GLfloat* params);
    void GetVertexAttribdv(GLuint index, GLenum pname, GLdouble* params);
    void GetVertexAttribiv(GLuint index, GLenum pname, GLint* params);
    void GetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer);
    void GetVertexAttribIiv(GLuint index, GLenum pname, GLint* params);
    void GetVertexAttribIuiv(GLuint index, GLenum pname, GLuint* params);
    void CreateVertexArrays(GLsizei n, GLuint* arrays);
    void DisableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void EnableVertexArrayAttrib(GLuint vaobj, GLuint index);
    void VertexArrayElementBuffer(GLuint vaobj, GLuint buffer);
    void VertexArrayVertexBuffer(GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);
    void GetVertexArrayiv(GLuint vaobj, GLenum pname, GLint* param);
    void GetVertexArrayIndexediv(GLuint vaobj, GLuint index, GLenum pname, GLint* param);
    void GetVertexArrayIndexed64iv(GLuint vaobj, GLuint index, GLenum pname, GLint64* param);
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
