// MobileGL - MobileGL/MG_Benchmark/Program/ProgramBench.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <cstring>
#include <benchmark/benchmark.h>

#include "Init.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

// Shader source code for benchmarking
const char* vsSrc = R"(#version 460

layout (location = 0) in vec4 Position;
in float fIn4;
in float fIn2;
in float fIn5;
in float fIn6;
in float fIn1;
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

    oneTexel = (1.0 * (fIn1 * fIn2 * fIn3 * fIn4 * fIn5 * fIn6)) / InSize;

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

static void BM_CompileVertexShader(benchmark::State& state) {
    SizeT vsLen = strlen(vsSrc);

    for (auto _ : state) {
        // Create and compile vertex shader
        GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsSrc, NULL);
        CompileShader(vs);

        // Cleanup
        DeleteShader(vs);
    }

    // Set the counter for Compiles/Second
    state.counters["VS_Compiles/Second"] = state.iterations();
    state.counters["VS_Bytes/Second"] = state.iterations() * vsLen;
}
BENCHMARK(BM_CompileVertexShader)->Unit(benchmark::kMillisecond);

static void BM_CompileFragmentShader(benchmark::State& state) {
    SizeT fsLen = strlen(fsSrc);

    for (auto _ : state) {
        // Create and compile fragment shader
        GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsSrc, NULL);
        CompileShader(fs);

        // Cleanup
        DeleteShader(fs);
    }

    // Set the counter for Compiles/Second
    state.counters["FS_Compiles/Second"] = state.iterations();
    state.counters["FS_Bytes/Second"] = state.iterations() * fsLen;
}
BENCHMARK(BM_CompileFragmentShader)->Unit(benchmark::kMillisecond);

static void BM_CompileBothShaders(benchmark::State& state) {
    SizeT vsLen = strlen(vsSrc);
    SizeT fsLen = strlen(fsSrc);

    for (auto _ : state) {
        // Create and compile vertex shader
        GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsSrc, NULL);
        CompileShader(vs);

        // Create and compile fragment shader
        GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsSrc, NULL);
        CompileShader(fs);

        // Cleanup
        DeleteShader(vs);
        DeleteShader(fs);
    }

    // Set the counter for Compiles/Second
    state.counters["VS_Compiles/Second"] = state.iterations();
    state.counters["FS_Compiles/Second"] = state.iterations();
    state.counters["VS_Bytes/Second"] = state.iterations() * vsLen;
    state.counters["FS_Bytes/Second"] = state.iterations() * fsLen;
}
BENCHMARK(BM_CompileBothShaders)->Unit(benchmark::kMillisecond);

static void BM_LinkProgram(benchmark::State& state) {
    // Create and compile vertex shader
    GLuint vs = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vs, 1, &vsSrc, NULL);
    CompileShader(vs);

    // Create and compile fragment shader
    GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &fsSrc, NULL);
    CompileShader(fs);

    for (auto _ : state) {
        GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        DeleteProgram(program);
    }

    // Cleanup
    DeleteShader(vs);
    DeleteShader(fs);

    // Set the counter for Compiles/Second
    state.counters["Links/Second"] = state.iterations();
}
BENCHMARK(BM_LinkProgram)->Unit(benchmark::kMillisecond);

static void BM_CompileAndLink(benchmark::State& state) {
    SizeT vsLen = strlen(vsSrc);
    SizeT fsLen = strlen(fsSrc);

    for (auto _ : state) {
        // Create and compile vertex shader
        GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsSrc, NULL);
        CompileShader(vs);

        // Create and compile fragment shader
        GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsSrc, NULL);
        CompileShader(fs);

        GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        DeleteProgram(program);

        // Cleanup
        DeleteShader(vs);
        DeleteShader(fs);
    }

    // Set the counter for Compiles/Second
    state.counters["VS_Compiles/Second"] = state.iterations();
    state.counters["FS_Compiles/Second"] = state.iterations();
    state.counters["VS_Bytes/Second"] = state.iterations() * vsLen;
    state.counters["FS_Bytes/Second"] = state.iterations() * fsLen;
    state.counters["Links/Second"] = state.iterations();
}

BENCHMARK(BM_CompileAndLink)->Unit(benchmark::kMillisecond);

int main(int argc, char** argv) {
    Initialize();
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    char arg0_default[] = "benchmark";
    char* args_default = reinterpret_cast<char*>(arg0_default);
    if (!argv) {
        argc = 1;
        argv = &args_default;
    }
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
