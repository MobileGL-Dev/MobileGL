// MobileGL - MobileGL/MG_Impl/GLXImpl/GLXImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#if defined(__linux__) && !defined(__ANDROID__)

namespace MobileGL::MG_Impl::GLXImpl {
    // GLX layered on MobileGL's own EGL, mirroring WGLImpl/CGLImpl. Handles are
    // opaque to callers; XVisualInfo crosses the ABI as void* so this header
    // needs no Xlib includes (Includes.h forward-declares Display/XID/Window).
    using GLXFBConfigHandle = void*;
    using GLXContextHandle = void*;
    using GLXDrawableHandle = unsigned long; // XID

    int QueryExtension(Display* dpy, int* errorBase, int* eventBase);
    int QueryVersion(Display* dpy, int* major, int* minor);
    const char* QueryExtensionsString(Display* dpy, int screen);
    const char* GetClientString(Display* dpy, int name);
    const char* QueryServerString(Display* dpy, int screen, int name);

    GLXFBConfigHandle* GetFBConfigs(Display* dpy, int screen, int* nelements);
    GLXFBConfigHandle* ChooseFBConfig(Display* dpy, int screen, const int* attribList, int* nelements);
    int GetFBConfigAttrib(Display* dpy, GLXFBConfigHandle config, int attribute, int* value);
    void* GetVisualFromFBConfig(Display* dpy, GLXFBConfigHandle config);
    void* ChooseVisual(Display* dpy, int screen, int* attribList);
    int GetConfig(Display* dpy, void* visualInfo, int attribute, int* value);

    GLXContextHandle CreateContext(Display* dpy, void* visualInfo, GLXContextHandle share, int direct);
    GLXContextHandle CreateNewContext(Display* dpy, GLXFBConfigHandle config, int renderType,
                                      GLXContextHandle share, int direct);
    GLXContextHandle CreateContextAttribsARB(Display* dpy, GLXFBConfigHandle config, GLXContextHandle share,
                                             int direct, const int* attribList);
    void DestroyContext(Display* dpy, GLXContextHandle context);
    int MakeCurrent(Display* dpy, GLXDrawableHandle drawable, GLXContextHandle context);
    int MakeContextCurrent(Display* dpy, GLXDrawableHandle draw, GLXDrawableHandle read,
                           GLXContextHandle context);
    void SwapBuffers(Display* dpy, GLXDrawableHandle drawable);

    GLXDrawableHandle CreateWindow(Display* dpy, GLXFBConfigHandle config, GLXDrawableHandle window,
                                   const int* attribList);
    void DestroyWindow(Display* dpy, GLXDrawableHandle window);

    GLXContextHandle GetCurrentContext();
    GLXDrawableHandle GetCurrentDrawable();
    GLXDrawableHandle GetCurrentReadDrawable();
    Display* GetCurrentDisplay();
    int IsDirect(Display* dpy, GLXContextHandle context);
    void WaitGL();
    void WaitX();
    int QueryContext(Display* dpy, GLXContextHandle context, int attribute, int* value);
    void QueryDrawable(Display* dpy, GLXDrawableHandle drawable, int attribute, unsigned int* value);

    void SwapIntervalEXT(Display* dpy, GLXDrawableHandle drawable, int interval);
    int SwapIntervalMESA(unsigned int interval);
    int GetSwapIntervalMESA();
    int SwapIntervalSGI(int interval);

    // Name -> exported glX entry point (table lives with the exports).
    void* GetGLXEntryPoint(const char* name);
} // namespace MobileGL::MG_Impl::GLXImpl

#endif // __linux__ && !__ANDROID__
