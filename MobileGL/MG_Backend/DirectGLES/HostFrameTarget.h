// MobileGL - MobileGL/MG_Backend/DirectGLES/HostFrameTarget.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

struct AHardwareBuffer;

namespace MobileGL::MG_Backend::DirectGLES {
    // THE FRAME THE DISPLAY HOST IS ABOUT TO SCAN OUT, AS A DRAW TARGET.
    //
    // The display host (the Android side that owns the screen) allocates its frames itself and
    // offers each one over a socket as an AHardwareBuffer.  That is not a convenience: measured
    // on the device, AHardwareBuffer_createFromHandle accepts an AHardwareBuffer this side
    // allocated and refuses one the display queue handed over (gralloc BAD_VALUE), so the frames
    // a render server can draw into are the ones the host made for it.
    //
    // What this makes of such a frame: an EGLImage the driver accepts as a renderbuffer, and a
    // framebuffer that is the drawing target while that frame is the current one.  None of
    // Android's window machinery is involved - no ANativeWindow, no SurfaceFlinger - which is
    // the point: the host puts the frame on the glass when this side says it is drawn.
    struct HostFrameTarget {
        void* Image = nullptr;   // EGLImageKHR; opaque here so this header stays free of EGL types
        Uint Renderbuffer = 0;
        Uint Framebuffer = 0;
        Uint Width = 0;
        Uint Height = 0;
    };

    // Whether the driver advertises what this is spelled in.  False is answered with the missing
    // name in `why`, never an abort: a server that cannot take host frames has to be able to
    // say so.
    Bool HostFrameTargetSupported(String& why);

    // Imports `buffer` as a render target.  On failure `target` is left empty and `why` says
    // which call refused; no GL name is leaked.
    Bool HostFrameTargetCreate(const struct AHardwareBuffer* buffer, HostFrameTarget& target, String& why);

    // Binds the target as the current draw framebuffer, called when its frame becomes current.
    void HostFrameTargetBind(const HostFrameTarget& target);

    // Drops the GL names and the EGLImage.  The AHardwareBuffer belongs to the bridge, which
    // releases it; this never touches it.
    void HostFrameTargetDestroy(HostFrameTarget& target);
}
