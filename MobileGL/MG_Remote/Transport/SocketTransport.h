// MobileGL - MobileGL/MG_Remote/Transport/SocketTransport.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// ITransport over a connected stream socket. Package `so` (CONTRACT-P6.md).
//
// WHY THIS IS THE FIRST P6 CODE THAT RUNS. It is the whole control plane for a
// second process, and it needs NOTHING from the second process to be correct:
// CONTRACT-P6 and the spawn plan both require it green over a plain
// socketpair() with two threads, running the WHOLE InProcessTransportTest
// suite, BEFORE a child is ever forked. Same discipline as SessionRings.h's
// "the attach half is the half P6 replaces, so exercise it now" - a transport
// first exercised on the day the process appears is a transport whose bugs all
// arrive at once.
//
// WHAT IT IS NOT. It is not the data plane. The hot path bypasses ITransport
// entirely (ITransport.h) and crosses through ILink, which is a separate seam
// with a separate implementation. This class carries the handshake, surface
// ops, resync, aux requests and fatals - the rare, variable-length,
// must-evolve traffic.
//
// THREE THINGS IT DOES NOT REINVENT, because they are already in the tree and
// already tested:
//   Framing.h    - AppendFrame writes [u32 'MGLF'][u32 len][payload]; FrameReader
//                  is a real byte-stream reassembler with the BUFFER_TOO_SMALL
//                  keep-the-message semantics ITransport requires. A socket
//                  hands you arbitrary fragments, which is precisely what
//                  FrameReader was written for and what InProcessTransport's
//                  message queue never needed.
//   FdPassing.h  - SCM_RIGHTS over a DEDICATED SOCK_DGRAM aux socket, so an fd
//                  offer is one datagram and cannot be half-consumed.
//   Doorbell.h   - SocketDoorbell already exists, is unit-tested, and has NO
//                  production construction site until now. It was written for
//                  this and is priced as new work here, not as already paid for.
//
// LIFETIME. The transport owns its stream fd and its aux fd and closes both in
// Shutdown/dtor. Shutdown is idempotent and tears down BOTH directions, which
// is what ITransport promises and what a socket does anyway: after either end
// calls it, the peer's reads return 0 and its sends fail.

#pragma once

#include "Framing.h"
#include "ITransport.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace MobileGL::MG_Remote::Transport {

    class SocketTransport final : public ITransport {
    public:
        // Creates a connected pair and hands back one transport per end, with
        // the aux (fd-passing) socketpair already made. POSIX only; on a
        // platform without SCM_RIGHTS this refuses BY NAME rather than handing
        // back a transport whose ShareFd would surprise its caller later.
        static MobileGLResult CreatePair(std::unique_ptr<SocketTransport>& outClient,
                                         std::unique_ptr<SocketTransport>& outServer);

        // Adopts an already-connected stream fd - what `sm` uses on each side of
        // the fork, where fd 3 is the stream and fd 4 is the aux socket.
        // `auxFd` may be -1: the transport then answers MOBILEGL_ERR_UNSUPPORTED
        // to ShareFd/ReceiveFd, which is the honest answer for a link that
        // cannot pass descriptors (CONTRACT-P6 rule G: a transport declares what
        // it supports and may not invent a third answer).
        SocketTransport(int streamFd, int auxFd, TransportRole role);
        ~SocketTransport() override;

        MobileGLResult SendFrame(MobileGLByteSpan bytes) override;
        MobileGLResult ReceiveFrame(MobileGLMutableByteSpan buffer, std::uint64_t* outSize,
                                    std::uint32_t timeoutMs) override;
        std::uint64_t PeekFrameSize() override;

        MobileGLResult ShareFd(int fd, MobileGLByteSpan sideband) override;
        MobileGLResult ReceiveFd(int* outFd, MobileGLMutableByteSpan sideband,
                                 std::uint64_t* outSidebandSize, std::uint32_t timeoutMs) override;

        void Shutdown() override;
        TransportRole Role() const override { return m_role; }

        // For `sm`'s process discipline and for the arm-proof gate: the fds this
        // transport owns, so a test can assert the child inherited exactly these.
        int StreamFd() const { return m_streamFd; }
        int AuxFd() const { return m_auxFd; }

    private:
        // Pulls whatever the socket has into the reassembler. Returns
        // TRANSPORT_CLOSED on a clean EOF with nothing buffered.
        MobileGLResult PumpOnce(std::uint32_t timeoutMs);

        int m_streamFd = -1;
        int m_auxFd = -1;
        TransportRole m_role = TransportRole::Client;

        // ITransport's threading rule: callers serialise sends, and receives may
        // run on one dedicated reader thread concurrently with those sends. So
        // the two directions get one mutex each rather than one shared - a
        // single lock would make a blocked receive stall every send, which is
        // exactly the wedge the control plane must not have.
        std::mutex m_sendMutex;
        std::mutex m_recvMutex;

        FrameReader m_reader;
        bool m_closed = false;
        bool m_failed = false; // framing violated: latched, never recovers
    };

} // namespace MobileGL::MG_Remote::Transport
