// MobileGL - MobileGL/MG_Test/ShaderTranspiler/DemoteFloat64Test.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <sstream>
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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));

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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));
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
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(input, output, true));
    EXPECT_EQ(output, input) << Disassemble(output);
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(output));
}

TEST_F(DemoteFloat64Test, ModuleDeclaresFloat64AnswersBothWays) {
    const Vector<Uint32> wide = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(wide.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(wide));

    Vector<Uint32> demoted;
    ASSERT_TRUE(ShaderCompiler::DemoteFloat64ToFloat32(wide, demoted, true));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64(demoted));

    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64({}));
}

TEST_F(DemoteFloat64Test, TheSharedChainDemotesToo) {
    // Production never calls the pass on its own: it reaches it through the one chain every
    // module goes through at link, on both backends.
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64(output)) << Disassemble(output);
}

// ---------------------------------------------------------------------------
// The capability gate. A backend that consumes 64-bit floats itself gets none of this.
// ---------------------------------------------------------------------------

namespace {
    // Everything kWideVertexSource has except the 64-bit vertex INPUT, which is what the
    // whole-program demotion falls back for. A fragment stage, so there is no input to have.
    constexpr const char* kWideFragmentSource = R"(#version 460 core
layout(std140, binding = 0) uniform Blk {
    float  a;
    double d;
    dvec2  v2;
    dvec4  v4;
    dmat4  m4;
    double arr[3];
};
layout(location = 0) uniform double uScale;
layout(location = 0) in vec3 inNormal;
layout(location = 0) out float fOut;
void main() {
    double s = d * uScale + a;
    s += v2.x + v4.y + m4[0].z + arr[0] + arr[1] + arr[2] + 0.5lf;
    fOut = float(s) + inNormal.x;
}
)";
} // namespace

// THE NEGATIVE CONTROL for the whole change: the identical module through the identical entry
// point answers both ways, and the only thing that moved is the capability argument.
TEST_F(DemoteFloat64Test, TheSharedChainKeepsFloat64WhenTheBackendConsumesIt) {
    const Vector<Uint32> input = CompileToSpirv(GL_FRAGMENT_SHADER, kWideFragmentSource);
    ASSERT_FALSE(input.empty());
    ASSERT_TRUE(DeclaresFloat64Capability(input));
    ASSERT_GT(CountFloatTypesOfWidth(input, 64), 0u);

    Vector<Uint32> native;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, native, true, true, true));
    EXPECT_TRUE(DeclaresFloat64Capability(native)) << Disassemble(native);
    EXPECT_GT(CountFloatTypesOfWidth(native, 64), 0u) << Disassemble(native);
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(native));

    Vector<Uint32> demoted;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, demoted, true, true, false));
    EXPECT_FALSE(DeclaresFloat64Capability(demoted)) << Disassemble(demoted);
    EXPECT_EQ(CountFloatTypesOfWidth(demoted, 64), 0u) << Disassemble(demoted);
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64(demoted));

    EXPECT_NE(native, demoted);
}

// The exception the vertex path needs, at the level ProgramSpirvTask asks it: no backend here can
// FETCH 64 bits, so a stage that declares a Float64 input is demoted whole even where the rest of
// its doubles could have survived.
TEST_F(DemoteFloat64Test, AFloat64VertexInputIsRecognisedAndOnlyOnAVertexStage) {
    const Vector<Uint32> vertexWithDoubleInput = CompileToSpirv(GL_VERTEX_SHADER, kWideVertexSource);
    ASSERT_FALSE(vertexWithDoubleInput.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64VertexInput(vertexWithDoubleInput));

    // Doubles everywhere but the inputs: the same verdict must be false, or nothing would ever
    // take the native path.
    const Vector<Uint32> fragmentWithDoubles = CompileToSpirv(GL_FRAGMENT_SHADER, kWideFragmentSource);
    ASSERT_FALSE(fragmentWithDoubles.empty());
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64VertexInput(fragmentWithDoubles));

    // A vertex stage whose doubles are all internal is fine too - it is the INPUT that cannot be
    // fed, not the stage.
    const String vertexWithoutDoubleInput = R"(#version 460 core
layout(location = 0) uniform double uScale;
layout(location = 0) in vec3 inPos;
layout(location = 0) out float vOut;
void main() {
    double s = double(inPos.x) * uScale + 0.5lf;
    vOut = float(s);
    gl_Position = vec4(float(s));
}
)";
    const Vector<Uint32> internalOnly = CompileToSpirv(GL_VERTEX_SHADER, vertexWithoutDoubleInput);
    ASSERT_FALSE(internalOnly.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresFloat64(internalOnly));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64VertexInput(internalOnly));

    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresFloat64VertexInput({}));
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
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true));

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
    EXPECT_FALSE(ShaderCompiler::DemoteFloat64ToFloat32(notSpirv, output, true));
}

// EliminateFloatEqualsZeroPass re-spells a comparison against 0.0 through GLSL.std.450 FAbs, so
// that no float-equality instruction reaches a driver that gets one wrong. Deciding WHICH
// constants are zero used to read every float constant as though it were 32 bits wide, and on a
// 64-bit constant that reads the LOW half of the mantissa - which is zero for 1.0lf, 2.0lf, 0.5lf
// and every other round double a shader is likely to spell. Each of those was mistaken for 0.0, so
// a comparison against 1.0lf became a test against ZERO, and came out true for a uniform holding
// exactly 1.0. That is the whole of KHR-GL43.compute_shader.fp64-case2.
//
// The replacement itself used to be an epsilon ball, `abs(x) < 1e-4`, which called any legitimately
// small value zero: KHR-GL3x.buffer_objects.triangles computes a specular term of ~6e-5 at a large
// render target and rendered black. It is exact now - `abs(x) <= 0.0` / `abs(x) > 0.0` against the
// module's own zero constant - and the tests below pin both halves of that: only a genuine 0.0 is
// matched, and what the compare tests against is the constant the source itself spelled.
//
// Asserted on the optimized module rather than through a driver, because that is where the
// rewrite happens and its fingerprint there is unambiguous: the rewrite introduces a
// GLSL.std.450 FAbs, and nothing else in these shaders would.
namespace {
    String OptimizedDisassembly(const String& source) {
        const Vector<Uint32> input = CompileToSpirv(GL_COMPUTE_SHADER, source);
        EXPECT_FALSE(input.empty());
        if (input.empty()) return {};
        Vector<Uint32> output;
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(input, output, true, true));
        return Disassemble(output);
    }

    Bool RewritesToAnAbsoluteValueTest(const String& source) {
        return OptimizedDisassembly(source).find("FAbs") != String::npos;
    }

    String CompareAgainstUsing(const String& type, const String& op, const String& literal) {
        return "#version 430 core\n"
               "layout(local_size_x = 1) in;\n"
               "buffer Result { int g_result; };\n"
               "uniform " + type + " g_0;\n"
               "void main() {\n"
               "  g_result = 0;\n"
               "  if (g_0 " + op + " " + literal + ") g_result = 1;\n"
               "}\n";
    }

    String CompareAgainst(const String& type, const String& literal) {
        return CompareAgainstUsing(type, "!=", literal);
    }

    // Every instruction of a disassembly, split into whitespace-separated tokens, so an operand can
    // be identified by position instead of by a substring another opcode might also contain -
    // `OpFOrdLessThan` is a prefix of `OpFOrdLessThanEqual`, and those two are the whole difference
    // between the epsilon rewrite and the exact one.
    Vector<Vector<String>> TokenizedInstructions(const String& disassembly) {
        Vector<Vector<String>> instructions;
        StringStream lines(disassembly);
        String line;
        while (std::getline(lines, line)) {
            Vector<String> tokens;
            StringStream words(line);
            String word;
            while (words >> word) tokens.push_back(word);
            instructions.push_back(tokens);
        }
        return instructions;
    }

    // The compare the rewrite leaves behind, e.g. `%22 = OpFOrdLessThanEqual %bool %21 %float_0`,
    // or an empty vector if the module has none. These four opcodes are the only ones the pass
    // emits and nothing else in these shaders produces one.
    Vector<String> FindRewrittenCompare(const String& disassembly) {
        for (const Vector<String>& tokens : TokenizedInstructions(disassembly)) {
            if (tokens.size() < 6 || tokens[1] != "=") continue;
            if (tokens[2] == "OpFOrdLessThanEqual" || tokens[2] == "OpFUnordLessThanEqual" ||
                tokens[2] == "OpFOrdGreaterThan" || tokens[2] == "OpFUnordGreaterThan") {
                return tokens;
            }
        }
        return {};
    }

    // Result id of the module's 0.0 constant of the type FAbs produces - the constant the source
    // itself spelled - found without assuming what the disassembler names it or how it prints the
    // literal.
    String FindZeroConstantId(const String& disassembly) {
        const Vector<Vector<String>> instructions = TokenizedInstructions(disassembly);
        String floatTypeId;
        for (const Vector<String>& tokens : instructions) {
            if (tokens.size() >= 7 && tokens[2] == "OpExtInst" && tokens[5] == "FAbs") {
                floatTypeId = tokens[3];
                break;
            }
        }
        if (floatTypeId.empty()) return {};

        for (const Vector<String>& tokens : instructions) {
            if (tokens.size() < 5 || tokens[2] != "OpConstant" || tokens[3] != floatTypeId) continue;
            char* end = nullptr;
            const double value = std::strtod(tokens[4].c_str(), &end);
            if (end != nullptr && *end == '\0' && value == 0.0) return tokens[0];
        }
        return {};
    }

    // The shape the pass promises: the given opcode (either NaN half of it), tested against the
    // module's own zero constant rather than against anything this pass invented.
    void ExpectComparedAgainstModuleZero(const String& source, const String& orderedOpcode,
                                         const String& unorderedOpcode) {
        const String disassembly = OptimizedDisassembly(source);
        const Vector<String> compare = FindRewrittenCompare(disassembly);
        ASSERT_FALSE(compare.empty()) << "no rewritten compare in the optimized module\n"
                                      << disassembly;
        EXPECT_TRUE(compare[2] == orderedOpcode || compare[2] == unorderedOpcode)
            << "expected " << orderedOpcode << " (or its unordered twin), got " << compare[2] << "\n"
            << disassembly;

        const String zeroId = FindZeroConstantId(disassembly);
        ASSERT_FALSE(zeroId.empty()) << "the module has no 0.0 constant of the abs() type\n"
                                     << disassembly;
        EXPECT_EQ(compare.back(), zeroId)
            << "the rewrite compares against " << compare.back()
            << " instead of the module's own zero; a synthesized threshold is the epsilon bug\n"
            << disassembly;
    }
} // namespace

TEST_F(DemoteFloat64Test, AComparisonAgainstANonZeroDoubleIsLeftAlone) {
    EXPECT_FALSE(RewritesToAnAbsoluteValueTest(CompareAgainst("double", "1.0LF")))
        << "a double compared against 1.0lf was rewritten into a test against zero";
}

TEST_F(DemoteFloat64Test, AComparisonAgainstZeroIsStillRewritten) {
    EXPECT_TRUE(RewritesToAnAbsoluteValueTest(CompareAgainst("double", "0.0LF")))
        << "the rewrite must still fire for a genuine comparison against zero";
}

TEST_F(DemoteFloat64Test, TheThirtyTwoBitBehaviourIsUnchanged) {
    EXPECT_FALSE(RewritesToAnAbsoluteValueTest(CompareAgainst("float", "1.0")))
        << "a float compared against 1.0 must not be rewritten";
    EXPECT_TRUE(RewritesToAnAbsoluteValueTest(CompareAgainst("float", "0.0")))
        << "the 32-bit behaviour this pass shipped with must be preserved exactly";
}

// The pass matches ZERO, not "small". The old constant-is-zero test was `fabs(v) <= 1e-4`, so a
// float compared against exactly 1e-4 was declared a comparison against zero and rewritten into
// `abs(x) >= 1e-4` - a different question from the one the shader asked, against a constant that
// was never zero to begin with.
TEST_F(DemoteFloat64Test, AComparisonAgainstASmallNonZeroLiteralIsLeftAlone) {
    EXPECT_FALSE(RewritesToAnAbsoluteValueTest(CompareAgainst("float", "0.0001")))
        << "a float compared against 1e-4 was treated as a comparison against zero";
    EXPECT_FALSE(RewritesToAnAbsoluteValueTest(CompareAgainst("double", "0.0001LF")))
        << "the 64-bit accessor must judge the constant just as exactly as the 32-bit one";
}

// What replaces the compare, not just that something did. Both properties here are what makes the
// rewrite exact rather than a tolerance, and neither is visible in the FAbs fingerprint above.
TEST_F(DemoteFloat64Test, TheRewriteComparesAbsAgainstTheModulesOwnZero) {
    // `x == 0.0` -> `abs(x) <= 0.0`. The equality has to be INSIDE the replacement: with a strict
    // `<` and no epsilon left to hide behind, +/-0 would stop comparing equal to zero.
    ExpectComparedAgainstModuleZero(CompareAgainstUsing("float", "==", "0.0"),
                                    "OpFOrdLessThanEqual", "OpFUnordLessThanEqual");
    // `x != 0.0` -> `abs(x) > 0.0`, the strict complement of the above.
    ExpectComparedAgainstModuleZero(CompareAgainstUsing("float", "!=", "0.0"), "OpFOrdGreaterThan",
                                    "OpFUnordGreaterThan");
}
