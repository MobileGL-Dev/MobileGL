// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TriangleScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE SMALLEST THING THAT DRAWS: one VBO, one program, one VAO, a clear, a
// VBO-backed glDrawArrays and a glReadPixels.
//
// This is target B of P5's reduced path (BRIEF-P5 4). It is an ORDINARY GL scenario and runs
// in every lane; the DirectGLES.Split. entries run the same two cases with
// MOBILEGL_TRANSPORT=inproc, where the same body is the smallest workload that crosses a real
// ring. Four things about its shape are decisions rather than defaults, and all four come from
// the measured verb census (~/w7/notes/p5/verb-census.md):
//
// 1. THE DRAW IS VBO-BACKED AND HAS NO CLIENT-ARRAY INDICES. glDrawArrays against a buffer
//    bound to GL_ARRAY_BUFFER, never glDrawElements with a client pointer. That keeps
//    kDrawHasUserIndices' MGHostSpan out of the first IPC frame entirely - the split filling of
//    a host span is P8 - and it is why table 0 can pin kCapNeedsHostIndexBytes and
//    kCapNeedsHostUboBytes at 0 for the whole of P5.
//
// 2. THERE IS NO glFlush, AND ITS ABSENCE IS THE POINT. `Flush` is not a GLFunctionsTable slot
//    at all, and MG_Impl/GLImpl/Exporting/Definitions.cpp:111-112 makes glFlush() and glFinish()
//    LITERALLY EMPTY BODIES - one MGLOG_D and a return. A scenario that called glFlush to order
//    its readback would be ordering nothing and would still pass, which makes the ordering it
//    believes in unfalsifiable. glReadPixels IS the ordering point: it is a blocking readback on
//    both backends today and a SEG_REPLY round trip under split, so the pixels it returns are
//    the pixels the draw produced or the case fails.
//
// 3. THE FIRST CASE ENDS WITH EndFrame(), DELIBERATELY. `Present` has ZERO call sites in
//    MG_Impl - it is reached only through EGLImpl.cpp:178 -> BackendObject.cpp:396 - so a
//    scenario that never swaps never touches it, and B would then exercise a STRICT SUBSET of
//    what ClearThenReadPixelsScenario already covers. BRIEF-P5 4.B lists Present(67) among the
//    catalogue rows this target needs, so the frame boundary is here on purpose: B is A's slot
//    set plus DrawArrays, not minus Present.
//
// 4. THE SECOND CASE REDRAWS ACROSS A FRAME BOUNDARY WITHOUT REBUILDING ANYTHING. The VBO, the
//    program and the VAO outlive the swap and only the clear colour changes. Under split that
//    is the difference between "the client re-declares every object every frame" (which would
//    pass a single-frame test and be the whole cost of the design) and a steady state; under
//    monolith it is the backend's per-frame retire/aging path, which is exactly where P3a's
//    respecify bug lived.

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

        // #version 330 core, because that is what the retrace lane's
        // MESA_GLSL_VERSION_OVERRIDE pins and what every other scenario in this module that does
        // not need a later feature uses.
        constexpr const char* kVertexSource = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() { oColor = vec4(vColor, 1.0); }
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // A single triangle with its base at y = -0.8 and its apex at y = +0.8, so the
        // interior box the cases assert on (the middle 10% of the width, a fifth of the way up)
        // is far from every edge and the corner box they assert the CLEAR on is far outside it.
        // Flat green: one colour over the whole primitive means an offender pixel is a real
        // disagreement rather than an interpolation rounding difference between two drivers.
        constexpr Vertex kTriangle[3] = {
            {-0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
            {0.8f, -0.8f, 0.0f, 1.0f, 0.0f},
            {0.0f, 0.8f, 0.0f, 1.0f, 0.0f},
        };

        class TriangleScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                // Ready() is false both when there is no GPU and when the base SetUp skipped a
                // Split lane that has no client to assert against (ScenarioFixture.h).
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(kTriangle)), kTriangle, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<void*>(2 * sizeof(float)));
                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);
                ASSERT_EQ(FirstGLError(), 0u) << "building the one VBO and one VAO this scenario has";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                m_vbo = m_vao = m_program = 0;
            }

            // Clear, draw, read back. No glFlush between the draw and the readback: see the
            // file header, point 2.
            Image ClearThenDrawThenRead(float clearR, float clearG, float clearB) {
                HeadlessGL& gl = Gl();
                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(clearR, clearG, clearB, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                return ReadPixels(gl.Width(), gl.Height());
            }

            // The box inside the triangle: the middle tenth of the width, a fifth of the way up
            // from the base, which is interior for the vertex set above at any surface size the
            // harness uses.
            void ExpectTriangleInterior(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, (w * 45) / 100, (w * 55) / 100, (h * 20) / 100,
                                           (h * 30) / 100, color, 0.0, when));
            }

            // The bottom-left corner, which is below the triangle's base and left of its left
            // edge, so it carries the clear and nothing else.
            void ExpectClearedCorner(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, 0, (w * 5) / 100, 0, (h * 5) / 100, color, 0.0, when));
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
        };

    } // namespace

    // The reduced path's target B, in one case: GetCaps (reached by the first glCompileShader of
    // the context, not by any verb - verb-census trap 2), Clear, DrawArrays, ReadPixels, Present.
    TEST_F(TriangleScenario, AVboBackedTriangleReachesReadPixels) {
        if (!Ready() || IsSkipped()) return;

        const Image image = ClearThenDrawThenRead(0.0f, 0.0f, 1.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTriangleInterior(image, "green", "the interior of a VBO-backed glDrawArrays triangle");
        ExpectClearedCorner(image, "blue", "the corner outside the triangle, which carries the clear");

        // The frame boundary, deliberately (file header, point 3): this is the only thing in the
        // scenario that reaches the Present slot.
        Gl().EndFrame();
    }

    // Steady state: the same VBO, program and VAO across a swap, with only the clear colour
    // changing. Nothing is re-created, so a client that re-declared its objects every frame and
    // a backend that lost them at the frame boundary both show up here and in no single-frame
    // case.
    TEST_F(TriangleScenario, TheSameVboAndVaoRedrawAcrossAFrameBoundary) {
        if (!Ready() || IsSkipped()) return;

        const Image first = ClearThenDrawThenRead(0.0f, 0.0f, 1.0f);
        ExpectTriangleInterior(first, "green", "frame 0's triangle");
        ExpectClearedCorner(first, "blue", "frame 0's clear");
        Gl().EndFrame();

        // Second frame: black clear, nothing else touched.
        const Image second = ClearThenDrawThenRead(0.0f, 0.0f, 0.0f);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectTriangleInterior(second, "green",
                               "frame 1's triangle, drawn from the SAME VBO and VAO with no "
                               "re-specification of either");
        ExpectClearedCorner(second, "black", "frame 1's clear, which is the only thing that changed");
        Gl().EndFrame();
    }

} // namespace MGITest
