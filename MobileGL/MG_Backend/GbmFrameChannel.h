// MobileGL - MobileGL/MG_Backend/GbmFrameChannel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include "HostFrameBridge.h"

struct AHardwareBuffer;

namespace MobileGL::MG_Backend {
    // THE FRAME THE HOST HANDED OVER, HANDED ON TO THE CONTAINER AS A dma-buf.
    //
    // The display host owns a pool of AHardwareBuffers: it allocates them, this side imports the
    // one it offers as a draw target (HostFrameBridge, DirectGLES::HostFrameTarget), and the host
    // puts it on the glass when this side answers that it is drawn.  The Wayland compositor in the
    // Linux container has to render into THOSE SAME frames - it runs this project's own GBM, and a
    // gbm_bo_create there answers with the phone's frame instead of a dma-heap allocation of its
    // own - so every frame this side takes has to reach the container as that frame's dma-buf
    // descriptor.  That, and the release that comes back, is all this channel carries.
    //
    // The container's end is MobileGL/MG_Gbm/GbmHostChannel.cpp.  It reads exactly the bytes
    // declared below and answers with a release once its compositor no longer reads the frame.
    // The three shapes ARE that file's GbmFrameOffer/GbmFrameRelease and GbmInternal.h's
    // AHBNativeHandleHeader, field for field and packing included.  They are restated here rather
    // than shared because the two ends are built into different libraries from different build
    // configurations: MG_Backend is the Android render server and never sees libgbm's headers,
    // while MG_Gbm is built for the container and never sees Android's.  A change on either side
    // is a protocol change, and the magic is what makes a mismatched pair fail loudly instead of
    // being read as plausible numbers.
    //
    // ONE MESSAGE PER FRAME, on SOCK_SEQPACKET, and that is part of the protocol rather than an
    // implementation detail: an offer and its handle only mean anything together, and the message
    // boundary is what lets the reader take the two as one thing.  The container derives the
    // length of the message it expects from the handle's own two counts and refuses any other
    // length, so this end sends the offer immediately followed by the handle's counts and its two
    // arrays, with the descriptors in the SCM_RIGHTS control data of that same sendmsg.
    //
    // The channel is OPTIONAL, and that is the rule the implementation is built around: a
    // container that is not running, has died, or has stopped reading may cost the render path a
    // syscall or two, and may never block it, fail it, or grow without bound.  Every entry point
    // below is non-blocking and answers silence with silence.
    inline constexpr Uint32 kGbmFrameOfferMagic = 0x4647474Du;   // 'M','G','G','F' in memory order
    inline constexpr Uint32 kGbmFrameReleaseMagic = 0x5247474Du; // 'M','G','G','R' in memory order
    inline constexpr Uint32 kGbmFrameVersion = 1u;
    // WHERE THE CONTAINER DIALS.  /data/local/tmp is the one directory both halves of a split run
    // can be pointed at on a device, and the default lives on this end because this end is the
    // server.  The container's compiled-in default is /run/mobilegl-gbm.sock - the same protocol
    // with the path each side can reach unaided - so a deployment that puts the two in different
    // mount namespaces sets MOBILEGL_GBM_SOCKET on both; it is read here and in
    // GbmHostChannelCreate, from the same variable, for that reason.
    inline constexpr const char* kGbmFrameSocketPath = "/data/local/tmp/mobilegl-gbm.sock";
    inline constexpr const char* kGbmFrameSocketEnv = "MOBILEGL_GBM_SOCKET";

    // One frame the host offered, described for the container.  Width, Height, StrideBytes and
    // Format are the host's own numbers for the frame (HostFrameOffer), carried through
    // unchanged: they describe the memory the descriptor points at, and this end is not the place
    // where a number the other two ends agreed on gets rewritten.
    struct GbmFrameOffer {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Seq;         // this channel's, one per frame offered
        Uint32 Index;       // the host's own buffer index, echoed so the phone can match a frame
        Uint32 Width;
        Uint32 Height;
        Uint32 StrideBytes;
        Uint32 Format;
        Uint32 UsageLo;
        Uint32 UsageHi;
    } __attribute__((packed));

    // What the container says once it is done with a frame.  It may arrive late, it may never
    // arrive, and both are cheap here: the channel keeps a count and no per-frame record (see
    // Outstanding below), so nothing is leaked and nothing grows when one is missed.
    struct GbmFrameRelease {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Seq;
    } __attribute__((packed));

    // An AHardwareBuffer native handle as it appears on the wire: the platform's native_handle_t
    // (version, then the two counts, then the descriptor array followed by the integer array),
    // written by AHardwareBuffer_sendHandleToUnixSocket and read by the container's
    // AHBNativeHandleHeader.  The counts are 32 bit on every Android ABI, which is why they are
    // spelled out here rather than taken from a platform header this library cannot include.
    struct GbmNativeHandleHeader {
        Int32 Version;
        Int32 NumFds;
        Int32 NumInts;
    } __attribute__((packed));

    // ONE CONTAINER, ONE LISTENING SOCKET, ONE THREAD.
    //
    // There is no lock in here on purpose: the channel is opened, offered to and closed from the
    // apply thread, which is the one thread that takes the host's frames and draws them, so there
    // is no second thread to order against - the same reason HostFrameBridge has none.
    class GbmFrameChannel {
    public:
        GbmFrameChannel() = default;
        GbmFrameChannel(const GbmFrameChannel&) = delete;
        GbmFrameChannel& operator=(const GbmFrameChannel&) = delete;
        ~GbmFrameChannel();

        // Creates the listening socket if it is not there yet, and does nothing otherwise.  It
        // never waits for a container: the socket is bound, listen(1) is called, and the accept
        // happens later, when a frame is actually offered.  A caller that wants the container to
        // find the socket before the first frame - the container dials once at its own device
        // creation and retries about once a second - can call this when the host-framed surface
        // is initialized, beside HostFrameBridge::Open.
        Bool Open();

        // Offers one frame the host just handed over, as the dma-buf descriptor behind it.
        // False means the frame did not go out, for any of the reasons this channel treats as
        // ordinary: no container is connected, the container just died, or the frame could not be
        // described.  None of them is the caller's problem and none of them is fatal - the frame
        // is drawn and presented exactly as it would have been without a channel at all.
        Bool OfferFrame(struct AHardwareBuffer* buffer, const HostFrameOffer& offer);

        // Reads whatever the container has said since the last call and marks the channel down if
        // the connection has ended.  OfferFrame already does this once per frame, so a caller only
        // needs it if it wants releases drained on a path that has no new frame in it.
        void Poll();

        // True while a container is connected and the frames are being offered to it.
        Bool IsUp() const { return m_client >= 0; }

        // How many frames have been offered and not released, as a count of messages and not a
        // record of them.  It is here to be logged, and it is deliberately the only per-frame state
        // the channel keeps: a release that never arrives costs a number that is too high, and
        // nothing else, which is what makes a container that stops reading harmless.
        Uint64 Outstanding() const { return m_outstanding; }

        // Closes the connection, the listening socket and the node it created.  Idempotent, safe
        // from a destructor, and safe to call while a container is connected: the container sees
        // the connection end and goes back to allocating its frames locally, which is the state it
        // starts in.
        void Close();

    private:
        // The socketpair a frame's handle is handed to and read back from.  Recreated on demand,
        // because the one call the NDK offers for a buffer's descriptors sends them as a message
        // of its own and the container takes one message per frame - see the note in the .cpp.
        Bool EnsureRelay();
        void DropRelay();
        Bool RelayHandle(struct AHardwareBuffer* buffer, Uint8* payload, SizeT capacity, SizeT& payloadBytes,
                         Int* fds, Int& fdCount);

        // Takes the one container the protocol allows, and closes anyone else who dials while it
        // is connected rather than leaving them to believe they are being served.
        void AcceptDialer();

        // Closes the connection and says why, once.  The channel stays open for the next container.
        void MarkDown(const char* what, Int error);

        Int m_listen = -1;
        Int m_client = -1;
        Int m_relay[2] = {-1, -1};
        String m_path;
        // Bumped only for a frame that actually went out, so a sequence number is never reused and
        // a release can never be read as belonging to a frame of an earlier connection.
        Uint32 m_nextSeq = 0;
        Uint64 m_offered = 0;
        Uint64 m_released = 0;
        Uint64 m_outstanding = 0;
    };
}
