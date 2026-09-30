// MobileGL - MobileGL/MG_Impl/EGLImpl/Exporting/GlvndVendor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MOBILEGL AS A GLVND EGL VENDOR.
//
// On a system with libglvnd, the libEGL.so.1 an application links is not an implementation at all:
// it is a dispatcher, and the implementations are vendor libraries it loads from
// /usr/share/glvnd/egl_vendor.d (or from whatever __EGL_VENDOR_LIBRARY_FILENAMES names) and routes
// every EGL call to.  That is the supported way to be the implementation an application actually
// talks to, and on this machine it is the ONLY way that reaches a caller which does not link EGL
// directly: kwin goes through libepoxy, which dlopen()s libEGL.so.1 and dlsyms its entry points at
// startup, so a library injected with LD_PRELOAD is invisible to it however loudly it is named.
//
// What glvnd asks a vendor for is the other half of that ABI, and this file is it: glvnd calls
// __egl_Main once, the library answers with the functions it wants dispatched to it, and - the part
// that decides whether a compositor can use it at all - with the string naming the PLATFORMS it
// serves.  A vendor that does not claim EGL_PLATFORM_GBM_KHR is never asked for a GBM display, and
// the caller that asked for one is told there is no usable device: the client sitting in its maps,
// loaded and never called, is exactly what that looks like from the outside.
//
// The entry points below are this library's own (MG_Impl::EGLImpl, the same functions
// Exporting/Definitions.cpp exports), so a vendor call and a direct call cannot drift apart.

#include <Includes.h>
#include <glvnd/libeglabi.h>

#include "../EGLImpl.h"
#include "../EGLPlatformExtensions.h"

namespace {
    using namespace MobileGL;

    const __EGLapiExports* g_eglExports = nullptr;

    // The platforms this vendor serves.  GBM is the one that matters here: the compositor asks for
    // a display on the GBM device it opened, and a vendor that does not name the platform is not
    // asked at all.  This side treats a GBM device as it treats every other native display - the
    // frames come from the display host, not from the device - so claiming the platform is honest
    // rather than optimistic.
    constexpr const char* kPlatformExtensions = MG_Impl::EGLImpl::kPlatformExtensionString;

    // glvnd asks for the client-only extension list separately from the platform one; the split is
    // its own invention, and this side answers both from the same query.
    const char* EGLAPIENTRY VendorQueryString(EGLDisplay dpy, EGLint name) {
        return MG_Impl::EGLImpl::QueryString(dpy, name);
    }

    EGLDisplay EGLAPIENTRY VendorGetPlatformDisplay(EGLenum platform, void* native_display,
                                                    const EGLAttrib* attrib_list) {
        // EGL_NONE means the application called eglGetDisplay(EGL_DEFAULT_DISPLAY): the vendor is
        // being asked to name its own default display.
        if (platform == EGL_NONE) {
            return MG_Impl::EGLImpl::GetDisplay(EGL_DEFAULT_DISPLAY);
        }
        return MG_Impl::EGLImpl::GetPlatformDisplay(platform, native_display, attrib_list);
    }

    EGLBoolean EGLAPIENTRY VendorGetSupportsAPI(EGLenum api) {
        // What this library answers for EGL_CLIENT_APIS, said as a question.
        return (api == EGL_OPENGL_ES_API || api == EGL_OPENGL_API) ? EGL_TRUE : EGL_FALSE;
    }

    const char* EGLAPIENTRY VendorGetVendorString(int name) {
        if (name == __EGL_VENDOR_STRING_PLATFORM_EXTENSIONS) {
            return kPlatformExtensions;
        }
        return nullptr;
    }

    // GLVND ASKS FOR EVERY EGL ENTRY POINT BY NAME, AND DROPS THE VENDOR IF ONE IS MISSING: the
    // first eight functions the table below names are looked up before this library is ever asked
    // for a display, and a single null answer means glvnd quietly stops considering it - the
    // library is loaded, __egl_Main has run, and nothing is ever dispatched to it.
    struct EntryPoint {
        const char* name;
        void* address;
    };

    // ---- the device family -------------------------------------------------------------------
    //
    // A compositor does not ask for a display and hope.  It asks which devices exist, reads the DRM
    // node off the one it wants, and only then asks for a display ON that device - so a vendor that
    // answers "no devices" is one it cannot use, however well the vendor draws.  The message it
    // gives is "no usable DRM render device", which names where it stopped rather than why.
    //
    // This implementation therefore names one device: the render node of the machine it runs on.
    // That is an honest answer even though the frames do not come from that node - they come from
    // the display host on the other end of the socket, which is why every surface here is a host
    // frame - because the question is which device this display is associated with, and the answer
    // is what lets the compositor match its own node to the display it is about to draw into.
    const char* DrmRenderNode() {
        // MOBILEGL_DEVICE_DRM_NODE names another node: a machine with more than one GPU, or a test
        // that wants the mismatch refused rather than papered over.
        const char* configured = std::getenv("MOBILEGL_DEVICE_DRM_NODE");
        return (configured != nullptr && configured[0] != '\0') ? configured : "/dev/dri/renderD128";
    }

    // One device, one identity.  A pointer to a private object rather than a number, so that a
    // device from somewhere else cannot compare equal to this one by accident.
    struct DeviceToken { int unused; };
    DeviceToken g_deviceToken;

    EGLBoolean EGLAPIENTRY VendorQueryDevicesEXT(EGLint max_devices, EGLDeviceEXT* devices, EGLint* num_devices) {
        if (num_devices == nullptr) {
            return EGL_FALSE;
        }
        *num_devices = 1;
        if (devices != nullptr && max_devices >= 1) {
            devices[0] = reinterpret_cast<EGLDeviceEXT>(&g_deviceToken);
        }
        return EGL_TRUE;
    }

    EGLBoolean EGLAPIENTRY VendorQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute, EGLAttrib* value) {
        if (device != reinterpret_cast<EGLDeviceEXT>(&g_deviceToken) || value == nullptr) {
            return EGL_FALSE;
        }
        switch (attribute) {
        case EGL_DRM_DEVICE_FILE_EXT:
        case EGL_DRM_RENDER_NODE_FILE_EXT:
            *value = reinterpret_cast<EGLAttrib>(DrmRenderNode());
            return EGL_TRUE;
        default:
            return EGL_FALSE;
        }
    }

    const char* EGLAPIENTRY VendorQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name) {
        if (device != reinterpret_cast<EGLDeviceEXT>(&g_deviceToken)) {
            return nullptr;
        }
        // EGL_EXT_device_drm asks for the node as a STRING, not as an attribute, and that query is
        // the one a compositor actually makes before it opens anything - it is how the display it is
        // about to create is matched to a node it already holds.  Answering only EGL_EXTENSIONS here
        // leaves that lookup empty, and the failure surfaces one layer down as the node "not being
        // openable", which is the compositor's wording for having had nothing to open.
        if (name == EGL_DRM_DEVICE_FILE_EXT || name == EGL_DRM_RENDER_NODE_FILE_EXT) {
            return DrmRenderNode();
        }
        if (name == EGL_EXTENSIONS) {
            return "EGL_EXT_device_drm EGL_EXT_device_drm_render_node";
        }
        return nullptr;
    }

    EGLBoolean EGLAPIENTRY VendorQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib* value) {
        (void)dpy;
        if (value == nullptr) {
            return EGL_FALSE;
        }
        if (attribute == EGL_DEVICE_EXT) {
            *value = reinterpret_cast<EGLAttrib>(&g_deviceToken);
            return EGL_TRUE;
        }
        return EGL_FALSE;
    }

    // Three of the names glvnd insists on are debug extensions this implementation does not
    // provide, and glvnd insists on them all the same - a vendor that answers null for any of them
    // is dropped.  They are answered honestly rather than pretended: KHR_debug is not in the
    // extension strings above, so a caller that reads them will not call these at all.
    EGLint EGLAPIENTRY VendorDebugMessageControlKHR(EGLDEBUGPROCKHR callback, const EGLAttrib* attrib_list) {
        (void)callback;
        (void)attrib_list;
        return EGL_FALSE;
    }

    EGLBoolean EGLAPIENTRY VendorQueryDebugKHR(EGLint attribute, EGLAttrib* value) {
        (void)attribute;
        (void)value;
        return EGL_FALSE;
    }

    EGLint EGLAPIENTRY VendorLabelObjectKHR(EGLDisplay dpy, EGLenum objectType, EGLObjectKHR object, EGLLabelKHR label) {
        (void)dpy;
        (void)objectType;
        (void)object;
        (void)label;
        return EGL_BAD_PARAMETER;
    }

    const EntryPoint kEglEntryPoints[] = {
        // The core of EGL 1.5, every one of which glvnd looks up by name before it considers this
        // library usable.
        {"eglInitialize", reinterpret_cast<void*>(eglInitialize)},
        {"eglChooseConfig", reinterpret_cast<void*>(eglChooseConfig)},
        {"eglCopyBuffers", reinterpret_cast<void*>(eglCopyBuffers)},
        {"eglCreateContext", reinterpret_cast<void*>(eglCreateContext)},
        {"eglCreatePbufferSurface", reinterpret_cast<void*>(eglCreatePbufferSurface)},
        {"eglCreatePixmapSurface", reinterpret_cast<void*>(eglCreatePixmapSurface)},
        {"eglCreateWindowSurface", reinterpret_cast<void*>(eglCreateWindowSurface)},
        {"eglDestroyContext", reinterpret_cast<void*>(eglDestroyContext)},
        {"eglDestroySurface", reinterpret_cast<void*>(eglDestroySurface)},
        {"eglGetConfigAttrib", reinterpret_cast<void*>(eglGetConfigAttrib)},
        {"eglGetConfigs", reinterpret_cast<void*>(eglGetConfigs)},
        {"eglMakeCurrent", reinterpret_cast<void*>(eglMakeCurrent)},
        {"eglQueryContext", reinterpret_cast<void*>(eglQueryContext)},
        {"eglQuerySurface", reinterpret_cast<void*>(eglQuerySurface)},
        {"eglSwapBuffers", reinterpret_cast<void*>(eglSwapBuffers)},
        {"eglTerminate", reinterpret_cast<void*>(eglTerminate)},
        {"eglWaitGL", reinterpret_cast<void*>(eglWaitGL)},
        {"eglWaitNative", reinterpret_cast<void*>(eglWaitNative)},
        {"eglBindTexImage", reinterpret_cast<void*>(eglBindTexImage)},
        {"eglReleaseTexImage", reinterpret_cast<void*>(eglReleaseTexImage)},
        {"eglSurfaceAttrib", reinterpret_cast<void*>(eglSurfaceAttrib)},
        {"eglSwapInterval", reinterpret_cast<void*>(eglSwapInterval)},
        {"eglBindAPI", reinterpret_cast<void*>(eglBindAPI)},
        {"eglCreatePbufferFromClientBuffer", reinterpret_cast<void*>(eglCreatePbufferFromClientBuffer)},
        {"eglReleaseThread", reinterpret_cast<void*>(eglReleaseThread)},
        {"eglWaitClient", reinterpret_cast<void*>(eglWaitClient)},
        {"eglGetError", reinterpret_cast<void*>(eglGetError)},
        {"eglCreateSync", reinterpret_cast<void*>(eglCreateSync)},
        {"eglDestroySync", reinterpret_cast<void*>(eglDestroySync)},
        {"eglClientWaitSync", reinterpret_cast<void*>(eglClientWaitSync)},
        {"eglGetSyncAttrib", reinterpret_cast<void*>(eglGetSyncAttrib)},
        {"eglWaitSync", reinterpret_cast<void*>(eglWaitSync)},
        {"eglCreateImage", reinterpret_cast<void*>(eglCreateImage)},
        {"eglDestroyImage", reinterpret_cast<void*>(eglDestroyImage)},
        {"eglCreatePlatformWindowSurface", reinterpret_cast<void*>(eglCreatePlatformWindowSurface)},
        {"eglCreatePlatformPixmapSurface", reinterpret_cast<void*>(eglCreatePlatformPixmapSurface)},
        // The platform and current-state queries: not in glvnd's required list, but the names an
        // application that talks to a vendor through a dispatcher will ask for by name.
        {"eglGetDisplay", reinterpret_cast<void*>(eglGetDisplay)},
        {"eglGetCurrentDisplay", reinterpret_cast<void*>(eglGetCurrentDisplay)},
        {"eglGetCurrentSurface", reinterpret_cast<void*>(eglGetCurrentSurface)},
        {"eglGetCurrentContext", reinterpret_cast<void*>(eglGetCurrentContext)},
        {"eglQueryAPI", reinterpret_cast<void*>(eglQueryAPI)},
        {"eglGetProcAddress", reinterpret_cast<void*>(eglGetProcAddress)},
        // The two queries whose answer depends on which side of glvnd is asking.
        {"eglQueryString", reinterpret_cast<void*>(VendorQueryString)},
        {"eglGetPlatformDisplay", reinterpret_cast<void*>(VendorGetPlatformDisplay)},
        {"eglGetPlatformDisplayEXT", reinterpret_cast<void*>(VendorGetPlatformDisplay)},
        // What glvnd requires and this implementation does not otherwise have.
        {"eglQueryDevicesEXT", reinterpret_cast<void*>(VendorQueryDevicesEXT)},
        {"eglQueryDeviceAttribEXT", reinterpret_cast<void*>(VendorQueryDeviceAttribEXT)},
        {"eglQueryDeviceStringEXT", reinterpret_cast<void*>(VendorQueryDeviceStringEXT)},
        {"eglQueryDisplayAttribEXT", reinterpret_cast<void*>(VendorQueryDisplayAttribEXT)},
        {"eglDebugMessageControlKHR", reinterpret_cast<void*>(VendorDebugMessageControlKHR)},
        {"eglQueryDebugKHR", reinterpret_cast<void*>(VendorQueryDebugKHR)},
        {"eglLabelObjectKHR", reinterpret_cast<void*>(VendorLabelObjectKHR)},
    };

    void* EGLAPIENTRY VendorGetProcAddress(const char* procName) {
        if (procName == nullptr) {
            return nullptr;
        }
        for (const EntryPoint& entry : kEglEntryPoints) {
            if (std::strcmp(entry.name, procName) == 0) {
                return entry.address;
            }
        }
        // Anything that is not EGL is a GL or GLES entry point, and the tree's own resolver is the
        // table for those.
        return reinterpret_cast<void*>(MG_Impl::EGLImpl::GetProcAddress(procName));
    }

    // glvnd hands a vendor the index each EGL entry point occupies in its dispatch table, so that a
    // vendor whose entry points are thin wrappers around glvnd can call back into the right slot.
    // This library's entry points ARE the implementation - they never re-enter glvnd - so there is
    // nothing to record; the callback exists because glvnd refuses a vendor that does not provide
    // one, and refusing it means the library is loaded and never dispatched to.
    void EGLAPIENTRY VendorSetDispatchIndex(const char* procName, int index) {
        (void)procName;
        (void)index;
    }

    // glvnd builds its GL/GLES dispatch out of these: one address per entry point, taken from the
    // vendor that owns the current display.  This library exports them all, so the answer is its
    // own table.
    void* EGLAPIENTRY VendorGetDispatchAddress(const char* procName) {
        if (procName == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<void*>(MG_Impl::EGLImpl::GetProcAddress(procName));
    }
}  // namespace

extern "C" MOBILEGL_EGL_API EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports* exports,
                                                  __EGLvendorInfo* vendor, __EGLapiImports* imports) {
    (void)vendor;
    if (EGL_VENDOR_ABI_GET_MAJOR_VERSION(version) != EGL_VENDOR_ABI_MAJOR_VERSION) {
        // A vendor that does not know the ABI it is being handed must say so, not guess: the
        // structures on either side are the ABI.
        MGLOG_E("__egl_Main: unsupported glvnd vendor ABI version 0x%08x (this library implements %u.%u)", version,
                static_cast<unsigned>(EGL_VENDOR_ABI_MAJOR_VERSION),
                static_cast<unsigned>(EGL_VENDOR_ABI_MINOR_VERSION));
        return EGL_FALSE;
    }
    if (exports == nullptr || imports == nullptr) {
        MGLOG_E("__egl_Main: glvnd handed no exports/imports table");
        return EGL_FALSE;
    }
    g_eglExports = exports;

    imports->getPlatformDisplay = VendorGetPlatformDisplay;
    imports->getSupportsAPI = VendorGetSupportsAPI;
    imports->getVendorString = VendorGetVendorString;
    imports->getProcAddress = VendorGetProcAddress;
    imports->getDispatchAddress = VendorGetDispatchAddress;
    imports->setDispatchIndex = VendorSetDispatchIndex;

    MGLOG_I("glvnd vendor: MobileGL is serving EGL through libglvnd (ABI %u.%u, platforms: %s)",
            static_cast<unsigned>(EGL_VENDOR_ABI_MAJOR_VERSION), static_cast<unsigned>(EGL_VENDOR_ABI_MINOR_VERSION),
            kPlatformExtensions);
    return EGL_TRUE;
}
