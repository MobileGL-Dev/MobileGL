// MobileGL - MobileGL/MG_Remote/Transport/AdoptTier.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 A1 (MG_Remote/CONTRACT-P11.md §1): THE PERSISTENT-MAP ADOPTION TIER IS SETTLED AT THE
// HANDSHAKE, BY THE SIDE THAT KNOWS THE DATA PLANE, BEFORE THE FIRST map_persistent.
//
// Tier names follow docs/Disaggregated/design/07: T0 = the client allocates an AHardwareBuffer
// and the server imports it; T1 = the server exports an opaque fd; T2 = the resource owner
// declines and the client keeps the shadow and pushes. Only T2 is implemented (T0 is P11
// package B's, T1 is closed), so MOBILEGL_IPC_ADOPT_TIER=0/1 is either refused by name or
// fatal - never a silent T2, and never discovered at the first map_persistent:
//
//   data plane      side     knob 0 / 1
//   Stream (tcp)    either   one line `Refuse{AdoptTierOnStream, "T<n>"}`, then T2. Never Fatal:
//                            a stream has no memory the two sides share, so no tier but T2
//                            can exist on it.
//   SharedSegments  client   Fatal{UnimplementedAdoptTier, "T<n>"} at the handshake, before any
//                            record is emitted.
//   SharedSegments  server   one warning that the client settles the tier on this plane, then
//                            T2. (inproc and fork share the client's environment, so the client
//                            dies by name right after; a unix-socket server launched on its own
//                            serves the T2 its client settled.)
//
// In MG_Remote/Transport (the link-ratchet's SERVER partition) because both roles call it:
// ServerSession::Accept and ClientSession's two Start halves. The at-use check the server's
// applier and codec ask (Client::AdoptTierIsEmulate) reads the settlement back.

#pragma once

#include <cstdint>

namespace MobileGL::MG_Remote::Transport {

    enum class AdoptTierSide : std::uint8_t { Client, Server };

    // Settles this session's tier from MOBILEGL_IPC_ADOPT_TIER and the negotiated data plane,
    // on `side`, and returns the tier the session runs (2 until package B). Logs at most one
    // line per call; aborts (SessionFail) only on the client over shared segments.
    std::uint32_t SettleAdoptTierAtHandshake(bool streamDataPlane, AdoptTierSide side);

    // True once a handshake in this process has settled the tier. The at-use check trusts a
    // settled session and still refuses by name a map_persistent no handshake settled.
    bool AdoptTierSettledAtHandshake();

    // Test seam: forget the settlement, so one process can drive both answers.
    void ForgetAdoptTierSettlementForTest();

} // namespace MobileGL::MG_Remote::Transport
