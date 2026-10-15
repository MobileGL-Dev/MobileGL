// MobileGL - MobileGL/MG_Test/TestMain.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <MG_Util/Async/ShaderCompilePool.h>

// gtest's main, plus one step: the shader compile workers stop before main returns, as an
// application's eglTerminate stops them. Left to the pool's atexit fallback, a worker still
// compiling when the tests end can deadlock the MSVC runtime: the fallback waits for the worker
// while holding the runtime's exit lock, and the worker needs that lock to register the destructor
// of a static it constructs for the first time.
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    MobileGL::MG_Util::Async::ShaderCompilePool::StopAndDrainProcessPoolAtExit();
    return result;
}
