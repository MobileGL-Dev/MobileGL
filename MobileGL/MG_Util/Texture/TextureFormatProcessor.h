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
        // Pairs with the bit above: the widened four-channel format has to stay renderable AND
        // keep 16-bit signed-normalized precision, which needs both EXT_texture_norm16 and
        // EXT_render_snorm. Without them the only renderable widening left is a half float, whose
        // 11-bit mantissa cannot represent a 16-bit SNORM channel exactly.
        NoSnorm16RenderTarget = 1 << 8,
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
