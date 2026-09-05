// MobileGL - MobileGL/MG_Test/Util/PipeStatsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The MGPipe boundary counters (plan B section 11 P0, corollary in section 2.3.1).
// No GL context and no driver: the module is arithmetic over a fixed set of counters,
// which is exactly what has to be pinned before anyone reads a number off a device.

#include <gtest/gtest.h>

#include <MG_Util/Metrics/PipeStats.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
    namespace PS = MobileGL::MG_Util::PipeStats;
    using MobileGL::String;
    using MobileGL::Uint32;
    using MobileGL::Uint64;

    class PipeStatsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            PS::ResetForTesting();
            PS::SetEnabledForTesting(true);
        }
        void TearDown() override {
            PS::SetEnabledForTesting(false);
            PS::ResetForTesting();
        }
    };

    // The off latch is the whole cost argument: every counting site in the two backends is
    // written as `if (Enabled()) ...`, so a false latch has to mean "nothing is counted".
    TEST_F(PipeStatsTest, EnabledLatchIsTheOnlyGate) {
        PS::SetEnabledForTesting(false);
        EXPECT_FALSE(PS::Enabled());
        PS::SetEnabledForTesting(true);
        EXPECT_TRUE(PS::Enabled());
    }

    TEST_F(PipeStatsTest, ByteClassesAccumulateIndependently) {
        PS::AddBytes(PS::ByteClass::StageBuffer, 100);
        PS::AddBytes(PS::ByteClass::StageBuffer, 40);
        PS::AddBytes(PS::ByteClass::StageTexture, 7);

        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageBuffer), 140u);
        EXPECT_EQ(PS::FrameBytes(PS::ByteClass::StageBuffer), 140u);
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageTexture), 7u);
        // Every other class untouched, the residual-value-block placeholder included.
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageUboGlobal), 0u);
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageUboNamed), 0u);
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::ResidualValueBlock), 0u);
    }

    // The frame accumulator is what feeds TracyPlot; the run total is what feeds the JSON
    // dump. A present must clear the first and keep the second.
    TEST_F(PipeStatsTest, PresentClearsTheFrameButKeepsTheTotal) {
        PS::AddBytes(PS::ByteClass::StageTexture, 512);
        PS::AddCalls(PS::CallClass::Draws, 3);
        PS::CountGate(PS::Gate::EsprytRenderState, /*hit=*/true);

        PS::OnPresent();

        EXPECT_EQ(PS::FrameBytes(PS::ByteClass::StageTexture), 0u);
        EXPECT_EQ(PS::FrameCalls(PS::CallClass::Draws), 0u);
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageTexture), 512u);
        EXPECT_EQ(PS::TotalCalls(PS::CallClass::Draws), 3u);
        EXPECT_EQ(PS::TotalGateHits(PS::Gate::EsprytRenderState), 1u);
        EXPECT_EQ(PS::FrameCount(), 1u);
    }

    TEST_F(PipeStatsTest, GateHitsAndMissesAreSeparateCounters) {
        for (Uint32 i = 0; i < 5; ++i) {
            PS::CountGate(PS::Gate::MagmaPipelineMemo, /*hit=*/true);
        }
        PS::CountGate(PS::Gate::MagmaPipelineMemo, /*hit=*/false);
        PS::CountGate(PS::Gate::MagmaDrawFastPath, /*hit=*/false);

        EXPECT_EQ(PS::TotalGateHits(PS::Gate::MagmaPipelineMemo), 5u);
        EXPECT_EQ(PS::TotalGateMisses(PS::Gate::MagmaPipelineMemo), 1u);
        EXPECT_EQ(PS::TotalGateHits(PS::Gate::MagmaDrawFastPath), 0u);
        EXPECT_EQ(PS::TotalGateMisses(PS::Gate::MagmaDrawFastPath), 1u);
    }

    // Bucket 0 is "no payload"; bucket n>0 is [2^(n-1), 2^n). The placeholder histogram is
    // the SEG_CMD sizing input (section 4.5.7), so its bucketing is pinned now rather than
    // when a generator first calls it.
    TEST_F(PipeStatsTest, PayloadHistogramBucketsByPowerOfTwo) {
        PS::RecordDrawPayloadBytes(0);
        PS::RecordDrawPayloadBytes(1);   // [1, 2)   -> bucket 1
        PS::RecordDrawPayloadBytes(2);   // [2, 4)   -> bucket 2
        PS::RecordDrawPayloadBytes(3);   // [2, 4)   -> bucket 2
        PS::RecordDrawPayloadBytes(48);  // [32, 64) -> bucket 6
        PS::RecordDrawPayloadBytes(64);  // [64, 128)-> bucket 7

        EXPECT_EQ(PS::TotalPayloadBucket(0), 1u);
        EXPECT_EQ(PS::TotalPayloadBucket(1), 1u);
        EXPECT_EQ(PS::TotalPayloadBucket(2), 2u);
        EXPECT_EQ(PS::TotalPayloadBucket(6), 1u);
        EXPECT_EQ(PS::TotalPayloadBucket(7), 1u);
    }

    // A record far larger than the last bucket must land in the last bucket, not past the
    // end of the array.
    TEST_F(PipeStatsTest, PayloadHistogramSaturatesInsteadOfOverflowing) {
        PS::RecordDrawPayloadBytes(~Uint64{0});
        EXPECT_EQ(PS::TotalPayloadBucket(PS::kPayloadHistogramBuckets - 1), 1u);
        EXPECT_EQ(PS::TotalPayloadBucket(PS::kPayloadHistogramBuckets), 0u);
    }

    // The summary line's shape is what an operator greps and what the smoke check in this
    // package matches, so it is pinned here rather than left to the log reader's memory.
    TEST_F(PipeStatsTest, SummaryLineCarriesEveryClassAndGate) {
        PS::AddCalls(PS::CallClass::Draws, 4);
        PS::AddCalls(PS::CallClass::AccessorCalls, 50);
        PS::AddBytes(PS::ByteClass::StageBuffer, 4096);
        PS::OnPresent();

        const String line = PS::FormatSummaryLine();
        EXPECT_NE(line.find("MGPipe stats:"), String::npos) << line;
        EXPECT_NE(line.find("draws=4"), String::npos) << line;
        // 50 accessor calls over 4 draws, two decimals, no <iomanip>.
        EXPECT_NE(line.find("acc/draw=12.50"), String::npos) << line;
        EXPECT_NE(line.find("buf=4096"), String::npos) << line;
        for (Uint32 i = 0; i < static_cast<Uint32>(PS::Gate::Count); ++i) {
            EXPECT_NE(line.find("="), String::npos);
        }
        EXPECT_NE(line.find("gates["), String::npos) << line;
        EXPECT_NE(line.find("tex[emit="), String::npos) << line;
    }

    // Successive summaries report WINDOWS, not run totals: a run total over a workload that
    // changes shape (load, then steady state) averages away the very number section 2.3.1
    // wants.
    TEST_F(PipeStatsTest, SummaryLinesReportDisjointWindows) {
        PS::AddCalls(PS::CallClass::Draws, 10);
        PS::OnPresent();
        const String first = PS::FormatSummaryLine();
        EXPECT_NE(first.find("draws=10"), String::npos) << first;

        PS::AddCalls(PS::CallClass::Draws, 3);
        PS::OnPresent();
        const String second = PS::FormatSummaryLine();
        EXPECT_NE(second.find("draws=3"), String::npos) << second;
        EXPECT_NE(second.find("frames=2"), String::npos) << second;
    }

    TEST_F(PipeStatsTest, SummaryLineSurvivesZeroDraws) {
        PS::OnPresent();
        const String line = PS::FormatSummaryLine();
        EXPECT_NE(line.find("acc/draw=0.00"), String::npos) << line;
    }

    TEST_F(PipeStatsTest, JsonDumpNamesEveryCounter) {
        PS::AddBytes(PS::ByteClass::StageUboNamed, 256);
        PS::CountGate(PS::Gate::MagmaDynamicTail, /*hit=*/false);
        PS::RecordDrawPayloadBytes(9);
        PS::OnPresent();

        const String json = PS::FormatJson();
        for (Uint32 i = 0; i < static_cast<Uint32>(PS::ByteClass::Count); ++i) {
            const String name = PS::NameOf(static_cast<PS::ByteClass>(i));
            EXPECT_NE(json.find("\"" + name + "\""), String::npos) << name << " missing from " << json;
        }
        for (Uint32 i = 0; i < static_cast<Uint32>(PS::CallClass::Count); ++i) {
            const String name = PS::NameOf(static_cast<PS::CallClass>(i));
            EXPECT_NE(json.find("\"" + name + "\""), String::npos) << name << " missing from " << json;
        }
        for (Uint32 i = 0; i < static_cast<Uint32>(PS::Gate::Count); ++i) {
            const String name = PS::NameOf(static_cast<PS::Gate>(i));
            EXPECT_NE(json.find("\"" + name + "\""), String::npos) << name << " missing from " << json;
        }
        EXPECT_NE(json.find("\"stage-ubo-named\": 256"), String::npos) << json;
        EXPECT_NE(json.find("\"frames\": 1"), String::npos) << json;
        EXPECT_NE(json.find("cmd-bytes-per-draw-histogram"), String::npos) << json;
    }

    // The counter names are the TracyPlot series names and the JSON keys; a rename is a
    // breaking change for every recorded baseline, so the whole set is pinned.
    TEST_F(PipeStatsTest, CounterNamesAreStable) {
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageBuffer), "stage-buffer");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageTexture), "stage-texture");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageUboGlobal), "stage-ubo-global");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageUboNamed), "stage-ubo-named");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageVertexClient), "stage-vertex-client");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageIndexClient), "stage-index-client");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::PersistentMapPush), "persistent-map-push");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::ResidualValueBlock), "residual-value-block");
        EXPECT_STREQ(PS::NameOf(PS::Gate::EsprytRenderState), "espryt-render-state");
        EXPECT_STREQ(PS::NameOf(PS::Gate::EsprytTextureSyncList), "espryt-texture-sync-list");
        EXPECT_STREQ(PS::NameOf(PS::Gate::EsprytUnitBindingsEpoch), "espryt-unit-bindings-epoch");
        EXPECT_STREQ(PS::NameOf(PS::Gate::MagmaDrawFastPath), "magma-draw-fastpath");
        EXPECT_STREQ(PS::NameOf(PS::Gate::MagmaPipelineMemo), "magma-pipeline-memo");
        EXPECT_STREQ(PS::NameOf(PS::Gate::MagmaDynamicTail), "magma-dynamic-tail");
    }

    // A summary is emitted every kSummaryFramePeriod presents. The period is a constant the
    // smoke check depends on, so a change to it has to break a test.
    TEST_F(PipeStatsTest, SummaryPeriodIsOneHundredAndTwentyFrames) {
        EXPECT_EQ(PS::kSummaryFramePeriod, 120u);
        for (Uint64 i = 0; i < PS::kSummaryFramePeriod; ++i) {
            PS::OnPresent();
        }
        EXPECT_EQ(PS::FrameCount(), PS::kSummaryFramePeriod);
    }
} // namespace
