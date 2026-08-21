// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TessellationDrawModeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GL_PATCHES AND THE TESSELLATION PIPELINE ARE EACH OTHER'S ONLY PARTNER.
//
// GL 4.6 core 10.1 states the rule in both directions, and both are GL_INVALID_OPERATION:
// a program with a tessellation evaluation shader may only be drawn with GL_PATCHES, and
// GL_PATCHES may only be drawn with such a program. MobileGL's draw-mode validator
// implemented the geometry-shader input-primitive rule and NOTHING for tessellation, which
// is two of the four sites KHR-GL43.transform_feedback.api_errors_test checks (all four
// share one copy-pasted message string, so the trace cannot say which one it stopped at).
//
// Needs a real context: the validator returns before either rule when no backend object is
// active, so the GPU-free negative-API suite cannot reach them.

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

        const char* const kTessControlSource = R"(#version 420 core
layout(vertices = 1) out;
void main()
{
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_out[gl_InvocationID].gl_Position = gl_in[0].gl_Position;
}
)";

        const char* const kTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw) in;
void main()
{
    gl_Position = gl_in[0].gl_Position;
}
)";

        const char* const kFragmentSource = R"(#version 420 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        class TessellationDrawModeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                if (!BackendHostsTessellation()) {
                    GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " ("
                                 << Gl().RendererString() << "); there is no patch draw to validate";
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
            // driver without GL_EXT_tessellation_shader and on a DirectVulkan device without
            // the tessellationShader feature.
            static bool BackendHostsTessellation() {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                DrainErrors();
                return maxTessGenLevel >= 1;
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            GLuint BuildProgram(const std::vector<std::pair<GLenum, const char*>>& stages) {
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

        // A tessellation program drawn with anything but GL_PATCHES.
        TEST_F(TessellationDrawModeScenario, TessellationProgramRejectsNonPatchModes) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program = BuildProgram({{GL_VERTEX_SHADER, kVertexSource},
                                                 {GL_TESS_CONTROL_SHADER, kTessControlSource},
                                                 {GL_TESS_EVALUATION_SHADER, kTessEvalSource},
                                                 {GL_FRAGMENT_SHADER, kFragmentSource}});
            ASSERT_NE(program, 0u) << "the tessellation program did not build: " << BuildLog();

            glUseProgram(program);
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            for (const GLenum mode : {static_cast<GLenum>(GL_POINTS), static_cast<GLenum>(GL_LINES),
                                      static_cast<GLenum>(GL_TRIANGLES)}) {
                glDrawArrays(mode, 0, 1);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                    << "mode " << mode << " must not be accepted while tessellation is active";
                DrainErrors();
            }

            // The one mode that IS accepted still is - a rule keyed any wider would break every
            // patch draw in the suite.
            glDrawArrays(GL_PATCHES, 0, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            DrainErrors();
        }

        // ... and the other direction: GL_PATCHES without a tessellation evaluation stage.
        TEST_F(TessellationDrawModeScenario, PatchesRejectedWithoutATessellationEvaluationStage) {
            if (!Ready()) GTEST_SKIP();

            const GLuint program =
                BuildProgram({{GL_VERTEX_SHADER, kVertexSource}, {GL_FRAGMENT_SHADER, kFragmentSource}});
            ASSERT_NE(program, 0u) << "the vertex/fragment program did not build: " << BuildLog();

            glUseProgram(program);
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            glDrawArrays(GL_PATCHES, 0, 1);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION))
                << "GL_PATCHES has no meaning without a tessellation evaluation stage";
            DrainErrors();

            // The same program with an ordinary mode is untouched.
            glDrawArrays(GL_TRIANGLES, 0, 3);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            DrainErrors();
        }

    } // namespace
} // namespace MGITest
