// MobileGL - MobileGL/MG_Impl/WGLImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// wingdi.h declares most wgl* entry points as WINGDIAPI (__declspec(dllimport)),
// which would reject our definitions. _GDI32_ is the SDK's "I am the module that
// implements these" switch: it turns WINGDIAPI into a plain declaration. It must
// be defined before the first windows.h inclusion in this translation unit.
#if defined(_WIN32) && !defined(_GDI32_)
#define _GDI32_ 1
#endif

#include <Includes.h>

#if defined(_WIN32)
#include "../WGLImpl.h"

namespace WGL = MobileGL::MG_Impl::WGLImpl;

// ---- Pixel-format entry points (gdi32 forwards ChoosePixelFormat/SetPixelFormat/
// ---- DescribePixelFormat/GetPixelFormat/SwapBuffers into these exports) ----

extern "C" int WINAPI wglChoosePixelFormat(HDC hdc, CONST PIXELFORMATDESCRIPTOR* ppfd) {
    return WGL::ChoosePixelFormat(hdc, ppfd);
}

extern "C" int WINAPI wglDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes,
                                                   LPPIXELFORMATDESCRIPTOR ppfd) {
    return WGL::DescribePixelFormat(hdc, iPixelFormat, nBytes, ppfd);
}

extern "C" int WINAPI wglGetPixelFormat(HDC hdc) {
    return WGL::GetPixelFormat(hdc);
}

extern "C" BOOL WINAPI wglSetPixelFormat(HDC hdc, int iPixelFormat, CONST PIXELFORMATDESCRIPTOR* ppfd) {
    return WGL::SetPixelFormat(hdc, iPixelFormat, ppfd);
}

extern "C" BOOL WINAPI wglSwapBuffers(HDC hdc) {
    return WGL::SwapBuffers(hdc);
}

// ---- Context management ----

extern "C" HGLRC WINAPI wglCreateContext(HDC hdc) {
    return WGL::CreateContext(hdc);
}

extern "C" HGLRC WINAPI wglCreateLayerContext(HDC hdc, int iLayerPlane) {
    return iLayerPlane == 0 ? WGL::CreateContext(hdc) : nullptr;
}

extern "C" BOOL WINAPI wglCopyContext(HGLRC, HGLRC, UINT) {
    MGLOG_W_ONCE("wglCopyContext is not supported");
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

extern "C" BOOL WINAPI wglDeleteContext(HGLRC hglrc) {
    return WGL::DeleteContext(hglrc);
}

extern "C" HGLRC WINAPI wglGetCurrentContext(VOID) {
    return WGL::GetCurrentContext();
}

extern "C" HDC WINAPI wglGetCurrentDC(VOID) {
    return WGL::GetCurrentDC();
}

extern "C" BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC hglrc) {
    return WGL::MakeCurrent(hdc, hglrc);
}

extern "C" BOOL WINAPI wglShareLists(HGLRC hglrcShare, HGLRC hglrcDest) {
    return WGL::ShareLists(hglrcShare, hglrcDest);
}

// ---- Proc address ----

extern "C" PROC WINAPI wglGetProcAddress(LPCSTR lpszProc) {
    return WGL::GetProcAddress(lpszProc);
}

extern "C" PROC WINAPI wglGetDefaultProcAddress(LPCSTR lpszProc) {
    return WGL::GetProcAddress(lpszProc);
}

// ---- Layer planes and palettes (unsupported; overlay planes do not exist here) ----

extern "C" BOOL WINAPI wglDescribeLayerPlane(HDC, int, int, UINT, LPLAYERPLANEDESCRIPTOR) {
    return FALSE;
}

extern "C" int WINAPI wglSetLayerPaletteEntries(HDC, int, int, int, CONST COLORREF*) {
    return 0;
}

extern "C" int WINAPI wglGetLayerPaletteEntries(HDC, int, int, int, COLORREF*) {
    return 0;
}

extern "C" BOOL WINAPI wglRealizeLayerPalette(HDC, int, BOOL) {
    return FALSE;
}

extern "C" BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT fuPlanes) {
    if (fuPlanes & WGL_SWAP_MAIN_PLANE) {
        return WGL::SwapBuffers(hdc);
    }
    return FALSE;
}

extern "C" DWORD WINAPI wglSwapMultipleBuffers(UINT n, CONST WGLSWAP* ps) {
    if (!ps) {
        return 0;
    }
    DWORD swapped = 0;
    for (UINT i = 0; i < n; ++i) {
        if (WGL::SwapBuffers(ps[i].hdc)) {
            ++swapped;
        }
    }
    return swapped;
}

// ---- Font rendering (legacy immediate-mode feature; not supported) ----

extern "C" BOOL WINAPI wglUseFontBitmapsA(HDC, DWORD, DWORD, DWORD) {
    MGLOG_W_ONCE("wglUseFontBitmapsA is not supported");
    return FALSE;
}

extern "C" BOOL WINAPI wglUseFontBitmapsW(HDC, DWORD, DWORD, DWORD) {
    MGLOG_W_ONCE("wglUseFontBitmapsW is not supported");
    return FALSE;
}

extern "C" BOOL WINAPI wglUseFontOutlinesA(HDC, DWORD, DWORD, DWORD, FLOAT, FLOAT, int,
                                                 LPGLYPHMETRICSFLOAT) {
    MGLOG_W_ONCE("wglUseFontOutlinesA is not supported");
    return FALSE;
}

extern "C" BOOL WINAPI wglUseFontOutlinesW(HDC, DWORD, DWORD, DWORD, FLOAT, FLOAT, int,
                                                 LPGLYPHMETRICSFLOAT) {
    MGLOG_W_ONCE("wglUseFontOutlinesW is not supported");
    return FALSE;
}

#endif // _WIN32
