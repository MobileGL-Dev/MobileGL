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

    // -----------------------------------------------------------------------
    // THE SPIN BUDGET IS COUNTED IN ITERATIONS, NOT IN CLOCK READS.
    //
    // WHY IT CHANGED (P5d round 3, package T item 3). Wait() used to spin until
    // steady_clock passed `start + spinUs`, reading the clock once per 64 spins.
    // On the workload device a steady_clock read is a real syscall (no vDSO), and
    // simpleperf at head 56a77348 measured the GL thread paying
    // steady_clock::now() 2.34 self / 5.91 inclusive plus __kernel_clock_gettime
    // 2.37 and clock_gettime 1.94 - about 6.6% of the thread spent asking what
    // time it is, at 900+ waits per frame, for a duration that is a HINT. The
    // common case (WaitForApplied, server not yet there) paid at least two of
    // those reads per wait even when the spin succeeded on its second iteration.
    //
    // WHAT REPLACES IT. Once per process the loop measures itself: how many
    // `load(acquire)` + CpuRelax() iterations fit in a microsecond. Wait() then
    // spins `itersPerUs * spinUs` iterations and reads no clock at all; only the
    // park phase - which is about to block, so a syscall there is noise - computes
    // a deadline.
    //
    // THE OVERSHOOT BOUND, STATED HONESTLY. The old loop overshot by at most one
    // batch of 64 yields. This one overshoots by whatever the calibration was
    // wrong by, and the calibration is a single reading taken on one core at one
    // frequency:
    //   - a later DVFS drop or a migration to a little core stretches the spin by
    //     the frequency ratio (on the workload device's big/little spread that is
    //     bounded by roughly 3-4x, so a 50 us budget can become ~200 us);
    //   - a preemption inside the spin adds one scheduler slice, exactly as before;
    //   - the calibration probe does not run the caller's `ready()`, which is one
    //     or two acquire loads, so a real iteration is dearer than a probed one and
    //     the spin ends EARLY rather than late by that factor;
    //   - the clamps below cap the pathological directions: a probe that was
    //     preempted cannot produce a budget smaller than kMinSpinItersPerUs, and a
    //     probe that saw a broken clock cannot produce one larger than
    //     kMaxSpinItersPerUs.
    // None of it is a correctness bound: `ready()` is re-tested every iteration
    // and the deadline, when there is one, is still measured against the real
    // clock in the park phase. The cost of being wrong is CPU burnt before a park
    // (too long) or a park that a few more spins would have avoided (too short).
    // -----------------------------------------------------------------------
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

        // How many Wait() calls on this bell exhausted their spin budget and
        // really blocked. THE WAIT LEDGER'S RAW READING: the pair (waits, parks)
        // is what says whether the spin budget is sized for the workload, and it
        // is the one number a wait counter cannot give - a wait that spun and a
        // wait that blocked cost three orders of magnitude apart.
        //
        // ONCE PER Wait, NOT ONCE PER Park: a wait that blocks, wakes on a
        // remembered Notify, re-tests false and blocks again is ONE wait that
        // paid the park, and a `chunkMs == 0` Park is a poll that does not block
        // at all. Both are counted out at the site below, so parks <= waits holds
        // for a single waiter.
        //
        // PER BELL, THOUGH, NOT PER WAIT PATH. A bell belongs to an ENDPOINT and
        // every waiter on that endpoint shares it: the client's self bell carries
        // SessionProducer's barrier waits AND the encoder's SEG_STAGE retirement
        // wait (PipeWireCodec's m_stageRetirementBell, which is the same object).
        // A ledger that prints parks as the subset of ITS OWN waits must pass a
        // `parkTally` to Wait rather than read this; this counter answers "parks
        // on this bell", which is what a test on a private bell wants.
        //
        // Read from any thread; the counter is monotone and relaxed, so a reader
        // may see a value one behind, which is exactly what a per-frame gauge
        // wants.
        std::uint64_t ParkEntries() const { return m_parkEntries.load(std::memory_order_relaxed); }

        // Spin `spinUs`, then park until `ready()` or the deadline.
        // `parked` is the RingControl flag the peer tests before ringing.
        //
        // `parkTally`, when given, is incremented alongside this bell's own park
        // counter - once, the first time THIS call really blocks. It is how a
        // caller gets parks that are a subset of its OWN waits off a bell it
        // shares with another subsystem; see ParkEntries() above.
        template <class Ready>
        bool Wait(std::atomic<std::uint32_t>& parked, Ready&& ready, std::uint32_t spinUs,
                  std::uint32_t timeoutMs, std::atomic<std::uint64_t>* parkTally = nullptr) {
            if (ready()) {
                return true;
            }

            // THE SPIN PHASE READS NO CLOCK AT ALL. It used to read one per 64
            // iterations plus one before the loop - see the SpinItersPerUs block
            // above for the measurement that made even that too dear, and for the
            // overshoot bound the iteration budget buys instead. The duration is
            // still a hint; what changed is that asking for it is free.
            //
            // spinUs == 0 NOW MEANS ZERO ITERATIONS, AND THAT IS A DELIBERATE CHANGE
            // rather than a side effect. The old batched loop tested
            // `(++spinBatch & 63) != 0 || now < spinEnd`, so a caller that asked for
            // no spin still ran 63 iterations before the first clock read could end
            // the loop. That was an artifact of the batch size, never a promise, and
            // multiplying a budget the caller deliberately set to zero would be the
            // wrong way to keep it. Two callers pass 0: PipeWireCodec's SEG_STAGE
            // retirement wait (rare, already slow, and it wants the park) and any
            // lane that sets MOBILEGL_IPC_SPIN_US=0 as a no-spin control, which now
            // really gets no spin - which is the point of such a control. The
            // short-circuit also keeps a zero-spin caller from paying the one-time
            // calibration probe for a reading it will never multiply by.
            const std::uint64_t budget =
                spinUs == 0 ? 0ull
                            : static_cast<std::uint64_t>(SpinItersPerUs()) *
                                  static_cast<std::uint64_t>(spinUs);
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
            // here are milliseconds (the verb barrier's 30 s, the drain's bound),
            // so the drift is under a part in a thousand - and erring LONG is the
            // safe direction for a deadline whose expiry is reported as a timeout.
            const auto deadline = timeoutMs == kWaitForever
                                      ? std::chrono::steady_clock::time_point::max()
                                      : std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(timeoutMs);

            // ONE PARK PER Wait is what the ledger counts, and this loop may Park
            // many times for one Wait; see the counting site below.
            bool countedPark = false;
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
                // COUNTED HERE, at the one place a waiter stops spinning and
                // really blocks (P5d round 3, package T item 4). Unconditional
                // rather than behind the stats latch: it is one relaxed add on a
                // path that is about to enter a condition variable or a poll(),
                // and a counter that only exists when MOBILEGL_PIPE_STATS is set
                // could not be read by a test that wants to prove the spin budget
                // avoided the park.
                //
                // TWO THINGS ARE COUNTED OUT, because this number is published as
                // the SUBSET of the waits that blocked, and a subset that can
                // exceed its own denominator is worse than no number at all:
                //   - RE-PARKS. One Wait can traverse this loop many times: a
                //     CondVarDoorbell remembers a Notify delivered while nobody
                //     was waiting, so Park returns at once, `ready()` is still
                //     false (the peer had already drained what it published) and
                //     the wait parks again. That is one wait that paid a park, not
                //     two, so only the first is counted.
                //   - POLLS. `chunkMs == 0` is the last sub-millisecond of a
                //     finite timeout rounding down; Park(0) does not block, and
                //     counting it would charge a park per iteration of a tight
                //     re-test loop that never left the CPU.
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
