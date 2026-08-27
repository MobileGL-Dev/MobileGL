// MobileGL - MobileGL/MG_Util/Math/FixedPointConversion.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>

#include <algorithm>
#include <cmath>

namespace MobileGL::MG_Util {
    // GL 4.6 core 2.3.5 "Fixed-Point Data Conversions", for the 32-bit signed normalized pair that
    // GL_TEXTURE_BORDER_COLOR is specified and queried in when the NON-"I" integer entry points are
    // used (glTexParameteriv / glSamplerParameteriv / glGetTexParameteriv / glGetSamplerParameteriv).
    // The "I" entry points (TexParameterIiv / Iuiv) carry a raw integer border colour instead and
    // must NOT go through these.
    //
    // The two directions have to be an exact pair or a legal round trip is destroyed: the CTS writes
    // {0,1,2,4} with glTexParameteriv and demands {0,1,2,4} back from glGetTexParameteriv. Reading
    // with a bare static_cast<GLint> (which is what the truncating read used to do) answers {0,0,0,0}
    // because equation 2.2 has already scaled those integers down to ~1e-9.
    //
    // b = 32, so the scale is 2^31 - 1 = 2147483647. It is held in DOUBLE deliberately: as a binary32
    // it rounds up to 2^31, and the inverse direction would then answer -2147483648 for f = -1.0
    // where the equation says -2147483647. The forward direction is unaffected either way (a small
    // integer divided by 2147483647 lands on the same float as one divided by 2^31), so one exact
    // constant serves both and the pair stays a true inverse: c -> c/(2^31-1) -> c.
    inline constexpr double kSignedNormalizedInt32Scale = 2147483647.0;

    // Equation 2.2: c / (2^(b-1) - 1), clamped below at -1 so the extra negative code (-2^31) does
    // not produce a value outside [-1, 1].
    inline Float SignedNormalizedInt32ToFloat(Int32 value) {
        return std::max(static_cast<Float>(static_cast<double>(value) / kSignedNormalizedInt32Scale), -1.0f);
    }

    // Equation 2.3: round(f * (2^(b-1) - 1)). f is clamped to [-1, 1] first, as the equation's domain
    // requires; the multiply is done in double so a near-1 float cannot round past INT32_MAX before
    // the cast, which is undefined behaviour rather than a saturating one.
    inline Int32 FloatToSignedNormalizedInt32(Float value) {
        if (std::isnan(value)) return 0;
        const Float clamped = std::clamp(value, -1.0f, 1.0f);
        const double scaled = std::round(static_cast<double>(clamped) * kSignedNormalizedInt32Scale);
        return static_cast<Int32>(std::clamp(scaled, -2147483648.0, 2147483647.0));
    }
} // namespace MobileGL::MG_Util
