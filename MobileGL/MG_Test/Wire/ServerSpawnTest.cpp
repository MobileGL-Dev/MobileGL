// MobileGL - MobileGL/MG_Test/Wire/ServerSpawnTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Two MobileGL processes, started INDEPENDENTLY and joined by a name.
// Package `sm`.
//
// THE SHAPE IS THE POINT. The server is launched with an endpoint and nothing
// else - no inherited descriptor, no shared memory, no handshake from its
// parent - and the client finds it by connecting to that name. Everything these
// cases prove therefore transfers unchanged to a server somebody started by
// hand, or that an Android Service started minutes earlier, which is the only
// arrangement the end state can use.
//
// Each assertion exists because something would otherwise pass silently:
//
//   the image links          - a6 found mobilegl_server_main existed NOWHERE in
//                              the tree, only in ARCHITECTURE.md:488-496. A
//                              target that failed to link would make every case
//                              below read as "did not run".
//   the bytes crossed a      - the echo carries the SERVER's own getpid(), so a
//   PROCESS                    test that accidentally talked to itself cannot
//                              pass. ID-124's lesson at the smallest scale.
//   the process tree is clean- §9.4: exactly one child while running, zero
//                              after. Counted, not promised.
//   the server dies on EOF   - and on EOF ONLY. §5.4 forbids a timeout from ever
//                              standing in for the death fact, so the client
//                              closes and the server must go by itself.
//   SCM_RIGHTS works across   - the mechanism the four segments ride on, proven
//   the boundary               before the data plane needs it.

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;

#if !defined(_WIN32)

namespace {

    // A private rendezvous per case. Independent processes need a NAME, and a
    // per-case one is what stops two runs colliding the way a default endpoint
    // would.
    std::string Endpoint(const char* label) {
        return std::string("/tmp/mgl-sm-") + label + "-" + std::to_string(::getpid()) + ".sock";
    }

    std::string ServerImage() {
        if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) {
            return explicitPath;
        }
        return "libMobileGLServer.so";
    }

    struct Session {
        Server::LaunchedServer server;
        std::unique_ptr<Transport::SocketTransport> client;
    };

    // The two halves a real deployment does separately: start a process, then
    // connect to it. The bounded connect retry is the CLIENT's, not the
    // launcher's - a launcher that waited for the server to be ready would be
    // coupling the two processes again by the back door.
    bool Bring(const char* label, Session* out) {
        const std::string endpoint = Endpoint(label);
        if (Server::LaunchServer(ServerImage(), endpoint, &out->server) != MOBILEGL_OK) {
            return false;
        }
        return Transport::SocketTransport::ConnectTo(endpoint, 10000, out->client) == MOBILEGL_OK;
    }

    // Closing the connection is what the server sees as EOF, and EOF is the whole
    // exit condition. The launcher holds nothing to close.
    void CloseAndReap(Session& session, int* outExitCode) {
        if (session.client) {
            session.client->Shutdown();
        }
        ASSERT_EQ(Server::ReapServer(session.server, 5000, outExitCode), MOBILEGL_OK);
    }

    // The REAL handshake, not an echo. ServerMain runs ServerSession::Accept,
    // which wants a Hello and answers Welcome plus six SCM_RIGHTS offers - so a
    // test that spoke anything else would only ever prove the socket works.
    // Driving ClientSession is what proves the SEGMENTS crossed.
    MobileGLResult Handshake(Session& session) {
        return Client::ClientSessionInstance().StartOverSocket(std::move(session.client));
    }

    std::string Exchange(Transport::ITransport& transport, const std::string& message,
                         std::uint32_t timeoutMs = 4000) {
        const MobileGLResult sent =
            transport.SendFrame(MobileGLByteSpan{message.data(), message.size()});
        if (sent != MOBILEGL_OK) {
            return "<send-failed>";
        }
        std::vector<std::uint8_t> buffer(64 * 1024);
        std::uint64_t size = 0;
        MobileGLMutableByteSpan span{buffer.data(), buffer.size()};
        const MobileGLResult got = transport.ReceiveFrame(span, &size, timeoutMs);
        if (got != MOBILEGL_OK) {
            return std::string("<result=") + std::to_string(static_cast<int>(got)) + ">";
        }
        return std::string(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::size_t>(size));
    }

} // namespace

TEST(ServerSpawnTest, StartsAServerProcessAndHandshakesAcrossIt) {
    const int before = Server::CountOwnChildren();
    ASSERT_GE(before, 0) << "/proc unreadable; the process-tree gate cannot run here";

    Session session;
    ASSERT_TRUE(Bring("handshake", &session));
    ASSERT_GT(session.server.pid, 0);
    EXPECT_NE(session.server.pid, static_cast<int>(::getpid()))
        << "that is this process, not a separate one";
    EXPECT_EQ(Server::CountOwnChildren(), before + 1);

    // THE WHOLE POINT. Hello crosses, Welcome comes back with four SegmentRefs,
    // six descriptors arrive over SCM_RIGHTS, and this process maps memory that
    // another process created. Nothing here was inherited.
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    EXPECT_TRUE(Client::ClientSession::Active() != nullptr)
        << "a handshake that returned OK must leave the session emit-armed";

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "the server must exit cleanly on EOF, not be killed";
    EXPECT_EQ(Server::CountOwnChildren(), before) << "a zombie would still be counted here";
}

TEST(ServerSpawnTest, AnUnresolvableImageIsANamedRefusalAndStartsNothing) {
    const int before = Server::CountOwnChildren();

    // S1 (CONTRACT-P6 §9). The accident this prevents is the one
    // ConfigLoader.cpp names in its own comment: a lane that asked for spawn,
    // silently got monolith, and went green on the wrong arm.
    Server::LaunchedServer server;
    EXPECT_EQ(Server::LaunchServer("/nonexistent/libMobileGLServer.so", Endpoint("missing"),
                                   &server),
              MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(server.pid, -1);
    EXPECT_EQ(Server::CountOwnChildren(), before) << "a refused launch must leave no process";
}

TEST(ServerSpawnTest, ConnectingToNothingIsANamedRefusalNotAHang) {
    // The other half of S1: the client's side. A rendezvous nobody is listening
    // on must be a bounded, named failure - never an unbounded wait, which is
    // what a caller would experience as the whole application freezing.
    std::unique_ptr<Transport::SocketTransport> client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NE(Transport::SocketTransport::ConnectTo(Endpoint("nobody"), 300, client), MOBILEGL_OK);
    EXPECT_FALSE(client);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count(),
              4000);
}

TEST(ServerSpawnTest, ADescriptorCrossesBetweenTheTwoProcesses) {
    // The mechanism the four shared segments ride on. Proving it now, on a pipe,
    // is what stops it being first exercised on the day the data plane needs it -
    // the same argument SessionRings.h makes about the attach half.
    //
    // NOTE it crosses the AUX connection, which is the second of the two the
    // client made. An fd offer is a sendmsg whose ancillary data rides with
    // specific bytes, so it may not share a socket with the framed control
    // stream: the frame reassembler and the descriptor receiver would race for
    // the same bytes.
    Session session;
    ASSERT_TRUE(Bring("scm", &session));

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);
    const std::string sideband = "SegmentRef{id=1,kind=Cmd}";
    EXPECT_EQ(session.client->ShareFd(pipeFds[0],
                                      MobileGLByteSpan{sideband.data(), sideband.size()}),
              MOBILEGL_OK);

    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    int exitCode = -1;
    CloseAndReap(session, &exitCode);
}

#endif // !_WIN32
