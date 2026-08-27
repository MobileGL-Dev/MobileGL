// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/IntegerBorderColorScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN INTEGER GL_TEXTURE_BORDER_COLOR REACHES AN isampler2D AS AN INTEGER.
//
// KHR-GL46.texture_border_clamp.Texture2D{R32I,R32UI} (and the 2DArray/3D siblings) set the border
// colour with glSamplerParameterIiv/Iuiv, sample outside the texture through an integer sampler and
// expect the value back. MobileGL returned 1132396544 on Espryt - which is 0x437F0000, the IEEE-754
// bits of 255.0f, i.e. the float border-colour register read through an integer sampler - and 0 on
// Magma, where the border fell through to VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK.
//
// Two independent halves, and this scenario covers both because it goes through the frontend:
//
//   * the STATE had no record of which entry point wrote the border colour. All three
//     representations are kept numerically in step, so the value alone cannot say whether the
//     application called glTexParameterfv or glTexParameterIiv.
//   * each backend then had exactly one border-colour call site: glTexParameterfv /
//     glSamplerParameterfv on DirectGLES, and a snap-to-one-of-four-predefined-values on
//     DirectVulkan that never emitted the VK_BORDER_COLOR_INT_* family at all.
//
// The border value is deliberately outside every predefined VkBorderColor and outside anything a
// float register could round-trip: (255, -1, 7, 3) is neither transparent black, nor opaque black,
// nor opaque white, so on DirectVulkan it can only be delivered through VK_EXT_custom_border_color.
// That makes the scenario a real test of the extension path on lavapipe rather than a palette hit.
//
// Both an integer image view and an integer border colour are involved, which is the other half of
// the Vulkan rule: VK_BORDER_COLOR_FLOAT_* on an integer image view is undefined behaviour
// regardless of the value, so even a border of (0,0,0,1) has to resolve to INT_OPAQUE_BLACK.
// InsideTexelsAreUnaffected is what keeps that from being asserted vacuously.

#include <array>
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

        constexpr int kOutputWidth = 8;
        constexpr int kOutputHeight = 8;

        // The texture's own texel, and the border. Neither is a Vulkan palette entry, and the border
        // is deliberately not derivable from the texel.
        constexpr std::int32_t kInsideTexel[4] = {11, 22, 33, 44};
        constexpr std::int32_t kBorderColor[4] = {255, -1, 7, 3};

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

        // One channel per draw, so a failure names the component that is wrong. The coordinate is a
        // uniform rather than a literal so the same program serves the border sample and the inside
        // sample and nothing can be constant-folded differently between them.
        std::string FragmentSource(int channel) {
            static const char* kChannels[4] = {"x", "y", "z", "w"};
            return std::string("#version 330 core\n\nuniform isampler2D smp;\nuniform vec2 uCoord;\n\n"
                               "out int out_color;\n\nvoid main()\n{\n    out_color = texture(smp, uCoord).") +
                   kChannels[channel] + ";\n}\n";
        }

        class IntegerBorderColorScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                // 2x2 RGBA32I. Integer textures are not filterable, so NEAREST is mandatory.
                const std::int32_t texels[4][4] = {{kInsideTexel[0], kInsideTexel[1], kInsideTexel[2], kInsideTexel[3]},
                                                  {kInsideTexel[0], kInsideTexel[1], kInsideTexel[2], kInsideTexel[3]},
                                                  {kInsideTexel[0], kInsideTexel[1], kInsideTexel[2], kInsideTexel[3]},
                                                  {kInsideTexel[0], kInsideTexel[1], kInsideTexel[2], kInsideTexel[3]}};
                glGenTextures(1, &m_sourceTexture);
                glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32I, 2, 2);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA_INTEGER, GL_INT, texels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                ASSERT_EQ(FirstGLError(), 0u) << "source texture setup left a GL error behind";

                // 8x8 R32I render target: an integer readback, so nothing is normalized on the way
                // out and a wrong value is reported as the number it actually was.
                glGenTextures(1, &m_outputTexture);
                glBindTexture(GL_TEXTURE_2D, m_outputTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32I, kOutputWidth, kOutputHeight);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_outputTexture, 0);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
                glGenVertexArrays(1, &m_vao);
                ASSERT_EQ(FirstGLError(), 0u) << "output framebuffer setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_sampler != 0) {
                    glBindSampler(0, 0);
                    glDeleteSamplers(1, &m_sampler);
                    m_sampler = 0;
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_outputTexture != 0) glDeleteTextures(1, &m_outputTexture);
                if (m_sourceTexture != 0) glDeleteTextures(1, &m_sourceTexture);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            // Samples `coord` through the integer sampler and returns every texel the draw wrote.
            std::vector<std::int32_t> RenderChannel(int channel, float coordX, float coordY) {
                const std::string fragment = FragmentSource(channel);
                std::string error;
                const unsigned int program = CompileProgram(kVertexSource, fragment.c_str(), &error);
                if (program == 0) {
                    ADD_FAILURE() << "channel " << channel << ": program did not build: " << error;
                    return {};
                }

                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glViewport(0, 0, kOutputWidth, kOutputHeight);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                // A clear value nothing under test can produce, so an undrawn target is not mistaken
                // for a correct one.
                const GLint clearValue[4] = {-559038737, 0, 0, 0};
                glClearBufferiv(GL_COLOR, 0, clearValue);

                glUseProgram(program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
                glUniform1i(glGetUniformLocation(program, "smp"), 0);
                glUniform2f(glGetUniformLocation(program, "uCoord"), coordX, coordY);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);

                std::vector<std::int32_t> texels(static_cast<std::size_t>(kOutputWidth) * kOutputHeight, 0);
                glReadPixels(0, 0, kOutputWidth, kOutputHeight, GL_RED_INTEGER, GL_INT, texels.data());
                glUseProgram(0);
                glDeleteProgram(program);
                return texels;
            }

            void ExpectAllTexels(const char* what, int channel, std::int32_t expected,
                                 const std::vector<std::int32_t>& texels) {
                if (texels.empty()) return;
                std::size_t offenders = 0;
                std::int32_t firstBad = 0;
                for (const std::int32_t texel : texels) {
                    if (texel == expected) continue;
                    if (offenders == 0) firstBad = texel;
                    ++offenders;
                }
                EXPECT_EQ(offenders, 0u) << what << " component " << channel << " returned " << firstBad
                                         << " instead of " << expected << " (" << offenders << " of " << texels.size()
                                         << " texels wrong)";
            }

            // Every component of the border, in one place, so both the texture-object and the
            // sampler-object case assert exactly the same thing.
            void ExpectBorderIsDelivered(const char* what) {
                for (int channel = 0; channel < 4; ++channel) {
                    // (-0.5, -0.5) is a full texture width outside the image on both axes, so
                    // CLAMP_TO_BORDER can only answer with the border colour.
                    const std::vector<std::int32_t> texels = RenderChannel(channel, -0.5f, -0.5f);
                    EXPECT_EQ(FirstGLError(), 0u) << what << ": the border draw left a GL error behind";
                    ExpectAllTexels(what, channel, kBorderColor[channel], texels);
                }
            }

            GLuint m_sourceTexture = 0;
            GLuint m_outputTexture = 0;
            GLuint m_fbo = 0;
            GLuint m_vao = 0;
            GLuint m_sampler = 0;
        };

    } // namespace

    // The floor, and the control that keeps the two tests below from passing vacuously: an INSIDE
    // sample has to fetch the texture's own texel. If this fails the sampler, the shader or the
    // integer readback is broken and nothing about the border colour has been measured.
    TEST_F(IntegerBorderColorScenario, InsideTexelsAreUnaffectedByTheBorderColour) {
        if (!Ready()) GTEST_SKIP();

        glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
        glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, kBorderColor);
        ASSERT_EQ(FirstGLError(), 0u) << "glTexParameterIiv(GL_TEXTURE_BORDER_COLOR) was rejected";

        for (int channel = 0; channel < 4; ++channel) {
            const std::vector<std::int32_t> texels = RenderChannel(channel, 0.5f, 0.5f);
            EXPECT_EQ(FirstGLError(), 0u) << "the inside draw left a GL error behind";
            ExpectAllTexels("inside sample", channel, kInsideTexel[channel], texels);
        }
        Gl().EndFrame();
    }

    // The regression, texture-object spelling. glTexParameterIiv is the entry point the frontend
    // already accepted and then flattened into the same FloatVec4 every other spelling wrote.
    TEST_F(IntegerBorderColorScenario, TexParameterIivBorderColourSurvivesToAnIntegerSampler) {
        if (!Ready()) GTEST_SKIP();

        glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
        glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, kBorderColor);
        ASSERT_EQ(FirstGLError(), 0u) << "glTexParameterIiv(GL_TEXTURE_BORDER_COLOR) was rejected";

        ExpectBorderIsDelivered("glTexParameterIiv");
        Gl().EndFrame();
    }

    // The regression, sampler-object spelling - which is the one the conformance cases actually use,
    // and a separate code path in both backends (BackendSamplerObject::Sync on DirectGLES, and the
    // sampler cache key on DirectVulkan, where a border colour that is not part of the key would
    // alias two samplers that differ only in it).
    TEST_F(IntegerBorderColorScenario, SamplerParameterIivBorderColourSurvivesToAnIntegerSampler) {
        if (!Ready()) GTEST_SKIP();

        glGenSamplers(1, &m_sampler);
        ASSERT_NE(m_sampler, 0u);
        glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glSamplerParameterIiv(m_sampler, GL_TEXTURE_BORDER_COLOR, kBorderColor);
        ASSERT_EQ(FirstGLError(), 0u) << "sampler-object setup was rejected";

        // The texture object carries a DIFFERENT border colour, so a pass here cannot come from the
        // texture's own state leaking through: GL 4.6 core 8.10 says a bound sampler object's state
        // wins over the texture's for every sampling parameter.
        const std::int32_t decoyBorder[4] = {0, 0, 0, 0};
        glBindTexture(GL_TEXTURE_2D, m_sourceTexture);
        glTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, decoyBorder);
        glBindSampler(0, m_sampler);
        ASSERT_EQ(FirstGLError(), 0u) << "binding the sampler object was rejected";

        ExpectBorderIsDelivered("glSamplerParameterIiv");
        glBindSampler(0, 0);
        Gl().EndFrame();
    }

} // namespace MGITest
