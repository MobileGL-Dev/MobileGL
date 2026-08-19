// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkTextureManager.h"

#include "ProgramFactory.h"

#include "MG_State/GLState/Core.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"

#include <Config.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vulkan/utility/vk_format_utils.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    // Compute shaders may legally sample framebuffer-attached textures (the GL feedback-loop rule
    // only covers rendering commands; e.g. Flywheel's Hi-Z depth pyramid downsample samples the
    // depth attachment of the bound draw framebuffer), so sampled-read barriers must cover the
    // compute stage in addition to the graphics stages. Set at Initialize from the renderer's
    // device-feature-derived mask: geometry/tessellation stage bits are invalid in a barrier when
    // their feature is off (VUID-vkCmdPipelineBarrier-srcStageMask-04090/-04091), and ALL_GRAPHICS
    // would also serialize against non-shader stages. The default only matters before a device
    // exists, when nothing records barriers.
    static VkPipelineStageFlags s_sampledReadStages =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    static Uint32 ComputeFullMipLevelCount(const IntVec3& baseTexelSize) {
        Int maxDimension = std::max<Int>(baseTexelSize.x(),
                                         std::max<Int>(baseTexelSize.y(), std::max<Int>(baseTexelSize.z(), 1)));
        Uint32 mipLevelCount = 1;
        while (maxDimension > 1) {
            maxDimension = std::max<Int>(maxDimension / 2, 1);
            ++mipLevelCount;
        }
        return mipLevelCount;
    }

    struct TextureFormatInfo {
        VkFormat format = VK_FORMAT_UNDEFINED;
        Bool expandRgbToRgba = false;
        Uint32 componentByteCount = 0;
        Array<Uint8, 4> alphaBytes = {0, 0, 0, 0};
    };

    struct TextureShapeInfo {
        VkImageType imageType = VK_IMAGE_TYPE_2D;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateFlags imageFlags = 0;
        Uint32 depth = 1;
        Uint32 arrayLayers = 1;
    };

    static Bool IsR11G11B10FFallbackEnabled() {
        return MG_Config::Features.MagmaR11G11B10FFallback;
    }

    static Bool IsMultisampleTextureUploadTarget(TextureUploadTarget target) {
        return target == TextureUploadTarget::Texture2DMultisample ||
               target == TextureUploadTarget::ProxyTexture2DMultisample ||
               target == TextureUploadTarget::Texture2DMultisampleArray ||
               target == TextureUploadTarget::ProxyTexture2DMultisampleArray;
    }

    static Bool IsMutableStorageImageFormat(VkFormat format) {
        if (!vkuFormatIsColor(format) || vkuFormatIsCompressed(format)) {
            return false;
        }

        // These are the uncompressed color compatibility classes covered by the core GLSL/SPIR-V
        // storage-image formats. OpenGL mutable texture storage uses image-format compatibility by
        // size, so a shader may legally reinterpret (for example) RGBA16_UNORM storage as rgba16f. Vulkan
        // requires the image to be mutable and the view formats to share this exact compatibility
        // class for the equivalent operation.
        switch (vkuFormatCompatibilityClass(format)) {
        case VKU_FORMAT_COMPATIBILITY_CLASS_8BIT:
        case VKU_FORMAT_COMPATIBILITY_CLASS_16BIT:
        case VKU_FORMAT_COMPATIBILITY_CLASS_32BIT:
        case VKU_FORMAT_COMPATIBILITY_CLASS_64BIT:
        case VKU_FORMAT_COMPATIBILITY_CLASS_128BIT:
            return true;
        default:
            return false;
        }
    }

    static Bool HasMatchingColorComponentLayout(VkFormat lhs, VkFormat rhs) {
        const VKU_FORMAT_INFO lhsInfo = vkuGetFormatInfo(lhs);
        const VKU_FORMAT_INFO rhsInfo = vkuGetFormatInfo(rhs);
        if (lhsInfo.component_count == 0 || lhsInfo.component_count != rhsInfo.component_count ||
            lhsInfo.texel_block_size != rhsInfo.texel_block_size ||
            lhsInfo.texels_per_block != 1 || rhsInfo.texels_per_block != 1) {
            return false;
        }
        for (Uint32 component = 0; component < lhsInfo.component_count; ++component) {
            if (lhsInfo.components[component].type != rhsInfo.components[component].type ||
                lhsInfo.components[component].size != rhsInfo.components[component].size) {
                return false;
            }
        }
        return true;
    }

    static Bool FormatMatchesSamplerNumericDomain(VkFormat format, SamplerNumericDomain numericDomain) {
        switch (numericDomain) {
        case SamplerNumericDomain::Float:
            return vkuFormatIsSampledFloat(format);
        case SamplerNumericDomain::SignedInteger:
            return vkuFormatIsSINT(format);
        case SamplerNumericDomain::UnsignedInteger:
            return vkuFormatIsUINT(format);
        case SamplerNumericDomain::Unknown:
            return true;
        }
        return false;
    }

    static Bool TryResolveSampleCountFlagBits(Int requestedSamples, VkSampleCountFlagBits& outSampleCount) {
        // GL promises "at least the requested samples", so a non-power-of-two
        // request (legal in GL, e.g. 3) rounds up to the next Vulkan bit.
        if (requestedSamples <= 1) {
            outSampleCount = VK_SAMPLE_COUNT_1_BIT;
            return true;
        }
        if (requestedSamples > 64) {
            return false;
        }
        Uint32 bit = 1;
        while (bit < static_cast<Uint32>(requestedSamples)) {
            bit <<= 1;
        }
        outSampleCount = static_cast<VkSampleCountFlagBits>(bit);
        return true;
    }

    static Bool IsCubeMapFaceUploadTarget(TextureUploadTarget target) {
        return target >= TextureUploadTarget::CubeMapPositiveX &&
               target <= TextureUploadTarget::CubeMapNegativeZ;
    }

    static Uint32 ResolveUploadArrayLayer(TextureUploadTarget target) {
        if (!IsCubeMapFaceUploadTarget(target)) {
            return 0;
        }
        return static_cast<Uint32>(target) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
    }

    static Bool IsValidSampledImageLayout(VkImageLayout layout) {
        switch (layout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_GENERAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            return true;
        default:
            return false;
        }
    }

    static VkImageLayout ResolveSampledReadOnlyLayout(VkImageAspectFlags aspectMask) {
        return (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    static void GetImageTransitionSourceState(VkImageLayout oldLayout,
                                              VkPipelineStageFlags& outSrcStageMask,
                                              VkAccessFlags& outSrcAccessMask) {
        switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            outSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            outSrcAccessMask = 0;
            return;
        case VK_IMAGE_LAYOUT_GENERAL:
            outSrcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            outSrcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            outSrcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            outSrcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            outSrcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            outSrcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            outSrcStageMask = s_sampledReadStages;
            outSrcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            outSrcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            outSrcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            outSrcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            outSrcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            return;
        default:
            MOBILEGL_ASSERT(false, "GetImageTransitionSourceState: unsupported layout=%d", static_cast<Int>(oldLayout));
            outSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            outSrcAccessMask = 0;
            return;
        }
    }

    VkTextureManager::TextureIdentity VkTextureManager::MakeTextureIdentity(
        MG_State::GLState::ITextureObject* texture) {
        return TextureIdentity{
            .texture = texture,
            .lifetimeId = texture ? texture->GetLifetimeId() : 0,
        };
    }

    static void GetImageTransitionDestinationState(VkImageLayout newLayout,
                                                   VkPipelineStageFlags& outDstStageMask,
                                                   VkAccessFlags& outDstAccessMask) {
        switch (newLayout) {
        case VK_IMAGE_LAYOUT_GENERAL:
            outDstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            outDstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            outDstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            outDstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            outDstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            outDstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            outDstStageMask = s_sampledReadStages;
            outDstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            outDstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            outDstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            outDstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            outDstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            return;
        default:
            MOBILEGL_ASSERT(false, "GetImageTransitionDestinationState: unsupported layout=%d", static_cast<Int>(newLayout));
            outDstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            outDstAccessMask = 0;
            return;
        }
    }

    static Bool PreserveTextureContentsOnRecreate(VkDevice device,
                                                  VkCommandPool commandPool,
                                                  VkQueue graphicsQueue,
                                                  const VkTextureManager::TextureResource& oldResource,
                                                  VkTextureManager::TextureResource& newResource) {
        MOBILEGL_ASSERT(device != VK_NULL_HANDLE, "PreserveTextureContentsOnRecreate: device is null");
        MOBILEGL_ASSERT(commandPool != VK_NULL_HANDLE, "PreserveTextureContentsOnRecreate: commandPool is null");
        MOBILEGL_ASSERT(graphicsQueue != VK_NULL_HANDLE, "PreserveTextureContentsOnRecreate: graphicsQueue is null");
        MOBILEGL_ASSERT(oldResource.image != VK_NULL_HANDLE, "PreserveTextureContentsOnRecreate: old image is null");
        MOBILEGL_ASSERT(newResource.image != VK_NULL_HANDLE, "PreserveTextureContentsOnRecreate: new image is null");

        const Uint32 preservedMipLevels = std::min(oldResource.mipLevels, newResource.mipLevels);
        if (preservedMipLevels == 0 || oldResource.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            return true;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer),
                  "vkAllocateCommandBuffers(texture preserve)");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_VERIFY(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(texture preserve)");

        Bool ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, newResource.image, newResource.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, newResource.aspect, 0, newResource.mipLevels);
        MOBILEGL_ASSERT(ok, "PreserveTextureContentsOnRecreate: failed to prepare destination image");

        VkImageLayout srcTrackedLayout = oldResource.layout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcTrackedLayout, srcStageMask, srcAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, oldResource.image, srcTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, oldResource.aspect, 0, preservedMipLevels);
        MOBILEGL_ASSERT(ok, "PreserveTextureContentsOnRecreate: failed to prepare source image");

        Vector<VkImageCopy> copyRegions;
        copyRegions.reserve(preservedMipLevels);
        for (Uint32 level = 0; level < preservedMipLevels; ++level) {
            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = oldResource.aspect;
            copy.srcSubresource.mipLevel = level;
            copy.srcSubresource.baseArrayLayer = 0;
            copy.srcSubresource.layerCount = oldResource.arrayLayers;
            copy.dstSubresource.aspectMask = newResource.aspect;
            copy.dstSubresource.mipLevel = level;
            copy.dstSubresource.baseArrayLayer = 0;
            copy.dstSubresource.layerCount = newResource.arrayLayers;
            copy.extent.width = std::max(oldResource.extent.width >> level, 1u);
            copy.extent.height = std::max(oldResource.extent.height >> level, 1u);
            copy.extent.depth = std::max(oldResource.depth >> level, 1u);
            copyRegions.push_back(copy);
        }

        vkCmdCopyImage(commandBuffer,
                       oldResource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       newResource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       static_cast<Uint32>(copyRegions.size()), copyRegions.data());

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionDestinationState(oldResource.layout, dstStageMask, dstAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, newResource.image, newResource.layout, oldResource.layout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, dstStageMask,
            VK_ACCESS_TRANSFER_WRITE_BIT, dstAccessMask, newResource.aspect, 0, newResource.mipLevels);
        MOBILEGL_ASSERT(ok, "PreserveTextureContentsOnRecreate: failed to restore destination layout");

        VK_VERIFY(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(texture preserve)");

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence(texture preserve)");
        VK_VERIFY(vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence), "vkQueueSubmit(texture preserve)");
        VK_VERIFY(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(texture preserve)");

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        return true;
    }

    static TextureFormatInfo ResolveTextureFormatInfo(TextureInternalFormat format) {
        switch (format) {
        case TextureInternalFormat::RGB:
        case TextureInternalFormat::RGB8:
        // Legacy low-bit RGB formats share the UNorm8 canonical shadow layout (see
        // TextureFormatProcessor), so they upload exactly like RGB8 with an alpha expand.
        case TextureInternalFormat::R3G3B2:
        case TextureInternalFormat::RGB4:
        case TextureInternalFormat::RGB5:
            return {VK_FORMAT_R8G8B8A8_UNORM, true, 1, {0xFF, 0x00, 0x00, 0x00}};
        // Low-bit RGBA formats: UNorm8x4 canonical shadow, no expansion needed.
        case TextureInternalFormat::RGBA2:
        case TextureInternalFormat::RGBA4:
        case TextureInternalFormat::RGB5A1:
            return {VK_FORMAT_R8G8B8A8_UNORM, false, 0, {0, 0, 0, 0}};
        // 10/12-bit RGB(A): UNorm16 canonical shadow.
        case TextureInternalFormat::RGB10:
        case TextureInternalFormat::RGB12:
            return {VK_FORMAT_R16G16B16A16_UNORM, true, 2, {0xFF, 0xFF, 0x00, 0x00}};
        case TextureInternalFormat::RGBA12:
            return {VK_FORMAT_R16G16B16A16_UNORM, false, 0, {0, 0, 0, 0}};
        case TextureInternalFormat::SRGB8:
            return {VK_FORMAT_R8G8B8A8_SRGB, true, 1, {0xFF, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB8Snorm:
            return {VK_FORMAT_R8G8B8A8_SNORM, true, 1, {0x7F, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB16:
            return {VK_FORMAT_R16G16B16A16_UNORM, true, 2, {0xFF, 0xFF, 0x00, 0x00}};
        case TextureInternalFormat::RGB16Snorm:
            return {VK_FORMAT_R16G16B16A16_SNORM, true, 2, {0xFF, 0x7F, 0x00, 0x00}};
        case TextureInternalFormat::RGB16F:
            return {VK_FORMAT_R16G16B16A16_SFLOAT, true, 2, {0x00, 0x3C, 0x00, 0x00}};
        case TextureInternalFormat::R11FG11FB10F:
            if (IsR11G11B10FFallbackEnabled()) {
                return {VK_FORMAT_R16G16B16A16_SFLOAT, true, 2, {0x00, 0x3C, 0x00, 0x00}};
            }
            return {MG_Util::ConvertTextureInternalFormatToVkEnum(format), false, 0, {0, 0, 0, 0}};
        case TextureInternalFormat::RGB32F:
            return {VK_FORMAT_R32G32B32A32_SFLOAT, true, 4, {0x00, 0x00, 0x80, 0x3F}};
        case TextureInternalFormat::RGB8I:
            return {VK_FORMAT_R8G8B8A8_SINT, true, 1, {0x01, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB8UI:
            return {VK_FORMAT_R8G8B8A8_UINT, true, 1, {0x01, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB16I:
            return {VK_FORMAT_R16G16B16A16_SINT, true, 2, {0x01, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB16UI:
            return {VK_FORMAT_R16G16B16A16_UINT, true, 2, {0x01, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB32I:
            return {VK_FORMAT_R32G32B32A32_SINT, true, 4, {0x01, 0x00, 0x00, 0x00}};
        case TextureInternalFormat::RGB32UI:
            return {VK_FORMAT_R32G32B32A32_UINT, true, 4, {0x01, 0x00, 0x00, 0x00}};
        default:
            return {MG_Util::ConvertTextureInternalFormatToVkEnum(format), false, 0, {0, 0, 0, 0}};
        }
    }

    static Bool ExpandRgbSourceToRgba(const void* source, SizeT sourceByteSize, const IntVec3& texelSize,
                                      const TextureFormatInfo& formatInfo, Vector<Uint8>& outExpandedData) {
        MOBILEGL_ASSERT(source != nullptr, "ExpandRgbSourceToRgba: source is null");
        MOBILEGL_ASSERT(formatInfo.expandRgbToRgba, "ExpandRgbSourceToRgba: format does not require RGB expansion");
        MOBILEGL_ASSERT(formatInfo.componentByteCount > 0,
                        "ExpandRgbSourceToRgba: invalid component size for expanded RGB format");

        const SizeT depth = static_cast<SizeT>(std::max(texelSize.z(), 1));
        const SizeT pixelCount = static_cast<SizeT>(texelSize.x()) * static_cast<SizeT>(texelSize.y()) * depth;
        MOBILEGL_ASSERT(pixelCount > 0, "ExpandRgbSourceToRgba: invalid texel size (%d, %d, %d)",
                        texelSize.x(), texelSize.y(), texelSize.z());
        MOBILEGL_ASSERT(sourceByteSize == pixelCount * formatInfo.componentByteCount * 3,
                        "ExpandRgbSourceToRgba: unexpected source byte size=%zu for pixelCount=%zu componentBytes=%u",
                        sourceByteSize, pixelCount, formatInfo.componentByteCount);

        outExpandedData.resize(pixelCount * formatInfo.componentByteCount * 4);
        const auto* src = static_cast<const Uint8*>(source);
        auto* dst = outExpandedData.data();
        const SizeT srcPixelSize = static_cast<SizeT>(formatInfo.componentByteCount) * 3;
        const SizeT dstPixelSize = static_cast<SizeT>(formatInfo.componentByteCount) * 4;
        for (SizeT pixel = 0; pixel < pixelCount; ++pixel) {
            const SizeT srcOffset = pixel * srcPixelSize;
            const SizeT dstOffset = pixel * dstPixelSize;
            std::memcpy(dst + dstOffset, src + srcOffset, srcPixelSize);
            std::memcpy(dst + dstOffset + srcPixelSize, formatInfo.alphaBytes.data(), formatInfo.componentByteCount);
        }
        return true;
    }

    static VkComponentSwizzle ToVkComponentSwizzle(TextureSwizzleParam swizzle) {
        switch (swizzle) {
        case TextureSwizzleParam::Red:
            return VK_COMPONENT_SWIZZLE_R;
        case TextureSwizzleParam::Green:
            return VK_COMPONENT_SWIZZLE_G;
        case TextureSwizzleParam::Blue:
            return VK_COMPONENT_SWIZZLE_B;
        case TextureSwizzleParam::Alpha:
            return VK_COMPONENT_SWIZZLE_A;
        case TextureSwizzleParam::Zero:
            return VK_COMPONENT_SWIZZLE_ZERO;
        case TextureSwizzleParam::One:
            return VK_COMPONENT_SWIZZLE_ONE;
        default:
            MOBILEGL_ASSERT(false, "ToVkComponentSwizzle: unsupported swizzle=%d", static_cast<Int>(swizzle));
            return VK_COMPONENT_SWIZZLE_IDENTITY;
        }
    }

    static VkComponentSwizzle ToVkSampledComponentSwizzle(TextureSwizzleParam swizzle, Bool alphaIsImplicitOne) {
        if (alphaIsImplicitOne && swizzle == TextureSwizzleParam::Alpha) {
            return VK_COMPONENT_SWIZZLE_ONE;
        }
        return ToVkComponentSwizzle(swizzle);
    }

    static VkComponentMapping ResolveSampledViewComponents(const MG_State::GLState::ITextureObject& texture,
                                                           const TextureFormatInfo& formatInfo) {
        const auto& swizzles = texture.GetAllSwizzleParams();
        const Bool alphaIsImplicitOne = formatInfo.expandRgbToRgba;
        VkComponentMapping components{
            ToVkSampledComponentSwizzle(swizzles.x(), alphaIsImplicitOne),
            ToVkSampledComponentSwizzle(swizzles.y(), alphaIsImplicitOne),
            ToVkSampledComponentSwizzle(swizzles.z(), alphaIsImplicitOne),
            ToVkSampledComponentSwizzle(swizzles.w(), alphaIsImplicitOne),
        };
        return components;
    }

    static Bool TryResolveTextureShapeInfo(const MG_State::GLState::ITextureObject& texture,
                                           TextureUploadTarget uploadTarget, const IntVec3& texelSize,
                                           TextureShapeInfo& outShape) {
        switch (uploadTarget) {
        case TextureUploadTarget::Texture1D:
        case TextureUploadTarget::ProxyTexture1D:
            outShape = {};
            outShape.imageType = VK_IMAGE_TYPE_1D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_1D;
            return true;
        case TextureUploadTarget::Texture1DArray:
        case TextureUploadTarget::ProxyTexture1DArray:
            MOBILEGL_ASSERT(texelSize.z() > 0,
                            "TryResolveTextureShapeInfo: invalid 1D array depth=%d for textureId=%d",
                            texelSize.z(), texture.GetExternalIndex());
            outShape.imageType = VK_IMAGE_TYPE_1D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            outShape.depth = 1;
            outShape.arrayLayers = static_cast<Uint32>(texelSize.z());
            return true;
        case TextureUploadTarget::Texture2D:
        case TextureUploadTarget::ProxyTexture2D:
        case TextureUploadTarget::TextureRectangle:
        case TextureUploadTarget::ProxyTextureRectangle:
            outShape = {};
            return true;
        case TextureUploadTarget::Texture2DMultisample:
        case TextureUploadTarget::ProxyTexture2DMultisample:
            outShape = {};
            return true;
        case TextureUploadTarget::Texture2DArray:
        case TextureUploadTarget::ProxyTexture2DArray:
            MOBILEGL_ASSERT(texelSize.z() > 0,
                            "TryResolveTextureShapeInfo: invalid 2D array depth=%d for textureId=%d",
                            texelSize.z(), texture.GetExternalIndex());
            outShape.imageType = VK_IMAGE_TYPE_2D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            outShape.depth = 1;
            outShape.arrayLayers = static_cast<Uint32>(texelSize.z());
            return true;
        case TextureUploadTarget::Texture2DMultisampleArray:
        case TextureUploadTarget::ProxyTexture2DMultisampleArray:
            MOBILEGL_ASSERT(texelSize.z() > 0,
                            "TryResolveTextureShapeInfo: invalid 2D multisample array depth=%d for textureId=%d",
                            texelSize.z(), texture.GetExternalIndex());
            outShape.imageType = VK_IMAGE_TYPE_2D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            outShape.depth = 1;
            outShape.arrayLayers = static_cast<Uint32>(texelSize.z());
            return true;
        case TextureUploadTarget::Texture3D:
        case TextureUploadTarget::ProxyTexture3D:
            MOBILEGL_ASSERT(texelSize.z() > 0,
                            "TryResolveTextureShapeInfo: invalid 3D texture depth=%d for textureId=%d",
                            texelSize.z(), texture.GetExternalIndex());
            outShape.imageType = VK_IMAGE_TYPE_3D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_3D;
            outShape.depth = static_cast<Uint32>(texelSize.z());
            return true;
        case TextureUploadTarget::CubeMapPositiveX:
        case TextureUploadTarget::CubeMapNegativeX:
        case TextureUploadTarget::CubeMapPositiveY:
        case TextureUploadTarget::CubeMapNegativeY:
        case TextureUploadTarget::CubeMapPositiveZ:
        case TextureUploadTarget::CubeMapNegativeZ:
        case TextureUploadTarget::ProxyCubeMap:
            MOBILEGL_ASSERT(texture.GetTarget() == TextureTarget::TextureCubeMap,
                            "TryResolveTextureShapeInfo: cube upload target on non-cube textureId=%d target=%s",
                            texture.GetExternalIndex(),
                            MG_Util::ConvertTextureTargetToString(texture.GetTarget()).c_str());
            MOBILEGL_ASSERT(texelSize.x() == texelSize.y(),
                            "TryResolveTextureShapeInfo: cube map textureId=%d is not square (%d x %d)",
                            texture.GetExternalIndex(), texelSize.x(), texelSize.y());
            outShape.imageType = VK_IMAGE_TYPE_2D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            outShape.imageFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            outShape.depth = 1;
            outShape.arrayLayers = 6;
            return true;
        case TextureUploadTarget::CubeMapArray:
        case TextureUploadTarget::ProxyCubeMapArray:
            // GL_TEXTURE_CUBE_MAP_ARRAY is an array texture whose layers happen to be cube faces:
            // one 2D image with arrayLayers = 6 * cubeCount, CUBE_COMPATIBLE so the whole thing can
            // be sampled as a samplerCubeArray. glTexStorage3D hands the 6*n through as the GL depth
            // and the upload path's depthSelectsArrayLayer already lists VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
            // so the copies address layers correctly.
            //
            // A depth that is not a whole number of cubes, or a non-square level, has no Vulkan shape
            // - declined the way every other unrepresentable target is. This function's Bool return
            // exists for exactly that; asserting here would abort the process on ordinary application
            // input, GL_PROXY_TEXTURE_CUBE_MAP_ARRAY above all.
            if (texelSize.z() <= 0 || (texelSize.z() % 6) != 0 || texelSize.x() != texelSize.y()) {
                return false;
            }
            outShape.imageType = VK_IMAGE_TYPE_2D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            outShape.imageFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            outShape.depth = 1;
            outShape.arrayLayers = static_cast<Uint32>(texelSize.z());
            return true;
        default:
            return false;
        }
    }

    Bool VkTextureManager::Initialize(const InitInfo& initInfo) {
        Shutdown();

        m_device = initInfo.device;
        m_physicalDevice = initInfo.physicalDevice;
        m_allocator = initInfo.allocator;
        m_commandPool = initInfo.commandPool;
        m_graphicsQueue = initInfo.graphicsQueue;
        m_imageFormatListSupported = initInfo.imageFormatListSupported;
        s_sampledReadStages = initInfo.sampledReadStageMask;
        m_currentFrameIndex = 0;
        m_deferredReleases.clear();
        m_deferredReleases.resize(initInfo.frameCount);
        m_deferredViewReleases.clear();
        m_deferredViewReleases.resize(initInfo.frameCount);

        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE && m_physicalDevice != VK_NULL_HANDLE && m_allocator != nullptr &&
                            m_commandPool != VK_NULL_HANDLE && m_graphicsQueue != VK_NULL_HANDLE,
                        "VkTextureManager::Initialize failed: invalid initialization info");
        MOBILEGL_ASSERT(initInfo.frameCount > 0,
                        "VkTextureManager::Initialize failed: frameCount must be > 0");

        TextureResource::s_device = m_device;
        TextureResource::s_allocator = m_allocator;

        // Own pool for the recycled upload-batch command buffers. Parking a
        // dozen reset-but-alive command buffers in the renderer's shared pool
        // interleaves their retained chunks with the frame command buffers
        // allocated/freed there every frame; isolating them keeps both pools'
        // internal allocators dense.
        VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                               VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        uploadPoolInfo.queueFamilyIndex = initInfo.graphicsQueueFamilyIndex;
        VK_VERIFY(vkCreateCommandPool(m_device, &uploadPoolInfo, nullptr, &m_uploadCommandPool),
                  "vkCreateCommandPool(texture upload batch)");

        return true;
    }

    void VkTextureManager::Shutdown() {
        if (m_device != VK_NULL_HANDLE) {
            // A still-open (never-submitted) batch is discarded, not submitted:
            // the renderer has already drained the device and the data has no
            // observer. Submitted batches are waited and recycled, then the
            // pools they recycled into are destroyed.
            DiscardPendingUploadBatch();
            ReclaimCompletedUploads(/*waitAll=*/true);
            DestroyUploadPools();
            if (m_uploadCommandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(m_device, m_uploadCommandPool, nullptr);
                m_uploadCommandPool = VK_NULL_HANDLE;
            }
        }
        DestroyDeferredReleases();
        ++m_resourceEraseEpoch;  // every memoized resource pointer dies with the map
        m_textureResources.clear();
        m_aliveObjects.clear();
        m_storageImageTextures.clear();

        m_device = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_allocator = nullptr;
        m_commandPool = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_currentFrameIndex = 0;
    }

    void VkTextureManager::BeginFrame(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredReleases.size(),
                        "VkTextureManager::BeginFrame invalid frame index %u (size=%zu)",
                        frameIndex, m_deferredReleases.size());
        MOBILEGL_ASSERT(frameIndex < m_deferredViewReleases.size(),
                        "VkTextureManager::BeginFrame invalid deferred-view frame index %u (size=%zu)",
                        frameIndex, m_deferredViewReleases.size());
        m_currentFrameIndex = frameIndex;
        CollectDeferredReleases(frameIndex);
        ReclaimCompletedUploads();

        // Frame-boundary GC: every 64 frame boundaries (~1 s at 60 fps) bounds the reclaim
        // latency for dead textures regardless of draw traffic — workloads that churn
        // textures through clears/readbacks alone never reach the draw-gated
        // CollectGarbage. Must run after CollectDeferredReleases above: the prune defers
        // its releases into this frame's slot, which was just drained, so they are
        // destroyed only after the slot's fence has been waited again one full frame-ring
        // cycle from now (never while an in-flight frame may still reference them).
        constexpr Uint32 kGcFrameInterval = 64;
        ++m_gcFrameCounter;
        if (m_gcFrameCounter % kGcFrameInterval == 0) {
            PruneDeadTextures();
        }
    }

    void VkTextureManager::CollectAllDeferredReleases() {
        const SizeT frameCount = std::min(m_deferredReleases.size(), m_deferredViewReleases.size());
        for (SizeT frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            CollectDeferredReleases(static_cast<Uint32>(frameIndex));
        }
    }

    void VkTextureManager::EraseTrackedTexture(const TextureIdentity& identity) {
        auto resourceIt = m_textureResources.find(identity);
        if (resourceIt != m_textureResources.end()) {
            DeferResourceRelease(Move(resourceIt->second));
            m_textureResources.erase(resourceIt);
        }
        m_aliveObjects.erase(identity);
        m_storageImageTextures.erase(identity);
        // Invalidate every cross-draw sampled-texture memo: the erased
        // resource's address may be reused by a future emplace.
        ++m_resourceEraseEpoch;
    }

    void VkTextureManager::PruneStaleTextureAliases(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }

        Vector<TextureIdentity> staleAliases;
        for (auto it = m_aliveObjects.begin(); it != m_aliveObjects.end(); ++it) {
            if (it->first.texture != texture) {
                continue;
            }
            const auto liveTexture = it->second.lock();
            if (!liveTexture || liveTexture.get() != texture ||
                liveTexture->GetLifetimeId() != it->first.lifetimeId) {
                staleAliases.emplace_back(it->first);
            }
        }
        for (const auto& identity : staleAliases) {
            EraseTrackedTexture(identity);
        }
    }

    void VkTextureManager::BeginDrawSyncScope() {
        m_drawSyncedThisDraw.clear();
        m_drawSyncScopeActive = true;
    }

    void VkTextureManager::EndDrawSyncScope() {
        m_drawSyncScopeActive = false;
        m_drawSyncedThisDraw.clear();
    }

    VkTextureManager::TextureResource* VkTextureManager::SyncTextureAndGetDescriptor(MG_State::GLState::ITextureObject& texture) {
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "SyncTextureAndGetDescriptor: m_device == VK_NULL_HANDLE");

        const TextureIdentity identity = MakeTextureIdentity(&texture);

        // Per-draw memo fast path (see BeginDrawSyncScope): a texture already fully
        // synced earlier in this draw cannot have changed since (no GL mutation runs
        // mid-SetupDraw), so skip the heavy SyncTexture work and hand back the
        // already-synced resource. Layout lives on the resource and is updated by the
        // transition path, so the short-circuited resource still reflects the truth.
        const Bool memoActive = m_drawSyncScopeActive;
        if (memoActive) {
            for (const DrawSyncedTexture& synced : m_drawSyncedThisDraw) {
                if (synced.identity == identity) {
                    if (synced.resource != nullptr && synced.resource->image != VK_NULL_HANDLE) {
                        return synced.resource;
                    }
                    break;  // resource unexpectedly gone -> fall through to a full sync
                }
            }
        }

        // Cross-draw memo probe (see SyncedTextureMemoEntry): skips both map
        // lookups and the (re)registration path for repeat-bound textures.
        TextureResource* resourcePtr = nullptr;
        for (Uint32 i = 0; i < kSyncedTextureMemoSize; ++i) {
            const SyncedTextureMemoEntry& memo = m_syncedTextureMemo[i];
            if (memo.texture == &texture && memo.lifetimeId == identity.lifetimeId &&
                memo.eraseEpoch == m_resourceEraseEpoch) {
                resourcePtr = memo.resource;
                break;
            }
        }

        if (resourcePtr == nullptr) {
            auto aliveIt = m_aliveObjects.find(identity);
            if (aliveIt != m_aliveObjects.end() && aliveIt->second.expired()) {
                EraseTrackedTexture(aliveIt->first);
                aliveIt = m_aliveObjects.end();
            }

            // Only (re)register and prune when this (texture, lifetime) pair is new: stale
            // aliases can only come into existence through an address reuse, which by
            // construction introduces a new identity. Doing this unconditionally made every
            // sampled-texture sync scan the entire alive-texture map per draw.
            if (aliveIt == m_aliveObjects.end()) {
                WeakPtr<MG_State::GLState::ITextureObject> aliveTexture;
                const auto& liveTexture = MG_State::pGLContext->GetTextureObject(texture.GetExternalIndex());
                if (liveTexture && liveTexture.get() == &texture) {
                    aliveTexture = liveTexture;
                } else {
                    // The name lookup legally fails while the object is alive: the name was
                    // deleted with the texture still attached to an FBO (the attachment's
                    // SharedPtr keeps it alive), or the name was reused by a new texture, or
                    // this is a default texture object (name 0 lives outside the name map).
                    // Register through the object's own control block so the resource created
                    // below still participates in weak-expiry GC instead of becoming an
                    // orphan no reclamation path can reach until Shutdown.
                    aliveTexture = texture.weak_from_this();
                }
                if (!aliveTexture.expired()) {
                    m_aliveObjects[identity] = Move(aliveTexture);
                    PruneStaleTextureAliases(&texture);
                }
            }

            auto it = m_textureResources.find(identity);
            if (it == m_textureResources.end()) {
                TextureResource initial{};
                auto [insertIt, _] = m_textureResources.emplace(identity, Move(initial));
                it = insertIt;
            }
            resourcePtr = &(it->second);
            m_syncedTextureMemo[m_syncedTextureMemoNext] =
                SyncedTextureMemoEntry{&texture, identity.lifetimeId, m_resourceEraseEpoch, resourcePtr};
            m_syncedTextureMemoNext = (m_syncedTextureMemoNext + 1) % kSyncedTextureMemoSize;
        }

        if (!SyncTexture(texture, *resourcePtr)) {
            MGLOG_D("%s: Syncing texture %d failed", __func__, texture.GetExternalIndex());
            return nullptr;
        }

        if (memoActive) {
            Bool recorded = false;
            for (const DrawSyncedTexture& synced : m_drawSyncedThisDraw) {
                if (synced.identity == identity) {
                    recorded = true;
                    break;
                }
            }
            if (!recorded) {
                m_drawSyncedThisDraw.push_back({identity, resourcePtr});
            }
        }

        return resourcePtr;
    }

    VkImageView VkTextureManager::GetOrCreateViewAtMipLevel(MG_State::GLState::ITextureObject& texture, Uint32 mipLevel) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE || mipLevel >= resource->mipLevels) {
            return VK_NULL_HANDLE;
        }

        if (resource->perMipViews.size() != resource->mipLevels) {
            resource->perMipViews.resize(resource->mipLevels, VK_NULL_HANDLE);
        }

        VkImageView& perMipView = resource->perMipViews[mipLevel];
        if (perMipView != VK_NULL_HANDLE) {
            return perMipView;
        }

        perMipView = CreateImageView(resource->image, resource->format, resource->aspect, resource->viewType,
                                     mipLevel, 1, 0, resource->arrayLayers);
        if (perMipView == VK_NULL_HANDLE) {
            MGLOG_D("%s: CreateImageView failed for textureId=%d mipLevel=%u", __func__, texture.GetExternalIndex(), mipLevel);
            return VK_NULL_HANDLE;
        }

        return perMipView;
    }

    VkImageView VkTextureManager::GetOrCreateAttachmentViewAtMipLevel(MG_State::GLState::ITextureObject& texture,
                                                                      Uint32 mipLevel, Uint32 baseArrayLayer,
                                                                      Uint32 layerCount,
                                                                      VkImageViewType viewType) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE || mipLevel >= resource->mipLevels) {
            return VK_NULL_HANDLE;
        }
        // A 3D image has arrayLayers == 1 and keeps its GL layers on the z axis, so a per-slice
        // attachment view is a 2D view whose "array layer" is the slice - legal only on a
        // 2D-array-compatible image (VUID-VkImageViewCreateInfo-image-04970), which
        // SyncTextureResource asks for and may have had refused per format.
        if (resource->viewType == VK_IMAGE_VIEW_TYPE_3D && viewType == VK_IMAGE_VIEW_TYPE_2D) {
            const Uint32 sliceCount = std::max(resource->depth >> mipLevel, 1u);
            if ((resource->imageCreateFlags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) == 0 ||
                layerCount == 0 || baseArrayLayer >= sliceCount || baseArrayLayer + layerCount > sliceCount) {
                MGLOG_D("%s: cannot name slice span [%u, %u) of 3D textureId=%d (mip %u has %u slices, "
                        "2D-array-compatible=%d)",
                        __func__, baseArrayLayer, baseArrayLayer + layerCount, texture.GetExternalIndex(),
                        mipLevel, sliceCount,
                        (int)((resource->imageCreateFlags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0));
                return VK_NULL_HANDLE;
            }
        } else if (layerCount == 0 || baseArrayLayer >= resource->arrayLayers ||
                   baseArrayLayer + layerCount > resource->arrayLayers) {
            MGLOG_D("%s: invalid layer span [%u, %u) for textureId=%d arrayLayers=%u",
                    __func__, baseArrayLayer, baseArrayLayer + layerCount, texture.GetExternalIndex(),
                    resource->arrayLayers);
            return VK_NULL_HANDLE;
        }

        const Bool framebufferSrgbEnabled =
            MG_State::pGLContext->IsCapabilityEnabled(MobileGL::CapabilityInput::FramebufferSrgb);
        const VkFormat attachmentFormat = ResolveSrgbAttachmentWriteFormat(resource->format, framebufferSrgbEnabled);

        if (attachmentFormat == resource->format && baseArrayLayer == 0 && layerCount == resource->arrayLayers &&
            viewType == resource->viewType) {
            return GetOrCreateViewAtMipLevel(texture, mipLevel);
        }

        const TextureResource::AttachmentViewKey key{
            .mipLevel = mipLevel,
            .baseArrayLayer = baseArrayLayer,
            .layerCount = layerCount,
            .viewType = viewType,
            .viewFormat = attachmentFormat,
        };
        auto it = resource->attachmentViews.find(key);
        if (it == resource->attachmentViews.end()) {
            it = resource->attachmentViews.emplace(key, VK_NULL_HANDLE).first;
        }
        VkImageView& attachmentView = it->second;
        if (attachmentView != VK_NULL_HANDLE) {
            return attachmentView;
        }

        attachmentView = CreateImageView(resource->image, attachmentFormat, resource->aspect, viewType,
                                         mipLevel, 1, baseArrayLayer, layerCount);
        if (attachmentView == VK_NULL_HANDLE) {
            MGLOG_D("%s: CreateImageView failed for textureId=%d mipLevel=%u baseArrayLayer=%u layerCount=%u viewType=%d",
                    __func__, texture.GetExternalIndex(), mipLevel, baseArrayLayer, layerCount, static_cast<Int>(viewType));
            resource->attachmentViews.erase(it);
            return VK_NULL_HANDLE;
        }

        return attachmentView;
    }

    VkImageView VkTextureManager::GetOrCreateSampledViewAtMipLevel(MG_State::GLState::ITextureObject& texture,
                                                                   Uint32 mipLevel) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE || mipLevel >= resource->mipLevels) {
            return VK_NULL_HANDLE;
        }

        if (resource->perMipSampledViews.size() != resource->mipLevels) {
            resource->perMipSampledViews.resize(resource->mipLevels, VK_NULL_HANDLE);
        }

        VkImageView& perMipSampledView = resource->perMipSampledViews[mipLevel];
        if (perMipSampledView != VK_NULL_HANDLE) {
            return perMipSampledView;
        }

        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkComponentMapping sampledComponents = ResolveSampledViewComponents(texture, formatInfo);
        const VkImageAspectFlags sampledAspect =
            ResolveSampledImageViewAspectMask(resource->aspect, texture.GetDepthStencilTextureMode());
        perMipSampledView = CreateImageView(resource->image, resource->format, sampledAspect, resource->viewType,
                                            mipLevel, 1, 0, resource->arrayLayers, &sampledComponents);
        if (perMipSampledView == VK_NULL_HANDLE) {
            MGLOG_D("%s: CreateImageView failed for textureId=%d mipLevel=%u", __func__, texture.GetExternalIndex(),
                    mipLevel);
            return VK_NULL_HANDLE;
        }

        return perMipSampledView;
    }

    VkImageView VkTextureManager::GetOrCreateSampledImageView(MG_State::GLState::ITextureObject& texture,
                                                               VkFormat format) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE ||
            resource->sampledView == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        if (format == VK_FORMAT_UNDEFINED || format == resource->format) {
            return resource->sampledView;
        }
        if (!AreSampledImageViewFormatsCompatible(resource->format, format)) {
            MGLOG_E_ONCE("%s: incompatible sampled image view format=%d for textureId=%d imageFormat=%d",
                    __func__, static_cast<Int>(format), texture.GetExternalIndex(),
                    static_cast<Int>(resource->format));
            return VK_NULL_HANDLE;
        }
        if ((resource->imageCreateFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0) {
            MGLOG_E_ONCE("%s: textureId=%d needs mutable image format=%d for sampled view format=%d",
                    __func__, texture.GetExternalIndex(), static_cast<Int>(resource->format),
                    static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }

        const TextureResource::SampledImageViewKey key{
            .baseMipLevel = resource->sampledBaseMipLevel,
            .levelCount = resource->sampledLevelCount,
            .viewType = resource->viewType,
            .format = format,
        };
        const auto existing = resource->alternateSampledViews.find(key);
        if (existing != resource->alternateSampledViews.end()) {
            return existing->second;
        }

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
        if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            MGLOG_E_ONCE("%s: sampled image view format=%d lacks VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT "
                    "for textureId=%d (available=0x%x)",
                    __func__, static_cast<Int>(format), texture.GetExternalIndex(),
                    static_cast<Uint32>(formatProperties.optimalTilingFeatures));
            return VK_NULL_HANDLE;
        }

        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkComponentMapping sampledComponents = ResolveSampledViewComponents(texture, formatInfo);
        const VkImageView view = CreateImageView(
            resource->image, format, VK_IMAGE_ASPECT_COLOR_BIT, resource->viewType,
            resource->sampledBaseMipLevel, resource->sampledLevelCount, 0, resource->arrayLayers,
            &sampledComponents, VK_IMAGE_USAGE_SAMPLED_BIT);
        if (view == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("%s: failed to create sampled image view textureId=%d imageFormat=%d viewFormat=%d",
                    __func__, texture.GetExternalIndex(), static_cast<Int>(resource->format),
                    static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }

        resource->alternateSampledViews.emplace(key, view);
        MGLOG_D("%s: created sampled image view textureId=%d imageFormat=%d viewFormat=%d mip=[%u,%u)",
                __func__, texture.GetExternalIndex(), static_cast<Int>(resource->format),
                static_cast<Int>(format), resource->sampledBaseMipLevel,
                resource->sampledBaseMipLevel + resource->sampledLevelCount);
        return view;
    }

    VkImageView VkTextureManager::GetOrCreateStorageImageView(MG_State::GLState::ITextureObject& texture,
                                                               Uint32 mipLevel, VkFormat format,
                                                               Bool layered, Int32 layer) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr || resource->image == VK_NULL_HANDLE || mipLevel >= resource->mipLevels ||
            resource->sampleCount != VK_SAMPLE_COUNT_1_BIT ||
            (resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
            return VK_NULL_HANDLE;
        }

        if (format == VK_FORMAT_UNDEFINED) {
            format = resource->format;
        }
        if (!AreStorageImageViewFormatsCompatible(resource->format, format)) {
            MGLOG_E_ONCE("%s: incompatible storage image view format=%d for textureId=%d imageFormat=%d",
                    __func__, static_cast<Int>(format), texture.GetExternalIndex(),
                    static_cast<Int>(resource->format));
            return VK_NULL_HANDLE;
        }
        if (format != resource->format &&
            (resource->imageCreateFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0) {
            MGLOG_E_ONCE("%s: textureId=%d needs mutable image format=%d for storage view format=%d",
                    __func__, texture.GetExternalIndex(), static_cast<Int>(resource->format),
                    static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }

        Uint32 baseArrayLayer = 0;
        Uint32 layerCount = resource->arrayLayers;
        VkImageViewType viewType = resource->viewType;
        if (!layered) {
            switch (resource->viewType) {
            case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
                viewType = VK_IMAGE_VIEW_TYPE_1D;
                break;
            case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            case VK_IMAGE_VIEW_TYPE_CUBE:
            case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            case VK_IMAGE_VIEW_TYPE_3D:
                MGLOG_E_ONCE("%s: non-layered 3D storage views are unsupported for textureId=%d",
                        __func__, texture.GetExternalIndex());
                return VK_NULL_HANDLE;
            default:
                break;
            }

            if (viewType != resource->viewType) {
                if (layer < 0 || static_cast<Uint32>(layer) >= resource->arrayLayers) {
                    MGLOG_E_ONCE("%s: storage image layer=%d is out of range for textureId=%d arrayLayers=%u",
                            __func__, layer, texture.GetExternalIndex(), resource->arrayLayers);
                    return VK_NULL_HANDLE;
                }
                baseArrayLayer = static_cast<Uint32>(layer);
                layerCount = 1;
            }
        }

        const Bool isFullResourceView = baseArrayLayer == 0 && layerCount == resource->arrayLayers &&
                                        viewType == resource->viewType;
        if (format == resource->format && isFullResourceView) {
            return GetOrCreateViewAtMipLevel(texture, mipLevel);
        }

        const TextureResource::StorageImageViewKey key{
            .mipLevel = mipLevel,
            .baseArrayLayer = baseArrayLayer,
            .layerCount = layerCount,
            .viewType = viewType,
            .format = format,
        };
        auto it = resource->storageImageViews.find(key);
        if (it != resource->storageImageViews.end()) {
            return it->second;
        }

        VkFormatFeatureFlags requiredFormatFeatures = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (format != resource->format &&
            (format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT)) {
            requiredFormatFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        }
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
        if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) != requiredFormatFeatures) {
            MGLOG_E_ONCE("%s: storage image view format=%d lacks required features=0x%x for textureId=%d "
                    "(available=0x%x)",
                    __func__, static_cast<Int>(format), static_cast<Uint32>(requiredFormatFeatures),
                    texture.GetExternalIndex(), static_cast<Uint32>(formatProperties.optimalTilingFeatures));
            return VK_NULL_HANDLE;
        }

        const VkImageView view = CreateImageView(resource->image, format, VK_IMAGE_ASPECT_COLOR_BIT, viewType,
                                                 mipLevel, 1, baseArrayLayer, layerCount, nullptr,
                                                 VK_IMAGE_USAGE_STORAGE_BIT);
        if (view == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("%s: failed to create storage image view for textureId=%d mip=%u imageFormat=%d viewFormat=%d",
                    __func__, texture.GetExternalIndex(), mipLevel, static_cast<Int>(resource->format),
                    static_cast<Int>(format));
            return VK_NULL_HANDLE;
        }
        resource->storageImageViews.emplace(key, view);
        MGLOG_D("%s: created storage image view textureId=%d mip=%u imageFormat=%d viewFormat=%d",
                __func__, texture.GetExternalIndex(), mipLevel, static_cast<Int>(resource->format),
                static_cast<Int>(format));
        return view;
    }

    void VkTextureManager::StampTextureRecordingUse(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }
        auto it = m_textureResources.find(MakeTextureIdentity(texture));
        if (it != m_textureResources.end()) {
            it->second.lastRecordingGeneration = m_recordingGeneration;
        }
    }

    void VkTextureManager::UpdateTrackedImageLayout(MG_State::GLState::ITextureObject* texture, VkImageLayout newLayout) {
        MOBILEGL_ASSERT(texture != nullptr, "UpdateTrackedImageLayout: texture is null");
        auto it = m_textureResources.find(MakeTextureIdentity(texture));
        MOBILEGL_ASSERT(it != m_textureResources.end(),
                        "UpdateTrackedImageLayout: textureId=%d has no tracked resource", texture->GetExternalIndex());
        MOBILEGL_ASSERT(it->second.image != VK_NULL_HANDLE,
                        "UpdateTrackedImageLayout: textureId=%d has null image", texture->GetExternalIndex());
        it->second.layout = newLayout;
    }

    void VkTextureManager::UpdateTrackedImageLayoutAfterAttachmentWrite(VkCommandBuffer commandBuffer,
                                                                        MG_State::GLState::ITextureObject* texture,
                                                                        Uint32 writtenMipLevel,
                                                                        VkImageLayout newLayout) {
        MOBILEGL_ASSERT(texture != nullptr, "UpdateTrackedImageLayoutAfterAttachmentWrite: texture is null");
        auto it = m_textureResources.find(MakeTextureIdentity(texture));
        MOBILEGL_ASSERT(it != m_textureResources.end(),
                        "UpdateTrackedImageLayoutAfterAttachmentWrite: textureId=%d has no tracked resource",
                        texture->GetExternalIndex());

        auto& resource = it->second;
        MOBILEGL_ASSERT(resource.image != VK_NULL_HANDLE,
                        "UpdateTrackedImageLayoutAfterAttachmentWrite: textureId=%d has null image",
                        texture->GetExternalIndex());
        MOBILEGL_ASSERT(writtenMipLevel < resource.mipLevels,
                        "UpdateTrackedImageLayoutAfterAttachmentWrite: textureId=%d mipLevel=%u out of range %u",
                        texture->GetExternalIndex(), writtenMipLevel, resource.mipLevels);
        // Pre-pass stream bookkeeping: the render pass that just ended wrote this image.
        StampResourceRecordingUse(resource);

        if (resource.layout != newLayout && resource.mipLevels > 1) {
            VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags srcAccessMask = 0;
            GetImageTransitionSourceState(resource.layout, srcStageMask, srcAccessMask);

            VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags dstAccessMask = 0;
            GetImageTransitionDestinationState(newLayout, dstStageMask, dstAccessMask);

            if (writtenMipLevel > 0) {
                VkImageLayout lowerMipLayout = resource.layout;
                const Bool lowerTransitioned = TransitionImageLayout(
                    commandBuffer, resource.image, lowerMipLayout, newLayout,
                    srcStageMask, dstStageMask, srcAccessMask, dstAccessMask,
                    resource.aspect, 0, writtenMipLevel);
                MOBILEGL_ASSERT(lowerTransitioned,
                                "UpdateTrackedImageLayoutAfterAttachmentWrite: failed to transition lower mip levels for textureId=%d",
                                texture->GetExternalIndex());
            }

            const Uint32 upperBaseMipLevel = writtenMipLevel + 1;
            if (upperBaseMipLevel < resource.mipLevels) {
                VkImageLayout upperMipLayout = resource.layout;
                const Bool upperTransitioned = TransitionImageLayout(
                    commandBuffer, resource.image, upperMipLayout, newLayout,
                    srcStageMask, dstStageMask, srcAccessMask, dstAccessMask,
                    resource.aspect, upperBaseMipLevel, resource.mipLevels - upperBaseMipLevel);
                MOBILEGL_ASSERT(upperTransitioned,
                                "UpdateTrackedImageLayoutAfterAttachmentWrite: failed to transition upper mip levels for textureId=%d",
                                texture->GetExternalIndex());
            }
        }

        resource.layout = newLayout;
    }

    Bool VkTextureManager::TransitionTextureForSampling(VkCommandBuffer commandBuffer, MG_State::GLState::ITextureObject& texture) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr) {
            return false;
        }
        if (IsValidSampledImageLayout(resource->layout)) {
            return true;
        }
        if (resource->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            MGLOG_W_ONCE("TransitionTextureForSampling: textureId=%d is still in VK_IMAGE_LAYOUT_UNDEFINED before sampling",
                    texture.GetExternalIndex());
        }

        VkImageLayout targetLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        if ((resource->aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            MOBILEGL_ASSERT(resource->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                            resource->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            "TransitionTextureForSampling: unsupported color layout=%d for textureId=%d",
                            static_cast<Int>(resource->layout), texture.GetExternalIndex());
            if (resource->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            targetLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if ((resource->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0) {
            MOBILEGL_ASSERT(resource->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                            resource->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            "TransitionTextureForSampling: unsupported depth/stencil layout=%d for textureId=%d",
                            static_cast<Int>(resource->layout), texture.GetExternalIndex());
            if (resource->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            }
            targetLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        } else {
            MOBILEGL_ASSERT(false, "TransitionTextureForSampling: unsupported aspect mask=0x%x for textureId=%d",
                            static_cast<Uint32>(resource->aspect), texture.GetExternalIndex());
        }

        const Bool ok = TransitionImageLayout(commandBuffer, resource->image, resource->layout, targetLayout, srcStageMask,
                                              s_sampledReadStages, srcAccessMask,
                                              VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels);
        MOBILEGL_ASSERT(ok, "TransitionTextureForSampling: transition failed for textureId=%d", texture.GetExternalIndex());
        // Pre-pass stream bookkeeping: a command referencing the image was recorded.
        StampResourceRecordingUse(*resource);
        return ok;
    }

    Bool VkTextureManager::TransitionTextureForStorageImage(VkCommandBuffer commandBuffer,
                                                            MG_State::GLState::ITextureObject& texture) {
        TextureResource* resource = SyncTextureAndGetDescriptor(texture);
        if (resource == nullptr) {
            return false;
        }
        if (resource->sampleCount != VK_SAMPLE_COUNT_1_BIT) {
            MGLOG_D("TransitionTextureForStorageImage: multisample textureId=%d is not exposed as a storage image",
                    texture.GetExternalIndex());
            return false;
        }
        if (resource->layout == VK_IMAGE_LAYOUT_GENERAL) {
            return true;
        }

        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkAccessFlags srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        GetImageTransitionSourceState(resource->layout, srcStageMask, srcAccessMask);

        const Bool ok = TransitionImageLayout(commandBuffer, resource->image, resource->layout,
                                              VK_IMAGE_LAYOUT_GENERAL, srcStageMask,
                                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, srcAccessMask,
                                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                              resource->aspect, 0, resource->mipLevels);
        MOBILEGL_ASSERT(ok, "TransitionTextureForStorageImage: transition failed for textureId=%d",
                        texture.GetExternalIndex());
        // Pre-pass stream bookkeeping: a command referencing the image was recorded.
        StampResourceRecordingUse(*resource);
        return ok;
    }

    Bool VkTextureManager::SnapshotTextureForSampling(VkCommandBuffer commandBuffer,
                                                      MG_State::GLState::ITextureObject& texture,
                                                      SamplerNumericDomain numericDomain,
                                                      VkPipelineStageFlags consumerShaderStageMask,
                                                      SampledTextureSnapshot& outSnapshot) {
        outSnapshot = {};
        TextureResource* source = SyncTextureAndGetDescriptor(texture);
        if (source == nullptr || source->image == VK_NULL_HANDLE || source->sampleCount != VK_SAMPLE_COUNT_1_BIT ||
            source->sampledLevelCount == 0) {
            return false;
        }

        const VkFormat sampledFormat = ResolveSampledImageViewFormat(source->format, numericDomain);
        if (sampledFormat == VK_FORMAT_UNDEFINED ||
            !AreSampledImageViewFormatsCompatible(source->format, sampledFormat)) {
            MGLOG_E_ONCE("SnapshotTextureForSampling: textureId=%d cannot create sampled view format=%d from image format=%d",
                         texture.GetExternalIndex(), static_cast<Int>(sampledFormat), static_cast<Int>(source->format));
            return false;
        }
        if (sampledFormat != source->format &&
            (source->imageCreateFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0) {
            MGLOG_E_ONCE("SnapshotTextureForSampling: textureId=%d needs unavailable mutable image format=%d for sampled view=%d",
                         texture.GetExternalIndex(), static_cast<Int>(source->format), static_cast<Int>(sampledFormat));
            return false;
        }

        VkImageType imageType = VK_IMAGE_TYPE_2D;
        switch (source->viewType) {
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
            imageType = VK_IMAGE_TYPE_1D;
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            imageType = VK_IMAGE_TYPE_3D;
            break;
        default:
            break;
        }

        TextureResource snapshot{};
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = source->imageCreateFlags;
        imageInfo.imageType = imageType;
        imageInfo.extent = {source->extent.width, source->extent.height, source->depth};
        imageInfo.mipLevels = source->mipLevels;
        imageInfo.arrayLayers = source->arrayLayers;
        imageInfo.format = source->format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Keep the temporary's view-format list just as narrow as the source's sampler use. This
        // has no storage-image usage, so unlike an app image binding the exact list is knowable.
        Vector<VkFormat> viewFormats;
        VkImageFormatListCreateInfo formatListInfo{};
        if (m_imageFormatListSupported && (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
            viewFormats.push_back(source->format);
            if (sampledFormat != source->format) {
                viewFormats.push_back(sampledFormat);
            }
            formatListInfo.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            formatListInfo.viewFormatCount = static_cast<Uint32>(viewFormats.size());
            formatListInfo.pViewFormats = viewFormats.data();
            imageInfo.pNext = &formatListInfo;
        }

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const VkResult createResult =
            vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &snapshot.image, &snapshot.allocation, nullptr);
        if (createResult != VK_SUCCESS) {
            MGLOG_E_ONCE("SnapshotTextureForSampling: vmaCreateImage failed result=%d textureId=%d", createResult,
                         texture.GetExternalIndex());
            return false;
        }

        snapshot.extent = source->extent;
        snapshot.depth = source->depth;
        snapshot.arrayLayers = source->arrayLayers;
        snapshot.mipLevels = source->mipLevels;
        snapshot.sampledBaseMipLevel = source->sampledBaseMipLevel;
        snapshot.sampledLevelCount = source->sampledLevelCount;
        snapshot.format = source->format;
        snapshot.aspect = source->aspect;
        snapshot.viewType = source->viewType;
        snapshot.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        snapshot.imageCreateFlags = imageInfo.flags;
        snapshot.usageFlags = imageInfo.usage;

        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkComponentMapping sampledComponents = ResolveSampledViewComponents(texture, formatInfo);
        const VkImageAspectFlags sampledAspect =
            ResolveSampledImageViewAspectMask(snapshot.aspect, texture.GetDepthStencilTextureMode());
        snapshot.sampledView = CreateImageView(snapshot.image, sampledFormat, sampledAspect, snapshot.viewType,
                                               snapshot.sampledBaseMipLevel, snapshot.sampledLevelCount, 0,
                                               snapshot.arrayLayers, &sampledComponents);
        if (snapshot.sampledView == VK_NULL_HANDLE) {
            MGLOG_E_ONCE("SnapshotTextureForSampling: failed to create sampled view textureId=%d", texture.GetExternalIndex());
            return false;
        }

        VkPipelineStageFlags sourceStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags sourceAccessMask = 0;
        const VkImageLayout sourceLayout = source->layout;
        GetImageTransitionSourceState(sourceLayout, sourceStageMask, sourceAccessMask);
        if (!TransitionImageLayout(commandBuffer, source->image, source->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   sourceStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT, sourceAccessMask,
                                   VK_ACCESS_TRANSFER_READ_BIT, source->aspect, 0, source->mipLevels) ||
            !TransitionImageLayout(commandBuffer, snapshot.image, snapshot.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, snapshot.aspect, snapshot.sampledBaseMipLevel,
                                   snapshot.sampledLevelCount)) {
            return false;
        }

        Vector<VkImageCopy> copyRegions;
        copyRegions.reserve(snapshot.sampledLevelCount);
        for (Uint32 level = snapshot.sampledBaseMipLevel;
             level < snapshot.sampledBaseMipLevel + snapshot.sampledLevelCount; ++level) {
            VkImageCopy copy{};
            copy.srcSubresource = {source->aspect, level, 0, source->arrayLayers};
            copy.dstSubresource = {snapshot.aspect, level, 0, snapshot.arrayLayers};
            copy.extent = {std::max(source->extent.width >> level, 1u),
                           std::max(source->extent.height >> level, 1u),
                           std::max(source->depth >> level, 1u)};
            copyRegions.push_back(copy);
        }
        vkCmdCopyImage(commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<Uint32>(copyRegions.size()), copyRegions.data());

        if (!TransitionImageLayout(commandBuffer, snapshot.image, snapshot.layout,
                                   ResolveSampledReadOnlyLayout(snapshot.aspect), VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   consumerShaderStageMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, snapshot.aspect, snapshot.sampledBaseMipLevel,
                                   snapshot.sampledLevelCount) ||
            !TransitionImageLayout(commandBuffer, source->image, source->layout, sourceLayout,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, consumerShaderStageMask,
                                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                   source->aspect, 0, source->mipLevels)) {
            return false;
        }

        StampResourceRecordingUse(*source);
        outSnapshot = {.imageView = snapshot.sampledView, .layout = snapshot.layout};
        DeferResourceRelease(Move(snapshot));
        return true;
    }

    void VkTextureManager::MarkStorageImageTexture(MG_State::GLState::ITextureObject& texture) {
        m_storageImageTextures.insert(MakeTextureIdentity(&texture));
    }

    Bool VkTextureManager::NeedsStorageUsageUpgrade(MG_State::GLState::ITextureObject& texture) const {
        const TextureIdentity identity = MakeTextureIdentity(&texture);
        if (m_storageImageTextures.find(identity) == m_storageImageTextures.end()) {
            return false;
        }
        const auto it = m_textureResources.find(identity);
        // No image yet: the first sync creates it with STORAGE straight away, so there is nothing
        // to preserve and nothing to order against.
        return it != m_textureResources.end() && it->second.image != VK_NULL_HANDLE &&
               !it->second.storageUsageResolved;
    }

    Bool VkTextureManager::NeedsMipChainGrowth(MG_State::GLState::ITextureObject& texture) const {
        const TextureIdentity identity = MakeTextureIdentity(&texture);
        const auto it = m_textureResources.find(identity);
        // No image yet: the first sync sizes the chain from the levels the texture already
        // defines, so nothing is recreated and there is nothing to order against.
        if (it == m_textureResources.end() || it->second.image == VK_NULL_HANDLE) {
            return false;
        }
        const TextureResource& resource = it->second;
        const IntVec3 extent = {static_cast<Int>(resource.extent.width), static_cast<Int>(resource.extent.height),
                                static_cast<Int>(resource.depth)};
        return resource.mipLevels < ComputeFullMipLevelCount(extent);
    }

    Bool VkTextureManager::NeedsStorageImagePreparation(MG_State::GLState::ITextureObject& texture) const {
        const TextureIdentity identity = MakeTextureIdentity(&texture);
        const auto it = m_textureResources.find(identity);
        if (it == m_textureResources.end()) {
            return true;
        }
        const TextureResource& resource = it->second;
        if (resource.image == VK_NULL_HANDLE || resource.layout != VK_IMAGE_LAYOUT_GENERAL) {
            return true;
        }
        // The image predates this texture's first image-unit binding, so it was created without
        // STORAGE usage and has to be recreated - which is illegal inside a render pass.
        if (!resource.storageUsageResolved &&
            m_storageImageTextures.find(identity) != m_storageImageTextures.end()) {
            return true;
        }
        // Mirror SyncTexture's cross-draw skip condition: any version drift means the sync
        // path may upload or rebuild, both of which need the render pass ended first.
        const auto* mipTexture = MG_State::GLState::AsMipmapTexture(&texture);
        const Uint32 mipLevelCount = mipTexture != nullptr ? mipTexture->GetMipmapLevelCount() : 0u;
        return resource.syncedContentVersion != texture.GetContentVersion() ||
               resource.syncedTextureParamsVersion != texture.GetTextureParamsVersion() ||
               resource.syncedMipLevelCount != mipLevelCount;
    }

    Bool VkTextureManager::TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                                                 VkImageLayout& trackedLayout, VkImageLayout newLayout,
                                                 VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                                                 VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
                                                 VkImageAspectFlags aspectMask, Uint32 baseMipLevel,
                                                 Uint32 levelCount) {
        MOBILEGL_ASSERT(image != VK_NULL_HANDLE, "TransitionImageLayout: m_image == VK_NULL_HANDLE");
        MOBILEGL_ASSERT(!((dstAccessMask & VK_ACCESS_TRANSFER_READ_BIT) != 0 &&
                          (dstStageMask & VK_PIPELINE_STAGE_TRANSFER_BIT) == 0),
                        "TransitionImageLayout: invalid dstAccess/dstStage pair (dstAccess=0x%x, dstStage=0x%x, oldLayout=%d, newLayout=%d)",
                        static_cast<Uint32>(dstAccessMask), static_cast<Uint32>(dstStageMask), static_cast<Int>(trackedLayout),
                        static_cast<Int>(newLayout));

        if (trackedLayout == newLayout) {
            return true;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccessMask;
        barrier.dstAccessMask = dstAccessMask;
        barrier.oldLayout = trackedLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspectMask;
        barrier.subresourceRange.baseMipLevel = baseMipLevel;
        barrier.subresourceRange.levelCount = levelCount;
        barrier.subresourceRange.baseArrayLayer = 0;
        // Every layer, always - see the declaration for why layout tracking leaves no other
        // correct answer. VK_REMAINING_ARRAY_LAYERS rather than the image's own `arrayLayers`
        // because those are not the same number for a 3D image: MobileGL creates 3D images
        // 2D_ARRAY_COMPATIBLE and their arrayLayers is 1, which today Vulkan reads as "all depth
        // slices" but will read as "depth slice 0" once VK_KHR_maintenance9 is enabled. The
        // validation layer warns about that literal 1 by name.
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        trackedLayout = newLayout;
        return true;
    }

    SizeT VkTextureManager::CollectGarbage() {
        // Draw-gated stagger (1 in 256 calls): keeps the per-draw cost at one counter
        // bump. The guaranteed reclaim path is the frame-boundary prune in BeginFrame;
        // this remains as a cheap assist so draw-heavy workloads reclaim sooner.
        m_gcCounter++;
        if (m_gcCounter != 0) {
            return 0;
        }
        return PruneDeadTextures();
    }

    SizeT VkTextureManager::PruneDeadTextures() {
        // Erasing entries would dangle the raw TextureResource pointers memoized for the
        // current draw; every call path (BeginFrame, and CollectGarbage at the top of a
        // freshly opened draw-sync scope) runs before any memo entry is recorded.
        MOBILEGL_ASSERT(m_drawSyncedThisDraw.empty(),
                        "PruneDeadTextures: draw-sync memo holds raw resource pointers an erase would dangle");

        Vector<MG_State::GLState::ITextureObject*> expiredTextures;
        expiredTextures.reserve(m_aliveObjects.size());
        for (auto it = m_aliveObjects.begin(); it != m_aliveObjects.end(); ++it) {
            if (it->second.expired()) {
                expiredTextures.emplace_back(it->first.texture);
            }
        }
        for (auto* texture : expiredTextures) {
            PruneStaleTextureAliases(texture);
        }
        SizeT prunedCount = expiredTextures.size();

        // Orphan sweep: after the pass above, m_aliveObjects holds only live entries.
        // Registration in SyncTextureAndGetDescriptor cannot fail for a SharedPtr-owned
        // texture (weak_from_this fallback), so a resource whose identity has no alive
        // entry has no trackable owner: its GL-side object is gone, or was never
        // shared-owned, in which case recreation on a later sync is the safe fallback.
        // Destruction goes through the per-frame deferred queues, never immediate.
        Vector<TextureIdentity> orphanIdentities;
        for (auto it = m_textureResources.begin(); it != m_textureResources.end(); ++it) {
            if (m_aliveObjects.find(it->first) == m_aliveObjects.end()) {
                orphanIdentities.emplace_back(it->first);
            }
        }
        for (const auto& identity : orphanIdentities) {
            EraseTrackedTexture(identity);
        }
        prunedCount += orphanIdentities.size();
        return prunedCount;
    }

    Bool VkTextureManager::SyncTexture(MG_State::GLState::ITextureObject &texture,
                                       TextureResource &outResource) {
        // Cross-draw fast path: if the resource is already built and neither the texture's
        // pixel content (bumped in MarkStorageDirty) nor its params changed since the last
        // sync, there is nothing to re-check or re-upload - skip CheckMipmapCompleteness,
        // SyncTextureResource, SyncTextureViews and the per-level dirty scan. Layout is
        // maintained separately by the transition path, so the resource still reflects truth.
        const Uint64 syncingContentVersion = texture.GetContentVersion();
        const auto* syncingMipTexture = MG_State::GLState::AsMipmapTexture(&texture);
        const Uint32 syncingMipLevelCount =
            syncingMipTexture != nullptr ? syncingMipTexture->GetMipmapLevelCount() : 0u;
        // A pending storage-usage upgrade also has to bust the skip: nothing about the texture's
        // content or params changed, but the image itself must be recreated with STORAGE usage
        // before it can back an image-unit descriptor.
        const Bool storageUpgradePending =
            !outResource.storageUsageResolved &&
            m_storageImageTextures.find(MakeTextureIdentity(&texture)) != m_storageImageTextures.end();
        if (outResource.image != VK_NULL_HANDLE && !storageUpgradePending &&
            outResource.syncedContentVersion == syncingContentVersion &&
            outResource.syncedTextureParamsVersion == texture.GetTextureParamsVersion() &&
            outResource.syncedMipLevelCount == syncingMipLevelCount) {
            return true;
        }

        TextureUploadTarget uploadTarget = TextureUploadTarget::Unknown;
        IntVec3 texelSize{0, 0, 0};
        SizeT byteSize = 0;
        Uint32 mipLevelCount = 0;
        if (!CheckMipmapCompleteness(texture, uploadTarget, texelSize, byteSize, mipLevelCount)) {
            MGLOG_D("%s: mipmap not complete", __func__);
            return false;
        }

        auto* mipTexture = MG_State::GLState::AsMipmapTexture(&texture);
        if (!mipTexture) {
            MGLOG_D("%s: not TextureObjectMipmap", __func__);
            return false;
        }

        if (!SyncTextureResource(texture, uploadTarget, texelSize, byteSize, mipLevelCount, outResource)) {
            MGLOG_D("%s: SyncTextureResource failed", __func__);
            return false;
        }
        if (!SyncTextureViews(texture, outResource)) {
            MGLOG_D("%s: SyncTextureViews failed", __func__);
            return false;
        }

        Vector<TextureUploadTarget> dirtyTargets;
        if (outResource.viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            dirtyTargets = mipTexture->GetUploadTargets();
        } else {
            dirtyTargets.push_back(uploadTarget);
        }
        Bool hasDirtyMipLevel = false;
        for (const TextureUploadTarget target : dirtyTargets) {
            const Uint32 targetMipLevelCount = std::min(mipLevelCount, GetUploadMipLevelCount(*mipTexture, target));
            for (Uint32 level = 0; level < targetMipLevelCount; ++level) {
                if (mipTexture->IsStorageDirty(target, level)) {
                    hasDirtyMipLevel = true;
                    break;
                }
            }
            if (hasDirtyMipLevel) {
                break;
            }
        }
        if (!hasDirtyMipLevel) {
            outResource.syncedContentVersion = syncingContentVersion;
            outResource.syncedMipLevelCount = syncingMipLevelCount;
            return true;
        }

        if (!UploadDirtyMipLevels(*mipTexture, uploadTarget, outResource)) {
            MGLOG_D("%s: UploadDirtyMipLevels failed", __func__);
            return false;
        }
        outResource.syncedContentVersion = syncingContentVersion;
        outResource.syncedMipLevelCount = syncingMipLevelCount;
        return true;
    }

    Bool VkTextureManager::SyncTextureResource(const MG_State::GLState::ITextureObject &texture,
                                               TextureUploadTarget uploadTarget,
                                               const IntVec3 &texelSize, SizeT byteSize, Uint32 mipLevels,
                                               TextureResource &resource) {
        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        VkFormat format = formatInfo.format;
        if (format == VK_FORMAT_UNDEFINED) {
            MGLOG_D("%s: format == VK_FORMAT_UNDEFINED", __func__);
            return false;
        }
        // X8_D24 lacks optimal-tiling support on several drivers (lavapipe included);
        // D32_SFLOAT holds every 24-bit depth value exactly, and the upload path
        // converts the shadow words to float (see the pure-depth branch below).
        if (format == VK_FORMAT_X8_D24_UNORM_PACK32) {
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
            constexpr VkFormatFeatureFlags kDepthAttachmentAndSample =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((formatProperties.optimalTilingFeatures & kDepthAttachmentAndSample) != kDepthAttachmentAndSample) {
                format = VK_FORMAT_D32_SFLOAT;
            }
        }
        if (texelSize.x() <= 0 || texelSize.y() <= 0 /*|| byteSize == 0*/) {
            MGLOG_D("%s: texelSize or byteSize is zero", __func__);
            return false;
        }
        if (mipLevels == 0) {
            MGLOG_D("%s: no mip levels", __func__);
            return false;
        }
        const Bool isMultisampleTexture = IsMultisampleTextureUploadTarget(uploadTarget);
        // A texture that has only ever defined level 0 gets a single-level backing
        // (ANGLE's model). Preallocating the full chain put every render target
        // onto Adreno's multi-mip image layout and grew each texture by a third
        // for levels most textures never define. Once a second level is defined
        // the backing is recreated ONE time with the full chain (the
        // preserve-copy path below carries the pixels over), so sequentially-
        // defined atlas mips do not recreate per level, and glGenerateMipmap -
        // which defines every level before syncing - works unchanged.
        TextureShapeInfo shapeInfo{};
        const Bool supportedShape = TryResolveTextureShapeInfo(texture, uploadTarget, texelSize, shapeInfo);
        // ComputeFullMipLevelCount takes max(x, y, z), and for every ARRAY shape z is the layer
        // count, not a mip-able axis: a 4x4 array with 192 layers asked for 6 levels on an image
        // whose legal maximum is 3 (VUID-VkImageCreateInfo-mipLevels-00958). Only the image's own
        // extent - width, height and shapeInfo.depth, which is 1 for every array - can bound it.
        // lavapipe has been letting this through unvalidated; a strict driver would not.
        const IntVec3 mipExtent{texelSize.x(), texelSize.y(), static_cast<Int>(shapeInfo.depth)};
        const Uint32 fullMipLevels = ComputeFullMipLevelCount(mipExtent);
        const Uint32 backingMipLevels =
            isMultisampleTexture ? 1u : (mipLevels > 1 ? std::min(std::max(mipLevels, fullMipLevels), fullMipLevels) : 1u);
        if (!supportedShape) {
            // A gap in this backend's coverage, not a broken invariant: the GL front end accepts
            // targets this manager has no Vulkan image shape for yet (cube map arrays above all).
            // Declining the sync leaves the texture unbacked - wrong, but recoverable - where an
            // assertion would take the whole process down instead.
            MGLOG_W_ONCE("SyncTextureResource: unsupported uploadTarget=%s textureTarget=%s textureId=%d size=(%d,%d,%d) "
                    "mipLevels=%u vkViewType=%d",
                    MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                    MG_Util::ConvertTextureTargetToString(texture.GetTarget()).c_str(), texture.GetExternalIndex(),
                    texelSize.x(), texelSize.y(), texelSize.z(), mipLevels,
                    static_cast<Int>(MG_Util::ConvertTextureUploadTargetToVkEnum(uploadTarget)));
            return false;
        }
        VkSampleCountFlagBits resolvedSampleCount = VK_SAMPLE_COUNT_1_BIT;
        if (isMultisampleTexture &&
            !TryResolveSampleCountFlagBits(texture.GetSamples(), resolvedSampleCount)) {
            MGLOG_D("%s: unsupported multisample count=%d for textureId=%d target=%s", __func__,
                    texture.GetSamples(), texture.GetExternalIndex(),
                    MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str());
            return false;
        }
        // glTexStorage*Multisample(samples = 1) is legal GL, but a one-sample image cannot back a
        // sampler2DMS: VUID-RuntimeSpirv-samples-08726 forbids an OpTypeImage with MS = 1 from
        // reading an image created with VK_SAMPLE_COUNT_1_BIT, and the fetch returns undefined data
        // rather than an error. GL only promises "at least the requested number of samples", so
        // giving a multisample texture two is both legal and the only way to keep the shader's view
        // of it honest. GL_TEXTURE_SAMPLES still reports what the application asked for - that is
        // read off the texture object, not off the image.
        if (isMultisampleTexture && resolvedSampleCount == VK_SAMPLE_COUNT_1_BIT) {
            resolvedSampleCount = VK_SAMPLE_COUNT_2_BIT;
        }

        const VkImageAspectFlags aspect = GetAspectMaskForFormat(format);
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
        // Only textures that have actually been bound to a GL image unit get STORAGE usage (and
        // the MUTABLE_FORMAT it drags in for format-reinterpreting image views). Requesting it
        // for every storage-capable colour texture costs real bandwidth: Adreno cannot keep UBWC
        // compression on an image that may be written through a storage descriptor, so the whole
        // render target - MC's included - runs uncompressed. MarkStorageImageTexture upgrades a
        // texture before its first image-unit draw, and the usage below feeds the compatibility
        // check so the upgrade recreates the image.
        const Bool markedAsStorageImage =
            m_storageImageTextures.find(MakeTextureIdentity(
                const_cast<MG_State::GLState::ITextureObject*>(&texture))) != m_storageImageTextures.end();
        // Storage-image CAPABILITY (does the format allow it at all) is deliberately separate from
        // whether this texture actually needs the usage. MUTABLE_FORMAT keys off capability, as
        // before: format-reinterpreting views are not a storage-only concern - the SAMPLED path
        // needs them too (GetOrCreateSampledImageView bails out without it, see ~line 892), so
        // tying MUTABLE_FORMAT to the image-unit mark would break sampled format reinterpretation
        // for every texture that never becomes a storage image.
        const Bool storageImageCapable =
            !isMultisampleTexture &&
            (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        const Bool supportsStorageImage = storageImageCapable && markedAsStorageImage;
        VkImageCreateFlags imageCreateFlags = shapeInfo.imageFlags;
        // One z slice of a 3D texture can only be attached to a framebuffer through a 2D view over
        // it, which needs the image to be 2D-array-compatible (Vulkan 1.1 core, promoted from
        // VK_KHR_maintenance1). Asked for optimistically and withdrawn per format below if the
        // driver refuses - losing it only costs per-slice attachment, while failing creation would
        // lose the texture entirely.
        if (shapeInfo.imageType == VK_IMAGE_TYPE_3D && !isMultisampleTexture &&
            m_2dArrayCompatibleUnsupported.find(format) == m_2dArrayCompatibleUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        }
        if (storageImageCapable && IsMutableStorageImageFormat(format) &&
            m_mutableFormatUnsupported.find(format) == m_mutableFormatUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
        // sRGB color images attach through their UNORM twin while GL_FRAMEBUFFER_SRGB is
        // disabled (see ResolveSrgbAttachmentWriteFormat), which needs format-reinterpreting
        // views - multisample sRGB render targets included.
        if (ResolveSrgbAttachmentWriteFormat(format, false) != format &&
            (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            m_mutableFormatUnsupported.find(format) == m_mutableFormatUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }

        VkImageUsageFlags desiredUsage =
            VK_IMAGE_USAGE_SAMPLED_BIT |
            (supportsStorageImage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
            ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
            (((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) ?
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                 0);
        if (!isMultisampleTexture) {
            desiredUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        // Round a multisample request up to a count the device supports for this
        // format (GL only promises "at least"), mirroring the renderbuffer path.
        if (isMultisampleTexture && resolvedSampleCount != VK_SAMPLE_COUNT_1_BIT) {
            auto supportedIt = m_multisampleCountsByFormat.find(format);
            if (supportedIt == m_multisampleCountsByFormat.end()) {
                VkImageFormatProperties imageFormatProperties{};
                VkSampleCountFlags supported = VK_SAMPLE_COUNT_1_BIT;
                if (vkGetPhysicalDeviceImageFormatProperties(m_physicalDevice, format, shapeInfo.imageType,
                                                             VK_IMAGE_TILING_OPTIMAL, desiredUsage, imageCreateFlags,
                                                             &imageFormatProperties) == VK_SUCCESS) {
                    supported = imageFormatProperties.sampleCounts;
                }
                supportedIt = m_multisampleCountsByFormat.emplace(format, supported).first;
            }
            const VkSampleCountFlags supported = supportedIt->second;
            if ((supported & resolvedSampleCount) == 0) {
                Uint32 rounded = 0;
                for (Uint32 bit = static_cast<Uint32>(resolvedSampleCount) << 1; bit <= VK_SAMPLE_COUNT_64_BIT;
                     bit <<= 1) {
                    if ((supported & bit) != 0) {
                        rounded = bit;
                        break;
                    }
                }
                if (rounded == 0) {
                    // Never land on one sample: that is the VUID-RuntimeSpirv-samples-08726
                    // violation the floor above exists to avoid, and it would come back silently
                    // for any format whose only supported count is 1.
                    for (Uint32 bit = static_cast<Uint32>(resolvedSampleCount) >> 1;
                         bit > static_cast<Uint32>(VK_SAMPLE_COUNT_1_BIT); bit >>= 1) {
                        if ((supported & bit) != 0) {
                            rounded = bit;
                            break;
                        }
                    }
                }
                if (rounded != 0) {
                    resolvedSampleCount = static_cast<VkSampleCountFlagBits>(rounded);
                }
            }
        }

        const Bool compatible = resource.image != VK_NULL_HANDLE && resource.format == format &&
                                resource.extent.width == static_cast<Uint32>(texelSize.x()) &&
                                resource.extent.height == static_cast<Uint32>(texelSize.y()) &&
                                resource.depth == shapeInfo.depth &&
                                resource.arrayLayers == shapeInfo.arrayLayers &&
                                resource.viewType == shapeInfo.viewType &&
                                resource.sampleCount == resolvedSampleCount &&
                                resource.imageCreateFlags == imageCreateFlags &&
                                resource.usageFlags == desiredUsage &&
                                resource.mipLevels == backingMipLevels;
        if (compatible) {
            if (resource.perMipViews.size() != backingMipLevels) {
                resource.perMipViews.resize(backingMipLevels, VK_NULL_HANDLE);
            }
            if (resource.perMipSampledViews.size() != backingMipLevels) {
                resource.perMipSampledViews.resize(backingMipLevels, VK_NULL_HANDLE);
            }
            // Keeping the image is itself the answer to the mark: either it already carries
            // STORAGE, or this format can never carry it. Either way there is nothing left to
            // recreate, so stop reporting the texture as needing preparation.
            resource.storageUsageResolved = markedAsStorageImage;
            return true;
        }

        const Bool preserveExistingContent =
            resource.image != VK_NULL_HANDLE &&
            resource.format == format &&
            resource.extent.width == static_cast<Uint32>(texelSize.x()) &&
            resource.extent.height == static_cast<Uint32>(texelSize.y()) &&
            resource.depth == shapeInfo.depth &&
            resource.arrayLayers == shapeInfo.arrayLayers &&
            resource.viewType == shapeInfo.viewType &&
            resource.sampleCount == resolvedSampleCount &&
            resource.imageCreateFlags == imageCreateFlags &&
            resolvedSampleCount == VK_SAMPLE_COUNT_1_BIT &&
            // '<=' rather than '<': a storage-usage upgrade recreates the image with an
            // unchanged mip count, and its contents (a render target's pixels live only on the
            // GPU) still have to survive. The vkCmdCopyImage below copies min(mipLevels).
            resource.mipLevels <= backingMipLevels &&
            resource.layout != VK_IMAGE_LAYOUT_UNDEFINED;

        std::unique_ptr<TextureResource> preservedResource;
        if (preserveExistingContent) {
            preservedResource = std::make_unique<TextureResource>(Move(resource));
        } else {
            DeferResourceRelease(Move(resource));
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = imageCreateFlags;
        imageInfo.imageType = shapeInfo.imageType;
        imageInfo.extent.width = static_cast<Uint32>(texelSize.x());
        imageInfo.extent.height = static_cast<Uint32>(texelSize.y());
        imageInfo.extent.depth = shapeInfo.depth;
        imageInfo.mipLevels = backingMipLevels;
        imageInfo.arrayLayers = shapeInfo.arrayLayers;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = desiredUsage;
        imageInfo.samples = resolvedSampleCount;

        // Bound the mutability. A blindly-mutable image has to be laid out so that ANY format in
        // its compatibility class can be viewed, which costs bandwidth compression on tilers;
        // naming the exact set instead lets the driver keep it. Only safe when that set really is
        // exhaustive, so it is restricted to textures that are not image-unit bound: sampled views
        // can only ever ask for ResolveSampledImageViewFormat's output, whereas glBindImageTexture
        // may name any compatible format, which nothing here can enumerate ahead of time.
        Vector<VkFormat> viewFormats;
        VkImageFormatListCreateInfo formatListInfo{};
        if (m_imageFormatListSupported && !supportsStorageImage &&
            (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
            viewFormats.push_back(format);
            for (const SamplerNumericDomain domain : {SamplerNumericDomain::Float,
                                                      SamplerNumericDomain::SignedInteger,
                                                      SamplerNumericDomain::UnsignedInteger}) {
                const VkFormat viewFormat = ResolveSampledImageViewFormat(format, domain);
                if (viewFormat == VK_FORMAT_UNDEFINED) {
                    continue;
                }
                if (std::find(viewFormats.begin(), viewFormats.end(), viewFormat) == viewFormats.end()) {
                    viewFormats.push_back(viewFormat);
                }
            }
            formatListInfo.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
            formatListInfo.viewFormatCount = static_cast<Uint32>(viewFormats.size());
            formatListInfo.pViewFormats = viewFormats.data();
            imageInfo.pNext = &formatListInfo;
        }

        if (isMultisampleTexture || (imageInfo.flags & (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                        VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) != 0) {
            VkImageFormatProperties imageFormatProperties{};
            VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                imageInfo.flags, &imageFormatProperties);
            if (imageFormatResult != VK_SUCCESS && !isMultisampleTexture &&
                (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
                // Losing reinterpreted views only degrades the formatless-image feature for
                // this texture; failing creation would lose the texture entirely, so retry
                // as a plain immutable-format image.
                MGLOG_W_ONCE("%s: mutable image format=%d is unsupported for textureId=%d; creating "
                        "without VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (format reinterpretation "
                        "will be unavailable for it)",
                        __func__, static_cast<Int>(format), texture.GetExternalIndex());
                // Remember the verdict so later syncs of same-format textures neither retry
                // the probe nor flag-mismatch against this image and recreate it.
                m_mutableFormatUnsupported.insert(format);
                imageInfo.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                imageCreateFlags = imageInfo.flags;
                imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                    m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                    imageInfo.flags, &imageFormatProperties);
            }
            if (imageFormatResult != VK_SUCCESS && !isMultisampleTexture &&
                (imageInfo.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0) {
                // Losing 2D-array compatibility only costs per-slice framebuffer attachment for this
                // format; failing creation would lose the texture entirely. Remembered so later syncs
                // neither reprobe nor flag-mismatch against this image and recreate it.
                MGLOG_W_ONCE("%s: VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT is unsupported for format=%d "
                        "textureId=%d; creating without it (per-slice framebuffer attachment will be "
                        "unavailable for it)",
                        __func__, static_cast<Int>(format), texture.GetExternalIndex());
                m_2dArrayCompatibleUnsupported.insert(format);
                imageInfo.flags &= ~VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
                imageCreateFlags = imageInfo.flags;
                imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                    m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                    imageInfo.flags, &imageFormatProperties);
            }
            if (imageFormatResult != VK_SUCCESS ||
                (isMultisampleTexture && (imageFormatProperties.sampleCounts & resolvedSampleCount) == 0)) {
                MGLOG_D("%s: image flags=0x%x sampleCount=%d are unsupported for textureId=%d target=%s "
                        "format=%d usage=0x%x",
                        __func__, static_cast<Uint32>(imageInfo.flags), texture.GetSamples(),
                        texture.GetExternalIndex(),
                        MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                        static_cast<Int>(format), static_cast<Uint32>(imageInfo.usage));
                // The preserved image was written by GPU work that may still be in flight
                // (preserve requires layout != UNDEFINED); park it on the deferred ring
                // like every other destruction path instead of letting the unique_ptr
                // destroy it synchronously under the GPU.
                if (preservedResource) {
                    DeferResourceRelease(Move(*preservedResource));
                }
                return false;
            }
        }
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        // Soft failure like the unsupported-sample-count path above: a driver can pass the
        // vkGetPhysicalDeviceImageFormatProperties pre-check yet still refuse the creation
        // (e.g. multisampled depth on lavapipe); the texture simply stays unbacked.
        const VkResult createImageResult =
            vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr);
        if (createImageResult != VK_SUCCESS) {
            // E_ONCE, not F: the comment above says it - this is a soft failure the caller
            // recovers from, and it re-fires on every sync of every texture the driver refuses.
            MGLOG_E_ONCE("SyncTextureResource: vmaCreateImage failed (%d) textureId=%d extent=%ux%u depth=%u layers=%u "
                    "mips=%u samples=%d format=%d",
                    createImageResult, texture.GetExternalIndex(), imageInfo.extent.width, imageInfo.extent.height,
                    imageInfo.extent.depth, imageInfo.arrayLayers, imageInfo.mipLevels,
                    static_cast<Int>(imageInfo.samples), static_cast<Int>(imageInfo.format));
            resource.image = VK_NULL_HANDLE;
            resource.allocation = nullptr;
            // Same as the probe failure above: the preserved live image must go through
            // the deferred ring, never a synchronous destructor while frames that
            // reference it are still in flight.
            if (preservedResource) {
                DeferResourceRelease(Move(*preservedResource));
            }
            return false;
        }
        ++m_textureImageEpoch; // a new attachment image invalidates cached render passes

        resource.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        resource.extent = {static_cast<Uint32>(texelSize.x()), static_cast<Uint32>(texelSize.y())};
        resource.depth = shapeInfo.depth;
        resource.arrayLayers = shapeInfo.arrayLayers;
        resource.mipLevels = backingMipLevels;
        resource.perMipViews.assign(backingMipLevels, VK_NULL_HANDLE);
        resource.perMipSampledViews.assign(backingMipLevels, VK_NULL_HANDLE);
        resource.sampledBaseMipLevel = 0;
        resource.sampledLevelCount = mipLevels;
        resource.format = format;
        resource.aspect = aspect;
        resource.viewType = shapeInfo.viewType;
        resource.sampleCount = resolvedSampleCount;
        resource.imageCreateFlags = imageCreateFlags;
        resource.usageFlags = imageInfo.usage;
        resource.storageUsageResolved = markedAsStorageImage;
        resource.syncedTextureParamsVersion = 0;

        if (preservedResource) {
            // The preserve copy reads the OLD image on its own immediately-
            // submitted-and-waited command buffer; a batched upload into that
            // image still sitting in the open batch must reach the queue first
            // or the copy carries pre-upload texels forward.
            FlushPendingUploads();
            const Bool preserved = PreserveTextureContentsOnRecreate(
                m_device, m_commandPool, m_graphicsQueue, *preservedResource, resource);
            MOBILEGL_ASSERT(preserved,
                            "SyncTextureResource: failed to preserve texture contents while growing mip chain");
            DeferResourceRelease(Move(*preservedResource));
        }
        return true;
    }

    void VkTextureManager::DeferResourceRelease(TextureResource&& resource) {
        // The deferred-release queues are drained under fence/queue-idle proofs
        // that only cover SUBMITTED work; a recorded-but-unsubmitted upload
        // batch referencing this image would escape them. Push the batch onto
        // the queue first so every later proof covers it. Rare (only recreate/
        // erase of an image uploaded this very frame), so the flush is cheap.
        if (m_uploadBatchOpen && resource.image != VK_NULL_HANDLE &&
            std::find(m_uploadBatchImages.begin(), m_uploadBatchImages.end(), resource.image) !=
                m_uploadBatchImages.end()) {
            FlushPendingUploads();
        }
        if (resource.image == VK_NULL_HANDLE && resource.fullView == VK_NULL_HANDLE &&
            resource.sampledView == VK_NULL_HANDLE &&
            resource.perMipViews.empty() && resource.perMipSampledViews.empty() &&
            resource.attachmentViews.empty() && resource.alternateSampledViews.empty() &&
            resource.storageImageViews.empty()) {
            return;
        }

        if (m_deferredReleases.empty()) {
            resource.Reset();
            return;
        }

        MOBILEGL_ASSERT(m_currentFrameIndex < m_deferredReleases.size(),
                        "VkTextureManager::DeferResourceRelease invalid current frame index %u (size=%zu)",
                        m_currentFrameIndex, m_deferredReleases.size());
        m_deferredReleases[m_currentFrameIndex].push_back(Move(resource));
    }

    void VkTextureManager::CollectDeferredReleases(Uint32 frameIndex) {
        MOBILEGL_ASSERT(frameIndex < m_deferredReleases.size(),
                        "VkTextureManager::CollectDeferredReleases invalid frame index %u (size=%zu)",
                        frameIndex, m_deferredReleases.size());
        m_deferredReleases[frameIndex].clear();

        MOBILEGL_ASSERT(frameIndex < m_deferredViewReleases.size(),
                        "VkTextureManager::CollectDeferredReleases invalid deferred-view frame index %u (size=%zu)",
                        frameIndex, m_deferredViewReleases.size());
        for (const VkImageView view : m_deferredViewReleases[frameIndex]) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, view, nullptr);
            }
        }
        m_deferredViewReleases[frameIndex].clear();
    }

    void VkTextureManager::ReclaimCompletedUploads(Bool waitAll) {
        if (m_pendingUploadReclaims.empty()) {
            return;
        }

        SizeT completed = 0;
        for (; completed < m_pendingUploadReclaims.size(); ++completed) {
            PendingUploadReclaim& entry = m_pendingUploadReclaims[completed];
            if (waitAll) {
                VK_VERIFY(vkWaitForFences(m_device, 1, &entry.fence, VK_TRUE, UINT64_MAX),
                          "vkWaitForFences(texture upload reclaim)");
            } else if (vkGetFenceStatus(m_device, entry.fence) != VK_SUCCESS) {
                break;
            }
            // Recycle, don't destroy: the fence resets into the fence pool,
            // the command buffer resets into the CB pool (m_uploadCommandPool
            // carries RESET_COMMAND_BUFFER_BIT), and the staging blocks
            // return to the block pool for the next batch to bump-allocate.
            // This is where the mc_tex_stream win comes from: the per-upload
            // fence create/destroy + command-buffer alloc/free ioctl traffic
            // was the measured 41%-in-kernel cost, not the submit itself.
            if (vkResetFences(m_device, 1, &entry.fence) == VK_SUCCESS) {
                m_freeUploadFences.push_back(entry.fence);
            } else {
                vkDestroyFence(m_device, entry.fence, nullptr);
            }
            if (vkResetCommandBuffer(entry.commandBuffer, 0) == VK_SUCCESS) {
                m_freeUploadCommandBuffers.push_back(entry.commandBuffer);
            } else {
                vkFreeCommandBuffers(m_device, m_uploadCommandPool, 1, &entry.commandBuffer);
            }
            for (auto& block : entry.stagingBlocks) {
                RecycleUploadStagingBlock(Move(block));
            }
            entry.stagingBlocks.clear();
        }
        m_pendingUploadReclaims.erase(m_pendingUploadReclaims.begin(),
                                      m_pendingUploadReclaims.begin() + static_cast<std::ptrdiff_t>(completed));
    }

    void VkTextureManager::RecycleUploadStagingBlock(UploadStagingBlock&& block) {
        if (block.buffer == VK_NULL_HANDLE) {
            return;
        }
        // Bound the idle pool: a one-off giant upload (initial atlas define)
        // must not pin its staging memory forever.
        constexpr VkDeviceSize kMaxFreeUploadStagingBytes = 32u * 1024u * 1024u;
        if (m_allocator == nullptr || m_freeUploadStagingBytes + block.capacity > kMaxFreeUploadStagingBytes) {
            vmaDestroyBuffer(m_allocator, block.buffer, block.allocation);
            return;
        }
        block.cursor = 0;
        m_freeUploadStagingBytes += block.capacity;
        m_freeUploadStagingBlocks.push_back(Move(block));
    }

    VkCommandBuffer VkTextureManager::EnsureUploadBatchOpen() {
        if (m_uploadBatchOpen) {
            return m_uploadBatchCommandBuffer;
        }
        if (!m_freeUploadCommandBuffers.empty()) {
            m_uploadBatchCommandBuffer = m_freeUploadCommandBuffers.back();
            m_freeUploadCommandBuffers.pop_back();
        } else {
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = m_uploadCommandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            VK_VERIFY(vkAllocateCommandBuffers(m_device, &allocInfo, &m_uploadBatchCommandBuffer),
                      "vkAllocateCommandBuffers(texture upload batch)");
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_VERIFY(vkBeginCommandBuffer(m_uploadBatchCommandBuffer, &beginInfo),
                  "vkBeginCommandBuffer(texture upload batch)");
        m_uploadBatchOpen = true;
        return m_uploadBatchCommandBuffer;
    }

    Uint8* VkTextureManager::AcquireUploadStagingSpace(VkDeviceSize size, VkBuffer& outBuffer,
                                                       VkDeviceSize& outBaseOffset) {
        // 16 covers every uncompressed texel size in use (1..16 bytes) and the
        // bufferOffset multiple-of-4 rule; per-item offsets inside the span
        // keep the pre-batching tight packing.
        constexpr VkDeviceSize kUploadStagingAlignment = 16;
        constexpr VkDeviceSize kUploadStagingBlockSize = 1u * 1024u * 1024u;
        UploadStagingBlock* current = m_uploadBatchBlocks.empty() ? nullptr : &m_uploadBatchBlocks.back();
        VkDeviceSize alignedCursor = 0;
        if (current != nullptr) {
            alignedCursor = (current->cursor + (kUploadStagingAlignment - 1)) & ~(kUploadStagingAlignment - 1);
            if (alignedCursor + size > current->capacity) {
                current = nullptr;
            }
        }
        if (current == nullptr) {
            UploadStagingBlock block;
            for (SizeT i = 0; i < m_freeUploadStagingBlocks.size(); ++i) {
                if (m_freeUploadStagingBlocks[i].capacity >= size) {
                    block = Move(m_freeUploadStagingBlocks[i]);
                    m_freeUploadStagingBytes -= block.capacity;
                    m_freeUploadStagingBlocks.erase(m_freeUploadStagingBlocks.begin() +
                                                    static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (block.buffer == VK_NULL_HANDLE) {
                VkBufferCreateInfo bufferInfo{};
                bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bufferInfo.size = std::max(kUploadStagingBlockSize, size);
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VmaAllocationCreateInfo stagingAllocationInfo{};
                stagingAllocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                stagingAllocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
                stagingAllocationInfo.requiredFlags =
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                VmaAllocationInfo allocationResult{};
                VK_VERIFY(vmaCreateBuffer(m_allocator, &bufferInfo, &stagingAllocationInfo, &block.buffer,
                                          &block.allocation, &allocationResult),
                          "vmaCreateBuffer(texture upload staging block)");
                block.mapped = static_cast<Uint8*>(allocationResult.pMappedData);
                block.capacity = bufferInfo.size;
                MOBILEGL_ASSERT(block.mapped != nullptr,
                                "AcquireUploadStagingSpace: staging block is not persistently mapped");
            }
            block.cursor = 0;
            m_uploadBatchBlocks.push_back(Move(block));
            current = &m_uploadBatchBlocks.back();
            alignedCursor = 0;
        }
        outBuffer = current->buffer;
        outBaseOffset = alignedCursor;
        current->cursor = alignedCursor + size;
        return current->mapped + alignedCursor;
    }

    void VkTextureManager::FlushPendingUploads() {
        if (!m_uploadBatchOpen) {
            return;
        }
        VK_VERIFY(vkEndCommandBuffer(m_uploadBatchCommandBuffer), "vkEndCommandBuffer(texture upload batch)");

        VkFence uploadFence = VK_NULL_HANDLE;
        if (!m_freeUploadFences.empty()) {
            uploadFence = m_freeUploadFences.back();
            m_freeUploadFences.pop_back();
        } else {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VK_VERIFY(vkCreateFence(m_device, &fenceInfo, nullptr, &uploadFence), "vkCreateFence(texture upload)");
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_uploadBatchCommandBuffer;
        VK_VERIFY(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, uploadFence), "vkQueueSubmit(texture upload batch)");

        PendingUploadReclaim reclaim;
        reclaim.fence = uploadFence;
        reclaim.commandBuffer = m_uploadBatchCommandBuffer;
        reclaim.stagingBlocks = Move(m_uploadBatchBlocks);
        m_pendingUploadReclaims.push_back(Move(reclaim));
        m_uploadBatchCommandBuffer = VK_NULL_HANDLE;
        m_uploadBatchOpen = false;
        m_uploadBatchBlocks.clear();
        m_uploadBatchImages.clear();
        m_uploadBatchStagingBytes = 0;

        ReclaimCompletedUploads();
        // Backstop for pathological upload storms: bound in-flight staging
        // memory by blocking on the oldest batch only once the list is deep.
        constexpr SizeT kMaxPendingTextureUploads = 16;
        if (m_pendingUploadReclaims.size() > kMaxPendingTextureUploads) {
            VK_VERIFY(vkWaitForFences(m_device, 1, &m_pendingUploadReclaims.front().fence, VK_TRUE, UINT64_MAX),
                      "vkWaitForFences(texture upload backstop)");
            ReclaimCompletedUploads();
        }
    }

    void VkTextureManager::DiscardPendingUploadBatch() {
        if (!m_uploadBatchOpen) {
            return;
        }
        // The batch was never submitted, so the command buffer is in the
        // recording state, not pending - freeing it is legal.
        vkFreeCommandBuffers(m_device, m_uploadCommandPool, 1, &m_uploadBatchCommandBuffer);
        m_uploadBatchCommandBuffer = VK_NULL_HANDLE;
        m_uploadBatchOpen = false;
        for (auto& block : m_uploadBatchBlocks) {
            RecycleUploadStagingBlock(Move(block));
        }
        m_uploadBatchBlocks.clear();
        m_uploadBatchImages.clear();
        m_uploadBatchStagingBytes = 0;
    }

    void VkTextureManager::DestroyUploadPools() {
        for (auto& block : m_freeUploadStagingBlocks) {
            if (block.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(m_allocator, block.buffer, block.allocation);
            }
        }
        m_freeUploadStagingBlocks.clear();
        m_freeUploadStagingBytes = 0;
        if (!m_freeUploadCommandBuffers.empty()) {
            vkFreeCommandBuffers(m_device, m_uploadCommandPool, static_cast<Uint32>(m_freeUploadCommandBuffers.size()),
                                 m_freeUploadCommandBuffers.data());
            m_freeUploadCommandBuffers.clear();
        }
        for (const VkFence fence : m_freeUploadFences) {
            vkDestroyFence(m_device, fence, nullptr);
        }
        m_freeUploadFences.clear();
    }

    void VkTextureManager::DestroyDeferredReleases() {
        for (auto& deferredReleases : m_deferredReleases) {
            deferredReleases.clear();
        }
        m_deferredReleases.clear();

        for (auto& deferredViews : m_deferredViewReleases) {
            for (const VkImageView view : deferredViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, view, nullptr);
                }
            }
            deferredViews.clear();
        }
        m_deferredViewReleases.clear();
    }

    void VkTextureManager::DeferViewRelease(VkImageView view) {
        if (view == VK_NULL_HANDLE) {
            return;
        }

        if (m_deferredViewReleases.empty()) {
            vkDestroyImageView(m_device, view, nullptr);
            return;
        }

        MOBILEGL_ASSERT(m_currentFrameIndex < m_deferredViewReleases.size(),
                        "VkTextureManager::DeferViewRelease invalid current frame index %u (size=%zu)",
                        m_currentFrameIndex, m_deferredViewReleases.size());
        m_deferredViewReleases[m_currentFrameIndex].push_back(view);
    }

    Bool VkTextureManager::SyncTextureViews(const MG_State::GLState::ITextureObject& texture, TextureResource& resource) {
        MOBILEGL_ASSERT(resource.image != VK_NULL_HANDLE, "SyncTextureViews: image == VK_NULL_HANDLE");

        Uint32 baseMipLevel = 0;
        Uint32 levelCount = 1;
        ResolveViewMipRange(texture, resource.mipLevels, baseMipLevel, levelCount);

        const Bool needsRecreate =
            resource.fullView == VK_NULL_HANDLE ||
            resource.sampledView == VK_NULL_HANDLE ||
            resource.sampledBaseMipLevel != baseMipLevel ||
            resource.sampledLevelCount != levelCount ||
            resource.syncedTextureParamsVersion != texture.GetTextureParamsVersion();
        if (!needsRecreate) {
            return true;
        }

        if (resource.fullView != VK_NULL_HANDLE) {
            DeferViewRelease(resource.fullView);
            resource.fullView = VK_NULL_HANDLE;
        }
        if (resource.sampledView != VK_NULL_HANDLE) {
            DeferViewRelease(resource.sampledView);
            resource.sampledView = VK_NULL_HANDLE;
        }
        for (auto& sampledView : resource.perMipSampledViews) {
            if (sampledView != VK_NULL_HANDLE) {
                DeferViewRelease(sampledView);
                sampledView = VK_NULL_HANDLE;
            }
        }
        for (const auto& [_, sampledView] : resource.alternateSampledViews) {
            DeferViewRelease(sampledView);
        }
        resource.alternateSampledViews.clear();

        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkComponentMapping sampledComponents = ResolveSampledViewComponents(texture, formatInfo);
        resource.fullView = CreateImageView(resource.image, resource.format, resource.aspect, resource.viewType,
                                            baseMipLevel, levelCount, 0, resource.arrayLayers, &sampledComponents);
        if (resource.fullView == VK_NULL_HANDLE) {
            return false;
        }
        const VkImageAspectFlags sampledAspect =
            ResolveSampledImageViewAspectMask(resource.aspect, texture.GetDepthStencilTextureMode());
        resource.sampledView = CreateImageView(resource.image, resource.format, sampledAspect, resource.viewType,
                                               baseMipLevel, levelCount, 0, resource.arrayLayers, &sampledComponents);
        if (resource.sampledView == VK_NULL_HANDLE) {
            return false;
        }

        resource.sampledBaseMipLevel = baseMipLevel;
        resource.sampledLevelCount = levelCount;
        resource.syncedTextureParamsVersion = texture.GetTextureParamsVersion();
        return true;
    }

    VkImageView VkTextureManager::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                                  VkImageViewType viewType, Uint32 baseMipLevel, Uint32 levelCount,
                                                  Uint32 baseArrayLayer,
                                                  Uint32 layerCount,
                                                  const VkComponentMapping* components,
                                                  VkImageUsageFlags viewUsage) const {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.components = components != nullptr ?
            *components :
            VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                               VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.baseMipLevel = baseMipLevel;
        viewInfo.subresourceRange.levelCount = levelCount;
        viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
        viewInfo.subresourceRange.layerCount = layerCount;

        VkImageViewUsageCreateInfo usageInfo{};
        if (viewUsage != 0) {
            usageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
            usageInfo.usage = viewUsage;
            viewInfo.pNext = &usageInfo;
        }

        VkImageView view = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &view), "vkCreateImageView(texture)");
        return view;
    }

    Bool VkTextureManager::UploadDirtyMipLevels(MG_State::GLState::TextureObjectMipmap &mipmapTexture,
                                        TextureUploadTarget uploadTarget,
                                        TextureResource &outResource) {
        struct UploadItem {
            TextureUploadTarget target = TextureUploadTarget::Unknown;
            Uint32 level = 0;
            Uint32 baseArrayLayer = 0;
            SizeT uploadByteSize = 0;
            IntVec3 texelSize = {0, 0, 0};
            const void* source = nullptr;
            Vector<Uint8> expandedData;
            VkDeviceSize offset = 0;
            // Sub-region upload (a small sprite in a big atlas): only the dirty box
            // is staged and copied. texelSize keeps the LEVEL extent - the staging
            // row copy needs it for the shadow's stride. Plain color formats only;
            // the RGB-expand and depth(+stencil) conversion passes rewrite whole
            // levels and stay full-size.
            Bool subRegion = false;
            IntVec3 regionLo = {0, 0, 0};
            IntVec3 regionSize = {0, 0, 0};
            SizeT texelBytes = 0;
            // Scatter refinement of the single dirty box: when the storage's rect
            // list reports the writes' true footprint (~100 sprites whose union box
            // spans the whole atlas), each rect is staged tightly and copied with
            // its own VkBufferImageCopy in ONE vkCmdCopyBufferToImage. Empty means
            // "stage the one box above". Only set while subRegion.
            Vector<MG_State::GLState::MipmapDirtyRegion> rects;
        };

        Vector<UploadItem> uploadItems;
        Vector<TextureUploadTarget> targets;
        if (outResource.viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            targets = mipmapTexture.GetUploadTargets();
        } else {
            targets.push_back(uploadTarget);
        }
        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(mipmapTexture.GetFormat());

        VkDeviceSize stagingSize = 0;
        for (const TextureUploadTarget target : targets) {
            const Uint32 definedMipLevels = GetUploadMipLevelCount(mipmapTexture, target);
            MOBILEGL_ASSERT(definedMipLevels <= outResource.mipLevels,
                            "UploadDirtyMipLevels: defined mip level count %u exceeds backing mip level count %u for textureId=%d target=%s",
                            definedMipLevels, outResource.mipLevels, mipmapTexture.GetExternalIndex(),
                            MG_Util::ConvertTextureUploadTargetToString(target).c_str());

            for (Uint32 level = 0; level < definedMipLevels; ++level) {
                if (!mipmapTexture.IsStorageDirty(target, level)) {
                    continue;
                }

                const auto texelSize = mipmapTexture.GetMipmapTexelSize(target, level);
                const auto byteSize = mipmapTexture.GetMipmapByteSize(target, level);
                if (texelSize.x() <= 0 || texelSize.y() <= 0 || byteSize == 0) {
                    mipmapTexture.MarkStorageDirty(target, level, false);
                    continue;
                }

                const void* source = mipmapTexture.MapMipmapData(target, level);
                if (source == nullptr) {
                    MGLOG_D("%s: MapmipmapData failed at target %s level %d", __func__,
                            MG_Util::ConvertTextureUploadTargetToString(target).c_str(), level);
                    return false;
                }

                UploadItem uploadItem{};
                uploadItem.target = target;
                uploadItem.level = level;
                uploadItem.baseArrayLayer = ResolveUploadArrayLayer(target);
                uploadItem.texelSize = texelSize;
                uploadItem.source = source;
                uploadItem.offset = stagingSize;
                uploadItem.uploadByteSize = byteSize;
                if (!formatInfo.expandRgbToRgba &&
                    GetAspectMaskForFormat(outResource.format) == VK_IMAGE_ASPECT_COLOR_BIT) {
                    const auto region = mipmapTexture.GetStorageDirtyRegion(target, level);
                    const SizeT texelCount = static_cast<SizeT>(texelSize.x()) *
                                             static_cast<SizeT>(texelSize.y()) *
                                             static_cast<SizeT>(std::max(texelSize.z(), 1));
                    if (!region.Empty() && !region.CoversWholeLevel(texelSize) && texelCount > 0 &&
                        byteSize % texelCount == 0) {
                        uploadItem.subRegion = true;
                        uploadItem.regionLo = region.lo;
                        uploadItem.regionSize = {region.hi.x() - region.lo.x(), region.hi.y() - region.lo.y(),
                                                 region.hi.z() - region.lo.z()};
                        uploadItem.texelBytes = byteSize / texelCount;
                        uploadItem.uploadByteSize = static_cast<SizeT>(uploadItem.regionSize.x()) *
                                                    static_cast<SizeT>(uploadItem.regionSize.y()) *
                                                    static_cast<SizeT>(uploadItem.regionSize.z()) *
                                                    uploadItem.texelBytes;
                        // Scatter refinement: the storage only hands out its rect list
                        // when the rects' summed area is materially smaller than the
                        // union box (0 otherwise), so taking it always stages fewer
                        // bytes than the box - the very amplification this path exists
                        // to avoid paying twice.
                        MG_State::GLState::MipmapDirtyRegion
                            dirtyRects[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                        const SizeT dirtyRectCount = mipmapTexture.GetStorageDirtyRects(
                            target, level, dirtyRects, MG_State::GLState::MipmapStorage::kMaxDirtyRects);
                        if (dirtyRectCount >= 2) {
                            uploadItem.rects.assign(dirtyRects, dirtyRects + dirtyRectCount);
                            SizeT rectTexels = 0;
                            for (const auto& rect : uploadItem.rects) {
                                rectTexels += rect.TexelCount();
                            }
                            uploadItem.uploadByteSize = rectTexels * uploadItem.texelBytes;
                        }
                    }
                }
                if (formatInfo.expandRgbToRgba) {
                    const Bool expanded = ExpandRgbSourceToRgba(source, byteSize, texelSize, formatInfo,
                                                                uploadItem.expandedData);
                    MOBILEGL_ASSERT(expanded,
                                    "UploadDirtyMipLevels: failed to expand RGB textureId=%d target=%s level=%u to RGBA staging data",
                                    mipmapTexture.GetExternalIndex(),
                                    MG_Util::ConvertTextureUploadTargetToString(target).c_str(), level);
                    uploadItem.uploadByteSize = uploadItem.expandedData.size();
                }
                uploadItems.push_back(Move(uploadItem));
                if (!uploadItems.back().expandedData.empty()) {
                    uploadItems.back().source = uploadItems.back().expandedData.data();
                }
                stagingSize += static_cast<VkDeviceSize>(uploadItems.back().uploadByteSize);
            }
        }

        if (uploadItems.empty()) {
            return true;
        }

        // Combined depth-stencil images need per-aspect copies (VkBufferImageCopy aspectMask
        // must have exactly one bit set), so de-interleave the shadow's GL wire format into
        // a depth plane followed by a stencil plane per upload item.
        const VkImageAspectFlags uploadAspectMask = GetAspectMaskForFormat(outResource.format);
        const Bool isCombinedDepthStencil =
            (uploadAspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && (uploadAspectMask & VK_IMAGE_ASPECT_STENCIL_BIT);
        if (isCombinedDepthStencil) {
            const Bool srcIsD24S8 = outResource.format == VK_FORMAT_D24_UNORM_S8_UINT;
            const Bool srcIsD32FS8 = outResource.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (!srcIsD24S8 && !srcIsD32FS8) {
                MGLOG_E_ONCE("UploadDirtyMipLevels: unsupported combined depth-stencil format %d for textureId=%d",
                        static_cast<Int>(outResource.format), mipmapTexture.GetExternalIndex());
                for (const auto& item : uploadItems) {
                    mipmapTexture.MarkStorageDirty(item.target, item.level, false);
                }
                return true;
            }
            stagingSize = 0;
            for (auto& item : uploadItems) {
                const SizeT texelCount = static_cast<SizeT>(item.texelSize.x()) *
                                         static_cast<SizeT>(item.texelSize.y()) *
                                         static_cast<SizeT>(std::max(item.texelSize.z(), 1));
                const SizeT shadowTexelSize = item.uploadByteSize / std::max<SizeT>(texelCount, 1);
                MOBILEGL_ASSERT(shadowTexelSize == 4 || shadowTexelSize == 8,
                                "UploadDirtyMipLevels: unexpected depth-stencil shadow texel size %zu for textureId=%d",
                                shadowTexelSize, mipmapTexture.GetExternalIndex());
                // Depth plane as the aspect's buffer-copy format (32-bit word for
                // D24: low 24 bits; float for D32F), then one stencil byte per texel.
                Vector<Uint8> deinterleaved(texelCount * 4 + texelCount);
                Uint8* depthPlane = deinterleaved.data();
                Uint8* stencilPlane = deinterleaved.data() + texelCount * 4;
                const Uint8* shadow = static_cast<const Uint8*>(item.source);
                for (SizeT t = 0; t < texelCount; ++t) {
                    if (shadowTexelSize == 8) {
                        // GL_FLOAT_32_UNSIGNED_INT_24_8_REV: float depth, then a word
                        // with stencil in its low 8 bits.
                        float depthValue;
                        Uint32 stencilWord;
                        std::memcpy(&depthValue, shadow + t * 8, sizeof(depthValue));
                        std::memcpy(&stencilWord, shadow + t * 8 + 4, sizeof(stencilWord));
                        if (srcIsD32FS8) {
                            std::memcpy(depthPlane + t * 4, &depthValue, sizeof(depthValue));
                        } else {
                            const float clamped = std::min(std::max(depthValue, 0.0f), 1.0f);
                            const Uint32 depthWord = static_cast<Uint32>(clamped * 16777215.0f + 0.5f);
                            std::memcpy(depthPlane + t * 4, &depthWord, sizeof(depthWord));
                        }
                        stencilPlane[t] = static_cast<Uint8>(stencilWord & 0xFFu);
                    } else {
                        // GL_UNSIGNED_INT_24_8: depth in the high 24 bits, stencil low 8.
                        Uint32 packed;
                        std::memcpy(&packed, shadow + t * 4, sizeof(packed));
                        if (srcIsD24S8) {
                            const Uint32 depthWord = packed >> 8;
                            std::memcpy(depthPlane + t * 4, &depthWord, sizeof(depthWord));
                        } else {
                            const float depthValue = static_cast<float>(packed >> 8) / 16777215.0f;
                            std::memcpy(depthPlane + t * 4, &depthValue, sizeof(depthValue));
                        }
                        stencilPlane[t] = static_cast<Uint8>(packed & 0xFFu);
                    }
                }
                item.expandedData = Move(deinterleaved);
                item.source = item.expandedData.data();
                item.uploadByteSize = item.expandedData.size();
                item.offset = stagingSize;
                stagingSize += static_cast<VkDeviceSize>(item.uploadByteSize);
            }
        }

        // Pure-depth images whose canonical shadow layout differs from the image texel
        // layout (the shadow keeps a full-scale 16/32-bit unorm word or a float; the
        // image may be X8_D24 or a D32_SFLOAT fallback) convert per texel here.
        if (uploadAspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) {
            const TextureInternalFormat depthInternal = mipmapTexture.GetFormat();
            const Bool shadowIsFloat = depthInternal == TextureInternalFormat::DepthComponent32F;
            const Bool dstIsFloat = outResource.format == VK_FORMAT_D32_SFLOAT;
            const Bool dstIsD24Word = outResource.format == VK_FORMAT_X8_D24_UNORM_PACK32;
            stagingSize = 0;
            for (auto& item : uploadItems) {
                const SizeT texelCount = static_cast<SizeT>(item.texelSize.x()) *
                                         static_cast<SizeT>(item.texelSize.y()) *
                                         static_cast<SizeT>(std::max(item.texelSize.z(), 1));
                const SizeT shadowTexelSize = item.uploadByteSize / std::max<SizeT>(texelCount, 1);
                const Bool needsConversion =
                    (dstIsFloat && !shadowIsFloat) || (dstIsD24Word && shadowTexelSize == 4 && !shadowIsFloat);
                if (needsConversion) {
                    Vector<Uint8> converted(texelCount * 4);
                    const Uint8* shadow = static_cast<const Uint8*>(item.source);
                    for (SizeT t = 0; t < texelCount; ++t) {
                        Uint32 wide = 0;
                        if (shadowTexelSize == 2) {
                            Uint16 raw = 0;
                            std::memcpy(&raw, shadow + t * 2, sizeof(raw));
                            wide = (static_cast<Uint32>(raw) << 16) | raw;
                        } else {
                            std::memcpy(&wide, shadow + t * 4, sizeof(wide));
                        }
                        if (dstIsFloat) {
                            const float value = static_cast<float>(static_cast<double>(wide) / 4294967295.0);
                            std::memcpy(converted.data() + t * 4, &value, sizeof(value));
                        } else { // X8_D24: depth in the low 24 bits of a 32-bit word
                            const Uint32 word = wide >> 8;
                            std::memcpy(converted.data() + t * 4, &word, sizeof(word));
                        }
                    }
                    item.expandedData = Move(converted);
                    item.source = item.expandedData.data();
                    item.uploadByteSize = item.expandedData.size();
                }
                item.offset = stagingSize;
                stagingSize += static_cast<VkDeviceSize>(item.uploadByteSize);
            }
        }

        // Rare mid-frame hazard, kept at parity with the old per-upload
        // submits: this image already has an upload recorded in the OPEN batch
        // and has since been referenced by the frame's open recording (drawn).
        // Appending here would merge both uploads into the same pre-frame
        // submission the old code split into two; flush first so the second
        // upload lands in its own later submission, exactly like before.
        if (m_uploadBatchOpen && WasTouchedThisRecording(outResource) &&
            std::find(m_uploadBatchImages.begin(), m_uploadBatchImages.end(), outResource.image) !=
                m_uploadBatchImages.end()) {
            FlushPendingUploads();
        }
        // Bound the staging bytes a single batch can pin before its fence can
        // reclaim them.
        constexpr VkDeviceSize kMaxBatchStagingBytes = 64u * 1024u * 1024u;
        if (m_uploadBatchOpen && m_uploadBatchStagingBytes + stagingSize > kMaxBatchStagingBytes) {
            FlushPendingUploads();
        }

        VkCommandBuffer commandBuffer = EnsureUploadBatchOpen();
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceSize stagingBase = 0;
        Uint8* mapped = AcquireUploadStagingSpace(stagingSize, stagingBuffer, stagingBase);
        for (const auto& item : uploadItems) {
            Uint8* dst = mapped + item.offset;
            if (!item.subRegion) {
                std::memcpy(dst, item.source, item.uploadByteSize);
                continue;
            }
            // Tight-pack the dirty box(es): the shadow keeps whole-level rows, the
            // staging slice holds only the region (bufferRowLength stays 0). Multi-
            // rect items pack their rects back to back in list order; the copy loop
            // below recomputes the same running offsets.
            const SizeT levelRowBytes = static_cast<SizeT>(item.texelSize.x()) * item.texelBytes;
            const SizeT levelSliceBytes = static_cast<SizeT>(item.texelSize.y()) * levelRowBytes;
            const Uint8* src = static_cast<const Uint8*>(item.source);
            const auto packBox = [&](Uint8* out, const IntVec3& lo, const IntVec3& boxSize) {
                const SizeT boxRowBytes = static_cast<SizeT>(boxSize.x()) * item.texelBytes;
                for (Int z = 0; z < boxSize.z(); ++z) {
                    for (Int y = 0; y < boxSize.y(); ++y) {
                        const Uint8* srcRow = src + static_cast<SizeT>(lo.z() + z) * levelSliceBytes +
                                              static_cast<SizeT>(lo.y() + y) * levelRowBytes +
                                              static_cast<SizeT>(lo.x()) * item.texelBytes;
                        std::memcpy(out + (static_cast<SizeT>(z) * static_cast<SizeT>(boxSize.y()) + y) *
                                              boxRowBytes,
                                    srcRow, boxRowBytes);
                    }
                }
                return static_cast<SizeT>(boxSize.x()) * static_cast<SizeT>(boxSize.y()) *
                       static_cast<SizeT>(boxSize.z()) * item.texelBytes;
            };
            if (!item.rects.empty()) {
                for (const auto& rect : item.rects) {
                    dst += packBox(dst, rect.lo,
                                   IntVec3{rect.hi.x() - rect.lo.x(), rect.hi.y() - rect.lo.y(),
                                           rect.hi.z() - rect.lo.z()});
                }
                continue;
            }
            packBox(dst, item.regionLo, item.regionSize);
        }

        const VkImageAspectFlags aspectMask = GetAspectMaskForFormat(outResource.format);
        VkPipelineStageFlags uploadSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags uploadSrcAccessMask = 0;
        GetImageTransitionSourceState(outResource.layout, uploadSrcStageMask, uploadSrcAccessMask);
        Bool ok = TransitionImageLayout(commandBuffer, outResource.image,
                                        outResource.layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        uploadSrcStageMask,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        uploadSrcAccessMask,
                                        VK_ACCESS_TRANSFER_WRITE_BIT,
                                        aspectMask, 0, outResource.mipLevels);
        MOBILEGL_ASSERT(ok, "TransitionImageLayout to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL failed");

        // Array textures keep their GL "depth" in VkImage array layers, so the
        // copy must address layerCount, not imageExtent.depth (which is invalid
        // for 2D images and silently dropped every layer past the first).
        const Bool depthSelectsArrayLayer = outResource.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                                            outResource.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                                            outResource.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        for (const auto& item : uploadItems) {
            if (!item.rects.empty()) {
                // Multi-rect item: one VkBufferImageCopy per rect, all submitted in a
                // single vkCmdCopyBufferToImage. The rect list is pairwise disjoint by
                // construction, so no two copies write the same texels. Multi-rect
                // implies subRegion, which implies a plain color aspect - the combined
                // depth-stencil split below can never see one of these.
                VkBufferImageCopy rectCopies[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                Uint32 rectCopyCount = 0;
                VkDeviceSize runningOffset = item.offset;
                for (const auto& rect : item.rects) {
                    const IntVec3 rectSize = {rect.hi.x() - rect.lo.x(), rect.hi.y() - rect.lo.y(),
                                              rect.hi.z() - rect.lo.z()};
                    const Uint32 rectDepth = static_cast<Uint32>(std::max(rectSize.z(), 1));
                    VkBufferImageCopy rectCopy{};
                    rectCopy.bufferOffset = stagingBase + runningOffset;
                    rectCopy.bufferRowLength = 0;
                    rectCopy.bufferImageHeight = 0;
                    rectCopy.imageSubresource.aspectMask = aspectMask;
                    rectCopy.imageSubresource.mipLevel = item.level;
                    rectCopy.imageSubresource.baseArrayLayer = item.baseArrayLayer;
                    rectCopy.imageSubresource.layerCount = 1;
                    rectCopy.imageOffset = {rect.lo.x(), rect.lo.y(),
                                            depthSelectsArrayLayer ? 0 : rect.lo.z()};
                    rectCopy.imageExtent = {static_cast<Uint32>(rectSize.x()),
                                            static_cast<Uint32>(rectSize.y()),
                                            depthSelectsArrayLayer ? 1u : rectDepth};
                    if (depthSelectsArrayLayer) {
                        // The GL "depth" axis addresses array layers here, so a partial
                        // z-range narrows the layer span rather than the extent.
                        rectCopy.imageSubresource.baseArrayLayer =
                            item.baseArrayLayer + static_cast<Uint32>(rect.lo.z());
                        rectCopy.imageSubresource.layerCount = rectDepth;
                    }
                    rectCopies[rectCopyCount++] = rectCopy;
                    runningOffset += static_cast<VkDeviceSize>(rect.TexelCount() * item.texelBytes);
                }
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, outResource.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, rectCopyCount, rectCopies);
                continue;
            }
            const Uint32 depthOrLayers = item.texelSize.z() > 0 ? static_cast<Uint32>(item.texelSize.z()) : 1u;
            VkBufferImageCopy copy{};
            copy.bufferOffset = stagingBase + item.offset;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = aspectMask;
            copy.imageSubresource.mipLevel = item.level;
            copy.imageSubresource.baseArrayLayer = item.baseArrayLayer;
            copy.imageSubresource.layerCount = depthSelectsArrayLayer ? depthOrLayers : 1;
            copy.imageOffset = {0, 0, 0};
            copy.imageExtent = {static_cast<Uint32>(item.texelSize.x()), static_cast<Uint32>(item.texelSize.y()),
                                depthSelectsArrayLayer ? 1u : depthOrLayers};
            if (item.subRegion) {
                const Uint32 regionDepth = static_cast<Uint32>(std::max(item.regionSize.z(), 1));
                copy.imageOffset = {item.regionLo.x(), item.regionLo.y(),
                                    depthSelectsArrayLayer ? 0 : item.regionLo.z()};
                copy.imageExtent = {static_cast<Uint32>(item.regionSize.x()),
                                    static_cast<Uint32>(item.regionSize.y()),
                                    depthSelectsArrayLayer ? 1u : regionDepth};
                if (depthSelectsArrayLayer) {
                    // The GL "depth" axis addresses array layers here, so a partial
                    // z-range narrows the layer span rather than the extent.
                    copy.imageSubresource.baseArrayLayer =
                        item.baseArrayLayer + static_cast<Uint32>(item.regionLo.z());
                    copy.imageSubresource.layerCount = regionDepth;
                }
            }
            if (isCombinedDepthStencil) {
                const SizeT texelCount = static_cast<SizeT>(item.texelSize.x()) *
                                         static_cast<SizeT>(item.texelSize.y()) *
                                         static_cast<SizeT>(std::max(item.texelSize.z(), 1));
                VkBufferImageCopy depthCopy = copy;
                depthCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VkBufferImageCopy stencilCopy = copy;
                stencilCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                stencilCopy.bufferOffset = stagingBase + item.offset + static_cast<VkDeviceSize>(texelCount) * 4;
                const VkBufferImageCopy copies[2] = {depthCopy, stencilCopy};
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, outResource.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
                continue;
            }
            vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, outResource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
        }

        const VkImageLayout finalLayout = ResolveSampledReadOnlyLayout(aspectMask);
        VkImageLayout uploadLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ok = TransitionImageLayout(commandBuffer, outResource.image,
                                   uploadLayout,
                                   finalLayout,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   s_sampledReadStages,
                                   VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT,
                                   aspectMask, 0, outResource.mipLevels);
        MOBILEGL_ASSERT(ok, "TransitionImageLayout to sampled read-only layout failed");
        outResource.layout = finalLayout;

        // Ordering argument (replaces the old immediate per-texture submit):
        // this upload is RECORDED into the shared batch command buffer, which
        // FlushPendingUploads submits - with one vkQueueSubmit and one pooled
        // fence for the whole batch - strictly BEFORE any other submission on
        // the same queue whose commands could consume the image: the renderer
        // flushes at every frame-command-buffer submit (mid-frame flush,
        // readback, Present), and the texture manager flushes before the
        // preserve-on-recreate copy and before deferring an image the batch
        // references. The frame command buffer therefore still lands behind
        // the uploads on the queue, so a texture uploaded and then immediately
        // sampled in the same frame sees its data exactly as it did when each
        // upload was its own submit. No fence is waited here, for the same
        // reason as before: the batch queues behind the previous frame's
        // rendering, and a synchronous wait would drain the GPU; the staging
        // blocks/command buffer are parked on the reclaim list at flush time
        // and recycled once the batch fence signals.
        if (std::find(m_uploadBatchImages.begin(), m_uploadBatchImages.end(), outResource.image) ==
            m_uploadBatchImages.end()) {
            m_uploadBatchImages.push_back(outResource.image);
        }
        m_uploadBatchStagingBytes += stagingSize;

        if (!ok) {
            MGLOG_D("%s: texture upload cmd failed", __func__);
            return false;
        }
        for (const auto& item : uploadItems) {
            mipmapTexture.MarkStorageDirty(item.target, item.level, false);
        }
        outResource.layout = finalLayout;
        // Large batches flush right away instead of riding until the frame
        // submit: a big copy amortizes its own vkQueueSubmit, submitting it
        // early lets the GPU overlap the copy with the rest of the frame's
        // CPU recording (measurably faster than a frame-tail burst), and the
        // frame-tail burst pattern was observed to leave the GPU in a
        // latency state that taxes whatever runs next. Small uploads keep
        // accumulating, so a lightmap+sprite frame still costs one submit.
        constexpr VkDeviceSize kEagerUploadFlushBytes = 128u * 1024u;
        if (m_uploadBatchStagingBytes >= kEagerUploadFlushBytes) {
            FlushPendingUploads();
        }
        return true;
    }

    Bool VkTextureManager::CheckMipmapCompleteness(const MG_State::GLState::ITextureObject& texture,
                                                   TextureUploadTarget& outTarget,
                                                   IntVec3& outTexelSize,
                                                   SizeT& outByteSize,
                                                   Uint32& outMipLevelCount) {
        const auto* mipTexture = MG_State::GLState::AsMipmapTexture(&texture);
        if (!mipTexture) {
            MGLOG_D("%s: not TextureObjectMipmap", __func__);
            return false;
        }
        const auto& targets = texture.GetUploadTargets();
        if (targets.empty()) {
            MGLOG_D("%s: upload target empty", __func__);
            return false;
        }

        for (const auto target : targets) {
            const Uint32 mipLevelCount = GetUploadMipLevelCount(*mipTexture, target);
            if (mipLevelCount == 0) {
                MGLOG_D("%s: mipLevelCount == 0", __func__);
                continue;
            }

            // Backing VkImage allocation still uses storage mip 0 as the physical image extent.
            // GL_TEXTURE_BASE_LEVEL / MAX_LEVEL are applied later when building the sampled view.
            const auto storageBaseTexelSize = mipTexture->GetMipmapTexelSize(target, 0);
            const auto storageBaseByteSize = mipTexture->GetMipmapByteSize(target, 0);
            if (storageBaseTexelSize.x() <= 0 || storageBaseTexelSize.y() <= 0 /*|| storageBaseByteSize == 0*/) {
                continue;
            }

            outTarget = target;
            outTexelSize = storageBaseTexelSize;
            outByteSize = storageBaseByteSize;
            outMipLevelCount = mipLevelCount;
            return true;
        }
        MGLOG_D("%s: no valid target or mipmap", __func__);
        return false;
    }

    Uint32 VkTextureManager::GetUploadMipLevelCount(const MG_State::GLState::TextureObjectMipmap& texture,
                                                    TextureUploadTarget target) {
        const Uint totalLevelCount = texture.GetMipmapLevelCount();
        if (totalLevelCount == 0) {
            return 0;
        }

        Uint32 validLevelCount = 0;
        for (Uint level = 0; level < totalLevelCount; ++level) {
            const auto size = texture.GetMipmapTexelSize(target, level);
            const auto byteSize = texture.GetMipmapByteSize(target, level);
            if (size.x() <= 0 || size.y() <= 0 /*|| byteSize == 0*/) {
                break;
            }
            ++validLevelCount;
        }
        return validLevelCount;
    }

    void VkTextureManager::ResolveViewMipRange(const MG_State::GLState::ITextureObject& texture, Uint32 mipLevels,
                                               Uint32& outBaseMipLevel, Uint32& outLevelCount) {
        MOBILEGL_ASSERT(mipLevels > 0, "ResolveViewMipRange: mipLevels must be > 0");

        Uint32 definedMipLevels = mipLevels;
        if (const auto* mipTexture = MG_State::GLState::AsMipmapTexture(&texture)) {
            const auto& targets = texture.GetUploadTargets();
            for (const auto target : targets) {
                const Uint32 uploadMipLevels = GetUploadMipLevelCount(*mipTexture, target);
                if (uploadMipLevels == 0) {
                    continue;
                }
                definedMipLevels = std::min(mipLevels, uploadMipLevels);
                break;
            }
        }
        MOBILEGL_ASSERT(definedMipLevels > 0, "ResolveViewMipRange: texture has no defined mip levels");

        const auto& levelRange = texture.GetLevelRange();
        const Uint32 maxAvailableMipLevel = definedMipLevels - 1;
        const Uint32 requestedBaseMipLevel = std::min(static_cast<Uint32>(levelRange.x()), maxAvailableMipLevel);
        Uint32 requestedMaxMipLevel = std::min(static_cast<Uint32>(levelRange.y()), maxAvailableMipLevel);
        if (requestedMaxMipLevel < requestedBaseMipLevel) {
            requestedMaxMipLevel = requestedBaseMipLevel;
        }

        outBaseMipLevel = requestedBaseMipLevel;
        outLevelCount = requestedMaxMipLevel - requestedBaseMipLevel + 1;
    }

    VkImageAspectFlags VkTextureManager::GetAspectMaskForFormat(VkFormat format) {
        switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    VkImageAspectFlags VkTextureManager::ResolveSampledImageViewAspectMask(VkImageAspectFlags imageAspect,
                                                                           GLenum depthStencilTextureMode) {
        if ((imageAspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
        // A sampled view of a combined depth/stencil image may name exactly one aspect
        // (VUID-VkDescriptorImageInfo-imageView-01976), and GL_DEPTH_STENCIL_TEXTURE_MODE is
        // what picks it - the whole content of GL_ARB_stencil_texturing. Depth stays the
        // default, so nothing that never sets the mode changes shape. The texture's params
        // version moves with the mode, which is what makes the cached views be rebuilt.
        if (depthStencilTextureMode == GL_STENCIL_INDEX && (imageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        if ((imageAspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((imageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return imageAspect;
    }

    VkFormat VkTextureManager::ResolveSampledImageViewFormat(VkFormat imageFormat,
                                                              SamplerNumericDomain numericDomain) {
        // Depth/stencil images always sample through the existing depth-aspect sampledView.
        // Combined formats (D24S8, D32FS8) are multi-numeric, so vkuFormatIsSampledFloat is
        // false for them by design, yet their depth aspect reads as float in every GL depth
        // texture mode; Vulkan also forbids reinterpreting them through color-class views.
        // Integer domains keep the same view (pre-reinterpretation behavior for stencil-index
        // style access) rather than failing the draw.
        if (vkuFormatIsDepthOrStencil(imageFormat)) {
            return imageFormat;
        }
        if (imageFormat == VK_FORMAT_UNDEFINED || numericDomain == SamplerNumericDomain::Unknown ||
            FormatMatchesSamplerNumericDomain(imageFormat, numericDomain)) {
            return imageFormat;
        }
        if (!IsMutableStorageImageFormat(imageFormat)) {
            return VK_FORMAT_UNDEFINED;
        }

        // Preserve component ordering and bit widths. This selects R32_UINT for an R32_SFLOAT
        // texture sampled by a usampler rather than an arbitrary member (such as
        // R8G8B8A8_UINT) of Vulkan's broad 32-bit compatibility class.
        for (Int candidateValue = static_cast<Int>(VK_FORMAT_R4G4_UNORM_PACK8);
             candidateValue <= static_cast<Int>(VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
             ++candidateValue) {
            const VkFormat candidate = static_cast<VkFormat>(candidateValue);
            if (!IsMutableStorageImageFormat(candidate) ||
                !FormatMatchesSamplerNumericDomain(candidate, numericDomain) ||
                !HasMatchingColorComponentLayout(imageFormat, candidate) ||
                !AreSampledImageViewFormatsCompatible(imageFormat, candidate)) {
                continue;
            }

            // If an integer backing is intentionally bit-read through a float sampler, require
            // a true floating-point view. Normalized/scaled views satisfy OpTypeFloat but apply
            // an unrelated numeric conversion to those bits.
            if (numericDomain == SamplerNumericDomain::Float && !vkuFormatIsSFLOAT(candidate)) {
                continue;
            }
            return candidate;
        }
        return VK_FORMAT_UNDEFINED;
    }

    Bool VkTextureManager::AreSampledImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat) {
        if (imageFormat == viewFormat) {
            return true;
        }
        return IsMutableStorageImageFormat(imageFormat) && IsMutableStorageImageFormat(viewFormat) &&
               vkuFormatCompatibilityClass(imageFormat) == vkuFormatCompatibilityClass(viewFormat);
    }

    Bool VkTextureManager::AreStorageImageViewFormatsCompatible(VkFormat imageFormat, VkFormat viewFormat) {
        if (imageFormat == viewFormat) {
            return true;
        }
        return IsMutableStorageImageFormat(imageFormat) && IsMutableStorageImageFormat(viewFormat) &&
               vkuFormatCompatibilityClass(imageFormat) == vkuFormatCompatibilityClass(viewFormat);
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
