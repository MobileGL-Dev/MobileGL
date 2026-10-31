// MobileGL - MobileGL/MG_Backend/EGLSurfaceRegistry.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EGLSurfaceRegistry.h"
#include <algorithm>

namespace MobileGL::MG_Backend {
    namespace {
        Bool IsReleaseCurrentRequest(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
            (void)dpy;
            return draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT;
        }

        std::thread::id CurrentThreadKey() {
            return std::this_thread::get_id();
        }
    } // namespace

    Bool EGLSurfaceRegistry::InitializeDisplay(EGLDisplay dpy, EGLint* major, EGLint* minor) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (dpy == EGL_NO_DISPLAY) {
            MGLOG_E("InitializeEGLDisplay failed: invalid EGLDisplay");
            return false;
        }

        // A DISPLAY IS A CONNECTION, NOT A RESOURCE THIS BACKEND OWNS, and refusing the second one
        // refused a caller for a reason that is not there: the surface and context tables are keyed
        // by the display each was created on, every operation carries its own display handle, and
        // this field has no reader anywhere - it was the whole of the rule.
        //
        // What the rule cost was a compositor.  One asks for the display it will draw into AFTER
        // probing another, so the second eglInitialize is the one that matters, and a refusal there
        // is reported one layer up as "no usable DRM render device" - the probe having succeeded
        // and the real display having failed, with nothing in between that says which was refused.
        m_display = dpy;
        m_displayInitialized = true;
        if (major) {
            *major = 1;
        }
        if (minor) {
            *minor = 5;
        }
        return true;
    }

    Bool EGLSurfaceRegistry::CreateWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return RegisterWindowSurface(surface, handle) && Activate(surface);
    }

    Bool EGLSurfaceRegistry::ResizeSurface(EGLSurface surface, Uint32 width, Uint32 height) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto surfaceIt = m_surfaces.find(surface);
        if (surfaceIt == m_surfaces.end() || surfaceIt->second.Kind == SurfaceKind::None) {
            MGLOG_E("ResizeEGLWindowSurface failed: no window surface is initialized");
            return false;
        }
        // A pbuffer-backed window (a Wayland window presented through wl_shm) resizes too.
        surfaceIt->second.Window.Width = width;
        surfaceIt->second.Window.Height = height;
        surfaceIt->second.Width = static_cast<EGLint>(std::max<Uint32>(width, 1));
        surfaceIt->second.Height = static_cast<EGLint>(std::max<Uint32>(height, 1));
        if (m_activeSurface == surface) {
            m_windowHandle.Width = width;
            m_windowHandle.Height = height;
        }
        return true;
    }

    Bool EGLSurfaceRegistry::CreatePbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return RegisterPbufferSurface(surface, width, height) && Activate(surface);
    }

    Bool EGLSurfaceRegistry::RegisterWindowSurface(EGLSurface surface, const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_displayInitialized) {
            MGLOG_E("RegisterEGLWindowSurface failed: EGL display is not initialized");
            return false;
        }
        if (surface == EGL_NO_SURFACE) {
            MGLOG_E("RegisterEGLWindowSurface failed: invalid EGLSurface");
            return false;
        }
        if (handle.Backend == WindowBackend::Unknown || !handle.Handle) {
            MGLOG_E("RegisterEGLWindowSurface failed: invalid native window handle");
            return false;
        }

        auto& state = m_surfaces[surface];
        state = SurfaceState{
            .Kind = SurfaceKind::Window,
            .Window = handle,
            .Width = static_cast<EGLint>(std::max<Uint32>(handle.Width, 1)),
            .Height = static_cast<EGLint>(std::max<Uint32>(handle.Height, 1)),
        };
        return true;
    }

    Bool EGLSurfaceRegistry::RegisterPbufferSurface(EGLSurface surface, EGLint width, EGLint height) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_displayInitialized) {
            MGLOG_E("RegisterEGLPbufferSurface failed: EGL display is not initialized");
            return false;
        }
        if (surface == EGL_NO_SURFACE) {
            MGLOG_E("RegisterEGLPbufferSurface failed: invalid EGLSurface");
            return false;
        }
        if (width <= 0 || height <= 0) {
            MGLOG_E("RegisterEGLPbufferSurface failed: invalid size %dx%d", width, height);
            return false;
        }

        m_surfaces[surface] = SurfaceState{
            .Kind = SurfaceKind::Pbuffer,
            .Width = width,
            .Height = height,
        };
        return true;
    }

    const EGLSurfaceRegistry::SurfaceState* EGLSurfaceRegistry::Registered(EGLSurface surface) const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto surfaceIt = m_surfaces.find(surface);
        return surfaceIt == m_surfaces.end() ? nullptr : &surfaceIt->second;
    }

    Bool EGLSurfaceRegistry::Activate(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto* surfaceState = Registered(surface);
        if (!surfaceState) {
            MGLOG_E("ActivateEGLSurface failed: EGL surface is not registered");
            return false;
        }
        if (m_hasActiveSurface && m_activeSurface == surface) {
            return true;
        }

        m_activatingSurface = surface;
        if (surfaceState->Kind == SurfaceKind::Window) {
            SetWindowHandle(surfaceState->Window);
            if (!m_hooks.InitWindowSurface()) {
                MGLOG_E("ActivateEGLSurface failed: backend InitWindowSurface failed");
                return false;
            }
        } else if (surfaceState->Kind == SurfaceKind::Pbuffer) {
            if (!m_hooks.InitPbufferSurface(surfaceState->Width, surfaceState->Height)) {
                MGLOG_E("ActivateEGLSurface failed: backend InitPbufferSurface failed");
                return false;
            }
        } else {
            MGLOG_E("ActivateEGLSurface failed: unsupported surface kind");
            return false;
        }

        m_activeSurface = surface;
        m_hasActiveSurface = true;
        m_activeSurfaceKind = surfaceState->Kind;
        // NOT m_currentThreads.clear(): another thread's current context and surface are its
        // own (a multi-window client renders each window on its own thread), and wiping them made
        // that thread's next eglSwapBuffers fail with "no current context attached".
        auto activatedIt = m_surfaces.find(surface);
        if (activatedIt != m_surfaces.end() && !activatedIt->second.Activated) {
            activatedIt->second.Activated = true;
            m_capabilitiesInitialized = false;
        }
        return true;
    }

    Bool EGLSurfaceRegistry::MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto threadKey = CurrentThreadKey();
        if (IsReleaseCurrentRequest(dpy, draw, read, ctx)) {
            ReleaseThread(threadKey);
            return true;
        }

        if (!m_displayInitialized || m_display != dpy) {
            MGLOG_E("MakeEGLCurrent failed: EGL display mismatch or not initialized");
            return false;
        }
        if (!m_hasActiveSurface) {
            if (draw != read || !Activate(draw)) {
                MGLOG_E("MakeEGLCurrent failed: EGL surface is not initialized");
                return false;
            }
        }
        if (!Registered(draw) || !Registered(read)) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not registered");
            return false;
        }
        if (draw != read) {
            MGLOG_E("MakeEGLCurrent failed: separate draw/read surfaces are not supported");
            return false;
        }
        if (draw != m_activeSurface && !Activate(draw)) {
            MGLOG_E("MakeEGLCurrent failed: EGL surface is not backed by this backend");
            return false;
        }
        if (draw == EGL_NO_SURFACE || read == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
            MGLOG_E("MakeEGLCurrent failed: draw/read/context is invalid");
            return false;
        }

        if (!m_capabilitiesInitialized) {
            if (!m_hooks.InitCapabilities()) {
                MGLOG_E("MakeEGLCurrent failed: InitCapabilities failed");
                return false;
            }
            m_capabilitiesInitialized = true;
        }

        ReleaseThread(threadKey);
        m_currentThreads[threadKey] = CurrentState{
            .Display = dpy,
            .DrawSurface = draw,
            .ReadSurface = read,
            .Context = ctx,
        };
        return true;
    }

    void EGLSurfaceRegistry::ResetRuntimeState() {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_hasActiveSurface = false;
        m_capabilitiesInitialized = false;
        m_activeSurfaceKind = SurfaceKind::None;
        m_activeSurface = EGL_NO_SURFACE;
        m_windowHandle = {};
        m_currentThreads.clear();
    }

    void EGLSurfaceRegistry::ForgetActiveSurface() {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_hasActiveSurface = false;
        m_activeSurfaceKind = SurfaceKind::None;
        m_activeSurface = EGL_NO_SURFACE;
        m_windowHandle = {};
    }

    Bool EGLSurfaceRegistry::Swap(EGLDisplay dpy, EGLSurface draw) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_displayInitialized || m_display != dpy) {
            MGLOG_E("SwapEGLBuffers failed: EGL display mismatch or not initialized");
            return false;
        }
        const auto currentIt = m_currentThreads.find(CurrentThreadKey());
        if (currentIt == m_currentThreads.end()) {
            MGLOG_E("SwapEGLBuffers failed: no current context attached");
            return false;
        }
        if (currentIt->second.Display != dpy || currentIt->second.DrawSurface != draw ||
            currentIt->second.Context == EGL_NO_CONTEXT) {
            MGLOG_E("SwapEGLBuffers failed: draw surface is not current on this thread");
            return false;
        }
        if (draw == EGL_NO_SURFACE || !Registered(draw)) {
            MGLOG_E("SwapEGLBuffers failed: invalid draw surface");
            return false;
        }
        // The calling thread's surface, which another thread's make-current may have moved the
        // backend off: bring it back before presenting it.
        if ((!m_hasActiveSurface || draw != m_activeSurface) && !Activate(draw)) {
            MGLOG_E("SwapEGLBuffers failed: the draw surface could not be re-activated");
            return false;
        }

        const Hooks::PresentFunction present = m_hooks.GetPresentFunction();
        if (!present) {
            MGLOG_E("SwapEGLBuffers failed: backend Present function is null");
            return false;
        }

        present();
        return true;
    }

    Bool EGLSurfaceRegistry::IsCurrentOnAnyThread(EGLSurface surface) const {
        if (surface == EGL_NO_SURFACE) {
            return false;
        }
        for (const auto& current : m_currentThreads) {
            if (current.second.DrawSurface == surface || current.second.ReadSurface == surface) {
                return true;
            }
        }
        return false;
    }

    void EGLSurfaceRegistry::DestroyPendingIfUnused(EGLSurface surface) {
        auto surfaceIt = m_surfaces.find(surface);
        if (surfaceIt == m_surfaces.end() || !surfaceIt->second.DestroyPending || IsCurrentOnAnyThread(surface)) {
            return;
        }

        m_surfaces.erase(surfaceIt);
        if (m_activeSurface == surface) {
            m_hooks.OnSurfaceReleased(surface);
            ForgetActiveSurface();
        }
        m_hooks.OnSurfaceForgotten(surface);
    }

    void EGLSurfaceRegistry::ReleaseThread(const std::thread::id& threadKey) {
        auto currentIt = m_currentThreads.find(threadKey);
        if (currentIt == m_currentThreads.end()) {
            return;
        }

        const EGLSurface drawSurface = currentIt->second.DrawSurface;
        const EGLSurface readSurface = currentIt->second.ReadSurface;
        m_currentThreads.erase(currentIt);
        DestroyPendingIfUnused(drawSurface);
        DestroyPendingIfUnused(readSurface);
    }

    void EGLSurfaceRegistry::ReleaseSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto surfaceIt = m_surfaces.find(surface);
        if (surfaceIt == m_surfaces.end()) {
            return;
        }

        if (IsCurrentOnAnyThread(surface)) {
            surfaceIt->second.DestroyPending = true;
            return;
        }

        m_surfaces.erase(surfaceIt);
        if (m_activeSurface == surface) {
            m_hooks.OnSurfaceReleased(surface);
            ForgetActiveSurface();
        }
        m_hooks.OnSurfaceForgotten(surface);
    }

    void EGLSurfaceRegistry::ReleaseAll() {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        ResetRuntimeState();
        m_surfaces.clear();
        m_display = EGL_NO_DISPLAY;
        m_displayInitialized = false;
    }

    void EGLSurfaceRegistry::SetWindowHandle(const WindowHandle& handle) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_windowHandle = handle;
    }

    std::unique_lock<std::recursive_mutex> EGLSurfaceRegistry::Lock() const {
        return std::unique_lock<std::recursive_mutex>(m_mutex);
    }

    Bool EGLSurfaceRegistry::AdoptActiveSurface(EGLSurface surface) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const SurfaceState* state = Registered(surface);
        if (state == nullptr) return false;
        m_activeSurface = surface;
        m_hasActiveSurface = true;
        m_activeSurfaceKind = state->Kind;
        if (state->Kind == SurfaceKind::Window) m_windowHandle = state->Window;
        return true;
    }

    Vector<EGLSurface> EGLSurfaceRegistry::WindowSurfacesOn(void* nativeWindow) const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        Vector<EGLSurface> surfaces;
        for (const auto& [surface, state] : m_surfaces) {
            if (state.Kind == SurfaceKind::Window && state.Window.Handle == nativeWindow) surfaces.push_back(surface);
        }
        return surfaces;
    }

    Bool EGLSurfaceRegistry::SetWindowSurfaceExtent(EGLSurface surface, Uint32 width, Uint32 height,
                                                    void* nativeWindow) {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto state = m_surfaces.find(surface);
        if (state == m_surfaces.end()) return false;
        if (nativeWindow != nullptr) state->second.Window.Handle = nativeWindow;
        state->second.Window.Width = width;
        state->second.Window.Height = height;
        state->second.Width = static_cast<EGLint>(std::max<Uint32>(width, 1));
        state->second.Height = static_cast<EGLint>(std::max<Uint32>(height, 1));
        if (m_activeSurface == surface) m_windowHandle = state->second.Window;
        return true;
    }

    Bool EGLSurfaceRegistry::DisplayInitialized() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_displayInitialized;
    }

    EGLDisplay EGLSurfaceRegistry::Display() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_display;
    }

    Bool EGLSurfaceRegistry::HasActiveSurface() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_hasActiveSurface;
    }

    EGLSurface EGLSurfaceRegistry::ActiveSurface() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_activeSurface;
    }

    EGLSurfaceRegistry::SurfaceKind EGLSurfaceRegistry::ActiveSurfaceKind() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_activeSurfaceKind;
    }

    EGLSurface EGLSurfaceRegistry::ActivatingSurface() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_activatingSurface;
    }

    WindowHandle EGLSurfaceRegistry::CurrentWindowHandle() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_windowHandle;
    }

    Bool EGLSurfaceRegistry::CapabilitiesInitialized() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_capabilitiesInitialized;
    }

    Bool EGLSurfaceRegistry::IsRegistered(EGLSurface surface) const {
        return Registered(surface) != nullptr;
    }

    Optional<EGLSurfaceRegistry::SurfaceState> EGLSurfaceRegistry::Find(EGLSurface surface) const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const SurfaceState* state = Registered(surface);
        if (state == nullptr) return Nullopt;
        return *state;
    }

    Optional<EGLSurfaceRegistry::CurrentState> EGLSurfaceRegistry::CurrentOn(std::thread::id thread) const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto current = m_currentThreads.find(thread);
        if (current == m_currentThreads.end()) return Nullopt;
        return current->second;
    }

    SizeT EGLSurfaceRegistry::CurrentThreadCount() const {
        const std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_currentThreads.size();
    }
} // namespace MobileGL::MG_Backend
