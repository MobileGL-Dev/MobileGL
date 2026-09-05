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
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

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
    constexpr Uint32 kOpTypeRuntimeArray = 29;
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

    // The element type id of OpTypeRuntimeArray <arrayId>, or 0 when it is not one - which is
    // what a BOUNDED flattened member (an OpTypeArray) answers too, so the two shapes can be told
    // apart by the pair of helpers.
    Uint32 RuntimeArrayElementOf(const Vector<Uint32>& spirv, Uint32 arrayId) {
        Uint32 elementTypeId = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpTypeRuntimeArray || wordCount < 3 || words[1] != arrayId) return;
            elementTypeId = words[2];
        });
        return elementTypeId;
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

    // How many lines of a disassembly hold BOTH fragments - "OpIMul %uint" and "%uint_8", say -
    // which is how the index arithmetic the pass emits is pinned without a host that could run it.
    Uint32 CountLinesWith(const String& text, const String& first, const String& second) {
        Uint32 count = 0;
        SizeT lineStart = 0;
        while (lineStart < text.size()) {
            SizeT lineEnd = text.find('\n', lineStart);
            if (lineEnd == String::npos) lineEnd = text.size();
            const String line = text.substr(lineStart, lineEnd - lineStart);
            if (line.find(first) != String::npos && line.find(second) != String::npos) ++count;
            lineStart = lineEnd + 1;
        }
        return count;
    }

    // What every test of the open-ended shape asserts: the block collapsed to ONE member, which
    // is a `uint[]` RUNTIME array of stride 4 rather than a bounded one, and nothing 64-bit is
    // left for the demotion to find. Returns the disassembly for the arithmetic checks.
    String ExpectOpenEndedWordArray(const Vector<Uint32>& output, const String& blockName) {
        const String text = Disassemble(output);
        const Uint32 structId = StructIdNamed(output, blockName);
        EXPECT_NE(structId, 0u) << text;
        if (structId == 0) return text;
        const Vector<Uint32> members = MemberTypesOf(output, structId);
        EXPECT_EQ(members.size(), 1u) << "the block should have collapsed to one member\n" << text;
        if (members.size() != 1) return text;
        EXPECT_EQ(MemberOffsetsOf(output, structId), (Vector<Uint32>{0}));
        const Uint32 elementTypeId = RuntimeArrayElementOf(output, members[0]);
        EXPECT_NE(elementTypeId, 0u) << "member 0 is not a runtime array\n" << text;
        EXPECT_EQ(ArrayShapeOf(output, members[0]).first, 0u)
            << "an open-ended block must not be given a bounded length\n"
            << text;
        EXPECT_TRUE(IsUint32Type(output, elementTypeId)) << text;
        EXPECT_EQ(DecorationValueOf(output, members[0], kDecorationArrayStride), 4u) << text;
        EXPECT_EQ(CountFloatTypesOfWidth(output, 64), 0u) << text;
        return text;
    }

    // The compute shape every failing KHR-Single-GL45.subgroups fp64 case binds: one runtime
    // array of doubles, indexed by an invocation id, read whole-element.
    String OpenEndedComputeSource(const String& elementType) {
        return String(R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data { )") +
               elementType + R"( data[]; };
void main() {
    )" + elementType +
               R"( value = data[gl_LocalInvocationID.x] * data[0];
    result[gl_GlobalInvocationID.x] = uint(value)" +
               (elementType == "double" ? String{} : String(".x")) + R"();
}
)";
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

// ---------------------------------------------------------------------------
// The open-ended shape: a block whose last member is a runtime array. Before this was accepted
// the pass declined it and the demotion re-derived ArrayStride 4 for the now-float element, so
// `double data[]` read the application's 8-byte-stride buffer as 32-bit words - every fp64
// KHR-Single-GL45.subgroups case failed on exactly that.
// ---------------------------------------------------------------------------

TEST_F(FlattenFloat64StorageBlockTest, AnOpenEndedBlockOfDoublesBecomesAWordRuntimeArray) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, OpenEndedComputeSource("double"));
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Data");
    // Element i of the original array starts at word 2i, so the dynamic index is scaled by 2 ...
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_2"), 1u) << text;
    // ... and the constant `data[0]` is the pair of words at 0 and 1, reached through the one
    // member the block has left.
    EXPECT_GE(CountLinesWith(text, "OpAccessChain %_ptr_StorageBuffer_uint", "%uint_0 %uint_0"), 1u) << text;
}

TEST_F(FlattenFloat64StorageBlockTest, EachDoubleVectorWidthStepsByItsOwnStride) {
    struct Shape {
        const char* element;
        // std430 strides: dvec2 16 bytes, dvec3 and dvec4 32 bytes - i.e. 4, 8 and 8 words.
        const char* strideWords;
        // The last component's word offset inside one element, and the first one past it.
        const char* lastComponentWords;
        const char* firstWordPastIt;
    };
    const Shape shapes[] = {{"dvec2", "%uint_4", "%uint_2", "%uint_4"},
                            {"dvec3", "%uint_8", "%uint_4", "%uint_6"},
                            {"dvec4", "%uint_8", "%uint_6", "%uint_8"}};
    for (const Shape& shape : shapes) {
        SCOPED_TRACE(shape.element);
        const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, OpenEndedComputeSource(shape.element));
        ASSERT_FALSE(input.empty());
        const Vector<Uint32> output = Sanitize(input);
        ASSERT_FALSE(output.empty());

        const String text = ExpectOpenEndedWordArray(output, "Data");
        EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", shape.strideWords), 1u) << text;
        EXPECT_GE(CountLinesWith(text, "OpIAdd %uint", shape.lastComponentWords), 1u) << text;
        // A dvec3 is six words in a stride of eight: nothing may be read from the padding.
        EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", shape.firstWordPastIt), 0u) << text;
    }
}

TEST_F(FlattenFloat64StorageBlockTest, AFixedPrefixBeforeTheRuntimeArrayIsAddedToEveryIndex) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data {
    uvec4  head;
    double data[];
};
void main() {
    result[gl_GlobalInvocationID.x] = head.x + uint(data[gl_LocalInvocationID.x]);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Uint32 inputStructId = StructIdNamed(input, "Data");
    ASSERT_NE(inputStructId, 0u);
    EXPECT_EQ(MemberOffsetsOf(input, inputStructId), (Vector<Uint32>{0, 16})) << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = ExpectOpenEndedWordArray(output, "Data");
    // The 16-byte prefix is 4 words: element i is at word 4 + 2i.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_2"), 1u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", "%uint_4"), 1u) << text;
    // And the prefix member itself is still word 0.
    EXPECT_GE(CountLinesWith(text, "OpAccessChain %_ptr_StorageBuffer_uint", "%uint_0 %uint_0"), 1u) << text;
}

// OpArrayLength on the flattened member counts WORDS. GL's `.length()` is the number of whole
// elements the bound range holds past the array's offset, so the count has to be rebased and
// divided - in unsigned arithmetic, and clamped rather than wrapped when the range is shorter
// than the prefix.
namespace {
    // A prefix, an open-ended array of doubles, and a `.length()` of it - the one shape whose
    // rewrite is an instruction SPIRV-Cross has to spell rather than plain arithmetic.
    constexpr const char* kOpenEndedLengthSource = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data {
    uvec4  head;
    double data[];
};
void main() {
    result[gl_GlobalInvocationID.x] = uint(data.length()) + head.y;
}
)";
} // namespace

TEST_F(FlattenFloat64StorageBlockTest, TheLengthOfAnOpenEndedBlockIsRewrittenToAnElementCount) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, kOpenEndedLengthSource);
    ASSERT_FALSE(input.empty());
    // glslang asks for member 1's length and signs the answer.
    EXPECT_EQ(CountLinesWith(Disassemble(input), "OpArrayLength %uint", " 1"), 1u) << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = ExpectOpenEndedWordArray(output, "Data");
    // Re-aimed at the one member left, ...
    EXPECT_EQ(CountLinesWith(text, "OpArrayLength %uint", " 0"), 1u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpArrayLength %uint", " 1"), 0u) << text;
    // ... rebased past the 4-word prefix, clamped at zero when the range does not reach it, ...
    EXPECT_EQ(CountLinesWith(text, "OpISub %uint", "%uint_4"), 1u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpULessThan %bool", "%uint_4"), 1u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpSelect %uint", "%uint_0"), 1u) << text;
    // ... and divided by the 2-word stride, with glslang's own sign conversion still downstream.
    EXPECT_EQ(CountLinesWith(text, "OpUDiv %uint", "%uint_2"), 1u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpBitcast %int", ""), 1u) << text;
}

TEST_F(FlattenFloat64StorageBlockTest, TheLengthOfABlockWithNoPrefixNeedsNoClamp) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data { dvec2 data[]; };
void main() {
    result[gl_GlobalInvocationID.x] = uint(data.length());
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = ExpectOpenEndedWordArray(output, "Data");
    EXPECT_EQ(CountLinesWith(text, "OpArrayLength %uint", " 0"), 1u) << text;
    // Nothing to subtract, so nothing to clamp: the word count over the 4-word stride is it.
    EXPECT_EQ(CountLinesWith(text, "OpISub", ""), 0u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpSelect", ""), 0u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpUDiv %uint", "%uint_4"), 1u) << text;
}

// The graphics shape of the same CTS group: a fragment stage reading a `readonly` block. The
// NonWritable the qualifier became is a promise about the whole block, and has to be on the one
// member the flattened block keeps.
TEST_F(FlattenFloat64StorageBlockTest, AReadOnlyOpenEndedBlockKeepsNonWritable) {
    const String source = R"(#version 450 core
layout(binding = 4, std430) readonly buffer Buffer4 { dvec3 data[]; };
layout(location = 0) out vec4 o_color;
void main() {
    uint index = uint(gl_FragCoord.x);
    o_color = vec4(float(data[index].z));
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_FRAGMENT_SHADER, source);
    ASSERT_FALSE(input.empty());
    EXPECT_EQ(CountLinesWith(Disassemble(input), "OpMemberDecorate %Buffer4 0 NonWritable", ""), 1u)
        << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = ExpectOpenEndedWordArray(output, "Buffer4");
    EXPECT_EQ(CountLinesWith(text, "OpMemberDecorate %Buffer4 0 NonWritable", ""), 1u) << text;
    // dvec3: stride 8 words, .z at +4.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_8"), 1u) << text;
    EXPECT_GE(CountLinesWith(text, "OpIAdd %uint", "%uint_4"), 1u) << text;
}

// Writing through an open-ended block, which no CTS case does but any shader may: the store
// is decomposed into the same words the load would have read, so the bytes the application
// gets back are the ones GL says it wrote.
TEST_F(FlattenFloat64StorageBlockTest, AnOpenEndedBlockIsWrittenThroughTheSameWords) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 1) buffer Data { double data[]; };
void main() {
    data[gl_LocalInvocationID.x] = double(gl_LocalInvocationID.y);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Data");
    // One dynamic index, scaled to the 2-word element ...
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_2"), 1u) << text;
    // ... and the double left as exactly two word stores, nothing wider.
    EXPECT_EQ(CountLinesWith(text, "OpStore", ""), 2u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpAccessChain %_ptr_StorageBuffer_uint", ""), 2u) << text;
}

// The exact compute shader KHR-Single-GL45.subgroups.arithmetic.compute.subgroupmul_double
// generates, so the CTS shape is pinned as it is and not as a paraphrase of it.
TEST_F(FlattenFloat64StorageBlockTest, TheSubgroupMulDoubleComputeShaderIsFlattened) {
    const String source = R"(#version 450
#extension GL_KHR_shader_subgroup_arithmetic: enable
#extension GL_KHR_shader_subgroup_ballot: enable
layout (local_size_x = 16, local_size_y = 1, local_size_z = 1) in;
layout(binding = 0, std430) buffer Buffer0
{
  uint result[];
};
layout(binding = 1, std430) buffer Buffer1
{
  double data[];
};

void main (void)
{
  uvec3 globalSize = gl_NumWorkGroups * gl_WorkGroupSize;
  highp uint offset = globalSize.x * ((globalSize.y * gl_GlobalInvocationID.z) + gl_GlobalInvocationID.y) + gl_GlobalInvocationID.x;
  uvec4 mask = subgroupBallot(true);
  uint start = 0u, end = gl_SubgroupSize;
  double ref = double(1);
  uint tempResult = 0u;
  for (uint index = start; index < end; index++)
  {
    if (subgroupBallotBitExtract(mask, index))
    {
      ref = ref * data[index];
    }
  }
  tempResult = (abs(ref - subgroupMul(data[gl_SubgroupInvocationID])) < 0.00001) ? 0x1u : 0u;
  if (1u == (gl_SubgroupInvocationID % 2u))
  {
    mask = subgroupBallot(true);
    ref = double(1);
    for (uint index = start; index < end; index++)
    {
      if (subgroupBallotBitExtract(mask, index))
      {
        ref = ref * data[index];
      }
    }
    tempResult |= (abs(ref - subgroupMul(data[gl_SubgroupInvocationID])) < 0.00001) ? 0x2u : 0u;
  }
  else
  {
    tempResult |= 0x2u;
  }
  result[offset] = tempResult;
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Buffer1");
    // Four reads of the array, each scaled to the 2-word element.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_2"), 4u) << text;
    // The result block holds no double and is not the pass's business.
    const Uint32 resultStructId = StructIdNamed(output, "Buffer0");
    ASSERT_NE(resultStructId, 0u) << text;
    const Vector<Uint32> resultMembers = MemberTypesOf(output, resultStructId);
    ASSERT_EQ(resultMembers.size(), 1u);
    EXPECT_TRUE(IsUint32Type(output, RuntimeArrayElementOf(output, resultMembers[0]))) << text;
}

// A runtime array whose element is a MATRIX. The member's own MatrixStride and RowMajor
// decorations describe those elements, so a row-major one has to be declined - its columns are
// not contiguous, and addressing it in column order against a row-major buffer would be silently
// wrong bytes rather than a refusal. The column-major twin must flatten, stepping by the
// element's stride and then by the column's.
TEST_F(FlattenFloat64StorageBlockTest, ARowMajorMatrixRuntimeArrayIsLeftToTheDemotion) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1, row_major) buffer Data { dmat4 data[]; };
void main() {
    result[gl_GlobalInvocationID.x] = uint(data[gl_LocalInvocationID.x][1][2]);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    // The premise: glslang really did mark the member row-major.
    EXPECT_EQ(CountLinesWith(Disassemble(input), "OpMemberDecorate %Data 0 RowMajor", ""), 1u)
        << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = Disassemble(output);
    const Uint32 structId = StructIdNamed(output, "Data");
    ASSERT_NE(structId, 0u) << text;
    const Vector<Uint32> members = MemberTypesOf(output, structId);
    ASSERT_EQ(members.size(), 1u) << text;
    // Still a runtime array of matrices - narrowed to fp32 by the demotion, not re-addressed.
    EXPECT_NE(DecorationValueOf(output, members[0], kDecorationArrayStride), 4u)
        << "a row-major matrix element must not have been flattened into words\n"
        << text;
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", ""), 0u)
        << "nothing should have been re-addressed\n"
        << text;
}

TEST_F(FlattenFloat64StorageBlockTest, AColumnMajorMatrixRuntimeArrayStepsByItsColumnStride) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data { dmat2x4 data[]; };
void main() {
    dvec4 column = data[gl_LocalInvocationID.x][1];
    result[gl_GlobalInvocationID.x] = uint(column.w);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Data");
    // dmat2x4: two columns of dvec4, column stride 32 bytes, so one element is 64 bytes -
    // 16 words - and column 1 starts 8 words into it.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_16"), 1u) << text;
    // Exactly one +8: the column's own offset inside the element. A second would mean a word
    // past the column was being addressed off that same base.
    EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", "%uint_8"), 1u) << text;
    // All eight words of that column are read - the last of its four doubles ends at +7 ...
    EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", "%uint_7"), 1u) << text;
    // ... and the column that was not asked for is not touched: nothing is read at +9 or past.
    EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", "%uint_9"), 0u) << text;
    EXPECT_EQ(CountLinesWith(text, "OpIAdd %uint", "%uint_10"), 0u) << text;
}

// A runtime array whose element is a STRUCT: the same walk, and the same decline test, as a
// bounded array of them - a shape no other open-ended case reaches.
TEST_F(FlattenFloat64StorageBlockTest, AStructRuntimeArrayStepsByItsElementStride) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
struct Pair { double a; float b; };
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data { Pair data[]; };
void main() {
    result[gl_GlobalInvocationID.x] = uint(data[gl_LocalInvocationID.x].a) +
                                      uint(data[gl_LocalInvocationID.x].b);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Data");
    // std430 rounds `{ double a; float b; }` up to its 8-byte alignment: 16 bytes, 4 words,
    // with `b` two words in.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_4"), 2u) << text;
    EXPECT_GE(CountLinesWith(text, "OpIAdd %uint", "%uint_2"), 1u) << text;
}

// The leaf cap bounds ONE load or store, not a member's size: a block whose element is far too
// big to expand whole is still flattened while every access to it names a scalar. Declining it
// would leave the application's 8-byte-stride doubles to the demotion's re-derived stride 4 -
// the exact defect the open-ended shape exists to avoid.
TEST_F(FlattenFloat64StorageBlockTest, AHugeRuntimeArrayElementIsStillFlattenedWhenAccessesAreSmall) {
    const String source = R"(#version 430 core
layout(local_size_x = 16) in;
struct Big { dvec4 v[300]; };
layout(std430, binding = 0) buffer Sink { uint result[]; };
layout(std430, binding = 1) buffer Data { Big data[]; };
void main() {
    result[gl_GlobalInvocationID.x] = uint(data[gl_LocalInvocationID.x].v[3].y);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());

    const String text = ExpectOpenEndedWordArray(output, "Data");
    // 300 dvec4 of 32 bytes each: 9600 bytes, 2400 words per element - 1200 scalars, well past
    // the per-access cap that a whole-element load would have to respect and this never does.
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", "%uint_2400"), 1u) << text;
    // v[3].y is 3 * 8 + 2 = 26 words into the element.
    EXPECT_GE(CountLinesWith(text, "OpIAdd %uint", "%uint_26"), 1u) << text;
}

// The flatten preserves a byte layout ACROSS a narrowing; where the backend consumes 64-bit
// floats itself there is nothing to preserve, and the open-ended block has to keep its runtime
// array of doubles exactly as the driver would lay it out.
TEST_F(FlattenFloat64StorageBlockTest, TheNativePathLeavesAnOpenEndedBlockAndItsDoublesAlone) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, OpenEndedComputeSource("double"));
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true, true));
    ASSERT_FALSE(output.empty());

    const String text = Disassemble(output);
    const Uint32 structId = StructIdNamed(output, "Data");
    ASSERT_NE(structId, 0u) << text;
    const Vector<Uint32> members = MemberTypesOf(output, structId);
    ASSERT_EQ(members.size(), 1u) << text;
    EXPECT_NE(RuntimeArrayElementOf(output, members[0]), 0u)
        << "the member should still be a runtime array\n"
        << text;
    EXPECT_EQ(DecorationValueOf(output, members[0], kDecorationArrayStride), 8u)
        << "the array must keep the 8-byte stride the application bound\n"
        << text;
    EXPECT_GT(CountFloatTypesOfWidth(output, 64), 0u)
        << "nothing narrows here, so the doubles must survive\n"
        << text;
}

// The other backend prints the flattened module through SPIRV-Cross: an open-ended `uint[]`
// member has to come out as ESSL that names no 64-bit type. The `.length()` shape is here too,
// because the OpArrayLength the rewrite re-issues is the one instruction in it whose ESSL
// spelling is not plain arithmetic - if that backend ever refused it on the flattened member,
// a DirectGLES shader asking an fp64 buffer its length would fail at link and nowhere else.
namespace {
    String TranspileToEssl(const Vector<Uint32>& spirv) {
        using namespace MG_Util::ShaderTranspiler;
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        EXPECT_EQ(session.CreateOptions(&options), SPVC_SUCCESS);
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        EXPECT_EQ(session.SetOptions(options), SPVC_SUCCESS);

        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? *essl : String{};
    }
} // namespace

TEST_F(FlattenFloat64StorageBlockTest, AnOpenEndedBlockCanBeEmittedAsEssl) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, OpenEndedComputeSource("dvec4"));
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    ExpectOpenEndedWordArray(output, "Data");

    const String essl = TranspileToEssl(output);
    ASSERT_FALSE(essl.empty());
    EXPECT_EQ(essl.find("double"), String::npos) << essl;
    EXPECT_EQ(essl.find("dvec"), String::npos) << essl;
    EXPECT_NE(essl.find("uint"), String::npos) << essl;
}

TEST_F(FlattenFloat64StorageBlockTest, TheRewrittenLengthCanBeEmittedAsEssl) {
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, kOpenEndedLengthSource);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    ExpectOpenEndedWordArray(output, "Data");

    const String essl = TranspileToEssl(output);
    ASSERT_FALSE(essl.empty());
    EXPECT_EQ(essl.find("double"), String::npos) << essl;
    EXPECT_EQ(essl.find("dvec"), String::npos) << essl;
    // The length survived as a length - it was not folded away or dropped on the floor.
    EXPECT_NE(essl.find(".length()"), String::npos) << essl;
}

// ---------------------------------------------------------------------------
// The gate from the other side: a runtime array anywhere but the block's own last member is a
// shape GLSL cannot spell and this pass does not describe. SPIR-V can spell it, so both are
// hand-written, and both are invalid Vulkan SPIR-V - the chain runs without its validator here,
// which is also why neither can be a validation-failure count.
// ---------------------------------------------------------------------------

namespace {
    // `buffer Odd { double data[]; uint tail; }`, the runtime array FIRST.
    const char* kRuntimeArrayNotLastAsm = R"(
               OpCapability Shader
               OpCapability Float64
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpName %Odd "Odd"
               OpName %var ""
               OpDecorate %_runtimearr_double ArrayStride 8
               OpDecorate %Odd Block
               OpMemberDecorate %Odd 0 Offset 0
               OpMemberDecorate %Odd 1 Offset 8
               OpDecorate %var Binding 0
               OpDecorate %var DescriptorSet 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
     %double = OpTypeFloat 64
   %double_2 = OpConstant %double 2
%_runtimearr_double = OpTypeRuntimeArray %double
        %Odd = OpTypeStruct %_runtimearr_double %uint
%_ptr_StorageBuffer_Odd = OpTypePointer StorageBuffer %Odd
        %var = OpVariable %_ptr_StorageBuffer_Odd StorageBuffer
%_ptr_StorageBuffer_double = OpTypePointer StorageBuffer %double
       %main = OpFunction %void None %3
          %5 = OpLabel
          %6 = OpAccessChain %_ptr_StorageBuffer_double %var %int_0 %int_1
               OpStore %6 %double_2
               OpReturn
               OpFunctionEnd
)";

    // `struct Inner { double data[]; }; buffer Outer { uint head; Inner inner; }`: the runtime
    // array IS last, but of a member rather than of the block.
    const char* kRuntimeArrayNestedAsm = R"(
               OpCapability Shader
               OpCapability Float64
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpName %Outer "Outer"
               OpName %Inner "Inner"
               OpName %var ""
               OpDecorate %_runtimearr_double ArrayStride 8
               OpMemberDecorate %Inner 0 Offset 0
               OpDecorate %Outer Block
               OpMemberDecorate %Outer 0 Offset 0
               OpMemberDecorate %Outer 1 Offset 8
               OpDecorate %var Binding 0
               OpDecorate %var DescriptorSet 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
     %double = OpTypeFloat 64
   %double_2 = OpConstant %double 2
%_runtimearr_double = OpTypeRuntimeArray %double
      %Inner = OpTypeStruct %_runtimearr_double
      %Outer = OpTypeStruct %uint %Inner
%_ptr_StorageBuffer_Outer = OpTypePointer StorageBuffer %Outer
        %var = OpVariable %_ptr_StorageBuffer_Outer StorageBuffer
%_ptr_StorageBuffer_double = OpTypePointer StorageBuffer %double
       %main = OpFunction %void None %3
          %5 = OpLabel
          %6 = OpAccessChain %_ptr_StorageBuffer_double %var %int_1 %int_0 %int_1
               OpStore %6 %double_2
               OpReturn
               OpFunctionEnd
)";

    Vector<Uint32> AssembleUnchecked(const char* asmText) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        Vector<Uint32> module;
        EXPECT_TRUE(tools.Assemble(asmText, &module));
        return module;
    }
} // namespace

TEST_F(FlattenFloat64StorageBlockTest, ARuntimeArrayThatIsNotTheBlocksLastMemberIsLeftToTheDemotion) {
    struct Shape {
        const char* asmText;
        const char* blockName;
    };
    const Shape shapes[] = {{kRuntimeArrayNotLastAsm, "Odd"}, {kRuntimeArrayNestedAsm, "Outer"}};
    for (const Shape& shape : shapes) {
        SCOPED_TRACE(shape.blockName);
        const Vector<Uint32> input = AssembleUnchecked(shape.asmText);
        ASSERT_FALSE(input.empty());

        Vector<Uint32> output;
        ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, false, false));
        ASSERT_FALSE(output.empty());
        const String text = Disassemble(output);

        // Declined: both members are still there, and the demotion narrowed them the old way.
        const Uint32 structId = StructIdNamed(output, shape.blockName);
        ASSERT_NE(structId, 0u) << text;
        EXPECT_EQ(MemberTypesOf(output, structId).size(), 2u) << text;
        EXPECT_EQ(CountFloatTypesOfWidth(output, 64), 0u) << text;
        EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", ""), 0u)
            << "nothing should have been re-addressed\n"
            << text;
    }
}

// ---------------------------------------------------------------------------
// The front end declares types in first-use order, so a block that is the first thing the
// shader touches is declared before the module's `uint` - and the flattened member is an array
// OF `uint`. For an OPEN-ENDED block the pass moves that operand-less type up in front of the
// block rather than declining, so that where a buffer of doubles stands in the shader does not
// decide whether its bytes survive. A BOUNDED block in the same position keeps the decline it
// has always had: widening that is a change to a path this fix does not need, and the pair below
// pins both halves.
// ---------------------------------------------------------------------------

namespace {
    // The position of <id>'s declaration in instruction order, or 0 when it has none.
    Uint32 DeclarationIndexOf(const Vector<Uint32>& spirv, Uint32 id) {
        Uint32 index = 0;
        Uint32 found = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            ++index;
            if (found != 0 || wordCount < 2) return;
            // Every OpType* has its result id in word 1; that is all this is asked about.
            if (opcode >= kOpTypeInt && opcode <= kOpTypeStruct && words[1] == id) found = index;
        });
        return found;
    }

    Uint32 Uint32TypeIdOf(const Vector<Uint32>& spirv) {
        Uint32 typeId = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpTypeInt && wordCount >= 4 && words[2] == 32u && words[3] == 0u) typeId = words[1];
        });
        return typeId;
    }
} // namespace

TEST_F(FlattenFloat64StorageBlockTest, AnOpenEndedBlockDeclaredBeforeTheModulesUintIsStillFlattened) {
    // The block is the first thing main touches, and nothing before it needs a uint - not even
    // an array length, which is a uint constant and would declare one.
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Data { double data[]; };
layout(std430, binding = 1) buffer Sink { float result[]; };
void main() {
    result[0] = float(data[0] + data[1]);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Uint32 inputStructId = StructIdNamed(input, "Data");
    ASSERT_NE(inputStructId, 0u);
    const Uint32 inputUintId = Uint32TypeIdOf(input);
    // The premise: the module's uint really is declared after the block (or not at all).
    ASSERT_TRUE(inputUintId == 0 ||
                DeclarationIndexOf(input, inputUintId) > DeclarationIndexOf(input, inputStructId))
        << "this shader was meant to declare the block before any uint\n"
        << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = ExpectOpenEndedWordArray(output, "Data");
    const Uint32 structId = StructIdNamed(output, "Data");
    ASSERT_NE(structId, 0u) << text;
    const Vector<Uint32> members = MemberTypesOf(output, structId);
    ASSERT_EQ(members.size(), 1u) << text;
    // And the uint now stands in front of the block it is an element of.
    EXPECT_LT(DeclarationIndexOf(output, RuntimeArrayElementOf(output, members[0])),
              DeclarationIndexOf(output, structId))
        << text;
}

TEST_F(FlattenFloat64StorageBlockTest, ABoundedBlockDeclaredBeforeTheModulesUintIsLeftToTheDemotion) {
    // The same position, a bounded block: this is the shape that has always been declined, and
    // it stays declined - its members and the demotion's own repacking come through untouched.
    const String source = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Wide {
    double data0;
    dvec2  data1;
} g_wide;
layout(std430, binding = 1) buffer Sink { float result[]; };
void main() {
    double sum = g_wide.data0 + g_wide.data1.y;
    result[0] = float(sum);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Uint32 inputStructId = StructIdNamed(input, "Wide");
    ASSERT_NE(inputStructId, 0u);
    const Uint32 inputUintId = Uint32TypeIdOf(input);
    ASSERT_TRUE(inputUintId == 0 ||
                DeclarationIndexOf(input, inputUintId) > DeclarationIndexOf(input, inputStructId))
        << "this shader was meant to declare the block before any uint\n"
        << Disassemble(input);

    const Vector<Uint32> output = Sanitize(input);
    ASSERT_FALSE(output.empty());
    const String text = Disassemble(output);
    const Uint32 structId = StructIdNamed(output, "Wide");
    ASSERT_NE(structId, 0u) << text;
    EXPECT_EQ(MemberTypesOf(output, structId).size(), 2u)
        << "a bounded block in this position must keep the decline it shipped with\n"
        << text;
    EXPECT_EQ(CountLinesWith(text, "OpIMul %uint", ""), 0u)
        << "nothing should have been re-addressed\n"
        << text;
}
