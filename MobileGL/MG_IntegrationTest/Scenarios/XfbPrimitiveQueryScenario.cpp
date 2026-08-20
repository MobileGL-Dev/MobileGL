// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/XfbPrimitiveQueryScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// What the two transform feedback queries report for a VERTEX-ONLY capture that
// OVERFLOWS its buffer - the shape of KHR-GL30.transform_feedback.query_vertex_*,
// and the one place where the two targets must disagree:
//
//   * GL_PRIMITIVES_GENERATED counts what the capture stage assembled: 4 points.
//   * GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN counts what the capture buffers
//     took. With room for three vertices, a full buffer stops recording whole
//     primitives (GL 4.6 core 13.2.2), so the answer is 3, not 4 and not 6.
//
// Both numbers came from the backend's own GPU counter until the driver underneath
// DirectGLES was caught reporting exactly twice the written count for this shape
// (Adreno 830, vertex-only capture issued right after a large render pass). The
// frontend already computes the desktop-exact number for a capture with no geometry
// stage, so that is what answers PRIMITIVES_WRITTEN there now - and this scenario is
// what pins the value, on every backend, without a device.
//
// The non-overflowing case is the negative control: with room for all four points
// the two targets must AGREE at 4, so a "written" that silently reports the
// generated count cannot pass both cases at once.

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

        constexpr float kPoison = -1234.0f;
        // One vec4 per captured point.
        constexpr std::size_t kFloatsPerVertex = 4;
        constexpr std::size_t kBytesPerVertex = kFloatsPerVertex * sizeof(float);
        // The draw: four points, whichever way the capture buffer is sized.
        constexpr GLsizei kDrawnPoints = 4;

        GLuint CompileShader(GLenum type, const std::string& source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            const char* text = source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetShaderInfoLog(shader, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        // Vertex-only capture program - no geometry stage, so nothing amplifies and the
        // primitives written are the primitives drawn (up to the buffer's capacity).
        GLuint BuildCaptureProgram(std::string* log) {
            const std::string vertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_value;
out vec4 vs_out_value;
void main() {
  vs_out_value = vs_in_value;
}
)";
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            const char* varying = "vs_out_value";
            glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        class XfbPrimitiveQueryScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                std::string log;
                m_program = BuildCaptureProgram(&log);
                ASSERT_NE(m_program, 0u) << "capture program failed to build: " << log;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                // Vertex i is (i, i+1, i+2, i+3), so a record that landed in the wrong slot
                // is as visible as one that never landed at all.
                float vertices[kDrawnPoints * kFloatsPerVertex] = {};
                for (int point = 0; point < kDrawnPoints; ++point) {
                    for (std::size_t component = 0; component < kFloatsPerVertex; ++component) {
                        vertices[static_cast<std::size_t>(point) * kFloatsPerVertex + component] =
                            static_cast<float>(point) + static_cast<float>(component);
                    }
                }
                glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);

                glGenQueries(2, m_queries);
                ASSERT_NE(m_queries[0], 0u);
                ASSERT_NE(m_queries[1], 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                glDeleteQueries(2, m_queries);
                glBindVertexArray(0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                glUseProgram(0);
                ScenarioTest::TearDown();
            }

            // A capture buffer with room for exactly `vertexCapacity` records, poisoned so
            // that "captured nothing" is legible, bound to capture point 0.
            GLuint MakeCaptureBuffer(std::size_t vertexCapacity) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer);
                const std::vector<float> poison(vertexCapacity * kFloatsPerVertex, kPoison);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,
                             static_cast<GLsizeiptr>(vertexCapacity * kBytesPerVertex), poison.data(),
                             GL_DYNAMIC_DRAW);
                return buffer;
            }

            // ONE capture span, four points, with both query targets open across it - the
            // order KHR-GL30.transform_feedback.query_vertex_interleaved_test uses: the
            // queries wrap the whole span, never the other way round.
            void RunQueriedSpan(GLuint* written, GLuint* generated) {
                glEnable(GL_RASTERIZER_DISCARD);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);

                glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, m_queries[0]);
                glBeginQuery(GL_PRIMITIVES_GENERATED, m_queries[1]);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(GL_POINTS, 0, kDrawnPoints);
                glEndTransformFeedback();
                glEndQuery(GL_PRIMITIVES_GENERATED);
                glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);

                glDisable(GL_RASTERIZER_DISCARD);
                glUseProgram(0);

                *written = 0xFFFFFFFFu;
                *generated = 0xFFFFFFFFu;
                glGetQueryObjectuiv(m_queries[0], GL_QUERY_RESULT, written);
                glGetQueryObjectuiv(m_queries[1], GL_QUERY_RESULT, generated);
            }

            // The capture record at slot `point` must be the vertex the draw fetched there.
            static ::testing::AssertionResult CapturedVertexIs(const float* record, int point) {
                for (std::size_t component = 0; component < kFloatsPerVertex; ++component) {
                    const float expected = static_cast<float>(point) + static_cast<float>(component);
                    const float got = record[component];
                    // isfinite first: every ordered comparison against a NaN is false, so a
                    // pair of one-sided range tests REPORTS SUCCESS for uninitialised storage
                    // that happens to read as NaN.
                    if (!std::isfinite(got) || std::fabs(got - expected) > 0.01f) {
                        return ::testing::AssertionFailure()
                               << "point " << point << " component " << component << " is " << got << ", expected "
                               << expected << (got == kPoison ? " (the capture never reached these bytes)" : "");
                    }
                }
                return ::testing::AssertionSuccess();
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_queries[2] = {0, 0};
        };

        // The negative control: the buffer holds every point the draw produces, so both
        // targets must report the same 4. A "written" that is really the generated count
        // passes this case and fails the next one; a "written" that is really zero fails
        // this one.
        TEST_F(XfbPrimitiveQueryScenario, ACaptureThatFitsReportsEveryPrimitiveOnBothTargets) {
            if (!Ready()) GTEST_SKIP();

            const GLuint captureBuffer = MakeCaptureBuffer(kDrawnPoints);
            GLuint written = 0;
            GLuint generated = 0;
            RunQueriedSpan(&written, &generated);

            EXPECT_EQ(written, 4u);
            EXPECT_EQ(generated, 4u);

            std::vector<float> readback(kDrawnPoints * kFloatsPerVertex, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               static_cast<GLsizeiptr>(kDrawnPoints * kBytesPerVertex), readback.data());
            for (int point = 0; point < kDrawnPoints; ++point) {
                EXPECT_TRUE(CapturedVertexIs(readback.data() + static_cast<std::size_t>(point) * kFloatsPerVertex,
                                             point));
            }

            glDeleteBuffers(1, &captureBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The pin: four points into a buffer sized for three. The fourth is not written, so
        // the two targets part ways at 3 and 4 - the exact pair
        // KHR-GL30.transform_feedback.query_vertex_interleaved_test checks, and the pair the
        // Adreno driver counter got wrong (it answered 6).
        TEST_F(XfbPrimitiveQueryScenario, AnOverflowingVertexOnlyCaptureStopsWritingAtTheBufferCapacity) {
            if (!Ready()) GTEST_SKIP();

            constexpr std::size_t kCapacityVertices = 3;
            const GLuint captureBuffer = MakeCaptureBuffer(kCapacityVertices);
            GLuint written = 0;
            GLuint generated = 0;
            RunQueriedSpan(&written, &generated);

            EXPECT_EQ(written, 3u) << "the capture buffer holds " << kCapacityVertices << " points";
            EXPECT_EQ(generated, 4u) << "every point the draw assembled is generated, capacity or not";

            // The three records that DID fit are the first three points, in order: an
            // overflow truncates the capture, it does not scramble or drop what preceded it.
            std::vector<float> readback(kCapacityVertices * kFloatsPerVertex, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               static_cast<GLsizeiptr>(kCapacityVertices * kBytesPerVertex), readback.data());
            for (int point = 0; point < static_cast<int>(kCapacityVertices); ++point) {
                EXPECT_TRUE(CapturedVertexIs(readback.data() + static_cast<std::size_t>(point) * kFloatsPerVertex,
                                             point));
            }

            glDeleteBuffers(1, &captureBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

    } // namespace
} // namespace MGITest
