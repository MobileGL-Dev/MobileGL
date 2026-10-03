// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/GpuProgressMarkers.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include "../VkIncludes.h"
#include <Includes.h>
#include <MG_Backend/GpuHangWatch.h>

#include <atomic>
#include <mutex>

namespace MobileGL::MG_Backend::DirectVulkan {

    // WHICH OF THIS DEVICE'S SUBMISSIONS THE GPU HAS STARTED, AND WHICH IT HAS FINISHED
    // (GpuHangWatch.h says why that is the whole attribution of a GPU hang).
    //
    // A frame's submission is bracketed by two prerecorded one-command buffers: the first sets a
    // `started` event at TOP_OF_PIPE, the last sets a `finished` event once everything before it in
    // the queue has completed. The host reads both with vkGetEventStatus from the watch thread - no
    // fence is shared with the renderer's own waits and resets. On Adreno 750 a submission whose
    // fragment shader never ends reads started=SET from its first poll until the kernel resets the
    // GPU, and a submission queued behind it reads started=RESET for the whole stall.
    //
    // The renderer's thread brackets submissions (Acquire / Submitted / Abandon) and asks Hung(); the
    // watch thread samples. Everything Vulkan the two share - the events - is touched under m_mutex.
    class GpuProgressMarkers {
    public:
        struct Bracket {
            VkCommandBuffer Begin = VK_NULL_HANDLE;
            VkCommandBuffer End = VK_NULL_HANDLE;
            Uint32 Slot = ~0u;
            Bool Valid() const { return Slot != ~0u; }
        };

        GpuProgressMarkers() = default;
        ~GpuProgressMarkers() { Shutdown(); }
        GpuProgressMarkers(const GpuProgressMarkers&) = delete;
        GpuProgressMarkers& operator=(const GpuProgressMarkers&) = delete;

        // Off (no brackets, never hung) when the budget is 0 or the device cannot make events.
        void Init(VkDevice device, Uint32 queueFamilyIndex);
        // Before the device goes; its queue must be idle. Stops the watch first.
        void Shutdown();

        // The two buffers to put first and last in one submission (an invalid bracket when off or
        // when every slot is in flight: the submission then goes unwatched).
        Bracket Acquire();
        // The submission carrying `bracket` was accepted by vkQueueSubmit / was not submitted.
        void Submitted(const Bracket& bracket);
        void Abandon(const Bracket& bracket);

        // The watch named one of this device's submissions as hanging the GPU.
        Bool Hung() const { return m_hung.load(std::memory_order_acquire); }
        Uint64 HungRunningMs() const { return m_hungRunningMs.load(std::memory_order_acquire); }
        Bool Active() const { return m_watch != 0; }

        // vkQueueSubmit of `info` with `bracket` wrapped around its command buffers (which it puts
        // back before returning). At most 6 buffers of the caller's.
        static VkResult SubmitBracketed(VkQueue queue, VkSubmitInfo& info, VkFence fence, const Bracket& bracket);

    private:
        struct Slot {
            VkEvent Started = VK_NULL_HANDLE;
            VkEvent Finished = VK_NULL_HANDLE;
            VkCommandBuffer Begin = VK_NULL_HANDLE;
            VkCommandBuffer End = VK_NULL_HANDLE;
            Uint64 Id = 0;
            Bool InFlight = false; // submitted and not yet seen finished
            Bool Claimed = false;  // handed out by Acquire, not yet submitted or abandoned
            Bool Dirty = false;    // its events were set by a past submission: reset before reuse
        };
        static constexpr Uint32 kMaxSlots = 16;

        Bool AddSlot();
        static Bool Sample(void* self, Vector<MG_Backend::GpuHangWatch::SubmissionProgress>& out);
        static void OnHung(void* self, Uint64 submissionId, Uint64 runningMs);

        std::mutex m_mutex;
        VkDevice m_device = VK_NULL_HANDLE;
        VkCommandPool m_pool = VK_NULL_HANDLE;
        Vector<Slot> m_slots;
        Uint64 m_nextId = 1;
        Uint64 m_watch = 0;
        Bool m_lost = false;
        std::atomic<Bool> m_hung{false};
        std::atomic<Uint64> m_hungRunningMs{0};
    };

} // namespace MobileGL::MG_Backend::DirectVulkan
