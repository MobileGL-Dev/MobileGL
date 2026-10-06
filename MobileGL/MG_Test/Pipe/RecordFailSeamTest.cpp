// MobileGL - MobileGL/MG_Test/Pipe/RecordFailSeamTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE RECORD ARM'S FAIL / LATCH SEAM (MG_Pipe/PipeSessionFail.h), in both library shapes.
// Without MG_Remote the defaults are the monolith answer - the line on stderr, then the abort; with
// it, FatalFunnel.cpp's static registration is in place before any case runs, so the record arm's
// deaths count and publish through SessionFail exactly as a direct call did.

#include <MG_Pipe/PipeFatalFamily.h>
#include <MG_Pipe/PipeSessionFail.h>

#include <gtest/gtest.h>

#include <string>

using namespace MobileGL;
using MG_Pipe::MGFatalFamily;

TEST(RecordFailSeam, TheVocabularyIsTheDefFilesWords) {
    EXPECT_STREQ(MG_Pipe::FatalFamilyName(MGFatalFamily::ProtocolCorruption), "ProtocolCorruption");
    EXPECT_STREQ(MG_Pipe::FatalFamilyName(MGFatalFamily::BackendDeviceLost), "BackendDeviceLost");
    EXPECT_GT(MG_Pipe::MGFatalFamilyCount(), 10u);
}

TEST(RecordFailSeam, TheFunnelIsRegisteredWhereverMgRemoteIsLinked) {
#if MOBILEGL_BUILD_DISAGGREGATED
    EXPECT_NE(MG_Pipe::MGPipeRecordFailHookInstalled(), nullptr)
        << "FatalFunnel.cpp's static registration did not run: the record arm's deaths would skip "
           "SessionFail's count and SessionFault frame";
    EXPECT_NE(MG_Pipe::MGPipeRecordLatchHookInstalled(), nullptr);
    EXPECT_NE(MG_Pipe::MGPipeRecordLatchedHookInstalled(), nullptr);
#else
    EXPECT_EQ(MG_Pipe::MGPipeRecordFailHookInstalled(), nullptr) << "a library without MG_Remote has no funnel";
    EXPECT_EQ(MG_Pipe::MGPipeRecordLatchHookInstalled(), nullptr);
    EXPECT_EQ(MG_Pipe::MGPipeRecordLatchedHookInstalled(), nullptr);
#endif
    EXPECT_FALSE(MG_Pipe::MGPipeRecordLatched()) << "nothing latched in a process with no session";
}

TEST(RecordFailSeamDeathTest, AFailWritesTheCallersLineAndDies) {
    EXPECT_DEATH(MG_Pipe::MGPipeRecordFail(MGFatalFamily::ProtocolCorruption,
                                           "MGPipe: Fatal{ProtocolCorruption, \"record-seam-unit\"} n=%d", 7),
                 "Fatal\\{ProtocolCorruption, \"record-seam-unit\"\\} n=7");
}

TEST(RecordFailSeamDeathTest, ALatchWithNoSessionToLatchDiesTheSameWay) {
    EXPECT_DEATH((void)MG_Pipe::MGPipeRecordLatch(MGFatalFamily::BackendDeviceLost,
                                                  "MGPipe: Fatal{BackendDeviceLost, \"record-seam-unit\"} n=%d", 9),
                 "Fatal\\{BackendDeviceLost, \"record-seam-unit\"\\} n=9");
}

namespace {
    MGFatalFamily g_seenFamily{};
    std::string g_seenLine;
    bool CaptureLatch(MGFatalFamily family, const char* line) {
        g_seenFamily = family;
        g_seenLine = line;
        return false;
    }
    bool AlwaysLatched() { return true; }
} // namespace

// An installed latch hook gets the family and the FORMATTED line, and its answer is the call's.
TEST(RecordFailSeam, AnInstalledLatchHookGetsTheFormattedLine) {
    const auto fail = MG_Pipe::MGPipeRecordFailHookInstalled();
    const auto latch = MG_Pipe::MGPipeRecordLatchHookInstalled();
    const auto latched = MG_Pipe::MGPipeRecordLatchedHookInstalled();
    MG_Pipe::MGPipeInstallRecordFailHooks(fail, &CaptureLatch, &AlwaysLatched);
    const bool answer = MG_Pipe::MGPipeRecordLatch(MGFatalFamily::BackendDeviceLost,
                                                   "MGPipe: Fatal{BackendDeviceLost, \"captured\"} k=%u", 3u);
    const bool nowLatched = MG_Pipe::MGPipeRecordLatched();
    MG_Pipe::MGPipeInstallRecordFailHooks(fail, latch, latched);
    EXPECT_FALSE(answer);
    EXPECT_TRUE(nowLatched);
    EXPECT_EQ(g_seenFamily, MGFatalFamily::BackendDeviceLost);
    EXPECT_EQ(g_seenLine, "MGPipe: Fatal{BackendDeviceLost, \"captured\"} k=3");
    EXPECT_EQ(MG_Pipe::MGPipeRecordLatchHookInstalled(), latch) << "the case did not restore the hooks";
}
