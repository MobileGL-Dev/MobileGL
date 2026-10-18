// MobileGL - MobileGL/MG_Test/ShaderTranspiler/ExtendNarrowIntegerVertexInputsTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Magma fetches an integer attribute with the format of the shader input's signedness, so 8/16-bit
// data reaches an input of the other signedness extended by the wrong rule (Sodium's UNSIGNED_BYTE
// a_LightAndData read as `ivec4` turned 240 into -16). ExtendNarrowIntegerVertexInputsPass puts
// GL's value back. What has to hold: the module stays valid, exactly the listed locations get a
// zero-extension mask or a sign-extension, an array input is corrected element by element, the
// reflected vertex inputs do not move, and everything else is left byte-for-byte alone.

#include <gtest/gtest.h>

#include <algorithm>
#include <tuple>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <spirv-tools/libspirv.hpp>
#include <spirv_reflect.h>

using namespace MobileGL;
using MobileGL::MG_Util::ShaderTranspiler::NarrowIntegerVertexInputs;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec4 lightAndData;
layout(location = 2) in uvec2 texc;
layout(location = 3) in uvec4 sbyte;
layout(location = 4) in int shortAsInt;
layout(location = 5) in int packed[3];
layout(location = 8) in uint sshort;
flat out ivec4 a;
flat out uvec4 b;
void main() {
    a = lightAndData + ivec4(shortAsInt) + ivec4(packed[0], packed[1], packed[2], 0);
    b = uvec4(texc, sshort, 0u) + sbyte;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";
    constexpr const char* kFragment = R"(#version 330 core
flat in ivec4 a;
flat in uvec4 b;
out vec4 color;
void main() { color = vec4(a) + vec4(b); }
)";

    // Index 0 is the vertex module, 1 the fragment module (stages come back sorted).
    Vector<Vector<Uint32>> CompileProgram() {
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
        Vector<Vector<Uint32>> modules = binary.value();
        for (auto& module : modules)
            EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(module, module, true, true));
        return modules;
    }

    String Disassemble(const Vector<Uint32>& spirv) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        EXPECT_TRUE(tools.Disassemble(spirv, &text));
        return text;
    }

    String ValidationError(const Vector<Uint32>& spirv, spv_target_env env = SPV_ENV_VULKAN_1_1) {
        spvtools::SpirvTools tools(env);
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

    // The lines of `text` holding `opcode`, for checking an instruction's operands.
    Vector<String> LinesWith(const String& text, const String& opcode) {
        Vector<String> lines;
        SizeT begin = 0;
        while (begin < text.size()) {
            SizeT end = text.find('\n', begin);
            if (end == String::npos) end = text.size();
            String line = text.substr(begin, end - begin);
            if (line.find(opcode) != String::npos) lines.push_back(line);
            begin = end + 1;
        }
        return lines;
    }

    // (location, component count, numeric flags) of every non-builtin input, sorted: what the
    // program factory reflects the vertex inputs from.
    Vector<std::tuple<Uint32, Uint32, Uint32>> ReflectInputs(const Vector<Uint32>& spirv) {
        SpvReflectShaderModule module{};
        EXPECT_EQ(spvReflectCreateShaderModule(spirv.size() * sizeof(Uint32), spirv.data(), &module),
                  SPV_REFLECT_RESULT_SUCCESS);
        Uint32 count = 0;
        EXPECT_EQ(spvReflectEnumerateInputVariables(&module, &count, nullptr), SPV_REFLECT_RESULT_SUCCESS);
        Vector<SpvReflectInterfaceVariable*> inputs(count);
        EXPECT_EQ(spvReflectEnumerateInputVariables(&module, &count, inputs.data()), SPV_REFLECT_RESULT_SUCCESS);
        Vector<std::tuple<Uint32, Uint32, Uint32>> result;
        for (const auto* input : inputs) {
            if (input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) continue;
            result.emplace_back(input->location, input->numeric.vector.component_count,
                                static_cast<Uint32>(input->type_description->type_flags));
        }
        spvReflectDestroyShaderModule(&module);
        std::sort(result.begin(), result.end());
        return result;
    }

    // Sodium's shape plus one of each other kind: UNSIGNED_BYTE into ivec4 (1), BYTE into uvec4 (3),
    // UNSIGNED_SHORT into int (4), SHORT into uint (8), and UNSIGNED_BYTE into the middle element
    // of an int array (location 6 = packed[1]).
    NarrowIntegerVertexInputs SodiumLikeInputs() {
        NarrowIntegerVertexInputs inputs;
        inputs.zeroExtend8 = (1u << 1) | (1u << 6);
        inputs.signExtend8 = 1u << 3;
        inputs.zeroExtend16 = 1u << 4;
        inputs.signExtend16 = 1u << 8;
        return inputs;
    }
} // namespace

TEST(ExtendNarrowIntegerVertexInputs, ExtendsExactlyTheListedLocations) {
    MobileGL::Initialize();
    const auto modules = CompileProgram();
    ASSERT_EQ(modules.size(), 2u);
    const Vector<Uint32>& input = modules[0];
    const String original = Disassemble(input);
    ASSERT_EQ(CountOccurrences(original, "OpBitwiseAnd"), 0u) << original;
    ASSERT_EQ(CountOccurrences(original, "OpBitFieldSExtract"), 0u) << original;

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForVulkan(input, output, SodiumLikeInputs(), true));
    EXPECT_EQ(ValidationError(output), String{});
    const String text = Disassemble(output);

    // Zero-extensions: the ivec4 by 0xff, the int by 0xffff, and packed[1] (one element) by 0xff.
    const auto ands = LinesWith(text, "OpBitwiseAnd");
    ASSERT_EQ(ands.size(), 3u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitwiseAnd %v4int"), 1u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitwiseAnd %int"), 2u) << text;
    EXPECT_EQ(CountOccurrences(text, "%int_65535"), 2u) << text; // declaration + the one use
    EXPECT_NE(text.find("%int_255"), String::npos) << text;

    // Sign-extensions: the uvec4 from 8 bits, the uint from 16, both at bit offset 0.
    const auto extracts = LinesWith(text, "OpBitFieldSExtract");
    ASSERT_EQ(extracts.size(), 2u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitFieldSExtract %v4uint"), 1u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitFieldSExtract %uint"), 1u) << text;
    for (const auto& line : extracts)
        EXPECT_NE(line.find(" %uint_0 %uint_"), String::npos) << line;
    EXPECT_EQ(CountOccurrences(text, " %uint_0 %uint_8"), 1u) << text;
    EXPECT_EQ(CountOccurrences(text, " %uint_0 %uint_16"), 1u) << text;

    // The corrected inputs are now Private copies seeded once; the untouched ones stay Inputs.
    for (const char* name : {"%lightAndData", "%sbyte", "%shortAsInt", "%packed", "%sshort"}) {
        EXPECT_NE(text.find(String(name) + " = OpVariable %_ptr_Private_"), String::npos) << name << "\n" << text;
        EXPECT_EQ(CountOccurrences(text, String("OpStore ") + name + " "), 1u) << name << "\n" << text;
    }
    EXPECT_NE(text.find("%pos = OpVariable %_ptr_Input_v2float Input"), String::npos) << text;
    EXPECT_NE(text.find("%texc = OpVariable %_ptr_Input_v2uint Input"), String::npos) << text;

    // The array is split into its three elements and rebuilt, one element corrected.
    EXPECT_EQ(CountOccurrences(text, "OpCompositeExtract %int") - CountOccurrences(original, "OpCompositeExtract %int"),
              3u)
        << text;
    EXPECT_EQ(CountOccurrences(text, "OpCompositeConstruct %_arr_int_uint_3") -
                  CountOccurrences(original, "OpCompositeConstruct %_arr_int_uint_3"),
              1u)
        << text;

    // Same interface: every location, width and type the program factory reflects is unchanged.
    EXPECT_EQ(ReflectInputs(output), ReflectInputs(input));
}

// A list that names no integer input of the module - a float input, an empty location, a fragment
// module - leaves the bytes alone, so a mis-keyed variant cannot change what the shader computes.
TEST(ExtendNarrowIntegerVertexInputs, LeavesEverythingElseUnchanged) {
    MobileGL::Initialize();
    const auto modules = CompileProgram();
    ASSERT_EQ(modules.size(), 2u);

    NarrowIntegerVertexInputs floatOrEmpty;
    floatOrEmpty.zeroExtend8 = (1u << 0) | (1u << 12);
    floatOrEmpty.signExtend16 = 1u << 31;
    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForVulkan(modules[0], output, floatOrEmpty, true));
    EXPECT_EQ(output, modules[0]);

    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForVulkan(modules[1], output, SodiumLikeInputs(), true));
    EXPECT_EQ(output, modules[1]);

    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForVulkan(modules[0], output, {}, true));
    EXPECT_EQ(output, modules[0]);
}

// From SPIR-V 1.4 an entry point lists every global it uses, so the Private copies have to join
// the new Inputs in the interface. Checked by re-labelling the module 1.4 and validating it for an
// environment that accepts 1.4.
TEST(ExtendNarrowIntegerVertexInputs, ListsPrivateCopiesInASpirv14Interface) {
    MobileGL::Initialize();
    const auto modules = CompileProgram();
    ASSERT_EQ(modules.size(), 2u);
    Vector<Uint32> input = modules[0];
    ASSERT_GT(input.size(), 1u);
    input[1] = 0x00010400u;
    ASSERT_EQ(ValidationError(input, SPV_ENV_VULKAN_1_2), String{});

    Vector<Uint32> output;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForVulkan(input, output, SodiumLikeInputs(), false));
    EXPECT_EQ(ValidationError(output, SPV_ENV_VULKAN_1_2), String{});
    const auto entryPoint = LinesWith(Disassemble(output), "OpEntryPoint");
    ASSERT_EQ(entryPoint.size(), 1u);
    for (const char* name : {"%lightAndData", "%sbyte", "%shortAsInt", "%packed", "%sshort", "%texc"})
        EXPECT_NE(entryPoint[0].find(name), String::npos) << name << "\n" << entryPoint[0];
}
