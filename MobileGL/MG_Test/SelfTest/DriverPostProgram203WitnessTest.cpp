// MobileGL - MobileGL/MG_Test/SelfTest/DriverPostProgram203WitnessTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <string>

#include "MG_Util/SelfTest/DriverPostProgram203Witness.h"

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        Program203WitnessOutput MakeValidWitness(std::uint32_t numSubgroups) {
            Program203WitnessOutput output{};
            output.magic = kProgram203WitnessMagic;
            output.numSubgroups = numSubgroups;
            output.loopLength = ComputeProgram203WitnessLoopLength(numSubgroups);
            output.seenSubgroupMask =
                numSubgroups == kProgram203WitnessMaxSubgroups ? 0xffffffffu : (1u << numSubgroups) - 1u;

            // Valid test layouts use equal contiguous groups of the indexed
            // 1..512 input. The compact witness only needs their independent sums.
            const std::uint32_t subgroupSize = kProgram203WitnessInvocationCount / numSubgroups;
            for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
                const std::uint32_t first = subgroup * subgroupSize + 1u;
                const std::uint32_t last = first + subgroupSize - 1u;
                output.lastLaneWriterCount[subgroup] = 1u;
                output.indexedInputTotal[subgroup] = subgroupSize * (first + last) / 2u;
                output.rawPrefix[subgroup] = {static_cast<float>(output.indexedInputTotal[subgroup]), 0.0f};
            }
            output.owner511 = {subgroupSize, numSubgroups, numSubgroups - 1u, subgroupSize - 1u};

            auto cache = output.rawPrefix;
            for (std::uint32_t scanStage = 0u; scanStage < output.loopLength; ++scanStage) {
                auto cacheAfterStage = cache;
                for (std::uint32_t subgroup = 0u; subgroup < numSubgroups; ++subgroup) {
                    if ((subgroup & (1u << scanStage)) == 0u) continue;
                    const std::uint32_t sourceCacheIndex = (subgroup >> scanStage << scanStage) - 1u;
                    cacheAfterStage[subgroup].x += cache[sourceCacheIndex].x;
                    cacheAfterStage[subgroup].y += cache[sourceCacheIndex].y;
                }
                cache = cacheAfterStage;
                output.scanCache[scanStage] = cache;
            }
            output.finalAverage = {256.5f, 0.0f};
            return output;
        }

        Program203WitnessLimits MakeSufficientLimits() {
            Program203WitnessLimits limits;
            limits.computeStageSupported = true;
            limits.basicSubgroupSupported = true;
            limits.arithmeticSubgroupSupported = true;
            limits.subgroupSize = 32u;
            limits.maxComputeWorkGroupInvocations = kProgram203WitnessInvocationCount;
            limits.maxComputeWorkGroupSize = {32u, 16u, 1u};
            limits.maxComputeSharedMemorySize = kProgram203WitnessSharedMemoryBytes;
            limits.maxPerStageDescriptorStorageBuffers = 1u;
            limits.maxDescriptorSetStorageBuffers = 1u;
            limits.maxBoundDescriptorSets = 1u;
            limits.maxStorageBufferRange = sizeof(Program203WitnessOutput);
            return limits;
        }
    } // namespace

    TEST(DriverPostProgram203WitnessTest, ValidTwoSubgroupWitness) {
        const Program203WitnessValidationResult validation = ValidateProgram203Witness(MakeValidWitness(2u));
        ASSERT_TRUE(validation.ok) << validation.detail;
        EXPECT_EQ(validation.detail, "N=2, owner511=id1/lane255, 2 scan stages, average=(256.5,0)");
    }

    TEST(DriverPostProgram203WitnessTest, ValidThirtyTwoSubgroupWitness) {
        const Program203WitnessValidationResult validation = ValidateProgram203Witness(MakeValidWitness(32u));
        ASSERT_TRUE(validation.ok) << validation.detail;
        EXPECT_EQ(validation.detail, "N=32, owner511=id31/lane15, 6 scan stages, average=(256.5,0)");
    }

    TEST(DriverPostProgram203WitnessTest, RejectsNonuniformNumSubgroups) {
        Program203WitnessOutput output = MakeValidWitness(16u);
        output.topologyFlags |= Program203WitnessNonuniformNumSubgroups;
        const Program203WitnessValidationResult validation = ValidateProgram203Witness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, Program203WitnessValidationFailure::Topology);
        EXPECT_NE(validation.detail.find("gl_NumSubgroups differed"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, RejectsMissingAndOutOfRangeSubgroupIds) {
        Program203WitnessOutput missing = MakeValidWitness(16u);
        missing.seenSubgroupMask &= ~(1u << 7u);
        Program203WitnessValidationResult validation = ValidateProgram203Witness(missing);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("seen subgroup-ID mask"), std::string::npos);

        Program203WitnessOutput outOfRange = MakeValidWitness(16u);
        outOfRange.topologyFlags |= Program203WitnessInvalidSubgroupId;
        validation = ValidateProgram203Witness(outOfRange);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("invalid gl_SubgroupID"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, RejectsInvalidMultipleAndMissingLastLaneWriters) {
        Program203WitnessOutput invalidLane = MakeValidWitness(16u);
        invalidLane.topologyFlags |= Program203WitnessInvalidSubgroupLane;
        Program203WitnessValidationResult validation = ValidateProgram203Witness(invalidLane);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("invalid subgroup lane"), std::string::npos);

        Program203WitnessOutput multiple = MakeValidWitness(16u);
        multiple.lastLaneWriterCount[4] = 2u;
        validation = ValidateProgram203Witness(multiple);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("subgroup 4 has 2 source last-lane writers"), std::string::npos);

        Program203WitnessOutput missing = MakeValidWitness(16u);
        missing.lastLaneWriterCount[6] = 0u;
        validation = ValidateProgram203Witness(missing);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("subgroup 6 has 0 source last-lane writers"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, ReportsEarliestCorruptSourceScanStage) {
        Program203WitnessOutput output = MakeValidWitness(32u);
        output.scanCache[0][1].x += 1.0f;
        output.scanCache[3][5].x += 1.0f;
        Program203WitnessValidationResult validation = ValidateProgram203Witness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, Program203WitnessValidationFailure::SourceScan);
        EXPECT_EQ(validation.scanStage, 0u);
        EXPECT_NE(validation.detail.find("source scan stage 0, subgroup 1"), std::string::npos);

        output = MakeValidWitness(32u);
        output.scanCache[3][5].x += 1.0f;
        validation = ValidateProgram203Witness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, Program203WitnessValidationFailure::SourceScan);
        EXPECT_EQ(validation.scanStage, 3u);
        EXPECT_NE(validation.detail.find("source scan stage 3, subgroup 5"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, RejectsOwner511OutsideHighestFinalLane) {
        Program203WitnessOutput output = MakeValidWitness(16u);
        output.owner511.z = 14u;
        const Program203WitnessValidationResult validation = ValidateProgram203Witness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, Program203WitnessValidationFailure::FinalOwner);
        EXPECT_NE(validation.detail.find("not in the highest subgroup"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, RejectsIncorrectVectorFinalAverage) {
        Program203WitnessOutput output = MakeValidWitness(16u);
        output.finalAverage.y = 1.0f;
        const Program203WitnessValidationResult validation = ValidateProgram203Witness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, Program203WitnessValidationFailure::FinalAverage);
        EXPECT_NE(validation.detail.find("final average"), std::string::npos);
    }

    TEST(DriverPostProgram203WitnessTest, MissingNativeFeatureIsTheOnlySkipCondition) {
        for (const auto toggleMissingFeature : {0u, 1u, 2u}) {
            Program203WitnessLimits limits = MakeSufficientLimits();
            if (toggleMissingFeature == 0u) limits.computeStageSupported = false;
            if (toggleMissingFeature == 1u) limits.basicSubgroupSupported = false;
            if (toggleMissingFeature == 2u) limits.arithmeticSubgroupSupported = false;
            const Program203WitnessEligibilityResult eligibility = EvaluateProgram203WitnessEligibility(limits);
            EXPECT_EQ(eligibility.eligibility, Program203WitnessEligibility::SkipUnsupportedNativeFeatureSet)
                << eligibility.detail;
        }

        Program203WitnessLimits zeroSubgroupSize = MakeSufficientLimits();
        zeroSubgroupSize.subgroupSize = 0u;
        Program203WitnessEligibilityResult eligibility = EvaluateProgram203WitnessEligibility(zeroSubgroupSize);
        EXPECT_EQ(eligibility.eligibility, Program203WitnessEligibility::FailInadequateLimits) << eligibility.detail;

        Program203WitnessLimits limits = MakeSufficientLimits();
        limits.maxComputeWorkGroupInvocations = 511u;
        eligibility = EvaluateProgram203WitnessEligibility(limits);
        EXPECT_EQ(eligibility.eligibility, Program203WitnessEligibility::FailInadequateLimits) << eligibility.detail;

        limits = MakeSufficientLimits();
        limits.maxStorageBufferRange = sizeof(Program203WitnessOutput) - 1u;
        eligibility = EvaluateProgram203WitnessEligibility(limits);
        EXPECT_EQ(eligibility.eligibility, Program203WitnessEligibility::FailInadequateLimits) << eligibility.detail;
    }
} // namespace MobileGL::MG_Util::SelfTest
