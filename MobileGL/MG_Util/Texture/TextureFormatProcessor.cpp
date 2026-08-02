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
    Flags<PixelFormatNormalizeOptionBit>
    GetApplicablePixelFormatNormalizeOptions(GLenum internalFormat,
                                             Flags<PixelFormatNormalizeOptionBit> options) {
        Flags<PixelFormatNormalizeOptionBit> applicableOptions;
        switch (internalFormat) {
        case GL_DEPTH_COMPONENT32:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoDepthComponent32;
            break;
        case GL_RGBA16:
        case GL_RGBA12: // stored as RGBA16 (see NormalizePixelFormat)
        case GL_RG16:
        case GL_R16:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            break;
        case GL_RGB16:
        case GL_RGB10: // stored as RGB16 (see NormalizePixelFormat)
        case GL_RGB12: // stored as RGB16 (see NormalizePixelFormat)
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoRgb16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            break;
        case GL_RGB16_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoRGB16Snorm;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            if (options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget) {
                applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
            }
            break;
        case GL_RGBA16_SNORM:
        case GL_RG16_SNORM:
        case GL_R16_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16;
            break;
        case GL_RGBA8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm;
            break;
        case GL_RGB8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            break;
        case GL_RG8_SNORM:
        case GL_R8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            break;
        default:
            break;
        }
        return applicableOptions;
    }

    void NormalizePixelFormat(GLenum internalFormat, Flags<PixelFormatNormalizeOptionBit> options,
                              GLenum* outInternalFormat, GLenum* outFormat, GLenum* outType) {
#ifdef TRACY_ENABLE
        ZoneScopedC(TRACY_ZONECOLOR_BACKEND);
#endif
        // internal format
        if (outInternalFormat) {
            switch (internalFormat) {
            case GL_DEPTH_COMPONENT32:
                if (options & PixelFormatNormalizeOptionBit::NoDepthComponent32) {
                    // The unsized GL_DEPTH_COMPONENT base format is not a legal
                    // glTexStorage/glRenderbufferStorage internal format on ES, which left
                    // the attachment with no storage at all (KHR-GL3x.framebuffer_blit's
                    // GL_DEPTH_COMPONENT32 config then read an incomplete framebuffer).
                    // GL_DEPTH_COMPONENT24 is the nearest sized ES format that keeps the
                    // same fixed-point encoding, so the GL_UNSIGNED_INT transfer type below
                    // still describes the data; GL_DEPTH_COMPONENT32F would need a float
                    // conversion the upload path does not apply.
                    *outInternalFormat = GL_DEPTH_COMPONENT24;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGBA16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RGBA32F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGB16:
                if (options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget) {
                    // GL_RGB32F is a legal ES texture format but is not colour-renderable, so
                    // glTexStorage2DMultisample rejects it and the attachment ends up with no
                    // storage at all.
                    *outInternalFormat = GL_RGBA32F;
                    break;
                }
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoRgb16)) {
                    *outInternalFormat = GL_RGB32F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RG16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_RG32F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_R16:
                if (options & PixelFormatNormalizeOptionBit::NoNorm16) {
                    *outInternalFormat = GL_R32F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGBA16_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGB16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget) {
                    // A half float loses the low bits of a 16-bit SNORM channel, so keep the
                    // signed-normalized encoding whenever the driver can render to it.
                    *outInternalFormat = (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget)
                                             ? GL_RGBA16F
                                             : GL_RGBA16_SNORM;
                    break;
                }
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoRGB16Snorm) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_RGB16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RG16_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_RG16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_R16_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_R16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGBA8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm)) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGB8_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
                if (options & PixelFormatNormalizeOptionBit::NoSnorm8) {
                    *outInternalFormat = GL_RGB16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RG8_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoSnorm8) {
                    *outInternalFormat = GL_RG16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_R8_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoSnorm8) {
                    *outInternalFormat = GL_R16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            // Legacy desktop-GL sized normalized formats (GL CTS packed_pixels): ES drivers reject them
            // as internal formats, so store them in the closest ES-legal format with at least the same
            // per-channel precision (extra precision stays inside the CTS comparison epsilon, which is
            // derived from the requested format's bit widths). The upload (format, type) below matches
            // the canonical shadow layout in PixelStoreProcessor (UNorm8 / UNorm16 component arrays).
            case GL_R3_G3_B2:
            case GL_RGB4:
            case GL_RGB5:
                *outInternalFormat = GL_RGB565;
                break;
            case GL_RGB10:
            case GL_RGB12:
                *outInternalFormat = (options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                                             (options & PixelFormatNormalizeOptionBit::NoRgb16)
                                         ? GL_RGB32F
                                         : GL_RGB16;
                break;
            case GL_RGBA2:
                *outInternalFormat = GL_RGBA4;
                break;
            case GL_RGBA12:
                *outInternalFormat =
                    (options & PixelFormatNormalizeOptionBit::NoNorm16) ? GL_RGBA32F : GL_RGBA16;
                break;
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
            case GL_SRGB_ALPHA:
                *outFormat = GL_RGBA;
                break;

            // Color sized other
            case GL_RGB9_E5:
            case GL_R11F_G11F_B10F:
            case GL_RGB565:
                *outFormat = GL_RGB;
                break;
            case GL_RGB10_A2:
            case GL_RGB5_A1:
            case GL_RGBA4:
                *outFormat = GL_RGBA;
                break;
            case GL_RGB10_A2UI:
                *outFormat = GL_RGBA_INTEGER;
                break;

            // Legacy desktop-GL sized normalized formats
            case GL_R3_G3_B2:
            case GL_RGB4:
            case GL_RGB5:
            case GL_RGB10:
            case GL_RGB12:
                *outFormat = GL_RGB;
                break;
            case GL_RGBA2:
            case GL_RGBA12:
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
            case GL_RGB16:
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoRgb16)) {
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
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (internalFormat == GL_RGB16_SNORM &&
                     (options & PixelFormatNormalizeOptionBit::NoRGB16Snorm)) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outType = GL_FLOAT;
                    break;
                } else {
                    *outType = GL_SHORT;
                    break;
                }
            case GL_RGB8_SNORM:
            case GL_RG8_SNORM:
            case GL_R8_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoSnorm8) {
                    *outType = GL_FLOAT;
                    break;
                }
                *outType = GL_BYTE;
                break;
            case GL_RGBA8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm)) {
                    *outType = GL_FLOAT;
                    break;
                }
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
            case GL_RGB10_A2UI:
                *outType = GL_UNSIGNED_INT_2_10_10_10_REV;
                break;
            case GL_RGB5_A1:
                // The shadow stores RGB5_A1 as UNorm8x4 (see PixelStoreProcessor); ES accepts
                // GL_RGBA/GL_UNSIGNED_BYTE uploads for this internal format.
                *outType = GL_UNSIGNED_BYTE;
                break;

            // Legacy desktop-GL sized normalized formats: the upload type matches the canonical
            // shadow layout (UNorm8 for <=8-bit channels, UNorm16 for 10/12-bit channels).
            case GL_R3_G3_B2:
            case GL_RGB4:
            case GL_RGB5:
            case GL_RGB565:
            case GL_RGBA2:
            case GL_RGBA4:
                *outType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGB10:
            case GL_RGB12:
                *outType = (options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                                   (options & PixelFormatNormalizeOptionBit::NoRgb16)
                               ? GL_FLOAT
                               : GL_UNSIGNED_SHORT;
                break;
            case GL_RGBA12:
                *outType = (options & PixelFormatNormalizeOptionBit::NoNorm16) ? GL_FLOAT : GL_UNSIGNED_SHORT;
                break;

            // Unsized color formats keep their byte-per-channel client layout.
            case GL_RGBA:
            case GL_RGB:
            case GL_RG:
            case GL_RED:
            case GL_SRGB:
            case GL_SRGB_ALPHA:
                *outType = GL_UNSIGNED_BYTE;
                break;
                // Depth
            case GL_DEPTH_COMPONENT16:
                *outType = GL_UNSIGNED_SHORT;
                break;
            case GL_DEPTH_COMPONENT24:
                *outType = GL_UNSIGNED_INT;
                break;
            case GL_DEPTH_COMPONENT32:
                // Follows the internal-format normalization above: ES only accepts
                // GL_FLOAT data for a GL_DEPTH_COMPONENT32F store.
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
