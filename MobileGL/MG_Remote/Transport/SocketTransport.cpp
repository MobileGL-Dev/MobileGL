// MobileGL - MobileGL/MG_Remote/Transport/SocketTransport.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SocketTransport.h"

#include "AuthToken.h"
#include "Doorbell.h" // kWaitForever
#include "FdPassing.h"
#include "WireLog.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cstdio>

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#    define MOBILEGL_SOCKET_TRANSPORT_POSIX 1
#    include <poll.h>
#    include <sys/stat.h>
#    include <sys/un.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    if defined(__linux__)
#        include <sys/syscall.h>
#    endif
#else
#    define MOBILEGL_SOCKET_TRANSPORT_POSIX 0
#endif

namespace MobileGL::MG_Remote::Transport {

#if MOBILEGL_SOCKET_TRANSPORT_POSIX

    namespace {
        constexpr std::uint64_t kPumpChunkBytes = 64ull * 1024;

        bool TcpName(const std::string& name) { return name.compare(0, 6, "tcp://") == 0; }

        bool TcpAddress(const std::string& name, addrinfo** result) {
            *result = nullptr;
            const std::string address = name.substr(6);
            std::string host, port;
            if (!address.empty() && address[0] == '[') {
                const auto end = address.find(']');
                if (end == std::string::npos || end + 1 >= address.size() || address[end + 1] != ':')
                    return false;
                host = address.substr(1, end - 1);
                port = address.substr(end + 2);
            } else {
                const auto colon = address.find(':');
                if (colon == std::string::npos || address.find(':', colon + 1) != std::string::npos)
                    return false;
                host = address.substr(0, colon);
                port = address.substr(colon + 1);
            }
            if (host.empty() || port.empty() || port.find_first_not_of("0123456789") != std::string::npos)
                return false;
            char* end = nullptr;
            const auto number = std::strtoul(port.c_str(), &end, 10);
            if (*end || number == 0 || number > 65535) return false;
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_flags = AI_NUMERICSERV;
            return ::getaddrinfo(host.c_str(), port.c_str(), &hints, result) == 0;
        }

        bool Loopback(const sockaddr* address) {
            if (address->sa_family == AF_INET)
                return (ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) >> 24) == 127;
            if (address->sa_family == AF_INET6)
                return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
            return false;
        }

        bool TcpSocket(int fd) {
            sockaddr_storage address{};
            socklen_t size = sizeof(address);
            return ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0 &&
                   (address.ss_family == AF_INET || address.ss_family == AF_INET6);
        }

        bool ConfigureTcp(int fd) {
            const int one = 1, idle = 2, interval = 1, count = 3, timeout = 5000;
            if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0 ||
                ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) != 0) return false;
#if defined(TCP_KEEPIDLE)
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0) return false;
#endif
#if defined(TCP_KEEPINTVL)
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0) return false;
#endif
#if defined(TCP_KEEPCNT)
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) return false;
#endif
#if defined(TCP_USER_TIMEOUT)
            if (::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout)) != 0) return false;
#endif
            return true;
        }

        MobileGLResult ConnectTcp(const std::string& name, std::uint32_t timeoutMs, int* outFd) {
            addrinfo* addresses = nullptr;
            if (!TcpAddress(name, &addresses)) return MOBILEGL_ERR_INVALID_ARGUMENT;
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max(1u, timeoutMs));
            do {
                for (auto* address = addresses; address; address = address->ai_next) {
                    const int fd = ::socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP);
                    if (fd < 0) continue;
                    const int flags = ::fcntl(fd, F_GETFL, 0);
                    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                    bool connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
                    if (!connected && errno == EINPROGRESS) {
                        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()).count();
                        pollfd pfd{fd, POLLOUT, 0};
                        int rc;
                        do { rc = ::poll(&pfd, 1, static_cast<int>(std::max<std::int64_t>(0, remaining))); }
                        while (rc < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
                        int error = 0;
                        socklen_t length = sizeof(error);
                        connected = rc > 0 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
                    }
                    if (connected && ConfigureTcp(fd)) {
                        ::fcntl(fd, F_SETFL, flags);
                        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
                        *outFd = fd;
                        ::freeaddrinfo(addresses);
                        return MOBILEGL_OK;
                    }
                    ::close(fd);
                    if (std::chrono::steady_clock::now() >= deadline) break;
                }
                ::usleep(5000);
            } while (std::chrono::steady_clock::now() < deadline);
            ::freeaddrinfo(addresses);
            WireLogError("MG_Remote SocketTransport: TCP connect to %s exhausted %u ms", name.c_str(), timeoutMs);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }

        MobileGLResult ListenTcp(const std::string& name, int* outFd) {
            addrinfo* addresses = nullptr;
            if (!TcpAddress(name, &addresses)) return MOBILEGL_ERR_INVALID_ARGUMENT;
            // PH-7 (3), ID-P7-3. A CONFIGURED TOKEN IS EITHER A CREDENTIAL OR A REFUSAL.
            //
            // This read `getenv(...) != nullptr && token[0] != '\0'`, so ANY non-empty value -
            // one byte - opened a `tcp://0.0.0.0` listen. The listen is the only place that can
            // still say no cheaply and to the operator rather than to a peer, so the length is
            // asked here, and a short token fails the listen outright instead of quietly
            // becoming a loopback-only server that looks exactly like a working one.
            // AuthToken.h holds the constant and the reason.
            const char* token = ConfiguredAuthToken();
            if (token != nullptr && !AuthTokenIsLongEnough(token)) {
                WireLogError("MG_Remote: Refuse{Authentication} MOBILEGL_IPC_TOKEN is %zu bytes "
                             "and the minimum is %zu (PH-7 (3)); refusing to listen on %s rather "
                             "than authenticating with a guessable secret",
                             std::strlen(token), kMinimumAuthTokenBytes, name.c_str());
                ::freeaddrinfo(addresses);
                return MOBILEGL_ERR_PROTOCOL_MISMATCH;
            }
            const bool authenticated = token != nullptr;
            MobileGLResult result = MOBILEGL_ERR_UNSUPPORTED;
            for (auto* address = addresses; address; address = address->ai_next) {
                if (!authenticated && !Loopback(address->ai_addr)) {
                    // The word is RefuseCode's own enumerator. It read `AuthenticationRequired`
                    // for a whole phase, naming a value protocol.fbs has never had, so a reader
                    // grepping the refusal vocabulary found a word the wire cannot carry (PH-7
                    // (2)); fatal_census.py's rule 4 now refuses that.
                    WireLogError("MG_Remote: Refuse{Authentication} non-loopback TCP listen needs MOBILEGL_IPC_TOKEN");
                    result = MOBILEGL_ERR_PROTOCOL_MISMATCH;
                    continue;
                }
                const int fd = ::socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP);
                if (fd < 0) continue;
                const int one = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
                if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 && ::listen(fd, 8) == 0) {
                    *outFd = fd;
                    result = MOBILEGL_OK;
                    break;
                }
                ::close(fd);
            }
            ::freeaddrinfo(addresses);
            return result;
        }

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

    namespace {
        // A filesystem AF_UNIX path, bounded by sun_path. Refused BY NAME when it
        // does not fit rather than silently truncated - a truncated path is a
        // different socket, and both sides would then "succeed" onto two
        // different names.
        // A LEADING '@' MEANS THE ABSTRACT NAMESPACE, which is the convention systemd and D-Bus
        // use and the only rendezvous that works on Android.
        //
        // A filesystem AF_UNIX endpoint needs a writable directory, and an Android app has no
        // /tmp - measured, not assumed: the first device run of the spawn arm launched its server
        // fine and then died on `could not connect to "/tmp/mgl-15731-...sock" ... No such file or
        // directory`, because bind() in the child had nowhere to put the node. The app's own cache
        // dir would work, but only if every integrator remembered to say where it is, and the
        // library has no way to ask.
        //
        // An abstract name has no filesystem presence at all: no directory, no mode bits, no stale
        // node to unlink after a crash - which is the bind() failure `Listen` currently works
        // around. It is scoped to the network namespace, so the two processes of ONE app find each
        // other and nothing else can, which is exactly the reach this needs.
        //
        // Linux-only by construction. A path without '@' still works everywhere, and that is what
        // an explicit endpoint (P12's "connect to something somebody else is listening on") uses.
        bool IsAbstractEndpoint(const std::string& path) {
            return !path.empty() && path[0] == '@';
        }

        bool FillAddress(const std::string& path, sockaddr_un* out, socklen_t* outLen) {
            std::memset(out, 0, sizeof(*out));
            out->sun_family = AF_UNIX;
            if (path.empty() || path.size() >= sizeof(out->sun_path)) {
                WireLogError("MG_Remote SocketTransport: endpoint path is empty or longer than "
                             "sun_path allows (%zu >= %zu): \"%s\"",
                             path.size(), sizeof(out->sun_path), path.c_str());
                return false;
            }
            if (IsAbstractEndpoint(path)) {
                // sun_path[0] STAYS NUL and the rest is the name. The length carries the name's
                // end - there is no terminator - so it is offsetof + 1 + (size - 1), which is
                // offsetof + size. Getting this wrong does not fail: it binds a DIFFERENT name
                // (one padded with NULs), and the peer then connects to something that is not
                // there.
                std::memcpy(out->sun_path + 1, path.c_str() + 1, path.size() - 1);
                *outLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
                return true;
            }
            std::memcpy(out->sun_path, path.c_str(), path.size());
            *outLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
            return true;
        }

        MobileGLResult ConnectOne(const std::string& path, std::uint32_t timeoutMs, int* outFd) {
            if (TcpName(path)) return ConnectTcp(path, timeoutMs, outFd);
            sockaddr_un address{};
            socklen_t length = 0;
            if (!FillAddress(path, &address, &length)) {
                return MOBILEGL_ERR_INVALID_ARGUMENT;
            }
            // Retry with a bound budget: the server may not have reached bind()
            // yet. An exhausted budget is a NAMED refusal, never a fallback -
            // CONTRACT-P6 §3.4 calls for a bounded retry and says so.
            const std::uint32_t deadline = timeoutMs == 0 ? 1 : timeoutMs;
            for (std::uint32_t waited = 0;; waited += 5) {
                const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) {
                    WireLogError("MG_Remote SocketTransport: socket() failed: %s",
                                 std::strerror(errno));
                    return MOBILEGL_ERR_UNSUPPORTED;
                }
                if (::connect(fd, reinterpret_cast<sockaddr*>(&address), length) == 0) {
                    *outFd = fd;
                    return MOBILEGL_OK;
                }
                const int failure = errno;
                ::close(fd);
                if (waited >= deadline) {
                    WireLogError("MG_Remote SocketTransport: could not connect to \"%s\" within "
                                 "%u ms: %s - REFUSING BY NAME, there is no monolith fallback "
                                 "from here",
                                 path.c_str(), deadline, std::strerror(failure));
                    return MOBILEGL_ERR_TRANSPORT_CLOSED;
                }
                ::usleep(5000);
            }
        }
    } // namespace

    MobileGLResult SocketTransport::Listen(const std::string& path, int* outListenFd) {
        if (outListenFd == nullptr) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        *outListenFd = -1;
        if (TcpName(path)) return ListenTcp(path, outListenFd);
        sockaddr_un address{};
        socklen_t length = 0;
        if (!FillAddress(path, &address, &length)) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        // A stale socket file from a server that died is the normal case, not the
        // exceptional one, and bind() fails on it with EADDRINUSE. Unlinking first
        // is what every AF_UNIX server does; the alternative is a server that
        // cannot restart until someone cleans up by hand. An ABSTRACT name has no
        // node to go stale and unlink("@...") would delete a file of that literal
        // name if one happened to exist.
        const bool abstractEndpoint = IsAbstractEndpoint(path);
        if (!abstractEndpoint) {
            ::unlink(path.c_str());
        }

        const int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0) {
            WireLogError("MG_Remote SocketTransport: socket() failed: %s", std::strerror(errno));
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&address), length) != 0) {
            WireLogError("MG_Remote SocketTransport: bind(\"%s\") failed: %s", path.c_str(),
                         std::strerror(errno));
            ::close(listenFd);
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        // 0600: the rendezvous is between one user's processes. An AF_UNIX path
        // is a filesystem object and its mode is the only access control there is.
        // An abstract name is not a filesystem object and has none; its reach is
        // the network namespace, which for an Android app is the app.
        if (!abstractEndpoint) {
            ::chmod(path.c_str(), 0600);
        }
        if (::listen(listenFd, 4) != 0) {
            WireLogError("MG_Remote SocketTransport: listen(\"%s\") failed: %s", path.c_str(),
                         std::strerror(errno));
            ::close(listenFd);
            if (!abstractEndpoint) {
                ::unlink(path.c_str());
            }
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        *outListenFd = listenFd;
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::AcceptPair(int listenFd, std::uint32_t timeoutMs,
                                               std::unique_ptr<SocketTransport>& outServer) {
        outServer.reset();
        if (listenFd < 0) {
            return MOBILEGL_ERR_INVALID_ARGUMENT;
        }
        int accepted[2] = {-1, -1};
        for (int index = 0; index < 2; ++index) {
            const std::uint32_t budget = index == 0 ? timeoutMs : 2000;
            for (;;) {
                const int ready = WaitReadable(listenFd, budget);
                if (ready <= 0) {
                    for (int fd : accepted) {
                        if (fd >= 0) ::close(fd);
                    }
                    // Half a client is not a client. Both connections or neither, so
                    // a peer that died between the two cannot leave a session that
                    // looks up but has no way to receive a descriptor.
                    if (index != 0 || ready < 0) WireLogError("MG_Remote SocketTransport: only %d of 2 connections arrived within "
                                 "%u ms", index, timeoutMs);
                    return ready == 0 ? MOBILEGL_ERR_TIMEOUT : MOBILEGL_ERR_TRANSPORT_CLOSED;
                }
                accepted[index] = ::accept(listenFd, nullptr, nullptr);
                if (accepted[index] >= 0) break;
                // EINTR (this process reaps SIGCHLD by construction) and ECONNABORTED
                // (the peer reset between poll() and accept()) are not transport
                // failures: the listener is still good. Re-poll within the budget
                // rather than returning TRANSPORT_CLOSED, which the --serve
                // supervisor turns into a full server exit (return 73).
                if (errno == EINTR || errno == ECONNABORTED) continue;
                for (int fd : accepted) {
                    if (fd >= 0) ::close(fd);
                }
                WireLogError("MG_Remote SocketTransport: accept failed: %s", std::strerror(errno));
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            ::fcntl(accepted[index], F_SETFD, FD_CLOEXEC);
            if (TcpSocket(accepted[index]) && !ConfigureTcp(accepted[index])) {
                for (int fd : accepted) if (fd >= 0) ::close(fd);
                return MOBILEGL_ERR_UNSUPPORTED;
            }
        }
        // First connection is control, second is aux. The order IS the protocol;
        // it is stated here and in ConnectTo and nowhere else.
        outServer = std::make_unique<SocketTransport>(accepted[0], accepted[1],
                                                      TransportRole::Server);
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::ConnectTo(const std::string& path, std::uint32_t timeoutMs,
                                              std::unique_ptr<SocketTransport>& outClient) {
        outClient.reset();
        int control = -1;
        const MobileGLResult first = ConnectOne(path, timeoutMs, &control);
        if (first != MOBILEGL_OK) {
            return first;
        }
        int aux = -1;
        // The second connect has a SHORT budget: the server is provably up - we
        // just connected to it - so a slow second connection means something is
        // wrong rather than something is starting.
        const MobileGLResult second = ConnectOne(path, 2000, &aux);
        if (second != MOBILEGL_OK) {
            ::close(control);
            return second;
        }
        outClient = std::make_unique<SocketTransport>(control, aux, TransportRole::Client);
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::ConnectControl(const std::string& path, std::uint32_t timeoutMs,
                                                   std::unique_ptr<SocketTransport>& outClient) {
        if (!TcpName(path)) return ConnectTo(path, timeoutMs, outClient);
        outClient.reset();
        int control = -1;
        const MobileGLResult connected = ConnectOne(path, timeoutMs, &control);
        if (connected != MOBILEGL_OK) return connected;
        outClient = std::make_unique<SocketTransport>(control, -1, TransportRole::Client);
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::ConnectDataConnection(const std::string& path, std::uint32_t timeoutMs,
                                                          MobileGLByteSpan firstFrame, int* outFd) {
        if (outFd == nullptr || !TcpName(path)) return MOBILEGL_ERR_INVALID_ARGUMENT;
        *outFd = -1;
        std::vector<std::uint8_t> framed;
        const MobileGLResult appended = AppendFrame(framed, firstFrame.data, firstFrame.size);
        if (appended != MOBILEGL_OK) return appended;
        int fd = -1;
        const MobileGLResult connected = ConnectOne(path, timeoutMs, &fd);
        if (connected != MOBILEGL_OK) return connected;
        std::size_t written = 0;
        while (written < framed.size()) {
            const ssize_t n = ::send(fd, framed.data() + written, framed.size() - written, MSG_NOSIGNAL);
            if (n > 0) { written += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            WireLogError("MG_Remote SocketTransport: the data connection's DataBind could not be "
                         "sent (%zu of %zu bytes): %s", written, framed.size(), std::strerror(errno));
            ::close(fd);
            return MOBILEGL_ERR_TRANSPORT_CLOSED;
        }
        *outFd = fd;
        return MOBILEGL_OK;
    }

    MobileGLResult SocketTransport::AcceptOne(int listenFd, std::uint32_t timeoutMs, int* outFd) {
        if (listenFd < 0 || outFd == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
        *outFd = -1;
        for (;;) {
            const int ready = WaitReadable(listenFd, timeoutMs);
            if (ready == 0) return MOBILEGL_ERR_TIMEOUT;
            if (ready < 0) return MOBILEGL_ERR_TRANSPORT_CLOSED;
            const int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0) {
                // The same two non-failures AcceptPair re-polls on.
                if (errno == EINTR || errno == ECONNABORTED) continue;
                WireLogError("MG_Remote SocketTransport: accept failed: %s", std::strerror(errno));
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (TcpSocket(fd) && !ConfigureTcp(fd)) {
                ::close(fd);
                continue;
            }
            *outFd = fd;
            return MOBILEGL_OK;
        }
    }

    MobileGLResult SocketTransport::ReceiveOneFrame(int fd, std::uint32_t timeoutMs, std::uint64_t maxBytes,
                                                    std::vector<std::uint8_t>* outPayload) {
        if (fd < 0 || outPayload == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
        outPayload->clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        const auto readExactly = [&](std::uint8_t* into, std::size_t size) -> MobileGLResult {
            while (size != 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) return MOBILEGL_ERR_TIMEOUT;
                const int ready = WaitReadable(fd, static_cast<std::uint32_t>(left));
                if (ready == 0) return MOBILEGL_ERR_TIMEOUT;
                if (ready < 0) return MOBILEGL_ERR_TRANSPORT_CLOSED;
                const ssize_t n = ::recv(fd, into, size, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return MOBILEGL_ERR_TRANSPORT_CLOSED;
                into += n;
                size -= static_cast<std::size_t>(n);
            }
            return MOBILEGL_OK;
        };
        std::uint8_t header[kFrameHeaderSize];
        const MobileGLResult headerRead = readExactly(header, sizeof(header));
        if (headerRead != MOBILEGL_OK) return headerRead;
        std::uint32_t magic = 0, length = 0;
        std::memcpy(&magic, header, sizeof(magic));
        std::memcpy(&length, header + 4, sizeof(length));
        if (magic != kFrameMagic || length > maxBytes) return MOBILEGL_ERR_PROTOCOL_MISMATCH;
        outPayload->resize(length);
        const MobileGLResult payloadRead = readExactly(outPayload->data(), length);
        if (payloadRead != MOBILEGL_OK) outPayload->clear();
        return payloadRead;
    }

    MobileGLResult SocketTransport::MintNonce(std::uint8_t* out, std::size_t size) {
        if (out == nullptr) return MOBILEGL_ERR_INVALID_ARGUMENT;
        std::size_t filled = 0;
#if defined(__linux__) && defined(SYS_getrandom)
        while (filled < size) {
            const long n = ::syscall(SYS_getrandom, out + filled, size - filled, 0);
            if (n > 0) { filled += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            break; // ENOSYS on an old kernel: the device file below is the same pool.
        }
#endif
        if (filled < size) {
            const int source = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            while (source >= 0 && filled < size) {
                const ssize_t n = ::read(source, out + filled, size - filled);
                if (n > 0) { filled += static_cast<std::size_t>(n); continue; }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            if (source >= 0) ::close(source);
        }
        if (filled == size) return MOBILEGL_OK;
        WireLogError("MG_Remote SocketTransport: the kernel CSPRNG gave %zu of %zu bytes; a data "
                     "nonce is not minted from anything weaker", filled, size);
        return MOBILEGL_ERR_UNSUPPORTED;
    }

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
        : m_streamFd(streamFd), m_auxFd(auxFd), m_tcp(TcpSocket(streamFd)), m_role(role) {}

    int SocketTransport::TakeDataFd() {
        if (!m_tcp) return -1;
        const int fd = m_auxFd;
        m_auxFd = -1;
        return fd;
    }

    void SocketTransport::CloseLocalCopy() {
        CloseIfOpen(m_streamFd);
        CloseIfOpen(m_auxFd);
        m_closed = true;
    }

    MobileGLResult SocketTransport::ShutdownSend() {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (m_streamFd < 0 || m_closed) return MOBILEGL_ERR_TRANSPORT_CLOSED;
        return ::shutdown(m_streamFd, SHUT_WR) == 0 ? MOBILEGL_OK : MOBILEGL_ERR_TRANSPORT_CLOSED;
    }

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
            // SendFrame is also the log sink: do not recurse into the logger.
            std::fprintf(stderr, "MG_Remote SocketTransport: send failed after %llu of %llu bytes: %s\n",
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
                // Request EOF can be a half-close: drain queued frames and keep
                // the reverse direction usable for teardown diagnostics.
                m_readClosed = true;
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
            if (m_closed || m_readClosed) {
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
        if (!m_reader.HasMessage() && !m_closed && !m_readClosed) {
            // A non-blocking pump, so a caller that has not received yet can
            // still size its buffer. Errors are not reported here: PeekFrameSize
            // answers "how big is the next message", and 0 means "none buffered",
            // which is the truthful answer for every failure this can hit.
            (void)PumpOnce(0);
        }
        return m_reader.PendingMessageSize();
    }

    std::uint64_t SocketTransport::BufferedBytes() {
        std::lock_guard<std::mutex> lock(m_recvMutex);
        return m_reader.BufferedBytes();
    }

    MobileGLResult SocketTransport::ShareFd(int fd, MobileGLByteSpan sideband) {
        if (m_tcp || m_auxFd < 0) {
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
        if (m_tcp || m_auxFd < 0) {
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        return FdPassing::ReceiveFd(m_auxFd, outFd, sideband, outSidebandSize, timeoutMs);
    }

    void SocketTransport::Shutdown() {
        // Wake a blocking ReceiveFrame before taking its lock.
        if (m_streamFd >= 0) ::shutdown(m_streamFd, SHUT_RDWR);
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
    int SocketTransport::TakeDataFd() { return -1; }
    void SocketTransport::CloseLocalCopy() {}
    MobileGLResult SocketTransport::ShutdownSend() { return MOBILEGL_ERR_UNSUPPORTED; }

    MobileGLResult SocketTransport::SendFrame(MobileGLByteSpan) { return MOBILEGL_ERR_UNSUPPORTED; }
    MobileGLResult SocketTransport::ReceiveFrame(MobileGLMutableByteSpan, std::uint64_t*,
                                                 std::uint32_t) {
        return MOBILEGL_ERR_UNSUPPORTED;
    }
    std::uint64_t SocketTransport::PeekFrameSize() { return 0; }
    std::uint64_t SocketTransport::BufferedBytes() { return 0; }
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
