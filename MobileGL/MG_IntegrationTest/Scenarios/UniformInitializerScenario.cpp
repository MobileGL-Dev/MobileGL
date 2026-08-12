// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UniformInitializerScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A DEFAULT-BLOCK UNIFORM'S DECLARED INITIALIZER.
//
// Desktop GLSL has allowed "uniform int i = 1;" since 1.20, and the initializer is not a
// suggestion: it is the value the uniform reads until the application calls glUniform*, and
// the value it goes back to after every relink. Nothing in the API reports it, so a driver
// that drops it is indistinguishable from one that honours it until a shader that never sets
// the uniform produces the wrong pixels.
//
// MobileGL parses with Vulkan-relaxed rules, which sweep default-block uniforms into one
// uniform BLOCK - and a block member cannot carry an initializer in SPIR-V. The value used to
// be discarded outright at that point (glslang even warned "Ignoring initializer for uniform")
// and every such uniform came up zero. That is not a corner case: a large share of
// KHR-GL43.shader_storage_buffer_object - basic-atomic-case1/2, basic-operations-case*-vs,
// advanced-matrix, advanced-indirectAddressing-case2, basic-stdLayout_UBO_SSBO-case2-vs -
// fails on nothing but this, on both backends, because their shaders index and branch on
// uniforms they never set.
//
// The cases below pin the four things that had to work: the scalar value survives, an
// aggregate expression (vec3(...), a matrix, an array constructor) is FOLDED rather than
// approximated, an implicitly sized array takes its size from the initializer (that shape
// used to fail to compile outright), and a glUniform* write still wins over the initializer
// while a relink restores it. Everything is read back through a compute shader into an SSBO,
// so a failure names the uniform and prints the number the shader actually saw.

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

        // Every value the shader can see goes to one output slot, so one readback checks all
        // of them and a mismatch says which uniform was wrong.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
uniform int   g_scalar   = 7;
uniform vec3  g_vector   = vec3(10.0, 20.0, 30.0);
uniform mat3  g_matrix   = mat3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);
uniform int   g_array[]  = int[](11, 22, 33, 44);
uniform uint  g_unsigned = 3u;
uniform bool  g_flag     = true;
layout(std430, binding = 0) buffer Output {
    int g_out[];
};
void main() {
    g_out[0] = g_scalar;
    g_out[1] = int(g_vector.x);
    g_out[2] = int(g_vector.y);
    g_out[3] = int(g_vector.z);
    // Column-major: [column][row]. Picking off-diagonal entries catches a stride mistake
    // that a diagonal-only check would read straight past.
    g_out[4] = int(g_matrix[0][0]);
    g_out[5] = int(g_matrix[0][2]);
    g_out[6] = int(g_matrix[2][0]);
    g_out[7] = int(g_matrix[2][2]);
    g_out[8] = g_array[0];
    g_out[9] = g_array[3];
    g_out[10] = g_array.length();
    g_out[11] = int(g_unsigned);
    g_out[12] = g_flag ? 1 : 0;
}
)";

        constexpr int kOutputSlots = 13;

        class UniformInitializerScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;

                glGenBuffers(1, &m_output);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                const std::vector<int> zeroes(kOutputSlots, 0);
                glBufferData(GL_SHADER_STORAGE_BUFFER, kOutputSlots * sizeof(int), zeroes.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_output);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_output != 0) glDeleteBuffers(1, &m_output);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            unsigned int CompileComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    m_buildLog = std::string("compute program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            std::vector<int> Dispatch() {
                glUseProgram(m_program);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<int> values(kOutputSlots, -1);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kOutputSlots * sizeof(int), values.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glUseProgram(0);
                return values;
            }

            unsigned int m_program = 0;
            unsigned int m_output = 0;
            std::string m_buildLog;
        };

        TEST_F(UniformInitializerScenario, AnUnsetUniformReadsItsDeclaredInitializer) {
            if (!Ready()) return;
            const std::vector<int> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            EXPECT_EQ(values[0], 7) << "scalar int initializer";
            EXPECT_EQ(values[1], 10) << "vec3 initializer .x";
            EXPECT_EQ(values[2], 20) << "vec3 initializer .y";
            EXPECT_EQ(values[3], 30) << "vec3 initializer .z";
            EXPECT_EQ(values[4], 1) << "mat3 initializer [0][0]";
            EXPECT_EQ(values[5], 3) << "mat3 initializer [0][2] - column stride";
            EXPECT_EQ(values[6], 7) << "mat3 initializer [2][0] - column stride";
            EXPECT_EQ(values[7], 9) << "mat3 initializer [2][2]";
            EXPECT_EQ(values[8], 11) << "array initializer element 0";
            EXPECT_EQ(values[9], 44) << "array initializer element 3";
            EXPECT_EQ(values[10], 4) << "implicitly sized array took its size from the initializer";
            EXPECT_EQ(values[11], 3) << "uint initializer";
            EXPECT_EQ(values[12], 1) << "bool initializer";
        }

        TEST_F(UniformInitializerScenario, AnApplicationWriteBeatsTheInitializer) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint scalar = glGetUniformLocation(m_program, "g_scalar");
            const GLint vector = glGetUniformLocation(m_program, "g_vector");
            const GLint element = glGetUniformLocation(m_program, "g_array[3]");
            ASSERT_GE(scalar, 0);
            ASSERT_GE(vector, 0);
            ASSERT_GE(element, 0);
            glUniform1i(scalar, 99);
            const float replacement[3] = {1.0f, 2.0f, 3.0f};
            glUniform3fv(vector, 1, replacement);
            glUniform1i(element, 55);
            glUseProgram(0);

            const std::vector<int> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(values[0], 99);
            EXPECT_EQ(values[1], 1);
            EXPECT_EQ(values[3], 3);
            EXPECT_EQ(values[9], 55);
            // Untouched uniforms keep their initializers - a seed that only worked when
            // nothing else was written would pass the first case and still be wrong here.
            EXPECT_EQ(values[8], 11);
            EXPECT_EQ(values[11], 3);
        }

        TEST_F(UniformInitializerScenario, RelinkingRestoresTheInitializer) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint scalar = glGetUniformLocation(m_program, "g_scalar");
            ASSERT_GE(scalar, 0);
            glUniform1i(scalar, 1234);
            glUseProgram(0);
            ASSERT_EQ(Dispatch()[0], 1234);

            glLinkProgram(m_program);
            GLint linked = 0;
            glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE);

            const std::vector<int> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(values[0], 7) << "a relink puts every uniform back to its initializer";
            EXPECT_EQ(values[1], 10);
        }

    } // namespace
} // namespace MGITest
