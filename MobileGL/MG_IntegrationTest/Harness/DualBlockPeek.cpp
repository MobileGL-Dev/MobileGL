// MobileGL - MobileGL/MG_IntegrationTest/Harness/DualBlockPeek.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DualBlockPeek.h"

#if defined(MGITEST_SPLIT_RUNTIME_PEEK) && !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#if MOBILEGL_PIPE_PUSH
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Impl/Pipe/PipeFill.h>
// The serials live behind FilledState(), which exists only where the poison build flag is on -
// and PipeInputs.h derives that flag, so the question can only be asked AFTER the include. A
// disaggregated build always has it (MOBILEGL_BUILD_DISAGGREGATED is one of its three arms).
#if MOBILEGL_PIPE_POISON
#define MGITEST_DUAL_BLOCK_PEEK_LIVE 1
#endif
#endif
#endif

namespace MGITest {

#if defined(MGITEST_DUAL_BLOCK_PEEK_LIVE)
    // The serials live behind the poison build's FilledState(); the peek is live exactly in
    // the configurations where MOBILEGL_PIPE_POISON is (a disaggregated build always has it,
    // which is what the #if above says), so an answer here is never a different build's guess.
    bool PeekDualBlockState(DualBlockPeekState* out) {
        if (out == nullptr) return false;
        namespace MGP = MobileGL::MG_Pipe;
        out->peekAvailable = true;
        out->roleSplitActive = MGP::MGPipeRoleSplitActive();
        MGP::PipeInputs& client = MGP::MGPipeClientInputs();
        out->blocksDistinct = &client != &MGP::gPipeInputs;
        out->clientSerial = client.FilledState().CurrentVerbSerial;
        out->serverSerial = MGP::gPipeInputs.FilledState().CurrentVerbSerial;
        out->serverIdentity = MGP::gPipeInputs.ContextIdentity();
        return true;
    }

    bool DualBlockDriveClientVerbBoundary() {
        // ReadPixels: a verb whose class mask carries the pixel-pack state the scenario dirties
        // (so the emission step has a record to publish and the fixture's wire-moved assertion
        // is fed), while the read_pixels VERB RECORD itself is never emitted - that is the GL
        // entry point's job, not the validate point's - so nothing here reaches the server's
        // stamp path (Clear / DrawVbo / ReadPixels / Blit) and the server block's serial is
        // provably not this call's to move.
        MobileGL::MG_Pipe::MGPipeValidateForVerb(MobileGL::MG_Pipe::MGPipeVerb::ReadPixels);
        MobileGL::MG_Pipe::MGPipeLeaveVerb();
        return true;
    }
#else
    bool PeekDualBlockState(DualBlockPeekState*) { return false; }
    bool DualBlockDriveClientVerbBoundary() { return false; }
#endif

} // namespace MGITest
