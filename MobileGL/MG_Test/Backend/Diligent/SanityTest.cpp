// MobileGL - MobileGL/MG_Test/Backend/Diligent/SanityTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#include <gtest/gtest.h>
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/Diligent/BackendObject_Diligent.h>
#include <MG_Backend/Diligent/Renderer/DiligentRenderer.h>
#include <MG_Backend/BackendObjects.h>

using namespace MobileGL;
using namespace MobileGL::MG_Backend;

TEST(DiligentVulkanBackend, CreatesDiligentDeviceAndAdvertisesGL32) {
    DiligentBackend::BackendObject_Diligent backend;

    backend.Initialize();

    EXPECT_EQ(backend.GetBackendType(), BackendType::DiligentVulkan);

    const RendererInfo& info = backend.GetRendererInfo();
    EXPECT_GE(info.RendererGLInfo.TargetGLVersion.Major, 3);
    if (info.RendererGLInfo.TargetGLVersion.Major == 3) {
        EXPECT_GE(info.RendererGLInfo.TargetGLVersion.Minor, 2);
    }

    EXPECT_FALSE(backend.GetBackendAPIVersionString().empty());
    EXPECT_FALSE(backend.GetRendererInfo().BackendName.empty());
}

TEST(DiligentVulkanBackend, ClearsAndDrawsTriangleOffscreen) {
    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();

    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping offscreen rendering test";
    }

    // Clear to green, then draw a red triangle over the center using a
    // dynamically uploaded vertex buffer.
    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    const float triangleVertices[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
         0.0f,  0.5f,
    };
    renderer->DrawVertices(triangleVertices, 3);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red from the triangle";
    EXPECT_LT(center[1], 50) << "center should not be green";

    std::uint8_t corner[4] = {};
    renderer->ReadPixels(0, 0, 1, 1, corner);
    EXPECT_GT(corner[1], 200) << "corner should remain green after clear";
    EXPECT_LT(corner[0], 50) << "corner should not be red";
}
