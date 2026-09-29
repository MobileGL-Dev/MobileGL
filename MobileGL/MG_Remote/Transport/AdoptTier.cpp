// MobileGL - MobileGL/MG_Remote/Transport/AdoptTier.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "AdoptTier.h"

#include <Config.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Util/Debug/Log.h>

#include <atomic>

namespace MobileGL::MG_Remote::Transport {

    namespace {
        std::atomic<bool> g_settled{false};

        const char* SideName(AdoptTierSide side) { return side == AdoptTierSide::Client ? "client" : "server"; }
    } // namespace

    std::uint32_t SettleAdoptTierAtHandshake(bool streamDataPlane, AdoptTierSide side) {
        const std::uint32_t knob = MG_Config::Ipc.AdoptTier;
        if (knob > 2) {
            // Unreachable through ConfigLoader (it admits 0..2); a direct assignment is the only
            // way here, and "P11 implements it" of a 7 would be a lie.
            SessionFail(MGFatalFamily::UnimplementedAdoptTier,
                        "MGPipe: Fatal{UnimplementedAdoptTier, \"%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u is not an "
                        "adoption tier; the values are 0 and 1 (not implemented) and 2 (emulate, the default)",
                        static_cast<unsigned>(knob), static_cast<unsigned>(knob));
        }
        if (knob == 2) {
            g_settled.store(true, std::memory_order_release);
            return 2;
        }
        if (streamDataPlane) {
            // ONE LINE PER SESSION, on the side that holds the knob. The tcp lane's server is a
            // shared fixture that never sees the client's environment, so each side says it for
            // itself; neither side dies, because T2 is the only tier a stream can carry.
            MGLOG_W("MG_Remote %s: Refuse{AdoptTierOnStream, \"T%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u names a "
                    "shared-mapping adoption tier and this session's data plane is a stream (tcp): the two "
                    "sides share no memory to map, so the session runs T2 (emulate: the client keeps the "
                    "shadow and pushes)",
                    SideName(side), static_cast<unsigned>(knob), static_cast<unsigned>(knob));
            g_settled.store(true, std::memory_order_release);
            return 2;
        }
        if (side == AdoptTierSide::Server) {
            MGLOG_W("MG_Remote server: MOBILEGL_IPC_ADOPT_TIER=%u is the client's to settle on a shared-segment "
                    "link (a client naming T0/T1 dies by name at its own handshake); this server serves T2",
                    static_cast<unsigned>(knob));
            g_settled.store(true, std::memory_order_release);
            return 2;
        }
        SessionFail(MGFatalFamily::UnimplementedAdoptTier,
                    "MGPipe: Fatal{UnimplementedAdoptTier, \"T%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u names a "
                    "shared-mapping adoption tier no package implements yet (T0 is P11 package B's, T1 is "
                    "closed); refused at the handshake over shared segments, before any record is emitted. "
                    "Unset it or set 2 (emulate)",
                    static_cast<unsigned>(knob), static_cast<unsigned>(knob));
    }

    bool AdoptTierSettledAtHandshake() { return g_settled.load(std::memory_order_acquire); }

    void ForgetAdoptTierSettlementForTest() { g_settled.store(false, std::memory_order_release); }

} // namespace MobileGL::MG_Remote::Transport
