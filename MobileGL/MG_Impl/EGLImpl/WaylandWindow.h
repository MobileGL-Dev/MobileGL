// MobileGL - MobileGL/MG_Impl/EGLImpl/WaylandWindow.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// WAYLAND WINDOW SURFACES.  A Wayland client hands eglCreateWindowSurface a wl_egl_window, and what
// the compositor shows is whatever wl_buffer the client attaches to that window's wl_surface.  The
// pixels of a MobileGL frame are drawn where the backend is - on a split build, in another process
// on another OS - so a window surface here is a pbuffer the backend draws into, plus this: at every
// swap the frame is read back and attached to the wl_surface as a wl_shm buffer.
//
// libwayland-client is loaded at run time (it is already in every Wayland client's process), so the
// library neither links it nor needs its headers to build.

#include <Includes.h>

#if defined(__linux__) && !defined(__ANDROID__)
#define MOBILEGL_WAYLAND_WINDOWS 1
#else
#define MOBILEGL_WAYLAND_WINDOWS 0
#endif

#if MOBILEGL_WAYLAND_WINDOWS

#include <EGL/egl.h>

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    // EGL_PLATFORM_WAYLAND_KHR and EGL_PLATFORM_WAYLAND_EXT are the same enum.
    inline constexpr EGLenum kPlatformWayland = 0x31D8;

    // Whether a display is a Wayland one: asked for as EGL_PLATFORM_WAYLAND, or (eglGetDisplay with
    // no platform) a native display that is a wl_display - recognised the way Mesa does, by its
    // first word being libwayland-client's wl_display_interface.
    Bool IsWaylandDisplay(Uint64 nativeDisplay, EGLenum platform);

    // Whether `window` looks like a wl_egl_window this library can present to.
    Bool IsWaylandWindow(const void* window);

    // The size the application last gave its wl_egl_window.
    Bool WindowSize(const void* window, EGLint* width, EGLint* height);

    // One window surface's presentation: the shm buffers, the event queue they are dispatched on,
    // and the readback scratch.  Created for a wl_egl_window on a wl_display; null with the reason
    // logged when libwayland-client or the compositor's wl_shm is not there.
    class WindowSurface {
    public:
        static UniquePtr<WindowSurface> Create(void* wlDisplay, void* wlEglWindow, EGLint width, EGLint height,
                                               Bool hasAlpha);
        ~WindowSurface();
        WindowSurface(const WindowSurface&) = delete;
        WindowSurface& operator=(const WindowSurface&) = delete;

        // Reads the frame back out of the current context's default framebuffer and puts it on
        // the window: attach, damage, commit.  The application's read-framebuffer and pack state
        // are put back exactly as they were.  False (logged) when the frame did not reach the
        // window; the swap itself goes on regardless.
        Bool Present();

        // wl_egl_window_resize's effect, taken: true (with the new size) when the application gave
        // its wl_egl_window a size this presentation is not at yet. The presentation adopts it -
        // its next wl_shm buffer is that size - and the caller resizes the drawable behind it.
        Bool TakeResize(EGLint* width, EGLint* height);

        struct Impl;

    private:
        explicit WindowSurface(UniquePtr<Impl> impl);
        UniquePtr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland

#endif
