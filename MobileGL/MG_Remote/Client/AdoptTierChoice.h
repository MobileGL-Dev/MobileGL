// MobileGL - MobileGL/MG_Remote/Client/AdoptTierChoice.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P13 W5: the adoption tier's verdict at map_persistent, split out of PersistentMapTracker when the
// tracker left MG_Remote for the record arm (it runs without a transport; this does not).

#pragma once

#include <Includes.h>

namespace MobileGL::MG_Remote::Client {
    // R-6's tier gate, read at use. True for MOBILEGL_IPC_ADOPT_TIER=2, the only implemented
    // tier. The tiers are design/07's: T0 = the client allocates an AHardwareBuffer and the
    // server imports it (P11 package B), T1 = the server exports an opaque fd (closed), T2 =
    // decline and push. 0 and 1 are SETTLED AT THE HANDSHAKE (P11 A1, Transport/AdoptTier.h):
    // a stream refuses them by name and runs T2, a client over shared segments dies by name
    // there. So a settled session answers true here; only a map_persistent no handshake
    // settled is still a named refusal. It is asked
    // by MGPipeApplyMapPersistent, which is where the decline is decided, so the client's
    // three adoption call sites keep their existing "null means declined" branch and the
    // map-persistent-roundtrips counter keeps counting ATTEMPTS in both arms (E3(c) asserts
    // mpr is equal between the monolith and the split arm, which is only true if the decline
    // happens after the count, on the applier's side of the emission).
    Bool AdoptTierIsEmulate();
} // namespace MobileGL::MG_Remote::Client
