// MobileGL - MobileGL/MG_Test/Pipe/ProgramEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's program family: create/bind/delete_shader_state, set_draw_program,
// set_dispatch_program and set_global_constants.
//
// THE ONE PIN THAT IS EASIEST TO LOSE AND WORST TO LOSE: set_global_constants' Version is
// GetUBOContentVersion(), and ~0u is the BACKENDS' "never uploaded" sentinel - the wrap skips
// it - so the client must never emit it. A record carrying the sentinel would tell a backend
// that a block it has just been handed was never uploaded.
//
// THE SUITE IS `ProgramEmit`, not `ProgramEmitTest`: the file is XTest.cpp and the suite is X,
// this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT: the
// applier-side cases are the wire commits' and the emitter-side cases are the client
// package's, and neither has to come back to MG_Test/Pipe/CMakeLists.txt to add one.
//
// IT HAS ITS OWN main() for ResourceEmitTest's reason. Every case is a visible SKIP in a pull
// build rather than a vanishing test, so `ctest -N` stays name-for-name identical between the
// pull and the push trees.

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
#include <MG_Impl/Pipe/ProgramEmit.h>
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
} // namespace

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder.
TEST(ProgramEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeProgramEmitterInstance(), &MGPipeProgramEmitterInstance());
    EXPECT_TRUE(kMGPipeWiredProgramSubsystem == 0 ||
                kMGPipeWiredProgramSubsystem == kMGPipeSubsystemPrograms);
    // The record the applier starts from carries the sentinel, not 0: a program that has never
    // published a default-uniform-block image must not look like one that published version 0.
    const MGPipeShaderCsoRecord fresh{};
    EXPECT_EQ(fresh.GlobalConstantsVersion, ~Uint32{0});
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-programemit-test-" + std::to_string(ProcessId()) + ".log");
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
