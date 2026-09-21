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
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
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
    const Uint64 production = CapsAbiFingerprint();
    EXPECT_NE(production, 0u) << "0 is reserved for \"not stated\"";
    EXPECT_EQ(production, CapsAbiFingerprint()) << "not stable within one build";

    const Transport::AbiFingerprintInputs inputs = CapsAbiFingerprintInputs();
    EXPECT_EQ(production, Transport::MixAbiFingerprint(inputs))
        << "CapsAbiFingerprint() is not MixAbiFingerprint over CapsAbiFingerprintInputs(): the "
           "handshake compares a value this case cannot reach, which is finding 6 again";

    // The inputs are the real ones, so a CapsAbiFingerprintInputs() that hard-coded a size
    // would be caught here rather than agreed with by a peer built from a different tree.
    EXPECT_EQ(inputs.DynamicParamsSize, sizeof(MG_Backend::DynamicBackendParameters));
    EXPECT_EQ(inputs.CapsSize, sizeof(MG_Pipe::MGPCaps));
    EXPECT_EQ(inputs.FunctionTableSize, sizeof(MG_Backend::GLFunctionsTable));
    EXPECT_EQ(inputs.FormatCapabilityTargets,
              static_cast<Uint64>(MG_Backend::kFormatCapabilityTargetCount));
    EXPECT_EQ(inputs.FormatCapabilityFormats,
              static_cast<Uint64>(MG_Backend::kFormatCapabilityFormatCount));
    EXPECT_NE(inputs.FormatCapabilitiesCodecVersion, 0u);
    EXPECT_NE(inputs.RendererInfoCodecVersion, 0u);
    EXPECT_EQ(inputs.OpCount, static_cast<Uint64>(MG_Pipe::MGPWireOp::kOpCount));
    EXPECT_EQ(inputs.AbiVersion, static_cast<Uint32>(MOBILEGL_ABI_VERSION(
                                     MOBILEGL_PROTOCOL_ABI_MAJOR, MOBILEGL_PROTOCOL_ABI_MINOR)));
    // CONTRACT-P6 4.2. THE ASSERTION SPLITS WITH THE MACRO PAIR, and it has to: this used to
    // demand a non-empty stamp unconditionally and was RED in every build whose git could not be
    // read - a real signal, but one that told the reader nothing about WHICH of the two states
    // the build was in. Now the build system states it, and both states are asserted.
    ASSERT_NE(inputs.BuildStamp, nullptr);
    EXPECT_EQ(inputs.BuildStampPresent, static_cast<Uint32>(MOBILEGL_BUILD_STAMP_PRESENT))
        << "the fingerprint disagrees with the build system about whether this build has a stamp";
#if MGL_HANDSHAKE_TEST_HAS_GIT_HASH
    EXPECT_STREQ(inputs.BuildStamp, MOBILEGL_BUILD_STAMP_VALUE);
#endif
#if MOBILEGL_BUILD_STAMP_PRESENT
    EXPECT_NE(inputs.BuildStamp[0], '\0')
        << "the build claims a stamp and the fingerprint mixed an empty one";
#else
    // A stampless build is a KNOWN, DECLARED state - CMake warns at configure time - but it must
    // not be a silent one here either. What still has to hold is that the ABSENCE is mixed, which
    // the BuildStampPresent perturbation below proves.
    EXPECT_STREQ(inputs.BuildStamp, "")
        << "PRESENT is 0 but a value came through; the two halves of the pair disagree";
#endif

    // Every input moves the answer. Each lambda changes exactly one field of a copy of the
    // REAL inputs, so what is proven is that the production value depends on that field.
    const auto perturbed = [&](auto&& mutate) {
        Transport::AbiFingerprintInputs copy = inputs;
        mutate(copy);
        return Transport::MixAbiFingerprint(copy);
    };
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.DynamicParamsSize; }))
        << "sizeof(DynamicBackendParameters) is not mixed";
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.CapsSize; }))
        << "sizeof(MGPCaps) is not mixed";
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.FunctionTableSize; }))
        << "sizeof(GLFunctionsTable) is not mixed";
    EXPECT_NE(production,
              perturbed([](Transport::AbiFingerprintInputs& i) { ++i.FormatCapabilityTargets; }))
        << "kFormatCapabilityTargetCount is not mixed";
    EXPECT_NE(production,
              perturbed([](Transport::AbiFingerprintInputs& i) { ++i.FormatCapabilityFormats; }))
        << "kFormatCapabilityFormatCount is not mixed";
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) {
                  ++i.FormatCapabilitiesCodecVersion;
              }))
        << "kFormatCapabilitiesCodecVersion is not mixed";
    EXPECT_NE(production,
              perturbed([](Transport::AbiFingerprintInputs& i) { ++i.RendererInfoCodecVersion; }))
        << "kRendererInfoCodecVersion is not mixed";
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.OpCount; }))
        << "MGPWireOp::kOpCount is not mixed (ID-33)";
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.AbiVersion; }))
        << "the protocol ABI version is not mixed (ID-33)";
    EXPECT_NE(production,
              perturbed([](Transport::AbiFingerprintInputs& i) { i.BuildStamp = "not-this-build"; }))
        << "the git stamp is not mixed";
    // A missing stamp is not the same as an empty one, and neither is the same as a real build.
    // THE PRESENCE FLAG IS ITSELF AN INPUT (4.2), and this is the assertion that makes a
    // stampless build safe to reason about: a peer that says "I could not name my commit" must
    // not produce the same fingerprint as one whose commit genuinely is "". It has to move the
    // answer in EITHER build state, which is why it sits outside the #if below.
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { ++i.BuildStampPresent; }))
        << "BuildStampPresent is not mixed; 'no stamp' and 'empty stamp' are one input again";

    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { i.BuildStamp = nullptr; }));
#if MOBILEGL_BUILD_STAMP_PRESENT
    // Vacuous without a stamp: production's value already IS "", so this perturbation would be
    // the identity and the assertion would pass while proving nothing.
    EXPECT_NE(production, perturbed([](Transport::AbiFingerprintInputs& i) { i.BuildStamp = ""; }));
#endif
    EXPECT_NE(perturbed([](Transport::AbiFingerprintInputs& i) { i.BuildStamp = nullptr; }),
              perturbed([](Transport::AbiFingerprintInputs& i) { i.BuildStamp = ""; }))
        << "\"no stamp\" and \"an empty stamp\" collapsed into one input";
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
