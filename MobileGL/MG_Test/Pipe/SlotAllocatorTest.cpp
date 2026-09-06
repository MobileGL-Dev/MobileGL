// MobileGL - MobileGL/MG_Test/Pipe/SlotAllocatorTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The client slot allocator's identity contract: gen moves only on reuse, and a recycled address never reproduces a handle (P2 brief C.0 c3).
//
// STUB, and deliberately one. It is created by the P2 CONTRACT commit together with its
// CMakeLists.txt registration, so that the package which owns its CONTENTS
// (P2 package A, commit c3 on p2/spans) never has to touch MG_Test/Pipe/CMakeLists.txt - no two P2
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
    // Slot 0 is reserved for every kind - null, and the default framebuffer for kind
    // Framebuffer - so the first allocatable slot is 1 in every build.
    TEST(SlotAllocator, PlaceholderUntilTheOwningPackageFillsThisIn) {
        EXPECT_EQ(kMGPipeFirstAllocatableSlot, 1u);
        EXPECT_TRUE(MGPipeHandleIsNull(kMGPipeNullHandle));
        EXPECT_FALSE(MGPipeHandleIsNull(kMGPipeDefaultFramebuffer));
    }
} // namespace
