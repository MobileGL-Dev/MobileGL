// MobileGL - MobileGL/MG_Backend/GbmFrameChannel.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GbmFrameChannel.h"

#if MOBILEGL_BUILD_DISAGGREGATED && defined(__ANDROID__)

#include <android/hardware_buffer.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// WHAT THIS FILE IS WRITTEN AGAINST, AND WHAT HAS NOT BEEN RUN.
//
// The other end exists and is tested on this machine: MobileGL/MG_Gbm/GbmHostChannel.cpp dials
// this socket with SOCK_SEQPACKET, reads one message per frame, computes that message's expected
// length from the handle's own two counts, and refuses any other length.  Every byte below is
// written to that parser.  There is no phone attached to the machine this was written on, so the
// file is compile-verified for aarch64 Android and nothing more: the first run on a device is what
// will say whether the relay in RelayHandle() reads back the handle's bytes exactly as
// AHardwareBuffer_sendHandleToUnixSocket writes them, which is why that path is written to
// survive both layouts the platform's serializer could have (see the note there).

namespace MobileGL::MG_Backend {
    namespace {
        // THE LAYOUT IS A PROTOCOL, SO ITS SIZES ARE ASSERTED AND NOT ASSUMED.  The container
        // derives the length of a frame message from these three sizes plus the handle's counts,
        // so a field added here, or the packing attribute dropped, would not be a compile error
        // over there - it would be every frame skipped with a message about its length.
        static_assert(sizeof(GbmFrameOffer) == 10 * sizeof(Uint32), "the offer is ten 32 bit fields on the wire");
        static_assert(sizeof(GbmFrameRelease) == 3 * sizeof(Uint32), "the release is three 32 bit fields on the wire");
        static_assert(sizeof(GbmNativeHandleHeader) == 3 * sizeof(Int32),
                      "a native handle begins with a version and two counts, on every Android ABI");

        // The container reads one message per frame into a 4 KiB buffer and skips a message that
        // does not fit in it (GbmHostMessageMax, GbmHostChannel.cpp), so a message larger than this
        // is refused here rather than built and dropped there.
        constexpr SizeT kMessageMax = 4096;
        // The descriptors one handle may carry, which is the container's own cap (GbmHostMaxFds).
        // Every AHardwareBuffer handle this project has seen carries one - the dma-buf - and both
        // ends refuse a frame whose handle names more than they can take rather than forward a
        // truncated list into a buffer of unknown memory.
        constexpr Int kMaxHandleFds = 8;
        // How many release messages one call reads before it returns to the frame.  A cap and not a
        // loop to exhaustion: the messages left behind stay in the socket, whose buffer the kernel
        // bounds, and the next frame drains them.
        constexpr Int kDrainMax = 64;
        // How long the relay read waits for bytes the send one statement earlier has already put in
        // the socket.  It exists only so that an invariant broken somewhere else cannot park the
        // apply thread; on a local socketpair the wait is zero.
        constexpr Int kRelayWaitMs = 2;

        // close() may overwrite the errno a caller is in the middle of reporting, and every caller
        // here is reporting a failure errno describes.
        void CloseQuietly(Int fd) {
            if (fd < 0) return;
            const Int savedErrno = errno;
            ::close(fd);
            errno = savedErrno;
        }
    }

    GbmFrameChannel::~GbmFrameChannel() { Close(); }

    Bool GbmFrameChannel::Open() {
        if (m_listen >= 0) return true;

        const char* configured = ::getenv(kGbmFrameSocketEnv);
        const char* path = (configured != nullptr && configured[0] != 0) ? configured : kGbmFrameSocketPath;
        constexpr SizeT pathMax = sizeof(((struct sockaddr_un*)nullptr)->sun_path);
        if (std::strlen(path) >= pathMax) {
            MGLOG_E_ONCE("gbm frame channel: the container socket path %s does not fit in sun_path (%zu bytes); no "
                         "container can be served",
                         path, pathMax);
            return false;
        }

        // SOCK_SEQPACKET because that is what the container dials with (GbmHostChannel::EnsureOpen
        // asks for exactly this), and a stream listener answers it EPROTOTYPE - the container
        // prints that as "a socket of a different type".  SOCK_NONBLOCK because the accept below
        // runs on the apply thread and may not wait for a container that is not there; SOCK_CLOEXEC
        // because every process this server starts would otherwise inherit a socket that receives
        // the phone's frames.
        const Int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            MGLOG_E_ONCE("gbm frame channel: socket(AF_UNIX, SOCK_SEQPACKET): %s; the frames are drawn and presented "
                         "without a container",
                         std::strerror(errno));
            return false;
        }

        // A node left behind by a server that died is not a reason to refuse a container: bind
        // would answer EADDRINUSE for a socket nobody is listening on.
        ::unlink(path);

        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        // The terminator travels with the name: sun_path is a C string for a pathname socket, and a
        // name without one is a name the kernel reads past the end of.
        std::memcpy(address.sun_path, path, std::strlen(path) + 1);
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
            MGLOG_E_ONCE("gbm frame channel: bind(%s): %s; the frames are drawn and presented without a container",
                         path, std::strerror(errno));
            CloseQuietly(fd);
            return false;
        }
        // connect() to a pathname socket needs write permission on the node, and the container is
        // not this process's user, so the umask's usual 0755 would leave it refused for a reason it
        // could not see.  0666 is what makes the node reachable by the other half of a split run; a
        // chmod that fails is not fatal, because a peer running as this uid still dials it.
        if (::chmod(path, 0666) != 0) {
            MGLOG_W_ONCE("gbm frame channel: chmod(%s, 0666): %s; a container running as another user cannot dial "
                         "this socket",
                         path, std::strerror(errno));
        }
        if (::listen(fd, 1) != 0) {
            MGLOG_E_ONCE("gbm frame channel: listen(%s): %s; the frames are drawn and presented without a container",
                         path, std::strerror(errno));
            ::unlink(path);
            CloseQuietly(fd);
            return false;
        }

        m_listen = fd;
        m_path = path;
        MGLOG_I("gbm frame channel: listening on %s for one container (SOCK_SEQPACKET, one message per frame)", path);
        return true;
    }

    Bool GbmFrameChannel::EnsureRelay() {
        if (m_relay[0] >= 0 && m_relay[1] >= 0) return true;
        // Both ends are dropped before a new pair is made: half a socketpair is a socket with bytes
        // in it that nothing will ever read, and the next handle read from it would be the tail of
        // the last one.
        DropRelay();
        Int pair[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) != 0) {
            MGLOG_E_ONCE("gbm frame channel: socketpair for the handle relay: %s; no frame can be offered until one "
                         "can be made",
                         std::strerror(errno));
            return false;
        }
        m_relay[0] = pair[0];
        m_relay[1] = pair[1];
        return true;
    }

    void GbmFrameChannel::DropRelay() {
        CloseQuietly(m_relay[0]);
        CloseQuietly(m_relay[1]);
        m_relay[0] = -1;
        m_relay[1] = -1;
    }

    Bool GbmFrameChannel::RelayHandle(struct AHardwareBuffer* buffer, Uint8* payload, SizeT capacity, SizeT& payloadBytes,
                                      Int* fds, Int& fdCount) {
        payloadBytes = 0;
        fdCount = 0;
        if (!EnsureRelay()) return false;

        // WHY A FRAME'S HANDLE GOES THROUGH A SOCKETPAIR OF THIS CHANNEL'S OWN.
        //
        // The container takes ONE message per frame - the offer, then the handle's counts, then the
        // arrays those counts describe, with the descriptors in that same message's SCM_RIGHTS
        // control data - and it refuses a message of any other length.  Two sends cannot be one
        // message on a message-oriented socket, so AHardwareBuffer_sendHandleToUnixSocket, which
        // sends a handle as a message of its own, is pointed at a socketpair this channel owns, and
        // what comes out of the other end is both halves of what the container wants: the handle's
        // own bytes, read here, and its descriptors, as SCM_RIGHTS.
        //
        // The alternative does not exist: the NDK's <android/hardware_buffer.h> declares no way to
        // ask a buffer for its descriptors.  AHardwareBuffer_getNativeHandle is a platform API and
        // is absent from the NDK's copy of that header (checked against NDK 27.3.13750724 in this
        // tree's build-termux toolchain), so this is the one route from an AHardwareBuffer to a
        // dma-buf descriptor that an app can take.
        const Int code = AHardwareBuffer_sendHandleToUnixSocket(buffer, m_relay[0]);
        if (code != 0) {
            MGLOG_W_ONCE("gbm frame channel: the frame's handle did not cross the relay socket (rc %d, errno %d); the "
                         "frame is not offered to the container",
                         code, errno);
            // The relay is dropped rather than kept: a send that failed part way through would
            // leave bytes that the next handle's read would take for its own header.
            DropRelay();
            return false;
        }

        struct iovec vector = {};
        vector.iov_base = payload;
        vector.iov_len = capacity;
        Uint8 control[CMSG_SPACE(sizeof(Int) * (SizeT)kMaxHandleFds)] = {};
        struct msghdr message = {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);

        // MSG_DONTWAIT for the promise at the top of this file, and MSG_CMSG_CLOEXEC so a descriptor
        // read out of the relay does not survive into a child of this process, which would then hold
        // the phone's frame until it died.
        ssize_t got = ::recvmsg(m_relay[1], &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd waiting = {};
            waiting.fd = m_relay[1];
            waiting.events = POLLIN;
            if (::poll(&waiting, 1, kRelayWaitMs) > 0) {
                // The kernel rewrites these on every call, so a retry starts from the same state as
                // the first attempt.
                message.msg_controllen = sizeof(control);
                message.msg_flags = 0;
                got = ::recvmsg(m_relay[1], &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            }
        }
        if (got < 0) {
            MGLOG_W_ONCE("gbm frame channel: a relayed handle could not be read back (%d: %s); the frame is not "
                         "offered to the container",
                         errno, std::strerror(errno));
            DropRelay();
            return false;
        }
        if (got == 0) {
            // Zero bytes on a connected socket is the end of it.  Only this process holds either end
            // of this socketpair, so this is a bug being reported rather than a peer going away.
            MGLOG_W_ONCE("gbm frame channel: the handle relay is closed; the frame is not offered to the container");
            DropRelay();
            return false;
        }

        // The descriptors come out of the control data before anything is decided about the
        // message, because they belong to this process from the moment they were received: a path
        // that returns without closing them leaks a descriptor per frame.
        Int collected = 0;
        for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
            const SizeT bytes = (SizeT)header->cmsg_len - CMSG_LEN(0);
            const Int count = (Int)(bytes / sizeof(Int));
            const Int* descriptors = reinterpret_cast<const Int*>(CMSG_DATA(header));
            for (Int i = 0; i < count; ++i) {
                if (collected < kMaxHandleFds) {
                    fds[collected++] = descriptors[i];
                } else {
                    // More descriptors than a frame can carry: they are this process's now, so they
                    // are closed here rather than leaked.
                    CloseQuietly(descriptors[i]);
                }
            }
        }

        if ((message.msg_flags & MSG_CTRUNC) != 0) {
            MGLOG_W_ONCE("gbm frame channel: a frame's control data did not fit in %zu bytes; the frame is not "
                         "offered because its descriptor list is incomplete",
                         sizeof(control));
            for (Int i = 0; i < collected; ++i) CloseQuietly(fds[i]);
            DropRelay();
            return false;
        }
        if ((message.msg_flags & MSG_TRUNC) != 0 || (SizeT)got < sizeof(GbmNativeHandleHeader)) {
            // A message longer than a handle can be, or shorter than the three counts every handle
            // begins with.  Either way it cannot describe a frame, and a relay that has been read
            // past the end of one message must not carry the next frame's header: it is recreated.
            MGLOG_W_ONCE("gbm frame channel: a relayed handle is %zd bytes and %s; the frame is not offered", got,
                         ((message.msg_flags & MSG_TRUNC) != 0) ? "longer than a handle can be"
                                                                : "too short to hold its own counts");
            for (Int i = 0; i < collected; ++i) CloseQuietly(fds[i]);
            DropRelay();
            return false;
        }

        payloadBytes = (SizeT)got;
        fdCount = collected;
        return true;
    }

    void GbmFrameChannel::AcceptDialer() {
        if (m_listen < 0) return;
        // accept4, so the connection is close-on-exec and non-blocking from the moment it exists: a
        // container that stops reading must not be able to park a send on the apply thread.
        const Int fd = ::accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            // EAGAIN is nobody dialling, which is the ordinary state of a phone whose container is
            // not running.  The rest are worth one line between them: the listener outlives them and
            // the next frame tries again.
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
                MGLOG_W_ONCE("gbm frame channel: accept on %s failed (%d: %s); a container cannot connect until the "
                             "next frame tries again",
                             m_path.c_str(), errno, std::strerror(errno));
            }
            return;
        }
        if (m_client < 0) {
            m_client = fd;
            MGLOG_I("gbm frame channel: a container is connected on %s; every frame the host offers is sent to it as "
                    "the dma-buf descriptor behind it",
                    m_path.c_str());
            return;
        }
        // ONE CLIENT, which is what the protocol is written for: a second dialer is closed rather
        // than left connected, so that it learns it is not the one being served instead of holding a
        // connection whose messages nobody reads.
        CloseQuietly(fd);
        MGLOG_W_ONCE("gbm frame channel: a second peer dialled %s while a container is connected; it is closed and "
                     "the frames stay with the connected one",
                     m_path.c_str());
    }

    void GbmFrameChannel::MarkDown(const char* what, Int error) {
        if (m_client < 0) return;
        CloseQuietly(m_client);
        m_client = -1;
        if (error != 0) {
            MGLOG_W("gbm frame channel: the container connection is down: %s failed (%d: %s); the frames are still "
                    "drawn and presented, and the container stops receiving them until it dials again",
                    what, error, std::strerror(error));
        } else {
            MGLOG_W("gbm frame channel: the container connection is down: %s; the frames are still drawn and "
                    "presented, and the container stops receiving them until it dials again",
                    what);
        }
    }

    Bool GbmFrameChannel::OfferFrame(struct AHardwareBuffer* buffer, const HostFrameOffer& offer) {
        if (buffer == nullptr) return false;
        // Open() does nothing once the socket exists, so this is an integer test in the common case.
        Open();

        // Releases first, and the accept after them: a container that has died is noticed and its
        // replacement taken before this frame is handed to a socket the kernel has already given up
        // on, which is what lets a compositor that restarted receive frames from the very next one
        // rather than from the frame after it.
        Poll();
        AcceptDialer();
        if (m_client < 0) return false;

        Uint8 payload[kMessageMax];
        SizeT payloadBytes = 0;
        Int fds[kMaxHandleFds];
        for (Int i = 0; i < kMaxHandleFds; ++i) fds[i] = -1;
        Int fdCount = 0;
        if (!RelayHandle(buffer, payload, sizeof(payload), payloadBytes, fds, fdCount)) return false;

        GbmNativeHandleHeader handle = {};
        std::memcpy(&handle, payload, sizeof(handle));
        const SizeT tail = ((SizeT)handle.NumFds + (SizeT)handle.NumInts) * sizeof(Int32);
        const SizeT messageBytes = sizeof(GbmFrameOffer) + sizeof(GbmNativeHandleHeader) + tail;

        // EVERY REASON THIS CHANNEL REFUSES A FRAME, IN ONE PLACE.  Each of them is a frame the
        // container would refuse anyway - it checks the same counts and the same geometry - and a
        // frame that is refused here costs nothing: no message, no sequence number, and no entry in
        // the count of frames waiting to be released.
        const char* refusal = nullptr;
        if (offer.Width == 0 || offer.Height == 0 || offer.StrideBytes == 0) {
            refusal = "the host described it with a width, height or stride of zero, and a buffer with no size is "
                      "not one a compositor can render into";
        } else if (handle.NumFds < 1) {
            refusal = "its handle carries no descriptor, and a frame without a dma-buf is not one the container "
                      "can render into";
        } else if (handle.NumFds > kMaxHandleFds) {
            refusal = "its handle carries more descriptors than either end takes";
        } else if (handle.NumInts < 0) {
            refusal = "its handle claims a negative number of integers";
        } else if ((SizeT)handle.NumInts > kMessageMax / sizeof(Int32)) {
            // This is what keeps the message's length arithmetic from wrapping on a 32 bit ABI: a
            // handle whose integer array alone is longer than a whole message cannot be carried by
            // this protocol whatever the rest of it says, and the check below would otherwise be
            // reading a number that had already turned into a small and plausible one.
            refusal = "its handle names more integers than a whole message could carry";
        } else if (fdCount < handle.NumFds) {
            refusal = "fewer descriptors arrived with it than its handle names";
        } else if (messageBytes > kMessageMax) {
            refusal = "the message it makes is longer than the container reads";
        }

        Bool offered = false;
        if (refusal != nullptr) {
            // ONCE, not per frame: a host that describes every frame the same broken way must not
            // turn this into a line per frame on the apply thread.
            MGLOG_W_ONCE("gbm frame channel: frame %u (%ux%u) is not offered to the container: %s", offer.Index,
                         offer.Width, offer.Height, refusal);
        } else {
            // One message, built whole: the offer, then the handle's own three counts, then the two
            // arrays the counts describe.  What came back from the relay is copied in as far as it
            // goes and the remainder is left zero, because the container checks the LENGTH it
            // derives from those counts and never reads the arrays themselves - it takes the
            // descriptors from the control data and hands the first one to GBM.  So a handle whose
            // bytes are shorter than its own counts imply is padded rather than refused, and one
            // longer than they imply is cut to what the counts say; either way the length the
            // container computes is the length it receives.
            Uint8 message[kMessageMax] = {};
            GbmFrameOffer out = {};
            out.Magic = kGbmFrameOfferMagic;
            out.Version = kGbmFrameVersion;
            out.Seq = m_nextSeq;
            out.Index = offer.Index;
            out.Width = offer.Width;
            out.Height = offer.Height;
            out.StrideBytes = offer.StrideBytes;
            out.Format = offer.Format;
            out.UsageLo = offer.UsageLo;
            out.UsageHi = offer.UsageHi;
            std::memcpy(message, &out, sizeof(out));
            const SizeT handleBytes = (payloadBytes < sizeof(GbmNativeHandleHeader) + tail)
                                          ? payloadBytes
                                          : (sizeof(GbmNativeHandleHeader) + tail);
            std::memcpy(message + sizeof(out), payload, handleBytes);

            struct iovec vector = {};
            vector.iov_base = message;
            vector.iov_len = messageBytes;
            Uint8 control[CMSG_SPACE(sizeof(Int) * (SizeT)kMaxHandleFds)] = {};
            struct msghdr outgoing = {};
            outgoing.msg_iov = &vector;
            outgoing.msg_iovlen = 1;
            outgoing.msg_control = control;
            outgoing.msg_controllen = CMSG_SPACE(sizeof(Int) * (SizeT)handle.NumFds);
            struct cmsghdr* rights = CMSG_FIRSTHDR(&outgoing);
            rights->cmsg_level = SOL_SOCKET;
            rights->cmsg_type = SCM_RIGHTS;
            rights->cmsg_len = CMSG_LEN(sizeof(Int) * (SizeT)handle.NumFds);
            std::memcpy(CMSG_DATA(rights), fds, sizeof(Int) * (SizeT)handle.NumFds);

            // MSG_DONTWAIT for the promise at the top of this file, and MSG_NOSIGNAL because a
            // server killed by SIGPIPE when the container goes away takes the frames it was drawing
            // with it.  A full socket, a reset peer and a broken pipe all say the same thing here:
            // this connection can no longer be trusted with a frame, so it is closed and the next
            // container to dial is served.
            const ssize_t sent = ::sendmsg(m_client, &outgoing, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent != (ssize_t)messageBytes) {
                MarkDown("sendmsg", (sent < 0) ? errno : 0);
            } else {
                m_nextSeq = out.Seq + 1;
                ++m_offered;
                ++m_outstanding;
                offered = true;
                // The first offer is the one worth a line, and it carries the host's own numbers:
                // the format in particular is the field the two ends could disagree about without
                // either failing, so the value the host actually sends is put where a run on a
                // device can be read afterwards.
                MGLOG_I_ONCE("gbm frame channel: the first frame is offered to the container (seq %u index %u %ux%u "
                             "stride %u format 0x%08x)",
                             out.Seq, out.Index, out.Width, out.Height, out.StrideBytes, out.Format);
                MGLOG_D("gbm frame channel: frame seq=%u index=%u offered (%zu bytes, %d descriptor(s), %llu "
                        "outstanding)",
                        out.Seq, out.Index, messageBytes, handle.NumFds, (unsigned long long)m_outstanding);
            }
        }

        // The kernel took its own copies of these when the message was sent, and one that was not
        // sent is not the container's to hold: every path closes what the relay handed over.
        for (Int i = 0; i < fdCount; ++i) CloseQuietly(fds[i]);
        return offered;
    }

    void GbmFrameChannel::Poll() {
        if (m_client < 0) return;
        for (Int drained = 0; drained < kDrainMax; ++drained) {
            GbmFrameRelease release = {};
            struct iovec vector = {};
            vector.iov_base = &release;
            vector.iov_len = sizeof(release);
            struct msghdr message = {};
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            // recvmsg and not recv, for MSG_TRUNC: a message longer than a release can only be a
            // peer speaking something else, and a release read out of the first twelve bytes of it
            // would name a frame nobody released.
            const ssize_t got = ::recvmsg(m_client, &message, MSG_DONTWAIT);
            if (got < 0) {
                if (errno == EINTR) continue;
                // Nothing said, which is what a container that is between frames looks like.
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                MarkDown("recvmsg", errno);
                return;
            }
            if (got == 0) {
                // Zero bytes on a connected socket is the end of it, whether the container exited or
                // the kernel gave up on it.  Its replacement is taken by the next frame.
                MarkDown("the container closed the connection", 0);
                return;
            }
            if ((message.msg_flags & MSG_TRUNC) != 0 || got != (ssize_t)sizeof(release)) {
                // The message is skipped, not the connection: on a message-oriented socket a
                // message too long for the buffer is consumed whole, so the next one is still a
                // message rather than the tail of this one.
                MGLOG_W_ONCE("gbm frame channel: the container sent a message that is not a %zu byte release; it is "
                             "skipped",
                             sizeof(release));
                continue;
            }
            if (release.Magic != kGbmFrameReleaseMagic) {
                MGLOG_W_ONCE("gbm frame channel: a message from the container begins with 0x%08x rather than 'MGGR'; "
                             "it is skipped",
                             release.Magic);
                continue;
            }
            if (release.Version != kGbmFrameVersion) {
                // Refused rather than acted on: the version is what says how the rest of the message
                // is laid out, and a release read with the wrong layout names a frame the container
                // did not mean.
                MGLOG_E_ONCE("gbm frame channel: the container speaks protocol version %u where this server speaks "
                             "version %u; its releases are refused until the two agree",
                             release.Version, (Uint32)kGbmFrameVersion);
                continue;
            }
            ++m_released;
            if (m_outstanding > 0) --m_outstanding;
            // The first release is the only one worth a line: it is the proof that the container
            // took the descriptor and its compositor retired the frame, where every later one says
            // the same thing sixty times a second.
            MGLOG_I_ONCE("gbm frame channel: the container is releasing frames (first: seq %u)", release.Seq);
        }
        // Anything past this cap stays in the socket, whose buffer the kernel bounds, and goes out
        // with the next frame: a peer that says nothing but releases may not spend the apply thread
        // here.
    }

    void GbmFrameChannel::Close() {
        // The connection is closed quietly rather than marked down: this is a teardown, not a
        // failure, and a warning about it would be noise in a log read for the failures.
        CloseQuietly(m_client);
        m_client = -1;
        DropRelay();
        if (m_listen >= 0) {
            // The node is this channel's to remove only while it still IS this channel's socket: a
            // server that was restarted has bound the same path with a socket of its own, and a
            // teardown that unlinked it would take the successor's node away with it.  st_dev and
            // st_ino against the listening descriptor is the cheapest way to ask, and a check that
            // fails only leaves a name behind that the next Open() unlinks anyway.
            struct stat node = {};
            struct stat socket = {};
            if (!m_path.empty() && ::stat(m_path.c_str(), &node) == 0 && ::fstat(m_listen, &socket) == 0 &&
                node.st_dev == socket.st_dev && node.st_ino == socket.st_ino) {
                ::unlink(m_path.c_str());
            }
            CloseQuietly(m_listen);
            m_listen = -1;
        }
        m_path.clear();
        // The counters are history and stay: a caller that logs Outstanding() after a teardown is
        // asking what happened, and IsUp() is what answers what is connected now.
    }
}

#else  // !(MOBILEGL_BUILD_DISAGGREGATED && __ANDROID__)

namespace MobileGL::MG_Backend {
    // A build that is not the Android render server has no AHardwareBuffer to offer and no
    // container beside it.  The class exists so that callers do not have to be compiled
    // differently, and every entry point answers the way an absent container does.
    GbmFrameChannel::~GbmFrameChannel() = default;
    Bool GbmFrameChannel::Open() { return false; }
    Bool GbmFrameChannel::OfferFrame(struct AHardwareBuffer*, const HostFrameOffer&) { return false; }
    void GbmFrameChannel::Poll() {}
    void GbmFrameChannel::Close() {}
}

#endif

// End of Source File Header
