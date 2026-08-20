// MobileGL - MobileGL/MG_Benchmark/Transpile/TranspileProfile.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Per-stage stopwatch for the DirectGLES program-build path.
//
// The chain a program goes through is GLSL text -> ShaderSourceProcessor -> glslang parse ->
// glslang link + mapIO -> GlslangToSpv -> SanitizeAndOptimizeBinary -> SPIRV-Reflect
// (global-UBO routing) -> a run of separate ShaderCompiler::...ForEssl optimizer round trips
// and Declares*/Probe* module parses -> SPIRV-Cross -> ESSL text passes -> the driver.
//
// Every one of those ...ForEssl entry points builds its own spvtools::Optimizer and calls
// Run(), i.e. a full SPIR-V parse + IR build + re-serialize per call, and every Declares*
// probe is another full BuildModule. This program measures each of them separately, plus a
// zero-pass Optimizer::Run and a bare BuildModule on the same binary, so the FIXED
// parse/serialize overhead can be separated from the work the passes actually do. It also
// runs the same pass set merged onto ONE Optimizer, which is the saving a merged chain
// would realise.
//
// Measurement only: it links MobileGL_s and drives the public ShaderCompiler API, so it
// needs no GL context and no driver. The driver's own glCompileShader/glLinkProgram is
// therefore NOT included here - see the report for how to measure that half on device.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "Init.h"

#include <MG_State/GLState/ProgramState/ShaderStage.h>
#include <MG_Util/ShaderTranspiler/CompileEnv.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/SpvcSession.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <MG_Util/ShaderTranspiler/SpirvPasses/ClampMultisampleFetchPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/Lower1DArrayImagesPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/PrivateToEntryLocalPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/EmulateNoPerspectivePass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/LowerDrawParametersPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/LowerViewportIndexPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/NormalizeRectCoordinatesPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/SplitArrayVertexInputsPass.h>
#include <MG_Util/ShaderTranspiler/SpirvPasses/StripUboMemberRelaxedPrecisionPass.h>

#include "glslang/Include/PoolAlloc.h"
#include "glslang/MachineIndependent/Initialize.h"

#include "source/opt/build_module.h"
#include "source/opt/ir_context.h"
#include "spirv-tools/optimizer.hpp"

namespace MobileGL::MG_Util::ShaderTranspiler {
    // Defined in ShaderCompiler.cpp at namespace scope, with no header declaration; the
    // resource table every glslang parse is handed.
    TBuiltInResource BuildTBuiltInResource(const CompileEnv* env);
} // namespace MobileGL::MG_Util::ShaderTranspiler

using namespace MobileGL;
using namespace MobileGL::MG_Util::ShaderTranspiler;
using Clock = std::chrono::steady_clock;

namespace {

    // ---------------------------------------------------------------- timing

    struct Stat {
        double medianUs = 0.0;
        double p10Us = 0.0;
        double p90Us = 0.0;
        double minUs = 0.0;
        int reps = 0;
    };

    // Keeps a measured result observable so the optimizer cannot delete the work.
    volatile unsigned long long g_sink = 0;
    template <typename T>
    void benchmarkSink(T value) {
        g_sink += static_cast<unsigned long long>(value);
    }

    Stat Summarize(std::vector<double>& samples) {
        std::sort(samples.begin(), samples.end());
        Stat s;
        s.reps = static_cast<int>(samples.size());
        s.minUs = samples.front();
        s.medianUs = samples[samples.size() / 2];
        s.p10Us = samples[static_cast<size_t>(samples.size() * 0.10)];
        s.p90Us = samples[static_cast<size_t>(samples.size() * 0.90)];
        return s;
    }

    // Runs `body` until at least kMinReps samples exist AND kBudgetMs has elapsed, capped at
    // kMaxReps. Three untimed warm-up calls first, so allocator growth and first-touch page
    // faults land outside the sample set.
    template <typename F>
    Stat Measure(F&& body, const double budgetMs = 250.0, const int minReps = 15,
                 const int maxReps = 2000) {
        for (int i = 0; i < 3; ++i) body();

        std::vector<double> samples;
        samples.reserve(64);
        const auto start = Clock::now();
        while (true) {
            const auto t0 = Clock::now();
            body();
            const auto t1 = Clock::now();
            samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (static_cast<int>(samples.size()) >= maxReps) break;
            const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - start).count();
            if (static_cast<int>(samples.size()) >= minReps && elapsedMs >= budgetMs) break;
        }
        return Summarize(samples);
    }

    // Same loop, but `body` returns the microseconds of the part that should be timed - for
    // stages whose set-up (a fresh glslang parse, a fresh link) has to happen per iteration
    // and must NOT be counted.
    template <typename F>
    Stat MeasureInner(F&& body, const double budgetMs = 250.0, const int minReps = 15,
                      const int maxReps = 2000) {
        for (int i = 0; i < 3; ++i) (void)body();

        std::vector<double> samples;
        samples.reserve(64);
        const auto start = Clock::now();
        while (true) {
            samples.push_back(body());
            if (static_cast<int>(samples.size()) >= maxReps) break;
            const double elapsedMs =
                std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (static_cast<int>(samples.size()) >= minReps && elapsedMs >= budgetMs) break;
        }
        return Summarize(samples);
    }

    struct Row {
        std::string corpus;
        std::string stage;   // "VS" / "FS" / "program"
        std::string what;
        std::string kind;    // roundtrip / probe / frontend / crosscompile / baseline / merged
        size_t moduleWords = 0;
        Stat stat;
    };

    std::vector<Row> g_rows;

    void Emit(const std::string& corpus, const std::string& stage, const std::string& what,
              const std::string& kind, size_t words, const Stat& s) {
        g_rows.push_back(Row{corpus, stage, what, kind, words, s});
        std::printf("%-14s %-8s %-40s %-13s %7zu %10.1f %10.1f %10.1f %6d\n", corpus.c_str(),
                    stage.c_str(), what.c_str(), kind.c_str(), words, s.medianUs, s.p10Us,
                    s.p90Us, s.reps);
        std::fflush(stdout);
    }

    // ---------------------------------------------------------------- corpora

    // (a) The pair already in MG_Benchmark/Program/ProgramBench.cpp.
    const char* kTinyVs = R"(#version 460

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

    vec2 dummy2 = TestMat2[0];
    vec3 dummy3 = TestMat3[0];

    oneTexel = (1.0 * (fIn1 * fIn2 * fIn3 * fIn4 * fIn5 * fIn6)) / InSize;

    texCoord = Position.xy / OutSize;
})";

    const char* kTinyFs = R"(#version 460

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

    float RedValue = dot(InTexel.rgb, RedMatrix);
    float GreenValue = dot(InTexel.rgb, GreenMatrix0);
    float BlueValue = dot(InTexel.rgb, BlueMatrix);
    vec3 OutColor = vec3(RedValue, GreenValue, BlueValue);

    OutColor = (OutColor * ColorScale) + Offset;

    float Luma = dot(OutColor, Gray);
    vec3 Chroma = OutColor - Luma;
    OutColor = (Chroma * Saturation) + Luma;

    fragColor = vec4(OutColor, float(intVal));
})";

    // (b) KHR-GL33.texture_swizzle.smoke_*, reproduced from the templates in
    // external/openglcts/modules/gl/gl3cTextureSwizzleTests.cpp (SmokeTest::getVertexShader /
    // getFragmentShader). One instantiation: source format usampler2D ("uint"/"u"), output
    // format "uint", 2D target, access "texture", channel "r". prepareAndTestProgram builds
    // both the fragment-tested and the vertex-tested pair, which is why both appear here;
    // that is the pair the case links 2592 times.
    const char* kCtsBlankVs = R"(#version 330 core

void main()
{
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, 1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, 1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,-1.0, 0.0, 1.0); break;
      case 3: gl_Position = vec4( 1.0,-1.0, 0.0, 1.0); break;
    }
}
)";

    const char* kCtsTestFs = R"(#version 330 core

uniform usampler2D sampler;

out uint out_color;

void main()
{
    uint result = texture(sampler, vec2(0, 0)).r;

    out_color = result;
}
)";

    const char* kCtsTestVs = R"(#version 330 core

uniform usampler2D sampler;

flat out uint result;

void main()
{
    result = texture(sampler, vec2(0, 0)).r;

    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, 1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, 1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,-1.0, 0.0, 1.0); break;
      case 3: gl_Position = vec4( 1.0,-1.0, 0.0, 1.0); break;
    }
}
)";

    const char* kCtsBlankFs = R"(#version 330 core

flat in uint result;

out uint out_color;

void main()
{
    out_color = result;
}
)";

    // (c) A large, realistic deferred-lighting fragment shader in the shape Iris shader packs
    // reach the transpiler in: a flat wall of default-block uniforms (so the global-UBO
    // packing and the OpName traffic are representative), several samplers, helper functions,
    // a shadow-filter loop, PBR, fog, and tonemapping. Synthesized rather than copied so the
    // benchmark carries no third-party shader-pack source.
    const char* kBigVs = R"(#version 330 core

in vec3 vaPosition;
in vec4 vaColor;
in vec2 vaUV0;
in ivec2 vaUV2;
in vec3 vaNormal;
in vec4 at_tangent;
in vec2 mc_Entity;
in vec2 mc_midTexCoord;

uniform mat4 modelViewMatrix;
uniform mat4 modelViewMatrixInverse;
uniform mat4 projectionMatrix;
uniform mat4 projectionMatrixInverse;
uniform mat4 gbufferModelView;
uniform mat4 gbufferModelViewInverse;
uniform mat4 gbufferProjection;
uniform mat4 gbufferProjectionInverse;
uniform mat4 shadowModelView;
uniform mat4 shadowProjection;
uniform mat4 textureMatrix;
uniform vec3 cameraPosition;
uniform vec3 previousCameraPosition;
uniform vec3 chunkOffset;
uniform float frameTimeCounter;
uniform float rainStrength;
uniform float viewWidth;
uniform float viewHeight;
uniform int worldTime;
uniform int frameCounter;

out vec4 vColor;
out vec2 vTexCoord;
out vec2 vLightCoord;
out vec3 vNormal;
out vec3 vTangent;
out vec3 vViewPos;
out vec3 vWorldPos;
out vec3 vShadowPos;
out float vBlockId;
out float vFogDepth;

vec3 WavePosition(vec3 worldPos, float id, float t) {
    if (id < 0.5) return worldPos;
    float phase = worldPos.x * 0.35 + worldPos.z * 0.27 + t * 1.7;
    float amp = 0.045 * (1.0 + rainStrength);
    worldPos.x += sin(phase) * amp;
    worldPos.z += cos(phase * 1.13) * amp;
    worldPos.y += sin(phase * 0.71) * amp * 0.5;
    return worldPos;
}

void main() {
    vec3 localPos = vaPosition + chunkOffset;
    vec3 worldPos = localPos + cameraPosition;
    worldPos = WavePosition(worldPos, mc_Entity.x, frameTimeCounter);
    localPos = worldPos - cameraPosition;

    vec4 viewPos = modelViewMatrix * vec4(localPos, 1.0);
    gl_Position = projectionMatrix * viewPos;

    vColor = vaColor;
    vTexCoord = (textureMatrix * vec4(vaUV0, 0.0, 1.0)).xy;
    vLightCoord = clamp((vec2(vaUV2) - 8.0) / 240.0, 0.0, 1.0);
    vNormal = normalize(mat3(modelViewMatrix) * vaNormal);
    vTangent = normalize(mat3(modelViewMatrix) * at_tangent.xyz);
    vViewPos = viewPos.xyz;
    vWorldPos = worldPos;
    vBlockId = mc_Entity.x;
    vFogDepth = length(viewPos.xyz);

    vec4 shadowView = shadowModelView * vec4(localPos, 1.0);
    vec4 shadowClip = shadowProjection * shadowView;
    vShadowPos = shadowClip.xyz / max(shadowClip.w, 1e-5) * 0.5 + 0.5;
}
)";

    const char* kBigFs = R"(#version 330 core

uniform sampler2D gtexture;
uniform sampler2D lightmap;
uniform sampler2D normals;
uniform sampler2D specular;
uniform sampler2D noisetex;
uniform sampler2D depthtex0;
uniform sampler2D depthtex1;
uniform sampler2D colortex0;
uniform sampler2D colortex1;
uniform sampler2D colortex2;
uniform sampler2D colortex3;
uniform sampler2D colortex4;
uniform sampler2DShadow shadowtex0;
uniform sampler2DShadow shadowtex1;
uniform sampler2D shadowcolor0;

uniform mat4 gbufferModelView;
uniform mat4 gbufferModelViewInverse;
uniform mat4 gbufferProjection;
uniform mat4 gbufferProjectionInverse;
uniform mat4 gbufferPreviousModelView;
uniform mat4 gbufferPreviousProjection;
uniform mat4 shadowModelView;
uniform mat4 shadowModelViewInverse;
uniform mat4 shadowProjection;
uniform mat4 shadowProjectionInverse;

uniform vec3 cameraPosition;
uniform vec3 previousCameraPosition;
uniform vec3 sunPosition;
uniform vec3 moonPosition;
uniform vec3 shadowLightPosition;
uniform vec3 upPosition;
uniform vec3 fogColor;
uniform vec3 skyColor;
uniform vec4 entityColor;

uniform float frameTimeCounter;
uniform float frameTime;
uniform float sunAngle;
uniform float shadowAngle;
uniform float rainStrength;
uniform float wetness;
uniform float aspectRatio;
uniform float viewWidth;
uniform float viewHeight;
uniform float near;
uniform float far;
uniform float nightVision;
uniform float blindness;
uniform float darknessFactor;
uniform float screenBrightness;
uniform float eyeAltitude;
uniform float centerDepthSmooth;
uniform float playerMood;

uniform int worldTime;
uniform int worldDay;
uniform int moonPhase;
uniform int frameCounter;
uniform int heldItemId;
uniform int heldBlockLightValue;
uniform int isEyeInWater;
uniform int hideGUI;
uniform ivec2 eyeBrightness;
uniform ivec2 eyeBrightnessSmooth;

in vec4 vColor;
in vec2 vTexCoord;
in vec2 vLightCoord;
in vec3 vNormal;
in vec3 vTangent;
in vec3 vViewPos;
in vec3 vWorldPos;
in vec3 vShadowPos;
in float vBlockId;
in float vFogDepth;

layout(location = 0) out vec4 outColor0;
layout(location = 1) out vec4 outColor1;
layout(location = 2) out vec4 outColor2;
layout(location = 3) out vec4 outColor3;

const float PI = 3.14159265358979323846;
const float SHADOW_BIAS = 0.0009;
const int SHADOW_SAMPLES = 12;

const vec2 kPoisson[12] = vec2[12](
    vec2(-0.3260, -0.4058), vec2( 0.7912, -0.0421), vec2(-0.1958, -0.9018),
    vec2(-0.2354,  0.5259), vec2( 0.4479,  0.5734), vec2( 0.9036,  0.4162),
    vec2(-0.7935, -0.5960), vec2(-0.0344,  0.0468), vec2(-0.9174,  0.2495),
    vec2( 0.4443, -0.7563), vec2(-0.5551, -0.0998), vec2( 0.1770,  0.9294)
);

float Luminance(vec3 c) {
    return dot(c, vec3(0.2125, 0.7154, 0.0721));
}

float LinearizeDepth(float d) {
    return (2.0 * near * far) / (far + near - (d * 2.0 - 1.0) * (far - near));
}

vec3 ScreenToView(vec3 screenPos) {
    vec4 ndc = vec4(screenPos * 2.0 - 1.0, 1.0);
    vec4 view = gbufferProjectionInverse * ndc;
    return view.xyz / view.w;
}

vec3 ViewToWorld(vec3 viewPos) {
    return (gbufferModelViewInverse * vec4(viewPos, 1.0)).xyz + cameraPosition;
}

float Hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

mat2 RotationFromAngle(float a) {
    float s = sin(a);
    float c = cos(a);
    return mat2(c, -s, s, c);
}

float DistributionGGX(vec3 n, vec3 h, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float ndoth = max(dot(n, h), 0.0);
    float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-6);
}

float GeometrySchlick(float ndotv, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndotv / (ndotv * (1.0 - k) + k);
}

float GeometrySmith(vec3 n, vec3 v, vec3 l, float roughness) {
    return GeometrySchlick(max(dot(n, v), 0.0), roughness) *
           GeometrySchlick(max(dot(n, l), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float SampleShadow(vec3 shadowPos, float ndotl) {
    if (shadowPos.x < 0.0 || shadowPos.x > 1.0 || shadowPos.y < 0.0 || shadowPos.y > 1.0) {
        return 1.0;
    }
    float bias = SHADOW_BIAS * (1.0 + (1.0 - ndotl) * 3.0);
    float angle = Hash12(gl_FragCoord.xy + float(frameCounter)) * PI * 2.0;
    mat2 rot = RotationFromAngle(angle);
    float radius = 1.4 / 2048.0 * (1.0 + wetness);
    float sum = 0.0;
    for (int i = 0; i < SHADOW_SAMPLES; ++i) {
        vec2 offset = rot * kPoisson[i] * radius;
        sum += texture(shadowtex1, vec3(shadowPos.xy + offset, shadowPos.z - bias));
    }
    return sum / float(SHADOW_SAMPLES);
}

vec3 SkyLight(vec3 normal, float lightLevel) {
    vec3 up = normalize(mat3(gbufferModelViewInverse) * upPosition);
    float hemi = clamp(dot(normal, up) * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = mix(fogColor, skyColor, hemi);
    return sky * lightLevel * (1.0 - rainStrength * 0.6);
}

vec3 BlockLight(float lightLevel) {
    vec3 warm = vec3(1.0, 0.62, 0.28);
    float shaped = pow(lightLevel, 2.2);
    return warm * shaped * (1.0 + float(heldBlockLightValue) * 0.01);
}

vec3 ApplyFog(vec3 color, float dist, vec3 viewDir) {
    float density = 0.0016 * (1.0 + rainStrength * 2.0 + float(isEyeInWater) * 12.0);
    float amount = 1.0 - exp(-dist * density);
    vec3 tint = mix(fogColor, skyColor, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    return mix(color, tint, clamp(amount, 0.0, 1.0));
}

vec3 Tonemap(vec3 c) {
    c *= 1.6;
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    c = a / b;
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

void main() {
    vec4 albedo = texture(gtexture, vTexCoord) * vColor;
    if (albedo.a < 0.1) discard;

    vec4 normalTex = texture(normals, vTexCoord);
    vec4 specTex = texture(specular, vTexCoord);

    vec3 n = normalize(vNormal);
    vec3 t = normalize(vTangent);
    vec3 b = cross(n, t);
    mat3 tbn = mat3(t, b, n);
    vec3 tangentNormal = normalTex.xyz * 2.0 - 1.0;
    tangentNormal.z = sqrt(max(1.0 - dot(tangentNormal.xy, tangentNormal.xy), 0.0));
    vec3 normal = normalize(tbn * tangentNormal);

    float roughness = clamp(1.0 - specTex.r, 0.02, 1.0);
    float metallic = clamp(specTex.g, 0.0, 1.0);
    float emissive = clamp(specTex.b, 0.0, 1.0);
    float porosity = clamp(specTex.a, 0.0, 1.0);

    vec3 viewDir = normalize(-vViewPos);
    vec3 lightDir = normalize(shadowLightPosition);
    vec3 halfDir = normalize(viewDir + lightDir);

    float ndotl = max(dot(normal, lightDir), 0.0);
    float shadow = SampleShadow(vShadowPos, ndotl);

    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
    float ndf = DistributionGGX(normal, halfDir, roughness);
    float geo = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fres = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);

    vec3 kd = (vec3(1.0) - fres) * (1.0 - metallic);
    vec3 spec = (ndf * geo * fres) /
                max(4.0 * max(dot(normal, viewDir), 0.0) * ndotl, 1e-4);

    vec3 sunColor = mix(vec3(1.0, 0.92, 0.80), vec3(0.32, 0.42, 0.66), rainStrength);
    vec3 direct = (kd * albedo.rgb / PI + spec) * sunColor * ndotl * shadow;

    vec3 ambient = SkyLight(mat3(gbufferModelViewInverse) * normal, vLightCoord.y) * albedo.rgb;
    vec3 blockLit = BlockLight(vLightCoord.x) * albedo.rgb;
    vec3 emissiveLit = albedo.rgb * emissive * 4.0;

    vec3 color = direct + ambient + blockLit + emissiveLit;

    float wetMix = clamp(wetness * (1.0 - porosity) * vLightCoord.y, 0.0, 1.0);
    color = mix(color, color * 0.72, wetMix);

    color *= 1.0 - clamp(blindness + darknessFactor * 0.5, 0.0, 1.0);
    color += vec3(nightVision) * 0.06 * Luminance(albedo.rgb);

    vec3 worldViewDir = normalize(mat3(gbufferModelViewInverse) * -viewDir);
    color = ApplyFog(color, vFogDepth, worldViewDir);
    color = Tonemap(color);

    outColor0 = vec4(color, albedo.a);
    outColor1 = vec4(normal * 0.5 + 0.5, 1.0);
    outColor2 = vec4(roughness, metallic, emissive, 1.0);
    outColor3 = vec4(vLightCoord, vBlockId / 255.0, shadow);
}
)";

    // ------------------------------------------------------- shared helpers

    spvtools::MessageConsumer SilentConsumer() {
        return [](spv_message_level_t, const char*, const spv_position_t&, const char*) {};
    }

    struct BuiltProgram {
        std::vector<GLenum> types;
        std::vector<std::vector<unsigned>> raw;         // straight out of GlslangToSpv
        std::vector<std::vector<unsigned>> sanitized;   // after SanitizeAndOptimizeBinary
    };

    // Fresh parse of both stages. glslang mutates a TShader during link/mapIO, so anything
    // that needs to be timed repeatedly has to rebuild these each iteration.
    std::vector<SharedPtr<glslang::TShader>> ParseBoth(const std::string& vsPre,
                                                       const std::string& fsPre,
                                                       const CompileEnv& env) {
        std::vector<SharedPtr<glslang::TShader>> out;
        for (const auto& [type, src] :
             {std::pair<GLenum, const std::string*>{GL_VERTEX_SHADER, &vsPre},
              std::pair<GLenum, const std::string*>{GL_FRAGMENT_SHADER, &fsPre}}) {
            ShaderAttrib attrib{.shaderType = type, .sourceStr = *src, .flags = 0, .env = &env};
            auto r = ShaderCompiler::CompileShader(attrib);
            if (!r) {
                std::fprintf(stderr, "PARSE FAILED (%s):\n%s\n",
                             type == GL_VERTEX_SHADER ? "VS" : "FS", r.error().log.c_str());
                std::abort();
            }
            out.push_back(r.value());
        }
        return out;
    }

    // ------------------------------------------------------------ the sweep

    void ProfileCorpus(const std::string& name, const char* vsSrc, const char* fsSrc) {
        const CompileEnv& env = *GetDefaultCompileEnv();

        // ---- stage 0: ShaderSourceProcessor (pure String -> String) ----
        {
            Stat s = Measure([&] {
                String src = vsSrc;
                PreprocessShaderSource(ShaderStage::Vertex, src, env);
                benchmarkSink(src.size());
            });
            Emit(name, "VS", "ShaderSourceProcessor.Preprocess", "frontend", 0, s);
        }
        {
            Stat s = Measure([&] {
                String src = fsSrc;
                PreprocessShaderSource(ShaderStage::Fragment, src, env);
                benchmarkSink(src.size());
            });
            Emit(name, "FS", "ShaderSourceProcessor.Preprocess", "frontend", 0, s);
        }

        String vsPre = vsSrc;
        PreprocessShaderSource(ShaderStage::Vertex, vsPre, env);
        String fsPre = fsSrc;
        PreprocessShaderSource(ShaderStage::Fragment, fsPre, env);

        // ---- stage 1: glslang parse ----
        {
            Stat s = Measure([&] {
                ShaderAttrib a{.shaderType = GL_VERTEX_SHADER, .sourceStr = vsPre, .flags = 0, .env = &env};
                auto r = ShaderCompiler::CompileShader(a);
                benchmarkSink(r.has_value());
            });
            Emit(name, "VS", "glslang.parse", "frontend", 0, s);
        }
        {
            Stat s = Measure([&] {
                ShaderAttrib a{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fsPre, .flags = 0, .env = &env};
                auto r = ShaderCompiler::CompileShader(a);
                benchmarkSink(r.has_value());
            });
            Emit(name, "FS", "glslang.parse", "frontend", 0, s);
        }

        // ---- stage 2: glslang link + mapIO (fresh parses each iteration, untimed) ----
        {
            Stat inner = MeasureInner([&] {
                auto shaders = ParseBoth(vsPre, fsPre, env);
                const auto t0 = Clock::now();
                ProgramAttrib pa{.shaders = shaders};
                auto p = ShaderCompiler::LinkProgram(pa);
                const auto t1 = Clock::now();
                benchmarkSink(p.has_value());
                return std::chrono::duration<double, std::micro>(t1 - t0).count();
            });
            Emit(name, "program", "glslang.link+mapIO", "frontend", 0, inner);
        }

        // ---- stage 3: GlslangToSpv ----
        {
            Stat inner = MeasureInner([&] {
                auto shaders = ParseBoth(vsPre, fsPre, env);
                ProgramAttrib pa{.shaders = shaders};
                auto p = ShaderCompiler::LinkProgram(pa);
                std::vector<GLenum> types{GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
                ProgramBinaryAttrib ba{.shaderTypes = types, .program = *p.value()};
                const auto t0 = Clock::now();
                auto bin = ShaderCompiler::GetSpirvBinaryFromProgram(ba);
                const auto t1 = Clock::now();
                benchmarkSink(bin.has_value());
                return std::chrono::duration<double, std::micro>(t1 - t0).count();
            });
            Emit(name, "program", "glslang.GlslangToSpv (both stages)", "frontend", 0, inner);
        }

        // ---- build the artefacts every later measurement runs against ----
        BuiltProgram built;
        {
            auto shaders = ParseBoth(vsPre, fsPre, env);
            ProgramAttrib pa{.shaders = shaders};
            auto p = ShaderCompiler::LinkProgram(pa);
            if (!p) {
                std::fprintf(stderr, "LINK FAILED: %s\n", p.error().log.c_str());
                std::abort();
            }
            built.types = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
            ProgramBinaryAttrib ba{.shaderTypes = built.types, .program = *p.value()};
            auto bin = ShaderCompiler::GetSpirvBinaryFromProgram(ba);
            built.raw = bin.value();
            built.sanitized = built.raw;
            for (auto& m : built.sanitized) {
                Vector<uint32_t> out;
                if (!ShaderCompiler::SanitizeAndOptimizeBinary(m, out, true, false)) {
                    std::fprintf(stderr, "SANITIZE FAILED\n");
                    std::abort();
                }
                m = out;
            }
        }

        // ---- stage 4: SanitizeAndOptimizeBinary, per stage ----
        for (size_t i = 0; i < built.raw.size(); ++i) {
            const char* stage = i == 0 ? "VS" : "FS";
            const Vector<Uint32>& in = built.raw[i];
            Stat s = Measure([&] {
                Vector<uint32_t> out;
                ShaderCompiler::SanitizeAndOptimizeBinary(in, out, true, false);
                benchmarkSink(out.size());
            });
            Emit(name, stage, "SanitizeAndOptimizeBinary (11 passes)", "roundtrip", in.size(), s);

            Stat sv = Measure([&] {
                Vector<uint32_t> out;
                ShaderCompiler::SanitizeAndOptimizeBinary(in, out, true, true);
                benchmarkSink(out.size());
            });
            Emit(name, stage, "SanitizeAndOptimizeBinary + spirv-val", "roundtrip", in.size(), sv);

            // Which of the eleven passes the time goes to: the two optimizing ones
            // (PrivateToEntryLocal feeding AggressiveDCE) against the nine legality ones.
            Stat pd = Measure([&] {
                spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
                opt.SetMessageConsumer(SilentConsumer());
                opt.RegisterPass(PrivateToEntryLocalPass::CreatePrivateToEntryLocalPass());
                spvtools::OptimizerOptions o;
                o.set_run_validator(false);
                Vector<uint32_t> out;
                opt.Run(in.data(), in.size(), &out, o);
                benchmarkSink(out.size());
            });
            Emit(name, stage, "  Sanitize part: PrivateToEntryLocal only", "roundtrip", in.size(), pd);

            Stat ad = Measure([&] {
                spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
                opt.SetMessageConsumer(SilentConsumer());
                opt.RegisterPass(spvtools::CreateAggressiveDCEPass(false));
                spvtools::OptimizerOptions o;
                o.set_run_validator(false);
                Vector<uint32_t> out;
                opt.Run(in.data(), in.size(), &out, o);
                benchmarkSink(out.size());
            });
            Emit(name, stage, "  Sanitize part: AggressiveDCE only", "roundtrip", in.size(), ad);
        }

        // Everything below runs on the SANITIZED module, which is what the DirectGLES
        // backend actually receives (ProgramSpirvTask stores the optimized binary).
        for (size_t i = 0; i < built.sanitized.size(); ++i) {
            const bool isVs = (i == 0);
            const char* stage = isVs ? "VS" : "FS";
            const Vector<Uint32>& mod = built.sanitized[i];
            const size_t words = mod.size();

            // ---- the two fixed-cost baselines ----
            {
                Stat s = Measure([&] {
                    spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
                    spvtools::OptimizerOptions o;
                    o.set_run_validator(false);
                    opt.SetMessageConsumer(SilentConsumer());
                    Vector<uint32_t> out;
                    opt.Run(mod.data(), mod.size(), &out, o);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "BASELINE Optimizer::Run, ZERO passes", "baseline", words, s);
            }
            {
                Stat s = Measure([&] {
                    auto ctx = spvtools::BuildModule(SPV_ENV_VULKAN_1_1, SilentConsumer(),
                                                     mod.data(), mod.size());
                    benchmarkSink(ctx != nullptr);
                });
                Emit(name, stage, "BASELINE BuildModule only (parse, no emit)", "baseline", words, s);
            }

            // ---- the gate probes ----
            {
                Stat s = Measure([&] {
                    auto f = ShaderCompiler::ProbeSpirvGateFeatures(mod);
                    benchmarkSink(f.WritesViewportIndexOutput || f.DeclaresMultisampledImage);
                });
                Emit(name, stage, "ProbeSpirvGateFeatures (merged 2 gates)", "probe", words, s);
            }
            {
                Stat s = Measure([&] {
                    benchmarkSink(ShaderCompiler::DeclaresViewportIndexBuiltin(mod));
                });
                Emit(name, stage, "DeclaresViewportIndexBuiltin", "probe", words, s);
            }
            {
                Stat s = Measure([&] {
                    benchmarkSink(ShaderCompiler::DeclaresMultisampledImage(mod));
                });
                Emit(name, stage, "DeclaresMultisampledImage", "probe", words, s);
            }
            {
                Stat s = Measure([&] {
                    benchmarkSink(ShaderCompiler::DeclaresFormatlessStorageImage(mod));
                });
                Emit(name, stage, "DeclaresFormatlessStorageImage", "probe", words, s);
            }
            {
                Stat s = Measure([&] {
                    benchmarkSink(ShaderCompiler::ModuleDeclaresBufferTextureSampler(mod));
                });
                Emit(name, stage, "ModuleDeclaresBufferTextureSampler", "probe", words, s);
            }
            {
                // Early-outs on every module that declares no 1D-array storage image, i.e.
                // InspectBinary's BuildModule + a full copy of the binary.
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::Lower1DArrayImagesForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "Lower1DArrayImagesForEssl (gate-only path)", "probe", words, s);
            }
            if (!isVs) {
                // Same shape: BinaryHasDynamicOutputIndexing's BuildModule + a copy.
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "LegalizeFragmentOutputIndexingForEssl (gate-only)", "probe", words, s);
            }

            // ---- the individual optimizer round trips ----
            if (isVs) {
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::LowerDrawParametersForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "LowerDrawParametersForEssl", "roundtrip", words, s);

                Stat s2 = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::SplitArrayVertexInputsForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "SplitArrayVertexInputsForEssl", "roundtrip", words, s2);

                Vector<uint32_t> split;
                ShaderCompiler::SplitArrayVertexInputsForEssl(mod, split, false);
                Stat s3 = Measure([&] { benchmarkSink(split != mod); }, 100.0);
                Emit(name, stage, "  (its 'did anything change' vector compare)", "roundtrip", words, s3);
            } else {
                // A pass whose Process() acquires the def-use manager BEFORE asking whether it
                // has anything to do, run on a stage that can never contain its builtins. The
                // delta over the zero-pass baseline is the cost of the analysis alone.
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::LowerDrawParametersForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "LowerDrawParametersForEssl (no-op stage: def-use cost)",
                     "roundtrip", words, s);
            }
            {
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "StripUboMemberRelaxedPrecisionForEssl", "roundtrip", words, s);
            }
            {
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::LowerRectImages(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "LowerRectImages", "roundtrip", words, s);
            }
            {
                // Armed on Adreno (integer multisample really is 1 while 4 is advertised).
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::ClampMultisampleFetchesForEssl(mod, out, 4, 1, 1, 4, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "ClampMultisampleFetchesForEssl (if armed)", "roundtrip", words, s);
            }
            {
                // Armed on a driver without GL_OES_viewport_array (Mali).
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::LowerViewportIndexForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "LowerViewportIndexForEssl (if armed)", "roundtrip", words, s);
            }
            {
                // Armed on a driver without GL_NV_shader_noperspective_interpolation.
                Stat s = Measure([&] {
                    Vector<uint32_t> out;
                    ShaderCompiler::EmulateNoPerspectiveForEssl(mod, out, false);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "EmulateNoPerspectiveForEssl (if armed)", "roundtrip", words, s);
            }

            // ---- SPIRV-Reflect (global-UBO routing) and SPIRV-Cross ----
            {
                Stat s = Measure([&] {
                    SpvcSession session(mod, SessionUsageBit::Reflection);
                    benchmarkSink(session.ParseMetaData());
                });
                Emit(name, stage, "SPIRV-Reflect ParseMetaData (UBO routing)", "crosscompile", words, s);
            }
            {
                Stat s = Measure([&] {
                    SpvcSession session(mod, SessionUsageBit::Transpile);
                    spvc_compiler_options options;
                    session.CreateOptions(&options);
                    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
                    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
                    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS,
                                                   SPVC_FALSE);
                    session.SetOptions(options);
                    const char* result = nullptr;
                    session.Compile(&result);
                    benchmarkSink(result != nullptr ? std::strlen(result) : 0);
                });
                Emit(name, stage, "SPIRV-Cross parse+emit ESSL", "crosscompile", words, s);
            }

            // ---- the whole backend chain, as Managers.cpp runs it on an Adreno-class
            //      driver: viewport gate OFF, multisample-clamp gate ON, noperspective
            //      supported, no XFB, no format-less image, no storage-block override.
            {
                Stat s = Measure([&] {
                    const Vector<Uint32>* eff = &mod;
                    Vector<uint32_t> a, b, c, d, e, f;
                    if (isVs && ShaderCompiler::LowerDrawParametersForEssl(*eff, a, false) && !a.empty())
                        eff = &a;
                    auto gates = ShaderCompiler::ProbeSpirvGateFeatures(*eff);
                    if (gates.DeclaresMultisampledImage &&
                        ShaderCompiler::ClampMultisampleFetchesForEssl(*eff, b, 4, 1, 1, 4, false) &&
                        !b.empty())
                        eff = &b;
                    if (isVs && ShaderCompiler::SplitArrayVertexInputsForEssl(*eff, c, false) &&
                        !c.empty() && c != *eff)
                        eff = &c;
                    if (ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(*eff, d, false) && !d.empty())
                        eff = &d;
                    if (ShaderCompiler::LowerRectImages(*eff, e, false) && !e.empty()) eff = &e;
                    if (ShaderCompiler::Lower1DArrayImagesForEssl(*eff, f, false) && !f.empty()) eff = &f;
                    Vector<uint32_t> g;
                    if (!isVs && ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(*eff, g, false) &&
                        !g.empty())
                        eff = &g;
                    benchmarkSink(eff->size());
                });
                Emit(name, stage, "TOTAL current SPIR-V chain (backend, Adreno)", "chain", words, s);
            }

            // ---- the same work, merged onto ONE Optimizer ----
            {
                Stat s = Measure([&] {
                    spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
                    opt.SetMessageConsumer(SilentConsumer());
                    if (isVs) {
                        opt.RegisterPass(LowerDrawParametersPass::CreateLowerDrawParametersPass());
                        opt.RegisterPass(SplitArrayVertexInputsPass::CreateSplitArrayVertexInputsPass());
                    }
                    opt.RegisterPass(ClampMultisampleFetchPass::CreateClampMultisampleFetchPass(4, 1, 1, 4));
                    opt.RegisterPass(
                        StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass());
                    opt.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());
                    spvtools::OptimizerOptions o;
                    o.set_run_validator(false);
                    Vector<uint32_t> out;
                    opt.Run(mod.data(), mod.size(), &out, o);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "TOTAL merged SPIR-V chain (one Run)", "merged", words, s);
            }
            // Same, plus the 1D-array-image lowering registered as an ordinary pass instead of
            // being reached through its own BuildModule gate.
            {
                Stat s = Measure([&] {
                    spvtools::Optimizer opt(SPV_ENV_VULKAN_1_1);
                    opt.SetMessageConsumer(SilentConsumer());
                    if (isVs) {
                        opt.RegisterPass(LowerDrawParametersPass::CreateLowerDrawParametersPass());
                        opt.RegisterPass(SplitArrayVertexInputsPass::CreateSplitArrayVertexInputsPass());
                    }
                    opt.RegisterPass(ClampMultisampleFetchPass::CreateClampMultisampleFetchPass(4, 1, 1, 4));
                    opt.RegisterPass(
                        StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass());
                    opt.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());
                    opt.RegisterPass(Lower1DArrayImagesPass::CreateLower1DArrayImagesPass());
                    spvtools::OptimizerOptions o;
                    o.set_run_validator(false);
                    Vector<uint32_t> out;
                    opt.Run(mod.data(), mod.size(), &out, o);
                    benchmarkSink(out.size());
                });
                Emit(name, stage, "TOTAL merged + 1D-array pass (one Run)", "merged", words, s);
            }
        }
    }

} // namespace

int main(int argc, char** argv) {
    // "--floor-loop" parses the same trivial shader forever, so an external sampler can
    // attribute the source-independent part of a glslang parse.
    const bool floorLoop = argc > 1 && std::strcmp(argv[1], "--floor-loop") == 0;

    // Times the first glslang parse of the process separately: glslang builds its built-in
    // symbol tables lazily, under a process-wide lock, on the first parse of each
    // (version, spvVersion, profile, source) combination.
    MobileGL::Initialize();

    const CompileEnv& env = *GetDefaultCompileEnv();
    {
        const auto t0 = Clock::now();
        String src = kCtsTestFs;
        PreprocessShaderSource(ShaderStage::Fragment, src, env);
        ShaderAttrib a{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = src, .flags = 0, .env = &env};
        auto r = ShaderCompiler::CompileShader(a);
        const auto t1 = Clock::now();
        std::printf("# glslang FIRST parse of the process (cold built-in tables): %.1f us (ok=%d)\n",
                    std::chrono::duration<double, std::micro>(t1 - t0).count(), r.has_value() ? 1 : 0);
    }
    ShaderCompiler::PrewarmBuiltins();

    if (floorLoop) {
        static const char* kEmpty330 = "#version 330 core\nvoid main() {}\n";
        String pre = kEmpty330;
        PreprocessShaderSource(ShaderStage::Vertex, pre, env);
        std::printf("# floor loop; sample me\n");
        std::fflush(stdout);
        for (;;) {
            ShaderAttrib a{
                .shaderType = GL_VERTEX_SHADER, .sourceStr = pre, .flags = 0, .env = &env};
            auto r = ShaderCompiler::CompileShader(a);
            benchmarkSink(r.has_value());
        }
    }

    std::printf("\n%-14s %-8s %-40s %-13s %7s %10s %10s %10s %6s\n", "corpus", "stage", "what",
                "kind", "words", "median_us", "p10_us", "p90_us", "reps");
    std::printf("%s\n", std::string(120, '-').c_str());

    // The per-parse FLOOR. glslang caches its version/profile built-in symbol table across
    // parses, but AddContextSpecificSymbols() - the resource-dependent half, gl_MaxVertexAttribs
    // and friends, generated from the TBuiltInResource we hand it - is rebuilt on every single
    // TShader::parse (ShaderLang.cpp: it is called unconditionally after the cached table is
    // adopted). An empty shader measures exactly that, with no user code to confound it.
    {
        static const char* kEmpty330 = "#version 330 core\nvoid main() {}\n";
        static const char* kEmpty460 = "#version 460\nvoid main() {}\n";
        for (const auto& [label, src] : {std::pair<const char*, const char*>{"330 core", kEmpty330},
                                         std::pair<const char*, const char*>{"460", kEmpty460}}) {
            String pre = src;
            PreprocessShaderSource(ShaderStage::Vertex, pre, env);
            Stat s = Measure([&] {
                ShaderAttrib a{
                    .shaderType = GL_VERTEX_SHADER, .sourceStr = pre, .flags = 0, .env = &env};
                auto r = ShaderCompiler::CompileShader(a);
                benchmarkSink(r.has_value());
            });
            Emit("floor", "VS", std::string("glslang.parse of empty main(), #version ") + label,
                 "frontend", 0, s);
        }
    }

    // What that floor is made of. glslang caches the version/profile built-in symbol table
    // (SharedSymbolTables) but calls AddContextSpecificSymbols() unconditionally on every
    // TShader::parse (ShaderLang.cpp), and that constructs a TBuiltIns, GENERATES the
    // resource-dependent built-in declarations as GLSL text from the TBuiltInResource, and
    // then PARSES that text into a fresh symbol-table level. Only the generate half can be
    // timed from outside; the rest of the floor is the re-parse of the text it produced.
    {
        const TBuiltInResource resources = BuildTBuiltInResource(&env);
        glslang::SpvVersion spvVersion;
        spvVersion.spv = 0x00010300;
        spvVersion.vulkan = 100;
        glslang::TPoolAllocator pool;
        glslang::SetThreadPoolAllocator(&pool);
        size_t commonBytes = 0;
        size_t stageBytes = 0;
        Stat s = MeasureInner(
            [&]() -> double {
                pool.push();
                const auto t0 = Clock::now();
                glslang::TBuiltIns builtIns;
                builtIns.initialize(resources, 330, ECoreProfile, spvVersion, EShLangVertex);
                const auto t1 = Clock::now();
                commonBytes = builtIns.getCommonString().size();
                stageBytes = builtIns.getStageString(EShLangVertex).size();
                const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                pool.pop();
                return us;
            },
            150.0, 15, 400);
        glslang::SetThreadPoolAllocator(nullptr);
        Emit("floor", "VS", "  of which: TBuiltIns::initialize (generate only)", "frontend", 0, s);
        std::printf("#   generated built-in declaration text: common %zu bytes, vertex stage %zu bytes\n",
                    commonBytes, stageBytes);
    }

    // Splitting the floor further: how much of it is object set-up that never touches the
    // source, and how much is MobileGL's particular glslang configuration rather than a parse
    // as such.
    {
        static const char* kEmpty330 = "#version 330 core\nvoid main() {}\n";
        const TBuiltInResource resources = BuildTBuiltInResource(&env);

        Stat ctor = Measure([&] {
            auto sh = MakeShared<glslang::TShader>(EShLangVertex);
            const char* src[] = {kEmpty330};
            sh->setStrings(src, 1);
            sh->setNanMinMaxClamp(true);
            sh->setInvertY(true);
            sh->setPreamble("#undef VULKAN\n");
            sh->setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan, 450);
            sh->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
            sh->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
            sh->setEnvInputVulkanRulesRelaxed();
            sh->setAutoMapLocations(true);
            sh->setAutoMapBindings(true);
            sh->setGlobalUniformBlockName(GLOBAL_UBO_NAME);
            benchmarkSink(sh != nullptr);
        });
        Emit("floor", "VS", "  TShader ctor+configure, NO parse()", "frontend", 0, ctor);

        Stat bare = Measure([&] {
            auto sh = MakeShared<glslang::TShader>(EShLangVertex);
            const char* src[] = {kEmpty330};
            sh->setStrings(src, 1);
            TBuiltInResource r = resources;
            benchmarkSink(sh->parse(&r, 330, ECoreProfile, false, true, EShMsgDefault));
        });
        Emit("floor", "VS", "  parse() with NO MobileGL env configuration", "frontend", 0, bare);
    }

    std::printf("\n%-14s %-8s %-40s %-13s %7s %10s %10s %10s %6s\n", "corpus", "stage", "what",
                "kind", "words", "median_us", "p10_us", "p90_us", "reps");
    std::printf("%s\n", std::string(120, '-').c_str());

    ProfileCorpus("tiny", kTinyVs, kTinyFs);
    ProfileCorpus("cts_fs_tested", kCtsBlankVs, kCtsTestFs);
    ProfileCorpus("cts_vs_tested", kCtsTestVs, kCtsBlankFs);
    ProfileCorpus("iris_large", kBigVs, kBigFs);

    return 0;
}
