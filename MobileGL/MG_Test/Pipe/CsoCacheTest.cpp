// MobileGL - MobileGL/MG_Test/Pipe/CsoCacheTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The 64-entry render-state CSO cache: hash, probe, memcmp, LRU evict, and the content-addressing-off control (P2 brief D7).
//
// STUB, and deliberately one. It is created by the P2 CONTRACT commit together with its
// CMakeLists.txt registration, so that the package which owns its CONTENTS
// (P2 package B, p2/tracker) never has to touch MG_Test/Pipe/CMakeLists.txt - no two P2
// packages edit the same file, which is what keeps the integrator's rebases clean.
//
// The placeholder case is not decoration: without it the binary has no test, and
// gtest_discover_tests on a binary with no test is a silently green lane.
#include <gtest/gtest.h>

#include "Includes.h"
#include <MG_Pipe/MGPipe.h>

using namespace MobileGL;
using namespace MobileGL::MG_Pipe;

namespace {
    // The cache does not exist yet; the behaviour bit it is measured against does, and it
    // is deliberately the TOP bit so no subsystem allocation can ever collide with it.
    TEST(CsoCache, PlaceholderUntilTheOwningPackageFillsThisIn) {
        EXPECT_EQ(kMGPipeBehaviourNoCsoContentAddressing, 1ull << 63);
    }
} // namespace
