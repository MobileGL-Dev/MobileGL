// MobileGL - MobileGL/MG_Impl/GLImpl/Buffer/GL_Buffer.h
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
    void GetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
    void GetBufferParameteri64v(GLenum target, GLenum pname, GLint64* params);
    void GetBufferPointerv(GLenum target, GLenum pname, void** params);
    GLboolean IsBuffer(GLuint buffer);
    void DeleteBuffers(GLsizei n, const GLuint* buffers);
    void FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
    GLboolean UnmapBuffer(GLenum target);
    void* MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    void* MapBuffer(GLenum target, GLenum access);
    void BufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);
    void CreateBuffers(GLsizei n, GLuint* buffers);
    void NamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags);
    void NamedBufferData(GLuint buffer, GLsizeiptr size, const void* data, GLenum usage);
    void NamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data);
    void CopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset,
                                GLsizeiptr size);
    void ClearNamedBufferData(GLuint buffer, GLenum internalformat, GLenum format, GLenum type, const void* data);
    void ClearNamedBufferSubData(GLuint buffer, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format,
                                 GLenum type, const void* data);
    void* MapNamedBuffer(GLuint buffer, GLenum access);
    void* MapNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access);
    GLboolean UnmapNamedBuffer(GLuint buffer);
    void FlushMappedNamedBufferRange(GLuint buffer, GLintptr offset, GLsizeiptr length);
    void GetNamedBufferParameteriv(GLuint buffer, GLenum pname, GLint* params);
    void GetNamedBufferParameteri64v(GLuint buffer, GLenum pname, GLint64* params);
    void GetNamedBufferPointerv(GLuint buffer, GLenum pname, void** params);
    void CopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset,
                           GLsizeiptr size);
    void BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data);
    void GetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);
    void GetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, void* data);
    void BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
    void BindBuffer(GLenum target, GLuint buffer);
    void GenBuffers(GLsizei n, GLuint* buffers);
    void BindBufferBase(GLenum target, GLuint index, GLuint buffer);
    void BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void BindBuffersBase(GLenum target, GLuint first, GLsizei count, const GLuint* buffers);
    void BindBuffersRange(GLenum target, GLuint first, GLsizei count, const GLuint* buffers, const GLintptr* offsets,
                          const GLsizeiptr* sizes);

} // namespace MobileGL::MG_Impl::GLImpl
