// MobileGL - MobileGL/MG_Test/ShaderTranspiler/ClampMultisampleFetchTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ClampMultisampleFetchPass exists because MobileGL advertises one multisample ceiling and the ES
// driver underneath delivers another. GL 4.6 core table 23.53 forces GL_MAX_SAMPLES and
// GL_MAX_INTEGER_SAMPLES up to 4; Adreno and Mali back an integer multisample texture with ONE
// sample, and DirectGLES quietly allocates that (ClampSamplesToBackendSupport). A CTS shader that
// bakes in `texelFetch(usampler2DMS, coord, 3)` - which is what
// KHR-GL33/40/41.texture_swizzle.functional_* and KHR-GLxx.texture_size_promotion.functional do -
// then reads a sample the storage does not have.
//
// So what has to hold is per-fetch and per-category at once: the squeezed category's Sample
// operand must come back in range, a category that is not squeezed must be untouched, a module
// with no multisampled image at all must come out byte for byte as it went in, and every result
// must still be a valid module. Real GLSL through the same glslang path the backends use, for the
// same reason LowerViewportIndexTest.cpp does it: what matters is what glslang actually emits.

#include <gtest/gtest.h>

#define SPV_ENABLE_UTILITY_CODE
#include "glslang/SPIRV/spirv.hpp11"
#undef SPV_ENABLE_UTILITY_CODE

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

#include <map>
#include <string>
#include <vector>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    // GLSL.std.450 instruction number (see 3rdparty/glslang/SPIRV/GLSL.std.450.h). The signed
    // minimum, which is what a GLSL `int` sample index asks for.
    constexpr Uint32 kGlslStd450SMin = 39u;

    // What MobileGL tells the application GL_MAX_SAMPLES / GL_MAX_INTEGER_SAMPLES are, i.e.
    // GL_Getter's kFrontendMaxSamples floor. Each test supplies its own backend-real ceilings
    // against it; Adreno and Mali's Immortalis-G925 both really answer 1 for integer formats.
    constexpr Int32 kAdvertisedMaxSamples = 4;

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

    Vector<Uint32> CompileFragment(const String& source) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
        if (!shaderResult) return {};

        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER},
                                         .program = *programResult.value()};
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

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer(
            [](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
                ADD_FAILURE() << "spirv-val at word " << position.index << ": " << message;
            });
        return tools.Validate(spirv);
    }

    // OpImageFetch words: 0 opcode/count, 1 result type, 2 result id, 3 image, 4 coordinate,
    // 5 the optional image-operands mask, 6.. the ids that mask asks for.
    struct ImageFetch {
        Uint32 resultId = 0u;
        Uint32 imageId = 0u;
        Uint32 mask = 0u;
        Vector<Uint32> maskOperandIds;
    };

    Vector<ImageFetch> CollectImageFetches(const Vector<Uint32>& spirv) {
        Vector<ImageFetch> fetches;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpImageFetch || wordCount < 5u) return;
            ImageFetch fetch{};
            fetch.resultId = words[2];
            fetch.imageId = words[3];
            if (wordCount > 5u) {
                fetch.mask = words[5];
                for (Uint32 word = 6u; word < wordCount; ++word) {
                    fetch.maskOperandIds.push_back(words[word]);
                }
            }
            fetches.push_back(fetch);
        });
        return fetches;
    }

    // OpExtInst words: 0 opcode/count, 1 result type, 2 result id, 3 set, 4 instruction number,
    // 5.. the operand ids.
    struct ExtInst {
        Uint32 resultId = 0u;
        Uint32 instructionNumber = 0u;
        Vector<Uint32> operandIds;
    };

    Vector<ExtInst> CollectExtInsts(const Vector<Uint32>& spirv) {
        Vector<ExtInst> extInsts;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpExtInst || wordCount < 5u) return;
            ExtInst extInst{};
            extInst.resultId = words[2];
            extInst.instructionNumber = words[4];
            for (Uint32 word = 5u; word < wordCount; ++word) {
                extInst.operandIds.push_back(words[word]);
            }
            extInsts.push_back(extInst);
        });
        return extInsts;
    }

    std::map<Uint32, Uint32> CollectScalarConstants(const Vector<Uint32>& spirv) {
        std::map<Uint32, Uint32> values;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == spv::Op::OpConstant && wordCount == 4u) values[words[2]] = words[3];
        });
        return values;
    }

    // The one fetch carrying an explicit Sample operand. glslang emits Sample on its own for a
    // multisample texelFetch - there is no texelFetchOffset for a multisampled sampler - so the
    // sample id is the mask's first and only operand.
    const ImageFetch* FindSampleCarryingFetch(const Vector<ImageFetch>& fetches) {
        for (const ImageFetch& fetch : fetches) {
            if ((fetch.mask & static_cast<Uint32>(spv::ImageOperandsMask::Sample)) != 0u) {
                return &fetch;
            }
        }
        return nullptr;
    }

    const ImageFetch* FindLodCarryingFetch(const Vector<ImageFetch>& fetches) {
        for (const ImageFetch& fetch : fetches) {
            if ((fetch.mask & static_cast<Uint32>(spv::ImageOperandsMask::Lod)) != 0u) {
                return &fetch;
            }
        }
        return nullptr;
    }

    // KHR-GL4x.texture_swizzle.functional's integer multisample read in miniature: the sample
    // index is the advertised GL_MAX_INTEGER_SAMPLES - 1, baked in as a literal, which is exactly
    // the value the one-sample allocation underneath cannot answer. The plain sampler2D fetch is
    // the negative control - a NON-multisampled image whose Lod operand this pass must not touch.
    const char* const kIntegerMultisampleFetch = R"(#version 410 core
uniform usampler2DMS uintMs;
uniform sampler2D plain;
out vec4 fragColor;
void main() {
    uvec4 texel = texelFetch(uintMs, ivec2(gl_FragCoord.xy), 3);
    vec4 other = texelFetch(plain, ivec2(gl_FragCoord.xy), 0);
    fragColor = vec4(texel) * 0.5 + other;
}
)";

    // The colour class, which real devices squeeze to something above 1 rather than to 1.
    const char* const kColorMultisampleFetch = R"(#version 410 core
uniform sampler2DMS colorMs;
out vec4 fragColor;
void main() {
    fragColor = texelFetch(colorMs, ivec2(gl_FragCoord.xy), 3);
}
)";

    // Every stage on a squeezed device goes through the probe, so the one that declares no
    // multisampled image has to come back untouched.
    const char* const kNoMultisampleFetch = R"(#version 410 core
uniform sampler2D plain;
out vec4 fragColor;
void main() {
    fragColor = texelFetch(plain, ivec2(gl_FragCoord.xy), 0);
}
)";
} // namespace

class ClampMultisampleFetchTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the clamped module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

// The probe is the gate that keeps every ordinary stage off an optimizer round trip, so it has to
// answer no for a shader that never reads a multisample texture - and yes for the ones that do.
TEST_F(ClampMultisampleFetchTest, TheProbeAnswersOnlyForAMultisampledImage) {
    const Vector<Uint32> plain = CompileFragment(kNoMultisampleFetch);
    ASSERT_FALSE(plain.empty());
    EXPECT_FALSE(ShaderCompiler::DeclaresMultisampledImage(plain));

    const Vector<Uint32> integerMs = CompileFragment(kIntegerMultisampleFetch);
    ASSERT_FALSE(integerMs.empty());
    EXPECT_TRUE(ShaderCompiler::DeclaresMultisampledImage(integerMs));

    const Vector<Uint32> colorMs = CompileFragment(kColorMultisampleFetch);
    ASSERT_FALSE(colorMs.empty());
    EXPECT_TRUE(ShaderCompiler::DeclaresMultisampledImage(colorMs));

    // Runs on every stage of every program on a squeezed device, so it must survive a stage that
    // produced no SPIR-V rather than pushing a parse diagnostic for it.
    EXPECT_FALSE(ShaderCompiler::DeclaresMultisampledImage({}));
}

// The overwhelming majority of modules. Behind the probe they never reach the pass at all, but the
// pass has to be inert for them on its own, or a future caller that forgets the gate silently
// re-serialises every shader in the program.
TEST_F(ClampMultisampleFetchTest, LeavesAModuleWithoutAMultisampledImageUntouched) {
    const Vector<Uint32> input = CompileFragment(kNoMultisampleFetch);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ClampMultisampleFetchesForEssl(
        input, output, /*maxColorSamples=*/4, /*maxIntegerSamples=*/1, /*maxDepthSamples=*/4,
        kAdvertisedMaxSamples, true));
    EXPECT_EQ(output, input) << Disassemble(output);
}

// The bug itself. GL_MAX_INTEGER_SAMPLES says 4, the texture has one sample, and the shader asks
// for sample 3.
TEST_F(ClampMultisampleFetchTest, ReplacesAnOutOfRangeIntegerSampleWithZero) {
    const Vector<Uint32> input = CompileFragment(kIntegerMultisampleFetch);
    ASSERT_FALSE(input.empty());

    const Vector<ImageFetch> before = CollectImageFetches(input);
    ASSERT_EQ(before.size(), 2u) << Disassemble(input);
    const ImageFetch* sampleBefore = FindSampleCarryingFetch(before);
    const ImageFetch* lodBefore = FindLodCarryingFetch(before);
    ASSERT_NE(sampleBefore, nullptr) << Disassemble(input);
    ASSERT_NE(lodBefore, nullptr) << Disassemble(input);
    ASSERT_EQ(sampleBefore->maskOperandIds.size(), 1u);
    const std::map<Uint32, Uint32> constantsBefore = CollectScalarConstants(input);
    ASSERT_EQ(constantsBefore.count(sampleBefore->maskOperandIds.front()), 1u);
    EXPECT_EQ(constantsBefore.at(sampleBefore->maskOperandIds.front()), 3u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ClampMultisampleFetchesForEssl(
        input, output, /*maxColorSamples=*/4, /*maxIntegerSamples=*/1, /*maxDepthSamples=*/4,
        kAdvertisedMaxSamples, true));
    ASSERT_FALSE(output.empty());
    const String dis = Disassemble(output);
    ASSERT_TRUE(Validates(output)) << dis;

    const Vector<ImageFetch> after = CollectImageFetches(output);
    ASSERT_EQ(after.size(), 2u) << dis;
    const ImageFetch* sampleAfter = FindSampleCarryingFetch(after);
    ASSERT_NE(sampleAfter, nullptr) << dis;
    ASSERT_EQ(sampleAfter->maskOperandIds.size(), 1u) << dis;

    // Sample 0 is the only one a one-sample allocation has - and it is a CONSTANT, not a computed
    // minimum: at K == 1 there is nothing to compare against. An id that resolves in the constant
    // table cannot also be some OpExtInst's result.
    const std::map<Uint32, Uint32> constantsAfter = CollectScalarConstants(output);
    ASSERT_EQ(constantsAfter.count(sampleAfter->maskOperandIds.front()), 1u) << dis;
    EXPECT_EQ(constantsAfter.at(sampleAfter->maskOperandIds.front()), 0u) << dis;

    // The float sampler2D in the same module is not multisampled, so its Lod fetch has to come
    // through with the same image, the same mask and the same operand.
    const ImageFetch* lodAfter = FindLodCarryingFetch(after);
    ASSERT_NE(lodAfter, nullptr) << dis;
    EXPECT_EQ(lodAfter->imageId, lodBefore->imageId) << dis;
    EXPECT_EQ(lodAfter->mask, lodBefore->mask) << dis;
    EXPECT_EQ(lodAfter->maskOperandIds, lodBefore->maskOperandIds) << dis;
}

// The same shader on a device whose integer ceiling really is what MobileGL advertises. Nothing is
// out of range, so nothing may be rewritten - and the module must not even be re-serialised.
TEST_F(ClampMultisampleFetchTest, LeavesTheFetchAloneWhenTheCategoryReachesTheAdvertisedMaximum) {
    const Vector<Uint32> input = CompileFragment(kIntegerMultisampleFetch);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ClampMultisampleFetchesForEssl(
        input, output, /*maxColorSamples=*/4, /*maxIntegerSamples=*/4, /*maxDepthSamples=*/4,
        kAdvertisedMaxSamples, true));
    EXPECT_EQ(output, input) << Disassemble(output);
}

// A category squeezed to something above 1 cannot be answered with a constant: an index the
// allocation does have must survive, so only the upper bound moves.
TEST_F(ClampMultisampleFetchTest, ClampsAColorSampleWithAMinimum) {
    const Vector<Uint32> input = CompileFragment(kColorMultisampleFetch);
    ASSERT_FALSE(input.empty());

    const Vector<ImageFetch> before = CollectImageFetches(input);
    ASSERT_EQ(before.size(), 1u) << Disassemble(input);
    ASSERT_EQ(before.front().maskOperandIds.size(), 1u);
    const Uint32 originalSampleId = before.front().maskOperandIds.front();

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ClampMultisampleFetchesForEssl(
        input, output, /*maxColorSamples=*/2, /*maxIntegerSamples=*/4, /*maxDepthSamples=*/4,
        kAdvertisedMaxSamples, true));
    ASSERT_FALSE(output.empty());
    const String dis = Disassemble(output);
    ASSERT_TRUE(Validates(output)) << dis;

    const Vector<ImageFetch> after = CollectImageFetches(output);
    ASSERT_EQ(after.size(), 1u) << dis;
    ASSERT_EQ(after.front().maskOperandIds.size(), 1u) << dis;
    const Uint32 clampedSampleId = after.front().maskOperandIds.front();
    EXPECT_NE(clampedSampleId, originalSampleId) << dis;

    const Vector<ExtInst> extInsts = CollectExtInsts(output);
    const ExtInst* minimum = nullptr;
    for (const ExtInst& extInst : extInsts) {
        if (extInst.resultId == clampedSampleId) minimum = &extInst;
    }
    ASSERT_NE(minimum, nullptr) << dis;
    EXPECT_EQ(minimum->instructionNumber, kGlslStd450SMin) << dis;
    ASSERT_EQ(minimum->operandIds.size(), 2u) << dis;
    EXPECT_EQ(minimum->operandIds[0], originalSampleId) << dis;

    // min(sample, K - 1), i.e. the last sample a two-sample allocation has.
    const std::map<Uint32, Uint32> constants = CollectScalarConstants(output);
    ASSERT_EQ(constants.count(minimum->operandIds[1]), 1u) << dis;
    EXPECT_EQ(constants.at(minimum->operandIds[1]), 1u) << dis;
}
