// MobileGL - MobileGL/MG_Test/ShaderTranspiler/DemoteFloat64Test.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

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
    // The test-side reference walker, deliberately independent of the production code: a bug in
    // the pass must not be able to hide behind the same helper. Counts OpTypeFloat declarations of
    // a given width and collects the Offset literal of every OpMemberDecorate, in module order.
    constexpr Uint32 kSpirvHeaderWordCount = 5;
    constexpr Uint32 kOpTypeFloat = 22;
    constexpr Uint32 kOpName = 5;
    constexpr Uint32 kOpMemberDecorate = 72;
    constexpr Uint32 kOpFConvert = 115;
    constexpr Uint32 kOpCapability = 17;
    constexpr Uint32 kDecorationOffset = 35;
    constexpr Uint32 kCapabilityFloat64 = 10;

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

    Uint32 CountFloatTypesOfWidth(const Vector<Uint32>& spirv, Uint32 width) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpTypeFloat && wordCount >= 3 && words[2] == width) ++count;
        });
        return count;
    }

    Uint32 CountFConverts(const Vector<Uint32>& spirv) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32*, Uint32) {
            if (opcode == kOpFConvert) ++count;
        });
        return count;
    }

    Bool DeclaresFloat64Capability(const Vector<Uint32>& spirv) {
        Bool found = false;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpCapability && wordCount >= 2 && words[1] == kCapabilityFloat64) found = true;
        });
        return found;
    }

    // Byte offset of every member of the struct named `blockName`, in member order.
    Vector<Uint32> CollectOffsetsOf(const Vector<Uint32>& spirv, const String& blockName) {
        Uint32 structId = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != kOpName || wordCount < 3 || structId != 0) return;
            const char* text = reinterpret_cast<const char*>(&words[2]);
            const SizeT maxBytes = (wordCount - 2) * sizeof(Uint32);
            if (std::strncmp(text, blockName.c_str(), maxBytes) == 0) structId = words[1];
        });
        if (structId == 0) return {};

        std::map<Uint32, Uint32> offsetByMember;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpMemberDecorate && wordCount >= 5 && words[1] == structId &&
                words[3] == kDecorationOffset) {
                offsetByMember[words[2]] = words[4];
            }
        });

        Vector<Uint32> offsets;
        for (const auto& [member, offset] : offsetByMember) offsets.push_back(offset);
        return offsets;
    }

    // Everything the production pipeline does to a source before the pass sees it.
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

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        tools.Disassemble(spirv, &text);
        return text;
    }

    // A vertex shader that exercises every shape the pass has to handle at once: a block with
    // double / dvec2 / dvec3 / dvec4 / dmat4 members between two floats (so a shifted offset would
    // be visible), a default-block double uniform, a 64-bit vertex input, a double-typed array, an
    // implicit float->double conversion and an explicit double->float one.
    const char* kWideVertexSource = R"(#version 460 core
layout(std140, binding = 0) uniform Blk {
    float a;
    double d;
    dvec2  v2;
    dvec3  v3;
    dvec4  v4;
    dmat4  m4;
    double arr[3];
    float  z;
};
layout(location = 0) uniform double uScale;
layout(location = 0) in dvec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out float vOut;
void main() {
    double s = d * uScale + a;
    dvec3 p = inPos * v3 + v2.xyx + v4.xyz + dvec3(m4[0].xyz);
    s += p.x + p.y + p.z + arr[0] + arr[1] + arr[2] + z + 0.5lf;
    vOut = float(s) + inNormal.x;
    gl_Position = vec4(float(s));
}
)";
} // namespace

class DemoteFloat64Test : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        ShaderCompiler::SetSpirvValidationEnabled(true);
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        // The wrapper validates its OUTPUT on every run, so this covers every demotion the test
        // performed without any of them having to say so.
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the demoted module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

TEST_F(DemoteFloat64Test, DemotesEveryWidthAndDropsTheCapability) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(input.empty());
    ASSERT_EQ(CountFloatTypesOfWidth(input, 64), 1u) << Disassemble(input);
    ASSERT_TRUE(DeclaresFloat64Capability(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    EXPECT_EQ(CountFloatTypesOfWidth(output, 64), 0u) << Disassemble(output);
    // And exactly one 32-bit float type survives: the merge has to happen, or spirv-val rejects
    // the second declaration.
    EXPECT_EQ(CountFloatTypesOfWidth(output, 32), 1u) << Disassemble(output);
    EXPECT_FALSE(DeclaresFloat64Capability(output));
}

TEST_F(DemoteFloat64Test, RederivesTheStd140LayoutOfADemotedUniformBlock) {
    const String source = R"(#version 460 core
layout(std140, binding = 0) uniform Blk {
    float  a;
    double d;
    dvec2  v2;
    dvec3  v3;
    dvec4  v4;
    dmat4  m4;
    double arr[3];
    float  z;
};
layout(location = 0) out float vOut;
void main() {
    vOut = float(d + v2.x + v3.y + v4.z + m4[2].w + arr[1] + a + z);
    gl_Position = vec4(vOut);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());
    // What glslang laid out for the 64-bit members, which is what an application computing
    // std140 by hand would also get.
    EXPECT_EQ(CollectOffsetsOf(input, "Blk"), (Vector<Uint32>{0, 8, 16, 32, 64, 96, 224, 272}))
        << Disassemble(input);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    // std140 for the demoted members: float at 4, vec2 at 8, vec3 at 16 (aligned like a vec4),
    // vec4 at 32, mat4 at 48 with a 16-byte column stride, the array at 112 with the std140
    // 16-byte element stride, and the trailing float at 160. This is what SPIRV-Cross has to be
    // able to re-derive for GLSL ES, which has no member layout(offset=) to fall back on.
    EXPECT_EQ(CollectOffsetsOf(output, "Blk"), (Vector<Uint32>{0, 4, 8, 16, 32, 48, 112, 160}))
        << Disassemble(output);
    const String text = Disassemble(output);
    EXPECT_NE(text.find("MatrixStride 16"), String::npos) << text;
    EXPECT_NE(text.find("ArrayStride 16"), String::npos) << text;
}

TEST_F(DemoteFloat64Test, RederivesTheStd430LayoutOfADemotedStorageBlock) {
    const String source = R"(#version 460 core
layout(std430, binding = 0) buffer Ssbo {
    double head;
    dvec4  wide;
    double tail[4];
};
layout(location = 0) out float vOut;
void main() {
    vOut = float(head + wide.w + tail[3]);
    gl_Position = vec4(vOut);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());
    EXPECT_EQ(CollectOffsetsOf(input, "Ssbo"), (Vector<Uint32>{0, 32, 64})) << Disassemble(input);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    // std430, so the array packs at its element size rather than being rounded to 16: float at 0,
    // vec4 at 16, float[4] at 32 with a 4-byte stride. A storage block must NOT come out std140,
    // which is the whole reason the packing is chosen per storage class.
    EXPECT_EQ(CollectOffsetsOf(output, "Ssbo"), (Vector<Uint32>{0, 16, 32})) << Disassemble(output);
    EXPECT_NE(Disassemble(output).find("ArrayStride 4"), String::npos) << Disassemble(output);
}

TEST_F(DemoteFloat64Test, LeavesTheLayoutOfABlockWithoutDoublesAlone) {
    const String source = R"(#version 460 core
layout(std140, binding = 0) uniform Blk {
    float a;
    vec3  v3;
    mat4  m4;
};
layout(std140, binding = 1) uniform Wide {
    float w;
    double d;
};
layout(location = 0) out float vOut;
void main() {
    vOut = float(a + v3.y + m4[1].z + float(d) + w);
    gl_Position = vec4(vOut);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());
    const Vector<Uint32> before = CollectOffsetsOf(input, "Blk");
    ASSERT_FALSE(before.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    // Only the block that actually narrowed is re-laid-out. Touching the other one would be
    // churn at best, and a disagreement with glslang's own layout at worst.
    EXPECT_EQ(CollectOffsetsOf(output, "Blk"), before) << Disassemble(output);
    EXPECT_EQ(CollectOffsetsOf(output, "Wide"), (Vector<Uint32>{0, 4})) << Disassemble(output);
}

TEST_F(DemoteFloat64Test, FoldsTheConversionsThatBecameIdentities) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(input.empty());
    ASSERT_GT(CountFConverts(input), 0u) << "the fixture no longer converts between the two widths";

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    // SPIR-V requires the two component widths of an OpFConvert to differ, so every one of them
    // has to be gone: both sides are 32 bits now.
    EXPECT_EQ(CountFConverts(output), 0u) << Disassemble(output);
}

TEST_F(DemoteFloat64Test, NarrowsDoubleConstantsToTheirFloatValue) {
    const String source = R"(#version 460 core
layout(location = 0) out float outValue;
void main() {
    double d = 0.5lf;
    outValue = float(d * 0.25lf);
    gl_Position = vec4(0.0);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));

    // A 64-bit literal is two words wide and a 32-bit one is a single word, so a constant left
    // unconverted is not merely imprecise - it is an unparseable instruction. Disassembling both
    // values proves the re-encode produced the right number, not just the right width.
    const String text = Disassemble(output);
    EXPECT_NE(text.find("OpConstant %float 0.5"), String::npos) << text;
    EXPECT_NE(text.find("OpConstant %float 0.25"), String::npos) << text;
}

TEST_F(DemoteFloat64Test, LeavesAModuleWithoutDoublesByteIdentical) {
    const String source = R"(#version 460 core
layout(location = 0) in vec4 inPos;
void main() { gl_Position = inPos; }
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));
    // The pass reports SuccessWithoutChange here, and SPIRV-Tools asserts (in assert-enabled
    // builds) that such a run round-trips byte-identically.
    EXPECT_EQ(output, input);
}

TEST_F(DemoteFloat64Test, DeclinesAModuleThatBitcastsAcrossTheWidthBoundary) {
    // packDouble2x32 is defined only for a 64-bit result: there is no 32-bit answer to give, and
    // narrowing one side of the surrounding OpBitcast alone produces a module spirv-val rejects.
    // The contract is that such a module comes back untouched rather than broken.
    const String source = R"(#version 460 core
#extension GL_ARB_gpu_shader_fp64 : require
layout(location = 0) uniform uvec2 uPacked;
layout(location = 0) out float outValue;
void main() {
    double d = packDouble2x32(uPacked);
    outValue = float(d);
    gl_Position = vec4(0.0);
}
)";
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, source);
    ASSERT_FALSE(input.empty());
    ASSERT_EQ(CountFloatTypesOfWidth(input, 64), 1u) << Disassemble(input);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output));
    EXPECT_EQ(output, input) << Disassemble(output);
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(output));
}

TEST_F(DemoteFloat64Test, ModuleDeclaresFloat64AnswersBothWays) {
    const Vector<Uint32> wide = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(wide.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(wide));

    Vector<Uint32> demoted;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(wide, demoted));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64(demoted));

    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64({}));
}

TEST_F(DemoteFloat64Test, TheSharedChainDemotesToo) {
    // Production never calls the pass on its own: it reaches it through the one chain every
    // module goes through at link, on both backends.
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64(output)) << Disassemble(output);
}

// The payoff on the Espryt path: SPIRV-Cross throws "FP64 not supported in ES profile" for every
// one of these before demotion, so the program simply could not be transpiled at all.
class DemoteFloat64EsslTest : public DemoteFloat64Test, public ::testing::WithParamInterface<const char*> {};

INSTANTIATE_TEST_SUITE_P(
    Shapes, DemoteFloat64EsslTest,
    ::testing::Values(
        // A double that never reaches an interface: locals and literals only.
        R"(#version 460 core
layout(location = 0) out float vOut;
void main() {
    double s = 0.5lf;
    for (int i = 0; i < 3; ++i) s = s * 1.5lf + 0.25lf;
    vOut = float(s);
    gl_Position = vec4(float(s));
}
)",
        // A default-block double uniform: the glUniform*d path, and the block MobileGL lays out
        // itself.
        R"(#version 460 core
layout(location = 0) uniform double uScale;
layout(location = 1) uniform dvec3 uOffset;
layout(location = 2) uniform dmat4 uTransform;
layout(location = 0) out float vOut;
void main() {
    dvec3 p = uOffset * uScale + dvec3(uTransform[1].xyz);
    vOut = float(p.x + p.y + p.z);
    gl_Position = vec4(float(p.x));
}
)",
        // An application-declared std140 block whose members are 64-bit.
        R"(#version 460 core
layout(std140, binding = 0) uniform Blk {
    float  a;
    double d;
    dvec2  v2;
    dvec3  v3;
    dvec4  v4;
    dmat4  m4;
    double arr[3];
    float  z;
};
layout(location = 0) out float vOut;
void main() {
    double s = d + v2.x + v3.y + v4.z + m4[2].w + arr[1] + a + z;
    vOut = float(s);
    gl_Position = vec4(float(s));
}
)",
        // A 64-bit vertex input, which is what glVertexAttribLFormat feeds.
        R"(#version 460 core
layout(location = 0) in dvec3 inPos;
layout(location = 2) in double inWeight;
layout(location = 0) out float vOut;
void main() {
    vOut = float(inPos.x + inPos.y + inPos.z + inWeight);
    gl_Position = vec4(vOut);
}
)",
        // An std430 storage block, whose double members pack differently again.
        R"(#version 460 core
layout(std430, binding = 0) buffer Ssbo {
    double head;
    dvec4  wide;
    double tail[4];
};
layout(location = 0) out float vOut;
void main() {
    double s = head + wide.w + tail[3];
    vOut = float(s);
    gl_Position = vec4(float(s));
}
)"));

TEST_P(DemoteFloat64EsslTest, TheDemotedModuleCanBeEmittedAsEssl) {
    using namespace MG_Util::ShaderTranspiler;
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, GetParam());
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output));

    SpvcSession session(output, SessionUsageBit::Transpile);
    spvc_compiler_options options;
    ASSERT_EQ(session.CreateOptions(&options), SPVC_SUCCESS);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
    ASSERT_EQ(session.SetOptions(options), SPVC_SUCCESS);

    auto essl = ShaderCompiler::DecompileShader(session);
    ASSERT_TRUE(essl) << essl.error().log;
    EXPECT_NE(essl->find("#version 320 es"), String::npos) << *essl;
    // ESSL has no 64-bit float spelling at all, so any of these in the output is SPIRV-Cross
    // having emitted something no ES driver will compile.
    EXPECT_EQ(essl->find("double"), String::npos) << *essl;
    EXPECT_EQ(essl->find("dvec"), String::npos) << *essl;
    EXPECT_EQ(essl->find("dmat"), String::npos) << *essl;
}

TEST_F(DemoteFloat64Test, RejectsGarbageInput) {
    const Vector<Uint32> notSpirv{0xdeadbeefu, 0u, 0u, 0u, 0u};
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::DemoteFloat64ToFloat32(notSpirv, output));
}
