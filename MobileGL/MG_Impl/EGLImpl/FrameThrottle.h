// MobileGL - MobileGL/MG_Impl/EGLImpl/FrameThrottle.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// HOW A WAYLAND WINDOW'S SWAPS ARE PACED. A Wayland compositor tells a client when to draw next with
// wl_surface.frame: the callback is done when the compositor has used the frame - painted it, or
// decided it would not be seen - and a compositor whose output is off (DPMS off, nothing on screen)
// sends none at all. An EGL window surface with a swap interval above 0 asks for that callback with
// every commit, and the swap after it waits until it is done. So an application drawing as fast as
// eglSwapBuffers lets it draws at the compositor's pace, and stops - instead of burning CPU and GPU
// on frames nobody sees - while the compositor shows nothing. With interval 0 nothing waits.
//
// The bookkeeping, apart from libwayland (WaylandWindow.cpp issues the requests and dispatches).

#include <Includes.h>

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    class FrameThrottle {
    public:
        // Before a commit: whether it should carry a frame request. One callback is outstanding at
        // a time; the commits of an interval-0 window carry none.
        Bool WantsFrameRequest(Int swapInterval) const { return swapInterval > 0 && !m_outstanding; }

        // A frame request went with the commit.
        void Requested() { m_outstanding = true; }

        // The compositor answered the request (wl_callback.done).
        void Done() { m_outstanding = false; }

        // Before a swap's frame is drawn into a buffer: whether it has to wait for the callback
        // first. An interval of 0 never waits, even for a request a previous interval made.
        Bool MustWait(Int swapInterval) const { return swapInterval > 0 && m_outstanding; }

        Bool Outstanding() const { return m_outstanding; }

    private:
        Bool m_outstanding = false;
    };
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland
