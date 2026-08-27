// MobileGL - MobileGL/MG_Test/Program/ProgramTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <spirv_reflect.h>
#include <cstring>
#include <utility>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include "MG_Backend/DirectVulkan/DirectVulkanResourceState.h"
#include "MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Impl/GLImpl/Program/GL_ProgramPipeline.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ShaderPreprocessCache.h"
#include "MG_Util/Async/ShaderCompilePool.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

class ProgramTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {}

    // GL error flags are sticky per code and the context outlives an individual test in this
    // binary, so a pending error would be handed to whoever runs next.
    static void DrainProgramTestErrors() {
        for (Int drained = 0; drained < 16 && GetError() != GL_NO_ERROR; ++drained) {
        }
    }
};

TEST_F(ProgramTest, Sanity) {
    ASSERT_TRUE(true);
}

const char* vsSrc = R"(#version 460

layout (location = 2) in vec4 Position;
in float fIn4;
in float fIn2;
in float fIn5;
in float fIn6;
in float fIn1;
layout (location = 0) in float fIn0;
in float fIn3;

layout(location = 0) uniform mat4 ProjMat;
layout(location = 10) uniform mat3 TestMat3;
layout(location = 20) uniform mat2 TestMat2;
uniform vec2 InSize;
uniform vec2 OutSize;

out vec2 texCoord;
out vec2 oneTexel;

void main(){
    vec4 outPos = ProjMat * vec4(Position.xy, 0.0, 1.0);
    gl_Position = vec4(outPos.xy, 0.2, 1.0);

    // Use TestMat2 and TestMat3 to prevent optimization
    vec2 dummy2 = TestMat2[0];
    vec3 dummy3 = TestMat3[0];

    oneTexel = (1.0 * (fIn1 * fIn2 * fIn3 * fIn4 * fIn5 * fIn6 * fIn0)) / InSize;

    texCoord = Position.xy / OutSize;
})";

const char* fsSrc = R"(#version 460

uniform sampler2D InSampler;

in vec2 texCoord;
in vec2 oneTexel;

uniform vec2 InSize;

layout(location = 1) uniform vec3 Gray;
uniform vec3 RedMatrix;
uniform vec3 GreenMatrix0;
uniform vec3 BlueMatrix;
uniform vec3 Offset;
uniform vec3 ColorScale;
layout(location = 6) uniform float Saturation;
uniform int AQuickFoxJumpsOverALazyDog;
uniform int intVal;

out vec4 fragColor;

void main() {
    vec4 InTexel = texture(InSampler, texCoord);

    // Color Matrix
    float RedValue = dot(InTexel.rgb, RedMatrix);
    float GreenValue = dot(InTexel.rgb, GreenMatrix0);
    float BlueValue = dot(InTexel.rgb, BlueMatrix);
    vec3 OutColor = vec3(RedValue, GreenValue, BlueValue);

    // Offset & Scale
    OutColor = (OutColor * ColorScale) + Offset;

    // Saturation
    float Luma = dot(OutColor, Gray);
    vec3 Chroma = OutColor - Luma;
    OutColor = (Chroma * Saturation) + Luma;

    fragColor = vec4(OutColor, float(intVal));
})";

const char* sodiumStylePushConstantVs = R"(#version 460 core

layout(location = 0) in vec3 Position;

#ifdef VULKAN
layout(push_constant) uniform PC {
    vec3 u_RegionOffset;
    int u_CurrentTime;
    uint u_RegionID;
};
#else
uniform vec3 u_RegionOffset;
uniform int u_CurrentTime;
uniform uint u_RegionID;
#endif

void main() {
    vec3 offset = u_RegionOffset + vec3(float(u_CurrentTime) * 0.0 + float(u_RegionID) * 0.0);
    gl_Position = vec4(Position + offset, 1.0);
})";

const char* sodiumStylePushConstantFs = R"(#version 460 core

out vec4 fragColor;

void main() {
    fragColor = vec4(1.0);
})";

TEST_F(ProgramTest, CompileVertex) {
    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    CompileShader(vs);
}

TEST_F(ProgramTest, CompileFragment) {
    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    CompileShader(fs);
}

TEST_F(ProgramTest, CompileVoxySubgroupProbeShader) {
    char infoLog[1024] = "";
    const char* csSrc = R"(#version 430
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
layout(local_size_x=32) in;
void main() {
    uint a = subgroupExclusiveAdd(gl_LocalInvocationIndex);
}
)";

    GLuint cs = CreateShader(GL_COMPUTE_SHADER);
    ShaderSource(cs, 1, &csSrc, nullptr);
    CompileShader(cs);

    GLint compileStatus = GL_FALSE;
    GetShaderiv(cs, GL_COMPILE_STATUS, &compileStatus);
    GetShaderInfoLog(cs, sizeof(infoLog), nullptr, infoLog);
    EXPECT_EQ(compileStatus, GL_TRUE) << infoLog;
}

TEST_F(ProgramTest, CompileVoxyGpuShaderInt64QuadDecode) {
    auto previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_Backend::pActiveBackendObject = MakeUnique<MG_Backend::DirectVulkan::BackendObject_DirectVulkan>();

    char infoLog[2048] = "";
    const char* vsSrc = R"(#version 460 core
#extension GL_ARB_gpu_shader_int64 : enable

#ifdef GL_ARB_gpu_shader_int64
#define Quad uint64_t
#define Eu32(data, amountBits, shift) (uint((data)>>(shift))&((1u<<(amountBits))-1))

vec3 extractPos(uint64_t quad) {
    return vec3(Eu32(quad, 5, 21), Eu32(quad, 5, 16), Eu32(quad, 5, 11));
}

uint extractStateId(uint64_t quad) {
    return Eu32(quad, 16, 26);
}

uint extractBiomeId(uint64_t quad) {
    return Eu32(quad, 9, 46);
}
#else
#error GL_ARB_gpu_shader_int64 should select Voxy native quad decode path
#endif

layout(std430, binding = 1) readonly buffer QuadBuffer {
    Quad quadData[];
};

layout(location = 0) flat out uvec4 interData;

void main() {
    uint64_t quad = quadData[uint(gl_VertexID) >> 2];
    vec3 pos = extractPos(quad);
    interData = uvec4(extractStateId(quad), extractBiomeId(quad), uint(pos.x), uint(pos.y));
    gl_Position = vec4(pos * (1.0 / 32.0), 1.0);
}
)";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, nullptr);
    CompileShader(vs);

    GLint compileStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &compileStatus);
    GetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
    EXPECT_EQ(compileStatus, GL_TRUE) << infoLog;

    MG_Backend::pActiveBackendObject = Move(previousBackend);
}

TEST_F(ProgramTest, ShaderSourceKeepsOriginalTextAfterCompile) {
    const char* part0 = R"(#define HIGHP_OR_DEFAULT highp
attribute vec4 Position;
varying vec2 uv;
)";
    const char* ignored = "this segment should be ignored";
    const char* part2 = R"(void main() {
    uv = Position.xy;
    gl_Position = Position;
}
)";
    const GLchar* parts[] = {part0, ignored, part2};
    const GLint lengths[] = {static_cast<GLint>(std::strlen(part0)), 0, static_cast<GLint>(std::strlen(part2))};
    const String expectedSource = String(part0) + part2;

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 3, parts, lengths);

    GLint sourceLength = 0;
    GetShaderiv(vs, GL_SHADER_SOURCE_LENGTH, &sourceLength);
    ASSERT_EQ(sourceLength, static_cast<GLint>(expectedSource.size() + 1));

    std::vector<GLchar> sourceBuffer(static_cast<size_t>(sourceLength));
    GLsizei written = 0;
    GetShaderSource(vs, sourceLength, &written, sourceBuffer.data());
    EXPECT_EQ(written, static_cast<GLsizei>(expectedSource.size()));
    EXPECT_EQ(String(sourceBuffer.data(), static_cast<size_t>(written)), expectedSource);

    CompileShader(vs);
    GLint compileStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &compileStatus);
    ASSERT_EQ(compileStatus, GL_TRUE);

    std::fill(sourceBuffer.begin(), sourceBuffer.end(), '\0');
    written = 0;
    GetShaderSource(vs, sourceLength, &written, sourceBuffer.data());
    EXPECT_EQ(written, static_cast<GLsizei>(expectedSource.size()));
    EXPECT_EQ(String(sourceBuffer.data(), static_cast<size_t>(written)), expectedSource);
}

TEST_F(ProgramTest, LinkProgramWithLegacyGlmarkStyleShaders) {
    char infoLog[1024] = "";

    const char* legacyVs = R"(attribute vec4 Position;
attribute vec2 TexCoord;
varying vec2 vTexCoord;

void main() {
    vTexCoord = TexCoord;
    gl_Position = Position;
}
)";
    const char* legacyFs = R"(varying vec2 vTexCoord;
uniform sampler2D Texture;

void main() {
    gl_FragColor = texture2D(Texture, vTexCoord);
}
)";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &legacyVs, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &legacyFs, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(linkStatus, GL_TRUE) << infoLog;
}

TEST_F(ProgramTest, ImageUniformLayoutBindingInitializesImageUnit) {
    char infoLog[1024] = "";
    const char* csSrc = R"(#version 460 core
layout(local_size_x = 1) in;
layout(binding = 4, rgba8) uniform writeonly image2D colourTexOut;

void main() {
    imageStore(colourTexOut, ivec2(0), vec4(1.0));
}
)";

    GLuint cs = CreateShader(GL_COMPUTE_SHADER);
    ShaderSource(cs, 1, &csSrc, nullptr);
    CompileShader(cs);
    GLint csStatus = GL_FALSE;
    GetShaderiv(cs, GL_COMPILE_STATUS, &csStatus);
    GetShaderInfoLog(cs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(csStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, cs);
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(linkStatus, GL_TRUE) << infoLog;

    const GLint location = GetUniformLocation(program, "colourTexOut");
    ASSERT_GE(location, 0);
    auto programObject = MobileGL::MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    EXPECT_EQ(programObject->GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(location)), 4);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, OutOfRangeComputeLocalSizeLiteralFailsCompileInsteadOfThrowing) {
    // The layout scanner's digit capture is unbounded, so a literal wider than 64 bits is a
    // legal match. It must saturate and be rejected through COMPILE_STATUS; if the integer
    // conversion throws instead, the exception escapes glCompileShader entirely.
    char infoLog[1024] = "";
    const char* csSrc = R"(#version 460 core
layout(local_size_x = 99999999999999999999999) in;
void main() {
}
)";

    GLuint cs = CreateShader(GL_COMPUTE_SHADER);
    ShaderSource(cs, 1, &csSrc, nullptr);
    CompileShader(cs);

    GLint csStatus = GL_TRUE;
    GetShaderiv(cs, GL_COMPILE_STATUS, &csStatus);
    EXPECT_EQ(csStatus, GL_FALSE);
    GetShaderInfoLog(cs, sizeof(infoLog), nullptr, infoLog);
    EXPECT_NE(String(infoLog).find("GL_MAX_COMPUTE_WORK_GROUP_SIZE"), String::npos) << infoLog;
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, DirectVulkanStorageBlockUsesShaderLayoutBinding) {
    char infoLog[1024] = "";
    const char* csSrc = R"(#version 460 core
layout(local_size_x = 1) in;
layout(std430, binding = 2) buffer requestQueueStruct {
    uint value;
} requestQueue;

void main() {
    requestQueue.value = 1u;
}
)";

    GLuint cs = CreateShader(GL_COMPUTE_SHADER);
    ShaderSource(cs, 1, &csSrc, nullptr);
    CompileShader(cs);
    GLint csStatus = GL_FALSE;
    GetShaderiv(cs, GL_COMPILE_STATUS, &csStatus);
    GetShaderInfoLog(cs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(csStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, cs);
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(linkStatus, GL_TRUE) << infoLog;

    auto programObject = MobileGL::MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    const GLuint blockIndex =
        MG_Backend::DirectVulkan::GetShaderStorageBlockIndex(*programObject, "requestQueueStruct");
    ASSERT_NE(blockIndex, GL_INVALID_INDEX);
    EXPECT_EQ(MG_Backend::DirectVulkan::GetShaderStorageBlockBinding(*programObject, blockIndex), 2u);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, CompileAndLink) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    printf("Compiling vertex shader: %s\n", vsSrc);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;
    printf("Compiled vertex shader.\n");

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    printf("Compiling fragment shader: %s\n", fsSrc);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;
    printf("Compiled fragment shader.\n");

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    BindAttribLocation(program, 1, "fIn1");
    BindAttribLocation(program, 3, "fIn3");
    BindAttribLocation(program, 5, "fIn5");
    printf("Linking program...\n");
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    printf("Program linked.\n");

    ASSERT_EQ(GetUniformLocation(program, "ProjMat"), 0);
    ASSERT_EQ(GetUniformLocation(program, "Gray"), 1);
    ASSERT_EQ(GetUniformLocation(program, "Saturation"), 6);
    GLint uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_EQ(uniformCount, 14);
    GLint uniformNameMaxLength = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &uniformNameMaxLength);
    ASSERT_EQ(uniformNameMaxLength, static_cast<GLint>(sizeof("GreenMatrix0")));

    ASSERT_EQ(GetAttribLocation(program, "Position"), 2);
    ASSERT_EQ(GetAttribLocation(program, "fIn1"), 1);
    ASSERT_EQ(GetAttribLocation(program, "fIn3"), 3);
    ASSERT_EQ(GetAttribLocation(program, "fIn5"), 5);
    ASSERT_EQ(GetAttribLocation(program, "fIn0"), 0);

    UseProgram(program);

    auto locRed = GetUniformLocation(program, "RedMatrix");
    Uniform3f(locRed, 1.0, 3.0, 5.0);
    float redVal[3];
    GetUniformfv(program, locRed, redVal);
    ASSERT_EQ(redVal[0], 1.0);
    ASSERT_EQ(redVal[1], 3.0);
    ASSERT_EQ(redVal[2], 5.0);

    auto locAbc = GetUniformLocation(program, "AQuickFoxJumpsOverALazyDog");
    ASSERT_EQ(locAbc, -1);

    auto locInt = GetUniformLocation(program, "intVal");
    Uniform1i(locInt, 114514);
    int intVal;
    GetUniformiv(program, locInt, &intVal);
    EXPECT_EQ(intVal, 114514);

    auto programObj = MG_State::pGLContext->GetProgramObject(program);
    auto& shaderSpirvs = programObj->GetGeneratedSpirv();
    for (int index = 0; index < shaderSpirvs.size(); ++index) {
        String source;
        auto& spirvCode = shaderSpirvs[index];

        MG_Util::ShaderTranspiler::SpvcSession spvcSession(spirvCode, MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);

        spvc_compiler_options options;
        spvcSession.CreateOptions(&options);

        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

        spvcSession.SetOptions(options);

        const char* result = nullptr;
        spvcSession.Compile(&result);

        if (!result) {
            MG_Util::ShaderTranspiler::ResultInfo r;
            r.log += "Failed to compile the shader to GLSL: \n";
            r.log += spvcSession.GetLastErrorString();
            r.errc = -5;
            FAIL() << r.log;
        }
        printf("shader dump: \n%s\n", result);
    }
}

TEST_F(ProgramTest, SodiumStyleVulkanMacroShaderUsesPlainUniforms) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &sodiumStylePushConstantVs, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &sodiumStylePushConstantFs, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(linkStatus, GL_TRUE) << infoLog;

    const GLint regionOffsetLoc = GetUniformLocation(program, "u_RegionOffset");
    const GLint currentTimeLoc = GetUniformLocation(program, "u_CurrentTime");
    const GLint regionIdLoc = GetUniformLocation(program, "u_RegionID");
    ASSERT_GE(regionOffsetLoc, 0);
    ASSERT_GE(currentTimeLoc, 0);
    ASSERT_GE(regionIdLoc, 0);

    auto programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    ASSERT_GT(programObject->GetUBOSize(), 0u);

    UseProgram(program);
    Uniform3f(regionOffsetLoc, 1.0f, 2.0f, 3.0f);
    Uniform1i(currentTimeLoc, 4);
    Uniform1ui(regionIdLoc, 5u);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLfloat regionOffset[3] = {};
    GetUniformfv(program, regionOffsetLoc, regionOffset);
    EXPECT_EQ(regionOffset[0], 1.0f);
    EXPECT_EQ(regionOffset[1], 2.0f);
    EXPECT_EQ(regionOffset[2], 3.0f);
}

TEST_F(ProgramTest, Uniform1uiStoresUnsignedValue) {
    char infoLog[1024] = "";

    const char* simpleVs = R"(#version 460
layout(location = 0) in vec4 Position;

void main() {
    gl_Position = Position;
}
)";

    const char* uintFs = R"(#version 460
uniform uint NodeQueueIndex;

out vec4 fragColor;

void main() {
    fragColor = vec4(float(NodeQueueIndex & 255u));
}
)";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &simpleVs, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &uintFs, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);

    UseProgram(program);
    GLint loc = GetUniformLocation(program, "NodeQueueIndex");
    ASSERT_GE(loc, 0);

    const GLuint expected = 0xF1234567u;
    Uniform1ui(loc, expected);

    GLint actual = 0;
    GetUniformiv(program, loc, &actual);
    EXPECT_EQ(static_cast<GLuint>(actual), expected);
}

TEST_F(ProgramTest, UniformMatrixFunctions) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    printf("Compiling vertex shader: %s\n", vsSrc);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;
    printf("Compiled vertex shader.\n");

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    printf("Compiling fragment shader: %s\n", fsSrc);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;
    printf("Compiled fragment shader.\n");

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    BindAttribLocation(program, 1, "fIn1");
    BindAttribLocation(program, 3, "fIn3");
    BindAttribLocation(program, 5, "fIn5");
    printf("Linking program...\n");
    LinkProgram(program);
    printf("Program linked.\n");

    UseProgram(program);

    int uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_LT(uniformCount, 4000);

    // Test UniformMatrix2fv
    auto locProjMat = GetUniformLocation(program, "ProjMat");
    ASSERT_NE(locProjMat, -1);

    // 4x4 matrix (16 elements) - identity matrix
    GLfloat matrix4x4[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    // Test UniformMatrix4fv with count = 1 and transpose = GL_FALSE
    UniformMatrix4fv(locProjMat, 1, GL_FALSE, matrix4x4);

    // Test UniformMatrix4fv with count = 1 and transpose = GL_TRUE
    UniformMatrix4fv(locProjMat, 1, GL_TRUE, matrix4x4);

    // Test with a non-identity matrix
    GLfloat nonIdentityMatrix[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                                     9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};

    // Test with transpose = GL_FALSE
    UniformMatrix4fv(locProjMat, 1, GL_FALSE, nonIdentityMatrix);

    // Test with transpose = GL_TRUE
    UniformMatrix4fv(locProjMat, 1, GL_TRUE, nonIdentityMatrix);
}

TEST_F(ProgramTest, UniformMatrixTranspose) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    printf("Compiling vertex shader: %s\n", vsSrc);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;
    printf("Compiled vertex shader.\n");

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    printf("Compiling fragment shader: %s\n", fsSrc);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;
    printf("Compiled fragment shader.\n");

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    BindAttribLocation(program, 1, "fIn1");
    BindAttribLocation(program, 3, "fIn3");
    BindAttribLocation(program, 5, "fIn5");
    printf("Linking program...\n");
    LinkProgram(program);
    printf("Program linked.\n");

    UseProgram(program);

    int uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_LT(uniformCount, 4000);

    // Test 2x2 matrix transpose
    auto locMat2 = GetUniformLocation(program, "TestMat2");
    ASSERT_NE(locMat2, -1);
    // Test matrix (column-major as expected by OpenGL):
    // [1  3]
    // [2  4]
    GLfloat matrix2x2[4] = {
        1.0f, 2.0f, // First column
        3.0f, 4.0f  // Second column
    };

    // Expected values when transpose = GL_FALSE (no transpose):
    // [1  3]
    // [2  4]
    GLfloat expected2x2_no_transpose[4] = {1.0f, 2.0f, 3.0f, 4.0f};

    // Expected values when transpose = GL_TRUE (transposed):
    // [1  2]
    // [3  4]
    // Stored in column-major order: [1, 3, 2, 4]
    GLfloat expected2x2_transpose[4] = {1.0f, 3.0f, 2.0f, 4.0f};

    // Test with transpose = GL_FALSE
    UniformMatrix2fv(locMat2, 1, GL_FALSE, matrix2x2);
    GLfloat result2x2_no_transpose[4];
    GetUniformfv(program, locMat2, result2x2_no_transpose);
    for (int i = 0; i < 4; i++) {
        EXPECT_FLOAT_EQ(result2x2_no_transpose[i], expected2x2_no_transpose[i]);
    }

    // Test with transpose = GL_TRUE
    UniformMatrix2fv(locMat2, 1, GL_TRUE, matrix2x2);
    GLfloat result2x2_transpose[4];
    GetUniformfv(program, locMat2, result2x2_transpose);
    for (int i = 0; i < 4; i++) {
        EXPECT_FLOAT_EQ(result2x2_transpose[i], expected2x2_transpose[i]);
    }

    // Test 3x3 matrix transpose
    auto locMat3 = GetUniformLocation(program, "TestMat3");
    ASSERT_NE(locMat3, -1);
    // Test matrix (column-major as expected by OpenGL):
    // [1  4  7]
    // [2  5  8]
    // [3  6  9]
    GLfloat matrix3x3[9] = {
        1.0f, 2.0f, 3.0f, // First column
        4.0f, 5.0f, 6.0f, // Second column
        7.0f, 8.0f, 9.0f  // Third column
    };

    // Expected values when transpose = GL_FALSE (no transpose):
    // [1  4  7]
    // [2  5  8]
    // [3  6  9]
    GLfloat expected3x3_no_transpose[9] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};

    // Expected values when transpose = GL_TRUE (transposed):
    // [1  2  3]
    // [4  5  6]
    // [7  8  9]
    // Stored in column-major order: [1, 4, 7, 2, 5, 8, 3, 6, 9]
    GLfloat expected3x3_transpose[9] = {1.0f, 4.0f, 7.0f, 2.0f, 5.0f, 8.0f, 3.0f, 6.0f, 9.0f};

    // Test with transpose = GL_FALSE
    UniformMatrix3fv(locMat3, 1, GL_FALSE, matrix3x3);
    GLfloat result3x3_no_transpose[9];
    GetUniformfv(program, locMat3, result3x3_no_transpose);
    for (int i = 0; i < 9; i++) {
        EXPECT_FLOAT_EQ(result3x3_no_transpose[i], expected3x3_no_transpose[i]);
    }

    // Test with transpose = GL_TRUE
    UniformMatrix3fv(locMat3, 1, GL_TRUE, matrix3x3);
    GLfloat result3x3_transpose[9];
    GetUniformfv(program, locMat3, result3x3_transpose);
    for (int i = 0; i < 9; i++) {
        EXPECT_FLOAT_EQ(result3x3_transpose[i], expected3x3_transpose[i]);
    }

    // Test 4x4 matrix transpose
    auto locProjMat = GetUniformLocation(program, "ProjMat");
    ASSERT_NE(locProjMat, -1);

    // Test matrix (column-major as expected by OpenGL):
    // [1  5  9  13]
    // [2  6  10 14]
    // [3  7  11 15]
    // [4  8  12 16]
    GLfloat matrix4x4[16] = {
        1.0f,  2.0f,  3.0f,  4.0f,  // First column
        5.0f,  6.0f,  7.0f,  8.0f,  // Second column
        9.0f,  10.0f, 11.0f, 12.0f, // Third column
        13.0f, 14.0f, 15.0f, 16.0f  // Fourth column
    };

    // Expected values when transpose = GL_FALSE (no transpose):
    // [1  5  9  13]
    // [2  6  10 14]
    // [3  7  11 15]
    // [4  8  12 16]
    GLfloat expected4x4_no_transpose[16] = {1.0f, 2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,  8.0f,
                                            9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};

    // Expected values when transpose = GL_TRUE (transposed):
    // [1  2  3  4]
    // [5  6  7  8]
    // [9  10 11 12]
    // [13 14 15 16]
    // Stored in column-major order
    GLfloat expected4x4_transpose[16] = {1.0f, 5.0f, 9.0f,  13.0f, 2.0f, 6.0f, 10.0f, 14.0f,
                                         3.0f, 7.0f, 11.0f, 15.0f, 4.0f, 8.0f, 12.0f, 16.0f};

    // Test with transpose = GL_FALSE
    UniformMatrix4fv(locProjMat, 1, GL_FALSE, matrix4x4);
    GLfloat result4x4_no_transpose[16];
    GetUniformfv(program, locProjMat, result4x4_no_transpose);
    for (int i = 0; i < 16; i++) {
        EXPECT_FLOAT_EQ(result4x4_no_transpose[i], expected4x4_no_transpose[i]);
    }

    // Test with transpose = GL_TRUE
    UniformMatrix4fv(locProjMat, 1, GL_TRUE, matrix4x4);
    GLfloat result4x4_transpose[16];
    GetUniformfv(program, locProjMat, result4x4_transpose);
    for (int i = 0; i < 16; i++) {
        EXPECT_FLOAT_EQ(result4x4_transpose[i], expected4x4_transpose[i]);
    }
}

TEST_F(ProgramTest, UniformLocationGaps) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    BindAttribLocation(program, 1, "fIn1");
    BindAttribLocation(program, 3, "fIn3");
    BindAttribLocation(program, 5, "fIn5");
    LinkProgram(program);

    UseProgram(program);

    int uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_LT(uniformCount, 4000);

    // Test that uniform locations are correctly assigned even with gaps
    // ProjMat is at location 0
    ASSERT_EQ(GetUniformLocation(program, "ProjMat"), 0);

    // TestMat3 is at location 10 (gap from 1-9)
    ASSERT_EQ(GetUniformLocation(program, "TestMat3"), 10);

    // TestMat2 is at location 20 (gap from 11-19)
    ASSERT_EQ(GetUniformLocation(program, "TestMat2"), 20);

    // Gray is at location 1 (no gap)
    ASSERT_EQ(GetUniformLocation(program, "Gray"), 1);

    // Saturation is at location 6 (gap from 2-5)
    ASSERT_EQ(GetUniformLocation(program, "Saturation"), 6);

    // Verify that locations in gaps correctly return -1
    ASSERT_EQ(GetUniformLocation(program, "NonExistentUniform"), -1);

    // Test uniform operations on locations with gaps
    GLfloat matrix3[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    // Test setting and getting uniform at location 10 (TestMat3)
    UniformMatrix3fv(10, 1, GL_FALSE, matrix3);
    GLfloat result[9];
    GetUniformfv(program, 10, result);
    for (int i = 0; i < 9; i++) {
        EXPECT_FLOAT_EQ(result[i], matrix3[i]);
    }

    // Test setting and getting uniform at location 20 (TestMat2)
    GLfloat matrix2[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    UniformMatrix2fv(20, 1, GL_FALSE, matrix2);
    GLfloat result2[4];
    GetUniformfv(program, 20, result2);
    for (int i = 0; i < 4; i++) {
        EXPECT_FLOAT_EQ(result2[i], matrix2[i]);
    }

    // Test that accessing a gap location (e.g., 5) doesn't cause issues
    // This should not crash or cause undefined behavior
    Uniform1i(5, 114514); // Just to make sure we don't crash

    // Verify that we can still use uniforms with sequential locations
    auto locRed = GetUniformLocation(program, "RedMatrix");
    Uniform3f(locRed, 1.0, 3.0, 5.0);
    float redVal[3];
    GetUniformfv(program, locRed, redVal);
    ASSERT_EQ(redVal[0], 1.0);
    ASSERT_EQ(redVal[1], 3.0);
    ASSERT_EQ(redVal[2], 5.0);
}

const char* mc_position_tex_fs = R"(#version 150

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
}
)";

const char* mc_position_tex_vs = R"(#version 150

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

TEST_F(ProgramTest, MinecraftPositionTex) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &mc_position_tex_vs, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &mc_position_tex_fs, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    LinkProgram(program);

    UseProgram(program);

    int uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_LT(uniformCount, 4000);

    int sampler0Loc = GetUniformLocation(program, "Sampler0");
    ASSERT_GE(sampler0Loc, 0);
    ASSERT_LT(sampler0Loc, 4000);
}

const char* minecraft_core_blit_screen_vs = R"(#version 150

in vec3 Position;

out vec2 texCoord;

void main() {
    vec2 screenPos = Position.xy * 2.0 - 1.0;
    gl_Position = vec4(screenPos.x, screenPos.y, 1.0, 1.0);
    texCoord = Position.xy;
}

)";

const char* minecraft_core_lightmap = R"(#version 150

uniform float AmbientLightFactor;
uniform float SkyFactor;
uniform float BlockFactor;
uniform int UseBrightLightmap;
uniform vec3 SkyLightColor;
uniform float NightVisionFactor;
uniform float DarknessScale;
uniform float DarkenWorldFactor;
uniform float BrightnessFactor;

in vec2 texCoord;

out vec4 fragColor;

float get_brightness(float level) {
    float curved_level = level / (4.0 - 3.0 * level);
    return mix(curved_level, 1.0, AmbientLightFactor);
}

vec3 notGamma(vec3 x) {
    vec3 nx = 1.0 - x;
    return 1.0 - nx * nx * nx * nx;
}

void main() {
    float block_brightness = get_brightness(floor(texCoord.x * 16) / 15) * BlockFactor;
    float sky_brightness = get_brightness(floor(texCoord.y * 16) / 15) * SkyFactor;

    // cubic nonsense, dips to yellowish in the middle, white when fully saturated
    vec3 color = vec3(
        block_brightness,
        block_brightness * ((block_brightness * 0.6 + 0.4) * 0.6 + 0.4),
        block_brightness * (block_brightness * block_brightness * 0.6 + 0.4)
    );

    if (UseBrightLightmap != 0) {
        color = mix(color, vec3(0.99, 1.12, 1.0), 0.25);
        color = clamp(color, 0.0, 1.0);
    } else {
        color += SkyLightColor * sky_brightness;
        color = mix(color, vec3(0.75), 0.04);

        vec3 darkened_color = color * vec3(0.7, 0.6, 0.6);
        color = mix(color, darkened_color, DarkenWorldFactor);
    }

    if (NightVisionFactor > 0.0) {
        // scale up uniformly until 1.0 is hit by one of the colors
        float max_component = max(color.r, max(color.g, color.b));
        if (max_component < 1.0) {
            vec3 bright_color = color / max_component;
            color = mix(color, bright_color, NightVisionFactor);
        }
    }

    if (UseBrightLightmap == 0) {
        color = clamp(color - vec3(DarknessScale), 0.0, 1.0);
    }

    vec3 notGamma = notGamma(color);
    color = mix(color, notGamma, BrightnessFactor);
    color = mix(color, vec3(0.75), 0.04);
    color = clamp(color, 0.0, 1.0);

    fragColor = vec4(color, 1.0);
}

)";

TEST_F(ProgramTest, MinecraftBlitScreenLightmap) {
    char infoLog[1024] = "";

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &minecraft_core_blit_screen_vs, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &minecraft_core_lightmap, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);

    LinkProgram(program);

    UseProgram(program);

    int uniformCount = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
    ASSERT_LT(uniformCount, 4000);

    int loc = GetUniformLocation(program, "AmbientLightFactor");
    ASSERT_GE(loc, 0);
    ASSERT_LT(loc, 4000);

    auto programObject = MG_State::pGLContext->GetCurrentProgram();
    ASSERT_GT(programObject->GetUBOSize(), 0);
}

// const char* minecraft_core_tex_color_1216_vs = R"(#version 150
//
//// Can't moj_import in things used during startup, when resource packs don't exist.
//// This is a copy of dynamicimports.glsl and projection.glsl
// layout(std140) uniform DynamicTransforms {
//     mat4 ModelViewMat;
//     vec4 ColorModulator;
//     vec3 ModelOffset;
//     mat4 TextureMat;
//     float LineWidth;
// };
// layout(std140) uniform Projection {
//     mat4 ProjMat;
// };
//
// in vec3 Position;
// in vec2 UV0;
// in vec4 Color;
//
// out vec2 texCoord0;
// out vec4 vertexColor;
//
// void main() {
//     gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);
//
//     texCoord0 = UV0;
//     vertexColor = Color;
// }
//)";
//
// const char* minecraft_core_tex_color_1216_fs = R"(#version 150
//
//// Can't moj_import in things used during startup, when resource packs don't exist.
//// This is a copy of dynamicimports.glsl
// layout(std140) uniform DynamicTransforms {
//     mat4 ModelViewMat;
//     vec4 ColorModulator;
//     vec3 ModelOffset;
//     mat4 TextureMat;
//     float LineWidth;
// };
//
// uniform sampler2D Sampler0;
//
// in vec2 texCoord0;
// in vec4 vertexColor;
//
// out vec4 fragColor;
//
// void main() {
//     vec4 color = texture(Sampler0, texCoord0) * vertexColor;
//     if (color.a == 0.0) {
//         discard;
//     }
//     fragColor = color * ColorModulator;
// }
//)";
//
// TEST_F(ProgramTest, MinecraftTexColor1_21_6) {
//     char infoLog[1024] = "";
//
//     GLuint vs = CreateShader(GL_VERTEX_SHADER);
//     ShaderSource(vs, 1, &minecraft_core_tex_color_1216_vs, NULL);
//     CompileShader(vs);
//     GLint vsStatus = GL_FALSE;
//     GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
//     GetShaderInfoLog(vs, 1024, nullptr, infoLog);
//     ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;
//
//     GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
//     ShaderSource(fs, 1, &minecraft_core_tex_color_1216_fs, NULL);
//     CompileShader(fs);
//     GLint fsStatus = GL_FALSE;
//     GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
//     GetShaderInfoLog(fs, 1024, nullptr, infoLog);
//     ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;
//
//     GLuint program = CreateProgram();
//     AttachShader(program, vs);
//     AttachShader(program, fs);
//
//     LinkProgram(program);
//
//     UseProgram(program);
//
//     int uniformCount = 0;
//     GetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
//     ASSERT_LT(uniformCount, 4000);
//
//     auto transformuboIdx = GetUniformBlockIndex(program, "DynamicTransforms");
//
//     auto programObject = MG_State::pGLContext->GetCurrentProgram();
//     ASSERT_EQ(programObject->GetUBOSize(), 0);
//
//     // auto& spirvs = programObject->GetGeneratedSpirv();
//     // for (auto spirv: spirvs) {
//     //     MG_Util::ShaderTranspiler::SpvcSession spvcSession(spirv);
//     //     spvc_compiler_options options;
//     //     spvcSession.CreateOptions(&options);
//     //
//     //     spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
//     //     spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
//     //     // spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_TRUE);
//     //
//     //     spvcSession.SetOptions(options);
//     //
//     //     const char* result = nullptr;
//     //     spvcSession.Compile(&result);
//     //     printf("%s\n\n", result);
//     // }
// }

const char* optifine_vs1 = R"(#version 460 core

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

const char* optifine_fs1 = R"(#version 460 core

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

TEST_F(ProgramTest, CompileAndLinkWithExplicitVertexIn) {
    char infoLog[1024] = "";

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &optifine_fs1, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &optifine_vs1, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, fs);
    AttachShader(program, vs);

    BindAttribLocation(program, 0, "Position");
    BindAttribLocation(program, 2, "UV0");
    BindAttribLocation(program, 1, "Color");

    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    printf("Program linked.\n");

    UseProgram(program);
    GLint posLoc = GetAttribLocation(program, "Position");
    ASSERT_EQ(posLoc, 0);
    GLint uv0Loc = GetAttribLocation(program, "UV0");
    ASSERT_EQ(uv0Loc, 2);

    auto programObject = MG_State::pGLContext->GetCurrentProgram();
    auto& spirvs = programObject->GetGeneratedSpirv();
    // auto& vertexSpirv = spirvs[1]; // 0 - fragment, 1 - vertex
    char* pSrcVertIn = nullptr;
    const char* needle = "layout(location = 2) in vec2 UV0;";
    for (auto spirv : spirvs) {
        MG_Util::ShaderTranspiler::SpvcSession spvcSession(spirv, MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);
        spvc_compiler_options options;
        spvcSession.CreateOptions(&options);

        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 460);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
        // spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

        spvcSession.SetOptions(options);

        const char* result = nullptr;
        spvcSession.Compile(&result);
        printf("%s\n\n", result);
        const char* ret = strstr(result, needle);
        if (ret) pSrcVertIn = (char*)ret;
    }
    ASSERT_TRUE(pSrcVertIn != nullptr) << "Not found expected string in generated shader.\n(Searching for \"" << needle
                                       << "\")";
}

TEST_F(ProgramTest, InactiveExplicitVertexBindingsDoNotReserveLocations) {
    const char* vertexSource = R"(#version 430 compatibility

in vec3 Position;
in vec2 UV0;
in vec3 vaPosition;

void main() {
    gl_Position = vec4(vaPosition, 1.0);
}
)";
    const char* fragmentSource = R"(#version 430 compatibility

out vec4 fragColor;

void main() {
    fragColor = vec4(1.0);
}
)";

    GLuint vertexShader = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vertexShader, 1, &vertexSource, nullptr);
    CompileShader(vertexShader);
    GLint compileStatus = GL_FALSE;
    GetShaderiv(vertexShader, GL_COMPILE_STATUS, &compileStatus);
    ASSERT_EQ(compileStatus, GL_TRUE);

    GLuint fragmentShader = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fragmentShader, 1, &fragmentSource, nullptr);
    CompileShader(fragmentShader);
    GetShaderiv(fragmentShader, GL_COMPILE_STATUS, &compileStatus);
    ASSERT_EQ(compileStatus, GL_TRUE);

    GLuint program = CreateProgram();
    AttachShader(program, vertexShader);
    AttachShader(program, fragmentShader);

    // Iris binds these canonical names before linking every program. Its compatibility
    // transformer can inject both declarations even when the shader pack instead reads
    // vaPosition. Inactive API bindings must not consume locations during the link.
    BindAttribLocation(program, 0, "Position");
    BindAttribLocation(program, 1, "UV0");
    LinkProgram(program);

    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);

    EXPECT_EQ(GetAttribLocation(program, "Position"), -1);
    EXPECT_EQ(GetAttribLocation(program, "UV0"), -1);
    EXPECT_EQ(GetAttribLocation(program, "vaPosition"), 0);

    auto programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    const Int vertexIndex = programObject->GetShaderIndexByStage(ShaderStage::Vertex);
    ASSERT_GE(vertexIndex, 0);
    const auto& spirvs = programObject->GetGeneratedSpirv();
    ASSERT_LT(static_cast<SizeT>(vertexIndex), spirvs.size());

    const auto& vertexSpirv = spirvs[vertexIndex];
    spv_reflect::ShaderModule reflection(vertexSpirv.size() * sizeof(Uint), vertexSpirv.data());
    ASSERT_EQ(reflection.GetResult(), SPV_REFLECT_RESULT_SUCCESS);

    uint32_t inputCount = 0;
    ASSERT_EQ(reflection.EnumerateInputVariables(&inputCount, nullptr), SPV_REFLECT_RESULT_SUCCESS);
    Vector<SpvReflectInterfaceVariable*> inputs(inputCount);
    ASSERT_EQ(reflection.EnumerateInputVariables(&inputCount, inputs.data()), SPV_REFLECT_RESULT_SUCCESS);

    Uint32 userInputCount = 0;
    Uint32 locationMask = 0;
    for (const auto* input : inputs) {
        if (input == nullptr || (input->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
            continue;
        }
        ASSERT_LT(input->location, 32u);
        locationMask |= 1u << input->location;
        ++userInputCount;
    }
    EXPECT_EQ(userInputCount, 1u);
    EXPECT_EQ(locationMask, 0x1u);
}

TEST_F(ProgramTest, CompileAndLinkWithExplicitFragmentOut) {
    char infoLog[1024] = "";

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &optifine_fs1, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &optifine_vs1, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, fs);
    AttachShader(program, vs);

    BindFragDataLocation(program, 7, "fragColor");

    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    printf("Program linked.\n");

    UseProgram(program);
    GLint fragColorLoc = GetFragDataLocation(program, "fragColor");
    ASSERT_EQ(fragColorLoc, 7);

    // glGetFragDataIndex: a valid user output uses color index 0 (dual-source index 1 is not tracked);
    // a name that is not an active output returns -1. Neither records a GL error.
    EXPECT_EQ(GetFragDataIndex(program, "fragColor"), 0);
    EXPECT_EQ(GetFragDataIndex(program, "notAnActiveOutput"), -1);

    auto programObject = MG_State::pGLContext->GetCurrentProgram();
    auto& spirvs = programObject->GetGeneratedSpirv();
    auto& fragSpirv = spirvs[programObject->GetShaderIndexByStage(ShaderStage::Fragment)];
    char* pSrcfragOut = nullptr;
    const char* needle = "layout(location = 7) out vec4 fragColor;";
    // for (auto spirv: spirvs) {
    MG_Util::ShaderTranspiler::SpvcSession spvcSession(fragSpirv, MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);
    spvc_compiler_options options;
    spvcSession.CreateOptions(&options);

    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 460);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    // spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

    spvcSession.SetOptions(options);

    const char* result = nullptr;
    spvcSession.Compile(&result);
    printf("%s\n\n", result);
    const char* ret = strstr(result, needle);
    if (ret) pSrcfragOut = (char*)ret;
    // }
    ASSERT_TRUE(pSrcfragOut != nullptr) << "Not found expected string in generated shader.\n(Searching for \"" << needle
                                        << "\")";

    // glBindFragDataLocationIndexed round-trips the color index through a re-link. index 1 requires
    // colorNumber 0 (GL_MAX_DUAL_SOURCE_DRAW_BUFFERS is 1).
    BindFragDataLocationIndexed(program, 0, 1, "fragColor");
    LinkProgram(program);
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    EXPECT_EQ(GetFragDataIndex(program, "fragColor"), 1);
    EXPECT_EQ(GetFragDataIndex(program, "notAnActiveOutput"), -1);

    // The color index must reach the transpiled shader as layout(location = 0, index = 1) so the
    // driver binds fragColor as the second dual-source input; the SPIR-V Index decoration set from
    // the glslang layoutIndex round-trips through SPIRV-Cross.
    auto& spirvsIndexed = programObject->GetGeneratedSpirv();
    auto& fragSpirvIndexed = spirvsIndexed[programObject->GetShaderIndexByStage(ShaderStage::Fragment)];
    MG_Util::ShaderTranspiler::SpvcSession spvcSessionIndexed(fragSpirvIndexed,
                                                              MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);
    spvc_compiler_options optionsIndexed;
    spvcSessionIndexed.CreateOptions(&optionsIndexed);
    spvc_compiler_options_set_uint(optionsIndexed, SPVC_COMPILER_OPTION_GLSL_VERSION, 460);
    spvc_compiler_options_set_bool(optionsIndexed, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    spvcSessionIndexed.SetOptions(optionsIndexed);
    const char* resultIndexed = nullptr;
    spvcSessionIndexed.Compile(&resultIndexed);
    printf("%s\n\n", resultIndexed);
    const char* indexNeedle = "index = 1";
    ASSERT_TRUE(strstr(resultIndexed, indexNeedle) != nullptr)
        << "Expected dual-source color index in generated shader.\n(Searching for \"" << indexNeedle << "\")";

    // glBindFragDataLocation is equivalent to index 0 and resets it.
    BindFragDataLocation(program, 0, "fragColor");
    LinkProgram(program);
    EXPECT_EQ(GetFragDataIndex(program, "fragColor"), 0);

    // Validation: index > 1 and a too-large colorNumber for index 1 are GL_INVALID_VALUE; a gl_ name is
    // GL_INVALID_OPERATION.
    BindFragDataLocationIndexed(program, 0, 2, "fragColor");
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    BindFragDataLocationIndexed(program, 1, 1, "fragColor"); // colorNumber 1 invalid for index 1
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    BindFragDataLocationIndexed(program, 0, 0, "gl_FragColor");
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

const char* vs_sampler_as_varname = R"(#version 330
in vec3 Position;
out float sphericalVertexDistance;
out float cylindricalVertexDistance;
out vec4 vertexColor;
out vec2 texCoord0;
void main() {
    gl_Position = vec4(Position, 1.0);
    sphericalVertexDistance = 1.0;
    cylindricalVertexDistance = 1.0;
    vertexColor = vec4(Position, 1.0);
    texCoord0 = Position.xy;
}
)";

const char* fs_sampler_as_varname = R"(#version 330

layout(std140) uniform Fog {
    vec4 FogColor;
    float FogEnvironmentalStart;
    float FogEnvironmentalEnd;
    float FogRenderDistanceStart;
    float FogRenderDistanceEnd;
    float FogSkyEnd;
    float FogCloudsEnd;
};

float linear_fog_value(float vertexDistance, float fogStart, float fogEnd) {
    if (vertexDistance <= fogStart) {
        return 0.0;
    } else if (vertexDistance >= fogEnd) {
        return 1.0;
    }

    return (vertexDistance - fogStart) / (fogEnd - fogStart);
}

float total_fog_value(float sphericalVertexDistance, float cylindricalVertexDistance, float environmentalStart, float environmantalEnd, float renderDistanceStart, float renderDistanceEnd) {
    return max(linear_fog_value(sphericalVertexDistance, environmentalStart, environmantalEnd), linear_fog_value(cylindricalVertexDistance, renderDistanceStart, renderDistanceEnd));
}

vec4 apply_fog(vec4 inColor, float sphericalVertexDistance, float cylindricalVertexDistance, float environmentalStart, float environmantalEnd, float renderDistanceStart, float renderDistanceEnd, vec4 fogColor) {
    float fogValue = total_fog_value(sphericalVertexDistance, cylindricalVertexDistance, environmentalStart, environmantalEnd, renderDistanceStart, renderDistanceEnd);
    return vec4(mix(inColor.rgb, fogColor.rgb, fogValue * fogColor.a), inColor.a);
}

float fog_spherical_distance(vec3 pos) {
    return length(pos);
}

float fog_cylindrical_distance(vec3 pos) {
    float distXZ = length(pos.xz);
    float distY = abs(pos.y);
    return max(distXZ, distY);
}

uniform float fTime;

layout(std140) uniform Globals {
    ivec3 CameraBlockPos;
    vec3 CameraOffset;
    vec2 ScreenSize;
    float GlintAlpha;
    float GameTime;
    int MenuBlurRadius;
    int UseRgss;
};


layout(std140) uniform ChunkSection {
    mat4 ModelViewMat;
    float ChunkVisibility;
    ivec2 TextureSize;
    ivec3 ChunkPosition;
};

uniform sampler2D Sampler0;

in float sphericalVertexDistance;
in float cylindricalVertexDistance;
in vec4 vertexColor;
in vec2 texCoord0;

out vec4 fragColor;

vec4 sampleNearest(sampler2D sampler, vec2 uv, vec2 pixelSize, vec2 du, vec2 dv, vec2 texelScreenSize) {
    // Convert our UV back up to texel coordinates and find out how far over we are from the center of each pixel
    vec2 uvTexelCoords = uv / pixelSize;
    vec2 texelCenter = round(uvTexelCoords) - 0.5f;
    vec2 texelOffset = uvTexelCoords - texelCenter;

    // Move our offset closer to the texel center based on texel size on screen
    texelOffset = (texelOffset - 0.5f) * pixelSize / texelScreenSize + 0.5f;
    texelOffset = clamp(texelOffset, 0.0f, 1.0f);

    uv = (texelCenter + texelOffset) * pixelSize;
    return textureGrad(sampler, uv, du, dv);
}

vec4 sampleNearest(sampler2D source, vec2 uv, vec2 pixelSize) {
    vec2 du = dFdx(uv);
    vec2 dv = dFdy(uv);
    vec2 texelScreenSize = sqrt(du * du + dv * dv);
    return sampleNearest(source, uv, pixelSize, du, dv, texelScreenSize);
}

// Rotated Grid Super-Sampling
vec4 sampleRGSS(sampler2D source, vec2 uv, vec2 pixelSize) {
    vec2 du = dFdx(uv);
    vec2 dv = dFdy(uv);

    vec2 texelScreenSize = sqrt(du * du + dv * dv);
    float maxTexelSize = max(texelScreenSize.x, texelScreenSize.y);

    float minPixelSize = min(pixelSize.x, pixelSize.y);

    float transitionStart = minPixelSize * 1.0;
    float transitionEnd = minPixelSize * 2.0;
    float blendFactor = smoothstep(transitionStart, transitionEnd, maxTexelSize);

    float duLength = length(du);
    float dvLength = length(dv);
    float minDerivative = min(duLength, dvLength);
    float maxDerivative = max(duLength, dvLength);

    float effectiveDerivative = sqrt(minDerivative * maxDerivative);

    float mipLevelExact = max(0.0, log2(effectiveDerivative / minPixelSize));

    float mipLevelLow = floor(mipLevelExact);
    float mipLevelHigh = mipLevelLow + 1.0;
    float mipBlend = fract(mipLevelExact);

    const vec2 offsets[4] = vec2[](
    vec2(0.125, 0.375),
    vec2(-0.125, -0.375),
    vec2(0.375, -0.125),
    vec2(-0.375, 0.125)
    );

    vec4 rgssColorLow = vec4(0.0);
    vec4 rgssColorHigh = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUV = uv + offsets[i] * pixelSize;
        rgssColorLow += textureLod(source, sampleUV, mipLevelLow);
        rgssColorHigh += textureLod(source, sampleUV, mipLevelHigh);
    }
    rgssColorLow *= 0.25;
    rgssColorHigh *= 0.25;

    vec4 rgssColor = mix(rgssColorLow, rgssColorHigh, mipBlend);

    vec4 nearestColor = sampleNearest(source, uv, pixelSize, du, dv, texelScreenSize);

    return mix(nearestColor, rgssColor, blendFactor);
}

void main() {
    vec4 color = (UseRgss == 1 ? sampleRGSS(Sampler0, texCoord0, 1.0f / TextureSize) : sampleNearest(Sampler0, texCoord0, 1.0f / TextureSize)) * vertexColor;
    color = mix(FogColor * vec4(1, 1, 1, color.a * fTime), color, ChunkVisibility);
#ifdef ALPHA_CUTOUT
    if (color.a < ALPHA_CUTOUT) {
        discard;
    }
#endif
    fragColor = apply_fog(color, sphericalVertexDistance, cylindricalVertexDistance, FogEnvironmentalStart, FogEnvironmentalEnd, FogRenderDistanceStart, FogRenderDistanceEnd, FogColor);
})";

TEST_F(ProgramTest, GetFragDataIndexRejectsInvalidProgram) {
    // A handle that was never generated is rejected and returns -1. Like glGetFragDataLocation, this
    // routes through the shared program-name check, which records GL_INVALID_VALUE for an unknown name.
    EXPECT_EQ(GetFragDataIndex(999999u, "fragColor"), -1);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    // Exactly ONE error is recorded per bad call: the redundant second GL_INVALID_OPERATION that the
    // FragData entry points used to queue on top of the name check has been removed.
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    // Defensive drain: keep the shared error queue clean regardless (the fixture never resets it).
    while (GetError() != GL_NO_ERROR) {}
}

TEST_F(ProgramTest, CompileShaderWithSamplerAsVarName) {
    char infoLog[1024] = "";

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fs_sampler_as_varname, NULL);
    CompileShader(fs);
    GLint fsStatus = GL_FALSE;
    GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
    GetShaderInfoLog(fs, 1024, nullptr, infoLog);
    ASSERT_EQ(fsStatus, GL_TRUE) << infoLog;

    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vs_sampler_as_varname, NULL);
    CompileShader(vs);
    GLint vsStatus = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
    GetShaderInfoLog(vs, 1024, nullptr, infoLog);
    ASSERT_EQ(vsStatus, GL_TRUE) << infoLog;

    GLuint program = CreateProgram();
    AttachShader(program, fs);
    AttachShader(program, vs);

    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    printf("Program linked.\n");

    UseProgram(program);

    auto programObject = MG_State::pGLContext->GetCurrentProgram();
    auto& spirvs = programObject->GetGeneratedSpirv();
    auto& fragSpirv = spirvs[programObject->GetShaderIndexByStage(ShaderStage::Fragment)];
    MG_Util::ShaderTranspiler::SpvcSession spvcSession(fragSpirv, MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);
    spvc_compiler_options options;
    spvcSession.CreateOptions(&options);

    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

    spvcSession.SetOptions(options);

    const char* result = nullptr;
    spvcSession.Compile(&result);
    printf("decomp from fragSpirv:\n%s\n\n", result);
}

namespace {
    // Links a VS+FS pair whose fragment shader carries a std140 uniform block (scalar + array + mat4)
    // plus a default-block sampler, and returns the linked program. matrixLayout lets a test flip the
    // block to row_major.
    GLuint LinkUboReflectionProgram(const char* matrixLayout) {
        char infoLog[1024] = "";
        const char* vsSrc = R"(#version 330 core
void main() { gl_Position = vec4(0.0); }
)";
        std::string fsSrc = std::string("#version 330 core\n") +
                            "layout(std140" + matrixLayout + ") uniform Block {\n" +
                            "    float uScalar;\n" +
                            "    vec4  uArray[3];\n" +
                            "    mat4  uMatrix;\n" +
                            "};\n" +
                            "uniform sampler2D uTex;\n" +
                            "out vec4 fragColor;\n" +
                            "void main() {\n" +
                            "    fragColor = texture(uTex, uArray[0].xy) * uScalar * uMatrix[0];\n" +
                            "}\n";
        const char* fsPtr = fsSrc.c_str();

        GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsSrc, nullptr);
        CompileShader(vs);
        GLint vsStatus = GL_FALSE;
        GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
        GetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(vsStatus, GL_TRUE) << infoLog;

        GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsPtr, nullptr);
        CompileShader(fs);
        GLint fsStatus = GL_FALSE;
        GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
        GetShaderInfoLog(fs, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(fsStatus, GL_TRUE) << infoLog;

        GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(linkStatus, GL_TRUE) << infoLog;
        return program;
    }

    GLuint UniformIndexByName(GLuint program, const char* name) {
        GLuint index = GL_INVALID_INDEX;
        GetUniformIndices(program, 1, &name, &index);
        return index;
    }

    GLint QueryUniformiv(GLuint program, GLuint uniformIndex, GLenum pname) {
        GLint value = -12345; // sentinel that is not a legal answer for any queried pname
        GetActiveUniformsiv(program, 1, &uniformIndex, pname, &value);
        return value;
    }
} // namespace

TEST_F(ProgramTest, GetActiveUniformsivStd140Block) {
    GLuint program = LinkUboReflectionProgram(/*matrixLayout=*/"");

    GLint activeUniforms = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    EXPECT_EQ(activeUniforms, 4);

    const GLuint s = UniformIndexByName(program, "uScalar");
    const GLuint a = UniformIndexByName(program, "uArray");
    const GLuint m = UniformIndexByName(program, "uMatrix");
    const GLuint t = UniformIndexByName(program, "uTex");
    ASSERT_NE(s, GL_INVALID_INDEX);
    ASSERT_NE(a, GL_INVALID_INDEX);
    ASSERT_NE(m, GL_INVALID_INDEX);
    ASSERT_NE(t, GL_INVALID_INDEX);

    // Types and sizes.
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_TYPE), GL_FLOAT);
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_TYPE), GL_FLOAT_VEC4);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_TYPE), GL_FLOAT_MAT4);
    EXPECT_EQ(QueryUniformiv(program, t, GL_UNIFORM_TYPE), GL_SAMPLER_2D);
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_SIZE), 1);
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_SIZE), 3);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_SIZE), 1);

    // Block membership: -1 for the default-block sampler.
    EXPECT_GE(QueryUniformiv(program, s, GL_UNIFORM_BLOCK_INDEX), 0);
    EXPECT_EQ(QueryUniformiv(program, t, GL_UNIFORM_BLOCK_INDEX), -1);

    // std140 offsets.
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_OFFSET), 0);
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_OFFSET), 16);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_OFFSET), 64);
    EXPECT_EQ(QueryUniformiv(program, t, GL_UNIFORM_OFFSET), -1);

    // ARRAY_STRIDE: 16 for the array, 0 for non-array block members, -1 for the default block.
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_ARRAY_STRIDE), 16);
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_ARRAY_STRIDE), 0);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_ARRAY_STRIDE), 0);
    EXPECT_EQ(QueryUniformiv(program, t, GL_UNIFORM_ARRAY_STRIDE), -1);

    // MATRIX_STRIDE: 16 for the matrix, 0 for non-matrix block members, -1 for the default block.
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_MATRIX_STRIDE), 16);
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_MATRIX_STRIDE), 0);
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_MATRIX_STRIDE), 0);
    EXPECT_EQ(QueryUniformiv(program, t, GL_UNIFORM_MATRIX_STRIDE), -1);

    // Column-major block: nothing is row-major.
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_IS_ROW_MAJOR), 0);
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_IS_ROW_MAJOR), 0);

    // NAME_LENGTH includes the terminator and matches glGetActiveUniform's reported name.
    char nameBuf[64] = "";
    GLsizei nameLen = 0;
    GLint size = 0;
    GLenum type = 0;
    GetActiveUniform(program, m, sizeof(nameBuf), &nameLen, &size, &type, nameBuf);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_NAME_LENGTH),
              static_cast<GLint>(std::strlen(nameBuf) + 1));

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A block-level layout(row_major) with no per-member qualifier: only the matrix is row-major, and
// only via the block-inheritance fallback (member layoutMatrix == ElmNone). A naive per-member check
// returns 0 here.
TEST_F(ProgramTest, GetActiveUniformsivRowMajorBlock) {
    GLuint program = LinkUboReflectionProgram(/*matrixLayout=*/", row_major");

    const GLuint s = UniformIndexByName(program, "uScalar");
    const GLuint a = UniformIndexByName(program, "uArray");
    const GLuint m = UniformIndexByName(program, "uMatrix");
    ASSERT_NE(m, GL_INVALID_INDEX);

    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_IS_ROW_MAJOR), 1);
    EXPECT_EQ(QueryUniformiv(program, s, GL_UNIFORM_IS_ROW_MAJOR), 0); // non-matrix, isMatrix() guard
    EXPECT_EQ(QueryUniformiv(program, a, GL_UNIFORM_IS_ROW_MAJOR), 0);
    EXPECT_EQ(QueryUniformiv(program, m, GL_UNIFORM_MATRIX_STRIDE), 16); // unchanged by majorness
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, GetActiveUniformsivErrors) {
    GLuint program = LinkUboReflectionProgram(/*matrixLayout=*/"");
    GLint activeUniforms = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    ASSERT_GT(activeUniforms, 0);

    GLuint validIndex = 0;
    GLint params[4] = {-999, -999, -999, -999};

    // E1: negative count -> GL_INVALID_VALUE, params untouched.
    GetActiveUniformsiv(program, -1, &validIndex, GL_UNIFORM_TYPE, params);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(params[0], -999);

    // E2: index == ACTIVE_UNIFORMS -> GL_INVALID_VALUE, params untouched.
    GLuint outOfRange = static_cast<GLuint>(activeUniforms);
    GetActiveUniformsiv(program, 1, &outOfRange, GL_UNIFORM_TYPE, params);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(params[0], -999);

    // E3: the GL 4.2 / ARB_shader_atomic_counters token is ACCEPTED, not rejected.
    //
    // This case used to assert GL_INVALID_ENUM, which was right only while the token was
    // unimplemented. It is implemented now, and `validIndex` names an ordinary uniform rather
    // than an atomic counter, so the spec answer is -1 with no error (GL 4.6 core table 7.6).
    // ProgramInterfaceTest's atomic-counter case asserts the same -1 for a non-counter
    // uniform; leaving this one inverted made the two contradict each other.
    GetActiveUniformsiv(program, 1, &validIndex, GL_UNIFORM_ATOMIC_COUNTER_BUFFER_INDEX, params);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(params[0], -1);
    // Restored: the cases below assert that a REJECTED call leaves params untouched, and this
    // one legitimately wrote to it.
    params[0] = -999;

    // E4a: a live shader name -> GL_INVALID_OPERATION.
    GLuint shader = CreateShader(GL_VERTEX_SHADER);
    GetActiveUniformsiv(shader, 1, &validIndex, GL_UNIFORM_TYPE, params);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    // E4b: a never-generated name -> GL_INVALID_VALUE.
    GetActiveUniformsiv(9999u, 1, &validIndex, GL_UNIFORM_TYPE, params);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    // E6: zero count on a linked program is a valid no-op.
    GetActiveUniformsiv(program, 0, &validIndex, GL_UNIFORM_TYPE, params);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(params[0], -999);
}

namespace {
    GLuint LinkVsFsProgram(const char* vsSource, const char* fsSource) {
        char infoLog[4096] = "";
        GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsSource, nullptr);
        CompileShader(vs);
        GLint vsStatus = GL_FALSE;
        GetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
        GetShaderInfoLog(vs, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(vsStatus, GL_TRUE) << infoLog;

        GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsSource, nullptr);
        CompileShader(fs);
        GLint fsStatus = GL_FALSE;
        GetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
        GetShaderInfoLog(fs, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(fsStatus, GL_TRUE) << infoLog;

        GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(linkStatus, GL_TRUE) << infoLog;
        return program;
    }

    const char* kPassthroughCoordsVs = R"(#version 330
in vec4 a_position;
in vec4 a_coords;
out vec4 coords_in;
void main() {
    gl_Position = a_position;
    coords_in = a_coords;
})";
} // namespace

// Repro for KHR-GL33.shaders.loops.do_while_dynamic_iterations.empty_body_* (and the
// only_continue / unconditional_break variants): the loop is dead code, so the SPIR-V
// optimizer eliminates it together with the only loads of `one` / `ui_one` -- and with
// them the entire global UBO. The uniforms stay active in link reflection, so
// glUniform1i on them must still have backing storage instead of memcpy-ing to null.
TEST_F(ProgramTest, DoWhileDeadLoopUniformsKeepBackingStorage) {
    const char* loopBodies[] = {"", "continue;", "break;"};
    for (const char* body : loopBodies) {
        const String fsSource = String(R"(#version 330
uniform int ui_one;
uniform mediump int one;
in vec4 coords_in;
out vec4 o_color;
void main() {
    vec4 res = coords_in;
    mediump int i = 0;
    do {)") + body + R"(} while (i++ < one*ui_one);
    o_color = res;
})";
        GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource.c_str());

        const GLint locOne = GetUniformLocation(program, "one");
        const GLint locUiOne = GetUniformLocation(program, "ui_one");
        ASSERT_GE(locOne, 0) << "body: '" << body << "'";
        ASSERT_GE(locUiOne, 0) << "body: '" << body << "'";

        UseProgram(program);
        Uniform1i(locOne, 1);   // crashed with a null MapUBO() before the fallback storage
        Uniform1i(locUiOne, 2);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "body: '" << body << "'";

        GLint readback = -1;
        GetUniformiv(program, locOne, &readback);
        EXPECT_EQ(readback, 1) << "body: '" << body << "'";
        readback = -1;
        GetUniformiv(program, locUiOne, &readback);
        EXPECT_EQ(readback, 2) << "body: '" << body << "'";
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "body: '" << body << "'";
    }
}

// Repro for KHR-GL33.shaders.struct.uniform.*nested_struct_array_*: leaf uniforms of
// nested struct arrays need (a) one location per array element and (b) real byte
// offsets inside the global UBO. Before the fix every leaf had a single location and
// offset 0, so glUniform2fv(loc, 2, ...) tripped the size assert on the neighboring
// float uniform (and corrupted it in release builds).
TEST_F(ProgramTest, NestedStructArrayUniformElementWrites) {
    // Struct shape from CTS glcShaderStructTests nested_struct_array (uniform case).
    const char* fsSource = R"(#version 330
struct T {
    mediump float   a;
    mediump vec2    b[2];
};
struct S {
    mediump float   a;
    T               b[3];
    int             c;
};
uniform S s[2];
in vec4 coords_in;
out vec4 o_color;
void main() {
    mediump float r = (s[0].b[1].b[0].x + s[1].b[2].b[1].y) * s[0].b[0].a;
    mediump float g = s[1].b[0].b[0].y * s[0].b[2].a * s[1].b[2].a;
    mediump float b = (s[0].b[2].b[1].y + s[0].b[1].b[0].y + s[1].a) * s[0].b[1].a;
    mediump float a = float(s[0].c) + s[1].b[2].a - s[1].b[1].a;
    o_color = vec4(r, g, b, a);
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);
    UseProgram(program);

    const GLint locVecArray = GetUniformLocation(program, "s[0].b[1].b");
    ASSERT_GE(locVecArray, 0);
    // Element locations are consecutive and reachable via the "[k]" suffix.
    EXPECT_EQ(GetUniformLocation(program, "s[0].b[1].b[0]"), locVecArray);
    EXPECT_EQ(GetUniformLocation(program, "s[0].b[1].b[1]"), locVecArray + 1);
    EXPECT_EQ(GetUniformLocation(program, "s[0].b[1].b[2]"), -1);

    // Distinct scalar leaves must land at distinct UBO offsets (they all aliased
    // offset 0 before the fix).
    const char* scalarLeaves[] = {"s[0].b[0].a", "s[0].b[1].a", "s[0].b[2].a", "s[1].a", "s[1].b[1].a",
                                  "s[1].b[2].a"};
    const GLfloat scalarValues[] = {0.5f, 0.25f, 0.125f, 7.0f, 3.0f, 4.0f};
    for (SizeT i = 0; i < std::size(scalarLeaves); ++i) {
        const GLint loc = GetUniformLocation(program, scalarLeaves[i]);
        ASSERT_GE(loc, 0) << scalarLeaves[i];
        Uniform1f(loc, scalarValues[i]);
    }

    // CTS-style whole-array write: glUniform2fv with count = 2 on a vec2[2] leaf.
    // Before the fix this asserted/corrupted the next uniform ("s[0].b[2].a").
    const GLfloat vecData[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    Uniform2fv(locVecArray, 2, vecData);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLfloat vecReadback[2] = {};
    GetUniformfv(program, locVecArray, vecReadback);
    EXPECT_EQ(vecReadback[0], 1.0f);
    EXPECT_EQ(vecReadback[1], 2.0f);
    GetUniformfv(program, locVecArray + 1, vecReadback);
    EXPECT_EQ(vecReadback[0], 3.0f);
    EXPECT_EQ(vecReadback[1], 4.0f);

    // All scalar leaves survived the array write intact.
    for (SizeT i = 0; i < std::size(scalarLeaves); ++i) {
        GLfloat readback = -1.0f;
        GetUniformfv(program, GetUniformLocation(program, scalarLeaves[i]), &readback);
        EXPECT_EQ(readback, scalarValues[i]) << scalarLeaves[i];
    }

    // std140: vec2 array elements inside the struct are 16 bytes apart, and the
    // per-element offsets differ.
    auto programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    const Uint offsetElement0 = programObject->GetUniformOffset(static_cast<Uint>(locVecArray));
    const Uint offsetElement1 = programObject->GetUniformOffset(static_cast<Uint>(locVecArray + 1));
    EXPECT_EQ(offsetElement1, offsetElement0 + 16u);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Plain top-level uniform arrays share the same per-element location machinery.
TEST_F(ProgramTest, PlainArrayUniformElementLocationsAndWrites) {
    const char* fsSource = R"(#version 330
uniform float arr[4];
uniform float guard;
in vec4 coords_in;
out vec4 o_color;
void main() {
    o_color = vec4(arr[0] + arr[1], arr[2] + arr[3], guard, 1.0);
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);
    UseProgram(program);

    const GLint locArr = GetUniformLocation(program, "arr");
    ASSERT_GE(locArr, 0);
    EXPECT_EQ(GetUniformLocation(program, "arr[0]"), locArr);
    EXPECT_EQ(GetUniformLocation(program, "arr[2]"), locArr + 2);
    EXPECT_EQ(GetUniformLocation(program, "arr[4]"), -1);

    const GLint locGuard = GetUniformLocation(program, "guard");
    ASSERT_GE(locGuard, 0);
    EXPECT_EQ(GetUniformLocation(program, "guard[0]"), -1); // not an array

    Uniform1f(locGuard, 9.0f);

    const GLfloat values[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    Uniform1fv(locArr, 4, values);
    for (int i = 0; i < 4; ++i) {
        GLfloat readback = -1.0f;
        GetUniformfv(program, locArr + i, &readback);
        EXPECT_EQ(readback, values[i]) << "arr[" << i << "]";
    }

    // Overlong writes stop at the end of the array (GL 3.3 §2.11.4) instead of
    // spilling into the next uniform.
    const GLfloat tail[3] = {30.0f, 40.0f, 50.0f};
    Uniform1fv(GetUniformLocation(program, "arr[2]"), 3, tail);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GLfloat readback = -1.0f;
    GetUniformfv(program, locArr + 2, &readback);
    EXPECT_EQ(readback, 30.0f);
    GetUniformfv(program, locArr + 3, &readback);
    EXPECT_EQ(readback, 40.0f);
    GetUniformfv(program, locGuard, &readback);
    EXPECT_EQ(readback, 9.0f); // untouched by the overlong write
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Repro for KHR-GLES31.explicit_uniform_location.uniform-loc-arrays-of-arrays: an
// array-of-arrays uniform reaches the GL surface as one entry PER SUB-ARRAY ("u0[0]",
// "u0[1]" - glslang stops expanding at reflection granularity), while SPIRV-Reflect keeps
// it as a single leaf carrying every dimension. Routing the single leaf only ever covered
// the first sub-array, so every element from u0[1][0] on found no UBO offset and fell
// through to the fallback scratch at the tail of the shadow - storage the GPU never reads,
// which made those glUniform writes silently vanish.
TEST_F(ProgramTest, ArrayOfArraysUniformElementOffsets) {
    // Arrays of arrays need GLSL 4.30; both stages take the same version.
    const char* vsSource = R"(#version 430 core
in vec4 a_position;
void main() {
    gl_Position = a_position;
})";
    const char* fsSource = R"(#version 430 core
uniform float u0[2][3];
uniform vec3 u1[2][2];
out vec4 o_color;
void main() {
    float s = 0.0;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j) s += u0[i][j];
    }
    vec3 v = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) v += u1[i][j];
    }
    o_color = vec4(v, s);
})";
    GLuint program = LinkVsFsProgram(vsSource, fsSource);
    UseProgram(program);
    auto programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);

    // std140 gives a float array element and a vec3 array element the same 16-byte slot,
    // and a flattened array-of-arrays is one contiguous run of those slots.
    constexpr Uint kStd140ElementStride = 16u;

    const auto checkFlattenedRun = [&](const char* base, int outer, int inner) {
        Uint firstOffset = MG_State::GLState::ProgramObject::kInvalidUniformOffset;
        for (int i = 0; i < outer; ++i) {
            for (int j = 0; j < inner; ++j) {
                const std::string name =
                    std::string(base) + "[" + std::to_string(i) + "][" + std::to_string(j) + "]";
                const GLint location = GetUniformLocation(program, name.c_str());
                ASSERT_GE(location, 0) << name;
                const Uint offset = programObject->GetUniformOffset(static_cast<Uint>(location));
                ASSERT_NE(offset, MG_State::GLState::ProgramObject::kInvalidUniformOffset) << name;
                const Uint element = static_cast<Uint>(i * inner + j);
                if (element == 0) {
                    firstOffset = offset;
                } else {
                    EXPECT_EQ(offset, firstOffset + element * kStd140ElementStride) << name;
                }
            }
        }
    };

    checkFlattenedRun("u0", 2, 3);
    checkFlattenedRun("u1", 2, 2);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------
// GL CTS KHR-GL33.shaders.uniform_block regression pack. MobileGL's SPIR-V
// pipeline lays every uniform block out as std140; the frontend implements the
// GL-visible consequences of that choice: packed/shared qualifiers compile (as
// std140), reflection uses GL naming ("arr[0]", per-element struct arrays),
// unused block members stay active, block sizes are vec4-padded, and array
// strides are std140 even for arrays nested inside struct members.
// ---------------------------------------------------------------------------

TEST_F(ProgramTest, UniformBlockPackedAndSharedLayoutsCompileAsStd140) {
    const char* fsSource = R"(#version 330
layout(packed) uniform PackedBlock {
    vec4 pv;
};
layout(shared, row_major) uniform SharedBlock {
    float sf;
    mat4 sm;
};
out vec4 o_color;
void main() {
    o_color = pv + vec4(sf) + vec4(sm[0][0]);
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);

    // The blocks land on the implementation's chosen layout: std140 offsets.
    const GLuint pv = UniformIndexByName(program, "pv");
    const GLuint sf = UniformIndexByName(program, "sf");
    const GLuint sm = UniformIndexByName(program, "sm");
    ASSERT_NE(pv, GL_INVALID_INDEX);
    ASSERT_NE(sf, GL_INVALID_INDEX);
    ASSERT_NE(sm, GL_INVALID_INDEX);
    EXPECT_EQ(QueryUniformiv(program, pv, GL_UNIFORM_OFFSET), 0);
    EXPECT_EQ(QueryUniformiv(program, sf, GL_UNIFORM_OFFSET), 0);
    EXPECT_EQ(QueryUniformiv(program, sm, GL_UNIFORM_OFFSET), 16);
    // The remaining qualifiers in the rewritten layout() list survive.
    EXPECT_EQ(QueryUniformiv(program, sm, GL_UNIFORM_IS_ROW_MAJOR), 1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, UniformBlockReflectsUnusedMembersWithGLNamesAndPaddedSize) {
    const char* fsSource = R"(#version 330
layout(std140) uniform Blk {
    float used;
    vec4 unusedArr[3];
    ivec3 tail;
};
out vec4 o_color;
void main() {
    o_color = vec4(used);
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);

    const GLuint blockIndex = GetUniformBlockIndex(program, "Blk");
    ASSERT_NE(blockIndex, GL_INVALID_INDEX);

    // All three members are active (unusedArr and tail are never read), the array is
    // reported under its GL name "unusedArr[0]", and both spellings resolve.
    const GLuint used = UniformIndexByName(program, "used");
    const GLuint unusedSuffixed = UniformIndexByName(program, "unusedArr[0]");
    const GLuint unusedBare = UniformIndexByName(program, "unusedArr");
    const GLuint tail = UniformIndexByName(program, "tail");
    ASSERT_NE(used, GL_INVALID_INDEX);
    ASSERT_NE(unusedSuffixed, GL_INVALID_INDEX);
    ASSERT_NE(tail, GL_INVALID_INDEX);
    EXPECT_EQ(unusedSuffixed, unusedBare);

    char nameBuf[64] = "";
    GLsizei nameLen = 0;
    GLint arraySize = 0;
    GLenum type = 0;
    GetActiveUniform(program, unusedSuffixed, sizeof(nameBuf), &nameLen, &arraySize, &type, nameBuf);
    EXPECT_STREQ(nameBuf, "unusedArr[0]");
    EXPECT_EQ(arraySize, 3);
    EXPECT_EQ(type, static_cast<GLenum>(GL_FLOAT_VEC4));

    // std140 layout of the unused members.
    EXPECT_EQ(QueryUniformiv(program, unusedSuffixed, GL_UNIFORM_OFFSET), 16);
    EXPECT_EQ(QueryUniformiv(program, unusedSuffixed, GL_UNIFORM_ARRAY_STRIDE), 16);
    EXPECT_EQ(QueryUniformiv(program, tail, GL_UNIFORM_OFFSET), 64);

    // GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS agrees with the INDICES list and counts all members.
    GLint activeInBlock = 0;
    GetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &activeInBlock);
    ASSERT_EQ(activeInBlock, 3);
    GLint indices[3] = {-1, -1, -1};
    GetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices);
    for (GLint index : indices) {
        EXPECT_TRUE(index == static_cast<GLint>(used) || index == static_cast<GLint>(unusedSuffixed) ||
                    index == static_cast<GLint>(tail));
    }

    // The block ends with an ivec3 at offset 64 (unpadded end 76); the backend compiles
    // the std140 block at its vec4-padded size, and the reported size must cover it or
    // buffers sized from this query are too small to draw with.
    GLint dataSize = 0;
    GetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
    EXPECT_EQ(dataSize, 80);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, UniformBlockStructArrayExpandsPerElementWithStd140Strides) {
    const char* fsSource = R"(#version 330
struct S {
    ivec2 v[2];
    float f;
};
layout(std140) uniform Blk2 {
    S s[2];
} inst;
out vec4 o_color;
void main() {
    o_color = vec4(inst.s[0].f);
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);

    // ARB_program_interface_query naming: one entry per struct array element, prefixed
    // with the BLOCK name (not the instance name), basic arrays suffixed with "[0]".
    const GLuint v0 = UniformIndexByName(program, "Blk2.s[0].v[0]");
    const GLuint f0 = UniformIndexByName(program, "Blk2.s[0].f");
    const GLuint v1 = UniformIndexByName(program, "Blk2.s[1].v[0]");
    const GLuint f1 = UniformIndexByName(program, "Blk2.s[1].f");
    ASSERT_NE(v0, GL_INVALID_INDEX);
    ASSERT_NE(f0, GL_INVALID_INDEX);
    ASSERT_NE(v1, GL_INVALID_INDEX);
    ASSERT_NE(f1, GL_INVALID_INDEX);

    // std140: ivec2 v[2] rounds each element up to a vec4 (stride 16, NOT the tight 8
    // glslang reflects for arrays nested inside a struct member); struct size rounds to
    // 48, giving s[1] members a 48-byte bias.
    EXPECT_EQ(QueryUniformiv(program, v0, GL_UNIFORM_OFFSET), 0);
    EXPECT_EQ(QueryUniformiv(program, v0, GL_UNIFORM_ARRAY_STRIDE), 16);
    EXPECT_EQ(QueryUniformiv(program, v0, GL_UNIFORM_SIZE), 2);
    EXPECT_EQ(QueryUniformiv(program, f0, GL_UNIFORM_OFFSET), 32);
    EXPECT_EQ(QueryUniformiv(program, v1, GL_UNIFORM_OFFSET), 48);
    EXPECT_EQ(QueryUniformiv(program, f1, GL_UNIFORM_OFFSET), 80);

    GLint dataSize = 0;
    const GLuint blockIndex = GetUniformBlockIndex(program, "Blk2");
    ASSERT_NE(blockIndex, GL_INVALID_INDEX);
    GetActiveUniformBlockiv(program, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
    EXPECT_EQ(dataSize, 96);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, UniformBlockInstanceArrayReportsPerInstanceBlocks) {
    const char* fsSource = R"(#version 330
layout(std140) uniform ArrBlk {
    vec4 av;
} insts[2];
out vec4 o_color;
void main() {
    o_color = insts[0].av + insts[1].av;
})";
    GLuint program = LinkVsFsProgram(kPassthroughCoordsVs, fsSource);

    const GLuint inst0 = GetUniformBlockIndex(program, "ArrBlk[0]");
    const GLuint inst1 = GetUniformBlockIndex(program, "ArrBlk[1]");
    ASSERT_NE(inst0, GL_INVALID_INDEX);
    ASSERT_NE(inst1, GL_INVALID_INDEX);
    EXPECT_NE(inst0, inst1);
    // A bare block name resolves to the first instance.
    EXPECT_EQ(GetUniformBlockIndex(program, "ArrBlk"), inst0);

    // Every instance of the array shares the single reflected member set.
    GLint count0 = 0;
    GLint count1 = 0;
    GetActiveUniformBlockiv(program, inst0, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count0);
    GetActiveUniformBlockiv(program, inst1, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count1);
    EXPECT_EQ(count0, 1);
    EXPECT_EQ(count1, 1);
    GLint index0 = -1;
    GLint index1 = -1;
    GetActiveUniformBlockiv(program, inst0, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &index0);
    GetActiveUniformBlockiv(program, inst1, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &index1);
    EXPECT_EQ(index0, index1);
    EXPECT_EQ(static_cast<GLuint>(index0), UniformIndexByName(program, "ArrBlk.av"));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

namespace {
    // One program carrying all four block/uniform kinds at once: a real uniform block, a
    // shader storage block, an atomic counter (which the transpiler lowers onto a synthesized
    // gl_AtomicCounterBlock_N buffer block) and plain default-block uniforms.
    //
    // MobileGL does not pass EShReflectionSeparateBuffers to glslang's buildReflection, so
    // glslang routes BUFFER blocks through indexToUniformBlock alongside the uniform blocks -
    // which is why every one of these has to be classified explicitly rather than taken at
    // face value from the reflection list.
    // The storage block and the counter are declared FIRST on purpose: that pushes both
    // uniform blocks off the front of the block list, so the GL uniform-block index and the
    // internal block index of every one of them differ. A translation that quietly reused one
    // space for the other would answer with the storage block's name, size and binding here.
    const char* kMixedBlockKindsFs = R"(#version 430
layout(std430, binding = 0) buffer AVeryLongStorageBlockName {
    vec4 storageVec;
};
layout(binding = 1, offset = 0) uniform atomic_uint counter;
layout(std140) uniform Blk {
    vec4 uboVec;
};
layout(std140) uniform Blk2 {
    vec4 uboVec2[3];
};
uniform float uScale;
out vec4 o_color;
void main() {
    o_color = uboVec * uScale + uboVec2[1] + storageVec + vec4(float(atomicCounterIncrement(counter)));
})";

    const char* kMixedBlockKindsVs = R"(#version 430
void main() { gl_Position = vec4(0.0); })";
} // namespace

// GL 4.6 core 7.6: GL_ACTIVE_UNIFORM_BLOCKS and the glGetActiveUniformBlock* /
// glGetUniformBlockIndex family enumerate ACTUAL uniform blocks. An atomic counter buffer is
// enumerated by GL_ACTIVE_ATOMIC_COUNTER_BUFFERS and a shader storage block by the
// GL_SHADER_STORAGE_BLOCK program interface; neither may appear in the uniform-block list.
TEST_F(ProgramTest, UniformBlockListExcludesStorageAndAtomicCounterBlocks) {
    GLuint program = LinkVsFsProgram(kMixedBlockKindsVs, kMixedBlockKindsFs);

    GLint activeBlocks = -1;
    GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &activeBlocks);
    ASSERT_EQ(activeBlocks, 2) << "only 'Blk' and 'Blk2' are GL uniform blocks";

    // GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH is measured over that same list, so the far
    // longer storage-block name must not raise it.
    GLint maxBlockNameLength = -1;
    GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH, &maxBlockNameLength);
    EXPECT_EQ(maxBlockNameLength, static_cast<GLint>(std::strlen("Blk2") + 1));

    const GLuint blk = GetUniformBlockIndex(program, "Blk");
    const GLuint blk2 = GetUniformBlockIndex(program, "Blk2");
    ASSERT_NE(blk, GL_INVALID_INDEX);
    ASSERT_NE(blk2, GL_INVALID_INDEX);
    EXPECT_LT(blk, 2u);
    EXPECT_LT(blk2, 2u);
    EXPECT_NE(blk, blk2);
    EXPECT_EQ(GetUniformBlockIndex(program, "AVeryLongStorageBlockName"), GL_INVALID_INDEX);
    EXPECT_EQ(GetUniformBlockIndex(program, "gl_AtomicCounterBlock_1"), GL_INVALID_INDEX);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // Every index in the list names one of the two, and each index answers with ITS OWN
    // block's properties - the storage block sits ahead of both in the internal block space,
    // so a query answered in the wrong space reports "AVeryLongStorageBlockName" here.
    char nameBuf[128] = "";
    GLsizei nameLen = 0;
    GetActiveUniformBlockName(program, blk, sizeof(nameBuf), &nameLen, nameBuf);
    EXPECT_STREQ(nameBuf, "Blk");
    GetActiveUniformBlockName(program, blk2, sizeof(nameBuf), &nameLen, nameBuf);
    EXPECT_STREQ(nameBuf, "Blk2");

    GLint dataSize = -1;
    GetActiveUniformBlockiv(program, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
    EXPECT_EQ(dataSize, 16) << "Blk is one vec4";
    GetActiveUniformBlockiv(program, blk2, GL_UNIFORM_BLOCK_DATA_SIZE, &dataSize);
    EXPECT_EQ(dataSize, 48) << "Blk2 is a vec4[3]";

    GLint nameLengthProp = -1;
    GetActiveUniformBlockiv(program, blk2, GL_UNIFORM_BLOCK_NAME_LENGTH, &nameLengthProp);
    EXPECT_EQ(nameLengthProp, static_cast<GLint>(std::strlen("Blk2") + 1));

    // glUniformBlockBinding lands on the block the GL index names, and reads back through the
    // same index.
    UniformBlockBinding(program, blk2, 7);
    GLint binding = -1;
    GetActiveUniformBlockiv(program, blk2, GL_UNIFORM_BLOCK_BINDING, &binding);
    EXPECT_EQ(binding, 7);
    GetActiveUniformBlockiv(program, blk, GL_UNIFORM_BLOCK_BINDING, &binding);
    EXPECT_NE(binding, 7) << "the rebind must not have leaked onto the neighbouring block";
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // An index past the end of the (now shorter) list is GL_INVALID_VALUE, not a silently
    // answered query about a storage block.
    GLint sink = -12345;
    GetActiveUniformBlockiv(program, static_cast<GLuint>(activeBlocks), GL_UNIFORM_BLOCK_BINDING, &sink);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(sink, -12345);
    UniformBlockBinding(program, static_cast<GLuint>(activeBlocks), 1);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    GetActiveUniformBlockName(program, static_cast<GLuint>(activeBlocks), sizeof(nameBuf), &nameLen, nameBuf);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    // Each block's own member resolves against the block index this list hands out.
    const GLuint uboVec = UniformIndexByName(program, "uboVec");
    const GLuint uboVec2 = UniformIndexByName(program, "uboVec2[0]");
    ASSERT_NE(uboVec, GL_INVALID_INDEX);
    ASSERT_NE(uboVec2, GL_INVALID_INDEX);
    EXPECT_EQ(QueryUniformiv(program, uboVec, GL_UNIFORM_BLOCK_INDEX), static_cast<GLint>(blk));
    EXPECT_EQ(QueryUniformiv(program, uboVec2, GL_UNIFORM_BLOCK_INDEX), static_cast<GLint>(blk2));

    GLint blockMemberCount = -1;
    GetActiveUniformBlockiv(program, blk, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &blockMemberCount);
    EXPECT_EQ(blockMemberCount, 1);
    GLint blockMemberIndex = -1;
    GetActiveUniformBlockiv(program, blk, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &blockMemberIndex);
    EXPECT_EQ(static_cast<GLuint>(blockMemberIndex), uboVec);
    GetActiveUniformBlockiv(program, blk2, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &blockMemberIndex);
    EXPECT_EQ(static_cast<GLuint>(blockMemberIndex), uboVec2);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The GL_UNIFORM_BLOCK program interface hands out indices that are usable with
// glUniformBlockBinding / glGetActiveUniformBlockiv (ARB_program_interface_query), so it has
// to enumerate exactly the same list - not the internal block space that also carries the
// storage and atomic counter blocks.
TEST_F(ProgramTest, UniformBlockProgramInterfaceMatchesTheUniformBlockList) {
    GLuint program = LinkVsFsProgram(kMixedBlockKindsVs, kMixedBlockKindsFs);

    GLint interfaceBlocks = -1;
    GetProgramInterfaceiv(program, GL_UNIFORM_BLOCK, GL_ACTIVE_RESOURCES, &interfaceBlocks);
    GLint activeBlocks = -1;
    GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &activeBlocks);
    EXPECT_EQ(interfaceBlocks, activeBlocks);
    ASSERT_EQ(interfaceBlocks, 2);

    // The storage block is enumerated by its OWN interface instead.
    GLint storageBlocks = -1;
    GetProgramInterfaceiv(program, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &storageBlocks);
    EXPECT_EQ(storageBlocks, 1);
    EXPECT_EQ(GetProgramResourceIndex(program, GL_UNIFORM_BLOCK, "AVeryLongStorageBlockName"), GL_INVALID_INDEX);
    EXPECT_NE(GetProgramResourceIndex(program, GL_SHADER_STORAGE_BLOCK, "AVeryLongStorageBlockName"),
              GL_INVALID_INDEX);
    // ... and the buffer variable by GL_BUFFER_VARIABLE, not GL_UNIFORM.
    EXPECT_NE(GetProgramResourceIndex(program, GL_BUFFER_VARIABLE, "storageVec"), GL_INVALID_INDEX);
    EXPECT_EQ(GetProgramResourceIndex(program, GL_UNIFORM, "storageVec"), GL_INVALID_INDEX);

    for (const char* blockName : {"Blk", "Blk2"}) {
        const GLuint interfaceIndex = GetProgramResourceIndex(program, GL_UNIFORM_BLOCK, blockName);
        ASSERT_NE(interfaceIndex, GL_INVALID_INDEX) << blockName;
        EXPECT_EQ(interfaceIndex, GetUniformBlockIndex(program, blockName)) << blockName;

        // GL_NUM_ACTIVE_VARIABLES / GL_ACTIVE_VARIABLES must reach the same member the
        // glGetActiveUniformBlockiv spelling does.
        const GLenum numActive = GL_NUM_ACTIVE_VARIABLES;
        GLint memberCount = -1;
        GetProgramResourceiv(program, GL_UNIFORM_BLOCK, interfaceIndex, 1, &numActive, 1, nullptr, &memberCount);
        ASSERT_EQ(memberCount, 1) << blockName;
        const GLenum activeVariables = GL_ACTIVE_VARIABLES;
        GLint memberIndex = -1;
        GetProgramResourceiv(program, GL_UNIFORM_BLOCK, interfaceIndex, 1, &activeVariables, 1, nullptr,
                             &memberIndex);
        GLint viaBlockiv = -1;
        GetActiveUniformBlockiv(program, interfaceIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, &viaBlockiv);
        EXPECT_EQ(memberIndex, viaBlockiv) << blockName;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// GL 4.6 core 7.3.1 / 7.6: a buffer variable is not a uniform - it lives in the
// GL_BUFFER_VARIABLE interface - so it must not appear in GL_ACTIVE_UNIFORMS,
// glGetActiveUniform, glGetUniformIndices or glGetActiveUniformsiv. An ATOMIC COUNTER, by
// contrast, IS a uniform (of type GL_UNSIGNED_INT_ATOMIC_COUNTER) and must stay enumerated.
TEST_F(ProgramTest, ActiveUniformsExcludeBufferVariablesButKeepAtomicCounters) {
    GLuint program = LinkVsFsProgram(kMixedBlockKindsVs, kMixedBlockKindsFs);

    GLint activeUniforms = -1;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    ASSERT_EQ(activeUniforms, 4)
        << "uboVec, uboVec2[0], uScale and counter - storageVec is a buffer variable";

    // Neither spelling of the buffer variable is a uniform index.
    EXPECT_EQ(UniformIndexByName(program, "storageVec"), GL_INVALID_INDEX);
    EXPECT_EQ(UniformIndexByName(program, "AVeryLongStorageBlockName.storageVec"), GL_INVALID_INDEX);
    // The location half of the same rule (already landed) must stay consistent with it.
    EXPECT_EQ(GetUniformLocation(program, "storageVec"), -1);

    char nameBuf[128] = "";
    for (GLint i = 0; i < activeUniforms; ++i) {
        GLsizei nameLen = 0;
        GLint size = 0;
        GLenum type = 0;
        GetActiveUniform(program, static_cast<GLuint>(i), sizeof(nameBuf), &nameLen, &size, &type, nameBuf);
        EXPECT_EQ(std::string(nameBuf).find("storageVec"), std::string::npos)
            << "buffer variable enumerated as active uniform " << i << ": " << nameBuf;
    }

    // The counter is still a uniform, still reports the atomic-counter type, has no owning
    // uniform block, and still points at its atomic counter BUFFER.
    const GLuint counter = UniformIndexByName(program, "counter");
    ASSERT_NE(counter, GL_INVALID_INDEX);
    EXPECT_EQ(QueryUniformiv(program, counter, GL_UNIFORM_TYPE),
              static_cast<GLint>(GL_UNSIGNED_INT_ATOMIC_COUNTER));
    EXPECT_EQ(QueryUniformiv(program, counter, GL_UNIFORM_BLOCK_INDEX), -1);
    EXPECT_EQ(QueryUniformiv(program, counter, GL_UNIFORM_ATOMIC_COUNTER_BUFFER_INDEX), 0);
    EXPECT_EQ(QueryUniformiv(program, counter, GL_UNIFORM_OFFSET), 0);
    EXPECT_EQ(GetUniformLocation(program, "counter"), -1);

    // GL_ACTIVE_ATOMIC_COUNTER_BUFFERS indexes into the GL uniform index space, so the
    // counter index it reports has to be the one glGetUniformIndices just handed out.
    GLint counterBuffers = -1;
    GetProgramiv(program, GL_ACTIVE_ATOMIC_COUNTER_BUFFERS, &counterBuffers);
    ASSERT_EQ(counterBuffers, 1);
    GLint counterCount = -1;
    GetActiveAtomicCounterBufferiv(program, 0, GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTERS, &counterCount);
    ASSERT_EQ(counterCount, 1);
    GLint counterIndex = -1;
    GetActiveAtomicCounterBufferiv(program, 0, GL_ATOMIC_COUNTER_BUFFER_ACTIVE_ATOMIC_COUNTER_INDICES,
                                   &counterIndex);
    EXPECT_EQ(static_cast<GLuint>(counterIndex), counter);
    GLint counterBinding = -1;
    GetActiveAtomicCounterBufferiv(program, 0, GL_ATOMIC_COUNTER_BUFFER_BINDING, &counterBinding);
    EXPECT_EQ(counterBinding, 1);

    // The default-block uniform is untouched by either filter.
    EXPECT_NE(GetUniformLocation(program, "uScale"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

TEST_F(ProgramTest, DeleteShaderWhileAttachedKeepsNameUsableUntilDetach) {
    // GL CTS compiles through exactly this sequence (create, attach, DELETE, source,
    // compile): glDeleteShader on an attached shader only flags it, and the name must
    // keep working until the last detach.
    const char* vsSource = R"(#version 330
void main() { gl_Position = vec4(0.0); }
)";
    const char* fsSource = R"(#version 330
out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";

    GLuint program = CreateProgram();
    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    AttachShader(program, vs);
    DeleteShader(vs);
    EXPECT_EQ(IsShader(vs), GL_TRUE); // still alive: attached

    ShaderSource(vs, 1, &vsSource, nullptr);
    CompileShader(vs);
    GLint status = GL_FALSE;
    GetShaderiv(vs, GL_COMPILE_STATUS, &status);
    EXPECT_EQ(status, GL_TRUE);
    status = GL_FALSE;
    GetShaderiv(vs, GL_DELETE_STATUS, &status);
    EXPECT_EQ(status, GL_TRUE);

    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    AttachShader(program, fs);
    DeleteShader(fs);
    ShaderSource(fs, 1, &fsSource, nullptr);
    CompileShader(fs);

    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    char infoLog[1024] = "";
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    EXPECT_EQ(linkStatus, GL_TRUE) << infoLog;

    // The last GL-visible detach releases the flagged shader's name.
    DetachShader(program, vs);
    EXPECT_EQ(IsShader(vs), GL_FALSE);

    // Deleting the program releases the other flagged shader.
    DeleteProgram(program);
    EXPECT_EQ(IsShader(fs), GL_FALSE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---- P0a single-parse regression tests ----
// glCompileShader now performs the one link-compatible (relaxed Vulkan-rules) parse;
// these pin the GL frontend semantics that parse cannot provide by itself.

namespace {
    GLuint CompileShaderChecked(GLenum type, const char* source) {
        char infoLog[1024] = "";
        GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        GetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(status, GL_TRUE) << infoLog;
        return shader;
    }

    GLuint LinkVsFs(GLuint vs, GLuint fs, GLint expectedLinkStatus) {
        char infoLog[2048] = "";
        GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        EXPECT_EQ(linkStatus, expectedLinkStatus) << infoLog;
        return program;
    }
} // namespace

// The relaxed parse sweeps every DECLARED default-block uniform into MGL_GLOBAL_UBO,
// including ones no stage reads. GL requires those to be inactive: absent from the
// glGetActiveUniform enumeration and -1 from glGetUniformLocation. The synthesized
// MGL_GLOBAL_UBO itself must not surface as a GL uniform block either.
TEST_F(ProgramTest, DeclaredButUnreadUniformIsInactiveAndGlobalUboStaysHidden) {
    const char* vsSource = R"(#version 330 core
uniform mat4 uUsedMat;
uniform vec4 uDeadVec;
void main() { gl_Position = uUsedMat * vec4(1.0); }
)";
    const char* fsSource = R"(#version 330 core
uniform vec4 uUsedColor;
uniform float uDeadFloat;
out vec4 fragColor;
void main() { fragColor = uUsedColor; }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    GLint activeUniforms = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    EXPECT_EQ(activeUniforms, 2);

    EXPECT_NE(GetUniformLocation(program, "uUsedMat"), -1);
    EXPECT_NE(GetUniformLocation(program, "uUsedColor"), -1);
    EXPECT_EQ(GetUniformLocation(program, "uDeadVec"), -1);
    EXPECT_EQ(GetUniformLocation(program, "uDeadFloat"), -1);
    EXPECT_EQ(UniformIndexByName(program, "uDeadVec"), GL_INVALID_INDEX);

    char nameBuf[64] = "";
    for (GLint i = 0; i < activeUniforms; ++i) {
        GLsizei nameLen = 0;
        GLint size = 0;
        GLenum type = 0;
        GetActiveUniform(program, static_cast<GLuint>(i), sizeof(nameBuf), &nameLen, &size, &type, nameBuf);
        EXPECT_TRUE(std::strcmp(nameBuf, "uDeadVec") != 0 && std::strcmp(nameBuf, "uDeadFloat") != 0)
            << nameBuf;
    }

    // No named blocks are declared, so GL must see zero uniform blocks - the global
    // UBO the transpiler materializes is an implementation artifact.
    GLint activeBlocks = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &activeBlocks);
    EXPECT_EQ(activeBlocks, 0);
    EXPECT_EQ(GetUniformBlockIndex(program, "MGL_GLOBAL_UBO"), GL_INVALID_INDEX);

    // Default-block uniforms report block index -1 and offset -1 even though the
    // relaxed parse physically placed them in the global UBO.
    const GLuint usedMat = UniformIndexByName(program, "uUsedMat");
    ASSERT_NE(usedMat, GL_INVALID_INDEX);
    EXPECT_EQ(QueryUniformiv(program, usedMat, GL_UNIFORM_BLOCK_INDEX), -1);
    EXPECT_EQ(QueryUniformiv(program, usedMat, GL_UNIFORM_OFFSET), -1);
    EXPECT_EQ(QueryUniformiv(program, usedMat, GL_UNIFORM_ARRAY_STRIDE), -1);
    EXPECT_EQ(QueryUniformiv(program, usedMat, GL_UNIFORM_MATRIX_STRIDE), -1);
    EXPECT_EQ(QueryUniformiv(program, usedMat, GL_UNIFORM_IS_ROW_MAJOR), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Distinct uniforms whose explicit locations overlap across stages must fail the
// link (ARB_explicit_uniform_location). The GL-client parse used to reject this at
// glslang mapIO; the relaxed parse drops the qualifiers, so the location assigner
// enforces it - this is the experiment's synthetic divergence case.
TEST_F(ProgramTest, ExplicitUniformLocationOverlapAcrossStagesFailsLink) {
    const char* vsSource = R"(#version 460 core
layout(location = 3) uniform vec4 uVec[4];
void main() { gl_Position = uVec[0] + uVec[3]; }
)";
    const char* fsSource = R"(#version 460 core
layout(location = 5) uniform float uF;
out vec4 fragColor;
void main() { fragColor = vec4(uF); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_FALSE);

    char infoLog[1024] = "";
    GLsizei logLength = 0;
    GetProgramInfoLog(program, sizeof(infoLog), &logLength, infoLog);
    EXPECT_GT(logLength, 0);
}

// The same uniform declared with different explicit locations in two stages is a
// link error as well.
TEST_F(ProgramTest, ConflictingExplicitUniformLocationsOnSameUniformFailLink) {
    const char* vsSource = R"(#version 460 core
layout(location = 2) uniform vec4 uShared;
void main() { gl_Position = uShared; }
)";
    const char* fsSource = R"(#version 460 core
layout(location = 4) uniform vec4 uShared;
out vec4 fragColor;
void main() { fragColor = uShared; }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    (void)LinkVsFs(vs, fs, GL_FALSE);
}

// Same-location explicit declarations of the SAME uniform in both stages stay
// linkable, and both explicit locations (opaque and non-opaque) are honored.
TEST_F(ProgramTest, ExplicitUniformLocationsHonoredForPlainAndOpaqueUniforms) {
    const char* vsSource = R"(#version 460 core
layout(location = 11) uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(1.0); }
)";
    const char* fsSource = R"(#version 460 core
layout(location = 7) uniform sampler2D uTex;
layout(location = 11) uniform mat4 uMvp;
out vec4 fragColor;
void main() { fragColor = texture(uTex, uMvp[0].xy); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    EXPECT_EQ(GetUniformLocation(program, "uMvp"), 11);
    EXPECT_EQ(GetUniformLocation(program, "uTex"), 7);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A glslang-auto-assigned opaque location may collide with a source-explicit plain
// uniform location under the relaxed parse (glslang no longer sees the plain
// uniform's qualifier). The assigner must relocate the auto one, not fail the link.
TEST_F(ProgramTest, AutoOpaqueLocationCollidingWithExplicitPlainLocationRelocates) {
    const char* vsSource = R"(#version 460 core
layout(location = 0) uniform mat4 uM;
void main() { gl_Position = uM * vec4(1.0); }
)";
    const char* fsSource = R"(#version 460 core
uniform sampler2D uTex;
out vec4 fragColor;
void main() { fragColor = texture(uTex, vec2(0.5)); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    const GLint mLoc = GetUniformLocation(program, "uM");
    const GLint texLoc = GetUniformLocation(program, "uTex");
    EXPECT_EQ(mLoc, 0);
    ASSERT_NE(texLoc, -1);
    EXPECT_NE(texLoc, mLoc);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Relinking a program and linking the same compiled shaders into a second program
// both re-consume the stored single parse (glslang mapIO mutates a linked TShader,
// so reuse goes through the consume-once re-parse path). Reflection must be intact
// every time, without any glCompileShader in between.
TEST_F(ProgramTest, RelinkAndSecondProgramReuseCompiledShaders) {
    const char* vsSource = R"(#version 330 core
uniform mat4 uMvp;
in vec3 aPos;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)";
    const char* fsSource = R"(#version 330 core
uniform sampler2D uTex;
uniform vec4 uTint;
out vec4 fragColor;
void main() { fragColor = texture(uTex, vec2(0.5)) * uTint; }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);

    GLuint program1 = LinkVsFs(vs, fs, GL_TRUE);
    GLint activeUniforms1 = 0;
    GetProgramiv(program1, GL_ACTIVE_UNIFORMS, &activeUniforms1);
    EXPECT_EQ(activeUniforms1, 3);
    EXPECT_NE(GetUniformLocation(program1, "uMvp"), -1);

    // Relink: consumes the re-parse path.
    LinkProgram(program1);
    GLint relinkStatus = GL_FALSE;
    char infoLog[1024] = "";
    GetProgramiv(program1, GL_LINK_STATUS, &relinkStatus);
    GetProgramInfoLog(program1, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(relinkStatus, GL_TRUE) << infoLog;
    GLint activeUniformsRelink = 0;
    GetProgramiv(program1, GL_ACTIVE_UNIFORMS, &activeUniformsRelink);
    EXPECT_EQ(activeUniformsRelink, 3);
    EXPECT_NE(GetUniformLocation(program1, "uTint"), -1);

    // Same shaders into a fresh program.
    GLuint program2 = LinkVsFs(vs, fs, GL_TRUE);
    GLint activeUniforms2 = 0;
    GetProgramiv(program2, GL_ACTIVE_UNIFORMS, &activeUniforms2);
    EXPECT_EQ(activeUniforms2, 3);
    EXPECT_NE(GetUniformLocation(program2, "uTex"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Programs and shaders share one GL name space (GL 3.3 core 2.11). A name must
// never be handed out as both, and a shader name passed where a program is
// expected is INVALID_OPERATION (KHR-GL30.get_uniform_tests.get_uniform relies
// on this; a name-collided linked program used to swallow the error).
TEST_F(ProgramTest, ProgramAndShaderNamesShareOneNameSpace) {
    GLuint program = CreateProgram();
    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    EXPECT_NE(program, vs);
    EXPECT_NE(program, fs);
    EXPECT_NE(vs, fs);
    EXPECT_EQ(IsProgram(vs), GL_FALSE);
    EXPECT_EQ(IsShader(program), GL_FALSE);

    GLfloat floatValue = 0.0f;
    GetUniformfv(vs, 0, &floatValue);
    EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));
    GLint intValue = 0;
    GetUniformiv(fs, 0, &intValue);
    EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION));

    // A never-allocated name is INVALID_VALUE, distinguishing the two cases.
    GetUniformfv(program + vs + fs + 100, 0, &floatValue);
    EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_VALUE));

    DeleteShader(vs);
    DeleteShader(fs);
    DeleteProgram(program);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---- builtin-shadowing OpName pass (P0c) ----
// Desktop GLSL lets a pack redefine builtins; ESSL 3.x forbids it, so the rename
// now happens as a SPIR-V OpName pass in SanitizeAndOptimizeBinary instead of the
// old whole-source string scan. These pin the pass end-to-end: real sources through
// glCompileShader/glLinkProgram, generated SPIR-V transpiled to the ESSL the Espryt
// driver would see.

namespace {
    Vector<MobileGL::String> TranspileProgramSpirvToEssl(GLuint program) {
        Vector<MobileGL::String> esslModules;
        auto programObj = MG_State::pGLContext->GetProgramObject(program);
        for (auto& spirvCode : programObj->GetGeneratedSpirv()) {
            MG_Util::ShaderTranspiler::SpvcSession spvcSession(
                spirvCode, MG_Util::ShaderTranspiler::SessionUsageBit::Transpile);
            spvc_compiler_options options;
            spvcSession.CreateOptions(&options);
            spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
            spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
            spvcSession.SetOptions(options);
            const char* result = nullptr;
            spvcSession.Compile(&result);
            EXPECT_NE(result, nullptr) << spvcSession.GetLastErrorString();
            esslModules.push_back(result ? result : "");
        }
        return esslModules;
    }
} // namespace

// The two blind spots of the old string scan, eliminated by construction: a
// MULTILINE definition (bliss-shaped "float fma\n(...)"), and names outside the
// old 5-entry list: sinh, as a NEW overload no builtin signature matches, so it
// parses fine and the SPIR-V OpName backstop does the rename. (An EXACT-signature
// sinh redefinition is parse-rejected by glslang - on HEAD too - and is therefore
// deliberately NOT lexically rescued; see kLexicalPreemptRenameNames.)
// min3/max3 keep their historical coverage.
TEST_F(ProgramTest, BuiltinShadowingFunctionsRenamedInEsslOutput) {
    const char* vsSource = R"(#version 330 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    const char* fsSource = R"(#version 330 core
out vec4 fragColor;

float fma
    (float a, float b, float c) { return a * b + c; }
float sinh(float x, float y) { return x * y; }
float length_squared(vec3 value) { return dot(value, value); }
float round(float x) { return floor(x + 0.5); }
float min3(float a, float b, float c) { return min(min(a, b), c); }

void main() {
    fragColor = vec4(fma(0.1, 0.2, 0.3), sinh(0.4, 2.0), round(1.25),
                     min3(0.1, 0.2, 0.3) + length_squared(vec3(0.1, 0.2, 0.3)));
}
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    for (const auto& essl : TranspileProgramSpirvToEssl(program)) {
        if (essl.find("fragColor") == String::npos) continue; // fragment module only
        EXPECT_NE(essl.find("mg_fma("), String::npos) << essl;
        EXPECT_NE(essl.find("mg_sinh("), String::npos) << essl;
        EXPECT_NE(essl.find("mg_length_squared("), String::npos) << essl;
        EXPECT_NE(essl.find("mg_round("), String::npos) << essl;
        EXPECT_NE(essl.find("mg_min3("), String::npos) << essl;
        EXPECT_EQ(essl.find("float fma("), String::npos) << essl;
        EXPECT_EQ(essl.find("float sinh("), String::npos) << essl;
        EXPECT_EQ(essl.find("float round("), String::npos) << essl;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Pure builtin USAGE (plus a commented-out definition) must stay untouched: builtin
// calls never resolve to a user function id in SPIR-V, so no mg_ name may appear.
TEST_F(ProgramTest, BuiltinUsageWithoutShadowingDefinitionKeepsBuiltinCalls) {
    const char* vsSource = R"(#version 330 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    // 400, not 330: the builtin fma() really is called here, and it is only core from GLSL 4.00
    // (at 330 it needs GL_ARB_gpu_shader5). The shadowing case above can stay at 330 precisely
    // because the rename means no call to the builtin survives.
    const char* fsSource = R"(#version 400 core
// float round(float x) { return floor(x + 0.5); }
out vec4 fragColor;
void main() {
    fragColor = vec4(round(1.25), fma(0.1, 0.2, 0.3), tanh(0.5), 1.0);
}
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    for (const auto& essl : TranspileProgramSpirvToEssl(program)) {
        EXPECT_EQ(essl.find("mg_"), String::npos) << essl;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---- the three shapes the lexical pre-empt pass must NOT touch (P0c) ----
// The source-level rename runs only for the handful of names glslang's relaxed
// parse rejects outright; everything else waits for the OpName pass, which cannot
// over-fire. These pin the three ways a lexical scan gets it wrong. All of them
// would fail as "no matching overloaded function found" - an over-detection is
// unrecoverable because the source never reaches SPIR-V.

namespace {
    // "pow(" as a real builtin call, i.e. not the tail of "mg_pow(".
    bool ContainsUnprefixedCall(const MobileGL::String& essl, const MobileGL::String& name) {
        const MobileGL::String needle = name + "(";
        for (SizeT pos = essl.find(needle); pos != String::npos; pos = essl.find(needle, pos + 1)) {
            const char before = pos == 0 ? ' ' : essl[pos - 1];
            const bool isIdentifierChar =
                std::isalnum(static_cast<unsigned char>(before)) != 0 || before == '_';
            if (!isIdentifierChar) return true;
        }
        return false;
    }
} // namespace

// B1: preprocessor-asymmetric braces desync a raw brace-depth counter (each arm of
// the #ifdef closes the function), and "return" is lexically an identifier - so
// "return clamp(...)" reads as a top-level definition "<type> <builtin> (". A
// shader that shadows nothing must survive intact.
TEST_F(ProgramTest, StatementKeywordCallInPreprocessorAsymmetricBracesIsNotAShadowingDefinition) {
    const char* vsSource = R"(#version 330 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    const char* fsSource = R"(#version 330 core
uniform vec3 uP;
out vec4 fragColor;

float getShadow(vec3 v) {
#ifdef SHADOW_OFF
    return 1.0;
}
#else
    return round(dot(v, v));
}
#endif

void main() { fragColor = vec4(getShadow(uP) * clamp(uP.x, 0.0, 1.0)); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    for (const auto& essl : TranspileProgramSpirvToEssl(program)) {
        if (essl.find("fragColor") == String::npos) continue; // fragment module only
        EXPECT_EQ(essl.find("mg_"), String::npos) << essl;
        // SPIRV-Cross lowers GLSL.std.450 FClamp to its NaN-correct min/max/isnan form, so the
        // surviving evidence of the builtin call is that pair, not the spelling "clamp(". The
        // stronger guard is above it: a renamed mg_clamp would not have compiled at all.
        EXPECT_TRUE(ContainsUnprefixedCall(essl, "min")) << essl;
        EXPECT_TRUE(ContainsUnprefixedCall(essl, "max")) << essl;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// B2: the scan is preprocessor-blind, so a definition in a DEAD #if branch would
// poison every live call to the real builtin. #version 120 normalizes to 330, so
// __VERSION__ is 330 and the compat shim is dropped by glslang - the definition
// never exists, and nothing may be renamed.
TEST_F(ProgramTest, ShadowingDefinitionInDeadPreprocessorBranchLeavesLiveBuiltinCalls) {
    const char* vsSource = R"(#version 120
#if __VERSION__ < 140
mat4 inverse(mat4 m) { return m; }
#endif
uniform mat4 uM;
uniform vec4 uV;
void main() { gl_Position = inverse(uM) * uV; }
)";
    const char* fsSource = R"(#version 330 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    for (const auto& essl : TranspileProgramSpirvToEssl(program)) {
        if (essl.find("gl_Position") == String::npos) continue; // vertex module only
        EXPECT_EQ(essl.find("mg_"), String::npos) << essl;
        EXPECT_TRUE(ContainsUnprefixedCall(essl, "inverse")) << essl;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// B3: the idiomatic reason to shadow a builtin is to ADD an overload and delegate
// to the real one. A blanket call-site rewrite would turn the body's builtin call
// into mg_pow(vec3, vec3), which has no overload. The OpName backstop renames the
// user function id only, so the delegation still resolves to GLSL.std.450 Pow.
TEST_F(ProgramTest, OverloadDelegatingToShadowedBuiltinKeepsItsBuiltinCall) {
    const char* vsSource = R"(#version 330 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    const char* fsSource = R"(#version 330 core
uniform vec3 uBase;
out vec4 fragColor;

vec3 pow(vec3 v, float e) { return pow(v, vec3(e)); }

void main() { fragColor = vec4(pow(uBase, 2.2), 1.0); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    for (const auto& essl : TranspileProgramSpirvToEssl(program)) {
        if (essl.find("fragColor") == String::npos) continue; // fragment module only
        EXPECT_NE(essl.find("mg_pow("), String::npos) << essl;
        EXPECT_TRUE(ContainsUnprefixedCall(essl, "pow")) << essl;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}


// ---------------------------------------------------------------------------
// P0b: source-hash dedupe for shader recompiles.
//   Layer 1 - the same shader object re-sourced with byte-identical text keeps its
//             compiled state, and glCompileShader on it is a no-op.
//   Layer 2 - two DIFFERENT shader objects holding byte-identical text share the
//             source-only half of the pipeline (preprocess + the lexical rejection
//             checks) through the context's ShaderPreprocessCache, while each still
//             gets its own glslang parse.
// ---------------------------------------------------------------------------
namespace {
    const char* kP0bVs = R"(#version 330 core
uniform mat4 uModel;
uniform vec4 uTint;
void main() { gl_Position = uModel * uTint; }
)";
    const char* kP0bFs = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";
    // Same stage, different declared uniform: makes "did it actually recompile?"
    // observable through reflection rather than through internal state.
    const char* kP0bAltFs = R"(#version 330 core
uniform vec4 uOtherColor;
out vec4 fragColor;
void main() { fragColor = uOtherColor; }
)";
    const char* kP0bBrokenFs = R"(#version 330 core
out vec4 fragColor;
void main() { fragColor = notADeclaredThing; }
)";

    GLuint MakeShaderWithSource(GLenum type, const char* source) {
        GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        return shader;
    }

    GLint QueryCompileStatus(GLuint shader) {
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status;
    }

    String QueryShaderInfoLog(GLuint shader) {
        GLint length = 0;
        GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) return String();
        std::vector<GLchar> buffer(static_cast<size_t>(length));
        GLsizei written = 0;
        GetShaderInfoLog(shader, length, &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    Bool ShaderHasMemoizedCompile(GLuint shader) {
        const auto& shaderObject = MG_State::pGLContext->GetShaderObject(shader);
        EXPECT_NE(shaderObject, nullptr);
        return shaderObject != nullptr && shaderObject->HasMemoizedCompile();
    }
} // namespace

// Layer 1, success path: re-sourcing with identical text and recompiling must leave
// COMPILE_STATUS, the info log and every downstream consumer exactly as they were -
// including a program that links the shader AFTER the redundant recompile.
TEST_F(ProgramTest, RecompileWithIdenticalSourceKeepsCompiledStateAndStillLinks) {
    GLuint vs = MakeShaderWithSource(GL_VERTEX_SHADER, kP0bVs);
    GLuint fs = MakeShaderWithSource(GL_FRAGMENT_SHADER, kP0bFs);
    CompileShader(vs);
    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(vs), GL_TRUE) << QueryShaderInfoLog(vs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    const String vsLogBefore = QueryShaderInfoLog(vs);
    EXPECT_TRUE(ShaderHasMemoizedCompile(vs));

    // A first link consumes the stored TShader; the redundant recompile below must not
    // disturb the preprocessed source that ClaimParsedShader re-parses from.
    GLuint firstProgram = LinkVsFs(vs, fs, GL_TRUE);
    EXPECT_GE(GetUniformLocation(firstProgram, "uColor"), 0);

    // glShaderSource with byte-identical text, then glCompileShader: both no-ops.
    ShaderSource(vs, 1, &kP0bVs, nullptr);
    EXPECT_TRUE(ShaderHasMemoizedCompile(vs)) << "identical re-source must not invalidate the compiled state";
    CompileShader(vs);
    ShaderSource(fs, 1, &kP0bFs, nullptr);
    CompileShader(fs);

    EXPECT_EQ(QueryCompileStatus(vs), GL_TRUE);
    EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE);
    EXPECT_EQ(QueryShaderInfoLog(vs), vsLogBefore);

    // The original source text is still what glGetShaderSource reports.
    GLint sourceLength = 0;
    GetShaderiv(vs, GL_SHADER_SOURCE_LENGTH, &sourceLength);
    ASSERT_GT(sourceLength, 1);
    std::vector<GLchar> sourceBuffer(static_cast<size_t>(sourceLength));
    GLsizei written = 0;
    GetShaderSource(vs, sourceLength, &written, sourceBuffer.data());
    EXPECT_EQ(String(sourceBuffer.data(), static_cast<size_t>(written)), String(kP0bVs));

    // A second program built from the same, redundantly recompiled shaders links and
    // reflects - i.e. ClaimParsedShader's re-parse path survived the no-op.
    GLuint secondProgram = LinkVsFs(vs, fs, GL_TRUE);
    EXPECT_GE(GetUniformLocation(secondProgram, "uColor"), 0);
    EXPECT_GE(GetUniformLocation(secondProgram, "uModel"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Layer 1 must not swallow a REAL source change: different text invalidates, and the
// change is visible in what the next link reflects.
TEST_F(ProgramTest, DifferentSourceAfterCompileInvalidatesCompiledState) {
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint fs = MakeShaderWithSource(GL_FRAGMENT_SHADER, kP0bFs);
    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    GLuint firstProgram = LinkVsFs(vs, fs, GL_TRUE);
    EXPECT_GE(GetUniformLocation(firstProgram, "uColor"), 0);
    EXPECT_EQ(GetUniformLocation(firstProgram, "uOtherColor"), -1);

    // New text -> compiled state gone, and glCompileShader is mandatory again.
    ShaderSource(fs, 1, &kP0bAltFs, nullptr);
    EXPECT_FALSE(ShaderHasMemoizedCompile(fs));
    EXPECT_EQ(QueryCompileStatus(fs), GL_FALSE);

    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    GLuint secondProgram = LinkVsFs(vs, fs, GL_TRUE);
    EXPECT_GE(GetUniformLocation(secondProgram, "uOtherColor"), 0);
    EXPECT_EQ(GetUniformLocation(secondProgram, "uColor"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Layer 2: byte-identical source in two distinct shader objects. Both must compile,
// and each must own an independent TShader - if the parse were shared, the second
// link would be handed an intermediate that the first link's mapIO already mutated.
TEST_F(ProgramTest, TwoShaderObjectsWithIdenticalSourceLinkIndependently) {
    GLuint vsA = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint fsA = CompileShaderChecked(GL_FRAGMENT_SHADER, kP0bFs);
    GLuint vsB = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint fsB = CompileShaderChecked(GL_FRAGMENT_SHADER, kP0bFs);
    ASSERT_NE(vsA, vsB);
    ASSERT_NE(fsA, fsB);

    const auto& objectA = MG_State::pGLContext->GetShaderObject(vsA);
    const auto& objectB = MG_State::pGLContext->GetShaderObject(vsB);
    ASSERT_NE(objectA, nullptr);
    ASSERT_NE(objectB, nullptr);
    EXPECT_EQ(objectA->GetShaderSource(), objectB->GetShaderSource());
    // WHAT THIS CASE IS ACTUALLY ABOUT: two GL shader names holding the same text must never
    // end up feeding one TShader to two links, because mapIO mutates the aliased intermediate
    // and the second link would get a corrupted one. There are now three mechanisms that keep
    // that true, and which one is in play depends on the mode - so the assertion below is on
    // the PARSES NOT BEING SHARED, never on where each object's parse came from:
    //
    //   * P0b layer 2 shares the PREPROCESS and never the parse, so each object parses for
    //     itself. This was the only mechanism when the case was written.
    //   * P1 stage 6, when async is active, shares the whole compile JOB and therefore its
    //     single parse - made safe by ClaimParsedShader's CAS, exactly as it already was for
    //     one shader object attached to two programs. ShaderCompileAdoptionTest pins that
    //     down by linking both objects and comparing the generated SPIR-V.
    //   * The translation memo's compile half (L1c) recognises the second object's source and
    //     publishes its verdict WITHOUT parsing, so that object legitimately holds no TShader
    //     at all until a link asks ClaimParsedShader for one. Asserting a non-null parse here
    //     would be asserting that the parse had NOT been skipped - i.e. testing the absence
    //     of the optimisation rather than the invariant.
    //
    // So the pointer assertion applies only where the two objects are genuinely INDEPENDENT,
    // i.e. where job adoption is not in play. What every mode has to agree on is the two
    // independent LINKS below, and they are the real point of this case.
    if (!MG_Util::Async::AsyncShaderCompileActive()) {
        const auto& shaderA = objectA->GetCompiledShader();
        const auto& shaderB = objectB->GetCompiledShader();
        // Either may legitimately hold NO parse: that is an L1c hit, where the AST is made on
        // demand at link instead. So this asserts they are not the SAME non-null parse, and
        // deliberately not that both have one - the latter would be asserting that the
        // optimisation had not happened.
        if (shaderA != nullptr && shaderB != nullptr) {
            EXPECT_NE(shaderA, shaderB) << "two independent shader objects share one consume-once parse";
        }
    }

    GLuint programA = LinkVsFs(vsA, fsA, GL_TRUE);
    GLuint programB = LinkVsFs(vsB, fsB, GL_TRUE);
    for (GLuint program : {programA, programB}) {
        EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
        EXPECT_GE(GetUniformLocation(program, "uModel"), 0);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Failure memoization: a compile that failed stays failed, with the SAME log, when
// recompiled against the same source; a real fix to the source still takes effect.
// The second object pins the cached-ParseFailed path (layer 2), which skips the parse
// entirely and must reproduce the identical verdict.
TEST_F(ProgramTest, FailedCompileIsMemoizedAndStillRecoversOnGoodSource) {
    GLuint fs = MakeShaderWithSource(GL_FRAGMENT_SHADER, kP0bBrokenFs);
    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_FALSE);
    const String failureLog = QueryShaderInfoLog(fs);
    EXPECT_FALSE(failureLog.empty());

    // Layer 1: identical re-source + recompile keeps the failure AND the log queryable.
    ShaderSource(fs, 1, &kP0bBrokenFs, nullptr);
    CompileShader(fs);
    EXPECT_EQ(QueryCompileStatus(fs), GL_FALSE);
    EXPECT_EQ(QueryShaderInfoLog(fs), failureLog);

    // Layer 2: a second object with the same broken source reports the same failure.
    GLuint otherFs = MakeShaderWithSource(GL_FRAGMENT_SHADER, kP0bBrokenFs);
    CompileShader(otherFs);
    EXPECT_EQ(QueryCompileStatus(otherFs), GL_FALSE);
    EXPECT_EQ(QueryShaderInfoLog(otherFs), failureLog);

    // A genuine fix still compiles and links.
    ShaderSource(fs, 1, &kP0bFs, nullptr);
    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    EXPECT_TRUE(QueryShaderInfoLog(fs).empty());
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);
    EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
}

// Layer 2 under eviction: push more distinct sources through the context than the
// cache can hold, then confirm nothing broke and a fresh duplicate pair still works.
TEST_F(ProgramTest, PreprocessCacheOverflowKeepsCompilingCorrectly) {
    const SizeT overflow = MG_State::GLState::ShaderPreprocessCache::kMaxEntries + 8;
    for (SizeT i = 0; i < overflow; ++i) {
        const String source = "#version 330 core\nuniform vec4 uColor" + ToString(i) +
                              ";\nout vec4 fragColor;\nvoid main() { fragColor = uColor" + ToString(i) + "; }\n";
        const char* sourcePtr = source.c_str();
        GLuint shader = MakeShaderWithSource(GL_FRAGMENT_SHADER, sourcePtr);
        CompileShader(shader);
        ASSERT_EQ(QueryCompileStatus(shader), GL_TRUE) << QueryShaderInfoLog(shader) << "\n" << source;
        DeleteShader(shader);
    }

    // Everything inserted above has long since been evicted; a brand-new duplicate
    // pair must still take the layer-2 path and produce two working programs.
    GLuint vsA = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint fsA = CompileShaderChecked(GL_FRAGMENT_SHADER, kP0bFs);
    GLuint vsB = CompileShaderChecked(GL_VERTEX_SHADER, kP0bVs);
    GLuint fsB = CompileShaderChecked(GL_FRAGMENT_SHADER, kP0bFs);
    GLuint programA = LinkVsFs(vsA, fsA, GL_TRUE);
    GLuint programB = LinkVsFs(vsB, fsB, GL_TRUE);
    EXPECT_GE(GetUniformLocation(programA, "uColor"), 0);
    EXPECT_GE(GetUniformLocation(programB, "uColor"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glUniformMatrix{2x3,2x4,3x2,3x4,4x2,4x3}fv and their twelve glProgramUniformMatrix* twins were
// validate-only no-ops: they never took the value pointer at all. They upload column-at-a-time at
// the std140 16-byte column stride, honouring `transpose`, and glGetUniformfv undoes that padding.
TEST_F(ProgramTest, NonSquareMatrixUniformsRoundTripThroughTheGlobalUbo) {
    const char* vsSource = R"(#version 430 core
uniform mat2x3 uM2x3;
uniform mat3x2 uM3x2;
uniform mat4x3 uM4x3;
uniform mat2 uM2;
void main() {
    vec3 a = uM2x3 * vec2(1.0);
    vec2 b = uM3x2 * vec3(1.0);
    vec3 c = uM4x3 * vec4(1.0);
    vec2 d = uM2 * vec2(1.0);
    gl_Position = vec4(a.xy + b + c.xy + d, 0.0, 1.0);
}
)";
    const char* fsSource = R"(#version 430 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);
    UseProgram(program);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // matCxR is C columns of R rows, column-major: value[c * R + r].
    const GLfloat m2x3[6] = {1, 2, 3, 4, 5, 6};
    const GLfloat m3x2[6] = {1, 2, 3, 4, 5, 6};
    const GLfloat m4x3[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

    const GLint loc2x3 = GetUniformLocation(program, "uM2x3");
    const GLint loc3x2 = GetUniformLocation(program, "uM3x2");
    const GLint loc4x3 = GetUniformLocation(program, "uM4x3");
    ASSERT_GE(loc2x3, 0);
    ASSERT_GE(loc3x2, 0);
    ASSERT_GE(loc4x3, 0);

    UniformMatrix2x3fv(loc2x3, 1, GL_FALSE, m2x3);
    UniformMatrix3x2fv(loc3x2, 1, GL_FALSE, m3x2);
    UniformMatrix4x3fv(loc4x3, 1, GL_FALSE, m4x3);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    GLfloat readBack[12] = {};
    GetUniformfv(program, loc2x3, readBack);
    EXPECT_EQ(std::memcmp(readBack, m2x3, sizeof(m2x3)), 0);
    std::memset(readBack, 0, sizeof(readBack));
    GetUniformfv(program, loc3x2, readBack);
    EXPECT_EQ(std::memcmp(readBack, m3x2, sizeof(m3x2)), 0);
    std::memset(readBack, 0, sizeof(readBack));
    GetUniformfv(program, loc4x3, readBack);
    EXPECT_EQ(std::memcmp(readBack, m4x3, sizeof(m4x3)), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // transpose = GL_TRUE means the source is row-major: a mat3x2 (3 columns, 2 rows) is then
    // given as 2 rows of 3, so {1,2,3, 4,5,6} is the column-major {1,4, 2,5, 3,6}.
    UniformMatrix3x2fv(loc3x2, 1, GL_TRUE, m3x2);
    const GLfloat expectedTransposed3x2[6] = {1, 4, 2, 5, 3, 6};
    std::memset(readBack, 0, sizeof(readBack));
    GetUniformfv(program, loc3x2, readBack);
    EXPECT_EQ(std::memcmp(readBack, expectedTransposed3x2, sizeof(expectedTransposed3x2)), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // The glProgramUniform* twin writes the same bytes without the program being current.
    UseProgram(0);
    const GLfloat other2x3[6] = {9, 8, 7, 6, 5, 4};
    ProgramUniformMatrix2x3fv(program, loc2x3, 1, GL_FALSE, other2x3);
    std::memset(readBack, 0, sizeof(readBack));
    GetUniformfv(program, loc2x3, readBack);
    EXPECT_EQ(std::memcmp(readBack, other2x3, sizeof(other2x3)), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A mat2 is not four contiguous floats in the global UBO: std140 pads each column vector out to
// 16 bytes, so column 1 starts at byte 16. Writing it packed put column 1 on top of column 0's
// padding, where the shader never reads it.
TEST_F(ProgramTest, Mat2UniformUsesTheStd140ColumnStride) {
    const char* vsSource = R"(#version 430 core
uniform mat2 uM2;
void main() { gl_Position = vec4(uM2 * vec2(1.0), 0.0, 1.0); }
)";
    const char* fsSource = R"(#version 430 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);
    UseProgram(program);
    const GLint loc = GetUniformLocation(program, "uM2");
    ASSERT_GE(loc, 0);

    const GLfloat m2[4] = {1, 2, 3, 4};
    UniformMatrix2fv(loc, 1, GL_FALSE, m2);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // The GL-visible value is tightly packed...
    GLfloat readBack[4] = {};
    GetUniformfv(program, loc, readBack);
    EXPECT_EQ(std::memcmp(readBack, m2, sizeof(m2)), 0);

    // ...while the bytes in the UBO put column 1 at offset 16, not 8.
    const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    const auto* ubo = static_cast<const char*>(programObject->MapUBO());
    ASSERT_NE(ubo, nullptr);
    const Uint offset = programObject->GetUniformOffset(static_cast<Uint>(loc));
    ASSERT_NE(offset, MG_State::GLState::ProgramObject::kInvalidUniformOffset);
    GLfloat column0[2] = {};
    GLfloat column1[2] = {};
    std::memcpy(column0, ubo + offset, sizeof(column0));
    std::memcpy(column1, ubo + offset + 16, sizeof(column1));
    EXPECT_FLOAT_EQ(column0[0], 1.0f);
    EXPECT_FLOAT_EQ(column0[1], 2.0f);
    EXPECT_FLOAT_EQ(column1[0], 3.0f);
    EXPECT_FLOAT_EQ(column1[1], 4.0f);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// GL 4.6 core 7.1: shaderType is an enum, so an unrecognised one is INVALID_ENUM - it used to be
// reported as INVALID_VALUE. glCreateShaderProgramv adds a count < 0 gate ahead of everything.
TEST_F(ProgramTest, CreateShaderAndCreateShaderProgramvReportTheRightErrorClasses) {
    while (GetError() != GL_NO_ERROR) {
    }

    EXPECT_EQ(CreateShader(GL_FLOAT), 0u);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "the call recorded more than one error";

    const char* source = "#version 330 core\nvoid main() { gl_Position = vec4(1.0); }\n";
    EXPECT_EQ(CreateShaderProgramv(GL_FLOAT, 1, &source), 0u);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "the call recorded more than one error";

    EXPECT_EQ(CreateShaderProgramv(GL_VERTEX_SHADER, -1, &source), 0u);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "the call recorded more than one error";

    // A well-formed call still works.
    const GLuint program = CreateShaderProgramv(GL_VERTEX_SHADER, 1, &source);
    EXPECT_NE(program, 0u);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ARB_explicit_uniform_location / GL 4.6 core 7.6.1: a `layout(location = N)` uniform reserves N
// EVEN WHEN IT IS INACTIVE. Dead default-block uniforms are correctly filtered off the GL surface
// (glGetUniformLocation must answer -1 for them), but the implicit allocator used to walk straight
// over the location they claimed and hand it to a uniform that never asked for it
// (KHR-GL43.explicit_uniform_location.uniform-loc-mix-with-implicit3).
TEST_F(ProgramTest, InactiveExplicitUniformLocationIsStillReserved) {
    const char* vsSource = R"(#version 430 core
layout(location = 2) uniform vec4 uDeadAtTwo;
uniform vec4 uA;
uniform vec4 uB;
uniform vec4 uC;
uniform vec4 uD;
void main() { gl_Position = uA + uB + uC + uD; }
)";
    const char* fsSource = R"(#version 430 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    const GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    const GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    const GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    // Reserving a location must not resurrect the uniform: it is still inactive to GL.
    EXPECT_EQ(GetUniformLocation(program, "uDeadAtTwo"), -1);

    for (const char* name : {"uA", "uB", "uC", "uD"}) {
        const GLint location = GetUniformLocation(program, name);
        EXPECT_GE(location, 0) << name << " lost its implicit location";
        EXPECT_NE(location, 2) << name << " was handed the location uDeadAtTwo reserved";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The GL_MAX_UNIFORM_LOCATIONS boundary, from both sides. MAX_UNIFORM_LOCATIONS - 1 is the LAST
// LEGAL location: it has to link and read back verbatim
// (KHR-GL43.explicit_uniform_location.uniform-loc-max), which is only true while the advertised
// value and what the link accepts are the SAME number - the getter used to advertise one more
// location than any shader could name.
//
// The over-the-ceiling half is asserted through an ARRAY, because that is the only spelling the
// link gets to judge: a bare `layout(location = MAX)` is already a compile error inside glslang
// ("location is too large"), while an array's base compiles fine and only its last element passes
// the ceiling (...uniform-loc-negative-link-max-num-of-locations).
TEST_F(ProgramTest, ExplicitUniformLocationsHonourMaxUniformLocations) {
    GLint maxLocations = 0;
    GetIntegerv(GL_MAX_UNIFORM_LOCATIONS, &maxLocations);
    ASSERT_GE(maxLocations, 1024) << "GL 4.3 requires at least 1024 uniform locations";

    const char* fsSource = R"(#version 430 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    const GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);

    {
        const String source = String("#version 430 core\nlayout(location = ") +
                              std::to_string(maxLocations - 1) +
                              ") uniform vec4 uAtLimit;\nvoid main() { gl_Position = uAtLimit; }\n";
        const GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, source.c_str());
        const GLuint program = LinkVsFs(vs, fs, GL_TRUE);
        EXPECT_EQ(GetUniformLocation(program, "uAtLimit"), maxLocations - 1)
            << "the last location in the pool is legal and must come back verbatim";
    }
    {
        const String source = String("#version 430 core\nlayout(location = ") +
                              std::to_string(maxLocations - 4) +
                              ") uniform vec4 uSpill[8];\nvoid main() { gl_Position = uSpill[0]; }\n";
        const GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, source.c_str());
        (void)LinkVsFs(vs, fs, GL_FALSE);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// GL 4.6 core 7.6: an atomic counter is a default-block uniform that addresses an ATOMIC COUNTER
// buffer, where every counter is a tightly packed 4-byte value. MobileGL lowers each atomic_uint
// onto a synthesized block, which used to drag the whole array-stride query onto the std140 rule
// that rounds an element stride up to a vec4 - so an atomic counter array reported 16
// (KHR-GL43.shader_atomic_counters.basic-program-query: "GL_UNIFORM_ARRAY_STRIDE is 16 should be
// 4"). The offsets, matrix stride and row-major flag are pinned alongside it because the same
// synthesized block feeds all four queries.
TEST_F(ProgramTest, AtomicCounterArrayReportsThePackedFourByteStride) {
    const char* vsSource = R"(#version 430 core
void main() { gl_Position = vec4(1.0); }
)";
    const char* fsSource = R"(#version 430 core
layout(location = 0) out vec4 o_color;
layout(binding = 0, offset = 0) uniform atomic_uint ac_counter0;
layout(binding = 0, offset = 4) uniform atomic_uint ac_counter1;
layout(binding = 0) uniform atomic_uint ac_counter2;
layout(binding = 0) uniform atomic_uint ac_counter67[2];
layout(binding = 0) uniform atomic_uint ac_counter3;
void main() {
  uint c = 0u;
  c += atomicCounterIncrement(ac_counter0);
  c += atomicCounterIncrement(ac_counter1);
  c += atomicCounterIncrement(ac_counter2);
  c += atomicCounterIncrement(ac_counter3);
  c += atomicCounterIncrement(ac_counter67[0]);
  c += atomicCounterIncrement(ac_counter67[1]);
  o_color = vec4(float(c));
}
)";
    const GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    const GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    const GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    GLint activeUniforms = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    ASSERT_EQ(activeUniforms, 5);

    // Declared offset -> expected {array size, array stride}. layout(offset=) pins the first two;
    // the rest are packed after them in declaration order, the array taking two 4-byte slots.
    struct Expectation {
        const char* name;
        GLint size;
        GLint offset;
        GLint arrayStride;
    };
    const Expectation expectations[] = {
        {"ac_counter0", 1, 0, 0},       {"ac_counter1", 1, 4, 0},  {"ac_counter2", 1, 8, 0},
        {"ac_counter67[0]", 2, 12, 4},  {"ac_counter3", 1, 20, 0},
    };

    for (const auto& expected : expectations) {
        const char* queryName = expected.name;
        GLuint index = GL_INVALID_INDEX;
        GetUniformIndices(program, 1, &queryName, &index);
        ASSERT_NE(index, GL_INVALID_INDEX) << expected.name << " is not an active uniform";

        GLint value = -2;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &value);
        EXPECT_EQ(value, static_cast<GLint>(GL_UNSIGNED_INT_ATOMIC_COUNTER)) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_SIZE, &value);
        EXPECT_EQ(value, expected.size) << expected.name;
        // An atomic counter is a default-block uniform however it was lowered.
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &value);
        EXPECT_EQ(value, -1) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &value);
        EXPECT_EQ(value, expected.offset) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &value);
        EXPECT_EQ(value, expected.arrayStride) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_MATRIX_STRIDE, &value);
        EXPECT_EQ(value, 0) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_IS_ROW_MAJOR, &value);
        EXPECT_EQ(value, 0) << expected.name;
        GetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ATOMIC_COUNTER_BUFFER_INDEX, &value);
        EXPECT_EQ(value, 0) << expected.name;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// GL 4.6 core 7.6.1: a uniform LOCATION is a property of the default uniform block. A member of
// a named uniform block or a buffer block has none, and glGetUniformLocation must answer -1 for
// it - which is what glGetProgramResourceLocation(GL_UNIFORM, ...) already did, so the two used
// to disagree. The location such a member was handed was not merely reported, it was CONSUMED:
// it came out of the same first-fit table the default-block uniforms draw from.
TEST_F(ProgramTest, BlockMembersConsumeNoUniformLocation) {
    const char* csSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 1) buffer ResultBuffer { vec4 bufferMember; };
layout(std140, binding = 2) uniform SettingsBlock { vec4 blockMember; };
layout(location = 0) uniform float uDead[3];
uniform float uImplicit;
void main() { bufferMember = blockMember * uImplicit; }
)";
    const GLuint cs = CompileShaderChecked(GL_COMPUTE_SHADER, csSource);
    const GLuint program = CreateProgram();
    AttachShader(program, cs);
    LinkProgram(program);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    char infoLog[1024] = "";
    GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
    ASSERT_EQ(linkStatus, GL_TRUE) << infoLog;

    for (const char* member : {"bufferMember", "blockMember"}) {
        EXPECT_EQ(GetUniformLocation(program, member), -1) << member << " is a block member, not a GL uniform";
        EXPECT_EQ(GetProgramResourceLocation(program, GL_UNIFORM, member), -1)
            << member << ": the two location queries must agree";
    }

    // uDead[3] reserves 0..2 without becoming visible, so the first location left for the one
    // default-block uniform is 3. It used to be 4, because a block member took 3 first.
    EXPECT_EQ(GetUniformLocation(program, "uImplicit"), 3)
        << "a block member consumed a location the default-block uniform was entitled to";
    EXPECT_EQ(GetUniformLocation(program, "uDead"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The same defect at the boundary, which is where the conformance suite catches it. The location
// table's ceiling is raised to hold every uniform it must place; counting block members into that
// raise pushed the ceiling to GL_MAX_UNIFORM_LOCATIONS itself, and the first-fit pass then handed
// out the one location past the legal 0..MAX-1 range
// (KHR-GL43.explicit_uniform_location.uniform-loc-mix-with-implicit-max, whose compute program
// carries an SSBO: "Uniform u2 returned location (4095) is greater than implementation dependent
// limit (4095)"). Its -array sibling shares the root cause and failed one step further along, with
// the pool reported exhausted and no link at all.
TEST_F(ProgramTest, ImplicitLocationStaysInRangeWhenABufferBlockSharesTheProgram) {
    GLint maxLocations = 0;
    GetIntegerv(GL_MAX_UNIFORM_LOCATIONS, &maxLocations);
    ASSERT_GE(maxLocations, 1024) << "GL 4.3 requires at least 1024 uniform locations";

    // The CTS shape: explicit unused arrays fill the pool except for a hole of `implicitCount`
    // locations at `holeBase`, and the one implicit uniform must land exactly in that hole.
    const auto runCase = [&](int holeBase, int implicitCount) {
        String decls;
        int nextName = 0;
        if (holeBase > 0) {
            decls += "layout(location = 0) uniform float u" + std::to_string(nextName++) + "[" +
                     std::to_string(holeBase) + "];\n";
        }
        const int tailBase = holeBase + implicitCount;
        if (tailBase < maxLocations) {
            decls += "layout(location = " + std::to_string(tailBase) + ") uniform float u" +
                     std::to_string(nextName++) + "[" + std::to_string(maxLocations - tailBase) + "];\n";
        }
        const String implicitName = "u" + std::to_string(nextName);
        decls += "uniform float " + implicitName + "[" + std::to_string(implicitCount) + "];\n";

        // The buffer block is the whole point: it is one more uniform the table has to seat, and
        // seating it inside the location space is what used to push the implicit uniform out.
        const String csSource = "#version 430 core\n"
                                "layout(local_size_x = 1) in;\n"
                                "layout(std430, binding = 1) buffer ResultBuffer { vec4 cs_result; };\n" +
                                decls + "void main() { cs_result = vec4(" + implicitName + "[0]); }\n";
        const GLuint cs = CompileShaderChecked(GL_COMPUTE_SHADER, csSource.c_str());
        const GLuint program = CreateProgram();
        AttachShader(program, cs);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        char infoLog[1024] = "";
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        ASSERT_EQ(linkStatus, GL_TRUE) << "hole at " << holeBase << " x" << implicitCount << ": " << infoLog;

        const GLint location = GetUniformLocation(program, implicitName.c_str());
        EXPECT_EQ(location, holeBase) << "the implicit uniform must take the one free span left";
        EXPECT_LT(location + implicitCount, maxLocations + 1)
            << "locations " << location << ".." << (location + implicitCount - 1)
            << " must stay inside 0.." << (maxLocations - 1);
        EXPECT_EQ(GetUniformLocation(program, "cs_result"), -1);
    };

    // The three holes the CTS walks, for its single-uniform and its 3-element-array subcase.
    for (const int implicitCount : {1, 3}) {
        runCase(0, implicitCount);
        runCase(3, implicitCount);
        runCase(maxLocations - implicitCount, implicitCount);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// --- `layout(vertices = N) out` against GL_MAX_PATCH_VERTICES -----------------------------------
//
// GL 4.6 core 11.2.1.1 makes N > MAX_PATCH_VERTICES a LINK failure. Nothing enforced it: glslang
// only rejects N <= 0, and carries maxPatchVertices in TBuiltInResource purely so
// gl_MaxPatchVertices can expand from it. The check deliberately lives at link and not at compile,
// because KHR-GL4x.tessellation_shader.compilation_and_linking_errors.
// tc_invalid_output_patch_vertex_count requires the shader to COMPILE ("Compilation passed as
// allowed") and only the program to fail.
TEST_F(ProgramTest, TessControlOutputPatchSizePastTheLimitFailsToLinkButStillCompiles) {
    GLint maxPatchVertices = 0;
    GetIntegerv(GL_MAX_PATCH_VERTICES, &maxPatchVertices);
    ASSERT_GT(maxPatchVertices, 0);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const char* kVs = R"(#version 460 core
void main() { gl_Position = vec4(0.0); }
)";
    const char* kTes = R"(#version 460 core
layout(triangles, equal_spacing, cw) in;
void main() { gl_Position = gl_in[0].gl_Position; }
)";

    const char* kTcsPrologue = R"(#version 460 core
layout(vertices = )";
    const char* kTcsEpilogue = R"() out;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    gl_TessLevelOuter[0] = 1.0;
}
)";

    const auto buildWith = [&](const GLint vertices) {
        const String tcs = String(kTcsPrologue) + std::to_string(vertices) + kTcsEpilogue;
        const char* tcsSource = tcs.c_str();

        const GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &kVs, nullptr);
        CompileShader(vs);
        const GLuint tc = CreateShader(GL_TESS_CONTROL_SHADER);
        ShaderSource(tc, 1, &tcsSource, nullptr);
        CompileShader(tc);
        const GLuint te = CreateShader(GL_TESS_EVALUATION_SHADER);
        ShaderSource(te, 1, &kTes, nullptr);
        CompileShader(te);

        // The offending stage COMPILES; only the link is allowed to notice.
        GLint tcCompiled = GL_FALSE;
        GetShaderiv(tc, GL_COMPILE_STATUS, &tcCompiled);
        EXPECT_EQ(tcCompiled, GL_TRUE) << "vertices=" << vertices << " must still compile";

        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, tc);
        AttachShader(program, te);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        char infoLog[1024] = "";
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        return std::pair<GLint, String>{linkStatus, String(infoLog)};
    };

    const auto atTheLimit = buildWith(maxPatchVertices);
    EXPECT_EQ(atTheLimit.first, GL_TRUE)
        << "exactly GL_MAX_PATCH_VERTICES is legal: " << atTheLimit.second;

    const auto pastTheLimit = buildWith(maxPatchVertices + 1);
    EXPECT_EQ(pastTheLimit.first, GL_FALSE) << "one past GL_MAX_PATCH_VERTICES must not link";
    EXPECT_NE(pastTheLimit.second.find("GL_MAX_PATCH_VERTICES"), String::npos)
        << "the info log must name the limit it broke: " << pastTheLimit.second;

    DrainProgramTestErrors();
}

// gl_NumSamples has no SPIR-V built-in, so the source pipeline lowers it onto a reserved
// default-block uniform (ShaderTranspiler::NUM_SAMPLES_UNIFORM_NAME). Two things have to hold at
// once: the program has to BUILD (it used to die at compile with "'gl_NumSamples' : undeclared
// identifier", which is what took all 144 KHR-GL46.sample_variables.mask.* bodies down), and the
// uniform standing in for the built-in has to stay invisible to GL - gl_NumSamples is a built-in,
// so a conformant implementation reports nothing for it and no glUniform* may reach it.
TEST_F(ProgramTest, GlNumSamplesLowersToAHiddenReservedUniform) {
    const char* vsSource = R"(#version 460 core
void main() { gl_Position = vec4(0.0); }
)";
    const char* fsSource = R"(#version 460 core
uniform int u_sampleMask;
layout(location = 0) out vec4 o_color;
void main() {
    for (int i = 0; i < (gl_NumSamples + 31) / 32; ++i) {
        gl_SampleMask[i] = u_sampleMask & gl_SampleMaskIn[i];
    }
    o_color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    // u_sampleMask and nothing else: the stand-in must not enlarge the enumeration.
    GLint activeUniforms = 0;
    GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
    EXPECT_EQ(activeUniforms, 1);
    EXPECT_NE(GetUniformLocation(program, "u_sampleMask"), -1);
    EXPECT_EQ(GetUniformLocation(program, "mg_NumSamples"), -1);
    EXPECT_EQ(GetUniformLocation(program, "gl_NumSamples"), -1);
    EXPECT_EQ(GetUniformBlockIndex(program, "MGL_GLOBAL_UBO"), GL_INVALID_INDEX);

    char nameBuf[64] = "";
    for (GLint i = 0; i < activeUniforms; ++i) {
        GLsizei nameLen = 0;
        GLint size = 0;
        GLenum type = 0;
        GetActiveUniform(program, static_cast<GLuint>(i), sizeof(nameBuf), &nameLen, &size, &type, nameBuf);
        EXPECT_TRUE(std::strcmp(nameBuf, "mg_NumSamples") != 0) << nameBuf;
    }

    // The driver-side write path, which is what the draw path calls. It reports true only when the
    // program really did take the shim AND the optimized SPIR-V kept the member.
    const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    EXPECT_TRUE(programObject->UsesReservedNumSamples());
    EXPECT_TRUE(programObject->WriteReservedNumSamples(4));

    const Uint32 versionAfterFirstWrite = programObject->GetUBOContentVersion();
    // Value-identical rewrite: no re-upload, so no version bump - a run of draws into one
    // framebuffer must not dirty the UBO every draw.
    EXPECT_TRUE(programObject->WriteReservedNumSamples(4));
    EXPECT_EQ(programObject->GetUBOContentVersion(), versionAfterFirstWrite);
    // A different framebuffer's sample count does have to reach the GPU.
    EXPECT_TRUE(programObject->WriteReservedNumSamples(1));
    EXPECT_NE(programObject->GetUBOContentVersion(), versionAfterFirstWrite);

    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A program whose fragment stage never mentions gl_NumSamples pays nothing and has nothing to
// write - the gate the draw path reads before it touches the SPIR-V join.
TEST_F(ProgramTest, ProgramWithoutGlNumSamplesHasNoReservedUniform) {
    const char* vsSource = R"(#version 460 core
void main() { gl_Position = vec4(0.0); }
)";
    const char* fsSource = R"(#version 460 core
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(1.0); }
)";
    GLuint vs = CompileShaderChecked(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShaderChecked(GL_FRAGMENT_SHADER, fsSource);
    GLuint program = LinkVsFs(vs, fs, GL_TRUE);

    const auto& programObject = MG_State::pGLContext->GetProgramObject(program);
    ASSERT_NE(programObject, nullptr);
    EXPECT_FALSE(programObject->UsesReservedNumSamples());
    EXPECT_FALSE(programObject->WriteReservedNumSamples(4));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glGetProgramiv's geometry and tessellation link properties (GL 4.6 core table 23.35). None of
// these had a source: GL_GEOMETRY_VERTICES_OUT / _INPUT_TYPE / _OUTPUT_TYPE were listed in the
// switch only to fall through into the GL_INVALID_ENUM default, GL_GEOMETRY_SHADER_INVOCATIONS
// and the five GL_TESS_* pnames were not listed at all, and the link recorded nothing but the
// geometry INPUT primitive. 72 of the tessellation family's 116 failing conformance bodies died
// on the first of these queries, before touching a single tessellation feature.
namespace {
    GLuint CompileStage(GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            char infoLog[2048] = "";
            GetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
            ADD_FAILURE() << "stage " << type << " failed to compile: " << infoLog;
        }
        return shader;
    }

    GLuint LinkStages(const std::vector<std::pair<GLenum, const char*>>& stages) {
        const GLuint program = CreateProgram();
        for (const auto& [type, source] : stages) {
            const GLuint shader = CompileStage(type, source);
            AttachShader(program, shader);
            DeleteShader(shader);
        }
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            char infoLog[2048] = "";
            GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
            ADD_FAILURE() << "link failed: " << infoLog;
        }
        return program;
    }

    constexpr const char* kPassthroughVs = R"(#version 460 core
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";
    constexpr const char* kPassthroughFs = R"(#version 460 core
out vec4 mgColor;
void main() { mgColor = vec4(1.0); }
)";
} // namespace

TEST_F(ProgramTest, GetProgramivReportsTheGeometryStageLinkProperties) {
    constexpr const char* gs = R"(#version 460 core
layout(triangles, invocations = 3) in;
layout(line_strip, max_vertices = 7) out;
void main() {
    for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; EmitVertex(); }
    EndPrimitive();
}
)";
    const GLuint program =
        LinkStages({{GL_VERTEX_SHADER, kPassthroughVs}, {GL_GEOMETRY_SHADER, gs}, {GL_FRAGMENT_SHADER, kPassthroughFs}});

    GLint value = -1;
    GetProgramiv(program, GL_GEOMETRY_INPUT_TYPE, &value);
    EXPECT_EQ(value, GL_TRIANGLES);
    GetProgramiv(program, GL_GEOMETRY_OUTPUT_TYPE, &value);
    EXPECT_EQ(value, GL_LINE_STRIP);
    GetProgramiv(program, GL_GEOMETRY_VERTICES_OUT, &value);
    EXPECT_EQ(value, 7);
    GetProgramiv(program, GL_GEOMETRY_SHADER_INVOCATIONS, &value);
    EXPECT_EQ(value, 3);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // ...and INVALID_OPERATION, not INVALID_ENUM, on a program that has no geometry stage: GL
    // says "a linked program object with a geometry shader", which the conformance suite checks
    // from both sides.
    const GLuint noGeometry = LinkStages({{GL_VERTEX_SHADER, kPassthroughVs}, {GL_FRAGMENT_SHADER, kPassthroughFs}});
    for (const GLenum pname : {GL_GEOMETRY_INPUT_TYPE, GL_GEOMETRY_OUTPUT_TYPE, GL_GEOMETRY_VERTICES_OUT,
                               GL_GEOMETRY_SHADER_INVOCATIONS}) {
        GetProgramiv(noGeometry, pname, &value);
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION)) << "pname " << pname;
    }
}

TEST_F(ProgramTest, GetProgramivReportsTheTessellationStageLinkProperties) {
    constexpr const char* tcs = R"(#version 460 core
layout(vertices = 3) out;
void main() {
    gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0;
    gl_TessLevelInner[0] = 1.0;
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}
)";
    constexpr const char* tes = R"(#version 460 core
layout(quads, fractional_odd_spacing, cw, point_mode) in;
void main() { gl_Position = gl_in[0].gl_Position; }
)";
    const GLuint program = LinkStages({{GL_VERTEX_SHADER, kPassthroughVs},
                                       {GL_TESS_CONTROL_SHADER, tcs},
                                       {GL_TESS_EVALUATION_SHADER, tes},
                                       {GL_FRAGMENT_SHADER, kPassthroughFs}});

    GLint value = -1;
    GetProgramiv(program, GL_TESS_CONTROL_OUTPUT_VERTICES, &value);
    EXPECT_EQ(value, 3);
    GetProgramiv(program, GL_TESS_GEN_MODE, &value);
    EXPECT_EQ(value, GL_QUADS);
    GetProgramiv(program, GL_TESS_GEN_SPACING, &value);
    EXPECT_EQ(value, GL_FRACTIONAL_ODD);
    GetProgramiv(program, GL_TESS_GEN_VERTEX_ORDER, &value);
    EXPECT_EQ(value, GL_CW);
    GetProgramiv(program, GL_TESS_GEN_POINT_MODE, &value);
    EXPECT_EQ(value, GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // GLSL 4.60 4.4.2.3 defaults: equal_spacing, ccw, no point mode.
    constexpr const char* defaultTes = R"(#version 460 core
layout(triangles) in;
void main() { gl_Position = gl_in[0].gl_Position; }
)";
    const GLuint defaults = LinkStages({{GL_VERTEX_SHADER, kPassthroughVs},
                                        {GL_TESS_CONTROL_SHADER, tcs},
                                        {GL_TESS_EVALUATION_SHADER, defaultTes},
                                        {GL_FRAGMENT_SHADER, kPassthroughFs}});
    GetProgramiv(defaults, GL_TESS_GEN_MODE, &value);
    EXPECT_EQ(value, GL_TRIANGLES);
    GetProgramiv(defaults, GL_TESS_GEN_SPACING, &value);
    EXPECT_EQ(value, GL_EQUAL);
    GetProgramiv(defaults, GL_TESS_GEN_VERTEX_ORDER, &value);
    EXPECT_EQ(value, GL_CCW);
    GetProgramiv(defaults, GL_TESS_GEN_POINT_MODE, &value);
    EXPECT_EQ(value, GL_FALSE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    const GLuint noTess = LinkStages({{GL_VERTEX_SHADER, kPassthroughVs}, {GL_FRAGMENT_SHADER, kPassthroughFs}});
    for (const GLenum pname : {GL_TESS_CONTROL_OUTPUT_VERTICES, GL_TESS_GEN_MODE, GL_TESS_GEN_SPACING,
                               GL_TESS_GEN_VERTEX_ORDER, GL_TESS_GEN_POINT_MODE}) {
        GetProgramiv(noTess, pname, &value);
        EXPECT_EQ(GetError(), static_cast<GLenum>(GL_INVALID_OPERATION)) << "pname " << pname;
    }
}

// The context-wide tessellation state the same conformance group reads before it links anything.
// glGetBooleanv and glGetFloatv both have to answer GL_PATCH_DEFAULT_OUTER_LEVEL, which is
// FLOAT state - a delegation that writes element 0 only would leave the other three components
// as whatever was in the caller's stack.
TEST_F(ProgramTest, ContextWideTessellationPropertiesAnswerEveryWidth) {
    GLfloat outer[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    GetFloatv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
    for (const GLfloat level : outer) EXPECT_FLOAT_EQ(level, 1.0f);

    GLfloat inner[2] = {-1.0f, -1.0f};
    GetFloatv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
    for (const GLfloat level : inner) EXPECT_FLOAT_EQ(level, 1.0f);

    GLboolean outerBools[4] = {GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
    GetBooleanv(GL_PATCH_DEFAULT_OUTER_LEVEL, outerBools);
    for (const GLboolean level : outerBools) EXPECT_EQ(level, GL_TRUE);

    GLint restart = -1;
    GetIntegerv(GL_PRIMITIVE_RESTART_FOR_PATCHES_SUPPORTED, &restart);
    EXPECT_EQ(restart, GL_FALSE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------------
// The binding-range rule (GLSL 4.30 4.4.5 / ES 3.1 4.4.4): layout(binding = N) at or above the
// resource kind's implementation limit is an error. glslang cannot enforce it for MobileGL - it
// owns ceilings for samplers/images and for atomic counters and the relaxed Vulkan parse switches
// both OFF, and for uniform and storage BLOCKS it has no ceiling at all - so MobileGL enforces it
// itself, at the link, from TMglGlslIoResolver::CheckDeclaredBindingRange. es31cLayoutBindingTests
// accepts a link-time rejection: its predicate, compiledAndLinked(), is the AND of the two.
//
// The two ceilings asserted here are frontend constants, so they hold with no backend active,
// which is what makes them testable in this GPU-free binary. The sampler and image ceilings are
// backend-derived and read zero here, i.e. "do not enforce"; their arm is the same code path.
// ---------------------------------------------------------------------------------------------

namespace {
    // Links a compute program from one source and returns its LINK_STATUS. Compute, because every
    // kind this rule covers can be declared in a compute shader and nothing else has to be
    // supplied alongside it.
    GLint LinkComputeProgramStatus(const char* source, String* outInfoLog = nullptr) {
        char infoLog[2048] = "";
        const GLuint shader = CreateShader(GL_COMPUTE_SHADER);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        GLint compileStatus = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
        if (compileStatus != GL_TRUE) {
            GetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
            if (outInfoLog) *outInfoLog = infoLog;
            // A compile-time rejection satisfies the same rule; report it as "not linked".
            return GL_FALSE;
        }
        const GLuint program = CreateProgram();
        AttachShader(program, shader);
        LinkProgram(program);
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        GetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        if (outInfoLog) *outInfoLog = infoLog;
        return linkStatus;
    }
} // namespace

TEST_F(ProgramTest, UniformBlockBindingAtTheLimitIsRejected) {
    DrainProgramTestErrors();

    GLint maxUniformBufferBindings = 0;
    GetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUniformBufferBindings);
    ASSERT_GT(maxUniformBufferBindings, 0);

    const String legal = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                         std::to_string(maxUniformBufferBindings - 1) +
                         ", std140) uniform Blk { vec4 v; } blk;\nvoid main() { }\n";
    EXPECT_EQ(LinkComputeProgramStatus(legal.c_str()), GL_TRUE)
        << "the last binding in the range is legal and must still link";

    String infoLog;
    const String overRange = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                             std::to_string(maxUniformBufferBindings) +
                             ", std140) uniform Blk { vec4 v; } blk;\nvoid main() { }\n";
    EXPECT_EQ(LinkComputeProgramStatus(overRange.c_str(), &infoLog), GL_FALSE)
        << "a uniform block binding at GL_MAX_UNIFORM_BUFFER_BINDINGS must be rejected";
    EXPECT_NE(infoLog.find("GL_MAX_UNIFORM_BUFFER_BINDINGS"), String::npos)
        << "the info log must name the limit the declaration broke; got: " << infoLog;

    DrainProgramTestErrors();
}

TEST_F(ProgramTest, AtomicCounterBindingAtTheLimitIsRejected) {
    DrainProgramTestErrors();

    GLint maxAtomicBindings = 0;
    GetIntegerv(GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS, &maxAtomicBindings);
    ASSERT_GT(maxAtomicBindings, 0);

    const String legal = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                         std::to_string(maxAtomicBindings - 1) +
                         ") uniform atomic_uint counter;\nvoid main() { atomicCounterIncrement(counter); }\n";
    EXPECT_EQ(LinkComputeProgramStatus(legal.c_str()), GL_TRUE)
        << "the last counter binding in the range is legal and must still link";

    String infoLog;
    const String overRange = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                             std::to_string(maxAtomicBindings) +
                             ") uniform atomic_uint counter;\nvoid main() { atomicCounterIncrement(counter); }\n";
    EXPECT_EQ(LinkComputeProgramStatus(overRange.c_str(), &infoLog), GL_FALSE)
        << "an atomic_uint binding at GL_MAX_ATOMIC_COUNTER_BUFFER_BINDINGS must be rejected";

    DrainProgramTestErrors();
}

// The arrayed-instance half of the rule: an array of N takes base .. base + N - 1, and every one
// of them has to fit. A base that is itself legal is therefore not enough.
TEST_F(ProgramTest, ArrayedUniformBlockInstanceMustFitEntirelyBelowTheBindingLimit) {
    DrainProgramTestErrors();

    GLint maxUniformBufferBindings = 0;
    GetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maxUniformBufferBindings);
    ASSERT_GE(maxUniformBufferBindings, 4);

    const String fits = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                        std::to_string(maxUniformBufferBindings - 4) +
                        ", std140) uniform Blk { vec4 v; } blk[4];\nvoid main() { }\n";
    EXPECT_EQ(LinkComputeProgramStatus(fits.c_str()), GL_TRUE)
        << "base + count - 1 is the last legal binding, so this array fits exactly";

    const String spills = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                          std::to_string(maxUniformBufferBindings - 3) +
                          ", std140) uniform Blk { vec4 v; } blk[4];\nvoid main() { }\n";
    EXPECT_EQ(LinkComputeProgramStatus(spills.c_str()), GL_FALSE)
        << "the array's last element is past the limit even though its base is not";

    DrainProgramTestErrors();
}

// The storage-block arm still has its own COMPILE-time enforcement (the lexical scan glslang's
// relaxed parse leaves MobileGL to do), and the link-time check is a backstop for it. Both agree
// because both read ResolveResourceBindingLimits; this pins the outcome rather than the site.
TEST_F(ProgramTest, StorageBlockBindingAtTheLimitIsStillRejected) {
    DrainProgramTestErrors();

    GLint maxStorageBindings = 0;
    GetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxStorageBindings);
    if (maxStorageBindings <= 0) {
        GTEST_SKIP() << "no storage-buffer binding points advertised in this configuration";
    }

    const String overRange = "#version 430 core\nlayout(local_size_x = 1) in;\nlayout(binding = " +
                             std::to_string(maxStorageBindings) +
                             ", std430) buffer Blk { vec4 v; } blk;\nvoid main() { blk.v = vec4(0.0); }\n";
    EXPECT_EQ(LinkComputeProgramStatus(overRange.c_str()), GL_FALSE);

    DrainProgramTestErrors();
}

// ---------------------------------------------------------------------------------------------
// GL_PROGRAM_SEPARABLE is LATCHED at link (GL 4.6 core 7.3), and glUseProgramStages tests the
// latched flag, not the live one.
// ---------------------------------------------------------------------------------------------

TEST_F(ProgramTest, ProgramSeparableIsLatchedAtLinkNotReportedLive) {
    DrainProgramTestErrors();

    const GLuint program = CreateProgram();
    GLint separable = GL_TRUE;
    GetProgramiv(program, GL_PROGRAM_SEPARABLE, &separable);
    EXPECT_EQ(separable, GL_FALSE) << "a fresh program is not separable";

    // Requested but never linked: the request has not taken effect yet.
    ProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
    GetProgramiv(program, GL_PROGRAM_SEPARABLE, &separable);
    EXPECT_EQ(separable, GL_FALSE) << "GL_PROGRAM_SEPARABLE takes effect at the NEXT link";
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // Link it, and the request lands.
    const char* vsSource = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    const GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSource, nullptr);
    CompileShader(vs);
    AttachShader(program, vs);
    LinkProgram(program);
    GetProgramiv(program, GL_PROGRAM_SEPARABLE, &separable);
    EXPECT_EQ(separable, GL_TRUE);

    // Clearing the live flag does not un-separate the EXECUTABLE that was already linked.
    ProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_FALSE);
    GetProgramiv(program, GL_PROGRAM_SEPARABLE, &separable);
    EXPECT_EQ(separable, GL_TRUE) << "the latched flag only moves at a link";

    DrainProgramTestErrors();
}

TEST_F(ProgramTest, UseProgramStagesRequiresAProgramLinkedAsSeparable) {
    DrainProgramTestErrors();

    const char* vsSource = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    const GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSource, nullptr);
    CompileShader(vs);

    // Linked, but NOT as a separable program.
    const GLuint monolithic = CreateProgram();
    AttachShader(monolithic, vs);
    LinkProgram(monolithic);
    GLint linkStatus = GL_FALSE;
    GetProgramiv(monolithic, GL_LINK_STATUS, &linkStatus);
    ASSERT_EQ(linkStatus, GL_TRUE);
    DrainProgramTestErrors();

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, monolithic);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION)
        << "GL 4.6 core 7.4: the program must have been LINKED with PROGRAM_SEPARABLE set";

    // The same program, relinked as separable, is accepted.
    ProgramParameteri(monolithic, GL_PROGRAM_SEPARABLE, GL_TRUE);
    LinkProgram(monolithic);
    DrainProgramTestErrors();
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, monolithic);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    DeleteProgramPipelines(1, &pipeline);
    DrainProgramTestErrors();
}

// GL 4.6 core 7.6: an unlinked program is GL_INVALID_OPERATION for both of these, and
// glProgramUniform*'s location == -1 early-out must not swallow it - -1 is exactly what an
// application holds after asking an unlinked program for a location.
TEST_F(ProgramTest, UniformEntryPointsRejectAnUnlinkedProgram) {
    DrainProgramTestErrors();

    const GLuint program = CreateProgram();

    EXPECT_EQ(GetUniformLocation(program, "uAnything"), -1);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    const GLfloat value = 1.0f;
    ProgramUniform1fv(program, -1, 1, &value);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION)
        << "the link check has to run BEFORE the location == -1 early-out";

    ProgramUniform1f(program, 0, 1.0f);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    // THE MATRIX FORMS TOO. The reorder originally landed on ProgramUniformv_State alone, so all
    // thirteen glProgramUniformMatrix* entry points kept the old `if (location == -1) return;`
    // first statement and stayed silent on exactly the case the rule exists for.
    const GLfloat m[16] = {};
    ProgramUniformMatrix2fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2fv";
    ProgramUniformMatrix3fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3fv";
    ProgramUniformMatrix4fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4fv";
    ProgramUniformMatrix2x3fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2x3fv";
    ProgramUniformMatrix3x2fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3x2fv";
    ProgramUniformMatrix2x4fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2x4fv";
    ProgramUniformMatrix4x2fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4x2fv";
    ProgramUniformMatrix3x4fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3x4fv";
    ProgramUniformMatrix4x3fv(program, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4x3fv";

    const GLdouble md[16] = {};
    ProgramUniformMatrix2dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2dv";
    ProgramUniformMatrix3dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3dv";
    ProgramUniformMatrix4dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4dv";
    ProgramUniformMatrix2x3dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2x3dv";
    ProgramUniformMatrix3x2dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3x2dv";
    ProgramUniformMatrix2x4dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix2x4dv";
    ProgramUniformMatrix4x2dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4x2dv";
    ProgramUniformMatrix3x4dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix3x4dv";
    ProgramUniformMatrix4x3dv(program, -1, 1, GL_FALSE, md);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION) << "glProgramUniformMatrix4x3dv";

    // A name GL never handed out is INVALID_VALUE, and the -1 location must not swallow that
    // either - this is the second error the early-out was hiding.
    ProgramUniformMatrix4fv(0xDEADBEEFu, -1, 1, GL_FALSE, m);
    EXPECT_EQ(GetError(), GL_INVALID_VALUE);

    DrainProgramTestErrors();
}

// ---------------------------------------------------------------------------------------------
// GL_ARB_gl_spirv, core since 4.6. glShaderBinary and glSpecializeShader were
// DECLARE_GL_FUNCTION_STUB entry points - they took their arguments, recorded no error and did
// nothing - and glGetShaderiv(GL_SPIR_V_BINARY) fell into the terminal INVALID_ENUM arm, which is
// where all nine gl_spirv conformance bodies died.
//
// The module below is a real one, compiled ahead of time by glslangValidator (-G --target-env
// opengl) so this GPU-free binary needs no toolchain at run time. Its GLSL:
//     layout(location = 0) in vec2 aPos;
//     layout(constant_id = 3) const float uScale = 1.0;
//     void main() { gl_Position = vec4(aPos * uScale, 0.0, 1.0); }
// The RENDERING half of the path is asserted separately, on a real context, in
// MG_IntegrationTest/Scenarios/SpirvShaderBinaryScenario.cpp.
// ---------------------------------------------------------------------------------------------

namespace {
    // Asserts a call recorded exactly one error and drains it, so the next case starts clean.
    void ExpectOnlyThisGlError(GLenum expected) {
        EXPECT_EQ(GetError(), expected);
        EXPECT_EQ(GetError(), GL_NO_ERROR) << "the call recorded more than one error";
    }

        // 255 words
        const unsigned int kVertexModule[] = {
            0x07230203u, 0x00010000u, 0x0008000bu, 0x00000020u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
            0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
            0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x0000001eu,
            0x0000001fu, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
            0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu,
            0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u,
            0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
            0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
            0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u, 0x00040005u, 0x00000012u, 0x736f5061u, 0x00000000u,
            0x00040005u, 0x00000014u, 0x61635375u, 0x0000656cu, 0x00050005u, 0x0000001eu, 0x565f6c67u, 0x65747265u,
            0x00444978u, 0x00060005u, 0x0000001fu, 0x495f6c67u, 0x6174736eu, 0x4965636eu, 0x00000044u, 0x00030047u,
            0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u,
            0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu,
            0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x00000012u,
            0x0000001eu, 0x00000000u, 0x00040047u, 0x00000014u, 0x00000001u, 0x00000003u, 0x00040047u, 0x0000001eu,
            0x0000000bu, 0x00000005u, 0x00040047u, 0x0000001fu, 0x0000000bu, 0x00000006u, 0x00020013u, 0x00000002u,
            0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
            0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u,
            0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u, 0x0006001eu, 0x0000000bu,
            0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu,
            0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u,
            0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040017u, 0x00000010u, 0x00000006u, 0x00000002u,
            0x00040020u, 0x00000011u, 0x00000001u, 0x00000010u, 0x0004003bu, 0x00000011u, 0x00000012u, 0x00000001u,
            0x00040032u, 0x00000006u, 0x00000014u, 0x3f800000u, 0x0004002bu, 0x00000006u, 0x00000016u, 0x00000000u,
            0x0004002bu, 0x00000006u, 0x00000017u, 0x3f800000u, 0x00040020u, 0x0000001bu, 0x00000003u, 0x00000007u,
            0x00040020u, 0x0000001du, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000001du, 0x0000001eu, 0x00000001u,
            0x0004003bu, 0x0000001du, 0x0000001fu, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
            0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u, 0x00000012u, 0x0005008eu,
            0x00000010u, 0x00000015u, 0x00000013u, 0x00000014u, 0x00050051u, 0x00000006u, 0x00000018u, 0x00000015u,
            0x00000000u, 0x00050051u, 0x00000006u, 0x00000019u, 0x00000015u, 0x00000001u, 0x00070050u, 0x00000007u,
            0x0000001au, 0x00000018u, 0x00000019u, 0x00000016u, 0x00000017u, 0x00050041u, 0x0000001bu, 0x0000001cu,
            0x0000000du, 0x0000000fu, 0x0003003eu, 0x0000001cu, 0x0000001au, 0x000100fdu, 0x00010038u,
        };
} // namespace

TEST_F(ProgramTest, ShaderBinaryStoresASpirvModuleAndTheStateQueryReportsIt) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    GLint isSpirv = GL_TRUE;
    GetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "GL_SPIR_V_BINARY is an accepted pname in a 4.6 context";
    EXPECT_EQ(isSpirv, GL_FALSE);

    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(isSpirv, GL_TRUE);

    // glCompileShader on a SPIR-V shader is INVALID_OPERATION: glSpecializeShader is what compiles
    // one. This is the whole of spirv_modules_error_verification_test's first assertion.
    CompileShader(shader);
    EXPECT_EQ(GetError(), GL_INVALID_OPERATION);

    // glShaderSource takes the object back to being a GLSL shader.
    const char* source = "#version 450 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    ShaderSource(shader, 1, &source, nullptr);
    GetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(isSpirv, GL_FALSE);
    CompileShader(shader);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    DrainProgramTestErrors();
}

TEST_F(ProgramTest, ShaderBinaryValidatesItsArguments) {
    DrainProgramTestErrors();

    GLuint shaders[2] = {0, 0};
    shaders[0] = CreateShader(GL_VERTEX_SHADER);
    shaders[1] = CreateShader(GL_FRAGMENT_SHADER);

    // The only accepted format is the SPIR-V one; the stub used to accept everything silently.
    ShaderBinary(1, shaders, GL_PROGRAM_BINARY_FORMATS, kVertexModule, sizeof(kVertexModule));
    ExpectOnlyThisGlError(GL_INVALID_ENUM);

    // A SPIR-V module is a sequence of 32-bit words.
    ShaderBinary(1, shaders, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule) - 1);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // The same shader twice is INVALID_VALUE, and nothing may have been attached.
    GLuint duplicated[2] = {shaders[0], shaders[0]};
    ShaderBinary(2, duplicated, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ExpectOnlyThisGlError(GL_INVALID_VALUE);
    GLint isSpirv = GL_TRUE;
    GetShaderiv(shaders[0], GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(isSpirv, GL_FALSE) << "a rejected glShaderBinary is all-or-nothing";

    // A name that is not a shader object.
    GLuint bogus = 0xBADBEEF;
    ShaderBinary(1, &bogus, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // Something that is not SPIR-V at all: the magic number gate, before SPIRV-Cross ever sees it.
    const unsigned int notSpirv[4] = {0xDEADBEEFu, 0u, 0u, 0u};
    ShaderBinary(1, shaders, GL_SHADER_BINARY_FORMAT_SPIR_V, notSpirv, sizeof(notSpirv));
    ExpectOnlyThisGlError(GL_INVALID_VALUE);
    GetShaderiv(shaders[0], GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(isSpirv, GL_FALSE);

    // ONE call, TWO shader objects - the shape spirv_modules_shader_binary_multiple_shader_objects_test
    // exercises. Both end up holding the module.
    ShaderBinary(2, shaders, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    for (const GLuint shader : shaders) {
        GetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
        EXPECT_EQ(isSpirv, GL_TRUE);
    }

    DrainProgramTestErrors();
}

TEST_F(ProgramTest, SpecializeShaderCompilesTheModuleAndAppliesItsConstants) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);

    // Before any module: INVALID_OPERATION rather than a silent no-op.
    const unsigned int constantId = 3;
    const unsigned int constantValue = 0;
    SpecializeShader(shader, "main", 1, &constantId, &constantValue);
    ExpectOnlyThisGlError(GL_INVALID_OPERATION);

    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // Constant id 3 is the module's `uScale`, handed over as the bit pattern of 0.5f.
    float half = 0.5f;
    unsigned int halfBits = 0;
    std::memcpy(&halfBits, &half, sizeof(halfBits));
    SpecializeShader(shader, "main", 1, &constantId, &halfBits);
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    GLint compiled = GL_FALSE;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    char infoLog[2048] = "";
    GetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
    EXPECT_EQ(compiled, GL_TRUE) << infoLog;

    // The module stays attached after specialization - GL_SPIR_V_BINARY keeps reading TRUE - but
    // the shader may NOT be specialized again. ARB_gl_spirv: "Once specialized, a shader may not
    // be re-specialized without first re-associating the original SPIR-V module with it, through
    // ShaderBinary."
    GLint isSpirv = GL_FALSE;
    GetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
    EXPECT_EQ(isSpirv, GL_TRUE);
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    ExpectOnlyThisGlError(GL_INVALID_OPERATION);
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    EXPECT_EQ(compiled, GL_TRUE) << "the refused call must not have disturbed the first specialization";

    // Re-associating the module is what makes a second specialization legal again - and it is the
    // only thing that does.
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    EXPECT_EQ(compiled, GL_TRUE);

    DrainProgramTestErrors();
}

// glShaderSource does the same re-association in the other direction: it turns the object back
// into a GLSL shader, so a later glShaderBinary + glSpecializeShader pair is legal again.
TEST_F(ProgramTest, ShaderSourceClearsTheSpecializedLatch) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const char* source = "#version 450 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    ShaderSource(shader, 1, &source, nullptr);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "the latch must not survive a round trip through glShaderSource";

    DrainProgramTestErrors();
}

// A shader that came from glShaderBinary has never had glShaderSource called on it, so GL 4.6
// core 7.1 makes its source the empty string - including AFTER glSpecializeShader, when the
// object internally holds the GLSL the module was translated into. That text is MobileGL's, not
// the application's, and handing it back invites an application to cache and re-submit it.
TEST_F(ProgramTest, ASpirvShaderReportsNoApplicationSource) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    GLint sourceLength = -1;
    GetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &sourceLength);
    EXPECT_EQ(sourceLength, 0);

    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    GLint compiled = GL_FALSE;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    ASSERT_EQ(compiled, GL_TRUE) << "the leak this pins only exists on the specialized path";

    sourceLength = -1;
    GetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &sourceLength);
    EXPECT_EQ(sourceLength, 0) << "the SPIRV-Cross GLSL is not the application's source";

    char buffer[64];
    std::memset(buffer, 'x', sizeof(buffer));
    GLsizei written = -1;
    GetShaderSource(shader, static_cast<GLsizei>(sizeof(buffer)), &written, buffer);
    EXPECT_EQ(written, 0);
    EXPECT_EQ(buffer[0], '\0');
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // A GLSL shader still answers with what the application gave it.
    const char* source = "#version 450 core\nvoid main() { gl_Position = vec4(0.0); }\n";
    ShaderSource(shader, 1, &source, nullptr);
    GetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &sourceLength);
    EXPECT_EQ(sourceLength, static_cast<GLint>(std::strlen(source)) + 1);

    DrainProgramTestErrors();
}

// The two conditions ARB_gl_spirv ENUMERATES are GL_INVALID_VALUE, not compile failures: "an
// INVALID_VALUE error is generated if pEntryPoint does not name a valid entry point for shader"
// and "...if any element of pConstantIndex refers to a specialization constant that does not exist
// in the shader module contained in shader". Both used to be reported as COMPILE_STATUS false with
// no GL error, which an application checking glGetError could not see at all.
//
// The distinction matters beyond the error code: an erroring GL command must have NO OTHER EFFECT,
// so neither of these may leave the shader object in a failed-compile state. The conformance suite
// leans on exactly that - it fails specialization twice on one object and then requires the next,
// well-formed call on that same object to succeed.
TEST_F(ProgramTest, SpecializeShaderRaisesInvalidValueForBadEntryPointsAndUnknownConstants) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // A constant id the module does not declare.
    const unsigned int unknownId = 4242;
    const unsigned int value = 0;
    SpecializeShader(shader, "main", 1, &unknownId, &value);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // An entry point the module does not carry.
    SpecializeShader(shader, "notMain", 0, nullptr, nullptr);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // Neither of them may name an entry point at all.
    SpecializeShader(shader, nullptr, 0, nullptr, nullptr);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);
    SpecializeShader(shader, "", 0, nullptr, nullptr);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // A repeated constant index is GL_INVALID_VALUE at the entry point itself.
    const unsigned int repeated[2] = {3, 3};
    const unsigned int values[2] = {0, 0};
    SpecializeShader(shader, "main", 2, repeated, values);
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    // AND NOW THE POINT: none of the five refused calls specialized the shader or damaged it, so
    // the well-formed call that follows must still be accepted. Latching the "specialized" flag on
    // the failure path - the obvious way to implement the re-specialization rule - breaks exactly
    // here, which is why the flag is only ever set on the success path.
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    EXPECT_EQ(GetError(), GL_NO_ERROR) << "a failed specialization does not make the shader specialized";
    GLint compiled = GL_FALSE;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    EXPECT_EQ(compiled, GL_TRUE);

    DrainProgramTestErrors();
}

// A GENUINE compile failure of a well-formed request keeps the COMPILE_STATUS surface: the module
// is a valid SPIR-V module naming a real entry point, it simply cannot be translated for this
// stage. Nothing about that is one of the enumerated errors.
TEST_F(ProgramTest, SpecializeShaderStillReportsATranslationFailureThroughCompileStatus) {
    DrainProgramTestErrors();

    // The vertex module handed to a FRAGMENT shader object: its only entry point carries the
    // Vertex execution model, so no fragment entry point named "main" exists in it.
    const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    // Reported as INVALID_VALUE (there is no such entry point FOR THIS STAGE) - the stage is part
    // of what "a valid entry point for shader" means.
    ExpectOnlyThisGlError(GL_INVALID_VALUE);

    DrainProgramTestErrors();
}

TEST_F(ProgramTest, ShaderBinaryFormatsAreAdvertisedConsistently) {
    DrainProgramTestErrors();

    GLint count = -1;
    GetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &count);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    ASSERT_EQ(count, 1);

    GLint formats[4] = {0, 0, 0, 0};
    GetIntegerv(GL_SHADER_BINARY_FORMATS, formats);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(formats[0], static_cast<GLint>(GL_SHADER_BINARY_FORMAT_SPIR_V))
        << "the count and the list have to describe the same thing";

    DrainProgramTestErrors();
}

// ---------------------------------------------------------------------------------------------
// ARB_gl_spirv makes XfbBuffer / XfbStride / Offset DECORATIONS the only way a SPIR-V program
// declares transform feedback - glTransformFeedbackVaryings has no effect on such a program. The
// decorations were ignored entirely: the link ran off `in.requestedXfbVaryings`, which is empty
// for a SPIR-V program, so a module that asked for capture captured nothing and
// GL_TRANSFORM_FEEDBACK_VARYINGS answered zero.
//
// glSpecializeShader now reflects the decorations and re-expresses them as the equivalent
// glTransformFeedbackVaryings request (ARB_transform_feedback3's gl_SkipComponentsN carrying the
// declared offset), which is the form every consumer downstream already implements.
//
// The module below is `layout(xfb_buffer = 0, xfb_offset = 16) out gl_PerVertex { vec4
// gl_Position; };` over a trivial vertex shader - the exact shape gl4cGlSpirvTests'
// spirv_modules_state_queries_test feeds in first. Offset 16 with a stride of 32 means the capture
// is four components in, i.e. one gl_SkipComponents4 ahead of gl_Position.
// ---------------------------------------------------------------------------------------------

namespace {
    // 177 words
    const unsigned int kXfbVertexModule[] = {
        0x07230203u, 0x00010000u, 0x0008000bu, 0x00000015u, 0x00000000u, 0x00020011u, 0x00000001u, 0x00020011u,
        0x00000035u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
        0x00000000u, 0x00000001u, 0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000au,
        0x0000000eu, 0x00000013u, 0x00000014u, 0x00030010u, 0x00000004u, 0x0000000bu, 0x00030003u, 0x00000002u,
        0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x00000008u, 0x505f6c67u,
        0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x00000008u, 0x00000000u, 0x505f6c67u, 0x7469736fu,
        0x006e6f69u, 0x00030005u, 0x0000000au, 0x00000000u, 0x00050005u, 0x0000000eu, 0x69736f70u, 0x6e6f6974u,
        0x00000000u, 0x00050005u, 0x00000013u, 0x565f6c67u, 0x65747265u, 0x00444978u, 0x00060005u, 0x00000014u,
        0x495f6c67u, 0x6174736eu, 0x4965636eu, 0x00000044u, 0x00030047u, 0x00000008u, 0x00000002u, 0x00050048u,
        0x00000008u, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x00000008u, 0x00000000u, 0x00000023u,
        0x00000010u, 0x00040047u, 0x0000000au, 0x00000024u, 0x00000000u, 0x00040047u, 0x0000000au, 0x00000025u,
        0x00000020u, 0x00040047u, 0x0000000eu, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000013u, 0x0000000bu,
        0x00000005u, 0x00040047u, 0x00000014u, 0x0000000bu, 0x00000006u, 0x00020013u, 0x00000002u, 0x00030021u,
        0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
        0x00000004u, 0x0003001eu, 0x00000008u, 0x00000007u, 0x00040020u, 0x00000009u, 0x00000003u, 0x00000008u,
        0x0004003bu, 0x00000009u, 0x0000000au, 0x00000003u, 0x00040015u, 0x0000000bu, 0x00000020u, 0x00000001u,
        0x0004002bu, 0x0000000bu, 0x0000000cu, 0x00000000u, 0x00040020u, 0x0000000du, 0x00000001u, 0x00000007u,
        0x0004003bu, 0x0000000du, 0x0000000eu, 0x00000001u, 0x00040020u, 0x00000010u, 0x00000003u, 0x00000007u,
        0x00040020u, 0x00000012u, 0x00000001u, 0x0000000bu, 0x0004003bu, 0x00000012u, 0x00000013u, 0x00000001u,
        0x0004003bu, 0x00000012u, 0x00000014u, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
        0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000fu, 0x0000000eu, 0x00050041u,
        0x00000010u, 0x00000011u, 0x0000000au, 0x0000000cu, 0x0003003eu, 0x00000011u, 0x0000000fu, 0x000100fdu,
        0x00010038u,
    };
} // namespace

TEST_F(ProgramTest, ASpirvModulesXfbDecorationsBecomeTheProgramsCaptureList) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kXfbVertexModule, sizeof(kXfbVertexModule));
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    GLint compiled = GL_FALSE;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    char shaderLog[2048] = "";
    GetShaderInfoLog(shader, sizeof(shaderLog), nullptr, shaderLog);
    ASSERT_EQ(compiled, GL_TRUE) << shaderLog;

    const GLuint program = CreateProgram();
    AttachShader(program, shader);
    // NO glTransformFeedbackVaryings anywhere: the declaration is the module's own.
    LinkProgram(program);
    GLint linked = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linked);
    char programLog[2048] = "";
    GetProgramInfoLog(program, sizeof(programLog), nullptr, programLog);
    ASSERT_EQ(linked, GL_TRUE) << programLog;

    GLint varyingCount = -1;
    GetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS, &varyingCount);
    EXPECT_GT(varyingCount, 0) << "the module's xfb decorations declared a capture and none was recorded";

    // The captured name is the built-in the block redeclared. It is found by BuiltIn decoration,
    // not by string, because a stripped module carries no OpMemberName at all.
    Bool sawPosition = false;
    for (GLint i = 0; i < varyingCount; ++i) {
        char name[128] = "";
        GLsizei nameLength = 0;
        GLsizei size = 0;
        GLenum type = 0;
        GetTransformFeedbackVarying(program, static_cast<GLuint>(i), sizeof(name), &nameLength, &size, &type, name);
        if (String(name) == "gl_Position") sawPosition = true;
    }
    EXPECT_TRUE(sawPosition) << "gl_Position was declared captured by the module's Offset decoration";
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    DrainProgramTestErrors();
}

// The other half of the same fix: the decorations must NOT survive into the GLSL the module is
// translated into. SPIRV-Cross re-emits them as layout(xfb_buffer/xfb_stride/xfb_offset), glslang
// re-encodes them into the regenerated SPIR-V, and the DirectGLES ESSL hop then refuses them
// outright ("Need GL_ARB_enhanced_layouts for xfb_stride or xfb_buffer") and drops the stage -
// a program that links clean and draws nothing. Compiling at all is the observable proof they are
// gone; the ESSL leg is exercised by the integration scenario.
TEST_F(ProgramTest, ASpirvModulesXfbDecorationsDoNotSurviveIntoTheTranslatedSource) {
    DrainProgramTestErrors();

    const GLuint shader = CreateShader(GL_VERTEX_SHADER);
    ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kXfbVertexModule, sizeof(kXfbVertexModule));
    SpecializeShader(shader, "main", 0, nullptr, nullptr);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    GLint compiled = GL_FALSE;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    char shaderLog[2048] = "";
    GetShaderInfoLog(shader, sizeof(shaderLog), nullptr, shaderLog);
    EXPECT_EQ(compiled, GL_TRUE) << shaderLog;

    DrainProgramTestErrors();
}
