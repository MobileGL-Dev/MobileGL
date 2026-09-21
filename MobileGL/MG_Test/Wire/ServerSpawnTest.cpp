// MobileGL - MobileGL/MG_Test/Wire/ServerSpawnTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// Two MobileGL processes, for the first time. Package `sm`.
//
// What these cases pin is the scaffold every later P6 package stands on, and
// each assertion exists because something specific would otherwise pass
// silently:
//
//   the image links           - a6 found mobilegl_server_main existed NOWHERE in
//                               the tree, only in ARCHITECTURE.md:488-496. If
//                               the target failed to link, everything below is
//                               unreachable and the failure would read as "test
//                               did not run".
//   the fds survive execve    - FdPassing::CreateSocketPair sets SOCK_CLOEXEC,
//                               so the aux socket WOULD vanish. dup2 onto fixed
//                               numbers is what clears it; if that were ever
//                               changed to a plain assignment the child would
//                               come up with fd 4 closed and the symptom would
//                               be a hang, not an error.
//   the bytes crossed a PROCESS - the echo carries the child's own getpid(),
//                               so a test that accidentally talked to itself
//                               cannot pass. This is ID-124's lesson applied to
//                               the smallest possible case.
//   the process tree is clean - CONTRACT-P6 §9.4: exactly one child while
//                               running, zero after. Counted, not promised.
//   the child dies on EOF     - and on EOF ONLY. §5.4 forbids a timeout from
//                               ever standing in for the death fact, so the
//                               parent closes and the child must go by itself.

#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/Doorbell.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;

#if !defined(_WIN32)

namespace {

    // The image lives beside the test binary's build root. The production
    // resolver (MOBILEGL_IPC_SERVER_PATH, then dladdr) is exercised separately
    // below; here we want a path we control so a resolution bug cannot look
    // like a spawn bug.
    std::string ServerImage() {
        if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) {
            return explicitPath;
        }
        return "libMobileGLServer.so";
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

TEST(ServerSpawnTest, SpawnsAServerProcessAndTalksToIt) {
    const int before = Server::CountOwnChildren();
    ASSERT_GE(before, 0) << "/proc unreadable; the process-tree gate cannot run here";

    Server::SpawnedServer server;
    ASSERT_EQ(Server::SpawnServer(ServerImage(), &server), MOBILEGL_OK);
    ASSERT_GT(server.pid, 0);
    ASSERT_TRUE(server.transport);
    EXPECT_NE(server.pid, static_cast<int>(::getpid())) << "that is this process, not a child";

    // Exactly one more child while it runs - §9.4, counted.
    EXPECT_EQ(Server::CountOwnChildren(), before + 1);

    // The reply carries the CHILD's pid, computed in the child. A test that
    // accidentally held both ends of one socket cannot produce this string.
    const std::string reply = Exchange(*server.transport, "Hello{abiMajor=1}");
    EXPECT_EQ(reply, "Hello{abiMajor=1}|server-pid=" + std::to_string(server.pid));

    int exitCode = -1;
    EXPECT_EQ(Server::ReapServer(server, 4000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "the child must exit cleanly on EOF, not be killed";
    EXPECT_EQ(server.pid, -1);

    // Nothing left behind. A zombie would still be counted here.
    EXPECT_EQ(Server::CountOwnChildren(), before);
}

TEST(ServerSpawnTest, TheChildExitsOnEofAndOnlyOnEof) {
    Server::SpawnedServer server;
    ASSERT_EQ(Server::SpawnServer(ServerImage(), &server), MOBILEGL_OK);

    // It must NOT exit just because it is idle: §5.4 forbids a timeout from
    // standing in for the death fact, and a server that wandered off on its own
    // would make "the peer is gone" unfalsifiable.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(Server::CountOwnChildren(), 1) << "the child exited while merely idle";
    EXPECT_EQ(Exchange(*server.transport, "still-there"),
              "still-there|server-pid=" + std::to_string(server.pid));

    // Now close. EOF is the whole signal.
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(server, 4000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0);
}

TEST(ServerSpawnTest, ManyMessagesCrossTheProcessBoundaryInOrder) {
    Server::SpawnedServer server;
    ASSERT_EQ(Server::SpawnServer(ServerImage(), &server), MOBILEGL_OK);

    // Order across a real process boundary, with the reassembler on both sides.
    for (int i = 0; i < 128; ++i) {
        const std::string message = "msg-" + std::to_string(i);
        EXPECT_EQ(Exchange(*server.transport, message),
                  message + "|server-pid=" + std::to_string(server.pid));
    }

    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(server, 4000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0);
}

TEST(ServerSpawnTest, AnUnresolvableImageIsANamedRefusalAndSpawnsNothing) {
    const int before = Server::CountOwnChildren();

    // S1 (CONTRACT-P6 §9). The accident this prevents is the one
    // ConfigLoader.cpp names in its own comment: a lane that asked for spawn,
    // silently got monolith, and went green on the wrong arm.
    Server::SpawnedServer server;
    EXPECT_EQ(Server::SpawnServer("/nonexistent/libMobileGLServer.so", &server),
              MOBILEGL_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(server.pid, -1);
    EXPECT_FALSE(server.transport) << "a refused spawn must not hand back a usable transport";
    EXPECT_EQ(Server::CountOwnChildren(), before) << "a refused spawn must leave no process";
}

TEST(ServerSpawnTest, TheChildsEnvironmentIsScrubbedOfEveryClientKnob) {
    // Catch (b). The child refuses to start with exit 65 if any MOBILEGL_IPC_*
    // or MOBILEGL_TRANSPORT survived into it, so setting one here and getting a
    // clean start back is the assertion. S3 falsifies it by removing the scrub.
    ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
    ::setenv("MOBILEGL_IPC_RING_MB", "64", 1);

    Server::SpawnedServer server;
    ASSERT_EQ(Server::SpawnServer(ServerImage(), &server), MOBILEGL_OK);
    EXPECT_EQ(Exchange(*server.transport, "scrubbed"),
              "scrubbed|server-pid=" + std::to_string(server.pid))
        << "the child refused to start, which means a client knob survived the scrub";

    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(server, 4000, &exitCode), MOBILEGL_OK);
    EXPECT_EQ(exitCode, 0) << "exit 64 = the dial catch fired; exit 65 = the scrub leaked";

    ::unsetenv("MOBILEGL_TRANSPORT");
    ::unsetenv("MOBILEGL_IPC_RING_MB");
}

TEST(ServerSpawnTest, ADescriptorCrossesToTheChildProcess) {
    // The mechanism the four shared segments will ride on. Proving it now, on
    // a pipe, is what stops it being first exercised on the day the data plane
    // needs it - the same argument SessionRings.h makes about the attach half.
    Server::SpawnedServer server;
    ASSERT_EQ(Server::SpawnServer(ServerImage(), &server), MOBILEGL_OK);

    int pipeFds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipeFds), 0);
    const std::string sideband = "SegmentRef{id=1,kind=Cmd}";
    EXPECT_EQ(server.transport->ShareFd(pipeFds[0],
                                        MobileGLByteSpan{sideband.data(), sideband.size()}),
              MOBILEGL_OK)
        << "SCM_RIGHTS across execve: if the aux fd lost its number or kept "
           "close-on-exec, this is where it shows";

    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    int exitCode = -1;
    ASSERT_EQ(Server::ReapServer(server, 4000, &exitCode), MOBILEGL_OK);
}

#endif // !_WIN32
