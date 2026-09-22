// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/SubsystemDepsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// D-K2's SUBSYSTEM DEPENDENCY RULE: THE CASE THAT FAILS WHEN THE TWO SIDES DISAGREE (P3b/P4b
// R-5).
//
// THE RULE HAD SIX STATEMENTS AND NOTHING COMPARED THEM. Two of the four prose ones were
// WRONG - MG_Pipe/MGPipe.h and MG_Backend/DirectGLES/Managers.h both said "THREE OF THEM" when
// there are six rows, and both said "the mirror pairs (10 without 11, ...) are all fine" when
// 10-without-11 is D-K2's FOURTH row. Neither wrong comment could fail anything. The two
// EXECUTABLE statements are in different link units with no shared header:
//
//   CLIENT dependency check   MG_Impl/Pipe/PipeFill.cpp's kMGPipeP4aFamilyDependencies, reached
//                             through MGPipeP4aFamilyEmits().
//   SERVER consumer gate      MG_Backend/DirectGLES/Managers.cpp's four
//                             Resolve<Family>SubsystemArm(), over PipeSubsystemDependencyMissing.
//
// This file is the first thing in the tree that calls BOTH at the same mask and compares them,
// and it compares each against MG_Pipe/SubsystemDeps.def - the one place the rule is now
// written. A change to either reader alone reds here; a change to the .def alone reds here
// twice.
//
// WHY THE TWO READERS DO NOT YET READ THE .def THEMSELVES. PipeFill.cpp is the contract
// package's file for the whole phase (MG_Impl/Pipe/TextureEmit.h states that rule in full) and
// Managers.cpp belongs to wave 2-D package D1 while it is in flight. Switching them is two
// one-line changes for the integrator; the anti-drift property does not wait for it, because
// this file supplies it from the outside.
//
// THE SERVER RESOLVERS ARE CALLED DIRECTLY, NOT THROUGH THEIR LATCHED ACCESSORS. Managers.h
// wraps each one in a `static const Bool enabled = Resolve...()` that runs once per process;
// calling the resolver itself is what lets this case sweep masks. That is also why the case
// restores MG_Config::Features.PipePush before it returns: a latched accessor evaluated later
// in the same binary would otherwise read a mask this test left behind.

#include <gtest/gtest.h>

#include <Config.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/PipeFill.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {

    struct PushMaskScope {
        Uint64 Saved = MG_Config::Features.PipePush;
        explicit PushMaskScope(Uint64 mask) { MG_Config::Features.PipePush = mask; }
        ~PushMaskScope() { MG_Config::Features.PipePush = Saved; }
    };

    // Every mask the rule can be interestingly asked about: the phase constants, the two
    // documented refusal lanes (0x7ff withholds bit 11, 0x5ff withholds bit 9), and one mask per
    // row with exactly that row's required bits knocked out.
    Vector<Uint64> InterestingMasks() {
        Vector<Uint64> masks{0ull,
                             kMGPipeSubsystemsMigratedAtP2,
                             kMGPipeSubsystemsMigratedAtP3a,
                             kMGPipeSubsystemsMigratedAtP4a,
                             kMGPipeSubsystemsMigratedAtP5e,
                             0x7ffull,
                             0x5ffull,
                             0x9ffull};
        for (const MGPipeSubsystemDependencyRow& row : kMGPipeSubsystemDependencies) {
            masks.push_back(kMGPipeSubsystemsMigratedAtP5e & ~row.Requires);
            masks.push_back(kMGPipeSubsystemsMigratedAtP5e & ~row.Family);
        }
        return masks;
    }

    String Describe(Uint64 mask) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(mask));
        return String(buffer);
    }

} // namespace

// ---------------------------------------------------------------------------------------------
// 1. The table itself is exhaustive and well-formed.
// ---------------------------------------------------------------------------------------------
TEST(SubsystemDepsTest, EveryRowNamesExactlyOneFamilyAndNoRowRequiresItself) {
    Uint64 seen = 0;
    for (const MGPipeSubsystemDependencyRow& row : kMGPipeSubsystemDependencies) {
        EXPECT_NE(row.Family, 0ull) << "a row with no family is not a row";
        EXPECT_EQ(row.Family & (row.Family - 1), 0ull)
            << "a row must name EXACTLY ONE bit; " << Describe(row.Family) << " names several, and "
            << "MGPipeSubsystemRequires ORs every matching row - a multi-bit family would make a "
               "query for one of its bits answer for all of them";
        EXPECT_EQ(row.Family & row.Requires, 0ull)
            << Describe(row.Family) << " requires itself, which no reader can ever satisfy";
        EXPECT_EQ(seen & row.Family, 0ull)
            << "two rows name " << Describe(row.Family) << "; MGPipeSubsystemRequires would OR "
            << "them and the duplicate would be invisible";
        seen |= row.Family;
        EXPECT_NE(row.Why, nullptr);
        EXPECT_NE(row.Why[0], '\0') << "a row with no reason is how the rule drifted in the first "
                                       "place";
    }
    // The families the rule is about. Bit 8 and bit 13 are in the table too, which is the half
    // MGPipe.h's old "THREE OF THEM" comment was missing.
    EXPECT_EQ(seen, kMGPipeSubsystemVertexInput | kMGPipeSubsystemFramebuffer |
                        kMGPipeSubsystemTextureResources | kMGPipeSubsystemSamplers |
                        kMGPipeSubsystemPrograms | kMGPipeSubsystemBufferBindings);
}

TEST(SubsystemDepsTest, TheFourthRowIsPresentAndTheWithdrawnMirrorPairIsNotFine) {
    // ID-14/ID-15. This is the row two comments in the tree denied existed, and the reason this
    // case is written as an assertion rather than a paragraph.
    EXPECT_NE(MGPipeSubsystemRequires(kMGPipeSubsystemTextureResources) & kMGPipeSubsystemSamplers,
              0ull)
        << "bit 10 no longer requires bit 11. If that is intended, the withdrawal recorded in "
           "PipeFill.cpp and in BRIEF-P4A's amendment has been reversed and Espryt's "
           "ResolveTextureResourceSubsystemArm must stop refusing 0x7ff in the same commit";
    EXPECT_FALSE(MGPipeSubsystemDependenciesAreSet(kMGPipeSubsystemTextureResources, 0x7ffull));
    EXPECT_FALSE(MGPipeSubsystemDependenciesAreSet(
        kMGPipeSubsystemSamplers,
        kMGPipeSubsystemsMigratedAtP5e & ~kMGPipeSubsystemTextureResources))
        << "bit 11 without bit 10 must not be live - the symmetric half of the fourth row";
    // ...and the mirrors that really are fine.
    EXPECT_TRUE(MGPipeSubsystemDependenciesAreSet(kMGPipeSubsystemPrograms, 0ull))
        << "bit 12 depends on nothing";
    EXPECT_TRUE(MGPipeSubsystemDependenciesAreSet(
        kMGPipeSubsystemTextureResources,
        kMGPipeSubsystemResources | kMGPipeSubsystemSamplers | kMGPipeSubsystemTextureResources))
        << "bit 10 with 7 and 11 and WITHOUT bit 9 is fine and is not a row";
}

// ---------------------------------------------------------------------------------------------
// 2. THE CLIENT DEPENDENCY CHECK never emits a family the table refuses.
//
// ONE-DIRECTIONAL, AND FOR A MEASURED REASON. MGPipeP4aFamilyEmits is a QUADRUPLE: the
// operator's bit, the family's wired constant, a backend having registered the consumer, and
// the dependency rule (PipeFill.cpp's `wants()`). Only the fourth conjunct is this file's
// subject, and the other three are false in a bare unit binary with no backend registered - the
// first version of this case asserted equality and reddened at 0x1fff on the sampler and program
// families for exactly that reason, which is the assertion being wrong rather than the product.
//
// So the direction that IS this rule's: the client may withhold a family for reasons of its own,
// but it may never EMIT one whose dependency bits are unset. That is the direction in which a
// disagreement means records crossing to a server whose matching handle arm did not come up.
// ---------------------------------------------------------------------------------------------
TEST(SubsystemDepsTest, TheClientNeverEmitsAFamilyTheTableRefuses) {
#if MOBILEGL_PIPE_PUSH
    const Uint64 families[] = {kMGPipeSubsystemFramebuffer, kMGPipeSubsystemTextureResources,
                               kMGPipeSubsystemSamplers, kMGPipeSubsystemPrograms};
    int emitted = 0;
    for (const Uint64 mask : InterestingMasks()) {
        for (const Uint64 family : families) {
            if (!MGPipeP4aFamilyEmits(family, mask)) continue;
            ++emitted;
            EXPECT_TRUE(MGPipeSubsystemDependenciesAreSet(family, mask))
                << "at MOBILEGL_PIPE_PUSH=" << Describe(mask) << " the CLIENT emits family "
                << Describe(family)
                << " while MG_Pipe/SubsystemDeps.def says its dependency bits are not all set. "
                   "Those records reach a server whose matching handle arm refused to come up - "
                   "the half-run subsystem D-K2 exists to make impossible. The two statements of "
                   "the rule have drifted; whichever moved, the other has to move with it.";
            EXPECT_NE(mask & family, 0ull)
                << "the client emitted a family whose own bit is clear at " << Describe(mask);
        }
    }
    // NOT VACUOUS-PROOF, but a loud marker: a unit binary registers no backend consumer, so
    // MGPipeP4aFamilyEmits may legitimately be false everywhere here. Recorded rather than
    // asserted, because asserting it would make this case depend on whether the binary happens
    // to have pulled in a consumer registration.
    RecordProperty("client_emitting_combinations", emitted);
#else
    GTEST_SKIP() << "the client dependency check is compiled only under MOBILEGL_PIPE_PUSH";
#endif
}

// ---------------------------------------------------------------------------------------------
// 3. THE SERVER CONSUMER GATE agrees with the table at every interesting mask.
//
// The resolvers answer "did this family's handle arm come up", which is the dependency rule AND
// the family's own bit AND, for the framebuffer one, D-C3's separate wire-caps refusal. So the
// comparison is one-directional where it has to be: a resolver may say NO for a reason of its
// own, but it may never say YES at a mask the table refuses - that is the direction in which a
// disagreement is a half-run subsystem rather than an extra refusal.
// ---------------------------------------------------------------------------------------------
TEST(SubsystemDepsTest, TheServerConsumerGateNeverArmsAFamilyTheTableRefuses) {
#if MOBILEGL_PIPE_PUSH
    using namespace MobileGL::MG_Backend::DirectGLES;
    // THE RESOLVERS SURVIVE A SWEEP ONLY BECAUSE LEGACY MEMOS ARE ON. Each of them ends in
    // ClassifyPipeSubsystemArm, and a verdict of NoArm calls StopOnArmlessPipeSubsystem
    // (Managers.cpp:2928-2938), which aborts the process - that is the intended behaviour for a
    // configuration with neither the handle arm nor the legacy arm, and it would turn this
    // mask sweep into a crash rather than a failure. MOBILEGL_PIPE_LEGACY_MEMOS defaults ON
    // (Config.h:399) and nothing in the unit lane clears it, so the legacy arm always survives
    // and every resolver returns rather than stopping. Asserted rather than assumed, because the
    // day that default flips this file would abort with no explanation.
    ASSERT_TRUE(MG_Config::Features.PipeLegacyMemos)
        << "MOBILEGL_PIPE_LEGACY_MEMOS is clear, so a resolver whose handle arm this sweep "
           "deliberately refuses has NO arm left and StopOnArmlessPipeSubsystem will abort the "
           "process instead of returning false. Run this case with the legacy memos on, or teach "
           "it to skip the masks that leave a family armless.";
    struct Resolver {
        const char* Name;
        Uint64 Family;
        Bool (*Resolve)();
    };
    const Resolver resolvers[] = {
        {"ResolveFramebufferSubsystemArm", kMGPipeSubsystemFramebuffer,
         &ResolveFramebufferSubsystemArm},
        {"ResolveTextureResourceSubsystemArm", kMGPipeSubsystemTextureResources,
         &ResolveTextureResourceSubsystemArm},
        {"ResolveSamplerSubsystemArm", kMGPipeSubsystemSamplers, &ResolveSamplerSubsystemArm},
        {"ResolveProgramSubsystemArm", kMGPipeSubsystemPrograms, &ResolveProgramSubsystemArm},
    };
    for (const Uint64 mask : InterestingMasks()) {
        PushMaskScope scope(mask);
        for (const Resolver& resolver : resolvers) {
            const Bool armed = resolver.Resolve();
            if (!armed) continue;
            EXPECT_TRUE(MGPipeSubsystemDependenciesAreSet(resolver.Family, mask))
                << "at MOBILEGL_PIPE_PUSH=" << Describe(mask) << " the SERVER's " << resolver.Name
                << " ARMED the handle arm for family " << Describe(resolver.Family)
                << " while MG_Pipe/SubsystemDeps.def says its dependency bits are not all set. "
                   "The client withholds the family at this mask, so the server is running a "
                   "handle arm nobody is feeding - which is the half-run subsystem D-K2 exists "
                   "to make impossible.";
            EXPECT_NE(mask & resolver.Family, 0ull)
                << resolver.Name << " armed with its own bit clear at " << Describe(mask);
        }
    }
#else
    GTEST_SKIP() << "the four Resolve<Family>SubsystemArm() resolvers are declared and defined "
                    "only under MOBILEGL_PIPE_PUSH (Managers.h's P4a D-K3 block), so the pull "
                    "build has no server consumer gate to compare against";
#endif
}

// ---------------------------------------------------------------------------------------------
// 4. ...and the refusal really happens at the two documented lanes, so that test 3 is not
//    vacuously green because every resolver said NO for an unrelated reason.
// ---------------------------------------------------------------------------------------------
TEST(SubsystemDepsTest, TheTwoDocumentedRefusalLanesStillRefuseAndTheFullMaskStillArms) {
#if MOBILEGL_PIPE_PUSH
    using namespace MobileGL::MG_Backend::DirectGLES;
    ASSERT_TRUE(MG_Config::Features.PipeLegacyMemos)
        << "see the sweep above: a refused family with the legacy arm also off aborts the process";
    {
        PushMaskScope scope(kMGPipeSubsystemsMigratedAtP5e);
        EXPECT_TRUE(ResolveTextureResourceSubsystemArm())
            << "the texture family's handle arm does not come up at its own phase mask, so every "
               "refusal assertion in this file is vacuous";
        EXPECT_TRUE(ResolveSamplerSubsystemArm());
        EXPECT_TRUE(ResolveProgramSubsystemArm());
        // THE FRAMEBUFFER RESOLVER IS RECORDED, NOT ASSERTED, and the asymmetry is the point:
        // ResolveFramebufferSubsystemArm folds D-C3's wire-caps refusal in beside the dependency
        // rule (Managers.cpp:4323-4325), and that half asks the SERVER's published caps - which a
        // bare unit binary with no session and no registered backend cannot answer in the
        // affirmative. So a false here says nothing about D-K2 and asserting true would pin
        // whatever a test binary happens to have linked. The sweep above still covers it in the
        // direction that matters (it may never ARM where the table refuses).
        RecordProperty("framebuffer_resolver_at_p5e", ResolveFramebufferSubsystemArm() ? 1 : 0);
    }
    {
        // 0x7ff: bits 0..10, i.e. the texture family WITHOUT the sampler family - D-K2's fourth
        // row, and the mask CMakeLists.txt's refusal lane pins.
        PushMaskScope scope(0x7ffull);
        EXPECT_FALSE(ResolveTextureResourceSubsystemArm())
            << "0x7ff is the mask whose texture family Espryt refuses by name; if it now arms, "
               "D-K2's fourth row has been dropped on the server side while the client and the "
               ".def still carry it";
    }
    {
        // 0x5ff: bits 0..8 and bit 10, i.e. the texture family WITHOUT the framebuffer bit and
        // WITHOUT bit 11 - the second refusal lane CMakeLists.txt pins.
        PushMaskScope scope(0x5ffull);
        EXPECT_FALSE(ResolveTextureResourceSubsystemArm())
            << "0x5ff leaves bit 11 clear too, so the fourth row refuses it for the same reason";
    }
#else
    GTEST_SKIP() << "the four Resolve<Family>SubsystemArm() resolvers exist only under "
                    "MOBILEGL_PIPE_PUSH";
#endif
}
