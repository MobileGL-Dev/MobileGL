// MobileGL - MobileGL/MG_Test/ShaderTranspiler/ShadingRateEsslTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// GL_EXT_fragment_shading_rate's built-ins on the ESSL leg. glslang lowers gl_ShadingRateEXT and
// gl_PrimitiveShadingRateEXT to the ShadingRateKHR / PrimitiveShadingRateKHR built-ins, which
// SPIRV-Cross spells only for Vulkan GLSL - for ESSL it refused the whole stage. SpvcSession's
// Prepare/FinishShadingRateBuiltinsForEssl pair carries them through under a sentinel name and
// puts the ESSL built-in back; these cases pin the emitted text.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    Vector<Vector<Uint32>> CompileProgramToSpirv(const Vector<Pair<GLenum, const char*>>& stages) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        Vector<GLenum> types;
        // The frontend's own source pass first: it is what retargets the ES sources the suite
        // submits to the desktop profile glslang compiles them under.
        Vector<String> sources;
        sources.reserve(stages.size());
        for (const auto& [stage, source] : stages) {
            sources.emplace_back(source);
            PreprocessShaderSource(stage == GL_VERTEX_SHADER ? ShaderStage::Vertex : ShaderStage::Fragment,
                                   sources.back());
            ShaderAttrib shaderAttrib{.shaderType = stage, .sourceStr = sources.back()};
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

    String Transpile(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        EXPECT_TRUE(essl) << (essl ? String{} : essl.error().log);
        return essl ? essl.value() : String{};
    }

    // The KHR-GL4x.fragment_shading_rate.complex.* pair, primitive variant.
    constexpr const char* kVertex = "#version 310 es\n"
                                    "#extension GL_EXT_fragment_shading_rate : enable\n"
                                    "precision highp float;\n"
                                    "precision highp int;\n"
                                    "layout(location = 0) in vec4 position;\n"
                                    "uniform int primShadingRate;\n"
                                    "void main() {\n"
                                    "    gl_Position = position;\n"
                                    "    gl_PrimitiveShadingRateEXT = primShadingRate;\n"
                                    "}\n";
    constexpr const char* kFragment = "#version 310 es\n"
                                      "#extension GL_EXT_fragment_shading_rate : enable\n"
                                      "precision highp float;\n"
                                      "precision highp int;\n"
                                      "layout(location = 0) out ivec4 color0;\n"
                                      "uniform int shadingRate;\n"
                                      "void main() {\n"
                                      "    color0.x = gl_ShadingRateEXT;\n"
                                      "    color0.w = gl_ShadingRateEXT != shadingRate ? 1 : 0;\n"
                                      "}\n";
} // namespace

class ShadingRateEsslTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
};

TEST_F(ShadingRateEsslTest, BothBuiltInsReachTheEsslUnderTheirExtensionNames) {
    const auto modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertex}, {GL_FRAGMENT_SHADER, kFragment}});
    ASSERT_EQ(modules.size(), 2u);

    for (SizeT i = 0; i < modules.size(); ++i) {
        const String essl = Transpile(modules[i]);
        ASSERT_FALSE(essl.empty()) << "stage " << i << " was refused";
        const char* builtin = i == 0 ? "gl_PrimitiveShadingRateEXT" : "gl_ShadingRateEXT";
        EXPECT_NE(essl.find(builtin), String::npos) << essl;
        EXPECT_NE(essl.find("#extension GL_EXT_fragment_shading_rate : require"), String::npos) << essl;
        EXPECT_EQ(essl.find("mgl_sentinel"), String::npos) << "a sentinel survived:\n" << essl;
        // The built-in is the driver's; redeclaring it as an interface variable is illegal ESSL.
        EXPECT_EQ(essl.find(String("int ") + builtin + ";"), String::npos) << essl;
    }
}

TEST_F(ShadingRateEsslTest, AStageWithoutTheBuiltInsIsLeftAlone) {
    constexpr const char* kPlainFragment = "#version 310 es\n"
                                           "precision highp float;\n"
                                           "layout(location = 0) out vec4 color0;\n"
                                           "void main() { color0 = vec4(1.0); }\n";
    const auto modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, kVertex}, {GL_FRAGMENT_SHADER, kPlainFragment}});
    ASSERT_EQ(modules.size(), 2u);
    const String essl = Transpile(modules[1]);
    ASSERT_FALSE(essl.empty());
    EXPECT_EQ(essl.find("GL_EXT_fragment_shading_rate"), String::npos) << essl;
}

TEST(ShadingRateEsslFixupTest, OnlyTheDeclarationIsDroppedAndOnlyWholeIdentifiersAreRenamed) {
    String essl = "#version 320 es\n"
                  "flat in highp int mgl_sentinel_ShadingRateEXT;\n"
                  "layout(location = 3) flat in highp int mgl_sentinel_ShadingRateEXT;\n"
                  "int mgl_sentinel_ShadingRateEXTx = 1;\n"
                  "void main() {\n"
                  "    int a = mgl_sentinel_ShadingRateEXT;\n"
                  "    a += (mgl_sentinel_ShadingRateEXT & 3);\n"
                  "}\n";
    SpvcSession::FinishShadingRateBuiltinsForEssl(essl);
    EXPECT_EQ(essl, "#version 320 es\n"
                    "int mgl_sentinel_ShadingRateEXTx = 1;\n"
                    "void main() {\n"
                    "    int a = gl_ShadingRateEXT;\n"
                    "    a += (gl_ShadingRateEXT & 3);\n"
                    "}\n");
}
