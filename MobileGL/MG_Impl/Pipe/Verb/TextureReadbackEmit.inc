// Included in VerbPort.cpp's anonymous namespace (P13 W5: moved from MG_Remote/Client).
// The server returns owned tight pixels. Only the client interprets PACK state
// and application pointers. A pack-buffer destination (P9) is answered by nothing: the
// client sends the layout it computed and the server lands the rows in the buffer; the
// reply form's "PBO rows are ordinary buffer uploads after the reply" remains the arm for
// a buffer PackBufferReadbackTarget refuses.
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

        // P9 (W1, MG_Remote/CONTRACT-P9.md §1): MAY A READ INTO THIS PACK BUFFER GO WITHOUT A
        // REPLY? The answer is the buffer's handle when it may - the read then crosses as
        // read_pixels_to_buffer / get_texture_image_to_buffer, lands server-side, and the buffer
        // is marked GPU-written - and the null handle when the reply-slot form has to carry it.
        // `end` is the last byte past the application's offset the read touches.
        //
        // Each refusal is a case the mark cannot keep its promise in, not a preference:
        //   - MOBILEGL_IPC_PBO_READBACK_SYNC=1, the A/B and red-once arm;
        //   - no readback route (BufferWritebackIsReachable: no size, or the resource family
        //     is not on the wire), so SyncGpuWrites would clear the mark with nothing fetched;
        //   - a MAPPED buffer. Only a persistent mapping can be live across a read, and its
        //     reader takes the bytes straight from the pointer through no call this side sees:
        //     no SyncGpuWrites would ever run for it, and the pixels would never arrive;
        //   - a range MGPSubData cannot name (a buffer offset stops at 2^31-1);
        //   - a buffer that never crossed: it has no handle for the server to write into, and
        //     the reply form's UploadSubData is what creates it.
        MG_Pipe::MGPipeHandle PackBufferReadbackTarget(const SharedPtr<MG_State::GLState::BufferObject>& pbo,
                                                       Uint64 end) {
            if (!pbo) return MG_Pipe::kMGPipeNullHandle;
#if MOBILEGL_BUILD_DISAGGREGATED
            // The transport's own fallback knob (MOBILEGL_IPC_PBO_READBACK_SYNC): the reply form.
            if (MG_Config::Ipc.PboReadbackSync != 0) return MG_Pipe::kMGPipeNullHandle;
#endif
            if (!BufferWritebackIsReachable(*pbo) || pbo->IsMapped()) return MG_Pipe::kMGPipeNullHandle;
            if (end > 0x7FFFFFFFull) return MG_Pipe::kMGPipeNullHandle;
            return MG_Pipe::MGPipeSlots().FindByLifetimeId(MG_Pipe::MGPipeKind::Buffer, pbo->GetLifetimeId());
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
            VerbChannel session = ChannelFor("GetTextureImage");
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
            // P9 (W1): into a pack buffer, ONE record and no wait. The whole level crosses at
            // once - there is no reply slot to fit, and the server reads the whole level for
            // every window of the reply form anyway - and the rows land in the buffer at the
            // layout computed above. The mark goes first, like every producer row's: the set
            // describes the work the record is about to start.
            if (pbo) {
                const MG_Pipe::MGPipeHandle target = PackBufferReadbackTarget(pbo, pboOffset + layout.End);
                if (!MG_Pipe::MGPipeHandleIsNull(target)) {
                    MarkReadPixelsPackBuffer();
                    MG_Pipe::MGPReadbackToBuffer record{};
                    record.Src = info;
                    record.Src.DstOffset = 0;
                    record.Src.DstSize = tight;
                    record.Dst = target;
                    record.DstBase = pboOffset + layout.Start;
                    record.RowStride = layout.RowStride;
                    record.ImageStride = layout.ImageStride;
                    record.SwapGroup = ReadbackSwapGroup(type, pack);
                    session.EmitAndWait(MG_Pipe::MGPWireOp::GetTextureImageToBuffer, &record,
                                        sizeof(record), nullptr, 0, nullptr, 0, nullptr);
                    return;
                }
            }
            Vector<Uint8> bytes(static_cast<SizeT>(tight));
            const Uint64 capacity = session.MaxReplyBytes();
            if (!capacity) MG_Pipe::MGPipeProtocolFatal("GetTextureImage.reply", "zero reply capacity");
            for (Uint64 offset = 0; offset < tight; ) {
                info.DstOffset = offset;
                info.DstSize = std::min(tight - offset, capacity);
                Int32 status = MG_Pipe::MGPipeReplySink::kStatusError;
                Uint64 received = 0;
                session.EmitAndWait(MG_Pipe::MGPWireOp::GetTextureImage, &info, sizeof(info), nullptr, 0,
                                    bytes.data() + offset, info.DstSize, &status, &received);
                if (!RequireReadbackReplyComplete(status, received, info.DstSize)) return;
                offset += info.DstSize;
            }
            ApplyReadbackByteSwap(bytes.data(), tight, type, pack);
            if (pbo) pbo->SyncGpuWrites();
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
