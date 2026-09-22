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
#include "../Protocol/SurfaceOpCodec.h"
#include "ServerLoop.h"
#include "ServerSession.h"
#include "SurfaceControlFrame.h"

#include <Config.h>  // MG_Config::Transport - D1 pins it to Spawn in this process
#include <MG_Backend/MGPipe/PipeInputs.h>  // D1c: MGPipeSetServerProcessRole
#include <MG_Backend/ServerRole.h>
#include <MG_Util/Metrics/PipeStats.h> // step 1.6: the counters' latch, server-process half
#include <Init.h>   // MG_ConfigLoader::Init - the child loads its own config (step 1.5)

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

    // ---- 1.5. CONFIG, AND THEN D1.
    //
    // TWO SEPARATE DEFECTS LIVED HERE, and both were silent.
    //
    // (a) NOTHING LOADED THE CONFIG AT ALL. mobilegl_server_main calls
    //     InitServerRoleForSpawn directly rather than MobileGL::Initialize, and
    //     MG_ConfigLoader::Init is a step of the latter - so every MG_Config
    //     value in this process was its STATIC DEFAULT. MOBILEGL_BACKEND_TYPE
    //     survives the launcher's scrub on purpose, and it was being read by
    //     nobody: a DirectVulkan session would have brought up a DirectGLES
    //     server and then disagreed with its client about which backend the
    //     capability mask describes.
    //
    // (b) THE CHILD RESOLVED Transport == Monolith. CONTRACT-P6 §3.1 (D1) says
    //     it must read as Spawn, and a6 measured the cost of getting it wrong:
    //     226 live lines test `Transport != Monolith` to select the SERVER arm,
    //     148 of them in MG_Backend. Monolith flips all of them to frontend glue
    //     in the one process that has no frontend. Observed, exactly as
    //     predicted: TextureImpl::UnitTexturesByHandle() answered false, Clear
    //     took the legacy arm that dereferences borrowed FRONTEND slot pointers,
    //     and the apply thread took a SIGSEGV inside SyncNeccessaryTextures on
    //     the first Clear record of the OpenRA retrace.
    //
    // IT IS SET HERE AND NOT THROUGH THE ENVIRONMENT because the environment is
    // where it must NOT appear: LooksLikeAClientEnvironment above exits 65 if
    // MOBILEGL_TRANSPORT survived into this process, and that check is
    // anti-recursion catch (b). D1's own answer is that Dial - not Transport -
    // is the anti-recursion axis, so Transport is free to say what is true.
    MobileGL::MG_ConfigLoader::Init();
    MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::Spawn;
    // D1c/D10: THIS PROCESS IS THE SERVER, stated once and before any GL work.
    //
    // Four guards used to ask "am I on the apply thread" or "is a ClientSession active" - both
    // questions a server PROCESS answers wrongly: it has no ClientSession at all, and every one
    // of its threads is a server thread, not just the applier's. Saying it here is what arms
    // them; without it they are compiled in and permanently false, which is worse than absent
    // because it reads as coverage.
    MobileGL::MG_Pipe::MGPipeSetServerProcessRole(true);
    // ---- 1.6. THE MGPIPE COUNTERS, and this is the ONLY call site outside
    // MobileGL::Initialize().
    //
    // GATE 8'S SERVER HALF WAS STRUCTURALLY ZERO WITHOUT THIS, and the shape of the
    // defect is worth stating: the child never runs MobileGL::Initialize, so
    // PipeStats::Init() - a step of it - never ran here, and g_pipeStatsEnabled
    // stayed false. Every one of the ~80 `if (Enabled())` sites in the backends and
    // in MG_Remote (ServerLoop's own ServerWaits/ServerParks publish included)
    // compiled in and was permanently false in this process. The contract's
    // mandatory "socket doorbell vs inproc condvar" number therefore had no server
    // half under `spawn` at all, and its absence read exactly like a real zero:
    // `wait[srv=0 srvpark=0]` is what "the server never waited" looks like too.
    //
    // IT GOES AFTER THE CONFIG LOAD AND AFTER THE ROLE IS STATED, and both halves
    // of that order are load-bearing:
    //
    //   * AFTER MG_ConfigLoader::Init, because Init() LATCHES the flag out of
    //     MG_Config::Features.PipeStats, and the parser does not exist until the
    //     line above has run. Calling it earlier would latch the static default
    //     (false) and the MOBILEGL_PIPE_STATS the launcher passed would be read by
    //     nobody - the same class of defect the config load itself used to have.
    //   * AFTER MGPipeSetServerProcessRole, because that is what makes the LOG SINK
    //     know this process is the server, and PipeStats' summary line is emitted
    //     through MGLOG_I. The sink resolves its role from MOBILEGL_IPC_ROLE (which
    //     the launcher sets and its envp scrub preserves) and would in fact be
    //     right either way, but ordering it this way means the line cannot be filed
    //     under the client by a future change to that resolution. What the contract
    //     requires - that the summary lands in `<base>.server.log` - is asserted by
    //     the lane, not assumed from this comment.
    //
    // MOBILEGL_PIPE_STATS / _PERIOD / _FILE MEAN EXACTLY WHAT THEY MEAN IN THE
    // CLIENT PROCESS, and that is the whole point of routing through Init(): all
    // three are read from the child's own environment by the same parser and
    // latched by the same function. Nothing about the stats channel is spawn-shaped.
    // One asymmetry is real and intended: this process has no backend Present of
    // its own, so its line cadence rides the present RECORDS it applies
    // (PipeApplier -> DirectGLES/DirectVulkan Present -> OnPresent) rather than a
    // swap the server does not perform. A spawn session that presents N times
    // produces N windows here, which is the same N the client's own line counts.
    MobileGL::MG_Util::PipeStats::Init();
    WireLogError("MG_Remote server: pid=%d config loaded, backend=%d, Transport pinned to Spawn "
                 "(D1: this process IS the server arm)",
                 selfPid, static_cast<int>(MobileGL::MG_Config::ActiveBackendType));

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
    // PARK AND NOTIFY ON DIFFERENT DESCRIPTORS, and this is not a refinement -
    // the single-fd form CANNOT WAKE ITSELF. send() on one end of a socketpair
    // is delivered to the OTHER end, so a one-fd bell built on serverBell[0]
    // writes to serverBell[1] and polls serverBell[0]: correct for waking a
    // PEER, useless for waking a thread in this process. The server needs
    // exactly that, because the control pump posts into the apply thread's
    // mailbox and then has to ring the bell that thread is parked on. Measured
    // symptom when it was wrong: every eglInitialize crossed, landed in the
    // mailbox, and timed out at 5 s because nothing ever woke mgl-srv-apply.
    SocketDoorbell selfBell(/*parkFd=*/serverBell[0], /*notifyFd=*/serverBell[1], /*code=*/1,
                            /*ownsFds=*/true);
    // RINGS ONLY. The client parks on clientBell[0]; writing clientBell[1] is
    // what reaches it. parkFd = -1 says in the type that this side never waits.
    SocketDoorbell peerBell(/*parkFd=*/-1, /*notifyFd=*/clientBell[1], /*code=*/1,
                            /*ownsFds=*/true);

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
        // SLOTS 4, 5 AND 6 ARE THE BELLS - three, not two, and the third is the
        // one the two-fd form above forces:
        //   4  serverBell[1]  the client's handle to RING THE SERVER
        //   5  clientBell[0]  the client's PARK end
        //   6  clientBell[1]  the client's handle to ring ITSELF, for the same
        //                     reason the server needs serverBell[1]
        // Sharing DUPs, so the descriptors these bells own stay valid here.
        const int bellFds[3] = {serverBell[1], clientBell[0], clientBell[1]};
        for (std::uint32_t index = 0; index < 3; ++index) {
            Sideband sideband{4 + index, 0};
            if (control->ShareFd(bellFds[index], MobileGLByteSpan{&sideband, sizeof(sideband)}) !=
                MOBILEGL_OK) {
                std::fprintf(stderr, "MG_Remote server: pid=%d could not share bell %u\n", selfPid,
                             index);
                std::fflush(nullptr);
                ::_exit(69);
            }
        }
        // clientBell[0] is the CLIENT's park end and no bell here owns it; the
        // other three belong to selfBell and peerBell and are closed with them.
        ::close(clientBell[0]);
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
                 "(backend up, seven descriptors handed over, apply thread running)",
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
        // `cp`: decode, apply, reply. THIS THREAD IS NOT THE APPLY THREAD, which
        // is what CONTRACT-P6 §6.2 requires - a control arrival must not wake the
        // applier, and frame decode must not happen on the thread holding the
        // native context. ServerApplyWireSurfaceOp posts through ServerLoop's
        // existing one-slot mailbox, unchanged.
        const ::MobileGL::Wire::CtrlEnvelope* envelope = nullptr;
        {
            ::flatbuffers::Verifier verifier(buffer.data(), static_cast<std::size_t>(size));
            if (::MobileGL::Wire::VerifyCtrlEnvelopeBuffer(verifier)) {
                envelope = ::MobileGL::Wire::GetCtrlEnvelope(buffer.data());
            }
        }
        if (envelope == nullptr || envelope->msg_type() != ::MobileGL::Wire::CtrlMsg::SurfaceOp ||
            envelope->msg_as_SurfaceOp() == nullptr) {
            WireLogError("MG_Remote server: pid=%d a control frame that is not a verifiable "
                         "SurfaceOp arrived; ignoring it would be the silent drop this project "
                         "refuses, so the session ends here",
                         selfPid);
            break;
        }

        Server::SurfaceControlFrame reply{};
        const MobileGLResult applied =
            ServerApplyWireSurfaceOp(*envelope->msg_as_SurfaceOp(), &reply);
        (void)applied; // carried in the reply's ok/result, not thrown away here

        ::flatbuffers::FlatBufferBuilder builder(256);
        EncodeSurfaceReplyFrame(reply, &builder);
        if (control->SendFrame(
                MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()}) != MOBILEGL_OK) {
            break;
        }
    }

    loop.Stop();
    session.Close();
    control->Shutdown();
    // Step 1.6's other half, and its position is the same argument MobileGL::Destroy
    // makes: AFTER loop.Stop(), because the apply thread is what publishes
    // ServerWaits / ServerParks and a dump taken while it still runs is a race; and
    // BEFORE _exit, because _exit runs no atexit handler and this is the only place
    // the server's run totals and its JSON dump can be written at all. Without it
    // the server printed whatever windows its last period boundary happened to
    // reach and lost the final one - the same "the last frame's numbers can be
    // lost" case Init.cpp's Shutdown call exists to avoid on the client side.
    MobileGL::MG_Util::PipeStats::Shutdown();
    std::fflush(nullptr);
    ::_exit(0);
}
