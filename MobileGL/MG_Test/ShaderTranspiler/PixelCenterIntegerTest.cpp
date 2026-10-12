// MobileGL - MobileGL/MG_Test/ShaderTranspiler/PixelCenterIntegerTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// `layout(pixel_center_integer) in vec4 gl_FragCoord;` compiles to the PixelCenterInteger
// execution mode, which the Vulkan environment forbids: DirectVulkan's validation refused every
// such program (KHR-GL45.direct_state_access.textures_* draw through one). The lowered module
// must validate, carry no such mode, and shift exactly the x/y reads of the builtin.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    Vector<Uint32> CompileFragment(const char* fragment) {
        using namespace MG_Util::ShaderTranspiler;
        constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] : Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, kVertex},
                                                                           {GL_FRAGMENT_SHADER, fragment}}) {
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
        Vector<Uint32> module = binary.value()[1];
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        return module;
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

    SizeT CountOccurrences(const String& text, const String& needle) {
        SizeT count = 0;
        for (SizeT at = text.find(needle); at != String::npos; at = text.find(needle, at + needle.size())) ++count;
        return count;
    }
} // namespace

// The direct_state_access shape: the whole vector converted to integer texel coordinates.
TEST(LowerPixelCenterInteger, ShiftsWholeVectorReadsAndDropsTheMode) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileFragment(R"(#version 330 core
layout(pixel_center_integer) in vec4 gl_FragCoord;
uniform sampler2D data;
out vec4 color;
void main() { color = texelFetch(data, ivec2(gl_FragCoord.xy), 0); }
)");
    ASSERT_FALSE(input.empty());
    ASSERT_NE(Disassemble(input).find("PixelCenterInteger"), String::npos);
    EXPECT_FALSE(ValidationError(input).empty()) << "the unlowered module is what Vulkan refuses";

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerPixelCenterIntegerForVulkan(input, output, true));
    EXPECT_EQ(ValidationError(output), String{});
    const String text = Disassemble(output);
    EXPECT_EQ(text.find("PixelCenterInteger"), String::npos) << text;
    EXPECT_EQ(CountOccurrences(text, "OpFSub"), 1u) << text;
    EXPECT_NE(text.find("OpConstantComposite %v4float %float_0_5 %float_0_5 %float_0 %float_0"), String::npos)
        << text;
}

// Single components arrive through an access chain: x and y move, z and w keep their values.
TEST(LowerPixelCenterInteger, ShiftsOnlyTheXAndYComponents) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileFragment(R"(#version 330 core
layout(pixel_center_integer) in vec4 gl_FragCoord;
out vec4 color;
void main() { color = vec4(gl_FragCoord.x, gl_FragCoord.y, gl_FragCoord.z, gl_FragCoord.w); }
)");
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerPixelCenterIntegerForVulkan(input, output, true));
    EXPECT_EQ(ValidationError(output), String{});
    const String text = Disassemble(output);
    EXPECT_EQ(text.find("PixelCenterInteger"), String::npos) << text;
    EXPECT_EQ(CountOccurrences(text, "OpFSub"), 2u) << text;
}

// A module without the mode is left alone.
TEST(LowerPixelCenterInteger, LeavesDefaultCentredModulesUnchanged) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileFragment(R"(#version 330 core
out vec4 color;
void main() { color = vec4(gl_FragCoord.xy, 0.0, 1.0); }
)");
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::LowerPixelCenterIntegerForVulkan(input, output, true));
    EXPECT_EQ(Disassemble(output), Disassemble(input));
}
