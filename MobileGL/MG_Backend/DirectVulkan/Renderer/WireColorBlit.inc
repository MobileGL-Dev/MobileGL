// Included by VulkanRenderer.cpp inside the backend namespace, after WireFramebuffer.inc.
// The native blit owns no frontend object and never invokes the GL shader compiler.
#if MOBILEGL_BUILD_DISAGGREGATED
#include "WireColorBlitSpirv.h"

    struct VulkanRenderer::WireColorBlitResources {
        struct Target {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
        Vector<Target> targets;
    };

    void VulkanRenderer::DestroyWireColorBlitResources() {
        if (!m_wireColorBlitResources) return;
        // Shutdown calls this after vkDeviceWaitIdle and after all per-submit
        // descriptor pools/framebuffers/views have been collected.
        auto& cache = *m_wireColorBlitResources;
        for (const auto& target : cache.targets) {
            vkDestroyPipeline(m_device, target.pipeline, nullptr);
            vkDestroyRenderPass(m_device, target.renderPass, nullptr);
        }
        vkDestroySampler(m_device, cache.nearest, nullptr);
        vkDestroySampler(m_device, cache.linear, nullptr);
        vkDestroyPipelineLayout(m_device, cache.pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_device, cache.descriptorLayout, nullptr);
        vkDestroyShaderModule(m_device, cache.vertex, nullptr);
        vkDestroyShaderModule(m_device, cache.fragment, nullptr);
        delete m_wireColorBlitResources;
        m_wireColorBlitResources = nullptr;
    }

    Bool VulkanRenderer::BlitWireColorToDefault(WireImage source, WireImage destination,
        GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter) {
        if (!destination.isDefault) return false;
        return BlitWireColorImage(source, destination, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filter, false);
    }

    Bool VulkanRenderer::BlitWireColorImage(WireImage source, WireImage destination,
        GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter, Bool mipmap) {
        const Bool sameImage = source.image == destination.image;
        if (source.isDefault || (sameImage && source.level == destination.level && source.layer == destination.layer) ||
            source.samples != VK_SAMPLE_COUNT_1_BIT || destination.samples != VK_SAMPLE_COUNT_1_BIT ||
            source.is3D || destination.is3D || (source.viewType != VK_IMAGE_VIEW_TYPE_2D &&
                            source.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
            (destination.viewType != VK_IMAGE_VIEW_TYPE_2D && destination.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY) ||
            !vkuFormatIsSampledFloat(source.format)) return false;
        if (sx0 == sx1 || sy0 == sy1 || dx0 == dx1 || dy0 == dy1) return true;
        if (!source.extent.width || !source.extent.height ||
            !destination.extent.width || !destination.extent.height) return false;
        const auto transform = destination.isDefault ? m_swapchainObject.GetPreTransform() :
                                                       VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        if (transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
            transform != VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR &&
            transform != VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR &&
            transform != VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) return false;
        VkFormatProperties sourceProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, source.format, &sourceProperties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            (filter == GL_LINEAR ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
        if ((sourceProperties.optimalTilingFeatures & required) != required) return false;
        // Mip generation samples and writes the texture's own color space;
        // GL_FRAMEBUFFER_SRGB only controls framebuffer operations.
        if (!mipmap) destination.format = ResolveSrgbAttachmentWriteFormat(destination.format,
            MGB_CTX->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb));
        VkFormatProperties destinationProperties{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, destination.format, &destinationProperties);
        if (!(destinationProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
        const VkImageLayout sourceLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkImageLayout destinationLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        struct Constants {
            float srcRect[4];
            float dstRect[4];
            Int32 surfaceTransform;
        };
        static_assert(sizeof(Constants) == 36);
        if (!m_wireColorBlitResources) {
            m_wireColorBlitResources = new WireColorBlitResources;
            auto& cache = *m_wireColorBlitResources;
            const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = bytes;
                info.pCode = code;
                VK_VERIFY(vkCreateShaderModule(m_device, &info, nullptr, &module));
            };
            shader(kWireColorBlitVertexSpirv, sizeof(kWireColorBlitVertexSpirv), cache.vertex);
            shader(kWireColorBlitFragmentSpirv, sizeof(kWireColorBlitFragmentSpirv), cache.fragment);
            const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                       1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layout.bindingCount = 1;
            layout.pBindings = &binding;
            VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &cache.descriptorLayout));
            const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Constants)};
            VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayout.setLayoutCount = 1;
            pipelineLayout.pSetLayouts = &cache.descriptorLayout;
            pipelineLayout.pushConstantRangeCount = 1;
            pipelineLayout.pPushConstantRanges = &push;
            VK_VERIFY(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &cache.pipelineLayout));
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
            sampler.maxLod = 0.0f;
            VK_VERIFY(vkCreateSampler(m_device, &sampler, nullptr, &cache.nearest));
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            VK_VERIFY(vkCreateSampler(m_device, &sampler, nullptr, &cache.linear));
        }
        auto& cache = *m_wireColorBlitResources;
        WireColorBlitResources::Target* target = nullptr;
        for (auto& entry : cache.targets) {
            if (entry.format == destination.format && entry.layout == destinationLayout) { target = &entry; break; }
        }
        if (!target) {
            cache.targets.emplace_back();
            target = &cache.targets.back();
            target->format = destination.format;
            target->layout = destinationLayout;
            VkAttachmentDescription attachment{};
            attachment.format = destination.format;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = attachment.finalLayout = destinationLayout;
            const VkAttachmentReference color{0, destinationLayout};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
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
            VkPipelineColorBlendAttachmentState colorMask{};
            colorMask.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &colorMask;
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
            pipeline.pColorBlendState = &blend;
            pipeline.pDynamicState = &dynamic;
            pipeline.layout = cache.pipelineLayout;
            pipeline.renderPass = target->renderPass;
            VK_VERIFY(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &target->pipeline));
        }

        m_textureManager->FlushPendingUploads();
        // Distinct mip/layer views of the same image are legal shader input and
        // output. GENERAL keeps the whole-image layout tracker truthful; the
        // transition also orders the previous mip's attachment write.
        TransitionWireImage(source, sourceLayout);
        if (sameImage) destination.layout = source.layout;
        else TransitionWireImage(destination, destinationLayout);
        const auto makeView = [&](const WireImage& image) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = image.format;
            // A layered framebuffer blit reads its first layer, as the native path does.
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, image.level, 1, image.layer, 1};
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
        // Each in-flight blit owns its descriptor set. Updating a cached set
        // would rewrite the source view of an older queued blit.
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
        const VkDescriptorImageInfo sampled{filter == GL_LINEAR ? cache.linear : cache.nearest,
                                           sourceView, sourceLayout};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        const float sw = static_cast<float>(source.extent.width), sh = static_cast<float>(source.extent.height);
        const Bool quarterTurn = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                 transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        const float dw = static_cast<float>(quarterTurn ? destination.extent.height : destination.extent.width);
        const float dh = static_cast<float>(quarterTurn ? destination.extent.width : destination.extent.height);
        // The shared vertex shader flips window Y. Offscreen images keep the
        // source row order, so undo that flip in the destination rectangle.
        if (!destination.isDefault) {
            dy0 = static_cast<GLint>(destination.extent.height) - dy0;
            dy1 = static_cast<GLint>(destination.extent.height) - dy1;
        }
        const Constants constants{{sx0 / sw, sy0 / sh, (static_cast<float>(sx1) - sx0) / sw,
                                    (static_cast<float>(sy1) - sy0) / sh},
                                  {dx0 / dw, dy0 / dh, (static_cast<float>(dx1) - dx0) / dw,
                                    (static_cast<float>(dy1) - dy0) / dh},
                                  static_cast<Int32>(ToBlitSurfaceTransform(transform))};
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
        vkCmdPushConstants(frame.commandBuffer, cache.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(constants), &constants);
        vkCmdDraw(frame.commandBuffer, 6, 1, 0, 0);
        vkCmdEndRenderPass(frame.commandBuffer);
        ResetDynamicStateShadow();
        InvalidatePipelineMemo();
        // This native pass binds its own set/layout without going through the
        // application's descriptor binder. Its next fast path must rebind.
        if (m_uniformManager) m_uniformManager->OnCommandBufferBoundary();
        WireRetiredObjects retired{};
        retired.framebuffers.push_back(framebuffer);
        retired.imageViews = {sourceView, destinationView};
        retired.descriptorPools.push_back(descriptorPool);
        RetireWireObjects(Move(retired));
        if (destination.isDefault) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, true);
        return true;
    }
#endif
