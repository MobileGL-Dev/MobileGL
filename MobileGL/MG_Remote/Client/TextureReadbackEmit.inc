// Included in EmitTables.cpp's client-only anonymous namespace.
// The server returns owned tight pixels. Only the client interprets PACK state
// and application pointers; PBO rows are ordinary buffer uploads after the reply.
        void EmitGetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                                 TextureUploadTarget uploadTarget, GLint level, GLenum format,
                                 GLenum type, GLsizei, void* pixels) {
            if (!texture || texture->GetStorageType() != TextureStorageType::Mipmap) return;
            auto* mipmap = static_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
            const Bool wholeCube = texture->GetTarget() == TextureTarget::TextureCubeMap &&
                                   uploadTarget == TextureUploadTarget::Unknown;
            const auto firstTarget = wholeCube ? TextureUploadTarget::CubeMapPositiveX : uploadTarget;
            const IntVec3 size = mipmap->GetMipmapTexelSize(firstTarget, level);
            if (size.x() <= 0 || size.y() <= 0 || size.z() <= 0) return;
            ClientSession& session = RequireSession("GetTextureImage");
            BeforeReadOnlyVerb();
            // A by-name texture need not occur in any currently bound sampler.
            MG_Pipe::MGPipeTextureEmitterInstance().DrainTextureSubData(*MG_State::pGLContext);
            const Uint64 bpp = ReadbackBytesPerPixel(format, type);
            const Uint64 depth = wholeCube ? 6 : static_cast<Uint64>(size.z());
            const Uint64 tight = static_cast<Uint64>(size.x()) * size.y() * depth * bpp;
            MG_Pipe::MGPReadbackInfo info{};
            info.Res = MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::Texture,
                                                              texture->GetLifetimeId());
            info.Box = {0, 0, 0, static_cast<Uint32>(size.x()), static_cast<Uint32>(size.y()),
                        static_cast<Uint32>(depth)};
            info.Format = format;
            info.Type = type;
            info.Target = wholeCube ? GL_TEXTURE_CUBE_MAP : MG_Util::ConvertTextureUploadTargetToGLEnum(uploadTarget);
            info.Level = static_cast<Uint16>(level);
            Vector<Uint8> bytes(static_cast<SizeT>(tight));
            const Uint64 capacity = session.MaxReplyBytes();
            if (!capacity) Wire::WireProtocolFatal("GetTextureImage.reply", "zero reply capacity");
            for (Uint64 offset = 0; offset < tight; ) {
                info.DstOffset = offset;
                info.DstSize = std::min(tight - offset, capacity);
                Int32 status = Wire::ReplySink::kStatusError;
                Uint64 received = 0;
                session.EmitAndWait(MG_Pipe::MGPWireOp::GetTextureImage, &info, sizeof(info), nullptr, 0,
                                    bytes.data() + offset, info.DstSize, &status, &received);
                RequireReadbackReplyComplete(status, received, info.DstSize);
                offset += info.DstSize;
            }
            const auto pack = MG_State::pGLContext->GetPixelStoreParameters(false);
            ApplyReadbackByteSwap(bytes.data(), tight, type, pack);
            const Uint64 rowBytes = static_cast<Uint64>(size.x()) * bpp;
            const Uint64 alignment = std::max<Int>(pack.Alignment, 1);
            const Uint64 rowPixels = pack.RowLength > 0 ? pack.RowLength : size.x();
            const Uint64 rowStride = (rowPixels * bpp + alignment - 1) / alignment * alignment;
            const auto target = texture->GetTarget();
            const Bool imagePack = wholeCube || target == TextureTarget::Texture3D ||
                target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMapArray;
            const Uint64 imageRows = imagePack && pack.ImageHeight > 0 ? pack.ImageHeight : size.y();
            const Uint64 imageStride = imageRows * rowStride;
            const Uint64 start = static_cast<Uint64>(pack.SkipRows) * rowStride +
                static_cast<Uint64>(pack.SkipPixels) * bpp +
                (imagePack ? static_cast<Uint64>(pack.SkipImages) * imageStride : 0);
            const auto& pbo = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
            for (Uint64 z = 0; z < depth; ++z) {
                for (Int y = 0; y < size.y(); ++y) {
                    const Uint8* source = bytes.data() + (z * size.y() + y) * rowBytes;
                    const Uint64 offset = start + z * imageStride + y * rowStride;
                    if (pbo) pbo->UploadSubData({source, static_cast<SizeT>(rowBytes)},
                                               reinterpret_cast<SizeT>(pixels) + offset);
                    else if (pixels) Memcpy(static_cast<Uint8*>(pixels) + offset, source, rowBytes);
                }
            }
        }

        void EmitGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
            const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            const auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            const auto& texture = unit.GetBindingSlot(textureTarget).GetBoundObject();
            EmitGetTextureImage(texture, MG_Util::ConvertGLEnumToTextureUploadTarget(target),
                                level, format, type, -1, pixels);
        }
