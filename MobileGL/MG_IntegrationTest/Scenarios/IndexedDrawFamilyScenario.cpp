// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/IndexedDrawFamilyScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE DRAW FAMILY P5b's d1 PUTS ON THE WIRE (MG_Remote/CONTRACT-P5B.md §2 d1): one
// VBO, one EBO, one program, one VAO, and one picture per draw entry point whose correctness
// DEPENDS on the fields that entry point adds to draw_vbo. Two quads live in the vertex buffer,
// a left one and a right one, and every case draws exactly one of them or both through a
// different entry point:
//
//   glDrawElements                    the element-buffer offset (Start = offset / IndexSize)
//   glDrawElements, no EBO bound      a CLIENT index array - the kDrawHasUserIndices span the
//                                     client stages into SEG_STAGE (the P8 resolve-on-client
//                                     rule, applied by d1)
//   glDrawElementsBaseVertex          IndexBias: the same six indices land on the other quad
//   glDrawRangeElements               kDrawHasIndexRange with MinIndex / MaxIndex
//   glDrawElementsInstancedBaseVertex InstanceCount: the Minecraft trace's own slot
//                                     (improved-transparency-minecraft-26.3 first-stops here)
//   glMultiDrawElementsBaseVertex     NumDraws = 2 with a per-range base vertex
//   glMultiDrawArrays                 NumDraws = 2, arrays
//   glMultiDrawElementsIndirect       kDrawIsIndirect, the MGPDrawIndirect second tail
//
// It is an ORDINARY GL scenario and runs in every lane; the DirectGLES.Split. entries run the
// same bodies under MOBILEGL_TRANSPORT=inproc, where a field that did not cross is a wrong
// picture (the other quad, or no quad) rather than a green. The harness destructor's emit-seq
// check (ScenarioFixture.h) is the statement that records crossed at all; the boxes below are
// the statement that the RIGHT fields crossed.
//
// No glFlush anywhere, for TriangleScenario's reason (its header, point 2): glReadPixels is the
// ordering point on both backends and the SEG_REPLY round trip under split.

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

        // gl_InstanceID shifts a quad one full quad-width to the right per instance, so the
        // second instance of the LEFT quad lands exactly on the RIGHT quad's box: an instance
        // count that did not cross draws one quad, one that did draws two.
        constexpr const char* kVertexSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() {
    gl_Position = vec4(aPos.x + float(gl_InstanceID), aPos.y, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
out vec4 oColor;
void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        struct Vertex {
            float x, y;
        };

        // Vertices 0..3: the LEFT quad's corners; 4..7: the RIGHT quad's corners (for the
        // indexed draws). Vertices 8..13 and 14..19: the same two quads as six vertices each
        // (for the arrays draws). x spans [-0.9, -0.1] and [0.1, 0.9]; y spans [-0.8, 0.8].
        constexpr Vertex kVertices[20] = {
            {-0.9f, -0.8f}, {-0.1f, -0.8f}, {-0.1f, 0.8f}, {-0.9f, 0.8f}, // 0..3 left
            {0.1f, -0.8f},  {0.9f, -0.8f},  {0.9f, 0.8f},  {0.1f, 0.8f},  // 4..7 right
            {-0.9f, -0.8f}, {-0.1f, -0.8f}, {-0.1f, 0.8f},                 // 8..13 left, arrays
            {-0.1f, 0.8f},  {-0.9f, 0.8f},  {-0.9f, -0.8f},
            {0.1f, -0.8f},  {0.9f, -0.8f},  {0.9f, 0.8f},                  // 14..19 right, arrays
            {0.9f, 0.8f},   {0.1f, 0.8f},   {0.1f, -0.8f},
        };

        // Indices 0..5 draw the left quad; 6..11 draw the right one. GL_UNSIGNED_SHORT, so the
        // second run starts at BYTE offset 12 and Start = 6 on the wire.
        constexpr std::uint16_t kIndices[12] = {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4};
        constexpr GLsizeiptr kRightRunByteOffset = 6 * sizeof(std::uint16_t);

        struct DrawElementsIndirectCommand {
            std::uint32_t count, instanceCount, firstIndex, baseVertex, baseInstance;
        };

        class IndexedDrawFamilyScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(kVertices)), kVertices, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glEnableVertexAttribArray(0);
                glGenBuffers(1, &m_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(kIndices)), kIndices, GL_STATIC_DRAW);
                ASSERT_EQ(FirstGLError(), 0u) << "building the VBO, the EBO and the VAO";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
                if (m_indirect != 0) glDeleteBuffers(1, &m_indirect);
                if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                m_indirect = m_ebo = m_vbo = m_vao = m_program = 0;
            }

            // Clear to blue, run `draw`, read back. The draw is the ONLY thing that differs
            // between the cases.
            template <class Draw>
            Image ClearThenDrawThenRead(Draw draw) {
                HeadlessGL& gl = Gl();
                BindDefaultFramebuffer();
                glViewport(0, 0, gl.Width(), gl.Height());
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                draw();
                return ReadPixels(gl.Width(), gl.Height());
            }

            // The interior of the left quad, of the right quad, and the bottom-left corner
            // that no quad covers (it carries the clear).
            void ExpectLeft(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, (w * 20) / 100, (w * 30) / 100, (h * 40) / 100,
                                           (h * 60) / 100, color, 0.0, when + " (left quad)"));
            }
            void ExpectRight(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, (w * 70) / 100, (w * 80) / 100, (h * 40) / 100,
                                           (h * 60) / 100, color, 0.0, when + " (right quad)"));
            }
            void ExpectCorner(const Image& image, const char* color, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                EXPECT_TRUE(RegionIsMostly(image, 0, (w * 3) / 100, 0, (h * 3) / 100, color, 0.0,
                                           when + " (corner, the clear)"));
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
            unsigned int m_ebo = 0;
            unsigned int m_indirect = 0;
        };

    } // namespace

    // The census's own first blocker for every Minecraft trace: an element-buffer glDrawElements.
    // The byte offset selects the RIGHT quad, so an offset that crossed as 0 (or not at all)
    // paints the left one.
    TEST_F(IndexedDrawFamilyScenario, AnElementBufferDrawElementsOffsetSelectsTheRightQuad) {
        if (!Ready() || IsSkipped()) return;
        const Image image = ClearThenDrawThenRead([] {
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,
                           reinterpret_cast<const void*>(kRightRunByteOffset));
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectRight(image, "green", "glDrawElements at byte offset 12");
        ExpectLeft(image, "blue", "glDrawElements at byte offset 12 must not paint the left quad");
        ExpectCorner(image, "blue", "glDrawElements");
        // The frame boundary, once, so the lane reaches Present as TriangleScenario does.
        Gl().EndFrame();
    }

    // No element buffer bound: `indices` is the application's own array. Under split the client
    // stages the twelve bytes and the record names the run (kDrawHasUserIndices); the server
    // resolves it for the call only. The array names the RIGHT quad's vertices directly.
    TEST_F(IndexedDrawFamilyScenario, AClientIndexArrayIsStagedAndDrawsTheQuadItNames) {
        if (!Ready() || IsSkipped()) return;
        static const std::uint16_t kClientIndices[6] = {4, 5, 6, 6, 7, 4};
        const Image image = ClearThenDrawThenRead([] {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); // the VAO's element slot, emptied
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, kClientIndices);
        });
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectRight(image, "green", "glDrawElements from a client index array");
        ExpectLeft(image, "blue", "a client index array naming vertices 4..7 must not paint the left quad");
        ExpectCorner(image, "blue", "client index array");
    }

    // The same six indices as the left quad, plus a base vertex of 4: IndexBias is what moves
    // the picture to the right quad.
    TEST_F(IndexedDrawFamilyScenario, DrawElementsBaseVertexMovesTheSameIndicesToTheOtherQuad) {
        if (!Ready() || IsSkipped()) return;
        const Image image = ClearThenDrawThenRead([] {
            glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 4);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectRight(image, "green", "glDrawElementsBaseVertex(basevertex = 4)");
        ExpectLeft(image, "blue", "a base vertex of 4 must not paint the left quad");
    }

    // kDrawHasIndexRange: the ranged form with the right quad's index range and byte offset.
    TEST_F(IndexedDrawFamilyScenario, DrawRangeElementsDrawsTheRangedRun) {
        if (!Ready() || IsSkipped()) return;
        const Image image = ClearThenDrawThenRead([] {
            glDrawRangeElements(GL_TRIANGLES, 4, 7, 6, GL_UNSIGNED_SHORT,
                                reinterpret_cast<const void*>(kRightRunByteOffset));
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectRight(image, "green", "glDrawRangeElements(4..7) at byte offset 12");
        ExpectLeft(image, "blue", "glDrawRangeElements must not paint the left quad");
    }

    // The Minecraft trace's own entry point (improved-transparency-minecraft-26.3 first-stops at
    // DrawElementsInstancedBaseVertex). Two instances of the LEFT quad: gl_InstanceID shifts the
    // second onto the right box, so an InstanceCount that did not cross paints one quad.
    TEST_F(IndexedDrawFamilyScenario, DrawElementsInstancedBaseVertexPaintsOneQuadPerInstance) {
        if (!Ready() || IsSkipped()) return;
        const Image two = ClearThenDrawThenRead([] {
            glDrawElementsInstancedBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 2, 0);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectLeft(two, "green", "instance 0 of the left quad");
        ExpectRight(two, "green", "instance 1 of the left quad, shifted by gl_InstanceID");
        ExpectCorner(two, "blue", "instanced draw");

        const Image one = ClearThenDrawThenRead([] {
            glDrawElementsInstancedBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, 1, 0);
        });
        ExpectLeft(one, "green", "one instance");
        ExpectRight(one, "blue", "one instance must not paint the right quad");
    }

    // NumDraws = 2 with a per-range base vertex (the census's MultiDrawElementsBaseVertex, 18
    // entries): both quads from the same six indices.
    TEST_F(IndexedDrawFamilyScenario, MultiDrawElementsBaseVertexPaintsEverySubDraw) {
        if (!Ready() || IsSkipped()) return;
        const Image image = ClearThenDrawThenRead([] {
            const GLsizei counts[2] = {6, 6};
            const void* offsets[2] = {nullptr, nullptr};
            const GLint baseVertices[2] = {0, 4};
            glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_SHORT, offsets, 2, baseVertices);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectLeft(image, "green", "sub-draw 0 (base vertex 0)");
        ExpectRight(image, "green", "sub-draw 1 (base vertex 4)");
        ExpectCorner(image, "blue", "multi-draw");
    }

    // NumDraws = 2, arrays: the six-vertex copies of both quads.
    TEST_F(IndexedDrawFamilyScenario, MultiDrawArraysPaintsEverySubDraw) {
        if (!Ready() || IsSkipped()) return;
        const Image image = ClearThenDrawThenRead([] {
            const GLint firsts[2] = {8, 14};
            const GLsizei counts[2] = {6, 6};
            glMultiDrawArrays(GL_TRIANGLES, firsts, counts, 2);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectLeft(image, "green", "sub-draw 0 (first 8)");
        ExpectRight(image, "green", "sub-draw 1 (first 14)");
        ExpectCorner(image, "blue", "multi-draw arrays");
    }

    // kDrawIsIndirect: two DrawElementsIndirectCommands in a GL_DRAW_INDIRECT_BUFFER, the second
    // at firstIndex 6. The record carries the buffer's handle, the byte offset and the count;
    // the server reads the commands from ITS copy of the buffer and never from a host pointer.
    TEST_F(IndexedDrawFamilyScenario, MultiDrawElementsIndirectDrawsFromTheIndirectBuffer) {
        if (!Ready() || IsSkipped()) return;
        const DrawElementsIndirectCommand commands[2] = {{6, 1, 0, 0, 0}, {6, 1, 6, 0, 0}};
        glGenBuffers(1, &m_indirect);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirect);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, GLsizeiptr(sizeof(commands)), commands, GL_STATIC_DRAW);
        ASSERT_EQ(FirstGLError(), 0u) << "building the indirect buffer";
        const Image both = ClearThenDrawThenRead([] {
            glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, nullptr, 2, 0);
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectLeft(both, "green", "indirect command 0");
        ExpectRight(both, "green", "indirect command 1 (firstIndex 6)");
        ExpectCorner(both, "blue", "indirect draw");

        // The second command alone, by byte offset: Offset on the wire is the call's own.
        const Image second = ClearThenDrawThenRead([] {
            glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                                   reinterpret_cast<const void*>(sizeof(DrawElementsIndirectCommand)));
        });
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectRight(second, "green", "glDrawElementsIndirect at byte offset 20");
        ExpectLeft(second, "blue", "the second command alone must not paint the left quad");
    }

} // namespace MGITest
