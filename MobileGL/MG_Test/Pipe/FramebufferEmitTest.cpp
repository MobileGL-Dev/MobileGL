// MobileGL - MobileGL/MG_Test/Pipe/FramebufferEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's framebuffer family: set_framebuffer_state, emitted per bound target, on both sides of
// the call.
//
// THIS SUITE IS A NAMED GATE. The phase's descriptor-consistency gate is "for every framebuffer
// configuration the emitted MGPFramebufferState reproduces exactly the values the backend's
// SyncToBackend family reads from the frontend today, field by field", and it is spelled
// `ctest -R 'FramebufferEmit\.'`; its negative control is a script that stops the conversion
// copying ONE field (MGPSurface::Layered) and expects this suite to go red NAMING that field.
// So a case here must fail by field name, never by a bare count, or the control cannot answer.
//
// THE SUITE IS `FramebufferEmit`, not `FramebufferEmitTest`: the file is XTest.cpp and the
// suite is X, this directory's convention, and it is what the gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT. The
// applier-side cases (a record's lifecycle, the per-target storage, what a make-current does
// and does not clear) are the wire commits'; the emitter-side cases (the resolved read
// surface, the draw-buffer array in the content hash, a recycled handle never suppressed
// against its predecessor, every attachment field surviving the surface conversion, an
// attachment point above the wire width refused rather than truncated, a re-storaged attached
// renderbuffer publishing its new extent) are the client package's - and neither of them has
// to come back to MG_Test/Pipe/CMakeLists.txt to add one.
//
// IT HAS ITS OWN main() for the same reason ResourceEmitTest and VertexInputEmitTest do: the
// applier's bounds and protocol trip wires report through a log line in a shipped push build
// and std::abort() in a poison or verify one, so a case that drives one reads the line back
// out of a file this process names before anything logs.
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
#include <MG_Impl/Pipe/FramebufferEmit.h>
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

// The one case the contract commit lands, and it is not a placeholder: it pins the SHAPE every
// later case depends on. The emitter is a process singleton that is heap-constructed and
// intentionally leaked, because a static holding client state whose destructor an exit handler
// can run is the exit-order use-after-free this design closed once already - `exit` runs the
// frontend's own teardown into a pipe whose allocator has already been destroyed. One
// allocation for the life of the process, no destructor to lose.
TEST(FramebufferEmit, TheEmitterIsOneNeverDestroyedProcessSingleton) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(&MGPipeFramebufferEmitterInstance(), &MGPipeFramebufferEmitterInstance());
    // And the family's wired-subsystem constant is either 0 or its own bit and nothing else.
    // It is 0 until this family's emitter has a body; the OR in PipeFill.cpp is what turns it
    // into the switch, so a header that set the wrong bit would switch the wrong family on.
    EXPECT_TRUE(kMGPipeWiredFramebufferSubsystem == 0 ||
                kMGPipeWiredFramebufferSubsystem == kMGPipeSubsystemFramebuffer);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client emitter in a pull build";
#endif
}

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-framebufferemit-test-" + std::to_string(ProcessId()) + ".log");
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
