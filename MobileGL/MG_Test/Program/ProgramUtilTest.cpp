// MobileGL - MobileGL/MG_Test/Program/ProgramUtilTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <string>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/RenameSamplerFunctionParameterPass.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/glslang/UniformTraverser.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

using namespace MobileGL;

class ProgramUtilTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {}
};

TEST_F(ProgramUtilTest, Sanity) {
    ASSERT_TRUE(true);
}

TEST_F(ProgramUtilTest, RenameSamplerFunctionParameterInSpirvPass) {
    using namespace MG_Util::ShaderTranspiler;

    const String spirvText = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %outColor
               OpExecutionMode %main OriginUpperLeft
               OpName %globalSampler "sampler"
               OpName %paramSampler "sampler"
               OpName %main "main"
               OpDecorate %outColor Location 0
       %void = OpTypeVoid
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
     %mainFn = OpTypeFunction %void
    %paramFn = OpTypeFunction %void %float
  %outV4Ptr = OpTypePointer Output %v4float
 %privatePtr = OpTypePointer Private %float
   %outColor = OpVariable %outV4Ptr Output
%globalSampler = OpVariable %privatePtr Private
     %helper = OpFunction %void None %paramFn
%paramSampler = OpFunctionParameter %float
 %helperBody = OpLabel
               OpReturn
               OpFunctionEnd
       %main = OpFunction %void None %mainFn
   %mainBody = OpLabel
               OpReturn
               OpFunctionEnd
)";

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    Vector<uint32_t> inputBinary;
    ASSERT_TRUE(tools.Assemble(spirvText, &inputBinary));

    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);
    optimizer.RegisterPass(RenameSamplerFunctionParameterPass::CreateRenameSamplerFunctionParameterPass());

    Vector<uint32_t> outputBinary;
    ASSERT_TRUE(optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options));

    String outputText;
    ASSERT_TRUE(tools.Disassemble(outputBinary, &outputText));

    EXPECT_NE(outputText.find("\"MGL_COMPAT_sampler\""), String::npos);

    SizeT exactSamplerNameCount = 0;
    SizeT searchOffset = 0;
    while ((searchOffset = outputText.find("\"sampler\"", searchOffset)) != String::npos) {
        ++exactSamplerNameCount;
        searchOffset += std::strlen("\"sampler\"");
    }
    EXPECT_EQ(exactSamplerNameCount, 1u);
}

TEST_F(ProgramUtilTest, PreprocessLegacyVertexShaderModernizesGlmarkStyleSource) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#define HIGHP_OR_DEFAULT highp
attribute vec3 position;
varying vec2 uv;
uniform HIGHP_OR_DEFAULT mat4 modelViewProjection;

void main() {
    uv = position.xy;
    gl_Position = modelViewProjection * vec4(position, 1.0);
})";

    PreprocessShaderSource(ShaderStage::Vertex, source);

    EXPECT_EQ(source.find("#version 460 core\n"), 0);
    EXPECT_NE(source.find("in vec3 position;"), String::npos);
    EXPECT_NE(source.find("out vec2 uv;"), String::npos);
    EXPECT_EQ(source.find("attribute"), String::npos);
    EXPECT_EQ(source.find("varying"), String::npos);
    EXPECT_EQ(source.find("HIGHP_OR_DEFAULT"), String::npos);
    EXPECT_EQ(source.find("#define"), String::npos);
}

TEST_F(ProgramUtilTest, PreprocessLegacyFragmentShaderModernizesGlmarkStyleSource) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#define MEDIUMP_OR_DEFAULT mediump
varying vec2 uv;
uniform sampler2D texture0;

void main() {
    MEDIUMP_OR_DEFAULT vec4 color = texture2D(texture0, uv);
    gl_FragColor = color;
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_EQ(source.find("#version 460 core\n"), 0);
    EXPECT_NE(source.find("out vec4 mg_FragColor;\n"), String::npos);
    EXPECT_NE(source.find("in vec2 uv;"), String::npos);
    EXPECT_NE(source.find("texture(texture0, uv)"), String::npos);
    EXPECT_NE(source.find("mg_FragColor = color;"), String::npos);
    EXPECT_EQ(source.find("gl_FragColor"), String::npos);
    EXPECT_EQ(source.find("texture2D"), String::npos);
    EXPECT_EQ(source.find("MEDIUMP_OR_DEFAULT"), String::npos);
    EXPECT_EQ(source.find("mediump"), String::npos);
    EXPECT_EQ(source.find("#define"), String::npos);
}

TEST_F(ProgramUtilTest, PreprocessFragmentShaderInjectsDepthRangeShim) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 460 core
out float depth;

void main() {
    depth = gl_DepthRange.diff * 0.5 + gl_DepthRange.near;
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("struct mg_DepthRangeParameters"), String::npos);
    EXPECT_NE(source.find("#define gl_DepthRange mg_DepthRange"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessFragmentShaderRenamesMin3Max3Helpers) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 460 core
out vec4 fragColor;

float min3(float a, float b, float c) { return min(min(a, b), c); }
float max3(float a, float b, float c) { return max(max(a, b), c); }

void main() {
    float dark = min3(0.1, 0.2, 0.3);
    float bright = max3(max3(0.1, 0.2, 0.3), 0.4, 0.5);
    fragColor = vec4(dark, bright, 0.0, 1.0);
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("float mg_min3("), String::npos);
    EXPECT_NE(source.find("float mg_max3("), String::npos);
    EXPECT_NE(source.find("mg_min3(0.1, 0.2, 0.3)"), String::npos);
    EXPECT_NE(source.find("mg_max3(mg_max3(0.1, 0.2, 0.3), 0.4, 0.5)"), String::npos);
    EXPECT_EQ(source.find("float min3("), String::npos);
    EXPECT_EQ(source.find("float max3("), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

const char* vs = R"(#version 150

in vec4 Position;

uniform mat4 ProjMat;
uniform vec2 InSize;
uniform vec2 OutSize;

out vec2 texCoord;
out vec2 oneTexel;

void main(){
    vec4 outPos = ProjMat * vec4(Position.xy, 0.0, 1.0);
    gl_Position = vec4(outPos.xy, 0.2, 1.0);

    oneTexel = 1.0 / InSize;

    texCoord = Position.xy / OutSize;
})";

TEST_F(ProgramUtilTest, CompileSimpleVertexShader) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        ASSERT_NE(res.error().errc, 0);
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log;
    }
}

const char* fs = R"(#version 150

uniform sampler2D InSampler;

in vec2 texCoord;
in vec2 oneTexel;

uniform vec2 InSize;

uniform vec3 Gray;
uniform vec3 RedMatrix;
uniform vec3 GreenMatrix;
uniform vec3 BlueMatrix;
uniform vec3 Offset;
uniform vec3 ColorScale;
uniform float Saturation;

out vec4 fragColor;

void main() {
    vec4 InTexel = texture(InSampler, texCoord);

    // Color Matrix
    float RedValue = dot(InTexel.rgb, RedMatrix);
    float GreenValue = dot(InTexel.rgb, GreenMatrix);
    float BlueValue = dot(InTexel.rgb, BlueMatrix);
    vec3 OutColor = vec3(RedValue, GreenValue, BlueValue);

    // Offset & Scale
    OutColor = (OutColor * ColorScale) + Offset;

    // Saturation
    float Luma = dot(OutColor, Gray);
    vec3 Chroma = OutColor - Luma;
    OutColor = (Chroma * Saturation) + Luma;

    fragColor = vec4(OutColor, 1.0);
})";

const char* daily_weather_variation_vs = R"(#version 150

struct DailyWeatherVariation {
    vec2 clouds_cumulus_coverage;
    vec2 clouds_altocumulus_coverage;
    vec2 clouds_cirrus_coverage;
    float clouds_cumulus_congestus_amount;
    float clouds_stratus_amount;
    float fogginess;
    float aurora_amount;
    float nlc_amount;
    mat2x3 aurora_colors;
};

in vec4 Position;
out DailyWeatherVariation daily_weather_variation;

DailyWeatherVariation get_daily_weather_variation() {
    DailyWeatherVariation daily_weather_variation;
    daily_weather_variation.clouds_cumulus_coverage = vec2(1.0, 2.0);
    daily_weather_variation.clouds_altocumulus_coverage = vec2(3.0, 4.0);
    daily_weather_variation.clouds_cirrus_coverage = vec2(5.0, 6.0);
    daily_weather_variation.clouds_cumulus_congestus_amount = 7.0;
    daily_weather_variation.clouds_stratus_amount = 8.0;
    daily_weather_variation.fogginess = 9.0;
    daily_weather_variation.aurora_amount = 10.0;
    daily_weather_variation.nlc_amount = 11.0;
    daily_weather_variation.aurora_colors = mat2x3(vec3(12.0, 13.0, 14.0), vec3(15.0, 16.0, 17.0));
    return daily_weather_variation;
}

void main() {
    gl_Position = Position;
    daily_weather_variation = get_daily_weather_variation();
})";

const char* daily_weather_variation_fs = R"(#version 150

struct DailyWeatherVariation {
    vec2 clouds_cumulus_coverage;
    vec2 clouds_altocumulus_coverage;
    vec2 clouds_cirrus_coverage;
    float clouds_cumulus_congestus_amount;
    float clouds_stratus_amount;
    float fogginess;
    float aurora_amount;
    float nlc_amount;
    mat2x3 aurora_colors;
};

in DailyWeatherVariation daily_weather_variation;
out vec4 fragColor;

void main() {
    vec3 aurora = daily_weather_variation.aurora_colors[1];
    DailyWeatherVariation variation = daily_weather_variation;
    vec2 coverage = variation.clouds_cumulus_coverage + daily_weather_variation.clouds_altocumulus_coverage;
    fragColor = vec4(coverage, aurora.x + variation.aurora_amount, 1.0);
})";

TEST_F(ProgramUtilTest, CompileSimpleFragmentShader) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        ASSERT_NE(res.error().errc, 0);
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log;
    }
}

const char* position_color_fsh = R"(#version 150

in vec4 vertexColor;

uniform vec4 ColorModulator;

out vec4 fragColor;

void main() {
    vec4 color = vertexColor;
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
})";

TEST_F(ProgramUtilTest, CompileFragmentShaderWithDiscard) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = position_color_fsh};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        ASSERT_NE(res.error().errc, 0);
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log;
    }

    ProgramAttrib programAttrib{// .shaderTypes = { GL_FRAGMENT_SHADER },
                                .shaders = {res.value()}};

    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) {
        ASSERT_NE(program_res.error().errc, 0);
        FAIL() << "errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;
    }

    auto program = program_res.value();

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_FRAGMENT_SHADER},
        .program = *program,
    };
    auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);

    auto spirvs = bin_res.value();

    Vector<SpvcSession> sessions(spirvs.size());
    for (SizeT i = 0; i < spirvs.size(); ++i) {
        sessions[i] = SpvcSession(spirvs[i], SessionUsageBit::Transpile);
    }

    for (SizeT i = 0; i < spirvs.size(); ++i) {
        std::cout << "Decompiling " << MG_Util::ConvertGLEnumToString(binaryAttrib.shaderTypes[i]) << std::endl;
        auto src = ShaderCompiler::DecompileShader(sessions[i]);
        if (!src) {
            ASSERT_NE(src.error().errc, 0);
            FAIL() << "errc: " << src.error().errc << "\nlog: " << src.error().log;
        } else {
            std::cout << src.value() << std::endl;
        }

        if (src.value().find("demote") != std::string::npos) {
            FAIL() << "Found unsupported demote!";
        }
    }
}

const char* vs_location = R"(#version 460

in vec4 Position;

layout(location = 1) uniform mat4 ProjMat;
layout(location = 20) uniform vec2 InSize;
uniform vec2 OutSize;

out vec2 texCoord;
out vec2 oneTexel;

void main(){
    vec4 outPos = ProjMat * vec4(Position.xy, 0.0, 1.0);
    gl_Position = vec4(outPos.xy, 0.2, 1.0);

    oneTexel = 1.0 / InSize;

    texCoord = Position.xy / OutSize;
})";

TEST_F(ProgramUtilTest, CompileVertexShaderWithLocation) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib attrib{
        .shaderType = GL_VERTEX_SHADER, .sourceStr = vs_location, .flags = ShaderCompileBits::CompileForOpenGL};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        ASSERT_NE(res.error().errc, 0);
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log;
    }
    UnorderedMap<String, Int> uniforms;

    auto pShader = res.value();
    auto root = pShader->getIntermediate()->getTreeRoot();
    UniformTraverser traverser;
    root->traverse(&traverser);
    auto& symbols = traverser.GetCollectedSymbols();
    for (const auto& symbol : symbols) {
        uniforms[symbol->getName().c_str()] = symbol->getQualifier().layoutLocation;
    }

    EXPECT_EQ(uniforms["ProjMat"], 1);
    EXPECT_EQ(uniforms["InSize"], 20);
    EXPECT_EQ(uniforms["OutSize"], 4095);
}

TEST_F(ProgramUtilTest, CompileAndLinkProgram) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib vs_attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vs};
    auto vs_res = ShaderCompiler::CompileShader(vs_attrib);
    if (!vs_res) {
        ASSERT_NE(vs_res.error().errc, 0);
        FAIL() << "errc: " << vs_res.error().errc << "\nlog: " << vs_res.error().log;
    }

    ShaderAttrib fs_attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto fs_res = ShaderCompiler::CompileShader(fs_attrib);
    if (!fs_res) {
        ASSERT_NE(fs_res.error().errc, 0);
        FAIL() << "errc: " << fs_res.error().errc << "\nlog: " << fs_res.error().log;
    }

    ProgramAttrib programAttrib{// .shaderTypes = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER },
                                .shaders = {vs_res.value(), fs_res.value()}};

    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) {
        ASSERT_NE(program_res.error().errc, 0);
        FAIL() << "errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;
    }
}

TEST_F(ProgramUtilTest, DecompProgram) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib vs_attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vs};
    auto vs_res = ShaderCompiler::CompileShader(vs_attrib);
    if (!vs_res) {
        ASSERT_NE(vs_res.error().errc, 0);
        FAIL() << "errc: " << vs_res.error().errc << "\nlog: " << vs_res.error().log;
    }

    ShaderAttrib fs_attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto fs_res = ShaderCompiler::CompileShader(fs_attrib);
    if (!fs_res) {
        ASSERT_NE(fs_res.error().errc, 0);
        FAIL() << "errc: " << fs_res.error().errc << "\nlog: " << fs_res.error().log;
    }

    ProgramAttrib programAttrib{// .shaderTypes = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER },
                                .shaders = {vs_res.value(), fs_res.value()}};

    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) {
        ASSERT_NE(program_res.error().errc, 0);
        FAIL() << "errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;
    }

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
        .program = *program_res.value(),
    };
    auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);

    auto spirvs = bin_res.value();

    Vector<SpvcSession> sessions(spirvs.size());
    for (SizeT i = 0; i < spirvs.size(); ++i) {
        sessions[i] = SpvcSession(spirvs[i], SessionUsageBit::Transpile);
    }

    for (SizeT i = 0; i < spirvs.size(); ++i) {
        std::cout << "Decompiling " << MG_Util::ConvertGLEnumToString(binaryAttrib.shaderTypes[i]) << std::endl;
        auto src = ShaderCompiler::DecompileShader(sessions[i]);
        if (!src) {
            ASSERT_NE(src.error().errc, 0);
            FAIL() << "errc: " << src.error().errc << "\nlog: " << src.error().log;
        } else {
            std::cout << src.value() << std::endl;
        }
    }

    // spirv link check
    auto vs_outputs = sessions[0].GetShaderInterface(SPVC_RESOURCE_TYPE_STAGE_OUTPUT);
    auto fs_inputs = sessions[1].GetShaderInterface(SPVC_RESOURCE_TYPE_STAGE_INPUT);

    ASSERT_EQ(vs_outputs.size(), fs_inputs.size());

    for (size_t i = 0; i < vs_outputs.size(); ++i) {
        EXPECT_EQ(vs_outputs[i].location, fs_inputs[i].location);
    }

    auto vs_uniforms = sessions[0].GetShaderInterface(SPVC_RESOURCE_TYPE_GL_PLAIN_UNIFORM);
    auto fs_uniforms = sessions[1].GetShaderInterface(SPVC_RESOURCE_TYPE_GL_PLAIN_UNIFORM);

    std::unordered_map<std::string, uint32_t> uniform_locations;
    for (const auto& uniform : vs_uniforms) {
        uniform_locations[uniform.name] = uniform.location;
    }

    for (const auto& uniform : fs_uniforms) {
        auto it = uniform_locations.find(uniform.name);
        if (it != uniform_locations.end()) {
            EXPECT_EQ(it->second, uniform.location);
        }
    }

    auto vs_samplers = sessions[0].GetShaderInterface(SPVC_RESOURCE_TYPE_SAMPLED_IMAGE);
    auto fs_samplers = sessions[1].GetShaderInterface(SPVC_RESOURCE_TYPE_SAMPLED_IMAGE);

    std::unordered_map<std::string, uint32_t> sampler_locations;
    for (const auto& uniform : vs_uniforms) {
        sampler_locations[uniform.name] = uniform.location;
    }

    for (const auto& uniform : fs_uniforms) {
        auto it = sampler_locations.find(uniform.name);
        if (it != sampler_locations.end()) {
            EXPECT_EQ(it->second, uniform.location);
        }
    }

    auto& meta0 = sessions[0].GetMetadata();
    auto& meta1 = sessions[1].GetMetadata();

    for (auto& [name, offset] : meta0.plainUniformOffsetsInUBO) {
        printf("%s: \t%u\n", name.c_str(), offset);
    }

    printf("\n");

    for (auto& [name, offset] : meta1.plainUniformOffsetsInUBO) {
        printf("%s: \t%u\n", name.c_str(), offset);
    }

    EXPECT_EQ(meta0.plainUniformOffsetsInUBO.size(), meta1.plainUniformOffsetsInUBO.size());
    for (auto& [name, offset] : meta0.plainUniformOffsetsInUBO) {
        EXPECT_EQ(offset, meta1.plainUniformOffsetsInUBO.at(name));
    }
}

TEST_F(ProgramUtilTest, FlattenDailyWeatherVariationInterfaceInSpirvPass) {
    using namespace MG_Util::ShaderTranspiler;

    String vsSource = daily_weather_variation_vs;
    String fsSource = daily_weather_variation_fs;
    PreprocessShaderSource(ShaderStage::Vertex, vsSource);
    PreprocessShaderSource(ShaderStage::Fragment, fsSource);

    ShaderAttrib vsAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vsSource};
    auto vsRes = ShaderCompiler::CompileShader(vsAttrib);
    if (!vsRes) {
        ASSERT_NE(vsRes.error().errc, 0);
        FAIL() << "errc: " << vsRes.error().errc << "\nlog: " << vsRes.error().log;
    }

    ShaderAttrib fsAttrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fsSource};
    auto fsRes = ShaderCompiler::CompileShader(fsAttrib);
    if (!fsRes) {
        ASSERT_NE(fsRes.error().errc, 0);
        FAIL() << "errc: " << fsRes.error().errc << "\nlog: " << fsRes.error().log;
    }

    ProgramAttrib programAttrib{.shaders = {vsRes.value(), fsRes.value()}};
    auto programRes = ShaderCompiler::LinkProgram(programAttrib);
    if (!programRes) {
        ASSERT_NE(programRes.error().errc, 0);
        FAIL() << "errc: " << programRes.error().errc << "\nlog: " << programRes.error().log;
    }

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
        .program = *programRes.value(),
    };
    auto binRes = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binRes.has_value());

    Vector<Vector<uint32_t>> optimizedSpirvs;
    optimizedSpirvs.reserve(binRes->size());
    for (const auto& spirv : binRes.value()) {
        Vector<uint32_t> optimized;
        ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(spirv, optimized));
        optimizedSpirvs.push_back(std::move(optimized));
    }

    Vector<SpvcSession> sessions(optimizedSpirvs.size());
    for (SizeT i = 0; i < optimizedSpirvs.size(); ++i) {
        sessions[i] = SpvcSession(optimizedSpirvs[i], SessionUsageBit::Transpile);
    }

    auto vertexSource = ShaderCompiler::DecompileShader(sessions[0]);
    auto fragmentSource = ShaderCompiler::DecompileShader(sessions[1]);
    ASSERT_TRUE(vertexSource.has_value());
    ASSERT_TRUE(fragmentSource.has_value());

    EXPECT_EQ(vertexSource->find("out DailyWeatherVariation "), std::string::npos);
    EXPECT_EQ(fragmentSource->find("in DailyWeatherVariation "), std::string::npos);
    EXPECT_NE(vertexSource->find("daily_weather_variation_clouds_cumulus_coverage"), std::string::npos);
    EXPECT_NE(vertexSource->find("daily_weather_variation_aurora_colors"), std::string::npos);
    EXPECT_NE(fragmentSource->find("daily_weather_variation_clouds_altocumulus_coverage"), std::string::npos);
    EXPECT_NE(fragmentSource->find("daily_weather_variation_aurora_colors"), std::string::npos);

    const struct ExpectedInterface {
        const char* name;
        uint32_t location;
    } expectedInterfaces[] = {
        {"daily_weather_variation_clouds_cumulus_coverage", 0},
        {"daily_weather_variation_clouds_altocumulus_coverage", 1},
        {"daily_weather_variation_clouds_cirrus_coverage", 2},
        {"daily_weather_variation_clouds_cumulus_congestus_amount", 3},
        {"daily_weather_variation_clouds_stratus_amount", 4},
        {"daily_weather_variation_fogginess", 5},
        {"daily_weather_variation_aurora_amount", 6},
        {"daily_weather_variation_nlc_amount", 7},
        {"daily_weather_variation_aurora_colors", 8},
    };

    const auto vsOutputs = sessions[0].GetShaderInterface(SPVC_RESOURCE_TYPE_STAGE_OUTPUT);
    const auto fsInputs = sessions[1].GetShaderInterface(SPVC_RESOURCE_TYPE_STAGE_INPUT);
    ASSERT_EQ(vsOutputs.size(), std::size(expectedInterfaces));
    ASSERT_EQ(fsInputs.size(), std::size(expectedInterfaces));

    for (const auto& expected : expectedInterfaces) {
        bool foundVertex = false;
        for (const auto& output : vsOutputs) {
            if (output.name == expected.name) {
                foundVertex = true;
                EXPECT_EQ(output.location, expected.location);
                break;
            }
        }
        EXPECT_TRUE(foundVertex) << "missing vertex output: " << expected.name;

        bool foundFragment = false;
        for (const auto& input : fsInputs) {
            if (input.name == expected.name) {
                foundFragment = true;
                EXPECT_EQ(input.location, expected.location);
                break;
            }
        }
        EXPECT_TRUE(foundFragment) << "missing fragment input: " << expected.name;
    }
}

const char* blit_vs = R"(#version 460 core

in vec3 Position;
in vec2 UV0;

uniform mat4 ModelViewMat;
uniform mat4 ProjMat;

out vec2 texCoord0;

void main() {
    gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);

    texCoord0 = UV0;
}
)";

const char* blit_fs = R"(#version 460 core

uniform sampler2D Sampler0;

uniform vec4 ColorModulator;

in vec2 texCoord0;

out vec4 fragColor;

void main() {
    vec4 color = texture(Sampler0, texCoord0);
    if (color.a == 0.0) {
        discard;
    }
    fragColor = color * ColorModulator;
})";

TEST_F(ProgramUtilTest, CompileAndLinkBlitProgram) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib vs_attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = blit_vs};
    auto vs_res = ShaderCompiler::CompileShader(vs_attrib);
    if (!vs_res) {
        ASSERT_NE(vs_res.error().errc, 0);
        FAIL() << "errc: " << vs_res.error().errc << "\nlog: " << vs_res.error().log;
    }

    ShaderAttrib fs_attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = blit_fs};
    auto fs_res = ShaderCompiler::CompileShader(fs_attrib);
    if (!fs_res) {
        ASSERT_NE(fs_res.error().errc, 0);
        FAIL() << "errc: " << fs_res.error().errc << "\nlog: " << fs_res.error().log;
    }

    UnorderedMap<String, Uint> attribLocations;
    attribLocations["Position"] = 0;
    attribLocations["UV0"] = 2;

    ProgramAttrib programAttrib{// .shaderTypes = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER },
                                .shaders = {vs_res.value(), fs_res.value()},
                                .explicitVertexInLocations = attribLocations};

    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) {
        ASSERT_NE(program_res.error().errc, 0);
        FAIL() << "errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;
    }
    auto program = program_res.value();
    program->buildReflection();
    auto inCnt = program->getNumPipeInputs();
    for (int i = 0; i < inCnt; i++) {
        auto& in = program->getPipeInput(i);
        auto it = attribLocations.find(in.name);
        if (it != attribLocations.end()) {
            ASSERT_EQ(it->second, in.layoutLocation());
            std::cout << in.name << ": location = " << it->second << "\n";
            attribLocations.erase(it);
        }
    }

    ASSERT_TRUE(attribLocations.empty()) << "Not all vertex input location mapped!";

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER},
        .program = *program,
    };
    auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);

    auto spirvs = bin_res.value();
    Vector<SpvcSession> sessions(spirvs.size());
    for (SizeT i = 0; i < spirvs.size(); ++i) {
        sessions[i] = SpvcSession(spirvs[i], SessionUsageBit::Transpile);
    }

    for (SizeT i = 0; i < spirvs.size(); ++i) {
        std::cout << "Decompiling " << MG_Util::ConvertGLEnumToString(binaryAttrib.shaderTypes[i]) << std::endl;
        auto src = ShaderCompiler::DecompileShader(sessions[i]);
        if (!src) {
            ASSERT_NE(src.error().errc, 0);
            FAIL() << "errc: " << src.error().errc << "\nlog: " << src.error().log;
        } else {
            std::cout << "src: " << src.value() << std::endl;
        }
    }
}
