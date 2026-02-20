// MobileGL - MobileGL/MG_Backend/DirectVulkanTMP/Renderer/SwapchainManager.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include "MG_Util/Math/VectorTypes.h"
#include "VulkanContext.h"
#include "VkCommon.h"

namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager {
    class SwapchainManager {
    public:
        explicit SwapchainManager(VulkanContext& ctx) : m_ctx(ctx) {}
        ~SwapchainManager();

        SwapchainManager(const SwapchainManager&) = delete;
        SwapchainManager& operator=(const SwapchainManager&) = delete;

        void Initialize();
        void Recreate();
        void SetViewportSize(const UintVec2& size) { m_viewportSize = size; }

        VkSwapchainKHR GetSwapchain() const { return m_swapchain; }
        VkFormat GetFormat() const { return m_format; }
        VkExtent2D GetExtent() const { return m_extent; }
        const Vector<VkImage>& GetImages() const { return m_images; }
        const Vector<VkImageView>& GetImageViews() const { return m_imageViews; }
        const Vector<VkFramebuffer>& GetFramebuffers() const { return m_framebuffers; }
        void SetFramebuffers(Vector<VkFramebuffer>&& framebuffers);
        Vector<VkFence>& GetImagesInFlight() { return m_imagesInFlight; }
        const UintVec2& GetViewportSize() const { return m_viewportSize; }

    private:
        void CreateSwapchain(VkSwapchainKHR oldSwapchain);
        void DestroySwapchain();

        VulkanContext& m_ctx;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkFormat m_format = VK_FORMAT_UNDEFINED;
        VkExtent2D m_extent{0, 0};
        Vector<VkImage> m_images;
        Vector<VkImageView> m_imageViews;
        Vector<VkFramebuffer> m_framebuffers;
        Vector<VkFence> m_imagesInFlight;
        UintVec2 m_viewportSize{0, 0};
    };
} // namespace MobileGL::MG_Backend::DirectVulkanTMP::VkManager
