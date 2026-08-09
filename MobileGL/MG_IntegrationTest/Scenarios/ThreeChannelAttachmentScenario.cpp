// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ThreeChannelAttachmentScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THREE-CHANNEL COLOUR ATTACHMENTS, on a live driver.
//
// The bug: no OpenGL ES driver renders to a three-channel image. EXT_render_snorm covers
// R/RG/RGBA only, EXT_color_buffer_float excludes RGB16F, and RGB integer formats are not
// colour-renderable anywhere. Complementary Reimagined declares colortex1 = RGB8_SNORM and
// colortex2 = RGB16F, so every framebuffer Iris built from them answered
// GL_FRAMEBUFFER_UNSUPPORTED and Iris refused to load the shaderpack. DirectGLES now stores such
// an attachment in its four-channel sibling (GL_RGB8_SNORM -> GL_RGBA16F) and reports the
// substitution as a caveat capability, which is what makes glCheckFramebufferStatus say COMPLETE.
//
// WHY THIS SCENARIO EXISTS RATHER THAN A UNIT TEST. The unit tests in
// MG_Test/Framebuffer/FramebufferTest.cpp drive a HAND-BUILT capability cache: they prove the
// frontend accepts a caveat capability, and prove the colour-mask/clear discipline that keeps a
// widened attachment's stored alpha at 1.0, but they cannot prove that a real driver's probe
// actually PRODUCES that caveat. Only a live glCheckFramebufferStatus can, and the answer is
// per-driver, not per-platform:
//
//   Mesa llvmpipe (the headless CI driver), ES 3.2, GL_TEXTURE_2D colour attachment:
//     COMPLETE               GL_RGB8, GL_RGB16F, GL_R11F_G11F_B10F, every RGBA*
//     INCOMPLETE_ATTACHMENT  GL_RGB8_SNORM, GL_SRGB8, every RGB integer format
//     UNSUPPORTED            GL_RGB32F
//
// So the widening is LIVE on llvmpipe - "the desktop build is unaffected" was simply wrong, and
// the CI retraces were green before the fix only because retrace ignores what
// glCheckFramebufferStatus returns. This scenario is the gate that actually looks.
//
// DirectGLES only. DirectVulkan's format story is its own (Vulkan exposes R8G8B8_SNORM on almost
// nothing, and Magma substitutes on different terms); asserting Espryt's answers there would
// only pin a coincidence.

#include <cmath>
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
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // Two outputs so the mixed case is covered: draw buffer 0 is a natively renderable
        // four-channel format whose alpha the application owns, draw buffer 1 is the widened
        // three-channel one whose alpha the format says is 1.0. Both alphas are deliberately
        // NOT 1.0 in the shader, so an implementation that simply passed the value through would
        // fail the second assertion.
        constexpr const char* kFS = R"(#version 330 core
layout(location = 0) out vec4 oNative;
layout(location = 1) out vec4 oWidened;
void main() {
    oNative  = vec4(1.0, 0.0, 0.0, 0.25);
    oWidened = vec4(0.0, 1.0, 0.0, 0.75);
}
)";

        constexpr int kSize = 16;

        class ThreeChannelAttachmentScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (Gl().BackendName() != "DirectGLES") {
                    GTEST_SKIP() << "three-channel widening is a DirectGLES substitution; backend is "
                                 << Gl().BackendName();
                }
            }

            // A single-level 2D texture in `internalFormat`, or 0 when the driver rejects the
            // storage outright (which is a different failure from rejecting the ATTACHMENT).
            static GLuint MakeTexture(GLenum internalFormat) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kSize, kSize);
                if (glGetError() != GL_NO_ERROR) {
                    glDeleteTextures(1, &texture);
                    return 0;
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            static GLenum SingleAttachmentStatus(GLenum internalFormat) {
                const GLuint texture = MakeTexture(internalFormat);
                if (texture == 0) return GL_NONE;
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &fbo);
                glDeleteTextures(1, &texture);
                return status;
            }
        };

        // THE regression gate for the frontend's answer: this is the exact call Iris makes, and
        // GL_FRAMEBUFFER_UNSUPPORTED here is the whole shaderpack load failure.
        TEST_F(ThreeChannelAttachmentScenario, ThreeChannelColorAttachmentsReportComplete) {
            if (!Ready() || IsSkipped()) return;

            // GL_RGB8 is the control: colour-renderable in ES core, so it must pass with or
            // without any substitution. If it ever fails, nothing below means anything.
            EXPECT_EQ(SingleAttachmentStatus(GL_RGB8), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "GL_RGB8 is ES-core colour-renderable";

            // Complementary Reimagined's colortex1 and colortex2.
            EXPECT_EQ(SingleAttachmentStatus(GL_RGB8_SNORM), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "colortex1 (RGB8_SNORM) must be renderable through the four-channel widening";
            EXPECT_EQ(SingleAttachmentStatus(GL_RGB16F), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "colortex2 (RGB16F) must be renderable, natively or through the widening";

            // The other formats the widening covers. GL_RGB32F only reaches a renderable
            // four-channel form when EXT_color_buffer_float is present, so a half-float-only
            // driver legitimately answers UNSUPPORTED for it - see the POST's per-format row.
            EXPECT_EQ(SingleAttachmentStatus(GL_SRGB8), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            EXPECT_EQ(SingleAttachmentStatus(GL_RGB8UI), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The other half: the substitution has to be INVISIBLE. A three-channel format has no
        // alpha, so GL answers 1.0 for it - and that answer has to hold after a draw that wrote
        // something else into the widened storage's real alpha channel, which is what the
        // colour-mask discipline in SyncRenderState is for. GL_DST_ALPHA blending and
        // glBlitFramebuffer read that stored alpha inside the driver, where no readback fixup can
        // reach it, so "the storage really holds 1.0" is the only workable invariant.
        TEST_F(ThreeChannelAttachmentScenario, WidenedAttachmentReadsBackOpaqueWhileItsNeighbourKeepsItsAlpha) {
            if (!Ready() || IsSkipped()) return;

            std::string error;
            const GLuint program = CompileProgram(kVS, kFS, &error);
            ASSERT_NE(program, 0u) << error;

            const GLuint nativeTexture = MakeTexture(GL_RGBA16F);
            const GLuint widenedTexture = MakeTexture(GL_RGB8_SNORM);
            ASSERT_NE(nativeTexture, 0u);
            ASSERT_NE(widenedTexture, 0u);

            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, nativeTexture, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, widenedTexture, 0);
            const GLenum drawBuffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(2, drawBuffers);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            glViewport(0, 0, kSize, kSize);
            // Alpha 0.0 on purpose: the widened attachment must come back 1.0 anyway, and the
            // native one must come back 0.0 where the draw does not cover it.
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0;
            GLuint vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            std::vector<float> pixels(static_cast<std::size_t>(kSize) * kSize * 4, -1.0f);

            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_FLOAT, pixels.data());
            EXPECT_NEAR(pixels[0], 0.0f, 0.02f) << "widened attachment red";
            EXPECT_NEAR(pixels[1], 1.0f, 0.02f) << "widened attachment green";
            EXPECT_NEAR(pixels[2], 0.0f, 0.02f) << "widened attachment blue";
            EXPECT_NEAR(pixels[3], 1.0f, 0.001f)
                << "a three-channel format has no alpha channel, so GL must report 1.0 for it";

            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_FLOAT, pixels.data());
            EXPECT_NEAR(pixels[0], 1.0f, 0.02f) << "native attachment red";
            EXPECT_NEAR(pixels[3], 0.25f, 0.02f)
                << "the alpha discipline must not leak onto a natively renderable attachment";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
            glDeleteTextures(1, &nativeTexture);
            glDeleteTextures(1, &widenedTexture);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

        // The case above can be satisfied by the readback fixup alone (ForceWideReadAlphaToOne
        // rewrites glReadPixels' alpha), so it does NOT prove the STORED alpha is 1.0. This one
        // does, by asking the driver to read that alpha itself: GL_DST_ALPHA blending multiplies
        // by the destination alpha inside the raster pipeline, where nothing MobileGL does can
        // intervene. Same reason GL_ONE_MINUS_DST_ALPHA and glBlitFramebuffer are covered for
        // free once this holds - and the reason the discipline is a write mask rather than a
        // readback patch.
        //
        // Ablation-checked on llvmpipe, each half separately: disable the alpha doctoring in
        // SyncRenderState and the opaque draw leaves 0.25 in the stored alpha; disable the clear
        // substitution in Clear() and it stays at the application's 0.0. Either way this case
        // reads back the wrong number, which is what makes it a gate rather than a description.
        TEST_F(ThreeChannelAttachmentScenario, DstAlphaBlendingSeesOneInAWidenedAttachment) {
            if (!Ready() || IsSkipped()) return;

            static constexpr const char* kSingleOutFS = R"(#version 330 core
out vec4 oColor;
uniform vec4 uColor;
void main() { oColor = uColor; }
)";
            std::string error;
            const GLuint program = CompileProgram(kVS, kSingleOutFS, &error);
            ASSERT_NE(program, 0u) << error;
            const GLint colorLocation = glGetUniformLocation(program, "uColor");
            ASSERT_GE(colorLocation, 0);

            const GLuint widenedTexture = MakeTexture(GL_RGB8_SNORM);
            ASSERT_NE(widenedTexture, 0u);
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, widenedTexture, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0;
            GLuint vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glViewport(0, 0, kSize, kSize);

            // The clear's alpha is 0.0 and the draw's is 0.25 - neither is the 1.0 the format
            // implies, so both halves of the discipline have to fire for the blend below to see
            // 1.0: the clear substitutes it, and the draw is masked away from it.
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform4f(colorLocation, 0.0f, 1.0f, 0.0f, 0.25f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // dst = stored alpha; src factor GL_DST_ALPHA, dst factor GL_ZERO, source white
            // => the destination colour becomes (storedAlpha, storedAlpha, storedAlpha).
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_ALPHA, GL_ZERO);
            glUniform4f(colorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);

            std::vector<float> pixels(static_cast<std::size_t>(kSize) * kSize * 4, -1.0f);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_FLOAT, pixels.data());
            EXPECT_NEAR(pixels[0], 1.0f, 0.02f)
                << "GL_DST_ALPHA read the stored alpha of a three-channel attachment; it must be 1.0";

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
            glDeleteTextures(1, &widenedTexture);
            glDeleteProgram(program);
            EXPECT_EQ(FirstGLError(), 0u) << GLErrorName(FirstGLError());
        }

    } // namespace
} // namespace MGITest
