// MobileGL - MobileGL/MG_Impl/CGLImpl/CGLImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/OpenGL.h>

namespace MobileGL::MG_Impl::CGLImpl {
    CGLError ChoosePixelFormat(const CGLPixelFormatAttribute* attribs, CGLPixelFormatObj* pix, GLint* npix);
    CGLError DestroyPixelFormat(CGLPixelFormatObj pix);
    CGLError DescribePixelFormat(CGLPixelFormatObj pix, GLint pixNum, CGLPixelFormatAttribute attrib, GLint* value);
    void ReleasePixelFormat(CGLPixelFormatObj pix);
    CGLPixelFormatObj RetainPixelFormat(CGLPixelFormatObj pix);
    GLuint GetPixelFormatRetainCount(CGLPixelFormatObj pix);

    CGLError CreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj* ctx);
    CGLError DestroyContext(CGLContextObj ctx);
    CGLContextObj RetainContext(CGLContextObj ctx);
    void ReleaseContext(CGLContextObj ctx);
    GLuint GetContextRetainCount(CGLContextObj ctx);
    CGLPixelFormatObj GetPixelFormat(CGLContextObj ctx);

    CGLError SetCurrentContext(CGLContextObj ctx);
    CGLContextObj GetCurrentContext();
    CGLError SetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint* params);
    CGLError GetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint* params);
    CGLError UpdateContext(CGLContextObj ctx);
    CGLError ClearDrawable(CGLContextObj ctx);
    CGLError FlushDrawable(CGLContextObj ctx);
    CGLError LockContext(CGLContextObj ctx);
    CGLError UnlockContext(CGLContextObj ctx);
    void GetVersion(GLint* majorvers, GLint* minorvers);
    const char* ErrorString(CGLError error);

    CGLError AttachDrawable(CGLContextObj ctx, void* nsView, void* metalLayer);
    void* GetContextNSObject(CGLContextObj ctx);
    void SetContextNSObject(CGLContextObj ctx, void* nsObject);
}
#endif
