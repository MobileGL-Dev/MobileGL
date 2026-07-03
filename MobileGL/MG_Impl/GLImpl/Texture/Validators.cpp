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

    Bool ValidateTextureInternalFormatCompatibleWithInput(TextureInputFormat format,
                                                          TextureInternalFormat internalFormat,
                                                          TexturePixelDataType type) {
        if (type == TexturePixelDataType::UnsignedByte332 || type == TexturePixelDataType::UnsignedByte233Rev ||
            type == TexturePixelDataType::UnsignedShort565 || type == TexturePixelDataType::UnsignedShort565Rev ||
            type == TexturePixelDataType::UnsignedInt101111Rev) {
            if (format != TextureInputFormat::RGB) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                                 "Invalid format for the given type"));
                return false;
            }
        }

        if (type == TexturePixelDataType::UnsignedShort4444 || type == TexturePixelDataType::UnsignedShort4444Rev ||
            type == TexturePixelDataType::UnsignedShort5551 || type == TexturePixelDataType::UnsignedShort1555Rev ||
            type == TexturePixelDataType::UnsignedInt8888 || type == TexturePixelDataType::UnsignedInt8888Rev ||
            type == TexturePixelDataType::UnsignedInt1010102 || type == TexturePixelDataType::UnsignedInt2101010Rev ||
            type == TexturePixelDataType::UnsignedInt5999Rev) {
            if (format != TextureInputFormat::RGBA && format != TextureInputFormat::BGRA) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                                 "Invalid format for the given type"));
                return false;
            }
        }

        if (internalFormat == TextureInternalFormat::DepthComponent ||
            internalFormat == TextureInternalFormat::DepthComponent16 ||
            internalFormat == TextureInternalFormat::DepthComponent24 ||
            internalFormat == TextureInternalFormat::DepthComponent32F) {
            if (format != TextureInputFormat::DepthComponent) {
                MG_State::pGLContext->RecordError(
                    ErrorCode::InvalidOperation,
                    MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                                 "Invalid format for depth component internal format"));
                return false;
            }
        }

        if (format == TextureInputFormat::DepthComponent &&
            (internalFormat != TextureInternalFormat::DepthComponent &&
             internalFormat != TextureInternalFormat::DepthComponent16 &&
             internalFormat != TextureInternalFormat::DepthComponent24 &&
             internalFormat != TextureInternalFormat::DepthComponent32F &&
             internalFormat != TextureInternalFormat::DepthComponent32 // workaround for Minecraft 1.21.5+
             )) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureInternalFormatCompatibleWithInput",
                                             "Invalid internal format for depth component format"));
            return false;
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

    Bool ValidateTextureObject(SharedPtr<MG_State::GLState::ITextureObject> textureObject) {
        if (!textureObject) {
            MG_State::pGLContext->RecordError(
                ErrorCode::InvalidOperation,
                MakeUnique<GenericErrorInfo>("MG_Impl/GLImpl", "ValidateTextureObject", "Texture object is null"));
            return false;
        }
        return true;
    }

    Bool ValidateTextureTargetUniformity(SharedPtr<MG_State::GLState::ITextureObject> textureObject,
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

    Bool ValidateTextureSubImageOffsets(SharedPtr<MG_State::GLState::ITextureObject> textureObject, Int xoffset,
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
