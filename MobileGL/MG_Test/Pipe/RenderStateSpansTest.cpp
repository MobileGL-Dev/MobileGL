// MobileGL - MobileGL/MG_Test/Pipe/RenderStateSpansTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// G7: the render-state chunk table, its subset hash and the setter-consistency walk (P2 brief D19).
//
// STUB, and deliberately one. It is created by the P2 CONTRACT commit together with its
// CMakeLists.txt registration, so that the package which owns its CONTENTS
// (P2 package A, commit c2 on p2/spans) never has to touch MG_Test/Pipe/CMakeLists.txt - no two P2
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
    // The one fact this file can already state in EVERY build: the member list the chunk
    // table was derived from is non-empty and is what generated/PipeSpanTable.inc pins.
    TEST(RenderStateSpans, PlaceholderUntilTheOwningPackageFillsThisIn) {
        EXPECT_GT(kMGPipePipelineStateMemberCount, 0u);
        EXPECT_STREQ(kMGPipePipelineStateMembers[0], "PatchVertices");
    }
} // namespace
