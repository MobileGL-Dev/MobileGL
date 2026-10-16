// MobileGL - MobileGL/MG_Backend/ClientReadback.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ClientReadback.h"
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Util/Math/HalfFloat.h>
#include <MG_Util/Math/SmallFloat.h>

#include <algorithm>
#include <cmath>

namespace MobileGL::MG_Backend {
    // ---- Client-format readback conversion helpers -------------------------------------------------
    // ReadPixels/GetTexImage read a guaranteed wide RGBA(_INTEGER) layout from the ES driver and repack
    // it on the CPU into the client's (format, type) layout. Everything here is pure byte shuffling so
    // unit tests can assert the exact packed words; field positions follow GL 3.3 table 3.6 and mirror
    // the GL CTS packed_pixels oracle (glcPackedPixelsTests.cpp pack_UNSIGNED_* helpers).
    namespace ClientReadback {
        using MG_Util::DecodeHalfBitsToFloat;
        using MG_Util::EncodeFloatToHalfBits;

        Bool GetReadbackChannelMapping(GLenum format, ReadbackChannelMapping& outMapping) {
            switch (format) {
            case GL_RED:          outMapping = {{0, 0, 0, 0}, 1, false}; return true;
            case GL_RED_INTEGER:  outMapping = {{0, 0, 0, 0}, 1, true};  return true;
            // Desktop-GL single-channel client formats (GL CTS packed_pixels rgba8_format_green/blue):
            // the destination holds one component sourced from the named channel of the wide RGBA read.
            // GL_ALPHA is mapped here from the raw enum because the state layer folds it into Red for the
            // legacy alpha-texture upload hack.
            case GL_GREEN:         outMapping = {{1, 0, 0, 0}, 1, false}; return true;
            case GL_GREEN_INTEGER: outMapping = {{1, 0, 0, 0}, 1, true};  return true;
            case GL_BLUE:          outMapping = {{2, 0, 0, 0}, 1, false}; return true;
            case GL_BLUE_INTEGER:  outMapping = {{2, 0, 0, 0}, 1, true};  return true;
            case GL_ALPHA:         outMapping = {{3, 0, 0, 0}, 1, false}; return true;
            case GL_ALPHA_INTEGER: outMapping = {{3, 0, 0, 0}, 1, true};  return true;
            case GL_RG:           outMapping = {{0, 1, 0, 0}, 2, false}; return true;
            case GL_RG_INTEGER:   outMapping = {{0, 1, 0, 0}, 2, true};  return true;
            case GL_RGB:          outMapping = {{0, 1, 2, 0}, 3, false}; return true;
            case GL_RGB_INTEGER:  outMapping = {{0, 1, 2, 0}, 3, true};  return true;
            case GL_BGR:          outMapping = {{2, 1, 0, 0}, 3, false}; return true;
            case GL_BGR_INTEGER:  outMapping = {{2, 1, 0, 0}, 3, true};  return true;
            case GL_RGBA:         outMapping = {{0, 1, 2, 3}, 4, false}; return true;
            case GL_RGBA_INTEGER: outMapping = {{0, 1, 2, 3}, 4, true};  return true;
            case GL_BGRA:         outMapping = {{2, 1, 0, 3}, 4, false}; return true;
            case GL_BGRA_INTEGER: outMapping = {{2, 1, 0, 3}, 4, true};  return true;
            default:
                return false;
            }
        }

        Bool GetPackedReadbackLayout(GLenum type, PackedReadbackLayout& out) {
            switch (type) {
            // Non-REV types pack the first format component starting at the most significant bit,
            // *_REV types starting at the least significant bit (GL CTS pack_UNSIGNED_SHORT_5_6_5:
            // R bits 15-11; pack_UNSIGNED_SHORT_1_5_5_5_REV: R bits 4-0, A bit 15).
            case GL_UNSIGNED_BYTE_3_3_2:          out = {3, {3, 3, 2, 0},    {5, 2, 0, 0},    1, false}; return true;
            case GL_UNSIGNED_BYTE_2_3_3_REV:      out = {3, {3, 3, 2, 0},    {0, 3, 6, 0},    1, false}; return true;
            case GL_UNSIGNED_SHORT_5_6_5:         out = {3, {5, 6, 5, 0},    {11, 5, 0, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_5_6_5_REV:     out = {3, {5, 6, 5, 0},    {0, 5, 11, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_4_4_4_4:       out = {4, {4, 4, 4, 4},    {12, 8, 4, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_4_4_4_4_REV:   out = {4, {4, 4, 4, 4},    {0, 4, 8, 12},   2, false}; return true;
            case GL_UNSIGNED_SHORT_5_5_5_1:       out = {4, {5, 5, 5, 1},    {11, 6, 1, 0},   2, false}; return true;
            case GL_UNSIGNED_SHORT_1_5_5_5_REV:   out = {4, {5, 5, 5, 1},    {0, 5, 10, 15},  2, false}; return true;
            case GL_UNSIGNED_INT_8_8_8_8:         out = {4, {8, 8, 8, 8},    {24, 16, 8, 0},  4, false}; return true;
            case GL_UNSIGNED_INT_8_8_8_8_REV:     out = {4, {8, 8, 8, 8},    {0, 8, 16, 24},  4, false}; return true;
            case GL_UNSIGNED_INT_10_10_10_2:      out = {4, {10, 10, 10, 2}, {22, 12, 2, 0},  4, false}; return true;
            case GL_UNSIGNED_INT_2_10_10_10_REV:  out = {4, {10, 10, 10, 2}, {0, 10, 20, 30}, 4, false}; return true;
            // Packed-float RGB types: fields hold unsigned small floats; 5_9_9_9_REV's shared 5-bit
            // exponent (bits 31-27) is emitted by EncodeSharedExponentRGB9E5, not a component field.
            case GL_UNSIGNED_INT_10F_11F_11F_REV: out = {3, {11, 11, 10, 0}, {0, 11, 22, 0},  4, true};  return true;
            case GL_UNSIGNED_INT_5_9_9_9_REV:     out = {3, {9, 9, 9, 0},    {0, 9, 18, 0},   4, true};  return true;
            default:
                return false;
            }
        }

        SizeT GetReadbackComponentSize(GLenum type) {
            PackedReadbackLayout packedLayout{};
            if (GetPackedReadbackLayout(type, packedLayout)) {
                return packedLayout.byteSize;
            }
            switch (type) {
            case GL_UNSIGNED_BYTE:
            case GL_BYTE:
                return 1;
            case GL_UNSIGNED_SHORT:
            case GL_SHORT:
            case GL_HALF_FLOAT:
                return 2;
            case GL_UNSIGNED_INT:
            case GL_INT:
            case GL_FLOAT:
                return 4;
            default:
                return 0;
            }
        }

        SizeT GetReadbackDstPixelSize(const ReadbackChannelMapping& mapping, GLenum type) {
            PackedReadbackLayout packedLayout{};
            if (GetPackedReadbackLayout(type, packedLayout)) {
                if (packedLayout.fieldCount != mapping.channelCount) {
                    return 0; // 3-field packed types pair with 3-component formats only, 4 with 4
                }
                if (mapping.isInteger && packedLayout.isFloatPacked) {
                    return 0; // packed-float RGB types never pair with integer formats
                }
                return packedLayout.byteSize;
            }
            if (mapping.isInteger && (type == GL_FLOAT || type == GL_HALF_FLOAT)) {
                return 0;
            }
            const SizeT componentSize = GetReadbackComponentSize(type);
            return componentSize == 0 ? 0 : static_cast<SizeT>(mapping.channelCount) * componentSize;
        }

        namespace {
            void WritePackedReadbackWord(Uint8* dst, Uint32 word, SizeT byteSize) {
                switch (byteSize) {
                case 1: {
                    const auto out = static_cast<Uint8>(word);
                    Memcpy(dst, &out, sizeof(out));
                    break;
                }
                case 2: {
                    const auto out = static_cast<Uint16>(word);
                    Memcpy(dst, &out, sizeof(out));
                    break;
                }
                default:
                    Memcpy(dst, &word, sizeof(word));
                    break;
                }
            }
        } // namespace

        // Shared encoders live in MG_Util/Math/SmallFloat.h so the upload conversion
        // (PixelStoreProcessor) uses byte-identical packing; kept exported here for unit tests.
        Uint32 EncodeFloatToUnsignedF11(Float value) { return MG_Util::EncodeFloatToUnsignedF11(value); }
        Uint32 EncodeFloatToUnsignedF10(Float value) { return MG_Util::EncodeFloatToUnsignedF10(value); }
        Uint32 EncodeSharedExponentRGB9E5(const Float rgb[3]) { return MG_Util::EncodeSharedExponentRGB9E5(rgb); }

        void ConvertWideReadbackRow(const Uint8* src, Uint8* dst, SizeT width, GLenum wideType,
                                    const ReadbackChannelMapping& mapping, GLenum type) {
            PackedReadbackLayout packedLayout{};
            const Bool isPacked = GetPackedReadbackLayout(type, packedLayout);
            const SizeT dstComponentSize = GetReadbackComponentSize(type);
            const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
            const SizeT srcPixelBytes = 4 * GetReadbackComponentSize(wideType);

            for (SizeT col = 0; col < width; ++col) {
                const Uint8* srcPixel = src + col * srcPixelBytes;
                Uint8* dstPixel = dst + col * dstPixelBytes;
                if (mapping.isInteger) {
                    Int64 srcValues[4];
                    for (Int c = 0; c < 4; ++c) {
                        srcValues[c] = wideType == GL_INT
                                           ? static_cast<Int64>(reinterpret_cast<const Int32*>(srcPixel)[c])
                                           : static_cast<Int64>(reinterpret_cast<const Uint32*>(srcPixel)[c]);
                    }
                    if (isPacked) {
                        // Integer sources clamp each component to the unsigned range of its field
                        // (GL 3.3 section 4.3.1 final conversion).
                        Uint32 word = 0;
                        for (Int ch = 0; ch < packedLayout.fieldCount; ++ch) {
                            const Int64 fieldMax = (Int64{1} << packedLayout.width[ch]) - 1;
                            const auto v = static_cast<Uint32>(
                                std::clamp<Int64>(srcValues[mapping.sourceChannel[ch]], 0, fieldMax));
                            word |= v << packedLayout.shift[ch];
                        }
                        WritePackedReadbackWord(dstPixel, word, packedLayout.byteSize);
                    } else {
                        for (Int ch = 0; ch < mapping.channelCount; ++ch) {
                            const Int64 v = srcValues[mapping.sourceChannel[ch]];
                            Uint8* dstComponent = dstPixel + static_cast<SizeT>(ch) * dstComponentSize;
                            switch (type) {
                            case GL_UNSIGNED_BYTE:
                                *dstComponent = static_cast<Uint8>(std::clamp<Int64>(v, 0, 255));
                                break;
                            case GL_BYTE: {
                                const auto out = static_cast<Int8>(std::clamp<Int64>(v, -128, 127));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_SHORT: {
                                const auto out = static_cast<Uint16>(std::clamp<Int64>(v, 0, 65535));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_SHORT: {
                                const auto out = static_cast<Int16>(std::clamp<Int64>(v, -32768, 32767));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_INT: {
                                const auto out = static_cast<Uint32>(std::clamp<Int64>(v, 0, 4294967295LL));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_INT: {
                                const auto out =
                                    static_cast<Int32>(std::clamp<Int64>(v, -2147483648LL, 2147483647LL));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                } else {
                    Float srcValues[4];
                    switch (wideType) {
                    case GL_UNSIGNED_BYTE:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = static_cast<Float>(srcPixel[c]) / 255.0f;
                        }
                        break;
                    case GL_BYTE:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = std::max(
                                static_cast<Float>(reinterpret_cast<const Int8*>(srcPixel)[c]) / 127.0f, -1.0f);
                        }
                        break;
                    case GL_UNSIGNED_SHORT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] =
                                static_cast<Float>(reinterpret_cast<const Uint16*>(srcPixel)[c]) / 65535.0f;
                        }
                        break;
                    case GL_SHORT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = std::max(
                                static_cast<Float>(reinterpret_cast<const Int16*>(srcPixel)[c]) / 32767.0f, -1.0f);
                        }
                        break;
                    case GL_HALF_FLOAT:
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = DecodeHalfBitsToFloat(reinterpret_cast<const Uint16*>(srcPixel)[c]);
                        }
                        break;
                    default: // GL_FLOAT
                        for (Int c = 0; c < 4; ++c) {
                            srcValues[c] = reinterpret_cast<const Float*>(srcPixel)[c];
                        }
                        break;
                    }
                    if (isPacked) {
                        Uint32 word = 0;
                        if (packedLayout.isFloatPacked) {
                            const Float fields[3] = {srcValues[mapping.sourceChannel[0]],
                                                     srcValues[mapping.sourceChannel[1]],
                                                     srcValues[mapping.sourceChannel[2]]};
                            word = type == GL_UNSIGNED_INT_5_9_9_9_REV
                                       ? EncodeSharedExponentRGB9E5(fields)
                                       : (EncodeFloatToUnsignedF11(fields[0]) << packedLayout.shift[0]) |
                                             (EncodeFloatToUnsignedF11(fields[1]) << packedLayout.shift[1]) |
                                             (EncodeFloatToUnsignedF10(fields[2]) << packedLayout.shift[2]);
                        } else {
                            // Normalized encode: round(clamp(v, 0, 1) * (2^bits - 1)) into each field.
                            for (Int ch = 0; ch < packedLayout.fieldCount; ++ch) {
                                const auto fieldMax = static_cast<Float>((1u << packedLayout.width[ch]) - 1u);
                                const auto v = static_cast<Uint32>(std::llround(
                                    std::clamp(srcValues[mapping.sourceChannel[ch]], 0.0f, 1.0f) * fieldMax));
                                word |= v << packedLayout.shift[ch];
                            }
                        }
                        WritePackedReadbackWord(dstPixel, word, packedLayout.byteSize);
                    } else {
                        for (Int ch = 0; ch < mapping.channelCount; ++ch) {
                            const Float v = srcValues[mapping.sourceChannel[ch]];
                            Uint8* dstComponent = dstPixel + static_cast<SizeT>(ch) * dstComponentSize;
                            switch (type) {
                            case GL_UNSIGNED_BYTE:
                                *dstComponent =
                                    static_cast<Uint8>(std::llround(std::clamp(v, 0.0f, 1.0f) * 255.0));
                                break;
                            case GL_BYTE: {
                                const auto out =
                                    static_cast<Int8>(std::llround(std::clamp(v, -1.0f, 1.0f) * 127.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_SHORT: {
                                const auto out =
                                    static_cast<Uint16>(std::llround(std::clamp(v, 0.0f, 1.0f) * 65535.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_SHORT: {
                                const auto out =
                                    static_cast<Int16>(std::llround(std::clamp(v, -1.0f, 1.0f) * 32767.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_UNSIGNED_INT: {
                                const auto out = static_cast<Uint32>(
                                    std::llround(static_cast<Double>(std::clamp(v, 0.0f, 1.0f)) * 4294967295.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_INT: {
                                const auto out = static_cast<Int32>(
                                    std::llround(static_cast<Double>(std::clamp(v, -1.0f, 1.0f)) * 2147483647.0));
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            case GL_FLOAT:
                                Memcpy(dstComponent, &v, sizeof(v));
                                break;
                            case GL_HALF_FLOAT: {
                                const Uint16 out = EncodeFloatToHalfBits(v);
                                Memcpy(dstComponent, &out, sizeof(out));
                                break;
                            }
                            default:
                                break;
                            }
                        }
                    }
                }
            }
        }

        static SizeT AlignReadbackRow(SizeT rowBytes, Int alignment) {
            const SizeT align = alignment > 0 ? static_cast<SizeT>(alignment) : 1;
            return (rowBytes + align - 1) / align * align;
        }

        // Walks the client-side destination the PACK parameters describe and hands each row to
        // `fillRow(slice, row, dstRow)`, which writes width * dstPixelBytes bytes of finished client
        // texels. Shared by the converting and the raw-word stores so both address the destination
        // identically. The destination is always client memory: a readback into a pack buffer takes
        // the verb path and never reaches these stores.
        // applyPackImageParams: GL_PACK_IMAGE_HEIGHT / GL_PACK_SKIP_IMAGES apply only to GetTexImage
        // of 3D/array images; ReadPixels and 2D GetTexImage ignore them (GL 3.3 sections 4.3.1, 6.1.4).
        // Per the GL addressing rules, slice k row j lands at
        // SKIP_IMAGES*imageStride + SKIP_ROWS*rowStride + SKIP_PIXELS*pixelBytes
        //   + k*imageStride + j*rowStride, with imageStride = max(IMAGE_HEIGHT, sliceHeight)*rowStride.
        template <typename FillRow>
        static Bool StoreClientRows(SizeT dstPixelBytes, SizeT swapGroupSize, GLsizei width, GLsizei sliceHeight,
                                    GLsizei sliceCount, void* pixels, Bool applyPackImageParams, FillRow&& fillRow) {
            // Destination layout is computed from the client-side PACK parameters; only the actual pixel
            // rows are written so skip regions of the destination stay untouched.
            const auto packParams = MG_Pipe::gPipeInputs->GetPixelStoreParameters(false);
            const SizeT rowPixels = static_cast<SizeT>(packParams.RowLength > 0 ? packParams.RowLength : width);
            const SizeT dstRowStride = AlignReadbackRow(rowPixels * dstPixelBytes, packParams.Alignment);
            const SizeT imageRows =
                applyPackImageParams && packParams.ImageHeight > 0
                    ? static_cast<SizeT>(packParams.ImageHeight)
                    : static_cast<SizeT>(sliceHeight);
            const SizeT dstImageStride = imageRows * dstRowStride;
            const SizeT skipImages =
                applyPackImageParams ? static_cast<SizeT>(std::max(packParams.SkipImages, 0)) : SizeT{0};
            const SizeT dstSkipOffset = skipImages * dstImageStride +
                                        static_cast<SizeT>(std::max(packParams.SkipRows, 0)) * dstRowStride +
                                        static_cast<SizeT>(std::max(packParams.SkipPixels, 0)) * dstPixelBytes;
            const SizeT dstRowBytes = static_cast<SizeT>(width) * dstPixelBytes;

            Vector<Uint8> convertedRow(dstRowBytes);

            for (GLsizei slice = 0; slice < sliceCount; ++slice) {
                for (GLsizei row = 0; row < sliceHeight; ++row) {
                    fillRow(slice, row, convertedRow.data());

                    if (packParams.SwapBytes && swapGroupSize > 1) {
                        for (SizeT offset = 0; offset + swapGroupSize <= dstRowBytes; offset += swapGroupSize) {
                            std::reverse(convertedRow.data() + offset, convertedRow.data() + offset + swapGroupSize);
                        }
                    }

                    const SizeT dstOffset = dstSkipOffset + static_cast<SizeT>(slice) * dstImageStride +
                                         static_cast<SizeT>(row) * dstRowStride;
                    Memcpy(static_cast<Uint8*>(pixels) + dstOffset, convertedRow.data(), dstRowBytes);
                }
            }
            return true;
        }

        // Repacks wide RGBA(_INTEGER) rows into the client's (format, type) layout, honoring the
        // client-side PACK parameters. `wide` holds
        // `sliceHeight * sliceCount` rows of `width` texels (slice-major, tightly stacked),
        // 4 components x GetReadbackComponentSize(wideType) bytes each.
        Bool StoreWideRowsToClient(const Uint8* wide, GLenum wideType, GLsizei width, GLsizei sliceHeight,
                                   GLsizei sliceCount, const ReadbackChannelMapping& mapping, GLenum type,
                                   void* pixels, Bool applyPackImageParams) {
            const SizeT dstPixelBytes = GetReadbackDstPixelSize(mapping, type);
            if (dstPixelBytes == 0) {
                return false;
            }
            PackedReadbackLayout packedLayout{};
            const Bool isPackedType = GetPackedReadbackLayout(type, packedLayout);
            const SizeT swapGroupSize = isPackedType ? packedLayout.byteSize : GetReadbackComponentSize(type);
            const SizeT srcPixelBytes = 4 * GetReadbackComponentSize(wideType);

            return StoreClientRows(dstPixelBytes, swapGroupSize, width, sliceHeight, sliceCount, pixels,
                                   applyPackImageParams,
                                   [&](GLsizei slice, GLsizei row, Uint8* dstRow) {
                                       const SizeT flatRow = static_cast<SizeT>(slice) *
                                                                 static_cast<SizeT>(sliceHeight) +
                                                             static_cast<SizeT>(row);
                                       const Uint8* srcRow =
                                           wide + flatRow * static_cast<SizeT>(width) * srcPixelBytes;
                                       ConvertWideReadbackRow(srcRow, dstRow, static_cast<SizeT>(width), wideType,
                                                              mapping, type);
                                   });
        }

        Bool StorePackedWordsToClient(const Uint8* srcWords, GLsizei width, GLsizei sliceHeight, GLsizei sliceCount,
                                      GLenum type, void* pixels, Bool applyPackImageParams) {
            PackedReadbackLayout packedLayout{};
            if (!GetPackedReadbackLayout(type, packedLayout) || packedLayout.byteSize != 4) {
                return false;
            }
            const SizeT srcRowBytes = static_cast<SizeT>(width) * 4;

            return StoreClientRows(4, packedLayout.byteSize, width, sliceHeight, sliceCount, pixels,
                                   applyPackImageParams,
                                   [&](GLsizei slice, GLsizei row, Uint8* dstRow) {
                                       const SizeT flatRow = static_cast<SizeT>(slice) *
                                                                 static_cast<SizeT>(sliceHeight) +
                                                             static_cast<SizeT>(row);
                                       Memcpy(dstRow, srcWords + flatRow * srcRowBytes, srcRowBytes);
                                   });
        }
    } // namespace ClientReadback
} // namespace MobileGL::MG_Backend
