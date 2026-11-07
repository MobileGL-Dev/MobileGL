// MobileGL - MobileGL/MG_Test/ShaderTranspiler/EmulateClipDistanceTest.cpp
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
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::CompileEnv;
using MobileGL::MG_Util::ShaderTranspiler::SessionUsageBit;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;
using MobileGL::MG_Util::ShaderTranspiler::SpvcSession;

namespace {
    // The exact bytes ProgramSpirvTask hands the emulation: linked, generated, sanitized.
    Vector<Vector<Uint32>> CompileProgramToSpirv(const Vector<Pair<GLenum, const char*>>& stages,
                                                 Vector<GLenum>& types) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        types.clear();
        for (const auto& [stage, source] : stages) {
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
        // RAW glslang output: ProgramSpirvTask runs the emulation BEFORE the sanitize chain.
        return Move(binaryResult.value());
    }

    // What ProgramSpirvTask does right after the emulation, so every validity / transpile
    // assertion is made against the bytes the backends really receive.
    void SanitizeAll(Vector<Vector<Uint32>>& modules) {
        for (auto& module : modules) {
            EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        }
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
        String messages;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            messages += message;
            messages += '\n';
        });
        const Bool ok = tools.Validate(spirv);
        EXPECT_TRUE(ok) << messages;
        return ok;
    }

    Bool Contains(const String& haystack, const char* needle) {
        return haystack.find(needle) != String::npos;
    }

    // The shape from the field report (Mali-G615, Espryt): a #version 330 core vertex stage
    // writing all eight distances from uniform planes. The enables uniform is what
    // InjectClipDistanceEnablesShim adds on an emulating backend; spelled out here because a
    // unit test has no backend to arm the shim.
    const char* kEightPlaneVertex = R"(#version 330 core
uniform uint mg_ClipDistanceEnables;
uniform vec4 u_ClipPlane[8];
uniform mat4 u_ModelView;
in vec4 a_Position;
void main() {
    vec4 eyePos = u_ModelView * a_Position;
    gl_Position = eyePos;
    gl_ClipDistance[0] = dot(u_ClipPlane[0], eyePos);
    gl_ClipDistance[1] = dot(u_ClipPlane[1], eyePos);
    gl_ClipDistance[2] = dot(u_ClipPlane[2], eyePos);
    gl_ClipDistance[3] = dot(u_ClipPlane[3], eyePos);
    gl_ClipDistance[4] = dot(u_ClipPlane[4], eyePos);
    gl_ClipDistance[5] = dot(u_ClipPlane[5], eyePos);
    gl_ClipDistance[6] = dot(u_ClipPlane[6], eyePos);
    gl_ClipDistance[7] = dot(u_ClipPlane[7], eyePos);
}
)";

    const char* kPlainFragment = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

    constexpr Uint32 kBudget = 32;
} // namespace

TEST(EmulateClipDistance, TheReportedEightPlaneVertexStageClipsByDiscard) {
    Vector<GLenum> types;
    auto modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kEightPlaneVertex}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    ASSERT_TRUE(Contains(Disassemble(modules[0]), "OpCapability ClipDistance"));

    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    SanitizeAll(modules);
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_EQ(outcome.distanceCount, 8u);
    EXPECT_TRUE(outcome.enablesFound);

    for (const auto& module : modules) {
        EXPECT_TRUE(Validates(module));
        EXPECT_FALSE(Contains(Disassemble(module), "OpCapability ClipDistance"));
    }
    const String vertexAsm = Disassemble(modules[0]);
    EXPECT_TRUE(Contains(vertexAsm, "mg_ClipDistanceEmu0"));
    EXPECT_TRUE(Contains(vertexAsm, "mg_ClipDistanceEmu1"));
    EXPECT_TRUE(Contains(vertexAsm, "OpBitwiseAnd"));
    const String fragmentAsm = Disassemble(modules[1]);
    EXPECT_TRUE(Contains(fragmentAsm, "OpKill"));
    EXPECT_TRUE(Contains(fragmentAsm, "mg_ClipDistanceEmu1"));

    // The ES hop must no longer need GL_EXT_clip_cull_distance anywhere.
    const String vertexEssl = Transpile(modules[0]);
    const String fragmentEssl = Transpile(modules[1]);
    EXPECT_FALSE(Contains(vertexEssl, "gl_ClipDistance")) << vertexEssl;
    EXPECT_FALSE(Contains(vertexEssl, "clip_cull_distance")) << vertexEssl;
    EXPECT_TRUE(Contains(vertexEssl, "mg_ClipDistanceEmu0")) << vertexEssl;
    EXPECT_TRUE(Contains(fragmentEssl, "discard")) << fragmentEssl;
    EXPECT_FALSE(Contains(fragmentEssl, "clip_cull_distance")) << fragmentEssl;
}

TEST(EmulateClipDistance, OneDistanceUsesOneCarrierAndNoEnablesMeansAllEnabled) {
    const char* vertex = R"(#version 330 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_ClipDistance[0] = gl_Position.x;
}
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    SanitizeAll(modules);
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_EQ(outcome.distanceCount, 1u);
    EXPECT_FALSE(outcome.enablesFound);
    for (const auto& module : modules)
        EXPECT_TRUE(Validates(module));
    const String vertexAsm = Disassemble(modules[0]);
    EXPECT_TRUE(Contains(vertexAsm, "mg_ClipDistanceEmu0"));
    EXPECT_FALSE(Contains(vertexAsm, "mg_ClipDistanceEmu1"));
}

TEST(EmulateClipDistance, AProgramThatNeverClipsIsUntouched) {
    const char* vertex = R"(#version 330 core
void main() { gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0); }
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    const auto before = modules;
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    EXPECT_FALSE(outcome.emulated);
    EXPECT_TRUE(outcome.declineDetail.empty());
    EXPECT_EQ(modules, before);
}

TEST(EmulateClipDistance, AGeometryStageProducesBeforeEveryEmitAndTheVertexWriteIsRedirected) {
    const char* vertex = R"(#version 330 core
uniform uint mg_ClipDistanceEnables;
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_ClipDistance[0] = -1.0;
}
)";
    const char* geometry = R"(#version 330 core
uniform uint mg_ClipDistanceEnables;
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_ClipDistance[0] = gl_in[i].gl_Position.x;
        gl_ClipDistance[1] = 1.0;
        EmitVertex();
    }
    EndPrimitive();
}
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv(
        {{GL_VERTEX_SHADER, vertex}, {GL_GEOMETRY_SHADER, geometry}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 3u);
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    SanitizeAll(modules);
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_EQ(outcome.distanceCount, 2u);
    for (const auto& module : modules) {
        EXPECT_TRUE(Validates(module));
        EXPECT_FALSE(Contains(Disassemble(module), "OpCapability ClipDistance"));
    }
    // The vertex stage is not the producer: redirected, no carriers.
    EXPECT_FALSE(Contains(Disassemble(modules[0]), "mg_ClipDistanceEmu0"));
    EXPECT_TRUE(Contains(Disassemble(modules[1]), "mg_ClipDistanceEmu0"));
}

TEST(EmulateClipDistance, AFragmentStageReadOfGlClipDistanceIsServedFromTheCarriers) {
    const char* vertex = R"(#version 330 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_ClipDistance[0] = 0.5;
}
)";
    const char* fragment = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(gl_ClipDistance[0], 1.0, 0.0, 1.0); }
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    SanitizeAll(modules);
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    for (const auto& module : modules) {
        EXPECT_TRUE(Validates(module));
        EXPECT_FALSE(Contains(Disassemble(module), "OpCapability ClipDistance"));
    }
    const String fragmentEssl = Transpile(modules[1]);
    EXPECT_FALSE(Contains(fragmentEssl, "gl_ClipDistance")) << fragmentEssl;
    EXPECT_TRUE(Contains(fragmentEssl, "mg_ClipDistanceShadow")) << fragmentEssl;
}

TEST(EmulateClipDistance, AGlInReadDeclinesAndLeavesEveryModuleAlone) {
    const char* vertex = R"(#version 330 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_ClipDistance[0] = 1.0;
}
)";
    const char* geometry = R"(#version 330 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main() {
    gl_Position = gl_in[0].gl_Position + vec4(gl_in[0].gl_ClipDistance[0]);
    EmitVertex();
}
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv(
        {{GL_VERTEX_SHADER, vertex}, {GL_GEOMETRY_SHADER, geometry}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 3u);
    const auto before = modules;
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    EXPECT_FALSE(outcome.emulated);
    EXPECT_NE(outcome.declineDetail.find("gl_in"), String::npos) << outcome.declineDetail;
    EXPECT_EQ(modules, before);
}

TEST(EmulateClipDistance, ACaptureOfGlClipDistanceDeclines) {
    Vector<GLenum> types;
    auto modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kEightPlaneVertex}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    const auto before = modules;
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, true, kBudget, outcome, true, true));
    EXPECT_FALSE(outcome.emulated);
    EXPECT_FALSE(outcome.declineDetail.empty());
    EXPECT_EQ(modules, before);
}

// End to end through the source pipeline: the shim's uniform must survive glslang and the
// sanitize chain so the producer finds it. Without this the draw path's mask has nowhere to
// land and every distance silently clips (the all-enabled fallback).
TEST(EmulateClipDistance, TheShimmedEnablesUniformReachesTheProducer) {
    CompileEnv emulating{};
    emulating.backend = BackendType::DirectGLES;
    emulating.params.EmulatesClipDistance = true;
    String vertex = R"(#version 400 core
out float gl_ClipDistance[2];
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexID];
    gl_Position = vec4(p, 0.0, 1.0);
    gl_ClipDistance[0] = p.x;
    gl_ClipDistance[1] = p.y;
}
)";
    String fragment = "#version 400 core\nout vec4 o;\nvoid main() { o = vec4(1.0); }\n";
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, vertex, emulating);
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Fragment, fragment, emulating);
    ASSERT_TRUE(Contains(vertex, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << vertex;
    Vector<GLenum> types;
    auto modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, vertex.c_str()}, {GL_FRAGMENT_SHADER, fragment.c_str()}}, types);
    ASSERT_EQ(modules.size(), 2u);
    EXPECT_TRUE(Contains(Disassemble(modules[0]), "mg_ClipDistanceEnables")) << Disassemble(modules[0]);
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true));
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_TRUE(outcome.enablesFound) << vertex;
    // And the read the producer added keeps the member alive through the sanitize chain.
    SanitizeAll(modules);
    for (const auto& module : modules)
        EXPECT_TRUE(Validates(module));
    EXPECT_TRUE(Contains(Disassemble(modules[0]), "mg_ClipDistanceEnables"));
}

// Native masking (Vulkan with shaderClipDistance): the producer keeps the built-in and the
// capability, writes `enabled(i) ? d[i] : 1.0` back into it, and nothing else changes - no
// carriers, no discard, the fragment stage byte-identical.
TEST(EmulateClipDistance, NativeMaskingWritesTheMaskedDistancesBackIntoTheBuiltIn) {
    Vector<GLenum> types;
    auto modules =
        CompileProgramToSpirv({{GL_VERTEX_SHADER, kEightPlaneVertex}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 2u);
    const auto fragmentBefore = modules[1];
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true,
                                                              /*nativeMask=*/true));
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_EQ(outcome.distanceCount, 8u);
    EXPECT_TRUE(outcome.enablesFound);
    EXPECT_EQ(modules[1], fragmentBefore);
    const String vertexRaw = Disassemble(modules[0]);
    EXPECT_TRUE(Contains(vertexRaw, "OpCapability ClipDistance"));
    EXPECT_TRUE(Contains(vertexRaw, "OpBitwiseAnd"));
    EXPECT_FALSE(Contains(vertexRaw, "mg_ClipDistanceEmu0"));
    SanitizeAll(modules);
    for (const auto& module : modules)
        EXPECT_TRUE(Validates(module));
    EXPECT_FALSE(Contains(Disassemble(modules[1]), "OpKill"));
}

// Native masking leaves gl_in[] reads native: a geometry stage that reads the vertex stage's
// distances and re-emits them is the producer, and the vertex stage is untouched.
TEST(EmulateClipDistance, NativeMaskingKeepsGlInReadsAndMasksOnlyTheProducer) {
    const char* vertex = R"(#version 330 core
void main() {
    gl_Position = vec4(float(gl_VertexID), 0.0, 0.0, 1.0);
    gl_ClipDistance[0] = gl_Position.x;
}
)";
    const char* geometry = R"(#version 330 core
uniform uint mg_ClipDistanceEnables;
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_ClipDistance[0] = gl_in[i].gl_ClipDistance[0];
        EmitVertex();
    }
}
)";
    Vector<GLenum> types;
    auto modules = CompileProgramToSpirv(
        {{GL_VERTEX_SHADER, vertex}, {GL_GEOMETRY_SHADER, geometry}, {GL_FRAGMENT_SHADER, kPlainFragment}}, types);
    ASSERT_EQ(modules.size(), 3u);
    const auto vertexBefore = modules[0];
    ShaderCompiler::ClipDistanceEmulationOutcome outcome;
    ASSERT_TRUE(ShaderCompiler::EmulateClipDistanceForProgram(modules, types, false, kBudget, outcome, true, true,
                                                              /*nativeMask=*/true));
    EXPECT_TRUE(outcome.emulated) << outcome.declineDetail;
    EXPECT_TRUE(outcome.enablesFound);
    EXPECT_EQ(modules[0], vertexBefore);
    SanitizeAll(modules);
    for (const auto& module : modules)
        EXPECT_TRUE(Validates(module));
}

TEST(EmulateClipDistance, TheEnablesShimArmsOnlyOnAnEmulatingBackendAndOnlyWhereItIsNamed) {
    CompileEnv emulating{};
    emulating.backend = BackendType::DirectGLES;
    emulating.params.EmulatesClipDistance = true;
    CompileEnv native = emulating;
    native.params.EmulatesClipDistance = false;

    const String vertex = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); gl_ClipDistance[0] = 1.0; }\n";
    const String fragment = "#version 330 core\nout vec4 o; void main() { o = vec4(1.0); }\n";
    const String nonClipping = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";

    String source = vertex;
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, source, emulating);
    EXPECT_TRUE(Contains(source, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << source;

    source = vertex;
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, source, native);
    EXPECT_FALSE(Contains(source, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << source;

    source = fragment;
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Fragment, source, emulating);
    EXPECT_FALSE(Contains(source, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << source;

    source = nonClipping;
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, source, emulating);
    EXPECT_FALSE(Contains(source, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << source;

    // A native backend that masks in the shader (Magma) needs the uniform too.
    CompileEnv masking = native;
    masking.backend = BackendType::DirectVulkan;
    masking.params.MasksNativeClipDistances = true;
    source = vertex;
    MG_Util::ShaderTranspiler::PreprocessShaderSource(ShaderStage::Vertex, source, masking);
    EXPECT_TRUE(Contains(source, MG_Util::ShaderTranspiler::CLIP_DISTANCE_ENABLES_UNIFORM_NAME)) << source;
}
