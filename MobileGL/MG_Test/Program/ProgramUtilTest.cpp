// MobileGL - MobileGL/MG_Test/Program/ProgramUtilTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/LegalizeFragmentOutputIndexPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/RenameSamplerFunctionParameterPass.h>
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/glslang/UniformTraverser.h>
#include <MG_State/GLState/ProgramState/ShaderPreprocessCache.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
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

// The LEXICAL half must fire at the source level (before the parse) for the
// preempt-list names - the end-to-end ESSL tests cannot tell which half did the
// rename, and for these names the parse would fail without the source rewrite.
TEST_F(ProgramUtilTest, PreprocessRenamesLexicalPreemptShadowingInSource) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 460 core
out vec4 fragColor;

float min3(float a, float b, float c) { return min(min(a, b), c); }

void main() {
    fragColor = vec4(min3(0.1, 0.2, 0.3));
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("float mg_min3("), String::npos) << source;
    EXPECT_NE(source.find("mg_min3(0.1, 0.2, 0.3)"), String::npos) << source;
    EXPECT_EQ(source.find("float min3("), String::npos) << source;
}

// GLSL has no multi-line string or character literal, so a lone apostrophe never opens one - it is
// an English contraction, in a comment or in a diagnostic directive. MaskCommentsAndQuotedText used
// to disagree: it entered its quoted-text region on the apostrophe and, having no end-of-line rule,
// stayed there to the end of the file, blanking everything after it for every consumer of the mask
// (the tokenizer, the #version inspection, the explicit-location and opaque-binding extractors).
//
// Apostrophes inside comments were never affected - the comment region claims them first - but that
// is exactly the property the fix must not break, so pin it.
TEST_F(ProgramUtilTest, PreprocessKeepsApostrophesInsideCommentsHarmless) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 460 core
// don't do this: the sampler isn't bound before the first frame
/* and here's a block comment whose apostrophes shouldn't matter either */
uniform sampler2D tex;
in vec2 uv;
out vec4 fragColor;

void main() {
    fragColor = texture(tex, uv);
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    EXPECT_NE(source.find("void main()"), String::npos) << "shader body was blanked:\n" << source;
    EXPECT_NE(source.find("fragColor = texture(tex, uv);"), String::npos) << source;

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// The case the old masker actually broke: an apostrophe in real (non-comment) text. Everything after
// it looked like string interior, so ExtractExplicitUniformLocations tokenized a blank source and
// handed the GL location assigner an empty map - the uniform silently lost its explicit location.
TEST_F(ProgramUtilTest, PreprocessApostropheInDirectiveKeepsLaterCodeVisibleToExtractors) {
    using namespace MG_Util::ShaderTranspiler;

    String source = R"(#version 460 core
#pragma MG_NOTE(this pack can't run without explicit locations)
layout(location = 7) uniform vec4 tint;
in vec2 uv;
out vec4 fragColor;

void main() {
    fragColor = tint * uv.x;
}
)";
    PreprocessShaderSource(ShaderStage::Fragment, source);

    const UnorderedMap<String, Int> locations = ExtractExplicitUniformLocations(source);
    ASSERT_EQ(locations.count("tint"), 1u) << "extractor went blind past the apostrophe:\n" << source;
    EXPECT_EQ(locations.at("tint"), 7);

    ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
    auto res = ShaderCompiler::CompileShader(attrib);
    if (!res) {
        FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
    }
}

// PreprocessShaderSource used to rediscover "where does the #version directive end?" once per
// injection - up to five whole-source masks and line scans per compile for one offset. It now takes
// the anchor once, from the pass that creates it, and tracks it.
//
// These three sources drive every consumer of that anchor: NormalizeLineDirectives (both the
// keep branch and the drop-ahead-of-#version branch), ModernizeLegacyGLSL's gl_FragColor
// injection, and InjectDepthRangeBuiltinShim's. The expected texts are the byte-exact output of
// the pre-memo implementation, captured from it - the change is pure memoization and is allowed to
// move no byte at all.
//
// Case B and case C are the two ways the anchor moves out from under the memo, and are why it is
// tracked rather than simply cached: B deletes a #line that precedes the version directive, and C
// has ModernizeLegacyGLSL's raw ReplaceIdentifier rewrite "varying"/"texture2D" inside a comment
// banner ahead of it, pulling the anchor six bytes left. An offset cached blindly would put the
// injected declaration six bytes inside the version line.
TEST_F(ProgramUtilTest, PreprocessLegacyFragmentShaderOutputIsByteStableAcrossTheVersionAnchor) {
    using namespace MG_Util::ShaderTranspiler;

    const char* kLegacyBody = R"(#line 30
varying vec2 uv;
uniform sampler2D tex;

void main() {
    float d = gl_DepthRange.diff;
    gl_FragColor = texture2D(tex, uv) * d;
}
)";
    const char* kExpectedBody =
        "#version 330 core /*mobilegl-normalized-legacy*/\n"
        "struct mg_DepthRangeParameters { float near; float far; float diff; };\n"
        "const mg_DepthRangeParameters mg_DepthRange = mg_DepthRangeParameters(0.0, 1.0, 1.0);\n"
        "#define gl_DepthRange mg_DepthRange\n"
        "out vec4 mg_FragColor;\n"
        "#line 30\n"
        "in vec2 uv;\n"
        "uniform sampler2D tex;\n"
        "\n"
        "void main() {\n"
        "    float d = gl_DepthRange.diff;\n"
        "    mg_FragColor = texture(tex, uv) * d;\n"
        "}\n";

    {
        SCOPED_TRACE("A: version directive at offset 0");
        String source = String("#version 120\n") + kLegacyBody;
        PreprocessShaderSource(ShaderStage::Fragment, source);
        EXPECT_EQ(source, String(kExpectedBody));

        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
        }
    }

    {
        SCOPED_TRACE("B: a #line ahead of the version directive is dropped, shortening the prefix");
        String source = String("// pack preamble\n#line 1 \"world.fsh\"\n#version 120\n") + kLegacyBody;
        PreprocessShaderSource(ShaderStage::Fragment, source);
        // The dropped directive leaves its newline behind, so line numbering is untouched.
        EXPECT_EQ(source, String("// pack preamble\n\n") + kExpectedBody);
    }

    {
        SCOPED_TRACE("C: a comment banner ahead of the version directive is itself rewritten");
        String source = R"(/* legacy varying / texture2D helpers */
#version 120
varying vec2 uv;
uniform sampler2D tex;
void main() {
    gl_FragColor = texture2D(tex, uv);
}
)";
        PreprocessShaderSource(ShaderStage::Fragment, source);
        EXPECT_EQ(source, String("/* legacy in / texture helpers */\n"
                                 "#version 330 core /*mobilegl-normalized-legacy*/\n"
                                 "out vec4 mg_FragColor;\n"
                                 "in vec2 uv;\n"
                                 "uniform sampler2D tex;\n"
                                 "void main() {\n"
                                 "    mg_FragColor = texture(tex, uv);\n"
                                 "}\n"));

        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            FAIL() << "errc: " << res.error().errc << "\nlog: " << res.error().log << "\nsource:\n" << source;
        }
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// P1: CompileEnv - the compile pipeline's snapshot of everything outside
// (stage, source). These pin the two properties the rest of P1 rides on: the
// compute limits really are carried in the snapshot (an off-thread
// GL_MAX_COMPUTE_WORK_GROUP_SIZE query would silently return 0 and reject a
// legal local_size), and the fingerprint really does move when they do.
// ---------------------------------------------------------------------------
TEST_F(ProgramUtilTest, CompileEnvCarriesComputeLimitsAndFrontendMinima) {
    using MobileGL::MG_Util::ShaderTranspiler::CaptureCompileEnv;

    const auto env = CaptureCompileEnv();
    ASSERT_NE(env, nullptr);
    // With no backend the snapshot is the frontend minimum, never zero - the value an
    // off-thread GetIntegeri_v would have left behind.
    EXPECT_GE(env->maxComputeWorkGroupSize[0], 1024u);
    EXPECT_GE(env->maxComputeWorkGroupSize[1], 1024u);
    EXPECT_GE(env->maxComputeWorkGroupSize[2], 64u);
    EXPECT_GE(env->maxComputeWorkGroupInvocations, 1024u);
    EXPECT_NE(env->fingerprint, 0u);
}

TEST_F(ProgramUtilTest, CompileEnvFingerprintTracksEveryInput) {
    using MobileGL::MG_Util::ShaderTranspiler::CompileEnv;
    using MobileGL::MG_Util::ShaderTranspiler::ComputeCompileEnvFingerprint;

    CompileEnv base;
    const Uint64 baseline = ComputeCompileEnvFingerprint(base);
    EXPECT_EQ(ComputeCompileEnvFingerprint(base), baseline) << "fingerprint must be deterministic";

    // A device that allows a bigger workgroup than the frontend minimum is a DIFFERENT
    // compile environment: a memo taken under the smaller limit must not be reusable.
    CompileEnv biggerZ = base;
    biggerZ.maxComputeWorkGroupSize[2] = 256;
    EXPECT_NE(ComputeCompileEnvFingerprint(biggerZ), baseline);

    CompileEnv moreInvocations = base;
    moreInvocations.maxComputeWorkGroupInvocations = 2048;
    EXPECT_NE(ComputeCompileEnvFingerprint(moreInvocations), baseline);

    CompileEnv otherBackend = base;
    otherBackend.backend = MobileGL::BackendType::DirectVulkan;
    EXPECT_NE(ComputeCompileEnvFingerprint(otherBackend), baseline);

    CompileEnv otherLimits = base;
    otherLimits.params.MaxVertexAttribs = 31;
    EXPECT_NE(ComputeCompileEnvFingerprint(otherLimits), baseline);

    CompileEnv otherExtensions = base;
    otherExtensions.advertisedExtensions.push_back(MobileGL::E_GL_ARB_gpu_shader_int64);
    EXPECT_NE(ComputeCompileEnvFingerprint(otherExtensions), baseline);

    CompileEnv otherQuirk = base;
    otherQuirk.subgroupPrefixScanQuirk = MobileGL::MG_Config::QuirkOverride::ForceOn;
    EXPECT_NE(ComputeCompileEnvFingerprint(otherQuirk), baseline);
}

// The no-backend fallback must stay exactly what the pipeline used to do inline:
// everything counts as advertised, because there is nothing to gate against.
TEST_F(ProgramUtilTest, CompileEnvWithoutBackendAdvertisesEverything) {
    using MobileGL::MG_Util::ShaderTranspiler::CompileEnv;

    CompileEnv env;
    EXPECT_FALSE(env.HasBackend());
    EXPECT_TRUE(env.IsExtensionAdvertised(MobileGL::E_GL_ARB_gpu_shader_int64));

    env.backend = MobileGL::BackendType::DirectGLES;
    EXPECT_TRUE(env.HasBackend());
    EXPECT_FALSE(env.IsExtensionAdvertised(MobileGL::E_GL_ARB_gpu_shader_int64));
    env.advertisedExtensions.push_back(MobileGL::E_GL_ARB_gpu_shader_int64);
    EXPECT_TRUE(env.IsExtensionAdvertised(MobileGL::E_GL_ARB_gpu_shader_int64));
}

// P0b layer 2: ShaderPreprocessCache, tested directly. The GL-level behaviour it
// enables is covered end to end in ProgramTest; these pin the container itself,
// where the interesting cases (hash collisions, both eviction budgets) are hard
// to provoke through glCompileShader.
// ---------------------------------------------------------------------------
namespace {
    using MobileGL::MG_State::GLState::ShaderPreprocessCache;
    using MobileGL::MG_State::GLState::ShaderPreprocessOutcome;
    using MobileGL::MG_State::GLState::ShaderPreprocessResult;
    using MobileGL::MG_State::GLState::ShaderPreprocessResultPtr;

    // The env fingerprint every test below keys against, unless it is specifically
    // exercising the fingerprint itself.
    constexpr MobileGL::Uint64 kEnvA = 0x1111'2222'3333'4444ull;
    constexpr MobileGL::Uint64 kEnvB = 0x5555'6666'7777'8888ull;

    ShaderPreprocessResultPtr MakeResult(const String& preprocessed) {
        auto result = MakeShared<ShaderPreprocessResult>();
        result->outcome = ShaderPreprocessOutcome::Preprocessed;
        result->preprocessedSource = preprocessed;
        result->explicitUniformLocations["uMarker"] = 7;
        result->explicitOpaqueBindings["sMarker"] = 3;
        return result;
    }
} // namespace

TEST_F(ProgramUtilTest, ShaderPreprocessCacheRoundTripsAndSeparatesStages) {
    ShaderPreprocessCache cache;
    const String source = "// a shader\nvoid main() {}\n";
    const Uint64 hash = ShaderPreprocessCache::HashSource(source);

    EXPECT_EQ(cache.Find(ShaderStage::Vertex, hash, source, kEnvA), nullptr);

    cache.Insert(ShaderStage::Vertex, hash, source, kEnvA, MakeResult("vertex-preprocessed"));
    const ShaderPreprocessResultPtr hit = cache.Find(ShaderStage::Vertex, hash, source, kEnvA);
    ASSERT_NE(hit, nullptr);
    EXPECT_TRUE(hit->Preprocessed());
    EXPECT_EQ(hit->preprocessedSource, "vertex-preprocessed");
    const auto uniformIt = hit->explicitUniformLocations.find("uMarker");
    ASSERT_NE(uniformIt, hit->explicitUniformLocations.end());
    EXPECT_EQ(uniformIt->second, 7);
    const auto bindingIt = hit->explicitOpaqueBindings.find("sMarker");
    ASSERT_NE(bindingIt, hit->explicitOpaqueBindings.end());
    EXPECT_EQ(bindingIt->second, 3u);

    // Byte-identical source, different stage: a different key, so still a miss. Two
    // stages sharing one entry would hand a fragment shader a vertex preprocess.
    EXPECT_EQ(cache.Find(ShaderStage::Fragment, hash, source, kEnvA), nullptr);
    cache.Insert(ShaderStage::Fragment, hash, source, kEnvA, MakeResult("fragment-preprocessed"));
    const ShaderPreprocessResultPtr fragmentHit = cache.Find(ShaderStage::Fragment, hash, source, kEnvA);
    ASSERT_NE(fragmentHit, nullptr);
    EXPECT_EQ(fragmentHit->preprocessedSource, "fragment-preprocessed");
    EXPECT_EQ(cache.Find(ShaderStage::Vertex, hash, source, kEnvA)->preprocessedSource, "vertex-preprocessed");
    EXPECT_EQ(cache.GetEntryCount(), 2u);
}

TEST_F(ProgramUtilTest, ShaderPreprocessCacheMemoizesRejectionVerdictsDistinctly) {
    ShaderPreprocessCache cache;
    const String reservedSource = "int packed;\n";
    const String localSizeSource = "layout(local_size_x = 99999) in;\n";

    auto reserved = MakeShared<ShaderPreprocessResult>();
    reserved->outcome = ShaderPreprocessOutcome::ReservedIdentifierRejected;
    reserved->infoLog = "reserved identifier";
    auto localSize = MakeShared<ShaderPreprocessResult>();
    localSize->outcome = ShaderPreprocessOutcome::ComputeLocalSizeRejected;
    localSize->infoLog = "local_size too big";

    cache.Insert(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(reservedSource), reservedSource, kEnvA,
                 Move(reserved));
    cache.Insert(ShaderStage::Compute, ShaderPreprocessCache::HashSource(localSizeSource), localSizeSource, kEnvA,
                 Move(localSize));

    const ShaderPreprocessResultPtr reservedHit =
        cache.Find(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(reservedSource), reservedSource, kEnvA);
    ASSERT_NE(reservedHit, nullptr);
    EXPECT_FALSE(reservedHit->Preprocessed());
    EXPECT_EQ(reservedHit->outcome, ShaderPreprocessOutcome::ReservedIdentifierRejected);
    EXPECT_EQ(reservedHit->infoLog, "reserved identifier");

    const ShaderPreprocessResultPtr localSizeHit =
        cache.Find(ShaderStage::Compute, ShaderPreprocessCache::HashSource(localSizeSource), localSizeSource, kEnvA);
    ASSERT_NE(localSizeHit, nullptr);
    EXPECT_EQ(localSizeHit->outcome, ShaderPreprocessOutcome::ComputeLocalSizeRejected);
    EXPECT_EQ(localSizeHit->infoLog, "local_size too big");
}

// Correctness must not ride on a 64-bit hash. Feed two different sources of the same
// length under a forged, identical hash: the entry stores the full original text, so
// the impostor lookup must miss instead of returning the wrong preprocess.
TEST_F(ProgramUtilTest, ShaderPreprocessCacheRejectsForgedHashCollision) {
    ShaderPreprocessCache cache;
    const String real = "void main() { int a = 1; }\n";
    const String impostor = "void main() { int a = 2; }\n";
    ASSERT_EQ(real.length(), impostor.length());
    ASSERT_NE(real, impostor);
    const Uint64 forgedHash = 0xdeadbeefcafef00dull;

    cache.Insert(ShaderStage::Vertex, forgedHash, real, kEnvA, MakeResult("real-preprocessed"));

    ASSERT_NE(cache.Find(ShaderStage::Vertex, forgedHash, real, kEnvA), nullptr);
    EXPECT_EQ(cache.Find(ShaderStage::Vertex, forgedHash, impostor, kEnvA), nullptr);

    // The colliding newcomer wins the slot rather than being silently dropped, so it
    // is the previous occupant that degrades to a miss - never a wrong hit.
    cache.Insert(ShaderStage::Vertex, forgedHash, impostor, kEnvA, MakeResult("impostor-preprocessed"));
    const ShaderPreprocessResultPtr impostorHit = cache.Find(ShaderStage::Vertex, forgedHash, impostor, kEnvA);
    ASSERT_NE(impostorHit, nullptr);
    EXPECT_EQ(impostorHit->preprocessedSource, "impostor-preprocessed");
    EXPECT_EQ(cache.Find(ShaderStage::Vertex, forgedHash, real, kEnvA), nullptr);
    EXPECT_EQ(cache.GetEntryCount(), 1u);
}

// P1: the compile environment joins the key. A memo computed against one backend's
// GL_MAX_COMPUTE_WORK_GROUP_* limits must never be handed back after the environment
// changed (backend swap), which is exactly what CompileEnv::fingerprint keys on.
TEST_F(ProgramUtilTest, ShaderPreprocessCacheMissesOnChangedEnvFingerprint) {
    ShaderPreprocessCache cache;
    const String source = "layout(local_size_x = 512) in;\nvoid main() {}\n";
    const Uint64 hash = ShaderPreprocessCache::HashSource(source);

    cache.Insert(ShaderStage::Compute, hash, source, kEnvA, MakeResult("env-a-preprocessed"));
    ASSERT_NE(cache.Find(ShaderStage::Compute, hash, source, kEnvA), nullptr);
    EXPECT_EQ(cache.Find(ShaderStage::Compute, hash, source, kEnvB), nullptr);

    // Both environments can coexist; neither can see the other's verdict.
    cache.Insert(ShaderStage::Compute, hash, source, kEnvB, MakeResult("env-b-preprocessed"));
    EXPECT_EQ(cache.Find(ShaderStage::Compute, hash, source, kEnvA)->preprocessedSource, "env-a-preprocessed");
    EXPECT_EQ(cache.Find(ShaderStage::Compute, hash, source, kEnvB)->preprocessedSource, "env-b-preprocessed");
    EXPECT_EQ(cache.GetEntryCount(), 2u);
}

// A hit hands out shared ownership, so the payload survives the eviction of its entry.
// Under the old raw-pointer API this read was a use-after-free the moment two compiles
// ran concurrently.
TEST_F(ProgramUtilTest, ShaderPreprocessCacheHitOutlivesEviction) {
    ShaderPreprocessCache cache;
    const String source = "void main() { int keep = 1; }\n";
    const Uint64 hash = ShaderPreprocessCache::HashSource(source);
    cache.Insert(ShaderStage::Vertex, hash, source, kEnvA, MakeResult("survivor"));

    const ShaderPreprocessResultPtr held = cache.Find(ShaderStage::Vertex, hash, source, kEnvA);
    ASSERT_NE(held, nullptr);

    cache.Clear();
    EXPECT_EQ(cache.Find(ShaderStage::Vertex, hash, source, kEnvA), nullptr);
    EXPECT_EQ(held->preprocessedSource, "survivor");
}

TEST_F(ProgramUtilTest, ShaderPreprocessCacheEvictsFifoOnEntryCap) {
    ShaderPreprocessCache cache;
    Vector<String> sources;
    const SizeT overflow = ShaderPreprocessCache::kMaxEntries + 8;
    for (SizeT i = 0; i < overflow; ++i) {
        sources.push_back("void main() { int a = " + ToString(i) + "; }\n");
        cache.Insert(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(sources.back()), sources.back(), kEnvA,
                     MakeResult("pp" + ToString(i)));
        EXPECT_LE(cache.GetEntryCount(), ShaderPreprocessCache::kMaxEntries);
    }
    EXPECT_EQ(cache.GetEntryCount(), ShaderPreprocessCache::kMaxEntries);

    // FIFO: the first `overflow - kMaxEntries` insertions are gone, the rest resident.
    for (SizeT i = 0; i < overflow; ++i) {
        const ShaderPreprocessResultPtr hit =
            cache.Find(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(sources[i]), sources[i], kEnvA);
        if (i < overflow - ShaderPreprocessCache::kMaxEntries) {
            EXPECT_EQ(hit, nullptr) << "entry " << i << " should have been evicted";
        } else {
            ASSERT_NE(hit, nullptr) << "entry " << i << " should still be resident";
            EXPECT_EQ(hit->preprocessedSource, "pp" + ToString(i));
        }
    }

    cache.Clear();
    EXPECT_EQ(cache.GetEntryCount(), 0u);
    EXPECT_EQ(cache.GetStoredSourceBytes(), 0u);
}

TEST_F(ProgramUtilTest, ShaderPreprocessCacheHonorsByteBudget) {
    ShaderPreprocessCache cache;
    // Well under the entry cap, well over the byte budget: the byte budget must be the
    // one that binds, and the accounting must come back down as entries are evicted.
    const SizeT chunk = ShaderPreprocessCache::kMaxStoredSourceBytes / 8;
    for (SizeT i = 0; i < 24; ++i) {
        String source(chunk, static_cast<char>('a' + (i % 26)));
        cache.Insert(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(source), source, kEnvA, MakeResult(""));
        EXPECT_LE(cache.GetStoredSourceBytes(), ShaderPreprocessCache::kMaxStoredSourceBytes);
        EXPECT_LT(cache.GetEntryCount(), ShaderPreprocessCache::kMaxEntries);
    }

    // A single source larger than the whole budget is refused outright: caching it
    // would evict every other entry and then immediately itself.
    const SizeT before = cache.GetEntryCount();
    const String oversized(ShaderPreprocessCache::kMaxStoredSourceBytes + 1, 'z');
    cache.Insert(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(oversized), oversized, kEnvA, MakeResult(""));
    EXPECT_EQ(cache.GetEntryCount(), before);
    EXPECT_EQ(cache.Find(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(oversized), oversized, kEnvA), nullptr);
}

// Every vertex input that reaches SPIR-V must carry a Location decoration - including the
// declarations glslang's io-mapper considers INACTIVE.
//
// The shape is Iris's: seven attributes, only some of them bound through
// glBindAttribLocation (ProgramAttrib::explicitVertexInLocations), and at least one neither
// bound nor referenced. GL says only active inputs get generic attribute locations, so the
// resolver deliberately does not RESERVE a slot for a dead one - but it must still RESOLVE a
// location for it, because glslang emits an OpVariable for every declared global (the entry
// point's interface comes from the linker objects) and SPIR-V requires every non-built-in
// Input to be decorated (VUID-StandaloneSpirv-Location-04916).
//
// This test drives the FRONTEND rather than the GL entry points on purpose: it checks the RAW
// GlslangToSpv output, before SanitizeAndOptimizeBinary. A GL-level test cannot see the defect
// for an unreferenced attribute, because AggressiveDCE deletes the offending variable on its
// way to the backend - and yet the real victim (Iris' mc_midTexCoord, Adreno 830,
// programHash 0x4a7e9a37fb49caa1) survived DCE and killed the pipeline with VK_ERROR_UNKNOWN.
TEST_F(ProgramUtilTest, PartiallyBoundVertexInputsAllReceiveALocation) {
    using namespace MG_Util::ShaderTranspiler;

    const String vertexSource = R"(#version 460 core
in vec3 a_Position;
in vec4 a_Color;
in vec2 a_TexCoord;
in vec2 mc_midTexCoord;
in vec4 mc_Entity;
in vec3 iris_Normal;
in vec4 a_Unreferenced;
out vec4 v_Color;
void main() {
    v_Color = a_Color + vec4(a_TexCoord, 0.0, 0.0) + vec4(mc_midTexCoord, 0.0, 0.0) + mc_Entity
            + vec4(iris_Normal, 0.0);
    gl_Position = vec4(a_Position, 1.0);
}
)";

    ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = vertexSource};
    auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
    ASSERT_TRUE(shaderResult) << shaderResult.error().log;

    // PARTIALLY bound, and deliberately not a dense 0..N run - exactly what Iris does.
    // mc_midTexCoord and a_Unreferenced are left unbound.
    UnorderedMap<String, Uint> explicitVertexIns;
    explicitVertexIns["a_Position"] = 0;
    explicitVertexIns["a_Color"] = 1;
    explicitVertexIns["a_TexCoord"] = 2;
    explicitVertexIns["iris_Normal"] = 10;
    explicitVertexIns["mc_Entity"] = 11;
    ProgramAttrib programAttrib{.shaders = {shaderResult.value()},
                                .explicitVertexInLocations = explicitVertexIns};
    auto programResult = ShaderCompiler::LinkProgram(programAttrib);
    ASSERT_TRUE(programResult) << programResult.error().log;

    ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER}, .program = *programResult.value()};
    auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
    ASSERT_TRUE(binaryResult) << binaryResult.error().log;
    ASSERT_EQ(binaryResult->size(), 1u);
    const auto& vertexBinary = binaryResult->front();

    // The authoritative check - this is the same validator whose VUID the driver enforces.
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String validatorMessages;
    tools.SetMessageConsumer([&validatorMessages](spv_message_level_t, const char*, const spv_position_t&,
                                                  const char* message) {
        if (message != nullptr) validatorMessages += String(message) + "\n";
    });
    EXPECT_TRUE(tools.Validate(vertexBinary))
        << "the raw vertex module is not valid SPIR-V; Adreno rejects the whole pipeline for this "
        << "while lavapipe tolerates it:\n"
        << validatorMessages;

    // ...and, independently of the validator, every non-built-in Input carries a UNIQUE location.
    constexpr unsigned kOpDecorate = 71, kOpVariable = 59;
    constexpr unsigned kDecorationBuiltIn = 11, kDecorationLocation = 30;
    constexpr unsigned kStorageClassInput = 1;
    std::map<unsigned, unsigned> locationById;
    std::set<unsigned> builtInIds;
    std::vector<unsigned> inputIds;
    for (SizeT i = 5; i < vertexBinary.size();) { // 5-word header
        const unsigned wordCount = vertexBinary[i] >> 16;
        const unsigned opcode = vertexBinary[i] & 0xFFFFu;
        ASSERT_GT(wordCount, 0u) << "malformed SPIR-V instruction stream";
        if (i + wordCount > vertexBinary.size()) break;
        if (opcode == kOpDecorate && wordCount >= 4 && vertexBinary[i + 2] == kDecorationLocation) {
            locationById[vertexBinary[i + 1]] = vertexBinary[i + 3];
        } else if (opcode == kOpDecorate && wordCount >= 3 && vertexBinary[i + 2] == kDecorationBuiltIn) {
            builtInIds.insert(vertexBinary[i + 1]);
        } else if (opcode == kOpVariable && wordCount >= 4 && vertexBinary[i + 3] == kStorageClassInput) {
            inputIds.push_back(vertexBinary[i + 2]);
        }
        i += wordCount;
    }

    std::set<unsigned> usedLocations;
    SizeT checked = 0;
    for (const unsigned id : inputIds) {
        if (builtInIds.count(id) != 0) continue;
        const auto it = locationById.find(id);
        ASSERT_NE(it, locationById.end())
            << "vertex input id " << id << " reached SPIR-V with no Location decoration";
        EXPECT_TRUE(usedLocations.insert(it->second).second)
            << "two vertex inputs were assigned location " << it->second;
        ++checked;
    }
    EXPECT_GE(checked, 7u) << "expected all seven declared inputs to be present in the raw module";
}

namespace {
    // Storage-class census of module-scope OpVariables plus an OpFunctionCall count -
    // everything the dead-interface-elimination tests need to see, nothing more.
    struct SpirvVariableCensus {
        SizeT inputCount = 0;
        SizeT outputCount = 0;
        SizeT privateCount = 0;
        SizeT functionCallCount = 0;
    };

    SpirvVariableCensus TakeVariableCensus(const Vector<Uint32>& spirv) {
        constexpr unsigned kOpVariable = 59, kOpFunctionCall = 57;
        constexpr unsigned kStorageClassInput = 1, kStorageClassPrivate = 6, kStorageClassOutput = 3;
        SpirvVariableCensus census;
        for (SizeT i = 5; i < spirv.size();) { // 5-word header
            const unsigned wordCount = spirv[i] >> 16;
            const unsigned opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            if (opcode == kOpVariable && wordCount >= 4) {
                switch (spirv[i + 3]) {
                    case kStorageClassInput: ++census.inputCount; break;
                    case kStorageClassOutput: ++census.outputCount; break;
                    case kStorageClassPrivate: ++census.privateCount; break;
                    default: break;
                }
            } else if (opcode == kOpFunctionCall) {
                ++census.functionCallCount;
            }
            i += wordCount;
        }
        return census;
    }

    // The exact Iris shim shape that shipped an invalid module for a month: a declared
    // vertex input whose only use is the initializer of a file-scope global nothing ever
    // reads, in a shader whose main() still contains calls (which is what used to make
    // ADCE keep the whole chain alive).
    constexpr const char* kDeadPrivateChainVertexSource = R"(#version 460 core
in vec3 a_Position;
in vec2 mc_midTexCoord;
out vec4 v_Color;
vec4 iris_MidTex = vec4(mc_midTexCoord * (1.0 / 32768.0), 0.0, 1.0);
vec4 helperTint();
void main() {
    v_Color = helperTint();
    gl_Position = vec4(a_Position, 1.0);
}
vec4 helperTint() { return vec4(1.0); }
)";

    Vector<Uint32> CompileVertexToRawSpirv(const String& source) {
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_VERTEX_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        if (!shaderResult) {
            ADD_FAILURE() << shaderResult.error().log;
            return {};
        }
        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        if (!programResult) {
            ADD_FAILURE() << programResult.error().log;
            return {};
        }
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_VERTEX_SHADER},
                                         .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult || binaryResult->size() != 1u) {
            ADD_FAILURE() << (binaryResult ? "unexpected module count"
                                           : binaryResult.error().log);
            return {};
        }
        return binaryResult->front();
    }

    struct SpirvValidationScope {
        bool previous;
        explicit SpirvValidationScope(bool enabled)
            : previous(MG_Util::ShaderTranspiler::ShaderCompiler::SpirvValidationEnabled()) {
            MG_Util::ShaderTranspiler::ShaderCompiler::SetSpirvValidationEnabled(enabled);
        }
        ~SpirvValidationScope() {
            MG_Util::ShaderTranspiler::ShaderCompiler::SetSpirvValidationEnabled(previous);
        }
    };
} // namespace

TEST_F(ProgramUtilTest, DeadPrivateChainVertexInputIsEliminatedFromOptimizedBinary) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileVertexToRawSpirv(kDeadPrivateChainVertexSource);
    ASSERT_FALSE(raw.empty());

    const SpirvVariableCensus before = TakeVariableCensus(raw);
    // Preconditions that make this module exercise the ADCE conservatism gate: the dead
    // input is present, its Private sink is present, and main() still contains a call.
    // Four Inputs, not two: the frontend always emits gl_VertexIndex/gl_InstanceIndex
    // built-ins alongside a_Position and mc_midTexCoord.
    ASSERT_EQ(before.inputCount, 4u)
        << "expected a_Position, mc_midTexCoord, gl_VertexIndex and gl_InstanceIndex in the raw module";
    ASSERT_GE(before.privateCount, 1u);
    ASSERT_GE(before.functionCallCount, 1u)
        << "helperTint() was inlined by the frontend; this test no longer covers the "
        << "entry-point-with-calls shape it exists for";

    Vector<Uint32> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));

    const SpirvVariableCensus after = TakeVariableCensus(optimized);
    EXPECT_EQ(after.inputCount, 1u)
        << "mc_midTexCoord feeds only a never-read Private global and must not reach the driver";

    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
    String validatorMessages;
    tools.SetMessageConsumer([&validatorMessages](spv_message_level_t, const char*,
                                                  const spv_position_t&, const char* message) {
        if (message != nullptr) validatorMessages += String(message) + "\n";
    });
    EXPECT_TRUE(tools.Validate(optimized)) << validatorMessages;
}

TEST_F(ProgramUtilTest, DeclaredButUnwrittenOutputSurvivesOptimization) {
    using namespace MG_Util::ShaderTranspiler;

    // Chocapic-class packs declare varyings some variants never write while the paired
    // fragment shader still reads them. The OpVariable (and its Location) must survive the
    // chain on both backends: Espryt's ESSL link would otherwise fail with "varying not
    // declared in vertex shader", and Magma's stage-interface contract breaks the same way.
    // ADCE guarantees this only while remove_outputs stays false - this test freezes that.
    const Vector<Uint32> raw = CompileVertexToRawSpirv(R"(#version 460 core
in vec3 a_Position;
out vec4 v_Written;
out vec4 v_NeverWritten;
void main() {
    v_Written = vec4(1.0);
    gl_Position = vec4(a_Position, 1.0);
}
)");
    ASSERT_FALSE(raw.empty());
    // v_Written, v_NeverWritten, and the gl_PerVertex block are all Output-storage variables.
    const SpirvVariableCensus before = TakeVariableCensus(raw);
    ASSERT_GE(before.outputCount, 3u);

    Vector<Uint32> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));
    EXPECT_EQ(TakeVariableCensus(optimized).outputCount, before.outputCount)
        << "a declared-but-unwritten output was deleted; a fragment stage reading it now "
        << "fails to link (ES) or breaks the Vulkan stage interface";
}

TEST_F(ProgramUtilTest, ValidationLatchFlagsInvalidModuleWithoutChangingResults) {
    using namespace MG_Util::ShaderTranspiler;

    // Only LIVE inputs, so the chain cannot heal the module by deleting them: both
    // survive to the output, undecorated, and the output is invalid SPIR-V.
    Vector<Uint32> raw = CompileVertexToRawSpirv(R"(#version 460 core
in vec3 a_Position;
in vec4 a_Color;
out vec4 v_Color;
void main() {
    v_Color = a_Color;
    gl_Position = vec4(a_Position, 1.0);
}
)");
    ASSERT_FALSE(raw.empty());

    // Strip every Input Location decoration - the exact defect class the
    // TMglGlslIoResolver used to ship ([VUID-StandaloneSpirv-Location-04916]).
    constexpr unsigned kOpDecorate = 71, kOpVariable = 59;
    constexpr unsigned kDecorationLocation = 30, kStorageClassInput = 1;
    std::set<unsigned> inputIds;
    for (SizeT i = 5; i < raw.size();) {
        const unsigned wordCount = raw[i] >> 16;
        const unsigned opcode = raw[i] & 0xFFFFu;
        ASSERT_GT(wordCount, 0u);
        if (i + wordCount > raw.size()) break;
        if (opcode == kOpVariable && wordCount >= 4 && raw[i + 3] == kStorageClassInput) {
            inputIds.insert(raw[i + 2]);
        }
        i += wordCount;
    }
    SizeT strippedCount = 0;
    for (SizeT i = 5; i < raw.size();) {
        const unsigned wordCount = raw[i] >> 16;
        const unsigned opcode = raw[i] & 0xFFFFu;
        if (wordCount == 0 || i + wordCount > raw.size()) break;
        if (opcode == kOpDecorate && wordCount >= 4 && raw[i + 2] == kDecorationLocation &&
            inputIds.count(raw[i + 1]) != 0) {
            raw.erase(raw.begin() + static_cast<std::ptrdiff_t>(i),
                      raw.begin() + static_cast<std::ptrdiff_t>(i + wordCount));
            ++strippedCount;
            continue; // do not advance: the next instruction moved into place
        }
        i += wordCount;
    }
    ASSERT_GE(strippedCount, 2u) << "expected to strip both live inputs' Location decorations";

    Vector<Uint32> optimized;
    {
        // The armed lane: control flow is IDENTICAL to shipping (the wrapper still
        // succeeds - fail-open call sites downstream must not see a different world),
        // and the failure latch is the signal. This is the catch that took a device
        // bisect to find when the validator was off everywhere.
        SpirvValidationScope validationOn(true);
        const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));
        EXPECT_GT(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
            << "an invalid optimized module must bump the validation-failure latch";
    }
    {
        // The shipping configuration: same result, no validation, latch untouched.
        SpirvValidationScope validationOff(false);
        const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();
        EXPECT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));
        EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore);
    }
}

namespace {
    // OpTypeImage: result id (+1), sampled type (+2), dim (+3). Dim::Rect == 4.
    SizeT CountRectImageTypes(const Vector<Uint32>& spirv) {
        constexpr unsigned kOpTypeImage = 25, kDimRect = 4;
        SizeT count = 0;
        for (SizeT i = 5; i < spirv.size();) {
            const unsigned wordCount = spirv[i] >> 16;
            const unsigned opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            if (opcode == kOpTypeImage && wordCount >= 4 && spirv[i + 3] == kDimRect) {
                ++count;
            }
            i += wordCount;
        }
        return count;
    }

    // True when any OpDecorate Location targets a UniformConstant/Uniform-storage
    // variable ([VUID-StandaloneSpirv-Location-06672]).
    bool AnyLocationOnUniformStorage(const Vector<Uint32>& spirv) {
        constexpr unsigned kOpDecorate = 71, kOpVariable = 59, kDecorationLocation = 30;
        constexpr unsigned kStorageUniformConstant = 0, kStorageUniform = 2;
        std::set<unsigned> locatedIds;
        for (SizeT i = 5; i < spirv.size();) {
            const unsigned wordCount = spirv[i] >> 16;
            const unsigned opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            if (opcode == kOpDecorate && wordCount >= 4 && spirv[i + 2] == kDecorationLocation) {
                locatedIds.insert(spirv[i + 1]);
            }
            i += wordCount;
        }
        for (SizeT i = 5; i < spirv.size();) {
            const unsigned wordCount = spirv[i] >> 16;
            const unsigned opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) break;
            if (opcode == kOpVariable && wordCount >= 4 &&
                (spirv[i + 3] == kStorageUniformConstant || spirv[i + 3] == kStorageUniform) &&
                locatedIds.count(spirv[i + 2]) != 0) {
                return true;
            }
            i += wordCount;
        }
        return false;
    }
} // namespace

TEST_F(ProgramUtilTest, RectangleSamplerModuleLeavesTheChainVulkanLegal) {
    using namespace MG_Util::ShaderTranspiler;

    // Dim::Rect is invalid under every Vulkan environment; the lowering used to run
    // only in the backends, i.e. AFTER the chain whose output the validating lanes
    // check. It now runs inside the chain, so the driver-bound bytes are rect-free.
    const Vector<Uint32> raw = CompileVertexToRawSpirv(R"(#version 460 core
in vec3 a_Position;
uniform sampler2DRect uRect;
out vec4 v_Color;
void main() {
    v_Color = texture(uRect, a_Position.xy);
    gl_Position = vec4(a_Position, 1.0);
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_GE(CountRectImageTypes(raw), 1u) << "glslang no longer emits Dim::Rect for sampler2DRect";

    SpirvValidationScope validationOn(true);
    const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();
    Vector<Uint32> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));
    EXPECT_EQ(CountRectImageTypes(optimized), 0u);
    EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
        << "a rectangle module must leave the chain valid, not latched as a failure";
}

TEST_F(ProgramUtilTest, ExplicitSamplerLocationIsStrippedFromTheOptimizedBinary) {
    using namespace MG_Util::ShaderTranspiler;

    // glslang's relaxed GL path keeps layout(location=N) on the UniformConstant
    // variable, which Vulkan forbids; nothing downstream reads it (GL locations come
    // from phase-A reflection, Vulkan bindings go by name).
    const Vector<Uint32> raw = CompileVertexToRawSpirv(R"(#version 460 core
in vec3 a_Position;
layout(location = 5) uniform sampler2D uTex;
out vec4 v_Color;
void main() {
    v_Color = texture(uTex, a_Position.xy);
    gl_Position = vec4(a_Position, 1.0);
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_TRUE(AnyLocationOnUniformStorage(raw))
        << "glslang no longer keeps the explicit uniform location; the strip pass may be obsolete";

    SpirvValidationScope validationOn(true);
    const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();
    Vector<Uint32> optimized;
    ASSERT_TRUE(ShaderCompiler::SanitizeAndOptimizeBinary(raw, optimized));
    EXPECT_FALSE(AnyLocationOnUniformStorage(optimized));
    EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
        << "the stripped module must validate clean";
}

// --- Fragment-output array indexing (GLSL ES needs a constant integral expression) -------------
//
// SPIR-V lets a fragment shader index an output array with any integer; GLSL ES does not
// (GLSL ES 3.00 4.3.6). SPIRV-Cross carries the dynamic index straight into the ESSL, a strict
// driver rejects the shader, the program links nothing, and every draw using it silently draws
// nothing - which is what empties the translucent layer of improved-transparency-minecraft-26.3
// on the Android DirectGLES (ANGLE) lane while Mesa, being lenient, renders it correctly.
namespace {
    Vector<Uint32> CompileFragmentToRawSpirv(const String& source) {
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib shaderAttrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = source};
        auto shaderResult = ShaderCompiler::CompileShader(shaderAttrib);
        if (!shaderResult) {
            ADD_FAILURE() << shaderResult.error().log;
            return {};
        }
        ProgramAttrib programAttrib{.shaders = {shaderResult.value()}};
        auto programResult = ShaderCompiler::LinkProgram(programAttrib);
        if (!programResult) {
            ADD_FAILURE() << programResult.error().log;
            return {};
        }
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER},
                                         .program = *programResult.value()};
        auto binaryResult = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binaryResult || binaryResult->size() != 1u) {
            ADD_FAILURE() << (binaryResult ? "unexpected module count" : binaryResult.error().log);
            return {};
        }
        return binaryResult->front();
    }

    String DisassembleSpirv(const Vector<Uint32>& binary) {
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String text;
        tools.Disassemble(binary, &text);
        return text;
    }

    // Every `name[` in the emitted ESSL is followed by a digit. A surviving dynamic index reads
    // `coeff[attachmentIndex]` or `coeff[_123]`, which is the exact construct ES compilers refuse.
    bool AllArrayIndicesAreLiterals(const String& essl, const String& name) {
        const String needle = name + "[";
        SizeT offset = 0;
        bool sawAny = false;
        while ((offset = essl.find(needle, offset)) != String::npos) {
            const SizeT indexStart = offset + needle.size();
            if (indexStart >= essl.size()) return false;
            // A declaration (`out vec4 coeff[2];`) and a constant index both read as a digit.
            if (std::isdigit(static_cast<unsigned char>(essl[indexStart])) == 0) return false;
            sawAny = true;
            offset = indexStart;
        }
        return sawAny;
    }

    String DecompileToEssl(const Vector<Uint32>& binary) {
        using namespace MG_Util::ShaderTranspiler;
        SpvcSession session(binary, SessionUsageBit::Transpile);
        auto essl = ShaderCompiler::DecompileShader(session);
        if (!essl) {
            ADD_FAILURE() << "decompile errc: " << essl.error().errc << "\nlog: " << essl.error().log;
            return {};
        }
        return essl.value();
    }
} // namespace

// The shape Minecraft 26.3's OIT coefficient shader has: the index comes from a loop counter, so
// the stock folding chain (loop-control hint, ssa-rewrite, loop-unroll, ccp, simplification,
// dead-branch-elim) turns every write into a constant-indexed one and the fallback never runs.
TEST_F(ProgramUtilTest, LoopDerivedFragmentOutputIndexFoldsToConstantIndices) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileFragmentToRawSpirv(R"(#version 330 core
out vec4 coeff[2];
in vec4 vColor;
in float vDepth;
void main() {
    for (int attachmentIndex = 0; attachmentIndex < 2; ++attachmentIndex) {
        for (int i = 0; i < 4; ++i) {
            coeff[attachmentIndex][i] = vColor[i] * float(attachmentIndex + i) * vDepth;
        }
    }
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_TRUE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(raw))
        << "the fixture must reproduce the defect before the fix is asked to remove it:\n"
        << DisassembleSpirv(raw);

    SpirvValidationScope validationOn(true);
    const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();

    Vector<Uint32> legalized;
    ASSERT_TRUE(ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(raw, legalized));
    ASSERT_FALSE(legalized.empty());

    const String disassembly = DisassembleSpirv(legalized);
    EXPECT_FALSE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(legalized))
        << "no fragment output may be left indexed by anything but a constant:\n" << disassembly;
    EXPECT_EQ(disassembly.find("OpSwitch"), String::npos)
        << "a loop-derived index must fold, not fall back to the switch lowering:\n" << disassembly;
    EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
        << "the legalized module must stay validator-clean";

    const String essl = DecompileToEssl(legalized);
    ASSERT_FALSE(essl.empty());
    EXPECT_TRUE(AllArrayIndicesAreLiterals(essl, "coeff"))
        << "the generated ESSL still indexes a fragment output with a non-constant:\n" << essl;
}

// The fallback half: an index computed from a uniform cannot be folded by any amount of
// unrolling, so the write becomes a switch over the array's range and the read becomes
// constant-indexed loads combined with selects.
TEST_F(ProgramUtilTest, GenuinelyDynamicFragmentOutputIndexLowersToConstantSwitch) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileFragmentToRawSpirv(R"(#version 330 core
uniform int uTarget;
out vec4 coeff[2];
in vec4 vColor;
void main() {
    coeff[0] = vColor;
    coeff[1] = vColor * 0.5;
    coeff[uTarget] = coeff[uTarget] * 2.0;
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_TRUE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(raw))
        << DisassembleSpirv(raw);

    SpirvValidationScope validationOn(true);
    const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();

    Vector<Uint32> legalized;
    ASSERT_TRUE(ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(raw, legalized));
    ASSERT_FALSE(legalized.empty());

    const String disassembly = DisassembleSpirv(legalized);
    EXPECT_FALSE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(legalized))
        << "the uniform-driven index must be lowered away:\n" << disassembly;
    EXPECT_NE(disassembly.find("OpSwitch"), String::npos)
        << "the dynamic write must become a switch over the array range:\n" << disassembly;
    EXPECT_NE(disassembly.find("OpSelect"), String::npos)
        << "the dynamic read must become constant-indexed loads and a select:\n" << disassembly;
    EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
        << "the lowered module must stay validator-clean:\n" << disassembly;

    const String essl = DecompileToEssl(legalized);
    ASSERT_FALSE(essl.empty());
    EXPECT_TRUE(AllArrayIndicesAreLiterals(essl, "coeff"))
        << "the generated ESSL still indexes a fragment output with a non-constant:\n" << essl;
}

// The bound on the folding half. The index here IS loop-derived, so unrolling would fold it -
// but the loop runs 512 times, and fully unrolling it would multiply the shader by 512 to save
// a switch with two cases. Past the trip-count cap the loop is left alone and the fallback takes
// it, which is cheap in the array length instead of the trip count.
TEST_F(ProgramUtilTest, ALoopTooLongToUnrollFallsBackToTheSwitchLowering) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileFragmentToRawSpirv(R"(#version 330 core
out vec4 coeff[2];
in vec4 vColor;
void main() {
    coeff[0] = vec4(0.0);
    coeff[1] = vec4(0.0);
    for (int i = 0; i < 512; ++i) {
        coeff[i % 2] += vColor * 0.001;
    }
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_TRUE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(raw));

    SpirvValidationScope validationOn(true);
    const Uint64 failuresBefore = ShaderCompiler::SpirvValidationFailureCount();

    Vector<Uint32> legalized;
    ASSERT_TRUE(ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(raw, legalized));
    ASSERT_FALSE(legalized.empty());

    const String disassembly = DisassembleSpirv(legalized);
    EXPECT_FALSE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(legalized))
        << "the index must be legalized even when the loop is left standing:\n" << disassembly;
    EXPECT_NE(disassembly.find("OpLoopMerge"), String::npos)
        << "a 512-trip loop must NOT be unrolled - that is the whole point of the cap:\n"
        << disassembly;
    EXPECT_NE(disassembly.find("OpSwitch"), String::npos)
        << "with the loop standing, the write must go through the switch lowering:\n" << disassembly;
    EXPECT_EQ(ShaderCompiler::SpirvValidationFailureCount(), failuresBefore)
        << "lowering inside a loop body must stay validator-clean:\n" << disassembly;

    const String essl = DecompileToEssl(legalized);
    ASSERT_FALSE(essl.empty());
    EXPECT_TRUE(AllArrayIndicesAreLiterals(essl, "coeff"))
        << "the generated ESSL still indexes a fragment output with a non-constant:\n" << essl;
}

// The gate: a fragment shader that never indexes an output array dynamically must come back byte
// for byte, so no shader that did not need this pays for it or is perturbed by it.
TEST_F(ProgramUtilTest, FragmentWithoutDynamicOutputIndexingIsPassedThroughUnchanged) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileFragmentToRawSpirv(R"(#version 330 core
out vec4 coeff[2];
in vec4 vColor;
void main() {
    for (int i = 0; i < 4; ++i) {
        coeff[0][i] = vColor[i];
    }
    coeff[1] = vColor;
}
)");
    ASSERT_FALSE(raw.empty());
    ASSERT_FALSE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(raw));

    Vector<Uint32> legalized;
    ASSERT_TRUE(ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(raw, legalized));
    EXPECT_EQ(legalized, raw) << "the module must not be rewritten - not even re-serialized - when "
                                 "nothing indexes a fragment output dynamically";
}

// Stages other than fragment may index an output array dynamically in ESSL (the array here is a
// varying, not a draw buffer), so detection must not fire on them at all.
TEST_F(ProgramUtilTest, DynamicOutputIndexingOutsideTheFragmentStageIsNotDetected) {
    using namespace MG_Util::ShaderTranspiler;

    const Vector<Uint32> raw = CompileVertexToRawSpirv(R"(#version 330 core
in vec3 a_Position;
out vec4 v_Values[2];
uniform int uTarget;
void main() {
    v_Values[0] = vec4(0.0);
    v_Values[1] = vec4(1.0);
    v_Values[uTarget] = vec4(a_Position, 1.0);
    gl_Position = vec4(a_Position, 1.0);
}
)");
    ASSERT_FALSE(raw.empty());
    EXPECT_FALSE(LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(raw))
        << "only fragment outputs carry the constant-index rule:\n" << DisassembleSpirv(raw);
}

// ---------------------------------------------------------------------------------------
// Buffer-texture samplers (samplerBuffer / isamplerBuffer / usamplerBuffer)
//
// Buffer textures are core in OpenGL 3.1 and MobileGL advertises a 4.x context, so an
// application may sample one without asking. On the ES side they only became core in 3.2,
// and SPIRV-Cross emits `#extension GL_EXT_texture_buffer : require` for any Dim=Buffer
// image it renders below ESSL 320. On a driver with neither EXT_ nor OES_texture_buffer that
// directive - and the isamplerBuffer keyword behind it - fail to compile, the program never
// links, and every draw using it is a silent no-op. DirectGLES asks the detector below so it
// can name that as the missing capability it is, instead of leaving a driver info log the
// shipped INFO build compiles out.
// ---------------------------------------------------------------------------------------

namespace {
    // Compiles `source` for `stage` and returns the module, or fails the calling test.
    Vector<Uint32> BuildSpirvForStage(const String& source, GLenum stage) {
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib attrib{.shaderType = stage, .sourceStr = source};
        auto res = ShaderCompiler::CompileShader(attrib);
        if (!res) {
            ADD_FAILURE() << "compile errc: " << res.error().errc << "\nlog: " << res.error().log;
            return {};
        }
        ProgramAttrib programAttrib{.shaders = {res.value()}};
        auto program_res = ShaderCompiler::LinkProgram(programAttrib);
        if (!program_res) {
            ADD_FAILURE() << "link errc: " << program_res.error().errc << "\nlog: " << program_res.error().log;
            return {};
        }
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {stage}, .program = *program_res.value()};
        auto bin_res = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!bin_res) {
            ADD_FAILURE() << "spirv errc: " << bin_res.error().errc << "\nlog: " << bin_res.error().log;
            return {};
        }
        if (bin_res.value().size() != 1u) {
            ADD_FAILURE() << "expected exactly one module, got " << bin_res.value().size();
            return {};
        }
        return bin_res.value()[0];
    }
} // namespace

// The shape of Minecraft 26.3's cloud vertex shader: no vertex attributes at all, the whole
// geometry read out of a GL_R8I buffer texture indexed by gl_VertexID.
TEST_F(ProgramUtilTest, BufferTextureSamplerIsDetectedInTheModule) {
    using namespace MG_Util::ShaderTranspiler;

    String vs = R"(#version 330 core
uniform isamplerBuffer CloudFaces;
out vec4 vColor;
void main() {
    int face = texelFetch(CloudFaces, gl_VertexID).r;
    vColor = vec4(float(face) / 255.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";
    const Vector<Uint32> spirv = BuildSpirvForStage(vs, GL_VERTEX_SHADER);
    ASSERT_FALSE(spirv.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresBufferTextureSampler(spirv))
        << "an isamplerBuffer must be recognised as a buffer texture";
}

// The float and unsigned spellings lower to the same Dim=Buffer image with a different
// sampled type, so all three have to be caught by the same check.
TEST_F(ProgramUtilTest, FloatAndUnsignedBufferSamplersAreDetectedToo) {
    using namespace MG_Util::ShaderTranspiler;

    String floatFs = R"(#version 330 core
uniform samplerBuffer Data;
out vec4 fragColor;
void main() { fragColor = texelFetch(Data, 3); }
)";
    const Vector<Uint32> floatSpirv = BuildSpirvForStage(floatFs, GL_FRAGMENT_SHADER);
    ASSERT_FALSE(floatSpirv.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresBufferTextureSampler(floatSpirv));

    String uintFs = R"(#version 330 core
uniform usamplerBuffer Data;
out vec4 fragColor;
void main() { fragColor = vec4(texelFetch(Data, 3)); }
)";
    const Vector<Uint32> uintSpirv = BuildSpirvForStage(uintFs, GL_FRAGMENT_SHADER);
    ASSERT_FALSE(uintSpirv.empty());
    EXPECT_TRUE(ShaderCompiler::ModuleDeclaresBufferTextureSampler(uintSpirv));
}

// The negative control that keeps the detector from turning into "declares any sampler":
// an ordinary sampler2D must not put a program on the unsupported path on a driver that is
// perfectly able to run it.
TEST_F(ProgramUtilTest, OrdinaryTextureSamplersAreNotBufferTextures) {
    using namespace MG_Util::ShaderTranspiler;

    String fs = R"(#version 330 core
uniform sampler2D Albedo;
uniform isampler2D Ids;
in vec2 vUv;
out vec4 fragColor;
void main() { fragColor = texture(Albedo, vUv) + vec4(texelFetch(Ids, ivec2(0), 0)); }
)";
    const Vector<Uint32> spirv = BuildSpirvForStage(fs, GL_FRAGMENT_SHADER);
    ASSERT_FALSE(spirv.empty());
    EXPECT_FALSE(ShaderCompiler::ModuleDeclaresBufferTextureSampler(spirv))
        << "only Dim=Buffer images are buffer textures";
}

// Pins the SPIRV-Cross behaviour the whole defect rests on: below ESSL 320 it synthesizes an
// EXT_texture_buffer requirement from the image type itself. There is nothing in the module
// to strip - which is why the OES driver is served by retargeting the emitted directive
// (RetargetTextureBufferExtension) rather than by rewriting the SPIR-V.
TEST_F(ProgramUtilTest, BufferTextureSamplerEmitsTheExtDirectiveInEssl) {
    using namespace MG_Util::ShaderTranspiler;

    String fs = R"(#version 330 core
uniform isamplerBuffer Data;
out vec4 fragColor;
void main() { fragColor = vec4(texelFetch(Data, 3)); }
)";
    const Vector<Uint32> spirv = BuildSpirvForStage(fs, GL_FRAGMENT_SHADER);
    ASSERT_FALSE(spirv.empty());

    // Emitted at ESSL 310 the way DirectGLES does on an ES 3.1 host (ShaderCompiler's own
    // DecompileShader helper hardcodes 320, where the question does not arise). This is the
    // version the defect lives at: the emulator SDK's ANGLE is ES 3.1 with neither extension.
    auto emitAt = [&spirv](unsigned version) -> String {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        session.CreateOptions(&options);
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, version);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        session.SetOptions(options);
        const char* result = nullptr;
        session.Compile(&result);
        return result ? String(result) : String();
    };

    const String essl310 = emitAt(310);
    ASSERT_FALSE(essl310.empty()) << "ESSL 310 emission failed outright";
    EXPECT_NE(essl310.find("isamplerBuffer"), String::npos)
        << "the buffer sampler must survive to ESSL:\n" << essl310;
    EXPECT_NE(essl310.find("GL_EXT_texture_buffer"), String::npos)
        << "SPIRV-Cross requires EXT_texture_buffer below ESSL 320, and hardcodes that spelling - "
           "which is the whole reason an OES-only driver needs the emitted directive retargeted:\n"
        << essl310;

    // At 320 buffer textures are ES core, so there is no directive to get wrong. This half is
    // what makes the ES 3.2 tier a Pass with nothing to do rather than a silent dependency.
    const String essl320 = emitAt(320);
    ASSERT_FALSE(essl320.empty()) << "ESSL 320 emission failed outright";
    EXPECT_NE(essl320.find("isamplerBuffer"), String::npos) << essl320;
    EXPECT_EQ(essl320.find("GL_EXT_texture_buffer"), String::npos)
        << "ES 3.2 has buffer textures in core; requiring the extension there would be wrong:\n"
        << essl320;

}
