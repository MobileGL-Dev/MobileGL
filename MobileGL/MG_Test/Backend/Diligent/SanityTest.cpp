// MobileGL - MobileGL/MG_Test/Backend/Diligent/SanityTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only

#include <gtest/gtest.h>
#include <MG_Backend/BackendObject.h>
#include <MG_Backend/Diligent/BackendObject_Diligent.h>
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
