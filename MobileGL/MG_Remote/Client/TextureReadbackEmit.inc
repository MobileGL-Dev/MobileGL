// Included in EmitTables.cpp's client-only anonymous namespace.
// The server returns owned tight pixels. Only the client interprets PACK state
// and application pointers; PBO rows are ordinary buffer uploads after the reply.
        struct ReadbackLayout {
            Uint64 RowBytes = 0, RowStride = 0, ImageStride = 0, Start = 0, End = 0, Tight = 0;
        };

        Bool CheckedReadbackLayout(Uint64 width, Uint64 height, Uint64 depth, Uint64 bpp,
                                   const PixelStoreParameters& pack, Bool imagePack, ReadbackLayout& out) {
            constexpr Uint64 limit = std::numeric_limits<SizeT>::max();
            auto mul = [&](Uint64 a, Uint64 b, Uint64& result) {
                if (a && b > limit / a) return false;
                result = a * b; return true;
            };
            auto add = [&](Uint64 a, Uint64 b, Uint64& result) {
                if (a > limit || b > limit - a) return false;
                result = a + b; return true;
            };
            if (!width || !height || !depth || !bpp) return false;
            const Uint64 align = std::max<Int>(pack.Alignment, 1);
            Uint64 rawRow = 0, rounded = 0, skipRows = 0, skipPixels = 0, skipImages = 0;
            Uint64 lastImage = 0, lastRow = 0;
            if (!mul(width, bpp, out.RowBytes) ||
                !mul(pack.RowLength > 0 ? pack.RowLength : width, bpp, rawRow) ||
                !add(rawRow, align - 1, rounded) ||
                !mul(rounded / align, align, out.RowStride) ||
                !mul(imagePack && pack.ImageHeight > 0 ? pack.ImageHeight : height, out.RowStride, out.ImageStride) ||
                !mul(pack.SkipRows, out.RowStride, skipRows) || !mul(pack.SkipPixels, bpp, skipPixels) ||
                !mul(imagePack ? pack.SkipImages : 0, out.ImageStride, skipImages) ||
                !add(skipRows, skipPixels, out.Start) || !add(out.Start, skipImages, out.Start) ||
                !mul(depth - 1, out.ImageStride, lastImage) || !mul(height - 1, out.RowStride, lastRow) ||
                !add(out.Start, lastImage, out.End) || !add(out.End, lastRow, out.End) ||
                !add(out.End, out.RowBytes, out.End) || !mul(out.RowBytes, height, out.Tight) ||
                !mul(out.Tight, depth, out.Tight)) return false;
            return true;
        }

        void RecordReadbackRangeError(const char* caller) {
            MG_State::pGLContext->RecordError(ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Remote/Client", caller,
                    "Pixel packing exceeds the destination or its addressable range."));
        }

        void EmitGetTextureImage(const SharedPtr<MG_State::GLState::ITextureObject>& texture,
                                 TextureUploadTarget uploadTarget, GLint level, GLenum format,
                                 GLenum type, GLsizei bufSize, void* pixels) {
            if (!texture || texture->GetStorageType() != TextureStorageType::Mipmap) return;
            auto* mipmap = static_cast<MG_State::GLState::TextureObjectMipmap*>(texture.get());
            const Bool wholeCube = texture->GetTarget() == TextureTarget::TextureCubeMap &&
                                   uploadTarget == TextureUploadTarget::Unknown;
            const auto firstTarget = wholeCube ? TextureUploadTarget::CubeMapPositiveX : uploadTarget;
            const IntVec3 size = mipmap->GetMipmapTexelSize(firstTarget, level);
            if (size.x() <= 0 || size.y() <= 0 || size.z() <= 0) return;
            const Uint64 bpp = ReadbackBytesPerPixel(format, type);
            const Uint64 depth = wholeCube ? 6 : static_cast<Uint64>(size.z());
            const auto pack = MG_State::pGLContext->GetPixelStoreParameters(false);
            const auto target = texture->GetTarget();
            const Bool imagePack = wholeCube || target == TextureTarget::Texture3D ||
                target == TextureTarget::Texture2DArray || target == TextureTarget::TextureCubeMapArray;
            const auto pbo = MG_State::pGLContext->GetBufferBindingSlot(BufferTarget::PixelPack).GetBoundObject();
            const SizeT pboOffset = reinterpret_cast<SizeT>(pixels);
            ReadbackLayout layout;
            if (!CheckedReadbackLayout(size.x(), size.y(), depth, bpp, pack, imagePack, layout) ||
                (bufSize >= 0 && layout.End > static_cast<Uint64>(bufSize)) ||
                (pbo && (pboOffset > pbo->GetSize() || layout.End > pbo->GetSize() - pboOffset))) {
                RecordReadbackRangeError("GetTextureImage");
                return;
            }
            const Uint64 tight = layout.Tight;
            ClientSession& session = RequireSession("GetTextureImage");
            BeforeReadOnlyVerb();
            // A by-name texture need not occur in any currently bound sampler.
            MG_Pipe::MGPipeTextureEmitterInstance().DrainTextureSubData(*MG_State::pGLContext);
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
            ApplyReadbackByteSwap(bytes.data(), tight, type, pack);
            for (Uint64 z = 0; z < depth; ++z) {
                for (Int y = 0; y < size.y(); ++y) {
                    Uint8* source = bytes.data() + (z * size.y() + y) * layout.RowBytes;
                    const Uint64 offset = layout.Start + z * layout.ImageStride + y * layout.RowStride;
                    if (pbo) pbo->UploadSubData({source, static_cast<SizeT>(layout.RowBytes)}, pboOffset + offset);
                    else if (pixels) Memcpy(static_cast<Uint8*>(pixels) + offset, source, layout.RowBytes);
                }
            }
        }

        void EmitGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels) {
            const auto textureTarget = MG_Util::ConvertGLEnumToTextureTarget(target);
            auto& unit = MG_State::pGLContext->GetTextureUnitObject(MG_State::pGLContext->GetActiveTextureUnit());
            const auto& texture = unit.GetBindingSlot(textureTarget).GetBoundObject();
            EmitGetTextureImage(texture, MG_Util::ConvertGLEnumToTextureUploadTarget(target),
                                level, format, type, -1, pixels);
        }
