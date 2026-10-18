// MobileGL - MobileGL/MG_Test/ShaderTranspiler/StripCompatPerVertexMembersTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A compatibility-profile stage that only uses core features must reach the backends as the
// same Vulkan-valid SPIR-V as its core twin. glslang declares the legacy gl_PerVertex members
// (gl_FrontColor, gl_TexCoord, ...) without a BuiltIn, which leaves a module that writes only
// gl_Position invalid for Vulkan until the sanitize chain removes them.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/StripCompatPerVertexMembersPass.h>

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;
using MobileGL::MG_Util::ShaderTranspiler::StripCompatPerVertexMembersPass;

namespace {
    ShaderStage StageOf(GLenum type) {
        switch (type) {
        case GL_VERTEX_SHADER:
            return ShaderStage::Vertex;
        case GL_TESS_CONTROL_SHADER:
            return ShaderStage::TessControl;
        case GL_TESS_EVALUATION_SHADER:
            return ShaderStage::TessEval;
        case GL_GEOMETRY_SHADER:
            return ShaderStage::Geometry;
        default:
            return ShaderStage::Fragment;
        }
    }

    // Preprocesses, compiles and links a whole program the way the link task does, and returns
    // one module per stage: straight out of GlslangToSpv, or after the shared sanitize chain.
    Vector<Vector<Uint32>> CompileProgram(const Vector<Pair<GLenum, String>>& stages, Bool sanitize = true) {
        using namespace MG_Util::ShaderTranspiler;
        const CompileEnv env{};
        Vector<String> sources;
        sources.reserve(stages.size());
        Vector<SharedPtr<glslang::TShader>> shaders;
        Vector<GLenum> types;
        for (const auto& [type, text] : stages) {
            String& source = sources.emplace_back(text);
            PreprocessShaderSource(StageOf(type), source, env);
            ShaderAttrib attrib{.shaderType = type, .sourceStr = source};
            auto shader = ShaderCompiler::CompileShader(attrib);
            EXPECT_TRUE(shader) << (shader ? String{} : shader.error().log);
            if (!shader) return {};
            shaders.push_back(shader.value());
            types.push_back(type);
        }
        ProgramAttrib programAttrib{.shaders = shaders};
        auto program = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(program) << (program ? String{} : program.error().log);
        if (!program) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = types, .program = *program.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binary) << (binary ? String{} : binary.error().log);
        if (!binary) return {};
        Vector<Vector<Uint32>> modules = Move(binary.value());
        if (sanitize) {
            for (auto& module : modules) {
                EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
            }
        }
        return modules;
    }

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        EXPECT_TRUE(tools.Disassemble(spirv, &text));
        return text;
    }

    String ValidationError(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            error += message != nullptr ? message : "";
        });
        return tools.Validate(spirv) ? String{} : (error.empty() ? String{"invalid"} : error);
    }

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    // Runs the pass alone, as the sanitize chain would, on a hand-assembled module.
    Vector<Uint32> RunPass(const Vector<Uint32>& input) {
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
        optimizer.RegisterPass(StripCompatPerVertexMembersPass::CreateStripCompatPerVertexMembersPass());
        spvtools::OptimizerOptions options;
        options.set_run_validator(false);
        Vector<Uint32> output;
        EXPECT_TRUE(optimizer.Run(input.data(), input.size(), &output, options));
        return output;
    }

    // Every compatibility source is compiled as `#version 460 compatibility`
    // (GetNormalizedVersionDirective), so its core twin is the 460 core source.
    String Compat(const char* body, const char* version = "410") {
        return String("#version ") + version + " compatibility\n" + body;
    }

    String Core(const char* body) {
        return String("#version 460 core\n") + body;
    }

    // Disassembly with ids renumbered in order of appearance, so two modules that differ only in
    // the ids glslang happened to allocate compare equal.
    String Canonical(const Vector<Uint32>& spirv) {
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
        optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
        Vector<Uint32> compacted;
        EXPECT_TRUE(optimizer.Run(spirv.data(), spirv.size(), &compacted));
        return Disassemble(compacted);
    }

    // Compiles both programs and expects every compatibility module to validate and to equal the
    // core twin's.
    void ExpectSameAsCoreTwin(const Vector<Pair<GLenum, String>>& compatStages,
                              const Vector<Pair<GLenum, String>>& coreStages) {
        const auto compat = CompileProgram(compatStages);
        const auto core = CompileProgram(coreStages);
        ASSERT_EQ(compat.size(), compatStages.size());
        ASSERT_EQ(core.size(), coreStages.size());
        for (SizeT i = 0; i < compat.size(); ++i) {
            EXPECT_EQ(ValidationError(compat[i]), "") << Disassemble(compat[i]);
            EXPECT_EQ(Canonical(compat[i]), Canonical(core[i])) << "stage " << i;
        }
    }

    const char* kVertexBody = R"(in vec2 pos;
out vec4 c;
void main() {
    c = vec4(0.25, 0.5, 0.75, 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

    const char* kFragmentBody = R"(in vec4 c;
out vec4 o;
void main() { o = c; }
)";

    // Writes the core members after the legacy ones, so their indices have to move.
    const char* kClipCullVertexBody = R"(in vec2 pos;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    gl_PointSize = 2.0;
    gl_ClipDistance[0] = pos.x;
    gl_CullDistance[0] = pos.y;
}
)";

    const char* kGeometryBody = R"(layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec4 c[];
out vec4 gc;
void main() {
    for (int i = 0; i < 3; ++i) {
        gc = c[i];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
)";

    const char* kGeometryFragmentBody = R"(in vec4 gc;
out vec4 o;
void main() { o = gc; }
)";

    const char* kTessControlBody = R"(layout(vertices = 3) out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_TessLevelOuter[0] = 1.0;
    gl_TessLevelOuter[1] = 1.0;
    gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
}
)";

    const char* kTessEvalBody = R"(layout(triangles) in;
void main() {
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position +
                  gl_TessCoord.z * gl_in[2].gl_Position;
}
)";

    const char* kPositionOnlyVertexBody = R"(in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

    const char* kWhiteFragmentBody = R"(out vec4 o;
void main() { o = vec4(1.0); }
)";

    // A compatibility-only built-in in use: there is no core shape to give this stage.
    const char* kFrontColorVertexBody = R"(in vec2 pos;
void main() {
    gl_FrontColor = vec4(1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

    // Reads a legacy member only into dead code, which the sanitize chain removes first.
    const char* kDeadLegacyReadGeometryBody = R"(layout(points) in;
layout(points, max_vertices = 1) out;
void main() {
    vec4 unused = gl_in[0].gl_FrontColor;
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
}
)";

    // A mixed built-in block reached through a whole-block load, which the pass cannot follow.
    const char* kWholeBlockLoadAsm = R"(
               OpCapability Geometry
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %gl_in %out_block
               OpExecutionMode %main InputPoints
               OpExecutionMode %main Invocations 1
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpDecorate %gl_PerVertex Block
       %void = OpTypeVoid
      %fn_ty = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%gl_PerVertex = OpTypeStruct %v4float %v4float
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
        %arr = OpTypeArray %gl_PerVertex %uint_1
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
               OpEmitVertex
               OpReturn
               OpFunctionEnd
)";

    // The OpTypeStruct line and the member decorations of every per-vertex block, in order.
    String PerVertexShape(const String& disassembly) {
        String shape;
        SizeT lineStart = 0;
        while (lineStart < disassembly.size()) {
            SizeT lineEnd = disassembly.find('\n', lineStart);
            if (lineEnd == String::npos) lineEnd = disassembly.size();
            const String line = disassembly.substr(lineStart, lineEnd - lineStart);
            if (line.find("%gl_PerVertex") != String::npos &&
                (line.find("OpTypeStruct") != String::npos || line.find("OpMemberDecorate") != String::npos)) {
                shape += line + "\n";
            }
            lineStart = lineEnd + 1;
        }
        return shape;
    }
} // namespace

class StripCompatPerVertexMembersTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
};

TEST_F(StripCompatPerVertexMembersTest, VertexStageMatchesItsCoreTwin) {
    const auto raw =
        CompileProgram({{GL_VERTEX_SHADER, Compat(kVertexBody)}, {GL_FRAGMENT_SHADER, Compat(kFragmentBody)}}, false);
    ASSERT_EQ(raw.size(), 2u);
    // The defect, pinned: glslang's own module mixes legacy and built-in members.
    EXPECT_NE(ValidationError(raw[0]).find("BuiltIn"), String::npos) << Disassemble(raw[0]);

    for (const char* version : {"410", "150"}) {
        SCOPED_TRACE(version);
        ExpectSameAsCoreTwin(
            {{GL_VERTEX_SHADER, Compat(kVertexBody, version)}, {GL_FRAGMENT_SHADER, Compat(kFragmentBody, version)}},
            {{GL_VERTEX_SHADER, Core(kVertexBody)}, {GL_FRAGMENT_SHADER, Core(kFragmentBody)}});
    }
}

TEST_F(StripCompatPerVertexMembersTest, CoreMembersAfterTheLegacyOnesAreRenumbered) {
    ExpectSameAsCoreTwin(
        {{GL_VERTEX_SHADER, Compat(kClipCullVertexBody)}, {GL_FRAGMENT_SHADER, Compat(kWhiteFragmentBody)}},
        {{GL_VERTEX_SHADER, Core(kClipCullVertexBody)}, {GL_FRAGMENT_SHADER, Core(kWhiteFragmentBody)}});
}

TEST_F(StripCompatPerVertexMembersTest, GeometryStageInputAndOutputBlocks) {
    ExpectSameAsCoreTwin({{GL_VERTEX_SHADER, Compat(kVertexBody)},
                          {GL_GEOMETRY_SHADER, Compat(kGeometryBody)},
                          {GL_FRAGMENT_SHADER, Compat(kGeometryFragmentBody)}},
                         {{GL_VERTEX_SHADER, Core(kVertexBody)},
                          {GL_GEOMETRY_SHADER, Core(kGeometryBody)},
                          {GL_FRAGMENT_SHADER, Core(kGeometryFragmentBody)}});
}

TEST_F(StripCompatPerVertexMembersTest, TessellationStagesInputAndOutputBlocks) {
    ExpectSameAsCoreTwin({{GL_VERTEX_SHADER, Compat(kPositionOnlyVertexBody)},
                          {GL_TESS_CONTROL_SHADER, Compat(kTessControlBody)},
                          {GL_TESS_EVALUATION_SHADER, Compat(kTessEvalBody)},
                          {GL_FRAGMENT_SHADER, Compat(kWhiteFragmentBody)}},
                         {{GL_VERTEX_SHADER, Core(kPositionOnlyVertexBody)},
                          {GL_TESS_CONTROL_SHADER, Core(kTessControlBody)},
                          {GL_TESS_EVALUATION_SHADER, Core(kTessEvalBody)},
                          {GL_FRAGMENT_SHADER, Core(kWhiteFragmentBody)}});
}

TEST_F(StripCompatPerVertexMembersTest, ALegacyReadThatDiesInTheChainDoesNotPinTheBlock) {
    const auto compat = CompileProgram({{GL_VERTEX_SHADER, Compat(kPositionOnlyVertexBody)},
                                        {GL_GEOMETRY_SHADER, Compat(kDeadLegacyReadGeometryBody)},
                                        {GL_FRAGMENT_SHADER, Compat(kWhiteFragmentBody)}});
    ASSERT_EQ(compat.size(), 3u);
    EXPECT_EQ(ValidationError(compat[1]), "") << Disassemble(compat[1]);
}

TEST_F(StripCompatPerVertexMembersTest, AnAccessedLegacyMemberKeepsTheBlock) {
    const auto raw = CompileProgram(
        {{GL_VERTEX_SHADER, Compat(kFrontColorVertexBody)}, {GL_FRAGMENT_SHADER, Compat(kWhiteFragmentBody)}}, false);
    const auto compat = CompileProgram(
        {{GL_VERTEX_SHADER, Compat(kFrontColorVertexBody)}, {GL_FRAGMENT_SHADER, Compat(kWhiteFragmentBody)}});
    ASSERT_EQ(raw.size(), 2u);
    ASSERT_EQ(compat.size(), 2u);
    const String shape = PerVertexShape(Disassemble(compat[0]));
    EXPECT_EQ(shape, PerVertexShape(Disassemble(raw[0])));
    EXPECT_NE(shape.find("OpTypeStruct %v4float %float %_arr_float_uint_1 %v4float %v4float"), String::npos) << shape;
    // The ES hop still takes the module as it did before.
    EXPECT_NE(Transpile(compat[0]).find("gl_Position"), String::npos);
}

TEST_F(StripCompatPerVertexMembersTest, AWholeBlockLoadLeavesTheModuleUntouched) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    Vector<Uint32> module;
    ASSERT_TRUE(tools.Assemble(kWholeBlockLoadAsm, &module));
    EXPECT_EQ(RunPass(module), module);
}
