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
#include <atomic>

#include "Framing.h"
#include "ITransport.h"

#include <cstdint>
#include <string>
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

        // ---- independent processes: listen / connect -----------------------
        //
        // THE TWO PROCESSES ARE NOT COUPLED BY INHERITANCE. An earlier shape of
        // this package forked the server and handed it fds 3..6; that can never
        // be the end state, where the server is a standing application and the
        // client connects to it from somewhere else entirely - possibly from
        // another kernel. So the rendezvous is a NAME, and both sides are
        // started by whoever starts them.
        //
        // TWO CONNECTIONS, not one. SCM_RIGHTS needs a socket of its own: an fd
        // offer is a sendmsg whose ancillary data rides with specific BYTES, and
        // interleaving those bytes with the framed control stream would make the
        // frame reassembler and the descriptor receiver race for the same bytes.
        // So the client connects twice to the same listening path - first
        // connection is control, second is aux - and the server accepts them in
        // that order. One name, no extra configuration, and FdPassing keeps the
        // dedicated socket it was written for.
        //
        // `path` is a filesystem AF_UNIX path. ARCHITECTURE.md §15.1 ruled
        // against one for the FORK shape, where the fds were inherited and a
        // path would have been a strictly larger attack surface for no gain.
        // Independent processes have to name a rendezvous somehow, and a path
        // with 0600 permissions in a private directory is the portable answer;
        // the abstract namespace is Linux-only and TCP would be reachable off
        // the machine.
        static MobileGLResult Listen(const std::string& path, int* outListenFd);

        // Accepts one client's TWO connections, in order.
        static MobileGLResult AcceptPair(int listenFd, std::uint32_t timeoutMs,
                                         std::unique_ptr<SocketTransport>& outServer);

        // The client's half: connects twice to `path`.
        static MobileGLResult ConnectTo(const std::string& path, std::uint32_t timeoutMs,
                                        std::unique_ptr<SocketTransport>& outClient);

        // ---- PH-7 (4), ID-P7-3: a TCP data connection is BOUND, not paired --------
        //
        // AcceptPair/ConnectTo pair a session's two connections by ARRIVAL ORDER inside a
        // window: first is control, second is data. That is a protocol only while nothing sits
        // between the two processes. Over a Windows `adb forward` the two arrive reordered, the
        // data connection is read as control, and every session child exits 67 waiting for a
        // Hello that went down the other socket - and anybody who can reach the port can race a
        // connection into the gap and become somebody else's data plane. So on TCP the session
        // is ONE connection until it is authenticated; the server mints `Welcome.dataNonce`,
        // and the client then opens its data connection and presents the nonce as that
        // connection's first frame (DataBind). The server matches the value, not the order.
        // Unix endpoints keep AcceptPair: their second connection is the SCM_RIGHTS socket of a
        // same-user rendezvous, not a data plane anybody else can reach.

        // Connects the control connection only on `tcp://`; identical to ConnectTo otherwise.
        static MobileGLResult ConnectControl(const std::string& path, std::uint32_t timeoutMs,
                                             std::unique_ptr<SocketTransport>& outClient);
        // Opens one more connection to `path` and writes `firstFrame` (a CtrlEnvelope carrying
        // DataBind, framed here) as its first bytes. *outFd is the caller's to attach a
        // StreamLink to.
        static MobileGLResult ConnectDataConnection(const std::string& path, std::uint32_t timeoutMs,
                                                    MobileGLByteSpan firstFrame, int* outFd);
        // Accepts ONE connection (TCP options applied, CLOEXEC). MOBILEGL_ERR_TIMEOUT when none.
        // MOBILEGL_ERR_OUT_OF_MEMORY when accept(2) ran out of descriptors or memory (EMFILE,
        // ENFILE, ENOBUFS, ENOMEM): the connection stays in the backlog and the listener is fine,
        // so a caller that serves many peers can wait and try again (the TCP supervisor does);
        // TRANSPORT_CLOSED for every other failure.
        static MobileGLResult AcceptOne(int listenFd, std::uint32_t timeoutMs, int* outFd);
        // Reads EXACTLY one framed message from a raw socket - the header, then the payload,
        // and not one byte more. A DataBind is followed on the same connection by StreamLink's
        // own frames, which a buffered reader would swallow. PROTOCOL_MISMATCH for a bad magic
        // or a payload over `maxBytes`, TIMEOUT when the frame did not complete in time.
        static MobileGLResult ReceiveOneFrame(int fd, std::uint32_t timeoutMs, std::uint64_t maxBytes,
                                              std::vector<std::uint8_t>* outPayload);
        // `size` bytes from the kernel CSPRNG. Never a PRNG fallback: an error is an error.
        static MobileGLResult MintNonce(std::uint8_t* out, std::size_t size);

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
        bool IsTcp() const { return m_tcp; }
        // TCP's second connection belongs exclusively to the data plane.
        // Taking it disables all descriptor passing on this transport.
        int TakeDataFd();
        // Parent of a session child closes its duplicate without shutdown(2),
        // which would also disconnect the child's inherited socket.
        void CloseLocalCopy();
        // End requests while retaining the receive side for final diagnostics/EOF.
        MobileGLResult ShutdownSend();
        // Bytes received but not yet returned as a whole frame. Non-zero after a TRANSPORT_CLOSED
        // means the peer closed INSIDE a frame, which the steady-state control loop names (fuzz
        // arm 1) rather than treating it as the client's normal end.
        std::uint64_t BufferedBytes();

    private:
        // Pulls whatever the socket has into the reassembler. Returns
        // TRANSPORT_CLOSED on a clean EOF with nothing buffered.
        MobileGLResult PumpOnce(std::uint32_t timeoutMs);

        int m_streamFd = -1;
        int m_auxFd = -1;
        bool m_tcp = false;
        TransportRole m_role = TransportRole::Client;

        // ITransport's threading rule: callers serialise sends, and receives may
        // run on one dedicated reader thread concurrently with those sends. So
        // the two directions get one mutex each rather than one shared - a
        // single lock would make a blocked receive stall every send, which is
        // exactly the wedge the control plane must not have.
        std::mutex m_sendMutex;
        std::mutex m_recvMutex;

        FrameReader m_reader;
        std::atomic<bool> m_closed{false};
        std::atomic<bool> m_readClosed{false};
        bool m_failed = false; // framing violated: latched, never recovers
    };

} // namespace MobileGL::MG_Remote::Transport
