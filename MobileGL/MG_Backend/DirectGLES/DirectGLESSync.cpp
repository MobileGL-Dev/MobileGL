// MobileGL - MobileGL/MG_Backend/DirectGLES/DirectGLESSync.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DirectGLESInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace {
        // Backend fence handle: a native ES sync plus the ES context
        // generation it was created under.
        struct GLESSyncObject {
            GLsync esSync = nullptr;
            Uint contextGeneration = 0;
        };

        // Backend timer-query handle: a native GL query object name plus the
        // ES context generation it was created under. Stale-generation
        // handles read as available with a zero result, and deleting them
        // only frees the wrapper (the dead ES context already reclaimed the
        // query object).
        struct GLESQueryObject {
            GLuint queryId = 0;
            Uint contextGeneration = 0;
            // Non-zero for the core (non-timer) query targets - occlusion and transform
            // feedback primitives - and then holds the glBeginQuery target, which glEndQuery
            // needs back. Their results are counts reachable only through the core
            // glGetQueryObjectuiv getter: GL_EXT_disjoint_timer_query's 64-bit
            // glGetQueryObjectui64vEXT is timer-specific and may be entirely absent on
            // drivers that otherwise fully support these core ES queries.
            GLenum coreTarget = 0;
        };
    } // namespace
    // EGL ground-truth verification stamp. glvnd's eglGetCurrentContext performs
    // fork detection with a real getpid() syscall on every call, and this
    // predicate sits 2-3 deep in every draw - measured at 16% of the render
    // thread on a live workload. NOT thread_local although the stamp is
    // per-owner-thread state: a shared-library thread_local costs a
    // __tls_get_addr call per access, which itself showed at 1.5% of the draw
    // loop. Plain globals are equivalent because only the thread that passes the
    // owner-identity check below can ever stamp or trust them, only one thread
    // can be the owner at a time, and the ONLY writers of the owner id -
    // MakeCurrent/ReleaseCurrent - reset the stamps before publishing a new
    // owner, so a stamp can never leak across an ownership change. Atomics
    // (relaxed) because the resets may come from a non-owner thread; the
    // owner-id release/acquire pairing orders them.
    std::atomic<Uint64> g_eglVerifiedFrameSerial{~0ull};
    std::atomic<Uint> g_eglVerifiedContextGeneration{0};

    Bool IsBackendContextCurrentOnThisThread() {
        const NativeContextTuple& tuple = ActiveNativeContext();
        if (tuple.Context == EGL_NO_CONTEXT) {
            return false;
        }
        if (g_backendContextOwnerThread.load(std::memory_order_acquire) != std::this_thread::get_id()) {
            return false;
        }
        // Belt and braces: EGL itself is the ground truth. A migration that bypassed
        // MakeCurrent()/ReleaseCurrent() must not leave a stale ownership claim
        // standing, or GL calls would silently no-op while shadow bookkeeping (bind
        // cache, synced serials) still advances. Re-verify once per (frame, context
        // generation) rather than per call: an external migration is caught at the
        // next frame boundary instead of the next call, which recovers the
        // bookkeeping just the same, without paying a syscall on every draw.
        const Uint64 frameSerial = g_currentFrameSerial.load(std::memory_order_relaxed);
        if (g_eglVerifiedFrameSerial.load(std::memory_order_relaxed) == frameSerial &&
            g_eglVerifiedContextGeneration.load(std::memory_order_relaxed) == g_syncContextGeneration) {
            return true;
        }
        if (g_EGLFuncs.eglGetCurrentContext && g_EGLFuncs.eglGetCurrentContext() != tuple.Context) {
            return false;
        }
        g_eglVerifiedFrameSerial.store(frameSerial, std::memory_order_relaxed);
        g_eglVerifiedContextGeneration.store(g_syncContextGeneration, std::memory_order_relaxed);
        return true;
    }

    BackendSyncHandle FenceSync() {
        // ES fences can only be created on the thread that owns the ES context
        // (Flywheel and friends fence on the render thread, which does).
        // Returning null makes the frontend fall back to an always-signaled
        // sync object.
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glFenceSync) {
            return nullptr;
        }
        GLsync esSync = g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (esSync == nullptr) {
            return nullptr;
        }
        return new GLESSyncObject{esSync, g_syncContextGeneration};
    }

    GLenum ClientWaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr) {
            return GL_ALREADY_SIGNALED;
        }
        // The creating ES context is gone: its GPU work either completed or
        // died with the context; waiting is meaningless either way.
        if (sync->contextGeneration != g_syncContextGeneration) {
            return GL_ALREADY_SIGNALED;
        }
        // Degraded path: a thread that does not own the ES context cannot
        // issue GL calls, so report signaled instead of blocking on state we
        // cannot observe. Fence waits normally arrive on the render thread,
        // which owns the context.
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glClientWaitSync) {
            return GL_ALREADY_SIGNALED;
        }
        return g_GLESFuncs.glClientWaitSync(sync->esSync, flags & GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
    }

    void WaitSync(BackendSyncHandle handle, GLbitfield flags, GLuint64 timeout) {
        (void)flags;
        (void)timeout;
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr || sync->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glWaitSync) {
            return;
        }
        // ES 3.0 requires flags == 0 and timeout == GL_TIMEOUT_IGNORED.
        g_GLESFuncs.glWaitSync(sync->esSync, 0, GL_TIMEOUT_IGNORED);
    }

    void DeleteSync(BackendSyncHandle handle) {
        auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr) {
            return;
        }
        if (sync->contextGeneration == g_syncContextGeneration && IsBackendContextCurrentOnThisThread() &&
            g_GLESFuncs.glDeleteSync) {
            g_GLESFuncs.glDeleteSync(sync->esSync);
        }
        // Otherwise the ES sync is abandoned; the ES context reclaims all of
        // its sync objects when it is destroyed.
        delete sync;
    }

    Bool GetSyncStatus(BackendSyncHandle handle) {
        const auto* sync = static_cast<GLESSyncObject*>(handle);
        if (sync == nullptr || sync->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glGetSynciv) {
            return true;
        }
        GLint status = GL_SIGNALED;
        GLsizei length = 0;
        g_GLESFuncs.glGetSynciv(sync->esSync, GL_SYNC_STATUS, 1, &length, &status);
        return status == GL_SIGNALED;
    }

    // GL timer queries, backed by GL_EXT_disjoint_timer_query. The desktop
    // tokens from glext.h are used throughout: GL_TIME_ELAPSED (0x88BF),
    // GL_TIMESTAMP (0x8E28), GL_QUERY_RESULT (0x8866) and
    // GL_QUERY_RESULT_AVAILABLE (0x8867) are numerically identical to their
    // _EXT counterparts.

    Bool AreTimerQueriesSupported() {
        return g_GLESCapabilities.SupportsDisjointTimerQuery && g_GLESFuncs.glGenQueries &&
               g_GLESFuncs.glDeleteQueries && g_GLESFuncs.glBeginQuery && g_GLESFuncs.glEndQuery &&
               g_GLESFuncs.glGetQueryObjectuiv && g_GLESFuncs.glQueryCounterEXT && g_GLESFuncs.glGetQueryObjectui64vEXT;
    }

    namespace {
        // The entry point the resolved tier's support ships, or null when there is none.
        MG_External::GLES::glTexBuffer_PTR ResolveTexBufferEntryPoint() {
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            switch (g_GLESCapabilities.TextureBufferSupport) {
            case Tier::ExtensionEXT:
                return g_GLESFuncs.glTexBufferEXT ? g_GLESFuncs.glTexBufferEXT : g_GLESFuncs.glTexBuffer;
            case Tier::ExtensionOES:
                return g_GLESFuncs.glTexBufferOES ? g_GLESFuncs.glTexBufferOES : g_GLESFuncs.glTexBuffer;
            case Tier::CoreEs32:
                return g_GLESFuncs.glTexBuffer;
            case Tier::None:
            default:
                return nullptr;
            }
        }

        MG_External::GLES::glTexBufferRange_PTR ResolveTexBufferRangeEntryPoint() {
            using Tier = MG_External::GLESCapabilities::TextureBufferTier;
            switch (g_GLESCapabilities.TextureBufferSupport) {
            case Tier::ExtensionEXT:
                return g_GLESFuncs.glTexBufferRangeEXT ? g_GLESFuncs.glTexBufferRangeEXT : g_GLESFuncs.glTexBufferRange;
            case Tier::ExtensionOES:
                return g_GLESFuncs.glTexBufferRangeOES ? g_GLESFuncs.glTexBufferRangeOES : g_GLESFuncs.glTexBufferRange;
            case Tier::CoreEs32:
                return g_GLESFuncs.glTexBufferRange;
            case Tier::None:
            default:
                return nullptr;
            }
        }
    } // namespace

    Bool AreBufferTexturesSupported() {
        // Both halves matter. The tier is what the driver ADVERTISES, and it is only meaningful
        // once the capabilities have been filled in; the resolved pointer is what MobileGL can
        // actually call, through the spelling that tier's support ships. Gating on the
        // unsuffixed name alone would call an entry point an EXT/OES driver never exported.
        return g_GLESCapabilities.TextureBufferSupport != MG_External::GLESCapabilities::TextureBufferTier::None &&
               ResolveTexBufferEntryPoint() != nullptr;
    }

    void CallTexBuffer(GLenum target, GLenum internalFormat, GLuint buffer) {
        MG_External::GLES::glTexBuffer_PTR entryPoint = ResolveTexBufferEntryPoint();
        if (entryPoint == nullptr) {
            return;
        }
        entryPoint(target, internalFormat, buffer);
    }

    Bool CallTexBufferRange(GLenum target, GLenum internalFormat, GLuint buffer, GLintptr offset, GLsizeiptr size) {
        MG_External::GLES::glTexBufferRange_PTR entryPoint = ResolveTexBufferRangeEntryPoint();
        if (entryPoint == nullptr) {
            return false;
        }
        entryPoint(target, internalFormat, buffer, offset, size);
        return true;
    }

    const char* GetBufferTextureTierName() {
        using Tier = MG_External::GLESCapabilities::TextureBufferTier;
        switch (g_GLESCapabilities.TextureBufferSupport) {
        case Tier::CoreEs32:
            return "core (ES 3.2)";
        case Tier::ExtensionEXT:
            return "GL_EXT_texture_buffer";
        case Tier::ExtensionOES:
            return "GL_OES_texture_buffer";
        case Tier::None:
        default:
            return "unsupported";
        }
    }

    BackendQueryHandle BeginTimeElapsedQuery() {
        // Query objects can only be created on the thread that owns the ES
        // context (MC's F3 profiler queries on the render thread, which
        // does). Returning null makes the frontend fall back to an
        // immediately available zero result.
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glBeginQuery(GL_TIME_ELAPSED, queryId);
        return new GLESQueryObject{queryId, g_syncContextGeneration};
    }

    void EndTimeElapsedQuery(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glEndQuery) {
            return;
        }
        // ES tracks the active query per target, not per object, so the
        // handle only guards the degraded paths above.
        g_GLESFuncs.glEndQuery(GL_TIME_ELAPSED);
    }

    BackendQueryHandle QueryCounterTimestamp() {
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glQueryCounterEXT(queryId, GL_TIMESTAMP);
        return new GLESQueryObject{queryId, g_syncContextGeneration};
    }

    // The core (non-timer) query targets - occlusion and transform feedback primitives.
    // Unlike the timer queries above these are core ES (no GL_EXT_disjoint_timer_query
    // needed), so they share one begin/end pair keyed on the glBeginQuery target.
    static Bool AreCoreQueriesSupported() {
        return g_GLESFuncs.glGenQueries && g_GLESFuncs.glDeleteQueries && g_GLESFuncs.glBeginQuery &&
               g_GLESFuncs.glEndQuery && g_GLESFuncs.glGetQueryObjectuiv;
    }

    static BackendQueryHandle BeginCoreQuery(GLenum target) {
        if (!IsBackendContextCurrentOnThisThread() || !AreCoreQueriesSupported()) {
            return nullptr;
        }
        GLuint queryId = 0;
        g_GLESFuncs.glGenQueries(1, &queryId);
        if (queryId == 0) {
            return nullptr;
        }
        g_GLESFuncs.glBeginQuery(target, queryId);
        return new GLESQueryObject{queryId, g_syncContextGeneration, target};
    }

    static void EndCoreQuery(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr || query->coreTarget == 0 || query->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glEndQuery) {
            return;
        }
        g_GLESFuncs.glEndQuery(query->coreTarget);
    }

    BackendQueryHandle BeginOcclusionQuery() {
        // ES only implements the boolean ANY_SAMPLES_PASSED variant, not an exact
        // GL_SAMPLES_PASSED count; the frontend already coerces ANY_SAMPLES_PASSED*
        // targets to boolean, and desktop GL_SAMPLES_PASSED reads a 0/1 approximation.
        return BeginCoreQuery(GL_ANY_SAMPLES_PASSED);
    }

    void EndOcclusionQuery(BackendQueryHandle handle) {
        EndCoreQuery(handle);
    }

    // GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN / GL_PRIMITIVES_GENERATED. The frontend
    // otherwise counts primitives on the CPU from the draw calls, which cannot see a
    // geometry shader's amplification; the real driver's counters are exact. Returning
    // null keeps that CPU accounting as the fallback.
    BackendQueryHandle BeginXfbPrimitivesQuery(Bool generated) {
        // GL_PRIMITIVES_GENERATED is only a legal query target from ES 3.2 on (it comes
        // with geometry shaders); issuing it earlier just leaves a stray GL_INVALID_ENUM
        // that some later unrelated glGetError would report as its own failure.
        if (generated && g_GLESCapabilities.GLESVersion.Major * 10 + g_GLESCapabilities.GLESVersion.Minor < 32) {
            return nullptr;
        }
        return BeginCoreQuery(generated ? GL_PRIMITIVES_GENERATED : GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    }

    void EndXfbPrimitivesQuery(BackendQueryHandle handle) {
        EndCoreQuery(handle);
    }

    Bool IsQueryResultAvailable(BackendQueryHandle handle) {
        const auto* query = static_cast<GLESQueryObject*>(handle);
        // Null/stale handles report available so the frontend proceeds to
        // GetQueryResult64, which finalizes them as zero. A thread that does
        // not own the ES context also reports available: GetQueryResult64
        // then returns false and the frontend keeps the handle for a later
        // read from the owning thread.
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glGetQueryObjectuiv) {
            return true;
        }
        GLuint available = GL_FALSE;
        g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT_AVAILABLE, &available);
        return available != GL_FALSE;
    }

    Bool GetQueryResult64(BackendQueryHandle handle, Bool wait, Uint64* outNanoseconds) {
        *outNanoseconds = 0;
        const auto* query = static_cast<GLESQueryObject*>(handle);
        // Null handles never had a GL query object, handles from a
        // since-destroyed ES context lost theirs, and missing entry points
        // can never produce a reading (belt and braces: the creators already
        // require them): zero is the FINAL result in all three cases, so
        // report it as produced and let the frontend cache it and release
        // the handle.
        if (query == nullptr || query->contextGeneration != g_syncContextGeneration ||
            !g_GLESFuncs.glGetQueryObjectuiv || (query->coreTarget == 0 && !g_GLESFuncs.glGetQueryObjectui64vEXT)) {
            return true;
        }
        // A thread that does not own the ES context cannot issue GL calls,
        // but the result still lands on the owning context eventually: report
        // "not obtainable yet" so the frontend keeps the handle and a later
        // availability poll / result read from the owning thread can still
        // produce the real value.
        if (!IsBackendContextCurrentOnThisThread()) {
            return false;
        }
        if (wait) {
            // Reading GL_QUERY_RESULT blocks in the driver until the result
            // lands, but only after the commands were flushed; flush once,
            // then poll availability for a bounded ~100ms before dropping to
            // a glFinish as the last resort (ClientWaitSync has no polling
            // loop to mirror - it delegates its timeout to the driver, which
            // a query-object read cannot do).
            if (g_GLESFuncs.glFlush) {
                g_GLESFuncs.glFlush();
            }
            constexpr Int kMaxAvailabilityPolls = 1000; // ~100ms at 100us per poll
            GLuint available = GL_FALSE;
            for (Int i = 0; i < kMaxAvailabilityPolls && available == GL_FALSE; ++i) {
                g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT_AVAILABLE, &available);
                if (available == GL_FALSE) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            if (available == GL_FALSE && g_GLESFuncs.glFinish) {
                g_GLESFuncs.glFinish();
            }
        }
        // GL_EXT_disjoint_timer_query's GPU_DISJOINT_EXT signal is
        // deliberately ignored: after a disjoint event (power state change,
        // context switch) the result may be garbage, which is tolerable for
        // an F3 GPU% readout, and consuming the latched flag here could hide
        // the event from another observer.
        if (query->coreTarget != 0) {
            GLuint result32 = 0;
            g_GLESFuncs.glGetQueryObjectuiv(query->queryId, GL_QUERY_RESULT, &result32);
            *outNanoseconds = static_cast<Uint64>(result32);
            return true;
        }
        GLuint64 result = 0;
        g_GLESFuncs.glGetQueryObjectui64vEXT(query->queryId, GL_QUERY_RESULT, &result);
        *outNanoseconds = static_cast<Uint64>(result);
        return true;
    }

    void DeleteBackendQuery(BackendQueryHandle handle) {
        auto* query = static_cast<GLESQueryObject*>(handle);
        if (query == nullptr) {
            return;
        }
        if (query->contextGeneration == g_syncContextGeneration && IsBackendContextCurrentOnThisThread() &&
            g_GLESFuncs.glDeleteQueries) {
            g_GLESFuncs.glDeleteQueries(1, &query->queryId);
        }
        // Otherwise the GL query object is abandoned; the ES context reclaims
        // all of its query objects when it is destroyed.
        delete query;
    }

    Int64 GetGpuTimestampNs() {
        // Synchronous GPU clock sample; 0 tells the frontend GL_TIMESTAMP
        // getter to fall back.
        if (!IsBackendContextCurrentOnThisThread() || !AreTimerQueriesSupported() || !g_GLESFuncs.glGetInteger64v) {
            return 0;
        }
        GLint64 timestamp = 0;
        g_GLESFuncs.glGetInteger64v(GL_TIMESTAMP, &timestamp);
        return static_cast<Int64>(timestamp);
    }

    Uint64 CurrentFrameSerial() {
        return g_currentFrameSerial.load(std::memory_order_relaxed);
    }
    Uint64 CompletedFrameSerial() {
        return g_completedFrameSerial.load(std::memory_order_relaxed);
    }

    namespace {
        // THE WATERMARK IS CONTIGUOUS. Fences signal in submission order only within ONE context,
        // and the serials are handed out to every native context of every session in turn, so the
        // highest signaled serial says nothing about an older fence of another context still
        // pending. A serial is complete when no live fence at or below it is pending.
        void PollFrameFences() {
            if (!g_GLESFuncs.glGetSynciv) return;
            Uint64 lowestPending = ~Uint64{0};
            for (FrameFence& slot : g_frameFenceRing) {
                if (!slot.sync) continue;
                if (slot.contextGeneration != g_syncContextGeneration) {
                    slot.sync = nullptr; // its context reclaimed it, and its work is over
                    continue;
                }
                GLint status = GL_SIGNALED;
                GLsizei length = 0;
                g_GLESFuncs.glGetSynciv(slot.sync, GL_SYNC_STATUS, 1, &length, &status);
                if (status == GL_SIGNALED) {
                    if (g_GLESFuncs.glDeleteSync) g_GLESFuncs.glDeleteSync(slot.sync);
                    slot.sync = nullptr;
                } else if (slot.serial < lowestPending) {
                    lowestPending = slot.serial;
                }
            }
            const Uint64 completed =
                lowestPending == ~Uint64{0} ? g_currentFrameSerial.load(std::memory_order_relaxed) : lowestPending - 1;
            if (completed > g_completedFrameSerial.load(std::memory_order_relaxed)) {
                g_completedFrameSerial.store(completed, std::memory_order_relaxed);
            }
        }

        FrameFence* OldestPendingFrameFence() {
            FrameFence* oldest = nullptr;
            for (FrameFence& slot : g_frameFenceRing) {
                if (!slot.sync || slot.contextGeneration != g_syncContextGeneration) continue;
                if (!oldest || slot.serial < oldest->serial) oldest = &slot;
            }
            return oldest;
        }
    } // namespace

    Bool WaitForFrameSerialCompleted(Uint64 serial, Uint64 timeoutNs) {
        if (CompletedFrameSerial() >= serial) return true;
        if (!IsBackendContextCurrentOnThisThread() || !g_GLESFuncs.glClientWaitSync) return false;
        // Oldest pending fence first: the watermark can only pass a serial once every older fence
        // (of any context) has signaled. Each fence was flushed when it was made (Present's swap),
        // so waiting from this context needs no flush of the other.
        for (int guard = 0; guard < kFrameFenceRingDepth + 1; ++guard) {
            PollFrameFences();
            if (CompletedFrameSerial() >= serial) return true;
            FrameFence* oldest = OldestPendingFrameFence();
            if (!oldest) return false;
            const GLenum status = g_GLESFuncs.glClientWaitSync(oldest->sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeoutNs);
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) return false;
        }
        PollFrameFences();
        return CompletedFrameSerial() >= serial;
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    namespace {
        // Once per thread: a latched session declines everything after this verb anyway, and the
        // apply thread is the session's own.
        thread_local Bool t_deviceLossLatched = false;

        Bool LatchOnResetStatus(const char* site, GLenum status) {
            if (status == GL_NO_ERROR) return false;
            // ONLY THE GUILTY CONTEXT ENDS ITS SESSION. The contexts ask for reset notification
            // (EnsureNativeContext) so the driver can name the context whose work hung or faulted the
            // GPU; a reset it reports to an INNOCENT context - one whose work only waited behind the
            // stall, the compositor's above all - is noted and the session goes on as it did before
            // contexts were told. Adreno 750 measured: the guilty context reads GUILTY once, after
            // which the driver drops everything it submits; innocent ones read NO_ERROR.
            static thread_local Bool innocentNoted = false;
            if (status == GL_INNOCENT_CONTEXT_RESET) {
                if (!innocentNoted) {
                    innocentNoted = true;
                    MGLOG_W("Espryt:%s - the driver reports a GPU reset this session's context did not cause "
                            "(GL_INNOCENT_CONTEXT_RESET); the session goes on",
                            site != nullptr ? site : "?");
                }
                return false;
            }
            t_deviceLossLatched = true;
            (void)MG_Pipe::MGPipeSessionLatch(MG_Pipe::MGPipeFatalFamily::DeviceLost,
                                              "MGPipe: Fatal{BackendDeviceLost, \"Espryt:%s\"} - this session's EGL "
                                              "context was reset (glGetGraphicsResetStatus 0x%x); the session ends "
                                              "and its client reads a lost context, every other session keeps running",
                                              site != nullptr ? site : "?", static_cast<unsigned>(status));
            return true;
        }
    } // namespace

    Bool LatchIfDeviceLost(const char* site, Bool debugKnob) {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return false;
        if (t_deviceLossLatched) return true;
        GLenum status = GL_NO_ERROR;
        if (g_GLESFuncs.glGetGraphicsResetStatus != nullptr && IsBackendContextCurrentOnThisThread()) {
            status = g_GLESFuncs.glGetGraphicsResetStatus();
        }
        if (status == GL_NO_ERROR && debugKnob && MG_Pipe::MGPipeDebugDeviceLossDue())
            status = GL_UNKNOWN_CONTEXT_RESET;
        return LatchOnResetStatus(site, status);
    }

    Bool LatchIfGuiltyBeforeApply() {
        if (MG_Config::Transport == MG_Config::TransportMode::Monolith) return false;
        if (t_deviceLossLatched) return true;
        // Asked every time the apply thread wakes, so it must leave nothing behind: not
        // IsBackendContextCurrentOnThisThread, whose once-per-frame EGL verification stamp it would
        // spend at the top of the loop (where the session may be between two of its contexts) - only
        // whether this thread holds a native context and the backend's ownership.
        if (g_GLESFuncs.glGetGraphicsResetStatus == nullptr || t_native.boundContext == EGL_NO_CONTEXT ||
            g_backendContextOwnerThread.load(std::memory_order_acquire) != std::this_thread::get_id())
            return false;
        return LatchOnResetStatus("apply", g_GLESFuncs.glGetGraphicsResetStatus());
    }
#endif

    void Present() {
#if MOBILEGL_BUILD_DISAGGREGATED
        if (LatchIfDeviceLost("present", /*debugKnob=*/false)) return;
#endif
        // Insert one fence per frame BEFORE the swap (eglSwapBuffers' implicit flush
        // makes it reachable), then non-blocking-poll prior frames' fences AFTER to
        // advance the completed-frame watermark that gates buffer-pool recycling.
        const Bool canFence = IsBackendContextCurrentOnThisThread() && g_GLESFuncs.glFenceSync;
        Uint64 presentedSerial = 0;
        if (canFence) {
            const Uint64 serial = g_currentFrameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
            presentedSerial = serial;
            FrameFence& slot = g_frameFenceRing[serial % kFrameFenceRingDepth];
            if (slot.sync && slot.contextGeneration == g_syncContextGeneration) {
                // The slot's fence is a ring-depth of serials old and may be another context's:
                // dropping it unsignaled would let the watermark pass work nobody waited for.
                if (g_GLESFuncs.glClientWaitSync) {
                    constexpr Uint64 kOldFenceWaitNs = 100ull * 1000 * 1000;
                    (void)g_GLESFuncs.glClientWaitSync(slot.sync, GL_SYNC_FLUSH_COMMANDS_BIT, kOldFenceWaitNs);
                }
                if (g_GLESFuncs.glDeleteSync) g_GLESFuncs.glDeleteSync(slot.sync);
            }
            slot = {g_GLESFuncs.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), g_syncContextGeneration, serial};
        }

#if MOBILEGL_BUILD_DISAGGREGATED
        // The session's frame boundary: the shared images its frame sampled get the frame's fence,
        // so their producers' next copies wait for it.
        SharedImageImpl::PublishPendingReads();
#endif

        // P14 S4: the session's surface, not the process's.
        //
        // EXT -> KHR -> plain. The loader fills eglSwapBuffersWithDamageEXT from whichever
        // spelling the driver provides (Adreno exposes only eglSwapBuffersWithDamageKHR), so
        // the null check below is the whole chain: a driver with neither presents with plain
        // eglSwapBuffers. Passing n_rects == 0 is not a partial present - the extension
        // defines it as "the entire surface is implicitly damaged and the behaviour is
        // equivalent to calling eglSwapBuffers" - and the wire protocol carries no damage
        // rectangles for the server to forward, so the full surface is what is meant anyway.
        //
        // The damage is the client's, from the present record (CurrentPresentDamage): GL window
        // coordinates, which is what the native call takes. Whole surface = no rectangles.
        //
        // ONLY A WINDOW SURFACE TAKES RECTANGLES: the platform's swap-with-damage hands them to the
        // surface's native window, and a pbuffer has none (Android's dereferences it regardless).
        NativeSessionState& session = ActiveNativeSession(); // before the lock: it takes it too
        const EGLSurface drawSurface = session.Draw;
        Bool windowSurface = false;
        if (drawSurface != EGL_NO_SURFACE) {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            if (const auto* entry = FindSessionSurfaceLocked(session, drawSurface))
                windowSurface = entry->Window != static_cast<NativeWindowType>(0);
        }
        EGLBoolean mglSwapped = EGL_FALSE;
        if (g_EGLFuncs.eglSwapBuffersWithDamageEXT != nullptr) {
            const MG_Util::Damage::Region& damage = MG_Backend::CurrentPresentDamage();
            EGLint rects[MG_Util::Damage::kMaxRects * 4] = {};
            const EGLint count = static_cast<EGLint>(MG_Util::Damage::NativeSwapRects(windowSurface, damage, rects));
            mglSwapped =
                g_EGLFuncs.eglSwapBuffersWithDamageEXT(g_Display, drawSurface, count > 0 ? rects : nullptr, count);
            // A DRIVER THAT NAMES THE ENTRY POINT AND THEN REFUSES THE SWAP. Some vendor EGLs
            // export eglSwapBuffersWithDamageEXT and answer EGL_FALSE on a window surface without
            // setting an error (measured on a 4.14 vendor kernel with an Adreno driver; the plain
            // entry point presents the same surface happily). Keeping the frame on that answer
            // queues nothing at all: the compositor composites into a window whose buffer queue
            // never sees a buffer. Fall back to the plain call instead of dropping the frame.
            if (mglSwapped == EGL_FALSE && g_EGLFuncs.eglSwapBuffers != nullptr) {
                MGLOG_W_ONCE("DirectGLES: this driver's eglSwapBuffersWithDamageEXT refused a window "
                             "surface; presenting through eglSwapBuffers for the rest of the process");
                mglSwapped = g_EGLFuncs.eglSwapBuffers(g_Display, drawSurface);
            }
        } else if (g_EGLFuncs.eglSwapBuffers != nullptr) {
            mglSwapped = g_EGLFuncs.eglSwapBuffers(g_Display, drawSurface);
        }
        NoteDrawSurfaceMoved(); // a window surface takes a new size at its swap
        if (drawSurface != EGL_NO_SURFACE) {
            const std::lock_guard<std::mutex> lock(g_nativeRegistryMutex);
            if (auto* entry = FindSessionSurfaceLocked(session, drawSurface)) ++entry->Presents;
        }
        // A frame boundary: the next frame's age is asked anew, and its window buffer use flushed.
        t_ageAskedSurface = EGL_NO_SURFACE;
        t_windowBufferFlushIssued = false;

        // P15: MOBILEGL_FRAMES_IN_FLIGHT, Espryt's half. Magma bounds the frames the GPU has
        // queued with its frame contexts (frame S+1 reuses the context of frame S+1-N); this is
        // the same bound on the ES driver: after presenting S, wait for S+1-N to complete. The
        // platform's own queueBuffer throttle already waits for S-1, so only N = 1 is stricter
        // than what Android does by itself; larger N is the same contract stated, not a change.
        const Uint32 framesInFlight = MG_Config::Features.FramesInFlight;
        if (presentedSerial != 0 && framesInFlight != 0 && framesInFlight < static_cast<Uint32>(kFrameFenceRingDepth) &&
            g_GLESFuncs.glClientWaitSync && presentedSerial + 1 > framesInFlight) {
            const Uint64 target = presentedSerial + 1 - framesInFlight;
            const FrameFence& oldest = g_frameFenceRing[target % kFrameFenceRingDepth];
            if (oldest.sync && oldest.serial == target && oldest.contextGeneration == g_syncContextGeneration) {
                constexpr Uint64 kFramesInFlightWaitNs = 1000ull * 1000 * 1000;
                (void)g_GLESFuncs.glClientWaitSync(oldest.sync, GL_SYNC_FLUSH_COMMANDS_BIT, kFramesInFlightWaitNs);
            }
        }

        if (canFence) PollFrameFences();

        // After the watermark advanced: retire grown-away ring stores and record the
        // frame's ring high-water marks for slot reclamation.
        BufferImpl::UboRingOnPresent();
        BufferImpl::UnpackRingOnPresent();
        BufferImpl::UploadRingOnPresent();
        BufferImpl::TrimBufferPool();

        // THE frame boundary for the MGPipe counters: publish this frame's plots, fold the
        // frame into the run totals and, every 120th frame, emit the summary line.
        if (MG_Util::PipeStats::Enabled()) {
            MG_Util::PipeStats::OnPresent();
        }
    }
} // namespace MobileGL::MG_Backend::DirectGLES
