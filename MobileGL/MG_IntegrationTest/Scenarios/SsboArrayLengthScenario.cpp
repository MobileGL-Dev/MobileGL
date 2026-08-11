// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SsboArrayLengthScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - length() ON AN SSBO's UNSIZED ARRAY.
//
// GLSL's `arr.length()` on the trailing runtime array of a shader storage block is not a compile
// time constant: it is (bound range - the array's byte offset inside the block) / array stride,
// evaluated against whatever the descriptor actually covers. Three separate pieces of MobileGL
// have to agree for that to come out right - the byte offsets the block layout was compiled with,
// the buffer the frontend binding resolves to, and the offset/size a glBindBufferRange asked for -
// and a defect in any one of them shows up only as a wrong integer, never as an error.
//
// KHR-GL43.shader_storage_buffer_object.advanced-unsizedArrayLength-* (28 Magma failures, all 28
// passing on Espryt) reports exactly that: lengths too large by roughly the size of the members
// preceding the array. The cases here are the same shape, reduced to what can be asserted in one
// dispatch: a block with no preamble, a block with one, a two-element ARRAY OF BLOCKS (which
// consumes two consecutive bindings and is where the conformance failures concentrate), and the
// two glBindBufferRange forms.
//
// Every length is written into one output SSBO and read back, so a failure names the block and
// prints the number the shader saw.

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

        // Bindings 0..3 are inputs (2 and 3 are the block array), 4 is the output.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer Input0 {
    ivec4 g_input0[];
};
layout(std430, binding = 1) readonly buffer Input1 {
    ivec4 pad1;
    ivec4 data[];
} g_input1;
layout(std430, binding = 2) readonly buffer Input23 {
    ivec4 data[];
} g_input23[2];
layout(std430, binding = 4) buffer Output {
    int g_length[];
};
void main() {
    g_length[0] = g_input0.length();
    g_length[1] = g_input1.data.length();
    g_length[2] = g_input23[0].data.length();
    g_length[3] = g_input23[1].data.length();
}
)";

        constexpr int kElementBytes = 16; // ivec4, std430

        class SsboArrayLengthScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                GLint blocks = 0;
                glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
                if (blocks < 5) {
                    GTEST_SKIP() << "GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS is " << blocks << "; this needs 5";
                }
                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;
            }

            void TearDown() override {
                if (!Ready()) return;
                if (!m_buffers.empty()) glDeleteBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
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

            // A buffer of `elements` ivec4s, filled with a recognisable pattern.
            GLuint MakeStorageBuffer(int elements) {
                std::vector<int> contents(static_cast<std::size_t>(elements) * 4, 41);
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(elements) * kElementBytes, contents.data(), GL_DYNAMIC_COPY);
                m_buffers.push_back(buffer);
                return buffer;
            }

            // Dispatches once and returns the four lengths the shader observed.
            std::vector<int> RunAndReadLengths(GLuint outputBuffer) {
                glUseProgram(m_program);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<int> lengths(4, -1);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, outputBuffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                   static_cast<GLsizeiptr>(lengths.size() * sizeof(int)), lengths.data());
                return lengths;
            }

            unsigned int m_program = 0;
            std::string m_buildLog;
            std::vector<GLuint> m_buffers;
        };

    } // namespace

    // glBindBufferBase everywhere: the plain case, and the one that pins the block array.
    TEST_F(SsboArrayLengthScenario, WholeBufferBindingsReportTheElementCount) {
        if (!Ready() || IsSkipped()) return;

        // input1 carries one ivec4 of preamble before its runtime array, so a length that ignores
        // the member offset comes back one too large there and only there.
        const GLuint input0 = MakeStorageBuffer(7);
        const GLuint input1 = MakeStorageBuffer(1 + 5);
        const GLuint input2 = MakeStorageBuffer(3);
        const GLuint input3 = MakeStorageBuffer(4);
        const GLuint output = MakeStorageBuffer(4);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, input0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, input1);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, input2);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, input3);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, output);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<int> lengths = RunAndReadLengths(output);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(lengths[0], 7) << "Input0 (no preamble, 7 elements) reported length " << lengths[0];
        EXPECT_EQ(lengths[1], 5) << "Input1 (1 ivec4 of preamble, 6 elements of storage) reported length "
                                 << lengths[1] << "; 6 means the array's byte offset inside the block was ignored";
        EXPECT_EQ(lengths[2], 3) << "Input23[0] (binding 2, 3 elements) reported length " << lengths[2];
        EXPECT_EQ(lengths[3], 4) << "Input23[1] (binding 3, 4 elements) reported length " << lengths[3]
                                 << "; a block array's second element must resolve to the NEXT binding";
    }

    // glBindBufferRange with a non-zero offset: length() must see only the bound window.
    TEST_F(SsboArrayLengthScenario, RangeBindingsReportTheBoundWindow) {
        if (!Ready() || IsSkipped()) return;

        GLint alignment = 1;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
        if (alignment > 2 * kElementBytes) {
            GTEST_SKIP() << "GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT is " << alignment
                         << "; a two-element offset cannot be expressed";
        }

        const GLuint input0 = MakeStorageBuffer(7);
        const GLuint input1 = MakeStorageBuffer(1 + 5);
        const GLuint input2 = MakeStorageBuffer(3);
        const GLuint input3 = MakeStorageBuffer(4);
        const GLuint output = MakeStorageBuffer(4);
        // Input0: window starts two elements in, so 5 remain.
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, input0, 2 * kElementBytes, 5 * kElementBytes);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, input1);
        // Both elements of the block array get a window, so a failure says whether the array's
        // FIRST element is handled and only the later ones are lost, or neither is.
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, input2, 0, 2 * kElementBytes);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 3, input3, 0, 2 * kElementBytes);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, output);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<int> lengths = RunAndReadLengths(output);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(lengths[0], 5) << "Input0 bound as [2 elements, 5 elements) reported length " << lengths[0]
                                 << "; 7 means glBindBufferRange's offset/size never reached the descriptor";
        EXPECT_EQ(lengths[2], 2) << "Input23[0] bound as [0, 2 elements) reported length " << lengths[2];
        EXPECT_EQ(lengths[3], 2) << "Input23[1] bound as [0, 2 elements) reported length " << lengths[3];

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, input0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, input3);
    }
} // namespace MGITest
