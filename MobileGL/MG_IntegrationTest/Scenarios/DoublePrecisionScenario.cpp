// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DoublePrecisionScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GLSL DOUBLES, RUN AT SINGLE PRECISION.
//
// No mobile GPU has 64-bit floats. Adreno and Mali both report shaderFloat64 == VK_FALSE, so
// Magma cannot build a module that declares the Float64 capability, and ESSL has no fp64 type
// at all, so SPIRV-Cross refuses the module outright on Espryt ("FP64 not supported in ES
// profile") and the program never reaches the driver. MobileGL therefore narrows every 64-bit
// float in a shader to 32 bits (ShaderTranspiler::DemoteFloat64Pass) rather than declining the
// shader: `double` compiles and runs everywhere, at float precision.
//
// The narrowing is only half a contract. The other half is the API side: the global UBO is
// laid out by reflecting the DEMOTED module, so glUniform*d has to store a float where the
// shader reads a float, glGetUniform*v has to read one back, and a dmat4's columns are now
// std140-padded like any other matrix's. Every one of those is a byte offset that fails
// silently - the uniform simply reads as something else - so the cases below set values
// through the API and have the SHADER report what it saw.
//
// What is deliberately NOT asserted: that the values are exact to double precision. They are
// not, and cannot be. Every expectation here is the float value of the double that was set,
// which is the whole point.

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

        // Doubles in every shape the demotion has to handle - a scalar, a vector, a matrix
        // whose column stride changes, an array whose element stride changes - all reported
        // through one float SSBO so a single readback says which one moved.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
uniform double uScalar;
uniform dvec3  uVector;
uniform dmat4  uMatrix;
uniform double uArray[3];
layout(std430, binding = 0) buffer Output {
    float g_out[];
};
void main() {
    g_out[0] = float(uScalar);
    g_out[1] = float(uVector.x);
    g_out[2] = float(uVector.y);
    g_out[3] = float(uVector.z);
    // Column-major [column][row]. Off-diagonal entries catch a column-stride mistake that a
    // diagonal-only check reads straight past.
    g_out[4] = float(uMatrix[0][0]);
    g_out[5] = float(uMatrix[0][3]);
    g_out[6] = float(uMatrix[3][0]);
    g_out[7] = float(uMatrix[3][3]);
    g_out[8] = float(uArray[0]);
    g_out[9] = float(uArray[1]);
    g_out[10] = float(uArray[2]);
    // Arithmetic on doubles, including an implicit float->double conversion and a literal
    // with the fp64 suffix: this is what an application actually writes, and it is the part
    // that has to survive the conversion folding.
    double accumulated = uScalar * 2.0lf + 1.5;
    g_out[11] = float(accumulated);
}
)";

        constexpr int kOutputSlots = 12;

        class DoublePrecisionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;

                glGenBuffers(1, &m_output);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                const std::vector<float> zeroes(kOutputSlots, 0.0f);
                glBufferData(GL_SHADER_STORAGE_BUFFER, kOutputSlots * sizeof(float), zeroes.data(),
                             GL_DYNAMIC_DRAW);
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

            std::vector<float> Dispatch() {
                glUseProgram(m_program);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<float> values(kOutputSlots, -1.0f);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kOutputSlots * sizeof(float), values.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glUseProgram(0);
                return values;
            }

            unsigned int m_program = 0;
            unsigned int m_output = 0;
            std::string m_buildLog;
        };

        TEST_F(DoublePrecisionScenario, ADoubleUniformReachesTheShaderAtFloatPrecision) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint scalar = glGetUniformLocation(m_program, "uScalar");
            ASSERT_GE(scalar, 0);
            // 0.1 has no exact float (or double) representation, so this only passes if the
            // value really travelled through the demoted slot rather than being read out of
            // some other four bytes.
            glUniform1d(scalar, 0.1);
            glUseProgram(0);

            const std::vector<float> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_FLOAT_EQ(values[0], static_cast<float>(0.1));
            EXPECT_FLOAT_EQ(values[11], static_cast<float>(static_cast<float>(0.1) * 2.0f + 1.5f))
                << "arithmetic on the demoted value, including the folded fp64 literal";
        }

        TEST_F(DoublePrecisionScenario, EveryDoubleShapeLandsInItsOwnSlot) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint scalar = glGetUniformLocation(m_program, "uScalar");
            const GLint vector = glGetUniformLocation(m_program, "uVector");
            const GLint matrix = glGetUniformLocation(m_program, "uMatrix");
            const GLint array0 = glGetUniformLocation(m_program, "uArray[0]");
            const GLint array2 = glGetUniformLocation(m_program, "uArray[2]");
            ASSERT_GE(scalar, 0);
            ASSERT_GE(vector, 0);
            ASSERT_GE(matrix, 0);
            ASSERT_GE(array0, 0);
            ASSERT_GE(array2, 0);

            glUniform1d(scalar, 5.0);
            const GLdouble vectorValue[3] = {11.0, 12.0, 13.0};
            glUniform3dv(vector, 1, vectorValue);
            // Column-major, and every entry distinct so a transposed or mis-strided write
            // cannot land on a value that happens to match.
            GLdouble matrixValue[16] = {};
            for (int i = 0; i < 16; ++i) matrixValue[i] = 100.0 + i;
            glUniformMatrix4dv(matrix, 1, GL_FALSE, matrixValue);
            const GLdouble arrayValue[3] = {71.0, 72.0, 73.0};
            glUniform1dv(array0, 3, arrayValue);
            glUseProgram(0);

            const std::vector<float> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_FLOAT_EQ(values[0], 5.0f) << "scalar double";
            EXPECT_FLOAT_EQ(values[1], 11.0f) << "dvec3 .x";
            EXPECT_FLOAT_EQ(values[2], 12.0f) << "dvec3 .y";
            EXPECT_FLOAT_EQ(values[3], 13.0f) << "dvec3 .z";
            EXPECT_FLOAT_EQ(values[4], 100.0f) << "dmat4 [0][0]";
            EXPECT_FLOAT_EQ(values[5], 103.0f) << "dmat4 [0][3] - within the first column";
            EXPECT_FLOAT_EQ(values[6], 112.0f) << "dmat4 [3][0] - column stride";
            EXPECT_FLOAT_EQ(values[7], 115.0f) << "dmat4 [3][3]";
            EXPECT_FLOAT_EQ(values[8], 71.0f) << "double array element 0";
            EXPECT_FLOAT_EQ(values[9], 72.0f) << "double array element 1 - element stride";
            EXPECT_FLOAT_EQ(values[10], 73.0f) << "double array element 2";
        }

        TEST_F(DoublePrecisionScenario, TheTransposeFlagStillTransposes) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint matrix = glGetUniformLocation(m_program, "uMatrix");
            ASSERT_GE(matrix, 0);
            GLdouble matrixValue[16] = {};
            for (int i = 0; i < 16; ++i) matrixValue[i] = 100.0 + i;
            glUniformMatrix4dv(matrix, 1, GL_TRUE, matrixValue);
            glUseProgram(0);

            const std::vector<float> values = Dispatch();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            // Transposed, so [column][row] now reads the source's [row][column].
            EXPECT_FLOAT_EQ(values[4], 100.0f) << "dmat4 [0][0] is on the diagonal either way";
            EXPECT_FLOAT_EQ(values[5], 112.0f) << "dmat4 [0][3] after transpose";
            EXPECT_FLOAT_EQ(values[6], 103.0f) << "dmat4 [3][0] after transpose";
            EXPECT_FLOAT_EQ(values[7], 115.0f) << "dmat4 [3][3] is on the diagonal either way";
        }

        TEST_F(DoublePrecisionScenario, TheUniformIsStillReportedAsADouble) {
            if (!Ready()) return;
            // The demotion is an implementation detail of how the value is STORED. What the
            // shader source declared is what the application asked about, so the reflection
            // keeps answering GL_DOUBLE* - an application that switches on the type and calls
            // glUniform*d has to keep working, and it is the glUniform*d path that is correct
            // for these uniforms.
            struct Expectation {
                const char* name;
                GLenum type;
                GLint size;
            };
            const Expectation expectations[] = {
                {"uScalar", GL_DOUBLE, 1},
                {"uVector", GL_DOUBLE_VEC3, 1},
                {"uMatrix", GL_DOUBLE_MAT4, 1},
                {"uArray[0]", GL_DOUBLE, 3},
            };

            GLint activeUniforms = 0;
            glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &activeUniforms);
            ASSERT_GT(activeUniforms, 0);

            for (const Expectation& expectation : expectations) {
                bool found = false;
                for (GLint index = 0; index < activeUniforms; ++index) {
                    char name[128] = {};
                    GLsizei length = 0;
                    GLint size = 0;
                    GLenum type = 0;
                    glGetActiveUniform(m_program, static_cast<GLuint>(index), sizeof(name) - 1, &length, &size,
                                       &type, name);
                    if (std::string(name, static_cast<size_t>(length)) != expectation.name) continue;
                    found = true;
                    EXPECT_EQ(type, expectation.type) << expectation.name;
                    EXPECT_EQ(size, expectation.size) << expectation.name;
                    break;
                }
                EXPECT_TRUE(found) << "glGetActiveUniform never reported " << expectation.name;
            }
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(DoublePrecisionScenario, GetUniformdvReadsBackWhatWasStored) {
            if (!Ready()) return;
            glUseProgram(m_program);
            const GLint scalar = glGetUniformLocation(m_program, "uScalar");
            const GLint vector = glGetUniformLocation(m_program, "uVector");
            const GLint matrix = glGetUniformLocation(m_program, "uMatrix");
            ASSERT_GE(scalar, 0);
            ASSERT_GE(vector, 0);
            ASSERT_GE(matrix, 0);
            glUniform1d(scalar, 0.1);
            const GLdouble vectorValue[3] = {11.5, 12.5, 13.5};
            glUniform3dv(vector, 1, vectorValue);
            GLdouble matrixValue[16] = {};
            for (int i = 0; i < 16; ++i) matrixValue[i] = 100.0 + i;
            glUniformMatrix4dv(matrix, 1, GL_FALSE, matrixValue);
            glUseProgram(0);

            // The readback has to undo exactly what the write did - the same std140 column
            // padding, the same 4-byte components - or a dmat4 comes back with its columns
            // shifted and nothing else in the API would say so.
            GLdouble readScalar = 0.0;
            glGetUniformdv(m_program, scalar, &readScalar);
            EXPECT_DOUBLE_EQ(readScalar, static_cast<double>(static_cast<float>(0.1)))
                << "the value is what a float can hold, not the double that was passed in";

            GLdouble readVector[3] = {};
            glGetUniformdv(m_program, vector, readVector);
            EXPECT_DOUBLE_EQ(readVector[0], 11.5);
            EXPECT_DOUBLE_EQ(readVector[1], 12.5);
            EXPECT_DOUBLE_EQ(readVector[2], 13.5);

            GLdouble readMatrix[16] = {};
            glGetUniformdv(m_program, matrix, readMatrix);
            for (int i = 0; i < 16; ++i) {
                EXPECT_DOUBLE_EQ(readMatrix[i], 100.0 + i) << "dmat4 component " << i;
            }

            // The float query sees the same storage through the type it is actually stored as.
            GLfloat readFloat = 0.0f;
            glGetUniformfv(m_program, scalar, &readFloat);
            EXPECT_FLOAT_EQ(readFloat, static_cast<float>(0.1));
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(DoublePrecisionScenario, TheFp64ExtensionIsNotAdvertised) {
            if (!Ready()) return;
            // The shader above compiled, linked and ran without the extension string, which is
            // the point: an application does not need GL_ARB_gpu_shader_fp64 advertised to USE
            // doubles here. What the string additionally promises is 64-bit precision, and that
            // is the one thing the demotion cannot deliver - so it stays off unless
            // MOBILEGL_ADVERTISE_FP64 asks for it, and an application that branches on the
            // string keeps taking its float path.
            GLint extensionCount = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
            ASSERT_GT(extensionCount, 0);
            bool advertised = false;
            for (GLint i = 0; i < extensionCount; ++i) {
                const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (name != nullptr && std::string(name) == "GL_ARB_gpu_shader_fp64") advertised = true;
            }
            EXPECT_FALSE(advertised);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(DoublePrecisionScenario, A64BitVertexFormatIsDeclinedOnEveryBackend) {
            if (!Ready()) return;
            // The demotion leaves no 64-bit shader input to feed, so there is nothing a 64-bit
            // vertex FETCH could be fetched into - on either backend, and no longer only on the
            // ones whose device lacks shaderFloat64. Declined loudly rather than accepted and
            // drawn as garbage; the matching POST row says the same thing at startup.
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            while (glGetError() != GL_NO_ERROR) {}

            glVertexAttribLFormat(0, 3, GL_DOUBLE, 0);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);
            while (glGetError() != GL_NO_ERROR) {}
        }

    } // namespace
} // namespace MGITest
