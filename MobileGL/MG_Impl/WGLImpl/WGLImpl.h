// MobileGL - MobileGL/MG_Impl/WGLImpl/WGLImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#if defined(_WIN32)

namespace MobileGL::MG_Impl::WGLImpl {
    // Classic opengl32.dll surface. gdi32's ChoosePixelFormat/SetPixelFormat/
    // DescribePixelFormat/GetPixelFormat/SwapBuffers forward into the loaded
    // opengl32.dll's wgl* exports, so these back both call paths.
    int ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR* pfd);
    int DescribePixelFormat(HDC hdc, int format, UINT size, PIXELFORMATDESCRIPTOR* pfd);
    int GetPixelFormat(HDC hdc);
    BOOL SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR* pfd);
    BOOL SwapBuffers(HDC hdc);

    HGLRC CreateContext(HDC hdc);
    BOOL DeleteContext(HGLRC hglrc);
    BOOL MakeCurrent(HDC hdc, HGLRC hglrc);
    HGLRC GetCurrentContext();
    HDC GetCurrentDC();
    BOOL ShareLists(HGLRC hglrcShare, HGLRC hglrcDest);

    PROC GetProcAddress(const char* name);
} // namespace MobileGL::MG_Impl::WGLImpl

#endif // _WIN32
