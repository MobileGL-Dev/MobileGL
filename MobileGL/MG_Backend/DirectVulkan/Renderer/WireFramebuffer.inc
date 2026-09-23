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

    // P7 wave 2-B. THE NAME STAYS FIRST AND THE DRIVER'S ANSWER FOLLOWS IT. Two Iris cases
    // died on the Redmi (Adreno 830v2) with `Fatal{UnmigratedVerb, "Magma:mipmap-shader-
    // format-or-shape"}` and that line was the whole evidence: five disjunctions share the
    // name, so the one thing the device could have told us - WHICH format, at what shape,
    // missing WHICH feature bit - was the one thing it did not. The census, the retrace
    // refusal tally and every log grep key on the leading word, so it is unchanged and
    // unprefixed; everything after the first space is for the reader of the next device run.
    static String MagmaWireShapeDetail(const char* name, VkFormat format, VkFormat storageFormat,
                                       const VkFormatProperties& props, VkImageAspectFlags aspect,
                                       VkImageViewType viewType, VkImageUsageFlags usage,
                                       const VkExtent2D& extent, Uint32 depth, Uint32 levels,
                                       Uint32 baseLayer, Uint32 layerCount, Uint32 arrayLayers) {
        const auto hex = [](Uint32 value) {
            static const char* digits = "0123456789abcdef";
            String out = "0x";
            Bool leading = true;
            for (Int shift = 28; shift >= 0; shift -= 4) {
                const Uint32 nibble = (value >> shift) & 0xfu;
                if (nibble == 0 && leading && shift) continue;
                leading = false;
                out.push_back(digits[nibble]);
            }
            return out;
        };
        const VkFormatFeatureFlags absent = ~props.optimalTilingFeatures;
        String missing;
        const auto note = [&](VkFormatFeatureFlags bit, const char* word) {
            if (!(absent & bit)) return;
            if (!missing.empty()) missing += "|";
            missing += word;
        };
        note(VK_FORMAT_FEATURE_BLIT_SRC_BIT, "BLIT_SRC");
        note(VK_FORMAT_FEATURE_BLIT_DST_BIT, "BLIT_DST");
        note(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED");
        note(VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "SAMPLED_LINEAR");
        note(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT");
        note(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT");
        if (missing.empty()) missing = "none";
        String out = name;
        out += " format=" + std::to_string(static_cast<Int>(format));
        // A view that reinterprets its owner's format is the OTHER reason the native blit
        // arm is skipped, and it is invisible in the format number alone.
        if (storageFormat != format) out += " storage=" + std::to_string(static_cast<Int>(storageFormat));
        out += " extent=" + std::to_string(extent.width) + "x" + std::to_string(extent.height) +
               "x" + std::to_string(depth);
        out += " levels=" + std::to_string(levels);
        out += " aspect=" + hex(static_cast<Uint32>(aspect));
        out += " view=" + std::to_string(static_cast<Int>(viewType));
        out += " usage=" + hex(static_cast<Uint32>(usage));
        out += " layers=" + std::to_string(baseLayer) + "+" + std::to_string(layerCount) +
               "/" + std::to_string(arrayLayers);
        out += " missing=" + missing;
        return out;
    }

    // THE HOST GATE CANNOT REACH THE SHADER MIP ARM ON ITS OWN. lavapipe reports
    // BLIT_SRC|BLIT_DST for every colour format this lane can create, so `nativeBlit` is
    // always true there and both shader shapes below would only ever run on the phone -
    // which is how the Adreno refusal reached a release in the first place. The knob makes
    // them reachable from a host lane and is read exactly once:
    //   1 - drop the native blit (the arm an Adreno takes for a format without BLIT_DST);
    //   2 - also hide the destination's COLOR_ATTACHMENT usage (the arm it takes for a
    //       sampled-only texture, which renders through owned scratch and copies back).
    // MGITEST_, not MOBILEGL_: it is a test knob, it is not mirrored into the accepted-env
    // table, and this file is compiled only into the disaggregated build.
    static Uint32 MagmaWireForcedShaderMipmapTier() {
        static const Uint32 tier = [] {
            const char* value = std::getenv("MGITEST_MAGMA_FORCE_SHADER_MIPMAP");
            if (!value || !*value) return 0u;
            return static_cast<Uint32>(std::strtoul(value, nullptr, 10));
        }();
        return tier;
    }

    // THE SAME PROBLEM FOR THE ROTATED DEFAULT-BLIT ARM. Every surface this lane can create is
    // VK_SURFACE_TRANSFORM_IDENTITY - headless, pbuffer or X11 - and the Redmi evidence for this
    // cluster is a pbuffer too, so the scratch arm that exists FOR rotation would never execute
    // anywhere a test could watch it. The knob forces it on an identity surface, where it has to
    // produce exactly the pixels the fall-through arm produces; that equality is the assertion.
    // It does NOT fake a rotation: nothing downstream is told the surface turned.
    static Bool MagmaWireForcedDefaultBlitScratch() {
        static const Bool forced = [] {
            const char* value = std::getenv("MGITEST_MAGMA_FORCE_DEFAULT_BLIT_SCRATCH");
            return value && *value && value[0] != '0';
        }();
        return forced;
    }

    // AND THE SAME PROBLEM AGAIN FOR THE SHADER MULTISAMPLE RESOLVE (P7 wave 2-B2). lavapipe
    // carries VK_KHR_create_renderpass2 AND VK_KHR_depth_stencil_resolve, so the extension arm
    // is taken for every depth/stencil resolve this host can run and the shader arm - the one
    // that exists for the device WITHOUT them, which is where the session used to die at
    // `Magma:multisample-depth-resolve-capability` - would never execute under any lane. The
    // knob drops the extension arm. It does not pretend the extension is missing anywhere else:
    // nothing but the arm choice reads it.
    static Bool MagmaWireForcedShaderDepthResolve() {
        static const Bool forced = [] {
            const char* value = std::getenv("MGITEST_MAGMA_FORCE_SHADER_DEPTH_RESOLVE");
            return value && *value && value[0] != '0';
        }();
        return forced;
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
        // P7 wave 2-B2: the shader multisample resolve SAMPLES this image, which is a question
        // about the image and not about the format - VkTextureManager asks for SAMPLED on every
        // attachment it creates, and a device that declined it would make the descriptor write
        // undefined rather than fail. Carried here so the arm can decline instead.
        out.usageFlags = resource->usageFlags;
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

    // P7 wave 2-B (exit gate 3). THE READBACK SAYS WHAT IT IS WAITING FOR INSTEAD OF INFERRING IT.
    //
    // Every other transition in this file derives its barrier's SOURCE scope from the image's
    // tracked layout, through GetImageTransitionSourceState (VulkanRenderer.cpp). That is right
    // for a transition - but two of its arms, VK_IMAGE_LAYOUT_UNDEFINED and
    // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT with no access
    // bits, which is Vulkan's spelling of AN EMPTY FIRST SCOPE: wait for nothing. For a
    // transition out of UNDEFINED that is correct (the contents are being discarded); for a
    // READBACK it is not, because the pixels being copied were written by the very commands the
    // barrier declines to wait for. A just-presented default framebuffer sits in exactly the
    // second of those layouts.
    //
    // It matters on the wire arm and not on the monolith one because of where the draws are.
    // ReadWirePixels' FlushPendingCommands has already put the frame's draws on the queue under a
    // pooled fence and returned WITHOUT waiting, so the copy recorded below lands in a LATER
    // submission and this barrier is the only thing that could order the two. The monolith
    // readback never flushes first: its draws and its copy are one submission under one fence.
    //
    // Measured on the host (OpenRA, DirectVulkan, inproc): at this point the counters read
    // submit=72 completed=71 - the draws' submission is in flight and unwaited. The host does not
    // diverge because its default framebuffer's tracked layout here is GENERAL, which takes
    // GetImageTransitionSourceState's `default:` arm (ALL_COMMANDS / MEMORY_WRITE) and orders the
    // copy by accident. The device's is not.
    void VulkanRenderer::RecordWireReadbackWriteBarrier() {
        auto& frame = m_frameContext.GetCurrent();
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (VkRenderPassManager::GetActiveRenderPass()) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        // ALL_COMMANDS as the source scope reaches back across the submission boundary: a
        // barrier's first scope is every command earlier in SUBMISSION order on this queue, not
        // merely earlier in this command buffer.
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
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
        RecordWireReadbackWriteBarrier();
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

    Bool VulkanRenderer::DeclineWireDepthStencilResolveShape(const WireImage& source,
        const WireImage& destination, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
        GLint dx0, GLint dy0, GLint dx1, GLint dy1) {
        const Int64 spanX = static_cast<Int64>(sx1) - sx0;
        const Int64 spanY = static_cast<Int64>(sy1) - sy0;
        const Int64 dstSpanX = static_cast<Int64>(dx1) - dx0;
        const Int64 dstSpanY = static_cast<Int64>(dy1) - dy0;
        const Int64 width = spanX < 0 ? -spanX : spanX;
        const Int64 height = spanY < 0 ? -spanY : spanY;
        const Int64 dstWidth = dstSpanX < 0 ? -dstSpanX : dstSpanX;
        const Int64 dstHeight = dstSpanY < 0 ? -dstSpanY : dstSpanY;
        // A SCALE has no arm here and is not supposed to: 18.3.1 makes a multisample blit whose
        // rectangles differ in size INVALID_OPERATION. Rule I (a), the same three lines the
        // colour arm's multisample-destination decline uses. A ZERO span on one side only is a
        // size mismatch too (review round 2): it used to return silently with the other side
        // non-empty, where GL raises the error. Only when BOTH sides are empty on an axis is
        // there nothing to do and nothing to say - that is the caller's early return.
        if (width != dstWidth || height != dstHeight) {
            MGLOG_E_ONCE("DirectVulkan::ResolveWireDepthStencil: a %lldx%lld source rectangle "
                         "cannot reach a %lldx%lld destination rectangle - a multisample resolve "
                         "carries no scale; declining the blit",
                         static_cast<long long>(width), static_cast<long long>(height),
                         static_cast<long long>(dstWidth), static_cast<long long>(dstHeight));
            RecordTextureCopyError("ResolveWireDepthStencil", ErrorCode::InvalidOperation,
                                   "glBlitFramebuffer from a multisampled read framebuffer requires "
                                   "identical source and destination rectangle dimensions.");
            return true;
        }
        if (!width || !height) return false;
        // WHICH MIRRORS HAVE AN ARM. The copy-out in ResolveWireDepthStencil moves the resolved
        // aspect through a buffer, and a buffer copy addresses ROWS: one VkBufferImageCopy per
        // row, walked backwards, is a vertical mirror for free. A HORIZONTAL one would need a
        // region per TEXEL, which is not a copy any more - and re-encoding across formats
        // (BlitDepthAcrossFormats, shared with the monolith arm) has no flip either. Both
        // decline by name, each under its OWN once-site (review round 2: one site covered both,
        // so after the first X decline a cross-format Y decline logged nothing); §6 in
        // notes/p7/magma-b2.md carries them. These are declines of a LEGAL blit - GL would
        // raise no error here - so the INVALID_OPERATION is rule I (a)'s named refusal, not
        // GL's answer.
        const Bool mirrorX = (spanX < 0) != (dstSpanX < 0);
        const Bool mirrorY = (spanY < 0) != (dstSpanY < 0);
        if (mirrorX) {
            MGLOG_E_ONCE("DirectVulkan::ResolveWireDepthStencil: a multisample resolve mirrored on "
                         "X (source format %d, destination format %d) has no arm - the copy-out "
                         "reverses whole rows and nothing smaller; declining the blit",
                         static_cast<Int>(source.format), static_cast<Int>(destination.format));
            RecordTextureCopyError("ResolveWireDepthStencil", ErrorCode::InvalidOperation,
                                   "This device cannot mirror that multisample depth/stencil blit.");
            return true;
        }
        if (mirrorY && source.format != destination.format) {
            MGLOG_E_ONCE("DirectVulkan::ResolveWireDepthStencil: a multisample resolve mirrored on "
                         "Y across two formats (source format %d, destination format %d) has no "
                         "arm - the cross-format re-encode has no flip; declining the blit",
                         static_cast<Int>(source.format), static_cast<Int>(destination.format));
            RecordTextureCopyError("ResolveWireDepthStencil", ErrorCode::InvalidOperation,
                                   "This device cannot mirror that multisample depth/stencil blit.");
            return true;
        }
        return false;
    }

    void VulkanRenderer::ResolveWireDepthStencil(WireImage source, WireImage destination,
        GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1) {
        // P7 wave 2-B2 (review round): A REVERSED RECTANGLE IS NOT A MALFORMED ONE. This guard
        // used to require `sx1 > sx0` and `sy1 > sy0` and take the session with a named Fatal
        // otherwise - so `glBlitFramebuffer(0, h, w, 0, 0, 0, w, h, COLOR|DEPTH)` out of a
        // multisample framebuffer blitted its COLOUR correctly (the colour arm has handled a
        // flip since P5f and now handles a scale too) and then died on its DEPTH. GL 4.6 core
        // 18.3.1 asks for identical DIMENSIONS, not identical corners - Mesa compares absolute
        // spans - so a flip is a legal blit that has to produce a mirrored picture.
        //
        // Spans are therefore signed and everything downstream works off the min corner.
        //
        // THE SHAPE DECLINES ARE DECIDED ONCE PER CALL (review round 2). BlitWireFramebuffers
        // asks DeclineWireDepthStencilResolveShape before ANY aspect runs, because the colour
        // lambda runs first: asked only here, a scaled COLOR|DEPTH blit scaled and wrote its
        // colour and THEN recorded INVALID_OPERATION on its depth - a partial effect GL 18.3.1
        // forbids (the whole call errors) - and an X-mirrored one mirrored its colour and left
        // its depth alone. The call below is the same question for a caller that did not ask;
        // on the BlitWireFramebuffers path it has already answered false.
        if (DeclineWireDepthStencilResolveShape(source, destination, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1))
            return;
        const Int64 spanX = static_cast<Int64>(sx1) - sx0;
        const Int64 spanY = static_cast<Int64>(sy1) - sy0;
        if (!spanX || !spanY) return; // both sides empty on an axis (the helper errored otherwise)
        const Int64 width = spanX < 0 ? -spanX : spanX;
        const Int64 height = spanY < 0 ? -spanY : spanY;
        // Past the helper the only mirror left is a same-format vertical one.
        const Bool mirrorY = (spanY < 0) != (dy1 < dy0);
        const GLint sxMin = std::min(sx0, sx1), syMin = std::min(sy0, sy1);
        const GLint dxMin = std::min(dx0, dx1), dyMin = std::min(dy0, dy1);
        if (source.isDefault || destination.isDefault || source.is3D || destination.is3D ||
            sxMin < 0 || syMin < 0 || dxMin < 0 || dyMin < 0 ||
            static_cast<Uint64>(sxMin + width) > source.extent.width ||
            static_cast<Uint64>(syMin + height) > source.extent.height ||
            static_cast<Uint64>(dxMin + width) > destination.extent.width ||
            static_cast<Uint64>(dyMin + height) > destination.extent.height)
            MagmaWireFatal("multisample-depth-resolve-region");
        VkImageAspectFlags formatAspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (source.format == VK_FORMAT_S8_UINT) formatAspects = VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (source.format == VK_FORMAT_D16_UNORM_S8_UINT || source.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                 source.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
            formatAspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        // P7 wave 2-B2, CONTRACT-P7 §3.2 (`multisample-blit-aspect`): TWO ARMS, and which one
        // runs is a fact about the DEVICE, not about the shape. VK_KHR_depth_stencil_resolve is
        // the direct route and is taken wherever it exists; where it does not - which is where
        // the session used to end at `Magma:multisample-depth-resolve-capability`, on the first
        // glBlitFramebuffer(GL_DEPTH_BUFFER_BIT) out of a multisampled read framebuffer - a
        // baked pass fetches sample ZERO and writes gl_FragDepth (or, with
        // VK_EXT_shader_stencil_export, gl_FragStencilRefARB). Sample zero in both arms, because
        // VK_RESOLVE_MODE_SAMPLE_ZERO_BIT is what the subpass below asks for.
        //
        // THE TWO CAPABILITY TESTS ARE NOT THE SAME TEST, and the difference is structural: the
        // subpass resolves every aspect the FORMAT has, so it needs a mode for each of them,
        // while the shader pass writes only the one aspect this blit asked for and leaves the
        // rest of the scratch undefined - which is safe because the copy below reads exactly
        // that aspect.
        const Bool extensionResolve = !MagmaWireForcedShaderDepthResolve() && m_wireCreateRenderPass2 &&
            (!(formatAspects & VK_IMAGE_ASPECT_DEPTH_BIT) ||
             (m_wireDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) &&
            (!(formatAspects & VK_IMAGE_ASPECT_STENCIL_BIT) ||
             (m_wireStencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT));

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

        VkImageView views[2]{};
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (extensionResolve) {
        TransitionWireImage(source, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        TransitionWireImage(resolved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

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
        VK_VERIFY(m_wireCreateRenderPass2(m_device, &passInfo, nullptr, &pass));
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments = views;
        framebufferInfo.width = source.extent.width;
        framebufferInfo.height = source.extent.height;
        framebufferInfo.layers = 1;
        VK_VERIFY(vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &framebuffer));
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = {{sxMin, syMin}, {static_cast<Uint32>(width), static_cast<Uint32>(height)}};
        vkCmdBeginRenderPass(m_frameContext.GetCurrent().commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(m_frameContext.GetCurrent().commandBuffer);
        } else if (!ResolveWireDepthStencilWithShader(source, resolved, source.aspect, sxMin, syMin,
                                                      static_cast<Uint32>(width), static_cast<Uint32>(height))) {
            // NEITHER ARM, and rule I (a) says what that is: this device has no
            // VK_KHR_depth_stencil_resolve and the pass cannot stand in for it either - the
            // commonest reason being a stencil-only resolve without VK_EXT_shader_stencil_export,
            // where a fragment shader cannot put a value in the stencil aspect at all. A decline
            // leaves the destination as the application last wrote it, which is what
            // glBlitFramebuffer would do if the driver refused the resolve.
            MGLOG_E_ONCE("DirectVulkan::ResolveWireDepthStencil: resolving aspect 0x%x of a %u-sample "
                         "format %d needs either VK_KHR_depth_stencil_resolve (renderpass2=%d, "
                         "depth modes 0x%x, stencil modes 0x%x) or a shader pass "
                         "(stencil export=%d, source usage 0x%x); declining the blit",
                         static_cast<Uint32>(source.aspect), static_cast<Uint32>(source.samples),
                         static_cast<Int>(source.format), m_wireCreateRenderPass2 ? 1 : 0,
                         static_cast<Uint32>(m_wireDepthResolveModes),
                         static_cast<Uint32>(m_wireStencilResolveModes),
                         m_wireShaderStencilExport ? 1 : 0, static_cast<Uint32>(source.usageFlags));
            RecordTextureCopyError("ResolveWireDepthStencil", ErrorCode::InvalidOperation,
                                   "This device cannot resolve that multisample depth/stencil aspect.");
            WireRetiredObjects declined{};
            declined.textures.push_back(Move(scratch));
            RetireWireObjects(Move(declined));
            return;
        }
        auto& frame = m_frameContext.GetCurrent();
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
            // THE ROW STRIDE IS PADDED TO 4 BYTES. A depth/stencil buffer<->image copy wants
            // every bufferOffset on a multiple of 4 (VUID-vkCmdCopyBufferToImage-dstImage-07978,
            // pRegions-07978 in older layers), and the mirror below starts a region on every
            // row - so a tight row of 1-byte stencil texels whose width is not a multiple of 4,
            // or of 2-byte D16 texels with an odd width, put every row after the first on an
            // illegal offset. The scissor clip in BlitWireFramebuffers produces exactly such
            // widths. Both copies name the padded length as bufferRowLength, so the buffer
            // layout is one layout whichever way it is walked.
            const VkDeviceSize texelsPer4Bytes = 4 / texelSize;
            const VkDeviceSize paddedWidth =
                (static_cast<VkDeviceSize>(width) + texelsPer4Bytes - 1) / texelsPer4Bytes * texelsPer4Bytes;
            const VkDeviceSize bytes = paddedWidth * height * texelSize;
            aspectBuffer = MakeUnique<VkBufferObject>();
            if (!aspectBuffer->Create({.allocator = m_allocator, .size = bytes,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .memoryUsage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE}))
                MagmaWireFatal("multisample-depth-resolve-buffer");
            VkBufferImageCopy copy{};
            copy.bufferRowLength = static_cast<Uint32>(paddedWidth);
            copy.imageSubresource = {source.aspect, 0, 0, 1};
            copy.imageOffset = {sxMin, syMin, 0};
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
            copy.imageOffset = {dxMin, dyMin, 0};
            if (!mirrorY) {
                vkCmdCopyBufferToImage(frame.commandBuffer, aspectBuffer->GetHandle(), destination.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else {
                // THE VERTICAL MIRROR, and it costs one region per row rather than a pass. The
                // buffer holds the resolved band bottom row first (the image copy above wrote it
                // from syMin upwards, one padded stride per row); writing row r of that buffer
                // to destination row dyMin + height - 1 - r reverses the band. Each region
                // starts on a padded row, which is what keeps its bufferOffset a multiple of 4.
                Vector<VkBufferImageCopy> rows(static_cast<SizeT>(height), copy);
                const VkDeviceSize rowBytes = paddedWidth * texelSize;
                for (Int64 row = 0; row < height; ++row) {
                    auto& region = rows[static_cast<SizeT>(row)];
                    region.bufferOffset = static_cast<VkDeviceSize>(row) * rowBytes;
                    region.imageOffset = {dxMin, static_cast<Int32>(dyMin + height - 1 - row), 0};
                    region.imageExtent = {static_cast<Uint32>(width), 1, 1};
                }
                vkCmdCopyBufferToImage(frame.commandBuffer, aspectBuffer->GetHandle(), destination.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<Uint32>(rows.size()), rows.data());
            }
        } else {
            // A logical GL depth format may use different native renderbuffer
            // and texture formats. Re-encode only after producing real resolved
            // samples. Keep all scratch objects local across this helper's flush.
            if (!BlitDepthAcrossFormats(frame, resolved.image, resolved.format, &scratch->layout, 0, 0,
                destination.image, destination.format, destination.trackedLayout, destination.level, destination.layer,
                sxMin, syMin, dxMin, dyMin, static_cast<GLint>(width), static_cast<GLint>(height),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                source.aspect == VK_IMAGE_ASPECT_STENCIL_BIT))
                MagmaWireFatal("multisample-depth-resolve-conversion");
        }
        WireRetiredObjects retired{};
        // The shader arm owns and retires its own pass, framebuffer and views, so these four
        // are null on that path - pushed only when the extension arm made them.
        if (framebuffer != VK_NULL_HANDLE) retired.framebuffers.push_back(framebuffer);
        if (pass != VK_NULL_HANDLE) retired.renderPasses.push_back(pass);
        if (views[0] != VK_NULL_HANDLE) retired.imageViews = {views[0], views[1]};
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
        // P7 wave 2-B2 (review round 2): ONE ANSWER FOR THE WHOLE CALL. The aspects below run
        // colour first, so a multisample depth/stencil shape that declines (a scale, an X
        // mirror, a Y mirror across two formats) used to be discovered only after the colour
        // attachment had already been scaled or mirrored and written - the call then recorded
        // INVALID_OPERATION with half its effect in place, which GL 18.3.1 does not allow (an
        // erroring blit writes nothing). The shape is a fact about the rectangles and the two
        // images, so it is asked here, before any aspect runs, for every depth/stencil aspect
        // the mask names that would take the multisample resolve arm.
        //
        // A default READ framebuffer is never multisampled here, so it is left to the lambda. A
        // default DRAW framebuffer is asked like any other (review round 3): round 2 left it out
        // on the belief that a default draw behind a multisample read was ResolveWireDepthStencil's
        // region Fatal whatever the shape, but that arm asks DeclineWireDepthStencilResolveShape
        // BEFORE its region check - so a scaled (or X-mirrored) COLOR|DEPTH blit out of a
        // multisample framebuffer onto the window scaled and wrote its colour through
        // BlitWireColorToDefault and then recorded INVALID_OPERATION on its depth, the very
        // partial effect this block exists to close. Both sides are resolved as READS
        // (isWriteTarget=false): only their format and samples are consulted here, and a write
        // resolve of the default framebuffer would re-point it at the acquired swapchain image
        // before anything had been written to it.
        if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && !read->IsDefault) {
            const auto declines = [&](const MG_Pipe::MGPSurface& src, const MG_Pipe::MGPSurface& dst,
                                      VkImageAspectFlags aspect) -> Bool {
                const WireImage source = ResolveWireImage(*read, src, aspect, /*isWriteTarget=*/false);
                if (!source.image || source.samples == VK_SAMPLE_COUNT_1_BIT) return false;
                const WireImage destination = ResolveWireImage(*draw, dst, aspect, /*isWriteTarget=*/false);
                if (!destination.image || destination.samples != VK_SAMPLE_COUNT_1_BIT) return false;
                return DeclineWireDepthStencilResolveShape(source, destination, sx0, sy0, sx1, sy1,
                                                           dx0, dy0, dx1, dy1);
            };
            if (((mask & GL_DEPTH_BUFFER_BIT) && declines(read->Depth, draw->Depth, VK_IMAGE_ASPECT_DEPTH_BIT)) ||
                ((mask & GL_STENCIL_BUFFER_BIT) && declines(read->Stencil, draw->Stencil, VK_IMAGE_ASPECT_STENCIL_BIT)))
                return;
        }
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
                //
                // P7 wave 2-B2, CONTRACT-P7 §3.2 (`multisample-blit-shape`): AND IT CANNOT SCALE.
                // vkCmdResolveImage carries ONE extent and one offset per side, so this arm used
                // to refuse every rectangle whose destination was a different size from its
                // source - a shape glBlitFramebuffer names, and one the monolith arm reaches by
                // this very route (VulkanRenderer.cpp's `regionWasTransformed` branch resolves
                // into a scratch and blits THAT). The two legs split the work the same way here:
                // the resolve reads the source band AT RAW OFFSETS into a scratch sized by the
                // SOURCE rect, and only the second leg - an ordinary vkCmdBlitImage on a
                // single-sampled image, which is the one thing it is for - sees the scale and the
                // flip. So the size equality drops out of this guard; what stays is the part
                // neither leg can express (a rotated default source, a 3D source) or that names
                // texels the source does not have.
                const Int64 width = std::abs(static_cast<Int64>(sx1) - sx0);
                const Int64 height = std::abs(static_cast<Int64>(sy1) - sy0);
                if (!width || !height || dx0 == dx1 || dy0 == dy1) return;
                if (source.isDefault || source.is3D ||
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
                    // The filter only started MATTERING with the scale above: at 1:1 LINEAR and
                    // NEAREST read the same texel, so nothing here ever asked whether the format
                    // could be filtered. VK_FILTER_LINEAR on a source whose optimalTilingFeatures
                    // lack SAMPLED_IMAGE_FILTER_LINEAR is undefined behaviour, and GL says filter
                    // is ignored for a multisample resolve in the first place - so a format that
                    // cannot be filtered takes NEAREST rather than the command being illegal.
                    VkFormatProperties scratchProperties{};
                    vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle, source.format, &scratchProperties);
                    const VkFilter blitFilter =
                        filter == GL_LINEAR && (scratchProperties.optimalTilingFeatures &
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                            ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                    vkCmdBlitImage(m_frameContext.GetCurrent().commandBuffer, resolved.image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &copy, blitFilter);
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
                // P7 wave 2-B, CONTRACT-P7 §3.2: `default-color-blit-shape@P7` RETIRES, in three
                // pieces, because the one Fatal covered three unrelated situations.
                //
                // The shader blit is here for ROTATION and for nothing else. It can only SAMPLE
                // its source - sampler2D, a float result, one of two 2D view types - so an
                // RGBA8UI attachment, a 3D slice or an array layer made it return false, and the
                // refusal fired even on a surface with NO rotation, where the ordinary
                // vkCmdBlitImage arm twenty lines down would have done the blit correctly and
                // does handle those shapes.
                const auto transform = m_swapchainObject.GetPreTransform();
                const Bool knownRotation = transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
                                           transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                           transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ||
                                           transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
                if (!knownRotation) {
                    // Mirrors, and mirror-rotations, are the shapes neither arm can express: the
                    // blit rectangle would have to be reflected, and this package does not have a
                    // device that reports one to check the result against. A decline (§3.2's
                    // "outside the four rotations"), not a Fatal, and it says which transform.
                    MGLOG_E_ONCE("DirectVulkan::BlitWireFramebuffers: the surface pre-transform is 0x%x, "
                                 "which is neither identity nor one of the three rotations either blit "
                                 "arm can express; declining the blit to the default framebuffer",
                                 static_cast<Uint32>(transform));
                    return;
                }
                if (!MagmaWireForcedDefaultBlitScratch() &&
                    BlitWireColorToDefault(source, destination, sx0, sy0, sx1, sy1,
                                           dx0, dy0, dx1, dy1, filter)) {
                    m_swapchainObject.SetImageContentDefined(destination.swapchainImageIndex, true);
                    return;
                }
                // A source the shader pass cannot sample, on a rotated surface. Move the region
                // into an owned 2D image of a sampled-float format in the same size-compatibility
                // class and blit THAT. The knob forces this arm on an identity surface, where it
                // must produce the same pixels the fall-through below produces - which is what
                // makes it reachable from a host lane at all (every host surface here is
                // identity, and the Redmi evidence is a pbuffer, so nothing else would run it).
                if (transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
                    MagmaWireForcedDefaultBlitScratch()) {
                    if (BlitWireColorToDefaultThroughScratch(source, destination, sx0, sy0, sx1, sy1,
                                                             dx0, dy0, dx1, dy1, filter)) {
                        m_swapchainObject.SetImageContentDefined(destination.swapchainImageIndex, true);
                        return;
                    }
                    MGLOG_E_ONCE("DirectVulkan::BlitWireFramebuffers: a %ux%u source of format %d "
                                 "(view type %d) cannot reach the rotated (0x%x) default framebuffer "
                                 "through either the shader pass or a scratch copy; declining the blit",
                                 source.extent.width, source.extent.height,
                                 static_cast<Int>(source.format), static_cast<Int>(source.viewType),
                                 static_cast<Uint32>(transform));
                    return;
                }
                // Identity: fall through to the ordinary vkCmdBlitImage arm below. It already
                // maps GL's bottom row onto the native image's last row for a default target,
                // and vkCmdBlitImage takes integer formats and array layers natively. Its 3D
                // addressing was wrong and is fixed with this package - see the srcBaseLayer /
                // srcZ note down there; the bug was invisible until now because a 3D source hit
                // the Fatal above and never reached that arm.
            }
            const Bool sameImage = source.image == destination.image;
            const VkImageLayout srcLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const VkImageLayout dstLayout = sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            TransitionWireImage(source,srcLayout);
            if (sameImage) destination.layout = source.layout;
            else TransitionWireImage(destination,dstLayout);
            VkImageBlit region{};
            // A 3D IMAGE CARRIES ITS SLICE ON Z, NOT ON THE LAYER AXIS. Its only subresource is
            // layer 0, and the selected slice is the z of the offsets - which is how the copy
            // endpoint (`CopyImageSliceMapping`) and the multisample resolve arm above already
            // address one. This arm addressed it as `baseArrayLayer = source.layer` with z 0..1,
            // so a blit from slice 1 of a 3D texture read slice 0 out of a layer the image does
            // not have. Nothing caught it, because until this package that shape reached the
            // arm's Fatal first and never got here at all.
            const Uint32 srcBaseLayer = source.is3D ? 0u : source.layer;
            const Uint32 dstBaseLayer = destination.is3D ? 0u : destination.layer;
            const Int32 srcZ = source.is3D ? static_cast<Int32>(source.layer) : 0;
            const Int32 dstZ = destination.is3D ? static_cast<Int32>(destination.layer) : 0;
            region.srcSubresource = {aspect,source.level,srcBaseLayer,1};
            region.dstSubresource = {aspect,destination.level,dstBaseLayer,1};
            region.srcOffsets[0] = {sx0,sy0,srcZ}; region.srcOffsets[1] = {sx1,sy1,srcZ+1};
            region.dstOffsets[0] = {dx0,dy0,dstZ}; region.dstOffsets[1] = {dx1,dy1,dstZ+1};
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
                // P7 wave 2-B2, CONTRACT-P7 §3.2: WHAT IS LEFT HERE IS A MULTISAMPLE DESTINATION.
                // Every blit that resolves a multisample SOURCE into a single-sample destination
                // returned above - the depth/stencil arm at the top of this lambda through
                // ResolveWireDepthStencil, the colour arm below it through its resolve scratch -
                // and both of those now carry their own scale and flip. So the samples of the two
                // sides can only differ here by the DESTINATION being multisampled, which is the
                // shape this arm declines rather than the one it performs.
                const Int32 width = region.srcOffsets[1].x - region.srcOffsets[0].x;
                const Int32 height = region.srcOffsets[1].y - region.srcOffsets[0].y;
                if (width <= 0 || height <= 0 || width != region.dstOffsets[1].x - region.dstOffsets[0].x ||
                    height != region.dstOffsets[1].y - region.dstOffsets[0].y || source.format != destination.format) {
                    // `multisample-blit-shape@P7` RETIRES: the half that had a shape neither
                    // vkCmdResolveImage nor vkCmdBlitImage could express is done above, and this
                    // is the half that has none - vkCmdCopyImage is the only command that writes
                    // a multisampled image, it takes one extent, and GL 4.6 core 18.3.1 makes a
                    // multisample blit whose rectangles differ in size (or whose formats differ)
                    // INVALID_OPERATION rather than a picture. Rule I (a): say it once, record
                    // the error GL names, leave the destination alone, keep the session.
                    MGLOG_E_ONCE("DirectVulkan::BlitWireFramebuffers: a %dx%d source rectangle "
                                 "(format %d, %u sample(s)) cannot reach a %dx%d destination "
                                 "rectangle (format %d, %u sample(s)) - a blit onto a multisample "
                                 "attachment carries no scale, no flip and no format conversion; "
                                 "declining the blit",
                                 width, height, static_cast<Int>(source.format),
                                 static_cast<Uint32>(source.samples),
                                 region.dstOffsets[1].x - region.dstOffsets[0].x,
                                 region.dstOffsets[1].y - region.dstOffsets[0].y,
                                 static_cast<Int>(destination.format),
                                 static_cast<Uint32>(destination.samples));
                    RecordTextureCopyError("BlitWireFramebuffers", ErrorCode::InvalidOperation,
                                           "glBlitFramebuffer onto a multisampled draw framebuffer requires "
                                           "identical source and destination rectangles and formats.");
                    return;
                }
                if (source.samples == destination.samples) {
                    VkImageCopy copy{region.srcSubresource, region.srcOffsets[0], region.dstSubresource,
                        region.dstOffsets[0], {static_cast<Uint32>(width), static_cast<Uint32>(height), 1}};
                    vkCmdCopyImage(command, source.image, srcLayout, destination.image, dstLayout, 1, &copy);
                } else {
                    // `multisample-blit-aspect@P7` RETIRES. The half with an arm is
                    // ResolveWireDepthStencil's: a multisample depth or stencil SOURCE now
                    // reaches a single-sample destination on every device, through
                    // VK_KHR_depth_stencil_resolve where it exists and through the baked pass in
                    // WireMultisampleResolve.inc where it does not. This is the half with none -
                    // the sample counts differ and the DESTINATION is the multisampled side, so
                    // it is either one sample fanned out to N or N reduced to M.
                    //
                    // Vulkan has no command for either: vkCmdResolveImage only ever writes a
                    // single-sample destination, vkCmdBlitImage refuses both sides, and
                    // vkCmdCopyImage requires the counts to match. Reaching it through a draw
                    // would need a pipeline with the destination's sample count and a
                    // multisample source view, i.e. a second copy of the resolve machinery
                    // pointed the other way, for a shape GL 4.6 core 18.3.1 calls
                    // INVALID_OPERATION whenever the two rectangles are not identical and
                    // describes as sample replication when they are. Rule I (a): name both
                    // counts, record the error, leave the destination alone.
                    MGLOG_E_ONCE("DirectVulkan::BlitWireFramebuffers: aspect 0x%x cannot go from a "
                                 "%u-sample source to a %u-sample destination - a resolve only ever "
                                 "writes one sample and a copy needs the counts to match; declining "
                                 "the blit",
                                 static_cast<Uint32>(aspect), static_cast<Uint32>(source.samples),
                                 static_cast<Uint32>(destination.samples));
                    RecordTextureCopyError("BlitWireFramebuffers", ErrorCode::InvalidOperation,
                                           "glBlitFramebuffer cannot change the sample count of an "
                                           "attachment in that direction.");
                    return;
                }
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
        // P7 wave 2-B, CONTRACT-P7 §3.2: `depth-stencil-mipmap@P7` RETIRES FOR DEPTH AND
        // DECLINES FOR DEPTH|STENCIL - and the decline comes BEFORE the chain is grown, so a
        // level this verb is not going to fill is not created and then recorded as defined.
        //
        // The combined case is a decline rather than a Fatal because the monolith arm does no
        // better: its shader fallback asserts depth-only (VulkanRenderer.cpp's GenerateMipmap,
        // "shader fallback only supports depth-only textures") and an assert is nothing in the
        // release build both arms ship as. Nothing in GL promises a stencil mip either - the
        // pass writes gl_FragDepth and would leave the stencil aspect of the level it produced
        // undefined, which is worse than leaving the whole level alone and saying so.
        if (resource->aspect != VK_IMAGE_ASPECT_COLOR_BIT && resource->aspect != VK_IMAGE_ASPECT_DEPTH_BIT) {
            MGLOG_E_ONCE("DirectVulkan::GenerateWireMipmap: aspect 0x%x (a combined depth/stencil or "
                         "stencil-only texture, format %d) has no mip generation on either arm; "
                         "declining and leaving levels %u..%u as they are",
                         static_cast<Uint32>(resource->aspect), static_cast<Int>(resource->format),
                         base + 1, base + count - 1);
            return;
        }
        if (!m_textureManager->GrowWireTextureMipChain(handle,base+count,frame.commandBuffer))
            MagmaWireFatal("mipmap-growth");
        VkFormatProperties props{};
        const VkFormat format = viewFormat == VK_FORMAT_UNDEFINED ? resource->format : viewFormat;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice.handle,format,&props);
        const auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        const Uint32 forcedShaderTier = MagmaWireForcedShaderMipmapTier();
        const Bool nativeBlit = forcedShaderTier == 0 && format == resource->format &&
                                (props.optimalTilingFeatures & required) == required;
        const VkFilter filter = props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ?
                                VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (!nativeBlit && resource->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
            // P7 wave 2-B, CONTRACT-P7 §5.1 (A): THE DEPTH ARM, and it is first because a
            // depth format without BLIT_DST is the ordinary case, not the exotic one -
            // VK_FORMAT_FEATURE_BLIT_DST is optional for depth and plenty of devices decline
            // it. Before this package the whole branch below was colour-only, so such a
            // texture reached `mipmap-shader-format-or-shape` on its very first disjunction
            // while the monolith arm quietly generated the chain with a shader.
            //
            // The per-level pass is WireDepthMipmap.inc's, which is the monolith's fragment
            // shader with the monolith's arithmetic on a baked pipeline that owns no frontend
            // object - §3.2's "observably the monolith's" without the GL program the monolith
            // needs to get there.
            if (baseLayer >= resource->arrayLayers || layerCount > resource->arrayLayers - baseLayer)
                MagmaWireFatal(MagmaWireShapeDetail("mipmap-shader-format-or-shape", format,
                    resource->format, props, resource->aspect, resource->viewType,
                    resource->usageFlags, resource->extent, resource->depth, resource->mipLevels,
                    baseLayer, layerCount, resource->arrayLayers).c_str());
            for (Uint32 level = base + 1; level < base + count; ++level) {
                for (Uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
                    WireImage source{};
                    source.image = resource->image;
                    source.format = format;
                    source.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
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
                    // The pass reports the SHAPE it cannot take (1D, 3D, multisample, a
                    // format without sampled or depth-attachment support), so the refusal
                    // keeps its name and gains the reason.
                    if (!GenerateWireDepthMipLevel(source, destination))
                        MagmaWireFatal(MagmaWireShapeDetail("mipmap-shader-format-or-shape", format,
                            resource->format, props, resource->aspect, resource->viewType,
                            resource->usageFlags, resource->extent, resource->depth,
                            resource->mipLevels, baseLayer, layerCount,
                            resource->arrayLayers).c_str());
                }
            }
        } else if (!nativeBlit) {
            // Packed floating-point color formats may support sampling and
            // rendering without BLIT_DST. Generate from actual GPU mip contents
            // through the native shader pass; no client or staged texels enter.
            //
            // TWO SHADER SHAPES, not one. The arm that was here rendered STRAIGHT INTO the
            // next level, which needs the image to have been created with
            // COLOR_ATTACHMENT usage - and a texture whose format the device refuses as a
            // colour attachment never gets that bit (VkTextureManager.cpp:2156-2160), so
            // every sampled-only texture without BLIT support fell into the refusal rather
            // than into a fallback. When the FORMAT can be rendered and only the IMAGE
            // cannot, an owned scratch attachment of the destination level's size renders
            // the mip and vkCmdCopyImage moves it in: a raw texel move, no conversion, and
            // the texels still come from GPU contents rather than from a staged shadow.
            const Bool colour = resource->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
            const Bool planar = resource->viewType != VK_IMAGE_VIEW_TYPE_1D &&
                                resource->viewType != VK_IMAGE_VIEW_TYPE_1D_ARRAY &&
                                resource->viewType != VK_IMAGE_VIEW_TYPE_3D;
            const Bool layersInRange = baseLayer < resource->arrayLayers &&
                                       layerCount <= resource->arrayLayers - baseLayer;
            // The shader arm samples through a sampler2D and writes a vec4, which is what
            // BlitWireColorImage's own float check enforces; naming it here rather than
            // letting the blit return false is what puts the reason in the fatal line.
            const Bool sampledFloat = vkuFormatIsSampledFloat(format);
            const Bool renderableFormat =
                (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
            const Bool attachable = forcedShaderTier < 2 &&
                (resource->usageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
            const Bool viaScratch = !attachable && renderableFormat &&
                (resource->usageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
            if (!colour || !planar || !layersInRange || !sampledFloat || (!attachable && !viaScratch))
                MagmaWireFatal(MagmaWireShapeDetail("mipmap-shader-format-or-shape", format,
                    resource->format, props, resource->aspect, resource->viewType,
                    resource->usageFlags, resource->extent, resource->depth, resource->mipLevels,
                    baseLayer, layerCount, resource->arrayLayers).c_str());
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
                    const GLenum blitFilter = filter == VK_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
                    if (attachable) {
                        if (!BlitWireColorImage(source, destination, 0, 0, source.extent.width,
                            source.extent.height, 0, 0, destination.extent.width,
                            destination.extent.height, blitFilter, true))
                            MagmaWireFatal("mipmap-shader-blit");
                        continue;
                    }
                    // Owned, per level, and retired with the submit that consumes it - the
                    // shape the multisample resolve above already uses. A shared scratch
                    // resized here would destroy an image an earlier queued pass still reads.
                    auto scratch = MakeUnique<VkTextureManager::TextureResource>();
                    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                    info.imageType = VK_IMAGE_TYPE_2D;
                    info.format = format;
                    info.extent = {destination.extent.width, destination.extent.height, 1};
                    info.mipLevels = info.arrayLayers = 1;
                    info.samples = VK_SAMPLE_COUNT_1_BIT;
                    info.tiling = VK_IMAGE_TILING_OPTIMAL;
                    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT;
                    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    VmaAllocationCreateInfo allocation{};
                    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                    allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                    if (vmaCreateImage(m_allocator, &info, &allocation, &scratch->image,
                                       &scratch->allocation, nullptr) != VK_SUCCESS)
                        MagmaWireFatal("mipmap-shader-scratch-allocation");
                    scratch->format = format;
                    scratch->extent = destination.extent;
                    scratch->mipLevels = scratch->arrayLayers = scratch->depth = 1;
                    scratch->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    WireImage target{};
                    target.image = scratch->image;
                    target.format = format;
                    target.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                    target.extent = destination.extent;
                    target.trackedLayout = &scratch->layout;
                    if (!BlitWireColorImage(source, target, 0, 0, source.extent.width,
                        source.extent.height, 0, 0, target.extent.width, target.extent.height,
                        blitFilter, true))
                        MagmaWireFatal("mipmap-shader-blit");
                    target.layout = scratch->layout;
                    TransitionWireImage(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    WireImage writeBack = destination;
                    writeBack.layout = resource->layout;
                    TransitionWireImage(writeBack, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    VkImageCopy copy{};
                    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
                    copy.extent = {destination.extent.width, destination.extent.height, 1};
                    vkCmdCopyImage(m_frameContext.GetCurrent().commandBuffer, scratch->image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resource->image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    WireRetiredObjects retired{};
                    retired.textures.push_back(Move(scratch));
                    RetireWireObjects(Move(retired));
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
                // PH-4: the record's own target and format, not NoteLevelDefined's Tex2D/RGBA8
                // defaults - a 2D-array or 3D level has depth > 1, which the Tex2D device-limit rule
                // refuses by name (TextureViewScenario.GenerateMipmapThroughAView on all Magma arms).
                store.NoteLevelDefined(key,static_cast<Uint16>(upload),static_cast<Uint16>(level),extent,
                                       record.Desc.Target,record.Desc.InternalFormat);
                store.MarkLevelGpuDirty(key,static_cast<Uint16>(upload),static_cast<Uint16>(level),true);
            }
        }
    }
#endif
