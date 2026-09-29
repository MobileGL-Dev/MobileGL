// MobileGL - MobileGL/MG_Remote/Server/PairAcceptor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 PAIR: see PairAcceptor.h for what is paired, what is refused and why.

#include "PairAcceptor.h"

#if !defined(_WIN32)

#include "../Handshake.h" // AuthenticatePeerToken / ValidatePeerHandshake, for a Hello sent first
#include "../Transport/WireLog.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace MobileGL::MG_Remote::Server {

    namespace {
        namespace Protocol = ::MobileGL::Wire;
        using Transport::SocketTransport;
        using Transport::TransportRole;
        using Step = FirstFrameAssembler::Step;

        // A pause after accept(2) ran out of descriptors or memory: the connection waits in the
        // backlog and the listener is fine (TcpSupervisor pauses the same way).
        constexpr std::uint32_t kAcceptPauseMs = 100;

        std::int64_t MillisecondsUntil(PairAcceptor::Clock::time_point when, PairAcceptor::Clock::time_point now) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
        }

        // Whatever the peer already sent, read and dropped (non-blocking, bounded), so the close
        // after a refusal does not become an RST that discards the refusal (ServerMain.cpp's
        // DrainUnread; RefuseBusy's comment has the reason).
        void DrainUnread(int fd) {
            char sink[4096];
            for (int rounds = 0; rounds < 256; ++rounds) {
                if (::recv(fd, sink, sizeof(sink), MSG_DONTWAIT) <= 0) return;
            }
        }

        void Refuse(SocketTransport& connection, Protocol::RefuseCode code, const char* detail) {
            ::flatbuffers::FlatBufferBuilder builder(256);
            auto refusal = Protocol::CreateRefuseDirect(builder, code, detail);
            auto envelope = Protocol::CreateCtrlEnvelope(builder, Protocol::CtrlMsg::Refuse, refusal.Union());
            Protocol::FinishCtrlEnvelopeBuffer(builder, envelope);
            (void)connection.SendFrame({builder.GetBufferPointer(), builder.GetSize()});
            Transport::WireLogError("MG_Remote server: Refuse{%s} %s", Protocol::EnumNameRefuseCode(code), detail);
        }

        // Refuses a connection held as a bare descriptor, and closes it (with shutdown(2): here the
        // descriptor is only ever the supervisor's).
        void RefuseAndClose(int fd, Protocol::RefuseCode code, const char* detail) {
            SocketTransport connection(fd, -1, TransportRole::Server);
            DrainUnread(fd);
            Refuse(connection, code, detail);
        }
    } // namespace

    PairAcceptor::PairAcceptor(int listenFd, std::uint32_t budgetMs) : m_listener(listenFd), m_budgetMs(budgetMs) {}

    PairAcceptor::~PairAcceptor() {
        for (auto& waiting : m_waiting) Drop(waiting, true, kPairStopped);
        for (const auto& pair : m_ready) {
            RefuseAndClose(pair.first, Protocol::RefuseCode::Busy, kPairStopped);
            ::close(pair.second);
        }
    }

    void PairAcceptor::CloseInForkedChild() {
        for (auto& waiting : m_waiting)
            if (waiting.fd >= 0) ::close(waiting.fd);
        for (const auto& pair : m_ready) {
            ::close(pair.first);
            ::close(pair.second);
        }
        m_waiting.clear();
        m_ready.clear();
    }

    MobileGLResult PairAcceptor::Accept(std::uint32_t timeoutMs, std::unique_ptr<SocketTransport>& outServer) {
        outServer.reset();
        if (m_listener < 0) return MOBILEGL_ERR_INVALID_ARGUMENT;
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        std::vector<pollfd> fds;
        std::vector<std::size_t> polled; // index into m_waiting of fds[1..]
        for (;;) {
            if (!m_ready.empty()) {
                const auto pair = m_ready.front();
                m_ready.pop_front();
                outServer = std::make_unique<SocketTransport>(pair.first, pair.second, TransportRole::Server);
                return MOBILEGL_OK;
            }
            auto now = Clock::now();
            Expire(now);
            // While accepting is paused the listener sits out the poll (it stays readable and would
            // spin the loop), and the pause's end is a wakeup.
            const bool acceptPaused = now < m_acceptResumeAt;
            fds.clear();
            polled.clear();
            fds.push_back({acceptPaused ? -1 : m_listener, POLLIN, 0});
            std::int64_t wait = std::clamp<std::int64_t>(MillisecondsUntil(until, now), 0, 1 << 30);
            for (std::size_t index = 0; index < m_waiting.size(); ++index) {
                const Waiting& waiting = m_waiting[index];
                wait = std::clamp<std::int64_t>(MillisecondsUntil(waiting.deadline, now) + 1, 0, wait);
                // A bound connection is not read again: on control its Hello follows, and that is
                // the session's to read. It only waits for its partner or its deadline.
                if (waiting.bound) continue;
                fds.push_back({waiting.fd, POLLIN, 0});
                polled.push_back(index);
            }
            if (acceptPaused) wait = std::clamp<std::int64_t>(MillisecondsUntil(m_acceptResumeAt, now) + 1, 0, wait);
            const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(wait));
            if (ready < 0 && errno != EINTR) {
                Transport::WireLogError("MG_Remote server: the pairing poll failed: %s", std::strerror(errno));
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            if (ready > 0) {
                for (std::size_t slot = 0; slot < polled.size(); ++slot)
                    if (fds[slot + 1].revents != 0) Read(m_waiting[polled[slot]]);
                Compact();
                if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
                    const MobileGLResult accepted = AcceptNew(Clock::now());
                    if (accepted != MOBILEGL_OK) return accepted;
                }
            }
            if (m_ready.empty() && Clock::now() >= until) return MOBILEGL_ERR_TIMEOUT;
        }
    }

    // Accepts everything the listener has; each connection waits in the set until it identifies.
    MobileGLResult PairAcceptor::AcceptNew(Clock::time_point now) {
        for (int burst = 0; burst < 64; ++burst) {
            int fd = -1;
            const MobileGLResult accepted = SocketTransport::AcceptOne(m_listener, 0, &fd);
            if (accepted == MOBILEGL_ERR_TIMEOUT) return MOBILEGL_OK;
            if (accepted == MOBILEGL_ERR_OUT_OF_MEMORY) {
                m_acceptResumeAt = now + std::chrono::milliseconds(kAcceptPauseMs);
                MGLOG_W("MG_Remote server: accept paused for %u ms: out of descriptors or memory with %zu "
                        "connections waiting to be paired",
                        kAcceptPauseMs, m_waiting.size());
                return MOBILEGL_OK;
            }
            if (accepted != MOBILEGL_OK) {
                Transport::WireLogError("MG_Remote server: the listener failed while pairing (rc=%d)",
                                        static_cast<int>(accepted));
                return MOBILEGL_ERR_TRANSPORT_CLOSED;
            }
            if (m_waiting.size() >= kPairMaxUnpaired) {
                // The oldest connection that has not said who it is goes first; with none, the oldest.
                auto victim = std::find_if(m_waiting.begin(), m_waiting.end(),
                                           [](const Waiting& waiting) { return !waiting.bound; });
                if (victim == m_waiting.end()) victim = m_waiting.begin();
                MGLOG_W("MG_Remote server: %zu connections are waiting to be paired; the oldest %s one is dropped",
                        m_waiting.size(), victim->bound ? "identified" : "unidentified");
                Drop(*victim, true, kPairFull);
                m_waiting.erase(victim);
            }
            Waiting waiting;
            waiting.fd = fd;
            waiting.deadline = now + std::chrono::milliseconds(m_budgetMs);
            m_waiting.push_back(std::move(waiting));
        }
        return MOBILEGL_OK;
    }

    void PairAcceptor::Read(Waiting& waiting) {
        switch (waiting.frame.Pump(waiting.fd)) {
            case Step::NeedMore:
                return;
            case Step::Complete:
                Judge(waiting);
                return;
            case Step::EndedEmpty:
                // A readiness probe - a connect() and a close() - costs an accept and nothing else.
                MGLOG_D("MG_Remote server: a connection closed before presenting a PairBind (a probe); dropped");
                ::close(waiting.fd);
                waiting.fd = -1;
                return;
            case Step::NotAFrame:
                RefuseAndClose(waiting.fd, Protocol::RefuseCode::MalformedHello, kFirstFrameNotAFrame);
                waiting.fd = -1;
                return;
            case Step::EndedPartial:
                RefuseAndClose(waiting.fd, Protocol::RefuseCode::MalformedHello, kFirstFrameTruncated);
                waiting.fd = -1;
                return;
        }
    }

    // A complete first frame. A PairBind either completes a pair or waits for its partner.
    void PairAcceptor::Judge(Waiting& waiting) {
        std::uint8_t nonce[Transport::kPairNonceBytes] = {};
        bool aux = false;
        if (!Transport::DecodePairBind(waiting.frame.Payload(), nonce, &aux)) {
            RefuseUnpairable(waiting);
            return;
        }
        for (auto& other : m_waiting) {
            if (&other == &waiting || other.fd < 0 || !other.bound ||
                !Transport::ConstantTimeNonceMatch(other.nonce, nonce))
                continue;
            if (other.aux == aux) {
                MGLOG_W("MG_Remote server: a second %s connection presented a pair nonce that is already waiting",
                        aux ? "aux" : "control");
                Drop(waiting, false, kPairDuplicate);
                return;
            }
            m_ready.emplace_back(aux ? other.fd : waiting.fd, aux ? waiting.fd : other.fd);
            other.fd = -1;
            waiting.fd = -1;
            return;
        }
        waiting.bound = true;
        waiting.aux = aux;
        std::memcpy(waiting.nonce, nonce, sizeof(nonce));
    }

    // A first frame that is not a PairBind. Nothing can be paired with it, and it is told so by name.
    void PairAcceptor::RefuseUnpairable(Waiting& waiting) {
        const auto& payload = waiting.frame.Payload();
        const int fd = waiting.fd;
        waiting.fd = -1;
        SocketTransport connection(fd, -1, TransportRole::Server);
        DrainUnread(fd);
        const FirstFrameShape shape = ClassifyFirstFrame(payload.data(), payload.size());
        if (shape == FirstFrameShape::Hello) {
            // A client from before PairBind (control revision < 4) writes its Hello first. It is
            // asked what RunSession would ask, in RunSession's order - the token, then the wire - so
            // an older client hears Refuse{WireFingerprint} exactly as a session would answer it.
            const auto* hello = Protocol::GetCtrlEnvelope(payload.data())->msg_as_Hello();
            if (AuthenticatePeerToken(connection, hello->token()) != MOBILEGL_OK) return;
            if (ValidatePeerHandshake(connection, hello->abiMajor(), hello->abiMinor(), hello->wireFingerprint(),
                                      hello->buildFingerprint() ? hello->buildFingerprint()->c_str() : nullptr,
                                      hello->dialMode()) != MOBILEGL_OK)
                return;
            Refuse(connection, Protocol::RefuseCode::MalformedHello, kPairHelloFirst);
            return;
        }
        if (shape == FirstFrameShape::DataBind) {
            // RunSession's own words for a DataBind that reaches a listener with no session for it.
            Refuse(connection, Protocol::RefuseCode::Authentication, "data connection names no live session");
            return;
        }
        Refuse(connection, Protocol::RefuseCode::MalformedHello, FirstFrameRefusalDetail(shape));
    }

    // Past its deadline a connection is dropped; nothing that is still inside its budget is touched.
    void PairAcceptor::Expire(Clock::time_point now) {
        for (auto& waiting : m_waiting) {
            if (waiting.fd < 0 || now < waiting.deadline) continue;
            if (waiting.bound)
                MGLOG_W("MG_Remote server: a%s connection's partner never presented its pair nonce within %u ms; "
                        "dropped",
                        waiting.aux ? "n aux" : " control", m_budgetMs);
            Drop(waiting, false, waiting.bound ? kPairNoPartner : kPairNoBind);
        }
        Compact();
    }

    void PairAcceptor::Drop(Waiting& waiting, bool busy, const char* detail) {
        if (waiting.fd < 0) return;
        if (waiting.bound && waiting.aux) {
            ::shutdown(waiting.fd, SHUT_RDWR);
            ::close(waiting.fd);
        } else {
            RefuseAndClose(waiting.fd, busy ? Protocol::RefuseCode::Busy : Protocol::RefuseCode::Authentication,
                           detail);
        }
        waiting.fd = -1;
    }

    void PairAcceptor::Compact() {
        m_waiting.erase(std::remove_if(m_waiting.begin(), m_waiting.end(),
                                       [](const Waiting& waiting) { return waiting.fd < 0; }),
                        m_waiting.end());
    }

} // namespace MobileGL::MG_Remote::Server

#endif
