// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTimerQueryManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkTimerQueryManager.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    Bool VkTimerQueryManager::Initialize(const InitInfo& initInfo) {
        Shutdown();

        MOBILEGL_ASSERT(initInfo.device != VK_NULL_HANDLE, "VkTimerQueryManager::Initialize requires valid VkDevice");
        MOBILEGL_ASSERT(initInfo.frameCount > 0, "VkTimerQueryManager::Initialize requires non-zero frame count");
        if (initInfo.timestampValidBits == 0 || initInfo.timestampPeriodNs <= 0.0f || initInfo.slotsPerPool == 0) {
            MGLOG_W_ONCE("VkTimerQueryManager: timestamps unsupported (validBits=%u, period=%f, slots=%u)",
                    initInfo.timestampValidBits, initInfo.timestampPeriodNs, initInfo.slotsPerPool);
            return false;
        }

        m_device = initInfo.device;
        m_timestampPeriodNs = initInfo.timestampPeriodNs;
        m_validBitsMask = initInfo.timestampValidBits >= 64
                              ? ~0ull
                              : ((1ull << initInfo.timestampValidBits) - 1ull);
        m_slotsPerPool = initInfo.slotsPerPool;
        m_pools.resize(initInfo.frameCount);

        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = m_slotsPerPool;
        for (auto& poolState : m_pools) {
            const VkResult result = vkCreateQueryPool(m_device, &poolInfo, nullptr, &poolState.pool);
            if (result != VK_SUCCESS) {
                MGLOG_E_ONCE("VkTimerQueryManager: vkCreateQueryPool failed with %s", VkResultToString(result));
                Shutdown();
                return false;
            }
        }
        return true;
    }

    void VkTimerQueryManager::Shutdown() {
        if (m_device != VK_NULL_HANDLE) {
            for (auto& poolState : m_pools) {
                if (poolState.pool != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(m_device, poolState.pool, nullptr);
                }
            }
        }
        // Records the frontend still holds simply stay unharvested; their
        // results read back as 0.
        m_pools.clear();
        m_device = VK_NULL_HANDLE;
        m_timestampPeriodNs = 0.0f;
        m_validBitsMask = 0;
        m_slotsPerPool = 0;
    }

    void VkTimerQueryManager::OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer, Uint32 frameIndex,
                                                           Uint64 frameSerial) {
        MOBILEGL_ASSERT(frameIndex < m_pools.size(), "VkTimerQueryManager frame index out of range");
        auto& poolState = m_pools[frameIndex];
        if (poolState.preparedFrameSerial == frameSerial) {
            // Recording re-began within the same frame (mid-frame readback
            // submit or the Present layout transition); the pool was already
            // harvested and reset for this cycle, and resetting again would
            // clobber timestamps written earlier in the frame.
            return;
        }

        // Harvest what the pool's previous cycle left behind. The frame slot's
        // fence was waited before re-recording, so every executed query is
        // already available and the reads return immediately.
        DrainPoolPending(poolState);

        vkCmdResetQueryPool(commandBuffer, poolState.pool, 0, m_slotsPerPool);
        poolState.cursor = 0;
        poolState.exhaustionWarned = false;
        poolState.preparedFrameSerial = frameSerial;
    }

    SharedPtr<VkTimerQueryManager::TimestampRecord> VkTimerQueryManager::WriteTimestamp(VkCommandBuffer commandBuffer,
                                                                                        Uint32 frameIndex,
                                                                                        Uint64 frameSerial) {
        MOBILEGL_ASSERT(frameIndex < m_pools.size(), "VkTimerQueryManager frame index out of range");
        auto& poolState = m_pools[frameIndex];
        if (poolState.cursor >= m_slotsPerPool) {
            if (!poolState.exhaustionWarned) {
                MGLOG_W_ONCE("VkTimerQueryManager: frame %u timestamp pool exhausted (%u slots); further timer queries "
                        "this frame fall back to the frontend path",
                        frameIndex, m_slotsPerPool);
                poolState.exhaustionWarned = true;
            }
            return nullptr;
        }

        auto record = MakeShared<TimestampRecord>();
        record->poolIndex = frameIndex;
        record->slot = poolState.cursor++;
        record->frameSerial = frameSerial;
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, poolState.pool, record->slot);
        poolState.pendingRecords.push_back(record);
        return record;
    }

    Bool VkTimerQueryManager::TryHarvest(TimestampRecord& record) {
        if (record.harvested) {
            return true;
        }
        if (m_device == VK_NULL_HANDLE || record.poolIndex >= m_pools.size()) {
            return false;
        }

        Uint64 resultWithAvailability[2] = {0, 0};
        const VkResult result = vkGetQueryPoolResults(
            m_device, m_pools[record.poolIndex].pool, record.slot, 1, sizeof(resultWithAvailability),
            resultWithAvailability, sizeof(Uint64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            MGLOG_E_ONCE("VkTimerQueryManager: vkGetQueryPoolResults failed with %s", VkResultToString(result));
            return false;
        }
        if (resultWithAvailability[1] == 0) {
            return false;
        }
        record.rawTicks = resultWithAvailability[0];
        record.harvested = true;
        return true;
    }

    void VkTimerQueryManager::InvalidatePendingRecords() {
        for (auto& poolState : m_pools) {
            DrainPoolPending(poolState);
            // Force a harvest-free reset cycle the next time this pool's frame
            // begins recording.
            poolState.preparedFrameSerial = 0;
        }
    }

    void VkTimerQueryManager::DrainPoolPending(PoolState& poolState) {
        for (auto& record : poolState.pendingRecords) {
            if (record->harvested) {
                continue;
            }
            if (!TryHarvest(*record)) {
                // The commands carrying this timestamp never executed (they
                // were dropped, e.g. by a swapchain recreation mid-frame).
                // Mark the record resolved-as-invalid so waits on it cannot
                // hang; its result reads back as 0.
                record->harvested = true;
                record->valid = false;
            }
        }
        poolState.pendingRecords.clear();
    }

    Uint64 VkTimerQueryManager::MaskToValidBits(Uint64 ticks) const {
        return ticks & m_validBitsMask;
    }

    Uint64 VkTimerQueryManager::ElapsedNs(const TimestampRecord& begin, const TimestampRecord& end) const {
        if (!begin.valid || !end.valid) {
            return 0;
        }
        const Uint64 deltaTicks = MaskToValidBits(end.rawTicks - begin.rawTicks);
        return static_cast<Uint64>(static_cast<double>(deltaTicks) * static_cast<double>(m_timestampPeriodNs));
    }

    Uint64 VkTimerQueryManager::TimestampNs(const TimestampRecord& record) const {
        if (!record.valid) {
            return 0;
        }
        return static_cast<Uint64>(static_cast<double>(MaskToValidBits(record.rawTicks)) *
                                   static_cast<double>(m_timestampPeriodNs));
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
