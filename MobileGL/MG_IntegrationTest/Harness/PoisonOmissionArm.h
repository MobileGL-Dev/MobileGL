// MobileGL - MobileGL/MG_IntegrationTest/Harness/PoisonOmissionArm.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// NEGATIVE CONTROL B'S OMISSION, ARMED FROM THE HARNESS. PoisonOmissionScenario's re-executed
// child withholds the stamp of one (verb, field) pair before it runs the sequence, so that verb's
// read of the field must be Fatal{UnmigratedPipeInput, "<Field>@<Verb>"}. The library has no knob
// for it: only a -DMOBILEGL_PIPE_VERIFY=ON build compiles the setter (MG_Impl/Pipe/PipeFill.h,
// MGPipeSetPoisonOmission), and this file is the one place that calls it.
//
// A SEPARATE TRANSLATION UNIT for PipeApplyPeek's reason, verbatim: the scenario sources include
// the GL headers with prototypes and the pipe headers are not meant to meet them in one file.
//
// Both entry points return false, touching nothing, where the setter is out of reach: a build
// without MOBILEGL_PIPE_VERIFY, or Android, where this module links the shipping libMobileGL.so
// built -fvisibility=hidden.

#pragma once

namespace MGITest {

    // True when this build can arm the omission at all.
    bool PoisonOmissionArmAvailable();

    // Withholds the stamp of `field` at `verb` (kMGPipeVerbNames / kMGPipeInputFieldNames spellings)
    // for the rest of this process, in the client's fill and in the server's verb stamp. Call it on
    // the thread that issues the GL calls. False when the build cannot, or a name is unknown.
    bool ArmPoisonOmission(const char* verb, const char* field);

} // namespace MGITest
