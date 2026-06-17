// MobileGL - MobileGL/MG_Util/Converters/MGToMG/TextureEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumConverter.h"
#include "MG_Util/Converters/MGToStr/TextureEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        TextureTarget ConvertTextureUploadTargetToTextureTarget(TextureUploadTarget target) {
            switch (target) {
            case TextureUploadTarget::Texture2D:
            case TextureUploadTarget::ProxyTexture2D:
                return TextureTarget::Texture2D;
            case TextureUploadTarget::Texture1DArray:
            case TextureUploadTarget::ProxyTexture1DArray:
                return TextureTarget::Texture1DArray;
            case TextureUploadTarget::TextureRectangle:
            case TextureUploadTarget::ProxyTextureRectangle:
                return TextureTarget::TextureRectangle;
            case TextureUploadTarget::CubeMapPositiveX:
            case TextureUploadTarget::CubeMapNegativeX:
            case TextureUploadTarget::CubeMapPositiveY:
            case TextureUploadTarget::CubeMapNegativeY:
            case TextureUploadTarget::CubeMapPositiveZ:
            case TextureUploadTarget::CubeMapNegativeZ:
            case TextureUploadTarget::ProxyCubeMap:
                return TextureTarget::TextureCubeMap;
            case TextureUploadTarget::CubeMapArray:
            case TextureUploadTarget::ProxyCubeMapArray:
                return TextureTarget::TextureCubeMapArray;
            case TextureUploadTarget::Texture2DMultisample:
            case TextureUploadTarget::ProxyTexture2DMultisample:
                return TextureTarget::Texture2DMultisample;
            case TextureUploadTarget::Texture2DMultisampleArray:
            case TextureUploadTarget::ProxyTexture2DMultisampleArray:
                return TextureTarget::Texture2DMultisampleArray;
            case TextureUploadTarget::Texture3D:
            case TextureUploadTarget::ProxyTexture3D:
                return TextureTarget::Texture3D;
            case TextureUploadTarget::Texture1D:
            case TextureUploadTarget::ProxyTexture1D:
                return TextureTarget::Texture1D;
            case TextureUploadTarget::Texture2DArray:
            case TextureUploadTarget::ProxyTexture2DArray:
                return TextureTarget::Texture2DArray;
            default:
                return TextureTarget::Unknown;
            }
        }

        TextureInternalFormat ConvertInternalFormatToSized(TextureInternalFormat internalformat,
                                                           TextureInputFormat format, TexturePixelDataType type) {
            switch (internalformat) {
            case TextureInternalFormat::R8:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::R3G3B2:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGB5A1:
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGB10A2:
            case TextureInternalFormat::RGB10A2UI:
            case TextureInternalFormat::RGBA12:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::SRGB8Alpha8:
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32: // not a standard format in OpenGL core profile
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
                return internalformat;
            // probably we should assume unorm here?
            case TextureInternalFormat::RGBA: {
                switch (type) {
                case TexturePixelDataType::UnsignedByte:
                    return TextureInternalFormat::RGBA8;
                case TexturePixelDataType::UnsignedShort:
                    return TextureInternalFormat::RGBA16;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            case TextureInternalFormat::RGB: {
                switch (type) {
                case TexturePixelDataType::UnsignedByte:
                    return TextureInternalFormat::RGB8;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            case TextureInternalFormat::RG: {
                switch (type) {
                case TexturePixelDataType::UnsignedByte:
                    return TextureInternalFormat::RG8;
                case TexturePixelDataType::UnsignedShort:
                    return TextureInternalFormat::RG16;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            case TextureInternalFormat::Red: {
                switch (type) {
                case TexturePixelDataType::UnsignedByte:
                    return TextureInternalFormat::R8;
                case TexturePixelDataType::UnsignedShort:
                    return TextureInternalFormat::R16;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            case TextureInternalFormat::DepthComponent: {
                switch (type) {
                case TexturePixelDataType::UnsignedShort:
                    return TextureInternalFormat::DepthComponent16;
                case TexturePixelDataType::UnsignedInt:
                    return TextureInternalFormat::DepthComponent32;
                case TexturePixelDataType::Float:
                    return TextureInternalFormat::DepthComponent32F;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            case TextureInternalFormat::DepthStencil: {
                switch (type) {
                case TexturePixelDataType::UnsignedInt248:
                    return TextureInternalFormat::Depth24Stencil8;
                default:
                    MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, "
                            "returning original.",
                            __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                            MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                            MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                    return internalformat;
                }
            }
            default: {
                MGLOG_W("%s: Can't infer sized internal format from internalformat=%s, format=%s, type=%s, returning "
                        "original.",
                        __func__, MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str(),
                        MG_Util::ConvertTextureInputFormatToString(format).c_str(),
                        MG_Util::ConvertTexturePixelDataTypeToString(type).c_str());
                return internalformat;
            }
            }
        }

        TextureInternalFormat ConvertInternalFormatToUnsized(TextureInternalFormat internalformat) {
            switch (internalformat) {
            case TextureInternalFormat::R8:
            case TextureInternalFormat::R8Snorm:
            case TextureInternalFormat::R16:
            case TextureInternalFormat::R16Snorm:
            case TextureInternalFormat::R16F:
            case TextureInternalFormat::R32F:
            case TextureInternalFormat::R8I:
            case TextureInternalFormat::R8UI:
            case TextureInternalFormat::R16I:
            case TextureInternalFormat::R16UI:
            case TextureInternalFormat::R32I:
            case TextureInternalFormat::R32UI:
            case TextureInternalFormat::Red:
                return TextureInternalFormat::Red;
            case TextureInternalFormat::RG8:
            case TextureInternalFormat::RG8Snorm:
            case TextureInternalFormat::RG16:
            case TextureInternalFormat::RG16Snorm:
            case TextureInternalFormat::RG16F:
            case TextureInternalFormat::RG32F:
            case TextureInternalFormat::RG8I:
            case TextureInternalFormat::RG8UI:
            case TextureInternalFormat::RG16I:
            case TextureInternalFormat::RG16UI:
            case TextureInternalFormat::RG32I:
            case TextureInternalFormat::RG32UI:
            case TextureInternalFormat::RG:
                return TextureInternalFormat::RG;
            case TextureInternalFormat::R3G3B2:
            case TextureInternalFormat::RGB4:
            case TextureInternalFormat::RGB5:
            case TextureInternalFormat::RGB8:
            case TextureInternalFormat::RGB8Snorm:
            case TextureInternalFormat::RGB10:
            case TextureInternalFormat::RGB12:
            case TextureInternalFormat::RGB16Snorm:
            case TextureInternalFormat::RGB16F:
            case TextureInternalFormat::RGB32F:
            case TextureInternalFormat::R11FG11FB10F:
            case TextureInternalFormat::RGB9E5:
            case TextureInternalFormat::SRGB8:
            case TextureInternalFormat::RGB8I:
            case TextureInternalFormat::RGB8UI:
            case TextureInternalFormat::RGB16I:
            case TextureInternalFormat::RGB16UI:
            case TextureInternalFormat::RGB32I:
            case TextureInternalFormat::RGB32UI:
            case TextureInternalFormat::RGB:
                return TextureInternalFormat::RGB;
            case TextureInternalFormat::RGBA2:
            case TextureInternalFormat::RGBA4:
            case TextureInternalFormat::RGB5A1:
            case TextureInternalFormat::RGBA8:
            case TextureInternalFormat::RGBA8Snorm:
            case TextureInternalFormat::RGB10A2:
            case TextureInternalFormat::RGB10A2UI:
            case TextureInternalFormat::RGBA12:
            case TextureInternalFormat::RGBA16:
            case TextureInternalFormat::RGBA16Snorm:
            case TextureInternalFormat::SRGB8Alpha8:
            case TextureInternalFormat::RGBA16F:
            case TextureInternalFormat::RGBA32F:
            case TextureInternalFormat::RGBA8I:
            case TextureInternalFormat::RGBA8UI:
            case TextureInternalFormat::RGBA16I:
            case TextureInternalFormat::RGBA16UI:
            case TextureInternalFormat::RGBA32I:
            case TextureInternalFormat::RGBA32UI:
            case TextureInternalFormat::RGBA:
                return TextureInternalFormat::RGBA;
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32:
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::DepthComponent:
                return TextureInternalFormat::DepthComponent;
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
                return TextureInternalFormat::DepthStencil;
            default:
                MGLOG_W("%s: Unknown or unhandled internal format %s, returning original.", __func__,
                        MG_Util::ConvertTextureInternalFormatToString(internalformat).c_str());
                return internalformat;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
