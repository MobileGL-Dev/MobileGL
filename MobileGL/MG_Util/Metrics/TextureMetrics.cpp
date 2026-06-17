// MobileGL - MobileGL/MG_Util/Metrics/TextureMetrics.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureMetrics.h"
#include "Defines.h"
#include "MG_Util/Math/VectorTypes.h"

namespace MobileGL {
    namespace MG_Util {
        SizeT GetSizedInternalFormatSizeInBytes(TextureInternalFormat internal) {
            switch (internal) {
            case TextureInternalFormat::R8:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R3G3B2:
                return 1;

            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::DepthComponent16:
                return 2;

            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::DepthComponent24:
                return 3;

            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGB5A1:
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::SRGB8Alpha8:
            case TextureInternalFormat::RGB10A2:
            case TextureInternalFormat::RGB10A2UI:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::DepthStencil:
            case TextureInternalFormat::Depth24Stencil8:

            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG16F:
                return 4;

            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
                return 6;

            case TextureInternalFormat::RGBA12:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
                return 8;

            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                return 12;

            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::Depth32FStencil8:
                return 16;

            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
                return 4;

            default:
                return 0;
            }
        }

        SizeT GetBaseInputFormatComponentCount(TextureInputFormat format) {
            switch (format) {
            case TextureInputFormat::Red:
            case TextureInputFormat::RInteger:
                return 1;
            case TextureInputFormat::RG:
            case TextureInputFormat::RGInteger:
                return 2;
            case TextureInputFormat::RGB:
            case TextureInputFormat::BGR:
            case TextureInputFormat::RGBInteger:
            case TextureInputFormat::BGRInteger:
                return 3;
            case TextureInputFormat::RGBA:
            case TextureInputFormat::BGRA:
            case TextureInputFormat::RGBAInteger:
            case TextureInputFormat::BGRAInteger:
                return 4;
            case TextureInputFormat::StencilIndex:
            case TextureInputFormat::DepthComponent:
            case TextureInputFormat::DepthStencil:
                return 1;
            default:
                MGLOG_D("%s: Unknown input format!", __func__);
                return 0;
            }
        }

        SizeT GetBaseInternalFormatComponentCount(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::DepthStencil:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
                // Depth stencil is actually 2 components
                // tho real formats always gives byte size in whole
                // so we count this as one here
            case TextureInternalFormat::Red:
            case TextureInternalFormat::R8:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
                return 1;
            case TextureInternalFormat::RG:
            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
                return 2;
            case TextureInternalFormat::RGB:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
                return 3;
            case TextureInternalFormat::RGBA:
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::SRGB8Alpha8:
            case TextureInternalFormat::RGBA12:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGB10A2:
            case TextureInternalFormat::RGB10A2UI:
                return 4;
            default:
                return 0;
            }
        }

        SizeT GetSizedTexturePixelDataTypeSize(TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte332:
            case TexturePixelDataType::UnsignedByte233Rev:
                return 1;
            case TexturePixelDataType::UnsignedShort565:
            case TexturePixelDataType::UnsignedShort565Rev:
            case TexturePixelDataType::UnsignedShort4444:
            case TexturePixelDataType::UnsignedShort4444Rev:
            case TexturePixelDataType::UnsignedShort5551:
            case TexturePixelDataType::UnsignedShort1555Rev:
                return 2;
            case TexturePixelDataType::UnsignedInt8888:
            case TexturePixelDataType::UnsignedInt8888Rev:
            case TexturePixelDataType::UnsignedInt1010102:
            case TexturePixelDataType::UnsignedInt2101010Rev:
            case TexturePixelDataType::UnsignedInt101111Rev:
            case TexturePixelDataType::UnsignedInt5999Rev:
            case TexturePixelDataType::UnsignedInt248:
            case TexturePixelDataType::Float32UnsignedInt248Rev:
                return 4;
            default:
                return 0;
            }
        }

        SizeT GetBaseTexturePixelDataTypeSize(TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
            case TexturePixelDataType::Byte:
                return 1;
            case TexturePixelDataType::UnsignedShort:
            case TexturePixelDataType::HalfFloat:
            case TexturePixelDataType::Short:
                return 2;
            case TexturePixelDataType::UnsignedInt:
            case TexturePixelDataType::Float:
            case TexturePixelDataType::Int:
                return 4;
            default:
                return 0;
            }
        }

        SizeT GetTexturePixelDataTypeSize(TexturePixelDataType type) {
            SizeT sizedPixelFormatSize = GetSizedTexturePixelDataTypeSize(type);
            if (sizedPixelFormatSize > 0) return sizedPixelFormatSize;
            return GetBaseTexturePixelDataTypeSize(type);
        }

        SizeT GetInternalBytesPerPixel(TextureInternalFormat internalformat, TexturePixelDataType type) {
            SizeT sizedTextureFormatSize = GetSizedInternalFormatSizeInBytes(internalformat);
            if (sizedTextureFormatSize > 0) return sizedTextureFormatSize;
            SizeT sizedPixelFormatSize = GetSizedTexturePixelDataTypeSize(type);
            if (sizedPixelFormatSize > 0) return sizedPixelFormatSize;
            SizeT chCount = GetBaseInternalFormatComponentCount(internalformat);
            SizeT bytesPerChannel = GetBaseTexturePixelDataTypeSize(type);
            return chCount * bytesPerChannel;
        }

        SizeT GetInputBytesPerPixel(TextureInputFormat inputFormat, TexturePixelDataType type) {
            SizeT sizedPixelFormatSize = GetSizedTexturePixelDataTypeSize(type);
            if (sizedPixelFormatSize > 0) return sizedPixelFormatSize;
            SizeT bytesPerChannel = GetBaseTexturePixelDataTypeSize(type);
            SizeT chCount = GetBaseInputFormatComponentCount(inputFormat);
            return chCount * bytesPerChannel;
        }

        SizeT CalculateInputTextureImageSize(TextureInputFormat inputFormat, TexturePixelDataType pixelDataType,
                                             IntVec3 size) {
            return GetInputBytesPerPixel(inputFormat, pixelDataType) * size.x() * size.y() * size.z();
        }

        ComponentSizes GetComponentSizesForInternalFormat(TextureInternalFormat internal) {
            ComponentSizes s = {};

            switch (internal) {
            case TextureInternalFormat::R8:
            case TextureInternalFormat::Red:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
                s.Red = 8;
                break;

            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R16F:
                s.Red = 16;
                break;

            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG:
                s.Red = 8;
                s.Green = 8;
                break;

            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG16F:
                s.Red = 16;
                s.Green = 16;
                break;

            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB:
                s.Red = 8;
                s.Green = 8;
                s.Blue = 8;
                break;
            case TextureInternalFormat::RGB10:
                s.Red = 10;
                s.Green = 10;
                s.Blue = 10;
                break;
            case TextureInternalFormat::RGB12:
                s.Red = 12;
                s.Green = 12;
                s.Blue = 12;
                break;
            case TextureInternalFormat::RGB16:
            case TextureInternalFormat::RGB16Snorm:
                s.Red = 16;
                s.Green = 16;
                s.Blue = 16;
                break;

            case TextureInternalFormat::R3G3B2:
                s.Red = 3;
                s.Green = 3;
                s.Blue = 2;
                break;

            case TextureInternalFormat::RGBA2:
                s.Red = 2;
                s.Green = 2;
                s.Blue = 2;
                s.Alpha = 2;
                break;
            case TextureInternalFormat::RGBA4:
                s.Red = 4;
                s.Green = 4;
                s.Blue = 4;
                s.Alpha = 4;
                break;
            case TextureInternalFormat::RGB5A1:
                s.Red = 5;
                s.Green = 5;
                s.Blue = 5;
                s.Alpha = 1;
                break;
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::SRGB8Alpha8:
            case TextureInternalFormat::RGBA:
                s.Red = 8;
                s.Green = 8;
                s.Blue = 8;
                s.Alpha = 8;
                break;
            case TextureInternalFormat::RGB10A2:
            case TextureInternalFormat::RGB10A2UI:
                s.Red = 10;
                s.Green = 10;
                s.Blue = 10;
                s.Alpha = 2;
                break;

            case TextureInternalFormat::R32F:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
                s.Red = 32;
                break;
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
                s.Red = 32;
                s.Green = 32;
                break;

            case TextureInternalFormat::RGBA12:
                s.Red = 12;
                s.Green = 12;
                s.Blue = 12;
                s.Alpha = 12;
                break;
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA16F:
                s.Red = 16;
                s.Green = 16;
                s.Blue = 16;
                s.Alpha = 16;
                break;

            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
                s.Red = 16;
                s.Green = 16;
                s.Blue = 16;
                break;
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
                s.Red = 32;
                s.Green = 32;
                s.Blue = 32;
                break;

            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
                s.Red = 32;
                s.Green = 32;
                s.Blue = 32;
                s.Alpha = 32;
                break;

            case TextureInternalFormat::R11FG11FB10F:
                s.Red = 11;
                s.Green = 11;
                s.Blue = 10;
                break;
            case TextureInternalFormat::RGB9E5:
                s.Red = 9;
                s.Green = 9;
                s.Blue = 9;
                break;

            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
                s.Depth = 16;
                break;
            case TextureInternalFormat::DepthComponent24:
                s.Depth = 24;
                break;
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
                s.Depth = 32;
                break;
            case TextureInternalFormat::DepthStencil:
            case TextureInternalFormat::Depth24Stencil8:
                s.Depth = 24;
                s.Stencil = 8;
                break;
            case TextureInternalFormat::Depth32FStencil8:
                s.Depth = 32;
                s.Stencil = 8;
                break;
            default:
                MOBILEGL_ASSERT(false, "Unimplemented internal format in GetComponentSizesForInternalFormat: %d",
                                static_cast<Int>(internal));
                break;
            }

            return s;
        }

    } // namespace MG_Util
} // namespace MobileGL
