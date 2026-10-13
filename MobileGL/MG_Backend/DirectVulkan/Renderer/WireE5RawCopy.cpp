// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireE5RawCopy.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// See WireE5RawCopy.h. The probe's plumbing is WireDepthResolveProbe.cpp's: one throwaway command
// buffer, a bounded fence wait that leaks rather than idle-waits a hung GPU, teardown on every
// other path.

#include "WireE5RawCopy.h"

#include <MG_Util/Debug/Log.h>

#include <cstring>
#include <vulkan/vulkan_core.h>

namespace MobileGL::MG_Backend::DirectVulkan {
#include "WireE5RawCopySpirv.h"

    namespace {
        // The shaders' push-constant block: ivec4 sourceOffset, ivec4 extent.
        struct RegionPush {
            Int32 sourceOffset[4];
            Int32 extent[4];
        };
        constexpr Uint32 kGroupSize = 8;
    } // namespace

    VkResult CreateWireE5RawCopyKit(VkDevice device, WireE5RawCopyKit& kit) {
        const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes;
            info.pCode = code;
            return vkCreateShaderModule(device, &info, nullptr, &module);
        };
        VkResult result = shader(kWireE5RawCopySpirv, sizeof(kWireE5RawCopySpirv), kit.arrayModule);
        if (result != VK_SUCCESS) return result;
        result = shader(kWireE5RawCopy3DSpirv, sizeof(kWireE5RawCopy3DSpirv), kit.volumeModule);
        if (result != VK_SUCCESS) return result;
        const VkDescriptorSetLayoutBinding bindings[2]{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 2;
        layout.pBindings = bindings;
        result = vkCreateDescriptorSetLayout(device, &layout, nullptr, &kit.descriptorLayout);
        if (result != VK_SUCCESS) return result;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RegionPush)};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &kit.descriptorLayout;
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &push;
        result = vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &kit.pipelineLayout);
        if (result != VK_SUCCESS) return result;
        const auto pipeline = [&](VkShaderModule module, VkPipeline& out) {
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = module;
            info.stage.pName = "main";
            info.layout = kit.pipelineLayout;
            return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out);
        };
        result = pipeline(kit.arrayModule, kit.arrayPipeline);
        if (result != VK_SUCCESS) return result;
        result = pipeline(kit.volumeModule, kit.volumePipeline);
        if (result != VK_SUCCESS) return result;
        // texelFetch ignores the filter; the sampler exists only because a GLSL usampler is a
        // COMBINED image sampler. An integer format must not be linearly filtered anyway.
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.maxLod = 0.0f;
        return vkCreateSampler(device, &sampler, nullptr, &kit.nearest);
    }

    void DestroyWireE5RawCopyKit(VkDevice device, WireE5RawCopyKit& kit) {
        vkDestroySampler(device, kit.nearest, nullptr);
        vkDestroyPipeline(device, kit.arrayPipeline, nullptr);
        vkDestroyPipeline(device, kit.volumePipeline, nullptr);
        vkDestroyPipelineLayout(device, kit.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, kit.descriptorLayout, nullptr);
        vkDestroyShaderModule(device, kit.arrayModule, nullptr);
        vkDestroyShaderModule(device, kit.volumeModule, nullptr);
        kit = {};
    }

    Bool WireE5RawCopySupported(VkPhysicalDevice physicalDevice) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R32_UINT, &properties);
        constexpr VkFormatFeatureFlags kRequired = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        return (properties.optimalTilingFeatures & kRequired) == kRequired;
    }

    VkImageCreateInfo WireE5RawCopyTwinInfo(const WireE5RawCopyRegion& region) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = region.volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32_UINT;
        info.extent = {region.extent.width, region.extent.height, region.volume ? region.extent.depth : 1u};
        info.mipLevels = 1;
        info.arrayLayers = region.volume ? 1u : region.layerCount;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return info;
    }

    void DestroyWireE5RawCopyObjects(VkDevice device, WireE5RawCopyObjects& objects) {
        vkDestroyImageView(device, objects.sourceView, nullptr);
        vkDestroyImageView(device, objects.twinView, nullptr);
        vkDestroyDescriptorPool(device, objects.descriptorPool, nullptr);
        objects = {};
    }

    VkResult RecordWireE5RawCopy(VkDevice device, VkCommandBuffer commandBuffer, const WireE5RawCopyKit& kit,
                                 const WireE5RawCopyRegion& region, VkImageLayout sourceLayout, VkImage twin,
                                 WireE5RawCopyObjects& objects) {
        if (kit.arrayPipeline == VK_NULL_HANDLE || kit.volumePipeline == VK_NULL_HANDLE)
            return VK_ERROR_INITIALIZATION_FAILED;
        const Uint32 slices = region.volume ? region.extent.depth : region.layerCount;
        if (region.extent.width == 0 || region.extent.height == 0 || slices == 0) return VK_SUCCESS;

        // The source through R32_UINT: same 32-bit compatibility class, so a plain view of a
        // MUTABLE_FORMAT image. A 3D view covers every slice; the shader offsets into it.
        VkImageViewCreateInfo sourceInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        sourceInfo.image = region.source;
        sourceInfo.viewType = region.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        sourceInfo.format = VK_FORMAT_R32_UINT;
        sourceInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, region.level, 1,
                                       region.volume ? 0u : region.baseLayer, region.volume ? 1u : region.layerCount};
        VkResult result = vkCreateImageView(device, &sourceInfo, nullptr, &objects.sourceView);
        if (result != VK_SUCCESS) return result;
        VkImageViewCreateInfo twinInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        twinInfo.image = twin;
        twinInfo.viewType = region.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        twinInfo.format = VK_FORMAT_R32_UINT;
        twinInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, region.volume ? 1u : region.layerCount};
        result = vkCreateImageView(device, &twinInfo, nullptr, &objects.twinView);
        if (result != VK_SUCCESS) return result;

        const VkDescriptorPoolSize poolSizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = poolSizes;
        result = vkCreateDescriptorPool(device, &pool, nullptr, &objects.descriptorPool);
        if (result != VK_SUCCESS) return result;
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = objects.descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &kit.descriptorLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        result = vkAllocateDescriptorSets(device, &alloc, &set);
        if (result != VK_SUCCESS) return result;
        const VkDescriptorImageInfo sampled{kit.nearest, objects.sourceView, sourceLayout};
        const VkDescriptorImageInfo storage{VK_NULL_HANDLE, objects.twinView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        for (Uint32 i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &sampled;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &storage;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // Every earlier write - transfer, attachment or shader - visible to the fetch, and the
        // fresh twin ready for storage writes.
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkImageMemoryBarrier twinBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        twinBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        twinBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        twinBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        twinBarrier.srcQueueFamilyIndex = twinBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        twinBarrier.image = twin;
        twinBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &memory, 0, nullptr, 1, &twinBarrier);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          region.volume ? kit.volumePipeline : kit.arrayPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, kit.pipelineLayout, 0, 1, &set, 0,
                                nullptr);
        const RegionPush push{{region.offset.x, region.offset.y, region.volume ? region.offset.z : 0, 0},
                              {static_cast<Int32>(region.extent.width), static_cast<Int32>(region.extent.height),
                               static_cast<Int32>(slices), 0}};
        vkCmdPushConstants(commandBuffer, kit.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer, (region.extent.width + kGroupSize - 1) / kGroupSize,
                      (region.extent.height + kGroupSize - 1) / kGroupSize, slices);

        twinBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        twinBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        twinBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        twinBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &twinBarrier);
        return VK_SUCCESS;
    }

    String DescribeWireE5TransferProbe(const WireE5TransferProbeMeasurement& measurement) {
        if (!measurement.ran)
            return format("not run ({})", measurement.failureReason.empty() ? "?" : measurement.failureReason);
        return format("30x30 transfer read: {}/{} words changed (first {:#010x} -> {:#010x}); shader read: {} "
                      "changed; 16x16 transfer read: {}/{} changed",
                      measurement.subjectMismatches, measurement.texels, measurement.firstExpected,
                      measurement.firstObserved, measurement.controlMismatches, measurement.smallMismatches,
                      measurement.smallTexels);
    }

    namespace {
        constexpr Uint32 kSubjectExtent = 30;
        constexpr Uint32 kSmallExtent = 16;
        constexpr Uint64 kFenceTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

        // A non-canonical RGB9_E5 word: exponent 1..31 and every mantissa below 256, so the same
        // value also encodes with the exponent one lower and every mantissa doubled. A path that
        // decodes and re-encodes changes it; a bit move does not.
        Uint32 NonCanonicalWord(Uint32 index) {
            Uint32 state = index * 2654435761u + 0x9E3779B9u;
            state ^= state >> 15;
            state *= 2246822519u;
            state ^= state >> 13;
            const Uint32 r = state & 0xFFu;
            const Uint32 g = (state >> 8) & 0xFFu;
            const Uint32 b = ((state >> 16) & 0xFFu) | ((r | g) == 0 ? 1u : 0u);
            const Uint32 exponent = 1u + ((state >> 24) % 31u);
            return r | (g << 9) | (b << 18) | (exponent << 27);
        }

        struct ProbeObjects {
            VkDevice device = VK_NULL_HANDLE;
            Vector<VkImage> images;
            Vector<VkDeviceMemory> memories;
            Vector<VkBuffer> buffers;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            WireE5RawCopyKit kit;
            WireE5RawCopyObjects copy;

            void Destroy() {
                DestroyWireE5RawCopyObjects(device, copy);
                for (VkImage image : images) vkDestroyImage(device, image, nullptr);
                for (VkBuffer buffer : buffers) vkDestroyBuffer(device, buffer, nullptr);
                for (VkDeviceMemory memory : memories) vkFreeMemory(device, memory, nullptr);
                vkDestroyCommandPool(device, commandPool, nullptr);
                vkDestroyFence(device, fence, nullptr);
                DestroyWireE5RawCopyKit(device, kit);
            }
        };

        Optional<Uint32> PickMemoryType(const VkPhysicalDeviceMemoryProperties& properties, Uint32 allowed,
                                        VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags required) {
            Optional<Uint32> fallback;
            for (Uint32 i = 0; i < properties.memoryTypeCount; ++i) {
                if (!(allowed & (1u << i))) continue;
                const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
                if (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) continue;
                if ((flags & required) != required) continue;
                if ((flags & wanted) == wanted) return i;
                if (!fallback) fallback = i;
            }
            return fallback;
        }

        void ImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
                          VkImageLayout newLayout, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                          VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    } // namespace

    WireE5TransferProbeMeasurement RunWireE5TransferProbe(const WireE5TransferProbeContext& context) {
        WireE5TransferProbeMeasurement measurement;
        const VkDevice device = context.device;
        if (device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE) {
            measurement.failureReason = "no device or queue";
            return measurement;
        }
        if (!WireE5RawCopySupported(context.physicalDevice)) {
            measurement.failureReason = "R32_UINT is not a sampled + storage + transfer-source format here";
            return measurement;
        }
        // The usage production gives a wire E5B9G9R9 texture: SAMPLED | TRANSFER_SRC | TRANSFER_DST,
        // plus COLOR_ATTACHMENT where the format can be one.
        VkFormatProperties e5Properties{};
        vkGetPhysicalDeviceFormatProperties(context.physicalDevice, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, &e5Properties);
        constexpr VkFormatFeatureFlags kE5Required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                                     VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((e5Properties.optimalTilingFeatures & kE5Required) != kE5Required) {
            measurement.failureReason = "E5B9G9R9 is not a sampled transfer format here";
            return measurement;
        }
        VkImageUsageFlags e5Usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (e5Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
            e5Usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        ProbeObjects objects;
        objects.device = device;
        Bool leak = false;
        struct Teardown {
            ProbeObjects& objects;
            Bool& leak;
            ~Teardown() {
                if (!leak) objects.Destroy();
            }
        } teardown{objects, leak};
        const auto fail = [&](String reason) {
            measurement.ran = false;
            measurement.failureReason = Move(reason);
            return measurement;
        };

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(context.physicalDevice, &memoryProperties);
        const auto bindImage = [&](VkImage image) -> Bool {
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, image, &requirements);
            const Optional<Uint32> type = PickMemoryType(memoryProperties, requirements.memoryTypeBits,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
            if (!type) return false;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = *type;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
            objects.memories.push_back(memory);
            return vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS;
        };
        const auto makeImage = [&](const VkImageCreateInfo& info, VkImage& out) -> Bool {
            if (vkCreateImage(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.images.push_back(out);
            return bindImage(out);
        };
        const auto makeE5 = [&](Uint32 extent, VkImage& out) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            info.extent = {extent, extent, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = e5Usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return makeImage(info, out);
        };
        // Host-visible buffers: the upload source and the three readbacks, one allocation each.
        struct HostBuffer {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            Bool coherent = false;
        };
        const auto makeBuffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, HostBuffer& out) -> Bool {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
            objects.buffers.push_back(out.buffer);
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
            const Optional<Uint32> type =
                PickMemoryType(memoryProperties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (!type) return false;
            out.coherent =
                (memoryProperties.memoryTypes[*type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = *type;
            if (vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
            objects.memories.push_back(out.memory);
            return vkBindBufferMemory(device, out.buffer, out.memory, 0) == VK_SUCCESS;
        };

        constexpr Uint32 kTexels = kSubjectExtent * kSubjectExtent;
        constexpr Uint32 kSmallTexels = kSmallExtent * kSmallExtent;
        constexpr VkDeviceSize kBytes = kTexels * sizeof(Uint32);
        constexpr VkDeviceSize kSmallBytes = kSmallTexels * sizeof(Uint32);

        VkImage subject = VK_NULL_HANDLE, small = VK_NULL_HANDLE, twin = VK_NULL_HANDLE;
        if (!makeE5(kSubjectExtent, subject) || !makeE5(kSmallExtent, small))
            return fail("E5B9G9R9 image creation failed");
        WireE5RawCopyRegion region;
        region.source = subject;
        region.extent = {kSubjectExtent, kSubjectExtent, 1};
        if (!makeImage(WireE5RawCopyTwinInfo(region), twin)) return fail("R32_UINT twin creation failed");
        HostBuffer upload, subjectOut, controlOut, smallOut;
        if (!makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload) ||
            !makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, subjectOut) ||
            !makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, controlOut) ||
            !makeBuffer(kSmallBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, smallOut))
            return fail("buffer creation failed");
        if (const VkResult result = CreateWireE5RawCopyKit(device, objects.kit); result != VK_SUCCESS)
            return fail(format("raw copy kit creation failed (VkResult {})", static_cast<Int>(result)));

        {
            void* mapped = nullptr;
            if (vkMapMemory(device, upload.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
                return fail("vkMapMemory (upload) failed");
            Uint32* words = static_cast<Uint32*>(mapped);
            for (Uint32 i = 0; i < kTexels; ++i) words[i] = NonCanonicalWord(i);
            if (!upload.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = upload.memory;
                range.size = VK_WHOLE_SIZE;
                vkFlushMappedMemoryRanges(device, 1, &range);
            }
            vkUnmapMemory(device, upload.memory);
        }

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = context.queueFamilyIndex;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &objects.commandPool) != VK_SUCCESS)
            return fail("vkCreateCommandPool failed");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = objects.commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &commandInfo, &cmd) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers failed");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) return fail("vkBeginCommandBuffer failed");

        // (1) Upload: transfer writes into E5B9G9R9 move bits (measured), so both images hold
        // exactly the generated words; the small one gets the first 8x8 of them.
        for (const VkImage image : {subject, small}) {
            ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kSubjectExtent, kSubjectExtent, 1};
        vkCmdCopyBufferToImage(cmd, upload.buffer, subject, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkBufferImageCopy smallCopy = copy;
        smallCopy.bufferRowLength = kSubjectExtent;
        smallCopy.imageExtent = {kSmallExtent, kSmallExtent, 1};
        vkCmdCopyBufferToImage(cmd, upload.buffer, small, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &smallCopy);

        // (2) SUBJECT and the small reading: plain transfer reads.
        for (const VkImage image : {subject, small}) {
            ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
        }
        vkCmdCopyImageToBuffer(cmd, subject, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, subjectOut.buffer, 1, &copy);
        VkBufferImageCopy smallOutCopy = copy;
        smallOutCopy.imageExtent = {kSmallExtent, kSmallExtent, 1};
        vkCmdCopyImageToBuffer(cmd, small, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, smallOut.buffer, 1, &smallOutCopy);

        // (3) CONTROL: the route, exactly as production records it.
        ImageBarrier(cmd, subject, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
        if (const VkResult result =
                RecordWireE5RawCopy(device, cmd, objects.kit, region, VK_IMAGE_LAYOUT_GENERAL, twin, objects.copy);
            result != VK_SUCCESS)
            return fail(format("recording the raw copy failed (VkResult {})", static_cast<Int>(result)));
        vkCmdCopyImageToBuffer(cmd, twin, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, controlOut.buffer, 1, &copy);

        VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0,
                             nullptr, 0, nullptr);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return fail("vkEndCommandBuffer failed");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device, &fenceInfo, nullptr, &objects.fence) != VK_SUCCESS)
            return fail("vkCreateFence failed");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (const VkResult result = vkQueueSubmit(context.queue, 1, &submit, objects.fence); result != VK_SUCCESS)
            return fail(format("vkQueueSubmit failed (VkResult {})", static_cast<Int>(result)));
        const VkResult waited = vkWaitForFences(device, 1, &objects.fence, VK_TRUE, kFenceTimeoutNs);
        if (waited == VK_TIMEOUT) {
            leak = true;
            measurement.fenceWaitTimedOut = true;
            return fail("the probe's fence did not signal within 5 s");
        }
        if (waited != VK_SUCCESS) return fail(format("vkWaitForFences failed (VkResult {})", static_cast<Int>(waited)));

        const auto read = [&](const HostBuffer& buffer, const Uint32*& words) -> Bool {
            void* mapped = nullptr;
            if (vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
                return false;
            if (!buffer.coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = buffer.memory;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(device, 1, &range);
            }
            words = static_cast<const Uint32*>(mapped);
            return true;
        };
        const Uint32 *subjectWords = nullptr, *controlWords = nullptr, *smallWords = nullptr;
        if (!read(subjectOut, subjectWords) || !read(controlOut, controlWords) || !read(smallOut, smallWords))
            return fail("vkMapMemory (readback) failed");
        measurement.texels = kTexels;
        for (Uint32 i = 0; i < kTexels; ++i) {
            const Uint32 expected = NonCanonicalWord(i);
            if (subjectWords[i] != expected) {
                if (measurement.subjectMismatches++ == 0) {
                    measurement.firstExpected = expected;
                    measurement.firstObserved = subjectWords[i];
                }
            }
            if (controlWords[i] != expected) ++measurement.controlMismatches;
        }
        measurement.smallTexels = kSmallTexels;
        for (Uint32 y = 0; y < kSmallExtent; ++y)
            for (Uint32 x = 0; x < kSmallExtent; ++x)
                if (smallWords[y * kSmallExtent + x] != NonCanonicalWord(y * kSubjectExtent + x))
                    ++measurement.smallMismatches;
        vkUnmapMemory(device, subjectOut.memory);
        vkUnmapMemory(device, controlOut.memory);
        vkUnmapMemory(device, smallOut.memory);
        measurement.ran = true;
        return measurement;
    }

    WireE5TransferProbeMeasurement RunWireE5TransferProbeOnThrowawayDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                                           VkInstance instance,
                                                                           VkPhysicalDevice physicalDevice,
                                                                           Uint32 graphicsQueueFamilyIndex) {
        WireE5TransferProbeMeasurement measurement;
        const auto createDevice =
            reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(instance, "vkCreateDevice"));
        if (createDevice == nullptr) {
            measurement.failureReason = "no vkCreateDevice";
            return measurement;
        }
        const Float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        VkDevice device = VK_NULL_HANDLE;
        if (const VkResult result = createDevice(physicalDevice, &deviceInfo, nullptr, &device);
            result != VK_SUCCESS || device == VK_NULL_HANDLE) {
            measurement.failureReason = format("vkCreateDevice failed (VkResult {})", static_cast<Int>(result));
            return measurement;
        }
        WireE5TransferProbeContext context;
        context.physicalDevice = physicalDevice;
        context.device = device;
        vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &context.queue);
        context.queueFamilyIndex = graphicsQueueFamilyIndex;
        measurement = RunWireE5TransferProbe(context);
        if (!measurement.fenceWaitTimedOut) vkDestroyDevice(device, nullptr);
        return measurement;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
