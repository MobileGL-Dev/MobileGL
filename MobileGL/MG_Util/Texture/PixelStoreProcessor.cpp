// MobileGL - MobileGL/MG_Util/Texture/PixelStoreProcessor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PixelStoreProcessor.h"
#include "MG_Util/Math/HalfFloat.h"
#include "MG_Util/Math/SmallFloat.h"
#include <cmath>

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

    // ---- Unpack channel expansion / type conversion ------------------------------------------------------------
    // The shadow mip buffer stores every level in the internal format's canonical layout: its channels in
    // R,G,B(,A) order, encoded with the component type the backends upload with (see
    // TextureFormatProcessor::NormalizePixelFormat; channelCount * componentSize matches
    // GetSizedInternalFormatSizeInBytes for every format listed below). When the client's (format, type)
    // does not already produce that byte layout, each texel is decoded to RGBA (float for normalized/float
    // formats, integer for *_INTEGER formats, missing G/B = 0 and missing A = 1) and re-encoded.

    namespace {
        enum class ShadowComponent {
            UNorm8,
            SNorm8,
            UNorm16,
            SNorm16,
            UInt8,
            Int8,
            UInt16,
            Int16,
            UInt32,
            Int32,
            Half,
            Float32,
            UNorm32, // 32-bit fixed-point depth shadow
        };

        struct InternalShadowLayout {
            Int channelCount;
            ShadowComponent component;
            Bool isInteger;
        };

        SizeT GetShadowComponentSize(ShadowComponent component) {
            switch (component) {
            case ShadowComponent::UNorm8:
            case ShadowComponent::SNorm8:
            case ShadowComponent::UInt8:
            case ShadowComponent::Int8:
                return 1;
            case ShadowComponent::UNorm16:
            case ShadowComponent::SNorm16:
            case ShadowComponent::UInt16:
            case ShadowComponent::Int16:
            case ShadowComponent::Half:
                return 2;
            default:
                return 4;
            }
        }

        Bool GetInternalShadowLayout(TextureInternalFormat internal, InternalShadowLayout& out) {
            switch (internal) {
            // Depth shadows follow TextureFormatProcessor::NormalizePixelFormat: 16-bit
            // unorm for DEPTH_COMPONENT16, 32-bit unorm for the 24/32-bit fixed-point
            // depths, float for DEPTH_COMPONENT32F.
            case TextureInternalFormat::DepthComponent16:
                out = {1, ShadowComponent::UNorm16, false};
                return true;
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent:
                out = {1, ShadowComponent::UNorm32, false};
                return true;
            case TextureInternalFormat::DepthComponent32F:
                out = {1, ShadowComponent::Float32, false};
                return true;

            case TextureInternalFormat::R8:
            case TextureInternalFormat::Red:     out = {1, ShadowComponent::UNorm8, false}; return true;
            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG:      out = {2, ShadowComponent::UNorm8, false}; return true;
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB:
            case TextureInternalFormat::SRGB8:   out = {3, ShadowComponent::UNorm8, false}; return true;
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA:
            case TextureInternalFormat::SRGB8Alpha8: out = {4, ShadowComponent::UNorm8, false}; return true;

            // Legacy desktop-GL sized normalized formats are stored in the closest ES-legal layout
            // (see TextureFormatProcessor::NormalizePixelFormat): 8-bit unorm for <=8-bit channels,
            // 16-bit unorm for 10/12-bit channels.
            case TextureInternalFormat::R3G3B2:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:    out = {3, ShadowComponent::UNorm8, false}; return true;
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGB5A1:  out = {4, ShadowComponent::UNorm8, false}; return true;
            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:   out = {3, ShadowComponent::UNorm16, false}; return true;
            case TextureInternalFormat::RGBA12:  out = {4, ShadowComponent::UNorm16, false}; return true;

            case TextureInternalFormat::R8Snorm:    out = {1, ShadowComponent::SNorm8, false}; return true;
            case TextureInternalFormat::RG8Snorm:   out = {2, ShadowComponent::SNorm8, false}; return true;
            case TextureInternalFormat::RGB8Snorm:  out = {3, ShadowComponent::SNorm8, false}; return true;
            case TextureInternalFormat::RGBA8Snorm: out = {4, ShadowComponent::SNorm8, false}; return true;

            case TextureInternalFormat::R16:    out = {1, ShadowComponent::UNorm16, false}; return true;
            case TextureInternalFormat::RG16:   out = {2, ShadowComponent::UNorm16, false}; return true;
            case TextureInternalFormat::RGB16:  out = {3, ShadowComponent::UNorm16, false}; return true;
            case TextureInternalFormat::RGBA16: out = {4, ShadowComponent::UNorm16, false}; return true;

            case TextureInternalFormat::R16Snorm:    out = {1, ShadowComponent::SNorm16, false}; return true;
            case TextureInternalFormat::RG16Snorm:   out = {2, ShadowComponent::SNorm16, false}; return true;
            case TextureInternalFormat::RGB16Snorm:  out = {3, ShadowComponent::SNorm16, false}; return true;
            case TextureInternalFormat::RGBA16Snorm: out = {4, ShadowComponent::SNorm16, false}; return true;

            case TextureInternalFormat::R16F:    out = {1, ShadowComponent::Half, false}; return true;
            case TextureInternalFormat::RG16F:   out = {2, ShadowComponent::Half, false}; return true;
            case TextureInternalFormat::RGB16F:  out = {3, ShadowComponent::Half, false}; return true;
            case TextureInternalFormat::RGBA16F: out = {4, ShadowComponent::Half, false}; return true;

            case TextureInternalFormat::R32F:    out = {1, ShadowComponent::Float32, false}; return true;
            case TextureInternalFormat::RG32F:   out = {2, ShadowComponent::Float32, false}; return true;
            case TextureInternalFormat::RGB32F:  out = {3, ShadowComponent::Float32, false}; return true;
            case TextureInternalFormat::RGBA32F: out = {4, ShadowComponent::Float32, false}; return true;

            case TextureInternalFormat::R8UI:    out = {1, ShadowComponent::UInt8, true}; return true;
            case TextureInternalFormat::RG8UI:   out = {2, ShadowComponent::UInt8, true}; return true;
            case TextureInternalFormat::RGB8UI:  out = {3, ShadowComponent::UInt8, true}; return true;
            case TextureInternalFormat::RGBA8UI: out = {4, ShadowComponent::UInt8, true}; return true;

            case TextureInternalFormat::R8I:    out = {1, ShadowComponent::Int8, true}; return true;
            case TextureInternalFormat::RG8I:   out = {2, ShadowComponent::Int8, true}; return true;
            case TextureInternalFormat::RGB8I:  out = {3, ShadowComponent::Int8, true}; return true;
            case TextureInternalFormat::RGBA8I: out = {4, ShadowComponent::Int8, true}; return true;

            case TextureInternalFormat::R16UI:    out = {1, ShadowComponent::UInt16, true}; return true;
            case TextureInternalFormat::RG16UI:   out = {2, ShadowComponent::UInt16, true}; return true;
            case TextureInternalFormat::RGB16UI:  out = {3, ShadowComponent::UInt16, true}; return true;
            case TextureInternalFormat::RGBA16UI: out = {4, ShadowComponent::UInt16, true}; return true;

            case TextureInternalFormat::R16I:    out = {1, ShadowComponent::Int16, true}; return true;
            case TextureInternalFormat::RG16I:   out = {2, ShadowComponent::Int16, true}; return true;
            case TextureInternalFormat::RGB16I:  out = {3, ShadowComponent::Int16, true}; return true;
            case TextureInternalFormat::RGBA16I: out = {4, ShadowComponent::Int16, true}; return true;

            case TextureInternalFormat::R32UI:    out = {1, ShadowComponent::UInt32, true}; return true;
            case TextureInternalFormat::RG32UI:   out = {2, ShadowComponent::UInt32, true}; return true;
            case TextureInternalFormat::RGB32UI:  out = {3, ShadowComponent::UInt32, true}; return true;
            case TextureInternalFormat::RGBA32UI: out = {4, ShadowComponent::UInt32, true}; return true;

            case TextureInternalFormat::R32I:    out = {1, ShadowComponent::Int32, true}; return true;
            case TextureInternalFormat::RG32I:   out = {2, ShadowComponent::Int32, true}; return true;
            case TextureInternalFormat::RGB32I:  out = {3, ShadowComponent::Int32, true}; return true;
            case TextureInternalFormat::RGBA32I: out = {4, ShadowComponent::Int32, true}; return true;

            default:
                // Packed internal layouts (RGB10A2, RGB9E5, ...), depth/stencil and unsized formats
                // have no component-array shadow layout (packed ones are handled below).
                return false;
            }
        }

        // Packed internal formats whose shadow bytes hold the ES upload word directly
        // (GL_UNSIGNED_INT_2_10_10_10_REV / 5_9_9_9_REV / 10F_11F_11F_REV encoding, 4 bytes/texel).
        enum class PackedInternalKind {
            UNorm2101010Rev, // GL_RGB10_A2
            UInt2101010Rev,  // GL_RGB10_A2UI
            FloatR11G11B10,  // GL_R11F_G11F_B10F
            FloatRGB9E5,     // GL_RGB9_E5
        };

        struct InternalPackedLayout {
            PackedInternalKind kind;
            Int channelCount;
            Bool isInteger;
        };

        Bool GetInternalPackedLayout(TextureInternalFormat internal, InternalPackedLayout& out) {
            switch (internal) {
            case TextureInternalFormat::RGB10A2:
                out = {PackedInternalKind::UNorm2101010Rev, 4, false};
                return true;
            case TextureInternalFormat::RGB10A2UI:
                out = {PackedInternalKind::UInt2101010Rev, 4, true};
                return true;
            case TextureInternalFormat::R11FG11FB10F:
                out = {PackedInternalKind::FloatR11G11B10, 3, false};
                return true;
            case TextureInternalFormat::RGB9E5:
                out = {PackedInternalKind::FloatRGB9E5, 3, false};
                return true;
            default:
                return false;
            }
        }

        // The one client (format, type) pair whose word is bit-identical to the packed internal
        // word, if any. Everything else has to go through the decode/encode conversion.
        Bool IsRawPackedPixelPair(PackedInternalKind kind, TextureInputFormat format,
                                  TexturePixelDataType type) {
            switch (kind) {
            case PackedInternalKind::UNorm2101010Rev:
                return format == TextureInputFormat::RGBA && type == TexturePixelDataType::UnsignedInt2101010Rev;
            case PackedInternalKind::UInt2101010Rev:
                return format == TextureInputFormat::RGBAInteger &&
                       type == TexturePixelDataType::UnsignedInt2101010Rev;
            case PackedInternalKind::FloatR11G11B10:
                return format == TextureInputFormat::RGB && type == TexturePixelDataType::UnsignedInt101111Rev;
            case PackedInternalKind::FloatRGB9E5:
                return format == TextureInputFormat::RGB && type == TexturePixelDataType::UnsignedInt5999Rev;
            default:
                return false;
            }
        }

        Uint32 EncodePackedInternalWordFloat(PackedInternalKind kind, const Float rgba[4]) {
            switch (kind) {
            case PackedInternalKind::UNorm2101010Rev: {
                const auto field = [](Float v, Uint32 maxValue) {
                    return static_cast<Uint32>(std::llround(std::clamp(v, 0.0f, 1.0f) * static_cast<Float>(maxValue)));
                };
                return field(rgba[0], 1023u) | (field(rgba[1], 1023u) << 10) | (field(rgba[2], 1023u) << 20) |
                       (field(rgba[3], 3u) << 30);
            }
            case PackedInternalKind::FloatR11G11B10:
                return EncodeFloatToUnsignedF11(rgba[0]) | (EncodeFloatToUnsignedF11(rgba[1]) << 11) |
                       (EncodeFloatToUnsignedF10(rgba[2]) << 22);
            case PackedInternalKind::FloatRGB9E5:
                return EncodeSharedExponentRGB9E5(rgba);
            default:
                return 0;
            }
        }

        Uint32 EncodePackedInternalWordInt(PackedInternalKind kind, const Int64 rgba[4]) {
            if (kind != PackedInternalKind::UInt2101010Rev) {
                return 0;
            }
            const auto field = [](Int64 v, Int64 maxValue) {
                return static_cast<Uint32>(std::clamp<Int64>(v, 0, maxValue));
            };
            return field(rgba[0], 1023) | (field(rgba[1], 1023) << 10) | (field(rgba[2], 1023) << 20) |
                   (field(rgba[3], 3) << 30);
        }

        struct UnpackChannelMapping {
            Int formatPosition[4]; // position of R,G,B,A within the input format's component list; -1 = missing
            Int channelCount;
            Bool isInteger;
        };

        Bool GetUnpackChannelMapping(TextureInputFormat format, UnpackChannelMapping& out) {
            switch (format) {
            case TextureInputFormat::Red:          out = {{0, -1, -1, -1}, 1, false}; return true;
            case TextureInputFormat::RInteger:     out = {{0, -1, -1, -1}, 1, true};  return true;
            case TextureInputFormat::Green:        out = {{-1, 0, -1, -1}, 1, false}; return true;
            case TextureInputFormat::GreenInteger: out = {{-1, 0, -1, -1}, 1, true};  return true;
            case TextureInputFormat::Blue:         out = {{-1, -1, 0, -1}, 1, false}; return true;
            case TextureInputFormat::BlueInteger:  out = {{-1, -1, 0, -1}, 1, true};  return true;
            case TextureInputFormat::Alpha:        out = {{-1, -1, -1, 0}, 1, false}; return true;
            case TextureInputFormat::AlphaInteger: out = {{-1, -1, -1, 0}, 1, true};  return true;
            case TextureInputFormat::RG:           out = {{0, 1, -1, -1}, 2, false};  return true;
            case TextureInputFormat::RGInteger:    out = {{0, 1, -1, -1}, 2, true};   return true;
            case TextureInputFormat::RGB:          out = {{0, 1, 2, -1}, 3, false};   return true;
            case TextureInputFormat::RGBInteger:   out = {{0, 1, 2, -1}, 3, true};    return true;
            case TextureInputFormat::BGR:          out = {{2, 1, 0, -1}, 3, false};   return true;
            case TextureInputFormat::BGRInteger:   out = {{2, 1, 0, -1}, 3, true};    return true;
            case TextureInputFormat::RGBA:         out = {{0, 1, 2, 3}, 4, false};    return true;
            case TextureInputFormat::RGBAInteger:  out = {{0, 1, 2, 3}, 4, true};     return true;
            case TextureInputFormat::BGRA:         out = {{2, 1, 0, 3}, 4, false};    return true;
            case TextureInputFormat::BGRAInteger:  out = {{2, 1, 0, 3}, 4, true};     return true;
            // A depth value converts like a single normalized/float channel.
            case TextureInputFormat::DepthComponent: out = {{0, -1, -1, -1}, 1, false}; return true;
            default:
                return false; // stencil / packed depth-stencil / unknown
            }
        }

        struct PackedTypeLayout {
            Int fieldCount;
            Int width[4]; // bit width of each format component, in component order
            Int totalBits;
            Bool reversed; // *_REV: the first format component sits in the least significant bits
        };

        Bool GetPackedTypeLayout(TexturePixelDataType type, PackedTypeLayout& out) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte332:      out = {3, {3, 3, 2, 0}, 8, false};  return true;
            case TexturePixelDataType::UnsignedByte233Rev:   out = {3, {3, 3, 2, 0}, 8, true};   return true;
            case TexturePixelDataType::UnsignedShort565:     out = {3, {5, 6, 5, 0}, 16, false}; return true;
            case TexturePixelDataType::UnsignedShort565Rev:  out = {3, {5, 6, 5, 0}, 16, true};  return true;
            case TexturePixelDataType::UnsignedShort4444:    out = {4, {4, 4, 4, 4}, 16, false}; return true;
            case TexturePixelDataType::UnsignedShort4444Rev: out = {4, {4, 4, 4, 4}, 16, true};  return true;
            case TexturePixelDataType::UnsignedShort5551:    out = {4, {5, 5, 5, 1}, 16, false}; return true;
            case TexturePixelDataType::UnsignedShort1555Rev: out = {4, {5, 5, 5, 1}, 16, true};  return true;
            case TexturePixelDataType::UnsignedInt8888:      out = {4, {8, 8, 8, 8}, 32, false}; return true;
            case TexturePixelDataType::UnsignedInt8888Rev:   out = {4, {8, 8, 8, 8}, 32, true};  return true;
            case TexturePixelDataType::UnsignedInt1010102:   out = {4, {10, 10, 10, 2}, 32, false}; return true;
            case TexturePixelDataType::UnsignedInt2101010Rev: out = {4, {10, 10, 10, 2}, 32, true}; return true;
            default:
                return false; // shared-exponent / packed-float / depth-stencil types stay on the legacy path
            }
        }

        // Base data types whose in-memory encoding equals a shadow component encoding (fast-path check).
        Bool GetDirectShadowComponentForType(TexturePixelDataType type, Bool isInteger, ShadowComponent& out) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                out = isInteger ? ShadowComponent::UInt8 : ShadowComponent::UNorm8;
                return true;
            case TexturePixelDataType::Byte:
                out = isInteger ? ShadowComponent::Int8 : ShadowComponent::SNorm8;
                return true;
            case TexturePixelDataType::UnsignedShort:
                out = isInteger ? ShadowComponent::UInt16 : ShadowComponent::UNorm16;
                return true;
            case TexturePixelDataType::Short:
                out = isInteger ? ShadowComponent::Int16 : ShadowComponent::SNorm16;
                return true;
            case TexturePixelDataType::UnsignedInt:
                out = isInteger ? ShadowComponent::UInt32 : ShadowComponent::UNorm32;
                return true;
            case TexturePixelDataType::Int:
                if (!isInteger) return false;
                out = ShadowComponent::Int32;
                return true;
            case TexturePixelDataType::HalfFloat:
                if (isInteger) return false;
                out = ShadowComponent::Half;
                return true;
            case TexturePixelDataType::Float:
                if (isInteger) return false;
                out = ShadowComponent::Float32;
                return true;
            default:
                return false;
            }
        }

        Bool IsIdentityChannelOrder(const UnpackChannelMapping& mapping) {
            for (Int i = 0; i < 4; ++i) {
                const Int expected = i < mapping.channelCount ? i : -1;
                if (mapping.formatPosition[i] != expected) return false;
            }
            return true;
        }

        struct UnpackConversionSpec {
            UnpackChannelMapping mapping;
            InternalShadowLayout internal;
            PackedTypeLayout packed;
            Bool isPacked;
            TexturePixelDataType type;
            SizeT inputPixelSize;
            SizeT swapGroupSize; // UNPACK_SWAP_BYTES group: packed word size, or the component size
            SizeT internalPixelSize;
            Bool internalIsPacked;
            InternalPackedLayout internalPacked;
        };

        Bool IsValidUnpackPixelPair(TextureInputFormat format, TexturePixelDataType type) {
            UnpackChannelMapping mapping{};
            if (!GetUnpackChannelMapping(format, mapping)) return false;

            PackedTypeLayout packed{};
            if (GetPackedTypeLayout(type, packed)) {
                return packed.fieldCount == mapping.channelCount;
            }

            switch (type) {
            case TexturePixelDataType::UnsignedInt5999Rev:
            case TexturePixelDataType::UnsignedInt101111Rev:
                return !mapping.isInteger && mapping.channelCount == 3;
            default: {
                ShadowComponent component{};
                return GetDirectShadowComponentForType(type, mapping.isInteger, component);
            }
            }
        }

        // Returns true when the (format, type) -> internal-format upload needs a per-texel conversion;
        // returns false both for layouts that already match the shadow bytes (memcpy fast path) and for
        // combinations the converter does not support (legacy copy behavior).
        Bool GetUnpackConversionSpec(TextureInternalFormat internal, TextureInputFormat format,
                                     TexturePixelDataType type, UnpackConversionSpec& out) {
            InternalShadowLayout layout{};
            InternalPackedLayout packedInternal{};
            const Bool hasComponentLayout = GetInternalShadowLayout(internal, layout);
            const Bool hasPackedInternal = !hasComponentLayout && GetInternalPackedLayout(internal, packedInternal);
            if (!hasComponentLayout && !hasPackedInternal) return false;
            const Bool internalIsInteger = hasComponentLayout ? layout.isInteger : packedInternal.isInteger;
            const Int internalChannelCount = hasComponentLayout ? layout.channelCount : packedInternal.channelCount;

            UnpackChannelMapping mapping{};
            if (!GetUnpackChannelMapping(format, mapping)) return false;
            if (mapping.isInteger != internalIsInteger) return false; // rejected upstream; stay safe

            PackedTypeLayout packed{};
            const Bool isPacked = GetPackedTypeLayout(type, packed);
            if (isPacked) {
                if (packed.fieldCount != mapping.channelCount) return false;
                // Byte layout already equals the RGBA8 shadow layout on little-endian.
                if (internal == TextureInternalFormat::RGBA8 && format == TextureInputFormat::RGBA &&
                    type == TexturePixelDataType::UnsignedInt8888Rev) {
                    return false;
                }
                // The client word already equals the packed internal word (memcpy fast path).
                if (hasPackedInternal && IsRawPackedPixelPair(packedInternal.kind, format, type)) {
                    return false;
                }
            } else {
                ShadowComponent direct{};
                const Bool hasDirect = GetDirectShadowComponentForType(type, mapping.isInteger, direct);
                switch (type) {
                case TexturePixelDataType::UnsignedByte:
                case TexturePixelDataType::Byte:
                case TexturePixelDataType::UnsignedShort:
                case TexturePixelDataType::Short:
                case TexturePixelDataType::UnsignedInt:
                case TexturePixelDataType::Int:
                    break;
                case TexturePixelDataType::Float:
                case TexturePixelDataType::HalfFloat:
                    if (mapping.isInteger) return false; // rejected upstream
                    break;
                case TexturePixelDataType::UnsignedInt5999Rev:
                case TexturePixelDataType::UnsignedInt101111Rev:
                    // Packed-float RGB source words (decoded in ConvertUnpackRow); only pair with
                    // GL_RGB, which the state layer already enforces.
                    if (mapping.isInteger || mapping.channelCount != 3) return false;
                    // The client word already equals the packed internal word.
                    if (hasPackedInternal && IsRawPackedPixelPair(packedInternal.kind, format, type)) {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
                if (hasComponentLayout && hasDirect && direct == layout.component &&
                    mapping.channelCount == layout.channelCount && IsIdentityChannelOrder(mapping)) {
                    return false; // input already matches the shadow layout
                }
            }

            out.mapping = mapping;
            out.internal = hasComponentLayout
                               ? layout
                               : InternalShadowLayout{internalChannelCount, ShadowComponent::UNorm8, internalIsInteger};
            out.packed = packed;
            out.isPacked = isPacked;
            out.type = type;
            out.inputPixelSize = GetInputBytesPerPixel(format, type);
            const Bool isPackedFloatWord = type == TexturePixelDataType::UnsignedInt5999Rev ||
                                           type == TexturePixelDataType::UnsignedInt101111Rev;
            out.swapGroupSize = isPacked ? static_cast<SizeT>(packed.totalBits / 8)
                                         : (isPackedFloatWord ? 4 : GetBaseTexturePixelDataTypeSize(type));
            out.internalIsPacked = hasPackedInternal;
            out.internalPacked = packedInternal;
            out.internalPixelSize =
                hasPackedInternal ? 4
                                  : static_cast<SizeT>(layout.channelCount) * GetShadowComponentSize(layout.component);
            return true;
        }

        Float DecodeComponentToFloat(const Uint8* p, TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                return static_cast<Float>(*p) / 255.0f;
            case TexturePixelDataType::Byte: {
                Int8 v;
                Memcpy(&v, p, sizeof(v));
                return std::max(static_cast<Float>(v) / 127.0f, -1.0f);
            }
            case TexturePixelDataType::UnsignedShort: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return static_cast<Float>(v) / 65535.0f;
            }
            case TexturePixelDataType::Short: {
                Int16 v;
                Memcpy(&v, p, sizeof(v));
                return std::max(static_cast<Float>(v) / 32767.0f, -1.0f);
            }
            case TexturePixelDataType::UnsignedInt: {
                Uint32 v;
                Memcpy(&v, p, sizeof(v));
                return static_cast<Float>(static_cast<Double>(v) / 4294967295.0);
            }
            case TexturePixelDataType::Int: {
                Int32 v;
                Memcpy(&v, p, sizeof(v));
                return static_cast<Float>(std::max(static_cast<Double>(v) / 2147483647.0, -1.0));
            }
            case TexturePixelDataType::HalfFloat: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return DecodeHalfBitsToFloat(v);
            }
            case TexturePixelDataType::Float: {
                Float v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            default:
                return 0.0f;
            }
        }

        Int64 DecodeComponentToInt(const Uint8* p, TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                return *p;
            case TexturePixelDataType::Byte: {
                Int8 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case TexturePixelDataType::UnsignedShort: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case TexturePixelDataType::Short: {
                Int16 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case TexturePixelDataType::UnsignedInt: {
                Uint32 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case TexturePixelDataType::Int: {
                Int32 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            default:
                return 0;
            }
        }

        Uint32 ReadPackedWord(const Uint8* p, Int totalBits) {
            switch (totalBits) {
            case 8:
                return *p;
            case 16: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            default: {
                Uint32 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            }
        }

        Uint32 ExtractPackedField(Uint32 word, const PackedTypeLayout& packed, Int position, Int& outWidth) {
            Int shift;
            if (packed.reversed) {
                shift = 0;
                for (Int i = 0; i < position; ++i) shift += packed.width[i];
            } else {
                shift = packed.totalBits;
                for (Int i = 0; i <= position; ++i) shift -= packed.width[i];
            }
            outWidth = packed.width[position];
            const Uint32 mask = (1u << outWidth) - 1u;
            return (word >> shift) & mask;
        }

        void EncodeShadowComponentFloat(Uint8* dst, ShadowComponent component, Float v) {
            switch (component) {
            case ShadowComponent::UNorm8: {
                const auto out = static_cast<Uint8>(std::llround(std::clamp(v, 0.0f, 1.0f) * 255.0));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::SNorm8: {
                const auto out = static_cast<Int8>(std::llround(std::clamp(v, -1.0f, 1.0f) * 127.0));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::UNorm16: {
                const auto out = static_cast<Uint16>(std::llround(std::clamp(v, 0.0f, 1.0f) * 65535.0));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::SNorm16: {
                const auto out = static_cast<Int16>(std::llround(std::clamp(v, -1.0f, 1.0f) * 32767.0));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::Half: {
                const Uint16 out = EncodeFloatToHalfBits(v);
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::Float32:
                Memcpy(dst, &v, sizeof(v));
                break;
            case ShadowComponent::UNorm32: {
                const auto out = static_cast<Uint32>(
                    std::llround(static_cast<double>(std::clamp(v, 0.0f, 1.0f)) * 4294967295.0));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            default:
                break; // integer components never reach the float encoder
            }
        }

        void EncodeShadowComponentInt(Uint8* dst, ShadowComponent component, Int64 v) {
            switch (component) {
            case ShadowComponent::UInt8: {
                const auto out = static_cast<Uint8>(std::clamp<Int64>(v, 0, 255));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::Int8: {
                const auto out = static_cast<Int8>(std::clamp<Int64>(v, -128, 127));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::UInt16: {
                const auto out = static_cast<Uint16>(std::clamp<Int64>(v, 0, 65535));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::Int16: {
                const auto out = static_cast<Int16>(std::clamp<Int64>(v, -32768, 32767));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::UInt32: {
                const auto out = static_cast<Uint32>(std::clamp<Int64>(v, 0, 4294967295LL));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            case ShadowComponent::Int32: {
                const auto out = static_cast<Int32>(std::clamp<Int64>(v, -2147483648LL, 2147483647LL));
                Memcpy(dst, &out, sizeof(out));
                break;
            }
            default:
                break; // float components never reach the integer encoder
            }
        }

        void ConvertUnpackRow(const Uint8* src, Uint8* dst, SizeT pixelCount, const UnpackConversionSpec& conv) {
            const SizeT dstComponentSize = GetShadowComponentSize(conv.internal.component);
            const SizeT srcComponentSize = conv.isPacked ? 0 : GetBaseTexturePixelDataTypeSize(conv.type);
            for (SizeT i = 0; i < pixelCount; ++i) {
                const Uint8* s = src + i * conv.inputPixelSize;
                Uint8* d = dst + i * conv.internalPixelSize;
                if (conv.internal.isInteger) {
                    Int64 rgba[4] = {0, 0, 0, 1};
                    if (conv.isPacked) {
                        const Uint32 word = ReadPackedWord(s, conv.packed.totalBits);
                        for (Int ch = 0; ch < 4; ++ch) {
                            const Int pos = conv.mapping.formatPosition[ch];
                            if (pos < 0) continue;
                            Int width = 0;
                            rgba[ch] = ExtractPackedField(word, conv.packed, pos, width);
                        }
                    } else {
                        for (Int ch = 0; ch < 4; ++ch) {
                            const Int pos = conv.mapping.formatPosition[ch];
                            if (pos < 0) continue;
                            rgba[ch] = DecodeComponentToInt(s + static_cast<SizeT>(pos) * srcComponentSize, conv.type);
                        }
                    }
                    if (conv.internalIsPacked) {
                        const Uint32 word = EncodePackedInternalWordInt(conv.internalPacked.kind, rgba);
                        Memcpy(d, &word, sizeof(word));
                        continue;
                    }
                    for (Int ch = 0; ch < conv.internal.channelCount; ++ch) {
                        EncodeShadowComponentInt(d + static_cast<SizeT>(ch) * dstComponentSize,
                                                 conv.internal.component, rgba[ch]);
                    }
                } else {
                    Float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    if (conv.type == TexturePixelDataType::UnsignedInt5999Rev ||
                        conv.type == TexturePixelDataType::UnsignedInt101111Rev) {
                        // Packed-float RGB source word: decode the shared-exponent / small-float fields.
                        Uint32 word;
                        Memcpy(&word, s, sizeof(word));
                        Float comps[3];
                        if (conv.type == TexturePixelDataType::UnsignedInt5999Rev) {
                            DecodeSharedExponentRGB9E5(word, comps);
                        } else {
                            comps[0] = DecodeUnsignedF11ToFloat(word & 0x7FFu);
                            comps[1] = DecodeUnsignedF11ToFloat((word >> 11) & 0x7FFu);
                            comps[2] = DecodeUnsignedF10ToFloat((word >> 22) & 0x3FFu);
                        }
                        for (Int ch = 0; ch < 4; ++ch) {
                            const Int pos = conv.mapping.formatPosition[ch];
                            if (pos < 0 || pos >= 3) continue;
                            rgba[ch] = comps[pos];
                        }
                    } else if (conv.isPacked) {
                        const Uint32 word = ReadPackedWord(s, conv.packed.totalBits);
                        for (Int ch = 0; ch < 4; ++ch) {
                            const Int pos = conv.mapping.formatPosition[ch];
                            if (pos < 0) continue;
                            Int width = 0;
                            const Uint32 field = ExtractPackedField(word, conv.packed, pos, width);
                            rgba[ch] = static_cast<Float>(field) / static_cast<Float>((1u << width) - 1u);
                        }
                    } else {
                        for (Int ch = 0; ch < 4; ++ch) {
                            const Int pos = conv.mapping.formatPosition[ch];
                            if (pos < 0) continue;
                            rgba[ch] =
                                DecodeComponentToFloat(s + static_cast<SizeT>(pos) * srcComponentSize, conv.type);
                        }
                    }
                    if (conv.internalIsPacked) {
                        const Uint32 word = EncodePackedInternalWordFloat(conv.internalPacked.kind, rgba);
                        Memcpy(d, &word, sizeof(word));
                        continue;
                    }
                    for (Int ch = 0; ch < conv.internal.channelCount; ++ch) {
                        EncodeShadowComponentFloat(d + static_cast<SizeT>(ch) * dstComponentSize,
                                                   conv.internal.component, rgba[ch]);
                    }
                }
            }
        }
    } // namespace

    Bool IsRawPackedPixelTransfer(TextureInternalFormat internalFormat, TextureInputFormat clientFormat,
                                  TexturePixelDataType clientType) {
        InternalPackedLayout packedInternal{};
        if (!GetInternalPackedLayout(internalFormat, packedInternal)) {
            return false;
        }
        return IsRawPackedPixelPair(packedInternal.kind, clientFormat, clientType);
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

        // GL_DEPTH_COMPONENT client data may populate a packed depth-stencil internal
        // format (the stencil half becomes zero); the generic channel converter cannot
        // express the packed shadow words, so convert here.
        const Bool packedDepthStencilInternal = targetInternalFormat == TextureInternalFormat::Depth24Stencil8 ||
            targetInternalFormat == TextureInternalFormat::DepthStencil ||
            targetInternalFormat == TextureInternalFormat::Depth32FStencil8;
        if (!isBitmap && packedDepthStencilInternal && textureInputFormat == TextureInputFormat::DepthComponent &&
            (inputDataType == TexturePixelDataType::Float || inputDataType == TexturePixelDataType::UnsignedInt ||
             inputDataType == TexturePixelDataType::UnsignedShort)) {
            const Bool floatShadow = targetInternalFormat == TextureInternalFormat::Depth32FStencil8;
            const SizeT outPixelSize = floatShadow ? 8 : 4;
            outSize = static_cast<SizeT>(width) * height * std::max(depth, 1) * outPixelSize;
            Uint8* outputPixels = static_cast<Uint8*>(malloc(outSize));
            if (!outputPixels) {
                outSize = 0;
                return nullptr;
            }
            const Uint8* srcBase = static_cast<const Uint8*>(inputPixels) +
                static_cast<SizeT>(params.SkipImages) * static_cast<SizeT>(effectiveHeight) * inputRowStride +
                static_cast<SizeT>(params.SkipRows) * inputRowStride +
                static_cast<SizeT>(params.SkipPixels) * pixelSize;
            Uint8* dst = outputPixels;
            for (Int z = 0; z < std::max(depth, 1); ++z) {
                for (Int y = 0; y < height; ++y) {
                    const Uint8* srcRow = srcBase +
                        static_cast<SizeT>(z) * static_cast<SizeT>(effectiveHeight) * inputRowStride +
                        static_cast<SizeT>(y) * inputRowStride;
                    for (Int x = 0; x < width; ++x) {
                        Float depthValue = 0.0f;
                        if (inputDataType == TexturePixelDataType::Float) {
                            Memcpy(&depthValue, srcRow + static_cast<SizeT>(x) * 4, sizeof(depthValue));
                        } else if (inputDataType == TexturePixelDataType::UnsignedInt) {
                            Uint32 raw = 0;
                            Memcpy(&raw, srcRow + static_cast<SizeT>(x) * 4, sizeof(raw));
                            depthValue = static_cast<Float>(static_cast<double>(raw) / 4294967295.0);
                        } else {
                            Uint16 raw = 0;
                            Memcpy(&raw, srcRow + static_cast<SizeT>(x) * 2, sizeof(raw));
                            depthValue = static_cast<Float>(raw) / 65535.0f;
                        }
                        if (floatShadow) {
                            const Uint32 stencilWord = 0;
                            Memcpy(dst, &depthValue, sizeof(depthValue));
                            Memcpy(dst + 4, &stencilWord, sizeof(stencilWord));
                            dst += 8;
                        } else {
                            const Uint32 depth24 = static_cast<Uint32>(
                                std::llround(static_cast<double>(std::clamp(depthValue, 0.0f, 1.0f)) * 16777215.0));
                            const Uint32 word = depth24 << 8;
                            Memcpy(dst, &word, sizeof(word));
                            dst += 4;
                        }
                    }
                }
            }
            return outputPixels;
        }

        UnpackConversionSpec conversion{};
        const Bool needConversion =
            !isBitmap && GetUnpackConversionSpec(targetInternalFormat, textureInputFormat, inputDataType, conversion);
        const SizeT outputPixelSize = needConversion ? conversion.internalPixelSize : pixelSize;
        const SizeT outputRowStride = static_cast<SizeT>(width) * outputPixelSize;

        const Int startX = params.SkipPixels;
        const Int startY = params.SkipRows;
        const Int startZ = params.SkipImages;

        const Int copyWidth = width;
        const Int copyHeight = height;
        const Int copyDepth = depth;

        MGLOG_D("%s: start at: (%d, %d, %d), copy size: (%d, %d, %d), i/o row stride: (%d, %dx%d), convert: %d",
                __func__, startX, startY, startZ, copyWidth, copyHeight, copyDepth, inputRowStride, width,
                outputPixelSize, needConversion ? 1 : 0);

        if (copyWidth <= 0 || copyHeight <= 0 || copyDepth <= 0) {
            outSize = 0;
            return nullptr;
        }

        outSize = static_cast<SizeT>(copyWidth) * copyHeight * copyDepth * outputPixelSize;
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

        const Bool isByteType =
            (inputDataType == TexturePixelDataType::UnsignedByte || inputDataType == TexturePixelDataType::Byte);
        // UNPACK_SWAP_BYTES applies to the input elements (packed word / component) before conversion.
        const Bool conversionSwapsBytes = needConversion && params.SwapBytes && conversion.swapGroupSize > 1;
        Vector<Uint8> swapScratch;
        if (conversionSwapsBytes) {
            swapScratch.resize(static_cast<SizeT>(copyWidth) * pixelSize);
        }
        for (Int z = 0; z < copyDepth; ++z) {
            const Uint8* layerSrc = src;
            Uint8* layerDst = dst;

            for (Int y = 0; y < copyHeight; ++y) {
                if (needConversion) {
                    const Uint8* rowSrc = layerSrc;
                    if (conversionSwapsBytes) {
                        Memcpy(swapScratch.data(), layerSrc, static_cast<SizeT>(copyWidth) * pixelSize);
                        const SizeT groupCount = static_cast<SizeT>(copyWidth) * pixelSize / conversion.swapGroupSize;
                        SwapBytes(swapScratch.data(), conversion.swapGroupSize, groupCount);
                        rowSrc = swapScratch.data();
                    }
                    ConvertUnpackRow(rowSrc, layerDst, static_cast<SizeT>(copyWidth), conversion);
                } else {
                    Memcpy(layerDst, layerSrc, static_cast<SizeT>(copyWidth) * pixelSize);

                    if (params.SwapBytes && !isByteType) {
                        // GL_UNPACK_SWAP_BYTES swaps within each element (component or packed
                        // word), never across a whole multi-component pixel.
                        SizeT swapGroup = GetSizedTexturePixelDataTypeSize(inputDataType);
                        if (swapGroup == 0) swapGroup = GetBaseTexturePixelDataTypeSize(inputDataType);
                        if (swapGroup > 1) {
                            MGLOG_D("%s: SwapBytes (group %d)", __func__, static_cast<Int>(swapGroup));
                            SwapBytes(layerDst, swapGroup,
                                      static_cast<SizeT>(copyWidth) * pixelSize / swapGroup);
                        }
                    }

                    if (params.LSBFirst && isBitmap) {
                        MGLOG_D("%s: LSBFirst", __func__);
                        ProcessLSBFirst(layerDst, static_cast<SizeT>(copyWidth), 1);
                    }
                }

                layerSrc += inputRowStride;
                layerDst += outputRowStride;
            }

            src += static_cast<SizeT>(effectiveHeight) * inputRowStride;
            dst += static_cast<SizeT>(copyHeight) * outputRowStride;
        }

        return outputPixels;
    }

    Bool ConvertOnePixelToInternal(TextureInternalFormat targetInternalFormat,
                                   TextureInputFormat textureInputFormat,
                                   TexturePixelDataType inputDataType,
                                   const void* inputPixel,
                                   Vector<Uint8>& outputPixel) {
        outputPixel.clear();
        if (inputPixel == nullptr || !IsValidUnpackPixelPair(textureInputFormat, inputDataType)) return false;

        PixelStoreParameters params{};
        params.Alignment = 1;
        SizeT convertedSize = 0;
        void* converted = ProcessTexturePixelsDataUnpack(
            inputPixel, params, targetInternalFormat, textureInputFormat, inputDataType, {1, 1, 1}, false,
            convertedSize);
        const SizeT expectedSize = MG_Util::GetSizedInternalFormatSizeInBytes(targetInternalFormat);
        if (converted == nullptr || convertedSize != expectedSize || expectedSize == 0) {
            if (converted != nullptr) free(converted);
            return false;
        }

        outputPixel.resize(convertedSize);
        Memcpy(outputPixel.data(), converted, convertedSize);
        free(converted);
        return true;
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

    namespace {
        Float DecodeShadowComponentToFloat(const Uint8* p, ShadowComponent component) {
            switch (component) {
            case ShadowComponent::UNorm8:
                return static_cast<Float>(*p) / 255.0f;
            case ShadowComponent::SNorm8: {
                Int8 v;
                Memcpy(&v, p, sizeof(v));
                return std::max(static_cast<Float>(v) / 127.0f, -1.0f);
            }
            case ShadowComponent::UNorm16: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return static_cast<Float>(v) / 65535.0f;
            }
            case ShadowComponent::SNorm16: {
                Int16 v;
                Memcpy(&v, p, sizeof(v));
                return std::max(static_cast<Float>(v) / 32767.0f, -1.0f);
            }
            case ShadowComponent::Half: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return DecodeHalfBitsToFloat(v);
            }
            case ShadowComponent::Float32: {
                Float v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case ShadowComponent::UNorm32: {
                Uint32 v;
                Memcpy(&v, p, sizeof(v));
                return static_cast<Float>(static_cast<double>(v) / 4294967295.0);
            }
            default:
                return 0.0f;
            }
        }

        Int64 DecodeShadowComponentToInt(const Uint8* p, ShadowComponent component) {
            switch (component) {
            case ShadowComponent::UInt8:
                return *p;
            case ShadowComponent::Int8: {
                Int8 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case ShadowComponent::UInt16: {
                Uint16 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case ShadowComponent::Int16: {
                Int16 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case ShadowComponent::UInt32: {
                Uint32 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            case ShadowComponent::Int32: {
                Int32 v;
                Memcpy(&v, p, sizeof(v));
                return v;
            }
            default:
                return 0;
            }
        }
    } // namespace

    Bool DecodeShadowDataToWideRGBA(TextureInternalFormat internalFormat, const void* src, SizeT pixelCount,
                                    Vector<Uint8>& outWide, Bool& outIsInteger, Bool& outIsSigned) {
        if (!src) return false;
        const Uint8* srcBytes = static_cast<const Uint8*>(src);

        InternalShadowLayout layout{};
        if (GetInternalShadowLayout(internalFormat, layout)) {
            const SizeT componentSize = GetShadowComponentSize(layout.component);
            const SizeT srcPixelSize = static_cast<SizeT>(layout.channelCount) * componentSize;
            outIsInteger = layout.isInteger;
            outIsSigned = layout.component == ShadowComponent::Int8 || layout.component == ShadowComponent::Int16 ||
                          layout.component == ShadowComponent::Int32;
            outWide.resize(pixelCount * 16);
            if (layout.isInteger) {
                auto* dst = reinterpret_cast<Uint32*>(outWide.data());
                for (SizeT i = 0; i < pixelCount; ++i) {
                    const Uint8* s = srcBytes + i * srcPixelSize;
                    for (Int ch = 0; ch < 4; ++ch) {
                        Int64 v = ch == 3 ? 1 : 0;
                        if (ch < layout.channelCount) {
                            v = DecodeShadowComponentToInt(s + static_cast<SizeT>(ch) * componentSize,
                                                           layout.component);
                        }
                        if (outIsSigned) {
                            const auto out = static_cast<Int32>(v);
                            Memcpy(&dst[i * 4 + ch], &out, sizeof(out));
                        } else {
                            dst[i * 4 + ch] = static_cast<Uint32>(v);
                        }
                    }
                }
            } else {
                auto* dst = reinterpret_cast<Float*>(outWide.data());
                for (SizeT i = 0; i < pixelCount; ++i) {
                    const Uint8* s = srcBytes + i * srcPixelSize;
                    for (Int ch = 0; ch < 4; ++ch) {
                        Float v = ch == 3 ? 1.0f : 0.0f;
                        if (ch < layout.channelCount) {
                            v = DecodeShadowComponentToFloat(s + static_cast<SizeT>(ch) * componentSize,
                                                             layout.component);
                        }
                        dst[i * 4 + ch] = v;
                    }
                }
            }
            return true;
        }

        InternalPackedLayout packedInternal{};
        if (GetInternalPackedLayout(internalFormat, packedInternal)) {
            outIsInteger = packedInternal.isInteger;
            outIsSigned = false;
            outWide.resize(pixelCount * 16);
            if (packedInternal.isInteger) {
                auto* dst = reinterpret_cast<Uint32*>(outWide.data());
                for (SizeT i = 0; i < pixelCount; ++i) {
                    Uint32 word;
                    Memcpy(&word, srcBytes + i * 4, sizeof(word));
                    dst[i * 4 + 0] = word & 0x3FFu;
                    dst[i * 4 + 1] = (word >> 10) & 0x3FFu;
                    dst[i * 4 + 2] = (word >> 20) & 0x3FFu;
                    dst[i * 4 + 3] = (word >> 30) & 0x3u;
                }
            } else {
                auto* dst = reinterpret_cast<Float*>(outWide.data());
                for (SizeT i = 0; i < pixelCount; ++i) {
                    Uint32 word;
                    Memcpy(&word, srcBytes + i * 4, sizeof(word));
                    switch (packedInternal.kind) {
                    case PackedInternalKind::UNorm2101010Rev:
                        dst[i * 4 + 0] = static_cast<Float>(word & 0x3FFu) / 1023.0f;
                        dst[i * 4 + 1] = static_cast<Float>((word >> 10) & 0x3FFu) / 1023.0f;
                        dst[i * 4 + 2] = static_cast<Float>((word >> 20) & 0x3FFu) / 1023.0f;
                        dst[i * 4 + 3] = static_cast<Float>((word >> 30) & 0x3u) / 3.0f;
                        break;
                    case PackedInternalKind::FloatR11G11B10:
                        dst[i * 4 + 0] = DecodeUnsignedF11ToFloat(word & 0x7FFu);
                        dst[i * 4 + 1] = DecodeUnsignedF11ToFloat((word >> 11) & 0x7FFu);
                        dst[i * 4 + 2] = DecodeUnsignedF10ToFloat((word >> 22) & 0x3FFu);
                        dst[i * 4 + 3] = 1.0f;
                        break;
                    case PackedInternalKind::FloatRGB9E5: {
                        Float rgb[3];
                        DecodeSharedExponentRGB9E5(word, rgb);
                        dst[i * 4 + 0] = rgb[0];
                        dst[i * 4 + 1] = rgb[1];
                        dst[i * 4 + 2] = rgb[2];
                        dst[i * 4 + 3] = 1.0f;
                        break;
                    }
                    default:
                        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 0.0f;
                        dst[i * 4 + 3] = 1.0f;
                        break;
                    }
                }
            }
            return true;
        }

        return false;
    }
} // namespace MobileGL::MG_Util::PixelStoreProcessor
