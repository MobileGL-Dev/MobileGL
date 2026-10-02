// MobileGL - MobileGL/MG_Test/ShaderTranspiler/RelaxedOpaqueStructTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A UNIFORM STRUCT WITH A SAMPLER INSIDE IT. The Vulkan-relaxed front end moves loose uniforms
// into the default uniform block and splits the struct's opaque members out as uniforms of their
// own; the struct that stays in the block must lose those members, or the block carries a sampler
// - invalid for Vulkan, and a driver that takes it anyway reads a descriptor out of uniform-buffer
// bytes. Shadertoy declares its channels exactly so, as ANONYMOUS structs
// (`uniform struct { vec3 size; float time; int loaded; sampler2D sampler; } iCh0, ...;`), and
// on Adreno that module GPU-page-faulted on a garbage address until the device was lost. The
// named-struct form was already stripped; the anonymous one was not.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    Vector<Vector<Uint32>> CompileProgram(const char* vertex, const char* fragment) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        Vector<GLenum> types;
        for (const auto& [stage, source] : Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, vertex},
                                                                           {GL_FRAGMENT_SHADER, fragment}}) {
            ShaderAttrib attrib{.shaderType = stage, .sourceStr = source};
            auto shader = ShaderCompiler::CompileShader(attrib);
            EXPECT_TRUE(shader) << (shader ? String{} : shader.error().log);
            if (!shader) return {};
            shaders.push_back(shader.value());
            types.push_back(stage);
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
        for (auto& module : modules) {
            EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        }
        return modules;
    }

    // The validator's complaint followed by the module, or empty when the module is valid.
    String VulkanValidationError(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                                     const char* message) { error += message != nullptr ? message : ""; });
        if (tools.Validate(spirv)) return {};
        String text;
        tools.Disassemble(spirv, &text);
        return error + "\n" + text;
    }

    constexpr const char* kVertex = "#version 330 core\n"
                                    "layout(location = 0) in vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
} // namespace

TEST(RelaxedOpaqueStructTest, AnAnonymousUniformStructLosesItsSamplerInTheDefaultBlock) {
    MobileGL::Initialize();
    const auto modules = CompileProgram(kVertex,
        "#version 330 core\n"
        "uniform struct { vec3 size; float time; int loaded; sampler2D smp; } c0, c1, c2, c3;\n"
        "uniform vec3 res; out vec4 o;\n"
        "void main() { o = texture(c1.smp, gl_FragCoord.xy / res.xy) + vec4(c0.size, c0.time + float(c2.loaded)); }");
    ASSERT_EQ(modules.size(), 2u);
    EXPECT_EQ(VulkanValidationError(modules[1]), String{});
}

TEST(RelaxedOpaqueStructTest, ANamedUniformStructLosesItsSamplerInTheDefaultBlock) {
    MobileGL::Initialize();
    const auto modules = CompileProgram(kVertex,
        "#version 330 core\n"
        "struct Channel { vec3 size; float time; int loaded; sampler2D smp; };\n"
        "uniform Channel c0; uniform Channel c1; uniform vec3 res; out vec4 o;\n"
        "void main() { o = texture(c1.smp, gl_FragCoord.xy / res.xy) + vec4(c0.size, c0.time); }");
    ASSERT_EQ(modules.size(), 2u);
    EXPECT_EQ(VulkanValidationError(modules[1]), String{});
}
