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
//
// P13 W5: the vocabulary itself (the enum, its names, its count) lives in MG_Pipe/PipeFatalFamily.h
// so the record arm can die by name in a library without MG_Remote; this header adds the wire
// projection and re-exports the vocabulary under its old names.

#pragma once

#include <MG_Pipe/PipeFatalFamily.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>

namespace MobileGL::MG_Remote {

    using ::MobileGL::MG_Pipe::FatalFamilyName;
    using ::MobileGL::MG_Pipe::MGFatalFamily;
    using ::MobileGL::MG_Pipe::MGFatalFamilyCount;

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

} // namespace MobileGL::MG_Remote
