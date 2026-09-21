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
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/FdPassing.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
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

TEST(ServerSpawnTest, AnEglControlOpCrossesToTheOtherProcessAndAnswers) {
    // `cp`. All twelve Server* EGL forwarders funnel through
    // ServerLoop::RunSurfaceControlFrame, so this exercises the ONE seam that
    // sends them: encode -> control socket -> the server's pump ->
    // ServerApplyWireSurfaceOp -> its one-slot mailbox -> the apply thread ->
    // SurfaceReply -> back here.
    //
    // Under inproc this same call posts into a mailbox in THIS process. The
    // assertion that it is not doing that here is the server's pid in the
    // handshake plus the fact that ServerLoopInstance() in this process was
    // never started - a local post would find no apply thread and time out.
    Session session;
    ASSERT_TRUE(Bring("eglop", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    // eglInitialize's forwarder: the simplest op that has a real answer.
    EGLint major = -1;
    EGLint minor = -1;
    const bool ok = Server::ServerInitializeEGLDisplay(EGL_NO_DISPLAY, &major, &minor);

    // WHAT IS ASSERTED IS THE ROUND TRIP, not the EGL result. A headless CI
    // machine has no display to initialise, so `ok` is expected to be false -
    // and that is fine, because `cp` is done when the op reaches the other
    // process and an answer comes back.
    //
    // THE OUT-PARAMETERS ARE THE PROOF. ServerInitializeEGLDisplay writes them
    // "whenever the dispatch ran, success or not" (ServerLoop.cpp's own comment
    // at the forwarder), so they move off -1 if and only if a SurfaceReply came
    // back from the other process. A transport failure returns before the
    // write-back and leaves both at -1; that is the difference this asserts.
    EXPECT_NE(major, -1) << "no SurfaceReply came back: the op never reached the other process";
    EXPECT_NE(minor, -1) << "no SurfaceReply came back: the op never reached the other process";
    (void)ok;

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
}

TEST(ServerSpawnTest, AKilledServerLatchesDeviceLostFromTheHangupAndNotFromADeadline) {
    // `dl`, and exit gate S2: kill -9 the server mid-session and the client must find out FROM A
    // DESCRIPTOR. This is the red-once for CONTRACT-P6 D5c, whose whole content is that the
    // client's own bell CANNOT witness the death - it holds both ends of that socketpair, so the
    // fact has to come from somewhere else.
    //
    // WHAT MAKES THIS FALSIFIABLE IS THE CLOCK, not the boolean. A timeout would eventually
    // report "gone" too, and 5.4 forbids that: under P5e run-ahead a server one frame behind is
    // the INTENDED steady state, so anything armed by a deadline fires on a healthy session under
    // load. The bound below is therefore deliberately far under every wait in the system - the
    // verb barrier is 120000 ms and the control reply 5000 ms - so a pass here cannot be a
    // timeout wearing the right answer.
    Session session;
    ASSERT_TRUE(Bring("devicelost", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);
    EXPECT_FALSE(Client::ClientSession::DeviceLost())
        << "a healthy session must not read as a lost device";

    Transport::Doorbell* bell = Client::ClientSessionInstance().SelfDoorbellForTest();
    ASSERT_NE(bell, nullptr);
    EXPECT_FALSE(bell->PeerHungUp()) << "nothing has hung up yet";

    ASSERT_EQ(::kill(session.server.pid, SIGKILL), 0);

    // Park with a SHORT budget, repeatedly, until the hangup lands. Each Park is what a real
    // waiter does; the loop exists because the kill is asynchronous and the first poll may win
    // the race. 200 x 10 ms is two seconds of patience against waits measured in minutes.
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 200 && !bell->PeerHungUp(); ++attempt) {
        (void)bell->Park(10);
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start).count();

    EXPECT_TRUE(bell->PeerHungUp())
        << "the witness never reported the hangup; the bell is watching a descriptor this side "
           "also holds, which is the exact defect D5c names";
    EXPECT_TRUE(bell->Dead()) << "a peer that hung up is a dead bell";
    EXPECT_LT(elapsedMs, 3000)
        << "took " << elapsedMs << " ms: too slow to be a hangup and fast enough only for a "
           "descriptor, so this assertion is what separates the two mechanisms";

    // AND THE LATCH ITSELF. PeerHungUp is the cause; DeviceLost is the session-scoped
    // consequence, and it is what glGetGraphicsResetStatus reports.
    Client::ClientSessionInstance().LatchDeviceLost("the test killed the server");
    EXPECT_TRUE(Client::ClientSession::DeviceLost());

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    (void)Server::ReapServer(session.server, 5000, &exitCode);
}

TEST(ServerSpawnTest, AnOrderlyStopIsNotADeviceLoss) {
    // The session-level control: a clean shutdown must leave the latch disarmed, or every exit
    // would report a context reset and the status would mean nothing.
    //
    // NOTE WHAT THIS DOES *NOT* PROVE, because the first draft of this comment claimed it did
    // and the R-16 falsification caught the lie. Collapsing PeerHungUp() into Dead() leaves this
    // test green: Stop() kills the bell through CondVarDoorbell::Kill(), and under SPAWN the
    // self bell is a SocketDoorbell, which has no Kill at all - so Dead() is false here either
    // way. ADeadBellIsNotAlwaysAHungUpPeer below is the case that separates them.
    Session session;
    ASSERT_TRUE(Bring("orderly", &session));
    ASSERT_EQ(Handshake(session), MOBILEGL_OK);

    Transport::Doorbell* bell = Client::ClientSessionInstance().SelfDoorbellForTest();
    ASSERT_NE(bell, nullptr);

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "the server must have exited cleanly, not been killed";

    EXPECT_FALSE(bell->PeerHungUp())
        << "an orderly Stop() must not look like a peer that died";
    EXPECT_FALSE(Client::ClientSession::DeviceLost())
        << "a clean teardown armed the device-lost latch; every exit would report a reset";
}

TEST(ServerSpawnTest, ADeadBellIsNotAlwaysAHungUpPeer) {
    // THE ASSERTION THAT MAKES `dl`'s CAUSE/FACT SPLIT LOAD-BEARING, and it exists because the
    // session-level control above turned out not to. Park() latches m_dead for several reasons
    // that are NOT a peer death - POLLERR, POLLNVAL, an unrecognised revents - and each of them
    // is a fault in THIS process's descriptor. A latch armed from Dead() would report a lost
    // GPU for a bug in our own fd handling.
    //
    // Falsified by construction: make PeerHungUp() return Dead() and this goes red, which is
    // exactly what the contract's D5c distinction has to be able to do.
    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

    // ownsFds=false: the bell must not close what this test is about to invalidate underneath
    // it, or the destructor closes a descriptor number somebody else has since been given.
    Transport::SocketDoorbell bell(pair[0], pair[1], /*code=*/1, /*ownsFds=*/false);
    bell.SetDeathWitness(-1); // no witness: nothing here can legitimately report a hangup
    EXPECT_FALSE(bell.Dead());
    EXPECT_FALSE(bell.PeerHungUp());

    // Close the park end UNDER the bell. poll() then answers POLLNVAL, which Park treats as a
    // descriptor it must never poll again - dead, but not bereaved.
    ASSERT_EQ(::close(pair[0]), 0);
    (void)bell.Park(50);

    EXPECT_TRUE(bell.Dead()) << "a bell polling a closed fd cannot be anything but dead";
    EXPECT_FALSE(bell.PeerHungUp())
        << "our own descriptor went bad and the bell called it a peer death; a device-lost latch "
           "taken from this would report a lost GPU for a local fd bug";

    ::close(pair[1]);
}

TEST(ServerSpawnTest, AnAbstractEndpointNeedsNoWritableDirectoryAndLeavesNoFile) {
    // THE ONLY RENDEZVOUS THAT WORKS ON ANDROID, and the reason is not a preference.
    // An app has no writable /tmp; the first device run of the spawn retrace launched
    // its server, bind() had nowhere to put the node, and the client refused by name
    // 20 s later. ClientSession::StartSpawned mints '@' names now, so this pins the
    // shape that path depends on.
    //
    // TWO FACTS, and the second is what a filesystem endpoint cannot give: the
    // handshake works over it, and NOTHING IS LEFT BEHIND - no node to go stale, no
    // directory to be writable, no unlink to forget after a crash.
    const std::string endpoint = std::string("@mgl-abstract-") + std::to_string(::getpid());

    Session session;
    ASSERT_EQ(Server::LaunchServer(ServerImage(), endpoint, &session.server), MOBILEGL_OK);
    ASSERT_EQ(Transport::SocketTransport::ConnectTo(endpoint, 10000, session.client), MOBILEGL_OK);
    ASSERT_EQ(Handshake(session), MOBILEGL_OK)
        << "an abstract endpoint carried the connection but not the session";

    // A leading '@' is a NAME, not a path. If FillAddress had treated it as one, the
    // bind would have created a file literally called "@mgl-..." in the working
    // directory - which would still have worked here, and would have failed on the
    // one platform this exists for.
    EXPECT_NE(::access(endpoint.c_str(), F_OK), 0)
        << "an abstract name must leave no filesystem node: " << endpoint;

    Client::ClientSessionInstance().Stop();
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(session.server, 5000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0);
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
