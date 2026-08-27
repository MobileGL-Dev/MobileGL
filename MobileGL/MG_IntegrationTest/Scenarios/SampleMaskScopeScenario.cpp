// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SampleMaskScopeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GL_SAMPLE_MASK IS A MULTISAMPLE FRAGMENT OPERATION, SO IT DOES NOTHING AT ONE SAMPLE.
//
// GL 4.6 core 17.3.3 groups alpha-to-coverage, sample coverage and the sample mask together and
// says they make no change "if MULTISAMPLE is disabled, or if the value of SAMPLE_BUFFERS is not
// one". SAMPLE_BUFFERS is 0 for a single-sample framebuffer, so on one the mask is inert whatever
// glSampleMaski last wrote.
//
// Vulkan has no such rule. VkPipelineMultisampleStateCreateInfo::pSampleMask is ANDed with
// rasterization coverage at every rasterizationSamples, and at one sample that coverage is bit 0
// alone - so a mask with bit 0 clear discards every fragment of every primitive. Plumbing
// glSampleMaski straight into pSampleMask therefore turned an ordinary and legal GL sequence into
// a fully black draw:
//
//     glEnable(GL_SAMPLE_MASK); glSampleMaski(0, 0x2);   // while an MSAA target is bound
//     ... render ...
//     glBindFramebuffer(GL_FRAMEBUFFER, 0); draw a fullscreen quad to present
//
// Neither piece of state is per-framebuffer, so nothing resets it when the target changes, and
// dEQP/GL-CTS multisample cases leave exactly these masks behind. That is the MSAA-then-present
// shape every application uses.
//
// The cases below are single-sample by construction (the scenario harness's colour FBO), so each
// one asserts that the mask changed nothing.

#include <string>

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

        constexpr int kFboSize = 32;

        constexpr const char* kQuadVertexSource = R"(#version 430 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        constexpr const char* kGreenFragmentSource = R"(#version 430 core
out vec4 o_color;
void main() {
    o_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        class SampleMaskScopeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                m_target = MakeColorFbo(kFboSize, kFboSize);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                glGenVertexArrays(1, &m_vao);
                std::string error;
                m_program = CompileProgram(kQuadVertexSource, kGreenFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;
            }

            void TearDown() override {
                if (!Ready()) return;
                // Process-wide GL state: leaving it set would hand the next scenario in this
                // process the very bug under test.
                glDisable(GL_SAMPLE_MASK);
                glSampleMaski(0, 0xFFFFFFFFu);
                glBindVertexArray(0);
                glUseProgram(0);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                DestroyColorFbo(m_target);
                ScenarioTest::TearDown();
            }

            void ExpectQuadStillPaints(const char* what) {
                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glBindVertexArray(m_vao);
                glUseProgram(m_program);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                EXPECT_EQ(FirstGLError(), 0u) << what << ": the draw raised a GL error";

                const Image image = ReadPixels(kFboSize, kFboSize);
                ASSERT_FALSE(image.Empty()) << what << ": the readback came back empty";
                EXPECT_TRUE(RegionIsMostly(image, 0, kFboSize - 1, 0, kFboSize - 1, "green", 0.0, what))
                    << what << ": an all-black target means the sample mask discarded every fragment, "
                    << "which GL says it cannot do on a single-sample framebuffer";
            }

            ColorFbo m_target{};
            GLuint m_vao = 0;
            unsigned int m_program = 0;
        };

    } // namespace

    // The exact reported shape: bit 0 clear, so the single sample of a single-sample target is
    // masked off if the mask is applied at all.
    TEST_F(SampleMaskScopeScenario, AMaskWithBitZeroClearDoesNotDiscardASingleSampleDraw) {
        if (!Ready() || IsSkipped()) return;
        glEnable(GL_SAMPLE_MASK);
        glSampleMaski(0, 0x2);
        ASSERT_EQ(FirstGLError(), 0u) << "setting the sample mask raised a GL error";
        ExpectQuadStillPaints("GL_SAMPLE_MASK enabled with mask 0x2");
    }

    // Zero is the strongest form of the same thing, and the mask value the CTS's mask_zero cases
    // set.
    TEST_F(SampleMaskScopeScenario, AZeroMaskDoesNotDiscardASingleSampleDraw) {
        if (!Ready() || IsSkipped()) return;
        glEnable(GL_SAMPLE_MASK);
        glSampleMaski(0, 0x0);
        ASSERT_EQ(FirstGLError(), 0u) << "setting the sample mask raised a GL error";
        ExpectQuadStillPaints("GL_SAMPLE_MASK enabled with mask 0");
    }

    // Control: the same mask word with the capability disabled has never had any effect, so this
    // one passed before the fix too. It is here so a regression that ignores the enable bit
    // instead of the sample count is still caught.
    TEST_F(SampleMaskScopeScenario, ADisabledSampleMaskDoesNotDiscardASingleSampleDraw) {
        if (!Ready() || IsSkipped()) return;
        glDisable(GL_SAMPLE_MASK);
        glSampleMaski(0, 0x0);
        ASSERT_EQ(FirstGLError(), 0u) << "setting the sample mask raised a GL error";
        ExpectQuadStillPaints("GL_SAMPLE_MASK disabled with mask 0");
    }

    // The mask is state, not a draw parameter, so a second draw after the first must not inherit
    // a pipeline built while the memo word and the payload disagreed. Two draws either side of a
    // mask change, both to the same single-sample target, both required to paint.
    TEST_F(SampleMaskScopeScenario, ChangingTheMaskBetweenSingleSampleDrawsKeepsBothPainting) {
        if (!Ready() || IsSkipped()) return;
        glEnable(GL_SAMPLE_MASK);
        glSampleMaski(0, 0xFFFFFFFFu);
        ExpectQuadStillPaints("first draw, full mask");
        glSampleMaski(0, 0x2);
        ASSERT_EQ(FirstGLError(), 0u) << "changing the sample mask raised a GL error";
        ExpectQuadStillPaints("second draw, mask 0x2");
    }

    // GL_MAX_SAMPLE_MASK_WORDS must be 1 on both backends: MobileGL stores one word and
    // SampleMaski_State raises GL_INVALID_VALUE for any maskNumber above 0, so advertising more
    // makes dEQP's per-case gluStateReset - which issues glSampleMaski up to the advertised count
    // - fail every case. DirectGLES clamped; DirectVulkan forwarded the raw device limit.
    TEST_F(SampleMaskScopeScenario, TheAdvertisedSampleMaskWordCountMatchesWhatSampleMaskiAccepts) {
        if (!Ready() || IsSkipped()) return;
        GLint words = 0;
        glGetIntegerv(GL_MAX_SAMPLE_MASK_WORDS, &words);
        ASSERT_EQ(FirstGLError(), 0u) << "querying GL_MAX_SAMPLE_MASK_WORDS raised a GL error";
        EXPECT_EQ(words, 1) << "every word below the advertised count must be writable, and only word 0 is";
        for (GLint word = 0; word < words; ++word) {
            glSampleMaski(static_cast<GLuint>(word), 0xFFFFFFFFu);
            EXPECT_EQ(FirstGLError(), 0u) << "glSampleMaski(" << word << ", ...) was refused although "
                                          << "GL_MAX_SAMPLE_MASK_WORDS advertises " << words << " words";
        }
    }

} // namespace MGITest
