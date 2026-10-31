// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/NarrowIntegerEsslTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// A vertex stage armed with the narrow-integer seeds, through the same steps the DirectGLES build
// takes: the array-input split, the RuntimeWidths pass, ESSL 3.00 emission and the uniform
// promotion. The two widths have to come out as `uniform highp uint` (anything else is read
// uninitialized or at too low a precision) and the result has to be ESSL 3.00 a compiler accepts.
// The emission here uses plain ESSL 3.00 options, not the build's whole pass chain; the
// integration lanes run that chain (NarrowIntegerAttributeScenario).

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Managers.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "Init.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectGLES::PromoteDrawParameterGlobalsToUniforms;
using MobileGL::MG_Backend::DirectGLES::PromoteNarrowIntegerWidthGlobals;
using MobileGL::MG_Util::ShaderTranspiler::kNarrowIntegerWidths16GlobalName;
using MobileGL::MG_Util::ShaderTranspiler::kNarrowIntegerWidths8GlobalName;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec4 lightAndData;
layout(location = 2) in uvec2 texc;
layout(location = 3) in int packed[2];
flat out ivec4 a;
flat out uvec4 b;
void main() {
    a = lightAndData + ivec4(packed[0], packed[1], 0, 0);
    b = uvec4(texc, 0u, 0u);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";
    constexpr const char* kFragment = R"(#version 330 core
flat in ivec4 a;
flat in uvec4 b;
out vec4 color;
void main() { color = vec4(a) + vec4(b); }
)";

    Vector<Uint32> CompileVertexModule() {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] :
             Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, kVertex}, {GL_FRAGMENT_SHADER, kFragment}}) {
            ShaderAttrib attrib{.shaderType = stage, .sourceStr = source};
            auto shader = ShaderCompiler::CompileShader(attrib);
            EXPECT_TRUE(shader) << (shader ? String{} : shader.error().log);
            if (!shader) return {};
            shaders.push_back(shader.value());
        }
        ProgramAttrib programAttrib{.shaders = shaders};
        auto program = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(program) << (program ? String{} : program.error().log);
        if (!program) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
                                         .program = *program.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binary) << (binary ? String{} : binary.error().log);
        if (!binary || binary->size() != 2) return {};
        Vector<Uint32> module = binary.value()[0];
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        return module;
    }

    String EmitEssl300(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        session.CreateOptions(&options);
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 300);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        session.SetOptions(options);
        const char* result = nullptr;
        session.Compile(&result);
        EXPECT_NE(result, nullptr) << (session.GetLastErrorString() ? session.GetLastErrorString() : "");
        return result != nullptr ? String(result) : String{};
    }

    // Declarations of `name` as a uint, whatever their qualifiers or initializer.
    SizeT CountUintDeclarations(const String& source, const char* name) {
        SizeT count = 0;
        for (const char* suffix : {";", " ="}) {
            const String declaration = String("uint ") + name + suffix;
            for (SizeT pos = source.find(declaration); pos != String::npos; pos = source.find(declaration, pos + 1))
                ++count;
        }
        return count;
    }

    String CompileErrorAsEssl300Vertex(const String& source) {
        glslang::TShader shader(EShLangVertex);
        const char* strings[] = {source.c_str()};
        shader.setStrings(strings, 1);
        if (shader.parse(GetDefaultResources(), 300, EEsProfile, false, false, EShMsgDefault)) return {};
        return shader.getInfoLog();
    }
} // namespace

TEST(NarrowIntegerEssl, ArmedVertexStageDeclaresBothWidthsAsHighpUniformsInValidEssl300) {
    MobileGL::Initialize();
    const Vector<Uint32> module = CompileVertexModule();
    ASSERT_FALSE(module.empty());

    Vector<Uint32> split;
    ASSERT_TRUE(ShaderCompiler::SplitArrayVertexInputsForEssl(module, split, true));
    Vector<Uint32> seeded;
    Bool changed = false;
    ASSERT_TRUE(
        ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(split.empty() ? module : split, seeded, changed, true));
    ASSERT_TRUE(changed);

    const String emitted = EmitEssl300(seeded);
    ASSERT_FALSE(emitted.empty());
    const String essl = PromoteDrawParameterGlobalsToUniforms(emitted, GL_VERTEX_SHADER);
    for (const char* name : {kNarrowIntegerWidths8GlobalName, kNarrowIntegerWidths16GlobalName}) {
        EXPECT_NE(essl.find(String("uniform highp uint ") + name + ";"), String::npos) << name << "\n" << essl;
        // Exactly one declaration, and it is the uniform.
        EXPECT_EQ(CountUintDeclarations(essl, name), 1u) << name << "\n" << essl;
    }
    for (const char* bitfield : {"bitfieldExtract", "bitfieldInsert"})
        EXPECT_EQ(essl.find(bitfield), String::npos) << essl;
    EXPECT_EQ(CompileErrorAsEssl300Vertex(essl), String{}) << essl;
}

TEST(NarrowIntegerEssl, OnlyALineStartingDeclarationIsPromoted) {
    const String promoted = PromoteNarrowIntegerWidthGlobals("#version 300 es\n"
                                                             "uint mg_NarrowIntegerWidths8 = 0u;\n"
                                                             "highp uint mg_NarrowIntegerWidths16;\n"
                                                             "void main() { uint x = mg_NarrowIntegerWidths8; }\n");
    EXPECT_EQ(promoted, "#version 300 es\n"
                        "uniform highp uint mg_NarrowIntegerWidths8;\n"
                        "uniform highp uint mg_NarrowIntegerWidths16;\n"
                        "void main() { uint x = mg_NarrowIntegerWidths8; }\n");
    // Already a uniform: untouched, and a mediump spelling is promoted to highp all the same.
    EXPECT_EQ(PromoteNarrowIntegerWidthGlobals(promoted), promoted);
    EXPECT_EQ(PromoteNarrowIntegerWidthGlobals("mediump uint mg_NarrowIntegerWidths16;\n"),
              "uniform highp uint mg_NarrowIntegerWidths16;\n");
}
