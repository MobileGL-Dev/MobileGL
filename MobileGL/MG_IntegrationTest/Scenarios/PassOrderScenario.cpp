// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PassOrderScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - WORK ON AN IMAGE IN ONE PASS IS ORDERED AGAINST THE NEXT PASS THAT TOUCHES IT.
//
// Magma records a draw pass per framebuffer and orders a new pass after earlier image writes with
// a memory barrier, shared between the pass's attachments and the sampled images of its draws.
// Each case below hands an image from one piece of work to the next with no readback in between,
// so only that barrier orders them:
//   - render into a texture, then sample it in the next pass (read after write);
//   - render into a texture, render elsewhere, render into the texture again (write after write);
//   - blit into a texture, then sample it (a render-pass blit, then read);
//   - copy into a texture with glCopyImageSubData, then sample it (an out-of-pass transfer write,
//     then read - the shape whose barrier must not be skipped on the strength of a barrier
//     recorded between the transfer's pass close and its write);
//   - blit one half of a texture onto its other half, then sample it (the one transfer whose
//     image stays in GENERAL, so a memory barrier and no layout transition orders it);
//   - sample a texture, then render into it (write after read).
// A software rasterizer executes in order and renders these right without the barrier, so the
// gate that goes red when the barrier is skipped is synchronization validation (submit-time),
// run over these cases; the pixel checks hold the semantics.

#include <array>
#include <vector>

#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {
using Pixel = std::array<GLubyte, 4>;
constexpr Pixel red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255}, black{0, 0, 0, 255};
constexpr int kWidth = 16, kHeight = 8, kSize = 8;
constexpr const char* kVertex = R"(#version 330 core
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(p[gl_VertexID], 0, 1);
})";
constexpr const char* kSample = R"(#version 330 core
uniform sampler2D src;
layout(location=0) out vec4 color;
void main() { color = texelFetch(src, ivec2(gl_FragCoord.xy) % 8, 0); })";
constexpr const char* kTint = R"(#version 330 core
uniform vec4 tint;
layout(location=0) out vec4 color;
void main() { color = tint; })";

class PassOrderScenario : public ScenarioTest {
protected:
    GLuint target = 0, sample = 0, tint = 0, vao = 0;
    GLint tintLocation = -1;
    std::vector<GLuint> textures, fbos;

    void SetUp() override {
        ScenarioTest::SetUp();
        if (!Ready()) return;
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        target = NewFramebuffer(NewTexture(kWidth, kHeight, black));
        sample = Link(kSample);
        tint = Link(kTint);
        tintLocation = glGetUniformLocation(tint, "tint");
    }

    void TearDown() override {
        if (Ready()) {
            glUseProgram(0);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
            if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()), textures.data());
            if (sample) glDeleteProgram(sample);
            if (tint) glDeleteProgram(tint);
        }
        ScenarioTest::TearDown();
    }

    GLuint NewTexture(int width, int height, Pixel color) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        const std::vector<Pixel> pixels(size_t(width) * size_t(height), color);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return texture;
    }

    GLuint NewFramebuffer(GLuint texture) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        fbos.push_back(fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        return fbo;
    }

    static GLuint Link(const char* fragment) {
        const GLuint linked = glCreateProgram();
        const std::array<GLenum, 2> kinds{GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        const std::array<const char*, 2> sources{kVertex, fragment};
        for (size_t i = 0; i < kinds.size(); ++i) {
            const GLuint shader = glCreateShader(kinds[i]);
            glShaderSource(shader, 1, &sources[i], nullptr);
            glCompileShader(shader);
            glAttachShader(linked, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(linked);
        GLint ok = GL_FALSE;
        glGetProgramiv(linked, GL_LINK_STATUS, &ok);
        EXPECT_EQ(ok, GL_TRUE);
        return linked;
    }

    // Fills the viewport (x, y, w, h) of `fbo` with `color` by drawing.
    void Fill(GLuint fbo, Pixel color, int x, int y, int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glUseProgram(tint);
        glUniform4f(tintLocation, color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f);
        glViewport(x, y, w, h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Samples `texture` into the 8x8 tile `tile` of the target.
    void SampleInto(GLuint texture, int tile) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glUseProgram(sample);
        glBindTexture(GL_TEXTURE_2D, texture);
        glViewport(tile * kSize, 0, kSize, kHeight);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void ExpectPixel(int x, int y, Pixel expected, const char* why) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        Pixel actual{};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        for (size_t channel = 0; channel < actual.size(); ++channel)
            EXPECT_EQ(int(actual[channel]), int(expected[channel])) << why << " channel=" << channel;
        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << why;
    }
};

TEST_F(PassOrderScenario, ATextureRenderedInOnePassIsSampledByTheNext) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewTexture(kSize, kSize, black);
    const GLuint fbo = NewFramebuffer(texture);
    glFinish();
    Fill(fbo, green, 0, 0, kSize, kSize);
    SampleInto(texture, 0);
    SampleInto(texture, 1);
    ExpectPixel(4, 4, green, "the next pass samples what the previous pass rendered");
    ExpectPixel(12, 4, green, "a second draw in the same pass samples it too");
}

TEST_F(PassOrderScenario, ATextureRenderedAgainAfterAnotherPassKeepsBothWrites) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewTexture(kSize, kSize, black);
    const GLuint fbo = NewFramebuffer(texture);
    glFinish();
    Fill(fbo, red, 0, 0, kSize, kSize);
    Fill(target, black, 0, 0, kWidth, kHeight); // another pass in between
    Fill(fbo, blue, 0, 0, kSize / 2, kSize);    // the left half again
    SampleInto(texture, 0);
    ExpectPixel(1, 4, blue, "the second write to the texture lands over the first");
    ExpectPixel(6, 4, red, "the first write survives where the second did not reach");
}

TEST_F(PassOrderScenario, ATextureBlittedIntoIsSampledByTheNextDraw) {
    if (!Ready() || IsSkipped()) return;
    const GLuint source = NewTexture(kSize, kSize, green);
    const GLuint sourceFbo = NewFramebuffer(source);
    const GLuint texture = NewTexture(kSize, kSize, black);
    const GLuint fbo = NewFramebuffer(texture);
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, kSize, kSize, 0, 0, kSize, kSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SampleInto(texture, 0);
    ExpectPixel(4, 4, green, "the draw after the blit samples the blitted texels");
}

TEST_F(PassOrderScenario, ATextureCopiedIntoIsSampledByTheNextDraw) {
    if (!Ready() || IsSkipped()) return;
    const GLuint source = NewTexture(kSize, kSize, green);
    const GLuint texture = NewTexture(kSize, kSize, black);
    glFinish();
    SampleInto(texture, 1); // a pass is open on the target when the copy arrives
    glCopyImageSubData(source, GL_TEXTURE_2D, 0, 0, 0, 0, texture, GL_TEXTURE_2D, 0, 0, 0, 0, kSize, kSize, 1);
    SampleInto(texture, 0);
    ExpectPixel(4, 4, green, "the draw after the copy samples the copied texels");
    ExpectPixel(12, 4, black, "the draw before the copy samples the old texels");
}

TEST_F(PassOrderScenario, ATextureBlittedWithinItselfIsSampledByTheNextDraw) {
    if (!Ready() || IsSkipped()) return;
    // GL allows a blit between non-overlapping regions of one image; ES 3.x makes identical read
    // and draw buffers GL_INVALID_OPERATION, and DirectGLES forwards the blit to the driver as is,
    // so the blit is dropped there (a DirectGLES conformance gap of its own, not this case's subject).
    if (Gl().BackendName() == "DirectGLES") GTEST_SKIP() << "DirectGLES does not emulate a same-image blit";
    const GLuint texture = NewTexture(kSize, kSize, red);
    const GLuint fbo = NewFramebuffer(texture);
    glFinish();
    Fill(fbo, green, 0, 0, kSize / 2, kSize);
    SampleInto(texture, 1); // a pass is open on the target when the blit arrives
    // The left half onto the right half of the same image: one image is both ends of the
    // transfer, so it stays in GENERAL and its only ordering edge is the memory barrier.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, kSize / 2, kSize, kSize / 2, 0, kSize, kSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SampleInto(texture, 0);
    ExpectPixel(6, 4, green, "the draw after the blit samples the blitted half");
    ExpectPixel(14, 4, red, "the draw before the blit samples the right half's old texels");
}

TEST_F(PassOrderScenario, ATextureSampledBeforeItIsRenderedIntoKeepsTheOldTexelsForThatDraw) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewTexture(kSize, kSize, red);
    const GLuint fbo = NewFramebuffer(texture);
    glFinish();
    SampleInto(texture, 0);
    Fill(fbo, blue, 0, 0, kSize, kSize);
    SampleInto(texture, 1);
    ExpectPixel(4, 4, red, "the draw before the render samples the old texels");
    ExpectPixel(12, 4, blue, "the draw after the render samples the new texels");
}

} // namespace
} // namespace MGITest
