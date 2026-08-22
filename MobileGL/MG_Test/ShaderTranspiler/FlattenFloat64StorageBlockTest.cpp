// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FlattenFloat64StorageBlockTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// FlattenFloat64StorageBlockPass, over the module the production chain actually hands it:
// ShaderCompiler::SanitizeAndOptimizeBinary, where the pass sits immediately before the fp64
// demotion. The behavioural half - that a block copied through the flattened words comes back
// byte for byte - is DoublePrecisionScenario's; what only a module walk can say is WHICH blocks
// were flattened, how wide, and that the ones this pass must not touch came through unchanged.

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>

#include <spirv-tools/libspirv.hpp>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    // A test-side reference walker, deliberately independent of the production code: a bug in
    // the pass must not be able to hide behind the same helper.
    constexpr Uint32 kSpirvHeaderWordCount = 5;
    constexpr Uint32 kOpName = 5;
    constexpr Uint32 kOpDecorate = 71;
    constexpr Uint32 kOpMemberDecorate = 72;
    constexpr Uint32 kOpTypeInt = 21;
    constexpr Uint32 kOpTypeFloat = 22;
    constexpr Uint32 kOpTypeArray = 28;
    constexpr Uint32 kOpTypeStruct = 30;
    constexpr Uint32 kOpConstant = 43;
    constexpr Uint32 kDecorationArrayStride = 6;
    constexpr Uint32 kDecorationOffset = 35;

    template <typename Visitor>
    void ForEachInstruction(const Vector<Uint32>& spirv, Visitor&& visit) {
        for (SizeT i = kSpirvHeaderWordCount; i < spirv.size();) {
            const Uint32 wordCount = spirv[i] >> 16;
            const Uint32 opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            visit(opcode, &spirv[i], wordCount);
            i += wordCount;
        }
    }

    Uint32 StructIdNamed(const Vector<Uint32>& spirv, const String& name) {
        Uint32 structId = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpName || wordCount < 3 || structId != 0) return;
            const char* text = reinterpret_cast<const char*>(&words[2]);
            const SizeT available = static_cast<SizeT>(wordCount - 2) * sizeof(Uint32);
            // The whole name, not a prefix of it: "Wide" must not match "WideOther".
            if (available <= name.size() || text[name.size()] != 0) return;
            if (std::strncmp(text, name.c_str(), name.size()) == 0) structId = words[1];
        });
        return structId;
    }

    // The operands of OpTypeStruct <structId>, i.e. one type id per member.
    Vector<Uint32> MemberTypesOf(const Vector<Uint32>& spirv, Uint32 structId) {
        Vector<Uint32> members;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpTypeStruct || wordCount < 2 || words[1] != structId) return;
            for (Uint32 i = 2; i < wordCount; ++i) members.push_back(words[i]);
        });
        return members;
    }

    Vector<Uint32> MemberOffsetsOf(const Vector<Uint32>& spirv, Uint32 structId) {
        std::map<Uint32, Uint32> byMember;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpMemberDecorate || wordCount < 5 || words[1] != structId) return;
            if (words[3] != kDecorationOffset) return;
            byMember[words[2]] = words[4];
        });
        Vector<Uint32> offsets;
        for (const auto& [member, offset] : byMember) offsets.push_back(offset);
        return offsets;
    }

    Uint32 DecorationValueOf(const Vector<Uint32>& spirv, Uint32 id, Uint32 decoration) {
        Uint32 value = 0xFFFFFFFFu;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpDecorate || wordCount < 4 || words[1] != id || words[2] != decoration) return;
            value = words[3];
        });
        return value;
    }

    // (element type id, declared length) of OpTypeArray <arrayId>, or (0, 0).
    std::pair<Uint32, Uint32> ArrayShapeOf(const Vector<Uint32>& spirv, Uint32 arrayId) {
        Uint32 elementTypeId = 0;
        Uint32 lengthConstantId = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpTypeArray || wordCount < 4 || words[1] != arrayId) return;
            elementTypeId = words[2];
            lengthConstantId = words[3];
        });
        if (elementTypeId == 0) return {0, 0};
        Uint32 length = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpConstant || wordCount < 4 || words[2] != lengthConstantId) return;
            length = words[3];
        });
        return {elementTypeId, length};
    }

    Bool IsUint32Type(const Vector<Uint32>& spirv, Uint32 typeId) {
        Bool isUint = false;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpTypeInt || wordCount < 4 || words[1] != typeId) return;
            isUint = words[2] == 32u && words[3] == 0u;
        });
        return isUint;
    }

    Uint32 CountFloatTypesOfWidth(const Vector<Uint32>& spirv, Uint32 width) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpTypeFloat && wordCount >= 3 && words[2] == width) ++count;
        });
        return count;
    }

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        tools.Disassemble(spirv, &text);
        return text;
    }

    Vector<Uint32> CompileToSpirv(GLenum stage, const String& source) {
        using namespace MG_Util::ShaderTranspiler;
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

    // The whole shared chain, exactly as the frontend runs it at link.
    Vector<Uint32> Sanitize(const Vector<Uint32>& input) {
        Vector<Uint32> output;
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true));
        return output;
    }

    // The block std140 lays out as data0@0, data1[3]@16 stride 16, data2@64 column stride 16,
    // data3@112, data4[2]@128 stride 16, data5@160, data6@192 - 216 bytes, i.e. 54 words.
    constexpr const char* kStd140BlockSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std140, binding = 0) buffer Wide {
    int    data0;
    float  data1[3];
    mat3x2 data2;
    double data3;
    double data4[2];
    int    data5;
    dvec3  data6;
} g_wide;
void main() {
    g_wide.data0 = 1;
    for (int i = 0; i < 3; ++i) g_wide.data1[i] = float(i);
    g_wide.data2 = mat3x2(1.0);
    g_wide.data3 = 2.0lf;
    for (int i = 0; i < 2; ++i) g_wide.data4[i] = double(i);
    g_wide.data5 = 3;
    g_wide.data6 = dvec3(4.0lf);
}
)";
} // namespace

class FlattenFloat64StorageBlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        // The wrapper validates its output on every run, so this covers every rewrite the test
        // performed without any of them having to say so.
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the flattened module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

TEST_F(FlattenFloat64StorageBlockTest, AStorageBlockWithDoublesBecomesOneWordArray) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, kStd140BlockSource);
    ASSERT_FALSE(input.empty());
    // Before: seven members, at the std140 offsets the standard requires WITH the doubles.
    const Uint32 inputStructId = StructIdNamed(input, "Wide");
    ASSERT_NE(inputStructId, 0u) << Disassemble(input);
    EXPECT_EQ(MemberOffsetsOf(input, inputStructId),
              (Vector<Uint32>{0, 16, 64, 112, 128, 160, 192}))
        << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const Uint32 structId = StructIdNamed(output, "Wide");
    ASSERT_NE(structId, 0u) << Disassemble(output);
    const Vector<Uint32> members = MemberTypesOf(output, structId);
    ASSERT_EQ(members.size(), 1u) << "the block should have collapsed to one member\n"
                                  << Disassemble(output);
    EXPECT_EQ(MemberOffsetsOf(output, structId), (Vector<Uint32>{0}));

    const auto [elementTypeId, length] = ArrayShapeOf(output, members[0]);
    ASSERT_NE(elementTypeId, 0u) << "member 0 is not an array\n" << Disassemble(output);
    EXPECT_TRUE(IsUint32Type(output, elementTypeId)) << Disassemble(output);
    // 216 bytes is where the standard puts the end of this block; 216 / 4 = 54 words.
    EXPECT_EQ(length, 54u) << Disassemble(output);
    EXPECT_EQ(DecorationValueOf(output, members[0], kDecorationArrayStride), 4u);

    // And the demotion that runs straight afterwards still has nothing 64-bit left to find.
    EXPECT_EQ(CountFloatTypesOfWidth(output, 64), 0u) << Disassemble(output);
}

// The gate, from the other side: a storage block with no 64-bit member keeps every member and
// every offset it was compiled with. This is what makes the pass free for every shader that does
// not use doubles - which is all of them but a handful.
TEST_F(FlattenFloat64StorageBlockTest, AStorageBlockWithoutDoublesIsLeftAlone) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std140, binding = 0) buffer Plain {
    int    data0;
    float  data1[3];
    mat3x2 data2;
    int    data3;
} g_plain;
void main() {
    g_plain.data0 = 1;
    for (int i = 0; i < 3; ++i) g_plain.data1[i] = float(i);
    g_plain.data2 = mat3x2(1.0);
    g_plain.data3 = 2;
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const Uint32 structId = StructIdNamed(output, "Plain");
    ASSERT_NE(structId, 0u) << Disassemble(output);
    EXPECT_EQ(MemberTypesOf(output, structId).size(), 4u) << Disassemble(output);
    EXPECT_EQ(MemberOffsetsOf(output, structId), (Vector<Uint32>{0, 16, 64, 112}))
        << Disassemble(output);
}

// A plain UNIFORM block is deliberately NOT flattened, however many doubles it holds: the
// frontend's glUniform*d routing is built by reflecting the DEMOTED module
// (ProgramSpirvTask::BuildGlobalUboRouting), so a representation change there would have to move
// with it. It keeps its members and takes the demotion's repacking, exactly as before.
TEST_F(FlattenFloat64StorageBlockTest, AUniformBlockWithDoublesIsLeftToTheDemotion) {
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std140, binding = 0) uniform Params {
    int    data0;
    double data1;
    int    data2;
} g_params;
layout(std430, binding = 0) buffer Sink {
    float g_out[];
};
void main() {
    g_out[0] = float(g_params.data0) + float(g_params.data1) + float(g_params.data2);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const Uint32 structId = StructIdNamed(output, "Params");
    ASSERT_NE(structId, 0u) << Disassemble(output);
    EXPECT_EQ(MemberTypesOf(output, structId).size(), 3u)
        << "a uniform block must not be flattened\n"
        << Disassemble(output);
    // The demotion's re-derived std140 layout for `int, float, int`, which is what the frontend
    // reflects and what glUniform*d then writes into.
    EXPECT_EQ(MemberOffsetsOf(output, structId), (Vector<Uint32>{0, 4, 8})) << Disassemble(output);
}

// ---------------------------------------------------------------------------
// The capability-gated half: a backend that consumes 64-bit floats natively gets neither pass.
// ---------------------------------------------------------------------------

// The flatten exists to preserve a byte layout ACROSS a narrowing. Where nothing narrows there is
// nothing to preserve and the driver lays the block out itself - so the block keeps its seven
// members at the offsets glslang computed, and the doubles in it are still doubles.
TEST_F(FlattenFloat64StorageBlockTest, TheNativePathLeavesTheBlockAndItsDoublesAlone) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, kStd140BlockSource);
    ASSERT_FALSE(input.empty());
    const Uint32 inputStructId = StructIdNamed(input, "Wide");
    ASSERT_NE(inputStructId, 0u);
    const Vector<Uint32> inputOffsets = MemberOffsetsOf(input, inputStructId);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true, true));
    ASSERT_FALSE(output.empty());

    const Uint32 structId = StructIdNamed(output, "Wide");
    ASSERT_NE(structId, 0u) << Disassemble(output);
    EXPECT_EQ(MemberTypesOf(output, structId).size(), 7u)
        << "the block must not be flattened when nothing is narrowing it\n"
        << Disassemble(output);
    EXPECT_EQ(MemberOffsetsOf(output, structId), inputOffsets) << Disassemble(output);
    EXPECT_GT(CountFloatTypesOfWidth(output, 64), 0u) << Disassemble(output);
}

// And the control: the SAME module through the SAME entry point with the bit clear is flattened
// exactly as it always was. This is the pair that pins "capability-false is byte-for-byte the old
// behaviour" at the level the device A/B checks.
TEST_F(FlattenFloat64StorageBlockTest, TheDemotedPathIsUnchangedByTheCapabilityArgument) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, kStd140BlockSource);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> explicitlyDemoted;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, explicitlyDemoted, true, true, false));
    // The four-argument spelling every existing caller uses, which must keep meaning "demote".
    const Vector<Uint32> defaulted = Sanitize(input);
    EXPECT_EQ(explicitlyDemoted, defaulted);
    EXPECT_EQ(CountFloatTypesOfWidth(defaulted, 64), 0u) << Disassemble(defaulted);
}
