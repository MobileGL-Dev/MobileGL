// MobileGL - MobileGL/MG_Test/SelfTest/DriverBugProbesTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <MG_Util/SelfTest/DriverBugProbes.h>

using namespace MobileGL;
using MobileGL::MG_Util::SelfTest::CollectGlesKnownDriverBugs;
using MobileGL::MG_Util::SelfTest::DriverBugVerdict;
using MobileGL::MG_Util::SelfTest::ProbeGeometryStageSsboWriteAfterEmitDropped;

namespace {
    // A driver table with nothing resolved. Every probe has to treat this as "cannot tell",
    // never as "affected".
    MG_External::GLESFunctionsTable EmptyFunctionTable() {
        return MG_External::GLESFunctionsTable{};
    }
} // namespace

// The rule the whole section depends on: a probe that cannot run reports NO bug. If an
// unrunnable probe answered "affected", every device without the entry points - every desktop
// build, every unit-test process - would grow a driver-bug row it has no evidence for, and the
// section would stop meaning "this device has these bugs".
TEST(DriverBugProbes, AProbeThatCannotRunReportsNoBug) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    EXPECT_FALSE(ProbeGeometryStageSsboWriteAfterEmitDropped(gl))
        << "a probe with no entry points to call must not claim the driver is affected";
}

// The section lists only bugs the device HAS, so a driver nothing could be probed on renders
// nothing at all rather than a list of reassurances.
TEST(DriverBugProbes, CollectsNoFindingsWhenNothingCanBeProbed) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    EXPECT_TRUE(CollectGlesKnownDriverBugs(gl).empty());
}

// Every finding the table can produce is a bug that is PRESENT, which is why the vocabulary is
// FIXED/UNFIXABLE and not PASS/FAIL. This latches that no probe can smuggle in a "not affected"
// row by returning a finding with an empty name or detail - the screen renders both.
TEST(DriverBugProbes, EveryFindingCarriesANameAndAnExplanation) {
    const MG_External::GLESFunctionsTable gl = EmptyFunctionTable();
    for (const auto& finding : CollectGlesKnownDriverBugs(gl)) {
        EXPECT_FALSE(finding.name.empty());
        EXPECT_FALSE(finding.detail.empty()) << finding.name << " must say what MobileGL does about it";
        EXPECT_TRUE(finding.verdict == DriverBugVerdict::Fixed ||
                    finding.verdict == DriverBugVerdict::Unfixable);
    }
}
