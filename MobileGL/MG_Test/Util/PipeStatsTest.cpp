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

#include <Config.h>
#include <MG_Util/Debug/Log.h>
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
        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageIndirectCmd), 0u);
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

    TEST_F(PipeStatsTest, InitLatchesTheSummaryPeriodFromTheConfigAndNeverKeepsZero) {
        // The device retrace harness never reaches the teardown dump, so the summary
        // cadence is the only way a short fixture yields numbers at all: it must follow
        // MOBILEGL_PIPE_STATS_PERIOD, and a zero must fall back rather than divide.
        const Uint32 saved = MobileGL::MG_Config::Features.PipeStatsPeriod;
        MobileGL::MG_Config::Features.PipeStatsPeriod = 7;
        PS::Init();
        EXPECT_EQ(PS::SummaryFramePeriod(), 7u);
        MobileGL::MG_Config::Features.PipeStatsPeriod = 0;
        PS::Init();
        EXPECT_EQ(PS::SummaryFramePeriod(), PS::kDefaultSummaryFramePeriod);
        MobileGL::MG_Config::Features.PipeStatsPeriod = saved;
        PS::Init();
    }

    // THE LATCH IS THE WHOLE CONTRACT, and this case exists because a spawn server got
    // it wrong in a way no other test could see. `Init()` is a step of
    // MobileGL::Initialize(), and the spawn server process never calls that - it calls
    // MG_ConfigLoader::Init + InitServerRoleForSpawn - so `g_pipeStatsEnabled` stayed
    // false there and every `if (Enabled())` site in the process was permanently
    // false. The observable consequence was not a wrong number but an ABSENT one:
    // the server printed no summary line at all, and `wait[srv=0 srvpark=0]` (what a
    // reader would have taken for "the server never waited") is in fact what this
    // case's "false" arm looks like.
    //
    // It pins both directions, because only the pair is falsifiable: Init() must TAKE
    // the config's value when it is true, and must CLEAR the latch when it is false -
    // a version that only ever set the flag true would pass a one-sided test while
    // making MOBILEGL_PIPE_STATS=0 mean nothing in a long-lived process.
    TEST_F(PipeStatsTest, InitIsTheOnlyLatchAndItTakesTheConfigBothWays) {
        const bool saved = MobileGL::MG_Config::Features.PipeStats;

        MobileGL::MG_Config::Features.PipeStats = true;
        PS::Init();
        EXPECT_TRUE(PS::Enabled()) << "a true config must arm the counters";

        MobileGL::MG_Config::Features.PipeStats = false;
        PS::Init();
        EXPECT_FALSE(PS::Enabled()) << "and a false one must disarm them, not leave the old latch";

        MobileGL::MG_Config::Features.PipeStats = saved;
        PS::Init();
    }

    // A summary line is emitted by whichever role reaches OnPresent, and this is what makes
    // the spawn server's line appear at all: it has no swap of its own, so its cadence
    // rides the present records it applies. The assertion is the arithmetic that cadence
    // rests on - k presentations produce k windows when the period is 1 - because that is
    // the property the lane's per-frame rates divide by.
    TEST_F(PipeStatsTest, PresentationsAtTheDefaultPeriodOfOneMakeOneWindowEach) {
        const Uint32 saved = MobileGL::MG_Config::Features.PipeStatsPeriod;
        MobileGL::MG_Config::Features.PipeStatsPeriod = 1;
        PS::Init();

        for (Uint32 i = 0; i < 5; ++i) {
#if MOBILEGL_PIPE_PUSH
            PS::PublishGauge(PS::Gauge::ServerWaits, static_cast<Uint64>(100 * (i + 1)));
#endif
            PS::OnPresent();
        }
        EXPECT_EQ(PS::FrameCount(), 5u);
#if MOBILEGL_PIPE_PUSH
        // The gauge is a RUN TOTAL: publishing five increasing values leaves the last one,
        // never their sum. This is the mistake the report's first aggregation script made.
        // Gauges are push-only (gate G1, see PipeStats.h); the pull flavour keeps the case
        // registered for name parity and asserts only the window arithmetic above.
        EXPECT_EQ(PS::GaugeValue(PS::Gauge::ServerWaits), 500u);
#endif

        MobileGL::MG_Config::Features.PipeStatsPeriod = saved;
        PS::Init();
    }

    // P6 gate 8: THE DUMP PATH IS ROLE-DERIVED AND THE BASE NAME IS NOT A FILE.
    //
    // The trap this exists for: under `spawn` both roles reach PipeStats::Shutdown() (the client
    // through MobileGL::Destroy, the server through ServerMain's own call), and before this both
    // opened MOBILEGL_PIPE_STATS_FILE itself with trunc - so the second writer silently replaced
    // the first, and a reader held one role's numbers under a name that claimed to be the run's.
    //
    // ---- WHAT THIS CASE ASSERTS, AND WHAT ITS FIRST VERSION GOT WRONG ----
    //
    // The first version asserted about MG_Util::Debug::RoleLogPath directly: that the two roles get
    // different paths, that neither equals the base name. All of that PASSED WITH THE FIX REVERTED -
    // it tested the naming helper, which was never broken. What was broken was that the dump did
    // not CALL it. A test that stays green while the defect is present is worse than no test.
    //
    // So the subject here is RoleDerivedJsonDumpPathForTesting(), the same expression the dump and
    // the banner use; and the third assertion is the negative control the design rests on - the
    // base name must not exist as a file, so a reader that was not updated gets ENOENT and says so
    // rather than a file that looks like a complete run and is half of one.
    TEST_F(PipeStatsTest, JsonDumpPathIsRoleDerivedAndTheBaseNameIsNeverOpened) {
#if !MOBILEGL_BUILD_DISAGGREGATED
        // RoleDerivedJsonDumpPathForTesting is compiled only with two roles (gate G1: a monolith
        // build has one and needs no derivation). The NAME stays registered in every build so
        // ctest -N matches between flavours; the body only exists where the collision does.
        GTEST_SKIP() << "role-derived dump paths only exist with two roles (MOBILEGL_BUILD_DISAGGREGATED=OFF)";
#else
        using MobileGL::MG_Util::Debug::LogRole;

        const String saved = MobileGL::MG_Config::Features.PipeStatsFile;
        const char* base = "/tmp/mobilegl-pipestats-role-test.json";

        // Unset: no path at all, and specifically not the empty string spelled as a file name.
        MobileGL::MG_Config::Features.PipeStatsFile = "";
        EXPECT_TRUE(PS::RoleDerivedJsonDumpPathForTesting().empty());

        MobileGL::MG_Config::Features.PipeStatsFile = base;
        const String path = PS::RoleDerivedJsonDumpPathForTesting();

        // (1) THE DUMP PATH IS THE DERIVED ONE, spelled out, so a change to the rule is a change
        // to this test. A unit process is the client role.
        EXPECT_EQ(path, String("/tmp/mobilegl-pipestats-role-test.client.json"));

        // (2) THE BASE NAME IS NOT THE PATH. This is the collision itself: if the dump wrote the
        // configured name, spawn's second writer would truncate the first.
        EXPECT_NE(path, String(base));

        // (3) THE NEGATIVE CONTROL. PipeStats writes the derived path, never the base, so nothing
        // may have created the base. A fresh base is absent, and this fails if that ever stops
        // being true - which is exactly what a revert of the derivation does.
        std::remove(base);
        std::ifstream baseFile(base);
        EXPECT_FALSE(baseFile.good())
            << "the configured base name exists; something opened it directly, and under spawn "
               "that is the trunc-versus-trunc collision this derivation exists to prevent";

        MobileGL::MG_Config::Features.PipeStatsFile = saved;
#endif
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

        const String line = PS::FormatWindowLine();
        EXPECT_NE(line.find("MGPipe stats:"), String::npos) << line;
        EXPECT_NE(line.find("draws=4"), String::npos) << line;
        // 50 accessor calls over 4 draws, two decimals, no <iomanip>.
        EXPECT_NE(line.find("acc/draw=12.50"), String::npos) << line;
        EXPECT_NE(line.find("buf=4096.00"), String::npos) << line;
        for (Uint32 i = 0; i < static_cast<Uint32>(PS::Gate::Count); ++i) {
            EXPECT_NE(line.find("="), String::npos);
        }
        EXPECT_NE(line.find("gates["), String::npos) << line;
        EXPECT_NE(line.find("tex[emit="), String::npos) << line;
#if MOBILEGL_PIPE_PUSH
        // P2's two render-state CSO counters ride the same line, short-named. Push-only:
        // the pull build has no CSO to mint and must stay symbol-identical.
        EXPECT_NE(line.find("cso[csom="), String::npos) << line;
        EXPECT_NE(line.find("csob="), String::npos) << line;
        // P3a's persistent-map acquisition attempts ride the same bracket. It is the counter
        // the storage-regrow gate reads, so its short name is pinned where an operator's
        // grep would break.
        EXPECT_NE(line.find("mpr="), String::npos) << line;
        // P4a's emission bracket, and its short names are pinned for exactly the same reason:
        // fbe/sve/sse/sie are the four suppressors' hit rates and ctu is the client half of
        // the upload-shape comparison, so a rename breaks every recorded reading of them.
        EXPECT_NE(line.find("emit[fbe="), String::npos) << line;
        EXPECT_NE(line.find("sve="), String::npos) << line;
        EXPECT_NE(line.find("sse="), String::npos) << line;
        EXPECT_NE(line.find("sie="), String::npos) << line;
        EXPECT_NE(line.find("ctu="), String::npos) << line;
        // And the new ByteClass rides the ordinary bytes[] bracket under a short name that is
        // NOT "csob": the cso[] bracket above already prints csob= for the CSO bind count.
        EXPECT_NE(line.find("csob-blob="), String::npos) << line;
        // P6 GATE 8'S TWO NUMBERS (CONTRACT-P6.md §9 item 8), pinned where an operator's grep
        // would break. `seg` is the ByteClass that says how many bytes a frame stages into
        // SEG_STAGE, and `wrec` / `wrec/f` are the post-chunking record count and its per-frame
        // figure. The short name is `seg` and not `stage`, which would read as a prefix of
        // `stage-buffer`'s long name and of nothing else in the bracket; it is also not `ssb`,
        // because this is a per-frame BYTES field and `ssb` reads like a count.
        EXPECT_NE(line.find("seg="), String::npos) << line;
        EXPECT_NE(line.find("wrec="), String::npos) << line;
        EXPECT_NE(line.find("wrec/f="), String::npos) << line;
        // P5d round 3's wait ledger. Four fields on a bracket of their own, and they are
        // pinned here for the reason every other short name is: they are what the inproc
        // performance work reads out of a run's log, so a rename or a dropped field breaks
        // every recorded measurement of the split's handoff.
        EXPECT_NE(line.find("wait[srv="), String::npos) << line;
        EXPECT_NE(line.find("srvpark="), String::npos) << line;
        EXPECT_NE(line.find("cli="), String::npos) << line;
        EXPECT_NE(line.find("clipark="), String::npos) << line;
#endif
    }

    // Per-frame fields carry two decimals for the same reason acc/draw does: they are small
    // and load-bearing (bytes/f sizes SEG_STAGE), and integer division silently rounds a
    // whole unit off each of them. 26 draws over 14 frames is 1.86, not 1.
    TEST_F(PipeStatsTest, PerFrameFieldsKeepTwoDecimals) {
        PS::AddCalls(PS::CallClass::Draws, 26);
        PS::AddBytes(PS::ByteClass::StageBuffer, 1360);
        for (Uint32 i = 0; i < 14; ++i) {
            PS::OnPresent();
        }

        const String line = PS::FormatWindowLine();
        EXPECT_NE(line.find("draws/f=1.86"), String::npos) << line;
        EXPECT_NE(line.find("buf=97.14"), String::npos) << line;
    }

#if MOBILEGL_PIPE_PUSH
    // P6 GATE 8's TWO NUMBERS (CONTRACT-P6.md §9 item 8), and each half is pinned because each
    // half is a way the deliverable can be wrong without looking wrong.
    //
    // `seg` IS THE PRODUCER'S OWN BYTES. The wire encoder adds `size` and not the allocator's
    // Align8 of it (PipeWireCodec.cpp's StageAllocate), so a blob of 4096 stages 4096. Checked
    // here on the module's own arithmetic because that is the only place the rule can be read
    // without a live ring: the encoder's call site is deliberately one line and has no test of
    // its own.
    TEST_F(PipeStatsTest, StageSegmentBytesIsAWindowedByteClassLikeAnyOther) {
        PS::AddBytes(PS::ByteClass::StageSegmentBytes, 4096);
        PS::AddBytes(PS::ByteClass::StageSegmentBytes, 2048);
        PS::AddCalls(PS::CallClass::Draws, 1);
        PS::OnPresent();

        EXPECT_EQ(PS::TotalBytes(PS::ByteClass::StageSegmentBytes), 6144u);
        EXPECT_EQ(PS::FrameBytes(PS::ByteClass::StageSegmentBytes), 0u) << "a Present clears the frame";
        // 6144 over one frame, two decimals, in the ordinary bytes[] bracket.
        EXPECT_NE(PS::FormatWindowLine().find("seg=6144.00"), String::npos)
            << PS::FormatWindowLine();

        PS::AdvanceSummaryWindow();
        // And it obeys the file's LABEL rule, not a rule of its own: the new window has no
        // Present in it, so the bracket is relabelled "bytes[" and `seg` carries the window
        // TOTAL verbatim rather than a figure divided by a faked frame count. That is the same
        // rule the bytes[] bracket follows and the same one the two window-total tests below
        // pin; asserting "seg=0.00" here is what the first cut of this case did, and it was
        // asserting a per-frame form the line deliberately does not print.
        const String empty = PS::FormatWindowLine();
        EXPECT_NE(empty.find("bytes["), String::npos) << empty;
        EXPECT_EQ(empty.find("bytes/f["), String::npos) << empty;
        EXPECT_NE(empty.find("seg=0"), String::npos) << empty;
    }

    // `wrec` IS A COUNT, SO IT DOES NOT DIVIDE LIKE A BYTE CLASS - and the per-frame form must
    // read off the line, which is why the field exists at all rather than the operator doing the
    // division by hand. 26 records over 13 frames is 2.00; the window total is printed beside it
    // so the pair is self-checking.
    TEST_F(PipeStatsTest, WireRecordsCarryBothTheWindowCountAndItsPerFrameForm) {
        PS::AddCalls(PS::CallClass::WireRecords, 26);
        for (Uint32 i = 0; i < 13; ++i) {
            PS::OnPresent();
        }

        const String line = PS::FormatWindowLine();
        EXPECT_NE(line.find("wrec=26"), String::npos) << line;
        EXPECT_NE(line.find("wrec/f=2.00"), String::npos) << line;
    }

    // AND THE LABEL RULE THE FILE ALREADY OWNS APPLIES TO IT: a window with no Present in it has
    // no per-frame reading, so `wrec/f` carries the window total rather than a figure divided by
    // a faked 1. This is the same 47x overstatement PipeStatsTest pins for the bytes[] bracket,
    // one field over, and it is the shape a trace slice with no swap at all would hit.
    TEST_F(PipeStatsTest, WireRecordsFallBackToTheWindowTotalWithoutAFrame) {
        PS::AddCalls(PS::CallClass::WireRecords, 47);

        const String line = PS::FormatWindowLine();
        EXPECT_EQ(PS::FrameCount(), 0u);
        EXPECT_NE(line.find("wrec=47"), String::npos) << line;
        EXPECT_NE(line.find("wrec/f=47"), String::npos) << line;
        EXPECT_EQ(line.find("wrec/f=47.00"), String::npos) << line;
    }

    // THE STAGED-BLOB DISTRIBUTION, which is the optional deliverable MEASUREMENTS.md:342 says
    // has never existed. It shares the draw-payload histogram's edges - bucket 0 is "0 bytes",
    // bucket n>0 is [2^(n-1), 2^n) - so the two are read the same way, and it is a COUNT per
    // bucket rather than a byte sum: the shape is the deliverable and the bytes have their own
    // counter in `seg`.
    TEST_F(PipeStatsTest, StagedBlobHistogramSharesThePayloadBucketEdges) {
        PS::RecordStagedBlobBytes(1);        // [1, 2)     -> bucket 1
        PS::RecordStagedBlobBytes(4096);     // [4096,8192)-> bucket 13
        PS::RecordStagedBlobBytes(4096);     // same bucket
        PS::RecordStagedBlobBytes(8192);     // [8192,...) -> bucket 14: the edge is exclusive

        EXPECT_EQ(PS::TotalStagedBlobBucket(1), 1u);
        EXPECT_EQ(PS::TotalStagedBlobBucket(13), 2u);
        EXPECT_EQ(PS::TotalStagedBlobBucket(14), 1u);
        EXPECT_EQ(PS::TotalStagedBlobBucket(0), 0u);
        // Saturation and the out-of-range read, exactly as the draw-payload histogram's.
        PS::RecordStagedBlobBytes(~Uint64{0});
        EXPECT_EQ(PS::TotalStagedBlobBucket(PS::kStagedBlobHistogramBuckets - 1), 1u);
        EXPECT_EQ(PS::TotalStagedBlobBucket(PS::kStagedBlobHistogramBuckets), 0u);
    }

    // A distribution is a RUN TOTAL and has no business being windowed: it is a shape over the
    // whole workload, and a Present that cleared it would report the shape of one frame as if it
    // were the run's. It reaches an operator through the JSON dump - the same channel and for the
    // same reason the draw-payload histogram uses it - so that is where it is asserted.
    TEST_F(PipeStatsTest, StagedBlobHistogramIsARunTotalAndReachesTheJsonDump) {
        PS::RecordStagedBlobBytes(1024);
        PS::OnPresent();

        const String json = PS::FormatJson();
        EXPECT_NE(json.find("\"staged-blob-bytes-histogram\""), String::npos) << json;
        EXPECT_NE(json.find("\"wire-records\""), String::npos) << json;
        EXPECT_NE(json.find("\"stage-segment-bytes\""), String::npos) << json;
        // Run total, not a window: the sample above survives the Present.
        EXPECT_EQ(PS::TotalStagedBlobBucket(11), 1u);

        // The ordering is the JSON document's shape, not decoration: the new array is a SIBLING
        // of the draw-payload one, so a reader that walks the object finds both where the two
        // comments say they are.
        EXPECT_LT(json.find("\"cmd-bytes-per-draw-histogram\""),
                  json.find("\"staged-blob-bytes-histogram\""))
            << json;
    }
#endif

    // Successive summaries report WINDOWS, not run totals: a run total over a workload that
    // changes shape (load, then steady state) averages away the very number section 2.3.1
    // wants. Advancing the window is an explicit call, not a side effect of formatting.
    TEST_F(PipeStatsTest, SummaryLinesReportDisjointWindows) {
        PS::AddCalls(PS::CallClass::Draws, 10);
        PS::OnPresent();
        const String first = PS::FormatWindowLine();
        EXPECT_NE(first.find("draws=10"), String::npos) << first;
        PS::AdvanceSummaryWindow();

        PS::AddCalls(PS::CallClass::Draws, 3);
        PS::OnPresent();
        const String second = PS::FormatWindowLine();
        EXPECT_NE(second.find("draws=3"), String::npos) << second;
        EXPECT_NE(second.find("frames=2"), String::npos) << second;
    }

    // FormatWindowLine is pure. It used to rewrite the window bases as a side effect of
    // formatting, so any second reader - a probe, a test, a second reporting channel -
    // silently zeroed the next window.
    TEST_F(PipeStatsTest, FormattingTwiceDoesNotConsumeTheWindow) {
        PS::AddCalls(PS::CallClass::Draws, 7);
        PS::OnPresent();

        const String first = PS::FormatWindowLine();
        const String second = PS::FormatWindowLine();
        EXPECT_EQ(first, second) << first << "\n" << second;
        EXPECT_NE(second.find("draws=7"), String::npos) << second;

        // ...and advancing explicitly does close it.
        PS::AdvanceSummaryWindow();
        const String third = PS::FormatWindowLine();
        EXPECT_NE(third.find("draws=0"), String::npos) << third;
    }

    TEST_F(PipeStatsTest, SummaryLineSurvivesZeroDraws) {
        PS::AddCalls(PS::CallClass::AccessorCalls, 12);
        PS::OnPresent();
        const String line = PS::FormatWindowLine();
        // No draw in the window means there is no per-draw number - and "0.00" beside a
        // non-zero acc= would read as one.
        EXPECT_NE(line.find("acc/draw=n/a"), String::npos) << line;
        EXPECT_NE(line.find("acc=12"), String::npos) << line;
    }

    // A window with no Present in it has no per-frame reading at all. This used to divide by
    // a faked 1 and print the window TOTALS under a "/f" label: a scenario slice that draws
    // 47 times and never presents reported 1,404,550 staged bytes as a per-frame figure,
    // which is a 47x overstatement of the SEG_STAGE sizing input this package exists to
    // produce.
    TEST_F(PipeStatsTest, SummaryLineSurvivesZeroFrames) {
        PS::AddCalls(PS::CallClass::Draws, 47);
        PS::AddBytes(PS::ByteClass::StageBuffer, 1404550);

        const String line = PS::FormatWindowLine();
        EXPECT_EQ(PS::FrameCount(), 0u);
        EXPECT_NE(line.find("window=0"), String::npos) << line;
        EXPECT_NE(line.find("draws/f=n/a"), String::npos) << line;
        // The bracket is relabelled rather than divided: totals, and marked as totals.
        EXPECT_EQ(line.find("bytes/f["), String::npos) << line;
        EXPECT_NE(line.find("bytes[buf=1404550"), String::npos) << line;
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
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageIndirectCmd), "stage-indirect-cmd");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::PersistentMapPush), "persistent-map-push");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::ResidualValueBlock), "residual-value-block");
#if MOBILEGL_PIPE_PUSH
        EXPECT_STREQ(PS::NameOf(PS::CallClass::RenderStateCsoMints), "render-state-cso-mints");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::RenderStateCsoBinds), "render-state-cso-binds");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::MapPersistentRoundtrips), "map-persistent-roundtrips");
        // P4a's six. The four set counters are how the suppressors' hit rates are read, ctu is
        // the client-side twin of Espryt's tex-upload-emissions - a divergence between the two
        // is the only way an upload-SHAPE regression becomes visible, because SSIM cannot see
        // the box/rect split at all - and cso-blob-bytes is the ByteClass that discharges the
        // summary line's missing CSO-blob row.
        EXPECT_STREQ(PS::NameOf(PS::CallClass::FramebufferEmissions), "framebuffer-emissions");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::SamplerViewEmissions), "sampler-view-emissions");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::SamplerStateEmissions), "sampler-state-emissions");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::ShaderImageEmissions), "shader-image-emissions");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::ClientTextureUploadEmissions),
                     "client-tex-upload-emissions");
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::CsoBlobBytes), "cso-blob-bytes");
        // P6 gate 8's two, under their long names - the JSON dump keys and the names the
        // MEASUREMENTS.md entry will quote.
        EXPECT_STREQ(PS::NameOf(PS::ByteClass::StageSegmentBytes), "stage-segment-bytes");
        EXPECT_STREQ(PS::NameOf(PS::CallClass::WireRecords), "wire-records");
#endif
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
        EXPECT_EQ(PS::SummaryFramePeriod(), 120u);
        for (Uint64 i = 0; i < PS::SummaryFramePeriod(); ++i) {
            PS::OnPresent();
        }
        EXPECT_EQ(PS::FrameCount(), PS::SummaryFramePeriod());
    }
} // namespace
