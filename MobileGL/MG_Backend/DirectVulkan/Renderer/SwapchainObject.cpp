// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/SwapchainObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SwapchainObject.h"

#include "MG_Impl/GLImpl/Framebuffer/GL_Framebuffer.h"
#include "MG_State/GLState/TextureState/TextureObject2D.h"

#if defined(__has_include)
#if __has_include(<vulkan/vk_enum_string_helper.h>)
#include <vulkan/vk_enum_string_helper.h>
#define MOBILEGL_HAS_VK_ENUM_STRING_HELPER 1
#else
#define MOBILEGL_HAS_VK_ENUM_STRING_HELPER 0
#endif
#else
#define MOBILEGL_HAS_VK_ENUM_STRING_HELPER 0
#endif

#if !MOBILEGL_HAS_VK_ENUM_STRING_HELPER
static const char* string_VkFormat(VkFormat) {
    return "VkFormat(unknown)";
}

static const char* string_VkColorSpaceKHR(VkColorSpaceKHR) {
    return "VkColorSpaceKHR(unknown)";
}

static const char* string_VkPresentModeKHR(VkPresentModeKHR) {
    return "VkPresentModeKHR(unknown)";
}

static const char* string_VkSurfaceTransformFlagBitsKHR(VkSurfaceTransformFlagBitsKHR) {
    return "VkSurfaceTransformFlagBitsKHR(unknown)";
}
#endif

namespace MobileGL::MG_Backend::DirectVulkan {
    namespace {
        Bool HasStencilComponent(VkFormat format) {
            return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        }

        VkFormat FindSupportedDepthStencilFormat(VkPhysicalDevice physicalDevice) {
            const VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                           VK_FORMAT_D32_SFLOAT};
            for (VkFormat format : candidates) {
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
                if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                    return format;
                }
            }
            return VK_FORMAT_UNDEFINED;
        }

        Uint32 FindMemoryType(VkPhysicalDevice physicalDevice, Uint32 typeFilter, VkMemoryPropertyFlags properties) {
            VkPhysicalDeviceMemoryProperties memProperties{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

            for (Uint32 i = 0; i < memProperties.memoryTypeCount; i++) {
                if ((typeFilter & (1 << i)) &&
                    (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                    return i;
                }
            }

            MOBILEGL_ASSERT(false, "Failed to find suitable memory type.");
            return 0;
        }
    } // namespace

    SwapchainObject::SwapchainCapabilities SwapchainObject::GetSwapchainCapabilities(VkPhysicalDevice physicalDevice,
                                                                                      VkSurfaceKHR surface) {
        SwapchainCapabilities swapchainCapabilities{};

        VK_VERIFY(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &swapchainCapabilities.capabilities));

        Uint32 formatCount = 0;
        VK_VERIFY(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr));
        if (formatCount != 0) {
            swapchainCapabilities.surfaceFormats.resize(formatCount);
            VK_VERIFY(vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice, surface, &formatCount, swapchainCapabilities.surfaceFormats.data()));
        }

        Uint32 presentModeCount = 0;
        VK_VERIFY(
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr));
        if (presentModeCount != 0) {
            swapchainCapabilities.presentModes.resize(presentModeCount);
            VK_VERIFY(vkGetPhysicalDeviceSurfacePresentModesKHR(
                physicalDevice, surface, &presentModeCount, swapchainCapabilities.presentModes.data()));
        }

        return swapchainCapabilities;
    }

    VkSurfaceFormatKHR SwapchainObject::ChooseSwapchainSurfaceFormat(
        const Vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if ((availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
                 availableFormat.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }
        for (const auto& availableFormat : availableFormats) {
            if ((availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB ||
                 availableFormat.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        // TODO: Properly rank other formats
        return availableFormats[0];
    }

    VkPresentModeKHR SwapchainObject::ChooseSwapchainPresentMode(
        const Vector<VkPresentModeKHR>& availablePresentModes) {
        for (auto desiredPresentMode : s_desiredPresentModes) {
            for (const auto& presentMode : availablePresentModes) {
                if (presentMode == desiredPresentMode) {
                    return presentMode;
                }
            }
        }

        // TODO: Properly rank other modes
        return availablePresentModes[0];
    }

    void SwapchainObject::Create(VkDevice device, VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                 Uint32 graphicsQueueFamily, Uint32 presentQueueFamily, Uint32 minImageCountHint,
                                 VkExtent2D desiredExtent) {
        const auto swapchainCapabilities = GetSwapchainCapabilities(physicalDevice, surface);
        MOBILEGL_ASSERT(swapchainCapabilities.IsComplete(),
                        "SwapchainObject::Create failed: incomplete swapchain capabilities");

        MGLOG_I("Got %d surface formats:", swapchainCapabilities.surfaceFormats.size());
        for (const auto& sf : swapchainCapabilities.surfaceFormats) {
            MGLOG_I("    [%s, %s]", string_VkFormat(sf.format), string_VkColorSpaceKHR(sf.colorSpace));
        }

        const auto pickedSurfaceFormat = ChooseSwapchainSurfaceFormat(swapchainCapabilities.surfaceFormats);
        MGLOG_I("Picked surface format: [%s, %s]", string_VkFormat(pickedSurfaceFormat.format),
                string_VkColorSpaceKHR(pickedSurfaceFormat.colorSpace));

        MGLOG_I("Got %d present modes:", swapchainCapabilities.presentModes.size());
        for (const auto& pm : swapchainCapabilities.presentModes) {
            MGLOG_I("    %s", string_VkPresentModeKHR(pm));
        }

        const auto presentMode = ChooseSwapchainPresentMode(swapchainCapabilities.presentModes);
        MGLOG_I("Picked present mode: %s", string_VkPresentModeKHR(presentMode));

        const auto& swapchainCaps = swapchainCapabilities.capabilities;
        Uint32 targetImageCount = std::max<Uint32>(minImageCountHint, swapchainCaps.minImageCount);
        if (swapchainCaps.maxImageCount != 0) {
            targetImageCount = std::min(targetImageCount, swapchainCaps.maxImageCount);
        }
        MGLOG_I("Set minImageCount = %u", targetImageCount);
        MGLOG_I("Swapchain currentTransform = %s",
                string_VkSurfaceTransformFlagBitsKHR(swapchainCaps.currentTransform));

        VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        createInfo.surface = surface;
        createInfo.minImageCount = targetImageCount;
        createInfo.imageFormat = pickedSurfaceFormat.format;
        createInfo.imageColorSpace = pickedSurfaceFormat.colorSpace;
        createInfo.imageExtent = swapchainCaps.currentExtent;
        if (createInfo.imageExtent.width == UINT32_MAX || createInfo.imageExtent.height == UINT32_MAX) {
            createInfo.imageExtent.width = std::clamp(desiredExtent.width,
                                                      swapchainCaps.minImageExtent.width,
                                                      swapchainCaps.maxImageExtent.width);
            createInfo.imageExtent.height = std::clamp(desiredExtent.height,
                                                       swapchainCaps.minImageExtent.height,
                                                       swapchainCaps.maxImageExtent.height);
        }
        if (swapchainCaps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
            swapchainCaps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
            std::swap(createInfo.imageExtent.width, createInfo.imageExtent.height);
        }

        createInfo.imageArrayLayers = 1;
        const VkImageUsageFlags requiredImageUsage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        MOBILEGL_ASSERT((swapchainCaps.supportedUsageFlags & requiredImageUsage) == requiredImageUsage,
                        "Swapchain does not support required usage flags (COLOR_ATTACHMENT | TRANSFER_DST). "
                        "supportedUsageFlags=0x%x",
                        static_cast<Uint32>(swapchainCaps.supportedUsageFlags));

        VkImageUsageFlags imageUsage = requiredImageUsage;
        if ((swapchainCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
            imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        createInfo.imageUsage = imageUsage;
        MGLOG_I("Swapchain imageUsage = 0x%x (supportedUsageFlags = 0x%x)", static_cast<Uint32>(createInfo.imageUsage),
                static_cast<Uint32>(swapchainCaps.supportedUsageFlags));
        Uint32 queueFamilyIndices[] = {graphicsQueueFamily, presentQueueFamily};
        if (graphicsQueueFamily != presentQueueFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }
        createInfo.preTransform = swapchainCaps.currentTransform;
        MGLOG_I("Set swapchain preTransform = %s", string_VkSurfaceTransformFlagBitsKHR(createInfo.preTransform));
        const VkCompositeAlphaFlagBitsKHR compositeAlphaCandidates[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto candidate : compositeAlphaCandidates) {
            if ((swapchainCaps.supportedCompositeAlpha & candidate) != 0) {
                createInfo.compositeAlpha = candidate;
                break;
            }
        }
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        m_surfaceFormat = {createInfo.imageFormat, createInfo.imageColorSpace};
        m_extent = createInfo.imageExtent;
        m_preTransform = createInfo.preTransform;

        VK_VERIFY(vkCreateSwapchainKHR(device, &createInfo, nullptr, &m_swapchain));

        Uint32 imageCount = 0;
        VK_VERIFY(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, nullptr));

        m_images.resize(imageCount, VK_NULL_HANDLE);
        VK_VERIFY(vkGetSwapchainImagesKHR(device, m_swapchain, &imageCount, m_images.data()));
        m_imageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

        CreateImageViews(device);
        CreateDepthStencilResources(device, physicalDevice);

        MGLOG_I("Swapchain created, extent = %dx%d, swapchain imageCount = %d", m_extent.width, m_extent.height,
                imageCount);

        // Properly initialize Default FBO here
        auto& defaultFBOInfo = MG_Impl::GLImpl::FramebufferImpl::pDefaultFramebufferInfo;
        const Int extentWidth = static_cast<Int>(createInfo.imageExtent.width);
        const Int extentHeight = static_cast<Int>(createInfo.imageExtent.height);
        const SizeT defaultAttachmentByteSize =
            static_cast<SizeT>(createInfo.imageExtent.width) * static_cast<SizeT>(createInfo.imageExtent.height) * 4;

        auto* colorTex = static_cast<MG_State::GLState::TextureObject2D*>(defaultFBOInfo->colorAttachment.get());
        colorTex->AllocateStorage(
            TextureUploadTarget::Texture2D, 0, {
                {extentWidth, extentHeight, 1},
                defaultAttachmentByteSize}); // TODO: 4 is format size
        TextureInternalFormat depthFormat = TextureInternalFormat::Depth24Stencil8;
        switch (m_depthStencilFormat) {
            case VK_FORMAT_D24_UNORM_S8_UINT:
                depthFormat = TextureInternalFormat::Depth24Stencil8;
                break;
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                depthFormat = TextureInternalFormat::Depth32FStencil8;
                break;
            case VK_FORMAT_D32_SFLOAT:
                depthFormat = TextureInternalFormat::DepthComponent32F;
                break;
            default:
                depthFormat = TextureInternalFormat::Depth24Stencil8;
                break;
        }
        auto* depthTex = static_cast<MG_State::GLState::TextureObject2D*>(defaultFBOInfo->depthAttachment.get());
        depthTex->SetInternalFormat(depthFormat);
        depthTex->AllocateStorage(TextureUploadTarget::Texture2D, 0, {
            {extentWidth, extentHeight, 1},
            defaultAttachmentByteSize}); // TODO: 4 is format size

    }

    void SwapchainObject::CreateDepthStencilResources(VkDevice device, VkPhysicalDevice physicalDevice) {
        DestroyDepthStencilResources(device);

        const auto imageCount = static_cast<Uint32>(m_images.size());
        if (imageCount == 0) {
            return;
        }

        m_depthStencilFormat = FindSupportedDepthStencilFormat(physicalDevice);
        MOBILEGL_ASSERT(m_depthStencilFormat != VK_FORMAT_UNDEFINED, "No supported depth/stencil format found.");

        m_depthStencilImages.assign(imageCount, VK_NULL_HANDLE);
        m_depthStencilImageMemories.assign(imageCount, VK_NULL_HANDLE);
        m_depthStencilImageViews.assign(imageCount, VK_NULL_HANDLE);
        m_depthStencilImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

        for (Uint32 i = 0; i < imageCount; ++i) {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.extent.width = m_extent.width;
            imageInfo.extent.height = m_extent.height;
            imageInfo.extent.depth = 1;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.format = m_depthStencilFormat;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VK_VERIFY(vkCreateImage(device, &imageInfo, nullptr, &m_depthStencilImages[i]), "vkCreateImage(depth)");

            VkMemoryRequirements memRequirements{};
            vkGetImageMemoryRequirements(device, m_depthStencilImages[i], &memRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex =
                FindMemoryType(physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_VERIFY(vkAllocateMemory(device, &allocInfo, nullptr, &m_depthStencilImageMemories[i]),
                      "vkAllocateMemory(depth)");
            VK_VERIFY(vkBindImageMemory(device, m_depthStencilImages[i], m_depthStencilImageMemories[i], 0),
                      "vkBindImageMemory(depth)");

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_depthStencilImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_depthStencilFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (HasStencilComponent(m_depthStencilFormat)) {
                viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VK_VERIFY(vkCreateImageView(device, &viewInfo, nullptr, &m_depthStencilImageViews[i]),
                      "vkCreateImageView(depth)");
        }
    }

    void SwapchainObject::DestroyDepthStencilResources(VkDevice device) {
        for (auto view : m_depthStencilImageViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        m_depthStencilImageViews.clear();

        for (auto image : m_depthStencilImages) {
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(device, image, nullptr);
            }
        }
        m_depthStencilImages.clear();

        for (auto memory : m_depthStencilImageMemories) {
            if (memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, memory, nullptr);
            }
        }
        m_depthStencilImageMemories.clear();
        m_depthStencilImageLayouts.clear();
        m_depthStencilFormat = VK_FORMAT_UNDEFINED;
    }

    void SwapchainObject::Shutdown(VkDevice device) {
        DestroyDepthStencilResources(device);

        for (auto imageView : m_imageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        m_imageViews.clear();

        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }

        m_images.clear();
        m_imageLayouts.clear();
        m_preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    VkImage SwapchainObject::GetImage(Uint32 index) const {
        MOBILEGL_ASSERT(index < m_images.size(), "Swapchain image index out of range");
        return m_images[index];
    }

    VkImageLayout SwapchainObject::GetImageLayout(Uint32 index) const {
        MOBILEGL_ASSERT(index < m_imageLayouts.size(), "Swapchain image layout index out of range");
        return m_imageLayouts[index];
    }

    void SwapchainObject::SetImageLayout(Uint32 index, VkImageLayout layout) {
        MOBILEGL_ASSERT(index < m_imageLayouts.size(), "Swapchain image layout index out of range");
        m_imageLayouts[index] = layout;
    }

    VkImage SwapchainObject::GetDepthStencilImage(Uint32 index) const {
        MOBILEGL_ASSERT(index < m_depthStencilImages.size(), "Swapchain depth/stencil image index out of range");
        return m_depthStencilImages[index];
    }

    VkImageView SwapchainObject::GetDepthStencilImageView(Uint32 index) const {
        MOBILEGL_ASSERT(index < m_depthStencilImageViews.size(),
                        "Swapchain depth/stencil image view index out of range");
        return m_depthStencilImageViews[index];
    }

    VkImageLayout SwapchainObject::GetDepthStencilImageLayout(Uint32 index) const {
        MOBILEGL_ASSERT(index < m_depthStencilImageLayouts.size(),
                        "Swapchain depth/stencil image layout index out of range");
        return m_depthStencilImageLayouts[index];
    }

    void SwapchainObject::SetDepthStencilImageLayout(Uint32 index, VkImageLayout layout) {
        MOBILEGL_ASSERT(index < m_depthStencilImageLayouts.size(),
                        "Swapchain depth/stencil image layout index out of range");
        m_depthStencilImageLayouts[index] = layout;
    }

    void SwapchainObject::CreateImageViews(VkDevice device) {
        m_imageViews.resize(m_images.size(), VK_NULL_HANDLE);
        for (SizeT i = 0; i < m_imageViews.size(); i++) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = m_images[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = m_surfaceFormat.format;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            VK_VERIFY(vkCreateImageView(device, &createInfo, nullptr, &m_imageViews[i]));
        }
        MGLOG_I("Swapchain image views created");
    }

#undef VK_VERIFY
} // namespace MobileGL::MG_Backend::DirectVulkan
