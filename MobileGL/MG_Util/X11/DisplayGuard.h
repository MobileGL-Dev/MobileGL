// MobileGL - MobileGL/MG_Util/X11/DisplayGuard.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once

// MOBILEGL NEVER DIALS AN X DISPLAY ITS OWN PROCESS SERVES.
//
// A rootless X server that draws with MobileGL (Xwayland's glamor) loads this library into the X
// server itself. Anything here that opens a connection to $DISPLAY then connects to the very
// process that is blocked inside the call - which can never answer: the X server deadlocks on its
// own socket. So every place that would open an X connection asks MayDialX11Display first.
//
// "Serves" is decided by facts about the process, not by who called: it holds a LISTENING unix
// socket bound to that display's name (/tmp/.X11-unix/X<n>, as a path or in the abstract
// namespace - an X server started with -listenfd holds the ones its compositor bound), or its
// executable is an X server's. MOBILEGL_X11_DIAL=0 forbids every dial.
//
// The pure pieces take their inputs (the /proc/net/unix text, the process's socket inodes, the
// executable name) so a host test pins them without being an X server.

#include <Includes.h>

namespace MobileGL::MG_Util::X11 {
    // The display number of an X display name - ":0", ":1.0", "unix:2", "host:3.1",
    // "/tmp/.X11-unix/X4" - or -1 when the name has none.
    Int32 ParseDisplayNumber(const char* name);

    // Whether an executable's base name is an X server's (Xwayland, Xorg, Xvfb, ...).
    Bool IsXServerProgram(const String& basename);

    // The inodes of the LISTENING sockets in a /proc/net/unix table bound to display `number`'s
    // socket name, as a path or abstract (leading '@').
    Vector<Uint64> ListeningDisplaySocketInodes(const String& procNetUnix, Int32 number);

    struct ProcessFacts {
        String ExeBasename;
        String ProcNetUnix;
        Vector<Uint64> OwnSocketInodes; // inodes of the sockets in the process's descriptor table
    };

    // Whether the process the facts describe serves display `number`.
    Bool ServesDisplay(const ProcessFacts& facts, Int32 number);

    // The decision: false for no name, a forbidding override ("0"/"false"/"off"), or a display this
    // process serves; a name with no number is dialed only by a process that is not an X server.
    Bool MayDial(const char* displayName, const ProcessFacts& facts, const char* overrideValue);

    // The real process's facts, read from /proc.
    ProcessFacts ReadProcessFacts();

    // MayDial for this process (facts read on each call: rare - a display creation, a window
    // surface - and a process may start serving later). Logs a refusal once.
    Bool MayDialX11Display(const char* displayName);
} // namespace MobileGL::MG_Util::X11
