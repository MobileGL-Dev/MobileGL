// MobileGL - MobileGL/MG_Remote/Server/ServerLoop.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for package v1 - the phase's highest-risk package.

#include "ServerLoop.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Server {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedServerLoop, \"%s\"} - P5 package v1 has not landed "                       \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    MobileGLResult ServerLoop::Start(ServerSession&) { MGP5_C0_STUB("ServerLoop::Start"); }

    void ServerLoop::Stop() { MGP5_C0_STUB("ServerLoop::Stop"); }

    // Not a stub: teardown asks this to decide whether to Kill and join at all, and a teardown
    // helper that aborts when the thread was never started is a hang in the shutdown path.
    Bool ServerLoop::Running() const { return m_running; }

    MG_Backend::BackendObject* ServerLoop::Backend() { MGP5_C0_STUB("ServerLoop::Backend"); }

    MobileGLResult ServerLoop::RunOnApplyThread(ControlWork, void*) {
        MGP5_C0_STUB("ServerLoop::RunOnApplyThread");
    }

    ServerLoop& ServerLoopInstance() {
        // ID-8: leak at exit, like every MG_Remote singleton.
        static ServerLoop& instance = *new ServerLoop{};
        return instance;
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Server
