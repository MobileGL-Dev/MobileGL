// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/XfbRepeatedCaptureScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A CAPTURE MUST STILL RECORD WHEN IT IS NOT THE FIRST ONE IN THE PROCESS,
// AND THE CAPTURE STAGE MAY BE ANY OF THE FOUR THAT CAN BE THE LAST ONE.
//
// The conformance suite exposed a whole family of transform feedback failures that no
// existing scenario could reproduce, because every one of them ran ONE capture, from a
// VERTEX stage, in a freshly initialised process. What the suite actually does is
// different in three ways at once, and each of them turned out to matter:
//
//   * it runs case after case in ONE GL context, resetting state between them - and the
//     reset is not a fresh context. Its transform feedback part
//     (framework/opengl/gluStateReset.cpp resetStateGLCore) unbinds the generic
//     GL_TRANSFORM_FEEDBACK_BUFFER and then clears every indexed capture point from 0 to
//     GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, which permanently raises MobileGL's
//     touched-binding-point high-water mark. Every later capture that uses fewer points
//     than that - i.e. every INTERLEAVED_ATTRIBS capture - then had the unused tail
//     re-cleared on the driver immediately before glBeginTransformFeedback.
//     ReplayDeqpStateReset below is that reset, reduced to the calls that touch capture
//     state, so a defect that only appears from the second capture onwards is reachable
//     here instead of only on a device.
//
//   * the capture stage is frequently a GEOMETRY or a TESSELLATION EVALUATION shader,
//     never a plain vertex shader. The tree had zero coverage for either: none of the
//     Xfb* scenarios mentioned tessellation and neither TessellationDrawModeScenario nor
//     GeometryDrawModeScenario mentioned transform feedback.
//
//   * the capture program frequently has NO FRAGMENT STAGE at all, because it draws
//     under GL_RASTERIZER_DISCARD and never rasterises anything. That is legal in
//     desktop GL and the shape most "use transform feedback as a readback channel"
//     tests are built on.
//
// Every case here asserts the captured BYTES, never just the absence of a GL error: the
// failure this guards against writes nothing and raises nothing, so a buffer that kept
// its poison is the only thing that distinguishes it from success.

#include <cmath>
#include <string>
#include <utility>
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

        // Nothing a capture can legitimately produce, so a component that still reads it
        // names the failure ("the capture never reached these bytes") instead of looking
        // like an ordinary numeric mismatch.
        constexpr int kPoison = -987654;

        const char* const kPassthroughVertexSource = R"(#version 420 core
layout(location = 0) in int vs_in_value;
flat out int vs_out_value;
void main()
{
    vs_out_value = vs_in_value;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // The primitive_counter shape: one flat int per emitted vertex, several vertices
        // per input primitive, so the capture is geometry-AMPLIFIED and the CPU-side
        // primitive model cannot predict its length.
        const char* const kPointAmplifyingGeometrySource = R"(#version 420 core
layout(points) in;
layout(points, max_vertices = 2) out;
flat in int vs_out_value[];
flat out int gs_out_value;
void main()
{
    for (int i = 0; i < 2; ++i)
    {
        gs_out_value = vs_out_value[0];
        gl_Position = gl_in[0].gl_Position;
        EmitVertex();
        EndPrimitive();
    }
}
)";

        // Adjacency input. Only a geometry stage can consume it, and CountPrimitivesForDraw
        // used to answer 0 for every adjacency mode, which silently excluded the whole draw
        // from the capture accounting.
        const char* const kAdjacencyGeometrySource = R"(#version 420 core
layout(lines_adjacency) in;
layout(points, max_vertices = 1) out;
flat in int vs_out_value[];
flat out int gs_out_value;
void main()
{
    gs_out_value = vs_out_value[1];
    gl_Position = gl_in[1].gl_Position;
    EmitVertex();
    EndPrimitive();
}
)";

        const char* const kTessControlSource = R"(#version 420 core
layout(vertices = 1) out;
flat in int vs_out_value[];
patch out int tcs_out_value;
void main()
{
    tcs_out_value = vs_out_value[0];
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_out[gl_InvocationID].gl_Position = gl_in[0].gl_Position;
}
)";

        const char* const kTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw) in;
patch in int tcs_out_value;
flat out int tes_out_value;
void main()
{
    tes_out_value = tcs_out_value;
    gl_Position = gl_in[0].gl_Position;
}
)";

        const char* const kFragmentSource = R"(#version 420 core
flat in int gs_out_value;
out vec4 fragColor;
void main()
{
    fragColor = vec4(float(gs_out_value), 0.0, 0.0, 1.0);
}
)";

        class XfbRepeatedCaptureScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                const int values[kInputVertices] = {10, 11, 12, 13};
                glBufferData(GL_ARRAY_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
                glVertexAttribIPointer(0, 1, GL_INT, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (const GLuint program : m_programs) {
                    glDeleteProgram(program);
                }
                m_programs.clear();
                glBindVertexArray(0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vbo = 0;
                m_vao = 0;
                ScenarioTest::TearDown();
            }

            static constexpr int kInputVertices = 4;

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            static bool BackendHostsGeometry() {
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                DrainErrors();
                return maxGeometryOutputVertices >= 2;
            }

            static bool BackendHostsTessellation() {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                DrainErrors();
                return maxTessGenLevel >= 1;
            }

            // The transform-feedback-relevant half of deqp's resetStateGLCore, in its order.
            // It runs between EVERY pair of conformance cases, and running one capture
            // through it is the difference between "the first capture in the process" and
            // every other one.
            static void ReplayDeqpStateReset() {
                glBindVertexArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDisable(GL_RASTERIZER_DISCARD);
                glUseProgram(0);
                GLint maxSeparateAttribs = 0;
                glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, &maxSeparateAttribs);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
                for (GLint index = 0; index < maxSeparateAttribs; ++index) {
                    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLuint>(index), 0);
                }
                DrainErrors();
            }

            static std::string InfoLog(GLuint object, bool isShader) {
                GLint length = 0;
                if (isShader) {
                    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
                } else {
                    glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
                }
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                if (isShader) {
                    glGetShaderInfoLog(object, length + 1, nullptr, buffer.data());
                } else {
                    glGetProgramInfoLog(object, length + 1, nullptr, buffer.data());
                }
                return buffer.data();
            }

            GLuint BuildCaptureProgram(const std::vector<std::pair<GLenum, const char*>>& stages,
                                       const char* varying) {
                return BuildCaptureProgram(stages, std::vector<const char*>{varying});
            }

            // Builds a capture program out of `stages` capturing `varyings` interleaved.
            // Returns 0 and fills m_buildLog on failure.
            GLuint BuildCaptureProgram(const std::vector<std::pair<GLenum, const char*>>& stages,
                                       const std::vector<const char*>& varyings) {
                m_buildLog.clear();
                std::vector<GLuint> shaders;
                bool ok = true;
                for (const auto& [stage, source] : stages) {
                    const GLuint shader = glCreateShader(stage);
                    glShaderSource(shader, 1, &source, nullptr);
                    glCompileShader(shader);
                    GLint compiled = 0;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    shaders.push_back(shader);
                    if (compiled == GL_FALSE) {
                        m_buildLog = InfoLog(shader, true);
                        ok = false;
                        break;
                    }
                }
                GLuint program = 0;
                if (ok) {
                    program = glCreateProgram();
                    for (const GLuint shader : shaders) {
                        glAttachShader(program, shader);
                    }
                    glTransformFeedbackVaryings(program, static_cast<GLsizei>(varyings.size()), varyings.data(),
                                                GL_INTERLEAVED_ATTRIBS);
                    glLinkProgram(program);
                    GLint linked = GL_FALSE;
                    glGetProgramiv(program, GL_LINK_STATUS, &linked);
                    if (linked == GL_FALSE) {
                        m_buildLog = InfoLog(program, false);
                        glDeleteProgram(program);
                        program = 0;
                    }
                }
                for (const GLuint shader : shaders) {
                    glDeleteShader(shader);
                }
                if (program != 0) m_programs.push_back(program);
                return program;
            }

            // One capture span. `captureMode` is the transform feedback primitive mode,
            // `drawMode`/`count` the draw. Returns the capture buffer's contents.
            std::vector<int> RunCaptureSpan(GLuint program, GLenum captureMode, GLenum drawMode, GLsizei count,
                                            std::size_t capturedInts) {
                std::vector<int> poison(capturedInts, kPoison);
                GLuint xfbBuffer = 0;
                glGenBuffers(1, &xfbBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, xfbBuffer);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capturedInts * sizeof(int)), poison.data(),
                             GL_STATIC_COPY);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                // The capture point is the ONLY thing bound; the generic
                // GL_TRANSFORM_FEEDBACK_BUFFER binding comes along for the ride, exactly as
                // the conformance tests rely on (GL 4.6 core 6.1.1).
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

                glBindVertexArray(m_vao);
                glUseProgram(program);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(captureMode);
                glDrawArrays(drawMode, 0, count);
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);

                std::vector<int> readback(capturedInts, kPoison);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                                   static_cast<GLsizeiptr>(capturedInts * sizeof(int)), readback.data());
                glUseProgram(0);
                glDeleteBuffers(1, &xfbBuffer);
                return readback;
            }

            static ::testing::AssertionResult CapturedNothing(const std::vector<int>& data) {
                for (std::size_t i = 0; i < data.size(); ++i) {
                    if (data[i] != kPoison) {
                        return ::testing::AssertionFailure() << "component " << i << " is " << data[i];
                    }
                }
                return ::testing::AssertionSuccess();
            }

            static ::testing::AssertionResult CapturedIs(const std::vector<int>& data,
                                                         const std::vector<int>& expected) {
                if (data.size() != expected.size()) {
                    return ::testing::AssertionFailure()
                           << "captured " << data.size() << " value(s), expected " << expected.size();
                }
                for (std::size_t i = 0; i < data.size(); ++i) {
                    if (data[i] != expected[i]) {
                        ::testing::AssertionResult failure = ::testing::AssertionFailure();
                        failure << "component " << i << " is " << data[i] << ", expected " << expected[i];
                        if (data[i] == kPoison) {
                            failure << " (the capture never reached these bytes)";
                        }
                        return failure;
                    }
                }
                return ::testing::AssertionSuccess();
            }

            std::vector<GLuint> m_programs;
            std::string m_buildLog;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

        // THE REGRESSION GUARD FOR THE WHOLE FAMILY. Two geometry-stage captures in one
        // process with the conformance suite's own state reset between them; the assertion
        // that matters is on the SECOND one, which is the one every device run failed while
        // whichever body happened to land first in its process passed.
        TEST_F(XfbRepeatedCaptureScenario, ASecondGeometryCaptureAfterADeqpStateResetStillRecords) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString() << ")";
            }

            // Two vertices emitted per input point, so the capture is amplified beyond what
            // the CPU primitive model can predict from the draw alone.
            const std::vector<int> expected = {10, 10, 11, 11, 12, 12, 13, 13};

            for (int capture = 0; capture < 3; ++capture) {
                // A fresh program per capture, because that is what a fresh conformance case
                // builds - and it is what makes the driver recycle program and buffer names.
                const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource},
                                                            {GL_GEOMETRY_SHADER, kPointAmplifyingGeometrySource},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                           "gs_out_value");
                ASSERT_NE(program, 0u) << "capture " << capture << " program failed to build: " << m_buildLog;

                const std::vector<int> captured =
                    RunCaptureSpan(program, GL_POINTS, GL_POINTS, kInputVertices, expected.size());
                EXPECT_TRUE(CapturedIs(captured, expected))
                    << "capture " << capture << " of 3 in this process"
                    << (capture == 0 ? "" : " (every earlier one was followed by a deqp-shaped state reset)");
                EXPECT_EQ(glGetError(), GL_NO_ERROR) << "capture " << capture;

                glDeleteProgram(program);
                m_programs.pop_back();
                ReplayDeqpStateReset();
                glBindVertexArray(m_vao);
            }
        }

        // The tessellation half, which had no coverage anywhere in the tree: a capture taken
        // from a GL_PATCHES draw, whose last vertex-processing stage is the evaluation shader
        // and whose record count only the tessellator knows.
        TEST_F(XfbRepeatedCaptureScenario, ACaptureFromAPatchesDrawRecords) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }

            // One input patch of one vertex, all levels at 1: the tessellator emits exactly
            // one triangle, so three captured vertices all carrying the first input value.
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kTessEvalSource}},
                                                       "tes_out_value");
            ASSERT_NE(program, 0u) << "patch capture program failed to build: " << m_buildLog;

            const std::vector<int> expected = {10, 10, 10};
            const std::vector<int> captured = RunCaptureSpan(program, GL_TRIANGLES, GL_PATCHES, 1, expected.size());
            EXPECT_TRUE(CapturedIs(captured, expected));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // A capture program with NO FRAGMENT STAGE, drawn under GL_RASTERIZER_DISCARD. Legal
        // in desktop GL, and the shape most transform-feedback-as-readback tests use; the
        // program above only differs from it by the fragment shader, so a failure here is
        // specifically about the missing stage.
        TEST_F(XfbRepeatedCaptureScenario, ACaptureFromAFragmentlessProgramRecords) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString() << ")";
            }

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource},
                                                        {GL_GEOMETRY_SHADER, kPointAmplifyingGeometrySource}},
                                                       "gs_out_value");
            ASSERT_NE(program, 0u) << "fragmentless capture program failed to build: " << m_buildLog;

            const std::vector<int> expected = {10, 10, 11, 11, 12, 12, 13, 13};
            const std::vector<int> captured =
                RunCaptureSpan(program, GL_POINTS, GL_POINTS, kInputVertices, expected.size());
            EXPECT_TRUE(CapturedIs(captured, expected));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // An ADJACENCY draw feeding the capture. CountPrimitivesForDraw answered 0 for all
        // four adjacency modes, which made the transform feedback accounting skip the draw
        // entirely - so neither the captured-vertex counter nor the geometry-capture-draw
        // flag moved, and anything downstream of either was working from "nothing happened".
        TEST_F(XfbRepeatedCaptureScenario, ACaptureFromAnAdjacencyDrawRecords) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString() << ")";
            }

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource},
                                                        {GL_GEOMETRY_SHADER, kAdjacencyGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       "gs_out_value");
            ASSERT_NE(program, 0u) << "adjacency capture program failed to build: " << m_buildLog;

            // Four vertices of GL_LINES_ADJACENCY are one line primitive; the shader emits
            // the second vertex of the four, which is the line's first real endpoint.
            const std::vector<int> expected = {11};
            const std::vector<int> captured =
                RunCaptureSpan(program, GL_POINTS, GL_LINES_ADJACENCY, kInputVertices, expected.size());
            EXPECT_TRUE(CapturedIs(captured, expected));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // An adjacency draw with NO geometry stage. GL 4.6 core table 13.1 admits
        // GL_LINES_ADJACENCY and GL_LINE_STRIP_ADJACENCY under capture mode GL_LINES (and the
        // triangle pair under GL_TRIANGLES): without a geometry shader the adjacent vertices
        // are ignored and the primitive assembled is a plain line, so the combination is legal
        // and must capture. MobileGL's active-capture primitive-mode table listed only the
        // non-adjacency modes, so this raised GL_INVALID_OPERATION and dropped the draw
        // entirely - the buffer kept its pre-draw bytes and the application saw an error the
        // spec does not allow. Distinct from ACaptureFromAnAdjacencyDrawRecords above, which
        // HAS a geometry stage and therefore bypasses that table completely.
        TEST_F(XfbRepeatedCaptureScenario, AVertexOnlyAdjacencyCaptureRecords) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program =
                BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource}}, "vs_out_value");
            ASSERT_NE(program, 0u) << "vertex-only capture program failed to build: " << m_buildLog;

            // Four vertices of GL_LINES_ADJACENCY are one line whose real endpoints are the
            // middle pair, so the capture is those two vertices in order.
            const std::vector<int> expected = {11, 12};
            const std::vector<int> captured =
                RunCaptureSpan(program, GL_LINES, GL_LINES_ADJACENCY, kInputVertices, expected.size());

            // THE GUARD FOR THE DEFECT ITSELF, and it is backend-independent: the frontend
            // validator must not reject the combination. It used to record
            // GL_INVALID_OPERATION and return before the draw was ever issued.
            EXPECT_EQ(glGetError(), GL_NO_ERROR)
                << "a capture-mode/draw-mode pair GL 4.6 core table 13.1 admits must raise no error";

            // Whether the capture then RECORDS is a backend question, and the two answer it
            // differently. ES 3.2 (10.1) supports the adjacency primitive types only for a
            // pipeline with a geometry shader, so DirectGLES has nothing to forward this draw
            // to; desktop GL and Vulkan both assemble the plain line and capture it. Asserting
            // the data unconditionally would be asserting that DirectGLES emulates a whole ES
            // restriction away, which is a separate piece of work and not what this guards.
            if (Gl().BackendName() == "DirectGLES") {
                GTEST_SKIP() << "DirectGLES cannot forward a geometry-shader-less adjacency draw: ES 3.2 10.1 "
                                "supports the adjacency primitive types only with a geometry stage. The frontend "
                                "no longer rejects the draw (checked above), which is the defect this covers.";
            }
            EXPECT_TRUE(CapturedIs(captured, expected));
        }

        // A CAPTURE MUST NEVER LAND IN A BUFFER THE APPLICATION DID NOT BIND FOR IT.
        //
        // A capture list may legally begin with gl_NextBuffer, which leaves capture buffer 0
        // with stride 0 and nothing to capture - so glBeginTransformFeedback does not require a
        // buffer at point 0 and the application binds only point 1. The driver-side program is
        // a single-buffer interleaved capture (the pseudo-varyings are consumed at link time),
        // so it writes capture point 0, and MobileGL redirects that into scratch storage and
        // scatters the records afterwards.
        //
        // Two ways that went wrong, both fixed here: the scratch was sized by reading each
        // target's stride at its POSITION in a list that skips unbound buffers, which for this
        // layout read stride 0 for everything and produced a zero capacity; and when the
        // scratch then failed to bind, the span opened anyway onto whatever capture point 0
        // still held from an earlier capture in the process - silently overwriting an unrelated
        // application buffer. The first span below exists purely to leave such a binding behind.
        TEST_F(XfbRepeatedCaptureScenario, ACaptureListBeginningWithGlNextBufferSparesTheEarlierBuffer) {
            if (!Ready()) GTEST_SKIP();

            const std::size_t capturedInts = 4;
            const GLsizeiptr captureBytes = static_cast<GLsizeiptr>(capturedInts * sizeof(int));

            // Span A: an ordinary capture, so capture point 0 is left holding bufferA.
            const GLuint programA =
                BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource}}, "vs_out_value");
            ASSERT_NE(programA, 0u) << "plain capture program failed to build: " << m_buildLog;

            std::vector<int> poison(capturedInts, kPoison);
            GLuint bufferA = 0;
            glGenBuffers(1, &bufferA);
            glBindBuffer(GL_ARRAY_BUFFER, bufferA);
            glBufferData(GL_ARRAY_BUFFER, captureBytes, poison.data(), GL_STATIC_COPY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, bufferA);

            glBindVertexArray(m_vao);
            glUseProgram(programA);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, kInputVertices);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            glUseProgram(0);

            std::vector<int> afterA(capturedInts, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBytes, afterA.data());
            const std::vector<int> spanAExpected = {10, 11, 12, 13};
            ASSERT_TRUE(CapturedIs(afterA, spanAExpected)) << "the setup span itself did not capture";

            // Span B: gl_NextBuffer first, so buffer 0 captures nothing and only point 1 is bound.
            const GLuint programB = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource}},
                                                        {"gl_NextBuffer", "vs_out_value"});
            if (programB == 0) {
                GTEST_SKIP() << "gl_NextBuffer capture lists are not linkable on " << Gl().BackendName() << " ("
                             << Gl().RendererString() << "): " << m_buildLog;
            }

            GLuint bufferB = 0;
            glGenBuffers(1, &bufferB);
            glBindBuffer(GL_ARRAY_BUFFER, bufferB);
            glBufferData(GL_ARRAY_BUFFER, captureBytes, poison.data(), GL_STATIC_COPY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            // Point 0 released, point 1 is the only destination this capture asks for.
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, bufferB);

            glUseProgram(programB);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, kInputVertices);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);
            glUseProgram(0);

            // THE ASSERTION THAT MATTERS: bufferA was not a destination of this capture, so it
            // must still read exactly what span A left in it. A failure here is the corruption.
            std::vector<int> bufferAAfterB(capturedInts, 0);
            glBindBuffer(GL_ARRAY_BUFFER, bufferA);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, captureBytes, bufferAAfterB.data());
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            EXPECT_TRUE(CapturedIs(bufferAAfterB, spanAExpected))
                << "the gl_NextBuffer capture wrote into the buffer the PREVIOUS span had bound";

            EXPECT_EQ(glGetError(), GL_NO_ERROR);

            // ...and, where the backend places this layout at all, the buffer it WAS asked to
            // write gets the records. That placement is the DirectGLES scatter path, whose
            // scratch sizing used to read each target's stride at its POSITION in a list that
            // skips unbound capture buffers - which for a leading gl_NextBuffer read stride 0
            // for every target and sized the scratch at zero. DirectVulkan does not implement a
            // leading-gl_NextBuffer layout at all (it captures nothing into bufferB); that is a
            // pre-existing gap of its own, and the assertion above - that it corrupts nothing
            // while declining - is what matters for it.
            const bool backendPlacesLeadingNextBuffer = Gl().BackendName() != "DirectVulkan";
            if (backendPlacesLeadingNextBuffer) {
                std::vector<int> bufferBAfter(capturedInts, kPoison);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBytes, bufferBAfter.data());
                EXPECT_TRUE(CapturedIs(bufferBAfter, spanAExpected));
            }

            // Unbound and deleted BEFORE any skip: a capture point left pointing at a buffer
            // this test deleted would follow the process into the next scenario.
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, 0);
            glDeleteBuffers(1, &bufferA);
            glDeleteBuffers(1, &bufferB);

            if (!backendPlacesLeadingNextBuffer) {
                GTEST_SKIP() << "DirectVulkan does not place a capture list beginning with gl_NextBuffer; it "
                                "captures nothing, which the no-corruption assertion above has already covered.";
            }
        }

        // The control for all of the above: a span that never draws must leave the capture
        // buffer alone. Without it "the buffer kept its poison" could be read as the correct
        // outcome of some path rather than as the bug, and the tightened early returns in
        // StartPendingTransformFeedback have to keep this legal case legal.
        TEST_F(XfbRepeatedCaptureScenario, ASpanThatNeverDrawsLeavesTheCaptureBufferAlone) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsGeometry()) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString() << ")";
            }

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPassthroughVertexSource},
                                                        {GL_GEOMETRY_SHADER, kPointAmplifyingGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       "gs_out_value");
            ASSERT_NE(program, 0u) << "capture program failed to build: " << m_buildLog;

            const std::size_t capturedInts = 8;
            std::vector<int> poison(capturedInts, kPoison);
            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, xfbBuffer);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capturedInts * sizeof(int)), poison.data(),
                         GL_STATIC_COPY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

            glUseProgram(program);
            glBeginTransformFeedback(GL_POINTS);
            glEndTransformFeedback();
            glUseProgram(0);

            std::vector<int> readback(capturedInts, 0);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               static_cast<GLsizeiptr>(capturedInts * sizeof(int)), readback.data());
            EXPECT_TRUE(CapturedNothing(readback));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);

            glDeleteBuffers(1, &xfbBuffer);
        }

    } // namespace
} // namespace MGITest
