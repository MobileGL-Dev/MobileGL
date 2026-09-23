// MobileGL - MobileGL/MG_Backend/DirectVulkan/Renderer/WireDepthResolveProbe.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// See WireDepthResolveProbe.h for the arms and the probe's shape, WireDepthResolveArm.h for the
// verdict. The plumbing is the PRIMITIVES_GENERATED probe's (MG_Util/SelfTest/
// PrimitivesGeneratedNoXfbProbe.cpp): one throwaway command buffer, a bounded fence wait that
// leaks rather than idle-waits a hung GPU, teardown on every other path.

#include "WireDepthResolveProbe.h"

#include <MG_Util/Debug/Log.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <vulkan/vulkan_core.h>

#if MOBILEGL_BUILD_DISAGGREGATED
namespace MobileGL::MG_Backend::DirectVulkan {
#include "WireMultisampleResolveSpirv.h"

    VkImageAspectFlags WireDepthStencilFormatAspects(VkFormat format) {
        if (format == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
        if (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
            format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    VkImageViewCreateInfo WireDepthStencilViewInfo(VkImage image, VkFormat format, VkImageAspectFlags aspects,
                                                   Uint32 level, Uint32 layer) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {aspects, level, 1, layer, 1};
        return info;
    }

    VkResult CreateWireDepthStencilResolvePass(PFN_vkCreateRenderPass2 createRenderPass2, VkDevice device,
                                               VkFormat format, VkSampleCountFlagBits samples, VkRenderPass* pass) {
        const VkImageAspectFlags formatAspects = WireDepthStencilFormatAspects(format);
        VkAttachmentDescription2 attachments[2]{};
        for (Uint32 i = 0; i < 2; ++i) {
            auto& attachment = attachments[i];
            attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachment.format = format;
            attachment.samples = i == 0 ? samples : VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = attachment.stencilLoadOp =
                i == 0 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
        VkAttachmentReference2 sourceReference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        sourceReference.attachment = 0;
        sourceReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sourceReference.aspectMask = formatAspects;
        VkAttachmentReference2 resolveReference = sourceReference;
        resolveReference.attachment = 1;
        VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode =
            (formatAspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.stencilResolveMode =
            (formatAspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &resolveReference;
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pNext = &resolve;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &sourceReference;
        VkRenderPassCreateInfo2 passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        passInfo.attachmentCount = 2;
        passInfo.pAttachments = attachments;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        return createRenderPass2(device, &passInfo, nullptr, pass);
    }

    VkResult CreateWireShaderResolveKit(VkDevice device, Bool shaderStencilExport, WireShaderResolveKit& kit) {
        const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes;
            info.pCode = code;
            return vkCreateShaderModule(device, &info, nullptr, &module);
        };
        VkResult result =
            shader(kWireMultisampleResolveVertexSpirv, sizeof(kWireMultisampleResolveVertexSpirv), kit.vertex);
        if (result != VK_SUCCESS) return result;
        result = shader(kWireMultisampleDepthResolveFragmentSpirv, sizeof(kWireMultisampleDepthResolveFragmentSpirv),
                        kit.depthFragment);
        if (result != VK_SUCCESS) return result;
        // THE STENCIL MODULE IS ONLY CREATED WHERE IT CAN RUN: its SPIR-V declares the
        // StencilExportEXT capability, and handing a module with an unsupported capability to
        // vkCreateShaderModule is undefined behaviour - not an error this could catch.
        if (shaderStencilExport) {
            result = shader(kWireMultisampleStencilResolveFragmentSpirv,
                            sizeof(kWireMultisampleStencilResolveFragmentSpirv), kit.stencilFragment);
            if (result != VK_SUCCESS) return result;
        }
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        result = vkCreateDescriptorSetLayout(device, &layout, nullptr, &kit.descriptorLayout);
        if (result != VK_SUCCESS) return result;
        // NO PUSH CONSTANT RANGE AT ALL: the pass reads its coordinate from gl_FragCoord and its
        // rectangle from the scissor, so neither stage takes an input.
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &kit.descriptorLayout;
        result = vkCreatePipelineLayout(device, &pipelineLayout, nullptr, &kit.pipelineLayout);
        if (result != VK_SUCCESS) return result;
        // NEAREST: texelFetch ignores the sampler's filter, and most devices do not advertise
        // linear filtering for a depth format anyway. The sampler exists only because GLSL's
        // sampler2DMS is a COMBINED image sampler.
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.maxLod = 0.0f;
        return vkCreateSampler(device, &sampler, nullptr, &kit.nearest);
    }

    void DestroyWireShaderResolveKit(VkDevice device, WireShaderResolveKit& kit) {
        vkDestroySampler(device, kit.nearest, nullptr);
        vkDestroyPipelineLayout(device, kit.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, kit.descriptorLayout, nullptr);
        vkDestroyShaderModule(device, kit.vertex, nullptr);
        vkDestroyShaderModule(device, kit.depthFragment, nullptr);
        vkDestroyShaderModule(device, kit.stencilFragment, nullptr);
        kit = {};
    }

    VkResult CreateWireShaderResolveRenderPass(VkDevice device, VkFormat format, VkRenderPass* pass) {
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        // DONT_CARE on BOTH aspects: the pass writes every texel of the scissored rect for the
        // aspect it owns, and the caller copies only that aspect out. The other aspect of a packed
        // format is left undefined on purpose - it is never read, and LOADing it would order this
        // pass behind a write nobody is waiting for.
        attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference depthStencil{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthStencil;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        return vkCreateRenderPass(device, &info, nullptr, pass);
    }

    VkResult CreateWireShaderResolvePipeline(VkDevice device, const WireShaderResolveKit& kit, VkRenderPass pass,
                                             VkImageAspectFlags aspect, VkPipeline* pipeline) {
        const VkShaderModule fragment =
            aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? kit.stencilFragment : kit.depthFragment;
        if (fragment == VK_NULL_HANDLE || kit.vertex == VK_NULL_HANDLE) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto& stage : stages) {
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = kit.vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthStencilState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencilState.minDepthBounds = 0.0f;
        depthStencilState.maxDepthBounds = 1.0f;
        if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
            // ALWAYS + write: gl_FragDepth is the answer, and a comparison against whatever
            // DONT_CARE left behind would discard some of it.
            depthStencilState.depthTestEnable = VK_TRUE;
            depthStencilState.depthWriteEnable = VK_TRUE;
            depthStencilState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        } else {
            // REPLACE on every outcome, compare ALWAYS, full write mask: that is what makes the
            // shader-exported reference the value that lands. The static `reference` is overridden
            // by gl_FragStencilRefARB, which is the whole point of the extension.
            depthStencilState.stencilTestEnable = VK_TRUE;
            VkStencilOpState op{};
            op.failOp = op.passOp = op.depthFailOp = VK_STENCIL_OP_REPLACE;
            op.compareOp = VK_COMPARE_OP_ALWAYS;
            op.compareMask = op.writeMask = 0xFFu;
            op.reference = 0;
            depthStencilState.front = depthStencilState.back = op;
        }
        // No colour attachment in this render pass, so no attachment states either.
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &samples;
        info.pDepthStencilState = &depthStencilState;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = kit.pipelineLayout;
        info.renderPass = pass;
        return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, pipeline);
    }

    void RecordWireShaderResolveDraw(VkCommandBuffer commandBuffer, const WireShaderResolveKit& kit,
                                     VkRenderPass pass, VkPipeline pipeline, VkFramebuffer framebuffer,
                                     VkDescriptorSet descriptorSet, VkExtent2D attachmentExtent, VkRect2D rect) {
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        // The RECTANGLE is the render area, exactly as the render-pass arm's renderArea is - the
        // scratch outside it is never read.
        begin.renderArea = rect;
        vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        // THE VIEWPORT IS THE WHOLE ATTACHMENT AND THE SCISSOR IS THE RECT, and the two are not
        // interchangeable here: gl_FragCoord is what the fragment shader fetches by, so the
        // clip-space-to-attachment mapping has to be the identity for every rectangle. A viewport
        // shrunk onto the rect would slide the whole image instead of moving a window over it.
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(attachmentExtent.width),
                                  static_cast<float>(attachmentExtent.height), 0.0f, 1.0f};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &rect);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, kit.pipelineLayout, 0, 1,
                                &descriptorSet, 0, nullptr);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);
    }

    namespace {
        constexpr Uint32 kProbeExtent = 4;
        constexpr Uint32 kProbeTexels = kProbeExtent * kProbeExtent;
        // Every Vulkan device must support 4 samples on a depth/stencil attachment
        // (framebufferDepthSampleCounts), and the CTS case the probe stands for fails at 2 and 4.
        constexpr VkSampleCountFlagBits kProbeSamples = VK_SAMPLE_COUNT_4_BIT;
        constexpr Float kClearDepth = 0.25f;
        constexpr Float kSentinelDepth = 0.75f;
        constexpr Uint32 kClearStencil = 0x5Au;
        constexpr Uint32 kSentinelStencil = 0xA5u;
        constexpr Uint64 kFenceTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;

        // The formats the wire maps GL's depth/stencil internal formats onto (VkTextureManager),
        // packed first: the case that set all this off failed on every one of them.
        struct ProbeFormat {
            VkFormat format;
            const char* name;
        };
        constexpr ProbeFormat kProbeFormats[] = {
            {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
            {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"},
            {VK_FORMAT_D16_UNORM, "D16_UNORM"},
            {VK_FORMAT_X8_D24_UNORM_PACK32, "X8_D24_UNORM_PACK32"},
            {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
            {VK_FORMAT_S8_UINT, "S8_UINT"},
        };

        // The usages production gives the two images: a wire multisample depth/stencil attachment
        // (VkTextureManager: SAMPLED | DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST) and
        // ResolveWireDepthStencil's resolve scratch (DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC).
        constexpr VkImageUsageFlags kSourceUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        constexpr VkImageUsageFlags kScratchUsage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        Bool IsFloatDepth(VkFormat format) {
            return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        }

        Uint32 DepthCopyBytes(VkFormat format) { return format == VK_FORMAT_D16_UNORM ? 2u : 4u; }

        Uint32 EncodeDepth(VkFormat format, Float depth) {
            if (IsFloatDepth(format)) return std::bit_cast<Uint32>(depth);
            const double scale = format == VK_FORMAT_D16_UNORM ? 65535.0 : 16777215.0;
            return static_cast<Uint32>(std::lround(static_cast<double>(depth) * scale));
        }

        Uint32 ReadDepthWord(VkFormat format, const Uint8* texel) {
            if (format == VK_FORMAT_D16_UNORM) {
                Uint16 word = 0;
                std::memcpy(&word, texel, sizeof(word));
                return word;
            }
            Uint32 word = 0;
            std::memcpy(&word, texel, sizeof(word));
            // A D24 aspect copies out as a 32-bit word whose top byte is undefined.
            return IsFloatDepth(format) ? word : (word & 0x00FFFFFFu);
        }

        Uint32 WordDistance(Uint32 a, Uint32 b) { return a > b ? a - b : b - a; }

        struct ProbeImage {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
        };

        struct ControlPass {
            Bool ready = false;
            ProbeImage target;
            VkRenderPass pass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkImageView sampledView = VK_NULL_HANDLE;
            VkImageView targetView = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
        };

        struct FormatWork {
            SizeT reading = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspects = 0;
            ProbeImage subjectSource, controlSource, subjectTarget;
            VkRenderPass resolvePass = VK_NULL_HANDLE;
            VkImageView resolveViews[2]{};
            VkFramebuffer resolveFramebuffer = VK_NULL_HANDLE;
            VkDeviceSize subjectDepthOffset = 0, subjectStencilOffset = 0;
            ControlPass depthControl, stencilControl;
        };

        // Everything the probe creates, destroyed together (or leaked together on a timeout).
        struct ProbeObjects {
            VkDevice device = VK_NULL_HANDLE;
            Vector<VkImage> images;
            Vector<VkDeviceMemory> memories;
            Vector<VkImageView> views;
            Vector<VkRenderPass> passes;
            Vector<VkFramebuffer> framebuffers;
            Vector<VkPipeline> pipelines;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            WireShaderResolveKit kit;

            void Destroy() {
                for (VkPipeline pipeline : pipelines) vkDestroyPipeline(device, pipeline, nullptr);
                for (VkFramebuffer framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
                for (VkRenderPass pass : passes) vkDestroyRenderPass(device, pass, nullptr);
                for (VkImageView view : views) vkDestroyImageView(device, view, nullptr);
                for (VkImage image : images) vkDestroyImage(device, image, nullptr);
                vkDestroyBuffer(device, buffer, nullptr);
                for (VkDeviceMemory memory : memories) vkFreeMemory(device, memory, nullptr);
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
                vkDestroyCommandPool(device, commandPool, nullptr);
                vkDestroyFence(device, fence, nullptr);
                DestroyWireShaderResolveKit(device, kit);
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

        void ImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspects,
                          VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                          VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = srcAccess;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {aspects, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
    } // namespace

    WireDepthResolveProbeMeasurement RunWireDepthResolveProbe(const WireDepthResolveProbeContext& context) {
        WireDepthResolveProbeMeasurement measurement;
        measurement.subjectElided = context.elideSubject;
        const VkDevice device = context.device;
        if (device == VK_NULL_HANDLE || context.queue == VK_NULL_HANDLE || context.createRenderPass2 == nullptr) {
            measurement.failureReason = "no device, queue or vkCreateRenderPass2";
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
        const auto makeImage = [&](VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                   ProbeImage& out) -> Bool {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {kProbeExtent, kProbeExtent, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = samples;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
            objects.images.push_back(out.image);
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, out.image, &requirements);
            const Optional<Uint32> type = PickMemoryType(memoryProperties, requirements.memoryTypeBits,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
            if (!type) return false;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = *type;
            if (vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
            objects.memories.push_back(out.memory);
            return vkBindImageMemory(device, out.image, out.memory, 0) == VK_SUCCESS;
        };
        const auto makeView = [&](const VkImageViewCreateInfo& info, VkImageView& out) -> Bool {
            if (vkCreateImageView(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.views.push_back(out);
            return true;
        };
        const auto makeFramebuffer = [&](VkRenderPass pass, const VkImageView* views, Uint32 count,
                                         VkFramebuffer& out) -> Bool {
            VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            info.renderPass = pass;
            info.attachmentCount = count;
            info.pAttachments = views;
            info.width = info.height = kProbeExtent;
            info.layers = 1;
            if (vkCreateFramebuffer(device, &info, nullptr, &out) != VK_SUCCESS) return false;
            objects.framebuffers.push_back(out);
            return true;
        };
        // The wire's clear (WireFramebuffer.inc, ClearWireFramebuffer): a LOAD/STORE pass over every
        // aspect of the format with vkCmdClearAttachments inside it. Recorded later; built now.
        struct ClearPass {
            VkRenderPass pass = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkImageAspectFlags aspects = 0;
            Float depth = 0.0f;
            Uint32 stencil = 0;
        };
        Vector<ClearPass> clears;
        const auto makeClear = [&](const ProbeImage& image, VkFormat format, VkSampleCountFlagBits samples,
                                   Float depth, Uint32 stencil) -> Bool {
            const VkImageAspectFlags aspects = WireDepthStencilFormatAspects(format);
            VkImageView view = VK_NULL_HANDLE;
            if (!makeView(WireDepthStencilViewInfo(image.image, format, aspects, 0, 0), view)) return false;
            VkAttachmentDescription attachment{};
            attachment.format = format;
            attachment.samples = samples;
            attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &reference;
            VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            ClearPass clear;
            if (vkCreateRenderPass(device, &info, nullptr, &clear.pass) != VK_SUCCESS) return false;
            objects.passes.push_back(clear.pass);
            if (!makeFramebuffer(clear.pass, &view, 1, clear.framebuffer)) return false;
            clear.aspects = aspects;
            clear.depth = depth;
            clear.stencil = stencil;
            clears.push_back(clear);
            return true;
        };

        // THE CONTROL'S FORMAT-INDEPENDENT HALF. A kit that cannot be made leaves every control
        // un-run, which the verdict reads as INCONCLUSIVE - never as the bug.
        String kitFailure;
        if (const VkResult result = CreateWireShaderResolveKit(device, context.shaderStencilExport, objects.kit);
            result != VK_SUCCESS)
            kitFailure = format("shader resolve kit creation failed (VkResult {})", static_cast<Int>(result));

        // Readback layout: every region on a 16-byte boundary (a depth/stencil buffer copy wants a
        // multiple of 4).
        VkDeviceSize bufferBytes = 0;
        const auto reserve = [&](VkDeviceSize bytes) {
            const VkDeviceSize offset = bufferBytes;
            bufferBytes += (bytes + 15u) & ~VkDeviceSize{15u};
            return offset;
        };

        Vector<FormatWork> work;
        for (const ProbeFormat& probeFormat : kProbeFormats) {
            WireDepthResolveFormatReading reading;
            reading.name = probeFormat.name;
            reading.format = static_cast<Int32>(probeFormat.format);
            reading.samples = static_cast<Uint32>(kProbeSamples);
            const SizeT readingIndex = measurement.formats.size();
            measurement.formats.push_back(reading);
            auto& slot = measurement.formats.back();
            // A skipped format records nothing: the clears it had already built are dropped with it
            // (their images stay in `objects` and are only destroyed).
            const SizeT clearsBefore = clears.size();
            const auto skip = [&](String reason) {
                slot.skipReason = Move(reason);
                clears.resize(clearsBefore);
            };

            const VkFormat format = probeFormat.format;
            const VkImageAspectFlags aspects = WireDepthStencilFormatAspects(format);
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(context.physicalDevice, format, &formatProperties);
            constexpr VkFormatFeatureFlags kRequired =
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            if ((formatProperties.optimalTilingFeatures & kRequired) != kRequired) {
                skip("not a sampled depth/stencil attachment format here");
                continue;
            }
            VkImageFormatProperties imageProperties{};
            if (vkGetPhysicalDeviceImageFormatProperties(context.physicalDevice, format, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, kSourceUsage, 0,
                                                         &imageProperties) != VK_SUCCESS ||
                !(imageProperties.sampleCounts & kProbeSamples)) {
                skip("no 4x multisample image with the wire's attachment usage");
                continue;
            }
            if (vkGetPhysicalDeviceImageFormatProperties(context.physicalDevice, format, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, kScratchUsage, 0,
                                                         &imageProperties) != VK_SUCCESS) {
                skip("no single-sample image with the resolve scratch's usage");
                continue;
            }
            // ResolveWireDepthStencil's own test for the render-pass arm, per format.
            if (((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                 !(context.depthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) ||
                ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                 !(context.stencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))) {
                skip("the render-pass arm is not taken for this format (no SAMPLE_ZERO resolve mode)");
                continue;
            }

            FormatWork item;
            item.reading = readingIndex;
            item.format = format;
            item.aspects = aspects;
            if (!makeImage(format, kProbeSamples, kSourceUsage, item.subjectSource) ||
                !makeImage(format, kProbeSamples, kSourceUsage, item.controlSource) ||
                !makeImage(format, VK_SAMPLE_COUNT_1_BIT, kScratchUsage, item.subjectTarget)) {
                skip("image creation failed");
                continue;
            }
            if (CreateWireDepthStencilResolvePass(context.createRenderPass2, device, format, kProbeSamples,
                                                  &item.resolvePass) != VK_SUCCESS) {
                skip("the render-pass arm's vkCreateRenderPass2 failed");
                continue;
            }
            objects.passes.push_back(item.resolvePass);
            if (!makeView(WireDepthStencilViewInfo(item.subjectSource.image, format, aspects, 0, 0),
                          item.resolveViews[0]) ||
                !makeView(WireDepthStencilViewInfo(item.subjectTarget.image, format, aspects, 0, 0),
                          item.resolveViews[1]) ||
                !makeFramebuffer(item.resolvePass, item.resolveViews, 2, item.resolveFramebuffer)) {
                skip("the render-pass arm's views or framebuffer could not be created");
                continue;
            }
            if (!makeClear(item.subjectSource, format, kProbeSamples, kClearDepth, kClearStencil) ||
                !makeClear(item.controlSource, format, kProbeSamples, kClearDepth, kClearStencil) ||
                !makeClear(item.subjectTarget, format, VK_SAMPLE_COUNT_1_BIT, kSentinelDepth, kSentinelStencil)) {
                skip("the clear passes could not be created");
                continue;
            }
            // The control, one target per aspect: the shader pass's attachment is DONT_CARE on both
            // aspects, so a second aspect's pass into the same target would discard the first.
            const auto makeControl = [&](VkImageAspectFlags aspect, ControlPass& control) {
                if (!kitFailure.empty()) return;
                if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && objects.kit.stencilFragment == VK_NULL_HANDLE) return;
                if (!makeImage(format, VK_SAMPLE_COUNT_1_BIT, kScratchUsage, control.target)) return;
                if (CreateWireShaderResolveRenderPass(device, format, &control.pass) != VK_SUCCESS) return;
                objects.passes.push_back(control.pass);
                if (CreateWireShaderResolvePipeline(device, objects.kit, control.pass, aspect, &control.pipeline) !=
                    VK_SUCCESS)
                    return;
                objects.pipelines.push_back(control.pipeline);
                if (!makeView(WireDepthStencilViewInfo(item.controlSource.image, format, aspect, 0, 0),
                              control.sampledView) ||
                    !makeView(WireDepthStencilViewInfo(control.target.image, format, aspects, 0, 0),
                              control.targetView) ||
                    !makeFramebuffer(control.pass, &control.targetView, 1, control.framebuffer))
                    return;
                if (!makeClear(control.target, format, VK_SAMPLE_COUNT_1_BIT, kSentinelDepth, kSentinelStencil))
                    return;
                control.ready = true;
            };
            if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) makeControl(VK_IMAGE_ASPECT_DEPTH_BIT, item.depthControl);
            if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) makeControl(VK_IMAGE_ASPECT_STENCIL_BIT, item.stencilControl);

            if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
                item.subjectDepthOffset = reserve(kProbeTexels * DepthCopyBytes(format));
                if (item.depthControl.ready) item.depthControl.offset = reserve(kProbeTexels * DepthCopyBytes(format));
            }
            if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
                item.subjectStencilOffset = reserve(kProbeTexels);
                if (item.stencilControl.ready) item.stencilControl.offset = reserve(kProbeTexels);
            }
            slot.ran = true;
            work.push_back(item);
        }
        if (work.empty()) return fail("no format could be probed");

        // Descriptor sets for the controls, one per draw (the production shader arm's own rule).
        Uint32 controlDraws = 0;
        for (const FormatWork& item : work)
            controlDraws += (item.depthControl.ready ? 1u : 0u) + (item.stencilControl.ready ? 1u : 0u);
        if (controlDraws > 0) {
            const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, controlDraws};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool.maxSets = controlDraws;
            pool.poolSizeCount = 1;
            pool.pPoolSizes = &poolSize;
            if (vkCreateDescriptorPool(device, &pool, nullptr, &objects.descriptorPool) != VK_SUCCESS)
                return fail("vkCreateDescriptorPool failed");
            for (FormatWork& item : work) {
                for (ControlPass* control : {&item.depthControl, &item.stencilControl}) {
                    if (!control->ready) continue;
                    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    alloc.descriptorPool = objects.descriptorPool;
                    alloc.descriptorSetCount = 1;
                    alloc.pSetLayouts = &objects.kit.descriptorLayout;
                    if (vkAllocateDescriptorSets(device, &alloc, &control->set) != VK_SUCCESS)
                        return fail("vkAllocateDescriptorSets failed");
                    const VkDescriptorImageInfo sampled{objects.kit.nearest, control->sampledView,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = control->set;
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &sampled;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }

        // The host-visible readback buffer.
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        Bool bufferCoherent = false;
        {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bufferBytes;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &info, nullptr, &objects.buffer) != VK_SUCCESS)
                return fail("vkCreateBuffer failed");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, objects.buffer, &requirements);
            const Optional<Uint32> type =
                PickMemoryType(memoryProperties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (!type) return fail("no host-visible memory type for the readback buffer");
            bufferCoherent = (memoryProperties.memoryTypes[*type].propertyFlags &
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = *type;
            if (vkAllocateMemory(device, &allocate, nullptr, &bufferMemory) != VK_SUCCESS)
                return fail("vkAllocateMemory (readback) failed");
            objects.memories.push_back(bufferMemory);
            if (vkBindBufferMemory(device, objects.buffer, bufferMemory, 0) != VK_SUCCESS)
                return fail("vkBindBufferMemory failed");
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

        const VkRect2D fullRect{{0, 0}, {kProbeExtent, kProbeExtent}};
        // (1) Every image UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL, the way TransitionWireImage
        // moves a fresh image (source state from UNDEFINED, destination ALL_COMMANDS).
        for (const FormatWork& item : work) {
            const VkImage images[] = {item.subjectSource.image, item.controlSource.image, item.subjectTarget.image,
                                      item.depthControl.ready ? item.depthControl.target.image : VK_NULL_HANDLE,
                                      item.stencilControl.ready ? item.stencilControl.target.image : VK_NULL_HANDLE};
            for (const VkImage image : images) {
                if (image == VK_NULL_HANDLE) continue;
                ImageBarrier(cmd, image, item.aspects, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            }
        }
        // (2) The inputs and the sentinels, cleared the wire's way.
        for (const ClearPass& clear : clears) {
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = clear.pass;
            begin.framebuffer = clear.framebuffer;
            begin.renderArea = fullRect;
            vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
            VkClearAttachment attachment{};
            attachment.aspectMask = clear.aspects;
            attachment.clearValue.depthStencil = {clear.depth, clear.stencil};
            const VkClearRect rect{fullRect, 0, 1};
            vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
            vkCmdEndRenderPass(cmd);
        }
        // (3) What TransitionWireImage records when the layout already matches - the case of a
        // source the application cleared and then resolves.
        {
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                 &memory, 0, nullptr, 0, nullptr);
        }
        // (4) THE SUBJECT: the render-pass arm, begun and ended with no draw. Under the test-only
        // elideSubject it is not recorded at all: its target keeps the sentinel cleared in (2) and
        // the layout it was left in, which is what (6) expects either way.
        if (!context.elideSubject) {
            for (const FormatWork& item : work) {
                VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                begin.renderPass = item.resolvePass;
                begin.framebuffer = item.resolveFramebuffer;
                begin.renderArea = fullRect;
                vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(cmd);
            }
        }
        // (5) THE CONTROL: the shader arm on the twin source, which TransitionWireImage moves to
        // SHADER_READ_ONLY_OPTIMAL first (both aspects of a packed format).
        for (const FormatWork& item : work) {
            if (!item.depthControl.ready && !item.stencilControl.ready) continue;
            ImageBarrier(cmd, item.controlSource.image, item.aspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            for (const ControlPass* control : {&item.depthControl, &item.stencilControl}) {
                if (!control->ready) continue;
                RecordWireShaderResolveDraw(cmd, objects.kit, control->pass, control->pipeline, control->framebuffer,
                                            control->set, {kProbeExtent, kProbeExtent}, fullRect);
            }
        }
        // (6) Every target to TRANSFER_SRC with the barrier ResolveWireDepthStencil puts after its
        // resolve (a depth/stencil resolve writes in COLOR_ATTACHMENT_OUTPUT), then one copy per aspect.
        const auto copyOut = [&](VkImage image, VkImageAspectFlags aspect, VkDeviceSize offset) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {aspect, 0, 0, 1};
            copy.imageExtent = {kProbeExtent, kProbeExtent, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, objects.buffer, 1, &copy);
        };
        const auto toTransfer = [&](VkImage image, VkImageAspectFlags aspects) {
            ImageBarrier(cmd, image, aspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        };
        for (const FormatWork& item : work) {
            toTransfer(item.subjectTarget.image, item.aspects);
            if (item.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                copyOut(item.subjectTarget.image, VK_IMAGE_ASPECT_DEPTH_BIT, item.subjectDepthOffset);
            if (item.aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
                copyOut(item.subjectTarget.image, VK_IMAGE_ASPECT_STENCIL_BIT, item.subjectStencilOffset);
            if (item.depthControl.ready) {
                toTransfer(item.depthControl.target.image, item.aspects);
                copyOut(item.depthControl.target.image, VK_IMAGE_ASPECT_DEPTH_BIT, item.depthControl.offset);
            }
            if (item.stencilControl.ready) {
                toTransfer(item.stencilControl.target.image, item.aspects);
                copyOut(item.stencilControl.target.image, VK_IMAGE_ASPECT_STENCIL_BIT, item.stencilControl.offset);
            }
        }
        {
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = objects.buffer;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &barrier, 0, nullptr);
        }
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
            // The submission may still be executing: destroying anything it names, or idle-waiting
            // the device, is the hang the bound exists to prevent. Everything leaks.
            leak = true;
            measurement.fenceWaitTimedOut = true;
            return fail("the probe's fence did not signal within 5 s");
        }
        if (waited != VK_SUCCESS) return fail(format("vkWaitForFences failed (VkResult {})", static_cast<Int>(waited)));

        void* mapped = nullptr;
        if (vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
            return fail("vkMapMemory failed");
        if (!bufferCoherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = bufferMemory;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        const Uint8* bytes = static_cast<const Uint8*>(mapped);
        const auto tally = [&](WireDepthResolveAspectReading& aspect, VkDeviceSize offset, Uint32 texelBytes,
                               Bool depth, VkFormat format, Bool subject) {
            Uint32 matches = 0, sentinels = 0, first = 0;
            for (Uint32 i = 0; i < kProbeTexels; ++i) {
                const Uint8* texel = bytes + offset + static_cast<VkDeviceSize>(i) * texelBytes;
                const Uint32 word = depth ? ReadDepthWord(format, texel) : static_cast<Uint32>(*texel);
                if (i == 0) first = word;
                if (WordDistance(word, aspect.expected) <= aspect.tolerance) ++matches;
                if (WordDistance(word, aspect.sentinel) <= aspect.tolerance) ++sentinels;
            }
            if (subject) {
                aspect.renderPassMatches = matches;
                aspect.renderPassSentinels = sentinels;
                aspect.renderPassFirst = first;
            } else {
                aspect.shaderRan = true;
                aspect.shaderMatches = matches;
                aspect.shaderFirst = first;
            }
        };
        for (const FormatWork& item : work) {
            WireDepthResolveFormatReading& reading = measurement.formats[item.reading];
            if (item.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
                auto& aspect = reading.depth;
                aspect.measured = true;
                aspect.texels = kProbeTexels;
                aspect.expected = EncodeDepth(item.format, kClearDepth);
                aspect.sentinel = EncodeDepth(item.format, kSentinelDepth);
                aspect.tolerance = IsFloatDepth(item.format) ? 0u : 1u;
                tally(aspect, item.subjectDepthOffset, DepthCopyBytes(item.format), true, item.format, true);
                if (item.depthControl.ready)
                    tally(aspect, item.depthControl.offset, DepthCopyBytes(item.format), true, item.format, false);
            }
            if (item.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
                auto& aspect = reading.stencil;
                aspect.measured = true;
                aspect.texels = kProbeTexels;
                aspect.expected = kClearStencil;
                aspect.sentinel = kSentinelStencil;
                aspect.tolerance = 0;
                tally(aspect, item.subjectStencilOffset, 1, false, item.format, true);
                if (item.stencilControl.ready)
                    tally(aspect, item.stencilControl.offset, 1, false, item.format, false);
            }
        }
        vkUnmapMemory(device, bufferMemory);
        measurement.ran = true;
        if (!kitFailure.empty()) measurement.failureReason = kitFailure;
        return measurement;
    }

    WireDepthResolveProbeMeasurement RunWireDepthResolveProbeOnThrowawayDevice(
        PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkInstance instance, VkPhysicalDevice physicalDevice,
        Uint32 graphicsQueueFamilyIndex, const Vector<VkExtensionProperties>& deviceExtensions,
        Bool& renderPassArmAvailable, Bool& shaderStencilExport) {
        WireDepthResolveProbeMeasurement measurement;
        const auto has = [&](const char* name) {
            for (const VkExtensionProperties& extension : deviceExtensions)
                if (std::strcmp(extension.extensionName, name) == 0) return true;
            return false;
        };
        renderPassArmAvailable = has(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) &&
                                 has(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        shaderStencilExport = has(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        if (!renderPassArmAvailable) {
            measurement.failureReason = "no VK_KHR_create_renderpass2 + VK_KHR_depth_stencil_resolve";
            return measurement;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
        if (getProperties2 == nullptr)
            getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
        const auto createDevice =
            reinterpret_cast<PFN_vkCreateDevice>(getInstanceProcAddr(instance, "vkCreateDevice"));
        if (properties.apiVersion < VK_API_VERSION_1_1 || getProperties2 == nullptr || createDevice == nullptr) {
            renderPassArmAvailable = false;
            measurement.failureReason = "a pre-1.1 device, or no vkGetPhysicalDeviceProperties2 / vkCreateDevice";
            return measurement;
        }
        VkPhysicalDeviceDepthStencilResolveProperties resolveProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &resolveProperties;
        getProperties2(physicalDevice, &properties2);

        // The renderer's own extension set for the two arms (VulkanRenderer.cpp,
        // CreateLogicalDeviceAndQueues), nothing more.
        const Float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        Vector<const char*> extensions{VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
                                       VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME};
        if (shaderStencilExport) extensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<Uint32>(extensions.size());
        deviceInfo.ppEnabledExtensionNames = extensions.data();
        VkDevice device = VK_NULL_HANDLE;
        if (const VkResult result = createDevice(physicalDevice, &deviceInfo, nullptr, &device);
            result != VK_SUCCESS || device == VK_NULL_HANDLE) {
            measurement.failureReason = format("vkCreateDevice failed (VkResult {})", static_cast<Int>(result));
            return measurement;
        }
        WireDepthResolveProbeContext context;
        context.physicalDevice = physicalDevice;
        context.device = device;
        vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &context.queue);
        context.queueFamilyIndex = graphicsQueueFamilyIndex;
        context.createRenderPass2 =
            reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetDeviceProcAddr(device, "vkCreateRenderPass2KHR"));
        context.depthResolveModes = resolveProperties.supportedDepthResolveModes;
        context.stencilResolveModes = resolveProperties.supportedStencilResolveModes;
        context.shaderStencilExport = shaderStencilExport;
        measurement = RunWireDepthResolveProbe(context);
        // A timed-out probe leaked its children on purpose; destroying their device would block
        // exactly where the bound says not to.
        if (!measurement.fenceWaitTimedOut) vkDestroyDevice(device, nullptr);
        return measurement;
    }
} // namespace MobileGL::MG_Backend::DirectVulkan
#endif
