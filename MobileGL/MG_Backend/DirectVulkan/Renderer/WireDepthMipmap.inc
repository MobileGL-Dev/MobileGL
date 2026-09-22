// Included by VulkanRenderer.cpp inside the backend namespace, after WireColorBlit.inc.
// The native depth mip owns no frontend object and never invokes the GL shader compiler.
//
// WHY THIS FILE EXISTS AT ALL. vkCmdBlitImage is the ordinary way to halve a mip, and for a
// depth format it is often not available: VK_FORMAT_FEATURE_BLIT_DST is optional for depth,
// and the monolith arm has carried a shader fallback for exactly that case since before the
// split (VulkanRenderer.cpp's GenerateDepthMipmapWithShader). The wire arm had no such
// fallback, so every depth mip the device could not blit died as
// `Magma:mipmap-shader-format-or-shape`, and a DEPTH|STENCIL texture died one guard earlier
// as `Magma:depth-stencil-mipmap@P7` whether or not it could be blitted.
//
// The monolith fallback is not reusable here and that is the whole of CONTRACT-P7 §5.1 (A):
// it builds a GL ProgramObject, two GL ShaderObjects and a GL sampler, binds through the
// uniform manager's global UBO, and writes uSrcTexelSize as a default-block uniform - four
// frontend objects and a GL compiler on the server's apply thread. The baked program below is
// the same shader with the same arithmetic, driven by a push constant and its own descriptor
// set, which is what lets the server generate a depth mip while owning nothing of the client.
#if MOBILEGL_BUILD_DISAGGREGATED
#include "WireDepthMipmapSpirv.h"

    struct VulkanRenderer::WireDepthMipmapResources {
        struct Target {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
        Vector<Target> targets;
    };

    void VulkanRenderer::DestroyWireDepthMipmapResources() {
        if (!m_wireDepthMipmapResources) return;
        // Shutdown calls this after vkDeviceWaitIdle and after all per-submit
        // descriptor pools/framebuffers/views have been collected.
        auto& cache = *m_wireDepthMipmapResources;
        for (const auto& target : cache.targets) {
            vkDestroyPipeline(m_device, target.pipeline, nullptr);
            vkDestroyRenderPass(m_device, target.renderPass, nullptr);
        }
        vkDestroySampler(m_device, cache.nearest, nullptr);
        vkDestroyPipelineLayout(m_device, cache.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, cache.descriptorLayout, nullptr);
        vkDestroyShaderModule(m_device, cache.vertex, nullptr);
        vkDestroyShaderModule(m_device, cache.fragment, nullptr);
        delete m_wireDepthMipmapResources;
        m_wireDepthMipmapResources = nullptr;
    }

    // ONE LEVEL, source -> destination, both depth subresources of the same image. The shape
    // mirrors BlitWireColorImage deliberately: the same lazily built module pair, the same
    // per-format render pass/pipeline cache, the same per-submit descriptor pool, and the
    // same GENERAL-on-both-sides layout rule for a same-image pass, so the two native passes
    // cannot disagree about how an image is handed between them.
    Bool VulkanRenderer::GenerateWireDepthMipLevel(WireImage source, WireImage destination) {
        const Bool sameImage = source.image == destination.image;
        if (!source.image || !destination.image) return false;
        if (sameImage && source.level == destination.level && source.layer == destination.layer)
            return false;
        if (source.isDefault || destination.isDefault) return false;
        if (source.samples != VK_SAMPLE_COUNT_1_BIT || destination.samples != VK_SAMPLE_COUNT_1_BIT)
            return false;
        if (source.is3D || destination.is3D) return false;
        if (source.format != destination.format) return false;
        // DEPTH ONLY. A combined depth/stencil image can be sampled for its depth and
        // rendered as a depth attachment, but this pass writes gl_FragDepth and NOTHING
        // else - the stencil aspect would come out of the render pass undefined, which is
        // not what GL's glGenerateMipmap promises for the level it just produced. The
        // caller declines that shape rather than producing half a level.
        if (source.aspect != VK_IMAGE_ASPECT_DEPTH_BIT ||
            destination.aspect != VK_IMAGE_ASPECT_DEPTH_BIT) return false;
        if (!source.extent.width || !source.extent.height ||
            !destination.extent.width || !destination.extent.height) return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, source.format, &properties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((properties.optimalTilingFeatures & required) != required) return false;

        // The baked block's layout, and the offsets are not a guess: the generated header
        // decorates its members at 0 / 16 / 32 / 40, which is where a vec4, a vec4, an int
        // and an 8-byte-aligned ivec2 land. The pad is what makes the C++ agree.
        struct Constants {
            float srcRect[4];
            float dstRect[4];
            Int32 surfaceTransform;
            Int32 reservedForIvec2Alignment;
            Int32 srcTexelSize[2];
        };
        static_assert(sizeof(Constants) == 48);
        static_assert(offsetof(Constants, srcTexelSize) == 40);

        if (!m_wireDepthMipmapResources) {
            m_wireDepthMipmapResources = new WireDepthMipmapResources;
            auto& cache = *m_wireDepthMipmapResources;
            const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = bytes;
                info.pCode = code;
                VK_VERIFY(vkCreateShaderModule(m_device, &info, nullptr, &module));
            };
            shader(kWireDepthMipmapVertexSpirv, sizeof(kWireDepthMipmapVertexSpirv), cache.vertex);
            shader(kWireDepthMipmapFragmentSpirv, sizeof(kWireDepthMipmapFragmentSpirv), cache.fragment);
            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout.bindingCount = 1;
            layout.pBindings = &binding;
            VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &cache.descriptorLayout));
            // BOTH STAGES ON ONE RANGE: the block is declared identically in the vertex and
            // fragment sources (a push-constant block must be), the vertex stage reads the
            // rects and the fragment stage reads srcTexelSize.
            const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(Constants)};
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayout.setLayoutCount = 1;
            pipelineLayout.pSetLayouts = &cache.descriptorLayout;
            pipelineLayout.pushConstantRangeCount = 1;
            pipelineLayout.pPushConstantRanges = &push;
            VK_VERIFY(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &cache.pipelineLayout));
            // NEAREST, and it is not a default: the fragment shader does its own 2x2 box with
            // texelFetch, so any filtering the sampler added would be applied twice. It also
            // keeps the sampler legal on a device that does not advertise linear filtering for
            // depth formats, which is most of them.
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
            sampler.maxLod = 0.0f;
            VK_VERIFY(vkCreateSampler(m_device, &sampler, nullptr, &cache.nearest));
        }
        auto& cache = *m_wireDepthMipmapResources;

        const VkImageLayout sourceLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL
                                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkImageLayout destinationLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL
                                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        WireDepthMipmapResources::Target* target = nullptr;
        for (auto& entry : cache.targets) {
            if (entry.format == destination.format && entry.layout == destinationLayout) {
                target = &entry;
                break;
            }
        }
        if (!target) {
            cache.targets.emplace_back();
            target = &cache.targets.back();
            target->format = destination.format;
            target->layout = destinationLayout;
            VkAttachmentDescription attachment{};
            attachment.format = destination.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            // DONT_CARE, not LOAD: the pass writes gl_FragDepth at every texel of the level
            // it is producing, so there is nothing in the destination worth loading, and a
            // level that has just been grown has no defined contents to load anyway.
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = attachment.finalLayout = destinationLayout;
            const VkAttachmentReference depth{0, destinationLayout};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &depth;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 1;
            pass.pAttachments = &attachment;
            pass.subpassCount = 1;
            pass.pSubpasses = &subpass;
            VK_VERIFY(vkCreateRenderPass(m_device, &pass, nullptr, &target->renderPass));
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto& stage : stages) {
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stage.pName = "main";
            }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = cache.vertex;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = cache.fragment;
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
            // ALWAYS + write enabled: the fragment shader's gl_FragDepth is the answer, and a
            // comparison against whatever DONT_CARE left behind would discard some of it.
            VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            depthStencil.minDepthBounds = 0.0f;
            depthStencil.maxDepthBounds = 1.0f;
            // No colour attachment in this render pass, so no attachment states either.
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamicStates;
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount = 2;
            pipeline.pStages = stages;
            pipeline.pVertexInputState = &vertex;
            pipeline.pInputAssemblyState = &assembly;
            pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &raster;
            pipeline.pMultisampleState = &samples;
            pipeline.pDepthStencilState = &depthStencil;
            pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = cache.pipelineLayout;
            pipeline.renderPass = target->renderPass;
            VK_VERIFY(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &target->pipeline));
        }

        m_textureManager->FlushPendingUploads();
        // Distinct mip views of the same image are legal shader input and output at once.
        // GENERAL keeps the whole-image layout tracker truthful; the transition also orders
        // the previous level's attachment write against this level's sample of it.
        TransitionWireImage(source, sourceLayout);
        if (sameImage) destination.layout = source.layout;
        else TransitionWireImage(destination, destinationLayout);
        const auto makeView = [&](const WireImage& image) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = image.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, image.level, 1, image.layer, 1};
            VkImageView handle = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateImageView(m_device, &view, nullptr, &handle));
            return handle;
        };
        const VkImageView sourceView = makeView(source), destinationView = makeView(destination);
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = target->renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &destinationView;
        framebufferInfo.width = destination.extent.width;
        framebufferInfo.height = destination.extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer));
        // Each in-flight pass owns its descriptor set, for the colour blit's reason: updating
        // a cached set would rewrite the source view of an older queued pass.
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        VK_VERIFY(vkCreateDescriptorPool(m_device, &pool, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &cache.descriptorLayout;
        VK_VERIFY(vkAllocateDescriptorSets(m_device, &alloc, &descriptorSet));
        const VkDescriptorImageInfo sampled{cache.nearest, sourceView, sourceLayout};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        // The destination rectangle is flipped exactly as BlitWireColorImage flips it for an
        // offscreen target, because it shares that file's vertex shader and therefore its
        // window-Y flip. With the flip undone, destination row r samples texCoord.y
        // (r + 0.5) / dstHeight, and the fragment's ivec2(texCoord * srcTexelSize) lands on
        // the same source texels the monolith arm's fullscreen triangle lands on - which is
        // the point: §3.2 retires a refusal by making the wire arm observably the monolith's,
        // half-texel offset included.
        const Constants constants{
            {0.0f, 0.0f, 1.0f, 1.0f},
            {0.0f, 1.0f, 1.0f, -1.0f},
            0, 0,
            {static_cast<Int32>(source.extent.width), static_cast<Int32>(source.extent.height)}};
        auto& frame = m_frameContext.GetCurrent();
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = target->renderPass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, destination.extent};
        vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, target->pipeline);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(destination.extent.width),
                                  static_cast<float>(destination.extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, destination.extent};
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                cache.pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, cache.pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(constants), &constants);
        vkCmdDraw(frame.commandBuffer, 6, 1, 0, 0);
        vkCmdEndRenderPass(frame.commandBuffer);
        ResetDynamicStateShadow();
        InvalidatePipelineMemo();
        // This native pass binds its own set/layout without going through the application's
        // descriptor binder. Its next fast path must rebind.
        if (m_uniformManager) m_uniformManager->OnCommandBufferBoundary();
        WireRetiredObjects retired{};
        retired.framebuffers.push_back(framebuffer);
        retired.imageViews = {sourceView, destinationView};
        retired.descriptorPools.push_back(descriptorPool);
        RetireWireObjects(Move(retired));
        return true;
    }
#endif
