// MobileGL - MobileGL/MG_Impl/WGLImpl/WGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WGLImpl.h"

#if defined(_WIN32)
#include "../EGLImpl/EGLImpl.h"
#include "../GetProcAddress.h"
#include <Init.h>

namespace MobileGL::MG_Impl::WGLImpl {
    namespace {
        // WGL_ARB_pixel_format
        constexpr int WGL_NUMBER_PIXEL_FORMATS_ARB = 0x2000;
        constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
        constexpr int WGL_DRAW_TO_BITMAP_ARB = 0x2002;
        constexpr int WGL_ACCELERATION_ARB = 0x2003;
        constexpr int WGL_NEED_PALETTE_ARB = 0x2004;
        constexpr int WGL_NEED_SYSTEM_PALETTE_ARB = 0x2005;
        constexpr int WGL_SWAP_LAYER_BUFFERS_ARB = 0x2006;
        constexpr int WGL_SWAP_METHOD_ARB = 0x2007;
        constexpr int WGL_NUMBER_OVERLAYS_ARB = 0x2008;
        constexpr int WGL_NUMBER_UNDERLAYS_ARB = 0x2009;
        constexpr int WGL_TRANSPARENT_ARB = 0x200A;
        constexpr int WGL_SHARE_DEPTH_ARB = 0x200C;
        constexpr int WGL_SHARE_STENCIL_ARB = 0x200D;
        constexpr int WGL_SHARE_ACCUM_ARB = 0x200E;
        constexpr int WGL_SUPPORT_GDI_ARB = 0x200F;
        constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
        constexpr int WGL_DOUBLE_BUFFER_ARB = 0x2011;
        constexpr int WGL_STEREO_ARB = 0x2012;
        constexpr int WGL_PIXEL_TYPE_ARB = 0x2013;
        constexpr int WGL_COLOR_BITS_ARB = 0x2014;
        constexpr int WGL_RED_BITS_ARB = 0x2015;
        constexpr int WGL_RED_SHIFT_ARB = 0x2016;
        constexpr int WGL_GREEN_BITS_ARB = 0x2017;
        constexpr int WGL_GREEN_SHIFT_ARB = 0x2018;
        constexpr int WGL_BLUE_BITS_ARB = 0x2019;
        constexpr int WGL_BLUE_SHIFT_ARB = 0x201A;
        constexpr int WGL_ALPHA_BITS_ARB = 0x201B;
        constexpr int WGL_ALPHA_SHIFT_ARB = 0x201C;
        constexpr int WGL_ACCUM_BITS_ARB = 0x201D;
        constexpr int WGL_ACCUM_RED_BITS_ARB = 0x201E;
        constexpr int WGL_ACCUM_GREEN_BITS_ARB = 0x201F;
        constexpr int WGL_ACCUM_BLUE_BITS_ARB = 0x2020;
        constexpr int WGL_ACCUM_ALPHA_BITS_ARB = 0x2021;
        constexpr int WGL_DEPTH_BITS_ARB = 0x2022;
        constexpr int WGL_STENCIL_BITS_ARB = 0x2023;
        constexpr int WGL_AUX_BUFFERS_ARB = 0x2024;
        constexpr int WGL_NO_ACCELERATION_ARB = 0x2025;
        constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
        constexpr int WGL_SWAP_EXCHANGE_ARB = 0x2028;
        constexpr int WGL_TYPE_RGBA_ARB = 0x202B;
        // WGL_ARB_multisample
        constexpr int WGL_SAMPLE_BUFFERS_ARB = 0x2041;
        constexpr int WGL_SAMPLES_ARB = 0x2042;
        // WGL_ARB_create_context / _profile / _no_error
        constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
        constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
        constexpr int WGL_CONTEXT_LAYER_PLANE_ARB = 0x2093;
        constexpr int WGL_CONTEXT_FLAGS_ARB = 0x2094;
        constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
        constexpr int WGL_CONTEXT_DEBUG_BIT_ARB = 0x0001;
        constexpr int WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB = 0x0002;
        constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
        constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;
        constexpr int WGL_CONTEXT_OPENGL_NO_ERROR_ARB = 0x31B3;
        constexpr DWORD ERROR_INVALID_VERSION_ARB = 0x2095;
        constexpr DWORD ERROR_INVALID_PROFILE_ARB = 0x2096;

        struct PixelFormatInfo {
            GLint AlphaBits;
            GLint DepthBits;
            GLint StencilBits;
        };

        // Mirrors the two EGLState configs (RGBA8 + depth24, stencil 8 / stencil 0).
        constexpr PixelFormatInfo kPixelFormats[] = {
            {8, 24, 8},
            {8, 24, 0},
        };
        constexpr int kPixelFormatCount = static_cast<int>(std::size(kPixelFormats));

        struct ContextObject {
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLConfig Config = nullptr;
            EGLContext Context = EGL_NO_CONTEXT;
        };

        struct WindowSurface {
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLSurface Surface = EGL_NO_SURFACE;
            Uint32 Width = 0;
            Uint32 Height = 0;
        };

        std::recursive_mutex& RegistryMutex() {
            static auto* mutex = new std::recursive_mutex();
            return *mutex;
        }

        UnorderedMap<HGLRC, ContextObject>& Contexts() {
            static auto* contexts = new UnorderedMap<HGLRC, ContextObject>();
            return *contexts;
        }

        UnorderedMap<HWND, WindowSurface>& WindowSurfaces() {
            static auto* surfaces = new UnorderedMap<HWND, WindowSurface>();
            return *surfaces;
        }

        UnorderedMap<HWND, int>& WindowPixelFormats() {
            static auto* formats = new UnorderedMap<HWND, int>();
            return *formats;
        }

        Uint64& NextContextHandle() {
            static auto* handle = new Uint64(0x10000);
            return *handle;
        }

        struct ThreadCurrent {
            HDC DC = nullptr;
            HGLRC Context = nullptr;
        };
        thread_local ThreadCurrent t_current;

        Int& SwapIntervalShadow() {
            static auto* interval = new Int(1);
            return *interval;
        }

        void EnsureInitialized() {
            // Initialize() loads backend libraries and glslang, which must not run
            // under the loader lock; first WGL call is the earliest safe moment.
            static std::once_flag once;
            std::call_once(once, [] { MobileGL::Initialize(); });
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

        HGLRC EncodeContext(Uint64 handle) {
            return reinterpret_cast<HGLRC>(static_cast<SizeT>(handle));
        }

        ContextObject* TryGetContext(HGLRC hglrc) {
            auto& contexts = Contexts();
            auto it = contexts.find(hglrc);
            return it == contexts.end() ? nullptr : &it->second;
        }

        const PixelFormatInfo& PixelFormatForWindow(HWND hwnd) {
            auto& formats = WindowPixelFormats();
            auto it = formats.find(hwnd);
            int index = it == formats.end() ? 1 : it->second;
            if (index < 1 || index > kPixelFormatCount) {
                index = 1;
            }
            return kPixelFormats[index - 1];
        }

        Bool QueryClientSize(HWND hwnd, Uint32& width, Uint32& height) {
            RECT rect{};
            if (!GetClientRect(hwnd, &rect)) {
                return false;
            }
            width = static_cast<Uint32>(std::max<LONG>(rect.right - rect.left, 1));
            height = static_cast<Uint32>(std::max<LONG>(rect.bottom - rect.top, 1));
            return true;
        }

        // The backends never query the HWND client size themselves; the WGL layer
        // owns size discovery and pushes changes through the internal resize hook
        // (same contract as the macOS CGL layer).
        void SyncSurfaceSize(HWND hwnd, WindowSurface& surface) {
            Uint32 width = 0;
            Uint32 height = 0;
            if (!QueryClientSize(hwnd, width, height)) {
                return;
            }
            if (width == surface.Width && height == surface.Height) {
                return;
            }
            if (EGLImpl::ResizePlatformWindowSurface(surface.Display, surface.Surface,
                                                     static_cast<EGLint>(width), static_cast<EGLint>(height))) {
                surface.Width = width;
                surface.Height = height;
            }
        }

        WindowSurface* EnsureWindowSurface(HWND hwnd, const ContextObject& context) {
            auto& surfaces = WindowSurfaces();
            auto it = surfaces.find(hwnd);
            if (it != surfaces.end()) {
                SyncSurfaceSize(hwnd, it->second);
                return &it->second;
            }

            Uint32 width = 0;
            Uint32 height = 0;
            if (!QueryClientSize(hwnd, width, height)) {
                MGLOG_E("wgl: GetClientRect failed for HWND %p", hwnd);
                return nullptr;
            }

            const EGLAttrib attribs[] = {
                EGL_WIDTH, static_cast<EGLAttrib>(width),
                EGL_HEIGHT, static_cast<EGLAttrib>(height),
                EGL_NONE,
            };
            EGLSurface surface =
                EGLImpl::CreatePlatformWindowSurface(context.Display, context.Config, hwnd, attribs);
            if (surface == EGL_NO_SURFACE) {
                MGLOG_E("wgl: failed to create window surface for HWND %p (%ux%u)", hwnd, width, height);
                return nullptr;
            }

            WindowSurface record;
            record.Display = context.Display;
            record.Surface = surface;
            record.Width = width;
            record.Height = height;
            auto [inserted, _] = surfaces.emplace(hwnd, record);
            return &inserted->second;
        }

        HGLRC CreateContextFromEGLAttribs(HDC hdc, HGLRC share, const EGLint* contextAttribs) {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            EGLDisplay display = EnsureDisplay();
            if (display == EGL_NO_DISPLAY) {
                MGLOG_E("wgl: no EGL display");
                return nullptr;
            }
            EGLImpl::BindAPI(EGL_OPENGL_API);

            EGLContext shareContext = EGL_NO_CONTEXT;
            if (share) {
                auto* shareObject = TryGetContext(share);
                if (!shareObject) {
                    SetLastError(ERROR_INVALID_HANDLE);
                    return nullptr;
                }
                shareContext = shareObject->Context;
            }

            HWND hwnd = WindowFromDC(hdc);
            const PixelFormatInfo& pixelFormat = PixelFormatForWindow(hwnd);
            const EGLint configAttribs[] = {
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, pixelFormat.AlphaBits,
                EGL_DEPTH_SIZE, pixelFormat.DepthBits,
                EGL_STENCIL_SIZE, pixelFormat.StencilBits,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_NONE,
            };
            EGLConfig config = nullptr;
            EGLint configCount = 0;
            if (!EGLImpl::ChooseConfig(display, configAttribs, &config, 1, &configCount) || configCount <= 0) {
                MGLOG_E("wgl: eglChooseConfig failed");
                return nullptr;
            }

            EGLContext eglContext = EGLImpl::CreateContext(display, config, shareContext, contextAttribs);
            if (eglContext == EGL_NO_CONTEXT) {
                MGLOG_E("wgl: eglCreateContext failed");
                return nullptr;
            }

            ContextObject object;
            object.Display = display;
            object.Config = config;
            object.Context = eglContext;
            const auto handle = EncodeContext(NextContextHandle()++);
            Contexts()[handle] = object;
            MGLOG_I("wgl: created context %p (EGL context %p)", handle, eglContext);
            return handle;
        }

        // ---- WGL extension entry points (resolved via wglGetProcAddress only) ----

        const char* WINAPI Ext_GetExtensionsStringARB(HDC) {
            return "WGL_ARB_create_context WGL_ARB_create_context_no_error WGL_ARB_create_context_profile "
                   "WGL_ARB_extensions_string WGL_ARB_pixel_format WGL_EXT_extensions_string WGL_EXT_swap_control";
        }

        const char* WINAPI Ext_GetExtensionsStringEXT() {
            return Ext_GetExtensionsStringARB(nullptr);
        }

        HGLRC WINAPI Ext_CreateContextAttribsARB(HDC hdc, HGLRC hShareContext, const int* attribList) {
            EnsureInitialized();
            int major = 1;
            int minor = 0;
            int profileMask = 0;
            int flags = 0;
            if (attribList) {
                for (SizeT i = 0; attribList[i] != 0; i += 2) {
                    const int attrib = attribList[i];
                    const int value = attribList[i + 1];
                    switch (attrib) {
                    case WGL_CONTEXT_MAJOR_VERSION_ARB:
                        major = value;
                        break;
                    case WGL_CONTEXT_MINOR_VERSION_ARB:
                        minor = value;
                        break;
                    case WGL_CONTEXT_PROFILE_MASK_ARB:
                        profileMask = value;
                        break;
                    case WGL_CONTEXT_FLAGS_ARB:
                        flags = value;
                        break;
                    case WGL_CONTEXT_LAYER_PLANE_ARB:
                        if (value != 0) {
                            SetLastError(ERROR_INVALID_PARAMETER);
                            return nullptr;
                        }
                        break;
                    case WGL_CONTEXT_OPENGL_NO_ERROR_ARB:
                        // Accepted and ignored: MobileGL always validates.
                        break;
                    default:
                        MGLOG_D("wglCreateContextAttribsARB: ignoring attrib 0x%04x = 0x%x", attrib, value);
                        break;
                    }
                }
            }

            if (major < 1 || (profileMask & ~(WGL_CONTEXT_CORE_PROFILE_BIT_ARB |
                                              WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB))) {
                SetLastError(profileMask ? ERROR_INVALID_PROFILE_ARB : ERROR_INVALID_VERSION_ARB);
                return nullptr;
            }

            Vector<EGLint> attribs = {
                EGL_CONTEXT_MAJOR_VERSION, major,
                EGL_CONTEXT_MINOR_VERSION, minor,
            };
            const Bool wantsCompat = (profileMask & WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB) != 0;
            if (major > 3 || (major == 3 && minor >= 2) || profileMask != 0) {
                attribs.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK);
                attribs.push_back(wantsCompat ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT
                                              : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT);
            }
            if (flags & WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) {
                attribs.push_back(EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE);
                attribs.push_back(EGL_TRUE);
            }
            if (flags & WGL_CONTEXT_DEBUG_BIT_ARB) {
                attribs.push_back(EGL_CONTEXT_OPENGL_DEBUG);
                attribs.push_back(EGL_TRUE);
            }
            attribs.push_back(EGL_NONE);

            return CreateContextFromEGLAttribs(hdc, hShareContext, attribs.data());
        }

        BOOL WINAPI Ext_SwapIntervalEXT(int interval) {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            EGLDisplay display = EnsureDisplay();
            if (display == EGL_NO_DISPLAY) {
                return FALSE;
            }
            if (interval < 0) {
                // Adaptive vsync is not supported; clamp to regular vsync.
                interval = 1;
            }
            EGLImpl::SwapInterval(display, interval);
            SwapIntervalShadow() = interval;
            return TRUE;
        }

        int WINAPI Ext_GetSwapIntervalEXT() {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            return SwapIntervalShadow();
        }

        int PixelFormatAttribValue(int format, int attrib) {
            const PixelFormatInfo& info = kPixelFormats[format - 1];
            switch (attrib) {
            case WGL_NUMBER_PIXEL_FORMATS_ARB:
                return kPixelFormatCount;
            case WGL_SUPPORT_OPENGL_ARB:
            case WGL_DRAW_TO_WINDOW_ARB:
            case WGL_DOUBLE_BUFFER_ARB:
                return 1;
            case WGL_ACCELERATION_ARB:
                return WGL_FULL_ACCELERATION_ARB;
            case WGL_PIXEL_TYPE_ARB:
                return WGL_TYPE_RGBA_ARB;
            case WGL_COLOR_BITS_ARB:
                return 32;
            case WGL_RED_BITS_ARB:
            case WGL_GREEN_BITS_ARB:
            case WGL_BLUE_BITS_ARB:
                return 8;
            case WGL_RED_SHIFT_ARB:
                return 16;
            case WGL_GREEN_SHIFT_ARB:
                return 8;
            case WGL_BLUE_SHIFT_ARB:
                return 0;
            case WGL_ALPHA_BITS_ARB:
                return info.AlphaBits;
            case WGL_ALPHA_SHIFT_ARB:
                return 24;
            case WGL_DEPTH_BITS_ARB:
                return info.DepthBits;
            case WGL_STENCIL_BITS_ARB:
                return info.StencilBits;
            case WGL_SWAP_METHOD_ARB:
                return WGL_SWAP_EXCHANGE_ARB;
            case WGL_DRAW_TO_BITMAP_ARB:
            case WGL_NEED_PALETTE_ARB:
            case WGL_NEED_SYSTEM_PALETTE_ARB:
            case WGL_SWAP_LAYER_BUFFERS_ARB:
            case WGL_NUMBER_OVERLAYS_ARB:
            case WGL_NUMBER_UNDERLAYS_ARB:
            case WGL_TRANSPARENT_ARB:
            case WGL_SHARE_DEPTH_ARB:
            case WGL_SHARE_STENCIL_ARB:
            case WGL_SHARE_ACCUM_ARB:
            case WGL_SUPPORT_GDI_ARB:
            case WGL_STEREO_ARB:
            case WGL_ACCUM_BITS_ARB:
            case WGL_ACCUM_RED_BITS_ARB:
            case WGL_ACCUM_GREEN_BITS_ARB:
            case WGL_ACCUM_BLUE_BITS_ARB:
            case WGL_ACCUM_ALPHA_BITS_ARB:
            case WGL_AUX_BUFFERS_ARB:
            case WGL_SAMPLE_BUFFERS_ARB:
            case WGL_SAMPLES_ARB:
            default:
                return 0;
            }
        }

        BOOL WINAPI Ext_GetPixelFormatAttribivARB(HDC, int iPixelFormat, int iLayerPlane, UINT nAttributes,
                                                  const int* piAttributes, int* piValues) {
            if (iLayerPlane != 0 || !piAttributes || !piValues) {
                return FALSE;
            }
            // Format 0 is only valid for WGL_NUMBER_PIXEL_FORMATS_ARB queries.
            if (iPixelFormat < 0 || iPixelFormat > kPixelFormatCount) {
                return FALSE;
            }
            const int format = iPixelFormat == 0 ? 1 : iPixelFormat;
            for (UINT i = 0; i < nAttributes; ++i) {
                piValues[i] = PixelFormatAttribValue(format, piAttributes[i]);
            }
            return TRUE;
        }

        BOOL WINAPI Ext_GetPixelFormatAttribfvARB(HDC hdc, int iPixelFormat, int iLayerPlane, UINT nAttributes,
                                                  const int* piAttributes, FLOAT* pfValues) {
            if (!pfValues) {
                return FALSE;
            }
            Vector<int> values(nAttributes);
            if (!Ext_GetPixelFormatAttribivARB(hdc, iPixelFormat, iLayerPlane, nAttributes, piAttributes,
                                               values.data())) {
                return FALSE;
            }
            for (UINT i = 0; i < nAttributes; ++i) {
                pfValues[i] = static_cast<FLOAT>(values[i]);
            }
            return TRUE;
        }

        BOOL WINAPI Ext_ChoosePixelFormatARB(HDC, const int* piAttribIList, const FLOAT*, UINT nMaxFormats,
                                             int* piFormats, UINT* nNumFormats) {
            if (!piFormats || !nNumFormats) {
                return FALSE;
            }
            int wantedStencil = 0;
            if (piAttribIList) {
                for (SizeT i = 0; piAttribIList[i] != 0; i += 2) {
                    if (piAttribIList[i] == WGL_STENCIL_BITS_ARB) {
                        wantedStencil = piAttribIList[i + 1];
                    }
                }
            }
            UINT count = 0;
            const int preferred = wantedStencil > 0 ? 1 : 2;
            const int fallback = wantedStencil > 0 ? 2 : 1;
            if (count < nMaxFormats) {
                piFormats[count++] = preferred;
            }
            if (count < nMaxFormats) {
                piFormats[count++] = fallback;
            }
            *nNumFormats = count;
            return TRUE;
        }

        struct WGLExtensionProc {
            const char* Name;
            PROC Proc;
        };

        const WGLExtensionProc kWGLExtensionProcs[] = {
            {"wglGetExtensionsStringARB", reinterpret_cast<PROC>(Ext_GetExtensionsStringARB)},
            {"wglGetExtensionsStringEXT", reinterpret_cast<PROC>(Ext_GetExtensionsStringEXT)},
            {"wglCreateContextAttribsARB", reinterpret_cast<PROC>(Ext_CreateContextAttribsARB)},
            {"wglSwapIntervalEXT", reinterpret_cast<PROC>(Ext_SwapIntervalEXT)},
            {"wglGetSwapIntervalEXT", reinterpret_cast<PROC>(Ext_GetSwapIntervalEXT)},
            {"wglGetPixelFormatAttribivARB", reinterpret_cast<PROC>(Ext_GetPixelFormatAttribivARB)},
            {"wglGetPixelFormatAttribfvARB", reinterpret_cast<PROC>(Ext_GetPixelFormatAttribfvARB)},
            {"wglChoosePixelFormatARB", reinterpret_cast<PROC>(Ext_ChoosePixelFormatARB)},
        };
    } // namespace

    int ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR* pfd) {
        EnsureInitialized();
        MGLOG_D("wglChoosePixelFormat(hdc=%p)", hdc);
        // Format 1 (RGBA8 + depth24/stencil8) satisfies every request; a format
        // exceeding the asked-for capabilities is a legal ChoosePixelFormat answer.
        (void)pfd;
        return 1;
    }

    int DescribePixelFormat(HDC hdc, int format, UINT size, PIXELFORMATDESCRIPTOR* pfd) {
        EnsureInitialized();
        MGLOG_D("wglDescribePixelFormat(hdc=%p, format=%d)", hdc, format);
        if (!pfd) {
            return kPixelFormatCount;
        }
        if (size < sizeof(PIXELFORMATDESCRIPTOR) || format < 1 || format > kPixelFormatCount) {
            return 0;
        }

        const PixelFormatInfo& info = kPixelFormats[format - 1];
        std::memset(pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
        pfd->nSize = sizeof(PIXELFORMATDESCRIPTOR);
        pfd->nVersion = 1;
        pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_EXCHANGE
#if defined(PFD_SUPPORT_COMPOSITION)
                       | PFD_SUPPORT_COMPOSITION
#endif
            ;
        pfd->iPixelType = PFD_TYPE_RGBA;
        pfd->cColorBits = 32;
        pfd->cRedBits = 8;
        pfd->cRedShift = 16;
        pfd->cGreenBits = 8;
        pfd->cGreenShift = 8;
        pfd->cBlueBits = 8;
        pfd->cBlueShift = 0;
        pfd->cAlphaBits = static_cast<BYTE>(info.AlphaBits);
        pfd->cAlphaShift = 24;
        pfd->cDepthBits = static_cast<BYTE>(info.DepthBits);
        pfd->cStencilBits = static_cast<BYTE>(info.StencilBits);
        pfd->iLayerType = PFD_MAIN_PLANE;
        return kPixelFormatCount;
    }

    int GetPixelFormat(HDC hdc) {
        EnsureInitialized();
        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd) {
            return 0;
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto& formats = WindowPixelFormats();
        auto it = formats.find(hwnd);
        return it == formats.end() ? 0 : it->second;
    }

    BOOL SetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR*) {
        EnsureInitialized();
        MGLOG_D("wglSetPixelFormat(hdc=%p, format=%d)", hdc, format);
        if (format < 1 || format > kPixelFormatCount) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        WindowPixelFormats()[hwnd] = format;
        return TRUE;
    }

    BOOL SwapBuffers(HDC hdc) {
        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto& surfaces = WindowSurfaces();
        auto it = surfaces.find(hwnd);
        if (it == surfaces.end()) {
            MGLOG_W("wglSwapBuffers: no surface for HWND %p", hwnd);
            return FALSE;
        }
        SyncSurfaceSize(hwnd, it->second);
        return EGLImpl::SwapBuffers(it->second.Display, it->second.Surface) == EGL_TRUE ? TRUE : FALSE;
    }

    HGLRC CreateContext(HDC hdc) {
        EnsureInitialized();
        MGLOG_I("wglCreateContext(hdc=%p)", hdc);
        // A legacy WGL context is a compatibility-profile context; MobileGL keys
        // its relaxed-semantics mode off the explicit compatibility bit.
        const EGLint attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        return CreateContextFromEGLAttribs(hdc, nullptr, attribs);
    }

    BOOL DeleteContext(HGLRC hglrc) {
        EnsureInitialized();
        MGLOG_I("wglDeleteContext(%p)", hglrc);
        if (t_current.Context == hglrc) {
            MakeCurrent(nullptr, nullptr);
        }
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(hglrc);
        if (!object) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        if (object->Context != EGL_NO_CONTEXT) {
            EGLImpl::DestroyContext(object->Display, object->Context);
        }
        Contexts().erase(hglrc);
        return TRUE;
    }

    BOOL MakeCurrent(HDC hdc, HGLRC hglrc) {
        EnsureInitialized();
        MGLOG_D("wglMakeCurrent(hdc=%p, hglrc=%p)", hdc, hglrc);
        if (!hglrc) {
            if (!t_current.Context) {
                t_current = {};
                return TRUE;
            }
            const EGLBoolean released =
                EGLImpl::MakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            t_current = {};
            return released == EGL_TRUE ? TRUE : FALSE;
        }

        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        auto* object = TryGetContext(hglrc);
        if (!object) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }

        WindowSurface* surface = EnsureWindowSurface(hwnd, *object);
        if (!surface) {
            return FALSE;
        }

        if (!EGLImpl::MakeCurrent(object->Display, surface->Surface, surface->Surface, object->Context)) {
            MGLOG_E("wglMakeCurrent: eglMakeCurrent failed (hdc=%p, hglrc=%p)", hdc, hglrc);
            return FALSE;
        }
        t_current = {hdc, hglrc};
        return TRUE;
    }

    HGLRC GetCurrentContext() {
        return t_current.Context;
    }

    HDC GetCurrentDC() {
        return t_current.DC;
    }

    BOOL ShareLists(HGLRC hglrcShare, HGLRC hglrcDest) {
        // All MobileGL contexts alias one global GL object namespace, so every
        // pair of contexts already shares.
        const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        if (!TryGetContext(hglrcShare) || !TryGetContext(hglrcDest)) {
            SetLastError(ERROR_INVALID_HANDLE);
            return FALSE;
        }
        return TRUE;
    }

    PROC GetProcAddress(const char* name) {
        EnsureInitialized();
        if (!name) {
            return nullptr;
        }
        if (name[0] == 'w' && name[1] == 'g' && name[2] == 'l') {
            for (const auto& entry : kWGLExtensionProcs) {
                if (std::strcmp(entry.Name, name) == 0) {
                    return entry.Proc;
                }
            }
            MGLOG_D("wglGetProcAddress: unknown wgl entry point %s", name);
            return nullptr;
        }
        return reinterpret_cast<PROC>(MG_Impl::GetProcAddress(name));
    }
} // namespace MobileGL::MG_Impl::WGLImpl

#endif // _WIN32
