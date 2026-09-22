// P5f fm: included by VulkanRenderer.cpp inside the backend namespace. All inputs here
// belong to the applier; no frontend object, lifetime-id lookup or allocator is used.
#if MOBILEGL_BUILD_DISAGGREGATED
    // P7 wave 0. THE MESSAGE IS UNCHANGED; where it goes is not. This funnel is how eleven of
    // the fifteen P7-marked refusals die (the other four are WireDescriptorFatal's), and until now
    // every one of them raised its own abort: no SessionFault frame for the peer, no
    // SessionFaultCount() for exit gate S8, and nothing tying the hand-typed word to
    // FatalFamilies.def. MGPipeSessionFail hands the line to the hook MG_Remote installs at
    // server-role init, whose arm for UnmigratedVerb is SessionFail(MGFatalFamily::
    // UnmigratedVerb, ...) - and with no hook installed it logs and aborts exactly as this did.
    [[noreturn]] static void MagmaWireFatal(const char* detail) {
        MG_Pipe::MGPipeSessionFail(MG_Pipe::MGPipeFatalFamily::UnmigratedVerb,
                                   "MGPipe: Fatal{UnmigratedVerb, \"Magma:%s\"}", detail);
    }

    VulkanRenderer::WireImage VulkanRenderer::ResolveWireImage(
        const MG_Pipe::MGPFramebufferState& fbo, const MG_Pipe::MGPSurface& surface,
        VkImageAspectFlags aspect, Bool isWriteTarget) {
        WireImage out{};
        out.aspect = aspect;
        if (fbo.IsDefault) {
            out.isDefault = true;
            out.extent = m_swapchainObject.GetExtent();
            out.swapchainImageIndex = isWriteTarget ? DefaultFramebufferWriteIndex(true)
                                                    : DefaultFramebufferReadIndex(true);
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                out.image = m_swapchainObject.GetImage(out.swapchainImageIndex);
                out.format = m_swapchainObject.GetSurfaceFormat().format;
                out.layout = m_swapchainObject.GetImageLayout(out.swapchainImageIndex);
            } else {
                out.image = m_swapchainObject.GetDepthStencilImage(out.swapchainImageIndex);
                out.format = m_swapchainObject.GetDepthStencilFormat();
                out.layout = m_swapchainObject.GetDepthStencilImageLayout(out.swapchainImageIndex);
            }
            return out;
        }
        if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone) return out;
        const Bool renderbuffer = surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer;
        Uint32 level = surface.Level, layer = surface.Layer, availableLayers = 1;
        // FramebufferTexture2D names a cube face in UploadTarget; its Layer is
        // still zero. Resolve that face before applying a texture view's window,
        // just as the monolith attachment resolver does.
        const auto uploadTarget = static_cast<TextureUploadTarget>(surface.UploadTarget);
        if (!renderbuffer && !surface.Layered && uploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
            uploadTarget <= TextureUploadTarget::CubeMapNegativeZ)
            layer = static_cast<Uint32>(uploadTarget) - static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX);
        VkFormat viewFormat = VK_FORMAT_UNDEFINED;
        out.storage = renderbuffer ? surface.Res : m_textureManager->ResolveWireTextureStorage(
            surface.Res, level, layer, &viewFormat, &availableLayers);
        if (MG_Pipe::MGPipeHandleIsNull(out.storage)) MagmaWireFatal("framebuffer-view-record");
        auto* resource = m_textureManager->SyncTextureResourceByHandle(out.storage, renderbuffer);
        if (!resource) MagmaWireFatal("framebuffer-resource");
        out.image = resource->image;
        out.format = viewFormat != VK_FORMAT_UNDEFINED ? viewFormat : resource->format;
        out.extent = {std::max(resource->extent.width >> level, 1u),
                      std::max(resource->extent.height >> level, 1u)};
        out.layout = resource->layout;
        out.trackedLayout = &resource->layout;
        out.samples = resource->sampleCount;
        out.level = level;
        out.levels = resource->mipLevels;
        out.layer = layer;
        out.is3D = resource->depth > 1;
        out.layers = surface.Layered ? (out.is3D ? std::max(resource->depth >> level, 1u) : availableLayers) : 1;
        const Bool oneDimensional = resource->viewType == VK_IMAGE_VIEW_TYPE_1D ||
                                    resource->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        out.viewType = oneDimensional ? (out.layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) :
                                      (out.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        if (out.level >= resource->mipLevels || out.layer + out.layers >
            (out.is3D ? std::max(resource->depth >> level, 1u) : resource->arrayLayers))
            MagmaWireFatal("framebuffer-subresource");
        return out;
    }

    void VulkanRenderer::TransitionWireImage(WireImage& image, VkImageLayout layout) {
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (VkRenderPassManager::GetActiveRenderPass()) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags access = 0;
        GetImageTransitionSourceState(image.layout, stage, access);
        // One tracked layout per image: move the complete mip chain, never just the surface.
        VkImageAspectFlags barrierAspect = image.aspect;
        if (image.format == VK_FORMAT_D24_UNORM_S8_UINT || image.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            barrierAspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        if (image.layout == layout) {
            // GENERAL->GENERAL is not a layout transition, but the next load,
            // sample or blit still depends on earlier attachment/shader writes.
            // The old per-verb queue idle must not be its only ordering edge.
            VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        }
        if (!VkTextureManager::TransitionImageLayout(frame.commandBuffer, image.image, image.layout, layout,
            stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, barrierAspect, 0, image.levels))
            MagmaWireFatal("image-transition");
        if (image.trackedLayout) *image.trackedLayout = image.layout;
        if (image.isDefault) {
            if (image.aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                m_swapchainObject.SetImageLayout(image.swapchainImageIndex, image.layout);
            else m_swapchainObject.SetDepthStencilImageLayout(image.swapchainImageIndex, image.layout);
        }
    }

    void VulkanRenderer::ClearWireFramebuffer(const MG_Pipe::MGPFramebufferState& fbo,
                                              const ClearAttachmentPayload& payload, GLint drawbuffer) {
        RetireWireDrawPass();
        if (MGB_CTX->IsCapabilityEnabled(CapabilityInput::RasterizerDiscard)) return;
        const auto clearSurface = [&](const MG_Pipe::MGPSurface& surface, VkImageAspectFlags aspect,
                                      Uint32 colorIndex) {
            if (!fbo.IsDefault && surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone) return;
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                const auto mask = MGB_CTX->GetColorMaskIndexed(colorIndex);
                if (!mask.r() && !mask.g() && !mask.b() && !mask.a()) return;
                if (!mask.r() || !mask.g() || !mask.b() || !mask.a()) {
                    MGLOG_W_ONCE("DirectVulkan: glClear with a partial color mask is not supported");
                    return;
                }
            }
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                if (!MGB_CTX->GetDepthMask()) aspect &= ~VK_IMAGE_ASPECT_DEPTH_BIT;
            }
            if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
                const Uint32 mask = MGB_CTX->GetStencilState(StencilFace::Front).WriteMask & 255u;
                if (!mask) aspect &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
                else if (mask != 255u) {
                    MGLOG_W_ONCE("DirectVulkan: glClear with a partial stencil write mask is not supported");
                    aspect &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
                }
            }
            if (!aspect) return;
            WireImage image = ResolveWireImage(fbo, surface, aspect, /*isWriteTarget=*/true);
            if (!image.image) return;
            m_textureManager->FlushPendingUploads();
            VkRect2D area{{0, 0}, image.extent};
            if (MGB_CTX->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
                const auto& box = MGB_CTX->GetScissorBox();
                const Int32 x0 = std::max(box.x(), 0), y0 = std::max(box.y(), 0);
                const Int32 x1 = std::min(box.x() + box.z(), static_cast<Int32>(image.extent.width));
                const Int32 y1 = std::min(box.y() + box.w(), static_cast<Int32>(image.extent.height));
                if (x0 >= x1 || y0 >= y1) return;
                area = {{x0, y0}, {static_cast<Uint32>(x1-x0), static_cast<Uint32>(y1-y0)}};
                if (image.isDefault && !MapDefaultFramebufferReadbackRect(x0, y0, x1-x0, y1-y0,
                    image.extent, m_swapchainObject.GetPreTransform(), &area.offset, &area.extent))
                    MagmaWireFatal("clear-surface-transform");
            }
            const Bool color = aspect == VK_IMAGE_ASPECT_COLOR_BIT;
            const auto layout = color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            TransitionWireImage(image, layout);
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image.image;
            viewInfo.viewType = image.viewType;
            viewInfo.format = image.format;
            viewInfo.subresourceRange = {color ? VK_IMAGE_ASPECT_COLOR_BIT :
                (image.format == VK_FORMAT_D24_UNORM_S8_UINT || image.format == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : image.aspect), image.level, 1, image.layer, image.layers};
            VkImageView view = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &view));
            VkAttachmentDescription attachment{};
            attachment.format = image.format;
            attachment.samples = image.samples;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = layout;
            attachment.finalLayout = layout;
            VkAttachmentReference ref{0, layout};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = color ? 1 : 0;
            subpass.pColorAttachments = color ? &ref : nullptr;
            subpass.pDepthStencilAttachment = color ? nullptr : &ref;
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.attachmentCount = 1; passInfo.pAttachments = &attachment;
            passInfo.subpassCount = 1; passInfo.pSubpasses = &subpass;
            VkRenderPass pass = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateRenderPass(m_device, &passInfo, nullptr, &pass));
            VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbInfo.renderPass = pass; fbInfo.attachmentCount = 1; fbInfo.pAttachments = &view;
            fbInfo.width = image.extent.width; fbInfo.height = image.extent.height; fbInfo.layers = image.layers;
            VkFramebuffer fb = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateFramebuffer(m_device, &fbInfo, nullptr, &fb));
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            begin.renderPass = pass; begin.framebuffer = fb; begin.renderArea = area;
            auto clearPayload = payload;
            if (color) PreCompensateSrgbClearColor(clearPayload, image.format);
            VkClearAttachment clear{};
            clear.aspectMask = aspect;
            clear.clearValue.color = MakeVkClearColorValue(clearPayload, false);
            if (!color) clear.clearValue.depthStencil = {payload.depth, payload.stencil};
            VkClearRect rect{area, 0, image.layers};
            auto& frame = m_frameContext.GetCurrent();
            vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdClearAttachments(frame.commandBuffer, 1, &clear, 1, &rect);
            vkCmdEndRenderPass(frame.commandBuffer);
            WireRetiredObjects retired{};
            retired.framebuffers.push_back(fb);
            retired.renderPasses.push_back(pass);
            retired.imageViews.push_back(view);
            RetireWireObjects(Move(retired));
            if (!image.isDefault && surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                m_textureManager->MarkWireTextureGpuWritten(image.storage, image.level, image.layer, image.layers);
            if (image.isDefault) {
                if (color) m_swapchainObject.SetImageContentDefined(image.swapchainImageIndex, true);
                else m_swapchainObject.SetDepthStencilContentDefined(image.swapchainImageIndex, true);
            }
        };
        if (payload.mask & GL_COLOR_BUFFER_BIT) {
            for (Uint32 i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
                if (drawbuffer >= 0 && static_cast<Uint32>(drawbuffer) != i) continue;
                const Int32 attachment = fbo.DrawBuffers[i];
                if (attachment >= 0 && attachment < static_cast<Int32>(MG_Pipe::kMGPipeMaxColorAttachments))
                    clearSurface(fbo.Color[attachment], VK_IMAGE_ASPECT_COLOR_BIT, i);
            }
        }
        if (payload.mask & GL_DEPTH_BUFFER_BIT) clearSurface(fbo.Depth, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
        if (payload.mask & GL_STENCIL_BUFFER_BIT) clearSurface(fbo.Stencil, VK_IMAGE_ASPECT_STENCIL_BIT, 0);
    }

    void VulkanRenderer::ReadWirePixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                        GLenum format, GLenum type, void* pixels) {
        if (width <= 0 || height <= 0) return;
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("wire-verb-flush");
        const auto* fbo = MG_Pipe::MGPipeApplier().ReadFramebuffer();
        if (!fbo) MagmaWireFatal("read-framebuffer-record");
        VkImageAspectFlags aspect = format == GL_DEPTH_COMPONENT ? VK_IMAGE_ASPECT_DEPTH_BIT :
                                   format == GL_STENCIL_INDEX ? VK_IMAGE_ASPECT_STENCIL_BIT :
                                   VK_IMAGE_ASPECT_COLOR_BIT;
        if (format == GL_DEPTH_STENCIL) aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        const auto& surface = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? fbo->Depth :
                              aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? fbo->Stencil : fbo->ReadSurface;
        WireImage source = ResolveWireImage(*fbo, surface, aspect, /*isWriteTarget=*/false);
        if (!source.image) return;
        if (source.isDefault && aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
            !m_swapchainObject.IsImageContentDefined(source.swapchainImageIndex)) {
            // Named rather than silent: the readback hands back whatever the image holds. The
            // ordinary cause is a readback of the default framebuffer applied after the Present
            // that consumed it, where the app's own colour is still what is expected (see
            // m_defaultFramebufferImageIndex).
            MGLOG_W_ONCE("DirectVulkan::ReadPixels: no write is recorded for swapchain image %u of the "
                         "default framebuffer; the readback returns its raw pixels",
                         source.swapchainImageIndex);
        }
        m_textureManager->FlushPendingUploads();
        TransitionWireImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            ReadDepthStencilImageToClient(source.image,source.format,
                source.trackedLayout ? source.trackedLayout : &source.layout,aspect,
                source.level,source.layer,x,y,width,height,format,type,pixels,source.isDefault);
            if (source.isDefault) m_swapchainObject.SetDepthStencilImageLayout(source.swapchainImageIndex,source.layout);
            return;
        }
        const SizeT texelSize = aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : GetReadbackTexelSize(source.format);
        if (!texelSize) MagmaWireFatal("read-format");
        const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * texelSize;
        VkBufferObject buffer;
        if (!buffer.Create({.allocator=m_allocator, .size=size, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .memoryUsage=VMA_MEMORY_USAGE_AUTO, .allocationFlags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT}))
            MagmaWireFatal("read-buffer-allocation");
        VkOffset2D offset{x,y};
        VkExtent2D extent{static_cast<Uint32>(width), static_cast<Uint32>(height)};
        if (source.isDefault && !MapDefaultFramebufferReadbackRect(x,y,width,height,source.extent,
            m_swapchainObject.GetPreTransform(),&offset,&extent)) MagmaWireFatal("read-surface-transform");
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect,source.level,source.is3D ? 0u : source.layer,1};
        region.imageOffset = {offset.x,offset.y,source.is3D ? static_cast<Int32>(source.layer) : 0};
        region.imageExtent = {extent.width,extent.height,1};
        auto& frame = m_frameContext.GetCurrent();
        vkCmdCopyImageToBuffer(frame.commandBuffer,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              buffer.GetHandle(),1,&region);
        if (!SubmitReadbackCommandsAndWait(frame)) MagmaWireFatal("read-submit");
        const auto* bytes = static_cast<const Uint8*>(buffer.Map());
        if (!bytes || !buffer.Invalidate(size)) MagmaWireFatal("read-map");
        Vector<Uint8> remapped;
        if (source.isDefault) {
            remapped.resize(static_cast<SizeT>(size));
            if (!RemapDefaultFramebufferReadback(bytes,width,height,m_swapchainObject.GetPreTransform(),
                                                texelSize,remapped.data())) MagmaWireFatal("read-remap");
            bytes = remapped.data();
        }
        if (!PackReadbackToClientOrPbo(bytes,source.format,width,height,1,format,type,pixels,false,true))
            MagmaWireFatal("read-conversion");
    }

    void VulkanRenderer::ResolveWireDepthStencil(WireImage source, WireImage destination,
        GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1) {
        const Int64 width = static_cast<Int64>(sx1) - sx0;
        const Int64 height = static_cast<Int64>(sy1) - sy0;
        if (!width || !height) return;
        if (source.isDefault || destination.isDefault || source.is3D || destination.is3D ||
            width <= 0 || height <= 0 || width != static_cast<Int64>(dx1) - dx0 ||
            height != static_cast<Int64>(dy1) - dy0 || sx0 < 0 || sy0 < 0 || dx0 < 0 || dy0 < 0 ||
            static_cast<Uint64>(sx1) > source.extent.width || static_cast<Uint64>(sy1) > source.extent.height ||
            static_cast<Uint64>(dx1) > destination.extent.width || static_cast<Uint64>(dy1) > destination.extent.height)
            MagmaWireFatal("multisample-depth-resolve-region");
        VkImageAspectFlags formatAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (source.format == VK_FORMAT_S8_UINT) formatAspects = VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (source.format == VK_FORMAT_D16_UNORM_S8_UINT || source.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                 source.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            formatAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!m_wireCreateRenderPass2 ||
            ((formatAspects & VK_IMAGE_ASPECT_DEPTH_BIT) && !(m_wireDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) ||
            ((formatAspects & VK_IMAGE_ASPECT_STENCIL_BIT) && !(m_wireStencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)))
            MagmaWireFatal("multisample-depth-resolve-capability");

        // vkCmdResolveImage is color-only. A depth/stencil resolve subpass must
        // write at the source coordinates and may require both aspects to use
        // the same mode. Resolve both into owned scratch storage, then copy
        // only the GL-requested aspect, preserving the other destination aspect.
        auto scratch = MakeUnique<VkTextureManager::TextureResource>();
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = source.format;
        imageInfo.extent = {source.extent.width, source.extent.height, 1};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (vmaCreateImage(m_allocator, &imageInfo, &allocation, &scratch->image, &scratch->allocation, nullptr) != VK_SUCCESS)
            MagmaWireFatal("multisample-depth-resolve-allocation");
        scratch->format = source.format;
        scratch->extent = source.extent;
        scratch->aspect = formatAspects;
        WireImage resolved{};
        resolved.image = scratch->image;
        resolved.format = scratch->format;
        resolved.extent = scratch->extent;
        resolved.aspect = formatAspects;
        resolved.trackedLayout = &scratch->layout;
        TransitionWireImage(source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        TransitionWireImage(resolved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkImageView views[2]{};
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = source.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = source.format;
        viewInfo.subresourceRange = {formatAspects, source.level, 1, source.layer, 1};
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &views[0]));
        viewInfo.image = resolved.image;
        viewInfo.subresourceRange = {formatAspects, 0, 1, 0, 1};
        VK_VERIFY(vkCreateImageView(m_device, &viewInfo, nullptr, &views[1]));
        VkAttachmentDescription2 attachments[2]{};
        for (Uint32 i = 0; i < 2; ++i) {
            auto& attachment = attachments[i];
            attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachment.format = source.format;
            attachment.samples = i == 0 ? source.samples : VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = attachment.stencilLoadOp = i == 0 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
        resolve.depthResolveMode = (formatAspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        resolve.stencilResolveMode = (formatAspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
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
        VkRenderPass pass = VK_NULL_HANDLE;
        VK_VERIFY(m_wireCreateRenderPass2(m_device, &passInfo, nullptr, &pass));
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = source.extent.width;
        framebufferInfo.height = source.extent.height;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer));
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{sx0, sy0}, {static_cast<Uint32>(width), static_cast<Uint32>(height)}};
        auto& frame = m_frameContext.GetCurrent();
        vkCmdBeginRenderPass(frame.commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(frame.commandBuffer);
        // Depth/stencil resolve writes execute in COLOR_ATTACHMENT_OUTPUT,
        // unlike ordinary depth attachment writes. Include both when exposing
        // the resolved image to a transfer; layout-based inference only covers
        // EARLY/LATE_FRAGMENT_TESTS and misses the resolve's store operation.
        if (!VkTextureManager::TransitionImageLayout(frame.commandBuffer, resolved.image, scratch->layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, formatAspects))
            MagmaWireFatal("multisample-depth-resolve-transition");
        resolved.layout = scratch->layout;
        TransitionWireImage(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        UniquePtr<VkBufferObject> aspectBuffer;
        if (source.format == destination.format) {
            // Copy one aspect through its buffer representation. Some packed
            // depth/stencil image-copy paths copy the complete native word even
            // for a single-aspect region. A stencil byte or depth-only word in
            // this GPU buffer cannot replace the destination's other aspect.
            const VkDeviceSize texelSize = source.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 :
                (source.format == VK_FORMAT_D16_UNORM || source.format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4);
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * texelSize;
            aspectBuffer = MakeUnique<VkBufferObject>();
            if (!aspectBuffer->Create({.allocator = m_allocator, .size = bytes,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE}))
                MagmaWireFatal("multisample-depth-resolve-buffer");
            VkBufferImageCopy copy{};
            copy.imageSubresource = {source.aspect, 0, 0, 1};
            copy.imageOffset = {sx0, sy0, 0};
            copy.imageExtent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
            vkCmdCopyImageToBuffer(frame.commandBuffer, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   aspectBuffer->GetHandle(), 1, &copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = aspectBuffer->GetHandle();
            barrier.size = bytes;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 1, &barrier, 0, nullptr);
            copy.imageSubresource = {source.aspect, destination.level, destination.layer, 1};
            copy.imageOffset = {dx0, dy0, 0};
            vkCmdCopyBufferToImage(frame.commandBuffer, aspectBuffer->GetHandle(), destination.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        } else {
            // A logical GL depth format may use different native renderbuffer
            // and texture formats. Re-encode only after producing real resolved
            // samples. Keep all scratch objects local across this helper's flush.
            if (!BlitDepthAcrossFormats(frame, resolved.image, resolved.format, &scratch->layout, 0, 0,
                destination.image, destination.format, destination.trackedLayout, destination.level, destination.layer,
                sx0, sy0, dx0, dy0, static_cast<GLint>(width), static_cast<GLint>(height),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                source.aspect == VK_IMAGE_ASPECT_STENCIL_BIT))
                MagmaWireFatal("multisample-depth-resolve-conversion");
        }
        WireRetiredObjects retired{};
        retired.framebuffers.push_back(framebuffer);
        retired.renderPasses.push_back(pass);
        retired.imageViews = {views[0], views[1]};
        retired.textures.push_back(Move(scratch));
        if (aspectBuffer) retired.buffers.push_back(Move(aspectBuffer));
        RetireWireObjects(Move(retired));
    }

    void VulkanRenderer::BlitWireFramebuffers(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
        RetireWireDrawPass();
        // The scissor test clips blit writes: intersect the destination rectangle with
        // the scissor box and shrink the source proportionally.
        if (MGB_CTX->IsCapabilityEnabled(CapabilityInput::ScissorTest)) {
            const IntVec4& scissor = MGB_CTX->GetScissorBox();
            const auto clipAxis = [](GLint& d0, GLint& d1, GLint& s0, GLint& s1, GLint clipLo, GLint clipHi) -> Bool {
                const Bool dstFlipped = d1 < d0;
                GLint lo = dstFlipped ? d1 : d0;
                GLint hi = dstFlipped ? d0 : d1;
                const GLint newLo = std::max(lo, clipLo);
                const GLint newHi = std::min(hi, clipHi);
                if (newLo >= newHi) {
                    return false;
                }
                const double srcSpan = static_cast<double>(s1 - s0);
                const double dstSpan = static_cast<double>(d1 - d0);
                const double scale = dstSpan != 0.0 ? srcSpan / dstSpan : 0.0;
                const GLint origD0 = d0;
                const GLint clippedD0 = dstFlipped ? newHi : newLo;
                const GLint clippedD1 = dstFlipped ? newLo : newHi;
                s0 = s0 + static_cast<GLint>(std::lround((clippedD0 - origD0) * scale));
                s1 = s0 + static_cast<GLint>(std::lround((clippedD1 - clippedD0) * scale));
                d0 = clippedD0;
                d1 = clippedD1;
                return true;
            };
            if (!clipAxis(dx0, dx1, sx0, sx1, scissor.x(), scissor.x() + scissor.z()) ||
                !clipAxis(dy0, dy1, sy0, sy1, scissor.y(), scissor.y() + scissor.w())) {
                return; // fully scissored out
            }
        }

        auto& state = MG_Pipe::MGPipeApplier();
        const Bool named = !MG_Pipe::MGPipeHandleIsNull(state.VerbBlitReadFbo) ||
                           !MG_Pipe::MGPipeHandleIsNull(state.VerbBlitDrawFbo);
        const auto* read = named ? state.FramebufferRecordFor(state.VerbBlitReadFbo) : state.ReadFramebuffer();
        const auto* draw = named ? state.FramebufferRecordFor(state.VerbBlitDrawFbo) : state.DrawFramebuffer();
        if (!read || !draw) MagmaWireFatal("blit-framebuffer-record");
        state.VerbBlitNamedConsumed = named;
        const auto blit = [&](const MG_Pipe::MGPSurface& src, const MG_Pipe::MGPSurface& dst,
                              VkImageAspectFlags aspect) {
            WireImage source = ResolveWireImage(*read,src,aspect, /*isWriteTarget=*/false);
            WireImage destination = ResolveWireImage(*draw,dst,aspect, /*isWriteTarget=*/true);
            if (!source.image || !destination.image) return;
            m_textureManager->FlushPendingUploads();
            if (aspect != VK_IMAGE_ASPECT_COLOR_BIT && source.samples != VK_SAMPLE_COUNT_1_BIT &&
                destination.samples == VK_SAMPLE_COUNT_1_BIT) {
                ResolveWireDepthStencil(source, destination, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
                if (dst.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                    m_textureManager->MarkWireTextureGpuWritten(destination.storage, destination.level, destination.layer, 1);
                return;
            }
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT && source.samples != VK_SAMPLE_COUNT_1_BIT &&
                destination.samples == VK_SAMPLE_COUNT_1_BIT) {
                // A resolve cannot flip axes or convert RGBA to a BGRA/default
                // attachment. Resolve into the source format first, then use
                // the existing single-sample blit for those GL semantics.
                const Int64 width = std::abs(static_cast<Int64>(sx1) - sx0);
                const Int64 height = std::abs(static_cast<Int64>(sy1) - sy0);
                if (!width || !height) return;
                if (source.isDefault || source.is3D || width != std::abs(static_cast<Int64>(dx1) - dx0) ||
                    height != std::abs(static_cast<Int64>(dy1) - dy0) ||
                    std::min(sx0, sx1) < 0 || std::min(sy0, sy1) < 0 ||
                    static_cast<Uint64>(std::max(sx0, sx1)) > source.extent.width ||
                    static_cast<Uint64>(std::max(sy0, sy1)) > source.extent.height)
                    MagmaWireFatal("multisample-resolve-region");
                auto scratch = MakeUnique<VkTextureManager::TextureResource>();
                VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                info.imageType = VK_IMAGE_TYPE_2D;
                info.format = source.format;
                info.extent = {static_cast<Uint32>(width), static_cast<Uint32>(height), 1};
                info.mipLevels = info.arrayLayers = 1;
                info.samples = VK_SAMPLE_COUNT_1_BIT;
                info.tiling = VK_IMAGE_TILING_OPTIMAL;
                info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VmaAllocationCreateInfo allocation{};
                allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                if (vmaCreateImage(m_allocator, &info, &allocation, &scratch->image, &scratch->allocation, nullptr) != VK_SUCCESS)
                    MagmaWireFatal("multisample-resolve-allocation");
                scratch->format = source.format;
                scratch->extent = {info.extent.width, info.extent.height};
                scratch->mipLevels = scratch->arrayLayers = scratch->depth = 1;
                scratch->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                WireImage resolved{};
                resolved.image = scratch->image;
                resolved.format = scratch->format;
                resolved.extent = scratch->extent;
                resolved.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                resolved.trackedLayout = &scratch->layout;
                TransitionWireImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                TransitionWireImage(resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageResolve resolve{};
                resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, source.level, source.layer, 1};
                resolve.srcOffset = {std::min(sx0, sx1), std::min(sy0, sy1), 0};
                resolve.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                resolve.extent = info.extent;
                vkCmdResolveImage(m_frameContext.GetCurrent().commandBuffer, source.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolved.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
                const GLint rx0 = sx1 > sx0 ? 0 : static_cast<GLint>(width);
                const GLint rx1 = sx1 > sx0 ? static_cast<GLint>(width) : 0;
                const GLint ry0 = sy1 > sy0 ? 0 : static_cast<GLint>(height);
                const GLint ry1 = sy1 > sy0 ? static_cast<GLint>(height) : 0;
                if (destination.isDefault) {
                    if (!BlitWireColorToDefault(resolved, destination, rx0, ry0, rx1, ry1,
                                                dx0, dy0, dx1, dy1, filter))
                        MagmaWireFatal("multisample-default-blit");
                    m_swapchainObject.SetImageContentDefined(destination.swapchainImageIndex, true);
                } else {
                    TransitionWireImage(resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    TransitionWireImage(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkImageBlit copy{};
                    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.srcOffsets[0] = {rx0, ry0, 0}; copy.srcOffsets[1] = {rx1, ry1, 1};
                    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, destination.level,
                        destination.is3D ? 0u : destination.layer, 1};
                    const Int32 z = destination.is3D ? static_cast<Int32>(destination.layer) : 0;
                    copy.dstOffsets[0] = {dx0, dy0, z}; copy.dstOffsets[1] = {dx1, dy1, z + 1};
                    vkCmdBlitImage(m_frameContext.GetCurrent().commandBuffer, resolved.image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &copy, filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
                    if (dst.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                        m_textureManager->MarkWireTextureGpuWritten(destination.storage, destination.level, destination.layer, 1);
                }
                // The shader blit's views were retired first. Keep their backing
                // image until the same submit completes; resizing a shared
                // scratch image here would destroy an older queued resolve.
                WireRetiredObjects retired{};
                retired.textures.push_back(Move(scratch));
                RetireWireObjects(Move(retired));
                return;
            }
            // GL names logical window coordinates. Android's native swapchain may
            // be quarter-turned (e.g. logical 2620x1280 over an image 1280x2620).
            // An axis-aligned vkCmdBlitImage cannot express that rotation; issuing
            // the logical rectangle directly also writes outside the native image.
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT && destination.isDefault && !source.isDefault &&
                source.samples == VK_SAMPLE_COUNT_1_BIT && destination.samples == VK_SAMPLE_COUNT_1_BIT &&
                source.image != destination.image) {
                if (!BlitWireColorToDefault(source, destination, sx0, sy0, sx1, sy1,
                                            dx0, dy0, dx1, dy1, filter))
                    MagmaWireFatal("default-color-blit-shape@P7");
                m_swapchainObject.SetImageContentDefined(destination.swapchainImageIndex, true);
                return;
            }
            const Bool sameImage = source.image == destination.image;
            const VkImageLayout srcLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkImageLayout dstLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            TransitionWireImage(source,srcLayout);
            if (sameImage) destination.layout = source.layout;
            else TransitionWireImage(destination,dstLayout);
            VkImageBlit region{};
            region.srcSubresource = {aspect,source.level,source.layer,1};
            region.dstSubresource = {aspect,destination.level,destination.layer,1};
            region.srcOffsets[0] = {sx0,sy0,0}; region.srcOffsets[1] = {sx1,sy1,1};
            region.dstOffsets[0] = {dx0,dy0,0}; region.dstOffsets[1] = {dx1,dy1,1};
            // Headless and identity surfaces store GL's bottom row at Vulkan's last row.
            if (source.isDefault) {
                region.srcOffsets[0].y = static_cast<GLint>(source.extent.height)-sy0;
                region.srcOffsets[1].y = static_cast<GLint>(source.extent.height)-sy1;
            }
            if (destination.isDefault) {
                region.dstOffsets[0].y = static_cast<GLint>(destination.extent.height)-dy0;
                region.dstOffsets[1].y = static_cast<GLint>(destination.extent.height)-dy1;
            }
            auto command = m_frameContext.GetCurrent().commandBuffer;
            if (source.samples != VK_SAMPLE_COUNT_1_BIT || destination.samples != VK_SAMPLE_COUNT_1_BIT) {
                const Int32 width = region.srcOffsets[1].x - region.srcOffsets[0].x;
                const Int32 height = region.srcOffsets[1].y - region.srcOffsets[0].y;
                if (width <= 0 || height <= 0 || width != region.dstOffsets[1].x - region.dstOffsets[0].x ||
                    height != region.dstOffsets[1].y - region.dstOffsets[0].y || source.format != destination.format)
                    MagmaWireFatal("multisample-blit-shape@P7");
                if (source.samples != VK_SAMPLE_COUNT_1_BIT && destination.samples == VK_SAMPLE_COUNT_1_BIT &&
                    aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                    VkImageResolve resolve{region.srcSubresource, region.srcOffsets[0],
                        region.dstSubresource, region.dstOffsets[0], {static_cast<Uint32>(width), static_cast<Uint32>(height), 1}};
                    vkCmdResolveImage(command, source.image, srcLayout, destination.image, dstLayout, 1, &resolve);
                } else if (source.samples == destination.samples) {
                    VkImageCopy copy{region.srcSubresource, region.srcOffsets[0], region.dstSubresource,
                        region.dstOffsets[0], {static_cast<Uint32>(width), static_cast<Uint32>(height), 1}};
                    vkCmdCopyImage(command, source.image, srcLayout, destination.image, dstLayout, 1, &copy);
                } else MagmaWireFatal("multisample-blit-aspect@P7");
            } else {
                vkCmdBlitImage(command,source.image,srcLayout,destination.image,dstLayout,1,&region,
                    filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            }
            if (!destination.isDefault && dst.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                m_textureManager->MarkWireTextureGpuWritten(destination.storage, destination.level, destination.layer, destination.layers);
            if (destination.isDefault) {
                if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) m_swapchainObject.SetImageContentDefined(destination.swapchainImageIndex,true);
                else m_swapchainObject.SetDepthStencilContentDefined(destination.swapchainImageIndex,true);
            }
        };
        if (mask & GL_COLOR_BUFFER_BIT) {
            for (Uint32 i=0;i<MG_Pipe::kMGPipeMaxColorAttachments;++i) {
                const Int32 attachment = draw->DrawBuffers[i];
                if (attachment>=0 && attachment<static_cast<Int32>(MG_Pipe::kMGPipeMaxColorAttachments))
                    blit(read->ReadSurface,draw->Color[attachment],VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }
        if (mask & GL_DEPTH_BUFFER_BIT) blit(read->Depth,draw->Depth,VK_IMAGE_ASPECT_DEPTH_BIT);
        if (mask & GL_STENCIL_BUFFER_BIT) blit(read->Stencil,draw->Stencil,VK_IMAGE_ASPECT_STENCIL_BIT);
    }
    void VulkanRenderer::CopyWireFramebufferToTexture(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                      GLint x, GLint y, GLsizei width, GLsizei height) {
        if (width <= 0 || height <= 0) return;
        if (target != GL_TEXTURE_2D) {
            RecordTextureCopyError(__func__,ErrorCode::InvalidOperation,
                                   "CopyTexSubImage2D currently only supports GL_TEXTURE_2D destinations.");
            return;
        }
        RetireWireDrawPass();
        auto& state = MG_Pipe::MGPipeApplier();
        const auto* read = state.ReadFramebuffer();
        if (!read) MagmaWireFatal("copy-framebuffer-record");
        const auto* texture = m_textureManager->SyncTextureResourceByHandle(state.VerbCopyTexDst);
        if (!texture) MagmaWireFatal("copy-destination-record");
        const VkImageAspectFlags aspect = texture->aspect;
        MG_Pipe::MGPFramebufferState fake{};
        MG_Pipe::MGPSurface surface{};
        surface.Kind = MG_Pipe::kMGPipeSurfaceKindTexture;
        surface.Res = state.VerbCopyTexDst;
        surface.Level = static_cast<Uint16>(level);
        WireImage source = ResolveWireImage(*read,aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? read->Depth :
            aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? read->Stencil : read->ReadSurface,aspect,
            /*isWriteTarget=*/false);
        WireImage destination = ResolveWireImage(fake,surface,aspect, /*isWriteTarget=*/false);
        if (!source.image || !destination.image) return;
        m_textureManager->FlushPendingUploads();
        const Bool sameImage = source.image == destination.image;
        const VkImageLayout srcLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkImageLayout dstLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        TransitionWireImage(source,srcLayout);
        if (sameImage) destination.layout = source.layout;
        else TransitionWireImage(destination,dstLayout);
        VkImageBlit region{};
        region.srcSubresource = {aspect,source.level,source.layer,1};
        region.dstSubresource = {aspect,destination.level,destination.layer,1};
        region.srcOffsets[0] = {x,y,0}; region.srcOffsets[1] = {x+width,y+height,1};
        region.dstOffsets[0] = {xoffset,yoffset,0}; region.dstOffsets[1] = {xoffset+width,yoffset+height,1};
        if (source.isDefault) {
            region.srcOffsets[0].y = static_cast<Int32>(source.extent.height)-y;
            region.srcOffsets[1].y = static_cast<Int32>(source.extent.height)-y-height;
        }
        vkCmdBlitImage(m_frameContext.GetCurrent().commandBuffer,source.image,srcLayout,
            destination.image,dstLayout,1,&region,VK_FILTER_NEAREST);
        m_textureManager->MarkWireTextureGpuWritten(destination.storage,destination.level,destination.layer);
    }

    void VulkanRenderer::GenerateWireMipmap() {
        RetireWireDrawPass();
        auto& state = MG_Pipe::MGPipeApplier();
        const auto object = state.VerbMipRes;
        if (MG_Pipe::MGPipeHandleIsNull(object) || object.Slot >= state.TextureResources.size())
            MagmaWireFatal("mipmap-record");
        const auto& objectRecord = state.TextureResources[object.Slot];
        if (!objectRecord.Live || objectRecord.Gen != object.Gen) MagmaWireFatal("mipmap-record-generation");
        // LevelCount is the logical end-exclusive level, not the number of
        // levels after BaseLevel. Clip in the view's coordinates before the
        // resolver adds its root-storage offset.
        const Uint32 logicalBase = state.VerbMipBaseLevel;
        Uint32 logicalEnd = std::min<Uint32>(state.VerbMipLevelCount, objectRecord.Desc.Levels);
        if (objectRecord.ParamsSerial)
            logicalEnd = std::min(logicalEnd, static_cast<Uint32>(objectRecord.Params.MaxLevel) + 1);
        if (!MG_Pipe::MGPipeHandleIsNull(objectRecord.Desc.ViewOf)) {
            const auto viewHandle = objectRecord.ViewCso;
            if (MG_Pipe::MGPipeHandleIsNull(viewHandle) || viewHandle.Slot >= state.SamplerViewCsos.size())
                MagmaWireFatal("mipmap-view-record");
            const auto& view = state.SamplerViewCsos[viewHandle.Slot];
            if (!view.Live || view.Gen != viewHandle.Gen || view.View.Texture != object)
                MagmaWireFatal("mipmap-view-generation");
            logicalEnd = std::min<Uint32>(logicalEnd, view.View.NumLevels);
        }
        if (logicalEnd <= logicalBase + 1) return;
        Uint32 base = logicalBase, baseLayer = 0, layerCount = 1;
        VkFormat viewFormat = VK_FORMAT_UNDEFINED;
        const auto handle = m_textureManager->ResolveWireTextureStorage(state.VerbMipRes, base, baseLayer,
                                                                       &viewFormat, &layerCount);
        auto* resource = m_textureManager->SyncTextureResourceByHandle(handle);
        if (!resource) MagmaWireFatal("mipmap-resource");
        m_textureManager->FlushPendingUploads();
        RetireWireDrawPass();
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        Uint32 fullLevelCount = 1;
        for (Uint32 size = std::max({resource->extent.width, resource->extent.height, resource->depth});
             size > 1; size >>= 1) ++fullLevelCount;
        if (base >= fullLevelCount) MagmaWireFatal("mipmap-base-extent");
        const Uint32 count = std::min(logicalEnd - logicalBase, fullLevelCount - base);
        if (count <= 1) return;
        if (!m_textureManager->GrowWireTextureMipChain(handle,base+count,frame.commandBuffer))
            MagmaWireFatal("mipmap-growth");
        if (resource->aspect != VK_IMAGE_ASPECT_COLOR_BIT && resource->aspect != VK_IMAGE_ASPECT_DEPTH_BIT)
            MagmaWireFatal("depth-stencil-mipmap@P7");
        VkFormatProperties props{};
        const VkFormat format = viewFormat == VK_FORMAT_UNDEFINED ? resource->format : viewFormat;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle,format,&props);
        const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        const Bool nativeBlit = format == resource->format && (props.optimalTilingFeatures & required) == required;
        const VkFilter filter = props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ?
                                VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (!nativeBlit) {
            // Packed floating-point color formats may support sampling and
            // rendering without BLIT_DST. Generate from actual GPU mip contents
            // through the native shader pass; no client or staged texels enter.
            if (resource->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
                resource->viewType == VK_IMAGE_VIEW_TYPE_1D || resource->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
                resource->viewType == VK_IMAGE_VIEW_TYPE_3D ||
                !(resource->usageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
                baseLayer >= resource->arrayLayers || layerCount > resource->arrayLayers - baseLayer)
                MagmaWireFatal("mipmap-shader-format-or-shape");
            for (Uint32 level = base + 1; level < base + count; ++level) {
                for (Uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
                    WireImage source{};
                    source.image = resource->image;
                    source.format = format;
                    source.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    source.layout = resource->layout;
                    source.trackedLayout = &resource->layout;
                    source.levels = resource->mipLevels;
                    source.level = level - 1;
                    source.layer = layer;
                    source.extent = {std::max(resource->extent.width >> (level - 1), 1u),
                                     std::max(resource->extent.height >> (level - 1), 1u)};
                    WireImage destination = source;
                    destination.level = level;
                    destination.extent = {std::max(resource->extent.width >> level, 1u),
                                          std::max(resource->extent.height >> level, 1u)};
                    if (!BlitWireColorImage(source, destination, 0, 0, source.extent.width, source.extent.height,
                        0, 0, destination.extent.width, destination.extent.height,
                        filter == VK_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST, true))
                        MagmaWireFatal("mipmap-shader-blit");
                }
            }
        } else {
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            VkAccessFlags access = 0;
            GetImageTransitionSourceState(resource->layout,stage,access);
            if (!VkTextureManager::TransitionImageLayout(frame.commandBuffer,resource->image,resource->layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,stage,VK_PIPELINE_STAGE_TRANSFER_BIT,access,
                VK_ACCESS_TRANSFER_WRITE_BIT,resource->aspect,0,resource->mipLevels)) MagmaWireFatal("mipmap-transition");
            for (Uint32 level=base+1;level<base+count;++level) {
                VkImageLayout previous = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                VkTextureManager::TransitionImageLayout(frame.commandBuffer,resource->image,previous,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,resource->aspect,level-1,1);
                VkImageBlit blit{};
                blit.srcSubresource = {resource->aspect,level-1,baseLayer,layerCount};
                blit.dstSubresource = {resource->aspect,level,baseLayer,layerCount};
                blit.srcOffsets[1] = {static_cast<Int32>(std::max(resource->extent.width>>(level-1),1u)),
                    static_cast<Int32>(std::max(resource->extent.height>>(level-1),1u)),
                    static_cast<Int32>(std::max(resource->depth>>(level-1),1u))};
                blit.dstOffsets[1] = {static_cast<Int32>(std::max(resource->extent.width>>level,1u)),
                    static_cast<Int32>(std::max(resource->extent.height>>level,1u)),
                    static_cast<Int32>(std::max(resource->depth>>level,1u))};
                vkCmdBlitImage(frame.commandBuffer,resource->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              resource->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
                VkTextureManager::TransitionImageLayout(frame.commandBuffer,resource->image,previous,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,resource->aspect,level-1,1);
            }
            resource->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        auto& record = state.TextureResources[handle.Slot];
        record.Desc.Levels = std::max<Uint16>(record.Desc.Levels,static_cast<Uint16>(base+count));
        resource->syncedWireSerial = ++record.Serial;
        auto& store = MG_Remote::Server::ServerStagedTexture();
        const Uint64 key = MG_Remote::Server::StagedTextureStore::KeyForHandle(handle);
        const auto target = static_cast<MG_Pipe::MGPipeResourceTarget>(record.Desc.Target);
        // The resource descriptor determines the upload-target set; no texture unit or
        // client upload-target vector participates in defining the generated shadow.
        for (Uint32 face = target == MG_Pipe::MGPipeResourceTarget::TexCube ? baseLayer : 0;
             face < (target == MG_Pipe::MGPipeResourceTarget::TexCube ? baseLayer + layerCount : 1u); ++face) {
            TextureUploadTarget upload = TextureUploadTarget::Texture2D;
            switch (target) {
            case MG_Pipe::MGPipeResourceTarget::Tex1D: upload=TextureUploadTarget::Texture1D; break;
            case MG_Pipe::MGPipeResourceTarget::Tex3D: upload=TextureUploadTarget::Texture3D; break;
            case MG_Pipe::MGPipeResourceTarget::Tex1DArray: upload=TextureUploadTarget::Texture1DArray; break;
            case MG_Pipe::MGPipeResourceTarget::Tex2DArray: upload=TextureUploadTarget::Texture2DArray; break;
            case MG_Pipe::MGPipeResourceTarget::TexCubeArray: upload=TextureUploadTarget::CubeMapArray; break;
            case MG_Pipe::MGPipeResourceTarget::TexCube:
                upload=static_cast<TextureUploadTarget>(static_cast<Uint32>(TextureUploadTarget::CubeMapPositiveX)+face); break;
            default: break;
            }
            for (Uint32 level=base+1;level<base+count;++level) {
                const IntVec3 extent = MG_Remote::Server::StagedTextureMipExtent(
                    record.Desc.Target, record.Desc.Width, record.Desc.Height, record.Desc.Depth, level);
                store.NoteLevelDefined(key,static_cast<Uint16>(upload),static_cast<Uint16>(level),extent);
                store.MarkLevelGpuDirty(key,static_cast<Uint16>(upload),static_cast<Uint16>(level),true);
            }
        }
    }
#endif
