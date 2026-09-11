// MobileGL - MobileGL/MG_Remote/Server/ServerSession.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P5 c0 stubs for package s1.

#include "ServerSession.h"

#include <MG_Util/Debug/Log.h>

#include <cstdlib>

namespace MobileGL::MG_Remote::Server {

#define MGP5_C0_STUB(what)                                                                                             \
    do {                                                                                                               \
        MGLOG_F("MGPipe: Fatal{UnimplementedServerSession, \"%s\"} - P5 package s1 has not landed "                    \
                "this yet; c0 shipped the signature only",                                                             \
                what);                                                                                                 \
        std::abort();                                                                                                  \
    } while (0)

    ServerSession* ServerSession::Active() { return nullptr; }

    MobileGLResult ServerSession::Accept(Transport::ITransport&) { MGP5_C0_STUB("ServerSession::Accept"); }

    MobileGLResult ServerSession::PublishCapsSnapshot() {
        MGP5_C0_STUB("ServerSession::PublishCapsSnapshot");
    }

    Transport::RingConsumer& ServerSession::CommandRing() { return m_commands; }
    Transport::RingControl& ServerSession::Control() { MGP5_C0_STUB("ServerSession::Control"); }
    Wire::SegmentTable& ServerSession::Segments() { return m_segments; }
    PipeApplier& ServerSession::Applier() { return m_applier; }
    ReplyPool& ServerSession::Replies() { return m_replies; }

    Transport::Doorbell& ServerSession::ConsumerDoorbell() {
        MGP5_C0_STUB("ServerSession::ConsumerDoorbell");
    }
    Transport::Doorbell& ServerSession::ProducerDoorbell() {
        MGP5_C0_STUB("ServerSession::ProducerDoorbell");
    }

#undef MGP5_C0_STUB

} // namespace MobileGL::MG_Remote::Server
