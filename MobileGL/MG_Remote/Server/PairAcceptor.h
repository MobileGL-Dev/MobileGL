// MobileGL - MobileGL/MG_Remote/Server/PairAcceptor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 PAIR. A SOCKET CLIENT'S TWO CONNECTIONS ARE PAIRED BY WHO THEY ARE, NOT BY WHEN THEY ARRIVED.
//
// SocketTransport::ConnectTo opens two connections to one listener - control, then aux (the
// SCM_RIGHTS socket; on tcp:// the data connection). The server used to take "the next two accepted
// connections, first is control" (SocketTransport::AcceptPair). That was a protocol only while
// nothing else could reach the listener, and on the device it was not (B0-CROSS-APP.md F2): the
// server app's readiness probe - one connect() and close() - became the next client's control
// connection, that session read the probe's socket and ended ("control peer closed before sending
// a first frame"), and the client got no Welcome. Two clients connecting at the same moment could
// just as well swap halves.
//
// Now each connection's first frame is a PairBind (protocol.fbs, control revision 4): the client's
// nonce, the same on both, and which half it is. This acceptor keeps every accepted connection in a
// small set until it has presented one, and hands out a control and an aux connection only when
// their nonces match. What it does with everything else:
//   - a connection that closes having sent nothing (a readiness probe) is closed, quietly;
//   - a first frame that is not a PairBind is refused by name. A Hello is a client from before
//     PairBind: it is asked what the session would ask, in the session's order - the token, then the
//     wire (Handshake.h) - so it hears Refuse{WireFingerprint} as it would from any session, and one
//     that passes both is still refused, because it cannot be paired;
//   - a connection that has not presented a PairBind, or whose partner has not presented the same
//     nonce, within the budget is dropped - a control connection with a refusal, an aux connection
//     (whose client reads descriptors there, not frames) by closing it - and nobody else's
//     connection is touched;
//   - past kPairMaxUnpaired connections the oldest one that has not identified itself (else the
//     oldest) is dropped the same way.
//
// It never reads a byte past a PairBind (FirstFrameAssembler): on control the Hello follows it, and
// the session reads that itself; on aux the descriptors follow it.
//
// Stateful because pairs complete out of order: with A-control, B-control, B-aux, A-aux, B is
// handed out first and A-control has to still be here for the call that hands out A.

#pragma once

#if !defined(_WIN32)

#include "../Transport/PairBind.h"
#include "../Transport/SocketTransport.h"
#include "PreAuthGate.h" // FirstFrameAssembler

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    // How long an accepted connection may stay unpaired: to present its PairBind, and then to be
    // joined by its partner. The old AcceptPair's wait for a second connection, and ConnectTo's
    // budget for opening it.
    inline constexpr std::uint32_t kPairBudgetMs = 2000;
    // Connections held unpaired at once. A client needs two, and the rendezvous is local.
    inline constexpr std::size_t kPairMaxUnpaired = 16;

    // The refusal details a peer (and the log) sees.
    inline constexpr const char* kPairNoBind = "no PairBind within the pairing budget";
    inline constexpr const char* kPairNoPartner =
        "no aux connection presented this connection's pair nonce within the pairing budget";
    inline constexpr const char* kPairHelloFirst =
        "the control connection's first frame is a Hello, not a PairBind (control revision 4 pairs by nonce)";
    inline constexpr const char* kPairDuplicate = "a connection with this pair nonce and role is already waiting";
    inline constexpr const char* kPairFull = "too many connections are waiting to be paired";
    inline constexpr const char* kPairStopped = "the server stopped accepting before this connection was paired";

    class PairAcceptor {
    public:
        using Clock = std::chrono::steady_clock;

        // `listenFd` stays the caller's. `budgetMs` is kPairBudgetMs outside the unit test.
        explicit PairAcceptor(int listenFd, std::uint32_t budgetMs = kPairBudgetMs);
        // Every connection still held is let go of (kPairStopped, Busy), with shutdown(2).
        ~PairAcceptor();
        PairAcceptor(const PairAcceptor&) = delete;
        PairAcceptor& operator=(const PairAcceptor&) = delete;

        // One client's pair, waiting at most `timeoutMs` for it: OK with `outServer` holding the
        // control connection as its stream and the aux connection; MOBILEGL_ERR_TIMEOUT when no pair
        // completed in time (connections still unpaired are KEPT for the next call); and
        // MOBILEGL_ERR_TRANSPORT_CLOSED when the listener itself failed.
        MobileGLResult Accept(std::uint32_t timeoutMs, std::unique_ptr<Transport::SocketTransport>& outServer);

        // A forked session child's copy of every held descriptor, closed WITHOUT shutdown(2) - that
        // would disconnect the supervisor's copy too - and without a word: they are the supervisor's.
        void CloseInForkedChild();

        // Connections accepted and not yet handed out as a pair.
        std::size_t Unpaired() const { return m_waiting.size(); }

    private:
        struct Waiting {
            int fd = -1;
            Clock::time_point deadline{};
            FirstFrameAssembler frame;
            bool bound = false; // its PairBind has arrived
            bool aux = false;
            std::uint8_t nonce[Transport::kPairNonceBytes] = {};
        };

        MobileGLResult AcceptNew(Clock::time_point now);
        void Read(Waiting& waiting);
        void Judge(Waiting& waiting);
        void RefuseUnpairable(Waiting& waiting);
        void Expire(Clock::time_point now);
        // A control connection, or one that never said which it is, is refused (Busy when it was
        // crowded out or the server stopped, else Authentication) and closed; an aux connection is
        // closed - its client reads descriptors there, never frames.
        void Drop(Waiting& waiting, bool busy, const char* detail);
        void Compact();

        int m_listener;
        std::uint32_t m_budgetMs;
        std::vector<Waiting> m_waiting;                 // accept order, oldest first
        std::deque<std::pair<int, int>> m_ready;        // {control, aux}, not yet handed out
        Clock::time_point m_acceptResumeAt{};
    };

} // namespace MobileGL::MG_Remote::Server

#endif
