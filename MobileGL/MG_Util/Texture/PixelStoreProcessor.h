// MobileGL - MobileGL/MG_Util/Texture/PixelStoreProcessor.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/RenderState/RenderState.h>
#include "MG_State/GLState/TextureState/TextureEnum.h"
#include "MG_Util/Metrics/TextureMetrics.h"

namespace MobileGL::MG_Util::PixelStoreProcessor {
    void* ProcessTexturePixelsDataUnpack(const void* inputPixels, const PixelStoreParameters& params,
                                         TextureInternalFormat targetInternalFormat,
                                         TextureInputFormat textureInputFormat, TexturePixelDataType inputDataType,
                                         IntVec3 dimension, Bool isBitmap, SizeT& outSize);
    void* ProcessTexturePixelsDataPack(const void* inputPixels, const PixelStoreParameters& params,
                                       TextureInternalFormat srcInternalFormat, TexturePixelDataType srcDataType,
                                       TextureInputFormat dstInputFormat, TexturePixelDataType dstDataType,
                                       IntVec3 dimension, Bool isBitmap, SizeT& outSize);
    Bool ConvertOnePixelToInternal(TextureInternalFormat targetInternalFormat,
                                   TextureInputFormat textureInputFormat,
                                   TexturePixelDataType inputDataType,
                                   const void* inputPixel,
                                   Vector<Uint8>& outputPixel);

    void ProcessColorSwizzle(void* data, SizeT pixelCount, const Vector<TextureSwizzleParam>& swizzle);

    // True when a packed internal format's 32-bit storage word IS the client (format, type) word,
    // so the transfer has to move the words verbatim in both directions.
    //
    // Decoding such a texel to float and re-encoding it is NOT a no-op: RGB9_E5 stores a shared
    // exponent with redundant encodings, and the spec's encode algorithm (GL 4.6 8.5.2) always
    // emits the canonical one - 0xf8fc0000 and 0xe7e00000 are the same value 8064, but only the
    // latter is canonical. glTexImage followed by glGetTexImage must hand back the bits the
    // application uploaded, which is what GL CTS KHR-GL43.copy_image compares
    // ("CopyImageSubData modified contents of source image").
    //
    // Only the pairs whose bit layouts are identical qualify; a genuinely different client format
    // or type still needs the decode/encode conversion.
    Bool IsRawPackedPixelTransfer(TextureInternalFormat internalFormat, TextureInputFormat clientFormat,
                                  TexturePixelDataType clientType);

    // Decodes the canonical shadow-mip storage of `internalFormat` into wide RGBA texels for CPU
    // readback (GetTexImage of non-renderable formats). Non-integer formats fill outWide with
    // 4 Floats per texel; integer formats fill it with 4 Uint32/Int32 per texel and set
    // outIsInteger (outIsSigned tells signed from unsigned). Missing channels read 0 (G/B) and
    // 1 / 1.0f (A). Returns false when the format has no canonical shadow layout.
    Bool DecodeShadowDataToWideRGBA(TextureInternalFormat internalFormat, const void* src, SizeT pixelCount,
                                    Vector<Uint8>& outWide, Bool& outIsInteger, Bool& outIsSigned);
} // namespace MobileGL::MG_Util::PixelStoreProcessor
