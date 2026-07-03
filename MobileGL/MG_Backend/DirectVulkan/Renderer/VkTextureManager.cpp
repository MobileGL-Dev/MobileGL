// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "VkTextureManager.h"

#include "MG_State/GLState/Core.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace MobileGL::MG_Backend::DirectVulkan {
    static constexpr VkPipelineStageFlags kGraphicsSampledReadStages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;

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

    static Bool IsMultisampleTextureUploadTarget(TextureUploadTarget target) {
        return target == TextureUploadTarget::Texture2DMultisample ||
               target == TextureUploadTarget::ProxyTexture2DMultisample ||
               target == TextureUploadTarget::Texture2DMultisampleArray ||
               target == TextureUploadTarget::ProxyTexture2DMultisampleArray;
    }

    static Bool TryResolveSampleCountFlagBits(Int requestedSamples, VkSampleCountFlagBits& outSampleCount) {
        switch (requestedSamples) {
        case 1:
            outSampleCount = VK_SAMPLE_COUNT_1_BIT;
            return true;
        case 2:
            outSampleCount = VK_SAMPLE_COUNT_2_BIT;
            return true;
        case 4:
            outSampleCount = VK_SAMPLE_COUNT_4_BIT;
            return true;
        case 8:
            outSampleCount = VK_SAMPLE_COUNT_8_BIT;
            return true;
        case 16:
            outSampleCount = VK_SAMPLE_COUNT_16_BIT;
            return true;
        case 32:
            outSampleCount = VK_SAMPLE_COUNT_32_BIT;
            return true;
        case 64:
            outSampleCount = VK_SAMPLE_COUNT_64_BIT;
            return true;
        default:
            return false;
        }
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
            outSrcStageMask = kGraphicsSampledReadStages;
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
            outDstStageMask = kGraphicsSampledReadStages;
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
            0, VK_ACCESS_TRANSFER_WRITE_BIT, newResource.aspect, 0, newResource.mipLevels,
            newResource.arrayLayers);
        MOBILEGL_ASSERT(ok, "PreserveTextureContentsOnRecreate: failed to prepare destination image");

        VkImageLayout srcTrackedLayout = oldResource.layout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(srcTrackedLayout, srcStageMask, srcAccessMask);
        ok = VkTextureManager::TransitionImageLayout(
            commandBuffer, oldResource.image, srcTrackedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            srcStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
            srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT, oldResource.aspect, 0, preservedMipLevels,
            oldResource.arrayLayers);
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
            VK_ACCESS_TRANSFER_WRITE_BIT, dstAccessMask, newResource.aspect, 0, newResource.mipLevels,
            newResource.arrayLayers);
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
            return {VK_FORMAT_R8G8B8A8_UNORM, true, 1, {0xFF, 0x00, 0x00, 0x00}};
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

    static Bool ShouldDumpTextureUploadStats() {
        static const Bool enabled = [] {
            const char* value = std::getenv("MOBILEGL_TEXTURE_UPLOAD_STATS");
            return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    static void DumpTextureSyncStats(Int textureId, TextureInternalFormat format, TextureUploadTarget uploadTarget,
                                     Uint32 mipLevelCount, const IntVec3& texelSize, SizeT byteSize,
                                     Bool hasDirtyMipLevel) {
        if (!ShouldDumpTextureUploadStats()) {
            return;
        }

        std::fprintf(stderr,
                     "MOBILEGL_TEXTURE_SYNC_STATS texture=%d format=%s target=%s mips=%u size=%dx%dx%d "
                     "bytes=%zu dirty=%d\n",
                     textureId,
                     MG_Util::ConvertTextureInternalFormatToString(format).c_str(),
                     MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                     mipLevelCount,
                     texelSize.x(),
                     texelSize.y(),
                     texelSize.z(),
                     byteSize,
                     hasDirtyMipLevel ? 1 : 0);
    }

    static void DumpTextureUploadStats(Int textureId, TextureUploadTarget target, Uint32 level,
                                       const IntVec3& texelSize, const void* data, SizeT byteSize, Uint32 channels) {
        if (channels == 0 && texelSize.x() > 0 && texelSize.y() > 0) {
            const SizeT depth = static_cast<SizeT>(std::max(texelSize.z(), 1));
            const SizeT pixelCount = static_cast<SizeT>(texelSize.x()) * static_cast<SizeT>(texelSize.y()) * depth;
            if (pixelCount > 0 && byteSize % pixelCount == 0) {
                channels = static_cast<Uint32>(std::min<SizeT>(byteSize / pixelCount, 4));
            }
        }
        if (!ShouldDumpTextureUploadStats() || data == nullptr || byteSize == 0 || channels == 0) {
            return;
        }

        const auto* bytes = static_cast<const Uint8*>(data);
        Uint8 minValue = 255;
        Uint8 maxValue = 0;
        SizeT nonZero = 0;
        Uint64 sum = 0;
        Uint64 neighborDiff = 0;
        SizeT neighborCount = 0;

        for (SizeT i = 0; i < byteSize; ++i) {
            minValue = std::min(minValue, bytes[i]);
            maxValue = std::max(maxValue, bytes[i]);
            nonZero += bytes[i] != 0 ? 1 : 0;
            sum += bytes[i];
        }

        const SizeT width = static_cast<SizeT>(std::max(texelSize.x(), 0));
        const SizeT height = static_cast<SizeT>(std::max(texelSize.y(), 0));
        const SizeT pixelStride = channels;
        if (width > 1 && height > 0 && byteSize >= width * height * pixelStride) {
            for (SizeT y = 0; y < height; ++y) {
                const SizeT row = y * width * pixelStride;
                for (SizeT x = 1; x < width; ++x) {
                    const SizeT prev = row + (x - 1) * pixelStride;
                    const SizeT cur = row + x * pixelStride;
                    for (SizeT c = 0; c < std::min<SizeT>(channels, 3); ++c) {
                        neighborDiff += static_cast<Uint64>(
                            std::abs(static_cast<Int>(bytes[cur + c]) - static_cast<Int>(bytes[prev + c])));
                        ++neighborCount;
                    }
                }
            }
        }

        const double avg = byteSize > 0 ? static_cast<double>(sum) / static_cast<double>(byteSize) : 0.0;
        const double avgNeighborDiff =
            neighborCount > 0 ? static_cast<double>(neighborDiff) / static_cast<double>(neighborCount) : 0.0;
        std::fprintf(stderr,
                     "MOBILEGL_TEXTURE_UPLOAD_STATS texture=%d target=%s level=%u size=%dx%dx%d bytes=%zu "
                     "channels=%u min=%u max=%u avg=%.2f nonzero=%zu neighborDiff=%.2f first=%u,%u,%u,%u\n",
                     textureId,
                     MG_Util::ConvertTextureUploadTargetToString(target).c_str(),
                     level,
                     texelSize.x(),
                     texelSize.y(),
                     texelSize.z(),
                     byteSize,
                     channels,
                     static_cast<Uint>(minValue),
                     static_cast<Uint>(maxValue),
                     avg,
                     nonZero,
                     avgNeighborDiff,
                     byteSize > 0 ? static_cast<Uint>(bytes[0]) : 0u,
                     byteSize > 1 ? static_cast<Uint>(bytes[1]) : 0u,
                     byteSize > 2 ? static_cast<Uint>(bytes[2]) : 0u,
                     byteSize > 3 ? static_cast<Uint>(bytes[3]) : 0u);
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

        return true;
    }

    void VkTextureManager::Shutdown() {
        DestroyDeferredReleases();
        m_textureResources.clear();
        m_aliveObjects.clear();

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
    }

    void VkTextureManager::EraseTrackedTexture(const TextureIdentity& identity) {
        auto resourceIt = m_textureResources.find(identity);
        if (resourceIt != m_textureResources.end()) {
            DeferResourceRelease(Move(resourceIt->second));
            m_textureResources.erase(resourceIt);
        }
        m_aliveObjects.erase(identity);
    }

    void VkTextureManager::PruneStaleTextureAliases(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }

        Vector<TextureIdentity> staleAliases;
        staleAliases.reserve(m_aliveObjects.size());
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

    VkTextureManager::TextureResource* VkTextureManager::SyncTextureAndGetDescriptor(MG_State::GLState::ITextureObject& texture) {
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "SyncTextureAndGetDescriptor: m_device == VK_NULL_HANDLE");

        const TextureIdentity identity = MakeTextureIdentity(&texture);
        auto aliveIt = m_aliveObjects.find(identity);
        if (aliveIt != m_aliveObjects.end() && aliveIt->second.expired()) {
            EraseTrackedTexture(aliveIt->first);
        }

        const auto& liveTexture = MG_State::pGLContext->GetTextureObject(texture.GetExternalIndex());
        if (liveTexture && liveTexture.get() == &texture) {
            m_aliveObjects[identity] = WeakPtr<MG_State::GLState::ITextureObject>(liveTexture);
            PruneStaleTextureAliases(&texture);
        }

        auto it = m_textureResources.find(identity);
        if (it == m_textureResources.end()) {
            TextureResource initial{};
            auto [insertIt, _] = m_textureResources.emplace(identity, Move(initial));
            it = insertIt;
        }

        if (!SyncTexture(texture, it->second)) {
            MGLOG_D("%s: Syncing texture %d failed", __func__, texture.GetExternalIndex());
            return nullptr;
        }

        return &(it->second);
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
        if (layerCount == 0 || baseArrayLayer >= resource->arrayLayers ||
            baseArrayLayer + layerCount > resource->arrayLayers) {
            MGLOG_D("%s: invalid layer span [%u, %u) for textureId=%d arrayLayers=%u",
                    __func__, baseArrayLayer, baseArrayLayer + layerCount, texture.GetExternalIndex(),
                    resource->arrayLayers);
            return VK_NULL_HANDLE;
        }

        if (baseArrayLayer == 0 && layerCount == resource->arrayLayers && viewType == resource->viewType) {
            return GetOrCreateViewAtMipLevel(texture, mipLevel);
        }

        const TextureResource::AttachmentViewKey key{
            .mipLevel = mipLevel,
            .baseArrayLayer = baseArrayLayer,
            .layerCount = layerCount,
            .viewType = viewType,
        };
        auto it = resource->attachmentViews.find(key);
        if (it == resource->attachmentViews.end()) {
            it = resource->attachmentViews.emplace(key, VK_NULL_HANDLE).first;
        }
        VkImageView& attachmentView = it->second;
        if (attachmentView != VK_NULL_HANDLE) {
            return attachmentView;
        }

        attachmentView = CreateImageView(resource->image, resource->format, resource->aspect, viewType,
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
        const VkImageAspectFlags sampledAspect = ResolveSampledImageViewAspectMask(resource->aspect);
        perMipSampledView = CreateImageView(resource->image, resource->format, sampledAspect, resource->viewType,
                                            mipLevel, 1, 0, resource->arrayLayers, &sampledComponents);
        if (perMipSampledView == VK_NULL_HANDLE) {
            MGLOG_D("%s: CreateImageView failed for textureId=%d mipLevel=%u", __func__, texture.GetExternalIndex(),
                    mipLevel);
            return VK_NULL_HANDLE;
        }

        return perMipSampledView;
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
                    resource.aspect, 0, writtenMipLevel, resource.arrayLayers);
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
                    resource.aspect, upperBaseMipLevel, resource.mipLevels - upperBaseMipLevel,
                    resource.arrayLayers);
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
            MGLOG_W("TransitionTextureForSampling: textureId=%d is still in VK_IMAGE_LAYOUT_UNDEFINED before sampling",
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
                                              kGraphicsSampledReadStages, srcAccessMask,
                                              VK_ACCESS_SHADER_READ_BIT, resource->aspect, 0, resource->mipLevels,
                                              resource->arrayLayers);
        MOBILEGL_ASSERT(ok, "TransitionTextureForSampling: transition failed for textureId=%d", texture.GetExternalIndex());
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
                                              resource->aspect, 0, resource->mipLevels, resource->arrayLayers);
        MOBILEGL_ASSERT(ok, "TransitionTextureForStorageImage: transition failed for textureId=%d",
                        texture.GetExternalIndex());
        return ok;
    }

    Bool VkTextureManager::TransitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                                                 VkImageLayout& trackedLayout, VkImageLayout newLayout,
                                                 VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                                                 VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
                                                 VkImageAspectFlags aspectMask, Uint32 baseMipLevel, Uint32 levelCount,
                                                 Uint32 layerCount) {
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
        barrier.subresourceRange.layerCount = layerCount;
        vkCmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        trackedLayout = newLayout;
        return true;
    }

    SizeT VkTextureManager::CollectGarbage() {
        m_gcCounter++;
        if (m_gcCounter != 0) {
            return 0;
        }

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
        return expiredTextures.size();
    }

    Bool VkTextureManager::SyncTexture(MG_State::GLState::ITextureObject &texture,
                                       TextureResource &outResource) {
        TextureUploadTarget uploadTarget = TextureUploadTarget::Unknown;
        IntVec3 texelSize{0, 0, 0};
        SizeT byteSize = 0;
        Uint32 mipLevelCount = 0;
        if (!CheckMipmapCompleteness(texture, uploadTarget, texelSize, byteSize, mipLevelCount)) {
            MGLOG_D("%s: mipmap not complete", __func__);
            return false;
        }

        auto* mipTexture = dynamic_cast<MG_State::GLState::TextureObjectMipmap*>(&texture);
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
        DumpTextureSyncStats(texture.GetExternalIndex(), texture.GetFormat(), uploadTarget, mipLevelCount,
                             texelSize, byteSize, hasDirtyMipLevel);
        if (!hasDirtyMipLevel) {
            return true;
        }

        if (!UploadDirtyMipLevels(*mipTexture, uploadTarget, outResource)) {
            MGLOG_D("%s: UploadDirtyMipLevels failed", __func__);
            return false;
        }
        return true;
    }

    Bool VkTextureManager::SyncTextureResource(const MG_State::GLState::ITextureObject &texture,
                                               TextureUploadTarget uploadTarget,
                                               const IntVec3 &texelSize, SizeT byteSize, Uint32 mipLevels,
                                               TextureResource &resource) {
        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkFormat format = formatInfo.format;
        if (format == VK_FORMAT_UNDEFINED) {
            MGLOG_D("%s: format == VK_FORMAT_UNDEFINED", __func__);
            return false;
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
        const Uint32 backingMipLevels =
            isMultisampleTexture ? 1u : std::max(mipLevels, ComputeFullMipLevelCount(texelSize));
        TextureShapeInfo shapeInfo{};
        const Bool supportedShape = TryResolveTextureShapeInfo(texture, uploadTarget, texelSize, shapeInfo);
        MOBILEGL_ASSERT(supportedShape,
                        "SyncTextureResource: unsupported uploadTarget=%s textureTarget=%s textureId=%d size=(%d,%d,%d) "
                        "mipLevels=%u vkViewType=%d",
                        MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                        MG_Util::ConvertTextureTargetToString(texture.GetTarget()).c_str(),
                        texture.GetExternalIndex(), texelSize.x(), texelSize.y(), texelSize.z(), mipLevels,
                        static_cast<Int>(MG_Util::ConvertTextureUploadTargetToVkEnum(uploadTarget)));
        if (!supportedShape) {
            MGLOG_D("%s: not Texture2D, unsupported", __func__);
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

        const Bool compatible = resource.image != VK_NULL_HANDLE && resource.format == format &&
                                resource.extent.width == static_cast<Uint32>(texelSize.x()) &&
                                resource.extent.height == static_cast<Uint32>(texelSize.y()) &&
                                resource.depth == shapeInfo.depth &&
                                resource.arrayLayers == shapeInfo.arrayLayers &&
                                resource.viewType == shapeInfo.viewType &&
                                resource.sampleCount == resolvedSampleCount &&
                                resource.mipLevels == backingMipLevels;
        if (compatible) {
            if (resource.perMipViews.size() != backingMipLevels) {
                resource.perMipViews.resize(backingMipLevels, VK_NULL_HANDLE);
            }
            if (resource.perMipSampledViews.size() != backingMipLevels) {
                resource.perMipSampledViews.resize(backingMipLevels, VK_NULL_HANDLE);
            }
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
            resolvedSampleCount == VK_SAMPLE_COUNT_1_BIT &&
            resource.mipLevels < backingMipLevels &&
            resource.layout != VK_IMAGE_LAYOUT_UNDEFINED;

        std::unique_ptr<TextureResource> preservedResource;
        if (preserveExistingContent) {
            preservedResource = std::make_unique<TextureResource>(Move(resource));
        } else {
            DeferResourceRelease(Move(resource));
        }

        auto aspect = GetAspectMaskForFormat(format);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = shapeInfo.imageFlags;
    imageInfo.imageType = shapeInfo.imageType;
        imageInfo.extent.width = static_cast<Uint32>(texelSize.x());
        imageInfo.extent.height = static_cast<Uint32>(texelSize.y());
    imageInfo.extent.depth = shapeInfo.depth;
        imageInfo.mipLevels = backingMipLevels;
    imageInfo.arrayLayers = shapeInfo.arrayLayers;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
        const Bool supportsStorageImage =
            !isMultisampleTexture &&
            (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                          (supportsStorageImage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
                          ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
                          (((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) ?
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                               0);
        if (!isMultisampleTexture) {
            imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        imageInfo.samples = resolvedSampleCount;
        if (isMultisampleTexture) {
            VkImageFormatProperties imageFormatProperties{};
            const VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                imageInfo.flags, &imageFormatProperties);
            if (imageFormatResult != VK_SUCCESS ||
                (imageFormatProperties.sampleCounts & resolvedSampleCount) == 0) {
                MGLOG_D("%s: sampleCount=%d is unsupported for textureId=%d target=%s format=%d usage=0x%x",
                        __func__, texture.GetSamples(), texture.GetExternalIndex(),
                        MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                        static_cast<Int>(format), static_cast<Uint32>(imageInfo.usage));
                return false;
            }
        }
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_VERIFY(vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr),
                  "vmaCreateImage(texture)");

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
        resource.syncedTextureParamsVersion = 0;

        if (preservedResource) {
            const Bool preserved = PreserveTextureContentsOnRecreate(
                m_device, m_commandPool, m_graphicsQueue, *preservedResource, resource);
            MOBILEGL_ASSERT(preserved,
                            "SyncTextureResource: failed to preserve texture contents while growing mip chain");
            DeferResourceRelease(Move(*preservedResource));
        }
        return true;
    }

    void VkTextureManager::DeferResourceRelease(TextureResource&& resource) {
        if (resource.image == VK_NULL_HANDLE && resource.fullView == VK_NULL_HANDLE &&
            resource.sampledView == VK_NULL_HANDLE &&
            resource.perMipViews.empty() && resource.perMipSampledViews.empty()) {
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

        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(texture.GetFormat());
        const VkComponentMapping sampledComponents = ResolveSampledViewComponents(texture, formatInfo);
        resource.fullView = CreateImageView(resource.image, resource.format, resource.aspect, resource.viewType,
                                            baseMipLevel, levelCount, 0, resource.arrayLayers, &sampledComponents);
        if (resource.fullView == VK_NULL_HANDLE) {
            return false;
        }
        const VkImageAspectFlags sampledAspect = ResolveSampledImageViewAspectMask(resource.aspect);
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
                                                  const VkComponentMapping* components) const {
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
                DumpTextureUploadStats(mipmapTexture.GetExternalIndex(), uploadItems.back().target,
                                       uploadItems.back().level, uploadItems.back().texelSize,
                                       uploadItems.back().source, uploadItems.back().uploadByteSize,
                                       formatInfo.expandRgbToRgba ? 4u : 0u);
                stagingSize += static_cast<VkDeviceSize>(uploadItems.back().uploadByteSize);
            }
        }

        if (uploadItems.empty()) {
            return true;
        }

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = stagingSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAllocationInfo{};
        stagingAllocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        stagingAllocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        stagingAllocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VK_VERIFY(vmaCreateBuffer(m_allocator, &bufferInfo, &stagingAllocationInfo, &stagingBuffer, &stagingAllocation, nullptr),
                  "vmaCreateBuffer(staging texture)");

        void* mapped = nullptr;
        VK_VERIFY(vmaMapMemory(m_allocator, stagingAllocation, &mapped), "vmaMapMemory(staging texture)");
        for (const auto& item : uploadItems) {
            std::memcpy(static_cast<Uint8*>(mapped) + item.offset, item.source, item.uploadByteSize);
        }
        vmaUnmapMemory(m_allocator, stagingAllocation);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer), "vkAllocateCommandBuffers(texture)");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_VERIFY(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(texture)");

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
                                        aspectMask, 0, outResource.mipLevels, outResource.arrayLayers);
        MOBILEGL_ASSERT(ok, "TransitionImageLayout to VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL failed");

        for (const auto& item : uploadItems) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = item.offset;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = aspectMask;
            copy.imageSubresource.mipLevel = item.level;
            copy.imageSubresource.baseArrayLayer = item.baseArrayLayer;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = {0, 0, 0};
            copy.imageExtent = {static_cast<Uint32>(item.texelSize.x()), static_cast<Uint32>(item.texelSize.y()),
                                item.texelSize.z() > 0 ? static_cast<Uint32>(item.texelSize.z()) : 1u};
            vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, outResource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
        }

        const VkImageLayout finalLayout = ResolveSampledReadOnlyLayout(aspectMask);
        VkImageLayout uploadLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ok = TransitionImageLayout(commandBuffer, outResource.image,
                                   uploadLayout,
                                   finalLayout,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   kGraphicsSampledReadStages,
                                   VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT,
                                   aspectMask, 0, outResource.mipLevels, outResource.arrayLayers);
        MOBILEGL_ASSERT(ok, "TransitionImageLayout to sampled read-only layout failed");
        outResource.layout = finalLayout;

        VK_VERIFY(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(texture)");

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence uploadFence = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFence(m_device, &fenceInfo, nullptr, &uploadFence), "vkCreateFence(texture upload)");

        VK_VERIFY(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, uploadFence), "vkQueueSubmit(texture)");
        VK_VERIFY(vkWaitForFences(m_device, 1, &uploadFence, VK_TRUE, UINT64_MAX), "vkWaitForFences(texture upload)");
        vkDestroyFence(m_device, uploadFence, nullptr);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);

        vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);

        if (!ok) {
            MGLOG_D("%s: texture upload cmd failed", __func__);
            return false;
        }
        for (const auto& item : uploadItems) {
            mipmapTexture.MarkStorageDirty(item.target, item.level, false);
        }
        outResource.layout = finalLayout;
        return true;
    }

    Bool VkTextureManager::CheckMipmapCompleteness(const MG_State::GLState::ITextureObject& texture,
                                                   TextureUploadTarget& outTarget,
                                                   IntVec3& outTexelSize,
                                                   SizeT& outByteSize,
                                                   Uint32& outMipLevelCount) {
        const auto* mipTexture = dynamic_cast<const MG_State::GLState::TextureObjectMipmap*>(&texture);
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
        if (const auto* mipTexture = dynamic_cast<const MG_State::GLState::TextureObjectMipmap*>(&texture)) {
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

    VkImageAspectFlags VkTextureManager::ResolveSampledImageViewAspectMask(VkImageAspectFlags imageAspect) {
        if ((imageAspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
        if ((imageAspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((imageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return imageAspect;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
