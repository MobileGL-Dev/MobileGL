// MobileGL - MobileGL/MG_Test/Wire/InProcessServerTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), D5 and D8, on the Linux host.
//
// D5: THE IN-PROCESS DISPLAY SERVER, RUN ON A THREAD. The on-screen server is the display Activity's
// own process: mobilegl_server_serve_inprocess serves tcp:// on a thread and runs every session on a
// thread of the same process (an ANativeWindow survives neither fork nor exec). Nothing about that
// needs Android, so it is proven here: a process forked from this test runs the entry on a
// std::thread - its main thread only waits for SIGTERM to call mobilegl_server_stop_inprocess - and
// forked peers talk to it over loopback TCP. What is proven:
//   * sessions run one after another IN ONE PROCESS (every Welcome names the server process's own
//     pid) and each is a real one (a record applied, a pbuffer made current, a green clear read back
//     where headless EGL exists) - so RunSession returned and cleaned up after each;
//   * a session that LATCHED does not decline the next (ResetSessionLatch between sessions);
//   * an authenticated second Hello while a session is live is Refuse{Busy};
//   * the first session's backend is pinned for the process lifetime;
//   * stop returns the serving thread with 0 and the process exits cleanly.
//
// D8: A FORKED SESSION CHILD DIES WITH ITS SUPERVISOR (PR_SET_PDEATHSIG). The exec'd supervisor is
// SIGKILLed with a session live and its client holding the connection; this test is the child
// subreaper, so the orphaned session child is reaped HERE - killed by SIGKILL, not still running.

#include "P12ServerRig.h"

#include <MG_Remote/Server/InProcessServer.h>

#include <atomic>
#include <csignal>
#include <pthread.h>
#include <sys/prctl.h>

using namespace MobileGL;
using namespace P12;

namespace {

    // THE PROCESS THAT IS THE IN-PROCESS DISPLAY SERVER. The environment is the one the display
    // Activity sets before loading the library; the entry runs on a thread; SIGTERM (blocked in every
    // thread, taken synchronously here) is the stop.
    [[noreturn]] void RunInProcessServerProcess(const std::string& endpoint, const std::string& logBase) {
        ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
        ::setenv("MOBILEGL_IPC_DIAL", "no", 1);
        for (const char* name : {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB",
                                 "MOBILEGL_IPC_STAGE_MB", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_SURFACE",
                                 "MOBILEGL_BACKEND_TYPE"})
            ::unsetenv(name);
        sigset_t stop;
        sigemptyset(&stop);
        sigaddset(&stop, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stop, nullptr);
        std::atomic<bool> returned{false};
        std::atomic<int> served{-1};
        std::thread server([&] {
            served.store(mobilegl_server_serve_inprocess(endpoint.c_str()));
            returned.store(true);
        });
        bool stopping = false;
        while (!returned.load()) {
            timespec slice{0, 100 * 1000 * 1000};
            if (sigtimedwait(&stop, nullptr, &slice) == SIGTERM && !stopping) {
                stopping = true;
                mobilegl_server_stop_inprocess();
            }
        }
        server.join();
        std::fflush(nullptr);
        // 0 only for the orderly shape: stopped, and the serving thread returned 0.
        ::_exit(stopping && served.load() == 0 ? 0 : 100 + (served.load() & 0x7f));
    }

    bool LaunchInProcessServer(const std::string& label, ServerProcess* server) {
        server->logBase = "/tmp/mgl-p12-inproc-" + label + "-" + std::to_string(::getpid()) + ".log";
        PinHeadlessEgl();
        for (int attempt = 0; attempt < 5; ++attempt) {
            const int port = FreeLoopbackPort();
            if (port <= 0) return false;
            server->endpoint = "tcp://127.0.0.1:" + std::to_string(port);
            Debug::TruncateRoleLogs(server->logBase.c_str());
            std::fflush(nullptr);
            const pid_t pid = ::fork();
            if (pid < 0) return false;
            if (pid == 0) RunInProcessServerProcess(server->endpoint, server->logBase);
            server->pid = pid;
            bool listening = false;
            (void)WaitFor(
                [&] {
                    if (server->Log().find("listening on") != std::string::npos) return listening = true;
                    return server->WaitExit(0) && server->pid <= 0;
                },
                10000);
            if (listening) return true;
            if (server->pid > 0) return false;
        }
        return false;
    }

    // The in-process supervisor's reap line for its n-th session, or "" while it has not been reaped.
    std::string InProcessReapLine(const ServerProcess& server, unsigned ordinal) {
        const std::string log = server.Log();
        const std::string needle = "in-process session #" + std::to_string(ordinal) + " pid=";
        for (auto at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) {
            const auto end = log.find('\n', at);
            const std::string line = log.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (line.find(" reaped ") != std::string::npos) return line;
        }
        return {};
    }

    void ApplyOneRecord(Remote::Client::ClientSession& client, PeerReport& r) {
        P::MGPMemoryBarrier barrier{};
        barrier.Bits = 0x2000u;
        r.applied = Emit(client, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
    }

    void LatchTheSession(Remote::Client::ClientSession& client, PeerReport& r) {
        // A CAMetalLayer* on the wire: Fatal{UnmigratedSurface, "MetalLayer@P12"}, latched.
        (void)SendSurfaceOp(client, r, static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface),
                            static_cast<Uint8>(::MobileGL::Wire::WindowKind::MetalLayer), 0x1234);
    }

} // namespace

// D5, THE HEADLINE. Three sessions, one process: a healthy one, one that latches, and a healthy one
// after it. Red with ResetSessionLatch removed from the supervisor's reap: session C's Welcome arrives
// but the latch still stands, so its apply thread declines the ring (applied 0) and its EGL ops are
// answered PROTOCOL_MISMATCH.
TEST(InProcessServer, SequentialSessionsShareOneProcessAndALatchedSessionDoesNotDeclineTheNext) {
    ServerProcess server;
    ASSERT_TRUE(LaunchInProcessServer("seq", &server)) << "the in-process server did not start:\n" << server.Log();
    const auto serverPid = static_cast<Uint32>(server.pid);

    const PeerReport a = RunPeer(server.endpoint, HealthySession, /*stopCleanly=*/true);
    ASSERT_EQ(a.started, 1) << "session A got no Welcome (" << a.note << ")\n" << server.Log();
    EXPECT_EQ(a.serverPid, serverPid) << "the session is not the in-process server's own process";
    EXPECT_EQ(a.applied, 1) << a.note;
    std::string reapA;
    ASSERT_TRUE(WaitFor([&] { return !(reapA = InProcessReapLine(server, 1)).empty(); }, 15000)) << server.Log();
    EXPECT_NE(reapA.find("exit=0 "), std::string::npos) << reapA;

    const PeerReport b = RunPeer(server.endpoint, LatchTheSession, /*stopCleanly=*/false);
    ASSERT_EQ(b.started, 1) << "session B got no Welcome (" << b.note << ")";
    EXPECT_EQ(b.serverPid, serverPid);
    std::string reapB;
    ASSERT_TRUE(WaitFor([&] { return !(reapB = InProcessReapLine(server, 2)).empty(); }, 15000)) << server.Log();
    EXPECT_NE(reapB.find("exit=75 (latched fault)"), std::string::npos) << "session B did not end latched: " << reapB;

    const PeerReport c = RunPeer(server.endpoint, HealthySession, /*stopCleanly=*/true);
    ASSERT_EQ(c.started, 1) << "session C got no Welcome after a latched session (" << c.note << ")\n" << server.Log();
    EXPECT_EQ(c.serverPid, serverPid);
    EXPECT_EQ(c.applied, 1) << "session C did not apply a record: the latch was not reset (" << c.note << ")";
    std::string reapC;
    ASSERT_TRUE(WaitFor([&] { return !(reapC = InProcessReapLine(server, 3)).empty(); }, 15000)) << server.Log();
    EXPECT_NE(reapC.find("exit=0 "), std::string::npos) << reapC;
    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 1u) << "only session B may latch";

    // STOP: the serving thread returns 0 and the process exits cleanly (exit 0 is only produced for a
    // stop that the serving thread answered with 0).
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
    EXPECT_NE(server.Log().find("in-process display server stopped"), std::string::npos);

    // Where headless EGL exists, both healthy sessions rendered - the backend came up, went away with
    // session A and came up again in the same process.
    if (a.egl == 0 || c.egl == 0) {
        GTEST_SKIP() << "sessions, latch reset and stop proven; the render half needs headless EGL (" << a.note
                     << " / " << c.note << ")";
    }
    EXPECT_EQ(a.status, Remote::Wire::ReplySink::kStatusOk) << a.note;
    EXPECT_EQ(a.pixel, 0xFF00FF00u) << "session A's clear did not read back green";
    EXPECT_EQ(c.status, Remote::Wire::ReplySink::kStatusOk) << c.note;
    EXPECT_EQ(c.pixel, 0xFF00FF00u) << "session C's clear did not read back green";
}

// D5: ONE SESSION AT A TIME. An authenticated second Hello while a session is live is refused Busy
// by the supervisor thread - and served once the first has gone. Red with the thread hand-off leaving
// `m_active` unset: the second Hello is handed a second session thread.
TEST(InProcessServer, AnAuthenticatedSecondHelloWhileASessionIsLiveIsRefusedBusy) {
    ServerProcess server;
    ASSERT_TRUE(LaunchInProcessServer("busy", &server)) << server.Log();
    pid_t held = -1;
    const PeerReport a = RunPeer(server.endpoint, ApplyOneRecord, /*stopCleanly=*/false, 30000, /*holdMs=*/5000, &held);
    ASSERT_EQ(a.started, 1) << a.note << "\n" << server.Log();
    ASSERT_EQ(a.applied, 1) << a.note;

    const PeerReport b = RunPeer(server.endpoint, ApplyOneRecord, /*stopCleanly=*/true, 15000);
    EXPECT_EQ(b.started, 0) << "a second session was admitted while one was live";
    EXPECT_NE(server.Log().find("Refuse{Busy} one session is already active"), std::string::npos) << server.Log();

    ReleaseHeldPeer(held);
    ASSERT_TRUE(WaitFor([&] { return !InProcessReapLine(server, 1).empty(); }, 15000)) << server.Log();
    const PeerReport c = RunPeer(server.endpoint, ApplyOneRecord, /*stopCleanly=*/true);
    EXPECT_EQ(c.started, 1) << "the next session after the busy one was not served (" << c.note << ")";
    EXPECT_EQ(c.applied, 1);
}

// D5: THE BACKEND IS PINNED FOR THE PROCESS LIFETIME. With no MOBILEGL_BACKEND_TYPE the first
// session's backend (Espryt here) is the pin, and a later Hello asking for Magma is refused by the
// pinned-backend refusal - Espryt and Magma never share a process. Red with the in-process pin
// deleted: session B is admitted.
TEST(InProcessServer, TheFirstSessionsBackendIsPinnedForTheProcessLifetime) {
    ServerProcess server;
    ASSERT_TRUE(LaunchInProcessServer("pin", &server)) << server.Log();
    const PeerReport a = RunPeer(server.endpoint, ApplyOneRecord, /*stopCleanly=*/true);
    ASSERT_EQ(a.started, 1) << a.note << "\n" << server.Log();
    ASSERT_TRUE(WaitFor([&] { return !InProcessReapLine(server, 1).empty(); }, 15000)) << server.Log();

    const PeerReport b = RunPeer(server.endpoint, ApplyOneRecord, /*stopCleanly=*/true, 30000, 0, nullptr, [] {
        MobileGL::MG_Config::ActiveBackendType = MobileGL::BackendType::DirectVulkan;
    });
    EXPECT_EQ(b.started, 0) << "a Magma session was admitted into a process whose first session was Espryt";
    const std::string log = server.Log();
    EXPECT_NE(log.find("pinned backend type"), std::string::npos) << log;
    EXPECT_NE(log.find("Hello.backendType disagrees with pinned backend"), std::string::npos) << log;
}

// D8. Red with DieWithSupervisor's prctl deleted: the session child is still running (its client is
// holding the connection) when the wait for it runs out.
TEST(SupervisorChildren, AForkedSessionChildDiesWithItsSupervisor) {
    // Orphans of this test's descendants are reparented HERE, so the session child can be reaped and
    // its cause of death read.
    ASSERT_EQ(::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0), 0);
    ServerProcess supervisor;
    ASSERT_TRUE(LaunchSupervisor("pdeathsig", &supervisor)) << supervisor.Log();
    pid_t held = -1;
    const PeerReport a =
        RunPeer(supervisor.endpoint, ApplyOneRecord, /*stopCleanly=*/false, 30000, /*holdMs=*/8000, &held);
    ASSERT_EQ(a.started, 1) << a.note << "\n" << supervisor.Log();
    const auto sessionChild = static_cast<pid_t>(a.serverPid);
    ASSERT_GT(sessionChild, 0);
    ASSERT_NE(sessionChild, supervisor.pid) << "not a forked session child";
    ASSERT_EQ(::kill(sessionChild, 0), 0) << "the session child is not running before the supervisor dies";

    ::kill(supervisor.pid, SIGKILL);
    ASSERT_TRUE(supervisor.WaitExit(5000));
    int status = 0;
    const bool reaped = WaitFor(
        [&] { return ::waitpid(sessionChild, &status, WNOHANG) == sessionChild; }, 5000);
    EXPECT_TRUE(reaped) << "the session child outlived its supervisor by 5 s with its client still connected";
    if (reaped) {
        EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
            << "the session child ended, but not by the parent-death SIGKILL (status 0x" << std::hex << status << ")";
    } else {
        ::kill(sessionChild, SIGKILL);
        (void)::waitpid(sessionChild, &status, 0);
    }
    ReleaseHeldPeer(held);
}
