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
                if (m_shapeOutput != 0) glDeleteBuffers(1, &m_shapeOutput);
                if (m_shapeProgram != 0) glDeleteProgram(m_shapeProgram);
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
            unsigned int m_shapeProgram = 0;
            unsigned int m_shapeOutput = 0;
            std::string m_buildLog;
        };

        // Every double-typed uniform shape GLSL has, all thirteen of them, in one program - the
        // shape of KHR-GL43.compute_shader.fp64-case2. The scalar and the square matrices are
        // covered by the cases above; what only a set like this reaches is the NON-SQUARE
        // matrices, whose column stride and total size both change when the demotion turns a
        // 64-bit column into a 32-bit one, and whose members therefore move every uniform
        // declared after them.
        //
        // The shader reports every component separately rather than one pass/fail flag, because
        // "the readback is wrong" is not a diagnosis: a wrong column stride, a wrong member
        // offset and a wrong narrowing all fail the same single comparison, and only the
        // component map says which.
        // No #version here on purpose: it is handed over as a separate source string, the way
        // the CTS case hands it over.
        constexpr const char* kAllDoubleShapesSource = R"(
layout(local_size_x = 1) in;
uniform double  g_0;
uniform dvec2   g_1;
uniform dvec3   g_2;
uniform dvec4   g_3;
uniform dmat2   g_4;
uniform dmat2x3 g_5;
uniform dmat2x4 g_6;
uniform dmat3x2 g_7;
uniform dmat3   g_8;
uniform dmat3x4 g_9;
uniform dmat4x2 g_10;
uniform dmat4x3 g_11;
uniform dmat4   g_12;
layout(std430, binding = 0) buffer Output {
    float g_out[];
};
void main() {
    g_out[0] = float(g_0);
    for (int i = 0; i < 2; ++i) g_out[1 + i] = float(g_1[i]);
    for (int i = 0; i < 3; ++i) g_out[3 + i] = float(g_2[i]);
    for (int i = 0; i < 4; ++i) g_out[6 + i] = float(g_3[i]);
    for (int c = 0; c < 2; ++c) for (int r = 0; r < 2; ++r) g_out[10 + c * 2 + r] = float(g_4[c][r]);
    for (int c = 0; c < 2; ++c) for (int r = 0; r < 3; ++r) g_out[14 + c * 3 + r] = float(g_5[c][r]);
    for (int c = 0; c < 2; ++c) for (int r = 0; r < 4; ++r) g_out[20 + c * 4 + r] = float(g_6[c][r]);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 2; ++r) g_out[28 + c * 2 + r] = float(g_7[c][r]);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) g_out[34 + c * 3 + r] = float(g_8[c][r]);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 4; ++r) g_out[43 + c * 4 + r] = float(g_9[c][r]);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 2; ++r) g_out[55 + c * 2 + r] = float(g_10[c][r]);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 3; ++r) g_out[63 + c * 3 + r] = float(g_11[c][r]);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) g_out[75 + c * 4 + r] = float(g_12[c][r]);
}
)";

        // The values the CTS case sets, spelled the way it spells them - column-major, and small
        // enough that every one is exact in a float. Nothing here is a precision question; a
        // component that comes back wrong came back from the wrong bytes.
        constexpr double kG0 = 1.0;
        constexpr double kG1[2] = {2.0, 3.0};
        constexpr double kG2[3] = {4.0, 5.0, 6.0};
        constexpr double kG3[4] = {7.0, 8.0, 9.0, 10.0};
        constexpr double kG4[4] = {11.0, 12.0, 13.0, 14.0};
        constexpr double kG5[6] = {15.0, 16.0, 17.0, 18.0, 19.0, 20.0};
        constexpr double kG6[8] = {21.0, 22.0, 23.0, 24.0, 25.0, 26.0, 27.0, 28.0};
        constexpr double kG7[6] = {29.0, 30.0, 31.0, 32.0, 33.0, 34.0};
        constexpr double kG8[9] = {35.0, 36.0, 37.0, 38.0, 39.0, 40.0, 41.0, 42.0, 43.0};
        constexpr double kG9[12] = {44.0, 45.0, 46.0, 47.0, 48.0, 49.0, 50.0, 51.0, 52.0, 53.0, 54.0, 55.0};
        constexpr double kG10[8] = {56.0, 57.0, 58.0, 59.0, 60.0, 61.0, 62.0, 63.0};
        constexpr double kG11[12] = {63.0, 64.0, 65.0, 66.0, 67.0, 68.0, 69.0, 70.0, 71.0, 27.0, 73.0, 74.0};
        constexpr double kG12[16] = {75.0, 76.0, 77.0, 78.0, 79.0, 80.0, 81.0, 82.0,
                                     83.0, 84.0, 85.0, 86.0, 87.0, 88.0, 89.0, 90.0};

        struct DoubleShape {
            const char* name;
            int base;
            int columns; // 1 for the scalar and the vectors
            int rows;    // component count for the scalar and the vectors
            const double* values;
        };

        constexpr DoubleShape kDoubleShapes[] = {
            {"g_0 double", 0, 1, 1, &kG0},      {"g_1 dvec2", 1, 1, 2, kG1},
            {"g_2 dvec3", 3, 1, 3, kG2},        {"g_3 dvec4", 6, 1, 4, kG3},
            {"g_4 dmat2", 10, 2, 2, kG4},       {"g_5 dmat2x3", 14, 2, 3, kG5},
            {"g_6 dmat2x4", 20, 2, 4, kG6},     {"g_7 dmat3x2", 28, 3, 2, kG7},
            {"g_8 dmat3", 34, 3, 3, kG8},       {"g_9 dmat3x4", 43, 3, 4, kG9},
            {"g_10 dmat4x2", 55, 4, 2, kG10},   {"g_11 dmat4x3", 63, 4, 3, kG11},
            {"g_12 dmat4", 75, 4, 4, kG12},
        };

        constexpr int kAllShapeSlots = 91;

        // The conformance case's own shader, kept verbatim down to the literal suffixes and the
        // unnamed, unqualified storage block - except that each comparison sets its OWN bit
        // instead of collapsing all thirteen into one flag. That single flag is the whole reason
        // the case was unexplained for a wave: it says "something is wrong" and nothing else.
        //
        // Verbatim matters here. Reading the components out one at a time (the case above)
        // passes; whatever fails does so through the shape the conformance case actually
        // writes - whole-matrix comparison against a constructor, a storage block with no
        // layout qualifier and no instance name, values reached with constant indices.
        constexpr const char* kCtsShapedSource = R"(
layout(local_size_x = 1) in;
buffer Result {
  int g_result;
};
uniform double g_0;
uniform dvec2 g_1;
uniform dvec3 g_2;
uniform dvec4 g_3;
uniform dmat2 g_4;
uniform dmat2x3 g_5;
uniform dmat2x4 g_6;
uniform dmat3x2 g_7;
uniform dmat3 g_8;
uniform dmat3x4 g_9;
uniform dmat4x2 g_10;
uniform dmat4x3 g_11;
uniform dmat4 g_12;

void main() {
  g_result = 0;

  if (g_0 != 1.0LF) g_result |= 1;
  if (g_1 != dvec2(2.0LF, 3.0LF)) g_result |= 2;
  if (g_2 != dvec3(4.0LF, 5.0LF, 6.0LF)) g_result |= 4;
  if (g_3 != dvec4(7.0LF, 8.0LF, 9.0LF, 10.0LF)) g_result |= 8;

  if (g_4 != dmat2(11.0LF, 12.0LF, 13.0LF, 14.0LF)) g_result |= 16;
  if (g_5 != dmat2x3(15.0LF, 16.0LF, 17.0LF, 18.0LF, 19.0LF, 20.0LF)) g_result |= 32;
  if (g_6 != dmat2x4(21.0LF, 22.0LF, 23.0LF, 24.0LF, 25.0LF, 26.0LF, 27.0LF, 28.0LF)) g_result |= 64;

  if (g_7 != dmat3x2(29.0LF, 30.0LF, 31.0LF, 32.0LF, 33.0LF, 34.0LF)) g_result |= 128;
  if (g_8 != dmat3(35.0LF, 36.0LF, 37.0LF, 38.0LF, 39.0LF, 40.0LF, 41.0LF, 42.0LF, 43.0LF)) g_result |= 256;
  if (g_9 != dmat3x4(44.0LF, 45.0LF, 46.0LF, 47.0LF, 48.0LF, 49.0LF, 50.0LF, 51.0LF, 52.0LF, 53.0LF, 54.0LF, 55.0LF)) g_result |= 512;

  if (g_10 != dmat4x2(56.0, 57.0, 58.0, 59.0, 60.0, 61.0, 62.0, 63.0)) g_result |= 1024;
  if (g_11 != dmat4x3(63.0, 64.0, 65.0, 66.0, 67.0, 68.0, 69.0, 70.0, 71.0, 27.0, 73, 74.0)) g_result |= 2048;
  if (g_12 != dmat4(75.0, 76.0, 77.0, 78.0, 79.0, 80.0, 81.0, 82.0, 83.0, 84.0, 85.0, 86.0, 87.0, 88.0, 89.0, 90.0)) g_result |= 4096;
}
)";

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

        TEST_F(DoublePrecisionScenario, ADoubleUniformKeepsItsDeclaredInitializer) {
            if (!Ready()) return;
            // A declared initializer is seeded straight into the uniform shadow at link, and the
            // seeding used to skip 64-bit floats outright ("no 32-bit shadow encoding") - which
            // was true before the demotion and silently left every such uniform reading zero.
            const char* source = R"(#version 430 core
layout(local_size_x = 1) in;
uniform double uSeeded = 2.5lf;
uniform dvec3  uSeededVector = dvec3(4.0lf, 5.0lf, 6.0lf);
layout(std430, binding = 0) buffer Output {
    float g_out[];
};
void main() {
    g_out[0] = float(uSeeded);
    g_out[1] = float(uSeededVector.x);
    g_out[2] = float(uSeededVector.y);
    g_out[3] = float(uSeededVector.z);
}
)";
            const GLuint program = CompileComputeProgram(source);
            ASSERT_NE(program, 0u) << m_buildLog;

            glUseProgram(program);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            std::vector<float> values(4, -1.0f);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 4 * sizeof(float), values.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glUseProgram(0);
            glDeleteProgram(program);

            EXPECT_FLOAT_EQ(values[0], 2.5f) << "scalar double initializer";
            EXPECT_FLOAT_EQ(values[1], 4.0f) << "dvec3 initializer .x";
            EXPECT_FLOAT_EQ(values[2], 5.0f) << "dvec3 initializer .y";
            EXPECT_FLOAT_EQ(values[3], 6.0f) << "dvec3 initializer .z";
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(DoublePrecisionScenario, EveryDoubleUniformShapeArrivesWhereTheShaderReadsIt) {
            if (!Ready()) return;
            // Built the way the CTS case builds it, because every step of that build has been a
            // bug here at least once: the source arrives as TWO strings (the version directive
            // and the body), the shader is attached before it has a source and deleted while
            // still attached, and the program is linked twice.
            m_shapeProgram = glCreateProgram();
            ASSERT_NE(m_shapeProgram, 0u);
            {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glAttachShader(m_shapeProgram, shader);
                glDeleteShader(shader);
                const char* const sources[2] = {"#version 430 core\n", kAllDoubleShapesSource};
                glShaderSource(shader, 2, sources, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    FAIL() << "compute shader did not compile: " << log;
                }
            }
            glLinkProgram(m_shapeProgram);
            {
                GLint linkedOnce = 0;
                glGetProgramiv(m_shapeProgram, GL_LINK_STATUS, &linkedOnce);
                if (linkedOnce == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(m_shapeProgram, sizeof(log) - 1, nullptr, log);
                    FAIL() << "compute program did not link: " << log;
                }
            }

            glGenBuffers(1, &m_shapeOutput);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shapeOutput);
            const std::vector<float> zeroes(kAllShapeSlots, 0.0f);
            glBufferData(GL_SHADER_STORAGE_BUFFER, kAllShapeSlots * sizeof(float), zeroes.data(), GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_shapeOutput);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            const auto location = [&](const char* name) { return glGetUniformLocation(m_shapeProgram, name); };

            // Pass one sets through glProgramUniform*, pass two through glUniform* after a
            // re-link - the two entry-point families the CTS case exercises, and two different
            // routes into the same uniform storage.
            const auto setWithProgramUniform = [&]() {
                glProgramUniform1d(m_shapeProgram, location("g_0"), kG0);
                glProgramUniform2d(m_shapeProgram, location("g_1"), kG1[0], kG1[1]);
                glProgramUniform3d(m_shapeProgram, location("g_2"), kG2[0], kG2[1], kG2[2]);
                glProgramUniform4d(m_shapeProgram, location("g_3"), kG3[0], kG3[1], kG3[2], kG3[3]);
                glProgramUniformMatrix2dv(m_shapeProgram, location("g_4"), 1, GL_FALSE, kG4);
                glProgramUniformMatrix2x3dv(m_shapeProgram, location("g_5"), 1, GL_FALSE, kG5);
                glProgramUniformMatrix2x4dv(m_shapeProgram, location("g_6"), 1, GL_FALSE, kG6);
                glProgramUniformMatrix3x2dv(m_shapeProgram, location("g_7"), 1, GL_FALSE, kG7);
                glProgramUniformMatrix3dv(m_shapeProgram, location("g_8"), 1, GL_FALSE, kG8);
                glProgramUniformMatrix3x4dv(m_shapeProgram, location("g_9"), 1, GL_FALSE, kG9);
                glProgramUniformMatrix4x2dv(m_shapeProgram, location("g_10"), 1, GL_FALSE, kG10);
                glProgramUniformMatrix4x3dv(m_shapeProgram, location("g_11"), 1, GL_FALSE, kG11);
                glProgramUniformMatrix4dv(m_shapeProgram, location("g_12"), 1, GL_FALSE, kG12);
            };
            // Deliberately does NOT re-issue glUseProgram: the CTS case leaves the program
            // current across the re-link and writes into it from there, so this is the path
            // where a re-link has to keep the current program's uniform storage addressable.
            const auto setWithUniform = [&]() {
                glUniform1d(location("g_0"), kG0);
                glUniform2d(location("g_1"), kG1[0], kG1[1]);
                glUniform3d(location("g_2"), kG2[0], kG2[1], kG2[2]);
                glUniform4d(location("g_3"), kG3[0], kG3[1], kG3[2], kG3[3]);
                glUniformMatrix2dv(location("g_4"), 1, GL_FALSE, kG4);
                glUniformMatrix2x3dv(location("g_5"), 1, GL_FALSE, kG5);
                glUniformMatrix2x4dv(location("g_6"), 1, GL_FALSE, kG6);
                glUniformMatrix3x2dv(location("g_7"), 1, GL_FALSE, kG7);
                glUniformMatrix3dv(location("g_8"), 1, GL_FALSE, kG8);
                glUniformMatrix3x4dv(location("g_9"), 1, GL_FALSE, kG9);
                glUniformMatrix4x2dv(location("g_10"), 1, GL_FALSE, kG10);
                glUniformMatrix4x3dv(location("g_11"), 1, GL_FALSE, kG11);
                glUniformMatrix4dv(location("g_12"), 1, GL_FALSE, kG12);
            };

            const auto dispatchAndRead = [&]() {
                glUseProgram(m_shapeProgram);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<float> values(kAllShapeSlots, -1.0f);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shapeOutput);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kAllShapeSlots * sizeof(float), values.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                // The program stays current on purpose - see setWithUniform.
                return values;
            };

            const auto expectEverything = [](const std::vector<float>& values, const char* pass) {
                for (const DoubleShape& shape : kDoubleShapes) {
                    for (int c = 0; c < shape.columns; ++c) {
                        for (int r = 0; r < shape.rows; ++r) {
                            const int component = c * shape.rows + r;
                            EXPECT_FLOAT_EQ(values[shape.base + component],
                                            static_cast<float>(shape.values[component]))
                                << pass << ": " << shape.name << " column " << c << " row " << r;
                        }
                    }
                }
            };

            setWithProgramUniform();
            expectEverything(dispatchAndRead(), "glProgramUniform*");

            // A re-link zeroes every uniform, so pass two proves its own writes rather than
            // reading pass one's bytes back.
            glLinkProgram(m_shapeProgram);
            GLint linked = 0;
            glGetProgramiv(m_shapeProgram, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shapeOutput);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, kAllShapeSlots * sizeof(float), zeroes.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

            setWithUniform();
            expectEverything(dispatchAndRead(), "glUniform* after re-link");
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(DoublePrecisionScenario, TheConformanceUniformShaderAgreesWithEveryValueItWasGiven) {
            if (!Ready()) return;
            m_shapeProgram = glCreateProgram();
            ASSERT_NE(m_shapeProgram, 0u);
            {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glAttachShader(m_shapeProgram, shader);
                glDeleteShader(shader);
                const char* const sources[2] = {"#version 430 core\n", kCtsShapedSource};
                glShaderSource(shader, 2, sources, nullptr);
                glCompileShader(shader);
                GLint compiled = 0;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[2048] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    FAIL() << "compute shader did not compile: " << log;
                }
            }
            glLinkProgram(m_shapeProgram);
            GLint linked = 0;
            glGetProgramiv(m_shapeProgram, GL_LINK_STATUS, &linked);
            if (linked == GL_FALSE) {
                char log[2048] = {};
                glGetProgramInfoLog(m_shapeProgram, sizeof(log) - 1, nullptr, log);
                FAIL() << "compute program did not link: " << log;
            }

            glGenBuffers(1, &m_shapeOutput);
            const GLint seed = 123;
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_shapeOutput);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(seed), &seed, GL_STATIC_DRAW);

            const auto location = [&](const char* name) { return glGetUniformLocation(m_shapeProgram, name); };
            glProgramUniform1d(m_shapeProgram, location("g_0"), kG0);
            glProgramUniform2d(m_shapeProgram, location("g_1"), kG1[0], kG1[1]);
            glProgramUniform3d(m_shapeProgram, location("g_2"), kG2[0], kG2[1], kG2[2]);
            glProgramUniform4d(m_shapeProgram, location("g_3"), kG3[0], kG3[1], kG3[2], kG3[3]);
            glProgramUniformMatrix2dv(m_shapeProgram, location("g_4"), 1, GL_FALSE, kG4);
            glProgramUniformMatrix2x3dv(m_shapeProgram, location("g_5"), 1, GL_FALSE, kG5);
            glProgramUniformMatrix2x4dv(m_shapeProgram, location("g_6"), 1, GL_FALSE, kG6);
            glProgramUniformMatrix3x2dv(m_shapeProgram, location("g_7"), 1, GL_FALSE, kG7);
            glProgramUniformMatrix3dv(m_shapeProgram, location("g_8"), 1, GL_FALSE, kG8);
            glProgramUniformMatrix3x4dv(m_shapeProgram, location("g_9"), 1, GL_FALSE, kG9);
            glProgramUniformMatrix4x2dv(m_shapeProgram, location("g_10"), 1, GL_FALSE, kG10);
            glProgramUniformMatrix4x3dv(m_shapeProgram, location("g_11"), 1, GL_FALSE, kG11);
            glProgramUniformMatrix4dv(m_shapeProgram, location("g_12"), 1, GL_FALSE, kG12);

            glUseProgram(m_shapeProgram);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

            GLint disagreements = -1;
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(disagreements), &disagreements);
            for (int bit = 0; bit < 13; ++bit) {
                EXPECT_EQ(disagreements & (1 << bit), 0)
                    << kDoubleShapes[bit].name << " did not compare equal to the value it was given";
            }
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
