// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PointSizeDemotionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE gl_PointSize DEMOTION IS CLIENT-INVISIBLE, AND IT ACTUALLY ARMS.
//
// On a device that hosts the built-in in tessellation/geometry stages (llvmpipe and
// lavapipe both do), gl_PointSize travels as itself; on one that does not (the Mali
// devices this exists for), phase B demotes it to an ordinary varying
// (ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram) and the capture
// machinery follows it there. This scenario runs in BOTH configurations and must hand
// back identical bytes: the ambient registrations take the native path, and the
// PointSizeDemotion. registrations pin MOBILEGL_POINT_SIZE_DEMOTION=1 so the demotion
// runs on the same healthy drivers - CopyImagePacked16Scenario's dual-configuration
// contract, applied to a value chain instead of a storage format.
//
// The VALUE is the whole contract: every case writes gl_PointSize in one stage, reads it
// back out of gl_in[] in the next, and captures it by name under rasterizer discard, so
// one wrong link anywhere in VS -> TCS -> TES -> GS -> capture lands in the readback.
// The RASTERIZED size is deliberately not asserted anywhere: with the built-in unhosted
// it falls back to 1.0 by spec on both targets, which is exactly the honest residue the
// demotion documents (point_rendering-style bodies keep failing truthfully).
//
// The assertions are on the captured BYTES against a CPU-computed reference, never on
// the absence of a GL error: every failure this guards against is silent.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

        constexpr float kPoison = -987654.0f;

        const char* const kFragmentSource = R"(#version 460 core
layout(location = 0) out vec4 fragColor;
void main()
{
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        // The full chain, with per-vertex VARIATION seeded in the vertex stage so a control
        // invocation that read or wrote the wrong slot changes the sum: 2,3,4 arrive, 3,4,5
        // leave, the evaluation stage sums its patch to 12, the geometry stage doubles what
        // it read to 24.
        const char* const kChainVertexSource = R"(#version 460 core
void main()
{
    gl_Position  = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 2.0 + float(gl_VertexID);
}
)";

        const char* const kChainTessControlSource = R"(#version 460 core
layout(vertices = 3) out;
void main()
{
    gl_out[gl_InvocationID].gl_Position  = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_PointSize = gl_in[gl_InvocationID].gl_PointSize + 1.0;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

        const char* const kChainTessEvalSource = R"(#version 460 core
layout(triangles, equal_spacing, cw, point_mode) in;
void main()
{
    gl_Position  = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = gl_in[0].gl_PointSize + gl_in[1].gl_PointSize + gl_in[2].gl_PointSize;
}
)";

        const char* const kChainGeometrySource = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main()
{
    gl_Position  = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize * 2.0;
    EmitVertex();
    EndPrimitive();
}
)";

        // The geometry-only chain: no tessellation required of the stack at all.
        const char* const kPointVertexSource = R"(#version 460 core
void main()
{
    gl_Position  = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 7.0;
}
)";

        const char* const kPointGeometrySource = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main()
{
    gl_Position  = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize + 1.0;
    EmitVertex();
    EndPrimitive();
}
)";

        class PointSizeDemotionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                DrainErrors();
            }

            void TearDown() override {
                if (Ready()) {
                    glUseProgram(0);
                    for (const GLuint program : m_programs) {
                        glDeleteProgram(program);
                    }
                    m_programs.clear();
                    glBindVertexArray(0);
                    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                    m_vao = 0;
                }
                ScenarioTest::TearDown();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            static bool BackendHostsTessellation() {
                GLint maxTessGenLevel = 0;
                glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxTessGenLevel);
                DrainErrors();
                return maxTessGenLevel >= 1;
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
                        m_buildLog = InfoLog(shader, true) + "\n--- source ---\n" + source;
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
                    glTransformFeedbackVaryings(program, static_cast<GLsizei>(varyings.size()),
                                                varyings.data(), GL_INTERLEAVED_ATTRIBS);
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

            // One capture span over `vertexCount` vertices of `drawMode`, recorded as
            // GL_POINTS. The buffer is poison-filled first so bytes the capture never wrote
            // name themselves.
            std::vector<float> RunCaptureSpan(GLuint program, GLenum drawMode, GLsizei vertexCount,
                                              std::size_t capturedFloats) {
                const std::vector<float> poison(capturedFloats, kPoison);
                GLuint xfbBuffer = 0;
                glGenBuffers(1, &xfbBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, xfbBuffer);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capturedFloats * sizeof(float)),
                             poison.data(), GL_STATIC_COPY);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

                glBindVertexArray(m_vao);
                glUseProgram(program);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(drawMode, 0, vertexCount);
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);

                std::vector<float> readback(capturedFloats, kPoison);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                                   static_cast<GLsizeiptr>(capturedFloats * sizeof(float)),
                                   readback.data());
                glUseProgram(0);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
                glDeleteBuffers(1, &xfbBuffer);
                return readback;
            }

            static ::testing::AssertionResult ComponentIs(const std::vector<float>& data,
                                                          std::size_t index, float expected,
                                                          float epsilon = 1e-4f) {
                if (index >= data.size()) {
                    return ::testing::AssertionFailure()
                           << "component " << index << " is past the capture buffer";
                }
                const float actual = data[index];
                if (actual == kPoison) {
                    return ::testing::AssertionFailure()
                           << "component " << index << " still holds the poison value - the capture "
                           << "never reached these bytes (expected " << expected << ")";
                }
                if (std::isnan(actual) || std::abs(actual - expected) > epsilon) {
                    return ::testing::AssertionFailure()
                           << "component " << index << " is " << actual << ", expected " << expected;
                }
                return ::testing::AssertionSuccess();
            }

            // The library log, for the arming case. Same machinery and same reasoning as
            // UnlocatedIoBlockScenario: MOBILEGL_LOG_FILE_PATH is read at log-init, the file
            // is appended to by every process in the lane, and only bytes appended after the
            // snapshot may satisfy an assertion.
            static std::filesystem::path LibraryLogPath() {
                const char* path = std::getenv("MOBILEGL_LOG_FILE_PATH");
                return (path != nullptr && *path != '\0') ? std::filesystem::path(path)
                                                          : std::filesystem::path();
            }

            static std::uintmax_t LibraryLogSize() {
                std::error_code ec;
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return 0;
                const std::uintmax_t size = std::filesystem::file_size(path, ec);
                return ec ? 0 : size;
            }

            static std::string LibraryLogSince(std::uintmax_t offset) {
                const std::filesystem::path path = LibraryLogPath();
                if (path.empty()) return {};
                std::ifstream file(path, std::ios::binary);
                if (!file.good()) return {};
                file.seekg(static_cast<std::streamoff>(offset));
                return std::string((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            }

            std::string m_buildLog;

        private:
            GLuint m_vao = 0;
            std::vector<GLuint> m_programs;
        };

        // The five-stage chain. 24.0 can only arrive if the vertex mirror, both control-stage
        // redirects (read AND write), the evaluation stage's three gl_in reads and the
        // geometry stage's read all carried the right value - one wrong link and the sum
        // moves. point_mode with every level at 1 emits three points; the first record proves
        // the mechanism, exactly as TessellationXfbCaptureScenario reasons.
        TEST_F(PointSizeDemotionScenario, TheValueSurvivesTheFiveStageChainIntoTheCapture) {
            if (!Ready()) return;
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " ("
                             << Gl().RendererString() << ")";
            }
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kChainVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kChainTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kChainTessEvalSource},
                                                        {GL_GEOMETRY_SHADER, kChainGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            const std::vector<float> captured = RunCaptureSpan(program, GL_PATCHES, 3, 3);
            EXPECT_TRUE(ComponentIs(captured, 0, 24.0f));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The same chain without a geometry stage: the capture then binds to the evaluation
        // stage's value (the sum, 12.0) - which is also the boundary where a demoted program
        // switches its capture carrier from the Io chain to the capture name.
        TEST_F(PointSizeDemotionScenario, TheEvaluationStageOwnsTheCaptureWithoutAGeometryStage) {
            if (!Ready()) return;
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " ("
                             << Gl().RendererString() << ")";
            }
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kChainVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kChainTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kChainTessEvalSource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            const std::vector<float> captured = RunCaptureSpan(program, GL_PATCHES, 3, 3);
            EXPECT_TRUE(ComponentIs(captured, 0, 12.0f));

            // The GL query surface keeps the truthful spelling whatever the backends renamed
            // underneath: reflection is a phase-A product and the demotion happens after it.
            char varyingName[64] = {};
            GLsizei nameLength = 0;
            GLsizei varyingSize = 0;
            GLenum varyingType = 0;
            glGetTransformFeedbackVarying(program, 0, sizeof(varyingName), &nameLength, &varyingSize,
                                          &varyingType, varyingName);
            EXPECT_STREQ(varyingName, "gl_PointSize");
            EXPECT_EQ(varyingType, static_cast<GLenum>(GL_FLOAT));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The geometry-only chain: gl_in[0].gl_PointSize read straight off the vertex stage,
        // no tessellation involved - the VS -> GS boundary of the demotion on its own.
        TEST_F(PointSizeDemotionScenario, AGeometryOnlyChainCarriesTheVertexValue) {
            if (!Ready()) return;

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPointVertexSource},
                                                        {GL_GEOMETRY_SHADER, kPointGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            const std::vector<float> captured = RunCaptureSpan(program, GL_POINTS, 1, 1);
            EXPECT_TRUE(ComponentIs(captured, 0, 8.0f));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // THE ONE CASE THAT CAN FAIL WHEN THE DEMOTION SILENTLY STOPS BEING ARMED.
        //
        // Everything above captures the right bytes on llvmpipe and lavapipe whether the
        // demotion ran or not - these machines host the built-in - so those cases pin that
        // the demotion does no HARM and can say nothing about whether it happened. The
        // arming is where the cheap mistake lives: MOBILEGL_POINT_SIZE_DEMOTION maps onto
        // the two Supports*PointSize capability bits INVERTED (forcing the demotion on
        // means declaring the built-in UNHOSTED), and a swap of those arms - or a dropped
        // env bit anywhere between ConfigLoader, the backend init, CompileEnv and the L1
        // key - would disable the device repair with every rendering case still green.
        //
        // Same machinery as UnlocatedIoBlockScenario's arming case: the environment says
        // the demotion is pinned on, therefore the library must SAY it demoted something.
        // The observable is the latched MGLOG_I each backend emits when it first builds a
        // demoted program; both spell "demoted to an ordinary varying", so this one case
        // covers both pinned lanes without a backend gate.
        TEST_F(PointSizeDemotionScenario, TheDemotionIsActuallyArmedWhenTheEnvironmentPinsItOn) {
            if (!Ready()) return;
            if (AmbientQuirkFromEnvironment("MOBILEGL_POINT_SIZE_DEMOTION") != AmbientQuirk::On) {
                GTEST_SKIP() << "this case needs the demotion pinned ON for the whole process, which "
                                "is what the PointSizeDemotion. ctest entries do with "
                                "MOBILEGL_POINT_SIZE_DEMOTION=1; with the variable unset the detected "
                                "capabilities decide, and on this machine the built-in is hosted - so "
                                "there would be nothing to observe";
            }
            if (LibraryLogPath().empty()) {
                GTEST_SKIP() << "MOBILEGL_POINT_SIZE_DEMOTION is pinned on but MOBILEGL_LOG_FILE_PATH "
                                "is not set, so the library has nowhere to record that it demoted "
                                "anything; the PointSizeDemotion. ctest entries set both";
            }

            // Taken BEFORE the program is built, so the line this looks for can only be one
            // this process wrote.
            const std::uintmax_t before = LibraryLogSize();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kPointVertexSource},
                                                        {GL_GEOMETRY_SHADER, kPointGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;
            // Drawn as well as built, so a stack that defers its backend program to first
            // use still reaches the build the latched line fires in - and the capture must
            // STILL be right through the carrier.
            const std::vector<float> captured = RunCaptureSpan(program, GL_POINTS, 1, 1);
            EXPECT_TRUE(ComponentIs(captured, 0, 8.0f))
                << "the pinned-on lane did not even capture correctly";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);

            const std::string appended = LibraryLogSince(before);
            EXPECT_NE(appended.find("demoted to an ordinary varying"), std::string::npos)
                << "MOBILEGL_POINT_SIZE_DEMOTION is pinned ON, a geometry program reading and "
                   "writing gl_PointSize was built and captured, and no backend ever reported "
                   "demoting it. The demotion is not armed - check the override mapping in the "
                   "backend inits (it is inverted on purpose), the CompileEnv accessors, and "
                   "ProgramSpirvTask's verdict plumbing. Log appended by this test:\n"
                << appended;
        }

    } // namespace
} // namespace MGITest
