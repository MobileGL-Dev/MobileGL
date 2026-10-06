// MobileGL - MobileGL/MG_Remote/Client/AdoptTierChoice.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "AdoptTierChoice.h"

#include <Config.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Remote/Transport/AdoptTier.h>

namespace MobileGL::MG_Remote::Client {
    Bool AdoptTierIsEmulate() {
        const Uint32 tier = MG_Config::Ipc.AdoptTier;
        if (tier == 2) return true;
        // P11 B2: UNSET (T0 by default) reaching the T2 path - settled or not - is T2, quietly: a
        // session-free caller (a unit case, a harness) runs the P5 default it always ran, and a
        // settled session already counted its fallback at the handshake or its first map_persistent.
        if (tier == MG_Config::kAdoptTierUnset) return true;
        // P11 A1 (CONTRACT-P11 §1): THE VERDICT IS THE HANDSHAKE'S, NOT THIS CALL'S. Both roles
        // settle the tier from the knob and the data plane before the first record
        // (Transport/AdoptTier.cpp): a stream refuses T0/T1 by name and runs T2, a client over
        // shared segments dies by name there, and a server over shared segments serves the T2
        // its client settled. So a settled session reaching this with 0/1 is a T2 session and
        // says nothing more - the one line was the handshake's.
        if (tier <= 1 && Transport::AdoptTierSettledAtHandshake()) return true;
        // A map_persistent NO handshake settled (a session-free caller) is still a NAMED
        // refusal rather than a silent T2: falling back would make `MOBILEGL_IPC_ADOPT_TIER=0`
        // look like a working T0 run and silently produce pmap bytes it must not produce.
        // Anything above 2 is not a tier at all.
        // `dl` (CONTRACT-P6 5.2): the family word every death here carries.
        if (tier <= 1) {
            SessionFail(MGFatalFamily::UnimplementedAdoptTier,
                    "MGPipe: Fatal{UnimplementedAdoptTier, \"T%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u "
                    "reached a map_persistent that no handshake settled; T0 and T1 are not "
                    "implemented and only T2 (emulate) runs.",
                    static_cast<unsigned>(tier), static_cast<unsigned>(tier));
        } else {
            SessionFail(MGFatalFamily::UnimplementedAdoptTier,
                    "MGPipe: Fatal{UnimplementedAdoptTier, \"%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u is "
                    "not an adoption tier; the only values are 0 and 1 (P11) and 2 (emulate, the "
                    "P5 default).",
                    static_cast<unsigned>(tier), static_cast<unsigned>(tier));
        }
    }
} // namespace MobileGL::MG_Remote::Client
