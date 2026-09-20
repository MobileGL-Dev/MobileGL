// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DefaultFramebufferAcrossSwapScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE DEFAULT FRAMEBUFFER KEEPS NAMING THE IMAGE GL LAST RENDERED INTO,
// ACROSS A SWAP.
//
// A retrace takes its snapshot at the swap call, BEFORE the present executes: that is the
// frame the trace's golden image shows. A disaggregated backend applies the present record
// asynchronously (kWaitPresent: the client does not wait for the apply), and DirectVulkan's
// Present() ends by acquiring the image for the frame that is about to start - so the
// readback that belongs to the frame being presented can be applied AFTER that acquire. It
// used to resolve the freshly acquired image, which nothing had written: the sundial-lite
// retrace came back pure black (409920 zero pixels) while every offscreen path in it was
// fine, and the same library passed 30/30 locally because the two orders only differ under
// load. DirectGLES never had the bug - the driver keeps handing back the buffer the app drew
// into - which is the control this scenario needs: a DirectGLES failure here is the
// scenario's, not the backend's.
//
// Two claims, and neither can be satisfied by ignoring the other:
//
//   * a readback issued after a swap, with no draw in between, still sees the colour the
//     app drew into the default framebuffer (that is the retrace snapshot's frame);
//   * the NEXT draw into the default framebuffer does land in the image the readback then
//     resolves - i.e. the write re-points the default framebuffer at what Present()
//     acquired, rather than every later frame inheriting the presented image.
//
// Together they pin the invariant the backend implements (VulkanRenderer.h,
// m_defaultFramebufferImageIndex): Present() runs ahead, a write to the default framebuffer
// is what makes the acquired image the one GL addresses, and reads resolve whatever that is.

#include <cmath>
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

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFS = R"(#version 330 core
in vec4 vColor;
out vec4 o_color;
void main() { o_color = vColor; }
)";

        class DefaultFramebufferAcrossSwapScenario : public ScenarioTest {};

        // One interleaved VBO: aPos (vec2) then aColor (vec4). CompileProgram pins those two
        // attribute names to locations 0 and 1. The colours below are fully saturated or fully
        // off, so the readback lands on a named colour whatever the driver's exact rounding is.
        void DrawFullViewportQuad(unsigned int program, float red, float green, float blue) {
            const float vertices[] = {
                -1.0f, -1.0f, red, green, blue, 1.0f,
                 1.0f, -1.0f, red, green, blue, 1.0f,
                -1.0f,  1.0f, red, green, blue, 1.0f,
                 1.0f,  1.0f, red, green, blue, 1.0f,
            };
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                                  reinterpret_cast<void*>(2 * sizeof(float)));
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        void PaintDefaultFramebuffer(unsigned int program, float red, float green, float blue) {
            BindDefaultFramebuffer();
            glViewport(0, 0, HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            DrawFullViewportQuad(program, red, green, blue);
        }

    } // namespace

    TEST_F(DefaultFramebufferAcrossSwapScenario, AReadbackAfterASwapStillSeesTheFrameThatWasDrawn) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        PaintDefaultFramebuffer(program, 1.0f, 0.0f, 0.0f);
        const Image before = ReadPixels(width, height);
        ASSERT_TRUE(RegionIsMostly(before, 0, width - 1, 0, height - 1, "red", 0.0,
                                   "the readback of the frame that was just drawn"))
            << "the setup frame did not paint; the cross-swap claim would be vacuous";
        EXPECT_EQ(FirstGLError(), 0u);

        // The swap is the frame boundary the retrace snapshot sits on. Nothing is drawn into
        // the default framebuffer afterwards, so this is exactly the readback that used to
        // resolve the image Present() had just acquired - the black sundial-lite frame.
        gl.EndFrame();

        const Image after = ReadPixels(width, height);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(RegionIsMostly(after, 0, width - 1, 0, height - 1, "red", 0.0,
                                   "the readback of the frame that was presented"))
            << "the default framebuffer stopped naming the image the app rendered into once a swap "
               "had run; Present() acquires ahead of the app (and a kWaitPresent record is applied "
               "without the app waiting), so this readback must not resolve the freshly acquired "
               "image " << gl.FrameIndex() << " frames in";

        gl.EndFrame();
        glDeleteProgram(program);
    }

    TEST_F(DefaultFramebufferAcrossSwapScenario, ADrawAfterASwapLandsInTheImageTheReadbackThenSees) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 8);
        ASSERT_GE(height, 8);

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        PaintDefaultFramebuffer(program, 1.0f, 0.0f, 0.0f);
        gl.EndFrame();

        // The other half of the same rule, and the half that a fix which simply stopped
        // following the swapchain index would break: the first draw after a swap has to land in
        // the image Present() acquired, and the readback has to follow it there.
        PaintDefaultFramebuffer(program, 0.0f, 0.0f, 1.0f);
        const Image repainted = ReadPixels(width, height);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(RegionIsMostly(repainted, 0, width - 1, 0, height - 1, "blue", 0.0,
                                   "the readback of the frame drawn after the swap"))
            << "a draw issued after a swap was not visible to glReadPixels: the default framebuffer "
               "did not re-point at the image Present() acquired";

        gl.EndFrame();
        glDeleteProgram(program);
    }

} // namespace MGITest
