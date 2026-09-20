// P5f fm: included by VulkanRenderer.cpp inside the backend namespace. All inputs here
// belong to the applier; no frontend object, lifetime-id lookup or allocator is used.
#if MOBILEGL_BUILD_DISAGGREGATED
    [[noreturn]] static void MagmaWireFatal(const char* detail) {
        MGLOG_F("MGPipe: Fatal{UnmigratedVerb, \"Magma:%s\"}", detail);
        std::abort();
    }

    VulkanRenderer::WireImage VulkanRenderer::ResolveWireImage(
        const MG_Pipe::MGPFramebufferState& fbo, const MG_Pipe::MGPSurface& surface,
        VkImageAspectFlags aspect) {
        WireImage out{};
        out.aspect = aspect;
        if (fbo.IsDefault) {
            out.isDefault = true;
            out.extent = m_swapchainObject.GetExtent();
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
                out.image = m_swapchainObject.GetImage(m_imageIndexAcquired);
                out.format = m_swapchainObject.GetSurfaceFormat().format;
                out.layout = m_swapchainObject.GetImageLayout(m_imageIndexAcquired);
            } else {
                out.image = m_swapchainObject.GetDepthStencilImage(m_imageIndexAcquired);
                out.format = m_swapchainObject.GetDepthStencilFormat();
                out.layout = m_swapchainObject.GetDepthStencilImageLayout(m_imageIndexAcquired);
            }
            return out;
        }
        if (surface.Kind == MG_Pipe::kMGPipeSurfaceKindNone) return out;
        const Bool renderbuffer = surface.Kind == MG_Pipe::kMGPipeSurfaceKindRenderbuffer;
        Uint32 level = surface.Level, layer = surface.Layer, availableLayers = 1;
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
        if (!VkTextureManager::TransitionImageLayout(frame.commandBuffer, image.image, image.layout, layout,
            stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, access,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, barrierAspect, 0, image.levels))
            MagmaWireFatal("image-transition");
        if (image.trackedLayout) *image.trackedLayout = image.layout;
        if (image.isDefault) {
            if (image.aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                m_swapchainObject.SetImageLayout(m_imageIndexAcquired, image.layout);
            else m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired, image.layout);
        }
    }

    void VulkanRenderer::ClearWireFramebuffer(const MG_Pipe::MGPFramebufferState& fbo,
                                              const ClearAttachmentPayload& payload, GLint drawbuffer) {
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("wire-verb-flush");
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
            WireImage image = ResolveWireImage(fbo, surface, aspect);
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
            viewInfo.viewType = image.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
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
            // P5f changes ownership, not Magma's wait contract. Synchronous retirement keeps
            // these temporary server resources bounded and makes their lifetime explicit.
            if (!SubmitReadbackCommandsAndWait(frame)) MagmaWireFatal("clear-submit");
            vkDestroyFramebuffer(m_device, fb, nullptr);
            vkDestroyRenderPass(m_device, pass, nullptr);
            vkDestroyImageView(m_device, view, nullptr);
            if (!image.isDefault && surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                m_textureManager->MarkWireTextureGpuWritten(image.storage, image.level, image.layer, image.layers);
            if (image.isDefault) {
                if (color) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, true);
                else m_swapchainObject.SetDepthStencilContentDefined(m_imageIndexAcquired, true);
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
        WireImage source = ResolveWireImage(*fbo, surface, aspect);
        if (!source.image) return;
        m_textureManager->FlushPendingUploads();
        TransitionWireImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            ReadDepthStencilImageToClient(source.image,source.format,
                source.trackedLayout ? source.trackedLayout : &source.layout,aspect,
                source.level,source.layer,x,y,width,height,format,type,pixels,source.isDefault);
            if (source.isDefault) m_swapchainObject.SetDepthStencilImageLayout(m_imageIndexAcquired,source.layout);
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

    void VulkanRenderer::BlitWireFramebuffers(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("wire-verb-flush");
        auto& state = MG_Pipe::MGPipeApplier();
        const Bool named = !MG_Pipe::MGPipeHandleIsNull(state.VerbBlitReadFbo) ||
                           !MG_Pipe::MGPipeHandleIsNull(state.VerbBlitDrawFbo);
        const auto* read = named ? state.FramebufferRecordFor(state.VerbBlitReadFbo) : state.ReadFramebuffer();
        const auto* draw = named ? state.FramebufferRecordFor(state.VerbBlitDrawFbo) : state.DrawFramebuffer();
        if (!read || !draw) MagmaWireFatal("blit-framebuffer-record");
        state.VerbBlitNamedConsumed = named;
        const auto blit = [&](const MG_Pipe::MGPSurface& src, const MG_Pipe::MGPSurface& dst,
                              VkImageAspectFlags aspect) {
            WireImage source = ResolveWireImage(*read,src,aspect);
            WireImage destination = ResolveWireImage(*draw,dst,aspect);
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
                if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired,true);
                else m_swapchainObject.SetDepthStencilContentDefined(m_imageIndexAcquired,true);
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
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("wire-verb-flush");
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
            aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? read->Stencil : read->ReadSurface,aspect);
        WireImage destination = ResolveWireImage(fake,surface,aspect);
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
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("wire-verb-flush");
        auto& state = MG_Pipe::MGPipeApplier();
        Uint32 base = state.VerbMipBaseLevel, baseLayer = 0, layerCount = 1;
        const auto handle = m_textureManager->ResolveWireTextureStorage(state.VerbMipRes, base, baseLayer,
                                                                       nullptr, &layerCount);
        auto* resource = m_textureManager->SyncTextureResourceByHandle(handle);
        if (!resource) MagmaWireFatal("mipmap-resource");
        m_textureManager->FlushPendingUploads();
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("mipmap-flush");
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        const Uint32 count = state.VerbMipLevelCount;
        if (count <= 1) return;
        if (!m_textureManager->GrowWireTextureMipChain(handle,base+count,frame.commandBuffer))
            MagmaWireFatal("mipmap-growth");
        if (resource->aspect != VK_IMAGE_ASPECT_COLOR_BIT && resource->aspect != VK_IMAGE_ASPECT_DEPTH_BIT)
            MagmaWireFatal("depth-stencil-mipmap@P7");
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle,resource->format,&props);
        const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((props.optimalTilingFeatures & required) != required) MagmaWireFatal("mipmap-native-format@P7");
        const VkFilter filter = props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ?
                                VK_FILTER_LINEAR : VK_FILTER_NEAREST;
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
