// f1 split-only pixel controls: every result depends on the migrated verb.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <utility>
#if !defined(_WIN32) && GTEST_HAS_DEATH_TEST
#include <unistd.h>
#endif
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {
constexpr const char* kWireVertexIdTriangle = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";

GLuint BuildWireProgram(std::initializer_list<std::pair<GLenum, const char*>> sources) {
    const GLuint program = glCreateProgram();
    for (const auto& stage : sources) {
        const GLuint shader = glCreateShader(stage.first);
        glShaderSource(shader, 1, &stage.second, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[4096]{};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            ADD_FAILURE() << "wire pixel shader compilation: " << log;
            glDeleteShader(shader);
            glDeleteProgram(program);
            return 0;
        }
        glAttachShader(program, shader);
        glDeleteShader(shader);
    }
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        ADD_FAILURE() << "wire pixel program link: " << log;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

class F1WireScenario : public ScenarioTest {
protected:
    GLuint fbo = 0, texture = 0;
    void SetUp() override {
        ScenarioTest::SetUp();
        if (!Ready()) return;
        const auto why = SplitRuntimeSkipReason();
        if (!why.empty()) GTEST_SKIP() << why;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glStencilMask(~0u);
    }
    void TearDown() override {
        if (!Ready()) return;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (texture) glDeleteTextures(1, &texture);
        ScenarioTest::TearDown();
    }
    void Attach(GLenum format, GLenum attachment = GL_COLOR_ATTACHMENT0, int levels = 1) {
        glTexStorage2D(GL_TEXTURE_2D, levels, format, 8, 8);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, 0);
        if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); }
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE)) << "F1.setup.framebuffer";
    }
};
}

TEST_F(F1WireScenario, ClearBufferfvPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearBufferfv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA8);
    const GLfloat value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearBufferfv(GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearBufferfv.wire";
    GLubyte pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearBufferfv.error";
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "F1.ClearBufferfv.pixels";
}

TEST_F(F1WireScenario, ClearNamedFramebufferfvPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearNamedFramebufferfv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA8);
    const GLfloat value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearNamedFramebufferfv(fbo, GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearNamedFramebufferfv.wire";
    GLubyte pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearNamedFramebufferfv.error";
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "F1.ClearNamedFramebufferfv.pixels";
}

TEST_F(F1WireScenario, ClearBufferivPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearBufferiv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA32I);
    const GLint value[4] = {-37, 19, -11, 5};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearBufferiv(GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearBufferiv.wire";
    GLint pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA_INTEGER, GL_INT, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearBufferiv.error";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(pixel[i], value[i]) << "F1.ClearBufferiv.pixels";
}

TEST_F(F1WireScenario, ClearNamedFramebufferivPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearNamedFramebufferiv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA32I);
    const GLint value[4] = {-37, 19, -11, 5};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearNamedFramebufferiv(fbo, GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearNamedFramebufferiv.wire";
    GLint pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA_INTEGER, GL_INT, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearNamedFramebufferiv.error";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(pixel[i], value[i]) << "F1.ClearNamedFramebufferiv.pixels";
}

TEST_F(F1WireScenario, ClearBufferuivPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearBufferuiv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA32UI);
    const GLuint value[4] = {37, 19, 11, 5};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearBufferuiv(GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearBufferuiv.wire";
    GLuint pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearBufferuiv.error";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(pixel[i], value[i]) << "F1.ClearBufferuiv.pixels";
}

TEST_F(F1WireScenario, ClearNamedFramebufferuivPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearNamedFramebufferuiv.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA32UI);
    const GLuint value[4] = {37, 19, 11, 5};
    const auto before = PeekSplitRuntime().emitSeq;
    glClearNamedFramebufferuiv(fbo, GL_COLOR, 0, value);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearNamedFramebufferuiv.wire";
    GLuint pixel[4]{};
    glReadPixels(2, 3, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearNamedFramebufferuiv.error";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(pixel[i], value[i]) << "F1.ClearNamedFramebufferuiv.pixels";
}

TEST_F(F1WireScenario, ClearBufferfiPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearBufferfi.pixels fails.
    if (!Ready()) return;
    Attach(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT);
    const auto before = PeekSplitRuntime().emitSeq;
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 0.375f, 91);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearBufferfi.wire";
    GLuint pixel = 0;
    glReadPixels(2, 3, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, &pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearBufferfi.error";
    EXPECT_EQ(pixel & 255u, 91u) << "F1.ClearBufferfi.pixels";
    EXPECT_NEAR(double(pixel >> 8) / 16777215.0, 0.375, 0.00001) << "F1.ClearBufferfi.pixels";
}

TEST_F(F1WireScenario, ClearNamedFramebufferfiPixels) {
    // Red once (executed, reverted): zero the clear record values; F1.ClearNamedFramebufferfi.pixels fails.
    if (!Ready()) return;
    Attach(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT);
    const auto before = PeekSplitRuntime().emitSeq;
    glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, 0.375f, 91);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ClearNamedFramebufferfi.wire";
    GLuint pixel = 0;
    glReadPixels(2, 3, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, &pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ClearNamedFramebufferfi.error";
    EXPECT_EQ(pixel & 255u, 91u) << "F1.ClearNamedFramebufferfi.pixels";
    EXPECT_NEAR(double(pixel >> 8) / 16777215.0, 0.375, 0.00001) << "F1.ClearNamedFramebufferfi.pixels";
}

TEST_F(F1WireScenario, CopyTexImage2DPixels) {
    // Red once (executed, reverted): omit the copy sink call; F1.CopyTexImage2D.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint destination = 0;
    glGenTextures(1, &destination);
    glBindTexture(GL_TEXTURE_2D, destination);

    const auto before = PeekSplitRuntime().emitSeq;
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 3, 4, 4, 0);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.CopyTexImage2D.wire";
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    GLubyte pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyTexImage2D.error";
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "F1.CopyTexImage2D.pixels";
    glDeleteTextures(1, &destination);
}

TEST_F(F1WireScenario, CopyTexSubImage2DPixels) {
    // Red once (executed, reverted): omit the copy sink call; F1.CopyTexSubImage2D.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint destination = 0;
    glGenTextures(1, &destination);
    glBindTexture(GL_TEXTURE_2D, destination);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    const auto before = PeekSplitRuntime().emitSeq;
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 2, 3, 2, 2);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.CopyTexSubImage2D.wire";
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
    GLubyte pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyTexSubImage2D.error";
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "F1.CopyTexSubImage2D.pixels";
    glDeleteTextures(1, &destination);
}

TEST_F(F1WireScenario, GenerateMipmapPixels) {
    // Red once (executed, reverted): omit the mipmap sink call; F1.GenerateMipmap.pixels fails.
    if (!Ready()) return;
    Attach(GL_RGBA8, GL_COLOR_ATTACHMENT0, 4);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto before = PeekSplitRuntime().emitSeq;
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.GenerateMipmap.wire";
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 2);
    GLubyte pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.GenerateMipmap.error";
    const int expected[4] = {64, 128, 191, 255};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 1) << "F1.GenerateMipmap.pixels";
}
TEST_F(F1WireScenario, GenerateMipmapPackedFloatPixels) {
    if (!Ready()) return;
    // Only level zero exists initially. Generating the special-format chain must use the
    // shape published by the client, and level two must contain the generated GPU pixels.
    std::array<GLfloat, 8 * 8 * 3> source{};
    for (size_t i = 0; i < source.size(); i += 3) {
        source[i] = 0.25f; source[i + 1] = 0.5f; source[i + 2] = 0.75f;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, 8, 8, 0, GL_RGB, GL_FLOAT, source.data());
    const auto before = PeekSplitRuntime().emitSeq;
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 2);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    GLfloat pixel[4]{};
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_FLOAT, pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    const GLfloat expected[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(pixel[i], expected[i], 0.01f);
}

TEST_F(F1WireScenario, GenerateMipmapDepthPixels) {
    if (!Ready()) return;
    std::array<GLfloat, 8 * 8> source{};
    source.fill(0.375f);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 8, 8, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, source.data());
    const auto before = PeekSplitRuntime().emitSeq;
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 2);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    GLfloat pixel = 0;
    glReadPixels(1, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &pixel);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    EXPECT_NEAR(pixel, 0.375f, 0.00001f);
}

TEST_F(F1WireScenario, VertexIdSamplerAndScalarUniformPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    const char* fragment = R"(#version 430 core
uniform sampler2D sourceTexture;
uniform float scale;
layout(location=0) out vec4 color;
void main() { color = vec4(texture(sourceTexture, vec2(0.5)).rgb * scale, 1.0); }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLuint vao = 0, source = 0, sampler = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao); // No attributes, VBO, UBO or SSBO participates in either draw.
    glGenTextures(1, &source);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, source);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    const std::array<GLubyte, 4> texel{64, 128, 192, 255};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindSampler(3, sampler);
    glUseProgram(program);
    const GLint sourceLocation = glGetUniformLocation(program, "sourceTexture");
    const GLint scaleLocation = glGetUniformLocation(program, "scale");
    ASSERT_GE(sourceLocation, 0);
    ASSERT_GE(scaleLocation, 0);
    glUniform1i(sourceLocation, 3);
    glViewport(0, 0, 8, 8);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_FRAMEBUFFER_SRGB);
    for (const GLfloat scale : {0.5f, 0.25f}) {
        glUniform1f(scaleLocation, scale); // Post-link changes must come from GlobalConstants.
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        for (int channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(pixel[channel], texel[channel] * scale, 1) << "scalar uniform " << scale;
        EXPECT_EQ(pixel[3], 255);
    }
    glUseProgram(0);
    glBindVertexArray(0);
    glBindSampler(3, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glDeleteSamplers(1, &sampler);
    glDeleteTextures(1, &source);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, ComputeImageStoreFramebufferPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const char* compute = R"(#version 430 core
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(rgba8, binding=2) writeonly uniform image2D destination;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    imageStore(destination, p, vec4(vec2(p + ivec2(1)) / 8.0, 0.5, 1.0));
}
)";
    const GLuint program = BuildWireProgram({{GL_COMPUTE_SHADER, compute}});
    ASSERT_NE(program, 0u);
    glBindImageTexture(2, texture, 0, GL_FALSE, 7, GL_WRITE_ONLY, GL_RGBA8); // 2D ignores the layer.
    glUseProgram(program);
    glDispatchCompute(8, 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    // Read through an FBO so the assertion does not depend on the class-C GetTexImage path.
    std::array<GLubyte, 8 * 8 * 4> pixels{};
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const size_t at = static_cast<size_t>(y * 8 + x) * 4;
            EXPECT_NEAR(pixels[at], (x + 1) * 255.0 / 8.0, 1) << x << ", " << y;
            EXPECT_NEAR(pixels[at + 1], (y + 1) * 255.0 / 8.0, 1) << x << ", " << y;
            EXPECT_NEAR(pixels[at + 2], 128, 1);
            EXPECT_EQ(pixels[at + 3], 255);
        }
    }
    glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(0);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, EnabledVertexBufferDrawKeepsNamedP7Fatal) {
    if (!Ready()) return;
#if !defined(_WIN32) && GTEST_HAS_DEATH_TEST
    Attach(GL_RGBA8);
    const char* vertex = R"(#version 430 core
layout(location=0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";
    const char* fragment = R"(#version 430 core
layout(location=0) out vec4 color;
void main() { color = vec4(0.0, 1.0, 0.0, 1.0); }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLfloat positions[] = {-1, -1, 3, -1, -1, 3};
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glUseProgram(program);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

    // A fresh exec reconstructs the fixture and apply thread; no child executes GL
    // against a forked mutex/thread state. Capture the named log because shipped
    // builds may disable console logging, which makes a stderr regex insufficient.
    const char* oldLogEnv = std::getenv("MOBILEGL_LOG_FILE_PATH");
    const bool hadLogEnv = oldLogEnv != nullptr;
    const std::string oldLogPath = hadLogEnv ? oldLogEnv : "";
    const std::string childLog = oldLogPath.empty()
        ? "/tmp/mobilegl-vbo-death-" + std::to_string(static_cast<long long>(::getpid())) + ".log"
        : oldLogPath + ".vbo-death";
    std::remove(childLog.c_str());
    ASSERT_EQ(::setenv("MOBILEGL_LOG_FILE_PATH", childLog.c_str(), 1), 0);
    const std::string oldStyle = ::testing::FLAGS_gtest_death_test_style;
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH({
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
    }, "");
    ::testing::FLAGS_gtest_death_test_style = oldStyle;
    if (hadLogEnv) ::setenv("MOBILEGL_LOG_FILE_PATH", oldLogPath.c_str(), 1);
    else ::unsetenv("MOBILEGL_LOG_FILE_PATH");
    std::ifstream log(childLog, std::ios::binary);
    std::ostringstream contents;
    contents << log.rdbuf();
    log.close();
    EXPECT_NE(contents.str().find("buffer-legacy-arm"), std::string::npos)
        << "draw must keep the existing named P7 refusal, not die at a new object/field access\n"
        << contents.str();
    std::remove(childLog.c_str());
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
#else
    GTEST_SKIP() << "requires gtest thread-safe exec death tests and POSIX environment handling";
#endif
}

TEST_F(F1WireScenario, SrgbDrawTracksFramebufferConversion) {
    if (!Ready()) return;
    Attach(GL_SRGB8_ALPHA8);
    const char* fragment = R"(#version 430 core
layout(location=0) out vec4 color;
void main() { color = vec4(0.5, 0.5, 0.5, 1.0); }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(0, 0, 8, 8);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_RASTERIZER_DISCARD);
    for (const bool conversion : {false, true}) {
        if (conversion) glEnable(GL_FRAMEBUFFER_SRGB);
        else glDisable(GL_FRAMEBUFFER_SRGB);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        for (int channel = 0; channel < 3; ++channel)
            EXPECT_NEAR(pixel[channel], conversion ? 188 : 128, 1) << "FRAMEBUFFER_SRGB=" << conversion;
        EXPECT_EQ(pixel[3], 255);
    }
    glDisable(GL_FRAMEBUFFER_SRGB);
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, Texture1DFramebufferClearPixels) {
    if (!Ready()) return;
    GLuint line = 0;
    glGenTextures(1, &line);
    glBindTexture(GL_TEXTURE_1D, line);
    glTexStorage1D(GL_TEXTURE_1D, 1, GL_RGBA8, 8);
    glFramebufferTexture1D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_1D, line, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    std::array<GLubyte, 8 * 4> pixels{};
    glReadPixels(0, 0, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    const int expected[] = {64, 128, 191, 255};
    for (int x = 0; x < 8; ++x)
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(pixels[x * 4 + channel], expected[channel], 1) << "1D texel " << x;
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0);
    glBindTexture(GL_TEXTURE_1D, 0);
    glDeleteTextures(1, &line);
}

TEST_F(F1WireScenario, PartialTextureUploadPreservesGpuClearPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    // The client shadow is red. The GPU then changes every texel to green, so
    // applying a later one-pixel upload as a whole shadow loses observable data.
    std::array<GLubyte, 8 * 8 * 4> red{};
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const std::array<GLubyte, 4> blue{0, 0, 255, 255};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());

    std::array<GLubyte, 8 * 8 * 4> pixels{};
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::array<GLubyte, 4> expected = x == 1 && y == 2
                ? blue : std::array<GLubyte, 4>{0, 255, 0, 255};
            const size_t at = static_cast<size_t>(y * 8 + x) * 4;
            const std::array<GLubyte, 4> actual{pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
            EXPECT_EQ(actual, expected) << "GPU clear survived outside upload at " << x << ", " << y;
        }
    }
}

TEST_F(F1WireScenario, TextureViewClearTargetsItsRootMipAndLayer) {
    if (!Ready()) return;
    GLuint root = 0, view = 0;
    glGenTextures(1, &root);
    glBindTexture(GL_TEXTURE_2D_ARRAY, root);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 2, GL_RGBA8, 8, 8, 2);
    std::array<GLubyte, 8 * 8 * 2 * 4> red{};
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 8, 8, 2,
                    GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 4, 4, 2,
                    GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    std::array<GLubyte, 4 * 4 * 4> blue{};
    for (size_t i = 0; i < blue.size(); i += 4) {
        blue[i + 2] = 255;
        blue[i + 3] = 255;
    }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 4, 4, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D, root, GL_RGBA8, 1, 1, 1, 1);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "view construction";
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    std::array<GLubyte, 4> pixel{};
    glReadPixels(1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 0, 255})) << "view reads its cleared window";
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 1, 1);
    glReadPixels(1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 0, 255})) << "view clear reaches root mip 1, layer 1";
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 1, 0);
    glReadPixels(1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 0, 255, 255})) << "the neighboring root layer is preserved";
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 0, 1);
    glReadPixels(1, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{255, 0, 0, 255})) << "the neighboring root mip is preserved";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0);
    glDeleteTextures(1, &view);
    glDeleteTextures(1, &root);
}

TEST_F(F1WireScenario, NamedBlitPreservesBindingsAndRestoresNextVerbsPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint fbos[3]{}, textures[3]{};
    glGenFramebuffers(3, fbos);
    glGenTextures(3, textures);
    for (int i = 0; i < 3; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 8, 8);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        glClearColor(0, i == 1 ? 1 : 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[2]);
    const auto before = PeekSplitRuntime().emitSeq;
    glBlitNamedFramebuffer(fbo, fbos[0], 0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    EXPECT_GT(PeekSplitRuntime().emitSeq, before);
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    EXPECT_EQ(read, GLint(fbos[1]));
    EXPECT_EQ(draw, GLint(fbos[2]));

    // No intervening bind: this must clear the restored draw FBO, not the DSA destination.
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[2]);
    std::array<GLubyte, 4> pixel{};
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{255, 0, 255, 255})) << "named blit restored draw before clear";
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{255, 0, 0, 255})) << "unbound named blit copied source";

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]);
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[2]);
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 255, 255})) << "ordinary blit follows restored bindings";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDeleteFramebuffers(3, fbos);
    glDeleteTextures(3, textures);
}

TEST_F(F1WireScenario, NamedBlitDefaultEndpointPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBlitNamedFramebuffer(fbo, 0, 0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLint read = 0, draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    EXPECT_EQ(read, GLint(fbo));
    EXPECT_EQ(draw, GLint(fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    std::array<GLubyte, 4> pixel{};
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{255, 0, 0, 255})) << "default draw endpoint";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitNamedFramebuffer(0, fbo, 0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    EXPECT_EQ(read, GLint(fbo));
    EXPECT_EQ(draw, GLint(fbo));
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{255, 0, 0, 255})) << "default read endpoint";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
}
} // namespace MGITest
