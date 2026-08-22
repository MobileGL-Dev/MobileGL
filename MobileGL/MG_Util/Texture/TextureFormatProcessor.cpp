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
        // The two render-target bits reach EVERY signed-normalized format, one-, two- and
        // four-channel included. They used to be granted to GL_RGB16_SNORM alone, which left the
        // other seven with no colour-renderable fallback at all on a driver without
        // EXT_render_snorm: an R8_SNORM or R16_SNORM attachment (what KHR-GL4x.texture_swizzle
        // renders into for every SNORM source format) got no substitute, so the ES framebuffer was
        // incomplete, the draw landed nowhere and the readback fell through to the never-written
        // CPU shadow.
        case GL_RGB16_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoRGB16Snorm;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
            break;
        case GL_RGBA16_SNORM:
        case GL_RG16_SNORM:
        case GL_R16_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoNorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget;
            break;
        case GL_RGBA8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget;
            break;
        case GL_RGB8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget;
            break;
        case GL_RG8_SNORM:
        case GL_R8_SNORM:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8;
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget;
            break;
        // The rest of the three-channel formats no real ES driver renders to. They have no
        // other fallback: none of the driver/forced option bits names them, so before the
        // render-target widening existed for ordinary targets an FBO attachment in one of
        // them could only ever be answered GL_FRAMEBUFFER_UNSUPPORTED (Complementary
        // Reimagined's colortex2 = RGB16F).
        //
        // GL_RGB9_E5 is deliberately absent: its four-channel sibling would have to be a
        // half float, which means unpacking the shared exponent on every transfer, and
        // nothing renders to a shared-exponent format on desktop GL either.
        case GL_RGB16F:
        case GL_RGB32F:
        case GL_SRGB8:
        case GL_RGB8I:
        case GL_RGB8UI:
        case GL_RGB16I:
        case GL_RGB16UI:
        case GL_RGB32I:
        case GL_RGB32UI:
            applicableOptions |= options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget;
            break;
        default:
            break;
        }
        return applicableOptions;
    }

    namespace {
        // The four-channel sibling a three-channel format is widened to when the target has to
        // stay colour-renderable, together with the transfer pair that describes client data for
        // it. Kept in one place because all three of NormalizePixelFormat's switches have to agree:
        // reporting the widened storage but the original three-channel base format emitted
        // inconsistent triples such as (GL_RGBA16F, GL_RGB, GL_BYTE), which is
        // GL_INVALID_OPERATION for glTexImage2D on ES. That only ever went unnoticed because the
        // bit was reachable for multisample storage alone, and glTexStorage*Multisample takes no
        // transfer pair at all.
        struct ThreeChannelWidening {
            GLenum InternalFormat = GL_UNKNOWN_MGL;
            GLenum Format = GL_UNKNOWN_MGL;
            GLenum Type = GL_UNKNOWN_MGL;

            explicit operator Bool() const { return InternalFormat != GL_UNKNOWN_MGL; }
        };

        ThreeChannelWidening GetThreeChannelRenderTargetWidening(GLenum internalFormat,
                                                                 Flags<PixelFormatNormalizeOptionBit> options) {
            if (!(options & PixelFormatNormalizeOptionBit::NoThreeChannelRenderTarget)) {
                return {};
            }
            switch (internalFormat) {
            // Signed-normalized: matches what the always-on NoRGBA8Snorm fallback already does to
            // GL_RGBA8_SNORM, so the two SNORM8 formats land on the same storage.
            case GL_RGB8_SNORM:
                return {GL_RGBA16F, GL_RGBA, GL_FLOAT};
            case GL_RGB16_SNORM:
                // A half float loses the low bits of a 16-bit SNORM channel, so keep the
                // signed-normalized encoding whenever the driver can render to it - and when it
                // cannot, widen to the 32-bit float, which is the only renderable storage that
                // still holds all 65535 channel values exactly. GL_RGBA16F here handed -23451/32767
                // back as -23457, six times the +/-1-step window KHR-GL4x.texture_swizzle allows.
                return (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget)
                           ? ThreeChannelWidening{GL_RGBA32F, GL_RGBA, GL_FLOAT}
                           : ThreeChannelWidening{GL_RGBA16_SNORM, GL_RGBA, GL_SHORT};
            // Unsigned-normalized 16-bit (and the legacy 10/12-bit formats stored as RGB16):
            // GL_RGB32F is a legal ES texture format but is not colour-renderable either.
            case GL_RGB16:
            case GL_RGB10:
            case GL_RGB12:
                // Same reasoning as GL_RGB16_SNORM above, and the same shape: keep the
                // unsigned-normalized encoding whenever the driver has it, because
                // GL_RGBA16 is the SAME-WIDTH four-channel sibling and GL_RGBA32F is not.
                // That matters beyond storage size. ARB_texture_view puts all five 48-bit
                // formats in one view class, so a GL_RGB16 texture viewed as GL_RGB16UI has
                // to alias storage the ES driver also considers compatible; against a
                // GL_RGBA32F carrier the view is a different class and glTextureView is
                // refused outright (KHR-GL4x.texture_view.view_classes). Against GL_RGBA16
                // the whole class lands on ES's 64-bit class and every channel reinterprets
                // bit-exactly. EXT_texture_norm16 - the absence of which is what NoNorm16
                // means - is also what makes GL_RGBA16 colour-renderable, so the two
                // questions have one answer.
                return (options & PixelFormatNormalizeOptionBit::NoNorm16)
                           ? ThreeChannelWidening{GL_RGBA32F, GL_RGBA, GL_FLOAT}
                           : ThreeChannelWidening{GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT};
            // Floating point.
            case GL_RGB16F:
                return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
            case GL_RGB32F:
                return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
            // sRGB: GL_SRGB8_ALPHA8 keeps the sRGB encoding of the colour channels and stores
            // the added alpha linearly, which is exactly the three-channel format's semantics.
            case GL_SRGB8:
                return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
            // Integer.
            case GL_RGB8I:
                return {GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE};
            case GL_RGB8UI:
                return {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE};
            case GL_RGB16I:
                return {GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT};
            case GL_RGB16UI:
                return {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT};
            case GL_RGB32I:
                return {GL_RGBA32I, GL_RGBA_INTEGER, GL_INT};
            case GL_RGB32UI:
                return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT};
            default:
                return {};
            }
        }
    } // namespace

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
            // NoSnorm16RenderTarget outranks the other two 16-bit fallbacks on purpose: it is the
            // only one whose substitute has to be EXACT, so it picks the 32-bit float rather than
            // the half the driver/ANGLE fallbacks settle for. The capability probe folds the
            // driver options and the render-target options into one set while the runtime storage
            // choice can see the render-target bit alone (GetRuntimeFallbackNormalizeOptions), so
            // the two would disagree on the storage format without a fixed precedence.
            case GL_RGBA16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) {
                    *outInternalFormat = GL_RGBA32F;
                    break;
                }
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGB16_SNORM:
                // The three-channel widening below replaces this whenever the target has to stay
                // renderable; GL_RGB32F keeps the precision for the targets that do not.
                if (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) {
                    *outInternalFormat = GL_RGB32F;
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
                if (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) {
                    *outInternalFormat = GL_RG32F;
                    break;
                }
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_RG16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_R16_SNORM:
                if (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget) {
                    *outInternalFormat = GL_R32F;
                    break;
                }
                if ((options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16)) {
                    *outInternalFormat = GL_R16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            // 8-bit SNORM: the half float already IS exact here, so the render-target bit lands on
            // the same storage the other two 8-bit fallbacks pick.
            case GL_RGBA8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
                    *outInternalFormat = GL_RGBA16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RGB8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
                    *outInternalFormat = GL_RGB16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_RG8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
                    *outInternalFormat = GL_RG16F;
                    break;
                }
                *outInternalFormat = internalFormat;
                break;
            case GL_R8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
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
            //
            // The <=8-bit ones land on the 8-bit-per-channel storage that layout ALREADY is, rather
            // than on the narrower GL_RGB565/GL_RGBA4 they nominally fit in. Storing them narrower
            // made the driver requantize the UNorm8 shadow bytes on every upload, and that step is
            // exact only by luck: 5-bit value 2 encodes as UNorm8 16, and 16/255*31 = 1.945 sits
            // astride the 5-bit boundary, so a driver that truncates hands back 1 (all twelve
            // KHR-GL43.copy_image.functional rgb4->rgb4 cases fail on Mali, at verify()'s FIRST
            // check - a plain glTexImage/glGetTexImage round trip with no copy involved). The
            // 8-bit store removes the requantization entirely; the client word round-trips
            // exactly, because encoding an n-bit field to UNorm8 with rounding and back is the
            // identity for every n <= 8. It is also what DirectVulkan has always done with them
            // (VkTextureManager::ResolveTextureFormatInfo resolves all six legacy low-bit formats
            // to R8G8B8A8_UNORM), so the two backends now agree here.
            //
            // Only the DESKTOP-ONLY formats move. GL_RGBA4 and GL_RGB5_A1 are ES formats an
            // application can legitimately ask for - the same normalization picks the storage for
            // glRenderbufferStorage - so widening them would be a memory decision, not a
            // correctness one. Nothing about the REPORTED precision moves either way:
            // GL_TEXTURE_*_SIZE and glGetInternalformativ answer from TextureMetrics, keyed on the
            // requested format, not on the ES storage.
            case GL_R3_G3_B2:
            case GL_RGB4:
            case GL_RGB5:
                *outInternalFormat = GL_RGB8;
                break;
            case GL_RGB10:
            case GL_RGB12:
                *outInternalFormat = (options & PixelFormatNormalizeOptionBit::NoNorm16) ||
                                             (options & PixelFormatNormalizeOptionBit::NoRgb16)
                                         ? GL_RGB32F
                                         : GL_RGB16;
                break;
            case GL_RGBA2:
                *outInternalFormat = GL_RGBA8;
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
                MGLOG_E_ONCE("NormalizePixelFormat: outFormat: unhandled internalFormat: %s",
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
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm16RenderTarget)) {
                    *outType = GL_FLOAT;
                    break;
                } else {
                    *outType = GL_SHORT;
                    break;
                }
            case GL_RGB8_SNORM:
            case GL_RG8_SNORM:
            case GL_R8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
                    *outType = GL_FLOAT;
                    break;
                }
                *outType = GL_BYTE;
                break;
            case GL_RGBA8_SNORM:
                if ((options & PixelFormatNormalizeOptionBit::NoSnorm8) ||
                    (options & PixelFormatNormalizeOptionBit::NoRGBA8Snorm) ||
                    (options & PixelFormatNormalizeOptionBit::NoSnorm8RenderTarget)) {
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
                MGLOG_E_ONCE("NormalizePixelFormat: outType: unhandled internalFormat: %s",
                        MG_Util::ConvertGLEnumToString(internalFormat).c_str());
                // Fallback handling for other formats
                *outType = GL_UNSIGNED_BYTE;
                break;
            }
        }

        // Applied last, over whatever the three switches above chose: widening a three-channel
        // format to keep a colour attachment renderable outranks every other fallback, because
        // the others all pick a three-channel storage the driver still refuses to render to
        // (GL_RGB8_SNORM -> GL_RGB16F under NoSnorm8, GL_RGB16 -> GL_RGB32F under NoNorm16).
        // All three outputs move together: reporting the widened storage while leaving the
        // three-channel base format and its component type in place produced triples like
        // (GL_RGBA16F, GL_RGB, GL_BYTE), which ES rejects for glTexImage2D outright.
        if (const ThreeChannelWidening widening = GetThreeChannelRenderTargetWidening(internalFormat, options)) {
            if (outInternalFormat) *outInternalFormat = widening.InternalFormat;
            if (outFormat) *outFormat = widening.Format;
            if (outType) *outType = widening.Type;
        }
    }
} // namespace MobileGL::MG_Util::TextureFormatProcessor
