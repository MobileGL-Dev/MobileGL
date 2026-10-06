// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/VkTextureManager.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RenderPassGuard.h"
#include "VkTextureManager.h"

#include "ProgramFactory.h"

#include "MG_State/GLState/Core.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"
#include "MG_Util/Converters/MGToVk/TextureEnumConverter.h"
#include "MG_Util/Metrics/PipeStats.h"

#include <Config.h>
// P5f (fm): the handle-keyed texture arm's two sources - the applier's resource records
// (shape) and the server's staged-texture store (texels).
#include <MG_Pipe/PipeApply.h>
#include <MG_Backend/Record/StagedTextureStore.h>
#include "../DirectVulkan.h"
// P7 wave 2 package B3: rule I's tally for the silent exits on this file's wire arm.
#include "WireDeclineTally.h"
#if MOBILEGL_BUILD_DISAGGREGATED
// Shared images: a texture whose level 0 is a server-allocated AHardwareBuffer.
#include <MG_Remote/Server/SharedImageRegistry.h>
#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif
#endif
#include <string>
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
    // Per Magma session: each session's renderer is its own device.
    struct SampledReadStages {
        VkPipelineStageFlags value = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    };
    static SessionLocal<SampledReadStages> s_sampledReadStagesSlot;

    static VkImageUsageFlags ResolveTextureUsage(VkImageAspectFlags aspect,
                                                 VkFormatFeatureFlags formatFeatures,
                                                 Bool supportsStorageImage) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (supportsStorageImage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        // A color aspect does not imply renderability (for example, RGB9_E5).
        if ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        return usage;
    }

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
            outSrcStageMask = s_sampledReadStagesSlot->value;
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
        // A GL texture view (ARB_texture_view) is identified by the texture whose STORAGE it
        // views, not by itself. Everything this identity keys - the TextureResource, the tracked
        // image layout, the alive-object weak reference, the storage-usage marks, the per-draw
        // sync memos - is a property of the IMAGE, and a view shares that image exactly. Doing
        // the resolution here rather than at each call site is what makes it impossible to miss
        // one: a layout update posted against a view's own identity would have found no resource
        // at all, which is precisely how an attached view came back blank.
        //
        // One hop suffices and cannot recurse: glTextureView composes a view-of-a-view onto the
        // root at creation, so a storage owner is never itself a view.
        if (texture != nullptr) {
            const auto& storageOwner = texture->GetViewStorageOwner();
            if (storageOwner) {
                texture = storageOwner.get();
            }
        }
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
            outDstStageMask = s_sampledReadStagesSlot->value;
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
                                                  VkTextureManager::TextureResource& newResource
                                                  , Bool splitAspects = false
                                                  ) {
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
            if (splitAspects) {
                for (const VkImageAspectFlags aspect : {VK_IMAGE_ASPECT_COLOR_BIT,
                                                        VK_IMAGE_ASPECT_DEPTH_BIT,
                                                        VK_IMAGE_ASPECT_STENCIL_BIT}) {
                    if ((oldResource.aspect & aspect) == 0) continue;
                    copy.srcSubresource.aspectMask = aspect;
                    copy.dstSubresource.aspectMask = aspect;
                    copyRegions.push_back(copy);
                }
            } else
            copyRegions.push_back(copy);
        }

        EndActiveRenderPassOn(commandBuffer);
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

    TextureFormatInfo ResolveTextureFormatInfo(TextureInternalFormat format) {
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

        if (!source || texelSize.x() <= 0 || texelSize.y() <= 0 || !formatInfo.componentByteCount ||
            formatInfo.componentByteCount > formatInfo.alphaBytes.size()) return false;
        const SizeT depth = static_cast<SizeT>(std::max(texelSize.z(), 1));
        const SizeT width = static_cast<SizeT>(texelSize.x());
        const SizeT height = static_cast<SizeT>(texelSize.y());
        if (height > std::numeric_limits<SizeT>::max() / width ||
            depth > std::numeric_limits<SizeT>::max() / width / height) return false;
        const SizeT pixelCount = width * height * depth;
        if (pixelCount > std::numeric_limits<SizeT>::max() / (formatInfo.componentByteCount * 4) ||
            sourceByteSize != pixelCount * formatInfo.componentByteCount * 3) return false;
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

    Bool VkTextureManager::Initialize(const InitInfo& initInfo) {
        Shutdown();

        m_device = initInfo.device;
        m_physicalDevice = initInfo.physicalDevice;
        m_allocator = initInfo.allocator;
        m_commandPool = initInfo.commandPool;
        m_graphicsQueue = initInfo.graphicsQueue;
        m_imageFormatListSupported = initInfo.imageFormatListSupported;
        s_sampledReadStagesSlot->value = initInfo.sampledReadStageMask;
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

        TextureResource::s_device.Get() = m_device;
        TextureResource::s_allocator.Get() = m_allocator;

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
        // Destroy images while the device/allocator still exist, including after context death.
        m_wireTextureResources.clear();
        m_wireRenderbufferResources.clear();
        m_sharedImageUses.clear();
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
        m_viewRequestedImageFlags.erase(identity);
        m_viewRequestedFormats.erase(identity);
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

    void VkTextureManager::StampTextureRecordingUse(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }
        auto it = m_textureResources.find(MakeTextureIdentity(texture));
        if (it != m_textureResources.end()) {
            it->second.lastRecordingGeneration = m_recordingGeneration;
        }
    }

    void VkTextureManager::StampTextureRecordingWrite(MG_State::GLState::ITextureObject* texture) {
        if (texture == nullptr) {
            return;
        }
        auto it = m_textureResources.find(MakeTextureIdentity(&StorageTextureOf(*texture)));
        if (it != m_textureResources.end()) {
            StampResourceRecordingWrite(it->second);
        }
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
        StampResourceRecordingWrite(resource);

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
        EndActiveRenderPassOn(commandBuffer);
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
        const auto pruneWire = [&](auto& resources, const auto& records) {
            for (auto it = resources.begin(); it != resources.end();) {
                const Uint32 slot = static_cast<Uint32>(it->first >> 32) & 0x7fffffffu;
                const Uint32 generation = static_cast<Uint32>(it->first);
                if (slot < records.size() && records[slot].Live && records[slot].Gen == generation) {
                    ++it;
                    continue;
                }
                DeferResourceRelease(Move(it->second));
                it = resources.erase(it);
                ++m_resourceEraseEpoch;
                ++prunedCount;
            }
        };
        const auto& applier = MG_Pipe::MGPipeApplier();
        pruneWire(m_wireTextureResources, applier.TextureResources);
        pruneWire(m_wireRenderbufferResources, applier.RenderbufferResources);
        return prunedCount;
    }

    // ---------------------------------------------------------------------------------
    // P5f (fm): THE HANDLE-KEYED TEXTURE ARM
    // ---------------------------------------------------------------------------------
    //
    // SyncTexture reads the frontend ITextureObject: the target, the format, the level
    // chain, the dirty scan and - through UploadDirtyMipLevels - the CLIENT's mip shadow
    // texels. Under an active transport none of that memory is the server's to read
    // (rule E), which is exactly what Magma's texture-legacy-arm scope waived from P5c until
    // P7 retired it (SyncTexture). This arm is the same sync re-sourced: the SHAPE comes from the applier's
    // resource record (MGPipeApplier().TextureResources - the resource_create /
    // resource_respecify descriptors), the TEXELS from the server's staged-texture store
    // (P5c tx, adopted at apply time), and the WHICH-LEVELS from the record's
    // pending-upload set, consumed entry by entry exactly where the upload lands
    // (D-D5's rule: the set survives a bail, so a consumption is only ever written after
    // the bytes were actually staged).
    //
    // What this arm deliberately does NOT have, and the loud answer each gets:
    //   * buffer textures (StorageKind == Buffer): the buffer twin is P7's - declined;
    //   * sampled/storage VIEW construction: clear/blit/readback/mipmap name images and
    //     subresources, never descriptors, so no VkImageView is built here at all;
    //   * the sampled-parameter sync (MGPTextureParams): nobody on those verbs samples.
    //
    // The map is keyed by StagedTextureStore::KeyForHandle - slot AND generation - so a
    // recycled slot's new owner can never inherit its predecessor's image, and the sweep in
    // PruneDeadTextures drops an entry whose record died.

    // TryResolveTextureShapeInfo's answers derived from the wire descriptor instead of the
    // frontend object. The descriptor's Width/Height/Depth are the BASE level's GL extent and
    // ArrayLayers the layer count the emitter already normalised (MGPipeTextureExtentOf: a 1D
    // array's layers are already out of its height, a cube map reports 6).
    static Bool ResolveWireTextureShapeInfo(const MG_Pipe::MGPResourceDesc& desc, TextureShapeInfo& outShape) {
        using MG_Pipe::MGPipeResourceTarget;
        outShape = {};
        switch (static_cast<MGPipeResourceTarget>(desc.Target)) {
        case MGPipeResourceTarget::Tex1D:
            outShape.imageType = VK_IMAGE_TYPE_1D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_1D;
            return desc.Width > 0;
        case MGPipeResourceTarget::Tex2D:
        case MGPipeResourceTarget::TexRect:
        case MGPipeResourceTarget::Tex2DMS:
        case MGPipeResourceTarget::Renderbuffer:
            return desc.Width > 0 && desc.Height > 0;
        case MGPipeResourceTarget::Tex3D:
            outShape.imageType = VK_IMAGE_TYPE_3D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_3D;
            outShape.depth = desc.Depth;
            return desc.Width > 0 && desc.Height > 0 && desc.Depth > 0;
        case MGPipeResourceTarget::Tex1DArray:
            outShape.imageType = VK_IMAGE_TYPE_1D;
            outShape.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            outShape.arrayLayers = desc.ArrayLayers;
            return desc.Width > 0 && desc.ArrayLayers > 0;
        case MGPipeResourceTarget::Tex2DArray:
        case MGPipeResourceTarget::Tex2DMSArray:
            outShape.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            outShape.arrayLayers = desc.ArrayLayers;
            return desc.Width > 0 && desc.Height > 0 && desc.ArrayLayers > 0;
        case MGPipeResourceTarget::TexCube:
            outShape.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            outShape.imageFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            outShape.arrayLayers = 6;
            return desc.Width > 0 && desc.Width == desc.Height;
        case MGPipeResourceTarget::TexCubeArray:
            // glTexStorage3D hands 6n through as the depth; the emitter normalised it into
            // ArrayLayers. A non-whole cube count has no Vulkan shape - declined like every
            // other unrepresentable target.
            outShape.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            outShape.imageFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            outShape.arrayLayers = desc.ArrayLayers;
            return desc.Width > 0 && desc.Width == desc.Height && desc.ArrayLayers > 0 &&
                   (desc.ArrayLayers % 6) == 0;
        default:
            // Buffer (a record routing fault on a texture path) and TexBuffer (P7's buffer
            // twin) have no mipmapped image here.
            return false;
        }
    }

    // WireTextureTargetOf: Espryt's BufferImpl::StagedTextureTargetForPipeTarget answered
    // Magma-locally (the DirectGLES header is not this backend's to include).
    static TextureTarget WireTextureTargetOfPipeTarget(Uint8 pipeTarget) {
        using MG_Pipe::MGPipeResourceTarget;
        switch (static_cast<MGPipeResourceTarget>(pipeTarget)) {
        case MGPipeResourceTarget::Tex1D: return TextureTarget::Texture1D;
        case MGPipeResourceTarget::Tex2D: return TextureTarget::Texture2D;
        case MGPipeResourceTarget::Tex3D: return TextureTarget::Texture3D;
        case MGPipeResourceTarget::Tex1DArray: return TextureTarget::Texture1DArray;
        case MGPipeResourceTarget::Tex2DArray: return TextureTarget::Texture2DArray;
        case MGPipeResourceTarget::TexCube: return TextureTarget::TextureCubeMap;
        case MGPipeResourceTarget::TexCubeArray: return TextureTarget::TextureCubeMapArray;
        case MGPipeResourceTarget::Tex2DMS: return TextureTarget::Texture2DMultisample;
        case MGPipeResourceTarget::Tex2DMSArray: return TextureTarget::Texture2DMultisampleArray;
        case MGPipeResourceTarget::TexRect: return TextureTarget::TextureRectangle;
        case MGPipeResourceTarget::TexBuffer: return TextureTarget::TextureBuffer;
        default: return TextureTarget::Unknown;
        }
    }

    // The per-level byte transform UploadDirtyMipLevels runs between the shadow and the
    // staging buffer, factored onto values so the wire arm can run it without the frontend
    // object: RGB -> RGBA expansion, pure-depth word conversion (the image may be the
    // D32_SFLOAT fallback for X8_D24), and the combined depth-stencil de-interleave. Returns
    // false - the upload's decline - when the (format, layout) pair has no conversion here.
    static Bool ConvertWireLevelBytes(const void* source, SizeT byteSize, const IntVec3& texelSize,
                                      VkFormat imageFormat, VkImageAspectFlags aspect,
                                      TextureInternalFormat internalFormat,
                                      const TextureFormatInfo& formatInfo, Vector<Uint8>& outOwned,
                                      const void*& outSource, SizeT& outByteSize) {
        outSource = static_cast<const Uint8*>(source);
        outByteSize = byteSize;
        if (formatInfo.expandRgbToRgba) {
            if (!ExpandRgbSourceToRgba(source, byteSize, texelSize, formatInfo, outOwned)) {
                return false;
            }
            outSource = outOwned.data();
            outByteSize = outOwned.size();
            return true;
        }
        const Bool isCombinedDepthStencil =
            (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const SizeT texelCount = static_cast<SizeT>(texelSize.x()) * static_cast<SizeT>(texelSize.y()) *
                                 static_cast<SizeT>(std::max(texelSize.z(), 1));
        if (texelCount == 0) return false;
        if (isCombinedDepthStencil) {
            const Bool srcIsD24S8 = imageFormat == VK_FORMAT_D24_UNORM_S8_UINT;
            const Bool srcIsD32FS8 = imageFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (!srcIsD24S8 && !srcIsD32FS8) return false;
            const SizeT shadowTexelSize = byteSize / texelCount;
            if (shadowTexelSize != 4 && shadowTexelSize != 8) return false;
            outOwned.resize(texelCount * 4 + texelCount);
            Uint8* depthPlane = outOwned.data();
            Uint8* stencilPlane = outOwned.data() + texelCount * 4;
            const Uint8* shadow = static_cast<const Uint8*>(source);
            for (SizeT t = 0; t < texelCount; ++t) {
                if (shadowTexelSize == 8) {
                    // GL_FLOAT_32_UNSIGNED_INT_24_8_REV: float depth, then a word with the
                    // stencil in its low 8 bits.
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
            outSource = outOwned.data();
            outByteSize = outOwned.size();
            return true;
        }
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
            const Bool shadowIsFloat = internalFormat == TextureInternalFormat::DepthComponent32F;
            const Bool dstIsFloat = imageFormat == VK_FORMAT_D32_SFLOAT;
            const Bool dstIsD24Word = imageFormat == VK_FORMAT_X8_D24_UNORM_PACK32;
            const SizeT shadowTexelSize = byteSize / texelCount;
            const Bool needsConversion =
                (dstIsFloat && !shadowIsFloat) || (dstIsD24Word && shadowTexelSize == 4 && !shadowIsFloat);
            if (needsConversion) {
                outOwned.resize(texelCount * 4);
                const Uint8* shadow = static_cast<const Uint8*>(source);
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
                        std::memcpy(outOwned.data() + t * 4, &value, sizeof(value));
                    } else { // X8_D24: depth in the low 24 bits of a 32-bit word
                        const Uint32 word = wide >> 8;
                        std::memcpy(outOwned.data() + t * 4, &word, sizeof(word));
                    }
                }
                outSource = outOwned.data();
                outByteSize = outOwned.size();
            }
        }
        return true;
    }

    Bool VkTextureManager::ReadUnbackedWireLevel(MG_Pipe::MGPipeHandle handle, TextureUploadTarget target,
                                                Uint32 level, const IntVec3& extent, VkFormat& format,
                                                Vector<Uint8>& bytes) {
        const auto& records = MG_Pipe::MGPipeApplier().TextureResources;
        if (!handle.Slot || handle.Slot >= records.size()) return false;
        const auto& record = records[handle.Slot];
        if (!record.Live || record.Gen != handle.Gen || !MG_Pipe::MGPipeHandleIsNull(record.Desc.ViewOf)) return false;
        auto& store = MG_Record::ServerStagedTexture();
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(handle);
        const auto upload = static_cast<Uint16>(target);
        const auto native = m_wireTextureResources.find(key);
        if (native != m_wireTextureResources.end() && native->second.image && level < native->second.mipLevels) {
            const auto& image = native->second;
            const IntVec3 nativeExtent = ToVulkanLevelExtent(WireTextureTargetOfPipeTarget(record.Desc.Target), extent);
            const Bool volume = image.viewType == VK_IMAGE_VIEW_TYPE_3D;
            const Uint64 firstLayer = ResolveUploadArrayLayer(target);
            if (nativeExtent.x() == static_cast<Int>(std::max(image.extent.width >> level, 1u)) &&
                nativeExtent.y() == static_cast<Int>(std::max(image.extent.height >> level, 1u)) &&
                (volume ? nativeExtent.z() == static_cast<Int>(std::max(image.depth >> level, 1u))
                        : firstLayer + static_cast<Uint64>(nativeExtent.z()) <= image.arrayLayers))
                return false; // a failed sync cannot substitute CPU bytes for this native image
        }
        if (!store.IsCovered(key, upload, level) || store.IsLevelGpuDirty(key, upload, level) ||
            store.LevelExtentOrUndefined(key, upload, level) != extent) {
            const auto actual = store.LevelExtentOrUndefined(key, upload, level);
            MGLOG_E("Magma unbacked read: {%u,%u} target=%u level=%u covered=%d gpuDirty=%d extent=%dx%dx%d requested=%dx%dx%d bytes=%zu",
                    handle.Slot, handle.Gen, upload, level, store.IsCovered(key, upload, level),
                    store.IsLevelGpuDirty(key, upload, level), actual.x(), actual.y(), actual.z(),
                    extent.x(), extent.y(), extent.z(), store.LevelByteSize(key, upload, level));
            return false;
        }
        const auto logical = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
        const auto formatInfo = ResolveTextureFormatInfo(logical);
        format = formatInfo.format;
        if (format == VK_FORMAT_UNDEFINED) return false;
        const void* source = store.RequireLevelBytes(key, upload, level, "unbacked texture level readback");
        const void* converted = nullptr;
        SizeT convertedSize = 0;
        Vector<Uint8> owned;
        if (!ConvertWireLevelBytes(source, store.LevelByteSize(key, upload, level),
                ToVulkanLevelExtent(WireTextureTargetOfPipeTarget(record.Desc.Target), extent), format,
                GetAspectMaskForFormat(format), logical, formatInfo, owned, converted, convertedSize)) return false;
        const auto* begin = static_cast<const Uint8*>(converted);
        bytes.assign(begin, begin + convertedSize);
        return true;
    }

    // D-D5's consume, Magma-local: the level's entry leaves the applier's pending set ONLY
    // here, where its bytes were actually staged. (Espryt's twin is Managers.cpp's
    // ConsumePipeTextureUpload; the decode rule is the shared one.)
    static void ConsumeWireTextureUpload(MG_Pipe::MGPipeResourceRecord& record, Uint16 uploadTarget, Uint16 level) {
        for (SizeT index = 0; index < record.PendingUploads.size(); ++index) {
            const auto& pending = record.PendingUploads[index];
            if (MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget) != static_cast<Uint8>(uploadTarget) ||
                pending.Level != level) {
                continue;
            }
            record.PendingUploads[index] = std::move(record.PendingUploads.back());
            record.PendingUploads.pop_back();
            return;
        }
    }

    Bool VkTextureManager::SyncWireTextureShape(const MG_Pipe::MGPipeResourceRecord& record,
                                                TextureResource& resource, Bool requireStorage) {
        const MG_Pipe::MGPResourceDesc& desc = record.Desc;
        // A YUV shared image is converted INTO the texture's own RGBA8 storage, which the rest of
        // this function defines as for any texture; the image is only named here, and the
        // renderer converts it at its first use in each frame (WireYuvImage.inc).
        if (record.SharedImageId != resource.yuvImageId && !m_syncingYuvTextureStorage) {
            // Respecified away from (or to another) YUV image: its source goes with it, once the
            // GPU is done with it (a released resource's Reset is deferred).
            if (resource.yuvSource != nullptr) {
                TextureResource retired;
                retired.yuvSource = std::move(resource.yuvSource);
                DeferResourceRelease(Move(retired));
            }
            resource.yuvImageId = 0;
            resource.yuvOwner.reset();
            resource.yuvSource.reset();
            resource.yuvConvertedFrame = 0;
        }
#if MOBILEGL_BUILD_DISAGGREGATED
        if (record.SharedImageId != 0 && !m_syncingYuvTextureStorage) {
            const auto yuv = MG_Remote::Server::SharedImages::Find(record.SharedImageId);
            if (yuv != nullptr && MG_Remote::Server::SharedImages::FourccIsYuv(yuv->Fourcc)) {
                m_syncingYuvTextureStorage = true;
                const Bool defined = SyncWireTextureShape(record, resource, requireStorage);
                m_syncingYuvTextureStorage = false;
                if (!defined) return false;
                if (resource.yuvImageId != record.SharedImageId) {
                    resource.yuvImageId = record.SharedImageId;
                    resource.yuvOwner = yuv;
                    resource.yuvSource.reset();
                    resource.yuvConvertedFrame = 0;
                }
                return true;
            }
        }
#endif
        // Level 0 is a shared image: bind its buffer. An image that cannot be bound here leaves
        // the texture the storage its NULL-data definition asked for, below.
        if (record.SharedImageId != 0 && !m_syncingYuvTextureStorage) {
            const SharedImageBind bound = SyncWireSharedImage(record, resource, requireStorage);
            if (bound != SharedImageBind::Unresolved) return bound == SharedImageBind::Bound;
        }
        if (desc.StorageKind != static_cast<Uint8>(TextureStorageType::Mipmap)) {
            MGLOG_W_ONCE("Magma wire texture {slot=%u, gen=%u}: buffer-backed textures are not migrated; declined",
                         desc.Resource.Slot, desc.Resource.Gen);
            WireDeclineTally::Count(WireDeclineSite::ShapeBufferBacked);
            return false;
        }
        // ResolveWireTextureStorage has already followed ViewOf. Views share the storage
        // owner's image and layout; allocating a second image would break aliasing.
        if (!MG_Pipe::MGPipeHandleIsNull(desc.ViewOf)) {
            MGL_WIRE_DECLINE_AT(ShapeViewOfStorage,
                                "texture {slot=%u, gen=%u} is a view whose storage owner did not resolve",
                                desc.Resource.Slot, desc.Resource.Gen);
            return false;
        }
        if (desc.Levels == 0) {
            // A create before the first respecify: no storage, nothing to back.
            MGL_WIRE_DECLINE_AT(ShapeNoLevels, "texture {slot=%u, gen=%u} has no defined level yet",
                                desc.Resource.Slot, desc.Resource.Gen);
            return false;
        }
        TextureShapeInfo shapeInfo{};
        if (!ResolveWireTextureShapeInfo(desc, shapeInfo)) {
            MGLOG_W_ONCE("Magma wire texture {slot=%u, gen=%u}: no Vulkan shape for pipe target %u "
                         "(%ux%ux%u, %u layers); declined",
                         desc.Resource.Slot, desc.Resource.Gen, static_cast<Uint32>(desc.Target), desc.Width,
                         desc.Height, desc.Depth, static_cast<Uint32>(desc.ArrayLayers));
            WireDeclineTally::Count(WireDeclineSite::ShapeNoVulkanShape);
            return false;
        }
        const TextureInternalFormat internalFormat = static_cast<TextureInternalFormat>(desc.InternalFormat);
        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(internalFormat);
        VkFormat format = formatInfo.format;
        if (format == VK_FORMAT_UNDEFINED) {
            MGLOG_W_ONCE("Magma wire texture {slot=%u, gen=%u}: no backing VkFormat for internal format 0x%x",
                         desc.Resource.Slot, desc.Resource.Gen, desc.InternalFormat);
            WireDeclineTally::Count(WireDeclineSite::ShapeNoVkFormat);
            return false;
        }
        // X8_D24 lacks optimal-tiling support on several drivers (lavapipe included); the same
        // fallback SyncTextureResource applies, and the upload arm converts the shadow words.
        if (format == VK_FORMAT_X8_D24_UNORM_PACK32) {
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
            constexpr VkFormatFeatureFlags kDepthAttachmentAndSample =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((formatProperties.optimalTilingFeatures & kDepthAttachmentAndSample) != kDepthAttachmentAndSample) {
                format = VK_FORMAT_D32_SFLOAT;
            }
        }
        const Bool isMultisample =
            desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2DMS) ||
            desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2DMSArray) ||
            (desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Renderbuffer) && desc.Samples > 1);
        VkSampleCountFlagBits resolvedSampleCount = VK_SAMPLE_COUNT_1_BIT;
        if (isMultisample) {
            if (!TryResolveSampleCountFlagBits(static_cast<Int>(desc.Samples), resolvedSampleCount)) {
                MGLOG_W_ONCE("Magma wire texture {slot=%u, gen=%u}: unsupported sample count %u",
                             desc.Resource.Slot, desc.Resource.Gen, static_cast<Uint32>(desc.Samples));
                WireDeclineTally::Count(WireDeclineSite::ShapeSampleCount);
                return false;
            }
            // glTexStorage*Multisample(samples = 1) is legal GL, but a one-sample image cannot
            // back a sampler2DMS (VUID-RuntimeSpirv-samples-08726).
            if (resolvedSampleCount == VK_SAMPLE_COUNT_1_BIT) {
                resolvedSampleCount = VK_SAMPLE_COUNT_2_BIT;
            }
        }

        const VkImageAspectFlags aspect = GetAspectMaskForFormat(format);
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);
        // The descriptor's sticky bind mask carries the image-unit hint, so the image is born
        // with STORAGE usage and no re-mint ever pulls texels back (TextureEmit.h D-A3).
        const Bool markedAsStorageImage = requireStorage || resource.storageUsageResolved ||
            (desc.BindMask & MG_Pipe::kMGPipeBindShaderImage) != 0;
        const Bool storageImageCapable =
            !isMultisample && (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        const Bool supportsStorageImage = storageImageCapable && markedAsStorageImage;
        VkImageCreateFlags imageCreateFlags = shapeInfo.imageFlags;
        if (shapeInfo.imageType == VK_IMAGE_TYPE_3D && !isMultisample &&
            m_2dArrayCompatibleUnsupported.find(format) == m_2dArrayCompatibleUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        }
        // A view can be created after its storage was first used. Give wire color storage
        // its legal format-compatibility class from birth, so that view creation needs no
        // frontend identity lookup and cannot silently use an immutable-format VkImage.
        if (IsMutableStorageImageFormat(format) &&
            m_mutableFormatUnsupported.find(format) == m_mutableFormatUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }
        // sRGB color images attach through their UNORM twin while GL_FRAMEBUFFER_SRGB is off.
        if (ResolveSrgbAttachmentWriteFormat(format, false) != format &&
            (aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0 &&
            m_mutableFormatUnsupported.find(format) == m_mutableFormatUnsupported.end()) {
            imageCreateFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }

        VkImageUsageFlags desiredUsage =
            ResolveTextureUsage(aspect, formatProperties.optimalTilingFeatures, supportsStorageImage);
        // Multisample attachments are also transfer sources for vkCmdResolveImage.
        desiredUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // Round a multisample request up to a count the device supports for this format (GL
        // only promises "at least"), the renderbuffer path's rule.
        if (isMultisample && resolvedSampleCount != VK_SAMPLE_COUNT_1_BIT) {
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
                    for (Uint32 bit = static_cast<Uint32>(resolvedSampleCount) >> 1;
                         bit > static_cast<Uint32>(VK_SAMPLE_COUNT_1_BIT); bit >>= 1) {
                        if ((supported & bit) != 0) {
                            rounded = bit;
                            break;
                        }
                    }
                }
                if (rounded == 0 && (supported & VK_SAMPLE_COUNT_1_BIT) != 0) {
                    MGLOG_W_ONCE("Magma wire texture: multisample format %d supports no count above one on this "
                                 "device; backing it with a single sample",
                                 static_cast<Int>(format));
                    rounded = static_cast<Uint32>(VK_SAMPLE_COUNT_1_BIT);
                }
                if (rounded != 0) {
                    resolvedSampleCount = static_cast<VkSampleCountFlagBits>(rounded);
                }
            }
        }

        // The ANGLE-shaped backing rule SyncTextureResource runs: a texture that has only ever
        // defined level 0 gets a single-level backing; a second defined level recreates it ONCE
        // with the full chain. The full-chain count bounds by the image's own extent (array
        // layers are not a mip-able axis).
        // The wire retains the GL base extent: a 1D-array's Height is its layer count.
        const Uint32 imageHeight = shapeInfo.imageType == VK_IMAGE_TYPE_1D ? 1u : desc.Height;
        const IntVec3 mipExtent{static_cast<Int>(desc.Width), static_cast<Int>(imageHeight),
                                static_cast<Int>(shapeInfo.depth)};
        const Uint32 fullMipLevels = ComputeFullMipLevelCount(mipExtent);
        const Uint32 backingMipLevels =
            isMultisample ? 1u
                          : (desc.Levels > 1 ? std::min(std::max<Uint32>(desc.Levels, fullMipLevels), fullMipLevels)
                                             : 1u);

        // A shared image never stands in for allocated storage: the attach is over (or never
        // resolved), so the texture gets storage of its own, and none of the image's texels.
        const Bool compatible = resource.image != VK_NULL_HANDLE && resource.sharedImageId == 0 &&
                                resource.format == format &&
                                resource.extent.width == desc.Width && resource.extent.height == imageHeight &&
                                resource.depth == shapeInfo.depth && resource.arrayLayers == shapeInfo.arrayLayers &&
                                resource.viewType == shapeInfo.viewType &&
                                resource.sampleCount == resolvedSampleCount &&
                                resource.imageCreateFlags == imageCreateFlags &&
                                resource.usageFlags == desiredUsage && resource.mipLevels >= backingMipLevels;
        if (compatible) {
            resource.storageUsageResolved = markedAsStorageImage;
            return true;
        }

        // A named-level respecify changes ONLY that level; a BindMask update changes no
        // storage at all. Preserve every compatible old level when growing/upgrading the
        // allocation. A whole-store redefinition permits (but does not require) old texels.
        const Bool preserve = resource.image != VK_NULL_HANDLE && resource.sharedImageId == 0 &&
                              resource.format == format &&
                              resource.extent.width == desc.Width && resource.extent.height == imageHeight &&
                              resource.depth == shapeInfo.depth && resource.arrayLayers == shapeInfo.arrayLayers &&
                              resource.viewType == shapeInfo.viewType &&
                              resource.sampleCount == VK_SAMPLE_COUNT_1_BIT &&
                              resolvedSampleCount == VK_SAMPLE_COUNT_1_BIT &&
                              resource.layout != VK_IMAGE_LAYOUT_UNDEFINED;
        // Preservation submits its copy separately. Earlier draws/clears of the
        // old image must reach the queue first; waiting only for the preservation
        // fence cannot order work that is still in the renderer's open recording.
        // The descriptor caller preflights this before capturing its command buffer.
        if (preserve && pVulkanRenderer && !pVulkanRenderer->FlushWirePendingCommandsForTextureUpdate()) {
            MGL_WIRE_DECLINE_AT(ShapePreserveFlushFailed,
                                "texture {slot=%u, gen=%u}: earlier work could not be submitted before "
                                "preserving its texels across a reallocation",
                                desc.Resource.Slot, desc.Resource.Gen);
            return false;
        }
        TextureResource replacement;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = imageCreateFlags;
        imageInfo.imageType = shapeInfo.imageType;
        imageInfo.extent.width = desc.Width;
        imageInfo.extent.height = imageHeight;
        imageInfo.extent.depth = shapeInfo.depth;
        imageInfo.mipLevels = backingMipLevels;
        imageInfo.arrayLayers = shapeInfo.arrayLayers;
        imageInfo.format = format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = desiredUsage;
        imageInfo.samples = resolvedSampleCount;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (isMultisample || (imageInfo.flags & (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                                 VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) != 0) {
            VkImageFormatProperties imageFormatProperties{};
            VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage, imageInfo.flags,
                &imageFormatProperties);
            if (imageFormatResult != VK_SUCCESS && !isMultisample &&
                (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) != 0) {
                // Losing reinterpreted views only degrades the formatless-image feature;
                // failing creation would lose the texture entirely.
                m_mutableFormatUnsupported.insert(format);
                imageInfo.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                imageCreateFlags = imageInfo.flags;
                imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                    m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                    imageInfo.flags, &imageFormatProperties);
            }
            if (imageFormatResult != VK_SUCCESS && !isMultisample &&
                (imageInfo.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0) {
                m_2dArrayCompatibleUnsupported.insert(format);
                imageInfo.flags &= ~VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
                imageCreateFlags = imageInfo.flags;
                imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                    m_physicalDevice, format, imageInfo.imageType, imageInfo.tiling, imageInfo.usage,
                    imageInfo.flags, &imageFormatProperties);
            }
            if (imageFormatResult != VK_SUCCESS ||
                (isMultisample && (imageFormatProperties.sampleCounts & resolvedSampleCount) == 0)) {
                // Was an MGLOG_D, which a Release build compiles away, so on the builds that ship
                // this exit left no line - one of five Shape* exits that did (with ViewOf,
                // NoLevels, preserve-flush and preserve-copy) before B3's review round.
                MGL_WIRE_DECLINE_AT(ShapeImageFlagsUnsupported,
                                    "texture {slot=%u, gen=%u}: image flags=0x%x sampleCount=%d are unsupported "
                                    "by the device",
                                    desc.Resource.Slot, desc.Resource.Gen, static_cast<Uint32>(imageInfo.flags),
                                    static_cast<Int>(imageInfo.samples));
                return false;
            }
        }

        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        // Soft failure, SyncTextureResource's rule: a driver can pass the pre-check yet refuse
        // the creation; the texture stays unbacked and the caller declines.
        const VkResult createImageResult =
            vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &replacement.image, &replacement.allocation, nullptr);
        if (createImageResult != VK_SUCCESS) {
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: vmaCreateImage failed (%d) extent=%ux%u depth=%u "
                         "layers=%u mips=%u samples=%d format=%d",
                         desc.Resource.Slot, desc.Resource.Gen, createImageResult, imageInfo.extent.width,
                         imageInfo.extent.height, imageInfo.extent.depth, imageInfo.arrayLayers,
                         imageInfo.mipLevels, static_cast<Int>(imageInfo.samples),
                         static_cast<Int>(imageInfo.format));
            WireDeclineTally::Count(WireDeclineSite::ShapeCreateImageFailed);
            return false;
        }
        ++m_textureImageEpoch; // a new attachment image invalidates cached render passes

        replacement.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        replacement.extent = {imageInfo.extent.width, imageInfo.extent.height};
        replacement.depth = imageInfo.extent.depth;
        replacement.arrayLayers = imageInfo.arrayLayers;
        replacement.mipLevels = backingMipLevels;
        replacement.sampledBaseMipLevel = 0;
        replacement.sampledLevelCount = backingMipLevels;
        replacement.format = format;
        replacement.aspect = aspect;
        replacement.viewType = shapeInfo.viewType;
        replacement.sampleCount = resolvedSampleCount;
        replacement.imageCreateFlags = imageInfo.flags;
        replacement.usageFlags = desiredUsage;
        replacement.storageUsageResolved = markedAsStorageImage;
        if (preserve) {
            FlushPendingUploads();
            if (!PreserveTextureContentsOnRecreate(m_device, m_commandPool, m_graphicsQueue,
                                                   resource, replacement, true)) {
                MGL_WIRE_DECLINE_AT(ShapePreserveCopyFailed,
                                    "texture {slot=%u, gen=%u}: its texels could not be copied into the "
                                    "reallocated image",
                                    desc.Resource.Slot, desc.Resource.Gen);
                return false;
            }
        }
        DeferResourceRelease(Move(resource));
        std::destroy_at(&resource);
        std::construct_at(&resource, Move(replacement));
        return true;
    }

    Bool VkTextureManager::UploadPendingWireLevels(MG_Pipe::MGPipeHandle handle,
                                                   MG_Pipe::MGPipeResourceRecord& record,
                                                   TextureResource& resource) {
        if (record.PendingUploads.empty()) {
            return true;
        }
        auto& store = MG_Record::ServerStagedTexture();
        // This arm runs under a transport (the store copies) or on monolith's record arm (it
        // aliases the frontend's level shadows); a store that holds neither has nothing to upload.
        MOBILEGL_ASSERT(store.Holds(),
                        "UploadPendingWireLevels under a staged store that holds no levels: the wire arm needs one");
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(handle);
        const TextureInternalFormat internalFormat = static_cast<TextureInternalFormat>(record.Desc.InternalFormat);
        const TextureFormatInfo formatInfo = ResolveTextureFormatInfo(internalFormat);
        const TextureTarget target = WireTextureTargetOfPipeTarget(record.Desc.Target);

        struct WireUploadItem {
            Uint16 uploadTarget = 0;
            Uint16 level = 0;
            Uint32 baseArrayLayer = 0;
            IntVec3 texelSize = {0, 0, 0};
            VkOffset3D imageOffset{};
            const void* source = nullptr;
            SizeT byteSize = 0;
            Vector<Uint8> owned;
            VkDeviceSize offset = 0;
        };
        Vector<WireUploadItem> items;
        VkDeviceSize stagingSize = 0;
        Bool incomplete = false;
        for (const auto& pending : record.PendingUploads) {
            const Uint16 uploadTarget = MG_Pipe::MGPipeSubDataUploadTargetOf(pending.UploadTarget);
            const Uint16 level = pending.Level;
            if (level >= resource.mipLevels) {
                // A pending entry the current image cannot hold: the level was defined past the
                // accepted descriptor's chain. Loud, and left in the set - the next respecify
                // or generation grows into it or drops it.
                MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: pending upload for level %u exceeds the "
                             "image's %u levels; left pending",
                             handle.Slot, handle.Gen, static_cast<Uint32>(level), resource.mipLevels);
                WireDeclineTally::Count(WireDeclineSite::UploadLevelBeyondChain);
                continue;
            }
            const IntVec3 glExtent = store.LevelExtentOrUndefined(key, uploadTarget, level);
            const SizeT byteSize = store.LevelByteSize(key, uploadTarget, level);
            // The staged run IS the whole level shadow (StagedTextureStore.h's coverage rule):
            // it exists because the applier accepted this entry, so a miss is the store's own
            // Fatal{StageSnapshotTooNarrow}, not a silent zero.
            const Uint8* bytes = store.RequireLevelBytes(key, uploadTarget, level,
                                                         "VkTextureManager::UploadPendingWireLevels");
            // Vulkan geometry, like the image this stages into: a 1D array's layers move out of
            // the shadow's height into z.
            const IntVec3 texelSize = ToVulkanLevelExtent(target, glExtent);
            // GL permits independently defined mutable mip images. A Vulkan
            // image can only hold the canonical halving chain. Keep an unusual
            // level in owned staging instead of copying outside a VkImage mip.
            const Bool volume = resource.viewType == VK_IMAGE_VIEW_TYPE_3D;
            if (texelSize.x() != static_cast<Int>(std::max(resource.extent.width >> level, 1u)) ||
                texelSize.y() != static_cast<Int>(std::max(resource.extent.height >> level, 1u)) ||
                (volume && texelSize.z() != static_cast<Int>(std::max(resource.depth >> level, 1u))) ||
                (!volume && Uint64(ResolveUploadArrayLayer(static_cast<TextureUploadTarget>(uploadTarget))) +
                    static_cast<Uint64>(texelSize.z()) > resource.arrayLayers)) {
                // NOT `incomplete`: this arm reports SUCCESS while leaving the entry pending,
                // so the draw proceeds against texels that were never uploaded. Counted
                // separately for exactly that reason (B3).
                MGL_WIRE_DECLINE_AT(UploadLevelExtentMismatch,
                                    "texture {slot=%u, gen=%u} level %u: the staged extent does not match the "
                                    "image's extent for that level; left pending and NOT uploaded",
                                    handle.Slot, handle.Gen, static_cast<Uint32>(level));
                continue;
            }
            if (texelSize.x() <= 0 || texelSize.y() <= 0 || byteSize == 0) {
                MGL_WIRE_DECLINE_AT(UploadEmptyLevel, "texture {slot=%u, gen=%u} level %u is empty",
                                    handle.Slot, handle.Gen, static_cast<Uint32>(level));
                incomplete = true;
                continue;
            }
            WireUploadItem item{};
            item.uploadTarget = uploadTarget;
            item.level = level;
            item.baseArrayLayer = ResolveUploadArrayLayer(static_cast<TextureUploadTarget>(uploadTarget));
            item.texelSize = texelSize;
            if (!ConvertWireLevelBytes(bytes, byteSize, texelSize, resource.format, resource.aspect,
                                       internalFormat, formatInfo,
                                       item.owned, item.source, item.byteSize)) {
                MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: no shadow-to-image conversion for format %d; "
                             "level %u left pending",
                             handle.Slot, handle.Gen, static_cast<Int>(resource.format),
                             static_cast<Uint32>(level));
                WireDeclineTally::Count(WireDeclineSite::UploadConversion);
                incomplete = true;
                continue;
            }
            // Adoption covers the complete CPU shadow, but only the emitted regions are
            // authoritative after a GPU write. Uploading the whole run here overwrites
            // untouched texels of a prior clear/blit/mipmap with stale client bytes.
            const SizeT levelTexels = static_cast<SizeT>(texelSize.x()) * texelSize.y() *
                                      std::max(texelSize.z(), 1);
            const Bool combined = (resource.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                                  (resource.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
            if (vkuFormatIsCompressed(resource.format) || !levelTexels ||
                item.byteSize % levelTexels != 0 || item.byteSize / levelTexels == 0) {
                MGL_WIRE_DECLINE_AT(UploadTexelArithmetic,
                                    "texture {slot=%u, gen=%u} level %u: compressed, or its byte size is not a "
                                    "whole number of texels",
                                    handle.Slot, handle.Gen, static_cast<Uint32>(level));
                incomplete = true;
                continue;
            }
            const SizeT texelBytes = combined ? 4u : item.byteSize / levelTexels;
            const Bool arrayLayers = resource.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                                     resource.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                                     resource.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            const auto addRegion = [&](const MG_Pipe::MGPBox& glBox) {
                IntVec3 offset{glBox.X, glBox.Y, glBox.Z};
                IntVec3 extent{static_cast<Int>(glBox.W), static_cast<Int>(glBox.H),
                               static_cast<Int>(glBox.D)};
                if (target == TextureTarget::Texture1DArray) {
                    offset = {glBox.X, 0, glBox.Y};
                    extent = {static_cast<Int>(glBox.W), 1, static_cast<Int>(glBox.H)};
                }
                for (Int axis = 0; axis < 3; ++axis) {
                    if (offset[axis] < 0 || extent[axis] <= 0 ||
                        static_cast<Uint64>(offset[axis]) + static_cast<Uint64>(extent[axis]) >
                            static_cast<Uint64>(texelSize[axis])) {
                        MGL_WIRE_DECLINE_AT(UploadRegionOutOfBounds,
                                            "texture {slot=%u, gen=%u} level %u: a staged region lies outside "
                                            "the level",
                                            handle.Slot, handle.Gen, static_cast<Uint32>(level));
                        incomplete = true;
                        return;
                    }
                }
                WireUploadItem region{};
                region.uploadTarget = uploadTarget;
                region.level = level;
                region.baseArrayLayer = item.baseArrayLayer + (arrayLayers ? offset.z() : 0);
                region.texelSize = extent;
                region.imageOffset = {offset.x(), offset.y(), arrayLayers ? 0 : offset.z()};
                const SizeT regionTexels = static_cast<SizeT>(extent.x()) * extent.y() * extent.z();
                region.owned.resize(regionTexels * (combined ? 5u : texelBytes));
                const auto* source = static_cast<const Uint8*>(item.source);
                for (Int z = 0; z < extent.z(); ++z) {
                    for (Int y = 0; y < extent.y(); ++y) {
                        const SizeT sourceTexel =
                            (static_cast<SizeT>(offset.z() + z) * texelSize.y() + offset.y() + y) *
                                texelSize.x() + offset.x();
                        const SizeT destinationTexel =
                            (static_cast<SizeT>(z) * extent.y() + y) * extent.x();
                        std::memcpy(region.owned.data() + destinationTexel * texelBytes,
                                    source + sourceTexel * texelBytes, extent.x() * texelBytes);
                        if (combined) {
                            std::memcpy(region.owned.data() + regionTexels * 4 + destinationTexel,
                                        source + levelTexels * 4 + sourceTexel, extent.x());
                        }
                    }
                }
                region.source = region.owned.data();
                region.byteSize = region.owned.size();
                stagingSize = (stagingSize + 15u) & ~VkDeviceSize{15u};
                region.offset = stagingSize;
                stagingSize += static_cast<VkDeviceSize>(region.byteSize);
                items.push_back(Move(region));
            };
            if (pending.Regions.empty()) {
                addRegion(pending.UnionBox);
            } else {
                for (const auto& region : pending.Regions)
                    addRegion({region.X, region.Y, region.Z, region.W, region.H, region.D});
            }
        }
        // B3: the pending gauge is sampled here, on every pass through the funnel, because a
        // refusal below does NOT consume the entries - they and their level shadows stay
        // alive for the next draw to trip over (and, on a heavy-texture trace, to accumulate).
        {
            Uint64 pendingBytes = 0;
            for (const auto& pending : record.PendingUploads) {
                pendingBytes += static_cast<Uint64>(
                    store.LevelByteSize(MG_Record::StagedTextureStore::KeyForHandle(handle),
                                        pending.UploadTarget, pending.Level));
            }
            WireDeclineTally::SetPendingGauge(static_cast<Uint64>(record.PendingUploads.size()), pendingBytes);
        }
        if (incomplete) {
            MGL_WIRE_DECLINE_AT(UploadIncomplete,
                                "texture {slot=%u, gen=%u}: at least one pending level could not be staged; "
                                "every draw that samples it is declined until it can",
                                handle.Slot, handle.Gen);
            return false;
        }
        if (items.empty()) {
            return true;
        }

        // This upload will be independently submitted by FlushPendingUploads.
        // Submit older renderer work before adding the new bytes to the batch,
        // otherwise Draw(old T), TexSubImage(T), Draw(new T) can upload before
        // the first draw. A clean texture never reaches this submission boundary.
        if (pVulkanRenderer && !pVulkanRenderer->FlushWirePendingCommandsForTextureUpdate()) {
            MGL_WIRE_DECLINE_AT(UploadFlushFailed,
                                "texture {slot=%u, gen=%u}: earlier renderer work could not be submitted before "
                                "its upload batch",
                                handle.Slot, handle.Gen);
            return false;
        }

        // UploadDirtyMipLevels' batching rules, verbatim: flush an open batch that already
        // writes this image and was touched by the open recording, and bound the staging bytes
        // a batch pins.
        if (m_uploadBatchOpen && WasTouchedThisRecording(resource) &&
            std::find(m_uploadBatchImages.begin(), m_uploadBatchImages.end(), resource.image) !=
                m_uploadBatchImages.end()) {
            FlushPendingUploads();
        }
        constexpr VkDeviceSize kMaxBatchStagingBytes = 64u * 1024u * 1024u;
        if (m_uploadBatchOpen && m_uploadBatchStagingBytes + stagingSize > kMaxBatchStagingBytes) {
            FlushPendingUploads();
        }

        VkCommandBuffer commandBuffer = EnsureUploadBatchOpen();
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceSize stagingBase = 0;
        Uint8* mapped = AcquireUploadStagingSpace(stagingSize, stagingBuffer, stagingBase);
        for (const auto& item : items) {
            std::memcpy(mapped + item.offset, item.source, item.byteSize);
        }

        const VkImageAspectFlags aspectMask = resource.aspect;
        VkPipelineStageFlags uploadSrcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags uploadSrcAccessMask = 0;
        GetImageTransitionSourceState(resource.layout, uploadSrcStageMask, uploadSrcAccessMask);
        Bool ok = TransitionImageLayout(commandBuffer, resource.image, resource.layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uploadSrcStageMask,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, uploadSrcAccessMask,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, aspectMask, 0, resource.mipLevels);
        MOBILEGL_ASSERT(ok, "UploadPendingWireLevels: transition to TRANSFER_DST failed");

        const Bool isCombinedDepthStencil =
            (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 && (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const Bool depthSelectsArrayLayer = resource.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                                            resource.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                                            resource.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        for (const auto& item : items) {
            const Uint32 depthOrLayers =
                item.texelSize.z() > 0 ? static_cast<Uint32>(item.texelSize.z()) : 1u;
            VkBufferImageCopy copy{};
            copy.bufferOffset = stagingBase + item.offset;
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = aspectMask;
            copy.imageSubresource.mipLevel = item.level;
            copy.imageSubresource.baseArrayLayer = item.baseArrayLayer;
            copy.imageSubresource.layerCount = depthSelectsArrayLayer ? depthOrLayers : 1;
            copy.imageOffset = item.imageOffset;
            copy.imageExtent = {static_cast<Uint32>(item.texelSize.x()),
                                static_cast<Uint32>(item.texelSize.y()),
                                depthSelectsArrayLayer ? 1u : depthOrLayers};
            if (isCombinedDepthStencil) {
                // Per-aspect copies: VkBufferImageCopy's aspectMask must name exactly one
                // aspect; the de-interleaved run is the depth plane followed by stencil bytes.
                const SizeT texelCount = static_cast<SizeT>(item.texelSize.x()) *
                                         static_cast<SizeT>(item.texelSize.y()) *
                                         static_cast<SizeT>(std::max(item.texelSize.z(), 1));
                VkBufferImageCopy depthCopy = copy;
                depthCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VkBufferImageCopy stencilCopy = copy;
                stencilCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                stencilCopy.bufferOffset = stagingBase + item.offset + static_cast<VkDeviceSize>(texelCount) * 4;
                const VkBufferImageCopy copies[2] = {depthCopy, stencilCopy};
                EndActiveRenderPassOn(commandBuffer);
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, resource.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
            } else {
                EndActiveRenderPassOn(commandBuffer);
                vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, resource.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
        }

        const VkImageLayout finalLayout = ResolveSampledReadOnlyLayout(aspectMask);
        VkImageLayout uploadLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ok = TransitionImageLayout(commandBuffer, resource.image, uploadLayout, finalLayout,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, s_sampledReadStagesSlot->value,
                                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, aspectMask, 0,
                                   resource.mipLevels);
        MOBILEGL_ASSERT(ok, "UploadPendingWireLevels: transition to the sampled layout failed");
        resource.layout = finalLayout;

        if (std::find(m_uploadBatchImages.begin(), m_uploadBatchImages.end(), resource.image) ==
            m_uploadBatchImages.end()) {
            m_uploadBatchImages.push_back(resource.image);
        }
        m_uploadBatchStagingBytes += stagingSize;

        // The bytes are staged; NOW the entries leave the pending set (D-D5).
        for (const auto& item : items) {
            ConsumeWireTextureUpload(record, item.uploadTarget, item.level);
        }
        return true;
    }

    MG_Pipe::MGPipeHandle VkTextureManager::ResolveWireTextureStorage(
        MG_Pipe::MGPipeHandle handle, Uint32& level, Uint32& layer,
        VkFormat* viewFormat, Uint32* layerCount, Bool* outsideWindow) {
        const auto& state = MG_Pipe::MGPipeApplier();
        if (viewFormat) *viewFormat = VK_FORMAT_UNDEFINED;
        if (outsideWindow) *outsideWindow = false;
        const auto outside = [outsideWindow] {
            if (outsideWindow) *outsideWindow = true;
            return MG_Pipe::kMGPipeNullHandle;
        };
        Uint32 mappedLevel = level, mappedLayer = layer;
        Uint32 remainingLayers = ~Uint32{0};
        VkFormat outerFormat = VK_FORMAT_UNDEFINED;
        // Production views point directly to their root. The bound also makes a corrupt
        // or future multi-hop record graph terminate without ever following client data.
        for (SizeT hop = 0; hop <= state.TextureResources.size(); ++hop) {
            if (MG_Pipe::MGPipeHandleIsNull(handle) || handle.Slot >= state.TextureResources.size())
                return MG_Pipe::kMGPipeNullHandle;
            const auto& record = state.TextureResources[handle.Slot];
            if (!record.Live || record.Gen != handle.Gen) return MG_Pipe::kMGPipeNullHandle;
            if (MG_Pipe::MGPipeHandleIsNull(record.Desc.ViewOf)) {
                // Levels == 0 is a texture with no storage at all: every level is outside it.
                if (mappedLevel >= record.Desc.Levels) return outside();
                const Uint32 layers = record.Desc.Target ==
                    static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex3D)
                        ? std::max(record.Desc.Depth >> mappedLevel, 1u)
                        : std::max<Uint32>(record.Desc.ArrayLayers, 1u);
                if (mappedLayer >= layers) return outside();
                level = mappedLevel;
                layer = mappedLayer;
                if (viewFormat) *viewFormat = outerFormat;
                if (layerCount) *layerCount = std::min(remainingLayers, layers - mappedLayer);
                return handle;
            }
            const auto viewHandle = record.ViewCso;
            if (MG_Pipe::MGPipeHandleIsNull(viewHandle) || viewHandle.Slot >= state.SamplerViewCsos.size())
                return MG_Pipe::kMGPipeNullHandle;
            const auto& viewRecord = state.SamplerViewCsos[viewHandle.Slot];
            const auto& view = viewRecord.View;
            if (!viewRecord.Live || viewRecord.Gen != viewHandle.Gen || view.Texture != handle)
                return MG_Pipe::kMGPipeNullHandle;
            if (mappedLevel >= view.NumLevels || mappedLayer >= view.NumLayers) return outside();
            if (outerFormat == VK_FORMAT_UNDEFINED) {
                outerFormat = ResolveTextureFormatInfo(static_cast<TextureInternalFormat>(view.InternalFormat)).format;
                if (outerFormat == VK_FORMAT_UNDEFINED) return MG_Pipe::kMGPipeNullHandle;
            }
            remainingLayers = std::min(remainingLayers, static_cast<Uint32>(view.NumLayers) - mappedLayer);
            mappedLevel += view.MinLevel;
            mappedLayer += view.MinLayer;
            handle = record.Desc.ViewOf;
        }
        return MG_Pipe::kMGPipeNullHandle;
    }

    Bool VkTextureManager::ImportSharedImage(void* nativeBuffer, Uint32 width, Uint32 height,
                                             ImportedSharedImage& out, String& why) {
        out = ImportedSharedImage{};
#if defined(__ANDROID__)
        auto* ahb = static_cast<AHardwareBuffer*>(nativeBuffer);
        if (ahb == nullptr) {
            why = "the image has no AHardwareBuffer";
            return false;
        }
        VulkanRenderer::WireAhbImport ctx;
        if (pVulkanRenderer == nullptr || !pVulkanRenderer->GetWireAhbImport(ctx)) {
            why = "the device did not take VK_ANDROID_external_memory_android_hardware_buffer";
            return false;
        }
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(ahb, &desc);
        if (desc.width != width || desc.height != height || desc.layers != 1) {
            why = "the buffer is " + std::to_string(desc.width) + "x" + std::to_string(desc.height) + "x" +
                  std::to_string(desc.layers) + ", not " + std::to_string(width) + "x" + std::to_string(height);
            return false;
        }
        const auto getProperties =
            reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(ctx.getAhbProperties);
        VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        properties.pNext = &formatProperties;
        VkResult result = getProperties(ctx.device, ahb, &properties);
        if (result != VK_SUCCESS) {
            why = "vkGetAndroidHardwareBufferPropertiesANDROID -> " + std::to_string(static_cast<int>(result));
            return false;
        }
        // The RGBA8/RGBX8 buffers the registry allocates have a Vulkan format; an external-only
        // format would need a YCbCr conversion and is not a colour buffer this path can write.
        if (formatProperties.format == VK_FORMAT_UNDEFINED) {
            why = "the buffer has no Vulkan format (external format only)";
            return false;
        }
        // Usage is bounded by the buffer's own usage (the AHardwareBuffer usage equivalence);
        // transfers need no buffer usage bit.
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) != 0 &&
            (formatProperties.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
            usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ((desc.usage & AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT) != 0 &&
            (formatProperties.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0)
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = formatProperties.format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        result = vkCreateImage(ctx.device, &imageInfo, nullptr, &out.image);
        if (result != VK_SUCCESS) {
            out.image = VK_NULL_HANDLE;
            why = "vkCreateImage(external AHB) -> " + std::to_string(static_cast<int>(result));
            return false;
        }
        Int32 type = -1;
        for (Uint32 i = 0; i < ctx.memory.memoryTypeCount && type < 0; ++i) {
            if ((properties.memoryTypeBits & (1u << i)) != 0 &&
                (ctx.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
                type = static_cast<Int32>(i);
        }
        for (Uint32 i = 0; i < ctx.memory.memoryTypeCount && type < 0; ++i) {
            if ((properties.memoryTypeBits & (1u << i)) != 0) type = static_cast<Int32>(i);
        }
        if (type < 0) {
            why = "no memory type can import it";
            DestroyImportedSharedImage(ctx.device, out);
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = out.image;
        VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
        import.pNext = &dedicated;
        import.buffer = ahb;
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.pNext = &import;
        allocateInfo.allocationSize = properties.allocationSize;
        allocateInfo.memoryTypeIndex = static_cast<Uint32>(type);
        result = vkAllocateMemory(ctx.device, &allocateInfo, nullptr, &out.memory);
        if (result != VK_SUCCESS) {
            out.memory = VK_NULL_HANDLE;
            why = "vkAllocateMemory(import AHB) -> " + std::to_string(static_cast<int>(result));
            DestroyImportedSharedImage(ctx.device, out);
            return false;
        }
        result = vkBindImageMemory(ctx.device, out.image, out.memory, 0);
        if (result != VK_SUCCESS) {
            why = "vkBindImageMemory -> " + std::to_string(static_cast<int>(result));
            DestroyImportedSharedImage(ctx.device, out);
            return false;
        }
        out.format = formatProperties.format;
        out.usage = usage;
        out.features = formatProperties.formatFeatures;
        out.queueFamily = ctx.queueFamily;
        return true;
#else
        (void)nativeBuffer, (void)width, (void)height;
        why = "shared images are imported on Android only";
        return false;
#endif
    }

    void VkTextureManager::DestroyImportedSharedImage(VkDevice device, ImportedSharedImage& image) {
        if (image.image != VK_NULL_HANDLE) vkDestroyImage(device, image.image, nullptr);
        if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device, image.memory, nullptr);
        image = ImportedSharedImage{};
    }

#if MOBILEGL_BUILD_DISAGGREGATED
    VkTextureManager::SharedImageBind VkTextureManager::SyncWireSharedImage(
        const MG_Pipe::MGPipeResourceRecord& record, TextureResource& resource, Bool requireStorage) {
        const MG_Pipe::MGPResourceDesc& desc = record.Desc;
        const Uint64 id = record.SharedImageId;
        if (requireStorage) {
            // The buffer's usage has no storage equivalent; a reallocation would drop the image.
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: shared image %llu is bound as a shader image, "
                         "which it cannot back; declined",
                         desc.Resource.Slot, desc.Resource.Gen, static_cast<unsigned long long>(id));
            return SharedImageBind::Failed;
        }
        // Already this image: the serial moved for something else (params, the bind mask).
        if (resource.sharedImageId == id && resource.image != VK_NULL_HANDLE) return SharedImageBind::Bound;

        const auto image = MG_Remote::Server::SharedImages::Find(id);
        if (image == nullptr) {
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: shared image %llu is no longer live; the texture "
                         "keeps its own (undefined) level 0",
                         desc.Resource.Slot, desc.Resource.Gen, static_cast<unsigned long long>(id));
            return SharedImageBind::Unresolved;
        }
        const Bool flat2D = desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::Tex2D) ||
                            desc.Target == static_cast<Uint8>(MG_Pipe::MGPipeResourceTarget::TexRect);
        if (!flat2D || desc.Width != image->Width || desc.Height != image->Height ||
            !MG_Pipe::MGPipeHandleIsNull(desc.ViewOf)) {
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: pipe target %u at %ux%u cannot take shared image "
                         "%llu (%ux%u); the texture keeps its own level 0",
                         desc.Resource.Slot, desc.Resource.Gen, static_cast<Uint32>(desc.Target), desc.Width,
                         desc.Height, static_cast<unsigned long long>(id), image->Width, image->Height);
            return SharedImageBind::Unresolved;
        }
        ImportedSharedImage imported;
        String why;
        if (!ImportSharedImage(image->Native, image->Width, image->Height, imported, why)) {
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: shared image %llu not imported - %s; the texture "
                         "keeps its own level 0",
                         desc.Resource.Slot, desc.Resource.Gen, static_cast<unsigned long long>(id), why.c_str());
            return SharedImageBind::Unresolved;
        }

        // NOT ACQUIRED HERE. The writer (a client session's present, on its own device) releases
        // the image to VK_QUEUE_FAMILY_FOREIGN_EXT in GENERAL - also the layout every wire
        // descriptor samples in - once per frame it writes. Each use acquires it again when the
        // write generation moved, waiting for that write's fence on the GPU, and the frame boundary
        // releases it back (VulkanRenderer::AcquireSharedImage, WireSharedImage.inc).

        TextureResource replacement;
        replacement.image = imported.image;
        replacement.importedMemory = imported.memory;
        replacement.layout = VK_IMAGE_LAYOUT_GENERAL;
        replacement.extent = {image->Width, image->Height};
        replacement.depth = 1;
        replacement.arrayLayers = 1;
        replacement.mipLevels = 1;
        replacement.sampledBaseMipLevel = 0;
        replacement.sampledLevelCount = 1;
        replacement.format = imported.format;
        replacement.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        replacement.viewType = VK_IMAGE_VIEW_TYPE_2D;
        replacement.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        replacement.imageCreateFlags = 0;
        replacement.usageFlags = imported.usage;
        replacement.sharedImageId = id;
        replacement.sharedImageOwner = image;
        replacement.sharedImageAlphaOne = MG_Remote::Server::SharedImages::FourccIgnoresAlpha(image->Fourcc);
        ++m_textureImageEpoch; // a new attachment image invalidates cached render passes
        DeferResourceRelease(Move(resource));
        std::destroy_at(&resource);
        std::construct_at(&resource, Move(replacement));
        MGLOG_D("Magma wire texture {slot=%u, gen=%u}: level 0 is shared image %llu (%ux%u)", desc.Resource.Slot,
                desc.Resource.Gen, static_cast<unsigned long long>(id), image->Width, image->Height);
        return SharedImageBind::Bound;
    }
#else
    // P13 W5: a library without a transport allocates no shared image, so no record names one.
    VkTextureManager::SharedImageBind VkTextureManager::SyncWireSharedImage(
        const MG_Pipe::MGPipeResourceRecord&, TextureResource&, Bool) {
        return SharedImageBind::Unresolved;
    }
#endif

    VkTextureManager::TextureResource* VkTextureManager::SyncTextureResourceByHandle(
        MG_Pipe::MGPipeHandle handle, Bool renderbuffer, Bool requireStorage) {
        MOBILEGL_ASSERT(m_device != VK_NULL_HANDLE, "SyncTextureResourceByHandle: m_device == VK_NULL_HANDLE");
        if (MG_Pipe::MGPipeHandleIsNull(handle)) {
            return nullptr;
        }
        auto& applier = MG_Pipe::MGPipeApplier();
        if (!renderbuffer) {
            Uint32 level = 0, layer = 0;
            VkFormat viewFormat = VK_FORMAT_UNDEFINED;
            const auto storage = ResolveWireTextureStorage(handle, level, layer, &viewFormat);
            if (MG_Pipe::MGPipeHandleIsNull(storage)) {
                MGL_WIRE_DECLINE_AT(TexResolveStorage,
                                    "texture {slot=%u, gen=%u}: no storage owner resolved", handle.Slot,
                                    handle.Gen);
                return nullptr;
            }
            if (storage != handle) {
                auto* resource = SyncTextureResourceByHandle(storage, false, requireStorage);
                if (!resource) return nullptr;
                if (viewFormat != resource->format &&
                    ((resource->imageCreateFlags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0 ||
                     vkuFormatCompatibilityClass(viewFormat) != vkuFormatCompatibilityClass(resource->format))) {
                    MGLOG_E_ONCE("Magma wire view: format %d cannot reinterpret storage format %d",
                                 static_cast<Int>(viewFormat), static_cast<Int>(resource->format));
                    WireDeclineTally::Count(WireDeclineSite::TexViewFormat);
                    return nullptr;
                }
                return resource;
            }
        }
        auto& records = renderbuffer ? applier.RenderbufferResources : applier.TextureResources;
        auto& resources = renderbuffer ? m_wireRenderbufferResources : m_wireTextureResources;
        if (handle.Slot >= records.size()) {
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: no applier record at that slot; declined",
                         handle.Slot, handle.Gen);
            WireDeclineTally::Count(WireDeclineSite::TexNoApplierRecord);
            return nullptr;
        }
        auto& record = records[handle.Slot];
        if (!record.Live || record.Gen != handle.Gen) {
            // FramebufferRecordFor's rule: a dead or recycled slot is a loud refusal, never a
            // quiet answer from whatever the slot holds today.
            MGLOG_E_ONCE("Magma wire texture {slot=%u, gen=%u}: the applier's record is %s; declined",
                         handle.Slot, handle.Gen,
                         !record.Live ? "dead" : "of another generation");
            WireDeclineTally::Count(WireDeclineSite::TexRecordDeadOrStale);
            return nullptr;
        }
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(handle);
        auto [it, inserted] = resources.try_emplace(key);
        (void)inserted;
        TextureResource& resource = it->second;
        if (resource.image != VK_NULL_HANDLE && resource.syncedWireSerial == record.Serial &&
            record.PendingUploads.empty() && (!requireStorage || (resource.usageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0)) {
            return &resource;
        }
        if (!SyncWireTextureShape(record, resource, requireStorage)) {
            MGL_WIRE_DECLINE_AT(TexShapeSync, "texture {slot=%u, gen=%u}: no backing image; see the Shape* tally",
                                handle.Slot, handle.Gen);
            return nullptr;
        }
        if (!renderbuffer && !UploadPendingWireLevels(handle, record, resource)) {
            MGL_WIRE_DECLINE_AT(TexUploadPendingLevels,
                                "texture {slot=%u, gen=%u}: pending levels could not be uploaded; see the "
                                "Upload* tally",
                                handle.Slot, handle.Gen);
            return nullptr;
        }
        if (requireStorage && (resource.usageFlags & VK_IMAGE_USAGE_STORAGE_BIT) == 0) {
            MGL_WIRE_DECLINE_AT(TexStorageUsage,
                                "texture {slot=%u, gen=%u} is bound as a shader image but its allocation has no "
                                "STORAGE usage",
                                handle.Slot, handle.Gen);
            return nullptr;
        }
        resource.sharedImageKey = resource.sharedImageId != 0 || resource.yuvImageId != 0 ? key : 0;
        resource.syncedWireSerial = record.Serial;
        return &resource;
    }

    void VkTextureManager::MarkWireTextureGpuWritten(MG_Pipe::MGPipeHandle handle, Uint32 mipLevel,
                                                      Uint32 baseArrayLayer, Uint32 layerCount) {
        Uint32 availableLayers = 0;
        handle = ResolveWireTextureStorage(handle, mipLevel, baseArrayLayer, nullptr, &availableLayers);
        layerCount = std::min(layerCount, availableLayers);
        const auto& records = MG_Pipe::MGPipeApplier().TextureResources;
        if (MG_Pipe::MGPipeHandleIsNull(handle) || handle.Slot >= records.size()) return;
        const auto& record = records[handle.Slot];
        if (!record.Live || record.Gen != handle.Gen) return;
        using MG_Pipe::MGPipeResourceTarget;
        TextureUploadTarget target;
        switch (static_cast<MGPipeResourceTarget>(record.Desc.Target)) {
        case MGPipeResourceTarget::Tex1D: target = TextureUploadTarget::Texture1D; break;
        case MGPipeResourceTarget::Tex1DArray: target = TextureUploadTarget::Texture1DArray; break;
        case MGPipeResourceTarget::Tex3D: target = TextureUploadTarget::Texture3D; break;
        case MGPipeResourceTarget::TexRect: target = TextureUploadTarget::TextureRectangle; break;
        case MGPipeResourceTarget::Tex2DMS: target = TextureUploadTarget::Texture2DMultisample; break;
        case MGPipeResourceTarget::Tex2DMSArray: target = TextureUploadTarget::Texture2DMultisampleArray; break;
        case MGPipeResourceTarget::Tex2DArray: target = TextureUploadTarget::Texture2DArray; break;
        case MGPipeResourceTarget::TexCubeArray: target = TextureUploadTarget::CubeMapArray; break;
        case MGPipeResourceTarget::TexCube: {
            const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(handle);
            for (Uint32 face = baseArrayLayer; face < 6 && face - baseArrayLayer < layerCount; ++face)
                MG_Record::ServerStagedTexture().MarkLevelGpuDirty(key,
                    static_cast<Uint16>(TextureUploadTarget::CubeMapPositiveX) + face,
                    static_cast<Uint16>(mipLevel), true);
            return;
        }
        default: target = TextureUploadTarget::Texture2D; break;
        }
        MG_Record::ServerStagedTexture().MarkLevelGpuDirty(
            MG_Record::StagedTextureStore::KeyForHandle(handle),
            static_cast<Uint16>(target), static_cast<Uint16>(mipLevel), true);
    }

    Bool VkTextureManager::GrowWireTextureMipChain(MG_Pipe::MGPipeHandle handle, Uint32 requiredMipLevels,
                                                   VkCommandBuffer commandBuffer) {
        if (MG_Pipe::MGPipeHandleIsNull(handle)) {
            return false;
        }
        const Uint64 key = MG_Record::StagedTextureStore::KeyForHandle(handle);
        auto it = m_wireTextureResources.find(key);
        if (it == m_wireTextureResources.end() || it->second.image == VK_NULL_HANDLE) {
            return false;
        }
        TextureResource& resource = it->second;
        if (resource.mipLevels >= requiredMipLevels) {
            return true;
        }
        if (resource.sampleCount != VK_SAMPLE_COUNT_1_BIT) {
            return false; // a multisample image has no mip chain to grow
        }

        // The caller (GenerateMipmap's record arm) flushed every pending submission, so the old
        // image is GPU-idle; the carry-over copy is recorded straight into the frame's command
        // buffer and the old image is parked on the deferred-release ring behind it.
        // Keep the live allocation intact until allocation of its replacement succeeds.
        TextureResource& old = resource;
        TextureResource grown;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = old.imageCreateFlags;
        imageInfo.imageType = old.viewType == VK_IMAGE_VIEW_TYPE_1D || old.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                  ? VK_IMAGE_TYPE_1D
                                  : (old.viewType == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);
        imageInfo.extent = {old.extent.width, old.extent.height, old.depth};
        imageInfo.mipLevels = requiredMipLevels;
        imageInfo.arrayLayers = old.arrayLayers;
        imageInfo.format = old.format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = old.usageFlags;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const VkResult createImageResult =
            vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &grown.image, &grown.allocation, nullptr);
        if (createImageResult != VK_SUCCESS) {
            MGLOG_E_ONCE("GrowWireTextureMipChain: vmaCreateImage failed (%d)", createImageResult);
            return false;
        }
        ++m_textureImageEpoch;

        const VkImageAspectFlags aspect = old.aspect;
        const VkImageLayout oldLayout = old.layout;
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags srcAccessMask = 0;
        GetImageTransitionSourceState(oldLayout, srcStageMask, srcAccessMask);
        Bool ok = TransitionImageLayout(commandBuffer, old.image, old.layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcStageMask,
                                        VK_PIPELINE_STAGE_TRANSFER_BIT, srcAccessMask,
                                        VK_ACCESS_TRANSFER_READ_BIT, aspect, 0, old.mipLevels);
        MOBILEGL_ASSERT(ok, "GrowWireTextureMipChain: source transition failed");
        VkImageLayout grownLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ok = TransitionImageLayout(commandBuffer, grown.image, grownLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, aspect, 0,
                                   requiredMipLevels);
        MOBILEGL_ASSERT(ok, "GrowWireTextureMipChain: destination transition failed");

        for (Uint32 level = 0; level < old.mipLevels; ++level) {
            VkImageCopy copy{};
            copy.srcSubresource = {aspect, level, 0, old.arrayLayers};
            copy.dstSubresource = {aspect, level, 0, old.arrayLayers};
            copy.srcOffset = {0, 0, 0};
            copy.dstOffset = {0, 0, 0};
            copy.extent = {std::max(old.extent.width >> level, 1u), std::max(old.extent.height >> level, 1u),
                           std::max(old.depth >> level, 1u)};
            for (const VkImageAspectFlags copyAspect : {VK_IMAGE_ASPECT_COLOR_BIT,
                                                        VK_IMAGE_ASPECT_DEPTH_BIT,
                                                        VK_IMAGE_ASPECT_STENCIL_BIT}) {
                if ((aspect & copyAspect) == 0) continue;
                copy.srcSubresource.aspectMask = copyAspect;
                copy.dstSubresource.aspectMask = copyAspect;
                EndActiveRenderPassOn(commandBuffer);
                vkCmdCopyImage(commandBuffer, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, grown.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
        }

        VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags dstAccessMask = 0;
        GetImageTransitionDestinationState(oldLayout, dstStageMask, dstAccessMask);
        ok = TransitionImageLayout(commandBuffer, grown.image, grownLayout, oldLayout,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, dstStageMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   dstAccessMask, aspect, 0, requiredMipLevels);
        MOBILEGL_ASSERT(ok, "GrowWireTextureMipChain: restore transition failed");

        grown.layout = oldLayout;
        grown.extent = old.extent;
        grown.depth = old.depth;
        grown.arrayLayers = old.arrayLayers;
        grown.mipLevels = requiredMipLevels;
        grown.sampledBaseMipLevel = old.sampledBaseMipLevel;
        grown.sampledLevelCount = requiredMipLevels;
        grown.format = old.format;
        grown.aspect = old.aspect;
        grown.viewType = old.viewType;
        grown.sampleCount = old.sampleCount;
        grown.imageCreateFlags = old.imageCreateFlags;
        grown.usageFlags = old.usageFlags;
        grown.storageUsageResolved = old.storageUsageResolved;
        grown.syncedWireSerial = old.syncedWireSerial;

        DeferResourceRelease(Move(resource));
        std::destroy_at(&resource);
        std::construct_at(&resource, Move(grown));
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
            resource.storageImageViews.empty()
            && resource.yuvSource == nullptr
        ) {
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

    Bool VkTextureManager::WireUploadsAreIdle() {
        // An open batch still owns references even before it has a fence. Poll
        // submitted batches without waiting; reclamation is deferred if any lives.
        if (m_uploadBatchOpen) return false;
        ReclaimCompletedUploads();
        return m_pendingUploadReclaims.empty();
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

    MG_State::GLState::ITextureObject& VkTextureManager::StorageTextureOf(
        MG_State::GLState::ITextureObject& texture) {
        const auto& storageOwner = texture.GetViewStorageOwner();
        return storageOwner ? *storageOwner : texture;
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

} // namespace MobileGL::MG_Backend::DirectVulkan
