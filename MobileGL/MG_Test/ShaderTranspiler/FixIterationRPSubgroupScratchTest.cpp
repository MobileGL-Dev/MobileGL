// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FixIterationRPSubgroupScratchTest.cpp
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

#include <algorithm>
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

    // The declared lengths of every Workgroup-storage array variable, sorted.
    std::vector<Uint32> WorkgroupArrayLengths(const Vector<Uint32>& spirv) {
        std::map<Uint32, Uint32> constantValues;      // constant id -> value
        std::map<Uint32, Uint32> arrayLengthIds;      // array type id -> length constant id
        std::map<Uint32, Uint32> pointerPointees;     // pointer type id -> pointee type id
        std::vector<Uint32> workgroupPointerTypes;    // type ids of Workgroup variables
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            switch (opcode) {
            case spv::Op::OpConstant:
                if (wordCount >= 4u) constantValues[words[2]] = words[3];
                break;
            case spv::Op::OpTypeArray:
                if (wordCount >= 4u) arrayLengthIds[words[1]] = words[3];
                break;
            case spv::Op::OpTypePointer:
                if (wordCount >= 4u &&
                    static_cast<spv::StorageClass>(words[2]) == spv::StorageClass::Workgroup) {
                    pointerPointees[words[1]] = words[3];
                }
                break;
            case spv::Op::OpVariable:
                if (wordCount >= 4u &&
                    static_cast<spv::StorageClass>(words[3]) == spv::StorageClass::Workgroup) {
                    workgroupPointerTypes.push_back(words[1]);
                }
                break;
            default:
                break;
            }
        });
        std::vector<Uint32> lengths;
        for (const Uint32 pointerTypeId : workgroupPointerTypes) {
            const auto pointee = pointerPointees.find(pointerTypeId);
            if (pointee == pointerPointees.end()) continue;
            const auto lengthId = arrayLengthIds.find(pointee->second);
            if (lengthId == arrayLengthIds.end()) continue;
            const auto value = constantValues.find(lengthId->second);
            if (value != constantValues.end()) lengths.push_back(value->second);
        }
        std::sort(lengths.begin(), lengths.end());
        return lengths;
    }

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position,
                                    const char* message) {
            ADD_FAILURE() << "spirv-val at word " << position.index << ": " << message;
        });
        return tools.Validate(spirv);
    }

    // iterationRP's reduction fingerprint: 32x16x1, subgroupInclusiveAdd on a
    // vec2, and the pack's own 32-entry gl_SubgroupID-indexed scratch. A second,
    // plainly indexed array rides along to prove the patch is surgical.
    constexpr const char* kIterationRPShapedSource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { float value; } outputData;
shared vec2 prefixSumCache[32];
shared float plainScratch[4];
void main() {
    vec2 sampleLuminance = vec2(float(gl_LocalInvocationIndex), 0.0);
    sampleLuminance = subgroupInclusiveAdd(sampleLuminance);
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleLuminance;
    plainScratch[gl_LocalInvocationIndex & 3u] = sampleLuminance.x;
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
    if (gl_LocalInvocationIndex == 511u)
        outputData.value = prefixSumCache[0].x / 512.0 + plainScratch[0];
}
)";

    // Same scratch idiom, different workgroup shape - NOT iterationRP, so the
    // fingerprint must refuse it even though it would break identically.
    constexpr const char* kWrongWorkgroupShapeSource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 64, local_size_y = 8, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { float value; } outputData;
shared vec2 prefixSumCache[32];
void main() {
    vec2 v = subgroupInclusiveAdd(vec2(1.0, 0.0));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = v;
    barrier();
    if (gl_LocalInvocationIndex == 0u)
        outputData.value = prefixSumCache[0].x;
}
)";

    // Right shape, but a float scan and a float[32] scratch - not the pack's
    // vec2 accumulator signature.
    constexpr const char* kWrongElementTypeSource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { float value; } outputData;
shared float cache[32];
void main() {
    float v = subgroupInclusiveAdd(float(gl_LocalInvocationIndex));
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        cache[gl_SubgroupID] = v;
    barrier();
    if (gl_LocalInvocationIndex == 0u)
        outputData.value = cache[0];
}
)";
} // namespace

TEST(FixIterationRPSubgroupScratchPass, GrowsThePacksScratchForNarrowSubgroups) {
    const Vector<Uint32> input = CompileCompute(kIterationRPShapedSource);
    ASSERT_FALSE(input.empty());
    ASSERT_EQ(WorkgroupArrayLengths(input), (std::vector<Uint32>{4u, 32u}));

    // lavapipe: 8-lane subgroups over 512 invocations need 64 entries; the
    // plainly indexed neighbour must keep its 4.
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(input, output, 8u, true));
    EXPECT_EQ(WorkgroupArrayLengths(output), (std::vector<Uint32>{4u, 64u}));
    EXPECT_TRUE(Validates(output));
}

TEST(FixIterationRPSubgroupScratchPass, LeavesPackWidthAssumptionsAloneOnWideDevices) {
    const Vector<Uint32> input = CompileCompute(kIterationRPShapedSource);
    ASSERT_FALSE(input.empty());

    // >= 16 lanes means at most 32 subgroups: the pack's declared size holds and
    // the module must pass through byte-identical.
    for (const Uint32 nativeSize : {16u, 32u, 64u, 128u}) {
        Vector<Uint32> output;
        ASSERT_TRUE(ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(input, output, nativeSize, true));
        EXPECT_EQ(output, input) << "native width " << nativeSize;
    }
}

TEST(FixIterationRPSubgroupScratchPass, RefusesAModuleOutsideTheFingerprint) {
    for (const char* source : {kWrongWorkgroupShapeSource, kWrongElementTypeSource}) {
        const Vector<Uint32> input = CompileCompute(source);
        ASSERT_FALSE(input.empty());
        Vector<Uint32> output;
        ASSERT_TRUE(ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(input, output, 8u, true));
        EXPECT_EQ(output, input);
    }
}

TEST(FixIterationRPSubgroupScratchPass, IsIdempotent) {
    const Vector<Uint32> input = CompileCompute(kIterationRPShapedSource);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(input, once, 8u, true));
    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(once, twice, 8u, true));
    EXPECT_EQ(twice, once);
}
