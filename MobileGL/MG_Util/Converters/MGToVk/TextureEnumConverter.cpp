// MobileGL - MobileGL/MG_Util/Converters/MGToVk/TextureEnumConverter.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureEnumConverter.h"
#include "MG_Util/Types.h"

namespace MobileGL {
    namespace MG_Util {
        VkImageViewType ConvertTextureTargetToVkEnum(TextureTarget target) {
            switch (target) {
            case TextureTarget::Texture1D:
                return VK_IMAGE_VIEW_TYPE_1D;
            case TextureTarget::Texture2D:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureTarget::Texture3D:
                return VK_IMAGE_VIEW_TYPE_3D;
            case TextureTarget::TextureCubeMap:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureTarget::Texture2DArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case TextureTarget::Texture2DMultisample:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureTarget::TextureCubeMapArray:
                return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case TextureTarget::Texture1DArray:
                return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            case TextureTarget::TextureRectangle:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureTarget::Texture2DMultisampleArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case TextureTarget::TextureBuffer:
            default:
                return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
            }
        }

        VkFormat ConvertTextureInputFormatToVkEnum(TextureInputFormat format) {
            switch (format) {
            case TextureInputFormat::Red:
                return VK_FORMAT_R8_UNORM;
            case TextureInputFormat::RG:
                return VK_FORMAT_R8G8_UNORM;
            case TextureInputFormat::RGB:
                return VK_FORMAT_R8G8B8_UNORM;
            case TextureInputFormat::BGR:
                return VK_FORMAT_B8G8R8_UNORM;
            case TextureInputFormat::RGBA:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureInputFormat::BGRA:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case TextureInputFormat::RInteger:
                return VK_FORMAT_R8_UINT;
            case TextureInputFormat::RGInteger:
                return VK_FORMAT_R8G8_UINT;
            case TextureInputFormat::RGBInteger:
                return VK_FORMAT_R8G8B8_UINT;
            case TextureInputFormat::BGRInteger:
                return VK_FORMAT_B8G8R8_UINT;
            case TextureInputFormat::RGBAInteger:
                return VK_FORMAT_R8G8B8A8_UINT;
            case TextureInputFormat::BGRAInteger:
                return VK_FORMAT_B8G8R8A8_UINT;
            case TextureInputFormat::StencilIndex:
                return VK_FORMAT_S8_UINT;
            case TextureInputFormat::DepthComponent:
                return VK_FORMAT_D32_SFLOAT;
            case TextureInputFormat::DepthStencil:
                return VK_FORMAT_D24_UNORM_S8_UINT;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }

        VkFormat ConvertTextureInternalFormatToVkEnum(TextureInternalFormat format) {
            switch (format) {
            case TextureInternalFormat::R8:
                return VK_FORMAT_R8_UNORM;
            case TextureInternalFormat::R8Snorm:
                return VK_FORMAT_R8_SNORM;
            case TextureInternalFormat::R16:
                return VK_FORMAT_R16_UNORM;
            case TextureInternalFormat::R16Snorm:
                return VK_FORMAT_R16_SNORM;
            case TextureInternalFormat::RG8:
                return VK_FORMAT_R8G8_UNORM;
            case TextureInternalFormat::RG8Snorm:
                return VK_FORMAT_R8G8_SNORM;
            case TextureInternalFormat::RG16:
                return VK_FORMAT_R16G16_UNORM;
            case TextureInternalFormat::RG16Snorm:
                return VK_FORMAT_R16G16_SNORM;
            case TextureInternalFormat::R3G3B2:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB4:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB5:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB8:
                return VK_FORMAT_R8G8B8_UNORM;
            case TextureInternalFormat::RGB8Snorm:
                return VK_FORMAT_R8G8B8_SNORM;
            case TextureInternalFormat::RGB10:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB12:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB16:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGB16Snorm:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGBA2:
                return VK_FORMAT_UNDEFINED;
            case TextureInternalFormat::RGBA4:
                return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            case TextureInternalFormat::RGB5A1:
                return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
            case TextureInternalFormat::RGBA8:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureInternalFormat::RGBA8Snorm:
                return VK_FORMAT_R8G8B8A8_SNORM;
            case TextureInternalFormat::RGB10A2:
                return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            case TextureInternalFormat::RGB10A2UI:
                return VK_FORMAT_A2R10G10B10_UINT_PACK32;
            case TextureInternalFormat::RGBA16:
                return VK_FORMAT_R16G16B16A16_UNORM;
            case TextureInternalFormat::RGBA16Snorm:
                return VK_FORMAT_R16G16B16A16_SNORM;
            case TextureInternalFormat::SRGB8:
                return VK_FORMAT_R8G8B8_SRGB;
            case TextureInternalFormat::SRGB8Alpha8:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case TextureInternalFormat::R16F:
                return VK_FORMAT_R16_SFLOAT;
            case TextureInternalFormat::RG16F:
                return VK_FORMAT_R16G16_SFLOAT;
            case TextureInternalFormat::RGB16F:
                return VK_FORMAT_R16G16B16_SFLOAT;
            case TextureInternalFormat::RGBA16F:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case TextureInternalFormat::R32F:
                return VK_FORMAT_R32_SFLOAT;
            case TextureInternalFormat::RG32F:
                return VK_FORMAT_R32G32_SFLOAT;
            case TextureInternalFormat::RGB32F:
                return VK_FORMAT_R32G32B32_SFLOAT;
            case TextureInternalFormat::RGBA32F:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case TextureInternalFormat::R11FG11FB10F:
                return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case TextureInternalFormat::RGB9E5:
                return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            case TextureInternalFormat::R8I:
                return VK_FORMAT_R8_SINT;
            case TextureInternalFormat::R8UI:
                return VK_FORMAT_R8_UINT;
            case TextureInternalFormat::R16I:
                return VK_FORMAT_R16_SINT;
            case TextureInternalFormat::R16UI:
                return VK_FORMAT_R16_UINT;
            case TextureInternalFormat::R32I:
                return VK_FORMAT_R32_SINT;
            case TextureInternalFormat::R32UI:
                return VK_FORMAT_R32_UINT;
            case TextureInternalFormat::RG8I:
                return VK_FORMAT_R8G8_SINT;
            case TextureInternalFormat::RG8UI:
                return VK_FORMAT_R8G8_UINT;
            case TextureInternalFormat::RG16I:
                return VK_FORMAT_R16G16_SINT;
            case TextureInternalFormat::RG16UI:
                return VK_FORMAT_R16G16_UINT;
            case TextureInternalFormat::RG32I:
                return VK_FORMAT_R32G32_SINT;
            case TextureInternalFormat::RG32UI:
                return VK_FORMAT_R32G32_UINT;
            case TextureInternalFormat::RGB8I:
                return VK_FORMAT_R8G8B8_SINT;
            case TextureInternalFormat::RGB8UI:
                return VK_FORMAT_R8G8B8_UINT;
            case TextureInternalFormat::RGB16I:
                return VK_FORMAT_R16G16B16_SINT;
            case TextureInternalFormat::RGB16UI:
                return VK_FORMAT_R16G16B16_UINT;
            case TextureInternalFormat::RGB32I:
                return VK_FORMAT_R32G32B32_SINT;
            case TextureInternalFormat::RGB32UI:
                return VK_FORMAT_R32G32B32_UINT;
            case TextureInternalFormat::RGBA8I:
                return VK_FORMAT_R8G8B8A8_SINT;
            case TextureInternalFormat::RGBA8UI:
                return VK_FORMAT_R8G8B8A8_UINT;
            case TextureInternalFormat::RGBA16I:
                return VK_FORMAT_R16G16B16A16_SINT;
            case TextureInternalFormat::RGBA16UI:
                return VK_FORMAT_R16G16B16A16_UINT;
            case TextureInternalFormat::RGBA32I:
                return VK_FORMAT_R32G32B32A32_SINT;
            case TextureInternalFormat::RGBA32UI:
                return VK_FORMAT_R32G32B32A32_UINT;
            case TextureInternalFormat::DepthComponent:
                return VK_FORMAT_D32_SFLOAT;
            case TextureInternalFormat::DepthComponent16:
                return VK_FORMAT_D16_UNORM;
            case TextureInternalFormat::DepthComponent24:
                return VK_FORMAT_X8_D24_UNORM_PACK32;
            case TextureInternalFormat::DepthComponent32F:
                return VK_FORMAT_D32_SFLOAT;
            case TextureInternalFormat::Depth24Stencil8:
                return VK_FORMAT_D24_UNORM_S8_UINT;
            case TextureInternalFormat::Depth32FStencil8:
                return VK_FORMAT_D32_SFLOAT_S8_UINT;
            case TextureInternalFormat::DepthComponent32:
                return VK_FORMAT_D32_SFLOAT;
            case TextureInternalFormat::DepthStencil:
                return VK_FORMAT_D24_UNORM_S8_UINT;
            case TextureInternalFormat::Red:
                return VK_FORMAT_R8_UNORM;
            case TextureInternalFormat::RG:
                return VK_FORMAT_R8G8_UNORM;
            case TextureInternalFormat::RGB:
                return VK_FORMAT_R8G8B8_UNORM;
            case TextureInternalFormat::RGBA:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureInternalFormat::RGBA12:
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }

        VkFormat ConvertTexturePixelDataTypeToVkEnum(TexturePixelDataType type) {
            switch (type) {
            case TexturePixelDataType::UnsignedByte:
                return VK_FORMAT_R8_UINT;
            case TexturePixelDataType::Byte:
                return VK_FORMAT_R8_SINT;
            case TexturePixelDataType::UnsignedShort:
                return VK_FORMAT_R16_UINT;
            case TexturePixelDataType::Short:
                return VK_FORMAT_R16_SINT;
            case TexturePixelDataType::UnsignedInt:
                return VK_FORMAT_R32_UINT;
            case TexturePixelDataType::Int:
                return VK_FORMAT_R32_SINT;
            case TexturePixelDataType::Float:
                return VK_FORMAT_R32_SFLOAT;
            case TexturePixelDataType::HalfFloat:
                return VK_FORMAT_R16_SFLOAT;
            case TexturePixelDataType::UnsignedInt248:
                return VK_FORMAT_D24_UNORM_S8_UINT;
            case TexturePixelDataType::UnsignedByte332:
                return VK_FORMAT_R8_UINT;
            case TexturePixelDataType::UnsignedByte233Rev:
                return VK_FORMAT_R8_UINT;
            case TexturePixelDataType::UnsignedShort565:
                return VK_FORMAT_R5G6B5_UNORM_PACK16;
            case TexturePixelDataType::UnsignedShort565Rev:
                return VK_FORMAT_R5G6B5_UNORM_PACK16;
            case TexturePixelDataType::UnsignedShort4444:
                return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            case TexturePixelDataType::UnsignedShort4444Rev:
                return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
            case TexturePixelDataType::UnsignedShort5551:
                return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
            case TexturePixelDataType::UnsignedShort1555Rev:
                return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
            case TexturePixelDataType::UnsignedInt8888:
                return VK_FORMAT_R8G8B8A8_UINT;
            case TexturePixelDataType::UnsignedInt8888Rev:
                return VK_FORMAT_R8G8B8A8_UINT;
            case TexturePixelDataType::UnsignedInt1010102:
                return VK_FORMAT_A2R10G10B10_UINT_PACK32;
            case TexturePixelDataType::UnsignedInt2101010Rev:
                return VK_FORMAT_A2R10G10B10_UINT_PACK32;
            case TexturePixelDataType::UnsignedInt101111Rev:
                return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case TexturePixelDataType::UnsignedInt5999Rev:
                return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            case TexturePixelDataType::Float32UnsignedInt248Rev:
                return VK_FORMAT_D32_SFLOAT_S8_UINT;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }

        VkImageViewType ConvertTextureUploadTargetToVkEnum(TextureUploadTarget target) {
            switch (target) {
            case TextureUploadTarget::Texture2D:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::Texture1D:
                return VK_IMAGE_VIEW_TYPE_1D;
            case TextureUploadTarget::Texture3D:
                return VK_IMAGE_VIEW_TYPE_3D;
            case TextureUploadTarget::ProxyTexture2D:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::Texture1DArray:
                return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            case TextureUploadTarget::ProxyTexture1DArray:
                return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            case TextureUploadTarget::TextureRectangle:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::ProxyTextureRectangle:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::CubeMapPositiveX:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::CubeMapNegativeX:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::CubeMapPositiveY:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::CubeMapNegativeY:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::CubeMapPositiveZ:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::CubeMapNegativeZ:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::ProxyCubeMap:
                return VK_IMAGE_VIEW_TYPE_CUBE;
            case TextureUploadTarget::Texture2DMultisample:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::ProxyTexture2DMultisample:
                return VK_IMAGE_VIEW_TYPE_2D;
            case TextureUploadTarget::CubeMapArray:
                return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case TextureUploadTarget::ProxyCubeMapArray:
                return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case TextureUploadTarget::Texture2DArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case TextureUploadTarget::ProxyTexture2DArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case TextureUploadTarget::Texture2DMultisampleArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case TextureUploadTarget::ProxyTexture2DMultisampleArray:
                return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            default:
                return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
            }
        }

        VkFilter ConvertSamplerFilterModeToVkEnum(SamplerFilterMode filter, SamplerMipmapMode mipmap) {
            (void)mipmap;
            switch (filter) {
            case SamplerFilterMode::Nearest:
                return VK_FILTER_NEAREST;
            case SamplerFilterMode::Linear:
                return VK_FILTER_LINEAR;
            default:
                return VK_FILTER_NEAREST;
            }
        }

        VkSamplerMipmapMode ConvertSamplerMipmapModeToVkEnum(SamplerMipmapMode v) {
            switch (v) {
            case SamplerMipmapMode::Nearest:
                return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            case SamplerMipmapMode::Linear:
                return VK_SAMPLER_MIPMAP_MODE_LINEAR;
            case SamplerMipmapMode::None:
            default:
                return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            }
        }

        VkSamplerAddressMode ConvertSamplerWrapModeToVkEnum(SamplerWrapMode v) {
            switch (v) {
            case SamplerWrapMode::ClampToEdge:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case SamplerWrapMode::MirroredRepeat:
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SamplerWrapMode::Repeat:
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerWrapMode::ClampToBorder:
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            case SamplerWrapMode::MirrorClampToEdge:
                return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            default:
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        }

        VkBool32 ConvertSamplerCompareModeToVkEnum(SamplerCompareMode v) {
            switch (v) {
            case SamplerCompareMode::None:
                return VK_FALSE;
            case SamplerCompareMode::CompareToTexture:
                return VK_TRUE;
            default:
                return VK_FALSE;
            }
        }

        VkCompareOp ConvertSamplerCompareFuncToVkEnum(SamplerCompareFunc v) {
            switch (v) {
            case SamplerCompareFunc::Never:
                return VK_COMPARE_OP_NEVER;
            case SamplerCompareFunc::Less:
                return VK_COMPARE_OP_LESS;
            case SamplerCompareFunc::Equal:
                return VK_COMPARE_OP_EQUAL;
            case SamplerCompareFunc::LessEqual:
                return VK_COMPARE_OP_LESS_OR_EQUAL;
            case SamplerCompareFunc::Greater:
                return VK_COMPARE_OP_GREATER;
            case SamplerCompareFunc::NotEqual:
                return VK_COMPARE_OP_NOT_EQUAL;
            case SamplerCompareFunc::GreaterEqual:
                return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case SamplerCompareFunc::Always:
                return VK_COMPARE_OP_ALWAYS;
            default:
                return VK_COMPARE_OP_ALWAYS;
            }
        }

        VkComponentSwizzle ConvertTextureSwizzleParamToVkEnum(TextureSwizzleParam v) {
            switch (v) {
            case TextureSwizzleParam::Red:
                return VK_COMPONENT_SWIZZLE_R;
            case TextureSwizzleParam::Green:
                return VK_COMPONENT_SWIZZLE_G;
            case TextureSwizzleParam::Blue:
                return VK_COMPONENT_SWIZZLE_B;
            case TextureSwizzleParam::Alpha:
                return VK_COMPONENT_SWIZZLE_A;
            case TextureSwizzleParam::Zero:
                return VK_COMPONENT_SWIZZLE_ZERO;
            case TextureSwizzleParam::One:
                return VK_COMPONENT_SWIZZLE_ONE;
            default:
                return VK_COMPONENT_SWIZZLE_IDENTITY;
            }
        }
    } // namespace MG_Util
} // namespace MobileGL
