// MobileGL - MobileGL/MG_Test/SelfTest/PrimitivesGeneratedNoXfbProbeTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The primitives-generated-without-transform-feedback probe's VERDICT and ARMING
// logic, pinned over synthetic measurements. The Vulkan plumbing needs a GPU; the
// two pure functions are where the cheap mistakes live - a verdict that reads a
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
//   - and the refuse-to-guess shapes (half counts, missing mandatory shapes).

#include <gtest/gtest.h>

#include <MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.h>

using MobileGL::Bool;
using MobileGL::Uint64;
using MobileGL::MG_Config::QuirkOverride;
using MobileGL::MG_Util::SelfTest::EvaluatePrimitivesGeneratedNoXfbVerdict;
using MobileGL::MG_Util::SelfTest::ChoosePrimitivesGeneratedReroute;
using MobileGL::MG_Util::SelfTest::PrimGenRerouteKind;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbMeasurement;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbShapeMeasurement;
using MobileGL::MG_Util::SelfTest::PrimitivesGeneratedNoXfbVerdict;

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
