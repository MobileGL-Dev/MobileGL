// MobileGL - MobileGL/MG_Remote/Transport/WireLog.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A one-function logging shim for the wire layer's header-only code.
//
// MG_Util/Debug/Log.h includes <Includes.h>, the GL frontend's umbrella
// header - 661 headers, measured with `clang++ -H`. That is fine inside a
// .cpp, and Ring.cpp / Doorbell.cpp / the transports all do it. It is not fine
// in a header of this layer: ITransport.h states the rule ("nothing about a
// byte pipe needs the GL frontend's umbrella header") because these headers
// are included by both roles and by the eventual server-side binary, and
// because the disaggregated build's include-graph purity gate (plan section
// 10.3, gate A) asserts on `-H` output rather than on symbols. Framing.h was
// the one header under Transport/ that broke the rule; it now calls this
// instead, and the umbrella stays inside WireLog.cpp.
//
// ERROR and FATAL only, deliberately. Everything routed here is a latched
// protocol violation, never per-frame noise; non-critical wire lines use
// MGLOG_D from a .cpp, where the INFO build compiles them out entirely.

#pragma once

namespace MobileGL::MG_Remote::Transport {

    // Formats one line and emits it at ERROR level (MGLOG_E). printf-style,
    // with the format checked against the arguments at compile time.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    void
    WireLogError(const char* format, ...);

    // Formats one line, emits it at FATAL level (MGLOG_F), ECHOES IT TO STDERR,
    // and aborts. Every `Fatal{...}` this layer raises goes through here.
    //
    // WHY STDERR AS WELL. Defines.h builds the logger with the console sink OFF
    // and the file sink ON, so an MGLOG line reaches exactly one place: the log
    // file. A Fatal is the last thing the process says, and three readers need
    // it somewhere other than a file whose path they may not know: a terminal,
    // a CI job log, and a gtest death test - which matches its regex against
    // the CHILD'S STDERR and nothing else. The wave-1 review (ID-46 finding 10)
    // found both SessionTestDeath controls written with an empty regex for
    // precisely that reason: with the diagnostic reachable only through the
    // file sink there was nothing on stderr to name, so a bare std::abort() or
    // a segfault satisfied them. The echo is what lets a death control name
    // its diagnostic (R-16: a negative control asserts its own failure reason).
    [[noreturn]]
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    void
    WireLogFatal(const char* format, ...);

} // namespace MobileGL::MG_Remote::Transport
