// MobileGL - MobileGL/MG_Remote/Server/InProcessServer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Entry points of the in-process display server (ARCHITECTURE.md §15.2). Its window lives in the
// app's process and survives neither fork nor exec, so a thread of that process serves, with a
// thread per session instead of a fork: tcp:// runs the --serve supervisor (pre-auth, one session,
// Busy), a unix endpoint up to MOBILEGL_IPC_INPROC_MAX_SESSIONS at once. Latches reset between
// sessions, the backend is pinned for the process, and there is no crash isolation.
// Set before libMobileGL loads: MOBILEGL_IPC_DIAL=no (else 64); none of MOBILEGL_TRANSPORT,
// MOBILEGL_IPC_SERVER_PATH, _RING_MB or _STAGE_MB (else 65); MOBILEGL_IPC_TOKEN for a non-loopback
// listen. The JNI glue installs ServerDisplayInstance(); without one, ServerOwned is refused.

#pragma once

extern "C" {

// Serves `endpoint` on the CALLING thread until mobilegl_server_stop_inprocess is called and the
// live session (if any) has ended, then returns 0. The endpoint is tcp://host:port, or - for a
// same-device client - a unix @abstract name or filesystem path, whose control+data pair and
// SCM_RIGHTS shm segments make it the fastest on-device shape. Other returns, all logged by
// name: 64 / 65 (the environment preconditions above), 71 (empty endpoint), 72 (could not
// listen - EADDRINUSE is retried for up to ~5 s first, the previous server may still be dying),
// 73 (the listener failed), 74 (a server is already serving in this process). Re-callable after
// it returned.
__attribute__((visibility("default"))) int mobilegl_server_serve_inprocess(const char* endpoint);

// Asks the in-process server to stop and returns at once (it does not wait: call it from a UI
// thread). The live session ends within one control slice (~100 ms) - or when the op it is running
// returns (a ServerOwned wait for a window is interrupted too) - and then serve returns 0.
__attribute__((visibility("default"))) void mobilegl_server_stop_inprocess(void);

} // extern "C"
