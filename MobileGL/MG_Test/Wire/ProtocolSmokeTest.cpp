// MobileGL - MobileGL/MG_Test/Wire/ProtocolSmokeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The committed control-plane schema: encode/decode a handshake through the
// generated header, and pin the union tag values, which are wire numbers that
// may only ever be appended to.

#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Transport/Framing.h>
#include <MG_Remote/Transport/InProcessTransport.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace MobileGL::Wire;
namespace Transport = MobileGL::MG_Remote::Transport;

namespace {

    std::vector<std::uint8_t> BuildHello() {
        ::flatbuffers::FlatBufferBuilder builder(1024);
        const std::vector<std::uint8_t> config{1, 2, 3, 4};
        auto hello = CreateHelloDirect(builder, MOBILEGL_PROTOCOL_ABI_MAJOR,
                                       MOBILEGL_PROTOCOL_ABI_MINOR, "mobilegl-test-build",
                                       /*backendType=*/2, /*pid=*/4242, &config);
        auto envelope = CreateCtrlEnvelope(builder, CtrlMsg::Hello, hello.Union());
        FinishCtrlEnvelopeBuffer(builder, envelope);
        const std::uint8_t* begin = builder.GetBufferPointer();
        return std::vector<std::uint8_t>(begin, begin + builder.GetSize());
    }

} // namespace

TEST(ProtocolSmokeTest, HelloRoundTrips) {
    const std::vector<std::uint8_t> buffer = BuildHello();

    // Every message from the peer is verified before a single field is read:
    // the control plane is parsed from another process's memory.
    ::flatbuffers::Verifier verifier(buffer.data(), buffer.size());
    ASSERT_TRUE(VerifyCtrlEnvelopeBuffer(verifier));
    ASSERT_TRUE(CtrlEnvelopeBufferHasIdentifier(buffer.data()));

    const CtrlEnvelope* envelope = GetCtrlEnvelope(buffer.data());
    ASSERT_NE(envelope, nullptr);
    ASSERT_EQ(envelope->msg_type(), CtrlMsg::Hello);

    const Hello* hello = envelope->msg_as_Hello();
    ASSERT_NE(hello, nullptr);
    EXPECT_EQ(hello->abiMajor(), static_cast<std::uint32_t>(MOBILEGL_PROTOCOL_ABI_MAJOR));
    EXPECT_EQ(hello->abiMinor(), static_cast<std::uint32_t>(MOBILEGL_PROTOCOL_ABI_MINOR));
    ASSERT_NE(hello->buildFingerprint(), nullptr);
    EXPECT_EQ(hello->buildFingerprint()->str(), "mobilegl-test-build");
    EXPECT_EQ(hello->backendType(), 2u);
    EXPECT_EQ(hello->pid(), 4242u);
    ASSERT_NE(hello->configBlob(), nullptr);
    ASSERT_EQ(hello->configBlob()->size(), 4u);
    EXPECT_EQ(hello->configBlob()->Get(3), 4u);

    // A message of the wrong kind reads back as null rather than as garbage.
    EXPECT_EQ(envelope->msg_as_Welcome(), nullptr);
}

TEST(ProtocolSmokeTest, WelcomeCarriesTheFourSegmentAnnouncements) {
    ::flatbuffers::FlatBufferBuilder builder(1024);
    auto cmd = CreateSegmentRefDirect(builder, 1, SegmentKind::Cmd, 8ull * 1024 * 1024, "cmd");
    auto stage = CreateSegmentRefDirect(builder, 2, SegmentKind::Stage, 32ull * 1024 * 1024, "stage");
    auto reply = CreateSegmentRefDirect(builder, 3, SegmentKind::Reply, 8ull * 1024 * 1024, "reply");
    auto event = CreateSegmentRefDirect(builder, 4, SegmentKind::Event, 256ull * 1024, "event");
    auto welcome = CreateWelcome(builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                                 /*serverPid=*/99, cmd, stage, reply, event);
    auto envelope = CreateCtrlEnvelope(builder, CtrlMsg::Welcome, welcome.Union());
    FinishCtrlEnvelopeBuffer(builder, envelope);

    ::flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
    ASSERT_TRUE(VerifyCtrlEnvelopeBuffer(verifier));

    const Welcome* parsed = GetCtrlEnvelope(builder.GetBufferPointer())->msg_as_Welcome();
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->serverPid(), 99u);
    ASSERT_NE(parsed->cmdRing(), nullptr);
    EXPECT_EQ(parsed->cmdRing()->kind(), SegmentKind::Cmd);
    EXPECT_EQ(parsed->cmdRing()->sizeBytes(), 8ull * 1024 * 1024);
    ASSERT_NE(parsed->stageRing(), nullptr);
    EXPECT_EQ(parsed->stageRing()->sizeBytes(), 32ull * 1024 * 1024);
    ASSERT_NE(parsed->eventRing(), nullptr);
    EXPECT_EQ(parsed->eventRing()->sizeBytes(), 256ull * 1024);
}

TEST(ProtocolSmokeTest, UnionTagsAreFrozenWireValues) {
    // Appending to CtrlMsg is a compatible change; reordering it is not. If
    // this test has to be edited, the schema change was a wire break.
    EXPECT_EQ(static_cast<int>(CtrlMsg::NONE), 0);
    EXPECT_EQ(static_cast<int>(CtrlMsg::Hello), 1);
    EXPECT_EQ(static_cast<int>(CtrlMsg::Welcome), 2);
    EXPECT_EQ(static_cast<int>(CtrlMsg::CapsSnapshot), 3);
    EXPECT_EQ(static_cast<int>(CtrlMsg::SurfaceOp), 4);
    EXPECT_EQ(static_cast<int>(CtrlMsg::SurfaceReply), 5);
    EXPECT_EQ(static_cast<int>(CtrlMsg::ResyncRequest), 6);
    EXPECT_EQ(static_cast<int>(CtrlMsg::ResyncDone), 7);
    EXPECT_EQ(static_cast<int>(CtrlMsg::AuxRequest), 8);
    EXPECT_EQ(static_cast<int>(CtrlMsg::Fatal), 9);
    EXPECT_EQ(static_cast<int>(CtrlMsg::LogLine), 10);

    EXPECT_EQ(static_cast<int>(SegmentKind::Cmd), 1);
    EXPECT_EQ(static_cast<int>(SegmentKind::Adopt), 6);
    EXPECT_EQ(static_cast<int>(LogLevel::Error), 3);
    EXPECT_EQ(static_cast<int>(FatalCode::ProtocolCorruption), 1);
}

TEST(ProtocolSmokeTest, TruncatedMessageFailsVerificationInsteadOfReadingGarbage) {
    std::vector<std::uint8_t> buffer = BuildHello();
    ASSERT_GT(buffer.size(), 8u);
    buffer.resize(buffer.size() / 2);

    ::flatbuffers::Verifier verifier(buffer.data(), buffer.size());
    EXPECT_FALSE(VerifyCtrlEnvelopeBuffer(verifier));
}

TEST(ProtocolSmokeTest, TravelsAcrossTheTransportUnchanged) {
    std::unique_ptr<Transport::InProcessTransport> client;
    std::unique_ptr<Transport::InProcessTransport> server;
    Transport::InProcessTransport::CreatePair(client, server);

    const std::vector<std::uint8_t> sent = BuildHello();
    ASSERT_EQ(client->SendFrame(MobileGLByteSpan{sent.data(), sent.size()}), MOBILEGL_OK);

    const std::uint64_t pending = server->PeekFrameSize();
    ASSERT_EQ(pending, sent.size());
    std::vector<std::uint8_t> received(pending);
    std::uint64_t size = 0;
    MobileGLMutableByteSpan span{received.data(), received.size()};
    ASSERT_EQ(server->ReceiveFrame(span, &size, 1000), MOBILEGL_OK);
    ASSERT_EQ(size, sent.size());

    ::flatbuffers::Verifier verifier(received.data(), received.size());
    ASSERT_TRUE(VerifyCtrlEnvelopeBuffer(verifier));
    const Hello* hello = GetCtrlEnvelope(received.data())->msg_as_Hello();
    ASSERT_NE(hello, nullptr);
    EXPECT_EQ(hello->pid(), 4242u);
}
