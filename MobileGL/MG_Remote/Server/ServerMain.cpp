// MobileGL - MobileGL/MG_Remote/Server/ServerMain.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The server process's entry point. Package `sm` (CONTRACT-P6.md §3).
//
// ARCHITECTURE.md:488-496 has described this symbol for three phases and a6
// found it existed nowhere in the tree: the only MobileGLServer target was the
// P0 spike stub, which links no MobileGL library at all. This is the real one.
//
// WHAT IT IS IN THIS PACKAGE. The process, the inherited descriptors, the
// control transport and the exit discipline - and nothing above them yet. It
// accepts a control connection, answers, and exits on EOF. The session, the
// applier and the four shared segments arrive with the packages that own them;
// wiring them from here before `lk` has a seam to wire them THROUGH is how the
// call sites end up written twice.
//
// WHY IT STILL EARNS ITS PLACE NOW: it is the first time two MobileGL
// processes exist and talk, and every later package needs exactly this scaffold
// to have been proven - that the image links, that execve keeps the fds, that
// the child dies when the parent goes away, and that nothing is left behind.
//
// _exit, NOT exit (CONTRACT-P6 §12.4). It decides whether g_processTeardown is
// ever set, and therefore whether seven twin destructors take their early-out
// or call into a driver whose context is already gone. The log is flushed
// explicitly first, because _exit does not run atexit handlers and a server
// that died without saying why is the diagnostic this project refuses.

#include "../Transport/Doorbell.h" // kWaitForever
#include "../Transport/SocketTransport.h"
#include "../Transport/WireLog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#    include <unistd.h>
#endif

namespace {

    constexpr int kInheritedStreamFd = 3;
    constexpr int kInheritedAuxFd = 4;

    // The child must not be able to become a client. CONTRACT-P6 §3.1: this is
    // catch (a), STRUCTURAL and separate from role selection, because forcing
    // MG_Config::Transport to Monolith - what ARCHITECTURE.md:488 says - would
    // flip 148 MG_Backend lines to the frontend arm in the one process with no
    // frontend. Catch (b) is the parent's envp scrub, and S3 falsifies it by
    // leaving the scrub out; the diagnostic below is what names WHICH fired.
    bool DialingIsForbidden() {
        const char* dial = std::getenv("MOBILEGL_IPC_DIAL");
        return dial != nullptr && std::strcmp(dial, "no") == 0;
    }

    bool LooksLikeAClientEnvironment() {
        // Anything the parent should have scrubbed. If one of these survived,
        // catch (b) failed and we say so by name rather than quietly relying on
        // catch (a) - two safeties that cannot be told apart are one safety.
        static constexpr const char* kNames[] = {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH",
                                                 "MOBILEGL_IPC_RING_MB", "MOBILEGL_IPC_STAGE_MB"};
        for (const char* name : kNames) {
            if (std::getenv(name) != nullptr) {
                return true;
            }
        }
        return false;
    }

} // namespace

extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    using namespace MobileGL::MG_Remote::Transport;

    const int selfPid = static_cast<int>(::getpid());

    // The arm-proof line (CONTRACT-P6 §9.5). ID-124 proved a lane can run
    // monolith and pass every other gate, so each entry has to leave its own
    // evidence: the pid it ran as, and the transport it ran on.
    WireLogError("MG_Remote server: pid=%d transport=spawn role=server", selfPid);

    if (!DialingIsForbidden()) {
        // stderr DIRECTLY, not just WireLogError: Defines.h builds the logger
        // with the console sink OFF (WireLog.h says so), so an error reaches
        // exactly one place - a file whose path the parent may not know. A
        // server that refused to start and could not say why is the diagnostic
        // this project refuses, and ARCHITECTURE.md §15.2 already warns that the
        // app process's stderr is /dev/null on Android, which is why the parent
        // ALSO gets this as a non-zero exit code it can name.
        std::fprintf(stderr, "MG_Remote server: pid=%d refusing to start - MOBILEGL_IPC_DIAL=no "
                             "was not set; anti-recursion catch (a) is missing\n", selfPid);
        WireLogError("MG_Remote server: pid=%d refusing to start - MOBILEGL_IPC_DIAL=no was not "
                     "set. The anti-recursion catch (a) is missing, which means this process was "
                     "not started by SpawnServer and could try to spawn a server of its own.",
                     selfPid);
        std::fflush(nullptr);
        ::_exit(64);
    }
    if (LooksLikeAClientEnvironment()) {
        // Catch (a) already stopped us; this names catch (b)'s failure so the
        // two are distinguishable, which is the whole point of having two.
        std::fprintf(stderr, "MG_Remote server: pid=%d the envp scrub FAILED - a client knob "
                             "survived into the child; catch (a) held, catch (b) did not\n",
                     selfPid);
        WireLogError("MG_Remote server: pid=%d the envp scrub FAILED - a MOBILEGL_TRANSPORT or "
                     "MOBILEGL_IPC_* survived into the child. Catch (a) held; catch (b) did not.",
                     selfPid);
        std::fflush(nullptr);
        ::_exit(65);
    }

    SocketTransport control(kInheritedStreamFd, kInheritedAuxFd, TransportRole::Server);

    // The loop this package owns: read a control frame, answer it, and exit on
    // EOF. `cp` replaces the body with the real surface-op pump; the SHAPE -
    // one reader, EOF ends the process, no timeout decides death - is what has
    // to be right now, because CONTRACT-P6 §5.4 forbids a timeout from ever
    // standing in for the death fact.
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        std::uint64_t size = 0;
        MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
        const MobileGLResult result = control.ReceiveFrame(span, &size, kWaitForever);
        if (result == MOBILEGL_ERR_TRANSPORT_CLOSED) {
            WireLogError("MG_Remote server: pid=%d peer closed the control stream; exiting",
                         selfPid);
            break;
        }
        if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL) {
            buffer.resize(static_cast<std::size_t>(size));
            continue; // the message is still queued: that is ITransport's contract
        }
        if (result != MOBILEGL_OK) {
            WireLogError("MG_Remote server: pid=%d control stream failed (result=%d); exiting",
                         selfPid, static_cast<int>(result));
            break;
        }

        // Package sm's stand-in for the control pump: echo the frame back with
        // our pid appended, so the parent's test can prove the bytes crossed a
        // PROCESS boundary and not just a socket it held both ends of.
        std::string reply(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<std::size_t>(size));
        reply += "|server-pid=" + std::to_string(selfPid);
        const MobileGLResult sent =
            control.SendFrame(MobileGLByteSpan{reply.data(), reply.size()});
        if (sent != MOBILEGL_OK) {
            break;
        }
    }

    control.Shutdown();
    std::fflush(nullptr);
    ::_exit(0);
}
