// Server-owned draw/dispatch setup. The legacy setup remains the monolith implementation.
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

    // Through the same seam as MagmaWireFatal, and for the same reason (P7 wave 0). The family
    // stays RoleViolation: reaching the legacy buffer arm inside a Magma server is a role
    // invariant the two halves must hold, not a verb the wire failed to migrate.
    [[noreturn]] static void WireBufferLegacyFatal() {
        MG_Pipe::MGPipeSessionFail(
            MG_Pipe::MGPipeFatalFamily::RoleViolation,
            "MGPipe: Fatal{RoleViolation, \"buffer-legacy-arm\"} (Magma P7 buffer consumer)");
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
        m_wireDrawPassCompatibilityId = 0;
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
        m_wireDrawPassCompatibilityId = 0;
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
        // The per-draw containers (VulkanRenderer.h's WireDrawScratch). Nothing re-enters this
        // function from inside it today; if something ever does, it gets its own set rather
        // than clearing the outer draw's.
        WireDrawScratch nestedScratch;
        WireDrawScratch& scratch = m_wirePreparationDepth == 0 ? m_wireDrawScratch : nestedScratch;
        const auto program = WireProgramSource(state.BoundShaderCso);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) {
            MGL_WIRE_DECLINE_AT(ProgramLinkOrSpirv, "program is not linked or carries no SPIR-V");
            return false;
        }
        // P8-D: the wire arm's own indirect draw (DrawWireIndirectNative) is the one caller that
        // may pass this aspect; a monolith indirect entry point reached here is still the role
        // violation.
        if ((aspects & DrawSetupAspect::IndirectDrawBuffer) && !m_wireNativeIndirectDraw)
            WireBufferLegacyFatal();
        MakeXfbWritesVisible();
        const auto* fbo = state.DrawFramebuffer();
        if (!fbo) MagmaWireFatal("draw-framebuffer-record");
        // THE PREVIOUS DRAW'S RENDER PASS STAYS OPEN until this draw's attachments are known: a
        // draw to the same attachments continues it (below), and on a tiler a pass per draw also
        // loaded and stored every attachment once per draw. Resource work in between that may not
        // sit inside a pass (a barrier, a copy, a clear) closes it itself (RenderPassGuard.h).
        // CPU preparation may synchronize GPU-produced vertex/index bytes. Do
        // not rewind an arena while this draw owns as-yet-unrecorded slices.
        WirePreparationScope preparing(m_wirePreparationDepth);

        ProgramFactory::CompileOptionFlags flags =
            ProgramFactory::CompileOptionFlags(GetBaseTransformFlagsRaw(fbo->IsDefault));
        if (m_transformFeedbackFeatureEnabled && MG_Pipe::gPipeInputs.IsTransformFeedbackActive() &&
            program.GetTransformFeedbackVaryingCount() > 0)
            flags |= ProgramFactory::CompileOptionBit::XfbCapture;
        const auto* programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        if (!(aspects & DrawSetupAspect::IndexBuffer) && programObject->readsBaseVertexBuiltin) {
            flags |= ProgramFactory::CompileOptionBit::ZeroBaseVertex;
            programObject = &m_programFactory->GetOrCreateProgram(program, flags);
        }
        if (programObject->invalidSpirvDeclined) {
            MGL_WIRE_DECLINE_AT(ProgramInvalidSpirv, "a module of the program fails SPIR-V validation and was never built");
            return false;
        }
        if (programObject->pointSizeCapabilityUnsupported) {
            MGL_WIRE_DECLINE_AT(PointSizeCapability, "the program writes gl_PointSize and the device cannot express it");
            return false;
        }
        const auto& programObj = *programObject;
        m_currentDrawXfbCaptureDeclined = programObj.xfbCaptureDeclined;
        if (!m_uniformManager->PrepareWireTextureResources(program, programObj)) {
            MGL_WIRE_DECLINE_AT(TextureResourcesUnprepared, "a sampled/storage image could not be made ready; see the Tex* tally");
            return false;
        }
        // Shared images another session writes: acquired ahead of the recording that samples them
        // (WireSharedImage.inc).
        AcquireNotedSharedImages();
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
        auto& input = scratch.input;
        // P7 wave 2 package C, CONTRACT-P7 §3.2: THE `vertex-layout` ROW IS RETIRED AND SPLIT.
        //
        // A false is now ONE reason and only one - `buffer-window`, an enabled attribute the
        // program reads whose slot lies outside the window set_vertex_buffers published. That
        // is a disagreement about what crossed, not about what this device can express, so it
        // keeps a named Fatal through the hook (MGPipeSessionFail -> Session::Fail, which is
        // what rule I asks of a refusal that must stay a refusal). It is the check that caught
        // the Redmi VAO hash collision fixed in 376c04be, which is why it is not softened.
        // The P7 marker is gone from the detail string with the abort: this is a permanent
        // protocol error, not a shape the split path has yet to migrate, so it must not keep
        // counting against the census of unmigrated refusals (scripts/ci/fatal_census.py
        // counts the marker literally, which is also why no comment in this package spells it).
        if (!m_vertexInputStateFactory->BuildWireVertexInput(vao, state,
                programObj.activeVertexInputLocationMask, input))
            MagmaWireFatal("vertex-layout-buffer-window");
        // The seven device/format reasons are now MASKED OUT of the vertex input state, and
        // this is the monolith arm's pre-flight for them, statement for statement
        // (VulkanRenderer.cpp's SetupDraw: "Fail loudly rather than render wrong pixels"). An
        // enabled array with no usable VkFormat never reaches the vertex input state, which
        // makes it indistinguishable from a DISABLED one - and a disabled one is answered
        // below by uploading the current attribute value, so without this check the draw would
        // quietly feed the shader a constant in place of the application's vertex data.
        //
        // A DECLINE, NOT A FATAL (rule I (a)): the draw is skipped, the reason is named once,
        // and the observable is the monolith lane's for the same VAO on the same device.
        // BuildWireVertexInput only considers attributes the program reads, so a masked bit is
        // always an active one and this predicate is the whole of the difference.
        if ((input.unsupportedAttribMask & programObj.activeVertexInputLocationMask) != 0) {
            MGLOG_E_ONCE("Magma wire draw skipped: program=%u reads vertex attribute location "
                         "mask 0x%x whose enabled array has no supported vertex format",
                         program.GetExternalIndex(),
                         input.unsupportedAttribMask & programObj.activeVertexInputLocationMask);
            WireDeclineTally::Count(WireDeclineSite::VertexFormatUnsupported);
            return false;
        }
        auto& vertexSlices = scratch.vertexSlices;
        vertexSlices.assign(input.bindings.size(), BufferSlice{});
        for (SizeT binding = 0; binding < input.bindings.size(); ++binding) {
            const Uint32 location = input.bindingAttributeLocations[binding];
            const auto& attr = vao.Attributes[location];
            const auto& resource = state.VertexBuffers[location];
            // P8-F: A RECORD INVARIANT, NOT AN UNMIGRATED VERB (it carried a P8 phase tag).
            // The client turns every enabled client-memory array into an owned buffer before the
            // draw record is built, or refuses the draw itself (MG_Impl/Pipe/OwnedDrawInputs.h),
            // so an attribute the program reads with no buffer handle is a foreign peer's record.
            if (MG_Pipe::MGPipeHandleIsNull(resource.Res))
                MG_Pipe::MGPipeSessionFail(MG_Pipe::MGPipeFatalFamily::ProtocolCorruption,
                    "MGPipe: Fatal{ProtocolCorruption, \"Magma:client-vertex-array\"} location=%u: an "
                    "attribute the program reads carries no buffer handle", location);
            BufferSlice slice{};
            if (!m_bufferManager.AcquireWireSlice(BufferKind::Vertex, resource.Res, slice)) {
                // A buffer with no data store is legal GL to bind (glGenBuffers + glBindBuffer, no
                // glBufferData), and the client mints its handle at construction while the create
                // only crosses with the first respecify (PipeFill.cpp's M-1) - so the handle may be
                // one this server never saw, or one it holds with zero bytes. Either way the draw
                // reads no storage: it is declined, as the GL draws nothing defined from it.
                static Bool s_described = false;
                if (!s_described && !m_bufferManager.IsKnownWireBuffer(resource.Res)) {
                    s_described = true;
                    m_bufferManager.DescribeWireBufferHistory(resource.Res);
                }
                MGL_WIRE_DECLINE_AT(VertexAttributeWindow, "an enabled attribute's buffer has no storage");
                return false;
            }
            const VkDeviceSize offset = input.bindingBaseOffsets[binding];
            const auto type = static_cast<DataType>(attr.Type);
            const SizeT elementSize = VertexInputStateFactory::GetAttributeByteSize(type, attr.Size, attr.IsBgra);
            if (offset > slice.size || elementSize > slice.size - offset) {
                MGL_WIRE_DECLINE_AT(VertexAttributeWindow, "an enabled attribute's first element lies outside its buffer");
                return false;
            }
            const auto conversion = input.bindingConversions[binding];
            if (conversion != VertexInputStateFactory::VertexStreamConversion::None) {
                const SizeT stride = static_cast<SizeT>(attr.Stride);
                SizeT count = stride ? 1 + (static_cast<SizeT>(slice.size - offset) - elementSize) / stride : 1;
                // Direct non-indexed draws bound the conversion; indexed/multi-draw may
                // name arbitrary vertices, so preserve the entire valid source stream. So may an
                // indirect draw (P8-D): its vertex range is in GPU words nobody read.
                if (stride && !(aspects & DrawSetupAspect::IndexBuffer) &&
                    !(aspects & DrawSetupAspect::IndirectDrawBuffer) && !resource.Divisor)
                    count = std::min<SizeT>(count, static_cast<SizeT>(drawParams.firstVertex) + drawParams.vertexCount);
                if (!count) {
                    MGL_WIRE_DECLINE_AT(VertexStreamCountZero, "the converted vertex stream would hold no elements");
                    return false;
                }
                Vector<Uint8> source((count - 1) * stride + elementSize);
                if (!m_bufferManager.ReadWireBuffer(resource.Res, offset, source.size(), source.data())) {
                    MGL_WIRE_DECLINE_AT(VertexStreamRead, "the vertex bytes to convert could not be read back");
                    return false;
                }
                Vector<Uint8> converted;
                // P7 wave 2 package C, CONTRACT-P7 §3.2 `vertex-format-conversion`: DECLINE.
                // The converter is asked only for a conversion BuildWireVertexInput already
                // chose, so a refusal here means the two disagree about what is convertible -
                // a MobileGL-internal defect, not a peer's malformed record and not a verb the
                // split path has yet to migrate. Killing the session for it told the peer
                // "protocol corruption" about a bug on this side; skipping the draw and naming
                // it once is the honest answer and is what the row asks for. The packed
                // converter that would make the refusal unreachable is the row's optional half
                // and is not taken here.
                if (!ConvertWireVertexStream(attr, conversion, source.data(), stride, count, converted)) {
                    MGLOG_E_ONCE("Magma wire draw skipped: no vertex stream converter for "
                                 "location=%u type=%u size=%u conversion=%d",
                                 location, attr.Type, static_cast<Uint32>(attr.Size),
                                 static_cast<Int>(conversion));
                    WireDeclineTally::Count(WireDeclineSite::VertexStreamConverter);
                    return false;
                }
                if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                        converted.data(), converted.size(), 16, vertexSlices[binding])) {
                    MGL_WIRE_DECLINE_AT(VertexTransientUpload, "the converted vertex stream did not fit the transient arena");
                    return false;
                }
            } else {
                slice.offset += offset;
                slice.size -= offset;
                vertexSlices[binding] = slice;
            }
        }

        BufferSlice indexSlice{};
        VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
        if (aspects & DrawSetupAspect::IndexBuffer) {
            if (!indices) {
                MGL_WIRE_DECLINE_AT(IndexViewMissing, "an indexed draw arrived with no index view");
                return false;
            }
            const SizeT width = MG_Util::GetGLTypeSize(indices->indexType);
            if (width != 1 && width != 2 && width != 4) {
                MGL_WIRE_DECLINE_AT(IndexTypeWidth, "the index type is not 1, 2 or 4 bytes wide");
                return false;
            }
            indexType = width == 4 ? VK_INDEX_TYPE_UINT32 : width == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8;
            const auto& restart = MG_Pipe::gPipeInputs.GetRenderStateParameters();
            const Uint32 fixed = MG_Util::FixedRestartIndexForGLType(indices->indexType);
            const Bool replaceRestart = restart.PrimitiveRestartEnabled && !restart.PrimitiveRestartFixedIndexEnabled &&
                restart.PrimitiveRestartIndex < fixed;
            const Bool expandByte = width == 1 && !m_indexTypeUint8ExtensionEnabled;
            const Bool cpuIndices = indices->forceClientMemory || MG_Pipe::MGPipeHandleIsNull(state.IndexBuffer.Res);
            Vector<Uint8> copied;
            const void* bytes = nullptr;
            if (cpuIndices) bytes = reinterpret_cast<const void*>(indices->indexByteOffset);
            else {
                if (!m_bufferManager.AcquireWireSlice(BufferKind::Index, state.IndexBuffer.Res, indexSlice)) {
                    // As for a vertex buffer: a bound element buffer with no data store yet.
                    static Bool s_described = false;
                    if (!s_described && !m_bufferManager.IsKnownWireBuffer(state.IndexBuffer.Res)) {
                        s_described = true;
                        m_bufferManager.DescribeWireBufferHistory(state.IndexBuffer.Res);
                    }
                    MGL_WIRE_DECLINE_AT(IndexWindow, "the element buffer has no storage");
                    return false;
                }
                const Uint64 offset = state.IndexBuffer.Offset + indices->indexByteOffset;
                if (offset < state.IndexBuffer.Offset || offset > indexSlice.size ||
                    indices->indexByteSize > indexSlice.size - offset) {
                    MGL_WIRE_DECLINE_AT(IndexWindow, "the index range lies outside its buffer");
                    return false;
                }
                if (replaceRestart || expandByte || offset % width) {
                    copied.resize(indices->indexByteSize);
                    if (!m_bufferManager.ReadWireBuffer(state.IndexBuffer.Res, offset, copied.size(), copied.data())) {
                        MGL_WIRE_DECLINE_AT(IndexStreamRead, "the indices to rewrite could not be read back");
                        return false;
                    }
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
                        bytes, uploadSize, 16, indexSlice)) {
                    MGL_WIRE_DECLINE_AT(IndexTransientUpload, "the rewritten indices did not fit the transient arena");
                    return false;
                }
            }
            if (!indexSlice.IsValid()) {
                MGL_WIRE_DECLINE_AT(IndexSliceInvalid, "no usable index slice was resolved");
                return false;
            }
        }

        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        // Descriptor resolution can upgrade a view's root image to STORAGE usage.
        // Resolve it before constructing framebuffer views so both roles of that image
        // name the same allocation even on its first storage-image use.
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_GRAPHICS)) {
            MGL_WIRE_DECLINE_AT(UniformBufferBinding, "the program's uniform/storage bindings could not be bound");
            return false;
        }

        auto& attachments = scratch.attachments;
        auto& viewInfos = scratch.viewInfos;
        auto& colors = scratch.colors;
        attachments.clear();
        viewInfos.clear();
        colors.clear();
        VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        VkExtent2D extent{~0u, ~0u};
        Uint32 layers = ~0u;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        Uint64 passHash = 0;
        auto& passKey = scratch.passKey;
        passKey.ResetKeepingCapacity();
        passKey.isDefault = fbo->IsDefault;
        passKey.framebufferSrgb = MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::FramebufferSrgb);
        if (fbo->IsDefault) {
            passKey.swapchain = m_swapchainObject.GetHandle();
            // A drawing pass on the default framebuffer is what makes the freshly acquired
            // swapchain image the one GL addresses (see m_defaultFramebufferImageIndex); the
            // attachments below resolve the same index, so this key has to be taken after it.
            passKey.swapchainImageIndex = DefaultFramebufferWriteIndex(true);
        }
        auto& sameLayoutAttachments = scratch.sameLayoutAttachments;
        sameLayoutAttachments.clear();
        const auto add = [&](const MG_Pipe::MGPSurface& surface, VkImageAspectFlags aspect) -> Uint32 {
            WireImage image = ResolveWireImage(*fbo, surface, aspect, /*isWriteTarget=*/true);
            if (!image.image) return VK_ATTACHMENT_UNUSED;
            m_textureManager->FlushPendingUploads();
            if (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                image.format = ResolveSrgbAttachmentWriteFormat(image.format,
                    MG_Pipe::gPipeInputs.IsCapabilityEnabled(CapabilityInput::FramebufferSrgb));
            const VkImageLayout layout = aspect == VK_IMAGE_ASPECT_COLOR_BIT ?
                VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_GENERAL;
            // Already in its attachment layout: its barrier (which closes any open pass) waits until
            // it is known whether the open pass continues with this very attachment.
            if (image.layout == layout) sameLayoutAttachments.push_back(image);
            else TransitionWireImage(image, layout);
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
                if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) m_swapchainObject.SetImageContentDefined(image.swapchainImageIndex, true);
                else m_swapchainObject.SetDepthStencilContentDefined(image.swapchainImageIndex, true);
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
        if (attachments.empty()) {
            MGL_WIRE_DECLINE_AT(NoAttachments, "the draw framebuffer resolved to no attachments");
            return false;
        }
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
        const Bool continuing = m_wireDrawPass && m_wireDrawPassKey == passKey;
        {
            const auto* active = VkRenderPassManager::GetActiveRenderPass();
            const Bool stillOpen = continuing && active != nullptr && active->commandBuffer == frame.commandBuffer;
            // A new pass orders itself after earlier writes to its attachments; a continued one
            // is the same pass, whose attachment writes are ordered by rasterization.
            if (!stillOpen)
                for (auto& image : sameLayoutAttachments) TransitionWireImage(image, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (!continuing) {
            RetireWireDrawPass();
            m_wireDrawPassCompatibilityId = 0;
        }
        if (!continuing && !AcquireCachedWireDrawPass(passKey)) {
            // A copy, not a move: passKey is the scratch's, and a new pass is the rare case.
            m_wireDrawPassKey = passKey;
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
        // Interned once per pass: a continued pass has the same key, so the same compatibility.
        if (m_wireDrawPassCompatibilityId == 0) {
            auto& pipelinePassKey = scratch.compatibilityKey;
            pipelinePassKey.attachmentFormatsAndSamples.clear();
            pipelinePassKey.colorReferences.clear();
            for (const auto& attachment : attachments)
                pipelinePassKey.attachmentFormatsAndSamples.push_back(
                    (static_cast<Uint64>(attachment.format) << 32) | static_cast<Uint32>(attachment.samples));
            for (const auto& color : colors) pipelinePassKey.colorReferences.push_back(color.attachment);
            pipelinePassKey.depthReference = depth.attachment;
            m_wireDrawPassCompatibilityId = m_wireRenderPassCompatibility.Intern(pipelinePassKey);
        }
        const Uint64 compatibilityId = m_wireDrawPassCompatibilityId;
        const auto pipeline = GetOrCreatePipelineWithInput(mode, program, programObj, flags, input, pass,
            ResolvePrimitiveRestartEnable(aspects, indices), compatibilityId);
        if (pipeline == VK_NULL_HANDLE) {
            MGL_WIRE_DECLINE_AT(PipelineUnavailable, "no graphics pipeline could be created for this draw");
            return false;
        }
        for (Uint32 i = 0; i < vertexSlices.size(); ++i)
            vkCmdBindVertexBuffers(frame.commandBuffer, i, 1, &vertexSlices[i].buffer, &vertexSlices[i].offset);
        Uint32 binding = static_cast<Uint32>(vertexSlices.size());
        for (Uint32 location = 0; location < kMaxVertexAttribs; ++location) {
            if (!(programObj.activeVertexInputLocationMask & (1u << location)) ||
                (input.attributeLocationMask & (1u << location))) continue;
            VkFormat format = VK_FORMAT_UNDEFINED;
            const void* data = nullptr;
            VkDeviceSize size = 0;
            if (!TryGetCurrentVertexAttributeUploadPayload(MG_Pipe::gPipeInputs.GetCurrentVertexAttribute(location),
                    programObj.vertexInputTypes[location], format, data, size)) {
                MGL_WIRE_DECLINE_AT(CurrentAttributePayload, "a disabled array's current attribute value has no upload payload");
                return false;
            }
            BufferSlice slice{};
            if (!m_bufferManager.UploadTransient(BufferKind::Vertex, m_frameContext.GetCurrentFrameIndex(),
                    data, size, 16, slice)) {
                MGL_WIRE_DECLINE_AT(CurrentAttributeUpload, "the current attribute value did not fit the transient arena");
                return false;
            }
            vkCmdBindVertexBuffers(frame.commandBuffer, binding++, 1, &slice.buffer, &slice.offset);
        }
        if (indexSlice.IsValid()) vkCmdBindIndexBuffer(frame.commandBuffer, indexSlice.buffer, indexSlice.offset, indexType);
        // Still open from the previous draw (nothing closed it, same command buffer): continue.
        const auto* active = VkRenderPassManager::GetActiveRenderPass();
        const Bool open = continuing && active != nullptr && active->commandBuffer == frame.commandBuffer &&
                          active->hash == pass.hash;
        if (!open && active != nullptr) VkRenderPassManager::EndRenderPass(frame.commandBuffer);
        if (!open && !VkRenderPassManager::BeginRenderPass(frame.commandBuffer, pass)) {
            MGL_WIRE_DECLINE_AT(BeginRenderPass, "the render pass for this draw could not be begun");
            return false;
        }
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        ApplyDynamicDrawStateTail(frame, pass.extent, fbo->IsDefault,
                                 ResolveDrawViewportCount(programObj.writesViewportIndexBuiltin));
        // Storage images this draw writes are what the next sampled-image barrier must cover.
        if (programObj.hasStorageImages) BumpWireImageWriteEpoch();
        if (MG_Util::PipeStats::Enabled()) MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::Draws, 1);
        return true;
    }

    // ---- P8-D: THE WIRE ARM'S INDIRECT DRAWS READ THEIR WORDS ON THE GPU ----------------------
    //
    // The shape the monolith arm records (VulkanRenderer.cpp's MultiDraw{Elements,Arrays}Indirect
    // and MultiDrawElementsIndirectCount), with the wire store's own VkBuffer in place of the
    // frontend buffer's resident slice. Before this, every wire indirect draw read its commands
    // through ReadWireBuffer and expanded them on the CPU, and a store a shader had written made
    // that read a whole-GPU wait (WaitForWireBufferHostAccess) per draw.
    //
    // THE ONE FALSE: a COUNT form this device cannot issue - the monolith arm's own condition for
    // vkCmdDrawIndexedIndirectCount (MultiDrawElementsIndirectCount), extended to the non-indexed
    // twin the monolith never loads. Everything else is issued here or declined by name.
    Bool VulkanRenderer::DrawWireIndirectNative(GLenum mode, GLenum type, Uint64 offset, GLsizei drawcount,
                                                GLsizei stride, Bool indexed, Bool counted, Uint64 countOffset) {
        if (drawcount <= 0) return true;
        if (counted) {
            const auto countEntry = indexed ? s_vkCmdDrawIndexedIndirectCount : s_vkCmdWireDrawIndirectCount;
            if (!m_drawIndirectCountExtensionEnabled || countEntry == nullptr ||
                (!m_multiDrawIndirectFeatureEnabled && drawcount != 1))
                return false;
        }
        const auto& state = MG_Pipe::MGPipeApplier();
        // GL's command sizes (GL 4.6 10.3.10), which are also Vulkan's.
        const Uint64 commandSize = indexed ? 5 * sizeof(Uint32) : 4 * sizeof(Uint32);
        if (stride == 0) stride = static_cast<GLsizei>(commandSize);
        BufferSlice commands{};
        if (MG_Pipe::MGPipeHandleIsNull(state.VerbIndirectBuffer) ||
            !m_bufferManager.AcquireWireSlice(BufferKind::Indirect, state.VerbIndirectBuffer, commands)) {
            MGL_WIRE_DECLINE_AT(IndirectBufferUnbound, "an indirect draw names no live indirect buffer store");
            return true;
        }
        const Uint64 span = static_cast<Uint64>(stride) * static_cast<Uint64>(drawcount - 1) + commandSize;
        // GL refuses a stride that is not a multiple of four (INVALID_VALUE), and Vulkan requires
        // it for drawCount > 1; a peer's record that breaks either is declined, not issued.
        if (stride < static_cast<GLsizei>(commandSize) || (stride & 3) != 0 || (offset & 3u) != 0 ||
            offset > commands.size || span > commands.size - offset) {
            MGL_WIRE_DECLINE_AT(IndirectCommandRange, "the indirect commands lie outside their store or are misaligned");
            return true;
        }
        BufferSlice parameters{};
        if (counted) {
            if (MG_Pipe::MGPipeHandleIsNull(state.VerbIndirectParameterBuffer) ||
                !m_bufferManager.AcquireWireSlice(BufferKind::Indirect, state.VerbIndirectParameterBuffer, parameters) ||
                (countOffset & 3u) != 0 || countOffset > parameters.size || parameters.size - countOffset < sizeof(Uint32)) {
                MGL_WIRE_DECLINE_AT(IndirectCountRange, "the indirect draw count lies outside its parameter store");
                return true;
            }
        }

        // The index view is the WHOLE bound element store past its binding offset, as the monolith
        // arm binds it: the commands name their own firstIndex, which only the GPU reads.
        IndexBufferView indices{};
        DrawCmdParam vertexRange{};
        vertexRange.instanceCount = 1;
        Flags<DrawSetupAspect> aspects = DrawSetupAspect::IndirectDrawBuffer;
        if (indexed) {
            const SizeT width = MG_Util::GetGLTypeSize(type);
            BufferSlice elements{};
            if (width == 0 || MG_Pipe::MGPipeHandleIsNull(state.IndexBuffer.Res) ||
                !m_bufferManager.AcquireWireSlice(BufferKind::Index, state.IndexBuffer.Res, elements) ||
                state.IndexBuffer.Offset > elements.size) {
                MGL_WIRE_DECLINE_AT(IndirectIndexStore, "an indexed indirect draw names no usable element store");
                return true;
            }
            indices.indexType = type;
            indices.indexByteOffset = 0;
            indices.indexByteSize = static_cast<SizeT>(elements.size - state.IndexBuffer.Offset);
            vertexRange.vertexCount = static_cast<Uint32>(indices.indexByteSize / width);
            aspects |= DrawSetupAspect::IndexBuffer;
        }

        // A shader write the store was marked with (MarkWireBufferGpuWritten) is made visible to
        // the indirect read here, once per write, so a program that omits GL_COMMAND_BARRIER_BIT
        // is not left racing a read the old CPU path used to take after a whole-GPU wait. The
        // application's own glMemoryBarrier is BuildMemoryBarrierForGlBarriers, as on the monolith
        // arm; transfer writes carry their own MEMORY_READ after-barrier (VkBufferManager.cpp).
        const Bool commandsWritten = m_bufferManager.TakeWireIndirectReadBarrier(state.VerbIndirectBuffer);
        const Bool countWritten = counted && m_bufferManager.TakeWireIndirectReadBarrier(state.VerbIndirectParameterBuffer);
        if (commandsWritten || countWritten) {
            RetireWireDrawPass();
            auto& barrierFrame = m_frameContext.GetCurrent();
            if (!barrierFrame.isCommandRecording) m_frameContext.BeginCommandRecording();
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr)
                VkRenderPassManager::EndRenderPass(barrierFrame.commandBuffer);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(barrierFrame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            if (MG_Util::PipeStats::Enabled())
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireIndirectBarriers, 1);
        }

        auto& frame = m_frameContext.GetCurrent();
        m_wireNativeIndirectDraw = true;
        const Bool ready = SetupDraw(frame, mode, aspects, vertexRange, indexed ? &indices : nullptr);
        m_wireNativeIndirectDraw = false;
        if (!ready) return true;
        MOBILEGL_ASSERT(frame.isCommandRecording, "%s: frame recording was not started", __func__);

        const VkCommandBuffer commandBuffer = frame.commandBuffer;
        const VkDeviceSize first = commands.offset + offset;
        const Uint32 count = static_cast<Uint32>(drawcount);
        const Uint32 byteStride = static_cast<Uint32>(stride);
        if (counted) {
            const auto countEntry = indexed ? s_vkCmdDrawIndexedIndirectCount : s_vkCmdWireDrawIndirectCount;
            countEntry(commandBuffer, commands.buffer, first, parameters.buffer, parameters.offset + countOffset,
                       count, byteStride);
        } else if (count == 1 || (!m_multiDrawForceUnrollIndirect && m_multiDrawIndirectFeatureEnabled)) {
            // maxDrawIndirectCount bounds one call; GL's drawcount does not.
            const Uint32 limit = std::max<Uint32>(1u, m_physicalDevice.properties.limits.maxDrawIndirectCount);
            for (Uint32 base = 0; base < count; base += limit) {
                const Uint32 chunk = std::min(count - base, limit);
                const VkDeviceSize at = first + static_cast<VkDeviceSize>(base) * byteStride;
                if (indexed) vkCmdDrawIndexedIndirect(commandBuffer, commands.buffer, at, chunk, byteStride);
                else vkCmdDrawIndirect(commandBuffer, commands.buffer, at, chunk, byteStride);
            }
        } else {
            // No multiDrawIndirect: one indirect draw per command, still read by the GPU
            // (the monolith arm's own fallback).
            for (Uint32 i = 0; i < count; ++i) {
                const VkDeviceSize at = first + static_cast<VkDeviceSize>(i) * byteStride;
                if (indexed) vkCmdDrawIndexedIndirect(commandBuffer, commands.buffer, at, 1, 0);
                else vkCmdDrawIndirect(commandBuffer, commands.buffer, at, 1, 0);
            }
        }
        if (MG_Util::PipeStats::Enabled())
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireIndirectNativeDraws, 1);
        return true;
    }

    void VulkanRenderer::DispatchWireCompute(GLuint x, GLuint y, GLuint z) {
        RetireWireDrawPass();
        auto& frame = m_frameContext.GetCurrent();
        const auto program = WireProgramSource(MG_Pipe::MGPipeApplier().DispatchProgram);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) {
            MGL_WIRE_DECLINE_AT(ComputeProgramLinkOrSpirv, "the compute program is not linked or carries no SPIR-V");
            return;
        }
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, {});
        if (programObj.invalidSpirvDeclined) {
            MGL_WIRE_DECLINE_AT(ComputeProgramInvalidSpirv, "a module of the compute program fails SPIR-V validation and was never built");
            return;
        }
        if (!m_uniformManager->PrepareWireTextureResources(program, programObj)) {
            MGL_WIRE_DECLINE_AT(ComputeTextureResourcesUnprepared, "a sampled/storage image could not be made ready for the dispatch");
            return;
        }
        AcquireNotedSharedImages(); // shared images another session writes (WireSharedImage.inc)
        const auto pipeline = GetOrCreateComputePipeline(programObj);
        if (!pipeline) {
            MGL_WIRE_DECLINE_AT(ComputePipelineUnavailable, "no compute pipeline could be created for this dispatch");
            return;
        }
        if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (!m_uniformManager->BindProgramUniformBuffers(frame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_COMPUTE)) {
            MGL_WIRE_DECLINE_AT(ComputeUniformBufferBinding, "the dispatch's uniform/storage bindings could not be bound");
            return;
        }
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        EndActiveRenderPassOn(frame.commandBuffer);
        vkCmdDispatch(frame.commandBuffer, x, y, z);
    }

    // ---- P8-SV: THE WIRE ARM'S INDIRECT DISPATCH READS ITS GROUP COUNTS ON THE GPU ----------
    //
    // The monolith arm's shape (VulkanRenderer.cpp's DispatchComputeIndirect) with the wire
    // store's own VkBuffer in place of the frontend buffer's resident slice. Before this the wire
    // arm read the three words through ReadWireBuffer and dispatched them from the CPU, and a store
    // a shader had written made that read a whole-GPU wait (WaitForWireBufferHostAccess) per call.
    // There is no shape the device cannot issue, so there is no CPU path left: a record whose
    // store or words are unusable is declined by name.
    //
    // The barrier is P8-D's draw rule (DrawWireIndirectNative): a store marked shader-written gets
    // one MEMORY_WRITE -> INDIRECT_COMMAND_READ barrier before its next indirect read, whether or
    // not the application issued GL_COMMAND_BARRIER_BIT; DRAW_INDIRECT is the stage that consumes
    // VkDispatchIndirectCommand too. It is TAKEN before the bindings below, because binding this
    // dispatch's own storage buffers marks them written, and a store that is both this dispatch's
    // parameters and one of its outputs must keep that mark for the next reader.
    void VulkanRenderer::DispatchWireComputeIndirect(GLintptr offset) {
        RetireWireDrawPass();
        const auto& state = MG_Pipe::MGPipeApplier();
        constexpr Uint64 kCommandSize = 3 * sizeof(Uint32); // GL 4.6 10.3.10 / VkDispatchIndirectCommand
        BufferSlice parameters{};
        if (MG_Pipe::MGPipeHandleIsNull(state.VerbDispatchIndirectBuffer) ||
            !m_bufferManager.AcquireWireSlice(BufferKind::Indirect, state.VerbDispatchIndirectBuffer, parameters)) {
            MGL_WIRE_DECLINE_AT(DispatchIndirectBufferUnbound, "an indirect dispatch names no live dispatch-indirect store");
            return;
        }
        // GL refuses an offset that is negative or not a multiple of four (INVALID_VALUE) and a
        // command past the end of the buffer (INVALID_OPERATION); a peer's record that breaks
        // either is declined, not issued.
        if (offset < 0 || (static_cast<Uint64>(offset) & 3u) != 0 || static_cast<Uint64>(offset) > parameters.size ||
            parameters.size - static_cast<Uint64>(offset) < kCommandSize) {
            MGL_WIRE_DECLINE_AT(DispatchIndirectRange, "the dispatch group counts lie outside their store or are misaligned");
            return;
        }
        auto& frame = m_frameContext.GetCurrent();
        if (m_bufferManager.TakeWireIndirectReadBarrier(state.VerbDispatchIndirectBuffer)) {
            if (!frame.isCommandRecording) m_frameContext.BeginCommandRecording();
            if (VkRenderPassManager::GetActiveRenderPass() != nullptr)
                VkRenderPassManager::EndRenderPass(frame.commandBuffer);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            if (MG_Util::PipeStats::Enabled())
                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireIndirectBarriers, 1);
        }
        const auto program = WireProgramSource(state.DispatchProgram);
        if (!program.GetLinkStatus() || !program.GetSpirvStatus()) {
            MGL_WIRE_DECLINE_AT(ComputeProgramLinkOrSpirv, "the compute program is not linked or carries no SPIR-V");
            return;
        }
        const auto& programObj = m_programFactory->GetOrCreateProgram(program, {});
        if (programObj.invalidSpirvDeclined) {
            MGL_WIRE_DECLINE_AT(ComputeProgramInvalidSpirv, "a module of the compute program fails SPIR-V validation and was never built");
            return;
        }
        if (!m_uniformManager->PrepareWireTextureResources(program, programObj)) {
            MGL_WIRE_DECLINE_AT(ComputeTextureResourcesUnprepared, "a sampled/storage image could not be made ready for the dispatch");
            return;
        }
        AcquireNotedSharedImages(); // shared images another session writes (WireSharedImage.inc)
        const auto pipeline = GetOrCreateComputePipeline(programObj);
        if (!pipeline) {
            MGL_WIRE_DECLINE_AT(ComputePipelineUnavailable, "no compute pipeline could be created for this dispatch");
            return;
        }
        // PrepareWireTextureResources may have flushed and begun a new recording.
        auto& dispatchFrame = m_frameContext.GetCurrent();
        if (!dispatchFrame.isCommandRecording) m_frameContext.BeginCommandRecording();
        if (!m_uniformManager->BindProgramUniformBuffers(dispatchFrame.commandBuffer, program, programObj,
                m_frameContext.GetCurrentFrameIndex(), VK_PIPELINE_BIND_POINT_COMPUTE)) {
            MGL_WIRE_DECLINE_AT(ComputeUniformBufferBinding, "the dispatch's uniform/storage bindings could not be bound");
            return;
        }
        vkCmdBindPipeline(dispatchFrame.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        EndActiveRenderPassOn(dispatchFrame.commandBuffer);
        vkCmdDispatchIndirect(dispatchFrame.commandBuffer, parameters.buffer,
                              parameters.offset + static_cast<VkDeviceSize>(offset));
        if (MG_Util::PipeStats::Enabled())
            MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::WireIndirectNativeDispatches, 1);
    }
