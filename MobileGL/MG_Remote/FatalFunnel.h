// MobileGL - MobileGL/MG_Remote/FatalFunnel.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The one place a session dies. SessionFail logs the line, publishes a SessionFault to the peer,
// counts it and aborts, so every Fatal under MG_Remote/ is named and counted. The message is
// forwarded verbatim, family word included, because log greps and the fatal census match it; the
// MGFatalFamily argument gives the wire FatalCode (ARCHITECTURE.md §15.3.1) without parsing the
// string, and a test asserts the family's name appears in it. Header-only Transport code, which
// must not include FatalFamily.h, uses WireLogFatal, whose abort reaches the same publisher.

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

    // ---- per-session latch ---------------------------------------------------------------
    // At a site whose input bytes the peer wrote and that can already return failure, an armed
    // SessionLatch logs SessionFail's line, publishes the SessionFault, records the first fault
    // and returns false; DrainRing stops applying and RunSession ends the session with
    // kSessionLatchedExitCode. Only ServerMain::RunSession arms it, in the session's latch domain.
    // Unarmed it is SessionFail: inproc keeps every Fatal, since a latched server would leave the
    // client's GL thread parked in the same process, and the client never arms it.
    // scripts/ci/fatal_census.py checks SessionLatch strings like SessionFail's.
    void ArmSessionLatch();
    bool SessionLatchArmed();
    // P12 (D5): THE IN-PROCESS DISPLAY SERVER RUNS SESSIONS ONE AFTER ANOTHER IN ONE PROCESS, so
    // the latch that a session child took to its _exit has to be put back for the next session:
    // disarmed, unlatched, the count and the first fault's line cleared. Called between sessions
    // (the in-process supervisor, after it has joined the session thread) and by nothing else; the
    // next RunSession arms it again. SessionFaultCount() is the process's telemetry and is kept.
    // Between sessions the latch is unarmed, so a fault there is a SessionFail - which in the
    // in-process shape takes the display Activity's process down with it: the crash isolation a
    // forked session child gave is what running in-process gives up (documented, D5).
    //
    // P14 S2: it now clears the CALLING THREAD'S DOMAIN (below), which between sessions is the
    // process default - a session that opened its own domain cannot leave a latch behind for the
    // next one to inherit, because the next one does not read that domain at all.
    void ResetSessionLatch();
    // True once the first fault has latched. One acquire load: DrainRing asks it before every pop.
    bool SessionLatched();
    // The first latched fault - its family and its line, verbatim. Meaningful only once
    // SessionLatched() is true; the line's storage lives for the process.
    MGFatalFamily SessionLatchedFamily();
    const char* SessionLatchedLine();
    // Every fault this armed process latched or declined after the first (>= 1 once latched).
    ::std::uint64_t SessionLatchCount();

    // ---- one latch domain per served session ---------------------------------------------
    // The latch is per session so one peer's bad bytes cannot stop a neighbor in the same
    // process: its readers (the apply loop, DrainRing, ServerMain's control loop) run on the
    // session's own threads. SessionRuntime opens a domain per served session and
    // ThreadSessionScope binds both of its threads to it; an unbound thread uses domain 0, the
    // process default (client, inproc, unit tests). FatalLatchDomainOpen returns 0 only when
    // domains run out, and the session then shares the default.
    // See docs/Disaggregated/design/11-state-ownership.md.
    inline constexpr int kMaxLatchDomains = 64;
    int FatalLatchDomainOpen();
    // The domain's state goes; no thread may still be bound to it (the session's apply thread is
    // joined and its control thread's scope has been destroyed by then).
    void FatalLatchDomainClose(int domain);
    // Binds the CALLING THREAD to `domain`, returning the domain it was bound to before so a
    // scope can put it back.
    int FatalLatchDomainBind(int domain);

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

    // THE FIRST LATCHED FAULT OF A SESSION, OBSERVED. Called on the latching thread (its latch
    // domain bound) once the latch is raised and BEFORE the SessionFault frame goes to the peer, so
    // what it records about the session is in place before the client can act on the loss. One
    // process-wide observer, replaced by the next install; it must not block. ServerLoop's marks the
    // session's lease on the server window as ending (ServerDisplay::MarkLeaseEnding).
    using SessionLatchObserver = void (*)();
    void SetSessionLatchObserver(SessionLatchObserver observer);

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
