// MobileGL - MobileGL/MG_Pipe/PipeFatalFamily.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: THE FATAL VOCABULARY WITHOUT THE WIRE. The record arm runs in a library that has no
// MG_Remote (the FCL shape), and its deaths still need their family words: a staged store that
// cannot answer, a record whose bounds are wrong. So the enum and its names are generated here
// from FatalFamilies.def, and MG_Remote/FatalFamily.h keeps only what needs the protocol - the
// projection of a family onto the seven-value wire FatalCode - and re-exports these names.

#pragma once

#include <cstddef>
#include <cstdint>

#include "FatalFamilies.def"

namespace MobileGL::MG_Pipe {

    // One value per row of FatalFamilies.def, in file order. The names ARE the family words that
    // appear in every `Fatal{...}` log line, which is what lets FatalFamilyName round-trip
    // against the string a site passes.
    enum class MGFatalFamily : ::std::uint32_t {
#define X(Family, WireCode, Why) Family,
        MGL_FATAL_FAMILY_LIST(X)
#undef X
    };

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

} // namespace MobileGL::MG_Pipe
