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

// The backend -> frontend reverse channel as named callbacks (ARCHITECTURE.md §8.1): shadow
// writeback, GPU-write notification, surface geometry and the like, which an in-process driver
// interface has no names for. Installed at context creation; direct calls in a monolith,
// records on the reverse channel under split, where their order relative to the command stream
// is a correctness requirement (§8.2). Server log lines travel the control plane as LogLine
// frames (MG_Remote/Transport/LogForward.h), not this channel.
namespace MobileGL::MG_Pipe {
    struct MGPipeCallbacks {
        // A driver-detected GL error that only the server could have seen. The text is
        // borrowed for this synchronous call; a transport producer copies it before return.
        void (*OnGlError)(Uint32 code, const char* message);
        // Ranges of a resource the GPU wrote; retires MarkGpuWritten.
        void (*OnGpuWritten)(MGPipeHandle res, Uint rangeCount, const MGPRange* ranges);
        void (*OnBufferWriteback)(MGPipeHandle res, Uint64 offset, MGPBlobRef bytes);
        void (*OnTextureWriteback)(MGPipeHandle res, const MGPBox* box, MGPBlobRef bytes);
        // SHAPE ONLY, never bytes: the client owns the CPU shadow and allocates the levels
        // itself.
        void (*OnMipLevelsGenerated)(MGPipeHandle res, Uint16 base, Uint16 count);
        // Retires the layering inversion where the swapchain writes into MG_Impl's
        // pDefaultFramebufferInfo.
        void (*OnSurfaceChanged)(const MGPSurfaceInfo* info);
        void (*OnCapsInvalidated)();
    };

    // Seven, and the count is asserted so an eighth cannot be added without touching the
    // transport's reverse-channel record table.
    inline constexpr SizeT kMGPipeCallbackCount = 7;
    static_assert(sizeof(MGPipeCallbacks) == kMGPipeCallbackCount * sizeof(void (*)()),
                  "MGPipeCallbacks gained or lost a callback");

    // Null-initialized: a backend that installs nothing sends nothing.
    inline MGPipeCallbacks gMGPipeCallbacks{};

    // Size of the SEG_EVENT ring, which bounds one reverse-channel record. OnBufferWriteback
    // carries its bytes inline, and the ring producer always refuses a record above half its
    // capacity (Transport/Ring.h) with Fatal{EventRingOverflow}, so producers slice wide ranges
    // (MGPipeBufferWritebackSliceBytes). The producers live in MG_Backend and cannot reach the
    // transport, so the server role installs this query at Accept and clears it at Close; a
    // monolith build leaves it null.
    using MGPipeEventRingCapacityQuery = Uint64 (*)();
    inline MGPipeEventRingCapacityQuery gMGPipeEventRingCapacityBytes = nullptr;

    // 0 means "do not slice": no transport is installed, so the callback is a direct call with
    // no ring under it and cutting the range would only multiply the calls.
    //
    // A QUARTER of the ring rather than the MaxRecordBytes half, which is the same number and
    // the same reasoning the client side uses (MG_Remote/Client/GpuWritePending.cpp): the record
    // carries its own header and an EventBufferWritebackHead beside the payload, and the ring may
    // still hold small events posted earlier in the same verb's apply. The 4 KiB floor is there so
    // that a pathologically small operator-supplied ring cannot make a slicing loop spin at zero
    // width; such a ring is broken anyway and the producer's Fatal{EventRingOverflow} names it on
    // the first post.
    inline Uint64 MGPipeBufferWritebackSliceBytes() {
        if (gMGPipeEventRingCapacityBytes == nullptr) return 0;
        const Uint64 capacity = gMGPipeEventRingCapacityBytes();
        if (capacity == 0) return 0;
        const Uint64 quarter = capacity / 4;
        return quarter < 4096 ? 4096 : quarter;
    }
} // namespace MobileGL::MG_Pipe
