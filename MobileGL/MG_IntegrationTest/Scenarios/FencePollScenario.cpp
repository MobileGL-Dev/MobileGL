// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/FencePollScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P10 (A, MG_Remote/CONTRACT-P10.md §1): FENCE POLLS ANSWERED FROM THE REVERSE CHANNEL.
//
// A glClientWaitSync / glGetSynciv(GL_SYNC_STATUS) used to be a kWaitReply record: its answer
// came back only after the server had applied every record before it, so a poll was a full
// pipeline drain and, over TCP, a round trip - once per frame in Minecraft's fence use. Now the
// server reports each fence the GPU finishes (kEventFenceSignaled) and the client answers polls
// from those reports; the Nth unanswered zero-timeout poll in a row (MOBILEGL_IPC_POLL_ESCALATE)
// still crosses, which is what bounds a loop whose report is late.
//
// Three cases. The first is ordinary GL - the idiom's two poll loops end - and its flush-bit arm
// runs on the monolith lanes too. The other two are about the wire and skip where there is none.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>

#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr auto kLoopBound = std::chrono::seconds(10);

        class FencePollScenario : public ScenarioTest {
        protected:
            // Some GPU work for a fence to wait behind.
            static void QueueWork() {
                glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        };

        // The GL idiom: `while (glClientWaitSync(s, flags, 0) == GL_TIMEOUT_EXPIRED) {}` and the
        // glGetSynciv twin. With nothing else in the loop, no record would ever be published if
        // the poll were not a doorbell point, and a local "not yet" would be repeated for ever if
        // nothing made it true - so the loop ending, bounded, is the property.
        //
        // WITHOUT GL_SYNC_FLUSH_COMMANDS_BIT GL itself does not promise the loop ends (the fence
        // may sit unflushed in the driver), so that arm is split-only: there the transport
        // promises it, because the poll flushes the link and an idle server submits.
        TEST_F(FencePollScenario, ZeroTimeoutPollLoopsEndInBoundedTime) {
            if (!Ready()) return;
            const bool split = SplitRuntimeSkipReason().empty();
            for (const GLbitfield flags : {GLbitfield(GL_SYNC_FLUSH_COMMANDS_BIT), GLbitfield(0)}) {
                if (flags == 0 && !split) continue;
                QueueWork();
                GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                ASSERT_NE(sync, nullptr);
                const auto start = Clock::now();
                GLenum status = GL_TIMEOUT_EXPIRED;
                unsigned long long polls = 0;
                while ((status = glClientWaitSync(sync, flags, 0)) == GL_TIMEOUT_EXPIRED &&
                       Clock::now() - start < kLoopBound) {
                    ++polls;
                }
                EXPECT_TRUE(status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
                    << "glClientWaitSync(flags 0x" << std::hex << flags << std::dec << ", 0) still "
                    << status << " after " << polls << " polls";
                glDeleteSync(sync);
            }
            QueueWork();
            GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            ASSERT_NE(sync, nullptr);
            const auto start = Clock::now();
            GLint value = GL_UNSIGNALED;
            while (value != GL_SIGNALED && Clock::now() - start < kLoopBound) {
                glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &value);
            }
            EXPECT_EQ(value, GL_SIGNALED) << "glGetSynciv(GL_SYNC_STATUS) never read signaled";
            glDeleteSync(sync);
            EXPECT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        // THE WIRE HALF: once a fence is known signaled, polling it costs nothing - no reply-slot
        // record, no round trip - however often it is asked. MOBILEGL_IPC_POLL_ESCALATE=0 puts the
        // round trip back and must fail both counters (its red-once).
        TEST_F(FencePollScenario, PollsOfASignaledFenceCrossNothing) {
            if (!Ready()) return;
            const std::string why = SplitRuntimeSkipReason();
            if (!why.empty()) GTEST_SKIP() << why;
            QueueWork();
            GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            ASSERT_NE(sync, nullptr);
            const GLenum waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
            ASSERT_TRUE(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED) << waited;

            constexpr unsigned kPolls = 100;
            const SplitRuntimeState before = PeekSplitRuntime();
            for (unsigned i = 0; i < kPolls; ++i) {
                EXPECT_EQ(glClientWaitSync(sync, 0, 0), static_cast<GLenum>(GL_ALREADY_SIGNALED));
                GLint value = GL_UNSIGNALED;
                glGetSynciv(sync, GL_SYNC_STATUS, 1, nullptr, &value);
                EXPECT_EQ(value, GL_SIGNALED);
            }
            const SplitRuntimeState after = PeekSplitRuntime();
            glDeleteSync(sync);
            EXPECT_EQ(after.replyPostings - before.replyPostings, 0u)
                << "polling a signaled fence published reply-slot records (MOBILEGL_IPC_POLL_ESCALATE=0 "
                   "does this on purpose)";
            EXPECT_EQ(after.fenceRoundTrips - before.fenceRoundTrips, 0u);
            EXPECT_EQ(after.fenceLocalAnswers - before.fenceLocalAnswers, 2ull * kPolls);
        }

        // THE SERVER'S HALF: a fence the GPU finishes while the client sends nothing - it only
        // polls - is still reported. The apply thread is idle then; it must submit what the driver
        // holds and keep re-checking rather than park for ever, or the only way out of the loop is
        // the escalation. The polls are paced (10 ms apart) so the escalation cannot come first by
        // rate: 64 of them is 640 ms, far more than a clear takes. Red-once: take the idle
        // re-check out of ServerLoop and this case escalates.
        TEST_F(FencePollScenario, AFenceThatFinishesWhileTheClientOnlyPollsIsReported) {
            if (!Ready()) return;
            const std::string why = SplitRuntimeSkipReason();
            if (!why.empty()) GTEST_SKIP() << why;
            // The counters are read BEFORE the fence exists: the server may report it while
            // glFenceSync's own record is still being applied, and a client call drains the event
            // ring, so a snapshot taken after glFenceSync could already include the report and the
            // delta below would read 0 for a fence that WAS reported (seen on the verify-split lane).
            const SplitRuntimeState before = PeekSplitRuntime();
            QueueWork();
            GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            ASSERT_NE(sync, nullptr);
            const auto start = Clock::now();
            GLenum status = GL_TIMEOUT_EXPIRED;
            while ((status = glClientWaitSync(sync, 0, 0)) == GL_TIMEOUT_EXPIRED &&
                   Clock::now() - start < kLoopBound) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const SplitRuntimeState after = PeekSplitRuntime();
            glDeleteSync(sync);
            EXPECT_TRUE(status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) << status;
            EXPECT_EQ(after.fenceEscalations - before.fenceEscalations, 0u)
                << "the loop needed an escalated round trip: the server did not report the fence on its "
                   "own while the client only polled";
            EXPECT_GE(after.fenceServerReports - before.fenceServerReports, 1u);
        }

        // P10 A's PAIRED RECORD - a measurement, not a gate: it skips unless
        // MGITEST_FENCE_PACE_BENCH=1. Minecraft's shape, isolated (in the replay corpus only
        // improved-transparency-minecraft-26.3 carries timed fence waits): every frame queues work
        // and a fence, swaps, and waits - with a timeout - on the fence it made two frames earlier.
        // The same entry run with MOBILEGL_IPC_POLL_ESCALATE=0 and with the default is the A/B; it
        // prints frames, wall time, reply-slot records and the fence counters.
        TEST_F(FencePollScenario, PacedFrameFenceWaitsBench) {
            if (!Ready()) return;
            const char* bench = std::getenv("MGITEST_FENCE_PACE_BENCH");
            if (bench == nullptr || std::string(bench) != "1") {
                GTEST_SKIP() << "a measurement: set MGITEST_FENCE_PACE_BENCH=1";
            }
            const std::string why = SplitRuntimeSkipReason();
            if (!why.empty()) GTEST_SKIP() << why;
            constexpr int kFrames = 600;
            // MGITEST_FENCE_PACE_LAG: how many frames back the waited fence is (default 2).
            const char* lagEnv = std::getenv("MGITEST_FENCE_PACE_LAG");
            const size_t lag = lagEnv != nullptr && std::atoi(lagEnv) > 0 ? static_cast<size_t>(std::atoi(lagEnv)) : 2;
            std::deque<GLsync> inFlight;
            const SplitRuntimeState before = PeekSplitRuntime();
            const auto start = Clock::now();
            for (int frame = 0; frame < kFrames; ++frame) {
                QueueWork();
                inFlight.push_back(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
                Gl().EndFrame();
                if (inFlight.size() > lag) {
                    GLsync old = inFlight.front();
                    inFlight.pop_front();
                    const GLenum waited = glClientWaitSync(old, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
                    EXPECT_TRUE(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED) << waited;
                    glDeleteSync(old);
                }
            }
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            const SplitRuntimeState after = PeekSplitRuntime();
            for (GLsync sync : inFlight) glDeleteSync(sync);
            const char* escalate = std::getenv("MOBILEGL_IPC_POLL_ESCALATE");
            std::printf("P10BENCH transport=%s poll-escalate=%s lag=%zu frames=%d ms=%.1f ms/frame=%.3f "
                        "reply-records=%llu fence-round-trips=%llu fence-local=%llu fence-reports=%llu\n",
                        after.transportName.c_str(), escalate != nullptr ? escalate : "(default)", lag, kFrames, ms,
                        ms / kFrames, after.replyPostings - before.replyPostings,
                        after.fenceRoundTrips - before.fenceRoundTrips,
                        after.fenceLocalAnswers - before.fenceLocalAnswers,
                        after.fenceServerReports - before.fenceServerReports);
        }

    } // namespace
} // namespace MGITest
