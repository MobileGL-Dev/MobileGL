// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ViewportArrayScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - gl_ViewportIndex ACTUALLY ROUTES, AND THE PER-INDEX STATE IT SELECTS IS REAL.
//
// The state half of ARB_viewport_array is asserted in MG_Test/State/RenderStateTest.cpp, which
// is a pure set/get exercise and would pass just as green against a backend that stores all 16
// rectangles and rasterizes only the first. This file is the other half: every case here routes
// primitives to a viewport OTHER than 0 and then looks at where the pixels landed.
//
// Three claims, one per case:
//   1. gl_ViewportIndex selects the viewport RECTANGLE - a 4x4 grid of 32x32 viewports, one
//      geometry-shader invocation per cell, and every cell must hold its own index.
//   2. gl_ViewportIndex selects the DEPTH RANGE - 16 one-pixel-wide viewports whose ranges are
//      (i/16, 1 - i/16), a quad at each end of clip space, and gl_FragCoord.z read back.
//      This is the claim that fails loudest against a single-viewport backend, because the
//      geometry is still in the right place while every depth comes back as viewport 0's.
//   3. The per-index SCISSOR TEST ENABLE is honoured. Vulkan has no per-viewport scissor-test
//      toggle, so a disabled index has to be given the whole framebuffer as its rectangle; the
//      case draws the same primitive into the same index twice, once with the test off and once
//      with it on, and requires the two results to differ in the documented direction.
//
// Case 1 runs a second time against the DEFAULT framebuffer. MobileGL Y-flips (and pre-transform
// rotates) the default framebuffer's rectangles and does not touch an FBO's, so a port that
// applies the flip to viewport 0 and forgets the other fifteen renders a correct-looking FBO and
// an upside-down window - the classic multi-viewport bug, and invisible to every FBO-only case.
//
// BOTH BACKENDS RUN EVERY CASE, by two completely different routes, which is the point of
// keeping them in one file. DirectVulkan declares sixteen viewports on the pipeline and lets the
// hardware route. DirectGLES has one viewport, one scissor rectangle and one depth range and no
// gl_ViewportIndex at all, so it EMULATES: the builtin becomes a flat varying, the fragment stage
// gets a gate, and the draw is replayed once per distinct viewport state (Managers.h,
// ForEachViewportRoutingPass). Every assertion below is about pixels, so it cannot tell the two
// apart - which is exactly what has to be true.
//
// DirectVulkan skips when the device lacks the multiViewport feature - Vulkan then forbids a
// pipeline from declaring more than one viewport at all, which is a device limit and not a
// MobileGL bug; lavapipe (every CI lane) and both Mali/Adreno devices support it, so the cases do
// run where it matters.
//
// The last case is the negative control for the emulation and runs on DirectGLES only: it builds
// the SAME program with the emulation switched off and requires the routing to collapse onto
// viewport 0. Without it every assertion above could be satisfied by a backend that happened to
// be right for some other reason, and the emulation's own switch would be untested.

#include <cmath>
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

        constexpr int kViewportCount = 16;
        constexpr int kGridSide = 4;   // 4x4 grid of viewports
        constexpr int kCellSize = 32;  // ... each 32x32
        constexpr int kSurfaceSide = kGridSide * kCellSize;
        constexpr GLint kUnwritten = -1;

        // A geometry shader is the only stage GL 4.1 lets write gl_ViewportIndex, and
        // `invocations` runs it once per viewport off a single input point - the same shape
        // KHR-GL43.viewport_array.draw_to_single_layer_with_multiple_viewports uses.
        const char* const kVertexSource = R"(#version 410 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

        const char* const kGridGeometrySource = R"(#version 410 core
layout(points, invocations = 16) in;
layout(triangle_strip, max_vertices = 4) out;
flat out int gsIndex;
void main() {
    gsIndex = gl_InvocationID;
    gl_ViewportIndex = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

        // One invocation, viewport chosen by a uniform: lets a case draw the SAME primitive into
        // the SAME index twice under two different scissor-enable states.
        const char* const kSingleGeometrySource = R"(#version 410 core
layout(points, invocations = 1) in;
layout(triangle_strip, max_vertices = 4) out;
uniform int uViewport;
flat out int gsIndex;
void main() {
    gsIndex = uViewport;
    gl_ViewportIndex = uViewport;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

        const char* const kIntFragmentSource = R"(#version 410 core
flat in int gsIndex;
layout(location = 0) out int fragColor;
void main() { fragColor = gsIndex; }
)";

        // Two quads, one at each end of clip space, so the fragment stage can report the depth
        // the viewport's range mapped them to. gl_FragCoord.z IS the post-range window depth, so
        // it reads back the per-viewport minDepth/maxDepth directly.
        const char* const kDepthGeometrySource = R"(#version 410 core
layout(points, invocations = 16) in;
layout(triangle_strip, max_vertices = 8) out;
void main() {
    gl_ViewportIndex = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, -1.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, -1.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  0.0, -1.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  0.0, -1.0, 1.0); EmitVertex();
    EndPrimitive();
    gl_Position = vec4(-1.0,  0.0, 1.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  0.0, 1.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 1.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 1.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

        const char* const kDepthFragmentSource = R"(#version 410 core
layout(location = 0) out float fragColor;
void main() { fragColor = gl_FragCoord.z; }
)";

        class ViewportArrayScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                GLint maxViewports = 0;
                glGetIntegerv(GL_MAX_VIEWPORTS, &maxViewports);
                ASSERT_GE(maxViewports, kViewportCount) << "GL 4.3 core requires GL_MAX_VIEWPORTS >= 16";

                m_program = BuildProgram(kGridGeometrySource, kIntFragmentSource);
                ASSERT_NE(m_program, 0u) << "grid program failed to build: " << m_buildLog;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                ResetViewportArrayState();
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                ResetViewportArrayState();
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            // Every case starts from the same slate: this fixture shares its context with every
            // other scenario in the process, and a leftover per-index scissor enable is exactly
            // the kind of state that would make a later case pass or fail for the wrong reason.
            static void ResetViewportArrayState() {
                for (int i = 0; i < kViewportCount; ++i) {
                    glDisablei(GL_SCISSOR_TEST, static_cast<GLuint>(i));
                }
                glDisable(GL_SCISSOR_TEST);
                glViewport(0, 0, kSurfaceSide, kSurfaceSide);
                glScissor(0, 0, kSurfaceSide, kSurfaceSide);
                glDepthRange(0.0, 1.0);
                glDisable(GL_DEPTH_TEST);
            }

            // The 4x4 grid: viewport y*4+x covers the cell whose lower-left corner is
            // (x*cellW, y*cellH), in GL's bottom-left-origin window coordinates. Parameterized on
            // the cell size because the default framebuffer this scenario also renders into is
            // deliberately non-square (HeadlessGL is 128x96, so a transposing bug cannot hide).
            static void SetupGridViewports(int cellW, int cellH) {
                std::vector<GLfloat> data(static_cast<size_t>(kViewportCount) * 4);
                for (int y = 0; y < kGridSide; ++y) {
                    for (int x = 0; x < kGridSide; ++x) {
                        const size_t base = static_cast<size_t>(y * kGridSide + x) * 4;
                        data[base + 0] = static_cast<GLfloat>(x * cellW);
                        data[base + 1] = static_cast<GLfloat>(y * cellH);
                        data[base + 2] = static_cast<GLfloat>(cellW);
                        data[base + 3] = static_cast<GLfloat>(cellH);
                    }
                }
                glViewportArrayv(0, kViewportCount, data.data());
            }

            GLuint BuildProgram(const char* geometrySource, const char* fragmentSource) {
                const GLuint vs = CompileStage(GL_VERTEX_SHADER, kVertexSource);
                if (vs == 0) return 0;
                const GLuint gs = CompileStage(GL_GEOMETRY_SHADER, geometrySource);
                if (gs == 0) {
                    glDeleteShader(vs);
                    return 0;
                }
                const GLuint fs = CompileStage(GL_FRAGMENT_SHADER, fragmentSource);
                if (fs == 0) {
                    glDeleteShader(vs);
                    glDeleteShader(gs);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, gs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                glDeleteShader(vs);
                glDeleteShader(gs);
                glDeleteShader(fs);
                if (!linked) {
                    GLint length = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                    std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
                    m_buildLog = log.data();
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            GLuint CompileStage(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled) return shader;
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
                m_buildLog = log.data();
                glDeleteShader(shader);
                return 0;
            }

            // An R32I colour target, pre-filled with kUnwritten so "nothing was drawn here" is
            // distinguishable from "index 0 was drawn here".
            struct IntTarget {
                GLuint fbo = 0;
                GLuint texture = 0;
            };

            // The "nothing drawn here" value is UPLOADED, not cleared: the CTS fills its R32I
            // targets the same way (fillTexture), and an upload cannot be confused with a clear
            // that a backend defers, reorders or drops - which is exactly the ambiguity a case
            // asserting "this cell must be untouched" cannot afford.
            static void FillIntTarget(const IntTarget& target, int width, int height) {
                const std::vector<GLint> unwritten(static_cast<size_t>(width) * height, kUnwritten);
                glBindTexture(GL_TEXTURE_2D, target.texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED_INTEGER, GL_INT, unwritten.data());
            }

            static IntTarget MakeIntTarget(int width, int height) {
                IntTarget target;
                glGenTextures(1, &target.texture);
                glBindTexture(GL_TEXTURE_2D, target.texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32I, width, height, 0, GL_RED_INTEGER, GL_INT, nullptr);
                glGenFramebuffers(1, &target.fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
                FillIntTarget(target, width, height);
                return target;
            }

            static void DestroyIntTarget(IntTarget& target) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (target.fbo != 0) glDeleteFramebuffers(1, &target.fbo);
                if (target.texture != 0) glDeleteTextures(1, &target.texture);
            }

            static std::vector<GLint> ReadInts(int width, int height) {
                std::vector<GLint> pixels(static_cast<size_t>(width) * height, 0);
                glReadPixels(0, 0, width, height, GL_RED_INTEGER, GL_INT, pixels.data());
                return pixels;
            }

            // The centre of grid cell (x, y), in the bottom-left-origin coordinates glReadPixels
            // returns. Sampling the centre rather than a corner keeps the assertion about WHICH
            // viewport was selected rather than about edge rounding.
            static GLint CellCentre(const std::vector<GLint>& pixels, int stride, int x, int y) {
                const int px = x * kCellSize + kCellSize / 2;
                const int py = y * kCellSize + kCellSize / 2;
                return pixels[static_cast<size_t>(py) * stride + px];
            }

            std::string m_buildLog;
            GLuint m_program = 0;
            GLuint m_vao = 0;
        };

        // --- 1. the viewport rectangle -------------------------------------------------------

        TEST_F(ViewportArrayScenario, EachViewportIndexRasterizesIntoItsOwnRectangle) {
            IntTarget target = MakeIntTarget(kSurfaceSide, kSurfaceSide);
            SetupGridViewports(kCellSize, kCellSize);
            glUseProgram(m_program);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_POINTS, 0, 1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);

            const std::vector<GLint> pixels = ReadInts(kSurfaceSide, kSurfaceSide);
            for (int y = 0; y < kGridSide; ++y) {
                for (int x = 0; x < kGridSide; ++x) {
                    const GLint expected = y * kGridSide + x;
                    EXPECT_EQ(CellCentre(pixels, kSurfaceSide, x, y), expected)
                        << "cell (" << x << ", " << y << ") should hold viewport index " << expected
                        << "; a single-viewport backend paints the whole image with 15 (the last invocation)";
                }
            }
            DestroyIntTarget(target);
        }

        // The same claim against the DEFAULT framebuffer, where MobileGL applies its Y-flip and
        // pre-transform rotation. Index 0 alone getting the mapping is the classic bug.
        TEST_F(ViewportArrayScenario, TheDefaultFramebufferAppliesTheSameFlipToEveryViewport) {
            const int surfaceW = Gl().Width();
            const int surfaceH = Gl().Height();
            ASSERT_GE(surfaceW, kGridSide);
            ASSERT_GE(surfaceH, kGridSide);
            const int cellW = surfaceW / kGridSide;
            const int cellH = surfaceH / kGridSide;

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            // Paint a value no viewport index can produce, so an unwritten cell is obvious.
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // The default framebuffer is 8-bit RGBA, so the index travels as a colour: cell i is
            // painted with red = i * 16, which is exact in 8 bits for i in [0, 16).
            const char* const kColorFragmentSource = R"(#version 410 core
flat in int gsIndex;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(float(gsIndex) * 16.0 / 255.0, 0.0, 0.0, 1.0); }
)";
            const GLuint colorProgram = BuildProgram(kGridGeometrySource, kColorFragmentSource);
            ASSERT_NE(colorProgram, 0u) << "colour program failed to build: " << m_buildLog;

            SetupGridViewports(cellW, cellH);
            glUseProgram(colorProgram);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_POINTS, 0, 1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);

            std::vector<unsigned char> pixels(static_cast<size_t>(surfaceW) * surfaceH * 4, 0);
            glReadPixels(0, 0, surfaceW, surfaceH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (int y = 0; y < kGridSide; ++y) {
                for (int x = 0; x < kGridSide; ++x) {
                    const int px = x * cellW + cellW / 2;
                    const int py = y * cellH + cellH / 2;
                    const int red = pixels[(static_cast<size_t>(py) * surfaceW + px) * 4];
                    const int expected = (y * kGridSide + x) * 16;
                    // One LSB of slack for an 8-bit round trip; the values are 16 apart, so this
                    // cannot confuse two neighbouring indices.
                    EXPECT_LE(std::abs(red - expected), 1)
                        << "default-framebuffer cell (" << x << ", " << y << ") holds red=" << red << ", expected "
                        << expected << ". A vertically mirrored grid means the Y-flip was applied to viewport 0 "
                        << "only";
                }
            }
            glDeleteProgram(colorProgram);
        }

        // --- 2. the depth range --------------------------------------------------------------

        TEST_F(ViewportArrayScenario, EachViewportIndexUsesItsOwnDepthRange) {
            // 16 columns one pixel wide and two rows tall: row 0 gets the near-plane quad, row 1
            // the far-plane one, so both ends of viewport i's range land in the same column.
            constexpr int kWidth = kViewportCount;
            constexpr int kHeight = 2;

            GLuint texture = 0;
            GLuint fbo = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kWidth, kHeight, 0, GL_RED, GL_FLOAT, nullptr);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            const GLfloat clearValue[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 0, clearValue);

            std::vector<GLfloat> viewports(static_cast<size_t>(kViewportCount) * 4);
            std::vector<GLdouble> ranges(static_cast<size_t>(kViewportCount) * 2);
            for (int i = 0; i < kViewportCount; ++i) {
                viewports[static_cast<size_t>(i) * 4 + 0] = static_cast<GLfloat>(i);
                viewports[static_cast<size_t>(i) * 4 + 1] = 0.0f;
                viewports[static_cast<size_t>(i) * 4 + 2] = 1.0f;
                viewports[static_cast<size_t>(i) * 4 + 3] = 2.0f;
                ranges[static_cast<size_t>(i) * 2 + 0] = static_cast<GLdouble>(i) / 16.0;
                ranges[static_cast<size_t>(i) * 2 + 1] = 1.0 - static_cast<GLdouble>(i) / 16.0;
            }
            glViewportArrayv(0, kViewportCount, viewports.data());
            glDepthRangeArrayv(0, kViewportCount, ranges.data());

            const GLuint depthProgram = BuildProgram(kDepthGeometrySource, kDepthFragmentSource);
            ASSERT_NE(depthProgram, 0u) << "depth program failed to build: " << m_buildLog;
            glUseProgram(depthProgram);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_POINTS, 0, 1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);

            std::vector<GLfloat> pixels(static_cast<size_t>(kWidth) * kHeight, 0.0f);
            glReadPixels(0, 0, kWidth, kHeight, GL_RED, GL_FLOAT, pixels.data());
            for (int i = 0; i < kViewportCount; ++i) {
                const float nearDepth = static_cast<float>(i) / 16.0f;
                const float farDepth = 1.0f - static_cast<float>(i) / 16.0f;
                // The tolerance covers depth-buffer-free rasterization of gl_FragCoord.z on a
                // software rasterizer; the per-index values are 1/16 apart, so it cannot let a
                // neighbouring viewport's range through, and viewport 0's range (0, 1) differs
                // from every other index by at least 1/16.
                EXPECT_NEAR(pixels[i], nearDepth, 1.0e-3f)
                    << "viewport " << i << " near-plane depth; got viewport 0's range if this is 0";
                EXPECT_NEAR(pixels[static_cast<size_t>(kWidth) + i], farDepth, 1.0e-3f)
                    << "viewport " << i << " far-plane depth; got viewport 0's range if this is 1";
            }

            glDeleteProgram(depthProgram);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
        }

        // --- 3. the per-index scissor-test enable --------------------------------------------

        TEST_F(ViewportArrayScenario, AnIndexedScissorEnableClipsOnlyThatIndex) {
            IntTarget target = MakeIntTarget(kSurfaceSide, kSurfaceSide);

            // One full-size viewport per index so the scissor rectangle is the ONLY thing that
            // can shrink the quad - the same separation KHR-GL43.viewport_array.scissor uses.
            glViewport(0, 0, kSurfaceSide, kSurfaceSide);
            std::vector<GLint> boxes(static_cast<size_t>(kViewportCount) * 4);
            for (int y = 0; y < kGridSide; ++y) {
                for (int x = 0; x < kGridSide; ++x) {
                    const size_t base = static_cast<size_t>(y * kGridSide + x) * 4;
                    boxes[base + 0] = x * kCellSize;
                    boxes[base + 1] = y * kCellSize;
                    boxes[base + 2] = kCellSize;
                    boxes[base + 3] = kCellSize;
                }
            }
            glScissorArrayv(0, kViewportCount, boxes.data());

            const GLuint singleProgram = BuildProgram(kSingleGeometrySource, kIntFragmentSource);
            ASSERT_NE(singleProgram, 0u) << "single-viewport program failed to build: " << m_buildLog;
            glUseProgram(singleProgram);
            glBindVertexArray(m_vao);
            const GLint uViewport = glGetUniformLocation(singleProgram, "uViewport");
            ASSERT_NE(uViewport, -1);

            constexpr GLint kProbeIndex = 6;  // grid cell (2, 1)
            constexpr int kProbeX = kProbeIndex % kGridSide;
            constexpr int kProbeY = kProbeIndex / kGridSide;

            // (a) scissor test ENABLED for this index: the quad is clipped to its 32x32 box.
            glUniform1i(uViewport, kProbeIndex);
            glEnablei(GL_SCISSOR_TEST, kProbeIndex);
            glDrawArrays(GL_POINTS, 0, 1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            {
                const std::vector<GLint> pixels = ReadInts(kSurfaceSide, kSurfaceSide);
                EXPECT_EQ(CellCentre(pixels, kSurfaceSide, kProbeX, kProbeY), kProbeIndex)
                    << "the scissored index must still paint inside its own box";
                for (int y = 0; y < kGridSide; ++y) {
                    for (int x = 0; x < kGridSide; ++x) {
                        if (x == kProbeX && y == kProbeY) continue;
                        EXPECT_EQ(CellCentre(pixels, kSurfaceSide, x, y), kUnwritten)
                            << "cell (" << x << ", " << y << ") is outside scissor rectangle " << kProbeIndex
                            << " and must be untouched";
                    }
                }
            }

            // (b) scissor test DISABLED for the same index, everything else identical: with no
            // per-viewport toggle in Vulkan this is the case that needs the disabled index to be
            // given the full framebuffer rectangle, and it is exactly where "leave the last
            // rectangle bound" would show up as a still-clipped quad.
            FillIntTarget(target, kSurfaceSide, kSurfaceSide);
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glDisablei(GL_SCISSOR_TEST, kProbeIndex);
            glDrawArrays(GL_POINTS, 0, 1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            {
                const std::vector<GLint> pixels = ReadInts(kSurfaceSide, kSurfaceSide);
                for (int y = 0; y < kGridSide; ++y) {
                    for (int x = 0; x < kGridSide; ++x) {
                        EXPECT_EQ(CellCentre(pixels, kSurfaceSide, x, y), kProbeIndex)
                            << "with the scissor test off for index " << kProbeIndex
                            << ", its full-viewport quad must cover cell (" << x << ", " << y << ")";
                    }
                }
            }

            glDeleteProgram(singleProgram);
            DestroyIntTarget(target);
        }

        // --- 4. the negative control for the DirectGLES emulation -----------------------------
        //
        // Everything above is a claim about pixels, and a claim about pixels cannot tell an
        // emulation that works from a backend that was going to be right anyway. This case builds
        // the SAME program in a process started with MOBILEGL_FORCE_VIEWPORT_ARRAY_EMULATION=0
        // (the NoViewportArrayEmulation. ctest entry) and requires case 1's
        // result to COLLAPSE: with no routing, every geometry invocation rasterizes against
        // viewport 0's rectangle, so the last invocation paints the whole surface and every cell
        // reads 15 instead of its own index. That is the pre-emulation behaviour this backend had
        // (and the failure signature KHR-GL43.viewport_array reported on it), pinned here so that
        // (a) the three cases above are known to be testing the emulation and not the weather,
        // and (b) the switch itself has a test.
        //
        // DirectGLES only: the flag steers nothing on DirectVulkan, which routes natively.
        TEST_F(ViewportArrayScenario, WithoutTheEmulationEveryIndexCollapsesOntoViewportZero) {
            if (Gl().BackendName() != "DirectGLES") {
                GTEST_SKIP() << "the emulation switch is a DirectGLES concern; DirectVulkan routes "
                                "gl_ViewportIndex natively and ignores it";
            }

            // The switch comes from the ENVIRONMENT, and this case runs only in a process that
            // was started with it off. It used to write MG_Config::Features directly, which is
            // not available to it any more: on Android this module links the shipping
            // libMobileGL.so - so that the on-device run validates the real artifact - and that
            // library exports no such symbol. The process-wide variable is also the more honest
            // spelling of the control, since it is the one a developer chasing this failure
            // would actually set. CMakeLists.txt registers the NoViewportArrayEmulation. ctest
            // entry for it, so the control still runs in every ctest run; anywhere else - the
            // ambient ctest entries, or the binary run straight from a device shell - the
            // emulation is on and this case skips.
            if (AmbientQuirkFromEnvironment("MOBILEGL_FORCE_VIEWPORT_ARRAY_EMULATION") != AmbientQuirk::Off) {
                GTEST_SKIP() << "this is the negative control for the emulation and needs it off for the "
                                "whole process; the NoViewportArrayEmulation. ctest entry runs it with "
                                "MOBILEGL_FORCE_VIEWPORT_ARRAY_EMULATION=0";
            }

            IntTarget target = MakeIntTarget(kSurfaceSide, kSurfaceSide);
            SetupGridViewports(kCellSize, kCellSize);

            GLuint unroutedProgram = 0;
            {
                // A program of its own rather than the fixture's, even though in this process
                // the fixture's was built unrouted too: the emitted ESSL is decided at link
                // time and memoized on a key that carries this flag, and building it here keeps
                // what this case measures independent of when SetUp happened to link.
                unroutedProgram = BuildProgram(kGridGeometrySource, kIntFragmentSource);
                ASSERT_NE(unroutedProgram, 0u) << "unrouted program failed to build: " << m_buildLog;
                glUseProgram(unroutedProgram);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_POINTS, 0, 1);
                ASSERT_EQ(glGetError(), GL_NO_ERROR);
            }

            const std::vector<GLint> pixels = ReadInts(kSurfaceSide, kSurfaceSide);
            // Cell (0, 0) IS viewport 0's rectangle, so it is the one cell an unrouted draw paints
            // with something. Everything it holds comes from the last geometry invocation.
            EXPECT_EQ(CellCentre(pixels, kSurfaceSide, 0, 0), kViewportCount - 1)
                << "with the emulation off, viewport 0's rectangle must hold the LAST invocation's "
                   "index - if it holds 0 the routing is still happening and this control proves "
                   "nothing";
            for (int y = 0; y < kGridSide; ++y) {
                for (int x = 0; x < kGridSide; ++x) {
                    if (x == 0 && y == 0) continue;
                    EXPECT_EQ(CellCentre(pixels, kSurfaceSide, x, y), kUnwritten)
                        << "cell (" << x << ", " << y << ") is outside viewport 0's rectangle and an "
                        << "unrouted draw cannot reach it";
                }
            }

            glUseProgram(0);
            glDeleteProgram(unroutedProgram);
            DestroyIntTarget(target);
        }

        // --- 5. an explicitly EMPTY scissor box clips, it does not mean "never written" --------
        //
        // Deliberately NOT a ViewportArrayScenario case, because that fixture's geometry stage
        // routes and this claim needs none of it: one viewport, one scissor rectangle, no
        // geometry stage - and it has to hold identically whether or not anything routes.
        //
        // glScissor(0, 0, 0, 0) is legal GL meaning "the scissor test rejects every fragment",
        // but it is byte-identical to the all-zero rectangle a context starts with, whose meaning
        // is the OPPOSITE ("the whole window", which the frontend cannot spell before a surface
        // exists). DirectGLES resolved the collision from the EXTENT, so it substituted the whole
        // surface for a deliberately empty box and inverted the request into "clip nothing" -
        // and did so on every draw, at any origin, no matter how many times the application had
        // already called glScissor. KHR-GL43.viewport_array.scissor_zero_dimension is the
        // conformance shape of exactly this, and it is what the written-flag now separates.

        const char* const kFullScreenVertexSource = R"(#version 330 core
void main() {
    // One clip-space-covering triangle straight from gl_VertexID: no buffers, no attributes,
    // and nothing that could clip the draw except the scissor rectangle under test.
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)";

        const char* const kConstantIntFragmentSource = R"(#version 330 core
layout(location = 0) out int fragColor;
void main() { fragColor = 7; }
)";
        constexpr GLint kPainted = 7;

        class EmptyScissorScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                m_program = BuildQuadProgram();
                ASSERT_NE(m_program, 0u) << "full-screen program failed to build: " << m_buildLog;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);

                glGenTextures(1, &m_texture);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32I, kSurfaceSide, kSurfaceSide, 0, GL_RED_INTEGER, GL_INT,
                             nullptr);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE)
                    << "R32I is required to be colour-renderable; an incomplete target would make every "
                       "assertion below vacuous";

                glViewport(0, 0, kSurfaceSide, kSurfaceSide);
                glDisable(GL_DEPTH_TEST);
                ResetScissorState();
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                // The context is shared with every other scenario in the process, and a leftover
                // 0x0 scissor box with the test enabled would silently blank whatever runs next.
                ResetScissorState();
                glScissor(0, 0, kSurfaceSide, kSurfaceSide);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            static void ResetScissorState() {
                for (int i = 0; i < kViewportCount; ++i) {
                    glDisablei(GL_SCISSOR_TEST, static_cast<GLuint>(i));
                }
                glDisable(GL_SCISSOR_TEST);
            }

            // Uploaded, not cleared, for the reason FillIntTarget gives - and here for a second
            // one that is decisive: glClear is ITSELF scissored, so a clear issued under the very
            // state this case is testing would be clipped away and prove nothing.
            void FillTarget() const {
                const std::vector<GLint> unwritten(static_cast<size_t>(kSurfaceSide) * kSurfaceSide, kUnwritten);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSurfaceSide, kSurfaceSide, GL_RED_INTEGER, GL_INT,
                                unwritten.data());
            }

            static std::vector<GLint> ReadTarget() {
                std::vector<GLint> pixels(static_cast<size_t>(kSurfaceSide) * kSurfaceSide, 0);
                glReadPixels(0, 0, kSurfaceSide, kSurfaceSide, GL_RED_INTEGER, GL_INT, pixels.data());
                return pixels;
            }

            GLuint BuildQuadProgram() {
                const GLuint vs = CompileOne(GL_VERTEX_SHADER, kFullScreenVertexSource);
                if (vs == 0) return 0;
                const GLuint fs = CompileOne(GL_FRAGMENT_SHADER, kConstantIntFragmentSource);
                if (fs == 0) {
                    glDeleteShader(vs);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                glDeleteShader(vs);
                glDeleteShader(fs);
                if (linked) return program;
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
                m_buildLog = log.data();
                glDeleteProgram(program);
                return 0;
            }

            GLuint CompileOne(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled) return shader;
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
                glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
                m_buildLog = log.data();
                glDeleteShader(shader);
                return 0;
            }

            std::string m_buildLog;
            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_fbo = 0;
            GLuint m_texture = 0;
        };

        TEST_F(EmptyScissorScenario, AnExplicitlyEmptyScissorBoxClipsEveryFragment) {
            // Positive control FIRST. Without it a regression that simply lost the draw entirely
            // would sail through the half below, which only asserts that nothing was painted.
            FillTarget();
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, kSurfaceSide, kSurfaceSide);
            glUseProgram(m_program);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            {
                const std::vector<GLint> pixels = ReadTarget();
                ASSERT_EQ(pixels.front(), kPainted) << "control: a full-surface scissor box must not clip";
                ASSERT_EQ(pixels.back(), kPainted) << "control: a full-surface scissor box must not clip";
            }

            // The case itself, and note it runs AFTER an explicit glScissor - the old
            // extent-based sentinel misfired here too, which is what made this a live rendering
            // bug and not just a first-frame startup quirk.
            FillTarget();
            glScissor(0, 0, 0, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);
            {
                const std::vector<GLint> pixels = ReadTarget();
                for (size_t i = 0; i < pixels.size(); ++i) {
                    ASSERT_EQ(pixels[i], kUnwritten)
                        << "texel " << i << " was painted through a 0x0 scissor box: the empty rectangle was "
                           "substituted with the whole surface, inverting 'clip everything' into 'clip nothing'";
                }
            }
        }

        TEST_F(EmptyScissorScenario, IndexedZeroDimensionScissorBoxesClipEveryFragment) {
            // The conformance shape: setup4x4Scissor(..., set_zeros=true) writes all 16 boxes
            // through glScissorArrayv with zero extents at a 4x4 grid of origins and enables the
            // test on every index. Index 0's box is (0, 0, 0, 0) - byte-identical to the
            // never-written default - which is precisely the collision the written flag breaks.
            // Backends that collapse every index to 0 (DirectGLES today) still pass: index 0's
            // box is empty, so the draw is clipped away, which is what the case requires.
            FillTarget();
            std::vector<GLint> boxes(static_cast<size_t>(kViewportCount) * 4, 0);
            for (int i = 0; i < kViewportCount; ++i) {
                boxes[static_cast<size_t>(i) * 4 + 0] = (i % kGridSide) * kCellSize;
                boxes[static_cast<size_t>(i) * 4 + 1] = (i / kGridSide) * kCellSize;
                // width and height stay 0 - that IS the case.
            }
            glScissorArrayv(0, kViewportCount, boxes.data());
            for (int i = 0; i < kViewportCount; ++i) {
                glEnablei(GL_SCISSOR_TEST, static_cast<GLuint>(i));
            }
            glUseProgram(m_program);
            glBindVertexArray(m_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ASSERT_EQ(glGetError(), GL_NO_ERROR);

            const std::vector<GLint> pixels = ReadTarget();
            for (size_t i = 0; i < pixels.size(); ++i) {
                ASSERT_EQ(pixels[i], kUnwritten) << "texel " << i << " was painted through a zero-extent indexed "
                                                                     "scissor box";
            }
        }

    } // namespace
} // namespace MGITest
