// MobileGL - MobileGL/MG_Remote/Transport/LogForward.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P9 W3: the graded, non-blocking log forward. The policy and its reasons are in LogForward.h.

#include "LogForward.h"

#include "../Protocol/generated/protocol_generated.h"

// The umbrella, for MOBILEGL_LOG_LEVEL_* and GetCurrentTime - a .cpp may pull it (WireLog.h).
#include <MG_Util/Debug/Log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace MobileGL::MG_Remote::Transport {

    namespace Wire = ::MobileGL::Wire;

    // THE TWO SCALES ARE ONE SCALE, and these are what would notice if either moved: a sixth
    // MobileGL level, a reordered wire enum, or Log.h's 2026-08-13 inversion coming back.
    static_assert(MOBILEGL_LOG_LEVEL_DEBUG == 0 && MOBILEGL_LOG_LEVEL_INFO == 1 && MOBILEGL_LOG_LEVEL_WARN == 2 &&
                      MOBILEGL_LOG_LEVEL_ERROR == 3 && MOBILEGL_LOG_LEVEL_FATAL == 4,
                  "MobileGL's log levels moved; re-read WireLogLevelFor / LogLevelFromWire");
    static_assert(static_cast<int>(Wire::LogLevel::Debug) == 0 && static_cast<int>(Wire::LogLevel::Info) == 1 &&
                      static_cast<int>(Wire::LogLevel::Warn) == 2 && static_cast<int>(Wire::LogLevel::Error) == 3 &&
                      static_cast<int>(Wire::LogLevel::Fatal) == 4,
                  "protocol.fbs's LogLevel moved; re-read WireLogLevelFor / LogLevelFromWire");

    Wire::LogLevel WireLogLevelFor(int level) {
        if (level <= MOBILEGL_LOG_LEVEL_DEBUG) return Wire::LogLevel::Debug;
        switch (level) {
        case MOBILEGL_LOG_LEVEL_INFO: return Wire::LogLevel::Info;
        case MOBILEGL_LOG_LEVEL_WARN: return Wire::LogLevel::Warn;
        case MOBILEGL_LOG_LEVEL_ERROR: return Wire::LogLevel::Error;
        default: return Wire::LogLevel::Fatal;
        }
    }

    int LogLevelFromWire(Wire::LogLevel level) {
        switch (level) {
        case Wire::LogLevel::Debug: return MOBILEGL_LOG_LEVEL_DEBUG;
        case Wire::LogLevel::Info: return MOBILEGL_LOG_LEVEL_INFO;
        case Wire::LogLevel::Warn: return MOBILEGL_LOG_LEVEL_WARN;
        case Wire::LogLevel::Error: return MOBILEGL_LOG_LEVEL_ERROR;
        case Wire::LogLevel::Fatal: return MOBILEGL_LOG_LEVEL_FATAL;
        }
        // A value this build's schema does not name: a newer peer's severity. Never demoted into
        // the lossy class - an unknown severity is filed as an error.
        return MOBILEGL_LOG_LEVEL_ERROR;
    }

    namespace {
        // The header a notice line carries: the shape of Log()'s own ("[time] [OS thread/LEVEL]: "),
        // so it reads like any other forwarded line and starts with '[' (WritePeerLog writes it as it
        // came). The "OS" slot names the layer instead: GetOSName is not callable outside Log.cpp.
        std::string NoticeHeader(int level) {
            return "[" + MG_Util::Debug::GetCurrentTime() + "] [MG_Remote mgl-log-fwd/" +
                   MG_Util::Debug::LogLevelTag(level) + "]: ";
        }
    } // namespace

    LogForwardChannel::LogForwardChannel(ITransport& transport, Unblock unblock, void* unblockUser,
                                         const LogForwardLimits& limits)
        : m_transport(transport), m_unblock(unblock), m_unblockUser(unblockUser), m_limits(limits),
          m_tokens(static_cast<double>(limits.errorBurstLines)), m_tokensAt(std::chrono::steady_clock::now()) {
        m_sender = std::thread([this] { Run(); });
    }

    LogForwardChannel::~LogForwardChannel() { (void)Close(); }

    void LogForwardChannel::ForwardThunk(void* user, int level, const char* line) {
        static_cast<LogForwardChannel*>(user)->Offer(level, line);
    }

    bool LogForwardChannel::IsUrgentLocked() const { return m_urgent > 0 || m_closing; }

    void LogForwardChannel::QueueGapNoticesLocked(bool lossyGapToo) {
        // ONE NOTICE PER GAP, however many lines it swallowed, so a notice never needs room of its
        // own worth refusing and the number of notices is bounded by the number of lines that did
        // get in. WHERE each class's gap is closed differs, on purpose:
        //   * an ERROR gap is marked before the very next item queued - the next ERROR line that
        //     fits, a LogFlush ack, Close - because an error gap is rare and where it sits matters;
        //   * a <= WARN gap is marked when the lossy class RECOVERS (the next line at or below WARN
        //     that fits), or at a LogFlush ack or Close. Not before every ERROR line that squeezes
        //     through in the middle of it: under a sustained stall that was one notice per ERROR
        //     line (F2 measured 373 in one run), which buries the errors it sits between. The notice
        //     names the time the gap began instead, so it still says where it started.
        char text[512];
        if (lossyGapToo && m_gapLossyLines != 0) {
            std::snprintf(text, sizeof(text),
                          "MG_Remote server: LogForward{Dropped} - %llu line(s) at or below WARN (%llu bytes) were "
                          "not forwarded, from %s until here: the control connection was not taking them and the "
                          "forward queue held its %llu-byte cap. The server's own log has every one\n",
                          static_cast<unsigned long long>(m_gapLossyLines),
                          static_cast<unsigned long long>(m_gapLossyBytes), m_gapLossySince.c_str(),
                          static_cast<unsigned long long>(m_limits.lossyQueueBytes));
            Item notice;
            notice.kind = Kind::Notice;
            notice.level = MOBILEGL_LOG_LEVEL_WARN;
            notice.seq = m_nextSeq++;
            notice.text = NoticeHeader(notice.level) + text;
            m_queue.push_back(std::move(notice));
            ++m_counters.notices;
            m_gapLossyLines = 0;
            m_gapLossyBytes = 0;
        }
        if (m_gapErrorLines != 0) {
            std::snprintf(text, sizeof(text),
                          "MG_Remote server: LogForward{Coalesced} - %llu ERROR line(s) were not forwarded here: the "
                          "forward queue held its %llu-byte cap of error lines the control connection had not "
                          "taken. The server's own log has every one\n",
                          static_cast<unsigned long long>(m_gapErrorLines),
                          static_cast<unsigned long long>(m_limits.errorQueueBytes));
            Item notice;
            notice.kind = Kind::Notice;
            notice.level = MOBILEGL_LOG_LEVEL_ERROR;
            notice.seq = m_nextSeq++;
            notice.text = NoticeHeader(notice.level) + text;
            m_queue.push_back(std::move(notice));
            ++m_counters.notices;
            m_gapErrorLines = 0;
        }
    }

    void LogForwardChannel::Offer(int level, const char* line) {
        if (line == nullptr) return;
        const auto started = std::chrono::steady_clock::now();
        const std::size_t length = std::strlen(line);
        std::unique_lock<std::mutex> lock(m_mutex);
        // The bound this whole file exists for, measured where it is spent: everything below FATAL
        // returns through here with the lock still held and no wait taken.
        const auto noteOfferTime = [this, started] {
            const auto spent = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                    .count());
            m_counters.maxOfferNs = std::max(m_counters.maxOfferNs, spent);
        };
        if (m_closing || m_counters.transportFailed) {
            // Offered after Close() began or after the peer went: it can never be written.
            ++m_counters.abandoned;
            return;
        }
        const bool lossy = level <= MOBILEGL_LOG_LEVEL_WARN;
        const bool fatal = level >= MOBILEGL_LOG_LEVEL_FATAL;
        if (lossy && m_lossyBytes + length > m_limits.lossyQueueBytes) {
            // The first drop of a gap stamps when it began; the notice that closes it names that.
            if (m_gapLossyLines == 0) m_gapLossySince = MG_Util::Debug::GetCurrentTime();
            ++m_counters.lossyDropped;
            m_counters.lossyDroppedBytes += length;
            ++m_gapLossyLines;
            m_gapLossyBytes += length;
            noteOfferTime();
            return;
        }
        // FATAL is never refused: the process is about to end, and its last word is the one line
        // the cap must not cost. The cap still bounds everything that can repeat.
        if (!lossy && !fatal && m_errorBytes + length > m_limits.errorQueueBytes) {
            ++m_counters.errorsCoalesced;
            ++m_gapErrorLines;
            noteOfferTime();
            return;
        }
        // A lossy line that fits closes a <= WARN gap; any line closes an ERROR gap.
        QueueGapNoticesLocked(/*lossyGapToo=*/lossy);
        Item item;
        item.kind = Kind::Line;
        item.level = level;
        item.seq = m_nextSeq++;
        item.text.assign(line, length);
        const std::uint64_t seq = item.seq;
        m_queue.push_back(std::move(item));
        if (lossy) m_lossyBytes += length;
        else m_errorBytes += length;
        if (fatal) ++m_urgent;
        m_cv.notify_all();
        if (!fatal) {
            noteOfferTime();
            return;
        }
        // FATAL: wait - bounded - until the sender has written this line and everything before it.
        //
        // ONCE PER STALL, NOT ONCE PER LINE. MGLOG_F is not only the last word before an abort: the
        // tree also uses it for survivable conditions (the EGL loader's "Failed to load EGL function"
        // lines, several in a row). If an earlier FATAL wait already ran out and the sender has not
        // written a single item since, the peer is still not reading and a second wait would only
        // spend another fatalFlushMs of this thread's time on the same answer - so it is counted as
        // a timeout at once. The line itself is queued either way.
        if (m_fatalGaveUpAtWritten == m_writtenSeq) {
            ++m_counters.fatalFlushTimeouts;
            return;
        }
        const auto deadline = started + std::chrono::milliseconds(m_limits.fatalFlushMs);
        if (!m_cv.wait_until(lock, deadline,
                             [this, seq] { return m_writtenSeq >= seq || m_counters.transportFailed; })) {
            ++m_counters.fatalFlushTimeouts;
            m_fatalGaveUpAtWritten = m_writtenSeq;
        }
    }

    void LogForwardChannel::OfferControlFrame(std::vector<std::uint8_t> frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closing || m_counters.transportFailed) {
            ++m_counters.abandoned;
            return;
        }
        // A gap before the acknowledgement is marked before it: the client reads the file after the
        // ack arrives, and a gap it cannot see there is a gap it will read as "nothing was said".
        QueueGapNoticesLocked(/*lossyGapToo=*/true);
        Item item;
        item.kind = Kind::Control;
        item.seq = m_nextSeq++;
        item.frame = std::move(frame);
        m_queue.push_back(std::move(item));
        ++m_urgent;
        m_cv.notify_all();
    }

    MobileGLResult LogForwardChannel::Send(const Item& item) {
        if (item.kind == Kind::Control) {
            return m_transport.SendFrame(MobileGLByteSpan{item.frame.data(), item.frame.size()});
        }
        flatbuffers::FlatBufferBuilder builder(item.text.size() + 64);
        const auto text = builder.CreateString(item.text);
        const auto log = Wire::CreateLogLine(builder, WireLogLevelFor(item.level), text);
        const auto envelope = Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::LogLine, log.Union());
        Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        return m_transport.SendFrame(MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()});
    }

    void LogForwardChannel::Run() {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mgl-log-fwd");
#endif
        // THIS THREAD NEVER LOGS. It is the logger's sink: a line it logged would come straight back
        // to its own queue (and in the non-session shape, through the log mutex a FATAL Offer can be
        // waiting behind). What it has to say is in the counters, which Close() hands to its owner.
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return; // stopped, and nothing left
            if (m_counters.transportFailed) {
                // The peer is gone (a write failed): nothing queued can be written any more.
                m_counters.abandoned += m_queue.size();
                m_writtenSeq = m_queue.back().seq;
                m_queue.clear();
                m_lossyBytes = 0;
                m_errorBytes = 0;
                m_urgent = 0;
                m_cv.notify_all();
                continue;
            }
            Item& front = m_queue.front();
            if (front.kind == Kind::Line && front.level == MOBILEGL_LOG_LEVEL_ERROR && !IsUrgentLocked()) {
                // THE ERROR PACE. A token bucket, refilled by the time since it was last looked at.
                // A line with no token waits - in the queue, in order - until one accrues, or until
                // something urgent (a FATAL line, a LogFlush ack, Close) makes the pace moot.
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - m_tokensAt).count();
                m_tokens = std::min(static_cast<double>(m_limits.errorBurstLines),
                                    m_tokens + elapsed * static_cast<double>(m_limits.errorLinesPerSecond));
                m_tokensAt = now;
                if (m_tokens < 1.0) {
                    if (!front.paced) {
                        front.paced = true;
                        ++m_counters.errorsPaced;
                    }
                    const double rate = std::max(1.0, static_cast<double>(m_limits.errorLinesPerSecond));
                    const auto untilToken = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>((1.0 - m_tokens) / rate));
                    m_cv.wait_for(lock, untilToken, [this] { return IsUrgentLocked() || m_stop; });
                    continue;
                }
                m_tokens -= 1.0;
            }
            Item item = std::move(front);
            m_queue.pop_front();
            if (item.kind == Kind::Line) {
                if (item.level <= MOBILEGL_LOG_LEVEL_WARN) m_lossyBytes -= item.text.size();
                else m_errorBytes -= item.text.size();
            }
            if (item.kind == Kind::Control || (item.kind == Kind::Line && item.level >= MOBILEGL_LOG_LEVEL_FATAL)) {
                --m_urgent;
            }
            m_sending = true;
            lock.unlock();
            const MobileGLResult sent = Send(item);
            lock.lock();
            m_sending = false;
            m_writtenSeq = item.seq;
            if (sent == MOBILEGL_OK) {
                if (item.kind == Kind::Line) ++m_counters.forwarded;
            } else if (sent == MOBILEGL_ERR_INVALID_ARGUMENT) {
                // Refused on its own shape (over the frame cap), not because the peer is gone.
                ++m_counters.abandoned;
            } else {
                ++m_counters.abandoned;
                m_counters.transportFailed = true;
            }
            m_cv.notify_all();
        }
    }

    LogForwardCounters LogForwardChannel::Close() {
        bool unblock = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_closed) return m_counters;
            if (!m_closing) {
                m_closing = true;
                // The last gap is marked too: a client reading the file after the session sees
                // that the tail it has is not the whole tail.
                QueueGapNoticesLocked(/*lossyGapToo=*/true);
                m_cv.notify_all();
                const auto drained = [this] {
                    return (m_queue.empty() && !m_sending) || m_counters.transportFailed;
                };
                if (!m_cv.wait_for(lock, std::chrono::milliseconds(m_limits.closeDrainMs), drained)) {
                    // Not taken within the budget: the peer is not reading. The rest is abandoned,
                    // and a sender inside a write is blocked on that peer and needs unblocking.
                    m_counters.abandoned += m_queue.size();
                    if (!m_queue.empty()) m_writtenSeq = m_queue.back().seq;
                    m_queue.clear();
                    m_lossyBytes = 0;
                    m_errorBytes = 0;
                    m_urgent = 0;
                    unblock = m_sending;
                }
            }
            m_stop = true;
            m_cv.notify_all();
        }
        if (unblock && m_unblock != nullptr) m_unblock(m_unblockUser);
        if (m_sender.joinable()) m_sender.join();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        return m_counters;
    }

    LogForwardCounters LogForwardChannel::Counters() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_counters;
    }

} // namespace MobileGL::MG_Remote::Transport
