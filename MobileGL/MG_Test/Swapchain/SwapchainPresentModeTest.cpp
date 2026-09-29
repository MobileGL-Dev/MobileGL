// MobileGL - MobileGL/MG_Test/Swapchain/SwapchainPresentModeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <gtest/gtest.h>

#include <MG_Backend/DirectVulkan/Renderer/SwapchainObject.h>

using namespace MobileGL;
using MobileGL::MG_Backend::DirectVulkan::SwapchainObject;

namespace {
    // What each surface reports. Android's loader exposes MAILBOX and FIFO (plus the shared
    // modes with VK_KHR_shared_presentable_image) but never IMMEDIATE; Mesa's X11 WSI has
    // all four classic modes.
    const Vector<VkPresentModeKHR> kAndroidModes{
        VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR,
        VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR};
    const Vector<VkPresentModeKHR> kAllClassicModes{VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                    VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    const Vector<VkPresentModeKHR> kFifoOnly{VK_PRESENT_MODE_FIFO_KHR};

    VkPresentModeKHR Choose(const Vector<VkPresentModeKHR>& modes, Optional<Int> swapInterval) {
        return SwapchainObject::ChooseSwapchainPresentMode(modes, swapInterval);
    }
} // namespace

// Before the app calls eglSwapInterval the backend keeps its own ranking (MAILBOX first).
TEST(SwapchainPresentMode, NoRequestKeepsTheDefaultRanking) {
    EXPECT_EQ(Choose(kAllClassicModes, Nullopt), VK_PRESENT_MODE_MAILBOX_KHR);
    EXPECT_EQ(Choose(kAndroidModes, Nullopt), VK_PRESENT_MODE_MAILBOX_KHR);
    EXPECT_EQ(Choose(kFifoOnly, Nullopt), VK_PRESENT_MODE_FIFO_KHR);
}

TEST(SwapchainPresentMode, IntervalZeroPrefersImmediateThenMailboxThenFifo) {
    EXPECT_EQ(Choose(kAllClassicModes, 0), VK_PRESENT_MODE_IMMEDIATE_KHR);
    EXPECT_EQ(Choose(kAndroidModes, 0), VK_PRESENT_MODE_MAILBOX_KHR);
    EXPECT_EQ(Choose({VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR}, 0), VK_PRESENT_MODE_FIFO_KHR);
    EXPECT_EQ(Choose(kFifoOnly, 0), VK_PRESENT_MODE_FIFO_KHR);
}

TEST(SwapchainPresentMode, PositiveIntervalIsFifo) {
    for (Int interval : {1, 2, 4}) {
        EXPECT_EQ(Choose(kAllClassicModes, interval), VK_PRESENT_MODE_FIFO_KHR) << "interval " << interval;
        EXPECT_EQ(Choose(kAndroidModes, interval), VK_PRESENT_MODE_FIFO_KHR) << "interval " << interval;
    }
}

TEST(SwapchainPresentMode, NegativeIntervalIsFifoRelaxedWhenSupported) {
    EXPECT_EQ(Choose(kAllClassicModes, -1), VK_PRESENT_MODE_FIFO_RELAXED_KHR);
    EXPECT_EQ(Choose(kAndroidModes, -1), VK_PRESENT_MODE_FIFO_KHR);
    EXPECT_EQ(Choose(kFifoOnly, -1), VK_PRESENT_MODE_FIFO_KHR);
}
