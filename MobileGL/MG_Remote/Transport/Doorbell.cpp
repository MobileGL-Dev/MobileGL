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
        // The death latch is tested under the same mutex Kill sets it under, so
        // a Kill cannot slip between this test and the wait below: it either
        // returns here or wakes the predicate.
        if (m_dead.load(std::memory_order_relaxed)) {
            return false;
        }
        if (m_impl->signals != 0) {
            --m_impl->signals;
            return true;
        }
        if (timeoutMs == 0) {
            return false;
        }
        const auto woken = [this] {
            return m_impl->signals != 0 || m_dead.load(std::memory_order_relaxed);
        };
        if (timeoutMs == kWaitForever) {
            m_impl->cv.wait(lock, woken);
        } else if (!m_impl->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), woken)) {
            return false;
        }
        if (m_dead.load(std::memory_order_relaxed)) {
            // Woken by Kill, not by an event. The caller re-tests its condition
            // regardless (Doorbell::Wait always does) and then sees Dead().
            return false;
        }
        --m_impl->signals;
        return true;
    }

    void CondVarDoorbell::Kill() {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_dead.store(true, std::memory_order_release);
        }
        // notify_all, not notify_one: both a raw Park and a Doorbell::Wait may
        // be parked here, and after this nobody will ring again.
        m_impl->cv.notify_all();
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
            if (written < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                // The peer is gone: it can never ring back either, so latch it
                // here too rather than waiting for a Park to discover it.
                m_dead = true;
                return;
            }
            MGLOG_D("MG_Remote doorbell: send failed (errno=%d)", errno);
            return;
        }
    }

    bool SocketDoorbell::Park(std::uint32_t timeoutMs) {
        if (m_fd < 0 || m_dead) {
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
            // revents has to be inspected, not just `ready > 0`. Once the peer
            // closes its end the descriptor is permanently poll-ready with
            // nothing to read (measured on Linux: revents=POLLIN|POLLHUP,
            // recv()==0), so treating any readiness as a wakeup turns every
            // park on a dead peer into a 100% CPU spin - unbounded, because
            // Doorbell::Wait re-parks until its deadline and kWaitForever has
            // none.
            if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
                MGLOG_D("MG_Remote doorbell: fd %d unusable (revents=0x%X)", m_fd,
                        static_cast<unsigned>(pfd.revents));
                m_dead = true;
                return false;
            }
            if ((pfd.revents & POLLIN) != 0) {
                if (Drain() != 0) {
                    return true; // a real wakeup byte
                }
                if (m_dead) {
                    return false; // EOF, not an event
                }
                // Ready but empty and still alive: someone else drained it.
                // Report the wakeup and let the caller re-test its condition.
                return true;
            }
            if ((pfd.revents & POLLHUP) != 0) {
                m_dead = true;
                return false;
            }
            // Readiness with no bit we requested or recognise: there is
            // nothing to consume and no way to make progress, so refuse to
            // poll this descriptor again.
            MGLOG_D("MG_Remote doorbell: fd %d ready with revents=0x%X", m_fd,
                    static_cast<unsigned>(pfd.revents));
            m_dead = true;
            return false;
        }
    }

    std::uint64_t SocketDoorbell::Drain() {
        // Level-triggered to edge-triggered: swallow every queued byte so one
        // stale wakeup cannot make later Parks return without an event.
        std::uint64_t consumed = 0;
        std::uint8_t scratch[64];
        for (;;) {
            const ssize_t got = ::recv(m_fd, scratch, sizeof(scratch), MSG_DONTWAIT);
            if (got > 0) {
                consumed += static_cast<std::uint64_t>(got);
                continue;
            }
            if (got == 0) {
                // Orderly shutdown on a stream socket: the peer is gone and
                // will never ring again.
                m_dead = true;
                return consumed;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return consumed; // drained
            }
            MGLOG_D("MG_Remote doorbell: recv failed (errno=%d)", errno);
            m_dead = true;
            return consumed;
        }
    }

    void SocketDoorbell::Reset() {
        if (m_fd < 0 || m_dead) {
            return;
        }
        (void)Drain();
    }

#endif // !_WIN32

} // namespace MobileGL::MG_Remote::Transport
