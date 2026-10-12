// MobileGL - MobileGL/MG_Util/Texture/EtcDecoder.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EtcDecoder.h"

#include <algorithm>

namespace MobileGL::MG_Util::EtcDecoder {
    namespace {
        // GL 4.6 core C.2 reads every block as a big-endian integer; bit 63 is the MSB of byte 0.
        Uint64 ReadBigEndian64(const Uint8* bytes) {
            Uint64 value = 0;
            for (Int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
            return value;
        }

        Uint32 Bits(Uint64 word, Uint32 high, Uint32 low) {
            return static_cast<Uint32>((word >> low) & ((Uint64{1} << (high - low + 1)) - 1));
        }

        Uint8 Clamp255(Int value) { return static_cast<Uint8>(std::clamp(value, 0, 255)); }
        Int Extend4(Uint32 v) { return static_cast<Int>((v << 4) | v); }
        Int Extend5(Uint32 v) { return static_cast<Int>((v << 3) | (v >> 2)); }
        Int Extend6(Uint32 v) { return static_cast<Int>((v << 2) | (v >> 4)); }
        Int Extend7(Uint32 v) { return static_cast<Int>((v << 1) | (v >> 6)); }

        // Table C.12: intensity modifiers of the individual/differential modes, indexed by the
        // codeword; a texel's (msb, lsb) picks +a, +b, -a, -b.
        constexpr Int kIntensityModifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},  {13, 42},
                                                   {18, 60}, {24, 80}, {33, 106}, {47, 183}};
        // Table C.14: the T and H mode distances.
        constexpr Int kDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
        // Table C.16: the EAC modifier tables (alpha, R11 and RG11 share them).
        constexpr Int kEacModifiers[16][8] = {
            {-3, -6, -9, -15, 2, 5, 8, 14},  {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
            {-2, -4, -6, -13, 1, 3, 5, 12},  {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
            {-4, -7, -8, -11, 3, 6, 7, 10},  {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
            {-2, -5, -8, -10, 1, 4, 7, 9},   {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
            {-3, -4, -7, -10, 2, 3, 6, 9},   {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
            {-3, -5, -7, -9, 2, 4, 6, 8}};

        // One ETC2 RGB block (with the punch-through alpha variant) into 16 RGBA texels,
        // row-major. Without punch-through alpha every alpha is 255.
        void DecodeEtc2Rgb(const Uint8* block, Bool punchthrough, Uint8 rgba[16][4]) {
            const Uint64 word = ReadBigEndian64(block);
            // The punch-through variant repurposes the diff bit as the opaque bit and has no
            // individual mode (C.2.3).
            const Bool diff = punchthrough || Bits(word, 33, 33) != 0;
            const Bool opaque = !punchthrough || Bits(word, 33, 33) != 0;
            const Bool flip = Bits(word, 32, 32) != 0;
            const Uint32 msbPlane = Bits(word, 31, 16);
            const Uint32 lsbPlane = Bits(word, 15, 0);
            // Texel (x, y) is pixel index x * 4 + y in both index planes.
            const auto texelIndex = [&](Int x, Int y) {
                const Int p = x * 4 + y;
                return static_cast<Int>((((msbPlane >> p) & 1u) << 1) | ((lsbPlane >> p) & 1u));
            };
            const auto write = [&](Int x, Int y, Int r, Int g, Int b, Uint8 a) {
                Uint8* t = rgba[y * 4 + x];
                t[0] = Clamp255(r);
                t[1] = Clamp255(g);
                t[2] = Clamp255(b);
                t[3] = a;
            };
            const auto transparent = [&](Int x, Int y) {
                Uint8* t = rgba[y * 4 + x];
                t[0] = t[1] = t[2] = t[3] = 0;
            };

            Int base[2][3] = {};
            if (!diff) {
                // Individual mode: two 4-bit colours.
                base[0][0] = Extend4(Bits(word, 63, 60));
                base[1][0] = Extend4(Bits(word, 59, 56));
                base[0][1] = Extend4(Bits(word, 55, 52));
                base[1][1] = Extend4(Bits(word, 51, 48));
                base[0][2] = Extend4(Bits(word, 47, 44));
                base[1][2] = Extend4(Bits(word, 43, 40));
            } else {
                // A 5-bit base plus a 3-bit signed delta; a sum that leaves [0, 31] selects the
                // T (red), H (green) or planar (blue) mode instead.
                const auto signed3 = [](Uint32 v) { return v >= 4 ? static_cast<Int>(v) - 8 : static_cast<Int>(v); };
                const Int r = static_cast<Int>(Bits(word, 63, 59));
                const Int g = static_cast<Int>(Bits(word, 55, 51));
                const Int b = static_cast<Int>(Bits(word, 47, 43));
                const Int r2 = r + signed3(Bits(word, 58, 56));
                const Int g2 = g + signed3(Bits(word, 50, 48));
                const Int b2 = b + signed3(Bits(word, 42, 40));
                if (r2 < 0 || r2 > 31) {
                    // T mode.
                    const Int c1[3] = {Extend4((Bits(word, 60, 59) << 2) | Bits(word, 57, 56)),
                                       Extend4(Bits(word, 55, 52)), Extend4(Bits(word, 51, 48))};
                    const Int c2[3] = {Extend4(Bits(word, 47, 44)), Extend4(Bits(word, 43, 40)),
                                       Extend4(Bits(word, 39, 36))};
                    const Int d = kDistances[(Bits(word, 35, 34) << 1) | Bits(word, 32, 32)];
                    Int paint[4][3];
                    for (Int c = 0; c < 3; ++c) {
                        paint[0][c] = c1[c];
                        paint[1][c] = c2[c] + d;
                        paint[2][c] = c2[c];
                        paint[3][c] = c2[c] - d;
                    }
                    for (Int y = 0; y < 4; ++y) {
                        for (Int x = 0; x < 4; ++x) {
                            const Int i = texelIndex(x, y);
                            if (!opaque && i == 2) {
                                transparent(x, y);
                                continue;
                            }
                            write(x, y, paint[i][0], paint[i][1], paint[i][2], 255);
                        }
                    }
                    return;
                }
                if (g2 < 0 || g2 > 31) {
                    // H mode.
                    const Uint32 r1 = Bits(word, 62, 59);
                    const Uint32 g1 = (Bits(word, 58, 56) << 1) | Bits(word, 52, 52);
                    const Uint32 b1 = (Bits(word, 51, 51) << 3) | Bits(word, 49, 47);
                    const Uint32 r2h = Bits(word, 46, 43);
                    const Uint32 g2h = Bits(word, 42, 39);
                    const Uint32 b2h = Bits(word, 38, 35);
                    const Uint32 value1 = (r1 << 8) | (g1 << 4) | b1;
                    const Uint32 value2 = (r2h << 8) | (g2h << 4) | b2h;
                    const Int d = kDistances[(Bits(word, 34, 34) << 2) | (Bits(word, 32, 32) << 1) |
                                             (value1 >= value2 ? 1u : 0u)];
                    const Int c1[3] = {Extend4(r1), Extend4(g1), Extend4(b1)};
                    const Int c2[3] = {Extend4(r2h), Extend4(g2h), Extend4(b2h)};
                    Int paint[4][3];
                    for (Int c = 0; c < 3; ++c) {
                        paint[0][c] = c1[c] + d;
                        paint[1][c] = c1[c] - d;
                        paint[2][c] = c2[c] + d;
                        paint[3][c] = c2[c] - d;
                    }
                    for (Int y = 0; y < 4; ++y) {
                        for (Int x = 0; x < 4; ++x) {
                            const Int i = texelIndex(x, y);
                            if (!opaque && i == 2) {
                                transparent(x, y);
                                continue;
                            }
                            write(x, y, paint[i][0], paint[i][1], paint[i][2], 255);
                        }
                    }
                    return;
                }
                if (b2 < 0 || b2 > 31) {
                    // Planar mode: always opaque, an origin and two gradients.
                    const Int o[3] = {Extend6(Bits(word, 62, 57)),
                                      Extend7((Bits(word, 56, 56) << 6) | Bits(word, 54, 49)),
                                      Extend6((Bits(word, 48, 48) << 5) | (Bits(word, 44, 43) << 3) |
                                              Bits(word, 41, 39))};
                    const Int h[3] = {Extend6((Bits(word, 38, 34) << 1) | Bits(word, 32, 32)),
                                      Extend7(Bits(word, 31, 25)), Extend6(Bits(word, 24, 19))};
                    const Int v[3] = {Extend6(Bits(word, 18, 13)), Extend7(Bits(word, 12, 6)),
                                      Extend6(Bits(word, 5, 0))};
                    for (Int y = 0; y < 4; ++y) {
                        for (Int x = 0; x < 4; ++x) {
                            Int c[3];
                            for (Int k = 0; k < 3; ++k) {
                                c[k] = (x * (h[k] - o[k]) + y * (v[k] - o[k]) + 4 * o[k] + 2) >> 2;
                            }
                            write(x, y, c[0], c[1], c[2], 255);
                        }
                    }
                    return;
                }
                // Differential mode.
                base[0][0] = Extend5(static_cast<Uint32>(r));
                base[1][0] = Extend5(static_cast<Uint32>(r2));
                base[0][1] = Extend5(static_cast<Uint32>(g));
                base[1][1] = Extend5(static_cast<Uint32>(g2));
                base[0][2] = Extend5(static_cast<Uint32>(b));
                base[1][2] = Extend5(static_cast<Uint32>(b2));
            }

            const Uint32 codeword[2] = {Bits(word, 39, 37), Bits(word, 36, 34)};
            for (Int y = 0; y < 4; ++y) {
                for (Int x = 0; x < 4; ++x) {
                    const Int sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
                    const Int i = texelIndex(x, y);
                    if (!opaque && i == 2) {
                        transparent(x, y);
                        continue;
                    }
                    // msb set means negative, lsb picks the larger magnitude. A non-opaque
                    // punch-through block drops the smaller magnitude to 0 (C.2.3).
                    Int modifier = kIntensityModifiers[codeword[sub]][i & 1];
                    if (!opaque && (i & 1) == 0) modifier = 0;
                    if (i & 2) modifier = -modifier;
                    write(x, y, base[sub][0] + modifier, base[sub][1] + modifier, base[sub][2] + modifier, 255);
                }
            }
        }

        // The 3-bit index of texel (x, y) in an EAC block's 48 index bits.
        Int EacIndex(Uint64 word, Int x, Int y) {
            const Int p = x * 4 + y;
            return static_cast<Int>((word >> (45 - 3 * p)) & 7u);
        }

        // An EAC alpha block (the first half of RGBA8_ETC2_EAC) into 16 alpha bytes, row-major.
        void DecodeEacAlpha(const Uint8* block, Uint8 alpha[16]) {
            const Uint64 word = ReadBigEndian64(block);
            const Int base = static_cast<Int>(Bits(word, 63, 56));
            const Int multiplier = static_cast<Int>(Bits(word, 55, 52));
            const Int* table = kEacModifiers[Bits(word, 51, 48)];
            for (Int y = 0; y < 4; ++y) {
                for (Int x = 0; x < 4; ++x) {
                    alpha[y * 4 + x] = Clamp255(base + table[EacIndex(word, x, y)] * multiplier);
                }
            }
        }

        // An R11 EAC block into 16 11-bit values: [0, 2047] unsigned, [-1023, 1023] signed.
        void DecodeEac11(const Uint8* block, Bool isSigned, Int values[16]) {
            const Uint64 word = ReadBigEndian64(block);
            Int base = static_cast<Int>(Bits(word, 63, 56));
            if (isSigned) {
                base = static_cast<Int>(static_cast<int8_t>(static_cast<Uint8>(base)));
                if (base == -128) base = -127;
            }
            const Int multiplier = static_cast<Int>(Bits(word, 55, 52));
            const Int* table = kEacModifiers[Bits(word, 51, 48)];
            for (Int y = 0; y < 4; ++y) {
                for (Int x = 0; x < 4; ++x) {
                    const Int modifier = table[EacIndex(word, x, y)];
                    // A zero multiplier still moves the value by the bare modifier (C.2.5).
                    const Int scaled = multiplier != 0 ? modifier * multiplier * 8 : modifier;
                    values[y * 4 + x] = isSigned ? std::clamp(base * 8 + scaled, -1023, 1023)
                                                 : std::clamp(base * 8 + 4 + scaled, 0, 2047);
                }
            }
        }

        // The 11-bit value in the 8-bit storage the format resolves to, rounded to nearest.
        Uint8 Eac11ToByte(Int value, Bool isSigned) {
            if (isSigned) {
                const Int magnitude = (std::abs(value) * 127 + 511) / 1023;
                return static_cast<Uint8>(static_cast<int8_t>(value < 0 ? -magnitude : magnitude));
            }
            return static_cast<Uint8>((value * 255 + 1023) / 2047);
        }

        SizeT BlockBytes(GLenum format) {
            switch (format) {
            case GL_COMPRESSED_RGBA8_ETC2_EAC:
            case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
            case GL_COMPRESSED_RG11_EAC:
            case GL_COMPRESSED_SIGNED_RG11_EAC:
                return 16;
            default:
                return 8;
            }
        }
    } // namespace

    Bool IsEtcFormat(GLenum compressedFormat) { return DecodedTexelBytes(compressedFormat) != 0; }

    SizeT DecodedTexelBytes(GLenum compressedFormat) {
        switch (compressedFormat) {
        case GL_COMPRESSED_RGB8_ETC2:
        case GL_COMPRESSED_SRGB8_ETC2:
            return 3;
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
        case GL_COMPRESSED_RGBA8_ETC2_EAC:
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
            return 4;
        case GL_COMPRESSED_R11_EAC:
        case GL_COMPRESSED_SIGNED_R11_EAC:
            return 1;
        case GL_COMPRESSED_RG11_EAC:
        case GL_COMPRESSED_SIGNED_RG11_EAC:
            return 2;
        default:
            return 0;
        }
    }

    Bool DecodeBlock(GLenum compressedFormat, const Uint8* block, Uint8* out) {
        Uint8 rgba[16][4];
        switch (compressedFormat) {
        case GL_COMPRESSED_RGB8_ETC2:
        case GL_COMPRESSED_SRGB8_ETC2:
            DecodeEtc2Rgb(block, false, rgba);
            for (Int t = 0; t < 16; ++t) std::copy(rgba[t], rgba[t] + 3, out + t * 3);
            return true;
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
            DecodeEtc2Rgb(block, true, rgba);
            for (Int t = 0; t < 16; ++t) std::copy(rgba[t], rgba[t] + 4, out + t * 4);
            return true;
        case GL_COMPRESSED_RGBA8_ETC2_EAC:
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: {
            Uint8 alpha[16];
            DecodeEacAlpha(block, alpha);
            DecodeEtc2Rgb(block + 8, false, rgba);
            for (Int t = 0; t < 16; ++t) {
                std::copy(rgba[t], rgba[t] + 3, out + t * 4);
                out[t * 4 + 3] = alpha[t];
            }
            return true;
        }
        case GL_COMPRESSED_R11_EAC:
        case GL_COMPRESSED_SIGNED_R11_EAC: {
            const Bool isSigned = compressedFormat == GL_COMPRESSED_SIGNED_R11_EAC;
            Int red[16];
            DecodeEac11(block, isSigned, red);
            for (Int t = 0; t < 16; ++t) out[t] = Eac11ToByte(red[t], isSigned);
            return true;
        }
        case GL_COMPRESSED_RG11_EAC:
        case GL_COMPRESSED_SIGNED_RG11_EAC: {
            const Bool isSigned = compressedFormat == GL_COMPRESSED_SIGNED_RG11_EAC;
            Int red[16];
            Int green[16];
            DecodeEac11(block, isSigned, red);
            DecodeEac11(block + 8, isSigned, green);
            for (Int t = 0; t < 16; ++t) {
                out[t * 2] = Eac11ToByte(red[t], isSigned);
                out[t * 2 + 1] = Eac11ToByte(green[t], isSigned);
            }
            return true;
        }
        default:
            return false;
        }
    }

    Bool DecodeRegion(GLenum compressedFormat, const Uint8* blocks, Int width, Int height, Int depth, Uint8* dest,
                      Int destWidth, Int destHeight, Int destDepth, Int destX, Int destY, Int destZ) {
        const SizeT texelBytes = DecodedTexelBytes(compressedFormat);
        if (texelBytes == 0 || blocks == nullptr || dest == nullptr) return false;
        if (width <= 0 || height <= 0 || depth <= 0) return true;
        const SizeT blockBytes = BlockBytes(compressedFormat);
        const Int blocksX = (width + 3) / 4;
        const Int blocksY = (height + 3) / 4;
        Uint8 texels[16 * 4];
        const Uint8* block = blocks;
        for (Int z = 0; z < depth; ++z) {
            const Int dz = destZ + z;
            for (Int by = 0; by < blocksY; ++by) {
                for (Int bx = 0; bx < blocksX; ++bx, block += blockBytes) {
                    if (dz < 0 || dz >= destDepth) continue;
                    DecodeBlock(compressedFormat, block, texels);
                    for (Int y = 0; y < 4; ++y) {
                        const Int sy = by * 4 + y;
                        const Int dy = destY + sy;
                        if (sy >= height || dy < 0 || dy >= destHeight) continue;
                        for (Int x = 0; x < 4; ++x) {
                            const Int sx = bx * 4 + x;
                            const Int dx = destX + sx;
                            if (sx >= width || dx < 0 || dx >= destWidth) continue;
                            const SizeT offset =
                                ((static_cast<SizeT>(dz) * static_cast<SizeT>(destHeight) + static_cast<SizeT>(dy)) *
                                     static_cast<SizeT>(destWidth) +
                                 static_cast<SizeT>(dx)) *
                                texelBytes;
                            std::copy(texels + (y * 4 + x) * texelBytes, texels + (y * 4 + x + 1) * texelBytes,
                                      dest + offset);
                        }
                    }
                }
            }
        }
        return true;
    }
} // namespace MobileGL::MG_Util::EtcDecoder
