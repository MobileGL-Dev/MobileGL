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
    // Destroys every still-registered query object exactly as DeleteQueries would.
    // Query objects are context-owned, and MobileGL::Destroy() tears every context
    // down, so the process-global registry has to be drained there: without this the
    // QueryObject and any backend timer-query wrapper leaked across every
    // eglTerminate/eglInitialize cycle, and the active-query/name-allocator state
    // from the dead context survived into the next one. Must run while the backend
    // function table is still populated, and before a re-initialized library could
    // pair the handles with the wrong backend's DeleteBackendQuery.
    void DestroyAllQueryObjects();
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
} // namespace MobileGL::MG_Impl::GLImpl
