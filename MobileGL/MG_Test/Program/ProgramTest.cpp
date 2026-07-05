// MobileGL - MobileGL/MG_Test/Program/ProgramTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include "MG_Backend/DirectVulkan/DirectVulkanResourceState.h"
#include "MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

class ProgramTest : public ::testing::Test {
protected:
    void SetUp() override { MobileGL::Initialize(); }

    void TearDown() override {}
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
