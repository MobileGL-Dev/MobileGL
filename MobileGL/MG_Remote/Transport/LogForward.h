// MobileGL - MobileGL/MG_Remote/Transport/LogForward.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P9 W3. THE SERVER'S LOG LINES ON THE CONTROL PLANE, GRADED, AND NEVER A REASON TO STOP APPLYING.
//
// On TCP the server forwards every line it logs to its client as a LogLine control frame, and the
// client files it under `<base>.server.log` (ControlInbox.cpp). Until this file the forward was a
// blocking SocketTransport::SendFrame made by the LOGGING THREAD, inside the logger: a client that
// stopped reading its control connection - a GL thread parked in a debugger, a ControlInbox whose
// bounded queue filled, a peer that simply stopped - filled the socket, and the next MGLOG on the
// apply thread never returned. Every record behind it waited; a forfeit that would have ended the
// session by name (ServerSession::ForfeitReverseChannel) was itself a log line and never finished
// being said. Design/04 §8.1 had written the policy down from the start (<= WARN lossy, >= ERROR
// lossless and rate limited), and LogLine.level has been on the wire for it since P6; nothing
// used either, and every line went out as Info.
//
// THE SHAPE. A LogForwardChannel owns a queue and ONE sender thread; the forwarder the logger
// calls (Offer) only enqueues, so the logging thread never touches the socket. The sender is the
// only thread that writes log frames, and it writes them in the order they were offered.
//
// THE POLICY, per severity (MOBILEGL_LOG_LEVEL_*):
//   * DEBUG / INFO / WARN are LOSSY. A line that would take the queue's lossy bytes past
//     LogForwardLimits::lossyQueueBytes is dropped and COUNTED. The drop is not silent to the
//     client either: when the gap closes - the next line at or below WARN that fits, a LogFlush
//     ack, or Close - the channel puts a WARN line of its own in the stream, `LogForward{Dropped} -
//     N line(s) at or below WARN (B bytes) were not forwarded, from <time> until here`, so the gap
//     in `<base>.server.log` is marked and counted. (ERROR lines that got through in the middle of
//     the gap sit before that line; the time says where the gap began.)
//   * ERROR is LOSSLESS, and PACED rather than dropped: at most errorLinesPerSecond leave per
//     second after a burst of errorBurstLines, and a line over that budget WAITS in the queue, in
//     order (it is delayed, never discarded). Memory still has to be bounded, so the error bytes
//     queued are capped too (errorQueueBytes, eight times the lossy cap): a line past it - a peer
//     that has not read for that long, or a flood well past the pace for that long - is not
//     queued, and is replaced in the stream by one ERROR line `LogForward{Coalesced} - N ERROR
//     line(s) were not forwarded here` (queued before the next item that does get in), never by
//     nothing. The server's own log file (written before any forward, Log.cpp) has every line
//     either way.
//   * FATAL is ERROR plus a wait: the logging thread is about to abort, and a line still in the
//     queue when it does is a line the client never gets - the one it most needs. So a FATAL
//     Offer waits until the sender has written everything up to and including it, for at most
//     fatalFlushMs, and then returns whether or not the peer took it - and once such a wait has run
//     out, later FATAL lines do not wait again until the sender has written something (MGLOG_F also
//     marks survivable conditions, several in a row). (Log.cpp hands a FATAL line over outside the
//     log mutex, so that wait holds nobody else's line.)
//   * A LogFlush acknowledgement (OfferControlFrame) is queued BEHIND every line offered before
//     it, which is what makes MGPipeSyncPeerLog's promise ("every line before the flush is in the
//     file when the ack arrives") still true now that a line is not on the wire when MGLOG returns.
//     While one is queued the sender does not pace: the client is waiting for it.
//
// THE BOUND. An Offer below FATAL takes the channel's mutex, copies the line and returns; the
// sender never holds that mutex across a socket write. So the time a logging thread spends
// forwarding is a lock and a copy, whatever the peer does - LogForwardCounters::maxOfferNs is that
// time measured, and the F2 fault injection (EventForfeitPeerTest) asserts on it.
//
// CLOSE. Close() stops taking lines, gives the queue closeDrainMs to go out UNPACED, and then stops
// the sender. A sender still inside a write when that budget runs out is blocked on a peer that is
// not reading; Close() then calls the `unblock` hook the owner supplied (ServerMain: shutdown(2) of
// the control socket - the session is over by then) so the thread can be joined rather than
// leaked. What was still queued is counted as abandoned.
//
// SCOPE. Only the TCP server shapes forward (ServerMain.cpp, MOBILEGL_IPC_LOG_FORWARD): a unix spawn
// server and an inproc apply thread write `<base>.server.log` themselves.

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
