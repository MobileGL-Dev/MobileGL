// MobileGL - MobileGL/MG_Test/Wire/FatalFamilyTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P6 `dl` (CONTRACT-P6 5.2). The Fatal-family table and its projection onto the wire FatalCode.

#include <MG_Pipe/PipeSessionFail.h>
#include <MG_Remote/FatalFamily.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace MobileGL::MG_Remote;
using MobileGL::Wire::FatalCode;

namespace {

    // Every family, walked once, so a new row is covered without editing this list.
    const MGFatalFamily kAllFamilies[] = {
#define X(Family, WireCode, Why) MGFatalFamily::Family,
        MGL_FATAL_FAMILY_LIST(X)
#undef X
    };

#if !defined(_WIN32)
    // P7 wave 0. SessionFaultCount() is the only thing the funnel leaves behind that a test can
    // read, and SessionFail aborts before returning - so the counter has to be read from INSIDE
    // the dying process. A SIGABRT handler turns it into the child's exit code: kFaultExitBase +
    // n. "The funnel was reached exactly once" is then a value the parent asserts rather than a
    // log line it has to parse, and a seam that bypassed the funnel would exit 0x60, not 0x61.
    constexpr int kFaultExitBase = 0x60;

    void ReportFaultCountAndExit(int) {
        std::_Exit(kFaultExitBase + static_cast<int>(MobileGL::MG_Remote::SessionFaultCount()));
    }

    // A hook of the TEST's own, to prove what the seam hands across independently of what
    // SessionFail then does with it. It echoes and returns; MGPipeSessionFail's contract is that
    // it aborts anyway, which is also under test here.
    void EchoingHook(MobileGL::MG_Pipe::MGPipeFatalFamily family, const char* line) {
        std::fprintf(stderr, "seam-saw family=%u line=%s\n", static_cast<unsigned>(family), line);
        std::fflush(stderr);
    }
#endif

} // namespace

TEST(FatalFamily, TheTableAndTheEnumAgreeOnCount) {
    EXPECT_EQ(sizeof(kAllFamilies) / sizeof(kAllFamilies[0]), MGFatalFamilyCount());
    // a6 censused thirty; the vocabulary has grown since, on purpose. The point of this line is
    // that the number is stated, not that it is any particular value - a change to it is a
    // deliberate edit here, which is exactly the "grows on purpose" the census gate also asks.
    EXPECT_GE(MGFatalFamilyCount(), 30u);
}

TEST(FatalFamily, EveryFamilyProjectsToADefinedWireCode) {
    // A total table: every family maps, and never to None (which is "no fatal"). None as a
    // projection would say a death was not a death.
    for (const MGFatalFamily family : kAllFamilies) {
        const FatalCode code = FatalCodeForFamily(family);
        EXPECT_NE(code, FatalCode::None)
            << FatalFamilyName(family) << " projects onto FatalCode::None";
    }
}

TEST(FatalFamily, NoFamilyProjectsOntoDeviceLost) {
    // 5.3: DeviceLost is armed from the doorbell's hangup, never raised as a Fatal. A family that
    // projected onto it would let an abort masquerade as a device loss, which is the exact
    // confusion the latch/abort split exists to prevent.
    for (const MGFatalFamily family : kAllFamilies) {
        EXPECT_NE(FatalCodeForFamily(family), FatalCode::DeviceLost)
            << FatalFamilyName(family) << " projects onto DeviceLost, which only the latch may set";
    }
}

TEST(FatalFamily, NamesAreDistinctAndNonEmpty) {
    std::set<std::string> seen;
    for (const MGFatalFamily family : kAllFamilies) {
        const std::string name = FatalFamilyName(family);
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "<unknown>") << "a real family resolved to the fallback name";
        EXPECT_TRUE(seen.insert(name).second) << name << " is not a distinct family name";
    }
}

TEST(FatalFamily, TheAnchorFamiliesProjectWhereTheirMeaningSays) {
    // A handful pinned by hand, so a projection that drifted in a bulk edit is caught by meaning
    // and not only by "it still maps to something".
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::ProtocolCorruption), FatalCode::ProtocolCorruption);
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::RingOverrun), FatalCode::RingOverrun);
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::EventRingOverflow), FatalCode::RingOverrun);
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::AbiMismatch), FatalCode::AbiMismatch);
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::StageSnapshotTooNarrow), FatalCode::SegmentMismatch);
    // The liveness families are what a WAITING peer attributes to the other side being gone.
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::BarrierTimeout), FatalCode::ServerCrashed);
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::ApplyThreadNotRunning), FatalCode::ServerCrashed);
    // An unmigrated verb is a protocol-level "cannot honour this", not a crash.
    EXPECT_EQ(FatalCodeForFamily(MGFatalFamily::UnmigratedVerb), FatalCode::ProtocolCorruption);
}

// ---------------------------------------------------------------------------------------------
// P7 wave 0: the seam MG_Backend's three Magma wire funnels die through (MG_Pipe/
// PipeSessionFail.h). Until this package they logged and raised their own std::abort(), so
// fifteen `@P7` refusals plus WireBufferLegacyFatal published no SessionFault to the peer, bumped
// no SessionFaultCount() and were invisible to the census gate. These three cases are the
// red-once for that: delete InstallPipeSessionFailHook()'s call in InitServerRoleCommon and the
// first two go red on the exit code, point a funnel back at MGLOG_F + abort and all three do.

TEST(FatalFunnelSeam, InstallingTheHookPointsTheSeamSomewhereOtherThanItsDefault) {
    using namespace MobileGL;
    ASSERT_EQ(MG_Pipe::MGPipeSessionFailHookInstalled(), nullptr)
        << "the seam was already installed before this case ran; the binary's default is the "
           "no-hook one and only a server-role init should change it";
    MG_Remote::InstallPipeSessionFailHook();
    EXPECT_NE(MG_Pipe::MGPipeSessionFailHookInstalled(), nullptr);
    // Restored, so the two cases below start from the same state whichever order they run in.
    MG_Pipe::MGPipeInstallSessionFailHook(nullptr);
    EXPECT_EQ(MG_Pipe::MGPipeSessionFailHookInstalled(), nullptr);
}

TEST(FatalFunnelSeam, TheMagmaWireVerbDeathReachesSessionFailAndBumpsTheFaultCount) {
#if defined(_WIN32)
    GTEST_SKIP() << "the seam's proof forks and reads a signal, which is POSIX only - the same "
                    "reason MG_Test/Wire's spawn cases are POSIX only";
#else
    using namespace MobileGL;
    // WireFramebuffer.inc's MagmaWireFatal, character for character - the funnel eleven of the
    // fifteen `@P7` refusals die through.
    EXPECT_EXIT(
        {
            std::signal(SIGABRT, &ReportFaultCountAndExit);
            MG_Remote::InstallPipeSessionFailHook();
            MG_Pipe::MGPipeSessionFail(MG_Pipe::MGPipeFatalFamily::UnmigratedVerb,
                                       "MGPipe: Fatal{UnmigratedVerb, \"Magma:%s\"}",
                                       "unit-probe");
        },
        ::testing::ExitedWithCode(kFaultExitBase + 1),
        "Fatal\\{UnmigratedVerb, \"Magma:unit-probe\"\\}")
        << "the Magma wire funnel did not end through SessionFail: either the line is not the "
           "site's own any more, or SessionFaultCount() did not move - which is what exit gate "
           "S8 reads and what every telemetry consumer of the funnel depends on";
#endif
}

TEST(FatalFunnelSeam, TheSeamHandsTheInstalledHookItsFamilyAndItsLineVerbatim) {
#if defined(_WIN32)
    GTEST_SKIP() << "POSIX only, as above";
#else
    using namespace MobileGL;
    // WireDraw.inc's WireBufferLegacyFatal, character for character. Through a hook of the test's
    // own rather than MG_Remote's, so what is under test is the SEAM - that the family enum and
    // the fully formatted line cross intact - and not what SessionFail then does with them. The
    // hook returns; MGPipeSessionFail must still abort, which is why this is an exit assertion.
    EXPECT_EXIT(
        {
            MG_Pipe::MGPipeInstallSessionFailHook(&EchoingHook);
            MG_Pipe::MGPipeSessionFail(
                MG_Pipe::MGPipeFatalFamily::RoleViolation,
                "MGPipe: Fatal{RoleViolation, \"buffer-legacy-arm\"} (Magma P7 buffer consumer)");
        },
        ::testing::KilledBySignal(SIGABRT),
        "seam-saw family=1 line=MGPipe: Fatal\\{RoleViolation, \"buffer-legacy-arm\"\\} "
        "\\(Magma P7 buffer consumer\\)")
        << "the seam changed what it hands across: either the family ordinal moved (which would "
           "re-point FatalFunnel.cpp's adapter arm at the wrong .def row) or the line is no "
           "longer the site's own string, which CONTRACT-P6 5.2 requires to stay byte-identical";
#endif
}

TEST(FatalFamily, TheSessionFaultFrameCarriesTheFamilyAndItsCode) {
    // `dl` step three (5.2): the Fatal frame a dying session publishes carries the full family
    // WORD beside the coarse code, so a peer names which family ended the session instead of
    // reading a bare EOF. This is the wire round-trip that proves the field is there and that the
    // code the frame carries is the family's own projection.
    for (const MGFatalFamily family : kAllFamilies) {
        ::flatbuffers::FlatBufferBuilder builder(256);
        const auto fatal = ::MobileGL::Wire::CreateFatalDirect(
            builder, FatalCodeForFamily(family), "detail line", FatalFamilyName(family));
        const auto root = ::MobileGL::Wire::CreateCtrlEnvelope(
            builder, ::MobileGL::Wire::CtrlMsg::Fatal, fatal.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, root);

        ::flatbuffers::Verifier verifier(builder.GetBufferPointer(), builder.GetSize());
        ASSERT_TRUE(::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier));
        const auto* envelope = ::MobileGL::Wire::GetCtrlEnvelope(builder.GetBufferPointer());
        ASSERT_EQ(envelope->msg_type(), ::MobileGL::Wire::CtrlMsg::Fatal);
        const auto* decoded = envelope->msg_as_Fatal();
        ASSERT_NE(decoded, nullptr);
        ASSERT_NE(decoded->family(), nullptr) << "the family field did not survive the wire";
        EXPECT_STREQ(decoded->family()->c_str(), FatalFamilyName(family));
        EXPECT_EQ(decoded->code(), FatalCodeForFamily(family));
    }
}
