// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UnwrittenPositionOutputScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A SHADER REDECLARES gl_PerVertex AND NEVER WRITES gl_Position.
//
// Legal, ordinary GLSL, and until now a process kill on DirectVulkan. The chain, all of it
// inside MobileGL's own SPIR-V plumbing:
//
//   1. glslang emits every DECLARED interface variable, used or not, and lists it on
//      OpEntryPoint. So `out gl_PerVertex { vec4 gl_Position; };` with no write still produces
//      the OpVariable, the OpMemberDecorate BuiltIn Position, and an interface slot.
//   2. At link, ShaderCompiler::SanitizeAndOptimizeBinary runs AggressiveDCE(remove_outputs =
//      false) - which may never delete an Output - and then RemoveUnusedInterfaceVariables,
//      which rebuilds the interface list from the variables instructions actually reference.
//      The OpVariable and its BuiltIn decoration SURVIVE; the interface slot is DELISTED.
//   3. At pipeline build, ProgramFactory picks the last pre-rasterisation stage and runs two
//      passes over it. GlToVulkanPositionFixPass finds the position target through the
//      surviving ANNOTATION and injects a load-modify-STORE through it. When gl_Position is in
//      the transform-feedback capture list, XfbCaptureDecoratePass::MirrorPositionForCapture
//      also injects an access chain and a LOAD through it.
//   4. Either injection is a static use of a variable that is no longer on the entry point's
//      interface, which is invalid SPIR-V ("Interface variable id <N> is used by entry point
//      'main' id <M>, but is not listed as an interface"). Mali r54 does not reject such a
//      module - it faults inside pipeline creation and takes the process down.
//
// Measured on a Mali-G1-Ultra as 216 KHR-GL44/45/46.tessellation_shader.tessellation_control_
// to_tessellation_evaluation.gl_MaxPatchVertices_Position_PointSize_* crashes; the CTS's TES
// there is exactly the shape below. It is not tessellation-specific and not XFB-specific: a
// vertex shader is enough, which is what these cases use.
//
// Every test captures a USER varying through transform feedback under GL_RASTERIZER_DISCARD.
// Position is undefined in the first two by construction, so it is never asserted on - what is
// asserted is that the capture came back at all, which it can only do if the driver accepted
// the module and built a pipeline.

#include <cstddef>
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

        constexpr std::size_t kCaptureFloats = 4;
        constexpr GLsizeiptr kCaptureBytes = static_cast<GLsizeiptr>(kCaptureFloats * sizeof(float));

        // The defect's shape: gl_PerVertex redeclared, gl_Position never assigned.
        constexpr const char* kUnwrittenPositionVertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_value;
out gl_PerVertex {
    vec4 gl_Position;
};
out vec4 vs_out_value;
void main() {
  vs_out_value = vs_in_value;
}
)";

        // The control that isolates the redeclaration: identical but for the one assignment.
        // This one keeps its interface slot through the sanitize chain, so both injections were
        // always legal on it - it must stay working.
        constexpr const char* kWrittenPositionVertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_value;
out gl_PerVertex {
    vec4 gl_Position;
};
out vec4 vs_out_value;
void main() {
  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
  vs_out_value = vs_in_value;
}
)";

        // The second control, and the one the CTS calls data_pass_through: no gl_PerVertex
        // redeclaration at all, so there is no Position annotation for the passes to find and
        // nothing to delist. It was never affected and proves the crash needs the redeclaration.
        constexpr const char* kNoPositionBlockVertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_value;
out vec4 vs_out_value;
void main() {
  vs_out_value = vs_in_value;
}
)";

        GLuint CompileVertexShader(const std::string& source, std::string* log) {
            const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
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

        // `captureNames` is what goes to glTransformFeedbackVaryings. Passing gl_Position in it
        // is what puts MirrorPositionForCapture on the path.
        GLuint BuildCaptureProgram(const char* vertexSource, const std::vector<const char*>& captureNames,
                                   std::string* log) {
            const GLuint vertexShader = CompileVertexShader(vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glTransformFeedbackVaryings(program, static_cast<GLsizei>(captureNames.size()), captureNames.data(),
                                        GL_INTERLEAVED_ATTRIBS);
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

        class UnwrittenPositionOutputScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                const float vertex[kCaptureFloats] = {1.0f, 2.0f, 3.0f, 4.0f};
                glBufferData(GL_ARRAY_BUFFER, kCaptureBytes, vertex, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindVertexArray(0);
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                glUseProgram(0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                ScenarioTest::TearDown();
            }

            // Links `vertexSource` with `captureNames`, runs one captured point, and checks that
            // the USER varying came back. `captureStride` is how many floats one captured vertex
            // occupies, so the user varying can be read out from behind a captured gl_Position.
            void ExpectUserVaryingIsCaptured(const char* vertexSource, const std::vector<const char*>& captureNames,
                                             std::size_t captureStride, std::size_t userVaryingOffset,
                                             const char* what) {
                std::string log;
                const GLuint program = BuildCaptureProgram(vertexSource, captureNames, &log);
                ASSERT_NE(program, 0u) << what << ": the capture program failed to build: " << log;

                const GLsizeiptr captureBytes = static_cast<GLsizeiptr>(captureStride * sizeof(float));
                GLuint xfbBuffer = 0;
                glGenBuffers(1, &xfbBuffer);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfbBuffer);
                // Pre-fill with a value the shader cannot produce, so "captured nothing" is
                // distinguishable from "captured the wrong thing".
                const std::vector<float> poison(captureStride, -1.0f);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, captureBytes, poison.data(), GL_DYNAMIC_READ);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
                ASSERT_EQ(FirstGLError(), 0u) << what << ": setting up the capture buffer raised a GL error";

                glEnable(GL_RASTERIZER_DISCARD);
                glUseProgram(program);
                glBindVertexArray(m_vao);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(GL_POINTS, 0, 1);
                glEndTransformFeedback();
                glBindVertexArray(0);
                glUseProgram(0);
                glDisable(GL_RASTERIZER_DISCARD);
                EXPECT_EQ(FirstGLError(), 0u) << what << ": the captured draw raised a GL error";

                std::vector<float> readback(captureStride, -2.0f);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfbBuffer);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, captureBytes, readback.data());
                for (std::size_t i = 0; i < kCaptureFloats; ++i) {
                    EXPECT_FLOAT_EQ(readback[userVaryingOffset + i], static_cast<float>(i + 1))
                        << what << ": captured float " << i << " came back as "
                        << readback[userVaryingOffset + i]
                        << "; the pre-fill value means the draw never produced a vertex, which is what an "
                           "invalid shader module looks like from out here";
                }

                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
                glDeleteBuffers(1, &xfbBuffer);
                glDeleteProgram(program);
            }

            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

    } // namespace

    // The clip fixup's half: PositionZRemap is on for every draw, so the fixup runs on this
    // program and used to inject a store through the delisted block.
    TEST_F(UnwrittenPositionOutputScenario, ARedeclaredButUnwrittenPositionStillDraws) {
        if (!Ready() || IsSkipped()) return;
        ExpectUserVaryingIsCaptured(kUnwrittenPositionVertexSource, {"vs_out_value"}, kCaptureFloats, 0,
                                    "redeclared, never written");
    }

    // The XFB half: capturing gl_Position adds an access chain and a LOAD through the same
    // delisted block, which the interface rule covers exactly as it covers the store. Position
    // itself is undefined here - only the user varying behind it is asserted.
    TEST_F(UnwrittenPositionOutputScenario, CapturingAnUnwrittenPositionStillDraws) {
        if (!Ready() || IsSkipped()) return;
        // DirectVulkan only, and not because the defect was backend-specific in principle - the
        // injection this pins lives in DirectVulkan's ProgramFactory, and DirectGLES cannot
        // reach the case at all: capturing gl_Position BY NAME off a shader that never writes it
        // comes back empty there, because the ESSL the transpiler emits has no such output for
        // the capture list to name. That is a known, separate DirectGLES gap (the same one that
        // blocks gl_Position/gl_PointSize capture in the tessellation capture segment), tracked
        // outside this scenario; asserting it here would only re-report it.
        if (Gl().BackendName() != "DirectVulkan") {
            GTEST_SKIP() << "capturing an unwritten gl_Position by name is a separate, known "
                         << "DirectGLES gap; this case pins the DirectVulkan injection";
        }
        ExpectUserVaryingIsCaptured(kUnwrittenPositionVertexSource, {"gl_Position", "vs_out_value"},
                                    kCaptureFloats * 2, kCaptureFloats, "capturing an unwritten gl_Position");
    }

    // Control: the same shader with the one assignment restored. Its block is never delisted,
    // so it exercises the path the fixup is actually for and must keep working.
    TEST_F(UnwrittenPositionOutputScenario, AWrittenRedeclaredPositionStillDraws) {
        if (!Ready() || IsSkipped()) return;
        ExpectUserVaryingIsCaptured(kWrittenPositionVertexSource, {"vs_out_value"}, kCaptureFloats, 0,
                                    "redeclared and written");
    }

    TEST_F(UnwrittenPositionOutputScenario, CapturingAWrittenPositionStillDraws) {
        if (!Ready() || IsSkipped()) return;
        ExpectUserVaryingIsCaptured(kWrittenPositionVertexSource, {"gl_Position", "vs_out_value"},
                                    kCaptureFloats * 2, kCaptureFloats, "capturing a written gl_Position");
    }

    // Control: no gl_PerVertex redeclaration, so no Position annotation and nothing to delist.
    TEST_F(UnwrittenPositionOutputScenario, AShaderWithNoPositionBlockStillDraws) {
        if (!Ready() || IsSkipped()) return;
        ExpectUserVaryingIsCaptured(kNoPositionBlockVertexSource, {"vs_out_value"}, kCaptureFloats, 0,
                                    "no gl_PerVertex block");
    }

} // namespace MGITest
