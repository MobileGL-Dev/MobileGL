// MobileGL - MobileGL/MG_Test/Util/JobNodeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>
#define MGL_TEST_GETPID _getpid
#else
#include <unistd.h>
#define MGL_TEST_GETPID getpid
#endif

#include "Includes.h"
#include <Config.h>

#include <MG_Util/Async/JobNode.h>
#include <MG_Util/Async/ShaderCompilePool.h>

using namespace MobileGL;
using namespace MobileGL::MG_Util::Async;

namespace {
    // Where this binary's MobileGL log lands, set by main() below. The engine-selection cases
    // read it back: MobileGL's desktop log sink is the FILE, not the console
    // (MOBILEGL_LOG_ENABLE_CONSOLE is 0 in Defines.h), so gtest's stdout capture would see
    // nothing, and "unrecognized value warns" is a contract worth pinning rather than
    // assuming - a silent fallback makes a misspelt engine name look exactly like an unset
    // variable.
    String g_logFilePath;

    // Log.cpp flushes the file after every line, so everything written before this call is
    // already visible.
    String ReadLogFrom(const std::streamoff offset) {
        std::ifstream file(g_logFilePath, std::ios::binary);
        if (!file) return {};
        file.seekg(offset);
        return String((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::streamoff LogSize() {
        std::error_code error;
        const auto size = std::filesystem::file_size(g_logFilePath, error);
        return error ? 0 : static_cast<std::streamoff>(size);
    }

    // Every test drives its own pool instance rather than ShaderCompilePool::Get(): the
    // process-wide pool is stopped permanently by StopAndDrain (that is the teardown
    // contract), so a test that drained the singleton would poison every test after it.
    constexpr Uint kTestThreads = 4;

    // A job whose body does exactly what the test tells it to. `ran` counts executions so
    // "enqueued once, ran once" is checkable, and the optional gate lets a test hold a job
    // inside its body while it inspects the node from the outside.
    class TestJob final : public JobNode {
    public:
        explicit TestJob(std::function<void(TestJob&)> body = {}) : m_body(Move(body)) {}

        std::atomic<Uint> ran{0};
        std::atomic<Bool> observedCancelledInBody{false};
        std::atomic<Bool> observedCancelledStateInBody{false};

    protected:
        void RunBody() override {
            ran.fetch_add(1, std::memory_order_acq_rel);
            if (m_body) m_body(*this);
            observedCancelledInBody.store(IsCancellationRequested(), std::memory_order_release);
            // A running body sees the request, not the outcome: the node is still Running
            // until it returns, which is exactly the cooperative contract.
            observedCancelledStateInBody.store(IsCancelled(), std::memory_order_release);
        }

    private:
        std::function<void(TestJob&)> m_body;
    };

    // A manual gate, so a test can pin a job in Running and observe the node meanwhile.
    class Gate {
    public:
        void Open() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_open = true;
            }
            m_cv.notify_all();
        }

        void Wait() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_open; });
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        Bool m_open = false;
    };

    // Live thread count of this process. Linux only - /proc/self/task has one entry per
    // thread - and 0 where that is not available, which is how the one case that uses it
    // decides to skip rather than to assert something it cannot see.
    SizeT LiveThreadCount() {
#ifdef __linux__
        std::error_code error;
        const auto count = static_cast<SizeT>(
            std::distance(std::filesystem::directory_iterator("/proc/self/task", error),
                          std::filesystem::directory_iterator()));
        return error ? 0 : count;
#else
        return 0;
#endif
    }

    Bool WaitUntil(const std::function<Bool()>& predicate,
                   const std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }
} // namespace

// ---------------------------------------------------------------------------------------
// Pool lifecycle
// ---------------------------------------------------------------------------------------

TEST(ShaderCompilePoolLifecycle, ConstructingAPoolStartsNoThreadUntilSomethingIsPosted) {
    const SizeT before = LiveThreadCount();

    ShaderCompilePool pool(kTestThreads);
    EXPECT_EQ(pool.GetThreadCount(), kTestThreads);
    EXPECT_EQ(pool.GetMaxConcurrency(), kTestThreads);

    if (before == 0) {
        // No thread census on this platform. The rest still holds: construction is
        // side-effect free and the pool destructs cleanly without ever having run.
        SUCCEED();
        return;
    }

    // "A build that never posts pays nothing" is a real requirement, not a stylistic one -
    // asynchronous compilation can be switched off entirely, and a switched-off pool that
    // still spawned its workers would cost every such process its threads and their stacks.
    // Worth asserting rather than asserting-by-comment now that an engine's thread shape is
    // selectable: the libfork engine starts its workers AND a dispatch thread of its own, so
    // a regression here would cost more than it used to.
    EXPECT_EQ(LiveThreadCount(), before) << "constructing a pool started " << (LiveThreadCount() - before)
                                         << " thread(s) before anything was posted";

    auto job = MakeShared<TestJob>();
    pool.Post(job);
    job->Wait();
    EXPECT_GT(LiveThreadCount(), before) << "the first Post started no thread at all, so the engine did not "
                                            "really run the job off the calling thread";
}

TEST(ShaderCompilePoolLifecycle, StopAndDrainIsIdempotentAndSafeOnAnUnusedPool) {
    ShaderCompilePool pool(kTestThreads);
    pool.StopAndDrain();
    pool.StopAndDrain();
    SUCCEED();
}

TEST(ShaderCompilePoolLifecycle, AStoppedPoolRunsPostedJobsInlineOnTheCallingThread) {
    ShaderCompilePool pool(kTestThreads);
    pool.StopAndDrain();

    const auto callingThread = std::this_thread::get_id();
    std::thread::id bodyThread{};
    auto job = MakeShared<TestJob>([&](TestJob&) { bodyThread = std::this_thread::get_id(); });

    pool.Post(job);

    // Terminal by the time Post returned - the whole point of the stopped-is-synchronous
    // rule: a late entry point after teardown still gets a correct result, it just gets it
    // without resurrecting a worker thread.
    EXPECT_TRUE(job->IsComplete());
    EXPECT_EQ(job->ran.load(), 1u);
    EXPECT_EQ(bodyThread, callingThread);
}

TEST(ShaderCompilePoolLifecycle, SetMaxConcurrencyIsClampedToTheThreadCount) {
    ShaderCompilePool pool(kTestThreads);
    pool.SetMaxConcurrency(0);
    EXPECT_EQ(pool.GetMaxConcurrency(), 1u);
    pool.SetMaxConcurrency(1000);
    EXPECT_EQ(pool.GetMaxConcurrency(), kTestThreads);
    pool.SetMaxConcurrency(2);
    EXPECT_EQ(pool.GetMaxConcurrency(), 2u);
}

TEST(ShaderCompilePoolLifecycle, DetectedThreadCountIsPositive) {
    EXPECT_GE(DetectShaderCompileThreadCount(), 1u);
}

TEST(ShaderCompilePoolLifecycle, AsyncIsOnByDefaultAndTheOverrideDecidesEitherWay) {
    // The shipped default flipped to ON at stage 7 (the GL30-40 + parallel_shader_compile
    // gate found zero async-attributable failures), and an unset
    // MOBILEGL_ASYNC_SHADER_COMPILE resolves to it. If the first expectation ever fails
    // without the constant having been deliberately flipped back, something disabled async
    // by accident - the kill switch below is the supported way off.
    //
    // Driven through Features rather than read from it: the suite is also run with
    // MOBILEGL_ASYNC_SHADER_COMPILE exported both ways, so a test that simply asserted
    // the resolved answer would fail in one of those runs or - worse - silently pass in a
    // binary that never loaded the config and prove nothing at all.
    EXPECT_TRUE(kAsyncShaderCompileDefault);

    const MG_Config::QuirkOverride saved = MG_Config::Features.AsyncShaderCompile;
    MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::Auto;
    EXPECT_EQ(AsyncShaderCompileEnabled(), kAsyncShaderCompileDefault);
    MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::ForceOn;
    EXPECT_TRUE(AsyncShaderCompileEnabled());
    MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::ForceOff;
    EXPECT_FALSE(AsyncShaderCompileEnabled());
    MG_Config::Features.AsyncShaderCompile = saved;
}

// ---------------------------------------------------------------------------------------
// Submit and join
// ---------------------------------------------------------------------------------------

TEST(JobNodeSubmit, PostedJobRunsOnAPoolThreadAndWaitJoinsIt) {
    ShaderCompilePool pool(kTestThreads);

    std::atomic<Bool> sawPoolThread{false};
    auto job = MakeShared<TestJob>(
        [&](TestJob&) { sawPoolThread.store(ShaderCompilePool::IsPoolThread(), std::memory_order_release); });

    pool.Post(job);
    job->Wait();

    EXPECT_TRUE(job->IsTerminal());
    EXPECT_TRUE(job->IsComplete());
    EXPECT_FALSE(job->IsCancelled());
    EXPECT_EQ(job->ran.load(), 1u);
    EXPECT_TRUE(sawPoolThread.load());
    // The joining thread is not a pool thread - the assert inside Wait() depends on it.
    EXPECT_FALSE(ShaderCompilePool::IsPoolThread());
}

TEST(JobNodeSubmit, WaitOnAnAlreadyTerminalJobReturnsImmediately) {
    ShaderCompilePool pool(kTestThreads);
    auto job = MakeShared<TestJob>();
    pool.Post(job);
    job->Wait();
    job->Wait();
    EXPECT_EQ(job->ran.load(), 1u);
}

TEST(JobNodeSubmit, RunInlineExecutesOnTheCallingThreadWithoutAPool) {
    auto job = MakeShared<TestJob>();
    job->RunInline();
    EXPECT_TRUE(job->IsComplete());
    EXPECT_EQ(job->ran.load(), 1u);
}

TEST(JobNodeSubmit, ManyJobsAllComplete) {
    constexpr Uint kJobs = 256;
    ShaderCompilePool pool(kTestThreads);

    Vector<SharedPtr<TestJob>> jobs;
    jobs.reserve(kJobs);
    std::atomic<Uint> completed{0};
    for (Uint i = 0; i < kJobs; ++i) {
        jobs.push_back(MakeShared<TestJob>([&](TestJob&) { completed.fetch_add(1, std::memory_order_acq_rel); }));
        pool.Post(jobs.back());
    }

    for (const auto& job : jobs) job->Wait();
    EXPECT_EQ(completed.load(), kJobs);
    for (const auto& job : jobs) {
        EXPECT_TRUE(job->IsComplete());
        EXPECT_EQ(job->ran.load(), 1u);
    }
}

TEST(JobNodeSubmit, AJobBodyMayPostAnotherJobToTheSamePool) {
    // The ProgramLinkTask::SubmitAfter shape, reduced to its scheduling core: the dependent is
    // posted by whichever thread drove the dependency terminal, which for a job that finished
    // on a worker is that WORKER. Every engine therefore has to accept a submission from
    // inside its own pool.
    //
    // Not a hypothetical: libfork refuses this outright at its normal entry point
    // (lf::schedule throws lf::schedule_in_worker, because a libfork worker may never block),
    // which is why the libfork engine owns a dispatch thread of its own. Without this case a
    // naive port passes every other test in the file and turns every dependency-released link
    // job into a cancelled one on the real GL path.
    ShaderCompilePool pool(kTestThreads);

    std::atomic<Bool> innerSawPoolThread{false};
    auto inner = MakeShared<TestJob>(
        [&](TestJob&) { innerSawPoolThread.store(ShaderCompilePool::IsPoolThread(), std::memory_order_release); });

    std::atomic<Bool> postedFromPoolThread{false};
    auto outer = MakeShared<TestJob>([&](TestJob&) {
        postedFromPoolThread.store(ShaderCompilePool::IsPoolThread(), std::memory_order_release);
        pool.Post(inner);
    });

    pool.Post(outer);
    outer->Wait();
    inner->Wait();

    EXPECT_TRUE(postedFromPoolThread.load()) << "the outer body did not run on a pool thread, so this case "
                                                "did not exercise posting from inside the pool";
    EXPECT_TRUE(outer->IsComplete());
    // The load-bearing one: the inner job RAN. A dispatch the engine refused would have
    // settled it Cancelled instead, and its body would never have executed.
    EXPECT_TRUE(inner->IsComplete()) << "a job posted from a pool thread was not dispatched";
    EXPECT_FALSE(inner->IsCancelled());
    EXPECT_EQ(inner->ran.load(), 1u);
    EXPECT_TRUE(innerSawPoolThread.load());
}

TEST(JobNodeSubmit, ABurstPostedFromInsideThePoolStillRunsInParallel) {
    // The tail of a pack load: one compile job goes terminal and its continuations release
    // several programs at once (ShaderCompileAdoptionMap lets one compile settle many), so a
    // WORKER posts a burst into a pool that is otherwise idle. Every one of those posts clears
    // the budget immediately, so the engine is handed `kBurst` runnable jobs from inside
    // itself - and it has to spread them, not run them one behind another on the thread that
    // submitted them.
    //
    // Asserting on peak concurrency rather than on wall time: the budget is the contract, and
    // an engine that dispatches within the budget but executes serially has silently turned
    // the budget into an upper bound nothing reaches.
    constexpr Uint kBurst = 4; // == kTestThreads, so the budget can hold all of them at once
    ShaderCompilePool pool(kTestThreads);

    std::atomic<Uint> live{0};
    std::atomic<Uint> peak{0};
    std::atomic<Uint> finished{0};

    Vector<SharedPtr<TestJob>> burst;
    burst.reserve(kBurst);
    for (Uint i = 0; i < kBurst; ++i) {
        burst.push_back(MakeShared<TestJob>([&](TestJob&) {
            const Uint now = live.fetch_add(1, std::memory_order_acq_rel) + 1;
            Uint seen = peak.load(std::memory_order_acquire);
            while (now > seen && !peak.compare_exchange_weak(seen, now, std::memory_order_acq_rel)) {
            }
            // Long enough that a serial engine cannot fake overlap, short enough to keep the
            // case cheap: with any real spread every body is inside this window together.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            live.fetch_sub(1, std::memory_order_acq_rel);
            finished.fetch_add(1, std::memory_order_acq_rel);
        }));
    }

    std::atomic<Bool> postedFromPoolThread{false};
    auto seeder = MakeShared<TestJob>([&](TestJob&) {
        postedFromPoolThread.store(ShaderCompilePool::IsPoolThread(), std::memory_order_release);
        for (const auto& job : burst) pool.Post(job);
    });

    pool.Post(seeder);
    seeder->Wait();
    for (const auto& job : burst) job->Wait();

    ASSERT_TRUE(postedFromPoolThread.load()) << "the burst was not posted from a pool thread";
    EXPECT_EQ(finished.load(), kBurst);
    EXPECT_GT(peak.load(), 1u) << "a burst posted from inside the pool ran strictly one at a time; the "
                                  "engine serialized work the budget had already cleared";
}

TEST(JobNodeSubmit, ConcurrencyBudgetIsNeverExceeded) {
    constexpr Uint kBudget = 2;
    constexpr Uint kJobs = 64;
    ShaderCompilePool pool(kTestThreads);
    pool.SetMaxConcurrency(kBudget);

    std::atomic<Uint> inFlight{0};
    std::atomic<Uint> peak{0};

    Vector<SharedPtr<TestJob>> jobs;
    jobs.reserve(kJobs);
    for (Uint i = 0; i < kJobs; ++i) {
        jobs.push_back(MakeShared<TestJob>([&](TestJob&) {
            const Uint current = inFlight.fetch_add(1, std::memory_order_acq_rel) + 1;
            Uint observed = peak.load(std::memory_order_acquire);
            while (current > observed && !peak.compare_exchange_weak(observed, current)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            inFlight.fetch_sub(1, std::memory_order_acq_rel);
        }));
        pool.Post(jobs.back());
    }

    for (const auto& job : jobs) job->Wait();
    // This is also the memory bound: it is what stops a 300-program pack load from putting
    // 300 glslang arenas in flight at once.
    EXPECT_LE(peak.load(), kBudget);
    EXPECT_GE(peak.load(), 1u);
}

// ---------------------------------------------------------------------------------------
// OnTerminal and dependency ordering
// ---------------------------------------------------------------------------------------

TEST(JobNodeContinuation, OnTerminalOnAnAlreadyTerminalNodeRunsInlineBeforeItReturns) {
    auto job = MakeShared<TestJob>();
    job->RunInline();
    ASSERT_TRUE(job->IsTerminal());

    Bool ranInline = false;
    const auto callingThread = std::this_thread::get_id();
    std::thread::id continuationThread{};
    job->OnTerminal([&] {
        ranInline = true;
        continuationThread = std::this_thread::get_id();
    });

    EXPECT_TRUE(ranInline);
    EXPECT_EQ(continuationThread, callingThread);
}

TEST(JobNodeContinuation, EveryContinuationFiresExactlyOnce) {
    constexpr Uint kContinuations = 8;
    ShaderCompilePool pool(kTestThreads);

    Gate gate;
    auto job = MakeShared<TestJob>([&](TestJob&) { gate.Wait(); });
    pool.Post(job);

    std::atomic<Uint> fired{0};
    for (Uint i = 0; i < kContinuations; ++i) {
        job->OnTerminal([&] { fired.fetch_add(1, std::memory_order_acq_rel); });
    }
    gate.Open();
    job->Wait();

    // Registered while the job was pending or running, so all of them are handed to the
    // finishing thread; a late one would have run inline instead. Either way: once each.
    EXPECT_TRUE(WaitUntil([&] { return fired.load() == kContinuations; }));
    EXPECT_EQ(fired.load(), kContinuations);

    // A continuation registered after the fact still fires, exactly once, inline.
    job->OnTerminal([&] { fired.fetch_add(1, std::memory_order_acq_rel); });
    EXPECT_EQ(fired.load(), kContinuations + 1);
}

TEST(JobNodeContinuation, DependencyCounterReachesZeroExactlyOnceAndOnlyAfterEveryDependency) {
    // The shape ProgramLinkTask::SubmitAfter uses: the dependent is posted by whichever
    // thread drives the counter to zero, so it is enqueued only once every dependency is
    // terminal - which is why no job body ever has to wait on another job.
    constexpr Uint kDeps = 16;
    ShaderCompilePool pool(kTestThreads);

    Vector<SharedPtr<TestJob>> deps;
    deps.reserve(kDeps);
    for (Uint i = 0; i < kDeps; ++i) deps.push_back(MakeShared<TestJob>());

    std::atomic<Int> remaining{static_cast<Int>(kDeps) + 1}; // +1 guard: nothing fires mid-registration
    std::atomic<Uint> released{0};
    std::atomic<Bool> allDepsTerminalAtRelease{false};

    const auto settle = [&] {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Bool allTerminal = true;
            for (const auto& dep : deps) allTerminal = allTerminal && dep->IsTerminal();
            allDepsTerminalAtRelease.store(allTerminal, std::memory_order_release);
            released.fetch_add(1, std::memory_order_acq_rel);
        }
    };

    for (const auto& dep : deps) {
        pool.Post(dep);
        dep->OnTerminal(settle);
    }
    settle(); // release the guard

    EXPECT_TRUE(WaitUntil([&] { return released.load() == 1u; }));
    EXPECT_EQ(released.load(), 1u);
    EXPECT_TRUE(allDepsTerminalAtRelease.load());
    for (const auto& dep : deps) EXPECT_TRUE(dep->IsComplete());
}

TEST(JobNodeContinuation, AlreadyTerminalDependenciesStillSettleTheCounterExactlyOnce) {
    // Same counter, but every dependency is terminal before registration, so every
    // continuation runs inline on the registering thread.
    constexpr Uint kDeps = 4;
    Vector<SharedPtr<TestJob>> deps;
    for (Uint i = 0; i < kDeps; ++i) {
        deps.push_back(MakeShared<TestJob>());
        deps.back()->RunInline();
    }

    std::atomic<Int> remaining{static_cast<Int>(kDeps) + 1};
    Uint released = 0;
    const auto settle = [&] {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) ++released;
    };
    for (const auto& dep : deps) dep->OnTerminal(settle);
    settle();

    EXPECT_EQ(released, 1u);
}

// ---------------------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------------------

TEST(JobNodeCancel, CancelBeforeAnyDispatchSettlesTheNodeAndSkipsTheBody) {
    ShaderCompilePool pool(kTestThreads);
    auto job = MakeShared<TestJob>();

    job->Cancel();
    EXPECT_TRUE(job->IsCancelled());
    EXPECT_TRUE(job->IsTerminal());
    EXPECT_FALSE(job->IsComplete());

    // Posting an already-cancelled node is a no-op, not a second run.
    pool.Post(job);
    job->Wait();
    EXPECT_EQ(job->ran.load(), 0u);
    EXPECT_TRUE(job->IsCancelled());
}

TEST(JobNodeCancel, CancelWhileTheBodyIsRunningLetsItFinishAndReportsCancelled) {
    ShaderCompilePool pool(kTestThreads);

    Gate gate;
    std::atomic<Bool> entered{false};
    auto job = MakeShared<TestJob>([&](TestJob&) {
        entered.store(true, std::memory_order_release);
        gate.Wait();
    });

    pool.Post(job);
    ASSERT_TRUE(WaitUntil([&] { return entered.load(); }));

    job->Cancel();
    // A running body is not interrupted - cancellation is cooperative - so the node is
    // still Running until the body returns.
    EXPECT_FALSE(job->IsTerminal());
    gate.Open();
    job->Wait();

    EXPECT_EQ(job->ran.load(), 1u);
    EXPECT_TRUE(job->IsCancelled());
    EXPECT_FALSE(job->IsComplete());
    EXPECT_TRUE(job->observedCancelledInBody.load());
    EXPECT_FALSE(job->observedCancelledStateInBody.load());
}

TEST(JobNodeCancel, CancelAfterCompletionDoesNotUndoTheResult) {
    ShaderCompilePool pool(kTestThreads);
    auto job = MakeShared<TestJob>();
    pool.Post(job);
    job->Wait();
    ASSERT_TRUE(job->IsComplete());

    job->Cancel();
    // The request is recorded, but a settled result is never retroactively undone.
    EXPECT_TRUE(job->IsCancellationRequested());
    EXPECT_TRUE(job->IsComplete());
    EXPECT_FALSE(job->IsCancelled());
    EXPECT_EQ(job->ran.load(), 1u);
}

TEST(JobNodeCancel, CancelReleasesContinuationsSoDependentsAreNotStranded) {
    auto job = MakeShared<TestJob>();
    std::atomic<Uint> fired{0};
    job->OnTerminal([&] { fired.fetch_add(1, std::memory_order_acq_rel); });

    job->Cancel();

    EXPECT_EQ(fired.load(), 1u);
    job->Wait(); // must not hang: a cancelled pending node is terminal
    EXPECT_TRUE(job->IsCancelled());
}

// ---------------------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------------------

TEST(JobNodeException, AnExceptionEscapingABodyCancelsTheJobInsteadOfTerminating) {
    // Asio propagates an exception out of thread_pool::run(), which is std::terminate for
    // the process. Containing it at the job boundary is what makes that impossible.
    ShaderCompilePool pool(kTestThreads);
    auto job = MakeShared<TestJob>([](TestJob&) { throw std::runtime_error("boom"); });

    pool.Post(job);
    job->Wait();

    EXPECT_TRUE(job->IsTerminal());
    EXPECT_TRUE(job->IsCancelled());
    EXPECT_FALSE(job->IsComplete());
    ASSERT_EQ(job->diagnostics.logLines.size(), 1u);
    EXPECT_NE(job->diagnostics.logLines[0].find("boom"), String::npos);
}

TEST(JobNodeException, ANonStandardExceptionIsContainedToo) {
    ShaderCompilePool pool(kTestThreads);
    auto job = MakeShared<TestJob>([](TestJob&) { throw 42; });

    pool.Post(job);
    job->Wait();

    EXPECT_TRUE(job->IsCancelled());
    ASSERT_EQ(job->diagnostics.logLines.size(), 1u);
}

TEST(JobNodeException, AThrowingJobDoesNotPoisonTheWorkerForLaterJobs) {
    ShaderCompilePool pool(kTestThreads);
    auto thrower = MakeShared<TestJob>([](TestJob&) { throw std::runtime_error("boom"); });
    pool.Post(thrower);
    thrower->Wait();

    auto healthy = MakeShared<TestJob>();
    pool.Post(healthy);
    healthy->Wait();
    EXPECT_TRUE(healthy->IsComplete());
}

// ---------------------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------------------

TEST(ShaderCompilePoolDrain, StopAndDrainWithAThousandQueuedJobsLeavesNoneRunningOrPending) {
    constexpr Uint kQueued = 1000;
    ShaderCompilePool pool(kTestThreads);
    pool.SetMaxConcurrency(1); // one slot, so everything behind the first job stays queued

    // Pin that slot with a job that will not return until this test says so. Everything
    // posted behind it is then PROVABLY still in the queue, which is what makes the counts
    // below exact.
    //
    // This case used to post a thousand trivial jobs and drain immediately, hoping the drain
    // would beat the workers to some of them - and then assert only that "some" were
    // cancelled. That hope does not survive an engine whose workers take their next job
    // without a scheduler round trip: the libfork engine drained all thousand before the
    // posting loop had finished, so the assertion failed about one run in fifty. The property
    // being tested (a drain ABANDONS queued work rather than running it) is real and
    // engine-independent; only the way it was provoked was a race.
    Gate gate;
    std::atomic<Bool> entered{false};
    auto blocker = MakeShared<TestJob>([&](TestJob&) {
        entered.store(true, std::memory_order_release);
        gate.Wait();
    });
    pool.Post(blocker);
    ASSERT_TRUE(WaitUntil([&] { return entered.load(); }));

    Vector<SharedPtr<TestJob>> queued;
    queued.reserve(kQueued);
    for (Uint i = 0; i < kQueued; ++i) {
        queued.push_back(MakeShared<TestJob>());
        pool.Post(queued.back());
    }
    for (const auto& job : queued) ASSERT_FALSE(job->IsTerminal());

    std::thread drain([&] { pool.StopAndDrain(); });
    // StopAndDrain settles the entire queue before it waits for the running body, so the
    // first cancelled node proves it is past that point - and the gate can then be released
    // without racing it.
    ASSERT_TRUE(WaitUntil([&] { return queued.front()->IsTerminal(); }));
    gate.Open();
    drain.join();

    // The job that was already running still finished: an in-flight body is waited for, not
    // interrupted.
    EXPECT_TRUE(blocker->IsComplete());
    EXPECT_EQ(blocker->ran.load(), 1u);

    // And every queued node is terminal, so nothing is left waiting on a worker that will
    // never come - settled as cancelled, with its body never entered.
    for (const auto& job : queued) {
        ASSERT_TRUE(job->IsTerminal());
        EXPECT_TRUE(job->IsCancelled());
        EXPECT_EQ(job->ran.load(), 0u);
    }
}

TEST(ShaderCompilePoolDrain, StopAndDrainWaitsForARunningBodyToReturn) {
    ShaderCompilePool pool(kTestThreads);

    Gate gate;
    std::atomic<Bool> entered{false};
    std::atomic<Bool> left{false};
    auto job = MakeShared<TestJob>([&](TestJob&) {
        entered.store(true, std::memory_order_release);
        gate.Wait();
        left.store(true, std::memory_order_release);
    });

    pool.Post(job);
    ASSERT_TRUE(WaitUntil([&] { return entered.load(); }));

    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate.Open();
    });
    pool.StopAndDrain();
    opener.join();

    // This is the guarantee library teardown relies on: once StopAndDrain returns, no worker
    // is still inside a body that could touch glslang's process globals.
    EXPECT_TRUE(left.load());
    EXPECT_TRUE(job->IsTerminal());
}

TEST(ShaderCompilePoolDrain, JobsPostedAfterADrainStillRun) {
    ShaderCompilePool pool(kTestThreads);
    pool.StopAndDrain();

    auto job = MakeShared<TestJob>();
    pool.Post(job);
    EXPECT_TRUE(job->IsComplete());
    EXPECT_EQ(job->ran.load(), 1u);
}

// ---------------------------------------------------------------------------------------
// Execution engine selection (MOBILEGL_ASYNC_POOL)
// ---------------------------------------------------------------------------------------
//
// The engine decides only HOW a job that the concurrency budget has already cleared reaches a
// worker thread. Everything else in this file - the budget, cancel request-vs-outcome, the
// continuation machinery, the inline fallback after a stop, the drain - is engine-independent
// by construction, which is why the whole suite is expected to pass unchanged with
// MOBILEGL_ASYNC_POOL unset and with it set to libfork. These cases pin the selection itself,
// so that a run of the matrix cannot silently test asio twice.

TEST(AsyncPoolEngineSelection, EveryAcceptedSpellingParsesToItsEngine) {
    EXPECT_EQ(ParseAsyncPoolEngine("asio"), AsyncPoolEngine::Asio);
    EXPECT_EQ(ParseAsyncPoolEngine("libfork"), AsyncPoolEngine::Libfork);
    // Case-insensitive, like the other named-value variables (MOBILEGL_*_MULTIDRAW_MODE).
    EXPECT_EQ(ParseAsyncPoolEngine("Libfork"), AsyncPoolEngine::Libfork);
    EXPECT_EQ(ParseAsyncPoolEngine("LIBFORK"), AsyncPoolEngine::Libfork);
    EXPECT_EQ(ParseAsyncPoolEngine("ASIO"), AsyncPoolEngine::Asio);

    EXPECT_STREQ(AsyncPoolEngineName(AsyncPoolEngine::Asio), "asio");
    EXPECT_STREQ(AsyncPoolEngineName(AsyncPoolEngine::Libfork), "libfork");
    // Round trip: whatever the name prints is a spelling the variable accepts back.
    EXPECT_EQ(ParseAsyncPoolEngine(AsyncPoolEngineName(AsyncPoolEngine::Asio)), AsyncPoolEngine::Asio);
    EXPECT_EQ(ParseAsyncPoolEngine(AsyncPoolEngineName(AsyncPoolEngine::Libfork)), AsyncPoolEngine::Libfork);
}

TEST(AsyncPoolEngineSelection, EmptyAndAutoAreTheDefaultEngineAndSaySoSilently) {
    // Unset resolves through the empty string, and "auto" is the spelling the other named
    // -value variables accept for "no preference". Neither is a mistake, so neither warns.
    const std::streamoff before = LogSize();
    EXPECT_EQ(ParseAsyncPoolEngine(""), AsyncPoolEngine::Asio);
    EXPECT_EQ(ParseAsyncPoolEngine("auto"), AsyncPoolEngine::Asio);
    EXPECT_EQ(ReadLogFrom(before).find("MOBILEGL_ASYNC_POOL"), String::npos)
        << "a legitimate value warned; only an unrecognized one may";
}

TEST(AsyncPoolEngineSelection, AnUnrecognizedEngineNameFallsBackToAsioAndWarns) {
    const std::streamoff before = LogSize();
    EXPECT_EQ(ParseAsyncPoolEngine("libfrok"), AsyncPoolEngine::Asio);

// The warning is the other half of the contract: a misspelt engine name that fell back
// silently would be indistinguishable from an unset variable, and a scaling measurement
// taken against the wrong engine is worse than no measurement.
//
// Guarded because MGLOG_W is a compile-time no-op unless the build's log level admits it -
// and the shipped level does not (Log.h orders the levels DEBUG=0, WARN=1, ERROR=2, INFO=3,
// FATAL=4 and gates on `ACTIVE <= LEVEL`, so the default INFO build enables only INFO and
// FATAL). Nothing is skipped: the fallback above is pinned in every build, and this half is
// checked by a build configured with
// -DMOBILEGL_LOG_ACTIVE_LEVEL=MOBILEGL_LOG_LEVEL_WARN. The same guard is what makes the
// preceding "says so silently" case honest rather than vacuously true.
#if MOBILEGL_LOG_ACTIVE_LEVEL <= MOBILEGL_LOG_LEVEL_WARN
    const String logged = ReadLogFrom(before);
    EXPECT_NE(logged.find("MOBILEGL_ASYNC_POOL"), String::npos) << "no warning names the variable; log tail: " << logged;
    EXPECT_NE(logged.find("libfrok"), String::npos)
        << "the warning does not quote the rejected value; log tail: " << logged;
    EXPECT_NE(logged.find("asio"), String::npos)
        << "the warning does not say what it fell back to; log tail: " << logged;
#else
    (void)before;
#endif
}

TEST(AsyncPoolEngineSelection, TheDetectedEngineIsTheOneTheEnvironmentAskedFor) {
    // Read the variable directly rather than through the pool, so this really compares the
    // process's answer against the environment the runner exported. This is the case that
    // makes "the suite passed with MOBILEGL_ASYNC_POOL=libfork" mean something.
    const char* const raw = std::getenv("MOBILEGL_ASYNC_POOL");
    const AsyncPoolEngine expected = ParseAsyncPoolEngine(raw != nullptr ? String(raw) : String());
    EXPECT_EQ(DetectAsyncPoolEngine(), expected);

    // Stable: resolved once per process, so it cannot drift between calls.
    EXPECT_EQ(DetectAsyncPoolEngine(), DetectAsyncPoolEngine());

    if (DetectAsyncPoolEngine() != AsyncPoolEngine::Asio) {
        // Selecting a non-default engine announces itself at INFO, which the shipped log level
        // does admit - so on the libfork half of the matrix this doubles as the positive
        // control for the log plumbing the preceding two cases read: it proves
        // MOBILEGL_LOG_FILE_PATH took effect and that ReadLogFrom really sees MobileGL's
        // output, rather than passing because the file is always empty.
        const String logged = ReadLogFrom(0);
        EXPECT_NE(logged.find("MOBILEGL_ASYNC_POOL"), String::npos)
            << "the selected engine was never announced, so this binary's log capture proves nothing";
        EXPECT_NE(logged.find(AsyncPoolEngineName(DetectAsyncPoolEngine())), String::npos);
    }
}

TEST(AsyncPoolEngineSelection, EveryPoolReportsTheProcessEngineAndRunsWorkOnIt) {
    ShaderCompilePool first(kTestThreads);
    ShaderCompilePool second(kTestThreads);
    EXPECT_EQ(first.GetEngine(), DetectAsyncPoolEngine());
    EXPECT_EQ(second.GetEngine(), first.GetEngine())
        << "two pools in one process disagree about the engine; a process must never run both";

    // And the engine it reports is the one that actually executed the work: the body ran off
    // the calling thread, on a thread the pool owns.
    const auto callingThread = std::this_thread::get_id();
    std::atomic<Bool> sawPoolThread{false};
    std::thread::id bodyThread{};
    auto job = MakeShared<TestJob>([&](TestJob&) {
        sawPoolThread.store(ShaderCompilePool::IsPoolThread(), std::memory_order_release);
        bodyThread = std::this_thread::get_id();
    });
    first.Post(job);
    job->Wait();

    ASSERT_TRUE(job->IsComplete());
    EXPECT_TRUE(sawPoolThread.load());
    EXPECT_NE(bodyThread, callingThread);
}

// gtest_main is replaced here for one reason: the engine-selection cases above assert that an
// unrecognized MOBILEGL_ASYNC_POOL value WARNS, and MobileGL's desktop log sink is the log
// file - MOBILEGL_LOG_ENABLE_CONSOLE is 0 in Defines.h, so there is nothing on stdout to
// capture. MOBILEGL_LOG_FILE_PATH is read by Log.cpp's InitFile() at the first log write in
// the process, so it has to be set before any test body runs.
int main(int argc, char** argv) {
    const std::filesystem::path logPath =
        std::filesystem::temp_directory_path() /
        ("mobilegl-jobnodetest-" + std::to_string(static_cast<long long>(MGL_TEST_GETPID())) + ".log");
    g_logFilePath = logPath.string();
    std::filesystem::remove(logPath);
#ifdef _WIN32
    ::_putenv_s("MOBILEGL_LOG_FILE_PATH", g_logFilePath.c_str());
#else
    ::setenv("MOBILEGL_LOG_FILE_PATH", g_logFilePath.c_str(), 1);
#endif

    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();

    // Best-effort: leaving a log file per test process in the temp directory would be litter,
    // and a failed run has already printed the tail it needed into the gtest output.
    std::error_code ignored;
    std::filesystem::remove(logPath, ignored);
    return result;
}
