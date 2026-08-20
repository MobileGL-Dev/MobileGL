// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/IterationRPProgram203Scenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Full iterationRP Program 203 golden input/output fixture. The original shader
// consumes deterministic complete textures and uniforms, then its complete
// 512x513 RG16F output image is compared against fixed half-float golden bits.
// This catches both a wrong exposure slot and collateral scratch corruption.

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {
        constexpr int kSceneWidth = 854;
        constexpr int kSceneHeight = 480;
        constexpr int kPixelDataWidth = 512;
        constexpr int kPixelDataHeight = 513;
        constexpr std::size_t kSceneTexelCount =
            static_cast<std::size_t>(kSceneWidth) * kSceneHeight;
        constexpr std::size_t kPixelDataTexelCount =
            static_cast<std::size_t>(kPixelDataWidth) * kPixelDataHeight;

        struct Rgba32f {
            float r, g, b, a;
        };

        struct Rg16 {
            std::uint16_t r, g;
        };

        static_assert(sizeof(Rgba32f) == 16);
        static_assert(sizeof(Rg16) == 4);

        // Captured from the fixed fixture on Adreno 830. These are the exact
        // RG16F storage bits for (0.806640625, 8.2578125), not rounded decimal
        // comparisons performed by the test.
        constexpr Rg16 kGoldenExposure = {0x3a74u, 0x4821u};

        constexpr const char* kCommonSource = R"glsl(
#version 430 core
#extension GL_KHR_shader_subgroup_arithmetic : require

uniform int frameCounter;
uniform float frameTime;
uniform float aspectRatio;
uniform vec2 pixelSize;
uniform float nightVision;
uniform float darknessLightFactor;
uniform sampler2D colortex2;
uniform sampler2D pixelData2D;
layout(rg16f) uniform image2D img_pixelData2D;

float remapSaturate(float x, float e0, float e1) {
    return clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
}

float GetExposureValue(float luminance) {
    float aeCurve = 0.65f;
    aeCurve = mix(aeCurve, clamp(aeCurve * 1.2f, 0.0f, 1.0f), nightVision);
    aeCurve *= remapSaturate(luminance, 2.0f, 1.0f) * 0.6f + 0.4f;
    float ae = pow(luminance, -aeCurve);
    ae *= 1.0f - min(darknessLightFactor * 2.0f, 0.9f);
    ae *= 8.5f;
    return ae;
}
)glsl";

        constexpr const char* kOriginalMain = R"glsl(
layout(local_size_x = 32, local_size_y = 16) in;
shared vec2 prefixSumCache[32];

void main() {
    vec2 texCoord = (vec2(gl_GlobalInvocationID.xy) + 0.5f) * vec2(1.0f / 32.0f, 1.0f / 16.0f);
    vec2 sampleCoord = texCoord * (1.0f / 64.0f);
    sampleCoord.x += (15.0f / 32.0f) + pixelSize.x * 12.0f;
    float tileExposure = dot(textureLod(colortex2, sampleCoord, 0.0f).rgb,
                             vec3(0.2125f, 0.7154f, 0.0721f));
    vec2 sampleLuminance = vec2(tileExposure, 0.0f);
    sampleLuminance = subgroupInclusiveAdd(sampleLuminance);
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleLuminance;
    barrier();

    uint loopLength = uint(findMSB(gl_NumSubgroups));
    loopLength += uint(gl_NumSubgroups - (1u << (loopLength - 1u)) > 0u);
    for (uint i = 0u; i < loopLength; ++i) {
        if ((gl_SubgroupID & (1u << i)) > 0u) {
            sampleLuminance += prefixSumCache[(gl_SubgroupID >> i << i) - 1u];
            if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
                prefixSumCache[gl_SubgroupID] = sampleLuminance;
        }
        barrier();
    }
    if (gl_LocalInvocationIndex == 511u)
        prefixSumCache[0] = sampleLuminance / 512.0f;
    barrier();

    float avg = prefixSumCache[0].x;
    vec2 tileDistance = texCoord * 2.0f - 1.0f;
    tileDistance.y /= aspectRatio;
    float centerDistance = length(tileDistance);
    float tileWeight = remapSaturate(centerDistance, 0.6f, 0.4f);
    tileExposure = max(7.0E-7f, tileExposure);
    float lumaWeight = avg / tileExposure;
    lumaWeight = pow(lumaWeight, remapSaturate(avg, 0.02f, 0.001f) * 0.4f + 0.2f);
    tileWeight *= lumaWeight;

    vec2 sampleExposure = vec2(tileExposure * tileWeight, tileWeight);
    sampleExposure = subgroupInclusiveAdd(sampleExposure);
    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleExposure;
    barrier();
    for (uint i = 0u; i < loopLength; ++i) {
        if ((gl_SubgroupID & (1u << i)) > 0u) {
            sampleExposure += prefixSumCache[(gl_SubgroupID >> i << i) - 1u];
            if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
                prefixSumCache[gl_SubgroupID] = sampleExposure;
        }
        barrier();
    }

    if (gl_LocalInvocationIndex == 511u) {
        float avgExposure = max(sampleExposure.x / sampleExposure.y * 29.3f, 1.0E-10f);
        avgExposure = log2(avgExposure);
        float prevAvgExposure = log2(texelFetch(pixelData2D, ivec2(0, 0), 0).x);
        float frameTimeFixed = frameTime + step(frameCounter, 20) * 100.0f;
        float exposureTime = clamp(frameTimeFixed * 2.0f, 0.0f, 1.0f);
        avgExposure = mix(prevAvgExposure, avgExposure, exposureTime);
        avgExposure = max(exp2(avgExposure), 1.0E-5f);
        float exposure = GetExposureValue(avgExposure);
        imageStore(img_pixelData2D, ivec2(0, 0), vec4(avgExposure, exposure, 0.0f, 0.0f));
    }
}
)glsl";

        GLuint CompileCompute(const char* mainSource, std::string* error) {
            const std::array<const GLchar*, 2> sources = {kCommonSource, mainSource};
            const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(shader, static_cast<GLsizei>(sources.size()), sources.data(), nullptr);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled != GL_TRUE) {
                std::array<char, 8192> log{};
                glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size() - 1), nullptr, log.data());
                *error = log.data();
                glDeleteShader(shader);
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, shader);
            glLinkProgram(program);
            glDeleteShader(shader);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                std::array<char, 8192> log{};
                glGetProgramInfoLog(program, static_cast<GLsizei>(log.size() - 1), nullptr, log.data());
                *error = log.data();
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        std::vector<Rgba32f> MakeSceneInput() {
            std::vector<Rgba32f> texels(kSceneTexelCount);
            for (int y = 0; y < kSceneHeight; ++y) {
                for (int x = 0; x < kSceneWidth; ++x) {
                    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9e3779b9u;
                    h ^= static_cast<std::uint32_t>(y) * 0x85ebca6bu;
                    h ^= h >> 16u;
                    h *= 0x7feb352du;
                    h ^= h >> 15u;
                    const float noise = static_cast<float>(h & 0xffffu) / 65535.0f;
                    float base = 0.0002f + noise * 0.075f;
                    const float dx = static_cast<float>(x - 420);
                    const float dy = static_cast<float>(y - 4);
                    base += 0.65f * std::exp(-(dx * dx + dy * dy) / 18.0f);
                    if (((x + y * 17) % 113) == 0) base += 1.75f;
                    texels[static_cast<std::size_t>(y) * kSceneWidth + x] =
                        {base * 0.83f, base * 1.07f, base * 1.31f, 1.0f};
                }
            }
            return texels;
        }

        std::uint16_t FloatToHalf(float value) {
            const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
            const std::uint32_t sign = (bits >> 16u) & 0x8000u;
            const std::uint32_t exponent = (bits >> 23u) & 0xffu;
            std::uint32_t mantissa = bits & 0x7fffffu;

            if (exponent == 0xffu) {
                return static_cast<std::uint16_t>(sign | (mantissa == 0 ? 0x7c00u : 0x7e00u));
            }
            int halfExponent = static_cast<int>(exponent) - 127 + 15;
            if (halfExponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
            if (halfExponent <= 0) {
                if (halfExponent < -10) return static_cast<std::uint16_t>(sign);
                mantissa |= 0x800000u;
                const unsigned shift = static_cast<unsigned>(14 - halfExponent);
                const std::uint32_t rounded = mantissa + ((1u << (shift - 1u)) - 1u) +
                                              ((mantissa >> shift) & 1u);
                return static_cast<std::uint16_t>(sign | (rounded >> shift));
            }
            mantissa += 0xfffu + ((mantissa >> 13u) & 1u);
            if ((mantissa & 0x800000u) != 0) {
                mantissa = 0;
                if (++halfExponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
            }
            return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(halfExponent) << 10u) |
                                              (mantissa >> 13u));
        }

        std::vector<Rg16> MakePixelDataInput() {
            std::vector<Rg16> texels(kPixelDataTexelCount);
            for (std::size_t i = 0; i < texels.size(); ++i) {
                texels[i] = {FloatToHalf(0.35f + static_cast<float>(i % 97u) * 0.0025f),
                             FloatToHalf(-0.45f + static_cast<float>(i % 89u) * 0.01f)};
            }
            texels[0] = {FloatToHalf(0.73f), FloatToHalf(1.25f)};
            return texels;
        }

        std::vector<Rg16> MakeGoldenOutput() {
            std::vector<Rg16> golden = MakePixelDataInput();
            golden[0] = kGoldenExposure;
            return golden;
        }

        GLuint MakeTexture(GLenum internalFormat, GLenum format, GLenum type, int width, int height,
                           const void* data) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format,
                         type, data);
            return texture;
        }

        void BindAndDispatch(GLuint program, GLuint scene, GLuint pixelData) {
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, scene);
            glUniform1i(glGetUniformLocation(program, "colortex2"), 3);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, pixelData);
            glUniform1i(glGetUniformLocation(program, "pixelData2D"), 4);
            glBindImageTexture(0, pixelData, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RG16F);
            glUniform1i(glGetUniformLocation(program, "img_pixelData2D"), 0);
            glUniform1i(glGetUniformLocation(program, "frameCounter"), 100);
            glUniform1f(glGetUniformLocation(program, "frameTime"), 1.0f / 60.0f);
            glUniform1f(glGetUniformLocation(program, "aspectRatio"),
                        static_cast<float>(kSceneWidth) / kSceneHeight);
            glUniform2f(glGetUniformLocation(program, "pixelSize"), 1.0f / kSceneWidth, 1.0f / kSceneHeight);
            glUniform1f(glGetUniformLocation(program, "nightVision"), 0.23f);
            glUniform1f(glGetUniformLocation(program, "darknessLightFactor"), 0.08f);
            glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }

        std::vector<Rg16> ReadWholeRgTexture(GLuint texture) {
            std::vector<Rg16> texels(kPixelDataTexelCount);
            glBindTexture(GL_TEXTURE_2D, texture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_HALF_FLOAT, texels.data());
            return texels;
        }

        class IterationRPProgram203Scenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                GLint stages = 0;
                GLint features = 0;
                GLint invocations = 0;
                glGetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &stages);
                glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
                glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
                const GLbitfield required =
                    GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR;
                if ((static_cast<GLbitfield>(stages) & GL_COMPUTE_SHADER_BIT) == 0 ||
                    (static_cast<GLbitfield>(features) & required) != required || invocations < 512) {
                    GTEST_SKIP() << "requires 512-invocation basic+arithmetic compute subgroups";
                }

                std::string error;
                m_original = CompileCompute(kOriginalMain, &error);
                ASSERT_NE(m_original, 0u) << "original Program 203: " << error;

                const std::vector<Rgba32f> scene = MakeSceneInput();
                const std::vector<Rg16> pixelData = MakePixelDataInput();
                m_scene = MakeTexture(GL_RGBA16F, GL_RGBA, GL_FLOAT, kSceneWidth, kSceneHeight, scene.data());
                m_originalOutput =
                    MakeTexture(GL_RG16F, GL_RG, GL_HALF_FLOAT, kPixelDataWidth, kPixelDataHeight,
                                pixelData.data());
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
            }

            void TearDown() override {
                if (!Ready()) return;
                const std::array<GLuint, 2> textures = {m_scene, m_originalOutput};
                glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
                if (m_original != 0) glDeleteProgram(m_original);
            }

            GLuint m_original = 0;
            GLuint m_scene = 0;
            GLuint m_originalOutput = 0;
        };
    } // namespace

    TEST_F(IterationRPProgram203Scenario, FixedCompleteInputProducesFixedCompleteGoldenOutput) {
        if (!Ready()) return;

        BindAndDispatch(m_original, m_scene, m_originalOutput);
        glFinish();
        const std::vector<Rg16> actual = ReadWholeRgTexture(m_originalOutput);
        const std::vector<Rg16> expected = MakeGoldenOutput();
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));

        std::size_t mismatchTexels = 0;
        std::size_t firstMismatch = actual.size();
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (actual[i].r != expected[i].r || actual[i].g != expected[i].g) {
                if (firstMismatch == actual.size()) firstMismatch = i;
                ++mismatchTexels;
            }
        }

        RecordProperty("program203_output_width", kPixelDataWidth);
        RecordProperty("program203_output_height", kPixelDataHeight);
        RecordProperty("program203_compared_texels", static_cast<long long>(actual.size()));
        RecordProperty("program203_mismatch_texels", static_cast<long long>(mismatchTexels));
        std::cout << "IterationRPProgram203Scenario complete-output actualExposureBits=(0x" << std::hex
                  << actual[0].r << ", 0x" << actual[0].g << ") goldenExposureBits=(0x" << expected[0].r
                  << ", 0x" << expected[0].g << std::dec << ") mismatches=" << mismatchTexels << '/'
                  << actual.size() << '\n';

        if (firstMismatch != actual.size()) {
            const std::size_t x = firstMismatch % kPixelDataWidth;
            const std::size_t y = firstMismatch / kPixelDataWidth;
            ADD_FAILURE() << "complete Program 203 output differs at " << x << ',' << y
                          << ": actual half bits=(0x" << std::hex << actual[firstMismatch].r << ", 0x"
                          << actual[firstMismatch].g << ") golden half bits=(0x" << expected[firstMismatch].r
                          << ", 0x" << expected[firstMismatch].g << std::dec << "); mismatched "
                          << mismatchTexels << " of " << actual.size() << " texels";
        }
        EXPECT_EQ(mismatchTexels, 0u);
    }
} // namespace MGITest
