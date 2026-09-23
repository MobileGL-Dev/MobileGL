// MobileGL - MobileGL/MG_Test/Wire/EventForfeitPeerTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Ph fuzz arm 3 (PLAN-PH-P34B-P7 §1.1, "fuzz arm 3: a peer that does not drain"; PH-6, ID-P7-2).
//
// TWO PROCESSES AND A SUPERVISOR, because every property this asserts is one the unit cases in
// RemoteClientTest cannot see: that the forfeit ends the SESSION PROCESS (not just the apply
// thread), that it ends it the ordinary way (exit 0, not a SIGABRT the supervisor counts as a
// fault), inside MOBILEGL_IPC_EVENT_WAIT_MS rather than the old 30 s, and that the supervisor then
// WELCOMES the next connection instead of answering it Busy for as long as the silent peer keeps
// its control connection open.
//
// THE PEER IS THE RAW-RECORD DRIVER'S SHAPE (ServerSpawnTest): a real ClientSession that
// handshakes, brings a pbuffer context up on the server through the EGL forwarders, stages a
// buffer, and then publishes buffer readbacks WITHOUT WAITING - so nothing on this side ever
// drains SEG_EVENT (a waiting client would, from inside its wait). Each readback makes the server
// post one writeback event of a quarter-ring slice: four fill the 256 KiB ring, the fifth cannot.
// Before PH-6 the server parked on that fifth one for 30 000 ms and then aborted with
// Fatal{EventRingOverflow}; now it forfeits the reverse channel after MOBILEGL_IPC_EVENT_WAIT_MS.
//
// BOTH DATA PLANES. The Shm cases are the spawn lane's plane (a unix control socket, SEG_EVENT in
// shared memory, the peer's drain is a cursor it never moves); the Stream cases are the tcp lane's
// (the event bytes cross the data connection, the peer's drain is a progress frame it never
// sends). Each plane runs in its own ctest entry under its own lane label.
//
// THE SCENARIOS (all but the first are the PH-6 fix round's):
//   * the peer stops draining and stays - NotDraining after the knob - and keeps TALKING on
//     control (a LogFlush every 100 ms), so the session must end without the control wait ever
//     timing out;
//   * the peer is KILLED while the server waits, with a knob longer than ServerLoop::Stop()'s
//     5000 ms join - PeerGone well inside the knob, exit 0 (whichever of the data bell's death and
//     the control EOF's stop reaches the waiting apply thread first);
//   * the same kill with a SURFACE OP QUEUED behind the wait - ServerMain is then blocked in the
//     apply thread's mailbox and cannot see the EOF, so only the data bell can report the death:
//     on the shm plane that is the server bell's death witness on the control socket;
//   * the peer stays but its control stream ENDS (a malformed frame) while the server waits -
//     `Stopped`, at once, exit 0, instead of Stop()'s join running out under the long knob;
//   * the peer HALF-CLOSES control (ClientSession::Stop's step 2) and keeps its data connection -
//     PeerGone, at once: on the stream plane only ServerMain's note that the control stream ended
//     can say so, because the data bell is still alive;
//   * the shm plane's server started by Server::LaunchServer, the launcher that SCRUBS the child's
//     MOBILEGL_IPC_* - the knob must survive the scrub (ServerSpawn.cpp's allow-list).
// Every knob here is NOT the default (2000), and every forfeit line is read for the value it
// names, so a knob the server silently ignores is red.
//
// Everything that uses a ClientSession runs in a FORKED CHILD. The session singleton latches
// device-lost for the rest of its process once its server goes away, so the "next connection"
// must come from a process that never had a first one.

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Protocol/SurfaceOpCodec.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Server/SurfaceControlFrame.h>
#include <MG_Remote/Transport/ILink.h>
#include <MG_Remote/Transport/ReplySlot.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MG_Util/Debug/Log.h>
#include <Config.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;
namespace MGP = MobileGL::MG_Pipe;

#if !defined(_WIN32)

namespace {

    // The server's patience in the stop-draining scenario. NOT the default (2000): the forfeit line
    // names the budget it used, and reading this value back is what proves the server parsed the
    // variable rather than fell back. Short enough that a lane pays little for it, long enough that
    // "the session ended after the knob" and "the session ended at once" are different readings.
    constexpr std::uint32_t kEventWaitMs = 1500;
    // The kill scenario's knob: longer than ServerLoop's 5000 ms bounded join (kJoinTimeoutMs), so
    // a wait that only a drain or the knob can end turns the peer's death into
    // Fatal{ApplyThreadJoinTimeout} - and a PeerGone that arrives "well inside" it cannot be the
    // knob running out.
    constexpr std::uint32_t kLongEventWaitMs = 8000;
    // The launched-server scenario's knob, set in the PEER's environment: it reaches the server
    // only through Server::LaunchServer's scrubbed environment.
    constexpr std::uint32_t kLaunchedEventWaitMs = 1200;
    // How long after its readbacks a peer waits before its raw control frame (so the server is
    // parked in the reservation by then), and how long the kill scenarios' peer lives: far under
    // every knob here.
    constexpr std::uint32_t kRawFrameAfterMs = 200;
    constexpr std::uint32_t kKillAfterMs = 400;
    // Five readbacks of one writeback slice each - a quarter of the ring the server announced,
    // less a little for the event's own head and record header, which is the producer-side
    // slicing rule (MGPipeBufferWritebackSliceBytes) - so four fill SEG_EVENT to within a few
    // hundred bytes and the fifth is the one the server waits on and drops. Measured, not
    // assumed: with four readbacks of this size all four fit and nothing is ever dropped.
    constexpr std::uint32_t kReadbacks = 5;
    constexpr MGP::MGPipeHandle kBuffer{41u, 1u};

    std::string ServerImage() {
        if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) return explicitPath;
        return "libMobileGLServer.so";
    }

    std::string WaitKnobEntry(std::uint32_t waitMs) {
        return "MOBILEGL_IPC_EVENT_WAIT_MS=" + std::to_string(waitMs);
    }

    std::uint16_t FreeLoopbackPort() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        std::uint16_t port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
            port = ntohs(address.sin_port);
        }
        ::close(fd);
        return port;
    }

    // tcp_supervisor_smoke.py's supervisor_environment, in C++: ServerMain's catch (b) refuses
    // TRANSPORT / SERVER_PATH / RING_MB / STAGE_MB outright, and nothing a CLIENT was told may
    // leak into the server - so every MOBILEGL_IPC_* goes and the few this scenario means are
    // stated. MOBILEGL_IPC_LOG_FORWARD=0 keeps the server's lines in its own role log, which is
    // what the assertions below read.
    std::vector<std::string> SupervisorEnvironment(const std::string& logBase, std::uint32_t waitMs) {
        std::vector<std::string> env;
        for (char** e = ::environ; e != nullptr && *e != nullptr; ++e) {
            const std::string entry(*e);
            if (entry.rfind("MOBILEGL_IPC_", 0) == 0 || entry.rfind("MOBILEGL_TRANSPORT=", 0) == 0 ||
                entry.rfind("MOBILEGL_BACKEND_TYPE=", 0) == 0 || entry.rfind("MOBILEGL_LOG_FILE_PATH=", 0) == 0) {
                continue;
            }
            env.push_back(entry);
        }
        env.emplace_back("MOBILEGL_IPC_ROLE=server");
        env.emplace_back("MOBILEGL_IPC_DIAL=no");
        env.emplace_back("MOBILEGL_IPC_LOG_FORWARD=0");
        env.emplace_back(WaitKnobEntry(waitMs));
        env.emplace_back("MOBILEGL_LOG_FILE_PATH=" + logBase);
        const std::string image = ServerImage();
        const auto slash = image.find_last_of('/');
        if (slash != std::string::npos) env.emplace_back("LD_LIBRARY_PATH=" + image.substr(0, slash));
        return env;
    }

    pid_t LaunchSupervisor(const std::string& endpoint, const std::string& logBase, std::uint32_t waitMs) {
        const std::string image = ServerImage();
        std::vector<std::string> env = SupervisorEnvironment(logBase, waitMs);
        std::vector<char*> envp;
        for (auto& entry : env) envp.push_back(entry.data());
        envp.push_back(nullptr);
        std::string argv0 = image, argv1 = endpoint, argv2 = "--serve";
        char* argv[] = {argv0.data(), argv1.data(), argv2.data(), nullptr};
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setpgid(0, 0);
            ::execve(image.c_str(), argv, envp.data());
            ::_exit(127);
        }
        return pid;
    }

    void StopSupervisor(pid_t pid) {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 500; ++i) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }

    std::string ServerLog(const std::string& logBase) {
        return MobileGL::MG_Util::Debug::ReadRoleLogs(logBase.c_str());
    }

    std::size_t Count(const std::string& text, const std::string& needle) {
        std::size_t n = 0;
        for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
        return n;
    }

    // The one log line that holds `needle`, for asserting what ELSE that line says.
    std::string LineWith(const std::string& text, const std::string& needle) {
        const auto at = text.find(needle);
        if (at == std::string::npos) return std::string();
        const auto begin = text.rfind('\n', at);
        const auto end = text.find('\n', at);
        return text.substr(begin == std::string::npos ? 0 : begin + 1,
                           (end == std::string::npos ? text.size() : end) - (begin == std::string::npos ? 0 : begin + 1));
    }

    // Polls the supervisor's log until `needle` has appeared `times` times, or the budget runs
    // out. Returns the milliseconds it took, or -1.
    long long WaitForLog(const std::string& logBase, const std::string& needle, std::size_t times,
                         std::chrono::milliseconds budget) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < budget) {
            if (Count(ServerLog(logBase), needle) >= times) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                             start).count();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return -1;
    }

    // THE CLIENT CONFIGURATION a lane's environment would have produced, set directly: a
    // connect-dial to the supervisor, the data plane that endpoint implies.
    void ConfigureClient(const std::string& control) {
        MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::Spawn;
        MobileGL::MG_Config::ActiveBackendType = MobileGL::BackendType::DirectGLES;
        MobileGL::MG_Config::Ipc.Control = control.c_str();
        MobileGL::MG_Config::Ipc.Data = "auto";
        ::unsetenv("MOBILEGL_IPC_TOKEN");
    }

    // The raw-record driver's encode-then-publish, minus the mutation: the record is legal, it is
    // the peer's NOT WAITING that this scenario is about. Publishing through the session's own
    // producer keeps the ring head and submittedSeq monotone.
    template <class Payload>
    bool PublishWithoutWaiting(Client::ClientSession& session, MGP::MGPWireOp op, const Payload& payload) {
        auto* link = session.DataLink();
        if (link == nullptr || !link->Attached()) return false;
        const std::uint64_t seq = session.Encoder().EncodeRecord(op, &payload, sizeof(payload));
        if (seq == Wire::kInvalidSeq) return false;
        session.Producer().PublishAndNotify(seq);
        return link->Flush() == MOBILEGL_OK;
    }

    // Child exit codes, so a red names its step.
    enum : int {
        kChildOk = 0,
        kNoWelcome = 10,
        kEglDisplay = 11,
        kEglSurface = 12,
        kEglCurrent = 13,
        kBufferCreate = 14,
        kBufferRespecify = 15,
        kBufferContent = 16,
        kReadbackPublish = 17,
        kLaunch = 18,
        kServerNotReaped = 19,
        kServerExitedNonZero = 20,
        kRawFrame = 21,
    };

    // Everything a peer does between its handshake and the moment SEG_EVENT is full with the
    // server waiting on one more writeback. Returns kChildOk or the step that failed.
    int FillTheEventRingWithoutDraining(Client::ClientSession& client) {
        // A pbuffer context on the server's apply thread, through the same forwarders
        // BackendObject_Remote uses. The handles are the client's names for them; the server keys
        // its surfaces by them and never dereferences one.
        const auto display = reinterpret_cast<EGLDisplay>(std::uintptr_t{0x4d47});
        const auto surface = reinterpret_cast<EGLSurface>(std::uintptr_t{0x5346});
        const auto context = reinterpret_cast<EGLContext>(std::uintptr_t{0x4358});
        EGLint major = 0, minor = 0;
        if (!Server::ServerInitializeEGLDisplay(display, &major, &minor)) return kEglDisplay;
        if (!Server::ServerCreateEGLPbufferSurface(surface, 16, 16)) return kEglSurface;
        if (!Server::ServerMakeEGLCurrent(display, surface, surface, context)) return kEglCurrent;

        // One buffer with defined content, staged whole: create, respecify, one sub-data record.
        // These three WAIT, and that is fine - the ring holds one small surface event at most so
        // far, and a drain here takes nothing the scenario needs.
        const std::uint64_t kSliceBytes = client.EventRingCapacityBytes() / 4 - 64;
        MGP::MGPResourceDesc desc{};
        desc.Resource = kBuffer;
        desc.Target = MGP::kMGPipeResourceTargetBuffer;
        desc.StorageKind = static_cast<MobileGL::Uint8>(MobileGL::TextureStorageType::Buffer);
        desc.BindMask = MGP::kMGPipeBindShaderBuffer;
        MobileGL::Int32 status = Wire::ReplySink::kStatusError;
        if (client.EmitAndWait(MGP::MGPWireOp::ResourceCreate, &desc, sizeof(desc), nullptr, 0, nullptr, 0,
                               &status) == Wire::kInvalidSeq ||
            status != Wire::ReplySink::kStatusOk) {
            return kBufferCreate;
        }
        desc.Width = static_cast<MobileGL::Uint32>(kSliceBytes);
        desc.Usage = 0; // BufferUsage::StreamDraw, the enum's first value
        desc.HasDefinedContent = 1;
        status = Wire::ReplySink::kStatusError;
        if (client.EmitAndWait(MGP::MGPWireOp::ResourceRespecify, &desc, sizeof(desc), nullptr, 0, nullptr, 0,
                               &status) == Wire::kInvalidSeq ||
            status != Wire::ReplySink::kStatusOk) {
            return kBufferRespecify;
        }
        std::vector<std::uint8_t> bytes(kSliceBytes, 0x5A);
        MGP::MGPSubData content{};
        content.Res = kBuffer;
        content.Target = MGP::kMGPipeResourceTargetBuffer;
        if (!MGP::MGPipeSetSubDataBufferRange(content, 0, kSliceBytes)) return kBufferContent;
        content.Blob = client.Encoder().StageBytes(bytes.data(), bytes.size());
        status = Wire::ReplySink::kStatusError;
        if (client.EmitAndWait(MGP::MGPWireOp::ResourceSubData, &content, sizeof(content), nullptr, 0, nullptr, 0,
                               &status) == Wire::kInvalidSeq ||
            status != Wire::ReplySink::kStatusOk) {
            return kBufferContent;
        }

        // THE PART UNDER TEST. kReadbacks (five) whole-buffer readbacks, published and never
        // waited for: the first four writebacks fill SEG_EVENT, the fifth is the one the server
        // waits on.
        for (std::uint32_t i = 0; i < kReadbacks; ++i) {
            MGP::MGPReadback readback{};
            readback.Res = kBuffer;
            readback.Offset = 0;
            readback.Size = kSliceBytes;
            if (!PublishWithoutWaiting(client, MGP::MGPWireOp::ResourceReadback, readback)) return kReadbackPublish;
        }
        return kChildOk;
    }

    enum class Scenario {
        StopsDraining,              // the peer stays, silent: NotDraining after the knob
        KilledWhileWaiting,         // SIGKILLed while the server waits: PeerGone, at once
        KilledWithAControlOpQueued, // the same, with a surface op stuck behind the wait
        ControlEndsWhileWaiting,    // the peer stays; its control stream ends: Stopped, at once
        ControlHalfClosedWhileWaiting, // the peer half-closes control (a teardown's first act),
                                       // data still open: PeerGone, at once
    };

    // A surface op the server's control loop has to post into the apply thread's mailbox - and
    // cannot, while that thread is parked in the reservation: ServerMain blocks there with it.
    // The seq is far from the session's own dense mint; nothing will read the reply.
    bool SendSurfaceOpBehindTheWait(Client::ClientSession& client) {
        Server::SurfaceControlFrame frame{};
        frame.kind = Server::SurfaceControlOp::InitializeDisplay;
        frame.seq = 0x7fffffffull;
        frame.display = 0x4d47;
        flatbuffers::FlatBufferBuilder builder(256);
        if (EncodeSurfaceOpFrame(frame, &builder) != SurfaceWireError::None) return false;
        auto* control = client.ControlSocketForTest();
        return control != nullptr &&
               control->SendFrame(MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()}) == MOBILEGL_OK;
    }

    // A LogFlush that asks for an acknowledgement (ack=false is the REQUEST; the server's answer
    // carries ack=true), exactly the frame ClientSession::SyncPeerLog sends - minus the drain that
    // SyncPeerLog does first.
    bool SendLogFlush(Client::ClientSession& client, std::uint64_t seq) {
        flatbuffers::FlatBufferBuilder builder(64);
        auto flush = ::MobileGL::Wire::CreateLogFlush(builder, seq, false);
        auto envelope =
            ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::LogFlush, flush.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        auto* control = client.ControlSocketForTest();
        return control != nullptr &&
               control->SendFrame(MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()}) == MOBILEGL_OK;
    }

    // Four bytes: too short to carry a CtrlEnvelope, which ServerMain answers by ending the session
    // (P7 wave 0's named refusal) - a control stream that ENDS while the peer is still there.
    bool SendMalformedControlFrame(Client::ClientSession& client) {
        static const std::uint8_t kJunk[4] = {0xde, 0xad, 0xbe, 0xef};
        auto* control = client.ControlSocketForTest();
        return control != nullptr && control->SendFrame(MobileGLByteSpan{kJunk, sizeof(kJunk)}) == MOBILEGL_OK;
    }

    // THE PEER THAT STOPS DRAINING. Writes one byte to `published` when the last readback is on
    // the wire, puts the scenario's raw control frame (if any) on the control stream once the server
    // is parked, then does nothing at all - no wait, no drain, no teardown - until the parent says
    // the supervisor has reaped its session (or kills it), and leaves by _exit: a clean Stop()
    // would drain.
    [[noreturn]] void NonDrainingPeer(const std::string& control, Scenario scenario, int published, int release) {
        ConfigureClient(control);
        auto& client = Client::ClientSessionInstance();
        if (client.StartSpawned() != MOBILEGL_OK) ::_exit(kNoWelcome);
        const int filled = FillTheEventRingWithoutDraining(client);
        if (filled != kChildOk) ::_exit(filled);
        const char one = 1;
        (void)::write(published, &one, 1);
        if (scenario == Scenario::KilledWithAControlOpQueued || scenario == Scenario::ControlEndsWhileWaiting ||
            scenario == Scenario::ControlHalfClosedWhileWaiting) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kRawFrameAfterMs));
            bool sent = false;
            if (scenario == Scenario::KilledWithAControlOpQueued) {
                sent = SendSurfaceOpBehindTheWait(client);
            } else if (scenario == Scenario::ControlEndsWhileWaiting) {
                sent = SendMalformedControlFrame(client);
            } else {
                // ClientSession::Stop's step 2, alone: the control stream's write half closes, the
                // process and its data connection stay. The server sees EOF on control and nothing
                // at all on the data plane.
                auto* socket = client.ControlSocketForTest();
                sent = socket != nullptr && socket->ShutdownSend() == MOBILEGL_OK;
            }
            if (!sent) ::_exit(kRawFrame);
        }
        if (scenario == Scenario::StopsDraining) {
            // SILENT ON SEG_EVENT, NOT ON CONTROL. A LogFlush every 100 ms, each of which the server
            // answers (and this peer never reads): a peer that talks keeps the server's control
            // wait from ever timing out, so the session may only end because its control loop asks
            // about the apply thread on every turn and not only on a quiet one. Until the server has
            // gone (a send fails) or the parent releases this peer.
            std::uint64_t seq = 0x7f000000ull;
            bool talking = true;
            for (;;) {
                struct pollfd wait{release, POLLIN, 0};
                if (::poll(&wait, 1, 100) != 0) break;
                if (talking) talking = SendLogFlush(client, ++seq);
            }
            ::_exit(kChildOk);
        }
        char go = 0;
        (void)::read(release, &go, 1);
        ::_exit(kChildOk);
    }

    // THE NEXT CONNECTION: a process that never had a session asks for one and must get Welcome.
    [[noreturn]] void NextPeer(const std::string& control) {
        ConfigureClient(control);
        auto& client = Client::ClientSessionInstance();
        if (client.StartSpawned() != MOBILEGL_OK) ::_exit(kNoWelcome);
        client.Stop();
        ::_exit(kChildOk);
    }

    int WaitChild(pid_t pid, std::chrono::milliseconds budget) {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            }
            if (std::chrono::steady_clock::now() - start > budget) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void RemoveRoleLogsIfGreen(const std::string& logBase) {
        // A green run leaves nothing in /tmp; a red one keeps both role logs for the triage. The
        // forfeit line and the reap lines go to this test's own output first, so a green lane log
        // still says what the server named and how the session ended.
        {
            const std::string log = ServerLog(logBase);
            for (const char* needle : {"ReverseChannelForfeit{", "mgl-srv-apply stops on", "reaped "}) {
                for (auto at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) {
                    const std::string line = LineWith(log.substr(at), needle);
                    std::printf("[fz3] %.260s\n", line.c_str());
                }
            }
            std::fflush(stdout);
        }
        if (::testing::Test::HasFailure()) return;
        for (const char* role : {".client.log", ".server.log"}) {
            ::unlink((logBase.substr(0, logBase.size() - 4) + role).c_str());
        }
    }

    const char* ScenarioTag(Scenario scenario) {
        switch (scenario) {
        case Scenario::StopsDraining: return "silent";
        case Scenario::KilledWhileWaiting: return "kill";
        case Scenario::KilledWithAControlOpQueued: return "kill-op";
        case Scenario::ControlEndsWhileWaiting: return "ctl-end";
        case Scenario::ControlHalfClosedWhileWaiting: return "ctl-half";
        }
        return "?";
    }

    void RunArm(bool tcp, Scenario scenario) {
        // A peer the parent kills leaves the release pipe with no reader; the parent's write to it
        // must be an EPIPE, not a SIGPIPE that takes this process down before the supervisor goes.
        std::signal(SIGPIPE, SIG_IGN);
        const bool killed =
            scenario == Scenario::KilledWhileWaiting || scenario == Scenario::KilledWithAControlOpQueued;
        const std::uint32_t waitMs = scenario == Scenario::StopsDraining ? kEventWaitMs : kLongEventWaitMs;
        const std::string tag = std::string(tcp ? "stream-" : "shm-") + ScenarioTag(scenario) + "-" +
                                std::to_string(::getpid());
        const std::string logBase = "/tmp/mgl-fz3-" + tag + ".log";
        MobileGL::MG_Util::Debug::TruncateRoleLogs(logBase.c_str());
        std::string endpoint, control;
        if (tcp) {
            const std::uint16_t port = FreeLoopbackPort();
            ASSERT_NE(port, 0) << "no free loopback port";
            endpoint = "tcp://127.0.0.1:" + std::to_string(port);
            control = endpoint;
        } else {
            endpoint = "@mgl-fz3-" + tag;
            control = "unix:" + endpoint;
        }
        const pid_t supervisor = LaunchSupervisor(endpoint, logBase, waitMs);
        ASSERT_GT(supervisor, 0);
        struct Reap {
            pid_t pid;
            ~Reap() { StopSupervisor(pid); }
        } reapSupervisor{supervisor};
        ASSERT_GE(WaitForLog(logBase, "listening on", 1, std::chrono::milliseconds(10000)), 0)
            << "the supervisor never listened\n" << ServerLog(logBase);

        int published[2] = {-1, -1}, release[2] = {-1, -1};
        ASSERT_EQ(::pipe(published), 0);
        ASSERT_EQ(::pipe(release), 0);
        std::fflush(nullptr);
        const pid_t peer = ::fork();
        ASSERT_GE(peer, 0);
        if (peer == 0) {
            ::close(published[0]);
            ::close(release[1]);
            NonDrainingPeer(control, scenario, published[1], release[0]);
        }
        ::close(published[1]);
        ::close(release[0]);
        struct ReleasePeer {
            int fd;
            pid_t pid;
            ~ReleasePeer() {
                const char go = 1;
                (void)::write(fd, &go, 1);
                ::close(fd);
                int status = 0;
                if (pid > 0 && ::waitpid(pid, &status, WNOHANG) == 0) {
                    ::kill(pid, SIGKILL);
                    ::waitpid(pid, &status, 0);
                }
            }
        } releasePeer{release[1], peer};

        // THE CLOCK STARTS WHEN THE LAST READBACK IS ON THE WIRE. A peer that failed before it
        // got there exits instead of writing, and read() returns 0.
        char byte = 0;
        const ssize_t got = ::read(published[0], &byte, 1);
        ::close(published[0]);
        if (got != 1) {
            const int code = WaitChild(peer, std::chrono::milliseconds(5000));
            releasePeer.pid = -1;
            FAIL() << "the non-draining peer did not reach its readbacks (child exit " << code << ")\n"
                   << ServerLog(logBase);
        }
        const auto published_at = std::chrono::steady_clock::now();

        if (scenario != Scenario::StopsDraining) {
            // THE WAIT ENDS EARLY, BY NAME. The server is parked in the fifth writeback's
            // reservation with 8000 ms of patience - over ServerLoop::Stop()'s 5000 ms join - and
            // either the peer dies (PeerGone) or its control stream ends under it while it stays
            // (Stopped). Both must end that wait well inside the knob, and the session must still
            // end the ordinary way, exit 0. Before the fix round the shm plane could not see the
            // death at all: the control EOF made ServerMain call loop.Stop(), the reservation
            // swallowed Stop()'s ring and waited on, and after 5000 ms the child died of
            // Fatal{ApplyThreadJoinTimeout} (`reaped signal=6 sessionsFaulted=1`); at a knob under
            // the join it was reported as NotDraining after the whole knob.
            // A half-closed control stream is the peer LEAVING - ServerMain says so to the session
            // before it stops the loop, so it is PeerGone even on the stream plane, whose data bell
            // is still alive; a malformed frame from a peer that stays is a stop.
            const char* expected = scenario == Scenario::ControlEndsWhileWaiting
                                       ? "ReverseChannelForfeit{Stopped} - kEventBufferWriteback"
                                       : "ReverseChannelForfeit{PeerGone} - kEventBufferWriteback";
            auto endedAt = std::chrono::steady_clock::now();
            if (killed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kKillAfterMs));
                ASSERT_EQ(::kill(peer, SIGKILL), 0);
                int status = 0;
                ::waitpid(peer, &status, 0);
                releasePeer.pid = -1;
                endedAt = std::chrono::steady_clock::now();
            }
            const long long stoppedMs = WaitForLog(logBase, "mgl-srv-apply stops on ReverseChannelForfeit", 1,
                                                   std::chrono::milliseconds(waitMs + 20000));
            const auto stoppedAfterEnd = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - endedAt).count();
            const long long reapedMs = WaitForLog(logBase, "reaped exit=0 sessionsFaulted=0", 1,
                                                  std::chrono::milliseconds(waitMs + 20000));
            const std::string log = ServerLog(logBase);
            ASSERT_GE(stoppedMs, 0) << "the server never stopped on a forfeit\n" << log;
            ASSERT_GE(reapedMs, 0) << "the supervisor never reaped the session with exit 0\n" << log;
            const std::string forfeit = LineWith(log, "ReverseChannelForfeit{");
            EXPECT_NE(forfeit.find(expected), std::string::npos)
                << "the early end of the wait was not named " << expected << "\n" << log;
            EXPECT_NE(forfeit.find(WaitKnobEntry(waitMs)), std::string::npos)
                << "the forfeit did not run against the knob this supervisor was given\n" << log;
            if (scenario == Scenario::ControlEndsWhileWaiting) {
                EXPECT_NE(log.find("control frame is 4 bytes"), std::string::npos)
                    << "the malformed control frame never reached the server's control loop\n" << log;
            }
            // WELL INSIDE THE KNOB: from a descriptor or the stop request, not from the 8000 ms
            // running out, and not from Stop()'s 5000 ms join either.
            EXPECT_LE(stoppedAfterEnd, 3000) << log;
            EXPECT_EQ(log.find("Fatal{"), std::string::npos) << log;
            EXPECT_EQ(log.find("reaped signal="), std::string::npos) << "a session died of a signal\n" << log;
        } else {
            // The server waits the knob, forfeits by name, stops the apply thread, ends the session;
            // the supervisor reaps it. Budget: the knob plus generous slack - and far under 30 s.
            const long long stoppedMs =
                WaitForLog(logBase, "mgl-srv-apply stops on ReverseChannelForfeit", 1,
                           std::chrono::milliseconds(waitMs + 20000));
            const long long reapedMs = WaitForLog(logBase, "reaped exit=0 sessionsFaulted=0", 1,
                                                  std::chrono::milliseconds(waitMs + 20000));
            const auto sinceReadbacks = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - published_at).count();
            const std::string log = ServerLog(logBase);
            ASSERT_GE(stoppedMs, 0) << "the server never stopped on a forfeit\n" << log;
            ASSERT_GE(reapedMs, 0) << "the supervisor never reaped the forfeited session with exit 0\n" << log;
            const std::string forfeit = LineWith(log, "ReverseChannelForfeit{");
            EXPECT_NE(forfeit.find("ReverseChannelForfeit{NotDraining} - kEventBufferWriteback"), std::string::npos)
                << "the forfeit was not named for the event that could not be placed\n" << log;
            // THE VARIABLE WAS READ: the budget the server names is the one this supervisor was
            // given, not the 2000 ms default it would fall back to.
            EXPECT_NE(forfeit.find(WaitKnobEntry(waitMs)), std::string::npos)
                << "the server did not wait against the MOBILEGL_IPC_EVENT_WAIT_MS it was given\n" << log;
            EXPECT_NE(log.find("mgl-srv-apply stops on ReverseChannelForfeit - 1 reverse-channel event(s) dropped"),
                      std::string::npos)
                << "the drop was not counted (one event: the fifth writeback)\n" << log;
            EXPECT_NE(log.find("the apply thread has stopped (ReverseChannelForfeit"), std::string::npos)
                << "the session child did not end the session from the server's side\n" << log;
            EXPECT_EQ(log.find("Fatal{EventRingOverflow"), std::string::npos) << log;
            EXPECT_EQ(log.find("reaped signal="), std::string::npos) << "a session died of a signal\n" << log;
            // THE KNOB, NOT 30 s: the forfeit waited most of MOBILEGL_IPC_EVENT_WAIT_MS (it cannot
            // have happened before the knob ran out on a peer that never drains), and the session was
            // gone well inside the knob plus the supervisor's 250 ms reap poll.
            EXPECT_GE(stoppedMs, static_cast<long long>(waitMs) * 8 / 10) << log;
            EXPECT_LE(sinceReadbacks, static_cast<long long>(waitMs) + 8000) << log;
        }

        // THE NEXT CONNECTION IS WELCOMED, by the same supervisor - while the silent peer still
        // holds its end of the old connection open, or after it died mid-wait.
        std::fflush(nullptr);
        const pid_t next = ::fork();
        ASSERT_GE(next, 0);
        if (next == 0) NextPeer(control);
        const int nextCode = WaitChild(next, std::chrono::milliseconds(30000));
        EXPECT_EQ(nextCode, static_cast<int>(kChildOk))
            << "the next connection was not welcomed (child exit " << nextCode << ")\n" << ServerLog(logBase);
        EXPECT_GE(WaitForLog(logBase, "reaped exit=0 sessionsFaulted=0", 2, std::chrono::milliseconds(10000)), 0)
            << "the welcomed session did not end cleanly\n" << ServerLog(logBase);
        EXPECT_EQ(Count(ServerLog(logBase), "Refuse{Busy}"), 0u) << ServerLog(logBase);
        RemoveRoleLogsIfGreen(logBase);
    }

    // THE LAUNCHER'S SCRUB, crossed (PH-6 fix round). Server::LaunchServer builds the child's
    // environment by removing every MOBILEGL_IPC_* except the few the server owns
    // (ServerSpawn.cpp ShouldScrub's allow-list). The supervisor arms above are started by this test
    // with an environment it writes itself, so they cannot see that list; this one is started the
    // way a fork-dial client starts its server, with the knob set in the PEER's environment, and
    // the server's own forfeit line has to name it.
    void RunLaunchedArm() {
        const std::string tag = "launched-" + std::to_string(::getpid());
        const std::string logBase = "/tmp/mgl-fz3-" + tag + ".log";
        const std::string endpoint = "@mgl-fz3-" + tag;
        MobileGL::MG_Util::Debug::TruncateRoleLogs(logBase.c_str());

        int published[2] = {-1, -1};
        ASSERT_EQ(::pipe(published), 0);
        std::fflush(nullptr);
        const pid_t peer = ::fork();
        ASSERT_GE(peer, 0);
        if (peer == 0) {
            ::close(published[0]);
            // What a lane would have exported: the client's own environment, which the launcher
            // scrubs on the way to the server.
            ::setenv("MOBILEGL_IPC_EVENT_WAIT_MS", std::to_string(kLaunchedEventWaitMs).c_str(), 1);
            ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
            MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::Spawn;
            MobileGL::MG_Config::ActiveBackendType = MobileGL::BackendType::DirectGLES;
            ::unsetenv("MOBILEGL_IPC_TOKEN");
            Server::LaunchedServer server;
            if (Server::LaunchServer(ServerImage(), endpoint, &server) != MOBILEGL_OK) ::_exit(kLaunch);
            std::unique_ptr<Transport::SocketTransport> transport;
            if (Transport::SocketTransport::ConnectTo(endpoint, 10000, transport) != MOBILEGL_OK) ::_exit(kNoWelcome);
            auto& client = Client::ClientSessionInstance();
            if (client.StartOverSocket(std::move(transport)) != MOBILEGL_OK) ::_exit(kNoWelcome);
            const int filled = FillTheEventRingWithoutDraining(client);
            if (filled != kChildOk) ::_exit(filled);
            const char one = 1;
            (void)::write(published[1], &one, 1);
            // The single-session server ends its session on the forfeit and exits by itself; this
            // peer, its parent, is the one that can say how.
            int exitCode = -1;
            if (Server::ReapServer(server, kLaunchedEventWaitMs + 15000, &exitCode) != MOBILEGL_OK) {
                ::kill(server.pid, SIGKILL);
                ::_exit(kServerNotReaped);
            }
            ::_exit(exitCode == 0 ? kChildOk : kServerExitedNonZero);
        }
        ::close(published[1]);
        char byte = 0;
        const ssize_t got = ::read(published[0], &byte, 1);
        ::close(published[0]);
        const auto published_at = std::chrono::steady_clock::now();
        const int code = WaitChild(peer, std::chrono::milliseconds(kLaunchedEventWaitMs + 20000));
        const auto sinceReadbacks = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - published_at).count();
        const std::string log = ServerLog(logBase);
        ASSERT_EQ(got, 1) << "the peer did not reach its readbacks (child exit " << code << ")\n" << log;
        EXPECT_EQ(code, static_cast<int>(kChildOk))
            << "the launched server was not reaped with exit 0 (child exit " << code << ")\n" << log;
        const std::string forfeit = LineWith(log, "ReverseChannelForfeit{");
        EXPECT_NE(forfeit.find("ReverseChannelForfeit{NotDraining} - kEventBufferWriteback"), std::string::npos)
            << log;
        EXPECT_NE(forfeit.find(WaitKnobEntry(kLaunchedEventWaitMs)), std::string::npos)
            << "the launched server did not wait against the knob its client exported - the launcher "
               "scrubbed it\n" << log;
        EXPECT_NE(log.find("the apply thread has stopped (ReverseChannelForfeit"), std::string::npos) << log;
        EXPECT_EQ(log.find("Fatal{"), std::string::npos) << log;
        EXPECT_GE(sinceReadbacks, static_cast<long long>(kLaunchedEventWaitMs) * 8 / 10) << log;
        EXPECT_LE(sinceReadbacks, static_cast<long long>(kLaunchedEventWaitMs) + 8000) << log;
        RemoveRoleLogsIfGreen(logBase);
    }

} // namespace

TEST(EventForfeitPeer, ShmPeerThatStopsDrainingIsForfeitedAndTheNextConnectionIsWelcomed) {
    RunArm(false, Scenario::StopsDraining);
}

TEST(EventForfeitPeer, ShmPeerKilledWhileTheServerWaitsIsPeerGoneWellInsideTheKnob) {
    RunArm(false, Scenario::KilledWhileWaiting);
}

TEST(EventForfeitPeer, ShmPeerKilledWithASurfaceOpQueuedBehindTheWaitIsPeerGoneFromTheBell) {
    RunArm(false, Scenario::KilledWithAControlOpQueued);
}

TEST(EventForfeitPeer, ShmControlStreamThatEndsWhileTheServerWaitsStopsTheWaitByName) {
    RunArm(false, Scenario::ControlEndsWhileWaiting);
}

TEST(EventForfeitPeer, ShmControlHalfClosedWhileTheServerWaitsIsPeerGone) {
    RunArm(false, Scenario::ControlHalfClosedWhileWaiting);
}

TEST(EventForfeitPeer, ShmLaunchedServerTakesTheWaitKnobThroughTheScrubbedEnvironment) { RunLaunchedArm(); }

TEST(EventForfeitPeer, StreamPeerThatStopsDrainingIsForfeitedAndTheNextConnectionIsWelcomed) {
    RunArm(true, Scenario::StopsDraining);
}

TEST(EventForfeitPeer, StreamPeerKilledWhileTheServerWaitsIsPeerGoneWellInsideTheKnob) {
    RunArm(true, Scenario::KilledWhileWaiting);
}

TEST(EventForfeitPeer, StreamPeerKilledWithASurfaceOpQueuedBehindTheWaitIsPeerGoneFromTheBell) {
    RunArm(true, Scenario::KilledWithAControlOpQueued);
}

TEST(EventForfeitPeer, StreamControlStreamThatEndsWhileTheServerWaitsStopsTheWaitByName) {
    RunArm(true, Scenario::ControlEndsWhileWaiting);
}

TEST(EventForfeitPeer, StreamControlHalfClosedWhileTheServerWaitsIsPeerGone) {
    RunArm(true, Scenario::ControlHalfClosedWhileWaiting);
}

#endif
