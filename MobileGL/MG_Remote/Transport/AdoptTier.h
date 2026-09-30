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
// MOBILEGL_IPC_ADOPT_TIER IS THE SWITCH (ruling ID-P11-14). UNSET - the default since 2026-09-29,
// the user's decision - asks for T0 and falls back to T2 QUIETLY (MGLOG_D only, still counted),
// so a tcp session or a host session is not one line longer; 0 asks for T0 BY NAME (the table
// below); 1 is refused by name and runs T2; 2 is T2 byte for byte. T0 that this session cannot use
// is never a Fatal - it is T2 plus one named line (knob 0) or one D line (unset):
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

    // THE HELLO'S T0 ASK WHEN THE KNOB IS UNSET (LinkTerms.adoptTier, protocol.fbs): T0 as asked
    // by 0, except that the server names a refusal only at MGLOG_D. The ask travels because only
    // the server knows why it did not grant T0 (its allow switch, its platform, its POST).
    inline constexpr std::uint32_t kAdoptAskT0Default = 0x80;
    // An ask that is a T0 ask (0 by name, kAdoptAskT0Default by default).
    inline bool AdoptAskIsT0(std::uint32_t ask) { return ask == 0 || ask == kAdoptAskT0Default; }
    // This process's knob asks for T0 (0, or unset).
    bool AdoptTierWantsT0();
    // This process's knob is unset: T0 fallbacks are quiet.
    bool AdoptTierIsDefault();

    // The tier the client's Hello asks for (LinkTerms.adoptTier): 0 when the knob says 0 and the
    // plane it proposes is shared segments, kAdoptAskT0Default when the knob is unset on shared
    // segments, else 2. Pure - the lines are the settle's.
    std::uint32_t AdoptTierAskFor(bool streamDataPlane);

    // Settles this side's handshake answer from MOBILEGL_IPC_ADOPT_TIER and the negotiated data
    // plane and returns the ask this side made (client) or serves by its own knob (server):
    // AdoptTierAskFor's value for a client asking T0 over shared segments, 2 otherwise. Logs at
    // most one line per call (A1's stream refusal - at MGLOG_D when the knob is unset - and the
    // T1 refusal). Never aborts for 0/1/unset; any other value (only a direct assignment can
    // make one) is still Fatal{UnimplementedAdoptTier}.
    std::uint32_t SettleAdoptTierAtHandshake(bool streamDataPlane, AdoptTierSide side);

    // True once a handshake in this process has settled the tier. The at-use check trusts a
    // settled session and still refuses by name a map_persistent no handshake settled.
    bool AdoptTierSettledAtHandshake();

    // Test seam: forget the settlement, so one process can drive both answers.
    void ForgetAdoptTierSettlementForTest();

} // namespace MobileGL::MG_Remote::Transport
