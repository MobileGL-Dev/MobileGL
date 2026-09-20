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
        // FlushPendingCommands only submits. These objects may still be referenced by
        // that submission, so retire this temporary pass after the queue completes.
        if (m_wireDrawPass || !m_wireDrawViews.empty()) VK_VERIFY(vkQueueWaitIdle(m_graphicsQueue));
        m_wireDrawPass.reset();
        for (const auto view : m_wireDrawViews) vkDestroyImageView(m_device, view, nullptr);
        m_wireDrawViews.clear();
    }

    Bool VulkanRenderer::SetupWireDraw(FrameContext::FrameData& frame, GLenum mode,
            Flags<DrawSetupAspect> aspects, const DrawCmdParam& drawParams, const IndexBufferView* indices) {
        auto& state = MG_Pipe::MGPipeApplier();
        const auto program = WireProgramSource(state.BoundShaderCso);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) return false;
        if (aspects & DrawSetupAspect::IndirectDrawBuffer || MGB_CTX->IsTransformFeedbackActive())
            WireBufferLegacyFatal();
        const auto* fbo = state.DrawFramebuffer();
        if (!fbo) MagmaWireFatal("draw-framebuffer-record");
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("draw-flush");
        DestroyWireDrawPass();

        ProgramFactory::CompileOptionFlags flags =
            ProgramFactory::CompileOptionFlags(GetBaseTransformFlagsRaw(fbo->IsDefault));
        const auto* programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        if (!(aspects & DrawSetupAspect::IndexBuffer) && programObject->readsBaseVertexBuiltin) {
            flags |= ProgramFactory::CompileOptionBit::ZeroBaseVertex;
            programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        }
        if (programObject->pointSizeCapabilityUnsupported) return false;
        const auto& programObj = *programObject;
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
        Vector<VkImageView> views;
        Vector<VkAttachmentReference> colors;
        VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        VkExtent2D extent{~0u, ~0u};
        Uint32 layers = ~0u;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        Uint64 passHash = 0;
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
            VkImageView view = VK_NULL_HANDLE;
            VK_VERIFY(vkCreateImageView(m_device, &info, nullptr, &view));
            m_wireDrawViews.push_back(view);
            views.push_back(view);
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
        m_wireDrawPass = MakeUnique<RenderPassEntry>();
        auto& pass = *m_wireDrawPass;
        VK_VERIFY(vkCreateRenderPass(m_device, &passInfo, nullptr, &pass.renderPass));
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = pass.renderPass;
        fbInfo.attachmentCount = static_cast<Uint32>(views.size());
        fbInfo.pAttachments = views.data();
        fbInfo.width = extent.width; fbInfo.height = extent.height; fbInfo.layers = layers;
        VK_VERIFY(vkCreateFramebuffer(m_device, &fbInfo, nullptr, &pass.framebuffer));
        pass.attachmentCount = static_cast<Uint32>(attachments.size());
        pass.colorAttachmentCount = static_cast<Uint32>(colors.size());
        pass.hasDepthStencilAttachment = depth.attachment != VK_ATTACHMENT_UNUSED;
        pass.sampleCount = samples;
        pass.extent = {static_cast<Int>(extent.width), static_cast<Int>(extent.height)};
        pass.layers = layers;
        pass.hash = pass.compatibilityHash = XXH64(colors.data(), colors.size() * sizeof(colors[0]), passHash);
        const auto pipeline = GetOrCreatePipelineWithInput(mode, program, programObj, flags, input, pass,
            ResolvePrimitiveRestartEnable(aspects, indices));
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
        if (HasPendingRecordedWork() && !FlushPendingCommands()) MagmaWireFatal("dispatch-flush");
        auto& frame = m_frameContext.GetCurrent();
        const auto program = WireProgramSource(MG_Pipe::MGPipeApplier().DispatchProgram);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) return;
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, {});
        const auto pipeline = GetOrCreateComputePipeline(programObj);
        if (!pipeline) return;
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_COMPUTE)) return;
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdDispatch(frame.commandBuffer, x, y, z);
    }
#endif
