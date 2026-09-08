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
