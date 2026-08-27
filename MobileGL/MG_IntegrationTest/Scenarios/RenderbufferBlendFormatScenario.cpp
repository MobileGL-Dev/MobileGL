// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/RenderbufferBlendFormatScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - BLENDING WORKS ON A RENDERBUFFER WHOSE GL FORMAT HAS NO EXACT VkFormat.
//
// DirectVulkan force-disables blending on an attachment whose VkFormat lacks
// VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, which is the right thing to do - blending on such a
// format is invalid pipeline state. The probe has to ask about the format the attachment ACTUALLY
// has, and for renderbuffers it asked a different question from the one that created the image: the
// image comes from ResolveTextureFormatInfo (which widens GL formats with no Vulkan twin onto a real
// one) while the probe used the strict 1:1 converter, which answers VK_FORMAT_UNDEFINED for RGBA2,
// RGBA12, RGB10, RGB12, RGB16 and the three-channel formats, and the 16-bit packed formats for RGBA4
// and RGB5_A1.
//
// VkFormatProperties for VK_FORMAT_UNDEFINED are all zero, so the probe concluded "not blendable"
// and every pipeline for that attachment was built with blendEnable = VK_FALSE - permanently, and
// silently apart from one log line. The source colour then overwrites the destination instead of
// blending with it, which is a wrong PICTURE, not a wrong error code.
//
// GL_RGB8 is the ordinary shape and is what this scenario leads with: it is a required
// colour-renderable format, its image has been R8G8B8A8_UNORM all along, and the probe asked about
// the 24-bit R8G8B8_UNORM that most drivers do not support at all. GL_RGBA4 covers the other half -
// a format whose probe answered a real-but-different VkFormat.
//
// DirectGLES is the control: it forwards the renderbuffer to the ES driver and blends whatever the
// driver blends, so a disagreement between the two backends is the defect.

#include <cstdint>
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

        constexpr int kExtent = 16;

        constexpr const char* kVertexSource = R"(#version 330 core
void main()
{
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, 1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, 1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,-1.0, 0.0, 1.0); break;
      case 3: gl_Position = vec4( 1.0,-1.0, 0.0, 1.0); break;
    }
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main()
{
    fragColor = uColor;
}
)";

        class RenderbufferBlendFormatScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << "program did not build: " << error;
                ASSERT_EQ(FirstGLError(), 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                Destroy();
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            void Destroy() {
                if (m_fbo != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &m_fbo);
                    m_fbo = 0;
                }
                if (m_renderbuffer != 0) {
                    glDeleteRenderbuffers(1, &m_renderbuffer);
                    m_renderbuffer = 0;
                }
            }

            // Returns false (having skipped, not failed) when the driver will not give us a complete
            // framebuffer for this format - GL only requires a subset of formats to be
            // colour-renderable, and the point of the scenario is blending, not format support.
            bool MakeTarget(GLenum internalFormat) {
                Destroy();
                glGenRenderbuffers(1, &m_renderbuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
                glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, kExtent, kExtent);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_renderbuffer);
                const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
                return status == GL_FRAMEBUFFER_COMPLETE;
            }

            void DrawColor(float r, float g, float b, float a) {
                glUseProgram(m_program);
                glUniform4f(glGetUniformLocation(m_program, "uColor"), r, g, b, a);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                glUseProgram(0);
            }

            GLuint m_renderbuffer = 0;
            GLuint m_fbo = 0;
            GLuint m_vao = 0;
            unsigned int m_program = 0;
        };

        // One draw of opaque black, then a 50%-alpha white draw over it with the ordinary
        // SRC_ALPHA / ONE_MINUS_SRC_ALPHA function. Blending gives mid-grey; a pipeline built with
        // blendEnable = VK_FALSE gives white, because the source simply overwrites.
        //
        // The tolerance is wide on purpose: RGBA4 has four bits per channel, so "mid-grey" is one of
        // a handful of representable values and the test must not become a quantisation test.
        void ExpectBlendedRatherThanOverwritten(const char* what) {
            const Image image = ReadPixels(kExtent, kExtent);
            ASSERT_FALSE(image.Empty()) << what;
            const Rgba8 centre = image.At(kExtent / 2, kExtent / 2);
            EXPECT_GT(int(centre.r), 40) << what << ": got " << centre << ", which is darker than a blend of "
                                                              "black and 50% white";
            EXPECT_LT(int(centre.r), 215) << what << ": got " << centre
                                          << ", which is the source colour - blending was disabled";
        }

    } // namespace

    // The ordinary case, and the one broken today rather than only after the format table was
    // unified: a three-channel colour renderbuffer. Its image has been R8G8B8A8_UNORM all along while
    // the blend probe asked about R8G8B8_UNORM, which most drivers do not support at all.
    TEST_F(RenderbufferBlendFormatScenario, BlendingWorksOnAThreeChannelRenderbuffer) {
        if (!Ready()) GTEST_SKIP();
        if (!MakeTarget(GL_RGB8)) GTEST_SKIP() << "GL_RGB8 renderbuffer is not framebuffer-complete here";

        glViewport(0, 0, kExtent, kExtent);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        DrawColor(0.0f, 0.0f, 0.0f, 1.0f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        DrawColor(1.0f, 1.0f, 1.0f, 0.5f);
        glDisable(GL_BLEND);
        EXPECT_EQ(FirstGLError(), 0u) << "the blended draw left a GL error behind";

        ExpectBlendedRatherThanOverwritten("GL_RGB8");
        Gl().EndFrame();
    }

    // The other half: a format whose strict converter answers a real-but-different VkFormat
    // (R4G4B4A4_UNORM_PACK16) while the image is R8G8B8A8_UNORM. Blend support for the packed 16-bit
    // formats is optional in Vulkan, so the probe could legitimately answer "no" for a format the
    // attachment does not have.
    TEST_F(RenderbufferBlendFormatScenario, BlendingWorksOnALowBitPackedRenderbuffer) {
        if (!Ready()) GTEST_SKIP();
        if (!MakeTarget(GL_RGBA4)) GTEST_SKIP() << "GL_RGBA4 renderbuffer is not framebuffer-complete here";

        glViewport(0, 0, kExtent, kExtent);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        DrawColor(0.0f, 0.0f, 0.0f, 1.0f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        DrawColor(1.0f, 1.0f, 1.0f, 0.5f);
        glDisable(GL_BLEND);
        EXPECT_EQ(FirstGLError(), 0u) << "the blended draw left a GL error behind";

        ExpectBlendedRatherThanOverwritten("GL_RGBA4");
        Gl().EndFrame();
    }

    // The control that keeps both of the above honest: the same sequence on the format whose probe
    // and image always agreed. If this one ever fails, the scenario is measuring the blend setup
    // rather than the format resolution.
    TEST_F(RenderbufferBlendFormatScenario, BlendingWorksOnAnRgba8Renderbuffer) {
        if (!Ready()) GTEST_SKIP();
        if (!MakeTarget(GL_RGBA8)) GTEST_SKIP() << "GL_RGBA8 renderbuffer is not framebuffer-complete here";

        glViewport(0, 0, kExtent, kExtent);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        DrawColor(0.0f, 0.0f, 0.0f, 1.0f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        DrawColor(1.0f, 1.0f, 1.0f, 0.5f);
        glDisable(GL_BLEND);
        EXPECT_EQ(FirstGLError(), 0u) << "the blended draw left a GL error behind";

        ExpectBlendedRatherThanOverwritten("GL_RGBA8");
        Gl().EndFrame();
    }

} // namespace MGITest
