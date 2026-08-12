// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DrawParametersScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// gl_BaseVertex / gl_BaseInstance / gl_DrawID (GL_ARB_shader_draw_parameters),
// read straight out of the shader that a draw command produced.
//
// Neither backend has these builtins for free, and each is wrong in its own way
// when nobody watches:
//
//   * DirectVulkan HAS a BaseVertex builtin, but Vulkan's carries the draw's
//     firstVertex on a NON-INDEXED draw where GL's is defined to be zero ("the
//     value passed to the baseVertex parameter, or zero for a command with no
//     such parameter"). Only the indexed meaning of the two agrees. Every
//     DrawArrays form therefore takes the ZeroBaseVertex program variant.
//   * DirectGLES has no such builtins at all: ESSL knows none of them, so the
//     transpiler demotes each one to a uniform the draw paths feed. A uniform
//     nobody writes keeps whatever the previous draw left in it - which is what
//     made gl_BaseVertex report a stale base vertex, and what made
//     gl_BaseInstance read an unbound storage buffer on a plain glDrawArrays.
//
// The shader paints the three values, so a draw that carries the wrong ones
// paints the wrong colour rather than merely disagreeing with an expectation
// somewhere. The framebuffer is cleared to WHITE and no case expects 255 in any
// channel, so "the draw did not happen" can never be mistaken for a pass.

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
#include <GL/glext.h>

namespace MGITest {
    namespace {

        // #version 450: glslang only declares the ARB builtins from 440 up.
        //
        // Each value is painted at 8 units per count, not 1: the errors these builtins
        // actually have are OFF BY ONE (a sub-draw that never got its own gl_DrawID reads
        // the previous one's, a base vertex that arrives one command late), and at one unit
        // per count no readback tolerance can tell those from rounding.
        //
        // And biased by two counts, so that ZERO is not the clamp floor. Five of these cases
        // expect zero, and an unbiased encoding would let every negative value - the shape a
        // sign or rebase mistake produces - clamp to the same black and pass.
        constexpr const char* kVertexSource = R"(#version 450 core
#extension GL_ARB_shader_draw_parameters : require
layout(location = 0) in vec2 aPos;
flat out vec3 vParams;
void main() {
    vParams = (vec3(gl_BaseVertexARB, gl_BaseInstanceARB, gl_DrawIDARB) * 8.0 + 16.0) / 255.0;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 450 core
flat in vec3 vParams;
out vec4 oColor;
void main() {
    oColor = vec4(vParams, 1.0);
}
)";

        struct Vertex {
            float x, y;
        };

        // 3 dummy vertices, then the left half of the viewport as two triangles,
        // then the right half. Nothing here is symmetric by accident:
        //
        //   * the padding makes a draw that ignores `first` / baseVertex paint a
        //     degenerate triangle (i.e. nothing) instead of the right picture;
        //   * the two halves let one multi-draw show TWO different gl_DrawID
        //     values in one readback.
        //
        // Indices 3..14 together cover the whole viewport, which is what the
        // single-draw cases use.
        constexpr int kPad = 3;
        constexpr int kLeftFirst = kPad;      // 3
        constexpr int kRightFirst = kPad + 6; // 9
        constexpr int kHalfCount = 6;

        std::vector<Vertex> SceneVertices() {
            std::vector<Vertex> vertices(static_cast<std::size_t>(kPad), Vertex{0.0f, 0.0f});
            const float bounds[2][2] = {{-1.0f, 0.0f}, {0.0f, 1.0f}};
            for (const auto& half : bounds) {
                const float x0 = half[0];
                const float x1 = half[1];
                vertices.push_back({x0, -1.0f});
                vertices.push_back({x1, -1.0f});
                vertices.push_back({x1, 1.0f});
                vertices.push_back({x0, -1.0f});
                vertices.push_back({x1, 1.0f});
                vertices.push_back({x0, 1.0f});
            }
            return vertices;
        }

        // GL's DrawArraysIndirectCommand / DrawElementsIndirectCommand, spelled out
        // so a test can write one without depending on a GL header's struct.
        struct ArraysCommand {
            std::uint32_t count, instanceCount, first, baseInstance;
        };
        struct ElementsCommand {
            std::uint32_t count, instanceCount, firstIndex;
            std::int32_t baseVertex;
            std::uint32_t baseInstance;
        };

        class DrawParametersScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                const std::vector<Vertex> vertices = SceneVertices();
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                             vertices.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(0));
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                for (GLuint* buffer : {&m_ebo, &m_indirect, &m_parameter, &m_vbo}) {
                    if (*buffer != 0) glDeleteBuffers(1, buffer);
                    *buffer = 0;
                }
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            template <typename T>
            void FillBuffer(GLuint& name, GLenum target, const std::vector<T>& data) {
                if (name == 0) glGenBuffers(1, &name);
                glBindBuffer(target, name);
                glBufferData(target, static_cast<GLsizeiptr>(data.size() * sizeof(T)), data.data(), GL_STATIC_DRAW);
            }

            // Clears to white, runs `draw` and reads the frame back.
            template <typename DrawFn>
            Image Render(DrawFn&& draw) {
                BindDefaultFramebuffer();
                glViewport(0, 0, HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
                ClearTo(1.0f, 1.0f, 1.0f, 1.0f);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                draw();
                return ReadPixels(HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
            }

            // The three builtins as the shader saw them, at a point in one half of
            // the viewport. `half` is 0 for the left half and 1 for the right.
            struct DrawParams {
                int baseVertex = -1, baseInstance = -1, drawId = -1;
            };
            // Decodes the biased 8-units-per-count encoding back to the integer the
            // shader saw. Rounding to the nearest step absorbs any UNORM slop; adjacent
            // values stay eight units apart, so an off-by-one still reads as one, and a
            // negative value lands below the bias and decodes negative rather than
            // clamping into a legitimate zero.
            static DrawParams ParamsAt(const Image& image, int half) {
                const int x = image.Width() * (1 + 2 * half) / 4;
                const Rgba8 pixel = image.At(x, image.Height() / 2);
                const auto decode = [](std::uint8_t channel) {
                    return (static_cast<int>(channel) - 16 + 4) / 8;
                };
                return {decode(pixel.r), decode(pixel.g), decode(pixel.b)};
            }

            static void ExpectParams(const Image& image, int half, const DrawParams& expected,
                                     const std::string& what) {
                const DrawParams actual = ParamsAt(image, half);
                EXPECT_EQ(actual.baseVertex, expected.baseVertex)
                    << what << ": gl_BaseVertex (half " << half << ")";
                EXPECT_EQ(actual.baseInstance, expected.baseInstance)
                    << what << ": gl_BaseInstance (half " << half << ")";
                EXPECT_EQ(actual.drawId, expected.drawId) << what << ": gl_DrawID (half " << half << ")";
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_ebo = 0;
            GLuint m_indirect = 0;
            GLuint m_parameter = 0;
        };

        // ---- the non-indexed forms: gl_BaseVertex is zero, `first` or not ----

        // Vulkan's BaseVertex would answer 3 here (the draw's firstVertex); GL's
        // must answer 0, because glDrawArrays has no baseVertex parameter at all.
        TEST_F(DrawParametersScenario, DrawArraysReportsAZeroBaseVertexDespiteItsFirst) {
            if (!Ready()) return;
            const Image image = Render([&] { glDrawArrays(GL_TRIANGLES, kLeftFirst, 2 * kHalfCount); });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 0, 0}, "glDrawArrays(first=3)");
            ExpectParams(image, 1, {0, 0, 0}, "glDrawArrays(first=3)");
        }

        TEST_F(DrawParametersScenario, DrawArraysInstancedBaseInstanceReportsItsBaseInstance) {
            if (!Ready()) return;
            const Image image = Render([&] {
                glDrawArraysInstancedBaseInstance(GL_TRIANGLES, kLeftFirst, 2 * kHalfCount, 1, 5);
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 5, 0}, "glDrawArraysInstancedBaseInstance(baseInstance=5)");
        }

        // The base instance of one draw must not survive into the next one. This is
        // the shape that broke on DirectGLES: the emulation uniform is per-program
        // state, so a draw that never writes it inherits the last writer's value.
        TEST_F(DrawParametersScenario, APlainDrawAfterABaseInstancedOneSeesZeroAgain) {
            if (!Ready()) return;
            const Image image = Render([&] {
                glDrawArraysInstancedBaseInstance(GL_TRIANGLES, kLeftFirst, 2 * kHalfCount, 1, 7);
                glDrawArrays(GL_TRIANGLES, kLeftFirst, 2 * kHalfCount);
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 0, 0}, "plain glDrawArrays after a base-instanced draw");
        }

        // ---- the indexed forms: gl_BaseVertex IS the base vertex ----

        TEST_F(DrawParametersScenario, DrawElementsBaseVertexReportsItsBaseVertex) {
            if (!Ready()) return;
            std::vector<std::uint32_t> indices;
            for (std::uint32_t i = 0; i < 2 * kHalfCount; ++i) indices.push_back(i);
            FillBuffer(m_ebo, GL_ELEMENT_ARRAY_BUFFER, indices);

            const Image image = Render([&] {
                glDrawElementsBaseVertex(GL_TRIANGLES, 2 * kHalfCount, GL_UNSIGNED_INT,
                                         reinterpret_cast<const void*>(0), kLeftFirst);
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {kLeftFirst, 0, 0}, "glDrawElementsBaseVertex(basevertex=3)");
            ExpectParams(image, 1, {kLeftFirst, 0, 0}, "glDrawElementsBaseVertex(basevertex=3)");
        }

        // ... and is zero again for the command that has none, including after one
        // that did: the same leak the base instance has, on the other builtin. The
        // preceding draw MUST carry a non-zero base vertex or this case proves nothing -
        // one index run reaches the geometry through the base vertex, the second through
        // its own indices, so the two draws paint the same picture with different
        // gl_BaseVertex and only the second one's value survives in the framebuffer.
        TEST_F(DrawParametersScenario, DrawElementsAfterABaseVertexDrawReportsZeroAgain) {
            if (!Ready()) return;
            std::vector<std::uint32_t> indices;
            for (std::uint32_t i = 0; i < 2 * kHalfCount; ++i) indices.push_back(i);
            for (std::uint32_t i = 0; i < 2 * kHalfCount; ++i) indices.push_back(i + kLeftFirst);
            FillBuffer(m_ebo, GL_ELEMENT_ARRAY_BUFFER, indices);
            const auto rebasedRun = reinterpret_cast<const void*>(2 * kHalfCount * sizeof(std::uint32_t));

            const Image image = Render([&] {
                glDrawElementsBaseVertex(GL_TRIANGLES, 2 * kHalfCount, GL_UNSIGNED_INT,
                                         reinterpret_cast<const void*>(0), kLeftFirst);
                glDrawElements(GL_TRIANGLES, 2 * kHalfCount, GL_UNSIGNED_INT, rebasedRun);
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 0, 0}, "glDrawElements after a base-vertex draw");
            ExpectParams(image, 1, {0, 0, 0}, "glDrawElements after a base-vertex draw");
        }

        // ---- the multi-draw forms: one gl_DrawID per sub-draw ----

        TEST_F(DrawParametersScenario, MultiDrawArraysNumbersItsSubDraws) {
            if (!Ready()) return;
            const GLint firsts[2] = {kLeftFirst, kRightFirst};
            const GLsizei counts[2] = {kHalfCount, kHalfCount};

            const Image image = Render([&] { glMultiDrawArrays(GL_TRIANGLES, firsts, counts, 2); });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 0, 0}, "glMultiDrawArrays sub-draw 0");
            ExpectParams(image, 1, {0, 0, 1}, "glMultiDrawArrays sub-draw 1");
        }

        // Every field of an indexed indirect command at once: its own gl_DrawID, the
        // baseVertex word (which the CPU reads out of the command) and the
        // baseInstance word (which DirectGLES reads through a storage-buffer view of
        // the very same buffer).
        TEST_F(DrawParametersScenario, MultiDrawElementsIndirectCarriesEveryCommandsParameters) {
            if (!Ready()) return;
            std::vector<std::uint32_t> indices;
            for (std::uint32_t i = 0; i < kHalfCount; ++i) indices.push_back(i);
            FillBuffer(m_ebo, GL_ELEMENT_ARRAY_BUFFER, indices);

            const std::vector<ElementsCommand> commands = {
                {kHalfCount, 1, 0, kLeftFirst, 0},
                {kHalfCount, 1, 0, kRightFirst, 4},
            };
            FillBuffer(m_indirect, GL_DRAW_INDIRECT_BUFFER, commands);

            const Image image = Render([&] {
                glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(0), 2,
                                            sizeof(ElementsCommand));
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {kLeftFirst, 0, 0}, "indirect command 0");
            ExpectParams(image, 1, {kRightFirst, 4, 1}, "indirect command 1");
        }

        // glMultiDrawArraysIndirectCount was missing from the DirectGLES backend
        // table entirely, so the frontend answered INVALID_OPERATION for every call
        // while GL_ARB_indirect_parameters was advertised. The parameter buffer here
        // holds a count SMALLER than maxdrawcount, so a path that ignores it draws a
        // third command over the top of the second and changes the right half.
        TEST_F(DrawParametersScenario, MultiDrawArraysIndirectCountObeysItsParameterBuffer) {
            if (!Ready()) return;
            const std::vector<ArraysCommand> commands = {
                {kHalfCount, 1, kLeftFirst, 0},
                {kHalfCount, 1, kRightFirst, 6},
                {kHalfCount, 1, kRightFirst, 9},
            };
            FillBuffer(m_indirect, GL_DRAW_INDIRECT_BUFFER, commands);
            const std::vector<std::uint32_t> parameters = {2};
            FillBuffer(m_parameter, GL_PARAMETER_BUFFER, parameters);

            const Image image = Render([&] {
                glMultiDrawArraysIndirectCount(GL_TRIANGLES, reinterpret_cast<const void*>(0), 0, 3,
                                               sizeof(ArraysCommand));
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectParams(image, 0, {0, 0, 0}, "counted indirect command 0");
            ExpectParams(image, 1, {0, 6, 1}, "counted indirect command 1");
        }

    } // namespace
} // namespace MGITest
