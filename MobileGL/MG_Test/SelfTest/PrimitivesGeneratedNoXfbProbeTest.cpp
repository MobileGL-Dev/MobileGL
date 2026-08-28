// MobileGL - MobileGL/MG_Test/SelfTest/PrimitivesGeneratedNoXfbProbeTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The primitives-generated-without-transform-feedback probe's VERDICT and ARMING
// logic, pinned over synthetic measurements. Recording the probe for real needs a
// GPU; the two pure functions are where the cheap mistakes live - a verdict that reads a
// half-broken driver as healthy, an override arm swapped so ForceOn disarms, a
// substitute ranked below a worse one - and every driver the campaign has
// characterised is written down here as a fake measurement so the mapping cannot
// drift without a red:
//   - a conforming driver (stream counts everywhere),
//   - Mesa lavapipe as measured 2026-08: stream silent everywhere, the dedicated
//     VK_EXT_primitives_generated_query exact everywhere (discard included), and
//     the statistics control exact on the plain shape but dead under rasterizer
//     discard (llvmpipe's discard short-circuit),
//   - the same driver without the dedicated query - the statistics tiers,
//   - a device with the defect and no working substitute,
//   - a substitute that would be WORSE than the stream query on some shape (the
//     never-worse rule the plain-only arm has to prove before it may arm),
//   - and the refuse-to-guess shapes (half counts, missing mandatory shapes).
//
// The last section pins the probe's TEARDOWN CONTRACT instead, driving the real
// RunPrimitivesGeneratedNoXfbProbe against a fake Vulkan driver whose fence wait
// can be made to expire: no GPU is needed for that, only the entry points the
// probe is handed, and what it does on that path is what keeps a hung driver from
// hanging the POST.

#include <gtest/gtest.h>

#include <cstdint>

#include <MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.h>

using MobileGL::Bool;
using MobileGL::Uint32;
using MobileGL::Uint64;
using MobileGL::MG_Config::QuirkOverride;
using MobileGL::MG_Util::SelfTest::EvaluatePrimitivesGeneratedNoXfbVerdict;
using MobileGL::MG_Util::SelfTest::ChoosePrimitivesGeneratedReroute;
using MobileGL::MG_Util::SelfTest::PrimGenRerouteKind;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbMeasurement;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbProbeContext;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbShapeMeasurement;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbVerdict;
using MobileGL::MG_Util::SelfTest::RunPrimitivesGeneratedNoXfbProbe;

namespace {
    struct ShapeAnswers {
        Uint64 stream = 0;
        // Negative-free encoding: measured flags separate from values.
        Bool pgqMeasured = false;
        Uint64 pgq = 0;
        Bool statMeasured = false;
        Uint64 stat = 0;
    };

    PrimitivesGeneratedNoXfbShapeMeasurement Shape(const ShapeAnswers& answers) {
        PrimitivesGeneratedNoXfbShapeMeasurement shape;
        shape.drawn = true;
        shape.expectedPrimitives = 1;
        shape.streamGenerated = answers.stream;
        shape.primitivesGeneratedExtMeasured = answers.pgqMeasured;
        shape.primitivesGeneratedExt = answers.pgq;
        shape.statisticsMeasured = answers.statMeasured;
        shape.statisticsClippingInput = answers.stat;
        return shape;
    }

    PrimitivesGeneratedNoXfbMeasurement Measurement(PrimitivesGeneratedNoXfbShapeMeasurement plain,
                                                    PrimitivesGeneratedNoXfbShapeMeasurement discard,
                                                    PrimitivesGeneratedNoXfbShapeMeasurement patches) {
        PrimitivesGeneratedNoXfbMeasurement measurement;
        measurement.ran = true;
        measurement.trianglesPlain = plain;
        measurement.trianglesDiscard = discard;
        measurement.patchesDiscard = patches;
        return measurement;
    }

    PrimitivesGeneratedNoXfbShapeMeasurement NotDrawn() {
        return PrimitivesGeneratedNoXfbShapeMeasurement{};
    }

    constexpr ShapeAnswers kHealthy{1, true, 1, true, 1};
    // The lavapipe measurement: stream silent, dedicated query exact, statistics
    // exact only where nothing is discarded.
    constexpr ShapeAnswers kLavapipePlain{0, true, 1, true, 1};
    constexpr ShapeAnswers kLavapipeDiscard{0, true, 1, true, 0};
} // namespace

// A conforming driver: the stream query counts every capture-less shape exactly.
// Controls agreeing changes nothing - health is decided by the subject.
TEST(PrimitivesGeneratedNoXfbVerdictTest, AConformingDriverReadsStreamCounts) {
    const auto measurement = Measurement(Shape(kHealthy), Shape(kHealthy), Shape(kHealthy));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::StreamCounts);
}

// ...and stays healthy with no tessellation stage to draw the patches shape with,
// and with no control at all - a control is only required to QUALIFY a
// substitute, never to certify health.
TEST(PrimitivesGeneratedNoXfbVerdictTest, HealthNeedsNeitherTessellationNorAControl) {
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape(kHealthy), Shape(kHealthy), NotDrawn())),
              PrimitivesGeneratedNoXfbVerdict::StreamCounts);
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({1}), Shape({1}), Shape({1}))),
              PrimitivesGeneratedNoXfbVerdict::StreamCounts);
}

// Mesa lavapipe as measured (2026-08): stream silent for every capture-less
// draw, the dedicated primitives-generated query exact on every shape (discard
// included), the statistics control dead under discard. The dedicated query must
// win - it is the only substitute that covers the CTS shape there.
TEST(PrimitivesGeneratedNoXfbVerdictTest, LavapipeShapedMeasurementTakesTheDedicatedQuery) {
    const auto measurement =
        Measurement(Shape(kLavapipePlain), Shape(kLavapipeDiscard), Shape(kLavapipeDiscard));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute);
}

// The affected-device hypothesis with no dedicated query: statistics exact on
// every shape, the CTS's discarded shapes included.
TEST(PrimitivesGeneratedNoXfbVerdictTest, StatisticsExactEverywhereIsTheFullStatisticsSubstitute) {
    const auto measurement = Measurement(Shape({0, false, 0, true, 1}), Shape({0, false, 0, true, 1}),
                                         Shape({0, false, 0, true, 1}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute);
}

// A dedicated query that is silent in the same way the stream query is must not
// be armed - the statistics tier decides instead.
TEST(PrimitivesGeneratedNoXfbVerdictTest, ASilentDedicatedQueryFallsThroughToStatistics) {
    const auto measurement = Measurement(Shape({0, true, 0, true, 1}), Shape({0, true, 0, true, 1}),
                                         Shape({0, true, 0, true, 1}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute);
}

// The llvmpipe statistics hole without the dedicated query to rescue it: exact on
// the plain shape, dead under discard. Repairs undiscarded queries only, and the
// verdict must say so.
TEST(PrimitivesGeneratedNoXfbVerdictTest, StatisticsDeadUnderDiscardIsThePlainOnlySubstitute) {
    const auto measurement = Measurement(Shape({0, false, 0, true, 1}), Shape({0, false, 0, true, 0}),
                                         Shape({0, false, 0, true, 0}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly);
}

// THE DOMINATION RULE. The plain-only substitute is armed for EVERY XFB-inactive
// draw, so it may only be armed where it is never worse than what it replaces:
// each shape it gets wrong must be one the stream query already answered 0 for.
// Here the discarded triangle is one the stream query answers EXACTLY (a driver
// whose silence is selective) and whose statistics read 0 - rerouting would turn
// that correct 1 into a 0, so the honest verdict is that nothing may be armed.
TEST(PrimitivesGeneratedNoXfbVerdictTest, ASubstituteWorseThanTheStreamOnAnyShapeIsRefused) {
    const auto measurement = Measurement(Shape({1, false, 0, true, 1}), Shape({1, false, 0, true, 0}),
                                         Shape({0, false, 0, true, 0}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::Unfixable);
    // The same shape with the statistics slot MISSING on the stream-exact shape is
    // the same trade: an unmeasured control cannot be assumed to answer.
    const auto unmeasured = Measurement(Shape({1, false, 0, true, 1}), Shape({1, false, 0, false, 0}),
                                        Shape({0, false, 0, true, 0}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(unmeasured),
              PrimitivesGeneratedNoXfbVerdict::Unfixable);
    // ...while the same selective silence WITH a substitute that covers the shapes
    // it must still qualifies: every shape the statistics miss read 0 anyway.
    const auto dominating = Measurement(Shape({1, false, 0, true, 1}), Shape({0, false, 0, true, 1}),
                                        Shape({0, false, 0, true, 0}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(dominating),
              PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly);
}

// The defect with no substitute: no control, controls silent, or a control that
// OVERCOUNTS the plain shape (as disqualifying as one that reads 0 - an exact
// match is what qualifies a substitute).
TEST(PrimitivesGeneratedNoXfbVerdictTest, StreamSilentWithoutAWorkingPlainControlIsUnfixable) {
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({0}), Shape({0}), Shape({0}))),
              PrimitivesGeneratedNoXfbVerdict::Unfixable);
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({0, true, 0, true, 0}), Shape({0, true, 0, true, 0}),
                              Shape({0, true, 0, true, 0}))),
              PrimitivesGeneratedNoXfbVerdict::Unfixable);
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({0, true, 2, true, 2}), Shape({0, true, 1, true, 1}),
                              Shape({0, true, 1, true, 1}))),
              PrimitivesGeneratedNoXfbVerdict::Unfixable);
}

// Refuse-to-guess shapes. A nonzero-but-wrong stream answer fits neither the
// defect (exact silence) nor health (the exact count), whichever shape carries
// it; and a probe that never ran, or lost its mandatory shapes, says nothing.
TEST(PrimitivesGeneratedNoXfbVerdictTest, AnswersFittingNeitherHealthNorTheDefectAreInconclusive) {
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({2, true, 1, true, 1}), Shape({0, true, 1, true, 1}),
                              Shape({0, true, 1, true, 1}))),
              PrimitivesGeneratedNoXfbVerdict::Inconclusive);
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(
                  Measurement(Shape({0, true, 1, true, 1}), Shape({3, true, 1, true, 1}),
                              Shape({0, true, 1, true, 1}))),
              PrimitivesGeneratedNoXfbVerdict::Inconclusive);

    PrimitivesGeneratedNoXfbMeasurement neverRan;
    neverRan.ran = false;
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(neverRan),
              PrimitivesGeneratedNoXfbVerdict::Inconclusive);

    const auto missingMandatoryShape = Measurement(Shape({0, true, 1, true, 1}), NotDrawn(), NotDrawn());
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(missingMandatoryShape),
              PrimitivesGeneratedNoXfbVerdict::Inconclusive);
}

// A partial silence is still the defect: the plain shape counts but the discarded
// ones read 0 (a driver that gates the stream counter on rasterization rather
// than on the capture). With a whole control the substitute is whole.
TEST(PrimitivesGeneratedNoXfbVerdictTest, SilenceOnOnlyTheDiscardShapesIsStillTheDefect) {
    const auto measurement = Measurement(Shape({1, true, 1, true, 1}), Shape({0, true, 1, true, 1}),
                                         Shape({0, true, 1, true, 1}));
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute);
}

// ===================== THE OVERRIDE MAPPING =====================
//
// The one-line swap this exists to catch: ForceOn and ForceOff exchanging arms,
// Auto arming on a verdict that never qualified a substitute, or the pool ranking
// inverting. Every cell of the (override x verdict) table is written out.

namespace {
    constexpr PrimitivesGeneratedNoXfbVerdict kAllVerdicts[] = {
        PrimitivesGeneratedNoXfbVerdict::Inconclusive,
        PrimitivesGeneratedNoXfbVerdict::StreamCounts,
        PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute,
        PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute,
        PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly,
        PrimitivesGeneratedNoXfbVerdict::Unfixable,
    };
}

TEST(PrimitivesGeneratedNoXfbArmingTest, ForceOffNeverReroutes) {
    for (const auto verdict : kAllVerdicts) {
        for (const Bool pgqUsable : {false, true}) {
            for (const Bool statsUsable : {false, true}) {
                EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::ForceOff, verdict, pgqUsable,
                                                           statsUsable),
                          PrimGenRerouteKind::None);
            }
        }
    }
}

TEST(PrimitivesGeneratedNoXfbArmingTest, ForceOnBypassesTheVerdictButNeverTheStructuralChecks) {
    for (const auto verdict : kAllVerdicts) {
        // The dedicated query wins where the device can host it...
        EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::ForceOn, verdict, true, true),
                  PrimGenRerouteKind::PrimitivesGeneratedExt);
        EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::ForceOn, verdict, true, false),
                  PrimGenRerouteKind::PrimitivesGeneratedExt);
        // ...statistics stand in where only they exist...
        EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::ForceOn, verdict, false, true),
                  PrimGenRerouteKind::ClippingStatistics);
        // ...and no pool means no reroute, forced or not.
        EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::ForceOn, verdict, false, false),
                  PrimGenRerouteKind::None);
    }
}

TEST(PrimitivesGeneratedNoXfbArmingTest, AutoFollowsExactlyTheSubstituteVerdicts) {
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute,
                  true, true),
              PrimGenRerouteKind::PrimitivesGeneratedExt);
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute, false, true),
              PrimGenRerouteKind::ClippingStatistics);
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitutePlainOnly,
                  false, true),
              PrimGenRerouteKind::ClippingStatistics);
    // The statistics verdicts never take the dedicated pool: that verdict only
    // exists when the dedicated query did NOT qualify.
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute, true, true),
              PrimGenRerouteKind::ClippingStatistics);
    for (const auto verdict :
         {PrimitivesGeneratedNoXfbVerdict::Inconclusive, PrimitivesGeneratedNoXfbVerdict::StreamCounts,
          PrimitivesGeneratedNoXfbVerdict::Unfixable}) {
        EXPECT_EQ(ChoosePrimitivesGeneratedReroute(QuirkOverride::Auto, verdict, true, true),
                  PrimGenRerouteKind::None);
    }
    // The structural checks bind Auto too.
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::PrimitivesGeneratedExtSubstitute,
                  false, true),
              PrimGenRerouteKind::None);
    EXPECT_EQ(ChoosePrimitivesGeneratedReroute(
                  QuirkOverride::Auto, PrimitivesGeneratedNoXfbVerdict::StatisticsSubstitute, false, false),
              PrimGenRerouteKind::None);
}

// ===================== THE FENCE-TIMEOUT CONTRACT =====================
//
// A driver whose queue never signals the probe's fence inside 5 s is the one case
// where the probe must NOT clean up: the submission may still be executing, so
// vkDeviceWaitIdle can block forever and destroying in-flight objects is
// undefined. It therefore leaks everything it made and says so in the measurement
// (`fenceWaitTimedOut`), which is what lets its callers make the same choice for
// the object THEY own - the driver POST leaks its throwaway VkDevice instead of
// destroying it under live children (vkDestroyDevice would be the very hang the
// bound exists to prevent), and the renderer, whose device is the real one, must
// not idle-wait it either. Neither guard is reachable from a unit test - the POST
// probe lives in an anonymous namespace and the renderer needs a GPU - so this
// pins the contract they both key on, at the boundary where it is produced.
//
// The fake driver below is the whole Vulkan surface the probe touches, with a
// dialable fence-wait result and per-entry-point call counters.

namespace {
    struct FakeDriverState {
        VkResult fenceWaitResult = VK_SUCCESS;
        Uint32 objectsCreated = 0;
        Uint32 destroyCalls = 0;
        Uint32 deviceWaitIdleCalls = 0;
        Uint32 queueSubmitCalls = 0;
        Uint64 streamGenerated = 1;
    };
    FakeDriverState g_fake;

    template <typename Handle>
    Handle FakeHandle() {
        ++g_fake.objectsCreated;
        // One cast form for both handle flavours: a pointer on 64-bit builds, a
        // uint64_t on 32-bit ones. The probe only ever compares against
        // VK_NULL_HANDLE, so any distinct nonzero value will do.
        return (Handle)(std::uintptr_t)(0x1000u + g_fake.objectsCreated * 0x10u);
    }

    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
                                                         const VkAllocationCallbacks*, VkCommandPool* out) {
        *out = FakeHandle<VkCommandPool>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*,
                                                              VkCommandBuffer* out) {
        *out = FakeHandle<VkCommandBuffer>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*,
                                                       const VkAllocationCallbacks*, VkQueryPool* out) {
        *out = FakeHandle<VkQueryPool>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR void VKAPI_CALL FakeCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, VkQueryControlFlags) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdBeginQueryIndexedEXT(VkCommandBuffer, VkQueryPool, uint32_t,
                                                           VkQueryControlFlags, uint32_t) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdEndQueryIndexedEXT(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*,
                                                        const VkAllocationCallbacks*, VkRenderPass* out) {
        *out = FakeHandle<VkRenderPass>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*,
                                                         const VkAllocationCallbacks*, VkFramebuffer* out) {
        *out = FakeHandle<VkFramebuffer>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR void VKAPI_CALL FakeCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*,
                                                      VkSubpassContents) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdEndRenderPass(VkCommandBuffer) {}
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*,
                                                          const VkAllocationCallbacks*, VkShaderModule* out) {
        *out = FakeHandle<VkShaderModule>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*,
                                                            const VkAllocationCallbacks*,
                                                            VkPipelineLayout* out) {
        *out = FakeHandle<VkPipelineLayout>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyPipelineLayout(VkDevice, VkPipelineLayout,
                                                         const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t count,
                                                               const VkGraphicsPipelineCreateInfo*,
                                                               const VkAllocationCallbacks*, VkPipeline* out) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = FakeHandle<VkPipeline>();
        }
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR void VKAPI_CALL FakeCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
    VKAPI_ATTR void VKAPI_CALL FakeCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {}
    VKAPI_ATTR VkResult VKAPI_CALL FakeCreateFence(VkDevice, const VkFenceCreateInfo*,
                                                   const VkAllocationCallbacks*, VkFence* out) {
        *out = FakeHandle<VkFence>();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL FakeDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {
        ++g_fake.destroyCalls;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
        ++g_fake.queueSubmitCalls;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) {
        return g_fake.fenceWaitResult;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t,
                                                           size_t dataSize, void* data, VkDeviceSize,
                                                           VkQueryResultFlags) {
        // The stream pool's {primitivesWritten, primitivesNeeded} pair; the probe
        // reads primitivesNeeded, and this fake device counts capture-less draws.
        if (data == nullptr || dataSize < 2 * sizeof(Uint64)) {
            return VK_INCOMPLETE;
        }
        auto* pair = static_cast<Uint64*>(data);
        pair[0] = 0;
        pair[1] = g_fake.streamGenerated;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL FakeDeviceWaitIdle(VkDevice) {
        ++g_fake.deviceWaitIdleCalls;
        return VK_SUCCESS;
    }

    PrimitivesGeneratedNoXfbProbeContext FakeProbeContext() {
        g_fake = FakeDriverState{};
        PrimitivesGeneratedNoXfbProbeContext context;
        context.device = (VkDevice)(std::uintptr_t)0xD0D0;
        context.queue = (VkQueue)(std::uintptr_t)0xC0C0;
        context.transformFeedbackQueriesUsable = true;
        // No controls and no tessellation: this fixture is about the teardown
        // contract, and the fewer optional slots the fewer moving parts.
        auto& fns = context.fns;
        fns.vkCreateCommandPool = FakeCreateCommandPool;
        fns.vkDestroyCommandPool = FakeDestroyCommandPool;
        fns.vkAllocateCommandBuffers = FakeAllocateCommandBuffers;
        fns.vkBeginCommandBuffer = FakeBeginCommandBuffer;
        fns.vkEndCommandBuffer = FakeEndCommandBuffer;
        fns.vkCreateQueryPool = FakeCreateQueryPool;
        fns.vkDestroyQueryPool = FakeDestroyQueryPool;
        fns.vkCmdResetQueryPool = FakeCmdResetQueryPool;
        fns.vkCmdBeginQuery = FakeCmdBeginQuery;
        fns.vkCmdEndQuery = FakeCmdEndQuery;
        fns.vkCmdBeginQueryIndexedEXT = FakeCmdBeginQueryIndexedEXT;
        fns.vkCmdEndQueryIndexedEXT = FakeCmdEndQueryIndexedEXT;
        fns.vkCreateRenderPass = FakeCreateRenderPass;
        fns.vkDestroyRenderPass = FakeDestroyRenderPass;
        fns.vkCreateFramebuffer = FakeCreateFramebuffer;
        fns.vkDestroyFramebuffer = FakeDestroyFramebuffer;
        fns.vkCmdBeginRenderPass = FakeCmdBeginRenderPass;
        fns.vkCmdEndRenderPass = FakeCmdEndRenderPass;
        fns.vkCreateShaderModule = FakeCreateShaderModule;
        fns.vkDestroyShaderModule = FakeDestroyShaderModule;
        fns.vkCreatePipelineLayout = FakeCreatePipelineLayout;
        fns.vkDestroyPipelineLayout = FakeDestroyPipelineLayout;
        fns.vkCreateGraphicsPipelines = FakeCreateGraphicsPipelines;
        fns.vkDestroyPipeline = FakeDestroyPipeline;
        fns.vkCmdBindPipeline = FakeCmdBindPipeline;
        fns.vkCmdDraw = FakeCmdDraw;
        fns.vkCreateFence = FakeCreateFence;
        fns.vkDestroyFence = FakeDestroyFence;
        fns.vkQueueSubmit = FakeQueueSubmit;
        fns.vkWaitForFences = FakeWaitForFences;
        fns.vkGetQueryPoolResults = FakeGetQueryPoolResults;
        fns.vkDeviceWaitIdle = FakeDeviceWaitIdle;
        return context;
    }
} // namespace

// The hung driver. Nothing the probe created may be destroyed, the device may not
// be idle-waited, and the measurement must SAY the wait timed out - a caller that
// owns the device reads that flag to leak it too, and `ran == false` alone cannot
// tell this apart from an ordinary setup failure (where teardown already ran and
// destroying the device is correct).
TEST(PrimitivesGeneratedNoXfbProbeTeardownTest, AFenceTimeoutLeaksEverythingAndReportsItself) {
    PrimitivesGeneratedNoXfbProbeContext context = FakeProbeContext();
    g_fake.fenceWaitResult = VK_TIMEOUT;

    const PrimitivesGeneratedNoXfbMeasurement measurement = RunPrimitivesGeneratedNoXfbProbe(context);

    EXPECT_FALSE(measurement.ran);
    EXPECT_TRUE(measurement.fenceWaitTimedOut)
        << "without this flag the POST destroys its throwaway VkDevice while the probe's children "
           "are alive and its submission may still be executing";
    EXPECT_GT(g_fake.queueSubmitCalls, 0u) << "the timeout must be the SUBMITTED probe's, not a setup failure";
    EXPECT_EQ(g_fake.destroyCalls, 0u)
        << "a probe that timed out must destroy nothing: the submission may still be executing";
    EXPECT_EQ(g_fake.deviceWaitIdleCalls, 0u)
        << "vkDeviceWaitIdle on a queue that missed a 5 s deadline is the hang the bound exists to "
           "prevent";
    // The verdict must not read a timed-out probe as anything but "no verdict".
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::Inconclusive);
}

// The control: a driver that signals normally gets the ordinary teardown - idle
// wait, every object destroyed, no timeout flag - so the case above is testing the
// timeout branch and not a probe that never cleans up at all.
TEST(PrimitivesGeneratedNoXfbProbeTeardownTest, ASignalledFenceTearsDownNormally) {
    PrimitivesGeneratedNoXfbProbeContext context = FakeProbeContext();
    g_fake.fenceWaitResult = VK_SUCCESS;
    g_fake.streamGenerated = 1; // healthy: the capture-less draws are counted

    const PrimitivesGeneratedNoXfbMeasurement measurement = RunPrimitivesGeneratedNoXfbProbe(context);

    EXPECT_TRUE(measurement.ran) << measurement.failureReason;
    EXPECT_FALSE(measurement.fenceWaitTimedOut);
    EXPECT_EQ(g_fake.deviceWaitIdleCalls, 1u);
    EXPECT_GT(g_fake.destroyCalls, 0u);
    EXPECT_EQ(EvaluatePrimitivesGeneratedNoXfbVerdict(measurement),
              PrimitivesGeneratedNoXfbVerdict::StreamCounts);
}
