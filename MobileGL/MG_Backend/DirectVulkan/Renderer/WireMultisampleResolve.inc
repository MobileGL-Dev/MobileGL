// Included by VulkanRenderer.cpp inside the backend namespace, after WireDepthMipmap.inc.
// The native multisample depth/stencil resolve owns no frontend object and never invokes the
// GL shader compiler.
//
// WHY THIS FILE EXISTS. vkCmdResolveImage is COLOUR ONLY. The one way Vulkan resolves a depth
// or stencil aspect without a shader is a render pass with a depth/stencil resolve attachment,
// which needs VK_KHR_create_renderpass2 and VK_KHR_depth_stencil_resolve - and until this
// package a device without either killed the session at
// `Magma:multisample-depth-resolve-capability` the first time an application resolved a
// multisample depth buffer, which is what glBlitFramebuffer(GL_DEPTH_BUFFER_BIT) from a
// multisampled read framebuffer IS. CONTRACT-P7 §3.2's `multisample-blit-aspect` row names both
// arms; this is the second one.
//
// SAMPLE ZERO IN BOTH ARMS. The extension arm asks for VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, so the
// shader arm fetches sample 0 - not an average. Two arms that answer differently on two devices
// would make "the wire arm is observably the monolith's" untestable, and GL 4.6 core 18.3.1
// requires a SELECTED sample for stencil anyway.
#if MOBILEGL_BUILD_DISAGGREGATED
#include "WireMultisampleResolveSpirv.h"

    struct VulkanRenderer::WireMultisampleResolveResources {
        struct Target {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = 0; // the ONE aspect this pipeline writes
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule depthFragment = VK_NULL_HANDLE, stencilFragment = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
        Vector<Target> targets;
    };

    void VulkanRenderer::DestroyWireMultisampleResolveResources() {
        if (!m_wireMultisampleResolveResources) return;
        // Shutdown calls this after vkDeviceWaitIdle and after all per-submit descriptor
        // pools/framebuffers/views have been collected.
        auto& cache = *m_wireMultisampleResolveResources;
        for (const auto& target : cache.targets) {
            vkDestroyPipeline(m_device, target.pipeline, nullptr);
            vkDestroyRenderPass(m_device, target.renderPass, nullptr);
        }
        vkDestroySampler(m_device, cache.nearest, nullptr);
        vkDestroyPipelineLayout(m_device, cache.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, cache.descriptorLayout, nullptr);
        vkDestroyShaderModule(m_device, cache.vertex, nullptr);
        vkDestroyShaderModule(m_device, cache.depthFragment, nullptr);
        vkDestroyShaderModule(m_device, cache.stencilFragment, nullptr);
        delete m_wireMultisampleResolveResources;
        m_wireMultisampleResolveResources = nullptr;
    }

    // ONE ASPECT, source -> resolved, both 2D images of the same format and extent, the source
    // multisampled and the destination not. Returns false for every shape it cannot express,
    // which the caller turns into a decline; it never ends the session.
    Bool VulkanRenderer::ResolveWireDepthStencilWithShader(WireImage source, WireImage resolved,
        VkImageAspectFlags aspect, GLint sx0, GLint sy0, Uint32 width, Uint32 height) {
        if (!source.image || !resolved.image || !width || !height) return false;
        if (aspect != VK_IMAGE_ASPECT_DEPTH_BIT && aspect != VK_IMAGE_ASPECT_STENCIL_BIT) return false;
        // A fragment shader cannot write the stencil aspect at all without
        // VK_EXT_shader_stencil_export. §3.2: decline stencil-only rather than produce a level
        // whose stencil is whatever the DONT_CARE left there.
        if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && !m_wireShaderStencilExport) return false;
        if (source.samples == VK_SAMPLE_COUNT_1_BIT) return false;
        if (source.format != resolved.format) return false;
        // SAMPLED is asked for by every attachment VkTextureManager creates; a device that
        // declined it would make the descriptor write below undefined, not failed.
        if (!(source.usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT)) return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, source.format, &properties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((properties.optimalTilingFeatures & required) != required) return false;

        VkImageAspectFlags formatAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (source.format == VK_FORMAT_S8_UINT) formatAspects = VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (source.format == VK_FORMAT_D16_UNORM_S8_UINT || source.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                 source.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            formatAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!(formatAspects & aspect)) return false;

        if (!m_wireMultisampleResolveResources) {
            m_wireMultisampleResolveResources = new WireMultisampleResolveResources;
            auto& cache = *m_wireMultisampleResolveResources;
            const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = bytes;
                info.pCode = code;
                VK_VERIFY(vkCreateShaderModule(m_device, &info, nullptr, &module));
            };
            shader(kWireMultisampleResolveVertexSpirv, sizeof(kWireMultisampleResolveVertexSpirv), cache.vertex);
            shader(kWireMultisampleDepthResolveFragmentSpirv,
                   sizeof(kWireMultisampleDepthResolveFragmentSpirv), cache.depthFragment);
            // THE STENCIL MODULE IS ONLY CREATED WHERE IT CAN RUN: its SPIR-V declares the
            // StencilExportEXT capability, and handing a module with an unsupported capability
            // to vkCreateShaderModule is undefined behaviour - not an error this could catch.
            if (m_wireShaderStencilExport)
                shader(kWireMultisampleStencilResolveFragmentSpirv,
                       sizeof(kWireMultisampleStencilResolveFragmentSpirv), cache.stencilFragment);
            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout.bindingCount = 1;
            layout.pBindings = &binding;
            VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &cache.descriptorLayout));
            // NO PUSH CONSTANT RANGE AT ALL: the pass reads its coordinate from gl_FragCoord and
            // its rectangle from the scissor, so neither stage takes an input.
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayout.setLayoutCount = 1;
            pipelineLayout.pSetLayouts = &cache.descriptorLayout;
            VK_VERIFY(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &cache.pipelineLayout));
            // NEAREST: texelFetch ignores the sampler's filter, and most devices do not
            // advertise linear filtering for a depth format anyway. The sampler exists only
            // because GLSL's sampler2DMS is a COMBINED image sampler.
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
            sampler.maxLod = 0.0f;
            VK_VERIFY(vkCreateSampler(m_device, &sampler, nullptr, &cache.nearest));
        }
        auto& cache = *m_wireMultisampleResolveResources;
        const VkShaderModule fragment = aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? cache.stencilFragment
                                                                              : cache.depthFragment;
        if (fragment == VK_NULL_HANDLE) return false;

        WireMultisampleResolveResources::Target* target = nullptr;
        for (auto& entry : cache.targets)
            if (entry.format == resolved.format && entry.aspect == aspect) { target = &entry; break; }
        if (!target) {
            cache.targets.emplace_back();
            target = &cache.targets.back();
            target->format = resolved.format;
            target->aspect = aspect;
            VkAttachmentDescription attachment{};
            attachment.format = resolved.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            // DONT_CARE on BOTH aspects: the pass writes every texel of the scissored rect for
            // the aspect it owns, and the caller copies only that aspect out. The other aspect
            // of a packed format is left undefined on purpose - it is never read, and LOADing
            // it would order this pass behind a write nobody is waiting for.
            attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            const VkAttachmentReference depthStencil{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.pDepthStencilAttachment = &depthStencil;
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
            VkPipelineDepthStencilStateCreateInfo depthStencilState{
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthStencilState.minDepthBounds = 0.0f;
            depthStencilState.maxDepthBounds = 1.0f;
            if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
                // ALWAYS + write: gl_FragDepth is the answer, and a comparison against whatever
                // DONT_CARE left behind would discard some of it.
                depthStencilState.depthTestEnable = VK_TRUE;
                depthStencilState.depthWriteEnable = VK_TRUE;
                depthStencilState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            } else {
                // REPLACE on every outcome, compare ALWAYS, full write mask: that is what makes
                // the shader-exported reference the value that lands. The static `reference` is
                // overridden by gl_FragStencilRefARB, which is the whole point of the extension.
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
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipeline.stageCount = 2;
            pipeline.pStages = stages;
            pipeline.pVertexInputState = &vertex;
            pipeline.pInputAssemblyState = &assembly;
            pipeline.pViewportState = &viewport;
            pipeline.pRasterizationState = &raster;
            pipeline.pMultisampleState = &samples;
            pipeline.pDepthStencilState = &depthStencilState;
            pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = cache.pipelineLayout;
            pipeline.renderPass = target->renderPass;
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                                          &target->pipeline) != VK_SUCCESS) {
                vkDestroyRenderPass(m_device, target->renderPass, nullptr);
                cache.targets.pop_back();
                return false;
            }
        }

        m_textureManager->FlushPendingUploads();
        TransitionWireImage(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        TransitionWireImage(resolved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = source.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = source.format;
        // ONE ASPECT on the sampled view: a depth/stencil image can only be SAMPLED through a
        // view that names exactly one of its aspects, which is also what makes the stencil half
        // a usampler2DMS and the depth half a sampler2DMS.
        viewInfo.subresourceRange = {aspect, source.level, 1, source.layer, 1};
        VkImageView sourceView = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &sourceView));
        viewInfo.image = resolved.image;
        // BOTH aspects on the attachment view: a depth/stencil ATTACHMENT names every aspect
        // its format has, whichever one the pipeline writes.
        viewInfo.subresourceRange = {formatAspects, 0, 1, 0, 1};
        VkImageView targetView = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &targetView));

        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = target->renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &targetView;
        framebufferInfo.width = resolved.extent.width;
        framebufferInfo.height = resolved.extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer));

        // Each in-flight pass owns its descriptor set, for the colour blit's reason: updating a
        // cached set would rewrite the source view of an older queued pass.
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
        const VkDescriptorImageInfo sampled{cache.nearest, sourceView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        auto& frame = m_frameContext.GetCurrent();
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = target->renderPass;
        begin.framebuffer = framebuffer;
        // The RECTANGLE is the render area, exactly as the extension arm's renderArea is - the
        // scratch outside it is never read.
        begin.renderArea = {{sx0, sy0}, {width, height}};
        vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, target->pipeline);
        // THE VIEWPORT IS THE WHOLE ATTACHMENT AND THE SCISSOR IS THE RECT, and the two are not
        // interchangeable here: gl_FragCoord is what the fragment shader fetches by, so the
        // clip-space-to-attachment mapping has to be the identity for every rectangle. A
        // viewport shrunk onto the rect would slide the whole image instead of moving a window
        // over it.
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(resolved.extent.width),
                                  static_cast<float>(resolved.extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{sx0, sy0}, {width, height}};
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                cache.pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.commandBuffer);
        ResetDynamicStateShadow();
        InvalidatePipelineMemo();
        // This native pass binds its own set/layout without going through the application's
        // descriptor binder. Its next fast path must rebind.
        if (m_uniformManager) m_uniformManager->OnCommandBufferBoundary();
        WireRetiredObjects retired{};
        retired.framebuffers.push_back(framebuffer);
        retired.imageViews = {sourceView, targetView};
        retired.descriptorPools.push_back(descriptorPool);
        RetireWireObjects(Move(retired));
        return true;
    }
#endif
