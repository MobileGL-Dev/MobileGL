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
#if MOBILEGL_BUILD_DISAGGREGATED
#include <MG_Backend/DirectVulkan/Renderer/MagmaProgramSource.h>
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
    X(MagmaPipeIdentityTest, ALiveObjectKeepsItsSlotItsGenerationAndItsMemo)             \
    X(MagmaPipeIdentityTest, AMagmaServerNeverPublishesTheRunAheadCapBit)

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

    // Application buffer record consumers do not imply run-ahead readiness.
    // Magma retains its lockstep lifetime/submission contract; the capability
    // must remain absent even when the Espryt readiness constant is true.
    TEST_F(MagmaPipeIdentityTest, AMagmaServerNeverPublishesTheRunAheadCapBit) {
        // Whatever the integration constant says. `true` is what the P5e integration commit
        // will pass, so the Magma answer is pinned on BOTH sides of that flip and the case
        // does not quietly stop asserting anything the day the constant moves.
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectVulkan, /*ready=*/false) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  0u);
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectVulkan, /*ready=*/true) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  0u);

        // And the Espryt half, so the case says what the arm IS and not only what it is not:
        // the bit is published for DirectGLES and ONLY once the integration commit flips
        // kMGPipeP5eRunAheadReady. Until then every P5e package lands with the wait rule inert.
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/false) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  0u);
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/true) &
                      static_cast<Uint64>(MG_Pipe::kCapRunAheadApply),
                  static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));

        // Bit 10 and nothing else: the arm contributes ONE bit, so a future editor cannot fold
        // an unrelated capability into it and have the two pins above still pass.
        EXPECT_EQ(MG_Pipe::MGPipeRunAheadCapBitsFor(BackendType::DirectGLES, /*ready=*/true),
                  static_cast<Uint64>(MG_Pipe::kCapRunAheadApply));
        static_assert(static_cast<Uint64>(MG_Pipe::kCapRunAheadApply) == (1ull << 10),
                      "kCapRunAheadApply moved bit; CONTRACT-P5E table 0 names bit 10");
    }
#endif // MOBILEGL_PIPE_PUSH

    TEST(MagmaProgramSourceTest, ServerBindingTailsReplaceLinkTimeDefaults) {
#if MOBILEGL_BUILD_DISAGGREGATED
        auto archive = MakeShared<MG_State::GLState::ProgramArchive>();
        archive->Link.glBlockIndexToTProgram = {0};
        archive->Link.blockReflection.resize(1);
        archive->Link.blockReflection[0].name = "Block";
        archive->Link.blockReflection[0].size = 12;
        archive->Link.uniformBlockBinding = {9};
        archive->Link.uniformSamplerOrImageUnitIndex = {12, 13};
        archive->Spirv.globalUboScratch = {99, 98};
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = archive;
        record.BlockBindings = {3};
        record.SamplerUnits = {{0, 4}};
        record.GlobalConstants = {5, 6};
        record.GlobalConstantsVersion = 11;
        const MagmaProgramSource source({7, 2}, record);
        EXPECT_TRUE(source.IsWire());
        EXPECT_EQ(source.Frontend(), nullptr);
        EXPECT_EQ(source.GetUniformBlockBinding(0), 3u);
        EXPECT_EQ(source.GetUniformBlockName(0), "Block");
        EXPECT_EQ(source.GetUBOSizeAt(0), 16u);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(0), 4);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(1), -1)
            << "an absent tail entry must not resurrect the archive's stale unit";
        ASSERT_EQ(source.GetUBOSize(), 2u);
        EXPECT_EQ(static_cast<const Uint8*>(source.GetUBOData())[0], 5u);
        EXPECT_EQ(source.GetUBOContentVersion(), 11u);
        record.BlockBindings[0] = 6;
        record.SamplerUnits[0].Unit = 8;
        record.GlobalConstants[0] = 42;
        record.GlobalConstantsVersion = 12;
        EXPECT_EQ(source.GetUniformBlockBinding(0), 6u);
        EXPECT_EQ(source.GetUniformSamplerOrImageUnitIndex(0), 8);
        EXPECT_EQ(static_cast<const Uint8*>(source.GetUBOData())[0], 42u);
        EXPECT_EQ(source.GetUBOContentVersion(), 12u);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }

    TEST(MagmaProgramSourceTest, ArrayUniformNamesResolveWithinTheArchivedUniform) {
#if MOBILEGL_BUILD_DISAGGREGATED
        auto archive = MakeShared<MG_State::GLState::ProgramArchive>();
        auto& link = archive->Link;
        link.maxUniformLocation = 2;
        link.uniformLocations["arr[0]"] = 0;
        link.uniformIndexInTProgram = {0, 0, 0};
        link.tProgramUniformIndexToGl = {0};
        link.uniformReflection.resize(1);
        link.uniformReflection[0].type.isArray = true;
        link.uniformReflection[0].arraySize = 3;
        link.uniformReflection[0].glDefineType = GL_SAMPLER_2D;
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = archive;
        const MagmaProgramSource source({8, 1}, record);
        EXPECT_EQ(source.GetUniformLocation("arr"), 0);
        EXPECT_EQ(source.GetUniformLocation("arr[2]"), 2);
        EXPECT_EQ(source.GetUniformLocation("arr[3]"), -1);
        EXPECT_EQ(source.GetUniformLocation("arr[-1]"), -1);
        EXPECT_EQ(source.GetUniformLocation("arr[999999999999999]"), -1);
        EXPECT_EQ(source.GetUniformLocation("unknown"), -1);
        EXPECT_TRUE(source.UniformLocationsAliasSameUniform(0, 2));
        EXPECT_FALSE(source.UniformLocationsAliasSameUniform(0, 3));
        EXPECT_EQ(source.GetUniformType(2), GL_SAMPLER_2D);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }

    TEST(MagmaProgramSourceTest, ReusedHandlesAndRecordVersionsHaveDistinctCacheIdentity) {
#if MOBILEGL_BUILD_DISAGGREGATED
        MG_Pipe::MGPipeShaderCsoRecord record{};
        record.Archive = MakeShared<MG_State::GLState::ProgramArchive>();
        record.Serial = 17;
        record.BindingsSerial = 21;
        record.GlobalConstantsVersion = 25;
        const MagmaProgramSource first({9, 2}, record);
        const MagmaProgramSource recycled({9, 3}, record);
        EXPECT_NE(first.GetLifetimeId(), recycled.GetLifetimeId());
        EXPECT_EQ(first.Handle(), (MG_Pipe::MGPipeHandle{9, 2}));
        const auto identity = first.GetLifetimeId();
        record.Serial = 18;
        record.BindingsSerial = 22;
        record.GlobalConstantsVersion = 26;
        EXPECT_EQ(first.GetLifetimeId(), identity) << "content changes do not mint object identities";
        EXPECT_EQ(first.GetBackendStateVersion(), 18u);
        EXPECT_EQ(first.GetBlockBindingVersion(), 22u);
        EXPECT_EQ(first.GetImageUnitVersion(), 22u);
        EXPECT_EQ(first.GetUBOContentVersion(), 26u);
#else
        GTEST_SKIP() << "server program sources require the disaggregated build";
#endif
    }
} // namespace
