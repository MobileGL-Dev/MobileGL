// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/IterationRPScratchFixScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE FIXTURE-SHAPED SUBGROUP REDUCTION, ON WHATEVER WIDTH THE DEVICE HAS.
//
// iterationRP's auto-exposure pass declares `shared vec2 prefixSumCache[32]` for a
// 512-invocation workgroup and combines per-subgroup subtotals through
// prefixSumCache[gl_SubgroupID]. The algorithm is width-agnostic; only the static 32
// bakes in "at most 32 subgroups", which every desktop capture satisfies and an 8-lane
// device (lavapipe: 64 subgroups) does not. DirectVulkan patches exactly that with
// FixIterationRPSubgroupScratchPass, growing the array to ceil(invocations / native
// width) on the modules that match the pack's reduction fingerprint.
//
// This scenario replays the fixture's reduction shape verbatim - the same 32-entry
// declaration, the same last-lane handoff, the same findMSB combine loop, and NO
// domain guard - and asserts only the width-independent result: the workgroup total.
// The inputs are small integers, so the fp32 sum is exact under any lane order and any
// association; a correct run produces the exact constant on a 4-lane device and a
// 128-lane device alike. Without the patch, a sub-16-lane device indexes the
// 32-entry array out of bounds - on lavapipe that is literal heap corruption - and
// this scenario is the regression test that keeps the patch working, and it runs on every device that
// has basic+arithmetic compute subgroups (unlike IterationRPFirstReductionScenario,
// which probes the UNREPAIRED source contract and must skip outside [16, 256]).

#include <cstdint>
#include <cstring>
#include <string>

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
        constexpr std::uint32_t kInvocationCount = 512u;
        // sum of 0..511, exactly representable and associativity-proof in fp32.
        constexpr float kExpectedTotal = 130816.0f;

        constexpr const char* kComputeSource = R"(#version 430 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require

layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;

layout(std430, binding = 0) buffer Output {
    float total;
    uint numSubgroups;
    uint maxSubgroupId;
} outputData;

shared vec2 prefixSumCache[32];

void main() {
    vec2 sampleLuminance = vec2(float(gl_LocalInvocationIndex), 0.0);
    sampleLuminance = subgroupInclusiveAdd(sampleLuminance);
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleLuminance;
    barrier();

    uint loopLength = uint(findMSB(gl_NumSubgroups));
    loopLength += uint(gl_NumSubgroups - (1u << (loopLength - 1u)) > 0u);

    for (uint scanStage = 0u; scanStage < loopLength; ++scanStage) {
        if ((gl_SubgroupID & (1u << scanStage)) > 0u) {
            sampleLuminance += prefixSumCache[(gl_SubgroupID >> scanStage << scanStage) - 1u];
            if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
                prefixSumCache[gl_SubgroupID] = sampleLuminance;
        }
        barrier();
    }

    if (gl_LocalInvocationIndex == 511u) {
        outputData.total = sampleLuminance.x;
        outputData.numSubgroups = gl_NumSubgroups;
    }
    atomicMax(outputData.maxSubgroupId, gl_SubgroupID);
}
)";

        struct OutputBlock {
            float total = -1.0f;
            std::uint32_t numSubgroups = 0;
            std::uint32_t maxSubgroupId = 0;
        };

        bool HasExtension(const char* wanted) {
            GLint extensionCount = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
            for (GLint i = 0; i < extensionCount; ++i) {
                const auto* extension =
                    reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (extension != nullptr && std::string(extension) == wanted) return true;
            }
            return false;
        }

        class IterationRPScratchFixScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                GLint stages = 0;
                GLint features = 0;
                GLint invocations = 0;
                const bool subgroupExtension = HasExtension("GL_KHR_shader_subgroup");
                if (subgroupExtension) {
                    glGetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &stages);
                    glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
                }
                glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
                const GLbitfield requiredFeatures =
                    GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR;
                if (!subgroupExtension || (static_cast<GLbitfield>(stages) & GL_COMPUTE_SHADER_BIT) == 0 ||
                    (static_cast<GLbitfield>(features) & requiredFeatures) != requiredFeatures ||
                    invocations < static_cast<GLint>(kInvocationCount)) {
                    GTEST_SKIP() << "needs GL_KHR_shader_subgroup basic+arithmetic in compute and a "
                                    "512-invocation workgroup";
                }

                m_program = CompileComputeProgram(kComputeSource);
                ASSERT_NE(m_program, 0u) << m_buildLog;

                glGenBuffers(1, &m_output);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                // maxSubgroupId starts at zero HOST-side: the word is touched only by
                // atomicMax during the dispatch, since a plain shader-side zeroing store
                // would race the other invocations' atomics (barrier() orders shared
                // memory, not SSBO stores).
                const OutputBlock poison{-1.0f, 0xa5a5a5a5u, 0u};
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(OutputBlock), &poison, GL_DYNAMIC_READ);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_output);
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
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

            OutputBlock Dispatch() {
                glUseProgram(m_program);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                OutputBlock block{};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_output);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(OutputBlock), &block);
                return block;
            }

            GLuint m_program = 0;
            GLuint m_output = 0;
            std::string m_buildLog;
        };
    } // namespace

    TEST_F(IterationRPScratchFixScenario, FixtureShapedReductionSumsEveryInvocation) {
        const OutputBlock block = Dispatch();
        EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

        // The topology diagnostics catch the failure modes by name before the sum does:
        // an out-of-bounds handoff corrupts the total, a wrong gl_NumSubgroups breaks
        // the combine loop's length.
        ASSERT_NE(block.numSubgroups, 0xa5a5a5a5u) << "invocation 511 never reached its store";
        EXPECT_GE(block.numSubgroups, 1u);
        EXPECT_LE(block.numSubgroups, kInvocationCount);
        EXPECT_LT(block.maxSubgroupId, block.numSubgroups)
            << "gl_SubgroupID exceeds gl_NumSubgroups - the inconsistency "
               "DeriveNumSubgroupsPass exists to repair";

        // Integer-valued fp32 inputs: the workgroup total is exact under any subgroup
        // width, lane order, and association. This is the value iterationRP's exposure
        // average is built from; without FixIterationRPSubgroupScratchPass an 8-lane
        // device writes prefixSumCache[32..63] out of bounds and this comparison fails.
        EXPECT_EQ(block.total, kExpectedTotal)
            << "workgroup reduction produced " << block.total << " with gl_NumSubgroups="
            << block.numSubgroups;
    }
} // namespace MGITest
