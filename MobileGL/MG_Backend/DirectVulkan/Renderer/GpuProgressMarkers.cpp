// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/GpuProgressMarkers.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "GpuProgressMarkers.h"

#include <MG_Util/Debug/Log.h>

namespace MobileGL::MG_Backend::DirectVulkan {

    namespace Watch = MG_Backend::GpuHangWatch;

    void GpuProgressMarkers::Init(VkDevice device, Uint32 queueFamilyIndex) {
        if (m_device != VK_NULL_HANDLE || device == VK_NULL_HANDLE) return;
        if (Watch::BudgetMs() == 0) {
            MGLOG_I("Magma: GPU hang watch off (MOBILEGL_GPU_HANG_BUDGET_MS=0)");
            return;
        }
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) {
            MGLOG_W("Magma: GPU hang watch off - no command pool for its markers");
            m_pool = VK_NULL_HANDLE;
            return;
        }
        m_device = device;
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            if (!AddSlot()) {
                MGLOG_W("Magma: GPU hang watch off - the device cannot make its progress markers");
                vkDestroyCommandPool(m_device, m_pool, nullptr);
                m_pool = VK_NULL_HANDLE;
                m_device = VK_NULL_HANDLE;
                return;
            }
        }
        m_watch = Watch::Register(this, &Sample, &OnHung);
        MGLOG_I("Magma: GPU hang watch on - a submission the GPU started and has not finished after %u ms ends "
                "its session",
                Watch::BudgetMs());
    }

    void GpuProgressMarkers::Shutdown() {
        // The watch first, outside m_mutex: Unregister waits out a sample in progress, which takes it.
        Watch::Unregister(m_watch);
        m_watch = 0;
        if (m_device == VK_NULL_HANDLE) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        for (Slot& slot : m_slots) {
            if (slot.Started != VK_NULL_HANDLE) vkDestroyEvent(m_device, slot.Started, nullptr);
            if (slot.Finished != VK_NULL_HANDLE) vkDestroyEvent(m_device, slot.Finished, nullptr);
        }
        m_slots.clear();
        if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(m_device, m_pool, nullptr); // frees the buffers
        m_pool = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
    }

    Bool GpuProgressMarkers::AddSlot() {
        if (m_slots.size() >= kMaxSlots) return false;
        Slot slot;
        const VkEventCreateInfo eventInfo{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkCommandBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = m_pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 2;
        const auto fail = [&] {
            if (slot.Started != VK_NULL_HANDLE) vkDestroyEvent(m_device, slot.Started, nullptr);
            if (slot.Finished != VK_NULL_HANDLE) vkDestroyEvent(m_device, slot.Finished, nullptr);
            if (buffers[0] != VK_NULL_HANDLE) vkFreeCommandBuffers(m_device, m_pool, 2, buffers);
            return false;
        };
        if (vkCreateEvent(m_device, &eventInfo, nullptr, &slot.Started) != VK_SUCCESS ||
            vkCreateEvent(m_device, &eventInfo, nullptr, &slot.Finished) != VK_SUCCESS ||
            vkAllocateCommandBuffers(m_device, &allocInfo, buffers) != VK_SUCCESS)
            return fail();
        // Prerecorded once and resubmitted for every submission the slot brackets; SIMULTANEOUS_USE
        // because the slot is reused once its `finished` event is set, which can be a moment before
        // the submission that set it formally leaves the pending state.
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if (vkBeginCommandBuffer(buffers[0], &beginInfo) != VK_SUCCESS) return fail();
        vkCmdSetEvent(buffers[0], slot.Started, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        if (vkEndCommandBuffer(buffers[0]) != VK_SUCCESS) return fail();
        if (vkBeginCommandBuffer(buffers[1], &beginInfo) != VK_SUCCESS) return fail();
        vkCmdSetEvent(buffers[1], slot.Finished, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        if (vkEndCommandBuffer(buffers[1]) != VK_SUCCESS) return fail();
        slot.Begin = buffers[0];
        slot.End = buffers[1];
        m_slots.push_back(slot);
        return true;
    }

    GpuProgressMarkers::Bracket GpuProgressMarkers::Acquire(Uint64 completedSubmitIndex) {
        Bracket bracket;
        if (m_device == VK_NULL_HANDLE) return bracket;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_lost) return bracket;
        Slot* free = nullptr;
        for (Slot& slot : m_slots) {
            if (slot.Claimed) continue;
            // Retire what finished since the watch last looked: frames come faster than its polls.
            if (slot.InFlight && vkGetEventStatus(m_device, slot.Finished) == VK_EVENT_SET) slot.InFlight = false;
            // Free once its submission is complete by fence, not by the event read (see the header).
            if (!slot.InFlight && slot.SubmitIndex <= completedSubmitIndex && free == nullptr) free = &slot;
        }
        if (free == nullptr) {
            if (!AddSlot()) return bracket;
            free = &m_slots.back();
        }
        if (free->Dirty) {
            if (vkResetEvent(m_device, free->Started) != VK_SUCCESS ||
                vkResetEvent(m_device, free->Finished) != VK_SUCCESS)
                return bracket;
            free->Dirty = false;
        }
        free->Claimed = true;
        bracket.Begin = free->Begin;
        bracket.End = free->End;
        bracket.Slot = static_cast<Uint32>(free - m_slots.data());
        return bracket;
    }

    void GpuProgressMarkers::Submitted(const Bracket& bracket, Uint64 submitIndex) {
        if (!bracket.Valid()) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (bracket.Slot >= m_slots.size()) return;
        Slot& slot = m_slots[bracket.Slot];
        slot.Claimed = false;
        slot.InFlight = true;
        slot.Dirty = true;
        slot.SubmitIndex = submitIndex;
        slot.Id = m_nextId++;
    }

    void GpuProgressMarkers::Abandon(const Bracket& bracket) {
        if (!bracket.Valid()) return;
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (bracket.Slot >= m_slots.size()) return;
        // A failed vkQueueSubmit may still have consumed nothing or something: reset before reuse.
        m_slots[bracket.Slot].Claimed = false;
        m_slots[bracket.Slot].Dirty = true;
    }

    Bool GpuProgressMarkers::Sample(void* self, Vector<MG_Backend::GpuHangWatch::SubmissionProgress>& out) {
        auto* markers = static_cast<GpuProgressMarkers*>(self);
        const std::lock_guard<std::mutex> lock(markers->m_mutex);
        if (markers->m_lost || markers->m_device == VK_NULL_HANDLE) return false;
        for (Slot& slot : markers->m_slots) {
            if (!slot.InFlight) continue;
            const VkResult finished = vkGetEventStatus(markers->m_device, slot.Finished);
            const VkResult started = vkGetEventStatus(markers->m_device, slot.Started);
            if ((finished != VK_EVENT_SET && finished != VK_EVENT_RESET) ||
                (started != VK_EVENT_SET && started != VK_EVENT_RESET)) {
                // VK_ERROR_DEVICE_LOST: the driver already reset the device and says so itself.
                markers->m_lost = true;
                return false;
            }
            if (finished == VK_EVENT_SET) {
                slot.InFlight = false;
                continue;
            }
            out.push_back({slot.Id, started == VK_EVENT_SET, false});
        }
        return true;
    }

    void GpuProgressMarkers::OnHung(void* self, Uint64 submissionId, Uint64 runningMs) {
        auto* markers = static_cast<GpuProgressMarkers*>(self);
        markers->m_hungRunningMs.store(runningMs, std::memory_order_release);
        markers->m_hung.store(true, std::memory_order_release);
        MGLOG_E("Magma: submission %llu of this session's VkDevice has run %llu ms on the GPU and has not finished - "
                "it is what stops the GPU for every process; the session ends at its next record and submits "
                "nothing more (budget %u ms, MOBILEGL_GPU_HANG_BUDGET_MS)",
                static_cast<unsigned long long>(submissionId), static_cast<unsigned long long>(runningMs),
                MG_Backend::GpuHangWatch::BudgetMs());
    }

    VkResult GpuProgressMarkers::SubmitBracketed(VkQueue queue, VkSubmitInfo& info, VkFence fence,
                                                 const Bracket& bracket) {
        constexpr Uint32 kMaxOwn = 6;
        if (!bracket.Valid() || info.commandBufferCount == 0 || info.commandBufferCount > kMaxOwn)
            return vkQueueSubmit(queue, 1, &info, fence);
        VkCommandBuffer buffers[kMaxOwn + 2];
        Uint32 count = 0;
        buffers[count++] = bracket.Begin;
        for (Uint32 i = 0; i < info.commandBufferCount; ++i) buffers[count++] = info.pCommandBuffers[i];
        buffers[count++] = bracket.End;
        const VkCommandBuffer* own = info.pCommandBuffers;
        const Uint32 ownCount = info.commandBufferCount;
        info.pCommandBuffers = buffers;
        info.commandBufferCount = count;
        const VkResult result = vkQueueSubmit(queue, 1, &info, fence);
        info.pCommandBuffers = own;
        info.commandBufferCount = ownCount;
        return result;
    }

} // namespace MobileGL::MG_Backend::DirectVulkan
