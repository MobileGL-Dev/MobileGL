// MobileGL - MobileGL/MG_Util/Math/SmallFloat.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <bit>
#include <cmath>
#include <limits>

namespace MobileGL::MG_Util {
    // Encodes an unsigned small float with a 5-bit exponent (bias 15) and mantissaBits mantissa
    // bits, per the EXT_packed_float conversion rules: negatives (including -Inf) go to zero,
    // +Inf stays +Inf, NaN stays NaN, and finite values above the largest representable value
    // clamp to it. The mantissa is truncated (rounding mode is implementation-defined).
    inline Uint32 EncodeFloatToUnsignedSmallFloat(Float value, Int mantissaBits) {
        const Uint32 bits = std::bit_cast<Uint32>(value);
        const Bool negative = (bits & 0x80000000u) != 0;
        const Uint32 exponent = (bits >> 23) & 0xFFu;
        const Uint32 mantissa = bits & 0x7FFFFFu;
        const Uint32 exponentMask = 0x1Fu << mantissaBits;
        if (exponent == 0xFFu) {
            if (mantissa != 0) {
                return exponentMask | 1u; // NaN keeps NaN
            }
            return negative ? 0u : exponentMask; // -Inf -> 0, +Inf -> +Inf
        }
        if (negative) {
            return 0u;
        }
        const Int32 smallExponent = static_cast<Int32>(exponent) - 127 + 15;
        if (smallExponent >= 31) { // above the largest finite value -> clamp to it
            return ((31u - 1u) << mantissaBits) | ((1u << mantissaBits) - 1u);
        }
        if (smallExponent <= 0) { // subnormal range: renormalize, flushing tiny values to zero
            const Uint32 fullMantissa = mantissa | 0x800000u;
            const Int32 shift = (23 - mantissaBits) + 1 - smallExponent;
            return shift > 23 ? 0u : fullMantissa >> shift;
        }
        return (static_cast<Uint32>(smallExponent) << mantissaBits) |
               (mantissa >> (23u - static_cast<Uint32>(mantissaBits)));
    }

    inline Uint32 EncodeFloatToUnsignedF11(Float value) { return EncodeFloatToUnsignedSmallFloat(value, 6); }
    inline Uint32 EncodeFloatToUnsignedF10(Float value) { return EncodeFloatToUnsignedSmallFloat(value, 5); }

    // Decodes an unsigned small float (5-bit exponent, bias 15, mantissaBits mantissa bits).
    inline Float DecodeUnsignedSmallFloatToFloat(Uint32 field, Int mantissaBits) {
        const Uint32 exponent = (field >> mantissaBits) & 0x1Fu;
        const Uint32 mantissa = field & ((1u << mantissaBits) - 1u);
        const Float mantissaScale = 1.0f / static_cast<Float>(1u << mantissaBits);
        if (exponent == 0) {
            return std::exp2(-14.0f) * static_cast<Float>(mantissa) * mantissaScale;
        }
        if (exponent == 31) {
            return mantissa == 0 ? std::numeric_limits<Float>::infinity()
                                 : std::numeric_limits<Float>::quiet_NaN();
        }
        return std::exp2(static_cast<Float>(exponent) - 15.0f) *
               (1.0f + static_cast<Float>(mantissa) * mantissaScale);
    }

    inline Float DecodeUnsignedF11ToFloat(Uint32 field) { return DecodeUnsignedSmallFloatToFloat(field, 6); }
    inline Float DecodeUnsignedF10ToFloat(Uint32 field) { return DecodeUnsignedSmallFloatToFloat(field, 5); }

    // RGB9E5 shared-exponent encode, following the EXT_texture_shared_exponent spec algorithm
    // (N = 9 mantissa bits, B = 15 exponent bias, Emax = 31).
    inline Uint32 EncodeSharedExponentRGB9E5(const Float rgb[3]) {
        constexpr Int kMantissaBits = 9;
        constexpr Int kExponentBias = 15;
        constexpr Float kSharedExpMax = 511.0f / 512.0f * 65536.0f; // (2^N-1)/2^N * 2^(Emax-B)

        Float clamped[3];
        for (Int i = 0; i < 3; ++i) {
            const Float v = rgb[i];
            clamped[i] = (std::isnan(v) || v < 0.0f) ? 0.0f : std::min(v, kSharedExpMax);
        }
        const Float maxComponent = std::max(clamped[0], std::max(clamped[1], clamped[2]));

        Int sharedExponent = 0; // all-zero input keeps the all-zero word
        if (maxComponent > 0.0f) {
            sharedExponent = std::max(-kExponentBias - 1, static_cast<Int>(std::floor(std::log2(maxComponent)))) +
                             1 + kExponentBias;
            const Float maxScaled = std::floor(
                maxComponent / std::exp2(static_cast<Float>(sharedExponent - kExponentBias - kMantissaBits)) +
                0.5f);
            if (maxScaled >= 512.0f) { // rounded up to 2^N: bump the shared exponent instead
                ++sharedExponent;
            }
        }

        const Float scale = std::exp2(static_cast<Float>(sharedExponent - kExponentBias - kMantissaBits));
        Uint32 word = static_cast<Uint32>(sharedExponent) << 27;
        for (Int i = 0; i < 3; ++i) {
            const auto field = static_cast<Uint32>(std::floor(clamped[i] / scale + 0.5f));
            word |= std::min(field, 511u) << (i * kMantissaBits);
        }
        return word;
    }

    // RGB9E5 shared-exponent decode.
    inline void DecodeSharedExponentRGB9E5(Uint32 word, Float outRgb[3]) {
        constexpr Int kMantissaBits = 9;
        constexpr Int kExponentBias = 15;
        const Int exponent = static_cast<Int>(word >> 27) - kExponentBias - kMantissaBits;
        const Float scale = std::exp2(static_cast<Float>(exponent));
        for (Int i = 0; i < 3; ++i) {
            outRgb[i] = static_cast<Float>((word >> (i * kMantissaBits)) & 0x1FFu) * scale;
        }
    }
} // namespace MobileGL::MG_Util
