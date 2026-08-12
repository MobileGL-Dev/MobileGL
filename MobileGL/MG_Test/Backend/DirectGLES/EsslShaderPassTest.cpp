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
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::ForceFlatIntegerVaryings;
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
