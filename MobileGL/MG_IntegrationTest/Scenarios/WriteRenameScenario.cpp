// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/WriteRenameScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glBufferSubData INTO A SMALL BUFFER AN EARLIER DRAW OF THE SAME FRAME STILL READS.
//
// P15 S0 (M4): DirectVulkan's wire arm answers such a write by giving the store a fresh buffer
// filled from a host shadow (write renaming) instead of a staged copy that ends the render pass.
// What renaming must get right:
//
//   * ORDER: the draw before the write reads the old bytes, the draw after reads the new ones.
//   * CARRY: bytes the write did not touch are in the new buffer (the shadow holds them).
//   * EVERY BIND SITE sees the new buffer: vertex, index and uniform buffers.
//   * ANY ALIGNMENT: a one-byte write renames too (the staged copy needed four-byte ranges).
//   * ACROSS FRAMES: a renamed store keeps drawing right in later frames.
//
// DirectGLES is the control; MOBILEGL_MAGMA_WRITE_RENAME=0 takes the staged copy.

#include <cstring>
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

        // Position in [0, 1] and a per-vertex colour.
        constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() { vColor = aColor; gl_Position = vec4(aPos * 2.0 - 1.0, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
in vec4 vColor;
layout(std140) uniform Tint { vec4 uTint; };
out vec4 o0;
void main() { o0 = vColor * uTint; }
)";

        constexpr int kSize = 64;

        struct Vertex {
            float x, y;
            unsigned char r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 12, "tight vertex");

        class WriteRenameScenario : public ScenarioTest {};

        // Four vertices of an axis-aligned quad as a triangle strip.
        void PutQuad(std::vector<Vertex>& v, int first, float x0, float y0, float x1, float y1, unsigned char r,
                     unsigned char g, unsigned char b) {
            v[first + 0] = {x0, y0, r, g, b, 255};
            v[first + 1] = {x1, y0, r, g, b, 255};
            v[first + 2] = {x0, y1, r, g, b, 255};
            v[first + 3] = {x1, y1, r, g, b, 255};
        }

        ::testing::AssertionResult Near(const Rgba8& got, int r, int g, int b) {
            const auto close = [](int v, int want) { return v >= want - 3 && v <= want + 3; };
            if (close(got.r, r) && close(got.g, g) && close(got.b, b)) return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "got (" << int(got.r) << ", " << int(got.g) << ", " << int(got.b)
                                                 << "), want (" << r << ", " << g << ", " << b << ")";
        }

        Rgba8 At(int x, int y) { return ReadPixelsRect(x, y, 1, 1).At(0, 0); }

        struct Scene {
            GLuint program = 0, fbo = 0, color = 0, vao = 0, vbo = 0, ibo = 0, ubo = 0;
        };

        // A target, the program, a VAO over `vertices` (float2 position + ubyte4 colour), a
        // four-index strip index buffer and a white tint.
        Scene MakeScene(const std::vector<Vertex>& vertices, std::string* error) {
            Scene s;
            s.program = CompileProgram(kVS, kFS, error);
            if (s.program == 0) return s;
            glUniformBlockBinding(s.program, glGetUniformBlockIndex(s.program, "Tint"), 0);
            glGenTextures(1, &s.color);
            glBindTexture(GL_TEXTURE_2D, s.color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
            glGenFramebuffers(1, &s.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.color, 0);
            glViewport(0, 0, kSize, kSize);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            glGenVertexArrays(1, &s.vao);
            glBindVertexArray(s.vao);
            glGenBuffers(1, &s.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(),
                         GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                                  reinterpret_cast<const void*>(offsetof(Vertex, r)));
            const unsigned short indices[] = {0, 1, 2, 3};
            glGenBuffers(1, &s.ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_DYNAMIC_DRAW);
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            glGenBuffers(1, &s.ubo);
            glBindBuffer(GL_UNIFORM_BUFFER, s.ubo);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(white), white, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, s.ubo);
            glUseProgram(s.program);
            return s;
        }

        void Destroy(Scene& s) {
            glDeleteBuffers(1, &s.ubo);
            glDeleteBuffers(1, &s.ibo);
            glDeleteBuffers(1, &s.vbo);
            glDeleteVertexArrays(1, &s.vao);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &s.fbo);
            glDeleteTextures(1, &s.color);
            glDeleteProgram(s.program);
        }

    } // namespace

    TEST_F(WriteRenameScenario, AVertexWriteBetweenDrawsKeepsOrderAndCarriesTheUntouchedBytes) {
        if (!Ready()) return;
        std::vector<Vertex> v(8);
        PutQuad(v, 0, 0.0f, 0.0f, 0.5f, 1.0f, 255, 0, 0); // left half, red
        PutQuad(v, 4, 0.5f, 0.0f, 1.0f, 1.0f, 0, 255, 0); // right half, green
        std::string error;
        Scene s = MakeScene(v, &error);
        ASSERT_NE(s.program, 0u) << error;

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // left half red: the bytes before the write
        // Rewrite the first quad only: the top-left quarter, blue. The second quad's bytes are
        // not part of the write.
        std::vector<Vertex> q(4);
        PutQuad(q, 0, 0.0f, 0.5f, 0.5f, 1.0f, 0, 0, 255);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(q.size() * sizeof(Vertex)), q.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4); // right half: the untouched second quad
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // top-left quarter: the new first quad
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 4, kSize / 4), 255, 0, 0)) << "bottom-left: the draw before the write";
        EXPECT_TRUE(Near(At(kSize / 4, kSize * 3 / 4), 0, 0, 255)) << "top-left: the draw after the write";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 255, 0)) << "right: bytes the write did not touch";
        Destroy(s);
    }

    TEST_F(WriteRenameScenario, AOneByteWriteBetweenDrawsIsSeenByTheNextDrawOnly) {
        if (!Ready()) return;
        std::vector<Vertex> v(4);
        PutQuad(v, 0, 0.0f, 0.0f, 0.5f, 1.0f, 255, 0, 0); // left half, red
        std::string error;
        Scene s = MakeScene(v, &error);
        ASSERT_NE(s.program, 0u) << error;

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // Move the quad right (eight-byte position writes) and add blue to every vertex with
        // one-byte writes, which no four-byte-aligned copy can carry.
        std::vector<Vertex> moved(4);
        PutQuad(moved, 0, 0.5f, 0.0f, 1.0f, 1.0f, 255, 0, 0);
        for (int i = 0; i < 4; ++i)
            glBufferSubData(GL_ARRAY_BUFFER, i * sizeof(Vertex), 8, &moved[i]);
        for (int i = 0; i < 4; ++i) {
            const unsigned char blue = 255;
            glBufferSubData(GL_ARRAY_BUFFER, i * sizeof(Vertex) + offsetof(Vertex, b), 1, &blue);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "left: the draw before the writes";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 255, 0, 255)) << "right: red plus the one-byte blue";
        Destroy(s);
    }

    TEST_F(WriteRenameScenario, UniformAndIndexWritesBetweenDrawsAreSeenByTheNextDraw) {
        if (!Ready()) return;
        std::vector<Vertex> v(8);
        PutQuad(v, 0, 0.0f, 0.0f, 0.5f, 1.0f, 255, 255, 255); // left half, white
        PutQuad(v, 4, 0.5f, 0.0f, 1.0f, 1.0f, 255, 255, 255); // right half, white
        std::string error;
        Scene s = MakeScene(v, &error);
        ASSERT_NE(s.program, 0u) << error;

        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        glBindBuffer(GL_UNIFORM_BUFFER, s.ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(red), red);
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr); // left half, red
        // Now the tint is green and the indices name the second quad.
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(green), green);
        const unsigned short second[] = {4, 5, 6, 7};
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(second), second);
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr); // right half, green
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "left: tint and indices before the writes";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 255, 0)) << "right: tint and indices after the writes";
        Destroy(s);
    }

    // Two renames of ONE store inside one frame: the second must not reuse the buffer the first
    // draw still reads (nothing is submitted yet, so a reused spare would be overwritten under it).
    TEST_F(WriteRenameScenario, ThreeDrawsAroundTwoRenamesInOneFrameEachReadTheirOwnBytes) {
        if (!Ready()) return;
        std::vector<Vertex> v(4);
        PutQuad(v, 0, 0.0f, 0.0f, 0.33f, 1.0f, 255, 0, 0); // left third, red
        std::string error;
        Scene s = MakeScene(v, &error);
        ASSERT_NE(s.program, 0u) << error;

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        std::vector<Vertex> middle(4), right(4);
        PutQuad(middle, 0, 0.33f, 0.0f, 0.66f, 1.0f, 0, 255, 0);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(middle.size() * sizeof(Vertex)), middle.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        PutQuad(right, 0, 0.66f, 0.0f, 1.0f, 1.0f, 0, 0, 255);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(right.size() * sizeof(Vertex)), right.data());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 6, kSize / 2), 255, 0, 0)) << "left: the first draw's bytes";
        EXPECT_TRUE(Near(At(kSize / 2, kSize / 2), 0, 255, 0)) << "middle: after the first rename";
        EXPECT_TRUE(Near(At(kSize * 5 / 6, kSize / 2), 0, 0, 255)) << "right: after the second rename";
        Destroy(s);
    }

    TEST_F(WriteRenameScenario, ARenamedStoreKeepsDrawingRightInLaterFrames) {
        if (!Ready()) return;
        std::vector<Vertex> v(4);
        PutQuad(v, 0, 0.0f, 0.0f, 0.5f, 1.0f, 255, 0, 0);
        std::string error;
        Scene s = MakeScene(v, &error);
        ASSERT_NE(s.program, 0u) << error;

        for (int frame = 0; frame < 6; ++frame) {
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
            glViewport(0, 0, kSize, kSize);
            glClear(GL_COLOR_BUFFER_BIT);
            // Each frame: draw the left quad, move it right, draw again - one rename per frame.
            std::vector<Vertex> left(4), right(4);
            PutQuad(left, 0, 0.0f, 0.0f, 0.5f, 1.0f, 255, 0, 0);
            PutQuad(right, 0, 0.5f, 0.0f, 1.0f, 1.0f, 0, 0, 255);
            glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(left.size() * sizeof(Vertex)), left.data());
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(right.size() * sizeof(Vertex)), right.data());
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "frame " << frame << " left";
            EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 0, 255)) << "frame " << frame << " right";
            Gl().EndFrame();
        }
        EXPECT_EQ(FirstGLError(), 0u);
        Destroy(s);
    }

} // namespace MGITest
