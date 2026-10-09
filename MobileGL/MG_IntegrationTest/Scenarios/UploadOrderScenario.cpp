// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UploadOrderScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TEXTURE UPLOAD LANDS EXACTLY WHERE THE APPLICATION ISSUED IT.
//
// Magma uploads a texture's pending levels in a batch submitted on its own, ahead of whatever the
// frame has recorded and not yet submitted. That is only right for an image no recorded command
// references yet; for any other image the recorded work has to be submitted first, or the upload
// overtakes commands that the application issued before it. Each case below puts a command that
// references the texture BEFORE the upload in the same recording and checks the command saw the
// old texels (or that the upload is not overwritten by it):
//   - a draw sampling the texture (the sampled-texture path);
//   - a clear of the texture as a colour attachment, issued as the FIRST command after a
//     glFinish, so it is resolved before the recording that holds it has begun.
// Every case reads back only after all of its commands, so no readback orders them for us.

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
constexpr Pixel red{255, 0, 0, 255}, green{0, 255, 0, 255}, black{0, 0, 0, 255};
constexpr int kWidth = 16, kHeight = 8;
constexpr const char* kVertex = R"(#version 330 core
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(p[gl_VertexID], 0, 1);
})";
constexpr const char* kSample = R"(#version 330 core
uniform sampler2D src;
layout(location=0) out vec4 color;
void main() { color = texelFetch(src, ivec2(0), 0); })";

class UploadOrderScenario : public ScenarioTest {
protected:
    GLuint target = 0, program = 0, vao = 0;
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
        program = Link();
    }

    void TearDown() override {
        if (Ready()) {
            glUseProgram(0);
            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (vao) glDeleteVertexArrays(1, &vao);
            if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
            if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()), textures.data());
            if (program) glDeleteProgram(program);
        }
        ScenarioTest::TearDown();
    }

    static std::vector<Pixel> Pixels(int width, int height, Pixel color) {
        return std::vector<Pixel>(size_t(width) * size_t(height), color);
    }

    GLuint NewTexture(int width, int height, Pixel color) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        const auto pixels = Pixels(width, height, color);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return texture;
    }

    static void Upload(GLuint texture, int width, int height, Pixel color) {
        const auto pixels = Pixels(width, height, color);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
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

    GLuint Link() {
        const GLuint linked = glCreateProgram();
        const std::array<GLenum, 2> kinds{GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        const std::array<const char*, 2> sources{kVertex, kSample};
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

    // Samples `texture` into the 8x8 tile `tile` of the target.
    void SampleInto(GLuint texture, int tile) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glUseProgram(program);
        glBindTexture(GL_TEXTURE_2D, texture);
        glViewport(tile * 8, 0, 8, kHeight);
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

TEST_F(UploadOrderScenario, AnUploadDoesNotReachBackIntoADrawThatSampledTheTextureBeforeIt) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewTexture(2, 2, red);
    glFinish(); // everything above is submitted: the draw below opens a fresh recording
    SampleInto(texture, 0);
    Upload(texture, 2, 2, green);
    SampleInto(texture, 1);
    ExpectPixel(4, 4, red, "the draw issued before the upload samples the old texels");
    ExpectPixel(12, 4, green, "the draw issued after the upload samples the new texels");
}

TEST_F(UploadOrderScenario, AnUploadLandsAfterAClearThatOpenedTheRecording) {
    if (!Ready() || IsSkipped()) return;
    const GLuint texture = NewTexture(2, 2, black);
    const GLuint attachment = NewFramebuffer(texture);
    glFinish(); // the clear below is the first command of a new recording
    glBindFramebuffer(GL_FRAMEBUFFER, attachment);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    Upload(texture, 2, 2, green); // after the clear, so it must win over the clear's blue
    SampleInto(texture, 0);
    ExpectPixel(4, 4, green, "the upload lands after the clear that preceded it");
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
}

} // namespace
} // namespace MGITest
