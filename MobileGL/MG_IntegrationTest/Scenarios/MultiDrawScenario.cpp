// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/MultiDrawScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario D - glMultiDrawElements(BaseVertex) against the draws it stands for.
//
// Neither entry point exists in OpenGL ES, so DirectGLES emulates both through a
// ladder of tiers (MG_Backend/DirectGLES/MultiDraw.cpp): a native
// glMultiDrawElementsBaseVertexEXT, synthesized indirect commands drawn one at a
// time or in one batch, a per-sub-draw replay, a CPU rewrite of the index stream,
// and a compute shader that flattens the whole batch into a single draw. They
// share nothing but their contract, which is the one thing asserted here:
//
//     a multi-draw must paint exactly what the unrolled single draws paint.
//
// The reference side never enters the emulation - it is a loop of
// glDrawElementsBaseVertex / glDrawElements - so a tier cannot make itself look
// right by breaking both sides the same way.
//
// The Minecraft retraces already cover the common shape (GL_UNSIGNED_INT indices
// in a bound element array buffer, small base vertices, GL_TRIANGLES) on every
// tier. What they contain none of, and what these cases are for, is the set of
// shapes where a tier has to decline or compensate rather than replay:
//
//   * narrow index types, where a rewritten stream has to widen (BYTE/SHORT);
//   * a base vertex past the index type's range, where folding it into the
//     indices at the source width silently wraps - GL adds base vertices at full
//     precision, so `ushort index 10 + baseVertex 70000` is vertex 70010 and not
//     vertex 4474;
//   * primitive restart, where a rewritten stream must carry the sentinel across
//     unrebased or the restart is lost and the strip welds shut;
//   * client-memory index arrays, which have no buffer for the indirect tiers to
//     address or for the compute tier to read;
//   * a strip mode, which the flattening tier must decline outright because
//     concatenation would weld one sub-draw's last primitive to the next
//     sub-draw's first.
//
// One process is one tier (MOBILEGL_ESPRYT_MULTIDRAW_MODE is read once at
// startup), so a single run exercises whichever tier this driver resolved to.
// Running the binary once per mode is what covers the ladder; each run is a
// complete, self-contained proof for the tier it landed on.

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

        constexpr const char* kVertexSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() {
    oColor = vec4(vColor, 1.0);
}
)";

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        // Four column quads spanning the viewport left to right, in four colours,
        // so a sub-draw that lands in the wrong place, draws the wrong vertices or
        // does not draw at all changes the picture rather than hiding inside it.
        constexpr int kColumns = 4;

        const Rgba8 kColumnColors[kColumns] = {
            {255, 0, 0, 255},
            {0, 255, 0, 255},
            {0, 0, 255, 255},
            {255, 255, 255, 255},
        };

        // `padVertices` leading dummies force every sub-draw to need its own base
        // vertex: without one applied, a draw reads the padding and paints black.
        std::vector<Vertex> ColumnVertices(int padVertices) {
            std::vector<Vertex> vertices(static_cast<std::size_t>(padVertices), Vertex{0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            for (int column = 0; column < kColumns; ++column) {
                const float x0 = -1.0f + 2.0f * static_cast<float>(column) / kColumns;
                const float x1 = -1.0f + 2.0f * static_cast<float>(column + 1) / kColumns;
                const Rgba8 color = kColumnColors[column];
                const float r = color.r / 255.0f;
                const float g = color.g / 255.0f;
                const float b = color.b / 255.0f;
                vertices.push_back({x0, -1.0f, r, g, b});
                vertices.push_back({x1, -1.0f, r, g, b});
                vertices.push_back({x1, 1.0f, r, g, b});
                vertices.push_back({x0, 1.0f, r, g, b});
            }
            return vertices;
        }

        // Every sub-draw uses the SAME six indices, 0..3 relative to its own quad;
        // only the base vertex tells the columns apart. That makes the base vertex
        // the load-bearing part of the batch.
        const std::uint32_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

        // One column, as a restart-separated pair of triangle strips. Two strips in
        // one sub-draw means the sentinel is genuinely interior: drop it and the two
        // halves weld into a single strip that paints across the gap between them.
        // Indices are relative to the sub-draw's own quad, like kQuadIndices.
        template <typename Index>
        std::vector<Index> RestartStripIndices(Index restartSentinel) {
            // 3,0,2,1 is the strip winding of the quad; splitting it around the
            // sentinel gives two degenerate-free halves that redraw the same area.
            return {Index{3}, Index{0}, Index{2}, restartSentinel, Index{0}, Index{2}, Index{1}};
        }

        class MultiDrawScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "program setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                ReleaseBuffers();
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // VAO + VBO, and an EBO only when `indexBytes` is non-null: a null one
            // leaves GL_ELEMENT_ARRAY_BUFFER unbound so the sub-draws address client
            // memory, which is the shape that forces the buffer-reading tiers out.
            void BuildScene(int padVertices, const void* indexBytes, std::size_t indexByteCount) {
                ReleaseBuffers();
                const std::vector<Vertex> vertices = ColumnVertices(padVertices);

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                             vertices.data(), GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      reinterpret_cast<const void*>(sizeof(float) * 2));

                if (indexBytes != nullptr) {
                    glGenBuffers(1, &m_ebo);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexByteCount), indexBytes,
                                 GL_STATIC_DRAW);
                }
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";
            }

            void ReleaseBuffers() {
                if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_ebo = 0;
                m_vbo = 0;
                m_vao = 0;
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_ebo = 0;
        };

        // Runs `draw`, reads the default framebuffer back and returns the image.
        template <typename DrawFn>
        Image RenderPass(GLuint program, GLuint vao, DrawFn&& draw) {
            BindDefaultFramebuffer();
            glViewport(0, 0, HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(program);
            glBindVertexArray(vao);
            draw();
            return ReadPixels(HeadlessGL::Get().Width(), HeadlessGL::Get().Height());
        }

        // The whole point of the file: two renderings of the same geometry, one
        // through the multi-draw emulation and one through the single-draw entry
        // points it stands for, must be identical to the byte.
        void ExpectSameImage(const Image& multiDraw, const Image& unrolled, const std::string& what) {
            ASSERT_FALSE(multiDraw.Empty()) << what << ": multi-draw readback was empty";
            ASSERT_FALSE(unrolled.Empty()) << what << ": reference readback was empty";
            EXPECT_EQ(multiDraw, unrolled)
                << what << ": glMultiDraw* painted something else than the draws it stands for ("
                << multiDraw.ByteDiffCount(unrolled) << " bytes differ; multi-draw quadrants "
                << multiDraw.QuadrantSignature() << ", unrolled quadrants " << unrolled.QuadrantSignature() << ")";
            // A pair of blank frames would satisfy the comparison above and prove
            // nothing at all - the failure mode a multi-draw path most often has is
            // drawing NOTHING (see the shipped glMultiDrawElementsBaseVertexEXT stub
            // that silently dropped every draw). Demand the columns really landed.
            EXPECT_NE(multiDraw.QuadrantSignature(), "black,black,black,black") << what << ": nothing was drawn at all";
        }

        // ---- GL_UNSIGNED_INT indices in a buffer, per-sub-draw base vertices ----

        TEST_F(MultiDrawScenario, BaseVertexBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 5; // odd, so nothing lines up by accident
            BuildScene(kPad, kQuadIndices, sizeof(kQuadIndices));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_INT, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "GL_UNSIGNED_INT indices, per-sub-draw base vertices");
        }

        // ---- glMultiDrawElements: no base vertices, distinct index offsets ----

        TEST_F(MultiDrawScenario, PlainBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            // No padding and no base vertices: each sub-draw reaches its own column
            // through its index offset instead.
            std::vector<std::uint32_t> indices;
            for (int column = 0; column < kColumns; ++column) {
                for (const std::uint32_t index : kQuadIndices) {
                    indices.push_back(index + static_cast<std::uint32_t>(column * 4));
                }
            }
            BuildScene(0, indices.data(), indices.size() * sizeof(std::uint32_t));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(i * 6 * sizeof(std::uint32_t)));
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElements(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets, kColumns);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElements(GL_TRIANGLES, counts[i], GL_UNSIGNED_INT, offsets[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "glMultiDrawElements with no base vertices");
        }

        // ---- narrow index types ----
        // A tier that rewrites the stream emits GL_UNSIGNED_INT whatever came in,
        // so these two say the widening reproduces the original draw exactly.

        TEST_F(MultiDrawScenario, UnsignedShortBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 3;
            std::uint16_t indices[6];
            for (int i = 0; i < 6; ++i)
                indices[i] = static_cast<std::uint16_t>(kQuadIndices[i]);
            BuildScene(kPad, indices, sizeof(indices));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_SHORT, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_SHORT, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "GL_UNSIGNED_SHORT indices");
        }

        TEST_F(MultiDrawScenario, UnsignedByteBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 3;
            std::uint8_t indices[6];
            for (int i = 0; i < 6; ++i)
                indices[i] = static_cast<std::uint8_t>(kQuadIndices[i]);
            // 24 bytes: a word multiple, which the compute tier needs of the source
            // buffer when the index type is narrower than a word.
            std::uint8_t padded[24] = {};
            for (int i = 0; i < 6; ++i)
                padded[i] = indices[i];
            BuildScene(kPad, padded, sizeof(padded));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_BYTE, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_BYTE, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "GL_UNSIGNED_BYTE indices");
        }

        // ---- a base vertex the index type cannot spell ----
        // GL adds the base vertex at full precision, so folding it into a
        // GL_UNSIGNED_SHORT index stream at the source width wraps and addresses the
        // wrong vertex. The columns here start past 65535, which no ushort index can
        // reach on its own.

        TEST_F(MultiDrawScenario, BaseVertexBeyondIndexTypeRangeMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 70000; // > 0xFFFF
            std::uint16_t indices[6];
            for (int i = 0; i < 6; ++i)
                indices[i] = static_cast<std::uint16_t>(kQuadIndices[i]);
            BuildScene(kPad, indices, sizeof(indices));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_SHORT, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_SHORT, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "base vertex past the GL_UNSIGNED_SHORT range");
        }

        // ---- client-memory index arrays ----
        // No element array buffer, so the indirect tiers have nothing to address and
        // the compute tier nothing to read; both must decline and hand the batch to
        // a tier that can replay it.

        TEST_F(MultiDrawScenario, ClientSideIndicesBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 5;
            BuildScene(kPad, nullptr, 0);

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = 6;
                offsets[i] = kQuadIndices;
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_INT, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "client-memory index arrays");
        }

        // ---- primitive restart inside a strip ----
        // Two things at once: a strip mode, which the flattening tier must decline
        // because concatenation would weld sub-draws together, and a restart
        // sentinel, which any tier that rewrites indices must carry across without
        // adding the base vertex to it.

        TEST_F(MultiDrawScenario, PrimitiveRestartStripBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 5;
            const std::vector<std::uint32_t> indices = RestartStripIndices<std::uint32_t>(0xFFFFFFFFu);
            BuildScene(kPad, indices.data(), indices.size() * sizeof(std::uint32_t));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = static_cast<GLsizei>(indices.size());
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLE_STRIP, counts, GL_UNSIGNED_INT, offsets, kColumns,
                                              baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, counts[i], GL_UNSIGNED_INT, offsets[i],
                                             baseVertices[i]);
                }
            });
            glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "GL_TRIANGLE_STRIP with primitive restart");
        }

        // Same, with GL_UNSIGNED_SHORT: the sentinel a rewritten stream has to
        // recognise is the index TYPE's all-ones value, not the rewritten stream's.
        TEST_F(MultiDrawScenario, PrimitiveRestartUnsignedShortBatchMatchesUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 5;
            const std::vector<std::uint16_t> indices = RestartStripIndices<std::uint16_t>(0xFFFFu);
            BuildScene(kPad, indices.data(), indices.size() * sizeof(std::uint16_t));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                counts[i] = static_cast<GLsizei>(indices.size());
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLE_STRIP, counts, GL_UNSIGNED_SHORT, offsets, kColumns,
                                              baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    glDrawElementsBaseVertex(GL_TRIANGLE_STRIP, counts[i], GL_UNSIGNED_SHORT, offsets[i],
                                             baseVertices[i]);
                }
            });
            glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "GL_TRIANGLE_STRIP with GL_UNSIGNED_SHORT primitive restart");
        }

        // ---- a batch with holes ----
        // Zero-count sub-draws draw nothing. The flattening tier's binary search
        // finds a sub-draw by prefix sum, and a zero-count entry repeats the
        // previous sum - so a search that resolves ties the other way would attribute
        // indices to the empty draw and paint the wrong column.

        TEST_F(MultiDrawScenario, ZeroCountSubDrawsMatchUnrolledDraws) {
            if (!Ready()) return;
            constexpr int kPad = 5;
            BuildScene(kPad, kQuadIndices, sizeof(kQuadIndices));

            GLsizei counts[kColumns];
            const void* offsets[kColumns];
            GLint baseVertices[kColumns];
            for (int i = 0; i < kColumns; ++i) {
                // Columns 1 and 2 are skipped, leaving the outer two painted.
                counts[i] = (i == 1 || i == 2) ? 0 : 6;
                offsets[i] = reinterpret_cast<const void*>(0);
                baseVertices[i] = kPad + i * 4;
            }

            const Image batched = RenderPass(m_program, m_vao, [&] {
                glMultiDrawElementsBaseVertex(GL_TRIANGLES, counts, GL_UNSIGNED_INT, offsets, kColumns, baseVertices);
            });
            const Image unrolled = RenderPass(m_program, m_vao, [&] {
                for (int i = 0; i < kColumns; ++i) {
                    if (counts[i] == 0) continue;
                    glDrawElementsBaseVertex(GL_TRIANGLES, counts[i], GL_UNSIGNED_INT, offsets[i], baseVertices[i]);
                }
            });
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ExpectSameImage(batched, unrolled, "a batch with zero-count sub-draws");
        }

        // The base-vertex family's argument checks (GL 4.6 core 10.3.9). These are what
        // KHR-GL4x.draw_elements_base_vertex_tests.invalid_* assert, and the reason the group sat
        // NotSupported for so long hid the fact that the entry points forwarded any argument
        // straight to the backend: a negative count reached the emulation as a huge unsigned
        // size. Each case drains the error queue first so the assertion names the call it made.
        TEST_F(MultiDrawScenario, BaseVertexDrawsRejectMalformedArguments) {
            if (!Ready()) return;
            constexpr int kPad = 0;
            BuildScene(kPad, kQuadIndices, sizeof(kQuadIndices));
            // A bound program and VAO are prerequisites, not decoration: the entry points check
            // "is there something to execute" (GL_INVALID_OPERATION) before they look at any
            // argument, so without these every case below would pass for the wrong reason.
            glUseProgram(m_program);
            glBindVertexArray(m_vao);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "scene setup left a GL error behind";

            const auto expectError = [&](const char* what, GLenum expected) {
                EXPECT_EQ(FirstGLError(), expected) << what;
                // FirstGLError stops at the first one; make sure nothing else is queued so the
                // next case starts clean.
                while (glGetError() != GL_NO_ERROR) {
                }
            };

            glDrawElementsBaseVertex(GL_TRIANGLES, -1, GL_UNSIGNED_INT, nullptr, 0);
            expectError("glDrawElementsBaseVertex with a negative count", GL_INVALID_VALUE);

            glDrawElementsBaseVertex(GL_TRIANGLES, 3, GL_NONE, nullptr, 0);
            expectError("glDrawElementsBaseVertex with a non-index type", GL_INVALID_ENUM);

            glDrawRangeElementsBaseVertex(GL_TRIANGLES, 3, 0, 3, GL_UNSIGNED_INT, nullptr, 0);
            expectError("glDrawRangeElementsBaseVertex with end < start", GL_INVALID_VALUE);

            // start = -1 arrives as 0xFFFFFFFF, so this is the same end < start rule seen from
            // the other side - and it is the shape the CTS's invalid_count case actually uses.
            glDrawRangeElementsBaseVertex(GL_TRIANGLES, static_cast<GLuint>(-1), 2, 1, GL_UNSIGNED_INT, nullptr, 0);
            expectError("glDrawRangeElementsBaseVertex with a wrapped start", GL_INVALID_VALUE);

            glDrawElementsInstancedBaseVertex(GL_TRIANGLES, 3, GL_UNSIGNED_INT, nullptr, -1, 0);
            expectError("glDrawElementsInstancedBaseVertex with a negative instancecount", GL_INVALID_VALUE);

            const GLsizei negativeCount = -1;
            const void* offsets[1] = {reinterpret_cast<const void*>(0)};
            const GLint baseVertices[1] = {0};
            glMultiDrawElementsBaseVertex(GL_TRIANGLES, &negativeCount, GL_UNSIGNED_INT, offsets, 1, baseVertices);
            expectError("glMultiDrawElementsBaseVertex with a negative element of count", GL_INVALID_VALUE);

            const GLsizei validCount = 6;
            glMultiDrawElementsBaseVertex(GL_TRIANGLES, &validCount, GL_UNSIGNED_INT, offsets, -1, baseVertices);
            expectError("glMultiDrawElementsBaseVertex with a negative drawcount", GL_INVALID_VALUE);

            // The well-formed call still has to go through, or the checks above would be
            // indistinguishable from a blanket rejection.
            glMultiDrawElementsBaseVertex(GL_TRIANGLES, &validCount, GL_UNSIGNED_INT, offsets, 1, baseVertices);
            expectError("a well-formed glMultiDrawElementsBaseVertex", GL_NO_ERROR);
        }

    } // namespace
} // namespace MGITest
