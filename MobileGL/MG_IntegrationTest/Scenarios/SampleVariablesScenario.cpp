// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SampleVariablesScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - gl_NumSamples REACHES THE SHADER, AND IT FOLLOWS THE DRAW FRAMEBUFFER.
//
// glslang declares gl_NumSamples only when it is NOT targeting SPIR-V - both the desktop and the
// ES branch of Initialize.cpp wrap `uniform int gl_NumSamples;` in `if (spvVersion.spv == 0)`,
// because SPIR-V has no NumSamples builtin to lower it to - and MobileGL always targets SPIR-V.
// Every fragment shader that read the built-in therefore died at COMPILE time with
// "'gl_NumSamples' : undeclared identifier", which is all 144 KHR-GL46.sample_variables.mask.*
// bodies plus their es_31_compatibility twins.
//
// The source pipeline now lowers it onto a reserved default-block uniform and the draw path writes
// the current draw framebuffer's sample count into it. Two claims, and the second is the one a
// compile-only test cannot make: the value must be the DRAW FRAMEBUFFER's, so one program drawn
// into a multisample target and then into a single-sample target has to report both counts. A
// link-time bake would pass the first assertion and fail the second, which is exactly why the
// write lives per draw.
//
// llvmpipe and lavapipe both offer 4x multisample RGBA8, so this runs for real in CI rather than
// skipping; the skips below are for a driver that offers no multisample renderbuffer at all.

#include <algorithm>
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

        constexpr const char* kVS = R"(#version 400 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        // gl_NumSamples scaled so each count lands on its own well-separated 8-bit value: 1 -> 16,
        // 2 -> 32, 4 -> 64. Every sample of the fragment gets the same colour, so the resolve blit
        // averages identical values and the readback is exact rather than approximate.
        constexpr const char* kFS = R"(#version 400 core
out vec4 o_color;
void main() { o_color = vec4(float(gl_NumSamples) * (16.0 / 255.0), 0.0, 0.0, 1.0); }
)";

        class SampleVariablesScenario : public ScenarioTest {};

        void DrawFullViewportQuad(unsigned int program) {
            static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

    } // namespace

    TEST_F(SampleVariablesScenario, GlNumSamplesFollowsTheDrawFramebuffersSampleCount) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        // The compile failure this scenario exists for lands here, with glslang's own text.
        ASSERT_NE(program, 0u) << error;

        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        const GLint requestedSamples = std::min<GLint>(maxSamples, 4);
        if (requestedSamples < 2) {
            glDeleteProgram(program);
            GTEST_SKIP() << "GL_MAX_SAMPLES is " << maxSamples << "; this needs a multisample renderbuffer";
        }

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);

        // ---- multisample target ----
        GLuint msFbo = 0, msRbo = 0;
        glGenFramebuffers(1, &msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
        glGenRenderbuffers(1, &msRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, requestedSamples, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteRenderbuffers(1, &msRbo);
            glDeleteFramebuffers(1, &msFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteProgram(program);
            GTEST_SKIP() << "no complete " << requestedSamples << "x multisample RGBA8 renderbuffer on this driver";
        }

        // What the driver actually allocated - a request is a lower bound, and the shader has to
        // agree with the query rather than with what was asked for.
        GLint realizedSamples = 0;
        glGetIntegerv(GL_SAMPLES, &realizedSamples);
        ASSERT_GE(realizedSamples, 2) << "the multisample framebuffer reports GL_SAMPLES " << realizedSamples;

        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        DrawFullViewportQuad(program);
        EXPECT_EQ(FirstGLError(), 0u);

        // Resolve into the default framebuffer to read it back.
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        EXPECT_EQ(FirstGLError(), 0u);

        {
            const Image resolved = ReadPixels(width, height);
            const Rgba8 centre = resolved.At(width / 2, height / 2);
            EXPECT_NEAR(centre.r, 16 * realizedSamples, 2)
                << "gl_NumSamples read " << (centre.r / 16.0) << " into a " << realizedSamples
                << "-sample framebuffer; 1 means the reserved uniform was never written, 0 means it was "
                << "written but never uploaded";
        }
        gl.EndFrame();

        // ---- the SAME program into a single-sample target ----
        // A link-time bake of the sample count would keep reporting the multisample value here.
        GLuint ssFbo = 0, ssRbo = 0;
        glGenFramebuffers(1, &ssFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, ssFbo);
        glGenRenderbuffers(1, &ssRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, ssRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ssRbo);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        DrawFullViewportQuad(program);
        EXPECT_EQ(FirstGLError(), 0u);

        {
            const Image single = ReadPixels(width, height);
            const Rgba8 centre = single.At(width / 2, height / 2);
            // GL 4.6 core 15.2.2: gl_NumSamples is ONE for a non-multisample framebuffer, where
            // glGetIntegerv(GL_SAMPLES) answers zero.
            EXPECT_NEAR(centre.r, 16, 2)
                << "gl_NumSamples read " << (centre.r / 16.0)
                << " into a single-sample framebuffer; the value is a property of the DRAW FRAMEBUFFER, "
                << "so re-using the program must re-write it";
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &ssRbo);
        glDeleteFramebuffers(1, &ssFbo);
        glDeleteRenderbuffers(1, &msRbo);
        glDeleteFramebuffers(1, &msFbo);
        glDeleteProgram(program);
        gl.EndFrame();
    }

    // ARB_sample_shading is advertised, and until now glMinSampleShading was a logging no-op while
    // glEnable(GL_SAMPLE_SHADING) fell out of RenderState::SetCapability's default arm - so an
    // application could ask for a shading rate and get silence from both halves.
    //
    // What this can and cannot assert. The RATE itself is not observable from a portable shader:
    // GL 4.6 core 14.3.1 makes any use of gl_SampleID or gl_SamplePosition force per-sample
    // evaluation on its own, so the very built-ins that would report the rate defeat the
    // measurement. What IS worth pinning is that the state now reaches both backends without
    // damage: DirectGLES forwards glEnable(GL_SAMPLE_SHADING) + glMinSampleShading to the ES
    // driver (and must not, on a driver that has neither, push an INVALID_ENUM into the
    // application's error queue), and DirectVulkan bakes sampleShadingEnable/minSampleShading into
    // a NEW pipeline - which it may only do with the device's sampleRateShading feature enabled.
    TEST_F(SampleVariablesScenario, SampleShadingStateReachesTheBackendWithoutDisturbingTheDraw) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        const GLint requestedSamples = std::min<GLint>(maxSamples, 4);
        if (requestedSamples < 2) {
            glDeleteProgram(program);
            GTEST_SKIP() << "GL_MAX_SAMPLES is " << maxSamples << "; sample shading needs a multisample target";
        }

        GLuint msFbo = 0, msRbo = 0;
        glGenFramebuffers(1, &msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
        glGenRenderbuffers(1, &msRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, requestedSamples, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteRenderbuffers(1, &msRbo);
            glDeleteFramebuffers(1, &msFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteProgram(program);
            GTEST_SKIP() << "no complete " << requestedSamples << "x multisample RGBA8 renderbuffer on this driver";
        }

        GLint realizedSamples = 0;
        glGetIntegerv(GL_SAMPLES, &realizedSamples);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glViewport(0, 0, width, height);

        glEnable(GL_SAMPLE_SHADING);
        glMinSampleShading(1.0f);
        EXPECT_EQ(glIsEnabled(GL_SAMPLE_SHADING), static_cast<GLboolean>(GL_TRUE));
        GLfloat rate = -1.0f;
        glGetFloatv(GL_MIN_SAMPLE_SHADING_VALUE, &rate);
        EXPECT_FLOAT_EQ(rate, 1.0f);
        EXPECT_EQ(FirstGLError(), 0u) << "enabling sample shading raised a GL error";

        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        DrawFullViewportQuad(program);
        EXPECT_EQ(FirstGLError(), 0u) << "the sample-shading draw raised a GL error";

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const Image resolved = ReadPixels(width, height);
        const Rgba8 centre = resolved.At(width / 2, height / 2);
        // The rate changes how OFTEN the shader runs, never what it computes - so the same
        // gl_NumSamples reading has to come back.
        EXPECT_NEAR(centre.r, 16 * realizedSamples, 2)
            << "the draw changed its result once sample shading was enabled";

        glMinSampleShading(0.0f);
        glDisable(GL_SAMPLE_SHADING);
        EXPECT_EQ(FirstGLError(), 0u);

        glDeleteRenderbuffers(1, &msRbo);
        glDeleteFramebuffers(1, &msFbo);
        glDeleteProgram(program);
        gl.EndFrame();
    }

} // namespace MGITest
