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
// swap the frame is put into a wl_buffer and attached to the wl_surface.
//
// Two kinds of wl_buffer. With a backend that has shared images (a split client) and a compositor
// offering zwp_linux_dmabuf_v1, each buffer is a server-allocated image exported as a dma-buf and
// the server copies the frame into it GPU-side: nothing crosses the CPU. Otherwise - no such
// global, no shared images, a refused import, MOBILEGL_WAYLAND_DMABUF=0 - the frame is read back
// into a wl_shm buffer.
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
#include <MG_Util/Damage/Damage.h>

namespace MobileGL::MG_Impl::EGLImpl {
    // The current context's default-framebuffer frame, `width` x `height`, written top-down in
    // B, G, R, A byte order (a wl_shm ARGB8888 buffer and a 24/32-bit little-endian X image both
    // hold that) to `dst`, one row every `dstStride` bytes. Through this library's own entry
    // points: the application's read framebuffer, pack buffer and pack state are saved and put
    // back, so the readback is invisible to it. `scratch` is the caller's, reused across frames.
    void ReadBackFrameBGRA(EGLint width, EGLint height, Vector<Uint8>& scratch, Uint8* dst, SizeT dstStride);
} // namespace MobileGL::MG_Impl::EGLImpl

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    // EGL_PLATFORM_WAYLAND_KHR and EGL_PLATFORM_WAYLAND_EXT are the same enum.
    inline constexpr EGLenum kPlatformWayland = 0x31D8;

    // Whether a display is a Wayland one: asked for as EGL_PLATFORM_WAYLAND, or (eglGetDisplay with
    // no platform) a native display that is a wl_display - recognised the way Mesa does, by its
    // first word being libwayland-client's wl_display_interface (or, for an application with its
    // own copy of libwayland, an interface named "wl_display").
    Bool IsWaylandDisplay(Uint64 nativeDisplay, EGLenum platform);

    // Whether `window` looks like a wl_egl_window this library can present to.
    Bool IsWaylandWindow(const void* window);

    // The size the application last gave its wl_egl_window.
    Bool WindowSize(const void* window, EGLint* width, EGLint* height);

    // One window surface's presentation: the dma-buf or shm buffers, the event queue they are
    // dispatched on, and the readback scratch.  Created for a wl_egl_window on a wl_display; null
    // with the reason logged when libwayland-client or the compositor's wl_shm is not there.
    class WindowSurface {
    public:
        static UniquePtr<WindowSurface> Create(void* wlDisplay, void* wlEglWindow, EGLint width, EGLint height,
                                               Bool hasAlpha);
        ~WindowSurface();
        WindowSurface(const WindowSurface&) = delete;
        WindowSurface& operator=(const WindowSurface&) = delete;

        // Puts the current context's default-framebuffer frame on the window: copied into a
        // shared image (or read back into a wl_shm buffer, the application's read-framebuffer and
        // pack state put back exactly as they were), then attach, damage, commit.  A shared-image
        // present that fails moves the window to wl_shm for good.  False (logged) when the frame
        // did not reach the window; the swap itself goes on regardless.
        //
        // `damage` (GL window coordinates, clipped to the window; Full = all of it) is what the
        // frame changed. A shared image is copied only where it differs from the frame - the
        // frame's damage and every frame's since that image was last written - and the surface is
        // damaged only where the frame changed, flipped to the buffer's top-left origin.
        //
        // `swapInterval` above 0 paces the swaps by the compositor's frame callbacks: this waits
        // for the previous frame's before taking a buffer (FrameThrottle.h).
        Bool Present(const MG_Util::Damage::Region& damage, Int swapInterval);

        // wl_egl_window_resize's effect, taken: true (with the new size) when the application gave
        // its wl_egl_window a size this presentation is not at yet. The presentation adopts it -
        // its next buffer is that size - and the caller resizes the drawable behind it.
        Bool TakeResize(EGLint* width, EGLint* height);

        struct Impl;

    private:
        explicit WindowSurface(UniquePtr<Impl> impl);
        UniquePtr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland

#endif
