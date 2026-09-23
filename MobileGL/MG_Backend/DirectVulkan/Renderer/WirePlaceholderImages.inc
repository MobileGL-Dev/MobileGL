// UniformManager-owned native descriptors for wire bindings with no image.
// No GL object is constructed or inserted into the client's resource namespace.
#if MOBILEGL_BUILD_DISAGGREGATED
    void UniformManager::DestroyWirePlaceholderImage(WirePlaceholderImage& image) const {
        if (m_device != VK_NULL_HANDLE) {
            if (image.clearFramebuffer) vkDestroyFramebuffer(m_device, image.clearFramebuffer, nullptr);
            if (image.clearPass) vkDestroyRenderPass(m_device, image.clearPass, nullptr);
            if (image.sampler) vkDestroySampler(m_device, image.sampler, nullptr);
            if (image.view) vkDestroyImageView(m_device, image.view, nullptr);
            if (image.image) vkDestroyImage(m_device, image.image, nullptr);
            if (image.memory) vkFreeMemory(m_device, image.memory, nullptr);
        }
        image = {};
    }

    void UniformManager::SetWireInvalidStorageImageArm(Bool deviceNullDescriptor) {
        const char* forced = std::getenv("MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER");
        const Bool forcePrivate = forced != nullptr && std::strcmp(forced, "1") == 0;
        m_wireInvalidStorageImagesBindNull = deviceNullDescriptor && !forcePrivate;
        MGLOG_I("Magma wire: an invalid or unbound image unit binds %s (VK_EXT_robustness2 nullDescriptor %s%s)",
                m_wireInvalidStorageImagesBindNull ? "a NULL storage descriptor"
                                                   : "a placeholder private to its (binding, unit)",
                deviceNullDescriptor ? "enabled" : "unavailable",
                deviceNullDescriptor && forcePrivate ? "; MGITEST_MAGMA_FORCE_PRIVATE_IMAGE_PLACEHOLDER=1" : "");
    }

    Bool UniformManager::ResolveWirePlaceholderImage(VkCommandBuffer commands, const MagmaProgramSource& program,
            const ProgramFactory::VkProgramObject& programObj, Uint32 binding, Bool storage,
            VkDescriptorImageInfo& out, VkFormat boundStorageFormat, Uint32 unit) const {
        // GL 4.6 core 8.26, codex closeout finding 2: every access through an invalid image unit
        // is invalid - a load returns zero, a store and an atomic are discarded. A NULL storage
        // descriptor (VK_EXT_robustness2 nullDescriptor) is exactly that, for any target, format
        // and sample count, so nothing below is needed.
        if (storage && m_wireInvalidStorageImagesBindNull) {
            out = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            return true;
        }
        if (binding >= programObj.samplerTextureTargetByBinding.size() ||
            binding >= programObj.samplerNumericDomainByBinding.size()) return false;
        const auto target = programObj.samplerTextureTargetByBinding[binding];
        const auto domain = programObj.samplerNumericDomainByBinding[binding];
        const Bool multisample = target == TextureTarget::Texture2DMultisample ||
                                target == TextureTarget::Texture2DMultisampleArray;
        Bool shadow = false;
        if (!storage) {
            const Int location = programObj.samplerUniformLocationByBinding[binding];
            switch (program.GetUniformType(static_cast<Uint>(location))) {
            case GL_SAMPLER_1D_SHADOW: case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_CUBE_SHADOW:
            case GL_SAMPLER_1D_ARRAY_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW:
            case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW: case GL_SAMPLER_2D_RECT_SHADOW: shadow = true; break;
            default: break;
            }
        }
        VkFormat format = VK_FORMAT_UNDEFINED;
        if (storage) {
            if (binding >= programObj.storageImageFormatByBinding.size()) return false;
            format = programObj.storageImageFormatByBinding[binding];
            // A formatless declaration of an INVALID binding (a unit whose texture lacks the
            // named level or layer) takes the format the application bound, where the device can
            // store to it and it is in the shader's numeric class. The R32 default expands a load
            // to (0, 0, 0, 1); a four-channel carrier cleared to zero reads (0, 0, 0, 0), which
            // is what KHR-GL46.shader_image_load_store.incomplete_textures compares against.
            if (format == VK_FORMAT_UNDEFINED && boundStorageFormat != VK_FORMAT_UNDEFINED) {
                const Bool classMatches = domain == SamplerNumericDomain::Float
                    ? !vkuFormatIsUINT(boundStorageFormat) && !vkuFormatIsSINT(boundStorageFormat)
                    : domain == SamplerNumericDomain::SignedInteger ? vkuFormatIsSINT(boundStorageFormat)
                    : domain == SamplerNumericDomain::UnsignedInteger && vkuFormatIsUINT(boundStorageFormat);
                VkFormatProperties properties{};
                if (classMatches) vkGetPhysicalDeviceFormatProperties(m_physicalDevice, boundStorageFormat, &properties);
                if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) format = boundStorageFormat;
            }
            if (format == VK_FORMAT_UNDEFINED) format = PlaceholderFormatForNumericDomain(domain);
        } else if (shadow) format = VK_FORMAT_D16_UNORM;
        else if (domain == SamplerNumericDomain::Float) format = VK_FORMAT_R8G8B8A8_UNORM;
        else if (domain == SamplerNumericDomain::SignedInteger) format = VK_FORMAT_R8G8B8A8_SINT;
        else if (domain == SamplerNumericDomain::UnsignedInteger) format = VK_FORMAT_R8G8B8A8_UINT;
        if (format == VK_FORMAT_UNDEFINED) return false;
        VkImageType imageType = VK_IMAGE_TYPE_2D;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        VkImageCreateFlags flags = 0;
        Uint32 layers = 1;
        switch (target) {
        case TextureTarget::Texture1D: imageType = VK_IMAGE_TYPE_1D; viewType = VK_IMAGE_VIEW_TYPE_1D; break;
        case TextureTarget::Texture1DArray: imageType = VK_IMAGE_TYPE_1D; viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case TextureTarget::Texture2D: case TextureTarget::TextureRectangle:
        case TextureTarget::Texture2DMultisample: break;
        case TextureTarget::Texture2DArray: case TextureTarget::Texture2DMultisampleArray:
            viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case TextureTarget::Texture3D: imageType = VK_IMAGE_TYPE_3D; viewType = VK_IMAGE_VIEW_TYPE_3D; break;
        case TextureTarget::TextureCubeMap: case TextureTarget::TextureCubeMapArray:
            flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; layers = 6;
            viewType = target == TextureTarget::TextureCubeMap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            break;
        default: return false;
        }
        // A STORAGE placeholder is private to its (binding, unit). Shared by shape alone, a store
        // through invalid unit A was visible, after a barrier, to a load through invalid unit B of
        // the same shape - one Vulkan image behind both (codex closeout finding 2). The clear
        // below still runs before each use; what a private image cannot hide is the SAME unit's
        // own store to its own later load inside one pass (CONTRACT-P7 §12).
        WirePlaceholderKey key;
        key.shape = static_cast<Uint32>(format) | (static_cast<Uint64>(target) << 32) |
                    (static_cast<Uint64>(storage) << 48) | (static_cast<Uint64>(shadow) << 49);
        if (storage) {
            key.binding = binding;
            key.unit = unit;
        }
        auto found = m_wirePlaceholderImages.find(key);
        if (found == m_wirePlaceholderImages.end()) {
            WirePlaceholderImage image{};
            image.format = format; image.layers = layers;
            image.aspect = shadow ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            const auto fail = [&] {
                MGLOG_E_ONCE("Magma wire placeholder allocation failed: target=%d format=%d storage=%d multisample=%d",
                    static_cast<Int>(target), static_cast<Int>(format), static_cast<Int>(storage), static_cast<Int>(multisample));
                DestroyWirePlaceholderImage(image);
                return false;
            };
            const VkImageUsageFlags usage = (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT) |
                (multisample ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            VkImageFormatProperties properties{};
            if (vkGetPhysicalDeviceImageFormatProperties(m_physicalDevice, format, imageType,
                    VK_IMAGE_TILING_OPTIMAL, usage, flags, &properties) != VK_SUCCESS) return fail();
            if (multisample) {
                // MS=1 in SPIR-V cannot name a single-sampled image, even when
                // the shader's runtime branch does not fetch that descriptor.
                image.samples = VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
                for (const auto sample : {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT,
                                          VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_32_BIT, VK_SAMPLE_COUNT_64_BIT}) {
                    if (properties.sampleCounts & sample) { image.samples = sample; break; }
                }
                if (image.samples == VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM) return fail();
            }
            VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            create.flags = flags; create.imageType = imageType; create.format = format;
            create.extent = {1, 1, 1}; create.mipLevels = 1; create.arrayLayers = layers;
            create.samples = image.samples; create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(m_device, &create, nullptr, &image.image) != VK_SUCCESS) return fail();
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(m_device, image.image, &requirements);
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
            Uint32 memoryType = ~0u;
            for (Uint32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i))) continue;
                if (memoryType == ~0u) memoryType = i;
                if (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                    memoryType = i; break;
                }
            }
            if (memoryType == ~0u) return fail();
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = memoryType;
            if (vkAllocateMemory(m_device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, image.image, image.memory, 0) != VK_SUCCESS) return fail();
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image; view.viewType = viewType; view.format = format;
            view.subresourceRange = {image.aspect, 0, 1, 0, layers};
            if (vkCreateImageView(m_device, &view, nullptr, &image.view) != VK_SUCCESS) return fail();
            if (!storage) {
                VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
                sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampler.compareEnable = shadow; sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                sampler.maxLod = 0.0f;
                if (vkCreateSampler(m_device, &sampler, nullptr, &image.sampler) != VK_SUCCESS) return fail();
            }
            if (multisample) {
                VkAttachmentDescription attachment{};
                attachment.format = format; attachment.samples = image.samples;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
                VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                VkSubpassDescription subpass{};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
                VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                pass.attachmentCount = 1; pass.pAttachments = &attachment;
                pass.subpassCount = 1; pass.pSubpasses = &subpass;
                VkSubpassDependency dependencies[2]{};
                dependencies[0] = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
                dependencies[1] = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, 0};
                pass.dependencyCount = 2; pass.pDependencies = dependencies;
                if (vkCreateRenderPass(m_device, &pass, nullptr, &image.clearPass) != VK_SUCCESS) return fail();
                VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                framebuffer.renderPass = image.clearPass; framebuffer.attachmentCount = 1;
                framebuffer.pAttachments = &image.view; framebuffer.width = framebuffer.height = 1; framebuffer.layers = layers;
                if (vkCreateFramebuffer(m_device, &framebuffer, nullptr, &image.clearFramebuffer) != VK_SUCCESS) return fail();
            }
            found = m_wirePlaceholderImages.emplace(key, image).first;
        }
        auto& image = found->second;
        // Sampled incomplete textures remain immutable opaque black. A storage
        // placeholder is reset before each use, so a prior discarded store cannot
        // leak into a later pass's loads through the same (binding, unit).
        if (storage || image.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            VkClearValue clear{};
            if (!storage) {
                if (domain == SamplerNumericDomain::SignedInteger) clear.color.int32[3] = 1;
                else if (domain == SamplerNumericDomain::UnsignedInteger) clear.color.uint32[3] = 1;
                else clear.color.float32[3] = 1.0f;
            }
            if (multisample) {
                VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                begin.renderPass = image.clearPass; begin.framebuffer = image.clearFramebuffer;
                begin.renderArea.extent = {1, 1}; begin.clearValueCount = 1; begin.pClearValues = &clear;
                vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(commands);
                image.layout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                if (!VkTextureManager::TransitionImageLayout(commands, image.image, image.layout,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, image.aspect)) return false;
                const VkImageSubresourceRange range{image.aspect, 0, 1, 0, image.layers};
                if (shadow) {
                    const VkClearDepthStencilValue depth{0.0f, 0};
                    vkCmdClearDepthStencilImage(commands, image.image, image.layout, &depth, 1, &range);
                } else vkCmdClearColorImage(commands, image.image, image.layout, &clear.color, 1, &range);
                if (!VkTextureManager::TransitionImageLayout(commands, image.image, image.layout,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        image.aspect)) return false;
            }
            VkMemoryBarrier visible{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            visible.srcAccessMask = multisample ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
            visible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(commands, multisample ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &visible, 0, nullptr, 0, nullptr);
        }
        out = {image.sampler, image.view, VK_IMAGE_LAYOUT_GENERAL};
        return true;
    }
#endif
