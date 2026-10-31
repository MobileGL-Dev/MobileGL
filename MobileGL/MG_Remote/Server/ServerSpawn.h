// MobileGL - MobileGL/MG_Remote/Server/ServerSpawn.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Launches a server process. The child gets an endpoint name and nothing else (no inherited
// descriptors, no shared memory) and listens on it like a server started by hand or by an Android
// Service, so a spawned server never depends on being its client's child.
// Anti-recursion is two independent catches (ARCHITECTURE.md §15.1), not forcing Transport to
// Monolith, which would turn off the server arms selected by `Transport != Monolith`:
// (a) MOBILEGL_IPC_DIAL=no, separate from the process's role; (b) MOBILEGL_TRANSPORT and the
// MOBILEGL_IPC_* knobs scrubbed from the child's environment. The child exits 64 for (a) and 65
// for (b), so the two are distinguishable.

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
    MobileGLResult LaunchServer(const std::string& imagePath, const std::string& endpoint, LaunchedServer* out);

    // The same launch with arguments after the endpoint - `--serve` for the fork-per-session
    // supervisor a TCP deployment runs (ServerMain.cpp). PH-1 (4)'s control needs exactly that
    // shape on a unit rig: a session that latches must leave a supervisor that serves the next
    // connection, and a single-session server exits with its only session.
    MobileGLResult LaunchServerWithArgs(const std::string& imagePath, const std::string& endpoint,
                                        const std::vector<std::string>& extraArgs, LaunchedServer* out);

    // Waits for the child, up to `timeoutMs`. Returns MOBILEGL_ERR_TIMEOUT if it
    // is still running, in which case the caller decides whether to escalate -
    // this function never signals a child it did not have to.
    MobileGLResult ReapServer(LaunchedServer& server, std::uint32_t timeoutMs, int* outExitCode);

    // How many children this process currently has, by scanning for our own
    // pid as a parent. The process-tree gate needs a number, not a promise:
    // "exactly one while running, zero after".
    int CountOwnChildren();

} // namespace MobileGL::MG_Remote::Server
