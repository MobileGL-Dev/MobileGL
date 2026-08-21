// MobileGL - MobileGL/MG_Test/ShaderTranspiler/WidenImageFormatsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// WidenImageFormatsPass exists because GL has forty image formats and GLSL ES core has thirteen,
// and no device MobileGL runs on advertises GL_NV_image_formats - so a shader declaring one of the
// other twenty-six has no legal ESSL spelling at all. SPIRV-Cross throws for some of them and the
// driver rejects the token for the rest ("'rg32f' : not a legal layout qualifier id"), and dropping
// the qualifier is refused too ("all images have to define layout format"), so the stage is lost
// and every draw with the program silently renders nothing while GL_LINK_STATUS still says TRUE.
//
// What has to hold is the emulation's exactness, in three parts at once: the DECLARED format must
// become the core carrier of the same per-channel width, every imageStore through it must have its
// surplus components replaced by GL's own (0.., 1) so the carrier's extra channels never hold
// anything GL has not defined, and every imageLoad must come back masked the same way. A module
// that declares only core formats - or one of the nine formats with no exact carrier - must come
// out untouched, because widening those would be an approximation rather than an emulation. Real
// GLSL through the same glslang path the backends use, for the same reason
// ClampMultisampleFetchTest.cpp does it: what matters is what glslang actually emits.

#include <gtest/gtest.h>

#define SPV_ENABLE_UTILITY_CODE
#include "glslang/SPIRV/spirv.hpp11"
#undef SPV_ENABLE_UTILITY_CODE

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

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

    bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        tools.SetMessageConsumer(
            [](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
                ADD_FAILURE() << "spirv-val at word " << position.index << ": " << message;
            });
        return tools.Validate(spirv);
    }

    // OpTypeImage words: 0 opcode/count, 1 result id, 2 sampled type, 3 Dim, 4 Depth, 5 Arrayed,
    // 6 MS, 7 Sampled, 8 Format. Sampled == 2 is a storage image, the only kind with a format.
    struct StorageImageType {
        Uint32 resultId = 0u;
        Uint32 format = 0u;
    };

    Vector<StorageImageType> CollectStorageImageTypes(const Vector<Uint32>& spirv) {
        Vector<StorageImageType> types;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpTypeImage || wordCount < 9u) return;
            if (words[7] != 2u) return;
            types.push_back(StorageImageType{words[1], words[8]});
        });
        return types;
    }

    // OpVectorShuffle words: 0 opcode/count, 1 result type, 2 result id, 3 vector 1, 4 vector 2,
    // 5.. the component selectors.
    struct VectorShuffle {
        Uint32 resultId = 0u;
        Uint32 firstVectorId = 0u;
        Uint32 secondVectorId = 0u;
        Vector<Uint32> components;
    };

    Vector<VectorShuffle> CollectVectorShuffles(const Vector<Uint32>& spirv) {
        Vector<VectorShuffle> shuffles;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpVectorShuffle || wordCount < 5u) return;
            VectorShuffle shuffle{};
            shuffle.resultId = words[2];
            shuffle.firstVectorId = words[3];
            shuffle.secondVectorId = words[4];
            for (Uint32 word = 5u; word < wordCount; ++word) {
                shuffle.components.push_back(words[word]);
            }
            shuffles.push_back(shuffle);
        });
        return shuffles;
    }

    // OpImageWrite words: 0 opcode/count, 1 image, 2 coordinate, 3 texel.
    Vector<Uint32> CollectImageWriteTexelIds(const Vector<Uint32>& spirv) {
        Vector<Uint32> texels;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpImageWrite || wordCount < 4u) return;
            texels.push_back(words[3]);
        });
        return texels;
    }

    // OpImageRead words: 0 opcode/count, 1 result type, 2 result id, 3 image, 4 coordinate.
    Vector<Uint32> CollectImageReadResultIds(const Vector<Uint32>& spirv) {
        Vector<Uint32> results;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpImageRead || wordCount < 5u) return;
            results.push_back(words[2]);
        });
        return results;
    }

    Bool HasComponents(const VectorShuffle& shuffle, const Vector<Uint32>& expected) {
        return shuffle.components == expected;
    }

    const VectorShuffle* FindShuffleWithResult(const Vector<VectorShuffle>& shuffles, Uint32 resultId) {
        for (const VectorShuffle& shuffle : shuffles) {
            if (shuffle.resultId == resultId) return &shuffle;
        }
        return nullptr;
    }

    const VectorShuffle* FindShuffleOver(const Vector<VectorShuffle>& shuffles, Uint32 firstVectorId) {
        for (const VectorShuffle& shuffle : shuffles) {
            if (shuffle.firstVectorId == firstVectorId) return &shuffle;
        }
        return nullptr;
    }

    // The ESSL SPIRV-Cross emits for a module, or the error it refused with - which is the whole
    // point for the formats in its is_desktop_only_format set: it THROWS rather than printing a
    // token, and the throw takes the stage with it.
    struct EsslAttempt {
        Bool succeeded = false;
        String text;
        String error;
    };

    EsslAttempt EmitEssl(const Vector<Uint32>& spirv) {
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        EsslAttempt attempt;
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        if (session.CreateOptions(&options) != SPVC_SUCCESS) return attempt;
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        if (session.SetOptions(options) != SPVC_SUCCESS) return attempt;
        auto essl = ShaderCompiler::DecompileShader(session);
        if (!essl) {
            attempt.error = essl.error().log;
            return attempt;
        }
        attempt.succeeded = true;
        attempt.text = *essl;
        return attempt;
    }

    // rg32f: two float channels, and the entry the four CTS allFormats walkers abort on. Both an
    // imageLoad and an imageStore, so both masks are exercised on one image.
    const char* const kRg32fLoadStore = R"(#version 430 core
layout(rg32f, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(1.0, 2.0, 3.0, 4.0));
    fragColor = texel;
}
)";

    // r8ui: ONE unsigned-integer channel, and the only format
    // KHR-GL43.shader_image_load_store.single-byte_data_alignment declares. SPIRV-Cross refuses to
    // print this one for ESSL at all, so before the widening the stage produced no text whatsoever.
    const char* const kR8uiLoadStore = R"(#version 430 core
layout(r8ui, binding = 0) uniform uimage2D img;
out vec4 fragColor;
void main() {
    uvec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), uvec4(7u, 8u, 9u, 10u));
    fragColor = vec4(texel);
}
)";

    // rgba32f is one of the thirteen GLSL ES already has; nothing may move.
    const char* const kCoreFormatLoadStore = R"(#version 430 core
layout(rgba32f, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(1.0, 2.0, 3.0, 4.0));
    fragColor = texel;
}
)";

    // rg16 is one of the NINE with no core carrier of the same per-channel width. Widening it
    // would change the quantisation an application sees, so it must be left alone and keep the
    // honest "no GLSL ES spelling" diagnostic instead.
    const char* const kRg16LoadStore = R"(#version 430 core
layout(rg16, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(1.0, 2.0, 3.0, 4.0));
    fragColor = texel;
}
)";
} // namespace

// The table itself, which is the single source of truth all three layers of the emulation ask -
// the shader rewrite, the ES texture storage and the glBindImageTexture argument. If it drifts
// the three stop agreeing, and a narrow texture read through a wide image goes out of bounds
// silently on every driver tested.
TEST(WidenImageFormats, SeventeenNonCoreFormatsHaveAnExactSameWidthCarrier) {
    struct Case {
        Uint requested;
        Uint carrier;
        Uint channels;
        const char* name;
    };
    const Case cases[] = {
        {0x8230, 0x8814, 2, "GL_RG32F -> GL_RGBA32F"},
        {0x822F, 0x881A, 2, "GL_RG16F -> GL_RGBA16F"},
        {0x822D, 0x881A, 1, "GL_R16F -> GL_RGBA16F"},
        {0x822B, 0x8058, 2, "GL_RG8 -> GL_RGBA8"},
        {0x8229, 0x8058, 1, "GL_R8 -> GL_RGBA8"},
        {0x8F95, 0x8F97, 2, "GL_RG8_SNORM -> GL_RGBA8_SNORM"},
        {0x8F94, 0x8F97, 1, "GL_R8_SNORM -> GL_RGBA8_SNORM"},
        {0x823B, 0x8D82, 2, "GL_RG32I -> GL_RGBA32I"},
        {0x8239, 0x8D88, 2, "GL_RG16I -> GL_RGBA16I"},
        {0x8233, 0x8D88, 1, "GL_R16I -> GL_RGBA16I"},
        {0x8237, 0x8D8E, 2, "GL_RG8I -> GL_RGBA8I"},
        {0x8231, 0x8D8E, 1, "GL_R8I -> GL_RGBA8I"},
        {0x823C, 0x8D70, 2, "GL_RG32UI -> GL_RGBA32UI"},
        {0x823A, 0x8D76, 2, "GL_RG16UI -> GL_RGBA16UI"},
        {0x8234, 0x8D76, 1, "GL_R16UI -> GL_RGBA16UI"},
        {0x8238, 0x8D7C, 2, "GL_RG8UI -> GL_RGBA8UI"},
        {0x8232, 0x8D7C, 1, "GL_R8UI -> GL_RGBA8UI"},
    };
    for (const Case& testCase : cases) {
        EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(testCase.requested), testCase.carrier)
            << testCase.name;
        EXPECT_EQ(ShaderCompiler::ImageFormatChannelCount(testCase.requested), testCase.channels)
            << testCase.name;
        // Every carrier is one of the thirteen ES has in core, or the widening would have moved
        // the problem rather than solved it - and every carrier has four channels, or the mask
        // selectors would address components that are not there.
        EXPECT_TRUE(ShaderCompiler::GLInternalFormatIsCoreEsslImageFormat(testCase.carrier))
            << testCase.name;
        EXPECT_EQ(ShaderCompiler::ImageFormatChannelCount(testCase.carrier), 4u) << testCase.name;
    }
}

TEST(WidenImageFormats, CoreFormatsAndTheNineWithoutAnExactCarrierAreRefused) {
    // The thirteen GLSL ES already has: nothing to carry.
    for (const Uint coreFormat : {0x8814u /*RGBA32F*/, 0x881Au /*RGBA16F*/, 0x822Eu /*R32F*/,
                                  0x8058u /*RGBA8*/, 0x8F97u /*RGBA8_SNORM*/, 0x8D82u /*RGBA32I*/,
                                  0x8D88u /*RGBA16I*/, 0x8D8Eu /*RGBA8I*/, 0x8235u /*R32I*/,
                                  0x8D70u /*RGBA32UI*/, 0x8D76u /*RGBA16UI*/, 0x8D7Cu /*RGBA8UI*/,
                                  0x8236u /*R32UI*/}) {
        EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(coreFormat), 0u)
            << "core format 0x" << std::hex << coreFormat;
    }
    // The nine with no core format of the same per-channel width. Carrying these would be an
    // approximation - a different quantisation, or a different numeric domain for anything that
    // samples the same texture - so they are deliberately left to the honest diagnostic.
    for (const Uint hardFormat : {0x8C3Au /*R11F_G11F_B10F*/, 0x8059u /*RGB10_A2*/,
                                  0x906Fu /*RGB10_A2UI*/, 0x805Bu /*RGBA16*/, 0x822Cu /*RG16*/,
                                  0x822Au /*R16*/, 0x8F9Bu /*RGBA16_SNORM*/, 0x8F99u /*RG16_SNORM*/,
                                  0x8F98u /*R16_SNORM*/}) {
        EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(hardFormat), 0u)
            << "format without an exact carrier 0x" << std::hex << hardFormat;
    }
    // Not an image format at all.
    EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(0x8051 /*GL_RGB8*/), 0u);
    EXPECT_EQ(ShaderCompiler::ImageFormatChannelCount(0x8051 /*GL_RGB8*/), 0u);
    EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(0), 0u);
}

TEST(WidenImageFormats, TwoChannelFloatImageBecomesRgba32fWithBothAccessesMasked) {
    const Vector<Uint32> spirv = CompileFragment(kRg32fLoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));
    // ...and there is nothing left for a second run to do.
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(widened));

    const auto beforeTypes = CollectStorageImageTypes(spirv);
    ASSERT_EQ(beforeTypes.size(), 1u);
    EXPECT_EQ(beforeTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rg32f));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba32f));

    const auto shuffles = CollectVectorShuffles(widened);

    // The STORE. GL drops the components a two-channel format does not have, so the carrier's
    // blue and alpha must be written as its own 0 and 1, never as what the shader passed.
    const auto texelIds = CollectImageWriteTexelIds(widened);
    ASSERT_EQ(texelIds.size(), 1u);
    const VectorShuffle* storeMask = FindShuffleWithResult(shuffles, texelIds.front());
    ASSERT_NE(storeMask, nullptr) << "the imageStore texel is not a masked value";
    EXPECT_TRUE(HasComponents(*storeMask, {0u, 1u, 6u, 7u}))
        << "expected (r, g, 0, 1) - components 0 and 1 of the texel, then 2 and 3 of (0,0,0,1)";

    // The LOAD. Same mask, on the other side: GL defines an imageLoad from a two-channel format
    // as (r, g, 0, 1) whatever the storage holds, which matters for storage this shader never
    // wrote (glTexStorage with no upload leaves the surplus channels undefined).
    const auto readIds = CollectImageReadResultIds(widened);
    ASSERT_EQ(readIds.size(), 1u);
    const VectorShuffle* loadMask = FindShuffleOver(shuffles, readIds.front());
    ASSERT_NE(loadMask, nullptr) << "the imageLoad result is consumed unmasked";
    EXPECT_TRUE(HasComponents(*loadMask, {0u, 1u, 6u, 7u}));
    EXPECT_NE(loadMask->resultId, readIds.front())
        << "the mask must be a separate value, or it would feed itself";
}

TEST(WidenImageFormats, SingleChannelUnsignedImageBecomesRgba8uiWithBothAccessesMasked) {
    const Vector<Uint32> spirv = CompileFragment(kR8uiLoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba8ui));

    const auto shuffles = CollectVectorShuffles(widened);
    const auto texelIds = CollectImageWriteTexelIds(widened);
    ASSERT_EQ(texelIds.size(), 1u);
    const VectorShuffle* storeMask = FindShuffleWithResult(shuffles, texelIds.front());
    ASSERT_NE(storeMask, nullptr);
    // Only red survives; green and blue take the constant's zeroes and alpha its one - the
    // INTEGER one, not a saturated field, which is what makes the uvec4 constant's fourth
    // component 1 rather than 0xFF.
    EXPECT_TRUE(HasComponents(*storeMask, {0u, 5u, 6u, 7u}));

    const auto readIds = CollectImageReadResultIds(widened);
    ASSERT_EQ(readIds.size(), 1u);
    const VectorShuffle* loadMask = FindShuffleOver(shuffles, readIds.front());
    ASSERT_NE(loadMask, nullptr);
    EXPECT_TRUE(HasComponents(*loadMask, {0u, 5u, 6u, 7u}));
}

// The point of the whole exercise, end to end: what reaches the ES driver.
//
// r8ui is in SPIRV-Cross's is_desktop_only_format set, so for an ESSL target it THROWS instead of
// printing a token and no text is produced at all - which is what
// KHR-GL43.shader_image_load_store.single-byte_data_alignment hit ("Attempting to use image format
// not supported in ES profile"), leaving a program that linked and drew nothing. rg32f is the
// other failure mode: SPIRV-Cross prints it happily and the DRIVER rejects it ("'rg32f' : not a
// legal layout qualifier id"). After the widening both come out naming a core format, which is the
// only thing on either side that makes the stage compilable.
TEST(WidenImageFormats, WidenedModulesEmitEsslNamingTheCoreCarrier) {
    {
        const Vector<Uint32> spirv = CompileFragment(kR8uiLoadStore);
        ASSERT_FALSE(spirv.empty());
        const EsslAttempt before = EmitEssl(spirv);
        EXPECT_FALSE(before.succeeded)
            << "SPIRV-Cross printed r8ui for an ES target; the widening's premise has changed:\n"
            << before.text;

        Vector<Uint32> widened;
        ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, false, true));
        const EsslAttempt after = EmitEssl(widened);
        ASSERT_TRUE(after.succeeded) << after.error;
        EXPECT_NE(after.text.find("rgba8ui"), String::npos) << after.text;
        // "rgba8ui" does not contain "r8ui", so this is a clean negative.
        EXPECT_EQ(after.text.find("r8ui"), String::npos) << after.text;
        // GL reads a one-channel image as (r, 0, 0, 1) and drops everything past r on a store, so
        // both accesses have to be spelled that way whatever the carrier holds.
        EXPECT_NE(after.text.find("uvec4(0u, 0u, 0u, 1u)"), String::npos) << after.text;
    }
    {
        const Vector<Uint32> spirv = CompileFragment(kRg32fLoadStore);
        ASSERT_FALSE(spirv.empty());
        // This one SPIRV-Cross does print - the token is simply not one GLSL ES has.
        const EsslAttempt before = EmitEssl(spirv);
        ASSERT_TRUE(before.succeeded) << before.error;
        EXPECT_NE(before.text.find("rg32f"), String::npos) << before.text;

        Vector<Uint32> widened;
        ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, false, true));
        const EsslAttempt after = EmitEssl(widened);
        ASSERT_TRUE(after.succeeded) << after.error;
        EXPECT_NE(after.text.find("rgba32f"), String::npos) << after.text;
        EXPECT_EQ(after.text.find("rg32f"), String::npos)
            << "the token no ES driver accepts is still in the emitted source:\n"
            << after.text;
    }
}

// The narrow mode, for a driver that HAS GL_NV_image_formats - Mesa, which every software lane
// runs on. There the driver can spell rg32f, so widening it would spend two to four times the
// texture memory to change nothing; but SPIRV-Cross STILL throws for r8ui rather than printing it,
// and the throw loses the stage whatever the driver would have accepted. So the extension narrows
// the emulation to its is_desktop_only_format set rather than switching it off.
TEST(WidenImageFormats, TheExtensionNarrowsTheWideningToWhatSpirvCrossWillNotPrint) {
    ASSERT_TRUE(ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(0x8230 /*GL_RG32F*/));
    ASSERT_FALSE(ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(0x8232 /*GL_R8UI*/));

    {   // rg32f: printable, so the narrow mode leaves it exactly as declared.
        const Vector<Uint32> spirv = CompileFragment(kRg32fLoadStore);
        ASSERT_FALSE(spirv.empty());
        EXPECT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv, false));
        EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(spirv, true));

        Vector<Uint32> widened;
        ShaderCompiler::WidenImageFormatsForEssl(spirv, widened,
                                                 /*onlyFormatsSpirvCrossRefusesToPrint=*/true, true);
        if (!widened.empty()) {
            const auto types = CollectStorageImageTypes(widened);
            ASSERT_EQ(types.size(), 1u);
            EXPECT_EQ(types.front().format, static_cast<Uint32>(spv::ImageFormat::Rg32f));
            EXPECT_EQ(CollectVectorShuffles(widened).size(), CollectVectorShuffles(spirv).size());
        }
    }
    {   // r8ui: unprintable, so the narrow mode still carries it - and must mask it exactly as
        // the wide mode does, because the storage and the bind widen with it either way.
        const Vector<Uint32> spirv = CompileFragment(kR8uiLoadStore);
        ASSERT_FALSE(spirv.empty());
        EXPECT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv, true));

        Vector<Uint32> widened;
        ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(
            spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/true, true));
        ASSERT_FALSE(widened.empty());
        EXPECT_TRUE(Validates(widened));
        const auto types = CollectStorageImageTypes(widened);
        ASSERT_EQ(types.size(), 1u);
        EXPECT_EQ(types.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba8ui));

        const auto shuffles = CollectVectorShuffles(widened);
        const auto texelIds = CollectImageWriteTexelIds(widened);
        ASSERT_EQ(texelIds.size(), 1u);
        const VectorShuffle* storeMask = FindShuffleWithResult(shuffles, texelIds.front());
        ASSERT_NE(storeMask, nullptr);
        EXPECT_TRUE(HasComponents(*storeMask, {0u, 5u, 6u, 7u}));
    }
}

TEST(WidenImageFormats, CoreFormatModuleIsHandedBackUntouched) {
    const Vector<Uint32> spirv = CompileFragment(kCoreFormatLoadStore);
    ASSERT_FALSE(spirv.empty());
    // The cheap probe is what keeps every ordinary shader off the optimizer entirely.
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true);
    if (!widened.empty()) {
        EXPECT_EQ(CollectVectorShuffles(widened).size(), CollectVectorShuffles(spirv).size())
            << "a core-format module must gain no masks";
        const auto types = CollectStorageImageTypes(widened);
        ASSERT_EQ(types.size(), 1u);
        EXPECT_EQ(types.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba32f));
    }
}

TEST(WidenImageFormats, FormatWithoutAnExactCarrierIsLeftAlone) {
    const Vector<Uint32> spirv = CompileFragment(kRg16LoadStore);
    ASSERT_FALSE(spirv.empty());

    const auto beforeTypes = CollectStorageImageTypes(spirv);
    ASSERT_EQ(beforeTypes.size(), 1u);
    EXPECT_EQ(beforeTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rg16));

    // rg16 has no core format with 16-bit unsigned-normalized channels behind it. Anything wider
    // would requantize differently from what the application asked for, so the pass declines and
    // CollectImageFormatBakeInputs reports the format as unspellable instead.
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true);
    if (!widened.empty()) {
        const auto afterTypes = CollectStorageImageTypes(widened);
        ASSERT_EQ(afterTypes.size(), 1u);
        EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rg16));
    }
}
