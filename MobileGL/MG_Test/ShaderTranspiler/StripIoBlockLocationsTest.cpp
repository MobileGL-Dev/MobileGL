// MobileGL - MobileGL/MG_Test/ShaderTranspiler/StripIoBlockLocationsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
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

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    // How many times `needle` occurs in `haystack`.
    SizeT CountOf(const String& haystack, const String& needle) {
        SizeT count = 0;
        for (SizeT at = haystack.find(needle); at != String::npos; at = haystack.find(needle, at + 1)) {
            ++count;
        }
        return count;
    }

    // The tessellation evaluation stage of
    // KHR-GLxx.shading_language_420pack.length_of_vector_and_matrix_*, reduced to what this
    // pass is about: one block consumed, one block produced, a plain varying in each
    // direction, and NO location written anywhere in the source. Every location in the
    // emitted ESSL is invented by glslang's cross-stage IO resolver.
    const char* kTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;

in  vec4 tcs_tes_result[];
out vec4 tes_gs_result;

in TCSOutputBlock {
    vec4 tcs_tes_variable;
} input_block[];
out TESOutputBlock {
    vec4 tes_gs_variable;
} output_block;

void main()
{
    tes_gs_result = tcs_tes_result[0];
    output_block.tes_gs_variable = input_block[0].tcs_tes_variable;
}
)";

    // The OTHER place a block's location can live. When the application locates the MEMBERS
    // rather than the block, glslang emits one OpMemberDecorate Location per member and
    // NOTHING on the variable - and SPIRV-Cross then suppresses the block-level qualifier and
    // prints the member ones instead. A strip that only looked at the variable would find
    // nothing to remove here, report "unchanged", and leave the emitted ESSL carrying exactly
    // the located block the driver drops the payload for.
    const char* kMemberLocatedTessEvalSource = R"(#version 450 core
layout(isolines, point_mode) in;

in TCSOutputBlock {
    layout(location = 4) vec4 tcs_tes_variable;
    layout(location = 5) vec4 tcs_tes_second;
} input_block[];
out TESOutputBlock {
    layout(location = 6) vec4 tes_gs_variable;
    layout(location = 7) vec4 tes_gs_second;
} output_block;

void main()
{
    output_block.tes_gs_variable = input_block[0].tcs_tes_variable;
    output_block.tes_gs_second = input_block[0].tcs_tes_second;
}
)";

    // A stage with no interface block at all: the pass must leave its located varyings alone
    // and report that it changed nothing, so the caller declines the re-serialised module.
    const char* kNoBlockTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;

in  vec4 tcs_tes_result[];
out vec4 tes_gs_result;

void main()
{
    tes_gs_result = tcs_tes_result[0];
}
)";
} // namespace

// NOTE ON spirv-val, because its absence here is deliberate and every sibling pass test
// asserts the opposite. Vulkan SPIR-V REQUIRES a Location decoration on every user-defined
// Input/Output variable ([VUID-StandaloneSpirv-Location-04915]), so a module whose interface
// blocks have had theirs removed is INVALID Vulkan SPIR-V by construction - that is what the
// pass was asked to produce. It never reaches a driver as SPIR-V: DirectGLES runs this last
// in its chain and hands the result straight to SPIRV-Cross, which needs no location to print
// a block. What the cases below assert instead is the thing that actually matters - that
// SPIRV-Cross still emits a complete, matchable interface from it.
class StripIoBlockLocationsTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
};

TEST_F(StripIoBlockLocationsTest, DropsTheQualifierFromBothBlocksAndLeavesVaryingsAlone) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kTessEvalSource);
    ASSERT_FALSE(input.empty());

    // The defect this exists for, pinned before the repair: SPIRV-Cross really does print a
    // location on the blocks, and on this driver that is what loses their payload.
    const String before = Transpile(input);
    EXPECT_NE(before.find(") in TCSOutputBlock"), String::npos) << before;
    EXPECT_NE(before.find(") out TESOutputBlock"), String::npos) << before;

    bool strippedAny = false;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::StripIoBlockLocationsForEssl(input, true, true, strippedAny, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(strippedAny);

    const String after = Transpile(output);
    // The blocks come out bare...
    EXPECT_NE(after.find("in TCSOutputBlock"), String::npos) << after;
    EXPECT_NE(after.find("out TESOutputBlock"), String::npos) << after;
    EXPECT_EQ(after.find(") in TCSOutputBlock"), String::npos)
        << "the consumed block still carries a layout qualifier:\n"
        << after;
    EXPECT_EQ(after.find(") out TESOutputBlock"), String::npos)
        << "the produced block still carries a layout qualifier:\n"
        << after;
    // ...and everything ES matches them by is untouched, which is what makes the unlocated
    // interface still find its other end.
    EXPECT_NE(after.find("input_block"), String::npos) << after;
    EXPECT_NE(after.find("output_block"), String::npos) << after;
    EXPECT_NE(after.find("tcs_tes_variable"), String::npos) << after;
    EXPECT_NE(after.find("tes_gs_variable"), String::npos) << after;
    // The PLAIN varyings keep their locations. They work on the affected driver, and a
    // fragment stage's inputs and a vertex stage's attributes are matched by them.
    EXPECT_NE(after.find("in vec4 tcs_tes_result"), String::npos) << after;
    EXPECT_NE(after.find("out vec4 tes_gs_result"), String::npos) << after;
    EXPECT_EQ(CountOf(after, "layout(location"), 2u)
        << "exactly the two plain varyings should still be located:\n"
        << after;
}

TEST_F(StripIoBlockLocationsTest, StripsOnlyTheDirectionTheCallerArmed) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kTessEvalSource);
    ASSERT_FALSE(input.empty());

    // A separate-shader-objects program that ENDS at this stage: the block it produces is
    // matched, in another program that never saw this decision, by the location alone. Only
    // the consumed side may lose its qualifier.
    bool strippedAny = false;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::StripIoBlockLocationsForEssl(input, true, false, strippedAny, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(strippedAny);

    const String after = Transpile(output);
    EXPECT_EQ(after.find(") in TCSOutputBlock"), String::npos) << after;
    EXPECT_NE(after.find(") out TESOutputBlock"), String::npos)
        << "the produced block's location was dropped even though its consumer is elsewhere:\n"
        << after;

    // And the mirror image, for a program that BEGINS at this stage.
    bool strippedOutputOnly = false;
    Vector<Uint32> outputOnly;
    ASSERT_TRUE(
        ShaderCompiler::StripIoBlockLocationsForEssl(input, false, true, strippedOutputOnly, outputOnly, true));
    ASSERT_FALSE(outputOnly.empty());
    EXPECT_TRUE(strippedOutputOnly);
    const String afterOutputOnly = Transpile(outputOnly);
    EXPECT_NE(afterOutputOnly.find(") in TCSOutputBlock"), String::npos) << afterOutputOnly;
    EXPECT_EQ(afterOutputOnly.find(") out TESOutputBlock"), String::npos) << afterOutputOnly;
}

// The regression guard for the shape a variable-only strip walks straight past.
TEST_F(StripIoBlockLocationsTest, DropsLocationsTheApplicationPutOnTheBlockMembers) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kMemberLocatedTessEvalSource);
    ASSERT_FALSE(input.empty());

    // The defect, pinned first: SPIRV-Cross prints the member locations, and there is no
    // block-level qualifier for a variable-level strip to find.
    const String before = Transpile(input);
    EXPECT_NE(before.find("layout(location = 4)"), String::npos) << before;
    EXPECT_NE(before.find("layout(location = 6)"), String::npos) << before;

    bool strippedAny = false;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::StripIoBlockLocationsForEssl(input, true, true, strippedAny, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(strippedAny) << "the member-located block was passed by, and reporting no change "
                                "makes the caller decline the module and say nothing about it";

    const String after = Transpile(output);
    EXPECT_EQ(CountOf(after, "layout(location"), 0u)
        << "a member location survived, so the emitted block is still the shape the driver "
           "drops the payload for:\n"
        << after;
    // The interface still has to be matchable: same blocks, same members, same order.
    EXPECT_NE(after.find("TCSOutputBlock"), String::npos) << after;
    EXPECT_NE(after.find("TESOutputBlock"), String::npos) << after;
    EXPECT_LT(after.find("tcs_tes_variable"), after.find("tcs_tes_second")) << after;
    EXPECT_LT(after.find("tes_gs_variable"), after.find("tes_gs_second")) << after;
}

// ...and the same shape with only ONE direction armed. The member decorations belong to the
// TYPE, so the unarmed block's must survive - it is matched, in another program, by exactly
// those numbers.
TEST_F(StripIoBlockLocationsTest, KeepsMemberLocationsOnTheDirectionTheCallerDidNotArm) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kMemberLocatedTessEvalSource);
    ASSERT_FALSE(input.empty());

    bool strippedAny = false;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::StripIoBlockLocationsForEssl(input, true, false, strippedAny, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_TRUE(strippedAny);

    const String after = Transpile(output);
    EXPECT_EQ(after.find("layout(location = 4)"), String::npos) << after;
    EXPECT_EQ(after.find("layout(location = 5)"), String::npos) << after;
    EXPECT_NE(after.find("layout(location = 6)"), String::npos)
        << "the produced block lost its member locations even though its consumer is elsewhere:\n"
        << after;
    EXPECT_NE(after.find("layout(location = 7)"), String::npos) << after;
}

TEST_F(StripIoBlockLocationsTest, ReportsNoChangeForAStageWithoutInterfaceBlocks) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kNoBlockTessEvalSource);
    ASSERT_FALSE(input.empty());
    const String before = Transpile(input);

    bool strippedAny = true; // deliberately wrong going in; the pass must clear it
    Vector<Uint32> output;
    ShaderCompiler::StripIoBlockLocationsForEssl(input, true, true, strippedAny, output, true);
    EXPECT_FALSE(strippedAny) << "a stage with no interface block must report nothing stripped, or "
                                 "the caller adopts a re-serialised module for nothing";
    // gl_PerVertex is an Input AND an Output block in this stage and must not be touched; the
    // located plain varyings must not be either. Either way the emitted ESSL is unchanged.
    if (!output.empty()) {
        EXPECT_EQ(Transpile(output), before);
    }
}

TEST_F(StripIoBlockLocationsTest, DeclinesWhenNeitherDirectionIsArmed) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kTessEvalSource);
    ASSERT_FALSE(input.empty());

    bool strippedAny = true;
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::StripIoBlockLocationsForEssl(input, false, false, strippedAny, output, true));
    EXPECT_FALSE(strippedAny);
    EXPECT_TRUE(output.empty()) << "an unarmed call must not even re-serialise the module";
}
