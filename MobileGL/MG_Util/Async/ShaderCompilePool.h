// MobileGL - MobileGL/MG_Util/Async/ShaderCompilePool.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Types.h>
#include <MG_Util/Async/JobNode.h>

// This header deliberately includes NO Asio and NO libfork header: both execution engines
// live behind the pimpl in ShaderCompilePool.cpp. They stay private implementation details of
// one translation unit, so no consumer target (MG_Test, MG_IntegrationTest, MG_Benchmark -
// each with its own target_include_directories) needs either include path, and no consumer
// pays their compile time. libfork in particular is a C++20-coroutine header set whose
// instantiation cost nothing outside the pool has any reason to carry. Do not add one here.

namespace MobileGL::MG_Util::Async {
    // Stage 7: on by default. The gate behind the flip (2026-08-09, headless Mesa, both
    // backends): GL30-40 mustpass + KHR-GL46.parallel_shader_compile at async=1 with the
    // extension advertised - 58,344 case-runs, 8 failures, and every one of the 8 also
    // fails standalone at async=0 and under the pre-P1 library, i.e. zero async-attributable
    // deltas. The risk this comment used to name - Iris and Sodium changing their submission
    // schedule the moment GL_KHR_parallel_shader_compile is advertised - remains the one
    // thing a recorded trace cannot cover, which is why the kill switch below stays.
    inline constexpr Bool kAsyncShaderCompileDefault = true;

    // MOBILEGL_ASYNC_SHADER_COMPILE forces the answer either way; unset keeps the built-in
    // default above. Falsy is a complete kill switch: it reverts the threading *and*
    // withdraws GL_KHR_parallel_shader_compile, so the application behaviour change goes
    // with it.
    //
    // This is the pure CONFIGURATION answer, and it is deliberately not affected by
    // glMaxShaderCompilerThreadsKHR: it is what decides whether the extension is advertised
    // at all, and an application that switched threading off through the extension has not
    // made the extension go away. Code deciding whether to enqueue asks
    // AsyncShaderCompileActive() instead.
    Bool AsyncShaderCompileEnabled();

    // ---- GL_KHR_parallel_shader_compile: glMaxShaderCompilerThreadsKHR(count) ----
    // The extension defines count == 0 as "no compiler threads": compilation must happen on
    // the application's thread. That is a mode switch, not a concurrency budget of one, so it
    // is a latch of its own rather than SetMaxConcurrency(1) - a budget of one would still
    // move the work off-thread and still report GL_COMPLETION_STATUS_KHR = GL_FALSE, both of
    // which the extension forbids after a zero count.
    //
    // The latch is process-wide, matching the pool it suspends. It is released by the next
    // nonzero glMaxShaderCompilerThreadsKHR/ARB, which is the only thing that releases it:
    // no implicit re-arm on eglInitialize, on a context switch or at any join, because an
    // application that asked for serial compilation gets to keep it until it asks otherwise.
    void SetAsyncShaderCompileSuspended(Bool suspended);
    Bool IsAsyncShaderCompileSuspended();

    // What every enqueue site branches on: the configuration flag AND the absence of a
    // glMaxShaderCompilerThreadsKHR(0). False makes glCompileShader/glLinkProgram run their
    // bodies inline, exactly as the flag-off path does, which is what makes a subsequent
    // GL_COMPLETION_STATUS_KHR read immediately GL_TRUE.
    Bool AsyncShaderCompileActive();

    // min(4, big cores), where a big core is one whose cpufreq ceiling is within 15% of the
    // machine maximum; the whole CPU count where that sysfs tree is absent. Clamped to [1, 4]
    // because peak RSS scales as workers x largest glslang arena, and four
    // Complementary-sized arenas is already the memory ceiling worth accepting on a phone.
    // MOBILEGL_ASYNC_SHADER_COMPILE_THREADS overrides it outright.
    Uint DetectShaderCompileThreadCount();

    // ---- MOBILEGL_ASYNC_POOL: which engine drives the worker threads ----------------------
    // The engine is ONLY the execution engine. The job queue, the concurrency budget and its
    // clamping, the suspension latch, cancel request-vs-outcome, the stopped-is-synchronous
    // fallback and the drain are all engine-independent - they live in ShaderCompilePool::Impl
    // and are shared verbatim by both engines, which is what lets the whole async suite run
    // unchanged against either one. An engine answers exactly one question: how does a job
    // that the budget has already cleared reach a worker thread?
    enum class AsyncPoolEngine : Uint8 {
        Asio,    // asio::thread_pool: one shared queue behind Asio's scheduler lock
        Libfork, // lf::lazy_pool: per-worker work-stealing deques, workers sleep when idle
    };

    // "asio" / "libfork" - the spelling the environment variable accepts and the log prints.
    const char* AsyncPoolEngineName(AsyncPoolEngine engine);

    // Parses one MOBILEGL_ASYNC_POOL value. Case-insensitive; empty, "auto" and anything
    // unrecognized resolve to Asio, and an unrecognized value warns (a misspelt engine name
    // would otherwise be indistinguishable from the default, and the whole point of the
    // variable is to know which engine ran).
    AsyncPoolEngine ParseAsyncPoolEngine(const String& value);

    // The process's engine, resolved from MOBILEGL_ASYNC_POOL on first call and cached. Every
    // pool constructed afterwards reports the same answer, so a process never mixes engines.
    AsyncPoolEngine DetectAsyncPoolEngine();

    class ShaderCompilePool {
    public:
        explicit ShaderCompilePool(Uint threadCount);
        ~ShaderCompilePool();
        ShaderCompilePool(const ShaderCompilePool&) = delete;
        ShaderCompilePool& operator=(const ShaderCompilePool&) = delete;

        // Process-wide pool, leak-at-exit like pGLContext. Sized by
        // DetectShaderCompileThreadCount() on first use; no thread is created until the first
        // Post, so a build that never enables async never starts one.
        static ShaderCompilePool& Get();

        // True only on a thread owned by some ShaderCompilePool. Backs the two asserts that
        // hold the design's invariants up: no GL/EGL reach-back from a worker, and no job
        // body waiting on another job.
        static Bool IsPoolThread();

        // Dispatches the node, or queues it behind the concurrency budget. A stopped pool -
        // and one whose process is exiting - runs the node inline on the calling thread
        // instead, so a late entry point can never resurrect worker threads.
        void Post(SharedPtr<JobNode> node);

        // Cancels everything still queued and joins everything already running. This is the
        // one cancellation path that waits, and it must run before glslang::FinalizeProcess()
        // and before pGLContext is destroyed: in-flight jobs hold their own inputs safely,
        // but they share glslang's process globals, which teardown is about to free.
        void StopAndDrain();

        Uint GetThreadCount() const;
        Uint GetMaxConcurrency() const;

        // The engine this pool was built with, latched at construction from
        // DetectAsyncPoolEngine(). Reported rather than re-resolved so that a pool cannot
        // change engines under its own workers.
        AsyncPoolEngine GetEngine() const;

        // Bounded concurrency doubles as the memory bound, and is how
        // glMaxShaderCompilerThreadsKHR(n) is honoured: a 300-program pack load cannot put
        // 300 glslang arenas in flight at once. Clamped to [1, thread count].
        void SetMaxConcurrency(Uint n);

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Util::Async
