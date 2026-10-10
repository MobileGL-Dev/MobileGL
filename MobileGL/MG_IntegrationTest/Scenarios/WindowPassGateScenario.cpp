// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/WindowPassGateScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A FRAME'S OFF-SCREEN WORK GOES TO THE GPU AHEAD OF ITS WINDOW PASS, AND LOSES NOTHING.
//
// About every other frame on Android renders into the window buffer the display has only just
// released, whose release fence is still pending. A frame submitted as one batch put ALL of its
// GPU work behind that fence, so the frame's off-screen passes started only after the display's
// next commit (the P15 present-wait timeline). Now only the window pass waits:
//
//   * DirectVulkan submits what a frame recorded before its first use of the acquired swapchain
//     image WITHOUT the acquire semaphore, and the submission that uses the image (or the
//     present's own) carries the wait (VulkanRenderer::SubmitAheadOfAcquiredImage);
//   * DirectGLES flushes once per frame just before the first default-framebuffer use of a
//     window surface (FlushAheadOfWindowBufferUse); the headless pbuffer here has no window
//     buffer, so on DirectGLES these cases are the pixel control.
//
// The cases put the frame shapes that split around the window pass: off-screen work then a blit
// into the window, off-screen work then a draw that samples it into the window (Minecraft's
// shape), frames that never touch the window at all, and a readback of the window inside the
// frame. Every frame changes what it draws, so a window pass that ran ahead of the off-screen
// work it reads, or a frame that lost its off-screen half, shows up as the previous frame's
// colours. The DirectVulkan.WindowPassGate. entry also asserts the renderer's latched MGLOG_I:
// on a renderer that submits the frame in one piece it is the assertion that goes red.

#include <string>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
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

        constexpr const char* kColorVS = R"(#version 330 core
in vec2 aPos;
in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kColorFS = R"(#version 330 core
in vec4 vColor;
out vec4 o_color;
void main() { o_color = vColor; }
)";

        // The window pass of Minecraft's frame: a full-screen draw that samples the off-screen target.
        constexpr const char* kSampleVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kSampleFS = R"(#version 330 core
uniform sampler2D uTex;
in vec2 vUv;
out vec4 o_color;
void main() { o_color = texture(uTex, vUv); }
)";

        struct NamedColor {
            float r, g, b;
            const char* name;
        };

        // Saturated colours only: ColorName() names them exactly whatever the rounding.
        constexpr NamedColor kCycle[] = {
            {1.0f, 0.0f, 0.0f, "red"},  {0.0f, 1.0f, 0.0f, "green"}, {0.0f, 0.0f, 1.0f, "blue"},
            {1.0f, 1.0f, 0.0f, "yellow"}, {1.0f, 1.0f, 1.0f, "white"},
        };
        constexpr int kCycleLength = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));
        constexpr int kFrames = 7;

        const NamedColor& Background(int frame) { return kCycle[frame % kCycleLength]; }
        const NamedColor& Foreground(int frame) { return kCycle[(frame + 2) % kCycleLength]; }

        // The NDC rectangle [x0, x1] x [y0, y1] in one colour; aPos at 0, aColor at 1.
        void DrawRect(unsigned int program, float x0, float y0, float x1, float y1, const NamedColor& c) {
            const float vertices[] = {
                x0, y0, c.r, c.g, c.b, 1.0f, x1, y0, c.r, c.g, c.b, 1.0f,
                x0, y1, c.r, c.g, c.b, 1.0f, x1, y1, c.r, c.g, c.b, 1.0f,
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
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        void DrawSampledFullScreen(unsigned int program, unsigned int texture) {
            const float vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glUniform1i(glGetUniformLocation(program, "uTex"), 0);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        enum class WindowPass { Blit, SampledDraw };

        class WindowPassGateScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                // Before the case draws anything, so the arming line can only be this case's; ctest
                // gives every case a process of its own and the renderer latches the line once.
                m_logBefore = PipeStatsWindow::MarkLaneLog();
                m_width = Gl().Width();
                m_height = Gl().Height();
                std::string error;
                m_colorProgram = CompileProgram(kColorVS, kColorFS, &error);
                ASSERT_NE(m_colorProgram, 0u) << error;
                m_sampleProgram = CompileProgram(kSampleVS, kSampleFS, &error);
                ASSERT_NE(m_sampleProgram, 0u) << error;
                m_target = MakeColorFbo(m_width, m_height);
                ASSERT_NE(m_target.fbo, 0u);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
            }

            void TearDown() override {
                if (Ready()) {
                    glUseProgram(0);
                    BindDefaultFramebuffer();
                    if (m_colorProgram != 0) glDeleteProgram(m_colorProgram);
                    if (m_sampleProgram != 0) glDeleteProgram(m_sampleProgram);
                    if (m_target.fbo != 0) DestroyColorFbo(m_target);
                }
                ScenarioTest::TearDown();
            }

            // The off-screen half of frame `frame`: its background, and its foreground on the left half.
            void RenderOffscreen(int frame) {
                BindFbo(m_target);
                const NamedColor& background = Background(frame);
                ClearTo(background.r, background.g, background.b, 1.0f);
                DrawRect(m_colorProgram, -1.0f, -1.0f, 0.0f, 1.0f, Foreground(frame));
            }

            void RenderWindowPass(WindowPass pass) {
                if (pass == WindowPass::Blit) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_target.fbo);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
                    glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT,
                                      GL_NEAREST);
                    BindDefaultFramebuffer();
                } else {
                    BindDefaultFramebuffer();
                    DrawSampledFullScreen(m_sampleProgram, m_target.texture);
                }
            }

            // Frame `frame`'s picture in the bound READ framebuffer: its foreground on the left half,
            // its background on the right. Two pixels in from the seam, which is where the halves meet.
            ::testing::AssertionResult HoldsFrame(const Image& image, int frame, const std::string& what) {
                const int seam = m_width / 2;
                auto left = RegionIsMostly(image, 0, seam - 3, 0, m_height - 1, Foreground(frame).name, 0.0,
                                           what + ", left half (frame " + std::to_string(frame) + "'s foreground)");
                if (!left) return left;
                return RegionIsMostly(image, seam + 2, m_width - 1, 0, m_height - 1, Background(frame).name, 0.0,
                                      what + ", right half (frame " + std::to_string(frame) + "'s background)");
            }

            // kFrames presented frames of the shape, then one more read back before its swap (a
            // readback after the swap would touch an image already handed to the presentation engine).
            void RunFramesThroughTheWindow(WindowPass pass) {
                for (int frame = 0; frame < kFrames; ++frame) {
                    RenderOffscreen(frame);
                    RenderWindowPass(pass);
                    ASSERT_EQ(FirstGLError(), 0u) << "frame " << frame;
                    Gl().EndFrame();
                }
                RenderOffscreen(kFrames);
                RenderWindowPass(pass);
                EXPECT_TRUE(HoldsFrame(ReadPixels(m_width, m_height), kFrames, "the window after the presented frames"))
                    << "the window pass did not show the off-screen work recorded ahead of it in the same frame";
                EXPECT_EQ(FirstGLError(), 0u);
                Gl().EndFrame();
            }

            // In the DirectVulkan.WindowPassGate. entry (DirectVulkan, a log of its own), a frame's
            // off-screen work must have gone to the queue ahead of its window pass during this case.
            void ExpectTheWindowPassWasGatedAlone() {
                const bool armed = std::getenv("MGITEST_WINDOW_PASS_GATE_LANE") != nullptr &&
                                   Gl().BackendName() == std::string("DirectVulkan") &&
                                   !PipeStatsWindow::LibraryLogPath().empty();
                RecordProperty("arming_checked", armed ? "yes" : "no");
                if (!armed) return;
                const std::string appended = PipeStatsWindow::ReadLaneLogSince(m_logBefore);
                EXPECT_NE(appended.find("off-screen work goes to the queue ahead of its window pass"), std::string::npos)
                    << "this case recorded off-screen work ahead of every window pass, but DirectVulkan never "
                       "reported submitting it ahead of the acquire wait: the whole frame still queues behind "
                       "the swapchain image's release (VulkanRenderer::SubmitAheadOfAcquiredImage). Log appended "
                       "by this case:\n"
                    << appended;
            }

            PipeStatsWindow::LogMark m_logBefore{};
            int m_width = 0;
            int m_height = 0;
            unsigned int m_colorProgram = 0;
            unsigned int m_sampleProgram = 0;
            ColorFbo m_target{};
        };

    } // namespace

    TEST_F(WindowPassGateScenario, OffscreenWorkThenABlitIntoTheWindowEveryFrame) {
        if (!Ready()) return;
        RunFramesThroughTheWindow(WindowPass::Blit);
        ExpectTheWindowPassWasGatedAlone();
    }

    TEST_F(WindowPassGateScenario, OffscreenWorkThenASampledDrawIntoTheWindowEveryFrame) {
        if (!Ready()) return;
        RunFramesThroughTheWindow(WindowPass::SampledDraw);
        ExpectTheWindowPassWasGatedAlone();
    }

    // The present submits the whole frame's work on frames that never use the window, and its own
    // submission then carries nothing but the wait. The window keeps working afterwards.
    TEST_F(WindowPassGateScenario, FramesThatNeverTouchTheWindowStillPresent) {
        if (!Ready()) return;
        for (int frame = 0; frame < kFrames; ++frame) {
            RenderOffscreen(frame);
            ASSERT_EQ(FirstGLError(), 0u) << "frame " << frame;
            Gl().EndFrame();
        }
        BindFbo(m_target);
        EXPECT_TRUE(HoldsFrame(ReadPixels(m_width, m_height), kFrames - 1, "the off-screen target after the swaps"))
            << "off-screen work of a frame that never used the window did not execute";

        RenderOffscreen(kFrames);
        RenderWindowPass(WindowPass::SampledDraw);
        EXPECT_TRUE(HoldsFrame(ReadPixels(m_width, m_height), kFrames, "the window in the frame after them"));
        EXPECT_EQ(FirstGLError(), 0u);
        Gl().EndFrame();
        ExpectTheWindowPassWasGatedAlone();
    }

    // A readback of the window inside the frame: its submission uses the acquired image, so it waits
    // for the acquire; the window pass's later work and the present must not wait for it again.
    TEST_F(WindowPassGateScenario, AReadbackOfTheWindowInsideTheFrame) {
        if (!Ready()) return;
        for (int frame = 0; frame < kFrames; ++frame) {
            RenderOffscreen(frame);
            RenderWindowPass(frame % 2 == 0 ? WindowPass::Blit : WindowPass::SampledDraw);
            EXPECT_TRUE(HoldsFrame(ReadPixels(m_width, m_height), frame, "the window read back inside its frame"));
            // More window work after the readback, in a later submission of the same frame.
            DrawRect(m_colorProgram, 0.0f, -1.0f, 1.0f, 1.0f, Foreground(frame));
            EXPECT_TRUE(RegionIsMostly(ReadPixels(m_width, m_height), 0, m_width - 1, 0, m_height - 1,
                                       Foreground(frame).name, 0.0, "the window painted over after its readback"));
            ASSERT_EQ(FirstGLError(), 0u) << "frame " << frame;
            Gl().EndFrame();
        }
        ExpectTheWindowPassWasGatedAlone();
    }

} // namespace MGITest
