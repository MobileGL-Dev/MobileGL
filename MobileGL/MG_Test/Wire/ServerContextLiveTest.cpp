// MobileGL - MobileGL/MG_Test/Wire/ServerContextLiveTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14: MGPipeServerSetContextLive is the calling SESSION's flag. The in-process display server
// runs KWin and every Plasma client in one process; a Qt client's eglMakeCurrent(NO_CONTEXT)
// (ServerLoop's ClientRelease arm) or a session's teardown used to clear one process global, and
// KWin's next draw then read its own served context as dead (Fatal{UnmigratedPipeInput,
// "GetFramebufferBindingSlot@DrawArrays"} on the device). Each session thread resolves its own
// PipeInputs block; the flag lives there.

#include <Config.h>
#include <MG_Backend/MGPipe/PipeInputs.h>

#include <gtest/gtest.h>

#include <thread>

using namespace MobileGL;

namespace {
    thread_local MG_Pipe::PipeInputs* t_sessionBlock = nullptr;

    MG_Pipe::PipeInputs* ResolveTestSessionBlock() { return t_sessionBlock; }

    // A fresh thread, bound to `block` before its first touch of gPipeInputs - the order
    // ThreadSessionScope keeps on a real session thread.
    template <typename Fn>
    void OnSessionThread(MG_Pipe::PipeInputs& block, Fn fn) {
        std::thread thread([&] {
            t_sessionBlock = &block;
            fn();
        });
        thread.join();
    }
} // namespace

TEST(ServerContextLiveTest, OneSessionsReleaseDoesNotKillANeighboursContext) {
    const MG_Pipe::PipeInputsThreadResolver previous = MG_Pipe::g_pipeInputsThreadResolver;
    MG_Pipe::g_pipeInputsThreadResolver = &ResolveTestSessionBlock;
    auto* kwinBlock = new MG_Pipe::PipeInputs();
    auto* appBlock = new MG_Pipe::PipeInputs();

    OnSessionThread(*kwinBlock, [] { MG_Pipe::MGPipeServerSetContextLive(true); });
    OnSessionThread(*appBlock, [] { MG_Pipe::MGPipeServerSetContextLive(true); });
    OnSessionThread(*appBlock, [] { MG_Pipe::MGPipeServerSetContextLive(false); });

    Bool kwinLive = false;
    Bool appLive = true;
    OnSessionThread(*kwinBlock, [&] { kwinLive = MG_Pipe::MGPipeServerContextIsLive(); });
    OnSessionThread(*appBlock, [&] { appLive = MG_Pipe::MGPipeServerContextIsLive(); });
    EXPECT_TRUE(kwinLive) << "a neighbour session's release marked this session's context dead";
    EXPECT_FALSE(appLive);

    MG_Pipe::g_pipeInputsThreadResolver = previous;
    // Leaked on purpose, like every PipeInputs block (ID-8): no thread may outlive its block.
}

// The Wire suites link gtest without gtest_main (MG_Test/Wire/CMakeLists.txt); nothing here
// needs a log file, so this is the plain runner.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
