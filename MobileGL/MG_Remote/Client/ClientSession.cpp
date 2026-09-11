// MobileGL - MobileGL/MG_Remote/Client/ClientSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for packages s1 (construction, handshake) and c1 (barrier, reply read).

#include "ClientSession.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Client {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedClientSession, \"%s\"} - P5 packages s1/c1 have not "                      \
                "landed this yet; c0 shipped the signature only",                                                      \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    // Null, not a Fatal: MG_Backend::Init() asks whether a session exists before it decides to
    // install the remote backend object, and that question has a legitimate "no" - it is the
    // monolith answer. Every call that PRESUMES a session aborts instead.
    ClientSession* ClientSession::Active() { return nullptr; }

    MobileGLResult ClientSession::Start(MG_Config::TransportMode, const String&) {
        MGP5_C0_STUB("ClientSession::Start");
    }

    void ClientSession::Stop() { MGP5_C0_STUB("ClientSession::Stop"); }

    Wire::PipeWireEncoder& ClientSession::Encoder() { return m_encoder; }

    CapsMirror& ClientSession::Caps() { return CapsMirrorInstance(); }

    Uint64 ClientSession::EmitAndWait(MG_Pipe::MGPWireOp, const void*, Uint64, const void*, Uint64,
                                      void*, Uint64, Int32*) {
        MGP5_C0_STUB("ClientSession::EmitAndWait");
    }

    Bool ClientSession::BarrierArmed() const { return m_barrierArmed; }

    // False, not a Fatal, for both: these are the R-1 mutual-exclusion assertion's two probes,
    // and an assertion helper that aborts when asked is worse than useless.
    Bool ClientSession::InBarrierWait() { return false; }
    Bool ClientSession::ApplyThreadIsInsideApplier() { return false; }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Client
