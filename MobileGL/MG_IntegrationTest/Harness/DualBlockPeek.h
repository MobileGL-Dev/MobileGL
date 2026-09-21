// MobileGL - MobileGL/MG_IntegrationTest/Harness/DualBlockPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P5f (f1), P5F-WIRE-COMPLETENESS.md §4/§6: THE DUAL-BLOCK REHEARSAL, READ FROM THE RUNNING
// PROCESS. The lane's negative control is a scenario that is green exactly when the two roles
// hold two DISTINCT PipeInputs blocks, and "the blocks are distinct" is a fact about pointers
// and serials inside the library - so, like SplitRuntimePeek, the reading is taken from the
// process and never inferred from the environment the entry was launched with.
//
// A SEPARATE TRANSLATION UNIT for PipeApplyPeek's reason, verbatim: the scenario sources
// include the GL headers with prototypes and PipeInputs.h is not meant to meet them in one
// file.
//
// PeekDualBlockState returns false, touching nothing, where it cannot look: a build without
// MOBILEGL_BUILD_DISAGGREGATED (the block split is compiled there only), or Android, where
// this module links the shipping libMobileGL.so built -fvisibility=hidden. A caller that gets
// false has learned NOTHING and must skip, not fail.

#pragma once

namespace MGITest {

    struct DualBlockPeekState {
        // False wherever the peek cannot look (see the header above). Everything else is
        // meaningful only when this is true.
        bool peekAvailable = false;
        // MGPipeRoleSplitRehearsalActive(): the knob AND a real transport. The control's red half turns
        // on this answer alone.
        bool roleSplitActive = false;
        // &MGPipeClientInputs() != &gPipeInputs - the fill side and the read side are two
        // different objects.
        bool blocksDistinct = false;
        // The two blocks' CurrentVerbSerial. The client block's advances with the fill side's
        // verb boundaries; the server block's moves only on a server verb stamp.
        unsigned long long clientSerial = 0;
        unsigned long long serverSerial = 0;
        // gPipeInputs.ContextIdentity(): non-null once the server has stamped a verb boundary
        // under the rehearsal (CONTRACT-P5E §3.2's server-owned identity).
        const void* serverIdentity = nullptr;
    };

    bool PeekDualBlockState(DualBlockPeekState* out);

    // Drives ONE client-side verb boundary - MGPipeValidateForVerb + MGPipeLeaveVerb - with no
    // draw behind it: exactly the fill side's write shape (serial bump, fill, verb set), and
    // nothing that stamps the server block. Returns false where the peek cannot look.
    bool DualBlockDriveClientVerbBoundary();

} // namespace MGITest
