// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SwizzleAccessRoutineScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - EVERY TEXTURE ACCESS ROUTINE READS THE SAME TEXEL OUT OF A usampler2DArray.
//
// KHR-GL33/GL40.texture_swizzle.smoke_access_idx_* sweeps the fourteen GLSL texture access
// routines against a 1x1x1 GL_RGBA32UI GL_TEXTURE_2D_ARRAY and asserts the fetched channel. On
// Espryt, `texture` and `textureGrad` pass while `textureLod`, `textureOffset`, `texelFetch`,
// `texelFetchOffset` and `textureLodOffset` fail - 21 cases per version, 42 across GL33 and GL40.
// The discriminator is the important part: the swizzle state is IDENTICAL across all of them, so
// swizzle delivery is not the defect; what differs is only how the routine is spelled, i.e. what
// SPIRV-Cross has to emit into ESSL for it.
//
// This scenario is that discriminator, reduced to something that fails in milliseconds: one draw
// per access routine against the same texture and the same swizzle, all reading the same texel.
// A routine that disagrees with the others is the defect, and the failure message names it.
//
// The shader shape is copied from the conformance test rather than idealised - including its
// `int(0)` level-of-detail argument, which is a desktop-GLSL implicit int->float conversion that
// ESSL does not have, and its zero offsets. Both are exactly the things a GLSL -> SPIR-V -> ESSL
// round trip can lose.
//
// DirectVulkan is the built-in control: it consumes the SPIR-V directly and never runs the ESSL
// emission, so a failure there would mean the scenario, not the backend.

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

        // The conformance test's own source texel, one recognisable value per channel.
        constexpr std::uint32_t kSourceTexel[4] = {0x3FFFFFFFu, 0x7FFFFFFFu, 0xBFFFFFFFu, 0xFFFFFFFFu};

        constexpr int kOutputWidth = 8;
        constexpr int kOutputHeight = 8;

        // The blank vertex shader the smoke test uses: a full-viewport strip with no attributes.
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

        struct AccessRoutine {
            const char* name;     // as it appears in the conformance case name
            const char* callText; // the whole TEXTURE_ACCESS(sampler, ARGUMENTS) expression
        };

        // Spelled exactly as gl3cTextureSwizzleTests.cpp's prepareArguments builds them for
        // GL_TEXTURE_2D_ARRAY: three coordinates, `int(0)` for the level, ivec2 offsets.
        constexpr AccessRoutine kRoutines[] = {
            {"texture", "texture(smp, vec3(0, 0, 0))"},
            {"textureLod", "textureLod(smp, vec3(0, 0, 0), int(0))"},
            {"textureOffset", "textureOffset(smp, vec3(0, 0, 0), ivec2(0, 0))"},
            {"texelFetch", "texelFetch(smp, ivec3(0, 0, 0), int(0))"},
            {"texelFetchOffset", "texelFetchOffset(smp, ivec3(0, 0, 0), int(0), ivec2(0, 0))"},
            {"textureLodOffset", "textureLodOffset(smp, vec3(0, 0, 0), int(0), ivec2(0, 0))"},
            {"textureGrad", "textureGrad(smp, vec3(0, 0, 0), vec2(0, 0), vec2(0, 0))"},
            {"textureGradOffset", "textureGradOffset(smp, vec3(0, 0, 0), vec2(0, 0), vec2(0, 0), ivec2(0, 0))"},
        };

        constexpr const char* kChannels[4] = {"x", "y", "z", "w"};

        std::string FragmentSource(const AccessRoutine& routine, int channel) {
            return std::string("#version 330 core\n\nuniform usampler2DArray smp;\n\nout uint out_color;\n\n"
                               "void main()\n{\n    uint result = ") +
                   routine.callText + "." + kChannels[channel] + ";\n\n    out_color = result;\n}\n";
        }

        class SwizzleAccessRoutineScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                // 1x1x1 RGBA32UI 2D array. Integer textures are not filterable, so NEAREST is
                // mandatory, and a single level means every LOD argument must resolve to 0.
                glGenTextures(1, &m_sourceTexture);
                glBindTexture(GL_TEXTURE_2D_ARRAY, m_sourceTexture);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA32UI, 1, 1, 1);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                                kSourceTexel);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                ASSERT_EQ(FirstGLError(), 0u) << "source texture setup left a GL error behind";

                // 8x8 R32UI render target, read back with glReadPixels.
                glGenTextures(1, &m_outputTexture);
                glBindTexture(GL_TEXTURE_2D, m_outputTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, kOutputWidth, kOutputHeight);
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
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_outputTexture != 0) glDeleteTextures(1, &m_outputTexture);
                if (m_sourceTexture != 0) glDeleteTextures(1, &m_sourceTexture);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            void SetSwizzle(GLenum r, GLenum g, GLenum b, GLenum a) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, m_sourceTexture);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_R, static_cast<GLint>(r));
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_G, static_cast<GLint>(g));
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_B, static_cast<GLint>(b));
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_A, static_cast<GLint>(a));
            }

            // Renders one access routine into the 8x8 target and returns every texel it wrote.
            // Returns an empty vector (with a gtest failure already recorded) if the program did
            // not build.
            std::vector<std::uint32_t> Render(const AccessRoutine& routine, int channel) {
                const std::string fragment = FragmentSource(routine, channel);
                std::string error;
                const unsigned int program = CompileProgram(kVertexSource, fragment.c_str(), &error);
                if (program == 0) {
                    ADD_FAILURE() << routine.name << " channel " << kChannels[channel]
                                  << ": program did not build: " << error << "\n--- source ---\n"
                                  << fragment;
                    return {};
                }

                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glViewport(0, 0, kOutputWidth, kOutputHeight);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                const GLuint clearValue[4] = {0xDEADBEEFu, 0u, 0u, 0u};
                glClearBufferuiv(GL_COLOR, 0, clearValue);

                glUseProgram(program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, m_sourceTexture);
                const GLint location = glGetUniformLocation(program, "smp");
                glUniform1i(location, 0);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);

                std::vector<std::uint32_t> texels(static_cast<std::size_t>(kOutputWidth) * kOutputHeight, 0);
                glReadPixels(0, 0, kOutputWidth, kOutputHeight, GL_RED_INTEGER, GL_UNSIGNED_INT, texels.data());
                glUseProgram(0);
                glDeleteProgram(program);
                return texels;
            }

            // Asserts every texel equals `expected`, naming the routine and the first offender.
            void ExpectAllTexels(const AccessRoutine& routine, int channel, std::uint32_t expected,
                                 const std::vector<std::uint32_t>& texels) {
                if (texels.empty()) return;
                std::size_t offenders = 0;
                std::uint32_t firstBad = 0;
                std::size_t firstIndex = 0;
                for (std::size_t i = 0; i < texels.size(); ++i) {
                    if (texels[i] == expected) continue;
                    if (offenders == 0) {
                        firstBad = texels[i];
                        firstIndex = i;
                    }
                    ++offenders;
                }
                EXPECT_EQ(offenders, 0u)
                    << routine.name << "(...)." << kChannels[channel] << " returned 0x" << std::hex << firstBad
                    << " instead of 0x" << expected << std::dec << " at texel " << firstIndex << " (" << offenders
                    << " of " << texels.size() << " wrong)";
            }

            GLuint m_sourceTexture = 0;
            GLuint m_outputTexture = 0;
            GLuint m_fbo = 0;
            GLuint m_vao = 0;
        };

    } // namespace

    // Identity swizzle: every routine must fetch the channel it was asked for. This is the
    // scenario's floor - it does not involve swizzling at all, so a failure here is purely about
    // how the access routine itself survives the trip to the backend.
    TEST_F(SwizzleAccessRoutineScenario, EveryAccessRoutineFetchesTheSameTexelUnderTheIdentitySwizzle) {
        if (!Ready() || IsSkipped()) return;
        SetSwizzle(GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA);
        ASSERT_EQ(FirstGLError(), 0u);

        for (const AccessRoutine& routine : kRoutines) {
            for (int channel = 0; channel < 4; ++channel) {
                const std::vector<std::uint32_t> texels = Render(routine, channel);
                EXPECT_EQ(FirstGLError(), 0u) << routine.name << " left a GL error behind";
                ExpectAllTexels(routine, channel, kSourceTexel[channel], texels);
            }
        }
        Gl().EndFrame();
    }

    // A real swizzle, applied to every routine. Reversing the channels means a routine that
    // silently drops the swizzle returns the UNSWIZZLED texel rather than nothing, so the
    // failure distinguishes "swizzle lost" from "fetch broken".
    TEST_F(SwizzleAccessRoutineScenario, EveryAccessRoutineSeesAReversedSwizzle) {
        if (!Ready() || IsSkipped()) return;
        SetSwizzle(GL_ALPHA, GL_BLUE, GL_GREEN, GL_RED);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::uint32_t expected[4] = {kSourceTexel[3], kSourceTexel[2], kSourceTexel[1], kSourceTexel[0]};
        for (const AccessRoutine& routine : kRoutines) {
            for (int channel = 0; channel < 4; ++channel) {
                const std::vector<std::uint32_t> texels = Render(routine, channel);
                EXPECT_EQ(FirstGLError(), 0u) << routine.name << " left a GL error behind";
                ExpectAllTexels(routine, channel, expected[channel], texels);
            }
        }
        Gl().EndFrame();
    }

    // GL_ONE and GL_ZERO, which the conformance table spells as the literal values 1 and 0 and
    // which the backend has to synthesise rather than fetch.
    TEST_F(SwizzleAccessRoutineScenario, EveryAccessRoutineSeesConstantSwizzleSources) {
        if (!Ready() || IsSkipped()) return;
        SetSwizzle(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::uint32_t expected[4] = {1u, 0u, 1u, 0u};
        for (const AccessRoutine& routine : kRoutines) {
            for (int channel = 0; channel < 4; ++channel) {
                const std::vector<std::uint32_t> texels = Render(routine, channel);
                EXPECT_EQ(FirstGLError(), 0u) << routine.name << " left a GL error behind";
                ExpectAllTexels(routine, channel, expected[channel], texels);
            }
        }
        Gl().EndFrame();
    }
} // namespace MGITest
