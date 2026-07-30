// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

#include <Includes.h>
#include "../VkIncludes.h"

namespace MobileGL::MG_Backend::DirectVulkan {
    class SwapchainObject {
    public:
        struct SwapchainCapabilities {
            VkSurfaceCapabilitiesKHR capabilities;
            Vector<VkSurfaceFormatKHR> surfaceFormats;
            Vector<VkPresentModeKHR> presentModes;

            Bool IsComplete() const {
                return !surfaceFormats.empty() && !presentModes.empty();
            }
        };

        static SwapchainCapabilities GetSwapchainCapabilities(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);
        static VkSurfaceFormatKHR ChooseSwapchainSurfaceFormat(const Vector<VkSurfaceFormatKHR>& availableFormats);
        static VkPresentModeKHR ChooseSwapchainPresentMode(const Vector<VkPresentModeKHR>& availablePresentModes);

        void Create(VkDevice device, VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, Uint32 graphicsQueueFamily,
                    Uint32 presentQueueFamily, Uint32 minImageCountHint, VkExtent2D desiredExtent);
        void Shutdown(VkDevice device);

        VkSwapchainKHR GetHandle() const { return m_swapchain; }
        const VkSurfaceFormatKHR& GetSurfaceFormat() const { return m_surfaceFormat; }
        VkExtent2D GetExtent() const { return m_extent; }
        // Surface-space extent (before the pre-rotation quarter-turn swap) this swapchain was
        // created from - the value to compare a freshly queried currentExtent against.
        VkExtent2D GetSurfaceExtent() const { return m_surfaceExtent; }
        VkSurfaceTransformFlagBitsKHR GetPreTransform() const { return m_preTransform; }
        const Vector<VkImage>& GetImages() const { return m_images; }
        const Vector<VkImageView>& GetImageViews() const { return m_imageViews; }
        VkFormat GetDepthStencilFormat() const { return m_depthStencilFormat; }
        const Vector<VkImageView>& GetDepthStencilImageViews() const { return m_depthStencilImageViews; }
        VkImage GetDepthStencilImage(Uint32 index) const;
        VkImageView GetDepthStencilImageView(Uint32 index) const;
        VkImageLayout GetDepthStencilImageLayout(Uint32 index) const;
        void SetDepthStencilImageLayout(Uint32 index, VkImageLayout layout);
        VkImage GetImage(Uint32 index) const;
        VkImageLayout GetImageLayout(Uint32 index) const;
        void SetImageLayout(Uint32 index, VkImageLayout layout);
        SizeT GetImageCount() const { return m_images.size(); }

        // EGL content-validity tracking for the default framebuffer. A color
        // buffer's content is undefined once its image has been presented
        // (EGL_BUFFER_DESTROYED swap behaviour, the implementation default),
        // and every ancillary (depth/stencil) buffer's content is undefined
        // after ANY swap regardless of swap behaviour (EGL 1.5 §3.10.1). The
        // render-pass manager turns an undefined attachment's tile load into
        // LOAD_OP_DONT_CARE. Flags start false (a fresh swapchain image holds
        // garbage) and a render pass storing into an attachment sets it back
        // to defined.
        Bool IsImageContentDefined(Uint32 index) const;
        void SetImageContentDefined(Uint32 index, Bool defined);
        Bool IsDepthStencilContentDefined(Uint32 index) const;
        void SetDepthStencilContentDefined(Uint32 index, Bool defined);
        void SetAllDepthStencilContentUndefined();

    private:
        void CreateImageViews(VkDevice device);
        void CreateDepthStencilResources(VkDevice device, VkPhysicalDevice physicalDevice);
        void DestroyDepthStencilResources(VkDevice device);
        static constexpr VkPresentModeKHR s_desiredPresentModes[] {
            VK_PRESENT_MODE_MAILBOX_KHR,
            VK_PRESENT_MODE_IMMEDIATE_KHR,
            VK_PRESENT_MODE_FIFO_RELAXED_KHR,
            VK_PRESENT_MODE_FIFO_KHR
        };

        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR m_surfaceFormat{};
        VkExtent2D m_extent{};
        VkExtent2D m_surfaceExtent{};
        VkSurfaceTransformFlagBitsKHR m_preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        Vector<VkImage> m_images;
        Vector<VkImageView> m_imageViews;
        Vector<VkImageLayout> m_imageLayouts;

        VkFormat m_depthStencilFormat = VK_FORMAT_UNDEFINED;
        Vector<VkImage> m_depthStencilImages;
        Vector<VkDeviceMemory> m_depthStencilImageMemories;
        Vector<VkImageView> m_depthStencilImageViews;
        Vector<VkImageLayout> m_depthStencilImageLayouts;
        Vector<Bool> m_imageContentDefined;
        Vector<Bool> m_depthStencilContentDefined;
    };
} // namespace MobileGL::MG_Backend::DirectVulkan
