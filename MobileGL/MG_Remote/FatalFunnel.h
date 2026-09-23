// MobileGL - MobileGL/MG_Remote/FatalFunnel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P6 `dl` (CONTRACT-P6 5.2). THE ONE PLACE A SESSION DIES.
//
// Every `Fatal{...}` under MG_Remote/ used to be an MGLOG_F followed by its own std::abort().
// a6 counted ~92 of them. Scattered, they gave the funnel's three benefits to nobody: there was
// no single place to publish a SessionFault to the peer before dying, no single telemetry point,
// and no way for a census to be sure a new death was named. This is that place.
//
// THE MESSAGE STRING IS PASSED VERBATIM, family word and all. The contract requires the log
// lines, the CI greps and every red-once to be byte-identical, so SessionFail does not compose
// the line from the family - it takes the SAME string the site used to hand MGLOG_F and forwards
// it unchanged. The family enum rides ALONGSIDE, redundantly, so the funnel knows the coarse wire
// FatalCode without parsing the string; a test asserts the enum's name appears in the string, so
// the redundancy cannot rot into disagreement.
//
// NOT in WireLog.h: that header is deliberately dependency-light (ITransport.h's rule, the
// include-graph purity gate), and this needs FatalFamily.h, which pulls the protocol header. The
// header-layer sites that must stay pure keep using WireLogFatal, whose own abort routes through
// the same publisher in WireLog.cpp.

#pragma once

#include <MG_Remote/FatalFamily.h>

#include <cstdint>

namespace MobileGL::MG_Remote {

    // Logs the line (byte-identical to the old MGLOG_F), echoes it to stderr so a death test can
    // see it, publishes a SessionFault{family, ...} to the peer if one is connected, bumps the
    // telemetry counter, and aborts. `fmt` is the FULL original message, e.g.
    // "MGPipe: Fatal{RingOverrun, \"SEG_CMD\"} - %s ...".
    [[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void SessionFail(MGFatalFamily family, const char* fmt, ...);

    // How many times a session has ended through the funnel this process. Exit gate S8 asserts
    // this is zero over a whole good run; a test reads it to prove the funnel was reached. A
    // latched fault (SessionLatch below) counts here too: it ends the session just as surely.
    ::std::uint64_t SessionFaultCount();

    // ---- PH-1 (3), ID-P7-1: THE PER-SESSION LATCH ---------------------------------------
    //
    // WHAT IT IS. At a site whose input bytes the PEER wrote, and which already sits in a
    // function that can say "no" (Bool / MobileGLResult), a named fault no longer has to kill the
    // process: SessionLatch logs the SAME line SessionFail would (family word and all, so every
    // `Fatal{` grep, the census and the SessionFault frame read exactly as before), publishes the
    // SessionFault to the peer, records the FIRST fault (family + line), and RETURNS false. The
    // site returns failure, the decoder declines, ServerLoop::DrainRing stops applying at its
    // next check, the apply thread leaves its loop, and ServerMain::RunSession closes the session
    // cleanly and exits kSessionLatchedExitCode - a named, counted end the supervisor reaps and
    // then serves the next connection, exactly as P6.5's fork-per-session already does for a
    // crash (ID-P7-1: "下一连接照常服务" is the supervisor's, not the latch's).
    //
    // WHERE IT IS ARMED, AND WHY ONLY THERE. ArmSessionLatch() is called by exactly one site:
    // ServerMain::RunSession, i.e. the spawn / TCP session child (with or without --serve), whose
    // process IS the session. Unarmed, SessionLatch IS SessionFail - same line, same abort - and
    // that is the answer everywhere else, deliberately:
    //   * INPROC keeps Fatal. CONTRACT-P5's R-2 arms (rules A/B/C, the four honesty arms) are
    //     Fatal{ProtocolCorruption} in the formal wording, and under inproc the client and the
    //     server share one process: a latched-but-alive server would leave the client's GL thread
    //     parked on a barrier in the SAME process with no EOF to wake it, and there is no
    //     supervisor to serve a "next connection" - the process is the session. So the latch
    //     changes nothing inproc and every death test stays a death test.
    //   * THE CLIENT never arms it: a client-side arm of a shared helper (the encoder's honesty
    //     pass) is a local bug, not a peer's bytes.
    // The census (scripts/ci/fatal_census.py rule 2) checks SessionLatch calls the way it checks
    // SessionFail calls: the string must carry its `Fatal{Word` and the word must have a .def row.
    void ArmSessionLatch();
    bool SessionLatchArmed();
    // True once the first fault has latched. One acquire load: DrainRing asks it before every pop.
    bool SessionLatched();
    // The first latched fault - its family and its line, verbatim. Meaningful only once
    // SessionLatched() is true; the line's storage lives for the process.
    MGFatalFamily SessionLatchedFamily();
    const char* SessionLatchedLine();
    // Every fault this armed process latched or declined after the first (>= 1 once latched).
    ::std::uint64_t SessionLatchCount();

    // The session child's exit status when its session ended on a latched fault. Distinct from
    // RunSession's other codes (64-74) so the supervisor's reap line names it; nonzero, so it is
    // counted in `sessionsFaulted` like every other fault.
    inline constexpr int kSessionLatchedExitCode = 75;

    // LATCH-OR-DIE. Armed: log + stderr echo + SessionFault (first fault only) + count, then
    // return false, so a Bool site reads `return SessionLatch(...);`. Unarmed: SessionFail with
    // the identical line (does not return).
    bool
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
        SessionLatch(MGFatalFamily family, const char* fmt, ...);

    // P7 wave 0. Points MG_Pipe's MGPipeSessionFail seam (MG_Pipe/PipeSessionFail.h) at
    // SessionFail, which is what makes the three Magma wire funnels in MG_Backend's renderer
    // publish a SessionFault, bump SessionFaultCount() and appear in the census like every other
    // death - without MG_Backend naming a single MG_Remote symbol.
    //
    // Called from MG_Backend/Init.cpp's InitServerRoleCommon, which BOTH the inproc server role
    // and the spawn/TCP session child run, so there is one install point rather than one per
    // transport. Installing it in the client process would be wrong and is not done: the hook's
    // families are the SERVER's renderer refusing a wire verb, and a client that installed it
    // would publish a SessionFault for a death the server never had.
    void InstallPipeSessionFailHook();

} // namespace MobileGL::MG_Remote
