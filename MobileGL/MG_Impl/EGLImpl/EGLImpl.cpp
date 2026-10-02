// MobileGL - MobileGL/MG_Impl/EGLImpl/EGLImpl.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EGLImpl.h"
#include "EGLPlatformExtensions.h"
#include "WaylandWindow.h"
#include "../GetProcAddress.h"
#include <Init.h>
#include <MG_Backend/BackendObjects.h>
#include <MG_State/EGLState/Core.h>
#if MOBILEGL_BUILD_DISAGGREGATED
// MG_Config::ServerOwnedWindowSurfaces (P12, MOBILEGL_IPC_SURFACE). Split-only: the pull build's
// translation unit is unchanged (G1).
#include <Config.h>
// P14 S1 (docs/Disaggregated/design/11-state-ownership.md). The EGL context lifecycle's
// client half: eglCreateContext/eglDestroyContext cross as two control frames, and a
// context switch emits the in-band bind_context record. Reached through MG_Remote::Client
// because that is the seam MG_Impl already uses for its other wire producers
// (PipeFill.cpp's emitters) and because the frame channel belongs to the server role - this
// file must not name ServerLoop's forwarders directly.
#include <MG_Remote/Client/WireTables.h>
#endif
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_set>

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

#if MOBILEGL_BUILD_DISAGGREGATED
        // P14 S1 (docs/Disaggregated/design/11-state-ownership.md). THE CONTEXT BINDING, AND
        // THE ONE PLACE IT IS EMITTED.
        //
        // `bind_context` is the ring's half of eglMakeCurrent: the MakeCurrent control frame
        // tells the server which native tuple to hold, and this record tells it which CONTEXT
        // every record after it belongs to - the attribution the session table was built for.
        //
        // IT IS EMITTED AFTER THE SWITCH HAS SUCCEEDED AND BEFORE eglMakeCurrent RETURNS, and
        // that is the whole ordering argument: the app thread cannot publish a record of the
        // new context until this call has returned, and the ring is FIFO, so the binding is
        // behind no verb of the context it names and ahead of every one that follows. Waiting
        // for the first verb instead would need the emitter to know which context the tracker
        // is about to prime; this needs only the token EGLState already holds.
        //
        // An unchanged token emits NOTHING: an identical eglMakeCurrent is a legal no-op in
        // EGL and the session is already bound to that context.
        //
        // THE COMPARISON IS AGAINST THE BINDING LAST SENT, NOT THIS THREAD'S PREVIOUS CONTEXT
        // (GLStreamScope in EGLImpl.h): the session has one current context and another thread
        // may have moved it since this one last bound. The caller holds EGLOperationMutex.
        thread_local Uint64 t_threadContextToken = 0;
        Uint64 g_boundContextToken = 0; // guarded by EGLOperationMutex
        // P14: the share groups of the two tokens above, kept beside them so the client can say
        // which group's object records a record written now lands in (StreamBoundShareGroupToken).
        thread_local Uint64 t_threadShareGroupToken = 0;
        Uint64 g_boundShareGroupToken = 0; // guarded by EGLOperationMutex

        void EmitContextBinding(EGLStateContext* state, EGLContext switched, Uint64 previousToken) {
            (void)previousToken;
            if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return;
            // The apply thread IS the server in this process; a record emitted there would
            // wait on the thread that has to apply it (EmitAndWait), which is a deadlock and
            // not a slow path.
            if (MG_Remote::Client::RunsAsTheServerRole()) return;
            t_threadContextToken = switched == EGL_NO_CONTEXT ? 0 : state->GetContextClientToken(switched);
            t_threadShareGroupToken = switched == EGL_NO_CONTEXT ? 0 : state->GetContextShareGroupToken(switched);
            StreamBindCallingThreadLocked();
        }
#endif

        std::recursive_mutex& EGLOperationMutex() {
            static std::recursive_mutex mutex;
            return mutex;
        }

#if MOBILEGL_BUILD_DISAGGREGATED
    } // namespace

    Bool StreamGateActive() {
        return MG_Config::Transport != MG_Config::TransportMode::Monolith &&
               !MG_Remote::Client::RunsAsTheServerRole();
    }

    std::recursive_mutex& StreamMutex() { return EGLOperationMutex(); }

    void StreamBindCallingThreadLocked() {
        const Uint64 token = t_threadContextToken;
        if (token == g_boundContextToken) return;
        if (MG_Remote::Client::EmitBindContextRecord(token)) {
            g_boundContextToken = token;
            g_boundShareGroupToken = t_threadShareGroupToken;
        }
    }

    Bool StreamBoundShareGroupToken(Uint64* outToken) {
        if (!StreamGateActive()) return false;
        *outToken = g_boundShareGroupToken;
        return true;
    }

    namespace {
#endif
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

        // EGL_KHR_partial_update: the surfaces whose application has declared a damage region. Their
        // buffer-age query tells the server the region follows (a driver whose age is only
        // partial_update's is then asked). Guarded by EGLOperationMutex; a destroyed surface leaves.
        std::unordered_set<EGLSurface>& DamageRegionSurfaces() {
            static auto* surfaces = new std::unordered_set<EGLSurface>();
            return *surfaces;
        }

#if MOBILEGL_WAYLAND_WINDOWS
        // Every Wayland window surface's presentation, by surface.  Guarded by EGLOperationMutex,
        // which every entry point that reaches it holds.
        UnorderedMap<EGLSurface, UniquePtr<Wayland::WindowSurface>>& WaylandSurfaces() {
            static auto* surfaces = new UnorderedMap<EGLSurface, UniquePtr<Wayland::WindowSurface>>();
            return *surfaces;
        }

        // A WINDOW ON A WAYLAND DISPLAY.  Whatever MOBILEGL_IPC_SURFACE says, the window is this
        // application's own wl_surface, so neither the server's window nor the display host's frame
        // is what it shows: the backend draws into a pbuffer of the window's size, and each swap
        // reads the frame back and attaches it to the wl_surface (WaylandWindow.h).  Answers false
        // when the display is not a Wayland one; otherwise *out is the surface, or EGL_NO_SURFACE
        // with the error set.
        // A gbm_surface is scanout memory of a device this library does not draw with; a window
        // on a GBM display is refused rather than taken for some other kind of window.
        template <typename State>
        Bool RefuseGbmWindow(State* state, EGLDisplay dpy) {
            constexpr EGLenum kPlatformGbm = 0x31D7; // EGL_PLATFORM_GBM_KHR == EGL_PLATFORM_GBM_MESA
            Uint64 nativeDisplay = 0;
            EGLenum platform = EGL_NONE;
            if (!state->GetDisplayNative(dpy, &nativeDisplay, &platform) || platform != kPlatformGbm) return false;
            MGLOG_W_ONCE("EGL: window surfaces on a GBM display are not supported (offscreen display only)");
            state->SetError(EGL_BAD_NATIVE_WINDOW);
            return true;
        }

        template <typename State>
        Bool CreateWaylandWindowSurface(State* state, EGLDisplay dpy, EGLConfig config, void* window,
                                        Bool platformWindow, EGLSurface* out) {
            Uint64 nativeDisplay = 0;
            EGLenum platform = EGL_NONE;
            if (!state->GetDisplayNative(dpy, &nativeDisplay, &platform) ||
                !Wayland::IsWaylandDisplay(nativeDisplay, platform)) {
                return false;
            }
            *out = EGL_NO_SURFACE;
            if (!state->IsDisplayInitialized(dpy)) {
                state->SetError(EGL_NOT_INITIALIZED);
                return true;
            }
            if (!state->ValidateConfigOnDisplay(dpy, config)) {
                state->SetError(EGL_BAD_CONFIG);
                return true;
            }
            EGLint width = 0;
            EGLint height = 0;
            if (!Wayland::WindowSize(window, &width, &height)) {
                state->SetError(EGL_BAD_NATIVE_WINDOW);
                return true;
            }
            EGLint alpha = 0;
            (void)state->GetConfigAttrib(dpy, config, EGL_ALPHA_SIZE, &alpha);
            auto presentation =
                Wayland::WindowSurface::Create(reinterpret_cast<void*>(static_cast<uintptr_t>(nativeDisplay)), window,
                                               width, height, alpha > 0);
            if (!presentation) {
                state->SetError(EGL_BAD_NATIVE_WINDOW);
                return true;
            }
            const EGLSurface surface =
                platformWindow ? state->CreatePlatformWindowSurface(dpy, config, window, nullptr)
                               : state->CreateWindowSurface(
                                     dpy, config, (NativeWindowType)(reinterpret_cast<uintptr_t>(window)), nullptr);
            if (surface == EGL_NO_SURFACE) return true;
            (void)state->ResizeSurface(dpy, surface, width, height);
            auto* backendObject = GetBackendObject(state);
            if (!backendObject || !backendObject->CreateEGLPbufferSurface(surface, width, height)) {
                MGLOG_E("eglCreateWindowSurface: the backend refused the %dx%d drawable behind a Wayland window",
                        width, height);
                state->DestroySurface(dpy, surface);
                state->SetError(EGL_BAD_ALLOC);
                return true;
            }
            WaylandSurfaces()[surface] = std::move(presentation);
            *out = surface;
            return true;
        }

        // A WAYLAND WINDOW THE APPLICATION RESIZED (wl_egl_window_resize). As with a driver's own
        // window surface, the new size takes effect at the next make-current or swap: the EGL
        // surface reports it, the wl_shm presentation adopts it, and the drawable behind it is
        // resized so the frame rendered at the new size is the frame read back. Qt resizes the
        // window and then makes the context current, so the make-current arm is the one it hits.
        void ApplyWaylandResize(EGLStateContext* state, EGLDisplay dpy, EGLSurface surface) {
            const auto it = WaylandSurfaces().find(surface);
            if (it == WaylandSurfaces().end()) return;
            EGLint width = 0;
            EGLint height = 0;
            if (!it->second->TakeResize(&width, &height)) return;
            (void)state->ResizeSurface(dpy, surface, width, height);
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            if (backendObject == nullptr ||
                !backendObject->ResizeEGLWindowSurface(surface, static_cast<Uint32>(width), static_cast<Uint32>(height))) {
                MGLOG_E("Wayland: the drawable behind a %dx%d window could not be resized", width, height);
            }
        }
#endif
    } // namespace

    EGLSurface CreateWindowSurface(EGLDisplay dpy, EGLConfig config, NativeWindowType window,
                                   const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
#if MOBILEGL_WAYLAND_WINDOWS
        if (RefuseGbmWindow(state, dpy)) return EGL_NO_SURFACE;
        {
            const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
            EGLSurface waylandSurface = EGL_NO_SURFACE;
            if (CreateWaylandWindowSurface(state, dpy, config, ToVoidHandle(window), /*platformWindow=*/false,
                                           &waylandSurface)) {
                return waylandSurface;
            }
        }
#endif
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
        const Bool serverOwnedWindow = MG_Config::ServerOwnedWindowSurfaces();
        if (IsNullNativeHandle(window) && !serverOwnedWindow) {
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
        if (serverOwnedWindow && requestedWidth == 0 && requestedHeight == 0 && !IsNullNativeHandle(window)) {
            auto* native = reinterpret_cast<ANativeWindow*>(ToVoidHandle(window));
            const int32_t nativeWidth = ANativeWindow_getWidth(native);
            const int32_t nativeHeight = ANativeWindow_getHeight(native);
            if (nativeWidth > 0 && nativeHeight > 0) {
                requestedWidth = static_cast<Uint32>(nativeWidth);
                requestedHeight = static_cast<Uint32>(nativeHeight);
                MGLOG_I("eglCreateWindowSurface: no EGL_WIDTH/EGL_HEIGHT; the server-owned window is asked for the "
                        "native window's %dx%d", nativeWidth, nativeHeight);
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
        EGLSurface surface = serverOwnedWindow
                                 ? state->CreateServerOwnedWindowSurface(
                                       dpy, config, windowHandle.Handle, static_cast<EGLint>(windowHandle.Width),
                                       static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/false)
                                 : state->CreateWindowSurface(dpy, config, window, attrib_list);
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

    EGLBoolean SwapBuffers(EGLDisplay dpy, EGLSurface draw) { return SwapBuffersWithDamage(dpy, draw, nullptr, 0); }

    // EGL_KHR/EXT_swap_buffers_with_damage. The rectangles (GL window coordinates, origin
    // bottom-left; none = the whole surface) are clipped to the surface and fitted into a record.
    // A Wayland window copies only what its shared image missed and damages only the frame's
    // rectangles; a server-owned window hands them to the native present. Either way they are a
    // hint about what changed, never a limit on what is shown.
    EGLBoolean SwapBuffersWithDamage(EGLDisplay dpy, EGLSurface draw, const EGLint* rects, EGLint n_rects) {
        const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
#if MOBILEGL_BUILD_DISAGGREGATED
        // The present is this thread's context's (GLStreamScope in EGLImpl.h).
        if (StreamGateActive()) StreamBindCallingThreadLocked();
#endif
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, draw)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }

        auto* backendObject = GetBackendObject(state);
        if (!backendObject) {
            MGLOG_E_ONCE("activeBackendObject not initialized!");
            return EGL_FALSE;
        }
        if (n_rects < 0 || (n_rects > 0 && rects == nullptr)) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        MG_Util::Damage::Region damage = MG_Util::Damage::Region::FromEglRects(rects, n_rects);
        if (!damage.IsFull()) {
            EGLint width = 0;
            EGLint height = 0;
            if (state->QuerySurface(dpy, draw, EGL_WIDTH, &width) && state->QuerySurface(dpy, draw, EGL_HEIGHT, &height)) {
                damage.Normalize(width, height);
            } else {
                damage.SetFull();
            }
        }
#if MOBILEGL_WAYLAND_WINDOWS
        // A Wayland window shows what is attached to it: the frame is read back and attached
        // BEFORE the swap, while it is still the drawable's content.
        if (auto it = WaylandSurfaces().find(draw); it != WaylandSurfaces().end() && !it->second->Present(damage)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
#endif
        const Bool swapped = damage.IsFull() ? backendObject->SwapEGLBuffers(dpy, draw)
                                             : backendObject->SwapEGLBuffersWithDamage(dpy, draw, damage);
        if (!swapped) {
            MGLOG_E_ONCE("eglSwapBuffers failed on thread=%s dpy=%p draw=%p", CurrentThreadIdString().c_str(), dpy, draw);
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
#if MOBILEGL_WAYLAND_WINDOWS
        // A resize made between frames takes effect for the next one.
        ApplyWaylandResize(state, dpy, draw);
#endif
        return EGL_TRUE;
    }

    EGLBoolean ChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size,
                            EGLint* num_config) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->ChooseConfig(dpy, attrib_list, configs, config_size, num_config) ? EGL_TRUE : EGL_FALSE;
    }

    EGLContext CreateContext(EGLDisplay dpy, EGLConfig config, EGLContext shareCtx, const EGLint* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_CONTEXT;
        }
        const EGLContext context = state->CreateContext(dpy, config, shareCtx, attrib_list);
        if (context == EGL_NO_CONTEXT) {
            return EGL_NO_CONTEXT;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // P14 S1 (docs/Disaggregated/design/11-state-ownership.md). THE TOKEN CROSSES BEFORE
        // eglCreateContext RETURNS. EGLState minted it, together with the share group it was
        // derived into (the sharing context's group, or a fresh one), and the server tables it
        // here through the same blocking handshake the surface forwarders use - so the first
        // bind_context after this call names a context the session already has, and the
        // sink's "this session never created that token" refusal is unreachable on the
        // well-behaved path.
        //
        // A REFUSAL DROPS THE CLIENT'S CONTEXT AGAIN. Keeping it would hand the application a
        // handle whose first verb the server refuses - a late failure where an early one is
        // available, and EGL_NO_CONTEXT with EGL_BAD_ACCESS is the early one. NoWire (the
        // pre-Start bring-up, the server role) keeps it: there is no server to refuse.
        if (MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            !MG_Remote::Client::RunsAsTheServerRole()) {
            const auto emitted = MG_Remote::Client::SendCreateContextFrame(
                state->GetContextClientToken(context), state->GetContextShareGroupToken(context), 0);
            if (emitted == MG_Remote::Client::ContextFrameEmit::Refused) {
                state->DestroyContext(dpy, context);
                state->SetError(EGL_BAD_ACCESS);
                return EGL_NO_CONTEXT;
            }
        }
#endif
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
#if MOBILEGL_BUILD_DISAGGREGATED
        // P14 S1. The context the calling thread had current BEFORE this call, as the wire
        // knows it. Read here because `state->MakeCurrent` below has already committed the
        // new one by the time the emission points are reached.
        const Uint64 oldContextToken = state->GetContextClientToken(oldContext);
#endif
        const String threadId = CurrentThreadIdString();

        MGLOG_D("eglMakeCurrent begin thread=%s dpy=%p draw=%p read=%p ctx=%p oldDpy=%p oldDraw=%p oldRead=%p oldCtx=%p",
                threadId.c_str(), dpy, draw, read, ctx, oldDisplay, oldDraw, oldRead, oldContext);

#if MOBILEGL_WAYLAND_WINDOWS
        if (draw != EGL_NO_SURFACE) ApplyWaylandResize(state, dpy, draw);
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
#if MOBILEGL_BUILD_DISAGGREGATED
            EmitContextBinding(state, EGL_NO_CONTEXT, oldContextToken);
#endif
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
#if MOBILEGL_BUILD_DISAGGREGATED
        EmitContextBinding(state, ctx, oldContextToken);
#endif
        return EGL_TRUE;
    }

    EGLBoolean DestroyContext(EGLDisplay dpy, EGLContext ctx) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        // P14 S1. Read BEFORE the client's own book drops the context: after DestroyContext
        // the handle no longer resolves and the token is unrecoverable. The frame is sent
        // AFTER the client's state machine has accepted the destroy, so one EGL refused (a
        // context still current on some thread, EGL_BAD_ACCESS) never reaches the server - the
        // two ends stay in step without the server having to guess why a token vanished.
        const Uint64 contextToken = state->GetContextClientToken(ctx);
#endif
        if (!state->DestroyContext(dpy, ctx)) {
            return EGL_FALSE;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (contextToken != 0 && MG_Config::Transport != MG_Config::TransportMode::Monolith &&
            !MG_Remote::Client::RunsAsTheServerRole()) {
            const std::lock_guard<std::recursive_mutex> streamLock(EGLOperationMutex());
            // The server drops its binding to 0 when the current context is destroyed
            // (ServerSession::DestroyContext's compare-and-swap); the gate mirrors it so the next
            // write re-binds.
            if (g_boundContextToken == contextToken) {
                g_boundContextToken = 0;
                g_boundShareGroupToken = 0;
            }
            const auto emitted = MG_Remote::Client::SendDestroyContextFrame(contextToken);
            if (emitted == MG_Remote::Client::ContextFrameEmit::Refused) {
                MGLOG_E_ONCE("eglDestroyContext: the server holds no context token %llu; the "
                             "session's table and this client's EGL book have drifted apart",
                             static_cast<unsigned long long>(contextToken));
            }
        }
#endif
        return EGL_TRUE;
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
        DamageRegionSurfaces().erase(surface);
#if MOBILEGL_WAYLAND_WINDOWS
        WaylandSurfaces().erase(surface);
#endif
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
            // The display's dma-buf EGLImages went with it; so do their shared-image references.
            for (const Uint64 sharedImageId : state->TakeOrphanedSharedImages()) {
                (void)backendObject->ReleaseSharedImage(sharedImageId);
            }
            backendObject->ReleaseEGLResources();
        }
        // The last initialized display is gone and nothing is current on any
        // thread: tear the whole library down deterministically inside the
        // EGL lifecycle (backend, GL/EGL state, glslang). A later EGL call
        // re-initializes lazily via GetStateEnsureInitialized(); process exit
        // then has nothing left to destroy.
        if (!state->HasAnyInitializedDisplay() && !state->HasAnyCurrentContext()) {
            // P14 S3: the process default context (and with it the GL objects the frontend
            // built while no EGL context was current) goes before Destroy() does, so nothing
            // it owns outlives glslang::FinalizeProcess() through a thread's fallback binding.
            MG_State::ReleaseProcessDefaultGLContext();
            MobileGL::Destroy();
        }
        return EGL_TRUE;
    }

    EGLBoolean ReleaseThread() {
        // Always succeeds (EGL 1.4 §3.11): after the last eglTerminate tore MobileGL down there is
        // simply nothing of this thread's left to release.
        if (!MG_State::pEGLContext) return EGL_TRUE;
        auto* state = GetState();
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
        // EGL_EXT_buffer_age. The buffers are the server's, so the server answers - in the stream,
        // behind the records already written, for the buffer the next frame draws into. Only for the
        // calling thread's draw surface: that is the buffer the question is about (and the stream is
        // bound to its context).
        if (attribute == EGL_BUFFER_AGE_EXT) {
            const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
            if (!SharedImagesAvailable()) {
                state->SetError(EGL_BAD_ATTRIBUTE);
                return EGL_FALSE;
            }
            if (!state->ValidateSurfaceOnDisplay(display, surface)) {
                state->SetError(EGL_BAD_SURFACE);
                return EGL_FALSE;
            }
            if (value == nullptr) {
                state->SetError(EGL_BAD_PARAMETER);
                return EGL_FALSE;
            }
            if (state->GetCurrentSurface(EGL_DRAW) != surface) {
                state->SetError(EGL_BAD_SURFACE);
                return EGL_FALSE;
            }
#if MOBILEGL_BUILD_DISAGGREGATED
            if (StreamGateActive()) StreamBindCallingThreadLocked();
#endif
            EGLint age = 0;
            auto* backendObject = MG_Backend::pActiveBackendObject.get();
            const Bool regionFollows = DamageRegionSurfaces().count(surface) != 0;
            if (backendObject == nullptr || !backendObject->QueryBufferAge(regionFollows, &age)) age = 0;
            *value = age;
            return EGL_TRUE;
        }
        // A size that changes under the application (a server-owned window resized) arrives from the
        // backend: let it bring the surface's record up to date first, so a caller that polls the size
        // while it draws nothing still sees the change.
        if (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) {
            if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) backendObject->RefreshSurfaceExtent(surface);
        }
        return state->QuerySurface(display, surface, attribute, value) ? EGL_TRUE : EGL_FALSE;
    }

    // EGL_KHR_partial_update: the region this frame draws, before it draws (GL window coordinates;
    // none = the whole surface). Forwarded in the stream to the server's native surface. More than
    // kMaxRects rectangles are merged, so an application declaring that many draws their bounds.
    EGLBoolean SetDamageRegion(EGLDisplay dpy, EGLSurface surface, const EGLint* rects, EGLint n_rects) {
        const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
        auto* state = GetState();
        if (!state) return EGL_FALSE;
        if (!SharedImagesAvailable()) {
            state->SetError(EGL_BAD_DISPLAY);
            return EGL_FALSE;
        }
        if (!state->ValidateSurfaceOnDisplay(dpy, surface)) {
            state->SetError(EGL_BAD_SURFACE);
            return EGL_FALSE;
        }
        if (state->GetCurrentSurface(EGL_DRAW) != surface) {
            state->SetError(EGL_BAD_MATCH);
            return EGL_FALSE;
        }
        if (n_rects < 0 || (n_rects > 0 && rects == nullptr)) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_FALSE;
        }
        MG_Util::Damage::Region region = MG_Util::Damage::Region::FromEglRects(rects, n_rects);
        if (!region.IsFull()) {
            EGLint width = 0;
            EGLint height = 0;
            if (state->QuerySurface(dpy, surface, EGL_WIDTH, &width) && state->QuerySurface(dpy, surface, EGL_HEIGHT, &height))
                region.Normalize(width, height);
            else
                region.SetFull();
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (StreamGateActive()) StreamBindCallingThreadLocked();
#endif
        DamageRegionSurfaces().insert(surface);
        if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) (void)backendObject->SetDamageRegion(region);
        return EGL_TRUE;
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
            if (display == EGL_NO_DISPLAY) return kClientExtensionString;
            return SharedImagesAvailable() ? kDisplayExtensionStringWithSharedImages : kDisplayExtensionString;
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

    Bool SharedImagesAvailable() {
#if MOBILEGL_BUILD_DISAGGREGATED
        // A split client's backend is the remote one, which forwards the shared-image verbs to
        // the server; the server's own apply thread and a monolith have no allocator behind them.
        return StreamGateActive() && MG_Backend::pActiveBackendObject != nullptr;
#else
        return false;
#endif
    }

    Bool LookupSharedImage(EGLImage image, Uint64* id, EGLint* width, EGLint* height) {
        auto* state = GetState();
        MG_State::EGLState::EGLContext::SharedImageInfo info;
        if (!state || !state->GetSharedImage(image, &info)) return false;
        *id = info.Id;
        *width = info.Width;
        *height = info.Height;
        return true;
    }

    EGLImage CreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                         const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_IMAGE;
        }
        if (target != EGL_LINUX_DMA_BUF_EXT) {
            return state->CreateImage(dpy, ctx, target, buffer, attrib_list);
        }

        // EGL_EXT_image_dma_buf_import. The only dma-bufs this library can bind are the server's
        // own shared images (a client's linux-dmabuf wl_buffer): the descriptor goes to the
        // server, which names the image it was exported from, or refuses it.
        if (!SharedImagesAvailable()) {
            state->SetError(EGL_BAD_PARAMETER);
            return EGL_NO_IMAGE;
        }
        MG_State::EGLState::EGLContext::DmaBufImportAttribs request;
        if (!state->PrepareDmaBufImport(dpy, ctx, buffer, attrib_list, &request)) {
            return EGL_NO_IMAGE;
        }
        auto* backendObject = GetBackendObject(state);
        Uint64 sharedImageId = 0;
        if (!backendObject ||
            !backendObject->ImportSharedImage(request.Fd, static_cast<Uint32>(request.Width),
                                              static_cast<Uint32>(request.Height), request.Fourcc, &sharedImageId) ||
            sharedImageId == 0) {
            MGLOG_I_ONCE("eglCreateImage: a %dx%d dma-buf is not one of the server's shared images; refused "
                         "(EGL_BAD_MATCH)",
                         request.Width, request.Height);
            state->SetError(EGL_BAD_MATCH);
            return EGL_NO_IMAGE;
        }
        const EGLImage image = state->CreateSharedImage(
            dpy, {.Id = sharedImageId, .Width = request.Width, .Height = request.Height, .Fourcc = request.Fourcc});
        if (image == EGL_NO_IMAGE) (void)backendObject->ReleaseSharedImage(sharedImageId);
        return image;
    }

    EGLBoolean DestroyImage(EGLDisplay dpy, EGLImage image) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        Uint64 sharedImageId = 0;
        if (!state->DestroyImage(dpy, image, &sharedImageId)) {
            return EGL_FALSE;
        }
        // A texture the image was bound to keeps its own reference on the server (EGLImage
        // siblings outlive the image), so only this handle's goes.
        if (sharedImageId != 0) {
            if (auto* backendObject = MG_Backend::pActiveBackendObject.get()) {
                (void)backendObject->ReleaseSharedImage(sharedImageId);
            }
        }
        return EGL_TRUE;
    }

    EGLBoolean QueryDmaBufFormats(EGLDisplay dpy, EGLint max_formats, EGLint* formats, EGLint* num_formats) {
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QueryDmaBufFormats(dpy, SharedImagesAvailable(), max_formats, formats, num_formats)
                   ? EGL_TRUE
                   : EGL_FALSE;
    }

    EGLBoolean QueryDmaBufModifiers(EGLDisplay dpy, EGLint format, EGLint max_modifiers, EGLuint64KHR* modifiers,
                                    EGLBoolean* external_only, EGLint* num_modifiers) {
        static_assert(sizeof(EGLuint64KHR) == sizeof(Uint64));
        auto* state = GetState();
        if (!state) {
            return EGL_FALSE;
        }
        return state->QueryDmaBufModifiers(dpy, SharedImagesAvailable(), format, max_modifiers,
                                           reinterpret_cast<Uint64*>(modifiers), external_only, num_modifiers)
                   ? EGL_TRUE
                   : EGL_FALSE;
    }

    EGLDisplay GetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list) {
        (void)attrib_list;

        auto* state = GetStateEnsureInitialized();
        if (!state) {
            return EGL_NO_DISPLAY;
        }
#if MOBILEGL_WAYLAND_WINDOWS
        // A "GBM" display whose native display is a wl_display is a Wayland one: Chromium's
        // Wayland GL layer names the GBM platform for every display it asks for, including the
        // one on its own Wayland connection whose windows are wl_egl_windows.
        constexpr EGLenum kPlatformGbm = 0x31D7;
        if (platform == kPlatformGbm && native_display != nullptr &&
            Wayland::IsWaylandDisplay(reinterpret_cast<Uint64>(native_display), EGL_NONE)) {
            platform = Wayland::kPlatformWayland;
        }
#endif
        return state->GetPlatformDisplay(platform, native_display);
    }

    EGLSurface CreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window,
                                           const EGLAttrib* attrib_list) {
        auto* state = GetState();
        if (!state) {
            return EGL_NO_SURFACE;
        }
#if MOBILEGL_WAYLAND_WINDOWS
        if (RefuseGbmWindow(state, dpy)) return EGL_NO_SURFACE;
        {
            const std::lock_guard<std::recursive_mutex> operationLock(EGLOperationMutex());
            EGLSurface waylandSurface = EGL_NO_SURFACE;
            if (CreateWaylandWindowSurface(state, dpy, config, native_window, /*platformWindow=*/true,
                                           &waylandSurface)) {
                return waylandSurface;
            }
        }
#endif
#if MOBILEGL_BUILD_DISAGGREGATED
        // P12 (on-screen server window), D1: as CreateWindowSurface - with MOBILEGL_IPC_SURFACE=server
        // the window is the server's, and a headless client's NULL is accepted.
        const Bool serverOwnedWindow = MG_Config::ServerOwnedWindowSurfaces();
        if (native_window == nullptr && !serverOwnedWindow) {
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
        EGLSurface surface = serverOwnedWindow
                                 ? state->CreateServerOwnedWindowSurface(
                                       dpy, config, native_window, static_cast<EGLint>(windowHandle.Width),
                                       static_cast<EGLint>(windowHandle.Height), /*platformWindow=*/true)
                                 : state->CreatePlatformWindowSurface(dpy, config, native_window, attrib_list);
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
        void* proc = MG_Impl::GetProcAddress(name);
        if (!proc) {
            MGLOG_D("Failed to get function: %s", name);
            return nullptr;
        }
        return (__eglMustCastToProperFunctionPointerType)proc;
    }
} // namespace MobileGL::MG_Impl::EGLImpl
