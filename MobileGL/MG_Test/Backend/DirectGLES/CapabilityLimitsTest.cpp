// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/CapabilityLimitsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_State/GLState/TextureState/TextureState.h>

using namespace MobileGL;

// Iris budgets every program's samplers (vertex and fragment together) against
// GL_MAX_TEXTURE_IMAGE_UNITS, so an ES driver's 16-per-stage answer leaves shader packs without a
// pipeline. The fragment limit reports the desktop value whenever the driver's combined limit can
// number that many units, and never more than the combined limit or the per-stage cap.
TEST(DirectGLESCapabilityLimits, AdvertisesDesktopFragmentTextureUnitsWhenCombinedLimitAllows) {
    constexpr Int kPerStageCap = MG_State::GLState::TextureState::MAX_PER_STAGE_TEXTURE_IMAGE_UNITS;
    MG_Backend::DirectGLES::BackendObject_DirectGLES backend;

    MG_External::GLESCapabilities adreno;
    adreno.MaxTextureImageUnits = 16;
    adreno.MaxVertexTextureImageUnits = 16;
    adreno.MaxCombinedTextureImageUnits = 96;
    backend.ApplyGLESCapabilitiesForTesting(adreno);
    EXPECT_EQ(backend.GetDynamicParameters().MaxTextureImageUnits, kPerStageCap);
    EXPECT_EQ(backend.GetDynamicParameters().MaxVertexTextureImageUnits, 16);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedTextureImageUnits, 96);

    MG_External::GLESCapabilities narrowCombined;
    narrowCombined.MaxTextureImageUnits = 16;
    narrowCombined.MaxCombinedTextureImageUnits = 20;
    backend.ApplyGLESCapabilitiesForTesting(narrowCombined);
    EXPECT_EQ(backend.GetDynamicParameters().MaxTextureImageUnits, 20);

    MG_External::GLESCapabilities wide;
    wide.MaxTextureImageUnits = 64;
    wide.MaxCombinedTextureImageUnits = 192;
    backend.ApplyGLESCapabilitiesForTesting(wide);
    EXPECT_EQ(backend.GetDynamicParameters().MaxTextureImageUnits, kPerStageCap);
}
