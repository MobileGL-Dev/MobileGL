// MobileGL - MobileGL/MG_Remote/Server/InProcessServer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D5. THE IN-PROCESS DISPLAY SERVER'S ENTRY POINTS.
//
// The on-screen server cannot be the exec'd supervisor that forks a child per session: the window
// it renders into (the display Activity's SurfaceView) is an ANativeWindow in the APP's process, and
// neither exec nor fork carries one. So the Activity's own process serves: a thread it starts calls
// mobilegl_server_serve_inprocess, which is `libMobileGLServer.so tcp://... --serve` with THREADS
// instead of fork - the same pre-auth, the same Busy for an authenticated second Hello while a
// session is live, one session at a time, each session a thread of this process, the latch reset
// between sessions and the backend pinned for the process lifetime. Crash isolation is what it
// gives up: a session's abort (an unarmed SessionFail) takes this process - the Activity - with it.
//
// PRECONDITIONS, the same as mobilegl_server_main's, set in the environment BEFORE libMobileGL is
// loaded (Os.setenv from Java): MOBILEGL_IPC_DIAL=no (else 64), none of MOBILEGL_TRANSPORT /
// MOBILEGL_IPC_SERVER_PATH / MOBILEGL_IPC_RING_MB / MOBILEGL_IPC_STAGE_MB (else 65),
// MOBILEGL_IPC_ROLE=server (forced if missing, as the supervisor does), MOBILEGL_IPC_TOKEN (required
// for a non-loopback listen; >= the minimum length), optionally MOBILEGL_BACKEND_TYPE (the pin),
// MOBILEGL_IPC_LOG_FORWARD, MOBILEGL_LOG_FILE_PATH.
//
// THE DISPLAY is separate: the JNI glue installs ServerDisplayInstance() (ServerDisplay.h) and
// attaches/detaches the window from the SurfaceHolder callbacks. A server with no display installed
// serves offscreen sessions only and refuses a ServerOwned surface by name.

#pragma once

extern "C" {

// Serves `endpoint` (tcp://host:port only) on the CALLING thread until mobilegl_server_stop_inprocess
// is called and the live session (if any) has ended, then returns 0. Other returns, all logged by
// name: 64 / 65 (the environment preconditions above), 71 (not a tcp:// endpoint), 72 (could not
// listen - EADDRINUSE is retried for up to ~5 s first, the previous server may still be dying), 73
// (the listener failed), 74 (a server is already serving in this process). Re-callable after it
// returned.
__attribute__((visibility("default"))) int mobilegl_server_serve_inprocess(const char* endpoint);

// Asks the in-process server to stop and returns at once (it does not wait: call it from a UI
// thread). The live session ends within one control slice (~100 ms) - or when the op it is running
// returns (a ServerOwned wait for a window is interrupted too) - and then serve returns 0.
__attribute__((visibility("default"))) void mobilegl_server_stop_inprocess(void);

} // extern "C"
