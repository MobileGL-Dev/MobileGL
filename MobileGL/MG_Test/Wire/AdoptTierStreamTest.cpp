// MobileGL - MobileGL/MG_Test/Wire/AdoptTierStreamTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P11 A1 (MG_Remote/CONTRACT-P11.md §1): MOBILEGL_IPC_ADOPT_TIER=0/1 ON A STREAM DATA PLANE.
//
// Real servers on loopback TCP and forked peers (P12ServerRig.h), so the session is the product's
// two-process tcp session and each side reads its OWN knob, as on the tcp lane:
//   * a client that names T0 or T1 against a `--serve` supervisor comes up, says
//     `Refuse{AdoptTierOnStream, "T<n>"}` exactly once, gets DECLINED for its map_persistent and
//     goes on applying records;
//   * an in-process display server (the render-server Activity's shape) that names T0 says the
//     same line once per session, never dies, and serves T2.
// The shared-segment half (a client naming T0/T1 dies at the handshake, before any map_persistent)
// is RemoteClientControls.AdoptTier*OverSharedSegmentsDiesAtTheHandshakeBeforeAnyMapPersistent.
//
// RED-ONCE: delete the stream branch of Transport::SettleAdoptTierAtHandshake -> the client peers
// die at the handshake (started 0, Fatal{UnimplementedAdoptTier}) and the server case finds no
// refusal line.

#include "P12ServerRig.h"

#include <MG_Remote/Server/InProcessServer.h>
#include <MG_Remote/Wire/PipeWireCodec.h>

#include <atomic>
#include <csignal>
#include <pthread.h>

using namespace MobileGL;
using namespace P12;

namespace {

    // A SERVER WHOSE OWN KNOB SAYS T0. Not the exec'd `--serve` supervisor: ServerSpawn scrubs
    // every MOBILEGL_IPC_* but five server-owned knobs from a child it launches, so a knob set here
    // would never reach it. The in-process display server (InProcessServerTest's shape - the device
    // render-server Activity FCL reaches over tcp) is a process forked from this one, sets the
    // knob in its own environment, and RunSession re-reads the environment for every session.
    [[noreturn]] void RunInProcessServerWithAdoptTier(const std::string& endpoint, const std::string& logBase,
                                                      const char* adoptTier) {
        ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
        ::setenv("MOBILEGL_IPC_DIAL", "no", 1);
        ::setenv("MOBILEGL_IPC_ADOPT_TIER", adoptTier, 1);
        for (const char* name : {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB",
                                 "MOBILEGL_IPC_STAGE_MB", "MOBILEGL_IPC_CONTROL", "MOBILEGL_IPC_SURFACE",
                                 "MOBILEGL_BACKEND_TYPE"})
            ::unsetenv(name);
        sigset_t stop;
        sigemptyset(&stop);
        sigaddset(&stop, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stop, nullptr);
        std::atomic<bool> returned{false};
        std::thread server([&] {
            (void)mobilegl_server_serve_inprocess(endpoint.c_str());
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
        ::_exit(0);
    }

    // THE SERVER GETS A LOG BASE OF ITS OWN (`server->logBase`), the peers another (`peerBase`, which
    // this process exports for the forks): a peer writes the lines the server forwards into
    // `<its base>.server.log`, opened "w", so sharing one base truncates the server's own file under
    // it (measured: NUL holes and the handshake's lines gone).
    bool LaunchInProcessServerWithAdoptTier(const std::string& label, const char* adoptTier, const std::string& peerBase,
                                            ServerProcess* server) {
        server->logBase = "/tmp/mgl-p11-" + label + "-server-" + std::to_string(::getpid()) + ".log";
        ::setenv("MOBILEGL_LOG_FILE_PATH", peerBase.c_str(), 1);
        Debug::TruncateRoleLogs(peerBase.c_str());
        PinHeadlessEgl();
        for (int attempt = 0; attempt < 5; ++attempt) {
            const int port = FreeLoopbackPort();
            if (port <= 0) return false;
            server->endpoint = "tcp://127.0.0.1:" + std::to_string(port);
            Debug::TruncateRoleLogs(server->logBase.c_str());
            std::fflush(nullptr);
            const pid_t pid = ::fork();
            if (pid < 0) return false;
            if (pid == 0) RunInProcessServerWithAdoptTier(server->endpoint, server->logBase, adoptTier);
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

    // The client's own role log for this rig's base (the peers inherit it from LaunchSupervisor).
    std::string ClientLog(const ServerProcess& server) {
        return ReadFile(Debug::RoleLogPath(server.logBase.c_str(), Debug::LogRole::Client));
    }

    std::string RefusalLine(Uint32 tier) {
        return "Refuse{AdoptTierOnStream, \"T" + std::to_string(tier) + "\"}";
    }

    // One map_persistent, then one ordinary record: the answer to the first and proof the session
    // is still applying after it.
    void MapPersistentThenARecord(Remote::Client::ClientSession& client, PeerReport& r) {
        P::MGPHandleOnly handle{};
        handle.Kind = static_cast<Uint32>(P::MGPipeKind::Buffer);
        Int32 status = Remote::Wire::ReplySink::kStatusError;
        const Uint64 seq =
            client.EmitAndWait(P::MGPWireOp::MapPersistent, &handle, sizeof(handle), nullptr, 0, nullptr, 0, &status);
        r.status = seq == Remote::Wire::kInvalidSeq ? -98 : status;
        P::MGPMemoryBarrier barrier{};
        barrier.Bits = 0x2000u;
        r.applied = Emit(client, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
    }

} // namespace

TEST(AdoptTierStream, AClientNamingT0OrT1IsRefusedOnceAndItsMapPersistentIsDeclined) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("adopt-client", &server)) << server.Log();
    for (Uint32 tier : {0u, 1u}) {
        SCOPED_TRACE(tier);
        const PeerReport r = RunPeer(server.endpoint, MapPersistentThenARecord, /*stopCleanly=*/true, 30000, 0,
                                     nullptr, [tier] { MG_Config::Ipc.AdoptTier = tier; });
        const std::string client = ClientLog(server);
        ASSERT_EQ(r.started, 1) << "the session did not come up: " << r.note << "\n" << client;
        EXPECT_EQ(r.status, Remote::Wire::ReplySink::kStatusDeclined)
            << "map_persistent was not DECLINED on a stream session\n" << client;
        EXPECT_EQ(r.applied, 1) << "the session stopped applying after the map_persistent\n" << client;
        EXPECT_EQ(Count(client, RefusalLine(tier)), 1u)
            << "a stream session names T" << tier << " exactly once, at the handshake\n" << client;
        EXPECT_EQ(Count(client, "Fatal{"), 0u) << client;
        EXPECT_EQ(Count(server.Log(), "AdoptTierOnStream"), 0u)
            << "the server's own knob is 2; the line is the client's\n" << server.Log();
    }
}

// P11 B2: THE KNOB UNSET - T0 BY DEFAULT since 2026-09-29 - ON A STREAM. The Hello asks 2 (a stream
// carries T2 only), the session runs T2, and nothing says so above MGLOG_D: every tcp session would
// otherwise carry A1's line. The client still COUNTS the fallback (ClientSession::T0Fallbacks,
// reported through PeerReport::pixel). The supervisor's own knob is unset as well (ServerSpawn
// scrubs it), so its stream settle is quiet too.
// RED-ONCE: log the unset stream settle at W instead of D -> one AdoptTierOnStream line, not 0.
TEST(AdoptTierStream, AnUnsetKnobRunsT2OnAStreamQuietlyAndCountsTheFallback) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("adopt-unset", &server)) << server.Log();
    const auto body = [](Remote::Client::ClientSession& client, PeerReport& r) {
        MapPersistentThenARecord(client, r);
        r.pixel = static_cast<Uint32>(client.T0Fallbacks());
    };
    const PeerReport r = RunPeer(server.endpoint, body, /*stopCleanly=*/true, 30000, 0, nullptr,
                                 [] { MG_Config::Ipc.AdoptTier = MG_Config::kAdoptTierUnset; });
    const std::string client = ClientLog(server);
    ASSERT_EQ(r.started, 1) << "the session did not come up: " << r.note << "\n" << client;
    EXPECT_EQ(r.status, Remote::Wire::ReplySink::kStatusDeclined) << "a stream session runs T2\n" << client;
    EXPECT_EQ(r.applied, 1) << client;
    EXPECT_EQ(r.pixel, 1u) << "the stream's T0 fallback was not counted\n" << client;
    for (const char* line : {"AdoptTierOnStream", "T0 by default", "AdoptT0Unavailable", "T0 session totals"}) {
        EXPECT_EQ(Count(client, line), 0u) << "an unset knob's fallback is MGLOG_D only: " << line << "\n" << client;
        EXPECT_EQ(Count(server.Log(), line), 0u) << line << "\n" << server.Log();
    }
    EXPECT_EQ(Count(client, "Fatal{"), 0u) << client;
}

TEST(AdoptTierStream, AServerNamingT0RefusesOncePerSessionAndServesT2) {
    ServerProcess server;
    const std::string peerBase = "/tmp/mgl-p11-adopt-server-peers-" + std::to_string(::getpid()) + ".log";
    ASSERT_TRUE(LaunchInProcessServerWithAdoptTier("adopt-server", "0", peerBase, &server)) << server.Log();
    // Both role files of the server's own base: which role a session thread of the in-process
    // server logs under is not what this case is about.
    const auto serverLog = [&server] { return Debug::ReadRoleLogs(server.logBase.c_str()); };
    for (Uint32 session = 1; session <= 2; ++session) {
        SCOPED_TRACE(session);
        const PeerReport r = RunPeer(server.endpoint, MapPersistentThenARecord, /*stopCleanly=*/true, 30000, 0,
                                     nullptr, [] { MG_Config::Ipc.AdoptTier = 2u; });
        const std::string client = ReadFile(Debug::RoleLogPath(peerBase.c_str(), Debug::LogRole::Client));
        ASSERT_EQ(r.started, 1) << r.note << "\n" << serverLog();
        EXPECT_EQ(r.status, Remote::Wire::ReplySink::kStatusDeclined) << serverLog();
        EXPECT_EQ(r.applied, 1) << serverLog();
        EXPECT_EQ(Count(client, "AdoptTierOnStream"), 0u) << "the client's knob is 2\n" << client;
        EXPECT_EQ(Count(serverLog(), RefusalLine(0)), session)
            << "one refusal line per server session, at its handshake\n" << serverLog();
    }
    EXPECT_EQ(Count(serverLog(), "Fatal{UnimplementedAdoptTier"), 0u) << serverLog();
    if (!::testing::Test::HasFailure()) {
        std::error_code ec;
        std::filesystem::remove(Debug::RoleLogPath(peerBase.c_str(), Debug::LogRole::Client), ec);
        std::filesystem::remove(Debug::RoleLogPath(peerBase.c_str(), Debug::LogRole::Server), ec);
    }
}
