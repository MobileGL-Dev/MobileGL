// MobileGL - MobileGL/MG_Util/Math/HalfFloat.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

namespace MobileGL::MG_Util {
    inline Float DecodeHalfBitsToFloat(Uint16 half) {
        const Uint32 sign = static_cast<Uint32>(half & 0x8000u) << 16;
        const Uint32 exponent = (half >> 10) & 0x1Fu;
        const Uint32 mantissa = half & 0x3FFu;
        Uint32 bits;
        if (exponent == 0) {
            if (mantissa == 0) {
                bits = sign; // signed zero
            } else {
                // Subnormal half: renormalize into a float exponent.
                Uint32 e = 127 - 15 + 1;
                Uint32 m = mantissa;
                while ((m & 0x400u) == 0) {
                    m <<= 1;
                    --e;
                }
                bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
            }
        } else if (exponent == 31) {
            bits = sign | 0x7F800000u | (mantissa << 13); // Inf / NaN
        } else {
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        }
        return std::bit_cast<Float>(bits);
    }

    inline Uint16 EncodeFloatToHalfBits(Float value) {
        const Uint32 bits = std::bit_cast<Uint32>(value);
        const auto sign = static_cast<Uint16>((bits >> 16) & 0x8000u);
        const Uint32 exponent = (bits >> 23) & 0xFFu;
        const Uint32 mantissa = bits & 0x7FFFFFu;
        if (exponent == 0xFF) { // Inf / NaN
            return static_cast<Uint16>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u));
        }
        const Int32 halfExponent = static_cast<Int32>(exponent) - 127 + 15;
        if (halfExponent >= 31) {
            return static_cast<Uint16>(sign | 0x7C00u); // overflow -> Inf
        }
        if (halfExponent <= 0) {
            if (halfExponent < -10) {
                return sign; // underflow -> signed zero
            }
            const Uint32 m = mantissa | 0x800000u;
            const Uint32 shift = static_cast<Uint32>(14 - halfExponent);
            Uint32 half = m >> shift;
            if ((m >> (shift - 1)) & 1u) {
                ++half; // round to nearest
            }
            return static_cast<Uint16>(sign | half);
        }
        Uint32 half = (static_cast<Uint32>(halfExponent) << 10) | (mantissa >> 13);
        if (mantissa & 0x1000u) {
            ++half; // round to nearest; a carry into the exponent is the correct result
        }
        return static_cast<Uint16>(sign | half);
    }
} // namespace MobileGL::MG_Util
