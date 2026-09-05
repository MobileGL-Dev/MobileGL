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
// The lost-wakeup window is closed by two seq_cst FENCES, not by the ordering
// of the park flag's own load and store:
//   - the waiter sets the flag, executes std::atomic_thread_fence(seq_cst),
//     and THEN re-tests the condition (Doorbell::Wait);
//   - the notifier publishes its watermark, executes the same fence, and THEN
//     reads the flag (NotifyIfParked).
// Both fences sit in the single seq_cst total order, so one precedes the
// other, and [atomics.order] then forces at least one side to observe the
// other's store. The flag's own accesses may be relaxed: they are not what
// closes the window.
//
// A seq_cst store paired with a seq_cst load would NOT be enough, which is
// why the fences are here and why neither may be removed. That Dekker
// argument needs all FOUR accesses in the total order, and the other two are
// not: the watermark publish is a release store (RingProducer::Publish) and
// the condition re-test is an acquire load. On x86 the gap is concrete rather
// than theoretical - a release store is a plain MOV that can still sit in the
// store buffer while the load of the park flag, also a plain MOV, reads 0, so
// the notifier skips the ring and the waiter parks on a stale watermark
// forever. (ARMv8 survives it only because STLR->LDAR is RCsc, i.e. by luck.)
//
// The other half of the contract is ordering between the caller and the
// fence: NotifyIfParked must be called AFTER the watermark is published. A
// fence only orders what precedes it.

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

        // True once the wakeup channel is permanently unusable, e.g. the peer
        // closed its end of the socket. A dead doorbell can never deliver
        // another wakeup AND its descriptor is permanently poll-ready, so Wait
        // must stop re-parking on it: otherwise a waiter with no deadline
        // burns a big core at full clock, which is the exact pathology the
        // bidirectional doorbell exists to prevent.
        virtual bool Dead() const { return false; }

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
                // Announce, FENCE, then re-test. The fence is the mechanism -
                // see the file header - so setting the flag itself is relaxed.
                parked.store(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (ready()) {
                    parked.store(0, std::memory_order_relaxed);
                    return true;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    parked.store(0, std::memory_order_relaxed);
                    return ready();
                }
                std::uint32_t chunkMs = kWaitForever;
                if (timeoutMs != kWaitForever) {
                    const auto remaining =
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                    chunkMs = remaining <= 0 ? 0 : static_cast<std::uint32_t>(remaining);
                }
                Park(chunkMs);
                // Clearing is relaxed on purpose: a notifier that reads a
                // stale 1 only rings a bell nobody is waiting on, which the
                // doorbell remembers and the next Park consumes. The dangerous
                // direction - a notifier reading 0 while the waiter is really
                // parked - is the one the fence above rules out.
                parked.store(0, std::memory_order_relaxed);
                if (ready()) {
                    return true;
                }
                if (Dead()) {
                    // Nothing can ring this bell again and parking on it no
                    // longer blocks, so looping here would spin at full clock
                    // for as long as the caller is willing to wait - which,
                    // with kWaitForever, is forever.
                    return false;
                }
                if (timeoutMs != kWaitForever && std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
            }
        }

    protected:
        Doorbell() = default;
    };

    // Rings `bell` only when the peer said it is parked.
    //
    // PRECONDITION: whatever the waiter's condition reads - the ring head, a
    // sequence watermark, a queue push - is ALREADY published when this is
    // called. The fence only orders what precedes it, so ringing before
    // publishing reopens the window this closes. The fence pairs with the one
    // in Doorbell::Wait; see the file header for why the flag's own memory
    // order is not what makes this sound.
    inline void NotifyIfParked(Doorbell& bell, std::atomic<std::uint32_t>& parked) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (parked.load(std::memory_order_relaxed) != 0) {
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
        // `fd` must be one end of an AF_UNIX socket pair, not a pipe: Notify
        // uses send() with MSG_DONTWAIT|MSG_NOSIGNAL and Park uses
        // poll()+recv(), which a pipe end refuses with ENOTSOCK. Prefer
        // SOCK_STREAM for the spawn transport - measured on Linux, a closed
        // peer makes a stream end report POLLIN|POLLHUP with recv()==0, which
        // is how death is detected, while a SOCK_DGRAM end reports no
        // readiness at all and a waiter with no deadline would simply hang.
        // When `ownsFd` the descriptor is closed with this object. `code` is
        // the byte written by Notify.
        SocketDoorbell(int fd, std::uint8_t code, bool ownsFd);
        ~SocketDoorbell() override;

        void Notify() override;
        bool Park(std::uint32_t timeoutMs) override;
        void Reset() override;
        bool Dead() const override { return m_dead; }

        int Fd() const { return m_fd; }

    private:
        // Consumes every queued wakeup byte and returns how many. Latches
        // m_dead on EOF: recv returning 0 on a stream socket is the peer's
        // hangup, not a wakeup, and the descriptor stays poll-ready forever
        // afterwards.
        std::uint64_t Drain();

        int m_fd;
        std::uint8_t m_code;
        bool m_ownsFd;
        bool m_dead = false;
    };
#endif

} // namespace MobileGL::MG_Remote::Transport
