// MobileGL - MobileGL/MG_Remote/Server/PairAcceptor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Pairs a socket client's two connections (control, then aux: the SCM_RIGHTS socket, or the data
// connection on tcp://) by nonce rather than arrival order, so a readiness probe or two clients
// connecting at once cannot be mispaired. Each connection's first frame is a PairBind with the
// client's nonce and its half; nothing past it is read. A connection that closes having sent
// nothing is closed quietly; a Hello is checked token then wire, as a session would, then refused;
// one unpaired after the budget, or the oldest unidentified one past kPairMaxUnpaired, is dropped
// without touching others. Stateful because pairs complete out of order.

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
