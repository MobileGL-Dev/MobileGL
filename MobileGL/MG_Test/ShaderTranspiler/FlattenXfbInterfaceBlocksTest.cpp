// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FlattenXfbInterfaceBlocksTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

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

    // The KHR-GL43.vertex_attrib_binding.basic-input capture program's output side, verbatim:
    // a 16-element vec4 array inside a named output block, which is what the capture list
    // addresses member by member ("StageData.attrib[0]" ... "StageData.attrib[15]").
    const char* kCaptureVertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib;
out StageData {
  vec4 attrib[16];
} vs_out;
void main() {
  for (int i = 0; i < 16; ++i) {
    vs_out.attrib[i] = vs_in_attrib;
  }
}
)";

    // Mixed member widths, so a member that claims the wrong number of locations moves every
    // member after it.
    // 440, because a location on the BLOCK is ARB_enhanced_layouts.
    const char* kMixedBlockVertexSource = R"(#version 440 core
layout(location = 0) in vec4 vs_in_attrib;
layout(location = 0) out StageData {
  vec4 first;
  vec2 second;
  mat4 third;
  vec4 fourth;
} vs_out;
void main() {
  vs_out.first = vs_in_attrib;
  vs_out.second = vs_in_attrib.xy;
  vs_out.third = mat4(vs_in_attrib.x);
  vs_out.fourth = vs_in_attrib;
}
)";
} // namespace

class FlattenXfbInterfaceBlocksTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        ShaderCompiler::SetSpirvValidationEnabled(true);
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the flattened module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

TEST_F(FlattenXfbInterfaceBlocksTest, FlattensACapturedBlockIntoOneVariablePerMember) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kCaptureVertexSource);
    ASSERT_FALSE(input.empty());

    std::set<String> flattened;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(input, {"StageData"}, flattened, output));
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(flattened, (std::set<String>{"StageData"}));

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_NE(dis.find("StageData_attrib"), String::npos) << dis;
    // The block itself must have stopped being an interface variable, or the driver would see
    // both spellings of the same data.
    EXPECT_NE(dis.find("Private"), String::npos) << dis;
}

// The declaration is the point of the whole exercise: the emitted ESSL has to declare a plain
// output ARRAY, not an interface block, because that is the shape the Adreno driver can capture.
// SPIR-V validation does NOT catch the difference - leaving the struct's Block decoration on the
// demoted shadow produced a module that validated and emitted `StageData vs_out;` next to a block
// declaration the driver rejected with a bare "'vs_out' : syntax error".
TEST_F(FlattenXfbInterfaceBlocksTest, TheEmittedDeclarationIsAPlainArrayNotABlock) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kCaptureVertexSource);
    ASSERT_FALSE(input.empty());

    // Negative control: untouched, the block is emitted AS a block.
    const String before = Transpile(input);
    EXPECT_NE(before.find("out StageData"), String::npos) << before;
    EXPECT_EQ(before.find("StageData_attrib"), String::npos) << before;

    std::set<String> flattened;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(input, {"StageData"}, flattened, output));

    const String after = Transpile(output);
    EXPECT_NE(after.find("StageData_attrib[16]"), String::npos) << after;
    EXPECT_EQ(after.find("out StageData"), String::npos)
        << "the block must not still be declared as an output block:\n"
        << after;
}

// GL 4.6 core 11.1.2.1: consecutive members take consecutive locations, and a member takes as
// many as its type needs. Getting a span wrong silently moves every member after it.
TEST_F(FlattenXfbInterfaceBlocksTest, GivesEachMemberItsOwnConsecutiveLocations) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kMixedBlockVertexSource);
    ASSERT_FALSE(input.empty());

    std::set<String> flattened;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(input, {"StageData"}, flattened, output));
    ASSERT_FALSE(output.empty());

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_NE(dis.find("OpDecorate %StageData_first Location 0"), String::npos) << dis;
    EXPECT_NE(dis.find("OpDecorate %StageData_second Location 1"), String::npos) << dis;
    EXPECT_NE(dis.find("OpDecorate %StageData_third Location 2"), String::npos) << dis;
    // mat4 takes four, so the member after it starts at 2 + 4.
    EXPECT_NE(dis.find("OpDecorate %StageData_fourth Location 6"), String::npos) << dis;
}

// Nothing captures this block, so nothing may touch it: a shader that merely HAS an output
// block must reach the driver exactly as it was.
TEST_F(FlattenXfbInterfaceBlocksTest, LeavesABlockNoCaptureNamesAlone) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kCaptureVertexSource);
    ASSERT_FALSE(input.empty());

    std::set<String> flattened;
    Vector<Uint32> output;
    ASSERT_TRUE(
        ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(input, {"SomeOtherBlock"}, flattened, output));
    EXPECT_TRUE(flattened.empty());

    const String after = Transpile(output);
    EXPECT_NE(after.find("out StageData"), String::npos) << after;
    EXPECT_EQ(after.find("StageData_attrib"), String::npos) << after;
}

// An empty request must not even run the optimizer: every program without transform feedback
// takes this path on every build.
TEST_F(FlattenXfbInterfaceBlocksTest, DeclinesAnEmptyRequestWithoutRewriting) {
    const Vector<Uint32> input = CompileToSpirv(GL_VERTEX_SHADER, kCaptureVertexSource);
    ASSERT_FALSE(input.empty());

    std::set<String> flattened;
    Vector<Uint32> output;
    EXPECT_FALSE(ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(input, {}, flattened, output));
    EXPECT_TRUE(flattened.empty());
    EXPECT_TRUE(output.empty());
}

// The capture list has to follow the declaration exactly, and only for blocks that were
// actually rewritten - a member of a block left alone keeps the application's spelling, and so
// does a name with no block prefix at all (gl_Position, a plain varying).
TEST_F(FlattenXfbInterfaceBlocksTest, RewritesOnlyTheCaptureNamesOfFlattenedBlocks) {
    String rewritten;
    EXPECT_TRUE(ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock("StageData.attrib[0]", {"StageData"},
                                                                       rewritten));
    EXPECT_EQ(rewritten, "StageData_attrib[0]");

    EXPECT_TRUE(
        ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock("StageData.attrib", {"StageData"}, rewritten));
    EXPECT_EQ(rewritten, "StageData_attrib");

    EXPECT_FALSE(
        ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock("Other.member", {"StageData"}, rewritten));
    EXPECT_FALSE(ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock("gl_Position", {"StageData"}, rewritten));
    EXPECT_FALSE(ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock("vColor", {"StageData"}, rewritten));
    EXPECT_FALSE(ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock(".leading", {"StageData"}, rewritten));
}
