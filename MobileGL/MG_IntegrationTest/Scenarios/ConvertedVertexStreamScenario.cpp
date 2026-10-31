// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ConvertedVertexStreamScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A VERTEX STREAM THE BACKEND CONVERTS ON THE CPU, REUSED AND RE-WRITTEN WITHIN A FRAME.
//
// P15 S0: DirectVulkan's wire arm converts a vertex stream whose layout the device cannot fetch
// (here a 16-bit attribute at an odd stride, which it repacks) once per (buffer content, layout)
// and reuses the converted copy for every draw that names it, frame after frame - a chunk
// renderer's multi-draws all name one arena. What that cache must get right:
//
//   * REUSE: two indexed draws in one frame over different base vertices of one converted stream
//     both read their own vertices out of the shared conversion.
//   * INVALIDATION: a glBufferSubData of the source between two draws in the SAME frame is seen by
//     the second draw (the store's content serial moved, so the cached conversion is not reused).
//   * ACROSS FRAMES: an unchanged stream drawn over more frames than there are frame slots keeps
//     drawing right, and a glBufferSubData between frames is seen by the next frame's draw.
//
// DirectGLES is the control; MOBILEGL_MAGMA_VERTEX_CONVERSION_CACHE=0 converts per draw.

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

        constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos * 2.0 - 1.0, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
uniform vec4 uColor;
out vec4 o0;
void main() { o0 = uColor; }
)";

        constexpr int kSize = 64;
        // 5-byte stride: the 4-byte (2 x GL_UNSIGNED_SHORT) attribute sits at odd offsets, which
        // no device fetches directly, so Magma repacks the stream on the CPU.
        constexpr int kStride = 5;

        class ConvertedVertexStreamScenario : public ScenarioTest {};

        // Writes vertex `index` of the interleaved stream: (x, y) in [0, 1] as normalized shorts.
        void PutVertex(std::vector<unsigned char>& bytes, int index, float x, float y) {
            const unsigned short sx = static_cast<unsigned short>(x * 65535.0f + 0.5f);
            const unsigned short sy = static_cast<unsigned short>(y * 65535.0f + 0.5f);
            std::memcpy(bytes.data() + index * kStride, &sx, 2);
            std::memcpy(bytes.data() + index * kStride + 2, &sy, 2);
        }

        // Four vertices of an axis-aligned quad as a triangle strip.
        void PutQuad(std::vector<unsigned char>& bytes, int first, float x0, float y0, float x1, float y1) {
            PutVertex(bytes, first + 0, x0, y0);
            PutVertex(bytes, first + 1, x1, y0);
            PutVertex(bytes, first + 2, x0, y1);
            PutVertex(bytes, first + 3, x1, y1);
        }

        ::testing::AssertionResult Near(const Rgba8& got, int r, int g, int b) {
            const auto close = [](int v, int want) { return v >= want - 3 && v <= want + 3; };
            if (close(got.r, r) && close(got.g, g) && close(got.b, b)) return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "got (" << int(got.r) << ", " << int(got.g) << ", " << int(got.b)
                                                 << "), want (" << r << ", " << g << ", " << b << ")";
        }

        struct Target {
            GLuint fbo = 0, color = 0;
        };

        Target MakeTarget() {
            Target t;
            glGenTextures(1, &t.color);
            glBindTexture(GL_TEXTURE_2D, t.color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
            glGenFramebuffers(1, &t.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
            glViewport(0, 0, kSize, kSize);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            return t;
        }

        Rgba8 At(int x, int y) {
            const Image image = ReadPixelsRect(x, y, 1, 1);
            return image.At(0, 0);
        }

    } // namespace

    TEST_F(ConvertedVertexStreamScenario, TwoIndexedDrawsShareOneConversionAndReadTheirOwnVertices) {
        if (!Ready()) return;
        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        Target target = MakeTarget();
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        std::vector<unsigned char> bytes(8 * kStride, 0);
        PutQuad(bytes, 0, 0.0f, 0.0f, 0.5f, 1.0f); // left half
        PutQuad(bytes, 4, 0.5f, 0.0f, 1.0f, 1.0f); // right half
        const unsigned short indices[] = {0, 1, 2, 3};
        GLuint vao = 0, vbo = 0, ibo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_UNSIGNED_SHORT, GL_TRUE, kStride, nullptr);
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glUseProgram(program);
        const GLint color = glGetUniformLocation(program, "uColor");
        glUniform4f(color, 1.0f, 0.0f, 0.0f, 1.0f);
        glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr, 0);
        glUniform4f(color, 0.0f, 1.0f, 0.0f, 1.0f);
        glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr, 4);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "left half: the first draw's vertices";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 255, 0)) << "right half: base vertex 4 of the same stream";

        glDeleteBuffers(1, &ibo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &target.fbo);
        glDeleteTextures(1, &target.color);
        glDeleteProgram(program);
    }

    TEST_F(ConvertedVertexStreamScenario, ASubDataBetweenTwoDrawsInOneFrameIsSeenByTheSecond) {
        if (!Ready()) return;
        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        Target target = MakeTarget();
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        std::vector<unsigned char> bytes(4 * kStride, 0);
        PutQuad(bytes, 0, 0.0f, 0.0f, 0.5f, 1.0f); // left half
        const unsigned short indices[] = {0, 1, 2, 3};
        GLuint vao = 0, vbo = 0, ibo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_UNSIGNED_SHORT, GL_TRUE, kStride, nullptr);
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glUseProgram(program);
        const GLint color = glGetUniformLocation(program, "uColor");
        glUniform4f(color, 1.0f, 0.0f, 0.0f, 1.0f);
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr);
        // Same buffer, same layout, new bytes: the quad moves to the right half.
        PutQuad(bytes, 0, 0.5f, 0.0f, 1.0f, 1.0f);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes.size()), bytes.data());
        glUniform4f(color, 0.0f, 0.0f, 1.0f, 1.0f);
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, nullptr);
        EXPECT_EQ(FirstGLError(), 0u);

        EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "left half: the draw before the sub-data";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 0, 255))
            << "right half: the draw after the sub-data must read the new bytes, not the cached conversion";

        glDeleteBuffers(1, &ibo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &target.fbo);
        glDeleteTextures(1, &target.color);
        glDeleteProgram(program);
    }

    TEST_F(ConvertedVertexStreamScenario, AConversionKeptAcrossFramesIsReplacedByASubDataBetweenFrames) {
        if (!Ready()) return;
        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;
        Target target = MakeTarget();
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        std::vector<unsigned char> bytes(4 * kStride, 0);
        PutQuad(bytes, 0, 0.0f, 0.0f, 0.5f, 1.0f); // left half
        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_UNSIGNED_SHORT, GL_TRUE, kStride, nullptr);
        glUseProgram(program);
        const GLint color = glGetUniformLocation(program, "uColor");

        // Several frames - more than any frame-slot count - drawing the one unchanged stream: every
        // frame after the first reuses the conversion the first one made.
        for (int frame = 0; frame < 5; ++frame) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glViewport(0, 0, kSize, kSize);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform4f(color, 1.0f, static_cast<float>(frame) / 4.0f, 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, (frame * 255 + 2) / 4, 0)) << "frame " << frame;
            EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 0, 0)) << "frame " << frame;
            Gl().EndFrame();
        }
        // New bytes between frames: the next frame's draw must not reuse the kept conversion.
        PutQuad(bytes, 0, 0.5f, 0.0f, 1.0f, 1.0f);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes.size()), bytes.data());
        for (int frame = 0; frame < 3; ++frame) {
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glViewport(0, 0, kSize, kSize);
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform4f(color, 0.0f, 0.0f, 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 0, 0, 0))
                << "frame " << frame << " after the sub-data: the old conversion was reused";
            EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 0, 255)) << "frame " << frame << " after the sub-data";
            Gl().EndFrame();
        }
        EXPECT_EQ(FirstGLError(), 0u);

        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &target.fbo);
        glDeleteTextures(1, &target.color);
        glDeleteProgram(program);
    }

} // namespace MGITest
