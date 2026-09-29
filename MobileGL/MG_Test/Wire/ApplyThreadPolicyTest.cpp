// MobileGL - MobileGL/MG_Test/Wire/ApplyThreadPolicyTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 B1 (CONTRACT-P11 B1): the apply thread's affinity and spin, selected per session from the
// peer's Hello dial mode and data plane. Every row of ApplyThreadPolicy.h's table, with the Redmi's
// topology (cpu0-5 performance, cpu6-7 prime: big 0xc0 of 0xff) and the shapes that must not be read
// as one.

#include <MG_Remote/Server/ApplyThreadPolicy.h>

#include <gtest/gtest.h>

using namespace MobileGL;
using namespace MobileGL::MG_Remote::Server;

namespace {
    constexpr Uint64 kRedmiBig = 0xc0;
    constexpr Uint64 kRedmiOnline = 0xff;
    constexpr Uint32 kConfigured = 50;
    constexpr Bool kShm = true;
    constexpr Bool kStream = false;
} // namespace

TEST(ApplyThreadPolicy, AutoKeepsEveryBigCoreAndTheConfiguredSpinForAnInProcessOrForkedPeer) {
    // inproc and the same-app fork spawn: exactly the pre-B1 `auto` (A4's measured shapes).
    for (const ApplyPeer peer : {ApplyPeer::InProcess, ApplyPeer::Forked}) {
        const auto policy =
            SelectApplyThreadPolicy("auto", false, kConfigured, peer, kShm, kRedmiBig, kRedmiOnline);
        EXPECT_TRUE(policy.recognised);
        EXPECT_EQ(policy.requestedMask, kRedmiBig) << ApplyPeerName(peer);
        EXPECT_EQ(policy.spinUs, kConfigured) << ApplyPeerName(peer);
    }
}

TEST(ApplyThreadPolicy, AutoKeepsEveryBigCoreForADialledInPeerOnTheStreamPlane) {
    // tcp: an io thread feeds the apply thread; nothing changes for a TCP client.
    const auto policy =
        SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Connected, kStream, kRedmiBig, kRedmiOnline);
    EXPECT_EQ(policy.requestedMask, kRedmiBig);
    EXPECT_EQ(policy.spinUs, kConfigured);
}

TEST(ApplyThreadPolicy, AutoGivesADialledInSharedSegmentPeerTheLowestPrimeCoreOnly) {
    const auto policy =
        SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Connected, kShm, kRedmiBig, kRedmiOnline);
    EXPECT_TRUE(policy.recognised);
    EXPECT_EQ(policy.requestedMask, 0x40u)
        << "the LOWEST big core: the scheduler puts a foreground client's GL thread on the highest";
    EXPECT_EQ(policy.spinUs, kConnectedPeerSpinUs);
    // A non-contiguous prime set: still exactly one core, the lowest.
    EXPECT_EQ(SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Connected, kShm, 0x0a, 0xff).requestedMask,
              0x02u);
    EXPECT_EQ(ReservedApplyCore(kRedmiBig, kRedmiOnline), 0x40u) << "the core the broker tells the helper to avoid";
}

TEST(ApplyThreadPolicy, AnEmptyOrMissingAffinityIsAuto) {
    for (const char* text : {static_cast<const char*>(nullptr), ""}) {
        EXPECT_EQ(SelectApplyThreadPolicy(text, false, kConfigured, ApplyPeer::Connected, kShm, kRedmiBig,
                                          kRedmiOnline)
                      .requestedMask,
                  0x40u);
        EXPECT_EQ(SelectApplyThreadPolicy(text, false, kConfigured, ApplyPeer::Forked, kShm, kRedmiBig, kRedmiOnline)
                      .requestedMask,
                  kRedmiBig);
    }
}

TEST(ApplyThreadPolicy, ADialledInPeerGetsNoAffinityWhereThereIsNoPrimePairToSplit) {
    // A symmetric machine (every cpu is "big"), a single prime core, an unreadable topology: none
    // has a second prime core to leave to the client, so the scheduler places both threads.
    for (const Uint64 big : {Uint64{0xff}, Uint64{0x80}, Uint64{0}}) {
        EXPECT_EQ(SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Connected, kShm, big, 0xff)
                      .requestedMask,
                  0u)
            << big;
        EXPECT_EQ(ReservedApplyCore(big, 0xff), 0u) << big;
    }
    // ...and the in-process / forked rows keep the whole (symmetric) big set as before.
    EXPECT_EQ(SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Forked, kShm, 0xff, 0xff).requestedMask,
              0xffu);
}

TEST(ApplyThreadPolicy, AnExplicitSpinWinsForADialledInPeer) {
    const auto policy =
        SelectApplyThreadPolicy("auto", true, 7, ApplyPeer::Connected, kShm, kRedmiBig, kRedmiOnline);
    EXPECT_EQ(policy.spinUs, 7u);
    EXPECT_EQ(policy.requestedMask, 0x40u);
}

TEST(ApplyThreadPolicy, AnExplicitMaskOrOffWinsForEveryPeerAndPlane) {
    for (const ApplyPeer peer : {ApplyPeer::InProcess, ApplyPeer::Forked, ApplyPeer::Connected}) {
        for (const Bool plane : {kShm, kStream}) {
            const auto off = SelectApplyThreadPolicy("off", false, kConfigured, peer, plane, kRedmiBig, kRedmiOnline);
            EXPECT_TRUE(off.recognised);
            EXPECT_EQ(off.requestedMask, 0u) << ApplyPeerName(peer);
            EXPECT_EQ(off.spinUs, kConfigured) << "an operator's affinity keeps the configured spin";
            const auto mask = SelectApplyThreadPolicy("0x80", false, kConfigured, peer, plane, kRedmiBig, kRedmiOnline);
            EXPECT_TRUE(mask.recognised);
            EXPECT_EQ(mask.requestedMask, 0x80u) << ApplyPeerName(peer);
            EXPECT_EQ(SelectApplyThreadPolicy("12", false, kConfigured, peer, plane, kRedmiBig, kRedmiOnline)
                          .requestedMask,
                      12u);
        }
    }
}

TEST(ApplyThreadPolicy, UnrecognisedAffinityTextAppliesNothingAndSaysSo) {
    for (const char* text : {"big", "0xzz", "auto ", "3cores"}) {
        const auto policy =
            SelectApplyThreadPolicy(text, false, kConfigured, ApplyPeer::Connected, kShm, kRedmiBig, kRedmiOnline);
        EXPECT_FALSE(policy.recognised) << text;
        EXPECT_EQ(policy.requestedMask, 0u) << text;
    }
}
