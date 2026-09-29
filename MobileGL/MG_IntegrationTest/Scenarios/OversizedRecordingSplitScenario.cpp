// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/OversizedRecordingSplitScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A FRAME WITH MORE DRAWS THAN ONE COMMAND BUFFER MAY HOLD IS SPLIT, AND LOSES NOTHING.
//
// DirectVulkan records a frame into one command buffer, and used to submit it only at Present.
// A driver keeps a command buffer's GPU memory until the buffer is freed, and the Adreno 830
// driver maps that memory into the process in 16 KiB chunks. Minecraft rd12's loading frame
// records 981,654 draws (3.1M commands) into one buffer. It grew to ~40k such mappings and
// aborted inside the driver's own calloc once vm.max_map_count (65530) was spent. The renderer
// now submits the open buffer after MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER draws and goes
// on recording into a fresh one (VulkanRenderer::SplitOversizedRecording).
//
// The first case writes each pixel of a 32x32 target with its own draw and its own uniform
// value, so it pins that no draw is lost and no per-draw state is left behind on either side of
// a split. The rest put GL state that one draw leaves for a later one ACROSS the split: a depth
// occluder, a stencil mark, a blend destination, the samples of a multisample target, an
// occlusion or primitives-generated query that is still open, and a transform-feedback capture
// that is still active. Each draws its "before" part, then kFillers draws, then its "after"
// part. kFillers is more than twice the SplitRecording lane's budget of 64, so at least one
// split lands between the two parts wherever the recording's count stood.
//
// The ambient registrations run the same draws unsplit, because the default budget is far above
// any of these frames; that is the control. The DirectVulkan.SplitRecording. entry pins the
// budget to 64, and there every case also asserts the renderer's latched MGLOG_I - the one
// assertion that is red on a renderer that never splits.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

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

        constexpr int kSide = 32;
        constexpr int kDraws = kSide * kSide;
        // More than twice the SplitRecording lane's budget (64): wherever the count stands when
        // they start, at least one split lands among them.
        constexpr int kFillers = 150;
        // Draws inside the query and capture spans: also more than twice the budget.
        constexpr int kSpanDraws = 200;

        // aPos spans the unit square; the uniform moves it onto cell (i % 32, i / 32) of a
        // 32x32 grid, which is exactly one pixel of a 32x32 target.
        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
uniform int uIndex;
void main() {
    vec2 cell = vec2(float(uIndex % 32), float(uIndex / 32));
    gl_Position = vec4((cell + aPos) / 16.0 - 1.0, 0.0, 1.0);
}
)";

        // Multiples of 8/255 survive an RGBA8 round trip exactly, so the readback bytes are
        // (8x, 8y, 255, 255) for the pixel at (x, y) and nothing else can produce them.
        constexpr const char* kFS = R"(#version 330 core
uniform int uIndex;
out vec4 o_color;
void main() {
    o_color = vec4(float(uIndex % 32) * 8.0 / 255.0, float(uIndex / 32) * 8.0 / 255.0, 1.0, 1.0);
}
)";

        // The state cases: the unit-square vertices (or a triangle of them) stretched over an
        // NDC rectangle at a given depth, in a uniform colour. A zero rectangle makes a
        // zero-area primitive - a draw that reaches the renderer and rasterizes nothing.
        constexpr const char* kRectVS = R"(#version 330 core
in vec2 aPos;
uniform vec4 uRect;
uniform float uDepth;
void main() { gl_Position = vec4(mix(uRect.xy, uRect.zw, aPos), uDepth, 1.0); }
)";
        constexpr const char* kRectFS = R"(#version 330 core
uniform vec4 uColor;
out vec4 o_color;
void main() { o_color = uColor; }
)";

        // The capture case: one point per draw, carrying 2 * uIndex + 1.
        constexpr const char* kCaptureVS = R"(#version 330 core
uniform int uIndex;
out float vs_value;
void main() {
    vs_value = float(uIndex) * 2.0 + 1.0;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";
        constexpr const char* kCaptureFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";

        constexpr float kCapturePoison = -12345.0f;

        // The budget the process was launched with, or 0 when it is unset or not a number.
        unsigned long PinnedDrawBudget() {
            const char* value = std::getenv("MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER");
            if (value == nullptr || *value == '\0') return 0;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            return (end != nullptr && *end == '\0') ? parsed : 0;
        }

        // The library log, for the arming assertion. MOBILEGL_LOG_FILE_PATH is read at log-init,
        // every process in the lane appends to it, and only bytes appended after the snapshot
        // count. P11 M2: ON A SPLIT BUILD IT IS A BASE NAME - the library writes one file per role
        // (PipeStatsWindow.h) - and the latched line comes from the RENDERER, which under a
        // transport is the server role: the apply thread under inproc, the spawned server process
        // under spawn (it inherits this entry's environment, budget and log base included). So the
        // mark and the read cover both roles, as PrimitivesGeneratedNoXfbScenario's arming does; a
        // read of the literal base found no file and reported an unsplit renderer. In the pull
        // build the client role is the whole log.
        std::string LibraryLogPath() { return PipeStatsWindow::LibraryLogPath(); }

        PipeStatsWindow::LogMark LibraryLogSize() { return PipeStatsWindow::MarkLaneLog(); }

        std::string LibraryLogSince(const PipeStatsWindow::LogMark& mark) {
            return PipeStatsWindow::ReadLaneLogSince(mark);
        }

        std::string Describe(const Rgba8& c) {
            std::ostringstream os;
            os << "rgba(" << static_cast<int>(c.r) << ", " << static_cast<int>(c.g) << ", " << static_cast<int>(c.b)
               << ", " << static_cast<int>(c.a) << ")";
            return os.str();
        }

        ::testing::AssertionResult EveryPixelHoldsItsOwnDraw(const Image& image) {
            int wrong = 0;
            int firstX = -1, firstY = -1;
            Rgba8 firstSeen{};
            for (int y = 0; y < kSide; ++y) {
                for (int x = 0; x < kSide; ++x) {
                    const Rgba8 expected{static_cast<std::uint8_t>(8 * x), static_cast<std::uint8_t>(8 * y), 255, 255};
                    const Rgba8 seen = image.At(x, y);
                    if (seen != expected) {
                        if (wrong == 0) {
                            firstX = x;
                            firstY = y;
                            firstSeen = seen;
                        }
                        ++wrong;
                    }
                }
            }
            if (wrong == 0) {
                return ::testing::AssertionSuccess();
            }
            return ::testing::AssertionFailure()
                   << wrong << " of " << kDraws << " pixels do not hold their own draw's colour. First: (" << firstX
                   << ", " << firstY << ") = " << firstSeen << ", drawn by draw " << (firstY * kSide + firstX)
                   << " (a pixel no draw reached still holds the clear colour 0,0,0,0)";
        }

        // Every pixel of columns [x0, x1] (all rows) is `expected`, each channel within `tolerance`.
        ::testing::AssertionResult ColumnsAre(const Image& image, int x0, int x1, const Rgba8& expected,
                                              int tolerance, const char* what) {
            int wrong = 0;
            int firstX = -1, firstY = -1;
            Rgba8 firstSeen{};
            for (int y = 0; y < kSide; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const Rgba8 seen = image.At(x, y);
                    const bool close = std::abs(seen.r - expected.r) <= tolerance &&
                                       std::abs(seen.g - expected.g) <= tolerance &&
                                       std::abs(seen.b - expected.b) <= tolerance &&
                                       std::abs(seen.a - expected.a) <= tolerance;
                    if (!close) {
                        if (wrong == 0) {
                            firstX = x;
                            firstY = y;
                            firstSeen = seen;
                        }
                        ++wrong;
                    }
                }
            }
            if (wrong == 0) return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure()
                   << what << ": " << wrong << " pixels of columns " << x0 << ".." << x1 << " are not "
                   << Describe(expected) << "; first (" << firstX << ", " << firstY << ") = " << Describe(firstSeen);
        }

        class OversizedRecordingSplitScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                // Taken before the case draws anything, so the line the arming check looks for
                // can only have been written by this case. The renderer latches it at its first
                // split, and ctest gives every case a process of its own.
                m_logBefore = LibraryLogSize();

                // 0..3: the unit square as a strip. 4..6: its lower-left triangle. 7..9: its
                // upper-right triangle - the two share the diagonal, so together they cover every
                // sample of the square exactly once.
                static const float kVertices[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                                                  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            }

            void TearDown() override {
                if (Ready()) {
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_STENCIL_TEST);
                    glDisable(GL_BLEND);
                    glDisable(GL_RASTERIZER_DISCARD);
                    glDepthMask(GL_TRUE);
                    glStencilMask(0xFF);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glUseProgram(0);
                    glBindVertexArray(0);
                    BindDefaultFramebuffer();
                    if (m_rectProgram != 0) glDeleteProgram(m_rectProgram);
                    if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                    for (GLuint rbo : m_renderbuffers) glDeleteRenderbuffers(1, &rbo);
                    for (GLuint fbo : m_framebuffers) glDeleteFramebuffers(1, &fbo);
                    for (ColorFbo& target : m_colorTargets) DestroyColorFbo(target);
                }
                ScenarioTest::TearDown();
            }

            // In the SplitRecording lane (budget pinned below what every case draws, DirectVulkan,
            // a log to read), a split must have happened during this case. Elsewhere this case is
            // the unsplit control and there is nothing to arm.
            void ExpectTheRecordingWasSplitWhereTheBudgetIsPinned() {
                const unsigned long budget = PinnedDrawBudget();
                const bool splitExpected = budget > 0 && budget * 2 < static_cast<unsigned long>(kFillers) &&
                                           Gl().BackendName() == std::string("DirectVulkan") &&
                                           !LibraryLogPath().empty();
                RecordProperty("arming_checked", splitExpected ? "yes" : "no");
                if (!splitExpected) return;
                const std::string appended = LibraryLogSince(m_logBefore);
                EXPECT_NE(appended.find("draws into one command buffer; submitting it"), std::string::npos)
                    << "MOBILEGL_MAGMA_MAX_DRAWS_PER_COMMAND_BUFFER=" << budget << " and this case recorded more "
                    << "than " << kFillers << " draws in one frame, but DirectVulkan never reported splitting the "
                       "recording. A renderer that does not split grows a single command buffer without bound "
                       "(rd12 on Adreno: ~40k driver mappings, then SIGABRT). See "
                       "VulkanRenderer::SplitOversizedRecording. Log appended by this case:\n"
                    << appended;
            }

            bool BuildRectProgram(std::string* error) {
                m_rectProgram = CompileProgram(kRectVS, kRectFS, error);
                if (m_rectProgram == 0) return false;
                m_rectLoc = glGetUniformLocation(m_rectProgram, "uRect");
                m_depthLoc = glGetUniformLocation(m_rectProgram, "uDepth");
                m_colorLoc = glGetUniformLocation(m_rectProgram, "uColor");
                glUseProgram(m_rectProgram);
                glBindVertexArray(m_vao);
                return true;
            }

            // The rectangle [x0, x1] x [y0, y1] in NDC; `first`/`count` pick the strip or a triangle.
            void DrawRect(float x0, float y0, float x1, float y1, float depth, float r, float g, float b, float a,
                          GLenum mode = GL_TRIANGLE_STRIP, GLint first = 0, GLsizei count = 4) {
                glUniform4f(m_rectLoc, x0, y0, x1, y1);
                glUniform1f(m_depthLoc, depth);
                glUniform4f(m_colorLoc, r, g, b, a);
                glDrawArrays(mode, first, count);
            }

            // One pixel of the 32x32 grid, by index.
            void DrawPixel(int index, float r, float g, float b, float a) {
                const float x = static_cast<float>(index % kSide);
                const float y = static_cast<float>(index / kSide);
                DrawRect(x / 16.0f - 1.0f, y / 16.0f - 1.0f, (x + 1.0f) / 16.0f - 1.0f, (y + 1.0f) / 16.0f - 1.0f, 0.0f,
                         r, g, b, a);
            }

            // Draws that reach the renderer and rasterize nothing: zero-area strips.
            void DrawFillers(int count) {
                for (int i = 0; i < count; ++i) {
                    DrawRect(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
                }
            }

            // A kSide x kSide RGBA8 target, optionally with a DEPTH24_STENCIL8 renderbuffer.
            // Returns fbo 0 when the driver will not complete it.
            ColorFbo MakeTarget(bool withDepthStencil) {
                ColorFbo target = MakeColorFbo(kSide, kSide);
                if (target.fbo == 0) return target;
                m_colorTargets.push_back(target);
                if (withDepthStencil) {
                    GLuint rbo = 0;
                    glGenRenderbuffers(1, &rbo);
                    m_renderbuffers.push_back(rbo);
                    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kSide, kSide);
                    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        target.fbo = 0;
                    }
                }
                return target;
            }

            Image ReadTarget(const ColorFbo& target) {
                glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                return ReadPixels(kSide, kSide);
            }

            PipeStatsWindow::LogMark m_logBefore{};
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_rectProgram = 0;
            GLint m_rectLoc = -1;
            GLint m_depthLoc = -1;
            GLint m_colorLoc = -1;
            std::vector<ColorFbo> m_colorTargets;
            std::vector<GLuint> m_renderbuffers;
            std::vector<GLuint> m_framebuffers;
        };

    } // namespace

    // kDraws single-pixel quads into a fresh target, one uniform value per draw, with no
    // readback, flush or rebind in between. Every pixel must hold its own draw's colour.
    TEST_F(OversizedRecordingSplitScenario, EveryDrawOfTheFrameLandsAcrossCommandBufferSplits) {
        if (!Ready()) return;

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        ColorFbo target = MakeTarget(false);
        ASSERT_NE(target.fbo, 0u) << "MakeColorFbo failed";

        BindFbo(target);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
        glUseProgram(program);
        glBindVertexArray(m_vao);
        const GLint indexLocation = glGetUniformLocation(program, "uIndex");
        for (int i = 0; i < kDraws; ++i) {
            glUniform1i(indexLocation, i);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        const Image image = ReadPixels(kSide, kSide);
        glUseProgram(0);
        glDeleteProgram(program);

        EXPECT_TRUE(EveryPixelHoldsItsOwnDraw(image));
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (a) An occluder drawn before the split must still fail the depth test of a draw after it:
    // the depth attachment's contents have to survive the render pass ending and re-beginning.
    TEST_F(OversizedRecordingSplitScenario, ADepthOccluderDrawnBeforeTheSplitHidesADrawAfterIt) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;
        ColorFbo target = MakeTarget(true);
        if (target.fbo == 0) GTEST_SKIP() << "no complete RGBA8 + DEPTH24_STENCIL8 target on this driver";

        BindFbo(target);
        glClearDepth(1.0);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);

        DrawRect(-1.0f, -1.0f, 0.0f, 1.0f, -0.5f, 1.0f, 0.0f, 0.0f, 1.0f); // left half, near, red
        DrawFillers(kFillers);
        DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f); // everything, far, green

        const Image image = ReadTarget(target);
        EXPECT_TRUE(ColumnsAre(image, 0, kSide / 2 - 1, Rgba8{255, 0, 0, 255}, 0,
                               "the near occluder drawn before the split must hide the far draw after it"));
        EXPECT_TRUE(ColumnsAre(image, kSide / 2, kSide - 1, Rgba8{0, 255, 0, 255}, 0,
                               "where nothing occludes, the far draw after the split must land"));
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (b) A stencil mark made before the split must gate a draw after it.
    TEST_F(OversizedRecordingSplitScenario, AStencilMarkMadeBeforeTheSplitGatesADrawAfterIt) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;
        ColorFbo target = MakeTarget(true);
        if (target.fbo == 0) GTEST_SKIP() << "no complete RGBA8 + DEPTH24_STENCIL8 target on this driver";

        BindFbo(target);
        glDisable(GL_DEPTH_TEST);
        glStencilMask(0xFF);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glEnable(GL_STENCIL_TEST);

        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        DrawRect(-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f); // left half: stencil 1, blue
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        DrawFillers(kFillers);
        DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f); // everything, green, where stencil == 1

        const Image image = ReadTarget(target);
        EXPECT_TRUE(ColumnsAre(image, 0, kSide / 2 - 1, Rgba8{0, 255, 0, 255}, 0,
                               "where the mark was made before the split, the draw after it must pass"));
        EXPECT_TRUE(ColumnsAre(image, kSide / 2, kSide - 1, Rgba8{0, 0, 0, 255}, 0,
                               "where no mark was made, the draw after the split must be stencilled out"));
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (c) Additive blending onto pixels written before the split: every one of kSpanDraws
    // full-target draws adds exactly 1/255 of red onto a target whose green was written first,
    // so the result counts every draw on both sides of every split.
    TEST_F(OversizedRecordingSplitScenario, BlendingAccumulatesOntoPixelsWrittenBeforeTheSplit) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;
        ColorFbo target = MakeTarget(false);
        ASSERT_NE(target.fbo, 0u) << "MakeColorFbo failed";

        BindFbo(target);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);

        DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 55.0f / 255.0f, 0.0f, 0.0f);
        for (int i = 0; i < kSpanDraws; ++i) {
            DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f / 255.0f, 0.0f, 0.0f, 0.0f);
        }

        const Image image = ReadTarget(target);
        EXPECT_TRUE(ColumnsAre(image, 0, kSide - 1, Rgba8{static_cast<std::uint8_t>(kSpanDraws), 55, 0, 0}, 1,
                               "every additive draw, and the green written before any split, must be in the sum"));
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (d) A multisample target drawn on both sides of the split, then resolved. Before the
    // split a triangle covers the lower-left half of the target; after it, the complementary
    // triangle covers the upper-right half. On the diagonal each pixel's samples are shared
    // between the two, so only a target that kept every SAMPLE across the split resolves them to
    // red + green = 255. One that was resolved or re-broadcast at the split gives about 190
    // there, and one whose samples were dropped gives black or garbage.
    TEST_F(OversizedRecordingSplitScenario, AMultisampleTargetKeepsItsSamplesAcrossTheSplit) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;

        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        if (maxSamples < 2) {
            GTEST_SKIP() << "GL_MAX_SAMPLES is " << maxSamples << "; this needs a multisample renderbuffer";
        }
        const GLsizei samples = std::min<GLint>(4, maxSamples);
        GLuint msFbo = 0, msRbo = 0;
        glGenFramebuffers(1, &msFbo);
        m_framebuffers.push_back(msFbo);
        glGenRenderbuffers(1, &msRbo);
        m_renderbuffers.push_back(msRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, kSide, kSide);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msRbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            GTEST_SKIP() << "no complete " << samples << "x multisample RGBA8 renderbuffer on this driver";
        }
        ColorFbo resolved = MakeTarget(false);
        ASSERT_NE(resolved.fbo, 0u) << "MakeColorFbo failed";

        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
        glViewport(0, 0, kSide, kSide);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, GL_TRIANGLES, 4, 3); // lower-left, red
        DrawFillers(kFillers);
        DrawRect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, GL_TRIANGLES, 7, 3); // upper-right, green

        glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved.fbo);
        glBlitFramebuffer(0, 0, kSide, kSide, 0, 0, kSide, kSide, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        const Image image = ReadTarget(resolved);

        int wrong = 0;
        std::string first;
        for (int y = 0; y < kSide && wrong < 1000; ++y) {
            for (int x = 0; x < kSide; ++x) {
                const Rgba8 seen = image.At(x, y);
                bool ok = false;
                // The diagonal x + y = kSide in pixel units passes through the pixels whose
                // x + y == kSide - 1; every other pixel lies wholly on one side of it.
                if (x + y < kSide - 1) {
                    ok = seen == Rgba8{255, 0, 0, 255};
                } else if (x + y > kSide - 1) {
                    ok = seen == Rgba8{0, 255, 0, 255};
                } else {
                    ok = seen.b == 0 && seen.a == 255 && seen.r > 0 && seen.g > 0 &&
                         std::abs(static_cast<int>(seen.r) + static_cast<int>(seen.g) - 255) <= 3;
                }
                if (!ok) {
                    if (wrong == 0) {
                        std::ostringstream os;
                        os << "(" << x << ", " << y << ") = " << Describe(seen);
                        first = os.str();
                    }
                    ++wrong;
                }
            }
        }
        EXPECT_EQ(wrong, 0) << wrong << " resolved pixels are wrong; first " << first
                            << ". Lower-left of the diagonal must be red, upper-right green, and a pixel on it "
                               "red + green = 255 with both present (its samples were split between a draw before "
                               "the command-buffer split and one after it)";
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (e) Occlusion queries open across the split. GL_SAMPLES_PASSED counts kSpanDraws
    // single-pixel draws on both sides of it; GL_ANY_SAMPLES_PASSED is answered by the one draw
    // made before the split, the fillers after it pass nothing - with a fillers-only control
    // that must answer false.
    TEST_F(OversizedRecordingSplitScenario, OcclusionQueriesOpenAcrossTheSplitCountBothSides) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;
        ColorFbo target = MakeTarget(false);
        ASSERT_NE(target.fbo, 0u) << "MakeColorFbo failed";
        BindFbo(target);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);

        GLuint queries[3] = {};
        glGenQueries(3, queries);
        FirstGLError();

        // Any samples: control first (fillers only), then one real pixel before the split.
        glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[0]);
        const unsigned int anyBeginError = FirstGLError();
        if (anyBeginError != 0u) {
            glDeleteQueries(3, queries);
            GTEST_SKIP() << Gl().BackendName() << " does not accept GL_ANY_SAMPLES_PASSED ("
                         << GLErrorName(anyBeginError) << ")";
        }
        DrawFillers(kFillers);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[1]);
        DrawPixel(0, 1.0f, 1.0f, 1.0f, 1.0f);
        DrawFillers(kFillers);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        GLuint anyControl = 7, anyAcross = 7;
        glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &anyControl);
        glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &anyAcross);
        EXPECT_EQ(anyControl, static_cast<GLuint>(GL_FALSE)) << "fillers alone rasterize nothing";
        EXPECT_EQ(anyAcross, static_cast<GLuint>(GL_TRUE))
            << "the only passing draw was made before the split; the query that spans it lost that answer";

        // Samples passed: kSpanDraws distinct single-sample pixels, spanning at least one split.
        glBeginQuery(GL_SAMPLES_PASSED, queries[2]);
        const unsigned int samplesBeginError = FirstGLError();
        if (samplesBeginError == 0u) {
            for (int i = 0; i < kSpanDraws; ++i) {
                DrawPixel(i, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            glEndQuery(GL_SAMPLES_PASSED);
            GLuint samplesPassed = 0;
            glGetQueryObjectuiv(queries[2], GL_QUERY_RESULT, &samplesPassed);
            if (Gl().BackendName() == std::string("DirectGLES")) {
                // OpenGL ES has no sample-counting occlusion query, only ANY_SAMPLES_PASSED*, so
                // DirectGLES answers GL_SAMPLES_PASSED with a 0/1 approximation (DirectGLES.cpp,
                // the occlusion-query result path). The exact count is not this backend's to give.
                RecordProperty("samples_passed", "0/1 approximation on OpenGL ES; exact count not checked");
                EXPECT_EQ(samplesPassed, 1u) << "samples passed, so the approximation must read 1";
            } else {
                EXPECT_EQ(samplesPassed, static_cast<GLuint>(kSpanDraws))
                    << "each of the " << kSpanDraws << " draws covers exactly one single-sample pixel";
            }
        } else {
            RecordProperty("samples_passed", std::string("not accepted: ") + GLErrorName(samplesBeginError));
        }
        glDeleteQueries(3, queries);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (e) GL_PRIMITIVES_GENERATED open across the split: kSpanDraws strips of two triangles.
    // A one-draw query first establishes that this stack counts at all.
    TEST_F(OversizedRecordingSplitScenario, APrimitivesGeneratedQueryOpenAcrossTheSplitCountsBothSides) {
        if (!Ready()) return;
        std::string error;
        ASSERT_TRUE(BuildRectProgram(&error)) << error;
        ColorFbo target = MakeTarget(false);
        ASSERT_NE(target.fbo, 0u) << "MakeColorFbo failed";
        BindFbo(target);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);

        GLuint queries[2] = {};
        glGenQueries(2, queries);
        FirstGLError();
        glBeginQuery(GL_PRIMITIVES_GENERATED, queries[0]);
        const unsigned int beginError = FirstGLError();
        if (beginError != 0u) {
            glDeleteQueries(2, queries);
            GTEST_SKIP() << Gl().BackendName() << " does not accept GL_PRIMITIVES_GENERATED ("
                         << GLErrorName(beginError) << ")";
        }
        DrawPixel(0, 1.0f, 1.0f, 1.0f, 1.0f);
        glEndQuery(GL_PRIMITIVES_GENERATED);
        GLuint control = 0;
        glGetQueryObjectuiv(queries[0], GL_QUERY_RESULT, &control);
        if (control == 0u) {
            glDeleteQueries(2, queries);
            GTEST_SKIP() << "GL_PRIMITIVES_GENERATED reads 0 for a single unsplit strip on " << Gl().BackendName()
                         << " (" << Gl().RendererString() << "): nothing this stack counts, so nothing to carry";
        }
        ASSERT_EQ(control, 2u) << "one strip of four vertices is two triangles";

        glBeginQuery(GL_PRIMITIVES_GENERATED, queries[1]);
        for (int i = 0; i < kSpanDraws; ++i) {
            DrawPixel(i, 1.0f, 1.0f, 1.0f, 1.0f);
        }
        glEndQuery(GL_PRIMITIVES_GENERATED);
        GLuint generated = 0;
        glGetQueryObjectuiv(queries[1], GL_QUERY_RESULT, &generated);
        EXPECT_EQ(generated, static_cast<GLuint>(2 * kSpanDraws))
            << kSpanDraws << " strips of two triangles each, drawn across at least one split";
        glDeleteQueries(2, queries);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

    // (f) Transform feedback active across the split: kSpanDraws one-point draws, draw i
    // capturing 2i + 1, must append in order into one buffer with nothing past the end, and
    // GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN must count every one.
    TEST_F(OversizedRecordingSplitScenario, ATransformFeedbackCaptureActiveAcrossTheSplitAppendsEveryDraw) {
        if (!Ready()) return;

        auto compile = [](GLenum stage, const char* source, std::string* log) -> GLuint {
            const GLuint shader = glCreateShader(stage);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char buffer[2048] = {};
                glGetShaderInfoLog(shader, sizeof(buffer) - 1, nullptr, buffer);
                *log = buffer;
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        };
        std::string log;
        const GLuint vs = compile(GL_VERTEX_SHADER, kCaptureVS, &log);
        ASSERT_NE(vs, 0u) << log;
        const GLuint fs = compile(GL_FRAGMENT_SHADER, kCaptureFS, &log);
        ASSERT_NE(fs, 0u) << log;
        const GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        const char* varyings[] = {"vs_value"};
        glTransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char buffer[2048] = {};
            glGetProgramInfoLog(program, sizeof(buffer) - 1, nullptr, buffer);
            glDeleteProgram(program);
            FAIL() << "capture program failed to link: " << buffer;
        }

        constexpr int kSlack = 4;
        std::vector<float> poison(kSpanDraws + kSlack, kCapturePoison);
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(poison.size() * sizeof(float)),
                     poison.data(), GL_STATIC_COPY);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
        GLuint written = 0;
        glGenQueries(1, &written);

        glUseProgram(program);
        glBindVertexArray(m_vao);
        const GLint indexLocation = glGetUniformLocation(program, "uIndex");
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, written);
        glBeginTransformFeedback(GL_POINTS);
        for (int i = 0; i < kSpanDraws; ++i) {
            glUniform1i(indexLocation, i);
            glDrawArrays(GL_POINTS, 0, 1);
        }
        glEndTransformFeedback();
        glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
        glDisable(GL_RASTERIZER_DISCARD);
        glUseProgram(0);

        GLuint primitivesWritten = 0;
        glGetQueryObjectuiv(written, GL_QUERY_RESULT, &primitivesWritten);
        std::vector<float> captured(poison.size(), 0.0f);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(captured.size() * sizeof(float)),
                           captured.data());
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
        glDeleteQueries(1, &written);
        glDeleteBuffers(1, &buffer);
        glDeleteProgram(program);

        EXPECT_EQ(primitivesWritten, static_cast<GLuint>(kSpanDraws));
        int wrong = 0;
        std::string first;
        for (int i = 0; i < static_cast<int>(captured.size()); ++i) {
            const float expected = i < kSpanDraws ? static_cast<float>(2 * i + 1) : kCapturePoison;
            if (captured[i] != expected) {
                if (wrong == 0) {
                    std::ostringstream os;
                    os << "record " << i << " = " << captured[i] << ", expected " << expected;
                    first = os.str();
                }
                ++wrong;
            }
        }
        EXPECT_EQ(wrong, 0) << wrong << " of " << captured.size() << " records are wrong; first " << first
                            << " (records past " << kSpanDraws << " must keep the poison)";
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTheRecordingWasSplitWhereTheBudgetIsPinned();
    }

} // namespace MGITest
