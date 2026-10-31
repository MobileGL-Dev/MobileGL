// MobileGL - MobileGL/MG_Remote/Server/SessionRuntime.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// One served session's process-local state ({ServerSession, ServerLoop, PipeInputs}) and the
// per-thread routing behind the singletons: on a thread inside a ThreadSessionScope,
// ServerSessionInstance(), ServerLoopInstance() and gPipeInputs resolve to that thread's session;
// elsewhere they resolve to the process-wide defaults. ServerMain::RunSession opens the scope on
// its control thread and ServerLoop on the session's apply thread. The in-process client+server
// shape (ClientSession::Start) opens none, so its app thread and applier keep sharing the one
// PipeInputs block the verb barrier relies on; keep it that way.
// See docs/Disaggregated/design/11-state-ownership.md.

#pragma once
#include <Includes.h>
#include <MG_Backend/MGPipe/PipeInputs.h>

#include "ServerLoop.h"
#include "ServerSession.h"

#include <cstdint>

namespace MobileGL::MG_Remote::Server {

    class SessionRuntime {
    public:
        SessionRuntime();
        ~SessionRuntime();
        SessionRuntime(const SessionRuntime&) = delete;
        SessionRuntime& operator=(const SessionRuntime&) = delete;

        ServerSession& Session() { return m_session; }
        ServerLoop& Loop() { return m_loop; }
        MG_Pipe::PipeInputs& Inputs() { return m_inputs; }

        // The latch domain this session owns (FatalFunnel.h). A named fault latched by one session
        // must not end another, and must not be cleared by another's reap.
        int LatchDomain() const { return m_latchDomain; }

    private:
        ServerSession m_session;
        ServerLoop m_loop;
        MG_Pipe::PipeInputs m_inputs;
        int m_latchDomain = 0;
    };

    namespace Detail {
        // The session of the CALLING thread, or nullptr when it is not a session's own thread.
        SessionRuntime* CurrentRuntime();
        ServerSession* CurrentSession();
        ServerLoop* CurrentLoop();
        // What MG_Pipe's per-thread resolver asks for: the calling thread's block, or nullptr for
        // "use the process-wide one".
        MG_Pipe::PipeInputs* CurrentInputs();

        // Installs the process-wide hooks the per-session blocks need (the PipeInputs resolver).
        // Idempotent, and called by ThreadSessionScope itself so no bring-up order can miss it.
        void InstallSessionRuntimeHooks();

        // THE SCOPE. Makes `runtime` the calling thread's session for as long as it lives: the
        // session/loop the singleton accessors answer with, the PipeInputs block gPipeInputs reads,
        // this session's segment resolver and this session's latch domain. Restores what was there
        // before on the way out (two nested scopes are not a shape anything uses, but a thread that
        // served one session and is then reused must not keep answering with the dead one).
        class ThreadSessionScope {
        public:
            explicit ThreadSessionScope(SessionRuntime& runtime);
            ~ThreadSessionScope();
            ThreadSessionScope(const ThreadSessionScope&) = delete;
            ThreadSessionScope& operator=(const ThreadSessionScope&) = delete;

        private:
            SessionRuntime* m_previous = nullptr;
            int m_previousLatchDomain = 0;
        };
    } // namespace Detail

    // Sessions accepted and not yet closed in THIS process. 1 is the old shape; more is this
    // slice's. ServerSession::Close gates the process-wide hooks (the reverse-channel table, the
    // segment resolver) on it, and ServerMain logs it beside each session it starts.
    Uint32 LiveServerSessionCount();

} // namespace MobileGL::MG_Remote::Server
