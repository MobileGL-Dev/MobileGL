// MobileGL - MobileGL/MG_Test/Pipeline/ViewportIndexReflectionTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ProgramFactory::ReflectedWritesViewportIndexBuiltin is the switch that decides whether a
// DirectVulkan pipeline declares one viewport or all sixteen. Getting it wrong is silent in both
// directions and neither direction is caught by a state test:
//
//   - a false NEGATIVE collapses every gl_ViewportIndex onto viewport 0, which is precisely the
//     bug the multi-viewport work exists to fix and which a set/get round trip cannot see;
//   - a false POSITIVE widens viewportCount for an ordinary Minecraft shader, costing a longer
//     vkCmdSetViewport per state change and, on a tiler, possibly a hardware fast path.
//
// So this compiles REAL GLSL through the same glslang path the renderer uses and reflects the
// SPIR-V that comes out, rather than asserting against hand-assembled words: what has to hold is
// that the detector agrees with what glslang actually emits for a shader that writes the builtin,
// including the stage-by-stage question of WHERE it may be written (GL 4.1 allows the geometry
// stage; ARB_shader_viewport_layer_array adds vertex and tessellation evaluation).
//
// The end-to-end claim - that a detected writer really does route pixels to its own viewport -
// lives in MG_IntegrationTest/Scenarios/ViewportArrayScenario.cpp.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"

#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv_reflect.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

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

    // Owns the reflection module so a failing EXPECT cannot leak it.
    class ReflectModule {
    public:
        explicit ReflectModule(const Vector<Uint32>& spirv) {
            if (spirv.empty()) return;
            m_created = spvReflectCreateShaderModule(spirv.size() * sizeof(Uint32), spirv.data(), &m_module) ==
                        SPV_REFLECT_RESULT_SUCCESS;
        }
        ~ReflectModule() {
            if (m_created) spvReflectDestroyShaderModule(&m_module);
        }
        ReflectModule(const ReflectModule&) = delete;
        ReflectModule& operator=(const ReflectModule&) = delete;

        Bool Created() const { return m_created; }
        const SpvReflectShaderModule& Get() const { return m_module; }

    private:
        SpvReflectShaderModule m_module{};
        Bool m_created = false;
    };

    class ViewportIndexReflectionTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };

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

    // Same stage, same shape, writing gl_Layer INSTEAD. Layered rendering and viewport routing
    // are different features and the detector must not confuse them: a Minecraft-style cubemap
    // pass writes gl_Layer and must keep the one-viewport pipeline.
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

    const char* const kPlainVertex = R"(#version 410 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

    const char* const kPlainFragment = R"(#version 410 core
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";

    TEST_F(ViewportIndexReflectionTest, TrueForAGeometryShaderThatAssignsViewportIndex) {
        const ReflectModule module(CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesViewportIndex));
        ASSERT_TRUE(module.Created());
        EXPECT_TRUE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(module.Get()))
            << "a shader that assigns gl_ViewportIndex must get a multi-viewport pipeline; missing it is what "
               "collapses every index onto viewport 0";
    }

    TEST_F(ViewportIndexReflectionTest, FalseForAGeometryShaderThatOnlyAssignsLayer) {
        const ReflectModule module(CompileToSpirv(GL_GEOMETRY_SHADER, kGeometryWritesLayerOnly));
        ASSERT_TRUE(module.Created());
        EXPECT_FALSE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(module.Get()))
            << "gl_Layer is layered rendering, not viewport routing; widening viewportCount for it costs the "
               "single-viewport fast path for nothing";
    }

    TEST_F(ViewportIndexReflectionTest, FalseForAPlainGeometryShader) {
        const ReflectModule module(CompileToSpirv(GL_GEOMETRY_SHADER, kPlainGeometry));
        ASSERT_TRUE(module.Created());
        EXPECT_FALSE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(module.Get()));
    }

    TEST_F(ViewportIndexReflectionTest, FalseForTheOrdinaryVertexAndFragmentStages) {
        // The shape every real application ships: neither stage may widen the pipeline.
        const ReflectModule vertexModule(CompileToSpirv(GL_VERTEX_SHADER, kPlainVertex));
        ASSERT_TRUE(vertexModule.Created());
        EXPECT_FALSE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(vertexModule.Get()));

        const ReflectModule fragmentModule(CompileToSpirv(GL_FRAGMENT_SHADER, kPlainFragment));
        ASSERT_TRUE(fragmentModule.Created());
        EXPECT_FALSE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(fragmentModule.Get()));
    }

    TEST_F(ViewportIndexReflectionTest, FalseForAnEmptyModuleWithoutDereferencing) {
        // A default-constructed module has no entry points. The scan runs on every link, so it
        // must survive a reflection that never got built rather than walk a null array.
        SpvReflectShaderModule emptyModule{};
        EXPECT_FALSE(ProgramFactory::ReflectedWritesViewportIndexBuiltin(emptyModule));
    }

} // namespace
