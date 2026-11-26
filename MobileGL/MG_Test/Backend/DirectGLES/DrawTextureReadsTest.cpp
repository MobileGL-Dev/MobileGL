// MobileGL - MobileGL/MG_Test/Backend/DirectGLES/DrawTextureReadsTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The per-context record of texture storage sampled in the current offscreen draw batch.
#include <gtest/gtest.h>
#include <MG_Backend/DirectGLES/DrawTextureReads.h>

namespace {
    using MobileGL::MG_Backend::DirectGLES::TextureImpl::DrawTextureReads;

    TEST(DrawTextureReads, RecordsOnlyOffscreenBatches) {
        DrawTextureReads reads;
        EXPECT_FALSE(reads.NeedsSamplerWalk(1, 1)) << "the window surface is not tracked";
        reads.NoteRead(3, 7);
        EXPECT_FALSE(reads.WasRead(3, 7));

        reads.BindFramebuffer(5);
        EXPECT_TRUE(reads.NeedsSamplerWalk(1, 1));
        reads.NoteRead(3, 7);
        EXPECT_TRUE(reads.WasRead(3, 7));
        EXPECT_FALSE(reads.WasRead(3, 8)) << "a recycled slot has a new generation";
        EXPECT_FALSE(reads.WasRead(4, 7));
    }

    TEST(DrawTextureReads, FramebufferChangesAndSwapsEndTheBatch) {
        DrawTextureReads reads;
        reads.BindFramebuffer(5);
        reads.NoteRead(2, 1);
        reads.BindFramebuffer(5);
        EXPECT_TRUE(reads.WasRead(2, 1)) << "rebinding the same framebuffer keeps the batch";

        reads.BindFramebuffer(6);
        EXPECT_FALSE(reads.WasRead(2, 1));
        reads.NoteRead(2, 1);
        reads.EndBatch();
        EXPECT_FALSE(reads.WasRead(2, 1));

        reads.NoteRead(9, 4);
        reads.DeleteFramebuffer(7);
        EXPECT_TRUE(reads.WasRead(9, 4)) << "deleting another framebuffer changes nothing";
        reads.DeleteFramebuffer(6);
        EXPECT_FALSE(reads.WasRead(9, 4));
        EXPECT_FALSE(reads.NeedsSamplerWalk(1, 1));
    }

    TEST(DrawTextureReads, SamplerWalkRepeatsOnlyWhenTheViewsChange) {
        DrawTextureReads reads;
        reads.BindFramebuffer(5);
        reads.RecordSamplerWalk(1, 10);
        EXPECT_FALSE(reads.NeedsSamplerWalk(1, 10));
        EXPECT_TRUE(reads.NeedsSamplerWalk(1, 11));
        EXPECT_TRUE(reads.NeedsSamplerWalk(2, 10));
        reads.EndBatch();
        EXPECT_TRUE(reads.NeedsSamplerWalk(1, 10));
    }
} // namespace
