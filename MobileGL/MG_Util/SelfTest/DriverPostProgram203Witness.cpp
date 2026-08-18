// MobileGL - MobileGL/MG_Util/SelfTest/DriverPostProgram203Witness.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DriverPostProgram203Witness.h"

#include <bit>
#include <sstream>
#include <utility>
#include <vector>

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        [[nodiscard]] Program203WitnessValidationResult Failure(Program203WitnessValidationFailure failure,
                                                                 std::string detail,
                                                                 std::uint32_t scanStage = 0u,
                                                                 std::uint32_t subgroup = 0u) {
            Program203WitnessValidationResult result;
            result.ok = false;
            result.failure = failure;
            result.scanStage = scanStage;
            result.subgroup = subgroup;
            result.detail = std::move(detail);
            return result;
        }

        [[nodiscard]] std::uint32_t FloatBits(float value) {
            return std::bit_cast<std::uint32_t>(value);
        }

        [[nodiscard]] bool SameBits(float lhs, float rhs) {
            return FloatBits(lhs) == FloatBits(rhs);
        }

        [[nodiscard]] bool SameBits(const Program203WitnessVec2& lhs, const Program203WitnessVec2& rhs) {
            return SameBits(lhs.x, rhs.x) && SameBits(lhs.y, rhs.y);
        }

        [[nodiscard]] std::string Vec2String(const Program203WitnessVec2& value) {
            std::ostringstream output;
            output << '(' << value.x << ',' << value.y << ')';
            return output.str();
        }

        [[nodiscard]] std::uint32_t ExpectedSeenSubgroupMask(std::uint32_t numSubgroups) {
            return numSubgroups == kProgram203WitnessMaxSubgroups ? 0xffffffffu : (1u << numSubgroups) - 1u;
        }

        [[nodiscard]] std::string JoinRequirements(const std::vector<std::string>& requirements) {
            std::ostringstream output;
            for (std::size_t i = 0; i < requirements.size(); ++i) {
                if (i != 0u) output << "; ";
                output << requirements[i];
            }
            return output.str();
        }
    } // namespace

    Program203WitnessEligibilityResult
    EvaluateProgram203WitnessEligibility(const Program203WitnessLimits& limits) {
        // This classification deliberately precedes numeric limits. An absent native
        // compute/basic/arithmetic subgroup contract means there is nothing to witness,
        // whereas every resource/entry-point failure on a capable device is a POST FAIL.
        if (!limits.computeStageSupported || !limits.basicSubgroupSupported || !limits.arithmeticSubgroupSupported) {
            std::vector<std::string> missing;
            if (!limits.computeStageSupported) missing.emplace_back("VK_SHADER_STAGE_COMPUTE_BIT");
            if (!limits.basicSubgroupSupported) missing.emplace_back("VK_SUBGROUP_FEATURE_BASIC_BIT");
            if (!limits.arithmeticSubgroupSupported) missing.emplace_back("VK_SUBGROUP_FEATURE_ARITHMETIC_BIT");
            return {Program203WitnessEligibility::SkipUnsupportedNativeFeatureSet,
                    "skipped because the native compute/basic/arithmetic subgroup feature set is unsupported (missing " +
                        JoinRequirements(missing) + ')'};
        }

        std::vector<std::string> inadequate;
        if (limits.subgroupSize == 0u) {
            inadequate.emplace_back("subgroupSize == 0");
        }
        if (limits.maxComputeWorkGroupInvocations < kProgram203WitnessInvocationCount) {
            inadequate.emplace_back("maxComputeWorkGroupInvocations < 512");
        }
        if (limits.maxComputeWorkGroupSize[0] < 32u || limits.maxComputeWorkGroupSize[1] < 16u ||
            limits.maxComputeWorkGroupSize[2] < 1u) {
            inadequate.emplace_back("maxComputeWorkGroupSize does not cover 32x16x1");
        }
        if (limits.maxComputeSharedMemorySize < kProgram203WitnessSharedMemoryBytes) {
            inadequate.emplace_back("maxComputeSharedMemorySize < " +
                                    std::to_string(kProgram203WitnessSharedMemoryBytes));
        }
        if (limits.maxPerStageDescriptorStorageBuffers < 1u) {
            inadequate.emplace_back("maxPerStageDescriptorStorageBuffers < 1");
        }
        if (limits.maxDescriptorSetStorageBuffers < 1u) {
            inadequate.emplace_back("maxDescriptorSetStorageBuffers < 1");
        }
        if (limits.maxBoundDescriptorSets < 1u) {
            inadequate.emplace_back("maxBoundDescriptorSets < 1");
        }
        if (limits.maxStorageBufferRange < sizeof(Program203WitnessOutput)) {
            inadequate.emplace_back("maxStorageBufferRange < " +
                                    std::to_string(sizeof(Program203WitnessOutput)));
        }
        if (!inadequate.empty()) {
            return {Program203WitnessEligibility::FailInadequateLimits,
                    "insufficient Vulkan limits for a 32x16x1 workgroup, one output SSBO, and " +
                        std::to_string(kProgram203WitnessSharedMemoryBytes) + " bytes of shared memory: " +
                        JoinRequirements(inadequate)};
        }
        return {Program203WitnessEligibility::Execute, {}};
    }

    std::uint32_t ComputeProgram203WitnessLoopLength(std::uint32_t numSubgroups) {
        if (numSubgroups < 2u || numSubgroups > kProgram203WitnessMaxSubgroups) return 0u;

        // Exact C++ spelling of the source's findMSB-based calculation. In
        // particular, its final iteration for powers of two is intentional.
        std::uint32_t loopLength = 0u;
        for (std::uint32_t value = numSubgroups; value > 1u; value >>= 1u) {
            ++loopLength;
        }
        loopLength += static_cast<std::uint32_t>(numSubgroups - (1u << (loopLength - 1u)) > 0u);
        return loopLength;
    }

    Program203WitnessValidationResult ValidateProgram203Witness(const Program203WitnessOutput& output) {
        // 1. Completion. A poisoned or unwritten result must never turn into a
        // topology diagnosis, because it says nothing about execution.
        if (output.magic != kProgram203WitnessMagic) {
            std::ostringstream detail;
            detail << "completion: magic was 0x" << std::hex << output.magic << ", expected 0x"
                   << kProgram203WitnessMagic;
            return Failure(Program203WitnessValidationFailure::Completion, detail.str());
        }

        // 2. Observed topology. All checks consume observations written by the
        // shader, rather than inferring subgroup layout from invocation indices.
        const std::uint32_t numSubgroups = output.numSubgroups;
        if (numSubgroups < 2u || numSubgroups > kProgram203WitnessMaxSubgroups) {
            std::ostringstream detail;
            detail << "topology: canonical gl_NumSubgroups=" << numSubgroups << " is outside [2, 32]";
            return Failure(Program203WitnessValidationFailure::Topology, detail.str());
        }
        if ((output.topologyFlags & Program203WitnessNonuniformNumSubgroups) != 0u) {
            return Failure(Program203WitnessValidationFailure::Topology,
                           "topology: gl_NumSubgroups differed across workgroup");
        }
        if ((output.topologyFlags & Program203WitnessInvalidNumSubgroups) != 0u) {
            return Failure(Program203WitnessValidationFailure::Topology,
                           "topology: an invocation reported gl_NumSubgroups outside [2, 32]");
        }
        if ((output.topologyFlags & Program203WitnessInvalidSubgroupId) != 0u) {
            return Failure(Program203WitnessValidationFailure::Topology,
                           "topology: an invocation reported an invalid gl_SubgroupID");
        }
        if ((output.topologyFlags & Program203WitnessInvalidSubgroupLane) != 0u) {
            return Failure(Program203WitnessValidationFailure::Topology,
                           "topology: an invocation reported an invalid subgroup lane");
        }
        if ((output.topologyFlags & ~(Program203WitnessNonuniformNumSubgroups |
                                      Program203WitnessInvalidNumSubgroups |
                                      Program203WitnessInvalidSubgroupId |
                                      Program203WitnessInvalidSubgroupLane)) != 0u) {
            std::ostringstream detail;
            detail << "topology: unknown topology flags 0x" << std::hex << output.topologyFlags;
            return Failure(Program203WitnessValidationFailure::Topology, detail.str());
        }
        const std::uint32_t expectedMask = ExpectedSeenSubgroupMask(numSubgroups);
        if (output.seenSubgroupMask != expectedMask) {
            std::ostringstream detail;
            detail << "topology: seen subgroup-ID mask was 0x" << std::hex << output.seenSubgroupMask
                   << ", expected 0x" << expectedMask;
            return Failure(Program203WitnessValidationFailure::Topology, detail.str());
        }
        const std::uint32_t expectedLoopLength = ComputeProgram203WitnessLoopLength(numSubgroups);
        if (output.loopLength != expectedLoopLength) {
            std::ostringstream detail;
            detail << "topology: loopLength was " << std::dec << output.loopLength << ", expected "
                   << expectedLoopLength;
            return Failure(Program203WitnessValidationFailure::Topology, detail.str());
        }
        for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
            if (output.lastLaneWriterCount[subgroup] != 1u) {
                std::ostringstream detail;
                detail << "topology: subgroup " << subgroup << " has "
                       << output.lastLaneWriterCount[subgroup] << " source last-lane writers, expected exactly 1";
                return Failure(Program203WitnessValidationFailure::Topology, detail.str(), 0u, subgroup);
            }
        }
        if (output.owner511.y != numSubgroups) {
            std::ostringstream detail;
            detail << "final owner: invocation 511 reported gl_NumSubgroups=" << output.owner511.y << ", expected "
                   << numSubgroups;
            return Failure(Program203WitnessValidationFailure::FinalOwner, detail.str());
        }
        if (output.owner511.z != numSubgroups - 1u) {
            std::ostringstream detail;
            detail << "final owner: invocation 511 is not in the highest subgroup (id" << output.owner511.z
                   << ", expected id" << (numSubgroups - 1u) << ')';
            return Failure(Program203WitnessValidationFailure::FinalOwner, detail.str());
        }
        if (output.owner511.x == 0u || output.owner511.w != output.owner511.x - 1u) {
            std::ostringstream detail;
            detail << "final owner: invocation 511 is not the last lane of highest subgroup (size "
                   << output.owner511.x << ", lane " << output.owner511.w << ')';
            return Failure(Program203WitnessValidationFailure::FinalOwner, detail.str());
        }

        // 3. Initial subgroup handoff. The atomic scalar totals are independent
        // of subgroupInclusiveAdd; their sum and the cache values establish that
        // the final lanes handed off the native vector inclusive-add results.
        std::uint64_t indexedTotal = 0u;
        for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
            indexedTotal += output.indexedInputTotal[subgroup];
        }
        if (indexedTotal != 131328u) {
            std::ostringstream detail;
            detail << "initial subgroup handoff: indexed input total was " << indexedTotal << ", expected 131328";
            return Failure(Program203WitnessValidationFailure::InitialSubgroupHandoff, detail.str());
        }
        for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
            const Program203WitnessVec2 expected = {static_cast<float>(output.indexedInputTotal[subgroup]), 0.0f};
            if (!SameBits(output.rawPrefix[subgroup], expected)) {
                std::ostringstream detail;
                detail << "initial subgroup handoff: subgroup " << subgroup << " rawPrefix observed "
                       << Vec2String(output.rawPrefix[subgroup]) << ", expected " << Vec2String(expected);
                return Failure(Program203WitnessValidationFailure::InitialSubgroupHandoff, detail.str(), 0u,
                               subgroup);
            }
        }

        // 4. Source scan. Do not substitute a conventional scan: this reproduces
        // the source cache index expression and stage ordering word for word.
        std::array<Program203WitnessVec2, kProgram203WitnessMaxSubgroups> expectedCache = output.rawPrefix;
        for (std::uint32_t scanStage = 0u; scanStage < expectedLoopLength; ++scanStage) {
            auto cacheAfterStage = expectedCache;
            for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
                if ((subgroup & (1u << scanStage)) > 0u) {
                    const std::uint32_t sourceCacheIndex = (subgroup >> scanStage << scanStage) - 1u;
                    cacheAfterStage[subgroup].x += expectedCache[sourceCacheIndex].x;
                    cacheAfterStage[subgroup].y += expectedCache[sourceCacheIndex].y;
                }
            }
            expectedCache = cacheAfterStage;
            for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
                if (!SameBits(output.scanCache[scanStage][subgroup], expectedCache[subgroup])) {
                    std::ostringstream detail;
                    detail << "source scan stage " << scanStage << ", subgroup " << subgroup << ": observed "
                           << Vec2String(output.scanCache[scanStage][subgroup]) << ", expected "
                           << Vec2String(expectedCache[subgroup]);
                    return Failure(Program203WitnessValidationFailure::SourceScan, detail.str(), scanStage, subgroup);
                }
            }
        }

        // 5. The owner contract was checked above with the other topology facts;
        // this final result remains a separate exact-vector check.
        const Program203WitnessVec2 expectedAverage = {256.5f, 0.0f};
        if (!SameBits(output.finalAverage, expectedAverage)) {
            std::ostringstream detail;
            detail << "final average: observed " << Vec2String(output.finalAverage) << ", expected "
                   << Vec2String(expectedAverage);
            return Failure(Program203WitnessValidationFailure::FinalAverage, detail.str());
        }

        std::ostringstream detail;
        detail << "N=" << numSubgroups << ", owner511=id" << output.owner511.z << "/lane" << output.owner511.w
               << ", " << expectedLoopLength << " scan stages, average=" << Vec2String(output.finalAverage);
        Program203WitnessValidationResult result;
        result.ok = true;
        result.failure = Program203WitnessValidationFailure::None;
        result.detail = detail.str();
        return result;
    }
} // namespace MobileGL::MG_Util::SelfTest
