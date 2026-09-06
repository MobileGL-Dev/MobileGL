// MobileGL - MobileGL/MG_Test/Pipe/PipeInputsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The P1 poison and verify shapes at the block level (P1 brief C.1): a fake GLContext, the
// real filler, the real accessors. Needs the push sources, so every case is a visible SKIP
// in a pull build rather than a vanishing test. The abort cases fork (HeadlessGL.cpp's
// pre-flight shape): the child performs the read that must be Fatal{UnmigratedPipeInput}
// and the parent reads SIGABRT out of waitpid and the exact line out of the log file that
// main() below points MOBILEGL_LOG_FILE_PATH at (LogLevelTest's shape). Never EXPECT_DEATH.
//
// The log file is per PROCESS: gtest_discover_tests runs every case as its own process, in
// parallel under ctest -j, and a name shared between them let a sibling's unlink or Fatal
// line land in this process's read. A forked child inherits its parent's path on purpose.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

#if MOBILEGL_PIPE_PUSH
#include <Config.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/PipeFill.h>
#include <MG_State/GLState/Core.h>
#endif

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define MGTEST_HAVE_FORK 1
#else
#include <process.h>
#define MGTEST_HAVE_FORK 0
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    std::string g_logPath;

    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    long ProcessId() {
#if defined(_WIN32)
        return static_cast<long>(::_getpid());
#else
        return static_cast<long>(::getpid());
#endif
    }

#if MOBILEGL_PIPE_PUSH
    using GLContext = MG_State::GLState::GLContext;

    // A live frontend context for the filler to read (SanityTest's idiom), restored on the
    // way out so the cases stay independent.
    class PipeInputsTest : public ::testing::Test {
    protected:
        void SetUp() override {
            m_previous = Move(MG_State::pGLContext);
            MG_State::pGLContext = MakeUnique<GLContext>();
            MGPipeSetPoisonOmission(nullptr, nullptr);
        }
        void TearDown() override {
            MGPipeSetPoisonOmission(nullptr, nullptr);
            MG_State::pGLContext = Move(m_previous);
        }
        UniquePtr<GLContext> m_previous;
    };

#if MOBILEGL_PIPE_POISON
    Bool Fresh(MGPipeInputField field) { return MGPipeInputFieldIsFresh(gPipeInputs.FilledState(), field); }
#endif

    [[maybe_unused]] constexpr const char* kOmittedFatal ="Fatal{UnmigratedPipeInput, \"GetActiveTextureUnit@GenerateMipmap\"}";
    [[maybe_unused]] constexpr const char* kOmissionKnob = "GenerateMipmap:GetActiveTextureUnit";

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;   // waitpid's status; -1 when fork or waitpid failed
        std::string Log;   // the lines the child appended to the log file
    };

    // Runs `body` in a forked child and returns its wait status and log delta. The child
    // must not use gtest assertions; it _exit(0)s when `body` returns, so a body that is
    // expected to die must be asserted dead by the parent (WIFSIGNALED), never assumed.
    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        const std::string before = ReadLog();
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) return result;
        if (pid == 0) {
            body();
            ::_exit(0);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) return result;
        result.Status = status;
        result.Log = ReadLog().substr(before.size());
        return result;
    }

    Bool DiedOfAbort(const ChildResult& r) { return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT; }
    Bool ExitedWith(const ChildResult& r, int code) { return WIFEXITED(r.Status) && WEXITSTATUS(r.Status) == code; }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif // MGTEST_HAVE_FORK
#endif // MOBILEGL_PIPE_PUSH
} // namespace

#if !MOBILEGL_PIPE_PUSH

TEST(PipeInputsTest,OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,ReadingAnOmittedFieldAbortsNamingTheVerb) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,ReadingAFilledFieldCompletes) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,CorruptedSnapshotFieldIsNamedWithItsSerial) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,EveryVerbFillsItsClassAndNothingElse) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,MutatedFieldIsNamedAtRead) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,PoisonOmitKnobArmsTheOmission) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,BadPoisonOmitKnobIsFatalNamingTheKnob) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,VerifyCorruptKnobNamesTheFieldAtEntry) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,BadVerifyCorruptKnobIsFatalNamingTheKnob) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}
TEST(PipeInputsTest,VerifyFatalOffLogsTheDivergenceAndContinues) {
    GTEST_SKIP() << "push not compiled in (MOBILEGL_PIPE_PUSH=OFF)";
}

#else // MOBILEGL_PIPE_PUSH

// Negative control B, layer 1 (P1 brief D6 / G5): the omitted (verb, field) pair is the
// ONLY thing that goes stale - every other bit of the verb class's mask is fresh, every
// field outside it is not, and the verb after it neither heals the field (not in kDraw's
// mask) nor loses one of its own.
TEST_F(PipeInputsTest, OmittingOneFieldForOneVerbLeavesExactlyThatFieldStale) {
#if !MOBILEGL_PIPE_POISON
    GTEST_SKIP() << "poison not compiled in (MOBILEGL_PIPE_POISON=0)";
#else
    MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
    EXPECT_TRUE(Fresh(MGPipeInputField::GetActiveTextureUnit));
    EXPECT_TRUE(Fresh(MGPipeInputField::GetTextureUnitObject));

    MGPipeSetPoisonOmission("GenerateMipmap", "GetActiveTextureUnit");
    MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
    EXPECT_TRUE(Fresh(MGPipeInputField::GetTextureUnitObject));
    EXPECT_FALSE(Fresh(MGPipeInputField::GetActiveTextureUnit));
    // The value was still copied: only the stamp is withheld.
    EXPECT_EQ(gPipeInputs.CurrentVerb(), MGPipeVerb::GenerateMipmap);
    // Exactly that field: a field is fresh iff its bit is in kTextureOp's mask and it is not
    // the omitted one.
    {
        const auto cls = kMGPipeVerbClass[static_cast<SizeT>(MGPipeVerb::GenerateMipmap)];
        const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(cls)];
        for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
            const auto field = static_cast<MGPipeInputField>(f);
            const Bool expected = MGPipeFieldMaskHas(mask, field) && field != MGPipeInputField::GetActiveTextureUnit;
            EXPECT_EQ(Fresh(field), expected) << kMGPipeInputFieldNames[f] << " after the omitted GenerateMipmap fill";
        }
    }

    MGPipeFillForVerb(MGPipeVerb::DrawArrays);
    EXPECT_FALSE(Fresh(MGPipeInputField::GetActiveTextureUnit));
    EXPECT_TRUE(Fresh(MGPipeInputField::GetBoundVertexArray));
    EXPECT_TRUE(Fresh(MGPipeInputField::GetRenderStateParameters));
    // A sticky field stays fresh across every verb.
    EXPECT_TRUE(Fresh(MGPipeInputField::RecordError));

    // And the omission is scoped to its verb: a different verb of the same class keeps it.
    MGPipeFillForVerb(MGPipeVerb::BindImageTexture);
    EXPECT_TRUE(Fresh(MGPipeInputField::GetActiveTextureUnit));
#endif
}

// Negative control B, layer 2 (G5): the read itself. The child fills GenerateMipmap with the
// omission and reads gPipeInputs.GetActiveTextureUnit(); the parent expects SIGABRT and the
// exact Fatal line, and that nothing else was fatal.
TEST_F(PipeInputsTest, ReadingAnOmittedFieldAbortsNamingTheVerb) {
#if !MOBILEGL_PIPE_POISON
    GTEST_SKIP() << "poison not compiled in (MOBILEGL_PIPE_POISON=0)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    const ChildResult r = RunInChild([] {
        MGPipeSetPoisonOmission("GenerateMipmap", "GetActiveTextureUnit");
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetRenderStateParameters(); // a filled field of the preceding draw: must not abort
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        (void)gPipeInputs.GetTextureUnitObject(0); // the sibling field: filled, must not abort
        (void)gPipeInputs.GetActiveTextureUnit();  // the omitted field: Fatal
        ::_exit(3);                                // reached only if the poison failed
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find(kOmittedFatal), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("@DrawArrays"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("GetTextureUnitObject@"), std::string::npos) << r.Log;
#endif
}

// The sibling: the same reads without the omission complete, and no Fatal is logged.
TEST_F(PipeInputsTest, ReadingAFilledFieldCompletes) {
#if !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    const ChildResult r = RunInChild([] {
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetRenderStateParameters();
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        (void)gPipeInputs.GetTextureUnitObject(0);
        (void)gPipeInputs.GetActiveTextureUnit();
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_EQ(r.Log.find("Fatal{"), std::string::npos) << r.Log;
#endif
}

// The MOBILEGL_PIPE_POISON_OMIT parser, through the Features field the loader fills: the
// child sets the knob after its parent filled without it, and the first fill after that
// parses it, logs the arming line and withholds the stamp exactly as the programmatic form.
TEST_F(PipeInputsTest, PoisonOmitKnobArmsTheOmission) {
#if !MOBILEGL_PIPE_POISON
    GTEST_SKIP() << "poison not compiled in (MOBILEGL_PIPE_POISON=0)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    MGPipeFillForVerb(MGPipeVerb::DrawArrays); // the parent's parse saw an empty knob
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipePoisonOmit = kOmissionKnob;
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        (void)gPipeInputs.GetTextureUnitObject(0); // the sibling field: filled, must not abort
        (void)gPipeInputs.GetActiveTextureUnit();  // the omitted field: Fatal
        ::_exit(3);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: poison omission armed - GetActiveTextureUnit@GenerateMipmap"), std::string::npos)
        << r.Log;
    EXPECT_NE(r.Log.find(kOmittedFatal), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("GetTextureUnitObject@"), std::string::npos) << r.Log;
#endif
}

// An unknown verb in the knob is Fatal{PipeVerifyBadKnob} naming the knob and the value,
// before any stamp is withheld.
TEST_F(PipeInputsTest, BadPoisonOmitKnobIsFatalNamingTheKnob) {
#if !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipePoisonOmit = "NoSuchVerb:GetActiveTextureUnit";
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        ::_exit(3);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{PipeVerifyBadKnob, \"MOBILEGL_PIPE_POISON_OMIT=NoSuchVerb:GetActiveTextureUnit\": "
                         "no such verb in kMGPipeVerbNames}"),
              std::string::npos)
        << r.Log;
    EXPECT_EQ(r.Log.find("poison omission armed"), std::string::npos) << r.Log;
#endif
}

// Negative control A at the block level (G4): a clean snapshot compares equal to the pushed
// block over the verb's mask; corrupting one field of the snapshot makes the compare name
// exactly that field, at the serial of the fill it belongs to.
TEST_F(PipeInputsTest, CorruptedSnapshotFieldIsNamedWithItsSerial) {
#if !MOBILEGL_PIPE_VERIFY
    GTEST_SKIP() << "verify not compiled in (MOBILEGL_PIPE_VERIFY=OFF)";
#else
    const Uint64 serialBefore = gPipeInputs.FilledState().CurrentVerbSerial;
    MGPipeFillForVerb(MGPipeVerb::DrawArrays);
    const Uint64 serial = gPipeInputs.FilledState().CurrentVerbSerial;
    EXPECT_EQ(serial, serialBefore + 1);
    const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(MGPipeVerbClass::kDraw)];

    static PipeInputs snapshot{};
    SnapshotFromGLContext(snapshot, mask);
    MGPipeInputField field = MGPipeInputField::kFieldCount;
    EXPECT_TRUE(MGPipeVerifyInputs(gPipeInputs, snapshot, mask, &field));
    EXPECT_EQ(field, MGPipeInputField::kFieldCount);

    ASSERT_TRUE(MGPipeApplyVerifyCorruption(snapshot, MGPipeInputField::GetRenderStateParameters));
    EXPECT_FALSE(MGPipeVerifyInputs(gPipeInputs, snapshot, mask, &field));
    EXPECT_EQ(field, MGPipeInputField::GetRenderStateParameters);
    EXPECT_STREQ(kMGPipeInputFieldNames[static_cast<SizeT>(field)], "GetRenderStateParameters");
    // The serial the report would print is the fill's, and it stamped that field.
    EXPECT_EQ(gPipeInputs.FilledState().FilledGen[static_cast<SizeT>(field)], serial);

    // A forwarded field has nothing to corrupt, and the corruption of a field outside the
    // mask is not seen by a compare over that mask.
    EXPECT_FALSE(MGPipeApplyVerifyCorruption(snapshot, MGPipeInputField::RecordError));
    SnapshotFromGLContext(snapshot, mask);
    ASSERT_TRUE(MGPipeApplyVerifyCorruption(snapshot, MGPipeInputField::GetPixelStoreParameters));
    EXPECT_FALSE(MGPipeFieldMaskHas(mask, MGPipeInputField::GetPixelStoreParameters));
    EXPECT_TRUE(MGPipeVerifyInputs(gPipeInputs, snapshot, mask, &field));
#endif
}

// The compare-at-read arm (P1 brief D8, the arm that is real in P1): a value that changes in
// the live context between the verb boundary and the read is Fatal{PipeVerifyDiffer,
// "Field@Verb", verb=<serial>, where=read}; the same read before the mutation completes.
TEST_F(PipeInputsTest, MutatedFieldIsNamedAtRead) {
#if !MOBILEGL_PIPE_VERIFY
    GTEST_SKIP() << "verify not compiled in (MOBILEGL_PIPE_VERIFY=OFF)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    MGPipeFillForVerb(MGPipeVerb::Clear); // the parent armed nothing: Features.PipeVerify is false here
    const Uint64 serial = gPipeInputs.FilledState().CurrentVerbSerial + 1; // the child's DrawArrays fill
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipeVerify = true;
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        const Float boundary = gPipeInputs.GetLineWidth(); // boundary == live: completes
        (void)gPipeInputs.GetRenderStateParameters();
        MG_State::pGLContext->SetLineWidth(boundary + 1.0f);
        if (MG_State::pGLContext->GetLineWidth() == boundary) ::_exit(7); // the mutation did not take
        (void)gPipeInputs.GetLineWidth(); // the stored value is stale against the live one: Fatal
        ::_exit(3);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: verify armed - 63 fields, 69 verbs, fatal=1"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: verify read of GetLineWidth (index 0, 0) differs from the live context"),
              std::string::npos)
        << r.Log;
    const std::string expected = "Fatal{PipeVerifyDiffer, \"GetLineWidth@DrawArrays\", verb=" + std::to_string(serial) +
                                 ", where=read}";
    EXPECT_NE(r.Log.find(expected), std::string::npos) << "expected " << expected << " in\n" << r.Log;
    EXPECT_EQ(r.Log.find("where=entry"), std::string::npos) << r.Log;
#endif
}

// The MOBILEGL_PIPE_VERIFY_CORRUPT parser through Features: the fill after the child arms it
// logs both arming lines and the entry compare names the corrupted field at that fill's
// serial, where=entry.
TEST_F(PipeInputsTest, VerifyCorruptKnobNamesTheFieldAtEntry) {
#if !MOBILEGL_PIPE_VERIFY
    GTEST_SKIP() << "verify not compiled in (MOBILEGL_PIPE_VERIFY=OFF)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    MGPipeFillForVerb(MGPipeVerb::Clear);
    const Uint64 serial = gPipeInputs.FilledState().CurrentVerbSerial + 1;
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipeVerify = true;
        MG_Config::Features.PipeVerifyCorrupt = "GetRenderStateParameters";
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        ::_exit(3);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: verify armed - 63 fields, 69 verbs, fatal=1"), std::string::npos) << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: verify corruption armed - GetRenderStateParameters"), std::string::npos) << r.Log;
    const std::string expected = "Fatal{PipeVerifyDiffer, \"GetRenderStateParameters@DrawArrays\", verb=" +
                                 std::to_string(serial) + ", where=entry}";
    EXPECT_NE(r.Log.find(expected), std::string::npos) << "expected " << expected << " in\n" << r.Log;
#endif
}

// An unknown field name in MOBILEGL_PIPE_VERIFY_CORRUPT is Fatal{PipeVerifyBadKnob} at
// arming, before the comparator reports anything.
TEST_F(PipeInputsTest, BadVerifyCorruptKnobIsFatalNamingTheKnob) {
#if !MOBILEGL_PIPE_VERIFY
    GTEST_SKIP() << "verify not compiled in (MOBILEGL_PIPE_VERIFY=OFF)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipeVerify = true;
        MG_Config::Features.PipeVerifyCorrupt = "NoSuchField";
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        ::_exit(3);
    });
    ASSERT_TRUE(DiedOfAbort(r)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("Fatal{PipeVerifyBadKnob, \"MOBILEGL_PIPE_VERIFY_CORRUPT=NoSuchField\": "
                         "no such field in kMGPipeInputFieldNames}"),
              std::string::npos)
        << r.Log;
    EXPECT_EQ(r.Log.find("verify armed"), std::string::npos) << r.Log;
    EXPECT_EQ(r.Log.find("PipeVerifyDiffer"), std::string::npos) << r.Log;
#endif
}

// MOBILEGL_PIPE_VERIFY_FATAL=0: the divergence is logged with its field and serial and the
// process goes on (the fill returns, the next fill's compare runs again).
TEST_F(PipeInputsTest, VerifyFatalOffLogsTheDivergenceAndContinues) {
#if !MOBILEGL_PIPE_VERIFY
    GTEST_SKIP() << "verify not compiled in (MOBILEGL_PIPE_VERIFY=OFF)";
#elif !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    MGPipeFillForVerb(MGPipeVerb::Clear);
    const Uint64 serial = gPipeInputs.FilledState().CurrentVerbSerial + 1;
    const ChildResult r = RunInChild([] {
        MG_Config::Features.PipeVerify = true;
        MG_Config::Features.PipeVerifyFatal = false;
        MG_Config::Features.PipeVerifyCorrupt = "GetRenderStateParameters";
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        MGPipeFillForVerb(MGPipeVerb::DrawElements);
        ::_exit(0);
    });
    ASSERT_TRUE(ExitedWith(r, 0)) << DescribeStatus(r) << "\n" << r.Log;
    EXPECT_NE(r.Log.find("MGPipe: verify armed - 63 fields, 69 verbs, fatal=0"), std::string::npos) << r.Log;
    const std::string first = "Fatal{PipeVerifyDiffer, \"GetRenderStateParameters@DrawArrays\", verb=" +
                              std::to_string(serial) + ", where=entry}";
    const std::string second = "Fatal{PipeVerifyDiffer, \"GetRenderStateParameters@DrawElements\", verb=" +
                               std::to_string(serial + 1) + ", where=entry}";
    EXPECT_NE(r.Log.find(first), std::string::npos) << "expected " << first << " in\n" << r.Log;
    EXPECT_NE(r.Log.find(second), std::string::npos) << "expected " << second << " in\n" << r.Log;
#endif
}

// Every verb fills exactly its class mask: after a fill, a field is fresh iff its bit is set
// (sticky fields included, since every class mask carries them).
TEST_F(PipeInputsTest, EveryVerbFillsItsClassAndNothingElse) {
#if !MOBILEGL_PIPE_POISON
    GTEST_SKIP() << "poison not compiled in (MOBILEGL_PIPE_POISON=0)";
#else
    for (SizeT v = 0; v < kMGPipeVerbCount; ++v) {
        const auto verb = static_cast<MGPipeVerb>(v);
        MGPipeFillForVerb(verb);
        const MGPipeFieldMask& mask = kMGPipeClassFieldMask[static_cast<SizeT>(kMGPipeVerbClass[v])];
        for (SizeT f = 0; f < kMGPipeInputFieldCount; ++f) {
            const auto field = static_cast<MGPipeInputField>(f);
            EXPECT_EQ(Fresh(field), MGPipeFieldMaskHas(mask, field))
                << kMGPipeInputFieldNames[f] << " after " << kMGPipeVerbNames[v];
        }
    }
    EXPECT_EQ(gPipeInputs.ContextIdentity(), static_cast<const void*>(MG_State::pGLContext.get()));
    EXPECT_TRUE(gPipeInputs.IsLive());
#endif
}

#endif // MOBILEGL_PIPE_PUSH

int main(int argc, char** argv) {
    // Before anything logs: MG_Util::Debug::InitFile() reads the variable once, on the first
    // write, and caches the FILE*. The name carries this process's pid (see the file header),
    // and the file is removed on the way out; a forked child that aborts leaves it to us.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-pipeinputs-test-" + std::to_string(ProcessId()) + ".log");
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    fs::remove(path, ec);
    return rc;
}
