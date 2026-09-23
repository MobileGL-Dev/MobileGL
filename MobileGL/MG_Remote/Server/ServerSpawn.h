// MobileGL - MobileGL/MG_Remote/Server/ServerSpawn.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// LAUNCHING a server process, and the process discipline around it.
// Package `sm` (CONTRACT-P6.md §3).
//
// IT LAUNCHES; IT DOES NOT COUPLE. The two processes are independent: this
// hands the child an endpoint NAME and nothing else - no inherited descriptors,
// no shared memory, no handshake. The child listens on that name and the client
// connects to it, exactly as it would to a server someone started by hand or
// that an Android Service started minutes earlier.
//
// An earlier shape of this file forked the server and handed it fds 3..6.
// Inheritance works and is less code, but it can only ever produce a server
// that is a CHILD OF ITS CLIENT - which the end state cannot use, because there
// the server is a standing application and the client is elsewhere, possibly on
// another kernel. So fork/execve survives here only as A WAY TO START A
// PROCESS, which is all it ever needed to be, and every fd it used to carry is
// gone.
//
// ANTI-RECURSION IS STILL TWO INDEPENDENT CATCHES, and CONTRACT-P6 §3.1 is why
// neither is ARCHITECTURE.md:488's. That rule says the child hard-sets
// Transport to Monolith; a6 measured that 226 live lines test
// `Transport != Monolith` to select the SERVER arm, 148 of them in MG_Backend,
// so obeying it flips the whole backend to frontend glue in the one process
// with no frontend. Instead:
//    (a) STRUCTURAL: MOBILEGL_IPC_DIAL=no, a separate axis from which role the
//        process plays;
//    (b) ENVIRONMENTAL: MOBILEGL_TRANSPORT and every MOBILEGL_IPC_* removed
//        from the child's envp.
// The child refuses with exit 64 for (a) and 65 for (b), so the two safeties
// are distinguishable - two that cannot be told apart are one.

#pragma once

#include <Includes.h>

#include "../Protocol/mg_protocol_base.h"

#include <memory>
#include <string>
#include <vector>

namespace MobileGL::MG_Remote::Server {

    struct LaunchedServer {
        // -1 when nothing was spawned. Non-negative means this process owes the
        // child a wait(): a SpawnedServer that goes out of scope without being
        // reaped leaves a zombie, which is exactly what the process-tree gate
        // (CONTRACT-P6 §9.4) counts.
        // -1 when nothing was launched. Non-negative means this process owes the
        // child a wait(): a LaunchedServer that goes out of scope unreaped leaves
        // a zombie, which is what the process-tree gate (§9.4) counts.
        int pid = -1;
        // The endpoint the child was told to listen on. The launcher's only
        // output besides the pid: everything else the client needs, it gets by
        // connecting.
        std::string endpoint;
    };

    // Locates the server image and starts it on `endpoint`. Returns as soon as
    // the process exists; the client's bounded connect retry is what waits for
    // it to finish binding (SocketTransport::ConnectTo).
    //
    // `imagePath` empty means "resolve it": MOBILEGL_IPC_SERVER_PATH first, then
    // dladdr on our own library to find libMobileGLServer.so beside it. An
    // unresolvable path is a NAMED REFUSAL and never a monolith fallback -
    // ConfigLoader.cpp names that accident and this is the code that must not
    // repeat it.
    MobileGLResult LaunchServer(const std::string& imagePath, const std::string& endpoint,
                                LaunchedServer* out);

    // The same launch with arguments after the endpoint - `--serve` for the fork-per-session
    // supervisor a TCP deployment runs (ServerMain.cpp). PH-1 (4)'s control needs exactly that
    // shape on a unit rig: a session that latches must leave a supervisor that serves the next
    // connection, and a single-session server exits with its only session.
    MobileGLResult LaunchServerWithArgs(const std::string& imagePath, const std::string& endpoint,
                                        const std::vector<std::string>& extraArgs,
                                        LaunchedServer* out);

    // Waits for the child, up to `timeoutMs`. Returns MOBILEGL_ERR_TIMEOUT if it
    // is still running, in which case the caller decides whether to escalate -
    // this function never signals a child it did not have to.
    MobileGLResult ReapServer(LaunchedServer& server, std::uint32_t timeoutMs, int* outExitCode);

    // How many children this process currently has, by scanning for our own
    // pid as a parent. The process-tree gate needs a number, not a promise:
    // "exactly one while running, zero after".
    int CountOwnChildren();

} // namespace MobileGL::MG_Remote::Server
