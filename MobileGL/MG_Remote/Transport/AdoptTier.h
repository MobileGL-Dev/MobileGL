// MobileGL - MobileGL/MG_Remote/Transport/AdoptTier.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 A1 + B2 (MG_Remote/CONTRACT-P11.md §1 and B2): THE PERSISTENT-MAP ADOPTION TIER IS ASKED
// AT THE HANDSHAKE, BY THE SIDE THAT KNOWS THE DATA PLANE, BEFORE THE FIRST map_persistent.
//
// Tier names follow docs/Disaggregated/design/07: T0 = the client allocates an AHardwareBuffer
// and the server imports it; T1 = the server exports an opaque fd (CLOSED, ID-P11-1); T2 = the
// resource owner declines and the client keeps the shadow and pushes.
//
// MOBILEGL_IPC_ADOPT_TIER IS THE SWITCH (ruling ID-P11-14): 2, the default, is today's T2 byte
// for byte; 0 asks for T0; 1 is refused by name and runs T2. T0 that this session cannot use is
// never a Fatal - it is T2 plus one named line:
//
//   data plane      side     knob 0                                  knob 1
//   Stream (tcp)    either   `Refuse{AdoptTierOnStream, "T0"}`, T2   `Refuse{AdoptTierOnStream, "T1"}`, T2
//                            (A1, unchanged: a stream shares no memory, so no tier but T2 exists)
//   SharedSegments  client   asks T0 in its Hello (LinkTerms.        `Refuse{AdoptTierClosed, "T1"}`, asks T2
//                            adoptTier = 0); whether it runs T0 is
//                            read at the first map_persistent from
//                            kCapAdoptT0 (ClientSession::AdoptT0)
//   SharedSegments  server   its own knob is not consulted: the client's ask decides, and the
//                            server grants it at its first native bind (ServerSession::
//                            SettleAdoptT0AtBind) or names why not
//
// In MG_Remote/Transport (the link-ratchet's SERVER partition) because both roles call it:
// ServerSession::Accept and ClientSession's two Start halves. The at-use check the server's
// applier and codec ask on the T2 path (Client::AdoptTierIsEmulate) reads the settlement back.

#pragma once

#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    enum class AdoptTierSide : std::uint8_t { Client, Server };

    // The tier the client's Hello asks for (LinkTerms.adoptTier): 0 when the knob says 0 and the
    // plane it proposes is shared segments, else 2. Pure - the lines are the settle's.
    std::uint32_t AdoptTierAskFor(bool streamDataPlane);

    // Settles this side's handshake answer from MOBILEGL_IPC_ADOPT_TIER and the negotiated data
    // plane and returns the tier this side asks (client) or serves by its own knob (server): 0
    // only for a client asking T0 over shared segments, 2 otherwise. Logs at most one line per
    // call (A1's stream refusal, the T1 refusal). Never aborts for 0/1; a knob above 2 (only a
    // direct assignment can make one) is still Fatal{UnimplementedAdoptTier}.
    std::uint32_t SettleAdoptTierAtHandshake(bool streamDataPlane, AdoptTierSide side);

    // True once a handshake in this process has settled the tier. The at-use check trusts a
    // settled session and still refuses by name a map_persistent no handshake settled.
    bool AdoptTierSettledAtHandshake();

    // Test seam: forget the settlement, so one process can drive both answers.
    void ForgetAdoptTierSettlementForTest();

} // namespace MobileGL::MG_Remote::Transport
