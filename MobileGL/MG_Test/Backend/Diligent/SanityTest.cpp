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
#include <MG_Impl/GLImpl/Buffer/GL_Buffer.h>
#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Impl/GLImpl/RenderState/GL_RenderState.h>
#include <MG_Impl/GLImpl/VertexArray/GL_VertexArray.h>
#include <MG_State/GLState/Core.h>
#include <Init.h>

using namespace MobileGL;
using namespace MobileGL::MG_Backend;
using namespace MobileGL::MG_Impl::GLImpl;

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

TEST(DiligentVulkanBackend, DrawsFromMobileGLState) {
    MobileGL::Initialize();

    // Build a minimal GL 3.2-style program through the real frontend.
    const char* vsSrc = R"(#version 330 core
layout(location = 0) in vec2 Position;
void main() { gl_Position = vec4(Position, 0.0, 1.0); }
)";
    const char* fsSrc = R"(#version 330 core
out vec4 Color;
void main() { Color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

    const GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, nullptr);
    CompileShader(vs);

    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, nullptr);
    CompileShader(fs);

    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    UseProgram(program);
    Viewport(0, 0, 256, 256);

    // Upload a triangle into a real GL buffer and describe it through a VAO.
    const float vertices[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
         0.0f,  0.5f,
    };
    GLuint vbo = 0;
    GenBuffers(1, &vbo);
    BindBuffer(GL_ARRAY_BUFFER, vbo);
    BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint vao = 0;
    GenVertexArrays(1, &vao);
    BindVertexArray(vao);
    EnableVertexAttribArray(0);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();
    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping state-driven test";
    }

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, 0, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red from state-driven triangle";
    EXPECT_LT(center[1], 50) << "center should not be green";

    std::uint8_t corner[4] = {};
    renderer->ReadPixels(0, 0, 1, 1, corner);
    EXPECT_GT(corner[1], 200) << "corner should remain green after clear";
    EXPECT_LT(corner[0], 50) << "corner should not be red";
}
