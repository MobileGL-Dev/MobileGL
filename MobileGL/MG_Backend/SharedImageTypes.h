// MobileGL - MobileGL/MG_Backend/SharedImageTypes.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend {
    // SHARED IMAGES: server-allocated color
    // buffers exported to clients as dma-buf descriptors.
    //
    // The server-side view of one image, as a backend sees it: `NativeBuffer` is the image's
    // AHardwareBuffer (null where there is none), `Fourcc` a DRM fourcc - ABGR8888 (R, G, B, A in
    // memory, GL RGBA8) or XBGR8888 (alpha ignored); ARGB8888 / XRGB8888 images are stored the
    // same way (SharedImageRegistry.h says why), so a backend binds every one as RGBA8.
    struct SharedImageView {
        Uint64 Id = 0;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Fourcc = 0;
        void* NativeBuffer = nullptr;
    };

    // The client-side answer of an allocation: the image's id and its exported descriptor
    // (`Fd`, owned by the caller), with the plane layout a linux-dmabuf wl_buffer is created with.
    struct SharedImageExport {
        Uint64 Id = 0;
        int Fd = -1;
        Uint32 Width = 0;
        Uint32 Height = 0;
        Uint32 Fourcc = 0;
        Uint32 Stride = 0;
        Uint32 Offset = 0;
        Uint64 Modifier = 0;
        // A YUV image's Cb Cr plane: in the same descriptor, at this offset and pitch.
        Uint32 Plane1Stride = 0;
        Uint32 Plane1Offset = 0;
    };

    // An import of a two-plane (YUV) dma-buf: where its planes are in the one descriptor, and
    // EGL_EXT_image_dma_buf_import's color hints (0 = not given).
    struct SharedImageImportLayout {
        Uint32 PlaneCount = 1;
        Uint32 Offset[2] = {0, 0};
        Uint32 Pitch[2] = {0, 0};
        Uint32 ColorSpace = 0;
        Uint32 Range = 0;
        Uint32 SitingX = 0;
        Uint32 SitingY = 0;
        // The DRM format modifier the import named; HasModifier false = none (the implicit one).
        Bool HasModifier = false;
        Uint64 Modifier = 0;
    };
} // namespace MobileGL::MG_Backend
