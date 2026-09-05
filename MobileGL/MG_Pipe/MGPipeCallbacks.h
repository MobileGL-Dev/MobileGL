// MobileGL - MobileGL/MG_Pipe/MGPipeCallbacks.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include "MGPipeHandles.h"
#include "MGPipeTypes.h"

// The backend -> frontend reverse channel, named (plan B section 7.1).
//
// Today this traffic is 95 call sites across 17 methods poked directly into frontend
// objects. gallium has no vocabulary for shadow writeback, GPU-write notification, texture
// re-send requests or default-framebuffer geometry, because in Mesa the state tracker and
// the driver share an address space. Naming them as ten callbacks plus one forward
// terminator (MGPipeContext::ResourceSubDataComplete) is the deliberate deviation (D8).
//
// Installed at context creation. In a monolith these are direct calls; under split they are
// records on the reverse channel, and their ORDER is a correctness requirement rather than
// an optimization (section 7.4).
namespace MobileGL::MG_Pipe {
    struct MGPipeCallbacks {
        // A driver-detected GL error that only the server could have seen.
        void (*OnGlError)(Uint32 code);
        // Ranges of a resource the GPU wrote; retires MarkGpuWritten.
        void (*OnGpuWritten)(MGPipeHandle res, Uint rangeCount, const MGPRange* ranges);
        void (*OnBufferWriteback)(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes);
        void (*OnTextureWriteback)(MGPipeHandle res, const MGPBox* box, MGPBlobRef bytes);
        // The one new stall class in this design (D-B6): the server recast a texture and
        // needs its texels back. The client answers with zero or more ResourceSubData
        // records terminated by ResourceSubDataComplete carrying the same pullSerial.
        void (*OnTexturePullRequest)(MGPipeHandle res, Uint16 target, Uint16 firstLevel, Uint16 levelCount,
                                     Uint64 pullSerial);
        // SHAPE ONLY, never bytes: the client owns the CPU shadow and allocates the levels
        // itself.
        void (*OnMipLevelsGenerated)(MGPipeHandle res, Uint16 base, Uint16 count);
        // Retires the layering inversion where the swapchain writes into MG_Impl's
        // pDefaultFramebufferInfo.
        void (*OnSurfaceChanged)(const MGPSurfaceInfo* info);
        void (*OnCapsInvalidated)();
        // <= WARN is lossy, >= ERROR is lossless and rate limited.
        void (*OnLog)(Uint8 level, const char* text);
        // The XFB scatter is a read-modify-write of the CLIENT's shadow, so the server
        // hands back the packed scratch and the client scatters (section 7.2.1).
        void (*OnXfbScatterReady)(MGPipeHandle scratch, Uint64 packedStride, Uint64 vertices);
    };

    // Ten, and the count is asserted so an eleventh cannot be added without touching the
    // transport's reverse-channel record table.
    inline constexpr SizeT kMGPipeCallbackCount = 10;
    static_assert(sizeof(MGPipeCallbacks) == kMGPipeCallbackCount * sizeof(void (*)()),
                  "MGPipeCallbacks gained or lost a callback");

    // Null-initialized: a backend that installs nothing sends nothing.
    inline MGPipeCallbacks gMGPipeCallbacks{};
} // namespace MobileGL::MG_Pipe
