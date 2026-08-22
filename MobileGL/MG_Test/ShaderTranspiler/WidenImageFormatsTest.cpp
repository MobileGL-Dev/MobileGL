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
// become a core carrier that loses nothing, every imageStore through it must have its surplus
// components replaced by GL's own (0.., 1) so the carrier's extra channels never hold anything GL
// has not defined, and every imageLoad must come back masked the same way. A module that declares
// only core formats - or one of the eight formats with no lossless carrier at all - must come
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
        Uint32 sampledTypeId = 0u;
    };

    Vector<StorageImageType> CollectStorageImageTypes(const Vector<Uint32>& spirv) {
        Vector<StorageImageType> types;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpTypeImage || wordCount < 9u) return;
            if (words[7] != 2u) return;
            types.push_back(StorageImageType{words[1], words[8], words[2]});
        });
        return types;
    }

    // "float" / "uint" / "int" / "" for a scalar numeric type id, which is the one thing that says
    // whether a declaration is still an image2D or has become a uimage2D.
    String ScalarTypeSpellingOf(const Vector<Uint32>& spirv, Uint32 typeId) {
        String spelling;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (words[1] != typeId) return;
            // OpTypeFloat words: 1 result id, 2 width. OpTypeInt adds 3 signedness.
            if (opcode == spv::Op::OpTypeFloat && wordCount >= 3u) {
                spelling = "float";
            } else if (opcode == spv::Op::OpTypeInt && wordCount >= 4u) {
                spelling = words[3] != 0u ? "int" : "uint";
            }
        });
        return spelling;
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

    // r11f_g11f_b10f: THREE float channels in a packed 32-bit word, and the only format the four
    // CTS allFormats/allTargets walkers still aborted on after the channel widening landed - it
    // has no core carrier of the same per-channel width, so it took rgba16f, whose 5-bit exponent
    // and longer mantissa represent every 11f and 10f value exactly.
    const char* const kR11fG11fB10fLoadStore = R"(#version 430 core
layout(r11f_g11f_b10f, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(1.0, 2.0, 3.0, 4.0));
    fragColor = texel;
}
)";

    // rg32f again, but as a BUFFER image. Same format, same carrier on paper - and it must be
    // left alone anyway, because a buffer image's texels are the application's buffer object.
    const char* const kRg32fBufferLoadStore = R"(#version 430 core
layout(rg32f, binding = 0) uniform imageBuffer img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, int(gl_FragCoord.x));
    imageStore(img, int(gl_FragCoord.x), vec4(1.0, 2.0, 3.0, 4.0));
    fragColor = texel;
}
)";

    // rgb10_a2ui: FOUR unsigned-integer channels of 10, 10, 10 and 2 bits, carried in an rgba16ui
    // that gives each of them sixteen. The only widening whose carrier has as many channels as the
    // original, so it is the only one where GL leaves NOTHING to pin and both accesses must come
    // out exactly as glslang emitted them.
    const char* const kRgb10A2uiLoadStore = R"(#version 430 core
layout(rgb10_a2ui, binding = 0) uniform uimage2D img;
out vec4 fragColor;
void main() {
    uvec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), uvec4(7u, 8u, 9u, 3u));
    fragColor = vec4(texel);
}
)";

    // rg16: TWO unsigned-normalized 16-bit channels, which core ESSL has no image format of any
    // width for. Carried as its own CODES in an rgba16ui, so the declaration comes out a
    // uimage2D and every access is wrapped in GL 4.6 2.3.5 as well as masked.
    const char* const kRg16LoadStore = R"(#version 430 core
layout(rg16, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(0.25, 0.5, 0.75, 1.0));
    fragColor = texel;
}
)";

    // rgba16_snorm: the signed twin, whose code is a two's-complement 16-bit integer sitting in an
    // UNSIGNED carrier channel - so the load has to sign-extend it back and the store has to mask
    // it down, neither of which the unsigned conversion does.
    const char* const kRgba16SnormLoadStore = R"(#version 430 core
layout(rgba16_snorm, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(1.0, -1.0, 0.5, -0.5));
    fragColor = texel;
}
)";

    // rgb10_a2: FOUR normalized channels that are not all the same width, so its denominator is
    // (1023, 1023, 1023, 3) and one number would be wrong for a quarter of every texel.
    const char* const kRgb10A2LoadStore = R"(#version 430 core
layout(rgb10_a2, binding = 0) uniform image2D img;
out vec4 fragColor;
void main() {
    vec4 texel = imageLoad(img, ivec2(gl_FragCoord.xy));
    imageStore(img, ivec2(gl_FragCoord.xy), vec4(0.25, 0.5, 0.75, 1.0));
    fragColor = texel;
}
)";
} // namespace

// The table itself, which is the single source of truth all three layers of the emulation ask -
// the shader rewrite, the ES texture storage and the glBindImageTexture argument. If it drifts
// the three stop agreeing, and a narrow texture read through a wide image goes out of bounds
// silently on every driver tested.
TEST(WidenImageFormats, TwentySixNonCoreFormatsHaveALosslessCoreCarrier) {
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
        // The one entry that is a re-encoding rather than a channel widening: 11f is e5m6 and 10f
        // is e5m5 against a half's s1e5m10, so the carrier is still lossless - and three channels,
        // so the mask has to pin only alpha.
        {0x8C3A, 0x881A, 3, "GL_R11F_G11F_B10F -> GL_RGBA16F"},
        // FOUR channels: 10, 10, 10 and 2 bits of unsigned integer all fit in sixteen, so nothing
        // is masked at all and only the packed TRANSFER is re-encoded.
        {0x906F, 0x8D76, 4, "GL_RGB10_A2UI -> GL_RGBA16UI"},
        // The seven NORMALIZED formats, carried as their own channel CODES in the same rgba16ui.
        // These are the entries whose carrier changes the shader-visible TYPE as well, which is
        // why every access through them is wrapped in GL 4.6 2.3.5 rather than only masked.
        {0x805B, 0x8D76, 4, "GL_RGBA16 -> GL_RGBA16UI"},
        {0x822C, 0x8D76, 2, "GL_RG16 -> GL_RGBA16UI"},
        {0x822A, 0x8D76, 1, "GL_R16 -> GL_RGBA16UI"},
        {0x8059, 0x8D76, 4, "GL_RGB10_A2 -> GL_RGBA16UI"},
        {0x8F9B, 0x8D76, 4, "GL_RGBA16_SNORM -> GL_RGBA16UI"},
        {0x8F99, 0x8D76, 2, "GL_RG16_SNORM -> GL_RGBA16UI"},
        {0x8F98, 0x8D76, 1, "GL_R16_SNORM -> GL_RGBA16UI"},
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

TEST(WidenImageFormats, CoreFormatsAreRefused) {
    // The thirteen GLSL ES already has: nothing to carry.
    for (const Uint coreFormat : {0x8814u /*RGBA32F*/, 0x881Au /*RGBA16F*/, 0x822Eu /*R32F*/,
                                  0x8058u /*RGBA8*/, 0x8F97u /*RGBA8_SNORM*/, 0x8D82u /*RGBA32I*/,
                                  0x8D88u /*RGBA16I*/, 0x8D8Eu /*RGBA8I*/, 0x8235u /*R32I*/,
                                  0x8D70u /*RGBA32UI*/, 0x8D76u /*RGBA16UI*/, 0x8D7Cu /*RGBA8UI*/,
                                  0x8236u /*R32UI*/}) {
        EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(coreFormat), 0u)
            << "core format 0x" << std::hex << coreFormat;
    }
    // Not an image format at all.
    EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(0x8051 /*GL_RGB8*/), 0u);
    EXPECT_EQ(ShaderCompiler::ImageFormatChannelCount(0x8051 /*GL_RGB8*/), 0u);
    EXPECT_EQ(ShaderCompiler::WidenedCoreEsslImageFormat(0), 0u);
}

// The denominators of GL 4.6 2.3.5, which is the whole difference between a carrier that holds a
// format's VALUES and one that holds its CODES. Both halves of DirectGLES's transfer read them
// (the upload's synthetic alpha and glGetTexImage's divide), and so does the shader rewrite, so a
// wrong entry here is wrong in three places at once and consistently - which is exactly the kind
// of error a round-trip test cannot see.
TEST(WidenImageFormats, OnlyTheNormalizedFormatsCarryCodesAndTheirDenominatorsAreTheFormatsOwn) {
    struct Case {
        Uint format;
        Uint32 channelMax[4];
        bool isSigned;
        const char* name;
    };
    const Case cases[] = {
        {0x805B, {65535u, 65535u, 65535u, 65535u}, false, "GL_RGBA16"},
        {0x822C, {65535u, 65535u, 65535u, 65535u}, false, "GL_RG16"},
        {0x822A, {65535u, 65535u, 65535u, 65535u}, false, "GL_R16"},
        // The one format whose channels are not all the same width, and the reason the answer is
        // four numbers rather than one: a two-bit alpha saturates at 3, not at 1023.
        {0x8059, {1023u, 1023u, 1023u, 3u}, false, "GL_RGB10_A2"},
        {0x8F9B, {32767u, 32767u, 32767u, 32767u}, true, "GL_RGBA16_SNORM"},
        {0x8F99, {32767u, 32767u, 32767u, 32767u}, true, "GL_RG16_SNORM"},
        {0x8F98, {32767u, 32767u, 32767u, 32767u}, true, "GL_R16_SNORM"},
    };
    for (const Case& testCase : cases) {
        Uint32 channelMax[4] = {0u, 0u, 0u, 0u};
        bool isSigned = !testCase.isSigned;
        EXPECT_TRUE(ShaderCompiler::NormalizedImageCarrierCodes(testCase.format, channelMax, isSigned))
            << testCase.name;
        for (Uint channel = 0; channel < 4; ++channel) {
            EXPECT_EQ(channelMax[channel], testCase.channelMax[channel])
                << testCase.name << " channel " << channel;
        }
        EXPECT_EQ(isSigned, testCase.isSigned) << testCase.name;
    }

    // Everything else keeps its own component type in the carrier, so nothing is converted: an
    // rg8's carrier channel really is an 8-bit unsigned normalized one, and an rgb10_a2ui's
    // channel really does hold the integer the shader stored.
    for (const Uint direct : {0x8230u /*RG32F*/, 0x8229u /*R8*/, 0x8F94u /*R8_SNORM*/,
                              0x8232u /*R8UI*/, 0x8C3Au /*R11F_G11F_B10F*/, 0x906Fu /*RGB10_A2UI*/,
                              0x8814u /*RGBA32F*/, 0x8051u /*RGB8, not an image format*/}) {
        Uint32 channelMax[4] = {7u, 7u, 7u, 7u};
        bool isSigned = true;
        EXPECT_FALSE(ShaderCompiler::NormalizedImageCarrierCodes(direct, channelMax, isSigned))
            << "format 0x" << std::hex << direct;
        for (Uint channel = 0; channel < 4; ++channel) {
            EXPECT_EQ(channelMax[channel], 7u) << "a refused format must leave the output alone";
        }
    }
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

// The three-channel case, which no format exercised before r11f_g11f_b10f was carried: only ALPHA
// is surplus, so the mask must take r, g and b from the texel and nothing but the fourth component
// from the (0, 0, 0, 1) constant. A mask that zeroed blue here - the shape a two-channel format
// wants - would silently drop the third channel of every store.
TEST(WidenImageFormats, ThreeChannelPackedFloatImageBecomesRgba16fWithOnlyAlphaPinned) {
    const Vector<Uint32> spirv = CompileFragment(kR11fG11fB10fLoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    const auto beforeTypes = CollectStorageImageTypes(spirv);
    ASSERT_EQ(beforeTypes.size(), 1u);
    EXPECT_EQ(beforeTypes.front().format, static_cast<Uint32>(spv::ImageFormat::R11fG11fB10f));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba16f));

    const auto shuffles = CollectVectorShuffles(widened);

    const auto texelIds = CollectImageWriteTexelIds(widened);
    ASSERT_EQ(texelIds.size(), 1u);
    const VectorShuffle* storeMask = FindShuffleWithResult(shuffles, texelIds.front());
    ASSERT_NE(storeMask, nullptr) << "the imageStore texel is not a masked value";
    EXPECT_TRUE(HasComponents(*storeMask, {0u, 1u, 2u, 7u}))
        << "expected (r, g, b, 1) - components 0, 1 and 2 of the texel, then 3 of (0,0,0,1)";

    const auto readIds = CollectImageReadResultIds(widened);
    ASSERT_EQ(readIds.size(), 1u);
    const VectorShuffle* loadMask = FindShuffleOver(shuffles, readIds.front());
    ASSERT_NE(loadMask, nullptr) << "the imageLoad result is consumed unmasked";
    EXPECT_TRUE(HasComponents(*loadMask, {0u, 1u, 2u, 7u}));
}

// The four-channel case, which is the whole of rgb10_a2ui's shader-side emulation: the carrier has
// as many channels as the original, every value of every channel fits, and GL therefore defines
// NOTHING about a surplus channel because there is none. So both accesses have to come out
// untouched - a pass that masked here would replace the alpha the application stored (0..3 of a
// two-bit channel, which the CTS walker writes as 3) with the constant 1 and drop blue outright.
TEST(WidenImageFormats, FourChannelIntegerImageBecomesRgba16uiWithNeitherAccessMasked) {
    const Vector<Uint32> spirv = CompileFragment(kRgb10A2uiLoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    const auto beforeTypes = CollectStorageImageTypes(spirv);
    ASSERT_EQ(beforeTypes.size(), 1u);
    EXPECT_EQ(beforeTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgb10a2ui));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba16ui));

    // The declaration moved and nothing else did.
    EXPECT_EQ(CollectVectorShuffles(widened).size(), CollectVectorShuffles(spirv).size())
        << "a carrier with as many channels as the original must add no mask";
    EXPECT_EQ(CollectImageReadResultIds(widened).size(), CollectImageReadResultIds(spirv).size())
        << "the imageLoad was duplicated for a rewrite that has nothing to rewrite";
}

// ...and the same module through the emitter, which is where the failure actually showed: ESSL has
// no `r11f_g11f_b10f` token, SPIRV-Cross throws for it, and the throw took every image uniform
// declared in the same stage with it.
TEST(WidenImageFormats, PackedFloatImageOnlyReachesEsslThroughTheCarrier) {
    const Vector<Uint32> spirv = CompileFragment(kR11fG11fB10fLoadStore);
    ASSERT_FALSE(spirv.empty());

    const EsslAttempt before = EmitEssl(spirv);
    EXPECT_FALSE(before.succeeded)
        << "SPIRV-Cross printed r11f_g11f_b10f for an ES target; the widening's premise has "
           "changed:\n"
        << before.text;

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    const EsslAttempt after = EmitEssl(widened);
    ASSERT_TRUE(after.succeeded) << after.error;
    EXPECT_NE(after.text.find("rgba16f"), String::npos) << after.text;
    EXPECT_EQ(after.text.find("r11f_g11f_b10f"), String::npos) << after.text;
}

// A BUFFER image is declined whatever its format, and the format alone cannot say so - rg32f is
// carried exactly when it is an image2D. What makes the difference is that widening REALLOCATES
// the texture behind the image in the carrier, and a buffer image has no texture storage to
// reallocate: its texels are the application's buffer object, usually also a vertex, index or
// storage buffer. Widening one leaves the shader striding 16 bytes through 8-byte texels - the
// measured symptom on an Adreno 830 was a 32-byte GL_RG32F buffer reading back
// [1,100] [0,1] [2,100] [0,1] instead of [1,100] [2,100] [3,100] [4,100], with the last two texels
// written past the end of the application's buffer.
TEST(WidenImageFormats, BufferImagesAreDeclinedEvenWhenTheirFormatHasACarrier) {
    const Vector<Uint32> spirv = CompileFragment(kRg32fBufferLoadStore);
    ASSERT_FALSE(spirv.empty());

    const auto types = CollectStorageImageTypes(spirv);
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types.front().format, static_cast<Uint32>(spv::ImageFormat::Rg32f))
        << "the fixture stopped declaring the format this test is about";

    // The gate says no, so the optimizer is never even run for it...
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));
    // ...and running it anyway changes nothing, which is what keeps the gate and the pass from
    // disagreeing about a module.
    Vector<Uint32> widened;
    ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                             /*enableSpirvValidation=*/true);
    EXPECT_TRUE(widened.empty() || widened == spirv) << "a buffer image was rewritten";

    // The same format in a NON-buffer image still widens, or this test would pass for the wrong
    // reason - a widening that had simply stopped working.
    const Vector<Uint32> planar = CompileFragment(kRg32fLoadStore);
    ASSERT_FALSE(planar.empty());
    EXPECT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(planar));
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

// The normalized carrier, which is the one that does not merely re-DECLARE the image: a 16-bit
// normalized channel has no core ESSL format of any width behind it, and no FLOAT carrier is
// honest either (a half has eleven mantissa bits against its sixteen), so what the rgba16ui holds
// is the format's own CODE. That changes the shader-visible TYPE, which is the thing to check -
// an image2D whose format moved to rgba16ui but whose sampled type stayed float is not merely
// wrong, it is invalid SPIR-V, and a module that kept the float type while the STORAGE became an
// integer texture would read whole texels as garbage.
TEST(WidenImageFormats, NormalizedImageBecomesAUimageWhoseAccessesConvertItsCodes) {
    const Vector<Uint32> spirv = CompileFragment(kRg16LoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    const auto beforeTypes = CollectStorageImageTypes(spirv);
    ASSERT_EQ(beforeTypes.size(), 1u);
    EXPECT_EQ(beforeTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rg16));
    EXPECT_EQ(ScalarTypeSpellingOf(spirv, beforeTypes.front().sampledTypeId), "float");

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, /*onlyFormatsSpirvCrossRefusesToPrint=*/false,
                                                         /*enableSpirvValidation=*/true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));
    EXPECT_FALSE(ShaderCompiler::DeclaresWidenableImageFormat(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba16ui));
    EXPECT_EQ(ScalarTypeSpellingOf(widened, afterTypes.front().sampledTypeId), "uint")
        << "the carrier's component type is unsigned integer, and spirv-val requires the image's "
           "Sampled Type to say so";

    // The masks are still there and still say what a two-channel format's surplus channels are -
    // the conversion wraps them, it does not replace them.
    const auto shuffles = CollectVectorShuffles(widened);
    const auto texelIds = CollectImageWriteTexelIds(widened);
    ASSERT_EQ(texelIds.size(), 1u);
    // The texel is now the PACKED value, so the mask is one step further back: find the shuffle
    // by its component selectors instead.
    Bool sawTwoChannelMask = false;
    for (const VectorShuffle& shuffle : shuffles) {
        sawTwoChannelMask = sawTwoChannelMask || HasComponents(shuffle, {0u, 1u, 6u, 7u});
    }
    EXPECT_TRUE(sawTwoChannelMask) << "expected the (r, g, 0, 1) mask a two-channel format needs";

    // ...and the ESSL says the whole story: a uimage2D holding rgba16ui, divided and multiplied
    // by the format's own 65535.
    const EsslAttempt after = EmitEssl(widened);
    ASSERT_TRUE(after.succeeded) << after.error;
    EXPECT_NE(after.text.find("uimage2D"), String::npos) << after.text;
    EXPECT_NE(after.text.find("rgba16ui"), String::npos) << after.text;
    EXPECT_EQ(after.text.find("rg16"), String::npos)
        << "the token no ES driver accepts is still in the emitted source:\n"
        << after.text;
    EXPECT_NE(after.text.find("65535.0"), String::npos)
        << "the unsigned-normalized denominator is 2^16 - 1:\n"
        << after.text;
    EXPECT_EQ(after.text.find("32767.0"), String::npos)
        << "an unsigned format must not take the SIGNED denominator:\n"
        << after.text;
}

// The signed half, which needs two things the unsigned one does not: the code is sign-extended
// out of the unsigned carrier channel on the way in, and the decode is max(c / 32767, -1) rather
// than the bare division - GL clamps -2^15/32767 up to exactly -1.
TEST(WidenImageFormats, SignedNormalizedImageSignExtendsItsCodeAndClampsAtMinusOne) {
    const Vector<Uint32> spirv = CompileFragment(kRgba16SnormLoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, false, true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba16ui));
    EXPECT_EQ(ScalarTypeSpellingOf(widened, afterTypes.front().sampledTypeId), "uint");

    // The sign extension is a shift PAIR, and the arithmetic one is what makes it a sign
    // extension rather than a zero extension.
    Bool sawShiftLeft = false;
    Bool sawArithmeticShiftRight = false;
    ForEachInstruction(widened, [&](spv::Op opcode, const Uint32*, Uint32) {
        sawShiftLeft = sawShiftLeft || opcode == spv::Op::OpShiftLeftLogical;
        sawArithmeticShiftRight = sawArithmeticShiftRight || opcode == spv::Op::OpShiftRightArithmetic;
    });
    EXPECT_TRUE(sawShiftLeft);
    EXPECT_TRUE(sawArithmeticShiftRight)
        << "a logical shift right would read every negative code as a large positive one";

    const EsslAttempt after = EmitEssl(widened);
    ASSERT_TRUE(after.succeeded) << after.error;
    EXPECT_NE(after.text.find("uimage2D"), String::npos) << after.text;
    EXPECT_NE(after.text.find("rgba16ui"), String::npos) << after.text;
    EXPECT_EQ(after.text.find("rgba16_snorm"), String::npos) << after.text;
    EXPECT_NE(after.text.find("32767.0"), String::npos)
        << "the signed-normalized denominator is 2^15 - 1:\n"
        << after.text;
    EXPECT_NE(after.text.find("-1.0"), String::npos)
        << "GL clamps the signed decode at -1:\n"
        << after.text;
}

// rgb10_a2, whose four channels are 10, 10, 10 and 2 bits: the only entry where one denominator
// would be wrong for a channel that IS present, rather than for one the mask discards anyway.
TEST(WidenImageFormats, TenTenTenTwoImageTakesAPerChannelDenominator) {
    const Vector<Uint32> spirv = CompileFragment(kRgb10A2LoadStore);
    ASSERT_FALSE(spirv.empty());
    ASSERT_TRUE(ShaderCompiler::DeclaresWidenableImageFormat(spirv));

    Vector<Uint32> widened;
    ASSERT_TRUE(ShaderCompiler::WidenImageFormatsForEssl(spirv, widened, false, true));
    ASSERT_FALSE(widened.empty());
    EXPECT_TRUE(Validates(widened));

    const auto afterTypes = CollectStorageImageTypes(widened);
    ASSERT_EQ(afterTypes.size(), 1u);
    EXPECT_EQ(afterTypes.front().format, static_cast<Uint32>(spv::ImageFormat::Rgba16ui));
    EXPECT_EQ(ScalarTypeSpellingOf(widened, afterTypes.front().sampledTypeId), "uint");

    const EsslAttempt after = EmitEssl(widened);
    ASSERT_TRUE(after.succeeded) << after.error;
    EXPECT_NE(after.text.find("1023.0"), String::npos) << after.text;
    EXPECT_NE(after.text.find("3.0"), String::npos)
        << "the two-bit alpha saturates at 3, not at 1023:\n"
        << after.text;
    EXPECT_EQ(after.text.find("rgb10_a2)"), String::npos) << after.text;
}
