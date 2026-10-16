// MobileGL - MobileGL/MG_Remote/Transport/LogForward.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Forwards the server's log lines to its client as LogLine control frames, graded by severity,
// so that logging below FATAL never waits on the peer: Offer() copies the line into a queue, and
// one sender thread writes frames in offer order without holding the queue's mutex. DEBUG to
// WARN are lossy, ERROR is paced but not dropped (coalesced only past errorQueueBytes), FATAL
// also waits up to fatalFlushMs, and a LogFlush ack follows every line offered before it.
// Policy, counters and Close(): ARCHITECTURE.md §8.3.1. Only TCP server shapes forward; other
// servers write `<base>.server.log` themselves.

#pragma once

#include "ITransport.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The wire's enum, declared opaquely so this header does not pull the generated schema (and
// flatbuffers) into every includer; the definition is protocol_generated.h's.
namespace MobileGL::Wire {
    enum class LogLevel : std::uint8_t;
}

namespace MobileGL::MG_Remote::Transport {

    // ---- the level mapping, both directions ------------------------------------------------
    //
    // MobileGL's five levels (MOBILEGL_LOG_LEVEL_DEBUG..FATAL, 0..4) and the wire's five
    // (LogLevel::Debug..Fatal, 0..4) are the same scale, and these two say so in code rather than
    // by a cast that would silently carry a sixth value across. Out of range: a MobileGL level
    // below DEBUG is DEBUG and above FATAL is FATAL; a wire level this build does not know (a
    // newer peer's) is read as ERROR - an unknown severity is never demoted into the lossy class.
    ::MobileGL::Wire::LogLevel WireLogLevelFor(int level);
    int LogLevelFromWire(::MobileGL::Wire::LogLevel level);

    struct LogForwardLimits {
        // Bytes of DEBUG/INFO/WARN text the queue may hold before such a line is dropped.
        std::uint64_t lossyQueueBytes = 1ull << 20;
        // Bytes of ERROR/FATAL text the queue may hold before such a line is coalesced.
        std::uint64_t errorQueueBytes = 8ull << 20;
        // The ERROR pace: a token bucket of errorBurstLines refilled at errorLinesPerSecond.
        std::uint32_t errorLinesPerSecond = 256;
        std::uint32_t errorBurstLines = 1024;
        // How long a FATAL Offer waits for its line to be written.
        std::uint32_t fatalFlushMs = 2000;
        // How long Close() lets the queue drain before it abandons the rest.
        std::uint32_t closeDrainMs = 2000;
    };

    struct LogForwardCounters {
        std::uint64_t forwarded = 0;          // lines written to the transport (notices excluded)
        std::uint64_t lossyDropped = 0;       // DEBUG/INFO/WARN lines dropped at the lossy cap
        std::uint64_t lossyDroppedBytes = 0;
        std::uint64_t errorsCoalesced = 0;    // ERROR/FATAL lines past the error cap
        std::uint64_t errorsPaced = 0;        // ERROR lines that waited for the pace (delayed, sent)
        std::uint64_t notices = 0;            // LogForward{Dropped} / {Coalesced} lines queued
        std::uint64_t abandoned = 0;          // queued items never written (close budget, dead peer)
        std::uint64_t fatalFlushTimeouts = 0; // FATAL Offers whose wait ran out
        std::uint64_t maxOfferNs = 0;         // the longest non-FATAL Offer
        bool transportFailed = false;         // a write failed; nothing more was sent
    };

    class LogForwardChannel {
    public:
        using Unblock = void (*)(void* user);

        // `transport` must outlive the channel. `unblock` (may be null) is called by Close() at most
        // once, from Close()'s thread, when the sender is still inside a write after closeDrainMs.
        LogForwardChannel(ITransport& transport, Unblock unblock, void* unblockUser,
                          const LogForwardLimits& limits = LogForwardLimits{});
        ~LogForwardChannel();

        LogForwardChannel(const LogForwardChannel&) = delete;
        LogForwardChannel& operator=(const LogForwardChannel&) = delete;

        // One line, `level` a MOBILEGL_LOG_LEVEL_* value. Never blocks on the peer below FATAL.
        void Offer(int level, const char* line);
        // A whole CtrlEnvelope frame, written after every line offered before it and never dropped
        // or paced (the LogFlush acknowledgement).
        void OfferControlFrame(std::vector<std::uint8_t> frame);
        // Stops taking lines, drains within closeDrainMs, stops the sender. Idempotent; returns the
        // final counters.
        LogForwardCounters Close();
        LogForwardCounters Counters() const;

        // MG_Util::Debug::LogForwarder's shape, `user` being the channel.
        static void ForwardThunk(void* user, int level, const char* line);

    private:
        enum class Kind : std::uint8_t { Line, Notice, Control };
        struct Item {
            Kind kind = Kind::Line;
            int level = 0;
            std::uint64_t seq = 0;
            std::string text;               // Line, Notice
            std::vector<std::uint8_t> frame; // Control
            bool paced = false;             // already counted as paced once
        };

        void Run();
        void QueueGapNoticesLocked(bool lossyGapToo);
        bool IsUrgentLocked() const;
        MobileGLResult Send(const Item& item);

        ITransport& m_transport;
        Unblock m_unblock;
        void* m_unblockUser;
        LogForwardLimits m_limits;

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<Item> m_queue;
        std::uint64_t m_lossyBytes = 0;
        std::uint64_t m_errorBytes = 0;
        std::uint64_t m_nextSeq = 1;
        std::uint64_t m_writtenSeq = 0; // the seq of the last item the sender finished with
        // m_writtenSeq when a FATAL wait last ran out; while it has not moved since, the next FATAL
        // does not wait again (Offer). ~0 = no wait has run out.
        std::uint64_t m_fatalGaveUpAtWritten = ~0ull;
        std::uint64_t m_urgent = 0;     // FATAL lines and control frames queued: do not pace
        // A gap not yet marked in the stream, per class.
        std::uint64_t m_gapLossyLines = 0;
        std::uint64_t m_gapLossyBytes = 0;
        std::string m_gapLossySince; // the time of the gap's first drop, for its notice
        std::uint64_t m_gapErrorLines = 0;
        double m_tokens = 0;
        std::chrono::steady_clock::time_point m_tokensAt{};
        bool m_sending = false;
        bool m_closing = false; // no more Offers; drain unpaced
        bool m_stop = false;    // the sender leaves once the queue is empty
        bool m_closed = false;
        LogForwardCounters m_counters;
        std::thread m_sender;
    };

} // namespace MobileGL::MG_Remote::Transport
