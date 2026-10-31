// MobileGL - MobileGL/MG_Test/Util/SharedImageFlushPolicyTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// When a glFlush publishes the session's shared-image accesses (implicit sync for a producer that
// renders into its dma-bufs, Xwayland's glamor): MG_Impl/EGLImpl/SharedImageFlushPolicy.h.

#include <MG_Impl/EGLImpl/SharedImageFlushPolicy.h>

#include <gtest/gtest.h>

using MobileGL::MG_Impl::EGLImpl::FlushPublishesSharedImageAccesses;

TEST(SharedImageFlushPolicy, AnXServerThatBoundImagesToTexturesFlushesAsABoundary) {
    // Xwayland: offscreen session, glamor bound its window pixmaps' images to textures.
    EXPECT_TRUE(FlushPublishesSharedImageAccesses(true, true, false, nullptr));
    // An ordinary GL client never bound one: its flushes stay free.
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(true, false, false, nullptr));
    // The compositor (owns the server window) samples client buffers and ends frames at its swaps.
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(true, true, true, nullptr));
    // Without shared images (a monolith) there is nothing to publish, whatever is asked.
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(false, true, false, nullptr));
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(false, true, false, "1"));
}

TEST(SharedImageFlushPolicy, TheOverrideForcesEitherWay) {
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(true, true, false, "0"));
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(true, true, false, "off"));
    EXPECT_TRUE(FlushPublishesSharedImageAccesses(true, false, true, "1"));
    EXPECT_TRUE(FlushPublishesSharedImageAccesses(true, true, true, "on"));
    // Anything else is the automatic answer.
    EXPECT_TRUE(FlushPublishesSharedImageAccesses(true, true, false, ""));
    EXPECT_FALSE(FlushPublishesSharedImageAccesses(true, true, true, "auto"));
}
