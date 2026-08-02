// MobileGL - MobileGL/MG_Test/Program/ProgramUtilTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>

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

TEST_F(ProgramUtilTest, UnformattedFloatStorageImagesKeepIntegerAtomicImagesTyped) {
    using namespace MG_Util::ShaderTranspiler;

    const String source = R"(#version 430 core
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(rgba16, binding = 0) uniform image2D floatImage;
layout(r32ui, binding = 1) uniform uimage2D atomicImage;

void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    imageStore(floatImage, coordinate, imageLoad(floatImage, coordinate));
    imageAtomicAdd(atomicImage, coordinate, 1u);
}
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_COMPUTE_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;

    ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
    auto programResult = ShaderCompiler::LinkProgram(programAttrib);
    ASSERT_TRUE(programResult) << programResult.error().log;

    ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_COMPUTE_SHADER}, .program = *programResult.value()};
    auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binaryResult) << binaryResult.error().log;
    ASSERT_EQ(binaryResult->size(), 1u);
    const auto& inputBinary = binaryResult->front();

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String inputText;
    ASSERT_TRUE(tools.Disassemble(inputBinary, &inputText));
    EXPECT_NE(inputText.find("2D 0 0 0 2 Rgba16"), String::npos) << inputText;
    EXPECT_NE(inputText.find("2D 0 0 0 2 R32ui"), String::npos) << inputText;
    EXPECT_EQ(inputText.find("StorageImageReadWithoutFormat"), String::npos) << inputText;
    EXPECT_EQ(inputText.find("StorageImageWriteWithoutFormat"), String::npos) << inputText;

    Vector<Uint32> outputBinary;
    ASSERT_TRUE(ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(inputBinary, outputBinary));

    String outputText;
    ASSERT_TRUE(tools.Disassemble(outputBinary, &outputText));
    EXPECT_EQ(outputText.find("2D 0 0 0 2 Rgba16"), String::npos) << outputText;
    EXPECT_NE(outputText.find("2D 0 0 0 2 Unknown"), String::npos) << outputText;
    EXPECT_NE(outputText.find("2D 0 0 0 2 R32ui"), String::npos) << outputText;

    const auto countOccurrences = [](const String& text, const String& needle) {
        SizeT count = 0;
        for (SizeT offset = 0; (offset = text.find(needle, offset)) != String::npos;
             offset += needle.size()) {
            ++count;
        }
        return count;
    };
    EXPECT_EQ(countOccurrences(outputText, "OpCapability StorageImageReadWithoutFormat"), 1u)
        << outputText;
    EXPECT_EQ(countOccurrences(outputText, "OpCapability StorageImageWriteWithoutFormat"), 1u)
        << outputText;
    EXPECT_TRUE(tools.Validate(outputBinary));

    Vector<Uint32> secondOutputBinary;
    ASSERT_TRUE(ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(outputBinary, secondOutputBinary));
    EXPECT_EQ(secondOutputBinary, outputBinary);
}

TEST_F(ProgramUtilTest, UnformattedFloatStorageImagesKeepFloatAtomicImageTypesTyped) {
    using namespace MG_Util::ShaderTranspiler;

    const String spirvText = R"(
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %target DescriptorSet 0
               OpDecorate %target Binding 0
       %void = OpTypeVoid
      %float = OpTypeFloat 32
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
      %image = OpTypeImage %float 2D 0 0 0 2 R32f
%imageUniformPtr = OpTypePointer UniformConstant %image
%imageTexelPtr = OpTypePointer Image %float
 %mainType = OpTypeFunction %void
       %zero = OpConstant %int 0
 %coordinate = OpConstantComposite %v2int %zero %zero
     %target = OpVariable %imageUniformPtr UniformConstant
       %main = OpFunction %void None %mainType
      %entry = OpLabel
   %texelPtr = OpImageTexelPointer %imageTexelPtr %target %coordinate %zero
               OpReturn
               OpFunctionEnd
)";

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    Vector<Uint32> inputBinary;
    ASSERT_TRUE(tools.Assemble(spirvText, &inputBinary));

    Vector<Uint32> outputBinary;
    ASSERT_TRUE(ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(inputBinary, outputBinary));

    String outputText;
    ASSERT_TRUE(tools.Disassemble(outputBinary, &outputText));
    EXPECT_NE(outputText.find("2D 0 0 0 2 R32f"), String::npos) << outputText;
    EXPECT_EQ(outputText.find("StorageImageReadWithoutFormat"), String::npos) << outputText;
    EXPECT_EQ(outputText.find("StorageImageWriteWithoutFormat"), String::npos) << outputText;
    String validationDiagnostics;
    tools.SetMessageConsumer([&validationDiagnostics](spv_message_level_t, const char*,
                                                       const spv_position_t&, const char* message) {
        validationDiagnostics += message;
    });
    EXPECT_TRUE(tools.Validate(outputBinary)) << validationDiagnostics;
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

    EXPECT_EQ(source.find("#version 330 core "), 0);
    EXPECT_NE(source.find("in vec3 position;"), String::npos);
    EXPECT_NE(source.find("out vec2 uv;"), String::npos);
    EXPECT_EQ(source.find("attribute"), String::npos);
    EXPECT_EQ(source.find("varying"), String::npos);
    // Precision-qualifier macros are left for glslang's own preprocessor to expand.
    EXPECT_NE(source.find("#define HIGHP_OR_DEFAULT highp"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// KHR-GL33.shaders.preprocessor.* — a block comment is one preprocessing token that the C/GLSL
// preprocessor replaces with a single space, even when it spans newlines inside a directive. glslang
// handles this natively, so MobileGL must not mangle it. These reproduce the CTS cases that failed
// because comment blanking preserved the interior newline, truncating multi-line #define bodies.
static void ExpectCompiles(MobileGL::ShaderStage stage, GLenum glStage, MobileGL::String source) {
    using namespace MG_Util::ShaderTranspiler;
    PreprocessShaderSource(stage, source);
    ShaderAttrib attrib{.shaderType = glStage, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessMultilineCommentInDefineBodyCompiles) {
    ExpectCompiles(ShaderStage::Fragment, GL_FRAGMENT_SHADER,
                   R"(#version 330
precision mediump float;
out float out0;
#define VALUE /* current
            value */ 4.2

void main()
{
    out0 = VALUE;
})");
}

TEST_F(ProgramUtilTest, PreprocessRedefineObjectMultilineCommentCompiles) {
    ExpectCompiles(ShaderStage::Fragment, GL_FRAGMENT_SHADER,
                   R"(#version 330
precision mediump float;
out float out0;
#    define  VAL1 1.0
#define        VAL2 2.0

#define RES2 /* fdsjklfdsjkl
                dsfjkhfdsjkh
                fdsjklhfdsjkh */ (RES1 * VAL2)
#define RES1    (VAL2 / VAL1)
#define RES2    /* ewrlkjhsadf */ (RES1 * VAL2)
#define VALUE    (RES2 + RES1)

void main()
{
    out0 = VALUE;
})");
}

TEST_F(ProgramUtilTest, PreprocessFunctionMacroRedefinitionMultilineCommentCompiles) {
    ExpectCompiles(ShaderStage::Fragment, GL_FRAGMENT_SHADER,
                   R"(#version 330
precision mediump float;
out float out0;
# define FUNC(a,b)        (a  +b)
# define FUNC(a,b)(a    /* comment
                         */ +b)

void main()
{
    out0 = FUNC(1.0, 2.0);
})");
}

// Note: KHR-GL3x.shaders.preprocessor.conditional_inclusion.basic_2 (`#define AAA defined(BBB)` used
// in `#if !AAA`) is intentionally NOT handled here. Generating the `defined` operator via macro
// expansion is undefined per the C/GLSL preprocessor spec, and glslang deliberately rejects it
// ("'defined' : cannot use in preprocessor expression when expanded from macros"). Making it pass
// would require MobileGL to run its own macro expansion ahead of glslang, which is exactly the
// preprocessing we defer to glslang; the two cases stay failing by design.

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

    EXPECT_EQ(source.find("#version 330 core "), 0);
    EXPECT_NE(source.find("out vec4 mg_FragColor;\n"), String::npos);
    EXPECT_NE(source.find("in vec2 uv;"), String::npos);
    EXPECT_NE(source.find("texture(texture0, uv)"), String::npos);
    EXPECT_NE(source.find("mg_FragColor = color;"), String::npos);
    EXPECT_EQ(source.find("gl_FragColor"), String::npos);
    EXPECT_EQ(source.find("texture2D"), String::npos);
    // Precision-qualifier macros are left for glslang's own preprocessor to expand.
    EXPECT_NE(source.find("#define MEDIUMP_OR_DEFAULT mediump"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessMinecraft112BlurShaderKeepsLegacySampleIdentifier) {
    using namespace MG_Util::ShaderTranspiler;

    // assets/minecraft/shaders/program/blur.fsh from the unmodified Minecraft 1.12 client jar.
    String source = R"(#version 120

uniform sampler2D DiffuseSampler;

varying vec2 texCoord;
varying vec2 oneTexel;

uniform vec2 InSize;

uniform vec2 BlurDir;
uniform float Radius;

void main() {
    vec4 blurred = vec4(0.0);
    float totalStrength = 0.0;
    float totalAlpha = 0.0;
    float totalSamples = 0.0;
    for(float r = -Radius; r <= Radius; r += 1.0) {
        vec4 sample = texture2D(DiffuseSampler, texCoord + oneTexel * r * BlurDir);

		// Accumulate average alpha
        totalAlpha = totalAlpha + sample.a;
        totalSamples = totalSamples + 1.0;

		// Accumulate smoothed blur
        float strength = 1.0 - abs(r / Radius);
        totalStrength = totalStrength + strength;
        blurred = blurred + sample;
    }
    gl_FragColor = vec4(blurred.rgb / (Radius * 2.0 + 1.0), totalAlpha);
}
)";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_EQ(source.find("#version 330 core "), 0);
    EXPECT_NE(source.find("vec4 sample = texture(DiffuseSampler"), String::npos);
    EXPECT_NE(source.find("totalAlpha = totalAlpha + sample.a;"), String::npos);
    EXPECT_NE(source.find("float totalSamples = 0.0;"), String::npos);
    EXPECT_NE(source.find("totalSamples = totalSamples + 1.0;"), String::npos);
    EXPECT_NE(source.find("blurred = blurred + sample;"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessLegacySampleInterfaceIdentifiersKeepNames) {
    using namespace MG_Util::ShaderTranspiler;

    String vertexSource = R"(#version 150
attribute vec3 sample;

void main() {
    gl_Position = vec4(sample, 1.0);
}
)";
    PreprocessShaderSource(ShaderStage::Vertex, vertexSource);

    EXPECT_EQ(vertexSource.find("#version 330 core "), 0);
    EXPECT_NE(vertexSource.find("in vec3 sample;"), String::npos);

    ShaderAttrib vertexAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vertexSource};
    auto vertexResult = ShaderCompiler::CompileShader(vertexAttrib);
    if (!vertexResult) {
        FAIL() << "errc: " << vertexResult.error().errc << "\nlog: " << vertexResult.error().log
               << "\nsource:\n" << vertexSource;
    }

    String fragmentSource = R"(#version 150
uniform sampler2D sample;
varying vec2 texCoord;

void main() {
    gl_FragColor = texture2D(sample, texCoord);
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, fragmentSource);

    EXPECT_EQ(fragmentSource.find("#version 330 core "), 0);
    EXPECT_NE(fragmentSource.find("uniform sampler2D sample;"), String::npos);
    EXPECT_NE(fragmentSource.find("texture(sample, texCoord)"), String::npos);

    ShaderAttrib fragmentAttrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fragmentSource};
    auto fragmentResult = ShaderCompiler::CompileShader(fragmentAttrib);
    if (!fragmentResult) {
        FAIL() << "errc: " << fragmentResult.error().errc << "\nlog: " << fragmentResult.error().log
               << "\nsource:\n" << fragmentSource;
    }
}

TEST_F(ProgramUtilTest, PreprocessEsslVersionsRemainVulkanCompatible) {
    using namespace MG_Util::ShaderTranspiler;

    const auto verifyVersion = [](const char* inputVersion, const char* expectedVersion) {
        SCOPED_TRACE(inputVersion);
        String source = inputVersion;
        source += R"(
precision mediump float;
out vec4 fragColor;

void main() {
    fragColor = vec4(1.0);
}
)";
        PreprocessShaderSource(ShaderStage::Fragment, source);

        EXPECT_EQ(source.find(expectedVersion), 0);

        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
        }
    };

    // Preserve the pre-existing desktop-core route: the current resource table cannot parse ESSL built-ins.
    verifyVersion("#version 300 es", "#version 460 core\n");
    verifyVersion("#version 310 es", "#version 460 core\n");
}

TEST_F(ProgramUtilTest, PreprocessModernDesktopVersionsRecognizesUtf8Bom) {
    using namespace MG_Util::ShaderTranspiler;

    const auto verifyVersion = [](const char* inputVersion) {
        SCOPED_TRACE(inputVersion);
        String source = "\xef\xbb\xbf";
        source += inputVersion;
        source += R"(
out vec4 fragColor;

void main() {
    fragColor = vec4(1.0);
}
)";
        PreprocessShaderSource(ShaderStage::Fragment, source);

        // An explicitly declared modern core version keeps its number (see "keep declared modern
        // GLSL versions strict"); only the BOM goes.
        EXPECT_EQ(source.find(String(inputVersion) + "\n"), 0);
        EXPECT_EQ(source.find("\xef\xbb\xbf"), String::npos);

        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
        }
    };

    verifyVersion("#version 400 core");
    verifyVersion("#version 460 core");
}

// KHR-GL33.shaders.preprocessor.directive.version_* (also re-run verbatim under GL40-GL44): the
// compiler must REJECT a malformed #version line. MobileGL used to rewrite the whole line to
// "#version 330 core" whenever it could scrape a leading integer - or treat an unknown profile token
// as core - which silently legalized every form below. CTS compiles the shader's own #version
// verbatim, so the rejection has to survive preprocessing (and the 460 retry).
TEST_F(ProgramUtilTest, PreprocessRejectsMalformedVersionDirectives) {
    using namespace MG_Util::ShaderTranspiler;

    const char* body = "\nout vec4 fragColor;\nvoid main() { fragColor = vec4(1.0); }\n";
    const auto rejects = [](const String& fullSource) {
        String src = fullSource;
        PreprocessShaderSource(ShaderStage::Fragment, src);
        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = src};
        auto res = ShaderCompiler::CompileShader(attrib);
        return res ? false : true;  // "rejects" == compile failed
    };

    // Silently legalized today - the five this fix must flip to rejection:
    EXPECT_TRUE(rejects(String("#version 329") + body))        << "329 is not a real version";
    EXPECT_TRUE(rejects(String("#version 331") + body))        << "331 is not a real version";
    EXPECT_TRUE(rejects(String("#version 330 foo") + body))    << "unknown profile keyword";
    EXPECT_TRUE(rejects(String("#version 330.0") + body))      << "float literal, not an int token";
    EXPECT_TRUE(rejects(String("#version 330 foobar") + body)) << "trailing tokens after a valid decl";

    // Already rejected (no leading integer, or #version is not the first token) - pinned so a future
    // change to the normalizer cannot start legalizing them either:
    EXPECT_TRUE(rejects(String("#version") + body))            << "missing version number";
    EXPECT_TRUE(rejects(String("#version foobar") + body))     << "identifier where the int belongs";
    EXPECT_TRUE(rejects(String("#version AAA") + body))        << "identifier where the int belongs";
    EXPECT_TRUE(rejects(String("precision mediump float;\n#version 330") + body))
        << "#version must be the first statement";
    EXPECT_TRUE(rejects(String("#define FOO BAR\n#version 330") + body))
        << "#version must precede a #define";
}

// The PASS half of the same CTS group: a valid decl, and #version preceded only by whitespace or a
// comment, must still compile. Guards the fix above from over-rejecting.
TEST_F(ProgramUtilTest, PreprocessKeepsValidVersionDirectivesCompiling) {
    using namespace MG_Util::ShaderTranspiler;

    const char* body = "\nout vec4 fragColor;\nvoid main() { fragColor = vec4(1.0); }\n";
    const auto compiles = [](const String& fullSource) {
        String src = fullSource;
        PreprocessShaderSource(ShaderStage::Fragment, src);
        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = src};
        auto res = ShaderCompiler::CompileShader(attrib);
        return res ? true : false;
    };

    EXPECT_TRUE(compiles(String("#version 330 core") + body));
    EXPECT_TRUE(compiles(String("\n#version 330 core") + body))
        << "leading whitespace is legal before #version";
    EXPECT_TRUE(compiles(String("// test\n#version 330 core") + body))
        << "a leading comment is legal before #version";
}

TEST_F(ProgramUtilTest, PreprocessUsesRealSpacedVersionDirectiveForInjectedOutput) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(// #version 460 core
/* "#version 400 core" */
#line 7 "#version 460 core"
# version 120
varying vec2 uv;

void main() {
    gl_FragColor = vec4(uv, 0.0, 1.0);
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    const SizeT versionPos = source.find("#version 330 core ");
    const SizeT outputPos = source.find("out vec4 mg_FragColor;\n");
    EXPECT_NE(versionPos, String::npos);
    // The normalized directive carries a marker recording that this 330 came from a legacy
    // declaration, so measure the line rather than assuming its length.
    EXPECT_EQ(outputPos, source.find('\n', versionPos) + 1);
    EXPECT_NE(source.find("// #version 460 core"), String::npos);
    // This #line sits ahead of the version directive, where GLSL would never have honoured it, so
    // it is still dropped. Directives that follow the version line are kept - see
    // PreprocessKeepsPlainLineDirectivesAndSparesLookalikeIdentifiers.
    EXPECT_EQ(source.find("#line"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// A banner line like "//*** NOTE ***" contains "/*" at offset 1 and no "*/" anywhere after it. The
// old hand-rolled comment stripper searched for "/*" with no lexical state, found that, failed to
// find a terminator, and erased everything from there to the end of the file - deleting the entire
// shader. Banner comments in that exact shape are common in Iris and OptiFine packs.
TEST_F(ProgramUtilTest, PreprocessKeepsShaderBodyAfterAStarredLineComment) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 330 core
//*** lighting pass ***
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0);
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("void main()"), String::npos) << "shader body was truncated:\n" << source;
    EXPECT_NE(source.find("fragColor = vec4(1.0);"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// The builtin-shadowing rename only fires when the shader really defines its own round/tanh/etc.
// Deciding that from a commented-out definition renames every genuine call to the builtin to a
// mg_ name that nothing defines, which fails to link.
TEST_F(ProgramUtilTest, PreprocessIgnoresCommentedOutBuiltinShadowingDefinition) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 330 core
// float round(float x) { return floor(x + 0.5); }
out vec4 fragColor;
void main() {
    fragColor = vec4(round(1.25));
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("round(1.25)"), String::npos) << "call was renamed from a comment:\n" << source;
    EXPECT_EQ(source.find("mg_round"), String::npos);
}

// A block-commented extension directive must not be treated as a real one - the int64 filter turns
// unsupported directives into #error, so reading one out of a comment manufactures a compile
// failure for a shader that never asked for the extension.
TEST_F(ProgramUtilTest, PreprocessIgnoresBlockCommentedExtensionDirectives) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 330 core
/*
#extension GL_ARB_gpu_shader_int64 : require
*/
out vec4 fragColor;
void main() {
    fragColor = vec4(1.0);
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_EQ(source.find("#error"), String::npos) << "#error synthesized from a comment:\n" << source;

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// KHR-GL33.shaders.preprocessor.builtin.line_* checks that __LINE__ follows #line. That only works
// if the directive reaches glslang, so a plain integer form must pass through untouched - while
// "#linear" and friends must not be mistaken for it.
TEST_F(ProgramUtilTest, PreprocessKeepsPlainLineDirectivesAndSparesLookalikeIdentifiers) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 330 core
out vec4 fragColor;
#line 42
float linear(float x) { return x; }
void main() {
#line 100
    fragColor = vec4(linear(float(__LINE__)));
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("#line 42"), String::npos) << source;
    EXPECT_NE(source.find("#line 100"), String::npos) << source;
    EXPECT_NE(source.find("float linear(float x)"), String::npos) << "identifier lookalike was eaten:\n" << source;

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessModernSampleQualifierStaysAtItsDeclaredVersion) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 400 core
sample in vec4 interpolatedColor;
out vec4 fragColor;

void main() {
    fragColor = interpolatedColor;
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_EQ(source.find("#version 400 core\n"), 0);
    EXPECT_NE(source.find("sample in vec4 interpolatedColor;"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessGpuShader5SampleQualifierUsesVersion460) {
    using namespace MG_Util::ShaderTranspiler;

    for (const char* extension : {"GL_ARB_gpu_shader5", "GL_NV_gpu_shader5"}) {
        SCOPED_TRACE(extension);
        String source = "#version 150\n#extension ";
        source += extension;
        source += R"( : enable
sample in vec4 interpolatedColor;
out vec4 fragColor;

void main() {
    fragColor = interpolatedColor;
}
)";
        PreprocessShaderSource(ShaderStage::Fragment, source);

        EXPECT_EQ(source.find("#version 460 core\n"), 0);
        EXPECT_NE(source.find("sample in vec4 interpolatedColor;"), String::npos);

        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
        }
    }
}

TEST_F(ProgramUtilTest, PreprocessLegacyFragmentShaderModernizesFragData) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 130
void main() {
    gl_FragData[0] = vec4(1.0);
    gl_FragData[1].a = 0.5;
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_EQ(source.find("#version 330 core "), 0);
    EXPECT_NE(source.find("layout(location = 0) out vec4 mg_FragData[8];\n"), String::npos);
    EXPECT_NE(source.find("mg_FragData[0] = vec4(1.0);"), String::npos);
    EXPECT_NE(source.find("mg_FragData[1].a = 0.5;"), String::npos);
    EXPECT_EQ(source.find("gl_FragData"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessKeepsDefaultPrecisionStatements) {
    using namespace MG_Util::ShaderTranspiler;

    // Mirrors the GL CTS helper shaders (e.g. glcPixelStorageModesTests): the old qualifier strip
    // turned "precision highp float;" into invalid "precision  float;". Precision qualifiers are
    // legal (and ignored) in the normalized desktop core profile, so they now pass through untouched.
    String source = R"(#version 330
precision highp float;
precision mediump int;
out vec4 fragColor;
uniform highp sampler2D tex;

void main() {
    highp vec2 uv = vec2(0.5);
    fragColor = texture(tex, uv);
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("precision highp float;"), String::npos);
    EXPECT_NE(source.find("precision mediump int;"), String::npos);
    EXPECT_NE(source.find("uniform highp sampler2D tex;"), String::npos);
    EXPECT_NE(source.find("fragColor = texture(tex, uv);"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessKeepsPrecisionInLegacyShaderForGlslang) {
    using namespace MG_Util::ShaderTranspiler;

    // Legacy ES-style shader: precision statements and qualifier macros are left for glslang
    // (its preprocessor expands the #define; the normalized 330 core parse ignores the qualifiers).
    String source = R"(#define HIGHP_OR_DEFAULT highp
precision HIGHP_OR_DEFAULT float;
precision mediump int;
varying vec2 uv;

void main() {
    mediump float shade = uv.x;
    gl_FragColor = vec4(uv, shade, 1.0);
})";

    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("precision HIGHP_OR_DEFAULT float;"), String::npos);
    EXPECT_NE(source.find("precision mediump int;"), String::npos);
    EXPECT_NE(source.find("in vec2 uv;"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
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

// Legacy desktop sources are normalized to "#version 330 core", which is stricter than the 460 they
// used to be forced to. A legacy shader using 420-era syntax without the matching #extension line
// is accepted by real drivers, so CompileShader retries the normalized source at 460 rather than
// failing. Only MobileGL's own normalization is rescued this way - an application-declared
// "#version 330" keeps strict 3.30 semantics, which is what the CTS negative-compile cases need.
TEST_F(ProgramUtilTest, CompileShaderRetriesAt460WhenNormalizedLegacyVersionRejects420Syntax) {
    using namespace MG_Util::ShaderTranspiler;
    String source = R"(#version 130
layout(binding = 0) uniform sampler2D InSampler;
varying vec2 texCoord;
void main() {
    gl_FragColor = texture2D(InSampler, texCoord);
})";
    PreprocessShaderSource(ShaderStage::Fragment, source);
    // The normal path still emits 330 - the retry must not become the default.
    ASSERT_EQ(source.find("#version 330 core"), 0u);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log;
    }

    // Same source compiled for the OpenGL environment must take the retry too.
    ShaderAttrib glAttrib{
        .shaderType = GL_FRAGMENT_SHADER, .sourceStr = source, .flags = ShaderCompileBits::CompileForOpenGL};
    auto glRes = ShaderCompiler::CompileShader(glAttrib);
    if (!glRes) {
        FAIL() << "errc: " << glRes.error().errc << "\nlog: " << glRes.error().log;
    }
}

TEST_F(ProgramUtilTest, CompileShaderStillFailsWithOriginalDiagnosticsWhenRetryCannotHelp) {
    using namespace MG_Util::ShaderTranspiler;
    String source = R"(#version 330
in vec2 texCoord;
out vec4 fragColor;
void main() {
    fragColor = thisFunctionDoesNotExist(texCoord);
})";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error().errc, -2);
    EXPECT_NE(res.error().log.find("thisFunctionDoesNotExist"), String::npos) << res.error().log;
}

TEST_F(ProgramUtilTest, RetargetLegacyVersionDirectiveOnlyTouchesNormalizedDesktopCore) {
    using namespace MG_Util::ShaderTranspiler;

    // Only MobileGL's own normalization is retargetable, and it is recognised by the marker the
    // preprocessor leaves on the directive line - so normalize a legacy source rather than
    // hand-writing the directive the marker belongs to.
    String normalized = "#version 130\nvoid main() {}\n";
    PreprocessShaderSource(ShaderStage::Vertex, normalized);
    ASSERT_EQ(normalized.find("#version 330 core "), 0u);
    EXPECT_TRUE(RetargetLegacyVersionDirectiveTo460(normalized));
    EXPECT_EQ(normalized.find("#version 460 core"), 0u);

    // An application that declared 330 itself keeps strict 3.30 semantics: raising it would
    // re-legalize the CTS negative-compile cases.
    String declared330 = "#version 330 core\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(declared330));
    EXPECT_EQ(declared330.find("#version 330 core"), 0u);

    // Already modern: nothing to retarget.
    String modern = "#version 460 core\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(modern));
    EXPECT_EQ(modern.find("#version 460 core"), 0u);

    // ES and compatibility sources keep what they declared.
    String es = "#version 300 es\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(es));
    EXPECT_EQ(es.find("#version 300 es"), 0u);

    String compat = "#version 330 compatibility\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(compat));
    EXPECT_EQ(compat.find("#version 330 compatibility"), 0u);

    // A commented-out directive is not the real one.
    String commented = "// #version 330 core\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(commented));
    EXPECT_EQ(commented.find("#version 460"), String::npos);

    // A malformed directive must NOT be rescued to 460 - that is what silently legalized the CTS
    // directive.version_* rejection cases. The bad version stays put so glslang keeps rejecting it.
    String badNumber = "#version 331\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(badNumber));
    EXPECT_EQ(badNumber.find("#version 460"), String::npos);

    String badProfile = "#version 330 foo\nvoid main() {}\n";
    EXPECT_FALSE(RetargetLegacyVersionDirectiveTo460(badProfile));
    EXPECT_EQ(badProfile.find("#version 460"), String::npos);
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

// noperspective is core desktop GLSL (1.30+) and maps to the SPIR-V NoPerspective decoration. It must
// reach glslang (not be stripped as text) so the SPIR-V carries the decoration; SPIRV-Cross then emits
// ESSL `noperspective` + the GL_NV_shader_noperspective_interpolation extension. Shader packs
// (Iris/Complementary) depend on it, and KHR-GL33.glsl_noperspective fails if the result matches
// smooth. This is the DirectGLES path with the NV extension available (SPIRV-Cross's default).
TEST_F(ProgramUtilTest, NoperspectiveInterpolationSurvivesToEssl) {
    using namespace MG_Util::ShaderTranspiler;

    String fs = R"(#version 330 core
noperspective in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";
    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) FAIL() << "compile errc: " << res.error().errc << "\nlog: " << res.error().log;

    ProgramAttrib programAttrib{.shaders = {res.value()}};
    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) FAIL() << "link errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;

    ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER}, .program = *program_res.value()};
    auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    if (!bin_res) FAIL() << "spirv errc: " << bin_res.error().errc << "\nlog: " << bin_res.error().log;
    ASSERT_EQ(bin_res.value().size(), 1u);

    SpvcSession session(bin_res.value()[0], SessionUsageBit::Transpile);
    auto essl = ShaderCompiler::DecompileShader(session);
    if (!essl) FAIL() << "decompile errc: " << essl.error().errc << "\nlog: " << essl.error().log;

    EXPECT_NE(essl.value().find("noperspective"), String::npos)
        << "noperspective was lost before it reached SPIR-V:\n" << essl.value();
    EXPECT_NE(essl.value().find("GL_NV_shader_noperspective_interpolation"), String::npos)
        << "SPIRV-Cross must require the NV extension for ES noperspective:\n" << essl.value();
}

// The old handling was a naked substring erase of "noperspective", so any identifier that merely
// contained those characters (a uniform named noperspectiveBlend, say) got mangled. Removing the
// strip fixes it - glslang, which is identifier-aware, is the only thing that should see the keyword.
TEST_F(ProgramUtilTest, PreprocessDoesNotCorruptIdentifiersContainingNoperspective) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 330 core
uniform float noperspectiveBlend;
out vec4 fragColor;
void main() { fragColor = vec4(noperspectiveBlend); }
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);
    EXPECT_NE(source.find("noperspectiveBlend"), String::npos)
        << "identifier was corrupted by substring stripping:\n" << source;
}

// The DirectGLES fallback for devices without GL_NV_shader_noperspective_interpolation: stripping the
// NoPerspective decoration makes SPIRV-Cross emit a plain smooth varying with no `#extension … :
// require`, so the shader still compiles (rendering as smooth) instead of being rejected by the driver.
TEST_F(ProgramUtilTest, StripNoPerspectiveFallbackProducesPlainEssl) {
    using namespace MG_Util::ShaderTranspiler;

    String fs = R"(#version 330 core
noperspective in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";
    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) FAIL() << "compile errc: " << res.error().errc << "\nlog: " << res.error().log;

    ProgramAttrib programAttrib{.shaders = {res.value()}};
    auto program_res = ShaderCompiler::LinkProgram(programAttrib);
    if (!program_res) FAIL() << "link errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;

    ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER}, .program = *program_res.value()};
    auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    if (!bin_res) FAIL() << "spirv errc: " << bin_res.error().errc << "\nlog: " << bin_res.error().log;
    ASSERT_EQ(bin_res.value().size(), 1u);

    // Precondition: with the decoration present the default decompile requires the NV extension.
    {
        SpvcSession session(bin_res.value()[0], SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        if (!essl) FAIL() << "decompile errc: " << essl.error().errc;
        ASSERT_NE(essl.value().find("noperspective"), String::npos) << essl.value();
    }

    // The fallback strips the decoration -> plain smooth ESSL, no extension require.
    Vector<Uint32> stripped;
    ASSERT_TRUE(ShaderCompiler::StripNoPerspectiveForEssl(bin_res.value()[0], stripped));
    ASSERT_FALSE(stripped.empty());

    SpvcSession session(stripped, SessionUsageBit::Transpile);
    auto essl = ShaderCompiler::DecompileShader(session);
    if (!essl) FAIL() << "decompile errc: " << essl.error().errc << "\nlog: " << essl.error().log;
    EXPECT_EQ(essl.value().find("noperspective"), String::npos)
        << "the decoration should be gone:\n" << essl.value();
    EXPECT_EQ(essl.value().find("GL_NV_shader_noperspective_interpolation"), String::npos)
        << "no extension require without the decoration:\n" << essl.value();
}

// Directly exercises BOTH decoration forms StripNoPerspectivePass handles: a plain-variable
// OpDecorate NoPerspective (in-operand 1) and an interface-block-member OpMemberDecorate NoPerspective
// (in-operand 2). The ESSL round-trip tests above use only a scalar input, so they never reach the
// member-decorate branch, which a block varying like `in Block { noperspective vec4 c; }` (common in
// shader packs) produces. Unrelated decorations (Flat, Location) must survive untouched.
TEST_F(ProgramUtilTest, StripNoPerspectivePassRemovesBothDecorateForms) {
    using namespace MG_Util::ShaderTranspiler;

    const String spirvText = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %plainVar %blockVar %flatVar
               OpExecutionMode %main OriginUpperLeft
               OpName %main "main"
               OpDecorate %plainVar Location 0
               OpDecorate %plainVar NoPerspective
               OpMemberDecorate %Block 0 NoPerspective
               OpDecorate %blockVar Location 1
               OpDecorate %flatVar Location 2
               OpDecorate %flatVar Flat
       %void = OpTypeVoid
     %mainFn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
        %int = OpTypeInt 32 1
    %inV4Ptr = OpTypePointer Input %v4float
   %plainVar = OpVariable %inV4Ptr Input
      %Block = OpTypeStruct %v4float
 %inBlockPtr = OpTypePointer Input %Block
   %blockVar = OpVariable %inBlockPtr Input
   %inIntPtr = OpTypePointer Input %int
    %flatVar = OpVariable %inIntPtr Input
       %main = OpFunction %void None %mainFn
   %mainBody = OpLabel
               OpReturn
               OpFunctionEnd
)";

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    Vector<uint32_t> inputBinary;
    ASSERT_TRUE(tools.Assemble(spirvText, &inputBinary));

    const auto countNoPerspective = [](const String& text) {
        SizeT count = 0, offset = 0;
        while ((offset = text.find("NoPerspective", offset)) != String::npos) {
            ++count;
            offset += std::strlen("NoPerspective");
        }
        return count;
    };

    String inputText;
    ASSERT_TRUE(tools.Disassemble(inputBinary, &inputText));
    ASSERT_EQ(countNoPerspective(inputText), 2u)
        << "fixture must carry both a plain and a member NoPerspective:\n" << inputText;

    Vector<uint32_t> outputBinary;
    ASSERT_TRUE(ShaderCompiler::StripNoPerspectiveForEssl(inputBinary, outputBinary));
    ASSERT_FALSE(outputBinary.empty());

    String outputText;
    ASSERT_TRUE(tools.Disassemble(outputBinary, &outputText));
    EXPECT_EQ(countNoPerspective(outputText), 0u)
        << "both NoPerspective decorations (OpDecorate and OpMemberDecorate) must be stripped:\n" << outputText;
    EXPECT_NE(outputText.find("Flat"), String::npos)
        << "the unrelated Flat decoration must survive:\n" << outputText;
    EXPECT_NE(outputText.find("Location"), String::npos)
        << "Location decorations must survive:\n" << outputText;
}

// Phase 2 emulation - fragment side. On a device without the NV extension the NoPerspective input is
// recovered as `load * gl_FragCoord.w` and the decoration removed; gl_FragCoord is synthesized because
// the shader did not otherwise use it. The emulated SPIR-V must validate and decompile without the
// extension require.
TEST_F(ProgramUtilTest, EmulateNoperspectiveFragmentRecoversWithFragCoordW) {
    using namespace MG_Util::ShaderTranspiler;

    String fs = R"(#version 330 core
noperspective in vec4 vColor;
out vec4 f;
void main() { f = vColor; }
)";
    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) FAIL() << "compile: " << res.error().log;
    ProgramAttrib pa{.shaders = {res.value()}};
    auto pr = ShaderCompiler::LinkProgram(pa);
    if (!pr) FAIL() << "link: " << pr.error().log;
    ProgramBinaryAttrib ba{.shaderTypes = {GL_FRAGMENT_SHADER}, .program = *pr.value()};
    auto br = ShaderCompiler::GetSpirvBinaryFromProgram(ba);
    if (!br) FAIL() << "spirv: " << br.error().log;
    ASSERT_EQ(br.value().size(), 1u);

    Vector<uint32_t> emulated;
    ASSERT_TRUE(ShaderCompiler::EmulateNoPerspectiveForEssl(br.value()[0], emulated));
    ASSERT_FALSE(emulated.empty());

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String dis;
    ASSERT_TRUE(tools.Disassemble(emulated, &dis));
    ASSERT_TRUE(tools.Validate(emulated)) << "emulated SPIR-V must be valid:\n" << dis;
    EXPECT_EQ(dis.find("NoPerspective"), String::npos) << "decoration must be stripped:\n" << dis;
    EXPECT_NE(dis.find("FragCoord"), String::npos) << "gl_FragCoord must be synthesized:\n" << dis;
    EXPECT_NE(dis.find("OpVectorTimesScalar"), String::npos) << "the recovery multiply must be present:\n" << dis;

    SpvcSession session(emulated, SessionUsageBit::Transpile);
    auto essl = ShaderCompiler::DecompileShader(session);
    if (!essl) FAIL() << "decompile: " << essl.error().log;
    EXPECT_EQ(essl.value().find("noperspective"), String::npos) << essl.value();
    EXPECT_EQ(essl.value().find("GL_NV_shader_noperspective_interpolation"), String::npos) << essl.value();
    EXPECT_NE(essl.value().find("gl_FragCoord"), String::npos) << "recovery must reference gl_FragCoord:\n" << essl.value();
}

// Phase 2 emulation - vertex side. The NoPerspective output is pre-multiplied by gl_Position.w before
// return and the decoration removed. Emulated SPIR-V must validate and decompile without the extension.
TEST_F(ProgramUtilTest, EmulateNoperspectiveVertexPreMultipliesByPositionW) {
    using namespace MG_Util::ShaderTranspiler;

    String vs = R"(#version 330 core
in vec4 pos;
noperspective out vec4 vColor;
void main() { gl_Position = pos; vColor = pos; }
)";
    ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vs};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) FAIL() << "compile: " << res.error().log;
    ProgramAttrib pa{.shaders = {res.value()}};
    auto pr = ShaderCompiler::LinkProgram(pa);
    if (!pr) FAIL() << "link: " << pr.error().log;
    ProgramBinaryAttrib ba{.shaderTypes = {GL_VERTEX_SHADER}, .program = *pr.value()};
    auto br = ShaderCompiler::GetSpirvBinaryFromProgram(ba);
    if (!br) FAIL() << "spirv: " << br.error().log;
    ASSERT_EQ(br.value().size(), 1u);

    Vector<uint32_t> emulated;
    ASSERT_TRUE(ShaderCompiler::EmulateNoPerspectiveForEssl(br.value()[0], emulated));
    ASSERT_FALSE(emulated.empty());

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String dis;
    ASSERT_TRUE(tools.Disassemble(emulated, &dis));
    ASSERT_TRUE(tools.Validate(emulated)) << "emulated SPIR-V must be valid:\n" << dis;
    EXPECT_EQ(dis.find("NoPerspective"), String::npos) << "decoration must be stripped:\n" << dis;
    EXPECT_NE(dis.find("OpVectorTimesScalar"), String::npos) << "the pre-multiply must be present:\n" << dis;

    SpvcSession session(emulated, SessionUsageBit::Transpile);
    auto essl = ShaderCompiler::DecompileShader(session);
    if (!essl) FAIL() << "decompile: " << essl.error().log;
    EXPECT_EQ(essl.value().find("noperspective"), String::npos) << essl.value();
    EXPECT_NE(essl.value().find("gl_Position"), String::npos) << "pre-multiply must reference gl_Position:\n" << essl.value();
}

namespace {
// Compiles one shader stage through the full pipeline and returns its SPIR-V, or fails the test.
MobileGL::Vector<uint32_t> CompileStageSpirv(GLenum type, const char* src) {
    using namespace MG_Util::ShaderTranspiler;
    ShaderAttrib attrib{.shaderType = type, .sourceStr = src};
    auto res = ShaderCompiler::CompileShader(attrib);
    EXPECT_TRUE(static_cast<bool>(res)) << (res ? "" : res.error().log);
    if (!res) return {};
    ProgramAttrib pa{.shaders = {res.value()}};
    auto pr = ShaderCompiler::LinkProgram(pa);
    EXPECT_TRUE(static_cast<bool>(pr)) << (pr ? "" : pr.error().log);
    if (!pr) return {};
    ProgramBinaryAttrib ba{.shaderTypes = {type}, .program = *pr.value()};
    auto br = ShaderCompiler::GetSpirvBinaryFromProgram(ba);
    EXPECT_TRUE(static_cast<bool>(br)) << (br ? "" : br.error().log);
    if (!br || br.value().empty()) return {};
    return br.value()[0];
}
} // namespace

// Regression: the vertex pre-multiply must be applied exactly once (in main), not once per function.
// glslang does not inline, so a helper function survives as its own OpFunction; instrumenting its
// return too would scale the varying by gl_Position.w twice (w^2).
TEST_F(ProgramUtilTest, EmulateNoperspectiveVertexWithHelperScalesExactlyOnce) {
    using namespace MG_Util::ShaderTranspiler;
    // helper() returns via OpReturnValue and adds (no vector*scalar), so the ONLY OpVectorTimesScalar
    // in the module is the emulation's pre-multiply. The old all-functions code injected it at both
    // helper's and main's return -> count 2; restricted to the entry function it is 1.
    auto spirv = CompileStageSpirv(GL_VERTEX_SHADER, R"(#version 330 core
in vec4 pos;
noperspective out vec4 vColor;
vec4 helper(vec4 x) { return x + vec4(1.0); }
void main() { gl_Position = pos; vColor = helper(pos); }
)");
    ASSERT_FALSE(spirv.empty());

    Vector<uint32_t> emulated;
    ASSERT_TRUE(ShaderCompiler::EmulateNoPerspectiveForEssl(spirv, emulated));
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String dis;
    ASSERT_TRUE(tools.Disassemble(emulated, &dis));
    ASSERT_TRUE(tools.Validate(emulated)) << dis;

    SizeT count = 0, off = 0;
    while ((off = dis.find("OpVectorTimesScalar", off)) != String::npos) {
        ++count;
        off += std::strlen("OpVectorTimesScalar");
    }
    EXPECT_EQ(count, 1u) << "the gl_Position.w pre-multiply must happen exactly once, not per function:\n" << dis;
}

// Regression: a single-component read (vColor.x), which glslang lowers via OpAccessChain, must still be
// recovered with gl_FragCoord.w - not silently left un-scaled.
TEST_F(ProgramUtilTest, EmulateNoperspectiveFragmentComponentReadIsRecovered) {
    using namespace MG_Util::ShaderTranspiler;
    auto spirv = CompileStageSpirv(GL_FRAGMENT_SHADER, R"(#version 330 core
noperspective in vec4 vColor;
out vec4 f;
void main() { f = vec4(vColor.x); }
)");
    ASSERT_FALSE(spirv.empty());

    Vector<uint32_t> emulated;
    ASSERT_TRUE(ShaderCompiler::EmulateNoPerspectiveForEssl(spirv, emulated));
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String dis;
    ASSERT_TRUE(tools.Disassemble(emulated, &dis));
    ASSERT_TRUE(tools.Validate(emulated)) << dis;
    EXPECT_EQ(dis.find("NoPerspective"), String::npos) << dis;
    EXPECT_NE(dis.find("FragCoord"), String::npos)
        << "the component read must still be recovered via gl_FragCoord.w:\n" << dis;
}

// Coverage: a scalar float varying exercises the OpFMul path; a vector varying the OpVectorTimesScalar
// path; multiple noperspective varyings in one stage are all handled.
TEST_F(ProgramUtilTest, EmulateNoperspectiveHandlesScalarAndMultipleVaryings) {
    using namespace MG_Util::ShaderTranspiler;
    auto spirv = CompileStageSpirv(GL_FRAGMENT_SHADER, R"(#version 330 core
noperspective in float a;
noperspective in vec2 b;
out vec4 f;
void main() { f = vec4(a, b, 1.0); }
)");
    ASSERT_FALSE(spirv.empty());

    Vector<uint32_t> emulated;
    ASSERT_TRUE(ShaderCompiler::EmulateNoPerspectiveForEssl(spirv, emulated));
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String dis;
    ASSERT_TRUE(tools.Disassemble(emulated, &dis));
    ASSERT_TRUE(tools.Validate(emulated)) << dis;
    EXPECT_EQ(dis.find("NoPerspective"), String::npos) << dis;
    EXPECT_NE(dis.find("OpFMul"), String::npos) << "the scalar varying must scale with OpFMul:\n" << dis;
    EXPECT_NE(dis.find("OpVectorTimesScalar"), String::npos)
        << "the vector varying must scale with OpVectorTimesScalar:\n" << dis;
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

const char* photon_shared_vec3_cs = R"(#version 460 core
layout(local_size_x = 16, local_size_y = 16) in;

shared vec3 shared_memory[256][9];

layout(location = 0) uniform int u_row;
layout(location = 1) uniform int u_col;
layout(location = 2) uniform vec3 u_value;

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    vec4 out_data[];
};

vec3 evaluate_row(vec3 row_values[9], uint col) {
    return row_values[col] + row_values[0];
}

void main() {
    uint row = gl_LocalInvocationIndex;
    uint col = u_col;

    shared_memory[row][col] = u_value;
    shared_memory[row][col] += vec3(1.0);

    vec3 loaded = shared_memory[row][col];
    float x = shared_memory[row][col].x;

    vec3 rowCopy[9] = shared_memory[0];

    memoryBarrierShared();
    barrier();

    out_data[gl_GlobalInvocationID.x] = vec4(loaded + rowCopy[col] + evaluate_row(shared_memory[0], col) + vec3(x), 1.0);
}
)";

TEST_F(ProgramUtilTest, DecomposeWorkgroupVec3InSpirvPass) {
    using namespace MG_Util::ShaderTranspiler;

    String csSource = photon_shared_vec3_cs;
    PreprocessShaderSource(ShaderStage::Compute, csSource);

    ShaderAttrib csAttrib{.shaderType = GL_COMPUTE_SHADER, .sourceStr = csSource};
    auto csRes = ShaderCompiler::CompileShader(csAttrib);
    if (!csRes) {
        ASSERT_NE(csRes.error().errc, 0);
        FAIL() << "errc: " << csRes.error().errc << "\nlog: " << csRes.error().log;
    }

    ProgramAttrib programAttrib{.shaders = {csRes.value()}};
    auto programRes = ShaderCompiler::LinkProgram(programAttrib);
    if (!programRes) {
        ASSERT_NE(programRes.error().errc, 0);
        FAIL() << "errc: " << programRes.error().errc << "\nlog: " << programRes.error().log;
    }

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_COMPUTE_SHADER},
        .program = *programRes.value(),
    };
    auto binRes = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binRes.has_value());
    ASSERT_FALSE(binRes->empty());

    Vector<uint32_t> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(binRes->at(0), optimized))
        << "SanitizeAndOptimizeBinary failed - the DecomposeWorkgroupVec3Pass may have "
           "encountered an unsupported pattern";

    spvtools::Optimizer parseOnlyOptimizer(SPV_ENV_VULKAN_1_1);
    Vector<uint32_t> parsedBinary;
    ASSERT_TRUE(parseOnlyOptimizer.Run(optimized.data(), optimized.size(), &parsedBinary))
        << "DecomposeWorkgroupVec3Pass emitted SPIR-V with invalid physical layout";

    SpvcSession session(optimized, SessionUsageBit::Transpile);
    auto sourceRes = ShaderCompiler::DecompileShader(session);
    ASSERT_TRUE(sourceRes.has_value()) << "errc: " << sourceRes.error().errc
                                       << "\nlog: " << sourceRes.error().log;

    const String& source = sourceRes.value();
    // The decomposed output must not contain a `shared vec3` declaration.
    EXPECT_EQ(source.find("shared vec3"), std::string::npos)
        << "DecomposeWorkgroupVec3Pass did not eliminate `shared vec3`:\n"
        << source;

    // It should now use a scalar array form (shared float ...).
    EXPECT_NE(source.find("shared float"), std::string::npos)
        << "Expected `shared float` in decomposed output:\n"
        << source;

    EXPECT_EQ(source.find("= shared_memory[0]"), std::string::npos)
        << "Decomposed output kept an invalid whole-row shared-memory load:\n"
        << source;
}

TEST_F(ProgramUtilTest, DecomposeWorkgroupVec3IgnoresNonWorkgroupVec3) {
    using namespace MG_Util::ShaderTranspiler;

    String csSource = R"(#version 460 core
layout(local_size_x = 1) in;

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    vec4 out_data[];
};

void main() {
    vec3 local = vec3(1.0, 2.0, 3.0);
    out_data[gl_GlobalInvocationID.x] = vec4(local, 1.0);
}
)";
    PreprocessShaderSource(ShaderStage::Compute, csSource);

    ShaderAttrib csAttrib{.shaderType = GL_COMPUTE_SHADER, .sourceStr = csSource};
    auto csRes = ShaderCompiler::CompileShader(csAttrib);
    if (!csRes) {
        ASSERT_NE(csRes.error().errc, 0);
        FAIL() << "errc: " << csRes.error().errc << "\nlog: " << csRes.error().log;
    }

    ProgramAttrib programAttrib{.shaders = {csRes.value()}};
    auto programRes = ShaderCompiler::LinkProgram(programAttrib);
    if (!programRes) {
        ASSERT_NE(programRes.error().errc, 0);
        FAIL() << "errc: " << programRes.error().errc << "\nlog: " << programRes.error().log;
    }

    ProgramBinaryAttrib binaryAttrib{
        .shaderTypes = {GL_COMPUTE_SHADER},
        .program = *programRes.value(),
    };
    auto binRes = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binRes.has_value());
    ASSERT_FALSE(binRes->empty());

    Vector<uint32_t> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(binRes->at(0), optimized));
}


TEST_F(ProgramUtilTest, PreprocessCoercesBlockPackingQualifiersToStd140) {
    using namespace MG_Util::ShaderTranspiler;

    // glslang rejects `packed`/`shared` outright when generating SPIR-V, and MobileGL's
    // UBO layout is always std140 anyway; the preprocessor rewrites the qualifiers so the
    // validation compile, reflection, and generated SPIR-V all agree on std140 (GL CTS
    // KHR-GL33.shaders.uniform_block.*.packed/shared).
    String source = R"(#version 330
layout(packed) uniform PackedBlock { vec4 pv; };
layout(shared, row_major) uniform SharedBlock { mat4 sm; };
layout ( shared ) uniform SpacedBlock { float sx; };
layout(std140) uniform KeptBlock { float kx; };
// A non-layout use of the identifier stays untouched (compute storage qualifier).
void main() {
    gl_Position = pv + vec4(sm[0][0]) + vec4(sx) + vec4(kx);
})";

    PreprocessShaderSource(ShaderStage::Vertex, source);

    EXPECT_EQ(source.find("packed"), String::npos);
    EXPECT_EQ(source.find("layout(shared"), String::npos);
    EXPECT_NE(source.find("layout(std140) uniform PackedBlock"), String::npos);
    EXPECT_NE(source.find("layout(std140, row_major) uniform SharedBlock"), String::npos);
    EXPECT_NE(source.find("layout ( std140 ) uniform SpacedBlock"), String::npos);
    EXPECT_NE(source.find("layout(std140) uniform KeptBlock"), String::npos);

    ShaderAttrib attrib{.shaderType = GL_VERTEX_SHADER,
                        .sourceStr = source,
                        .flags = ShaderCompileBits::CompileForOpenGL};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

TEST_F(ProgramUtilTest, PreprocessLeavesComputeSharedStorageQualifierAlone) {
    using namespace MG_Util::ShaderTranspiler;

    // `shared` is only a packing qualifier inside layout(...); the compute-shader storage
    // qualifier of the same spelling must survive.
    String source = R"(#version 430
layout(local_size_x = 8) in;
shared float sharedScratch[8];
layout(shared) uniform Blk { float bx; };
void main() {
    sharedScratch[gl_LocalInvocationIndex] = bx;
})";

    PreprocessShaderSource(ShaderStage::Compute, source);

    EXPECT_NE(source.find("shared float sharedScratch[8];"), String::npos);
    EXPECT_NE(source.find("layout(std140) uniform Blk"), String::npos);
}

namespace {
    String MakeLinearSubgroupPrefixScanShader() {
        return R"(#version 460 core
#extension GL_KHR_shader_subgroup_arithmetic : enable
layout(local_size_x = 1024) in;
shared float prefixSumCache[64];

layout(std430, binding = 0) writeonly buffer OutputBuffer {
    float outputValues[];
};

void main() {
    float importance = 1.0f;
    float prefixSum = subgroupInclusiveAdd(importance);
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u) prefixSumCache[gl_SubgroupID] = prefixSum;
    barrier();
    uint loopLength = uint(findMSB(gl_NumSubgroups));
    loopLength += uint(gl_NumSubgroups - (1u << (loopLength - 1u)) > 0u);
    for (uint i = 0; i < loopLength; i++) {
        if ((gl_SubgroupID & (1u << i)) > 0u) {
            prefixSum += prefixSumCache[(gl_SubgroupID >> i << i) - 1u];
            if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u) prefixSumCache[gl_SubgroupID] = prefixSum;
        }
        barrier();
    }
    if (gl_LocalInvocationID.x == uint(1024 - 1)) prefixSumCache[0] = prefixSum;
    barrier();
    float sum = prefixSumCache[0];
    float warp = (prefixSum - importance) / sum - float(gl_LocalInvocationID.x + 1u) / float(1024);
    outputValues[gl_GlobalInvocationID.x] = warp;
}
)";
    }
} // namespace

TEST_F(ProgramUtilTest, RewriteLinearSubgroupPrefixScanUsesSharedMemoryAndProducesValidSpirv) {
    using namespace MG_Util::ShaderTranspiler;

    String source = MakeLinearSubgroupPrefixScanShader();
    ASSERT_TRUE(RewriteLinearSubgroupPrefixScanForVulkan(ShaderStage::Compute, 64, source));

    EXPECT_NE(source.find("shared float prefixSumCache[1024]"), String::npos) << source;
    EXPECT_NE(source.find("mglVirtualSubgroupInvocation"), String::npos) << source;
    EXPECT_NE(source.find("for (uint mglPrefixLane"), String::npos) << source;
    EXPECT_EQ(source.find("subgroupInclusiveAdd"), String::npos) << source;
    EXPECT_EQ(source.find("gl_Subgroup"), String::npos) << source;

    const String onceRewritten = source;
    EXPECT_FALSE(RewriteLinearSubgroupPrefixScanForVulkan(ShaderStage::Compute, 64, source));
    EXPECT_EQ(source, onceRewritten);

    ShaderAttrib shaderAttrib{.shaderType = GL_COMPUTE_SHADER, .sourceStr = source};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log << "\nsource:\n" << source;

    ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
    auto programResult = ShaderCompiler::LinkProgram(programAttrib);
    ASSERT_TRUE(programResult) << programResult.error().log;

    ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_COMPUTE_SHADER}, .program = *programResult.value()};
    auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binaryResult) << binaryResult.error().log;
    ASSERT_EQ(binaryResult->size(), 1u);

    String validationDiagnostics;
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        validationDiagnostics += message;
        validationDiagnostics += '\n';
    });
    EXPECT_TRUE(tools.Validate(binaryResult->front())) << validationDiagnostics;

    String spirvText;
    ASSERT_TRUE(tools.Disassemble(binaryResult->front(), &spirvText));
    EXPECT_EQ(spirvText.find("OpGroupNonUniform"), String::npos) << spirvText;
}

TEST_F(ProgramUtilTest, RewriteLinearSubgroupPrefixScanRejectsOtherStagesAndSubgroupWidths) {
    using namespace MG_Util::ShaderTranspiler;

    const String original = MakeLinearSubgroupPrefixScanShader();
    for (const auto& [stage, subgroupSize] :
         {std::pair{ShaderStage::Compute, Uint32{32}}, std::pair{ShaderStage::Fragment, Uint32{64}},
          std::pair{ShaderStage::Compute, Uint32{96}}}) {
        String source = original;
        EXPECT_FALSE(RewriteLinearSubgroupPrefixScanForVulkan(stage, subgroupSize, source));
        EXPECT_EQ(source, original);
    }
}

TEST_F(ProgramUtilTest, RewriteLinearSubgroupPrefixScanRejectsPartialOrUnsafeTemplateMatches) {
    using namespace MG_Util::ShaderTranspiler;

    const auto expectUnchanged = [](String source) {
        const String original = source;
        EXPECT_FALSE(RewriteLinearSubgroupPrefixScanForVulkan(ShaderStage::Compute, 64, source));
        EXPECT_EQ(source, original);
    };

    String wrongLocalSize = MakeLinearSubgroupPrefixScanShader();
    wrongLocalSize.replace(wrongLocalSize.find("local_size_x = 1024"), std::strlen("local_size_x = 1024"),
                           "local_size_x = 512");
    expectUnchanged(std::move(wrongLocalSize));

    String cacheHasAnotherUse = MakeLinearSubgroupPrefixScanShader();
    cacheHasAnotherUse.insert(cacheHasAnotherUse.find("float importance"), "prefixSumCache[0] = 0.0f;\n    ");
    expectUnchanged(std::move(cacheHasAnotherUse));

    String extraSubgroupBuiltin = MakeLinearSubgroupPrefixScanShader();
    extraSubgroupBuiltin.insert(extraSubgroupBuiltin.find("float importance"),
                                "uvec4 extraMask = gl_SubgroupEqMask;\n    ");
    expectUnchanged(std::move(extraSubgroupBuiltin));

    String alteredBarrier = MakeLinearSubgroupPrefixScanShader();
    alteredBarrier.replace(alteredBarrier.find("barrier();"), std::strlen("barrier();"), "memoryBarrierShared();");
    expectUnchanged(std::move(alteredBarrier));

    String nestedScan = MakeLinearSubgroupPrefixScanShader();
    nestedScan.insert(nestedScan.find("float prefixSum ="), "if (importance > 0.0f) {\n    ");
    const SizeT consumerEnd = nestedScan.find(';', nestedScan.find("float warp ="));
    ASSERT_NE(consumerEnd, String::npos);
    nestedScan.insert(consumerEnd + 1, "\n    }");
    expectUnchanged(std::move(nestedScan));

    // ARB/NV spellings of lane-width-sensitive builtins must block the rewrite exactly
    // like their KHR counterparts.
    String arbSubgroupBuiltin = MakeLinearSubgroupPrefixScanShader();
    arbSubgroupBuiltin.insert(arbSubgroupBuiltin.find("float importance"),
                              "uint arbLane = gl_SubGroupInvocationARB;\n    ");
    expectUnchanged(std::move(arbSubgroupBuiltin));

    String arbBallotCall = MakeLinearSubgroupPrefixScanShader();
    arbBallotCall.insert(arbBallotCall.find("float importance"),
                         "uint64_t arbMask = ballotARB(true);\n    ");
    expectUnchanged(std::move(arbBallotCall));

    String nvWarpBuiltin = MakeLinearSubgroupPrefixScanShader();
    nvWarpBuiltin.insert(nvWarpBuiltin.find("float importance"),
                         "uint warpSize = gl_WarpSizeNV;\n    ");
    expectUnchanged(std::move(nvWarpBuiltin));

    String nvShuffleCall = MakeLinearSubgroupPrefixScanShader();
    nvShuffleCall.insert(nvShuffleCall.find("float importance"),
                         "float other = shuffleNV(1.0f, 0u, 32u);\n    ");
    expectUnchanged(std::move(nvShuffleCall));
}
