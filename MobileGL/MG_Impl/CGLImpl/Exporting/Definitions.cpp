// MobileGL - MobileGL/MG_Impl/CGLImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "../CGLImpl.h"

#if defined(__APPLE__)

MOBILEGL_CGL_API CGLError CGLChoosePixelFormat(const CGLPixelFormatAttribute* attribs,
                                               CGLPixelFormatObj* pix,
                                               GLint* npix) {
    return MobileGL::MG_Impl::CGLImpl::ChoosePixelFormat(attribs, pix, npix);
}

MOBILEGL_CGL_API CGLError CGLDestroyPixelFormat(CGLPixelFormatObj pix) {
    return MobileGL::MG_Impl::CGLImpl::DestroyPixelFormat(pix);
}

MOBILEGL_CGL_API CGLError CGLDescribePixelFormat(CGLPixelFormatObj pix,
                                                 GLint pix_num,
                                                 CGLPixelFormatAttribute attrib,
                                                 GLint* value) {
    return MobileGL::MG_Impl::CGLImpl::DescribePixelFormat(pix, pix_num, attrib, value);
}

MOBILEGL_CGL_API void CGLReleasePixelFormat(CGLPixelFormatObj pix) {
    MobileGL::MG_Impl::CGLImpl::ReleasePixelFormat(pix);
}

MOBILEGL_CGL_API CGLPixelFormatObj CGLRetainPixelFormat(CGLPixelFormatObj pix) {
    return MobileGL::MG_Impl::CGLImpl::RetainPixelFormat(pix);
}

MOBILEGL_CGL_API GLuint CGLGetPixelFormatRetainCount(CGLPixelFormatObj pix) {
    return MobileGL::MG_Impl::CGLImpl::GetPixelFormatRetainCount(pix);
}

MOBILEGL_CGL_API CGLError CGLCreateContext(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj* ctx) {
    return MobileGL::MG_Impl::CGLImpl::CreateContext(pix, share, ctx);
}

MOBILEGL_CGL_API CGLError CGLDestroyContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::DestroyContext(ctx);
}

MOBILEGL_CGL_API CGLContextObj CGLRetainContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::RetainContext(ctx);
}

MOBILEGL_CGL_API void CGLReleaseContext(CGLContextObj ctx) {
    MobileGL::MG_Impl::CGLImpl::ReleaseContext(ctx);
}

MOBILEGL_CGL_API GLuint CGLGetContextRetainCount(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::GetContextRetainCount(ctx);
}

MOBILEGL_CGL_API CGLPixelFormatObj CGLGetPixelFormat(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::GetPixelFormat(ctx);
}

MOBILEGL_CGL_API CGLError CGLSetCurrentContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::SetCurrentContext(ctx);
}

MOBILEGL_CGL_API CGLContextObj CGLGetCurrentContext(void) {
    return MobileGL::MG_Impl::CGLImpl::GetCurrentContext();
}

MOBILEGL_CGL_API CGLError CGLSetParameter(CGLContextObj ctx, CGLContextParameter pname, const GLint* params) {
    return MobileGL::MG_Impl::CGLImpl::SetParameter(ctx, pname, params);
}

MOBILEGL_CGL_API CGLError CGLGetParameter(CGLContextObj ctx, CGLContextParameter pname, GLint* params) {
    return MobileGL::MG_Impl::CGLImpl::GetParameter(ctx, pname, params);
}

MOBILEGL_CGL_API CGLError CGLUpdateContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::UpdateContext(ctx);
}

MOBILEGL_CGL_API CGLError CGLClearDrawable(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::ClearDrawable(ctx);
}

MOBILEGL_CGL_API CGLError CGLFlushDrawable(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::FlushDrawable(ctx);
}

MOBILEGL_CGL_API CGLError CGLLockContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::LockContext(ctx);
}

MOBILEGL_CGL_API CGLError CGLUnlockContext(CGLContextObj ctx) {
    return MobileGL::MG_Impl::CGLImpl::UnlockContext(ctx);
}

MOBILEGL_CGL_API void CGLGetVersion(GLint* majorvers, GLint* minorvers) {
    MobileGL::MG_Impl::CGLImpl::GetVersion(majorvers, minorvers);
}

MOBILEGL_CGL_API const char* CGLErrorString(CGLError error) {
    return MobileGL::MG_Impl::CGLImpl::ErrorString(error);
}

#endif
