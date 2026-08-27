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

#include <limits>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::BakeImageFormatQualifiers;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::BuildPassthroughTessControlEssl;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::ExtractPerVertexBlockMembers;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::ForceFlatIntegerVaryings;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_ARRAY_ELEMENT_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_READONLY_ALIAS_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_SPLIT_READ_ALIAS_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_WRITE_ALIAS_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::IMAGE_WRITEONLY_ALIAS_PREFIX;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::ImageArrayUnitPlan;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RemapImageArrayElementUnits;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RemoveLayoutBinding;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RequestExtendedImageFormats;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RequestViewportArrayExtension;
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

    // The pass tags the name of every declaration it rewrites with the REPAIR it applied, so the
    // expectations have to spell the tag that matches how the fixture uses the image.
    String RoAlias(const String& name) { return String(IMAGE_READONLY_ALIAS_PREFIX) + name; }
    String WoAlias(const String& name) { return String(IMAGE_WRITEONLY_ALIAS_PREFIX) + name; }
    String RwAlias(const String& name) { return String(IMAGE_SPLIT_READ_ALIAS_PREFIX) + name; }
    // The writeonly half is minted from the ALREADY access-tagged name, so it carries both.
    String WriteAlias(const String& name) { return String(IMAGE_WRITE_ALIAS_PREFIX) + name; }
    String SplitWriteAlias(const String& name) { return WriteAlias(RwAlias(name)); }
    // The scalar RemapImageArrayElementUnits declares for one element of a split image array.
    String Elem(const String& name, Int element) {
        return String(IMAGE_ARRAY_ELEMENT_PREFIX) + name + "_" + std::to_string(element);
    }
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
    // variables on one image unit legal - and both `coherent`, which is what makes the store
    // through one of them visible to the load through the other.
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform coherent readonly highp image2D " +
                                  RwAlias("goku") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform coherent writeonly highp image2D " +
                                  SplitWriteAlias("goku") + ";"))
        << out;

    // The load goes to the readonly half, the store to the writeonly one, and neither is called
    // what the application called it any more.
    EXPECT_TRUE(Contains(out, "imageLoad(" + RwAlias("goku") + ","));
    EXPECT_TRUE(Contains(out, "imageStore(" + SplitWriteAlias("goku") + ","));
    EXPECT_FALSE(Contains(out, "imageStore(goku,"));
    EXPECT_FALSE(Contains(out, "imageLoad(goku,"));
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
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba16f) uniform readonly highp image2DArray " +
                                  RoAlias("trunks") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "imageLoad(" + RoAlias("trunks") + ","));
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
    EXPECT_TRUE(
        Contains(out, "layout(binding = 3, rgba8) uniform writeonly highp image2D " + WoAlias("gohan") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + WoAlias("gohan") + ","));
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

// A declaration SPIRV-Cross already qualified needs no REPAIR - but it still needs the rename.
// The input to this pass is SPIRV-Cross output, not application source, and SPIRV-Cross picks
// `readonly` or `writeonly` from the accesses of the stage it is emitting, so "already qualified"
// says nothing about whether the other stages spell it the same way. The qualifiers must survive
// untouched; only the identifier changes.
TEST(SplitReadWriteImageUniformsTest, AlreadyQualifiedDeclarationsAreRenamedButNotRequalified) {
    const String source = R"(#version 320 es
layout(binding = 0, rgba8) uniform readonly highp image2D reader;
layout(binding = 1, rgba8) uniform writeonly highp image2D writer;
void main()
{
    imageStore(writer, ivec2(0), imageLoad(reader, ivec2(0)));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "layout(binding = 0, rgba8) uniform readonly highp image2D " +
                                  RoAlias("reader") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba8) uniform writeonly highp image2D " +
                                  WoAlias("writer") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + WoAlias("writer") + ",")) << out;
    EXPECT_TRUE(Contains(out, "imageLoad(" + RoAlias("reader") + ",")) << out;
    // Neither declaration is doubled and neither gains a qualifier it did not have: this is a
    // rename, not a repair.
    EXPECT_FALSE(Contains(out, IMAGE_WRITE_ALIAS_PREFIX)) << out;
    EXPECT_EQ(CountOf(out, "coherent"), 0u) << out;
    EXPECT_FALSE(Contains(out, "memoryBarrierImage")) << out;
}

// A declaration carrying BOTH qualifiers is a spelling no per-stage access analysis produces, so
// it came from the application and reads the same in every stage. Nothing to rename.
TEST(SplitReadWriteImageUniformsTest, ADeclarationQualifiedBothWaysIsLeftCompletelyAlone) {
    const String source = R"(#version 320 es
layout(binding = 0, rgba8) uniform readonly writeonly highp image2D inert;
void main()
{
    highp ivec2 size = imageSize(inert);
    if (size.x < 0) discard;
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
    EXPECT_TRUE(Contains(out, "layout(binding = 6, rgba8) uniform coherent readonly highp image2D " +
                                  RwAlias("gohan") + "[3];"))
        << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 6, rgba8) uniform coherent writeonly highp image2D " +
                                  SplitWriteAlias("gohan") + "[3];"))
        << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + SplitWriteAlias("gohan") + "[1],"));
    EXPECT_TRUE(Contains(out, "imageLoad(" + RwAlias("gohan") + "[2],"));
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

    // goku is read+write -> split (and coherent with it); goku_hd is write-only -> qualified in
    // place, not split, and left non-coherent because nothing aliases it. Both are renamed.
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba8) uniform coherent readonly highp image2D " +
                                  RwAlias("goku") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 1, rgba8) uniform coherent writeonly highp image2D " +
                                  SplitWriteAlias("goku") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 2, rgba8) uniform writeonly highp image2D " +
                                  WoAlias("goku_hd") + ";"))
        << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + WoAlias("goku_hd") + ","));
    EXPECT_FALSE(Contains(out, SplitWriteAlias("goku") + "_hd"));
    EXPECT_FALSE(Contains(out, SplitWriteAlias("goku_hd")));
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
    EXPECT_TRUE(Contains(out, "uniform readonly coherent restrict highp image2D " + RwAlias("goku") + ";"))
        << out;
    EXPECT_TRUE(
        Contains(out, "uniform writeonly coherent restrict highp image2D " + SplitWriteAlias("goku") + ";"))
        << out;
    // ...and the coherent the split adds is not a SECOND one: a repeated memory qualifier is a
    // compile error in ESSL, so the source's own has to be recognized.
    EXPECT_EQ(CountOf(out, "coherent"), 2u);
}

// The visibility half of the split, and the reason it is not cosmetic: GLSL orders a
// same-variable read-after-write within one invocation by construction, but once the store goes
// through `mg_imageWrite_goku` and the load through `goku` the two are DIFFERENT variables, and
// the ordering only holds if both are coherent. Desktop sources almost never say so - they had
// no reason to - which is how KHR-GL4x.shader_image_load_store.advanced-memory-order's
// store/load/compare loop started reading back the value it had not stored yet.
TEST(SplitReadWriteImageUniformsTest, SplitPairIsMadeCoherentEvenWhenTheSourceIsNot) {
    const String source = R"(#version 320 es
layout(binding = 2, rgba8) uniform highp image2D goku;
layout(binding = 3, rgba8) uniform highp image2D storeOnly;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    imageStore(goku, ivec2(0), vec4(1.0));
    mg_FragColor = imageLoad(goku, ivec2(0));
    imageStore(storeOnly, ivec2(0), vec4(2.0));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "uniform coherent readonly highp image2D " + RwAlias("goku") + ";")) << out;
    EXPECT_TRUE(Contains(out, "uniform coherent writeonly highp image2D " + SplitWriteAlias("goku") + ";"))
        << out;
    // Exactly the two halves of the pair, and nothing else: the store-only image is repaired in
    // place, has no alias to stay visible to, and must not pay for uncached access.
    EXPECT_EQ(CountOf(out, "coherent"), 2u);
    EXPECT_TRUE(Contains(out, "uniform writeonly highp image2D " + WoAlias("storeOnly") + ";")) << out;
}

// The ORDERING half of the split, which `coherent` alone does not buy. Coherent makes the store
// through one variable VISIBLE to a load through the other; it says nothing about the order of
// the two within a single invocation, and the ES compiler - seeing a write to one variable and a
// read of another it has no reason to believe alias - is free to serve the read from before the
// write. That is what advanced-memory-order measured on Adreno with the coherent pair already in
// place. memoryBarrierImage() is the primitive that orders them.
TEST(SplitReadWriteImageUniformsTest, EverySplitStoreIsFollowedByAnImageMemoryBarrier) {
    const String source = R"(#version 320 es
layout(binding = 2, rgba8) uniform highp image2D goku;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    imageStore(goku, ivec2(0), vec4(1.0));
    highp vec4 first = imageLoad(goku, ivec2(0));
    imageStore(goku, ivec2(0), vec4(2.0));
    mg_FragColor = first + imageLoad(goku, ivec2(0));
}
)";
    const String out = SplitReadWriteImageUniforms(source);

    EXPECT_TRUE(
        Contains(out, "imageStore(" + SplitWriteAlias("goku") + ", ivec2(0), vec4(1.0)); memoryBarrierImage();"))
        << out;
    EXPECT_TRUE(
        Contains(out, "imageStore(" + SplitWriteAlias("goku") + ", ivec2(0), vec4(2.0)); memoryBarrierImage();"))
        << out;
    // One per store, not one per shader and not one per load.
    EXPECT_EQ(CountOf(out, "memoryBarrierImage();"), 2u) << out;
}

// The barrier belongs to the SPLIT alone. A store-only image was repaired in place, nothing
// aliases it, and paying for a barrier there would slow down every shader that merely writes an
// image - which is most of them.
TEST(SplitReadWriteImageUniformsTest, ARepairedButUnsplitStoreGetsNoBarrier) {
    const String source = R"(#version 320 es
layout(binding = 3, rgba8) uniform highp image2D storeOnly;
void main()
{
    imageStore(storeOnly, ivec2(0), vec4(1.0));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "uniform writeonly highp image2D " + WoAlias("storeOnly") + ";")) << out;
    EXPECT_FALSE(Contains(out, "memoryBarrierImage")) << out;
}

// The store site is found by matching the call's own parentheses, not by looking for the next
// ')', so a nested call in the value argument does not truncate the statement and the barrier
// still lands after the whole thing.
TEST(SplitReadWriteImageUniformsTest, TheBarrierLandsAfterAStoreWithNestedParentheses) {
    const String source = R"(#version 320 es
layout(binding = 6, rgba8) uniform highp image2D gohan[3];
void main()
{
    imageStore(gohan[1], ivec2(0), max(imageLoad(gohan[2], ivec2(0)), vec4(0.5)));
}
)";
    const String out = SplitReadWriteImageUniforms(source);
    EXPECT_TRUE(Contains(out, "max(imageLoad(" + RwAlias("gohan") +
                                  "[2], ivec2(0)), vec4(0.5))); memoryBarrierImage();"))
        << out;
    EXPECT_EQ(CountOf(out, "memoryBarrierImage();"), 1u) << out;
}

// The split is the one thing that makes a stage declare MORE image uniforms than the application
// did, and MobileGL keeps advertising GL_MAX_*_IMAGE_UNIFORMS unadjusted (lowering it would fail
// basic-api and NotSupported-out every case that only uses readonly/writeonly images). So the
// count has to be reportable, or a link failure caused by the doubling looks like a driver
// mystery - which is what KHR-GL4x.shader_image_load_store.multiple-uniforms will hit the moment
// the format work stops masking it.
TEST(SplitReadWriteImageUniformsTest, TheSplitCountIsReportedToTheCaller) {
    const String twoSplits = R"(#version 320 es
layout(binding = 0, rgba8) uniform highp image2D goku;
layout(binding = 1, rgba16f) uniform highp image2D gohan;
layout(binding = 2, rgba8) uniform highp image2D storeOnly;
void main()
{
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
    imageStore(gohan, ivec2(0), imageLoad(gohan, ivec2(0)));
    imageStore(storeOnly, ivec2(0), vec4(0.0));
}
)";
    Uint splitCount = 99u;
    SplitReadWriteImageUniforms(twoSplits, &splitCount);
    EXPECT_EQ(splitCount, 2u) << "only the read+write pair counts; the store-only repair adds no uniform";

    // Every early return has to write the count too, or a caller reads whatever was there before.
    const String noImages = R"(#version 320 es
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = vec4(1.0);
}
)";
    splitCount = 99u;
    SplitReadWriteImageUniforms(noImages, &splitCount);
    EXPECT_EQ(splitCount, 0u);
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
    EXPECT_TRUE(Contains(out, "layout(binding = 8, rgba8ui) uniform readonly highp uimage2D " +
                                  RoAlias("sizeOnly") + ";"))
        << out;
    // The rename has to reach imageSize too, or the declaration and its only use stop agreeing.
    EXPECT_TRUE(Contains(out, "imageSize(" + RoAlias("sizeOnly") + ")")) << out;
    EXPECT_FALSE(Contains(out, IMAGE_WRITE_ALIAS_PREFIX));
}

// Neither minted name may land on an identifier the shader already uses - and there are two of
// them now, the access-tagged name of the repaired declaration and the writeonly half built on
// top of it. Both collisions are exercised at once.
TEST(SplitReadWriteImageUniformsTest, AliasNamesAvoidExistingIdentifiers) {
    const String stageCollision = RwAlias("taken");
    const String writeCollision = SplitWriteAlias("taken");
    const String source = "#version 320 es\n"
                          "layout(binding = 6, rgba8) uniform highp image2D taken;\n"
                          "highp vec4 " +
                          stageCollision + ";\nhighp vec4 " + writeCollision +
                          ";\nvoid main()\n{\n"
                          "    imageStore(taken, ivec2(0), imageLoad(taken, ivec2(0)) + " +
                          stageCollision + " + " + writeCollision + ");\n}\n";
    const String out = SplitReadWriteImageUniforms(source);

    EXPECT_FALSE(Contains(out, "image2D " + stageCollision + ";")) << out;
    EXPECT_FALSE(Contains(out, "image2D " + writeCollision + ";")) << out;
    EXPECT_TRUE(Contains(out, "image2D " + stageCollision + "X;")) << out;
    EXPECT_TRUE(Contains(out, "image2D " + writeCollision + "X;")) << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + writeCollision + "X,")) << out;
    EXPECT_TRUE(Contains(out, "imageLoad(" + stageCollision + "X,")) << out;
    // ...and the globals that forced the suffix are still themselves.
    EXPECT_TRUE(Contains(out, "highp vec4 " + stageCollision + ";")) << out;
    EXPECT_TRUE(Contains(out, "highp vec4 " + writeCollision + ";")) << out;
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
    // Declining means declining EVERYTHING: no qualifier, and no rename either. A rename that
    // moved the declaration but not the use inside helper() would be a compile error rather than
    // the wrong-but-compiling shader this pass refuses to guess at.
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

// The defect the rename exists for. The pass sees ONE stage at a time and picks the memory
// qualifier from the accesses in THAT stage, so a vertex shader that only stores and a fragment
// shader that only loads the same image came out `writeonly g_image` and `readonly g_image` -
// two declarations of one uniform name that GLSL requires to be identical. Adreno merges them
// and silently discards the vertex-stage stores (advanced-memory-dependentInvocation reads back
// the untouched zeros, with LINK_STATUS = 1 and an empty driver log). Tagging by the repair
// leaves nothing to merge.
TEST(SplitReadWriteImageUniformsTest, StagesThatUseAnImageDifferentlyGetDifferentNames) {
    const String vertexSource = R"(#version 320 es
layout(binding = 0, rgba32f) uniform coherent highp image2D g_image;
void main()
{
    imageStore(g_image, ivec2(0), vec4(1.0));
    gl_Position = vec4(0.0);
}
)";
    const String fragmentSource = R"(#version 320 es
layout(binding = 0, rgba32f) uniform coherent highp image2D g_image;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = imageLoad(g_image, ivec2(0));
}
)";
    const String vsOut = SplitReadWriteImageUniforms(vertexSource);
    const String fsOut = SplitReadWriteImageUniforms(fragmentSource);

    const String vsName = WoAlias("g_image");
    const String fsName = RoAlias("g_image");
    EXPECT_NE(vsName, fsName);
    EXPECT_TRUE(Contains(vsOut, "uniform writeonly coherent highp image2D " + vsName + ";")) << vsOut;
    EXPECT_TRUE(Contains(fsOut, "uniform readonly coherent highp image2D " + fsName + ";")) << fsOut;
    EXPECT_TRUE(Contains(vsOut, "imageStore(" + vsName + ",")) << vsOut;
    EXPECT_TRUE(Contains(fsOut, "imageLoad(" + fsName + ",")) << fsOut;
    // The whole point: after the rewrite the two stages no longer declare a common name, so
    // there is nothing for a linker to merge and mis-qualify.
    EXPECT_FALSE(Contains(vsOut, fsName)) << vsOut;
    EXPECT_FALSE(Contains(fsOut, vsName)) << fsOut;
    // Both bindings are untouched - the image unit is still the same one.
    EXPECT_TRUE(Contains(vsOut, "binding = 0"));
    EXPECT_TRUE(Contains(fsOut, "binding = 0"));
}

// The same defect, in the shape it actually reaches the driver in. SPIRV-Cross emits the access
// qualifier ITSELF whenever the stage only loads or only stores, so the declaration arrives here
// already legal - and this pass used to skip it on exactly that ground, leaving the vertex stage's
// `coherent writeonly g_image` and the fragment stage's `coherent readonly g_image` sharing one
// name. That is the pair a raw-ES probe on the Adreno 830 reproduces with no MobileGL in the
// process: the fragment stage reads back the untouched zeros
// (KHR-GL4x.shader_image_load_store.advanced-memory-dependentInvocation's [1,0,0,0.2]), and
// renaming either half fixes it. This is the emitted text of that test, verbatim.
TEST(SplitReadWriteImageUniformsTest, StagesSpirvCrossQualifiedDifferentlyGetDifferentNames) {
    const String vertexSource = R"(#version 320 es
layout(binding = 1, rgba32f) uniform coherent writeonly highp image2D g_image;
void main()
{
    imageStore(g_image, ivec2(0), vec4(2.0));
    gl_Position = vec4(0.0);
}
)";
    const String fragmentSource = R"(#version 320 es
layout(binding = 1, rgba32f) uniform coherent readonly highp image2D g_image;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = imageLoad(g_image, ivec2(0));
}
)";
    const String vsOut = SplitReadWriteImageUniforms(vertexSource);
    const String fsOut = SplitReadWriteImageUniforms(fragmentSource);

    const String vsName = WoAlias("g_image");
    const String fsName = RoAlias("g_image");
    EXPECT_NE(vsName, fsName);
    EXPECT_TRUE(Contains(vsOut, "uniform coherent writeonly highp image2D " + vsName + ";")) << vsOut;
    EXPECT_TRUE(Contains(fsOut, "uniform coherent readonly highp image2D " + fsName + ";")) << fsOut;
    EXPECT_TRUE(Contains(vsOut, "imageStore(" + vsName + ",")) << vsOut;
    EXPECT_TRUE(Contains(fsOut, "imageLoad(" + fsName + ",")) << fsOut;
    // Nothing left for a linker to merge and mis-qualify...
    EXPECT_FALSE(Contains(vsOut, fsName)) << vsOut;
    EXPECT_FALSE(Contains(fsOut, vsName)) << fsOut;
    // ...and the image unit is still the one the application asked for.
    EXPECT_TRUE(Contains(vsOut, "binding = 1")) << vsOut;
    EXPECT_TRUE(Contains(fsOut, "binding = 1")) << fsOut;
}

// ...and the budget half of it: two stages SPIRV-Cross qualified the SAME way must still land on
// one shared name, or every stage that names the image spends an image location of its own.
TEST(SplitReadWriteImageUniformsTest, StagesSpirvCrossQualifiedAlikeShareOneName) {
    const String stage = R"(#version 320 es
layout(binding = 1, rgba32f) uniform coherent readonly highp image2D g_image;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = imageLoad(g_image, ivec2(0));
}
)";
    const String first = SplitReadWriteImageUniforms(stage);
    const String second = SplitReadWriteImageUniforms(stage);
    EXPECT_EQ(first, second);
    EXPECT_TRUE(Contains(first, "uniform coherent readonly highp image2D " + RoAlias("g_image") + ";"))
        << first;
}

// The other side of that coin, and the one a per-STAGE tag got wrong. Two stages that use the
// image the same way emit byte-identical declarations, so they must arrive at ONE shared name:
// Adreno allocates an image LOCATION per distinct uniform, and giving each stage its own name
// multiplied a program's image-uniform count by the number of stages that mention it - which is
// how the five stages of KHR-GL43.shading_language_420pack.binding_images_texture_type_* went
// from 6 image uniforms to 30 and drew "Error: Image Image location or component exceeds max
// allowed." out of the Adreno 830 linker, with LINK_STATUS = TRUE already published by the
// frontend and every draw silently doing nothing.
TEST(SplitReadWriteImageUniformsTest, StagesThatUseAnImageAlikeShareOneName) {
    const String vertexSource = R"(#version 320 es
layout(binding = 1, rgba8) uniform highp image2D goku;
void main()
{
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
    gl_Position = vec4(0.0);
}
)";
    const String fragmentSource = R"(#version 320 es
layout(binding = 1, rgba8) uniform highp image2D goku;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
    mg_FragColor = vec4(0.0);
}
)";
    const String vsOut = SplitReadWriteImageUniforms(vertexSource);
    const String fsOut = SplitReadWriteImageUniforms(fragmentSource);

    // One name, arrived at independently by two different stages, so the linker merges them
    // back into the single image uniform the application declared.
    for (const String& out : {vsOut, fsOut}) {
        EXPECT_TRUE(Contains(out, "uniform coherent readonly highp image2D " + RwAlias("goku") + ";")) << out;
        EXPECT_TRUE(Contains(out, "uniform coherent writeonly highp image2D " + SplitWriteAlias("goku") + ";"))
            << out;
        EXPECT_TRUE(Contains(out, "imageLoad(" + RwAlias("goku") + ",")) << out;
        EXPECT_TRUE(Contains(out, "imageStore(" + SplitWriteAlias("goku") + ",")) << out;
    }
}

// One tag per repair, all three distinct, and each a legal identifier stem.
TEST(SplitReadWriteImageUniformsTest, EveryAccessTagIsDistinct) {
    const String prefixes[] = {String(IMAGE_READONLY_ALIAS_PREFIX), String(IMAGE_WRITEONLY_ALIAS_PREFIX),
                               String(IMAGE_SPLIT_READ_ALIAS_PREFIX), String(IMAGE_WRITE_ALIAS_PREFIX)};
    Vector<String> seenPrefixes;
    for (const String& prefix : prefixes) {
        // A GLSL identifier may not contain "__" (GLSL ES 3.20 3.7), and the prefix is glued
        // straight onto a name that may itself start with '_'.
        EXPECT_EQ(prefix.find("__"), String::npos) << prefix;
        for (const String& seen : seenPrefixes) {
            EXPECT_NE(seen, prefix) << prefix;
            // Nor may one be a prefix of another: the write half is minted on top of an
            // already-tagged name, so a shared stem would let two repairs collide.
            EXPECT_NE(prefix.rfind(seen, 0), 0u) << prefix << " vs " << seen;
        }
        seenPrefixes.push_back(prefix);
    }
}

// ---------------------------------------------------------------------------------------
// RemapImageArrayElementUnits
//
// ES takes an image unit only from layout(binding=N), and one declaration carries one of them,
// so an image array's elements land on N, N+1, N+2, ... Desktop GL lets an application point
// each element wherever it likes with glUniform1i, which ES makes an INVALID_OPERATION on an
// image uniform - there is no API side to fix, so the emitted text has to carry it.

namespace {
    // The advanced-sso-simple shape: a four-element image array on units 0, 2, 4, 6. The
    // subscripts are literals because LegalizeResourceArrayIndexingForEssl has already folded
    // the conformance case's `for (int i = 0; i < g_image.length(); ++i)` - ESSL forbids a
    // non-constant image-array subscript outright, so a loop counter never reaches this pass.
    const char* const kSsoImageArrayFS = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[4];
void main()
{
    imageStore(g_image[0], ivec2(gl_FragCoord.xy), vec4(1.0));
    imageStore(g_image[1], ivec2(gl_FragCoord.xy), vec4(1.0));
    imageStore(g_image[2], ivec2(gl_FragCoord.xy), vec4(1.0));
    imageStore(g_image[3], ivec2(gl_FragCoord.xy), vec4(1.0));
}
)";

    ImageArrayUnitPlan Plan(const String& name, const Vector<Int>& units) {
        ImageArrayUnitPlan plan;
        plan.name = name;
        plan.units = units;
        return plan;
    }
} // namespace

// The defect, end to end. Elements 0..3 need units 0, 2, 4, 6, so the array becomes four scalars
// carrying those four bindings. Before this, the single stamped binding sent the four elements to
// units 0, 1, 2, 3.
TEST(RemapImageArrayElementUnitsTest, NonConsecutiveUnitsSplitIntoOneScalarPerElement) {
    Vector<String> declined;
    const String out =
        RemapImageArrayElementUnits(kSsoImageArrayFS, {Plan("g_image", {0, 2, 4, 6})}, &declined);

    EXPECT_TRUE(declined.empty()) << (declined.empty() ? String() : declined[0]);
    const Int units[4] = {0, 2, 4, 6};
    for (Int element = 0; element < 4; ++element) {
        EXPECT_TRUE(Contains(out, "layout(rgba32f, binding = " + std::to_string(units[element]) +
                                      ") uniform writeonly highp image2D " + Elem("g_image", element) + ";"))
            << out;
        EXPECT_TRUE(Contains(out, "imageStore(" + Elem("g_image", element) + ", ivec2(gl_FragCoord.xy)"))
            << out;
    }
    // The array is gone entirely; nothing may still address units 0,1,2,3 through it.
    EXPECT_FALSE(Contains(out, "image2D g_image[4];")) << out;
    EXPECT_FALSE(Contains(out, "g_image[")) << out;
    // Exactly the four image uniforms the application declared - what the earlier widening cost
    // was the whole SPAN, seven here, which is the budget failure mode this shape removes.
    EXPECT_EQ(CountOf(out, "image2D "), 4u) << out;
}

// The other program of the same conformance case: units 1, 3, 5, 7 in the application's own
// element ORDER, which is what carries the assignment, so it must NOT be sorted or rebased.
TEST(RemapImageArrayElementUnitsTest, EachElementCarriesTheUnitTheApplicationGaveIt) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 3) uniform writeonly highp image2D g_image[4];
void main()
{
    imageStore(g_image[0], ivec2(0), vec4(2.0));
    imageStore(g_image[3], ivec2(0), vec4(2.0));
}
)";
    const String out = RemapImageArrayElementUnits(source, {Plan("g_image", {3, 1, 7, 5})});
    const Int units[4] = {3, 1, 7, 5};
    for (Int element = 0; element < 4; ++element) {
        EXPECT_TRUE(Contains(out, "binding = " + std::to_string(units[element]) +
                                      ") uniform writeonly highp image2D " + Elem("g_image", element) + ";"))
            << out;
    }
    // Only elements 0 and 3 are ever accessed; elements 1 and 2 are declared and unused, because
    // the reflection says the array has four of them.
    EXPECT_TRUE(Contains(out, "imageStore(" + Elem("g_image", 0) + ", ivec2(0)")) << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + Elem("g_image", 3) + ", ivec2(0)")) << out;
}

// Consecutive-from-element-zero is exactly what ESSL does unaided, so the emitted text of an
// ordinary image shader must come out byte-identical. The caller filters these; the pass must
// not depend on that.
TEST(RemapImageArrayElementUnitsTest, ConsecutiveUnitsAreLeftCompletelyAlone) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 2) uniform writeonly highp image2D g_image[3];
void main()
{
    imageStore(g_image[1], ivec2(0), vec4(1.0));
}
)";
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("g_image", {2, 3, 4})}), source);
    // ...and so is a plan for an array this stage does not declare at all: the reflection is
    // program-wide, the pass runs per stage.
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("other_image", {0, 4})}), source);
}

// A subscript that is not a literal names no element, so there is no scalar to rewrite it to.
// It should never arrive - LegalizeResourceArrayIndexingForEssl runs first and ESSL rejects the
// shape outright - but if one does, guessing an element would only change WHICH unit the access
// reaches wrongly. Decline, loudly, and change nothing.
TEST(RemapImageArrayElementUnitsTest, ANonLiteralSubscriptIsDeclinedAndNamed) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[4];
void main()
{
    for (int i = 0; i < 4; i++)
    {
        imageStore(g_image[i], ivec2(gl_FragCoord.xy), vec4(1.0));
    }
}
)";
    Vector<String> declined;
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("g_image", {0, 2, 4, 6})}, &declined), source);
    ASSERT_EQ(declined.size(), 1u);
    EXPECT_TRUE(Contains(declined[0], "g_image")) << declined[0];

    // A literal that is out of the reflected range is the same class of mismatch.
    const String outOfRange = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[2];
void main()
{
    imageStore(g_image[5], ivec2(0), vec4(1.0));
}
)";
    Vector<String> outOfRangeDeclined;
    EXPECT_EQ(RemapImageArrayElementUnits(outOfRange, {Plan("g_image", {0, 5})}, &outOfRangeDeclined),
              outOfRange);
    ASSERT_EQ(outOfRangeDeclined.size(), 1u);
}

// A uint subscript IS a literal element index. SPIRV-Cross prints an index in the type SPIR-V
// gave it and LegalizeResourceArrayIndexPass mints its per-element constants in the type of the
// index it replaced, so an array walked by anything unsigned - a `uint` loop counter, or
// anything derived from gl_LocalInvocationIndex, which is uint by definition - reaches this pass
// spelled `g_image[0u]`. Refusing the `u` declined the array and left every element on the
// consecutive units one binding hands out, silently.
TEST(RemapImageArrayElementUnitsTest, AUintSubscriptIsStillALiteralElementIndex) {
    const String source = R"(#version 320 es
layout(local_size_x = 1) in;
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[3];
void main()
{
    imageStore(g_image[0u], ivec2(0), vec4(1.0));
    imageStore(g_image[2U], ivec2(0), vec4(2.0));
}
)";
    Vector<String> declined;
    const String out = RemapImageArrayElementUnits(source, {Plan("g_image", {0, 4, 8})}, &declined);
    EXPECT_TRUE(declined.empty()) << (declined.empty() ? String() : declined[0]);

    EXPECT_TRUE(Contains(out, "binding = 0) uniform writeonly highp image2D " + Elem("g_image", 0) + ";")) << out;
    EXPECT_TRUE(Contains(out, "binding = 4) uniform writeonly highp image2D " + Elem("g_image", 1) + ";")) << out;
    EXPECT_TRUE(Contains(out, "binding = 8) uniform writeonly highp image2D " + Elem("g_image", 2) + ";")) << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + Elem("g_image", 0) + ", ivec2(0), vec4(1.0))")) << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + Elem("g_image", 2) + ", ivec2(0), vec4(2.0))")) << out;
    EXPECT_FALSE(Contains(out, "g_image[")) << out;
}

// ...and the suffix is not a licence to accept anything else that ends in one: `iu` is not a
// literal, and neither is a bare `u`.
TEST(RemapImageArrayElementUnitsTest, ASuffixAloneDoesNotMakeAnExpressionALiteral) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[2];
void main()
{
    highp int iu = 1;
    imageStore(g_image[iu], ivec2(0), vec4(1.0));
}
)";
    Vector<String> declined;
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("g_image", {0, 4})}, &declined), source);
    ASSERT_EQ(declined.size(), 1u);
}

// A use the pass cannot see a subscript on has no element index to rewrite, so splitting the
// array out from under it would leave it naming a declaration that no longer exists. Decline,
// loudly, and change nothing.
TEST(RemapImageArrayElementUnitsTest, AUseWithoutASubscriptIsDeclined) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[2];
void helper();
void main()
{
    imageStore(g_image[0], ivec2(0), vec4(1.0));
    helper(g_image);
}
)";
    Vector<String> declined;
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("g_image", {0, 5})}, &declined), source);
    ASSERT_EQ(declined.size(), 1u);
    EXPECT_TRUE(Contains(declined[0], "g_image")) << declined[0];
}

// The reflection and the emitted text have to be talking about the same array. If they are not,
// the pass has misidentified something and must not rewrite on a guess.
TEST(RemapImageArrayElementUnitsTest, AnExtentThatDisagreesWithTheReflectionIsDeclined) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 0) uniform writeonly highp image2D g_image[2];
void main()
{
    imageStore(g_image[0], ivec2(0), vec4(1.0));
}
)";
    Vector<String> declined;
    EXPECT_EQ(RemapImageArrayElementUnits(source, {Plan("g_image", {0, 4, 8})}, &declined), source);
    ASSERT_EQ(declined.size(), 1u);
}

// The two passes that run after it have to see the split declarations and keep their bindings: an
// ES image unit cannot be assigned through the API, so the qualifier is the only mechanism there
// is, and an element that is both read and written is split again into a pair that must BOTH
// carry that element's own unit.
TEST(RemapImageArrayElementUnitsTest, TheSplitElementsSurviveTheLaterImagePasses) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 4) uniform highp image2D g_image[2];
void main()
{
    imageStore(g_image[1], ivec2(0), imageLoad(g_image[0], ivec2(0)));
}
)";
    String out = RemapImageArrayElementUnits(source, {Plan("g_image", {4, 6})});
    out = SplitReadWriteImageUniforms(out);
    out = RemoveLayoutBinding(out);

    // Element 0 is only ever loaded and element 1 only ever stored, so neither is split into a
    // pair - but each keeps the unit the application gave it, which the array could not express.
    EXPECT_TRUE(Contains(out, "binding = 4")) << out;
    EXPECT_TRUE(Contains(out, "binding = 6")) << out;
    EXPECT_TRUE(Contains(out, "readonly highp image2D " + RoAlias(Elem("g_image", 0)) + ";")) << out;
    EXPECT_TRUE(Contains(out, "writeonly highp image2D " + WoAlias(Elem("g_image", 1)) + ";")) << out;
    EXPECT_TRUE(Contains(out, "imageStore(" + WoAlias(Elem("g_image", 1)) + ", ivec2(0), imageLoad(" +
                                  RoAlias(Elem("g_image", 0)) + ", ivec2(0)))"))
        << out;
    // Nothing is left addressing the array.
    EXPECT_FALSE(Contains(out, "g_image[")) << out;
}

// The same element both read and written IS split into a coherent pair, and both halves have to
// inherit that element's binding - the shape the widening used to have to carry on an array.
TEST(RemapImageArrayElementUnitsTest, AnElementThatIsBothReadAndWrittenIsSplitWithItsOwnBinding) {
    const String source = R"(#version 320 es
layout(rgba32f, binding = 4) uniform highp image2D g_image[2];
void main()
{
    imageStore(g_image[1], ivec2(0), imageLoad(g_image[1], ivec2(0)));
    imageStore(g_image[0], ivec2(0), vec4(0.0));
}
)";
    String out = RemapImageArrayElementUnits(source, {Plan("g_image", {4, 9})});
    out = SplitReadWriteImageUniforms(out);
    out = RemoveLayoutBinding(out);

    // Element 1 sits on unit 9, and both halves of its split pair say so.
    EXPECT_EQ(CountOf(out, "binding = 9"), 2u) << out;
    EXPECT_TRUE(Contains(out, "readonly highp image2D " + RwAlias(Elem("g_image", 1)) + ";")) << out;
    EXPECT_TRUE(Contains(out, "writeonly highp image2D " + WriteAlias(RwAlias(Elem("g_image", 1))) + ";"))
        << out;
    EXPECT_EQ(CountOf(out, "binding = 4"), 1u) << out;
}

// The gap that let a per-STAGE image rename reach production: every fixture above declares an
// image ARRAY, and the regression it caused was in the SCALAR images sitting next to one. A
// scalar with an explicit binding has to come out of the whole chain still on ITS OWN unit,
// still spelled once, and named the same thing every stage would name it - it is the array that
// needs repairing, not its neighbour.
TEST(RemapImageArrayElementUnitsTest, AScalarImageWithItsOwnBindingIsUntouchedByTheArrayRepair) {
    const String source = R"(#version 320 es
layout(rgba8, binding = 7) uniform highp image2D goku;
layout(rgba32f, binding = 4) uniform highp image2D g_image[2];
void main()
{
    imageStore(g_image[1], ivec2(0), imageLoad(g_image[0], ivec2(0)));
    imageStore(goku, ivec2(0), imageLoad(goku, ivec2(0)));
}
)";
    Vector<String> declined;
    String out = RemapImageArrayElementUnits(source, {Plan("g_image", {4, 9})}, &declined);
    EXPECT_TRUE(declined.empty());
    // The array pass may only ever touch the arrays it was handed a plan for.
    EXPECT_TRUE(Contains(out, "layout(rgba8, binding = 7) uniform highp image2D goku;")) << out;

    out = SplitReadWriteImageUniforms(out);
    out = RemoveLayoutBinding(out);

    // Unit 7 exactly twice - the two halves of the scalar's own split pair - and nothing has
    // moved it onto one of the array's units.
    EXPECT_EQ(CountOf(out, "binding = 7"), 2u) << out;
    EXPECT_TRUE(Contains(out, "readonly highp image2D " + RwAlias("goku") + ";")) << out;
    EXPECT_TRUE(Contains(out, "writeonly highp image2D " + SplitWriteAlias("goku") + ";")) << out;
    // ...and no per-stage tag anywhere: the name a scalar gets is a function of how this text
    // uses it, so every stage that uses it the same way keeps ONE shared uniform (Adreno spends
    // an image location per distinct one).
    EXPECT_FALSE(Contains(out, "mg_imageVs_")) << out;
    EXPECT_FALSE(Contains(out, "mg_imageFs_")) << out;
    EXPECT_FALSE(Contains(out, "mg_imageCs_")) << out;
}

// ---------------------------------------------------------------------------------------
// RetargetTextureBufferExtension
//
// Buffer textures are core in the OpenGL 3.1+ context MobileGL advertises, but in ES they
// only became core in 3.2; below that they need EXT_texture_buffer or OES_texture_buffer.
// SPIRV-Cross hardcodes the EXT spelling for every Dim=Buffer image it emits below ESSL 320
// and offers no way to ask for the other one, so on a driver that advertises only the OES
// name the `: require` is a hard compile error over a single token.
// ---------------------------------------------------------------------------------------

using Tier = MobileGL::MG_External::GLESCapabilities::TextureBufferTier;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RetargetTextureBufferExtension;

namespace {
    // What SPIRV-Cross actually emits for `uniform isamplerBuffer CloudFaces;` at ESSL 310 -
    // the shape that empties Minecraft 26.3's cloud layer on a driver without the extension.
    const String kBufferTextureShader = R"(#version 310 es
#extension GL_EXT_texture_buffer : require
precision highp float;
uniform highp isamplerBuffer CloudFaces;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = vec4(texelFetch(CloudFaces, gl_VertexID).r);
}
)";
} // namespace

TEST(RetargetTextureBufferExtensionTest, OesOnlyDriverGetsTheOesDirective) {
    const String out = RetargetTextureBufferExtension(kBufferTextureShader, Tier::ExtensionOES);
    EXPECT_TRUE(Contains(out, "#extension GL_OES_texture_buffer : require"))
        << "the OES driver's own spelling must reach the directive:\n" << out;
    EXPECT_FALSE(Contains(out, "GL_EXT_texture_buffer"))
        << "the EXT spelling this driver does not advertise must be gone:\n" << out;
    // Only the directive changes; the declaration and the fetch are identical between the two
    // extensions and must not be touched.
    EXPECT_TRUE(Contains(out, "uniform highp isamplerBuffer CloudFaces;"));
    EXPECT_TRUE(Contains(out, "texelFetch(CloudFaces, gl_VertexID)"));
}

TEST(RetargetTextureBufferExtensionTest, ExtDriverKeepsWhatSpirvCrossEmitted) {
    EXPECT_EQ(RetargetTextureBufferExtension(kBufferTextureShader, Tier::ExtensionEXT),
              kBufferTextureShader);
}

// ES 3.2 needs no directive at all, and SPIRV-Cross emits none at ESSL 320 - but a shader
// that arrived with one anyway must not be rewritten to a name the pass was not asked for.
TEST(RetargetTextureBufferExtensionTest, CoreAndUnsupportedTiersAreNoOps) {
    EXPECT_EQ(RetargetTextureBufferExtension(kBufferTextureShader, Tier::CoreEs32),
              kBufferTextureShader);
    EXPECT_EQ(RetargetTextureBufferExtension(kBufferTextureShader, Tier::None),
              kBufferTextureShader);
}

// The name is only the subject of a rewrite where it is the subject of an #extension
// directive. A shader that merely mentions it - in a comment SPIRV-Cross carried through, or
// in an identifier - is not an extension request and must come out byte-identical.
TEST(RetargetTextureBufferExtensionTest, OnlyExtensionDirectivesAreRewritten) {
    const String source = R"(#version 310 es
// GL_EXT_texture_buffer is what this shader would need
precision highp float;
uniform highp float GL_EXT_texture_buffer_lookalike;
layout(location = 0) out highp vec4 mg_FragColor;
void main()
{
    mg_FragColor = vec4(GL_EXT_texture_buffer_lookalike);
}
)";
    EXPECT_EQ(RetargetTextureBufferExtension(source, Tier::ExtensionOES), source);
}

// The dangerous collision, and the one the directive check alone does NOT catch:
// GL_EXT_texture_buffer is a strict prefix of GL_EXT_texture_buffer_object, a different and
// real extension that SPIRV-Cross emits from the same Dim=Buffer branch on its legacy-desktop
// path. Rewriting it would turn a valid request into one for a GL_OES_texture_buffer_object
// that does not exist. Only an identifier-boundary check saves this, so it gets its own test
// with the lookalike on a genuine #extension line.
TEST(RetargetTextureBufferExtensionTest, ALongerExtensionSharingThePrefixIsNotRewritten) {
    const String source = R"(#version 310 es
#extension GL_EXT_texture_buffer_object : require
precision highp float;
void main() {}
)";
    EXPECT_EQ(RetargetTextureBufferExtension(source, Tier::ExtensionOES), source);

    // And when both appear, exactly the exact-match one moves.
    const String mixed = R"(#version 310 es
#extension GL_EXT_texture_buffer_object : require
#extension GL_EXT_texture_buffer : require
precision highp float;
void main() {}
)";
    const String out = RetargetTextureBufferExtension(mixed, Tier::ExtensionOES);
    EXPECT_TRUE(Contains(out, "#extension GL_EXT_texture_buffer_object : require")) << out;
    EXPECT_TRUE(Contains(out, "#extension GL_OES_texture_buffer : require")) << out;
    EXPECT_EQ(CountOf(out, "GL_OES_texture_buffer_object"), 0u) << out;
}

// Whitespace between '#' and the keyword is legal in GLSL, and a shader carrying several
// extension directives must have exactly the one retargeted.
TEST(RetargetTextureBufferExtensionTest, SpacedDirectiveIsRewrittenAndNeighboursAreLeftAlone) {
    const String source = R"(#version 310 es
#  extension GL_EXT_texture_buffer : require
#extension GL_EXT_shader_io_blocks : require
precision highp float;
void main() {}
)";
    const String out = RetargetTextureBufferExtension(source, Tier::ExtensionOES);
    EXPECT_TRUE(Contains(out, "#  extension GL_OES_texture_buffer : require")) << out;
    EXPECT_TRUE(Contains(out, "#extension GL_EXT_shader_io_blocks : require"))
        << "an unrelated extension must survive untouched:\n" << out;
    EXPECT_EQ(CountOf(out, "GL_OES_texture_buffer"), 1u);
}

// Interpolation is only ever consumed at a fragment input, but an ES linker still compares the
// two sides of EVERY stage interface and rejects a program whose producer says `flat` and whose
// consumer does not. SPIRV-Cross prints `flat` on a vertex output and a geometry input of
// integer type and on nothing else, so a program with tessellation in the middle came out
// mismatched at both ends of the tessellator - "output vs_tcs_result interpolation mismatch
// with other stage" on Adreno, and a program that fails to link is a draw that paints nothing.
TEST(ForceFlatIntegerVaryingsTest, TessellationStagesGetTheQualifierOnBothSides) {
    const String tessControl = R"(#version 320 es
layout(vertices = 1) out;
layout(location = 0) in uint vs_tcs_result[];
layout(location = 0) out uint tcs_tes_result[1];
void main() { tcs_tes_result[gl_InvocationID] = vs_tcs_result[gl_InvocationID]; }
)";
    const String control = ForceFlatIntegerVaryings(tessControl, GL_TESS_CONTROL_SHADER);
    EXPECT_TRUE(Contains(control, "layout(location = 0) flat in uint vs_tcs_result[];")) << control;
    EXPECT_TRUE(Contains(control, "layout(location = 0) flat out uint tcs_tes_result[1];")) << control;

    const String tessEval = R"(#version 320 es
layout(isolines, point_mode) in;
layout(location = 0) in uint tcs_tes_result[];
layout(location = 0) out uint tes_gs_result;
void main() { tes_gs_result = tcs_tes_result[0]; }
)";
    const String eval = ForceFlatIntegerVaryings(tessEval, GL_TESS_EVALUATION_SHADER);
    EXPECT_TRUE(Contains(eval, "layout(location = 0) flat in uint tcs_tes_result[];")) << eval;
    EXPECT_TRUE(Contains(eval, "layout(location = 0) flat out uint tes_gs_result;")) << eval;
}

// The two ends the tessellation stages have to meet: what a vertex shader and a geometry shader
// already emitted before this pass learned about tessellation at all. Pinned here so the two
// sides cannot drift apart again.
TEST(ForceFlatIntegerVaryingsTest, TheStagesAroundTessellationAreUnchanged) {
    const String vertex = R"(#version 320 es
layout(location = 0) out uint vs_tcs_result;
void main() { vs_tcs_result = 1u; }
)";
    EXPECT_TRUE(Contains(ForceFlatIntegerVaryings(vertex, GL_VERTEX_SHADER),
                         "layout(location = 0) flat out uint vs_tcs_result;"));

    const String geometry = R"(#version 320 es
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in uint tes_gs_result[1];
layout(location = 0) out uint gs_fs_result;
void main() { gs_fs_result = tes_gs_result[0]; EmitVertex(); }
)";
    const String gs = ForceFlatIntegerVaryings(geometry, GL_GEOMETRY_SHADER);
    EXPECT_TRUE(Contains(gs, "layout(location = 0) flat in uint tes_gs_result[1];")) << gs;
    EXPECT_TRUE(Contains(gs, "layout(location = 0) flat out uint gs_fs_result;")) << gs;
}

// Non-integer interfaces keep whatever interpolation they were given: adding `flat` to a float
// varying would turn a smoothly interpolated value into a per-provoking-vertex constant, which
// is a rendering change, not a linker one.
TEST(ForceFlatIntegerVaryingsTest, FloatVaryingsAreNotTouched) {
    const String tessEval = R"(#version 320 es
layout(isolines, point_mode) in;
layout(location = 1) in vec2 tcs_tes_coord[];
layout(location = 1) out vec2 tes_gs_coord;
void main() { tes_gs_coord = tcs_tes_coord[0]; }
)";
    const String out = ForceFlatIntegerVaryings(tessEval, GL_TESS_EVALUATION_SHADER);
    EXPECT_TRUE(Contains(out, "layout(location = 1) in vec2 tcs_tes_coord[];")) << out;
    EXPECT_TRUE(Contains(out, "layout(location = 1) out vec2 tes_gs_coord;")) << out;
    EXPECT_EQ(CountOf(out, "flat"), 0u) << out;
}

// --- image format qualifier completion ---------------------------------------------------------
//
// GLSL ES requires a format layout qualifier on every image; desktop GLSL lets a writeonly
// declaration omit one. The format is normally written into the SPIR-V before SPIRV-Cross runs
// (BakeImageFormatsPass), but SPIRV-Cross THROWS rather than printing the formats it calls
// desktop-only for ESSL - r8ui among them - so those are completed here, on the emitted text.

// The KHR-GL4x.packed_depth_stencil.stencil_texturing stencil half: `writeonly uniform uimage2D`
// with GL_R8UI bound to its unit.
TEST(BakeImageFormatQualifiersTest, AFormatlessDeclarationGetsTheBoundFormat) {
    const String source = R"(#version 320 es
layout(binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(15u)); }
)";
    const String out = BakeImageFormatQualifiers(source, {{"uni_image", "r8ui"}});
    EXPECT_TRUE(Contains(out, "layout(r8ui, binding = 1) uniform writeonly highp uimage2D uni_image;")) << out;
}

// A declaration with NO layout at all still has to end up with one, or the driver rejects it for
// exactly the reason this pass exists.
TEST(BakeImageFormatQualifiersTest, ADeclarationWithNoLayoutGetsOne) {
    const String source = R"(#version 320 es
uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    const String out = BakeImageFormatQualifiers(source, {{"uni_image", "r16i"}});
    EXPECT_TRUE(Contains(out, "layout(r16i) uniform writeonly highp uimage2D uni_image;")) << out;
}

// A DECLARED format is authoritative and must survive, whatever the map says - the frontend never
// puts a declared image in the map, and the pass must not depend on that being true.
TEST(BakeImageFormatQualifiersTest, ADeclaredFormatIsNeverOverwritten) {
    const String source = R"(#version 320 es
layout(binding = 1, rgba8ui) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    const String out = BakeImageFormatQualifiers(source, {{"uni_image", "r8ui"}});
    EXPECT_EQ(out, source) << out;
}

// Only the named uniform. A second image in the same shader - format-less because the pass
// declined it, or because its unit holds nothing - must be left exactly as it is.
TEST(BakeImageFormatQualifiersTest, OnlyTheNamedUniformIsTouched) {
    const String source = R"(#version 320 es
layout(binding = 0) uniform writeonly highp uimage2D named;
layout(binding = 1) uniform writeonly highp uimage2D other;
void main() { imageStore(named, ivec2(0), uvec4(1u)); imageStore(other, ivec2(0), uvec4(2u)); }
)";
    const String out = BakeImageFormatQualifiers(source, {{"named", "r8ui"}});
    EXPECT_TRUE(Contains(out, "layout(r8ui, binding = 0) uniform writeonly highp uimage2D named;")) << out;
    EXPECT_TRUE(Contains(out, "layout(binding = 1) uniform writeonly highp uimage2D other;")) << out;
}

// The format the pass writes has to survive the two passes that run after it, or nothing was
// gained: the read+write split copies declarations, and the binding strip edits layout qualifiers.
TEST(BakeImageFormatQualifiersTest, TheWrittenFormatSurvivesTheLaterImagePasses) {
    const String source = R"(#version 320 es
layout(binding = 3) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    String out = BakeImageFormatQualifiers(source, {{"uni_image", "r8ui"}});
    out = SplitReadWriteImageUniforms(out);
    out = RemoveLayoutBinding(out);
    EXPECT_TRUE(Contains(out, "r8ui")) << out;
    EXPECT_TRUE(Contains(out, "binding = 3")) << out;
}

TEST(BakeImageFormatQualifiersTest, AnEmptyMapOrAnImagelessShaderIsANoOp) {
    const String withImage = R"(#version 320 es
layout(binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    EXPECT_EQ(BakeImageFormatQualifiers(withImage, {}), withImage);

    const String withoutImage = R"(#version 320 es
layout(location = 0) out highp vec4 mg_FragColor;
void main() { mg_FragColor = vec4(1.0); }
)";
    EXPECT_EQ(BakeImageFormatQualifiers(withoutImage, {{"uni_image", "r8ui"}}), withoutImage);
}

// --- GL_NV_image_formats directive --------------------------------------------------------------

TEST(RequestExtendedImageFormatsTest, TheDirectiveGoesRightAfterTheVersionLine) {
    const String source = R"(#version 320 es
layout(r8ui, binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    const String out = RequestExtendedImageFormats(source, true);
    EXPECT_TRUE(Contains(out, "#version 320 es\n#extension GL_NV_image_formats : require\n")) << out;
}

// Never speculatively: `#extension` naming an extension the driver does not advertise is itself a
// compile error, so the caller's "not needed" answer has to be honoured exactly.
TEST(RequestExtendedImageFormatsTest, NotNeededMeansNotEmitted) {
    const String source = R"(#version 320 es
layout(rgba8ui, binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    EXPECT_EQ(RequestExtendedImageFormats(source, false), source);
}

TEST(RequestExtendedImageFormatsTest, AnAlreadyPresentDirectiveIsNotDuplicated) {
    const String source = R"(#version 320 es
#extension GL_NV_image_formats : require
layout(r8ui, binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    const String out = RequestExtendedImageFormats(source, true);
    EXPECT_EQ(out, source);
    EXPECT_EQ(CountOf(out, "GL_NV_image_formats"), 1u) << out;
}

// --- GL_OES_viewport_array directive -------------------------------------------------------------

// SPIRV-Cross prints gl_ViewportIndex bare and requests nothing for it, and ESSL has no core
// spelling at any version - so without this directive the stage fails to compile, the program is
// marked unusable and every draw made with it silently renders nothing.
TEST(RequestViewportArrayExtensionTest, TheDirectiveGoesRightAfterTheVersionLine) {
    const String source = R"(#version 320 es
layout(points) in;
layout(points, max_vertices = 1) out;
void main() { gl_ViewportIndex = gl_InvocationID; EmitVertex(); }
)";
    const String out = RequestViewportArrayExtension(source, true);
    EXPECT_TRUE(Contains(out, "#version 320 es\n#extension GL_OES_viewport_array : require\n")) << out;
}

// Never speculatively: ARM's compiler hard-errors on an `#extension` naming a string the driver
// does not advertise, so the caller's "not needed" answer has to be honoured exactly. A driver
// without the extension gets the LowerViewportIndexPass fallback instead.
TEST(RequestViewportArrayExtensionTest, NotNeededMeansNotEmitted) {
    const String source = R"(#version 320 es
layout(points) in;
layout(points, max_vertices = 1) out;
void main() { gl_ViewportIndex = gl_InvocationID; EmitVertex(); }
)";
    EXPECT_EQ(RequestViewportArrayExtension(source, false), source);
}

TEST(RequestViewportArrayExtensionTest, AnAlreadyPresentDirectiveIsNotDuplicated) {
    const String source = R"(#version 320 es
#extension GL_OES_viewport_array : require
layout(points) in;
layout(points, max_vertices = 1) out;
void main() { gl_ViewportIndex = gl_InvocationID; EmitVertex(); }
)";
    const String out = RequestViewportArrayExtension(source, true);
    EXPECT_EQ(out, source);
    EXPECT_EQ(CountOf(out, "GL_OES_viewport_array"), 1u) << out;
}

// The two image directives and this one share the insertion point, so a shader that needs both
// must end up with both - and with #version still first.
TEST(RequestViewportArrayExtensionTest, CoexistsWithTheImageFormatDirective) {
    const String source = R"(#version 320 es
layout(r8ui, binding = 1) uniform writeonly highp uimage2D uni_image;
void main() { gl_ViewportIndex = 1; imageStore(uni_image, ivec2(0), uvec4(1u)); }
)";
    const String out = RequestViewportArrayExtension(RequestExtendedImageFormats(source, true), true);
    EXPECT_EQ(out.find("#version 320 es"), 0u) << out;
    EXPECT_TRUE(Contains(out, "#extension GL_NV_image_formats : require\n")) << out;
    EXPECT_TRUE(Contains(out, "#extension GL_OES_viewport_array : require\n")) << out;
}

// --- pass-through tessellation control stage --------------------------------------------------
//
// Desktop GL makes the tessellation control stage optional and takes the levels from
// PATCH_DEFAULT_OUTER_LEVEL / PATCH_DEFAULT_INNER_LEVEL; ES 3.2 has neither, and rejects a
// program that has an evaluation stage without a control stage - with an EMPTY info log. The
// synthesized stage is what stands in, and it has to MIRROR its two neighbours' gl_PerVertex
// rather than pick a shape, because a redeclaration that disagrees with the stage it feeds is an
// ES link error against a program that has nothing else wrong with it.

namespace {
    const FloatVec4 kDefaultOuter(1.0f, 1.0f, 1.0f, 1.0f);
    const FloatVec2 kDefaultInner(1.0f, 1.0f);
} // namespace

TEST(PassthroughTessControlEsslTest, DeclaresThePatchSizeAndWritesEveryTessLevel) {
    const String out = BuildPassthroughTessControlEssl(320, 4, "", "", kDefaultOuter, kDefaultInner);
    EXPECT_EQ(out.find("#version 320 es"), 0u) << out;
    EXPECT_TRUE(Contains(out, "layout(vertices = 4) out;")) << out;
    EXPECT_TRUE(Contains(out,
                         "gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;"))
        << out;
    // All six, unconditionally: writing a level the evaluation stage's domain does not use is
    // legal and ignored, and it saves the generator from having to know the domain.
    for (const char* level : {"gl_TessLevelOuter[0]", "gl_TessLevelOuter[1]", "gl_TessLevelOuter[2]",
                              "gl_TessLevelOuter[3]", "gl_TessLevelInner[0]", "gl_TessLevelInner[1]"}) {
        EXPECT_TRUE(Contains(out, String(level) + " = 1.000000;")) << level << "\n" << out;
    }
    // Nothing redeclared when the neighbours redeclared nothing - the driver's own built-in
    // gl_in/gl_out is then what both sides agree on, and redeclaring is what would break it.
    EXPECT_FALSE(Contains(out, "gl_PerVertex")) << out;
}

// glPatchParameterfv's state is compiled INTO this stage: ES has no PATCH_DEFAULT_*_LEVEL and no
// entry point to forward it to, so a generator that ignored these arguments would tessellate every
// control-stage-less program at level 1 whatever the application asked for.
TEST(PassthroughTessControlEsslTest, BakesTheDefaultTessLevelsIn) {
    const String out = BuildPassthroughTessControlEssl(320, 4, "", "", FloatVec4(2.0f, 3.0f, 4.0f, 5.0f),
                                                       FloatVec2(6.5f, 7.25f));
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[0] = 2.000000;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[1] = 3.000000;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[2] = 4.000000;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[3] = 5.000000;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelInner[0] = 6.500000;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelInner[1] = 7.250000;")) << out;
}

// Every level literal carries a decimal point even when the value is integral: ESSL reads
// `gl_TessLevelOuter[0] = 1;` as an int assigned to a float and refuses to compile the stage,
// which would take the whole program down with it.
TEST(PassthroughTessControlEsslTest, SpellsIntegralLevelsAsFloatLiterals) {
    const String out = BuildPassthroughTessControlEssl(320, 4, "", "", FloatVec4(2.0f, 2.0f, 2.0f, 2.0f),
                                                       FloatVec2(2.0f, 2.0f));
    EXPECT_FALSE(Contains(out, "= 2;")) << out;
}

// glPatchParameterfv accepts any float, NaN and infinity included. Emitting "nan" would make the
// synthesized stage fail to compile; 0.0 is the honest stand-in, because a level that is not a
// positive number discards the patch in GL too.
TEST(PassthroughTessControlEsslTest, NonFiniteLevelsBecomeZero) {
    const Float notANumber = std::numeric_limits<Float>::quiet_NaN();
    const Float infinity = std::numeric_limits<Float>::infinity();
    const String out = BuildPassthroughTessControlEssl(320, 4, "", "",
                                                       FloatVec4(notANumber, infinity, 1.0f, 1.0f),
                                                       FloatVec2(notANumber, 1.0f));
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[0] = 0.0;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelOuter[1] = 0.0;")) << out;
    EXPECT_TRUE(Contains(out, "gl_TessLevelInner[0] = 0.0;")) << out;
    EXPECT_FALSE(Contains(out, "nan")) << out;
    EXPECT_FALSE(Contains(out, "inf")) << out;
}

// ES 3.1 reaches tessellation only through the extension; the caller has already established
// that the driver runs the evaluation stage at all, so the only question is the spelling.
TEST(PassthroughTessControlEsslTest, RequestsTheExtensionBelowEs32) {
    const String out = BuildPassthroughTessControlEssl(310, 3, "", "", kDefaultOuter, kDefaultInner);
    EXPECT_EQ(out.find("#version 310 es"), 0u) << out;
    EXPECT_TRUE(Contains(out, "#extension GL_EXT_tessellation_shader : require")) << out;
}

TEST(PassthroughTessControlEsslTest, MirrorsTheNeighboursPerVertexBlocks) {
    const String inMembers = " highp vec4 gl_Position; highp float gl_PointSize; ";
    const String outMembers = " highp vec4 gl_Position; ";
    const String out = BuildPassthroughTessControlEssl(320, 4, inMembers, outMembers, kDefaultOuter, kDefaultInner);
    EXPECT_TRUE(Contains(out, "in gl_PerVertex {" + inMembers + "} gl_in[gl_MaxPatchVertices];")) << out;
    EXPECT_TRUE(Contains(out, "out gl_PerVertex {" + outMembers + "} gl_out[];")) << out;
}

TEST(ExtractPerVertexBlockMembersTest, ReadsEitherDirectionAndOnlyThatDirection) {
    const String essl = R"(#version 320 es
in gl_PerVertex { highp vec4 gl_Position; } gl_in[gl_MaxPatchVertices];
out gl_PerVertex { highp vec4 gl_Position; highp float gl_PointSize; } gl_out[];
void main() {}
)";
    const auto inMembers = ExtractPerVertexBlockMembers(essl, true);
    ASSERT_TRUE(inMembers.has_value()) << essl;
    EXPECT_TRUE(Contains(*inMembers, "gl_Position")) << *inMembers;
    EXPECT_FALSE(Contains(*inMembers, "gl_PointSize"))
        << "the `in` block must not pick up the `out` block's members: " << *inMembers;

    const auto outMembers = ExtractPerVertexBlockMembers(essl, false);
    ASSERT_TRUE(outMembers.has_value()) << essl;
    EXPECT_TRUE(Contains(*outMembers, "gl_PointSize")) << *outMembers;
}

// A shader that does not redeclare the block must report nothing, so the generator leaves the
// driver's built-in declaration alone rather than inventing one.
TEST(ExtractPerVertexBlockMembersTest, ReportsNothingWhenTheBlockIsNotRedeclared) {
    const String essl = R"(#version 320 es
layout(quads) in;
void main() { gl_Position = gl_in[0].gl_Position; }
)";
    EXPECT_FALSE(ExtractPerVertexBlockMembers(essl, true).has_value()) << essl;
    EXPECT_FALSE(ExtractPerVertexBlockMembers(essl, false).has_value()) << essl;
}

// "min" ends in "in" and "layout" ends in "out": the direction keyword has to be a whole token
// immediately before the block name, or an unrelated identifier would be read as a redeclaration.
TEST(ExtractPerVertexBlockMembersTest, DoesNotMatchAnIdentifierEndingInTheKeyword) {
    const String essl = R"(#version 320 es
struct fin gl_PerVertex { highp vec4 gl_Position; };
void main() {}
)";
    EXPECT_FALSE(ExtractPerVertexBlockMembers(essl, true).has_value()) << essl;
}
