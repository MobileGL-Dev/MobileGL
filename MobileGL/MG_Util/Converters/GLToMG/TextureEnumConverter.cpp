// MobileGL - MobileGL/MG_Util/Converters/GLToMG/TextureEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumConverter.h"
#include "MG_Util/Converters/GLToStr/GLEnumConverter.h"

namespace MobileGL {
    namespace MG_Util {
        TextureTarget ConvertGLEnumToTextureTarget(GLenum target) {
            switch (target) {
            case GL_TEXTURE_1D:
            case GL_PROXY_TEXTURE_1D:
                return TextureTarget::Texture1D;
            case GL_TEXTURE_2D:
            case GL_PROXY_TEXTURE_2D:
                return TextureTarget::Texture2D;
            case GL_TEXTURE_3D:
            case GL_PROXY_TEXTURE_3D:
                return TextureTarget::Texture3D;
            case GL_TEXTURE_CUBE_MAP:
            case GL_PROXY_TEXTURE_CUBE_MAP:
            case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
            case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
            case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
                return TextureTarget::TextureCubeMap;
            case GL_TEXTURE_2D_ARRAY:
            case GL_PROXY_TEXTURE_2D_ARRAY:
                return TextureTarget::Texture2DArray;
            case GL_TEXTURE_2D_MULTISAMPLE:
            case GL_PROXY_TEXTURE_2D_MULTISAMPLE:
                return TextureTarget::Texture2DMultisample;
            case GL_TEXTURE_CUBE_MAP_ARRAY:
            case GL_PROXY_TEXTURE_CUBE_MAP_ARRAY:
                return TextureTarget::TextureCubeMapArray;
            case GL_TEXTURE_BUFFER:
                return TextureTarget::TextureBuffer;
            case GL_TEXTURE_1D_ARRAY:
            case GL_PROXY_TEXTURE_1D_ARRAY:
                return TextureTarget::Texture1DArray;
            case GL_TEXTURE_RECTANGLE:
            case GL_PROXY_TEXTURE_RECTANGLE:
                return TextureTarget::TextureRectangle;
            case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
            case GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY:
                return TextureTarget::Texture2DMultisampleArray;
            default:
                return TextureTarget::Unknown;
            }
        }

        TextureInputFormat ConvertGLEnumToTextureInputFormat(GLenum format) {
            switch (format) {
            case GL_ALPHA:
            case GL_RED:
                return TextureInputFormat::Red;
            case GL_RG:
                return TextureInputFormat::RG;
            case GL_RGB:
                return TextureInputFormat::RGB;
            case GL_BGR:
                return TextureInputFormat::BGR;
            case GL_RGBA:
                return TextureInputFormat::RGBA;
            case GL_BGRA:
                return TextureInputFormat::BGRA;
            case GL_RED_INTEGER:
                return TextureInputFormat::RInteger;
            case GL_RG_INTEGER:
                return TextureInputFormat::RGInteger;
            case GL_RGB_INTEGER:
                return TextureInputFormat::RGBInteger;
            case GL_BGR_INTEGER:
                return TextureInputFormat::BGRInteger;
            case GL_RGBA_INTEGER:
                return TextureInputFormat::RGBAInteger;
            case GL_BGRA_INTEGER:
                return TextureInputFormat::BGRAInteger;
            case GL_STENCIL_INDEX:
                return TextureInputFormat::StencilIndex;
            case GL_DEPTH_COMPONENT:
                return TextureInputFormat::DepthComponent;
            case GL_DEPTH_STENCIL:
                return TextureInputFormat::DepthStencil;
            default:
                return TextureInputFormat::Unknown;
            }
        }

        TextureInternalFormat ConvertGLEnumToTextureInternalFormat(GLenum internalformat) {
            switch (internalformat) {
            case GL_R8:
                return TextureInternalFormat::R8;
            case GL_R8_SNORM:
                return TextureInternalFormat::R8Snorm;
            case GL_R16:
                return TextureInternalFormat::R16;
            case GL_R16_SNORM:
                return TextureInternalFormat::R16Snorm;
            case GL_RG8:
                return TextureInternalFormat::RG8;
            case GL_RG8_SNORM:
                return TextureInternalFormat::RG8Snorm;
            case GL_RG16:
                return TextureInternalFormat::RG16;
            case GL_RG16_SNORM:
                return TextureInternalFormat::RG16Snorm;
            case GL_R3_G3_B2:
                return TextureInternalFormat::R3G3B2;
            case GL_RGB4:
                return TextureInternalFormat::RGB4;
            case GL_RGB5:
                return TextureInternalFormat::RGB5;
            case GL_RGB8:
                return TextureInternalFormat::RGB8;
            case GL_RGB8_SNORM:
                return TextureInternalFormat::RGB8Snorm;
            case GL_RGB10:
                return TextureInternalFormat::RGB10;
            case GL_RGB12:
                return TextureInternalFormat::RGB12;
            case GL_RGB16:
                return TextureInternalFormat::RGB16;
            case GL_RGB16_SNORM:
                return TextureInternalFormat::RGB16Snorm;
            case GL_RGBA2:
                return TextureInternalFormat::RGBA2;
            case GL_RGBA4:
                return TextureInternalFormat::RGBA4;
            case GL_RGB5_A1:
                return TextureInternalFormat::RGB5A1;
            case GL_RGBA8:
                return TextureInternalFormat::RGBA8;
            case GL_RGBA8_SNORM:
                return TextureInternalFormat::RGBA8Snorm;
            case GL_RGB10_A2:
                return TextureInternalFormat::RGB10A2;
            case GL_RGB10_A2UI:
                return TextureInternalFormat::RGB10A2UI;
            case GL_RGBA12:
                return TextureInternalFormat::RGBA12;
            case GL_RGBA16:
                return TextureInternalFormat::RGBA16;
            case GL_RGBA16_SNORM:
                return TextureInternalFormat::RGBA16Snorm;
            case GL_SRGB8:
                return TextureInternalFormat::SRGB8;
            case GL_SRGB8_ALPHA8:
                return TextureInternalFormat::SRGB8Alpha8;
            case GL_R16F:
                return TextureInternalFormat::R16F;
            case GL_RG16F:
                return TextureInternalFormat::RG16F;
            case GL_RGB16F:
                return TextureInternalFormat::RGB16F;
            case GL_RGBA16F:
                return TextureInternalFormat::RGBA16F;
            case GL_R32F:
                return TextureInternalFormat::R32F;
            case GL_RG32F:
                return TextureInternalFormat::RG32F;
            case GL_RGB32F:
                return TextureInternalFormat::RGB32F;
            case GL_RGBA32F:
                return TextureInternalFormat::RGBA32F;
            case GL_R11F_G11F_B10F:
                return TextureInternalFormat::R11FG11FB10F;
            case GL_RGB9_E5:
                return TextureInternalFormat::RGB9E5;
            case GL_R8I:
                return TextureInternalFormat::R8I;
            case GL_R8UI:
                return TextureInternalFormat::R8UI;
            case GL_R16I:
                return TextureInternalFormat::R16I;
            case GL_R16UI:
                return TextureInternalFormat::R16UI;
            case GL_R32I:
                return TextureInternalFormat::R32I;
            case GL_R32UI:
                return TextureInternalFormat::R32UI;
            case GL_RG8I:
                return TextureInternalFormat::RG8I;
            case GL_RG8UI:
                return TextureInternalFormat::RG8UI;
            case GL_RG16I:
                return TextureInternalFormat::RG16I;
            case GL_RG16UI:
                return TextureInternalFormat::RG16UI;
            case GL_RG32I:
                return TextureInternalFormat::RG32I;
            case GL_RG32UI:
                return TextureInternalFormat::RG32UI;
            case GL_RGB8I:
                return TextureInternalFormat::RGB8I;
            case GL_RGB8UI:
                return TextureInternalFormat::RGB8UI;
            case GL_RGB16I:
                return TextureInternalFormat::RGB16I;
            case GL_RGB16UI:
                return TextureInternalFormat::RGB16UI;
            case GL_RGB32I:
                return TextureInternalFormat::RGB32I;
            case GL_RGB32UI:
                return TextureInternalFormat::RGB32UI;
            case GL_RGBA8I:
                return TextureInternalFormat::RGBA8I;
            case GL_RGBA8UI:
                return TextureInternalFormat::RGBA8UI;
            case GL_RGBA16I:
                return TextureInternalFormat::RGBA16I;
            case GL_RGBA16UI:
                return TextureInternalFormat::RGBA16UI;
            case GL_RGBA32I:
                return TextureInternalFormat::RGBA32I;
            case GL_RGBA32UI:
                return TextureInternalFormat::RGBA32UI;
            case GL_DEPTH_COMPONENT16:
                return TextureInternalFormat::DepthComponent16;
            case GL_DEPTH_COMPONENT24:
                return TextureInternalFormat::DepthComponent24;
            case GL_DEPTH_COMPONENT32:
                return TextureInternalFormat::DepthComponent32;
            case GL_DEPTH_COMPONENT32F:
                return TextureInternalFormat::DepthComponent32F;
            case GL_DEPTH24_STENCIL8:
                return TextureInternalFormat::Depth24Stencil8;
            case GL_DEPTH32F_STENCIL8:
                return TextureInternalFormat::Depth32FStencil8;
            case GL_DEPTH_COMPONENT:
                return TextureInternalFormat::DepthComponent;
            case GL_DEPTH_STENCIL:
                return TextureInternalFormat::DepthStencil;
            case GL_ALPHA:
            case GL_RED:
                return TextureInternalFormat::Red;
            case GL_RG:
                return TextureInternalFormat::RG;
            case GL_RGB:
                return TextureInternalFormat::RGB;
            case GL_RGBA:
                return TextureInternalFormat::RGBA;
            default:
                MGLOG_D("%s: unknown internal format %s", __func__,
                        MG_Util::ConvertGLEnumToString(internalformat).c_str());
                return TextureInternalFormat::Unknown;
            }
        }

        TexturePixelDataType ConvertGLEnumToTexturePixelDataType(GLenum type) {
            switch (type) {
            case GL_UNSIGNED_BYTE:
                return TexturePixelDataType::UnsignedByte;
            case GL_BYTE:
                return TexturePixelDataType::Byte;
            case GL_UNSIGNED_SHORT:
                return TexturePixelDataType::UnsignedShort;
            case GL_SHORT:
                return TexturePixelDataType::Short;
            case GL_UNSIGNED_INT:
                return TexturePixelDataType::UnsignedInt;
            case GL_INT:
                return TexturePixelDataType::Int;
            case GL_FLOAT:
                return TexturePixelDataType::Float;
            case GL_HALF_FLOAT:
                return TexturePixelDataType::HalfFloat;
            case GL_UNSIGNED_INT_24_8:
                return TexturePixelDataType::UnsignedInt248;
            case GL_UNSIGNED_BYTE_3_3_2:
                return TexturePixelDataType::UnsignedByte332;
            case GL_UNSIGNED_BYTE_2_3_3_REV:
                return TexturePixelDataType::UnsignedByte233Rev;
            case GL_UNSIGNED_SHORT_5_6_5:
                return TexturePixelDataType::UnsignedShort565;
            case GL_UNSIGNED_SHORT_5_6_5_REV:
                return TexturePixelDataType::UnsignedShort565Rev;
            case GL_UNSIGNED_SHORT_4_4_4_4:
                return TexturePixelDataType::UnsignedShort4444;
            case GL_UNSIGNED_SHORT_4_4_4_4_REV:
                return TexturePixelDataType::UnsignedShort4444Rev;
            case GL_UNSIGNED_SHORT_5_5_5_1:
                return TexturePixelDataType::UnsignedShort5551;
            case GL_UNSIGNED_SHORT_1_5_5_5_REV:
                return TexturePixelDataType::UnsignedShort1555Rev;
            case GL_UNSIGNED_INT_8_8_8_8:
                return TexturePixelDataType::UnsignedInt8888;
            case GL_UNSIGNED_INT_8_8_8_8_REV:
                return TexturePixelDataType::UnsignedInt8888Rev;
            case GL_UNSIGNED_INT_10F_11F_11F_REV:
                return TexturePixelDataType::UnsignedInt101111Rev;
            case GL_UNSIGNED_INT_2_10_10_10_REV:
                return TexturePixelDataType::UnsignedInt2101010Rev;
            case GL_UNSIGNED_INT_5_9_9_9_REV:
                return TexturePixelDataType::UnsignedInt5999Rev;
            case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
                return TexturePixelDataType::Float32UnsignedInt248Rev;
            default:
                return TexturePixelDataType::Unknown;
            }
        }

        TextureUploadTarget ConvertGLEnumToTextureUploadTarget(GLenum target) {
            switch (target) {
            case GL_TEXTURE_1D:
                return TextureUploadTarget::Texture1D;
            case GL_TEXTURE_2D:
                return TextureUploadTarget::Texture2D;
            case GL_TEXTURE_3D:
                return TextureUploadTarget::Texture3D;
            case GL_PROXY_TEXTURE_2D:
                return TextureUploadTarget::ProxyTexture2D;
            case GL_TEXTURE_1D_ARRAY:
                return TextureUploadTarget::Texture1DArray;
            case GL_PROXY_TEXTURE_1D_ARRAY:
                return TextureUploadTarget::ProxyTexture1DArray;
            case GL_TEXTURE_2D_ARRAY:
                return TextureUploadTarget::Texture2DArray;
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                return TextureUploadTarget::CubeMapArray;
            case GL_PROXY_TEXTURE_CUBE_MAP_ARRAY:
                return TextureUploadTarget::ProxyCubeMapArray;
            case GL_PROXY_TEXTURE_2D_ARRAY:
                return TextureUploadTarget::ProxyTexture2DArray;
            case GL_TEXTURE_RECTANGLE:
                return TextureUploadTarget::TextureRectangle;
            case GL_PROXY_TEXTURE_RECTANGLE:
                return TextureUploadTarget::ProxyTextureRectangle;
            case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
                return TextureUploadTarget::CubeMapPositiveX;
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
                return TextureUploadTarget::CubeMapNegativeX;
            case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
                return TextureUploadTarget::CubeMapPositiveY;
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
                return TextureUploadTarget::CubeMapNegativeY;
            case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
                return TextureUploadTarget::CubeMapPositiveZ;
            case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
                return TextureUploadTarget::CubeMapNegativeZ;
            case GL_TEXTURE_BUFFER:
                return TextureUploadTarget::TextureBuffer;
            case GL_PROXY_TEXTURE_CUBE_MAP:
                return TextureUploadTarget::ProxyCubeMap;
            case GL_TEXTURE_2D_MULTISAMPLE:
                return TextureUploadTarget::Texture2DMultisample;
            case GL_PROXY_TEXTURE_2D_MULTISAMPLE:
                return TextureUploadTarget::ProxyTexture2DMultisample;
            case GL_TEXTURE_2D_MULTISAMPLE_ARRAY:
                return TextureUploadTarget::Texture2DMultisampleArray;
            case GL_PROXY_TEXTURE_2D_MULTISAMPLE_ARRAY:
                return TextureUploadTarget::ProxyTexture2DMultisampleArray;
            default:
                return TextureUploadTarget::Unknown;
            }
        }

        SamplerFilterMode ConvertGLEnumToSamplerFilterMode(GLenum v) {
            switch (v) {
            case GL_NEAREST:
            case GL_NEAREST_MIPMAP_NEAREST:
            case GL_NEAREST_MIPMAP_LINEAR:
                return SamplerFilterMode::Nearest;
            case GL_LINEAR:
            case GL_LINEAR_MIPMAP_NEAREST:
            case GL_LINEAR_MIPMAP_LINEAR:
                return SamplerFilterMode::Linear;
            default:
                MGLOG_D("Unknown SamplerFilterMode %s", MG_Util::ConvertGLEnumToString(v).c_str());
                return SamplerFilterMode::Unknown;
            }
        }

        SamplerMipmapMode ConvertGLEnumToSamplerMipmapMode(GLenum v) {
            switch (v) {
            case GL_NEAREST:
            case GL_LINEAR:
                return SamplerMipmapMode::None; // 无 mipmap 时
            case GL_NEAREST_MIPMAP_NEAREST:
            case GL_LINEAR_MIPMAP_NEAREST:
                return SamplerMipmapMode::Nearest;
            case GL_NEAREST_MIPMAP_LINEAR:
            case GL_LINEAR_MIPMAP_LINEAR:
                return SamplerMipmapMode::Linear;
            default:
                return SamplerMipmapMode::Unknown;
            }
        }

        SamplerWrapMode ConvertGLEnumToSamplerWrapMode(GLenum v) {
            switch (v) {
            case GL_CLAMP_TO_EDGE:
                return SamplerWrapMode::ClampToEdge;
            case GL_MIRRORED_REPEAT:
                return SamplerWrapMode::MirroredRepeat;
            case GL_REPEAT:
                return SamplerWrapMode::Repeat;
            case GL_CLAMP_TO_BORDER:
                return SamplerWrapMode::ClampToBorder;
            case GL_MIRROR_CLAMP_TO_EDGE:
                return SamplerWrapMode::MirrorClampToEdge;
            default:
                return SamplerWrapMode::Unknown;
            }
        }

        SamplerCompareMode ConvertGLEnumToSamplerCompareMode(GLenum v) {
            switch (v) {
            case 0:
                return SamplerCompareMode::None; // 无 compare
            case GL_COMPARE_REF_TO_TEXTURE:
                return SamplerCompareMode::CompareToTexture;
            default:
                return SamplerCompareMode::Unknown;
            }
        }

        SamplerCompareFunc ConvertGLEnumToSamplerCompareFunc(GLenum v) {
            switch (v) {
            case GL_NEVER:
                return SamplerCompareFunc::Never;
            case GL_LESS:
                return SamplerCompareFunc::Less;
            case GL_EQUAL:
                return SamplerCompareFunc::Equal;
            case GL_LEQUAL:
                return SamplerCompareFunc::LessEqual;
            case GL_GREATER:
                return SamplerCompareFunc::Greater;
            case GL_NOTEQUAL:
                return SamplerCompareFunc::NotEqual;
            case GL_GEQUAL:
                return SamplerCompareFunc::GreaterEqual;
            case GL_ALWAYS:
                return SamplerCompareFunc::Always;
            default:
                return SamplerCompareFunc::Unknown;
            }
        }

        TextureSwizzleParam ConvertGLEnumToTextureSwizzleParam(GLenum v) {
            switch (v) {
            case GL_RED:
                return TextureSwizzleParam::Red;
            case GL_GREEN:
                return TextureSwizzleParam::Green;
            case GL_BLUE:
                return TextureSwizzleParam::Blue;
            case GL_ALPHA:
                return TextureSwizzleParam::Alpha;
            case GL_ZERO:
                return TextureSwizzleParam::Zero;
            case GL_ONE:
                return TextureSwizzleParam::One;
            default:
                return TextureSwizzleParam::Unknown;
            }
        }

        TextureSwizzleParam ConvertGLEnumPnameToTextureSwizzleParam(GLenum v) {
            switch (v) {
            case GL_TEXTURE_SWIZZLE_R:
                return TextureSwizzleParam::Red;
            case GL_TEXTURE_SWIZZLE_G:
                return TextureSwizzleParam::Green;
            case GL_TEXTURE_SWIZZLE_B:
                return TextureSwizzleParam::Blue;
            case GL_TEXTURE_SWIZZLE_A:
                return TextureSwizzleParam::Alpha;
            default:
                return TextureSwizzleParam::Unknown;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
