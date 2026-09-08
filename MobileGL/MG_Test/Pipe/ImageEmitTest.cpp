// MobileGL - MobileGL/MG_Test/Pipe/ImageEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's image-unit set: set_shader_images, the third of the three kVarTail unit sets. It rides
// the sampler family's subsystem bit - one family, one A/B - and has its own suite because its
// content hash has to cover two fields the other two sets do not carry.
//
// THE TWO CASES THIS SUITE EXISTS FOR: an ACCESS-mode change alone, and an INTERNAL-FORMAT
// change alone, each has to move the hash and emit the set. Both are live glBindImageTexture
// state, the format-less image bake keys on the format the shader was built against, and a
// hash over the bindings alone would suppress exactly the record that says the bake is stale.
// The behavioural gates beside them are the format-less bake and non-core-format scenarios,
// and the photon fixture on desktop retrace - the only fixture that has ever caught an
// image-binding-semantics regression, and one that must never be run on the Adreno.
//
// THE SUITE IS `ImageEmit`, not `ImageEmitTest`: the file is XTest.cpp and the suite is X,
// this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT.
//
// IT HAS ITS OWN main() for ResourceEmitTest's reason. Every case is a visible SKIP in a pull
// build rather than a vanishing test, so `ctest -N` stays name-for-name identical between the
// pull and the push trees.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#define MGTEST_HAVE_FORK 0
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define MGTEST_HAVE_FORK 1
#endif

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Impl/Pipe/ImageEmit.h>
#include <MG_Pipe/PipeApply.h>
#endif

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    String g_logPath;

    int ProcessId() {
#if defined(_WIN32)
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

#if MOBILEGL_PIPE_PUSH
    std::string ReadLog() {
        std::ifstream in(g_logPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // A fresh applier per case, BOTH SCOPES, and it takes both because there are two: a reset
    // is a make-current and deliberately KEEPS the object records, so a fixture that wants a
    // genuinely empty applier has to say the other one as well. Every case is its own process
    // under ctest, so this is belt and braces - but running the binary by hand must give the
    // same answers as running it under ctest.
    struct ApplierGuard {
        ApplierGuard() {
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
        ~ApplierGuard() {
            MGPipeApplierReset();
            MGPipeApplierReleaseObjectRecords();
        }
    };

#if MGTEST_HAVE_FORK
    struct ChildResult {
        int Status = -1;
        std::string Log;
    };

    template <class Body>
    ChildResult RunInChild(Body body) {
        ChildResult result;
        std::error_code ec;
        std::filesystem::remove(g_logPath, ec);
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
        result.Log = ReadLog();
        return result;
    }

    Bool DiedOfAbort(const ChildResult& r) { return WIFSIGNALED(r.Status) && WTERMSIG(r.Status) == SIGABRT; }
    std::string DescribeStatus(const ChildResult& r) {
        if (r.Status < 0) return "fork/waitpid failed";
        if (WIFEXITED(r.Status)) return "exited " + std::to_string(WEXITSTATUS(r.Status));
        if (WIFSIGNALED(r.Status)) return "signal " + std::to_string(WTERMSIG(r.Status));
        return "status " + std::to_string(r.Status);
    }
#endif // MGTEST_HAVE_FORK

    // Drives a call a trip wire must REFUSE, and asserts the wire NAMED what it refused. The
    // two arms differ by design: a poison or verify build stops the process, so the drive is a
    // forked child and the parent reads SIGABRT plus the line out of the log; a shipped push
    // build logs and carries on from a defined state, so there the line is read back in process
    // and the caller goes on to assert that nothing moved.
    template <class Body>
    void ExpectRefusedNaming(const char* needle, Body body) {
#if MOBILEGL_PIPE_POISON || MOBILEGL_PIPE_VERIFY
#if MGTEST_HAVE_FORK
        const std::string tagged = std::string("Fatal{ProtocolCorruption} ") + needle;
        const ChildResult child = RunInChild(body);
        EXPECT_TRUE(DiedOfAbort(child)) << DescribeStatus(child) << "; log: " << child.Log;
        EXPECT_NE(child.Log.find(tagged), std::string::npos)
            << "the gate fired without naming what it refused; wanted \"" << tagged << "\"; log: " << child.Log;
#else
        (void)needle;
        (void)body; // no fork on this platform; the verdict here is std::abort()
#endif
#else
        const std::string tagged = std::string("ProtocolCorruption ") + needle;
        const std::string before = ReadLog();
        body();
        EXPECT_NE(ReadLog().substr(before.size()).find(tagged), std::string::npos)
            << "the gate refused without saying what it refused; wanted \"" << tagged << "\"";
#endif
    }
#endif // MOBILEGL_PIPE_PUSH
} // namespace

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder.
TEST(ImageEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeImageEmitterInstance(), &MGPipeImageEmitterInstance());
    // The image set's window is bounded by the same merged unit space the other two sets use;
    // there is no separate image-unit capacity and there must not be one, because a record
    // whose window is checked against a different bound from the array it indexes is the shape
    // the applier's Fatal{ProtocolCorruption} exists to make impossible.
    EXPECT_EQ(kMGPipeMaxImageUnits, kMGPipeMaxTextureUnits);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

// =========================================================================================
// The APPLIER's half of set_shader_images (the wire commits'). The emitter's half - the
// high-water-zero early-out, the content hash covering Access and InternalFormat, the shutter
// keyed on the FRONTEND sampling-resolution generation - is the client package's.
// =========================================================================================

#if MOBILEGL_PIPE_PUSH
namespace {
    // Every field carries a value of its own, and two of them are the point: InternalFormat and
    // Access are live glBindImageTexture state that the format-less image bake keys on, so a
    // body that dropped either would leave the server baking against a format the shader was
    // not built for.
    MGPImageView ImageAt(Uint32 unit, Uint32 internalFormat, Uint8 access) {
        MGPImageView view{};
        view.Res = MGPipeHandle{unit + 1, 1};
        view.Unit = unit;
        view.InternalFormat = internalFormat;
        view.Layer = 3;
        view.Level = 2;
        view.Layered = 1;
        view.Access = access;
        return view;
    }
} // namespace
#endif

// The window rule, one field at a time: the entries land where the header says and nowhere
// else, and every field of an entry survives. Deleting the copy loop, the two window
// assignments or the serial bump leaves this red.
TEST(ImageEmit, TheImageSetLandsInItsWindowWithEveryFieldTheShaderWasBuiltAgainst) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    // Access is a Uint8 on the wire - the client's own read/write/read-write encoding, not a
    // GL enum - and InternalFormat is the application's, which the server recasts.
    const MGPImageView entries[2] = {ImageAt(2, 0x8814u /* GL_RGBA32F */, 2 /* write only */),
                                     ImageAt(3, 0x8230u /* GL_RG32F */, 3 /* read write */)};
    MGPShaderImages header{};
    header.Start = 2;
    header.Count = 2;
    header.ContentHash = 0x5150u;
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;
    // The other two sets' serials, taken AFTER the fixture: a reset and a teardown each advance
    // every working serial, so "unchanged" is measured from here rather than from zero.
    const Uint64 samplerViewsSerial = MGPipeApplier().SamplerViewsSerial;

    MGPipeApplySetShaderImages(header, entries);

    EXPECT_EQ(MGPipeApplier().ShaderImageStart, 2u);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Res, (MGPipeHandle{3, 1}));
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].InternalFormat, 0x8814u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Access, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].Access, 3u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Level, 2u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Layer, 3u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[2].Layered, 1u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].InternalFormat, 0x8230u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[1].Res)) << "the set wrote below its window";
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[4].Res)) << "the set wrote above its window";
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);

    // "The last set as received": a narrower set says nothing about what it does not name.
    MGPShaderImages narrow{};
    narrow.Start = 2;
    narrow.Count = 1;
    MGPipeApplySetShaderImages(narrow, entries);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[3].InternalFormat, 0x8230u)
        << "the entry outside the new window was cleared";
    EXPECT_EQ(MGPipeApplier().SamplerViewsSerial, samplerViewsSerial)
        << "the image set moved another set's serial; the three are independent";
#endif
}

// The window gate, at the bound and one past it, and the null-tail arm. The image-unit space
// is the same merged 192 the sampler units are.
TEST(ImageEmit, AnImageWindowPastTheImageUnitSpaceIsRefusedRatherThanTruncated) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(0, 0x8058u, 2);

    MGPShaderImages exact{};
    exact.Start = kMGPipeMaxImageUnits - 1;
    exact.Count = 1;
    MGPipeApplySetShaderImages(exact, &entry);
    ASSERT_EQ(MGPipeApplier().ShaderImageCount, 1u);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPShaderImages past{};
    past.Start = kMGPipeMaxImageUnits;
    past.Count = 1;
    past.ContentHash = 9;
    ExpectRefusedNaming("set_shader_images {start=192, count=1, hash=9}: the window runs past the merged "
                        "texture-unit space",
                        [&past, &entry]() { MGPipeApplySetShaderImages(past, &entry); });

    MGPShaderImages noTail{};
    noTail.Start = 0;
    noTail.Count = 1;
    ExpectRefusedNaming("set_shader_images {start=0, count=1, hash=0}: a non-empty set carries no entries",
                        [&noTail]() { MGPipeApplySetShaderImages(noTail, nullptr); });

    EXPECT_EQ(MGPipeApplier().ShaderImagesSerial, serialBefore) << "a refused set moved the serial";
    EXPECT_EQ(MGPipeApplier().ShaderImageStart, kMGPipeMaxImageUnits - 1);
#endif
}

// An EMPTY set is not a refusal: it is what a program with no image uniforms publishes, and it
// still moves the serial, because "no images" is a state the twin has to hear about.
TEST(ImageEmit, AnEmptySetIsAppliedRatherThanRefusedAndStillMovesTheSerial) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(0, 0x8058u, 1);
    MGPShaderImages filled{};
    filled.Count = 1;
    MGPipeApplySetShaderImages(filled, &entry);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPShaderImages empty{};
    MGPipeApplySetShaderImages(empty, nullptr);
    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 0u);
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);
    EXPECT_EQ(MGPipeApplier().BoundShaderImages[0].InternalFormat, 0x8058u)
        << "an empty window cleared entries it never named";
#endif
}

// D-J4: the image set is per-context WORKING state, so a make-current takes it and ADVANCES
// its serial rather than restarting it.
TEST(ImageEmit, AMakeCurrentClearsTheImageSetAndAdvancesItsSerial) {
#if !MOBILEGL_PIPE_PUSH
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
    ApplierGuard guard;
    const MGPImageView entry = ImageAt(1, 0x8058u, 1);
    MGPShaderImages header{};
    header.Start = 1;
    header.Count = 1;
    MGPipeApplySetShaderImages(header, &entry);
    const Uint64 serialBefore = MGPipeApplier().ShaderImagesSerial;

    MGPipeApplierReset();

    EXPECT_EQ(MGPipeApplier().ShaderImageCount, 0u);
    EXPECT_EQ(MGPipeApplier().ShaderImageStart, 0u);
    EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundShaderImages[1].Res));
    EXPECT_GT(MGPipeApplier().ShaderImagesSerial, serialBefore);
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-imageemit-test-" + std::to_string(ProcessId()) + ".log");
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
