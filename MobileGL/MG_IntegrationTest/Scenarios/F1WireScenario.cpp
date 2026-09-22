// f1 split-only pixel controls: every result depends on the migrated verb.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/SplitLane.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
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
    void PaintNonSquareBlitSource() {
        // GL coordinates: red/green on the bottom, blue/yellow on the top.
        // A non-square source makes accidental extent/axis interchange visible.
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 6, 4);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        constexpr GLfloat colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
        glEnable(GL_SCISSOR_TEST);
        for (int quadrant = 0; quadrant < 4; ++quadrant) {
            glScissor((quadrant % 2) * 3, (quadrant / 2) * 2, 3, 2);
            glClearColor(colors[quadrant][0], colors[quadrant][1], colors[quadrant][2], colors[quadrant][3]);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "paint non-square source FBO";
    }
    std::array<GLubyte, 4> ReadOnePixel(int x, int y) {
        std::array<GLubyte, 4> pixel{31, 47, 63, 79};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return pixel;
    }

};
}

TEST_F(F1WireScenario, TextureReadbackLargerThanAReplySlotContainsGpuWrites) {
    if (!Ready()) return;
    constexpr int width = 1024, height = 600;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, height / 2, width, height / 2);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    std::vector<GLubyte> pixels(width * height * 4, 0x5a);
    glGetTextureImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(pixels.size()), pixels.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    for (int y = 0; y < height; ++y) {
        for (int x : {0, width / 2, width - 1}) {
            const size_t offset = (size_t(y) * width + x) * 4;
            const std::array<GLubyte, 4> expected = y < height / 2
                ? std::array<GLubyte, 4>{255, 0, 0, 255} : std::array<GLubyte, 4>{0, 0, 255, 255};
            EXPECT_TRUE(std::equal(expected.begin(), expected.end(), pixels.begin() + offset)) << x << "," << y;
        }
    }
}

TEST_F(F1WireScenario, TextureReadbackRejectsPackedDestinationOverflowBeforeWriting) {
    if (!Ready()) return;
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    std::array<GLubyte, 8> actual{1, 2, 3, 4, 5, 6, 7, 8};
    const auto sentinel = actual;
    glGetTextureImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, 4, actual.data());
    EXPECT_EQ(glGetError(), GLenum(GL_INVALID_OPERATION));
    EXPECT_EQ(actual, sentinel);
    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, 4, actual.data(), GL_DYNAMIC_READ);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(glGetError(), GLenum(GL_INVALID_OPERATION));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    EXPECT_EQ(glGetError(), GLenum(GL_INVALID_OPERATION));
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, 4, actual.data());
    EXPECT_EQ(actual, sentinel);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);
}

TEST_F(F1WireScenario, TextureAndFramebufferReadsPreservePackBufferPadding) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    constexpr size_t size = 512, offset = 12, stride = 40;
    std::vector<GLubyte> expected(size, 0x5a), actual(size);
    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, size, expected.data(), GL_DYNAMIC_READ);
    glPixelStorei(GL_PACK_ROW_LENGTH, 10);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    for (int mode = 0; mode < 2; ++mode) {
        glBufferSubData(GL_PIXEL_PACK_BUFFER, 0, size, expected.data());
        if (mode == 0) glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(offset));
        else glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(offset));
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, size, actual.data());
        auto wanted = expected;
        for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 8; ++x) {
            const size_t at = offset + stride + 4 + y * stride + x * 4;
            wanted[at] = 0; wanted[at + 1] = 255; wanted[at + 2] = 0; wanted[at + 3] = 255;
        }
        EXPECT_EQ(actual, wanted) << "read API " << mode;
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);
}

// P5f exit: each published frame must have a real draw and server stamp, and
// zero residual reads. Missing instrumentation is a failure rather than a false zero.
TEST_F(F1WireScenario, EachWireFrameHasZeroResidualPulls) {
    if (!Ready()) return;
    if (!SplitLane::MarkerIsOne("MGITEST_P5F_RSP_LANE"))
        GTEST_SKIP() << "requires the dedicated per-frame stats lane";
    ASSERT_FALSE(PipeStatsWindow::LibraryLogPath().empty());
    Attach(GL_RGBA8);
    const char* fragment = R"(#version 430 core
uniform float value;
layout(location=0) out vec4 color;
void main() { color = vec4(value, 0.25, 0.75, 1.0); }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    const GLint location = glGetUniformLocation(program, "value");
    ASSERT_GE(location, 0);
    glViewport(0, 0, 8, 8);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_RASTERIZER_DISCARD);
    Gl().EndFrame(); // close setup's counter window
    for (int frame = 0; frame < 3; ++frame) {
        const float value = float(frame + 1) / 4.0f;
        glUniform1f(location, value);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(3, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        EXPECT_NEAR(pixel[0], value * 255.0f, 1) << "frame " << frame;
        EXPECT_NEAR(pixel[1], 64, 1);
        EXPECT_NEAR(pixel[2], 191, 1);
        EXPECT_EQ(pixel[3], 255);
        Gl().EndFrame();
        // Present returns before apply under run-ahead. Inspect this completed
        // frame's window, not the previous (possibly empty setup) log line.
        ASSERT_TRUE(WaitForSplitAppliedForTesting(PeekSplitRuntime().emitSeq));
        const auto window = PipeStatsWindow::LastFromLaneLog();
        ASSERT_TRUE(window.found) << "P5f rsp window missing on frame " << frame;
        // Magma does not publish Espryt's draw counter. The changing uniform and
        // checked pixel above prove each draw executed; a missing stats field still fails.
        EXPECT_GE(PipeStatsWindow::CounterOrAbsent(window, "draws"), 0) << window.line;
        EXPECT_GT(PipeStatsWindow::CounterOrAbsent(window, "vbs"), 0) << window.line;
        EXPECT_EQ(PipeStatsWindow::CounterOrAbsent(window, "rsp"), 0)
            << "P5f residual pull on frame " << frame << ": " << window.line;
        RecordProperty("p5f_frame_" + std::to_string(frame), window.line);
    }
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
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

    // Change the base on the GPU after its client upload. The regenerated mip
    // must read those new pixels, and offscreen shader blits must keep row order.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.875f, 0.375f, 0.125f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 4, 8, 4);
    glClearColor(0.125f, 0.75f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glGenerateMipmap(GL_TEXTURE_2D);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 2);
    GLfloat rows[8]{};
    glReadPixels(1, 0, 1, 2, GL_RGBA, GL_FLOAT, rows);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    const GLfloat generated[8] = {0.875f, 0.375f, 0.125f, 1.0f, 0.125f, 0.75f, 0.5f, 1.0f};
    for (int i = 0; i < 8; ++i) EXPECT_NEAR(rows[i], generated[i], 0.01f);
}

// P7 wave 2-B: THE ARM THE HOST CANNOT REACH ON ITS OWN.
//
// Two Iris cases (minecraft-1.21.4-fabric-iris-iterationt-in-world and its -nodsa twin) killed
// the server apply thread on a Redmi (Adreno 830v2) with
//     MGPipe: Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape"}
// deterministically, under both run-ahead and lockstep, while the monolith arm passed and the
// host lane stayed green. It stayed green because lavapipe reports BLIT_SRC|BLIT_DST for every
// colour format this scenario can create: `nativeBlit` is always true here, so NEITHER shader
// arm of GenerateWireMipmap has ever executed on the gate machine. A refusal no host lane can
// reach is a refusal that ships.
//
// MGITEST_MAGMA_FORCE_SHADER_MIPMAP is what makes them reachable, and it selects which one:
//   1 - drop the native blit only. The destination level is still a colour attachment, so the
//       pass renders straight into it (the arm an Adreno takes for a format without BLIT_DST).
//   2 - also treat the destination as not attachable. The pass renders into owned scratch and
//       copies the mip in (the arm it takes for a texture whose image never got
//       COLOR_ATTACHMENT usage - which, before this package, was the refusal itself).
//
// The assertion is the ORDINARY mip result, per half: a shader mip that renders somewhere else
// is still wrong unless the level holds what a blit would have put there, in GL's row order.
// Red once (executed, reverted): drop the scratch arm (force `viaScratch` false) and the tier-2
// entry dies with Fatal{UnmigratedVerb, "Magma:mipmap-shader-format-or-shape ..."} by name.
TEST_F(F1WireScenario, GenerateMipmapWithoutNativeBlitPixels) {
    if (!Ready()) return;
    const std::string tier = SplitLane::MarkerValue("MGITEST_MAGMA_FORCE_SHADER_MIPMAP");
    if (tier.empty())
        GTEST_SKIP() << "MGITEST_MAGMA_FORCE_SHADER_MIPMAP is unset: this driver reports "
                        "BLIT_SRC|BLIT_DST for GL_RGBA8, so the shader mip arms under test are "
                        "not entered and this case would assert the native blit twice over";
    Attach(GL_RGBA8, GL_COLOR_ATTACHMENT0, 4);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.75f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // Two different halves, not one flat colour: a pass that loses GL's row order averages to
    // the same number everywhere and a single-colour assertion would call that correct.
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 8, 4);
    glClearColor(0.25f, 0.75f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    const auto before = PeekSplitRuntime().emitSeq;
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.ShaderMip.wire";
    // Level one is 4x4, and each of its rows averages two source rows from ONE half - the
    // boundary never falls inside a destination texel, so the two colours survive unmixed.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 1);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    GLubyte rows[4][4]{};
    glReadPixels(1, 0, 1, 4, GL_RGBA, GL_UNSIGNED_BYTE, rows);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.ShaderMip.error";
    const int bottom[4] = {64, 191, 128, 255};
    const int top[4] = {191, 64, 128, 255};
    for (int row = 0; row < 4; ++row) {
        const int* expected = row < 2 ? bottom : top;
        for (int channel = 0; channel < 4; ++channel)
            EXPECT_NEAR(rows[row][channel], expected[channel], 2)
                << "F1.ShaderMip.pixels tier=" << tier << " row=" << row << " channel=" << channel;
    }
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

// P7 wave 2-B, CONTRACT-P7 §5.1 (A): the baked depth mip program, on the two GL depth formats
// that reach it as a depth-only Vulkan aspect (DEPTH_COMPONENT24 -> X8_D24_UNORM_PACK32,
// DEPTH_COMPONENT32F -> D32_SFLOAT). GenerateMipmapDepthPixels above passes on this host
// because lavapipe blits depth natively; the knob removes that arm, which is what an Adreno
// does by itself - VK_FORMAT_FEATURE_BLIT_DST is OPTIONAL for depth formats, so a depth chain
// through a shader is the ordinary case and not the exotic one.
//
// TWO HALVES AND A BAND THAT IS NOT ASSERTED. The generated level is read back through a real
// sampled attachment, so a flat fill would prove nothing: the source is split at its middle
// row instead. The ported filter takes a 2x2 box at ivec2(texCoord * srcTexelSize), which
// lands half a texel off the destination centre - the monolith's own arithmetic, kept
// deliberately (§3.2 retires a refusal by matching that arm, defects included). Under either
// reading of that half texel, destination rows 0..2 come only from the lower half and rows
// 4..7 only from the upper half; row 3 is the straddle and is the one row this case does not
// pin, because pinning it would assert the half-texel offset rather than the mip.
TEST_F(F1WireScenario, GenerateMipmapDepthWithoutNativeBlitPixels) {
    if (!Ready()) return;
    const std::string tier = SplitLane::MarkerValue("MGITEST_MAGMA_FORCE_SHADER_MIPMAP");
    if (tier.empty())
        GTEST_SKIP() << "MGITEST_MAGMA_FORCE_SHADER_MIPMAP is unset: this driver blits depth "
                        "natively, so the baked depth mip program under test is not entered";
    for (const GLenum internalFormat : {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32F}) {
        GLuint depth = 0;
        glGenTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexStorage2D(GL_TEXTURE_2D, 5, internalFormat, 16, 16);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE))
            << "format 0x" << std::hex << internalFormat;
        glDepthMask(GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        glClearDepth(0.75);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 16, 8);
        glClearDepth(0.25);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        const auto before = PeekSplitRuntime().emitSeq;
        glGenerateMipmap(GL_TEXTURE_2D);
        ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.DepthMip.wire";
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 1);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        GLfloat rows[8]{};
        glReadPixels(2, 0, 1, 8, GL_DEPTH_COMPONENT, GL_FLOAT, rows);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.DepthMip.error";
        for (int row = 0; row < 8; ++row) {
            if (row == 3) continue;
            const GLfloat expected = row < 3 ? 0.25f : 0.75f;
            EXPECT_NEAR(rows[row], expected, 0.002f)
                << "F1.DepthMip.pixels tier=" << tier << " format=0x" << std::hex << internalFormat
                << std::dec << " row=" << row;
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glDeleteTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, texture);
    }
    glClearDepth(1.0);
}

// The OTHER half of `depth-stencil-mipmap@P7`, and it is a decline rather than a retirement
// (CONTRACT-P7 §3.2): a combined depth/stencil texture has no mip generation on EITHER arm -
// the monolith's shader fallback asserts depth-only, and an assert is nothing in the release
// build both arms ship as. What this case pins is that the wire arm now says so and carries
// on, rather than taking the session down: no GL error, the session still alive afterwards,
// and level zero still holding what was cleared into it.
//
// No knob: the decline is decided on the ASPECT, before the blit-support question is asked,
// so it fires on every driver.
// Red once (executed, reverted): restore the MagmaWireFatal and this case dies with
// Fatal{UnmigratedVerb, "Magma:depth-stencil-mipmap@P7"}.
TEST_F(F1WireScenario, GenerateMipmapDepthStencilDeclinesAndKeepsTheSession) {
    if (!Ready()) return;
    GLuint depthStencil = 0;
    glGenTextures(1, &depthStencil);
    glBindTexture(GL_TEXTURE_2D, depthStencil);
    glTexStorage2D(GL_TEXTURE_2D, 4, GL_DEPTH24_STENCIL8, 8, 8);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthStencil, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearDepth(0.5);
    glClear(GL_DEPTH_BUFFER_BIT);
    glGenerateMipmap(GL_TEXTURE_2D);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.DepthStencilMip.error";
    // The session is what the retired Fatal used to take with it. A readback is a round trip
    // through the server, so it answers that question and the level-zero one at once.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthStencil, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    GLfloat level0 = 0;
    glReadPixels(1, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &level0);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.DepthStencilMip.readback";
    EXPECT_NEAR(level0, 0.5f, 0.002f) << "F1.DepthStencilMip.level0";
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    glDeleteTextures(1, &depthStencil);
    glBindTexture(GL_TEXTURE_2D, texture);
    glClearDepth(1.0);
}

TEST_F(F1WireScenario, GenerateMipmapHonorsMutableBaseAndMaxWithoutChangingOtherLevels) {
    if (!Ready()) return;
    const std::array<GLubyte, 4> colors[5] = {
        {255, 0, 0, 255}, {0, 255, 255, 255}, {0, 0, 255, 255},
        {255, 255, 0, 255}, {255, 0, 255, 255}};
    for (int level = 0; level < 5; ++level) {
        const int side = 16 >> level;
        std::vector<GLubyte> pixels(static_cast<size_t>(side * side * 4));
        for (size_t at = 0; at < pixels.size(); at += 4)
            std::copy(colors[level].begin(), colors[level].end(), pixels.begin() + at);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 1);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    // The actual base differs from its uploaded shadow. No readback separates
    // this GPU write from generation, and level zero contains a different color.
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    for (int level = 0; level < 5; ++level) {
        const int side = 16 >> level;
        GLint width = 0, height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
        EXPECT_EQ(width, side) << "level " << level;
        EXPECT_EQ(height, side) << "level " << level;
        std::vector<GLubyte> pixels(static_cast<size_t>(side * side * 4), 17);
        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const std::array<GLubyte, 4> expected = level == 1 || level == 2
            ? std::array<GLubyte, 4>{0, 255, 0, 255} : colors[level];
        size_t wrong = 0;
        for (size_t at = 0; at < pixels.size(); at += 4)
            if (!std::equal(expected.begin(), expected.end(), pixels.begin() + at)) ++wrong;
        EXPECT_EQ(wrong, 0u) << "level " << level << " must retain or generate its own pixels";
    }
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
}

TEST_F(F1WireScenario, GenerateMipmapThroughViewKeepsOwnerLevelsAndLayersOutsideItsWindow) {
    if (!Ready()) return;
    GLuint root = 0, view = 0;
    glGenTextures(1, &root);
    glBindTexture(GL_TEXTURE_2D_ARRAY, root);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 5, GL_RGBA8, 16, 16, 4);
    const std::array<GLubyte, 4> colors[5] = {
        {255, 0, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255},
        {255, 0, 255, 255}, {0, 255, 255, 255}};
    for (int level = 0; level < 5; ++level) {
        const int side = 16 >> level;
        std::vector<GLubyte> pixels(static_cast<size_t>(side * side * 4 * 4));
        for (size_t at = 0; at < pixels.size(); at += 4)
            std::copy(colors[level].begin(), colors[level].end(), pixels.begin() + at);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, side, side, 4,
                       GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    glGenTextures(1, &view);
    // Logical levels 0..2 alias owner levels 1..3; only owner layers 1..2 belong
    // to this view. Its nonzero BASE_LEVEL makes owner level 2 the source.
    glTextureView(view, GL_TEXTURE_2D_ARRAY, root, GL_RGBA8, 1, 3, 1, 2);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glTextureParameteri(view, GL_TEXTURE_BASE_LEVEL, 1);
    glTextureParameteri(view, GL_TEXTURE_MAX_LEVEL, 2);
    for (const int layer : {1, 2}) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 2, layer);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glGenerateTextureMipmap(view);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glBindTexture(GL_TEXTURE_2D_ARRAY, root);
    for (int level = 0; level < 5; ++level) {
        const int side = 16 >> level;
        GLint width = 0, height = 0, layers = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, level, GL_TEXTURE_DEPTH, &layers);
        EXPECT_EQ(width, side) << "owner mip " << level;
        EXPECT_EQ(height, side) << "owner mip " << level;
        ASSERT_EQ(layers, 4) << "generation through a two-layer view must not resize its owner's level";
        const size_t layerBytes = static_cast<size_t>(side * side * 4);
        std::vector<GLubyte> pixels(layerBytes * 4, 17);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int layer = 0; layer < 4; ++layer) {
            const bool generated = (level == 2 || level == 3) && (layer == 1 || layer == 2);
            const std::array<GLubyte, 4> expected = generated
                ? std::array<GLubyte, 4>{0, 255, 0, 255} : colors[level];
            size_t wrong = 0;
            for (size_t at = 0; at < layerBytes; at += 4)
                if (!std::equal(expected.begin(), expected.end(), pixels.begin() + layerBytes * layer + at)) ++wrong;
            EXPECT_EQ(wrong, 0u) << "owner mip " << level << ", layer " << layer;
        }
    }
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glDeleteTextures(1, &view);
    glDeleteTextures(1, &root);
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

TEST_F(F1WireScenario, ComputeImageStoreThroughViewPreservesOtherRootLayer) {
    if (!Ready()) return;
    GLuint root = 0, view = 0;
    glGenTextures(1, &root);
    glBindTexture(GL_TEXTURE_2D_ARRAY, root);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 8, 8, 2);
    std::array<GLubyte, 8 * 8 * 2 * 4> red{};
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 8, 8, 2,
                    GL_RGBA, GL_UNSIGNED_BYTE, red.data());
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 0, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    // Materialize the root's Vulkan image before any storage-image binding. The
    // subsequent view-only bind must upgrade the root and preserve its other layer.
    std::array<GLubyte, 4> initial{};
    glReadPixels(2, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, initial.data());
    ASSERT_EQ(initial, (std::array<GLubyte, 4>{255, 0, 0, 255}));
    glGenTextures(1, &view);
    glTextureView(view, GL_TEXTURE_2D, root, GL_RGBA8, 0, 1, 1, 1);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    const char* compute = R"(#version 430 core
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(rgba8, binding=2) writeonly uniform image2D destination;
void main() { imageStore(destination, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 1.0, 0.0, 1.0)); }
)";
    const GLuint program = BuildWireProgram({{GL_COMPUTE_SHADER, compute}});
    ASSERT_NE(program, 0u);
    glBindImageTexture(2, view, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(program);
    glDispatchCompute(8, 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    for (const int layer : {0, 1}) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, root, 0, layer);
        std::array<GLubyte, 8 * 8 * 4> pixels{};
        glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        const std::array<GLubyte, 4> expected = layer == 1
            ? std::array<GLubyte, 4>{0, 255, 0, 255} : std::array<GLubyte, 4>{255, 0, 0, 255};
        for (size_t at = 0; at < pixels.size(); at += 4) {
            const std::array<GLubyte, 4> actual{pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
            EXPECT_EQ(actual, expected) << "root layer " << layer << ", texel " << at / 4;
        }
    }
    glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glDeleteProgram(program);
    glDeleteTextures(1, &view);
    glDeleteTextures(1, &root);
}

// Historical case name retained for the registered test catalogue. The old P7
// refusal is retired: this case now requires the VBO draw's actual green pixels.
TEST_F(F1WireScenario, EnabledVertexBufferDrawKeepsNamedP7Fatal) {
    if (!Ready()) return;
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
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<GLubyte, 8 * 8 * 4> pixels{};
    glReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    for (size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_EQ(pixels[i], 0) << "VBO draw red component at pixel " << i / 4;
        EXPECT_EQ(pixels[i + 1], 255) << "VBO draw green component at pixel " << i / 4;
        EXPECT_EQ(pixels[i + 2], 0);
        EXPECT_EQ(pixels[i + 3], 255);
    }
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, UniformBufferRangeAndRebindPixels) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    const char* fragment = R"(#version 430 core
layout(std140, binding=3) uniform Colour { vec4 value; };
layout(location=0) out vec4 color;
void main() { color = value; }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    const GLuint block = glGetUniformBlockIndex(program, "Colour");
    ASSERT_NE(block, GLuint(GL_INVALID_INDEX));
    glUniformBlockBinding(program, block, 3);
    GLint alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    ASSERT_GT(alignment, 0);
    const size_t stride = ((sizeof(GLfloat) * 4 + alignment - 1) / alignment) * alignment;
    const std::array<GLfloat, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1}, blue{0, 0, 1, 1}, yellow{1, 1, 0, 1};
    std::vector<GLubyte> bytes(stride * 3);
    std::memcpy(bytes.data(), red.data(), sizeof(red));
    std::memcpy(bytes.data() + stride, green.data(), sizeof(green));
    std::memcpy(bytes.data() + 2 * stride, blue.data(), sizeof(blue));
    GLuint buffers[2]{}, vao = 0;
    glGenBuffers(2, buffers);
    glBindBuffer(GL_UNIFORM_BUFFER, buffers[0]);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW);
    std::memcpy(bytes.data() + stride, red.data(), sizeof(red));
    glBindBuffer(GL_UNIFORM_BUFFER, buffers[1]);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    const auto drawAndExpect = [&](const std::array<GLfloat, 4>& expected, const char* when) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << when;
        for (size_t i = 0; i < pixel.size(); ++i) EXPECT_EQ(pixel[i], GLubyte(expected[i] * 255)) << when << " channel " << i;
    };
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, buffers[0], GLintptr(stride), sizeof(green));
    drawAndExpect(green, "nonzero UBO range offset");
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, buffers[0], GLintptr(2 * stride), sizeof(blue));
    drawAndExpect(blue, "same UBO, changed range");
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, buffers[1], GLintptr(stride), sizeof(red));
    drawAndExpect(red, "changed UBO handle at the same binding and offset");
    glBindBuffer(GL_UNIFORM_BUFFER, buffers[1]);
    glBufferSubData(GL_UNIFORM_BUFFER, GLintptr(stride), sizeof(yellow), yellow.data());
    drawAndExpect(yellow, "subdata changes an already bound UBO");
    glBindBufferRange(GL_UNIFORM_BUFFER, 5, buffers[0], 0, sizeof(red));
    glUniformBlockBinding(program, block, 5);
    drawAndExpect(red, "program block binding changes without relinking");
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 5, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, ShortUniformBufferRangePadsMissingBytes) {
    if (!Ready()) return;
    // Magma's compatibility policy for short application UBOs, not a claim about
    // undefined short-range reads on arbitrary native GL drivers. Bytes outside
    // the bound range are deliberately nonzero, so widening a descriptor fails.
    if (Gl().BackendName() != "DirectVulkan") GTEST_SKIP() << "Magma short-UBO compatibility policy";
    Attach(GL_RGBA8);
    const char* fragment = R"(#version 430 core
layout(std140, binding=3) uniform ShortColour { vec4 value; vec4 tail; };
layout(location=0) out vec4 color;
void main() { color = value + tail; }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLint alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    ASSERT_GT(alignment, 0);
    const size_t offset = size_t(alignment);
    std::vector<GLubyte> bytes(offset + 8 * sizeof(GLfloat), 0);
    const GLfloat values[] = {0, 1, 0, 1, 0, 1, 1, 1};
    std::memcpy(bytes.data() + offset, values, sizeof(values));
    GLuint ubo = 0, vao = 0;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytes.size()), bytes.data(), GL_STATIC_DRAW);
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, ubo, GLintptr(offset), 5 * sizeof(GLfloat));
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<GLubyte, 4> pixel{};
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 0, 255})) << "range-external poison leaked into short UBO tail";
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &ubo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, SubWordUniformBufferRangePadsMissingBytes) {
    if (!Ready()) return;
    // The sibling of ShortUniformBufferRangePadsMissingBytes whose bound range does NOT
    // end on a four-byte boundary. That window is what `uniform-buffer-byte-tail`
    // used to refuse (a named Fatal that took the whole session with it), and it is the
    // live boundary the Redmi Minecraft run reported (notes/p5f/magma-inproc-fix.md #5).
    //
    // Only the SIZE can land off a word here. glBindBufferRange constrains the offset to
    // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT and records GL_INVALID_VALUE otherwise
    // (MG_Impl/GLImpl/Buffer/GL_Buffer.cpp ValidateBufferRangeOffsetAndSize), so an odd
    // offset never reaches the backend through the public API - but GL 4.6 core 6.1.1
    // puts no such rule on size, which is why 19 is a legal bind and 19 % 4 is not 0.
    if (Gl().BackendName() != "DirectVulkan") GTEST_SKIP() << "Magma short-UBO compatibility policy";
    Attach(GL_RGBA8);
    const char* fragment = R"(#version 430 core
layout(std140, binding=3) uniform ShortColour { vec4 value; vec4 tail; };
layout(location=0) out vec4 color;
void main() { color = value + tail; }
)";
    const GLuint program = BuildWireProgram({{GL_VERTEX_SHADER, kWireVertexIdTriangle},
                                             {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(program, 0u);
    GLint alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    ASSERT_GT(alignment, 0);
    const size_t offset = size_t(alignment);
    const GLsizeiptr boundSize = 19; // 16 bytes of `value` plus three bytes of tail.x
    std::vector<GLubyte> bytes(offset + 8 * sizeof(GLfloat), 0);
    const GLfloat values[] = {0, 1, 0, 1, 0, 1, 1, 1};
    std::memcpy(bytes.data() + offset, values, sizeof(values));
    // The byte immediately past the bound range, and the reason this case is sharper than
    // its four-byte sibling: rounding the read OUT to a word pulls byte 19 into the staging
    // slice, and the block must still end at byte 19. If it leaks, tail.x reads 0.5f and
    // the red channel comes back 127 instead of 0.
    bytes[offset + 19] = 0x3F;
    GLuint ubo = 0, vao = 0;
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(bytes.size()), bytes.data(), GL_STATIC_DRAW);
    glBindBufferRange(GL_UNIFORM_BUFFER, 3, ubo, GLintptr(offset), boundSize);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "a 19-byte uniform range is a legal bind";
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<GLubyte, 4> pixel{};
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 0, 255}))
        << "the sub-word uniform range did not pad to the reflected block exactly";
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteBuffers(1, &ubo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}

TEST_F(F1WireScenario, ComputeWrittenVertexAndIndexBuffersDrawAndReadBack) {
    if (!Ready()) return;
    Attach(GL_RGBA8);
    const char* compute = R"(#version 430 core
layout(local_size_x=1) in;
layout(std430, binding=0) buffer Vertices { vec4 positions[]; };
layout(std430, binding=1) buffer Indices { uint indices[]; };
void main() {
    positions[0] = vec4(-1, -1, 0, 1);
    positions[1] = vec4(3, -1, 0, 1);
    positions[2] = vec4(-1, 3, 0, 1);
    indices[0] = 0u; indices[1] = 1u; indices[2] = 2u;
}
)";
    const char* vertex = R"(#version 430 core
layout(location=0) in vec4 position;
void main() { gl_Position = position; }
)";
    const char* fragment = R"(#version 430 core
layout(location=0) out vec4 color;
void main() { color = vec4(0, 1, 0, 1); }
)";
    const GLuint cs = BuildWireProgram({{GL_COMPUTE_SHADER, compute}});
    const GLuint graphics = BuildWireProgram({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}});
    ASSERT_NE(cs, 0u);
    ASSERT_NE(graphics, 0u);
    GLuint buffers[2]{}, vao = 0;
    glGenBuffers(2, buffers);
    const GLfloat poisonPositions[12]{}; // A degenerate triangle if stale CPU bytes reach the draw.
    const GLuint poisonIndices[3]{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(poisonPositions), poisonPositions, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffers[0]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(poisonIndices), poisonIndices, GL_DYNAMIC_COPY);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, buffers[1]);
    glUseProgram(cs);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glUseProgram(graphics);
    glViewport(0, 0, 8, 8);
    glDisable(GL_DEPTH_TEST);
    const auto drawAndExpect = [&](const char* when) {
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << when;
        EXPECT_EQ(pixel, (std::array<GLubyte, 4>{0, 255, 0, 255})) << when;
    };
    drawAndExpect("GPU-written vertex/index bytes before any CPU readback");
    std::array<GLfloat, 12> actualPositions{};
    std::array<GLuint, 3> actualIndices{};
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(actualPositions), actualPositions.data());
    glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(actualIndices), actualIndices.data());
    EXPECT_EQ(actualPositions, (std::array<GLfloat, 12>{-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1}));
    EXPECT_EQ(actualIndices, (std::array<GLuint, 3>{0, 1, 2}));
    drawAndExpect("readback must not replace the canonical GPU-written buffers with old CPU shadows");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(2, buffers);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(cs);
    glDeleteProgram(graphics);
}

TEST_F(F1WireScenario, UnalignedAtomicCounterRangeDeclinesAndTheSessionLives) {
    if (!Ready()) return;
    // The one shape in the P7 unaligned-range cluster an application can actually reach.
    // GL 4.6 core 6.1.1 gives GL_ATOMIC_COUNTER_BUFFER no queryable offset alignment, so
    // glBindBufferRange only enforces offset % 4 on it, while glslang lowers the counter
    // block onto a storage buffer whose descriptor offset must be a multiple of
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (minStorageBufferOffsetAlignment: 16 on
    // lavapipe, 64 on Adreno). Offset 4 is therefore a legal bind the backend cannot
    // express. It used to take the session with it as a named Fatal; it is now a decline,
    // and what this case pins is exactly that - the dispatch is lost, nothing else is.
    if (Gl().BackendName() != "DirectVulkan") GTEST_SKIP() << "Magma descriptor-alignment decline";
    GLint storageAlignment = 0;
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlignment);
    if (storageAlignment <= 4) GTEST_SKIP() << "this device can express a 4-byte-aligned counter range";
    const char* compute = R"(#version 430 core
layout(local_size_x=1) in;
layout(binding=0, offset=0) uniform atomic_uint counter;
void main() { atomicCounterIncrement(counter); }
)";
    const GLuint cs = BuildWireProgram({{GL_COMPUTE_SHADER, compute}});
    ASSERT_NE(cs, 0u);
    GLuint counters = 0;
    glGenBuffers(1, &counters);
    const std::array<GLuint, 4> seed{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, counters);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, GLsizeiptr(seed.size() * sizeof(GLuint)), seed.data(), GL_DYNAMIC_COPY);
    // Offset 4: a multiple of 4 and so a legal GL bind, but not a multiple of the storage
    // alignment this device reports. The bind itself must NOT raise a GL error.
    glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 0, counters, GLintptr(4), GLsizeiptr(sizeof(GLuint)));
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "offset 4 is a legal GL_ATOMIC_COUNTER_BUFFER range";
    glUseProgram(cs);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    // The session is still here - this is the whole point - and it still answers GL.
    std::array<GLuint, 4> after{};
    glGetBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, GLsizeiptr(after.size() * sizeof(GLuint)), after.data());
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the declined dispatch must not poison the context";
    // The dispatch was declined, so the counter is untouched; the bytes outside the bound
    // range must be untouched either way. Both halves of that are worth pinning: a future
    // copy-back implementation turns the first EXPECT into seed[1] + 1 and nothing else.
    EXPECT_EQ(after[1], seed[1]) << "a declined counter dispatch must not have run";
    EXPECT_EQ(after[0], seed[0]);
    EXPECT_EQ(after[2], seed[2]);
    EXPECT_EQ(after[3], seed[3]);
    // And the context is still usable for ordinary work afterwards.
    GLuint probe = 0;
    glGenBuffers(1, &probe);
    glBindBuffer(GL_ARRAY_BUFFER, probe);
    glBufferData(GL_ARRAY_BUFFER, 16, seed.data(), GL_STATIC_DRAW);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the session survived the decline";
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &probe);
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, 0);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
    glUseProgram(0);
    glDeleteBuffers(1, &counters);
    glDeleteProgram(cs);
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

TEST_F(F1WireScenario, ColorBlitToDefaultIgnoresViewportAndPreservesOrientation) {
    if (!Ready()) return;
    ASSERT_GE(Gl().Width(), 16);
    ASSERT_GE(Gl().Height(), 16);
    ASSERT_NO_FATAL_FAILURE(PaintNonSquareBlitSource());
    const int width = Gl().Width(), height = Gl().Height();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // The destination really is the default framebuffer.
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(1, 2, 3, 5); // glBlitFramebuffer must ignore this unrelated viewport.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, 6, 4, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    EXPECT_EQ(ReadOnePixel(width / 4, height / 4), (std::array<GLubyte, 4>{255, 0, 0, 255})) << "bottom left";
    EXPECT_EQ(ReadOnePixel(3 * width / 4, height / 4), (std::array<GLubyte, 4>{0, 255, 0, 255})) << "bottom right";
    EXPECT_EQ(ReadOnePixel(width / 4, 3 * height / 4), (std::array<GLubyte, 4>{0, 0, 255, 255})) << "top left";
    EXPECT_EQ(ReadOnePixel(3 * width / 4, 3 * height / 4), (std::array<GLubyte, 4>{255, 255, 0, 255})) << "top right";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    EXPECT_EQ((std::array<GLint, 4>{viewport[0], viewport[1], viewport[2], viewport[3]}),
              (std::array<GLint, 4>{1, 2, 3, 5})) << "blit must not mutate the application's viewport";
    Gl().EndFrame();
}

TEST_F(F1WireScenario, ColorBlitToDefaultHonorsScissorAndReversedRect) {
    if (!Ready()) return;
    ASSERT_GE(Gl().Width(), 16);
    ASSERT_GE(Gl().Height(), 16);
    ASSERT_NO_FATAL_FAILURE(PaintNonSquareBlitSource());
    const int width = Gl().Width(), height = Gl().Height();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(2, 1, 5, 3);
    // Copy into the centre half with BOTH destination axes reversed. Scissor
    // keeps only its left half, so a plain unclipped vkCmdBlitImage is incorrect.
    glScissor(width / 4, height / 4, width / 4, height / 2);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, 6, 4, 3 * width / 4, 3 * height / 4,
                      width / 4, height / 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    EXPECT_EQ(ReadOnePixel(3 * width / 8, 3 * height / 8), (std::array<GLubyte, 4>{255, 255, 0, 255}))
        << "reversed bottom-left destination reads the source top-right yellow quadrant";
    EXPECT_EQ(ReadOnePixel(3 * width / 8, 5 * height / 8), (std::array<GLubyte, 4>{0, 255, 0, 255}))
        << "reversed top-left destination reads the source bottom-right green quadrant";
    const std::array<GLubyte, 4> untouched{255, 0, 255, 255};
    EXPECT_EQ(ReadOnePixel(5 * width / 8, 3 * height / 8), untouched) << "inside destination, outside scissor";
    EXPECT_EQ(ReadOnePixel(5 * width / 8, 5 * height / 8), untouched) << "upper destination outside scissor";
    EXPECT_EQ(ReadOnePixel(width / 8, height / 2), untouched) << "outside destination rectangle";
    EXPECT_EQ(ReadOnePixel(3 * width / 8, height / 8), untouched) << "below destination and scissor";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
    glDisable(GL_SCISSOR_TEST);
    Gl().EndFrame();
}

// P7 wave 2-B, CONTRACT-P7 §3.2: `default-color-blit-shape@P7` RETIRES.
//
// One Fatal covered three unrelated situations. The shader pass it guarded exists for ROTATION
// and can only SAMPLE its source - sampler2D, a float result, one of two 2D view types - so a
// 3D slice or an integer attachment made it return false and the refusal fired EVEN ON A SURFACE
// WITH NO ROTATION, where the ordinary vkCmdBlitImage arm below it would have done the blit
// correctly and does handle those shapes. Identity now falls through to that arm; a rotated
// surface moves the region into an owned 2D float scratch and blits that; anything outside the
// four rotations declines by name.
//
// THE SOURCE IS A 3D TEXTURE SLICE, which is ordinary defined GL (a layer of a 3D texture is a
// legal colour attachment) and is exactly the shape the shader pass cannot sample. The same case
// runs twice on each arm: plain, taking the identity fall-through, and under
// MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH, taking the rotated arm's scratch path on an identity
// surface - where it must produce THE SAME PIXELS. That equality is what makes the scratch arm
// testable at all: every surface this lane can create is identity, and so is the Redmi pbuffer
// the cluster's evidence comes from.
//
// Red once (executed, reverted): restore MagmaWireFatal("default-color-blit-shape@P7") and both
// entries die by that name.
TEST_F(F1WireScenario, ColorBlitToDefaultFromANonSampleableSourceDegrades) {
    if (!Ready()) return;
    ASSERT_GE(Gl().Width(), 16);
    ASSERT_GE(Gl().Height(), 16);
    const int width = Gl().Width(), height = Gl().Height();
    GLuint volume = 0, volumeFbo = 0;
    glGenTextures(1, &volume);
    glBindTexture(GL_TEXTURE_3D, volume);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 6, 4, 2);
    glGenFramebuffers(1, &volumeFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, volumeFbo);
    // Slice 1, not slice 0: a scratch copy that forgot the z origin would read the other slice
    // and every quadrant assertion below would still be about SOME real texels.
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, volume, 0, 1);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
    constexpr GLfloat colors[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
    glEnable(GL_SCISSOR_TEST);
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        glScissor((quadrant % 2) * 3, (quadrant / 2) * 2, 3, 2);
        glClearColor(colors[quadrant][0], colors[quadrant][1], colors[quadrant][2], colors[quadrant][3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    // The OTHER slice gets one flat colour that appears in no quadrant.
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, volume, 0, 0);
    glClearColor(0, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, volume, 0, 1);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.DefaultBlitShape.setup";

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, volumeFbo);
    const auto before = PeekSplitRuntime().emitSeq;
    glBlitFramebuffer(0, 0, 6, 4, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    EXPECT_GT(PeekSplitRuntime().emitSeq, before) << "F1.DefaultBlitShape.wire";
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const std::string arm = SplitLane::MarkerValue("MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH");
    EXPECT_EQ(ReadOnePixel(width / 4, height / 4), (std::array<GLubyte, 4>{255, 0, 0, 255}))
        << "F1.DefaultBlitShape bottom left (scratch arm=" << arm << ")";
    EXPECT_EQ(ReadOnePixel(3 * width / 4, height / 4), (std::array<GLubyte, 4>{0, 255, 0, 255}))
        << "F1.DefaultBlitShape bottom right (scratch arm=" << arm << ")";
    EXPECT_EQ(ReadOnePixel(width / 4, 3 * height / 4), (std::array<GLubyte, 4>{0, 0, 255, 255}))
        << "F1.DefaultBlitShape top left (scratch arm=" << arm << ")";
    EXPECT_EQ(ReadOnePixel(3 * width / 4, 3 * height / 4), (std::array<GLubyte, 4>{255, 255, 0, 255}))
        << "F1.DefaultBlitShape top right (scratch arm=" << arm << ")";
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.DefaultBlitShape.error";
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDeleteFramebuffers(1, &volumeFbo);
    glBindTexture(GL_TEXTURE_3D, 0);
    glDeleteTextures(1, &volume);
    glBindTexture(GL_TEXTURE_2D, texture);
    Gl().EndFrame();
}

namespace {
// One 8x8x2 RGBA8 array texture with two levels, filled so that EVERY subresource this pair
// of cases touches is distinguishable from every other one. A flat fill would let a copy that
// went to the wrong level, the wrong layer or nowhere at all read as correct.
//   level 0 layer 0: four quadrants  (GL row order: red/green on the bottom, blue/yellow up)
//   level 0 layer 1: magenta
//   level 1 layer 0: cyan       level 1 layer 1: white
constexpr std::array<GLubyte, 4> kInPlaceRed{255, 0, 0, 255};
constexpr std::array<GLubyte, 4> kInPlaceGreen{0, 255, 0, 255};
constexpr std::array<GLubyte, 4> kInPlaceBlue{0, 0, 255, 255};
constexpr std::array<GLubyte, 4> kInPlaceYellow{255, 255, 0, 255};
constexpr std::array<GLubyte, 4> kInPlaceMagenta{255, 0, 255, 255};
constexpr std::array<GLubyte, 4> kInPlaceCyan{0, 255, 255, 255};
constexpr std::array<GLubyte, 4> kInPlaceWhite{255, 255, 255, 255};

std::array<GLubyte, 4> InPlaceQuadrant(int x, int y) {
    if (y < 4) return x < 4 ? kInPlaceRed : kInPlaceGreen;
    return x < 4 ? kInPlaceBlue : kInPlaceYellow;
}

GLuint MakeInPlaceCopyTexture() {
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 2, GL_RGBA8, 8, 8, 2);
    std::vector<GLubyte> level0(8 * 8 * 2 * 4);
    for (int layer = 0; layer < 2; ++layer)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const auto colour = layer == 0 ? InPlaceQuadrant(x, y) : kInPlaceMagenta;
                const size_t at = (static_cast<size_t>(layer) * 64 + y * 8 + x) * 4;
                std::copy(colour.begin(), colour.end(), level0.begin() + at);
            }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 8, 8, 2, GL_RGBA, GL_UNSIGNED_BYTE, level0.data());
    std::vector<GLubyte> level1(4 * 4 * 2 * 4);
    for (int layer = 0; layer < 2; ++layer)
        for (int at = 0; at < 16; ++at) {
            const auto colour = layer == 0 ? kInPlaceCyan : kInPlaceWhite;
            std::copy(colour.begin(), colour.end(),
                      level1.begin() + (static_cast<size_t>(layer) * 16 + at) * 4);
        }
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 4, 4, 2, GL_RGBA, GL_UNSIGNED_BYTE, level1.data());
    return name;
}

// (level, side) -> the whole level, every layer, as glGetTexImage hands it back.
std::vector<GLubyte> ReadInPlaceLevel(GLint level, int side) {
    std::vector<GLubyte> pixels(static_cast<size_t>(side) * side * 2 * 4, 17);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

::testing::AssertionResult InPlaceTexelIs(const std::vector<GLubyte>& pixels, int side, int layer,
                                          int x, int y, const std::array<GLubyte, 4>& expected) {
    const size_t at = ((static_cast<size_t>(layer) * side * side) + static_cast<size_t>(y) * side + x) * 4;
    if (at + 4 > pixels.size()) return ::testing::AssertionFailure() << "texel out of range";
    for (int channel = 0; channel < 4; ++channel)
        if (pixels[at + channel] != expected[channel])
            return ::testing::AssertionFailure()
                   << "layer " << layer << " (" << x << "," << y << ") channel " << channel
                   << " is " << int(pixels[at + channel]) << ", expected " << int(expected[channel]);
    return ::testing::AssertionSuccess();
}
} // namespace

// P7 wave 2-B, CONTRACT-P7 §3.2: `copy-image-in-place@P7` RETIRES.
//
// glCopyImageSubData between two subresources of ONE texture is ordinary, defined GL (4.6 core
// 18.3.2 even spells out that a texture and a view of it are two objects over one image and may
// be copied between). The wire arm took the session down for it, because the TRANSFER_SRC /
// TRANSFER_DST pair below it cannot describe one image: both endpoints share one tracked
// VkImageLayout and the second transition undoes the first. GENERAL can describe it.
//
// The monolith arm still DECLINES this shape outright, so this case is split-only in the
// strongest sense - there is no monolith reading of it to compare against, and that asymmetry
// is deliberate rather than an oversight (§3.2 asks the wire arm to perform the copy).
//
// Red once (executed, reverted): restore MagmaWireFatal("copy-image-in-place@P7") and this case
// dies by that name.
TEST_F(F1WireScenario, CopyImageInPlaceAcrossLevelsAndLayers) {
    if (!Ready()) return;
    const GLuint array = MakeInPlaceCopyTexture();
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlace.setup";

    // LEVEL 0 -> LEVEL 1, same layer: the destination level is a different subresource of the
    // same image, so one GENERAL transition has to cover both of them.
    const auto before = PeekSplitRuntime().emitSeq;
    glCopyImageSubData(array, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                       array, GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 4, 4, 1);
    ASSERT_GT(PeekSplitRuntime().emitSeq, before) << "F1.CopyInPlace.wire";
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlace.levelCopy.error";
    {
        const auto level1 = ReadInPlaceLevel(1, 4);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlace.levelCopy.readback";
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                // The source rectangle is level 0's bottom-left quadrant, which is all red.
                EXPECT_TRUE(InPlaceTexelIs(level1, 4, 0, x, y, kInPlaceRed))
                    << "F1.CopyInPlace.level1.layer0";
                EXPECT_TRUE(InPlaceTexelIs(level1, 4, 1, x, y, kInPlaceWhite))
                    << "F1.CopyInPlace.level1.layer1 must not be touched";
            }
    }

    // LAYER 0 -> LAYER 1 at level 0: the same level, disjoint slices. Defined GL, and the
    // overlap decline must NOT catch it.
    glCopyImageSubData(array, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                       array, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 8, 8, 1);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlace.layerCopy.error";
    {
        const auto level0 = ReadInPlaceLevel(0, 8);
        ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlace.layerCopy.readback";
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                EXPECT_TRUE(InPlaceTexelIs(level0, 8, 0, x, y, InPlaceQuadrant(x, y)))
                    << "F1.CopyInPlace.level0.layer0 is the source and must be unchanged";
                EXPECT_TRUE(InPlaceTexelIs(level0, 8, 1, x, y, InPlaceQuadrant(x, y)))
                    << "F1.CopyInPlace.level0.layer1 must carry the source's quadrants";
            }
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glDeleteTextures(1, &array);
    glBindTexture(GL_TEXTURE_2D, texture);
}

// The one in-place shape that stays a DECLINE (§3.2): same image, same level, same layer,
// overlapping rectangles. GL 4.6 core 18.3.2 leaves that undefined, and GENERAL makes the
// command legal to RECORD rather than meaningful to execute - vkCmdCopyImage orders nothing
// between its own reads and writes inside one region. The case pins that nothing is recorded:
// no GL error, and the level still holds exactly what it held.
TEST_F(F1WireScenario, CopyImageInPlaceOverlapDeclinesAndLeavesTheLevelAlone) {
    if (!Ready()) return;
    const GLuint array = MakeInPlaceCopyTexture();
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlaceOverlap.setup";
    // (0,0)-(4,4) onto (2,2): four of the sixteen texels are in both rectangles.
    glCopyImageSubData(array, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                       array, GL_TEXTURE_2D_ARRAY, 0, 2, 2, 0, 4, 4, 1);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlaceOverlap.error";
    const auto level0 = ReadInPlaceLevel(0, 8);
    ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "F1.CopyInPlaceOverlap.readback";
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            EXPECT_TRUE(InPlaceTexelIs(level0, 8, 0, x, y, InPlaceQuadrant(x, y)))
                << "F1.CopyInPlaceOverlap.level0 must be untouched by a declined copy";
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glDeleteTextures(1, &array);
    glBindTexture(GL_TEXTURE_2D, texture);
}

} // namespace MGITest
