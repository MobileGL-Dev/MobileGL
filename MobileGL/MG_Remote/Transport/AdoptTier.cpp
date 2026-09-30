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

    bool AdoptTierIsDefault() { return MG_Config::Ipc.AdoptTier == MG_Config::kAdoptTierUnset; }

    bool AdoptTierWantsT0() { return MG_Config::Ipc.AdoptTier == 0 || AdoptTierIsDefault(); }

    std::uint32_t AdoptTierAskFor(bool streamDataPlane) {
        if (streamDataPlane || !AdoptTierWantsT0()) return 2u;
        return AdoptTierIsDefault() ? kAdoptAskT0Default : 0u;
    }

    std::uint32_t SettleAdoptTierAtHandshake(bool streamDataPlane, AdoptTierSide side) {
        const std::uint32_t knob = MG_Config::Ipc.AdoptTier;
        if (knob > 2 && knob != MG_Config::kAdoptTierUnset) {
            // Unreachable through ConfigLoader (it admits 0..2 or leaves the knob unset); a direct
            // assignment is the only way here, and "T<n>" of a 7 would be a lie.
            SessionFail(MGFatalFamily::UnimplementedAdoptTier,
                        "MGPipe: Fatal{UnimplementedAdoptTier, \"%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u is not an "
                        "adoption tier; the values are 0 (T0), 1 (closed) and 2 (emulate, the default)",
                        static_cast<unsigned>(knob), static_cast<unsigned>(knob));
        }
        g_settled.store(true, std::memory_order_release);
        if (knob == 2) return 2;
        if (streamDataPlane) {
            // A1, UNCHANGED FOR A NAMED KNOB: ONE LINE PER SESSION, on the side that holds the knob.
            // The tcp lane's server is a shared fixture that never sees the client's environment,
            // so each side says it for itself; neither side dies, because T2 is the only tier a
            // stream carries. AN UNSET KNOB (T0 by default) SAYS IT AT MGLOG_D: every tcp session
            // would otherwise carry the line. The client counts it (ClientSession::T0Fallbacks).
            if (AdoptTierIsDefault()) {
                MGLOG_D("MG_Remote %s: T0 by default is not available on a stream data plane (tcp): the "
                        "session runs T2 (MOBILEGL_IPC_ADOPT_TIER unset; set 0 to have this named)",
                        SideName(side));
                return 2;
            }
            MGLOG_W("MG_Remote %s: Refuse{AdoptTierOnStream, \"T%u\"} - MOBILEGL_IPC_ADOPT_TIER=%u names a "
                    "shared-mapping adoption tier and this session's data plane is a stream (tcp): the two "
                    "sides share no memory to map, so the session runs T2 (emulate: the client keeps the "
                    "shadow and pushes)",
                    SideName(side), static_cast<unsigned>(knob), static_cast<unsigned>(knob));
            return 2;
        }
        if (side == AdoptTierSide::Server) {
            // B2: over shared segments the CLIENT'S ask (its Hello's LinkTerms.adoptTier) decides,
            // and the server grants T0 at its first native bind or names why not
            // (ServerSession::SettleAdoptT0AtBind). Its own knob says nothing about this session.
            return 2;
        }
        if (knob == 1) {
            MGLOG_W("MG_Remote client: Refuse{AdoptTierClosed, \"T1\"} - MOBILEGL_IPC_ADOPT_TIER=1 names T1 "
                    "(the server exports an opaque fd), which P11 closed (ruling ID-P11-1: only Adreno Vulkan "
                    "could run it); the session runs T2 (emulate). Set 0 for T0 or 2 for T2");
            return 2;
        }
        // knob 0 (or unset) over shared segments: asked in the Hello. Nothing is said yet - the
        // answer is the server's, at its first native bind, and a T0 this session cannot use is
        // named once (knob 0) or said at MGLOG_D (unset), at the first map_persistent
        // (ClientSession::AdoptT0).
        return AdoptTierAskFor(false);
    }

    bool AdoptTierSettledAtHandshake() { return g_settled.load(std::memory_order_acquire); }

    void ForgetAdoptTierSettlementForTest() { g_settled.store(false, std::memory_order_release); }

} // namespace MobileGL::MG_Remote::Transport
