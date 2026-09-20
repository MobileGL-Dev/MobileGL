// Included in DirectGLES.cpp after the native packed-word readback helper.
// The server needs owned tight rows for a storage promotion, not client PACK
// layout or a fallback into a frontend texture's potentially stale shadow.
    class ScopedWireTextureReadLevel {
    public:
        ScopedWireTextureReadLevel(GLuint texture, GLenum target, GLint level)
            : m_texture(texture), m_target(target) {
            switch (target) {
            case GL_TEXTURE_2D: m_binding = GL_TEXTURE_BINDING_2D; break;
            case GL_TEXTURE_3D: m_binding = GL_TEXTURE_BINDING_3D; break;
            case GL_TEXTURE_2D_ARRAY: m_binding = GL_TEXTURE_BINDING_2D_ARRAY; break;
            case GL_TEXTURE_CUBE_MAP: m_binding = GL_TEXTURE_BINDING_CUBE_MAP; break;
            case GL_TEXTURE_CUBE_MAP_ARRAY: m_binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; break;
            default: return;
            }
            GLint previous = 0;
            DrainESErrors();
            g_GLESFuncs.glGetIntegerv(m_binding, &previous);
            g_GLESFuncs.glBindTexture(m_target, m_texture);
            g_GLESFuncs.glGetTexParameteriv(m_target, GL_TEXTURE_BASE_LEVEL, &m_baseLevel);
            g_GLESFuncs.glGetTexParameteriv(m_target, GL_TEXTURE_MAX_LEVEL, &m_maxLevel);
            m_saved = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            if (m_saved) {
                // ES framebuffer completeness can reject a defined level whose
                // base level or intervening mip chain is missing/inconsistent.
                // Read this existing native image as the sole base level. The
                // logical GL parameters and every cached copy remain unchanged.
                g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_BASE_LEVEL, level);
                g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_MAX_LEVEL, level);
                m_valid = g_GLESFuncs.glGetError() == GL_NO_ERROR;
            }
            g_GLESFuncs.glBindTexture(m_target, static_cast<GLuint>(previous));
        }

        ~ScopedWireTextureReadLevel() {
            if (!m_saved) return;
            // The packed-word path can use its own texture scratch binding.
            // Save the binding at restoration time too; do not assume an active
            // texture unit or make the backend's binding cache forget a bind.
            GLint previous = 0;
            g_GLESFuncs.glGetIntegerv(m_binding, &previous);
            g_GLESFuncs.glBindTexture(m_target, m_texture);
            g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_BASE_LEVEL, m_baseLevel);
            g_GLESFuncs.glTexParameteri(m_target, GL_TEXTURE_MAX_LEVEL, m_maxLevel);
            g_GLESFuncs.glBindTexture(m_target, static_cast<GLuint>(previous));
        }

        Bool Valid() const { return m_valid; }

    private:
        GLuint m_texture;
        GLenum m_target;
        GLenum m_binding = GL_NONE;
        GLint m_baseLevel = 0, m_maxLevel = 0;
        Bool m_saved = false, m_valid = false;
    };

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

    static Bool ReadWireDepthStencilLevelTight(GLuint texture, TextureTarget target,
                                               TextureUploadTarget uploadTarget, GLint level,
                                               const IntVec3& extent, GLenum format, GLenum type,
                                               Vector<Uint8>& bytes) {
        const SizeT pixelBytes = MG_Util::GetInputBytesPerPixel(
            MG_Util::ConvertGLEnumToTextureInputFormat(format), MG_Util::ConvertGLEnumToTexturePixelDataType(type));
        if (!pixelBytes) return false;
        const Bool needDepth = format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL;
        const Bool needStencil = format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL;
        ScopedFramebufferBinding bindings(true, false);
        GLuint fbo = 0;
        g_GLESFuncs.glGenFramebuffers(1, &fbo);
        struct DeleteFbo { GLuint id; ~DeleteFbo() { g_GLESFuncs.glDeleteFramebuffers(1, &id); } } cleanup{fbo};
        FramebufferImpl::BindFramebufferId(GL_READ_FRAMEBUFFER, fbo);
        ScopedPixelPackBuffer packBuffer(0);
        ScopedPackState packState(PixelStoreImpl::PackState{1, 0, 0, 0});
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        const GLenum point = needDepth ? (needStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT)
                                      : GL_STENCIL_ATTACHMENT;
        const SizeT count = static_cast<SizeT>(extent.x()) * extent.y();
        Vector<Uint8> result(count * extent.z() * pixelBytes);
        if (g_GLESFuncs.glMemoryBarrier) g_GLESFuncs.glMemoryBarrier(GL_ALL_BARRIER_BITS);
        g_GLESFuncs.glFinish();
        for (GLint z = 0; z < extent.z(); ++z) {
            if (nativeTarget == GL_TEXTURE_3D || nativeTarget == GL_TEXTURE_2D_ARRAY ||
                nativeTarget == GL_TEXTURE_CUBE_MAP_ARRAY)
                g_GLESFuncs.glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, point, texture, level, z);
            else
                g_GLESFuncs.glFramebufferTexture2D(GL_READ_FRAMEBUFFER, point,
                    TextureImpl::ConvertTextureUploadTargetToBackendGLEnum(uploadTarget), texture, level);
            g_GLESFuncs.glReadBuffer(GL_NONE);
            if (g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            Vector<Float> depth;
            Vector<Uint8> stencil;
            if (needDepth && !ReadDepthValuesNative(0, 0, extent.x(), extent.y(), depth)) return false;
            if (needStencil && !ReadStencilBytesNative(0, 0, extent.x(), extent.y(), stencil)) return false;
            auto* dst = result.data() + count * z * pixelBytes;
            for (SizeT i = 0; i < count; ++i) {
                if (format == GL_DEPTH_STENCIL) {
                    const Uint32 s = stencil[i];
                    if (type == GL_UNSIGNED_INT_24_8) {
                        const Uint32 packed = (NormalizedDepthToUnsigned(depth[i], 16777215.0) << 8) | s;
                        Memcpy(dst + i * pixelBytes, &packed, sizeof(packed));
                    } else if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) {
                        Memcpy(dst + i * pixelBytes, &depth[i], sizeof(Float));
                        Memcpy(dst + i * pixelBytes + sizeof(Float), &s, sizeof(s));
                    } else return false;
                } else if (type == GL_FLOAT) {
                    const Float value = needDepth ? depth[i] : static_cast<Float>(stencil[i]);
                    Memcpy(dst + i * pixelBytes, &value, sizeof(value));
                } else if (type == GL_HALF_FLOAT) {
                    const Uint16 value = MG_Util::EncodeFloatToHalfBits(
                        needDepth ? depth[i] : static_cast<Float>(stencil[i]));
                    Memcpy(dst + i * pixelBytes, &value, sizeof(value));
                } else {
                    const Bool signedType = type == GL_BYTE || type == GL_SHORT || type == GL_INT;
                    const Double maximum = signedType
                        ? (pixelBytes == 1 ? 127.0 : pixelBytes == 2 ? 32767.0 : 2147483647.0)
                        : (pixelBytes == 1 ? 255.0 : pixelBytes == 2 ? 65535.0 : 4294967295.0);
                    const Uint32 value = needDepth ? NormalizedDepthToUnsigned(depth[i], maximum) : stencil[i];
                    if (pixelBytes == 1) dst[i] = static_cast<Uint8>(value);
                    else if (pixelBytes == 2) { const Uint16 v = static_cast<Uint16>(value); Memcpy(dst + i * 2, &v, 2); }
                    else if (pixelBytes == 4) Memcpy(dst + i * 4, &value, 4);
                    else return false;
                }
            }
        }
        bytes = Move(result);
        return true;
    }

    Bool ReadTextureLevelTight(GLuint texture, TextureTarget target, TextureUploadTarget uploadTarget,
                               TextureInternalFormat logicalFormat, GLint level, const IntVec3& logicalExtent,
                               Bool sourceUsesImageCarrier, GLenum format, GLenum type, Vector<Uint8>& bytes) {
        if (!texture || level < 0 || TextureImpl::IsMultisampleTextureTarget(target)) return false;
        const IntVec3 extent = TextureImpl::GetBackendUploadSize(target, logicalExtent);
        const GLenum nativeTarget = TextureImpl::ConvertTextureTargetToBackendGLEnum(target);
        ScopedWireTextureReadLevel readLevel(texture, nativeTarget, level);
        if (!readLevel.Valid()) return false;
        if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX)
            return ReadWireDepthStencilLevelTight(texture, target, uploadTarget, level, extent, format, type, bytes);
        ReadbackChannelMapping mapping{};
        if (!GetReadbackChannelMapping(format, mapping)) return false;
        const SizeT pixelBytes = GetReadbackDstPixelSize(mapping, type);
        if (!pixelBytes || extent.x() <= 0 || extent.y() <= 0 || extent.z() <= 0) return false;
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
            const GLenum status = g_GLESFuncs.glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                MGLOG_E_ONCE("Espryt wire texture readback: framebuffer status=0x%x texture=%u level=%d target=0x%x",
                            status, texture, level, nativeTarget);
                return false;
            }
            Vector<Uint8> slice;
            if (!ReadWireColorRowsTight(extent.x(), extent.y(), format, type,
                    TextureImpl::BackendTextureFormatAddsAlpha(logicalFormat, target), carrier, slice) ||
                slice.size() != sliceBytes) return false;
            Memcpy(result.data() + static_cast<SizeT>(layer) * sliceBytes, slice.data(), sliceBytes);
        }
        bytes = Move(result);
        return true;
    }

    Bool ReadTextureImageWire(const MG_Pipe::MGPReadbackInfo& info, Vector<Uint8>& bytes) {
        const auto* record = PipeTextureRecordForHandle(info.Res);
        if (!record) return false;
        TextureTarget target = TextureTarget::Unknown;
        for (Int i = 0; i < static_cast<Int>(TextureTarget::TextureTargetCount); ++i)
            if (MG_Pipe::MGPipeResourceTargetForTextureTarget(static_cast<TextureTarget>(i)) == record->Desc.Target)
                target = static_cast<TextureTarget>(i);
        if (target == TextureTarget::Unknown) return false;
        const auto twin = TextureImpl::SyncTextureToBackendByHandle(info.Res);
        if (!twin) return false;
        const Bool wholeCube = info.Target == GL_TEXTURE_CUBE_MAP;
        const IntVec3 extent{static_cast<Int>(info.Box.W), static_cast<Int>(info.Box.H),
                            wholeCube ? 1 : static_cast<Int>(info.Box.D)};
        Vector<Uint8> result;
        for (Int face = 0; face < (wholeCube ? 6 : 1); ++face) {
            const auto uploadTarget = wholeCube
                ? static_cast<TextureUploadTarget>(static_cast<Int>(TextureUploadTarget::CubeMapPositiveX) + face)
                : MG_Util::ConvertGLEnumToTextureUploadTarget(info.Target);
            Vector<Uint8> slice;
            if (!ReadTextureLevelTight(twin->GetBackendTextureId(), target, uploadTarget,
                    static_cast<TextureInternalFormat>(record->Desc.InternalFormat), info.Level, extent,
                    twin->RequiresImageBindableStorage(), info.Format, info.Type, slice)) return false;
            result.insert(result.end(), slice.begin(), slice.end());
        }
        bytes = Move(result);
        return true;
    }
