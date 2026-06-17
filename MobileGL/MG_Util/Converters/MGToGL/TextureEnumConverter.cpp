// MobileGL - MobileGL/MG_Util/Converters/MGToGL/TextureEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumConverter.h"
#include "GL/gl.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    namespace MG_Util {
        GLenum ConvertTextureTargetToGLEnum(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
                return GL_TEXTURE_1D;
            case TextureTarget::Texture2D:
                return GL_TEXTURE_2D;
            case TextureTarget::Texture3D:
                return GL_TEXTURE_3D;
            case TextureTarget::TextureCubeMap:
                return GL_TEXTURE_CUBE_MAP;
            case TextureTarget::Texture2DArray:
                return GL_TEXTURE_2D_ARRAY;
            case TextureTarget::Texture2DMultisample:
                return GL_TEXTURE_2D_MULTISAMPLE;
            case TextureTarget::TextureCubeMapArray:
                return GL_TEXTURE_CUBE_MAP_ARRAY;
            case TextureTarget::TextureBuffer:
                return GL_TEXTURE_BUFFER;
            case TextureTarget::Texture1DArray:
                return GL_TEXTURE_1D_ARRAY;
            case TextureTarget::TextureRectangle:
                return GL_TEXTURE_RECTANGLE;
            case TextureTarget::Texture2DMultisampleArray:
                return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertTextureInputFormatToGLEnum(TextureInputFormat format) {
            switch (format) {
            case TextureInputFormat::Red:
                return GL_RED;
            case TextureInputFormat::RG:
                return GL_RG;
            case TextureInputFormat::RGB:
                return GL_RGB;
            case TextureInputFormat::BGR:
                return GL_BGR;
            case TextureInputFormat::RGBA:
                return GL_RGBA;
            case TextureInputFormat::BGRA:
                return GL_BGRA;
            case TextureInputFormat::RInteger:
                return GL_RED_INTEGER;
            case TextureInputFormat::RGInteger:
                return GL_RG_INTEGER;
            case TextureInputFormat::RGBInteger:
                return GL_RGB_INTEGER;
            case TextureInputFormat::BGRInteger:
                return GL_BGR_INTEGER;
            case TextureInputFormat::RGBAInteger:
                return GL_RGBA_INTEGER;
            case TextureInputFormat::BGRAInteger:
                return GL_BGRA_INTEGER;
            case TextureInputFormat::StencilIndex:
                return GL_STENCIL_INDEX;
            case TextureInputFormat::DepthComponent:
                return GL_DEPTH_COMPONENT;
            case TextureInputFormat::DepthStencil:
                return GL_DEPTH_STENCIL;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertTextureInternalFormatToGLEnum(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8:
                return GL_R8;
            case TextureInternalFormat::R8Snorm:
                return GL_R8_SNORM;
            case TextureInternalFormat::R16:
                return GL_R16;
            case TextureInternalFormat::R16Snorm:
                return GL_R16_SNORM;
            case TextureInternalFormat::RG8:
                return GL_RG8;
            case TextureInternalFormat::RG8Snorm:
                return GL_RG8_SNORM;
            case TextureInternalFormat::RG16:
                return GL_RG16;
            case TextureInternalFormat::RG16Snorm:
                return GL_RG16_SNORM;
            case TextureInternalFormat::R3G3B2:
                return GL_R3_G3_B2;
            case TextureInternalFormat::RGB4:
                return GL_RGB4;
            case TextureInternalFormat::RGB5:
                return GL_RGB5;
            case TextureInternalFormat::RGB8:
                return GL_RGB8;
            case TextureInternalFormat::RGB8Snorm:
                return GL_RGB8_SNORM;
            case TextureInternalFormat::RGB10:
                return GL_RGB10;
            case TextureInternalFormat::RGB12:
                return GL_RGB12;
            case TextureInternalFormat::RGB16:
                return GL_RGB16;
            case TextureInternalFormat::RGB16Snorm:
                return GL_RGB16_SNORM;
            case TextureInternalFormat::RGBA2:
                return GL_RGBA2;
            case TextureInternalFormat::RGBA4:
                return GL_RGBA4;
            case TextureInternalFormat::RGB5A1:
                return GL_RGB5_A1;
            case TextureInternalFormat::RGBA8:
                return GL_RGBA8;
            case TextureInternalFormat::RGBA8Snorm:
                return GL_RGBA8_SNORM;
            case TextureInternalFormat::RGB10A2:
                return GL_RGB10_A2;
            case TextureInternalFormat::RGB10A2UI:
                return GL_RGB10_A2UI;
            case TextureInternalFormat::RGBA12:
                return GL_RGBA12;
            case TextureInternalFormat::RGBA16:
                return GL_RGBA16;
            case TextureInternalFormat::RGBA16Snorm:
                return GL_RGBA16_SNORM;
            case TextureInternalFormat::SRGB8:
                return GL_SRGB8;
            case TextureInternalFormat::SRGB8Alpha8:
                return GL_SRGB8_ALPHA8;
            case TextureInternalFormat::R16F:
                return GL_R16F;
            case TextureInternalFormat::RG16F:
                return GL_RG16F;
            case TextureInternalFormat::RGB16F:
                return GL_RGB16F;
            case TextureInternalFormat::RGBA16F:
                return GL_RGBA16F;
            case TextureInternalFormat::R32F:
                return GL_R32F;
            case TextureInternalFormat::RG32F:
                return GL_RG32F;
            case TextureInternalFormat::RGB32F:
                return GL_RGB32F;
            case TextureInternalFormat::RGBA32F:
                return GL_RGBA32F;
            case TextureInternalFormat::R11FG11FB10F:
                return GL_R11F_G11F_B10F;
            case TextureInternalFormat::RGB9E5:
                return GL_RGB9_E5;
            case TextureInternalFormat::R8I:
                return GL_R8I;
            case TextureInternalFormat::R8UI:
                return GL_R8UI;
            case TextureInternalFormat::R16I:
                return GL_R16I;
            case TextureInternalFormat::R16UI:
                return GL_R16UI;
            case TextureInternalFormat::R32I:
                return GL_R32I;
            case TextureInternalFormat::R32UI:
                return GL_R32UI;
            case TextureInternalFormat::RG8I:
                return GL_RG8I;
            case TextureInternalFormat::RG8UI:
                return GL_RG8UI;
            case TextureInternalFormat::RG16I:
                return GL_RG16I;
            case TextureInternalFormat::RG16UI:
                return GL_RG16UI;
            case TextureInternalFormat::RG32I:
                return GL_RG32I;
            case TextureInternalFormat::RG32UI:
                return GL_RG32UI;
            case TextureInternalFormat::RGB8I:
                return GL_RGB8I;
            case TextureInternalFormat::RGB8UI:
                return GL_RGB8UI;
            case TextureInternalFormat::RGB16I:
                return GL_RGB16I;
            case TextureInternalFormat::RGB16UI:
                return GL_RGB16UI;
            case TextureInternalFormat::RGB32I:
                return GL_RGB32I;
            case TextureInternalFormat::RGB32UI:
                return GL_RGB32UI;
            case TextureInternalFormat::RGBA8I:
                return GL_RGBA8I;
            case TextureInternalFormat::RGBA8UI:
                return GL_RGBA8UI;
            case TextureInternalFormat::RGBA16I:
                return GL_RGBA16I;
            case TextureInternalFormat::RGBA16UI:
                return GL_RGBA16UI;
            case TextureInternalFormat::RGBA32I:
                return GL_RGBA32I;
            case TextureInternalFormat::RGBA32UI:
                return GL_RGBA32UI;
            case TextureInternalFormat::DepthComponent:
                return GL_DEPTH_COMPONENT;
            case TextureInternalFormat::DepthComponent16:
                return GL_DEPTH_COMPONENT16;
            case TextureInternalFormat::DepthComponent24:
                return GL_DEPTH_COMPONENT24;
            case TextureInternalFormat::DepthComponent32F:
                return GL_DEPTH_COMPONENT32F;
            case TextureInternalFormat::Depth24Stencil8:
                return GL_DEPTH24_STENCIL8;
            case TextureInternalFormat::Depth32FStencil8:
                return GL_DEPTH32F_STENCIL8;
            case TextureInternalFormat::DepthComponent32:
                return GL_DEPTH_COMPONENT32;
            case TextureInternalFormat::DepthStencil:
                return GL_DEPTH_STENCIL;
            case TextureInternalFormat::Red:
                return GL_RED;
            case TextureInternalFormat::RG:
                return GL_RG;
            case TextureInternalFormat::RGB:
                return GL_RGB;
            case TextureInternalFormat::RGBA:
                return GL_RGBA;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertTexturePixelDataTypeToGLEnum(TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                return GL_UNSIGNED_BYTE;
            case TexturePixelDataType::Byte:
                return GL_BYTE;
            case TexturePixelDataType::UnsignedShort:
                return GL_UNSIGNED_SHORT;
            case TexturePixelDataType::Short:
                return GL_SHORT;
            case TexturePixelDataType::UnsignedInt:
                return GL_UNSIGNED_INT;
            case TexturePixelDataType::Int:
                return GL_INT;
            case TexturePixelDataType::Float:
                return GL_FLOAT;
            case TexturePixelDataType::HalfFloat:
                return GL_HALF_FLOAT;
            case TexturePixelDataType::UnsignedInt248:
                return GL_UNSIGNED_INT_24_8;
            case TexturePixelDataType::UnsignedByte332:
                return GL_UNSIGNED_BYTE_3_3_2;
            case TexturePixelDataType::UnsignedByte233Rev:
                return GL_UNSIGNED_BYTE_2_3_3_REV;
            case TexturePixelDataType::UnsignedShort565:
                return GL_UNSIGNED_SHORT_5_6_5;
            case TexturePixelDataType::UnsignedShort565Rev:
                return GL_UNSIGNED_SHORT_5_6_5_REV;
            case TexturePixelDataType::UnsignedShort4444:
                return GL_UNSIGNED_SHORT_4_4_4_4;
            case TexturePixelDataType::UnsignedShort4444Rev:
                return GL_UNSIGNED_SHORT_4_4_4_4_REV;
            case TexturePixelDataType::UnsignedShort5551:
                return GL_UNSIGNED_SHORT_5_5_5_1;
            case TexturePixelDataType::UnsignedShort1555Rev:
                return GL_UNSIGNED_SHORT_1_5_5_5_REV;
            case TexturePixelDataType::UnsignedInt8888:
                return GL_UNSIGNED_INT_8_8_8_8;
            case TexturePixelDataType::UnsignedInt8888Rev:
                return GL_UNSIGNED_INT_8_8_8_8_REV;
            case TexturePixelDataType::UnsignedInt1010102:
                return GL_UNSIGNED_INT_10_10_10_2;
            case TexturePixelDataType::UnsignedInt2101010Rev:
                return GL_UNSIGNED_INT_2_10_10_10_REV;
            case TexturePixelDataType::UnsignedInt101111Rev:
                return GL_UNSIGNED_INT_10F_11F_11F_REV;
            case TexturePixelDataType::UnsignedInt5999Rev:
                return GL_UNSIGNED_INT_5_9_9_9_REV;
            case TexturePixelDataType::Float32UnsignedInt248Rev:
                return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertTextureUploadTargetToGLEnum(TextureUploadTarget target) {
            switch (target) {
            case TextureUploadTarget::Texture2D:
                return GL_TEXTURE_2D;
            case TextureUploadTarget::Texture1D:
                return GL_TEXTURE_1D;
            case TextureUploadTarget::Texture3D:
                return GL_TEXTURE_3D;
            case TextureUploadTarget::ProxyTexture2D:
                return GL_PROXY_TEXTURE_2D;
            case TextureUploadTarget::Texture1DArray:
                return GL_TEXTURE_1D_ARRAY;
            case TextureUploadTarget::ProxyTexture1DArray:
                return GL_PROXY_TEXTURE_1D_ARRAY;
            case TextureUploadTarget::TextureRectangle:
                return GL_TEXTURE_RECTANGLE;
            case TextureUploadTarget::ProxyTextureRectangle:
                return GL_PROXY_TEXTURE_RECTANGLE;
            case TextureUploadTarget::CubeMapPositiveX:
                return GL_TEXTURE_CUBE_MAP_POSITIVE_X;
            case TextureUploadTarget::CubeMapNegativeX:
                return GL_TEXTURE_CUBE_MAP_NEGATIVE_X;
            case TextureUploadTarget::CubeMapPositiveY:
                return GL_TEXTURE_CUBE_MAP_POSITIVE_Y;
            case TextureUploadTarget::CubeMapNegativeY:
                return GL_TEXTURE_CUBE_MAP_NEGATIVE_Y;
            case TextureUploadTarget::CubeMapPositiveZ:
                return GL_TEXTURE_CUBE_MAP_POSITIVE_Z;
            case TextureUploadTarget::CubeMapNegativeZ:
                return GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
            case TextureUploadTarget::ProxyCubeMap:
                return GL_PROXY_TEXTURE_CUBE_MAP;
            case TextureUploadTarget::Texture2DMultisample:
                return GL_TEXTURE_2D_MULTISAMPLE;
            case TextureUploadTarget::ProxyTexture2DMultisample:
                return GL_PROXY_TEXTURE_2D_MULTISAMPLE;
            case TextureUploadTarget::CubeMapArray:
                return GL_TEXTURE_CUBE_MAP_ARRAY;
            case TextureUploadTarget::ProxyCubeMapArray:
                return GL_PROXY_TEXTURE_CUBE_MAP_ARRAY;
            case TextureUploadTarget::Texture2DArray:
                return GL_TEXTURE_2D_ARRAY;
            case TextureUploadTarget::ProxyTexture2DArray:
                return GL_PROXY_TEXTURE_2D_ARRAY;
            case TextureUploadTarget::Texture2DMultisampleArray:
                return GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
            case TextureUploadTarget::ProxyTexture2DMultisampleArray:
                return GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertSamplerFilterModeToGLEnum(SamplerFilterMode filter, SamplerMipmapMode mipmap) {
            switch (mipmap) {
            case SamplerMipmapMode::None: {
                switch (filter) {
                case SamplerFilterMode::Nearest:
                    return GL_NEAREST;
                case SamplerFilterMode::Linear:
                    return GL_LINEAR;
                default:
                    return GL_UNKNOWN_MGL;
                }
            }
            case SamplerMipmapMode::Nearest: {
                switch (filter) {
                case SamplerFilterMode::Nearest:
                    return GL_NEAREST_MIPMAP_NEAREST;
                case SamplerFilterMode::Linear:
                    return GL_LINEAR_MIPMAP_NEAREST;
                default:
                    return GL_UNKNOWN_MGL;
                }
            }
            case SamplerMipmapMode::Linear: {
                switch (filter) {
                case SamplerFilterMode::Nearest:
                    return GL_NEAREST_MIPMAP_LINEAR;
                case SamplerFilterMode::Linear:
                    return GL_LINEAR_MIPMAP_LINEAR;
                default:
                    return GL_UNKNOWN_MGL;
                }
            }
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertSamplerMipmapModeToGLEnum(SamplerMipmapMode v) {
            switch (v) {
            case SamplerMipmapMode::None:
                return GL_NONE;
            case SamplerMipmapMode::Nearest:
                return GL_NEAREST_MIPMAP_NEAREST;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertSamplerWrapModeToGLEnum(SamplerWrapMode v) {
            switch (v) {
            case SamplerWrapMode::ClampToEdge:
                return GL_CLAMP_TO_EDGE;
            case SamplerWrapMode::MirroredRepeat:
                return GL_MIRRORED_REPEAT;
            case SamplerWrapMode::Repeat:
                return GL_REPEAT;
            case SamplerWrapMode::ClampToBorder:
                return GL_CLAMP_TO_BORDER;
            case SamplerWrapMode::MirrorClampToEdge:
                return GL_MIRROR_CLAMP_TO_EDGE;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertSamplerCompareModeToGLEnum(SamplerCompareMode v) {
            switch (v) {
            case SamplerCompareMode::None:
                return GL_NONE;
            case SamplerCompareMode::CompareToTexture:
                return GL_COMPARE_REF_TO_TEXTURE;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertSamplerCompareFuncToGLEnum(SamplerCompareFunc v) {
            switch (v) {
            case SamplerCompareFunc::Never:
                return GL_NEVER;
            case SamplerCompareFunc::Less:
                return GL_LESS;
            case SamplerCompareFunc::Equal:
                return GL_EQUAL;
            case SamplerCompareFunc::LessEqual:
                return GL_LEQUAL;
            case SamplerCompareFunc::Greater:
                return GL_GREATER;
            case SamplerCompareFunc::NotEqual:
                return GL_NOTEQUAL;
            case SamplerCompareFunc::GreaterEqual:
                return GL_GEQUAL;
            case SamplerCompareFunc::Always:
                return GL_ALWAYS;
            default:
                return GL_UNKNOWN_MGL;
            }
        }

        GLenum ConvertTextureSwizzleParamToGLEnum(TextureSwizzleParam v) {
            switch (v) {
            case TextureSwizzleParam::Red:
                return GL_RED;
            case TextureSwizzleParam::Green:
                return GL_GREEN;
            case TextureSwizzleParam::Blue:
                return GL_BLUE;
            case TextureSwizzleParam::Alpha:
                return GL_ALPHA;
            case TextureSwizzleParam::Zero:
                return GL_ZERO;
            case TextureSwizzleParam::One:
                return GL_ONE;
            default:
                return GL_UNKNOWN_MGL;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
