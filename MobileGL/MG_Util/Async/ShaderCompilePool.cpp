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

        Bool InProcessTeardown() { return g_processTeardown; }

        void EnsureProcessTeardownSentinel() {
            std::call_once(g_teardownSentinelOnce, [] { std::atexit(+[] { g_processTeardown = true; }); });
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
        void DispatchLocked() {
            while (!queue.empty() && inFlight < maxConcurrency && !stopped.load(std::memory_order_acquire)) {
                SharedPtr<JobNode> node = Move(queue.front());
                queue.pop_front();
                ++inFlight;
                asio::post(*pool, [this, node = Move(node)]() mutable { RunOnWorker(Move(node)); });
            }
        }

        void RunOnWorker(SharedPtr<JobNode> node) {
            tl_isPoolThread = true;
            // A node that was already handed to Asio when StopAndDrain ran still arrives
            // here; cancelling it first turns the dispatch into a state transition instead of
            // a full compile, so the drain's join() returns promptly.
            if (stopped.load(std::memory_order_acquire)) node->Cancel();
            node->Run();
            node.reset();

            const std::lock_guard<std::mutex> lock(mutex);
            --inFlight;
            DispatchLocked();
        }
    };

    ShaderCompilePool::ShaderCompilePool(const Uint threadCount) : m_impl(MakeUnique<Impl>(threadCount)) {}

    ShaderCompilePool::~ShaderCompilePool() { StopAndDrain(); }

    ShaderCompilePool& ShaderCompilePool::Get() {
        // Leak-at-exit, like the other MobileGL singletons: a process that exits without
        // eglTerminate hands the threads to the OS rather than joining them from a static
        // destructor, where the rest of the library may already be gone.
        static ShaderCompilePool* pool = new ShaderCompilePool(DetectShaderCompileThreadCount());
        return *pool;
    }

    Bool ShaderCompilePool::IsPoolThread() { return tl_isPoolThread; }

    Uint ShaderCompilePool::GetThreadCount() const { return m_impl->threadCount; }

    Uint ShaderCompilePool::GetMaxConcurrency() const {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->maxConcurrency;
    }

    void ShaderCompilePool::SetMaxConcurrency(const Uint n) {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->maxConcurrency = std::clamp(n, 1u, m_impl->threadCount);
        // Raising the budget releases whatever the old one was holding back.
        if (m_impl->pool) m_impl->DispatchLocked();
    }

    void ShaderCompilePool::Post(SharedPtr<JobNode> node) {
        if (!node) return;
        EnsureProcessTeardownSentinel();

        {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->stopped.load(std::memory_order_acquire) && !InProcessTeardown()) {
                if (!m_impl->pool) m_impl->pool = MakeUnique<asio::thread_pool>(m_impl->threadCount);
                m_impl->queue.push_back(Move(node));
                m_impl->DispatchLocked();
                return;
            }
        }

        // A stopped pool is a synchronous pool, not a black hole: the node still runs, just
        // on the caller's thread. Everything downstream already handles "terminal by the time
        // Post returns", because that is exactly what the inline path looks like. Run it
        // outside the lock - a body, or a continuation it releases, is free to Post again.
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
} // namespace MobileGL::MG_Util::Async
