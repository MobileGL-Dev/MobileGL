// MobileGL - MobileGL/MG_Util/Texture/EtcDecoder.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

// CPU decoder for the ten ETC2/EAC formats core GL requires from 4.3 on (GL 4.6 core
// appendix C.2). The CPU shadow keeps a compressed level in the UNCOMPRESSED storage its
// format resolves to (ConvertGLEnumToTextureInternalFormat), and this is what fills that
// storage from the application's blocks, so the level samples - and reads back - as its
// texels on every backend. The compressed bytes themselves stay beside it for
// glGetCompressedTexImage.
namespace MobileGL::MG_Util::EtcDecoder {
    // True for the ten GL_COMPRESSED_*ETC2* / *EAC* enums.
    Bool IsEtcFormat(GLenum compressedFormat);

    // Bytes one decoded texel takes in the uncompressed storage the format resolves to:
    // RGB8 / SRGB8 -> 3, RGBA8 / SRGB8_ALPHA8 -> 4, R8 / R8_SNORM -> 1, RG8 / RG8_SNORM -> 2.
    // 0 for a format IsEtcFormat rejects.
    SizeT DecodedTexelBytes(GLenum compressedFormat);

    // Decodes ONE 4x4 block (8 or 16 bytes, per the format) into `out`, 16 texels in row-major
    // order (texel (x, y) at index y * 4 + x), DecodedTexelBytes(format) bytes each. The signed
    // EAC formats write two's-complement SNORM bytes. Returns false for a format IsEtcFormat
    // rejects, leaving `out` untouched.
    Bool DecodeBlock(GLenum compressedFormat, const Uint8* block, Uint8* out);

    // Decodes a run of whole blocks covering a (width x height x depth) region and writes the
    // texels that fall inside it into a tightly packed destination image of
    // (destWidth x destHeight x destDepth), at (destX, destY, destZ). `blocks` is slice-major,
    // then row-major in blocks, exactly as glCompressedTex*Image* lays it out. Texels of the
    // last block row/column beyond the region (an image edge that is not a multiple of 4) are
    // dropped, as are texels beyond the destination.
    Bool DecodeRegion(GLenum compressedFormat, const Uint8* blocks, Int width, Int height, Int depth, Uint8* dest,
                      Int destWidth, Int destHeight, Int destDepth, Int destX, Int destY, Int destZ);
} // namespace MobileGL::MG_Util::EtcDecoder
