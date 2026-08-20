// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FixIterationRPBarrierTest.cpp
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

#include <map>
#include <vector>

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

    Vector<Uint32> CompileCompute(const String& source) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_COMPUTE_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};

        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_COMPUTE_SHADER}, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        return binaryResult->front();
    }

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer(
            [](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
                ADD_FAILURE() << "spirv-val at word " << position.index << ": " << message;
            });
        return tools.Validate(spirv);
    }

    Uint32 CountOpcode(const Vector<Uint32>& spirv, spv::Op wanted) {
        Uint32 count = 0u;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32*, Uint32) {
            if (opcode == wanted) ++count;
        });
        return count;
    }

    bool HasWorkgroupBarrierImmediatelyBeforeSecondScan(const Vector<Uint32>& spirv) {
        std::map<Uint32, Uint32> uintConstants;
        spv::Op previous = spv::Op::OpNop;
        Uint32 scanCount = 0u;
        bool found = false;
        const Uint32* previousWords = nullptr;
        Uint32 previousWordCount = 0u;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpConstant && wordCount >= 4u) {
                uintConstants[words[2]] = words[3];
            }
            if (opcode == spv::Op::OpGroupNonUniformFAdd && wordCount >= 6u &&
                static_cast<spv::GroupOperation>(words[4]) == spv::GroupOperation::InclusiveScan && ++scanCount == 2u &&
                previous == spv::Op::OpControlBarrier && previousWordCount == 4u) {
                found =
                    uintConstants[previousWords[1]] == static_cast<Uint32>(spv::Scope::Workgroup) &&
                    uintConstants[previousWords[2]] == static_cast<Uint32>(spv::Scope::Workgroup) &&
                    uintConstants[previousWords[3]] == (static_cast<Uint32>(spv::MemorySemanticsMask::AcquireRelease) |
                                                        static_cast<Uint32>(spv::MemorySemanticsMask::WorkgroupMemory));
            }
            previous = opcode;
            previousWords = words;
            previousWordCount = wordCount;
        });
        return found;
    }

    constexpr const char* kProgram203RaceShape = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { vec2 value; } outputData;
shared vec2 prefixSumCache[32];
void main() {
    vec2 sampleLuminance = subgroupInclusiveAdd(
        vec2(float(gl_LocalInvocationIndex), 1.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleLuminance;
    barrier();
    if (gl_LocalInvocationIndex == 511u)
        prefixSumCache[0] = sampleLuminance / 512.0;
    barrier();

    float avg = prefixSumCache[0].x;
    float weight = avg > 0.0 ? float(gl_LocalInvocationIndex + 1u) / avg : 0.0;
    vec2 sampleExposure = subgroupInclusiveAdd(vec2(weight, 1.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleExposure;
    barrier();
    if (gl_LocalInvocationIndex == 511u)
        outputData.value = sampleExposure;
}
)";

    constexpr const char* kAlreadySynchronizedShape = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { vec2 value; } outputData;
shared vec2 prefixSumCache[32];
void main() {
    vec2 first = subgroupInclusiveAdd(vec2(float(gl_LocalInvocationIndex), 1.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = first;
    barrier();
    if (gl_LocalInvocationIndex == 511u) prefixSumCache[0] = first / 512.0;
    barrier();
    float avg = prefixSumCache[0].x;
    barrier();
    vec2 second = subgroupInclusiveAdd(vec2(avg, 1.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = second;
    barrier();
    if (gl_LocalInvocationIndex == 511u) outputData.value = second;
}
)";

    constexpr const char* kForeignSingleScanShape = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { vec2 value; } outputData;
shared vec2 prefixSumCache[32];
void main() {
    vec2 value = subgroupInclusiveAdd(vec2(float(gl_LocalInvocationIndex), 1.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = value;
    barrier();
    if (gl_LocalInvocationIndex == 0u) outputData.value = prefixSumCache[0];
}
)";
} // namespace

TEST(FixIterationRPBarrierPass, InsertsWorkgroupBarrierBeforeSecondReduction) {
    const Vector<Uint32> input = CompileCompute(kProgram203RaceShape);
    ASSERT_FALSE(input.empty());
    const Uint32 inputBarrierCount = CountOpcode(input, spv::Op::OpControlBarrier);
    EXPECT_FALSE(HasWorkgroupBarrierImmediatelyBeforeSecondScan(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPBarrierForVulkan(input, output, true));
    EXPECT_EQ(CountOpcode(output, spv::Op::OpControlBarrier), inputBarrierCount + 1u);
    EXPECT_TRUE(HasWorkgroupBarrierImmediatelyBeforeSecondScan(output));
    EXPECT_TRUE(Validates(output));
}

TEST(FixIterationRPBarrierPass, LeavesOtherShapesByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kForeignSingleScanShape);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPBarrierForVulkan(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(FixIterationRPBarrierPass, LeavesAnAlreadySynchronizedShaderByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kAlreadySynchronizedShape);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPBarrierForVulkan(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(FixIterationRPBarrierPass, IsIdempotent) {
    const Vector<Uint32> input = CompileCompute(kProgram203RaceShape);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPBarrierForVulkan(input, once, true));
    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPBarrierForVulkan(once, twice, true));
    EXPECT_EQ(twice, once);
}
