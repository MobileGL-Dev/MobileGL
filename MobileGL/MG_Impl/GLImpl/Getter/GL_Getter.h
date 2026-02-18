// MobileGL - MobileGL/MG_Impl/GLImpl/Getter/GL_Getter.h
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
    const GLubyte* GetString(GLenum name);
    const GLubyte* GetStringi(GLenum name, GLuint index);
    void GetIntegerv(GLenum pname, GLint* params);
    GLenum GetError();
} // namespace MobileGL::MG_Impl::GLImpl
