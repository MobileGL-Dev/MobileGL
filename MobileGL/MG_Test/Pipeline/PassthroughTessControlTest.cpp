// MobileGL - MobileGL/MG_Test/Pipeline/PassthroughTessControlTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"

#include <map>
#include <set>
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::ProgramFactory;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    // A test-side SPIR-V walker, deliberately independent of the production reflection: the
    // generator's contract with the evaluation stage is "declare this many output vertices and
    // write these built-ins", and that has to be readable off the module itself.
    constexpr Uint32 kSpirvHeaderWordCount = 5;
    constexpr Uint32 kOpExecutionMode = 16;
    constexpr Uint32 kOpDecorate = 71;
    constexpr Uint32 kOpMemberDecorate = 72;
    constexpr Uint32 kExecutionModeOutputVertices = 26;
    constexpr Uint32 kDecorationBuiltIn = 11;

    // SpvBuiltIn values used below.
    constexpr Uint32 kBuiltInPosition = 0;
    constexpr Uint32 kBuiltInInvocationId = 8;
    constexpr Uint32 kBuiltInTessLevelOuter = 11;
    constexpr Uint32 kBuiltInTessLevelInner = 12;

    template <typename Visitor>
    void ForEachInstruction(const Vector<Uint32>& spirv, Visitor&& visit) {
        for (SizeT i = kSpirvHeaderWordCount; i < spirv.size();) {
            const Uint32 wordCount = spirv[i] >> 16;
            const Uint32 opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            visit(opcode, &spirv[i], wordCount);
            i += wordCount;
        }
    }

    // -1 when the module declares no OutputVertices mode at all, which is itself a failure the
    // tests want to see named rather than silently compared against a wrong number.
    Int DeclaredOutputVertices(const Vector<Uint32>& spirv) {
        Int declared = -1;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpExecutionMode && wordCount >= 4 && words[2] == kExecutionModeOutputVertices) {
                declared = static_cast<Int>(words[3]);
            }
        });
        return declared;
    }

    // The built-in members of every block in the module, keyed by the struct's result id, in
    // member order. A gl_PerVertex is exactly such a struct, and its member list IS the shape the
    // neighbouring stage has to agree with.
    constexpr Uint32 kOpTypeStruct = 30;

    std::map<Uint32, Vector<Uint32>> BuiltInBlockShapes(const Vector<Uint32>& spirv) {
        std::map<Uint32, Vector<Uint32>> shapes;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpMemberDecorate && wordCount >= 5 && words[3] == kDecorationBuiltIn) {
                shapes[words[1]].push_back(words[4]);
            }
        });
        return shapes;
    }

    // Member count of a struct type, so a shape comparison can also catch a block that grew a
    // NON-built-in member (which the decoration walk above would not see).
    Uint32 StructMemberCount(const Vector<Uint32>& spirv, Uint32 structId) {
        Uint32 count = 0;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpTypeStruct && wordCount >= 2 && words[1] == structId) {
                count = wordCount - 2;
            }
        });
        return count;
    }

    std::set<Uint32> DeclaredBuiltIns(const Vector<Uint32>& spirv) {
        std::set<Uint32> builtIns;
        ForEachInstruction(spirv, [&](Uint32 opcode, const Uint32* words, Uint32 wordCount) {
            if (opcode == kOpDecorate && wordCount >= 4 && words[2] == kDecorationBuiltIn) {
                builtIns.insert(words[3]);
            }
            if (opcode == kOpMemberDecorate && wordCount >= 5 && words[3] == kDecorationBuiltIn) {
                builtIns.insert(words[4]);
            }
        });
        return builtIns;
    }

    const FloatVec4 kDefaultOuter(1.0f, 1.0f, 1.0f, 1.0f);
    const FloatVec2 kDefaultInner(1.0f, 1.0f);

    Vector<Uint32> CompileGeneratedSource(Uint32 patchVertices) {
        using namespace MG_Util::ShaderTranspiler;
        const String source =
            ProgramFactory::BuildPassthroughTessControlSource(patchVertices, kDefaultOuter, kDefaultInner);

        ShaderAttrib shaderAttrib{.shaderType = GL_TESS_CONTROL_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log) << "\n" << source;
        if (!shaderResult) return {};

        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        EXPECT_TRUE(programResult) << (programResult ? String{} : programResult.error().log);
        if (!programResult) return {};

        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_TESS_CONTROL_SHADER},
                                         .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binaryResult) << (binaryResult ? String{} : binaryResult.error().log);
        if (!binaryResult || binaryResult->empty()) return {};
        return binaryResult->front();
    }
} // namespace

class PassthroughTessControlTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }
};

// The whole reason this stage is generated per patch size rather than once: GL takes the output
// patch size from PATCH_VERTICES, which is draw state. A program that links at the default 3 and
// draws at 4 - which is exactly what
// KHR-GL43.shader_storage_buffer_object.advanced-write-tessellation does - must get a stage built
// for 4, or its evaluation stage reads gl_in[3] out of a three-element array.
TEST_F(PassthroughTessControlTest, DeclaresTheRequestedPatchSize) {
    for (const Uint32 patchVertices : {1u, 2u, 3u, 4u, 16u, 32u}) {
        const Vector<Uint32> spirv = CompileGeneratedSource(patchVertices);
        ASSERT_FALSE(spirv.empty()) << "patchVertices=" << patchVertices;
        EXPECT_EQ(DeclaredOutputVertices(spirv), static_cast<Int>(patchVertices))
            << "patchVertices=" << patchVertices;
    }
}

// gl_Position in, gl_Position out, and both tessellation level arrays written: the four facts the
// evaluation stage downstream of this depends on. Position appearing at all is what makes the
// pass-through a pass-through; the levels are what GL's PATCH_DEFAULT_*_LEVEL state supplies when
// there is no control shader, and without them the tessellator produces nothing.
TEST_F(PassthroughTessControlTest, ForwardsPositionAndWritesBothLevelArrays) {
    const Vector<Uint32> spirv = CompileGeneratedSource(4);
    ASSERT_FALSE(spirv.empty());

    const std::set<Uint32> builtIns = DeclaredBuiltIns(spirv);
    EXPECT_TRUE(builtIns.contains(kBuiltInPosition));
    EXPECT_TRUE(builtIns.contains(kBuiltInInvocationId));
    EXPECT_TRUE(builtIns.contains(kBuiltInTessLevelOuter));
    EXPECT_TRUE(builtIns.contains(kBuiltInTessLevelInner));
}

// The generated source carries nothing but gl_Position across the interface. If that ever grows a
// user-defined varying, ReflectPassthroughTessControlNeed's "built-ins only" refusal stops being
// the right gate and both have to move together.
TEST_F(PassthroughTessControlTest, InterfaceIsBuiltInsOnly) {
    const String source = ProgramFactory::BuildPassthroughTessControlSource(4, kDefaultOuter, kDefaultInner);
    EXPECT_EQ(source.find("layout(location"), String::npos) << source;
    EXPECT_NE(source.find("layout(vertices = 4) out;"), String::npos) << source;
}

// glPatchParameterfv's levels are compiled into this stage - Vulkan has no dynamic state for them -
// so two different level sets must produce two different sources AND two different cache keys.
// Without the second half a pipeline memoised at one set of levels would be handed back after the
// application changed them, and the tessellation would silently stay at the old levels.
TEST_F(PassthroughTessControlTest, BakesTheDefaultTessLevelsInAndKeysOnThem) {
    const FloatVec4 outer(2.0f, 3.0f, 4.0f, 5.0f);
    const FloatVec2 inner(6.5f, 7.25f);
    const String source = ProgramFactory::BuildPassthroughTessControlSource(4, outer, inner);
    EXPECT_NE(source.find("gl_TessLevelOuter[0] = 2.0;"), String::npos) << source;
    EXPECT_NE(source.find("gl_TessLevelOuter[3] = 5.0;"), String::npos) << source;
    EXPECT_NE(source.find("gl_TessLevelInner[0] = 6.5;"), String::npos) << source;
    EXPECT_NE(source.find("gl_TessLevelInner[1] = 7.25;"), String::npos) << source;

    const Uint64 defaultKey =
        ProgramFactory::ComputePassthroughTessControlKey(4, kDefaultOuter, kDefaultInner);
    EXPECT_NE(ProgramFactory::ComputePassthroughTessControlKey(4, outer, inner), defaultKey);
    EXPECT_NE(ProgramFactory::ComputePassthroughTessControlKey(4, kDefaultOuter, inner), defaultKey);
    EXPECT_NE(ProgramFactory::ComputePassthroughTessControlKey(3, kDefaultOuter, kDefaultInner), defaultKey);
    EXPECT_EQ(ProgramFactory::ComputePassthroughTessControlKey(4, kDefaultOuter, kDefaultInner), defaultKey);
}

// The generated stage still has to COMPILE with non-default levels: an integral level spelled
// without a decimal point is an int literal, and `gl_TessLevelOuter[0] = 2;` does not compile.
TEST_F(PassthroughTessControlTest, CompilesWithNonDefaultLevels) {
    using namespace MG_Util::ShaderTranspiler;
    const String source =
        ProgramFactory::BuildPassthroughTessControlSource(4, FloatVec4(2.0f, 2.0f, 2.0f, 2.0f),
                                                          FloatVec2(2.0f, 2.0f));
    ShaderAttrib shaderAttrib{.shaderType = GL_TESS_CONTROL_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    EXPECT_TRUE(shaderResult) << (shaderResult ? String{} : shaderResult.error().log) << source;
}

// THE load-bearing test. Vulkan matches built-in interface blocks by their whole shape, and this
// stage is compiled ON ITS OWN - it never goes through the glslang link that gives a real program
// its gl_PerVertex. So the shape it declares has to equal the shape a linked vertex+evaluation
// program carries, and nothing at runtime says otherwise: a mismatch renders a black frame, no
// error, no validation message. That is exactly how the first cut of this shipped-and-failed
// (gl_Position only, three members short), and how the second did (glslang's default block for a
// standalone control stage, which appends gl_CullDistance where a linked program has no such
// member). This links the shader pair the motivating CTS case uses and compares the two shapes
// directly.
TEST_F(PassthroughTessControlTest, MatchesTheFrontendPerVertexBlock) {
    using namespace MG_Util::ShaderTranspiler;

    // Deliberately the shape of KHR-GL43.shader_storage_buffer_object.advanced-write-tessellation:
    // a vertex stage feeding an evaluation stage with no control stage in between.
    static const char* kVs = R"(#version 430 core
layout(location = 0) in vec4 g_in_position;
void main() { gl_Position = g_in_position; }
)";
    static const char* kTes = R"(#version 430 core
layout(quads) in;
void main() {
  vec4 p0 = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x);
  vec4 p1 = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, gl_TessCoord.x);
  gl_Position = mix(p0, p1, gl_TessCoord.y);
}
)";
    static const char* kFs = R"(#version 430 core
layout(location = 0) out vec4 g_fs_out;
void main() { g_fs_out = vec4(0, 1, 0, 1); }
)";

    const Vector<GLenum> types{GL_VERTEX_SHADER, GL_TESS_EVALUATION_SHADER, GL_FRAGMENT_SHADER};
    const Vector<const char*> sources{kVs, kTes, kFs};
    Vector<SharedPtr<glslang::TShader>> shaders;
    for (SizeT i = 0; i < types.size(); ++i) {
        ShaderAttrib attrib{.shaderType = types[i], .sourceStr = sources[i]};
        auto compiled = ShaderCompiler::CompileShader(attrib);
        ASSERT_TRUE(compiled) << compiled.error().log;
        shaders.push_back(compiled.value());
    }
    ProgramAttrib programAttrib{.shaders = shaders};
    auto linked = ShaderCompiler::LinkProgram(programAttrib);
    ASSERT_TRUE(linked) << linked.error().log;
    ProgramBinaryAttrib binaryAttrib{.shaderTypes = types, .program = *linked.value()};
    auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binary);
    ASSERT_EQ(binary->size(), types.size());

    // The evaluation stage's gl_in is the block the pass-through has to feed. It is the only
    // built-in block that stage declares as an input, so the module holds exactly one such shape
    // besides its own gl_PerVertex output - and both are the same shape, which is the point.
    const auto tesShapes = BuiltInBlockShapes((*binary)[1]);
    ASSERT_FALSE(tesShapes.empty());
    const Vector<Uint32> frontendShape = tesShapes.begin()->second;
    const Uint32 frontendMembers = StructMemberCount((*binary)[1], tesShapes.begin()->first);
    for (const auto& [structId, shape] : tesShapes) {
        EXPECT_EQ(shape, frontendShape) << "the evaluation stage's own built-in blocks disagree";
        EXPECT_EQ(StructMemberCount((*binary)[1], structId), frontendMembers);
    }

    const Vector<Uint32> passthrough = CompileGeneratedSource(4);
    ASSERT_FALSE(passthrough.empty());
    const auto passthroughShapes = BuiltInBlockShapes(passthrough);
    ASSERT_FALSE(passthroughShapes.empty());

    Uint32 perVertexBlocksChecked = 0;
    for (const auto& [structId, shape] : passthroughShapes) {
        // gl_TessLevelOuter/Inner are decorated on plain variables, not on a block, so every
        // struct that reaches here is a gl_PerVertex - gl_in's and gl_out's.
        EXPECT_EQ(shape, frontendShape)
            << "the pass-through control stage's gl_PerVertex no longer matches the one the "
               "frontend gives a linked vertex+evaluation program";
        EXPECT_EQ(StructMemberCount(passthrough, structId), frontendMembers)
            << "the pass-through control stage's gl_PerVertex has a different member count";
        ++perVertexBlocksChecked;
    }
    EXPECT_EQ(perVertexBlocksChecked, 2u) << "expected both gl_in and gl_out to be gl_PerVertex blocks";
}
