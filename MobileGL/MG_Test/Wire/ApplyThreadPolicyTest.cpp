// MobileGL - MobileGL/MG_Test/Wire/ApplyThreadPolicyTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 B1 (CONTRACT-P11 B1): the apply thread's affinity and spin, selected per session from the
// peer's Hello dial mode and data plane, and the core reserved for a dialled-in shared-segment peer
// on every topology shape that matters - the Redmi's (cpu0-5 performance, cpu6-7 prime: big 0xc0,
// prime 0xc0 of 0xff), a lone prime core beside other big cores, a lone prime core alone, symmetric
// machines, an unreadable topology.

#include <MG_Remote/Server/ApplyThreadPolicy.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

using namespace MobileGL;
using namespace MobileGL::MG_Remote::Server;

namespace {
    constexpr Uint64 kRedmiBig = 0xc0;
    constexpr Uint64 kRedmiPrime = 0xc0;
    constexpr Uint64 kRedmiOnline = 0xff;
    constexpr Uint32 kConfigured = 50;
    constexpr Bool kShm = true;
    constexpr Bool kStream = false;

    ApplyThreadPolicy Connected(const char* affinity, Uint64 big, Uint64 prime, Uint64 online) {
        return SelectApplyThreadPolicy(affinity, false, kConfigured, ApplyPeer::Connected, kShm, big, prime, online);
    }
} // namespace

TEST(ApplyThreadPolicy, AutoKeepsEveryBigCoreAndTheConfiguredSpinForAnInProcessOrForkedPeer) {
    // inproc and the same-app fork spawn: exactly the pre-B1 `auto` (A4's measured shapes).
    for (const ApplyPeer peer : {ApplyPeer::InProcess, ApplyPeer::Forked}) {
        const auto policy = SelectApplyThreadPolicy("auto", false, kConfigured, peer, kShm, kRedmiBig, kRedmiPrime,
                                                    kRedmiOnline);
        EXPECT_TRUE(policy.recognised);
        EXPECT_EQ(policy.requestedMask, kRedmiBig) << ApplyPeerName(peer);
        EXPECT_EQ(policy.spinUs, kConfigured) << ApplyPeerName(peer);
    }
}

TEST(ApplyThreadPolicy, AutoKeepsEveryBigCoreForADialledInPeerOnTheStreamPlane) {
    // tcp: an io thread feeds the apply thread; nothing changes for a TCP client.
    const auto policy = SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Connected, kStream, kRedmiBig,
                                                kRedmiPrime, kRedmiOnline);
    EXPECT_EQ(policy.requestedMask, kRedmiBig);
    EXPECT_EQ(policy.spinUs, kConfigured);
}

TEST(ApplyThreadPolicy, SeveralPrimeCoresGiveTheApplyThreadTheLowestOfThem) {
    const auto policy = Connected("auto", kRedmiBig, kRedmiPrime, kRedmiOnline);
    EXPECT_TRUE(policy.recognised);
    EXPECT_EQ(policy.requestedMask, 0x40u)
        << "the LOWEST prime core: the scheduler puts a foreground client's GL thread on the highest";
    EXPECT_EQ(policy.spinUs, kConnectedPeerSpinUs);
    EXPECT_NE(std::string(policy.rule).find("lowest of the prime cores"), std::string::npos) << policy.rule;
    // A non-contiguous prime set: still exactly one core, the lowest.
    EXPECT_EQ(Connected("auto", 0x0a, 0x0a, 0xff).requestedMask, 0x02u);
}

TEST(ApplyThreadPolicy, ALonePrimeCoreIsNeverTheApplyThreadsItTakesABigCoreBesideIt) {
    // cpu0-3 little, cpu4 the only core at the peak (3.3 GHz), cpu5-7 big (3.0, within 15%): the
    // lowest-numbered big core IS the prime here, and it stays the client's - the apply thread takes cpu5.
    ReservedCoreReason reason = ReservedCoreReason::NoAsymmetry;
    EXPECT_EQ(ReservedApplyCore(0xf0, 0x10, 0xff, &reason), 0x20u);
    EXPECT_EQ(reason, ReservedCoreReason::BigCoreBesideTheOnlyPrime);
    const auto policy = Connected("auto", 0xf0, 0x10, 0xff);
    EXPECT_EQ(policy.requestedMask, 0x20u);
    EXPECT_NE(std::string(policy.rule).find("beside the only prime core"), std::string::npos) << policy.rule;
    // A Snapdragon 8 Gen 2 shape: cpu0-2 little, cpu3-6 big within 15%, cpu7 the lone prime.
    EXPECT_EQ(ReservedApplyCore(0xf8, 0x80, 0xff), 0x08u);
}

TEST(ApplyThreadPolicy, ALonePrimeCoreWithNoOtherBigCoreLeavesTheApplyThreadUnpinned) {
    // A Tensor-G3-like shape read at 85%: only cpu8 is big, and it is the prime. Pinning the spinning
    // apply thread there would take the one core the client wants; it stays unpinned, and says so.
    ReservedCoreReason reason = ReservedCoreReason::NoAsymmetry;
    EXPECT_EQ(ReservedApplyCore(0x100, 0x100, 0x1ff, &reason), 0u);
    EXPECT_EQ(reason, ReservedCoreReason::OnlyPrimeCoreLeftAlone);
    const auto policy = Connected("auto", 0x80, 0x80, 0xff);
    EXPECT_EQ(policy.requestedMask, 0u);
    EXPECT_NE(std::string(policy.rule).find("unpinned - the only prime core"), std::string::npos) << policy.rule;
    EXPECT_EQ(policy.spinUs, kConnectedPeerSpinUs);
}

TEST(ApplyThreadPolicy, NoAsymmetryLeavesTheApplyThreadUnpinned) {
    // A symmetric machine (every cpu big and prime) and an unreadable topology (0): nothing to split.
    for (const auto& shape : {std::pair<Uint64, Uint64>{0xff, 0xff}, std::pair<Uint64, Uint64>{0, 0}}) {
        ReservedCoreReason reason = ReservedCoreReason::LowestOfSeveralPrimeCores;
        EXPECT_EQ(ReservedApplyCore(shape.first, shape.second, 0xff, &reason), 0u) << shape.first;
        EXPECT_EQ(reason, ReservedCoreReason::NoAsymmetry) << shape.first;
        EXPECT_NE(std::string(Connected("auto", shape.first, shape.second, 0xff).rule).find("no asymmetric"),
                  std::string::npos);
    }
    // ...and the in-process / forked rows keep the whole (symmetric) big set as before.
    EXPECT_EQ(SelectApplyThreadPolicy("auto", false, kConfigured, ApplyPeer::Forked, kShm, 0xff, 0xff, 0xff)
                  .requestedMask,
              0xffu);
}

TEST(ApplyThreadPolicy, AnEmptyOrMissingAffinityIsAuto) {
    for (const char* text : {static_cast<const char*>(nullptr), ""}) {
        EXPECT_EQ(Connected(text, kRedmiBig, kRedmiPrime, kRedmiOnline).requestedMask, 0x40u);
        EXPECT_EQ(SelectApplyThreadPolicy(text, false, kConfigured, ApplyPeer::Forked, kShm, kRedmiBig, kRedmiPrime,
                                          kRedmiOnline)
                      .requestedMask,
                  kRedmiBig);
        EXPECT_EQ(ReservedApplyCoreForConfig(text, kRedmiBig, kRedmiPrime, kRedmiOnline), 0x40u);
    }
}

TEST(ApplyThreadPolicy, AnExplicitSpinWinsForADialledInPeer) {
    const auto policy = SelectApplyThreadPolicy("auto", true, 7, ApplyPeer::Connected, kShm, kRedmiBig, kRedmiPrime,
                                                kRedmiOnline);
    EXPECT_EQ(policy.spinUs, 7u);
    EXPECT_EQ(policy.requestedMask, 0x40u);
}

TEST(ApplyThreadPolicy, AnExplicitMaskOrOffWinsForEveryPeerAndPlaneAndReservesNothing) {
    for (const ApplyPeer peer : {ApplyPeer::InProcess, ApplyPeer::Forked, ApplyPeer::Connected}) {
        for (const Bool plane : {kShm, kStream}) {
            const auto off = SelectApplyThreadPolicy("off", false, kConfigured, peer, plane, kRedmiBig, kRedmiPrime,
                                                     kRedmiOnline);
            EXPECT_TRUE(off.recognised);
            EXPECT_EQ(off.requestedMask, 0u) << ApplyPeerName(peer);
            EXPECT_EQ(off.spinUs, kConfigured) << "an operator's affinity keeps the configured spin";
            const auto mask = SelectApplyThreadPolicy("0x80", false, kConfigured, peer, plane, kRedmiBig,
                                                      kRedmiPrime, kRedmiOnline);
            EXPECT_TRUE(mask.recognised);
            EXPECT_EQ(mask.requestedMask, 0x80u) << ApplyPeerName(peer);
            EXPECT_EQ(SelectApplyThreadPolicy("12", false, kConfigured, peer, plane, kRedmiBig, kRedmiPrime,
                                              kRedmiOnline)
                          .requestedMask,
                      12u);
        }
    }
    // What the supervisor announces (and the helper avoids) under an operator's affinity: nothing.
    EXPECT_EQ(ReservedApplyCoreForConfig("0x80", kRedmiBig, kRedmiPrime, kRedmiOnline), 0u);
    EXPECT_EQ(ReservedApplyCoreForConfig("off", kRedmiBig, kRedmiPrime, kRedmiOnline), 0u);
}

TEST(ApplyThreadPolicy, UnrecognisedAffinityTextAppliesNothingAndSaysSo) {
    for (const char* text : {"big", "0xzz", "auto ", "3cores"}) {
        const auto policy = Connected(text, kRedmiBig, kRedmiPrime, kRedmiOnline);
        EXPECT_FALSE(policy.recognised) << text;
        EXPECT_EQ(policy.requestedMask, 0u) << text;
    }
}
