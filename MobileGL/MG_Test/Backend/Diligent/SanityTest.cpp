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
#include <MG_Impl/GLImpl/Texture/GL_Texture.h>
#include <MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h>
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

TEST(DiligentVulkanBackend, DrawsTexturedFromMobileGLState) {
    MobileGL::Initialize();

    const char* vsSrc = R"(#version 330 core
layout(location = 0) in vec2 Position;
layout(location = 1) in vec2 UV;
out vec2 vUV;
void main() { gl_Position = vec4(Position, 0.0, 1.0); vUV = UV; }
)";
    const char* fsSrc = R"(#version 330 core
uniform sampler2D g_Texture;
in vec2 vUV;
out vec4 Color;
void main() { Color = texture(g_Texture, vUV); }
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

    // Position + UV interleaved.
    const float vertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f, -0.5f, 1.0f, 0.0f,
         0.0f,  0.5f, 0.5f, 1.0f,
    };
    GLuint vbo = 0;
    GenBuffers(1, &vbo);
    BindBuffer(GL_ARRAY_BUFFER, vbo);
    BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint vao = 0;
    GenVertexArrays(1, &vao);
    BindVertexArray(vao);
    EnableVertexAttribArray(0);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    EnableVertexAttribArray(1);
    VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));

    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();
    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping textured state test";
    }

    // 2x2 solid red texture.
    const std::uint8_t redTexture[2 * 2 * 4] = {
        255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255,
    };
    ASSERT_TRUE(renderer->CreateTestTexture(redTexture, 2, 2));

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, 0, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red from textured triangle";
    EXPECT_LT(center[1], 50) << "center should not be green";
}

TEST(DiligentVulkanBackend, DrawsIndexedFromMobileGLState) {
    MobileGL::Initialize();

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

    const float vertices[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
         0.0f,  0.5f,
    };
    const GLuint indices[] = {0, 1, 2};

    GLuint vbo = 0;
    GenBuffers(1, &vbo);
    BindBuffer(GL_ARRAY_BUFFER, vbo);
    BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint ebo = 0;
    GenBuffers(1, &ebo);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    GLuint vao = 0;
    GenVertexArrays(1, &vao);
    BindVertexArray(vao);
    EnableVertexAttribArray(0);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();
    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping indexed state test";
    }

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, GL_UNSIGNED_INT, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red from indexed triangle";
    EXPECT_LT(center[1], 50) << "center should not be green";
}

TEST(DiligentVulkanBackend, DrawsRealTexturedFromMobileGLState) {
    MobileGL::Initialize();

    const char* vsSrc = R"(#version 330 core
layout(location = 0) in vec2 Position;
layout(location = 1) in vec2 UV;
out vec2 vUV;
void main() { gl_Position = vec4(Position, 0.0, 1.0); vUV = UV; }
)";
    const char* fsSrc = R"(#version 330 core
uniform sampler2D g_Texture;
in vec2 vUV;
out vec4 Color;
void main() { Color = texture(g_Texture, vUV); }
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

    const float vertices[] = {
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f, -0.5f, 1.0f, 0.0f,
         0.0f,  0.5f, 0.5f, 1.0f,
    };
    GLuint vbo = 0;
    GenBuffers(1, &vbo);
    BindBuffer(GL_ARRAY_BUFFER, vbo);
    BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLuint vao = 0;
    GenVertexArrays(1, &vao);
    BindVertexArray(vao);
    EnableVertexAttribArray(0);
    VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    EnableVertexAttribArray(1);
    VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));

    // Create a real front-end texture object instead of relying on the backend's
    // test helper. This exercises the automatic TextureObject -> Diligent sync.
    const std::uint8_t redTexture[2 * 2 * 4] = {
        255, 0, 0, 255, 255, 0, 0, 255,
        255, 0, 0, 255, 255, 0, 0, 255,
    };
    GLuint tex = 0;
    GenTextures(1, &tex);
    BindTexture(GL_TEXTURE_2D, tex);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, redTexture);
    // A single-level 2x2 texture is incomplete for the default mipmapped min filter,
    // so choose a non-mipmapped filter to exercise the real TextureObject sync path.
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();
    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping real-texture state test";
    }

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, 0, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red from state texture";
    EXPECT_LT(center[1], 50) << "center should not be green";
}

TEST(DiligentVulkanBackend, DrawsUniformFromMobileGLState) {
    MobileGL::Initialize();

    const char* vsSrc = R"(#version 330 core
layout(location = 0) in vec2 Position;
void main() { gl_Position = vec4(Position, 0.0, 1.0); }
)";
    const char* fsSrc = R"(#version 330 core
uniform vec4 u_color;
out vec4 Color;
void main() { Color = u_color; }
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

    const GLint colorLoc = GetUniformLocation(program, "u_color");
    ASSERT_GE(colorLoc, 0);
    Uniform4f(colorLoc, 0.0f, 0.0f, 1.0f, 1.0f);

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
        GTEST_SKIP() << "No Vulkan adapter available; skipping uniform state test";
    }

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, 0, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_LT(center[0], 50) << "center should not be red";
    EXPECT_LT(center[1], 50) << "center should not be green";
    EXPECT_GT(center[2], 200) << "center should be blue from uniform";
}


TEST(DiligentVulkanBackend, DrawsToOffscreenFramebufferFromMobileGLState) {
    MobileGL::Initialize();

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

    // 256x256 color texture attached to a user framebuffer.
    GLuint tex = 0;
    GenTextures(1, &tex);
    BindTexture(GL_TEXTURE_2D, tex);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    GLuint fbo = 0;
    GenFramebuffers(1, &fbo);
    BindFramebuffer(GL_FRAMEBUFFER, fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    DiligentBackend::BackendObject_Diligent backend;
    backend.Initialize();
    auto* renderer = backend.GetRenderer();
    if (renderer == nullptr) {
        GTEST_SKIP() << "No Vulkan adapter available; skipping framebuffer state test";
    }

    renderer->Clear(0.0f, 1.0f, 0.0f, 1.0f);
    renderer->DrawFromState(GL_TRIANGLES, 0, 3, 0, nullptr);
    renderer->Present();

    std::uint8_t center[4] = {};
    renderer->ReadPixels(128, 128, 1, 1, center);
    EXPECT_GT(center[0], 200) << "center should be red in the user framebuffer";
    EXPECT_LT(center[1], 50) << "center should not be green";

    std::uint8_t corner[4] = {};
    renderer->ReadPixels(0, 0, 1, 1, corner);
    EXPECT_GT(corner[1], 200) << "corner should remain green after clear";
    EXPECT_LT(corner[0], 50) << "corner should not be red";
}
