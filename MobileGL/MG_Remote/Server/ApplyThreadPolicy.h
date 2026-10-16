// MobileGL - MobileGL/MG_Remote/Server/ApplyThreadPolicy.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Where the apply thread runs and how long it spins before parking, chosen per session from the
// peer (the Hello's DialMode) and the data plane. With MOBILEGL_IPC_SERVER_AFFINITY=auto an
// in-process, forked or stream-plane peer gets every big core and the configured spin; a peer
// that dialled in over shared segments gets at most one core (ReservedApplyCore), never the only
// prime core, and kConnectedPeerSpinUs unless MOBILEGL_IPC_SPIN_US is set, because it writes the
// ring the apply thread spins on and its GL thread wants a prime core. An explicit mask or `off`
// wins for every peer. Pure functions, so the unit suite covers every row; the measurements are
// in ARCHITECTURE.md §14.1.

#pragma once

#include <Includes.h>

#include <cstdlib>
#include <cstring>

namespace MobileGL::MG_Remote::Server {

    // Who the session's client is, from its Hello (protocol.fbs DialMode).
    enum class ApplyPeer : Uint8 {
        InProcess = 0, // DialMode::No     - inproc: the client is another thread of this process
        Forked = 1,    // DialMode::Fork   - the client launched this server (same app, same build)
        Connected = 2, // DialMode::Connect - the client dialled a standing server: another process,
                       //                     possibly another app (tcp://, unix:, fd:)
    };

    // The apply thread's spin before it parks, for a dialled-in shared-segment peer when
    // MOBILEGL_IPC_SPIN_US is not set. 50 us - the global default - by measurement: parking at once
    // (0) lost in every cell measured (rd12 Espryt 120 -> 74 fps on the helper route, 27 -> 17 from the
    // shell), so the short spin stays (CONTRACT-P11 B1).
    inline constexpr Uint32 kConnectedPeerSpinUs = 50;

    struct ApplyThreadPolicy {
        Uint64 requestedMask = 0; // 0 = make no affinity call
        Uint32 spinUs = 0;
        Bool recognised = true; // false: the affinity text was not `auto`, `off` or a number
        const char* rule = "";  // what the log line names
    };

    inline const char* ApplyPeerName(ApplyPeer peer) {
        switch (peer) {
        case ApplyPeer::InProcess:
            return "in-process";
        case ApplyPeer::Forked:
            return "forked";
        case ApplyPeer::Connected:
            return "connected";
        }
        return "unknown";
    }

    inline Uint32 PopCount64(Uint64 mask) {
        Uint32 count = 0;
        for (; mask != 0; mask &= mask - 1)
            ++count;
        return count;
    }

    inline Uint64 LowestBit64(Uint64 mask) {
        return mask & (~mask + 1);
    }

    // Why ReservedApplyCore answered what it did - the log line names it.
    enum class ReservedCoreReason : Uint8 {
        LowestOfSeveralPrimeCores = 0, // two or more cores at the peak clock: the lowest of them
        BigCoreBesideTheOnlyPrime = 1, // one core at the peak clock: the lowest OTHER big core
        OnlyPrimeCoreLeftAlone = 2,    // one core at the peak clock and no other big core: none
        NoAsymmetry = 3,               // every cpu is big, or the topology was unreadable: none
    };

    inline const char* ReservedCoreReasonText(ReservedCoreReason reason) {
        switch (reason) {
        case ReservedCoreReason::LowestOfSeveralPrimeCores:
            return "auto: the lowest of the prime cores, the rest left to the dialled-in shared-segment client";
        case ReservedCoreReason::BigCoreBesideTheOnlyPrime:
            return "auto: a big core beside the only prime core, which is left to the dialled-in shared-segment client";
        case ReservedCoreReason::OnlyPrimeCoreLeftAlone:
            return "auto: unpinned - the only prime core is left to the dialled-in shared-segment client and there "
                   "is no other big core";
        case ReservedCoreReason::NoAsymmetry:
            return "auto: unpinned - no asymmetric big/prime topology to split";
        }
        return "";
    }

    // The core `auto` reserves for a dialled-in shared-segment peer's apply thread, which spins there.
    // `bigCoreMask` = cpus within 15% of the peak clock, `primeMask` = cpus AT the peak clock (both
    // DetectCoreTopology's), `onlineMask` = one bit per cpu counted. The rule never hands the spinning
    // apply thread the ONLY prime core - a foreground client's GL thread wants it:
    //   several prime cores                 -> the lowest prime core (the client takes a higher one)
    //   one prime core, other big cores     -> the lowest of those other big cores
    //   one prime core and nothing else big -> none (unpinned)
    //   no asymmetry (every cpu big, or 0)  -> none (unpinned)
    // The supervisor logs the answer at start-up and the broker hands it to the helper (the core the
    // client is kept off), so this function is the one source of that core.
    inline Uint64 ReservedApplyCore(Uint64 bigCoreMask, Uint64 primeMask, Uint64 onlineMask,
                                    ReservedCoreReason* outReason = nullptr) {
        ReservedCoreReason reason = ReservedCoreReason::NoAsymmetry;
        Uint64 core = 0;
        primeMask &= bigCoreMask;
        const Bool asymmetric = bigCoreMask != 0 && (bigCoreMask & onlineMask) != onlineMask && primeMask != 0;
        if (asymmetric) {
            if (PopCount64(primeMask) >= 2) {
                core = LowestBit64(primeMask);
                reason = ReservedCoreReason::LowestOfSeveralPrimeCores;
            } else if ((bigCoreMask & ~primeMask) != 0) {
                core = LowestBit64(bigCoreMask & ~primeMask);
                reason = ReservedCoreReason::BigCoreBesideTheOnlyPrime;
            } else {
                reason = ReservedCoreReason::OnlyPrimeCoreLeftAlone;
            }
        }
        if (outReason != nullptr) *outReason = reason;
        return core;
    }

    // `affinity` is MOBILEGL_IPC_SERVER_AFFINITY's raw text (null or empty = `auto`); `spinExplicit`
    // says MOBILEGL_IPC_SPIN_US was set in this process's environment and `configuredSpinUs` is its
    // value (ConfigLoader's, 50 when unset); `sharedSegments` is the session's data plane (false =
    // stream); `bigCoreMask` / `primeMask` are DetectCoreTopology()'s answer (0 when the topology is
    // unreadable) and `onlineMask` one bit per cpu the process could see.
    inline ApplyThreadPolicy SelectApplyThreadPolicy(const char* affinity, Bool spinExplicit, Uint32 configuredSpinUs,
                                                     ApplyPeer peer, Bool sharedSegments, Uint64 bigCoreMask,
                                                     Uint64 primeMask, Uint64 onlineMask) {
        ApplyThreadPolicy policy;
        policy.spinUs = configuredSpinUs;
        const Bool automatic = affinity == nullptr || affinity[0] == '\0' || std::strcmp(affinity, "auto") == 0;
        if (!automatic) {
            if (std::strcmp(affinity, "off") == 0) {
                policy.rule = "off";
                return policy;
            }
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(affinity, &end, 0);
            if (end == affinity || (end != nullptr && *end != '\0')) {
                policy.recognised = false;
                policy.rule = "unrecognised";
                return policy;
            }
            policy.requestedMask = static_cast<Uint64>(parsed);
            policy.rule = "explicit mask";
            return policy;
        }
        if (peer != ApplyPeer::Connected) {
            policy.requestedMask = bigCoreMask;
            policy.rule = "auto: every big core (in-process / forked peer)";
            return policy;
        }
        if (!sharedSegments) {
            // The stream plane: an io thread feeds the apply thread, which does not spin on the
            // client's writes - B0 measured 7.9 ms/frame of apply CPU there against 26.8 on shm.
            policy.requestedMask = bigCoreMask;
            policy.rule = "auto: every big core (dialled-in peer on the stream plane)";
            return policy;
        }
        policy.spinUs = spinExplicit ? configuredSpinUs : kConnectedPeerSpinUs;
        ReservedCoreReason reason = ReservedCoreReason::NoAsymmetry;
        policy.requestedMask = ReservedApplyCore(bigCoreMask, primeMask, onlineMask, &reason);
        policy.rule = ReservedCoreReasonText(reason);
        return policy;
    }

    // What the supervisor announces at start-up and the broker relays to the helper: the core this
    // process's configuration reserves for a dialled-in shared-segment peer's apply thread. An operator's
    // affinity (a mask or `off`) reserves nothing - the client is then not kept off any core.
    inline Uint64 ReservedApplyCoreForConfig(const char* affinity, Uint64 bigCoreMask, Uint64 primeMask,
                                             Uint64 onlineMask, ReservedCoreReason* outReason = nullptr) {
        const Bool automatic = affinity == nullptr || affinity[0] == '\0' || std::strcmp(affinity, "auto") == 0;
        if (!automatic) {
            if (outReason != nullptr) *outReason = ReservedCoreReason::NoAsymmetry;
            return 0;
        }
        return ReservedApplyCore(bigCoreMask, primeMask, onlineMask, outReason);
    }

} // namespace MobileGL::MG_Remote::Server
