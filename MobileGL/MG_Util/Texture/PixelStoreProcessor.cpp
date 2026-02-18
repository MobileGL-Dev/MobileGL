// MobileGL - MobileGL/MG_Util/Texture/PixelStoreProcessor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PixelStoreProcessor.h"

namespace MobileGL::MG_Util::PixelStoreProcessor {
    static SizeT CalculateRowStride(Int width, SizeT pixelSize, Int alignment) {
        if (width <= 0 || pixelSize == 0) return 0;

        const SizeT rowBytes = static_cast<SizeT>(width) * pixelSize;
        const SizeT alignedRowBytes = (rowBytes + alignment - 1) & ~static_cast<SizeT>(alignment - 1);
        return alignedRowBytes;
    }

    static void SwapBytes(void* data, SizeT size, SizeT count) {
        if (size <= 1) return;

        Uint8* bytes = static_cast<Uint8*>(data);
        for (SizeT i = 0; i < count; ++i) {
            Uint8* pixel = bytes + i * size;
            std::reverse(pixel, pixel + size);
        }
    }

    static inline Uint8 ReverseByteBits(Uint8 b) {
        Uint8 r = 0;
        for (int i = 0; i < 8; ++i) {
            r <<= 1;
            r |= (b & 1);
            b >>= 1;
        }
        return r;
    }

    static void ProcessLSBFirst(void* data, SizeT width, SizeT height) {
        if (!data || width == 0 || height == 0) return;

        const SizeT rowBytes = (width + 7) / 8; // packed bits per row
        Uint8* bytes = static_cast<Uint8*>(data);

        for (SizeT y = 0; y < height; ++y) {
            for (SizeT i = 0; i < rowBytes; ++i) {
                bytes[i] = ReverseByteBits(bytes[i]);
            }
            bytes += rowBytes;
        }
    }

    static Uint8 GetSwizzledChannelValue(Uint8* pixel, TextureSwizzleParam param) {
        switch (param) {
        case TextureSwizzleParam::Red:
            return pixel[0];
        case TextureSwizzleParam::Green:
            return pixel[1];
        case TextureSwizzleParam::Blue:
            return pixel[2];
        case TextureSwizzleParam::Alpha:
            return pixel[3];
        case TextureSwizzleParam::Zero:
            return 0;
        case TextureSwizzleParam::One:
            return 0xFF;
        default:
            return 0xBD;
        }
    }

    // assume 8 bit per channel
    // swizzle.size() == channel count
    void ProcessColorSwizzle(void* data, SizeT pixelCount, const Vector<TextureSwizzleParam>& swizzle) {
        const auto bpp = swizzle.size();
        Uint8* bytes = static_cast<Uint8*>(data);
        Uint8 pixelScratch[4];
        for (SizeT i = 0; i < pixelCount; ++i) {
            Uint8* pixel = bytes + i * bpp;
            for (SizeT ch = 0; ch < bpp; ++ch) {
                pixelScratch[ch] = GetSwizzledChannelValue(pixel, swizzle[ch]);
            }
            Memcpy(pixel, pixelScratch, bpp);
        }
    }

    void* ProcessTexturePixelsDataUnpack(const void* inputPixels, const PixelStoreParameters& params,
                                         TextureInternalFormat targetInternalFormat,
                                         TextureInputFormat textureInputFormat, TexturePixelDataType inputDataType,
                                         IntVec3 dimension, Bool isBitmap, SizeT& outSize) {
        const SizeT pixelSize = MG_Util::GetInputBytesPerPixel(textureInputFormat, inputDataType);

        Int width = dimension.x();
        Int height = dimension.y();
        Int depth = dimension.z();

        const Int effectiveWidth = (params.RowLength > 0) ? params.RowLength : width;
        const Int effectiveHeight = (params.ImageHeight > 0) ? params.ImageHeight : height;
        const SizeT inputRowStride = CalculateRowStride(effectiveWidth, pixelSize, params.Alignment);
        const SizeT outputRowStride = static_cast<SizeT>(width) * pixelSize;

        const Int startX = params.SkipPixels;
        const Int startY = params.SkipRows;
        const Int startZ = params.SkipImages;

        const Int copyWidth = width;
        const Int copyHeight = height;
        const Int copyDepth = depth;

        MGLOG_D("%s: start at: (%d, %d, %d), copy size: (%d, %d, %d), i/o row stride: (%d, %dx%d)", __func__, startX,
                startY, startZ, copyWidth, copyHeight, copyDepth, inputRowStride, width, pixelSize);

        if (copyWidth <= 0 || copyHeight <= 0 || copyDepth <= 0) {
            outSize = 0;
            return nullptr;
        }

        outSize = static_cast<SizeT>(copyWidth) * copyHeight * copyDepth * pixelSize;
        void* outputPixels = malloc(outSize);
        if (!outputPixels) {
            outSize = 0;
            return nullptr;
        }

        const Uint8* src = static_cast<const Uint8*>(inputPixels);
        Uint8* dst = static_cast<Uint8*>(outputPixels);

        src += static_cast<SizeT>(startZ) * static_cast<SizeT>(effectiveHeight) * inputRowStride;
        src += static_cast<SizeT>(startY) * inputRowStride;
        src += static_cast<SizeT>(startX) * pixelSize;

        Bool isByteType =
            (inputDataType == TexturePixelDataType::UnsignedByte || inputDataType == TexturePixelDataType::Byte);
        for (Int z = 0; z < copyDepth; ++z) {
            const Uint8* layerSrc = src;
            Uint8* layerDst = dst;

            for (Int y = 0; y < copyHeight; ++y) {
                Memcpy(layerDst, layerSrc, static_cast<SizeT>(copyWidth) * pixelSize);

                if (params.SwapBytes && pixelSize > 1 && !isByteType) {
                    MGLOG_D("%s: SwapBytes", __func__);
                    SwapBytes(layerDst, pixelSize, static_cast<SizeT>(copyWidth));
                }

                if (params.LSBFirst && isBitmap) {
                    MGLOG_D("%s: LSBFirst", __func__);
                    ProcessLSBFirst(layerDst, static_cast<SizeT>(copyWidth), 1);
                }

                if (textureInputFormat == TextureInputFormat::BGRA &&
                    targetInternalFormat == TextureInternalFormat::RGBA8) {
                    MGLOG_D("%s: Swizzle (BGRA)", __func__);
                    //                    MGLOG_D("%s: pixel0 before = %x", __func__, *((Uint32*)layerDst));
                    ProcessColorSwizzle(layerDst, static_cast<SizeT>(copyWidth),
                                        {TextureSwizzleParam::Green, TextureSwizzleParam::Blue,
                                         TextureSwizzleParam::Alpha, TextureSwizzleParam::Red});
                    //                    MGLOG_D("%s: pixel0 after  = %x", __func__, *((Uint32*)layerDst));
                }
                //                else
                //                    MGLOG_D("%s: pixel0        = %x", __func__, *((Uint32*)layerDst));

                layerSrc += inputRowStride;
                layerDst += outputRowStride;
            }

            src += static_cast<SizeT>(effectiveHeight) * inputRowStride;
            dst += static_cast<SizeT>(copyHeight) * outputRowStride;
        }

        return outputPixels;
    }

    void* ProcessTexturePixelsDataPack(const void* inputPixels, const PixelStoreParameters& params,
                                       TextureInternalFormat srcInternalFormat, TexturePixelDataType srcDataType,
                                       TextureInputFormat dstInputFormat, TexturePixelDataType dstDataType,
                                       IntVec3 dimension, Bool isBitmap, SizeT& outSize) {
        const SizeT pixelSize = MG_Util::GetInternalBytesPerPixel(srcInternalFormat, srcDataType);

        Int width = dimension.x();
        Int height = dimension.y();
        Int depth = dimension.z();

        if (pixelSize == 0) {
            outSize = 0;
            return nullptr;
        }

        // TODO: take care of PixelStoreParameters
        const SizeT outputRowStride = width * pixelSize;
        const Int effectiveHeight = height;
        const SizeT inputRowStride = outputRowStride;

        outSize = static_cast<SizeT>(outputRowStride) * static_cast<SizeT>(effectiveHeight) * static_cast<SizeT>(depth);
        void* outputPixels = malloc(outSize);
        if (!outputPixels) {
            outSize = 0;
            return nullptr;
        }

        Memset(outputPixels, 0, outSize);

        const Uint8* src = static_cast<const Uint8*>(inputPixels);
        Uint8* dst = static_cast<Uint8*>(outputPixels);

        Vector<Uint8> tempRow(static_cast<SizeT>(width) * pixelSize);

        bool needSwizzle = false;
        static Vector<TextureSwizzleParam> swizzle;
        swizzle = {TextureSwizzleParam::Red, TextureSwizzleParam::Green, TextureSwizzleParam::Blue,
                   TextureSwizzleParam::Alpha};
        if (srcInternalFormat == TextureInternalFormat::RGBA && dstInputFormat == TextureInputFormat::BGRA) {
            swizzle = {TextureSwizzleParam::Blue, TextureSwizzleParam::Green, TextureSwizzleParam::Red,
                       TextureSwizzleParam::Alpha};
            needSwizzle = true;
        }
        if (dstDataType == TexturePixelDataType::UnsignedInt8888) {
            std::reverse(swizzle.begin(), swizzle.end());
            needSwizzle = true;
        }

        for (Int z = 0; z < depth; ++z) {
            Uint8* layerDst = dst;
            const Uint8* layerSrc = src;

            for (Int y = 0; y < height; ++y) {
                Memcpy(tempRow.data(), layerSrc, static_cast<SizeT>(width) * pixelSize);

                if (params.SwapBytes && pixelSize > 1) {
                    SwapBytes(tempRow.data(), pixelSize, static_cast<SizeT>(width));
                }

                if (params.LSBFirst && isBitmap) {
                    ProcessLSBFirst(tempRow.data(), static_cast<SizeT>(width), 1);
                }

                if (needSwizzle) {
                    ProcessColorSwizzle(tempRow.data(), static_cast<SizeT>(width), swizzle);
                }

                Memcpy(layerDst, tempRow.data(), static_cast<SizeT>(width) * pixelSize);

                layerSrc += inputRowStride;
                layerDst += outputRowStride;
            }

            src += static_cast<SizeT>(height) * inputRowStride;
            dst += static_cast<SizeT>(effectiveHeight) * outputRowStride;
        }

        return outputPixels;
    }
} // namespace MobileGL::MG_Util::PixelStoreProcessor
