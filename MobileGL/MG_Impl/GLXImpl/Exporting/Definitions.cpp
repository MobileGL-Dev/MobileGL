// MobileGL - MobileGL/MG_Impl/GLXImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>
#include "../LookUp/LookUp.h"

MOBILEGL_GLX_API void* glXGetProcAddress(const char* name) {
    return MG_Impl::GLXImpl::GetProcAddress(name);
}

MOBILEGL_GLX_API void* glXGetProcAddressARB(const char* name) {
    return MG_Impl::GLXImpl::GetProcAddressARB(name);
}

#if defined(__linux__) && !defined(__ANDROID__)
#include "../GLXImpl.h"

namespace GLXImpl = MobileGL::MG_Impl::GLXImpl;

// GLX handle/type spellings from GL/glx.h, expressed without including it:
// GLXContext/GLXFBConfig are opaque pointers, drawables are XIDs, Bool is int,
// and XVisualInfo* crosses as void*.

MOBILEGL_GLX_API int glXQueryExtension(Display* dpy, int* errorBase, int* eventBase) {
    return GLXImpl::QueryExtension(dpy, errorBase, eventBase);
}

MOBILEGL_GLX_API int glXQueryVersion(Display* dpy, int* major, int* minor) {
    return GLXImpl::QueryVersion(dpy, major, minor);
}

MOBILEGL_GLX_API const char* glXQueryExtensionsString(Display* dpy, int screen) {
    return GLXImpl::QueryExtensionsString(dpy, screen);
}

MOBILEGL_GLX_API const char* glXGetClientString(Display* dpy, int name) {
    return GLXImpl::GetClientString(dpy, name);
}

MOBILEGL_GLX_API const char* glXQueryServerString(Display* dpy, int screen, int name) {
    return GLXImpl::QueryServerString(dpy, screen, name);
}

MOBILEGL_GLX_API void** glXGetFBConfigs(Display* dpy, int screen, int* nelements) {
    return GLXImpl::GetFBConfigs(dpy, screen, nelements);
}

MOBILEGL_GLX_API void** glXChooseFBConfig(Display* dpy, int screen, const int* attribList,
                                          int* nelements) {
    return GLXImpl::ChooseFBConfig(dpy, screen, attribList, nelements);
}

MOBILEGL_GLX_API int glXGetFBConfigAttrib(Display* dpy, void* config, int attribute, int* value) {
    return GLXImpl::GetFBConfigAttrib(dpy, config, attribute, value);
}

MOBILEGL_GLX_API void* glXGetVisualFromFBConfig(Display* dpy, void* config) {
    return GLXImpl::GetVisualFromFBConfig(dpy, config);
}

MOBILEGL_GLX_API void* glXChooseVisual(Display* dpy, int screen, int* attribList) {
    return GLXImpl::ChooseVisual(dpy, screen, attribList);
}

MOBILEGL_GLX_API int glXGetConfig(Display* dpy, void* visualInfo, int attribute, int* value) {
    return GLXImpl::GetConfig(dpy, visualInfo, attribute, value);
}

MOBILEGL_GLX_API void* glXCreateContext(Display* dpy, void* visualInfo, void* shareList, int direct) {
    return GLXImpl::CreateContext(dpy, visualInfo, shareList, direct);
}

MOBILEGL_GLX_API void* glXCreateNewContext(Display* dpy, void* config, int renderType,
                                           void* shareList, int direct) {
    return GLXImpl::CreateNewContext(dpy, config, renderType, shareList, direct);
}

MOBILEGL_GLX_API void* glXCreateContextAttribsARB(Display* dpy, void* config, void* shareContext,
                                                  int direct, const int* attribList) {
    return GLXImpl::CreateContextAttribsARB(dpy, config, shareContext, direct, attribList);
}

MOBILEGL_GLX_API void glXDestroyContext(Display* dpy, void* context) {
    GLXImpl::DestroyContext(dpy, context);
}

MOBILEGL_GLX_API int glXMakeCurrent(Display* dpy, unsigned long drawable, void* context) {
    return GLXImpl::MakeCurrent(dpy, drawable, context);
}

MOBILEGL_GLX_API int glXMakeContextCurrent(Display* dpy, unsigned long draw, unsigned long read,
                                           void* context) {
    return GLXImpl::MakeContextCurrent(dpy, draw, read, context);
}

MOBILEGL_GLX_API void glXSwapBuffers(Display* dpy, unsigned long drawable) {
    GLXImpl::SwapBuffers(dpy, drawable);
}

MOBILEGL_GLX_API unsigned long glXCreateWindow(Display* dpy, void* config, unsigned long window,
                                               const int* attribList) {
    return GLXImpl::CreateWindow(dpy, config, window, attribList);
}

MOBILEGL_GLX_API void glXDestroyWindow(Display* dpy, unsigned long window) {
    GLXImpl::DestroyWindow(dpy, window);
}

MOBILEGL_GLX_API void* glXGetCurrentContext() {
    return GLXImpl::GetCurrentContext();
}

MOBILEGL_GLX_API unsigned long glXGetCurrentDrawable() {
    return GLXImpl::GetCurrentDrawable();
}

MOBILEGL_GLX_API unsigned long glXGetCurrentReadDrawable() {
    return GLXImpl::GetCurrentReadDrawable();
}

MOBILEGL_GLX_API Display* glXGetCurrentDisplay() {
    return GLXImpl::GetCurrentDisplay();
}

MOBILEGL_GLX_API int glXIsDirect(Display* dpy, void* context) {
    return GLXImpl::IsDirect(dpy, context);
}

MOBILEGL_GLX_API void glXWaitGL() {
    GLXImpl::WaitGL();
}

MOBILEGL_GLX_API void glXWaitX() {
    GLXImpl::WaitX();
}

MOBILEGL_GLX_API int glXQueryContext(Display* dpy, void* context, int attribute, int* value) {
    return GLXImpl::QueryContext(dpy, context, attribute, value);
}

MOBILEGL_GLX_API void glXQueryDrawable(Display* dpy, unsigned long drawable, int attribute,
                                       unsigned int* value) {
    GLXImpl::QueryDrawable(dpy, drawable, attribute, value);
}

MOBILEGL_GLX_API void glXSwapIntervalEXT(Display* dpy, unsigned long drawable, int interval) {
    GLXImpl::SwapIntervalEXT(dpy, drawable, interval);
}

MOBILEGL_GLX_API int glXSwapIntervalMESA(unsigned int interval) {
    return GLXImpl::SwapIntervalMESA(interval);
}

MOBILEGL_GLX_API int glXGetSwapIntervalMESA() {
    return GLXImpl::GetSwapIntervalMESA();
}

MOBILEGL_GLX_API int glXSwapIntervalSGI(int interval) {
    return GLXImpl::SwapIntervalSGI(interval);
}

// Legacy entry points some loaders probe for; harmless no-op stubs.
MOBILEGL_GLX_API void glXCopyContext(Display*, void*, void*, unsigned long) {
    MGLOG_W_ONCE("glx: glXCopyContext is not supported");
}

MOBILEGL_GLX_API unsigned long glXCreateGLXPixmap(Display*, void*, unsigned long) {
    MGLOG_W_ONCE("glx: glXCreateGLXPixmap is not supported");
    return 0;
}

MOBILEGL_GLX_API void glXDestroyGLXPixmap(Display*, unsigned long) {}

MOBILEGL_GLX_API unsigned long glXCreatePixmap(Display*, void*, unsigned long, const int*) {
    MGLOG_W_ONCE("glx: glXCreatePixmap is not supported");
    return 0;
}

MOBILEGL_GLX_API void glXDestroyPixmap(Display*, unsigned long) {}

MOBILEGL_GLX_API unsigned long glXCreatePbuffer(Display*, void*, const int*) {
    MGLOG_W_ONCE("glx: glXCreatePbuffer is not supported");
    return 0;
}

MOBILEGL_GLX_API void glXDestroyPbuffer(Display*, unsigned long) {}

MOBILEGL_GLX_API void glXUseXFont(unsigned long, int, int, int) {
    MGLOG_W_ONCE("glx: glXUseXFont is not supported");
}

MOBILEGL_GLX_API void glXSelectEvent(Display*, unsigned long, unsigned long) {}

MOBILEGL_GLX_API void glXGetSelectedEvent(Display*, unsigned long, unsigned long* eventMask) {
    if (eventMask) {
        *eventMask = 0;
    }
}

namespace MobileGL::MG_Impl::GLXImpl {
    namespace {
        struct GLXEntryPoint {
            const char* Name;
            void* Proc;
        };

        const GLXEntryPoint kGLXEntryPoints[] = {
            {"glXChooseFBConfig", reinterpret_cast<void*>(glXChooseFBConfig)},
            {"glXChooseVisual", reinterpret_cast<void*>(glXChooseVisual)},
            {"glXCopyContext", reinterpret_cast<void*>(glXCopyContext)},
            {"glXCreateContext", reinterpret_cast<void*>(glXCreateContext)},
            {"glXCreateContextAttribsARB", reinterpret_cast<void*>(glXCreateContextAttribsARB)},
            {"glXCreateGLXPixmap", reinterpret_cast<void*>(glXCreateGLXPixmap)},
            {"glXCreateNewContext", reinterpret_cast<void*>(glXCreateNewContext)},
            {"glXCreatePbuffer", reinterpret_cast<void*>(glXCreatePbuffer)},
            {"glXCreatePixmap", reinterpret_cast<void*>(glXCreatePixmap)},
            {"glXCreateWindow", reinterpret_cast<void*>(glXCreateWindow)},
            {"glXDestroyContext", reinterpret_cast<void*>(glXDestroyContext)},
            {"glXDestroyGLXPixmap", reinterpret_cast<void*>(glXDestroyGLXPixmap)},
            {"glXDestroyPbuffer", reinterpret_cast<void*>(glXDestroyPbuffer)},
            {"glXDestroyPixmap", reinterpret_cast<void*>(glXDestroyPixmap)},
            {"glXDestroyWindow", reinterpret_cast<void*>(glXDestroyWindow)},
            {"glXGetClientString", reinterpret_cast<void*>(glXGetClientString)},
            {"glXGetConfig", reinterpret_cast<void*>(glXGetConfig)},
            {"glXGetCurrentContext", reinterpret_cast<void*>(glXGetCurrentContext)},
            {"glXGetCurrentDisplay", reinterpret_cast<void*>(glXGetCurrentDisplay)},
            {"glXGetCurrentDrawable", reinterpret_cast<void*>(glXGetCurrentDrawable)},
            {"glXGetCurrentReadDrawable", reinterpret_cast<void*>(glXGetCurrentReadDrawable)},
            {"glXGetFBConfigAttrib", reinterpret_cast<void*>(glXGetFBConfigAttrib)},
            {"glXGetFBConfigs", reinterpret_cast<void*>(glXGetFBConfigs)},
            {"glXGetProcAddress", reinterpret_cast<void*>(glXGetProcAddress)},
            {"glXGetProcAddressARB", reinterpret_cast<void*>(glXGetProcAddressARB)},
            {"glXGetSelectedEvent", reinterpret_cast<void*>(glXGetSelectedEvent)},
            {"glXGetSwapIntervalMESA", reinterpret_cast<void*>(glXGetSwapIntervalMESA)},
            {"glXGetVisualFromFBConfig", reinterpret_cast<void*>(glXGetVisualFromFBConfig)},
            {"glXIsDirect", reinterpret_cast<void*>(glXIsDirect)},
            {"glXMakeContextCurrent", reinterpret_cast<void*>(glXMakeContextCurrent)},
            {"glXMakeCurrent", reinterpret_cast<void*>(glXMakeCurrent)},
            {"glXQueryContext", reinterpret_cast<void*>(glXQueryContext)},
            {"glXQueryDrawable", reinterpret_cast<void*>(glXQueryDrawable)},
            {"glXQueryExtension", reinterpret_cast<void*>(glXQueryExtension)},
            {"glXQueryExtensionsString", reinterpret_cast<void*>(glXQueryExtensionsString)},
            {"glXQueryServerString", reinterpret_cast<void*>(glXQueryServerString)},
            {"glXQueryVersion", reinterpret_cast<void*>(glXQueryVersion)},
            {"glXSelectEvent", reinterpret_cast<void*>(glXSelectEvent)},
            {"glXSwapBuffers", reinterpret_cast<void*>(glXSwapBuffers)},
            {"glXSwapIntervalEXT", reinterpret_cast<void*>(glXSwapIntervalEXT)},
            {"glXSwapIntervalMESA", reinterpret_cast<void*>(glXSwapIntervalMESA)},
            {"glXSwapIntervalSGI", reinterpret_cast<void*>(glXSwapIntervalSGI)},
            {"glXUseXFont", reinterpret_cast<void*>(glXUseXFont)},
            {"glXWaitGL", reinterpret_cast<void*>(glXWaitGL)},
            {"glXWaitX", reinterpret_cast<void*>(glXWaitX)},
        };
    } // namespace

    void* GetGLXEntryPoint(const char* name) {
        for (const auto& entry : kGLXEntryPoints) {
            if (std::strcmp(entry.Name, name) == 0) {
                return entry.Proc;
            }
        }
        return nullptr;
    }
} // namespace MobileGL::MG_Impl::GLXImpl

#endif // __linux__ && !__ANDROID__
