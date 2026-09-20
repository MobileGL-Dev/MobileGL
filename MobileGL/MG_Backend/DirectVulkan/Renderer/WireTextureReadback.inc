// Server-owned texture readback. Included after WireFramebuffer.inc so image
// layout tracking and frame retirement are the same as every other wire verb.
#if MOBILEGL_BUILD_DISAGGREGATED
    static Bool PackWireDepthStencilTight(const Uint8* depth, const Uint8* stencil,
            VkFormat sourceFormat, SizeT count, GLenum format, GLenum type, Vector<Uint8>& result) {
        const Bool stencilOnly = format == GL_STENCIL_INDEX;
        const auto readDepth = [&](SizeT i) -> Float {
            if (sourceFormat == VK_FORMAT_D16_UNORM) {
                Uint16 word{}; Memcpy(&word, depth + i * 2, sizeof(word));
                return static_cast<Float>(word) / 65535.0f;
            }
            if (sourceFormat == VK_FORMAT_X8_D24_UNORM_PACK32 || sourceFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
                Uint32 word{}; Memcpy(&word, depth + i * 4, sizeof(word));
                return static_cast<Float>(word & 0xffffffu) / 16777215.0f;
            }
            Float value{}; Memcpy(&value, depth + i * sizeof(value), sizeof(value));
            return value;
        };
        SizeT size = 0;
        switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: size = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: size = 2; break;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: size = 4; break;
        case GL_UNSIGNED_INT_24_8: size = 4; break;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: size = 8; break;
        default: return false;
        }
        const Bool packed = type == GL_UNSIGNED_INT_24_8 || type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        if ((format == GL_DEPTH_STENCIL) != packed) return false;
        result.resize(count * size);
        for (SizeT i = 0; i < count; ++i) {
            Uint8* dst = result.data() + i * size;
            const Uint32 s = stencil ? stencil[i] : 0;
            const Float d = stencilOnly ? 0.0f : readDepth(i);
            switch (type) {
            case GL_FLOAT: {
                const Float value = stencilOnly ? static_cast<Float>(s) : d;
                Memcpy(dst, &value, sizeof(value)); break;
            }
            case GL_UNSIGNED_INT_24_8: {
                Uint32 depth24 = 0;
                if (sourceFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
                    Memcpy(&depth24, depth + i * 4, sizeof(depth24));
                    depth24 &= 0xffffffu;
                } else {
                    depth24 = static_cast<Uint32>(std::llround(std::clamp<double>(d, 0.0, 1.0) * 16777215.0));
                }
                const Uint32 value = (depth24 << 8) | s;
                Memcpy(dst, &value, sizeof(value)); break;
            }
            case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
                Memcpy(dst, &d, sizeof(d)); Memcpy(dst + 4, &s, sizeof(s)); break;
            default: {
                const Bool signedType = type == GL_BYTE || type == GL_SHORT || type == GL_INT;
                const Uint64 maximum = signedType ? ((Uint64{1} << (size * 8 - 1)) - 1)
                                                 : ((Uint64{1} << (size * 8)) - 1);
                const Uint32 value = stencilOnly ? s : static_cast<Uint32>(
                    std::llround(std::clamp<double>(d, 0.0, 1.0) * static_cast<double>(maximum)));
                Memcpy(dst, &value, size); break;
            }
            }
        }
        return true;
    }

    Bool VulkanRenderer::ReadTextureImageWire(const MG_Pipe::MGPReadbackInfo& info,
                                               Vector<Uint8>& ownedBytes) {
        ownedBytes.clear();
        const auto& box = info.Box;
        if (box.X || box.Y || box.Z || !box.W || !box.H || !box.D ||
            box.W > static_cast<Uint32>(std::numeric_limits<GLsizei>::max()) ||
            Uint64(box.H) * box.D > static_cast<Uint64>(std::numeric_limits<GLsizei>::max())) return false;
        const auto& state = MG_Pipe::MGPipeApplier();
        if (MG_Pipe::MGPipeHandleIsNull(info.Res) || info.Res.Slot >= state.TextureResources.size()) return false;
        const auto& record = state.TextureResources[info.Res.Slot];
        if (!record.Live || record.Gen != info.Res.Gen) return false;

        using Target = MG_Pipe::MGPipeResourceTarget;
        const auto target = static_cast<Target>(record.Desc.Target);
        const Bool oneDArray = target == Target::Tex1DArray;
        const Bool volume = target == Target::Tex3D;
        Uint32 level = info.Level, layer = 0, layerCount = 1, availableLayers = 1;
        VkExtent3D extent{box.W, box.H, 1};
        if (oneDArray) {
            if (info.Target != GL_TEXTURE_1D_ARRAY || box.D != 1) return false;
            layerCount = box.H; extent.height = 1;
        } else if (volume) {
            if (info.Target != GL_TEXTURE_3D) return false;
            extent.depth = box.D;
        } else if (target == Target::Tex2DArray || target == Target::TexCubeArray) {
            if (info.Target != (target == Target::Tex2DArray ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_CUBE_MAP_ARRAY)) return false;
            layerCount = box.D;
        } else if (target == Target::TexCube) {
            if (info.Target == GL_TEXTURE_CUBE_MAP) {
                if (box.D != 6) return false;
                layerCount = 6;
            } else if (info.Target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && info.Target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
                if (box.D != 1) return false;
                layer = info.Target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
            } else return false;
        } else {
            if (box.D != 1) return false;
            const GLenum expected = target == Target::Tex1D ? GL_TEXTURE_1D :
                                    target == Target::Tex2D ? GL_TEXTURE_2D :
                                    target == Target::TexRect ? GL_TEXTURE_RECTANGLE : GL_NONE;
            if (!expected || info.Target != expected || (target == Target::Tex1D && box.H != 1)) return false;
        }
        VkFormat viewFormat = VK_FORMAT_UNDEFINED;
        const auto storage = m_textureManager->ResolveWireTextureStorage(info.Res, level, layer, &viewFormat, &availableLayers);
        if (MG_Pipe::MGPipeHandleIsNull(storage)) return false;
        RetireWireDrawPass();
        MakeXfbWritesVisible();
        auto* resource = m_textureManager->SyncTextureResourceByHandle(storage);
        if (!resource || !resource->image || level >= resource->mipLevels ||
            resource->sampleCount != VK_SAMPLE_COUNT_1_BIT) return false;
        m_textureManager->FlushPendingUploads();
        const Bool nativeVolume = resource->viewType == VK_IMAGE_VIEW_TYPE_3D;
        const Uint32 nativeDepth = nativeVolume ? std::max(resource->depth >> level, 1u) : resource->arrayLayers;
        if (extent.width != std::max(resource->extent.width >> level, 1u) ||
            extent.height != std::max(resource->extent.height >> level, 1u) ||
            layer >= nativeDepth || (nativeVolume ? extent.depth : layerCount) > nativeDepth - layer ||
            (!nativeVolume && layerCount > availableLayers)) return false;
        // A view can reinterpret the same native words. Decode using the outer
        // view format, while Vulkan copies the storage allocation's bytes.
        const VkFormat format = viewFormat == VK_FORMAT_UNDEFINED ? resource->format : viewFormat;
        const Bool depth = info.Format == GL_DEPTH_COMPONENT || info.Format == GL_DEPTH_STENCIL;
        const Bool stencil = info.Format == GL_STENCIL_INDEX || info.Format == GL_DEPTH_STENCIL;
        const Bool depthStencil = depth || stencil;
        const VkImageAspectFlags aspects = depthStencil ?
            ((depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
        if ((resource->aspect & aspects) != aspects) return false;
        const Uint64 pixels64 = Uint64(box.W) * box.H * box.D;
        if (pixels64 > std::numeric_limits<SizeT>::max() / 16) return false;
        const SizeT pixels = static_cast<SizeT>(pixels64);
        const SizeT destinationPixelSize = MG_Util::GetInputBytesPerPixel(
            MG_Util::ConvertGLEnumToTextureInputFormat(info.Format), MG_Util::ConvertGLEnumToTexturePixelDataType(info.Type));
        if (!destinationPixelSize || pixels64 * destinationPixelSize != info.DstSize) return false;
        SizeT sourcePixelSize = GetReadbackTexelSize(resource->format);
        if (depthStencil) {
            switch (resource->format) {
            case VK_FORMAT_D16_UNORM: sourcePixelSize = 2; break;
            case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: sourcePixelSize = 4; break;
            case VK_FORMAT_S8_UINT: sourcePixelSize = 0; break;
            default: return false;
            }
        } else if (!sourcePixelSize || sourcePixelSize != GetReadbackTexelSize(format)) return false;
        const VkDeviceSize firstBytes = pixels * (depthStencil ? (depth ? sourcePixelSize : 0) : sourcePixelSize);
        const VkDeviceSize stencilOffset = (firstBytes + 3) & ~VkDeviceSize{3};
        const VkDeviceSize copyBytes = depthStencil ? stencilOffset + (stencil ? pixels : 0) : firstBytes;
        if (!copyBytes) return false;
        VkBufferObject readback;
        if (!readback.Create({.allocator = m_allocator, .size = copyBytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .memoryUsage = VMA_MEMORY_USAGE_AUTO, .allocationFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT})) return false;
        WireImage image{};
        image.image = resource->image; image.format = resource->format; image.aspect = resource->aspect;
        image.layout = resource->layout; image.trackedLayout = &resource->layout; image.levels = resource->mipLevels;
        const auto restore = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : image.layout;
        TransitionWireImage(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy regions[2]{};
        Uint32 regionCount = 0;
        const auto addCopy = [&](VkImageAspectFlags aspect, VkDeviceSize offset) {
            auto& copy = regions[regionCount++];
            copy.bufferOffset = offset;
            copy.imageSubresource = {aspect, level, nativeVolume ? 0u : layer, nativeVolume ? 1u : layerCount};
            copy.imageOffset = {0, 0, nativeVolume ? static_cast<Int32>(layer) : 0};
            copy.imageExtent = extent;
        };
        if (!depthStencil) addCopy(VK_IMAGE_ASPECT_COLOR_BIT, 0);
        if (depth) addCopy(VK_IMAGE_ASPECT_DEPTH_BIT, 0);
        if (stencil) addCopy(VK_IMAGE_ASPECT_STENCIL_BIT, stencilOffset);
        auto& frame = m_frameContext.GetCurrent();
        vkCmdCopyImageToBuffer(frame.commandBuffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              readback.GetHandle(), regionCount, regions);
        TransitionWireImage(image, restore);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        if (!SubmitReadbackCommandsAndWait(frame)) return false;
        const auto* mapped = static_cast<const Uint8*>(readback.Map());
        if (!mapped || !readback.Invalidate(copyBytes)) return false;
        Vector<Uint8> result;
        if (depthStencil) {
            if (!PackWireDepthStencilTight(depth ? mapped : nullptr, stencil ? mapped + stencilOffset : nullptr,
                                          resource->format, pixels, info.Format, info.Type, result)) return false;
        } else {
            using namespace DirectGLES::ReadbackImpl;
            ReadbackChannelMapping mapping{};
            if (!GetReadbackChannelMapping(info.Format, mapping) ||
                GetReadbackDstPixelSize(mapping, info.Type) != destinationPixelSize) return false;
            if (MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(GetPackedReadbackInternalFormat(format),
                    MG_Util::ConvertGLEnumToTextureInputFormat(info.Format),
                    MG_Util::ConvertGLEnumToTexturePixelDataType(info.Type))) {
                if (destinationPixelSize != sourcePixelSize) return false;
                result.assign(mapped, mapped + firstBytes);
            } else {
                Vector<Uint8> wide;
                GLenum wideType = GL_NONE;
                const GLsizei rows = static_cast<GLsizei>(Uint64(box.H) * box.D);
                if (!DecodeReadbackRowsToWide(mapped, format, box.W, rows, wide, wideType) ||
                    mapping.isInteger != (wideType == GL_INT || wideType == GL_UNSIGNED_INT)) return false;
                if (ResolveTextureFormatInfo(static_cast<TextureInternalFormat>(record.Desc.InternalFormat)).expandRgbToRgba) {
                    const Float floatOne = 1.0f;
                    const Uint32 integerOne = 1;
                    const void* alpha = wideType == GL_FLOAT ? static_cast<const void*>(&floatOne) : &integerOne;
                    for (SizeT i = 0; i < pixels; ++i)
                        Memcpy(wide.data() + (i * 4 + 3) * sizeof(Uint32), alpha, sizeof(Uint32));
                }
                result.resize(pixels * destinationPixelSize);
                const SizeT srcRow = static_cast<SizeT>(box.W) * 4 * sizeof(Uint32);
                const SizeT dstRow = static_cast<SizeT>(box.W) * destinationPixelSize;
                for (GLsizei row = 0; row < rows; ++row)
                    ConvertWideReadbackRow(wide.data() + row * srcRow, result.data() + row * dstRow,
                                           box.W, wideType, mapping, info.Type);
            }
        }
        if (result.size() != info.DstSize) return false;
        ownedBytes = Move(result);
        return true;
    }
#endif
