// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/Program203FirstReductionScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - PROGRAM 203'S FIRST SUBGROUP REDUCTION.
//
// Program 203 reduces a 32 x 16 exposure tile with a vector subgroup inclusive add,
// then a shared-memory scan of subgroup totals.  The source assumes that every
// subgroup has a last lane, that there are 2..32 subgroups, and that local index
// 511 belongs to the last subgroup and its last lane.  Those are source assumptions,
// not API contracts.  This probe intentionally does not repair them: it records the
// observed topology and makes each handoff independently observable.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
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

        constexpr std::size_t kInvocationCount = 512;
        constexpr std::size_t kScanStageCount = 6;
        constexpr std::uint32_t kQuietNanBits = 0x7fc00000u;
        constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

        struct UVec4 {
            std::uint32_t x;
            std::uint32_t y;
            std::uint32_t z;
            std::uint32_t w;
        };

        struct Vec4 {
            float x;
            float y;
            float z;
            float w;
        };

        // Matches the std430 block exactly. uvec4/vec4 arrays have a 16-byte
        // stride, floats are a dense scalar array, and the outer scan array is
        // stage-major in both GLSL and C++.
        struct ProbeOutput {
            std::array<UVec4, kInvocationCount> invocation;
            std::array<UVec4, kInvocationCount> subgroup;
            std::array<Vec4, kInvocationCount> reduction;
            std::array<float, kInvocationCount> finalAverage;
            std::array<std::array<float, kInvocationCount>, kScanStageCount> scanAfter;
        };

        static_assert(sizeof(UVec4) == 16);
        static_assert(sizeof(Vec4) == 16);
        static_assert(std::is_standard_layout_v<ProbeOutput>);
        static_assert(offsetof(ProbeOutput, invocation) == 0);
        static_assert(offsetof(ProbeOutput, subgroup) == 8192);
        static_assert(offsetof(ProbeOutput, reduction) == 16384);
        static_assert(offsetof(ProbeOutput, finalAverage) == 24576);
        static_assert(offsetof(ProbeOutput, scanAfter) == 26624);
        static_assert(sizeof(ProbeOutput) == 38912);

        enum class InputMode {
            SampledRgba32f,
            IndexedSsbo,
        };

        const char* InputModeName(InputMode mode) {
            return mode == InputMode::SampledRgba32f ? "sampled RGBA32F" : "indexed SSBO";
        }

        std::uint32_t FloatBits(float value) {
            return std::bit_cast<std::uint32_t>(value);
        }

        bool SameBits(float lhs, float rhs) {
            return FloatBits(lhs) == FloatBits(rhs);
        }

        bool IsQuietNanSentinel(float value) {
            return FloatBits(value) == kQuietNanBits;
        }

        bool DrainGlErrors() {
            bool hadError = false;
            while (glGetError() != GL_NO_ERROR) hadError = true;
            return hadError;
        }

        bool HasExtension(const char* wanted) {
            GLint extensionCount = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
            for (GLint i = 0; i < extensionCount; ++i) {
                const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                if (extension != nullptr && std::string(extension) == wanted) return true;
            }
            return false;
        }

        struct CapabilityInfo {
            bool subgroupExtension = false;
            GLint subgroupSize = 0;
            GLint supportedStages = 0;
            GLint supportedFeatures = 0;
            GLint maxComputeStorageBlocks = 0;
            GLint maxStorageBindings = 0;
            GLint maxWorkGroupInvocations = 0;
            std::array<GLint, 3> maxWorkGroupSize{};
            bool queryHadError = false;

            bool SupportsProbe() const {
                const auto stages = static_cast<GLbitfield>(supportedStages);
                const auto features = static_cast<GLbitfield>(supportedFeatures);
                return !queryHadError && subgroupExtension &&
                       (stages & GL_COMPUTE_SHADER_BIT) != 0 &&
                       (features & (GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR)) ==
                           (GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR) &&
                       maxComputeStorageBlocks >= 2 && maxStorageBindings >= 2 &&
                       maxWorkGroupInvocations >= static_cast<GLint>(kInvocationCount) && maxWorkGroupSize[0] >= 32 &&
                       maxWorkGroupSize[1] >= 16 && maxWorkGroupSize[2] >= 1;
            }

            std::string MissingRequirements() const {
                std::vector<std::string> missing;
                const auto stages = static_cast<GLbitfield>(supportedStages);
                const auto features = static_cast<GLbitfield>(supportedFeatures);
                if (queryHadError) missing.emplace_back("a subgroup/compute capability query generated GL error");
                if (!subgroupExtension) missing.emplace_back("GL_KHR_shader_subgroup");
                if ((stages & GL_COMPUTE_SHADER_BIT) == 0) {
                    missing.emplace_back("GL_COMPUTE_SHADER_BIT in GL_SUBGROUP_SUPPORTED_STAGES_KHR");
                }
                const auto requiredFeatures =
                    GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR;
                if ((features & requiredFeatures) != requiredFeatures) {
                    missing.emplace_back("basic|arithmetic in GL_SUBGROUP_SUPPORTED_FEATURES_KHR");
                }
                if (maxComputeStorageBlocks < 2 || maxStorageBindings < 2) {
                    missing.emplace_back("two compute SSBO bindings");
                }
                if (maxWorkGroupInvocations < static_cast<GLint>(kInvocationCount) || maxWorkGroupSize[0] < 32 ||
                    maxWorkGroupSize[1] < 16 || maxWorkGroupSize[2] < 1) {
                    missing.emplace_back("a 32x16x1 / 512-invocation compute workgroup");
                }

                std::ostringstream message;
                for (std::size_t i = 0; i < missing.size(); ++i) {
                    if (i != 0) message << ", ";
                    message << missing[i];
                }
                return message.str();
            }
        };

        CapabilityInfo QueryCapabilities() {
            CapabilityInfo info;
            DrainGlErrors();
            info.subgroupExtension = HasExtension("GL_KHR_shader_subgroup");
            glGetIntegerv(GL_SUBGROUP_SIZE_KHR, &info.subgroupSize);
            glGetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &info.supportedStages);
            glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &info.supportedFeatures);
            glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &info.maxComputeStorageBlocks);
            glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &info.maxStorageBindings);
            glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &info.maxWorkGroupInvocations);
            for (GLuint axis = 0; axis < info.maxWorkGroupSize.size(); ++axis) {
                glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, axis, &info.maxWorkGroupSize[axis]);
            }
            info.queryHadError = DrainGlErrors();
            return info;
        }

        void PrintMetadata(const CapabilityInfo& info, std::ostream& output) {
            output << "Program203FirstReductionScenario metadata: "
                   << "GL_SUBGROUP_SIZE_KHR=" << info.subgroupSize
                   << ", GL_SUBGROUP_SUPPORTED_STAGES_KHR=0x" << std::hex
                   << static_cast<GLbitfield>(info.supportedStages)
                   << ", GL_SUBGROUP_SUPPORTED_FEATURES_KHR=0x"
                   << static_cast<GLbitfield>(info.supportedFeatures) << std::dec
                   << ", subgroupExtension=" << info.subgroupExtension
                   << ", GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS=" << info.maxComputeStorageBlocks
                   << ", GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS=" << info.maxStorageBindings
                   << ", GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS=" << info.maxWorkGroupInvocations
                   << ", GL_MAX_COMPUTE_WORK_GROUP_SIZE=" << info.maxWorkGroupSize[0] << 'x'
                   << info.maxWorkGroupSize[1] << 'x' << info.maxWorkGroupSize[2]
                   << ", queryHadError=" << info.queryHadError << '\n';
        }

        bool DumpRequested() {
            const char* value = std::getenv("MOBILEGL_ITEST_SUBGROUP_PROBE_DUMP");
            return value != nullptr && std::string(value) == "1";
        }

        constexpr const char* kShaderPreamble = R"(#version 430 core
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require

layout(local_size_x = 32, local_size_y = 16, local_size_z = 1) in;

layout(std430, binding = 1) buffer SubgroupProbeOutput {
    uvec4 invocation[512];
    uvec4 subgroup[512];
    vec4 reduction[512];
    float finalAverage[512];
    float scanAfter[6][512];
} outProbe;

shared vec2 prefixSumCache[32];
)";

        constexpr const char* kSampledInput = R"(
uniform sampler2D colortex2;
uniform vec2 pixelSize;
)";

        constexpr const char* kIndexedInput = R"(
layout(std430, binding = 0) readonly buffer Input {
    float value[512];
} inputData;
)";

        // Only the expression producing tileExposure differs between the two
        // tests. The remainder is the program-203 first reduction, with stores
        // placed after its existing barriers to expose each handoff.
        constexpr const char* kSampledTileExposure = R"(
    vec2 texCoord = (vec2(gl_GlobalInvocationID.xy) + 0.5) *
                    vec2(1.0 / 32.0, 1.0 / 16.0);
    vec2 sampleCoord = texCoord * (1.0 / 64.0);
    sampleCoord.x += (15.0 / 32.0) + pixelSize.x * 12.0;

    float tileExposure = dot(
        textureLod(colortex2, sampleCoord, 0.0).rgb,
        vec3(0.2125, 0.7154, 0.0721));
)";

        constexpr const char* kIndexedTileExposure = R"(
    float tileExposure = inputData.value[gl_LocalInvocationIndex];
)";

        constexpr const char* kReductionBody = R"(
    vec2 sampleLuminance = vec2(tileExposure, 0.0);
    sampleLuminance = subgroupInclusiveAdd(sampleLuminance);
    float nativeInclusive = sampleLuminance.x;

    // This is a uniform, safety-only branch: it leaves an invalid source
    // contract visible without indexing past the 32-entry cache or underflowing
    // loopLength - 1. It is deliberately a failure on the CPU, not a skip.
    bool sourceDomain = gl_NumSubgroups >= 2u && gl_NumSubgroups <= 32u;
    if (!sourceDomain) {
        float qNaN = uintBitsToFloat(0x7fc00000u);
        uint localIndex = gl_LocalInvocationIndex;
        outProbe.invocation[localIndex] = uvec4(localIndex, gl_LocalInvocationID);
        outProbe.subgroup[localIndex] = uvec4(gl_SubgroupSize, gl_NumSubgroups, gl_SubgroupID,
                                               gl_SubgroupInvocationID);
        outProbe.reduction[localIndex] = vec4(tileExposure, nativeInclusive, qNaN, qNaN);
        outProbe.finalAverage[localIndex] = qNaN;
        for (uint stage = 0u; stage < 6u; ++stage)
            outProbe.scanAfter[stage][localIndex] = qNaN;
        return;
    }

    if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
        prefixSumCache[gl_SubgroupID] = sampleLuminance;
    barrier();

    float sourceRawSubtotal = prefixSumCache[gl_SubgroupID].x;

    uint loopLength = uint(findMSB(gl_NumSubgroups));
    loopLength += uint(gl_NumSubgroups - (1u << (loopLength - 1u)) > 0u);

    for (uint scanStage = 0u; scanStage < loopLength; ++scanStage) {
        if ((gl_SubgroupID & (1u << scanStage)) > 0u) {
            sampleLuminance += prefixSumCache[(gl_SubgroupID >> scanStage << scanStage) - 1u];
            if (gl_SubgroupInvocationID == gl_SubgroupSize - 1u)
                prefixSumCache[gl_SubgroupID] = sampleLuminance;
        }
        barrier();
        outProbe.scanAfter[scanStage][gl_LocalInvocationIndex] = sampleLuminance.x;
    }

    float sourceMergedPrefix = sampleLuminance.x;

    if (gl_LocalInvocationIndex == 511u)
        prefixSumCache[0] = sampleLuminance / 512.0;
    barrier();

    float avg = prefixSumCache[0].x;

    uint localIndex = gl_LocalInvocationIndex;
    outProbe.invocation[localIndex] = uvec4(localIndex, gl_LocalInvocationID);
    outProbe.subgroup[localIndex] = uvec4(gl_SubgroupSize, gl_NumSubgroups, gl_SubgroupID,
                                           gl_SubgroupInvocationID);
    outProbe.reduction[localIndex] = vec4(tileExposure, nativeInclusive, sourceRawSubtotal, sourceMergedPrefix);
    outProbe.finalAverage[localIndex] = avg;
}
)";

        std::string BuildProbeShader(InputMode mode) {
            std::string source = kShaderPreamble;
            source += mode == InputMode::SampledRgba32f ? kSampledInput : kIndexedInput;
            source += "\nvoid main() {\n";
            source += mode == InputMode::SampledRgba32f ? kSampledTileExposure : kIndexedTileExposure;
            source += kReductionBody;
            return source;
        }

        std::string FormatFloat(float value) {
            std::ostringstream text;
            text << std::hexfloat << value;
            return text.str();
        }

        struct ValidationResult {
            bool ok = true;
            std::string phase;
            std::string message;
            bool scanStageMismatch = false;
            int scanStage = -1;
            bool ownerEvaluated = false;
            bool index511IsSourceLastLaneWriter = false;
            bool index511IsHighestSubgroupMember = false;
            std::uint32_t highestObservedSubgroup = 0;
        };

        ValidationResult Failure(std::string phase, std::string message) {
            ValidationResult result;
            result.ok = false;
            result.phase = std::move(phase);
            result.message = std::move(message);
            return result;
        }

        constexpr float kSampledLuminance = 0.2125f + 0.7154f + 0.0721f;

        float ExpectedInput(InputMode mode, std::uint32_t localIndex) {
            return mode == InputMode::SampledRgba32f ? kSampledLuminance : static_cast<float>(localIndex + 1u);
        }

        ValidationResult ValidateProbe(const ProbeOutput& output, InputMode mode) {
            std::array<std::size_t, kInvocationCount> slotForLocal{};
            slotForLocal.fill(kNoSlot);

            // 1. Record identity. Slots are only used to locate each reported
            // local index; all subgroup behavior below groups recorded IDs/lanes.
            for (std::size_t slot = 0; slot < kInvocationCount; ++slot) {
                const std::uint32_t localIndex = output.invocation[slot].x;
                if (localIndex >= kInvocationCount) {
                    std::ostringstream message;
                    message << "output slot " << slot << " reports localIndex " << localIndex << " outside [0, 511]";
                    return Failure("record identity", message.str());
                }
                if (slotForLocal[localIndex] != kNoSlot) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " appears in output slots " << slotForLocal[localIndex]
                            << " and " << slot;
                    return Failure("record identity", message.str());
                }
                slotForLocal[localIndex] = slot;
            }
            for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                if (slotForLocal[localIndex] == kNoSlot) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " is missing from all 512 records";
                    return Failure("record identity", message.str());
                }
            }
            for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                const std::size_t slot = slotForLocal[localIndex];
                const UVec4& invocation = output.invocation[slot];
                const std::uint32_t expectedX = static_cast<std::uint32_t>(localIndex % 32u);
                const std::uint32_t expectedY = static_cast<std::uint32_t>(localIndex / 32u);
                if (invocation.y != expectedX || invocation.z != expectedY || invocation.w != 0u) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " reports local invocation (" << invocation.y << ','
                            << invocation.z << ',' << invocation.w << "), expected (" << expectedX << ',' << expectedY
                            << ",0)";
                    return Failure("record identity", message.str());
                }
                const float expectedInput = ExpectedInput(mode, static_cast<std::uint32_t>(localIndex));
                const float actualInput = output.reduction[slot].x;
                if (!SameBits(actualInput, expectedInput)) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " input was " << FormatFloat(actualInput) << ", expected "
                            << FormatFloat(expectedInput);
                    return Failure("input", message.str());
                }
            }

            // 2. Observed topology. Do not derive lanes or subgroup membership
            // from local invocation indices: only the values the shader recorded
            // participate in grouping.
            const std::uint32_t reportedNumSubgroups = output.subgroup[slotForLocal[0]].y;
            if (reportedNumSubgroups == 0u) {
                return Failure("observed topology", "localIndex 0 reported gl_NumSubgroups == 0");
            }
            if (reportedNumSubgroups > kInvocationCount) {
                std::ostringstream message;
                message << "reported gl_NumSubgroups=" << reportedNumSubgroups
                        << " exceeds the 512 recorded invocations, so at least one subgroup ID is missing";
                return Failure("observed topology", message.str());
            }
            std::vector<std::vector<std::size_t>> subgroupSlots(reportedNumSubgroups);
            for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                const std::size_t slot = slotForLocal[localIndex];
                const UVec4& subgroup = output.subgroup[slot];
                if (subgroup.x == 0u || subgroup.y == 0u) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " reported subgroupSize=" << subgroup.x
                            << ", numSubgroups=" << subgroup.y;
                    return Failure("observed topology", message.str());
                }
                if (subgroup.y != reportedNumSubgroups) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " reported numSubgroups=" << subgroup.y
                            << ", while localIndex 0 reported " << reportedNumSubgroups;
                    return Failure("observed topology", message.str());
                }
                if (subgroup.z >= reportedNumSubgroups) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " reported subgroupID=" << subgroup.z
                            << " outside [0, " << (reportedNumSubgroups - 1u) << ']';
                    return Failure("observed topology", message.str());
                }
                if (subgroup.w >= subgroup.x) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " reported laneID=" << subgroup.w
                            << " outside its subgroupSize=" << subgroup.x;
                    return Failure("observed topology", message.str());
                }
                subgroupSlots[subgroup.z].push_back(slot);
            }
            for (std::uint32_t subgroupID = 0; subgroupID < reportedNumSubgroups; ++subgroupID) {
                if (subgroupSlots[subgroupID].empty()) {
                    std::ostringstream message;
                    message << "reported gl_NumSubgroups=" << reportedNumSubgroups
                            << " but subgroupID " << subgroupID << " has no recorded members";
                    return Failure("observed topology", message.str());
                }
                auto& members = subgroupSlots[subgroupID];
                std::sort(members.begin(), members.end(), [&output](std::size_t lhs, std::size_t rhs) {
                    return output.subgroup[lhs].w < output.subgroup[rhs].w;
                });
                for (std::size_t i = 1; i < members.size(); ++i) {
                    if (output.subgroup[members[i - 1]].w == output.subgroup[members[i]].w) {
                        std::ostringstream message;
                        message << "subgroupID " << subgroupID << " contains duplicate laneID "
                                << output.subgroup[members[i]].w;
                        return Failure("observed topology", message.str());
                    }
                }
            }

            // 3. Native subgroup arithmetic, in the actual lane ordering emitted
            // by the driver. The fixture values and all partial sums are exactly
            // representable binary32 values, so compare representation, not epsilon.
            std::array<float, kInvocationCount> nativePrefix{};
            std::vector<float> nativeSubtotal(reportedNumSubgroups, 0.0f);
            for (std::uint32_t subgroupID = 0; subgroupID < reportedNumSubgroups; ++subgroupID) {
                float inclusive = 0.0f;
                for (const std::size_t slot : subgroupSlots[subgroupID]) {
                    const std::uint32_t localIndex = output.invocation[slot].x;
                    inclusive += ExpectedInput(mode, localIndex);
                    nativePrefix[slot] = inclusive;
                    const float actualNative = output.reduction[slot].y;
                    if (!SameBits(actualNative, inclusive)) {
                        std::ostringstream message;
                        message << "subgroupID " << subgroupID << ", laneID " << output.subgroup[slot].w
                                << ", localIndex " << localIndex << " nativeInclusive was " << FormatFloat(actualNative)
                                << ", expected " << FormatFloat(inclusive);
                        return Failure("native subgroup arithmetic", message.str());
                    }
                }
                nativeSubtotal[subgroupID] = inclusive;
            }

            // sourceDomain is the narrow source-side safety branch. It is checked
            // after native arithmetic so an unsupported source topology still
            // reports native subgroup behavior before failing explicitly.
            if (reportedNumSubgroups < 2u || reportedNumSubgroups > 32u) {
                for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                    const std::size_t slot = slotForLocal[localIndex];
                    const Vec4& reduction = output.reduction[slot];
                    if (!IsQuietNanSentinel(reduction.z) || !IsQuietNanSentinel(reduction.w) ||
                        !IsQuietNanSentinel(output.finalAverage[slot])) {
                        std::ostringstream message;
                        message << "program 203 source reduction has no valid contract for gl_NumSubgroups="
                                << reportedNumSubgroups << "; localIndex " << localIndex
                                << " did not preserve its qNaN source-reduction sentinel";
                        return Failure("source domain", message.str());
                    }
                    for (std::size_t stage = 0; stage < kScanStageCount; ++stage) {
                        if (!IsQuietNanSentinel(output.scanAfter[stage][slot])) {
                            std::ostringstream message;
                            message << "program 203 source reduction has no valid contract for gl_NumSubgroups="
                                    << reportedNumSubgroups << "; localIndex " << localIndex << ", scan stage " << stage
                                    << " did not preserve its qNaN source-reduction sentinel";
                            return Failure("source domain", message.str());
                        }
                    }
                }
                std::ostringstream message;
                message << "program 203 source reduction has no valid contract for observed gl_NumSubgroups="
                        << reportedNumSubgroups << " (requires 2..32); native subgroup results were recorded";
                return Failure("source domain", message.str());
            }

            // 4. Program-203 source writer and first shared-memory handoff.
            std::vector<std::size_t> sourceWriter(reportedNumSubgroups, kNoSlot);
            for (std::uint32_t subgroupID = 0; subgroupID < reportedNumSubgroups; ++subgroupID) {
                std::size_t writerCount = 0;
                for (const std::size_t slot : subgroupSlots[subgroupID]) {
                    const UVec4& subgroup = output.subgroup[slot];
                    if (subgroup.w == subgroup.x - 1u) {
                        sourceWriter[subgroupID] = slot;
                        ++writerCount;
                    }
                }
                if (writerCount != 1u) {
                    std::ostringstream message;
                    message << "subgroupID " << subgroupID << " has " << writerCount
                            << " recorded lane(s) where laneID == subgroupSize - 1; program 203 leaves that "
                               "shared-cache entry unwritten";
                    return Failure("source writer", message.str());
                }
                for (const std::size_t slot : subgroupSlots[subgroupID]) {
                    const float actualRawSubtotal = output.reduction[slot].z;
                    if (!SameBits(actualRawSubtotal, nativeSubtotal[subgroupID])) {
                        std::ostringstream message;
                        message << "subgroupID " << subgroupID << ", localIndex " << output.invocation[slot].x
                                << " sourceRawSubtotal was " << FormatFloat(actualRawSubtotal) << ", expected "
                                << FormatFloat(nativeSubtotal[subgroupID]);
                        return Failure("source raw subtotal", message.str());
                    }
                }
            }

            // 5. Reproduce the source loop exactly, including the redundant final
            // scan iteration on power-of-two subgroup counts. Reads and writes in
            // one iteration target disjoint cache entries, so update the cache at
            // the CPU equivalent of the source barrier.
            std::array<float, kInvocationCount> mergedPrefix = nativePrefix;
            std::vector<float> cache = nativeSubtotal;
            std::uint32_t loopLength = std::bit_width(reportedNumSubgroups) - 1u;
            loopLength +=
                static_cast<std::uint32_t>(reportedNumSubgroups - (1u << (loopLength - 1u)) > 0u);
            for (std::uint32_t scanStage = 0u; scanStage < loopLength; ++scanStage) {
                std::vector<float> cacheAfterStage = cache;
                for (std::uint32_t subgroupID = 0; subgroupID < reportedNumSubgroups; ++subgroupID) {
                    if ((subgroupID & (1u << scanStage)) == 0u) continue;
                    const std::uint32_t sourceCacheIndex = (subgroupID >> scanStage << scanStage) - 1u;
                    const float sourcePrefix = cache[sourceCacheIndex];
                    for (const std::size_t slot : subgroupSlots[subgroupID]) {
                        mergedPrefix[slot] += sourcePrefix;
                    }
                    cacheAfterStage[subgroupID] = mergedPrefix[sourceWriter[subgroupID]];
                }
                cache.swap(cacheAfterStage);
                for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                    const std::size_t slot = slotForLocal[localIndex];
                    const float actualAfterStage = output.scanAfter[scanStage][slot];
                    if (!SameBits(actualAfterStage, mergedPrefix[slot])) {
                        std::ostringstream message;
                        message << "scanStage " << scanStage << ", subgroupID " << output.subgroup[slot].z
                                << ", laneID " << output.subgroup[slot].w << ", localIndex " << localIndex
                                << " scanAfter was " << FormatFloat(actualAfterStage) << ", expected "
                                << FormatFloat(mergedPrefix[slot]);
                        ValidationResult result = Failure("source scan", message.str());
                        result.scanStageMismatch = true;
                        result.scanStage = static_cast<int>(scanStage);
                        return result;
                    }
                }
            }
            for (std::size_t localIndex = 0; localIndex < kInvocationCount; ++localIndex) {
                const std::size_t slot = slotForLocal[localIndex];
                const float actualMergedPrefix = output.reduction[slot].w;
                if (!SameBits(actualMergedPrefix, mergedPrefix[slot])) {
                    std::ostringstream message;
                    message << "localIndex " << localIndex << " sourceMergedPrefix was "
                            << FormatFloat(actualMergedPrefix) << ", expected " << FormatFloat(mergedPrefix[slot]);
                    return Failure("source scan", message.str());
                }
            }

            // 6. Final owner and average. The uniformity check is intentionally
            // separate from the source's topology contract at local index 511.
            const float firstAverage = output.finalAverage[slotForLocal[0]];
            for (std::size_t localIndex = 1; localIndex < kInvocationCount; ++localIndex) {
                const float actualAverage = output.finalAverage[slotForLocal[localIndex]];
                if (!SameBits(actualAverage, firstAverage)) {
                    std::ostringstream message;
                    message << "finalAverage differs: localIndex 0 has " << FormatFloat(firstAverage)
                            << ", localIndex " << localIndex << " has " << FormatFloat(actualAverage);
                    return Failure("final average", message.str());
                }
            }

            ValidationResult ownerResult;
            ownerResult.ownerEvaluated = true;
            for (std::uint32_t subgroupID = 0; subgroupID < reportedNumSubgroups; ++subgroupID) {
                if (!subgroupSlots[subgroupID].empty()) {
                    ownerResult.highestObservedSubgroup = std::max(ownerResult.highestObservedSubgroup, subgroupID);
                }
            }
            const std::size_t index511Slot = slotForLocal[kInvocationCount - 1u];
            const UVec4& index511Subgroup = output.subgroup[index511Slot];
            ownerResult.index511IsSourceLastLaneWriter =
                index511Subgroup.w == index511Subgroup.x - 1u;
            ownerResult.index511IsHighestSubgroupMember =
                index511Subgroup.z == ownerResult.highestObservedSubgroup;
            if (!ownerResult.index511IsSourceLastLaneWriter || !ownerResult.index511IsHighestSubgroupMember) {
                std::ostringstream message;
                message << "program 203 topology incompatibility: localIndex 511 is sourceLastLaneWriter="
                        << ownerResult.index511IsSourceLastLaneWriter << ", highestSubgroupMember="
                        << ownerResult.index511IsHighestSubgroupMember << " (subgroupID=" << index511Subgroup.z
                        << ", highest observed subgroupID=" << ownerResult.highestObservedSubgroup << ')';
                ownerResult.ok = false;
                ownerResult.phase = "final average";
                ownerResult.message = message.str();
                return ownerResult;
            }

            float total = 0.0f;
            for (const float subtotal : nativeSubtotal) total += subtotal;
            float sampledExpectedTotal = 0.0f;
            for (std::size_t i = 0; i < kInvocationCount; ++i) sampledExpectedTotal += kSampledLuminance;
            const float expectedTotal = mode == InputMode::IndexedSsbo ? 131328.0f : sampledExpectedTotal;
            if (!SameBits(total, expectedTotal) || !SameBits(mergedPrefix[index511Slot], expectedTotal)) {
                std::ostringstream message;
                message << "program 203 source total was " << FormatFloat(mergedPrefix[index511Slot])
                        << " (native total " << FormatFloat(total) << "), expected " << FormatFloat(expectedTotal);
                ownerResult.ok = false;
                ownerResult.phase = "final average";
                ownerResult.message = message.str();
                return ownerResult;
            }

            const float expectedAverage = mode == InputMode::IndexedSsbo ? 256.5f : sampledExpectedTotal / 512.0f;
            if (!SameBits(firstAverage, expectedAverage)) {
                std::ostringstream message;
                message << "finalAverage was " << FormatFloat(firstAverage) << ", expected "
                        << FormatFloat(expectedAverage);
                ownerResult.ok = false;
                ownerResult.phase = "final average";
                ownerResult.message = message.str();
                return ownerResult;
            }
            return ownerResult;
        }

        void DumpProbe(const ProbeOutput& output, const CapabilityInfo& capabilities, const ValidationResult& validation,
                       bool includeScanStages) {
            PrintMetadata(capabilities, std::cout);
            if (validation.ok) {
                std::cout << "Program203FirstReductionScenario firstFailure=none\n";
            } else {
                std::cout << "Program203FirstReductionScenario firstFailure=" << validation.phase << ": "
                          << validation.message << '\n';
            }
            std::cout << "localIndex,localX,localY,localZ,subgroupSize,numSubgroups,subgroupID,laneID,input,"
                         "nativeInclusive,subgroupSubtotal,mergedPrefix,finalAverage\n";
            for (std::size_t slot = 0; slot < kInvocationCount; ++slot) {
                const UVec4& invocation = output.invocation[slot];
                const UVec4& subgroup = output.subgroup[slot];
                const Vec4& reduction = output.reduction[slot];
                std::cout << invocation.x << ',' << invocation.y << ',' << invocation.z << ',' << invocation.w << ','
                          << subgroup.x << ',' << subgroup.y << ',' << subgroup.z << ',' << subgroup.w << ','
                          << std::hexfloat << reduction.x << ',' << reduction.y << ',' << reduction.z << ','
                          << reduction.w << ',' << output.finalAverage[slot] << std::defaultfloat << '\n';
            }
            if (includeScanStages) {
                std::cout << "scanStage,localIndex,scanAfter\n";
                for (std::size_t scanStage = 0; scanStage < kScanStageCount; ++scanStage) {
                    for (std::size_t slot = 0; slot < kInvocationCount; ++slot) {
                        std::cout << scanStage << ',' << output.invocation[slot].x << ',' << std::hexfloat
                                  << output.scanAfter[scanStage][slot] << std::defaultfloat << '\n';
                    }
                }
            }
        }

        class Program203FirstReductionScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                m_capabilities = QueryCapabilities();
                // GL_SUBGROUP_SIZE_KHR is diagnostic only. It is deliberately
                // never used to infer lane placement or an expected group count.
                PrintMetadata(m_capabilities, std::cout);
                RecordProperty("program203_gl_subgroup_size_khr", std::to_string(m_capabilities.subgroupSize));
                if (!m_capabilities.SupportsProbe()) {
                    GTEST_SKIP() << "subgroup probe requires " << m_capabilities.MissingRequirements();
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE0);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                if (m_inputBuffer != 0) glDeleteBuffers(1, &m_inputBuffer);
                if (m_outputBuffer != 0) glDeleteBuffers(1, &m_outputBuffer);
                if (m_program != 0) glDeleteProgram(m_program);
                m_texture = 0;
                m_inputBuffer = 0;
                m_outputBuffer = 0;
                m_program = 0;
            }

            GLuint CompileComputeProgram(const std::string& source, std::string* outError) {
                const char* text = source.c_str();
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                if (shader == 0) {
                    *outError = "glCreateShader(GL_COMPUTE_SHADER) returned 0";
                    return 0;
                }
                glShaderSource(shader, 1, &text, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[8192] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    *outError = std::string("the subgroup probe compute shader did not compile: ") + log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[8192] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    *outError = std::string("the subgroup probe compute program did not link: ") + log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            bool RunProbe(InputMode mode, ProbeOutput* output, std::string* outError) {
                m_program = CompileComputeProgram(BuildProbeShader(mode), outError);
                if (m_program == 0) return false;

                ProbeOutput poison{};
                std::memset(&poison, 0xa5, sizeof(poison));
                glGenBuffers(1, &m_outputBuffer);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_outputBuffer);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ProbeOutput), &poison, GL_DYNAMIC_COPY);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_outputBuffer);

                if (mode == InputMode::IndexedSsbo) {
                    std::array<float, kInvocationCount> values{};
                    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<float>(i + 1u);
                    glGenBuffers(1, &m_inputBuffer);
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_inputBuffer);
                    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(values), values.data(), GL_STATIC_DRAW);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_inputBuffer);
                } else {
                    constexpr std::array<float, 4> kOneTexel = {1.0f, 1.0f, 1.0f, 1.0f};
                    glGenTextures(1, &m_texture);
                    glActiveTexture(GL_TEXTURE3);
                    glBindTexture(GL_TEXTURE_2D, m_texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, kOneTexel.data());
                }

                if (const GLenum error = FirstGLError(); error != GL_NO_ERROR) {
                    std::ostringstream message;
                    message << "subgroup probe resource setup left " << GLErrorName(error);
                    *outError = message.str();
                    return false;
                }

                glUseProgram(m_program);
                if (mode == InputMode::SampledRgba32f) {
                    const GLint sampler = glGetUniformLocation(m_program, "colortex2");
                    const GLint pixelSize = glGetUniformLocation(m_program, "pixelSize");
                    if (sampler == -1 || pixelSize == -1) {
                        *outError = "the sampled probe uniforms were optimized away or not reflected";
                        return false;
                    }
                    glUniform1i(sampler, 3);
                    glUniform2f(pixelSize, 1.0f / 854.0f, 1.0f / 480.0f);
                }
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_outputBuffer);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(ProbeOutput), output);
                if (const GLenum error = FirstGLError(); error != GL_NO_ERROR) {
                    std::ostringstream message;
                    message << "subgroup probe dispatch/readback left " << GLErrorName(error);
                    *outError = message.str();
                    return false;
                }
                return true;
            }

            void RunAndValidate(InputMode mode) {
                ProbeOutput output{};
                std::string error;
                ASSERT_TRUE(RunProbe(mode, &output, &error)) << InputModeName(mode) << ": " << error;

                const ValidationResult validation = ValidateProbe(output, mode);
                if (validation.ownerEvaluated) {
                    RecordProperty("program203_index511_source_last_lane_writer",
                                   validation.index511IsSourceLastLaneWriter ? "true" : "false");
                    RecordProperty("program203_index511_highest_subgroup_member",
                                   validation.index511IsHighestSubgroupMember ? "true" : "false");
                    RecordProperty("program203_highest_observed_subgroup",
                                   std::to_string(validation.highestObservedSubgroup));
                    std::cout << "Program203FirstReductionScenario owner: localIndex511 sourceLastLaneWriter="
                              << validation.index511IsSourceLastLaneWriter << ", highestSubgroupMember="
                              << validation.index511IsHighestSubgroupMember << ", highestObservedSubgroup="
                              << validation.highestObservedSubgroup << '\n';
                }
                if (!validation.ok || DumpRequested()) {
                    DumpProbe(output, m_capabilities, validation, validation.scanStageMismatch || DumpRequested());
                }
                EXPECT_TRUE(validation.ok) << validation.phase << ": " << validation.message;
            }

            CapabilityInfo m_capabilities;
            GLuint m_program = 0;
            GLuint m_inputBuffer = 0;
            GLuint m_outputBuffer = 0;
            GLuint m_texture = 0;
        };

    } // namespace

    TEST_F(Program203FirstReductionScenario, SampledRgba32fFirstAverage) {
        if (!Ready() || IsSkipped()) return;
        RunAndValidate(InputMode::SampledRgba32f);
    }

    TEST_F(Program203FirstReductionScenario, IndexedInputTopologyAndReduction) {
        if (!Ready() || IsSkipped()) return;
        RunAndValidate(InputMode::IndexedSsbo);
    }

} // namespace MGITest
