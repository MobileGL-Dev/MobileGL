// Included by VulkanRenderer.cpp inside the backend namespace, after WireSharedImage.inc.
//
// YUV SHARED IMAGES (NV12, P010; MG_Remote/Server/SharedImageYuv.h). A YUV image is never a
// texture's image: the texture keeps its own RGBA8 level 0 (VkTextureManager::SyncWireTextureShape)
// and, at its first use in each frame, the image is CONVERTED into it here - the image's
// AHardwareBuffer imported with its external format, sampled through an immutable sampler that
// carries a VkSamplerYcbcrConversion built from the import's colour hints, one triangle over the
// target. Zero CPU copies for an image this server allocated; a foreign one (a video decoder's
// buffer) is first brought up to date by the registry's labelled CPU copy (RefreshForeign).
//
// Why converted rather than sampled in place: a Y'CbCr conversion is an immutable sampler, which
// lives in the descriptor set layout and therefore in every application pipeline that samples the
// texture. A conversion pass keeps those pipelines exactly what they are for an RGBA texture, at
// the cost of one small pass per new frame of the video.
#if MOBILEGL_BUILD_DISAGGREGATED
#include "WireYuvConvertSpirv.h"

    struct VulkanRenderer::WireYuvResources {
        PFN_vkCreateSamplerYcbcrConversion createConversion = nullptr;
        PFN_vkDestroySamplerYcbcrConversion destroyConversion = nullptr;
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        // One per (buffer format, conversion, target format). Never dropped before shutdown: a
        // device sees a handful of them.
        struct Pipeline {
            Uint64 externalFormat = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSamplerYcbcrModelConversion model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
            VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            VkComponentMapping components{};
            VkChromaLocation xOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
            VkChromaLocation yOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
            VkFilter chromaFilter = VK_FILTER_NEAREST;
            VkFormat target = VK_FORMAT_UNDEFINED;
            VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };
        Vector<Pipeline> pipelines;
    };

    namespace {
        // One texture's import of its YUV buffer (TextureResource::yuvSource): the image on the
        // buffer's memory and a view through the conversion of `pipeline`. Destroyed by the
        // resource's (deferred) Reset, so never under a submission still reading it.
        struct WireYuvSource {
            VkDevice device = VK_NULL_HANDLE;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            SizeT pipeline = 0;
            MG_Remote::Server::SharedImages::Yuv::Hints hints;
            ~WireYuvSource() {
                if (device == VK_NULL_HANDLE) return;
                if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
                if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
                if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
            }
        };

        VkSamplerYcbcrModelConversion YcbcrModelFor(const MG_Remote::Server::SharedImages::Yuv::Hints& hints) {
            namespace Yuv = MG_Remote::Server::SharedImages::Yuv;
            switch (hints.EffectiveColorSpace()) {
            case Yuv::kEglItuRec709:
                return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
            case Yuv::kEglItuRec2020:
                return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
            default:
                return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
            }
        }
    } // namespace

    void VulkanRenderer::DestroyWireYuvResources() {
        if (!m_wireYuvResources) return;
        auto& cache = *m_wireYuvResources;
        for (const auto& entry : cache.pipelines) {
            vkDestroyPipeline(m_device, entry.pipeline, nullptr);
            vkDestroyRenderPass(m_device, entry.renderPass, nullptr);
            vkDestroyPipelineLayout(m_device, entry.pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_device, entry.descriptorLayout, nullptr);
            vkDestroySampler(m_device, entry.sampler, nullptr);
            if (cache.destroyConversion != nullptr) cache.destroyConversion(m_device, entry.conversion, nullptr);
        }
        vkDestroyShaderModule(m_device, cache.vertex, nullptr);
        vkDestroyShaderModule(m_device, cache.fragment, nullptr);
        delete m_wireYuvResources;
        m_wireYuvResources = nullptr;
    }

    Bool VulkanRenderer::ConvertYuvSharedImage(VkTextureManager::TextureResource& resource) {
#if defined(__ANDROID__)
        namespace SI = MG_Remote::Server::SharedImages;
        if (resource.yuvConvertedFrame == m_yuvFrameSerial) return true;
        const auto image = std::static_pointer_cast<const SI::Image>(resource.yuvOwner);
        if (image == nullptr || image->Native == nullptr || resource.image == VK_NULL_HANDLE) return false;
        if (!m_samplerYcbcrConversion) {
            MGLOG_E_ONCE("Magma: YUV shared image %llu cannot be sampled: the device has no samplerYcbcrConversion",
                         static_cast<unsigned long long>(image->Id));
            return false;
        }
        // Whatever happens below, this frame does not try again.
        resource.yuvConvertedFrame = m_yuvFrameSerial;
        // THE CPU-COPY FALLBACK, for a buffer this server did not allocate (SharedImageRegistry.h).
        String why;
        if (image->IsForeign() && !SI::RefreshForeign(*image, why)) {
            MGLOG_E_ONCE("Magma: foreign YUV image %llu could not be refreshed: %s",
                         static_cast<unsigned long long>(image->Id), why.c_str());
        }
        const SI::Yuv::Hints hints = SI::GetYuvHints(*image);

        if (!m_wireYuvResources) {
            m_wireYuvResources = new WireYuvResources;
            auto& cache = *m_wireYuvResources;
            cache.createConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
                vkGetDeviceProcAddr(m_device, "vkCreateSamplerYcbcrConversion"));
            if (cache.createConversion == nullptr)
                cache.createConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
                    vkGetDeviceProcAddr(m_device, "vkCreateSamplerYcbcrConversionKHR"));
            cache.destroyConversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
                vkGetDeviceProcAddr(m_device, "vkDestroySamplerYcbcrConversion"));
            if (cache.destroyConversion == nullptr)
                cache.destroyConversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
                    vkGetDeviceProcAddr(m_device, "vkDestroySamplerYcbcrConversionKHR"));
            const auto shader = [&](const Uint32* code, SizeT bytes, VkShaderModule& module) {
                VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                info.codeSize = bytes;
                info.pCode = code;
                VK_VERIFY(vkCreateShaderModule(m_device, &info, nullptr, &module));
            };
            shader(kWireYuvConvertVertexSpirv, sizeof(kWireYuvConvertVertexSpirv), cache.vertex);
            shader(kWireYuvConvertFragmentSpirv, sizeof(kWireYuvConvertFragmentSpirv), cache.fragment);
        }
        auto& cache = *m_wireYuvResources;
        if (cache.createConversion == nullptr || cache.destroyConversion == nullptr) {
            MGLOG_E_ONCE("Magma: vkCreateSamplerYcbcrConversion is not available; YUV images cannot be sampled");
            return false;
        }

        // ---- the texture's import of the buffer, and the pipeline its conversion needs ----
        auto* source = static_cast<WireYuvSource*>(resource.yuvSource.get());
        if (source != nullptr && !(source->hints == hints)) {
            // Re-imported with other colour hints: the conversion (and so the view) changes.
            auto retired = MakeUnique<VkTextureManager::TextureResource>();
            retired->yuvSource = std::move(resource.yuvSource);
            WireRetiredObjects objects{};
            objects.textures.push_back(Move(retired));
            RetireWireObjects(Move(objects));
            source = nullptr;
        }
        if (source == nullptr) {
            WireAhbImport ctx;
            if (!GetWireAhbImport(ctx)) return false;
            auto* ahb = static_cast<AHardwareBuffer*>(image->Native);
            const auto getProperties =
                reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(ctx.getAhbProperties);
            VkAndroidHardwareBufferFormatPropertiesANDROID format{
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
            VkAndroidHardwareBufferPropertiesANDROID properties{
                VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
            properties.pNext = &format;
            if (getProperties(m_device, ahb, &properties) != VK_SUCCESS) {
                MGLOG_E_ONCE("Magma: YUV image %llu: vkGetAndroidHardwareBufferPropertiesANDROID failed",
                             static_cast<unsigned long long>(image->Id));
                return false;
            }
            // The conversion: the import's hints where it gave them, else what the buffer suggests.
            WireYuvResources::Pipeline key;
            key.externalFormat = format.format == VK_FORMAT_UNDEFINED ? format.externalFormat : 0;
            key.format = format.format;
            key.model = hints.ColorSpace != 0 ? YcbcrModelFor(hints) : format.suggestedYcbcrModel;
            key.range = hints.Range != 0 ? (hints.FullRange() ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL
                                                              : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW)
                                         : format.suggestedYcbcrRange;
            key.components = format.samplerYcbcrConversionComponents;
            const auto chroma = [&format](Uint32 siting, VkChromaLocation suggested) {
                const Bool midpoint = siting == SI::Yuv::kEglYuvChromaSiting0_5;
                const Bool cosited = siting == SI::Yuv::kEglYuvChromaSiting0;
                if (midpoint && (format.formatFeatures & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT))
                    return VK_CHROMA_LOCATION_MIDPOINT;
                if (cosited && (format.formatFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT))
                    return VK_CHROMA_LOCATION_COSITED_EVEN;
                return suggested;
            };
            key.xOffset = chroma(hints.SitingX, format.suggestedXChromaOffset);
            key.yOffset = chroma(hints.SitingY, format.suggestedYChromaOffset);
            key.chromaFilter =
                (format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0
                    ? VK_FILTER_LINEAR
                    : VK_FILTER_NEAREST;
            key.target = resource.format;
            SizeT index = cache.pipelines.size();
            for (SizeT i = 0; i < cache.pipelines.size(); ++i) {
                const auto& p = cache.pipelines[i];
                if (p.externalFormat == key.externalFormat && p.format == key.format && p.model == key.model &&
                    p.range == key.range && p.components.r == key.components.r &&
                    p.components.g == key.components.g && p.components.b == key.components.b &&
                    p.components.a == key.components.a && p.xOffset == key.xOffset && p.yOffset == key.yOffset &&
                    p.chromaFilter == key.chromaFilter && p.target == key.target) {
                    index = i;
                    break;
                }
            }
            if (index == cache.pipelines.size()) {
                VkExternalFormatANDROID external{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
                external.externalFormat = key.externalFormat;
                VkSamplerYcbcrConversionCreateInfo conversion{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
                conversion.pNext = key.externalFormat != 0 ? &external : nullptr;
                conversion.format = key.format;
                conversion.ycbcrModel = key.model;
                conversion.ycbcrRange = key.range;
                conversion.components = key.components;
                conversion.xChromaOffset = key.xOffset;
                conversion.yChromaOffset = key.yOffset;
                conversion.chromaFilter = key.chromaFilter;
                conversion.forceExplicitReconstruction = VK_FALSE;
                if (cache.createConversion(m_device, &conversion, nullptr, &key.conversion) != VK_SUCCESS) {
                    MGLOG_E_ONCE("Magma: YUV image %llu: vkCreateSamplerYcbcrConversion failed",
                                 static_cast<unsigned long long>(image->Id));
                    return false;
                }
                VkSamplerYcbcrConversionInfo conversionInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
                conversionInfo.conversion = key.conversion;
                VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                sampler.pNext = &conversionInfo;
                // Without separate reconstruction filters the sampler filters as the conversion does.
                sampler.magFilter = sampler.minFilter = key.chromaFilter;
                sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampler.maxLod = 0.0f;
                VK_VERIFY(vkCreateSampler(m_device, &sampler, nullptr, &key.sampler));
                const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                           VK_SHADER_STAGE_FRAGMENT_BIT, &key.sampler};
                VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                layout.bindingCount = 1;
                layout.pBindings = &binding;
                VK_VERIFY(vkCreateDescriptorSetLayout(m_device, &layout, nullptr, &key.descriptorLayout));
                VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pipelineLayout.setLayoutCount = 1;
                pipelineLayout.pSetLayouts = &key.descriptorLayout;
                VK_VERIFY(vkCreatePipelineLayout(m_device, &pipelineLayout, nullptr, &key.pipelineLayout));
                // Every texel of the target is written: nothing to load.
                VkAttachmentDescription attachment{};
                attachment.format = key.target;
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                VkSubpassDescription subpass{};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1;
                subpass.pColorAttachments = &color;
                VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
                pass.attachmentCount = 1;
                pass.pAttachments = &attachment;
                pass.subpassCount = 1;
                pass.pSubpasses = &subpass;
                VK_VERIFY(vkCreateRenderPass(m_device, &pass, nullptr, &key.renderPass));
                VkPipelineShaderStageCreateInfo stages[2]{};
                for (auto& stage : stages) {
                    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    stage.pName = "main";
                }
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = cache.vertex;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = cache.fragment;
                const VkPipelineVertexInputStateCreateInfo vertex{
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                VkPipelineInputAssemblyStateCreateInfo assembly{
                    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                viewport.viewportCount = viewport.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo raster{
                    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                raster.polygonMode = VK_POLYGON_MODE_FILL;
                raster.cullMode = VK_CULL_MODE_NONE;
                raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                raster.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo samples{
                    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
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
                pipeline.layout = key.pipelineLayout;
                pipeline.renderPass = key.renderPass;
                VK_VERIFY(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &key.pipeline));
                cache.pipelines.push_back(key);
                MGLOG_I("Magma: YUV conversion pipeline %zu: %s format %llu, model %d, range %d, chroma %d/%d, "
                        "filter %d",
                        index, key.externalFormat != 0 ? "external" : "Vulkan",
                        static_cast<unsigned long long>(key.externalFormat != 0 ? key.externalFormat
                                                                                : static_cast<Uint64>(key.format)),
                        static_cast<Int>(key.model), static_cast<Int>(key.range), static_cast<Int>(key.xOffset),
                        static_cast<Int>(key.yOffset), static_cast<Int>(key.chromaFilter));
            }
            const auto& chosen = cache.pipelines[index];

            // The buffer, imported with its external format, sampled only.
            auto created = std::make_shared<WireYuvSource>();
            created->device = m_device;
            created->pipeline = index;
            created->hints = hints;
            VkExternalFormatANDROID external{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
            external.externalFormat = chosen.externalFormat;
            VkExternalMemoryImageCreateInfo externalMemory{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            externalMemory.pNext = chosen.externalFormat != 0 ? &external : nullptr;
            externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.pNext = &externalMemory;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = chosen.format;
            imageInfo.extent = {image->Width, image->Height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &imageInfo, nullptr, &created->image) != VK_SUCCESS) {
                created->image = VK_NULL_HANDLE;
                MGLOG_E_ONCE("Magma: YUV image %llu: vkCreateImage(external format) failed",
                             static_cast<unsigned long long>(image->Id));
                return false;
            }
            Int32 type = -1;
            for (Uint32 i = 0; i < ctx.memory.memoryTypeCount && type < 0; ++i) {
                if ((properties.memoryTypeBits & (1u << i)) != 0) type = static_cast<Int32>(i);
            }
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.image = created->image;
            VkImportAndroidHardwareBufferInfoANDROID import{
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
            import.pNext = &dedicated;
            import.buffer = ahb;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.pNext = &import;
            allocate.allocationSize = properties.allocationSize;
            allocate.memoryTypeIndex = static_cast<Uint32>(type < 0 ? 0 : type);
            if (type < 0 || vkAllocateMemory(m_device, &allocate, nullptr, &created->memory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, created->image, created->memory, 0) != VK_SUCCESS) {
                MGLOG_E_ONCE("Magma: YUV image %llu: its buffer could not be imported",
                             static_cast<unsigned long long>(image->Id));
                return false;
            }
            VkSamplerYcbcrConversionInfo conversionInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
            conversionInfo.conversion = chosen.conversion;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.pNext = &conversionInfo;
            view.image = created->image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = chosen.format;
            view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(m_device, &view, nullptr, &created->view) != VK_SUCCESS) {
                created->view = VK_NULL_HANDLE;
                MGLOG_E_ONCE("Magma: YUV image %llu: its view could not be created",
                             static_cast<unsigned long long>(image->Id));
                return false;
            }
            MGLOG_I("Magma: YUV image %llu (%ux%u%s) sampled through conversion pipeline %zu",
                    static_cast<unsigned long long>(image->Id), image->Width, image->Height,
                    image->IsForeign() ? ", foreign: CPU-copied" : ", zero-copy", index);
            resource.yuvSource = created;
            source = created.get();
        }
        const auto& chosen = cache.pipelines[source->pipeline];

        // ---- the pass ----
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (VkRenderPassManager::GetActiveRenderPass()) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        m_textureManager->FlushPendingUploads();
        const Uint32 family = static_cast<Uint32>(m_physicalDevice.queueFamilies.graphicsFamily);
        // The buffer's content is its producer's (the CPU's copy, or the allocator's writer): it is
        // acquired from the foreign family for the read and handed back after it.
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = family;
        acquire.image = source->image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        EndActiveRenderPassOn(frame.commandBuffer);
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);

        WireImage target{};
        target.image = resource.image;
        target.format = resource.format;
        target.viewType = VK_IMAGE_VIEW_TYPE_2D;
        target.extent = resource.extent;
        target.layout = resource.layout;
        target.trackedLayout = &resource.layout;
        target.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        target.usageFlags = resource.usageFlags;
        target.levels = resource.mipLevels;
        TransitionWireImage(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkImageViewCreateInfo targetViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        targetViewInfo.image = resource.image;
        targetViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        targetViewInfo.format = resource.format;
        targetViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView targetView = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateImageView(m_device, &targetViewInfo, nullptr, &targetView));
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = chosen.renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &targetView;
        framebufferInfo.width = resource.extent.width;
        framebufferInfo.height = resource.extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer));
        // A multi-planar image may take more than one descriptor of its binding.
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        VK_VERIFY(vkCreateDescriptorPool(m_device, &pool, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &chosen.descriptorLayout;
        VK_VERIFY(vkAllocateDescriptorSets(m_device, &alloc, &descriptorSet));
        const VkDescriptorImageInfo sampled{chosen.sampler, source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sampled;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = chosen.renderPass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{0, 0}, resource.extent};
        vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chosen.pipeline);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(resource.extent.width),
                                  static_cast<float>(resource.extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, resource.extent};
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chosen.pipelineLayout, 0, 1,
                                &descriptorSet, 0, nullptr);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.commandBuffer);

        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = family;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        EndActiveRenderPassOn(frame.commandBuffer);
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
        // GENERAL, the layout every wire descriptor samples in, with the pass's writes made visible.
        target.layout = resource.layout;
        TransitionWireImage(target, VK_IMAGE_LAYOUT_GENERAL);
        ResetDynamicStateShadow();
        InvalidatePipelineMemo();
        if (m_uniformManager) m_uniformManager->OnCommandBufferBoundary();
        m_textureManager->StampResourceRecordingUse(resource);
        m_textureManager->StampResourceRecordingWrite(resource);
        WireRetiredObjects retired{};
        retired.framebuffers.push_back(framebuffer);
        retired.imageViews = {targetView};
        retired.descriptorPools.push_back(descriptorPool);
        RetireWireObjects(Move(retired));
        // The frame's fence orders the next refresh of a foreign buffer after this read.
        m_sharedImageReads.NoteRead(image);
        return true;
#else
        (void)resource;
        return false;
#endif
    }
#endif // MOBILEGL_BUILD_DISAGGREGATED
