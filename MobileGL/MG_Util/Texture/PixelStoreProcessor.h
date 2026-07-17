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
    void ProcessColorSwizzle(void* data, SizeT pixelCount, const Vector<TextureSwizzleParam>& swizzle);

    // Decodes the canonical shadow-mip storage of `internalFormat` into wide RGBA texels for CPU
    // readback (GetTexImage of non-renderable formats). Non-integer formats fill outWide with
    // 4 Floats per texel; integer formats fill it with 4 Uint32/Int32 per texel and set
    // outIsInteger (outIsSigned tells signed from unsigned). Missing channels read 0 (G/B) and
    // 1 / 1.0f (A). Returns false when the format has no canonical shadow layout.
    Bool DecodeShadowDataToWideRGBA(TextureInternalFormat internalFormat, const void* src, SizeT pixelCount,
                                    Vector<Uint8>& outWide, Bool& outIsInteger, Bool& outIsSigned);
} // namespace MobileGL::MG_Util::PixelStoreProcessor
