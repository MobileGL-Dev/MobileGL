// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ImageSizeAfterRespecScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A DRAW READS imageSize() AFTER THE IMAGE TEXTURE IS RE-SPECIFIED.
//
// KHR-GL43.shader_image_size.advanced-changeSize reduced to its mechanism. The application binds
// a texture to an image unit ONCE, draws, then re-specifies that same texture with a new size
// through glTexImage2D and draws again - without touching the image unit. GL says the unit
// references the texture OBJECT, so the second draw must see the new dimensions.
//
// On Espryt it did not, and the reason is two facts meeting:
//
//   1. ES 3.1 only allows IMMUTABLE storage on an image unit, so the backend forces glTexStorage
//      backing on any texture that reaches one (SyncTextureObjectToBackend's
//      imageBindableStorageRequired). Immutable storage cannot be redefined, so a glTexImage2D
//      that changes size or format has to MINT A NEW ES TEXTURE NAME.
//   2. The draw path never re-issued glBindImageTexture. Image units were established eagerly,
//      once, when the application called glBindImageTexture, and PrepareForDraw only ever
//      re-synced SAMPLED textures - so the unit kept pointing at the deleted name and
//      imageSize() reported whatever that stale binding still meant.
//
// A dispatch was never affected: PrepareForCompute has always swept the image units. This is a
// draw-path scenario for exactly that reason - a compute-shaped case cannot see the defect.
//
// Both backends run it. Magma re-derives its image descriptors per draw and so was never wrong
// here, which makes it the control: the two backends have to agree on what the second draw sees.

#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
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

        constexpr int kTargetSize = 8;

        constexpr const char* kVS = R"(#version 430 core
void main()
{
    // A single triangle that covers the whole target, with no vertex buffer at all: the
    // scenario is about the image unit, so nothing else may be able to make it fail.
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 3.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  3.0, 0.0, 1.0); break;
    }
}
)";

        // Green when the image the unit currently holds has the size the application last gave
        // it, red otherwise - the conformance case's own comparison, and its own colours.
        constexpr const char* kFS = R"(#version 430 core
layout(rgba8) readonly uniform image2D g_image;
uniform ivec2 g_expected_size;
layout(location = 0) out vec4 o_color;
void main()
{
    o_color = (imageSize(g_image) == g_expected_size) ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        class ImageSizeAfterRespecScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_color != 0) glDeleteTextures(1, &m_color);
                if (m_image != 0) glDeleteTextures(1, &m_image);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_program = m_fbo = m_color = m_image = m_vao = 0;
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            // imageSize() needs a fragment-stage image uniform; a driver that serves none should
            // skip rather than fail.
            bool FragmentImagesAreUsable() const {
                GLint maxImageUnits = 0;
                GLint maxFragmentImageUniforms = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                glGetIntegerv(GL_MAX_FRAGMENT_IMAGE_UNIFORMS, &maxFragmentImageUniforms);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits >= 1 && maxFragmentImageUniforms >= 1;
            }

            GLuint MakeProgram() {
                const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                glShaderSource(vs, 1, &kVS, nullptr);
                glShaderSource(fs, 1, &kFS, nullptr);
                glCompileShader(vs);
                glCompileShader(fs);
                for (const GLuint shader : {vs, fs}) {
                    GLint compiled = GL_FALSE;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    if (compiled == GL_FALSE) {
                        char log[4096] = {};
                        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                        ADD_FAILURE() << "a shader did not compile: " << log;
                        glDeleteShader(vs);
                        glDeleteShader(fs);
                        return 0;
                    }
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                glDeleteShader(vs);
                glDeleteShader(fs);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            void MakeRenderTarget() {
                glGenTextures(1, &m_color);
                glBindTexture(GL_TEXTURE_2D, m_color);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTargetSize, kTargetSize, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             nullptr);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_color, 0);
            }

            // Draw once with `expected` pushed to the shader and report the centre pixel.
            void DrawAndReadCentre(int expectedWidth, int expectedHeight, unsigned char (&centre)[4]) {
                const GLint location = glGetUniformLocation(m_program, "g_expected_size");
                ASSERT_NE(location, -1) << "the program has no g_expected_size uniform";
                glUseProgram(m_program);
                glUniform2i(location, expectedWidth, expectedHeight);
                glViewport(0, 0, kTargetSize, kTargetSize);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                ASSERT_EQ(FirstGLError(), 0u) << "the draw left a GL error";

                std::vector<unsigned char> pixels(static_cast<std::size_t>(kTargetSize) * kTargetSize * 4, 0);
                glReadPixels(0, 0, kTargetSize, kTargetSize, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                ASSERT_EQ(FirstGLError(), 0u) << "reading the target back errored";
                const std::size_t offset =
                    (static_cast<std::size_t>(kTargetSize / 2) * kTargetSize + kTargetSize / 2) * 4;
                for (int i = 0; i < 4; ++i) {
                    centre[i] = pixels[offset + static_cast<std::size_t>(i)];
                }
            }

            GLuint m_program = 0;
            GLuint m_fbo = 0;
            GLuint m_color = 0;
            GLuint m_image = 0;
            GLuint m_vao = 0;
        };

    } // namespace

    // The whole conformance shape: bind once, draw, re-specify the SAME texture smaller, draw
    // again. The first draw is the control - it proves the binding and the shader work at all -
    // and the second is the regression pin. Blue would mean the draw never ran; red means the
    // image unit answered with the size the texture had BEFORE the re-spec.
    TEST_F(ImageSizeAfterRespecScenario, ADrawSeesTheNewSizeOfARespecifiedImageTexture) {
        if (!Ready()) return;
        if (!FragmentImagesAreUsable()) GTEST_SKIP() << "no fragment-stage image uniform available";

        m_program = MakeProgram();
        if (m_program == 0) return;
        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);
        MakeRenderTarget();
        ASSERT_EQ(FirstGLError(), 0u) << "setting the render target up errored";

        glGenTextures(1, &m_image);
        glBindTexture(GL_TEXTURE_2D, m_image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindImageTexture(0, m_image, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        ASSERT_EQ(FirstGLError(), 0u) << "binding the image texture errored";

        unsigned char centre[4] = {0, 0, 0, 0};
        DrawAndReadCentre(32, 32, centre);
        EXPECT_EQ(static_cast<int>(centre[0]), 0) << "the FIRST draw already disagrees about imageSize(): got ("
                                                  << static_cast<int>(centre[0]) << ", "
                                                  << static_cast<int>(centre[1]) << ", "
                                                  << static_cast<int>(centre[2]) << ")";
        EXPECT_EQ(static_cast<int>(centre[1]), 255);

        // The re-spec. The image unit is deliberately NOT re-bound: GL 4.6 core 8.26 says the
        // unit references the texture object, so this alone has to be visible to the next draw.
        glBindTexture(GL_TEXTURE_2D, m_image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        ASSERT_EQ(FirstGLError(), 0u) << "re-specifying the image texture errored";

        DrawAndReadCentre(16, 16, centre);
        EXPECT_EQ(static_cast<int>(centre[0]), 0)
            << "after the re-spec the draw still sees the OLD image size; centre pixel was ("
            << static_cast<int>(centre[0]) << ", " << static_cast<int>(centre[1]) << ", "
            << static_cast<int>(centre[2]) << ")";
        EXPECT_EQ(static_cast<int>(centre[1]), 255);
    }

} // namespace MGITest
