// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DualBlockScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P5f (f1), P5F-WIRE-COMPLETENESS.md §6: THE DUAL-BLOCK MECHANISM'S NEGATIVE CONTROL, and it is
// deterministic rather than observational. "Give the two roles one PipeInputs block each"
// proves nothing if no case can tell one block from two, so this scenario asserts exactly that,
// from inside the process:
//
//   1. MGPipeRoleSplitRehearsalActive() - the knob is armed AND the transport is real;
//   2. &MGPipeClientInputs() != &gPipeInputs - two distinct objects;
//   3. a client verb boundary moves the CLIENT block's serial and leaves the SERVER block's
//      serial alone - the fill side's writes no longer reach the read side.
//
// With MOBILEGL_IPC_ROLE_SPLIT_STATE unset every one of the three fails by name - the block is
// shared, so (2) is false by construction and (3)'s "server serial does not move" fails because
// there is no second serial. The lane entry deliberately does NOT pin the knob in its own
// ENVIRONMENT: the gate's dualblock step exports MOBILEGL_IPC_ROLE_SPLIT_STATE=1 for the green
// form and scripts/ci/dualblock_negative_control.sh runs the same entry with it off for the red
// form, and a ctest ENVIRONMENT property would override the process environment in BOTH runs and
// make the red form unreachable. A bare `ctest -L integration-dualblock-split` without the knob
// is red, which is the honest answer for a lane whose name promises the split.
//
// No draw, clear or readback: under the armed knob every BARRIER-PULLED read is a named Fatal
// by design (that census is the lane's other half), and this case is the control - it must be
// GREEN on the armed arm. The lane pins MOBILEGL_IPC_RUN_AHEAD=0 so the fill decision is not a
// third variable: every verb boundary fills, deterministically.

#include "../Harness/DualBlockPeek.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {

class DualBlockScenario : public ScenarioTest {
protected:
    void SetUp() override {
        ScenarioTest::SetUp();
        if (!Ready()) return;
        const auto why = SplitRuntimeSkipReason();
        if (!why.empty()) GTEST_SKIP() << why;
    }
};

TEST_F(DualBlockScenario, TheTwoRolesHaveDistinctBlocks) {
    if (!Ready()) return;
    DualBlockPeekState before;
    ASSERT_TRUE(PeekDualBlockState(&before))
        << "the dual-block peek is compiled out: this entry is only ever registered in a "
           "disaggregated build, so a false here means the registration and the build disagree";
    ASSERT_TRUE(before.roleSplitActive)
        << "dual-block control: MGPipeRoleSplitRehearsalActive() is false - MOBILEGL_IPC_ROLE_SPLIT_STATE "
           "is not armed in this process (or the transport is monolith), so the two roles share "
           "one PipeInputs block and this case is the knob's negative control";
    EXPECT_TRUE(before.blocksDistinct)
        << "dual-block control: MGPipeClientInputs() and gPipeInputs are the SAME object with "
           "the rehearsal armed - the fill side and the read side were never split";
    EXPECT_NE(before.serverIdentity, nullptr)
        << "dual-block control: the server block's identity was never set (CONTRACT-P5E §3.2's "
           "server-owned SetIdentity) - the backend's identity-keyed memo caches read nullptr "
           "as a hit on their zero-initialised slot";

    // One client verb boundary, driven directly so no draw reaches the server: the CLIENT
    // block's serial must move and the SERVER block's must not. Under a shared block the second
    // half is impossible, which is the control's other edge. The glPixelStorei dirties the
    // pixel-pack state so the boundary's emission step has a record to publish - the fixture's
    // destructor asserts the wire moved on every armed split case.
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    ASSERT_TRUE(DualBlockDriveClientVerbBoundary());
    DualBlockPeekState after;
    ASSERT_TRUE(PeekDualBlockState(&after));
    EXPECT_GT(after.clientSerial, before.clientSerial)
        << "dual-block control: a client verb boundary did not move the client block's serial";
    EXPECT_EQ(after.serverSerial, before.serverSerial)
        << "dual-block control: a fill-side verb boundary moved the SERVER block's serial "
           << before.serverSerial << " -> " << after.serverSerial
           << " - the two roles are still writing one block";
}

} // namespace
} // namespace MGITest
