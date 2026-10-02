// MobileGL - MobileGL/MG_Impl/GLXImpl/Exporting/GlvndVendor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MOBILEGL AS A GLVND GLX VENDOR.
//
// The libGL.so.1 / libGLX.so.0 an X11 application links on a glvnd system is a dispatcher: it
// picks a vendor library per X screen - the one the X server names, or the one
// __GLX_VENDOR_LIBRARY_NAME names - loads it as libGLX_<name>.so.0 and routes every GLX call and,
// once a context is current, every GL call to it. Under a Wayland compositor the X server is
// Xwayland, which names the distribution's driver; that driver then needs a GPU device this
// machine's X clients do not have, and glxinfo ends in "failed to retrieve device information".
// Installed as libGLX_mobilegl.so.0 and named by __GLX_VENDOR_LIBRARY_NAME=mobilegl, this library
// is the one asked instead, through the same entry points it exports directly.
//
// The ABI (libglvnd's libglxabi.h, vendor ABI 1.0) is mirrored here rather than included: that
// header includes GL/glx.h and Xlib.h, and this library's GLX layer is built without X headers.

#include <Includes.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include "../GLXImpl.h"
#include "../../GetProcAddress.h"

namespace {
    using namespace MobileGL;

    struct GLXVendorInfo;
    using XBool = int;

    // __GLXapiExportsRec, field for field.
    struct GLXApiExports {
        GLXVendorInfo* (*getDynDispatch)(Display* dpy, int screen);
        GLXVendorInfo* (*getCurrentDynDispatch)();
        void* (*fetchDispatchEntry)(GLXVendorInfo* dynDispatch, int index);
        void* (*getCurrentContext)();
        int (*addVendorContextMapping)(Display* dpy, void* context, GLXVendorInfo* vendor);
        void (*removeVendorContextMapping)(Display* dpy, void* context);
        GLXVendorInfo* (*vendorFromContext)(void* context);
        int (*addVendorFBConfigMapping)(Display* dpy, void* config, GLXVendorInfo* vendor);
        void (*removeVendorFBConfigMapping)(Display* dpy, void* config);
        GLXVendorInfo* (*vendorFromFBConfig)(Display* dpy, void* config);
        int (*addVendorDrawableMapping)(Display* dpy, XID drawable, GLXVendorInfo* vendor);
        void (*removeVendorDrawableMapping)(Display* dpy, XID drawable);
        GLXVendorInfo* (*vendorFromDrawable)(Display* dpy, XID drawable);
    };

    // __GLXapiImportsRec, field for field; the patch hooks are optional and left null.
    struct GLXApiImports {
        XBool (*isScreenSupported)(Display* dpy, int screen);
        void* (*getProcAddress)(const unsigned char* procName);
        void* (*getDispatchAddress)(const unsigned char* procName);
        void (*setDispatchIndex)(const unsigned char* procName, int index);
        XBool (*notifyError)(Display* dpy, unsigned char error, XID resid, unsigned char opcode, XBool coreX11error);
        unsigned char (*isPatchSupported)(int type, int stubSize);
        unsigned char (*initiatePatch)(int type, int stubSize, void* lookupStubOffset);
        void (*releasePatch)();
        void (*patchThreadAttach)();
    };

    constexpr Uint32 kGLXVendorABIMajor = 1;

    const GLXApiExports* g_glxExports = nullptr;
    GLXVendorInfo* g_glxVendor = nullptr;

    XBool VendorIsScreenSupported(Display*, int) { return 1; }

    // GLX names are the exported entry points (each holds the stream lock); GL names are the
    // implementation's, as eglGetProcAddress hands them out.
    void* VendorGetProcAddress(const unsigned char* procName) {
        const char* name = reinterpret_cast<const char*>(procName);
        if (std::strncmp(name, "glX", 3) == 0) return MG_Impl::GLXImpl::GetGLXEntryPoint(name);
        return MG_Impl::GetProcAddress(name);
    }

    // A context made by an extension is one libGLX did not see created, so it is told whose it is
    // - or glXMakeCurrent on it finds no vendor and fails.
    void* VendorCreateContextAttribsARB(Display* dpy, void* config, void* share, int direct, const int* attribs) {
        using Create = void* (*)(Display*, void*, void*, int, const int*);
        auto* create = reinterpret_cast<Create>(MG_Impl::GLXImpl::GetGLXEntryPoint("glXCreateContextAttribsARB"));
        void* context = create ? create(dpy, config, share, direct, attribs) : nullptr;
        if (context && g_glxExports && g_glxExports->addVendorContextMapping(dpy, context, g_glxVendor) != 0) {
            using Destroy = void (*)(Display*, void*);
            if (auto* destroy = reinterpret_cast<Destroy>(MG_Impl::GLXImpl::GetGLXEntryPoint("glXDestroyContext"))) {
                destroy(dpy, context);
            }
            return nullptr;
        }
        return context;
    }

    // libGLX asks for the GLX functions it has no dispatcher of its own for (the extensions). This
    // is the only vendor in the process that answers for its screens, so the "dispatcher" is the
    // implementation itself; only context creation has bookkeeping to do on the way.
    void* VendorGetDispatchAddress(const unsigned char* procName) {
        const char* name = reinterpret_cast<const char*>(procName);
        if (std::strcmp(name, "glXCreateContextAttribsARB") == 0) {
            return reinterpret_cast<void*>(&VendorCreateContextAttribsARB);
        }
        return MG_Impl::GLXImpl::GetGLXEntryPoint(name);
    }

    void VendorSetDispatchIndex(const unsigned char*, int) {}
} // namespace

extern "C" MOBILEGL_API int __glx_Main(Uint32 version, const void* exportsTable, void* vendor, void* importsTable) {
    const auto* exports = static_cast<const GLXApiExports*>(exportsTable);
    auto* imports = static_cast<GLXApiImports*>(importsTable);
    if ((version >> 16) != kGLXVendorABIMajor || exports == nullptr || imports == nullptr) return 0;
    g_glxExports = exports;
    g_glxVendor = static_cast<GLXVendorInfo*>(vendor);
    imports->isScreenSupported = &VendorIsScreenSupported;
    imports->getProcAddress = &VendorGetProcAddress;
    imports->getDispatchAddress = &VendorGetDispatchAddress;
    imports->setDispatchIndex = &VendorSetDispatchIndex;
    imports->notifyError = nullptr;
    imports->isPatchSupported = nullptr;
    imports->initiatePatch = nullptr;
    imports->releasePatch = nullptr;
    imports->patchThreadAttach = nullptr;
    return 1;
}

#endif // __linux__ && !__ANDROID__
