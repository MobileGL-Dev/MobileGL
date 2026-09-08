// MobileGL - MobileGL/MG_Test/Pipe/ResourceEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P3a's resource family: the applier's record lifecycle and the client's emission of it.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT.
// Two packages fill this file in and neither of them touches MG_Test/Pipe/CMakeLists.txt to
// do it: the applier-side cases (a create marks the slot live, a respecify replaces the
// descriptor and bumps Serial, a destroy clears Live, a stale generation resolves to nothing,
// HasLiveHostWrites is false on every path this phase has, and the sub-data range encoding at
// both of its bounds with the emitter's splitter over it) belong to the branch that gives the
// entry points their bodies; the emitter-side cases (the sticky BindMask over every buffer
// target, on create AND on a following respecify; the slot released at destruction) belong to
// the client branch. They are disjoint TEST bodies in one file.
//
// THE SUITE IS `ResourceEmit`, not `ResourceEmitTest`: the file is XTest.cpp and the suite is
// X, which is this directory's convention (RenderStateSpansTest.cpp -> RenderStateSpans), and
// it is what the phase's gate greps for (`ctest -R '...|ResourceEmit\.'`).
//
// IT HAS ITS OWN main(), like PipeInputsTest and RenderStateSpansTest, and that is a decision
// taken here so that nobody has to come back to the CMake file for it: the applier's bounds
// gate reports through a trip wire whose verdict is a log line in a shipped push build and
// std::abort() in a poison or verify one, so a case that drives it reads the line back out of
// a file this process points MOBILEGL_LOG_FILE_PATH at before anything logs.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test - the applier is
// compiled only under MOBILEGL_PIPE_PUSH - so `ctest -N` stays name-for-name identical
// between the pull and the push trees.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
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

    // The op table is INSTALLED BY A BACKEND, at its own bring-up, and uninstalled at its
    // teardown - it is not part of the applier's state and MGPipeApplierReset deliberately
    // does not clear it. A process with no backend in it therefore has none, and that is the
    // fact the whole family's landability rests on: with no table registered every frontend
    // dispatch falls through to the op table this one replaces, so the client half can land
    // on its own without changing a single observable.
    //
    // It is also the negative control for the registration itself. A Set that did not stick
    // would leave the family permanently dark, and nothing else in the tree would say so.
    TEST(ResourceEmit, TheResourceOpTableIsUnregisteredUntilABackendInstallsOne) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        ASSERT_EQ(MGPipeGetResourceOps(), nullptr)
            << "something registered a resource op table in a unit-test process";

        static const MGPipeResourceOps ops{};
        MGPipeSetResourceOps(&ops);
        EXPECT_EQ(MGPipeGetResourceOps(), &ops);

        // A state reset is not a teardown: the table survives it, because the backend that
        // installed it is still there.
        MGPipeApplierReset();
        EXPECT_EQ(MGPipeGetResourceOps(), &ops);

        MGPipeSetResourceOps(nullptr);
        EXPECT_EQ(MGPipeGetResourceOps(), nullptr);
#endif
    }
} // namespace

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-resourceemit-test-" + std::to_string(ProcessId()) + ".log");
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
