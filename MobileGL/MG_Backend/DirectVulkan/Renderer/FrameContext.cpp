// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/FrameContext.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FrameContext.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    VkResult FrameContext::Initialize(VkDevice device, VkCommandPool commandPool, Uint32 frameCount) {
        Destroy(device, commandPool);
        m_frames.assign(frameCount, {});
        currentFrameIndex = 0;
        m_device = device;
        m_commandPool = commandPool;

        Vector<VkCommandBuffer> commandBuffers(frameCount, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = frameCount;
        VkResult result = vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
        if (result != VK_SUCCESS) {
            return result;
        }
        for (Uint32 i = 0; i < frameCount; ++i) {
            m_frames[i].commandBuffer = commandBuffers[i];
        }

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (Uint32 i = 0; i < frameCount; ++i) {
            result = CreateSyncObjectsForFrame(device, i, semaphoreInfo, fenceInfo);
            if (result != VK_SUCCESS) {
                Destroy(device, commandPool);
                return result;
            }
        }

        return VK_SUCCESS;
    }

    void FrameContext::Destroy(VkDevice device, VkCommandPool commandPool) {
        const Uint32 frameCount = static_cast<Uint32>(m_frames.size());
        Vector<VkCommandBuffer> commandBuffers(frameCount, VK_NULL_HANDLE);
        for (Uint32 i = 0; i < frameCount; ++i) {
            commandBuffers[i] = m_frames[i].commandBuffer;
        }

        for (Uint32 i = 0; i < frameCount; ++i) {
            DestroySyncObjectsForFrame(device, i);
        }
        DestroySwapchainSemaphores(device);
        if (device != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE && !m_frames.empty()) {
            for (auto& frame : m_frames) {
                FreeRetiredCommandBuffers(frame);
            }
            vkFreeCommandBuffers(device, commandPool, frameCount, commandBuffers.data());
        }
        m_frames.clear();
        currentFrameIndex = 0;
        m_device = VK_NULL_HANDLE;
        m_commandPool = VK_NULL_HANDLE;
    }

    FrameContext::FrameData& FrameContext::GetCurrent() {
        MOBILEGL_ASSERT(!m_frames.empty(), "FrameContext is not initialized");
        return m_frames[currentFrameIndex];
    }

    const FrameContext::FrameData& FrameContext::GetCurrent() const {
        MOBILEGL_ASSERT(!m_frames.empty(), "FrameContext is not initialized");
        return m_frames[currentFrameIndex];
    }

    Bool FrameContext::IsCommandRecording() const {
        return GetCurrent().isCommandRecording;
    }

    void FrameContext::AdvanceToNext() {
        MOBILEGL_ASSERT(!m_frames.empty(), "FrameContext is not initialized");
        currentFrameIndex = (currentFrameIndex + 1) % static_cast<Uint32>(m_frames.size());
        GetCurrent().isCommandRecording = false;
        GetCurrent().hasCommandBufferRecorded = false;
    }

    VkCommandBuffer& FrameContext::BeginCommandRecording(VkCommandBufferUsageFlags flags,
                                                         const VkCommandBufferInheritanceInfo* pInheritanceInfo) {
        auto& frame = GetCurrent();
        MOBILEGL_ASSERT(!frame.isCommandRecording, "BeginCommandRecording called while command buffer is already recording");

        frame.hasCommandBufferRecorded = false;
        VK_VERIFY(vkResetCommandBuffer(frame.commandBuffer, 0), "BeginCommandRecording, vkResetCommandBuffer");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = flags;
        beginInfo.pInheritanceInfo = pInheritanceInfo;
        VK_VERIFY(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "BeginCommandRecording, vkBeginCommandBuffer");

        frame.isCommandRecording = true;
        if (m_recordingObserver != nullptr) {
            m_recordingObserver->OnFrameCommandRecordingBegan(frame.commandBuffer);
        }
        return frame.commandBuffer;
    }

    void FrameContext::EndCommandRecording() {
        auto& frame = GetCurrent();
        MOBILEGL_ASSERT(frame.isCommandRecording, "EndCommandRecording called without active command buffer recording");
        VK_VERIFY(vkEndCommandBuffer(frame.commandBuffer), "EndCommandRecording, vkEndCommandBuffer");
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = true;
    }

    VkResult FrameContext::InitializeSwapchainSemaphores(VkDevice device, Uint32 swapchainImageCount) {
        DestroySwapchainSemaphores(device);
        if (swapchainImageCount == 0) {
            return VK_SUCCESS;
        }

        m_swapchainImageRenderFinishedSemaphores.assign(swapchainImageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (Uint32 imageIndex = 0; imageIndex < swapchainImageCount; ++imageIndex) {
            VkResult result =
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &m_swapchainImageRenderFinishedSemaphores[imageIndex]);
            if (result != VK_SUCCESS) {
                DestroySwapchainSemaphores(device);
                return result;
            }
        }
        return VK_SUCCESS;
    }

    void FrameContext::DestroySwapchainSemaphores(VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            for (auto semaphore : m_swapchainImageRenderFinishedSemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
        }
        m_swapchainImageRenderFinishedSemaphores.clear();
    }

    Bool FrameContext::TransitionToPresent(VkImage image, VkImageLayout oldLayout, VkImageLayout presentLayout) {
        auto& frame = GetCurrent();
        if (frame.hasCommandBufferRecorded || frame.isCommandRecording || oldLayout == presentLayout ||
            oldLayout == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) {
            return false;
        }

        auto& commandBuffer = BeginCommandRecording();

        VkImageMemoryBarrier presentBarrier{};
        presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        presentBarrier.srcAccessMask = 0;
        presentBarrier.dstAccessMask = 0;
        presentBarrier.oldLayout = oldLayout;
        presentBarrier.newLayout = presentLayout;
        presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.image = image;
        presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        presentBarrier.subresourceRange.baseMipLevel = 0;
        presentBarrier.subresourceRange.levelCount = 1;
        presentBarrier.subresourceRange.baseArrayLayer = 0;
        presentBarrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &presentBarrier);

        EndCommandRecording();
        return true;
    }

    FrameContext::SubmitInfoPacket FrameContext::GetSubmitInfo(Bool shouldSubmitCommandBuffer,
                                                               Uint32 swapchainImageIndex) const {
        const auto& frame = GetCurrent();
        MOBILEGL_ASSERT(!frame.isCommandRecording, "GetSubmitInfo called while command buffer recording is still active");
        AssertValidSwapchainImageIndex(swapchainImageIndex);
        SubmitInfoPacket packet{};
        packet.waitSemaphore = frame.imageAvailableSemaphore;
        packet.signalSemaphore = m_swapchainImageRenderFinishedSemaphores[swapchainImageIndex];
        packet.commandBuffer = frame.commandBuffer;

        packet.submitInfo.waitSemaphoreCount = frame.imageAvailableSemaphoreConsumed ? 0U : 1U;
        packet.submitInfo.pWaitSemaphores = frame.imageAvailableSemaphoreConsumed ? nullptr : &packet.waitSemaphore;
        packet.submitInfo.pWaitDstStageMask = frame.imageAvailableSemaphoreConsumed ? nullptr : &packet.waitDstStageMask;
        packet.submitInfo.commandBufferCount = shouldSubmitCommandBuffer ? 1U : 0U;
        packet.submitInfo.pCommandBuffers = shouldSubmitCommandBuffer ? &packet.commandBuffer : nullptr;
        packet.submitInfo.signalSemaphoreCount = 1;
        packet.submitInfo.pSignalSemaphores = &packet.signalSemaphore;
        return packet;
    }

    FrameContext::PresentInfoPacket FrameContext::GetPresentInfo(VkSwapchainKHR swapchain, Uint32 imageIndex) const {
        AssertValidSwapchainImageIndex(imageIndex);
        PresentInfoPacket packet{};
        packet.waitSemaphore = m_swapchainImageRenderFinishedSemaphores[imageIndex];
        packet.swapchain = swapchain;
        packet.imageIndex = imageIndex;

        packet.presentInfo.waitSemaphoreCount = 1;
        packet.presentInfo.pWaitSemaphores = &packet.waitSemaphore;
        packet.presentInfo.swapchainCount = 1;
        packet.presentInfo.pSwapchains = &packet.swapchain;
        packet.presentInfo.pImageIndices = &packet.imageIndex;
        packet.presentInfo.pResults = nullptr;
        return packet;
    }

    VkResult FrameContext::WaitAndAcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, Uint32& outImageIndex,
                                                   Uint64 timeout, VkFence acquireFence) {
        auto& frame = GetCurrent();
        VkResult result = vkWaitForFences(device, 1, &frame.imageInFlightFence, VK_TRUE, timeout);
        if (result != VK_SUCCESS) {
            return result;
        }
        // The slot's fence has been waited: every command buffer this slot
        // submitted (including mid-frame flushes) has finished executing.
        FreeRetiredCommandBuffers(frame);

        result = vkAcquireNextImageKHR(device, swapchain, timeout, frame.imageAvailableSemaphore, acquireFence,
                                       &outImageIndex);
        if (result != VK_SUCCESS) {
            return result;
        }

        frame.imageAvailableSemaphoreConsumed = false;
        return vkResetFences(device, 1, &frame.imageInFlightFence);
    }

    Uint32 FrameContext::GetCurrentFrameIndex() const {
        return currentFrameIndex;
    }

    Uint32 FrameContext::GetFrameCount() const {
        return static_cast<Uint32>(m_frames.size());
    }

    void FrameContext::SetRecordingObserver(IRecordingObserver* observer) {
        m_recordingObserver = observer;
    }

    VkResult FrameContext::RetireCurrentCommandBuffer() {
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE,
                        "RetireCurrentCommandBuffer requires an initialized FrameContext");
        auto& frame = GetCurrent();
        MOBILEGL_ASSERT(!frame.isCommandRecording,
                        "RetireCurrentCommandBuffer called while the command buffer is still recording");

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer replacement = VK_NULL_HANDLE;
        const VkResult result = vkAllocateCommandBuffers(m_device, &allocInfo, &replacement);
        if (result != VK_SUCCESS) {
            return result;
        }
        // lastSubmitIndex was just written by the renderer for the submission
        // that carried this command buffer.
        frame.retiredCommandBuffers.push_back({frame.commandBuffer, frame.lastSubmitIndex});
        frame.commandBuffer = replacement;
        return VK_SUCCESS;
    }

    void FrameContext::FreeRetiredCommandBuffers(FrameData& frame) {
        if (frame.retiredCommandBuffers.empty()) {
            return;
        }
        if (m_device != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE) {
            for (const auto& retired : frame.retiredCommandBuffers) {
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &retired.commandBuffer);
            }
        }
        frame.retiredCommandBuffers.clear();
    }

    void FrameContext::FreeRetiredCommandBuffersCompletedUpTo(Uint64 completedSubmitIndex) {
        if (m_device == VK_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE) {
            return;
        }
        for (auto& frame : m_frames) {
            // Retired buffers are appended in submit order, so the completed
            // ones form a prefix.
            SizeT completedCount = 0;
            while (completedCount < frame.retiredCommandBuffers.size() &&
                   frame.retiredCommandBuffers[completedCount].submitIndex <= completedSubmitIndex) {
                vkFreeCommandBuffers(m_device, m_commandPool, 1,
                                     &frame.retiredCommandBuffers[completedCount].commandBuffer);
                ++completedCount;
            }
            if (completedCount > 0) {
                frame.retiredCommandBuffers.erase(frame.retiredCommandBuffers.begin(),
                                                  frame.retiredCommandBuffers.begin() + completedCount);
            }
        }
    }

    void FrameContext::FreeAllRetiredCommandBuffers() {
        for (auto& frame : m_frames) {
            FreeRetiredCommandBuffers(frame);
        }
    }

    void FrameContext::AssertValidFrameIndex(Uint32 frameIndex) const {
        MOBILEGL_ASSERT(frameIndex < m_frames.size(), "FrameContext index out of range");
    }

    void FrameContext::AssertValidSwapchainImageIndex(Uint32 imageIndex) const {
        MOBILEGL_ASSERT(imageIndex < m_swapchainImageRenderFinishedSemaphores.size(),
                        "FrameContext swapchain image index out of range");
    }

    VkResult FrameContext::CreateSyncObjectsForFrame(VkDevice device, Uint32 frameIndex,
                                                     const VkSemaphoreCreateInfo& semaphoreInfo,
                                                     const VkFenceCreateInfo& fenceInfo) {
        AssertValidFrameIndex(frameIndex);
        DestroySyncObjectsForFrame(device, frameIndex);

        auto& frame = m_frames[frameIndex];
        VkResult result =
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailableSemaphore);
        if (result != VK_SUCCESS) {
            return result;
        }

        result = vkCreateFence(device, &fenceInfo, nullptr, &frame.imageInFlightFence);
        if (result != VK_SUCCESS) {
            vkDestroySemaphore(device, frame.imageAvailableSemaphore, nullptr);
            frame.imageAvailableSemaphore = VK_NULL_HANDLE;
            return result;
        }

        frame.hasCommandBufferRecorded = false;
        frame.isCommandRecording = false;
        frame.imageAvailableSemaphoreConsumed = false;
        return VK_SUCCESS;
    }

    void FrameContext::DestroySyncObjectsForFrame(VkDevice device, Uint32 frameIndex) {
        AssertValidFrameIndex(frameIndex);
        auto& frame = m_frames[frameIndex];
        if (device != VK_NULL_HANDLE && frame.imageInFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.imageInFlightFence, nullptr);
        }
        frame.imageInFlightFence = VK_NULL_HANDLE;

        if (device != VK_NULL_HANDLE && frame.imageAvailableSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailableSemaphore, nullptr);
        }
        frame.imageAvailableSemaphore = VK_NULL_HANDLE;
        frame.isCommandRecording = false;
        frame.hasCommandBufferRecorded = false;
        frame.imageAvailableSemaphoreConsumed = false;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
