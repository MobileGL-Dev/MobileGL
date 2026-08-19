// MobileGL - MobileGL/MG_Test/ShaderTranspiler/DeriveNumSubgroupsTest.cpp
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

    Uint32 FindBuiltinTarget(const Vector<Uint32>& spirv, spv::BuiltIn builtin) {
        Uint32 target = 0u;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpDecorate && wordCount >= 4u &&
                static_cast<spv::Decoration>(words[2]) == spv::Decoration::BuiltIn &&
                static_cast<spv::BuiltIn>(words[3]) == builtin) {
                target = words[1];
            }
        });
        return target;
    }

    Uint32 CountLoadsFrom(const Vector<Uint32>& spirv, Uint32 pointerId) {
        Uint32 count = 0u;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpLoad && wordCount >= 4u && words[3] == pointerId) ++count;
        });
        return count;
    }

    Uint32 CountOpcode(const Vector<Uint32>& spirv, spv::Op wanted) {
        Uint32 count = 0u;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32*, Uint32) {
            if (opcode == wanted) ++count;
        });
        return count;
    }

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        return tools.Validate(spirv);
    }

    constexpr const char* kNumSubgroupsOnlySource = R"(#version 450 core
#extension GL_KHR_shader_subgroup_basic : require
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { uint value; } outputData;
void main() {
    if (gl_LocalInvocationIndex == 0u)
        outputData.value = gl_NumSubgroups;
}
)";

    constexpr const char* kNoNumSubgroupsSource = R"(#version 450 core
layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;
layout(std430, binding = 0) buffer Output { uint value; } outputData;
void main() {
    if (gl_LocalInvocationIndex == 0u)
        outputData.value = gl_WorkGroupSize.x;
}
)";
} // namespace

TEST(DeriveNumSubgroupsPass, ReplacesBuiltinLoadAndSynthesizesSubgroupSize) {
    const Vector<Uint32> input = CompileCompute(kNumSubgroupsOnlySource);
    ASSERT_FALSE(input.empty());
    const Uint32 inputNumSubgroups = FindBuiltinTarget(input, spv::BuiltIn::NumSubgroups);
    ASSERT_NE(inputNumSubgroups, 0u);
    EXPECT_EQ(CountLoadsFrom(input, inputNumSubgroups), 1u);
    EXPECT_EQ(FindBuiltinTarget(input, spv::BuiltIn::SubgroupSize), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DeriveNumSubgroupsForVulkan(input, output, true));
    ASSERT_TRUE(Validates(output));

    const Uint32 outputNumSubgroups = FindBuiltinTarget(output, spv::BuiltIn::NumSubgroups);
    const Uint32 outputSubgroupSize = FindBuiltinTarget(output, spv::BuiltIn::SubgroupSize);
    ASSERT_NE(outputNumSubgroups, 0u);
    ASSERT_NE(outputSubgroupSize, 0u);
    EXPECT_EQ(CountLoadsFrom(output, outputNumSubgroups), 0u);
    EXPECT_EQ(CountLoadsFrom(output, outputSubgroupSize), 1u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpCompositeExtract), 3u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpIMul), 2u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpUDiv), 1u);
}

TEST(DeriveNumSubgroupsPass, IsIdempotent) {
    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::DeriveNumSubgroupsForVulkan(CompileCompute(kNumSubgroupsOnlySource), once, true));
    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::DeriveNumSubgroupsForVulkan(once, twice, true));
    EXPECT_EQ(twice, once);
}

TEST(DeriveNumSubgroupsPass, LeavesUnrelatedComputeShaderUntouched) {
    const Vector<Uint32> input = CompileCompute(kNoNumSubgroupsSource);
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DeriveNumSubgroupsForVulkan(input, output, true));
    EXPECT_EQ(output, input);
    EXPECT_EQ(FindBuiltinTarget(output, spv::BuiltIn::SubgroupSize), 0u);
}
