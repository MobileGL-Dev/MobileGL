// MobileGL - MobileGL/MG_Test/Util/JobNodeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "Includes.h"
#include <Config.h>

#include <MG_Util/Async/JobNode.h>
#include <MG_Util/Async/ShaderCompilePool.h>

using namespace MobileGL;
using namespace MobileGL::MG_Util::Async;

namespace {
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
    ShaderCompilePool pool(kTestThreads);
    EXPECT_EQ(pool.GetThreadCount(), kTestThreads);
    EXPECT_EQ(pool.GetMaxConcurrency(), kTestThreads);
    // Nothing observable to assert about thread creation from here; what this pins is that
    // construction is side-effect free and the pool destructs cleanly without ever running.
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

TEST(ShaderCompilePoolLifecycle, AsyncIsOffByDefaultAndTheOverrideDecidesEitherWay) {
    // The shipped default is still off, and an unset MOBILEGL_ASYNC_SHADER_COMPILE resolves
    // to it. If the first expectation ever fails without the constant having been
    // deliberately flipped, something enabled async by accident.
    //
    // Driven through Features rather than read from it: from stage 3 on, the whole suite is
    // also run with MOBILEGL_ASYNC_SHADER_COMPILE=1 exported, so a test that simply asserted
    // "the resolved answer is false" would either fail there or - worse - silently pass in a
    // binary that never loaded the config and prove nothing at all.
    EXPECT_FALSE(kAsyncShaderCompileDefault);

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
    constexpr Uint kJobs = 1000;
    ShaderCompilePool pool(kTestThreads);
    pool.SetMaxConcurrency(1); // keep the vast majority queued behind the budget

    Vector<SharedPtr<TestJob>> jobs;
    jobs.reserve(kJobs);
    for (Uint i = 0; i < kJobs; ++i) {
        jobs.push_back(MakeShared<TestJob>());
        pool.Post(jobs.back());
    }

    pool.StopAndDrain();

    // Every node is terminal, so nothing can be waiting on a worker that will never come.
    Uint complete = 0;
    Uint cancelled = 0;
    for (const auto& job : jobs) {
        ASSERT_TRUE(job->IsTerminal());
        if (job->IsComplete()) ++complete;
        if (job->IsCancelled()) ++cancelled;
        EXPECT_LE(job->ran.load(), 1u);
    }
    EXPECT_EQ(complete + cancelled, kJobs);
    EXPECT_GT(cancelled, 0u); // the drain really did abandon queued work rather than run it
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
