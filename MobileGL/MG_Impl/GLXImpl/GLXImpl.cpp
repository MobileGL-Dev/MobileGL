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
#include "../EGLImpl/WaylandWindow.h"
#include "../GetProcAddress.h"
#include "X11Present.h"
#include "X11PresentXcb.h"
#include <Init.h>
#include <Config.h>
#include <MG_Backend/BackendObjects.h>

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
        // GLX_EXT_buffer_age
        constexpr int GLX_BACK_BUFFER_AGE_EXT = 0x20F4;
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
        // The server answers its surfaces' buffer age (EGL_EXT_buffer_age) when it has shared images.
        constexpr const char* kGLXExtensionsWithBufferAge =
            "GLX_ARB_create_context GLX_ARB_create_context_no_error GLX_ARB_create_context_profile "
            "GLX_ARB_get_proc_address GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control "
            "GLX_EXT_buffer_age";

        const char* GLXExtensions() {
            return EGLImpl::SharedImagesAvailable() ? kGLXExtensionsWithBufferAge : kGLXExtensions;
        }

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
                    MGLOG_E_ONCE("glx: failed to load libX11 entry points");
                }
                return fns;
            }();
            return *functions;
        }

        // ---- X11 presentation of a remote drawable (libxcb through libX11's own connection,
        //      dlopen'd like the rest; xcb's request is plain arguments, no struct layouts) ----
        struct XcbVoidCookie {
            unsigned int Sequence;
        };
        struct XcbFunctions {
            void* (*GetXCBConnection)(Display*) = nullptr;
            Uint32 (*GenerateId)(void*) = nullptr;
            XcbVoidCookie (*CreateGC)(void*, Uint32, Uint32, Uint32, const void*) = nullptr;
            XcbVoidCookie (*FreeGC)(void*, Uint32) = nullptr;
            XcbVoidCookie (*PutImage)(void*, Uint8, Uint32, Uint32, Uint16, Uint16, Int16, Int16, Uint8, Uint8,
                                      Uint32, const Uint8*) = nullptr;
            Uint32 (*MaximumRequestLength)(void*) = nullptr;
            int (*Flush)(void*) = nullptr;

            Bool Valid() const {
                return GetXCBConnection && GenerateId && CreateGC && FreeGC && PutImage && MaximumRequestLength &&
                       Flush;
            }
        };

        const XcbFunctions& Xcb() {
            static const XcbFunctions* functions = [] {
                auto* fns = new XcbFunctions();
                void* x11xcb = dlopen("libX11-xcb.so.1", RTLD_LOCAL | RTLD_NOW);
                void* xcb = dlopen("libxcb.so.1", RTLD_LOCAL | RTLD_NOW);
                if (x11xcb && xcb) {
                    fns->GetXCBConnection =
                        reinterpret_cast<decltype(fns->GetXCBConnection)>(dlsym(x11xcb, "XGetXCBConnection"));
                    fns->GenerateId = reinterpret_cast<decltype(fns->GenerateId)>(dlsym(xcb, "xcb_generate_id"));
                    fns->CreateGC = reinterpret_cast<decltype(fns->CreateGC)>(dlsym(xcb, "xcb_create_gc"));
                    fns->FreeGC = reinterpret_cast<decltype(fns->FreeGC)>(dlsym(xcb, "xcb_free_gc"));
                    fns->PutImage = reinterpret_cast<decltype(fns->PutImage)>(dlsym(xcb, "xcb_put_image"));
                    fns->MaximumRequestLength = reinterpret_cast<decltype(fns->MaximumRequestLength)>(
                        dlsym(xcb, "xcb_get_maximum_request_length"));
                    fns->Flush = reinterpret_cast<decltype(fns->Flush)>(dlsym(xcb, "xcb_flush"));
                }
                if (!fns->Valid()) {
                    MGLOG_E_ONCE("glx: libX11-xcb/libxcb entry points missing; remote windows are not shown");
                }
                return fns;
            }();
            return *functions;
        }

        constexpr Uint8 kXcbImageFormatZPixmap = 2;

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
            ::Display* XDisplay = nullptr;
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLConfig Config = nullptr;
            EGLContext Context = EGL_NO_CONTEXT;
            const FBConfigInfo* FBConfig = nullptr;
        };

        // The server's shared images, as X11Present's chain uses them (the GL stream's verbs: the
        // caller holds the stream lock, as every GLX entry point does).
        class BackendImageSource final : public X11Present::ImageSource {
        public:
            Bool Allocate(Uint32 width, Uint32 height, Uint32 fourcc, X11Present::Image* out) override {
                auto* backend = MG_Backend::pActiveBackendObject.get();
                MG_Backend::SharedImageExport image;
                if (backend == nullptr || !backend->AllocateSharedImage(width, height, fourcc, &image)) return false;
                out->Id = image.Id;
                out->Fd = image.Fd;
                out->Width = image.Width;
                out->Height = image.Height;
                out->Stride = image.Stride;
                out->Offset = image.Offset;
                out->Fourcc = image.Fourcc;
                out->Modifier = image.Modifier;
                return true;
            }
            void Release(Uint64 id) override {
                if (auto* backend = MG_Backend::pActiveBackendObject.get()) (void)backend->ReleaseSharedImage(id);
            }
            Bool CopyFrame(Uint64 id, const MG_Util::Damage::Region& region) override {
                auto* backend = MG_Backend::pActiveBackendObject.get();
                return backend != nullptr && backend->PresentToSharedImage(id, region);
            }
        };

        struct DrawableSurface {
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLSurface Surface = EGL_NO_SURFACE;
            Uint32 Width = 0;
            Uint32 Height = 0;
            std::chrono::steady_clock::time_point LastSizePoll{};
            Bool RemotePbuffer = false;
            // A remote drawable's presentation: the window's depth, the GC its frames are put
            // with (0 until the first one) and the readback buffers.
            ::Display* XDisplay = nullptr;
            Uint32 Depth = 0;
            Uint32 Gc = 0;
            Vector<Uint8> Scratch;
            Vector<Uint8> Image;
            // How its frames reach the window (chosen at the first swap, X11Present::SelectPath):
            // DRI3+Present shared images, or the readback put with MIT-SHM or plain PutImage.
            Bool PathChosen = false;
            X11Present::Path Path = X11Present::Path::PutImage;
            UniquePtr<X11Present::Connection> PresentConnection;
            UniquePtr<BackendImageSource> Images;
            UniquePtr<X11Present::Chain> Chain;
            UniquePtr<X11Present::ShmPresenter> Shm;
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

        // vblank_mode=0 (the benchmark convention X11 GL drivers honour) never waits for the
        // display, whatever the application asks; anything else leaves the interval to it.
        Bool VblankForcedOff() {
            static const Bool off = [] {
                const char* mode = std::getenv("vblank_mode");
                return mode != nullptr && std::strcmp(mode, "0") == 0;
            }();
            return off;
        }

        Int& SwapIntervalShadow() {
            static auto* interval = new Int(VblankForcedOff() ? 0 : 1);
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

        Bool QueryDrawableSize(Display* dpy, GLXDrawableHandle drawable, Uint32& width, Uint32& height,
                               Uint32* depthOut = nullptr) {
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
            if (depthOut) *depthOut = depth;
            return true;
        }

        Int& SwapIntervalShadow();

        Int64 SteadyNowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // Which way this drawable's frames go, decided once (X11Present::SelectPath), with the
        // DRI3 chain set up when that is the way. A chain that cannot be set up falls through to
        // the readback paths.
        void ChoosePresentPath(GLXDrawableHandle drawable, DrawableSurface& surface, void* connection) {
            surface.PathChosen = true;
            X11Present::ConnectionCaps caps = X11Present::QueryConnectionCaps(connection);
            caps.SharedImages = EGLImpl::SharedImagesAvailable();
            const char* override = std::getenv("MOBILEGL_GLX_PRESENT");
            surface.Path = X11Present::SelectPath(caps, surface.Depth, override);
            if (surface.Path == X11Present::Path::Dri3Present) {
                surface.PresentConnection =
                    X11Present::CreateXcbConnection(connection, static_cast<Uint32>(drawable), caps);
                if (surface.PresentConnection) {
                    surface.Images = MakeUnique<BackendImageSource>();
                    surface.Chain = MakeUnique<X11Present::Chain>(
                        *surface.PresentConnection, *surface.Images, surface.Depth, static_cast<Int32>(surface.Width),
                        static_cast<Int32>(surface.Height), &SteadyNowMs);
                } else {
                    caps.Dri3Major = 0;
                    surface.Path = X11Present::SelectPath(caps, surface.Depth, override);
                }
            }
            if (surface.Path == X11Present::Path::ShmPutImage) {
                surface.Shm = X11Present::CreateShmPresenter(connection);
                if (!surface.Shm) surface.Path = X11Present::Path::PutImage;
            }
            MGLOG_I("glx: window 0x%lx (%ux%u, depth %u) presented through %s (DRI3 %u.%u, Present %u.%u, MIT-SHM "
                    "%u.%u, %s connection, shared images %s)",
                    drawable, surface.Width, surface.Height, surface.Depth, X11Present::PathName(surface.Path),
                    caps.Dri3Major, caps.Dri3Minor, caps.PresentMajor, caps.PresentMinor, caps.ShmMajor, caps.ShmMinor,
                    caps.LocalConnection ? "local" : "remote", caps.SharedImages ? "yes" : "no");
        }

        // The chain's resources go (pixmaps freed, images released); the drawable keeps whatever
        // readback path is left.
        void DropChain(DrawableSurface& surface) {
            surface.Chain.reset();
            surface.PresentConnection.reset();
            surface.Images.reset();
        }

        // A REMOTE DRAWABLE IS A PBUFFER, AND NOTHING OF IT REACHES THE X WINDOW BY ITSELF: the
        // backend draws in another process (another OS), where an XID means nothing. So at every
        // swap the frame goes to the window one of three ways (X11Present.h): copied GPU-side into
        // a shared image the X server has as a DRI3 pixmap and presented with PresentPixmap - no
        // CPU copy at all; or read back and put with MIT-SHM; or read back and put with PutImage,
        // the same contract as the Wayland windows' wl_shm presentation. Called with the drawable
        // current.
        void PresentRemoteDrawable(GLXDrawableHandle drawable, DrawableSurface& surface) {
            const EGLImpl::GLStreamScope stream; // the readback is GL records of this thread's context
            const auto& xcb = Xcb();
            if (!xcb.Valid() || !surface.XDisplay) return;
            if (surface.Depth != 24 && surface.Depth != 32) {
                MGLOG_W_ONCE("glx: a depth-%u window cannot be shown (24/32-bit only)", surface.Depth);
                return;
            }
            void* connection = xcb.GetXCBConnection(surface.XDisplay);
            if (!connection) return;
            if (!surface.PathChosen) ChoosePresentPath(drawable, surface, connection);
            if (surface.Chain) {
                if (surface.Chain->Present(MG_Util::Damage::Region::Full(), SwapIntervalShadow())) return;
                if (!surface.Chain->Broken()) return; // the frame is dropped; the chain goes on
                MGLOG_I("glx: window 0x%lx leaves DRI3+Present: %s; its frames are read back from now on", drawable,
                        surface.Chain->BrokenReason());
                DropChain(surface);
                X11Present::ConnectionCaps caps = X11Present::QueryConnectionCaps(connection);
                surface.Path = X11Present::SelectPath(caps, surface.Depth, "readback");
                if (surface.Path == X11Present::Path::ShmPutImage) {
                    surface.Shm = X11Present::CreateShmPresenter(connection);
                    if (!surface.Shm) surface.Path = X11Present::Path::PutImage;
                }
            }
            if (surface.Gc == 0) {
                surface.Gc = xcb.GenerateId(connection);
                (void)xcb.CreateGC(connection, surface.Gc, static_cast<Uint32>(drawable), 0, nullptr);
            }
            const EGLint width = static_cast<EGLint>(surface.Width);
            const EGLint height = static_cast<EGLint>(surface.Height);
            const SizeT rowBytes = static_cast<SizeT>(width) * 4;
            if (surface.Shm) {
                const Bool put = surface.Shm->Put(
                    static_cast<Uint32>(drawable), surface.Gc, surface.Depth, width, height,
                    [&](Uint8* rows, SizeT stride) { EGLImpl::ReadBackFrameBGRA(width, height, surface.Scratch, rows, stride); });
                if (put) return;
                MGLOG_I("glx: window 0x%lx: MIT-SHM put failed; PutImage from now on", drawable);
                surface.Shm.reset();
                surface.Path = X11Present::Path::PutImage;
            }
            surface.Image.resize(rowBytes * static_cast<SizeT>(height));
            EGLImpl::ReadBackFrameBGRA(width, height, surface.Scratch, surface.Image.data(), rowBytes);

            // A request over the server's maximum length closes the connection, so the image goes
            // in bands of whole rows that each fit (the request header is 24 bytes).
            const SizeT maxBytes = static_cast<SizeT>(xcb.MaximumRequestLength(connection)) * 4;
            const SizeT budget = maxBytes > 64 ? maxBytes - 64 : rowBytes;
            const EGLint bandRows = std::max<EGLint>(1, static_cast<EGLint>(budget / rowBytes));
            for (EGLint y = 0; y < height; y += bandRows) {
                const EGLint rows = std::min(bandRows, height - y);
                (void)xcb.PutImage(connection, kXcbImageFormatZPixmap, static_cast<Uint32>(drawable), surface.Gc,
                                   static_cast<Uint16>(width), static_cast<Uint16>(rows), 0, static_cast<Int16>(y), 0,
                                   static_cast<Uint8>(surface.Depth), static_cast<Uint32>(rowBytes * rows),
                                   surface.Image.data() + rowBytes * static_cast<SizeT>(y));
            }
            (void)xcb.Flush(connection);
        }

        void ReleaseDrawableSurface(DrawableSurface& surface) {
            DropChain(surface);
            surface.Shm.reset();
            if (surface.Gc != 0 && surface.XDisplay) {
                const auto& xcb = Xcb();
                if (void* connection = xcb.Valid() ? xcb.GetXCBConnection(surface.XDisplay) : nullptr) {
                    (void)xcb.FreeGC(connection, surface.Gc);
                }
            }
            EGLImpl::DestroySurface(surface.Display, surface.Surface);
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
            // A remote X drawable is a pbuffer; resizing it gives it a new one at the new size.
            if (EGLImpl::ResizePlatformWindowSurface(surface.Display, surface.Surface,
                                                     static_cast<EGLint>(width),
                                                     static_cast<EGLint>(height))) {
                surface.Width = width;
                surface.Height = height;
                // The shared images follow: the next frame goes into ones of the new size.
                if (surface.Chain) surface.Chain->Resize(static_cast<Int32>(width), static_cast<Int32>(height));
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
            Uint32 depth = 0;
            if (!QueryDrawableSize(dpy, drawable, width, height, &depth)) {
                MGLOG_E_ONCE("glx: XGetGeometry failed for drawable 0x%lx", drawable);
                return nullptr;
            }

            const EGLAttrib attribs[] = {
                EGL_WIDTH, static_cast<EGLAttrib>(width),
                EGL_HEIGHT, static_cast<EGLAttrib>(height),
                EGL_NONE,
            };
            const Bool remotePbuffer = MG_Config::Transport == MG_Config::TransportMode::Spawn ||
                                       MG_Config::Transport == MG_Config::TransportMode::UnixSocket;
            const EGLint pbufferAttribs[] = {EGL_WIDTH, static_cast<EGLint>(width),
                                             EGL_HEIGHT, static_cast<EGLint>(height), EGL_NONE};
            EGLSurface surface = remotePbuffer
                ? EGLImpl::CreatePbufferSurface(context.Display, context.Config, pbufferAttribs)
                : EGLImpl::CreatePlatformWindowSurface(
                    context.Display, context.Config, reinterpret_cast<void*>(drawable), attribs);
            if (surface == EGL_NO_SURFACE) {
                MGLOG_E_ONCE("glx: failed to create window surface for drawable 0x%lx (%ux%u)", drawable,
                        width, height);
                return nullptr;
            }

            DrawableSurface record;
            record.Display = context.Display;
            record.Surface = surface;
            record.Width = width;
            record.Height = height;
            record.RemotePbuffer = remotePbuffer;
            record.XDisplay = dpy;
            record.Depth = depth;
            record.LastSizePoll = std::chrono::steady_clock::now();
            auto [inserted, _] = surfaces.emplace(drawable, std::move(record));
            return &inserted->second;
        }

        GLXContextHandle CreateContextFromEGLAttribs(Display* dpy, const FBConfigInfo* fbconfig,
                                                     GLXContextHandle share,
                                                     const EGLint* contextAttribs) {
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            EGLDisplay display = EnsureDisplay();
            if (display == EGL_NO_DISPLAY) {
                MGLOG_E_ONCE("glx: no EGL display");
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
                MGLOG_E_ONCE("glx: eglChooseConfig failed");
                return nullptr;
            }

            EGLContext eglContext = EGLImpl::CreateContext(display, config, shareContext, contextAttribs);
            if (eglContext == EGL_NO_CONTEXT) {
                MGLOG_E_ONCE("glx: eglCreateContext failed");
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
        return GLXExtensions();
    }

    const char* GetClientString(Display*, int name) {
        switch (name) {
        case GLX_VENDOR:
            return "MobileGL";
        case GLX_VERSION:
            return "1.4";
        case GLX_EXTENSIONS:
            return GLXExtensions();
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
            MGLOG_E_ONCE("glx: eglMakeCurrent failed (drawable=0x%lx, ctx=%p)", drawable, context);
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
            MGLOG_W_ONCE("glx: glXMakeContextCurrent draw 0x%lx != read 0x%lx, using draw for both", draw,
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
            MGLOG_W_ONCE("glx: glXSwapBuffers with no surface for drawable 0x%lx", drawable);
            return;
        }
        // What the window shows is the frame the swap finishes, read back while it is still the
        // drawable's content (only from the thread it is current to: the readback is a GL call).
        if (it->second.RemotePbuffer && t_current.Draw == drawable && t_current.Context) {
            PresentRemoteDrawable(drawable, it->second);
        }
        EGLImpl::SwapBuffers(it->second.Display, it->second.Surface);
        // A resize takes effect for the next frame: at once when Present reported one, else at the
        // next throttled geometry poll.
        Int32 configuredWidth = 0;
        Int32 configuredHeight = 0;
        const Bool configured =
            it->second.Chain && it->second.Chain->TakeConfiguredSize(&configuredWidth, &configuredHeight) &&
            (static_cast<Uint32>(configuredWidth) != it->second.Width ||
             static_cast<Uint32>(configuredHeight) != it->second.Height);
        SyncSurfaceSize(dpy, drawable, it->second, configured);
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
        ReleaseDrawableSurface(it->second);
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
        case GLX_BACK_BUFFER_AGE_EXT: {
            // GLX_EXT_buffer_age: the age of the buffer the drawable's next frame draws into - the
            // server's pbuffer, which keeps its content - asked of the EGL surface behind it, which
            // answers only while it is the calling thread's draw surface.
            *value = 0;
            const std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
            auto& surfaces = DrawableSurfaces();
            auto it = surfaces.find(drawable);
            if (it == surfaces.end() || t_current.Draw != drawable) return;
            EGLint age = 0;
            if (EGLImpl::QuerySurface(it->second.Display, it->second.Surface, EGL_BUFFER_AGE_EXT, &age) && age > 0) {
                *value = static_cast<unsigned int>(age);
            }
            return;
        }
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
        interval = VblankForcedOff() ? 0 : std::clamp(interval, 0, 4);
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
