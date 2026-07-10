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
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        };

        struct PresentInfoPacket {
            VkSemaphore waitSemaphore = VK_NULL_HANDLE;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            Uint32 imageIndex = 0;
            VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        };

        struct FrameData {
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
            VkFence imageInFlightFence = VK_NULL_HANDLE;
            Bool isCommandRecording = false;
            Bool hasCommandBufferRecorded = false;
            Bool imageAvailableSemaphoreConsumed = false;
            // Command buffers submitted mid-frame (FlushPendingCommands) whose
            // execution is only known complete once this slot's fence has been
            // waited again; freed at that point.
            Vector<VkCommandBuffer> retiredCommandBuffers;
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
        // buffers are freed after the slot's fence is next waited.
        VkResult RetireCurrentCommandBuffer();

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
