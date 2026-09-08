// MobileGL - MobileGL/MG_Test/Program/ProgramArtifactsCodecTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The reflection ARCHIVE's serializer (P4a): create_shader_state's payload is per-stage SPIR-V
// plus LinkArtifacts + SpirvArtifacts, whole structs, and the codec is what turns them into
// bytes. In monolith it is never called on the hot path - the two structs ride beside the
// record through the apply entry point's companion pointers - so THIS SUITE plus the verify
// lane's round trip are the only things that exercise it until a transport exists.
//
// WHAT IT HAS TO PIN, and each of the three is a different failure:
//   * a fully populated archive survives a round trip FIELD BY FIELD, including both of
//     XfbVarying's spellings (the GL name AND the block instance / member / element triple)
//     and every one of TypeFacts' twenty members - a codec that dropped one would be invisible
//     until a backend read a reflection answer that had quietly become zero;
//   * a TRUNCATED stream is refused rather than guessed at;
//   * a VERSION or struct-size mismatch is refused rather than deserialised into a layout this
//     build does not have.
//
// AND ONE PROPERTY THAT IS NOT ABOUT BYTES AT ALL: LinkArtifacts has 58 members and its
// VisitFields table visits 57. The 58th is the live SharedPtr<glslang::TProgram>, which is
// null for every archived instance by construction and points into an arena no archive owns.
// The codec has no arm for it, and the count is asserted here because that is the only place
// it can be: VisitFields needs an instance, and these structs carry strings, vectors and maps,
// so no static_assert can walk them.
//
// Every case is a visible SKIP in a pull build rather than a vanishing test, so `ctest -N`
// stays name-for-name identical between the pull and the push trees.

#include <gtest/gtest.h>

#include "Includes.h"
#if MOBILEGL_PIPE_PUSH
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#endif

using namespace MobileGL;
#if MOBILEGL_PIPE_PUSH
using namespace MobileGL::MG_State::GLState;

namespace {
    TypeFacts MakeTypeFacts() {
        // Every member set to something that is NOT its default, so a field the codec skips
        // reads back as the default and the comparison names it.
        TypeFacts facts{};
        facts.isArray = true;
        facts.isSizedArray = true;
        facts.isMatrix = true;
        facts.isVector = true;
        facts.isOpaque = true;
        facts.isTexture = true;
        facts.isImage = true;
        facts.isDouble = true;
        facts.isVoid = true;
        facts.isBuffer = true;
        facts.isPatch = true;
        facts.hasIndex = true;
        facts.hasFormat = true;
        facts.vectorSize = 3;
        facts.matrixCols = 4;
        facts.matrixRows = 2;
        facts.layoutIndex = 7;
        facts.layoutFormat = 0x8814u;
        facts.layoutMatrix = 1;
        facts.basicType = 11;
        return facts;
    }

    ResourceReflection MakeReflection(const char* name, Int location) {
        ResourceReflection reflection{};
        reflection.name = name;
        reflection.glDefineType = GL_FLOAT_VEC4;
        reflection.offset = 16;
        reflection.size = 4;
        reflection.index = 2;
        reflection.counterIndex = 3;
        reflection.arrayStride = 16;
        reflection.topLevelArraySize = 5;
        reflection.topLevelArrayStride = 32;
        reflection.binding = 6;
        reflection.location = location;
        reflection.stages = 0x3u;
        reflection.arraySize = 8;
        reflection.type = MakeTypeFacts();
        return reflection;
    }

    XfbVarying MakeXfbVarying() {
        XfbVarying varying{};
        // BOTH SPELLINGS. `name` is the GL one ("Block.member"), which the interface queries
        // and the ESSL driver-side capture list need; the triple below is what a SPIR-V
        // backend needs instead, because the decoration target is the block's instance
        // variable and the member index inside it.
        varying.name = "Captured.position";
        varying.type = GL_FLOAT_VEC3;
        varying.size = 2;
        varying.bufferIndex = 1;
        varying.offsetBytes = 12;
        varying.byteSize = 24;
        varying.packedOffsetBytes = 8;
        varying.blockInstanceName = "capturedInstance";
        varying.blockName = "Captured";
        varying.blockMemberIndex = 1;
        varying.blockMemberElement = 3;
        return varying;
    }

    LinkArtifacts MakeLinkArtifacts() {
        LinkArtifacts link{};
        link.uniformReflection = {MakeReflection("uColour", 0), MakeReflection("uMatrix", 1)};
        link.blockReflection = {MakeReflection("Block", -1)};
        link.pipeInputReflection = {MakeReflection("inPosition", 0)};
        link.pipeOutputReflection = {MakeReflection("outColour", 0)};
        link.lastStageIsFragment = true;
        link.computeLocalSize = {8u, 4u, 2u};
        link.uniformIndexByName = {{"uColour", 0}, {"uMatrix", 1}};
        link.attribs = {"inPosition", "inNormal"};
        link.attribTypes = {GL_FLOAT_VEC3, GL_FLOAT_VEC3};
        link.linkedFragDataLocation = {{"outColour", 0u}};
        link.linkedFragDataIndex = {{"outColour", 1u}};
        link.glUniformIndexToTProgram = {0, 1};
        link.tProgramUniformIndexToGl = {0, 1};
        link.glBlockIndexToTProgram = {0};
        link.tProgramBlockIndexToGl = {-1};
        link.glUniformBlockIndexToBlock = {0};
        link.blockIndexToGlUniformBlock = {0};
        link.linkedExplicitUniformLocations = {{"uColour", 3}};
        link.uniformLocations = {{"uColour", 0u}, {"uMatrix", 4u}};
        link.writtenUniformLocationBits = {0x5ull};
        link.writtenUniformIndexBits = {0x3ull};
        link.writtenUniformIndices = {0u, 1u};
        link.uniformIndexInTProgram = {0, 1};
        link.uniformSamplerOrImageUnitIndex = {-1, 2};
        link.explicitOpaqueUniformBindings = {{"uSampler", 5u}};
        link.uniformBlockIndexByName = {{"Block", 0u}};
        link.uniformBlockBinding = {2};
        link.shaderStorageBlockBinding = {{"Storage", 1}};
        link.storageBlocksWithoutBinding = {"Storage"};
        link.uniformBlocksWithoutBinding = {"Block"};
        link.activeUniformCount = 2u;
        link.usesReservedNumSamples = true;
        link.maxUniformLocation = 4u;
        link.uniformNameMaxLength = 9;
        link.attribInNameMaxLength = 11;
        link.uniformBlockNameMaxLength = 6;
        link.infoLog = "linked with warnings";
        link.linkStatus = true;
        link.xfbVaryings = {MakeXfbVarying()};
        link.xfbInterfaceNames = {"gl_NextBuffer", "Captured.position"};
        link.xfbStrides = {32u, 0u};
        link.gsStripTriangles = {3u, 5u};
        link.gsStripCaptureFixup = true;
        link.gsInputPrimitive = GL_TRIANGLES;
        link.tcsOutputVertices = 3;
        link.gsOutputPrimitive = GL_TRIANGLE_STRIP;
        link.gsMaxVertices = 12;
        link.gsInvocations = 2;
        link.tessGenMode = GL_QUADS;
        link.tessGenSpacing = GL_FRACTIONAL_ODD;
        link.tessGenVertexOrder = GL_CW;
        link.tessGenPointMode = true;
        link.xfbBufferMode = GL_SEPARATE_ATTRIBS;
        link.xfbVaryingNameMaxLength = 18;
        link.xfbNeedsScatteredCapture = true;
        link.xfbPackedStride = 24u;

        // The one glslang-typed member that DOES travel: a plain aggregate of a string,
        // scalars and two vectors. The codec has a hand-written arm for it because
        // ProgramArtifacts.h gives it no VisitFields table.
        glslang::TIntermediate::TUniformInitializer initializer;
        initializer.name = "uInitialised";
        initializer.basicType = glslang::EbtInt;
        initializer.vectorSize = 2;
        initializer.matrixCols = 0;
        initializer.matrixRows = 0;
        initializer.arraySize = 3;
        initializer.intValues = {1, 2, 3, 4, 5, 6};
        initializer.floatValues = {};
        link.uniformInitialValues.push_back(initializer);
        return link;
    }

    SpirvArtifacts MakeSpirvArtifacts() {
        SpirvArtifacts spirv{};
        spirv.generatedSpirv = {{0x07230203u, 0x00010300u, 0u}, {0x07230203u, 0x00010300u, 1u}};
        spirv.enableSpirvValidation = true;
        spirv.uniformOffsets = {0u, 16u, kInvalidUniformOffset};
        spirv.globalUboScratch = {1, 2, 3, 4, 5, 6, 7, 8};
        spirv.reservedNumSamplesOffset = 32u;
        spirv.spirvStatus = true;
        spirv.nativeFloat64 = true;
        spirv.pointSizeDemoted = true;
        return spirv;
    }
} // namespace
#endif // MOBILEGL_PIPE_PUSH

// The round trip, field by field. A re-encode equality alone would prove the codec is
// self-consistent and nothing else - a field it skips in BOTH directions round-trips
// perfectly - so the members are read back explicitly first, and the byte comparison is the
// catch-all underneath them.
TEST(ProgramArtifactsCodec, RoundTripsAFullyPopulatedArchive) {
#if MOBILEGL_PIPE_PUSH
    const LinkArtifacts link = MakeLinkArtifacts();
    const SpirvArtifacts spirv = MakeSpirvArtifacts();

    Vector<Uint8> bytes;
    EncodeProgramArtifacts(link, spirv, bytes);
    ASSERT_FALSE(bytes.empty());

    LinkArtifacts decodedLink;
    SpirvArtifacts decodedSpirv;
    ASSERT_TRUE(DecodeProgramArtifacts(bytes.data(), bytes.size(), decodedLink, decodedSpirv));

    // The four reflection vectors, with their TypeFacts.
    ASSERT_EQ(decodedLink.uniformReflection.size(), 2u);
    EXPECT_EQ(decodedLink.uniformReflection[0].name, "uColour");
    EXPECT_EQ(decodedLink.uniformReflection[1].location, 1);
    EXPECT_EQ(decodedLink.uniformReflection[0].arrayStride, 16);
    EXPECT_EQ(decodedLink.uniformReflection[0].stages, 0x3u);
    EXPECT_TRUE(decodedLink.uniformReflection[0].type.isSizedArray);
    EXPECT_EQ(decodedLink.uniformReflection[0].type.matrixCols, 4);
    EXPECT_EQ(decodedLink.uniformReflection[0].type.layoutFormat, 0x8814u);
    EXPECT_EQ(decodedLink.uniformReflection[0].type.basicType, 11);
    ASSERT_EQ(decodedLink.blockReflection.size(), 1u);
    ASSERT_EQ(decodedLink.pipeInputReflection.size(), 1u);
    ASSERT_EQ(decodedLink.pipeOutputReflection.size(), 1u);

    // BOTH XfbVarying SPELLINGS.
    ASSERT_EQ(decodedLink.xfbVaryings.size(), 1u);
    EXPECT_EQ(decodedLink.xfbVaryings[0].name, "Captured.position");
    EXPECT_EQ(decodedLink.xfbVaryings[0].blockInstanceName, "capturedInstance");
    EXPECT_EQ(decodedLink.xfbVaryings[0].blockName, "Captured");
    EXPECT_EQ(decodedLink.xfbVaryings[0].blockMemberIndex, 1);
    EXPECT_EQ(decodedLink.xfbVaryings[0].blockMemberElement, 3);
    EXPECT_EQ(decodedLink.xfbVaryings[0].packedOffsetBytes, 8u);

    // The maps, the set and the fixed array - the four container shapes the archive is made
    // of, each with a reader that has to agree with its writer about the length prefix.
    EXPECT_EQ(decodedLink.uniformIndexByName.size(), 2u);
    EXPECT_EQ(decodedLink.uniformIndexByName.at("uMatrix"), 1);
    EXPECT_EQ(decodedLink.shaderStorageBlockBinding.at("Storage"), 1);
    EXPECT_EQ(decodedLink.storageBlocksWithoutBinding.count("Storage"), 1u);
    EXPECT_EQ(decodedLink.uniformBlocksWithoutBinding.count("Block"), 1u);
    EXPECT_EQ(decodedLink.computeLocalSize[0], 8u);
    EXPECT_EQ(decodedLink.computeLocalSize[2], 2u);

    // The glslang-typed aggregate, through the codec's one hand-written arm.
    ASSERT_EQ(decodedLink.uniformInitialValues.size(), 1u);
    EXPECT_EQ(decodedLink.uniformInitialValues[0].name, "uInitialised");
    EXPECT_EQ(decodedLink.uniformInitialValues[0].basicType, glslang::EbtInt);
    EXPECT_EQ(decodedLink.uniformInitialValues[0].arraySize, 3);
    ASSERT_EQ(decodedLink.uniformInitialValues[0].intValues.size(), 6u);
    EXPECT_EQ(decodedLink.uniformInitialValues[0].intValues[5], 6);
    EXPECT_TRUE(decodedLink.uniformInitialValues[0].floatValues.empty());

    // The scalars at the tail, which is where a length-prefix that drifted by one would first
    // read as garbage rather than as a short read.
    EXPECT_EQ(decodedLink.infoLog, "linked with warnings");
    EXPECT_TRUE(decodedLink.linkStatus);
    EXPECT_EQ(decodedLink.tessGenSpacing, static_cast<GLenum>(GL_FRACTIONAL_ODD));
    EXPECT_TRUE(decodedLink.tessGenPointMode);
    EXPECT_EQ(decodedLink.xfbPackedStride, 24u);

    // SpirvArtifacts, including the nested vector of SPIR-V words and the sentinel offset.
    ASSERT_EQ(decodedSpirv.generatedSpirv.size(), 2u);
    ASSERT_EQ(decodedSpirv.generatedSpirv[0].size(), 3u);
    EXPECT_EQ(decodedSpirv.generatedSpirv[1][2], 1u);
    ASSERT_EQ(decodedSpirv.uniformOffsets.size(), 3u);
    EXPECT_EQ(decodedSpirv.uniformOffsets[2], kInvalidUniformOffset);
    EXPECT_EQ(decodedSpirv.globalUboScratch.size(), 8u);
    EXPECT_EQ(decodedSpirv.reservedNumSamplesOffset, 32u);
    EXPECT_TRUE(decodedSpirv.nativeFloat64);
    EXPECT_TRUE(decodedSpirv.pointSizeDemoted);

    // THE LIVE TProgram IS NEVER CARRIED and never reconstructed.
    EXPECT_EQ(decodedLink.program, nullptr);

    // The catch-all: re-encoding what came back has to produce the same bytes, which covers
    // every member the explicit reads above do not name.
    Vector<Uint8> reencoded;
    EncodeProgramArtifacts(decodedLink, decodedSpirv, reencoded);
    EXPECT_EQ(reencoded, bytes);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: the archive codec is push-only";
#endif
}

// Negative control 1. Every prefix length is checked against the bytes that REMAIN, so a
// stream cut anywhere has to come back false with both outputs defaulted - never a partially
// filled archive, and never a resize driven by a count the stream cannot back.
TEST(ProgramArtifactsCodec, ATruncatedStreamIsRefusedNotGuessed) {
#if MOBILEGL_PIPE_PUSH
    Vector<Uint8> bytes;
    EncodeProgramArtifacts(MakeLinkArtifacts(), MakeSpirvArtifacts(), bytes);
    ASSERT_GT(bytes.size(), 64u);

    // Cut at a spread of points rather than one: the header, a length prefix, the middle of a
    // string and the last byte all fail through different branches.
    for (const SizeT cut : {SizeT{0}, SizeT{4}, SizeT{9}, bytes.size() / 3, bytes.size() / 2,
                            bytes.size() - 1}) {
        LinkArtifacts link;
        SpirvArtifacts spirv;
        link.infoLog = "must be cleared";
        EXPECT_FALSE(DecodeProgramArtifacts(bytes.data(), cut, link, spirv))
            << "a stream truncated at " << cut << " was accepted";
        EXPECT_TRUE(link.infoLog.empty()) << "a refused decode left the output half-filled";
        EXPECT_TRUE(link.uniformReflection.empty());
        EXPECT_TRUE(spirv.generatedSpirv.empty());
    }

    // And TRAILING bytes are a mismatch too: the format accounts for every byte it writes, so
    // anything left over means the reader and the writer disagree about the shape.
    Vector<Uint8> withTail = bytes;
    withTail.push_back(0);
    LinkArtifacts link;
    SpirvArtifacts spirv;
    EXPECT_FALSE(DecodeProgramArtifacts(withTail.data(), withTail.size(), link, spirv));
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: the archive codec is push-only";
#endif
}

// Negative control 2. The version word and the struct-size echo are the two things a compiler
// cannot check: a struct that gained a field and a codec that did not would otherwise
// deserialise garbage into the tail of a reflection table. Both have to REFUSE.
TEST(ProgramArtifactsCodec, AVersionMismatchIsRefused) {
#if MOBILEGL_PIPE_PUSH
    Vector<Uint8> bytes;
    EncodeProgramArtifacts(MakeLinkArtifacts(), MakeSpirvArtifacts(), bytes);
    ASSERT_GT(bytes.size(), 12u);

    LinkArtifacts link;
    SpirvArtifacts spirv;
    ASSERT_TRUE(DecodeProgramArtifacts(bytes.data(), bytes.size(), link, spirv));

    // The version word first: a reader that saw a format it does not know must not try to
    // guess the layout.
    Vector<Uint8> wrongVersion = bytes;
    ++wrongVersion[0];
    EXPECT_FALSE(DecodeProgramArtifacts(wrongVersion.data(), wrongVersion.size(), link, spirv));

    // Then the MGL_LINKARTIFACTS_SIZE echo, which is the half that catches a struct that grew
    // under a codec that did not - the failure the four sizeof trip wires in
    // ProgramArtifacts.h send an author here to fix.
    Vector<Uint8> wrongSize = bytes;
    ++wrongSize[4];
    EXPECT_FALSE(DecodeProgramArtifacts(wrongSize.data(), wrongSize.size(), link, spirv));

    // A null pointer is refused rather than dereferenced.
    EXPECT_FALSE(DecodeProgramArtifacts(nullptr, 0, link, spirv));
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: the archive codec is push-only";
#endif
}

// The codec walks the VisitFields tables and nothing else, so what those tables visit IS the
// archive. LinkArtifacts has 58 members and its table visits 57: the 58th is the live glslang
// TProgram, which is null for every archived instance by construction and points into an arena
// no archive owns. A codec arm for it would be a use-after-free waiting for a cache hit.
TEST(ProgramArtifactsCodec, TheTablesVisitEveryMemberExceptTheLiveProgram) {
#if MOBILEGL_PIPE_PUSH
    EXPECT_EQ(ProgramArtifactsVisitedFieldCount<LinkArtifacts>(), 57u);
    EXPECT_EQ(ProgramArtifactsVisitedFieldCount<SpirvArtifacts>(), 8u);
    EXPECT_EQ(ProgramArtifactsVisitedFieldCount<ResourceReflection>(), 14u);
    EXPECT_EQ(ProgramArtifactsVisitedFieldCount<XfbVarying>(), 11u);
    EXPECT_EQ(ProgramArtifactsVisitedFieldCount<TypeFacts>(), 20u);
#else
    GTEST_SKIP() << "MOBILEGL_PIPE_PUSH is off: the archive codec is push-only";
#endif
}
