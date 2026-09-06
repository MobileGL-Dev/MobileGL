// MobileGL - MobileGL/MG_Test/Pipe/TrackerTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The frontend state tracker: the dirty walk, the five aggregate generations, the per-bit fire counters (P2 brief D4).
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
    // The tracker does not exist yet; what is true in every build is that the subsystem
    // bitmask it dispatches on is allocated and does not overlap the behaviour bit.
    TEST(Tracker, PlaceholderUntilTheOwningPackageFillsThisIn) {
        EXPECT_EQ(kMGPipeSubsystemsMigratedAtP2 & kMGPipeBehaviourNoCsoContentAddressing, 0ull);
    }
} // namespace
