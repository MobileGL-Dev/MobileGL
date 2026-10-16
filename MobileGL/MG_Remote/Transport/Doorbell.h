// MobileGL - MobileGL/MG_Remote/Transport/Doorbell.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The bidirectional doorbell: a waiter spins, sets its park flag, then blocks, and the other side
// rings only when that flag is set (client -> server on consumerParked; server -> client on
// producerParked, after any watermark advance). Without the server -> client direction every client
// wait would be a long cross-process spin on one shared cache line. No platform wakeup primitive
// (futex, eventfd, named event): CondVarDoorbell in process, SocketDoorbell across processes.
// Two seq_cst fences close the lost-wakeup window: Doorbell::Wait's (after setting the flag, before
// re-testing) and NotifyIfParked's (after publishing, before reading the flag). Neither may be
// removed, and NotifyIfParked must follow the publish; why: ARCHITECTURE.md §11.3.1.

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

    // The spin budget is counted in iterations, not clock reads: CalibrateSpinItersPerUs measures
    // once per process how many acquire-load + CpuRelax() iterations fit in a microsecond, and Wait
    // spins itersPerUs * spinUs without reading a clock (a steady_clock read can be a syscall, and
    // Wait runs hundreds of times per frame). Only the park phase computes a deadline. The budget
    // is a hint, not a correctness bound: `ready()` is re-tested every iteration; a DVFS drop or a
    // move to a little core stretches the spin by the frequency ratio (roughly 3-4x); the probe
    // omits `ready()`, so real spins end early; the clamps in CalibrateSpinItersPerUs bound bad
    // readings.
    namespace Detail {
        // 0 until the first Wait calibrates it. Plain relaxed atomic rather than a
        // function-local static: a static's guard variable is a load and a branch on
        // EVERY Wait, and the whole point of this block is that the common path is a
        // load and a predicted branch.
        extern std::atomic<std::uint32_t> g_spinItersPerUs;
        // Measures, clamps, stores and returns. Two threads that race here both
        // store a legitimate reading and neither is wrong; there is nothing to
        // serialise, so nothing does.
        std::uint32_t CalibrateSpinItersPerUs();
    } // namespace Detail

    inline std::uint32_t SpinItersPerUs() {
        const std::uint32_t cached = Detail::g_spinItersPerUs.load(std::memory_order_relaxed);
        return cached != 0 ? cached : Detail::CalibrateSpinItersPerUs();
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

        // True once the wakeup channel is permanently unusable: the peer closed
        // its end of the socket, or the inproc channel was shut down. A dead
        // doorbell can never deliver another wakeup, and Wait must stop
        // re-parking on it - for the socket because its descriptor is
        // permanently poll-ready and a waiter with no deadline would burn a
        // big core at full clock, for the condvar because Park would otherwise
        // block forever and Shutdown could never join the waiter. Every
        // implementation has a death state; the base default is only for a
        // bell that cannot die.
        virtual bool Dead() const { return false; }

        // P6 `dl`: DEAD AND DEAD-BECAUSE-THE-PEER-DIED ARE NOT THE SAME FACT, and only the
        // second one is a device loss. Dead() is also true after an ORDERLY teardown -
        // CondVarDoorbell::Kill() is how Stop() wakes a parked applier, and the barrier reports
        // SessionWait::ShutDown for that too. Latching device-lost from Dead() would therefore
        // arm it on every clean exit.
        //
        // This says the peer HUNG UP: a descriptor whose far end only the peer held reported
        // hangup. Nothing this side did can produce it. False for every doorbell that has no
        // peer process, which is what makes the whole latch a no-op under inproc and monolith.
        virtual bool PeerHungUp() const { return false; }

        // How many Wait() calls on this bell exhausted their spin budget and really blocked,
        // counted once per Wait (re-parks and zero-length polls are not counted), so parks <= waits
        // for one waiter. The count is per bell, and a bell is shared by every waiter on its
        // endpoint (the client's self bell also carries PipeWireCodec's SEG_STAGE retirement wait);
        // a caller that needs parks as a subset of its own waits passes `parkTally` to Wait
        // instead. Monotone and relaxed: a reader on another thread may see a value one behind.
        std::uint64_t ParkEntries() const { return m_parkEntries.load(std::memory_order_relaxed); }

        // Spin `spinUs`, then park until `ready()` or the deadline.
        // `parked` is the RingControl flag the peer tests before ringing.
        //
        // `parkTally`, when given, is incremented alongside this bell's own park
        // counter - once, the first time THIS call really blocks. It is how a
        // caller gets parks that are a subset of its OWN waits off a bell it
        // shares with another subsystem; see ParkEntries() above.
        template <class Ready>
        bool Wait(std::atomic<std::uint32_t>& parked, Ready&& ready, std::uint32_t spinUs, std::uint32_t timeoutMs,
                  std::atomic<std::uint64_t>* parkTally = nullptr) {
            if (ready()) {
                return true;
            }

            // The spin phase reads no clock (see SpinItersPerUs). spinUs == 0 means no spin at all
            // and skips the calibration probe: PipeWireCodec's SEG_STAGE retirement wait passes 0
            // because it wants the park, and MOBILEGL_IPC_SPIN_US=0 is a no-spin control.
            const std::uint64_t budget =
                spinUs == 0 ? 0ull : static_cast<std::uint64_t>(SpinItersPerUs()) * static_cast<std::uint64_t>(spinUs);
            for (std::uint64_t spun = 0; spun < budget; ++spun) {
                if (ready()) {
                    return true;
                }
                CpuRelax();
            }

            // THE PARK PHASE, AND THE ONLY PLACE A DEADLINE IS COMPUTED. Reading
            // the clock here is free in the sense that matters: this side is about
            // to block on a condition variable or a poll(), which costs orders of
            // magnitude more than the syscall that sizes it. kWaitForever reads no
            // clock even here.
            //
            // THE DEADLINE STARTS NOW, NOT WHEN Wait WAS ENTERED. The spin the
            // caller asked for is no longer charged against its timeout, so the
            // total is bounded by `timeoutMs + the spin budget` rather than by
            // `timeoutMs`. spinUs is 50 by default and the timeouts that reach
            // here are milliseconds (the verb barrier's budget, the drain's bound),
            // so the drift is under a part in a thousand - and erring LONG is the
            // safe direction for a deadline whose expiry is reported as a timeout.
            const auto deadline = timeoutMs == kWaitForever
                                      ? std::chrono::steady_clock::time_point::max()
                                      : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

            // ONE PARK PER Wait is what the ledger counts, and this loop may Park
            // many times for one Wait; see the counting site below.
            bool countedPark = false;
            for (;;) {
                // Announce, FENCE, then re-test. The fence is the mechanism -
                // see the file header - so setting the flag itself is relaxed.
                // Release, so a notifier that acquires this 1 also sees what the waiter wrote
                // before parking (RingControl::producerWakeSeq). The fence below is still what
                // closes the lost-wakeup window.
                parked.store(1, std::memory_order_release);
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
                // Counted here, where a waiter really blocks, and unconditionally (one relaxed
                // add before a condvar wait or poll), so a test can read it without
                // MOBILEGL_PIPE_STATS. Only the first blocking Park of a Wait counts: a
                // CondVarDoorbell can return at once on a remembered Notify and re-park, and
                // chunkMs == 0 is a non-blocking poll at the end of a finite timeout. That keeps
                // parks a subset of waits.
                if (chunkMs != 0 && !countedPark) {
                    countedPark = true;
                    m_parkEntries.fetch_add(1, std::memory_order_relaxed);
                    if (parkTally != nullptr) {
                        parkTally->fetch_add(1, std::memory_order_relaxed);
                    }
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

    private:
        std::atomic<std::uint64_t> m_parkEntries{0};
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
        bool Dead() const override { return m_dead.load(std::memory_order_acquire); }

        // Hangs the bell up for good: every parked waiter returns false now and
        // every later Park returns false at once. The inproc twin of the socket
        // peer closing its end (SocketDoorbell latches m_dead on EOF), and what
        // InProcessChannel::Close rings instead of Notify. A Notify is consumed
        // by ONE Park; Doorbell::Wait then re-tests its condition, finds
        // nothing published, finds the bell alive, and with kWaitForever parks
        // again - so a Shutdown that only rang could never join a server thread
        // sitting in the design's own steady state (spun, set consumerParked,
        // blocked). Irreversible by design, like the socket's.
        void Kill();

    private:
        struct Impl;
        Impl* m_impl;
        std::atomic<bool> m_dead{false};
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

        // P6: PARK AND NOTIFY ON DIFFERENT DESCRIPTORS.
        //
        // The single-fd form above is the cross-process one: send() on this end
        // is delivered to the PEER's end, so the peer's own SocketDoorbell is
        // what receives it. That is correct, and it is also why the single-fd
        // form cannot wake ITSELF - and the server needs exactly that, because
        // its control pump posts into the apply thread's mailbox and must ring
        // the bell that thread is parked on. Under inproc CondVarDoorbell has no
        // such split: notify and wait are the same object.
        //
        // So: `parkFd` is polled, `notifyFd` is written. For a socketpair where
        // this side holds both ends they are [0] and [1], and a peer that must
        // also be able to ring it gets a DUP of [1]. `parkFd` may be -1 for a
        // bell this side only ever RINGS - Park then returns immediately, which
        // is the honest answer for an object that was never a waiter.
        SocketDoorbell(int parkFd, int notifyFd, std::uint8_t code, bool ownsFds);
        ~SocketDoorbell() override;

        void Notify() override;
        bool Park(std::uint32_t timeoutMs) override;
        void Reset() override;
        bool Dead() const override { return m_dead.load(std::memory_order_acquire); }
        bool PeerHungUp() const override { return m_peerHungUp.load(std::memory_order_acquire); }

        int Fd() const { return m_fd; }

        // The death witness: a descriptor whose far end only the peer holds (in practice the
        // control socket). The park descriptor cannot report the peer's death when this side also
        // holds the notify end of the same socketpair (the client rings itself through
        // clientBell[1]), so without a witness a dead server surfaces only as a barrier timeout.
        // Not owned and never read: Park polls it for hangup only, so unread control replies
        // neither wake the bell nor get taken from SocketTransport's reassembler. A hangup sets
        // PeerHungUp(). -1 disables it.
        void SetDeathWitness(int fd) { m_witnessFd = fd; }

    private:
        // Consumes every queued wakeup byte and returns how many. Latches
        // m_dead on EOF: recv returning 0 on a stream socket is the peer's
        // hangup, not a wakeup, and the descriptor stays poll-ready forever
        // afterwards.
        std::uint64_t Drain();

        int m_fd;             // polled
        int m_notifyFd;       // written; equals m_fd in the single-fd (cross-process) form
        int m_witnessFd = -1; // polled for hangup only; NOT owned, NEVER read
        // Set ONLY by the witness branch of Park, never by Kill or by our own EOF. See
        // Doorbell::PeerHungUp for why the distinction is the whole design.
        std::atomic<bool> m_peerHungUp{false};
        std::uint8_t m_code;
        bool m_ownsFd;
        // ATOMIC BECAUSE THE LATCH READS IT ACROSS THREADS. Park() runs on whichever thread is
        // waiting; the device-lost latch is consulted from the GL thread and from
        // glGetGraphicsResetStatus. It was a plain bool while Dead() had no cross-thread reader.
        std::atomic<bool> m_dead{false};
    };
#endif

} // namespace MobileGL::MG_Remote::Transport
