// MobileGL - MobileGL/MG_Test/Pipeline/NarrowIntegerVertexFormatsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Which vertex inputs take Magma's narrow-integer program variant. The pipeline fetches an integer
// attribute with the format of the shader input's signedness, which re-extends 8/16-bit data by the
// wrong rule exactly when the data and the input disagree in signedness; every other pairing -
// 32-bit data, matching signedness, float data or float inputs - must keep today's program.

#include <gtest/gtest.h>

#include <MG_Backend/DirectVulkan/Renderer/NarrowIntegerVertexFormats.h>
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::ClassifyNarrowIntegerVertexFormat;
using MobileGL::MG_Backend::DirectVulkan::IntegerVertexInputMasks;
using MobileGL::MG_Backend::DirectVulkan::NarrowIntegerAttributeMasks;
using MobileGL::MG_Backend::DirectVulkan::NarrowIntegerVertexFormat;
using MobileGL::MG_Backend::DirectVulkan::NarrowIntegerVertexInputs;
using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;
using MobileGL::MG_Backend::DirectVulkan::ResolveNarrowIntegerVertexInputs;

namespace {
    // The correction one (data format, shader input type) pair gets, alone at `location`.
    NarrowIntegerVertexInputs Resolve(VkFormat format, GLenum inputType, Uint32 location = 3) {
        IntegerVertexInputMasks inputs;
        inputs.Note(location, inputType);
        NarrowIntegerAttributeMasks data;
        data.Note(location, format);
        return ResolveNarrowIntegerVertexInputs(inputs, data);
    }

    NarrowIntegerVertexInputs Only(Uint32 NarrowIntegerVertexInputs::* field, Uint32 location = 3) {
        NarrowIntegerVertexInputs expected;
        expected.*field = 1u << location;
        return expected;
    }
} // namespace

TEST(NarrowIntegerVertexFormats, ClassifiesExactlyThe8And16BitIntegerFormats) {
    for (const VkFormat format :
         {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT})
        EXPECT_EQ(ClassifyNarrowIntegerVertexFormat(format), NarrowIntegerVertexFormat::Unsigned8) << format;
    for (const VkFormat format :
         {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT})
        EXPECT_EQ(ClassifyNarrowIntegerVertexFormat(format), NarrowIntegerVertexFormat::Unsigned16) << format;
    for (const VkFormat format :
         {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT})
        EXPECT_EQ(ClassifyNarrowIntegerVertexFormat(format), NarrowIntegerVertexFormat::Signed8) << format;
    for (const VkFormat format :
         {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT})
        EXPECT_EQ(ClassifyNarrowIntegerVertexFormat(format), NarrowIntegerVertexFormat::Signed16) << format;
    // 32-bit integers, float/normalized/scaled data and packed or reversed formats never qualify.
    for (const VkFormat format :
         {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32B32A32_SINT,
          VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R16G16_SSCALED,
          VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A2B10G10R10_UINT_PACK32,
          VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_UNDEFINED})
        EXPECT_EQ(ClassifyNarrowIntegerVertexFormat(format), NarrowIntegerVertexFormat::None) << format;
}

TEST(NarrowIntegerVertexFormats, CorrectsOnlyTheSignednessMismatches) {
    // Sodium 0.5's a_LightAndData: GL_UNSIGNED_BYTE x4 into `in ivec4`.
    EXPECT_EQ(Resolve(VK_FORMAT_R8G8B8A8_UINT, GL_INT_VEC4), Only(&NarrowIntegerVertexInputs::zeroExtend8));
    EXPECT_EQ(Resolve(VK_FORMAT_R8_UINT, GL_INT), Only(&NarrowIntegerVertexInputs::zeroExtend8));
    EXPECT_EQ(Resolve(VK_FORMAT_R16G16_UINT, GL_INT_VEC2), Only(&NarrowIntegerVertexInputs::zeroExtend16));
    EXPECT_EQ(Resolve(VK_FORMAT_R16G16B16_UINT, GL_INT_VEC3), Only(&NarrowIntegerVertexInputs::zeroExtend16));
    EXPECT_EQ(Resolve(VK_FORMAT_R8G8B8A8_SINT, GL_UNSIGNED_INT_VEC4), Only(&NarrowIntegerVertexInputs::signExtend8));
    EXPECT_EQ(Resolve(VK_FORMAT_R8G8_SINT, GL_UNSIGNED_INT_VEC2), Only(&NarrowIntegerVertexInputs::signExtend8));
    EXPECT_EQ(Resolve(VK_FORMAT_R16_SINT, GL_UNSIGNED_INT), Only(&NarrowIntegerVertexInputs::signExtend16));

    // Matching signedness: the fetch already extends by the data's rule.
    EXPECT_FALSE(Resolve(VK_FORMAT_R8G8B8A8_UINT, GL_UNSIGNED_INT_VEC4).Any());
    EXPECT_FALSE(Resolve(VK_FORMAT_R16_SINT, GL_INT).Any());
    // 32-bit data: the format swap renames bits, which is GL's reinterpretation already.
    EXPECT_FALSE(Resolve(VK_FORMAT_R32_UINT, GL_INT).Any());
    EXPECT_FALSE(Resolve(VK_FORMAT_R32G32B32A32_SINT, GL_UNSIGNED_INT_VEC4).Any());
    // Float data into an integer input and integer data into a float input are not this rule.
    EXPECT_FALSE(Resolve(VK_FORMAT_R8G8B8A8_USCALED, GL_INT_VEC4).Any());
    EXPECT_FALSE(Resolve(VK_FORMAT_R8_UNORM, GL_UNSIGNED_INT).Any());
    EXPECT_FALSE(Resolve(VK_FORMAT_R8G8B8A8_UINT, GL_FLOAT_VEC4).Any());
    EXPECT_FALSE(Resolve(VK_FORMAT_R16_SINT, GL_FLOAT).Any());
}

// Locations are independent bits, a location the program does not read as an integer is ignored
// even when its data is narrow, and the top location still fits.
TEST(NarrowIntegerVertexFormats, KeepsLocationsApart) {
    IntegerVertexInputMasks inputs;
    inputs.Note(0, GL_FLOAT_VEC3);
    inputs.Note(1, GL_INT_VEC4);
    inputs.Note(2, GL_UNSIGNED_INT_VEC2);
    inputs.Note(5, GL_UNSIGNED_INT_VEC4);
    inputs.Note(31, GL_INT);
    inputs.Note(32, GL_INT); // beyond the masks: ignored, not wrapped
    EXPECT_EQ(inputs.signedInputs, (1u << 1) | (1u << 31));
    EXPECT_EQ(inputs.unsignedInputs, (1u << 2) | (1u << 5));

    NarrowIntegerAttributeMasks data;
    data.Note(0, VK_FORMAT_R8G8B8A8_UINT); // a float input: not corrected
    data.Note(1, VK_FORMAT_R8G8B8A8_UINT);
    data.Note(2, VK_FORMAT_R16G16_UINT); // matching signedness
    data.Note(5, VK_FORMAT_R8G8B8A8_SINT);
    data.Note(7, VK_FORMAT_R16_SINT); // no shader input
    data.Note(31, VK_FORMAT_R16_UINT);
    data.Note(40, VK_FORMAT_R8_UINT);

    const NarrowIntegerVertexInputs resolved = ResolveNarrowIntegerVertexInputs(inputs, data);
    EXPECT_EQ(resolved.zeroExtend8, 1u << 1);
    EXPECT_EQ(resolved.zeroExtend16, 1u << 31);
    EXPECT_EQ(resolved.signExtend8, 1u << 5);
    EXPECT_EQ(resolved.signExtend16, 0u);

    // A program without integer inputs resolves to nothing whatever the data is.
    EXPECT_FALSE(ResolveNarrowIntegerVertexInputs(IntegerVertexInputMasks{}, data).Any());
}

// The variant's program-cache identity: distinct from the plain program and from every other
// correction set, and a pure function of its inputs.
TEST(NarrowIntegerVertexFormats, VariantHashKeysEveryCorrectionApart) {
    constexpr ProgramFactory::HashType kBase = 0x0123456789abcdefull;
    const NarrowIntegerVertexInputs sodium = Only(&NarrowIntegerVertexInputs::zeroExtend8, 3);
    const auto hash = ProgramFactory::MixNarrowIntegerVertexInputs(kBase, sodium);
    EXPECT_NE(hash, kBase);
    EXPECT_EQ(ProgramFactory::MixNarrowIntegerVertexInputs(kBase, sodium), hash);
    EXPECT_NE(ProgramFactory::MixNarrowIntegerVertexInputs(kBase + 1, sodium), hash);
    for (const auto field : {&NarrowIntegerVertexInputs::zeroExtend16, &NarrowIntegerVertexInputs::signExtend8,
                             &NarrowIntegerVertexInputs::signExtend16})
        EXPECT_NE(ProgramFactory::MixNarrowIntegerVertexInputs(kBase, Only(field, 3)), hash);
    EXPECT_NE(ProgramFactory::MixNarrowIntegerVertexInputs(kBase, Only(&NarrowIntegerVertexInputs::zeroExtend8, 4)),
              hash);
}

// The program half lives on the cache entry, and the cache's robin-hood insert moves entries
// around: a mask left behind by a move would pick another program's variant.
TEST(NarrowIntegerVertexFormats, IntegerInputMasksTravelWithTheProgramEntry) {
    ProgramFactory::VkProgramObject source;
    source.integerVertexInputs.signedInputs = 1u << 3;
    source.integerVertexInputs.unsignedInputs = 1u << 7;

    ProgramFactory::VkProgramObject moved(std::move(source));
    EXPECT_EQ(moved.integerVertexInputs.signedInputs, 1u << 3);
    EXPECT_EQ(moved.integerVertexInputs.unsignedInputs, 1u << 7);
    EXPECT_EQ(source.integerVertexInputs.signedInputs, 0u);
    EXPECT_EQ(source.integerVertexInputs.unsignedInputs, 0u);

    ProgramFactory::VkProgramObject assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.integerVertexInputs.signedInputs, 1u << 3);
    EXPECT_EQ(assigned.integerVertexInputs.unsignedInputs, 1u << 7);
    EXPECT_EQ(moved.integerVertexInputs.signedInputs, 0u);
    EXPECT_EQ(moved.integerVertexInputs.unsignedInputs, 0u);
}
