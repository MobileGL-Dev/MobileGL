// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/GeometryDrawModeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A GEOMETRY SHADER'S INPUT PRIMITIVE CONSTRAINS THE DRAW MODE, AND
// GL_NONE IS NOT A USABLE "NO GEOMETRY SHADER" SENTINEL.
//
// GL 4.6 core 11.3.1: mode must be one of the primitive types that decomposes into the
// geometry shader's declared input primitive, or the draw is GL_INVALID_OPERATION. The
// validator asked "is there a geometry stage?" by comparing the REFLECTED INPUT PRIMITIVE
// against GL_NONE - and GL_NONE and GL_POINTS are both 0, so a `layout(points) in` geometry
// shader answered "no geometry stage" and every mode sailed through. The rule was therefore
// dead for exactly the geometry shaders whose input primitive rejects the most modes.
//
// KHR-GL43.transform_feedback.api_errors_test is where it showed: it draws a points-in
// geometry program with GL_LINES through glDrawTransformFeedbackInstanced and requires
// INVALID_OPERATION. The bug is not specific to that entry point - every draw shares this
// validator - so the ordinary glDrawArrays spelling is pinned here too, and the lines-in
// program is the control that proves the rule was not simply widened.
//
// Needs a real context: the validator returns before this rule when no backend object is
// active, so the GPU-free negative-API suite cannot reach it.

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

        const char* const kVertexSource = R"(#version 420 core
void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // The input primitive the CTS case uses, and the one the GL_NONE sentinel erased.
        // `result` is here so the same program can be captured with transform feedback.
        const char* const kPointsInGeometrySource = R"(#version 420 core
layout(points) in;
layout(points, max_vertices = 1) out;
out float result;
void main()
{
    gl_Position = gl_in[0].gl_Position;
    result = 1.0;
    EmitVertex();
}
)";

        const char* const kLinesInGeometrySource = R"(#version 420 core
layout(lines) in;
layout(points, max_vertices = 1) out;
void main()
{
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
}
)";

        const char* const kFragmentSource = R"(#version 420 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        class GeometryDrawModeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                if (!BackendHostsGeometry()) {
                    GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " ("
                                 << Gl().RendererString() << "); there is no input primitive to validate";
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (const GLuint program : m_programs) {
                    glDeleteProgram(program);
                }
                m_programs.clear();
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
            }

            // The same real-backend probe IoBlockNameCollisionScenario uses: 0 on a DirectGLES
            // driver without GL_EXT_geometry_shader and on a DirectVulkan device without the
            // geometryShader feature.
            static bool BackendHostsGeometry() {
                GLint maxGeometryOutputVertices = 0;
                glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
                DrainErrors();
                return maxGeometryOutputVertices >= 4;
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            GLuint BuildProgram(const char* geometrySource, const char* capturedVarying = nullptr) {
                const std::vector<std::pair<GLenum, const char*>> stages = {
                    {GL_VERTEX_SHADER, kVertexSource},
                    {GL_GEOMETRY_SHADER, geometrySource},
                    {GL_FRAGMENT_SHADER, kFragmentSource}};

                std::vector<GLuint> shaders;
                bool ok = true;
                for (const auto& [stage, source] : stages) {
                    const GLuint shader = glCreateShader(stage);
                    glShaderSource(shader, 1, &source, nullptr);
                    glCompileShader(shader);
                    GLint compiled = 0;
                    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                    shaders.push_back(shader);
                    if (!compiled) {
                        m_buildLog = InfoLog(shader, true);
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    for (const GLuint shader : shaders) glDeleteShader(shader);
                    return 0;
                }

                const GLuint program = glCreateProgram();
                for (const GLuint shader : shaders) glAttachShader(program, shader);
                if (capturedVarying != nullptr) {
                    glTransformFeedbackVaryings(program, 1, &capturedVarying, GL_INTERLEAVED_ATTRIBS);
                }
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                for (const GLuint shader : shaders) glDeleteShader(shader);
                if (!linked) {
                    m_buildLog = InfoLog(program, false);
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
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

            const std::string& BuildLog() const { return m_buildLog; }

            GLuint m_vao = 0;
            std::vector<GLuint> m_programs;
            std::string m_buildLog;
        };

        // GL_POINTS is the only mode that decomposes into a points input primitive.
        TEST_F(GeometryDrawModeScenario, PointsInGeometryProgramRejectsEveryOtherMode) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program = BuildProgram(kPointsInGeometrySource);
            ASSERT_NE(program, 0u) << "the points-in geometry program did not build: " << BuildLog();

            glUseProgram(program);
            DrainErrors();

            for (const GLenum mode :
                 {static_cast<GLenum>(GL_LINES), static_cast<GLenum>(GL_LINE_STRIP),
                  static_cast<GLenum>(GL_TRIANGLES), static_cast<GLenum>(GL_TRIANGLE_STRIP)}) {
                glDrawArrays(mode, 0, 3);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                    << "mode " << mode << " does not decompose into the geometry shader's points input";
                DrainErrors();
            }

            // The one mode that IS compatible still draws.
            glDrawArrays(GL_POINTS, 0, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            DrainErrors();
        }

        // The same rule reached through glDrawTransformFeedback*, which is the spelling the CTS
        // case asks about. The capture span is really completed first, so GL_POINTS comes back
        // GL_NO_ERROR: without that the draw would report INVALID_OPERATION for the
        // never-ended-a-span reason instead and the case could not tell the two apart.
        TEST_F(GeometryDrawModeScenario, PointsInGeometryProgramRejectsNonPointModesOnFeedbackDraws) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program = BuildProgram(kPointsInGeometrySource, "result");
            ASSERT_NE(program, 0u) << "the points-in geometry program did not build: " << BuildLog();

            GLuint feedback = 0;
            glGenTransformFeedbacks(1, &feedback);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, feedback);
            GLuint captureBuffer = 0;
            glGenBuffers(1, &captureBuffer);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, captureBuffer);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 64, nullptr, GL_STATIC_DRAW);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBuffer);
            glUseProgram(program);
            DrainErrors();

            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, 1);
            glEndTransformFeedback();
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "the capture span did not complete";

            glDrawTransformFeedbackInstanced(GL_LINES, feedback, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                << "glDrawTransformFeedbackInstanced must honour the geometry input primitive";
            DrainErrors();

            glDrawTransformFeedbackStreamInstanced(GL_LINES, feedback, 0, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                << "glDrawTransformFeedbackStreamInstanced must honour the geometry input primitive";
            DrainErrors();

            // The compatible mode replays the captured span with no error at all, which is what
            // makes the two assertions above about the MODE and not about the span.
            glDrawTransformFeedbackInstanced(GL_POINTS, feedback, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "a compatible mode must still replay the captured span";
            DrainErrors();

            glUseProgram(0);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
            glDeleteBuffers(1, &captureBuffer);
            glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, 0);
            glDeleteTransformFeedbacks(1, &feedback);
            DrainErrors();
        }

        // The control: a lines-in geometry shader is a NON-zero input primitive, so it exercised
        // the rule even before the fix. It must still accept the line modes and still reject the
        // others - a fix that widened the rule instead of repairing its guard breaks this.
        TEST_F(GeometryDrawModeScenario, LinesInGeometryProgramStillAcceptsLineModesOnly) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program = BuildProgram(kLinesInGeometrySource);
            ASSERT_NE(program, 0u) << "the lines-in geometry program did not build: " << BuildLog();

            glUseProgram(program);
            DrainErrors();

            for (const GLenum mode : {static_cast<GLenum>(GL_LINES), static_cast<GLenum>(GL_LINE_STRIP),
                                      static_cast<GLenum>(GL_LINE_LOOP)}) {
                glDrawArrays(mode, 0, 2);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << "mode " << mode << " decomposes into lines and must be accepted";
                DrainErrors();
            }

            for (const GLenum mode : {static_cast<GLenum>(GL_POINTS), static_cast<GLenum>(GL_TRIANGLES)}) {
                glDrawArrays(mode, 0, 3);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                    << "mode " << mode << " does not decompose into lines";
                DrainErrors();
            }
        }

        // The other half of "ask the stage": WHICH stage list is asked. gsInputPrimitive is a
        // LINK artifact, so pairing it with the live attach list re-points the GL_NONE/GL_POINTS
        // aliasing instead of removing it - inside the window between glAttachShader and the
        // next link, the live list says "geometry present" while the artifact still reads
        // GL_NONE, which is 0, which is GL_POINTS, so every mode but GL_POINTS is rejected.
        //
        // GL 4.6 core 7.3 makes that window legal and ordinary: an attach affects the program's
        // executable only at the next link, and leaves LINK_STATUS alone. The attached shader
        // need not even compile. Worse, it does not heal - glDetachShader defers the removal to
        // the next Link() too, so the program would keep failing every non-POINTS draw until the
        // application happened to relink for some unrelated reason.
        TEST_F(GeometryDrawModeScenario, AttachingAGeometryStageAfterTheLinkDoesNotConstrainTheDrawMode) {
            if (!Ready()) GTEST_SKIP();

            // Deliberately NOT BuildProgram: the executable under test has no geometry stage.
            const GLuint program = glCreateProgram();
            m_programs.push_back(program);
            for (const auto& [stage, source] :
                 std::vector<std::pair<GLenum, const char*>>{{GL_VERTEX_SHADER, kVertexSource},
                                                             {GL_FRAGMENT_SHADER, kFragmentSource}}) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                glAttachShader(program, shader);
                glDeleteShader(shader);
            }
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE) << "the vertex+fragment program did not link";

            glUseProgram(program);
            DrainErrors();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "a program with no geometry stage must draw triangles";
            DrainErrors();

            const GLuint geometry = glCreateShader(GL_GEOMETRY_SHADER);
            glShaderSource(geometry, 1, &kPointsInGeometrySource, nullptr);
            glCompileShader(geometry);
            glAttachShader(program, geometry);
            glDeleteShader(geometry);
            DrainErrors();

            // Same executable as three lines ago - no relink has happened.
            glDrawArrays(GL_TRIANGLES, 0, 3);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "the attach does not reach the executable until the next link, so the geometry "
                   "shader's points input must not constrain this draw";
            DrainErrors();

            // And once it IS linked in, the rule applies - the fix must not have simply disabled it.
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE) << "the relink with the geometry stage failed";
            glUseProgram(program);
            DrainErrors();
            glDrawArrays(GL_TRIANGLES, 0, 3);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                << "now that the points-in geometry shader is in the executable, triangles must be rejected";
            DrainErrors();
        }

        // The tessellation guard above the geometry one had the identical defect, and it does not
        // even need the GL_NONE aliasing to misfire: it drives BOTH directions unconditionally, so
        // reading the live attach list rejects every non-GL_PATCHES draw the moment an evaluation
        // shader is attached, whether or not it was ever linked in.
        TEST_F(GeometryDrawModeScenario, AttachingATessEvalStageAfterTheLinkDoesNotForceGlPatches) {
            if (!Ready()) GTEST_SKIP();

            GLint maxPatchVertices = 0;
            glGetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVertices);
            DrainErrors();
            if (maxPatchVertices < 3) GTEST_SKIP() << "no tessellation stage on this backend";

            const GLuint program = glCreateProgram();
            m_programs.push_back(program);
            for (const auto& [stage, source] :
                 std::vector<std::pair<GLenum, const char*>>{{GL_VERTEX_SHADER, kVertexSource},
                                                             {GL_FRAGMENT_SHADER, kFragmentSource}}) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                glAttachShader(program, shader);
                glDeleteShader(shader);
            }
            glLinkProgram(program);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE) << "the vertex+fragment program did not link";

            glUseProgram(program);
            DrainErrors();

            static const char* const kTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, ccw) in;
void main()
{
    gl_Position = gl_in[0].gl_Position;
}
)";
            const GLuint tessEval = glCreateShader(GL_TESS_EVALUATION_SHADER);
            glShaderSource(tessEval, 1, &kTessEvalSource, nullptr);
            glCompileShader(tessEval);
            glAttachShader(program, tessEval);
            glDeleteShader(tessEval);
            DrainErrors();

            glDrawArrays(GL_TRIANGLES, 0, 3);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "the executable still has no tessellation stage, so GL_PATCHES must not be required";
            DrainErrors();
        }

    } // namespace
} // namespace MGITest
