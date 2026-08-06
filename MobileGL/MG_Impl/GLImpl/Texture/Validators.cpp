// MobileGL - MobileGL/MG_Impl/GLImpl/Texture/Validators.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Validators.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_State/GLState/Core.h>
#include <MG_State/GLState/ErrorState/Error.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToGL/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToMG/TextureEnumConverter.h>
#include <MG_Util/Converters/MGToStr/TextureEnumConverter.h>

namespace MobileGL::MG_Impl::GLImpl::TextureImpl {
    Bool ValidateTextureTarget(TextureTarget target) {
        if (target == TextureTarget::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureTarget", "Invalid texture target"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureUploadTarget(TextureUploadTarget textureUploadTarget) {
        if (textureUploadTarget == TextureUploadTarget::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureUploadTarget",
                                                                     "Invalid texture upload target"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureName(Uint texture, Bool allowZero) {
        if (texture == 0) {
            if (allowZero) return true;

            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureName", "Texture name cannot be zero"));
            return false;
        }

        if (!MG_State::pGLContext->ValidateTextureName(texture)) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureName", "Invalid texture name"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureInputFormat(TextureInputFormat format) {
        if (format == TextureInputFormat::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInputFormat",
                                                                     "Invalid texture input format"));
            return false;
        }
        return true;
    }

    Bool ValidateTexturePixelDataType(TexturePixelDataType texturePixelDataType) {
        if (texturePixelDataType == TexturePixelDataType::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTexturePixelDataType",
                                                                     "Invalid texture pixel data type"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureLevelNumber(GLint level) {
        if (level < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelNumber",
                                                                      "Texture level must be non-negative"));
            return false;
        }

        Int maxTextureSize = MG_Backend::DynamicBackendParameters{}.MaxTextureSize;
        if (MG_Backend::pActiveBackendObject) {
            maxTextureSize = MG_Backend::pActiveBackendObject->GetDynamicParameters().MaxTextureSize;
        }
        Int maxLevel = 0;
        for (Int size = std::max(maxTextureSize, 1); size > 1; size >>= 1) {
            ++maxLevel;
        }
        if (level > maxLevel) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelNumber",
                                                                      "Texture level exceeds GL_MAX_TEXTURE_SIZE"));
            return false;
        }

        return true;
    }

    Bool ValidateTextureSizeWithTextureUploadTarget(TextureUploadTarget target, GLsizei width, GLsizei height) {
        if (target == TextureUploadTarget::CubeMapPositiveX || target == TextureUploadTarget::CubeMapNegativeX ||
            target == TextureUploadTarget::CubeMapPositiveY || target == TextureUploadTarget::CubeMapNegativeY ||
            target == TextureUploadTarget::CubeMapPositiveZ || target == TextureUploadTarget::CubeMapNegativeZ) {
            if (width != height) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Width and height must be equal for cube map textures"));
                return false;
            }
        }

        if (!(target == TextureUploadTarget::Texture1DArray || target == TextureUploadTarget::ProxyTexture1DArray)) {
            if (height < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Height must be greater than or equal to zero"));
                return false;
            }
            // TODO: GL_INVALID_VALUE is generated if target is not GL_TEXTURE_1D_ARRAY or GL_PROXY_TEXTURE_1D_ARRAY
            // and height is greater than GL_MAX_TEXTURE_SIZE.
        }

        if (target == TextureUploadTarget::Texture1DArray || target == TextureUploadTarget::ProxyTexture1DArray) {
            if (height < 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeWithTarget",
                                                 "Height must be greater than or equal to zero"));
                return false;
            }
            // TODO: GL_INVALID_VALUE is generated if target is GL_TEXTURE_1D_ARRAY or GL_PROXY_TEXTURE_1D_ARRAY and
            // height is greater than GL_MAX_ARRAY_TEXTURE_LAYERS.
        }

        return true;
    }

    Bool ValidateTextureSizeRange(Int width, Int height, Int depth) {
        if (width < 0 || height < 0 || depth < 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSizeRange",
                                                                      "Width and height must be greater than zero"));
            return false;
        }

        // TODO: GL_INVALID_VALUE is generated if width is greater than GL_MAX_TEXTURE_SIZE.

        return true;
    }

    Bool ValidateTextureInternalFormat(TextureInternalFormat format) {
        if (format == TextureInternalFormat::Unknown) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidEnum, MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormat",
                                                                     "Invalid texture sized internal format"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureBorderNumber(Int border) {
        if (border != 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureBorderNumber", "Border must be zero"));
            return false;
        }
        return true;
    }

    Bool IsIntegerColorInputFormat(TextureInputFormat format) {
        return format == TextureInputFormat::RInteger || format == TextureInputFormat::RGInteger ||
               format == TextureInputFormat::RGBInteger || format == TextureInputFormat::BGRInteger ||
               format == TextureInputFormat::RGBAInteger || format == TextureInputFormat::BGRAInteger ||
               format == TextureInputFormat::GreenInteger || format == TextureInputFormat::BlueInteger ||
               format == TextureInputFormat::AlphaInteger;
    }

    Bool IsIntegerColorInternalFormat(TextureInternalFormat internalFormat) {
        switch (internalFormat) {
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
            case TextureInternalFormat::RGB10A2UI:
                return true;
            default:
                return false;
        }
    }

    static Bool IsDepthLikeInternalFormat(TextureInternalFormat internalFormat) {
        switch (internalFormat) {
            case TextureInternalFormat::DepthComponent:
            case TextureInternalFormat::DepthComponent16:
            case TextureInternalFormat::DepthComponent24:
            case TextureInternalFormat::DepthComponent32: // not core, kept for Minecraft 1.21.5+
            case TextureInternalFormat::DepthComponent32F:
            case TextureInternalFormat::Depth24Stencil8:
            case TextureInternalFormat::Depth32FStencil8:
            case TextureInternalFormat::DepthStencil:
            // Stencil-only is not a colour format either: a colour client format read against a
            // STENCIL_INDEX8 texture has to be the same INVALID_OPERATION as against a depth one.
            case TextureInternalFormat::StencilIndex8:
                return true;
            default:
                return false;
        }
    }

    static Bool IsDepthLikeInputFormat(TextureInputFormat format) {
        return format == TextureInputFormat::DepthComponent || format == TextureInputFormat::DepthStencil ||
               format == TextureInputFormat::StencilIndex;
    }

    // Client-memory format<->type pairing rules shared by pixel uploads (TexImage*) and readbacks
    // (ReadPixels, GetTexImage). Mirrors the desktop-GL validity matrix used by GL CTS packed_pixels
    // (glcPackedPixelsTests isFormatValid): packed types constrain the formats they may pair with, and
    // integer formats reject floating-point types; violations raise GL_INVALID_OPERATION.
    Bool ValidateClientFormatTypePairing(TextureInputFormat format, TexturePixelDataType type) {
        const auto recordInvalidOperation = [](const char* message) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateClientFormatTypePairing", message));
            return false;
        };

        if (type == TexturePixelDataType::UnsignedByte332 || type == TexturePixelDataType::UnsignedByte233Rev ||
            type == TexturePixelDataType::UnsignedShort565 || type == TexturePixelDataType::UnsignedShort565Rev) {
            if (format != TextureInputFormat::RGB && format != TextureInputFormat::RGBInteger) {
                return recordInvalidOperation("Packed RGB type requires RGB or RGB_INTEGER format");
            }
        }

        if (type == TexturePixelDataType::UnsignedInt101111Rev || type == TexturePixelDataType::UnsignedInt5999Rev) {
            if (format != TextureInputFormat::RGB) {
                return recordInvalidOperation("Packed float RGB type requires RGB format");
            }
        }

        if (type == TexturePixelDataType::UnsignedShort4444 || type == TexturePixelDataType::UnsignedShort4444Rev ||
            type == TexturePixelDataType::UnsignedShort5551 || type == TexturePixelDataType::UnsignedShort1555Rev ||
            type == TexturePixelDataType::UnsignedInt8888 || type == TexturePixelDataType::UnsignedInt8888Rev ||
            type == TexturePixelDataType::UnsignedInt1010102 || type == TexturePixelDataType::UnsignedInt2101010Rev) {
            if (format != TextureInputFormat::RGBA && format != TextureInputFormat::BGRA &&
                format != TextureInputFormat::RGBAInteger && format != TextureInputFormat::BGRAInteger) {
                return recordInvalidOperation("Packed RGBA type requires RGBA/BGRA (integer) format");
            }
        }

        if (type == TexturePixelDataType::UnsignedInt248 || type == TexturePixelDataType::Float32UnsignedInt248Rev) {
            if (format != TextureInputFormat::DepthStencil) {
                return recordInvalidOperation("Packed depth-stencil type requires DEPTH_STENCIL format");
            }
        }

        if (format == TextureInputFormat::DepthStencil && type != TexturePixelDataType::UnsignedInt248 &&
            type != TexturePixelDataType::Float32UnsignedInt248Rev) {
            return recordInvalidOperation("DEPTH_STENCIL format requires a packed depth-stencil type");
        }

        if (IsIntegerColorInputFormat(format) &&
            (type == TexturePixelDataType::Float || type == TexturePixelDataType::HalfFloat)) {
            return recordInvalidOperation("Integer format cannot be used with a floating-point type");
        }

        return true;
    }

    // Mirrors the desktop-GL validity matrix used by GL CTS packed_pixels (glcPackedPixelsTests
    // isFormatValid, INPUT_TEXIMAGE): packed-type/format pairing, depth-vs-color mismatch, and
    // integer-ness matching all raise GL_INVALID_OPERATION instead of reaching the upload path.
    Bool ValidateTextureInternalFormatCompatibleWithInput(TextureInputFormat format,
                                                          TextureInternalFormat internalFormat,
                                                          TexturePixelDataType type) {
        const auto recordInvalidOperation = [](const char* message) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                             message));
            return false;
        };

        if (!ValidateClientFormatTypePairing(format, type)) {
            return false;
        }

        // TexImage in core 3.3 has no stencil-only upload path (that arrived with GL 4.4).
        if (format == TextureInputFormat::StencilIndex) {
            return recordInvalidOperation("STENCIL_INDEX is not a valid texture upload format");
        }

        if (IsDepthLikeInputFormat(format) != IsDepthLikeInternalFormat(internalFormat)) {
            return recordInvalidOperation("Depth/stencil-ness of format and internal format must match");
        }

        if (IsIntegerColorInputFormat(format) != IsIntegerColorInternalFormat(internalFormat)) {
            return recordInvalidOperation("Integer-ness of format and internal format must match");
        }

        return true;
    }

    Bool ValidateTextureLevelWithUploadTarget(TextureUploadTarget target, Int level) {
        if (target == TextureUploadTarget::TextureRectangle || target == TextureUploadTarget::ProxyTextureRectangle) {
            if (level != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelWithUploadTarget",
                                                 "Level must be zero for rectangle textures"));
                return false;
            }
        }
        if (target == TextureUploadTarget::Texture2DMultisample ||
            target == TextureUploadTarget::ProxyTexture2DMultisample ||
            target == TextureUploadTarget::Texture2DMultisampleArray ||
            target == TextureUploadTarget::ProxyTexture2DMultisampleArray) {
            if (level != 0) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidValue,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureLevelWithUploadTarget",
                                                 "Level must be zero for multisample textures"));
                return false;
            }
        }
        return true;
    }

    Bool ValidateTextureObject(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject) {
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureObject", "Texture object is null"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureNotDefault(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                   const char* caller) {
        if (textureObject && textureObject->GetExternalIndex() == 0) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", caller,
                                             "This operation is not allowed on the default texture (zero is "
                                             "bound to the target)."));
            return false;
        }
        return true;
    }

    Bool ValidateTextureTargetUniformity(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject,
                                         TextureTarget target) {
        if (!textureObject) return true; // should be created later
        TextureTarget prevTarget = textureObject->GetTarget();
        if (prevTarget != target) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureTargetUniformity",
                                             "Texture target does not match the previously created texture"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureSubImageOffsets(const SharedPtr<MG_State::GLState::ITextureObject>& textureObject, Int xoffset,
                                        Int width, Int yoffset, Int height, Int zoffset, Int depth) {
        auto baseSize = textureObject->GetBaseSize();
        if (xoffset < 0 || (xoffset + width) > baseSize.x()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "xoffset must be non-negative and (xoffset + width) must not exceed "
                                             "the texture width."));
            return false;
        }
        if (baseSize.y() == 0) return true;

        if (yoffset < 0 || (yoffset + height) > baseSize.y()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "yoffset must be non-negative and (yoffset + height) must not exceed "
                                             "the texture height."));
            return false;
        }
        if (baseSize.z() == 0) return true;

        if (zoffset < 0 || (zoffset + depth) > baseSize.z()) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidValue,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureSubImageOffsets",
                                             "zoffset must be non-negative and (zoffset + depth) must not exceed "
                                             "the texture depth."));
            return false;
        }
        return true;
    }

    Bool ValidateBaseInternalFormatMatch(TextureInternalFormat format1, TextureInternalFormat format2) {
        auto unsizedFormat1 = MG_Util::ConvertInternalFormatToUnsized(format1);
        auto unsizedFormat2 = MG_Util::ConvertInternalFormatToUnsized(format2);
        if (unsizedFormat1 != unsizedFormat2) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>(
                    std::format("MG_Impl/GLImpl", "ValidateBaseInternalFormatMatch",
                                "The base internal format of the two formats do not match ({} vs. {})",
                                MG_Util::ConvertTextureInternalFormatToString(unsizedFormat1).c_str(),
                                MG_Util::ConvertTextureInternalFormatToString(unsizedFormat2).c_str())));
            return false;
        }
        return true;
    } // namespace TextureImpl
} // namespace MobileGL::MG_Impl::GLImpl::TextureImpl
