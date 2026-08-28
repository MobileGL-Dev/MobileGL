// MobileGL - MobileGL/MG_Test/ShaderTranspiler/DemotePointSizeTest.cpp
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

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    // Compiles and LINKS a whole program, then returns one sanitized module per stage - the
    // exact bytes ProgramSpirvTask hands the demotion in production, so every shape assertion
    // below is made against what the backends would really receive.
    Vector<Vector<Uint32>> CompileProgramToSpirv(const Vector<Pair<GLenum, const char*>>& stages) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        Vector<GLenum> types;
        for (const auto& [stage, source] : stages) {
            // sourceStr is a StringView; the literals handed in are static, so the view
            // stays valid for the whole compile.
            ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = source};
            auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
            EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log);
            if (!shaderResult) return {};
            shaders.push_back(shaderResult.value());
            types.push_back(stage);
        }
        ProgramAttrib programAttrib{.shaders = shaders};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = types, .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult) return {};
        Vector<Vector<Uint32>> modules = Move(binaryResult.value());
        for (auto& module : modules) {
            EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        }
        return modules;
    }

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        EXPECT_TRUE(tools.Disassemble(spirv, &text));
        return text;
    }

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    Bool Validates(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        return tools.Validate(spirv);
    }

    // The five-stage shape of the KHR-GL4x transform-feedback / tessellation capture bodies:
    // the value is WRITTEN in the vertex stage, READ from gl_in and re-written in every stage
    // after it, and the rasterized size never matters (the captures run under rasterizer
    // discard). This is exactly the class the demotion exists to rescue.
    const char* kVertexSource = R"(#version 460 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_PointSize = 2.0;
}
)";

    const char* kTessControlSource = R"(#version 460 core
layout(vertices = 3) out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_PointSize = gl_in[gl_InvocationID].gl_PointSize + 1.0;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

    const char* kTessEvalSource = R"(#version 460 core
layout(triangles, point_mode) in;
void main() {
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position +
                  gl_TessCoord.z * gl_in[2].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize + gl_in[1].gl_PointSize + gl_in[2].gl_PointSize;
}
)";

    const char* kGeometrySource = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize * 2.0;
    EmitVertex();
    EndPrimitive();
}
)";

    const char* kFragmentSource = R"(#version 460 core
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";

    // A control chain that never touches point size: the demotion must prove it changed
    // NOTHING here, byte for byte, because this is the overwhelming majority of programs on
    // an affected device.
    const char* kPlainTessControlSource = R"(#version 460 core
layout(vertices = 3) out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

    const char* kPlainTessEvalSource = R"(#version 460 core
layout(triangles, point_mode) in;
void main() {
    gl_Position = gl_in[0].gl_Position;
}
)";

    const char* kPlainVertexSource = R"(#version 460 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
}
)";

    // A tessellation evaluation module reaching PointSize through a WHOLE-STRUCT load - the
    // one shape the pass must refuse rather than half-rewrite. glslang never emits it, so it
    // is assembled by hand.
    const char* kWholeStructCopyTessEvalAsm = R"(
               OpCapability Tessellation
               OpCapability TessellationPointSize
               OpMemoryModel Logical GLSL450
               OpEntryPoint TessellationEvaluation %main "main" %gl_in %out_block
               OpExecutionMode %main Triangles
               OpExecutionMode %main SpacingEqual
               OpExecutionMode %main VertexOrderCcw
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize
               OpDecorate %gl_PerVertex Block
       %void = OpTypeVoid
      %fn_ty = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%gl_PerVertex = OpTypeStruct %v4float %float
       %uint = OpTypeInt 32 0
    %uint_32 = OpConstant %uint 32
        %arr = OpTypeArray %gl_PerVertex %uint_32
 %ptr_in_arr = OpTypePointer Input %arr
      %gl_in = OpVariable %ptr_in_arr Input
  %ptr_out_s = OpTypePointer Output %gl_PerVertex
  %out_block = OpVariable %ptr_out_s Output
   %ptr_in_s = OpTypePointer Input %gl_PerVertex
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
       %main = OpFunction %void None %fn_ty
      %entry = OpLabel
          %p = OpAccessChain %ptr_in_s %gl_in %int_0
          %v = OpLoad %gl_PerVertex %p
               OpStore %out_block %v
               OpReturn
               OpFunctionEnd
)";
} // namespace

class DemotePointSizeTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        m_validationFailuresBefore = ShaderCompiler::SpirvValidationFailureCount();
    }
    void TearDown() override {
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresBefore)
            << "a demoted module did not survive spirv-val";
    }

private:
    Uint64 m_validationFailuresBefore = 0;
};

TEST_F(DemotePointSizeTest, DemotesAFiveStageProgramWholesale) {
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                            {GL_TESS_CONTROL_SHADER, kTessControlSource},
                                                            {GL_TESS_EVALUATION_SHADER, kTessEvalSource},
                                                            {GL_GEOMETRY_SHADER, kGeometrySource},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 5u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};

    // The defect, pinned first: every tessellation/geometry stage really does declare the
    // capability the device lacks - the same probe production's declines use.
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]));
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[2]));
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[3]));

    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, /*captureRequestsPointSize=*/true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    // THE PRODUCTION GATE, as the arming guard: after demotion neither decline can arm.
    // Magma's refusal and Espryt's missing-extension failure both key off exactly these.
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[2]));
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[3]));
    for (const auto& module : modules) {
        EXPECT_TRUE(Validates(module));
    }

    // The carrier chain, boundary by boundary. No user varyings, so the shared location is 0.
    const String vs = Disassemble(modules[0]);
    EXPECT_NE(vs.find("OpName %mg_PointSizeIo0"), String::npos) << vs;
    EXPECT_NE(vs.find("OpStore %mg_PointSizeIo0"), String::npos)
        << "the vertex stage must mirror its built-in into the carrier:\n"
        << vs;
    EXPECT_NE(vs.find("BuiltIn PointSize"), String::npos)
        << "the vertex stage KEEPS its core built-in - only tess/geometry stages demote:\n"
        << vs;

    const String tcs = Disassemble(modules[1]);
    EXPECT_EQ(tcs.find("OpCapability TessellationPointSize"), String::npos) << tcs;
    EXPECT_NE(tcs.find("OpName %mg_PointSizeIo0"), String::npos) << tcs;
    EXPECT_NE(tcs.find("OpName %mg_PointSizeIo1"), String::npos) << tcs;

    const String tes = Disassemble(modules[2]);
    EXPECT_EQ(tes.find("OpCapability TessellationPointSize"), String::npos) << tes;
    EXPECT_NE(tes.find("OpName %mg_PointSizeIo1"), String::npos) << tes;
    EXPECT_NE(tes.find("OpName %mg_PointSizeIo2"), String::npos)
        << "with a geometry stage present the evaluation stage feeds the Io2 boundary, not the "
           "capture carrier:\n"
        << tes;

    const String gs = Disassemble(modules[3]);
    EXPECT_EQ(gs.find("OpCapability GeometryPointSize"), String::npos) << gs;
    EXPECT_NE(gs.find("OpName %mg_PointSizeIo2"), String::npos) << gs;
    EXPECT_NE(gs.find("OpName %mg_PointSizeCapture"), String::npos) << gs;
    EXPECT_NE(gs.find("OpDecorate %mg_PointSizeCapture Location 0"), String::npos) << gs;
    EXPECT_NE(gs.find("OpStore %mg_PointSizeCapture"), String::npos) << gs;

    // The struct keeps its member - declared, decorated, unaccessed - which is the shape a
    // point-size-free glslang module already has on every extension-less driver.
    EXPECT_NE(tes.find("BuiltIn PointSize"), String::npos) << tes;

    // What SPIRV-Cross then prints: no gl_PointSize anywhere in a demoted stage's ESSL (the
    // token DirectGLES's extension gate greps for), the carriers in its place.
    const String tesEssl = Transpile(modules[2]);
    EXPECT_EQ(tesEssl.find("gl_PointSize"), String::npos) << tesEssl;
    EXPECT_NE(tesEssl.find("mg_PointSizeIo1"), String::npos) << tesEssl;
    const String gsEssl = Transpile(modules[3]);
    EXPECT_EQ(gsEssl.find("gl_PointSize"), String::npos) << gsEssl;
    EXPECT_NE(gsEssl.find("mg_PointSizeCapture"), String::npos) << gsEssl;

    // Demotion is idempotent by construction: with the capability gone, a second pass over
    // the same modules finds nothing to arm on and must not touch a byte.
    Vector<Vector<Uint32>> again = modules;
    ShaderCompiler::PointSizeDemotionOutcome secondOutcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        again, types, true, true, true, secondOutcome, true, true));
    EXPECT_FALSE(secondOutcome.demoted);
    EXPECT_TRUE(secondOutcome.declineDetail.empty()) << secondOutcome.declineDetail;
    EXPECT_EQ(again, modules);
}

TEST_F(DemotePointSizeTest, WithoutAGeometryStageTheEvaluationStageOwnsTheCaptureCarrier) {
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                            {GL_TESS_CONTROL_SHADER, kTessControlSource},
                                                            {GL_TESS_EVALUATION_SHADER, kTessEvalSource},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    const String tes = Disassemble(modules[2]);
    EXPECT_NE(tes.find("OpName %mg_PointSizeCapture"), String::npos) << tes;
    EXPECT_NE(tes.find("OpStore %mg_PointSizeCapture"), String::npos) << tes;
    EXPECT_EQ(tes.find("OpName %mg_PointSizeIo2"), String::npos)
        << "no geometry stage, no Io2 boundary:\n"
        << tes;
}

TEST_F(DemotePointSizeTest, AGeometryOnlyProgramReadsTheVertexBoundary) {
    const char* geometryReadingVs = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize * 2.0;
    EmitVertex();
    EndPrimitive();
}
)";
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                            {GL_GEOMETRY_SHADER, geometryReadingVs},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 3u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, /*demoteTessellation=*/false, /*demoteGeometry=*/true, false, outcome, true,
        true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    const String gs = Disassemble(modules[1]);
    EXPECT_NE(gs.find("OpName %mg_PointSizeIo0"), String::npos)
        << "the geometry stage's input boundary is fed by the vertex stage:\n"
        << gs;
    const String vs = Disassemble(modules[0]);
    EXPECT_NE(vs.find("OpStore %mg_PointSizeIo0"), String::npos) << vs;
}

TEST_F(DemotePointSizeTest, TheCarrierLandsPastTheProgramsOwnVaryings) {
    const char* vsWithVarying = R"(#version 460 core
out vec4 v_color;
void main() {
    gl_Position = vec4(1.0);
    gl_PointSize = 3.0;
    v_color = vec4(0.5);
}
)";
    const char* gsWithVarying = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
in vec4 v_color[];
out vec4 g_color;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize;
    g_color = v_color[0];
    EmitVertex();
    EndPrimitive();
}
)";
    const char* fsWithVarying = R"(#version 460 core
in vec4 g_color;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = g_color; }
)";
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, vsWithVarying},
                                                            {GL_GEOMETRY_SHADER, gsWithVarying},
                                                            {GL_FRAGMENT_SHADER, fsWithVarying}});
    ASSERT_EQ(modules.size(), 3u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, false, true, true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    // v_color / g_color occupy location 0, so every carrier must sit at 1 - in every stage,
    // because producer and consumer match by location.
    const String vs = Disassemble(modules[0]);
    EXPECT_NE(vs.find("OpDecorate %mg_PointSizeIo0 Location 1"), String::npos) << vs;
    const String gs = Disassemble(modules[1]);
    EXPECT_NE(gs.find("OpDecorate %mg_PointSizeIo0 Location 1"), String::npos) << gs;
    EXPECT_NE(gs.find("OpDecorate %mg_PointSizeCapture Location 1"), String::npos) << gs;
}

TEST_F(DemotePointSizeTest, APointSizeFreeProgramStaysByteIdentical) {
    Vector<Vector<Uint32>> modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kPlainVertexSource},
                               {GL_TESS_CONTROL_SHADER, kPlainTessControlSource},
                               {GL_TESS_EVALUATION_SHADER, kPlainTessEvalSource},
                               {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);
    const Vector<Vector<Uint32>> before = modules;
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, false, outcome, true, true));
    EXPECT_FALSE(outcome.demoted);
    EXPECT_TRUE(outcome.declineDetail.empty()) << outcome.declineDetail;
    EXPECT_EQ(modules, before);
}

TEST_F(DemotePointSizeTest, AHostingDeviceStaysByteIdentical) {
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                            {GL_TESS_CONTROL_SHADER, kTessControlSource},
                                                            {GL_TESS_EVALUATION_SHADER, kTessEvalSource},
                                                            {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);
    const Vector<Vector<Uint32>> before = modules;
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    // Both verdicts say the device hosts the built-in: the un-forced lane's contract.
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, false, false, true, outcome, true, true));
    EXPECT_FALSE(outcome.demoted);
    EXPECT_EQ(modules, before);
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]))
        << "the un-demoted module must still arm the existing declines";
}

TEST_F(DemotePointSizeTest, ACaptureRequestForcesTheCarrierOnANonWritingCaptureStage) {
    // The control stage writes point size (arming the demotion); the evaluation stage never
    // does - but a by-name capture must still find the carrier declared there, holding
    // whatever an unwritten varying holds, exactly as the unwritten built-in would have.
    Vector<Vector<Uint32>> modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                               {GL_TESS_CONTROL_SHADER, kTessControlSource},
                               {GL_TESS_EVALUATION_SHADER, kPlainTessEvalSource},
                               {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, /*captureRequestsPointSize=*/true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    const String tes = Disassemble(modules[2]);
    EXPECT_NE(tes.find("OpName %mg_PointSizeCapture"), String::npos) << tes;
    EXPECT_TRUE(Validates(modules[2]));
}

TEST_F(DemotePointSizeTest, AWholeStructCopyDeclinesTheProgramByteIdentically) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    Vector<Uint32> module;
    ASSERT_TRUE(tools.Assemble(kWholeStructCopyTessEvalAsm, &module));
    ASSERT_TRUE(tools.Validate(module));

    Vector<Vector<Uint32>> modules{module};
    const Vector<GLenum> types{GL_TESS_EVALUATION_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, false, outcome, true, true));
    EXPECT_FALSE(outcome.demoted);
    EXPECT_FALSE(outcome.declineDetail.empty())
        << "a shape the pass cannot express must say so, not silently no-op";
    EXPECT_EQ(modules[0], module) << "a decline must not leave a half-demoted module behind";
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[0]))
        << "the declined module must still arm the existing honest refusals";
}
