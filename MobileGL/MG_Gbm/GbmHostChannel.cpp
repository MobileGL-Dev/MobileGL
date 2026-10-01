// MobileGL - MobileGL/MG_Gbm/GbmHostChannel.cpp
// SPDX-License-Identifier: LGPL-3.0-only
//
// The host-frame backend: the socket the container receives the phone's frames on.
//
// The container half of MobileGL runs a Wayland compositor, and the frames that
// compositor renders into have to be the frames the phone already owns - the phone
// allocates them, its GPU draws into them, and its display scans them out. A buffer
// allocated from the container's own dma-heap would be a copy that has to travel back
// to the phone before anything can be shown, so when the display host is reachable this
// backend hands the compositor the phone's own buffers instead, and the fd the
// compositor passes on is the fd of a frame the display path already knows.
//
// What that costs is the one thing GBM normally promises: the geometry. A frame's
// width, height, stride and format are the display's and not the caller's, so a
// gbm_bo_create that takes an offered frame answers with the offer's geometry, and the
// caller reads it back with gbm_bo_get_width and its neighbours. That is this backend's
// documented behaviour rather than a surprise: the host decides what a frame is.
//
// Three rules keep this backend from ever being worse than not having it.
//
//   * Nothing on the allocation path blocks. The socket is non-blocking and connect()
//     to a pathname socket either succeeds or fails immediately, so a call that cannot
//     be served from the host falls through to a local allocation inside the same call
//     instead of waiting for a phone that may never answer. The one wait in this file is
//     in device creation, after a display host has already accepted the connection, and
//     it exists because of a measured race: the compositor builds its swapchain within
//     microseconds of creating the device, while the offer for the first frame needs the
//     round trip of the connection plus whatever the phone does before it sends. Without
//     that wait the swapchain - the buffers that are shown for the life of the process -
//     is allocated locally and the host's frames arrive for nobody to take.
//   * A channel that dies is closed and marked down, and the next frame is allocated
//     locally again. This library is loaded by a compositor that has to keep painting,
//     so losing the display host may cost the frames their path to the screen but it
//     may not cost the session.
//   * A frame is released exactly once, when the record holding it is destroyed, and
//     only into the connection that offered it. Releasing on gbm_bo_destroy would be a
//     lie - the surface may still be scanning that frame out - and releasing an old
//     frame into a new connection would free a slot belonging to another session.
//
// The device mutex is the only lock in here. Both callers of this file - the allocation
// path and the buffer record's destructor - already run under it, so there is no second
// lock to order against and no state of this file's own that another thread can see.

#include "GbmInternal.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

namespace MobileGL::MG_Gbm
{
    namespace
    {
        // One frame per message, and a message is the offer followed by the handle the
        // offer describes. 4 KiB is far more than an AHardwareBuffer handle needs (a
        // handle is a version, two counts and their two arrays) while still being small
        // enough that a message which does not fit is a peer speaking something else -
        // which is worth refusing rather than growing a buffer for.
        constexpr SizeT GbmHostMessageMax = 4096;

        // The descriptors one handle may carry. An AHardwareBuffer handle carries one
        // (the dma-buf) in every layout this library has seen and the protocol reads only
        // the first, so this sizes the control buffer rather than claiming anything about
        // real handles.
        constexpr Int GbmHostMaxFds = 8;

        // How many messages one gbm_bo_create may look at before it gives up and
        // allocates locally. An unusable message is skipped rather than fatal, so this
        // bounds the work a peer that sends nothing but unusable messages can cause
        // inside a single allocation.
        constexpr Int GbmHostTakeAttempts = 16;

        // The floor between two connection attempts. A container with no display host is
        // the normal case, so a retry may not turn every allocation into a connect plus a
        // log line; one attempt per second is often enough that a phone which comes up is
        // picked up within a frame or two of the compositor's next allocation.
        constexpr std::chrono::milliseconds GbmHostRetryInterval{1000};

        // How long device creation waits for the display host's first frame once the
        // connection is up. A frame the phone offers immediately needs one round trip on
        // a unix socket, so this is orders of magnitude more than that and it is only
        // ever spent by a device that has a host which then says nothing; a compile or a
        // test run without a display host never reaches it, because the connect fails
        // first.
        constexpr Int GbmHostFirstOfferWaitMs = 250;

        // Frame releases the socket would not take. A release is twelve bytes, so a full
        // socket buffer is hundreds of frames' worth of them and this queue is only
        // reached once the phone has stopped reading; the cap is what stops a peer that
        // never reads from growing it without bound.
        constexpr SizeT GbmHostMaxPendingReleases = 256;

        // sun_path is where a pathname socket's name lives, and a name that does not fit
        // is one the kernel cannot be told about at all.
        constexpr SizeT GbmHostPathMax = sizeof(((struct sockaddr_un*)nullptr)->sun_path);

        // close() may overwrite errno, and every caller here is in the middle of
        // reporting a failure that errno describes. Saving it around the close keeps the
        // reported failure the one that happened.
        void GbmHostCloseQuietly(Int fd) noexcept
        {
            if (fd < 0)
            {
                return;
            }
            const Int savedErrno = errno;
            close(fd);
            errno = savedErrno;
        }
    } // namespace

    // One connection to the display host, for the life of one device.
    class GbmHostChannel
    {
    public:
        explicit GbmHostChannel(const String& path) noexcept : Path(path) {}
        ~GbmHostChannel() noexcept;

        GbmHostChannel(const GbmHostChannel&) = delete;
        GbmHostChannel& operator=(const GbmHostChannel&) = delete;

        // Connects when there is no connection and the retry interval has passed. A
        // failure is never the caller's problem: the channel stays closed and the
        // allocation that asked for a frame falls back to local memory.
        void EnsureOpen(::gbm_device& device) noexcept;

        // Waits, once, for the connection's first frame to be readable. Called from
        // device creation and nowhere else: see the banner for why that one wait exists.
        void AwaitFirstOffer(::gbm_device& device) noexcept;

        // The next frame the host offered, or false when none is waiting.
        Bool TakeFrame(::gbm_device& device, GbmHostFrame& frameOut) noexcept;

        // Queues the release for one frame and tries to send what is queued.
        void ReleaseFrame(::gbm_device& device, Uint32 seq, Uint64 generation) noexcept;

    private:
        // Turns one message into a frame. Everything it refuses is reported with the
        // reason, because a peer this library cannot understand is the one thing that
        // would otherwise be invisible from the outside.
        Bool ParseOffer(const Uint8* payload, SizeT length, const Int* fds, Int fdCount,
                        GbmHostFrame& frameOut) noexcept;

        // Sends what is queued. False means the connection can no longer be trusted to
        // carry a twelve byte message, with errno describing what happened.
        Bool FlushReleases() noexcept;

        // Closes the socket and drops whatever was queued for it.
        void CloseSocket() noexcept;

        // Closes the socket and tells the device what that means for its backend name.
        void MarkDown(::gbm_device& device, const char* what, Int error) noexcept;

        String Path;
        Int SocketFd = -1;
        // Set by a connection this process has just made, and read once by the allocation that
        // follows it: the accept and the frame that answers the dial happen on the other side of
        // this socket, so an allocation that reads immediately after connecting reads before the
        // answer to its own connection has been written.  See TakeFrame.
        Bool FreshConnection = false;
        // Bumped by every connection, so a frame taken from a connection which has since
        // died can be recognised as one that must not be released into its replacement.
        Uint64 Generation = 0;
        std::chrono::steady_clock::time_point NextRetry{};
        // The errno of the last failed attempt, so that a failure which repeats once a
        // second is not logged once a second.
        Int LastFailureErrno = 0;
        std::vector<GbmFrameRelease> PendingReleases;
        Bool ReleaseOverflowLogged = false;
    };

    // ---- the offered-frame record ------------------------------------------

    GbmHostFrame::~GbmHostFrame()
    {
        // The record owns the dup it was built on, which is why it is move-only: a copy
        // would close the same descriptor twice.
        if (Fd >= 0)
        {
            close(Fd);
            Fd = -1;
        }
    }

    GbmHostFrame::GbmHostFrame(GbmHostFrame&& other) noexcept
    {
        *this = static_cast<GbmHostFrame&&>(other);
    }

    GbmHostFrame& GbmHostFrame::operator=(GbmHostFrame&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (Fd >= 0)
        {
            close(Fd);
        }
        Seq = other.Seq;
        Index = other.Index;
        Width = other.Width;
        Height = other.Height;
        StrideBytes = other.StrideBytes;
        Format = other.Format;
        BytesPerPixel = other.BytesPerPixel;
        Usage = other.Usage;
        Generation = other.Generation;
        Fd = other.Fd;
        Size = other.Size;
        // The source must not close what this record now owns.
        other.Fd = -1;
        return *this;
    }

    // ---- the connection ----------------------------------------------------

    GbmHostChannel::~GbmHostChannel() noexcept
    {
        // One last try for whatever is queued - the phone may still be reading - and
        // then the socket goes away with the process.
        if (SocketFd >= 0 && !PendingReleases.empty())
        {
            FlushReleases();
        }
        if (!PendingReleases.empty())
        {
            MGLOG_W("gbm: %zu frame release(s) could not be sent to the display host at %s; the phone reclaims "
                    "those slots when the session ends",
                    PendingReleases.size(), Path.c_str());
        }
        CloseSocket();
    }

    void GbmHostChannel::EnsureOpen(::gbm_device& device) noexcept
    {
        if (SocketFd >= 0)
        {
            return;
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now < NextRetry)
        {
            return;
        }
        NextRetry = now + GbmHostRetryInterval;

        // SOCK_SEQPACKET because a frame and its descriptors are one message and the
        // reader may not reassemble either half; SOCK_NONBLOCK because the caller is a
        // compositor's frame loop and may not be parked on a phone; SOCK_CLOEXEC because
        // every process this compositor starts would otherwise inherit a socket it can
        // read frames from.
        const Int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            MGLOG_E_ONCE("gbm: cannot create the socket to the display host (%d: %s); every frame is allocated "
                         "locally",
                         errno, strerror(errno));
            return;
        }

        if (Path.empty() || Path.size() >= GbmHostPathMax)
        {
            MGLOG_E_ONCE("gbm: the display host socket path \"%s\" does not fit in sun_path (%zu bytes); every "
                         "frame is allocated locally",
                         Path.c_str(), GbmHostPathMax);
            GbmHostCloseQuietly(fd);
            return;
        }

        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        // The terminator travels with the name: sun_path is a C string for a pathname
        // socket, and a name without one is a name the kernel reads past the end of.
        memcpy(address.sun_path, Path.c_str(), Path.size() + 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0)
        {
            const Int connectErrno = errno;
            GbmHostCloseQuietly(fd);
            // A failure that repeats once a second is not news: a container with no
            // display host fails here on every retry, and a line per second saying so
            // would bury the rest of the log. A failure that changed is news, because it
            // says the path or the peer changed.
            if (connectErrno != LastFailureErrno)
            {
                LastFailureErrno = connectErrno;
                if (connectErrno == EPROTOTYPE)
                {
                    MGLOG_E("gbm: %s is a socket of a different type (%d: %s). The frame protocol is "
                            "SOCK_SEQPACKET - one message per frame, carrying that frame's descriptors - "
                            "because a reader cannot tell from a byte stream which frame a descriptor "
                            "belongs to; frames are allocated locally instead",
                            Path.c_str(), connectErrno, strerror(connectErrno));
                }
                else
                {
                    MGLOG_I("gbm: no display host at %s (%d: %s); every frame is allocated locally until one "
                            "answers",
                            Path.c_str(), connectErrno, strerror(connectErrno));
                }
            }
            return;
        }

        SocketFd = fd;
        FreshConnection = true;
        // A new connection is a new session with its own frames, which is what makes the
        // generation a correct answer to "may this release be sent here".
        Generation++;
        LastFailureErrno = 0;
        device.Impl.HostAvailable = true;
        device.Impl.BackendName = "mobilegl-host-frame";
        MGLOG_I("gbm: connected to the display host at %s (one SOCK_SEQPACKET message per frame, up to %zu "
                "bytes); frames offered over it become gbm_bos whose geometry is the offer's",
                Path.c_str(), GbmHostMessageMax);
    }

    void GbmHostChannel::AwaitFirstOffer(::gbm_device& device) noexcept
    {
        if (SocketFd < 0)
        {
            return;
        }
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(GbmHostFirstOfferWaitMs);
        for (;;)
        {
            const std::chrono::milliseconds left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
            {
                break;
            }
            // poll() and not recvmsg(): the frame has to stay in the socket for the
            // allocation that is about to ask for it, and a peek at a message carrying
            // SCM_RIGHTS is a question with no safe answer.
            struct pollfd waiting = {};
            waiting.fd = SocketFd;
            waiting.events = POLLIN;
            const Int ready = poll(&waiting, 1, (Int)left.count());
            if (ready > 0)
            {
                MGLOG_I("gbm: the display host at %s has a frame waiting; the buffers the compositor is about "
                        "to allocate come from it",
                        Path.c_str());
                return;
            }
            if (ready == 0)
            {
                break;
            }
            if (errno != EINTR)
            {
                MarkDown(device, "poll", errno);
                return;
            }
        }
        MGLOG_W("gbm: the display host at %s accepted the connection but offered no frame within %d ms; the "
                "buffers allocated before its first frame are local ones",
                Path.c_str(), GbmHostFirstOfferWaitMs);
    }

    Bool GbmHostChannel::TakeFrame(::gbm_device& device, GbmHostFrame& frameOut) noexcept
    {
        EnsureOpen(device);
        if (SocketFd < 0)
        {
            return false;
        }
        // THE FIRST ALLOCATION AFTER A DIAL WAITS FOR THE ANSWER TO IT. The accept and the offer that
        // answers the connection this call has just made are written by the phone, a round trip
        // behind the connect() that returns here — and what is allocated before that offer arrives
        // is a local buffer, which is what a compositor builds its whole swapchain out of and then
        // keeps for the life of the process. So a connection made on this path is given the same
        // first-offer window device creation gives one, once, and only while it is new.
        if (FreshConnection)
        {
            FreshConnection = false;
            AwaitFirstOffer(device);
        }
        // Releases the socket would not take last time are retried here, because this is
        // the one call in the library that happens once per frame and therefore the
        // natural place for a queue waiting for room to get its next chance.
        if (!PendingReleases.empty() && !FlushReleases())
        {
            MarkDown(device, "sendmsg", errno);
            return false;
        }

        Uint8 payload[GbmHostMessageMax];
        for (Int attempt = 0; attempt < GbmHostTakeAttempts; ++attempt)
        {
            Int receivedFds[GbmHostMaxFds];
            for (Int i = 0; i < GbmHostMaxFds; ++i)
            {
                receivedFds[i] = -1;
            }

            struct iovec payloadVector = {};
            payloadVector.iov_base = payload;
            payloadVector.iov_len = sizeof(payload);

            Uint8 control[CMSG_SPACE(sizeof(Int) * GbmHostMaxFds)] = {};

            struct msghdr message = {};
            message.msg_iov = &payloadVector;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof(control);

            // MSG_DONTWAIT for the promise at the top of this file, and MSG_CMSG_CLOEXEC
            // so a descriptor the phone sent does not survive into a child of this
            // compositor, which would then hold a frame nothing will ever release.
            const ssize_t received = recvmsg(SocketFd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (received < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Nothing offered yet, which is what an idle display looks like. The
                    // caller allocates locally and asks again on its next frame.
                    return false;
                }
                if (errno == EINTR)
                {
                    continue;
                }
                MarkDown(device, "recvmsg", errno);
                return false;
            }

            // The descriptors come out of the control data before anything is decided
            // about the message, because they belong to this process from the moment it
            // was received: a path that returns without closing them leaks a descriptor
            // per unusable message.
            Int fdCount = 0;
            for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
                 header = CMSG_NXTHDR(&message, header))
            {
                if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
                {
                    continue;
                }
                const SizeT bytes = (SizeT)header->cmsg_len - CMSG_LEN(0);
                const Int count = (Int)(bytes / sizeof(Int));
                const Int* descriptors = reinterpret_cast<const Int*>(CMSG_DATA(header));
                for (Int i = 0; i < count; ++i)
                {
                    if (fdCount < GbmHostMaxFds)
                    {
                        receivedFds[fdCount++] = descriptors[i];
                    }
                    else
                    {
                        // More descriptors than a frame can carry: they are this
                        // process's now, so they are closed here rather than leaked.
                        GbmHostCloseQuietly(descriptors[i]);
                    }
                }
            }

            if ((message.msg_flags & MSG_CTRUNC) != 0)
            {
                MGLOG_W_ONCE("gbm: an offered frame's control data did not fit in %zu bytes; the frame is "
                             "skipped because its descriptor list is incomplete",
                             sizeof(control));
                for (Int i = 0; i < fdCount; ++i)
                {
                    GbmHostCloseQuietly(receivedFds[i]);
                }
                continue;
            }

            if (received == 0)
            {
                // Zero bytes on a connected socket is the end of it, whether the phone
                // closed cleanly or the kernel gave up on it. Either way the frames have
                // to come from somewhere else from here on.
                for (Int i = 0; i < fdCount; ++i)
                {
                    GbmHostCloseQuietly(receivedFds[i]);
                }
                MarkDown(device, "the display host closed the connection", 0);
                return false;
            }

            if ((message.msg_flags & MSG_TRUNC) != 0)
            {
                // Longer than a frame message can be, so it is not one - and on a
                // message-oriented socket the rest is already gone while the next message
                // is still a whole message, which is why this skips one message instead
                // of ending the channel.
                MGLOG_W_ONCE("gbm: the display host sent a message longer than %zu bytes; it is skipped",
                             GbmHostMessageMax);
                for (Int i = 0; i < fdCount; ++i)
                {
                    GbmHostCloseQuietly(receivedFds[i]);
                }
                continue;
            }

            const Bool parsed = ParseOffer(payload, (SizeT)received, receivedFds, fdCount, frameOut);
            // Whatever the frame did not keep is closed here: the copy of the dma-buf
            // descriptor the phone sent (the frame owns its own dup) and the rest of the
            // handle, which is a layout this library does not interpret.
            for (Int i = 0; i < fdCount; ++i)
            {
                GbmHostCloseQuietly(receivedFds[i]);
            }
            if (parsed)
            {
                return true;
            }
        }
        return false;
    }

    Bool GbmHostChannel::ParseOffer(const Uint8* payload, SizeT length, const Int* fds, Int fdCount,
                                    GbmHostFrame& frameOut) noexcept
    {
        constexpr SizeT HeaderBytes = sizeof(GbmFrameOffer) + sizeof(AHBNativeHandleHeader);
        if (length < HeaderBytes)
        {
            MGLOG_W_ONCE("gbm: the display host sent a %zu byte message where a frame message is at least %zu "
                         "bytes (the offer followed by the handle header); it is skipped",
                         length, HeaderBytes);
            return false;
        }

        GbmFrameOffer offer = {};
        memcpy(&offer, payload, sizeof(offer));
        if (offer.Magic != GbmFrameOfferMagic)
        {
            MGLOG_W_ONCE("gbm: a message from the display host begins with 0x%08x rather than 'MGGF'; it is "
                         "skipped",
                         offer.Magic);
            return false;
        }
        if (offer.Version != GbmHostProtocolVersion)
        {
            // Refused rather than read as if the versions agreed: the version is what
            // says how the rest of the message is laid out, and a frame read with the
            // wrong layout is memory the compositor would render into.
            MGLOG_E_ONCE("gbm: the display host offers frames in protocol version %u where this library speaks "
                         "version %u; its frames are refused until the two agree",
                         offer.Version, (Uint32)GbmHostProtocolVersion);
            return false;
        }

        AHBNativeHandleHeader handle = {};
        memcpy(&handle, payload + sizeof(offer), sizeof(handle));
        if (handle.NumFds < 1 || handle.NumInts < 0)
        {
            MGLOG_W_ONCE("gbm: an offered frame's handle carries %d descriptor(s) and %d integer(s), where the "
                         "dma-buf is the first descriptor and there has to be one; the frame is skipped",
                         handle.NumFds, handle.NumInts);
            return false;
        }

        // The handle describes itself with its two counts, so the message has to be
        // exactly as long as those counts make it. Any other length means the two ends
        // disagree about the layout, and a frame built from a message whose layout was
        // guessed is a buffer of unknown memory.
        const SizeT expected = HeaderBytes + ((SizeT)handle.NumFds + (SizeT)handle.NumInts) * sizeof(Int32);
        if (length != expected)
        {
            MGLOG_W_ONCE("gbm: an offered frame's message is %zu bytes where its handle (%d descriptors, %d "
                         "integers) makes it %zu; the frame is skipped",
                         length, handle.NumFds, handle.NumInts, expected);
            return false;
        }
        if (fdCount < handle.NumFds)
        {
            MGLOG_W_ONCE("gbm: an offered frame's handle names %d descriptor(s) and %d arrived with the "
                         "message; the frame is skipped",
                         handle.NumFds, fdCount);
            return false;
        }
        if (offer.Width == 0 || offer.Height == 0 || offer.StrideBytes == 0)
        {
            MGLOG_W_ONCE("gbm: an offered frame is %ux%u with a stride of %u bytes; a buffer with no size is "
                         "not one this library can hand out",
                         offer.Width, offer.Height, offer.StrideBytes);
            return false;
        }

        // The frame is built on a dup of the first descriptor rather than on the
        // descriptor itself: the copy that arrived belongs to the message, and the frame
        // outlives it. Close-on-exec, like every other descriptor this library hands out.
        const Int frameFd = fcntl(fds[0], F_DUPFD_CLOEXEC, 0);
        if (frameFd < 0)
        {
            MGLOG_E_ONCE("gbm: cannot dup the descriptor of an offered frame (%d: %s)", errno, strerror(errno));
            return false;
        }

        // How long the frame really is, asked of the descriptor instead of computed from
        // the geometry: a gralloc dma-buf is usually larger than stride x height, and a
        // mapping has to cover what exists rather than what the arithmetic expects.
        SizeT size = 0;
        const off_t end = lseek(frameFd, 0, SEEK_END);
        if (end > 0)
        {
            size = (SizeT)end;
        }
        const SizeT described = (SizeT)offer.StrideBytes * (SizeT)offer.Height;
        if (size == 0)
        {
            // A descriptor that does not answer for its own length - an exporter that
            // does not implement llseek - leaves the offer's arithmetic as the only
            // length there is.
            size = described;
        }
        if (size < described)
        {
            MGLOG_W_ONCE("gbm: an offered frame's descriptor holds %zu bytes where %u rows of %u bytes need %zu; "
                         "the frame is skipped rather than mapped over memory that is not there",
                         size, offer.Height, offer.StrideBytes, described);
            GbmHostCloseQuietly(frameFd);
            return false;
        }

        frameOut.Seq = offer.Seq;
        frameOut.Index = offer.Index;
        frameOut.Width = offer.Width;
        frameOut.Height = offer.Height;
        frameOut.StrideBytes = offer.StrideBytes;
        frameOut.Format = offer.Format;
        frameOut.Usage = ((Uint64)offer.UsageHi << 32) | (Uint64)offer.UsageLo;
        frameOut.Generation = Generation;
        frameOut.Fd = frameFd;
        frameOut.Size = size;
        frameOut.BytesPerPixel = GbmFormatBytesPerPixel(offer.Format);
        if (frameOut.BytesPerPixel == 0)
        {
            // A format this library has no table entry for is still a frame the phone
            // drew. The offer's stride is then the only description of a pixel that
            // exists: a row of Width pixels is StrideBytes long.
            frameOut.BytesPerPixel = offer.StrideBytes / offer.Width;
        }

        MGLOG_I("gbm: frame seq=%u index=%u %ux%u stride=%u format=0x%08x (%u bytes per pixel, %zu bytes "
                "measured) taken from the display host",
                offer.Seq, offer.Index, offer.Width, offer.Height, offer.StrideBytes, offer.Format,
                frameOut.BytesPerPixel, size);
        return true;
    }

    void GbmHostChannel::ReleaseFrame(::gbm_device& device, Uint32 seq, Uint64 generation) noexcept
    {
        if (SocketFd < 0)
        {
            // A release only means anything on the connection that offered the frame,
            // and there is none.
            return;
        }
        if (generation != Generation)
        {
            // The frame was offered by a connection that has since died and been
            // replaced. Sending this into the replacement would name a frame of another
            // session, and the phone would recycle a slot this buffer was not made of.
            MGLOG_W_ONCE("gbm: a frame offered by a display host connection that has since died is not "
                         "released into its replacement");
            return;
        }
        if (PendingReleases.size() >= GbmHostMaxPendingReleases)
        {
            if (!ReleaseOverflowLogged)
            {
                ReleaseOverflowLogged = true;
                MGLOG_W("gbm: %zu frame releases to the display host at %s are already waiting for room in the "
                        "socket; this one is dropped rather than queued without bound, and the phone reclaims "
                        "the slot when the session ends",
                        PendingReleases.size(), Path.c_str());
            }
            return;
        }

        GbmFrameRelease release = {};
        release.Magic = GbmFrameReleaseMagic;
        release.Version = GbmHostProtocolVersion;
        release.Seq = seq;
        PendingReleases.push_back(release);

        if (!FlushReleases())
        {
            MarkDown(device, "sendmsg", errno);
        }
    }

    Bool GbmHostChannel::FlushReleases() noexcept
    {
        while (!PendingReleases.empty())
        {
            struct iovec releaseVector = {};
            releaseVector.iov_base = &PendingReleases.front();
            releaseVector.iov_len = sizeof(GbmFrameRelease);

            struct msghdr message = {};
            message.msg_iov = &releaseVector;
            message.msg_iovlen = 1;

            // MSG_DONTWAIT for the same reason every other call here is non-blocking, and
            // MSG_NOSIGNAL because a compositor killed by SIGPIPE when the phone goes
            // away is a desktop that disappears with it.
            const ssize_t sent = sendmsg(SocketFd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent == (ssize_t)sizeof(GbmFrameRelease))
            {
                PendingReleases.erase(PendingReleases.begin());
                continue;
            }
            if (sent < 0 && errno == EINTR)
            {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                // A full socket is a phone that has stopped reading rather than a dead
                // one: the release stays queued and goes out with a later frame.
                return true;
            }
            // A short send, a reset and a broken pipe all say the same thing: this
            // connection can no longer be trusted to have carried a twelve byte message,
            // and acting on a maybe is worse than handing the slot back when the session
            // ends. errno describes which of them it was.
            return false;
        }
        return true;
    }

    void GbmHostChannel::CloseSocket() noexcept
    {
        if (SocketFd >= 0)
        {
            GbmHostCloseQuietly(SocketFd);
            SocketFd = -1;
        }
        PendingReleases.clear();
    }

    void GbmHostChannel::MarkDown(::gbm_device& device, const char* what, Int error) noexcept
    {
        if (!PendingReleases.empty())
        {
            MGLOG_W("gbm: %zu frame release(s) were still queued for the display host and cannot be sent over a "
                    "connection that is gone; the phone reclaims those slots when the session ends",
                    PendingReleases.size());
        }
        CloseSocket();
        if (error != 0)
        {
            MGLOG_W("gbm: the display host connection at %s is down: %s failed (%d: %s); frames are allocated "
                    "locally from here on, and a new connection is attempted on a later allocation",
                    Path.c_str(), what, error, strerror(error));
        }
        else
        {
            MGLOG_W("gbm: the display host connection at %s is down: %s; frames are allocated locally from here "
                    "on, and a new connection is attempted on a later allocation",
                    Path.c_str(), what);
        }
        device.Impl.HostAvailable = false;
        device.Impl.BackendName = device.Impl.SelfAllocatedBackendName;
    }

    // ---- the device-facing entry points ------------------------------------

    void GbmHostChannelCreate(::gbm_device& device) noexcept
    {
        const char* configured = getenv("MOBILEGL_GBM_SOCKET");
        const String path =
            (configured != nullptr && configured[0] != '\0') ? String(configured) : String("/run/mobilegl-gbm.sock");
        device.Impl.SocketPath = path;
        try
        {
            GbmHostChannel* channel = new GbmHostChannel(path);
            device.Impl.Host = channel;
            // The one attempt made at creation. It is allowed to fail: a machine that is
            // not the container half of a split run has no display host, and that is a
            // device which allocates locally rather than a device that could not exist.
            channel->EnsureOpen(device);
            // A connection that came up is waited on once, and only here, so that the
            // compositor's swapchain is built out of the host's frames rather than out of
            // local ones it will keep for the life of the process.
            channel->AwaitFirstOffer(device);
        }
        catch (...)
        {
            device.Impl.Host = nullptr;
            device.Impl.HostAvailable = false;
            MGLOG_W("gbm: the display host channel could not be created; every frame is allocated locally");
        }
    }

    void GbmHostChannelDestroy(::gbm_device& device) noexcept
    {
        GbmHostChannel* channel = device.Impl.Host;
        if (channel == nullptr)
        {
            return;
        }
        device.Impl.Host = nullptr;
        device.Impl.HostAvailable = false;
        delete channel;
    }

    Bool GbmHostChannelTakeFrame(::gbm_device& device, GbmHostFrame& frameOut) noexcept
    {
        GbmHostChannel* channel = device.Impl.Host;
        if (channel == nullptr)
        {
            return false;
        }
        return channel->TakeFrame(device, frameOut);
    }

    void GbmHostChannelReleaseFrame(::gbm_device& device, Uint32 seq, Uint64 generation) noexcept
    {
        GbmHostChannel* channel = device.Impl.Host;
        if (channel == nullptr)
        {
            return;
        }
        channel->ReleaseFrame(device, seq, generation);
    }
} // namespace MobileGL::MG_Gbm

// End of Source File Header
