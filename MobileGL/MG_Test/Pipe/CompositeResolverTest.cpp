// MobileGL - MobileGL/MG_Test/Pipe/CompositeResolverTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P4a's program-pipeline COMPOSITE: GLContext::GetProgramForDraw() already flattens a pipeline
// into one hidden composite ProgramObject entirely in the frontend, so the client pushes ONE
// handle for it, allocated out of the ShaderCso reserved high band, and the server never
// learns it is a composite - it needs no "resolved draw program" hook at all.
//
// WHAT THIS SUITE IS ACTUALLY FOR: the composite's slot has TWO INDEPENDENT RELEASE PATHS -
// the pipeline cache's LRU eviction and the composite ProgramObject's own destructor - and
// both go through one client-side death helper. Either order has to free the slot exactly
// once, and the second call has to be a proven no-op rather than a lucky one. That is what the
// eviction-then-destruction pair and its mirror pin, and it is why the composite gets a leak
// case of its own beside the five ordinary kinds.
//
// THE SUITE IS `CompositeResolver`, not `CompositeResolverTest`: the file is XTest.cpp and the
// suite is X, this directory's convention.
//
// THE TARGET AND ITS ctest REGISTRATION ARE THE CONTRACT COMMIT'S; THE CONTENTS ARE NOT - the
// resolver itself, its signature-keyed cache and the two release orders are the client
// package's, and it never has to come back to MG_Test/Pipe/CMakeLists.txt.
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
#include <MG_Impl/Pipe/SlotAllocator.h>
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

// The contract commit's one case, and it pins the property everything else in this suite is
// built on: the composite band has EXACTLY ONE DOOR. The ordinary allocator refuses the band
// for kind ShaderCso, AllocateComposite is the only way in, and a slot from one can never be
// mistaken for a slot from the other - which is what reserving a band rather than setting a
// flag on the handle buys, and what keeps the resolver's lifetime bookkeeping out of the
// ordinary program allocator.
TEST(CompositeResolver, TheCompositeBandHasExactlyOneDoor) {
#if MOBILEGL_PIPE_PUSH
    MGPipeSlotAllocator slots;

    // The ordinary door never opens onto the band, however many times it is used.
    for (int i = 0; i < 8; ++i) {
        const MGPipeHandle ordinary = slots.Allocate(MGPipeKind::ShaderCso);
        EXPECT_FALSE(MGPipeHandleIsNull(ordinary));
        EXPECT_FALSE(MGPipeIsCompositeShaderSlot(ordinary.Slot));
    }

    // The composite door only ever opens onto it, and the handle it hands out is an ORDINARY
    // ShaderCso handle in every other respect - the same kind, the same {slot, gen} rules, the
    // same Free. The server cannot tell the difference and must not be able to.
    const MGPipeHandle composite = slots.AllocateComposite(4242);
    EXPECT_FALSE(MGPipeHandleIsNull(composite));
    EXPECT_TRUE(MGPipeIsCompositeShaderSlot(composite.Slot));
    EXPECT_TRUE(slots.IsLive(MGPipeKind::ShaderCso, composite));
    EXPECT_EQ(slots.FindByLifetimeId(MGPipeKind::ShaderCso, 4242), composite);

    // TWO RELEASE PATHS, ONE FREE. The second call resolves the same handle at a generation
    // the slot no longer has, and Free refuses it - which is what makes "the pipeline cache
    // evicted it and then the composite's destructor ran" safe in either order rather than a
    // double free that only shows up as slot theft much later.
    const Uint32 liveBefore = slots.LiveCount(MGPipeKind::ShaderCso);
    slots.Free(MGPipeKind::ShaderCso, composite);
    slots.Free(MGPipeKind::ShaderCso, composite);
    EXPECT_EQ(slots.LiveCount(MGPipeKind::ShaderCso), liveBefore - 1);
    EXPECT_FALSE(slots.IsLive(MGPipeKind::ShaderCso, composite));

    // And the slot really goes back to the band rather than to the ordinary free list: the
    // next composite reuses it with a bumped generation, and no ordinary program can be handed
    // it.
    const MGPipeHandle recycled = slots.AllocateComposite(4343);
    EXPECT_EQ(recycled.Slot, composite.Slot);
    EXPECT_NE(recycled.Gen, composite.Gen);
    EXPECT_FALSE(MGPipeIsCompositeShaderSlot(slots.Allocate(MGPipeKind::ShaderCso).Slot));
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: there is no client slot allocator in a pull build";
#endif
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::temp_directory_path() / ("mobilegl-compositeresolver-test-" + std::to_string(ProcessId()) + ".log");
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
