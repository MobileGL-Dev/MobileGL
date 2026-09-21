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
// found it existed nowhere in the tree. This is the real one.
//
// THE TWO PROCESSES ARE INDEPENDENT. This process is started by whoever starts
// it - a shell, a test, an Android Service - and it LISTENS. The client is
// started separately and CONNECTS. Nothing is inherited: no fds, no memory, no
// parent.
//
// That is a deliberate change from this package's first shape, which forked the
// server and handed it fds 3..6. Inheritance works and is less code, but it can
// only ever produce a server that is a CHILD OF ITS CLIENT - the one arrangement
// the end state cannot use, because there the server is a standing application
// and the client is somewhere else entirely, possibly on another kernel.
// CONTRACT-P6 §3.1 calls this Dial == Connect and had it as a later shape. It is
// the primary one.
//
// WHAT THIS PACKAGE OWNS: the process, the rendezvous, the backend, the session,
// the descriptor hand-off and the exit discipline. NOT what a control frame
// means - that is `cp` - and not the data-plane seam, which is `lk`.
//
// _exit, NOT exit (CONTRACT-P6 §12.4). It decides whether g_processTeardown is
// ever set, and therefore whether seven twin destructors take their early-out or
// call into a driver whose context is already gone. The log is flushed first,
// because _exit runs no atexit handler and a server that died without saying why
// is the diagnostic this project refuses.

#include "../Transport/Doorbell.h" // kWaitForever, SocketDoorbell
#include "../Transport/FdPassing.h"
#include "../Transport/SocketTransport.h"
#include "../Transport/WireLog.h"
#include "ServerLoop.h"
#include "ServerSession.h"

#include <MG_Backend/ServerRole.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace {

    // The server must not be able to become a client. CONTRACT-P6 §3.1: this is
    // catch (a), STRUCTURAL and separate from role selection, because forcing
    // MG_Config::Transport to Monolith - what ARCHITECTURE.md:488 says - would
    // flip 148 MG_Backend lines to the frontend arm in the one process with no
    // frontend. Catch (b) is the launcher's envp scrub, and the two report
    // through DIFFERENT exit codes so they are distinguishable: two safeties
    // that cannot be told apart are one safety.
    bool DialingIsForbidden() {
        const char* dial = std::getenv("MOBILEGL_IPC_DIAL");
        return dial != nullptr && std::strcmp(dial, "no") == 0;
    }

    bool LooksLikeAClientEnvironment() {
        static constexpr const char* kNames[] = {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH",
                                                 "MOBILEGL_IPC_RING_MB", "MOBILEGL_IPC_STAGE_MB"};
        for (const char* name : kNames) {
            if (std::getenv(name) != nullptr) {
                return true;
            }
        }
        return false;
    }

    // The rendezvous. A NAME, because independent processes have no other way to
    // find each other. No default: a default rendezvous is one that two unrelated
    // runs can collide on.
    std::string Endpoint(int argc, char** argv) {
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
            return argv[1];
        }
        if (const char* fromEnv = std::getenv("MOBILEGL_IPC_ENDPOINT")) {
            return fromEnv;
        }
        return std::string();
    }

} // namespace

extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv) {
    using namespace MobileGL::MG_Remote;
    using namespace MobileGL::MG_Remote::Transport;

    const int selfPid = static_cast<int>(::getpid());

    // The arm-proof line (CONTRACT-P6 §9.5). ID-124 proved a lane can run
    // monolith and pass every other gate, so each entry leaves its own evidence.
    WireLogError("MG_Remote server: pid=%d transport=spawn role=server", selfPid);

    if (!DialingIsForbidden()) {
        std::fprintf(stderr,
                     "MG_Remote server: pid=%d refusing to start - MOBILEGL_IPC_DIAL=no was not "
                     "set; anti-recursion catch (a) is missing\n",
                     selfPid);
        std::fflush(nullptr);
        ::_exit(64);
    }
    if (LooksLikeAClientEnvironment()) {
        std::fprintf(stderr,
                     "MG_Remote server: pid=%d the envp scrub FAILED - a client knob survived "
                     "into this process; catch (a) held, catch (b) did not\n",
                     selfPid);
        std::fflush(nullptr);
        ::_exit(65);
    }

    const std::string endpoint = Endpoint(argc, argv);
    if (endpoint.empty()) {
        std::fprintf(stderr,
                     "MG_Remote server: pid=%d no endpoint - pass one as argv[1] or set "
                     "MOBILEGL_IPC_ENDPOINT\n",
                     selfPid);
        std::fflush(nullptr);
        ::_exit(71);
    }

    // ---- 1. listen, BEFORE the backend. The client's bounded connect retry is
    // what tolerates the backend taking a while; a client that connected and then
    // waited for a Welcome a failed bring-up will never send would have to learn
    // about death from a timeout, which §5.4 forbids.
    int listenFd = -1;
    if (SocketTransport::Listen(endpoint, &listenFd) != MOBILEGL_OK) {
        std::fprintf(stderr, "MG_Remote server: pid=%d could not listen on \"%s\"\n", selfPid,
                     endpoint.c_str());
        std::fflush(nullptr);
        ::_exit(72);
    }
    WireLogError("MG_Remote server: pid=%d listening on \"%s\"", selfPid, endpoint.c_str());

    // ---- 2. the backend, through the SAME entry point the inproc server role
    // uses (MG_Backend/ServerRole.h). No EGL yet: the context is created on the
    // apply thread when the client's first eglMakeCurrent crosses.
    if (!MobileGL::MG_Backend::InitServerRoleForSpawn()) {
        std::fprintf(stderr,
                     "MG_Remote server: pid=%d could not create a backend; refusing to accept a "
                     "session it could never apply\n",
                     selfPid);
        ::close(listenFd);
        ::unlink(endpoint.c_str());
        std::fflush(nullptr);
        ::_exit(66);
    }

    // ---- 3. accept the client's TWO connections: control, then aux.
    std::unique_ptr<SocketTransport> control;
    if (SocketTransport::AcceptPair(listenFd, 30000, control) != MOBILEGL_OK) {
        std::fprintf(stderr, "MG_Remote server: pid=%d no client within 30 s\n", selfPid);
        ::close(listenFd);
        ::unlink(endpoint.c_str());
        std::fflush(nullptr);
        ::_exit(73);
    }
    // One session per process (CONTRACT-P6 §12.3), so the door closes behind the
    // one client that came through it. A second would collide on four process
    // globals and on DirectGLES's single per-process EGL tuple.
    ::close(listenFd);
    ::unlink(endpoint.c_str());

    // ---- 4. the two bells, created HERE and handed over. NOT inherited: there
    // is no parent to inherit from. One socketpair per DIRECTION, because
    // SocketDoorbell is "write a byte to wake the peer, poll to be woken" and a
    // single pair would have each side reading the byte it wrote to the other.
    int serverBell[2] = {-1, -1};
    int clientBell[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, serverBell) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, clientBell) != 0) {
        std::fprintf(stderr, "MG_Remote server: pid=%d bell socketpair failed\n", selfPid);
        std::fflush(nullptr);
        ::_exit(74);
    }
    SocketDoorbell selfBell(serverBell[0], /*code=*/1, /*ownsFd=*/true);
    SocketDoorbell peerBell(clientBell[0], /*code=*/1, /*ownsFd=*/true);

    Server::ServerSession& session = Server::ServerSessionInstance();
    session.SetExternalDoorbells(&selfBell, &peerBell);

    // ---- 5. Accept: the ABI assertion, the four segments, and Welcome.
    const MobileGLResult accepted = session.Accept(*control);
    if (accepted != MOBILEGL_OK) {
        std::fprintf(stderr, "MG_Remote server: pid=%d Accept failed (rc=%d)\n", selfPid,
                     static_cast<int>(accepted));
        std::fflush(nullptr);
        ::_exit(67);
    }

    // ---- 6. hand SIX descriptors over. THIS IS WHY SCM_RIGHTS IS IN THIS
    // DESIGN: the segments are anonymous - ASharedMemory_create has no filesystem
    // name at all, and memfd / shm_open+unlink are not openable either
    // (ShmSegmentPosix.cpp says so) - so the descriptor IS the only key, and this
    // process is the side that created them.
    //
    // The slot travels in the sideband rather than being inferred from arrival
    // order, so a reordered or dropped offer is a NAMED mismatch on arrival
    // instead of two segments quietly swapped.
    {
        struct Sideband {
            std::uint32_t slot;
            std::uint64_t bytes;
        };
        using Slot = SessionSegmentSlot;
        static constexpr Slot kSlots[4] = {Slot::Cmd, Slot::Stage, Slot::Reply, Slot::Event};
        for (const Slot slot : kSlots) {
            const std::uint32_t tag = static_cast<std::uint32_t>(slot);
            const int fd = session.Shm().DescriptorFor(slot);
            if (fd < 0) {
                std::fprintf(stderr, "MG_Remote server: pid=%d segment %u has no descriptor\n",
                             selfPid, tag);
                std::fflush(nullptr);
                ::_exit(68);
            }
            Sideband sideband{tag, session.Shm().AnnouncedSize(slot)};
            if (control->ShareFd(fd, MobileGLByteSpan{&sideband, sizeof(sideband)}) !=
                MOBILEGL_OK) {
                std::fprintf(stderr, "MG_Remote server: pid=%d could not share segment %u\n",
                             selfPid, tag);
                std::fflush(nullptr);
                ::_exit(69);
            }
        }
        // Slots 4 and 5 are the bells. The client gets the OTHER end of each pair:
        // it writes to the server's bell and polls its own.
        const int bellFds[2] = {serverBell[1], clientBell[1]};
        for (std::uint32_t index = 0; index < 2; ++index) {
            Sideband sideband{4 + index, 0};
            if (control->ShareFd(bellFds[index], MobileGLByteSpan{&sideband, sizeof(sideband)}) !=
                MOBILEGL_OK) {
                std::fprintf(stderr, "MG_Remote server: pid=%d could not share bell %u\n", selfPid,
                             index);
                std::fflush(nullptr);
                ::_exit(69);
            }
        }
        ::close(serverBell[1]);
        ::close(clientBell[1]);
    }

    // ---- 7. the apply thread. It holds the native context for its whole life,
    // which is what turns MakeCurrent's cache-invalidation storm into a one-off.
    Server::ServerLoop& loop = Server::ServerLoopInstance();
    if (loop.Start(session) != MOBILEGL_OK) {
        std::fprintf(stderr, "MG_Remote server: pid=%d the apply thread did not start\n", selfPid);
        std::fflush(nullptr);
        ::_exit(70);
    }

    WireLogError("MG_Remote server: pid=%d transport=spawn role=server ready "
                 "(backend up, six descriptors handed over, apply thread running)",
                 selfPid);

    // ---- 8. the control pump. EOF is the whole exit condition: no timeout may
    // ever stand in for the death fact (§5.4), because a server one frame behind
    // is the intended steady state and a server that is gone is a different thing.
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        std::uint64_t size = 0;
        MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
        const MobileGLResult result = control->ReceiveFrame(span, &size, kWaitForever);
        if (result == MOBILEGL_ERR_TRANSPORT_CLOSED) {
            WireLogError("MG_Remote server: pid=%d peer closed the control stream; exiting",
                         selfPid);
            break;
        }
        if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL) {
            buffer.resize(static_cast<std::size_t>(size));
            continue; // still queued: ITransport's contract
        }
        if (result != MOBILEGL_OK) {
            WireLogError("MG_Remote server: pid=%d control stream failed (rc=%d); exiting", selfPid,
                         static_cast<int>(result));
            break;
        }
        // `cp` owns what a control frame MEANS. Until then the pump exists so the
        // stream is drained and EOF is noticed; the echo carries this process's
        // own pid so a test that talked to itself cannot pass.
        std::string reply(reinterpret_cast<const char*>(buffer.data()),
                          static_cast<std::size_t>(size));
        reply += "|server-pid=" + std::to_string(selfPid);
        if (control->SendFrame(MobileGLByteSpan{reply.data(), reply.size()}) != MOBILEGL_OK) {
            break;
        }
    }

    loop.Stop();
    session.Close();
    control->Shutdown();
    std::fflush(nullptr);
    ::_exit(0);
}
