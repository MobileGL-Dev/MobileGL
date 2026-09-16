// f1 split-only pixel controls: every result depends on the migrated verb.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include <array>
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {
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
} // namespace MGITest
