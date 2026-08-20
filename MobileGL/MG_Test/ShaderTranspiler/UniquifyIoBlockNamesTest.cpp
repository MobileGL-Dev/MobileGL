// MobileGL - MobileGL/MG_Test/ShaderTranspiler/UniquifyIoBlockNamesTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>

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

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        tools.Disassemble(spirv, &text);
        return text;
    }

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    // The tessellation evaluation stage of
    // KHR-GL42/43.shading_language_420pack.length_of_vector_and_matrix_* and
    // .qualifier_order_block_*, reduced to the shape that matters: ONE block name used for
    // both the block this stage consumes and the block it produces. Legal desktop GLSL - the
    // input and output block namespaces are separate - and something SPIRV-Cross re-emits
    // verbatim, so the ESSL it produces declares two different blocks called TCSOutputBlock.
    const char* kCollidingTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;

in  vec4 tcs_tes_result[];
out vec4 tes_gs_result;

in TCSOutputBlock {
    vec4 tcs_tes_variable;
} input_block[];
out TCSOutputBlock {
    vec4 tes_gs_variable;
} output_block;

void main()
{
    tes_gs_result = tcs_tes_result[0];
    output_block.tes_gs_variable = input_block[0].tcs_tes_variable;
}
)";

    // The same stage with the two blocks already named apart, which is the overwhelmingly
    // common shape and the one that must go through untouched.
    const char* kDistinctTessEvalSource = R"(#version 420 core
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

    // gl_PerVertex is an Input block AND an Output block of one name in every tessellation
    // and geometry stage. It is the language's block, not the shader's, so it must never be
    // reported and never be renamed.
    const char* kBuiltinBlockOnlyTessEvalSource = R"(#version 420 core
layout(isolines, point_mode) in;

void main()
{
    gl_Position = gl_in[0].gl_Position;
}
)";
} // namespace

class UniquifyIoBlockNamesTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the renamed module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

TEST_F(UniquifyIoBlockNamesTest, ProbeReportsABlockNameUsedInBothDirections) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kCollidingTessEvalSource);
    ASSERT_FALSE(input.empty());

    std::set<String> colliding;
    std::set<String> declared;
    ShaderCompiler::ProbeIoBlockNamesForEssl(input, colliding, declared);

    EXPECT_EQ(colliding, (std::set<String>{"TCSOutputBlock"}));
    // The name set the caller picks a replacement out of has to contain what the module
    // already spells, or the replacement could land on top of an existing declaration.
    EXPECT_NE(declared.find("TCSOutputBlock"), declared.end());
    EXPECT_NE(declared.find("input_block"), declared.end());
    EXPECT_NE(declared.find("output_block"), declared.end());
}

TEST_F(UniquifyIoBlockNamesTest, ProbeIgnoresAStageWhoseBlocksAlreadyHaveDistinctNames) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kDistinctTessEvalSource);
    ASSERT_FALSE(input.empty());

    std::set<String> colliding;
    std::set<String> declared;
    ShaderCompiler::ProbeIoBlockNamesForEssl(input, colliding, declared);

    EXPECT_TRUE(colliding.empty());
    EXPECT_NE(declared.find("TCSOutputBlock"), declared.end());
}

TEST_F(UniquifyIoBlockNamesTest, ProbeNeverReportsTheBuiltinBlock) {
    const Vector<Uint32> input =
        CompileToSpirv(GL_TESS_EVALUATION_SHADER, kBuiltinBlockOnlyTessEvalSource);
    ASSERT_FALSE(input.empty());

    std::set<String> colliding;
    std::set<String> declared;
    ShaderCompiler::ProbeIoBlockNamesForEssl(input, colliding, declared);

    // gl_PerVertex is read through gl_in and written through gl_Position, i.e. it is exactly
    // the in-and-out-under-one-name shape - and renaming it would invent a block no driver
    // knows.
    EXPECT_TRUE(colliding.empty()) << "gl_PerVertex must never enter the rename plan";
}

TEST_F(UniquifyIoBlockNamesTest, RenamesTheTwoBlocksApartInTheEmittedEssl) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kCollidingTessEvalSource);
    ASSERT_FALSE(input.empty());
    // The generated ESSL really does declare the block twice under one name before the fix -
    // pinning the defect, not just the repair.
    const String before = Transpile(input);
    EXPECT_NE(before.find("in TCSOutputBlock"), String::npos) << before;
    EXPECT_NE(before.find("out TCSOutputBlock"), String::npos) << before;

    // The plan the DirectGLES program build makes for a five-stage program: what this stage
    // consumes is spelled after the tessellation control stage (pipeline index 1) and what it
    // produces after itself (pipeline index 2).
    const std::map<String, String> inputRenames{{"TCSOutputBlock", "TCSOutputBlock_mgio1"}};
    const std::map<String, String> outputRenames{{"TCSOutputBlock", "TCSOutputBlock_mgio2"}};

    std::set<String> renamed;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::UniquifyIoBlockNamesForEssl(input, inputRenames, outputRenames, renamed,
                                                            output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(renamed, (std::set<String>{"TCSOutputBlock"}));

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_EQ(dis.find("\"TCSOutputBlock\""), String::npos)
        << "the colliding name is still on a block struct:\n"
        << dis;
    EXPECT_NE(dis.find("\"TCSOutputBlock_mgio1\""), String::npos) << dis;
    EXPECT_NE(dis.find("\"TCSOutputBlock_mgio2\""), String::npos) << dis;

    const String after = Transpile(output);
    EXPECT_NE(after.find("TCSOutputBlock_mgio1"), String::npos) << after;
    EXPECT_NE(after.find("TCSOutputBlock_mgio2"), String::npos) << after;
    // Only the block TYPE name moves: the instance names are what the body reads and writes
    // through, and the member names are half of what ES matches the interface by.
    EXPECT_NE(after.find("input_block"), String::npos) << after;
    EXPECT_NE(after.find("output_block"), String::npos) << after;
    EXPECT_NE(after.find("tcs_tes_variable"), String::npos) << after;
    EXPECT_NE(after.find("tes_gs_variable"), String::npos) << after;
}

TEST_F(UniquifyIoBlockNamesTest, RenamesOnlyTheDirectionTheCallerPlanned) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kCollidingTessEvalSource);
    ASSERT_FALSE(input.empty());

    // A separate-shader-objects program that ends at this stage plans no output rename,
    // because the block's consumer lives in another program that never saw the plan.
    const std::map<String, String> inputRenames{{"TCSOutputBlock", "TCSOutputBlock_mgio1"}};

    std::set<String> renamed;
    Vector<Uint32> output;
    ASSERT_TRUE(
        ShaderCompiler::UniquifyIoBlockNamesForEssl(input, inputRenames, {}, renamed, output, true));
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(renamed, (std::set<String>{"TCSOutputBlock"}));

    const String dis = Disassemble(output);
    EXPECT_NE(dis.find("\"TCSOutputBlock_mgio1\""), String::npos) << dis;
    // The output block keeps the name the other program still spells.
    EXPECT_NE(dis.find("\"TCSOutputBlock\""), String::npos) << dis;
    EXPECT_EQ(dis.find("\"TCSOutputBlock_mgio2\""), String::npos) << dis;
}

TEST_F(UniquifyIoBlockNamesTest, ReportsNothingWhenThePlanNamesNoBlockThisStageDeclares) {
    const Vector<Uint32> input = CompileToSpirv(GL_TESS_EVALUATION_SHADER, kDistinctTessEvalSource);
    ASSERT_FALSE(input.empty());

    const std::map<String, String> renames{{"SomeOtherBlock", "SomeOtherBlock_mgio2"}};

    std::set<String> renamed;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::UniquifyIoBlockNamesForEssl(input, renames, renames, renamed, output, true));
    // Empty is what tells the DirectGLES program build to keep the module it already had
    // instead of adopting the optimizer's re-serialised copy.
    EXPECT_TRUE(renamed.empty());

    const String dis = Disassemble(output);
    EXPECT_NE(dis.find("\"TCSOutputBlock\""), String::npos) << dis;
    EXPECT_NE(dis.find("\"TESOutputBlock\""), String::npos) << dis;
}
