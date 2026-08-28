// MobileGL - MobileGL/MG_Util/Async/JobNode.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "JobNode.h"
#include "ShaderCompilePool.h"

#include <MG_State/GLState/Core.h>

namespace MobileGL::MG_Util::Async {
    namespace {
        Bool IsTerminalState(const JobState state) {
            return state == JobState::Complete || state == JobState::Cancelled;
        }

        // Job BODIES have been contained since stage 1 (JobNode::Run); continuations were
        // not, and stage 4 introduces the first real ones. A continuation runs on whichever
        // thread drove the node terminal - for a compile that finished on a worker, that is
        // inside an Asio handler, where an escaping exception means thread_pool::run()
        // rethrows and the process terminates. It would also skip every continuation after
        // it in the list, stranding unrelated dependents.
        //
        // Containing it here is a backstop, not the contract: a continuation cannot be
        // repaired from the outside (the dispatcher has no idea what the callback was for),
        // so the registrar still owns "this cannot fail". See JobNode::OnTerminal.
        void RunContinuation(const std::function<void()>& continuation) {
            if (!continuation) return;
            try {
                continuation();
            } catch (const std::exception& e) {
                MGLOG_E_ONCE("JobNode: a terminal continuation threw (%s); it has been contained, but whatever it "
                        "was going to do did not happen",
                        e.what());
            } catch (...) {
                MGLOG_E_ONCE("JobNode: a terminal continuation threw a non-std exception; it has been contained, "
                        "but whatever it was going to do did not happen");
            }
        }
    } // namespace

    Bool JobNode::IsTerminal() const { return IsTerminalState(m_state.load(std::memory_order_acquire)); }

    Bool JobNode::IsComplete() const { return m_state.load(std::memory_order_acquire) == JobState::Complete; }

    Bool JobNode::IsCancelled() const { return m_state.load(std::memory_order_acquire) == JobState::Cancelled; }

    Bool JobNode::IsCancellationRequested() const { return m_cancelled.load(std::memory_order_acquire); }

    JobState JobNode::State() const { return m_state.load(std::memory_order_acquire); }

    // The single place a node changes state. Keeping every transition here is what makes the
    // continuation list exactly-once: the same critical section that publishes the terminal
    // state also takes ownership of the callbacks, so a concurrent OnTerminal either lands in
    // the list before the swap or sees the terminal state and runs inline - never neither and
    // never both.
    Bool JobNode::TryTransition(const JobState from, const JobState to) {
        const Bool terminal = IsTerminalState(to);
        Vector<std::function<void()>> continuations;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (m_state.load(std::memory_order_relaxed) != from) return false;
            m_state.store(to, std::memory_order_release);
            if (terminal) continuations.swap(m_continuations);
        }
        if (!terminal) return true;

        m_cv.notify_all();
        // Run continuations OUTSIDE the lock: a continuation is free to call back into this
        // node (IsComplete, State) and, in the link-dependency case, to post the dependent
        // job to the pool from whichever thread drove this node terminal. Individually
        // contained, so one broken dependent cannot strand the rest of the list.
        for (auto& continuation : continuations) {
            RunContinuation(continuation);
        }
        return true;
    }

    void JobNode::Run() {
        if (m_cancelled.load(std::memory_order_acquire)) {
            TryTransition(JobState::Pending, JobState::Cancelled);
            return;
        }
        // Loses to a concurrent Cancel() that already took the node terminal, and to a second
        // dispatch of the same node. Either way there is nothing left to do.
        if (!TryTransition(JobState::Pending, JobState::Running)) return;

        try {
            RunBody();
        } catch (const std::exception& e) {
            // Asio propagates an exception escaping a handler out of thread_pool::run(),
            // which means std::terminate for the whole process. Every job boundary contains
            // it and reports the job as Cancelled; the joining GL thread then sees a node
            // that produced no result, which is the same shape as an abandoned node.
            diagnostics.logLines.push_back(
                {MOBILEGL_LOG_LEVEL_DEBUG, std::format("Job body threw: {}", e.what())});
            TryTransition(JobState::Running, JobState::Cancelled);
            return;
        } catch (...) {
            diagnostics.logLines.push_back(
                {MOBILEGL_LOG_LEVEL_DEBUG, String("Job body threw a non-std exception")});
            TryTransition(JobState::Running, JobState::Cancelled);
            return;
        }

        // Debug-only tripwire for the design's section 6 invariant: a compile or link body
        // must not need to raise a GL error. Anything that does belongs in the GL-thread
        // prologue of CompileShader_State / LinkProgram_State, next to the active-XFB relink
        // rejection that already works that way.
        MOBILEGL_ASSERT(diagnostics.errors.empty(),
                        "JobNode: a job body recorded %zu deferred GL error(s); compile and link bodies must not "
                        "raise GL errors (see the P1 design, section 6)",
                        diagnostics.errors.size());

        TryTransition(JobState::Running,
                      m_cancelled.load(std::memory_order_acquire) ? JobState::Cancelled : JobState::Complete);
    }

    void JobNode::RunInline() { Run(); }

    void JobNode::Wait() {
        // Invariant I4, mechanically enforced: no job body ever blocks on another job, so the
        // pool can never deadlock with all its workers waiting on each other.
        MOBILEGL_ASSERT(!ShaderCompilePool::IsPoolThread(),
                        "JobNode::Wait() called from a pool thread; job dependencies must be resolved by posting "
                        "late (SubmitAfter), never by waiting from inside a body");
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return IsTerminalState(m_state.load(std::memory_order_relaxed)); });
    }

    void JobNode::Cancel() {
        m_cancelled.store(true, std::memory_order_release);
        // A node that never reached a worker settles right here. Doing this rather than
        // waiting for a dispatch that may never come is what lets every cancel site
        // (glShaderSource over a pending compile, glDeleteProgram, teardown) drop the node
        // without a wait and without stranding a dependent link job behind it.
        TryTransition(JobState::Pending, JobState::Cancelled);
    }

    void JobNode::OnTerminal(std::function<void()> fn) {
        if (!fn) return;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!IsTerminalState(m_state.load(std::memory_order_relaxed))) {
                m_continuations.push_back(Move(fn));
                return;
            }
        }
        // Already terminal: the caller's thread runs it, through the same guard the deferred
        // path uses. OnTerminal is reached from Link()'s GL-thread prologue as well as from a
        // worker, and glLinkProgram is not a place an exception may escape from either.
        RunContinuation(fn);
    }

    void ApplyDeferredDiagnostics(JobNode& node) {
        MOBILEGL_ASSERT(!ShaderCompilePool::IsPoolThread(),
                        "ApplyDeferredDiagnostics() called from a pool thread; deferred diagnostics exist precisely "
                        "so that a worker never touches the GL error state");
        MOBILEGL_ASSERT(node.IsTerminal(),
                        "ApplyDeferredDiagnostics() called on a job that has not settled; its diagnostics are still "
                        "being written");

        if (!node.diagnostics.logLines.empty()) {
            Vector<DeferredLogLine> lines;
            lines.swap(node.diagnostics.logLines);
            for (const DeferredLogLine& line : lines) {
                // Per-line severity, because a shipped build compiles MGLOG_D away entirely
                // and a verdict that only this channel records would vanish with it. The
                // levels are the compile-time constants, so a suppressed one costs nothing
                // beyond the string the worker already built.
                switch (line.level) {
                case MOBILEGL_LOG_LEVEL_INFO:
                    MGLOG_I("%s", line.text.c_str());
                    break;
                case MOBILEGL_LOG_LEVEL_WARN:
                    MGLOG_W("%s", line.text.c_str());
                    break;
                case MOBILEGL_LOG_LEVEL_ERROR:
                    MGLOG_E("%s", line.text.c_str());
                    break;
                default:
                    MGLOG_D("%s", line.text.c_str());
                    break;
                }
            }
        }

        if (node.diagnostics.errors.empty()) return;
        Vector<DeferredError> errors;
        errors.swap(node.diagnostics.errors);
        // Ascending sequence == job-enqueue order == the order a serial implementation would
        // have recorded them in, which is what decides WHICH payload the application sees:
        // MobileGL implements GL's sticky-flag semantics, so a repeat of an already-pending
        // code is discarded and only the first occurrence of each code survives.
        std::sort(errors.begin(), errors.end(),
                  [](const DeferredError& a, const DeferredError& b) { return a.sequence < b.sequence; });
        if (!MG_State::pGLContext) return;
        for (DeferredError& error : errors) {
            MG_State::pGLContext->RecordError(error.code, Move(error.info));
        }
    }
} // namespace MobileGL::MG_Util::Async
