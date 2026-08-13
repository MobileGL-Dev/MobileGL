// MobileGL - MobileGL/MG_Util/Async/ShaderCompilePool.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderCompilePool.h"
#include <Config.h>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <cstdio>
#include <deque>

namespace MobileGL::MG_Util::Async {
    namespace {
        // The memory ceiling, not a throughput guess: peak RSS during a pack load scales as
        // (workers x largest glslang arena), and a shaderpack stage arena is large enough
        // that four concurrent ones is already as much as a phone should be asked for.
        constexpr Uint kMaxAutoShaderCompileThreads = 4;

        // A core counts as "big" if its cpufreq ceiling is within 15% of the fastest core's.
        // On a symmetric desktop that is every core; on a big.LITTLE phone it selects the
        // cluster the GL thread itself runs on.
        constexpr Uint64 kBigCoreFrequencyPercent = 85;

        thread_local Bool tl_isPoolThread = false;

        // Mirrors DirectGLES's InProcessTeardown()/EnsureProcessTeardownSentinel(): once the
        // process has entered exit(), starting a worker thread is unsafe (cross-translation
        // -unit static destruction order is unspecified, and glslang's process globals may
        // already be gone). The flag is latched by an atexit handler registered lazily on
        // first pool use, so it is guaranteed to run before any static destructor.
        Bool g_processTeardown = false;
        std::once_flag g_teardownSentinelOnce;
        // The process-wide pool from Get(), for the atexit handler to stop. Never the
        // stack-allocated pools a test builds - those join themselves in their destructor.
        std::atomic<ShaderCompilePool*> g_processPool{nullptr};

        Bool InProcessTeardown() { return g_processTeardown; }

        void EnsureProcessTeardownSentinel() {
            std::call_once(g_teardownSentinelOnce, [] {
                std::atexit(+[] {
                    g_processTeardown = true;
                    // Latching the flag is not enough: a worker that is ALREADY inside
                    // glslang has to be out of it before static destruction reaches
                    // glslang's process globals, the SPIRV-Tools tables, or anything else a
                    // job body touches. This is the same wait Init.cpp's DestroyImpl does -
                    // it just also has to happen for a process that exits without ever
                    // calling eglTerminate, which is the norm for a test binary and legal
                    // for an application. Registered here, during main, so it runs before
                    // the destructors of statics constructed at load time.
                    ShaderCompilePool::StopAndDrainProcessPoolAtExit();
                });
            });
        }

        Uint64 ReadCpuMaxFrequencyKHz(const Uint cpu) {
            const String path =
                std::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", cpu);
            std::FILE* file = std::fopen(path.c_str(), "r");
            if (file == nullptr) return 0;
            unsigned long long value = 0;
            const int scanned = std::fscanf(file, "%llu", &value);
            std::fclose(file);
            return scanned == 1 ? static_cast<Uint64>(value) : 0;
        }

        Uint DetectBigCoreCount() {
            const Uint cpuCount = std::max(1u, std::thread::hardware_concurrency());

            Vector<Uint64> frequencies;
            frequencies.reserve(cpuCount);
            for (Uint cpu = 0; cpu < cpuCount; ++cpu) {
                const Uint64 frequency = ReadCpuMaxFrequencyKHz(cpu);
                if (frequency == 0) break;
                frequencies.push_back(frequency);
            }

            // Windows, macOS, and containers that hide the cpufreq tree land here, as does a
            // partially readable tree: with no asymmetry information the honest answer is
            // "every core is a big core", and the [1, 4] clamp bounds it anyway.
            if (frequencies.size() != cpuCount) return cpuCount;

            const Uint64 peak = *std::max_element(frequencies.begin(), frequencies.end());
            const Uint64 threshold = peak * kBigCoreFrequencyPercent / 100;
            Uint bigCores = 0;
            for (const Uint64 frequency : frequencies) {
                if (frequency >= threshold) ++bigCores;
            }
            return bigCores > 0 ? bigCores : cpuCount;
        }
    } // namespace

    Bool AsyncShaderCompileEnabled() {
        switch (MG_Config::Features.AsyncShaderCompile) {
        case MG_Config::QuirkOverride::ForceOn: return true;
        case MG_Config::QuirkOverride::ForceOff: return false;
        case MG_Config::QuirkOverride::Auto: break;
        }
        return kAsyncShaderCompileDefault;
    }

    namespace {
        // Written only by glMaxShaderCompilerThreadsKHR/ARB, i.e. only on the GL thread, but
        // read by every enqueue decision, so it is atomic rather than plain: a worker never
        // reads it, but a second GL thread in another context shares this process-wide pool.
        std::atomic<Bool> g_asyncSuspendedByApplication{false};
    } // namespace

    void SetAsyncShaderCompileSuspended(const Bool suspended) {
        g_asyncSuspendedByApplication.store(suspended, std::memory_order_release);
    }

    Bool IsAsyncShaderCompileSuspended() {
        return g_asyncSuspendedByApplication.load(std::memory_order_acquire);
    }

    Bool AsyncShaderCompileActive() {
        return AsyncShaderCompileEnabled() && !IsAsyncShaderCompileSuspended();
    }

    Bool OptimisticShaderStatusActive() {
        switch (MG_Config::Features.AsyncOptimisticShaderStatus) {
        case MG_Config::QuirkOverride::ForceOn: return AsyncShaderCompileActive();
        case MG_Config::QuirkOverride::ForceOff: return false;
        case MG_Config::QuirkOverride::Auto: break;
        }
        return kOptimisticShaderStatusDefault && AsyncShaderCompileActive();
    }

    Uint DetectShaderCompileThreadCount() {
        if (const Uint32 configured = MG_Config::Features.AsyncShaderCompileThreads; configured > 0) {
            // An explicit request is honoured as given - it is the escape hatch for measuring
            // scaling and for working around a device - so it is not squeezed into [1, 4].
            return configured;
        }
        return std::clamp(DetectBigCoreCount(), 1u, kMaxAutoShaderCompileThreads);
    }

    struct ShaderCompilePool::Impl {
        explicit Impl(const Uint threads) : threadCount(std::max(1u, threads)), maxConcurrency(threadCount) {}

        const Uint threadCount;

        std::mutex mutex;
        // Created on the first dispatched Post, never in the constructor: asio::thread_pool
        // spawns its threads eagerly, and a build with async off must not pay for threads it
        // will never use.
        UniquePtr<asio::thread_pool> pool;
        std::deque<SharedPtr<JobNode>> queue;
        Uint inFlight = 0;
        Uint maxConcurrency;
        std::atomic<Bool> stopped{false};

        // Callers hold `mutex`. Hands as many queued nodes to Asio as the concurrency budget
        // allows. Posting under the lock is safe and is what keeps `pool` from being moved
        // out by a concurrent StopAndDrain between the decision and the dispatch: asio::post
        // only enqueues, it never runs the handler on the calling thread, so it cannot
        // re-enter this mutex.
        //
        // A node asio::post fails to hand off is appended to `toCancel` instead of being
        // Cancel()'d here: Cancel() runs the node's OnTerminal continuations inline (stage 4
        // added ProgramLinkTask::OnDepSettled as a real one), and a continuation is free to
        // call ShaderCompilePool::Post() again. Every caller of DispatchLocked holds `mutex`
        // (a plain, non-recursive std::mutex) - Cancel()'ing in here would let that
        // re-entrant Post() deadlock on the very lock this frame already owns. The caller
        // drains `toCancel` after releasing the lock.
        void DispatchLocked(Vector<SharedPtr<JobNode>>& toCancel) {
            while (!queue.empty() && inFlight < maxConcurrency && !stopped.load(std::memory_order_acquire)) {
                // Copy rather than move into the handler: if asio::post throws (it allocates)
                // the local SharedPtr is still valid, so the node can be settled instead of
                // being stranded Pending in a queue nothing will dispatch from again - a
                // joiner would block on it forever. Reclaiming the slot matters just as much:
                // a leaked `inFlight` shrinks the pool's concurrency budget permanently.
                SharedPtr<JobNode> node = queue.front();
                queue.pop_front();
                ++inFlight;
                try {
                    asio::post(*pool, [this, node]() mutable { RunOnWorker(Move(node)); });
                } catch (...) {
                    --inFlight;
                    toCancel.push_back(Move(node));
                }
            }
        }

        void RunOnWorker(SharedPtr<JobNode> node) {
            tl_isPoolThread = true;
            // A node that was already handed to Asio when StopAndDrain ran still arrives
            // here; cancelling it first turns the dispatch into a state transition instead of
            // a full compile, so the drain's join() returns promptly. This Cancel() runs
            // before `mutex` is ever taken in this frame, so it is not subject to the
            // re-entrancy hazard DispatchLocked's comment describes.
            if (stopped.load(std::memory_order_acquire)) node->Cancel();
            node->Run();
            node.reset();

            Vector<SharedPtr<JobNode>> toCancel;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                --inFlight;
                DispatchLocked(toCancel);
            }
            // Outside the lock: see DispatchLocked's comment.
            for (const auto& n : toCancel) {
                if (n) n->Cancel();
            }
        }
    };

    ShaderCompilePool::ShaderCompilePool(const Uint threadCount) : m_impl(MakeUnique<Impl>(threadCount)) {}

    ShaderCompilePool::~ShaderCompilePool() { StopAndDrain(); }

    ShaderCompilePool& ShaderCompilePool::Get() {
        // Leak-at-exit, like the other MobileGL singletons: the object itself is never
        // destroyed, so no static destructor can race a late entry point for it. Its THREADS
        // are a different matter and are stopped explicitly - by Init.cpp's DestroyImpl on
        // the normal path, and by the atexit sentinel below for a process that exits without
        // ever calling eglTerminate.
        static ShaderCompilePool* pool = [] {
            auto* created = new ShaderCompilePool(DetectShaderCompileThreadCount());
            g_processPool.store(created, std::memory_order_release);
            EnsureProcessTeardownSentinel();
            return created;
        }();
        return *pool;
    }

    Bool ShaderCompilePool::IsPoolThread() { return tl_isPoolThread; }

    Uint ShaderCompilePool::GetThreadCount() const { return m_impl->threadCount; }

    Uint ShaderCompilePool::GetMaxConcurrency() const {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->maxConcurrency;
    }

    void ShaderCompilePool::SetMaxConcurrency(const Uint n) {
        Vector<SharedPtr<JobNode>> toCancel;
        {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->maxConcurrency = std::clamp(n, 1u, m_impl->threadCount);
            // Raising the budget releases whatever the old one was holding back.
            if (m_impl->pool) m_impl->DispatchLocked(toCancel);
        }
        // Outside the lock: see DispatchLocked's comment.
        for (const auto& n2 : toCancel) {
            if (n2) n2->Cancel();
        }
    }

    void ShaderCompilePool::Post(SharedPtr<JobNode> node) {
        if (!node) return;
        EnsureProcessTeardownSentinel();

        // Enqueueing can throw: the thread_pool construction and asio::post both allocate,
        // and under memory pressure a throw here would escape glCompileShader leaving the
        // node Pending with nothing left to dispatch it - the first observable read would
        // then block the GL thread forever. Settle the node instead: a cancelled node is a
        // state every joiner already handles.
        //
        // `node` is still valid in the catch for every throw this try can produce. The
        // thread_pool construction runs before the move; deque::push_back is strongly
        // exception-safe and SharedPtr's move constructor is noexcept, so a throwing
        // push_back never consumed it; and DispatchLocked contains its own asio::post
        // failures rather than propagating them (see above). Keep it that way.
        Bool enqueued = false;
        Vector<SharedPtr<JobNode>> toCancel;
        try {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->stopped.load(std::memory_order_acquire) && !InProcessTeardown()) {
                if (!m_impl->pool) m_impl->pool = MakeUnique<asio::thread_pool>(m_impl->threadCount);
                m_impl->queue.push_back(Move(node));
                m_impl->DispatchLocked(toCancel);
                enqueued = true;
            }
        } catch (...) {
            MGLOG_E_ONCE("ShaderCompilePool::Post: enqueue failed; cancelling the job so its joiner "
                    "cannot block forever");
            if (node) node->Cancel();
            return;
        }
        // Outside the lock: see DispatchLocked's comment - a Cancel() here may run a
        // continuation (e.g. ProgramLinkTask::OnDepSettled) that calls back into Post().
        for (const auto& n : toCancel) {
            if (n) n->Cancel();
        }
        if (enqueued) return;

        // A stopped pool is a synchronous pool, not a black hole: the node still runs, just
        // on the caller's thread. Everything downstream already handles "terminal by the time
        // Post returns", because that is exactly what the inline path looks like. Run it
        // outside the lock - a body, or a continuation it releases, is free to Post again.
        //
        // Say so once. StopAndDrain is a one-way latch (see its tail), so from the first
        // eglTerminate onwards EVERY compile in this process silently runs on the GL thread;
        // without this line the only symptom is that asynchronous compilation stopped helping,
        // with nothing in the log to point at. Once, not per node: a pack load posts hundreds.
        static std::atomic<Bool> warnedStopped{false};
        if (!warnedStopped.exchange(true, std::memory_order_relaxed)) {
            MGLOG_W("ShaderCompilePool::Post: the pool is stopped (eglTerminate, or process exit); shader "
                    "compilation runs inline on the calling thread until MobileGL is re-initialized");
        }
        node->RunInline();
    }

    void ShaderCompilePool::StopAndDrain() {
        // asio::thread_pool::join() from a pool thread would deadlock on itself, and the
        // whole point of this call is that the GL thread waits for the workers.
        MOBILEGL_ASSERT(!IsPoolThread(), "ShaderCompilePool::StopAndDrain() called from a pool thread");

        std::deque<SharedPtr<JobNode>> abandoned;
        UniquePtr<asio::thread_pool> pool;
        {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->stopped.store(true, std::memory_order_release);
            abandoned.swap(m_impl->queue);
            pool = Move(m_impl->pool);
        }

        // Queued but never dispatched: settle them so anything chained behind them is
        // released rather than waiting for a worker that will never pick them up.
        for (const auto& node : abandoned) {
            if (node) node->Cancel();
        }

        if (pool) {
            pool->join(); // returns once every handler already handed to Asio has finished
            pool.reset();
        }

        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->inFlight = 0;
        // The pool stays stopped, so ShaderCompilePool::Get() keeps returning a stopped,
        // synchronous pool for the rest of the process. That is deliberate for the teardown
        // path this exists to serve; if a future stage wants eglTerminate followed by a fresh
        // eglInitialize to get its worker threads back, the re-arm belongs in
        // MobileGL::Initialize(), next to glslang::InitializeProcess().
    }

    void ShaderCompilePool::StopAndDrainProcessPoolAtExit() {
        if (ShaderCompilePool* pool = g_processPool.load(std::memory_order_acquire)) {
            pool->StopAndDrain();
        }
    }
} // namespace MobileGL::MG_Util::Async
