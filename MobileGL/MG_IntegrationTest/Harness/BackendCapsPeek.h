// MobileGL - MobileGL/MG_IntegrationTest/Harness/BackendCapsPeek.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The one place this module looks past the GL API into the active backend's caps block.
//
// It exists for exactly one assertion: that the six per-axis compute limits the MGPipe
// caps block carries (DynamicBackendParameters::MaxComputeWorkGroupCount/Size, plan B
// section 4.4.1) are the same numbers glGetIntegeri_v answers today, since P0.5 retires
// the getter in favour of the caps. A separate translation unit, because the scenario
// sources include the GL headers with prototypes and MobileGL's umbrella header is not
// meant to meet them in one file.

#pragma once

namespace MGITest {

    // Copies the active backend's MaxComputeWorkGroupCount / MaxComputeWorkGroupSize into the
    // two arrays and returns true. Returns false, touching nothing, where the caps block is
    // out of reach: on Android this module links the SHIPPING libMobileGL.so, built
    // -fvisibility=hidden, so no internal symbol resolves; on desktop it links MobileGL_s and
    // the read is direct.
    bool PeekComputeWorkGroupCaps(int outCount[3], int outSize[3]);

} // namespace MGITest
