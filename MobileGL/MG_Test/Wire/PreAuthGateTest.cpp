// MobileGL - MobileGL/MG_Test/Wire/PreAuthGateTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// PH-7 (5) and fuzz arm 1: the TCP supervisor's pre-auth gate, piece by piece.
//
// These are the cheap halves. The end-to-end halves - a live supervisor that must answer every
// malformed first frame by name, fork for nothing it has not authenticated, refuse a silent peer
// at its deadline, cap its pending connections and back an address off - are
// TcpLane.SupervisorProtocolControls (scripts/ci/tcp_supervisor_smoke.py). What is pinned here
// is what
// those scripts cannot see from outside: that each malformation lands on the shape it is named
// for (and not merely on SOME refusal), that the assembler never reads a byte past the first
// frame, and the backoff's arithmetic.

#include <MG_Remote/Handshake.h>
#include <MG_Remote/Server/PreAuthGate.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

using namespace MobileGL::MG_Remote;
using Server::FirstFrameShape;
namespace Wire = ::MobileGL::Wire;

namespace {

    std::vector<std::uint8_t> Finish(::flatbuffers::FlatBufferBuilder& builder, ::flatbuffers::Offset<Wire::CtrlEnvelope> root,
                                     const char* identifier = Wire::CtrlEnvelopeIdentifier()) {
        builder.Finish(root, identifier);
        return std::vector<std::uint8_t>(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    }

    std::vector<std::uint8_t> HelloPayload(const char* identifier = Wire::CtrlEnvelopeIdentifier()) {
        ::flatbuffers::FlatBufferBuilder builder(512);
        auto terms = Wire::CreateLinkTerms(builder);
        auto hello = Wire::CreateHelloDirect(builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                                             "build", 0, 1, nullptr, 0, 0, terms, "token", Wire::DialMode::Connect);
        return Finish(builder, Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::Hello, hello.Union()), identifier);
    }

    std::vector<std::uint8_t> DataBindPayload(std::size_t nonceBytes) {
        ::flatbuffers::FlatBufferBuilder builder(64);
        const std::vector<std::uint8_t> nonce(nonceBytes, 0x5A);
        auto bind = Wire::CreateDataBind(builder, builder.CreateVector(nonce));
        return Finish(builder, Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::DataBind, bind.Union()));
    }

    std::vector<std::uint8_t> Framed(const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> out;
        EXPECT_EQ(Transport::AppendFrame(out, payload.data(), payload.size()), MOBILEGL_OK);
        return out;
    }

    struct SocketPair {
        int fds[2] = {-1, -1};
        SocketPair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0); }
        ~SocketPair() {
            for (int fd : fds)
                if (fd >= 0) ::close(fd);
        }
        void Send(const void* bytes, std::size_t size) {
            ASSERT_EQ(::send(fds[0], bytes, size, MSG_NOSIGNAL), static_cast<ssize_t>(size));
        }
        void CloseWriter() {
            ::close(fds[0]);
            fds[0] = -1;
        }
    };

    struct ScopedEnvironment {
        std::string name, previous;
        bool had = false;
        ScopedEnvironment(const char* key, const char* value) : name(key) {
            if (const char* old = std::getenv(key)) { previous = old; had = true; }
            if (value) ::setenv(key, value, 1); else ::unsetenv(key);
        }
        ~ScopedEnvironment() {
            if (had) ::setenv(name.c_str(), previous.c_str(), 1); else ::unsetenv(name.c_str());
        }
    };

} // namespace

// Fuzz arm 1's five first-frame malformations, each on its OWN shape. Red once by deleting the
// CtrlEnvelopeBufferHasIdentifier line from ClassifyFirstFrame: the wrong-identifier Hello then
// classifies Unverifiable (VerifyCtrlEnvelopeBuffer asks the identifier too, but only as part of
// "did not verify"), and the first EXPECT_EQ names it.
TEST(PreAuthGateTest, EachMalformedFirstFrameLandsOnTheShapeItIsNamedFor) {
    const auto wrongIdentifier = HelloPayload("XXXX");
    EXPECT_EQ(Server::ClassifyFirstFrame(wrongIdentifier.data(), wrongIdentifier.size()), FirstFrameShape::NoIdentifier);
    // Shorter than a root offset plus an identifier: nothing to ask.
    const std::uint8_t tiny[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(Server::ClassifyFirstFrame(tiny, sizeof(tiny)), FirstFrameShape::NoIdentifier);
    EXPECT_EQ(Server::ClassifyFirstFrame(nullptr, 0), FirstFrameShape::NoIdentifier);
    // Our identifier, and a root offset that points nowhere: identifier yes, verifier no.
    std::vector<std::uint8_t> garbage(64, 0xEE);
    const std::uint32_t root = 0xFFFFFF00u;
    std::memcpy(garbage.data(), &root, sizeof(root));
    std::memcpy(garbage.data() + 4, Wire::CtrlEnvelopeIdentifier(), 4);
    EXPECT_EQ(Server::ClassifyFirstFrame(garbage.data(), garbage.size()), FirstFrameShape::Unverifiable);
    // A valid envelope of the wrong union type, and the 24-byte envelope whose tag says Hello but
    // whose table is NULL (flatbuffers verifies that as valid).
    ::flatbuffers::FlatBufferBuilder builder(64);
    auto flush = Wire::CreateLogFlush(builder, 1, false);
    const auto logFlush = Finish(builder, Wire::CreateCtrlEnvelope(builder, Wire::CtrlMsg::LogFlush, flush.Union()));
    EXPECT_EQ(Server::ClassifyFirstFrame(logFlush.data(), logFlush.size()), FirstFrameShape::NotAHello);
    ::flatbuffers::FlatBufferBuilder empty(64);
    const auto nullHello = Finish(empty, Wire::CreateCtrlEnvelope(empty, Wire::CtrlMsg::Hello, ::flatbuffers::Offset<void>()));
    EXPECT_EQ(Server::ClassifyFirstFrame(nullHello.data(), nullHello.size()), FirstFrameShape::NotAHello);
    // A DataBind is only a DataBind with exactly kDataNonceBytes of nonce.
    const auto shortBind = DataBindPayload(8);
    EXPECT_EQ(Server::ClassifyFirstFrame(shortBind.data(), shortBind.size()), FirstFrameShape::BadDataBind);
    // And the two well-formed shapes.
    const auto hello = HelloPayload();
    EXPECT_EQ(Server::ClassifyFirstFrame(hello.data(), hello.size()), FirstFrameShape::Hello);
    const auto bind = DataBindPayload(Transport::kDataNonceBytes);
    EXPECT_EQ(Server::ClassifyFirstFrame(bind.data(), bind.size()), FirstFrameShape::DataBind);
    // Every malformed shape has words; the two well-formed ones have none.
    for (auto shape : {FirstFrameShape::NoIdentifier, FirstFrameShape::Unverifiable, FirstFrameShape::BadDataBind,
                       FirstFrameShape::NotAHello})
        EXPECT_NE(Server::FirstFrameRefusalDetail(shape), nullptr);
    EXPECT_EQ(Server::FirstFrameRefusalDetail(FirstFrameShape::Hello), nullptr);
    EXPECT_EQ(Server::FirstFrameRefusalDetail(FirstFrameShape::DataBind), nullptr);
    EXPECT_STREQ(Server::FirstFrameRefusalDetail(FirstFrameShape::NoIdentifier), Server::kFirstFrameNoIdentifier);
}

// The assembler reads EXACTLY the first frame, however the peer splits it, and leaves what follows
// on the socket - a DataBind's connection carries StreamLink's frames straight after it, and they
// belong to the session child, not to the supervisor.
TEST(PreAuthGateTest, TheAssemblerStopsAtTheFirstFrameWhateverTheFragmentation) {
    SocketPair pair;
    const auto frame = Framed(HelloPayload());
    Server::FirstFrameAssembler assembler;
    EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::NeedMore);
    pair.Send(frame.data(), 5); // inside the header
    EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::NeedMore);
    pair.Send(frame.data() + 5, 10); // header done, payload begun
    EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::NeedMore);
    const char trailer[] = "stream-link-bytes";
    std::vector<std::uint8_t> rest(frame.begin() + 15, frame.end());
    rest.insert(rest.end(), trailer, trailer + sizeof(trailer));
    pair.Send(rest.data(), rest.size());
    ASSERT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::Complete);
    EXPECT_EQ(assembler.Payload(), HelloPayload());
    EXPECT_EQ(assembler.BytesSeen(), frame.size());
    char left[sizeof(trailer)] = {};
    ASSERT_EQ(::recv(pair.fds[1], left, sizeof(left), MSG_DONTWAIT), static_cast<ssize_t>(sizeof(trailer)));
    EXPECT_STREQ(left, trailer);
}

// A header that is not ours is refused on the header, before the payload is allocated for.
TEST(PreAuthGateTest, TheAssemblerRefusesAForeignMagicAndAnOversizeLengthOnTheHeader) {
    {
        SocketPair pair;
        const std::uint8_t header[8] = {'X', 'X', 'X', 'X', 16, 0, 0, 0};
        pair.Send(header, sizeof(header));
        Server::FirstFrameAssembler assembler;
        EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::NotAFrame);
    }
    {
        SocketPair pair;
        std::uint8_t header[8] = {};
        std::memcpy(header, &Transport::kFrameMagic, 4);
        const std::uint32_t length = static_cast<std::uint32_t>(Server::kPreAuthFirstFrameMaxBytes + 1);
        std::memcpy(header + 4, &length, 4);
        pair.Send(header, sizeof(header));
        Server::FirstFrameAssembler assembler;
        EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::NotAFrame);
        EXPECT_TRUE(assembler.Payload().empty());
    }
}

// A close before any byte is a port probe; a close inside the frame is a truncated first frame.
TEST(PreAuthGateTest, TheAssemblerTellsAProbeFromATruncatedFrame) {
    {
        SocketPair pair;
        pair.CloseWriter();
        Server::FirstFrameAssembler assembler;
        EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::EndedEmpty);
    }
    {
        SocketPair pair;
        const auto frame = Framed(HelloPayload());
        pair.Send(frame.data(), 5);
        pair.CloseWriter();
        Server::FirstFrameAssembler assembler;
        EXPECT_EQ(assembler.Pump(pair.fds[1]), Server::FirstFrameAssembler::Step::EndedPartial);
    }
}

// The backoff's arithmetic: nothing below the threshold, then base, doubling, capped; an
// authenticated Hello clears the address; old failures are forgotten; 0 turns it off; the table
// is bounded. Red once by making NoteFailure ignore m_after (window from the first failure): the
// first EXPECT_EQ(..., 0u) names it.
TEST(PreAuthGateTest, TheBackoffWindowStartsAtTheThresholdDoublesAndIsCapped) {
    Server::PreAuthKnobs knobs;
    knobs.backoffAfter = 3;
    knobs.backoffBaseMs = 100;
    Server::AuthBackoff backoff(knobs);
    const auto t0 = Server::AuthBackoff::Clock::now();
    const std::string peer = "192.0.2.7";
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 0u);
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 0u);
    EXPECT_EQ(backoff.RetryAfterMs(peer, t0), 0u);
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 100u);
    EXPECT_GT(backoff.RetryAfterMs(peer, t0), 0u);
    EXPECT_LE(backoff.RetryAfterMs(peer, t0), 100u);
    EXPECT_EQ(backoff.RetryAfterMs(peer, t0 + std::chrono::milliseconds(100)), 0u);
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 200u);
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 400u);
    EXPECT_EQ(backoff.WindowFor(3 + 20), Server::kAuthBackoffCapMs);
    EXPECT_EQ(backoff.WindowFor(0xFFFFFFFFu), Server::kAuthBackoffCapMs);
    // Another address is not affected.
    EXPECT_EQ(backoff.RetryAfterMs("192.0.2.8", t0), 0u);
    // An authenticated Hello clears the address.
    backoff.NoteSuccess(peer);
    EXPECT_EQ(backoff.RetryAfterMs(peer, t0), 0u);
    EXPECT_EQ(backoff.Failures(peer), 0u);
    // Failures older than the forget interval count from zero again.
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 0u);
    EXPECT_EQ(backoff.NoteFailure(peer, t0), 0u);
    const auto later = t0 + std::chrono::milliseconds(Server::kAuthBackoffForgetMs + 1);
    EXPECT_EQ(backoff.NoteFailure(peer, later), 0u);
    EXPECT_EQ(backoff.Failures(peer), 1u);
    // Off means off.
    knobs.backoffAfter = 0;
    Server::AuthBackoff off(knobs);
    for (int i = 0; i < 50; ++i) EXPECT_EQ(off.NoteFailure(peer, t0), 0u);
    EXPECT_EQ(off.RetryAfterMs(peer, t0), 0u);
    // Bounded, however many addresses fail.
    for (int i = 0; i < 1000; ++i) backoff.NoteFailure("198.51.100." + std::to_string(i), t0);
    EXPECT_LE(backoff.Tracked(), Server::kAuthBackoffTrackedPeers);
}

// The knobs: defaults, values, and the one knob where 0 means something.
TEST(PreAuthGateTest, TheKnobsReadTheirEnvironmentAndKeepTheirDefaultsOtherwise) {
    {
        ScopedEnvironment a("MOBILEGL_IPC_PREAUTH_MS", nullptr), b("MOBILEGL_IPC_PREAUTH_MAX", nullptr),
            c("MOBILEGL_IPC_AUTH_BACKOFF_AFTER", nullptr), d("MOBILEGL_IPC_AUTH_BACKOFF_MS", nullptr);
        const auto knobs = Server::PreAuthKnobs::FromEnvironment();
        EXPECT_EQ(knobs.deadlineMs, 2000u);
        EXPECT_EQ(knobs.maxPending, 8u);
        EXPECT_EQ(knobs.backoffAfter, 5u);
        EXPECT_EQ(knobs.backoffBaseMs, 1000u);
    }
    {
        ScopedEnvironment a("MOBILEGL_IPC_PREAUTH_MS", "500"), b("MOBILEGL_IPC_PREAUTH_MAX", "2"),
            c("MOBILEGL_IPC_AUTH_BACKOFF_AFTER", "0"), d("MOBILEGL_IPC_AUTH_BACKOFF_MS", "1500");
        const auto knobs = Server::PreAuthKnobs::FromEnvironment();
        EXPECT_EQ(knobs.deadlineMs, 500u);
        EXPECT_EQ(knobs.maxPending, 2u);
        EXPECT_EQ(knobs.backoffAfter, 0u);
        EXPECT_EQ(knobs.backoffBaseMs, 1500u);
    }
    {
        ScopedEnvironment a("MOBILEGL_IPC_PREAUTH_MS", "0"), b("MOBILEGL_IPC_PREAUTH_MAX", "two"),
            c("MOBILEGL_IPC_AUTH_BACKOFF_AFTER", "-1"), d("MOBILEGL_IPC_AUTH_BACKOFF_MS", "99999999999");
        const auto knobs = Server::PreAuthKnobs::FromEnvironment();
        EXPECT_EQ(knobs.deadlineMs, 2000u);
        EXPECT_EQ(knobs.maxPending, 8u);
        EXPECT_EQ(knobs.backoffAfter, 5u);
        EXPECT_EQ(knobs.backoffBaseMs, 1000u);
    }
}
