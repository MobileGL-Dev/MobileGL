// MobileGL - MobileGL/MG_Test/ShaderTranspiler/FullCoverageSampleMaskTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// GL ignores a fragment shader's gl_SampleMask without multisample fragment operations; Vulkan
// discards on a clear bit 0 at one sample (KHR-GL4x.sample_variables.mask.*.samples_0 drew
// nothing). The single-sample variant must validate and store nothing but all-ones.

#include <gtest/gtest.h>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include "spirv-tools/libspirv.hpp"

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr const char* kVertex = R"(#version 450 core
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";

    // The fragment module of a vertex + fragment program (stages come back sorted).
    Vector<Uint32> CompileFragment(const char* fragment) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] :
             Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, kVertex}, {GL_FRAGMENT_SHADER, fragment}}) {
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
        tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
            error += message != nullptr ? message : "";
        });
        return tools.Validate(spirv) ? String{} : (error.empty() ? String{"invalid"} : error);
    }

    SizeT CountOccurrences(const String& text, const String& needle) {
        SizeT count = 0;
        for (SizeT at = text.find(needle); at != String::npos; at = text.find(needle, at + needle.size()))
            ++count;
        return count;
    }

    // The CTS shape: the mask is computed from a uniform and gl_SampleMaskIn, one store per word.
    constexpr const char* kMaskingFragment = R"(#version 450 core
layout(location = 0) out vec4 color;
uniform int u_sampleMask;
void main() {
    gl_SampleMask[0] = u_sampleMask & gl_SampleMaskIn[0];
    color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";
} // namespace

TEST(FullCoverageSampleMask, EverySampleMaskStoreWritesAllOnes) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileFragment(kMaskingFragment);
    ASSERT_FALSE(input.empty());
    const String original = Disassemble(input);
    ASSERT_NE(original.find("BuiltIn SampleMask"), String::npos) << original;
    ASSERT_EQ(CountOccurrences(original, "%int_n1"), 0u) << original;

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FullCoverageSampleMaskForVulkan(input, output, true));
    EXPECT_EQ(ValidationError(output), String{});
    EXPECT_NE(output, input);
    const String text = Disassemble(output);
    // Every store into the builtin now stores the all-ones constant.
    SizeT stores = 0;
    for (SizeT at = text.find("OpStore "); at != String::npos; at = text.find("OpStore ", at + 1)) {
        const SizeT end = text.find('\n', at);
        const String line = text.substr(at, end - at);
        if (line.find("%int_n1") != String::npos) ++stores;
    }
    EXPECT_GE(stores, 1u) << text;
    EXPECT_NE(text.find("%int_n1 = OpConstant %int -1"), String::npos) << text;
}

// A module that never writes gl_SampleMask is left alone, so its program keeps one module.
TEST(FullCoverageSampleMask, LeavesModulesWithoutSampleMaskUnchanged) {
    MobileGL::Initialize();
    const Vector<Uint32> input = CompileFragment(R"(#version 450 core
layout(location = 0) out vec4 color;
void main() { color = vec4(float(gl_SampleMaskIn[0] & 1)); }
)");
    ASSERT_FALSE(input.empty());
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::FullCoverageSampleMaskForVulkan(input, output, true));
    EXPECT_EQ(output, input);
}
