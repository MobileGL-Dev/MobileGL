// MobileGL - MobileGL/MG_Impl/GLXImpl/X11PresentXcb.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// The X side of X11Present over libxcb: DRI3, Present, XFixes and MIT-SHM on the application's own
// connection (XGetXCBConnection of its Display - this library never opens one). Every libxcb entry
// point is resolved at run time, like the rest of the GLX layer, so nothing links libxcb; the
// protocol headers are only needed at build time, and a build without them has no DRI3 path
// (MOBILEGL_GLX_XCB_PRESENT is 0 and every window takes the readback path).

#include "X11Present.h"

#if defined(__linux__) && !defined(__ANDROID__) && __has_include(<xcb/dri3.h>) && __has_include(<xcb/present.h>) && \
    __has_include(<xcb/xfixes.h>) && __has_include(<xcb/shm.h>)
#define MOBILEGL_GLX_XCB_PRESENT 1
#else
#define MOBILEGL_GLX_XCB_PRESENT 0
#endif

namespace MobileGL::MG_Impl::GLXImpl::X11Present {
    // What the connection offers (queried once per connection). SharedImages is left false: that
    // one is the backend's to answer.
    ConnectionCaps QueryConnectionCaps(void* xcbConnection);

    // A window's Present events on a special-event queue of their own, its DRI3 imports and its
    // PresentPixmaps. Null when the connection lacks DRI3/Present or the queue cannot be set up.
    UniquePtr<Connection> CreateXcbConnection(void* xcbConnection, Uint32 window, const ConnectionCaps& caps);

    // THE MIT-SHM FALLBACK: frames read back into a shared segment (a memfd passed with
    // ShmAttachFd, so no SysV memory has to be shared with the X server) and put with ShmPutImage -
    // the pixels reach the X server without travelling down its socket. Two segments, alternating;
    // a segment is written again only after a round trip queued behind its last put has come back.
    class ShmPresenter {
    public:
        virtual ~ShmPresenter() = default;
        // `fill(rows, stride)` writes a width x height top-down 32bpp frame; then it is put on
        // `drawable` with `gc` at (0,0). False: not shown (the segment could not be made).
        virtual Bool Put(Uint32 drawable, Uint32 gc, Uint32 depth, Int32 width, Int32 height,
                         const std::function<void(Uint8*, SizeT)>& fill) = 0;
    };
    UniquePtr<ShmPresenter> CreateShmPresenter(void* xcbConnection);
} // namespace MobileGL::MG_Impl::GLXImpl::X11Present
