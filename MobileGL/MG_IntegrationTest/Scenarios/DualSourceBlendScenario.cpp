// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DualSourceBlendScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A DUAL-SOURCE BLEND DRAW HAS TO SURVIVE ON EVERY DRIVER.
//
// GL_SRC1_COLOR / GL_ONE_MINUS_SRC1_COLOR / GL_SRC1_ALPHA / GL_ONE_MINUS_SRC1_ALPHA
// (ARB_blend_func_extended, core since 3.3) need a backend capability that not every device has:
// GL_EXT_blend_func_extended on the ES driver, or the dualSrcBlend device feature on Vulkan. When
// the capability IS there both backends translate the factors properly, and that has always
// worked. When it is NOT, both backends used to THROW_EXCEPTION at draw time - and
// MG_Util/Types.h's THROW_EXCEPTION is a plain `throw`, with no catch anywhere in MG_Impl or
// MG_Backend, so the exception unwound out through the C GL ABI and killed the process. An
// application asking for a blend factor the device cannot do is a picture problem, never a reason
// to take the process down.
//
// Both are now a DECLINE: the attachment is drawn with blending off and neutral One/Zero factors,
// and the loss is logged once. So a dual-source draw has exactly two defined outcomes, and this
// scenario pins that it lands on one of them and never on a crash:
//
//   capability present - src0 * src1 + dst * (1 - src1)
//   capability absent  - src0, written straight through
//
// What each CI lane actually reaches: lavapipe has dualSrcBlend, so the DirectVulkan lane runs the
// whole sequence and measures the blend. Mesa's GLES front end on llvmpipe has no
// GL_EXT_blend_func_extended, so the ESSL stage carrying `layout(index = 1)` never compiles and the
// program renders nothing - the DirectGLES lane therefore SKIPS on the capability probe in SetUp
// rather than measuring a picture the driver never produced. The DECLINE arm itself - the path this
// scenario exists for - is unit-tested against stubbed capabilities in
// MG_Test/Framebuffer/FramebufferTest.cpp (DualSourceBlendIsDeclinedRatherThanThrownWhenTheExtensionIsMissing),
// which is the only place it can be reached without a driver that lacks the extension.
//
// The Vulkan half has a second edge the last case covers: the dual-source VUIDs
// (VUID-VkPipelineColorBlendAttachmentState-srcColorBlendFactor-00608 and its three siblings)
// forbid a VK_BLEND_FACTOR_SRC1_* anywhere in VkPipelineColorBlendAttachmentState without the
// feature, whatever blendEnable says - so leaving the factors in place while clearing the enable
// would still be invalid pipeline state.

#include <cstdint>
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

        // Two outputs on the SAME location, indices 0 and 1: the shader-side spelling of
        // dual-source output (GLSL 3.30 4.4.2, the `index` layout qualifier). No
        // glBindFragDataLocationIndexed needed, which keeps the program buildable through the
        // harness's compile-and-link helper.
        constexpr const char* kDualSourceFragmentSource = R"(#version 330 core
uniform vec4 uSrc0;
uniform vec4 uSrc1;
layout(location = 0, index = 0) out vec4 fragColor0;
layout(location = 0, index = 1) out vec4 fragColor1;
void main()
{
    fragColor0 = uSrc0;
    fragColor1 = uSrc1;
}
)";

        class DualSourceBlendScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glGenRenderbuffers(1, &m_renderbuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kExtent, kExtent);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_renderbuffer);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

                std::string error;
                m_program = CompileProgram(kVertexSource, kDualSourceFragmentSource, &error);
                m_programError = error;
                glViewport(0, 0, kExtent, kExtent);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);

                // Capability probe, not an assertion. A GL link that succeeded is not proof that
                // the BACKEND can run the program: DirectGLES transpiles to ESSL lazily at first
                // use, and GLSL ES has no `index` layout qualifier outside
                // GL_EXT_blend_func_extended, so on a driver without it the stage never compiles
                // and the draw renders nothing. One unblended white draw tells the two apart, and
                // the cases skip rather than measure a picture the driver never produced.
                if (m_program != 0) {
                    glDisable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ZERO);
                    Draw(/*src0=*/1.0f, /*src1=*/1.0f);
                    glFinish();
                    const Image probe = ReadPixels(kExtent, kExtent);
                    m_programRenders =
                        !probe.Empty() && static_cast<int>(probe.At(kExtent / 2, kExtent / 2).r) > 245;
                }
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glDisable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ZERO);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_renderbuffer != 0) glDeleteRenderbuffers(1, &m_renderbuffer);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
            }

            void Draw(float src0, float src1) {
                glUseProgram(m_program);
                glUniform4f(glGetUniformLocation(m_program, "uSrc0"), src0, src0, src0, 1.0f);
                glUniform4f(glGetUniformLocation(m_program, "uSrc1"), src1, src1, src1, 1.0f);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                glUseProgram(0);
            }

            // Both cases share this gate: nothing below can be measured on a backend that cannot
            // run a dual-source fragment program at all. Returns the skip reason, empty when the
            // program runs - NOT a void helper that calls GTEST_SKIP itself, because GTEST_SKIP
            // expands to a `return` and would leave only the HELPER, letting the case run its
            // assertions anyway and report Failed instead of Skipped.
            std::string WhyTheProgramCannotRun() const {
                if (m_program == 0) {
                    return "this driver cannot build a dual-source fragment shader: " + m_programError;
                }
                if (!m_programRenders) {
                    return "this backend links a dual-source fragment program but renders nothing with it "
                           "(GLSL ES has no `index` layout qualifier without GL_EXT_blend_func_extended)";
                }
                return {};
            }

            GLuint m_renderbuffer = 0;
            GLuint m_fbo = 0;
            GLuint m_vao = 0;
            unsigned int m_program = 0;
            bool m_programRenders = false;
            std::string m_programError;
        };

    } // namespace

    // The whole point of the scenario: this sequence used to be a process kill on any device
    // without the capability, and it has to be a picture either way.
    //
    // dst is black, src0 is white and src1 is mid-grey, with SRC1_COLOR / ONE_MINUS_SRC1_COLOR.
    //   blended  = 1.0 * 0.5 + 0.0 * 0.5 = 0.5  -> ~128
    //   declined = 1.0                          ->  255
    // Anything else means the factors were mistranslated rather than either honoured or declined.
    TEST_F(DualSourceBlendScenario, DualSourceBlendDrawProducesOneOfTheTwoDefinedResults) {
        if (!Ready()) GTEST_SKIP();
        if (const std::string reason = WhyTheProgramCannotRun(); !reason.empty()) GTEST_SKIP() << reason;

        glDisable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ZERO);
        Draw(/*src0=*/0.0f, /*src1=*/0.0f);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);
        EXPECT_EQ(FirstGLError(), 0u) << "glBlendFunc must accept the GL_SRC1_* factors - they are core since 3.3";
        Draw(/*src0=*/1.0f, /*src1=*/0.5f);
        glFinish();
        glDisable(GL_BLEND);
        EXPECT_EQ(FirstGLError(), 0u) << "the dual-source draw left a GL error behind";

        const Image image = ReadPixels(kExtent, kExtent);
        ASSERT_FALSE(image.Empty());
        const Rgba8 centre = image.At(kExtent / 2, kExtent / 2);
        const int red = static_cast<int>(centre.r);
        const bool blended = red > 100 && red < 160;
        const bool declined = red > 245;
        EXPECT_TRUE(blended || declined)
            << "got " << centre << ", which is neither the dual-source blend (~128) nor the declined "
            << "straight-through source (255) - the SRC1 factors were mistranslated";
        Gl().EndFrame();
    }

    // The same factors with blending DISABLED. Nothing may blend, and on the Vulkan side nothing
    // may reach VkPipelineColorBlendAttachmentState carrying a VK_BLEND_FACTOR_SRC1_* on a device
    // without dualSrcBlend - the VUIDs bind to the struct, not to blendEnable. The picture is the
    // source either way, so this case is really "no crash, no error, no surprise".
    TEST_F(DualSourceBlendScenario, DualSourceFactorsWithBlendingDisabledJustWriteTheSource) {
        if (!Ready()) GTEST_SKIP();
        if (const std::string reason = WhyTheProgramCannotRun(); !reason.empty()) GTEST_SKIP() << reason;

        glDisable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ZERO);
        Draw(/*src0=*/0.0f, /*src1=*/0.0f);

        glBlendFunc(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
        Draw(/*src0=*/1.0f, /*src1=*/0.25f);
        glFinish();
        EXPECT_EQ(FirstGLError(), 0u) << "a draw with SRC1 factors and blending off left a GL error behind";

        const Image image = ReadPixels(kExtent, kExtent);
        ASSERT_FALSE(image.Empty());
        const Rgba8 centre = image.At(kExtent / 2, kExtent / 2);
        EXPECT_GT(static_cast<int>(centre.r), 245)
            << "got " << centre << ": blending is disabled, so the source has to be written straight through";
        Gl().EndFrame();
    }

} // namespace MGITest
