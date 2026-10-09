// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClearOrderScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A CLEAR TAKES EFFECT WHERE THE APPLICATION ISSUED IT, WHICHEVER FRAMEBUFFER IT WENT THROUGH.
//
// Minecraft clears its targets through framebuffers of their own (a depth-only one for the shared
// depth texture, one per colour target) and draws through another framebuffer holding the same
// textures. Magma keeps such a clear on the image and folds it into the load op of the next pass
// that draws into the image, records it inside the open pass when that pass holds the image, and
// runs it before any other use (sampling, blit, readback, upload, respecify). Each case below is
// one of those paths, with no readback between the clear and the work that depends on it:
//   - a depth clear through a depth-only framebuffer, then a depth-tested draw through the main one
//     (folded into the next pass), and the same while the main pass is open (inside it);
//   - a colour clear through a framebuffer of its own, then a draw covering half the texture
//     through another framebuffer holding it (the other half keeps the clear colour);
//   - a cleared texture sampled, blitted from, uploaded into, read back and re-specified before
//     any pass draws into it;
//   - a scissored clear, which clears only its box.

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
constexpr int kSize = 8, kTargetWidth = 16;
constexpr const char* kVertex = R"(#version 330 core
uniform float z;
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(p[gl_VertexID], z, 1);
})";
constexpr const char* kSample = R"(#version 330 core
uniform sampler2D src;
layout(location=0) out vec4 color;
void main() { color = texelFetch(src, ivec2(gl_FragCoord.xy) % 8, 0); })";
constexpr const char* kTint = R"(#version 330 core
uniform vec4 tint;
layout(location=0) out vec4 color;
void main() { color = tint; })";

class ClearOrderScenario : public ScenarioTest {
protected:
    GLuint target = 0, sample = 0, tint = 0, vao = 0;
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
        glDepthMask(GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        target = NewFramebuffer(NewColor(kTargetWidth, kSize, black), 0);
        sample = Link(kSample);
        tint = Link(kTint);
    }

    void TearDown() override {
        if (Ready()) {
            glUseProgram(0);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClearDepth(1.0);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
            if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()), textures.data());
            if (sample) glDeleteProgram(sample);
            if (tint) glDeleteProgram(tint);
        }
        ScenarioTest::TearDown();
    }

    GLuint NewColor(int width, int height, Pixel color) {
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

    GLuint NewDepth(int width, int height) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return texture;
    }

    // `color` and/or `depth` may be 0.
    GLuint NewFramebuffer(GLuint color, GLuint depth) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        fbos.push_back(fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (color) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
        if (!color) {
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
        }
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

    static void ClearColor(GLuint fbo, Pixel color) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Draws `color` over (x, y, w, h) of `fbo` at depth `z` (NDC).
    void Fill(GLuint fbo, Pixel color, int x, int y, int w, int h, float z = 0.0f) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glUseProgram(tint);
        glUniform1f(glGetUniformLocation(tint, "z"), z);
        glUniform4f(glGetUniformLocation(tint, "tint"), color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f,
                    color[3] / 255.0f);
        glViewport(x, y, w, h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Samples `texture` into the 8x8 tile `tile` of the target.
    void SampleInto(GLuint texture, int tile) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glUseProgram(sample);
        glUniform1f(glGetUniformLocation(sample, "z"), 0.0f);
        glBindTexture(GL_TEXTURE_2D, texture);
        glViewport(tile * kSize, 0, kSize, kSize);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void ExpectPixel(GLuint fbo, int x, int y, Pixel expected, const char* why) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        Pixel actual{};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        for (size_t channel = 0; channel < actual.size(); ++channel)
            EXPECT_EQ(int(actual[channel]), int(expected[channel])) << why << " channel=" << channel;
        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << why;
    }

    // Main framebuffer (colour + depth) and a depth-only clear framebuffer sharing its depth.
    struct Pair {
        GLuint color, depth, main, depthOnly;
    };
    Pair NewPair() {
        Pair pair{};
        pair.color = NewColor(kSize, kSize, black);
        pair.depth = NewDepth(kSize, kSize);
        pair.main = NewFramebuffer(pair.color, pair.depth);
        pair.depthOnly = NewFramebuffer(0, pair.depth);
        Prime(pair.main);
        Prime(pair.depthOnly);
        return pair;
    }

    // Uses `fbo` once - a clear and a draw - so the cases below run on images whose records have
    // settled, the steady state a game frame sees (a first use republishes the attachments' records,
    // and Magma then runs a pending clear at once rather than folding it into the next pass).
    void Prime(GLuint fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        Fill(fbo, black, 0, 0, kSize, kSize);
        glDepthFunc(GL_LESS);
        glDisable(GL_DEPTH_TEST);
        glFinish();
    }

    // Draws one pixel of the target nothing checks: whatever pass held the case's images is then
    // closed, so a clear of them is not taken inside it.
    void SwitchPassAway() { Fill(target, black, kTargetWidth - 1, kSize - 1, 1, 1); }
};

TEST_F(ClearOrderScenario, ADepthClearThroughADepthOnlyFramebufferReachesTheNextPass) {
    if (!Ready() || IsSkipped()) return;
    const Pair pair = NewPair();
    glBindFramebuffer(GL_FRAMEBUFFER, pair.main);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // depth 0: nothing passes LESS
    glFinish();
    SwitchPassAway();
    glBindFramebuffer(GL_FRAMEBUFFER, pair.depthOnly);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT); // through the other framebuffer
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    Fill(pair.main, green, 0, 0, kSize, kSize, 0.0f);
    glDisable(GL_DEPTH_TEST);
    ExpectPixel(pair.main, 4, 4, green, "the draw after the depth-only clear sees the cleared depth");
}

TEST_F(ClearOrderScenario, ADepthClearThroughADepthOnlyFramebufferLandsInsideTheOpenPass) {
    if (!Ready() || IsSkipped()) return;
    const Pair pair = NewPair();
    glBindFramebuffer(GL_FRAMEBUFFER, pair.main);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    Fill(pair.main, red, 0, 0, kSize, kSize, -0.5f); // the main pass is open, depth now -0.5 -> 0.25
    glBindFramebuffer(GL_FRAMEBUFFER, pair.depthOnly);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT); // the open pass holds this depth texture
    Fill(pair.main, green, 0, 0, kSize, kSize, 0.0f); // behind the red one unless the clear landed
    glDisable(GL_DEPTH_TEST);
    ExpectPixel(pair.main, 4, 4, green, "the draw after the clear passes the cleared depth");
}

TEST_F(ClearOrderScenario, AColourClearThroughItsOwnFramebufferReachesAnotherFramebufferHoldingTheTexture) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, black);
    const GLuint own = NewFramebuffer(texture, 0);
    const GLuint depth = NewDepth(kSize, kSize);
    const GLuint other = NewFramebuffer(texture, depth);
    Prime(own);
    Prime(other);
    SwitchPassAway();
    ClearColor(own, red);
    Fill(other, blue, 0, 0, kSize / 2, kSize); // the left half, through the other framebuffer
    SampleInto(texture, 0);
    ExpectPixel(target, 1, 4, blue, "the draw through the other framebuffer lands over the clear");
    ExpectPixel(target, 6, 4, red, "the clear holds where the draw did not reach");
}

TEST_F(ClearOrderScenario, AClearedTextureIsSampledBeforeAnyPassDrawsIntoIt) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, green);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    ClearColor(fbo, red);
    SampleInto(texture, 0);
    ExpectPixel(target, 4, 4, red, "the sample sees the clear");
}

TEST_F(ClearOrderScenario, AClearedTextureIsBlittedFrom) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, green);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    ClearColor(fbo, red);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glBlitFramebuffer(0, 0, kSize, kSize, 0, 0, kSize, kSize, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ExpectPixel(target, 4, 4, red, "the blit reads the cleared texels");
}

TEST_F(ClearOrderScenario, AnUploadAfterAClearLandsOverIt) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, black);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    ClearColor(fbo, red);
    const std::vector<Pixel> patch(4, green);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, patch.data());
    SampleInto(texture, 0);
    ExpectPixel(target, 1, 1, green, "the upload after the clear is not overwritten by it");
    ExpectPixel(target, 6, 6, red, "the clear holds outside the upload");
}

TEST_F(ClearOrderScenario, AClearedTextureIsReadBack) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, green);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    ClearColor(fbo, red);
    ExpectPixel(fbo, 4, 4, red, "the readback sees the clear");
}

TEST_F(ClearOrderScenario, ARespecifiedTextureKeepsItsNewTexelsAndNotAnEarlierClear) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, green);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    ClearColor(fbo, red);
    const std::vector<Pixel> fresh(size_t(kSize) * kSize, blue);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, fresh.data());
    SampleInto(texture, 0);
    ExpectPixel(target, 4, 4, blue, "the re-specification replaces the cleared texels");
}

TEST_F(ClearOrderScenario, AScissoredClearClearsOnlyItsBox) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewColor(kSize, kSize, green);
    const GLuint fbo = NewFramebuffer(texture, 0);
    glFinish();
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, kSize / 2, kSize);
    ClearColor(fbo, red);
    glDisable(GL_SCISSOR_TEST);
    SampleInto(texture, 0);
    ExpectPixel(target, 1, 4, red, "inside the scissor box");
    ExpectPixel(target, 6, 4, green, "outside the scissor box");
}

} // namespace
} // namespace MGITest
