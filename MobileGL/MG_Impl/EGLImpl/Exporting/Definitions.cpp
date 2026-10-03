// MobileGL - MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>
#include "../EGLImpl.h"

#include <vector>

namespace {
    // The KHR/EXT spellings take EGLint attribute lists where the core 1.5 entry points take
    // EGLAttrib (pointer-sized). Reinterpreting one as the other reads two EGLints as one
    // attribute on 64-bit, so the list is widened instead.
    std::vector<EGLAttrib> WidenAttribList(const EGLint* attribs) {
        std::vector<EGLAttrib> widened;
        if (attribs == nullptr) return widened;
        for (; *attribs != EGL_NONE; attribs += 2) {
            widened.push_back(static_cast<EGLAttrib>(attribs[0]));
            widened.push_back(static_cast<EGLAttrib>(attribs[1]));
        }
        widened.push_back(EGL_NONE);
        return widened;
    }
} // namespace

MOBILEGL_EGL_API EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                                   const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateWindowSurface(dpy=%p, config=%p, window=%p, attrib_list=%p)", dpy, config, window, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateWindowSurface(dpy, config, window, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs,
                                            EGLint config_size, EGLint* num_config) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglChooseConfig(dpy=%p, attrib_list=%p, configs=%p, config_size=%d, num_config=%p)", dpy, attrib_list,
            configs, config_size, num_config);
    return MobileGL::MG_Impl::EGLImpl::ChooseConfig(dpy, attrib_list, configs, config_size, num_config);
}

MOBILEGL_EGL_API EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx,
                                             const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateContext(dpy=%p, config=%p, shareCtx=%p, attrib_list=%p)", dpy, config, shareCtx, attrib_list);
#ifdef TRACY_ENABLE
    tracy::StartupProfiler();
#endif
    return MobileGL::MG_Impl::EGLImpl::CreateContext(dpy, config, shareCtx, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglInitialize(dpy=%p, major=%p, minor=%p)", dpy, major, minor);
    return MobileGL::MG_Impl::EGLImpl::Initialize(dpy, major, minor);
}

MOBILEGL_EGL_API EGLDisplay eglGetDisplay(NativeDisplayType display) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetDisplay(display=%p)", display);
    return MobileGL::MG_Impl::EGLImpl::GetDisplay(display);
}

MOBILEGL_EGL_API EGLint eglGetError() {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetError()");
    return MobileGL::MG_Impl::EGLImpl::GetError();
}

MOBILEGL_EGL_API EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglMakeCurrent(dpy=%p, draw=%p, read=%p, ctx=%p)", dpy, draw, read, ctx);
    return MobileGL::MG_Impl::EGLImpl::MakeCurrent(dpy, draw, read, ctx);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroyContext(dpy=%p, ctx=%p)", dpy, ctx);
#ifdef TRACY_ENABLE
    tracy::ShutdownProfiler();
#endif
    return MobileGL::MG_Impl::EGLImpl::DestroyContext(dpy, ctx);
}

MOBILEGL_EGL_API EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroySurface(dpy=%p, surface=%p)", dpy, surface);
    return MobileGL::MG_Impl::EGLImpl::DestroySurface(dpy, surface);
}

MOBILEGL_EGL_API EGLBoolean eglTerminate(EGLDisplay dpy) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglTerminate(dpy=%p)", dpy);
    return MobileGL::MG_Impl::EGLImpl::Terminate(dpy);
}

MOBILEGL_EGL_API EGLBoolean eglReleaseThread(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglReleaseThread()");
    return MobileGL::MG_Impl::EGLImpl::ReleaseThread();
}

MOBILEGL_EGL_API EGLContext eglGetCurrentContext(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetCurrentContext()");
    return MobileGL::MG_Impl::EGLImpl::GetCurrentContext();
}

MOBILEGL_EGL_API EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetConfigAttrib(dpy=%p, config=%p, attribute=%d, value=%p)", dpy, config, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::GetConfigAttrib(dpy, config, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglBindAPI(EGLenum api) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglBindAPI(api=%u)", api);
    return MobileGL::MG_Impl::EGLImpl::BindAPI(api);
}

MOBILEGL_EGL_API EGLSurface eglGetCurrentSurface(EGLint readdraw) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetCurrentSurface(readdraw=%d)", readdraw);
    return MobileGL::MG_Impl::EGLImpl::GetCurrentSurface(readdraw);
}

MOBILEGL_EGL_API EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQuerySurface(display=%p, surface=%p, attribute=%d, value=%p)", display, surface, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::QuerySurface(display, surface, attribute, value);
}

MOBILEGL_EGL_API char const* eglQueryString(EGLDisplay display, EGLint name) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQueryString(display=%p, name=%d)", display, name);
    return MobileGL::MG_Impl::EGLImpl::QueryString(display, name);
}

MOBILEGL_EGL_API EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSwapInterval(dpy=%p, interval=%d)", dpy, interval);
    return MobileGL::MG_Impl::EGLImpl::SwapInterval(dpy, interval);
}

MOBILEGL_EGL_API EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface draw) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSwapBuffers(dpy=%p, draw=%p)", dpy, draw);
#ifdef TRACY_ENABLE
    FrameMark;
#endif
    return MobileGL::MG_Impl::EGLImpl::SwapBuffers(dpy, draw);
}

// EGL_KHR_swap_buffers_with_damage and EGL_EXT_swap_buffers_with_damage: one function, two names.
MOBILEGL_EGL_API EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface draw, const EGLint* rects,
                                                        EGLint n_rects) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSwapBuffersWithDamageKHR(dpy=%p, draw=%p, n_rects=%d)", dpy, draw, n_rects);
#ifdef TRACY_ENABLE
    FrameMark;
#endif
    return MobileGL::MG_Impl::EGLImpl::SwapBuffersWithDamage(dpy, draw, rects, n_rects);
}

MOBILEGL_EGL_API EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface draw, const EGLint* rects,
                                                        EGLint n_rects) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSwapBuffersWithDamageEXT(dpy=%p, draw=%p, n_rects=%d)", dpy, draw, n_rects);
#ifdef TRACY_ENABLE
    FrameMark;
#endif
    return MobileGL::MG_Impl::EGLImpl::SwapBuffersWithDamage(dpy, draw, rects, n_rects);
}

// EGL_KHR_partial_update.
MOBILEGL_EGL_API EGLBoolean eglSetDamageRegionKHR(EGLDisplay dpy, EGLSurface surface, EGLint* rects, EGLint n_rects) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSetDamageRegionKHR(dpy=%p, surface=%p, n_rects=%d)", dpy, surface, n_rects);
    return MobileGL::MG_Impl::EGLImpl::SetDamageRegion(dpy, surface, rects, n_rects);
}

MOBILEGL_EGL_API EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreatePbufferSurface(dpy=%p, config=%p, attrib_list=%p)", dpy, config, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePbufferSurface(dpy, config, attrib_list);
}

MOBILEGL_EGL_API __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetProcAddress(name=%s)", name ? name : "null");
    return MobileGL::MG_Impl::EGLImpl::GetProcAddress(name);
}

MOBILEGL_EGL_API EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglBindTexImage(dpy=%p, surface=%p, buffer=%d)", dpy, surface, buffer);
    return MobileGL::MG_Impl::EGLImpl::BindTexImage(dpy, surface, buffer);
}

MOBILEGL_EGL_API EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglReleaseTexImage(dpy=%p, surface=%p, buffer=%d)", dpy, surface, buffer);
    return MobileGL::MG_Impl::EGLImpl::ReleaseTexImage(dpy, surface, buffer);
}

MOBILEGL_EGL_API EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCopyBuffers(dpy=%p, surface=%p, target=%p)", dpy, surface, target);
    return MobileGL::MG_Impl::EGLImpl::CopyBuffers(dpy, surface, target);
}

MOBILEGL_EGL_API EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer,
                                                             EGLConfig config, const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreatePbufferFromClientBuffer(dpy=%p, buftype=%u, buffer=%p, config=%p, attrib_list=%p)", dpy, buftype,
            buffer, config, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePbufferFromClientBuffer(dpy, buftype, buffer, config, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                                   const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreatePixmapSurface(dpy=%p, config=%p, pixmap=%p, attrib_list=%p)", dpy, config, pixmap, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePixmapSurface(dpy, config, pixmap, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetConfigs(dpy=%p, configs=%p, config_size=%d, num_config=%p)", dpy, configs, config_size, num_config);
    return MobileGL::MG_Impl::EGLImpl::GetConfigs(dpy, configs, config_size, num_config);
}

MOBILEGL_EGL_API EGLDisplay eglGetCurrentDisplay(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetCurrentDisplay()");
    return MobileGL::MG_Impl::EGLImpl::GetCurrentDisplay();
}

MOBILEGL_EGL_API EGLenum eglQueryAPI(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQueryAPI()");
    return MobileGL::MG_Impl::EGLImpl::QueryAPI();
}

MOBILEGL_EGL_API EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQueryContext(dpy=%p, ctx=%p, attribute=%d, value=%p)", dpy, ctx, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::QueryContext(dpy, ctx, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglSurfaceAttrib(dpy=%p, surface=%p, attribute=%d, value=%d)", dpy, surface, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::SurfaceAttrib(dpy, surface, attribute, value);
}

MOBILEGL_EGL_API EGLBoolean eglWaitClient(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglWaitClient()");
    return MobileGL::MG_Impl::EGLImpl::WaitClient();
}

MOBILEGL_EGL_API EGLBoolean eglWaitGL(void) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglWaitGL()");
    return MobileGL::MG_Impl::EGLImpl::WaitGL();
}

MOBILEGL_EGL_API EGLBoolean eglWaitNative(EGLint engine) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglWaitNative(engine=%d)", engine);
    return MobileGL::MG_Impl::EGLImpl::WaitNative(engine);
}

MOBILEGL_EGL_API EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateSync(dpy=%p, type=%u, attrib_list=%p)", dpy, type, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateSync(dpy, type, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroySync(dpy=%p, sync=%p)", dpy, sync);
    return MobileGL::MG_Impl::EGLImpl::DestroySync(dpy, sync);
}

// The waits take no stream lock: they read EGL state only (its own lock), never the stream, and a
// native fence's wait blocks for as long as the fenced work runs - with the lock held, every other
// thread's GL call would wait with it.
MOBILEGL_EGL_API EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
    MGLOG_D("eglClientWaitSync(dpy=%p, sync=%p, flags=%d, timeout=%llu)", dpy, sync, flags,
            static_cast<unsigned long long>(timeout));
    return MobileGL::MG_Impl::EGLImpl::ClientWaitSync(dpy, sync, flags, timeout);
}

MOBILEGL_EGL_API EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetSyncAttrib(dpy=%p, sync=%p, attribute=%d, value=%p)", dpy, sync, attribute, value);
    return MobileGL::MG_Impl::EGLImpl::GetSyncAttrib(dpy, sync, attribute, value);
}

// EGL_KHR_fence_sync / EGL_KHR_wait_sync / EGL_ANDROID_native_fence_sync's spellings, which
// ANGLE's GLES-on-EGL backend loads by name (and only through them offers fences to Chrome): the
// same objects as EGL 1.5's, with EGLint attribute lists.
namespace {
    // EGL_KHR_fence_sync's attribute list widened to EGL 1.5's; at most 16 pairs, as no sync type
    // takes more than two attributes.
    bool WidenSyncAttribs(const EGLint* in, EGLAttrib* out, int capacity) {
        int n = 0;
        for (const EGLint* at = in; at != nullptr && at[0] != EGL_NONE; at += 2) {
            if (n + 3 > capacity) return false;
            out[n++] = static_cast<EGLAttrib>(at[0]);
            out[n++] = static_cast<EGLAttrib>(at[1]);
        }
        out[n] = EGL_NONE;
        return true;
    }
} // namespace

MOBILEGL_EGL_API EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateSyncKHR(dpy=%p, type=%u, attrib_list=%p)", dpy, type, attrib_list);
    EGLAttrib attribs[33];
    if (!WidenSyncAttribs(attrib_list, attribs, 33)) return EGL_NO_SYNC_KHR;
    return MobileGL::MG_Impl::EGLImpl::CreateSync(dpy, type, attribs);
}

MOBILEGL_EGL_API EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroySyncKHR(dpy=%p, sync=%p)", dpy, sync);
    return MobileGL::MG_Impl::EGLImpl::DestroySync(dpy, sync);
}

MOBILEGL_EGL_API EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
    MGLOG_D("eglClientWaitSyncKHR(dpy=%p, sync=%p, flags=%d, timeout=%llu)", dpy, sync, flags,
            static_cast<unsigned long long>(timeout));
    return MobileGL::MG_Impl::EGLImpl::ClientWaitSync(dpy, sync, flags, timeout);
}

MOBILEGL_EGL_API EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetSyncAttribKHR(dpy=%p, sync=%p, attribute=%d, value=%p)", dpy, sync, attribute, value);
    if (value == nullptr) return MobileGL::MG_Impl::EGLImpl::GetSyncAttrib(dpy, sync, attribute, nullptr);
    EGLAttrib wide = 0;
    const EGLBoolean ok = MobileGL::MG_Impl::EGLImpl::GetSyncAttrib(dpy, sync, attribute, &wide);
    if (ok == EGL_TRUE) *value = static_cast<EGLint>(wide);
    return ok;
}

MOBILEGL_EGL_API EGLint eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) {
    MGLOG_D("eglWaitSyncKHR(dpy=%p, sync=%p, flags=%d)", dpy, sync, flags);
    return MobileGL::MG_Impl::EGLImpl::WaitSync(dpy, sync, flags) == EGL_TRUE ? EGL_TRUE : EGL_FALSE;
}

MOBILEGL_EGL_API EGLint eglDupNativeFenceFDANDROID(EGLDisplay dpy, EGLSyncKHR sync) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDupNativeFenceFDANDROID(dpy=%p, sync=%p)", dpy, sync);
    return MobileGL::MG_Impl::EGLImpl::DupNativeFenceFD(dpy, sync);
}

MOBILEGL_EGL_API EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                         const EGLAttrib* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateImage(dpy=%p, ctx=%p, target=%u, buffer=%p, attrib_list=%p)", dpy, ctx, target, buffer,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateImage(dpy, ctx, target, buffer, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage image) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroyImage(dpy=%p, image=%p)", dpy, image);
    return MobileGL::MG_Impl::EGLImpl::DestroyImage(dpy, image);
}

// EGL_KHR_image_base's spellings. KWin loads these two by name and calls eglDestroyImageKHR
// for every cached client buffer when it dies, wl_shm ones included; a null answer from
// eglGetProcAddress made that a jump to address 0.
MOBILEGL_EGL_API EGLImage eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                               const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreateImageKHR(dpy=%p, ctx=%p, target=%u, buffer=%p, attrib_list=%p)", dpy, ctx, target, buffer,
            attrib_list);
    const std::vector<EGLAttrib> attribs = WidenAttribList(attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreateImage(dpy, ctx, target, buffer, attrib_list ? attribs.data() : nullptr);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImage image) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglDestroyImageKHR(dpy=%p, image=%p)", dpy, image);
    return MobileGL::MG_Impl::EGLImpl::DestroyImage(dpy, image);
}

// EGL_EXT_image_dma_buf_import_modifiers: the formats eglCreateImage(EGL_LINUX_DMA_BUF_EXT) takes,
// and (none) explicit modifiers - a compositor then imports with the implicit one.
MOBILEGL_EGL_API EGLBoolean eglQueryDmaBufFormatsEXT(EGLDisplay dpy, EGLint max_formats, EGLint* formats,
                                                     EGLint* num_formats) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQueryDmaBufFormatsEXT(dpy=%p, max_formats=%d)", dpy, max_formats);
    return MobileGL::MG_Impl::EGLImpl::QueryDmaBufFormats(dpy, max_formats, formats, num_formats);
}

MOBILEGL_EGL_API EGLBoolean eglQueryDmaBufModifiersEXT(EGLDisplay dpy, EGLint format, EGLint max_modifiers,
                                                       EGLuint64KHR* modifiers, EGLBoolean* external_only,
                                                       EGLint* num_modifiers) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglQueryDmaBufModifiersEXT(dpy=%p, format=0x%08x, max_modifiers=%d)", dpy, format, max_modifiers);
    return MobileGL::MG_Impl::EGLImpl::QueryDmaBufModifiers(dpy, format, max_modifiers, modifiers, external_only,
                                                            num_modifiers);
}

MOBILEGL_EGL_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native_display,
                                                  const EGLAttrib* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetPlatformDisplay(platform=%u, native_display=%p, attrib_list=%p)", platform, native_display,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::GetPlatformDisplay(platform, native_display, attrib_list);
}

MOBILEGL_EGL_API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void* native_display,
                                                     const EGLint* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglGetPlatformDisplayEXT(platform=%u, native_display=%p, attrib_list=%p)", platform, native_display,
            attrib_list);
    const std::vector<EGLAttrib> attribs = WidenAttribList(attrib_list);
    return MobileGL::MG_Impl::EGLImpl::GetPlatformDisplay(platform, native_display,
                                                          attrib_list ? attribs.data() : nullptr);
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                           const EGLAttrib* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreatePlatformWindowSurface(dpy=%p, config=%p, native_window=%p, attrib_list=%p)", dpy, config,
            native_window, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                           const EGLAttrib* attrib_list) {
    const MobileGL::MG_Impl::EGLImpl::StreamLockScope mglStreamLock;
    MGLOG_D("eglCreatePlatformPixmapSurface(dpy=%p, config=%p, native_pixmap=%p, attrib_list=%p)", dpy, config,
            native_pixmap, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePlatformPixmapSurface(dpy, config, native_pixmap, attrib_list);
}

// No stream lock, like eglClientWaitSync's: a native fence's server wait blocks the calling thread.
MOBILEGL_EGL_API EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
    MGLOG_D("eglWaitSync(dpy=%p, sync=%p, flags=%d)", dpy, sync, flags);
    return MobileGL::MG_Impl::EGLImpl::WaitSync(dpy, sync, flags);
}
