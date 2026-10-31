// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SsboArrayDynamicIndexScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A NON-CONSTANT INDEX INTO AN ARRAY OF SHADER STORAGE BLOCKS.
//
// GL 4.3 allows any dynamically-uniform expression there; GLSL ES keeps the ES 3.1 rule that the
// index must be a constant integral expression, and the Qualcomm compiler enforces it:
//
//     '[' : indexing into an SSBO array using a non-constant expression is not permitted
//
// The stage then never compiles, the backend program links nothing, and every dispatch is a
// silent no-op - while glGetProgramiv(GL_LINK_STATUS) keeps reporting the successful link the
// frontend already published. That is why the conformance failures
// (KHR-GL43.shader_storage_buffer_object.basic-stdLayout-case1/case4,
// advanced-indirectAddressing-case2, compute_shader.resources-max, 7 cases in all) read back as
// "the buffer was never written" rather than as an error, and why this scenario asserts on
// contents rather than on link status.
//
// Both index shapes the legalization has to cover are exercised in one dispatch: a loop induction
// variable (which folds when the loop unrolls) and a `uniform int` (which nothing can fold, so the
// switch/select lowering is what carries it), for a read AND for a write.

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

        // Bindings 0..3 are the block array, 4 is the output.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Slot {
    uint value;
} g_slots[4];
layout(std430, binding = 4) buffer Output {
    uint g_result[];
};
uniform int g_index;
void main() {
    // Loop-derived index: foldable by unrolling.
    for (int i = 0; i < 4; ++i) {
        g_result[i] = g_slots[i].value;
    }
    // Uniform-derived index: not foldable, read and write both.
    g_result[4] = g_slots[g_index].value;
    g_slots[g_index].value = 99u;
}
)";

        constexpr int kSlotCount = 4;
        constexpr int kResultCount = 5;

        class SsboArrayDynamicIndexScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                GLint blocks = 0;
                glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
                if (blocks < kSlotCount + 1) {
                    GTEST_SKIP() << "GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS is " << blocks << "; this needs "
                                 << kSlotCount + 1;
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

            GLuint MakeStorageBuffer(const std::vector<unsigned int>& contents) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(contents.size() * sizeof(unsigned int)),
                             contents.data(), GL_DYNAMIC_COPY);
                m_buffers.push_back(buffer);
                return buffer;
            }

            static std::vector<unsigned int> ReadBuffer(GLuint buffer, int count) {
                std::vector<unsigned int> values(static_cast<std::size_t>(count), 0xDEADBEEFu);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                   static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
                return values;
            }

            unsigned int m_program = 0;
            std::string m_buildLog;
            std::vector<GLuint> m_buffers;
        };

    } // namespace

    TEST_F(SsboArrayDynamicIndexScenario, ReadsAndWritesTheBlockTheIndexNames) {
        if (!Ready() || IsSkipped()) return;

        GLuint slots[kSlotCount] = {};
        for (int i = 0; i < kSlotCount; ++i) {
            slots[i] = MakeStorageBuffer({static_cast<unsigned int>(10 + i)});
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(i), slots[i]);
        }
        const GLuint output = MakeStorageBuffer(std::vector<unsigned int>(kResultCount, 0u));
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSlotCount, output);
        ASSERT_EQ(FirstGLError(), 0u);

        glUseProgram(m_program);
        const GLint indexLocation = glGetUniformLocation(m_program, "g_index");
        ASSERT_NE(indexLocation, -1);
        glUniform1i(indexLocation, 2);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        EXPECT_EQ(FirstGLError(), 0u);

        const std::vector<unsigned int> result = ReadBuffer(output, kResultCount);
        for (int i = 0; i < kSlotCount; ++i) {
            EXPECT_EQ(result[static_cast<std::size_t>(i)], static_cast<unsigned int>(10 + i))
                << "g_slots[" << i << "] read through the loop index came back as "
                << result[static_cast<std::size_t>(i)]
                << "; 0 means the stage never compiled and the dispatch was a silent no-op";
        }
        EXPECT_EQ(result[4], 12u) << "g_slots[g_index] with g_index = 2 read back as " << result[4];

        const std::vector<unsigned int> written = ReadBuffer(slots[2], 1);
        EXPECT_EQ(written[0], 99u) << "the uniform-indexed WRITE landed as " << written[0]
                                   << " instead of 99 in g_slots[2]";
        // The write must have gone to element 2 and nowhere else.
        for (int i = 0; i < kSlotCount; ++i) {
            if (i == 2) continue;
            const std::vector<unsigned int> untouched = ReadBuffer(slots[i], 1);
            EXPECT_EQ(untouched[0], static_cast<unsigned int>(10 + i))
                << "g_slots[" << i << "] was overwritten by a write that named element 2";
        }

        for (int i = 0; i <= kSlotCount; ++i) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(i), 0);
        }
    }

    // Four back-to-back dispatches with NO barrier between them, each writing one component of the
    // same uvec4 (KHR-GL43.shader_storage_buffer_object.advanced-switchBuffers-cs). GL leaves them
    // unordered, but the four components are separate memory locations and all four writes have
    // to land; a driver that stores a vector component as a read-modify-write of the whole vector
    // loses some unless the backend orders the dispatches (DirectGLES's PrepareForCompute does).
    TEST_F(SsboArrayDynamicIndexScenario, BackToBackDispatchesKeepEachOthersComponents) {
        if (!Ready() || IsSkipped()) return;
        const GLuint program = CompileComputeProgram(R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, std430) buffer Input { uint cookie[4]; } g_in;
layout(binding = 1, std430) buffer Output { uvec4 digest; };
void main() {
    switch (g_in.cookie[0] + g_in.cookie[1] + g_in.cookie[2] + g_in.cookie[3]) {
    case 0x000000ffu: digest.x = 0xff000000u; break;
    case 0x0000ff00u: digest.y = 0x00ff0000u; break;
    case 0x00ff0000u: digest.z = 0x0000ff00u; break;
    case 0xff000000u: digest.w = 0x000000ffu; break;
    }
}
)");
        ASSERT_NE(program, 0u) << m_buildLog;
        GLuint inputs[4] = {};
        for (int i = 0; i < 4; ++i) {
            // Four cookies summing to 0xff << 8i: dispatch i selects case i.
            const unsigned int part = 0x11u << (8 * i);
            inputs[i] = MakeStorageBuffer({part, part, part * 2, (0xffu << (8 * i)) - part * 4});
        }
        const GLuint output = MakeStorageBuffer({0u, 0u, 0u, 0u});
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, output);
        glUseProgram(program);
        for (int i = 0; i < 4; ++i) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, inputs[i]);
            glDispatchCompute(1, 1, 1);
        }
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
        glUseProgram(0);
        EXPECT_EQ(FirstGLError(), 0u);
        const std::vector<unsigned int> digest = ReadBuffer(output, 4);
        EXPECT_EQ(digest[0], 0xff000000u);
        EXPECT_EQ(digest[1], 0x00ff0000u);
        EXPECT_EQ(digest[2], 0x0000ff00u);
        EXPECT_EQ(digest[3], 0x000000ffu);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
        glDeleteProgram(program);
    }
} // namespace MGITest
