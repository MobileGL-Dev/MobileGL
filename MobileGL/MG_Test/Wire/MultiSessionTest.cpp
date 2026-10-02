// MobileGL - MobileGL/MG_Test/Wire/MultiSessionTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P14 S2 (docs/Disaggregated/design/11-state-ownership.md): SEVERAL SESSIONS IN ONE PROCESS.
//
// WHAT THIS SUITE OWNS. The in-process display server's unix supervisor used to serve one session
// at a time (an authenticated second client was Refuse{Busy}); it now runs each session on its own
// control thread with its own ServerSession/ServerLoop/PipeInputs block, its own latch domain and
// its own segment resolver (Server/SessionRuntime.h), up to a cap. Nothing here needs Android or a
// GPU: two clients open the same server's unix endpoint and both are served.
//
// THE THREE PROPERTIES, one case each:
//   * TwoSessionsInOneProcessAreServedAtOnce - both clients are live AT THE SAME TIME and every
//     Welcome names the SAME server process (red once by restoring the one-at-a-time supervisor:
//     the second Hello is Refuse{Busy} and `started` is 0). With headless EGL, each session clears
//     its own colour and reads it back - the clear's colour travels in that session's records, so a
//     read-back that came from the other session's state would be the other colour.
//   * SameNamesInTwoSessionsDoNotCollide - the two sessions mint the SAME names: the same client
//     context token (the session's {token -> ContextRuntime} table) and the same allocatable render
//     state handle and generation (the session's applier table). Both must be accepted, because a
//     name is a name IN A SESSION. Red once by sharing either table: the second CreateContext
//     answers ok=false and the second CreateRenderState is
//     Fatal{ProtocolCorruption, "Cso.handle"} through the latch (the session ends, exit 75).
//   * ClosingOneSessionLeavesTheOtherServing - session A ends cleanly while B is live and B keeps
//     applying records. Red once by releasing the process-wide pieces per session instead of per
//     process: A's Close uninstalls the reverse-channel callback table and the segment resolver
//     that B is still using, so B's next record is a Fatal{ProtocolCorruption} on a table-less
//     resolve or the session's own latch.
//
// WHAT THIS SUITE DOES NOT OWN, AND SAYS SO. The two sessions' NATIVE state (DirectGLES' display,
// config, context and surface are process-global today) is P14b's: one session's teardown still
// tears down the process's EGL context, so the render half is exercised while BOTH sessions are
// alive and never after one has closed, and the post-close proof is record-level (a record applied
// and answered) rather than a second read-back.

#include "P12ServerRig.h"

#include <MG_Remote/Server/InProcessServer.h>

#include <atomic>
#include <csignal>
#include <pthread.h>
#include <sys/prctl.h>

using namespace MobileGL;
using namespace P12;

namespace {

    // ---- the server: this test's own fork, the display server entry on a thread, a UNIX endpoint
    // (the TCP in-process shape keeps its one-session-at-a-time supervisor; this slice changed the
    // unix one, which is the shape anland's display server uses).

    [[noreturn]] void RunUnixInProcessServerProcess(const std::string& endpoint, const std::string& logBase) {
        ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
        ::setenv("MOBILEGL_IPC_DIAL", "no", 1);
        for (const char* name : {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB",
                                 "MOBILEGL_IPC_STAGE_MB", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_SURFACE",
                                 "MOBILEGL_IPC_INPROC_MAX_SESSIONS", "MOBILEGL_BACKEND_TYPE"})
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
        ::_exit(stopping && served.load() == 0 ? 0 : 100 + (served.load() & 0x7f));
    }

    bool LaunchUnixInProcessServer(ServerProcess* server, const std::string& label) {
        server->endpoint = "@mgl-multi-" + label + "-" + std::to_string(::getpid());
        server->logBase = "/tmp/mgl-multi-" + label + "-" + std::to_string(::getpid()) + ".log";
        ::setenv("MOBILEGL_LOG_FILE_PATH", server->logBase.c_str(), 1);
        PinHeadlessEgl();
        Debug::TruncateRoleLogs(server->logBase.c_str());
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) RunUnixInProcessServerProcess(server->endpoint, server->logBase);
        server->pid = pid;
        bool listening = false;
        (void)WaitFor(
            [&] {
                if (server->Log().find("listening on") != std::string::npos) return listening = true;
                return server->WaitExit(0) && server->pid <= 0;
            },
            10000);
        // The listener takes the native address; the client's control selector includes its
        // transport prefix (bare @name is not a supported MOBILEGL_IPC_CONTROL spelling).
        if (listening) server->endpoint = "unix:" + server->endpoint;
        return listening;
    }

    // ---- a peer this test drives STEP BY STEP, so a case can interleave two sessions: everything
    // the peer does is decided by the parent over a pipe, and every step answers with a report.

    // The two clear colours, packed the way the read-back hands them back (A<<24|B<<16|G<<8|R,
    // the little-endian bytes of a GL_RGBA/GL_UNSIGNED_BYTE texel).
    constexpr Uint32 kGreen = 0xFF00FF00u;
    constexpr Uint32 kRed = 0xFF0000FFu;

    enum PeerOp : Uint32 {
        kOpApplyRecord = 1, // one record through the applier (no GL)
        kOpCreateContext = 2, // ServerCreateEGLContext(arg) - the session's context token table
        kOpHealthySession = 3, // the rig's whole healthy session, `arg` = the clear colour
        kOpStop = 4,          // ClientSession::Stop, then the child leaves
        // P14 S5: mint context `arg` in share group `aux` (a real frontend share-group token, so
        // two contexts that pass different ones are a NON-shared pair and two that pass the same
        // one are a sharing pair).
        kOpCreateContextInGroup = 5,
        // P14 S5: the in-band binding record - the client's "from here on, my records belong to
        // context `arg`". EGLImpl::MakeCurrent emits exactly this after a successful make-current;
        // a case that drives records by hand has to emit it itself, or every record lands in the
        // session's "no context" applier.
        kOpBindContext = 6,
    };

    struct PeerCommand {
        Uint32 op = 0;
        Uint32 arg = 0;
        Uint32 aux = 0;
    };

    // The report of one step. `session` is the rig's own report for the steps that run a whole
    // session; the step-specific fields sit beside it.
    struct StepReport {
        Int32 started = 0;
        Uint32 serverPid = 0;
        Int32 context = -1;
        Int32 applied = 0;
        PeerReport session{};
        char note[128] = {};
    };

    void ApplyOneRecord(Remote::Client::ClientSession& client, StepReport& r) {
        P::MGPMemoryBarrier barrier{};
        barrier.Bits = 0x2000u;
        r.applied = Emit(client, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
    }

    struct Peer {
        pid_t pid = -1;
        int toChild = -1;
        int fromChild = -1;
        // The handshake's own report, read by StartPeer: a step's report must never be this one.
        StepReport ready{};

        StepReport Step(PeerOp op, Uint32 arg = 0, Uint32 aux = 0) {
            StepReport report{};
            PeerCommand command{op, arg, aux};
            if (::write(toChild, &command, sizeof(command)) != static_cast<ssize_t>(sizeof(command))) {
                std::snprintf(report.note, sizeof(report.note), "peer write failed");
                return report;
            }
            if (::read(fromChild, &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report))) {
                report = StepReport{};
                std::snprintf(report.note, sizeof(report.note), "peer died before reporting");
            }
            return report;
        }

        void Close() {
            if (pid <= 0) return;
            (void)Step(kOpStop);
            int status = 0;
            ::waitpid(pid, &status, 0);
            ::close(toChild);
            ::close(fromChild);
            toChild = -1;
            fromChild = -1;
            pid = -1;
        }

        ~Peer() {
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                int status = 0;
                (void)::waitpid(pid, &status, 0);
            }
            if (toChild >= 0) ::close(toChild);
            if (fromChild >= 0) ::close(fromChild);
        }
    };

    Peer StartPeer(const std::string& endpoint) {
        Peer peer;
        int commands[2] = {-1, -1};
        int reports[2] = {-1, -1};
        if (::pipe(commands) != 0 || ::pipe(reports) != 0) return peer;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(commands[1]);
            ::close(reports[0]);
            auto& client = Remote::Client::ClientSessionInstance();
            MobileGL::MG_Config::Ipc.Control = endpoint.c_str();
            MobileGL::MG_Config::Ipc.Data = "auto";
            const MobileGLResult started = client.StartSpawned();
            StepReport ready{};
            ready.started = started == MOBILEGL_OK ? 1 : 0;
            ready.serverPid = client.PeerServerPid();
            if (!ready.started) std::snprintf(ready.note, sizeof(ready.note), "start rc=%d", static_cast<int>(started));
            (void)::write(reports[1], &ready, sizeof(ready));
            for (;;) {
                PeerCommand command{};
                if (::read(commands[0], &command, sizeof(command)) != static_cast<ssize_t>(sizeof(command))) break;
                if (command.op == kOpStop) {
                    StepReport bye{};
                    bye.started = ready.started;
                    bye.serverPid = ready.serverPid;
                    if (ready.started) client.Stop();
                    (void)::write(reports[1], &bye, sizeof(bye));
                    break;
                }
                StepReport report{};
                report.started = ready.started;
                report.serverPid = ready.serverPid;
                switch (command.op) {
                case kOpApplyRecord:
                    ApplyOneRecord(client, report);
                    break;
                case kOpCreateContext:
                    report.context = Remote::Server::ServerCreateEGLContext(command.arg, /*shareGroupToken=*/0,
                                                                           /*flags=*/0)
                                         ? 1
                                         : 0;
                    break;
                case kOpCreateContextInGroup:
                    report.context = Remote::Server::ServerCreateEGLContext(command.arg, command.aux, /*flags=*/0)
                                         ? 1
                                         : 0;
                    break;
                case kOpBindContext: {
                    P::MGPBindContext bind{};
                    bind.ClientContextToken = command.arg;
                    report.applied = Emit(client, P::MGPWireOp::BindContext, &bind, sizeof(bind)) ? 1 : 0;
                    break;
                }
                case kOpHealthySession: {
                    PeerReport session{};
                    HealthySessionWithColor(client, session, command.arg);
                    report.session = session;
                    break;
                }
                default:
                    std::snprintf(report.note, sizeof(report.note), "unknown peer op %u", command.arg);
                    break;
                }
                (void)::write(reports[1], &report, sizeof(report));
            }
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(commands[0]);
        ::close(reports[1]);
        if (pid < 0) {
            ::close(commands[1]);
            ::close(reports[0]);
            return peer;
        }
        peer.pid = pid;
        peer.toChild = commands[1];
        peer.fromChild = reports[0];
        // The handshake's report, taken HERE so that no Step can read it and mistake it for its
        // own answer.
        if (::read(peer.fromChild, &peer.ready, sizeof(peer.ready)) !=
            static_cast<ssize_t>(sizeof(peer.ready))) {
            peer.ready = StepReport{};
            std::snprintf(peer.ready.note, sizeof(peer.ready.note), "peer died during its handshake");
        }
        return peer;
    }

    // The server's own line for the n-th session it started.
    std::string SessionStartedLine(const ServerProcess& server, unsigned long long ordinal) {
        const std::string log = server.Log();
        const std::string needle = "in-process unix session #" + std::to_string(ordinal) + " started";
        const auto at = log.find(needle);
        if (at == std::string::npos) return {};
        const auto end = log.find('\n', at);
        return log.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }

    // SIGPIPE would kill this test process the moment a peer it drives has already gone; every
    // write to a peer's pipe is guarded by the peer being alive, and this is the belt.
    class MultiSessionFixture : public ::testing::Test {
    protected:
        void SetUp() override { ::signal(SIGPIPE, SIG_IGN); }
    };

} // namespace

// THE HEADLINE. Two clients, one server process, both served at once.
TEST_F(MultiSessionFixture, TwoShmSessionsInOneProcessAreServedAtOnce) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixInProcessServer(&server, "two")) << "the in-process unix server did not start:\n" << server.Log();
    const auto serverPid = static_cast<Uint32>(server.pid);

    Peer a = StartPeer(server.endpoint);
    ASSERT_GT(a.pid, 0);
    ASSERT_EQ(a.ready.started, 1) << a.ready.note << "\n" << server.Log();
    EXPECT_EQ(a.ready.serverPid, serverPid) << "session A is not the in-process server's own process";
    const StepReport first = a.Step(kOpApplyRecord);
    ASSERT_EQ(first.applied, 1) << "session A applied no record: " << first.note;

    // WHILE A IS LIVE. Red once by restoring the one-at-a-time supervisor: this Hello is refused
    // Busy and `started` is 0.
    Peer b = StartPeer(server.endpoint);
    ASSERT_GT(b.pid, 0);
    ASSERT_EQ(b.ready.started, 1) << "a second session was refused while one was live (" << b.ready.note << ")\n"
                                  << server.Log();
    EXPECT_EQ(b.ready.serverPid, serverPid) << "the two sessions are not one process";
    const StepReport second = b.Step(kOpApplyRecord);
    EXPECT_EQ(second.applied, 1) << "session B applied no record: " << second.note;

    // Both sessions really were started, in this process, and neither latched.
    EXPECT_NE(SessionStartedLine(server, 1).find("pid=" + std::to_string(server.pid)), std::string::npos)
        << SessionStartedLine(server, 1) << "\n" << server.Log();
    EXPECT_NE(SessionStartedLine(server, 2).find("pid=" + std::to_string(server.pid)), std::string::npos)
        << SessionStartedLine(server, 2) << "\n" << server.Log();
    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 0u) << "a session latched a fault: " << server.Log();

    // THE RENDER HALF, one session at a time, BOTH STILL LIVE: each session's clear colour travels
    // in its own records and its read-back must be that colour. (The two sessions' native state is
    // P14b's; see this file's header for why the halves are sequential.)
    const StepReport aGl = a.Step(kOpHealthySession, kGreen);
    const StepReport bGl = b.Step(kOpHealthySession, kRed);
    if (aGl.session.egl == 0 || bGl.session.egl == 0) {
        GTEST_SKIP() << "concurrent sessions and their isolation proven; the render half needs headless EGL ("
                     << aGl.session.note << " / " << bGl.session.note << ")";
    }
    EXPECT_EQ(aGl.session.applied, 1) << aGl.session.note;
    EXPECT_EQ(aGl.session.status, Remote::Wire::ReplySink::kStatusOk) << aGl.session.note;
    EXPECT_EQ(aGl.session.pixel, kGreen) << "session A read back " << std::hex << aGl.session.pixel
                                         << ", not its own clear colour";
    EXPECT_EQ(bGl.session.applied, 1) << bGl.session.note;
    EXPECT_EQ(bGl.session.status, Remote::Wire::ReplySink::kStatusOk) << bGl.session.note;
    EXPECT_EQ(bGl.session.pixel, kRed) << "session B read back " << std::hex << bGl.session.pixel
                                       << ", not its own clear colour";

    b.Close();
    a.Close();
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
}

// A NAME IS A NAME IN A SESSION. Both sessions mint the same client context token and the same
// allocatable render state handle, and both are accepted.
TEST_F(MultiSessionFixture, SameNamesInTwoSessionsDoNotCollide) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixInProcessServer(&server, "names")) << server.Log();

    Peer a = StartPeer(server.endpoint);
    ASSERT_GT(a.pid, 0);
    ASSERT_EQ(a.ready.started, 1) << a.ready.note << "\n" << server.Log();
    const StepReport aStarted = a.Step(kOpCreateContext, 7);
    EXPECT_EQ(aStarted.context, 1) << "session A's own context token was refused";

    Peer b = StartPeer(server.endpoint);
    ASSERT_GT(b.pid, 0);
    ASSERT_EQ(b.ready.started, 1) << "a second session was refused (" << b.ready.note << ")\n" << server.Log();
    const StepReport bStarted = b.Step(kOpCreateContext, 7);
    // Red once by sharing the {token -> ContextRuntime} table: CreateContext(7) is already taken
    // and this answers ok=false - which the client's eglCreateContext turns into EGL_NO_CONTEXT.
    EXPECT_EQ(bStarted.context, 1) << "session B could not mint the token session A had already used";

    // And the same allocatable handle at the same generation: the rig's healthy session creates
    // the render state at {kMGPipeFirstAllocatableSlot, 1}, which is the same name in both.
    const StepReport noGlSession = b.Step(kOpApplyRecord);
    EXPECT_EQ(noGlSession.applied, 1) << "session B stopped applying after minting session A's names: "
                                      << noGlSession.note;
    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 0u) << "a session latched: " << server.Log();

    b.Close();
    a.Close();
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
}

// CLOSING ONE SESSION LEAVES THE OTHER SERVING. A ends; B keeps applying records, because the
// reverse-channel table and the segment resolver A's Close would have released are shared by the
// process and are still B's - and because the latch is B's own domain.
TEST_F(MultiSessionFixture, ClosingOneSessionLeavesTheOtherServing) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixInProcessServer(&server, "close")) << server.Log();

    Peer a = StartPeer(server.endpoint);
    ASSERT_GT(a.pid, 0);
    ASSERT_EQ(a.ready.started, 1) << a.ready.note << "\n" << server.Log();
    const StepReport aFirst = a.Step(kOpApplyRecord);
    ASSERT_EQ(aFirst.applied, 1) << "session A applied no record: " << aFirst.note;

    Peer b = StartPeer(server.endpoint);
    ASSERT_GT(b.pid, 0);
    ASSERT_EQ(b.ready.started, 1) << "session B was not admitted beside session A (" << b.ready.note << ")\n"
                                  << server.Log();
    const StepReport bFirst = b.Step(kOpApplyRecord);
    ASSERT_EQ(bFirst.applied, 1) << "session B applied no record: " << bFirst.note;

    a.Close();
    ASSERT_TRUE(WaitFor([&] { return server.Log().find("reaped exit=0") != std::string::npos; }, 5000))
        << "session A did not end cleanly:\n" << server.Log();
    EXPECT_EQ(::kill(static_cast<pid_t>(b.pid), 0), 0) << "closing A killed session B's peer";

    // B IS STILL SERVED: its records keep reaching its own applier and the server keeps answering.
    // Red once by releasing the process-wide resolver on session A's Close: this record's staged
    // bytes resolve to nothing and the session latches instead of applying.
    for (int record = 0; record < 3; ++record) {
        const StepReport after = b.Step(kOpApplyRecord);
        ASSERT_EQ(after.applied, 1) << "session B stopped applying after session A closed (record " << record
                                    << "): " << after.note << "\n" << server.Log();
    }
    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 0u) << "a session latched: " << server.Log();

    b.Close();
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
    EXPECT_TRUE(WaitFor([&] { return !SessionStartedLine(server, 2).empty(); }, 5000)) << server.Log();
}

// P14 S5 (docs/Disaggregated/design/11-state-ownership.md): TWO SESSIONS, TWO CONTEXTS EACH, MANY
// INTERLEAVED ROUNDS.
//
// WHAT THIS OWNS THAT THE APPLIER UNIT SUITE CANNOT. The keying is proved there against a
// resolver the test installs itself. Here the resolver is the PRODUCTION one
// (SessionRuntime.cpp's ThreadSessionScope, installed on the session's apply thread), the records
// come off a real ring through a real decoder, and the two sessions' apply threads run at the
// same time - so this is the case that would catch an applier that is per-process after all, or a
// resolution that reads state another thread is writing, or a context switch that leaks the
// previous context's working state into the records that follow it.
//
// EVERY ROUND: each peer switches its session's current context (the bind_context record the
// client emits on a real eglMakeCurrent) and then runs a whole clear-and-read-back through that
// context. The two peers alternate, so every round crosses a context switch AND a session.
//
// THE COLOUR IS THE WITNESS. Green and red are read back only through the applier the record was
// attributed to, so a session that read the other's working state would read the other's colour;
// a context that inherited its sibling's cleared inputs would fail the read-back's own
// completeness check. Red once by making MGPipeApplier() ignore the resolver: the two contexts of
// one session share one applier and one of the two clears lands on top of the other.
TEST_F(MultiSessionFixture, TwoSessionsInterleaveTwoContextsEachWithoutCrossTalk) {
    ServerProcess server;
    ASSERT_TRUE(LaunchUnixInProcessServer(&server, "interleave"))
        << "the in-process unix server did not start:\n" << server.Log();

    Peer a = StartPeer(server.endpoint);
    Peer b = StartPeer(server.endpoint);
    ASSERT_GT(a.pid, 0);
    ASSERT_GT(b.pid, 0);
    ASSERT_EQ(a.ready.started, 1) << a.ready.note << "\n" << server.Log();
    ASSERT_EQ(b.ready.started, 1) << "a second session was refused (" << b.ready.note << ")\n"
                                  << server.Log();

    // THE SAME TWO TOKENS AND THE SAME TWO GROUP TOKENS IN BOTH SESSIONS, on purpose: a name is a
    // name in a session, and the two group tokens say the two contexts of one session do NOT
    // share - which is the pair whose object records a process-wide applier merged.
    constexpr Uint32 kGroupOne = 9001;
    constexpr Uint32 kGroupTwo = 9002;
    for (Peer* peer : {&a, &b}) {
        ASSERT_EQ(peer->Step(kOpCreateContextInGroup, /*token=*/1, kGroupOne).context, 1);
        ASSERT_EQ(peer->Step(kOpCreateContextInGroup, /*token=*/2, kGroupTwo).context, 1);
    }

    bool reportedNoEgl = false;
    for (int round = 0; round < 8; ++round) {
        const Uint32 token = 1 + static_cast<Uint32>(round % 2);
        ASSERT_EQ(a.Step(kOpBindContext, token).applied, 1) << "session A could not bind context " << token;
        ASSERT_EQ(b.Step(kOpBindContext, token).applied, 1) << "session B could not bind context " << token;

        const StepReport aGl = a.Step(kOpHealthySession, kGreen);
        const StepReport bGl = b.Step(kOpHealthySession, kRed);
        if (aGl.session.egl == 0 || bGl.session.egl == 0) {
            reportedNoEgl = true;
            break;
        }
        ASSERT_EQ(aGl.session.status, Remote::Wire::ReplySink::kStatusOk)
            << "round " << round << ": " << aGl.session.note;
        ASSERT_EQ(bGl.session.status, Remote::Wire::ReplySink::kStatusOk)
            << "round " << round << ": " << bGl.session.note;
        EXPECT_EQ(aGl.session.pixel, kGreen) << "round " << round << ": session A read back "
                                             << std::hex << aGl.session.pixel;
        EXPECT_EQ(bGl.session.pixel, kRed) << "round " << round << ": session B read back "
                                           << std::hex << bGl.session.pixel;
    }
    if (reportedNoEgl) {
        GTEST_SKIP() << "the context/session routing is proven; the render half needs headless EGL ("
                     << a.ready.note << ")";
    }

    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 0u)
        << "a session latched during the interleave: " << server.Log();

    b.Close();
    a.Close();
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
}

// A LOST GPU DEVICE ENDS ITS OWN SESSION AND NO OTHER.
//
// The display server serves every client in one process, so a backend that met a device loss on
// one session's apply thread and died (Magma's wire funnels did: a failed submit took
// Fatal{UnmigratedVerb, "Magma:wire-verb-flush"} and SIGABRT) took the compositor and every other
// client down with it. A backend now hands a loss to MGPipeSessionLatch: that session latches
// Fatal{BackendDeviceLost}, declines what it was doing and closes (exit 75), and its neighbours
// keep applying. The loss is the debug knob's (MGPipeDebugDeviceLossDue) - the same device check
// the backends make at their readbacks and frame boundaries - because no GPU here faults on
// demand: the 2nd check in the process (session A's read-back, after B's) reports "lost".
//
// Red once by routing the latch seam through SessionFail (FatalFunnel.cpp's adapter): the whole
// server process aborts at A's read-back and session B's next record is never answered.
TEST_F(MultiSessionFixture, ADeviceLossEndsOnlyItsOwnSession) {
    ServerProcess server;
    ::setenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT", "2", 1);
    const bool launched = LaunchUnixInProcessServer(&server, "devlost");
    ::unsetenv("MOBILEGL_DEBUG_INJECT_DEVICE_LOST_AT");
    ASSERT_TRUE(launched) << "the in-process unix server did not start:\n" << server.Log();

    Peer a = StartPeer(server.endpoint);
    Peer b = StartPeer(server.endpoint);
    ASSERT_GT(a.pid, 0);
    ASSERT_GT(b.pid, 0);
    ASSERT_EQ(a.ready.started, 1) << a.ready.note << "\n" << server.Log();
    ASSERT_EQ(b.ready.started, 1) << b.ready.note << "\n" << server.Log();

    // Check #1: B's read-back is answered.
    const StepReport bFirst = b.Step(kOpHealthySession, kGreen);
    ASSERT_EQ(bFirst.session.egl, 1) << "no headless EGL for session B: " << bFirst.session.note << "\n" << server.Log();
    ASSERT_EQ(bFirst.session.pixel, kGreen) << bFirst.session.note << "\n" << server.Log();

    // Check #2: A's device is "lost" at its read-back. A's session latches and ends.
    const StepReport aLost = a.Step(kOpHealthySession, kRed);
    EXPECT_NE(aLost.session.pixel, kRed) << "session A read back a frame from a lost device";
    ASSERT_TRUE(WaitFor([&] { return server.Log().find("reaped exit=75") != std::string::npos; }, 5000))
        << "session A did not end on a latched fault:\n" << server.Log();
    const std::string log = server.Log();
    EXPECT_NE(log.find("Fatal{BackendDeviceLost, \"Espryt:read-pixels\"}"), std::string::npos) << log;
    EXPECT_NE(log.find("SessionLatch{BackendDeviceLost}"), std::string::npos) << log;
    EXPECT_TRUE(server.pid > 0 && ::kill(server.pid, 0) == 0) << "the server process died with session A";

    // B IS STILL SERVED, records and read-backs alike.
    for (int record = 0; record < 3; ++record) {
        const StepReport after = b.Step(kOpApplyRecord);
        ASSERT_EQ(after.applied, 1) << "session B stopped applying after session A's device loss (record "
                                    << record << "): " << after.note << "\n" << server.Log();
    }
    const StepReport bAgain = b.Step(kOpHealthySession, kGreen);
    EXPECT_EQ(bAgain.session.pixel, kGreen) << bAgain.session.note << "\n" << server.Log();
    EXPECT_EQ(Count(server.Log(), "SessionLatch{"), 1u) << "a session other than A latched:\n" << server.Log();

    b.Close();
    a.Close();
    server.Stop();
    EXPECT_TRUE(WIFEXITED(server.exitStatus) && WEXITSTATUS(server.exitStatus) == 0)
        << "the in-process server did not stop cleanly: status 0x" << std::hex << server.exitStatus;
}
