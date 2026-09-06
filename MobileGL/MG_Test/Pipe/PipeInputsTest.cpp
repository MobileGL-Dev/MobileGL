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

#else // MOBILEGL_PIPE_PUSH

// Negative control B, layer 1 (P1 brief D6 / G5): the omitted (verb, field) pair is the
// ONLY thing that goes stale - a sibling field of the same verb is fresh, and the verb after
// it neither heals the field (not in kDraw's mask) nor loses one of its own.
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
    const std::string before = ReadLog();
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child: no gtest assertions, _exit never exit.
        MGPipeSetPoisonOmission("GenerateMipmap", "GetActiveTextureUnit");
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetRenderStateParameters(); // a filled field of the preceding draw: must not abort
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        (void)gPipeInputs.GetTextureUnitObject(0); // the sibling field: filled, must not abort
        (void)gPipeInputs.GetActiveTextureUnit();  // the omitted field: Fatal
        ::_exit(3);                                // reached only if the poison failed
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status)) << "child exited normally with " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    EXPECT_EQ(WTERMSIG(status), SIGABRT);
    const std::string log = ReadLog().substr(before.size());
    EXPECT_NE(log.find(kOmittedFatal), std::string::npos) << log;
    EXPECT_EQ(log.find("@DrawArrays"), std::string::npos) << log;
    EXPECT_EQ(log.find("GetTextureUnitObject@"), std::string::npos) << log;
#endif
}

// The sibling: the same reads without the omission complete, and no Fatal is logged.
TEST_F(PipeInputsTest, ReadingAFilledFieldCompletes) {
#if !MGTEST_HAVE_FORK
    GTEST_SKIP() << "no fork() on this platform";
#else
    ASSERT_FALSE(g_logPath.empty()) << "main() did not set MOBILEGL_LOG_FILE_PATH";
    const std::string before = ReadLog();
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        MGPipeFillForVerb(MGPipeVerb::DrawArrays);
        (void)gPipeInputs.GetRenderStateParameters();
        MGPipeFillForVerb(MGPipeVerb::GenerateMipmap);
        (void)gPipeInputs.GetTextureUnitObject(0);
        (void)gPipeInputs.GetActiveTextureUnit();
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "child died on signal " << (WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    EXPECT_EQ(WEXITSTATUS(status), 0);
    const std::string log = ReadLog().substr(before.size());
    EXPECT_EQ(log.find("Fatal{"), std::string::npos) << log;
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
    // write, and caches the FILE*.
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "mobilegl-pipeinputs-test.log";
    std::error_code ec;
    fs::remove(path, ec);
    g_logPath = path.string();
#if defined(_WIN32)
    _putenv_s("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str());
#else
    setenv("MOBILEGL_LOG_FILE_PATH", g_logPath.c_str(), 1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
