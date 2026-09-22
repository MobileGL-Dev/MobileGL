// MobileGL - MobileGL/MG_Remote/FatalFamily.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P6 `dl` (CONTRACT-P6 5.2). The internal Fatal vocabulary and its total projection onto the
// wire FatalCode, both generated from FatalFamilies.def so a family, its name and its wire code
// cannot drift apart: adding a family is one row, and a row that forgets its code does not
// compile.

#pragma once

#include <MG_Remote/Protocol/generated/protocol_generated.h>

#include "FatalFamilies.def"

namespace MobileGL::MG_Remote {

    // One value per row of FatalFamilies.def, in file order. The names ARE the family words that
    // appear in every `Fatal{...}` log line, which is what lets FatalFamilyName round-trip
    // against the string a site passes.
    enum class MGFatalFamily : ::std::uint32_t {
#define X(Family, WireCode, Why) Family,
        MGL_FATAL_FAMILY_LIST(X)
#undef X
    };

    // THE PROJECTION, AS A TOTAL TABLE. Not a switch with a default arm: this build has no
    // -Werror, so a default would silently swallow a family added without a code. Every family
    // that exists has a row, and a family that does not exist is not a value of the enum.
    inline ::MobileGL::Wire::FatalCode FatalCodeForFamily(MGFatalFamily family) {
        switch (family) {
#define X(Family, WireCode, Why) \
    case MGFatalFamily::Family: return ::MobileGL::Wire::FatalCode::WireCode;
            MGL_FATAL_FAMILY_LIST(X)
#undef X
        }
        // Unreachable for any real enum value; kept so the function is total for the compiler
        // without a `default:` that would mask a missing row.
        return ::MobileGL::Wire::FatalCode::ProtocolCorruption;
    }

    // The family's own word, byte-for-byte what appears inside `Fatal{<name>, ...}`. A death
    // site passes both the enum and its full message string; a test asserts this name is present
    // in that string, which is what keeps the two from diverging.
    inline const char* FatalFamilyName(MGFatalFamily family) {
        switch (family) {
#define X(Family, WireCode, Why) \
    case MGFatalFamily::Family: return #Family;
            MGL_FATAL_FAMILY_LIST(X)
#undef X
        }
        return "<unknown>";
    }

    // The count, for a test that walks every family.
    inline constexpr ::std::size_t MGFatalFamilyCount() {
        ::std::size_t n = 0;
#define X(Family, WireCode, Why) ++n;
        MGL_FATAL_FAMILY_LIST(X)
#undef X
        return n;
    }

} // namespace MobileGL::MG_Remote
