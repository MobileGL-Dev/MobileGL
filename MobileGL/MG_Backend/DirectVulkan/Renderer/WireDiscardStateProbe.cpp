// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDiscardStateProbe.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// See WireDiscardStateProbe.h. Plumbing as in WireDepthResolveProbe.cpp: one throwaway command
// buffer, a bounded fence wait that leaks rather than idle-waits a hung GPU.

#include "WireDiscardStateProbe.h"

#include <vulkan/vulkan_core.h>

namespace MobileGL::MG_Backend::DirectVulkan {
#include "WireDiscardStateProbeSpirv.h"

    String DescribeWireDiscardStateProbe(const WireDiscardStateProbeMeasurement& measurement) {
        if (!measurement.ran)
            return format("not run ({})", measurement.failureReason.empty() ? "?" : measurement.failureReason);
        return format("a draw inheriting viewport/scissor after a discard draw: {}/{} pixels; after a clipped "
                      "rasterizing draw: {}/{}",
                      measurement.subjectGreen, measurement.pixels, measurement.controlGreen, measurement.pixels);
    }

    namespace {
        constexpr Uint32 kExtent = 16;
        constexpr Uint32 kPixels = kExtent * kExtent;
        constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr Uint64 kFenceTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

        struct ProbeObjects {
            VkDevice device = VK_NULL_HANDLE;
            Vector<VkImage> images;
            Vector<VkImageView> views;
            Vector<VkBuffer> buffers;
            Vector<VkDeviceMemory> memories;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkPipeline> pipelines;
            Vector<VkShaderModule> modules;
            VkRenderPass pass = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;

            void Destroy() {
                for (VkPipeline pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
                for (VkShaderModule module : modules) vkDestroyShaderModule(device, module, nullptr);
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
                for (VkFramebuffer framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
                vkDestroyRenderPass(device, pass, nullptr);
                for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
                for (VkImage image : images) vkDestroyImage(device, image, nullptr);
                for (VkBuffer buffer : buffers) vkDestroyBuffer(device, buffer, nullptr);
                for (VkDeviceMemory memory : memories) vkFreeMemory(device, memory, nullptr);
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
    } // namespace

    WireDiscardStateProbeMeasurement RunWireDiscardStateProbe(const WireDiscardStateProbeContext& context) {
        WireDiscardStateProbeMeasurement measurement;
        const VkDevice device = context.device;
        if (device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE) {
            measurement.failureReason = "no device or queue";
            return measurement;
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
        const auto allocate = [&](const VkMemoryRequirements& requirements, VkMemoryPropertyFlags wanted,
                                  VkMemoryPropertyFlags required, VkDeviceMemory& out, Bool* coherent) -> Bool {
            const Optional<Uint32> type =
                PickMemoryType(memoryProperties, requirements.memoryTypeBits, wanted, required);
            if (!type) return false;
            if (coherent)
                *coherent = (memoryProperties.memoryTypes[*type].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = *type;
            if (vkAllocateMemory(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.memories.push_back(out);
            return true;
        };

        // One render pass for both lanes: clear, draw twice, hand the target to the readback copy.
        {
            VkAttachmentDescription attachment{};
            attachment.format = kColorFormat;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &reference;
            // The readback copy waits on the pass's attachment writes.
            VkSubpassDependency dependency{};
            dependency.srcSubpass = 0;
            dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;
            if (vkCreateRenderPass(device, &info, nullptr, &objects.pass) != VK_SUCCESS)
                return fail("vkCreateRenderPass failed");
        }

        // Two lanes, SUBJECT (the first draw discarded) and CONTROL (the first draw clipped away):
        // each has its own target, framebuffer and readback.
        struct Lane {
            VkImage target = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkBuffer readback = VK_NULL_HANDLE;
            VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
            Bool readbackCoherent = false;
            VkPipeline first = VK_NULL_HANDLE;
        } lanes[2];
        for (Lane& lane : lanes) {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = kColorFormat;
            imageInfo.extent = {kExtent, kExtent, 1};
            imageInfo.mipLevels = imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &imageInfo, nullptr, &lane.target) != VK_SUCCESS)
                return fail("vkCreateImage failed");
            objects.images.push_back(lane.target);
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, lane.target, &requirements);
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            if (!allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, imageMemory, nullptr) ||
                vkBindImageMemory(device, lane.target, imageMemory, 0) != VK_SUCCESS)
                return fail("image memory failed");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = lane.target;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = kColorFormat;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS)
                return fail("vkCreateImageView failed");
            objects.views.push_back(view);
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = objects.pass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &view;
            framebufferInfo.width = framebufferInfo.height = kExtent;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &lane.framebuffer) != VK_SUCCESS)
                return fail("vkCreateFramebuffer failed");
            objects.framebuffers.push_back(lane.framebuffer);

            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = kPixels * 4;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bufferInfo, nullptr, &lane.readback) != VK_SUCCESS)
                return fail("vkCreateBuffer failed");
            objects.buffers.push_back(lane.readback);
            vkGetBufferMemoryRequirements(device, lane.readback, &requirements);
            if (!allocate(requirements, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          lane.readbackMemory, &lane.readbackCoherent) ||
                vkBindBufferMemory(device, lane.readback, lane.readbackMemory, 0) != VK_SUCCESS)
                return fail("readback memory failed");
        }

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &objects.pipelineLayout) != VK_SUCCESS)
            return fail("vkCreatePipelineLayout failed");
        const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& out) -> Bool {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes;
            info.pCode = code;
            if (vkCreateShaderModule(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.modules.push_back(out);
            return true;
        };
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        if (!shader(kWireDiscardStateProbeVertexSpirv, sizeof(kWireDiscardStateProbeVertexSpirv), vertex) ||
            !shader(kWireDiscardStateProbeFragmentSpirv, sizeof(kWireDiscardStateProbeFragmentSpirv), fragment))
            return fail("vkCreateShaderModule failed");

        // Every pipeline declares viewport and scissor dynamic; only `discard` differs.
        const auto makePipeline = [&](Bool discard, VkPipeline& out) -> Bool {
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto& stage : stages) {
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stage.pName = "main";
            }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertex;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragment;
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewportState.viewportCount = viewportState.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.rasterizerDiscardEnable = discard ? VK_TRUE : VK_FALSE;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &blendAttachment;
            constexpr VkDynamicState kDynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = kDynamic;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewportState;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &samples;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = objects.pipelineLayout;
            info.renderPass = objects.pass;
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.pipelines.push_back(out);
            return true;
        };
        VkPipeline second = VK_NULL_HANDLE;
        if (!makePipeline(true, lanes[0].first) || !makePipeline(false, lanes[1].first) || !makePipeline(false, second))
            return fail("pipeline creation failed");

        VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        commandPoolInfo.queueFamilyIndex = context.queueFamilyIndex;
        if (vkCreateCommandPool(device, &commandPoolInfo, nullptr, &objects.commandPool) != VK_SUCCESS)
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

        const VkClearValue black{{{0.0f, 0.0f, 0.0f, 1.0f}}};
        const VkViewport viewport{0.0f, 0.0f, static_cast<Float>(kExtent), static_cast<Float>(kExtent), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {kExtent, kExtent}};
        for (Uint32 i = 0; i < 2; ++i) {
            const Lane& lane = lanes[i];
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = objects.pass;
            begin.framebuffer = lane.framebuffer;
            begin.renderArea = scissor;
            begin.clearValueCount = 1;
            begin.pClearValues = &black;
            vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            // The first draw sets the state: discarded (SUBJECT) or a strip the vertex shader puts
            // outside the clip volume (CONTROL, vertices 4-7).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lane.first);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdDraw(cmd, 4, 1, i == 0 ? 0 : 4, 0);
            // The second draw covers the target with what it inherits.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, second);
            vkCmdDraw(cmd, 4, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {kExtent, kExtent, 1};
            vkCmdCopyImageToBuffer(cmd, lane.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lane.readback, 1, &copy);
        }
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

        Uint32* green[2] = {&measurement.subjectGreen, &measurement.controlGreen};
        for (Uint32 i = 0; i < 2; ++i) {
            const Lane& lane = lanes[i];
            void* mapped = nullptr;
            if (vkMapMemory(device, lane.readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
                return fail("vkMapMemory failed");
            if (!lane.readbackCoherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = lane.readbackMemory;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(device, 1, &range);
            }
            const Uint8* pixels = static_cast<const Uint8*>(mapped);
            for (Uint32 p = 0; p < kPixels; ++p)
                if (pixels[p * 4] < 16 && pixels[p * 4 + 1] > 240) ++*green[i];
            vkUnmapMemory(device, lane.readbackMemory);
        }
        measurement.pixels = kPixels;
        measurement.ran = true;
        return measurement;
    }

    WireDiscardStateProbeMeasurement RunWireDiscardStateProbeOnThrowawayDevice(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkPhysicalDevice physicalDevice,
        Uint32 graphicsQueueFamilyIndex) {
        WireDiscardStateProbeMeasurement measurement;
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
        WireDiscardStateProbeContext context;
        context.physicalDevice = physicalDevice;
        context.device = device;
        vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &context.queue);
        context.queueFamilyIndex = graphicsQueueFamilyIndex;
        measurement = RunWireDiscardStateProbe(context);
        if (!measurement.fenceWaitTimedOut) vkDestroyDevice(device, nullptr);
        return measurement;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
