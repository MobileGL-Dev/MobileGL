// MobileGL - MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>
#include "../EGLImpl.h"

MOBILEGL_EGL_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                                   const EGLint* attrib_list) {
    MGLOG_D("eglCreateWindowSurface(dpy=%p, config=%p, window=%p, attrib_list=%p)", dpy, config, window, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateWindowSurface(dpy, config, window, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs,
                                            EGLint config_size, EGLint* num_config) {
    MGLOG_D("eglChooseConfig(dpy=%p, attrib_list=%p, configs=%p, config_size=%d, num_config=%p)", dpy, attrib_list,
            configs, config_size, num_config);
    return MobileGL::MG_Impl::EGLImpl::ChooseConfig(dpy, attrib_list, configs, config_size, num_config);
}

MOBILEGL_EGL_API EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx,
                                             const EGLint* attrib_list) {
    MGLOG_D("eglCreateContext(dpy=%p, config=%p, shareCtx=%p, attrib_list=%p)", dpy, config, shareCtx, attrib_list);
#ifdef TRACY_ENABLE
    tracy::StartupProfiler();
#endif
    return MobileGL::MG_Impl::EGLImpl::CreateContext(dpy, config, shareCtx, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    MGLOG_D("eglInitialize(dpy=%p, major=%p, minor=%p)", dpy, major, minor);
    return MobileGL::MG_Impl::EGLImpl::Initialize(dpy, major, minor);
}

MOBILEGL_EGL_API EGLDisplay eglGetDisplay(NativeDisplayType display) {
    MGLOG_D("eglGetDisplay(display=%p)", display);
    return MobileGL::MG_Impl::EGLImpl::GetDisplay(display);
}

MOBILEGL_EGL_API EGLint eglGetError() {
    MGLOG_D("eglGetError()");
    return MobileGL::MG_Impl::EGLImpl::GetError();
}

MOBILEGL_EGL_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    MGLOG_D("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)", dpy, draw, read, ctx);
    return MobileGL::MG_Impl::EGLImpl::MakeCurrent(dpy, draw, read, ctx);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    MGLOG_D("eglDestroyContext(dpy=%p, ctx=%p)", dpy, ctx);
#ifdef TRACY_ENABLE
    tracy::ShutdownProfiler();
#endif
    return MobileGL::MG_Impl::EGLImpl::DestroyContext(dpy, ctx);
}

MOBILEGL_EGL_API EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    MGLOG_D("eglDestroySurface(dpy=%p, surface=%p)", dpy, surface);
    return MobileGL::MG_Impl::EGLImpl::DestroySurface(dpy, surface);
}

MOBILEGL_EGL_API EGLBoolean eglTerminate(EGLDisplay dpy) {
    MGLOG_D("eglTerminate(dpy=%p)", dpy);
    return MobileGL::MG_Impl::EGLImpl::Terminate(dpy);
}

MOBILEGL_EGL_API EGLBoolean eglReleaseThread(void) {
    MGLOG_D("eglReleaseThread()");
    return MobileGL::MG_Impl::EGLImpl::ReleaseThread();
}

MOBILEGL_EGL_API EGLContext eglGetCurrentContext(void) {
    MGLOG_D("eglGetCurrentContext()");
    return MobileGL::MG_Impl::EGLImpl::GetCurrentContext();
}

MOBILEGL_EGL_API EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
    MGLOG_D("eglGetConfigAttrib(dpy=%p, config=%p, attribute=%d, value=%p)", dpy, config, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::GetConfigAttrib(dpy, config, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglBindAPI(EGLenum api) {
    MGLOG_D("eglBindAPI(api=%u)", api);
    return MobileGL::MG_Impl::EGLImpl::BindAPI(api);
}

MOBILEGL_EGL_API EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    MGLOG_D("eglGetCurrentSurface(readdraw=%d)", readdraw);
    return MobileGL::MG_Impl::EGLImpl::GetCurrentSurface(readdraw);
}

MOBILEGL_EGL_API EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value) {
    MGLOG_D("eglQuerySurface(display=%p, surface=%p, attribute=%d, value=%p)", display, surface, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::QuerySurface(display, surface, attribute, value);
}

MOBILEGL_EGL_API char const* eglQueryString(EGLDisplay display, EGLint name) {
    MGLOG_D("eglQueryString(display=%p, name=%d)", display, name);
    return MobileGL::MG_Impl::EGLImpl::QueryString(display, name);
}

MOBILEGL_EGL_API EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    MGLOG_D("eglSwapInterval(dpy=%p, interval=%d)", dpy, interval);
    return MobileGL::MG_Impl::EGLImpl::SwapInterval(dpy, interval);
}

MOBILEGL_EGL_API EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface draw) {
    MGLOG_D("eglSwapBuffers(dpy=%p, draw=%p)", dpy, draw);
#ifdef TRACY_ENABLE
    FrameMark;
#endif
    return MobileGL::MG_Impl::EGLImpl::SwapBuffers(dpy, draw);
}

MOBILEGL_EGL_API EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
    MGLOG_D("eglCreatePbufferSurface(dpy=%p, config=%p, attrib_list=%p)", dpy, config, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePbufferSurface(dpy, config, attrib_list);
}

MOBILEGL_EGL_API __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    MGLOG_D("eglGetProcAddress(name=%s)", name ? name : "null");
    return MobileGL::MG_Impl::EGLImpl::GetProcAddress(name);
}

MOBILEGL_EGL_API EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    MGLOG_D("eglBindTexImage(dpy=%p, surface=%p, buffer=%d)", dpy, surface, buffer);
    return MobileGL::MG_Impl::EGLImpl::BindTexImage(dpy, surface, buffer);
}

MOBILEGL_EGL_API EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    MGLOG_D("eglReleaseTexImage(dpy=%p, surface=%p, buffer=%d)", dpy, surface, buffer);
    return MobileGL::MG_Impl::EGLImpl::ReleaseTexImage(dpy, surface, buffer);
}

MOBILEGL_EGL_API EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
    MGLOG_D("eglCopyBuffers(dpy=%p, surface=%p, target=%p)", dpy, surface, target);
    return MobileGL::MG_Impl::EGLImpl::CopyBuffers(dpy, surface, target);
}

MOBILEGL_EGL_API EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                                                             EGLConfig config, const EGLint* attrib_list) {
    MGLOG_D("eglCreatePbufferFromClientBuffer(dpy=%p, buftype=%u, buffer=%p, config=%p, attrib_list=%p)", dpy, buftype,
            buffer, config, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePbufferFromClientBuffer(dpy, buftype, buffer, config, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                                   const EGLint* attrib_list) {
    MGLOG_D("eglCreatePixmapSurface(dpy=%p, config=%p, pixmap=%p, attrib_list=%p)", dpy, config, pixmap, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePixmapSurface(dpy, config, pixmap, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
    MGLOG_D("eglGetConfigs(dpy=%p, configs=%p, config_size=%d, num_config=%p)", dpy, configs, config_size, num_config);
    return MobileGL::MG_Impl::EGLImpl::GetConfigs(dpy, configs, config_size, num_config);
}

MOBILEGL_EGL_API EGLDisplay eglGetCurrentDisplay(void) {
    MGLOG_D("eglGetCurrentDisplay()");
    return MobileGL::MG_Impl::EGLImpl::GetCurrentDisplay();
}

MOBILEGL_EGL_API EGLenum eglQueryAPI(void) {
    MGLOG_D("eglQueryAPI()");
    return MobileGL::MG_Impl::EGLImpl::QueryAPI();
}

MOBILEGL_EGL_API EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
    MGLOG_D("eglQueryContext(dpy=%p, ctx=%p, attribute=%d, value=%p)", dpy, ctx, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::QueryContext(dpy, ctx, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
    MGLOG_D("eglSurfaceAttrib(dpy=%p, surface=%p, attribute=%d, value=%d)", dpy, surface, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::SurfaceAttrib(dpy, surface, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglWaitClient(void) {
    MGLOG_D("eglWaitClient()");
    return MobileGL::MG_Impl::EGLImpl::WaitClient();
}

MOBILEGL_EGL_API EGLBoolean eglWaitGL(void) {
    MGLOG_D("eglWaitGL()");
    return MobileGL::MG_Impl::EGLImpl::WaitGL();
}

MOBILEGL_EGL_API EGLBoolean eglWaitNative(EGLint engine) {
    MGLOG_D("eglWaitNative(engine=%d)", engine);
    return MobileGL::MG_Impl::EGLImpl::WaitNative(engine);
}

MOBILEGL_EGL_API EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list) {
    MGLOG_D("eglCreateSync(dpy=%p, type=%u, attrib_list=%p)", dpy, type, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateSync(dpy, type, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync) {
    MGLOG_D("eglDestroySync(dpy=%p, sync=%p)", dpy, sync);
    return MobileGL::MG_Impl::EGLImpl::DestroySync(dpy, sync);
}

MOBILEGL_EGL_API EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
    MGLOG_D("eglClientWaitSync(dpy=%p, sync=%p, flags=%d, timeout=%llu)", dpy, sync, flags,
            static_cast<unsigned long long>(timeout));
    return MobileGL::MG_Impl::EGLImpl::ClientWaitSync(dpy, sync, flags, timeout);
}

MOBILEGL_EGL_API EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
    MGLOG_D("eglGetSyncAttrib(dpy=%p, sync=%p, attribute=%d, value=%p)", dpy, sync, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::GetSyncAttrib(dpy, sync, attribute, value);
}

MOBILEGL_EGL_API EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                         const EGLAttrib* attrib_list) {
    MGLOG_D("eglCreateImage(dpy=%p, ctx=%p, target=%u, buffer=%p, attrib_list=%p)", dpy, ctx, target, buffer,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateImage(dpy, ctx, target, buffer, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image) {
    MGLOG_D("eglDestroyImage(dpy=%p, image=%p)", dpy, image);
    return MobileGL::MG_Impl::EGLImpl::DestroyImage(dpy, image);
}

MOBILEGL_EGL_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native_display,
                                                  const EGLAttrib* attrib_list) {
    MGLOG_D("eglGetPlatformDisplay(platform=%u, native_display=%p, attrib_list=%p)", platform, native_display,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::GetPlatformDisplay(platform, native_display, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                           const EGLAttrib* attrib_list) {
    MGLOG_D("eglCreatePlatformWindowSurface(dpy=%p, config=%p, native_window=%p, attrib_list=%p)", dpy, config,
            native_window, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                           const EGLAttrib* attrib_list) {
    MGLOG_D("eglCreatePlatformPixmapSurface(dpy=%p, config=%p, native_pixmap=%p, attrib_list=%p)", dpy, config,
            native_pixmap, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePlatformPixmapSurface(dpy, config, native_pixmap, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
    MGLOG_D("eglWaitSync(dpy=%p, sync=%p, flags=%d)", dpy, sync, flags);
    return MobileGL::MG_Impl::EGLImpl::WaitSync(dpy, sync, flags);
}
