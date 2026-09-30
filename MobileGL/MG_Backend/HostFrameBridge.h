// MobileGL - MobileGL/MG_Backend/HostFrameBridge.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

struct AHardwareBuffer;

namespace MobileGL::MG_Backend {
    // THE DISPLAY HOST'S FRAMES, AS THEY ARRIVE.
    //
    // The Android side that owns the screen allocates its frames itself and offers each one over
    // an AF_UNIX socket: the handle travels ahead of a fixed-size description on the same
    // connection, and the host then waits for this side to say the frame is drawn before it puts
    // it on the glass.  Why the host allocates rather than this side asking for the display's own
    // buffers is measured, not chosen: AHardwareBuffer_createFromHandle accepts an
    // AHardwareBuffer this process allocated and refuses one the display queue handed over
    // (gralloc BAD_VALUE), so the only frames a render server can draw into are the ones the host
    // made for it.
    //
    // The three message shapes below ARE the host's (its ahb_bridge.h), field for field and
    // packing included.  They are restated here rather than shared because the two ends are built
    // by different projects; a change on either side is a protocol change, and the magic is what
    // makes a mismatched pair fail loudly instead of being read as plausible numbers.
    inline constexpr Uint32 kHostFrameMagic = 0x4D474C41u;  // the host spells it "ALGM" in memory
    inline constexpr Uint32 kHostFrameVersion = 1u;
    inline constexpr const char* kHostFrameSocket = "/data/local/tmp/mobilegl_bridge.sock";

    struct HostFrameOffer {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Index;
        Uint32 Width;
        Uint32 Height;
        Uint32 StrideBytes;
        Uint32 Format;
        Uint32 UsageLo;
        Uint32 UsageHi;
        char Note[64];
    } __attribute__((packed));

    struct HostFrameAck {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Index;
        Uint32 Imported;
        Uint32 Drawn;
        Uint32 FenceOk;
        Uint8 Color[4];
        char Note[64];
    } __attribute__((packed));

    struct HostFrameSeen {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Index;
        Uint32 Locked;
        Uint8 Observed[4];
        Uint8 ObservedCorner[4];
        char Note[64];
    } __attribute__((packed));

    // One host connection at a time, which is what the host offers: it dials, hands over a frame,
    // waits for the answer, and hands over the next.
    class HostFrameBridge {
    public:
        HostFrameBridge() = default;
        HostFrameBridge(const HostFrameBridge&) = delete;
        HostFrameBridge& operator=(const HostFrameBridge&) = delete;
        ~HostFrameBridge();

        // Listens on `path` (the well-known name by default) and waits for the host.  A host that
        // never dials is answered with false and a reason, never an abort.
        Bool Open(const char* path, Int acceptTimeoutMs, String& why);

        // Takes the next frame.  Ownership of the returned AHardwareBuffer is the caller's, who
        // must hand it back through ReleaseOwned() once the frame is off the glass.
        Bool Acquire(struct AHardwareBuffer** buffer, HostFrameOffer& offer, String& why);

        // Tells the host what became of the frame it offered, and reads back what the host's own
        // view of the pixels holds.  `drawn` is what the host waits for before it presents.
        Bool Complete(const HostFrameOffer& offer, Bool imported, Bool drawn, Bool fenceOk, String& why);

        // The readback the host returned for the last completed frame, as the host reported it.
        const HostFrameSeen& LastSeen() const { return m_lastSeen; }

        void Close();

    private:
        Int m_fd = -1;
        Int m_listen = -1;
        HostFrameSeen m_lastSeen{};
    };
}
