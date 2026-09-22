// MobileGL - MobileGL/MG_Test/SelfTest/BakedInternalShadersTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// MOBILEGL_BAKED_INTERNAL_SHADERS - the freshness gate (CONTRACT-P7 §5.1 (D)).
//
// WHAT A BAKED SPIR-V HEADER ACTUALLY IS: a compiler output committed to the tree, with its
// input committed beside it and NOTHING connecting the two. Edit WireColorBlit.frag and the
// build stays green, the lane stays green, and the shader that runs is the one from before
// the edit - silently, on every device, until somebody reads the words. The tree already had
// three such pairs before this package added a fourth, and one of them (the iterationRP
// witness) was measurably stale: its own header said so in prose, which is exactly as much
// enforcement as prose provides.
//
// So every baked header is recompiled here, from its committed source, with the IN-TREE
// glslang, and compared word for word.
//
// THIS TEST IS THE BAKER, NOT A SECOND OPINION. scripts/bake_internal_shaders.py does not
// compile anything: it runs this binary with MOBILEGL_BAKE_INTERNAL_SHADERS set, takes the
// words it emits, and splices them into the headers. That direction is deliberate. A gate
// that compiles through the glslang LIBRARY while the baker shells out to a glslangValidator
// BINARY has two compilers and no rule about which one is right, and the day their defaults
// diverge - an option the CLI passes by default, a version skew between a system tool and
// the submodule - the gate reds on a header nobody can regenerate. One compiler, one truth,
// and the gate's own output is what the tree stores.
//
// SOURCE PATH: the sources live in the source tree, not next to the binary, so the build
// hands their root in as MOBILEGL_BAKED_SHADER_ROOT. A test that cannot find them FAILS
// rather than skipping: a freshness gate that silently disarms is the thing it exists
// against.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <MG_Util/SelfTest/DriverPostIterationRPWitnessSpv.h>
#include <MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbeSpv.h>

namespace {

// The two Renderer headers are written to be included inside the backend namespace, after
// the project's own type aliases; they name Uint32 and declare nothing else. Including them
// here with a local alias keeps them byte-identical to what VulkanRenderer.cpp compiles -
// the alternative, a copy of the words in this file, is precisely the drift this gate is for.
using Uint32 = std::uint32_t;
#include <MG_Backend/DirectVulkan/Renderer/WireColorBlitSpirv.h>
#include <MG_Backend/DirectVulkan/Renderer/WireDepthMipmapSpirv.h>
#include <MG_Backend/DirectVulkan/Renderer/WireMultisampleResolveSpirv.h>

struct BakedShader {
    const char* name;            // ctest entry name, and the bake file's stem
    const char* source;          // repository-relative GLSL source
    const char* header;          // repository-relative header the words live in
    const char* symbol;          // the array identifier inside that header
    EShLanguage stage;
    glslang::EShTargetClientVersion client;
    glslang::EShTargetLanguageVersion spv;
    const std::uint32_t* words;
    std::size_t wordCount;
};

#define MGL_BAKED_ROW(name, source, header, symbol, stage, client, spv, array)              \
    BakedShader{name, source, header, symbol, stage, client, spv, (array),                  \
                sizeof(array) / sizeof((array)[0])}

const BakedShader kBakedShaders[] = {
    // The Magma wire arms' own programs: no GL program object, no registry entry, no runtime
    // transpilation, so the words in the header are the only copy that exists.
    MGL_BAKED_ROW("WireColorBlitVertex",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlit.vert",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlitSpirv.h",
                  "kWireColorBlitVertexSpirv", EShLangVertex,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireColorBlitVertexSpirv),
    MGL_BAKED_ROW("WireColorBlitFragment",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlit.frag",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireColorBlitSpirv.h",
                  "kWireColorBlitFragmentSpirv", EShLangFragment,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireColorBlitFragmentSpirv),
    MGL_BAKED_ROW("WireDepthMipmapVertex",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthMipmap.vert",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthMipmapSpirv.h",
                  "kWireDepthMipmapVertexSpirv", EShLangVertex,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireDepthMipmapVertexSpirv),
    MGL_BAKED_ROW("WireDepthMipmapFragment",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthMipmap.frag",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthMipmapSpirv.h",
                  "kWireDepthMipmapFragmentSpirv", EShLangFragment,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireDepthMipmapFragmentSpirv),
    // P7 wave 2-B2: the multisample depth/stencil resolve's own pass. THREE ROWS FOR TWO
    // PIPELINES - one vertex stage shared by both, and a fragment stage per aspect, because a
    // fragment shader cannot write the stencil aspect without VK_EXT_shader_stencil_export and
    // a device without that extension must still be able to compile the depth half.
    MGL_BAKED_ROW("WireMultisampleResolveVertex",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleResolve.vert",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleResolveSpirv.h",
                  "kWireMultisampleResolveVertexSpirv", EShLangVertex,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireMultisampleResolveVertexSpirv),
    MGL_BAKED_ROW("WireMultisampleDepthResolveFragment",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleDepthResolve.frag",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleResolveSpirv.h",
                  "kWireMultisampleDepthResolveFragmentSpirv", EShLangFragment,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireMultisampleDepthResolveFragmentSpirv),
    MGL_BAKED_ROW("WireMultisampleStencilResolveFragment",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleStencilResolve.frag",
                  "MobileGL/MG_Backend/DirectVulkan/Renderer/WireMultisampleResolveSpirv.h",
                  "kWireMultisampleStencilResolveFragmentSpirv", EShLangFragment,
                  glslang::EShTargetVulkan_1_0, glslang::EShTargetSpv_1_0,
                  kWireMultisampleStencilResolveFragmentSpirv),
    // The two driver self-test probes. They are not Magma's, and they are in this table for
    // the reason the contract names them: they are baked, so they can be stale, and one of
    // them was.
    MGL_BAKED_ROW("DriverPostIterationRPWitness",
                  "MobileGL/MG_Util/SelfTest/DriverPostIterationRPWitness.comp",
                  "MobileGL/MG_Util/SelfTest/DriverPostIterationRPWitnessSpv.h",
                  "kDriverPostIterationRPWitnessSpv", EShLangCompute,
                  glslang::EShTargetVulkan_1_1, glslang::EShTargetSpv_1_3,
                  MobileGL::MG_Util::SelfTest::kDriverPostIterationRPWitnessSpv),
    MGL_BAKED_ROW("PrimitivesGeneratedNoXfbProbeVert",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.vert",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbeSpv.h",
                  "kPrimitivesGeneratedNoXfbProbeVertSpv", EShLangVertex,
                  glslang::EShTargetVulkan_1_1, glslang::EShTargetSpv_1_3,
                  MobileGL::MG_Util::SelfTest::kPrimitivesGeneratedNoXfbProbeVertSpv),
    MGL_BAKED_ROW("PrimitivesGeneratedNoXfbProbeTesc",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.tesc",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbeSpv.h",
                  "kPrimitivesGeneratedNoXfbProbeTescSpv", EShLangTessControl,
                  glslang::EShTargetVulkan_1_1, glslang::EShTargetSpv_1_3,
                  MobileGL::MG_Util::SelfTest::kPrimitivesGeneratedNoXfbProbeTescSpv),
    MGL_BAKED_ROW("PrimitivesGeneratedNoXfbProbeTese",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbe.tese",
                  "MobileGL/MG_Util/SelfTest/PrimitivesGeneratedNoXfbProbeSpv.h",
                  "kPrimitivesGeneratedNoXfbProbeTeseSpv", EShLangTessEvaluation,
                  glslang::EShTargetVulkan_1_1, glslang::EShTargetSpv_1_3,
                  MobileGL::MG_Util::SelfTest::kPrimitivesGeneratedNoXfbProbeTeseSpv),
};

#undef MGL_BAKED_ROW

std::string RepositoryRoot() {
#ifdef MOBILEGL_BAKED_SHADER_ROOT
    return std::string(MOBILEGL_BAKED_SHADER_ROOT);
#else
    return std::string();
#endif
}

// glslang::InitializeProcess is process-wide and refcounted. One call for the whole binary:
// this test never runs MobileGL::Initialize, which is the other caller.
void EnsureGlslangProcess() {
    static const bool ready = glslang::InitializeProcess();
    ASSERT_TRUE(ready) << "glslang::InitializeProcess failed";
}

class BakedInternalShaders : public ::testing::TestWithParam<BakedShader> {};

TEST_P(BakedInternalShaders, HeaderMatchesItsSource) {
    const BakedShader& row = GetParam();
    ASSERT_NO_FATAL_FAILURE(EnsureGlslangProcess());

    const std::string root = RepositoryRoot();
    ASSERT_FALSE(root.empty())
        << "MOBILEGL_BAKED_SHADER_ROOT was not defined for this target: the gate cannot find "
           "the GLSL sources and must not pass by default";
    const std::string sourcePath = root + "/" + row.source;
    std::ifstream in(sourcePath, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot open " << sourcePath;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();
    ASSERT_FALSE(source.empty()) << sourcePath << " is empty";

    glslang::TShader shader(row.stage);
    const char* strings[] = {source.c_str()};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, row.stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, row.client);
    shader.setEnvTarget(glslang::EShTargetSpv, row.spv);
    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    ASSERT_TRUE(shader.parse(GetDefaultResources(), 100, false, messages))
        << row.source << " does not compile:\n" << shader.getInfoLog();

    glslang::TProgram program;
    program.addShader(&shader);
    ASSERT_TRUE(program.link(messages)) << row.source << " does not link:\n" << program.getInfoLog();
    glslang::TIntermediate* intermediate = program.getIntermediate(row.stage);
    ASSERT_NE(intermediate, nullptr);

    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*intermediate, spirv, &logger, &options);
    ASSERT_FALSE(spirv.empty()) << "GlslangToSpv produced nothing for " << row.source;

    // Bake mode writes what it computed and asserts nothing else: the script that sets this
    // variable is about to overwrite the header, so a red here would only stop it doing so.
    if (const char* bakeDir = std::getenv("MOBILEGL_BAKE_INTERNAL_SHADERS")) {
        const std::string out = std::string(bakeDir) + "/" + row.symbol + ".words";
        std::ofstream sink(out, std::ios::binary);
        ASSERT_TRUE(sink.good()) << "cannot write " << out;
        sink << row.header << "\n";
        for (unsigned int word : spirv) {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08x\n", word);
            sink << text;
        }
        ASSERT_TRUE(sink.good()) << "failed writing " << out;
        return;
    }

    ASSERT_EQ(spirv.size(), row.wordCount)
        << row.header << ": " << row.symbol << " has " << row.wordCount << " words, recompiling "
        << row.source << " with the in-tree glslang produces " << spirv.size()
        << ". Regenerate with scripts/bake_internal_shaders.py.";
    for (std::size_t at = 0; at < spirv.size(); ++at) {
        ASSERT_EQ(spirv[at], row.words[at])
            << row.header << ": " << row.symbol << " word " << at << " is stale against "
            << row.source << ". Regenerate with scripts/bake_internal_shaders.py.";
    }
}

INSTANTIATE_TEST_SUITE_P(BakedShaders, BakedInternalShaders,
                         ::testing::ValuesIn(kBakedShaders),
                         [](const ::testing::TestParamInfo<BakedShader>& info) {
                             return std::string(info.param.name);
                         });

} // namespace
