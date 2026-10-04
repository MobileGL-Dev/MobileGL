// MobileGL - MobileGL/MG_Remote/Server/SharedImageYuv.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// YUV SHARED IMAGES: the pure parts - plane copies and the YCbCr -> RGB conversion an image's
// colour hints select - kept free of Android and of any backend so the host tests can check them.
//
// A YUV image is sampled as RGBA: whoever binds it to a texture gets the image CONVERTED into the
// texture's own RGBA8 storage at its first use in each of the reader's frames (the backends do
// that on the GPU, from the image's AHardwareBuffer). The conversion is the one the EGLImage's
// EGL_EXT_image_dma_buf_import hints name - colour space, sample range and chroma siting - with
// that extension's defaults (ITU-R BT.601, narrow range, siting 0) where a hint is absent.

#pragma once
#include <Includes.h>

#include <cstring>

namespace MobileGL::MG_Remote::Server::SharedImages::Yuv {

    // EGL_EXT_image_dma_buf_import's hint values.
    inline constexpr Uint32 kEglItuRec601 = 0x327F;
    inline constexpr Uint32 kEglItuRec709 = 0x3280;
    inline constexpr Uint32 kEglItuRec2020 = 0x3281;
    inline constexpr Uint32 kEglYuvFullRange = 0x3282;
    inline constexpr Uint32 kEglYuvNarrowRange = 0x3283;
    inline constexpr Uint32 kEglYuvChromaSiting0 = 0x3284;
    inline constexpr Uint32 kEglYuvChromaSiting0_5 = 0x3285;

    // What an import said about its samples. 0 = not given (the extension's default applies).
    struct Hints {
        Uint32 ColorSpace = 0;
        Uint32 Range = 0;
        Uint32 SitingX = 0;
        Uint32 SitingY = 0;

        Uint32 EffectiveColorSpace() const {
            return ColorSpace == kEglItuRec709 || ColorSpace == kEglItuRec2020 ? ColorSpace : kEglItuRec601;
        }
        Bool FullRange() const { return Range == kEglYuvFullRange; }
        Bool MidpointX() const { return SitingX == kEglYuvChromaSiting0_5; }
        Bool MidpointY() const { return SitingY == kEglYuvChromaSiting0_5; }
        Bool operator==(const Hints& other) const {
            return ColorSpace == other.ColorSpace && Range == other.Range && SitingX == other.SitingX &&
                   SitingY == other.SitingY;
        }
    };

    // rgb = Matrix * (ycbcr - Offset), every value normalized to [0, 1] as a UNORM texture of the
    // image's sample size returns it. Matrix is COLUMN-major (a GLSL mat3 uniform as is).
    struct Conversion {
        float Matrix[9];
        float Offset[3];
    };

    // `sampleMax` is what a full-scale code reads as before normalization: 255 for 8-bit samples,
    // 65535/64 for P010 (10 bits in the high end of 16, read as UNORM16).
    inline Conversion ConversionFor(const Hints& hints, double sampleMax) {
        double kr = 0.299, kb = 0.114;
        if (hints.EffectiveColorSpace() == kEglItuRec709) {
            kr = 0.2126;
            kb = 0.0722;
        } else if (hints.EffectiveColorSpace() == kEglItuRec2020) {
            kr = 0.2627;
            kb = 0.0593;
        }
        const double kg = 1.0 - kr - kb;
        // Code values of an 8-bit-equivalent scale, multiplied up for deeper samples.
        const double scale = sampleMax / 255.0;
        double yOffset = 0.0, yScale = 1.0, cScale = 1.0;
        const double cOffset = 128.0 * scale / sampleMax;
        if (!hints.FullRange()) {
            yOffset = 16.0 * scale / sampleMax;
            yScale = sampleMax / (219.0 * scale);
            cScale = sampleMax / (224.0 * scale);
        }
        Conversion c{};
        // Columns: Y, Cb, Cr.
        const double m[9] = {
            yScale,
            yScale,
            yScale,
            0.0,
            -cScale * 2.0 * kb * (1.0 - kb) / kg,
            cScale * 2.0 * (1.0 - kb),
            cScale * 2.0 * (1.0 - kr),
            -cScale * 2.0 * kr * (1.0 - kr) / kg,
            0.0,
        };
        for (SizeT i = 0; i < 9; ++i) c.Matrix[i] = static_cast<float>(m[i]);
        c.Offset[0] = static_cast<float>(yOffset);
        c.Offset[1] = static_cast<float>(cOffset);
        c.Offset[2] = static_cast<float>(cOffset);
        return c;
    }

    // One component plane of a destination: where (x, y)'s sample of it lives is
    // Data + y * RowStride + x * PixelStride.
    struct PlaneView {
        Uint8* Data = nullptr;
        Int64 RowStride = 0;
        Uint32 PixelStride = 0;
    };
    struct PlanarLayout {
        PlaneView Y, Cb, Cr;
    };

    // A two-plane semi-planar source (NV12: Y, then Cb Cr interleaved at half resolution; P010
    // the same with 16-bit samples, `sampleBytes` = 2) into any destination layout: semi-planar
    // Cb-first rows are copied as they are, everything else (Cr first, fully planar) sample by
    // sample. False when the destination cannot hold the samples (null planes, a pixel stride
    // smaller than a sample).
    inline Bool CopySemiPlanar(const Uint8* srcY, Int64 srcYPitch, const Uint8* srcCbCr, Int64 srcCbCrPitch,
                               Uint32 width, Uint32 height, Uint32 sampleBytes, const PlanarLayout& dst) {
        if (srcY == nullptr || srcCbCr == nullptr || dst.Y.Data == nullptr || dst.Cb.Data == nullptr ||
            dst.Cr.Data == nullptr || (sampleBytes != 1 && sampleBytes != 2) || dst.Y.PixelStride < sampleBytes ||
            dst.Cb.PixelStride < sampleBytes || dst.Cr.PixelStride < sampleBytes)
            return false;
        const Uint32 chromaWidth = (width + 1) / 2;
        const Uint32 chromaHeight = (height + 1) / 2;
        // Luma.
        for (Uint32 y = 0; y < height; ++y) {
            const Uint8* from = srcY + static_cast<Int64>(y) * srcYPitch;
            Uint8* to = dst.Y.Data + static_cast<Int64>(y) * dst.Y.RowStride;
            if (dst.Y.PixelStride == sampleBytes) {
                std::memcpy(to, from, static_cast<SizeT>(width) * sampleBytes);
            } else {
                for (Uint32 x = 0; x < width; ++x)
                    std::memcpy(to + static_cast<SizeT>(x) * dst.Y.PixelStride, from + static_cast<SizeT>(x) * sampleBytes,
                                sampleBytes);
            }
        }
        // Chroma.
        const Bool sameInterleave = dst.Cb.PixelStride == 2 * sampleBytes && dst.Cr.PixelStride == 2 * sampleBytes &&
                                    dst.Cr.Data == dst.Cb.Data + sampleBytes && dst.Cr.RowStride == dst.Cb.RowStride;
        for (Uint32 y = 0; y < chromaHeight; ++y) {
            const Uint8* from = srcCbCr + static_cast<Int64>(y) * srcCbCrPitch;
            if (sameInterleave) {
                std::memcpy(dst.Cb.Data + static_cast<Int64>(y) * dst.Cb.RowStride, from,
                            static_cast<SizeT>(chromaWidth) * 2 * sampleBytes);
                continue;
            }
            Uint8* cb = dst.Cb.Data + static_cast<Int64>(y) * dst.Cb.RowStride;
            Uint8* cr = dst.Cr.Data + static_cast<Int64>(y) * dst.Cr.RowStride;
            for (Uint32 x = 0; x < chromaWidth; ++x) {
                const Uint8* pair = from + static_cast<SizeT>(x) * 2 * sampleBytes;
                std::memcpy(cb + static_cast<SizeT>(x) * dst.Cb.PixelStride, pair, sampleBytes);
                std::memcpy(cr + static_cast<SizeT>(x) * dst.Cr.PixelStride, pair + sampleBytes, sampleBytes);
            }
        }
        return true;
    }

} // namespace MobileGL::MG_Remote::Server::SharedImages::Yuv
