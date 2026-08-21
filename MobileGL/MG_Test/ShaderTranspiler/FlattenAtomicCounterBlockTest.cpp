// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FlattenAtomicCounterBlockTest.cpp
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

#include <cstring>
#include <map>
#include <string>
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

    // Test-side reference walker, deliberately independent of the production code.
    Uint32 FindAtomicCounterBlockStructId(const Vector<Uint32>& spirv) {
        const String prefix = MG_Util::ShaderTranspiler::ATOMIC_COUNTER_BLOCK_PREFIX;
        Uint32 structId = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpName || wordCount < 3u || structId != 0u) return;
            const char* text = reinterpret_cast<const char*>(&words[2]);
            const SizeT available = static_cast<SizeT>(wordCount - 2u) * sizeof(Uint32);
            if (available < prefix.size()) return;
            if (std::strncmp(text, prefix.c_str(), prefix.size()) != 0) return;
            structId = words[1];
        });
        return structId;
    }

    // The Offset of member `member` on struct `structId`, or -1.
    Int64 MemberOffsetOf(const Vector<Uint32>& spirv, Uint32 structId, Uint32 member) {
        Int64 offset = -1;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpMemberDecorate || wordCount < 5u) return;
            if (words[1] != structId || words[2] != member) return;
            if (static_cast<spv::Decoration>(words[3]) != spv::Decoration::Offset) return;
            offset = words[4];
        });
        return offset;
    }

    Uint32 MemberCountOf(const Vector<Uint32>& spirv, Uint32 structId) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpTypeStruct || wordCount < 2u || words[1] != structId) return;
            count = wordCount - 2u;
        });
        return count;
    }

    Uint32 MemberTypeOf(const Vector<Uint32>& spirv, Uint32 structId, Uint32 member) {
        Uint32 typeId = 0;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpTypeStruct || wordCount < 3u + member || words[1] != structId) return;
            typeId = words[2 + member];
        });
        return typeId;
    }

    // The declared length of an OpTypeArray, resolved through the uint constants in the module.
    Int64 ArrayLengthOf(const Vector<Uint32>& spirv, Uint32 arrayTypeId) {
        std::map<Uint32, Uint32> constants;
        Int64 length = -1;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpConstant && wordCount >= 4u) constants[words[2]] = words[3];
            if (opcode == spv::Op::OpTypeArray && wordCount >= 4u && words[1] == arrayTypeId) {
                const auto it = constants.find(words[3]);
                if (it != constants.end()) length = it->second;
            }
        });
        return length;
    }

    // KHR-GL43.compute_shader.resources-atomic-counter's non-zero-offset shape: two counters
    // declared eight bytes into the buffer, which glslang lowers to one block member at Offset 8.
    constexpr const char* kOffsetCounters = R"(#version 450 core
layout(local_size_x = 1) in;
layout(binding = 1, offset = 8) uniform atomic_uint g_counter[2];
layout(std430, binding = 0) buffer Output { uint value[]; } g_out;
void main() {
    g_out.value[0] = atomicCounterIncrement(g_counter[0]);
    g_out.value[1] = atomicCounterIncrement(g_counter[1]);
}
)";

    // The latch: offset 0 is what nearly every shader declares, and it transpiles today.
    constexpr const char* kNaturalCounters = R"(#version 450 core
layout(local_size_x = 1) in;
layout(binding = 1, offset = 0) uniform atomic_uint g_counter[2];
layout(std430, binding = 0) buffer Output { uint value[]; } g_out;
void main() {
    g_out.value[0] = atomicCounterIncrement(g_counter[0]);
    g_out.value[1] = atomicCounterIncrement(g_counter[1]);
}
)";

    constexpr const char* kNoCounters = R"(#version 450 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Output { uint value[]; } g_out;
void main() {
    g_out.value[0] = 1u;
}
)";
} // namespace

TEST(FlattenAtomicCounterBlockPass, MovesTheBlockToOffsetZeroAndGrowsTheArray) {
    const Vector<Uint32> input = CompileCompute(kOffsetCounters);
    ASSERT_FALSE(input.empty());
    const Uint32 structId = FindAtomicCounterBlockStructId(input);
    ASSERT_NE(structId, 0u) << "glslang did not lower the counters onto a gl_AtomicCounterBlock_*";
    ASSERT_EQ(MemberOffsetOf(input, structId, 0u), 8) << "the input's member 0 is not at the declared offset";

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(input, output, true));
    ASSERT_FALSE(output.empty());

    const Uint32 outStructId = FindAtomicCounterBlockStructId(output);
    ASSERT_EQ(outStructId, structId) << "the block's id must not move; SetAtomicCounterBlockBindings "
                                        "still finds it by name";
    EXPECT_EQ(MemberCountOf(output, outStructId), 1u);
    EXPECT_EQ(MemberOffsetOf(output, outStructId, 0u), 0)
        << "member 0 must sit at offset 0 or no std140/std430 layout can express the block";
    // Two counters eight bytes in: the flattened array has to cover bytes [0, 16), i.e. 4 uints,
    // so counter k lands on element 2 + k and therefore on byte 8 + 4k - where it was declared.
    EXPECT_EQ(ArrayLengthOf(output, MemberTypeOf(output, outStructId, 0u)), 4);
    EXPECT_TRUE(Validates(output));
}

TEST(FlattenAtomicCounterBlockPass, LeavesANaturallyPackedBlockByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kNaturalCounters);
    ASSERT_FALSE(input.empty());
    ASSERT_NE(FindAtomicCounterBlockStructId(input), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(FlattenAtomicCounterBlockPass, LeavesAShaderWithoutCountersByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kNoCounters);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(FlattenAtomicCounterBlockPass, IsIdempotent) {
    const Vector<Uint32> input = CompileCompute(kOffsetCounters);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(input, once, true));
    ASSERT_FALSE(once.empty());

    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(once, twice, true));
    EXPECT_EQ(twice, once);
}
