// MobileGL - MobileGL/MG_Remote/Server/ServerSession.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server half of a session: the consumer side of the rings, the handshake reply, the caps
// snapshot. Owner: package s1. Signatures by c0.
//
// The four segment sizes are already pinned by ProtocolSmokeTest.cpp:72 and are not up for
// re-derivation here: SEG_CMD 8 MiB, SEG_STAGE 32 MiB, SEG_REPLY 8 MiB, SEG_EVENT 256 KiB.
// MOBILEGL_IPC_RING_MB and MOBILEGL_IPC_STAGE_MB move the first two; the ring caps ONE record
// at half its size, so the default 8 MiB caps a record at 4 MiB (R-10).
//
// THE TWO DOORBELL ACCESSORS ARE ON THE CONCRETE CLASS, NOT ON ITransport
// (InProcessTransport.h:64-68). P5 decides this now rather than letting P6 discover it: the
// SESSION owns the pair and hands out references, so ITransport stays the dumb control-plane
// interface its header says it is and SocketTransport does not have to grow two accessors it
// has no natural home for. Discovering this in P6 would mean re-laying one package's call sites.

#pragma once
#include <Includes.h>

#include <MG_Pipe/MGPipe.h>

#include "../Transport/Doorbell.h"
#include "../Transport/ITransport.h"
#include "../Transport/Ring.h"
#include "../Wire/PipeWireCodec.h"
#include "PipeApplier.h"

namespace MobileGL::MG_Remote::Server {

    class ServerSession {
    public:
        static ServerSession* Active();

        // Maps the four segments, answers Hello with Welcome, and publishes the first
        // CapsSnapshot. The ABI assertion (CapsCodec.h) happens HERE, before a single record is
        // decoded: sizeof(DynamicBackendParameters), sizeof(MGPCaps), sizeof(GLFunctionsTable)
        // and the build fingerprint must match, and a mismatch is Fatal{AbiMismatch}.
        MobileGLResult Accept(Transport::ITransport& transport);

        // Re-publishes the whole snapshot. R-12: a SECOND arrival IS the invalidation signal,
        // which is how DirectGLES - which has no OnCapsInvalidated producer - tells the client
        // its InitCapabilities re-ran, without any dev-shaped backend edit. It is also the
        // re-open signal for "the server's ES context died and its rings were dropped", the
        // event MGPipeCallbacks has no eleventh slot for (MGPipeCallbacks.h:56-58).
        MobileGLResult PublishCapsSnapshot();

        Transport::RingConsumer& CommandRing();
        Transport::RingControl& Control();
        Wire::SegmentTable& Segments();
        PipeApplier& Applier();
        ReplyPool& Replies();

        // The client rings this one; the apply thread parks on it.
        Transport::Doorbell& ConsumerDoorbell();
        // The server rings this one, but only when producerParked is set (a store to a shared
        // cache line otherwise burns a big core for a whole frame on a phone).
        Transport::Doorbell& ProducerDoorbell();

    private:
        Transport::RingConsumer m_commands;
        Wire::SegmentTable m_segments;
        PipeApplier m_applier;
        ReplyPool m_replies;
    };

} // namespace MobileGL::MG_Remote::Server
