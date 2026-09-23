// MobileGL - MobileGL/MG_Remote/Server/PreAuthGate.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// PH-7 (5), CONTRACT-P7 §12, notes/p7/ph-f.md §6.4. THE TCP SUPERVISOR AUTHENTICATES BEFORE IT
// FORKS, AND AN UNAUTHENTICATED PEER'S WORK IS BOUNDED.
//
// Two exposures were on the tree before wave 2-F and were recorded by its review:
//   (a) the supervisor forked a session child for every accepted TCP connection and let the child
//       read the Hello - so any peer that could reach the port cost the server a fork, and a
//       silent one held that child for up to 10 s;
//   (b) the child ran ValidatePeerHandshake before AuthenticatePeerToken, so an unauthenticated
//       peer that sent a Hello with a wrong fingerprint was answered with Refuse{WireFingerprint}
//       carrying THIS build's wire fingerprint (and Refuse{BuildFingerprint} its build stamp).
//
// The supervisor now reads every new connection's FIRST FRAME itself, without blocking on any one
// peer, under a short deadline; a Hello is authenticated there, token first, and only an
// authenticated Hello is ever forked for. The pieces that decide are here, free of I/O policy and
// of the supervisor's loop, so they are unit-testable (PreAuthGateTest):
//
//   PreAuthKnobs        the four knobs, read once from the environment;
//   ClassifyFirstFrame  what a complete first frame IS - the fuzz arm 1 malformations each land
//                       on a named shape here, and the identifier is asked before the verifier;
//   FirstFrameAssembler reads EXACTLY one frame off a non-blocking socket, a few bytes at a time,
//                       and never a byte past it: a DataBind is followed on its connection by
//                       StreamLink's own frames, which belong to the session child;
//   AuthBackoff         failures per peer address, and the exponentially growing window during
//                       which that address is refused at accept, before a byte of it is read.
//
// What a failure IS, for the backoff: a Hello whose token did not match, a first frame that is not
// a well-formed control frame, a connection that let the deadline pass, and a connection that HELD
// its pending slot for kPreAuthProbeGraceMs or longer and then closed (or was displaced, below)
// without completing a frame. What it is NOT: a connection that closed within kPreAuthProbeGraceMs
// of its accept without sending a byte (a port probe costs the server an accept and nothing else),
// a DataBind that names no live session (a stale data connection of a session that just ended is a
// legitimate client's), and a refusal the backoff itself issued (counting those would let anybody
// behind a shared address keep that address locked out for ever).
//
// Fix round (the f2-auth critic's reproduction). The first cut counted no silent close at all, and
// the pending slots were one pool for everybody: one address could open MOBILEGL_IPC_PREAUTH_MAX
// silent connections, close them just before the deadline, open them again, and every Hello from
// anywhere was refused Busy for as long as it liked - no failure counted, no backoff ever. Two
// rules close that: a silent connection that held its slot past the grace is a failure (so the
// holder is backed off after MOBILEGL_IPC_AUTH_BACKOFF_AFTER of them), and a full queue is SHARED
// (ChoosePendingToDisplace): a newcomer from an address that holds fewer slots than the busiest
// address displaces the busiest address's oldest pending connection, so no one address can keep
// another out.

#pragma once

#include "../Protocol/generated/protocol_generated.h"
#include "../Transport/AuthToken.h"
#include "../Transport/Framing.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <sys/socket.h>
#endif

namespace MobileGL::MG_Remote::Server {

    // A first frame is a Hello or a DataBind, both a few hundred bytes. The old per-child read
    // allowed 1 MiB and so does this; the point of the bound is that it is checked against the
    // HEADER, before anything is allocated for the payload.
    inline constexpr std::uint64_t kPreAuthFirstFrameMaxBytes = 1024 * 1024;
    // The backoff window never grows past this (unless the base itself is larger).
    inline constexpr std::uint32_t kAuthBackoffCapMs = 60 * 1000;
    // Addresses tracked at once. A peer rotating through addresses gets no more than the bound on
    // pending connections gives everybody; the table only has to not grow without limit.
    inline constexpr std::size_t kAuthBackoffTrackedPeers = 256;
    // A failure this old no longer counts: the operator who mistyped a token yesterday starts
    // from zero today.
    inline constexpr std::uint32_t kAuthBackoffForgetMs = 10 * 60 * 1000;
    // A connection that closes (or is displaced) without a byte this soon after its accept is a
    // port probe; one that held its slot longer is a failure. A real client writes its first frame
    // the moment it connects, so it is never on the wrong side of this.
    inline constexpr std::uint32_t kPreAuthProbeGraceMs = 250;
    // MOBILEGL_IPC_PREAUTH_MAX never exceeds this. Every pending connection is a descriptor the
    // supervisor holds, and an unbounded knob would let a connection flood walk it into
    // RLIMIT_NOFILE (the supervisor also survives that now - it pauses accepting - but a knob
    // should not be the way in).
    inline constexpr std::uint32_t kPreAuthMaxPendingCeiling = 256;

    // The refusal details a peer (and the log) sees. Named once so the supervisor and the unit test
    // cannot drift on the words; the TCP lane's scripts mirror them rather than parse them, so a
    // rename here reds those controls instead of being followed silently.
    inline constexpr const char* kFirstFrameNotAFrame = "first frame is not a control frame";
    inline constexpr const char* kFirstFrameTruncated = "first frame ended before it was complete";
    inline constexpr const char* kFirstFrameNoIdentifier = "first frame carries no CtrlEnvelope identifier";
    inline constexpr const char* kFirstFrameUnverifiable = "first frame did not verify as a CtrlEnvelope";
    inline constexpr const char* kFirstFrameNotAHello = "first control frame is not a verifiable Hello";
    inline constexpr const char* kFirstFrameBadDataBind = "first frame is a DataBind without a 16-byte nonce";
    inline constexpr const char* kPreAuthDeadline = "no authenticated first frame within the pre-auth deadline";
    inline constexpr const char* kPreAuthFull = "too many connections are awaiting authentication";
    inline constexpr const char* kPreAuthDisplaced = "displaced from the pre-auth queue by a connection from another address";
    inline constexpr const char* kAuthBackoffRefusal = "too many failed authentications from this address; retry later";

    struct PreAuthKnobs {
        // MOBILEGL_IPC_PREAUTH_MS: how long a new connection has to present a complete first
        // frame. A real client writes its Hello (or DataBind) the moment it connects.
        std::uint32_t deadlineMs = 2000;
        // MOBILEGL_IPC_PREAUTH_MAX: connections the supervisor holds unauthenticated at once, at
        // most kPreAuthMaxPendingCeiling. When they are all taken a newcomer displaces the busiest
        // address's oldest one, or is refused Busy at accept (ChoosePendingToDisplace).
        std::uint32_t maxPending = 8;
        // MOBILEGL_IPC_AUTH_BACKOFF_AFTER: failures from one address before it is refused at
        // accept. 0 turns the backoff off (the fuzz arm does, to send many malformed frames from
        // one address and still be served afterwards).
        std::uint32_t backoffAfter = 5;
        // MOBILEGL_IPC_AUTH_BACKOFF_MS: the first window; each further failure doubles it, up to
        // kAuthBackoffCapMs.
        std::uint32_t backoffBaseMs = 1000;

        // An unset, empty, non-numeric or zero value keeps the default - except
        // MOBILEGL_IPC_AUTH_BACKOFF_AFTER, where 0 is the documented "off".
        static PreAuthKnobs FromEnvironment() {
            PreAuthKnobs knobs;
            knobs.deadlineMs = Read("MOBILEGL_IPC_PREAUTH_MS", knobs.deadlineMs, false);
            knobs.maxPending =
                std::min(Read("MOBILEGL_IPC_PREAUTH_MAX", knobs.maxPending, false), kPreAuthMaxPendingCeiling);
            knobs.backoffAfter = Read("MOBILEGL_IPC_AUTH_BACKOFF_AFTER", knobs.backoffAfter, true);
            knobs.backoffBaseMs = Read("MOBILEGL_IPC_AUTH_BACKOFF_MS", knobs.backoffBaseMs, false);
            return knobs;
        }

    private:
        static std::uint32_t Read(const char* name, std::uint32_t fallback, bool zeroAllowed) {
            const char* text = std::getenv(name);
            if (text == nullptr || *text == '\0') return fallback;
            char* end = nullptr;
            const unsigned long long value = std::strtoull(text, &end, 10);
            if (end == nullptr || *end != '\0' || value > 0xFFFFFFFFull) return fallback;
            if (value == 0 && !zeroAllowed) return fallback;
            return static_cast<std::uint32_t>(value);
        }
    };

    // What a complete first frame is. The identifier is asked FIRST (plan §1.1 fuzz arm 1 row):
    // the generated VerifyCtrlEnvelopeBuffer checks it too, but only as part of a general "did not
    // verify", and a peer speaking some other schema deserves the specific answer. msg_as_Hello()
    // and msg_as_DataBind() are part of the guard, not consequences of it: flatbuffers verifies a
    // NULL union member as valid, so a 24-byte envelope whose tag says Hello carries no Hello.
    enum class FirstFrameShape { NoIdentifier, Unverifiable, Hello, DataBind, BadDataBind, NotAHello };

    inline FirstFrameShape ClassifyFirstFrame(const std::uint8_t* payload, std::size_t size) {
        if (payload == nullptr || size < 8 || !::MobileGL::Wire::CtrlEnvelopeBufferHasIdentifier(payload))
            return FirstFrameShape::NoIdentifier;
        ::flatbuffers::Verifier verifier(payload, size);
        if (!::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier)) return FirstFrameShape::Unverifiable;
        const auto* envelope = ::MobileGL::Wire::GetCtrlEnvelope(payload);
        if (envelope->msg_type() == ::MobileGL::Wire::CtrlMsg::DataBind) {
            const auto* bind = envelope->msg_as_DataBind();
            return bind != nullptr && bind->nonce() != nullptr &&
                           bind->nonce()->size() == Transport::kDataNonceBytes
                       ? FirstFrameShape::DataBind
                       : FirstFrameShape::BadDataBind;
        }
        if (envelope->msg_type() == ::MobileGL::Wire::CtrlMsg::Hello && envelope->msg_as_Hello() != nullptr)
            return FirstFrameShape::Hello;
        return FirstFrameShape::NotAHello;
    }

    // The detail a malformed shape is refused with (Refuse{MalformedHello}); nullptr for the two
    // shapes that are not malformed.
    inline const char* FirstFrameRefusalDetail(FirstFrameShape shape) {
        switch (shape) {
            case FirstFrameShape::NoIdentifier: return kFirstFrameNoIdentifier;
            case FirstFrameShape::Unverifiable: return kFirstFrameUnverifiable;
            case FirstFrameShape::BadDataBind: return kFirstFrameBadDataBind;
            case FirstFrameShape::NotAHello: return kFirstFrameNotAHello;
            case FirstFrameShape::Hello:
            case FirstFrameShape::DataBind: return nullptr;
        }
        return kFirstFrameNotAHello;
    }

    // THE PENDING QUEUE IS SHARED BETWEEN ADDRESSES. Called when every pending slot is taken and a
    // connection from `newcomer` has just been accepted. `pending` is the addresses of the pending
    // connections, oldest first. The newcomer displaces the OLDEST pending connection of the
    // address that holds the most slots, unless its own address already holds as many as that -
    // then the newcomer is the one refused. Returns the index to displace, or pending.size() to
    // refuse the newcomer.
    //
    // So an address holding nothing is always let in, whoever fills the queue; an address can only
    // keep every slot while nobody else wants one; and among addresses with equal shares, the
    // connection that has waited longest - the one furthest from being a client that writes its
    // Hello on connect - goes first.
    inline std::size_t ChoosePendingToDisplace(const std::vector<std::string>& pending, const std::string& newcomer) {
        std::unordered_map<std::string, std::size_t> held;
        std::size_t most = 0;
        for (const auto& address : pending) most = std::max(most, ++held[address]);
        const auto own = held.find(newcomer);
        if (own != held.end() && own->second >= most) return pending.size();
        for (std::size_t index = 0; index < pending.size(); ++index)
            if (held[pending[index]] == most) return index;
        return pending.size();
    }

    // Failures per peer address and the window each address is refused for.
    class AuthBackoff {
    public:
        using Clock = std::chrono::steady_clock;

        explicit AuthBackoff(const PreAuthKnobs& knobs) : m_after(knobs.backoffAfter), m_baseMs(knobs.backoffBaseMs) {}

        // 0 when `address` may try now; otherwise the milliseconds until it may.
        std::uint32_t RetryAfterMs(const std::string& address, Clock::time_point now) const {
            if (m_after == 0) return 0;
            const auto found = m_peers.find(address);
            if (found == m_peers.end() || now >= found->second.until) return 0;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(found->second.until - now).count();
            return static_cast<std::uint32_t>(std::max<std::int64_t>(1, left));
        }

        // Counts one failure from `address` and returns the window now in force for it (0 while it
        // is still under the threshold, and always 0 with the backoff off).
        std::uint32_t NoteFailure(const std::string& address, Clock::time_point now) {
            auto found = m_peers.find(address);
            if (found == m_peers.end()) {
                if (m_peers.size() >= kAuthBackoffTrackedPeers) EvictOne(now);
                found = m_peers.emplace(address, Entry{}).first;
            }
            Entry& entry = found->second;
            if (entry.failures != 0 && now - entry.last > std::chrono::milliseconds(kAuthBackoffForgetMs))
                entry.failures = 0;
            if (entry.failures != 0xFFFFFFFFu) ++entry.failures;
            entry.last = now;
            if (m_after == 0 || entry.failures < m_after) return 0;
            const std::uint32_t window = WindowFor(entry.failures);
            entry.until = now + std::chrono::milliseconds(window);
            return window;
        }

        // An authenticated Hello from `address`: its count starts again from zero.
        void NoteSuccess(const std::string& address) { m_peers.erase(address); }

        std::uint32_t Failures(const std::string& address) const {
            const auto found = m_peers.find(address);
            return found == m_peers.end() ? 0u : found->second.failures;
        }
        std::size_t Tracked() const { return m_peers.size(); }

        // base << (failures - after), capped. Public so the unit test states the arithmetic.
        std::uint32_t WindowFor(std::uint32_t failures) const {
            const std::uint32_t cap = std::max(kAuthBackoffCapMs, m_baseMs);
            const std::uint32_t shift = failures <= m_after ? 0u : std::min<std::uint32_t>(failures - m_after, 31u);
            const std::uint64_t window = static_cast<std::uint64_t>(m_baseMs) << shift;
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(window, cap));
        }

    private:
        struct Entry {
            std::uint32_t failures = 0;
            Clock::time_point last{};
            Clock::time_point until{};
        };

        // Drops the entry whose last failure is oldest; one whose window has run out goes first.
        void EvictOne(Clock::time_point now) {
            auto victim = m_peers.end();
            for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
                const bool expired = it->second.until <= now;
                if (victim == m_peers.end()) { victim = it; continue; }
                const bool victimExpired = victim->second.until <= now;
                if (expired != victimExpired) {
                    if (expired) victim = it;
                    continue;
                }
                if (it->second.last < victim->second.last) victim = it;
            }
            if (victim != m_peers.end()) m_peers.erase(victim);
        }

        std::uint32_t m_after;
        std::uint32_t m_baseMs;
        std::unordered_map<std::string, Entry> m_peers;
    };

#if !defined(_WIN32)
    // Reads exactly one [u32 'MGLF'][u32 length][payload] frame off a NON-BLOCKING read of `fd`,
    // across as many calls as the peer takes, and never a byte beyond it.
    class FirstFrameAssembler {
    public:
        enum class Step {
            NeedMore,     // nothing wrong; the frame is not complete yet
            Complete,     // Payload() is the whole first frame
            NotAFrame,    // wrong magic, or a length over kPreAuthFirstFrameMaxBytes
            EndedEmpty,   // the peer closed without sending a byte
            EndedPartial, // the peer closed (or the read failed) inside the frame
        };

        Step Pump(int fd) {
            for (;;) {
                if (m_have < Transport::kFrameHeaderSize) {
                    const auto step = ReadInto(fd, m_header + m_have, Transport::kFrameHeaderSize - m_have);
                    if (step != Step::Complete) return step;
                    if (m_have < Transport::kFrameHeaderSize) continue;
                    std::uint32_t magic = 0, length = 0;
                    std::memcpy(&magic, m_header, sizeof(magic));
                    std::memcpy(&length, m_header + 4, sizeof(length));
                    if (magic != Transport::kFrameMagic || length > kPreAuthFirstFrameMaxBytes) return Step::NotAFrame;
                    m_payload.resize(length);
                    if (length == 0) return Step::Complete;
                    continue;
                }
                const std::size_t done = static_cast<std::size_t>(m_have - Transport::kFrameHeaderSize);
                if (done == m_payload.size()) return Step::Complete;
                const auto step = ReadInto(fd, m_payload.data() + done, m_payload.size() - done);
                if (step != Step::Complete) return step;
            }
        }

        const std::vector<std::uint8_t>& Payload() const { return m_payload; }
        std::uint64_t BytesSeen() const { return m_have; }

    private:
        // Complete = some bytes were read (the caller loops to see whether that finished anything).
        Step ReadInto(int fd, std::uint8_t* into, std::size_t size) {
            for (;;) {
                const ssize_t n = ::recv(fd, into, size, MSG_DONTWAIT);
                if (n > 0) {
                    m_have += static_cast<std::uint64_t>(n);
                    return Step::Complete;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Step::NeedMore;
                return m_have == 0 ? Step::EndedEmpty : Step::EndedPartial;
            }
        }

        std::uint8_t m_header[Transport::kFrameHeaderSize] = {};
        std::uint64_t m_have = 0;
        std::vector<std::uint8_t> m_payload;
    };
#endif

} // namespace MobileGL::MG_Remote::Server
