// MobileGL - MobileGL/MG_Test/ShaderTranspiler/ZeroLayerOutputTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL ignores gl_Layer for a non-layered framebuffer; Vulkan offsets a primitive by a Layer past the
// framebuffer's layer count (KHR-GL4x.texture_cube_map_array.color_depth_attachments lost every
// draw into layer-face 1). The non-layered variant must validate and store nothing but 0.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";
    constexpr const char* kFragment = R"(#version 330 core
out vec4 color;
void main() { color = vec4(1.0); }
)";

    // Index of the geometry module in the program's SPIR-V (stages come back sorted).
    Vector<Uint32> CompileGeometry(const char* geometry) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] : Vector<Pair<GLenum, const char*>>{
                 {GL_VERTEX_SHADER, kVertex}, {GL_GEOMETRY_SHADER, geometry}, {GL_FRAGMENT_SHADER, kFragment}}) {
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
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER, GL_GEOMETRY_SHADER, GL_FRAGMENT_SHADER},
                                         .program = *program.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        EXPECT_TRUE(binary) << (binary ? String{} : binary.error().log);
        if (!binary || binary->size() != 3) return {};
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

    constexpr const char* kLayeredGeometry = R"(#version 330 core
layout(points) in;
layout(triangle_strip, max_vertices = 3) out;
uniform int layer;
void main() {
    gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); gl_Layer = layer; EmitVertex();
    gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); gl_Layer = layer; EmitVertex();
    gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); gl_Layer = layer; EmitVertex();
    EndPrimitive();
}
)";
} // namespace

TEST(ZeroLayerOutput, EveryLayerStoreWritesZero) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileGeometry(kLayeredGeometry);
    ASSERT_FALSE(input.empty());
    const String original = Disassemble(input);
    ASSERT_EQ(CountOccurrences(original, "OpStore %gl_Layer "), 3u) << original;
    ASSERT_EQ(CountOccurrences(original, "OpStore %gl_Layer %int_0"), 0u) << original;

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ZeroLayerOutputForVulkan(input, output, true));
    EXPECT_EQ(ValidationError(output), String{});
    const String text = Disassemble(output);
    EXPECT_EQ(CountOccurrences(text, "OpStore %gl_Layer %int_0"), 3u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpStore %gl_Layer "), 3u) << text;
}

// A module that never writes gl_Layer is left alone, so its program keeps one stage list.
TEST(ZeroLayerOutput, LeavesModulesWithoutLayerUnchanged) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileGeometry(R"(#version 330 core
layout(points) in;
layout(points, max_vertices = 1) out;
void main() { gl_Position = vec4(0.0); EmitVertex(); EndPrimitive(); }
)");
    ASSERT_FALSE(input.empty());

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ZeroLayerOutputForVulkan(input, output, true));
    EXPECT_EQ(output, input);
}
