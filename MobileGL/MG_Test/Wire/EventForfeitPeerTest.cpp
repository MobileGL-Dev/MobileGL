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
// BOTH DATA PLANES. The Shm case is the spawn lane's plane (a unix control socket, SEG_EVENT in
// shared memory, the peer's drain is a cursor it never moves); the Stream case is the tcp lane's
// (the event bytes cross the data connection, the peer's drain is a progress frame it never
// sends). Each runs in its own ctest entry under its own lane label.
//
// Everything that uses a ClientSession runs in a FORKED CHILD. The session singleton latches
// device-lost for the rest of its process once its server goes away, so the "next connection"
// must come from a process that never had a first one.

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Transport/ILink.h>
#include <MG_Remote/Transport/ReplySlot.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
#include <MG_Util/Debug/Log.h>
#include <Config.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;
namespace MGP = MobileGL::MG_Pipe;

#if !defined(_WIN32)

namespace {

    // The server's patience in this scenario. Short enough that a lane pays little for it, long
    // enough that "the session ended after the knob" and "the session ended at once" are
    // different readings, and 15x under the 30 s constant it replaced.
    constexpr std::uint32_t kEventWaitMs = 2000;
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
    std::vector<std::string> SupervisorEnvironment(const std::string& logBase) {
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
        env.emplace_back("MOBILEGL_IPC_EVENT_WAIT_MS=" + std::to_string(kEventWaitMs));
        env.emplace_back("MOBILEGL_LOG_FILE_PATH=" + logBase);
        const std::string image = ServerImage();
        const auto slash = image.find_last_of('/');
        if (slash != std::string::npos) env.emplace_back("LD_LIBRARY_PATH=" + image.substr(0, slash));
        return env;
    }

    pid_t LaunchSupervisor(const std::string& endpoint, const std::string& logBase) {
        const std::string image = ServerImage();
        std::vector<std::string> env = SupervisorEnvironment(logBase);
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
    };

    // THE PEER THAT STOPS DRAINING. Writes one byte to `published` when the fourth readback is on
    // the wire, then does nothing at all - no wait, no drain, no teardown - until the parent says
    // the supervisor has reaped its session, and leaves by _exit: a clean Stop() would drain.
    [[noreturn]] void NonDrainingPeer(const std::string& control, int published, int release) {
        ConfigureClient(control);
        auto& client = Client::ClientSessionInstance();
        if (client.StartSpawned() != MOBILEGL_OK) ::_exit(kNoWelcome);

        // A pbuffer context on the server's apply thread, through the same forwarders
        // BackendObject_Remote uses. The handles are the client's names for them; the server keys
        // its surfaces by them and never dereferences one.
        const auto display = reinterpret_cast<EGLDisplay>(std::uintptr_t{0x4d47});
        const auto surface = reinterpret_cast<EGLSurface>(std::uintptr_t{0x5346});
        const auto context = reinterpret_cast<EGLContext>(std::uintptr_t{0x4358});
        EGLint major = 0, minor = 0;
        if (!Server::ServerInitializeEGLDisplay(display, &major, &minor)) ::_exit(kEglDisplay);
        if (!Server::ServerCreateEGLPbufferSurface(surface, 16, 16)) ::_exit(kEglSurface);
        if (!Server::ServerMakeEGLCurrent(display, surface, surface, context)) ::_exit(kEglCurrent);

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
            ::_exit(kBufferCreate);
        }
        desc.Width = static_cast<MobileGL::Uint32>(kSliceBytes);
        desc.Usage = 0; // BufferUsage::StreamDraw, the enum's first value
        desc.HasDefinedContent = 1;
        status = Wire::ReplySink::kStatusError;
        if (client.EmitAndWait(MGP::MGPWireOp::ResourceRespecify, &desc, sizeof(desc), nullptr, 0, nullptr, 0,
                               &status) == Wire::kInvalidSeq ||
            status != Wire::ReplySink::kStatusOk) {
            ::_exit(kBufferRespecify);
        }
        std::vector<std::uint8_t> bytes(kSliceBytes, 0x5A);
        MGP::MGPSubData content{};
        content.Res = kBuffer;
        content.Target = MGP::kMGPipeResourceTargetBuffer;
        if (!MGP::MGPipeSetSubDataBufferRange(content, 0, kSliceBytes)) ::_exit(kBufferContent);
        content.Blob = client.Encoder().StageBytes(bytes.data(), bytes.size());
        status = Wire::ReplySink::kStatusError;
        if (client.EmitAndWait(MGP::MGPWireOp::ResourceSubData, &content, sizeof(content), nullptr, 0, nullptr, 0,
                               &status) == Wire::kInvalidSeq ||
            status != Wire::ReplySink::kStatusOk) {
            ::_exit(kBufferContent);
        }

        // THE PART UNDER TEST. Four whole-buffer readbacks, published and never waited for.
        for (std::uint32_t i = 0; i < kReadbacks; ++i) {
            MGP::MGPReadback readback{};
            readback.Res = kBuffer;
            readback.Offset = 0;
            readback.Size = kSliceBytes;
            if (!PublishWithoutWaiting(client, MGP::MGPWireOp::ResourceReadback, readback)) ::_exit(kReadbackPublish);
        }
        const char one = 1;
        (void)::write(published, &one, 1);
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

    void RunArm(bool tcp) {
        const std::string tag = std::string(tcp ? "stream" : "shm") + "-" + std::to_string(::getpid());
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
        const pid_t supervisor = LaunchSupervisor(endpoint, logBase);
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
            NonDrainingPeer(control, published[1], release[0]);
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
                if (::waitpid(pid, &status, WNOHANG) == 0) {
                    ::kill(pid, SIGKILL);
                    ::waitpid(pid, &status, 0);
                }
            }
        } releasePeer{release[1], peer};

        // THE CLOCK STARTS WHEN THE FOURTH READBACK IS ON THE WIRE. A peer that failed before it
        // got there exits instead of writing, and read() returns 0.
        char byte = 0;
        const ssize_t got = ::read(published[0], &byte, 1);
        ::close(published[0]);
        if (got != 1) {
            const int code = WaitChild(peer, std::chrono::milliseconds(5000));
            FAIL() << "the non-draining peer did not reach its readbacks (child exit " << code << ")\n"
                   << ServerLog(logBase);
        }
        const auto published_at = std::chrono::steady_clock::now();

        // The server waits the knob, forfeits by name, stops the apply thread, ends the session;
        // the supervisor reaps it. Budget: the knob plus generous slack - and far under 30 s.
        const long long stoppedMs =
            WaitForLog(logBase, "mgl-srv-apply stops on ReverseChannelForfeit", 1,
                       std::chrono::milliseconds(kEventWaitMs + 20000));
        const long long reapedMs = WaitForLog(logBase, "reaped exit=0 sessionsFaulted=0", 1,
                                              std::chrono::milliseconds(kEventWaitMs + 20000));
        const auto sinceReadbacks = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - published_at).count();
        const std::string log = ServerLog(logBase);
        ASSERT_GE(stoppedMs, 0) << "the server never stopped on a forfeit\n" << log;
        ASSERT_GE(reapedMs, 0) << "the supervisor never reaped the forfeited session with exit 0\n" << log;
        EXPECT_NE(log.find("ReverseChannelForfeit{NotDraining} - kEventBufferWriteback"), std::string::npos)
            << "the forfeit was not named for the event that could not be placed\n" << log;
        EXPECT_NE(log.find("mgl-srv-apply stops on ReverseChannelForfeit - 1 reverse-channel event(s) dropped"),
                  std::string::npos)
            << "the drop was not counted (one event: the fourth writeback)\n" << log;
        EXPECT_NE(log.find("the apply thread has stopped (ReverseChannelForfeit"), std::string::npos)
            << "the session child did not end the session from the server's side\n" << log;
        EXPECT_EQ(log.find("Fatal{EventRingOverflow"), std::string::npos) << log;
        EXPECT_EQ(log.find("reaped signal="), std::string::npos) << "a session died of a signal\n" << log;
        // THE KNOB, NOT 30 s: the forfeit waited most of MOBILEGL_IPC_EVENT_WAIT_MS (it cannot
        // have happened before the knob ran out on a peer that never drains), and the session was
        // gone well inside the knob plus the supervisor's 250 ms reap poll.
        EXPECT_GE(stoppedMs, static_cast<long long>(kEventWaitMs) * 8 / 10) << log;
        EXPECT_LE(sinceReadbacks, static_cast<long long>(kEventWaitMs) + 8000) << log;

        // THE NEXT CONNECTION IS WELCOMED, by the same supervisor, while the silent peer still
        // holds its end of the old connection open.
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
        // A green run leaves nothing in /tmp; a red one keeps both role logs for the triage.
        if (!::testing::Test::HasFailure()) {
            for (const char* role : {".client.log", ".server.log"}) {
                ::unlink((logBase.substr(0, logBase.size() - 4) + role).c_str());
            }
        }
    }

} // namespace

TEST(EventForfeitPeer, ShmPeerThatStopsDrainingIsForfeitedAndTheNextConnectionIsWelcomed) { RunArm(false); }

TEST(EventForfeitPeer, StreamPeerThatStopsDrainingIsForfeitedAndTheNextConnectionIsWelcomed) { RunArm(true); }

#endif
