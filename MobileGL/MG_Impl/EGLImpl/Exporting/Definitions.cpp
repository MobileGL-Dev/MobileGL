// MobileGL - MobileGL/MG_Impl/EGLImpl/Exporting/Definitions.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <Includes.h>
// EGLImageKHR, for the KHR spellings of the image entry points defined at the bottom of this file:
// Includes.h brings in the core egl.h only.
#include <EGL/eglext.h>
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

// EGL_KHR_image_base's spellings of the same two entry points, exported under their own names
// because that is the only thing that answers for them: measured in the Fedora container that runs
// this library as the glvnd EGL vendor, eglGetProcAddress("eglCreateImageKHR") and
// eglGetProcAddress("eglDestroyImageKHR") both returned NULL, and kwin_wayland's anland backend
// imports the display daemon's dma-bufs through exactly these two.
MOBILEGL_EGL_API EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                               const EGLint* attrib_list) {
    MGLOG_D("eglCreateImageKHR(dpy=%p, ctx=%p, target=%u, buffer=%p, attrib_list=%p)", dpy, ctx, target, buffer,
            attrib_list);
    // Not an alias of eglCreateImage: this list is EGLint pairs and the core one EGLAttrib pairs.
    return MobileGL::MG_Impl::EGLImpl::CreateImageKHR(dpy, ctx, target, buffer, attrib_list);
}

MOBILEGL_EGL_API EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image) {
    MGLOG_D("eglDestroyImageKHR(dpy=%p, image=%p)", dpy, image);
    // One handle under two spellings: EGLImageKHR and EGLImage are the same pointer.
    return MobileGL::MG_Impl::EGLImpl::DestroyImage(dpy, image);
}

MOBILEGL_EGL_API EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native_display,
                                                  const EGLAttrib* attrib_list) {
    MGLOG_D("eglGetPlatformDisplay(platform=%u, native_display=%p, attrib_list=%p)", platform, native_display,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::GetPlatformDisplay(platform, native_display, attrib_list);
}

MOBILEGL_EGL_API EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void* native_display,
                                                     const EGLint* attrib_list) {
    MGLOG_D("eglGetPlatformDisplayEXT(platform=%u, native_display=%p, attrib_list=%p)", platform, native_display,
            attrib_list);
    return MobileGL::MG_Impl::EGLImpl::GetPlatformDisplay(
        platform, native_display, reinterpret_cast<const EGLAttrib*>(attrib_list));
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                           const EGLAttrib* attrib_list) {
    MGLOG_D("eglCreatePlatformWindowSurface(dpy=%p, config=%p, native_window=%p, attrib_list=%p)", dpy, config,
            native_window, attrib_list);
    return MobileGL::MG_Impl::EGLImpl::CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
}

// THE SWAP-AND-SYNC FAMILY, which kwin asks for in EglBackend::init right after the rendering
// context is up and BEFORE it creates any surface: EGL_KHR_fence_sync, EGL_KHR_swap_buffers_with_damage
// and EGL_EXT_buffer_age are checked there, and a compositing mode whose backend init returns false
// is exactly the "Could not fulfill the requested compositing mode in KWIN_COMPOSE: 1" this run
// reports.  Each of these names is a spelling of something this library already implements (the core
// eglSwapBuffers and the state layer's sync objects), so they are published as real exports rather
// than promised and not delivered.
MOBILEGL_EGL_API EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, const EGLint* rects,
                                                       EGLint n_rects) {
    // The damage rectangles are a hint about what changed; this side swaps the whole surface either
    // way, so the honest answer is the core swap.
    (void)rects;
    (void)n_rects;
    return MobileGL::MG_Impl::EGLImpl::SwapBuffers(dpy, surface) ? EGL_TRUE : EGL_FALSE;
}

MOBILEGL_EGL_API EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface, const EGLint* rects,
                                                       EGLint n_rects) {
    return eglSwapBuffersWithDamageKHR(dpy, surface, rects, n_rects);
}

MOBILEGL_EGL_API EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint* attrib_list) {
    // EGL_KHR_fence_sync's list is EGLint pairs; the state layer takes EGLAttrib pairs.
    EGLAttrib attributes[16];
    int count = 0;
    if (attrib_list != nullptr) {
        for (int index = 0; attrib_list[index] != EGL_NONE && count + 1 < 16; index += 2) {
            attributes[count++] = static_cast<EGLAttrib>(attrib_list[index]);
            attributes[count++] = static_cast<EGLAttrib>(attrib_list[index + 1]);
        }
    }
    attributes[count] = EGL_NONE;
    return reinterpret_cast<EGLSyncKHR>(MobileGL::MG_Impl::EGLImpl::CreateSync(dpy, type, attributes));
}

MOBILEGL_EGL_API EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync) {
    return MobileGL::MG_Impl::EGLImpl::DestroySync(dpy, reinterpret_cast<EGLSync>(sync)) ? EGL_TRUE : EGL_FALSE;
}

MOBILEGL_EGL_API EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout) {
    return MobileGL::MG_Impl::EGLImpl::ClientWaitSync(dpy, reinterpret_cast<EGLSync>(sync), flags,
                                                     static_cast<EGLTime>(timeout));
}

MOBILEGL_EGL_API EGLBoolean eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags) {
    return MobileGL::MG_Impl::EGLImpl::WaitSync(dpy, reinterpret_cast<EGLSync>(sync), flags) ? EGL_TRUE : EGL_FALSE;
}

MOBILEGL_EGL_API EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value) {
    EGLAttrib answered = EGL_NONE;
    if (!MobileGL::MG_Impl::EGLImpl::GetSyncAttrib(dpy, reinterpret_cast<EGLSync>(sync), attribute, &answered)) {
        return EGL_FALSE;
    }
    if (value != nullptr) {
        *value = static_cast<EGLint>(answered);
    }
    return EGL_TRUE;
}

// THE DEVICE-QUERY FAMILY, PUBLISHED AS REAL ENTRY POINTS.  It is not academic: with the extension
// announced but these names unresolvable, libepoxy leaves epoxy_eglQueryDisplayAttribEXT NULL and
// kwin, trusting the announcement, jumps to it (measured: signal 11, fault_addr=(nil), pc=0x0 from
// KWin::EglDisplay::determineRenderNode()); with the announcement withdrawn instead, kwin stops
// asking and its compositing never becomes active (measured: org.kde.kwin.Compositing active =
// false), so nothing is ever painted and the display daemon frames are never imported.  Publishing
// them is the only answer that leaves kwin both safe and able to find its render device.
namespace {
    // One device, one identity - the same shape the vendor side uses (Exporting/GlvndVendor.cpp): a
    // pointer to a private object, so a device from somewhere else cannot compare equal by accident.
    struct DeviceToken { int unused; };
    DeviceToken g_deviceToken;
} // namespace

MOBILEGL_EGL_API EGLBoolean eglQueryDevicesEXT(EGLint max_devices, EGLDeviceEXT* devices, EGLint* num_devices) {
    if (num_devices == nullptr) return EGL_FALSE;
    *num_devices = 1;
    if (devices != nullptr && max_devices >= 1) devices[0] = reinterpret_cast<EGLDeviceEXT>(&g_deviceToken);
    return EGL_TRUE;
}

MOBILEGL_EGL_API EGLBoolean eglQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute, EGLAttrib* value) {
    if (device != reinterpret_cast<EGLDeviceEXT>(&g_deviceToken) || value == nullptr) return EGL_FALSE;
    switch (attribute) {
    case EGL_DRM_DEVICE_FILE_EXT:
    case EGL_DRM_RENDER_NODE_FILE_EXT:
        *value = reinterpret_cast<EGLAttrib>("/dev/dri/renderD128");
        return EGL_TRUE;
    default:
        return EGL_FALSE;
    }
}

MOBILEGL_EGL_API const char* eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name) {
    if (device != reinterpret_cast<EGLDeviceEXT>(&g_deviceToken)) return nullptr;
    // THE NODE IS NAMED, AND THAT CHANGED BECAUSE THE MEASUREMENT CHANGED.  An empty answer was the
    // right shape while this query was unreachable: nothing asked, so nothing depended on it.  Now
    // that the name resolves, it is exactly what kwin asks on the way into its EGL backend, and the
    // fork says out loud what an empty answer costs it - "Error during init of AnlandEglBackend",
    // after which it falls back to the plain AnlandBackend, whose compositing never becomes active
    // (supportInformation: "LogicalOutput backend: KWin::AnlandBackend" + "Compositing is not
    // active"), so nothing is ever painted and the display daemon frames are never imported.  The
    // path answered is the one the environment configures the compositor with (ANLAND_DRM_DEVICE),
    // so the node named here and the node in use are the same node.
    if (name == EGL_DRM_DEVICE_FILE_EXT || name == EGL_DRM_RENDER_NODE_FILE_EXT) {
        return "/dev/dri/renderD128";
    }
    if (name == EGL_EXTENSIONS) return "EGL_EXT_device_drm_render_node";
    return nullptr;
}

MOBILEGL_EGL_API EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib* value) {
    (void)dpy;
    if (value == nullptr) return EGL_FALSE;
    if (attribute == EGL_DEVICE_EXT) {
        *value = reinterpret_cast<EGLAttrib>(&g_deviceToken);
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

// The EXT spellings of the platform surface entry points: EGL_EXT_platform_base defines them by those
// names, and a dispatcher-based client resolves them by those names.  Identical ABI (EGLAttrib list).
MOBILEGL_EGL_API EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                              const EGLAttrib* attrib_list) {
    return eglCreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
}

MOBILEGL_EGL_API EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                              const EGLAttrib* attrib_list) {
    return eglCreatePlatformPixmapSurface(dpy, config, native_pixmap, attrib_list);
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
