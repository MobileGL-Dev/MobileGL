// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTimerQueryManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include <Includes.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // GPU timestamp storage backing the GL timer-query frontend (GL_TIME_ELAPSED
    // spans and GL_TIMESTAMP one-shots): one VkQueryPool of timestamp slots per
    // frame in flight.
    //
    // Per-frame lifecycle: right after a frame slot's command buffer begins
    // recording (and before any render pass, since vkCmdResetQueryPool must be
    // recorded outside one), OnFrameCommandRecordingBegan harvests every
    // not-yet-read slot of the pool about to be reused (the slot's frame fence
    // was waited before re-recording, so the results are already available),
    // records a reset of the whole pool, and rewinds the allocation cursor.
    class VkTimerQueryManager {
    public:
        // One vkCmdWriteTimestamp landing spot. Shared (via SharedPtr) between
        // the frontend-held query object and the owning pool's pending list, so
        // deleting a query while its result is still in flight never leaves the
        // pool with a dangling record.
        struct TimestampRecord {
            Uint32 poolIndex = 0;
            Uint32 slot = 0;
            // VkBufferManager frame serial current when the timestamp was
            // recorded; result availability is bounded by its completion.
            Uint64 frameSerial = 0;
            Bool harvested = false;
            // Cleared when the recorded commands were dropped before they could
            // execute (swapchain recreation abandons the in-progress command
            // buffer); the result then reads back as 0.
            Bool valid = true;
            Uint64 rawTicks = 0;
        };

        struct InitInfo {
            VkDevice device = VK_NULL_HANDLE;
            Uint32 frameCount = 0;
            Uint32 timestampValidBits = 0;
            Float timestampPeriodNs = 0.0f; // nanoseconds per timestamp tick
            Uint32 slotsPerPool = 128;
        };

        Bool Initialize(const InitInfo& initInfo);
        // The caller guarantees the device is idle (same contract as the other
        // DirectVulkan managers' Shutdown paths).
        void Shutdown();

        // The per-frame hook described in the class comment. Re-begins within
        // the same frame serial (mid-frame readback submits, the Present layout
        // transition) are skipped so already-written slots survive.
        void OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer, Uint32 frameIndex, Uint64 frameSerial);

        // Allocates a slot from the frame's pool and records a bottom-of-pipe
        // vkCmdWriteTimestamp (valid both inside and outside a render pass).
        // Returns null on pool exhaustion, with one warning per pool cycle; the
        // frontend falls back gracefully on a null handle.
        SharedPtr<TimestampRecord> WriteTimestamp(VkCommandBuffer commandBuffer, Uint32 frameIndex,
                                                  Uint64 frameSerial);

        // Non-blocking single-slot read (WITH_AVAILABILITY, no WAIT). Returns
        // true once the record holds its raw ticks. Callers gate this on the
        // record's frame serial being complete.
        Bool TryHarvest(TimestampRecord& record);

        // Reads every pending result that is available (the caller guarantees
        // the device is idle) and marks the rest invalid. Called when recorded
        // but unsubmitted commands are dropped (swapchain recreation), which
        // would otherwise leave slots that never become available. Each pool is
        // reset lazily on its next OnFrameCommandRecordingBegan.
        void InvalidatePendingRecords();

        // end - begin using unsigned wrap arithmetic masked to the queue's
        // timestampValidBits, converted to nanoseconds. 0 if either record was
        // invalidated.
        Uint64 ElapsedNs(const TimestampRecord& begin, const TimestampRecord& end) const;
        // Raw GPU timestamp converted to nanoseconds. 0 if invalidated.
        Uint64 TimestampNs(const TimestampRecord& record) const;

    private:
        struct PoolState {
            VkQueryPool pool = VK_NULL_HANDLE;
            Uint32 cursor = 0;
            // Frame serial the pool was last harvested + reset for; guards
            // against double resets when recording re-begins mid-frame.
            Uint64 preparedFrameSerial = 0;
            Bool exhaustionWarned = false;
            Vector<SharedPtr<TimestampRecord>> pendingRecords;
        };

        Uint64 MaskToValidBits(Uint64 ticks) const;
        // Harvest (or invalidate, when the result never became available)
        // every pending record of a pool and clear its pending list.
        void DrainPoolPending(PoolState& pool);

        VkDevice m_device = VK_NULL_HANDLE;
        Float m_timestampPeriodNs = 0.0f;
        Uint64 m_validBitsMask = 0;
        Uint32 m_slotsPerPool = 0;
        Vector<PoolState> m_pools;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
