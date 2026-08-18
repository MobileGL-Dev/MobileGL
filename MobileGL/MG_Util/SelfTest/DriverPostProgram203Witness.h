// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostProgram203Witness.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Compact, native-Vulkan Program-203 first-reduction witness ABI and its pure
// validator. The types below deliberately mirror DriverPostProgram203Witness.comp's
// single std430 storage block; changing either side requires updating the static
// layout assertions here.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace MobileGL::MG_Util::SelfTest {
    constexpr std::uint32_t kProgram203WitnessMagic = 0x50323033u; // "P203"
    constexpr std::uint32_t kProgram203WitnessInvocationCount = 512u;
    constexpr std::uint32_t kProgram203WitnessMaxSubgroups = 32u;
    constexpr std::uint32_t kProgram203WitnessMaxScanStages = 6u;

    // These bit values are shared with the GLSL source. They document failures in
    // topology observations rather than guessing a topology from local IDs on the host.
    enum Program203WitnessTopologyFlag : std::uint32_t {
        Program203WitnessNonuniformNumSubgroups = 1u << 0u,
        Program203WitnessInvalidNumSubgroups = 1u << 1u,
        Program203WitnessInvalidSubgroupId = 1u << 2u,
        Program203WitnessInvalidSubgroupLane = 1u << 3u,
    };

    struct alignas(8) Program203WitnessVec2 {
        float x;
        float y;
    };

    struct alignas(16) Program203WitnessUVec4 {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t z;
        std::uint32_t w;
    };

    // std430 layout of DriverPostProgram203Witness.comp's Program203WitnessOutput block.
    struct alignas(16) Program203WitnessOutput {
        std::uint32_t magic;
        std::uint32_t topologyFlags;
        std::uint32_t numSubgroups;
        std::uint32_t loopLength;
        std::uint32_t seenSubgroupMask;

        Program203WitnessUVec4 owner511;

        std::array<std::uint32_t, kProgram203WitnessMaxSubgroups> lastLaneWriterCount;
        std::array<std::uint32_t, kProgram203WitnessMaxSubgroups> indexedInputTotal;

        std::array<Program203WitnessVec2, kProgram203WitnessMaxSubgroups> rawPrefix;
        std::array<std::array<Program203WitnessVec2, kProgram203WitnessMaxSubgroups>,
                   kProgram203WitnessMaxScanStages>
            scanCache;
        Program203WitnessVec2 finalAverage;
    };

    static_assert(std::is_standard_layout_v<Program203WitnessVec2>);
    static_assert(std::is_standard_layout_v<Program203WitnessUVec4>);
    static_assert(std::is_standard_layout_v<Program203WitnessOutput>);
    static_assert(sizeof(Program203WitnessVec2) == 8u);
    static_assert(alignof(Program203WitnessVec2) == 8u);
    static_assert(sizeof(Program203WitnessUVec4) == 16u);
    static_assert(alignof(Program203WitnessUVec4) == 16u);
    static_assert(offsetof(Program203WitnessOutput, magic) == 0u);
    static_assert(offsetof(Program203WitnessOutput, topologyFlags) == 4u);
    static_assert(offsetof(Program203WitnessOutput, numSubgroups) == 8u);
    static_assert(offsetof(Program203WitnessOutput, loopLength) == 12u);
    static_assert(offsetof(Program203WitnessOutput, seenSubgroupMask) == 16u);
    static_assert(offsetof(Program203WitnessOutput, owner511) == 32u);
    static_assert(offsetof(Program203WitnessOutput, lastLaneWriterCount) == 48u);
    static_assert(offsetof(Program203WitnessOutput, indexedInputTotal) == 176u);
    static_assert(offsetof(Program203WitnessOutput, rawPrefix) == 304u);
    static_assert(offsetof(Program203WitnessOutput, scanCache) == 560u);
    static_assert(offsetof(Program203WitnessOutput, finalAverage) == 2096u);
    static_assert(sizeof(Program203WitnessOutput) == 2112u);

    // The witness uses prefixSumCache[32], three scalar shared diagnostics, and
    // two 32-entry scalar diagnostic arrays in the GLSL source. Keep this
    // independent of the output SSBO size.
    constexpr std::uint32_t kProgram203WitnessSharedMemoryBytes =
        kProgram203WitnessMaxSubgroups * sizeof(Program203WitnessVec2) +
        3u * sizeof(std::uint32_t) +
        2u * kProgram203WitnessMaxSubgroups * sizeof(std::uint32_t);

    enum class Program203WitnessEligibility {
        Execute,
        SkipUnsupportedNativeFeatureSet,
        FailInadequateLimits,
    };

    // The raw physical-device conditions needed by the native witness. This is
    // intentionally distinct from MobileGL's advertised-extension policy.
    struct Program203WitnessLimits {
        bool computeStageSupported = false;
        bool basicSubgroupSupported = false;
        bool arithmeticSubgroupSupported = false;
        std::uint32_t subgroupSize = 0u;

        std::uint32_t maxComputeWorkGroupInvocations = 0u;
        std::array<std::uint32_t, 3> maxComputeWorkGroupSize{};
        std::uint32_t maxComputeSharedMemorySize = 0u;
        std::uint32_t maxPerStageDescriptorStorageBuffers = 0u;
        std::uint32_t maxDescriptorSetStorageBuffers = 0u;
        std::uint32_t maxBoundDescriptorSets = 0u;
        std::uint64_t maxStorageBufferRange = 0u;
    };

    struct Program203WitnessEligibilityResult {
        Program203WitnessEligibility eligibility = Program203WitnessEligibility::FailInadequateLimits;
        std::string detail;
    };

    enum class Program203WitnessValidationFailure {
        None,
        Completion,
        Topology,
        InitialSubgroupHandoff,
        SourceScan,
        FinalOwner,
        FinalAverage,
    };

    struct Program203WitnessValidationResult {
        bool ok = false;
        Program203WitnessValidationFailure failure = Program203WitnessValidationFailure::Completion;
        std::uint32_t scanStage = 0u;
        std::uint32_t subgroup = 0u;
        std::string detail;
    };

    [[nodiscard]] Program203WitnessEligibilityResult
    EvaluateProgram203WitnessEligibility(const Program203WitnessLimits& limits);

    // Mirrors the source's findMSB expression for valid N in [2, 32].
    [[nodiscard]] std::uint32_t ComputeProgram203WitnessLoopLength(std::uint32_t numSubgroups);

    [[nodiscard]] Program203WitnessValidationResult
    ValidateProgram203Witness(const Program203WitnessOutput& output);
} // namespace MobileGL::MG_Util::SelfTest
