// MobileGL - MobileGL/MG_Test/ShaderTranspiler/SpirvPassTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>

#include <spirv_reflect.h>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    // glslangValidator -V output. Both are vertex shaders writing gl_Position through
    // the gl_PerVertex block, i.e. the Position builtin arrives as OpMemberDecorate rather
    // than a plain OpDecorate - the shape real glslang output actually takes.

    //   #version 450
    //   layout(location = 0) in vec4 inPos;
    //   void main() { gl_Position = inPos; }
    constexpr Uint32 kPlainVertexSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000015u, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000000u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000011u, 0x00030003u,
        0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
        0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u,
        0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u,
        0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
        0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
        0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u,
        0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u,
        0x00040005u, 0x00000011u, 0x6f506e69u, 0x00000073u, 0x00030047u, 0x0000000bu,
        0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u,
        0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
        0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu,
        0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x00000011u, 0x0000001eu,
        0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u,
        0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
        0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu,
        0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u,
        0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au,
        0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu,
        0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u,
        0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040020u,
        0x00000010u, 0x00000001u, 0x00000007u, 0x0004003bu, 0x00000010u, 0x00000011u,
        0x00000001u, 0x00040020u, 0x00000013u, 0x00000003u, 0x00000007u, 0x00050036u,
        0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
        0x0004003du, 0x00000007u, 0x00000012u, 0x00000011u, 0x00050041u, 0x00000013u,
        0x00000014u, 0x0000000du, 0x0000000fu, 0x0003003eu, 0x00000014u, 0x00000012u,
        0x000100fdu, 0x00010038u,
    };

    //   ... plus `invariant gl_Position;` - already carries OpMemberDecorate %gl_PerVertex 0
    //   Invariant, so the pass must not add a duplicate.
    constexpr Uint32 kAlreadyInvariantVertexSpirv[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000015u, 0x00000000u, 0x00020011u,
        0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
        0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0007000fu, 0x00000000u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000011u, 0x00030003u,
        0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
        0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u,
        0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u,
        0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
        0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
        0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u,
        0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u,
        0x00040005u, 0x00000011u, 0x6f506e69u, 0x00000073u, 0x00030047u, 0x0000000bu,
        0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u,
        0x00040048u, 0x0000000bu, 0x00000000u, 0x00000012u, 0x00050048u, 0x0000000bu,
        0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u,
        0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu,
        0x00000004u, 0x00040047u, 0x00000011u, 0x0000001eu, 0x00000000u, 0x00020013u,
        0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
        0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u,
        0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u,
        0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u, 0x0006001eu,
        0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u,
        0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du,
        0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu,
        0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040020u, 0x00000010u, 0x00000001u,
        0x00000007u, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u, 0x00040020u,
        0x00000013u, 0x00000003u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u,
        0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u,
        0x00000012u, 0x00000011u, 0x00050041u, 0x00000013u, 0x00000014u, 0x0000000du,
        0x0000000fu, 0x0003003eu, 0x00000014u, 0x00000012u, 0x000100fdu, 0x00010038u,
    };

    // OpMemberDecorate <struct-id> <member> <decoration>
    constexpr Uint32 kOpMemberDecorate = 72;
    constexpr Uint32 kDecorationInvariant = 18;
    constexpr Uint32 kSpirvHeaderWordCount = 5;

    // Test-side reference walker. Deliberately independent of the production code so a bug in
    // the pass cannot hide behind the same helper; only used to count what the pass emitted.
    Uint32 CountInvariantMemberDecorations(const Vector<Uint32>& spirv) {
        Uint32 count = 0;
        for (SizeT i = kSpirvHeaderWordCount; i < spirv.size();) {
            const Uint32 wordCount = spirv[i] >> 16;
            const Uint32 opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) {
                break;
            }
            if (opcode == kOpMemberDecorate && wordCount >= 4 && spirv[i + 3] == kDecorationInvariant) {
                ++count;
            }
            i += wordCount;
        }
        return count;
    }

    template <SizeT WordCount>
    Vector<Uint32> ToVector(const Uint32 (&words)[WordCount]) {
        return Vector<Uint32>(words, words + WordCount);
    }
} // namespace

// --- DecoratePositionInvariantPass ---

TEST(DecoratePositionInvariant, AddsInvariantToThePositionMember) {
    const Vector<Uint32> input = ToVector(kPlainVertexSpirv);
    ASSERT_EQ(CountInvariantMemberDecorations(input), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DecoratePositionInvariantForVulkan(input, output));
    EXPECT_EQ(CountInvariantMemberDecorations(output), 1u);
}

TEST(DecoratePositionInvariant, DoesNotDuplicateAnExistingInvariant) {
    const Vector<Uint32> input = ToVector(kAlreadyInvariantVertexSpirv);
    ASSERT_EQ(CountInvariantMemberDecorations(input), 1u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DecoratePositionInvariantForVulkan(input, output));
    EXPECT_EQ(CountInvariantMemberDecorations(output), 1u);
    // The pass reports SuccessWithoutChange here, and SPIRV-Tools asserts (in assert-enabled
    // builds) that such a run round-trips byte-identically. Pin that from the outside so an
    // assert-enabled CI build cannot be the first thing to discover a violation.
    EXPECT_EQ(output, input);
}

TEST(DecoratePositionInvariant, IsIdempotent) {
    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::DecoratePositionInvariantForVulkan(ToVector(kPlainVertexSpirv), once));
    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::DecoratePositionInvariantForVulkan(once, twice));
    EXPECT_EQ(CountInvariantMemberDecorations(twice), 1u);
}

TEST(DecoratePositionInvariant, OutputStaysAReflectableModule) {
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DecoratePositionInvariantForVulkan(ToVector(kPlainVertexSpirv), output));

    SpvReflectShaderModule module{};
    ASSERT_EQ(spvReflectCreateShaderModule(output.size() * sizeof(Uint32), output.data(), &module),
              SPV_REFLECT_RESULT_SUCCESS);
    EXPECT_EQ(module.entry_point_count, 1u);
    spvReflectDestroyShaderModule(&module);
}

TEST(DecoratePositionInvariant, RejectsGarbageInput) {
    const Vector<Uint32> notSpirv{0xdeadbeefu, 0u, 0u, 0u, 0u};
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::DecoratePositionInvariantForVulkan(notSpirv, output));
}
