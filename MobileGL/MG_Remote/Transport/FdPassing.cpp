// MobileGL - MobileGL/MG_Remote/Transport/FdPassing.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FdPassing.h"

#include <MG_Util/Debug/Log.h>

#include <chrono>
#include <cstring>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// MSG_NOSIGNAL is Linux (and Android). macOS and the BSDs spell the same protection as the
// SO_NOSIGPIPE socket option, set once per socket at creation (CreateSocketPair below, and
// SocketDoorbell's constructor). With neither, a write to a hung-up peer raises SIGPIPE and
// kills the process instead of returning EPIPE.
#if !defined(_WIN32) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace MobileGL::MG_Remote::Transport::FdPassing {

#if defined(_WIN32)

    bool Supported() { return false; }

    MobileGLResult CreateSocketPair(int[2]) { return MOBILEGL_ERR_UNSUPPORTED; }

    MobileGLResult SendFd(int, int, MobileGLByteSpan) { return MOBILEGL_ERR_UNSUPPORTED; }

    MobileGLResult ReceiveFd(int, int*, MobileGLMutableByteSpan, std::uint64_t*, std::uint32_t) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }

#else

    namespace {
        // Every datagram starts with this, so the sideband length is explicit
        // and a stray datagram is recognisable.
        struct SidebandHeader {
            std::uint32_t magic;
            std::uint32_t sidebandSize;
        };
        constexpr std::uint32_t kSidebandMagic = 0x4446474Du; // 'MGFD' on the wire

        int WaitReadable(int socket, std::uint32_t timeoutMs) {
            const auto start = std::chrono::steady_clock::now();
            for (;;) {
                int pollTimeout = -1;
                if (timeoutMs != 0xFFFFFFFFu) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count();
                    const long long remaining = static_cast<long long>(timeoutMs) - elapsed;
                    pollTimeout = remaining <= 0 ? 0 : static_cast<int>(remaining);
                }
                struct pollfd pfd{};
                pfd.fd = socket;
                pfd.events = POLLIN;
                const int ready = ::poll(&pfd, 1, pollTimeout);
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                return ready;
            }
        }
    } // namespace

    bool Supported() { return true; }

    MobileGLResult CreateSocketPair(int outFds[2]) {
        if (outFds == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        int fds[2] = {-1, -1};
        int type = SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
        type |= SOCK_CLOEXEC;
#endif
        if (::socketpair(AF_UNIX, type, 0, fds) != 0) {
            MGLOG_E("MG_Remote fd passing: socketpair failed (errno=%d)", errno);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
#if defined(SO_NOSIGPIPE)
        // The per-socket form of MSG_NOSIGNAL, on the platforms that lack the per-call one.
        for (int fd : fds) {
            const int one = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        outFds[0] = fds[0];
        outFds[1] = fds[1];
        return MOBILEGL_OK;
    }

    MobileGLResult SendFd(int socket, int fd, MobileGLByteSpan sideband) {
        if (socket < 0 || fd < 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        if (sideband.size > kMaxSidebandBytes || (sideband.size != 0 && sideband.data == nullptr)) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        std::uint8_t payload[sizeof(SidebandHeader) + kMaxSidebandBytes];
        SidebandHeader header{};
        header.magic = kSidebandMagic;
        header.sidebandSize = static_cast<std::uint32_t>(sideband.size);
        std::memcpy(payload, &header, sizeof(header));
        if (sideband.size != 0) {
            std::memcpy(payload + sizeof(header), sideband.data,
                        static_cast<std::size_t>(sideband.size));
        }
        const std::size_t payloadSize = sizeof(header) + static_cast<std::size_t>(sideband.size);

        struct iovec iov{};
        iov.iov_base = payload;
        iov.iov_len = payloadSize;

        // CMSG_SPACE, not sizeof: the control buffer has to hold the aligned
        // cmsghdr as well as the descriptor.
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int))];
        } control{};
        std::memset(&control, 0, sizeof(control));

        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

        for (;;) {
            const ssize_t sent = ::sendmsg(socket, &msg, MSG_NOSIGNAL);
            if (sent >= 0) {
                if (static_cast<std::size_t>(sent) != payloadSize) {
                    // A datagram socket sends all or nothing.
                    MGLOG_E("MG_Remote fd passing: short datagram (%zd of %zu bytes)", sent,
                            payloadSize);
                    return MOBILEGL_ERR_TRANSPORT_CLOSED;
                }
                return MOBILEGL_OK;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            MGLOG_E("MG_Remote fd passing: sendmsg failed (errno=%d)", errno);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
    }

    MobileGLResult ReceiveFd(int socket, int* outFd, MobileGLMutableByteSpan sideband,
                             std::uint64_t* outSidebandSize, std::uint32_t timeoutMs) {
        if (socket < 0 || outFd == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *outFd = -1;
        if (outSidebandSize != nullptr) {
            *outSidebandSize = 0;
        }
        // Checked before the recvmsg: a datagram cannot be partially consumed,
        // so a too-small destination must never cost us the descriptor.
        if (sideband.size < kMaxSidebandBytes) {
            if (outSidebandSize != nullptr) {
                *outSidebandSize = kMaxSidebandBytes;
            }
            return MOBILEGL_ERR_BUFFER_TOO_SMALL;
        }
        if (sideband.data == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }

        const int ready = WaitReadable(socket, timeoutMs);
        if (ready < 0) {
            MGLOG_E("MG_Remote fd passing: poll failed (errno=%d)", errno);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
        if (ready == 0) {
            return MOBILEGL_ERR_TIMEOUT;
        }

        std::uint8_t payload[sizeof(SidebandHeader) + kMaxSidebandBytes];
        struct iovec iov{};
        iov.iov_base = payload;
        iov.iov_len = sizeof(payload);

        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(sizeof(int) * 4)];
        } control{};
        std::memset(&control, 0, sizeof(control));

        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.bytes;
        msg.msg_controllen = sizeof(control.bytes);

        ssize_t got = 0;
        for (;;) {
            int flags = 0;
#if defined(MSG_CMSG_CLOEXEC)
            flags |= MSG_CMSG_CLOEXEC;
#endif
            got = ::recvmsg(socket, &msg, flags);
            if (got >= 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECONNRESET) {
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            MGLOG_E("MG_Remote fd passing: recvmsg failed (errno=%d)", errno);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
        if (got == 0) {
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        // Collect every descriptor first, so an unexpected extra one is closed
        // rather than leaked, whatever else is wrong with the message.
        int received[4];
        int receivedCount = 0;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
            const int count = static_cast<int>(bytes / sizeof(int));
            for (int i = 0; i < count && receivedCount < 4; ++i) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(fd));
                received[receivedCount++] = fd;
            }
        }
#if !defined(MSG_CMSG_CLOEXEC)
        // No atomic close-on-exec on receive here (macOS, the BSDs): set it by hand on every
        // descriptor that arrived, before anything else can fork. The window between the
        // recvmsg and this loop is the platform's, not ours; leaving the flag off altogether
        // would hand every shared segment to every child the process ever spawns.
        for (int i = 0; i < receivedCount; ++i) {
            if (received[i] >= 0) {
                (void)::fcntl(received[i], F_SETFD, FD_CLOEXEC);
            }
        }
#endif
        const auto closeAll = [&](int keepIndex) {
            for (int i = 0; i < receivedCount; ++i) {
                if (i != keepIndex && received[i] >= 0) {
                    ::close(received[i]);
                }
            }
        };

        if ((msg.msg_flags & MSG_CTRUNC) != 0) {
            // The kernel dropped ancillary data: whatever arrived is not a
            // complete offer, and silently continuing would hand the caller a
            // half-transferred segment.
            MGLOG_E("MG_Remote fd passing: ancillary data truncated; the descriptor did not "
                    "arrive intact");
            closeAll(-1);
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        if (receivedCount != 1) {
            MGLOG_E("MG_Remote fd passing: expected exactly one descriptor, got %d", receivedCount);
            closeAll(-1);
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }
        if (static_cast<std::size_t>(got) < sizeof(SidebandHeader)) {
            MGLOG_E("MG_Remote fd passing: %zd byte datagram is shorter than the header", got);
            closeAll(-1);
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }

        SidebandHeader header{};
        std::memcpy(&header, payload, sizeof(header));
        if (header.magic != kSidebandMagic ||
            header.sidebandSize > kMaxSidebandBytes ||
            sizeof(SidebandHeader) + header.sidebandSize != static_cast<std::size_t>(got)) {
            MGLOG_E("MG_Remote fd passing: bad sideband header (magic=0x%08X size=%u datagram=%zd)",
                    header.magic, header.sidebandSize, got);
            closeAll(-1);
            return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        }

        if (header.sidebandSize != 0) {
            std::memcpy(sideband.data, payload + sizeof(SidebandHeader), header.sidebandSize);
        }
        if (outSidebandSize != nullptr) {
            *outSidebandSize = header.sidebandSize;
        }
        *outFd = received[0];
        closeAll(0);
        return MOBILEGL_OK;
    }

#endif // _WIN32

} // namespace MobileGL::MG_Remote::Transport::FdPassing
