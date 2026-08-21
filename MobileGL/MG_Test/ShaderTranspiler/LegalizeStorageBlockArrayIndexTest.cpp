// MobileGL - MobileGL/MG_Test/ShaderTranspiler/LegalizeStorageBlockArrayIndexTest.cpp
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

#include <set>
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

    // Test-side reference walker, deliberately independent of the production detection so a
    // bug in the pass cannot hide behind the same helper: true when some access chain rooted
    // at an array-of-storage-blocks variable carries a non-constant FIRST index, which is
    // exactly what the Qualcomm ES compiler refuses.
    bool HasDynamicBlockArrayIndex(const Vector<Uint32>& spirv) {
        std::set<Uint32> blockStructs;      // OpTypeStruct ids decorated Block / BufferBlock
        std::set<Uint32> constants;         // OpConstant / OpConstantNull result ids
        std::set<Uint32> blockArrayTypes;   // OpTypeArray ids whose element is such a struct
        std::set<Uint32> blockArrayPointers;// OpTypePointer ids pointing at one of those arrays
        std::set<Uint32> blockArrayVars;    // OpVariable ids of one of those pointer types

        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            switch (opcode) {
            case spv::Op::OpDecorate:
                if (wordCount >= 3u) {
                    const auto decoration = static_cast<spv::Decoration>(words[2]);
                    if (decoration == spv::Decoration::Block ||
                        decoration == spv::Decoration::BufferBlock) {
                        blockStructs.insert(words[1]);
                    }
                }
                break;
            case spv::Op::OpConstant:
                if (wordCount >= 3u) constants.insert(words[2]);
                break;
            case spv::Op::OpConstantNull:
                if (wordCount >= 3u) constants.insert(words[2]);
                break;
            case spv::Op::OpTypeArray:
                // OpTypeArray <result> <element type> <length>
                if (wordCount >= 4u && blockStructs.count(words[2]) != 0u) {
                    blockArrayTypes.insert(words[1]);
                }
                break;
            case spv::Op::OpTypePointer:
                // OpTypePointer <result> <storage class> <pointee>
                if (wordCount >= 4u && blockArrayTypes.count(words[3]) != 0u) {
                    blockArrayPointers.insert(words[1]);
                }
                break;
            case spv::Op::OpVariable:
                // OpVariable <result type> <result> <storage class>
                if (wordCount >= 4u && blockArrayPointers.count(words[1]) != 0u) {
                    blockArrayVars.insert(words[2]);
                }
                break;
            default:
                break;
            }
        });

        bool dynamic = false;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpAccessChain && opcode != spv::Op::OpInBoundsAccessChain) return;
            // OpAccessChain <result type> <result> <base> <index 0> ...
            if (wordCount < 5u) return;
            if (blockArrayVars.count(words[3]) == 0u) return;
            if (constants.count(words[4]) != 0u) return;
            dynamic = true;
        });
        return dynamic;
    }

    // `for (i = 0; i < 4; ++i)` over an array of storage blocks - the shape
    // KHR-GL43.shader_storage_buffer_object.basic-stdLayout-case1 uses. Foldable: the
    // induction variable is a literal after unrolling.
    constexpr const char* kLoopIndexedBlockArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Blk { uint data[4]; } g_blocks[4];
layout(std430, binding = 8) buffer Out { uint data[4]; } g_out;
void main() {
    for (int i = 0; i < 4; ++i) {
        g_out.data[i] = g_blocks[i].data[0];
    }
}
)";

    // A uniform-sourced index - the shape
    // KHR-GL43.shader_storage_buffer_object.advanced-indirectAddressing-case2 uses. Nothing
    // can fold it, so the switch/select lowering is what has to carry it.
    constexpr const char* kUniformIndexedBlockArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Blk { uint data[4]; } g_blocks[4];
layout(std430, binding = 8) buffer Out { uint value; } g_out;
uniform int g_index;
void main() {
    g_blocks[g_index].data[0] = 7u;
    g_out.value = g_blocks[g_index].data[1];
}
)";

    // The positive control from the device run: dynamic addressing through an array MEMBER of
    // ONE block is legal ES and must not be rewritten.
    constexpr const char* kArrayMemberInsideOneBlock = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Blk { uint data[4]; } g_block;
layout(std430, binding = 8) buffer Out { uint value; } g_out;
uniform int g_index;
void main() {
    g_out.value = g_block.data[g_index];
}
)";

    // A block array indexed only with literals is already legal ES.
    constexpr const char* kConstantIndexedBlockArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Blk { uint data[4]; } g_blocks[4];
layout(std430, binding = 8) buffer Out { uint value; } g_out;
void main() {
    g_out.value = g_blocks[2].data[0] + g_blocks[3].data[1];
}
)";
} // namespace

TEST(LegalizeStorageBlockArrayIndexPass, FoldsALoopIndexedBlockArray) {
    const Vector<Uint32> input = CompileCompute(kLoopIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    // Either half of the legalization is an acceptable outcome here - what the ES driver
    // cares about is only that no dynamic subscript survives.
    EXPECT_FALSE(HasDynamicBlockArrayIndex(output));
    EXPECT_TRUE(Validates(output));
}

TEST(LegalizeStorageBlockArrayIndexPass, LowersAUniformIndexedWriteToASwitchAndAReadToSelects) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicBlockArrayIndex(input));
    EXPECT_EQ(CountOpcode(input, spv::Op::OpSwitch), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(output));
    // One switch for the store, and one select per element past the first for the load.
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSwitch), 1u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSelect), 3u);
    EXPECT_TRUE(Validates(output));
}

TEST(LegalizeStorageBlockArrayIndexPass, LeavesADynamicMemberOfOneBlockByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kArrayMemberInsideOneBlock);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(LegalizeStorageBlockArrayIndexPass, LeavesAConstantIndexedBlockArrayByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kConstantIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(LegalizeStorageBlockArrayIndexPass, IsIdempotent) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedBlockArray);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(input, once, true));
    ASSERT_FALSE(once.empty());

    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::LegalizeStorageBlockArrayIndexingForEssl(once, twice, true));
    EXPECT_EQ(twice, once);
}
