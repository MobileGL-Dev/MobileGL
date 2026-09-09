// MobileGL - MobileGL/MG_IntegrationTest/Harness/P4aFinalFixPeek.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The white-box readings P4aFinalFixScenario.cpp takes, in a translation unit of their own for
// P4aSeamPeek.h's reason: a scenario TU includes the GL prototype headers and cannot include
// MG_Pipe/PipeApply.h or the Espryt managers beside them, and PipeApplyPeek.cpp is the gates
// package's file. Every entry point answers false where the reading cannot be taken (a pull
// build, Android, or an applier that holds no record for the name), and a false teaches the
// caller nothing - the case declines that half by name and keeps its public-GL verdict.
#pragma once

namespace MGITest {

    // The applier's resource record for a texture, found by its GL name (GlNameForDiag - a
    // diagnostics-only field, which is exactly what a test harness is).
    struct PipeTextureResourceRecordPeek {
        unsigned Slot;
        unsigned Gen;
        unsigned long long Serial;
        unsigned BindMask;
        unsigned ImageBindableHint;
        unsigned Levels;
        unsigned PendingUploads;
    };
    bool PeekPipeTextureResourceRecord(unsigned glTextureName, PipeTextureResourceRecordPeek* out);

} // namespace MGITest
