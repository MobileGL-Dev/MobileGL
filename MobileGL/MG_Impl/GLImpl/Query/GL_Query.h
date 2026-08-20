// MobileGL - MobileGL/MG_Impl/GLImpl/Query/GL_Query.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::GLImpl {
    void GenQueries(GLsizei n, GLuint* ids);
    void CreateQueries(GLenum target, GLsizei n, GLuint* ids);
    void DeleteQueries(GLsizei n, const GLuint* ids);
    GLboolean IsQuery(GLuint id);
    void BeginQuery(GLenum target, GLuint id);
    void EndQuery(GLenum target);
    void GetQueryiv(GLenum target, GLenum pname, GLint* params);
    void BeginQueryIndexed(GLenum target, GLuint index, GLuint id);
    void EndQueryIndexed(GLenum target, GLuint index);
    void GetQueryIndexediv(GLenum target, GLuint index, GLenum pname, GLint* params);
    void GetQueryObjectiv(GLuint id, GLenum pname, GLint* params);
    void GetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
    void GetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params);
    void GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params);
    void GetQueryBufferObjectiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void GetQueryBufferObjectuiv(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void GetQueryBufferObjecti64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void GetQueryBufferObjectui64v(GLuint id, GLuint buffer, GLenum pname, GLintptr offset);
    void QueryCounter(GLuint id, GLenum target);
    // Conditional rendering (GL 4.6 core 10.9). Implemented here rather than beside the drawing
    // entry points because the predicate is a QUERY OBJECT's result, and the object registry -
    // with the lock that guards it - lives in this file.
    void BeginConditionalRender(GLuint id, GLenum mode);
    void EndConditionalRender();
    // Destroys every still-registered query object exactly as DeleteQueries would.
    // GL requires queries to die with their context; called only from full library
    // teardown (DestroyImpl), where no context survives on any thread, so the
    // process-global registry can be drained wholesale. Must run while the backend
    // function table is still populated: each backend handle has to be released by
    // the backend that created it, never by a later re-initialized one (whose
    // DeleteBackendQuery would cast the wrapper to the wrong backend's type).
    // Same contract as DestroyAllSyncObjects.
    void DestroyAllQueryObjects();
} // namespace MobileGL::MG_Impl::GLImpl
