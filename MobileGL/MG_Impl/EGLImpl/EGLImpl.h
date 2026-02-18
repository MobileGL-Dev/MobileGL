// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLImpl.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Impl::EGLImpl {
    EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                   const EGLint* attrib_list);
    EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw);
    EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config);
    EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list);
    EGLBoolean Initialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
    EGLDisplay GetDisplay(NativeDisplayType display);
    EGLint GetError();
    EGLBoolean MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx);
    EGLBoolean DestroySurface(EGLDisplay dpy, EGLSurface surface);
    EGLBoolean Terminate(EGLDisplay dpy);
    EGLBoolean ReleaseThread();
    EGLContext GetCurrentContext();
    EGLBoolean GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
    EGLBoolean BindAPI(EGLenum api);
    EGLSurface GetCurrentSurface(EGLint readdraw);
    EGLBoolean QuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value);
    const char* QueryString(EGLDisplay display, EGLint name);
    EGLBoolean SwapInterval(EGLDisplay dpy, EGLint interval);
    EGLSurface CreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list);
    EGLBoolean BindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean ReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer);
    EGLBoolean CopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target);
    EGLSurface CreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config,
                                             const EGLint* attrib_list);
    EGLSurface CreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                   const EGLint* attrib_list);
    EGLBoolean GetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config);
    EGLDisplay GetCurrentDisplay();
    EGLenum QueryAPI();
    EGLBoolean QueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value);
    EGLBoolean SurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
    EGLBoolean WaitClient();
    EGLBoolean WaitGL();
    EGLBoolean WaitNative(EGLint engine);
    EGLSync CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list);
    EGLBoolean DestroySync(void* dpy, void* sync);
    EGLint ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout);
    EGLBoolean GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value);
    EGLImage CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                         const EGLAttrib* attrib_list);
    EGLBoolean DestroyImage(EGLDisplay dpy, EGLImage image);
    EGLDisplay GetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list);
    EGLSurface CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                           const EGLAttrib* attrib_list);
    EGLSurface CreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                           const EGLAttrib* attrib_list);
    EGLBoolean WaitSync(void* dpy, void* sync, int flags);
    __eglMustCastToProperFunctionPointerType GetProcAddress(const char* name);
} // namespace MobileGL::MG_Impl::EGLImpl
