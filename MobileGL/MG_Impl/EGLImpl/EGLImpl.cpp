// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EGLImpl.h"
#include "EGLPlatformExtensions.h"
#include "../GetProcAddress.h"
#include <Init.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/EGLState/Core.h>
#if MOBILEGL_BUILD_DISAGGREGATED
// MG_Config::ServerOwnedWindowSurfaces (P12, MOBILEGL_IPC_SURFACE). Split-only: the pull build's
// translation unit is unchanged (G1).
#include <Config.h>
#endif
#include <mutex>
#include <sstream>
#include <type_traits>

// The KHR spellings of the image entry points, exported by Exporting/Definitions.cpp.  Declared
// here because the bundled EGL headers declare them only under EGL_EGLEXT_PROTOTYPES, which no
// build of this library defines, and because eglGetProcAddress below answers them by name.
extern "C" EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                         const EGLint* attrib_list);
extern "C" EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImageKHR image);

// The swap-and-sync family the compositor resolves by name while bringing its EGL backend up; defined
// in Exporting/Definitions.cpp and answered from GetProcAddress below (see the definitions).
extern "C" EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, const EGLint* rects,
                                                  EGLint n_rects);
extern "C" EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface surface, const EGLint* rects,
                                                  EGLint n_rects);
extern "C" EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint* attrib_list);
extern "C" EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync);
extern "C" EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
extern "C" EGLBoolean eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags);
extern "C" EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint* value);
// THE DEVICE-QUERY FAMILY, exported by Exporting/Definitions.cpp and answered by name below: kwin
// reaches eglQueryDisplayAttribEXT through libepoxy's pointer (that is how finding a render device
// starts), and libepoxy asks for it by name.  See the definitions for why these had to exist.
extern "C" EGLBoolean eglQueryDevicesEXT(EGLint max_devices, EGLDeviceEXT* devices, EGLint* num_devices);
extern "C" EGLBoolean eglQueryDeviceAttribEXT(EGLDeviceEXT device, EGLint attribute, EGLAttrib* value);
extern "C" const char* eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name);
extern "C" EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib* value);
extern "C" EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_window,
                                                        const EGLAttrib* attrib_list);
extern "C" EGLSurface eglCreatePlatformPixmapSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                                        const EGLAttrib* attrib_list);

namespace MobileGL::MG_Impl::EGLImpl {
    namespace {
        using EGLStateContext = MG_State::EGLState::EGLContext;

        EGLStateContext* GetState() {
            if (!MG_State::pEGLContext) {
                MGLOG_E_ONCE("pEGLContext is null. MG_State may not be initialized.");
            }
            return MG_State::pEGLContext.get();
        }

        // Entry points that can legitimately be an application's FIRST EGL
        // call (display/proc-address/string queries) lazily bring MobileGL
        // up here, so the library needs no static constructor and can
        // re-initialize after the last eglTerminate tore everything down.
        // Teardown-ish entry points keep using GetState() and fail benignly
        // when MobileGL is not initialized.
        EGLStateContext* GetStateEnsureInitialized() {
            MobileGL::EnsureInitialized();
            return GetState();
        }

        MG_Backend::BackendObject* GetBackendObject(EGLStateContext* state) {
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            if (!backendObject && state) {
                state->SetError(EGL_NOT_INITIALIZED);
            }
            return backendObject;
        }

        std::recursive_mutex& EGLOperationMutex() {
            static std::recursive_mutex mutex;
            return mutex;
        }

        String CurrentThreadIdString() {
            std::ostringstream stream;
            stream << std::this_thread::get_id();
            return stream.str();
        }

        MG_Backend::WindowBackend DetectWindowBackend() {
#if defined(ANDROID) || defined(__ANDROID__)
            return MG_Backend::WindowBackend::Android;
#elif defined(__APPLE__)
            return MG_Backend::WindowBackend::MetalLayer;
#elif defined(_WIN32)
            return MG_Backend::WindowBackend::Win32;
#elif defined(__linux__)
            return MG_Backend::WindowBackend::X11;
#else
            return MG_Backend::WindowBackend::Unknown;
#endif
        }

        EGLint GetAttribValue(const EGLint* attribList, EGLint attrib, EGLint defaultValue) {
            if (!attribList) {
                return defaultValue;
            }
            for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                if (attribList[i] == attrib) {
                    return attribList[i + 1];
                }
            }
            return defaultValue;
        }

        EGLint GetAttribValueAttrib(const EGLAttrib* attribList, EGLint attrib, EGLint defaultValue) {
            if (!attribList) {
                return defaultValue;
            }
            for (SizeT i = 0; attribList[i] != EGL_NONE; i += 2) {
                if (attribList[i] == attrib) {
                    return static_cast<EGLint>(attribList[i + 1]);
                }
            }
            return defaultValue;
        }

        template <typename NativeType>
        Bool IsNullNativeHandle(NativeType nativeHandle) {
            if constexpr (std::is_pointer_v<NativeType>) {
                return nativeHandle == nullptr;
            } else {
                return nativeHandle == 0;
            }
        }

        template <typename NativeType>
        void* ToVoidHandle(NativeType nativeHandle) {
            if constexpr (std::is_pointer_v<NativeType>) {
                return reinterpret_cast<void*>(nativeHandle);
            } else {
                return reinterpret_cast<void*>(static_cast<SizeT>(nativeHandle));
            }
        }
    } // namespace

    EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                   const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        if (!state->IsDisplayInitialized(dpy)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_NO_SURFACE;
        }
        if (!state->ValidateConfigOnDisplay(dpy, config)) {
            state->SetError(EGL_BAD_CONFIG);
            return EGL_NO_SURFACE;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // P12 (on-screen server window), D1. With MOBILEGL_IPC_SURFACE=server and a remote server,
        // the window is the SERVER's: a headless client passes none, so NULL is not
        // EGL_BAD_NATIVE_WINDOW here. Whatever the client passed never reaches the wire - the
        // remote backend sends WindowKind::ServerOwned with token 0 (BackendObject_Remote).
        // Either on-screen shape leaves this side without a window of its own: =server asks the
        // server for its own window, =host draws into the frames the display host offers.
        const Bool windowlessOnScreen =
            MG_Config::ServerOwnedWindowSurfaces() || MG_Config::HostFrameWindowSurfaces();
        if (IsNullNativeHandle(window) && !windowlessOnScreen) {
#else
        if (IsNullNativeHandle(window)) {
#endif
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        Uint32 requestedWidth = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_WIDTH, 0), 0));
        Uint32 requestedHeight = static_cast<Uint32>(std::max<EGLint>(GetAttribValue(attrib_list, EGL_HEIGHT, 0), 0));
#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)
        // P12: A GAME'S OWN WINDOW SIZE IS THE SIZE OF THE FRAMES IT RENDERS, and a server-owned
        // window has to take that size or clip. SDL (Minecraft's Android backend) creates the EGL
        // window surface with no EGL_WIDTH / EGL_HEIGHT, so the request went out as 0x0 - "use the
        // server window's own size" - and a landscape 2620x1280 game drawn into a portrait 1280x2620
        // server window came out clipped to its bottom-left 1280x1280 corner (Espryt and Magma
        // alike; the same game in a landscape window was right). The native window it handed us
        // knows the size the game believes it has: ask for THAT, and the server fixes its window
        // buffer to it and aspect-fits the view (MobileGLDisplayActivity), letterboxed.
        if (windowlessOnScreen && requestedWidth == 0 && requestedHeight == 0 && !IsNullNativeHandle(window)) {
            auto* native = reinterpret_cast<ANativeWindow*>(ToVoidHandle(window));
            const int32_t nativeWidth = ANativeWindow_getWidth(native);
            const int32_t nativeHeight = ANativeWindow_getHeight(native);
            if (nativeWidth > 0 && nativeHeight > 0) {
                requestedWidth = static_cast<Uint32>(nativeWidth);
                requestedHeight = static_cast<Uint32>(nativeHeight);
                MGLOG_I("eglCreateWindowSurface: no EGL_WIDTH/EGL_HEIGHT; the on-screen surface is asked for "
                        "the native window's %dx%d", nativeWidth, nativeHeight);
            }
        }
#endif
        const MG_Backend::WindowHandle windowHandle = {
            .Backend = DetectWindowBackend(),
            .Handle = ToVoidHandle(window),
            .Width = requestedWidth,
            .Height = requestedHeight,
        };

#if MOBILEGL_BUILD_DISAGGREGATED
        EGLSurface surface = EGL_NO_SURFACE;
        if (MG_Config::ServerOwnedWindowSurfaces()) {
            // The server's own window: it makes one and draws into it.
            surface = state->CreateServerOwnedWindowSurface(
                dpy, config, windowHandle.Handle, static_cast<EGLint>(windowHandle.Width),
                static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/false);
        } else if (MG_Config::HostFrameWindowSurfaces()) {
            // The display host's frames, as above: no window of the client's either way.
            surface = state->CreateHostFrameWindowSurface(
                dpy, config, windowHandle.Handle, static_cast<EGLint>(windowHandle.Width),
                static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/false);
        } else {
            surface = state->CreateWindowSurface(dpy, config, window, attrib_list);
        }
#else
        EGLSurface surface = state->CreateWindowSurface(dpy, config, window, attrib_list);
#endif
        if (surface == EGL_NO_SURFACE) {
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            state->DestroySurface(dpy, surface);
            return EGL_NO_SURFACE;
        }
        if (!backendObject->CreateEGLWindowSurface(surface, windowHandle)) {
            state->DestroySurface(dpy, surface);
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        return surface;
    }

    EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw) {
        const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, draw)) {
            MGLOG_E_ONCE("eglSwapBuffers: surface %p is not a surface of display %p (EGL_BAD_SURFACE)", draw, dpy);
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        if (!backendObject->SwapEGLBuffers(dpy, draw)) {
            MGLOG_E_ONCE("eglSwapBuffers failed on thread=%s dpy=%p draw=%p", CurrentThreadIdString().c_str(), dpy, draw);
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config) {
        // eglChooseConfig can be an application's first EGL call after eglGetPlatformDisplay + eglInitialize,
        // and for a display built from an EGLDeviceEXT nothing has brought MobileGL up yet: GetState()
        // then answers NULL and the call lands in EGLContext::ChooseConfig with a null this - measured as
        // a crash inside that function (pc/lr both resolved to it) with kwin's own "pEGLContext is null.
        // MG_State may not be initialized." as the only warning, and reported one layer up as
        // "Could not initialize rendering context".
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_FALSE;
        }
        return state->ChooseConfig(dpy, attrib_list, configs, config_size, num_config) ? EGL_TRUE : EGL_FALSE;
    }

    EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list) {
        // WHICH CONTEXT IS BEING ASKED FOR, and what came back.  kwin's OpenGL compositing fails with
        // "Could not initialize rendering context" while the same path succeeds from a probe against
        // this same library (probe10: a 3.3 core context on a GBM surface, makeCurrent answered 1),
        // so the difference has to be in this argument list - and an EGL vendor cannot see it from the
        // outside.  The list is dumped in EGL_NONE-terminated pairs; EGL_NONE is 0x3038, not 0.
        MGLOG_I("eglCreateContext(dpy=%p, config=%p, share=%p)", dpy, config, shareCtx);
        if (attrib_list != nullptr) {
            for (int index = 0; attrib_list[index] != EGL_NONE; index += 2) {
                MGLOG_I("    attrib 0x%04x = %d", attrib_list[index], attrib_list[index + 1]);
            }
        } else {
            MGLOG_I("    (no attribute list)");
        }
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_NO_CONTEXT;
        }
        EGLContext context = state->CreateContext(dpy, config, shareCtx, attrib_list);
        MGLOG_I("eglCreateContext -> %p", static_cast<void*>(context));
        return context;
    }

    EGLBoolean Initialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->InitializeDisplay(dpy, major, minor)) {
            return EGL_FALSE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        if (!backendObject->InitializeEGLDisplay(dpy, major, minor)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLDisplay GetDisplay(NativeDisplayType display) {
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetDisplay(display);
    }

    EGLint GetError() {
        auto* state = GetState();
        if (!state) {
            return EGL_NOT_INITIALIZED;
        }
        return state->ConsumeError();
    }

    // THE DISPLAY HOST'S FRAME, AS THE COMPOSITOR PRESENTS IT.  Set by the surfaceless-bind hook in
    // MakeCurrent when this client (the compositor) takes the host frame; read by glFlush, which is
    // that compositor's only frame boundary - it never calls eglSwapBuffers, and without a present
    // the display host is never told a frame is done, so nothing ever reaches the glass.
    EGLDisplay g_hostFramePresentDisplay = nullptr;
    EGLSurface g_hostFramePresentSurface = nullptr;

    EGLBoolean MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }

        const auto oldDisplay = state->GetCurrentDisplay();
        const auto oldDraw = state->GetCurrentSurface(EGL_DRAW);
        const auto oldRead = state->GetCurrentSurface(EGL_READ);
        const auto oldContext = state->GetCurrentContext();
        const String threadId = CurrentThreadIdString();

        MGLOG_D("eglMakeCurrent begin thread=%s dpy=%p draw=%p read=%p ctx=%p oldDpy=%p oldDraw=%p oldRead=%p oldCtx=%p",
                threadId.c_str(), dpy, draw, read, ctx, oldDisplay, oldDraw, oldRead, oldContext);

#if MOBILEGL_BUILD_DISAGGREGATED
        // THE DISPLAY HOST'S FRAMES, TO A CLIENT THAT ASKS FOR NO SURFACE.
        //
        // A compositor on this route never calls eglCreateWindowSurface: it imports the display host's
        // dma-bufs and draws into them through framebuffers, binding its contexts with EGL_NO_SURFACE.
        // The host frame bridge, though, is opened by exactly one thing - a surface being CREATED and
        // activated (ServerLoop's CreateWindowSurface with the host-frame window backend ->
        // InitHostFrameSurface) - so with no surface ever created the bridge stayed shut, the display
        // host kept offering frames into a name nobody served ("offer buffer 0: bridge fd=-1"), and no
        // frame could reach the glass no matter how much was drawn.  So: when the session asked for
        // host-framed surfaces and a real context is being bound with no surface, make it the surface
        // eglCreatePlatformWindowSurface would have made, once, and bind that.  The size is the display
        // host's own frame size; the client's window value is not on this path at all.
        // ONLY THE COMPOSITOR TAKES THE HOST FRAME.  A desktop is full of processes that bind a
        // context with no surface - konsole, plasmashell, kactivitymanagerd - and measured with
        // probe15 as the second client: such a bind from a NON-compositor process made the host-frame
        // activation fail (makeCurrent=0) and the process aborted.  The compositor is marked by its
        // launcher with MOBILEGL_HOST_FRAME_CLIENT=1; everyone else keeps the plain surfaceless bind,
        // which is all they ever wanted.
        if (draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT &&
            MG_Config::HostFrameWindowSurfaces() &&
            (std::getenv("MOBILEGL_HOST_FRAME_CLIENT") != nullptr &&
             std::string("1") == std::getenv("MOBILEGL_HOST_FRAME_CLIENT"))) {
            static Bool hostFrameSurfaceTaken = false;
            if (!hostFrameSurfaceTaken) {
                hostFrameSurfaceTaken = true;
                EGLConfig hostFrameConfig = nullptr;
                EGLint hostFrameConfigCount = 0;
                const EGLint hostFrameChoose[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
                if (state->ChooseConfig(dpy, hostFrameChoose, &hostFrameConfig, 1,
                                        &hostFrameConfigCount) &&
                    hostFrameConfigCount > 0 && hostFrameConfig != nullptr) {
                    // BOTH HALVES, exactly as eglCreateWindowSurface makes one: the EGL state's
                    // registration (without it every later bind and swap of this surface was refused
                    // as EGL_BAD_SURFACE - measured: "eglMakeCurrent rejected by EGLState
                    // error=0x300d", and glFlush's swaps returned before reaching the backend), then
                    // the backend's own window-surface entry, which the remote backend routes to the
                    // host frame (BackendObject_Remote::CreateHostFrameWindowSurface) - the call that
                    // puts a CreateWindowSurface frame with the host-frame window backend on the wire.
                    const EGLSurface hostFrameSurface = state->CreateHostFrameWindowSurface(
                        dpy, hostFrameConfig, nullptr, 1440, 3200, /*platformWindow=*/false);
                    MG_Backend::WindowHandle hostFrameHandle{};
                    hostFrameHandle.Width = 1440;
                    hostFrameHandle.Height = 3200;
                    if (hostFrameSurface == EGL_NO_SURFACE) {
                        MGLOG_E("host frame: the EGL state refused the display host's frame surface");
                    } else if (!MG_Backend::pActiveBackendObject->CreateEGLWindowSurface(hostFrameSurface,
                                                                                       hostFrameHandle)) {
                        MGLOG_E("host frame: the backend refused the display host's frame surface");
                        state->DestroySurface(dpy, hostFrameSurface);
                    } else {
                        // The frame boundary of a compositor that never swaps is its glFlush.  glFlush
                        // has no EGL context of its own, so remember HERE which surface presents the
                        // display host's frame; glFlush presents it (see the host-frame present in
                        // glFlush's implementation).
                        g_hostFramePresentDisplay = dpy;
                        g_hostFramePresentSurface = hostFrameSurface;
                        MGLOG_I("host frame: the first surfaceless eglMakeCurrent made the display host's "
                                "frame surface %p (%dx%d)",
                                hostFrameSurface, 1440, 3200);
                    }
                } else {
                    MGLOG_E("host frame: no config for the display host's frame surface");
                }
            }
            // EVERY surfaceless bind, not only the first: the compositor rebinds with EGL_NO_SURFACE
            // all the time, and a bind without the surface leaves nothing current for glFlush's swap
            // to present (BackendObject::SwapEGLBuffers: "draw surface is not current on this thread").
            if (g_hostFramePresentSurface != nullptr && dpy == g_hostFramePresentDisplay) {
                draw = g_hostFramePresentSurface;
                read = g_hostFramePresentSurface;
            }
        }
#endif

        if (!state->MakeCurrent(dpy, draw, read, ctx)) {
            const EGLint error = state->ConsumeError();
            MGLOG_D("eglMakeCurrent rejected by EGLState thread=%s error=0x%04x", threadId.c_str(), error);
            state->SetError(error);
            return EGL_FALSE;
        }

        const Bool releaseCurrentRequest =
            draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        if (releaseCurrentRequest) {
            if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
                if (!backendObject->MakeEGLCurrent(dpy, draw, read, ctx)) {
                    MGLOG_E_ONCE("eglMakeCurrent release failed in backend thread=%s", threadId.c_str());
                    state->MakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
                    state->SetError(EGL_BAD_ACCESS);
                    return EGL_FALSE;
                }
            }
            MGLOG_D("eglMakeCurrent release succeeded thread=%s", threadId.c_str());
            return EGL_TRUE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            state->MakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
            return EGL_FALSE;
        }
        if (!backendObject->MakeEGLCurrent(dpy, draw, read, ctx)) {
            MGLOG_E_ONCE("eglMakeCurrent backend attach failed thread=%s dpy=%p draw=%p read=%p ctx=%p", threadId.c_str(),
                    dpy, draw, read, ctx);
            state->SetError(EGL_BAD_ACCESS);
            state->MakeCurrent(oldDisplay, oldDraw, oldRead, oldContext);
            return EGL_FALSE;
        }
        MGLOG_D("eglMakeCurrent attach succeeded thread=%s dpy=%p draw=%p read=%p ctx=%p", threadId.c_str(), dpy, draw,
                read, ctx);
        return EGL_TRUE;
    }

    EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroyContext(dpy, ctx) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean DestroySurface(EGLDisplay dpy, EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->DestroySurface(dpy, surface)) {
            return EGL_FALSE;
        }
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
            backendObject->ReleaseEGLSurface(surface);
        }
        return EGL_TRUE;
    }

    EGLBoolean Terminate(EGLDisplay dpy) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->TerminateDisplay(dpy)) {
            return EGL_FALSE;
        }
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
            backendObject->ReleaseEGLResources();
        }
        // The last initialized display is gone and nothing is current on any
        // thread: tear the whole library down deterministically inside the
        // EGL lifecycle (backend, GL/EGL state, glslang). A later EGL call
        // re-initializes lazily via GetStateEnsureInitialized(); process exit
        // then has nothing left to destroy.
        if (!state->HasAnyInitializedDisplay() && !state->HasAnyCurrentContext()) {
            MobileGL::Destroy();
        }
        return EGL_TRUE;
    }

    EGLBoolean ReleaseThread() {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
            (void)backendObject->MakeEGLCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        state->ReleaseThread();
        return EGL_TRUE;
    }

    EGLContext GetCurrentContext() {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_CONTEXT;
        }
        return state->GetCurrentContext();
    }

    EGLBoolean GetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetConfigAttrib(dpy, config, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean BindAPI(EGLenum api) {
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_FALSE;
        }
        switch (api) {
        case EGL_OPENGL_API:
        case EGL_OPENGL_ES_API:
        case EGL_OPENVG_API:
            state->SetBoundAPI(api);
            return EGL_TRUE;
        default:
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
    }

    EGLSurface GetCurrentSurface(EGLint readdraw) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->GetCurrentSurface(readdraw);
    }

    EGLBoolean QuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QuerySurface(display, surface, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    char const* QueryString(EGLDisplay display, EGLint name) {
        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return nullptr;
        }

        if (display != EGL_NO_DISPLAY && !state->ValidateDisplay(display)) {
            state->SetError(EGL_BAD_DISPLAY);
            return nullptr;
        }

        switch (name) {
        case EGL_VENDOR:
            return "MobileGL";
        case EGL_VERSION:
            return "1.5 MobileGL";
        case EGL_CLIENT_APIS:
            return "OpenGL OpenGL_ES";
        case EGL_EXTENSIONS:
            // One list, in EGLPlatformExtensions.h, because the vendor string glvnd reads and
            // this query an application reads have to agree: an application that does not see the
            // platform extension here never asks for a platform display at all.
            return display == EGL_NO_DISPLAY ? kClientExtensionString : kDisplayExtensionString;
        default:
            state->SetError(EGL_BAD_PARAMETER);
            return nullptr;
        }
    }

    EGLBoolean SwapInterval(EGLDisplay dpy, EGLint interval) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->SwapInterval(dpy, interval)) {
            return EGL_FALSE;
        }
        // Forward the request to the backend's native presentation path; without this
        // the app's vsync setting only ever reaches MobileGL's shadow state and the
        // native surface stays at the driver default (interval 1 = always vsynced).
        auto* backendObject = GetBackendObject(state);
        if (backendObject) {
            backendObject->SetEGLSwapInterval(static_cast<Int>(interval));
        }
        return EGL_TRUE;
    }

    EGLSurface CreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        const EGLint width = GetAttribValue(attrib_list, EGL_WIDTH, 1);
        const EGLint height = GetAttribValue(attrib_list, EGL_HEIGHT, 1);
        EGLSurface surface = state->CreatePbufferSurface(dpy, config, attrib_list);
        if (surface == EGL_NO_SURFACE) {
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            return EGL_NO_SURFACE;
        }
        if (!backendObject->CreateEGLPbufferSurface(surface, width, height)) {
            state->DestroySurface(dpy, surface);
            state->SetError(EGL_BAD_ALLOC);
            return EGL_NO_SURFACE;
        }

        return surface;
    }

    EGLBoolean BindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (buffer != EGL_BACK_BUFFER) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean ReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (buffer != EGL_BACK_BUFFER) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLBoolean CopyBuffers(EGLDisplay dpy, EGLSurface surface, EGLNativePixmapType target) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (IsNullNativeHandle(target)) {
            state->SetError(EGL_BAD_NATIVE_PIXMAP);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLSurface CreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype, EGLClientBuffer buffer, EGLConfig config,
                                             const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePbufferFromClientBuffer(dpy, buftype, buffer, config, attrib_list);
    }

    EGLSurface CreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap,
                                   const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePixmapSurface(dpy, config, pixmap, attrib_list);
    }

    EGLBoolean GetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetConfigs(dpy, configs, config_size, num_config) ? EGL_TRUE : EGL_FALSE;
    }

    EGLDisplay GetCurrentDisplay() {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetCurrentDisplay();
    }

    EGLenum QueryAPI() {
        auto* state = GetState();
        if (!state) {
            return EGL_OPENGL_API;
        }
        return state->GetBoundAPI();
    }

    EGLBoolean QueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QueryContext(dpy, ctx, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLBoolean SurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
        (void)value;

        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }

        switch (attribute) {
        case EGL_MIPMAP_LEVEL:
        case EGL_SWAP_BEHAVIOR:
        case EGL_TEXTURE_FORMAT:
        case EGL_TEXTURE_TARGET:
        case EGL_MIPMAP_TEXTURE:
            return EGL_TRUE;
        default:
            state->SetError(EGL_BAD_ATTRIBUTE);
            return EGL_FALSE;
        }
    }

    EGLBoolean WaitClient() {
        return EGL_TRUE;
    }

    EGLBoolean WaitGL() {
        return EGL_TRUE;
    }

    EGLBoolean WaitNative(EGLint engine) {
        (void)engine;
        return EGL_TRUE;
    }

    EGLSync CreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SYNC;
        }
        return state->CreateSync(dpy, type, attrib_list);
    }

    EGLBoolean DestroySync(EGLDisplay dpy, EGLSync sync) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroySync(dpy, sync) ? EGL_TRUE : EGL_FALSE;
    }

    EGLint ClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->ClientWaitSync(dpy, sync, flags, timeout);
    }

    EGLBoolean GetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib* value) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->GetSyncAttrib(dpy, sync, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    EGLImage CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                         const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_IMAGE;
        }
        return state->CreateImage(dpy, ctx, target, buffer, attrib_list);
    }

    EGLBoolean DestroyImage(EGLDisplay dpy, EGLImage image) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->DestroyImage(dpy, image) ? EGL_TRUE : EGL_FALSE;
    }

    EGLImageKHR CreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                               const EGLint* attrib_list) {
        // EGL_KHR_image_base spells its attribute list in EGLint pairs and the core eglCreateImage
        // in EGLAttrib pairs, and the two are not the same memory: on a 64-bit build one entry is
        // twice the width of the other, so the caller's array cannot be handed over as it stands.
        // Re-spelling it is therefore part of the entry point rather than a detail of the state.
        Vector<EGLAttrib> attribs;
        if (attrib_list != nullptr) {
            for (SizeT i = 0; attrib_list[i] != EGL_NONE; i += 2) {
                attribs.push_back(static_cast<EGLAttrib>(attrib_list[i]));
                attribs.push_back(static_cast<EGLAttrib>(attrib_list[i + 1]));
            }
            attribs.push_back(static_cast<EGLAttrib>(EGL_NONE));
        }
        return CreateImage(dpy, ctx, target, buffer, attribs.empty() ? nullptr : attribs.data());
    }

    EGLDisplay GetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list) {
        (void)attrib_list;

        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
        return state->GetPlatformDisplay(platform, native_display);
    }

    EGLSurface CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                           const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // P12 (on-screen server window), D1: as CreateWindowSurface - with MOBILEGL_IPC_SURFACE=server
        // the window is the server's, and a headless client's NULL is accepted.
        // Either on-screen shape leaves this side without a window of its own: =server asks the
        // server for its own window, =host draws into the frames the display host offers.
        const Bool windowlessOnScreen =
            MG_Config::ServerOwnedWindowSurfaces() || MG_Config::HostFrameWindowSurfaces();
        if (native_window == nullptr && !windowlessOnScreen) {
#else
        if (native_window == nullptr) {
#endif
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }
        if (!state->IsDisplayInitialized(dpy)) {
            state->SetError(EGL_NOT_INITIALIZED);
            return EGL_NO_SURFACE;
        }
        if (!state->ValidateConfigOnDisplay(dpy, config)) {
            state->SetError(EGL_BAD_CONFIG);
            return EGL_NO_SURFACE;
        }

        const MG_Backend::WindowHandle windowHandle = {
            .Backend = DetectWindowBackend(),
            .Handle = native_window,
            .Width = static_cast<Uint32>(std::max<EGLint>(GetAttribValueAttrib(attrib_list, EGL_WIDTH, 0), 0)),
            .Height = static_cast<Uint32>(std::max<EGLint>(GetAttribValueAttrib(attrib_list, EGL_HEIGHT, 0), 0)),
        };

#if MOBILEGL_BUILD_DISAGGREGATED
        EGLSurface surface = EGL_NO_SURFACE;
        if (MG_Config::ServerOwnedWindowSurfaces()) {
            // The server's own window: it makes one and draws into it.
            surface = state->CreateServerOwnedWindowSurface(
                dpy, config, native_window, static_cast<EGLint>(windowHandle.Width),
                static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/true);
        } else if (MG_Config::HostFrameWindowSurfaces()) {
            // The display host's frames: it allocates them, the server draws into them,
            // and the host puts each one on the glass when the server says it is drawn.
            surface = state->CreateHostFrameWindowSurface(
                dpy, config, native_window, static_cast<EGLint>(windowHandle.Width),
                static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/true);
        } else {
            surface = state->CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
        }
#else
        EGLSurface surface = state->CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
#endif
        if (surface == EGL_NO_SURFACE) {
            return EGL_NO_SURFACE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            state->DestroySurface(dpy, surface);
            return EGL_NO_SURFACE;
        }
        if (!backendObject->CreateEGLWindowSurface(surface, windowHandle)) {
            state->DestroySurface(dpy, surface);
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_NO_SURFACE;
        }

        return surface;
    }

    EGLBoolean ResizePlatformWindowSurface(EGLDisplay dpy, EGLSurface surface, EGLint width, EGLint height) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ResizeSurface(dpy, surface, width, height)) {
            return EGL_FALSE;
        }
        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        width = std::max<EGLint>(width, 1);
        height = std::max<EGLint>(height, 1);
        if (!backendObject->ResizeEGLWindowSurface(surface, static_cast<Uint32>(width), static_cast<Uint32>(height))) {
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return EGL_FALSE;
        }
        return EGL_TRUE;
    }

    EGLSurface CreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config, void* native_pixmap,
                                           const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
        return state->CreatePlatformPixmapSurface(dpy, config, native_pixmap, attrib_list);
    }

    EGLBoolean WaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->WaitSync(dpy, sync, flags) ? EGL_TRUE : EGL_FALSE;
    }

    __eglMustCastToProperFunctionPointerType GetProcAddress(const char* name) {
        if (!name) {
            return nullptr;
        }
        MobileGL::EnsureInitialized();

        MGLOG_D("eglGetProcAddress(%s)", name);

        // THE KHR SPELLINGS ARE ENTRY POINTS OF THEIR OWN, and by name is the only way anything
        // asks for them.  Measured in the Fedora container that runs this library as the glvnd EGL
        // vendor, before they were answered here: eglGetProcAddress("eglCreateImageKHR") -> NULL and
        // eglGetProcAddress("eglDestroyImageKHR") -> NULL.  kwin_wayland's anland backend imports
        // the display daemon's dma-bufs through exactly these two (AnlandEglLayer::importBuffers ->
        // EglBackend::importDmaBufAsTexture -> EglDisplay::importBufferAsImage ->
        // EglDisplay::createImage, which calls the pointer without a null check), so a NULL here is
        // a compositor that cannot draw a single pixel into the frames the daemon hands it.  The
        // core spellings were already in the table below; these are the same functions under the
        // names EGL_KHR_image_base gives them, which is also why they answer in the glvnd vendor
        // table beside them (Exporting/GlvndVendor.cpp).
        if (std::strcmp(name, "eglCreateImageKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglCreateImageKHR);
        }
        if (std::strcmp(name, "eglDestroyImageKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglDestroyImageKHR);
        }
        // The device-query family and the EXT platform-surface spellings.  Measured in the container
        // before they were answered here: eglGetProcAddress returned NULL for eglQueryDisplayAttribEXT,
        // eglQueryDevicesEXT, eglQueryDeviceAttribEXT, eglQueryDeviceStringEXT and
        // eglCreatePlatformWindowSurfaceEXT while the extension strings announced EGL_EXT_device_query -
        // a NULL libepoxy then called (kwin crashed at pc=0), and withdrawing the announcement instead
        // left kwin without a render device and its compositing inactive.
        if (std::strcmp(name, "eglQueryDevicesEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglQueryDevicesEXT);
        }
        if (std::strcmp(name, "eglQueryDeviceAttribEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglQueryDeviceAttribEXT);
        }
        if (std::strcmp(name, "eglQueryDeviceStringEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglQueryDeviceStringEXT);
        }
        if (std::strcmp(name, "eglQueryDisplayAttribEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglQueryDisplayAttribEXT);
        }
        if (std::strcmp(name, "eglCreatePlatformWindowSurfaceEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglCreatePlatformWindowSurfaceEXT);
        }
        if (std::strcmp(name, "eglCreatePlatformPixmapSurfaceEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglCreatePlatformPixmapSurfaceEXT);
        }
        // The swap-and-sync spellings kwin resolves by name while it is still bringing its EGL backend
        // up (see the definitions: EGL_KHR_fence_sync, the damage variants, EGL_EXT_buffer_age).
        if (std::strcmp(name, "eglSwapBuffersWithDamageKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglSwapBuffersWithDamageKHR);
        }
        if (std::strcmp(name, "eglSwapBuffersWithDamageEXT") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglSwapBuffersWithDamageEXT);
        }
        if (std::strcmp(name, "eglCreateSyncKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglCreateSyncKHR);
        }
        if (std::strcmp(name, "eglDestroySyncKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglDestroySyncKHR);
        }
        if (std::strcmp(name, "eglClientWaitSyncKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglClientWaitSyncKHR);
        }
        if (std::strcmp(name, "eglWaitSyncKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglWaitSyncKHR);
        }
        if (std::strcmp(name, "eglGetSyncAttribKHR") == 0) {
            return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(eglGetSyncAttribKHR);
        }

        void* proc = MG_Impl::GetProcAddress(name);
        if (!proc) {
            MGLOG_D("Failed to get function: %s", name);
            return nullptr;
        }
        return (__eglMustCastToProperFunctionPointerType)proc;
    }
} // namespace MobileGL::MG_Impl::EGLImpl
