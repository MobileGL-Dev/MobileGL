// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/InPassClearScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A CLEAR BETWEEN DRAWS INTO THE SAME FRAMEBUFFER.
//
// P15 S0: DirectVulkan's wire arm records a glClear / glClearBuffer* whose targets are the
// attachments of the draw pass for the bound framebuffer INSIDE that pass
// (VulkanRenderer::ClearWireFramebufferInPass, vkCmdClearAttachments) instead of closing it and
// running a pass of its own. What that path has to get right, and what each case pins:
//
//   * ORDER and SCISSOR: a draw, a scissored clear, a second draw - all in one pass. The clear
//     must land after the first draw and before the second, inside the scissor box only.
//   * THE DRAW-BUFFER INDEX: VkClearAttachment::colorAttachment is the SUBPASS colour slot, which
//     the wire pass lays out by draw-buffer slot, not by framebuffer attachment point. With
//     glDrawBuffers({NONE, ATTACHMENT0}) a clear writes attachment 0 and leaves attachment 1 alone;
//     glClearBufferfv(GL_COLOR, 1, ...) is the same slot by number.
//   * DEPTH and STENCIL: a depth / stencil clear between two depth- / stencil-tested draws decides
//     what the second draw passes.
//   * sRGB: a clear of an sRGB attachment with GL_FRAMEBUFFER_SRGB off stores the raw value, with it
//     on the encoded one - through the pass's attachment view, which is the UNORM twin when off.
//
// DirectGLES is the control (a host GL driver); MOBILEGL_MAGMA_INPASS_CLEAR=0 is the standalone
// path, which the same assertions hold for.

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

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
uniform float uDepth;
void main() { gl_Position = vec4(aPos, uDepth, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
uniform vec4 uColor;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() { o0 = uColor; o1 = uColor; }
)";

        constexpr int kSize = 64;

        class InPassClearScenario : public ScenarioTest {
        protected:
            unsigned int program = 0;
            GLuint vao = 0, vbo = 0;

            void Build() {
                std::string error;
                program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(program, 0u) << error;
                static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                glGenBuffers(1, &vbo);
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            }

            void Quad(float r, float g, float b, float a, float depth = 0.0f) {
                glUseProgram(program);
                glUniform4f(glGetUniformLocation(program, "uColor"), r, g, b, a);
                glUniform1f(glGetUniformLocation(program, "uDepth"), depth);
                glBindVertexArray(vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            void TearDown() override {
                if (vbo) glDeleteBuffers(1, &vbo);
                if (vao) glDeleteVertexArrays(1, &vao);
                if (program) glDeleteProgram(program);
                ScenarioTest::TearDown();
            }

            static GLuint Texture(GLenum internalFormat) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kSize, kSize);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                return texture;
            }

            static Rgba8 ReadAt(GLenum readBuffer, int x, int y) {
                glReadBuffer(readBuffer);
                const Image image = ReadPixelsRect(x, y, 1, 1);
                return image.At(0, 0);
            }
        };

        ::testing::AssertionResult Near(const Rgba8& got, int r, int g, int b, int a, int tolerance = 2) {
            const auto close = [&](int v, int want) { return v >= want - tolerance && v <= want + tolerance; };
            if (close(got.r, r) && close(got.g, g) && close(got.b, b) && close(got.a, a))
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure()
                   << "got (" << int(got.r) << ", " << int(got.g) << ", " << int(got.b) << ", " << int(got.a)
                   << "), want (" << r << ", " << g << ", " << b << ", " << a << ")";
        }

    } // namespace

    TEST_F(InPassClearScenario, AScissoredClearBetweenTwoDrawsLandsBetweenThem) {
        if (!Ready()) return;
        Build();
        const GLuint color = Texture(GL_RGBA8);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        Quad(1.0f, 0.0f, 0.0f, 1.0f);                         // red everywhere
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, kSize / 2, kSize);                    // left half
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);                         // green, left half
        glScissor(0, 0, kSize, kSize / 4);                    // bottom quarter
        Quad(0.0f, 0.0f, 1.0f, 1.0f);                         // blue, bottom quarter (scissor clips the draw)
        glDisable(GL_SCISSOR_TEST);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 4, kSize * 3 / 4), 0, 255, 0, 255)) << "left top: the clear";
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize * 3 / 4, kSize * 3 / 4), 255, 0, 0, 255))
            << "right top: the first draw, outside the clear's scissor box";
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 4, kSize / 8), 0, 0, 255, 255))
            << "left bottom: the second draw, after the clear";
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize * 3 / 4, kSize / 8), 0, 0, 255, 255));

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
    }

    TEST_F(InPassClearScenario, AClearWritesTheDrawBufferSlotsAttachmentOnly) {
        if (!Ready()) return;
        Build();
        const GLuint color0 = Texture(GL_RGBA8), color1 = Texture(GL_RGBA8);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, color1, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        // Both attachments red through a draw to both.
        const GLenum both[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, both);
        Quad(1.0f, 0.0f, 0.0f, 1.0f);
        // Draw buffer 1 is attachment 0 and draw buffer 0 is nothing: a draw + clear in that
        // layout touches attachment 0 only.
        const GLenum swapped[] = {GL_NONE, GL_COLOR_ATTACHMENT0};
        glDrawBuffers(2, swapped);
        Quad(1.0f, 1.0f, 0.0f, 1.0f); // yellow into attachment 0 (opens the pass in this layout)
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 0, 0, 255, 255))
            << "attachment 0 is draw buffer 1: cleared to blue";
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT1, kSize / 2, kSize / 2), 255, 0, 0, 255))
            << "attachment 1 is in no draw buffer: still the first draw's red";

        // glClearBufferfv names the draw-buffer slot by number.
        glDrawBuffers(2, both);
        Quad(1.0f, 0.0f, 0.0f, 1.0f);
        const float green[] = {0.0f, 1.0f, 0.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 1, green);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 255, 0, 0, 255)) << "slot 0 untouched";
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT1, kSize / 2, kSize / 2), 0, 255, 0, 255)) << "slot 1 cleared";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color0);
        glDeleteTextures(1, &color1);
    }

    TEST_F(InPassClearScenario, DepthAndStencilClearsBetweenTestedDrawsDecideTheSecondDraw) {
        if (!Ready()) return;
        Build();
        const GLuint color = Texture(GL_RGBA8), depthStencil = Texture(GL_DEPTH24_STENCIL8);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthStencil, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        glClearDepth(1.0);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // Depth: a near draw (z = -0.5 -> 0.25 in the window) writes depth; a clear to 1.0
        // between it and a farther draw (z = 0.0 -> 0.5) is the only way that draw passes LESS.
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        Quad(1.0f, 0.0f, 0.0f, 1.0f, -0.5f);
        glClear(GL_DEPTH_BUFFER_BIT);
        Quad(0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 0, 255, 0, 255))
            << "the depth clear between the draws let the farther draw pass";

        // Stencil: a draw that writes 1 everywhere, a stencil clear to 2, then a draw that only
        // passes where stencil == 2.
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        Quad(1.0f, 0.0f, 0.0f, 1.0f);
        glClearStencil(2);
        glClear(GL_STENCIL_BUFFER_BIT);
        glStencilFunc(GL_EQUAL, 2, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        Quad(0.0f, 0.0f, 1.0f, 1.0f);
        glDisable(GL_STENCIL_TEST);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 0, 0, 255, 255))
            << "the stencil clear between the draws let the EQUAL 2 draw pass";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
        glDeleteTextures(1, &depthStencil);
    }

    TEST_F(InPassClearScenario, AnSrgbClearStoresRawWithEncodingOffAndEncodedWithItOn) {
        if (!Ready()) return;
        Build();
        const GLuint color = Texture(GL_SRGB8_ALPHA8);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        glDisable(GL_FRAMEBUFFER_SRGB);
        Quad(1.0f, 0.0f, 0.0f, 1.0f); // opens the pass
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 128, 128, 128, 255))
            << "GL_FRAMEBUFFER_SRGB off: the raw value";

        glEnable(GL_FRAMEBUFFER_SRGB);
        Quad(1.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_FRAMEBUFFER_SRGB);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(ReadAt(GL_COLOR_ATTACHMENT0, kSize / 2, kSize / 2), 188, 188, 188, 255))
            << "GL_FRAMEBUFFER_SRGB on: linear 0.5 encoded";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &color);
    }

} // namespace MGITest
