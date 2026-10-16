// MobileGL - MobileGL/MG_Remote/Transport/SocketTransport.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// ITransport over a connected stream socket: the control plane (handshake, surface ops, resync,
// aux requests, fatals). The data plane bypasses ITransport and crosses through ILink.
// Framing.h's FrameReader reassembles the arbitrary fragments a socket delivers; descriptors
// travel by SCM_RIGHTS on a separate aux socket (FdPassing.h), never on the framed stream.
// The transport owns its stream and aux fds and closes both in Shutdown and the destructor.
// Shutdown is idempotent and closes both directions: the peer's reads return 0, its sends fail.

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
        // The server is a standing process reached by name; neither side inherits the other's
        // fds. A Unix endpoint uses two connections, control then aux, because SCM_RIGHTS
        // ancillary data must not interleave with the framed control stream. Each presents a
        // PairBind (one shared nonce, which half) as its first frame, and the server pairs them by
        // that nonce, never by arrival order (Server/PairAcceptor.h), so a probe's bare connect or
        // two clients at once cannot mis-pair. `path` is a filesystem AF_UNIX path (bound 0600),
        // '@' plus a Linux abstract name, or tcp://host:port.
        static MobileGLResult Listen(const std::string& path, int* outListenFd);

        // The client's half: connects twice to `path`, each connection presenting a PairBind
        // (the same fresh nonce; control, then aux) as its first frame. The server's half is
        // Server::PairAcceptor.
        static MobileGLResult ConnectTo(const std::string& path, std::uint32_t timeoutMs,
                                        std::unique_ptr<SocketTransport>& outClient);

        // P11 B1: the client's half when SOMEBODY ELSE connected - the `fd:<control>,<aux>`
        // endpoint. A broker in the server's own app (the only context SELinux lets connect to its
        // endpoint) opened both connections and handed the client ends over; this adopts them. Both
        // must be open, connected AF_UNIX stream sockets and distinct (refused by name otherwise,
        // nothing written). They become CLOEXEC. `pairedAtHandoff`: whoever connected them already
        // presented the PairBind pair (ID-P11-12: the server app's broker does, at hand-off), and
        // nothing is written; otherwise this presents one - a fresh nonce, control then aux - exactly
        // as ConnectTo does, before the Hello. TRANSPORT_CLOSED when that send failed: the descriptors
        // are then left open, because the server's refusal may be waiting on the control connection.
        static MobileGLResult AdoptConnectedPair(int controlFd, int auxFd,
                                                 std::unique_ptr<SocketTransport>& outClient,
                                                 bool pairedAtHandoff = false);

        // ---- PH-7 (4), ID-P7-3: a TCP data connection is BOUND, not paired --------
        //
        // The server used to pair a session's two connections by ARRIVAL ORDER inside a window
        // (AcceptPair): first is control, second is data. That is a protocol only while nothing
        // sits between the two processes. Over a Windows `adb forward` the two arrive reordered,
        // the data connection is read as control, and every session child exits 67 waiting for a
        // Hello that went down the other socket - and anybody who can reach the port can race a
        // connection into the gap and become somebody else's data plane. So on TCP the session
        // is ONE connection until it is authenticated; the server mints `Welcome.dataNonce`,
        // and the client then opens its data connection and presents the nonce as that
        // connection's first frame (DataBind). The server matches the value, not the order.
        // Unix endpoints keep their two connections from the start (the second is the SCM_RIGHTS
        // socket the Welcome's descriptors cross), and since P11 PAIR they too are matched by a
        // value - the client's PairBind nonce - not by order.

        // Is anything listening at `path` (a ConnectTo endpoint: an AF_UNIX path, '@' + an abstract
        // name, or tcp://host:port)? One non-blocking connect per address, bounded by `timeoutMs`
        // in total and never retried, then closed without a byte written - the server's pairing
        // drops a connection that closes before presenting a PairBind. For deciding whether to
        // offer a display at all, so it must not stall: a refused or absent endpoint answers at
        // once, and only an unresponsive TCP peer can take the whole bound.
        static bool Probe(const std::string& path, std::uint32_t timeoutMs);

        // Connects the control connection only on `tcp://`; identical to ConnectTo otherwise.
        static MobileGLResult ConnectControl(const std::string& path, std::uint32_t timeoutMs,
                                             std::unique_ptr<SocketTransport>& outClient);
        // Opens one more connection to `path` and writes `firstFrame` (a CtrlEnvelope carrying
        // DataBind, framed here) as its first bytes. *outFd is the caller's to attach a
        // StreamLink to.
        static MobileGLResult ConnectDataConnection(const std::string& path, std::uint32_t timeoutMs,
                                                    MobileGLByteSpan firstFrame, int* outFd);
        // Accepts ONE connection (TCP options applied, CLOEXEC; EINTR and ECONNABORTED re-polled -
        // the listener is still good). MOBILEGL_ERR_TIMEOUT when none.
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
