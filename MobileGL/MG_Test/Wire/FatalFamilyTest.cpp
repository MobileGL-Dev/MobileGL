// MobileGL - MobileGL/MG_Test/Wire/FatalFamilyTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P6 `dl` (CONTRACT-P6 5.2). The Fatal-family table and its projection onto the wire FatalCode.

#include <MG_Remote/FatalFamily.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

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
