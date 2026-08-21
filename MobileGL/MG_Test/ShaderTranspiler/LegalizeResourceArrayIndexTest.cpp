// MobileGL - MobileGL/MG_Test/ShaderTranspiler/LegalizeResourceArrayIndexTest.cpp
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
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
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

    // The image half of the same reference walker, and equally independent of the production
    // detection: true when some access chain rooted at an array-of-IMAGES variable carries a
    // non-constant FIRST index. A UniformConstant array whose element type is an OpTypeImage
    // with Sampled == 2 is what GLSL spells `image2D g_image[N]`; a sampler array is an
    // OpTypeSampledImage and is deliberately not matched here, because ESSL allows it a
    // dynamically-uniform index.
    bool HasDynamicImageArrayIndex(const Vector<Uint32>& spirv) {
        std::set<Uint32> storageImages;      // OpTypeImage ids with Sampled == 2
        std::set<Uint32> constants;          // OpConstant / OpConstantNull result ids
        std::set<Uint32> imageArrayTypes;    // OpTypeArray ids whose element is such an image
        std::set<Uint32> imageArrayPointers; // OpTypePointer ids pointing at one of those arrays
        std::set<Uint32> imageArrayVars;     // OpVariable ids of one of those pointer types

        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            switch (opcode) {
            case spv::Op::OpTypeImage:
                // OpTypeImage <result> <sampled type> <dim> <depth> <arrayed> <ms> <sampled>
                if (wordCount >= 8u && words[7] == 2u) storageImages.insert(words[1]);
                break;
            case spv::Op::OpConstant:
            case spv::Op::OpConstantNull:
                if (wordCount >= 3u) constants.insert(words[2]);
                break;
            case spv::Op::OpTypeArray:
                if (wordCount >= 4u && storageImages.count(words[2]) != 0u) {
                    imageArrayTypes.insert(words[1]);
                }
                break;
            case spv::Op::OpTypePointer:
                if (wordCount >= 4u && imageArrayTypes.count(words[3]) != 0u) {
                    imageArrayPointers.insert(words[1]);
                }
                break;
            case spv::Op::OpVariable:
                if (wordCount >= 4u && imageArrayPointers.count(words[1]) != 0u) {
                    imageArrayVars.insert(words[2]);
                }
                break;
            default:
                break;
            }
        });

        bool dynamic = false;
        ForEachInstruction(spirv, [&](spv::Op opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode != spv::Op::OpAccessChain && opcode != spv::Op::OpInBoundsAccessChain) return;
            if (wordCount < 5u) return;
            if (imageArrayVars.count(words[3]) == 0u) return;
            if (constants.count(words[4]) != 0u) return;
            dynamic = true;
        });
        return dynamic;
    }

    // The ESSL SPIRV-Cross prints for a module, or the error it refused with. This is where the
    // rule actually bites: the SPIR-V is legal Vulkan either way, and what a strict ES driver
    // reads is this text.
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

    // The IMAGE half, and the case that has always been broken independently of any per-element
    // unit remapping: a plain CONSECUTIVE image array subscripted by a loop variable. This is
    // KHR-GL42.shader_image_load_store.advanced-sso-simple's own fragment shader shape, and a raw
    // GLES probe on Mesa 26.1.4 at ES 3.2 refuses the ESSL it produces with "image arrays indexed
    // with non-constant expressions are forbidden in GLSL ES". Foldable: after unrolling every
    // subscript is a literal.
    constexpr const char* kLoopIndexedImageArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(rgba32f, binding = 0) uniform writeonly image2D g_image[4];
void main() {
    for (int i = 0; i < 4; ++i) {
        imageStore(g_image[i], ivec2(0), vec4(1.0));
    }
}
)";

    // A uniform-sourced image index: nothing can fold it, so the switch/select lowering is what
    // has to carry it. Both directions in one shader, as the block-array fixture does.
    constexpr const char* kUniformIndexedImageArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(rgba32f, binding = 0) uniform image2D g_image[4];
layout(std430, binding = 8) buffer Out { vec4 value; } g_out;
uniform int g_index;
void main() {
    imageStore(g_image[g_index], ivec2(0), vec4(7.0));
    g_out.value = imageLoad(g_image[g_index], ivec2(1));
}
)";

    // An image array indexed only with literals is already legal ES.
    constexpr const char* kConstantIndexedImageArray = R"(#version 450 core
layout(local_size_x = 1) in;
layout(rgba32f, binding = 0) uniform writeonly image2D g_image[4];
void main() {
    imageStore(g_image[1], ivec2(0), vec4(1.0));
    imageStore(g_image[3], ivec2(0), vec4(2.0));
}
)";

    // The positive control for the scope decision: ESSL 3.20 4.1.7 allows a SAMPLER array a
    // dynamically-uniform index, and the same raw GLES probe confirms it - both a loop-variable
    // subscript and a const-table lookup compile and link. Nothing here may be rewritten.
    constexpr const char* kUniformIndexedSamplerArray = R"(#version 450 core
layout(local_size_x = 1) in;
uniform sampler2D g_tex[4];
layout(std430, binding = 8) buffer Out { vec4 value; } g_out;
uniform int g_index;
void main() {
    g_out.value = texture(g_tex[g_index], vec2(0.5));
}
)";

    // An imageAtomic* reaches the array through OpImageTexelPointer, and running one per element
    // would perform every other element's atomic as well. The pass has to decline rather than
    // lower this.
    constexpr const char* kUniformIndexedImageAtomic = R"(#version 450 core
layout(local_size_x = 1) in;
layout(r32ui, binding = 0) uniform uimage2D g_image[4];
layout(std430, binding = 8) buffer Out { uint value; } g_out;
uniform int g_index;
void main() {
    g_out.value = imageAtomicAdd(g_image[g_index], ivec2(0), 1u);
}
)";
} // namespace

TEST(LegalizeResourceArrayIndexPass, FoldsALoopIndexedBlockArray) {
    const Vector<Uint32> input = CompileCompute(kLoopIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    // Either half of the legalization is an acceptable outcome here - what the ES driver
    // cares about is only that no dynamic subscript survives.
    EXPECT_FALSE(HasDynamicBlockArrayIndex(output));
    EXPECT_TRUE(Validates(output));
}

TEST(LegalizeResourceArrayIndexPass, LowersAUniformIndexedWriteToASwitchAndAReadToSelects) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicBlockArrayIndex(input));
    EXPECT_EQ(CountOpcode(input, spv::Op::OpSwitch), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(output));
    // One switch for the store, and one select per element past the first for the load.
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSwitch), 1u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSelect), 3u);
    EXPECT_TRUE(Validates(output));
}

TEST(LegalizeResourceArrayIndexPass, LeavesADynamicMemberOfOneBlockByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kArrayMemberInsideOneBlock);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(LegalizeResourceArrayIndexPass, LeavesAConstantIndexedBlockArrayByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kConstantIndexedBlockArray);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicBlockArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

TEST(LegalizeResourceArrayIndexPass, IsIdempotent) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedBlockArray);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, once, true));
    ASSERT_FALSE(once.empty());

    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(once, twice, true));
    EXPECT_EQ(twice, once);
}

// The case no test covered before, and the one that has nothing to do with per-element unit
// remapping: an ordinary consecutive image array written from a loop. Every emitted subscript has
// to end up a literal, or the ES driver drops the stage and every draw with it.
TEST(LegalizeResourceArrayIndexPass, FoldsALoopIndexedImageArray) {
    const Vector<Uint32> input = CompileCompute(kLoopIndexedImageArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicImageArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_FALSE(HasDynamicImageArrayIndex(output));
    EXPECT_TRUE(Validates(output));

    // ...and in the text the driver actually reads. Before: `g_image[i]`; after: four literals.
    const EsslAttempt before = EmitEssl(input);
    ASSERT_TRUE(before.succeeded) << before.error;
    EXPECT_NE(before.text.find("g_image[i]"), String::npos) << before.text;

    const EsslAttempt after = EmitEssl(output);
    ASSERT_TRUE(after.succeeded) << after.error;
    EXPECT_EQ(after.text.find("g_image[i]"), String::npos) << after.text;
    for (int element = 0; element < 4; ++element) {
        EXPECT_NE(after.text.find("g_image[" + std::to_string(element) + "]"), String::npos) << after.text;
    }
}

TEST(LegalizeResourceArrayIndexPass, LowersAUniformIndexedImageWriteToASwitchAndAReadToSelects) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedImageArray);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicImageArrayIndex(input));
    EXPECT_EQ(CountOpcode(input, spv::Op::OpSwitch), 0u);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_FALSE(HasDynamicImageArrayIndex(output));
    // One switch for the imageStore, and one select per element past the first for the imageLoad.
    // The selection is on the loaded TEXEL, never on the image object - an opaque type cannot be
    // selected at all - so there is one OpImageRead per element behind those selects.
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSwitch), 1u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSelect), 3u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpImageRead), 4u);
    EXPECT_EQ(CountOpcode(output, spv::Op::OpImageWrite), 4u);
    EXPECT_TRUE(Validates(output));

    const EsslAttempt after = EmitEssl(output);
    ASSERT_TRUE(after.succeeded) << after.error;
    for (int element = 0; element < 4; ++element) {
        EXPECT_NE(after.text.find("g_image[" + std::to_string(element) + "]"), String::npos) << after.text;
    }
}

TEST(LegalizeResourceArrayIndexPass, LeavesAConstantIndexedImageArrayByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kConstantIndexedImageArray);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicImageArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

// The scope decision, asserted rather than assumed: a sampler array indexed by a uniform is legal
// ESSL, so the module must come back untouched - not merely legal, byte for byte the same.
TEST(LegalizeResourceArrayIndexPass, LeavesADynamicallyIndexedSamplerArrayByteIdentical) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedSamplerArray);
    ASSERT_FALSE(input.empty());
    EXPECT_FALSE(HasDynamicImageArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    EXPECT_EQ(output, input);
}

// An imageAtomic* is the shape the lowering must refuse: its per-element rebuild would run every
// other element's read-modify-write. Declining leaves the illegal subscript in place - which is
// what the latched warning in LegalizeResourceArrayIndexingForEssl is for - but a half-transform
// would corrupt four images instead of losing one stage.
TEST(LegalizeResourceArrayIndexPass, DeclinesAUniformIndexedImageAtomic) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedImageAtomic);
    ASSERT_FALSE(input.empty());
    EXPECT_TRUE(HasDynamicImageArrayIndex(input));

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(HasDynamicImageArrayIndex(output));
    EXPECT_EQ(CountOpcode(output, spv::Op::OpSwitch), 0u);
    EXPECT_TRUE(Validates(output));
}

TEST(LegalizeResourceArrayIndexPass, IsIdempotentOnImageArrays) {
    const Vector<Uint32> input = CompileCompute(kUniformIndexedImageArray);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> once;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(input, once, true));
    ASSERT_FALSE(once.empty());

    Vector<Uint32> twice;
    ASSERT_TRUE(ShaderCompiler::LegalizeResourceArrayIndexingForEssl(once, twice, true));
    EXPECT_EQ(twice, once);
}
