// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/FrameContext.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "VulkanContext.h"
#include "VkCommon.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP {
    struct TrashBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Bool mapped = false;
    };
    struct TrashImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        Vector<VkImageView> mipViews;
    };
    namespace VkManager {
        struct FrameContext {
            VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
            VkSemaphore ImageAvailable = VK_NULL_HANDLE;
            VkSemaphore RenderFinished = VK_NULL_HANDLE;
            VkFence InFlightFence = VK_NULL_HANDLE;
            Uint32 CurrentImageIndex = 0;
            VkCommandPool CommandPool = VK_NULL_HANDLE;
            Vector<TrashBuffer> TrashBuffers;
            Vector<TrashImage> TrashImages;
            // VkCommandPool CommandPool = VK_NULL_HANDLE;

            void Initialize(VulkanContext& ctx, VkCommandPool pool);
            void Cleanup(VulkanContext& ctx);
        };
    } // namespace VkManager
} // namespace MobileGL::MG_Backend::DirectVulkanTMP
