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

    // A control stage that also carries CLIP DISTANCE. SPIRV-Cross force-redeclares the whole
    // gl_PerVertex output block for exactly this stage/builtin combination, and prints its
    // members from the struct's DECORATIONS rather than from what the module accesses - so a
    // demoted module's untouched PointSize member would still reach the driver's ESSL.
    const char* kClipDistanceTessControlSource = R"(#version 460 core
layout(vertices = 3) out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_PointSize = gl_in[gl_InvocationID].gl_PointSize + 1.0;
    gl_out[gl_InvocationID].gl_ClipDistance[0] = 0.5;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

    // The same clip-distance write and NO point-size access anywhere: the shape a
    // successfully demoted module would have been left in. glslang emits the whole
    // four-member gl_PerVertex block regardless, which is what makes it the exact
    // "declared but unaccessed" state the pass header's premise is about.
    const char* kClipDistanceUnusedPointSizeTessControlSource = R"(#version 460 core
layout(vertices = 3) out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_out[gl_InvocationID].gl_ClipDistance[0] = 0.5;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
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
    // token DirectGLES's extension gate greps for), the carriers in its place. The CONTROL
    // stage is transpiled too, and deliberately: it is the one stage SPIRV-Cross can be made
    // to redeclare the whole output block for, which is why the clip-distance combination
    // declines instead of demoting.
    const String tcsEssl = Transpile(modules[1]);
    EXPECT_EQ(tcsEssl.find("gl_PointSize"), String::npos) << tcsEssl;
    EXPECT_NE(tcsEssl.find("mg_PointSizeIo1"), String::npos) << tcsEssl;
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

    // And the driver-side half of the same contract: the ESSL DirectGLES hands its driver
    // has to DECLARE the carrier, because DirectGLES respells the glTransformFeedbackVaryings
    // request to that name. A carrier the transpile dropped would take the whole capture set
    // down with an ES link error naming a variable the application never wrote.
    const String tesEssl = Transpile(modules[2]);
    EXPECT_NE(tesEssl.find("mg_PointSizeCapture"), String::npos) << tesEssl;
}

// THE PRODUCTION SHAPE THE FORCED CARRIER EXISTS FOR, and the one the flag's own unit test
// could not reach: the capture stage never WRITES gl_PointSize, it only reads the incoming
// one. The demotion still arms - glslang declares GeometryPointSize on a READ - so the
// built-in leaves the module, and only the capture request can put a carrier back. In
// production that request arrives as ProgramLinkTask::SpirvHandoff::captureRequestsPointSize;
// this is the same value one layer down.
TEST_F(DemotePointSizeTest, AReadOnlyCaptureStageStillDeclaresTheCaptureCarrier) {
    const char* readOnlyGeometry = R"(#version 460 core
layout(points) in;
layout(points, max_vertices = 1) out;
out float g_echo;
void main() {
    gl_Position = gl_in[0].gl_Position;
    g_echo = gl_in[0].gl_PointSize;
    EmitVertex();
    EndPrimitive();
}
)";
    const char* echoFragment = R"(#version 460 core
in float g_echo;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(g_echo); }
)";
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                            {GL_GEOMETRY_SHADER, readOnlyGeometry},
                                                            {GL_FRAGMENT_SHADER, echoFragment}});
    ASSERT_EQ(modules.size(), 3u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};

    // The premise: a stage that only READS the built-in still declares the capability, so the
    // device still refuses it and the demotion still arms.
    ASSERT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]))
        << "a geometry stage that only reads gl_in[].gl_PointSize must still declare "
           "GeometryPointSize, or this whole class of program was never affected";

    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, false, true, /*captureRequestsPointSize=*/true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    const String gs = Disassemble(modules[1]);
    EXPECT_NE(gs.find("OpName %mg_PointSizeIo0"), String::npos)
        << "the read still has to reach the vertex stage's mirrored value:\n"
        << gs;
    EXPECT_NE(gs.find("OpName %mg_PointSizeCapture"), String::npos)
        << "the capture request must force the carrier even though this stage never writes "
           "the built-in; without it DirectGLES respells the capture to a name no stage "
           "declares and the whole capture set fails to link:\n"
        << gs;
    const String gsEssl = Transpile(modules[1]);
    EXPECT_NE(gsEssl.find("mg_PointSizeCapture"), String::npos) << gsEssl;
    EXPECT_EQ(gsEssl.find("gl_PointSize"), String::npos) << gsEssl;

    // Without the request there is nothing to bind a by-name capture to - which is exactly
    // what production did on every link while the request never reached this call.
    Vector<Vector<Uint32>> unrequested = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                                                                {GL_GEOMETRY_SHADER, readOnlyGeometry},
                                                                {GL_FRAGMENT_SHADER, echoFragment}});
    ASSERT_EQ(unrequested.size(), 3u);
    ShaderCompiler::PointSizeDemotionOutcome unrequestedOutcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        unrequested, types, false, true, /*captureRequestsPointSize=*/false, unrequestedOutcome, true,
        true));
    EXPECT_TRUE(unrequestedOutcome.demoted) << unrequestedOutcome.declineDetail;
    EXPECT_EQ(Disassemble(unrequested[1]).find("OpName %mg_PointSizeCapture"), String::npos)
        << "with no capture asking for it, the carrier must not be declared";
}

// THE PREMISE THE PASS HEADER USED TO STATE UNIVERSALLY: "declared but no longer accessed"
// is invisible to the ES hop. It is not, for one stage/builtin combination - and this case
// pins the mechanism with no demotion involved at all, so a future SPIRV-Cross that emitted
// by ACCESS would fail here first and the decline below could be relaxed.
TEST_F(DemotePointSizeTest, ARedeclaredControlBlockPrintsAnUnaccessedPointSizeMember) {
    Vector<Vector<Uint32>> modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kPlainVertexSource},
                               {GL_TESS_CONTROL_SHADER, kClipDistanceUnusedPointSizeTessControlSource},
                               {GL_TESS_EVALUATION_SHADER, kPlainTessEvalSource},
                               {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);

    // Nothing in this control stage touches point size, so nothing declares the capability -
    // it is byte-for-byte the state a demoted module would be left in.
    ASSERT_FALSE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]));
    const String tcs = Disassemble(modules[1]);
    EXPECT_NE(tcs.find("BuiltIn PointSize"), String::npos)
        << "the member has to still be declared for this case to say anything:\n"
        << tcs;

    const String tcsEssl = Transpile(modules[1]);
    EXPECT_NE(tcsEssl.find("gl_PointSize"), String::npos)
        << "SPIRV-Cross force-redeclares a control stage's gl_PerVertex output block when its "
           "clip/cull distances are live, and prints the block's members from their "
           "decorations rather than from what is accessed. DirectGLES's extension gate is a "
           "text search for this token over exactly this string:\n"
        << tcsEssl;
}

// ... and therefore this program declines rather than demoting: a mutated module that the
// driver still rejects is strictly worse than the honest refusal, because it also flips the
// program-wide verdict and the L1 key.
TEST_F(DemotePointSizeTest, AControlStageCarryingClipDistanceDeclinesTheProgram) {
    Vector<Vector<Uint32>> modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                               {GL_TESS_CONTROL_SHADER, kClipDistanceTessControlSource},
                               {GL_TESS_EVALUATION_SHADER, kTessEvalSource},
                               {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 4u);
    const Vector<Vector<Uint32>> before = modules;
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER,
                               GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, true, outcome, true, true));
    EXPECT_FALSE(outcome.demoted);
    EXPECT_NE(outcome.declineDetail.find("clip/cull"), String::npos) << outcome.declineDetail;
    EXPECT_EQ(modules, before) << "a decline must leave every module byte-identical";
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresTessellationOrGeometryPointSize(modules[1]))
        << "the declined program must still arm the existing honest refusals";
}

// A legal desktop-GL shape the passthrough machinery explicitly serves: an evaluation stage
// sitting straight on the vertex stage. Both backends synthesize the missing control stage,
// and that synthesized stage forwards gl_Position and nothing else - so the input carrier the
// demotion would create has no producer, and each backend's "reads a located input" guard
// would decline the program against a varying name the application never wrote. Declining the
// demotion instead keeps the modules, and the diagnostics, honest.
TEST_F(DemotePointSizeTest, AnEvaluationStageWithNoControlStageDeclines) {
    const char* readingTessEval = R"(#version 460 core
layout(triangles, point_mode) in;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize + 1.0;
}
)";
    Vector<Vector<Uint32>> modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertexSource},
                               {GL_TESS_EVALUATION_SHADER, readingTessEval},
                               {GL_FRAGMENT_SHADER, kFragmentSource}});
    ASSERT_EQ(modules.size(), 3u);
    const Vector<Vector<Uint32>> before = modules;
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_EVALUATION_SHADER, GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, true, true, true, outcome, true, true));
    EXPECT_FALSE(outcome.demoted);
    EXPECT_NE(outcome.declineDetail.find("control stage"), String::npos) << outcome.declineDetail;
    EXPECT_EQ(modules, before) << "a decline must leave every module byte-identical";
    EXPECT_FALSE(ShaderCompiler::ModuleReadsLocatedInput(modules[1]))
        << "the declined evaluation stage must not have acquired the located input carrier "
           "that both backends' pass-through guard refuses";
}

// The carrier is placed one past the highest location any stage CONSUMES, and a 64-bit
// vector consumes two of them. GL 4.6 core 11.1.2.1 says so for doubles, and
// ARB_gpu_shader_int64 - which DirectVulkan advertises unconditionally - extends the rule
// verbatim to i64/u64. An i64vec4 counted as one location would put the carrier on the
// SECOND location that varying already owns: two Output variables at one location, an
// invalid Vulkan interface and an ES link error naming a variable the application never
// wrote. This is the one direction the placement is not allowed to be wrong in.
TEST_F(DemotePointSizeTest, TheCarrierClearsA64BitIntegerVectorVarying) {
    const char* wideVertex = R"(#version 460 core
#extension GL_ARB_gpu_shader_int64 : require
layout(location = 0) flat out i64vec4 v_wide;
void main() {
    gl_Position = vec4(1.0);
    gl_PointSize = 3.0;
    v_wide = i64vec4(1, 2, 3, 4);
}
)";
    const char* wideGeometry = R"(#version 460 core
#extension GL_ARB_gpu_shader_int64 : require
layout(points) in;
layout(points, max_vertices = 1) out;
layout(location = 0) flat in i64vec4 v_wide[];
layout(location = 0) flat out i64vec4 g_wide;
void main() {
    gl_Position = gl_in[0].gl_Position;
    gl_PointSize = gl_in[0].gl_PointSize;
    g_wide = v_wide[0];
    EmitVertex();
    EndPrimitive();
}
)";
    const char* wideFragment = R"(#version 460 core
#extension GL_ARB_gpu_shader_int64 : require
layout(location = 0) flat in i64vec4 g_wide;
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(float(g_wide.x)); }
)";
    Vector<Vector<Uint32>> modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, wideVertex},
                                                            {GL_GEOMETRY_SHADER, wideGeometry},
                                                            {GL_FRAGMENT_SHADER, wideFragment}});
    ASSERT_EQ(modules.size(), 3u);
    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER};
    ShaderCompiler::PointSizeDemotionOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::DemoteTessellationGeometryPointSizeForProgram(
        modules, types, false, true, true, outcome, true, true));
    EXPECT_TRUE(outcome.demoted) << outcome.declineDetail;

    // v_wide / g_wide sit at location 0 and occupy 0 AND 1, so every carrier must clear 2.
    const String vs = Disassemble(modules[0]);
    EXPECT_NE(vs.find("OpDecorate %mg_PointSizeIo0 Location 2"), String::npos)
        << "the carrier landed on a location the i64vec4 varying already owns:\n"
        << vs;
    const String gs = Disassemble(modules[1]);
    EXPECT_NE(gs.find("OpDecorate %mg_PointSizeIo0 Location 2"), String::npos) << gs;
    EXPECT_NE(gs.find("OpDecorate %mg_PointSizeCapture Location 2"), String::npos) << gs;
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
