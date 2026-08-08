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

// This header deliberately includes NO Asio header: asio::thread_pool lives behind the pimpl
// in ShaderCompilePool.cpp. Asio stays a private implementation detail of one translation
// unit, so no consumer target (MG_Test, MG_IntegrationTest, MG_Benchmark - each with its own
// target_include_directories) needs the Asio include path, and no consumer pays its compile
// time. Do not add one here.

namespace MobileGL::MG_Util::Async {
    // Stage 1 ships the whole machinery switched off: the pool is constructible and tested,
    // but nothing in the GL pipeline posts to it. The flip to true happens only after the
    // real-client soak in the final stage, because the riskiest part of asynchronous
    // compilation is not the joins - it is that Iris and Sodium change their submission
    // schedule the moment GL_KHR_parallel_shader_compile is advertised, and a recorded trace
    // can never cover that path.
    inline constexpr Bool kAsyncShaderCompileDefault = false;

    // MOBILEGL_ASYNC_SHADER_COMPILE forces the answer either way; unset keeps the built-in
    // default above. Falsy is a complete kill switch: it reverts the threading *and* (from
    // the extension stage on) withdraws GL_KHR_parallel_shader_compile, so the application
    // behaviour change goes with it.
    Bool AsyncShaderCompileEnabled();

    // min(4, big cores), where a big core is one whose cpufreq ceiling is within 15% of the
    // machine maximum; the whole CPU count where that sysfs tree is absent. Clamped to [1, 4]
    // because peak RSS scales as workers x largest glslang arena, and four
    // Complementary-sized arenas is already the memory ceiling worth accepting on a phone.
    // MOBILEGL_ASYNC_SHADER_COMPILE_THREADS overrides it outright.
    Uint DetectShaderCompileThreadCount();

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

        // Bounded concurrency doubles as the memory bound, and is how
        // glMaxShaderCompilerThreadsKHR(n) is honoured: a 300-program pack load cannot put
        // 300 glslang arenas in flight at once. Clamped to [1, thread count].
        void SetMaxConcurrency(Uint n);

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
} // namespace MobileGL::MG_Util::Async
