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
// the driver share an address space. Naming them as nine callbacks plus one forward
// terminator (MGPipeContext::ResourceSubDataComplete) is the deliberate deviation (D8).
//
// NINE, NOT THE TEN PLAN B WROTE. The tenth was OnXfbScatterReady, and it went with the
// design it belonged to: plan B put the XFB scatter on the CLIENT (the server would hand
// back the packed scratch and the client would run the patch loop over its own shadow), so
// there had to be a callback that told the client the layout. P5c/P5f went the other way -
// the server reads its OWN staged shadow, scatters there and returns the reconciled range
// through OnBufferWriteback, which needs no second callback - and the declaration was left
// behind with zero producers, zero consumers and no EventKind of its own. Deleted here
// (P3b/P4b espryt D1 slice 3) rather than carried: an entry that cannot fire is one every
// reader of this file has to rule out by hand, and the size assertion below made it look
// load-bearing.
//
// Installed at context creation. In a monolith these are direct calls; under split they are
// records on the reverse channel, and their ORDER is a correctness requirement rather than
// an optimization (section 7.4).
namespace MobileGL::MG_Pipe {
    struct MGPipeCallbacks {
        // A driver-detected GL error that only the server could have seen. The text is
        // borrowed for this synchronous call; a transport producer copies it before return.
        void (*OnGlError)(Uint32 code, const char* message);
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
    };

    // Nine, and the count is asserted so a tenth cannot be added without touching the
    // transport's reverse-channel record table.
    inline constexpr SizeT kMGPipeCallbackCount = 9;
    static_assert(sizeof(MGPipeCallbacks) == kMGPipeCallbackCount * sizeof(void (*)()),
                  "MGPipeCallbacks gained or lost a callback");

    // Null-initialized: a backend that installs nothing sends nothing.
    inline MGPipeCallbacks gMGPipeCallbacks{};

#if MOBILEGL_BUILD_DISAGGREGATED
    // HOW MUCH OF ITSELF ONE REVERSE-CHANNEL RECORD MAY BE, and it is not a tuning knob: a
    // producer that ignores it kills the server process.
    //
    // OnBufferWriteback's bytes travel INLINE in one SEG_EVENT record, and a ring producer
    // refuses any record above Capacity()/2 OUTRIGHT (Transport/Ring.h) - not "when full", but
    // always, because above half a capacity a record is placeable at some head offsets and not
    // at others, so waiting for room would be a hang. The server's answer to a refusal is
    // Fatal{EventRingOverflow}, and SEG_EVENT defaults to 256 KiB, so ANY producer that posts a
    // whole range wider than ~128 KiB is a server that dies on a large enough buffer.
    //
    // THE PRODUCERS ARE IN MG_Backend, which cannot reach MG_Remote::Client's
    // BufferWritebackSliceBytes (that one is the CLIENT-linkage answer to the same question,
    // for requests going the other way) and must not reach ServerSession either. So the
    // transport publishes the one number through this hook, exactly as it publishes segment
    // resolution through gMGPipeSegmentResolver: the SERVER role installs it at Accept and
    // releases it at Close, and a monolith build leaves it null.
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
#endif
} // namespace MobileGL::MG_Pipe
