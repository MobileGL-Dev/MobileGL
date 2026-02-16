// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SwapchainManager.h"

namespace MobileGL::MG_Backend::DirectVulkan::VkManager {
    namespace {
        VkSurfaceFormatKHR ChooseSurfaceFormat(const Vector<VkSurfaceFormatKHR>& formats) {
            for (const auto& f : formats) {
                if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
                    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    return f;
                }
            }
            return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                   : formats[0];
        }

        VkPresentModeKHR ChoosePresentMode(const Vector<VkPresentModeKHR>& modes) {
            for (const auto& m : modes) {
                if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& caps, ANativeWindow* window) {
            if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
            // try to get frontend viewport size
            VkExtent2D extent{640, 480};
#ifdef VK_USE_PLATFORM_ANDROID_KHR
            if (window) {
                extent.width = static_cast<Uint32>(ANativeWindow_getWidth(window));
                extent.height = static_cast<Uint32>(ANativeWindow_getHeight(window));
            }
#else
            (void)window;
#endif
            extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, extent.width));
            extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, extent.height));
            return extent;
        }
    } // namespace

    SwapchainManager::~SwapchainManager() {
        DestroySwapchain();
    }

    void SwapchainManager::Initialize() {
        CreateSwapchain(VK_NULL_HANDLE);
    }

    void SwapchainManager::Recreate() {
        DestroySwapchain();
        CreateSwapchain(VK_NULL_HANDLE);
    }

    void SwapchainManager::SetFramebuffers(Vector<VkFramebuffer>&& framebuffers) {
        m_framebuffers = Move(framebuffers);
    }

    void SwapchainManager::DestroySwapchain() {
        auto device = m_ctx.GetDevice();
        if (device == VK_NULL_HANDLE) return;
        for (auto fb : m_framebuffers) {
            if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
        }
        m_framebuffers.clear();

        for (auto view : m_imageViews) {
            if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        }
        m_imageViews.clear();
        m_images.clear();
        m_imagesInFlight.clear();

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
        m_format = VK_FORMAT_UNDEFINED;
        m_extent = {0, 0};
    }

    void SwapchainManager::CreateSwapchain(VkSwapchainKHR oldSwapchain) {
        VkSurfaceCapabilitiesKHR caps{};
        VK_VERIFY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.GetPhysicalDevice(), m_ctx.GetSurface(), &caps),
                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        Uint32 fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx.GetPhysicalDevice(), m_ctx.GetSurface(), &fmtCount, nullptr);
        Vector<VkSurfaceFormatKHR> formats(fmtCount);
        if (fmtCount > 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx.GetPhysicalDevice(), m_ctx.GetSurface(), &fmtCount,
                                                 formats.data());
        }

        Uint32 modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx.GetPhysicalDevice(), m_ctx.GetSurface(), &modeCount, nullptr);
        Vector<VkPresentModeKHR> modes(modeCount);
        if (modeCount > 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx.GetPhysicalDevice(), m_ctx.GetSurface(), &modeCount,
                                                      modes.data());
        }

        VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
        VkPresentModeKHR presentMode = ChoosePresentMode(modes);
        VkExtent2D extent;
        if (m_viewportSize.x() == 0 || m_viewportSize.y() == 0) {
            extent = ChooseExtent(caps, m_ctx.GetWindow());
        } else {
            extent = {m_viewportSize.x(), m_viewportSize.y()};
        }

        Uint32 imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = m_ctx.GetSurface();
        sci.minImageCount = imageCount;
        sci.imageFormat = surfaceFormat.format;
        sci.imageColorSpace = surfaceFormat.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = presentMode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = oldSwapchain;

        VK_VERIFY(vkCreateSwapchainKHR(m_ctx.GetDevice(), &sci, nullptr, &m_swapchain), "vkCreateSwapchainKHR");

        Uint32 actualCount = 0;
        vkGetSwapchainImagesKHR(m_ctx.GetDevice(), m_swapchain, &actualCount, nullptr);
        m_images.resize(actualCount);
        vkGetSwapchainImagesKHR(m_ctx.GetDevice(), m_swapchain, &actualCount, m_images.data());

        m_imageViews.clear();
        m_imageViews.reserve(actualCount);
        for (auto image : m_images) {
            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image = image;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = surfaceFormat.format;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.baseMipLevel = 0;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.baseArrayLayer = 0;
            ivci.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateImageView(m_ctx.GetDevice(), &ivci, nullptr, &view), "vkCreateImageView swapchain");
            m_imageViews.push_back(view);
        }

        m_imagesInFlight.assign(actualCount, VK_NULL_HANDLE);
        m_format = surfaceFormat.format;
        m_extent = extent;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan::VkManager
