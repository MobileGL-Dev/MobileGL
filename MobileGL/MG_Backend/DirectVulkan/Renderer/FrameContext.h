// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/FrameContext.h
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
    class FrameContext {
    public:
        // Notified immediately after a frame command buffer begins recording
        // (before any render pass has been begun); every BeginCommandRecording
        // caller funnels through this single seam. Implemented by the renderer
        // to prepare per-frame timer-query pools (vkCmdResetQueryPool must be
        // recorded outside a render pass).
        class IRecordingObserver {
        public:
            virtual ~IRecordingObserver() = default;
            virtual void OnFrameCommandRecordingBegan(VkCommandBuffer commandBuffer) = 0;
        };

        struct SubmitInfoPacket {
            VkPipelineStageFlags waitDstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkSemaphore waitSemaphore = VK_NULL_HANDLE;
            VkSemaphore signalSemaphore = VK_NULL_HANDLE;
            // [0] = pre-pass command buffer (when recorded), then the frame
            // command buffer; submitInfo.pCommandBuffers points here.
            VkCommandBuffer commandBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        };

        struct PresentInfoPacket {
            VkSemaphore waitSemaphore = VK_NULL_HANDLE;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            Uint32 imageIndex = 0;
            VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        };

        // A command buffer submitted mid-frame (FlushPendingCommands), tagged
        // with the submit-tracker index it was submitted under so it can be
        // freed as soon as that submission is observed complete - without
        // waiting for the slot's fence to be waited again (present-less flush
        // loops never wait it).
        struct RetiredCommandBuffer {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            Uint64 submitIndex = 0;
        };

        struct FrameData {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            // Pre-pass work stream: out-of-pass commands (deferred clear
            // materialization, sampled-layout transitions) for resources the
            // frame's recording has not touched yet. Submitted immediately
            // BEFORE commandBuffer in the same vkQueueSubmit, so recording
            // into it never has to split the frame's active render pass.
            VkCommandBuffer preCommandBuffer = VK_NULL_HANDLE;
            VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
            VkFence imageInFlightFence = VK_NULL_HANDLE;
            Bool isCommandRecording = false;
            Bool hasCommandBufferRecorded = false;
            Bool isPreCommandRecording = false;
            Bool hasPreCommandBufferRecorded = false;
            Bool imageAvailableSemaphoreConsumed = false;
            // Command buffers submitted mid-frame (FlushPendingCommands),
            // appended in submit order; freed once their submission is known
            // complete (fence wait or completion poll).
            Vector<RetiredCommandBuffer> retiredCommandBuffers;
            // Submit-tracker index of this slot's most recent queue submission
            // (written by the renderer at submit time).
            Uint64 lastSubmitIndex = 0;
        };

        VkResult Initialize(VkDevice device, VkCommandPool commandPool, Uint32 frameCount);
        void Destroy(VkDevice device, VkCommandPool commandPool);

        // Lifecycle functions
        FrameData& GetCurrent();
        const FrameData& GetCurrent() const;
        Bool IsCommandRecording() const;
        void AdvanceToNext();
        VkCommandBuffer& BeginCommandRecording(VkCommandBufferUsageFlags flags = 0,
                                               const VkCommandBufferInheritanceInfo* pInheritanceInfo = nullptr);
        void EndCommandRecording();
        // Lazily opens the pre-pass work stream (see FrameData::preCommandBuffer).
        VkCommandBuffer BeginPreCommandRecording();
        // Closes the pre stream if open, marking it for submission ahead of the
        // frame command buffer. Safe to call when it never opened.
        void EndPreCommandRecordingIfOpen();
        // Drops an in-progress or recorded-but-unsubmitted pre stream (dropped
        // frame recordings, swapchain recreation).
        void AbandonPreCommandRecording();
        VkResult InitializeSwapchainSemaphores(VkDevice device, Uint32 swapchainImageCount);
        void DestroySwapchainSemaphores(VkDevice device);
        Bool TransitionToPresent(VkImage image, VkImageLayout oldLayout,
                                 VkImageLayout presentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        SubmitInfoPacket GetSubmitInfo(Bool shouldSubmitCommandBuffer, Uint32 swapchainImageIndex) const;
        PresentInfoPacket GetPresentInfo(VkSwapchainKHR swapchain, Uint32 imageIndex) const;
        VkResult WaitAndAcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, Uint32& outImageIndex,
                                         Uint64 timeout = UINT64_MAX, VkFence acquireFence = VK_NULL_HANDLE);

        // Parks the current (already ended and submitted) command buffer on the
        // slot's retired list and installs a freshly allocated one, so recording
        // can restart while the submitted buffer is still executing. Retired
        // buffers are freed after the slot's fence is next waited, or as soon
        // as their submission is observed complete.
        VkResult RetireCurrentCommandBuffer(Bool retirePreCommandBuffer = false);

        // Frees every retired command buffer whose tagged submission index is
        // known complete. Driven by the renderer's submit tracker on completion
        // events (fence waits and non-blocking polls), so present-less flush
        // loops reclaim their buffers without any extra wait.
        void FreeRetiredCommandBuffersCompletedUpTo(Uint64 completedSubmitIndex);
        // Frees every slot's retired command buffers. Only valid when the
        // caller has proven every queue submission complete.
        void FreeAllRetiredCommandBuffers();

        Uint32 GetCurrentFrameIndex() const;
        Uint32 GetFrameCount() const;

        // Observer may be null (no notifications). Not owned.
        void SetRecordingObserver(IRecordingObserver* observer);

    private:
        void AssertValidFrameIndex(Uint32 frameIndex) const;
        void AssertValidSwapchainImageIndex(Uint32 imageIndex) const;

        VkResult CreateSyncObjectsForFrame(VkDevice device, Uint32 frameIndex,
                                           const VkSemaphoreCreateInfo& semaphoreInfo,
                                           const VkFenceCreateInfo& fenceInfo);
        void DestroySyncObjectsForFrame(VkDevice device, Uint32 frameIndex);
        void FreeRetiredCommandBuffers(FrameData& frame);

        Vector<FrameData> m_frames;
        Vector<VkSemaphore> m_swapchainImageRenderFinishedSemaphores;
        Uint32 currentFrameIndex = 0;
        IRecordingObserver* m_recordingObserver = nullptr;
        // Stored at Initialize for retired-command-buffer management.
        VkDevice m_device = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
