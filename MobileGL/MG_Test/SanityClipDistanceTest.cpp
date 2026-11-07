// MobileGL - MobileGL/MG_Test/SanityClipDistanceTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The clip and cull distance limits each backend advertises: native where the device clips,
// the emulated eight where it does not, and cull only where it is hosted.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/BackendObject_DirectGLES.h>
#include <MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h>
#include <MG_Util/BackendLoaders/OpenGL/Loader.h>

// maxClipDistances is a LIMIT every Vulkan device reports; declaring ClipDistance in a module
// needs the shaderClipDistance FEATURE, which is separate and which VulkanRenderer enables only
// where the physical device has it. Without the feature the device's limit is never forwarded:
// the backend EMULATES the built-in instead (EmulateClipDistancePass), which is what lets it
// answer GL 3.0's minimum of eight on every Mali device. With the feature it keeps its real
// number and the native path.
TEST(DirectVulkanSanity, GatesClipDistancesOnTheShaderClipDistanceFeature) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    MG_External::VulkanCapabilities caps;
    caps.MaxClipDistances = 8;

    caps.SupportsShaderClipDistance = false;
    caps.MaxClipDistances = 0; // what a Mali device reports alongside the missing feature
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_TRUE(backend.GetDynamicParameters().EmulatesClipDistance);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);

    EXPECT_FALSE(backend.GetDynamicParameters().MasksNativeClipDistances);

    caps.SupportsShaderClipDistance = true;
    caps.MaxClipDistances = 8;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_FALSE(backend.GetDynamicParameters().EmulatesClipDistance);
    // Vulkan has no per-distance enable state, so the native path masks in the shader.
    EXPECT_TRUE(backend.GetDynamicParameters().MasksNativeClipDistances);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);
}

// The cull half of the same contract. shaderCullDistance is a SEPARATE feature from
// shaderClipDistance - VulkanRenderer enables each independently - so it gets its own gate, and
// the combined limit is gated on either being present because GL 4.6 core 11.1.3.10 makes it at
// least as large as both halves. These three used to be literal 8s inside BuildTBuiltInResource
// with no device consulted at all, which let glslang accept a gl_CullDistance write that then
// discarded every primitive it touched.
TEST(DirectVulkanSanity, GatesCullDistancesOnTheShaderCullDistanceFeature) {
    using namespace MobileGL;

    MG_Backend::DirectVulkan::BackendObject_DirectVulkan backend;
    MG_External::VulkanCapabilities caps;
    caps.MaxClipDistances = 8;
    caps.MaxCullDistances = 8;
    caps.MaxCombinedClipAndCullDistances = 8;

    // Neither feature: cull stays zero (a per-primitive cull has no per-fragment substitute),
    // while clip is emulated, so the combined limit describes the emulated eight.
    caps.SupportsShaderClipDistance = false;
    caps.SupportsShaderCullDistance = false;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);

    // Clip only: cull stays zero, and the combined limit still describes the clip capacity.
    caps.SupportsShaderClipDistance = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);

    caps.SupportsShaderCullDistance = true;
    backend.ApplyVulkanCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);
}

// DirectGLES reaches clip AND cull distances only through GL_EXT_clip_cull_distance, so the
// loader leaves all three at zero without it and the backend forwards that verbatim. Zero is the
// answer that stops a gl_CullDistance shader from reaching an ESSL compiler that would reject it.
TEST(DirectGLESSanity, ForwardsTheProbedClipAndCullDistanceLimits) {
    using namespace MobileGL;

    MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    MG_External::GLESCapabilities caps;
    backend.ApplyGLESCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 0);

    caps.SupportsClipDistance = true;
    caps.MaxClipDistances = 8;
    caps.MaxCullDistances = 8;
    caps.MaxCombinedClipAndCullDistances = 8;
    backend.ApplyGLESCapabilitiesForTesting(caps);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);
    EXPECT_FALSE(backend.GetDynamicParameters().EmulatesClipDistance);
}

// The loader's verdict that the driver cannot clip natively (no extension, or the POST clip probe
// caught it misbehaving) turns the clip limit into the emulated eight - GL 3.0's minimum - while
// cull keeps the driver's own answer: a per-primitive cull has no per-fragment substitute.
TEST(DirectGLESSanity, AnEmulatingLoaderVerdictAdvertisesEightClipDistancesAndNoCull) {
    using namespace MobileGL;

    MG_Backend::DirectGLES::BackendObject_DirectGLES backend;
    MG_External::GLESCapabilities caps; // no GL_EXT_clip_cull_distance, as on every Mali driver
    caps.EmulatesClipDistance = true;
    backend.ApplyGLESCapabilitiesForTesting(caps);
    EXPECT_TRUE(backend.GetDynamicParameters().EmulatesClipDistance);
    EXPECT_EQ(backend.GetDynamicParameters().MaxClipDistances, 8);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCullDistances, 0);
    EXPECT_EQ(backend.GetDynamicParameters().MaxCombinedClipAndCullDistances, 8);
}
