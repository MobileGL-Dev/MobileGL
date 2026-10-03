// MobileGL - MobileGL/MG_Impl/EGLImpl/SharedImageFlushPolicy.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// WHEN glFlush IS A SHARED-IMAGE BOUNDARY (docs/Disaggregated/notes/anland/plan-x11-gpu.md).
//
// A dma-buf producer that renders into its buffers and hands them on after a glFlush (an X
// server's glamor committing a window pixmap) relies on implicit sync: whoever samples the buffer
// next is ordered after the flushed work by the kernel. MobileGL's images are ordered by its server
// instead, and the server learns of a write at a boundary - a swap, a present, or this flush. So a
// flush becomes a round trip that publishes the session's accesses, but only in a process for which
// that can matter: it has bound a shared image to a texture, it does not own the server's window
// (the compositor samples client buffers and ends its frames at its swaps), and nobody turned it
// off. MOBILEGL_SHARED_IMAGE_FLUSH_SYNC=1 forces it on (still only with shared images), =0 off.
//
// Header-only and pure: a host test pins the table.

#include <Includes.h>

#include <cstring>

namespace MobileGL::MG_Impl::EGLImpl {
    inline Bool FlushPublishesSharedImageAccesses(Bool sharedImagesAvailable, Bool boundSharedImageToTexture,
                                                  Bool ownsServerWindow, const char* overrideValue) {
        if (!sharedImagesAvailable) return false;
        if (overrideValue != nullptr && overrideValue[0] != '\0') {
            if (std::strcmp(overrideValue, "0") == 0 || std::strcmp(overrideValue, "off") == 0 ||
                std::strcmp(overrideValue, "false") == 0)
                return false;
            if (std::strcmp(overrideValue, "1") == 0 || std::strcmp(overrideValue, "on") == 0 ||
                std::strcmp(overrideValue, "true") == 0)
                return true;
        }
        return boundSharedImageToTexture && !ownsServerWindow;
    }
} // namespace MobileGL::MG_Impl::EGLImpl
