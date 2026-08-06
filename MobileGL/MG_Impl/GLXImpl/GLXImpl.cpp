// MobileGL - MobileGL/MG_Impl/GLXImpl/GLXImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GLXImpl.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include "../EGLImpl/EGLImpl.h"
#include "../GetProcAddress.h"
#include <Init.h>

namespace MobileGL::MG_Impl::GLXImpl {
    namespace {
        // ---- GLX tokens (defined locally: GL/glx.h drags in a conflicting gl.h) ----
        constexpr int GLX_USE_GL = 1;
        constexpr int GLX_BUFFER_SIZE = 2;
        constexpr int GLX_LEVEL = 3;
        constexpr int GLX_RGBA = 4;
        constexpr int GLX_DOUBLEBUFFER = 5;
        constexpr int GLX_STEREO = 6;
        constexpr int GLX_AUX_BUFFERS = 7;
        constexpr int GLX_RED_SIZE = 8;
        constexpr int GLX_GREEN_SIZE = 9;
        constexpr int GLX_BLUE_SIZE = 10;
        constexpr int GLX_ALPHA_SIZE = 11;
        constexpr int GLX_DEPTH_SIZE = 12;
        constexpr int GLX_STENCIL_SIZE = 13;
        constexpr int GLX_ACCUM_RED_SIZE = 14;
        constexpr int GLX_ACCUM_GREEN_SIZE = 15;
        constexpr int GLX_ACCUM_BLUE_SIZE = 16;
        constexpr int GLX_ACCUM_ALPHA_SIZE = 17;
        // glXGetConfig/glXGetFBConfigAttrib error returns
        constexpr int GLX_BAD_ATTRIBUTE = 2;
        // glXGetClientString/glXQueryServerString names
        constexpr int GLX_VENDOR = 1;
        constexpr int GLX_VERSION = 2;
        constexpr int GLX_EXTENSIONS = 3;
        // GLX 1.3+ FBConfig attributes
        constexpr int GLX_CONFIG_CAVEAT = 0x20;
        constexpr int GLX_X_VISUAL_TYPE = 0x22;
        constexpr int GLX_TRANSPARENT_TYPE = 0x23;
        constexpr int GLX_TRANSPARENT_INDEX_VALUE = 0x24;
        constexpr int GLX_TRANSPARENT_RED_VALUE = 0x25;
        constexpr int GLX_TRANSPARENT_GREEN_VALUE = 0x26;
        constexpr int GLX_TRANSPARENT_BLUE_VALUE = 0x27;
        constexpr int GLX_TRANSPARENT_ALPHA_VALUE = 0x28;
        constexpr int GLX_DONT_CARE = static_cast<int>(0xFFFFFFFF);
        constexpr int GLX_NONE = 0x8000;
        constexpr int GLX_TRUE_COLOR = 0x8002;
        constexpr int GLX_VISUAL_ID = 0x800B;
        constexpr int GLX_SCREEN = 0x800C;
        constexpr int GLX_DRAWABLE_TYPE = 0x8010;
        constexpr int GLX_RENDER_TYPE = 0x8011;
        constexpr int GLX_X_RENDERABLE = 0x8012;
        constexpr int GLX_FBCONFIG_ID = 0x8013;
        constexpr int GLX_RGBA_TYPE = 0x8014;
        constexpr int GLX_MAX_PBUFFER_WIDTH = 0x8016;
        constexpr int GLX_MAX_PBUFFER_HEIGHT = 0x8017;
        constexpr int GLX_MAX_PBUFFER_PIXELS = 0x8018;
        constexpr int GLX_WIDTH = 0x801D;
        constexpr int GLX_HEIGHT = 0x801E;
        constexpr int GLX_WINDOW_BIT = 0x00000001;
        constexpr int GLX_RGBA_BIT = 0x00000001;
        constexpr int GLX_SAMPLE_BUFFERS = 100000;
        constexpr int GLX_SAMPLES = 100001;
        // GLX_EXT_swap_control
        constexpr int GLX_SWAP_INTERVAL_EXT = 0x20F1;
        constexpr int GLX_MAX_SWAP_INTERVAL_EXT = 0x20F2;
        // GLX_ARB_create_context / _profile / _no_error
        constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
        constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
        constexpr int GLX_CONTEXT_FLAGS_ARB = 0x2094;
        constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
        constexpr int GLX_CONTEXT_DEBUG_BIT_ARB = 0x0001;
        constexpr int GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB = 0x0002;
        constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
        constexpr int GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;
        constexpr int GLX_CONTEXT_OPENGL_NO_ERROR_ARB = 0x31B3;

        constexpr const char* kGLXExtensions =
            "GLX_ARB_create_context GLX_ARB_create_context_no_error GLX_ARB_create_context_profile "
            "GLX_ARB_get_proc_address GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control";

        // ---- Xlib access (dlopen'd at runtime, same convention as the backends;
        //      libX11 is never a link-time dependency of libMobileGL) ----

        // ABI-stable mirror of XVisualInfo (Xutil.h is not includable here: the
        // Bool/Status macros it needs were popped after the vulkan include).
        struct XVisualInfoCompat {
            void* visual;
            VisualID visualid;
            int screen;
            int depth;
            int c_class;
            unsigned long red_mask;
            unsigned long green_mask;
            unsigned long blue_mask;
            int colormap_size;
            int bits_per_rgb;
        };
        constexpr long kVisualIDMask = 0x1;
        constexpr long kVisualScreenMask = 0x2;
        constexpr long kVisualDepthMask = 0x4;
        constexpr long kVisualClassMask = 0x8;
        constexpr int kTrueColor = 4;

        struct X11Functions {
            void* Library = nullptr;
            int (*GetGeometry)(Display*, GLXDrawableHandle, GLXDrawableHandle*, int*, int*, unsigned int*,
                               unsigned int*, unsigned int*, unsigned int*) = nullptr;
            XVisualInfoCompat* (*GetVisualInfo)(Display*, long, XVisualInfoCompat*, int*) = nullptr;
            int (*GetDefaultScreen)(Display*) = nullptr;
            void* (*GetDefaultVisual)(Display*, int) = nullptr;
            VisualID (*VisualIDFromVisual)(void*) = nullptr;
            int (*Free)(void*) = nullptr;
            int (*Sync)(Display*, int) = nullptr;

            Bool Valid() const {
                return GetGeometry && GetVisualInfo && GetDefaultScreen && GetDefaultVisual &&
                       VisualIDFromVisual && Free;
            }
        };

        const X11Functions& X11() {
            static const X11Functions* functions = [] {
                auto* fns = new X11Functions();
                for (const char* name : {"libX11.so.6", "libX11.so"}) {
                    fns->Library = dlopen(name, RTLD_LOCAL | RTLD_NOW);
                    if (fns->Library) {
                        break;
                    }
                }
                if (fns->Library) {
                    fns->GetGeometry = reinterpret_cast<decltype(fns->GetGeometry)>(
                        dlsym(fns->Library, "XGetGeometry"));
                    fns->GetVisualInfo = reinterpret_cast<decltype(fns->GetVisualInfo)>(
                        dlsym(fns->Library, "XGetVisualInfo"));
                    fns->GetDefaultScreen = reinterpret_cast<decltype(fns->GetDefaultScreen)>(
                        dlsym(fns->Library, "XDefaultScreen"));
                    fns->GetDefaultVisual = reinterpret_cast<decltype(fns->GetDefaultVisual)>(
                        dlsym(fns->Library, "XDefaultVisual"));
                    fns->VisualIDFromVisual = reinterpret_cast<decltype(fns->VisualIDFromVisual)>(
                        dlsym(fns->Library, "XVisualIDFromVisual"));
                    fns->Free = reinterpret_cast<decltype(fns->Free)>(dlsym(fns->Library, "XFree"));
                    fns->Sync = reinterpret_cast<decltype(fns->Sync)>(dlsym(fns->Library, "XSync"));
                }
                if (!fns->Valid()) {
                    MGLOG_E("glx: failed to load libX11 entry points");
                }
                return fns;
            }();
            return *functions;
        }

        // ---- FBConfigs: mirror the two EGLState configs, stencil-8 first so
        //      stencil-wanting choosers (GLFW's default hints) match config 1 ----
        struct FBConfigInfo {
            int FBConfigId;
            GLint StencilBits;
        };
        constexpr FBConfigInfo kFBConfigs[] = {
            {1, 8},
            {2, 0},
        };
        constexpr int kFBConfigCount = static_cast<int>(std::size(kFBConfigs));

        const FBConfigInfo* TryGetFBConfig(GLXFBConfigHandle config) {
            const auto* info = static_cast<const FBConfigInfo*>(config);
            return (info >= kFBConfigs && info < kFBConfigs + kFBConfigCount) ? info : nullptr;
        }

        struct ContextObject {
            Display* XDisplay = nullptr;
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLConfig Config = nullptr;
            EGLContext Context = EGL_NO_CONTEXT;
            const FBConfigInfo* FBConfig = nullptr;
        };

        struct DrawableSurface {
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLSurface Surface = EGL_NO_SURFACE;
            Uint32 Width = 0;
            Uint32 Height = 0;
            std::chrono::steady_clock::time_point LastSizePoll{};
        };

        std::recursive_mutex& RegistryMutex() {
            static auto* mutex = new std::recursive_mutex();
            return *mutex;
        }

        UnorderedMap<GLXContextHandle, ContextObject>& Contexts() {
            static auto* contexts = new UnorderedMap<GLXContextHandle, ContextObject>();
            return *contexts;
        }

        UnorderedMap<GLXDrawableHandle, DrawableSurface>& DrawableSurfaces() {
            static auto* surfaces = new UnorderedMap<GLXDrawableHandle, DrawableSurface>();
            return *surfaces;
        }

        UnorderedMap<GLXDrawableHandle, const FBConfigInfo*>& WindowFBConfigs() {
            static auto* configs = new UnorderedMap<GLXDrawableHandle, const FBConfigInfo*>();
            return *configs;
        }

        Uint64& NextContextHandle() {
            static auto* handle = new Uint64(0x10000);
            return *handle;
        }

        struct ThreadCurrent {
            Display* XDisplay = nullptr;
            GLXDrawableHandle Draw = 0;
            GLXDrawableHandle Read = 0;
            GLXContextHandle Context = nullptr;
        };
        thread_local ThreadCurrent t_current;

        Int& SwapIntervalShadow() {
            static auto* interval = new Int(1);
            return *interval;
        }

        void EnsureInitialized() {
            // MobileGL::EnsureInitialized (not a local once_flag) so a fresh init
            // can follow a full teardown from the last eglTerminate.
            MobileGL::EnsureInitialized();
        }

        EGLDisplay EnsureDisplay() {
            EnsureInitialized();
            EGLDisplay display = EGLImpl::GetDisplay(EGL_DEFAULT_DISPLAY);
            if (display == EGL_NO_DISPLAY) {
                return EGL_NO_DISPLAY;
            }
            if (!EGLImpl::Initialize(display, nullptr, nullptr)) {
                return EGL_NO_DISPLAY;
            }
            return display;
        }

        GLXContextHandle EncodeContext(Uint64 handle) {
            return reinterpret_cast<GLXContextHandle>(static_cast<SizeT>(handle));
        }

        ContextObject* TryGetContext(GLXContextHandle context) {
            auto& contexts = Contexts();
            auto it = contexts.find(context);
            return it == contexts.end() ? nullptr : &it->second;
        }

        Bool QueryDrawableSize(Display* dpy, GLXDrawableHandle drawable, Uint32& width, Uint32& height) {
            const auto& x11 = X11();
            if (!x11.Valid() || !dpy || drawable == 0) {
                return false;
            }
            GLXDrawableHandle root = 0;
            int x = 0;
            int y = 0;
            unsigned int w = 0;
            unsigned int h = 0;
            unsigned int border = 0;
            unsigned int depth = 0;
            if (!x11.GetGeometry(dpy, drawable, &root, &x, &y, &w, &h, &border, &depth)) {
                return false;
            }
            width = std::max(w, 1u);
            height = std::max(h, 1u);
            return true;
        }

        // The backends never query the X window size themselves; the GLX layer
        // owns size discovery and pushes changes through the internal resize hook
        // (same contract as the WGL and CGL layers). Polling XGetGeometry is a
        // server round trip, so throttle steady-state swaps.
        void SyncSurfaceSize(Display* dpy, GLXDrawableHandle drawable, DrawableSurface& surface,
                             Bool force = false) {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now - surface.LastSizePoll < std::chrono::milliseconds(250)) {
                return;
            }
            surface.LastSizePoll = now;
            Uint32 width = 0;
            Uint32 height = 0;
            if (!QueryDrawableSize(dpy, drawable, width, height)) {
                return;
            }
            if (width == surface.Width && height == surface.Height) {
                return;
            }
            if (EGLImpl::ResizePlatformWindowSurface(surface.Display, surface.Surface,
                                                     static_cast<EGLint>(width),
                                                     static_cast<EGLint>(height))) {
                surface.Width = width;
                surface.Height = height;
            }
        }

        DrawableSurface* EnsureDrawableSurface(Display* dpy, GLXDrawableHandle drawable,
                                               const ContextObject& context) {
            auto& surfaces = DrawableSurfaces();
            auto it = surfaces.find(drawable);
            if (it != surfaces.end()) {
                SyncSurfaceSize(dpy, drawable, it->second, true);
                return &it->second;
            }

            Uint32 width = 0;
            Uint32 height = 0;
            if (!QueryDrawableSize(dpy, drawable, width, height)) {
                MGLOG_E("glx: XGetGeometry failed for drawable 0x%lx", drawable);
                return nullptr;
            }

            const EGLAttrib attribs[] = {
                EGL_WIDTH, static_cast<EGLAttrib>(width),
                EGL_HEIGHT, static_cast<EGLAttrib>(height),
                EGL_NONE,
            };
            EGLSurface surface = EGLImpl::CreatePlatformWindowSurface(
                context.Display, context.Config, reinterpret_cast<void*>(drawable), attribs);
            if (surface == EGL_NO_SURFACE) {
                MGLOG_E("glx: failed to create window surface for drawable 0x%lx (%ux%u)", drawable,
                        width, height);
                return nullptr;
            }

            DrawableSurface record;
            record.Display = context.Display;
            record.Surface = surface;
            record.Width = width;
            record.Height = height;
            record.LastSizePoll = std::chrono::steady_clock::now();
            auto [inserted, _] = surfaces.emplace(drawable, record);
            return &inserted->second;
        }

        GLXContextHandle CreateContextFromEGLAttribs(Display* dpy, const FBConfigInfo* fbconfig,
                                                     GLXContextHandle share,
                                                     const EGLint* contextAttribs) {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            EGLDisplay display = EnsureDisplay();
            if (display == EGL_NO_DISPLAY) {
                MGLOG_E("glx: no EGL display");
                return nullptr;
            }
            EGLImpl::BindAPI(EGL_OPENGL_API);

            EGLContext shareContext = EGL_NO_CONTEXT;
            if (share) {
                auto* shareObject = TryGetContext(share);
                if (!shareObject) {
                    return nullptr;
                }
                shareContext = shareObject->Context;
            }

            const EGLint configAttribs[] = {
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 24,
                EGL_STENCIL_SIZE, fbconfig->StencilBits,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_NONE,
            };
            EGLConfig config = nullptr;
            EGLint configCount = 0;
            if (!EGLImpl::ChooseConfig(display, configAttribs, &config, 1, &configCount) ||
                configCount <= 0) {
                MGLOG_E("glx: eglChooseConfig failed");
                return nullptr;
            }

            EGLContext eglContext = EGLImpl::CreateContext(display, config, shareContext, contextAttribs);
            if (eglContext == EGL_NO_CONTEXT) {
                MGLOG_E("glx: eglCreateContext failed");
                return nullptr;
            }

            ContextObject object;
            object.XDisplay = dpy;
            object.Display = display;
            object.Config = config;
            object.Context = eglContext;
            object.FBConfig = fbconfig;
            const auto handle = EncodeContext(NextContextHandle()++);
            Contexts()[handle] = object;
            MGLOG_I("glx: created context %p (EGL context %p, stencil %d)", handle, eglContext,
                    fbconfig->StencilBits);
            return handle;
        }

        int FBConfigAttribValue(Display* dpy, const FBConfigInfo& info, int attribute, int* value) {
            const auto& x11 = X11();
            switch (attribute) {
            case GLX_FBCONFIG_ID:
                *value = info.FBConfigId;
                return 0;
            case GLX_BUFFER_SIZE:
                *value = 32;
                return 0;
            case GLX_LEVEL:
                *value = 0;
                return 0;
            case GLX_DOUBLEBUFFER:
                *value = 1;
                return 0;
            case GLX_STEREO:
                *value = 0;
                return 0;
            case GLX_AUX_BUFFERS:
                *value = 0;
                return 0;
            case GLX_RED_SIZE:
            case GLX_GREEN_SIZE:
            case GLX_BLUE_SIZE:
            case GLX_ALPHA_SIZE:
                *value = 8;
                return 0;
            case GLX_DEPTH_SIZE:
                *value = 24;
                return 0;
            case GLX_STENCIL_SIZE:
                *value = info.StencilBits;
                return 0;
            case GLX_ACCUM_RED_SIZE:
            case GLX_ACCUM_GREEN_SIZE:
            case GLX_ACCUM_BLUE_SIZE:
            case GLX_ACCUM_ALPHA_SIZE:
                *value = 0;
                return 0;
            case GLX_RENDER_TYPE:
                *value = GLX_RGBA_BIT;
                return 0;
            case GLX_DRAWABLE_TYPE:
                *value = GLX_WINDOW_BIT;
                return 0;
            case GLX_X_RENDERABLE:
                *value = 1;
                return 0;
            case GLX_X_VISUAL_TYPE:
                *value = GLX_TRUE_COLOR;
                return 0;
            case GLX_CONFIG_CAVEAT:
            case GLX_TRANSPARENT_TYPE:
                *value = GLX_NONE;
                return 0;
            case GLX_TRANSPARENT_INDEX_VALUE:
            case GLX_TRANSPARENT_RED_VALUE:
            case GLX_TRANSPARENT_GREEN_VALUE:
            case GLX_TRANSPARENT_BLUE_VALUE:
            case GLX_TRANSPARENT_ALPHA_VALUE:
                *value = 0;
                return 0;
            case GLX_VISUAL_ID: {
                *value = 0;
                if (x11.Valid() && dpy) {
                    const int screen = x11.GetDefaultScreen(dpy);
                    void* visual = x11.GetDefaultVisual(dpy, screen);
                    if (visual) {
                        *value = static_cast<int>(x11.VisualIDFromVisual(visual));
                    }
                }
                return 0;
            }
            case GLX_SCREEN:
                *value = (x11.Valid() && dpy) ? x11.GetDefaultScreen(dpy) : 0;
                return 0;
            case GLX_MAX_PBUFFER_WIDTH:
            case GLX_MAX_PBUFFER_HEIGHT:
                *value = 16384;
                return 0;
            case GLX_MAX_PBUFFER_PIXELS:
                *value = 16384 * 16384;
                return 0;
            case GLX_SAMPLE_BUFFERS:
            case GLX_SAMPLES:
                *value = 0;
                return 0;
            default:
                *value = 0;
                return GLX_BAD_ATTRIBUTE;
            }
        }

        // Resolve the X visual all our configs render to: the default visual of
        // the screen (matches EGLState's EGL_NATIVE_VISUAL_ID policy), falling
        // back to any 24-bit TrueColor visual. Caller frees with XFree.
        XVisualInfoCompat* ResolveVisual(Display* dpy, int screen) {
            const auto& x11 = X11();
            if (!x11.Valid() || !dpy) {
                return nullptr;
            }
            if (screen < 0) {
                screen = x11.GetDefaultScreen(dpy);
            }
            if (void* visual = x11.GetDefaultVisual(dpy, screen)) {
                XVisualInfoCompat tmpl{};
                tmpl.visualid = x11.VisualIDFromVisual(visual);
                int count = 0;
                XVisualInfoCompat* info = x11.GetVisualInfo(dpy, kVisualIDMask, &tmpl, &count);
                if (info && count >= 1 && info->depth >= 24 && info->c_class == kTrueColor) {
                    return info;
                }
                if (info) {
                    x11.Free(info);
                }
            }
            XVisualInfoCompat tmpl{};
            tmpl.screen = screen;
            tmpl.depth = 24;
            tmpl.c_class = kTrueColor;
            int count = 0;
            XVisualInfoCompat* info = x11.GetVisualInfo(
                dpy, kVisualScreenMask | kVisualDepthMask | kVisualClassMask, &tmpl, &count);
            if (info && count < 1) {
                x11.Free(info);
                return nullptr;
            }
            return info;
        }
    } // namespace

    int QueryExtension(Display*, int* errorBase, int* eventBase) {
        if (errorBase) {
            *errorBase = 0;
        }
        if (eventBase) {
            *eventBase = 0;
        }
        return 1;
    }

    int QueryVersion(Display*, int* major, int* minor) {
        if (major) {
            *major = 1;
        }
        if (minor) {
            *minor = 4;
        }
        return 1;
    }

    const char* QueryExtensionsString(Display*, int) {
        return kGLXExtensions;
    }

    const char* GetClientString(Display*, int name) {
        switch (name) {
        case GLX_VENDOR:
            return "MobileGL";
        case GLX_VERSION:
            return "1.4";
        case GLX_EXTENSIONS:
            return kGLXExtensions;
        default:
            return nullptr;
        }
    }

    const char* QueryServerString(Display* dpy, int, int name) {
        return GetClientString(dpy, name);
    }

    GLXFBConfigHandle* GetFBConfigs(Display*, int, int* nelements) {
        auto** configs = static_cast<GLXFBConfigHandle*>(
            std::malloc(sizeof(GLXFBConfigHandle) * kFBConfigCount));
        if (!configs) {
            if (nelements) {
                *nelements = 0;
            }
            return nullptr;
        }
        for (int i = 0; i < kFBConfigCount; ++i) {
            configs[i] = const_cast<FBConfigInfo*>(&kFBConfigs[i]);
        }
        if (nelements) {
            *nelements = kFBConfigCount;
        }
        return configs;
    }

    GLXFBConfigHandle* ChooseFBConfig(Display* dpy, int screen, const int* attribList, int* nelements) {
        // Every config satisfies >= size matches for our fixed RGBA8888/24 caps;
        // the only distinguishing filters are stencil size and hard mismatches.
        Bool acceptConfig[kFBConfigCount] = {true, true};
        if (attribList) {
            for (SizeT i = 0; attribList[i] != 0; i += 2) {
                const int attrib = attribList[i];
                const int value = attribList[i + 1];
                if (value == GLX_DONT_CARE) {
                    continue;
                }
                switch (attrib) {
                case GLX_STENCIL_SIZE:
                    for (int c = 0; c < kFBConfigCount; ++c) {
                        if (kFBConfigs[c].StencilBits < value) {
                            acceptConfig[c] = false;
                        }
                    }
                    break;
                case GLX_FBCONFIG_ID:
                    for (int c = 0; c < kFBConfigCount; ++c) {
                        if (kFBConfigs[c].FBConfigId != value) {
                            acceptConfig[c] = false;
                        }
                    }
                    break;
                case GLX_RED_SIZE:
                case GLX_GREEN_SIZE:
                case GLX_BLUE_SIZE:
                case GLX_ALPHA_SIZE:
                    if (value > 8) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_BUFFER_SIZE:
                    if (value > 32) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_DEPTH_SIZE:
                    if (value > 24) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_DOUBLEBUFFER:
                    if (value != 1) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_STEREO:
                case GLX_AUX_BUFFERS:
                case GLX_SAMPLE_BUFFERS:
                case GLX_SAMPLES:
                case GLX_LEVEL:
                case GLX_ACCUM_RED_SIZE:
                case GLX_ACCUM_GREEN_SIZE:
                case GLX_ACCUM_BLUE_SIZE:
                case GLX_ACCUM_ALPHA_SIZE:
                    if (value > 0) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_RENDER_TYPE:
                    if ((value & GLX_RGBA_BIT) == 0) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                case GLX_DRAWABLE_TYPE:
                    if ((value & ~GLX_WINDOW_BIT) != 0) {
                        acceptConfig[0] = acceptConfig[1] = false;
                    }
                    break;
                default:
                    // Lenient like EGLState's ChooseConfig: unknown attributes
                    // are ignored rather than failing the whole request.
                    break;
                }
            }
        }

        int count = 0;
        for (int c = 0; c < kFBConfigCount; ++c) {
            if (acceptConfig[c]) {
                ++count;
            }
        }
        if (count == 0) {
            if (nelements) {
                *nelements = 0;
            }
            return nullptr;
        }
        auto** configs =
            static_cast<GLXFBConfigHandle*>(std::malloc(sizeof(GLXFBConfigHandle) * count));
        if (!configs) {
            if (nelements) {
                *nelements = 0;
            }
            return nullptr;
        }
        int out = 0;
        for (int c = 0; c < kFBConfigCount; ++c) {
            if (acceptConfig[c]) {
                configs[out++] = const_cast<FBConfigInfo*>(&kFBConfigs[c]);
            }
        }
        if (nelements) {
            *nelements = count;
        }
        (void)dpy;
        (void)screen;
        return configs;
    }

    int GetFBConfigAttrib(Display* dpy, GLXFBConfigHandle config, int attribute, int* value) {
        const auto* info = TryGetFBConfig(config);
        if (!info || !value) {
            return GLX_BAD_ATTRIBUTE;
        }
        return FBConfigAttribValue(dpy, *info, attribute, value);
    }

    void* GetVisualFromFBConfig(Display* dpy, GLXFBConfigHandle config) {
        if (!TryGetFBConfig(config)) {
            return nullptr;
        }
        return ResolveVisual(dpy, -1);
    }

    void* ChooseVisual(Display* dpy, int screen, int* attribList) {
        // Legacy visual chooser: our single RGBA8888/24/8 double-buffered visual
        // satisfies any RGBA request; color-index and stereo are unsupported.
        Bool wantsRGBA = false;
        if (attribList) {
            for (SizeT i = 0; attribList[i] != 0;) {
                const int attrib = attribList[i];
                if (attrib == GLX_RGBA || attrib == GLX_USE_GL || attrib == GLX_DOUBLEBUFFER) {
                    wantsRGBA = wantsRGBA || attrib == GLX_RGBA;
                    ++i;
                    continue;
                }
                if (attrib == GLX_STEREO) {
                    return nullptr;
                }
                i += 2;
            }
        }
        if (!wantsRGBA) {
            return nullptr;
        }
        return ResolveVisual(dpy, screen);
    }

    int GetConfig(Display*, void* visualInfo, int attribute, int* value) {
        if (!visualInfo || !value) {
            return GLX_BAD_ATTRIBUTE;
        }
        switch (attribute) {
        case GLX_USE_GL:
        case GLX_RGBA:
        case GLX_DOUBLEBUFFER:
            *value = 1;
            return 0;
        case GLX_STEREO:
        case GLX_LEVEL:
        case GLX_AUX_BUFFERS:
        case GLX_ACCUM_RED_SIZE:
        case GLX_ACCUM_GREEN_SIZE:
        case GLX_ACCUM_BLUE_SIZE:
        case GLX_ACCUM_ALPHA_SIZE:
            *value = 0;
            return 0;
        case GLX_BUFFER_SIZE:
            *value = 32;
            return 0;
        case GLX_RED_SIZE:
        case GLX_GREEN_SIZE:
        case GLX_BLUE_SIZE:
        case GLX_ALPHA_SIZE:
            *value = 8;
            return 0;
        case GLX_DEPTH_SIZE:
            *value = 24;
            return 0;
        case GLX_STENCIL_SIZE:
            *value = 8;
            return 0;
        default:
            *value = 0;
            return GLX_BAD_ATTRIBUTE;
        }
    }

    GLXContextHandle CreateContext(Display* dpy, void* visualInfo, GLXContextHandle share, int) {
        EnsureInitialized();
        MGLOG_I("glx: glXCreateContext(visual=%p)", visualInfo);
        if (!visualInfo) {
            return nullptr;
        }
        // A legacy GLX context is a compatibility-profile context; MobileGL keys
        // its relaxed-semantics mode off the explicit compatibility bit.
        const EGLint attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        return CreateContextFromEGLAttribs(dpy, &kFBConfigs[0], share, attribs);
    }

    GLXContextHandle CreateNewContext(Display* dpy, GLXFBConfigHandle config, int renderType,
                                      GLXContextHandle share, int) {
        EnsureInitialized();
        const auto* info = TryGetFBConfig(config);
        if (!info || renderType != GLX_RGBA_TYPE) {
            return nullptr;
        }
        const EGLint attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        return CreateContextFromEGLAttribs(dpy, info, share, attribs);
    }

    GLXContextHandle CreateContextAttribsARB(Display* dpy, GLXFBConfigHandle config,
                                             GLXContextHandle share, int, const int* attribList) {
        EnsureInitialized();
        const auto* info = TryGetFBConfig(config);
        if (!info) {
            return nullptr;
        }

        int major = 1;
        int minor = 0;
        int profileMask = 0;
        int flags = 0;
        if (attribList) {
            for (SizeT i = 0; attribList[i] != 0; i += 2) {
                const int attrib = attribList[i];
                const int value = attribList[i + 1];
                switch (attrib) {
                case GLX_CONTEXT_MAJOR_VERSION_ARB:
                    major = value;
                    break;
                case GLX_CONTEXT_MINOR_VERSION_ARB:
                    minor = value;
                    break;
                case GLX_CONTEXT_PROFILE_MASK_ARB:
                    profileMask = value;
                    break;
                case GLX_CONTEXT_FLAGS_ARB:
                    flags = value;
                    break;
                case GLX_CONTEXT_OPENGL_NO_ERROR_ARB:
                    // Accepted and ignored: MobileGL always validates.
                    break;
                default:
                    MGLOG_D("glXCreateContextAttribsARB: ignoring attrib 0x%04x = 0x%x", attrib,
                            value);
                    break;
                }
            }
        }

        if (major < 1 || (profileMask & ~(GLX_CONTEXT_CORE_PROFILE_BIT_ARB |
                                          GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB))) {
            return nullptr;
        }

        Vector<EGLint> attribs = {
            EGL_CONTEXT_MAJOR_VERSION, major,
            EGL_CONTEXT_MINOR_VERSION, minor,
        };
        const Bool wantsCompat = (profileMask & GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB) != 0;
        if (major > 3 || (major == 3 && minor >= 2) || profileMask != 0) {
            attribs.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK);
            attribs.push_back(wantsCompat ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT
                                          : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT);
        }
        if (flags & GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) {
            attribs.push_back(EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE);
            attribs.push_back(EGL_TRUE);
        }
        if (flags & GLX_CONTEXT_DEBUG_BIT_ARB) {
            attribs.push_back(EGL_CONTEXT_OPENGL_DEBUG);
            attribs.push_back(EGL_TRUE);
        }
        attribs.push_back(EGL_NONE);

        return CreateContextFromEGLAttribs(dpy, info, share, attribs.data());
    }

    void DestroyContext(Display*, GLXContextHandle context) {
        EnsureInitialized();
        MGLOG_I("glx: glXDestroyContext(%p)", context);
        if (t_current.Context == context) {
            MakeCurrent(t_current.XDisplay, 0, nullptr);
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(context);
        if (!object) {
            return;
        }
        if (object->Context != EGL_NO_CONTEXT) {
            EGLImpl::DestroyContext(object->Display, object->Context);
        }
        Contexts().erase(context);
    }

    int MakeCurrent(Display* dpy, GLXDrawableHandle drawable, GLXContextHandle context) {
        EnsureInitialized();
        MGLOG_D("glx: glXMakeCurrent(drawable=0x%lx, ctx=%p)", drawable, context);
        if (!context) {
            if (!t_current.Context) {
                t_current = {};
                return 1;
            }
            const EGLBoolean released =
                EGLImpl::MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            t_current = {};
            return released == EGL_TRUE ? 1 : 0;
        }

        if (drawable == 0) {
            return 0;
        }

        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(context);
        if (!object) {
            return 0;
        }

        DrawableSurface* surface = EnsureDrawableSurface(dpy, drawable, *object);
        if (!surface) {
            return 0;
        }

        if (!EGLImpl::MakeCurrent(object->Display, surface->Surface, surface->Surface,
                                  object->Context)) {
            MGLOG_E("glx: eglMakeCurrent failed (drawable=0x%lx, ctx=%p)", drawable, context);
            return 0;
        }
        t_current = {dpy, drawable, drawable, context};
        return 1;
    }

    int MakeContextCurrent(Display* dpy, GLXDrawableHandle draw, GLXDrawableHandle read,
                           GLXContextHandle context) {
        if (context && draw != read) {
            // MobileGL's backends reject split draw/read surfaces; bind the draw
            // drawable for both, which is what every real caller here needs.
            MGLOG_W("glx: glXMakeContextCurrent draw 0x%lx != read 0x%lx, using draw for both", draw,
                    read);
        }
        const int result = MakeCurrent(dpy, draw, context);
        if (result && context) {
            t_current.Read = read;
        }
        return result;
    }

    void SwapBuffers(Display* dpy, GLXDrawableHandle drawable) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto& surfaces = DrawableSurfaces();
        auto it = surfaces.find(drawable);
        if (it == surfaces.end()) {
            MGLOG_W("glx: glXSwapBuffers with no surface for drawable 0x%lx", drawable);
            return;
        }
        SyncSurfaceSize(dpy, drawable, it->second);
        EGLImpl::SwapBuffers(it->second.Display, it->second.Surface);
    }

    GLXDrawableHandle CreateWindow(Display*, GLXFBConfigHandle config, GLXDrawableHandle window,
                                   const int*) {
        EnsureInitialized();
        const auto* info = TryGetFBConfig(config);
        if (!info || window == 0) {
            return 0;
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        WindowFBConfigs()[window] = info;
        // The GLXWindow is the X window itself; the EGL surface is created
        // lazily on the first MakeCurrent (same pattern as WGL's HWND cache).
        return window;
    }

    void DestroyWindow(Display*, GLXDrawableHandle window) {
        EnsureInitialized();
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        WindowFBConfigs().erase(window);
        auto& surfaces = DrawableSurfaces();
        auto it = surfaces.find(window);
        if (it == surfaces.end()) {
            return;
        }
        EGLImpl::DestroySurface(it->second.Display, it->second.Surface);
        surfaces.erase(it);
    }

    GLXContextHandle GetCurrentContext() {
        return t_current.Context;
    }

    GLXDrawableHandle GetCurrentDrawable() {
        return t_current.Draw;
    }

    GLXDrawableHandle GetCurrentReadDrawable() {
        return t_current.Read;
    }

    Display* GetCurrentDisplay() {
        return t_current.XDisplay;
    }

    int IsDirect(Display*, GLXContextHandle) {
        return 1;
    }

    void WaitGL() {
        EGLImpl::WaitGL();
    }

    void WaitX() {
        const auto& x11 = X11();
        if (x11.Valid() && x11.Sync && t_current.XDisplay) {
            x11.Sync(t_current.XDisplay, 0);
        }
    }

    int QueryContext(Display*, GLXContextHandle context, int attribute, int* value) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(context);
        if (!object || !value) {
            return GLX_BAD_ATTRIBUTE;
        }
        switch (attribute) {
        case GLX_FBCONFIG_ID:
            *value = object->FBConfig ? object->FBConfig->FBConfigId : 0;
            return 0;
        case GLX_RENDER_TYPE:
            *value = GLX_RGBA_TYPE;
            return 0;
        case GLX_SCREEN:
            *value = 0;
            return 0;
        default:
            *value = 0;
            return GLX_BAD_ATTRIBUTE;
        }
    }

    void QueryDrawable(Display* dpy, GLXDrawableHandle drawable, int attribute, unsigned int* value) {
        if (!value) {
            return;
        }
        switch (attribute) {
        case GLX_WIDTH:
        case GLX_HEIGHT: {
            Uint32 width = 0;
            Uint32 height = 0;
            if (QueryDrawableSize(dpy, drawable, width, height)) {
                *value = attribute == GLX_WIDTH ? width : height;
            }
            return;
        }
        case GLX_SWAP_INTERVAL_EXT:
            *value = static_cast<unsigned int>(SwapIntervalShadow());
            return;
        case GLX_MAX_SWAP_INTERVAL_EXT:
            *value = 4;
            return;
        case GLX_FBCONFIG_ID: {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            auto& configs = WindowFBConfigs();
            auto it = configs.find(drawable);
            *value = it == configs.end() ? 0 : it->second->FBConfigId;
            return;
        }
        default:
            *value = 0;
            return;
        }
    }

    void SwapIntervalEXT(Display*, GLXDrawableHandle, int interval) {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        EGLDisplay display = EnsureDisplay();
        if (display == EGL_NO_DISPLAY) {
            return;
        }
        // MobileGL validates 0..4; adaptive vsync (negative) clamps to regular.
        interval = std::clamp(interval, 0, 4);
        EGLImpl::SwapInterval(display, interval);
        SwapIntervalShadow() = interval;
    }

    int SwapIntervalMESA(unsigned int interval) {
        SwapIntervalEXT(nullptr, 0, static_cast<int>(interval));
        return 0;
    }

    int GetSwapIntervalMESA() {
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        return SwapIntervalShadow();
    }

    int SwapIntervalSGI(int interval) {
        if (interval < 1) {
            interval = 1;
        }
        SwapIntervalEXT(nullptr, 0, interval);
        return 0;
    }
} // namespace MobileGL::MG_Impl::GLXImpl

#endif // __linux__ && !__ANDROID__
