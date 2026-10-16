// MobileGL - MobileGL/MG_Backend/ClientReadback.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Backend {
    // Pure CPU helpers of the client-format readback conversion (ReadPixels/GetTexImage repack a wide
    // RGBA(_INTEGER) read into the caller's (format, type) layout). Kept context-free so unit tests can
    // exercise the exact packing the GL CTS packed_pixels oracle compares against.
    namespace ClientReadback {
        struct ReadbackChannelMapping {
            Int sourceChannel[4]; // RGBA source channel feeding each destination component
            Int channelCount;     // destination component count
            Bool isInteger;
        };
        Bool GetReadbackChannelMapping(GLenum format, ReadbackChannelMapping& outMapping);

        // Byte size of one destination component of `type`; packed types report the packed word size.
        // 0 = type not supported by the conversion path.
        SizeT GetReadbackComponentSize(GLenum type);

        // Bit-field layout of a GL packed pixel type. width/shift are indexed in the client format's
        // component order (matching ReadbackChannelMapping); shift is the LSB position of the field in
        // the packed word: non-REV types pack the first component from the MSB, *_REV types from the
        // LSB (GL 3.3 table 3.6; field positions mirror the GL CTS glcPackedPixelsTests pack_* oracle).
        struct PackedReadbackLayout {
            Int fieldCount;     // format components stored in the packed word
            Int width[4];       // bit width of each component's field
            Int shift[4];       // LSB bit position of each component's field
            SizeT byteSize;     // packed word size in bytes (1, 2 or 4)
            Bool isFloatPacked; // 10F_11F_11F_REV / 5_9_9_9_REV: fields hold unsigned small floats
        };
        Bool GetPackedReadbackLayout(GLenum type, PackedReadbackLayout& out);

        // Unsigned small-float encoders (EXT_packed_float / EXT_texture_shared_exponent semantics).
        Uint32 EncodeFloatToUnsignedF11(Float value);
        Uint32 EncodeFloatToUnsignedF10(Float value);
        Uint32 EncodeSharedExponentRGB9E5(const Float rgb[3]);

        // Destination bytes per pixel for a (format mapping, type) readback pair; 0 when the pair is
        // not convertible (unknown type, packed field count != format component count, floating-point
        // or packed-float type with an integer format).
        SizeT GetReadbackDstPixelSize(const ReadbackChannelMapping& mapping, GLenum type);

        // Repacks one row of wide RGBA(_INTEGER) texels (4 components of wideType each) into the
        // client's (format, type) layout. src holds width * 4 * GetReadbackComponentSize(wideType)
        // bytes, dst receives width * GetReadbackDstPixelSize(mapping, type) bytes.
        void ConvertWideReadbackRow(const Uint8* src, Uint8* dst, SizeT width, GLenum wideType,
                                    const ReadbackChannelMapping& mapping, GLenum type);

        // Stores wide RGBA(_INTEGER) rows into client memory, honoring the client-side PACK
        // pixel-store parameters (row length, alignment, skips, swap-bytes, and - when
        // applyPackImageParams - image height/skip images). Shared by the DirectGLES and DirectVulkan
        // readback conversion paths.
        Bool StoreWideRowsToClient(const Uint8* wide, GLenum wideType, GLsizei width, GLsizei sliceHeight,
                                   GLsizei sliceCount, const ReadbackChannelMapping& mapping, GLenum type,
                                   void* pixels, Bool applyPackImageParams);

        // Stores packed 32-bit source words verbatim, with the same destination addressing and PACK
        // parameters as StoreWideRowsToClient. For the sources whose storage word already IS the
        // client word (MG_Util::IsRawPackedPixelTransfer): routing those through the wide float
        // intermediate re-encodes them, and the RGB9_E5 encoder canonicalizes
        // the shared exponent, so glGetTexImage would answer with different bits than were stored.
        // `srcWords` holds sliceHeight * sliceCount tightly stacked rows of `width` 32-bit words.
        // False when `type` is not a 4-byte packed type.
        Bool StorePackedWordsToClient(const Uint8* srcWords, GLsizei width, GLsizei sliceHeight, GLsizei sliceCount,
                                      GLenum type, void* pixels, Bool applyPackImageParams);
    } // namespace ClientReadback
} // namespace MobileGL::MG_Backend
