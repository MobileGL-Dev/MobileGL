// MobileGL - MobileGL/MG_Util/Texture/TextureFormatProcessor.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "TextureFormatProcessor.h"
#include "MG_Util/Converters/GLToStr/GLEnumConverter.h"

namespace MobileGL::MG_Util::TextureFormatProcessor {
    void NormalizePixelFormat(GLenum internalFormat, Flags<PixelFormatNormalizeOptionBit> options,
                              GLenum* outInternalFormat, GLenum* outFormat, GLenum* outType) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // internal format
        if (outInternalFormat) {
            switch (internalFormat) {
            case GL_DEPTH_COMPONENT32:
                *outInternalFormat = GL_DEPTH_COMPONENT;
                break;
            case GL_RGBA16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RGBA32F;
                    break;
                }
            case GL_RGB16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RGB32F;
                    break;
                }
            case GL_RG16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RG32F;
                    break;
                }
            case GL_R16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_R32F;
                    break;
                }
            case GL_RGBA16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
            case GL_RGB16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RGB16F;
                    break;
                }
            case GL_RG16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RG16F;
                    break;
                }
            case GL_R16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_R16F;
                    break;
                }
            default:
                *outInternalFormat = internalFormat;
                break;
            }
        }

        // format
        if (outFormat) {
            switch (internalFormat) {
            // Color Unsigned Normalized
            case GL_RGBA:
            case GL_RGBA16:
            case GL_RGBA8:
                *outFormat = GL_RGBA;
                break;

            case GL_RGB:
            case GL_RGB16:
            case GL_RGB8:
                *outFormat = GL_RGB;
                break;

            case GL_RG:
            case GL_RG16:
            case GL_RG8:
                *outFormat = GL_RG;
                break;

            case GL_RED:
            case GL_R16:
            case GL_R8:
                *outFormat = GL_RED;
                break;

            // Color Signed Normalized
            case GL_RGBA_SNORM:
            case GL_RGBA16_SNORM:
            case GL_RGBA8_SNORM:
                *outFormat = GL_RGBA;
                break;

            case GL_RGB_SNORM:
            case GL_RGB16_SNORM:
            case GL_RGB8_SNORM:
                *outFormat = GL_RGB;
                break;

            case GL_RG_SNORM:
            case GL_RG16_SNORM:
            case GL_RG8_SNORM:
                *outFormat = GL_RG;
                break;

            case GL_RED_SNORM:
            case GL_R16_SNORM:
            case GL_R8_SNORM:
                *outFormat = GL_RED;
                break;

            // Color Integer
            case GL_RGBA32UI:
            case GL_RGBA16UI:
            case GL_RGBA8UI:
            case GL_RGBA32I:
            case GL_RGBA16I:
            case GL_RGBA8I:
                *outFormat = GL_RGBA_INTEGER;
                break;
            case GL_RGB32UI:
            case GL_RGB16UI:
            case GL_RGB8UI:
            case GL_RGB32I:
            case GL_RGB16I:
            case GL_RGB8I:
                *outFormat = GL_RGB_INTEGER;
                break;
            case GL_RG32UI:
            case GL_RG16UI:
            case GL_RG8UI:
            case GL_RG32I:
            case GL_RG16I:
            case GL_RG8I:
                *outFormat = GL_RG_INTEGER;
                break;
            case GL_R32UI:
            case GL_R16UI:
            case GL_R8UI:
            case GL_R32I:
            case GL_R16I:
            case GL_R8I:
                *outFormat = GL_RED_INTEGER;
                break;

            // Color Float
            case GL_RGBA32F:
            case GL_RGBA16F:
                *outFormat = GL_RGBA;
                break;
            case GL_RGB32F:
            case GL_RGB16F:
                *outFormat = GL_RGB;
                break;
            case GL_RG32F:
            case GL_RG16F:
                *outFormat = GL_RG;
                break;
            case GL_R32F:
            case GL_R16F:
                *outFormat = GL_RED;
                break;

            // Color sRGB
            case GL_SRGB:
            case GL_SRGB8:
                *outFormat = GL_RGB;
                break;
            case GL_SRGB8_ALPHA8:
                *outFormat = GL_RGBA;
                break;

            // Color sized other
            case GL_RGB9_E5:
            case GL_R11F_G11F_B10F:
                *outFormat = GL_RGB;
                break;
            case GL_RGB10_A2:
            case GL_RGB5_A1:
                *outFormat = GL_RGBA;
                break;

            // Depth
            case GL_DEPTH_COMPONENT16:
            case GL_DEPTH_COMPONENT24:
            case GL_DEPTH_COMPONENT32:
            case GL_DEPTH_COMPONENT32F:
            case GL_DEPTH_COMPONENT:
                *outFormat = GL_DEPTH_COMPONENT;
                break;

            // Depth Stencil
            case GL_DEPTH24_STENCIL8:
            case GL_DEPTH32F_STENCIL8:
            case GL_DEPTH_STENCIL:
                *outFormat = GL_DEPTH_STENCIL;
                break;

            default:
                MGLOG_E("NormalizePixelFormat: outFormat: unhandled internalFormat: %s",
                        MG_Util::ConvertGLEnumToString(internalFormat).c_str());
                // Fallback handling for other formats
                // Try to infer format from internal format name
                if (strstr(MG_Util::ConvertGLEnumToString(internalFormat).c_str(), "RGBA") != nullptr) {
                    *outFormat = GL_RGBA;
                } else if (strstr(MG_Util::ConvertGLEnumToString(internalFormat).c_str(), "RGB") != nullptr) {
                    *outFormat = GL_RGB;
                } else if (strstr(MG_Util::ConvertGLEnumToString(internalFormat).c_str(), "RG") != nullptr) {
                    *outFormat = GL_RG;
                } else if (strstr(MG_Util::ConvertGLEnumToString(internalFormat).c_str(), "RED") != nullptr) {
                    *outFormat = GL_RED;
                } else {
                    *outFormat = GL_RGBA; // Ultimate fallback
                }
                break;
            }
        }

        // type
        if (outType) {
            switch (internalFormat) {
            // Color Unsigned Normalized
            case GL_RGBA16:
            case GL_RGB16:
            case GL_RG16:
            case GL_R16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    // converted to GL_RGBA32F
                    *outType = GL_FLOAT;
                    break;
                } else {
                    *outType = GL_UNSIGNED_SHORT;
                    break;
                }
            case GL_RGBA8:
            case GL_RGB8:
            case GL_RG8:
            case GL_R8:
                *outType = GL_UNSIGNED_BYTE;
                break;

            // Color Signed Normalized
            case GL_RGBA16_SNORM:
            case GL_RGB16_SNORM:
            case GL_RG16_SNORM:
            case GL_R16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outType = GL_FLOAT;
                    break;
                } else {
                    *outType = GL_SHORT;
                    break;
                }
            case GL_RGBA8_SNORM:
            case GL_RGB8_SNORM:
            case GL_RG8_SNORM:
            case GL_R8_SNORM:
                *outType = GL_BYTE;
                break;

            // Color Unsigned Integer
            case GL_RGBA32UI:
            case GL_RGB32UI:
            case GL_RG32UI:
            case GL_R32UI:
                *outType = GL_UNSIGNED_INT;
                break;
            case GL_RGBA16UI:
            case GL_RGB16UI:
            case GL_RG16UI:
            case GL_R16UI:
                *outType = GL_UNSIGNED_SHORT;
                break;
            case GL_RGBA8UI:
            case GL_RGB8UI:
            case GL_RG8UI:
            case GL_R8UI:
                *outType = GL_UNSIGNED_BYTE;
                break;

            // Color Integer
            case GL_RGBA32I:
            case GL_RGB32I:
            case GL_RG32I:
            case GL_R32I:
                *outType = GL_INT;
                break;
            case GL_RGBA16I:
            case GL_RGB16I:
            case GL_RG16I:
            case GL_R16I:
                *outType = GL_SHORT;
                break;
            case GL_RGBA8I:
            case GL_RGB8I:
            case GL_RG8I:
            case GL_R8I:
                *outType = GL_BYTE;
                break;

            // Color Float
            case GL_RGBA32F:
            case GL_RGB32F:
            case GL_RG32F:
            case GL_R32F:
                *outType = GL_FLOAT;
                break;
            case GL_RGBA16F:
            case GL_RGB16F:
            case GL_RG16F:
            case GL_R16F:
                *outType = GL_HALF_FLOAT;
                break;

            // Color sRGB
            case GL_SRGB8:
                *outType = GL_UNSIGNED_BYTE;
                break;
            case GL_SRGB8_ALPHA8:
                *outType = GL_UNSIGNED_BYTE;
                break;

            // Color sized other
            case GL_RGB9_E5:
                *outType = GL_UNSIGNED_INT_5_9_9_9_REV;
                break;
            case GL_R11F_G11F_B10F:
                *outType = GL_UNSIGNED_INT_10F_11F_11F_REV;
                break;
            case GL_RGB10_A2:
                *outType = GL_UNSIGNED_INT_2_10_10_10_REV;
                break;
            case GL_RGB5_A1:
                *outType = GL_UNSIGNED_SHORT_5_5_5_1;
                break;

                // Depth
            case GL_DEPTH_COMPONENT16:
                *outType = GL_UNSIGNED_SHORT;
                break;
            case GL_DEPTH_COMPONENT24:
                *outType = GL_UNSIGNED_INT;
                break;
            case GL_DEPTH_COMPONENT32:
                *outType = GL_UNSIGNED_INT;
                break;
            case GL_DEPTH_COMPONENT32F:
                *outType = GL_FLOAT;
                break;
            case GL_DEPTH_COMPONENT:
                *outType = GL_UNSIGNED_INT;
                break;

            // Depth Stencil
            case GL_DEPTH32F_STENCIL8:
                *outType = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
                break;
            case GL_DEPTH24_STENCIL8:
            case GL_DEPTH_STENCIL:
                *outType = GL_UNSIGNED_INT_24_8;
                break;

            default:
                MGLOG_E("NormalizePixelFormat: outType: unhandled internalFormat: %s",
                        MG_Util::ConvertGLEnumToString(internalFormat).c_str());
                // Fallback handling for other formats
                *outType = GL_UNSIGNED_BYTE;
                break;
            }
        }
    }
} // namespace MobileGL::MG_Util::TextureFormatProcessor
