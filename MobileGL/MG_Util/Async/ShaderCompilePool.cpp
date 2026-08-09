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

#include <libfork/core.hpp>
#include <libfork/schedule/lazy_pool.hpp>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <span>

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
                    if (ShaderCompilePool* pool = g_processPool.load(std::memory_order_acquire)) {
                        pool->StopAndDrain();
                    }
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

    Uint DetectShaderCompileThreadCount() {
        if (const Uint32 configured = MG_Config::Features.AsyncShaderCompileThreads; configured > 0) {
            // An explicit request is honoured as given - it is the escape hatch for measuring
            // scaling and for working around a device - so it is not squeezed into [1, 4].
            return configured;
        }
        return std::clamp(DetectBigCoreCount(), 1u, kMaxAutoShaderCompileThreads);
    }

    // ---- Engine selection -----------------------------------------------------------------

    const char* AsyncPoolEngineName(const AsyncPoolEngine engine) {
        switch (engine) {
        case AsyncPoolEngine::Libfork: return "libfork";
        case AsyncPoolEngine::Asio: break;
        }
        return "asio";
    }

    AsyncPoolEngine ParseAsyncPoolEngine(const String& value) {
        String lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "libfork") return AsyncPoolEngine::Libfork;
        if (lowered == "asio" || lowered == "auto" || lowered.empty()) return AsyncPoolEngine::Asio;
        // Not silent: a misspelt engine name resolving to the default would be
        // indistinguishable from not having set the variable at all, and the only reason to
        // set it is to know which engine ran.
        MGLOG_W("Config: Ignoring invalid env variable MOBILEGL_ASYNC_POOL='%s'; expected asio|libfork, "
                "using asio",
                value.c_str());
        return AsyncPoolEngine::Asio;
    }

    AsyncPoolEngine DetectAsyncPoolEngine() {
        // A live std::getenv rather than an MG_Config::Features mirror, and deliberately so:
        // a ShaderCompilePool is constructed by binaries that never call MobileGL::Initialize()
        // and therefore never run MG_ConfigLoader::Init() - MG_Test/Util/JobNodeTest builds
        // pools directly, and it is the suite that exercises the engines against each other.
        // Reading Features there would silently resolve to the default and the libfork half of
        // the test matrix would prove nothing. See the exemption list in Config.h.
        //
        // Resolved once per process (a function-local static): every pool in a process gets
        // the same engine, so a process can never end up running two.
        static const AsyncPoolEngine engine = [] {
            const char* value = std::getenv("MOBILEGL_ASYNC_POOL");
            const AsyncPoolEngine resolved = ParseAsyncPoolEngine(value != nullptr ? String(value) : String());
            if (resolved != AsyncPoolEngine::Asio) {
                MGLOG_I("ShaderCompilePool: MOBILEGL_ASYNC_POOL selected the %s execution engine",
                        AsyncPoolEngineName(resolved));
            }
            return resolved;
        }();
        return engine;
    }

    namespace {
        // ---- The engine boundary ----------------------------------------------------------
        // Submit() has exactly asio::post's contract, and ShaderCompilePool::Impl leans on all
        // four halves of it:
        //   * it NEVER runs `fn` on the calling thread. DispatchLocked calls it while holding
        //     the pool's plain, non-recursive mutex, and a job body (or a terminal
        //     continuation it releases) is free to call Post() again - an inline run would
        //     deadlock on the lock this frame already owns.
        //   * it is callable from ANY thread, a worker of this very pool included:
        //     ProgramLinkTask::OnDepSettled posts the link job from whichever thread drove the
        //     last compile terminal, which is a worker.
        //   * it may throw, and when it does it must not have consumed the caller's job node,
        //     so Post/DispatchLocked can settle the node instead of stranding it Pending with
        //     a joiner blocked forever.
        //   * once it has accepted `fn`, `fn` WILL run. A dropped callable is a node nothing
        //     ever settles, so the engines run it themselves rather than discard it.
        class JobExecutor {
        public:
            virtual ~JobExecutor() = default;
            JobExecutor() = default;
            JobExecutor(const JobExecutor&) = delete;
            JobExecutor& operator=(const JobExecutor&) = delete;

            virtual void Submit(std::function<void()> fn) = 0;

            // Returns once every callable ever handed to Submit has finished running. The
            // guarantee StopAndDrain sells to library teardown: after it returns, no worker is
            // still inside a job body that could touch glslang's process globals.
            virtual void JoinAll() = 0;
        };

        // ---- Engine 1: Asio (the shipped default) -----------------------------------------
        class AsioJobExecutor final : public JobExecutor {
        public:
            explicit AsioJobExecutor(const Uint threads) : m_pool(threads) {}

            // asio::post only enqueues; it never runs the handler on the calling thread, which
            // is what makes calling it under the pool mutex safe.
            void Submit(std::function<void()> fn) override { asio::post(m_pool, Move(fn)); }

            void JoinAll() override { m_pool.join(); }

        private:
            asio::thread_pool m_pool;
        };

        // ---- Engine 2: libfork ------------------------------------------------------------
        //
        // libfork is a continuation-stealing fork-join runtime, and the shape that fits here is
        // NOT fork-join: a job body is one coarse, blocking, non-forking unit (a glslang
        // compile), and the concurrency budget that bounds peak RSS is Impl's, not the
        // scheduler's. So libfork is used as a job executor - each dispatched job is a detached
        // root task - and what it is being asked to beat is Asio's single scheduler queue with
        // its per-worker work-stealing deques and sleeping workers.
        //
        // The one thing libfork forbids is the thing this pool does constantly: lf::schedule
        // (which lf::detach is built on) THROWS lf::schedule_in_worker when the calling thread
        // is a libfork worker, because workers may never block. Yet a worker submits on every
        // job completion - RunOnWorker's tail refills the budget - and again whenever a
        // terminal continuation posts (ProgramLinkTask::OnDepSettled). Routing those through a
        // separate dispatch thread works but costs two thread wakeups per job, which measured
        // 4x worse than Asio on short jobs. So instead a dispatched root is a CHAIN: when its
        // body returns it takes the next queued job itself and runs it in the same coroutine
        // on the same worker. The refill a worker submits is therefore absorbed by the very
        // chain that submitted it - no scheduler round trip, no wakeup - and libfork is only
        // entered for work that arrives from outside the pool.
        //
        // Absorption is bounded at one job per running chain, though, because a chain is one
        // worker: past that bound the queue would be jobs the budget has already cleared,
        // waiting behind each other on a single thread. See Submit.
        //
        // Why none of this can strand a job: the queue below is only ever added to from inside
        // a running chain (tl_chainOwner == this), and a chain exits only when it finds the
        // queue empty - unconditionally, whatever the bound says. Every other submitter goes
        // to the dispatch thread or straight to lf::detach.
        class LibforkJobExecutor;

        // Which executor's chain, if any, is running on this thread. Deliberately narrower
        // than ShaderCompilePool::IsPoolThread(): that flag is process-wide and latched
        // forever, so a worker of a DIFFERENT pool would read as "mine" and queue a job into a
        // chain that will never drain it. This says exactly "a chain of *this* executor is
        // executing on this thread, and it will look at the queue again before it exits".
        thread_local LibforkJobExecutor* tl_chainOwner = nullptr;

        // One dispatched job, heap-owned. It reaches its coroutine as a POINTER passed BY
        // VALUE: libfork forwards a root task's arguments into the coroutine frame, so a
        // by-value pointer is copied into the frame, whereas anything passed by reference
        // would dangle the moment lf::detach returns - and detach, unlike sync_wait, does not
        // outlive the task.
        struct LibforkJob {
            std::function<void()> body;
            LibforkJobExecutor* owner;
        };

        // A scheduler adaptor for lf::detach: it places external submissions round-robin over
        // lf::lazy_pool's worker contexts instead of letting the pool pick one at random.
        // Both reasons are load-bearing, and the second was worth 1.3x at a budget equal to
        // the worker count - the configuration MobileGL actually ships, since maxConcurrency
        // is clamped to the thread count:
        //   * lf::lazy_pool::schedule chooses its victim with a
        //     std::uniform_int_distribution over a lazy_pool-member xoshiro generator -
        //     unsynchronized mutable state, so two concurrent submissions are a data race
        //     inside libfork itself. An atomic cursor is not.
        //   * A worker's SUBMISSION list is drained only by that worker
        //     (worker_context::try_pop_all is documented "for use only by the owning worker
        //     thread"); a thief takes from the task deque, which is a different queue. So a
        //     job placed on a worker that is inside a long blocking body waits for that body
        //     rather than being stolen - and random placement of `budget` submissions over
        //     `budget` workers collides by the birthday rule. Round-robin lands the GL
        //     thread's burst one per worker, which is exactly the intended shape.
        struct RoundRobinSubmitter {
            std::span<lf::worker_context*> contexts;
            std::atomic<Uint64>* cursor;

            void schedule(const lf::submit_handle job) const {
                const Uint64 index = cursor->fetch_add(1, std::memory_order_relaxed);
                contexts[static_cast<SizeT>(index % contexts.size())]->schedule(job);
            }
        };

        void RunLibforkChain(LibforkJob* raw) noexcept;

        // The root task every dispatched chain runs as. libfork async function objects are
        // copyable, captureless callables returning lf::task<>, whose first parameter is the
        // combinator's synthesized first argument (unused here: this task neither forks nor
        // joins). The coroutine exists purely as libfork's entry protocol; the loop is in
        // RunLibforkChain.
        inline constexpr auto kLibforkChainTask = [](auto /*self*/, LibforkJob* job) -> lf::task<void> {
            RunLibforkChain(job);
            co_return;
        };

        class LibforkJobExecutor final : public JobExecutor {
        public:
            explicit LibforkJobExecutor(const Uint threads)
                : m_pool(static_cast<std::size_t>(std::max(1u, threads))), m_contexts(m_pool.contexts()),
                  m_fallback([this] { FallbackLoop(); }) {}

            ~LibforkJobExecutor() override {
                JoinAll();
                {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_fallbackStop = true;
                }
                m_fallbackCv.notify_all();
                if (m_fallback.joinable()) m_fallback.join();
                // m_pool is destroyed last, and only here: lf::lazy_pool may not be destructed
                // while any submitted task can still run or submit more. JoinAll() has
                // established the first and the joined fallback thread the second. Its
                // destructor then joins the worker threads, so a worker still unwinding a
                // finished coroutine frame is waited for rather than pulled out from under.
            }

            void Submit(std::function<void()> fn) override {
                if (tl_chainOwner == this) {
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    // The hot path: ONE job per running chain. A chain picks up exactly one
                    // queued job each time its body returns, so a queue no longer than the
                    // number of live chains is a queue every entry of which has a distinct
                    // worker waiting to take it - which is precisely the steady state this
                    // absorption exists for (every worker finishes a job and refills its own
                    // slot, all at once, with no scheduler round trip between them).
                    //
                    // Past that it is oversubscription, and absorbing it would be a
                    // correctness-preserving way to destroy the pool's parallelism: the
                    // budget would still say `maxConcurrency` jobs are in flight while one
                    // worker ran them one behind another. That is not hypothetical - it is
                    // the tail of a pack load, where one compile going terminal releases
                    // several programs at once (ShaderCompileAdoptionMap lets a single
                    // compile settle many) and the worker that drove it posts the whole
                    // burst into an otherwise idle pool. Measured before this branch existed:
                    // four such jobs took 4x one job's wall time on libfork and 1x on Asio.
                    //
                    // The overflow cannot go to lf::detach from here - a libfork worker may
                    // not schedule - so it goes to the dispatch thread, which detaches it to
                    // a worker of its own. That costs one thread wakeup; serializing costs a
                    // whole compile.
                    //
                    // The count is taken AFTER the push, not before: deque::push_back is
                    // strongly exception-safe, so an allocation failure here leaves `fn`
                    // intact for DispatchLocked to settle - but a count incremented in front
                    // of it would be a count nothing ever gives back, and JoinAll would wait
                    // on it forever.
                    const Bool takeable = m_chainQueue.size() < m_liveChains;
                    if (takeable) {
                        m_chainQueue.push_back(Move(fn));
                        ++m_outstanding;
                    } else {
                        m_fallbackQueue.push_back(Move(fn));
                        ++m_outstanding;
                        m_fallbackCv.notify_one();
                    }
                    return;
                }

                {
                    // Counted before anything can run it, so JoinAll cannot observe a zero
                    // that this job would have broken.
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_outstanding;
                }
                try {
                    DetachChain(Move(fn));
                } catch (const lf::schedule_in_worker&) {
                    // Submitted from a libfork worker that is not running one of my chains -
                    // a worker of another ShaderCompilePool. libfork will not take a
                    // submission from there at all, and the queue above is not safe for it
                    // (no chain of mine is running on that thread to drain it), so it goes to
                    // the fallback thread, which is neither. DetachChain restored `fn` before
                    // it threw.
                    const std::lock_guard<std::mutex> lock(m_mutex);
                    m_fallbackQueue.push_back(Move(fn));
                    m_fallbackCv.notify_one();
                } catch (...) {
                    // Out of memory. Give the count back and let the caller settle its node:
                    // that is Submit's contract and what DispatchLocked is written against.
                    Retire();
                    throw;
                }
            }

            void JoinAll() override {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_idleCv.wait(lock, [this] { return m_outstanding == 0; });
            }

            // A chain announces itself before it runs its first body, so that Submit's
            // absorption rule can count the workers that are going to come back and ask for
            // more. Under-counting is the only direction this can be wrong in (a detached
            // chain is not counted until it starts), and under-counting only sends work to
            // the dispatch thread that a chain could have taken - never the reverse.
            void EnterChain() noexcept {
                const std::lock_guard<std::mutex> lock(m_mutex);
                ++m_liveChains;
            }

            // The end of one job in a chain. Returns true having loaded `body` with the next
            // job to run on this same worker, false when there is nothing left - after which
            // the caller must touch neither `this` nor anything owned by it, because the
            // count this drops to zero may be the one JoinAll is waiting for.
            //
            // `body` must arrive empty: the finished job's captures (a strong reference to its
            // JobNode) are released by the chain, outside this lock, so that no JobNode
            // destructor ever runs inside the executor's critical section.
            Bool RetireAndTakeNext(std::function<void()>& body) noexcept {
                const std::lock_guard<std::mutex> lock(m_mutex);
                --m_outstanding;
                if (!m_chainQueue.empty()) {
                    // Unconditional, and it has to stay that way: a chain that exited while
                    // the queue was non-empty could be the last one, and the entry would then
                    // be waiting on a worker that never comes. That is what makes the
                    // absorption bound in Submit a scheduling policy rather than a liveness
                    // requirement.
                    //
                    // swap, not move-assign: std::function's move assignment is not noexcept,
                    // and this function is.
                    body.swap(m_chainQueue.front());
                    m_chainQueue.pop_front();
                    return true; // the taken job's own count stays held
                }
                --m_liveChains;
                // Notified while STILL HOLDING the lock, which is the whole reason this is not
                // the usual notify-after-unlock. The wakeup this sends can be the one that
                // lets JoinAll return and ~LibforkJobExecutor destroy m_idleCv - and a
                // std::condition_variable may not be destroyed while another thread is inside
                // notify_all() on it. Holding the lock across the notify means the waiter
                // cannot re-acquire the mutex, and therefore cannot leave wait(), until this
                // thread is out of both the notify and the unlock. ThreadSanitizer catches the
                // other order immediately (pthread_cond_destroy vs pthread_cond_broadcast).
                if (m_outstanding == 0) m_idleCv.notify_all();
                return false;
            }

        private:
            // Builds the root task and hands it to libfork. On any failure `fn` is restored,
            // so the caller can still decide what to do with the job.
            void DetachChain(std::function<void()>&& fn) {
                // `new T{...}` allocates before it constructs, so a throwing operator new
                // leaves `fn` untouched; the member move is std::function's noexcept one.
                LibforkJob* job = new LibforkJob{Move(fn), this};
                try {
                    lf::detach(RoundRobinSubmitter{m_contexts, &m_cursor}, kLibforkChainTask, job);
                } catch (...) {
                    // lf::schedule upholds the strong exception guarantee, so nothing was
                    // scheduled and the payload is still ours.
                    const UniquePtr<LibforkJob> owned(job);
                    fn = Move(owned->body);
                    throw;
                }
            }

            void Retire() noexcept {
                // Under the lock, for the reason RetireAndTakeNext spells out.
                const std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_outstanding == 0) m_idleCv.notify_all();
            }

            // The dispatch thread. It exists because lf::detach is illegal on a libfork worker
            // and legal here, and it serves the two cases Submit cannot take itself: a
            // submission from another pool's worker, and a chain's overflow past the
            // one-job-per-chain bound. It sleeps otherwise, and it dispatches rather than
            // executes - a body only ever runs here if libfork refuses the job outright.
            void FallbackLoop() {
                for (;;) {
                    std::function<void()> fn;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_fallbackCv.wait(lock, [this] { return !m_fallbackQueue.empty() || m_fallbackStop; });
                        // Emptiness is checked before the stop flag so that a stop can never
                        // strand accepted work: an accepted job always runs, because the node
                        // behind it has a joiner that would otherwise block forever.
                        if (m_fallbackQueue.empty()) return;
                        fn.swap(m_fallbackQueue.front());
                        m_fallbackQueue.pop_front();
                    }
                    try {
                        DetachChain(Move(fn));
                    } catch (...) {
                        MGLOG_E("ShaderCompilePool: libfork refused a fallback dispatch; running the job on "
                                "the dispatch thread instead of dropping it");
                        RunHere(Move(fn));
                    }
                }
            }

            // Last resort. Running the body here costs this engine its parallelism for one
            // job; dropping it would cost a joiner its wakeup forever.
            void RunHere(std::function<void()>&& fn) noexcept {
                try {
                    if (fn) fn();
                } catch (...) {
                    MGLOG_E("ShaderCompilePool: a job body escaped its own containment on the dispatch "
                            "thread; it has been swallowed to keep the thread alive");
                }
                fn = nullptr;
                Retire();
            }

            lf::lazy_pool m_pool;
            // Fixed for the pool's lifetime, so it is read once rather than per submission.
            std::span<lf::worker_context*> m_contexts;
            std::atomic<Uint64> m_cursor{0};

            std::mutex m_mutex;
            std::condition_variable m_fallbackCv;
            std::condition_variable m_idleCv;
            // Refills and continuations submitted from inside a chain: drained by the chains.
            std::deque<std::function<void()>> m_chainQueue;
            // Chains currently executing, i.e. workers that will look at m_chainQueue again
            // before they exit. The bound on how much Submit may absorb into a chain.
            Uint m_liveChains = 0;
            // Submissions from another pool's libfork worker, and the overflow of the rule
            // above: drained by m_fallback, which detaches each one to a worker.
            std::deque<std::function<void()>> m_fallbackQueue;
            // Everything submitted and not yet finished, whichever queue it is in and whether
            // or not it has reached a worker, so JoinAll needs a single predicate.
            Uint m_outstanding = 0;
            Bool m_fallbackStop = false;
            std::thread m_fallback;
        };

        void RunLibforkChain(LibforkJob* const raw) noexcept {
            UniquePtr<LibforkJob> job(raw);
            LibforkJobExecutor* const owner = job->owner;
            std::function<void()> body;
            body.swap(job->body);
            job.reset();

            LibforkJobExecutor* const savedOwner = tl_chainOwner;
            tl_chainOwner = owner;
            owner->EnterChain();

            for (;;) {
                try {
                    if (body) body();
                } catch (...) {
                    // JobNode::Run contains every body exception already; this is the backstop
                    // for the wrapper itself. An exception escaping here would be stashed in
                    // the root task's shared state, which lf::detach discards - i.e. silently
                    // lost - and would abandon the rest of the chain.
                    MGLOG_E("ShaderCompilePool: a job body escaped its own containment on a libfork worker; "
                            "it has been swallowed to keep the chain alive");
                }
                // Release the finished job's captures (its strong JobNode reference) HERE,
                // outside the executor's lock: a JobNode destructor is arbitrary code.
                body = nullptr;
                if (!owner->RetireAndTakeNext(body)) break;
            }

            // `owner` may already be destroyed - RetireAndTakeNext returning false can be the
            // call that releases a JoinAll. Nothing below touches it.
            tl_chainOwner = savedOwner;
        }

        UniquePtr<JobExecutor> MakeJobExecutor(const AsyncPoolEngine engine, const Uint threads) {
            switch (engine) {
            case AsyncPoolEngine::Libfork: return MakeUnique<LibforkJobExecutor>(threads);
            case AsyncPoolEngine::Asio: break;
            }
            return MakeUnique<AsioJobExecutor>(threads);
        }
    } // namespace

    struct ShaderCompilePool::Impl {
        explicit Impl(const Uint threads)
            : threadCount(std::max(1u, threads)), engine(DetectAsyncPoolEngine()), maxConcurrency(threadCount) {}

        const Uint threadCount;
        // Latched at construction, not re-read: a pool may not change engines under its own
        // workers, and GetEngine() is what the tests compare against the environment.
        const AsyncPoolEngine engine;

        std::mutex mutex;
        // Created on the first dispatched Post, never in the constructor: both engines spawn
        // their threads eagerly (asio::thread_pool its workers, lf::lazy_pool its workers plus
        // this file's dispatch thread), and a build with async off must not pay for threads it
        // will never use.
        UniquePtr<JobExecutor> executor;
        std::deque<SharedPtr<JobNode>> queue;
        Uint inFlight = 0;
        Uint maxConcurrency;
        std::atomic<Bool> stopped{false};

        // Callers hold `mutex`. Hands as many queued nodes to the engine as the concurrency
        // budget allows. Submitting under the lock is safe and is what keeps `executor` from
        // being moved out by a concurrent StopAndDrain between the decision and the dispatch:
        // Submit only enqueues, it never runs the callable on the calling thread, so it cannot
        // re-enter this mutex.
        //
        // A node the engine fails to accept is appended to `toCancel` instead of being
        // Cancel()'d here: Cancel() runs the node's OnTerminal continuations inline (stage 4
        // added ProgramLinkTask::OnDepSettled as a real one), and a continuation is free to
        // call ShaderCompilePool::Post() again. Every caller of DispatchLocked holds `mutex`
        // (a plain, non-recursive std::mutex) - Cancel()'ing in here would let that
        // re-entrant Post() deadlock on the very lock this frame already owns. The caller
        // drains `toCancel` after releasing the lock.
        //
        // The `stopped` check is also what keeps this loop from dereferencing a null
        // `executor`: StopAndDrain sets the flag and moves the executor out in the same
        // critical section, so a stopped pool never reaches the Submit below.
        void DispatchLocked(Vector<SharedPtr<JobNode>>& toCancel) {
            while (!queue.empty() && inFlight < maxConcurrency && !stopped.load(std::memory_order_acquire)) {
                // Copy rather than move into the callable: if Submit throws (both engines
                // allocate) the local SharedPtr is still valid, so the node can be settled
                // instead of being stranded Pending in a queue nothing will dispatch from
                // again - a joiner would block on it forever. Reclaiming the slot matters just
                // as much: a leaked `inFlight` shrinks the pool's concurrency budget
                // permanently.
                SharedPtr<JobNode> node = queue.front();
                queue.pop_front();
                ++inFlight;
                try {
                    executor->Submit([this, node]() mutable { RunOnWorker(Move(node)); });
                } catch (...) {
                    --inFlight;
                    toCancel.push_back(Move(node));
                }
            }
        }

        void RunOnWorker(SharedPtr<JobNode> node) {
            tl_isPoolThread = true;
            // A node that was already handed to the engine when StopAndDrain ran still arrives
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

    AsyncPoolEngine ShaderCompilePool::GetEngine() const { return m_impl->engine; }

    void ShaderCompilePool::SetMaxConcurrency(const Uint n) {
        Vector<SharedPtr<JobNode>> toCancel;
        {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->maxConcurrency = std::clamp(n, 1u, m_impl->threadCount);
            // Raising the budget releases whatever the old one was holding back.
            if (m_impl->executor) m_impl->DispatchLocked(toCancel);
        }
        // Outside the lock: see DispatchLocked's comment.
        for (const auto& n2 : toCancel) {
            if (n2) n2->Cancel();
        }
    }

    void ShaderCompilePool::Post(SharedPtr<JobNode> node) {
        if (!node) return;
        EnsureProcessTeardownSentinel();

        // Enqueueing can throw: building the engine and submitting to it both allocate (and
        // both spawn threads), and under memory pressure a throw here would escape
        // glCompileShader leaving the node Pending with nothing left to dispatch it - the
        // first observable read would then block the GL thread forever. Settle the node
        // instead: a cancelled node is a state every joiner already handles.
        //
        // `node` is still valid in the catch for every throw this try can produce. The engine
        // construction runs before the move; deque::push_back is strongly exception-safe and
        // SharedPtr's move constructor is noexcept, so a throwing push_back never consumed it;
        // and DispatchLocked contains its own Submit failures rather than propagating them
        // (see above). Keep it that way.
        Bool enqueued = false;
        Vector<SharedPtr<JobNode>> toCancel;
        try {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            if (!m_impl->stopped.load(std::memory_order_acquire) && !InProcessTeardown()) {
                if (!m_impl->executor) m_impl->executor = MakeJobExecutor(m_impl->engine, m_impl->threadCount);
                m_impl->queue.push_back(Move(node));
                m_impl->DispatchLocked(toCancel);
                enqueued = true;
            }
        } catch (...) {
            MGLOG_E("ShaderCompilePool::Post: enqueue failed; cancelling the job so its joiner "
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
        // Waiting for the workers from a worker would deadlock on itself (asio's join() says
        // so outright), and the whole point of this call is that the GL thread waits.
        MOBILEGL_ASSERT(!IsPoolThread(), "ShaderCompilePool::StopAndDrain() called from a pool thread");

        std::deque<SharedPtr<JobNode>> abandoned;
        UniquePtr<JobExecutor> executor;
        {
            const std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->stopped.store(true, std::memory_order_release);
            abandoned.swap(m_impl->queue);
            executor = Move(m_impl->executor);
        }

        // Queued but never dispatched: settle them so anything chained behind them is
        // released rather than waiting for a worker that will never pick them up.
        for (const auto& node : abandoned) {
            if (node) node->Cancel();
        }

        if (executor) {
            executor->JoinAll(); // returns once every job already handed to the engine is done
            executor.reset();    // and this stops the engine's threads
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
