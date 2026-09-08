// MobileGL - MobileGL/MG_Test/Pipe/TextureEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's texture and renderbuffer family: resource_create from the constructor,
// resource_respecify from every storage definition, set_texture_params from the parameter
// mutators, and resource_subdata from the drain list at the validate point.
//
// THIS SUITE IS A NAMED GATE (`ctest -R 'TextureEmit\.'`), and one of its invariants is the
// one nothing else in the tree can see: the union box and the region list have to describe the
// SAME texels, because the server picks the upload shape from them and SSIM is completely
// blind to which one it picked. The Mali cliff behind that choice is ~+6 ms/frame for a
// hundred one-rect jobs against one union box.
//
// THE SUITE IS `TextureEmit`, not `TextureEmitTest`: the file is XTest.cpp and the suite is X,
// this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT. The
// applier-side cases are the wire commits'; the emitter-side cases (every texture target
// mapping to its own resource target, every bind kind setting its mask bit and the bit being
// sticky across a respecify, the image-bindable hint being forever, the box/rect invariant,
// the level shadow's strides, the collapse to the box past the rect cap, an upload through a
// view keying on the storage owner, every texture's params naming its built-in sampler CSO and
// two identical samplers sharing one, and a destroyed texture releasing its resource, view and
// sampler slots) are the client package's - and neither has to come back to
// MG_Test/Pipe/CMakeLists.txt to add one.
//
// IT HAS ITS OWN main() for ResourceEmitTest's reason: the applier's bounds and protocol trip
// wires report through a log line in a shipped push build and std::abort() in a poison or
// verify one.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test, so `ctest -N`
// stays name-for-name identical between the pull and the push trees.

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
#include <MG_Impl/Pipe/TextureEmit.h>
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

// See FramebufferEmitTest's twin for why this is a shape pin rather than a placeholder: a
// static holding client state whose destructor an exit handler can run is the exit-order
// use-after-free this design closed once already.
TEST(TextureEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeTextureEmitterInstance(), &MGPipeTextureEmitterInstance());
    EXPECT_TRUE(kMGPipeWiredTextureSubsystem == 0 ||
                kMGPipeWiredTextureSubsystem == kMGPipeSubsystemTextureResources);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-textureemit-test-" + std::to_string(ProcessId()) + ".log");
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
