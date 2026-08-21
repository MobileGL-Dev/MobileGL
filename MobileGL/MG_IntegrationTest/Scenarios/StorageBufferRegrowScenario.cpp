// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/StorageBufferRegrowScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glBufferData GROWS A BUFFER THAT IS ALREADY BOUND AT AN INDEXED POINT.
//
// GL says the indexed binding follows the buffer object, so after the store is re-specified the
// shader sees the NEW extent. DirectGLES shadows the indexed bindings so a redundant
// glBindBufferBase can be skipped, and nothing used to invalidate that shadow when the store was
// re-specified underneath it - so on a driver that resolves a whole-buffer indexed binding's
// extent at BIND time (Adreno does; Mali does not) the shader kept seeing the OLD, smaller range.
// Stores past it are dropped and loads return zero, which is exactly what
// KHR-GL43.compute_shader.dispatch-indirect reported: the first iteration's 6 elements correct and
// everything past byte 24 zero, after the same buffer was re-specified from 24 to 96 bytes.
//
// The assertion is deliberately on the WHOLE grown range, so a partial write names the byte the
// stale extent stopped at.

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

        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output {
    uint g_data[];
};
void main() {
    g_data[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.x + 1u;
}
)";

        constexpr int kSmallElements = 6;  // 24 bytes - the first iteration's size
        constexpr int kLargeElements = 24; // 96 bytes - what the second iteration grows to

        class StorageBufferRegrowScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;
                glGenBuffers(1, &m_buffer);
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_buffer != 0) glDeleteBuffers(1, &m_buffer);
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

            void RespecifyTo(int elements) {
                const std::vector<unsigned int> zeros(static_cast<std::size_t>(elements), 0u);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(zeros.size() * sizeof(unsigned int)), zeros.data(),
                             GL_DYNAMIC_COPY);
            }

            std::vector<unsigned int> DispatchAndRead(int elements) {
                glUseProgram(m_program);
                glDispatchCompute(static_cast<GLuint>(elements), 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::vector<unsigned int> values(static_cast<std::size_t>(elements), 0xDEADBEEFu);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_buffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                   static_cast<GLsizeiptr>(values.size() * sizeof(unsigned int)), values.data());
                return values;
            }

            unsigned int m_program = 0;
            GLuint m_buffer = 0;
            std::string m_buildLog;
        };

    } // namespace

    TEST_F(StorageBufferRegrowScenario, AGrownStoreIsVisibleThroughItsExistingIndexedBinding) {
        if (!Ready() || IsSkipped()) return;

        // Iteration one: 24 bytes, bound once, six groups.
        RespecifyTo(kSmallElements);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_buffer);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<unsigned int> small = DispatchAndRead(kSmallElements);
        ASSERT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kSmallElements; ++i) {
            ASSERT_EQ(small[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                << "the 24-byte iteration itself did not write element " << i;
        }

        // Iteration two: the SAME buffer grows to 96 bytes with NO new glBindBufferBase, which is
        // what the application is entitled to do and what the shadow used to swallow.
        RespecifyTo(kLargeElements);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<unsigned int> large = DispatchAndRead(kLargeElements);
        EXPECT_EQ(FirstGLError(), 0u);
        for (int i = 0; i < kLargeElements; ++i) {
            EXPECT_EQ(large[static_cast<std::size_t>(i)], static_cast<unsigned int>(i + 1))
                << "element " << i << " (byte " << i * 4 << ") of the grown store came back as "
                << large[static_cast<std::size_t>(i)]
                << "; zero from element " << kSmallElements
                << " on means the shader still saw the pre-growth extent";
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    }
} // namespace MGITest
