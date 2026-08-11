// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClearThenReadPixelsScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A CLEAR OF THE DEFAULT FRAMEBUFFER IS VISIBLE TO glReadPixels WITH NO DRAW BETWEEN.
//
// DirectVulkan parks a glClear as a pending clear and folds it into the next render pass's
// loadOp. When nothing is drawn after the clear there is no render pass, and the readback path
// used to materialize pending clears only for USER framebuffers - so a readback right after a
// clear of the DEFAULT framebuffer blitted the untouched swapchain image and handed back the
// previous frame's colour.
//
// That is the whole of KHR-GL40.draw_indirect.negative-* (12 Magma failures): each case clears,
// issues a draw that correctly raises INVALID_OPERATION and therefore never executes, then reads
// the frame back expecting (0,0,0,0) and gets the previous case's (0.1,0.2,0.3,1). The staleness
// cannot appear in one frame, so the scenario paints a frame first and clears in the next.
//
// The alpha assertion is the second half of the same census finding: a cleared default
// framebuffer read back (0,0,0,1) where (0,0,0,0) was written, because the clear was routed
// through the default FBO's placeholder attachment, whose format can lack alpha, rather than
// through the swapchain image that actually has one.
//
// DirectGLES is the built-in control: a native GL driver has no deferred-clear model at all, so
// a failure there would mean the scenario, not the backend.

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
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        // The colour KHR-GL40.draw_indirect's fshSimple paints, so a stale readback shows up as
        // the same value the conformance log reports.
        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.1, 0.2, 0.3, 1.0); }
)";

        class ClearThenReadPixelsScenario : public ScenarioTest {};

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

    TEST_F(ClearThenReadPixelsScenario, ClearWithNoDrawIsVisibleToDefaultFramebufferReadPixels) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        // Frame 1: paint the whole default framebuffer, so there IS something stale to return.
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(1.0f, 1.0f, 1.0f, 1.0f);
        DrawFullViewportQuad(program);
        {
            const Image painted = ReadPixels(width, height);
            const Rgba8 centre = painted.At(width / 2, height / 2);
            ASSERT_NEAR(centre.r, 26, 2) << "the setup frame did not paint; the staleness test would be vacuous";
            ASSERT_NEAR(centre.g, 51, 2);
            ASSERT_NEAR(centre.b, 77, 2);
        }
        gl.EndFrame();

        // Frame 2: clear to transparent black and read back with NO draw at all.
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
        const Image cleared = ReadPixels(width, height);
        EXPECT_EQ(FirstGLError(), 0u);

        int nonZero = 0;
        int firstX = -1;
        int firstY = -1;
        Rgba8 firstOffender{};
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const Rgba8 pixel = cleared.At(x, y);
                if (pixel.r == 0 && pixel.g == 0 && pixel.b == 0 && pixel.a == 0) continue;
                if (nonZero == 0) {
                    firstX = x;
                    firstY = y;
                    firstOffender = pixel;
                }
                ++nonZero;
            }
        }
        EXPECT_EQ(nonZero, 0) << "glClear(0,0,0,0) followed by glReadPixels with no draw returned " << nonZero
                              << " of " << (width * height) << " non-zero pixels; first at (" << firstX << ", "
                              << firstY << ") = (" << static_cast<int>(firstOffender.r) << ", "
                              << static_cast<int>(firstOffender.g) << ", " << static_cast<int>(firstOffender.b)
                              << ", " << static_cast<int>(firstOffender.a) << ")";

        gl.EndFrame();
        glDeleteProgram(program);
    }

    // The same claim for a sub-rect read, which is the shape the conformance suite uses most and
    // the one whose orientation handling is separate (see OrientationScenario).
    TEST_F(ClearThenReadPixelsScenario, ClearWithNoDrawIsVisibleToASubRectReadback) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        DrawFullViewportQuad(program);
        gl.EndFrame();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
        const int rectWidth = width / 2;
        const int rectHeight = height / 2;
        const Image cleared = ReadPixelsRect(width / 4, height / 4, rectWidth, rectHeight);
        EXPECT_EQ(FirstGLError(), 0u);

        int nonZero = 0;
        for (int y = 0; y < rectHeight; ++y) {
            for (int x = 0; x < rectWidth; ++x) {
                const Rgba8 pixel = cleared.At(x, y);
                if (pixel.r != 0 || pixel.g != 0 || pixel.b != 0 || pixel.a != 0) ++nonZero;
            }
        }
        EXPECT_EQ(nonZero, 0) << nonZero << " of " << (rectWidth * rectHeight)
                              << " pixels in a sub-rect read after a draw-free clear were not zero";

        gl.EndFrame();
        glDeleteProgram(program);
    }

    // The other half of the same rule, and the one the first version of this fix got wrong: a
    // parked clear must be executed BEFORE whatever writes the framebuffer next, not whenever the
    // readback happens to notice it. Minecraft clears the default framebuffer, renders the world
    // into its own framebuffer and blits the result out; nothing in between opens a render pass on
    // the default framebuffer, so the clear stays parked across the whole frame. Materializing it
    // at readback time therefore ran it AFTER the blit and returned a blank frame - which is what
    // took every DirectVulkan retrace to ssim 0.000005.
    TEST_F(ClearThenReadPixelsScenario, ABlitIntoTheDefaultFramebufferSurvivesAnEarlierClear) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        // Paint a source framebuffer, exactly as a game renders its world off-screen.
        ColorFbo source = MakeColorFbo(width, height);
        ASSERT_NE(source.fbo, 0u);
        BindFbo(source);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        DrawFullViewportQuad(program);

        // Clear the DEFAULT framebuffer, then blit the source over it. The clear is white so a
        // frame that lost the blit is unmistakable, and the blit's colour is fshSimple's.
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        ClearTo(1.0f, 1.0f, 1.0f, 1.0f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        EXPECT_EQ(FirstGLError(), 0u);

        const Image blitted = ReadPixels(width, height);
        EXPECT_EQ(FirstGLError(), 0u);
        const Rgba8 centre = blitted.At(width / 2, height / 2);
        EXPECT_NEAR(centre.r, 26, 2) << "the blit into the default framebuffer did not survive the clear that "
                                        "preceded it; read back rgba(" << static_cast<int>(centre.r) << ", "
                                     << static_cast<int>(centre.g) << ", " << static_cast<int>(centre.b) << ", "
                                     << static_cast<int>(centre.a) << ")";
        EXPECT_NEAR(centre.g, 51, 2);
        EXPECT_NEAR(centre.b, 77, 2);

        DestroyColorFbo(source);
        gl.EndFrame();
        glDeleteProgram(program);
    }

    // The same ordering claim for the path that DOES open a render pass. It passes today (the
    // render pass folds the clear into its loadOp and pops it), and it is here so a future change
    // to the pending-clear lifecycle cannot quietly reverse clear and draw.
    TEST_F(ClearThenReadPixelsScenario, ADrawIntoTheDefaultFramebufferSurvivesAnEarlierClear) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(1.0f, 1.0f, 1.0f, 1.0f);
        DrawFullViewportQuad(program);
        EXPECT_EQ(FirstGLError(), 0u);

        const Image painted = ReadPixels(width, height);
        const Rgba8 centre = painted.At(width / 2, height / 2);
        EXPECT_NEAR(centre.r, 26, 2) << "the draw did not survive the clear that preceded it";
        EXPECT_NEAR(centre.g, 51, 2);
        EXPECT_NEAR(centre.b, 77, 2);

        gl.EndFrame();
        glDeleteProgram(program);
    }
} // namespace MGITest
