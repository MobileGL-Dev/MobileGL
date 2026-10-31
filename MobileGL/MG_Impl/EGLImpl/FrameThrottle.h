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
// decided it would not be seen - and a compositor that shows nothing (its output is off, the window
// is hidden) sends none at all. Every commit asks for one (one outstanding at a time), and the swap
// after it may wait:
//
//   - swap interval above 0: until the callback is done, however long that takes. An application
//     drawing as fast as eglSwapBuffers lets it then draws at the compositor's pace, and stops while
//     the compositor shows nothing (what every EGL on Wayland does).
//   - swap interval 0: not while the compositor keeps answering - an unpaced application stays
//     unpaced. But a callback left unanswered for kStarvedAfterMs means nothing of the window is
//     being shown; the swap then waits up to kStarvedWaitMs for it, so an application that draws on
//     its own clock (a browser on an animated page) drops to about one frame a second instead of
//     burning CPU and GPU on frames nobody sees, and is back at full rate with the first answer.
//
// The bookkeeping, apart from libwayland (WaylandWindow.cpp issues the requests and dispatches).

#include <Includes.h>

namespace MobileGL::MG_Impl::EGLImpl::Wayland {
    class FrameThrottle {
    public:
        static constexpr Int64 kStarvedAfterMs = 200;
        static constexpr Int64 kStarvedWaitMs = 1000;
        static constexpr Int64 kForever = -1;

        // Before a commit: whether it carries a frame request (none is outstanding).
        Bool WantsFrameRequest() const { return !m_outstanding; }

        // A frame request went with the commit made at `nowMs` (a monotonic clock).
        void Requested(Int64 nowMs) {
            m_outstanding = true;
            m_requestedAtMs = nowMs;
        }

        // The compositor answered the request (wl_callback.done).
        void Done() { m_outstanding = false; }

        // Before a swap's frame is drawn into a buffer, at `nowMs`: how long it waits for the
        // outstanding callback - 0 = not at all, kForever = until it comes.
        Int64 WaitBudgetMs(Int swapInterval, Int64 nowMs) const {
            if (!m_outstanding) return 0;
            if (swapInterval > 0) return kForever;
            return nowMs - m_requestedAtMs >= kStarvedAfterMs ? kStarvedWaitMs : 0;
        }

        Bool Outstanding() const { return m_outstanding; }

    private:
        Bool m_outstanding = false;
        Int64 m_requestedAtMs = 0;
    };
} // namespace MobileGL::MG_Impl::EGLImpl::Wayland
