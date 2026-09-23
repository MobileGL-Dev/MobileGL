// MobileGL - MobileGL/MG_Test/Pipe/MagmaAggregateWaitTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Deterministic, device-free control for P7 B4's aggregate submit wait. Production and this
// test share CollectSubmitFencePrefix: two submissions can carry one frame serial, so the
// targeted serial stays incomplete when the old single-fence control retires only one.
#include <gtest/gtest.h>

#include <MG_Backend/DirectVulkan/Renderer/SubmitFencePrefix.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
using MobileGL::MG_Backend::DirectVulkan::CollectSubmitFencePrefix;

struct Submit {
    std::uint64_t submitIndex;
    std::uint64_t frameSerial;
    int fence;
};

bool SerialComplete(std::uint64_t serial, const std::vector<Submit>& pending) {
    return std::none_of(pending.begin(), pending.end(), [serial](const Submit& submit) {
        return submit.frameSerial == serial;
    });
}

TEST(MagmaAggregateWaitTest, MissingTargetAfterAnEarlierPrefixIsRejected) {
    const std::vector<Submit> gap{{10, 42, 101}, {12, 43, 103}};
    std::vector<int> fences;

    EXPECT_FALSE(CollectSubmitFencePrefix(gap, 11, 0, fences));
    EXPECT_TRUE(fences.empty());
}

TEST(MagmaAggregateWaitTest, SingleFenceControlLeavesTheTargetFrameInFlight) {
    const std::vector<Submit> pending{{10, 42, 101}, {11, 42, 102}, {12, 43, 103}};

    // The old control waits only the first matching fence. The second submit still carries 42.
    auto singleFenceControl = pending;
    singleFenceControl.erase(singleFenceControl.begin());
    EXPECT_FALSE(SerialComplete(42, singleFenceControl));

    // The production selector includes both submissions through index 11, but not frame 43.
    std::vector<int> fences;
    ASSERT_TRUE(CollectSubmitFencePrefix(pending, 11, 0, fences));
    ASSERT_EQ(fences, (std::vector<int>{101, 102}));

    auto aggregateWait = pending;
    aggregateWait.erase(
        std::remove_if(aggregateWait.begin(), aggregateWait.end(), [&fences](const Submit& submit) {
            return std::find(fences.begin(), fences.end(), submit.fence) != fences.end();
        }),
        aggregateWait.end());
    EXPECT_TRUE(SerialComplete(42, aggregateWait));
    ASSERT_EQ(aggregateWait.size(), 1u);
    EXPECT_EQ(aggregateWait.front().frameSerial, 43u);
}
} // namespace
