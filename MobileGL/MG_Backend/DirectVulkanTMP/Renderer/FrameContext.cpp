// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/FrameContext.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FrameContext.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    void FrameContext::Initialize(VulkanContext& ctx, VkCommandPool pool) {
        CommandPool = pool;
        VkCommandBufferAllocateInfo abci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        abci.commandPool = pool;
        abci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        abci.commandBufferCount = 1;
        VK_VERIFY(vkAllocateCommandBuffers(ctx.GetDevice(), &abci, &CommandBuffer), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_VERIFY(vkCreateSemaphore(ctx.GetDevice(), &sci, nullptr, &ImageAvailable), "vkCreateSemaphore");
        VK_VERIFY(vkCreateSemaphore(ctx.GetDevice(), &sci, nullptr, &RenderFinished), "vkCreateSemaphore");

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_VERIFY(vkCreateFence(ctx.GetDevice(), &fci, nullptr, &InFlightFence), "vkCreateFence");
    }

    void FrameContext::Cleanup(VulkanContext& ctx) {
        auto device = ctx.GetDevice();
        if (InFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, InFlightFence, nullptr);
            InFlightFence = VK_NULL_HANDLE;
        }
        if (ImageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, ImageAvailable, nullptr);
            ImageAvailable = VK_NULL_HANDLE;
        }
        if (RenderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, RenderFinished, nullptr);
            RenderFinished = VK_NULL_HANDLE;
        }
        if (CommandBuffer != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, CommandPool, 1, &CommandBuffer);
            CommandBuffer = VK_NULL_HANDLE;
        }
        CommandPool = VK_NULL_HANDLE;
    }
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
