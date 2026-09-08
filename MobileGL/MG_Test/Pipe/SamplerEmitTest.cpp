// MobileGL - MobileGL/MG_Test/Pipe/SamplerEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's sampler family: the content-addressed sampler CSO, the identity-addressed sampler view
// per texture object, and the set_sampler_views / bind_sampler_states unit sets.
//
// THIS SUITE IS A NAMED GATE (`ctest -R 'SamplerEmit\.'`), and its negative control is a
// script that stops the conversion copying SamplerParameters::borderColorForm and expects this
// suite to go red NAMING that field - which it must, because all three border-colour
// representations are always numerically populated and the value alone cannot say which driver
// entry point to use.
//
// THE ONE CASE THAT LOOKS LIKE PARANOIA AND IS NOT: SamplerParameters is 100 bytes with THREE
// BYTES OF TRAILING PADDING, so a cache that hashes or memcmps the object's own bytes reads
// uninitialised memory and mints a fresh CSO per call - a 256-entry cache with a hit rate of
// zero, and nobody notices, because the pixels are right. The case that writes garbage into
// the padding through a byte pointer is what turns that into a red gate.
//
// THE SUITE IS `SamplerEmit`, not `SamplerEmitTest`: the file is XTest.cpp and the suite is X,
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
#include <MG_Impl/Pipe/SamplerEmit.h>
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
TEST(SamplerEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeSamplerEmitterInstance(), &MGPipeSamplerEmitterInstance());
    // One bit for the whole sampler family - the CSO, the view and all three unit sets,
    // including set_shader_images, whose emitter lives in ImageEmit.h. An operator switching
    // samplers off has to get the whole family's legacy arm, not two thirds of it.
    EXPECT_TRUE(kMGPipeWiredSamplerSubsystem == 0 ||
                kMGPipeWiredSamplerSubsystem == kMGPipeSubsystemSamplers);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-sampleremit-test-" + std::to_string(ProcessId()) + ".log");
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
