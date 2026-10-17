// MobileGL - MobileGL/MG_IntegrationTest/Harness/SplitLaneHarnessTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// SplitLane::TransportClaimMismatch on canned inputs: each claim must be refused when the process
// resolved another transport or the wrong process serves it, and accepted when both agree.

#include "SplitLane.h"

#include <gtest/gtest.h>

namespace {

    using MGITest::SplitLane::TransportClaimMismatch;

    constexpr unsigned long long kSelf = 4242;
    constexpr unsigned long long kServer = 4343;

    TEST(SplitLaneHarness, AnInprocClaimNeedsTheServerThreadHere) {
        EXPECT_EQ(TransportClaimMismatch("inproc", "", "inproc", kSelf, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "spawn", kServer, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "inproc", kServer, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "inproc", 0, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "monolith", 0, kSelf), "");
    }

    TEST(SplitLaneHarness, ASpawnClaimNeedsAServerProcessOfItsOwn) {
        EXPECT_EQ(TransportClaimMismatch("spawn", "", "spawn", kServer, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "", "inproc", kSelf, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "", "spawn", kSelf, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "", "spawn", 0, kSelf), "");
    }

    TEST(SplitLaneHarness, ATcpClaimIsASpawnSessionToAnotherProcess) {
        EXPECT_EQ(TransportClaimMismatch("spawn", "tcp://127.0.0.1:30613", "spawn", kServer, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "tcp://127.0.0.1:30613", "spawn", kSelf, kSelf), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "tcp://127.0.0.1:30613", "inproc", kSelf, kSelf), "");
    }

    TEST(SplitLaneHarness, NoClaimIsNotJudged) {
        EXPECT_EQ(TransportClaimMismatch("", "", "monolith", 0, kSelf), "");
        EXPECT_EQ(TransportClaimMismatch("monolith", "", "monolith", 0, kSelf), "");
    }

    TEST(SplitLaneHarness, AnUnknownOwnPidStillJudgesTheTransportAndTheServer) {
        EXPECT_EQ(TransportClaimMismatch("inproc", "", "inproc", kSelf, 0), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "spawn", kServer, 0), "");
        EXPECT_NE(TransportClaimMismatch("inproc", "", "inproc", 0, 0), "");
        EXPECT_EQ(TransportClaimMismatch("spawn", "", "spawn", kServer, 0), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "", "inproc", kSelf, 0), "");
        EXPECT_NE(TransportClaimMismatch("spawn", "", "spawn", 0, 0), "");
    }

    TEST(SplitLaneHarness, TheRefusalNamesBothPids) {
        const std::string message = TransportClaimMismatch("spawn", "", "spawn", kSelf, kSelf);
        EXPECT_NE(message.find("server pid 4242"), std::string::npos) << message;
        EXPECT_NE(message.find("this process 4242"), std::string::npos) << message;
    }

} // namespace
