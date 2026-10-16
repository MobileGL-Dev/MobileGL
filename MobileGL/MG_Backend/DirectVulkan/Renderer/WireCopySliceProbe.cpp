// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireCopySliceProbe.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// See WireCopySliceProbe.h. Plumbing as in WireDepthResolveProbe.cpp: one throwaway command
// buffer, a bounded fence wait that leaks rather than idle-waits a hung GPU.

#include "WireCopySliceProbe.h"

#include <vulkan/vulkan_core.h>

namespace MobileGL::MG_Backend::DirectVulkan {
    String DescribeWireCopySliceProbe(const WireCopySliceProbeMeasurement& measurement) {
        if (!measurement.ran)
            return format("not run ({})", measurement.failureReason.empty() ? "?" : measurement.failureReason);
        return format("one-region 2D-array A2B10G10R10_UINT -> 3D A2B10G10R10_UNORM copy: {}/{} texels wrong, "
                      "{}/{} slices exact; per-slice regions: {} wrong",
                      measurement.subjectMismatches, measurement.texels, measurement.subjectExactSlices,
                      measurement.slices, measurement.controlMismatches);
    }

    namespace {
        constexpr Uint32 kExtent = 7;
        constexpr Uint32 kSlices = 8;
        constexpr Uint32 kSliceTexels = kExtent * kExtent;
        constexpr Uint32 kTexels = kSliceTexels * kSlices;
        // The destination is larger than the region, as in the copy_image cases that drop slices.
        constexpr Uint32 kDestinationExtent = 30;
        constexpr Uint32 kDestinationDepth = 24;
        constexpr Uint32 kDestinationTexels = kDestinationExtent * kDestinationExtent * kDestinationDepth;
        constexpr VkFormat kSourceFormat = VK_FORMAT_A2B10G10R10_UINT_PACK32;
        constexpr VkFormat kDestinationFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        constexpr Uint64 kFenceTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

        Uint32 Word(Uint32 index, Uint32 salt) {
            Uint32 state = (index + salt * 0x10001u) * 2654435761u + 0x9E3779B9u;
            state ^= state >> 15;
            state *= 2246822519u;
            state ^= state >> 13;
            return state;
        }

        struct ProbeObjects {
            VkDevice device = VK_NULL_HANDLE;
            Vector<VkImage> images;
            Vector<VkBuffer> buffers;
            Vector<VkDeviceMemory> memories;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;

            void Destroy() {
                for (VkImage image : images)
                    vkDestroyImage(device, image, nullptr);
                for (VkBuffer buffer : buffers)
                    vkDestroyBuffer(device, buffer, nullptr);
                for (VkDeviceMemory memory : memories)
                    vkFreeMemory(device, memory, nullptr);
                vkDestroyCommandPool(device, commandPool, nullptr);
                vkDestroyFence(device, fence, nullptr);
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
                          VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    } // namespace

    WireCopySliceProbeMeasurement RunWireCopySliceProbe(const WireCopySliceProbeContext& context) {
        WireCopySliceProbeMeasurement measurement;
        const VkDevice device = context.device;
        if (device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE) {
            measurement.failureReason = "no device or queue";
            return measurement;
        }
        constexpr VkFormatFeatureFlags kRequired =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        for (const VkFormat format : {kSourceFormat, kDestinationFormat}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(context.physicalDevice, format, &properties);
            if ((properties.optimalTilingFeatures & kRequired) != kRequired) {
                measurement.failureReason = "A2B10G10R10 UINT/UNORM are not transfer formats here";
                return measurement;
            }
        }

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
        // The wire's images (VkTextureManager): MUTABLE_FORMAT (a storage-compatible colour class),
        // SAMPLED | TRANSFER_SRC | TRANSFER_DST | COLOR_ATTACHMENT, a 3D one 2D_ARRAY_COMPATIBLE.
        const auto makeImage = [&](Bool volume, VkFormat format, VkImage& out) -> Bool {
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(context.physicalDevice, format, &formatProperties);
            const Bool attachable =
                (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (volume ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0);
            info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = volume ? VkExtent3D{kDestinationExtent, kDestinationExtent, kDestinationDepth}
                                 : VkExtent3D{kExtent, kExtent, 1};
            info.mipLevels = 1;
            info.arrayLayers = volume ? 1u : kSlices;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | (attachable ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.images.push_back(out);
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, out, &requirements);
            const Optional<Uint32> type =
                PickMemoryType(memoryProperties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
            if (!type) return false;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = *type;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
            objects.memories.push_back(memory);
            return vkBindImageMemory(device, out, memory, 0) == VK_SUCCESS;
        };
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
        const auto access = [&](const HostBuffer& buffer, Bool write, auto&& body) -> Bool {
            void* mapped = nullptr;
            if (vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
                return false;
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = buffer.memory;
            range.size = VK_WHOLE_SIZE;
            if (!write && !buffer.coherent) vkInvalidateMappedMemoryRanges(device, 1, &range);
            body(static_cast<Uint32*>(mapped));
            if (write && !buffer.coherent) vkFlushMappedMemoryRanges(device, 1, &range);
            vkUnmapMemory(device, buffer.memory);
            return true;
        };

        VkImage source = VK_NULL_HANDLE, subject = VK_NULL_HANDLE, control = VK_NULL_HANDLE;
        if (!makeImage(false, kSourceFormat, source) || !makeImage(true, kDestinationFormat, subject) ||
            !makeImage(true, kDestinationFormat, control))
            return fail("image creation failed");
        HostBuffer sourceWords, sentinelWords, subjectOut, controlOut;
        if (!makeBuffer(kTexels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sourceWords) ||
            !makeBuffer(kDestinationTexels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sentinelWords) ||
            !makeBuffer(kTexels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, subjectOut) ||
            !makeBuffer(kTexels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, controlOut))
            return fail("buffer creation failed");
        if (!access(sourceWords, true,
                    [](Uint32* words) {
                        for (Uint32 i = 0; i < kTexels; ++i)
                            words[i] = Word(i, 1);
                    }) ||
            !access(sentinelWords, true, [](Uint32* words) {
                for (Uint32 i = 0; i < kDestinationTexels; ++i)
                    words[i] = Word(i, 2);
            }))
            return fail("vkMapMemory (upload) failed");

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

        // (1) The source's words, and a sentinel in both destinations so an untouched slice shows.
        for (const VkImage image : {source, subject, control})
            ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy layered{};
        layered.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kSlices};
        layered.imageExtent = {kExtent, kExtent, 1};
        VkBufferImageCopy volume{};
        volume.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        volume.imageExtent = {kExtent, kExtent, kSlices};
        VkBufferImageCopy wholeVolume = volume;
        wholeVolume.imageExtent = {kDestinationExtent, kDestinationExtent, kDestinationDepth};
        vkCmdCopyBufferToImage(cmd, sourceWords.buffer, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &layered);
        vkCmdCopyBufferToImage(cmd, sentinelWords.buffer, subject, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &wholeVolume);
        vkCmdCopyBufferToImage(cmd, sentinelWords.buffer, control, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &wholeVolume);
        ImageBarrier(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        for (const VkImage image : {subject, control})
            ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

        // (2) SUBJECT: the one region CopyImageSubData records. CONTROL: one region per slice.
        VkImageCopy whole{};
        whole.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kSlices};
        whole.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        whole.extent = {kExtent, kExtent, kSlices};
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, subject, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &whole);
        VkImageCopy perSlice[kSlices]{};
        for (Uint32 slice = 0; slice < kSlices; ++slice) {
            perSlice[slice].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, slice, 1};
            perSlice[slice].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            perSlice[slice].dstOffset = {0, 0, static_cast<Int32>(slice)};
            perSlice[slice].extent = {kExtent, kExtent, 1};
        }
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, control, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       kSlices, perSlice);

        // (3) Both destinations out: same-format reads of a 3D image, which the defect does not touch.
        for (const VkImage image : {subject, control})
            ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(cmd, subject, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, subjectOut.buffer, 1, &volume);
        vkCmdCopyImageToBuffer(cmd, control, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, controlOut.buffer, 1, &volume);
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

        const auto tally = [&](const Uint32* words, Uint32& mismatches, Uint32* exactSlices) {
            for (Uint32 slice = 0; slice < kSlices; ++slice) {
                Uint32 sliceMismatches = 0;
                for (Uint32 i = slice * kSliceTexels; i < (slice + 1) * kSliceTexels; ++i)
                    if (words[i] != Word(i, 1)) ++sliceMismatches;
                mismatches += sliceMismatches;
                if (exactSlices && sliceMismatches == 0) ++*exactSlices;
            }
        };
        if (!access(
                subjectOut, false,
                [&](Uint32* words) { tally(words, measurement.subjectMismatches, &measurement.subjectExactSlices); }) ||
            !access(controlOut, false, [&](Uint32* words) { tally(words, measurement.controlMismatches, nullptr); }))
            return fail("vkMapMemory (readback) failed");
        measurement.texels = kTexels;
        measurement.slices = kSlices;
        measurement.ran = true;
        return measurement;
    }

    WireCopySliceProbeMeasurement RunWireCopySliceProbeOnThrowawayDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                                         VkInstance instance,
                                                                         VkPhysicalDevice physicalDevice,
                                                                         Uint32 graphicsQueueFamilyIndex) {
        WireCopySliceProbeMeasurement measurement;
        const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(instance, "vkCreateDevice"));
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
        WireCopySliceProbeContext context;
        context.physicalDevice = physicalDevice;
        context.device = device;
        vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &context.queue);
        context.queueFamilyIndex = graphicsQueueFamilyIndex;
        measurement = RunWireCopySliceProbe(context);
        if (!measurement.fenceWaitTimedOut) vkDestroyDevice(device, nullptr);
        return measurement;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
