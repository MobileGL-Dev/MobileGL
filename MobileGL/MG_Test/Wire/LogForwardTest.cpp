// MobileGL - MobileGL/MG_Test/Wire/LogForwardTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P9 W3: the server's graded log forward (MG_Remote/Transport/LogForward.h), without a server.
//
// THREE LAYERS, EACH ONE A CLAIM THE NEXT DEPENDS ON:
//   1. THE LEVELS. MobileGL's five, the Android priorities Log() is handed, the wire's five - every
//      level both ways, and the out-of-range answers. Before W3 every forwarded line crossed as
//      Info, so a mapping that collapsed two levels would have been invisible; these cases are
//      what make a collapse red.
//   2. THE LOGGER. MGLOG_I/W/E/F on a server-role thread reach an installed forwarder with the level
//      of the macro that logged them - the chain Log() -> LogLevelFromAndroidPriority ->
//      LogForwarder, which is the half of "forwarding carries the level" the mapping alone does not
//      prove. And the client's writer files a peer line under the wire's level.
//   3. THE CHANNEL. Driven over a transport whose writes can be HELD (a peer that is not reading):
//      a caller is never held below FATAL, <= WARN is dropped and counted and the gap is marked in
//      the stream, ERROR is never dropped (paced, or coalesced into a named line past its own cap),
//      a FATAL waits for its write but only for its bound, a LogFlush ack goes out behind every
//      earlier line and unpaced, and Close() gets its sender back from a peer that never reads.
//      The last case does the same over a real socket pair the far end never reads.
//
// The two-process version - a real `--serve` session child whose apply thread floods its log while
// its TCP client stops reading control - is fault injection F2 in EventForfeitPeerTest.

#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Transport/LogForward.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Util/Debug/Log.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote::Transport;
namespace Debug = MobileGL::MG_Util::Debug;
namespace Wire = MobileGL::Wire;

namespace {

    using Clock = std::chrono::steady_clock;

    long long MillisecondsSince(Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }

    // One frame the channel wrote, decoded.
    struct Written {
        bool isLog = false;
        bool isFlushAck = false;
        Wire::LogLevel level = Wire::LogLevel::Debug;
        std::string text;
        std::uint64_t flushSeq = 0;
    };

    Written Decode(const std::uint8_t* bytes, std::size_t size) {
        Written out;
        flatbuffers::Verifier verifier(bytes, size);
        if (!Wire::VerifyCtrlEnvelopeBuffer(verifier)) return out;
        const auto* envelope = Wire::GetCtrlEnvelope(bytes);
        if (const auto* line = envelope->msg_as_LogLine()) {
            out.isLog = true;
            out.level = line->level();
            if (line->text()) out.text = line->text()->str();
        } else if (const auto* flush = envelope->msg_as_LogFlush()) {
            out.isFlushAck = flush->ack();
            out.flushSeq = flush->seq();
        }
        return out;
    }

    // A transport whose writes can be HELD: while the gate is shut, SendFrame blocks exactly as a
    // socket write to a peer that is not reading does. Cut() ends every held and later write with
    // TRANSPORT_CLOSED - what shutdown(2) does to the real one - and is the channel's unblock hook.
    class GatedTransport final : public ITransport {
    public:
        explicit GatedTransport(bool open) : m_open(open) {}

        MobileGLResult SendFrame(MobileGLByteSpan bytes) override {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_attempts;
            m_cv.notify_all();
            m_cv.wait(lock, [this] { return m_open || m_cut; });
            if (m_cut) return MOBILEGL_ERR_TRANSPORT_CLOSED;
            m_written.push_back(Decode(static_cast<const std::uint8_t*>(bytes.data), static_cast<std::size_t>(bytes.size)));
            m_cv.notify_all();
            return MOBILEGL_OK;
        }
        MobileGLResult ReceiveFrame(MobileGLMutableByteSpan, std::uint64_t*, std::uint32_t) override {
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        std::uint64_t PeekFrameSize() override { return 0; }
        MobileGLResult ShareFd(int, MobileGLByteSpan) override { return MOBILEGL_ERR_UNSUPPORTED; }
        MobileGLResult ReceiveFd(int*, MobileGLMutableByteSpan, std::uint64_t*, std::uint32_t) override {
            return MOBILEGL_ERR_UNSUPPORTED;
        }
        void Shutdown() override { Cut(); }
        TransportRole Role() const override { return TransportRole::Server; }

        void Open() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
            m_cv.notify_all();
        }
        void Cut() {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cut = true;
            ++m_cuts;
            m_cv.notify_all();
        }
        static void CutThunk(void* user) { static_cast<GatedTransport*>(user)->Cut(); }

        // Blocks until the channel's sender is inside a held write (or `budget` runs out).
        bool WaitForAttempt(std::chrono::milliseconds budget) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, budget, [this] { return m_attempts > 0; });
        }
        bool WaitForWritten(std::size_t count, std::chrono::milliseconds budget) {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, budget, [this, count] { return m_written.size() >= count; });
        }
        std::vector<Written> Frames() {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_written;
        }
        int Cuts() {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_cuts;
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_open = false;
        bool m_cut = false;
        int m_cuts = 0;
        std::size_t m_attempts = 0;
        std::vector<Written> m_written;
    };

    std::string Numbered(const char* series, int index, std::size_t pad = 0) {
        std::string line = "[00:00:00] [Linux test/X]: " + std::string(series) + " #" + std::to_string(index);
        line.append(pad, 'p');
        return line + "\n";
    }

    bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

    // The count a LogForward notice names ("... - N line(s) ..."), or -1.
    long long NoticeCount(const std::string& text, const char* word) {
        const std::string key = std::string("LogForward{") + word + "} - ";
        const auto at = text.find(key);
        if (at == std::string::npos) return -1;
        return std::strtoll(text.c_str() + at + key.size(), nullptr, 10);
    }

    std::vector<int> SeriesIndices(const std::vector<Written>& written, const char* series) {
        std::vector<int> indices;
        const std::string key = std::string(series) + " #";
        for (const auto& frame : written) {
            if (!frame.isLog) continue;
            const auto at = frame.text.find(key);
            if (at != std::string::npos) indices.push_back(std::atoi(frame.text.c_str() + at + key.size()));
        }
        return indices;
    }

    // ---- 1. the levels ------------------------------------------------------------------------

    TEST(LogForwardLevels, EveryMobileGLLevelCrossesTheWireAndComesBackUnchanged) {
        const struct {
            int level;
            Wire::LogLevel wire;
        } kRows[] = {
            {MOBILEGL_LOG_LEVEL_DEBUG, Wire::LogLevel::Debug}, {MOBILEGL_LOG_LEVEL_INFO, Wire::LogLevel::Info},
            {MOBILEGL_LOG_LEVEL_WARN, Wire::LogLevel::Warn},   {MOBILEGL_LOG_LEVEL_ERROR, Wire::LogLevel::Error},
            {MOBILEGL_LOG_LEVEL_FATAL, Wire::LogLevel::Fatal},
        };
        for (const auto& row : kRows) {
            EXPECT_EQ(WireLogLevelFor(row.level), row.wire) << "level " << row.level;
            EXPECT_EQ(LogLevelFromWire(row.wire), row.level) << Wire::EnumNameLogLevel(row.wire);
            EXPECT_EQ(LogLevelFromWire(WireLogLevelFor(row.level)), row.level) << "level " << row.level;
        }
        // Every wire value the schema has, back and forth, so a sixth appended value that nobody
        // mapped cannot hide behind the five rows above.
        for (const Wire::LogLevel wire : Wire::EnumValuesLogLevel()) {
            EXPECT_EQ(WireLogLevelFor(LogLevelFromWire(wire)), wire) << Wire::EnumNameLogLevel(wire);
        }
    }

    TEST(LogForwardLevels, OutOfRangeLevelsClampAndAnUnknownWireLevelIsNeverDemoted) {
        EXPECT_EQ(WireLogLevelFor(-3), Wire::LogLevel::Debug);
        EXPECT_EQ(WireLogLevelFor(MOBILEGL_LOG_LEVEL_FATAL + 5), Wire::LogLevel::Fatal);
        // A newer peer's severity this build has no name for is filed as an error: never lossy.
        EXPECT_EQ(LogLevelFromWire(static_cast<Wire::LogLevel>(9)), MOBILEGL_LOG_LEVEL_ERROR);
        EXPECT_EQ(LogLevelFromWire(static_cast<Wire::LogLevel>(255)), MOBILEGL_LOG_LEVEL_ERROR);
    }

    TEST(LogForwardLevels, TheAndroidPriorityLogIsHandedCarriesEachMgLogLevel) {
        // What MGLOG_D..MGLOG_F pass (Log.h), each to its own level.
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_DEBUG), MOBILEGL_LOG_LEVEL_DEBUG);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_INFO), MOBILEGL_LOG_LEVEL_INFO);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_WARN), MOBILEGL_LOG_LEVEL_WARN);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_ERROR), MOBILEGL_LOG_LEVEL_ERROR);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_FATAL), MOBILEGL_LOG_LEVEL_FATAL);
        // What no MGLOG_* passes.
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_VERBOSE), MOBILEGL_LOG_LEVEL_DEBUG);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_UNKNOWN), MOBILEGL_LOG_LEVEL_INFO);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_DEFAULT), MOBILEGL_LOG_LEVEL_INFO);
        EXPECT_EQ(Debug::LogLevelFromAndroidPriority(ANDROID_LOG_SILENT), MOBILEGL_LOG_LEVEL_INFO);
        EXPECT_STREQ(Debug::LogLevelTag(MOBILEGL_LOG_LEVEL_DEBUG), "DEBUG");
        EXPECT_STREQ(Debug::LogLevelTag(MOBILEGL_LOG_LEVEL_INFO), "INFO");
        EXPECT_STREQ(Debug::LogLevelTag(MOBILEGL_LOG_LEVEL_WARN), "WARN");
        EXPECT_STREQ(Debug::LogLevelTag(MOBILEGL_LOG_LEVEL_ERROR), "ERROR");
        EXPECT_STREQ(Debug::LogLevelTag(MOBILEGL_LOG_LEVEL_FATAL), "FATAL");
        EXPECT_STREQ(Debug::LogLevelTag(-1), "DEBUG");
        EXPECT_STREQ(Debug::LogLevelTag(99), "FATAL");
    }

    // ---- 2. the logger ------------------------------------------------------------------------

    // One log base for the whole binary: the file sinks latch their path on first use, and gtest
    // may run every case in one process.
    const std::string& LogBase() {
        static const std::string base = [] {
            const std::string path = "/tmp/mgl-logforward-" + std::to_string(::getpid()) + ".log";
            ::setenv("MOBILEGL_LOG_FILE_PATH", path.c_str(), 1);
            Debug::TruncateRoleLogs(path.c_str());
            return path;
        }();
        return base;
    }

    std::string ReadWhole(const std::string& path) {
        std::string all;
        if (FILE* file = std::fopen(path.c_str(), "rb")) {
            char chunk[4096];
            std::size_t got = 0;
            while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) all.append(chunk, got);
            std::fclose(file);
        }
        return all;
    }

    struct Captured {
        int level;
        std::string line;
    };
    std::mutex g_capturedMutex;
    std::vector<Captured> g_captured;
    void Capture(void*, int level, const char* line) {
        std::lock_guard<std::mutex> lock(g_capturedMutex);
        g_captured.push_back({level, line});
    }

    int CapturedLevelOf(const char* marker) {
        std::lock_guard<std::mutex> lock(g_capturedMutex);
        for (const auto& entry : g_captured) {
            if (Contains(entry.line, marker)) return entry.level;
        }
        return -1;
    }

    TEST(LogForwardLogger, EachMgLogMacroReachesTheForwarderWithItsOwnLevel) {
        (void)LogBase();
        // A thread of its own, so the server role this stamps does not leak into another case.
        std::thread server([] {
            Debug::SetThreadLogRole(Debug::LogRole::Server);
            Debug::SetLogForwarder(&Capture, nullptr);
            MGLOG_I("w3-logger-marker info");
            MGLOG_W("w3-logger-marker warn");
            MGLOG_E("w3-logger-marker error");
            // FATAL goes to the forwarder OUTSIDE the log mutex (Log.cpp) - still with its level.
            MGLOG_F("w3-logger-marker fatal");
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
            MGLOG_D("w3-logger-marker debug");
#endif
            Debug::SetLogForwarder(nullptr, nullptr);
        });
        server.join();
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker info"), MOBILEGL_LOG_LEVEL_INFO);
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker warn"), MOBILEGL_LOG_LEVEL_WARN);
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker error"), MOBILEGL_LOG_LEVEL_ERROR);
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker fatal"), MOBILEGL_LOG_LEVEL_FATAL);
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_DEBUG
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker debug"), MOBILEGL_LOG_LEVEL_DEBUG);
#endif
        // A client-role thread forwards nothing: the forwarder is the SERVER's.
        std::thread client([] {
            Debug::SetThreadLogRole(Debug::LogRole::Client);
            Debug::SetLogForwarder(&Capture, nullptr);
            MGLOG_E("w3-logger-marker client-side");
            Debug::SetLogForwarder(nullptr, nullptr);
        });
        client.join();
        EXPECT_EQ(CapturedLevelOf("w3-logger-marker client-side"), -1);
    }

    TEST(LogForwardLogger, APeerLineIsFiledUnderTheLevelTheWireCarried) {
        const std::string serverLog = Debug::RoleLogPath(LogBase().c_str(), Debug::LogRole::Server);
        // A line the server formatted is written as it came - its header already names its level.
        Debug::WritePeerLog(MOBILEGL_LOG_LEVEL_ERROR, "[01:02:03] [Linux mgl-srv-apply/ERROR]: w3-peer headed\n");
        // Bare text gets the wire's level as its tag, and a line end.
        Debug::WritePeerLog(MOBILEGL_LOG_LEVEL_WARN, "w3-peer bare warn");
        Debug::WritePeerLog(MOBILEGL_LOG_LEVEL_FATAL, "w3-peer bare fatal\n");
        Debug::WritePeerLog(LogLevelFromWire(Wire::LogLevel::Debug), "w3-peer bare debug");
        const std::string log = ReadWhole(serverLog);
        EXPECT_TRUE(Contains(log, "[01:02:03] [Linux mgl-srv-apply/ERROR]: w3-peer headed\n")) << log;
        EXPECT_TRUE(Contains(log, "[peer/WARN]: w3-peer bare warn\n")) << log;
        EXPECT_TRUE(Contains(log, "[peer/FATAL]: w3-peer bare fatal\n")) << log;
        EXPECT_TRUE(Contains(log, "[peer/DEBUG]: w3-peer bare debug\n")) << log;
        EXPECT_FALSE(Contains(log, "[peer/ERROR]: [01:02:03]")) << "a headed line was tagged twice\n" << log;
    }

    // ---- 3. the channel -----------------------------------------------------------------------

    TEST(LogForwardChannel, LinesGoOutInOrderWithTheirLevelOnTheWire) {
        GatedTransport transport(/*open=*/true);
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport);
        channel.Offer(MOBILEGL_LOG_LEVEL_DEBUG, "order debug\n");
        channel.Offer(MOBILEGL_LOG_LEVEL_INFO, "order info\n");
        channel.Offer(MOBILEGL_LOG_LEVEL_WARN, "order warn\n");
        channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, "order error\n");
        channel.Offer(MOBILEGL_LOG_LEVEL_FATAL, "order fatal\n");
        const auto counters = channel.Close();
        const auto written = transport.Frames();
        ASSERT_EQ(written.size(), 5u);
        const Wire::LogLevel levels[] = {Wire::LogLevel::Debug, Wire::LogLevel::Info, Wire::LogLevel::Warn,
                                         Wire::LogLevel::Error, Wire::LogLevel::Fatal};
        const char* texts[] = {"order debug\n", "order info\n", "order warn\n", "order error\n", "order fatal\n"};
        for (std::size_t i = 0; i < 5; ++i) {
            EXPECT_TRUE(written[i].isLog);
            EXPECT_EQ(written[i].level, levels[i]) << i;
            EXPECT_EQ(written[i].text, texts[i]) << i;
        }
        EXPECT_EQ(counters.forwarded, 5u);
        EXPECT_EQ(counters.lossyDropped, 0u);
        EXPECT_EQ(counters.abandoned, 0u);
        EXPECT_EQ(transport.Cuts(), 0) << "a drained Close must not cut the transport";
    }

    // THE CASE THE WHOLE FILE IS FOR. The peer stops reading (the sender's first write is held);
    // the caller keeps logging WARN lines. Every Offer must return at once - timed, on a worker,
    // under a watchdog so that a channel that DOES hold its caller reds this case instead of hanging
    // it - the lines past the lossy cap are dropped and counted, and once the peer reads again the
    // gap is marked in the stream, before the next line, with the count.
    TEST(LogForwardChannel, APeerThatStopsReadingCostsWarnLinesNotTheCallersTime) {
        GatedTransport transport(/*open=*/false);
        LogForwardLimits limits;
        limits.lossyQueueBytes = 16 * 1024;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        constexpr int kLines = 2000;
        std::atomic<bool> done{false};
        long long offeringMs = -1;
        std::thread caller([&] {
            const auto start = Clock::now();
            for (int i = 1; i <= kLines; ++i) channel.Offer(MOBILEGL_LOG_LEVEL_WARN, Numbered("stall warn", i, 100).c_str());
            offeringMs = MillisecondsSince(start);
            done = true;
        });
        const auto start = Clock::now();
        while (!done && MillisecondsSince(start) < 5000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool heldTheCaller = !done;
        transport.Open(); // lets a held caller finish, so a red is a red and not a hang
        caller.join();
        ASSERT_FALSE(heldTheCaller) << "2000 WARN Offers did not return within 5 s while the peer was not "
                                       "reading: the forward held its caller";
        EXPECT_LT(offeringMs, 2000) << "the Offers took " << offeringMs << " ms against a peer that was not reading";
        auto counters = channel.Counters();
        EXPECT_GT(counters.lossyDropped, 0u) << "nothing was dropped past a 16 KiB cap with ~230 KiB offered";
        EXPECT_LT(counters.maxOfferNs, 50ull * 1000 * 1000) << "one Offer took " << counters.maxOfferNs << " ns";
        // The peer reads again; once what was queued is out, the next line goes out behind a notice
        // naming the gap. (Waited for first: offered into a queue still full, it would be dropped.)
        ASSERT_TRUE(transport.WaitForWritten(kLines - counters.lossyDropped, std::chrono::milliseconds(5000)));
        channel.Offer(MOBILEGL_LOG_LEVEL_WARN, "after the gap\n");
        counters = channel.Close();
        const auto written = transport.Frames();
        ASSERT_FALSE(written.empty());
        EXPECT_EQ(written.back().text, "after the gap\n");
        ASSERT_GE(written.size(), 2u);
        const Written& notice = written[written.size() - 2];
        EXPECT_EQ(notice.level, Wire::LogLevel::Warn);
        EXPECT_EQ(NoticeCount(notice.text, "Dropped"), static_cast<long long>(counters.lossyDropped)) << notice.text;
        EXPECT_EQ(notice.text[0], '[') << "a notice must carry a header, so the client files it as it came";
        EXPECT_EQ(counters.notices, 1u);
        // Every line is accounted for: written, or counted as dropped.
        EXPECT_EQ(SeriesIndices(written, "stall warn").size() + counters.lossyDropped, static_cast<std::size_t>(kLines));
        EXPECT_EQ(counters.forwarded, SeriesIndices(written, "stall warn").size() + 1);
    }

    TEST(LogForwardChannel, ErrorLinesAreNeverDroppedWhileWarnLinesAre) {
        GatedTransport transport(/*open=*/false);
        LogForwardLimits limits;
        limits.lossyQueueBytes = 8 * 1024;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        constexpr int kEach = 500;
        for (int i = 1; i <= kEach; ++i) {
            channel.Offer(MOBILEGL_LOG_LEVEL_WARN, Numbered("mixed warn", i, 100).c_str());
            channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, Numbered("mixed error", i, 100).c_str());
        }
        auto counters = channel.Counters();
        EXPECT_GT(counters.lossyDropped, 0u);
        EXPECT_EQ(counters.errorsCoalesced, 0u);
        transport.Open();
        counters = channel.Close();
        const auto errors = SeriesIndices(transport.Frames(), "mixed error");
        ASSERT_EQ(errors.size(), static_cast<std::size_t>(kEach)) << "an ERROR line was lost while WARN lines were dropped";
        for (int i = 0; i < kEach; ++i) EXPECT_EQ(errors[i], i + 1) << "ERROR lines out of order at " << i;
        EXPECT_EQ(counters.errorsCoalesced, 0u);
        EXPECT_EQ(counters.abandoned, 0u);
    }

    // Memory is bounded for ERROR too - but past its cap an ERROR line becomes a COUNT in a named
    // ERROR line, never nothing: every line offered is written or counted by a notice.
    TEST(LogForwardChannel, ErrorLinesPastTheirCapAreCoalescedIntoANamedLineNotLost) {
        GatedTransport transport(/*open=*/false);
        LogForwardLimits limits;
        limits.errorQueueBytes = 4 * 1024;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        constexpr int kLines = 200;
        for (int i = 1; i <= kLines; ++i) channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, Numbered("capped error", i, 60).c_str());
        auto counters = channel.Counters();
        EXPECT_GT(counters.errorsCoalesced, 0u);
        transport.Open();
        ASSERT_TRUE(transport.WaitForWritten(kLines - counters.errorsCoalesced, std::chrono::milliseconds(5000)));
        channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, "after the error gap\n");
        counters = channel.Close();
        const auto written = transport.Frames();
        long long coalescedNamed = 0;
        for (const auto& frame : written) {
            const long long named = NoticeCount(frame.text, "Coalesced");
            if (named < 0) continue;
            EXPECT_EQ(frame.level, Wire::LogLevel::Error) << "a coalesced ERROR gap must be named at ERROR";
            coalescedNamed += named;
        }
        EXPECT_EQ(coalescedNamed, static_cast<long long>(counters.errorsCoalesced));
        EXPECT_EQ(SeriesIndices(written, "capped error").size() + counters.errorsCoalesced, static_cast<std::size_t>(kLines))
            << "an ERROR line was neither written nor counted";
        EXPECT_EQ(written.back().text, "after the error gap\n");
    }

    TEST(LogForwardChannel, ErrorLinesPastThePaceWaitInOrderAndAreNotDropped) {
        GatedTransport transport(/*open=*/true);
        LogForwardLimits limits;
        limits.errorLinesPerSecond = 40;
        limits.errorBurstLines = 4;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        constexpr int kLines = 24;
        const auto start = Clock::now();
        for (int i = 1; i <= kLines; ++i) channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, Numbered("paced error", i).c_str());
        EXPECT_LT(MillisecondsSince(start), 200) << "pacing held the caller";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // A burst of 4 and ~40/s: about 8 by now, and far fewer than all 24.
        EXPECT_LT(transport.Frames().size(), 18u) << "the pace did not hold anything back";
        ASSERT_TRUE(transport.WaitForWritten(kLines, std::chrono::milliseconds(5000))) << "paced lines never all went out";
        const auto counters = channel.Close();
        const auto indices = SeriesIndices(transport.Frames(), "paced error");
        ASSERT_EQ(indices.size(), static_cast<std::size_t>(kLines));
        for (int i = 0; i < kLines; ++i) EXPECT_EQ(indices[i], i + 1);
        EXPECT_GE(counters.errorsPaced, 10u);
        EXPECT_EQ(counters.errorsCoalesced, 0u);
    }

    // MGPipeSyncPeerLog's promise, kept through a queue: the ack is written after every line
    // offered before it - and at once, not at the ERROR pace, because the client is waiting on it.
    TEST(LogForwardChannel, AFlushAckGoesOutBehindEveryEarlierLineAndIsNotPaced) {
        GatedTransport transport(/*open=*/true);
        LogForwardLimits limits;
        limits.errorLinesPerSecond = 1;
        limits.errorBurstLines = 1;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        for (int i = 1; i <= 10; ++i) channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, Numbered("before ack", i).c_str());
        flatbuffers::FlatBufferBuilder builder(64);
        const auto flush = Wire::CreateLogFlush(builder, 77, true);
        const auto envelope = Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::LogFlush, flush.Union());
        Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        const auto start = Clock::now();
        channel.OfferControlFrame(
            std::vector<std::uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()));
        ASSERT_TRUE(transport.WaitForWritten(11, std::chrono::milliseconds(2000)))
            << "the ack (and the lines before it) waited for a 1-line-per-second pace";
        EXPECT_LT(MillisecondsSince(start), 1500);
        (void)channel.Close();
        const auto written = transport.Frames();
        ASSERT_EQ(written.size(), 11u);
        EXPECT_EQ(SeriesIndices(written, "before ack").size(), 10u);
        EXPECT_TRUE(written.back().isFlushAck);
        EXPECT_EQ(written.back().flushSeq, 77u);
    }

    TEST(LogForwardChannel, AFatalLineWaitsForItsWriteButOnlyForItsBound) {
        {
            // The peer reads: the Offer returns with the line (and the one before it) written.
            GatedTransport transport(/*open=*/true);
            LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport);
            channel.Offer(MOBILEGL_LOG_LEVEL_WARN, "before fatal\n");
            channel.Offer(MOBILEGL_LOG_LEVEL_FATAL, "fatal written\n");
            const auto written = transport.Frames();
            ASSERT_EQ(written.size(), 2u) << "a FATAL Offer returned before its line was written";
            EXPECT_EQ(written[1].level, Wire::LogLevel::Fatal);
            EXPECT_EQ(channel.Counters().fatalFlushTimeouts, 0u);
        }
        {
            // The peer does not read: the Offer returns after its bound, not never.
            GatedTransport transport(/*open=*/false);
            LogForwardLimits limits;
            limits.fatalFlushMs = 300;
            limits.closeDrainMs = 100;
            LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
            const auto start = Clock::now();
            channel.Offer(MOBILEGL_LOG_LEVEL_FATAL, "fatal held\n");
            const long long waited = MillisecondsSince(start);
            EXPECT_GE(waited, 250);
            EXPECT_LT(waited, 3000);
            EXPECT_EQ(channel.Counters().fatalFlushTimeouts, 1u);
        }
    }

    // A peer that never reads again: Close() gives the queue its budget, abandons the rest, cuts
    // the transport to get the sender out of its held write, and joins it.
    TEST(LogForwardChannel, CloseGetsItsSenderBackFromAPeerThatNeverReads) {
        GatedTransport transport(/*open=*/false);
        LogForwardLimits limits;
        limits.closeDrainMs = 200;
        LogForwardChannel channel(transport, &GatedTransport::CutThunk, &transport, limits);
        for (int i = 1; i <= 50; ++i) channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, Numbered("never read", i).c_str());
        ASSERT_TRUE(transport.WaitForAttempt(std::chrono::milliseconds(2000)));
        const auto start = Clock::now();
        const auto counters = channel.Close();
        EXPECT_LT(MillisecondsSince(start), 3000);
        EXPECT_EQ(transport.Cuts(), 1) << "the held sender was not unblocked";
        EXPECT_EQ(counters.abandoned, 50u) << "49 queued + the one inside the held write";
        EXPECT_TRUE(counters.transportFailed);
        EXPECT_EQ(counters.forwarded, 0u);
        // After Close, an Offer is refused (counted), never queued behind a dead sender.
        channel.Offer(MOBILEGL_LOG_LEVEL_ERROR, "after close\n");
        EXPECT_EQ(channel.Counters().abandoned, 51u);
    }

#if !defined(_WIN32)
    // The same stall over the real thing: a SocketTransport pair whose far end does not read until
    // the caller is done. The socket fills, the sender blocks inside SendFrame (as it would on TCP),
    // and the caller - 20,000 WARN lines of ~600 bytes, ~12 MB, far past any socket buffer - never
    // waits on it. Then the far end reads, and what it gets is levelled LogLines with the gap
    // marked. Close() needs no cut: the reader drains.
    TEST(LogForwardChannel, OverARealSocketThePeerNotReadingNeverHoldsTheCaller) {
        std::unique_ptr<SocketTransport> client, server;
        ASSERT_EQ(SocketTransport::CreatePair(client, server), MOBILEGL_OK);
        LogForwardChannel channel(*server, nullptr, nullptr);
        constexpr int kLines = 20000;
        const auto start = Clock::now();
        for (int i = 1; i <= kLines; ++i) {
            channel.Offer(i % 100 == 0 ? MOBILEGL_LOG_LEVEL_ERROR : MOBILEGL_LOG_LEVEL_WARN,
                          Numbered(i % 100 == 0 ? "socket error" : "socket warn", i, 560).c_str());
        }
        const long long offeringMs = MillisecondsSince(start);
        EXPECT_LT(offeringMs, 5000) << "offering took " << offeringMs << " ms against a socket nobody read";
        EXPECT_GT(channel.Counters().lossyDropped, 0u);
        EXPECT_EQ(channel.Counters().errorsCoalesced, 0u);
        // Now the far end reads, until the ack the channel queues behind everything arrives.
        flatbuffers::FlatBufferBuilder builder(64);
        const auto flush = Wire::CreateLogFlush(builder, 9, true);
        const auto envelope = Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::LogFlush, flush.Union());
        Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        channel.OfferControlFrame(
            std::vector<std::uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()));
        std::vector<Written> received;
        std::vector<std::uint8_t> frame(1 << 20);
        const auto readStart = Clock::now();
        for (;;) {
            ASSERT_LT(MillisecondsSince(readStart), 20000) << "the ack never arrived";
            std::uint64_t size = 0;
            const auto got = client->ReceiveFrame({frame.data(), frame.size()}, &size, 1000);
            if (got == MOBILEGL_ERR_TIMEOUT) continue;
            ASSERT_EQ(got, MOBILEGL_OK);
            received.push_back(Decode(frame.data(), static_cast<std::size_t>(size)));
            if (received.back().isFlushAck) break;
        }
        const auto counters = channel.Close();
        const auto errors = SeriesIndices(received, "socket error");
        ASSERT_EQ(errors.size(), static_cast<std::size_t>(kLines / 100)) << "an ERROR line was lost";
        for (std::size_t i = 0; i < errors.size(); ++i) EXPECT_EQ(errors[i], static_cast<int>((i + 1) * 100));
        long long droppedNamed = 0;
        for (const auto& line : received) {
            if (!line.isLog) continue;
            const long long named = NoticeCount(line.text, "Dropped");
            if (named >= 0) droppedNamed += named;
            else if (Contains(line.text, "socket warn")) EXPECT_EQ(line.level, Wire::LogLevel::Warn);
            else if (Contains(line.text, "socket error")) EXPECT_EQ(line.level, Wire::LogLevel::Error);
        }
        EXPECT_EQ(droppedNamed, static_cast<long long>(counters.lossyDropped));
        EXPECT_EQ(SeriesIndices(received, "socket warn").size() + counters.lossyDropped,
                  static_cast<std::size_t>(kLines - kLines / 100));
        EXPECT_EQ(counters.abandoned, 0u);
        EXPECT_FALSE(counters.transportFailed);
    }
#endif

} // namespace
