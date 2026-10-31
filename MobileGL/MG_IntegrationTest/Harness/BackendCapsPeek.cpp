// MobileGL - MobileGL/MG_IntegrationTest/Harness/BackendCapsPeek.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BackendCapsPeek.h"

#if !defined(__ANDROID__)
#include <MG_Backend/BackendObject.h>

namespace MobileGL::MG_Backend {
    // Declared in MG_Backend/BackendObjects.h, which also pulls in both backends' headers
    // and, through them, their loaders; the reference alone is all that is needed here.
    extern UniquePtr<BackendObject>& pActiveBackendObject;
} // namespace MobileGL::MG_Backend
#endif

namespace MGITest {

    bool PeekComputeWorkGroupCaps(int outCount[3], int outSize[3]) {
#if defined(__ANDROID__)
        (void)outCount;
        (void)outSize;
        return false;
#else
        const auto& backend = MobileGL::MG_Backend::pActiveBackendObject;
        if (!backend) {
            return false;
        }
        const MobileGL::MG_Backend::DynamicBackendParameters& caps = backend->GetDynamicParameters();
        for (int axis = 0; axis < 3; ++axis) {
            outCount[axis] = caps.MaxComputeWorkGroupCount[axis];
            outSize[axis] = caps.MaxComputeWorkGroupSize[axis];
        }
        return true;
#endif
    }

} // namespace MGITest
