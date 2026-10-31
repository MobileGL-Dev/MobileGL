// MobileGL - MobileGL/MG_Backend/DirectGLES/ManagersTextureSync.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ManagersInternal.h"

namespace MobileGL::MG_Backend::DirectGLES {
    namespace TextureImpl {
        // --- Unpack-ring staging (see BufferImpl::UnpackRingAvailable) ---------------
        // One rectangular (or whole-level) region of a level shadow, repacked TIGHTLY
        // into the persistently-mapped unpack PBO. The upload that follows passes the
        // returned ring offset with UNPACK_ROW_LENGTH / UNPACK_IMAGE_HEIGHT left at the
        // surrounding ScopedDefaultUnpackState's 0, so the driver reads exactly the
        // bytes staged here - strictly fewer than the ROW_LENGTH-strided client-pointer
        // upload it replaces, which made the driver walk the whole level's stride.
        //
        // `blocks` describes one or more source regions to stage back-to-back into a
        // SINGLE ring allocation; the ring offset of block i lands in blocks[i].offset.
        // Deliberately without default member initializers: call sites declare a
        // kMaxDirtyRects-sized array of these per dirty level and fill only the entries
        // they use, so the type stays trivially default-constructible and the
        // declaration costs nothing on the levels that never reach the ring.
        struct UnpackStagingBlock {
            const Uint8* src;     // top-left texel of the region in the level's upload bytes
            SizeT rowBytes;       // bytes per region row (region width * bpp)
            SizeT rows;           // region height
            SizeT slices;         // region depth (1 for 2D)
            SizeT srcRowStride;   // level row pitch
            SizeT srcSliceStride; // level slice pitch
            SizeT offset;         // out: byte offset into the ring store
        };

        // False (nothing staged, ring untouched) whenever the caller must keep the
        // client-pointer path: ring unavailable, a degenerate block, or a total that
        // overflows / exceeds the ring's cap.
        static Bool StageBlocksIntoUnpackRing(UnpackStagingBlock* blocks, SizeT blockCount) {
            if (blocks == nullptr || blockCount == 0) return false;
            if (!BufferImpl::UnpackRingAvailable()) return false;

            const SizeT maxBytes = BufferImpl::UnpackRingMaxBytes();
            SizeT total = 0;
            for (SizeT i = 0; i < blockCount; ++i) {
                const UnpackStagingBlock& b = blocks[i];
                if (b.src == nullptr || b.rowBytes == 0 || b.rows == 0 || b.slices == 0) return false;
                // Every factor is bounded by the level's own byte size, but the
                // multiplications are on SizeT: check each against the cap instead of
                // trusting a product that could have wrapped.
                if (b.rowBytes > maxBytes || b.rows > maxBytes / b.rowBytes) return false;
                const SizeT sliceBytes = b.rowBytes * b.rows;
                if (b.slices > maxBytes / sliceBytes) return false;
                const SizeT blockBytes = sliceBytes * b.slices;
                if (blockBytes > maxBytes - total) return false;
                total += blockBytes;
            }

            SizeT base = 0;
            if (!BufferImpl::UnpackRingAllocate(total, base)) return false;
            // AFTER the allocation: growing the ring replaces the store and its map.
            auto* ringBytes = static_cast<Uint8*>(BufferImpl::UnpackRingMappedPtr());
            if (ringBytes == nullptr) return false;

            SizeT cursor = base;
            for (SizeT i = 0; i < blockCount; ++i) {
                UnpackStagingBlock& b = blocks[i];
                b.offset = cursor;
                Uint8* dst = ringBytes + cursor;
                // Each block's size is a whole number of texels, so consecutive block
                // offsets stay multiples of the pixel type's size just as `base` is.
                if (b.rowBytes == b.srcRowStride && b.rowBytes * b.rows == b.srcSliceStride) {
                    Memcpy(dst, b.src, b.rowBytes * b.rows * b.slices); // region IS the level
                } else {
                    for (SizeT z = 0; z < b.slices; ++z) {
                        const Uint8* srcSlice = b.src + z * b.srcSliceStride;
                        if (b.rowBytes == b.srcRowStride) {
                            Memcpy(dst, srcSlice, b.rowBytes * b.rows); // full-width rows
                            dst += b.rowBytes * b.rows;
                            continue;
                        }
                        for (SizeT y = 0; y < b.rows; ++y) {
                            Memcpy(dst, srcSlice + y * b.srcRowStride, b.rowBytes);
                            dst += b.rowBytes;
                        }
                    }
                }
                cursor += b.rowBytes * b.rows * b.slices;
            }
            return true;
        }

        // The `pixels` argument of a PBO-sourced glTexSubImage: a byte offset dressed
        // up as a pointer.
        static const void* UnpackRingPixelOffset(SizeT offset) {
            return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
        }

        static Uint GetNormFallbackComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
                return 1;
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
                return 2;
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB10: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB12: // stored as RGB16 (UNorm16 shadow)
            case TextureInternalFormat::RGB16Snorm:
                return 3;
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA12: // stored as RGBA16 (UNorm16 shadow)
            case TextureInternalFormat::RGBA16Snorm:
                return 4;
            default:
                return 0;
            }
        }

        static Bool IsSnormFallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA16Snorm:
                return true;
            default:
                return false;
            }
        }

        static Bool IsNorm8FallbackFormat(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGBA8Snorm:
                return true;
            default:
                return false;
            }
        }

        // The bit pattern of 1.0 in an upload component type: what a format without alpha reads
        // back as, and therefore what the synthetic fourth channel of a widened render target has
        // to hold. Integer components carry the integer one, not a saturated field - and since
        // GL_BYTE/GL_SHORT/GL_UNSIGNED_BYTE/GL_UNSIGNED_SHORT serve both classes, `integerData`
        // is what decides, not the type.
        static Bool GetUploadComponentOneBits(GLenum uploadType, Bool integerData, Uint8* outOneBits,
                                              SizeT* outComponentSize) {
            switch (uploadType) {
            case GL_BYTE: {
                const Int8 one = integerData ? Int8(1) : Int8(0x7F);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_BYTE: {
                const Uint8 one = integerData ? Uint8(1) : Uint8(0xFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_SHORT: {
                const Int16 one = integerData ? Int16(1) : Int16(0x7FFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_SHORT: {
                const Uint16 one = integerData ? Uint16(1) : Uint16(0xFFFF);
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_HALF_FLOAT: {
                const Uint16 one = 0x3C00; // half 1.0
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_FLOAT: {
                const Float one = 1.0f;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_INT: {
                const Int32 one = 1;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            case GL_UNSIGNED_INT: {
                const Uint32 one = 1;
                Memcpy(outOneBits, &one, sizeof(one));
                *outComponentSize = sizeof(one);
                return true;
            }
            default:
                return false;
            }
        }

        // A three-channel format widened to four to keep a color attachment renderable (see
        // NormalizePixelFormat) is described to the driver as a four-component transfer, so the
        // three-component client data has to be repacked with an alpha of 1.0 - otherwise the
        // driver walks three texels' worth of data per four-texel row and the image shears.
        // `componentCount` is the SOURCE component count and `byteSize` the source's size, so this
        // runs after any type conversion (which keeps the component count) has already happened.
        const void* PrepareChannelWidenedUpload(Uint componentCount, const IntVec3& texelSize, const void* data,
                                                SizeT byteSize, GLenum uploadType, Vector<Uint8>& widenedData,
                                                Bool integerData, Uint32 alphaOneCodeOverride) {
            Uint8 oneBits[8] = {};
            SizeT componentSize = 0;
            // One and two source components as well as three: the image-format widening carries
            // GL_R8UI in a GL_RGBA8UI and GL_RG32F in a GL_RGBA32F (see
            // TextureImpl::GetImageBindableStorageWidening), and their surplus channels take the
            // same values the three-channel case gives its single added one - zeroes, and the
            // format's implied 1 in alpha.
            if (componentCount == 0 || componentCount > 3 || data == nullptr || byteSize == 0 ||
                !GetUploadComponentOneBits(uploadType, integerData, oneBits, &componentSize)) {
                return data;
            }
            // ...except where the carrier holds CODES of a normalized value (GL_R16 in a
            // GL_RGBA16UI), where the transfer type says GL_UNSIGNED_SHORT and neither of that
            // type's two "ones" is right: the integer 1 is a code for 1/65535 and the saturated
            // 0xFFFF is only right for the UNSIGNED 16-bit formats, not the signed ones, whose
            // saturated code is 0x7FFF. The caller passes the channel's own maximum instead.
            // Written through a value of the component's own width rather than as the low
            // `componentSize` bytes of the Uint32, so the encoding does not turn on the host's
            // byte order.
            if (alphaOneCodeOverride != 0u) {
                if (componentSize == sizeof(Uint16)) {
                    const auto one = static_cast<Uint16>(alphaOneCodeOverride);
                    Memcpy(oneBits, &one, sizeof(one));
                } else if (componentSize == sizeof(Uint32)) {
                    Memcpy(oneBits, &alphaOneCodeOverride, sizeof(alphaOneCodeOverride));
                } else if (componentSize == sizeof(Uint8)) {
                    const auto one = static_cast<Uint8>(alphaOneCodeOverride);
                    Memcpy(oneBits, &one, sizeof(one));
                }
            }

            const SizeT srcTexelBytes = componentSize * componentCount;
            // Sized from the level, never from the source: the driver reads a full
            // width*height*depth*4 components for the transfer it was handed, so a source that
            // somehow holds fewer texels must still leave a full destination behind (its tail
            // reads as transparent black with the format's implied opaque alpha) rather than a
            // short buffer the driver would run off the end of.
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / srcTexelBytes);

            widenedData.assign(texelCount * componentSize * 4, 0);
            const auto* src = static_cast<const Uint8*>(data);
            Uint8* dst = widenedData.data();
            for (SizeT i = 0; i < texelCount; ++i, dst += componentSize * 4) {
                if (i < copyTexelCount) {
                    Memcpy(dst, src, srcTexelBytes);
                    src += srcTexelBytes;
                }
                // ALWAYS at component 3, never at `componentCount`: GL's implied 1 is the ALPHA
                // channel, and a one- or two-component source leaves the channels between it and
                // alpha at the zero `assign` already wrote. For three components the two
                // expressions coincide, which is what this used to be written as.
                Memcpy(dst + componentSize * 3, oneBits, componentSize);
            }
            return widenedData.data();
        }

        static const void* PrepareNormFloatFallbackUpload(TextureInternalFormat format, const IntVec3& texelSize,
                                                          const void* data, SizeT byteSize, GLenum uploadType,
                                                          Vector<Float>& convertedData) {
            const Uint componentCount = GetNormFallbackComponentCount(format);
            if (componentCount == 0 || uploadType != GL_FLOAT || data == nullptr || byteSize == 0) {
                return data;
            }

            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 0));
            const SizeT componentTotal = texelCount * static_cast<SizeT>(componentCount);
            const SizeT sourceComponentSize = IsNorm8FallbackFormat(format) ? sizeof(Int8) : sizeof(Uint16);
            const SizeT sourceComponentTotal = byteSize / sourceComponentSize;
            if (componentTotal == 0 || sourceComponentTotal == 0) {
                return nullptr;
            }

            convertedData.assign(componentTotal, 0.0f);
            const SizeT copyComponentTotal = std::min(componentTotal, sourceComponentTotal);
            if (IsNorm8FallbackFormat(format)) {
                const Int8* src = static_cast<const Int8*>(data);
                constexpr Float invMaxSnorm8 = 1.0f / 127.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm8, -1.0f);
                }
            } else if (IsSnormFallbackFormat(format)) {
                const Int16* src = static_cast<const Int16*>(data);
                constexpr Float invMaxSnorm16 = 1.0f / 32767.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = std::max(static_cast<Float>(src[i]) * invMaxSnorm16, -1.0f);
                }
            } else {
                const Uint16* src = static_cast<const Uint16*>(data);
                constexpr Float invMaxUnorm16 = 1.0f / 65535.0f;
                for (SizeT i = 0; i < copyComponentTotal; ++i) {
                    convertedData[i] = static_cast<Float>(src[i]) * invMaxUnorm16;
                }
            }
            return convertedData.data();
        }

        // The two shadow -> upload conversions a fallback storage format can need, in order:
        // the component type first (SNORM/UNORM shadows into the float the fallback stores), then
        // the component count (three-channel client data into a four-channel widened render
        // target). They compose: GL_RGB8_SNORM on a driver with no renderable three-channel
        // format becomes GL_RGBA16F, so its Int8x3 shadow is converted to Float x3 and then
        // repacked as Float x4 with alpha 1.0.
        //
        // Both scratch buffers belong to the caller so they outlive the returned pointer; the
        // return value is `data` itself whenever neither conversion applies, which is what the
        // sub-rect upload fast path tests for.
        static const void* PrepareFallbackUpload(TextureInternalFormat format, TextureTarget target,
                                                 const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                 GLenum uploadType, Vector<Float>& convertedData,
                                                 Vector<Uint8>& widenedData) {
            const void* uploadData =
                PrepareNormFloatFallbackUpload(format, texelSize, data, byteSize, uploadType, convertedData);
            // The component-count switch first: it rules out every format that cannot be widened
            // (which is nearly all of them, including GL_RGBA8) without touching the capability
            // cache, so an ordinary atlas upload does not pay for a per-level cache lookup.
            const Uint componentCount = GetWidenableClientComponentCount(format);
            if (componentCount == 0 || !TextureImpl::BackendTextureFormatAddsAlpha(format, target)) {
                return uploadData;
            }
            // The type conversion above rewrites the level into `convertedData` at four bytes per
            // component while keeping the component count, so the widening's source size is that
            // buffer's, not the shadow's.
            const SizeT uploadByteSize = (!convertedData.empty() && uploadData == convertedData.data())
                                             ? convertedData.size() * sizeof(Float)
                                             : byteSize;
            return PrepareChannelWidenedUpload(componentCount, texelSize, uploadData, uploadByteSize, uploadType,
                                               widenedData, IsIntegerWidenableFormat(format));
        }

        // One channel of a packed r11f_g11f_b10f word as a float. The two 11-bit channels are
        // e5m6 and the 10-bit one e5m5 - IEEE-shaped but UNSIGNED, so there is no sign bit to
        // read and the exponent bias is the 15 a 5-bit exponent always carries.
        static Float DecodePackedUnsignedFloat(Uint32 bits, Uint mantissaBits) {
            const Uint32 mantissaScale = 1u << mantissaBits;
            const Uint32 mantissa = bits & (mantissaScale - 1u);
            const Uint32 exponent = bits >> mantissaBits;
            if (exponent == 0u) {
                // Subnormal, and zero with it: no implied leading 1, and the exponent is the
                // smallest NORMAL one rather than the encoded 0.
                return std::ldexp(static_cast<Float>(mantissa) / static_cast<Float>(mantissaScale), -14);
            }
            if (exponent == 31u) {
                return mantissa == 0u ? std::numeric_limits<Float>::infinity()
                                      : std::numeric_limits<Float>::quiet_NaN();
            }
            return std::ldexp(1.0f + static_cast<Float>(mantissa) / static_cast<Float>(mantissaScale),
                              static_cast<Int>(exponent) - 15);
        }

        // The r11f_g11f_b10f shadow decoded into the GL_RGBA / GL_FLOAT level its GL_RGBA16F
        // carrier is uploaded as. Alpha is the 1 GL defines for a format that has no alpha
        // channel, which is the same constant the shader-side mask writes, so a texel this
        // function produced and a texel an imageStore produced are indistinguishable.
        //
        // Sized from the LEVEL, not the source, for the reason PrepareChannelWidenedUpload is:
        // the driver reads a full width*height*depth*4 floats for the transfer it was handed.
        static const void* PreparePackedFloatWidenedUpload(const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                           Vector<Uint8>& widenedData) {
            constexpr SizeT kSourceTexelBytes = sizeof(Uint32);
            if (data == nullptr || byteSize < kSourceTexelBytes) {
                return data;
            }
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / kSourceTexelBytes);

            widenedData.assign(texelCount * 4u * sizeof(Float), 0);
            const auto* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Float*>(widenedData.data());
            for (SizeT i = 0; i < texelCount; ++i, dst += 4) {
                Float rgb[3] = {0.0f, 0.0f, 0.0f};
                if (i < copyTexelCount) {
                    Uint32 packed = 0;
                    // Through a memcpy rather than a Uint32 read of `src`: the shadow is a byte
                    // buffer with no alignment promise of its own.
                    Memcpy(&packed, src + i * kSourceTexelBytes, sizeof(packed));
                    rgb[0] = DecodePackedUnsignedFloat(packed & 0x7FFu, 6u);
                    rgb[1] = DecodePackedUnsignedFloat((packed >> 11u) & 0x7FFu, 6u);
                    rgb[2] = DecodePackedUnsignedFloat((packed >> 22u) & 0x3FFu, 5u);
                }
                dst[0] = rgb[0];
                dst[1] = rgb[1];
                dst[2] = rgb[2];
                dst[3] = 1.0f;
            }
            return widenedData.data();
        }

        // The rgb10_a2 / rgb10_a2ui shadow split into the four GL_UNSIGNED_SHORT channel CODES its
        // GL_RGBA16UI carrier is uploaded as. GL_UNSIGNED_INT_2_10_10_10_REV puts the FIRST
        // component in the LOW bits (that is what REV means), so red is bits 0-9, green 10-19,
        // blue 20-29 and alpha 30-31.
        //
        // The same split serves both formats: an rgb10_a2ui channel's code IS its value, and an
        // rgb10_a2 channel's code is the numerator of value = code / (2^b - 1) that the shader-side
        // unpack divides out. Neither is scaled here - the carrier holds the format's own bits.
        //
        // Sized from the LEVEL, not the source, for the reason PrepareChannelWidenedUpload is: the
        // driver reads a full width*height*depth*4 shorts for the transfer it was handed.
        const void* PreparePackedIntWidenedUpload(const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                  Vector<Uint8>& widenedData) {
            constexpr SizeT kSourceTexelBytes = sizeof(Uint32);
            if (data == nullptr || byteSize < kSourceTexelBytes) {
                return data;
            }
            const SizeT texelCount = static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
            if (texelCount == 0) {
                return data;
            }
            const SizeT copyTexelCount = std::min(texelCount, byteSize / kSourceTexelBytes);

            widenedData.assign(texelCount * 4u * sizeof(Uint16), 0);
            const auto* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Uint16*>(widenedData.data());
            for (SizeT i = 0; i < texelCount; ++i, dst += 4) {
                Uint32 packed = 0;
                if (i < copyTexelCount) {
                    // Through a memcpy rather than a Uint32 read of `src`: the shadow is a byte
                    // buffer with no alignment promise of its own.
                    Memcpy(&packed, src + i * kSourceTexelBytes, sizeof(packed));
                }
                dst[0] = static_cast<Uint16>(packed & 0x3FFu);
                dst[1] = static_cast<Uint16>((packed >> 10u) & 0x3FFu);
                dst[2] = static_cast<Uint16>((packed >> 20u) & 0x3FFu);
                dst[3] = static_cast<Uint16>((packed >> 30u) & 0x3u);
            }
            return widenedData.data();
        }

        // The transfer half of the image-format widening: an image-bindable texture whose ES
        // storage was widened to a core carrier is described to the driver as a four-component
        // transfer, so its narrower client data has to be repacked the same way the three-channel
        // color-renderable widening repacks its own.
        //
        // Three shapes, because the carriers come in three kinds. Most of them keep the frontend
        // format's component TYPE and only add channels, so padding the shadow out to four
        // components is the whole conversion. The two PACKED formats do not: their shadow is one
        // 32-bit word per texel, so the word has to be split - into four floats for
        // r11f_g11f_b10f's GL_RGBA16F, into four shorts for rgb10_a2ui's GL_RGBA16UI. Reading such
        // a word as components of the carrier's type - what the repack below would do - takes
        // twelve or sixteen bytes from a four-byte texel and shears the level, which is what the
        // allFormats LOAD walkers see and the STORE ones do not (a store overwrites every texel
        // the upload got wrong).
        //
        // Composes with PrepareFallbackUpload rather than replacing it, and the composition is a
        // no-op by construction: none of the widened formats is one GetWidenableClientComponentCount
        // reports a count for, and the SNORM shadow-to-float conversion only fires for a GL_FLOAT
        // transfer type, which the widened triple never picks for the two SNORM8 formats. So the
        // shadow reaches this untouched and one conversion is all that runs.
        static const void* PrepareImageWidenedUpload(const TextureImpl::ImageBindableStorageWidening& widening,
                                                     const IntVec3& texelSize, const void* data, SizeT byteSize,
                                                     Vector<Uint8>& widenedData) {
            if (!widening || widening.SourceChannels == 0 || widening.SourceChannels > 4) {
                return data;
            }
            switch (widening.SourceEncoding) {
            case TextureImpl::ImageWidenSourceEncoding::PackedFloat11f11f10f:
                return PreparePackedFloatWidenedUpload(texelSize, data, byteSize, widenedData);
            case TextureImpl::ImageWidenSourceEncoding::PackedInt2101010Rev:
                return PreparePackedIntWidenedUpload(texelSize, data, byteSize, widenedData);
            case TextureImpl::ImageWidenSourceEncoding::Components:
                break;
            }
            if (widening.SourceChannels == 4) {
                return data;
            }
            return PrepareChannelWidenedUpload(widening.SourceChannels, texelSize, data, byteSize, widening.Type,
                                               widenedData, widening.IntegerData,
                                               widening.CarriesNormalizedCodes() ? widening.ChannelMax[3] : 0u);
        }

        // Overwrites the (internal format, format, type) triple GenerateTextureFormatInfo chose
        // with the widened carrier's. Deliberately unconditional on anything but the widening
        // itself: whatever renderability fallback the triple carried, an image the driver refuses
        // to bind is useless, so the image constraint wins.
        static void ApplyImageBindableStorageWidening(const TextureImpl::ImageBindableStorageWidening& widening,
                                                      GLenum* inOutInternalFormat, GLenum* inOutFormat,
                                                      GLenum* inOutType) {
            if (!widening) {
                return;
            }
            if (inOutInternalFormat) *inOutInternalFormat = widening.InternalFormat;
            if (inOutFormat) *inOutFormat = widening.Format;
            if (inOutType) *inOutType = widening.Type;
        }

        // RGB565/RGB5_A1 shadow data is stored as 8-bit unorm; uploading it as GL_UNSIGNED_BYTE
        // leaves the 8-bit -> 5/6-bit requantization to the driver, whose rounding direction is
        // implementation-defined: Adreno rounds to nearest (lossless round trip) but Mali floors,
        // drifting mid-range texels one 5-bit step down and failing the KHR-GL3x
        // pixelstoragemodes.teximage3d rgb565/rgb5a1 1/32-eps checks. Repack the shadow rows into
        // the packed 16-bit client type with round-to-nearest instead - that recovers the original
        // 5/6-bit values exactly (the shadow expansion round(v * 255 / max) is injective), so the
        // driver stores them verbatim with no requantization left to its discretion. 4-bit formats
        // (RGBA4) are exempt: their 8-bit expansion (v * 17) is exact under either rounding.
        // Always retargets *inOutType for these formats (even for null data) so every upload of a
        // level uses the same client type.
        static const void* PreparePackedNormUpload(TextureInternalFormat format, const IntVec3& texelSize,
                                                   const void* data, SizeT byteSize, GLenum* inOutType,
                                                   Vector<Uint8>& packedData) {
            if (format != TextureInternalFormat::RGB5 && format != TextureInternalFormat::RGB5A1) {
                return data;
            }
            // With the storage widened to 8-bit-per-channel (the packed16 field-order quirk)
            // there is no driver requantization left for the repack to pre-empt - the shadow's
            // UNorm8 bytes ARE the stored bytes - and the packed 16-bit client type this leg
            // retargets to is not a legal upload for a GL_RGB8/GL_RGBA8 store at all.
            if (TextureImpl::UsesWidenedPacked16NormStorage(format)) {
                return data;
            }
            const Bool hasAlpha = format == TextureInternalFormat::RGB5A1;
            const GLenum packedType = hasAlpha ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
            // Idempotent across a region's level loop: glType is shared, so later levels arrive with
            // the already-retargeted packed type and must still be converted.
            if (*inOutType != GL_UNSIGNED_BYTE && *inOutType != packedType) {
                return data;
            }
            *inOutType = packedType;
            if (data == nullptr || byteSize == 0) {
                return data;
            }
            const SizeT srcPixelBytes = hasAlpha ? 4 : 3;
            const SizeT texelCount = std::min(static_cast<SizeT>(std::max(texelSize.x(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.y(), 0)) *
                                                  static_cast<SizeT>(std::max(texelSize.z(), 1)),
                                              byteSize / srcPixelBytes);
            packedData.resize(texelCount * sizeof(Uint16));
            const Uint8* src = static_cast<const Uint8*>(data);
            auto* dst = reinterpret_cast<Uint16*>(packedData.data());
            for (SizeT i = 0; i < texelCount; ++i, src += srcPixelBytes) {
                const Uint32 r = (static_cast<Uint32>(src[0]) * 31u + 127u) / 255u;
                const Uint32 b = (static_cast<Uint32>(src[2]) * 31u + 127u) / 255u;
                if (hasAlpha) {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 31u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 6) | (b << 1) | (src[3] >= 128 ? 1u : 0u));
                } else {
                    const Uint32 g = (static_cast<Uint32>(src[1]) * 63u + 127u) / 255u;
                    dst[i] = static_cast<Uint16>((r << 11) | (g << 5) | b);
                }
            }
            return packedData.data();
        }

        // "Some level of this texture holds an image", which is all the sync gate below actually
        // needs to know. Deliberately weaker than ITextureObject::IsComplete(): that predicate also
        // answers whether the texture SAMPLES as complete, so it must keep rejecting a chain with
        // undefined lower levels - but such a texture still has to be uploaded, or the level that
        // IS defined never reaches the driver at all.
        static Bool HasAnyDefinedMipmapLevel(const MG_State::GLState::ITextureObject* stateTextureObject) {
            const auto* mipmapObject = MG_State::GLState::AsMipmapTexture(stateTextureObject);
            if (mipmapObject == nullptr) return false;
            const auto levelCount = mipmapObject->GetMipmapLevelCount();
            for (const auto& uploadTarget : stateTextureObject->GetUploadTargets()) {
                for (Uint level = 0; level < levelCount; ++level) {
                    const auto levelTexelSize = mipmapObject->GetMipmapTexelSize(uploadTarget, level);
                    if (levelTexelSize.x() > 0 && levelTexelSize.y() > 0 && levelTexelSize.z() > 0) {
                        return true;
                    }
                }
            }
            return false;
        }
// True while this twin has NO noted handle, i.e. it is the monolith-glue half and the frontend
// object below is the only thing that can answer. A macro and not a member function because the
// pull build has no such member at all and this has to fold to a constant there (D-P: the pull
// build's preprocessed text may not gain a call).
#define MGB_TEXTURE_HANDLE_ARM_OFF(twin) (MG_Pipe::MGPipeHandleIsNull((twin).PushedSyncHandle()))
// The ~20 GetExternalIndex() reads this family makes in LOG LINES (scout G-S2-4). The pull
// expansion is the pre-P5e expression with one pair of parentheses around the receiver.
#define MGB_TEXTURE_DIAG_NAME(rec, obj) (TextureDiagName((obj), (rec)))
// The storage KIND, from the descriptor whenever one was resolved - the backstop arm of the
// storage switch used to ask the frontend object for a value it had just read from the record.
#define MGB_TEXTURE_STORAGE_KIND(rec, obj)                                                                             \
    ((rec) != nullptr ? static_cast<TextureStorageType>((rec)->Desc.StorageKind) : (obj)->GetStorageType())

        // P4a (D-D5), the dirty-ownership inversion, applied at the two places the upload loops
        // below actually ask: "does this (uploadTarget, level) owe an upload" and "it has been
        // uploaded". ARCHITECTURE.md §6 puts the rect model and the emission cursor on the
        // CLIENT, which clears its own flags AT EMISSION - so on the handle arm the frontend's
        // IsStorageDirty is already false for every level the applier accepted, and asking it
        // would upload nothing at all. The applier's pending set is what these two read instead;
        // it is server-side state, it survives this function's bail arms (an incomplete texture
        // returns early, a multisample target refreshes and skips), and an entry is consumed
        // ONLY where the level actually uploaded.
        //
        // THE UPLOAD-TARGET ENCODING is static_cast<Uint16>(TextureUploadTarget), which is what
        // package B must emit into MGPSubData::Target's cube-face half; it is recorded as a
        // deviation because the contract left it unstated.
        //
        // MACROS AND NOT LAMBDAS, and that is a G1 requirement rather than a style choice:
        // SyncMipmapsToBackend already holds nine ErrorLopper lambdas whose mangled names are
        // ...::$_N BY POSITION, so inserting two more renumbered every one of them - measured as
        // eight symbols added, eight removed and four std::function thunks resized in the PULL
        // build, whose symbol set P4a may not move by one byte (D-P). A macro expands to the
        // pre-P4a expression exactly when MOBILEGL_PIPE_PUSH is off, so the pull build's
        // preprocessed text, and therefore its object code, is unchanged. Both are #undef'd
        // immediately after the function.
// P5c (tx): THE READS THE FOUR UPLOAD ARMS MAKE, re-sourced. With an active transport the
// apply thread may not name the client's TextureObjectMipmap at all (rule E), so on the
// handle arm:
//
//   * the level TEXELS come from the server's staged-texture store, adopted at apply time
//     (MGB_LEVEL_TEXELS - Fatal{StageSnapshotTooNarrow} when no record covered the level,
//     which is the data-correctness refusal of the texture half);
//   * the per-level EXTENT and DEFINED-NESS come from the same store (fed by the sub-data
//     adoption and the respecify hook; {0,0,0} for a level nothing defined, which is exactly
//     GetMipmapTexelSize's answer for one);
//   * the level BYTE SIZE is the adopted run's length;
//   * the texture TARGET and the UPLOAD-TARGET list come from the descriptor.
//
// Every macro keeps the P4a discipline: the non-disaggregated expansion is the original
// frontend read, character for character modulo one pair of parentheses, so the pull and
// push builds compile exactly what they compiled before tx, and every disaggregated arm
// falls back to the frontend read when there is no active transport (the legacy arm, and
// monolith). MGB_STAGED_TEXTURE_LIVE is the runtime discriminator; pushedStorage/pushedRes
// are the function's own locals. All are #undef'd with the rest after the function.
#define MGB_STAGED_TEXTURE_LIVE (pushedStorage != nullptr && MG_Record::ServerStagedTexture().Holds())
#define MGB_TEXTURE_TARGET(obj)                                                                                        \
    (MGB_STAGED_TEXTURE_LIVE ? BufferImpl::StagedTextureTargetForPipeTarget(pushedStorage->Desc.Target)                \
                             : (obj)->GetTarget())
#define MGB_UPLOAD_TARGETS(obj)                                                                                        \
    (MGB_STAGED_TEXTURE_LIVE ? BufferImpl::StagedUploadTargetsForPipeTarget(pushedStorage->Desc.Target)                \
                             : (obj)->GetUploadTargets())
#define MGB_LEVEL_TEXEL_SIZE(obj, tgt, lvl)                                                                            \
    (MGB_STAGED_TEXTURE_LIVE ? MG_Record::ServerStagedTexture().LevelExtentOrUndefined(                                \
                                   MG_Record::StagedTextureStore::KeyForHandle(pushedRes), static_cast<Uint16>(tgt),   \
                                   static_cast<Uint16>(lvl))                                                           \
                             : (obj)->GetMipmapTexelSize(tgt, lvl))
#define MGB_LEVEL_BYTE_SIZE(obj, tgt, lvl)                                                                             \
    (MGB_STAGED_TEXTURE_LIVE                                                                                           \
         ? MG_Record::ServerStagedTexture().LevelByteSize(MG_Record::StagedTextureStore::KeyForHandle(pushedRes),      \
                                                          static_cast<Uint16>(tgt), static_cast<Uint16>(lvl))          \
         : (obj)->GetMipmapByteSize(tgt, lvl))
#define MGB_LEVEL_TEXELS(obj, tgt, lvl, site)                                                                          \
    (MGB_STAGED_TEXTURE_LIVE ? MG_Record::ServerStagedTexture().RequireLevelBytes(                                     \
                                   MG_Record::StagedTextureStore::KeyForHandle(pushedRes), static_cast<Uint16>(tgt),   \
                                   static_cast<Uint16>(lvl), site)                                                     \
                             : (obj)->MapMipmapData(tgt, lvl))

// P5c (tx): the disaggregated pair adds ONE term and ONE clear to the P4a shapes - the
// staged-texture store's GPU-dirty mark (T5: a level the GPU generated dirties the SERVER's
// shadow, and the pending set cannot see it). The mark is never set on Espryt today - its
// GenerateMipmap fills the levels on the driver - so the term is inert here and is the
// contract-shaped answer (§2.2's last row) rather than a hot-path cost: one m_any acquire
// load when the store is empty.
#define MGB_LEVEL_NEEDS_UPLOAD(obj, tgt, lvl)                                                                          \
    (pushedStorage != nullptr                                                                                          \
         ? (FindPipeTextureUpload(*pushedStorage, static_cast<Uint16>(tgt), static_cast<Uint16>(lvl)) != nullptr ||    \
            MG_Record::ServerStagedTexture().IsLevelGpuDirty(MG_Record::StagedTextureStore::KeyForHandle(pushedRes),   \
                                                             static_cast<Uint16>(tgt), static_cast<Uint16>(lvl)))      \
         : (obj)->IsStorageDirty(tgt, lvl))
// Re-resolves the record itself, so it is safe after any amount of driver work - and it
// invalidates any PendingUpload* taken earlier for THIS texture, which is why every such pointer
// is used and dropped inside one level's iteration.
#define MGB_LEVEL_UPLOAD_DONE(obj, tgt, lvl)                                                                           \
    do {                                                                                                               \
        if (pushedStorage != nullptr) {                                                                                \
            ConsumePipeTextureUpload(pushedRes, static_cast<Uint16>(tgt), static_cast<Uint16>(lvl));                   \
            if (MGB_STAGED_TEXTURE_LIVE) {                                                                             \
                MG_Record::ServerStagedTexture().MarkLevelGpuDirty(                                                    \
                    MG_Record::StagedTextureStore::KeyForHandle(pushedRes), static_cast<Uint16>(tgt),                  \
                    static_cast<Uint16>(lvl), false);                                                                  \
            }                                                                                                          \
        } else {                                                                                                       \
            (obj)->MarkStorageDirty(tgt, lvl, false);                                                                  \
        }                                                                                                              \
    } while (0)

        // THE STORAGE SHAPE, and on the handle arm it is the DESCRIPTOR's (C.3's d2: "the
        // m_prevTextureInfo probe re-keyed onto the resource record's Serial AND Desc"; review
        // M-2). v1 moved the parameters and the uploads and left the allocation reading the
        // frontend object, which put TWO AUTHORITIES on one value inside one twin -
        // SyncTextureParamsToBackend already answers MGB_TEXPARAM_FORMAT from
        // Desc.InternalFormat while this function allocated from GetFormat(). In monolith they
        // agree, so no lane can expose the split; under split the descriptor is all there is.
        //
        // WHAT THE DESCRIPTOR ANSWERS: internal format, base extent, level count, sample count,
        // fixed sample locations, immutability, storage kind and the buffer-texture window. What
        // it does NOT answer, and what still comes from the frontend shadow, is the PER-LEVEL
        // extent and the level BYTES - MGPResourceDesc carries one base extent, the per-level
        // sizes are derived from it, and the texels themselves ride on sub-data. A level the
        // descriptor's Levels claims and the shadow has not defined reads back {0,0,0} and the
        // per-level loops skip it, which is the same arm they take for a sparse chain today.
        //
        // AND Desc.HasDefinedContent IS DELIBERATELY NOT READ HERE (review N-8, and the
        // enumeration above exists to be exhaustive, so the omission is stated rather than
        // left to be noticed). c0e made it storage-defining and the BUFFER family reads it
        // three times, because a buffer's content is ONE fact about the whole resource. A
        // texture's is not: content is per (uploadTarget, level) and its authority is the
        // pending set (D-D5), which is per level and survives a bail. A whole-resource
        // "content is undefined" would say nothing the per-level loops do not already answer
        // by finding no level to upload, and acting on it would mean throwing away levels the
        // set still owes. The field stays a respecify CLASSIFIER (it moves the descriptor, so
        // wire refuses to call such a respecify metadata-only) and nothing more on this arm.
        //
        // MACROS, NOT LOCALS, FOR THE SAME REASON AS THE PAIR ABOVE (DV-9, and it was measured):
        // routing these through locals resized SyncTextureParamsToBackend by +9 bytes in the
        // PULL build and P4a's admitted-resize set is EMPTY. Each takes the object it would
        // otherwise have been spelled on, so the pull expansion is the pre-P4a expression with
        // one pair of parentheses around the receiver. All are #undef'd with the pair above.
#define MGB_STORAGE_FORMAT(obj)                                                                                        \
    (pushedStorage != nullptr ? static_cast<TextureInternalFormat>(pushedStorage->Desc.InternalFormat)                 \
                              : (obj)->GetFormat())
#define MGB_STORAGE_BASE_SIZE(obj)                                                                                     \
    (pushedStorage != nullptr                                                                                          \
         ? IntVec3{static_cast<Int>(pushedStorage->Desc.Width), static_cast<Int>(pushedStorage->Desc.Height),          \
                   static_cast<Int>(pushedStorage->Desc.Depth)}                                                        \
         : (obj)->GetBaseSize())
#define MGB_STORAGE_LEVELS(obj)                                                                                        \
    (pushedStorage != nullptr ? static_cast<Uint>(pushedStorage->Desc.Levels) : (obj)->GetMipmapLevelCount())
#define MGB_STORAGE_SAMPLES(obj)                                                                                       \
    (pushedStorage != nullptr ? static_cast<Int>(pushedStorage->Desc.Samples) : (obj)->GetSamples())
#define MGB_STORAGE_FIXED_SAMPLE_LOCATIONS(obj)                                                                        \
    (pushedStorage != nullptr ? pushedStorage->Desc.FixedSampleLocations != 0 : (obj)->HasFixedSampleLocations())
#define MGB_STORAGE_IMMUTABLE(obj)                                                                                     \
    (pushedStorage != nullptr ? pushedStorage->Desc.Immutable != 0 : (obj)->IsImmutable())
#define MGB_STORAGE_KIND(obj)                                                                                          \
    (pushedStorage != nullptr ? static_cast<TextureStorageType>(pushedStorage->Desc.StorageKind)                       \
                              : (obj)->GetStorageType())

        void BackendTextureObject::SyncMipmapsToBackend(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) {
            // P5e (tx2): null IS the by-handle arm once a handle has been noted on this twin;
            // see the twin note in SyncBuiltinSamplerToBackend. P5e (fix1): "a handle was noted"
            // is not on its own the statement that the record arm will be SELECTED - the view
            // test below and RequireImageBindableStorage's transition are transport-gated - so
            // the check is the one shared refusal and a noted handle on an arm that cannot serve
            // it aborts by name rather than falling into the frontend reads.
            if (!stateTextureObject && MGB_TEXTURE_NULL_FRONTEND_REFUSED(*this, "SyncMipmapsToBackend")) {
                MGLOG_E_ONCE("State texture object is null, cannot sync to backend.");
                return;
            }

            // P4a (D-B3/D-D5): on the handle arm the record IS the state this function reads.
            // P5e (tx2) hoists the resolution to the TOP of the function, because the view test
            // below needs it before anything else does; nothing else about it moves.
            //
            // The record pointer stays valid for the whole function: only a create/respecify for
            // a NEW slot grows MGPipeApplierState::TextureResources, and nothing this function
            // calls emits one. Pointers INTO record->PendingUploads do not - the consume is a
            // swap-and-pop - so every one of them is taken, used and dropped inside one level's
            // iteration, and never held across a consume.
            const MG_Pipe::MGPipeResourceRecord* pushedStorage = nullptr;
            MG_Pipe::MGPipeHandle pushedRes = MG_Pipe::kMGPipeNullHandle;
            pushedStorage = ResolveOwnRecord(stateTextureObject);
            pushedRes = !MG_Pipe::MGPipeHandleIsNull(m_pushedSyncHandle)
                            ? m_pushedSyncHandle
                            : (pushedStorage != nullptr ? pushedStorage->Desc.Resource : MG_Pipe::kMGPipeNullHandle);
            if (pushedStorage == nullptr) {
                MGLOG_E_ONCE("MGPipe: texture %u has no applier record on the handle arm, so its "
                             "storage and uploads cannot be driven from the pushed descriptor "
                             "(handle {%u, %u})",
                             TextureDiagName(stateTextureObject, pushedStorage), pushedRes.Slot, pushedRes.Gen);
                return;
            }
#if MOBILEGL_BUILD_DISAGGREGATED
            // SHARED IMAGES: ahead of the view test and of the staged-store and serial gates below,
            // none of which knows the texels belong to an image - the staged store holds none of
            // them, and the storage path would allocate fresh, empty storage over the image.
            if (pushedStorage != nullptr && (pushedStorage->SharedImageId != 0 || m_sharedImageId != 0) &&
                SyncSharedImageStorage(*pushedStorage)) {
                // EVERY use, not only the attach (the draw-clean gates never pass a bound image):
                // a write that landed since the last one is waited for on this context's GPU
                // before the command this sync is for, and the session's frame notes the read.
                if (m_sharedImage != nullptr) SharedImageImpl::AcquireForSampling(m_sharedImage);
                ReplaySharedImageUploads(*pushedStorage);
                // A YUV image is converted into the name at the frame's first use.
                if (m_sharedImage != nullptr && SharedImageImpl::IsYuv(m_sharedImage))
                    SharedImageImpl::ConvertYuvForSampling(m_sharedImage, GetBackendTextureId(),
                                                           pushedStorage->Desc.Width, pushedStorage->Desc.Height);
                return;
            }
#endif
            // P5e (tx2), CONTRACT-P5E §5.2 (scout G-S2-2): THE VIEW TEST MOVES BEHIND THE
            // RECORD. `IsTextureView()` was the one frontend read this function made BEFORE it
            // had resolved anything at all, and Desc.ViewOf is the carrier - it names the storage
            // owner BY HANDLE, so both the question and its answer are server-owned.
            //
            // Ruling 1's arm: the push-monolith build keeps the frontend test below token for
            // token, because there the two answers are the same answer and the verify comparator
            // needs the frontend arm.
            if (pushedStorage != nullptr && !MG_Pipe::MGPipeHandleIsNull(pushedStorage->Desc.ViewOf)) {
                SyncTextureViewToBackendByRecord(pushedRes, *pushedStorage, stateTextureObject);
                return;
            }

#ifdef TRACY_ENABLE
            ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif

            // P4a (D-B3/D-D5): on the handle arm the record IS the state this function reads.
            // Resolved once, here, because three things below want it - the cheap gate, the
            // per-level dirty questions and the union box / rect list / strides the staging
            // planner takes its shape from.
            //
            // The record pointer stays valid for the whole function: only a create/respecify
            // for a NEW slot grows MGPipeApplierState::TextureResources, and nothing this
            // function calls emits one. Pointers INTO record->PendingUploads do not - the
            // consume is a swap-and-pop - so every one of them is taken, used and dropped
            // inside one level's iteration, and never held across a consume.
            if (pushedStorage != nullptr) {
                // THE METADATA RESPECIFY, HONORED (ID-18 M4, review M-2). BindMask and
                // ImageBindableHint are STICKY facts the client discovers AFTER allocation - a
                // texture first bound as a shader image - and an IMMUTABLE texture never has a
                // later redefinition to carry them, so they arrive on a respecify that restates
                // the storage the resource already has. The applier classifies such a record as a
                // metadata update: no reallocation ack, no pending-upload clear, Serial moved.
                // THE TWIN'S HALF IS TO RE-DERIVE ITS STORAGE FLAGS FROM THE NEW MASK AND
                // RECREATE ONLY WHERE THE BACKEND NEEDS THE FLAG AT CREATION - and image
                // bindability is exactly such a flag on this backend: the carrier may be channel-
                // widened, which is a different allocation, not a different binding.
                //
                // RequireImageBindableStorage IS that path and it is idempotent, so this is the
                // whole of the re-derivation: it sets the sticky flag, clears m_isInitialized so
                // the storage below is regenerated, re-arms every defined level in BOTH dirty
                // models (see its own comment) so the widened carrier is replayed rather than
                // allocated empty, and forces the parameter resync the widening's swizzle needs.
                // Arriving on the CREATE instead - the common case once B publishes the hint -
                // costs nothing: m_isInitialized is already false and there are no levels to
                // replay.
                const Bool pushedWantsImageBindableStorage =
                    pushedStorage->Desc.ImageBindableHint != 0 ||
                    (pushedStorage->Desc.BindMask & MG_Pipe::kMGPipeBindShaderImage) != 0;
                if (pushedWantsImageBindableStorage && !m_imageBindableStorageRequired) {
                    // P5e (tx2), ruling 1's arm: under a transport the re-dirty half is a NAMED
                    // refusal at its entry (§5.2) instead of a walk of the client's level shadows;
                    // the push-monolith build keeps the frontend transition exactly as it is.
                    RequireImageBindableStorageByHandle(pushedRes, *pushedStorage);
                }

                // ONE compare replaces the whole cheap-gate trio AND the content version. The
                // applier bumps Serial on every respecify and every sub-data it applies to this
                // resource, which is exactly the union those four keys covered - without the
                // coarse behavior the sampling-resolution generation had, where any texture's
                // shape churn re-opened every other texture's gate.
                //
                // The pending set is the second half and it is not redundant: a previous sync
                // may have BAILED (an incomplete texture returns early, a multisample target
                // refreshes and skips) with the serial already stamped, and the set is what
                // survives that. Empty means every level this record ever declared has been
                // consumed by a sync that actually uploaded it.
                if (m_isInitialized && m_syncedResourceSerial != 0 && m_syncedResourceSerial == pushedStorage->Serial &&
                    pushedStorage->PendingUploads.empty()) {
                    return;
                }
            }

            // First-level clean gate (see the member comment): three version compares and no
            // virtual shape walk. Every mutation the slower probe below would catch bumps one of
            // the keys - shape via the context's sampling-resolution generation (coarse: any
            // texture's shape churn re-opens every gate, which only costs a fall-through to the
            // probe), CPU pixels via the content version, samples/fixed-locations via the params
            // version - and backend-side storage resets clear m_isInitialized. Restricted to
            // Mipmap storage like the probe fast path: a buffer texture's backing store can move
            // without any of these keys noticing.
            //
            // LEGACY ARM ONLY from P4a on: the memos it reads are the pre-handle ones and the
            // record above answers the same question in one compare.
            if (pushedStorage == nullptr)
                if (m_isInitialized && m_syncedShapeContextId != 0 && MG_Pipe::gPipeInputs->IsLive() &&
                    m_syncedShapeContextId == MG_Pipe::gPipeInputs->GetTextureContextId() &&
                    m_syncedShapeGeneration == MG_Pipe::gPipeInputs->GetSamplingResolutionGeneration() &&
                    m_syncedContentVersion == stateTextureObject->GetContentVersion() &&
                    m_syncedShapeParamsVersion == stateTextureObject->GetTextureParamsVersion() &&
                    stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                    return;
                }

            MGLOG_D("Syncing texture mipmaps with backend ID %u to backend for state ID %u", m_backendTextureId,
                    MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));

            GLenum target = ConvertTextureTargetToBackendGLEnum(MGB_TEXTURE_TARGET(stateTextureObject));
            auto targetInternal = MGB_TEXTURE_TARGET(stateTextureObject);
            MGLOG_D("    Texture target for syncing is %s",
                    MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
            if (!IsSupportedTextureTarget(targetInternal)) {
                MGLOG_E_ONCE("    Texture target %s is not supported, skipping.",
                             MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                return;
            }

            // The texture needs to be regenerated completely with glTexImage* calls if:
            // 1. Not initialized
            // 2. InternalFormat changed
            // 3. Size changed
            // 4. Mipmap levels changed

            // IsComplete() is the sampling predicate, and it calls a chain whose lower levels are
            // undefined incomplete - which is what a top-down build (upload level N, then level 0)
            // and ARB_clear_texture's conformance cases both produce. Bailing out on that shape
            // left the backend name with no levels whatsoever, so the level that WAS defined could
            // never be sampled or read back. Sync whenever some level holds an image; the per-level
            // loops below skip the degenerate ones individually.
            // P5c (gt, CONTRACT-P5C §6 layer 1): on the staged arm the same question is answered
            // out of the SERVER's staged-texture store, never out of the frontend object - the
            // store's Defined-ness (fed by the respecify hook and the sub-data adoption) IS "some
            // level holds an image", and IsComplete()'s sampling half is redundant with it: a
            // complete mipmap chain has a defined level 0, and the gate's whole job is to let an
            // incomplete-but-partly-defined chain through. The one divergence is deliberate and
            // stated: a chain whose EVERY level is 0x0 is complete-by-quirk on the frontend (the
            // "0x0 in last level" relaxation, TextureObject.cpp) and undefined here - and syncing
            // it would upload nothing either way, because every per-level loop below skips a
            // {0,0,0} level individually. A BUFFER texture never has staged levels, so its gate
            // is the descriptor's format - exactly what TextureObjectBase::IsComplete() reduces
            // to for that storage kind.
            if (MGB_STAGED_TEXTURE_LIVE) {
                const auto& stagedDesc = pushedStorage->Desc;
                Bool anyDefined = false;
                if (static_cast<TextureStorageType>(stagedDesc.StorageKind) == TextureStorageType::Buffer) {
                    // A buffer texture has no staged levels; its gate is the descriptor's
                    // format, which is what TextureObjectBase::IsComplete() reduces to here.
                    anyDefined =
                        static_cast<TextureInternalFormat>(stagedDesc.InternalFormat) != TextureInternalFormat::Unknown;
                } else {
                    auto& stagedStore = MG_Record::ServerStagedTexture();
                    const Uint64 stagedKey = MG_Record::StagedTextureStore::KeyForHandle(pushedRes);
                    for (const auto& uploadTarget : BufferImpl::StagedUploadTargetsForPipeTarget(stagedDesc.Target)) {
                        for (Uint32 level = 0; level < stagedDesc.Levels; ++level) {
                            if (stagedStore.IsLevelDefined(stagedKey, static_cast<Uint16>(uploadTarget),
                                                           static_cast<Uint16>(level))) {
                                anyDefined = true;
                                break;
                            }
                        }
                        if (anyDefined) break;
                    }
                }
                if (!anyDefined) {
                    MGLOG_D("Texture object with ID: %u has no defined image level, skipping sync.",
                            MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                    return;
                }
            } else if (!stateTextureObject->IsComplete() && !HasAnyDefinedMipmapLevel(stateTextureObject.get())) {
                MGLOG_D("Texture object with ID: %u has no defined image level, skipping sync.",
                        stateTextureObject->GetExternalIndex());
                return;
            }

            // Fast path: a fully-synced mipmap texture is the common per-draw case.
            // SyncNeccessaryTextures re-syncs every bound texture each draw, and the
            // scratch Bind below targets the temp unit - which sequential distinct
            // textures thrash, forcing a real glBindTexture per texture per draw. When
            // nothing needs uploading, skip the bind + upload machinery entirely;
            // BindCurrentTextures() re-establishes the real sampling bindings regardless.
            // The content-version stamp short-circuits before any shape probing: it
            // bumps on every CPU-side pixel mutation, so an unchanged stamp plus an
            // unchanged shape means no level can be dirty. Shape stays a separate
            // compare because a NULL-data glTexImage changes it without touching the
            // content version.
            //
            // LEGACY ARM ONLY from P4a on: on the handle arm the record's Serial plus an empty
            // pending set is the same statement in one compare, made above and BEFORE the
            // IsComplete()/shape walk rather than after it - so this probe would only re-derive
            // an answer already given, out of the very frontend state the arm exists to stop
            // reading.
            if (pushedStorage == nullptr)
                if (m_isInitialized && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap &&
                    m_syncedContentVersion != 0 && m_syncedContentVersion == stateTextureObject->GetContentVersion()) {
                    auto* mipmapObject = static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                    const auto probeBaseSize = stateTextureObject->GetBaseSize();
                    StateTextureBasicInfo probe = {stateTextureObject->GetFormat(),
                                                   static_cast<SizeT>(probeBaseSize.x()),
                                                   static_cast<SizeT>(probeBaseSize.y()),
                                                   static_cast<SizeT>(probeBaseSize.z()),
                                                   static_cast<SizeT>(mipmapObject->GetMipmapLevelCount()),
                                                   0,
                                                   stateTextureObject->GetSamples(),
                                                   stateTextureObject->HasFixedSampleLocations()};
                    if (probe == m_prevTextureInfo) {
                        MGLOG_D("Texture ID %u already fully synced, skipping scratch bind + upload.",
                                m_backendTextureId);
                        // The probe just proved "fully synced" from the real state, so the cheap
                        // gate may be (re)stamped here: the coarse generation only ever goes stale
                        // from OTHER textures' churn, and this draw re-validated this one.
                        if (MG_Pipe::gPipeInputs->IsLive()) {
                            m_syncedShapeContextId = MG_Pipe::gPipeInputs->GetTextureContextId();
                            m_syncedShapeGeneration = MG_Pipe::gPipeInputs->GetSamplingResolutionGeneration();
                            m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
                        }
                        return;
                    }
                }

            Bind(target);
            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });
            const auto baseSize = MGB_STORAGE_BASE_SIZE(stateTextureObject);
            StateTextureBasicInfo currentTextureInfo = {MGB_STORAGE_FORMAT(stateTextureObject),
                                                        static_cast<SizeT>(baseSize.x()),
                                                        static_cast<SizeT>(baseSize.y()),
                                                        static_cast<SizeT>(baseSize.z()),
                                                        0,
                                                        0,
                                                        MGB_STORAGE_SAMPLES(stateTextureObject),
                                                        MGB_STORAGE_FIXED_SAMPLE_LOCATIONS(stateTextureObject)};
            switch (MGB_STORAGE_KIND(stateTextureObject)) {
            case TextureStorageType::Mipmap: {
                auto* textureMipmapObject =
                    static_cast<MG_State::GLState::TextureObjectMipmap*>(stateTextureObject.get());
                const auto mipmapCount = MGB_STORAGE_LEVELS(textureMipmapObject);
                currentTextureInfo.mipmapLevels = mipmapCount;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);
                if (needsRegeneration && m_backendStorageImmutable) {
                    RecreateBackendTexture();
                    Bind(target);
                }

                // Only a texture that is actually image-bound pays for the widening: it doubles
                // or quadruples the storage, and RequireImageBindableStorage is sticky, so a
                // texture that is merely sampled keeps its narrow format for life. See
                // TextureImpl::GetImageBindableStorageWidening for what widens and why.
                const TextureImpl::ImageBindableStorageWidening imageWidening =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableStorageWidening(MGB_STORAGE_FORMAT(textureMipmapObject))
                        : TextureImpl::ImageBindableStorageWidening{};

                const Bool canAppendMipmaps =
                    m_isInitialized && !m_imageBindableStorageRequired && !MGB_STORAGE_IMMUTABLE(stateTextureObject) &&
                    currentTextureInfo.internalFormat == m_prevTextureInfo.internalFormat &&
                    currentTextureInfo.width == m_prevTextureInfo.width &&
                    currentTextureInfo.height == m_prevTextureInfo.height &&
                    currentTextureInfo.depth == m_prevTextureInfo.depth &&
                    currentTextureInfo.bufferExternalIndex == m_prevTextureInfo.bufferExternalIndex &&
                    currentTextureInfo.samples == m_prevTextureInfo.samples &&
                    currentTextureInfo.fixedSampleLocations == m_prevTextureInfo.fixedSampleLocations &&
                    currentTextureInfo.mipmapLevels > m_prevTextureInfo.mipmapLevels &&
                    !TextureImpl::IsMultisampleTextureTarget(targetInternal);

                MGLOG_D("%s: Got texture info: %dx%dx%d, mips %d, format %s", __func__, baseSize.x(), baseSize.y(),
                        baseSize.z(), mipmapCount,
                        MG_Util::ConvertTextureInternalFormatToString(MGB_STORAGE_FORMAT(textureMipmapObject)).c_str());

                if (canAppendMipmaps) {
                    MGLOG_D("Texture mip count increased for backend ID %u, appending levels %zu..%zu",
                            m_backendTextureId, m_prevTextureInfo.mipmapLevels, mipmapCount - 1);

                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(MGB_STORAGE_FORMAT(textureMipmapObject), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);

                    const auto& uploadTargets = MGB_UPLOAD_TARGETS(textureMipmapObject);
                    ScopedDefaultUnpackState unpackState;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = m_prevTextureInfo.mipmapLevels; level < mipmapCount; ++level) {
                            auto levelTexelSize = MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level);
                            // A level the application never defined reads back as {0, 0, 0}; now that a
                            // sparse chain is synced rather than skipped whole, leave those undefined on
                            // the driver instead of giving the name a 0x0 image at that index.
                            if (levelTexelSize.x() <= 0 || levelTexelSize.y() <= 0 || levelTexelSize.z() <= 0) {
                                MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                                continue;
                            }
                            auto levelByteSize = MGB_LEVEL_BYTE_SIZE(textureMipmapObject, uploadTarget, level);
                            bool levelDirty = MGB_LEVEL_NEEDS_UPLOAD(textureMipmapObject, uploadTarget, level);
                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            auto* pData =
                                (levelDirty && levelByteSize != 0)
                                    ? MGB_LEVEL_TEXELS(textureMipmapObject, uploadTarget, level, "append-mips")
                                    : nullptr;
                            Vector<Float> convertedUploadData;
                            Vector<Uint8> widenedUploadData;
                            const void* uploadData = PrepareFallbackUpload(
                                MGB_STORAGE_FORMAT(textureMipmapObject), targetInternal, levelTexelSize, pData,
                                levelByteSize, glType, convertedUploadData, widenedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData =
                                PreparePackedNormUpload(MGB_STORAGE_FORMAT(textureMipmapObject), levelTexelSize,
                                                        uploadData, levelByteSize, &glType, packedUploadData);

                            DebugImpl::ErrorLopper::Clear();
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(MGB_TEXTURE_TARGET(stateTextureObject), levelTexelSize);
                            switch (MapToBackendTextureTarget(MGB_TEXTURE_TARGET(stateTextureObject))) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                g_GLESFuncs.glTexImage2D(glUploadTarget, static_cast<GLint>(level),
                                                         (GLint)glInternalFormat, static_cast<GLsizei>(uploadSize.x()),
                                                         static_cast<GLsizei>(uploadSize.y()), 0, glFormat, glType,
                                                         uploadData);
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                g_GLESFuncs.glTexImage3D(
                                    glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                    static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                    static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                break;
                            default:
                                MGLOG_E_ONCE("Unhandled texture target %s", MG_Util::ConvertTextureTargetToString(
                                                                                MGB_TEXTURE_TARGET(stateTextureObject))
                                                                                .c_str());
                                break;
                            }
                            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__,
                                                          glUploadTarget, glInternalFormat, glFormat, glType,
                                                          pData](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, format=%s, "
                                        "type=%s, pixels=%p",
                                        func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                        MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                        MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                        MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                            });
                            MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                        }
                    }
                    needsRegeneration = false;
                }

                if (needsRegeneration) {
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture with ID: %u",
                            m_backendTextureId);
                    // A REDEFINITION IN PLACE TAKES THE SAME GENERATION A RE-MINT TAKES (P4a fable
                    // seam F-3, the pre-handle half). Mutable driver storage is redefined on the
                    // SAME id below, so unlike RecreateBackendTexture nothing moves the FBO twins'
                    // memo: the frontend framebuffer versions do not see a texture's respecify
                    // and the id did not change. An attached texture whose format moved to one
                    // with the same carrier (GL_SRGB8 -> GL_SRGB8_ALPHA8 on a driver that widens
                    // the first) therefore kept the framebuffer's alpha-widening mask, and every
                    // draw into it stayed masked. The first definition is not a redefinition and
                    // bumps nothing; a redefinition that went through RecreateBackendTexture above
                    // has already bumped.
                    // PUSH BUILDS ONLY. This is Espryt code the pull build would share, and G1
                    // keeps the pull library byte-identical to the P4a baseline (the bump resized
                    // this function and the renderbuffer twin's SyncToBackend: 0/0/2/0). So the
                    // pull build carries the pre-P4a hole until these lines land on dev on their
                    // own; every arm of a push build has the fix.
                    if (m_isInitialized && !m_backendStorageImmutable) {
                        ++FramebufferImpl::g_attachmentBackendIdGeneration;
                    }

                    // Regenerate all mipmap levels
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(MGB_STORAGE_FORMAT(textureMipmapObject), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);
                    ApplyImageBindableStorageWidening(imageWidening, &glInternalFormat, &glFormat, &glType);

                    const auto& uploadTargets = MGB_UPLOAD_TARGETS(textureMipmapObject);
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        // The frontend validates against the count MobileGL advertises, which can
                        // exceed what the driver takes for this format (Adreno: GL_MAX_SAMPLES 4,
                        // GL_MAX_INTEGER_SAMPLES 1). Clamp the ES call - and only the ES call:
                        // stateTextureObject keeps the requested count so GL_TEXTURE_SAMPLES and
                        // framebuffer completeness still report what the application asked for.
                        const auto backendSamples = static_cast<GLsizei>(ClampSamplesToBackendSupport(
                            GetFormatCapabilityTargetIndex(targetInternal), MGB_STORAGE_FORMAT(textureMipmapObject),
                            glFormat, static_cast<Int>(MGB_STORAGE_SAMPLES(stateTextureObject))));
                        // ES 3.1 8.19 requires width/height (and depth, for the array target) >= 1,
                        // so a degenerate size has nothing to allocate and must not reach the
                        // driver. The frontend deallocates such an image rather than defining it
                        // (GL 4.6 core 8.8), so this is belt and braces for any path that still
                        // syncs one.
                        const Bool hasAllocatableSize =
                            baseSize.x() >= 1 && baseSize.y() >= 1 &&
                            (targetInternal != TextureTarget::Texture2DMultisampleArray || baseSize.z() >= 1);
                        if (!hasAllocatableSize) {
                            MGLOG_D("Skipping multisample storage for texture %u: degenerate size (%d, %d, %d)",
                                    m_backendTextureId, baseSize.x(), baseSize.y(), baseSize.z());
                        } else {
                            switch (targetInternal) {
                            case TextureTarget::Texture2DMultisample:
                                g_GLESFuncs.glTexStorage2DMultisample(
                                    target, backendSamples, glInternalFormat, static_cast<GLsizei>(baseSize.x()),
                                    static_cast<GLsizei>(baseSize.y()),
                                    MGB_STORAGE_FIXED_SAMPLE_LOCATIONS(stateTextureObject) ? GL_TRUE : GL_FALSE);
                                break;
                            case TextureTarget::Texture2DMultisampleArray:
                                g_GLESFuncs.glTexStorage3DMultisample(
                                    target, backendSamples, glInternalFormat, static_cast<GLsizei>(baseSize.x()),
                                    static_cast<GLsizei>(baseSize.y()), static_cast<GLsizei>(baseSize.z()),
                                    MGB_STORAGE_FIXED_SAMPLE_LOCATIONS(stateTextureObject) ? GL_TRUE : GL_FALSE);
                                break;
                            default:
                                MOBILEGL_ASSERT(false, "Unexpected multisample target: %d",
                                                static_cast<Int>(targetInternal));
                                break;
                            }
                            m_backendStorageImmutable = true;
                        }
                        // The one storage branch that cleared the ES error queue without ever
                        // draining it again, so anything this call raised was left for an
                        // unrelated later query to trip over. Paired with its two siblings now.
                        DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__, target,
                                                      glInternalFormat, backendSamples](GLenum err) {
                            MGLOG_D("%s(%s:%d) ES error: %s. glTexStorage*Multisample: target=%s, internalformat=%s, "
                                    "samples=%d",
                                    func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                    MG_Util::ConvertGLEnumToString(target).c_str(),
                                    MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                    static_cast<Int>(backendSamples));
                        });
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                            }
                        }
                    } else if (MGB_STORAGE_IMMUTABLE(stateTextureObject) || m_imageBindableStorageRequired) {
                        DebugImpl::ErrorLopper::Clear();
                        BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                        const IntVec3 storageSize = GetBackendUploadSize(targetInternal, baseSize);
                        switch (MapToBackendTextureTarget(targetInternal)) {
                        case TextureTarget::Texture2D:
                        case TextureTarget::TextureCubeMap:
                            g_GLESFuncs.glTexStorage2D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()));
                            break;
                        case TextureTarget::Texture3D:
                        case TextureTarget::Texture2DArray:
                        case TextureTarget::TextureCubeMapArray:
                            g_GLESFuncs.glTexStorage3D(target, static_cast<GLsizei>(mipmapCount), glInternalFormat,
                                                       static_cast<GLsizei>(storageSize.x()),
                                                       static_cast<GLsizei>(storageSize.y()),
                                                       static_cast<GLsizei>(storageSize.z()));
                            break;
                        default:
                            MGLOG_E_ONCE("Unhandled immutable texture target %s",
                                         MG_Util::ConvertTextureTargetToString(targetInternal).c_str());
                            break;
                        }
                        DebugImpl::ErrorLopper::Loop(
                            [file = __FILE__, line = __LINE__, func = __func__, target, glInternalFormat](GLenum err) {
                                MGLOG_D("%s(%s:%d) ES error: %s. glTexStorage*: target=%s, internalformat=%s", func,
                                        file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                        MG_Util::ConvertGLEnumToString(target).c_str(),
                                        MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                            });
                        m_backendStorageImmutable = true;

                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelByteSize = MGB_LEVEL_BYTE_SIZE(textureMipmapObject, uploadTarget, level);
                                const bool levelDirty =
                                    MGB_LEVEL_NEEDS_UPLOAD(textureMipmapObject, uploadTarget, level);
                                if (levelDirty && levelByteSize != 0) {
                                    auto levelTexelSize =
                                        MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level);
                                    auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                    auto* pData =
                                        MGB_LEVEL_TEXELS(textureMipmapObject, uploadTarget, level, "immutable-regen");
                                    Vector<Float> convertedUploadData;
                                    Vector<Uint8> widenedUploadData;
                                    const void* uploadData = PrepareFallbackUpload(
                                        MGB_STORAGE_FORMAT(textureMipmapObject), targetInternal, levelTexelSize, pData,
                                        levelByteSize, glType, convertedUploadData, widenedUploadData);
                                    Vector<Uint8> packedUploadData;
                                    uploadData =
                                        PreparePackedNormUpload(MGB_STORAGE_FORMAT(textureMipmapObject), levelTexelSize,
                                                                uploadData, levelByteSize, &glType, packedUploadData);
                                    Vector<Uint8> imageWidenedUploadData;
                                    uploadData = PrepareImageWidenedUpload(imageWidening, levelTexelSize, uploadData,
                                                                           levelByteSize, imageWidenedUploadData);

                                    DebugImpl::ErrorLopper::Clear();
                                    BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                    const IntVec3 uploadSize = GetBackendUploadSize(targetInternal, levelTexelSize);
                                    switch (MapToBackendTextureTarget(targetInternal)) {
                                    case TextureTarget::Texture2D:
                                    case TextureTarget::TextureCubeMap:
                                        g_GLESFuncs.glTexSubImage2D(glUploadTarget, static_cast<GLint>(level), 0, 0,
                                                                    static_cast<GLsizei>(uploadSize.x()),
                                                                    static_cast<GLsizei>(uploadSize.y()), glFormat,
                                                                    glType, uploadData);
                                        break;
                                    case TextureTarget::Texture3D:
                                    case TextureTarget::Texture2DArray:
                                    case TextureTarget::TextureCubeMapArray:
                                        g_GLESFuncs.glTexSubImage3D(
                                            glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                            static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                            static_cast<GLsizei>(uploadSize.z()), glFormat, glType, uploadData);
                                        break;
                                    default:
                                        break;
                                    }
                                    DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__,
                                                                  glUploadTarget, glFormat, glType, pData](GLenum err) {
                                        MGLOG_D("%s(%s:%d) ES error: %s. glTexSubImage*: target=%s, format=%s, "
                                                "type=%s, pixels=%p",
                                                func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                                MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                                MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                                MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                    });
                                }
                                MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                            }
                        }
                    } else {
                        m_backendStorageImmutable = false;
                        ScopedDefaultUnpackState unpackState;
                        for (auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                auto levelTexelSize = MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level);
                                // See the append-mips loop: an undefined level stays undefined on the
                                // driver rather than becoming a 0x0 image.
                                if (levelTexelSize.x() <= 0 || levelTexelSize.y() <= 0 || levelTexelSize.z() <= 0) {
                                    MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                                    continue;
                                }
                                auto levelByteSize = MGB_LEVEL_BYTE_SIZE(textureMipmapObject, uploadTarget, level);
                                bool levelDirty = MGB_LEVEL_NEEDS_UPLOAD(textureMipmapObject, uploadTarget, level);
                                auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                                auto* pData =
                                    (levelDirty && levelByteSize != 0)
                                        ? MGB_LEVEL_TEXELS(textureMipmapObject, uploadTarget, level, "mutable-regen")
                                        : nullptr;
                                Vector<Float> convertedUploadData;
                                Vector<Uint8> widenedUploadData;
                                const void* uploadData = PrepareFallbackUpload(
                                    MGB_STORAGE_FORMAT(textureMipmapObject), targetInternal, levelTexelSize, pData,
                                    levelByteSize, glType, convertedUploadData, widenedUploadData);
                                Vector<Uint8> packedUploadData;
                                uploadData =
                                    PreparePackedNormUpload(MGB_STORAGE_FORMAT(textureMipmapObject), levelTexelSize,
                                                            uploadData, levelByteSize, &glType, packedUploadData);
                                MGLOG_D("%s: target: %s: syncing mip %d: %dx%dx%d, byteSize = %d, pData = %p, "
                                        "levelDirty = %s",
                                        __func__, MG_Util::ConvertTextureUploadTargetToString(uploadTarget).c_str(),
                                        level, levelTexelSize.x(), levelTexelSize.y(), levelTexelSize.z(),
                                        levelByteSize, pData, levelDirty ? "true" : "false");

                                DebugImpl::ErrorLopper::Clear();
                                BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                                auto textureTarget = MGB_TEXTURE_TARGET(stateTextureObject);
                                const IntVec3 uploadSize = GetBackendUploadSize(textureTarget, levelTexelSize);
                                switch (MapToBackendTextureTarget(textureTarget)) {
                                case TextureTarget::Texture2D:
                                case TextureTarget::TextureCubeMap: {
                                    g_GLESFuncs.glTexImage2D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()), 0,
                                        glFormat, glType, uploadData);
                                    break;
                                }
                                case TextureTarget::Texture3D:
                                case TextureTarget::Texture2DArray:
                                case TextureTarget::TextureCubeMapArray: {
                                    g_GLESFuncs.glTexImage3D(
                                        glUploadTarget, static_cast<GLint>(level), (GLint)glInternalFormat,
                                        static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                        static_cast<GLsizei>(uploadSize.z()), 0, glFormat, glType, uploadData);
                                    break;
                                }
                                default: {
                                    MGLOG_E_ONCE("Unhandled texture target %s",
                                                 MG_Util::ConvertTextureTargetToString(textureTarget).c_str());
                                }
                                }
                                DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__,
                                                              glUploadTarget, glInternalFormat, glFormat, glType,
                                                              pData](GLenum err) {
                                    MGLOG_D("%s(%s:%d) ES error: %s. glTexImage*: target=%s, internalformat=%s, "
                                            "format=%s, type=%s, pixels=%p",
                                            func, file, line, MG_Util::ConvertGLEnumToString(err).c_str(),
                                            MG_Util::ConvertGLEnumToString(glUploadTarget).c_str(),
                                            MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(),
                                            MG_Util::ConvertGLEnumToString(glFormat).c_str(),
                                            MG_Util::ConvertGLEnumToString(glType).c_str(), pData);
                                });
                                MGLOG_D("Regenerated mipmap level %d for texture with ID: %u", level,
                                        m_backendTextureId);
                                MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                            }
                        }
                    }

                    m_isInitialized = true;
                }

                { // Update all dirty mipmap levels
                    if (TextureImpl::IsMultisampleTextureTarget(targetInternal)) {
                        const auto& uploadTargets = MGB_UPLOAD_TARGETS(textureMipmapObject);
                        for (const auto& uploadTarget : uploadTargets) {
                            for (SizeT level = 0; level < mipmapCount; ++level) {
                                if (MGB_LEVEL_NEEDS_UPLOAD(textureMipmapObject, uploadTarget, level)) {
                                    MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                                }
                            }
                        }
                        break;
                    }

                    const auto mipmapCount = MGB_STORAGE_LEVELS(textureMipmapObject);
                    GLenum glInternalFormat, glType, glFormat;
                    TextureImpl::GenerateTextureFormatInfo(MGB_STORAGE_FORMAT(textureMipmapObject), &glInternalFormat,
                                                           &glFormat, &glType, targetInternal);
                    // The storage this level is being written into was widened when it was minted
                    // (see above), so the transfer pair has to describe the carrier here too - ES
                    // requires glTexSubImage's `format` to match the storage's base internal
                    // format, so a GL_RG upload into a GL_RGBA32F image is GL_INVALID_OPERATION.
                    ApplyImageBindableStorageWidening(imageWidening, &glInternalFormat, &glFormat, &glType);
                    const auto& uploadTargets = MGB_UPLOAD_TARGETS(textureMipmapObject);
                    ScopedDefaultUnpackState unpackState;
                    // Per-level scratch of the exact-footprint path, hoisted to reuse capacity.
                    Vector<MG_State::GLState::MipmapDirtyRegion> dirtyFootprint;
                    Vector<UnpackStagingBlock> stagingBlockOverflow;
                    for (auto& uploadTarget : uploadTargets) {
                        for (SizeT level = 0; level < mipmapCount; ++level) {
                            if (!MGB_LEVEL_NEEDS_UPLOAD(textureMipmapObject, uploadTarget, level)) {
                                continue;
                            }

                            auto byteSize = MGB_LEVEL_BYTE_SIZE(textureMipmapObject, uploadTarget, level);
                            if (byteSize == 0) {
                                MGLOG_D("Mipmap level %d has no data, skipping update.", level);
                                continue;
                            }

                            if (level > 0)
                                MGLOG_D("%s: Updating dirty mip %d for texture ID %u, size: %dx%d, "
                                        "byteSize: %d",
                                        __func__, level, m_backendTextureId,
                                        MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level).x(),
                                        MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level).y(), byteSize);

                            auto glUploadTarget = ConvertTextureUploadTargetToBackendGLEnum(uploadTarget);
                            BufferImpl::BindPixelUnpackBufferId(0); // no-op once the resting 0 state is pinned
                            DebugImpl::ErrorLopper::Loop(
                                [file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                                    MGLOG_D("%s(%s:%d) ES error: %s", func, file, line,
                                            MG_Util::ConvertGLEnumToString(err).c_str());
                                });
                            auto texelSize = MGB_LEVEL_TEXEL_SIZE(textureMipmapObject, uploadTarget, level);
                            const void* mipData =
                                MGB_LEVEL_TEXELS(textureMipmapObject, uploadTarget, level, "dirty-level");
                            Vector<Float> convertedUploadData;
                            Vector<Uint8> widenedUploadData;
                            const void* uploadData = PrepareFallbackUpload(
                                MGB_STORAGE_FORMAT(textureMipmapObject), targetInternal, texelSize, mipData, byteSize,
                                glType, convertedUploadData, widenedUploadData);
                            Vector<Uint8> packedUploadData;
                            uploadData = PreparePackedNormUpload(MGB_STORAGE_FORMAT(textureMipmapObject), texelSize,
                                                                 uploadData, byteSize, &glType, packedUploadData);
                            // Leaves `uploadData` pointing at its own buffer when it fires, with
                            // texels four components wide where the shadow's are one or two.
                            Vector<Uint8> imageWidenedUploadData;
                            uploadData = PrepareImageWidenedUpload(imageWidening, texelSize, uploadData, byteSize,
                                                                   imageWidenedUploadData);
                            const IntVec3 uploadSize =
                                GetBackendUploadSize(MGB_TEXTURE_TARGET(stateTextureObject), texelSize);
                            // Sub-rect upload: when only a region of the level changed (a
                            // 16x16 sprite in a 1024x512 atlas, the per-frame lightmap), upload
                            // just that region with UNPACK_ROW_LENGTH striding into the level.
                            // Not an optimization alone: the shadow never sees a GPU write, so
                            // the texels OUTSIDE the region may be stale there and must not go
                            // up over what the GPU drew. Every conversion above rewrites the
                            // whole level texel for texel into a tight buffer of its own, so the
                            // region is taken from whichever buffer `uploadData` now names, at
                            // that buffer's bytes per texel.
                            //
                            // THE SAME ON EVERY ARM (P11 M). `mipData` is the frontend shadow on
                            // the pull and push-monolith arms and the server's staged level on the
                            // split arms - the same tight client layout either way - and the
                            // conversions run on whichever side uploads, so the handle arm takes
                            // its region from the converted buffer exactly as the shadow arm
                            // does. What it must NOT take from the record then is the carried
                            // pitch and offset (below): those index the client's layout, not the
                            // converted one.
                            SizeT uploadBytes = 0;
                            if (uploadData == mipData) {
                                uploadBytes = byteSize;
                            } else if (!imageWidenedUploadData.empty() && uploadData == imageWidenedUploadData.data()) {
                                uploadBytes = imageWidenedUploadData.size();
                            } else if (!packedUploadData.empty() && uploadData == packedUploadData.data()) {
                                uploadBytes = packedUploadData.size();
                            } else if (!widenedUploadData.empty() && uploadData == widenedUploadData.data()) {
                                uploadBytes = widenedUploadData.size();
                            } else if (!convertedUploadData.empty() && uploadData == convertedUploadData.data()) {
                                uploadBytes = convertedUploadData.size() * sizeof(Float);
                            }
                            // A 1D array's ES image is a 2D array one texel high with the layers
                            // in depth, while the shadow keeps the layers as rows: same texels in
                            // the same order, so a region maps across by moving its y to z. (The
                            // target is the descriptor's on the split arms: the server may not
                            // name the client's texture object.)
                            const Bool layersAsRows =
                                MGB_TEXTURE_TARGET(stateTextureObject) == TextureTarget::Texture1DArray;
                            // P4a (D-D6, and ARCHITECTURE.md §9.1's ONE item moved out of the
                            // Espryt do-not-touch list): the DECISION below is unchanged and
                            // stays on the server - it is the side that pays the GPU cost, and
                            // the union-box-versus-N-rects choice is worth 16x the bytes on one
                            // axis and +6 ms/frame of Mali job cost on the other. What moves is
                            // the SOURCE of the shape and of the strides.
                            //
                            // On the handle arm the box and the rect list come from the record
                            // the client emitted; on the legacy arm they come from the frontend's
                            // own rect model, exactly as before. Both spellings feed the same
                            // three staging shapes and the same StageBlocksIntoUnpackRing, which
                            // is byte-identical (G5).
                            const MG_Pipe::MGPipeResourceRecord::PendingUpload* pendingUpload =
                                pushedStorage != nullptr
                                    ? FindPipeTextureUpload(*pushedStorage, static_cast<Uint16>(uploadTarget),
                                                            static_cast<Uint16>(level))
                                    : nullptr;
                            const auto dirtyRegion = [&]() -> MG_State::GLState::MipmapDirtyRegion {
                                if (pendingUpload == nullptr) {
                                    // P5c (tx): a level this arm owes with NO pending upload behind it
                                    // was dirtied by the GPU (T5), and the dirty answer is the
                                    // server's own mark on the staged shadow - the whole level,
                                    // because a generation touches all of it. The client is never
                                    // asked (§2.2's last row).
                                    if (MGB_STAGED_TEXTURE_LIVE) {
                                        const IntVec3 gpuExtent =
                                            MG_Record::ServerStagedTexture().LevelExtentOrUndefined(
                                                MG_Record::StagedTextureStore::KeyForHandle(pushedRes),
                                                static_cast<Uint16>(uploadTarget), static_cast<Uint16>(level));
                                        return MG_State::GLState::MipmapDirtyRegion{IntVec3{0, 0, 0}, gpuExtent};
                                    }
                                    return textureMipmapObject->GetStorageDirtyRegion(uploadTarget, level);
                                }
                                // MGPBox is {origin, extent}; MipmapDirtyRegion is {lo, hi}. The
                                // conversion is the whole difference between the two spellings.
                                const auto& box = pendingUpload->UnionBox;
                                return MG_State::GLState::MipmapDirtyRegion{IntVec3{box.X, box.Y, box.Z},
                                                                            IntVec3{box.X + static_cast<Int32>(box.W),
                                                                                    box.Y + static_cast<Int32>(box.H),
                                                                                    box.Z + static_cast<Int32>(box.D)}};
                            }();
                            const SizeT texelCount = static_cast<SizeT>(texelSize.x()) *
                                                     static_cast<SizeT>(texelSize.y()) *
                                                     static_cast<SizeT>(std::max(texelSize.z(), 1));
                            // Whether a box of the level can be addressed at all: whole texels in
                            // the upload bytes, and an ES image shaped like the shadow.
                            const Bool regionsAddressable =
                                !dirtyRegion.Empty() && texelCount > 0 && uploadBytes > 0 &&
                                uploadBytes % texelCount == 0 &&
                                (layersAsRows ? std::max(texelSize.z(), 1) == 1
                                              : uploadSize.x() == texelSize.x() && uploadSize.y() == texelSize.y() &&
                                                    std::max(uploadSize.z(), 1) == std::max(texelSize.z(), 1));
                            const Bool subRectEligible = regionsAddressable && !dirtyRegion.CoversWholeLevel(texelSize);
                            const SizeT bpp = regionsAddressable ? uploadBytes / texelCount : 0;
                            const IntVec3 regionSize = {dirtyRegion.hi.x() - dirtyRegion.lo.x(),
                                                        dirtyRegion.hi.y() - dirtyRegion.lo.y(),
                                                        dirtyRegion.hi.z() - dirtyRegion.lo.z()};
                            // STRIDES ARE CARRIED, NEVER INFERRED (D-D3), once there is a record
                            // to carry them: a sub-rect's rows are not contiguous in the level
                            // shadow, so each MGPSubRegion states the level's row and slice pitch
                            // and 0 means "tightly packed", which is what a whole-level region
                            // sends. The legacy arm keeps computing them from the level extent
                            // and the bytes-per-texel it just derived - the same two numbers,
                            // from the other side of the wire.
                            // m-2: THE PITCH IS THE LEVEL'S (D-D3), so every region of one level
                            // states the same one - and v1 read Regions.front() and would have
                            // strided the rest at the first one's pitch, silently, if a record
                            // ever disagreed. This is the assert the review asked for rather than
                            // a per-region loop feeding per-region strides: a record whose regions
                            // disagree is corrupt, not a shape the upload planner should learn.
                            // On disagreement the carried pair is DROPPED and the level's own
                            // extent times bpp is used, which is what the legacy arm computes.
                            Bool pendingStridesAgree = true;
                            if (pendingUpload != nullptr && !pendingUpload->Regions.empty()) {
                                const auto& firstRegion = pendingUpload->Regions.front();
                                for (const auto& region : pendingUpload->Regions) {
                                    if (region.SrcRowStride == firstRegion.SrcRowStride &&
                                        region.SrcSliceStride == firstRegion.SrcSliceStride) {
                                        continue;
                                    }
                                    pendingStridesAgree = false;
                                    MGLOG_E_ONCE("MGPipe: texture %u level %u carries regions with two "
                                                 "different level pitches (%u/%u and %u/%u) - a pitch is "
                                                 "the LEVEL's, so the carried pair is dropped and the "
                                                 "extent is used instead",
                                                 MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject),
                                                 static_cast<Uint>(level), firstRegion.SrcRowStride,
                                                 firstRegion.SrcSliceStride, region.SrcRowStride,
                                                 region.SrcSliceStride);
                                    break;
                                }
                            }
                            // P11 M: THE CARRIED PITCH AND OFFSET INDEX THE CLIENT'S LAYOUT - the
                            // level shadow, or the server's staged copy of it, which is `mipData`.
                            // A converted level's region is cut from the conversion's own tight
                            // buffer (above), whose texels are wider, so there the record's numbers
                            // name the wrong bytes and the pitch is derived from the upload
                            // buffer's bpp instead - which is what the shadow arm does.
                            const Bool carriedLayoutApplies = uploadData == mipData;
                            const SizeT carriedRowBytes =
                                (pendingUpload != nullptr && pendingStridesAgree && carriedLayoutApplies &&
                                 !pendingUpload->Regions.empty() && pendingUpload->Regions.front().SrcRowStride != 0)
                                    ? static_cast<SizeT>(pendingUpload->Regions.front().SrcRowStride)
                                    : 0;
                            const SizeT carriedSliceBytes =
                                (pendingUpload != nullptr && pendingStridesAgree && carriedLayoutApplies &&
                                 !pendingUpload->Regions.empty() && pendingUpload->Regions.front().SrcSliceStride != 0)
                                    ? static_cast<SizeT>(pendingUpload->Regions.front().SrcSliceStride)
                                    : 0;
                            const SizeT levelRowBytes =
                                carriedRowBytes != 0 ? carriedRowBytes : static_cast<SizeT>(texelSize.x()) * bpp;
                            const SizeT levelSliceBytes = carriedSliceBytes != 0
                                                              ? carriedSliceBytes
                                                              : static_cast<SizeT>(texelSize.y()) * levelRowBytes;
                            // N-5: THE SAME DISCIPLINE FOR SrcOffset AS FOR THE STRIDES, and the
                            // asymmetry it removes was the review's point: the strides got a loud
                            // refusal in the commit that started reading them while the offset -
                            // which decides the ADDRESS glTexSubImage reads (w*bpp x h x d) bytes
                            // from - was consumed raw. The cross-check is free, because
                            // rectUploadPtr four lines down computes exactly the same number from
                            // the region origin and the two pitches. A record whose regions
                            // disagree with their own origins is corrupt, so on disagreement the
                            // carried offsets are DROPPED - every use site falls back to the
                            // derived pointer, which is what the legacy arm sends - and one named
                            // line says so. Gated on subRectEligible because bpp is 0 otherwise
                            // and the regions are not read at all, and on carriedLayoutApplies
                            // because a converted level never reads them (MGB_RECT_SRC_PTR).
                            //
                            // FIX A2 IS THE SECOND LEGITIMATE DISAGREEMENT, and the fallback is
                            // exactly right for it: a level too large to stage whole crosses as
                            // several slab records (TextureEmit.h's MGPipeForEachTextureSlab) whose
                            // regions carry offsets into THEIR OWN run rather than into the level
                            // shadow. Every piece of the level is merged into this one entry, the
                            // entry's rects are then read out of the SERVER'S WHOLE LEVEL IMAGE -
                            // which is what the derived pointer names - and those piece-relative
                            // offsets are precisely the ones that must not be used here.
                            //
                            // GATED ON regionsAddressable, NOT subRectEligible: the per-region arm
                            // for a level the GPU may have written (m_driverMayHoldGpuWrites below)
                            // reads MGB_RECT_SRC_PTR too, and it runs exactly when the union box is
                            // the whole level - which is what two or more slab pieces always add up
                            // to. Checked only under subRectEligible, a damage rect split across
                            // pieces kept the later pieces' run-relative offsets and read their
                            // rows from the top of the level image (KWin's wl_shm window textures:
                            // the panel repeated down the desktop). bpp is valid whenever the
                            // regions are addressable.
                            Bool pendingOffsetsAgree = true;
                            if (pendingUpload != nullptr && regionsAddressable && carriedLayoutApplies) {
                                for (const auto& region : pendingUpload->Regions) {
                                    const SizeT derivedOffset = static_cast<SizeT>(region.Z) * levelSliceBytes +
                                                                static_cast<SizeT>(region.Y) * levelRowBytes +
                                                                static_cast<SizeT>(region.X) * bpp;
                                    if (static_cast<SizeT>(region.SrcOffset) == derivedOffset) continue;
                                    pendingOffsetsAgree = false;
                                    MGLOG_E_ONCE("MGPipe: texture %u level %u carries a region whose SrcOffset "
                                                 "%llu is not its own origin (%d,%d,%d) at the level's pitch "
                                                 "(%llu expected) - the carried offsets are dropped and every "
                                                 "rect is read from the shadow instead. Expected once for a "
                                                 "level cut into stage-chunk pieces, whose regions index their "
                                                 "own run; otherwise this names a record whose regions and "
                                                 "origins disagree",
                                                 MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject),
                                                 static_cast<Uint>(level),
                                                 static_cast<unsigned long long>(region.SrcOffset), region.X, region.Y,
                                                 region.Z, static_cast<unsigned long long>(derivedOffset));
                                    break;
                                }
                            }
                            const Uint8* regionPtr = static_cast<const Uint8*>(uploadData) +
                                                     static_cast<SizeT>(dirtyRegion.lo.z()) * levelSliceBytes +
                                                     static_cast<SizeT>(dirtyRegion.lo.y()) * levelRowBytes +
                                                     static_cast<SizeT>(dirtyRegion.lo.x()) * bpp;
                            // Scatter refinement behind the union box: ~100 sprite
                            // writes into an atlas leave a box that spans nearly the
                            // whole level while the touched texels are a few percent of
                            // it. The storage's bounded rect list recovers the true
                            // footprint; each rect is uploaded with the same
                            // ROW_LENGTH striding into the level as the box
                            // path. The storage only hands the list out when its
                            // summed area is materially smaller than the box (0
                            // otherwise), so the extra calls always move fewer bytes.
                            MG_State::GLState::MipmapDirtyRegion
                                dirtyRects[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                            SizeT dirtyRectCount = 0;
                            if (subRectEligible) {
                                if (pendingUpload != nullptr) {
                                    // RegionCount == 0 is LEGAL and means "the union box is the
                                    // whole story" (D-D3), which is the same statement the
                                    // frontend's GetStorageDirtyRects makes by returning 0 - so
                                    // both arms reach the box branch below by the same route.
                                    //
                                    // P11 M2: the record's list is the level's EXACT footprint now
                                    // (TextureEmit.h), which the storage's own rect list only
                                    // covered, and the storage withheld that list - "0 rects" - when
                                    // it held more rects than it keeps or when they added up to 3/4
                                    // of the union box (one driver call on nearly the same bytes).
                                    // Both cut-offs are this side's to apply now, so the box-or-rects
                                    // choice on a texture the GPU has not written is what it was.
                                    // (Once the GPU may have written it, the footprint below uploads
                                    // every region exactly, whatever this chose.) Never a partial
                                    // list: a truncated list is dropped writes.
                                    SizeT summedTexels = 0;
                                    for (const auto& region : pendingUpload->Regions) {
                                        summedTexels += static_cast<SizeT>(region.W) * static_cast<SizeT>(region.H) *
                                                        static_cast<SizeT>(std::max<Uint32>(region.D, 1));
                                    }
                                    const Bool listWithheld = pendingUpload->Regions.size() < 2 ||
                                                              pendingUpload->Regions.size() >
                                                                  MG_State::GLState::MipmapStorage::kMaxDirtyRects ||
                                                              summedTexels * 4 >= dirtyRegion.TexelCount() * 3;
                                    dirtyRectCount = listWithheld ? 0 : pendingUpload->Regions.size();
                                    for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                        const auto& region = pendingUpload->Regions[r];
                                        dirtyRects[r] = MG_State::GLState::MipmapDirtyRegion{
                                            IntVec3{region.X, region.Y, region.Z},
                                            IntVec3{region.X + static_cast<Int32>(region.W),
                                                    region.Y + static_cast<Int32>(region.H),
                                                    region.Z + static_cast<Int32>(region.D)}};
                                    }
                                } else
                                    // tx: with an active transport the rect list is the record's (the
                                    // pendingUpload arm above) or nothing - the server's GPU-dirty mark
                                    // is whole-level and has no scatter refinement to hand out, and the
                                    // frontend's rect model is not this side's to read.
                                    if (!MGB_STAGED_TEXTURE_LIVE)
                                        dirtyRectCount = textureMipmapObject->GetStorageDirtyRects(
                                            uploadTarget, level, dirtyRects,
                                            MG_State::GLState::MipmapStorage::kMaxDirtyRects);
                                // The scatter refinement pays only on the client-pointer path,
                                // where fewer bytes mean less driver-side copying. Through the
                                // unpack ring every glTexSubImage is a GPU copy job (Mali), so
                                // ~100 sprite rects become ~100 jobs whose fixed cost dwarfs
                                // the union box's extra bytes - measured +6 ms/frame of GPU
                                // time in MC's animated-atlas ticks. One box, one job.
                                if (BufferImpl::UnpackRingAvailable()) {
                                    dirtyRectCount = 0;
                                }
                            }
                            // Everything above - the union box, the ring's one-box rule, the rect
                            // list with its merges, fold and 3/4 cut-off - may upload shadow texels
                            // the client did not write, which is only harmless while the driver
                            // still holds those texels as they were last uploaded. Once it may hold
                            // GPU writes, the level goes up as exactly the texels written, one box
                            // (one ring job) each, whatever that costs: more boxes, and a union box
                            // that spans the whole level no longer means a whole-level upload.
                            const MG_State::GLState::MipmapDirtyRegion* uploadRects = dirtyRects;
                            SizeT uploadRectCount = dirtyRectCount;
                            if (m_driverMayHoldGpuWrites && regionsAddressable) {
                                if (pendingUpload != nullptr) {
                                    // The handle arm's footprint is the record's region list, all
                                    // of it and in order (MGB_RECT_SRC_PTR indexes the regions by
                                    // position); a box-only entry is its union box.
                                    dirtyFootprint.clear();
                                    for (const auto& region : pendingUpload->Regions) {
                                        dirtyFootprint.push_back(MG_State::GLState::MipmapDirtyRegion{
                                            IntVec3{region.X, region.Y, region.Z},
                                            IntVec3{region.X + static_cast<Int32>(region.W),
                                                    region.Y + static_cast<Int32>(region.H),
                                                    region.Z + static_cast<Int32>(region.D)}});
                                    }
                                } else
                                    // With an active transport and no record behind the level, the
                                    // server's own whole-level GPU-dirty mark is the whole story (the
                                    // same reason the rect list above is not read here).
                                    if (MGB_STAGED_TEXTURE_LIVE) {
                                        dirtyFootprint.clear();
                                    } else
                                        textureMipmapObject->GetStorageDirtyFootprint(uploadTarget, level,
                                                                                      dirtyFootprint);
                                // One box is the union box itself, which the branches below already
                                // upload exactly (as the whole level when it is the whole level).
                                if (dirtyFootprint.size() >= 2) {
                                    uploadRects = dirtyFootprint.data();
                                    uploadRectCount = dirtyFootprint.size();
                                } else {
                                    uploadRectCount = 0;
                                }
                            }
                            const Bool perRectUpload = regionsAddressable && uploadRectCount >= 2;
                            const auto rectUploadPtr = [&](const MG_State::GLState::MipmapDirtyRegion& rect) {
                                return static_cast<const Uint8*>(uploadData) +
                                       static_cast<SizeT>(rect.lo.z()) * levelSliceBytes +
                                       static_cast<SizeT>(rect.lo.y()) * levelRowBytes +
                                       static_cast<SizeT>(rect.lo.x()) * bpp;
                            };
// m-1 / D-D3, "carried, never inferred": MGPSubRegion::SrcOffset is the byte offset of that
// region's origin into the level shadow - the same number the lambda above derives from the
// origin and the two strides - and on the handle arm it is READ rather than re-derived, after
// the cross-check above has agreed that the two spellings say the same thing (N-5). It is read
// only while carriedLayoutApplies (uploadData == mipData), so the base the offset is relative to
// is the level shadow itself and never a conversion buffer: since dev's 8966d5c2 a converted
// level is sub-rect eligible too, and its rects are cut at the conversion's bpp (rectUploadPtr).
//
// THREE USE SITES, AND THE FIRST OF THEM IS UNREACHABLE (N-5's second half, and the correction
// to what esprytobj-v2.md m-1 claimed): dirtyRectCount is forced to 0 whenever
// UnpackRingAvailable(), and the staging-block loop that takes MGB_RECT_SRC_PTR(r, rect) first
// requires `ringUsable && dirtyRectCount >= 2`. The two conditions cannot both hold. That shape
// is pre-existing (the ring/scatter decision, not this package's), so the loop is left as it
// stands and the count is corrected here instead of the loop being deleted underneath it.
//
// A MACRO AND NOT A SECOND LAMBDA, for DV-9's measured reason: this function's nine ErrorLopper
// lambdas are mangled ...::$_N BY POSITION, so one more renumbers every one of them and moves
// the PULL build's symbol set, whose admitted-change set is EMPTY. The pull expansion is the
// pre-P4a call with one pair of parentheses. #undef'd with the rest.
#define MGB_RECT_SRC_PTR(idx, rect)                                                                                    \
    ((pendingUpload != nullptr && pendingOffsetsAgree && carriedLayoutApplies &&                                       \
      static_cast<SizeT>(idx) < pendingUpload->Regions.size())                                                         \
         ? static_cast<const Uint8*>(uploadData) +                                                                     \
               static_cast<SizeT>(pendingUpload->Regions[static_cast<SizeT>(idx)].SrcOffset)                           \
         : rectUploadPtr(rect))
                            // A shadow-grid box as the ES image addresses it, and the image height
                            // that strides the source to match; they differ only for a 1D array
                            // (layersAsRows), whose rows are ES layers of a one-texel-high image.
                            struct BackendBox {
                                GLint x, y, z;
                                GLsizei w, h, d;
                            };
                            const auto ToBackendBox = [&](const MG_State::GLState::MipmapDirtyRegion& rect) {
                                const GLsizei w = static_cast<GLsizei>(rect.hi.x() - rect.lo.x());
                                const GLsizei h = static_cast<GLsizei>(rect.hi.y() - rect.lo.y());
                                const GLsizei d = static_cast<GLsizei>(rect.hi.z() - rect.lo.z());
                                return layersAsRows ? BackendBox{rect.lo.x(), 0, rect.lo.y(), w, 1, h}
                                                    : BackendBox{rect.lo.x(), rect.lo.y(), rect.lo.z(), w, h, d};
                            };
                            const GLint imageHeight = layersAsRows ? 1 : texelSize.y();
                            // Unpack-ring staging plan, decided ONCE for whichever branch
                            // below runs: either every glTexSubImage of this level sources
                            // from the ring or none does, so the pixel-unpack binding is
                            // toggled exactly once per level. The plan's three shapes line
                            // up 1:1 with the switch's three branches.
                            //
                            // Regions are repacked TIGHTLY (row length = the region's own
                            // width), which is why the ring path issues no glPixelStorei at
                            // all: the surrounding ScopedDefaultUnpackState already holds
                            // ROW_LENGTH/IMAGE_HEIGHT at 0, which is exactly what a tight
                            // block wants. It also stages strictly fewer bytes than the
                            // client-pointer path makes the driver walk, which strides over
                            // the whole level width.
                            UnpackStagingBlock stagingBlockArray[MG_State::GLState::MipmapStorage::kMaxDirtyRects];
                            UnpackStagingBlock* stagingBlocks = stagingBlockArray;
                            SizeT stagingBlockCount = 0;
                            const Bool ringUsable = BufferImpl::UnpackRingAvailable();
                            if (!ringUsable) {
                                // Nothing to plan: every branch below keeps the client
                                // pointer and its ROW_LENGTH striding, unchanged.
                            } else if (perRectUpload) {
                                // Only an exact footprint outgrows the array (see uploadRects).
                                if (uploadRectCount > MG_State::GLState::MipmapStorage::kMaxDirtyRects) {
                                    stagingBlockOverflow.resize(uploadRectCount);
                                    stagingBlocks = stagingBlockOverflow.data();
                                }
                                for (SizeT r = 0; r < uploadRectCount; ++r) {
                                    const auto& rect = uploadRects[r];
                                    stagingBlocks[r] = {MGB_RECT_SRC_PTR(r, rect),
                                                        static_cast<SizeT>(rect.hi.x() - rect.lo.x()) * bpp,
                                                        static_cast<SizeT>(rect.hi.y() - rect.lo.y()),
                                                        static_cast<SizeT>(std::max(rect.hi.z() - rect.lo.z(), 1)),
                                                        levelRowBytes,
                                                        levelSliceBytes,
                                                        0};
                                }
                                stagingBlockCount = uploadRectCount;
                            } else if (subRectEligible) {
                                stagingBlocks[0] = {regionPtr,
                                                    static_cast<SizeT>(regionSize.x()) * bpp,
                                                    static_cast<SizeT>(regionSize.y()),
                                                    static_cast<SizeT>(std::max(regionSize.z(), 1)),
                                                    levelRowBytes,
                                                    levelSliceBytes,
                                                    0};
                                stagingBlockCount = 1;
                            } else if (uploadData == mipData && texelCount > 0 && byteSize % texelCount == 0) {
                                // Whole level, shadow bytes verbatim: `byteSize` is exactly
                                // what the driver would have read from the client pointer,
                                // and the shadow is already tightly packed. A CONVERTED
                                // level stays on the pointer path - its buffer's length is
                                // the conversion's, not the shadow's, so nothing here can
                                // size the driver's read of it. The whole-texels check is
                                // the same sanity the sub-rect path applies, and it is what
                                // makes "the shadow's bytes per texel == the transfer's"
                                // hold. (A 1D array's upload size permutes height and depth,
                                // but the texel count and the tight byte order are the same,
                                // so the flat copy still describes what the driver reads.)
                                stagingBlocks[0] = {
                                    static_cast<const Uint8*>(uploadData), static_cast<SizeT>(byteSize), 1, 1,
                                    static_cast<SizeT>(byteSize),          static_cast<SizeT>(byteSize), 0};
                                stagingBlockCount = 1;
                            }
                            const Bool ringStaged =
                                stagingBlockCount > 0 && StageBlocksIntoUnpackRing(stagingBlocks, stagingBlockCount);
                            if (ringStaged) {
                                BufferImpl::BindPixelUnpackBufferId(BufferImpl::UnpackRingBufferId());
                            }
                            // Ring-sourced or not: see NoteInPlaceTextureUpload.
                            NoteInPlaceTextureUpload();
                            if (MG_Util::PipeStats::Enabled()) {
                                // One emission per (upload target, level) that ships texels;
                                // the switch below turns it into either one union-box job or
                                // dirtyRectCount rect jobs. The box/rect split is counted
                                // separately from the bytes on purpose: SSIM is blind to it
                                // and the +6 ms/frame Mali cliff was a shape regression, not
                                // a byte regression (plan section 7.3).
                                const Bool rectShape = subRectEligible && dirtyRectCount >= 2;
                                Uint64 shippedBytes = 0;
                                if (rectShape) {
                                    for (SizeT r = 0; r < dirtyRectCount; ++r) {
                                        const auto& rect = dirtyRects[r];
                                        shippedBytes += static_cast<Uint64>(rect.hi.x() - rect.lo.x()) *
                                                        static_cast<Uint64>(rect.hi.y() - rect.lo.y()) *
                                                        static_cast<Uint64>(std::max(rect.hi.z() - rect.lo.z(), 1)) *
                                                        static_cast<Uint64>(bpp);
                                    }
                                } else if (subRectEligible) {
                                    shippedBytes =
                                        static_cast<Uint64>(regionSize.x()) * static_cast<Uint64>(regionSize.y()) *
                                        static_cast<Uint64>(std::max(regionSize.z(), 1)) * static_cast<Uint64>(bpp);
                                } else {
                                    shippedBytes = static_cast<Uint64>(byteSize);
                                }
                                MG_Util::PipeStats::AddBytes(MG_Util::PipeStats::ByteClass::StageTexture, shippedBytes);
                                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::TextureUploadEmissions, 1);
                                MG_Util::PipeStats::AddCalls(
                                    rectShape ? MG_Util::PipeStats::CallClass::TextureUploadRectEmissions
                                              : MG_Util::PipeStats::CallClass::TextureUploadBoxEmissions,
                                    1);
                                MG_Util::PipeStats::AddCalls(MG_Util::PipeStats::CallClass::TextureUploadJobs,
                                                             rectShape ? static_cast<Uint64>(dirtyRectCount) : 1u);
                            }
                            switch (MapToBackendTextureTarget(MGB_TEXTURE_TARGET(stateTextureObject))) {
                            case TextureTarget::Texture2D:
                            case TextureTarget::TextureCubeMap:
                                if (perRectUpload) {
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    for (SizeT r = 0; r < uploadRectCount; ++r) {
                                        const auto& rect = uploadRects[r];
                                        g_GLESFuncs.glTexSubImage2D(
                                            glUploadTarget, static_cast<GLint>(level), rect.lo.x(), rect.lo.y(),
                                            static_cast<GLsizei>(rect.hi.x() - rect.lo.x()),
                                            static_cast<GLsizei>(rect.hi.y() - rect.lo.y()), glFormat, glType,
                                            ringStaged ? UnpackRingPixelOffset(stagingBlocks[r].offset)
                                                       : static_cast<const void*>(MGB_RECT_SRC_PTR(r, rect)));
                                    }
                                    // The surrounding ScopedDefaultUnpackState shadow says 0.
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                } else if (subRectEligible) {
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                    g_GLESFuncs.glTexSubImage2D(
                                        glUploadTarget, static_cast<GLint>(level), dirtyRegion.lo.x(),
                                        dirtyRegion.lo.y(), static_cast<GLsizei>(regionSize.x()),
                                        static_cast<GLsizei>(regionSize.y()), glFormat, glType,
                                        ringStaged ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                   : static_cast<const void*>(regionPtr));
                                    // The surrounding ScopedDefaultUnpackState shadow says 0.
                                    if (!ringStaged) g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                } else {
                                    g_GLESFuncs.glTexSubImage2D(
                                        glUploadTarget, static_cast<GLint>(level), 0, 0,
                                        static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                        glFormat, glType,
                                        ringStaged ? UnpackRingPixelOffset(stagingBlocks[0].offset) : uploadData);
                                }
                                break;
                            case TextureTarget::Texture3D:
                            case TextureTarget::Texture2DArray:
                            // ES 3.2 has GL_TEXTURE_CUBE_MAP_ARRAY natively and it stores exactly
                            // like a 2D array whose depth is 6 * the cube count.
                            case TextureTarget::TextureCubeMapArray:
                                if (perRectUpload) {
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imageHeight);
                                    }
                                    for (SizeT r = 0; r < uploadRectCount; ++r) {
                                        const auto box = ToBackendBox(uploadRects[r]);
                                        g_GLESFuncs.glTexSubImage3D(
                                            glUploadTarget, static_cast<GLint>(level), box.x, box.y, box.z, box.w,
                                            box.h, box.d, glFormat, glType,
                                            ringStaged ? UnpackRingPixelOffset(stagingBlocks[r].offset)
                                                       : static_cast<const void*>(MGB_RECT_SRC_PTR(r, uploadRects[r])));
                                    }
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                                    }
                                } else if (subRectEligible) {
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, texelSize.x());
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, imageHeight);
                                    }
                                    const auto box = ToBackendBox(dirtyRegion);
                                    g_GLESFuncs.glTexSubImage3D(glUploadTarget, static_cast<GLint>(level), box.x, box.y,
                                                                box.z, box.w, box.h, box.d, glFormat, glType,
                                                                ringStaged
                                                                    ? UnpackRingPixelOffset(stagingBlocks[0].offset)
                                                                    : static_cast<const void*>(regionPtr));
                                    if (!ringStaged) {
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                        g_GLESFuncs.glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
                                    }
                                } else {
                                    g_GLESFuncs.glTexSubImage3D(
                                        glUploadTarget, static_cast<GLint>(level), 0, 0, 0,
                                        static_cast<GLsizei>(uploadSize.x()), static_cast<GLsizei>(uploadSize.y()),
                                        static_cast<GLsizei>(uploadSize.z()), glFormat, glType,
                                        ringStaged ? UnpackRingPixelOffset(stagingBlocks[0].offset) : uploadData);
                                }
                                break;
                            default:
                                MGLOG_E_ONCE("Unhandled texture target %s", MG_Util::ConvertTextureTargetToString(
                                                                                MGB_TEXTURE_TARGET(stateTextureObject))
                                                                                .c_str());
                                break;
                            }
                            if (ringStaged) {
                                // Back to the resting unbound state every other upload site
                                // in this file assumes (their BindPixelUnpackBufferId(0) is
                                // meant to stay a shadow no-op).
                                BufferImpl::BindPixelUnpackBufferId(0);
                            }
                            MGB_LEVEL_UPLOAD_DONE(textureMipmapObject, uploadTarget, level);
                        }
                    }
                }
                break;
            }
            case TextureStorageType::Buffer: {
                auto* textureBufferObject =
                    static_cast<MG_State::GLState::TextureObjectBuffer*>(stateTextureObject.get());
                // P5e (tx2), scout R5 RESOLVED. THE BACKING BUFFER IS THE DESCRIPTOR'S, AND SO IS
                // ITS WINDOW - `Desc.BufferForTexBuffer` names the store and `Desc.BufOffset /
                // BufSize` name the range, so the frontend binding slot below is not read at all
                // on the handle arm.
                //
                // The one thing that argument had to answer is D-A4's `frontend` parameter of
                // EnsureBufferResourceForHandle, which existed for the emulated-persistent-map
                // question. The E note's reading is confirmed at this head: that function's three
                // frontend reads (Managers.cpp's MappedData / SyncPersistentMappedRange /
                // HasDefinedContent) are all inside its MONOLITH arm - under an active transport
                // it is client-free (kimi audit row 29) - so `nullptr` is not a hole, it is the
                // whole of the split answer, and it is what §4.2's Buffer row already prescribes
                // (`EnsureBufferResourceForHandle(nullptr, h)`).
                //
                // WHOLE-VS-RANGE likewise stops asking the buffer object for its size: the
                // sentinel kMGPipeWholeBuffer IS "the whole store", which is the question
                // `rangeSize == buffer->GetSize()` was spelling the long way round.
                MG_Pipe::MGPipeHandle pushedTexBuffer = MG_Pipe::kMGPipeNullHandle;
                if (pushedStorage != nullptr) {
                    pushedTexBuffer = pushedStorage->Desc.BufferForTexBuffer;
                    if (MG_Pipe::MGPipeHandleIsNull(pushedTexBuffer)) {
                        MGLOG_E_ONCE("MGPipe: buffer texture %u's descriptor names no backing buffer on "
                                     "the handle arm, so it cannot be backed (handle {%u, %u})",
                                     MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject), pushedRes.Slot,
                                     pushedRes.Gen);
                        return;
                    }
                }
                // P5e (fix1 follow-up, ID-110): the third conjunct is the one fix1's report
                // asked for a ruling about. This arm hands the backing-buffer lookup
                // kNoFrontendBuffer and leans on the record answering; it was unreachable
                // under monolith only because every writer of a handle-note is
                // transport-gated - safe by a chain rather than by its own statement, which
                // is exactly the shape that cost the monolith arm 137 scenarios. Under a
                // transport the conjunct is already implied by `pushedStorage != nullptr`, so
                // this changes no behavior and closes no door; it says the thing instead of
                // depending on it.
                const Bool texBufferByRecord = pushedStorage != nullptr && MGB_TEXTURE_HANDLE_ARM_OFF(*this) == false;
                static const SharedPtr<MG_State::GLState::BufferObject> kNoFrontendBuffer{};
                auto& buffer = texBufferByRecord ? kNoFrontendBuffer
                                                 : textureBufferObject->GetBufferBindingSlot().GetBoundObject();
                if (!buffer && !texBufferByRecord) {
                    MGLOG_D("Texture buffer object with ID: %u has no bound buffer, skipping sync.",
                            MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                    return;
                }
                auto bufferIndex = buffer ? buffer->GetExternalIndex() : 0u;
                // The memo key is the HANDLE's slot on the handle arm: a GL name is never an
                // identity (section 4.2.1), and the record's Serial covers a slot recycled at a
                // new generation because a respecify moves it.
                currentTextureInfo.bufferExternalIndex =
                    pushedStorage != nullptr ? static_cast<Uint>(pushedTexBuffer.Slot) : bufferIndex;

                Bool needsRegeneration = !m_isInitialized || (currentTextureInfo != m_prevTextureInfo);

                // Need to sync texture buffer if not synced yet
                auto* backendBufferResource = BufferImpl::EnsureBufferResourceForHandle(buffer, pushedTexBuffer);
                if (!backendBufferResource || backendBufferResource->id == 0) {
                    MGLOG_E_ONCE("Failed to sync backing buffer for texture buffer with ID: %u",
                                 MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                    return;
                }

                // Bind buffer to texture
                auto backendId = backendBufferResource->id;

                GLenum glInternalFormat, glType, glFormat;
                TextureImpl::GenerateTextureFormatInfo(MGB_STORAGE_FORMAT(textureBufferObject), &glInternalFormat,
                                                       &glFormat, &glType, TextureTarget::TextureBuffer);
                // The view half of the buffer-image SPLIT. A buffer texture has no storage of its
                // own to widen, but the VIEW its format describes can be re-described one
                // component at a time over the same bytes - rg32f over N texels is r32f over 2N -
                // and WidenImageFormatsPass rewrites every access to subscript it that way. Only
                // for a texture that is actually image-bound: a sampled-only buffer texture keeps
                // the format the application asked for (see GetImageBindableBufferSplitFormat).
                //
                // The split goes on a SEPARATE name (m_bufferImageSplitViewId), not on this one.
                // Re-describing the application's own texture also re-describes what a
                // samplerBuffer reading it sees, and the sampler side is not subscript-rewritten -
                // so texelFetch(s, i) started returning component 2i of the base view instead of
                // texel i. rg32f is a legal SAMPLED buffer-texture format in ES 3.2; only the
                // IMAGE binding needs the split, so only the image binding's name carries it.
                const GLenum bufferImageSplitFormat =
                    m_imageBindableStorageRequired
                        ? TextureImpl::GetImageBindableBufferSplitFormat(MGB_STORAGE_FORMAT(textureBufferObject))
                        : GL_UNKNOWN_MGL;

                if (needsRegeneration) {
                    // Desktop GL has had buffer textures core since 3.1 and MobileGL advertises a
                    // 4.x context, so glTexBuffer is a legal call the app may make on any driver -
                    // but ES only gained them in 3.2, and g_GLESFuncs.glTexBuffer is simply null
                    // below that without EXT/OES_texture_buffer. Calling it was an unconditional
                    // null dereference. There is no conformant way to refuse the call (it is valid
                    // in the context MobileGL claims), so the texture is left unbacked and the
                    // reason is stated once per object, latched by the flag below. It was parked
                    // at MGLOG_I while the level ordering compiled MGLOG_W out of INFO builds;
                    // W is the correct level and now survives there.
                    if (!AreBufferTexturesSupported()) {
                        if (m_bufferTextureUnsupportedReported) {
                            break;
                        }
                        m_bufferTextureUnsupportedReported = true;
                        MGLOG_W("Texture buffer %u cannot be backed: this ES driver has no buffer "
                                "textures (%s). Every draw sampling it will read zero and every "
                                "shader declaring a samplerBuffer will fail to compile. MobileGL "
                                "still advertises GL_MAX_TEXTURE_BUFFER_SIZE = %d because an "
                                "OpenGL 4.x context may not report 0.",
                                MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject), GetBufferTextureTierName(),
                                g_GLESCapabilities.MaxTextureBufferSize);
                        break;
                    }
                    MGLOG_D("Texture state changed significantly or not initialized, regenerating texture buffer with "
                            "ID: %u, buffer ID: %u, buffer size: %zu, format: %s",
                            m_backendTextureId, backendId, buffer ? buffer->GetSize() : rangeSize,
                            MG_Util::ConvertGLEnumToString(glInternalFormat).c_str());
                    // A texture that names a window of the buffer needs the range form; the
                    // whole-buffer forms report offset 0 and the buffer's current size, which
                    // glTexBuffer expresses more directly (and works where the range entry point
                    // is absent).
                    // The WINDOW is carried: MGPResourceDesc::BufOffset / BufSize, with
                    // kMGPipeWholeBuffer (~0) meaning "the whole store, resolved live" - which is
                    // why the whole-buffer arm still asks the BUFFER for its size. That is the
                    // buffer family's own object (P3a's, not this record's), and resolving it
                    // here is exactly what the sentinel is documented to mean.
                    const SizeT rangeOffset = pushedStorage != nullptr
                                                  ? static_cast<SizeT>(pushedStorage->Desc.BufOffset)
                                                  : textureBufferObject->GetBufferRangeOffset();
                    // P5e (tx2): "the whole store" is the SENTINEL on the handle arm, not a size
                    // compare against a frontend BufferObject this arm may not name. The monolith
                    // arm keeps resolving the sentinel live, exactly as its comment says.
                    const Bool wholeBufferRange =
                        pushedStorage != nullptr
                            ? (pushedStorage->Desc.BufOffset == 0 &&
                               pushedStorage->Desc.BufSize == MG_Pipe::kMGPipeWholeBuffer)
                            : (textureBufferObject->GetBufferRangeOffset() == 0 &&
                               textureBufferObject->GetBufferRangeSizeInBytes() == buffer->GetSize());
                    const SizeT rangeSize = pushedStorage == nullptr
                                                ? textureBufferObject->GetBufferRangeSizeInBytes()
                                                : (pushedStorage->Desc.BufSize == MG_Pipe::kMGPipeWholeBuffer
                                                       ? (buffer ? buffer->GetSize() : 0u)
                                                       : static_cast<SizeT>(pushedStorage->Desc.BufSize));
                    // Through CallTexBuffer/CallTexBufferRange rather than g_GLESFuncs directly:
                    // the unsuffixed entry points are the ES 3.2 core spelling, and a driver
                    // whose buffer textures come from EXT/OES_texture_buffer exports the
                    // suffixed ones instead. The dispatchers pick whichever this tier ships.
                    if (wholeBufferRange) {
                        CallTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    } else if (!CallTexBufferRange(GL_TEXTURE_BUFFER, glInternalFormat, backendId,
                                                   static_cast<GLintptr>(rangeOffset),
                                                   static_cast<GLsizeiptr>(rangeSize))) {
                        MGLOG_W_ONCE("Texture buffer %u names a sub-range but the driver has no "
                                     "glTexBufferRange; binding the whole buffer instead",
                                     MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                        CallTexBuffer(GL_TEXTURE_BUFFER, glInternalFormat, backendId);
                    }
                    DebugImpl::ErrorLopper::Loop(
                        [file = __FILE__, line = __LINE__, func = __func__, glInternalFormat, backendId](GLenum err) {
                            MGLOG_D("%s(%s:%d) glTexBuffer(format=%s, buffer=%u) ES error: %s", func, file, line,
                                    MG_Util::ConvertGLEnumToString(glInternalFormat).c_str(), backendId,
                                    MG_Util::ConvertGLEnumToString(err).c_str());
                        });

                    // The image half of the SPLIT, on its own name over the same buffer. Minted
                    // lazily - only a texture that is both image-bound AND holds a format with no
                    // ESSL image spelling ever gets one - and re-pointed here, in the same
                    // regeneration gate as the view above, so the two never describe different
                    // buffers or different windows of one.
                    if (bufferImageSplitFormat != GL_UNKNOWN_MGL) {
                        if (m_bufferImageSplitViewId == 0) {
                            g_GLESFuncs.glGenTextures(1, &m_bufferImageSplitViewId);
                        }
                        if (m_bufferImageSplitViewId == 0) {
                            MGLOG_E_ONCE("Failed to generate the buffer-image split view for texture %u; "
                                         "its image binding will read the unsplit view.",
                                         MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                        } else {
                            g_GLESFuncs.glBindTexture(GL_TEXTURE_BUFFER, m_bufferImageSplitViewId);
                            if (wholeBufferRange) {
                                CallTexBuffer(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId);
                            } else if (!CallTexBufferRange(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId,
                                                           static_cast<GLintptr>(rangeOffset),
                                                           static_cast<GLsizeiptr>(rangeSize))) {
                                CallTexBuffer(GL_TEXTURE_BUFFER, bufferImageSplitFormat, backendId);
                            }
                            // The raw bind above went behind Bind()'s shadow, which tracks objects
                            // rather than names: leaving it claiming THIS object is bound would
                            // make the next Bind(GL_TEXTURE_BUFFER) a no-op and leave the split
                            // view bound in the application texture's place.
                            g_boundTexturesCache[g_activeTextureUnit]
                                                [static_cast<SizeT>(TextureTarget::TextureBuffer)] = nullptr;
                        }
                    }
                }
                break;
            }
            default:
                // TextureStorageType is {Mipmap, Buffer}, both handled above, so this is a
                // backstop for a state object that grew a new storage kind. Skipping the upload
                // renders wrong; throwing unwinds through the C GL ABI and kills the process.
                MGLOG_E_ONCE("DirectGLES texture sync: no upload path for storage type %d on texture %u; "
                             "skipping this sync",
                             static_cast<int>(MGB_TEXTURE_STORAGE_KIND(pushedStorage, stateTextureObject)),
                             MGB_TEXTURE_DIAG_NAME(pushedStorage, stateTextureObject));
                break;
            }

            DebugImpl::ErrorLopper::Loop([file = __FILE__, line = __LINE__, func = __func__](GLenum err) {
                MGLOG_D("%s(%s:%d) ES error: %s", func, file, line, MG_Util::ConvertGLEnumToString(err).c_str());
            });

            m_prevTextureInfo = currentTextureInfo;
            if (pushedStorage != nullptr) {
                // The handle arm's one stamp. Deliberately AFTER the switch, at the same
                // instant the legacy arm stamps its four keys, so a bail arm that returned
                // early leaves it untouched and the next sync re-runs - which is the property
                // the pending set exists to make safe (D-D5).
                //
                // The record pointer was resolved before any driver work and TextureResources
                // has not been grown since (nothing this function calls emits a create), so
                // re-reading Serial through it is sound; the PendingUploads entries it named
                // have been consumed one at a time by MGB_LEVEL_UPLOAD_DONE.
                m_syncedResourceSerial = pushedStorage->Serial;
                return;
            }
            // Everything dirty at entry is uploaded (or provably has no bytes to
            // upload); stamp the version so per-draw re-syncs short-circuit until
            // the next CPU-side mutation.
            m_syncedContentVersion = stateTextureObject->GetContentVersion();
            // Same instant, so the cheap gate's keys describe exactly this synced state.
            // Only Mipmap storage may arm it - the gate refuses other storage types anyway,
            // but a stale trio must not linger on an object that later switches type.
            if (MG_Pipe::gPipeInputs->IsLive() && stateTextureObject->GetStorageType() == TextureStorageType::Mipmap) {
                m_syncedShapeContextId = MG_Pipe::gPipeInputs->GetTextureContextId();
                m_syncedShapeGeneration = MG_Pipe::gPipeInputs->GetSamplingResolutionGeneration();
                m_syncedShapeParamsVersion = stateTextureObject->GetTextureParamsVersion();
            } else {
                m_syncedShapeContextId = 0;
            }
        }
#undef MGB_LEVEL_NEEDS_UPLOAD
#undef MGB_LEVEL_UPLOAD_DONE
#undef MGB_STORAGE_FORMAT
#undef MGB_STORAGE_BASE_SIZE
#undef MGB_STORAGE_LEVELS
#undef MGB_STORAGE_SAMPLES
#undef MGB_STORAGE_FIXED_SAMPLE_LOCATIONS
#undef MGB_STORAGE_IMMUTABLE
#undef MGB_STORAGE_KIND
#undef MGB_TEXTURE_TARGET
#undef MGB_UPLOAD_TARGETS
#undef MGB_LEVEL_TEXEL_SIZE
#undef MGB_LEVEL_BYTE_SIZE
#undef MGB_LEVEL_TEXELS
#undef MGB_STAGED_TEXTURE_LIVE

        // P5e (tx2), CONTRACT-P5E §5.2: THIS TWIN'S OWN RECORD, by the handle the caller noted on
        // the twin before it resolved it; the client allocator is never probed.
        const MG_Pipe::MGPipeResourceRecord* BackendTextureObject::ResolveOwnRecord(
            const SharedPtr<MG_State::GLState::ITextureObject>& stateTextureObject) const {
            (void)stateTextureObject;
            // P13: a twin is only ever reached by handle, and the handle is noted on it first.
            if (MG_Pipe::MGPipeHandleIsNull(m_pushedSyncHandle)) return nullptr;
            return PipeTextureRecordForHandle(m_pushedSyncHandle);
        }
    } // namespace TextureImpl
} // namespace MobileGL::MG_Backend::DirectGLES
