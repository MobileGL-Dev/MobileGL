// MobileGL - MobileGL/MG_IntegrationTest/Harness/PoisonOmissionArm.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PoisonOmissionArm.h"

#if !defined(__ANDROID__)
#include <MG_Pipe/MGPipe.h>
#include <MG_Impl/Pipe/PipeFill.h>
#if MOBILEGL_PIPE_VERIFY
#define MGITEST_POISON_OMISSION_ARM_LIVE 1
#endif
#endif

namespace MGITest {

#if defined(MGITEST_POISON_OMISSION_ARM_LIVE)
    bool PoisonOmissionArmAvailable() {
        return true;
    }

    bool ArmPoisonOmission(const char* verb, const char* field) {
        return MobileGL::MG_Pipe::MGPipeSetPoisonOmission(verb, field);
    }
#else
    bool PoisonOmissionArmAvailable() {
        return false;
    }

    bool ArmPoisonOmission(const char*, const char*) {
        return false;
    }
#endif

} // namespace MGITest
