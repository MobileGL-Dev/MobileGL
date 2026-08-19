// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostIterationRPWitness.h
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Compact, native-Vulkan iterationRP first-reduction witness ABI and its pure
// validator. The types below deliberately mirror DriverPostIterationRPWitness.comp's
// single std430 storage block; changing either side requires updating the static
// layout assertions here.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace MobileGL::MG_Util::SelfTest {
    // "P203": the pack's trace program id, kept stable so the checked-in witness
    // SPIR-V (DriverPostIterationRPWitnessSpv.h) needs no regeneration.
    constexpr std::uint32_t kIterationRPWitnessMagic = 0x50323033u;
    constexpr std::uint32_t kIterationRPWitnessInvocationCount = 512u;
    constexpr std::uint32_t kIterationRPWitnessMaxSubgroups = 32u;
    constexpr std::uint32_t kIterationRPWitnessMaxScanStages = 6u;

    // These bit values are shared with the GLSL source. They document failures in
    // topology observations rather than guessing a topology from local IDs on the host.
    enum IterationRPWitnessTopologyFlag : std::uint32_t {
        IterationRPWitnessNonuniformNumSubgroups = 1u << 0u,
        IterationRPWitnessInvalidNumSubgroups = 1u << 1u,
        IterationRPWitnessInvalidSubgroupId = 1u << 2u,
        IterationRPWitnessInvalidSubgroupLane = 1u << 3u,
    };

    struct alignas(8) IterationRPWitnessVec2 {
        float x;
        float y;
    };

    struct alignas(16) IterationRPWitnessUVec4 {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t z;
        std::uint32_t w;
    };

    // std430 layout of DriverPostIterationRPWitness.comp's IterationRPWitnessOutput block.
    struct alignas(16) IterationRPWitnessOutput {
        std::uint32_t magic;
        std::uint32_t topologyFlags;
        std::uint32_t numSubgroups;
        std::uint32_t loopLength;
        std::uint32_t seenSubgroupMask;

        IterationRPWitnessUVec4 owner511;

        std::array<std::uint32_t, kIterationRPWitnessMaxSubgroups> lastLaneWriterCount;
        std::array<std::uint32_t, kIterationRPWitnessMaxSubgroups> indexedInputTotal;

        std::array<IterationRPWitnessVec2, kIterationRPWitnessMaxSubgroups> rawPrefix;
        std::array<std::array<IterationRPWitnessVec2, kIterationRPWitnessMaxSubgroups>,
                   kIterationRPWitnessMaxScanStages>
            scanCache;
        IterationRPWitnessVec2 finalAverage;
    };

    static_assert(std::is_standard_layout_v<IterationRPWitnessVec2>);
    static_assert(std::is_standard_layout_v<IterationRPWitnessUVec4>);
    static_assert(std::is_standard_layout_v<IterationRPWitnessOutput>);
    static_assert(sizeof(IterationRPWitnessVec2) == 8u);
    static_assert(alignof(IterationRPWitnessVec2) == 8u);
    static_assert(sizeof(IterationRPWitnessUVec4) == 16u);
    static_assert(alignof(IterationRPWitnessUVec4) == 16u);
    static_assert(offsetof(IterationRPWitnessOutput, magic) == 0u);
    static_assert(offsetof(IterationRPWitnessOutput, topologyFlags) == 4u);
    static_assert(offsetof(IterationRPWitnessOutput, numSubgroups) == 8u);
    static_assert(offsetof(IterationRPWitnessOutput, loopLength) == 12u);
    static_assert(offsetof(IterationRPWitnessOutput, seenSubgroupMask) == 16u);
    static_assert(offsetof(IterationRPWitnessOutput, owner511) == 32u);
    static_assert(offsetof(IterationRPWitnessOutput, lastLaneWriterCount) == 48u);
    static_assert(offsetof(IterationRPWitnessOutput, indexedInputTotal) == 176u);
    static_assert(offsetof(IterationRPWitnessOutput, rawPrefix) == 304u);
    static_assert(offsetof(IterationRPWitnessOutput, scanCache) == 560u);
    static_assert(offsetof(IterationRPWitnessOutput, finalAverage) == 2096u);
    static_assert(sizeof(IterationRPWitnessOutput) == 2112u);

    // The witness uses prefixSumCache[32], three scalar shared diagnostics, and
    // two 32-entry scalar diagnostic arrays in the GLSL source. Keep this
    // independent of the output SSBO size.
    constexpr std::uint32_t kIterationRPWitnessSharedMemoryBytes =
        kIterationRPWitnessMaxSubgroups * sizeof(IterationRPWitnessVec2) +
        3u * sizeof(std::uint32_t) +
        2u * kIterationRPWitnessMaxSubgroups * sizeof(std::uint32_t);

    enum class IterationRPWitnessEligibility {
        Execute,
        SkipUnsupportedNativeFeatureSet,
        FailInadequateLimits,
    };

    // The raw physical-device conditions needed by the native witness. This is
    // intentionally distinct from MobileGL's advertised-extension policy.
    struct IterationRPWitnessLimits {
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

    struct IterationRPWitnessEligibilityResult {
        IterationRPWitnessEligibility eligibility = IterationRPWitnessEligibility::FailInadequateLimits;
        std::string detail;
    };

    enum class IterationRPWitnessValidationFailure {
        None,
        Completion,
        Topology,
        InitialSubgroupHandoff,
        SourceScan,
        FinalOwner,
        FinalAverage,
    };

    struct IterationRPWitnessValidationResult {
        bool ok = false;
        IterationRPWitnessValidationFailure failure = IterationRPWitnessValidationFailure::Completion;
        std::uint32_t scanStage = 0u;
        std::uint32_t subgroup = 0u;
        std::string detail;
    };

    [[nodiscard]] IterationRPWitnessEligibilityResult
    EvaluateIterationRPWitnessEligibility(const IterationRPWitnessLimits& limits);

    // Mirrors the source's findMSB expression for valid N in [2, 32].
    [[nodiscard]] std::uint32_t ComputeIterationRPWitnessLoopLength(std::uint32_t numSubgroups);

    [[nodiscard]] IterationRPWitnessValidationResult
    ValidateIterationRPWitness(const IterationRPWitnessOutput& output);
} // namespace MobileGL::MG_Util::SelfTest
