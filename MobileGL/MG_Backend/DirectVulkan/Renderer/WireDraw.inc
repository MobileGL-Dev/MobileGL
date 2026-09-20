// Server-owned draw/dispatch setup. The legacy setup remains the monolith implementation.
#if MOBILEGL_BUILD_DISAGGREGATED
    static MagmaProgramSource WireProgramSource(MG_Pipe::MGPipeHandle handle) {
        auto& state = MG_Pipe::MGPipeApplier();
        const Bool composite = handle.Slot >= MG_Pipe::kMGPipeShaderCsoCompositeSlotBase;
        const auto& records = composite ? state.CompositeShaderCsos : state.ShaderCsos;
        const SizeT index = composite ? handle.Slot - MG_Pipe::kMGPipeShaderCsoCompositeSlotBase : handle.Slot;
        if (MG_Pipe::MGPipeHandleIsNull(handle) || index >= records.size() ||
            !records[index].Live || records[index].Gen != handle.Gen || !records[index].Archive)
            MagmaWireFatal("program-record");
        return MagmaProgramSource(handle, records[index]);
    }

    [[noreturn]] static void WireBufferLegacyFatal() {
        MGLOG_F("MGPipe: Fatal{RoleViolation, \"buffer-legacy-arm\"} (Magma P7 buffer consumer)");
        std::abort();
    }

    static const MG_Pipe::MGPStreamOutputBegin& WireXfbSpan() {
        const auto& state = MG_Pipe::MGPipeApplier();
        const auto found = state.StreamOutputSpans.find(state.BoundStreamOutputLifetimeId);
        if (found == state.StreamOutputSpans.end()) MagmaWireFatal("xfb-span-record");
        return found->second;
    }

    Bool VulkanRenderer::BeginWireXfbCaptureForDraw(FrameContext::FrameData& frame) {
        const auto& span = WireXfbSpan();
        const auto program = WireProgramSource(span.CaptureProgram);
        const Uint32 count = static_cast<Uint32>(std::min<SizeT>(program.GetTransformFeedbackBufferCount(), 4));
        if (!count || !program.GetTransformFeedbackVaryingCount()) return false;
        VkBuffer buffers[4]{};
        VkDeviceSize offsets[4]{};
        VkDeviceSize sizes[4]{};
        Bool hasTarget = false;
        for (Uint32 i = 0; i < count; ++i) {
            // A leading gl_NextBuffer leaves a hole in the binding indices. No
            // output names that zero-stride target, and GL need not bind a buffer.
            if (!program.GetTransformFeedbackStride(i)) continue;
            const auto& target = span.Targets[i];
            BufferSlice slice{};
            if (MG_Pipe::MGPipeHandleIsNull(target.Res) ||
                !m_bufferManager.AcquireWireSlice(BufferKind::Vertex, target.Res, slice))
                MagmaWireFatal("xfb-buffer-resource");
            if (target.Offset >= slice.size) return false;
            const Uint64 size = target.Size == 0 || target.Size == MG_Pipe::kMGPipeWholeBuffer
                ? slice.size - target.Offset : target.Size;
            if (!size || size > slice.size - target.Offset) return false;
            buffers[i] = slice.buffer;
            offsets[i] = slice.offset + target.Offset;
            sizes[i] = size;
            hasTarget = true;
            // The server owns the storage. Publish dirtiness through the existing
            // reverse channel so CPU readback does not reuse the client's old shadow.
            m_bufferManager.MarkWireBufferGpuWritten(target.Res, target.Offset, size);
        }
        if (!hasTarget) return false;
        return BeginXfbCaptureWithBuffers(frame, count, buffers, offsets, sizes);
    }

    static Bool ConvertWireVertexStream(const MGPVertexAttribWire& attr,
            VertexInputStateFactory::VertexStreamConversion conversion, const Uint8* source,
            SizeT stride, SizeT count, Vector<Uint8>& output) {
        using Conversion = VertexInputStateFactory::VertexStreamConversion;
        const auto type = static_cast<DataType>(attr.Type);
        const SizeT elementBytes = VertexInputStateFactory::GetAttributeByteSize(type, attr.Size, attr.IsBgra);
        if (conversion == Conversion::Repack) {
            output.resize(count * elementBytes);
            for (SizeT i = 0; i < count; ++i)
                Memcpy(output.data() + i * elementBytes, source + i * stride, elementBytes);
            return true;
        }
        output.resize(count * attr.Size * sizeof(Float));
        const auto convert = [&]<typename T>() {
            for (SizeT i = 0; i < count; ++i) {
                for (SizeT component = 0; component < attr.Size; ++component) {
                    T value{};
                    Memcpy(&value, source + i * stride + component * sizeof(T), sizeof(T));
                    const Float converted = static_cast<Float>(value);
                    Memcpy(output.data() + (i * attr.Size + component) * sizeof(Float), &converted, sizeof(Float));
                }
            }
            return true;
        };
        switch (type) {
        case DataType::Float64: return convert.template operator()<Double>();
        case DataType::Int8: return convert.template operator()<Int8>();
        case DataType::Uint8: return convert.template operator()<Uint8>();
        case DataType::Int16: return convert.template operator()<Int16>();
        case DataType::Uint16: return convert.template operator()<Uint16>();
        case DataType::Int32: return convert.template operator()<Int32>();
        case DataType::Uint32: return convert.template operator()<Uint32>();
        default: return false;
        }
    }

    void VulkanRenderer::DestroyWireDrawPass() {
        // Only shutdown/swapchain replacement calls this, after device idle.
        if (m_wireDrawPass && m_frameContext.GetFrameCount() != 0) {
            auto& frame = m_frameContext.GetCurrent();
            if (frame.isCommandRecording && VkRenderPassManager::GetActiveRenderPass())
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        }
        m_wireDrawPass.reset();
        for (const auto view : m_wireDrawViews) vkDestroyImageView(m_device, view, nullptr);
        m_wireDrawViews.clear();
        m_wireDrawPassKey = {};
    }

    void VulkanRenderer::RetireWireObjects(WireRetiredObjects objects) {
        // The current recording is the next graphics submission. Do not use a
        // frame number: mid-frame flushes and readbacks can split one frame.
        objects.submitIndex = m_submitCounter + 1;
        m_wireRetiredObjects.push_back(Move(objects));
    }

    void VulkanRenderer::RetireWireDrawPass() {
        if (!m_wireDrawPass) return;
        auto& frame = m_frameContext.GetCurrent();
        if (frame.isCommandRecording && VkRenderPassManager::GetActiveRenderPass())
            VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        if (m_wireDrawPassCaches.size() < m_frameContext.GetFrameCount())
            m_wireDrawPassCaches.resize(m_frameContext.GetFrameCount());
        auto& cache = m_wireDrawPassCaches[frameIndex];
        // Bound a present-less stream of distinct framebuffer configurations.
        // Overflow retains the original submit-index retirement path.
        constexpr SizeT kMaxCachedWirePassesPerFrame = 32;
        if (cache.size() < kMaxCachedWirePassesPerFrame) {
            cache.push_back({Move(m_wireDrawPassKey), Move(m_wireDrawPass), Move(m_wireDrawViews)});
            return;
        }
        WireRetiredObjects retired{};
        retired.drawPass = Move(m_wireDrawPass);
        retired.imageViews = Move(m_wireDrawViews);
        RetireWireObjects(Move(retired));
        m_wireDrawPassKey = {};
    }

    Bool VulkanRenderer::AcquireCachedWireDrawPass(const WireDrawPassKey& key) {
        const Uint32 frameIndex = m_frameContext.GetCurrentFrameIndex();
        if (frameIndex >= m_wireDrawPassCaches.size()) return false;
        auto& cache = m_wireDrawPassCaches[frameIndex];
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            // Exact values, not a compatibility hash: two FBOs with the same
            // formats can name different images, windows or draw-buffer slots.
            if (!(it->key == key)) continue;
            m_wireDrawPassKey = Move(it->key);
            m_wireDrawPass = Move(it->pass);
            m_wireDrawViews = Move(it->views);
            cache.erase(it);
            return true;
        }
        return false;
    }

    void VulkanRenderer::ClearWireDrawPassCache(Uint32 frameIndex) {
        if (frameIndex >= m_wireDrawPassCaches.size()) return;
        auto& cache = m_wireDrawPassCaches[frameIndex];
        for (auto& entry : cache) {
            entry.pass.reset();
            for (const auto view : entry.views) vkDestroyImageView(m_device, view, nullptr);
        }
        cache.clear();
    }

    void VulkanRenderer::ClearAllWireDrawPassCaches() {
        for (Uint32 frameIndex = 0; frameIndex < m_wireDrawPassCaches.size(); ++frameIndex)
            ClearWireDrawPassCache(frameIndex);
    }

    void VulkanRenderer::CollectWireObjects(Uint64 completedSubmit, Bool all) {
        SizeT retained = 0;
        for (SizeT index = 0; index < m_wireRetiredObjects.size(); ++index) {
            auto& entry = m_wireRetiredObjects[index];
            if (!all && entry.submitIndex > completedSubmit) {
                if (retained != index) m_wireRetiredObjects[retained] = Move(entry);
                ++retained;
                continue;
            }
            // Framebuffers/descriptor sets stop naming their views first.
            entry.drawPass.reset();
            for (const auto pool : entry.descriptorPools) vkDestroyDescriptorPool(m_device, pool, nullptr);
            for (const auto framebuffer : entry.framebuffers) vkDestroyFramebuffer(m_device, framebuffer, nullptr);
            for (const auto pass : entry.renderPasses) vkDestroyRenderPass(m_device, pass, nullptr);
            for (const auto view : entry.imageViews) vkDestroyImageView(m_device, view, nullptr);
        }
        // Erase once: repeated front erases move the whole remaining cohort for
        // every draw retired by a fence, turning a batch retirement quadratic.
        m_wireRetiredObjects.resize(retained);
    }

    struct WirePreparationScope {
        Uint32& depth;
        explicit WirePreparationScope(Uint32& value) : depth(value) { ++depth; }
        ~WirePreparationScope() { --depth; }
    };

    Bool VulkanRenderer::SetupWireDraw(FrameContext::FrameData& frame, GLenum mode,
            Flags<DrawSetupAspect> aspects, const DrawCmdParam& drawParams, const IndexBufferView* indices) {
        auto& state = MG_Pipe::MGPipeApplier();
        const auto program = WireProgramSource(state.BoundShaderCso);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) return false;
        if (aspects & DrawSetupAspect::IndirectDrawBuffer)
            WireBufferLegacyFatal();
        MakeXfbWritesVisible();
        const auto* fbo = state.DrawFramebuffer();
        if (!fbo) MagmaWireFatal("draw-framebuffer-record");
        RetireWireDrawPass();
        // CPU preparation may synchronize GPU-produced vertex/index bytes. Do
        // not rewind an arena while this draw owns as-yet-unrecorded slices.
        WirePreparationScope preparing(m_wirePreparationDepth);

        ProgramFactory::CompileOptionFlags flags =
            ProgramFactory::CompileOptionFlags(GetBaseTransformFlagsRaw(fbo->IsDefault));
        if (m_transformFeedbackFeatureEnabled && MGB_CTX->IsTransformFeedbackActive() &&
            program.GetTransformFeedbackVaryingCount() > 0)
            flags |= ProgramFactory::CompileOptionBit::XfbCapture;
        const auto* programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        if (!(aspects & DrawSetupAspect::IndexBuffer) && programObject->readsBaseVertexBuiltin) {
            flags |= ProgramFactory::CompileOptionBit::ZeroBaseVertex;
            programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        }
        if (programObject->pointSizeCapabilityUnsupported) return false;
        const auto& programObj = *programObject;
        m_currentDrawXfbCaptureDeclined = programObj.xfbCaptureDeclined;
        if (!m_uniformManager->PrepareWireTextureResources(program, programObj)) return false;
        // Lazy image uploads/promotions can submit the old recording. Resolve
        // ALL attachments before descriptor binding captures the current cmd.
        for (Uint32 i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
            const Int32 slot = fbo->DrawBuffers[i];
            if (slot >= 0) (void)ResolveWireImage(*fbo, fbo->Color[slot], VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (fbo->IsDefault || fbo->Depth.Kind != MG_Pipe::kMGPipeSurfaceKindNone)
            (void)ResolveWireImage(*fbo, fbo->Depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        else if (fbo->Stencil.Kind != MG_Pipe::kMGPipeSurfaceKindNone)
            (void)ResolveWireImage(*fbo, fbo->Stencil, VK_IMAGE_ASPECT_STENCIL_BIT);
        m_textureManager->FlushPendingUploads();
        const auto vaoHandle = state.BoundVertexElements;
        if (MG_Pipe::MGPipeHandleIsNull(vaoHandle)) MagmaWireFatal("vertex-elements-record");
        if (vaoHandle.Slot >= state.VertexElementsCsos.size()) MagmaWireFatal("vertex-elements-record");
        const auto& vao = state.VertexElementsCsos[vaoHandle.Slot];
        if (!vao.Live || vao.Gen != vaoHandle.Gen) MagmaWireFatal("vertex-elements-generation");
        VertexInputStateFactory::BackendVertexInputState input{};
        if (!m_vertexInputStateFactory->BuildWireVertexInput(vao, state,
                programObj.activeVertexInputLocationMask, input)) MagmaWireFatal("vertex-layout@P7");
        Vector<BufferSlice> vertexSlices(input.bindings.size());
        for (SizeT binding = 0; binding < input.bindings.size(); ++binding) {
            const Uint32 location = input.bindingAttributeLocations[binding];
            const auto& attr = vao.Attributes[location];
            const auto& resource = state.VertexBuffers[location];
            if (MG_Pipe::MGPipeHandleIsNull(resource.Res)) MagmaWireFatal("client-vertex-array@P8");
            BufferSlice slice{};
            if (!m_bufferManager.AcquireWireSlice(BufferKind::Vertex, resource.Res, slice))
                MagmaWireFatal("vertex-buffer-resource");
            const VkDeviceSize offset = input.bindingBaseOffsets[binding];
            const auto type = static_cast<DataType>(attr.Type);
            const SizeT elementSize = VertexInputStateFactory::GetAttributeByteSize(type, attr.Size, attr.IsBgra);
            if (offset > slice.size || elementSize > slice.size - offset) return false;
            const auto conversion = input.bindingConversions[binding];
            if (conversion != VertexInputStateFactory::VertexStreamConversion::None) {
                const SizeT stride = static_cast<SizeT>(attr.Stride);
                SizeT count = stride ? 1 + (static_cast<SizeT>(slice.size - offset) - elementSize) / stride : 1;
                // Direct non-indexed draws bound the conversion; indexed/multi-draw may
                // name arbitrary vertices, so preserve the entire valid source stream.
                if (stride && !(aspects & DrawSetupAspect::IndexBuffer) && !resource.Divisor)
                    count = std::min<SizeT>(count, static_cast<SizeT>(drawParams.firstVertex) + drawParams.vertexCount);
                if (!count) return false;
                Vector<Uint8> source((count - 1) * stride + elementSize);
                if (!m_bufferManager.ReadWireBuffer(resource.Res, offset, source.size(), source.data())) return false;
                Vector<Uint8> converted;
                if (!ConvertWireVertexStream(attr, conversion, source.data(), stride, count, converted))
                    MagmaWireFatal("vertex-format-conversion@P7");
                if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                        converted.data(), converted.size(), 16, vertexSlices[binding])) return false;
            } else {
                slice.offset += offset;
                slice.size -= offset;
                vertexSlices[binding] = slice;
            }
        }

        BufferSlice indexSlice{};
        VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
        if (aspects & DrawSetupAspect::IndexBuffer) {
            if (!indices) return false;
            const SizeT width = MG_Util::GetGLTypeSize(indices->indexType);
            if (width != 1 && width != 2 && width != 4) return false;
            indexType = width == 4 ? VK_INDEX_TYPE_UINT32 : width == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8;
            const auto& restart = MGB_CTX->GetRenderStateParameters();
            const Uint32 fixed = MG_Util::FixedRestartIndexForGLType(indices->indexType);
            const Bool replaceRestart = restart.PrimitiveRestartEnabled && !restart.PrimitiveRestartFixedIndexEnabled &&
                restart.PrimitiveRestartIndex < fixed;
            const Bool expandByte = width == 1 && !m_indexTypeUint8ExtensionEnabled;
            const Bool cpuIndices = indices->forceClientMemory || MG_Pipe::MGPipeHandleIsNull(state.IndexBuffer.Res);
            Vector<Uint8> copied;
            const void* bytes = nullptr;
            if (cpuIndices) bytes = reinterpret_cast<const void*>(indices->indexByteOffset);
            else {
                if (!m_bufferManager.AcquireWireSlice(BufferKind::Index, state.IndexBuffer.Res, indexSlice))
                    MagmaWireFatal("index-buffer-resource");
                const Uint64 offset = state.IndexBuffer.Offset + indices->indexByteOffset;
                if (offset < state.IndexBuffer.Offset || offset > indexSlice.size ||
                    indices->indexByteSize > indexSlice.size - offset) return false;
                if (replaceRestart || expandByte || offset % width) {
                    copied.resize(indices->indexByteSize);
                    if (!m_bufferManager.ReadWireBuffer(state.IndexBuffer.Res, offset, copied.size(), copied.data())) return false;
                    bytes = copied.data();
                }
                indexSlice.offset += offset;
                indexSlice.size = indices->indexByteSize;
            }
            if (bytes) {
                Vector<Uint8> rewritten;
                SizeT uploadSize = indices->indexByteSize;
                if (replaceRestart || expandByte) {
                    const SizeT outWidth = expandByte ? 2 : width;
                    rewritten.resize(uploadSize / width * outWidth);
                    for (SizeT i = 0; i < uploadSize / width; ++i) {
                        Uint32 value = 0;
                        Memcpy(&value, static_cast<const Uint8*>(bytes) + i * width, width);
                        if (replaceRestart && value == restart.PrimitiveRestartIndex) value = fixed;
                        if (expandByte && ResolvePrimitiveRestartEnable(aspects, indices) && value == fixed) value = 0xffffu;
                        Memcpy(rewritten.data() + i * outWidth, &value, outWidth);
                    }
                    bytes = rewritten.data();
                    uploadSize = rewritten.size();
                    if (expandByte) indexType = VK_INDEX_TYPE_UINT16;
                }
                if (!m_bufferManager.UploadTransient(BufferKind::Index, m_frameContext.GetCurrentFrameIndex(),
                        bytes, uploadSize, 16, indexSlice)) return false;
            }
            if (!indexSlice.IsValid()) return false;
        }

        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        // Descriptor resolution can upgrade a view's root image to STORAGE usage.
        // Resolve it before constructing framebuffer views so both roles of that image
        // name the same allocation even on its first storage-image use.
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_GRAPHICS)) return false;

        Vector<VkAttachmentDescription> attachments;
        Vector<VkImageViewCreateInfo> viewInfos;
        Vector<VkAttachmentReference> colors;
        VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        VkExtent2D extent{~0u, ~0u};
        Uint32 layers = ~0u;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        Uint64 passHash = 0;
        WireDrawPassKey passKey{};
        passKey.isDefault = fbo->IsDefault;
        passKey.framebufferSrgb = MGB_CTX->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb);
        if (fbo->IsDefault) {
            passKey.swapchain = m_swapchainObject.GetHandle();
            passKey.swapchainImageIndex = m_imageIndexAcquired;
        }
        const auto add = [&](const MG_Pipe::MGPSurface& surface, VkImageAspectFlags aspect) -> Uint32 {
            WireImage image = ResolveWireImage(*fbo, surface, aspect);
            if (!image.image) return VK_ATTACHMENT_UNUSED;
            m_textureManager->FlushPendingUploads();
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                image.format = ResolveSrgbAttachmentWriteFormat(image.format,
                    MGB_CTX->IsCapabilityEnabled(CapabilityInput::FramebufferSrgb));
            const VkImageLayout layout = aspect == VK_IMAGE_ASPECT_COLOR_BIT ?
                VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_GENERAL;
            TransitionWireImage(image, layout);
            VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            info.image = image.image;
            info.viewType = image.viewType;
            info.format = image.format;
            info.subresourceRange = {aspect, image.level, 1, image.layer, image.layers};
            if (image.format == VK_FORMAT_D24_UNORM_S8_UINT || image.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
                info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            viewInfos.push_back(info);
            passKey.attachments.push_back({image.storage, static_cast<Uint32>(surface.Kind), image.image,
                image.viewType, image.format, info.subresourceRange.aspectMask, aspect,
                image.level, image.layer, image.layers, image.levels, image.extent.width, image.extent.height,
                image.samples});
            VkAttachmentDescription description{};
            description.format = image.format;
            description.samples = image.samples;
            description.loadOp = description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            description.storeOp = description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            description.initialLayout = description.finalLayout = layout;
            attachments.push_back(description);
            extent.width = std::min(extent.width, image.extent.width);
            extent.height = std::min(extent.height, image.extent.height);
            layers = std::min(layers, image.layers);
            samples = image.samples;
            passHash = XXH64(&description, sizeof(description), passHash);
            if (!image.isDefault && surface.Kind == MG_Pipe::kMGPipeSurfaceKindTexture)
                m_textureManager->MarkWireTextureGpuWritten(image.storage, image.level, image.layer, image.layers);
            if (image.isDefault) {
                if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) m_swapchainObject.SetImageContentDefined(m_imageIndexAcquired, true);
                else m_swapchainObject.SetDepthStencilContentDefined(m_imageIndexAcquired, true);
            }
            return static_cast<Uint32>(attachments.size() - 1);
        };
        for (Uint32 i = 0; i < MG_Pipe::kMGPipeMaxColorAttachments; ++i) {
            const Int32 slot = fbo->DrawBuffers[i];
            const Uint32 index = slot < 0 ? VK_ATTACHMENT_UNUSED : add(fbo->Color[slot], VK_IMAGE_ASPECT_COLOR_BIT);
            colors.push_back({index, VK_IMAGE_LAYOUT_GENERAL});
        }
        while (!colors.empty() && colors.back().attachment == VK_ATTACHMENT_UNUSED) colors.pop_back();
        if (fbo->IsDefault || fbo->Depth.Kind != MG_Pipe::kMGPipeSurfaceKindNone)
            depth.attachment = add(fbo->Depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        else if (fbo->Stencil.Kind != MG_Pipe::kMGPipeSurfaceKindNone)
            depth.attachment = add(fbo->Stencil, VK_IMAGE_ASPECT_STENCIL_BIT);
        if (attachments.empty()) return false;
        passKey.width = extent.width;
        passKey.height = extent.height;
        passKey.layers = layers;
        passKey.depthReference = depth.attachment;
        for (const auto& color : colors) passKey.colorReferences.push_back(color.attachment);
        // A native handle can be recycled after image replacement or death.
        // Sample epochs after every lazy resolve, then compare them with the key.
        passKey.textureImageEpoch = m_textureManager->GetTextureImageEpoch();
        passKey.resourceEraseEpoch = m_textureManager->GetResourceEraseEpoch();
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<Uint32>(colors.size());
        subpass.pColorAttachments = colors.data();
        subpass.pDepthStencilAttachment = depth.attachment != VK_ATTACHMENT_UNUSED ? &depth : nullptr;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount = static_cast<Uint32>(attachments.size());
        passInfo.pAttachments = attachments.data();
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        if (!AcquireCachedWireDrawPass(passKey)) {
            m_wireDrawPassKey = Move(passKey);
            Vector<VkImageView> views;
            views.reserve(viewInfos.size());
            for (const auto& info : viewInfos) {
                VkImageView view = VK_NULL_HANDLE;
                VK_VERIFY(vkCreateImageView(m_device, &info, nullptr, &view));
                m_wireDrawViews.push_back(view);
                views.push_back(view);
            }
            m_wireDrawPass = MakeUnique<RenderPassEntry>();
            auto& created = *m_wireDrawPass;
            VK_VERIFY(vkCreateRenderPass(m_device, &passInfo, nullptr, &created.renderPass));
            VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fbInfo.renderPass = created.renderPass;
            fbInfo.attachmentCount = static_cast<Uint32>(views.size());
            fbInfo.pAttachments = views.data();
            fbInfo.width = extent.width; fbInfo.height = extent.height; fbInfo.layers = layers;
            VK_VERIFY(vkCreateFramebuffer(m_device, &fbInfo, nullptr, &created.framebuffer));
            created.attachmentCount = static_cast<Uint32>(attachments.size());
            created.colorAttachmentCount = static_cast<Uint32>(colors.size());
            created.hasDepthStencilAttachment = depth.attachment != VK_ATTACHMENT_UNUSED;
            created.sampleCount = samples;
            created.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
            created.layers = layers;
            created.hash = created.compatibilityHash = XXH64(colors.data(), colors.size() * sizeof(colors[0]), passHash);
        }
        auto& pass = *m_wireDrawPass;
        WireRenderPassCompatibilityKey pipelinePassKey{};
        for (const auto& attachment : attachments)
            pipelinePassKey.attachmentFormatsAndSamples.push_back(
                (static_cast<Uint64>(attachment.format) << 32) | static_cast<Uint32>(attachment.samples));
        for (const auto& color : colors) pipelinePassKey.colorReferences.push_back(color.attachment);
        pipelinePassKey.depthReference = depth.attachment;
        const Uint64 compatibilityId = m_wireRenderPassCompatibility.Intern(pipelinePassKey);
        const auto pipeline = GetOrCreatePipelineWithInput(mode, program, programObj, flags, input, pass,
            ResolvePrimitiveRestartEnable(aspects, indices), compatibilityId);
        if (pipeline == VK_NULL_HANDLE) return false;
        for (Uint32 i = 0; i < vertexSlices.size(); ++i)
            vkCmdBindVertexBuffers(frame.commandBuffer, i, 1, &vertexSlices[i].buffer, &vertexSlices[i].offset);
        Uint32 binding = static_cast<Uint32>(vertexSlices.size());
        for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
            if (!(programObj.activeVertexInputLocationMask & (1u << location)) ||
                (input.attributeLocationMask & (1u << location))) continue;
            VkFormat format = VK_FORMAT_UNDEFINED;
            const void* data = nullptr;
            VkDeviceSize size = 0;
            if (!TryGetCurrentVertexAttributeUploadPayload(MGB_CTX->GetCurrentVertexAttribute(location),
                    programObj.vertexInputTypes[location], format, data, size)) return false;
            BufferSlice slice{};
            if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                    data, size, 16, slice)) return false;
            vkCmdBindVertexBuffers(frame.commandBuffer, binding++, 1, &slice.buffer, &slice.offset);
        }
        if (indexSlice.IsValid()) vkCmdBindIndexBuffer(frame.commandBuffer, indexSlice.buffer, indexSlice.offset, indexType);
        if (!VkRenderPassManager::BeginRenderPass(frame.commandBuffer, pass)) return false;
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        ApplyDynamicDrawStateTail(frame, pass.extent, fbo->IsDefault,
                                 ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin));
        if (MG_Util::PipeStats::Enabled()) MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::Draws, 1);
        return true;
    }

    void VulkanRenderer::DispatchWireCompute(GLuint x, GLuint y, GLuint z) {
        RetireWireDrawPass();
        auto& frame = m_frameContext.GetCurrent();
        const auto program = WireProgramSource(MG_Pipe::MGPipeApplier().DispatchProgram);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) return;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, {});
        if (!m_uniformManager->PrepareWireTextureResources(program, programObj)) return;
        const auto pipeline = GetOrCreateComputePipeline(programObj);
        if (!pipeline) return;
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_COMPUTE)) return;
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdDispatch(frame.commandBuffer, x, y, z);
    }
#endif
