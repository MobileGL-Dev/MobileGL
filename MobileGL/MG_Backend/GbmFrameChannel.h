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

    // THE OTHER DIRECTION: A dma-buf THE CONTAINER WANTS THIS SIDE TO BACK A TEXTURE WITH.
    //
    // The compositor's scene reaches the display daemon's dma-bufs through glEGLImageTargetTexture2DOES
    // (MG_Impl/GLImpl/Exporting/Definitions.cpp, MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp both
    // name this half as the next change).  eglCreateImageKHR has already held a dup() of every plane's
    // descriptor since the image was made; what was missing is the render server's half - telling THIS
    // side which texture that dma-buf backs, over the one channel that can carry a descriptor across the
    // container boundary.  This message is that half: the client names the texture target and the
    // texture's own name, describes the buffer exactly as drmModeAddFB2 would, and the plane descriptors
    // ride in the SCM_RIGHTS control data of the same sendmsg.  One message per import, as SEQPACKET
    // requires, so a descriptor and its description are never read apart.
    //
    // The reply is what the client waits for before a framebuffer is allowed to be complete over the
    // texture: Ok=0 means this side could not make the import real, and the client keeps treating the
    // texture as having no storage rather than letting it render into nothing.
    inline constexpr Uint32 kGbmImportRequestMagic = 0x4947474Du; // 'M','G','G','I' in memory order
    inline constexpr Uint32 kGbmImportReplyMagic = 0x52474749u;   // 'I','G','G','R' in memory order
    inline constexpr Uint32 kGbmImportVersion = 1u;
    inline constexpr Uint32 kGbmImportMaxPlanes = 4u;

    struct GbmImportRequest {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Target;      // the GL texture target the image was bound to (0x8D65 = GL_TEXTURE_EXTERNAL_OES)
        Uint32 TexName;     // the CLIENT's texture name; this side resolves its twin
        Uint32 Width;
        Uint32 Height;
        Uint32 FourCC;
        Uint32 PlaneCount;  // 1..kGbmImportMaxPlanes; the descriptors are the message's control data
        Int32 Stride[kGbmImportMaxPlanes];
        Int32 Offset[kGbmImportMaxPlanes];
        Uint64 Modifier[kGbmImportMaxPlanes];
        Uint32 HasModifier[kGbmImportMaxPlanes];
    } __attribute__((packed));

    struct GbmImportReply {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Ok;
        Uint32 Reserved;
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

    // ONE CONTAINER, ONE LISTENING SOCKET, TWO THREADS: the render path's, and an acceptor's.
    //
    // A compositor dials ONCE - when it creates its GBM device - and keeps that connection for the
    // life of the process, while this side offers a frame per drawn frame.  An accept that only ran
    // inside OfferFrame therefore ran after the dial it was supposed to answer, and a dialer that
    // nobody took kept the listener's queue full for every later one.  That is measured on the
    // phone rather than argued: with accept() only on the offer path, connect() from the container
    // answered EAGAIN(11) on four attempts out of four while the node sat there listening.
    //
    // So the accept has a thread of its own, started by Open() and stopped by Close(), and it
    // adopts what it takes on the spot - including sending it the frame the host last handed over,
    // because a compositor's first allocation is the one its swapchain is built from.  The
    // connected socket, the frame that is remembered, the relay and the counters are shared between
    // the two threads and guarded by the one mutex below; both entry points are non-blocking, so
    // neither thread can park the other for longer than a syscall.
    class GbmFrameChannel {
    public:
        GbmFrameChannel() = default;
        GbmFrameChannel(const GbmFrameChannel&) = delete;
        GbmFrameChannel& operator=(const GbmFrameChannel&) = delete;
        ~GbmFrameChannel();

        // Creates the listening socket if it is not there yet and starts the acceptor, and does
        // nothing otherwise.  It never waits for a container: the socket is bound, a backlog is
        // given to listen(), and the acceptor takes whoever dials from then on - whether or not a
        // frame is being drawn.  A caller that wants the container to find the socket before the
        // first frame - the container dials once at its own device creation and retries about once
        // a second - can call this when the host-framed surface is initialized, beside
        // HostFrameBridge::Open.
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

        // The frame the host handed over is gone - it was presented, or it was replaced - and this
        // channel may no longer hold it or offer it to anybody.  THE CHANNEL ITSELF STAYS OPEN, and
        // that is the point of the call: the listening socket, its node and the container's
        // connection outlive every frame, and a channel that was closed and reopened per frame took
        // the node away sixty times a second - a container dialling at the wrong moment got ENOENT
        // for a socket that was there a millisecond earlier and would be there a millisecond later.
        void DropFrame();

        // True while a container is connected and the frames are being offered to it.
        Bool IsUp() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_client >= 0;
        }

        // How many frames have been offered and not released, as a count of messages and not a
        // record of them.  It is here to be logged, and it is deliberately the only per-frame state
        // the channel keeps: a release that never arrives costs a number that is too high, and
        // nothing else, which is what makes a container that stops reading harmless.
        Uint64 Outstanding() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_outstanding;
        }

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

        // Takes the one container the protocol allows, and closes anyone else who dials while a
        // LIVE one is connected rather than leaving them to believe they are being served.  A
        // connection whose peer has exited is not a live one: it is closed here and its dialer's
        // replacement is taken, which is what a compositor that crashed and was restarted is.
        void AdoptLocked(Int fd);

        // True while the connected container is still there.  recvmsg with MSG_PEEK is what asks:
        // zero bytes is the peer's close, EAGAIN is a peer that is between frames, and a release
        // waiting in the socket is a peer that is alive - peeked at rather than read, because the
        // render path is the one that spends releases.
        Bool ClientAliveLocked();

        // Takes the one container the protocol allows out of whoever dialled.
        void AcceptLoop(Int listen);

        // Sends the frame that is the draw target to a container that has just connected.
        void ReofferFrameLocked();

        // Remembers the frame the host last handed over, holding a reference to it so that a
        // container which dials after the offer can still be given it.  The previous one is
        // released here; nothing else in the channel holds a frame.
        void RememberFrameLocked(struct AHardwareBuffer* buffer, const HostFrameOffer& offer, Uint32 seq);
        void ReleaseFrameLocked();

        // The offer itself: the handle relayed out of the buffer, the message built, one sendmsg.
        // A frame that has already been offered once is sent again with the sequence number it was
        // offered under and does not move the counters, because it is the same frame and not a new
        // one.
        Bool SendFrameLocked(struct AHardwareBuffer* buffer, const HostFrameOffer& offer, Uint32 seq, Bool counting);

        // The body of Poll(), for a caller that is already holding the lock.
        void PollLocked();

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
        // The frame the host handed over most recently, held by this channel's own reference for as
        // long as it is the draw target: it is what a container that dials late is sent.
        struct AHardwareBuffer* m_frame = nullptr;
        HostFrameOffer m_frameOffer = {};
        Uint32 m_frameSeq = 0;
        // The acceptor, and the flag that ends it.  The listening descriptor is handed to the thread
        // as its own copy, so it is the only thing in here that thread reads without the lock - and
        // Close() joins the thread before it closes that descriptor.
        std::thread m_acceptor;
        std::atomic<Bool> m_accepting{false};
        // Guards every member above, from both threads.  Mutable because the two const questions a
        // caller asks about the channel - is a container connected, how many frames are outstanding
        // - are answers that have to be read under it.
        mutable std::mutex m_mutex;
    };
}
