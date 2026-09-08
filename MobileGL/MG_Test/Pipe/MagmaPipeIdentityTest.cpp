// MobileGL - MobileGL/MG_Test/Pipe/MagmaPipeIdentityTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Magma's {slot, gen} mint (MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h) and the claim
// rule its per-slot memo tables use, at the one point HandleRecycleScenario cannot reach: a
// REAL SLOT REUSE.
//
// WHY THIS SUITE EXISTS (fix-aba review v1, MAJOR 1). The AbaControl integration lanes defeat
// the object identity that SELECTS the slot, and that is the whole of what a same-frame pixel
// reproducer can defeat:
//
//   * the mint has no death notification, so a slot returns to the free list only through
//     OnFrameBoundary's age sweep (kSweepInterval 256, kRetireAgeBoundaries 1024);
//   * HandleRecycleScenario issues five frame boundaries, so its replacement VAO gets a
//     BRAND-NEW slot at Gen 1 and the generation never participates in a compare;
//   * a real reuse needs >= 1024 idle boundaries, which puts the two draws in different frames
//     - and ResolvedVertexBindings, the only memo carrying a GPU slice rather than a layout,
//     declines across frames by design.
//
// So deleting the `++m_entries[index].Gen` in MagmaPipeIdentityTable::ClaimSlot leaves every
// arm of HandleRecycleScenario green. It reds AnIdleSlotIsRetiredAndReusedWithANewGeneration
// and AReusedSlotDoesNotServeItsPredecessorsMemo below, which is the whole point of the file.
//
// The suite lives beside SlotAllocatorTest because it asserts the same identity contract that
// file asserts for the client allocator - "Gen moves on REUSE and never on respecify" - for the
// second mint in the tree, the one Magma keeps because nothing in P2 can call the client
// allocator's Free (MagmaPipeArms.h says why). It needs no GL context, no driver and no Vulkan
// loader: MagmaPipeArms.h is header-only.
//
// Push-only, like everything it tests, so every case is a visible SKIP in a pull build rather
// than a vanishing test (G2 name parity).
#include <gtest/gtest.h>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#include <MG_Pipe/MGPipeHandles.h>

#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Backend/DirectVulkan/Renderer/MagmaPipeArms.h>
#endif

using namespace MobileGL;

namespace {
#if !MOBILEGL_PIPE_PUSH
    // The push build's case list, declared once so a case added on one side and forgotten on
    // the other shows up as a ctest-name diff rather than as a test that silently is not there
    // (the shape CsoCacheTest.cpp established).
#define MGL_MAGMA_PIPE_IDENTITY_TEST_LIST(X)                             \
    X(MagmaPipeIdentityTest, AnIdleSlotIsRetiredAndReusedWithANewGeneration)  \
    X(MagmaPipeIdentityTest, AReusedSlotDoesNotServeItsPredecessorsMemo)      \
    X(MagmaPipeIdentityTest, TheAbaControlKnobServesTheStaleMemoAcrossAReusedSlot) \
    X(MagmaPipeIdentityTest, ALiveObjectKeepsItsSlotItsGenerationAndItsMemo)

#define MGL_DECLARE_PULL_SKIP(Suite, Name)                                                         \
    TEST(Suite, Name) { GTEST_SKIP() << "compiled only under MOBILEGL_PIPE_PUSH"; }
    MGL_MAGMA_PIPE_IDENTITY_TEST_LIST(MGL_DECLARE_PULL_SKIP)
#undef MGL_DECLARE_PULL_SKIP
#else
    using namespace MobileGL::MG_Backend::DirectVulkan;
    using MG_Pipe::MGPipeHandle;

    // What VertexInputStateFactory::VaoBackendMemos is, reduced to the two fields the claim
    // rule needs: the Owner it compares, and one payload word standing in for the memo's
    // contents (there, the content hash and the resolved-entry pointer). The RULE is production
    // code - MagmaPipeClaimSlotMemos - not a copy of it.
    struct TestMemos {
        MGPipeHandle Owner = MG_Pipe::kMGPipeNullHandle;
        Uint64 Payload = 0;
    };

    // MagmaPipeIdentityTable's sweep cadence and retirement age are private, so the number of
    // boundaries needed to retire an object last used at boundary 0 is spelled out here: the
    // sweep runs when (boundary % 256) == 0 and retires entries idle for more than 1024
    // boundaries, so the first sweep that can retire it is boundary 1280. Every case that
    // depends on this ASSERTs the retire actually happened, so a change to either constant
    // fails loudly instead of silently turning these cases into "two unrelated objects".
    constexpr int kBoundariesToRetireAnObjectIdleSinceTheStart = 1280;

    class MagmaPipeIdentityTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_savedKnob = MG_Config::Features.PipeHandleAbaControl;
            MG_Config::Features.PipeHandleAbaControl = false;
        }
        void TearDown() override { MG_Config::Features.PipeHandleAbaControl = m_savedKnob; }

        // Ages the mint far enough for the sweep to retire everything that has been idle since
        // the start, while touching `keepAliveLifetimeId` on every boundary so that IT is never
        // retired. The keep-alive is what makes the reuse non-degenerate: it holds the first
        // allocatable slot, so the slot under test is not the one negative control C aliases
        // everything onto (kMagmaPipeAbaControlSlotIndex).
        static void AgeUntilTheSweepRetiresTheIdleSlots(MagmaPipeIdentityTable& mint,
                                                        Uint64 keepAliveLifetimeId) {
            for (int i = 0; i < kBoundariesToRetireAnObjectIdleSinceTheStart; ++i) {
                mint.Acquire(keepAliveLifetimeId);
                mint.OnFrameBoundary();
            }
        }

        Bool m_savedKnob = false;
    };

    // The precondition every case below rests on, asserted on its own so that a failure here
    // reads as "the mint stopped reusing slots" rather than as a memo bug.
    TEST_F(MagmaPipeIdentityTest, AnIdleSlotIsRetiredAndReusedWithANewGeneration) {
        MagmaPipeIdentityTable mint("VertexElementsCso");
        const MGPipeHandle keepAlive = mint.Acquire(1);
        const MGPipeHandle first = mint.Acquire(2);
        ASSERT_EQ(keepAlive.Slot, MG_Pipe::kMGPipeFirstAllocatableSlot);
        ASSERT_NE(first.Slot, keepAlive.Slot);
        ASSERT_EQ(first.Gen, 1u) << "a slot's first handout is generation 1";
        ASSERT_EQ(mint.LiveCount(), 2u);

        AgeUntilTheSweepRetiresTheIdleSlots(mint, 1);
        ASSERT_EQ(mint.LiveCount(), 1u)
            << "the idle slot was not retired, so nothing in this file is a slot REUSE";

        // The step HandleRecycleScenario cannot take. With an empty free list this would be a
        // brand-new slot at Gen 1 and the generation would never participate in any compare -
        // which is exactly what the scenario measures (redVao slot=2 gen=1, greenVao slot=3
        // gen=1) and why it cannot catch a deleted ++Gen.
        const MGPipeHandle second = mint.Acquire(3);
        EXPECT_EQ(second.Slot, first.Slot) << "a retired slot must come back before the high-water mark";
        EXPECT_EQ(second.Gen, first.Gen + 1u) << "a slot that changes owner must change generation";
        EXPECT_FALSE(second == first);
        EXPECT_EQ(mint.Count(), 2u) << "the reuse must not mint a third slot";
    }

    // THE CASE THE ++Gen IS LOAD-BEARING FOR. Knob off: the replacement gets the predecessor's
    // SLOT, so the slot cannot be what separates them - only the generation can.
    TEST_F(MagmaPipeIdentityTest, AReusedSlotDoesNotServeItsPredecessorsMemo) {
        MagmaPipeIdentityTable mint("VertexElementsCso");
        MagmaPipeSlotTable<TestMemos> memos;

        mint.Acquire(1); // the keep-alive, so the slot under test is not slot index 0
        const MGPipeHandle first = mint.Acquire(2);
        MagmaPipeClaimSlotMemos(memos, first).Payload = 0xDEADull;
        ASSERT_TRUE(MagmaPipeClaimSlotMemos(memos, first).Owner == first);
        ASSERT_EQ(MagmaPipeClaimSlotMemos(memos, first).Payload, 0xDEADull);

        AgeUntilTheSweepRetiresTheIdleSlots(mint, 1);
        const MGPipeHandle second = mint.Acquire(3);
        ASSERT_EQ(second.Slot, first.Slot) << "not a slot reuse, so this case would prove nothing";
        // EXPECT, not ASSERT: with the generation frozen the memo assertion below is exactly what
        // goes red, and a reader of the failure should see both halves rather than stop here.
        EXPECT_NE(second.Gen, first.Gen);

        const TestMemos& served = MagmaPipeClaimSlotMemos(memos, second);
        EXPECT_TRUE(served.Owner == second) << "the entry was not claimed for its new owner";
        EXPECT_EQ(served.Payload, 0ull)
            << "the replacement inherited the dead object's memo out of the SAME slot: the "
               "generation is the only thing that separates {slot, gen=N} from {slot, gen=N+1}, "
               "and it did not";
    }

    // The same shape with negative control C on, which is what makes the case above a control
    // rather than a tautology: with the knob on the memo IS served across the generation.
    //
    // The knob is set before the first claim, as a process-wide knob is in a real run: what it
    // defeats is the identity that selects the entry, so a run that stamps with it off and reads
    // with it on would be reading a different entry, not an aliased one.
    TEST_F(MagmaPipeIdentityTest, TheAbaControlKnobServesTheStaleMemoAcrossAReusedSlot) {
        MG_Config::Features.PipeHandleAbaControl = true;

        MagmaPipeIdentityTable mint("VertexElementsCso");
        MagmaPipeSlotTable<TestMemos> memos;

        mint.Acquire(1);
        const MGPipeHandle first = mint.Acquire(2);
        TestMemos& stamped = MagmaPipeClaimSlotMemos(memos, first);
        stamped.Payload = 0xDEADull;

        AgeUntilTheSweepRetiresTheIdleSlots(mint, 1);
        const MGPipeHandle second = mint.Acquire(3);
        ASSERT_EQ(second.Slot, first.Slot);
        EXPECT_NE(second.Gen, first.Gen);

        const TestMemos& served = MagmaPipeClaimSlotMemos(memos, second);
        EXPECT_EQ(&served, &stamped) << "the control must collapse every object onto one entry";
        EXPECT_EQ(served.Payload, 0xDEADull)
            << "negative control C is not defeating the claim rule any more: the replacement was "
               "NOT handed its predecessor's memo, so the AbaControl lanes assert nothing";
        EXPECT_TRUE(MG_Pipe::MGPipeHandleIsNull(served.Owner))
            << "the control hands the entry back UNCLEARED and UNCLAIMED - it never learns whose "
               "it is, which is what 'replace the identity with a constant' means";
    }

    // The other half of the {slot, gen} contract, and the reason a deleted ++Gen cannot be
    // 'fixed' by bumping Gen on every acquisition: a live object keeps its handle across
    // sweeps, so its memo survives a reconfiguration instead of being recomputed per draw.
    TEST_F(MagmaPipeIdentityTest, ALiveObjectKeepsItsSlotItsGenerationAndItsMemo) {
        MagmaPipeIdentityTable mint("VertexElementsCso");
        MagmaPipeSlotTable<TestMemos> memos;

        const MGPipeHandle handle = mint.Acquire(2);
        MagmaPipeClaimSlotMemos(memos, handle).Payload = 0xBEEFull;

        // Past two sweeps (256 and 512), drawn on every boundary.
        for (int i = 0; i < 700; ++i) {
            mint.Acquire(2);
            mint.OnFrameBoundary();
        }
        const MGPipeHandle again = mint.Acquire(2);
        EXPECT_TRUE(again == handle) << "a live object's handle moved under the age sweep";
        EXPECT_EQ(MagmaPipeClaimSlotMemos(memos, again).Payload, 0xBEEFull)
            << "a live object's memo was cleared without its slot changing owner";
        EXPECT_EQ(mint.Count(), 1u);
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace
