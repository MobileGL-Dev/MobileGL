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
    // The pass's modules, layouts, render passes and pipeline, and the draw it records, are
    // WireDepthResolveProbe.cpp's (g5-msprobe): the multisample depth/stencil resolve probe runs
    // this exact arm as its CONTROL, so it is defined once for both.
    struct VulkanRenderer::WireMultisampleResolveResources {
        struct Target {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = 0; // the ONE aspect this pipeline writes
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        WireShaderResolveKit kit;
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
        DestroyWireShaderResolveKit(m_device, cache.kit);
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

        const VkImageAspectFlags formatAspects = WireDepthStencilFormatAspects(source.format);
        if (!(formatAspects & aspect)) return false;

        if (!m_wireMultisampleResolveResources) {
            m_wireMultisampleResolveResources = new WireMultisampleResolveResources;
            VK_VERIFY(CreateWireShaderResolveKit(m_device, m_wireShaderStencilExport,
                                                 m_wireMultisampleResolveResources->kit));
        }
        auto& cache = *m_wireMultisampleResolveResources;
        const VkShaderModule fragment = aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? cache.kit.stencilFragment
                                                                              : cache.kit.depthFragment;
        if (fragment == VK_NULL_HANDLE) return false;

        WireMultisampleResolveResources::Target* target = nullptr;
        for (auto& entry : cache.targets)
            if (entry.format == resolved.format && entry.aspect == aspect) { target = &entry; break; }
        if (!target) {
            cache.targets.emplace_back();
            target = &cache.targets.back();
            target->format = resolved.format;
            target->aspect = aspect;
            VK_VERIFY(CreateWireShaderResolveRenderPass(m_device, resolved.format, &target->renderPass));
            if (CreateWireShaderResolvePipeline(m_device, cache.kit, target->renderPass, aspect,
                                                &target->pipeline) != VK_SUCCESS) {
                vkDestroyRenderPass(m_device, target->renderPass, nullptr);
                cache.targets.pop_back();
                return false;
            }
        }

        m_textureManager->FlushPendingUploads();
        TransitionWireImage(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        TransitionWireImage(resolved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        // ONE ASPECT on the sampled view: a depth/stencil image can only be SAMPLED through a
        // view that names exactly one of its aspects, which is also what makes the stencil half
        // a usampler2DMS and the depth half a sampler2DMS.
        const VkImageViewCreateInfo sourceViewInfo =
            WireDepthStencilViewInfo(source.image, source.format, aspect, source.level, source.layer);
        VkImageView sourceView = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &sourceViewInfo, nullptr, &sourceView));
        // BOTH aspects on the attachment view: a depth/stencil ATTACHMENT names every aspect
        // its format has, whichever one the pipeline writes.
        const VkImageViewCreateInfo targetViewInfo =
            WireDepthStencilViewInfo(resolved.image, resolved.format, formatAspects, 0, 0);
        VkImageView targetView = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &targetViewInfo, nullptr, &targetView));

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
        alloc.pSetLayouts = &cache.kit.descriptorLayout;
        VK_VERIFY(vkAllocateDescriptorSets(m_device, &alloc, &descriptorSet));
        const VkDescriptorImageInfo sampled{cache.kit.nearest, sourceView,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        auto& frame = m_frameContext.GetCurrent();
        RecordWireShaderResolveDraw(frame.commandBuffer, cache.kit, target->renderPass, target->pipeline, framebuffer,
                                    descriptorSet, resolved.extent, {{sx0, sy0}, {width, height}});
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
