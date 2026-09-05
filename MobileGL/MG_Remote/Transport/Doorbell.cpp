// MobileGL - MobileGL/MG_Remote/Transport/Doorbell.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Doorbell.h"

#include <MG_Util/Debug/Log.h>

#include <condition_variable>
#include <mutex>

#if !defined(_WIN32)
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace MobileGL::MG_Remote::Transport {

    // -----------------------------------------------------------------------
    // CondVarDoorbell
    // -----------------------------------------------------------------------

    struct CondVarDoorbell::Impl {
        std::mutex mutex;
        std::condition_variable cv;
        // Counted, not a flag: a wakeup that arrives while nobody is parked
        // must still be observed by the next Park.
        std::uint32_t signals = 0;
    };

    CondVarDoorbell::CondVarDoorbell() : m_impl(new Impl()) {}

    CondVarDoorbell::~CondVarDoorbell() { delete m_impl; }

    void CondVarDoorbell::Notify() {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            ++m_impl->signals;
        }
        m_impl->cv.notify_one();
    }

    bool CondVarDoorbell::Park(std::uint32_t timeoutMs) {
        std::unique_lock<std::mutex> lock(m_impl->mutex);
        if (m_impl->signals != 0) {
            --m_impl->signals;
            return true;
        }
        if (timeoutMs == 0) {
            return false;
        }
        if (timeoutMs == kWaitForever) {
            m_impl->cv.wait(lock, [this] { return m_impl->signals != 0; });
        } else if (!m_impl->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [this] { return m_impl->signals != 0; })) {
            return false;
        }
        --m_impl->signals;
        return true;
    }

    void CondVarDoorbell::Reset() {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->signals = 0;
    }

#if !defined(_WIN32)

    // -----------------------------------------------------------------------
    // SocketDoorbell
    // -----------------------------------------------------------------------

    SocketDoorbell::SocketDoorbell(int fd, std::uint8_t code, bool ownsFd)
        : m_fd(fd), m_code(code), m_ownsFd(ownsFd) {}

    SocketDoorbell::~SocketDoorbell() {
        if (m_ownsFd && m_fd >= 0) {
            ::close(m_fd);
        }
    }

    void SocketDoorbell::Notify() {
        if (m_fd < 0) {
            return;
        }
        const std::uint8_t byte = m_code;
        for (;;) {
            const ssize_t written = ::send(m_fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (written == 1) {
                return;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // The socket buffer already holds unread wakeups: the peer has
                // one pending, which is all a doorbell promises.
                return;
            }
            if (written < 0 && errno == EPIPE) {
                return; // peer gone; the waiter learns it from its own read
            }
            MGLOG_D("MG_Remote doorbell: send failed (errno=%d)", errno);
            return;
        }
    }

    bool SocketDoorbell::Park(std::uint32_t timeoutMs) {
        if (m_fd < 0) {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            int pollTimeout = -1;
            if (timeoutMs != kWaitForever) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                const long long remaining = static_cast<long long>(timeoutMs) - elapsed;
                pollTimeout = remaining <= 0 ? 0 : static_cast<int>(remaining);
            }
            struct pollfd pfd{};
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            const int ready = ::poll(&pfd, 1, pollTimeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue; // a signal is not a wakeup; keep the deadline
                }
                MGLOG_D("MG_Remote doorbell: poll failed (errno=%d)", errno);
                return false;
            }
            if (ready == 0) {
                return false; // timed out
            }
            Reset();
            return true;
        }
    }

    void SocketDoorbell::Reset() {
        if (m_fd < 0) {
            return;
        }
        // Level-triggered to edge-triggered: swallow every queued byte so one
        // stale wakeup cannot make later Parks return without an event.
        std::uint8_t scratch[64];
        for (;;) {
            const ssize_t got = ::recv(m_fd, scratch, sizeof(scratch), MSG_DONTWAIT);
            if (got > 0) {
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
    }

#endif // !_WIN32

} // namespace MobileGL::MG_Remote::Transport
