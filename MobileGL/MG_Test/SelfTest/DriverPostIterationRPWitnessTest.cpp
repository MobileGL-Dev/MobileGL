// MobileGL - MobileGL/MG_Test/SelfTest/DriverPostIterationRPWitnessTest.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <string>

#include "MG_Util/SelfTest/DriverPostIterationRPWitness.h"

namespace MobileGL::MG_Util::SelfTest {
    namespace {
        IterationRPWitnessOutput MakeValidWitness(std::uint32_t numSubgroups) {
            IterationRPWitnessOutput output{};
            output.magic = kIterationRPWitnessMagic;
            output.numSubgroups = numSubgroups;
            output.loopLength = ComputeIterationRPWitnessLoopLength(numSubgroups);
            output.seenSubgroupMask =
                numSubgroups == kIterationRPWitnessMaxSubgroups ? 0xffffffffu : (1u << numSubgroups) - 1u;

            // Valid test layouts use equal contiguous groups of the indexed
            // 1..512 input. The compact witness only needs their independent sums.
            const std::uint32_t subgroupSize = kIterationRPWitnessInvocationCount / numSubgroups;
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

        IterationRPWitnessLimits MakeSufficientLimits() {
            IterationRPWitnessLimits limits;
            limits.computeStageSupported = true;
            limits.basicSubgroupSupported = true;
            limits.arithmeticSubgroupSupported = true;
            limits.subgroupSize = 32u;
            limits.maxComputeWorkGroupInvocations = kIterationRPWitnessInvocationCount;
            limits.maxComputeWorkGroupSize = {32u, 16u, 1u};
            limits.maxComputeSharedMemorySize = kIterationRPWitnessSharedMemoryBytes;
            limits.maxPerStageDescriptorStorageBuffers = 1u;
            limits.maxDescriptorSetStorageBuffers = 1u;
            limits.maxBoundDescriptorSets = 1u;
            limits.maxStorageBufferRange = sizeof(IterationRPWitnessOutput);
            return limits;
        }
    } // namespace

    TEST(DriverPostIterationRPWitnessTest, ValidTwoSubgroupWitness) {
        const IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(MakeValidWitness(2u));
        ASSERT_TRUE(validation.ok) << validation.detail;
        EXPECT_EQ(validation.detail, "N=2, owner511=id1/lane255, 2 scan stages, average=(256.5,0)");
    }

    TEST(DriverPostIterationRPWitnessTest, ValidThirtyTwoSubgroupWitness) {
        const IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(MakeValidWitness(32u));
        ASSERT_TRUE(validation.ok) << validation.detail;
        EXPECT_EQ(validation.detail, "N=32, owner511=id31/lane15, 6 scan stages, average=(256.5,0)");
    }

    TEST(DriverPostIterationRPWitnessTest, RejectsNonuniformNumSubgroups) {
        IterationRPWitnessOutput output = MakeValidWitness(16u);
        output.topologyFlags |= IterationRPWitnessNonuniformNumSubgroups;
        const IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, IterationRPWitnessValidationFailure::Topology);
        EXPECT_NE(validation.detail.find("gl_NumSubgroups differed"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, RejectsMissingAndOutOfRangeSubgroupIds) {
        IterationRPWitnessOutput missing = MakeValidWitness(16u);
        missing.seenSubgroupMask &= ~(1u << 7u);
        IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(missing);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("seen subgroup-ID mask"), std::string::npos);

        IterationRPWitnessOutput outOfRange = MakeValidWitness(16u);
        outOfRange.topologyFlags |= IterationRPWitnessInvalidSubgroupId;
        validation = ValidateIterationRPWitness(outOfRange);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("invalid gl_SubgroupID"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, RejectsInvalidMultipleAndMissingLastLaneWriters) {
        IterationRPWitnessOutput invalidLane = MakeValidWitness(16u);
        invalidLane.topologyFlags |= IterationRPWitnessInvalidSubgroupLane;
        IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(invalidLane);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("invalid subgroup lane"), std::string::npos);

        IterationRPWitnessOutput multiple = MakeValidWitness(16u);
        multiple.lastLaneWriterCount[4] = 2u;
        validation = ValidateIterationRPWitness(multiple);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("subgroup 4 has 2 source last-lane writers"), std::string::npos);

        IterationRPWitnessOutput missing = MakeValidWitness(16u);
        missing.lastLaneWriterCount[6] = 0u;
        validation = ValidateIterationRPWitness(missing);
        EXPECT_FALSE(validation.ok);
        EXPECT_NE(validation.detail.find("subgroup 6 has 0 source last-lane writers"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, ReportsEarliestCorruptSourceScanStage) {
        IterationRPWitnessOutput output = MakeValidWitness(32u);
        output.scanCache[0][1].x += 1.0f;
        output.scanCache[3][5].x += 1.0f;
        IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, IterationRPWitnessValidationFailure::SourceScan);
        EXPECT_EQ(validation.scanStage, 0u);
        EXPECT_NE(validation.detail.find("source scan stage 0, subgroup 1"), std::string::npos);

        output = MakeValidWitness(32u);
        output.scanCache[3][5].x += 1.0f;
        validation = ValidateIterationRPWitness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, IterationRPWitnessValidationFailure::SourceScan);
        EXPECT_EQ(validation.scanStage, 3u);
        EXPECT_NE(validation.detail.find("source scan stage 3, subgroup 5"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, RejectsOwner511OutsideHighestFinalLane) {
        IterationRPWitnessOutput output = MakeValidWitness(16u);
        output.owner511.z = 14u;
        const IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, IterationRPWitnessValidationFailure::FinalOwner);
        EXPECT_NE(validation.detail.find("not in the highest subgroup"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, RejectsIncorrectVectorFinalAverage) {
        IterationRPWitnessOutput output = MakeValidWitness(16u);
        output.finalAverage.y = 1.0f;
        const IterationRPWitnessValidationResult validation = ValidateIterationRPWitness(output);
        EXPECT_FALSE(validation.ok);
        EXPECT_EQ(validation.failure, IterationRPWitnessValidationFailure::FinalAverage);
        EXPECT_NE(validation.detail.find("final average"), std::string::npos);
    }

    TEST(DriverPostIterationRPWitnessTest, MissingNativeFeatureIsTheOnlySkipCondition) {
        for (const auto toggleMissingFeature : {0u, 1u, 2u}) {
            IterationRPWitnessLimits limits = MakeSufficientLimits();
            if (toggleMissingFeature == 0u) limits.computeStageSupported = false;
            if (toggleMissingFeature == 1u) limits.basicSubgroupSupported = false;
            if (toggleMissingFeature == 2u) limits.arithmeticSubgroupSupported = false;
            const IterationRPWitnessEligibilityResult eligibility = EvaluateIterationRPWitnessEligibility(limits);
            EXPECT_EQ(eligibility.eligibility, IterationRPWitnessEligibility::SkipUnsupportedNativeFeatureSet)
                << eligibility.detail;
        }

        IterationRPWitnessLimits zeroSubgroupSize = MakeSufficientLimits();
        zeroSubgroupSize.subgroupSize = 0u;
        IterationRPWitnessEligibilityResult eligibility = EvaluateIterationRPWitnessEligibility(zeroSubgroupSize);
        EXPECT_EQ(eligibility.eligibility, IterationRPWitnessEligibility::FailInadequateLimits) << eligibility.detail;

        IterationRPWitnessLimits limits = MakeSufficientLimits();
        limits.maxComputeWorkGroupInvocations = 511u;
        eligibility = EvaluateIterationRPWitnessEligibility(limits);
        EXPECT_EQ(eligibility.eligibility, IterationRPWitnessEligibility::FailInadequateLimits) << eligibility.detail;

        limits = MakeSufficientLimits();
        limits.maxStorageBufferRange = sizeof(IterationRPWitnessOutput) - 1u;
        eligibility = EvaluateIterationRPWitnessEligibility(limits);
        EXPECT_EQ(eligibility.eligibility, IterationRPWitnessEligibility::FailInadequateLimits) << eligibility.detail;
    }
} // namespace MobileGL::MG_Util::SelfTest
