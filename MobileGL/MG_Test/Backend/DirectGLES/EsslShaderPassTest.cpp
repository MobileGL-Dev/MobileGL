// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/EsslShaderPassTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// The post-transpile textual passes the DirectGLES ("Espryt") backend runs over the ESSL
// SPIRV-Cross hands it (MG_Backend/DirectGLES/Utils.cpp). No GL context and no driver: the
// passes are pure String -> String, so the shapes they have to survive can be pinned here
// instead of only on a device.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/Utils.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_WRITE_ALIAS_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RemoveLayoutBinding;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::SplitReadWriteImageUniforms;

namespace {
    Bool Contains(const String& haystack, const String& needle) {
        return haystack.find(needle) != String::npos;
    }

    SizeT CountOf(const String& haystack, const String& needle) {
        SizeT count = 0;
        for (SizeT pos = haystack.find(needle); pos != String::npos; pos = haystack.find(needle, pos + 1)) {
            ++count;
        }
        return count;
    }

    String WriteAlias(const String& name) { return String(IMAGE_WRITE_ALIAS_PREFIX) + name; }
} // namespace

// The bug the pass exists for. SPIRV-Cross speculatively marks every storage image
// NonWritable+NonReadable, then clears NonReadable at the OpImageRead and NonWritable at the
// OpImageWrite, so an image the shader both reads and writes comes out carrying NEITHER
// `readonly` nor `writeonly` - which ESSL rejects for any format other than r32f/r32i/r32ui
// (GLSL ES 3.20 4.10). The device compile then fails and the draw silently binds program 0.
TEST(SplitReadWriteImageUniformsTest, ReadWriteImageIsSplitIntoAnAliasingPair) {
    const String source = R"(#version 320 es
layout(binding = 2, rgba8) uniform highp image2D goku;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    highp vec4 loaded = imageLoad(goku, ivec2(gl_FragCoord.xy));
    imageStore(goku, ivec2(gl_FragCoord.xy), loaded + vec4(0.25));
    mg_FragColor = loaded;
}
)";
    const String out = SplitReadWriteImageUniforms(source);

    // Both halves: same binding, same format, same type - which is what makes two image
    // variables on one image unit legal.
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform readonly highp image2D goku;"));
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform writeonly highp image2D " + WriteAlias("goku") + ";"));

    // The load keeps the original name, the store moves to the writeonly half.
    EXPECT_TRUE(Contains(out, "imageLoad(goku,"));
    EXPECT_TRUE(Contains(out, "imageStore(" + WriteAlias("goku") + ","));
    EXPECT_FALSE(Contains(out, "imageStore(goku,"));
}

// The split has to survive RemoveLayoutBinding, which runs straight after it: an ES image
// unit cannot be assigned through the API, so the layout qualifier is the only binding
// mechanism and both halves must still carry theirs afterwards.
TEST(SplitReadWriteImageUniformsTest, BothHalvesKeepTheirBindingThroughRemoveLayoutBinding) {
    const String source = R"(#version 320 es
layout(binding = 5, rgba8) uniform highp image2D goku;
void main()
{
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
}
)";
    const String out = RemoveLayoutBinding(SplitReadWriteImageUniforms(source));
    EXPECT_EQ(CountOf(out, "binding = 5"), 2u);
}

// Cheap hardening: the pass does not depend on SPIRV-Cross getting the read-only case right,
// and a shader that only reads must not pay for a second uniform.
TEST(SplitReadWriteImageUniformsTest, ReadOnlyImageGetsReadonlyAndIsNotSplit) {
    const String source = R"(#version 320 es
layout(binding = 1, rgba16f) uniform highp image2DArray trunks;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = imageLoad(trunks, ivec3(0));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba16f) uniform readonly highp image2DArray trunks;"));
    EXPECT_FALSE(Contains(out, "writeonly"));
    EXPECT_FALSE(Contains(out, IMAGE_WRITE_ALIAS_PREFIX));
    EXPECT_EQ(CountOf(out, "image2DArray"), 1u);
}

TEST(SplitReadWriteImageUniformsTest, WriteOnlyImageGetsWriteonlyAndIsNotSplit) {
    const String source = R"(#version 320 es
layout(binding = 3, rgba8) uniform highp image2D gohan;
void main()
{
    imageStore(gohan, ivec2(0), vec4(1.0));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "layout(binding = 3, rgba8) uniform writeonly highp image2D gohan;"));
    EXPECT_FALSE(Contains(out, "readonly"));
    EXPECT_FALSE(Contains(out, IMAGE_WRITE_ALIAS_PREFIX));
}

// r32f / r32i / r32ui are exactly the formats GLSL ES 3.20 4.10 exempts from the rule, so a
// read+write image in one of them is already legal and must not be doubled.
TEST(SplitReadWriteImageUniformsTest, ExemptFormatsAreLeftCompletelyAlone) {
    for (const char* format : {"r32f", "r32i", "r32ui"}) {
        const String type = String(format) == "r32f" ? "image2D" : (String(format) == "r32i" ? "iimage2D" : "uimage2D");
        const String source = "#version 320 es\nlayout(binding = 4, " + String(format) + ") uniform highp " + type +
                              " vegeta;\nvoid main()\n{\n    imageStore(vegeta, ivec2(0), imageLoad(vegeta, "
                              "ivec2(0)));\n}\n";
        EXPECT_EQ(SplitReadWriteImageUniforms(source), source) << "format " << format;
    }
}

// A declaration SPIRV-Cross already qualified is none of this pass's business.
TEST(SplitReadWriteImageUniformsTest, AlreadyQualifiedDeclarationsAreUntouched) {
    const String source = R"(#version 320 es
layout(binding = 0, rgba8) uniform readonly highp image2D reader;
layout(binding = 1, rgba8) uniform writeonly highp image2D writer;
void main()
{
    imageStore(writer, ivec2(0), imageLoad(reader, ivec2(0)));
}
)";
    EXPECT_EQ(SplitReadWriteImageUniforms(source), source);
}

// The binding of an image array is the array's base; splitting must keep the array on both
// halves (dropping the subscript would silently turn 3 units into 1).
TEST(SplitReadWriteImageUniformsTest, ImageArraySplitsAndKeepsItsArraySize) {
    const String source = R"(#version 320 es
layout(binding = 6, rgba8) uniform highp image2D gohan[3];
void main()
{
    imageStore(gohan[1], ivec2(0), imageLoad(gohan[2], ivec2(0)));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "layout(binding = 6, rgba8) uniform readonly highp image2D gohan[3];"));
    EXPECT_TRUE(Contains(out,
                         "layout(binding = 6, rgba8) uniform writeonly highp image2D " + WriteAlias("gohan") + "[3];"));
    EXPECT_TRUE(Contains(out, "imageStore(" + WriteAlias("gohan") + "[1],"));
    EXPECT_TRUE(Contains(out, "imageLoad(gohan[2],"));
}

// The rewrite is by identifier, not by substring: "goku" must not reach into "goku_hd", and
// the two images have to be classified independently.
TEST(SplitReadWriteImageUniformsTest, ANameThatIsAPrefixOfAnotherIsNotClobbered) {
    const String source = R"(#version 320 es
layout(binding = 1, rgba8) uniform highp image2D goku;
layout(binding = 2, rgba8) uniform highp image2D goku_hd;
void main()
{
    highp vec4 loaded = imageLoad(goku, ivec2(0));
    imageStore(goku, ivec2(0), loaded);
    imageStore(goku_hd, ivec2(0), loaded);
}
)";
    const String out = SplitReadWriteImageUniforms(source);

    // goku is read+write -> split; goku_hd is write-only -> qualified in place, not split.
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba8) uniform readonly highp image2D goku;"));
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba8) uniform writeonly highp image2D " + WriteAlias("goku") + ";"));
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform writeonly highp image2D goku_hd;"));
    EXPECT_TRUE(Contains(out, "imageStore(goku_hd,"));
    EXPECT_FALSE(Contains(out, WriteAlias("goku") + "_hd"));
    EXPECT_FALSE(Contains(out, WriteAlias("goku_hd")));
}

// Other qualifiers belong to both halves, and the memory qualifier goes where SPIRV-Cross
// puts it (right after `uniform`) so the image-rebinding regex in Managers.cpp still matches.
TEST(SplitReadWriteImageUniformsTest, ExistingQualifiersAreCarriedOntoBothHalves) {
    const String source = R"(#version 320 es
layout(binding = 2, rgba8) uniform coherent restrict highp image2D goku;
void main()
{
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "uniform readonly coherent restrict highp image2D goku;"));
    EXPECT_TRUE(
        Contains(out, "uniform writeonly coherent restrict highp image2D " + WriteAlias("goku") + ";"));
}

// imageSize reads no texels and writes none, so it decides nothing; readonly is what keeps
// such a declaration legal.
TEST(SplitReadWriteImageUniformsTest, ImageSizeAloneDoesNotCountAsALoadOrAStore) {
    const String source = R"(#version 320 es
layout(binding = 8, rgba8ui) uniform highp uimage2D sizeOnly;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = vec4(float(imageSize(sizeOnly).x));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "layout(binding = 8, rgba8ui) uniform readonly highp uimage2D sizeOnly;"));
    EXPECT_FALSE(Contains(out, IMAGE_WRITE_ALIAS_PREFIX));
}

// The alias must not land on an identifier the shader already uses.
TEST(SplitReadWriteImageUniformsTest, AliasNameAvoidsAnExistingIdentifier) {
    const String source = R"(#version 320 es
layout(binding = 6, rgba8) uniform highp image2D taken;
highp vec4 mg_imageWrite_taken;
void main()
{
    imageStore(taken, ivec2(0), imageLoad(taken, ivec2(0)) + mg_imageWrite_taken);
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_FALSE(Contains(out, "image2D " + WriteAlias("taken") + ";"));
    EXPECT_TRUE(Contains(out, "image2D " + WriteAlias("taken") + "X;"));
    EXPECT_TRUE(Contains(out, "imageStore(" + WriteAlias("taken") + "X,"));
    EXPECT_TRUE(Contains(out, "+ mg_imageWrite_taken)"));
}

// A use the pass cannot account for (here: the image handed to a user function) means it
// cannot know every store site, so it declines rather than emitting a half-rewritten shader.
TEST(SplitReadWriteImageUniformsTest, AnUnrecognizedUseLeavesTheDeclarationAlone) {
    const String source = R"(#version 320 es
layout(binding = 2, rgba8) uniform highp image2D passed;
highp vec4 helper(highp image2D img) { return imageLoad(img, ivec2(0)); }
void main()
{
    imageStore(passed, ivec2(0), helper(passed));
}
)";
    EXPECT_EQ(SplitReadWriteImageUniforms(source), source);
}

TEST(SplitReadWriteImageUniformsTest, ShaderWithoutImagesIsReturnedUnchanged) {
    const String source = R"(#version 320 es
layout(binding = 0) uniform highp sampler2D goku;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = texture(goku, vec2(0.5));
}
)";
    EXPECT_EQ(SplitReadWriteImageUniforms(source), source);
}
