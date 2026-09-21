// MobileGL - MobileGL/MG_Remote/Transport/SocketTransport.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SocketTransport.h"

#include "Doorbell.h" // kWaitForever
#include "FdPassing.h"
#include "WireLog.h"

#include <cerrno>
#include <cstring>
#include <memory>

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#    define MOBILEGL_SOCKET_TRANSPORT_POSIX 1
#    include <poll.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#else
#    define MOBILEGL_SOCKET_TRANSPORT_POSIX 0
#endif

namespace MobileGL::MG_Remote::Transport {

#if MOBILEGL_SOCKET_TRANSPORT_POSIX

    namespace {
        constexpr std::uint64_t kPumpChunkBytes = 64ull * 1024;

        void CloseIfOpen(int& fd) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }

        // poll() for readability. Returns:
        //   1  readable        0  timed out       -1  error (errno set)
        // EINTR is retried rather than reported: a signal is not a transport
        // event, and reporting it as one would make every caller's timeout
        // budget a lie on a process that takes signals (and `sm`'s child takes
        // SIGCHLD by construction).
        int WaitReadable(int fd, std::uint32_t timeoutMs) {
            for (;;) {
                struct pollfd pfd {};
                pfd.fd = fd;
                pfd.events = POLLIN;
                const int timeout =
                    timeoutMs == kWaitForever ? -1 : static_cast<int>(timeoutMs);
                const int rc = ::poll(&pfd, 1, timeout);
                if (rc < 0 && errno == EINTR) {
                    continue;
                }
                return rc;
            }
        }
    } // namespace

    MobileGLResult SocketTransport::CreatePair(std::unique_ptr<SocketTransport>& outClient,
                                               std::unique_ptr<SocketTransport>& outServer) {
        outClient.reset();
        outServer.reset();

        int stream[2] = {-1, -1};
        // SOCK_STREAM, not SOCK_SEQPACKET: Doorbell.h:390-398 already chose a
        // stream socket for the bell so that a closed peer surfaces as
        // POLLIN|POLLHUP with recv()==0 rather than as a hang, and the control
        // plane must not disagree with the bell about what death looks like.
        // Framing.h is what makes a stream safe: the message boundary is in the
        // payload, not in the socket.
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, stream) != 0) {
            WireLogError("MG_Remote SocketTransport: socketpair(SOCK_STREAM) failed: %s",
                         std::strerror(errno));
            return MOBILEGL_ERR_UNSUPPORTED;
        }

        int aux[2] = {-1, -1};
        const MobileGLResult auxResult = FdPassing::CreateSocketPair(aux);
        if (auxResult != MOBILEGL_OK) {
            ::close(stream[0]);
            ::close(stream[1]);
            WireLogError("MG_Remote SocketTransport: the aux (SCM_RIGHTS) pair failed");
            return auxResult;
        }

        outClient = std::make_unique<SocketTransport>(stream[0], aux[0], TransportRole::Client);
        outServer = std::make_unique<SocketTransport>(stream[1], aux[1], TransportRole::Server);
        return MOBILEGL_OK;
    }

    SocketTransport::SocketTransport(int streamFd, int auxFd, TransportRole role)
        : m_streamFd(streamFd), m_auxFd(auxFd), m_role(role) {}

    SocketTransport::~SocketTransport() { Shutdown(); }

    MobileGLResult SocketTransport::SendFrame(MobileGLByteSpan bytes) {
        if (bytes.size > kMaxFramePayloadSize) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (bytes.size != 0 && bytes.data == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_closed || m_streamFd < 0) {
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        // Framed into ONE buffer and written as one span. Not because a stream
        // needs it - it does not - but because a partial write of the header
        // followed by a failure would leave the peer's reassembler parked on a
        // length it will never receive, which is indistinguishable from a slow
        // sender and therefore not detectable at all.
        std::vector<std::uint8_t> framed;
        framed.reserve(static_cast<std::size_t>(kFrameHeaderSize + bytes.size));
        const MobileGLResult appended = AppendFrame(framed, bytes.data, bytes.size);
        if (appended != MOBILEGL_OK) {
            return appended;
        }

        std::uint64_t written = 0;
        while (written < framed.size()) {
            const ssize_t n = ::send(m_streamFd, framed.data() + written,
                                     static_cast<std::size_t>(framed.size() - written),
                                     MSG_NOSIGNAL);
            if (n > 0) {
                written += static_cast<std::uint64_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            // EPIPE / ECONNRESET are the peer being gone, which is a transport
            // state and not an error to shout about: the device-lost latch (dl)
            // is what turns it into a GL-visible fact.
            if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                m_closed = true;
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            WireLogError("MG_Remote SocketTransport: send failed after %llu of %llu bytes: %s",
                         static_cast<unsigned long long>(written),
                         static_cast<unsigned long long>(framed.size()), std::strerror(errno));
            m_closed = true;
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::PumpOnce(std::uint32_t timeoutMs) {
        // Caller holds m_recvMutex.
        if (m_failed) {
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        if (m_streamFd < 0) {
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        const int ready = WaitReadable(m_streamFd, timeoutMs);
        if (ready == 0) {
            return MOBILEGL_ERR_TIMEOUT;
        }
        if (ready < 0) {
            WireLogError("MG_Remote SocketTransport: poll failed: %s", std::strerror(errno));
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        std::uint8_t chunk[kPumpChunkBytes];
        for (;;) {
            const ssize_t n = ::recv(m_streamFd, chunk, sizeof(chunk), 0);
            if (n > 0) {
                const MobileGLResult fed =
                    m_reader.Feed(chunk, static_cast<std::uint64_t>(n));
                if (fed != MOBILEGL_OK) {
                    // Bad magic or an over-long length. FrameReader latches
                    // itself failed and never recovers; mirror that here so a
                    // caller cannot retry its way past a corrupt stream.
                    m_failed = true;
                    WireLogError("MG_Remote SocketTransport: framing violated on the stream; "
                                 "the transport is latched failed");
                    return MOBILEGL_ERR_PROTOCOL_MISMATCH;
                }
                return MOBILEGL_OK;
            }
            if (n == 0) {
                // Clean EOF. The peer is gone - but anything already reassembled
                // stays readable, which ITransport promises in as many words.
                m_closed = true;
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return MOBILEGL_ERR_TIMEOUT;
            }
            if (errno == ECONNRESET) {
                m_closed = true;
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            WireLogError("MG_Remote SocketTransport: recv failed: %s", std::strerror(errno));
            m_closed = true;
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
    }

    MobileGLResult SocketTransport::ReceiveFrame(MobileGLMutableByteSpan buffer,
                                                 std::uint64_t* outSize,
                                                 std::uint32_t timeoutMs) {
        if (outSize == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *outSize = 0;

        std::lock_guard<std::mutex> lock(m_recvMutex);
        if (m_failed) {
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }

        for (;;) {
            // Drain the reassembler FIRST, every time. A message that is already
            // whole must be returned even when the peer has since closed, and
            // must be returned without a syscall when the caller is polling.
            if (m_reader.HasMessage()) {
                return m_reader.TakeMessage(buffer, outSize);
            }
            if (m_closed) {
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }

            const MobileGLResult pumped = PumpOnce(timeoutMs);
            if (pumped == MOBILEGL_OK) {
                continue; // maybe a whole message now; maybe a fragment - loop decides
            }
            if (pumped == MOBILEGL_ERR_TRANSPORT_CLOSED && m_reader.HasMessage()) {
                return m_reader.TakeMessage(buffer, outSize);
            }
            return pumped;
        }
    }

    std::uint64_t SocketTransport::PeekFrameSize() {
        std::lock_guard<std::mutex> lock(m_recvMutex);
        if (m_failed) {
            return 0;
        }
        if (!m_reader.HasMessage() && !m_closed) {
            // A non-blocking pump, so a caller that has not received yet can
            // still size its buffer. Errors are not reported here: PeekFrameSize
            // answers "how big is the next message", and 0 means "none buffered",
            // which is the truthful answer for every failure this can hit.
            (void)PumpOnce(0);
        }
        return m_reader.PendingMessageSize();
    }

    MobileGLResult SocketTransport::ShareFd(int fd, MobileGLByteSpan sideband) {
        if (m_auxFd < 0) {
            // A link that declared no descriptor passing. Rule G: it does not
            // invent a third answer, and the caller transfers the bytes instead.
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_closed) {
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
        return FdPassing::SendFd(m_auxFd, fd, sideband);
    }

    MobileGLResult SocketTransport::ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband,
                                              std::uint64_t* outSidebandSize,
                                              std::uint32_t timeoutMs) {
        if (m_auxFd < 0) {
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        return FdPassing::ReceiveFd(m_auxFd, outFd, sideband, outSidebandSize, timeoutMs);
    }

    void SocketTransport::Shutdown() {
        // Idempotent, and tears down BOTH directions - which on a socket is what
        // closing does anyway. Taking both locks in a fixed order (send, then
        // recv) so a concurrent sender and the dedicated reader cannot deadlock
        // against each other here; every other path takes exactly one.
        std::lock_guard<std::mutex> sendLock(m_sendMutex);
        std::lock_guard<std::mutex> recvLock(m_recvMutex);
        if (m_streamFd >= 0) {
            // shutdown() before close() so a peer blocked in recv() wakes now
            // rather than whenever the last duplicate of this fd goes away.
            ::shutdown(m_streamFd, SHUT_RDWR);
        }
        CloseIfOpen(m_streamFd);
        CloseIfOpen(m_auxFd);
        m_closed = true;
    }

#else // !MOBILEGL_SOCKET_TRANSPORT_POSIX

    // Windows has no SCM_RIGHTS and ShmSegment::Adopt is POSIX-only, so P6 lands
    // POSIX only (CONTRACT-P6 §2.6). The refusal is NAMED at construction rather
    // than at first use: a transport that accepts a connection and then fails
    // every operation is the silent-fallback shape this project forbids.
    MobileGLResult SocketTransport::CreatePair(std::unique_ptr<SocketTransport>&,
                                               std::unique_ptr<SocketTransport>&) {
        WireLogError("MG_Remote SocketTransport: unsupported on this platform - P6 lands POSIX "
                     "only (CONTRACT-P6 §2.6); `unix:` and `pipe:` stay named refusals");
        return MOBILEGL_ERR_UNSUPPORTED;
    }

    SocketTransport::SocketTransport(int, int, TransportRole role) : m_role(role) {}
    SocketTransport::~SocketTransport() = default;

    MobileGLResult SocketTransport::SendFrame(MobileGLByteSpan) { return MOBILEGL_ERR_UNSUPPORTED; }
    MobileGLResult SocketTransport::ReceiveFrame(MobileGLMutableByteSpan, std::uint64_t*,
                                                 std::uint32_t) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    std::uint64_t SocketTransport::PeekFrameSize() { return 0; }
    MobileGLResult SocketTransport::ShareFd(int, MobileGLByteSpan) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult SocketTransport::ReceiveFd(int*, MobileGLMutableByteSpan, std::uint64_t*,
                                              std::uint32_t) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    MobileGLResult SocketTransport::PumpOnce(std::uint32_t) { return MOBILEGL_ERR_UNSUPPORTED; }
    void SocketTransport::Shutdown() { m_closed = true; }

#endif

} // namespace MobileGL::MG_Remote::Transport
