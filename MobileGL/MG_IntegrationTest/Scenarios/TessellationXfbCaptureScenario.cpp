// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TessellationXfbCaptureScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - WHAT A TESSELLATION EVALUATION STAGE OWES A TRANSFORM FEEDBACK CAPTURE.
//
// XfbRepeatedCaptureScenario already pins that a capture from a GL_PATCHES draw records
// AT ALL. Everything below is the part of the same pipeline it does not reach, and every
// case here is the reduced form of a conformance body that fails on a device:
//
//   * CAPTURING THE BUILT-INS BY NAME. glTransformFeedbackVaryings("gl_Position") /
//     ("gl_PointSize") on a program whose last vertex-processing stage is the evaluation
//     shader. Nothing in the tree captured a built-in from a tessellation stage, and the
//     two backends reach it by completely different routes - DirectGLES has to name a
//     real ESSL output on the driver's own glTransformFeedbackVaryings, DirectVulkan has
//     to decorate a SPIR-V built-in that lives inside gl_PerVertex.
//
//   * THE PER-VERTEX PAYLOAD THE CONTROL STAGE HANDS OVER. gl_PointSize and a
//     user-declared per-vertex interface block, both read back out of gl_in[] by the
//     evaluation stage and only then captured. This is the shape of
//     KHR-GL4x.tessellation_shader.tessellation_control_to_tessellation_evaluation.
//     gl_MaxPatchVertices_Position_PointSize, which is 216 of the ~240 conformance bodies
//     the family still fails: gl_Position arrives, and everything travelling beside it in
//     the same patch does not.
//
// The assertions are on the captured BYTES against a CPU-computed reference, never on the
// absence of a GL error: every failure this guards against is silent.

#include <cmath>
#include <cstring>
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
        // names the failure instead of looking like an ordinary numeric mismatch.
        constexpr float kPoison = -987654.0f;

        const char* const kFragmentSource = R"(#version 420 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

        class TessellationXfbCaptureScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
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
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                m_vao = 0;
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

            static GLint MaxPatchVertices() {
                GLint value = 0;
                glGetIntegerv(GL_MAX_PATCH_VERTICES, &value);
                DrainErrors();
                return value;
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

            GLuint BuildCaptureProgram(const std::vector<std::pair<GLenum, std::string>>& stages,
                                       const std::vector<const char*>& varyings) {
                m_buildLog.clear();
                std::vector<GLuint> shaders;
                bool ok = true;
                for (const auto& [stage, source] : stages) {
                    const GLuint shader = glCreateShader(stage);
                    const char* text = source.c_str();
                    glShaderSource(shader, 1, &text, nullptr);
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

            // One capture span over a single patch. Returns the capture buffer read back as
            // floats; `capturedFloats` is the whole buffer, poison-filled beforehand.
            std::vector<float> RunPatchCaptureSpan(GLuint program, GLenum captureMode, std::size_t capturedFloats) {
                const std::vector<float> poison(capturedFloats, kPoison);
                GLuint xfbBuffer = 0;
                glGenBuffers(1, &xfbBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, xfbBuffer);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capturedFloats * sizeof(float)), poison.data(),
                             GL_STATIC_COPY);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

                glBindVertexArray(m_vao);
                glUseProgram(program);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(captureMode);
                glDrawArrays(GL_PATCHES, 0, 1);
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);

                std::vector<float> readback(capturedFloats, kPoison);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                                   static_cast<GLsizeiptr>(capturedFloats * sizeof(float)), readback.data());
                glUseProgram(0);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
                glDeleteBuffers(1, &xfbBuffer);
                return readback;
            }

            static ::testing::AssertionResult ComponentIs(const std::vector<float>& data, std::size_t index,
                                                          float expected, float epsilon = 1e-4f) {
                if (index >= data.size()) {
                    return ::testing::AssertionFailure() << "component " << index << " is past the capture buffer";
                }
                const float actual = data[index];
                if (actual == kPoison) {
                    return ::testing::AssertionFailure()
                           << "component " << index << " still holds the poison value - the capture never reached "
                           << "these bytes (expected " << expected << ")";
                }
                if (std::isnan(actual) || std::abs(actual - expected) > epsilon) {
                    return ::testing::AssertionFailure()
                           << "component " << index << " is " << actual << ", expected " << expected;
                }
                return ::testing::AssertionSuccess();
            }

            // Defined below the shader builders it uses. `withPointSize` is the conformance
            // body's own should_pass_pointsize_data axis.
            void RunPerVertexPayloadCase(bool withPointSize);

            // Why the gl_PointSize cases cannot be run here, or empty when they can.
            //
            // gl_PointSize from a tessellation stage is a real DRIVER capability on both
            // targets - GL_EXT/OES_tessellation_point_size on an ES driver, the
            // shaderTessellationAndGeometryPointSize feature on a Vulkan device - and desktop GL
            // has no query that reports either, so this probes for it by running a program.
            //
            // The probe is deliberately NOT a gl_PointSize capture: it captures an ordinary user
            // varying out of a tessellation evaluation stage that ALSO writes gl_PointSize, and
            // compares that against the identical program without the write. A backend that
            // cannot express the built-in loses the whole stage (DirectGLES fails to compile it
            // and binds program 0; DirectVulkan cannot build the pipeline), so the plain varying
            // comes back untouched too - which is a capability answer, not a capture answer. If
            // BOTH come back untouched the probe itself is meaningless and it returns empty, so
            // the cases run and FAIL rather than skipping on an unrelated breakage.
            //
            // Returns the reason as a string instead of skipping directly: GTEST_SKIP expands to
            // a `return`, so a void helper would leave only the helper and let the case run its
            // assertions anyway and report Failed instead of Skipped.
            std::string WhyPointSizeCasesCannotRun();

            std::vector<GLuint> m_programs;
            std::string m_buildLog;
            GLuint m_vao = 0;
        };

        // ---------------------------------------------------------------------------------
        // Built-ins captured BY NAME from the evaluation stage.
        // ---------------------------------------------------------------------------------

        const char* const kMinimalVertexSource = R"(#version 420 core
void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kMinimalTessControlSource = R"(#version 420 core
layout(vertices = 1) out;
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[0].gl_Position;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

        // Values no stale buffer would hold by accident. The two sources differ ONLY by
        // gl_PointSize, so the pair isolates it: on a backend that lowers to ESSL the
        // built-in is not even declared in a tessellation stage without
        // GL_EXT_tessellation_point_size, and the whole shader then fails to compile.
        const char* const kPositionTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw, point_mode) in;
void main()
{
    gl_Position  = vec4(11.0, 12.0, 13.0, 14.0);
}
)";

        const char* const kPositionAndPointSizeTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw, point_mode) in;
void main()
{
    gl_Position  = vec4(11.0, 12.0, 13.0, 14.0);
    gl_PointSize = 5.0;
}
)";

        // The two probe programs. They differ by one statement; both capture `probe_value`,
        // which has nothing to do with point size.
        const char* const kPointSizeProbeTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw, point_mode) in;
out float probe_value;
void main()
{
    probe_value  = 42.0;
    gl_Position  = vec4(0.0, 0.0, 0.0, 1.0);
    gl_PointSize = 3.0;
}
)";

        const char* const kPointSizeFreeProbeTessEvalSource = R"(#version 420 core
layout(triangles, equal_spacing, cw, point_mode) in;
out float probe_value;
void main()
{
    probe_value = 42.0;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        std::string TessellationXfbCaptureScenario::WhyPointSizeCasesCannotRun() {
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            const auto probeCaptures = [&](const char* tessEvalSource) {
                const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kMinimalVertexSource},
                                                            {GL_TESS_CONTROL_SHADER, kMinimalTessControlSource},
                                                            {GL_TESS_EVALUATION_SHADER, tessEvalSource},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                           {"probe_value"});
                if (program == 0) return false;
                const std::vector<float> captured = RunPatchCaptureSpan(program, GL_POINTS, 3);
                DrainErrors();
                return captured[0] == 42.0f;
            };

            const bool withPointSize = probeCaptures(kPointSizeProbeTessEvalSource);
            if (withPointSize) return {};
            if (!probeCaptures(kPointSizeFreeProbeTessEvalSource)) {
                // The control failed too, so nothing here is about point size.
                return {};
            }
            return "this backend cannot express gl_PointSize in a tessellation stage at all - the same "
                   "program captures an ordinary varying with the gl_PointSize write removed and captures "
                   "nothing with it present (an ES driver without GL_EXT/OES_tessellation_point_size, or a "
                   "Vulkan device without shaderTessellationAndGeometryPointSize)";
        }

        TEST_F(TessellationXfbCaptureScenario, CapturesGlPositionByNameFromTheEvaluationStage) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kMinimalVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kMinimalTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kPositionTessEvalSource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_Position"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            // point_mode with every level at 1 emits three points, all carrying the same
            // constant; only the first record has to be right for the mechanism to be proven.
            const std::vector<float> captured = RunPatchCaptureSpan(program, GL_POINTS, 4 * 3);
            EXPECT_TRUE(ComponentIs(captured, 0, 11.0f));
            EXPECT_TRUE(ComponentIs(captured, 1, 12.0f));
            EXPECT_TRUE(ComponentIs(captured, 2, 13.0f));
            EXPECT_TRUE(ComponentIs(captured, 3, 14.0f));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        TEST_F(TessellationXfbCaptureScenario, CapturesGlPositionAndGlPointSizeByNameFromTheEvaluationStage) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }
            if (const std::string reason = WhyPointSizeCasesCannotRun(); !reason.empty()) GTEST_SKIP() << reason;
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kMinimalVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kMinimalTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kPositionAndPointSizeTessEvalSource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_Position", "gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            const std::vector<float> captured = RunPatchCaptureSpan(program, GL_POINTS, 5 * 3);
            EXPECT_TRUE(ComponentIs(captured, 0, 11.0f));
            EXPECT_TRUE(ComponentIs(captured, 1, 12.0f));
            EXPECT_TRUE(ComponentIs(captured, 2, 13.0f));
            EXPECT_TRUE(ComponentIs(captured, 3, 14.0f));
            EXPECT_TRUE(ComponentIs(captured, 4, 5.0f));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // ---------------------------------------------------------------------------------
        // The per-vertex payload the control stage hands to the evaluation stage.
        // ---------------------------------------------------------------------------------

        // The conformance body's own shapes, reduced to one patch and parameterised by the
        // output patch size so the caller can run the real GL_MAX_PATCH_VERTICES. The
        // `withPointSize` axis is the conformance body's own `should_pass_pointsize_data`,
        // which it varies together with point_mode - and which decides whether the whole
        // program even involves the per-vertex built-in that ESSL gates behind an extension.
        std::string PayloadVertexSource(bool withPointSize) {
            return R"(#version 420 core
out gl_PerVertex {
    vec4  gl_Position;
)" + std::string(withPointSize ? "    float gl_PointSize;\n" : "") +
                   R"(};
void main()
{
}
)";
        }

        std::string PayloadTessControlSource(int outputVertices, bool withPointSize) {
            const std::string perVertexTail = withPointSize ? "    float gl_PointSize;\n" : "";
            return R"(#version 420 core
layout(vertices = )" + std::to_string(outputVertices) +
                   R"() out;
in gl_PerVertex {
    vec4  gl_Position;
)" + perVertexTail +
                   R"(} gl_in[gl_MaxPatchVertices];
out gl_PerVertex {
    vec4  gl_Position;
)" + perVertexTail +
                   R"(} gl_out[];
out OUT_TC
{
     vec2 value1;
    ivec4 value2;
} result[];
void main()
{
)" + std::string(withPointSize
                                        ? "    gl_out[gl_InvocationID].gl_PointSize = 1.0 / float(gl_InvocationID + 1);\n"
                                        : "") +
                   R"(    gl_out[gl_InvocationID].gl_Position  = vec4(float(gl_InvocationID * 4 + 0), float(gl_InvocationID * 4 + 1),
                                                float(gl_InvocationID * 4 + 2), float(gl_InvocationID * 4 + 3));
    result[gl_InvocationID].value1       = vec2(1.0 / float(gl_InvocationID + 1), 1.0 / float(gl_InvocationID + 2));
    result[gl_InvocationID].value2       = ivec4(gl_InvocationID + 1, gl_InvocationID + 2,
                                                 gl_InvocationID + 3, gl_InvocationID + 4);
    gl_TessLevelInner[0] = 1.0;
    gl_TessLevelInner[1] = 1.0;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelOuter[3] = 1.0;
}
)";
        }

        // Deliberately NEVER writes gl_Position, exactly as the conformance shader does not:
        // the redeclared block is there so the evaluation stage can READ gl_in[], and an
        // output nothing stores is what UnwrittenPositionOutputScenario pins separately.
        std::string PayloadTessEvalSource(int inputVertices, bool withPointSize) {
            const std::string perVertexTail = withPointSize ? "    float gl_PointSize;\n" : "";
            return R"(#version 420 core
layout(isolines, equal_spacing, ccw, point_mode) in;
in gl_PerVertex {
    vec4  gl_Position;
)" + perVertexTail +
                   R"(} gl_in[gl_MaxPatchVertices];
out gl_PerVertex {
    vec4  gl_Position;
)" + perVertexTail +
                   R"(};
in OUT_TC
{
     vec2 value1;
    ivec4 value2;
} tc_data[];

)" + std::string(withPointSize ? "out      float te_pointsize;\n" : "") +
                   R"(out       vec4 te_position;
out       vec2 te_value1;
out flat ivec4 te_value2;

void main()
{
)" + std::string(withPointSize ? "    te_pointsize = 0.0;\n" : "") +
                   R"(    te_position  = vec4 (0.0);
    te_value1    = vec2 (0.0);
    te_value2    = ivec4(0);

    for (int n = 0; n < )" + std::to_string(inputVertices) +
                   R"(; ++n)
    {
)" + std::string(withPointSize ? "        te_pointsize += gl_in  [n].gl_PointSize;\n" : "") +
                   R"(        te_position  += gl_in  [n].gl_Position;
        te_value1    += tc_data[n].value1;
        te_value2    += tc_data[n].value2;
    }
}
)";
        }

        // The reduced conformance body. `withPointSize` selects between its two halves;
        // everything else - one input vertex, an output patch of GL_MAX_PATCH_VERTICES, a
        // user per-vertex block travelling beside gl_PerVertex, the capture taken off the
        // evaluation stage - is the same on both.
        void TessellationXfbCaptureScenario::RunPerVertexPayloadCase(bool withPointSize) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }
            if (withPointSize) {
                if (const std::string reason = WhyPointSizeCasesCannotRun(); !reason.empty()) GTEST_SKIP() << reason;
            }
            const GLint patchVertices = MaxPatchVertices();
            ASSERT_GE(patchVertices, 32) << "GL_MAX_PATCH_VERTICES is below the guaranteed minimum";

            // One input vertex per patch, an output patch of GL_MAX_PATCH_VERTICES vertices:
            // the control stage runs that many invocations and every one of them contributes.
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            std::vector<const char*> varyings = {"te_position", "te_value1", "te_value2"};
            if (withPointSize) varyings.push_back("te_pointsize");

            const GLuint program =
                BuildCaptureProgram({{GL_VERTEX_SHADER, PayloadVertexSource(withPointSize)},
                                     {GL_TESS_CONTROL_SHADER, PayloadTessControlSource(patchVertices, withPointSize)},
                                     {GL_TESS_EVALUATION_SHADER, PayloadTessEvalSource(patchVertices, withPointSize)},
                                     {GL_FRAGMENT_SHADER, kFragmentSource}},
                                    varyings);
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            float referencePointSize = 0.0f;
            float referencePosition[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float referenceValue1[2] = {0.0f, 0.0f};
            int referenceValue2[4] = {0, 0, 0, 0};
            for (int n = 0; n < patchVertices; ++n) {
                referencePointSize += 1.0f / static_cast<float>(n + 1);
                for (int c = 0; c < 4; ++c) {
                    referencePosition[c] += static_cast<float>(n * 4 + c);
                    referenceValue2[c] += n + 1 + c;
                }
                referenceValue1[0] += 1.0f / static_cast<float>(n + 1);
                referenceValue1[1] += 1.0f / static_cast<float>(n + 2);
            }

            // isolines with every level at 1 emits two points; the record stride is
            // vec4 + vec2 + ivec4 [+ float] components.
            const std::size_t stride = withPointSize ? 11 : 10;
            const std::vector<float> captured = RunPatchCaptureSpan(program, GL_POINTS, stride * 4);
            for (int c = 0; c < 4; ++c) {
                EXPECT_TRUE(ComponentIs(captured, static_cast<std::size_t>(c), referencePosition[c], 1e-2f))
                    << "te_position." << c << " (gl_in[].gl_Position)";
            }
            for (int c = 0; c < 2; ++c) {
                EXPECT_TRUE(ComponentIs(captured, static_cast<std::size_t>(4 + c), referenceValue1[c], 1e-3f))
                    << "te_value1." << c << " (the user per-vertex block the control stage wrote)";
            }
            for (int c = 0; c < 4; ++c) {
                const std::size_t index = static_cast<std::size_t>(6 + c);
                ASSERT_LT(index, captured.size());
                int actual = 0;
                std::memcpy(&actual, &captured[index], sizeof(actual));
                EXPECT_EQ(actual, referenceValue2[c])
                    << "te_value2." << c << " (the user per-vertex block's integer member)";
            }
            if (withPointSize) {
                EXPECT_TRUE(ComponentIs(captured, 10, referencePointSize, 1e-3f))
                    << "te_pointsize (gl_in[].gl_PointSize)";
            }
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        TEST_F(TessellationXfbCaptureScenario, TheEvaluationStageSeesTheUserPerVertexBlockOfItsPatch) {
            RunPerVertexPayloadCase(false);
        }

        // ---------------------------------------------------------------------------------
        // The same built-in, one stage over.
        // ---------------------------------------------------------------------------------

        // ESSL gates gl_PointSize behind a per-stage extension in BOTH non-vertex
        // vertex-processing stages - EXT/OES_tessellation_point_size for the two tessellation
        // stages, EXT/OES_geometry_point_size for the geometry one - and they are separate
        // extensions that do not imply each other, so the geometry arm is a second code path
        // rather than the same one. Nothing else in the tree writes gl_PointSize from a geometry
        // shader, so without this case the arm ships untested.
        const char* const kPointSizeGeometrySource = R"(#version 420 core
layout(points) in;
layout(points, max_vertices = 1) out;
out float gs_value;
void main()
{
    gs_value     = 7.0;
    gl_Position  = gl_in[0].gl_Position;
    gl_PointSize = 4.0;
    EmitVertex();
}
)";

        TEST_F(TessellationXfbCaptureScenario, CapturesGlPointSizeByNameFromTheGeometryStage) {
            if (!Ready()) GTEST_SKIP();
            GLint maxGeometryOutputVertices = 0;
            glGetIntegerv(GL_MAX_GEOMETRY_OUTPUT_VERTICES, &maxGeometryOutputVertices);
            DrainErrors();
            if (maxGeometryOutputVertices < 1) {
                GTEST_SKIP() << "no geometry stage on " << Gl().BackendName() << " (" << Gl().RendererString() << ")";
            }

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kMinimalVertexSource},
                                                        {GL_GEOMETRY_SHADER, kPointSizeGeometrySource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gs_value", "gl_PointSize"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
            const std::vector<float> poison(2, kPoison);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(poison.size() * sizeof(float)),
                         poison.data(), GL_STATIC_DRAW);

            glBindVertexArray(m_vao);
            glUseProgram(program);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, 1);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);

            std::vector<float> captured(2, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               static_cast<GLsizeiptr>(captured.size() * sizeof(float)), captured.data());
            EXPECT_TRUE(ComponentIs(captured, 0, 7.0f)) << "gs_value - an ordinary varying, which is lost too when "
                                                           "the stage carrying it fails to compile";
            EXPECT_TRUE(ComponentIs(captured, 1, 4.0f)) << "gl_PointSize";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);

            glUseProgram(0);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glDeleteBuffers(1, &xfbBuffer);
        }

        // ---------------------------------------------------------------------------------
        // The conformance body's own READBACK, which is not glGetBufferSubData.
        // ---------------------------------------------------------------------------------

        // Every case above reads the capture back with glGetBufferSubData because that is the
        // shortest path to the bytes. The conformance bodies do something else: they respecify
        // the buffer through the GENERIC GL_TRANSFORM_FEEDBACK_BUFFER binding with glBufferData
        // while it is simultaneously bound to indexed capture point 0, and then read it with
        // glMapBufferRange / glUnmapBuffer - twice, once per iteration of the same case, with no
        // fresh buffer in between. On a device the tessellation bodies stop at exactly that map
        // call, so the sequence itself is worth pinning: none of the map path's error conditions
        // may fire, and the mapped bytes must be the captured ones.
        TEST_F(TessellationXfbCaptureScenario, MapsTheCaptureBufferAfterEachOfTwoPatchDraws) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            const GLuint program = BuildCaptureProgram({{GL_VERTEX_SHADER, kMinimalVertexSource},
                                                        {GL_TESS_CONTROL_SHADER, kMinimalTessControlSource},
                                                        {GL_TESS_EVALUATION_SHADER, kPositionTessEvalSource},
                                                        {GL_FRAGMENT_SHADER, kFragmentSource}},
                                                       {"gl_Position"});
            ASSERT_NE(program, 0u) << "program failed to build: " << m_buildLog;

            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "binding the capture point";

            constexpr std::size_t kFloats = 4 * 3;
            constexpr GLsizeiptr kBytes = static_cast<GLsizeiptr>(kFloats * sizeof(float));
            for (int iteration = 0; iteration < 2; ++iteration) {
                // Respecified through the generic binding, exactly as the conformance body does,
                // while the same buffer is still bound to capture point 0.
                const std::vector<float> poison(kFloats, kPoison);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, kBytes, poison.data(), GL_STATIC_DRAW);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glBufferData, iteration " << iteration;

                glBindVertexArray(m_vao);
                glUseProgram(program);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_POINTS);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glBeginTransformFeedback, iteration " << iteration;
                glDrawArrays(GL_PATCHES, 0, 1);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glDrawArrays, iteration " << iteration;
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glEndTransformFeedback, iteration " << iteration;

                const auto* mapped =
                    static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, kBytes,
                                                               GL_MAP_READ_BIT));
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glMapBufferRange, iteration " << iteration;
                ASSERT_NE(mapped, nullptr) << "iteration " << iteration;
                const std::vector<float> captured(mapped, mapped + kFloats);
                EXPECT_EQ(glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER), GL_TRUE) << "iteration " << iteration;
                EXPECT_EQ(glGetError(), GL_NO_ERROR) << "glUnmapBuffer, iteration " << iteration;

                EXPECT_TRUE(ComponentIs(captured, 0, 11.0f)) << "iteration " << iteration;
                EXPECT_TRUE(ComponentIs(captured, 1, 12.0f)) << "iteration " << iteration;
                EXPECT_TRUE(ComponentIs(captured, 2, 13.0f)) << "iteration " << iteration;
                EXPECT_TRUE(ComponentIs(captured, 3, 14.0f)) << "iteration " << iteration;
                glUseProgram(0);
            }

            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glDeleteBuffers(1, &xfbBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The same patch with gl_PointSize travelling in gl_PerVertex beside gl_Position.
        // In ESSL gl_PointSize does not EXIST in a tessellation stage unless
        // GL_EXT_tessellation_point_size is requested, so a backend that lowers to ESSL
        // without asking for it does not merely lose the value - the stage fails to compile
        // and the whole program is replaced by program 0.
        TEST_F(TessellationXfbCaptureScenario, TheEvaluationStageSeesGlPointSizeAcrossItsPatch) {
            RunPerVertexPayloadCase(true);
        }

        // ---------------------------------------------------------------------------------
        // The same capture through a PROGRAM PIPELINE OBJECT.
        // ---------------------------------------------------------------------------------

        // The conformance body runs each of its configurations twice: once with a monolithic
        // program object and once with a pipeline of four separable programs, the capture
        // declared on the separable EVALUATION program. That second shape goes through the
        // hidden composite the pipeline object builds for the draw, and it is the only place a
        // tessellation capture and the composite meet - so the capture list has to survive being
        // taken from a program that is not the one bound.
        TEST_F(TessellationXfbCaptureScenario, CapturesFromASeparableEvaluationProgramInAPipelineObject) {
            if (!Ready()) GTEST_SKIP();
            if (!BackendHostsTessellation()) {
                GTEST_SKIP() << "no tessellation stages on " << Gl().BackendName() << " (" << Gl().RendererString()
                             << ")";
            }
            glPatchParameteri(GL_PATCH_VERTICES, 1);
            DrainErrors();

            // One separable program per stage. Only the evaluation program carries the capture
            // list, because it is the one whose outputs are captured.
            const auto buildSeparable = [&](GLenum stage, const char* source,
                                            const std::vector<const char*>& varyings) -> GLuint {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    m_buildLog = InfoLog(shader, true);
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
                glAttachShader(program, shader);
                if (!varyings.empty()) {
                    glTransformFeedbackVaryings(program, static_cast<GLsizei>(varyings.size()), varyings.data(),
                                                GL_INTERLEAVED_ATTRIBS);
                }
                glLinkProgram(program);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                glDeleteShader(shader);
                if (linked == GL_FALSE) {
                    m_buildLog = InfoLog(program, false);
                    glDeleteProgram(program);
                    return 0;
                }
                m_programs.push_back(program);
                return program;
            };

            m_buildLog.clear();
            const GLuint vertexProgram = buildSeparable(GL_VERTEX_SHADER, kMinimalVertexSource, {});
            ASSERT_NE(vertexProgram, 0u) << "separable vertex program: " << m_buildLog;
            const GLuint controlProgram = buildSeparable(GL_TESS_CONTROL_SHADER, kMinimalTessControlSource, {});
            ASSERT_NE(controlProgram, 0u) << "separable control program: " << m_buildLog;
            const GLuint evalProgram =
                buildSeparable(GL_TESS_EVALUATION_SHADER, kPositionTessEvalSource, {"gl_Position"});
            ASSERT_NE(evalProgram, 0u) << "separable evaluation program: " << m_buildLog;
            const GLuint fragmentProgram = buildSeparable(GL_FRAGMENT_SHADER, kFragmentSource, {});
            ASSERT_NE(fragmentProgram, 0u) << "separable fragment program: " << m_buildLog;

            GLuint pipeline = 0;
            glGenProgramPipelines(1, &pipeline);
            glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vertexProgram);
            glUseProgramStages(pipeline, GL_TESS_CONTROL_SHADER_BIT, controlProgram);
            glUseProgramStages(pipeline, GL_TESS_EVALUATION_SHADER_BIT, evalProgram);
            glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fragmentProgram);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "assembling the pipeline object";

            constexpr std::size_t kFloats = 4 * 3;
            const std::vector<float> poison(kFloats, kPoison);
            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(kFloats * sizeof(float)),
                         poison.data(), GL_STATIC_DRAW);

            glBindVertexArray(m_vao);
            glUseProgram(0);
            glBindProgramPipeline(pipeline);
            glEnable(GL_RASTERIZER_DISCARD);
            glBeginTransformFeedback(GL_POINTS);
            EXPECT_EQ(glGetError(), GL_NO_ERROR) << "glBeginTransformFeedback on a pipeline object";
            glDrawArrays(GL_PATCHES, 0, 1);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);

            std::vector<float> captured(kFloats, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               static_cast<GLsizeiptr>(kFloats * sizeof(float)), captured.data());
            EXPECT_TRUE(ComponentIs(captured, 0, 11.0f));
            EXPECT_TRUE(ComponentIs(captured, 1, 12.0f));
            EXPECT_TRUE(ComponentIs(captured, 2, 13.0f));
            EXPECT_TRUE(ComponentIs(captured, 3, 14.0f));
            EXPECT_EQ(glGetError(), GL_NO_ERROR);

            glBindProgramPipeline(0);
            glDeleteProgramPipelines(1, &pipeline);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
            glDeleteBuffers(1, &xfbBuffer);
        }

    } // namespace
} // namespace MGITest
