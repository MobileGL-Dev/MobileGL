// MobileGL - MobileGL/MG_Util/Texture/TextureFormatProcessor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL {
    enum class PixelFormatNormalizeOptionBit : Uint {
        NoNorm16 = 1 << 0,
        NoSnorm16 = 1 << 1,
        NoRgb16 = 1 << 2,
        NoSnorm8 = 1 << 3,
        NoDepthComponent32 = 1 << 4,
        NoRGBA8Snorm = 1 << 5,
        NoRGB16Snorm = 1 << 6,
        // The target must be colour-renderable and ES has no renderable three-channel
        // form of the requested format, so it has to be widened to the four-channel one.
        // Set for any colour-attachable target whose native three-channel form the driver
        // refused to render to (multisample storage always, since ES has no three-channel
        // multisample format at all; every other target only after its native probe failed).
        // The widening is visible to every transfer path, so it also retargets the (format,
        // type) pair NormalizePixelFormat reports: the upload has to describe four
        // components in the widened storage's component type, the backend has to expand
        // three-channel client data with an alpha of 1.0, and sampling/readback has to hide
        // the added alpha again (BackendTextureFormatAddsAlpha).
        NoThreeChannelRenderTarget = 1 << 7,
        // A 16-bit signed-normalized image has to back a colour attachment, and the driver cannot
        // render to the signed-normalized encoding itself: that needs both EXT_texture_norm16 and
        // EXT_render_snorm, and without either one an R16_SNORM / RG16_SNORM / RGB16_SNORM /
        // RGBA16_SNORM attachment is texture-only, so the framebuffer is never complete and the
        // draw silently lands nowhere. The substitute is a 32-bit float, NOT the half float the
        // other SNORM fallbacks use: a half's 11-bit mantissa cannot represent a 16-bit SNORM
        // channel exactly - its spacing just below 1.0 is 2^-11, some 16 SNORM steps, so
        // -23451/32767 comes back as -23457 - while a 32-bit float round-trips every one of the
        // 65535 channel values bit for bit.
        NoSnorm16RenderTarget = 1 << 8,
        // The 8-bit twin of the bit above: without EXT_render_snorm an R8_SNORM / RG8_SNORM /
        // RGB8_SNORM / RGBA8_SNORM colour attachment is not renderable either. Here a half float
        // IS exact - every value in [-127, 127] divided by 127 round-trips through a half - so the
        // substitute matches what the always-on GL_RGBA8_SNORM fallback already picks.
        NoSnorm8RenderTarget = 1 << 9,
        None = 0,
    };
    namespace MG_Util::TextureFormatProcessor {
        Flags<PixelFormatNormalizeOptionBit>
        GetApplicablePixelFormatNormalizeOptions(GLenum internalFormat,
                                                 Flags<PixelFormatNormalizeOptionBit> options);
        void NormalizePixelFormat(GLenum internalFormat, Flags<PixelFormatNormalizeOptionBit> options,
                                  GLenum* outInternalFormat, GLenum* outFormat, GLenum* outType);
    } // namespace MG_Util::TextureFormatProcessor
} // namespace MobileGL
