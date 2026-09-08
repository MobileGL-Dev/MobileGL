// MobileGL - MobileGL/MG_Test/Pipe/VertexInputEmitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P3a's vertex-input family: create/bind/delete_vertex_elements, set_vertex_buffers and
// set_index_buffer, on both sides of the call.
//
// THIS SUITE IS A NAMED GATE. The phase's G6 is "for every VAO configuration the emitted
// MGPVertexElements blob + MGPVertexBuffers set + MGPIndexBuffer reproduce exactly the values
// the backend's VAO twin reads from the frontend today, field by field, for all 32 attribute
// slots", and it is spelled `ctest -R 'VertexInputEmit\.'`; G7 is its negative control, a
// script that stops the wire conversion copying ONE field and expects this suite to go red
// NAMING that field. So a case here must fail by field name, never by a bare count, or the
// control cannot answer.
//
// THE SUITE IS `VertexInputEmit`, not `VertexInputEmitTest`: the file is XTest.cpp and the
// suite is X, this directory's convention (RenderStateSpansTest.cpp -> RenderStateSpans), and
// it is what both gates grep for.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT - the
// client package writes the conversion cases, the base-instance suppression pair and the
// create/bind ping-pong pair into this file without touching MG_Test/Pipe/CMakeLists.txt.
//
// IT HAS ITS OWN main() for the same reason ResourceEmitTest does: the applier refuses a
// vertex-elements record whose declared counts do not describe its own blob, and that verdict
// is a log line in a shipped push build and std::abort() in a poison or verify one.
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

    // A FRESH CONTEXT IS A FRESH SERVER, and for this family that is not a nicety: the three
    // serials are per-context MGGens and the backend's VAO twin decides "have I already
    // synced this?" by comparing its own memo against them. A reset that carried a previous
    // context's count over would let a twin believe it had synced a configuration it has
    // never seen - the one shape the tracker's complete-state rule exists to forbid.
    //
    // The bound handle is null rather than "whatever was bound", the window is empty rather
    // than 32 stale entries, and the fetch shift is 0 rather than the last draw's.
    TEST(VertexInputEmit, AResetApplierCarriesNoVertexInputStateOver) {
#if !MOBILEGL_PIPE_PUSH
        GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no applier in this build";
#else
        MGPipeApplierState& applier = MGPipeApplier();
        applier.BoundVertexElements = MGPipeHandle{7, 3};
        applier.VertexBufferStart = 1;
        applier.VertexBufferCount = 5;
        applier.VertexFetchBaseInstance = 9;
        applier.VertexBuffersSerial = 42;
        applier.IndexBufferSerial = 43;
        applier.MapPersistentRoundtrips = 44;

        MGPipeApplierReset();

        EXPECT_TRUE(MGPipeHandleIsNull(MGPipeApplier().BoundVertexElements));
        EXPECT_EQ(MGPipeApplier().VertexBufferStart, 0u);
        EXPECT_EQ(MGPipeApplier().VertexBufferCount, 0u);
        EXPECT_EQ(MGPipeApplier().VertexFetchBaseInstance, 0u);
        EXPECT_EQ(MGPipeApplier().VertexBuffersSerial, 0u);
        EXPECT_EQ(MGPipeApplier().IndexBufferSerial, 0u);
        EXPECT_EQ(MGPipeApplier().MapPersistentRoundtrips, 0u);
        EXPECT_TRUE(MGPipeApplier().VertexElementsCsos.empty());
        EXPECT_TRUE(MGPipeApplier().Resources.empty());
#endif
    }
} // namespace

int main(int argc, char** argv) {
    // Before anything logs: the logger reads this variable once, on its first write, and
    // caches the handle. The name carries this process's pid, and the file is removed on the
    // way out.
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-vertexinputemit-test-" + std::to_string(ProcessId()) + ".log");
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
