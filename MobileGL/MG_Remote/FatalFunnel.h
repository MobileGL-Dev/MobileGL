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
    // this is zero over a whole good run; a test reads it to prove the funnel was reached.
    ::std::uint64_t SessionFaultCount();

} // namespace MobileGL::MG_Remote
