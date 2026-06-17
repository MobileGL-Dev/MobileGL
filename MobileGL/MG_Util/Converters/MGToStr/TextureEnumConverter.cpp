// MobileGL - MobileGL/MG_Util/Converters/MGToStr/TextureEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumConverter.h"
#include "MG_State/GLState/TextureState/TextureObject.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    namespace MG_Util {
        String ConvertTextureTargetToString(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
                return "Texture1D";
            case TextureTarget::Texture2D:
                return "Texture2D";
            case TextureTarget::Texture3D:
                return "Texture3D";
            case TextureTarget::TextureCubeMap:
                return "TextureCubeMap";
            case TextureTarget::Texture2DArray:
                return "Texture2DArray";
            case TextureTarget::Texture2DMultisample:
                return "Texture2DMultisample";
            case TextureTarget::TextureCubeMapArray:
                return "TextureCubeMapArray";
            case TextureTarget::TextureRectangle:
                return "TextureRectangle";
            case TextureTarget::TextureBuffer:
                return "TextureBuffer";
            case TextureTarget::Texture1DArray:
                return "Texture1DArray";
            case TextureTarget::Texture2DMultisampleArray:
                return "Texture2DMultisampleArray";
            default:
                return "Unknown";
            }
        }

        String ConvertTextureInputFormatToString(TextureInputFormat format) {
            switch (format) {
            case TextureInputFormat::Red:
                return "Red";
            case TextureInputFormat::RG:
                return "RG";
            case TextureInputFormat::RGB:
                return "RGB";
            case TextureInputFormat::BGR:
                return "BGR";
            case TextureInputFormat::RGBA:
                return "RGBA";
            case TextureInputFormat::BGRA:
                return "BGRA";
            case TextureInputFormat::RInteger:
                return "RInteger";
            case TextureInputFormat::RGInteger:
                return "RGInteger";
            case TextureInputFormat::RGBInteger:
                return "RGBInteger";
            case TextureInputFormat::BGRInteger:
                return "BGRInteger";
            case TextureInputFormat::RGBAInteger:
                return "RGBAInteger";
            case TextureInputFormat::BGRAInteger:
                return "BGRAInteger";
            case TextureInputFormat::StencilIndex:
                return "StencilIndex";
            case TextureInputFormat::DepthComponent:
                return "DepthComponent";
            case TextureInputFormat::DepthStencil:
                return "DepthStencil";
            default:
                return "Unknown";
            }
        }

        String ConvertTextureInternalFormatToString(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8:
                return "R8";
            case TextureInternalFormat::R8Snorm:
                return "R8Snorm";
            case TextureInternalFormat::R16:
                return "R16";
            case TextureInternalFormat::R16Snorm:
                return "R16Snorm";
            case TextureInternalFormat::RG8:
                return "RG8";
            case TextureInternalFormat::RG8Snorm:
                return "RG8Snorm";
            case TextureInternalFormat::RG16:
                return "RG16";
            case TextureInternalFormat::RG16Snorm:
                return "RG16Snorm";
            case TextureInternalFormat::R3G3B2:
                return "R3G3B2";
            case TextureInternalFormat::RGB4:
                return "RGB4";
            case TextureInternalFormat::RGB5:
                return "RGB5";
            case TextureInternalFormat::RGB8:
                return "RGB8";
            case TextureInternalFormat::RGB8Snorm:
                return "RGB8Snorm";
            case TextureInternalFormat::RGB10:
                return "RGB10";
            case TextureInternalFormat::RGB12:
                return "RGB12";
            case TextureInternalFormat::RGB16:
                return "RGB16";
            case TextureInternalFormat::RGB16Snorm:
                return "RGB16Snorm";
            case TextureInternalFormat::RGBA2:
                return "RGBA2";
            case TextureInternalFormat::RGBA4:
                return "RGBA4";
            case TextureInternalFormat::RGB5A1:
                return "RGB5A1";
            case TextureInternalFormat::RGBA8:
                return "RGBA8";
            case TextureInternalFormat::RGBA8Snorm:
                return "RGBA8Snorm";
            case TextureInternalFormat::RGB10A2:
                return "RGB10A2";
            case TextureInternalFormat::RGB10A2UI:
                return "RGB10A2UI";
            case TextureInternalFormat::RGBA12:
                return "RGBA12";
            case TextureInternalFormat::RGBA16:
                return "RGBA16";
            case TextureInternalFormat::RGBA16Snorm:
                return "RGBA16Snorm";
            case TextureInternalFormat::SRGB8:
                return "SRGB8";
            case TextureInternalFormat::SRGB8Alpha8:
                return "SRGB8Alpha8";
            case TextureInternalFormat::R16F:
                return "R16F";
            case TextureInternalFormat::RG16F:
                return "RG16F";
            case TextureInternalFormat::RGB16F:
                return "RGB16F";
            case TextureInternalFormat::RGBA16F:
                return "RGBA16F";
            case TextureInternalFormat::R32F:
                return "R32F";
            case TextureInternalFormat::RG32F:
                return "RG32F";
            case TextureInternalFormat::RGB32F:
                return "RGB32F";
            case TextureInternalFormat::RGBA32F:
                return "RGBA32F";
            case TextureInternalFormat::R11FG11FB10F:
                return "R11FG11FB10F";
            case TextureInternalFormat::RGB9E5:
                return "RGB9E5";
            case TextureInternalFormat::R8I:
                return "R8I";
            case TextureInternalFormat::R8UI:
                return "R8UI";
            case TextureInternalFormat::R16I:
                return "R16I";
            case TextureInternalFormat::R16UI:
                return "R16UI";
            case TextureInternalFormat::R32I:
                return "R32I";
            case TextureInternalFormat::R32UI:
                return "R32UI";
            case TextureInternalFormat::RG8I:
                return "RG8I";
            case TextureInternalFormat::RG8UI:
                return "RG8UI";
            case TextureInternalFormat::RG16I:
                return "RG16I";
            case TextureInternalFormat::RG16UI:
                return "RG16UI";
            case TextureInternalFormat::RG32I:
                return "RG32I";
            case TextureInternalFormat::RG32UI:
                return "RG32UI";
            case TextureInternalFormat::RGB8I:
                return "RGB8I";
            case TextureInternalFormat::RGB8UI:
                return "RGB8UI";
            case TextureInternalFormat::RGB16I:
                return "RGB16I";
            case TextureInternalFormat::RGB16UI:
                return "RGB16UI";
            case TextureInternalFormat::RGB32I:
                return "RGB32I";
            case TextureInternalFormat::RGB32UI:
                return "RGB32UI";
            case TextureInternalFormat::RGBA8I:
                return "RGBA8I";
            case TextureInternalFormat::RGBA8UI:
                return "RGBA8UI";
            case TextureInternalFormat::RGBA16I:
                return "RGBA16I";
            case TextureInternalFormat::RGBA16UI:
                return "RGBA16UI";
            case TextureInternalFormat::RGBA32I:
                return "RGBA32I";
            case TextureInternalFormat::RGBA32UI:
                return "RGBA32UI";
            case TextureInternalFormat::DepthComponent:
                return "DepthComponent";
            case TextureInternalFormat::DepthComponent16:
                return "DepthComponent16";
            case TextureInternalFormat::DepthComponent24:
                return "DepthComponent24";
            case TextureInternalFormat::DepthComponent32:
                return "DepthComponent32";
            case TextureInternalFormat::DepthComponent32F:
                return "DepthComponent32F";
            case TextureInternalFormat::DepthStencil:
                return "DepthStencil";
            case TextureInternalFormat::Depth24Stencil8:
                return "Depth24Stencil8";
            case TextureInternalFormat::Depth32FStencil8:
                return "Depth32FStencil8";
            case TextureInternalFormat::Red:
                return "Red";
            case TextureInternalFormat::RG:
                return "RG";
            case TextureInternalFormat::RGB:
                return "RGB";
            case TextureInternalFormat::RGBA:
                return "RGBA";
            default:
                return "Unknown";
            }
        }

        String ConvertTexturePixelDataTypeToString(TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                return "UnsignedByte";
            case TexturePixelDataType::Byte:
                return "Byte";
            case TexturePixelDataType::UnsignedShort:
                return "UnsignedShort";
            case TexturePixelDataType::Short:
                return "Short";
            case TexturePixelDataType::UnsignedInt:
                return "UnsignedInt";
            case TexturePixelDataType::Int:
                return "Int";
            case TexturePixelDataType::Float:
                return "Float";
            case TexturePixelDataType::HalfFloat:
                return "HalfFloat";
            case TexturePixelDataType::UnsignedInt248:
                return "UnsignedInt248";
            case TexturePixelDataType::UnsignedByte332:
                return "UnsignedByte332";
            case TexturePixelDataType::UnsignedByte233Rev:
                return "UnsignedByte233Rev";
            case TexturePixelDataType::UnsignedShort565:
                return "UnsignedShort565";
            case TexturePixelDataType::UnsignedShort565Rev:
                return "UnsignedShort565Rev";
            case TexturePixelDataType::UnsignedShort4444:
                return "UnsignedShort4444";
            case TexturePixelDataType::UnsignedShort4444Rev:
                return "UnsignedShort4444Rev";
            case TexturePixelDataType::UnsignedShort5551:
                return "UnsignedShort5551";
            case TexturePixelDataType::UnsignedShort1555Rev:
                return "UnsignedShort1555Rev";
            case TexturePixelDataType::UnsignedInt8888:
                return "UnsignedInt8888";
            case TexturePixelDataType::UnsignedInt8888Rev:
                return "UnsignedInt8888Rev";
            case TexturePixelDataType::UnsignedInt1010102:
                return "UnsignedInt1010102";
            case TexturePixelDataType::UnsignedInt2101010Rev:
                return "UnsignedInt2101010Rev";
            case TexturePixelDataType::UnsignedInt101111Rev:
                return "UnsignedInt101111Rev";
            case TexturePixelDataType::UnsignedInt5999Rev:
                return "UnsignedInt5999Rev";
            case TexturePixelDataType::Float32UnsignedInt248Rev:
                return "Float32UnsignedInt248Rev";
            default:
                return "Unknown";
            }
        }

        String ConvertTextureUploadTargetToString(TextureUploadTarget target) {
            switch (target) {
            case TextureUploadTarget::Texture2D:
                return "Texture2D";
            case TextureUploadTarget::ProxyTexture2D:
                return "ProxyTexture2D";
            case TextureUploadTarget::Texture1DArray:
                return "Texture1DArray";
            case TextureUploadTarget::ProxyTexture1DArray:
                return "ProxyTexture1DArray";
            case TextureUploadTarget::TextureRectangle:
                return "TextureRectangle";
            case TextureUploadTarget::ProxyTextureRectangle:
                return "ProxyTextureRectangle";
            case TextureUploadTarget::CubeMapPositiveX:
                return "CubeMapPositiveX";
            case TextureUploadTarget::CubeMapNegativeX:
                return "CubeMapNegativeX";
            case TextureUploadTarget::CubeMapPositiveY:
                return "CubeMapPositiveY";
            case TextureUploadTarget::CubeMapNegativeY:
                return "CubeMapNegativeY";
            case TextureUploadTarget::CubeMapPositiveZ:
                return "CubeMapPositiveZ";
            case TextureUploadTarget::CubeMapNegativeZ:
                return "CubeMapNegativeZ";
            case TextureUploadTarget::ProxyCubeMap:
                return "ProxyCubeMap";
            case TextureUploadTarget::Texture2DMultisample:
                return "Texture2DMultisample";
            case TextureUploadTarget::ProxyTexture2DMultisample:
                return "ProxyTexture2DMultisample";
            case TextureUploadTarget::CubeMapArray:
                return "CubeMapArray";
            case TextureUploadTarget::ProxyCubeMapArray:
                return "ProxyCubeMapArray";
            case TextureUploadTarget::Texture3D:
                return "Texture3D";
            case TextureUploadTarget::ProxyTexture3D:
                return "ProxyTexture3D";
            case TextureUploadTarget::Texture1D:
                return "Texture1D";
            case TextureUploadTarget::ProxyTexture1D:
                return "ProxyTexture1D";
            case TextureUploadTarget::Texture2DArray:
                return "Texture2DArray";
            case TextureUploadTarget::ProxyTexture2DArray:
                return "ProxyTexture2DArray";
            case TextureUploadTarget::Texture2DMultisampleArray:
                return "Texture2DMultisampleArray";
            case TextureUploadTarget::ProxyTexture2DMultisampleArray:
                return "ProxyTexture2DMultisampleArray";
            default:
                return "Unknown";
            }
        }

        String ConvertSamplerFilterModeToString(SamplerFilterMode v) {
            switch (v) {
            case SamplerFilterMode::Nearest:
                return "Nearest";
            case SamplerFilterMode::Linear:
                return "Linear";
            default:
                return "Unknown";
            }
        }

        String ConvertSamplerMipmapModeToString(SamplerMipmapMode v) {
            switch (v) {
            case SamplerMipmapMode::None:
                return "None";
            case SamplerMipmapMode::Nearest:
                return "Nearest";
            default:
                return "Unknown";
            }
        }

        String ConvertSamplerWrapModeToString(SamplerWrapMode v) {
            switch (v) {
            case SamplerWrapMode::ClampToEdge:
                return "ClampToEdge";
            case SamplerWrapMode::MirroredRepeat:
                return "MirroredRepeat";
            case SamplerWrapMode::Repeat:
                return "Repeat";
            case SamplerWrapMode::ClampToBorder:
                return "ClampToBorder";
            default:
                return "Unknown";
            }
        }

        String ConvertSamplerCompareModeToString(SamplerCompareMode v) {
            switch (v) {
            case SamplerCompareMode::None:
                return "None";
            case SamplerCompareMode::CompareToTexture:
                return "CompareToTexture";
            default:
                return "Unknown";
            }
        }

        String ConvertSamplerCompareFuncToString(SamplerCompareFunc v) {
            switch (v) {
            case SamplerCompareFunc::Never:
                return "Never";
            case SamplerCompareFunc::Less:
                return "Less";
            case SamplerCompareFunc::Equal:
                return "Equal";
            case SamplerCompareFunc::LessEqual:
                return "LessEqual";
            case SamplerCompareFunc::Greater:
                return "Greater";
            case SamplerCompareFunc::NotEqual:
                return "NotEqual";
            case SamplerCompareFunc::GreaterEqual:
                return "GreaterEqual";
            case SamplerCompareFunc::Always:
                return "Always";
            default:
                return "Unknown";
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
