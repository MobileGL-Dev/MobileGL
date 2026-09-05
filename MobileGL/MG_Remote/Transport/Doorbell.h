// MobileGL - MobileGL/MG_Remote/Transport/Doorbell.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The bidirectional doorbell: spin briefly, then park.
//
// Both directions exist, and that is the point (inherited design, earlier plan
// section 6.2a):
//   - client -> server: the consumer spins, sets consumerParked, then blocks;
//     the producer rings only when consumerParked is set.
//   - server -> client: the client spins MOBILEGL_IPC_SPIN_US (default 50us),
//     sets producerParked, then blocks; the server rings after advancing any
//     watermark, only when producerParked is set.
// Without the second direction every client wait - present credit, a blocking
// kNeedsAck request, a full ring - degenerates into a cross-process spin on
// one shared cache line: up to a whole frame of a big core at full clock on a
// phone, fighting the GPU and the game's JVM for it. MobileGL has no affinity
// control anywhere in the tree, so it cannot even be pushed to a little core.
//
// Two implementations, no platform-specific wakeup primitive (no futex, no
// eventfd, no named event):
//   - CondVarDoorbell for `inproc` (one process, two threads),
//   - SocketDoorbell for `spawn` (one byte on a socket; POSIX only).
//
// The lost-wakeup window is closed by ordering, not by luck: the waiter stores
// its park flag and THEN re-tests the condition, while the notifier publishes
// the watermark and THEN tests the park flag. Both use seq_cst on those two
// accesses, so at least one of the two sees the other.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace MobileGL::MG_Remote::Transport {

    // MOBILEGL_IPC_SPIN_US default.
    inline constexpr std::uint32_t kDefaultSpinUs = 50;

    // Park with no deadline.
    inline constexpr std::uint32_t kWaitForever = 0xFFFFFFFFu;

    // Wire codes, so a shared socket can carry both directions distinguishably.
    inline constexpr std::uint8_t kDoorbellRingAdvanced = 0x01;      // client -> server
    inline constexpr std::uint8_t kDoorbellWatermarkAdvanced = 0x02; // server -> client

    inline void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    class Doorbell {
    public:
        virtual ~Doorbell() = default;

        Doorbell(const Doorbell&) = delete;
        Doorbell& operator=(const Doorbell&) = delete;

        // Wakes a parked peer. Cheap and idempotent: a wakeup that arrives when
        // nobody is parked is remembered, so the next Park returns immediately
        // rather than sleeping through an event that already happened.
        virtual void Notify() = 0;

        // Blocks until notified or the deadline passes. Returns true when a
        // wakeup was consumed. timeoutMs == 0 polls; kWaitForever never times
        // out.
        virtual bool Park(std::uint32_t timeoutMs) = 0;

        // Drops pending wakeups. Used when a waiter gives up, so a stale byte
        // does not make the next Park return spuriously forever.
        virtual void Reset() = 0;

        // Spin `spinUs`, then park until `ready()` or the deadline.
        // `parked` is the RingControl flag the peer tests before ringing.
        template <class Ready>
        bool Wait(std::atomic<std::uint32_t>& parked, Ready&& ready, std::uint32_t spinUs,
                  std::uint32_t timeoutMs) {
            if (ready()) {
                return true;
            }
            const auto start = std::chrono::steady_clock::now();
            const auto deadline = timeoutMs == kWaitForever
                                      ? std::chrono::steady_clock::time_point::max()
                                      : start + std::chrono::milliseconds(timeoutMs);

            const auto spinEnd = start + std::chrono::microseconds(spinUs);
            while (std::chrono::steady_clock::now() < spinEnd) {
                if (ready()) {
                    return true;
                }
                CpuRelax();
            }

            for (;;) {
                // Announce, THEN re-test: the notifier publishes and then reads
                // this flag, so one of the two orderings always sees the other.
                parked.store(1, std::memory_order_seq_cst);
                if (ready()) {
                    parked.store(0, std::memory_order_seq_cst);
                    return true;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    parked.store(0, std::memory_order_seq_cst);
                    return ready();
                }
                std::uint32_t chunkMs = kWaitForever;
                if (timeoutMs != kWaitForever) {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                    chunkMs = remaining <= 0 ? 0 : static_cast<std::uint32_t>(remaining);
                }
                Park(chunkMs);
                parked.store(0, std::memory_order_seq_cst);
                if (ready()) {
                    return true;
                }
                if (timeoutMs != kWaitForever && std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
            }
        }

    protected:
        Doorbell() = default;
    };

    // Rings `bell` only when the peer said it is parked. The seq_cst load pairs
    // with the waiter's seq_cst store of the same flag.
    inline void NotifyIfParked(Doorbell& bell, std::atomic<std::uint32_t>& parked) {
        if (parked.load(std::memory_order_seq_cst) != 0) {
            bell.Notify();
        }
    }

    // `inproc`: one process, two threads.
    class CondVarDoorbell final : public Doorbell {
    public:
        CondVarDoorbell();
        ~CondVarDoorbell() override;

        void Notify() override;
        bool Park(std::uint32_t timeoutMs) override;
        void Reset() override;

    private:
        struct Impl;
        Impl* m_impl;
    };

#if !defined(_WIN32)
    // `spawn`: one byte on a socket (one direction of a socketpair, or the aux
    // socket). POSIX only; the Windows path will use an overlapped named pipe
    // and is not part of this skeleton.
    class SocketDoorbell final : public Doorbell {
    public:
        // `fd` must be a socket or pipe end. When `ownsFd` the descriptor is
        // closed with this object. `code` is the byte written by Notify.
        SocketDoorbell(int fd, std::uint8_t code, bool ownsFd);
        ~SocketDoorbell() override;

        void Notify() override;
        bool Park(std::uint32_t timeoutMs) override;
        void Reset() override;

        int Fd() const { return m_fd; }

    private:
        int m_fd;
        std::uint8_t m_code;
        bool m_ownsFd;
    };
#endif

} // namespace MobileGL::MG_Remote::Transport
