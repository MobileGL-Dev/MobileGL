// MobileGL - MobileGL/MG_Backend/EGLSurfaceRegistry.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend {
    enum class WindowBackend {
        Android,
        X11,
        MetalLayer,
        Win32, // Handle is an HWND
        // TODO: Wayland, etc.
        WindowBackendCount,
        Unknown = -1
    };

    struct WindowHandle {
        WindowBackend Backend = WindowBackend::Unknown;
        void* Handle = nullptr;
        Uint32 Width = 0;
        Uint32 Height = 0;
    };

    // The EGL bookkeeping every backend shares: the display, the registered surfaces, the one surface
    // the backend has active, and each thread's current display, surfaces and context. A backend owns
    // one and is its Hooks; everything here, hooks included, runs under the registry's one recursive
    // lock, which a backend also takes for its own per-surface state (Lock).
    class EGLSurfaceRegistry {
    public:
        enum class SurfaceKind {
            None,
            Window,
            Pbuffer
        };

        struct CurrentState {
            EGLDisplay Display = EGL_NO_DISPLAY;
            EGLSurface DrawSurface = EGL_NO_SURFACE;
            EGLSurface ReadSurface = EGL_NO_SURFACE;
            EGLContext Context = EGL_NO_CONTEXT;
        };

        struct SurfaceState {
            SurfaceKind Kind = SurfaceKind::None;
            Bool DestroyPending = false;
            // Activated at least once: a re-activation (a multi-window client switching back)
            // is a re-bind and does not re-run the backend's capability probe.
            Bool Activated = false;
            WindowHandle Window;
            EGLint Width = 1;
            EGLint Height = 1;
        };

        // The owning backend's side. Each runs on the calling thread with the lock held and may call
        // back into the registry.
        class Hooks {
        public:
            using PresentFunction = void (*)();
            // Bring up the surface ActivatingSurface() names; a window's handle is CurrentWindowHandle().
            virtual Bool InitWindowSurface() = 0;
            virtual Bool InitPbufferSurface(EGLint width, EGLint height) = 0;
            virtual Bool InitCapabilities() = 0;
            // The active surface is being dropped (before ForgetActiveSurface).
            virtual void OnSurfaceReleased(EGLSurface surface) = 0;
            // Every surface the registry drops, active or not, once it is no longer registered.
            virtual void OnSurfaceForgotten(EGLSurface surface) = 0;
            // What Swap presents with; read after the swap's own activation.
            virtual PresentFunction GetPresentFunction() const = 0;

        protected:
            ~Hooks() = default;
        };

        explicit EGLSurfaceRegistry(Hooks& hooks) : m_hooks(hooks) {}
        EGLSurfaceRegistry(const EGLSurfaceRegistry&) = delete;
        EGLSurfaceRegistry& operator=(const EGLSurfaceRegistry&) = delete;

        // The registry's lock, for a backend whose own state changes with the registry's.
        [[nodiscard]] std::unique_lock<std::recursive_mutex> Lock() const;

        // ---- the EGL operations the backend's virtuals default to ---------------------------
        Bool InitializeDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor);
        Bool CreateWindowSurface(EGLSurface surface, const WindowHandle& handle);
        Bool CreatePbufferSurface(EGLSurface surface, EGLint width, EGLint height);
        Bool ResizeSurface(EGLSurface surface, Uint32 width, Uint32 height);
        Bool MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
        Bool Swap(EGLDisplay dpy, EGLSurface draw);
        void ReleaseSurface(EGLSurface surface);
        // Every surface, the display and every thread's current state; no hook runs.
        void ReleaseAll();
        void SetWindowHandle(const WindowHandle& handle);

        // ---- the steps a backend composes its own operations from ---------------------------
        Bool RegisterWindowSurface(EGLSurface surface, const WindowHandle& handle);
        Bool RegisterPbufferSurface(EGLSurface surface, EGLint width, EGLint height);
        Bool Activate(EGLSurface surface);
        // No active surface and no thread current; the registrations and the display stay.
        void ResetRuntimeState();
        // ResetRuntimeState for a released ACTIVE surface: the backend has no active surface any
        // more, but every thread's current context and surface stay as they were (a multi-window
        // client closing one popup must not unbind its other windows' threads).
        void ForgetActiveSurface();
        // `surface` becomes the active surface as it already is natively: no hook runs, and neither
        // its Activated flag nor the capability probe moves. False when it is not registered.
        Bool AdoptActiveSurface(EGLSurface surface);
        // The window surfaces on `nativeWindow`, in the order the registry holds them.
        Vector<EGLSurface> WindowSurfacesOn(void* nativeWindow) const;
        // A window surface's window is now `width`x`height` (and `nativeWindow` when one is given);
        // the active surface's window handle becomes its window. False when it is not registered.
        Bool SetWindowSurfaceExtent(EGLSurface surface, Uint32 width, Uint32 height, void* nativeWindow = nullptr);

        // ---- queries --------------------------------------------------------------------------
        Bool DisplayInitialized() const;
        EGLDisplay Display() const;
        Bool HasActiveSurface() const;
        EGLSurface ActiveSurface() const;
        SurfaceKind ActiveSurfaceKind() const;
        // The surface Activate is bringing up while it runs InitWindowSurface / InitPbufferSurface,
        // which take no surface argument: a backend that maps client surfaces to native ones reads it
        // to re-bind an existing native surface instead of making another. Never cleared.
        EGLSurface ActivatingSurface() const;
        WindowHandle CurrentWindowHandle() const;
        Bool CapabilitiesInitialized() const;
        Bool IsRegistered(EGLSurface surface) const;
        Optional<SurfaceState> Find(EGLSurface surface) const;
        Optional<CurrentState> CurrentOn(std::thread::id thread) const;
        SizeT CurrentThreadCount() const;

    private:
        const SurfaceState* Registered(EGLSurface surface) const;
        Bool IsCurrentOnAnyThread(EGLSurface surface) const;
        void DestroyPendingIfUnused(EGLSurface surface);
        void ReleaseThread(const std::thread::id& thread);

        Hooks& m_hooks;
        mutable std::recursive_mutex m_mutex;
        WindowHandle m_windowHandle;
        EGLDisplay m_display = EGL_NO_DISPLAY;
        EGLSurface m_activeSurface = EGL_NO_SURFACE;
        EGLSurface m_activatingSurface = EGL_NO_SURFACE;
        Bool m_displayInitialized = false;
        Bool m_hasActiveSurface = false;
        Bool m_capabilitiesInitialized = false;
        SurfaceKind m_activeSurfaceKind = SurfaceKind::None;
        UnorderedMap<std::thread::id, CurrentState> m_currentThreads;
        UnorderedMap<EGLSurface, SurfaceState> m_surfaces;
    };
} // namespace MobileGL::MG_Backend
