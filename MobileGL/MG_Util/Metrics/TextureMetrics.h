// MobileGL - MobileGL/MG_Util/Metrics/TextureMetrics.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <MG_State/GLState/TextureState/TextureObject.h>

namespace MobileGL {
    namespace MG_Util {
        SizeT GetSizedInternalFormatSizeInBytes(TextureInternalFormat internal);
        SizeT GetBaseInternalFormatComponentCount(TextureInternalFormat format);
        SizeT GetSizedTexturePixelDataTypeSize(TexturePixelDataType type);
        SizeT GetBaseTexturePixelDataTypeSize(TexturePixelDataType type);
        SizeT GetTexturePixelDataTypeSize(TexturePixelDataType type);
        // This should respect internal format more
        SizeT GetInternalBytesPerPixel(TextureInternalFormat internalformat, TexturePixelDataType type);
        // This should respect type more, representing data passed in
        SizeT GetInputBytesPerPixel(TextureInputFormat inputFormat, TexturePixelDataType type);
        SizeT CalculateInputTextureImageSize(TextureInputFormat inputFormat, TexturePixelDataType pixelDataType,
                                             IntVec3 size);
        ComponentSizes GetComponentSizesForInternalFormat(TextureInternalFormat internal);

        // A specific compressed internal format described the only two ways the CPU shadow needs it:
        // the texel footprint of one block and the bytes that block occupies. Kept as a GLenum query
        // rather than a TextureInternalFormat one on purpose - TextureInternalFormat has no
        // compressed enumerator (a compressed format resolves to the uncompressed storage backing
        // it), so by the time the enum has been converted the block geometry is gone.
        struct CompressedFormatInfo {
            // Zero means "internalformat is not one of the specific compressed formats core GL
            // requires" - which is exactly the glCompressedTexImage* INVALID_ENUM case, so callers
            // get the format test and the block geometry from one lookup.
            Uint blockWidth = 0;
            Uint blockHeight = 0;
            SizeT blockByteSize = 0;
        };
        CompressedFormatInfo GetCompressedFormatInfo(GLenum internalFormat);
        // Blocks are counted rounded up, exactly as GL 4.6 core 8.7 sizes a compressed image, so a
        // 4x4 BPTC image is one 16-byte block and a 1x1 one still is.
        SizeT CalculateCompressedTextureImageSize(const CompressedFormatInfo& info, IntVec3 size);
    } // namespace MG_Util
} // namespace MobileGL
