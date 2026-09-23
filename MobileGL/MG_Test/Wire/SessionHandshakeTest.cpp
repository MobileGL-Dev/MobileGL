// MobileGL - MobileGL/MG_Test/Wire/SessionHandshakeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The two handshakes, driven for real (P5 s1, wave 1.5 - ID-46 findings 6 and 7).
//
// WHY THIS SUITE EXISTS BESIDE SessionTest. SessionTest is the ring-owning suite and keeps the
// GL frontend's umbrella header out on purpose. Both of the wave-1 review's findings against it
// were the same defect seen twice: a case that observed a property of the thing it built itself
// - a null-union frame it never sent anywhere, a mixer production never called - and so could
// not go red when the production code it was named for was deleted. The cure for both is to
// start from the production entry point, and the production entry points (ServerSession::Accept,
// ClientSession::StartOverTransportPair, CapsAbiFingerprint) all reach Includes.h. So they are
// exercised here, in a target that carries the include paths and links gtest rather than
// gtest_main: each guard's refusal is asserted BY MESSAGE, and with the console sink compiled
// out (Defines.h) an MGLOG line reaches exactly one place, the log file this process names
// before anything logs - PipeWireCodecTest's main() shape.
//
// EVERY CASE BELOW CARRIES ITS RED-ONCE LINE, and each of those perturbations was run.

#include <gtest/gtest.h>
#include <MG_Util/Debug/Log.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Includes.h"

#include <MG_Remote/CapsCodec.h>
#include <MG_Remote/Handshake.h>
#include <MG_Pipe/PipeWireLayout.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerSession.h>
#include <MG_Remote/Transport/InProcessTransport.h>
#include <MG_Remote/Transport/SessionRings.h>

#if __has_include(<MGGitHash.h>)
#include <MGGitHash.h>
#define MGL_HANDSHAKE_TEST_HAS_GIT_HASH 1
#else
#define MGL_HANDSHAKE_TEST_HAS_GIT_HASH 0
#endif

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace Transport = MobileGL::MG_Remote::Transport;

namespace {

    std::string g_logPath;

    std::string ReadLog() {
        // BOTH ROLES' LOGS (P6). A death test asserts that the CHILD said something; which
        // role's thread said it is not what these cases are about, and refusals raised on the
        // apply thread are written under the SERVER role by construction.
        return MobileGL::MG_Util::Debug::ReadRoleLogs(g_logPath.c_str());
    }

    long ProcessId() {
#if defined(_WIN32)
        return static_cast<long>(::_getpid());
#else
        return static_cast<long>(::getpid());
#endif
    }

    bool Contains(const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    }

    // The reviewer's 24-byte shape (SessionTest.ANullUnionFrameVerifiesWhichIsThePremiseOf-
    // BothHandshakeGuards proves it verifies): the envelope's tag says `tag` and its union
    // member is NULL, because FlatBuffers' Verifier::VerifyTable is `return !table ||
    // table->Verify(*this)`.
    std::vector<Uint8> BuildNullUnionFrame(::MobileGL::Wire::CtrlMsg tag) {
        ::flatbuffers::FlatBufferBuilder builder(256);
        auto envelope =
            ::MobileGL::Wire::CreateCtrlEnvelope(builder, tag, ::flatbuffers::Offset<void>());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        const Uint8* begin = builder.GetBufferPointer();
        return std::vector<Uint8>(begin, begin + builder.GetSize());
    }

} // namespace

// ---------------------------------------------------------------------------
// The ABI fingerprint, from the production entry point (ID-46 finding 6)
// ---------------------------------------------------------------------------

// The value Hello and Welcome carry is CapsAbiFingerprint(). This case starts THERE, requires it
// to be the mixer over its own published inputs, pins those inputs to the real sizeofs and
// constants (ID-33's list: the three struct sizes, kOpCount, the protocol ABI version, the git
// stamp - plus the caps blob's extents and the two codec versions), and then perturbs every
// input by one through the same mixer and requires the answer to move. RED ONCE by replacing
// CapsAbiFingerprint()'s body with `return 1;` - the verifier's exact perturbation, which left
// the whole unit lane green before this case existed - and it fails on the first EXPECT_EQ
// below. Also red, separately, by deleting any one `mix(...)` line from MixAbiFingerprint: the
// matching EXPECT_NE names the input that stopped being mixed. Both perturbations were run.
TEST(SessionHandshakeTest, TheAbiFingerprintChangesWhenAnyOfItsInputsDoes) {
    const Uint64 production = WireFingerprint();
    const auto inputs = CapsAbiFingerprintInputs();
    EXPECT_NE(production, 0u);
    EXPECT_EQ(production, CapsAbiFingerprint());
    EXPECT_EQ(production, Transport::MixAbiFingerprint(inputs));
    EXPECT_EQ(inputs.DynamicParamsSize, sizeof(MG_Backend::DynamicBackendParameters));
    EXPECT_EQ(inputs.CapsSize, sizeof(MG_Pipe::MGPCaps));
    EXPECT_EQ(inputs.MemberLayout, MG_Pipe::kMGPipeWireMemberLayoutDigest);
    EXPECT_EQ(inputs.CatalogueLayout, MG_Pipe::kMGPipeWireCatalogueDigest);
    EXPECT_EQ(inputs.RenderStateLayout, MG_Pipe::WireRenderStateDigest());
    EXPECT_EQ(inputs.ProgramArtifactsCodecVersion, MG_State::GLState::kProgramArtifactsCodecVersion);
    EXPECT_EQ(inputs.ProgramArtifactsSchema, MG_State::GLState::ProgramArtifactsSchemaFingerprint());
    EXPECT_EQ(inputs.OpCount, static_cast<Uint64>(MG_Pipe::MGPWireOp::kOpCount));
    EXPECT_EQ(inputs.ControlSchemaRevision, static_cast<Uint64>(MOBILEGL_PROTOCOL_CONTROL_REVISION));
    EXPECT_EQ(inputs.PointerBits, sizeof(void*) * 8);
    const auto perturbed = [&](auto mutate) {
        auto copy = inputs;
        mutate(copy);
        return Transport::MixAbiFingerprint(copy);
    };
    EXPECT_NE(production, perturbed([](auto& i) { ++i.DynamicParamsSize; })) << "DynamicParamsSize";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.CapsSize; })) << "CapsSize";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.MemberLayout; })) << "MemberLayout";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.CatalogueLayout; })) << "CatalogueLayout";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.RenderStateLayout; })) << "RenderStateLayout";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.FormatCapabilityTargets; })) << "FormatCapabilityTargets";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.FormatCapabilityFormats; })) << "FormatCapabilityFormats";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.FormatCapabilitiesCodecVersion; })) << "FormatCapabilitiesCodecVersion";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.RendererInfoCodecVersion; })) << "RendererInfoCodecVersion";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.ProgramArtifactsCodecVersion; })) << "ProgramArtifactsCodecVersion";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.ProgramArtifactsSchema; })) << "ProgramArtifactsSchema";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.OpCount; })) << "OpCount";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.ControlSchemaRevision; })) << "ControlSchemaRevision";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.AbiVersion; })) << "AbiVersion";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.PointerBits; })) << "PointerBits";
    EXPECT_NE(production, perturbed([](auto& i) { ++i.LittleEndian; })) << "LittleEndian";
}

TEST(SessionHandshakeTest, SameWidthFieldReorderingChangesTheWireDigestWithoutABuildStamp) {
    using namespace MG_Pipe;
    std::vector<WireLayoutMember> fields(std::begin(kMGPipeWireLayoutMembers),
                                       std::end(kMGPipeWireLayoutMembers));
    // MGPRange.Offset and Size have the same width: sizeof-only checks cannot see this.
    SizeT offsetIndex = 0, sizeIndex = 0;
    for (SizeT i = 0; i < fields.size(); ++i) {
        if (std::strcmp(fields[i].Name, "MGPRange.Offset") == 0) offsetIndex = i;
        if (std::strcmp(fields[i].Name, "MGPRange.Size") == 0) sizeIndex = i;
    }
    ASSERT_NE(offsetIndex, sizeIndex);
    ASSERT_EQ(fields[offsetIndex].Size, fields[sizeIndex].Size);
    std::swap(fields[offsetIndex].Offset, fields[sizeIndex].Offset);
    EXPECT_NE(WireMemberLayoutDigest(fields.data(), fields.size()), kMGPipeWireMemberLayoutDigest);
    const auto before = WireFingerprint();
    EXPECT_STREQ(BuildFingerprint(), MOBILEGL_BUILD_STAMP_VALUE);
    EXPECT_EQ(BuildFingerprintPresent(), MOBILEGL_BUILD_STAMP_PRESENT != 0);
    EXPECT_EQ(before, WireFingerprint());
}

namespace {
    class ScopedHandshakeEnvironment {
    public:
        ScopedHandshakeEnvironment(const char* name, const char* value) : m_name(name) {
            const char* previous = std::getenv(name);
            m_present = previous != nullptr;
            if (m_present) m_previous = previous;
            Set(value);
        }
        ~ScopedHandshakeEnvironment() { Set(m_present ? m_previous.c_str() : nullptr); }
    private:
        void Set(const char* value) {
#if defined(_WIN32)
            ::_putenv_s(m_name, value == nullptr ? "" : value);
#else
            if (value == nullptr) ::unsetenv(m_name); else ::setenv(m_name, value, 1);
#endif
        }
        const char* m_name;
        bool m_present = false;
        std::string m_previous;
    };

    std::vector<Uint8> ReadHandshakeFrame(Transport::ITransport& transport) {
        Uint64 bytes = 0;
        if (transport.ReceiveFrame({nullptr, 0}, &bytes, 1000) != MOBILEGL_ERR_BUFFER_TOO_SMALL) return {};
        std::vector<Uint8> frame(bytes);
        if (transport.ReceiveFrame({frame.data(), frame.size()}, &bytes, 1000) != MOBILEGL_OK) return {};
        return frame;
    }

    void CheckHandshake(Uint32 major, Uint64 wire, const char* build,
                        ::MobileGL::Wire::DialMode dial, ::MobileGL::Wire::RefuseCode expected) {
        using namespace ::MobileGL::Wire;
        std::unique_ptr<Transport::InProcessTransport> client, server;
        Transport::InProcessTransport::CreatePair(client, server);
        ::flatbuffers::FlatBufferBuilder builder(512);
        auto terms = CreateLinkTerms(builder);
        auto hello = CreateHelloDirect(builder, major, MOBILEGL_PROTOCOL_ABI_MINOR, build,
                                       0, 1, nullptr, wire, wire, terms, nullptr, dial);
        auto root = CreateCtrlEnvelope(builder, CtrlMsg::Hello, hello.Union());
        FinishCtrlEnvelopeBuffer(builder, root);
        std::vector<Uint8> first(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
        Server::ServerSession session;
        Transport::SessionSegmentSizes sizes;
        sizes.CmdRingBytes = 4096; sizes.StageBytes = 4096;
        sizes.ReplyBytes = 4096; sizes.EventRingBytes = 4096;
        session.SetSegmentSizes(sizes);
        const auto result = session.Accept(*server, &first);
        EXPECT_EQ(result, expected == RefuseCode::None ? MOBILEGL_OK : MOBILEGL_ERR_PROTOCOL_MISMATCH);
        const auto reply = ReadHandshakeFrame(*client);
        ASSERT_FALSE(reply.empty());
        ::flatbuffers::Verifier verifier(reply.data(), reply.size());
        ASSERT_TRUE(VerifyCtrlEnvelopeBuffer(verifier));
        const auto* envelope = GetCtrlEnvelope(reply.data());
        if (expected == RefuseCode::None) {
            ASSERT_NE(envelope->msg_as_Welcome(), nullptr);
            EXPECT_EQ(envelope->msg_as_Welcome()->wireFingerprint(), WireFingerprint());
            ASSERT_NE(envelope->msg_as_Welcome()->linkTerms(), nullptr);
            EXPECT_EQ(envelope->msg_as_Welcome()->linkTerms()->cmdWindowBytes(), 4096u);
        } else {
            ASSERT_NE(envelope->msg_as_Refuse(), nullptr);
            EXPECT_EQ(envelope->msg_as_Refuse()->code(), expected);
            EXPECT_FALSE(session.Accepted());
        }
        session.Close();
    }
}

TEST(SessionHandshakeTest, WireMajorMismatchReturnsNamedRefuseWithoutAborting) {
    CheckHandshake(99, WireFingerprint(), BuildFingerprint(), ::MobileGL::Wire::DialMode::No,
                   ::MobileGL::Wire::RefuseCode::ProtocolVersion);
}
TEST(SessionHandshakeTest, WireLayoutMismatchReturnsNamedRefuseWithoutAborting) {
    CheckHandshake(MOBILEGL_PROTOCOL_ABI_MAJOR, WireFingerprint() ^ 1, BuildFingerprint(),
                   ::MobileGL::Wire::DialMode::No, ::MobileGL::Wire::RefuseCode::WireFingerprint);
}
TEST(SessionHandshakeTest, ConnectAcceptsDifferentBuildWithIdenticalWire) {
    ScopedHandshakeEnvironment required("MOBILEGL_IPC_REQUIRE_SAME_BUILD", "0");
    CheckHandshake(MOBILEGL_PROTOCOL_ABI_MAJOR, WireFingerprint(), "different-commit",
                   ::MobileGL::Wire::DialMode::Connect, ::MobileGL::Wire::RefuseCode::None);
}
TEST(SessionHandshakeTest, ForkRefusesDifferentBuildWithIdenticalWire) {
    CheckHandshake(MOBILEGL_PROTOCOL_ABI_MAJOR, WireFingerprint(), "different-commit",
                   ::MobileGL::Wire::DialMode::Fork, ::MobileGL::Wire::RefuseCode::BuildFingerprint);
}
// PH-8. THE SERVER SIZES THE SESSION; THE CLIENT'S HELLO ONLY ASKS.
//
// Hello.linkTerms carries four byte counts a client may fill in, and nothing on the server reads
// them for sizing: Accept builds the segments from its own SetSegmentSizes and Welcome states
// those. That was stated (CONTRACT-P65 LinkTerms) and never pinned, so a refactor that "honoured
// the client's request" would have handed any peer a 64 GiB allocation per connection. Here a
// Hello asks for 64 GiB in every window and the Welcome must come back with the server's own
// 4 KiB terms and 4 KiB segments - the allocation that happened is the one Welcome describes.
// RED ONCE by making Accept size from the Hello's terms (m_sizes <- hello->linkTerms()): the
// Accept below then fails to create a 64 GiB private segment, or the EXPECT_EQs name the window
// that was echoed. The two-process half, with the child's own VmPeak, is
// TcpLane.SupervisorProtocolControls' `hello_asks_64_gib`.
//
// "Clamped" in the name is the plan's word; what Accept does is IGNORE the ask - it never reads
// the four counts for sizing - and the session is server-sized. Since the F fix round that is
// said out loud: an ask above the granted terms logs one MGLOG_W naming both sides (rule I: a
// silent difference between what a client configured and what it got is the kind this phase
// removes), and this case requires the line. RED before that fix: the line is absent.
TEST(SessionHandshakeTest, AHelloAskingFor64GiBIsClampedToTheServersTermsAndAllocatesNothingOfIt) {
    using namespace ::MobileGL::Wire;
    constexpr Uint64 kAsk = 64ull << 30;
    std::unique_ptr<Transport::InProcessTransport> client, server;
    Transport::InProcessTransport::CreatePair(client, server);
    ::flatbuffers::FlatBufferBuilder builder(512);
    auto terms = CreateLinkTerms(builder, DataPlane::SharedSegments, WireForm::StructImage, kAsk, kAsk, kAsk, kAsk);
    auto hello = CreateHelloDirect(builder, MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR,
                                   BuildFingerprint(), 0, 1, nullptr, WireFingerprint(), WireFingerprint(),
                                   terms, nullptr, DialMode::No);
    auto root = CreateCtrlEnvelope(builder, CtrlMsg::Hello, hello.Union());
    FinishCtrlEnvelopeBuffer(builder, root);
    std::vector<Uint8> first(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    Server::ServerSession session;
    Transport::SessionSegmentSizes sizes;
    sizes.CmdRingBytes = 4096; sizes.StageBytes = 4096;
    sizes.ReplyBytes = 4096; sizes.EventRingBytes = 4096;
    session.SetSegmentSizes(sizes);
    const std::string before = ReadLog();
    ASSERT_EQ(session.Accept(*server, &first), MOBILEGL_OK);
    const std::string delta = ReadLog().substr(before.size());
    EXPECT_TRUE(Contains(delta, "MG_Remote server: Hello asked for windows the server does not grant"))
        << "the 64 GiB ask was ignored silently; an operator comparing the client's configured "
           "windows with the server's would see no line. Log delta:\n" << delta;
    const auto reply = ReadHandshakeFrame(*client);
    ASSERT_FALSE(reply.empty());
    ::flatbuffers::Verifier verifier(reply.data(), reply.size());
    ASSERT_TRUE(VerifyCtrlEnvelopeBuffer(verifier));
    const auto* welcome = GetCtrlEnvelope(reply.data())->msg_as_Welcome();
    ASSERT_NE(welcome, nullptr);
    ASSERT_NE(welcome->linkTerms(), nullptr);
    EXPECT_EQ(welcome->linkTerms()->cmdWindowBytes(), 4096u) << "the command window echoed the Hello";
    EXPECT_EQ(welcome->linkTerms()->stageWindowBytes(), 4096u) << "the stage window echoed the Hello";
    EXPECT_EQ(welcome->linkTerms()->eventWindowBytes(), 4096u) << "the event window echoed the Hello";
    EXPECT_EQ(welcome->linkTerms()->maxReplyBytes(),
              4096u / sizes.ReplySlotCount - sizeof(Transport::ReplySlotHeader)) << "the reply bound echoed the Hello";
    // The segments that EXIST are the server's: each announced size is a few pages at most, never
    // the ask. A 64 GiB request that had been honoured would show here even if Welcome's terms
    // had been rewritten afterwards.
    for (const auto* segment : {welcome->cmdRing(), welcome->stageRing(), welcome->replyPool(), welcome->eventRing()}) {
        ASSERT_NE(segment, nullptr);
        EXPECT_LE(segment->sizeBytes(), 64u * 1024u);
    }
    session.Close();
}

TEST(SessionHandshakeTest, ConnectCanRequireTheSameBuild) {
    ScopedHandshakeEnvironment required("MOBILEGL_IPC_REQUIRE_SAME_BUILD", "1");
    CheckHandshake(MOBILEGL_PROTOCOL_ABI_MAJOR, WireFingerprint(), "different-commit",
                   ::MobileGL::Wire::DialMode::Connect, ::MobileGL::Wire::RefuseCode::BuildFingerprint);
}

// ---------------------------------------------------------------------------
// The two null-union guards, driven THROUGH the handshakes (ID-46 finding 7)
// ---------------------------------------------------------------------------

// A frame whose tag says Hello and whose Hello is NULL, sent on a real InProcessTransport pair to
// a real ServerSession::Accept - a session of this case's own, not the process singleton. Accept
// must answer MOBILEGL_ERR_PROTOCOL_MISMATCH with its guard's own line, and must not have
// dereferenced the member: nothing Fatal in the log, nothing accepted. RED ONCE by deleting
// `envelope->msg_as_Hello() == nullptr` from the guard in ServerSession.cpp: the tag check
// passes, `hello` is nullptr, and `hello->buildFingerprint()` reads address 0 - the case dies
// instead of returning. That perturbation was run.
TEST(SessionHandshakeTest, ANullUnionHelloIsRefusedByAcceptRatherThanDereferenced) {
    std::unique_ptr<Transport::InProcessTransport> client;
    std::unique_ptr<Transport::InProcessTransport> server;
    Transport::InProcessTransport::CreatePair(client, server);
    ASSERT_NE(client, nullptr);
    ASSERT_NE(server, nullptr);

    const std::vector<Uint8> frame = BuildNullUnionFrame(::MobileGL::Wire::CtrlMsg::Hello);
    ASSERT_EQ(client->SendFrame(MobileGLByteSpan{frame.data(), frame.size()}), MOBILEGL_OK);

    Server::ServerSession session;
    const std::string before = ReadLog();
    EXPECT_EQ(session.Accept(*server), MOBILEGL_ERR_PROTOCOL_MISMATCH)
        << "a null-union Hello was not refused by the handshake";
    EXPECT_FALSE(session.Accepted());
    const std::string delta = ReadLog().substr(before.size());
    EXPECT_TRUE(Contains(delta, "MG_Remote server: the first control frame is not a verifiable Hello"))
        << "Accept refused, but not with the guard's own line. Log delta:\n"
        << delta;
    EXPECT_FALSE(Contains(delta, "Fatal{")) << "the refusal became a Fatal. Log delta:\n" << delta;
    session.Close();
}

// The Welcome guard. ClientSession::Start builds its transport pair itself, so nothing could put
// a frame on the server->client direction ahead of the server's Welcome - which is why
// StartOverTransportPair, Start's second half, is public (ClientSession.h). The frame is queued
// there BEFORE Start sends Hello: the server's Accept then runs for real (segments, Welcome,
// resolver), its genuine Welcome queues behind the null-union one, the client reads the
// null-union one first and must refuse it with the guard's own line, and Stop()'s not-started
// path must have closed the server the handshake had accepted. RED ONCE by deleting
// `envelope->msg_as_Welcome() == nullptr` from the guard in ClientSession.cpp: `welcome` is then
// nullptr and `welcome->buildFingerprint()` reads address 0 - the case dies. That perturbation
// was run.
TEST(SessionHandshakeTest, ANullUnionWelcomeIsRefusedByStartRatherThanDereferenced) {
    std::unique_ptr<Transport::InProcessTransport> client;
    std::unique_ptr<Transport::InProcessTransport> server;
    Transport::InProcessTransport::CreatePair(client, server);
    ASSERT_NE(client, nullptr);
    ASSERT_NE(server, nullptr);

    const std::vector<Uint8> frame = BuildNullUnionFrame(::MobileGL::Wire::CtrlMsg::Welcome);
    ASSERT_EQ(server->SendFrame(MobileGLByteSpan{frame.data(), frame.size()}), MOBILEGL_OK);

    Client::ClientSession& session = Client::ClientSessionInstance();
    Server::ServerSession& serverSession = Server::ServerSessionInstance();
    ASSERT_FALSE(session.Started());
    ASSERT_FALSE(serverSession.Accepted());

    const std::string before = ReadLog();
    EXPECT_EQ(session.StartOverTransportPair(std::move(client), std::move(server)),
              MOBILEGL_ERR_PROTOCOL_MISMATCH)
        << "a null-union Welcome was not refused by the handshake";
    EXPECT_FALSE(session.Started());
    EXPECT_EQ(Client::ClientSession::Active(), nullptr);
    // Stop()'s not-started path closes the server FIRST (ClientSession.cpp); a server left
    // m_accepted would refuse every later Start in this process.
    EXPECT_FALSE(serverSession.Accepted());
    EXPECT_EQ(Server::ServerSession::Active(), nullptr);

    const std::string delta = ReadLog().substr(before.size());
    EXPECT_TRUE(Contains(delta,
                         "MG_Remote client: the server's first control frame is not a verifiable "
                         "Welcome"))
        << "Start refused, but not with the guard's own line. Log delta:\n"
        << delta;
    EXPECT_FALSE(Contains(delta, "Fatal{AbiMismatch"))
        << "the null-union Welcome reached the fingerprint compare. Log delta:\n"
        << delta;
    // And the server's half of the handshake DID run - the Hello it received was this
    // client's real one - so the case drove Start past the point a stub would stop at.
    EXPECT_TRUE(Contains(delta, "MG_Remote server: accepted with NO backend"))
        << "ServerSession::Accept never ran, so the Welcome guard was not reached the way "
           "Start reaches it. Log delta:\n"
        << delta;
}

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid, because
    // gtest_discover_tests runs every case as its own process, in parallel under ctest -j.
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() /
                          ("mobilegl-sessionhandshake-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
