// Included in DirectGLES.cpp after the native packed-word readback helper.
// The server needs owned tight rows for a storage promotion, not client PACK
// layout or a fallback into a frontend texture's potentially stale shadow.
    static Bool ReadWireColorRowsTight(GLsizei width, GLsizei height, GLenum format, GLenum type,
                                      Bool forceOpaqueAlpha, const NormalizedImageCarrierRead& carrier,
                                      Vector<Uint8>& bytes) {
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) return false;
        const SizeT pixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (!pixelBytes || width <= 0 || height <= 0) return false;
        const GLenum componentType = QueryReadAttachmentComponentType();
        const Bool integerAttachment = componentType == GL_INT || componentType == GL_UNSIGNED_INT;
        if (carrier.Active() ? (mapping.isInteger || !integerAttachment)
                             : mapping.isInteger != integerAttachment) return false;

        GLint implFormat = 0, implType = 0;
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &implFormat);
        g_GLESFuncs.glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &implType);
        struct Candidate { GLenum format, type; };
        Vector<Candidate> candidates;
        if (carrier.Active()) {
            candidates.push_back({GL_RGBA_INTEGER, GL_UNSIGNED_INT});
        } else if (mapping.isInteger) {
            if (GetWideReadChannelCount(implFormat) > 0 && IsIntegerReadFormat(implFormat) &&
                (implType == GL_INT || implType == GL_UNSIGNED_INT))
                candidates.push_back({static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)});
            candidates.push_back({GL_RGBA_INTEGER, componentType == GL_INT ? GLenum(GL_INT) : GLenum(GL_UNSIGNED_INT)});
        } else {
            if (GetWideReadChannelCount(implFormat) > 0 && !IsIntegerReadFormat(implFormat) &&
                (CanDecodeWideSourceType(implType) ||
                 (implFormat == GL_RGBA && implType == GL_UNSIGNED_INT_2_10_10_10_REV)))
                candidates.push_back({static_cast<GLenum>(implFormat), static_cast<GLenum>(implType)});
            if (componentType == GL_FLOAT) candidates.push_back({GL_RGBA, GL_FLOAT});
            if (componentType == GL_SIGNED_NORMALIZED) {
                candidates.push_back({GL_RGBA, GL_SHORT});
                candidates.push_back({GL_RGBA, GL_BYTE});
            } else candidates.push_back({GL_RGBA, GL_UNSIGNED_BYTE});
        }
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        const SizeT pixels = static_cast<SizeT>(width) * static_cast<SizeT>(height);
        Vector<Uint8> wide;
        GLenum wideType = GL_NONE;
        Int channels = 0;
        DrainESErrors();
        for (const auto candidate : candidates) {
            channels = GetWideReadChannelCount(candidate.format);
            wide.resize(pixels * static_cast<SizeT>(channels) * GetReadbackComponentSize(candidate.type));
            g_GLESFuncs.glReadPixels(0, 0, width, height, candidate.format, candidate.type, wide.data());
            if (g_GLESFuncs.glGetError() == GL_NO_ERROR) { wideType = candidate.type; break; }
        }
        if (wideType == GL_NONE) return false;

        if (carrier.Active()) {
            Vector<Uint8> decoded(pixels * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(decoded.data());
            const auto* src = reinterpret_cast<const Uint32*>(wide.data());
            for (SizeT i = 0; i < pixels; ++i) {
                for (SizeT c = 0; c < 4; ++c) {
                    const Float divisor = static_cast<Float>(carrier.ChannelMax[c]);
                    dst[i * 4 + c] = carrier.SignedNormalized
                        ? std::max(static_cast<Float>(static_cast<Int16>(static_cast<Uint16>(src[i * 4 + c]))) / divisor, -1.0f)
                        : static_cast<Float>(src[i * 4 + c]) / divisor;
                }
            }
            wide = Move(decoded); wideType = GL_FLOAT; channels = 4;
        } else if (wideType == GL_UNSIGNED_INT_2_10_10_10_REV) {
            Vector<Uint8> decoded(pixels * 4 * sizeof(Float));
            auto* dst = reinterpret_cast<Float*>(decoded.data());
            for (SizeT i = 0; i < pixels; ++i) {
                Uint32 word = 0;
                Memcpy(&word, wide.data() + i * sizeof(word), sizeof(word));
                dst[i * 4] = static_cast<Float>(word & 1023u) / 1023.0f;
                dst[i * 4 + 1] = static_cast<Float>((word >> 10) & 1023u) / 1023.0f;
                dst[i * 4 + 2] = static_cast<Float>((word >> 20) & 1023u) / 1023.0f;
                dst[i * 4 + 3] = static_cast<Float>((word >> 30) & 3u) / 3.0f;
            }
            wide = Move(decoded); wideType = GL_FLOAT; channels = 4;
        }
        if (channels < 4) ExpandNarrowWideRead(wide, pixels, channels, wideType);
        if (forceOpaqueAlpha) ForceWideReadAlphaToOne(wide, pixels, wideType);
        Vector<Uint8> converted(pixels * pixelBytes);
        const SizeT sourceRowBytes = static_cast<SizeT>(width) * 4 * GetReadbackComponentSize(wideType);
        for (GLsizei row = 0; row < height; ++row)
            ReadbackImpl::ConvertWideReadbackRow(wide.data() + static_cast<SizeT>(row) * sourceRowBytes,
                converted.data() + static_cast<SizeT>(row) * width * pixelBytes, width, wideType, mapping, type);
        bytes = Move(converted);
        return true;
    }

    Bool ReadTextureLevelTight(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                               TextureInternalFormat logicalFormat, GLint level, const IntVec3& logicalExtent,
                               Bool sourceUsesImageCarrier, GLenum format, GLenum type, Vector<Uint8>& bytes) {
        if (!texture || level < 0 || TextureImpl::IsMultisampleTextureTarget(target)) return false;
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) return false;
        const SizeT pixelBytes = GetReadbackDstPixelSize(mapping, type);
        const IntVec3 extent = TextureImpl::GetBackendUploadSize(target, logicalExtent);
        if (!pixelBytes || extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) return false;
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        const GLenum faceTarget = TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
        const auto widening = sourceUsesImageCarrier
            ? TextureImpl::GetImageBindableStorageWidening(logicalFormat) : TextureImpl::ImageBindableStorageWidening{};
        const SizeT sliceBytes = static_cast<SizeT>(extent.x()) * extent.y() * pixelBytes;
        Vector<Uint8> result(sliceBytes * static_cast<SizeT>(extent.z()));
        if (g_GLESFuncs.glMemoryBarrier) g_GLESFuncs.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        g_GLESFuncs.glFinish();

        // A raw packed word may have redundant encodings or NaN payloads. Read
        // it through R32UI when the source really still uses that native layout.
        const Bool rawPacked = !widening && MG_Util::PixelStoreProcessor::IsRawPackedPixelTransfer(
            logicalFormat, MG_Util::ConvertGLEnumToTextureInputFormat(format),
            MG_Util::ConvertGLEnumToTexturePixelDataType(type));
        if (rawPacked && pixelBytes == sizeof(Uint32)) {
            const GLint face = uploadTarget >= TextureUploadTarget::CubeMapPositiveX &&
                               uploadTarget <= TextureUploadTarget::CubeMapNegativeZ
                ? static_cast<GLint>(uploadTarget) - static_cast<GLint>(TextureUploadTarget::CubeMapPositiveX) : 0;
            for (GLint layer = 0; layer < extent.z(); ++layer)
                if (!ReadPackedLevelWordsViaScratch(texture, nativeTarget, level, face + layer, extent.x(), extent.y(),
                        reinterpret_cast<Uint32*>(result.data() + static_cast<SizeT>(layer) * sliceBytes))) return false;
            bytes = Move(result);
            return true;
        }

        NormalizedImageCarrierRead carrier{};
        if (widening && widening.CarriesNormalizedCodes()) {
            for (SizeT c = 0; c < 4; ++c) carrier.ChannelMax[c] = widening.ChannelMax[c];
            carrier.SignedNormalized = widening.SignedNormalized;
        }
        TempFBOBinder binder(true);
        auto& framebuffer = binder.Framebuffer();
        for (GLint layer = 0; layer < extent.z(); ++layer) {
            if (nativeTarget == GL_TEXTURE_3D || nativeTarget == GL_TEXTURE_2D_ARRAY || nativeTarget == GL_TEXTURE_CUBE_MAP_ARRAY)
                ScratchFBOImpl::EnsureColorAttachmentLayer(framebuffer, GL_READ_FRAMEBUFFER, texture, level, layer);
            else
                ScratchFBOImpl::EnsureColorAttachment2D(framebuffer, GL_READ_FRAMEBUFFER, texture, faceTarget, level);
            ScratchFBOImpl::EnsureReadBuffer(framebuffer, GL_COLOR_ATTACHMENT0);
            if (g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            Vector<Uint8> slice;
            if (!ReadWireColorRowsTight(extent.x(), extent.y(), format, type,
                    TextureImpl::BackendTextureFormatAddsAlpha(logicalFormat, target), carrier, slice) ||
                slice.size() != sliceBytes) return false;
            Memcpy(result.data() + static_cast<SizeT>(layer) * sliceBytes, slice.data(), sliceBytes);
        }
        bytes = Move(result);
        return true;
    }
