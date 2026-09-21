// MobileGL - MobileGL/MG_Remote/Server/ServerSpawn.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// fork + execve of the server image, and the process discipline around it.
// Package `sm` (CONTRACT-P6.md §3).
//
// THE TWO FDS THE CHILD INHERITS, at fixed numbers:
//     fd 3  the control stream   (AF_UNIX SOCK_STREAM)
//     fd 4  the aux socket       (AF_UNIX SOCK_DGRAM, SCM_RIGHTS)
// Fixed because the child has no other way to learn them: argv would work but
// puts a file-descriptor number in a string that anything can read out of
// /proc/<pid>/cmdline, and an env var would have to survive the scrub below.
//
// dup2 IS WHAT MAKES THEM INHERITABLE. FdPassing::CreateSocketPair sets
// SOCK_CLOEXEC (FdPassing.cpp:89-90), so the aux socket would vanish at execve.
// dup2 does NOT copy the close-on-exec flag - the new descriptor always has it
// clear - so duplicating onto 3 and 4 both fixes the number and clears the
// flag, in one call each.
//
// ANTI-RECURSION IS TWO INDEPENDENT CATCHES, and CONTRACT-P6 §3.1 is why it is
// not the one ARCHITECTURE.md:488 describes. That rule says the child hard-sets
// Transport to Monolith; a6 measured what that does - 226 live lines test
// `Transport != Monolith` to select the SERVER arm, 148 of them in MG_Backend,
// so forcing Monolith flips the whole backend to frontend glue in the one
// process that has no frontend. So:
//    (a) STRUCTURAL: the child is told it must not dial, which is a separate
//        axis from which role it plays;
//    (b) ENVIRONMENTAL: MOBILEGL_TRANSPORT and every MOBILEGL_IPC_* are removed
//        from the child's envp, so even a child that ignored (a) has nothing to
//        dial with.
// S3 falsifies (b) by leaving the scrub out, and the diagnostic must name WHICH
// of the two fired or the double safety is untestable as two things.

#pragma once

#include "../Transport/SocketTransport.h"

#include <memory>
#include <string>

namespace MobileGL::MG_Remote::Server {

    struct SpawnedServer {
        // -1 when nothing was spawned. Non-negative means this process owes the
        // child a wait(): a SpawnedServer that goes out of scope without being
        // reaped leaves a zombie, which is exactly what the process-tree gate
        // (CONTRACT-P6 §9.4) counts.
        int pid = -1;
        std::unique_ptr<Transport::SocketTransport> transport; // the PARENT's end
    };

    // Locates the server image, forks, and execs it.
    //
    // `imagePath` empty means "resolve it": MOBILEGL_IPC_SERVER_PATH first, then
    // dladdr on our own library to find libMobileGLServer.so beside it. An
    // unresolvable path is a NAMED REFUSAL and never a monolith fallback -
    // ConfigLoader.cpp names that accident and this is the code that must not
    // repeat it.
    MobileGLResult SpawnServer(const std::string& imagePath, SpawnedServer* out);

    // Waits for the child, up to `timeoutMs`. Returns MOBILEGL_ERR_TIMEOUT if it
    // is still running, in which case the caller decides whether to escalate -
    // this function never signals a child it did not have to.
    MobileGLResult ReapServer(SpawnedServer& server, std::uint32_t timeoutMs, int* outExitCode);

    // How many children this process currently has, by scanning for our own
    // pid as a parent. The process-tree gate needs a number, not a promise:
    // "exactly one while running, zero after".
    int CountOwnChildren();

} // namespace MobileGL::MG_Remote::Server
