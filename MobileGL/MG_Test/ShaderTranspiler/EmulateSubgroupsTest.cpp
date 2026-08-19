// MobileGL - MobileGL/MG_Test/ShaderTranspiler/EmulateSubgroupsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#define SPV_ENABLE_UTILITY_CODE
#include "glslang/SPIRV/spirv.hpp11"
#undef SPV_ENABLE_UTILITY_CODE

#include "Includes.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr SizeT kSpirvHeaderWordCount = 5u;

    template <typename Visitor>
    void ForEachInstruction(const Vector<Uint32>& spirv, Visitor&& visit) {
        for (SizeT offset = kSpirvHeaderWordCount; offset < spirv.size();) {
            const Uint32 wordCount = spirv[offset] >> 16u;
            if (wordCount == 0u || offset + wordCount > spirv.size()) break;
            visit(static_cast<spv::Op>(spirv[offset] & 0xffffu), &spirv[offset], wordCount);
            offset += wordCount;
        }
    }

    Vector<Uint32> CompileStage(GLenum stage, const String& source) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};

        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {stage}, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        return binaryResult->front();
    }

    Uint32 CountGroupNonUniform(const Vector<Uint32>& spirv) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32*, Uint32) {
            if (opcode >= spv::Op::OpGroupNonUniformElect && opcode <= spv::Op::OpGroupNonUniformQuadSwap) {
                ++count;
            }
        });
        return count;
    }

    Uint32 CountGroupNonUniformCapabilities(const Vector<Uint32>& spirv) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpCapability || wordCount < 2u) return;
            const auto capability = static_cast<spv::Capability>(words[1]);
            if (capability >= spv::Capability::GroupNonUniform &&
                capability <= spv::Capability::GroupNonUniformQuad) {
                ++count;
            }
        });
        return count;
    }

    Uint32 CountOpcode(const Vector<Uint32>& spirv, spv::Op wanted) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32*, Uint32) {
            if (opcode == wanted) ++count;
        });
        return count;
    }

    bool HasWorkgroupVariable(const Vector<Uint32>& spirv) {
        bool found = false;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpVariable && wordCount >= 4u &&
                static_cast<spv::StorageClass>(words[3]) == spv::StorageClass::Workgroup) {
                found = true;
            }
        });
        return found;
    }

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position,
                                    const char* message) {
            ADD_FAILURE() << "spirv-val at word " << position.index << ": " << message;
        });
        return tools.Validate(spirv);
    }

    // One shader touching every lowered category: builtins, vote, arithmetic
    // scans, ballot math, shuffles, clustered and quad operations.
    constexpr const char* kEveryCategorySource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_vote : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_shuffle_relative : require
#extension GL_KHR_shader_subgroup_clustered : require
#extension GL_KHR_shader_subgroup_quad : require
layout(local_size_x = 48, local_size_y = 1, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { float value[]; } outputData;
void main() {
    uint slot = gl_LocalInvocationIndex * 24u;
    float v = float(gl_LocalInvocationIndex + 1u);
    outputData.value[slot + 0u] = float(gl_SubgroupSize);
    outputData.value[slot + 1u] = float(gl_NumSubgroups);
    outputData.value[slot + 2u] = float(gl_SubgroupID);
    outputData.value[slot + 3u] = float(gl_SubgroupInvocationID);
    outputData.value[slot + 4u] = float(gl_SubgroupEqMask.x + gl_SubgroupLtMask.x);
    outputData.value[slot + 5u] = subgroupElect() ? 1.0 : 0.0;
    outputData.value[slot + 6u] = subgroupAll(v > 0.0) ? 1.0 : 0.0;
    outputData.value[slot + 7u] = subgroupAny(v > 40.0) ? 1.0 : 0.0;
    outputData.value[slot + 8u] = subgroupAllEqual(gl_WorkGroupID.x) ? 1.0 : 0.0;
    outputData.value[slot + 9u] = subgroupAdd(v);
    outputData.value[slot + 10u] = subgroupInclusiveAdd(v);
    outputData.value[slot + 11u] = subgroupExclusiveMax(v);
    outputData.value[slot + 12u] = float(subgroupMin(gl_LocalInvocationIndex));
    uvec4 ballot = subgroupBallot((gl_LocalInvocationIndex & 1u) == 0u);
    outputData.value[slot + 13u] = float(subgroupBallotBitCount(ballot));
    outputData.value[slot + 14u] = float(subgroupBallotFindLSB(ballot));
    outputData.value[slot + 15u] = float(subgroupBallotFindMSB(ballot));
    outputData.value[slot + 16u] = subgroupInverseBallot(ballot) ? 1.0 : 0.0;
    outputData.value[slot + 17u] = subgroupBallotBitExtract(ballot, 3u) ? 1.0 : 0.0;
    outputData.value[slot + 18u] = subgroupBroadcast(v, 2u);
    outputData.value[slot + 19u] = subgroupBroadcastFirst(v);
    outputData.value[slot + 20u] = subgroupShuffle(v, gl_SubgroupInvocationID ^ 5u);
    outputData.value[slot + 21u] = subgroupShuffleXor(v, 1u) + subgroupShuffleUp(v, 1u) +
                                   subgroupShuffleDown(v, 1u);
    outputData.value[slot + 22u] = subgroupClusteredAdd(v, 4u);
    outputData.value[slot + 23u] = subgroupQuadBroadcast(v, 1u) + subgroupQuadSwapHorizontal(v);
    subgroupBarrier();
    subgroupMemoryBarrierShared();
}
)";

    constexpr const char* kNoSubgroupSource = R"(#version 450 core
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Output { uint value; } outputData;
void main() {
    if (gl_LocalInvocationIndex == 0u) outputData.value = gl_WorkGroupSize.x;
}
)";

    // An extended subgroup instruction (SPV_KHR_subgroup_rotate) alongside core
    // ones: outside the lowered set, so the pass must fail rather than emit
    // "subgroup-free" output that still rotates.
    constexpr const char* kRotateSource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_rotate : require
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer Output { float value[]; } outputData;
void main() {
    float v = subgroupAdd(float(gl_SubgroupInvocationID));
    outputData.value[gl_LocalInvocationIndex] = subgroupRotate(v, 1u);
}
)";

    // A 1024-invocation workgroup exchanging a vec4 and a float: the lowering
    // would need 16 KiB + 4 KiB of scratch, past the Vulkan-minimum shared
    // budget of 16384 bytes.
    constexpr const char* kScratchHungrySource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 1024) in;
layout(std430, binding = 0) buffer Output { vec4 value[]; } outputData;
void main() {
    vec4 wide = subgroupAdd(vec4(float(gl_LocalInvocationIndex)));
    wide.x += subgroupInclusiveAdd(float(gl_SubgroupInvocationID));
    outputData.value[gl_LocalInvocationIndex] = wide;
}
)";
} // namespace

TEST(EmulateSubgroupsPass, LowersEveryCategoryToSharedMemory) {
    const Vector<Uint32> input = CompileStage(GL_COMPUTE_SHADER, kEveryCategorySource);
    ASSERT_FALSE(input.empty());
    ASSERT_GT(CountGroupNonUniform(input), 0u);
    ASSERT_GT(CountGroupNonUniformCapabilities(input), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::EmulateSubgroupsForVulkan(input, output, 16384u, true));
    ASSERT_TRUE(Validates(output));

    // The whole point: nothing subgroup-shaped survives, so the module runs on a
    // device with no subgroup support at all.
    EXPECT_EQ(CountGroupNonUniform(output), 0u);
    EXPECT_EQ(CountGroupNonUniformCapabilities(output), 0u);
    // The exchanges go through workgroup-shared scratch behind control barriers.
    EXPECT_TRUE(HasWorkgroupVariable(output));
    EXPECT_GT(CountOpcode(output, spv::Op::OpControlBarrier), CountOpcode(input, spv::Op::OpControlBarrier));
}

TEST(EmulateSubgroupsPass, IsIdempotent) {
    const Vector<Uint32> input = CompileStage(GL_COMPUTE_SHADER, kEveryCategorySource);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::EmulateSubgroupsForVulkan(input, once, 16384u, true));
    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::EmulateSubgroupsForVulkan(once, twice, 16384u, true));
    EXPECT_EQ(twice, once);
}

TEST(EmulateSubgroupsPass, LeavesSubgroupFreeComputeUntouched) {
    const Vector<Uint32> input = CompileStage(GL_COMPUTE_SHADER, kNoSubgroupSource);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::EmulateSubgroupsForVulkan(input, output, 16384u, true));
    EXPECT_EQ(output, input);
}

TEST(EmulateSubgroupsPass, RefusesExtendedSubgroupInstructions) {
    const Vector<Uint32> input = CompileStage(GL_COMPUTE_SHADER, kRotateSource);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::EmulateSubgroupsForVulkan(input, output, 16384u, false));
}

TEST(EmulateSubgroupsPass, RefusesAModuleOverTheScratchBudget) {
    const Vector<Uint32> input = CompileStage(GL_COMPUTE_SHADER, kScratchHungrySource);
    ASSERT_FALSE(input.empty());
    // vec4 scratch (1024 slots * 16 bytes) plus float scratch (4 KiB) exceeds
    // the 16 KiB Vulkan-minimum budget.
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::EmulateSubgroupsForVulkan(input, output, 16384u, false));
    // A device advertising more shared memory takes the same module fine.
    Vector<Uint32> roomier;
    EXPECT_TRUE(ShaderCompiler::EmulateSubgroupsForVulkan(input, roomier, 32768u, true));
    EXPECT_TRUE(Validates(roomier));
}
