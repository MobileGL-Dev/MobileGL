// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FragCoordYFlipTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Magma's FragCoordYFlip variant redirects every read of gl_FragCoord to a Private copy. A
// single-component read (gl_FragCoord.y) is an access chain whose result type is an INPUT
// pointer; left that way on a Private base it is invalid SPIR-V that the driver still accepts and
// reads garbage through. The module must validate, and the chain must point at the copy.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Backend/DirectVulkan/Renderer/ProgramFactory.h>
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

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        EXPECT_TRUE(tools.Disassemble(spirv, &text));
        return text;
    }

    String ValidationError(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String error;
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                                     const char* message) { error += message != nullptr ? message : ""; });
        return tools.Validate(spirv) ? String{} : (error.empty() ? String{"invalid"} : error);
    }

    constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";

    void ExpectFlippedModuleValid(const char* fragment) {
        MobileGL::Initialize();
        auto modules = CompileProgram(kVertex, fragment);
        ASSERT_EQ(modules.size(), 2u);
        const Vector<Uint32>& input = modules[1];
        ASSERT_EQ(ValidationError(input), String{});

        Vector<Uint32> flipped;
        ASSERT_TRUE(MG_Backend::DirectVulkan::TransformSpirvForFragCoordYFlip(input, flipped, 720));
        EXPECT_EQ(ValidationError(flipped), String{}) << Disassemble(flipped);
        // The flip really happened: the height is subtracted, and no chain still reads the
        // builtin's Input storage.
        const String text = Disassemble(flipped);
        EXPECT_NE(text.find("OpFSub"), String::npos) << text;
        EXPECT_EQ(text.find("OpAccessChain %_ptr_Input_float"), String::npos) << text;
    }
} // namespace

TEST(FragCoordYFlipTest, SingleComponentReadStaysValid) {
    ExpectFlippedModuleValid(R"(#version 330 core
out vec4 color;
void main() { color = vec4(gl_FragCoord.y / 720.0, 0.0, 0.0, 1.0); }
)");
}

TEST(FragCoordYFlipTest, ComponentAndWholeVectorReadsStayValid) {
    ExpectFlippedModuleValid(R"(#version 330 core
out vec4 color;
uniform float limit;
void main() {
    float acc = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (gl_FragCoord.x > limit) acc += gl_FragCoord.y;
    }
    color = vec4(gl_FragCoord.xy, acc, 1.0);
}
)");
}
