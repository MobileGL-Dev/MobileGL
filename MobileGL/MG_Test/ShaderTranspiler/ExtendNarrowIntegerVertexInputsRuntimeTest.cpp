// MobileGL - MobileGL/MG_Test/ShaderTranspiler/ExtendNarrowIntegerVertexInputsRuntimeTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ExtendNarrowIntegerVertexInputsPass's RuntimeWidths mode, the one a backend uses when it cannot
// vary the program per draw. Every 32-bit integer input below location 32 is seeded from the two
// width globals, with shifts only - no bitfield instruction, which ESSL 3.00 cannot spell - and the
// module must stay valid, keep its reflected inputs and leave a module without integer inputs alone.

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
using MobileGL::MG_Util::ShaderTranspiler::kNarrowIntegerWidths16GlobalName;
using MobileGL::MG_Util::ShaderTranspiler::kNarrowIntegerWidths8GlobalName;
using MobileGL::MG_Util::ShaderTranspiler::ShaderCompiler;

namespace {
    constexpr const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in ivec4 lightAndData;
layout(location = 2) in uvec2 texc;
layout(location = 3) in int single;
layout(location = 4) in uint other;
layout(location = 5) in int packed[2];
flat out ivec4 a;
flat out uvec4 b;
void main() {
    a = lightAndData + ivec4(single, packed[0], packed[1], 0);
    b = uvec4(texc, other, 0u);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";
    constexpr const char* kFloatOnlyVertex = R"(#version 330 core
layout(location = 0) in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";
    constexpr const char* kFragment = R"(#version 330 core
flat in ivec4 a;
flat in uvec4 b;
out vec4 color;
void main() { color = vec4(a) + vec4(b); }
)";
    constexpr const char* kPlainFragment = R"(#version 330 core
out vec4 color;
void main() { color = vec4(1.0); }
)";

    // Index 0 is the vertex module, 1 the fragment module.
    Vector<Vector<Uint32>> CompileProgram(const char* vertex, const char* fragment) {
        using namespace MG_Util::ShaderTranspiler;
        Vector<SharedPtr<glslang::TShader>> shaders;
        for (const auto& [stage, source] :
             Vector<Pair<GLenum, const char*>>{{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}}) {
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

    Vector<String> LinesWith(const String& text, const String& needle) {
        Vector<String> lines;
        SizeT begin = 0;
        while (begin < text.size()) {
            SizeT end = text.find('\n', begin);
            if (end == String::npos) end = text.size();
            String line = text.substr(begin, end - begin);
            if (line.find(needle) != String::npos) lines.push_back(line);
            begin = end + 1;
        }
        return lines;
    }

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

    // Moves every `OpDecorate %id Location from` to `to`, in the binary itself.
    void MoveLocation(Vector<Uint32>& spirv, Uint32 from, Uint32 to) {
        for (SizeT word = 5; word < spirv.size();) {
            const Uint32 count = spirv[word] >> 16;
            const Uint32 opcode = spirv[word] & 0xFFFFu;
            if (count == 0) break;
            if (opcode == 71u /* OpDecorate */ && count == 4 && spirv[word + 2] == 30u /* Location */ &&
                spirv[word + 3] == from)
                spirv[word + 3] = to;
            word += count;
        }
    }
} // namespace

TEST(ExtendNarrowIntegerVertexInputsRuntimeWidths, SeedsEveryIntegerInputFromTheTwoWidths) {
    MobileGL::Initialize();
    const auto modules = CompileProgram(kVertex, kFragment);
    ASSERT_EQ(modules.size(), 2u);
    const String original = Disassemble(modules[0]);

    Vector<Uint32> output;
    Bool changed = false;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(modules[0], output, changed, true));
    EXPECT_TRUE(changed);
    EXPECT_EQ(ValidationError(output), String{});
    const String text = Disassemble(output);

    // The two globals: named, Private, initialized to 0 and never stored.
    for (const char* name : {kNarrowIntegerWidths8GlobalName, kNarrowIntegerWidths16GlobalName}) {
        const String id = String("%") + name;
        EXPECT_NE(text.find(id + " = OpVariable %_ptr_Private_uint Private %uint_0"), String::npos) << name << "\n"
                                                                                                    << text;
        EXPECT_EQ(CountOccurrences(text, String("OpStore ") + id + " "), 0u) << text;
        EXPECT_EQ(CountOccurrences(text, String("OpLoad %uint ") + id), 1u) << text;
    }

    // Every integer input is a seeded Private copy now; the float input is untouched.
    for (const char* name : {"%lightAndData", "%texc", "%single", "%other", "%packed"}) {
        EXPECT_NE(text.find(String(name) + " = OpVariable %_ptr_Private_"), String::npos) << name << "\n" << text;
        EXPECT_EQ(CountOccurrences(text, String("OpStore ") + name + " "), 1u) << name << "\n" << text;
    }
    EXPECT_NE(text.find("%pos = OpVariable %_ptr_Input_v2float Input"), String::npos) << text;

    // Six seeded values (the ivec4, the uvec2, the int, the uint and two array elements): a left
    // shift each, an arithmetic right shift for the two uint ones, a logical one for the four int
    // ones, and the shift amount splatted for the two vectors. No bitfield instruction at all.
    EXPECT_EQ(CountOccurrences(text, "OpShiftLeftLogical") - CountOccurrences(original, "OpShiftLeftLogical"), 6u)
        << text;
    EXPECT_EQ(CountOccurrences(text, "OpShiftRightArithmetic %v2uint") +
                  CountOccurrences(text, "OpShiftRightArithmetic %uint"),
              2u)
        << text;
    EXPECT_EQ(CountOccurrences(text, "OpShiftLeftLogical %v4int"), 1u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpShiftRightLogical %v4int"), 1u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpShiftRightLogical %int"), 3u) << text;
    for (const char* splat : {"OpCompositeConstruct %v4uint", "OpCompositeConstruct %v2uint"})
        EXPECT_EQ(CountOccurrences(text, splat) - CountOccurrences(original, splat), 1u) << splat << "\n" << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitFieldSExtract"), 0u) << text;
    EXPECT_EQ(CountOccurrences(text, "OpBitFieldUExtract"), 0u) << text;
    EXPECT_NE(text.find("%uint_24"), String::npos) << text;
    EXPECT_NE(text.find("%uint_16"), String::npos) << text;

    EXPECT_EQ(ReflectInputs(output), ReflectInputs(modules[0]));
}

// A width bit exists only for locations 0-31, and shifting a 32-bit value by 32 or more is
// undefined, so an input from location 32 up keeps its fetched value and its Input variable.
TEST(ExtendNarrowIntegerVertexInputsRuntimeWidths, LeavesLocationsFrom32UpAlone) {
    MobileGL::Initialize();
    auto modules = CompileProgram(kVertex, kFragment);
    ASSERT_EQ(modules.size(), 2u);
    Vector<Uint32> input = modules[0];
    MoveLocation(input, 1, 40); // lightAndData
    MoveLocation(input, 3, 33); // single

    Vector<Uint32> output;
    Bool changed = false;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(input, output, changed, true));
    EXPECT_TRUE(changed);
    const String text = Disassemble(output);
    EXPECT_NE(text.find("%lightAndData = OpVariable %_ptr_Input_v4int Input"), String::npos) << text;
    EXPECT_NE(text.find("%single = OpVariable %_ptr_Input_int Input"), String::npos) << text;
    EXPECT_NE(text.find("%texc = OpVariable %_ptr_Private_"), String::npos) << text;

    // Every integer input above the bits: nothing to seed, so nothing changes.
    for (const Uint32 location : {2u, 4u, 5u, 6u})
        MoveLocation(input, location, 50 + location);
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(input, output, changed, true));
    EXPECT_FALSE(changed);
    EXPECT_EQ(output, input);
}

TEST(ExtendNarrowIntegerVertexInputsRuntimeWidths, LeavesAModuleWithoutIntegerInputsAlone) {
    MobileGL::Initialize();
    const auto plain = CompileProgram(kFloatOnlyVertex, kPlainFragment);
    ASSERT_EQ(plain.size(), 2u);
    Vector<Uint32> output;
    Bool changed = true;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(plain[0], output, changed, true));
    EXPECT_FALSE(changed);
    EXPECT_EQ(output, plain[0]);

    const auto modules = CompileProgram(kVertex, kFragment);
    ASSERT_EQ(modules.size(), 2u);
    changed = true;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(modules[1], output, changed, true));
    EXPECT_FALSE(changed) << "a fragment module has no vertex inputs";
    EXPECT_EQ(output, modules[1]);
}

// From SPIR-V 1.4 an entry point lists every global it uses, so the two width globals have to
// join the interface beside the new Inputs and the Private copies.
TEST(ExtendNarrowIntegerVertexInputsRuntimeWidths, ListsTheWidthGlobalsInASpirv14Interface) {
    MobileGL::Initialize();
    const auto modules = CompileProgram(kVertex, kFragment);
    ASSERT_EQ(modules.size(), 2u);
    Vector<Uint32> input = modules[0];
    input[1] = 0x00010400u;
    ASSERT_EQ(ValidationError(input, SPV_ENV_VULKAN_1_2), String{});

    Vector<Uint32> output;
    Bool changed = false;
    ASSERT_TRUE(ShaderCompiler::ExtendNarrowIntegerVertexInputsForEssl(input, output, changed, false));
    EXPECT_TRUE(changed);
    EXPECT_EQ(ValidationError(output, SPV_ENV_VULKAN_1_2), String{});
    const auto entryPoint = LinesWith(Disassemble(output), "OpEntryPoint");
    ASSERT_EQ(entryPoint.size(), 1u);
    for (const char* name : {kNarrowIntegerWidths8GlobalName, kNarrowIntegerWidths16GlobalName, "lightAndData", "texc"})
        EXPECT_NE(entryPoint[0].find(String("%") + name), String::npos) << name << "\n" << entryPoint[0];
}
