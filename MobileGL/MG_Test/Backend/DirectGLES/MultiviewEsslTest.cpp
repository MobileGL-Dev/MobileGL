// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/MultiviewEsslTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// GL_OVR_multiview through the steps DirectGLES takes: glslang keeps the vertex stage's num_views,
// the SPIR-V loses its MultiView capability so SPIRV-Cross accepts both stages without a view
// count, and the text pass puts the directive and num_views back. The result has to be ESSL 3.20
// that a compiler accepts, in both stages.

#include <gtest/gtest.h>

#include <MG_Backend/DirectGLES/DirectGLES.h>
#include <MG_Backend/DirectGLES/Utils.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "Init.h"

#include <glslang/MachineIndependent/localintermediate.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectGLES::PrgramImpl::RequestMultiviewExtension;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    constexpr const char* kVertex = R"(#version 330 core
#extension GL_OVR_multiview2 : require
layout(num_views = 2) in;
layout(location = 0) in vec2 pos;
out float tint;
void main() {
    tint = float(gl_ViewID_OVR);
    gl_Position = vec4(pos.x + float(gl_ViewID_OVR), pos.y, 0.0, 1.0);
}
)";
    constexpr const char* kFragment = R"(#version 330 core
#extension GL_OVR_multiview2 : require
in float tint;
out vec4 color;
void main() { color = vec4(tint, float(gl_ViewID_OVR), 0.0, 1.0); }
)";

    struct LinkedModules {
        Int numViews = -1;
        Vector<Vector<Uint32>> modules;
    };

    LinkedModules CompileAndLink(const char* vertex, const char* fragment) {
        using namespace MG_Util::ShaderTranspiler;
        LinkedModules out;
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] :
             Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}}) {
            ShaderAttrib attrib{.shaderType = stage, .sourceStr = source};
            auto shader = ShaderCompiler::CompileShader(attrib);
            EXPECT_TRUE(shader) << (shader ? String{} : shader.error().log);
            if (!shader) return out;
            shaders.push_back(shader.value());
        }
        ProgramAttrib programAttrib{.shaders = shaders};
        auto program = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(program) << (program ? String{} : program.error().log);
        if (!program) return out;
        out.numViews = program.value()->getIntermediate(EShLangVertex)->getNumViews();
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
                                         .program = *program.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binary) << (binary ? String{} : binary.error().log);
        if (!binary || binary->size() != 2) return out;
        for (auto module : binary.value()) {
            EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
            out.modules.push_back(Move(module));
        }
        return out;
    }

    String EmitEssl320(const Vector<Uint32>& spirv) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        session.CreateOptions(&options);
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        session.SetOptions(options);
        const char* result = nullptr;
        session.Compile(&result);
        EXPECT_NE(result, nullptr) << (session.GetLastErrorString() ? session.GetLastErrorString() : "");
        return result != nullptr ? String(result) : String{};
    }

    String CompileErrorAsEssl320(const String& source, EShLanguage stage) {
        glslang::TShader shader(stage);
        const char* strings[] = {source.c_str()};
        shader.setStrings(strings, 1);
        if (shader.parse(GetDefaultResources(), 320, EEsProfile, false, false, EShMsgDefault)) return {};
        return shader.getInfoLog();
    }

    Bool DeclaresMultiviewCapability(const Vector<Uint32>& module) {
        constexpr Uint32 kOpCapability = 17;
        constexpr Uint32 kCapabilityMultiView = 4439;
        for (SizeT at = 5; at < module.size();) {
            const Uint32 wordCount = module[at] >> 16;
            if (wordCount == 0) return false;
            if ((module[at] & 0xFFFFu) == kOpCapability && module[at + 1] == kCapabilityMultiView) return true;
            at += wordCount;
        }
        return false;
    }
} // namespace

TEST(MultiviewEssl, BothStagesComeOutAsMultiviewEsslACompilerAccepts) {
    MobileGL::Initialize();
    const LinkedModules linked = CompileAndLink(kVertex, kFragment);
    ASSERT_EQ(linked.modules.size(), 2u);
    EXPECT_EQ(linked.numViews, 2) << "glslang dropped the vertex stage's num_views";

    const struct {
        GLenum glStage;
        EShLanguage stage;
    } stages[] = {{GL_VERTEX_SHADER, EShLangVertex}, {GL_FRAGMENT_SHADER, EShLangFragment}};
    for (SizeT i = 0; i < 2; ++i) {
        ASSERT_TRUE(DeclaresMultiviewCapability(linked.modules[i]));
        Vector<Uint32> stripped;
        ASSERT_TRUE(ShaderCompiler::StripMultiviewCapabilityForEssl(linked.modules[i], stripped));
        EXPECT_FALSE(DeclaresMultiviewCapability(stripped));

        const String emitted = EmitEssl320(stripped);
        ASSERT_FALSE(emitted.empty());
        EXPECT_NE(emitted.find("gl_ViewID_OVR"), String::npos) << emitted;
        EXPECT_EQ(emitted.find("GL_OVR_multiview"), String::npos) << "the emission must not depend on the view count";

        const String essl = RequestMultiviewExtension(emitted, stages[i].glStage, linked.numViews, true);
        EXPECT_NE(essl.find("#extension GL_OVR_multiview2 : require\n"), String::npos) << essl;
        const Bool vertex = stages[i].glStage == GL_VERTEX_SHADER;
        EXPECT_EQ(essl.find("layout(num_views = 2) in;") != String::npos, vertex) << essl;
        EXPECT_EQ(CompileErrorAsEssl320(essl, stages[i].stage), String{}) << essl;
    }
}

TEST(MultiviewEssl, TheDirectiveFollowsTheDriversExtensionAndNumViewsFollowsTheDirectives) {
    const String source = "#version 320 es\n"
                          "#extension GL_EXT_clip_cull_distance : require\n"
                          "\n"
                          "precision highp float;\n"
                          "void main() { gl_Position = vec4(float(gl_ViewID_OVR)); }\n";
    const String essl = RequestMultiviewExtension(source, GL_VERTEX_SHADER, 3, false);
    EXPECT_EQ(essl, "#version 320 es\n"
                    "#extension GL_OVR_multiview : require\n"
                    "#extension GL_EXT_clip_cull_distance : require\n"
                    "layout(num_views = 3) in;\n"
                    "\n"
                    "precision highp float;\n"
                    "void main() { gl_Position = vec4(float(gl_ViewID_OVR)); }\n");

    // Neither the built-in nor a view count: nothing to ask for.
    const String plain = "#version 320 es\nvoid main() {}\n";
    EXPECT_EQ(RequestMultiviewExtension(plain, GL_VERTEX_SHADER, 0, true), plain);
    EXPECT_EQ(RequestMultiviewExtension(plain, GL_FRAGMENT_SHADER, 2, true), plain);
}

TEST(MultiviewEssl, AModuleWithoutMultiviewIsLeftAlone) {
    MobileGL::Initialize();
    const LinkedModules linked = CompileAndLink("#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n",
                                                "#version 330 core\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n");
    ASSERT_EQ(linked.modules.size(), 2u);
    EXPECT_LT(linked.numViews, 0) << "no num_views qualifier was declared";
    Vector<Uint32> stripped;
    EXPECT_FALSE(ShaderCompiler::StripMultiviewCapabilityForEssl(linked.modules[0], stripped));
    EXPECT_TRUE(stripped.empty());
}

TEST(MultiviewEssl, NumViewsIsAVertexStageQualifierAboveZero) {
    MobileGL::Initialize();
    using namespace MG_Util::ShaderTranspiler;
    for (const auto& [stage, source] : Vector<Pair<GLenum, const char*>>{
             {GL_VERTEX_SHADER, "#version 330 core\n#extension GL_OVR_multiview : require\n"
                                "layout(num_views = 0) in;\nvoid main() { gl_Position = vec4(0.0); }\n"},
             {GL_FRAGMENT_SHADER, "#version 330 core\n#extension GL_OVR_multiview : require\n"
                                  "layout(num_views = 2) in;\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n"},
             {GL_VERTEX_SHADER, "#version 330 core\n#extension GL_OVR_multiview : require\n"
                                "layout(num_views = 2) in;\nlayout(num_views = 3) in;\n"
                                "void main() { gl_Position = vec4(0.0); }\n"},
         }) {
        ShaderAttrib attrib{.shaderType = stage, .sourceStr = source};
        EXPECT_FALSE(ShaderCompiler::CompileShader(attrib)) << source;
    }
}
