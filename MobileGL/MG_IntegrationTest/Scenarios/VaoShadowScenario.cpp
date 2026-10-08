// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/VaoShadowScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - ONE VAO WHOSE ATTRIBUTE STATE CHANGES BETWEEN DRAWS.
//
// P15: DirectGLES keeps a shadow of what each driver VAO holds (enable flags, divisors, pointers
// with the buffer they captured) and emits only differences. What the shadow must never do is
// skip a change the driver VAO needs:
//
//   * an attribute disabled between two draws reads its current value in the second;
//   * the same layout pointed at another buffer reads that buffer;
//   * a deleted buffer replaced by a new one (its driver id may be re-used) is read anew;
//   * a divisor changed between two instanced draws takes effect.
//
// DirectVulkan is the control; MOBILEGL_ESPRYT_VAO_SHADOW=0 re-emits everything.

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

        // Position in [0, 1] (+ an x offset per instance), colour from attribute 1.
        constexpr const char* kVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4((aPos + vec2(float(gl_InstanceID) * 0.5, 0.0)) * 2.0 - 1.0, 0.0, 1.0);
}
)";

        constexpr const char* kFS = R"(#version 330 core
in vec4 vColor;
out vec4 o0;
void main() { o0 = vColor; }
)";

        constexpr int kSize = 64;

        class VaoShadowScenario : public ScenarioTest {};

        ::testing::AssertionResult Near(const Rgba8& got, int r, int g, int b) {
            const auto close = [](int v, int want) { return v >= want - 3 && v <= want + 3; };
            if (close(got.r, r) && close(got.g, g) && close(got.b, b)) return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "got (" << int(got.r) << ", " << int(got.g) << ", " << int(got.b)
                                                 << "), want (" << r << ", " << g << ", " << b << ")";
        }

        Rgba8 At(int x, int y) { return ReadPixelsRect(x, y, 1, 1).At(0, 0); }

        // Quad positions (x0..x1 by full height) as a triangle strip.
        std::vector<float> Quad(float x0, float x1) { return {x0, 0.0f, x1, 0.0f, x0, 1.0f, x1, 1.0f}; }
        std::vector<float> Colors(float r, float g, float b) {
            std::vector<float> c;
            for (int i = 0; i < 4; ++i) c.insert(c.end(), {r, g, b, 1.0f});
            return c;
        }

        struct Scene {
            GLuint program = 0, fbo = 0, color = 0, vao = 0;
        };

        Scene MakeScene(std::string* error) {
            Scene s;
            s.program = CompileProgram(kVS, kFS, error);
            if (s.program == 0) return s;
            glGenTextures(1, &s.color);
            glBindTexture(GL_TEXTURE_2D, s.color);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
            glGenFramebuffers(1, &s.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.color, 0);
            glViewport(0, 0, kSize, kSize);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glGenVertexArrays(1, &s.vao);
            glBindVertexArray(s.vao);
            glUseProgram(s.program);
            return s;
        }

        GLuint Buffer(const std::vector<float>& data) {
            GLuint b = 0;
            glGenBuffers(1, &b);
            glBindBuffer(GL_ARRAY_BUFFER, b);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
            return b;
        }

        void Destroy(Scene& s) {
            glDeleteVertexArrays(1, &s.vao);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &s.fbo);
            glDeleteTextures(1, &s.color);
            glDeleteProgram(s.program);
        }

    } // namespace

    TEST_F(VaoShadowScenario, AnAttributeDisabledBetweenDrawsReadsItsCurrentValue) {
        if (!Ready()) return;
        std::string error;
        Scene s = MakeScene(&error);
        ASSERT_NE(s.program, 0u) << error;
        const GLuint left = Buffer(Quad(0.0f, 0.5f)), right = Buffer(Quad(0.5f, 1.0f));
        const GLuint red = Buffer(Colors(1.0f, 0.0f, 0.0f));
        glBindBuffer(GL_ARRAY_BUFFER, red);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, left);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // left: red from the buffer
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 0.0f, 0.0f, 1.0f, 1.0f);
        glBindBuffer(GL_ARRAY_BUFFER, right);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // right: the current value, blue
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(At(kSize / 4, kSize / 2), 255, 0, 0)) << "left: attribute 1 enabled";
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 0, 255)) << "right: attribute 1 disabled";
        // And back on: the driver VAO must be re-enabled, not left as the shadow last saw it.
        glClear(GL_COLOR_BUFFER_BIT);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 255, 0, 0)) << "right: attribute 1 enabled again";
        GLuint bufs[] = {left, right, red};
        glDeleteBuffers(3, bufs);
        Destroy(s);
    }

    TEST_F(VaoShadowScenario, TheSameLayoutOnAnotherBufferReadsThatBuffer) {
        if (!Ready()) return;
        std::string error;
        Scene s = MakeScene(&error);
        ASSERT_NE(s.program, 0u) << error;
        const GLuint pos = Buffer(Quad(0.0f, 1.0f));
        const GLuint red = Buffer(Colors(1.0f, 0.0f, 0.0f)), green = Buffer(Colors(0.0f, 1.0f, 0.0f));
        glBindBuffer(GL_ARRAY_BUFFER, pos);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, red);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_TRUE(Near(At(kSize / 2, kSize / 2), 255, 0, 0)) << "first buffer";
        // Identical pointer parameters, other buffer.
        glBindBuffer(GL_ARRAY_BUFFER, green);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(At(kSize / 2, kSize / 2), 0, 255, 0)) << "same layout, second buffer";
        GLuint bufs[] = {pos, red, green};
        glDeleteBuffers(3, bufs);
        Destroy(s);
    }

    TEST_F(VaoShadowScenario, ADeletedBufferReplacedByANewOneIsReadAnew) {
        if (!Ready()) return;
        std::string error;
        Scene s = MakeScene(&error);
        ASSERT_NE(s.program, 0u) << error;
        const GLuint pos = Buffer(Quad(0.0f, 1.0f));
        glBindBuffer(GL_ARRAY_BUFFER, pos);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        GLuint first = Buffer(Colors(1.0f, 0.0f, 0.0f));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_TRUE(Near(At(kSize / 2, kSize / 2), 255, 0, 0)) << "the first buffer";
        // Delete it and make another (the driver may hand the same id back), same parameters.
        glDeleteBuffers(1, &first);
        const GLuint second = Buffer(Colors(0.0f, 0.0f, 1.0f));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(Near(At(kSize / 2, kSize / 2), 0, 0, 255)) << "the replacement buffer";
        GLuint bufs[] = {pos, second};
        glDeleteBuffers(2, bufs);
        Destroy(s);
    }

    TEST_F(VaoShadowScenario, ADivisorChangedBetweenInstancedDrawsTakesEffect) {
        if (!Ready()) return;
        std::string error;
        Scene s = MakeScene(&error);
        ASSERT_NE(s.program, 0u) << error;
        // Two instances of the left half (instance 1 lands on the right half).
        const GLuint pos = Buffer(Quad(0.0f, 0.5f));
        // Per-vertex: 4 x red. Per-instance (divisor 1): instance 0 red, instance 1 green.
        std::vector<float> c = Colors(1.0f, 0.0f, 0.0f);
        c[4] = 0.0f; c[5] = 1.0f; c[6] = 0.0f; // second element green
        const GLuint colors = Buffer(c);
        glBindBuffer(GL_ARRAY_BUFFER, pos);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, colors);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribDivisor(1, 1);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 2);
        EXPECT_TRUE(Near(At(kSize * 3 / 4, kSize / 2), 0, 255, 0)) << "divisor 1: instance 1 reads element 1";
        glClear(GL_COLOR_BUFFER_BIT);
        glVertexAttribDivisor(1, 0);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 2);
        EXPECT_EQ(FirstGLError(), 0u);
        // Divisor 0: per vertex - only vertex 1 (bottom-right of each quad) is green, so the right
        // quad's top-left corner is almost pure red (a divisor still at 1 would make it pure green).
        const Rgba8 corner = At(kSize / 2 + 2, kSize - 2);
        EXPECT_TRUE(corner.r > 200 && corner.g < 40)
            << "divisor 0: the right quad's top-left reads per-vertex red, got (" << int(corner.r) << ", "
            << int(corner.g) << ", " << int(corner.b) << ")";
        GLuint bufs[] = {pos, colors};
        glDeleteBuffers(2, bufs);
        Destroy(s);
    }

} // namespace MGITest
