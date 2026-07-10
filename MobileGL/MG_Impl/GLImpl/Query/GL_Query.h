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
    void DeleteQueries(GLsizei n, const GLuint* ids);
    GLboolean IsQuery(GLuint id);
    void BeginQuery(GLenum target, GLuint id);
    void EndQuery(GLenum target);
    void GetQueryiv(GLenum target, GLenum pname, GLint* params);
    void GetQueryObjectiv(GLuint id, GLenum pname, GLint* params);
    void GetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params);
    void GetQueryObjecti64v(GLuint id, GLenum pname, GLint64* params);
    void GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params);
    void QueryCounter(GLuint id, GLenum target);
} // namespace MobileGL::MG_Impl::GLImpl
