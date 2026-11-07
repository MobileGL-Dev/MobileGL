// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostClipDistance.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The POST rows for clip and cull distances on both backends. GL_MAX_CLIP_DISTANCES >= 8 is a
// GL 3.0 requirement: PASS where the backend clips natively, WARN where MobileGL emulates the
// built-in, FAIL where it does neither.

#pragma once

#include <Includes.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

namespace MobileGL::MG_Util::SelfTest {
    struct ClipDistancePostRow {
        enum class Verdict {
            Pass,
            Warn,
            FailOptional
        };
        Verdict verdict = Verdict::Pass;
        String name;
        String detail;
    };

    // "Clip distances" and "Cull distances" for the GLES backend, from the loader's decision and
    // its cached native probe.
    Vector<ClipDistancePostRow> GlesClipDistancePostRows(const MG_External::GLESCapabilities& caps,
                                                         const MG_External::GLESFunctionsTable& glesFuncs);

    // "shaderClipDistance" and "shaderCullDistance" for the Vulkan backend, mirroring
    // BackendObject_DirectVulkan's decision: emulate exactly when the feature is absent, unless
    // MOBILEGL_CLIP_DISTANCE_EMULATION says otherwise.
    Vector<ClipDistancePostRow> VulkanClipDistancePostRows(Bool hasClipDistance, Bool hasCullDistance,
                                                           Uint32 maxClipDistances, Uint32 maxCullDistances);
} // namespace MobileGL::MG_Util::SelfTest
