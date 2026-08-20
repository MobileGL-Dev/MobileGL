// MobileGL - MobileGL/MG_Test/ShaderTranspiler/LowerViewportIndexTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// LowerViewportIndexPass is the DirectGLES fallback for a driver with no GL_OES_viewport_array.
// The thing it prevents is not a wrong pixel but a missing program: ESSL has no core
// gl_ViewportIndex at any version, SPIRV-Cross prints the identifier bare, and the driver rejects
// the stage - after which DirectGLES binds program 0 and every draw renders nothing while
// GL_LINK_STATUS still answers TRUE. So what has to hold is textual and structural at once: the
// emitted ESSL must stop naming the builtin, the module must stay valid, and gl_Layer - which IS
// core in ESSL 3.20 geometry shaders - must come through untouched.
//
// Real GLSL through the same glslang path the backends use, rather than hand-assembled words, for
// the same reason MG_Test/Pipeline/ViewportIndexReflectionTest.cpp does it: what matters is what
// glslang actually emits for these shaders.

#include <gtest/gtest.h>

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

    // ESSL 320, i.e. exactly what the DirectGLES transpile asks SPIRV-Cross for.
    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    Bool Contains(const String& haystack, const String& needle) {
        return haystack.find(needle) != String::npos;
    }

    // KHR-GL4x.viewport_array.draw_to_single_layer_with_multiple_viewports' geometry stage in
    // miniature: sixteen invocations, each routing its primitive to its own viewport. This is the
    // shape that today loses the whole program on a driver without GL_OES_viewport_array.
    const char* const kGeometryWritesViewportIndex = R"(#version 410 core
layout(points, invocations = 16) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    gl_ViewportIndex = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

    // Layered rendering, not viewport routing. gl_Layer IS core in ESSL 3.20 geometry shaders, so
    // demoting it would break a Minecraft-style cubemap pass that works today.
    const char* const kGeometryWritesLayerOnly = R"(#version 410 core
layout(points, invocations = 6) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    gl_Layer = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

    // Both at once, which is the case that separates "lowers the right builtin" from "lowers every
    // builtin it can reach": KHR-GL4x.viewport_array.draw_multiple_layers writes both.
    const char* const kGeometryWritesBoth = R"(#version 410 core
layout(points, invocations = 16) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    gl_ViewportIndex = gl_InvocationID;
    gl_Layer = gl_InvocationID;
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";

    const char* const kPlainGeometry = R"(#version 410 core
layout(points, invocations = 1) in;
layout(triangle_strip, max_vertices = 4) out;
void main() {
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); EmitVertex();
    gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); EmitVertex();
    EndPrimitive();
}
)";
} // namespace

class LowerViewportIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        MobileGL::Initialize();
        m_validationFailuresAtStart = ShaderCompiler::SpirvValidationFailureCount();
    }

    void TearDown() override {
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), m_validationFailuresAtStart)
            << "the lowered module did not survive spirv-val";
    }

    Uint64 m_validationFailuresAtStart = 0;
};

// The probe is the gate that keeps every ordinary stage off an optimizer round trip, so it has to
// answer no for a shader that never routes a viewport - and yes for the one that does.
TEST_F(LowerViewportIndexTest, TheProbeAnswersOnlyForAViewportIndexWriter) {
    const Vector<Uint32> plain = CompileToSpirv(GL_GEOMETRY_SHADER, kPlainGeometry);
    ASSERT_FALSE(plain.empty());
    EXPECT_FALSE(ShaderCompiler::DeclaresViewportIndexBuiltin(plain));

    const Vector<Uint32> layerOnly = CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesLayerOnly);
    ASSERT_FALSE(layerOnly.empty());
    EXPECT_FALSE(ShaderCompiler::DeclaresViewportIndexBuiltin(layerOnly));

    const Vector<Uint32> writer = CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesViewportIndex);
    ASSERT_FALSE(writer.empty());
    EXPECT_TRUE(ShaderCompiler::DeclaresViewportIndexBuiltin(writer));

    // Runs on every stage of every program on a driver without the extension, so it must survive a
    // stage that produced no SPIR-V rather than pushing a parse diagnostic for it.
    EXPECT_FALSE(ShaderCompiler::DeclaresViewportIndexBuiltin({}));
}

// The whole point: the emitted ESSL must stop naming a builtin the language does not have.
TEST_F(LowerViewportIndexTest, DemotesTheBuiltinToAnOrdinaryGlobal) {
    const Vector<Uint32> input = CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesViewportIndex);
    ASSERT_FALSE(input.empty());

    // Negative control, and the bug itself: untouched, SPIRV-Cross prints gl_ViewportIndex into
    // ESSL 320 and asks for no extension to go with it.
    const String before = Transpile(input);
    EXPECT_TRUE(Contains(before, "gl_ViewportIndex")) << before;

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerViewportIndexForEssl(input, output, true));
    ASSERT_FALSE(output.empty());

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_FALSE(Contains(dis, "BuiltIn ViewportIndex")) << dis;
    EXPECT_TRUE(Contains(dis, "mg_ViewportIndex")) << dis;
    EXPECT_TRUE(Contains(dis, "Private")) << dis;

    const String after = Transpile(output);
    EXPECT_TRUE(Contains(after, "mg_ViewportIndex")) << after;
    EXPECT_FALSE(Contains(after, "gl_ViewportIndex")) << after;
}

// gl_Layer is core in ESSL 3.20 geometry shaders and layered rendering works on this backend
// today. Lowering it too would trade one silent failure for another.
TEST_F(LowerViewportIndexTest, LeavesGlLayerAlone) {
    const Vector<Uint32> input = CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesBoth);
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerViewportIndexForEssl(input, output, true));
    ASSERT_FALSE(output.empty());

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_FALSE(Contains(dis, "BuiltIn ViewportIndex")) << dis;
    EXPECT_TRUE(Contains(dis, "BuiltIn Layer")) << dis;

    const String after = Transpile(output);
    EXPECT_TRUE(Contains(after, "gl_Layer")) << after;
    EXPECT_FALSE(Contains(after, "gl_ViewportIndex")) << after;
}

// Every other stage on a driver without the extension goes through this pass too (behind the
// probe), so a module it has nothing to do with must come out saying exactly what it said.
TEST_F(LowerViewportIndexTest, LeavesAModuleWithoutTheBuiltinUntouched) {
    const Vector<Uint32> input = CompileToSpirv(GL_GEOMETRY_SHADER, kPlainGeometry);
    ASSERT_FALSE(input.empty());

    const String before = Transpile(input);

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerViewportIndexForEssl(input, output, true));
    ASSERT_FALSE(output.empty());

    const String dis = Disassemble(output);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    ASSERT_TRUE(tools.Validate(output)) << dis;
    EXPECT_FALSE(Contains(dis, "mg_ViewportIndex")) << dis;
    EXPECT_EQ(Transpile(output), before);
}
