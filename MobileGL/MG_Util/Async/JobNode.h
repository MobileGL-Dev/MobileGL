// MobileGL - MobileGL/MG_Util/Async/JobNode.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_Util/Types.h>
#include <MG_State/GLState/ErrorState/ErrorCode.h>
#include <MG_State/GLState/ErrorState/ErrorInfo.h>

#include <condition_variable>

namespace MobileGL::MG_Util::Async {
    enum class JobState : Uint8 {
        Pending,   // constructed, not started; may still be sitting in a queue
        Running,   // a worker is inside RunBody()
        Complete,  // RunBody() returned normally and the node's outputs are readable
        Cancelled, // abandoned before it started, cancelled mid-run, or threw
    };

    // A GL error a job body wants to raise. Nothing in the compile/link pipeline produces
    // one today (see the design's section 6: GL defines compile/link *failure* as
    // COMPILE_STATUS/LINK_STATUS plus an info log, not as a GL error, which is exactly why
    // asynchronous compilation is legal at all), and JobNode::Finish asserts the vector is
    // still empty in debug builds. The mechanism exists so that the day a body genuinely
    // needs to raise one, the fix is to append here and let the join replay it on the GL
    // thread - not to reach for pGLContext->RecordError() from a worker.
    struct DeferredError {
        Uint64 sequence = 0; // job-global monotonic counter, assigned at record time
        ErrorCode code = ErrorCode::NoError;
        UniquePtr<ErrorInfo> info;
    };

    struct JobDiagnostics {
        Vector<DeferredError> errors; // replayed, in ascending `sequence`, by the join
        Vector<String> logLines;      // worker-side MGLOG text, flushed in order by the join
    };

    // The scheduling primitive every asynchronous compile and link is built on. A node owns
    // its inputs and its outputs; a worker reads only the former and writes only the latter,
    // which is what makes the "no worker touches GL-thread state" invariant structural
    // rather than review-enforced.
    //
    // State machine, and the only legal transitions:
    //   Pending -> Running   (a worker picked the node up)
    //   Pending -> Cancelled (cancelled before any worker started it)
    //   Running -> Complete  (RunBody() returned normally)
    //   Running -> Cancelled (cancelled mid-run, or RunBody() threw)
    // Complete and Cancelled are terminal and the node is immutable afterwards, so every
    // reader that observed IsTerminal() may read the outputs without further synchronization.
    //
    // enable_shared_from_this because a dependency edge outlives its registrar: a node that
    // posts itself from another node's continuation (ProgramLinkTask::OnDepSettled) has to
    // hand the pool a strong reference from inside itself. Every JobNode is therefore created
    // through MakeShared - a stack-allocated one may not use SubmitAfter-style chaining.
    class JobNode : public std::enable_shared_from_this<JobNode> {
    public:
        JobNode() = default;
        virtual ~JobNode() = default;
        JobNode(const JobNode&) = delete;
        JobNode& operator=(const JobNode&) = delete;

        // Lock-free and non-blocking - safe from any thread, including a pool thread.
        Bool IsTerminal() const;
        Bool IsComplete() const; // Complete only; this is what backs GL_COMPLETION_STATUS_KHR
        Bool IsCancelled() const; // settled AS cancelled - the outcome, not the request
        JobState State() const;

        // The cancellation *request*, which is what a body polls to bail out early: a
        // running job stays Running until its body returns, so IsCancelled() is still false
        // at that point. Kept separate from IsCancelled() precisely so the two questions
        // ("should I stop?" and "did it end up cancelled?") cannot be confused.
        Bool IsCancellationRequested() const;

        // Blocks until the node is terminal. GL thread only: a job body that waited on
        // another job could deadlock the whole pool, so this asserts it is not called from a
        // pool thread. Dependencies are resolved by posting late (see ProgramLinkTask::
        // SubmitAfter), never by waiting from inside a body.
        void Wait();

        // Cooperative and non-blocking. A node that has not started yet goes terminal
        // immediately, so anything waiting on it or chained behind it is released rather
        // than stranded; a running node is flagged and settles as Cancelled when its body
        // returns. Because every node owns its inputs and writes only into itself, an
        // abandoned node is always safe to simply drop - the caller never waits.
        void Cancel();

        // Runs `fn` once, when this node goes terminal. If the node is ALREADY terminal,
        // `fn` runs on the calling thread before OnTerminal returns. Exactly-once in both
        // directions: the callback is either handed to the finishing thread or run inline,
        // never both.
        //
        // A continuation must not throw. It is dispatched from whichever thread drove this
        // node terminal, which on the pool side is an Asio handler - an exception escaping
        // one propagates out of thread_pool::run() and terminates the process. The dispatcher
        // contains a throw anyway (see RunContinuation) so that one broken continuation
        // cannot strand the others, but the continuation itself is where the guarantee
        // belongs: whoever registers one owns the "and it cannot fail" argument, because the
        // dispatcher can only log, never repair. ProgramLinkTask::OnDepSettled is the worked
        // example - it catches internally and cancels itself, because a link that is never
        // posted is a joiner blocked forever.
        void OnTerminal(std::function<void()> fn);

        // Runs the body on the calling thread. The synchronous path (async disabled,
        // context-less internal shaders, a pool that has been stopped) goes through here, so
        // that "inline" and "on a worker" differ only in which thread executes RunBody().
        void RunInline();

        JobDiagnostics diagnostics;

    protected:
        virtual void RunBody() = 0;

    private:
        friend class ShaderCompilePool;

        // Pool entry point: cancel check -> RunBody() (exceptions contained) -> Finish().
        void Run();
        Bool TryTransition(JobState from, JobState to);

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<JobState> m_state{JobState::Pending};
        std::atomic<Bool> m_cancelled{false};
        Vector<std::function<void()>> m_continuations;
    };

    // Replays a settled node's worker-side diagnostics on the calling thread: log lines
    // first, in the order the body produced them, then any deferred GL error in ascending
    // `sequence`. GL thread only - it is the join that calls this, which is exactly the
    // point at which a deferred error becomes indistinguishable from one a serial
    // implementation would have raised inside glCompileShader/glLinkProgram (an application
    // cannot observe a pending job's effects by any other route).
    //
    // Drains what it replays, so calling it twice on one node is a no-op the second time.
    // Must be called with the node terminal.
    void ApplyDeferredDiagnostics(JobNode& node);
} // namespace MobileGL::MG_Util::Async
