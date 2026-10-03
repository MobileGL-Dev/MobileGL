// MobileGL - MobileGL/MG_Test/Util/X11DisplayGuardTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The X-connection safety rule (MG_Util/X11/DisplayGuard.h): MobileGL never opens a connection to a
// display its own process serves. The last case is the deadlock itself: a process that listens on
// an X display's socket, as Xwayland does, creates an EGL display with DISPLAY naming that socket.
// Before the guard EGLState dialed it to pick a visual and waited forever for a setup reply only
// the blocked process could send.

#include <MG_State/EGLState/Core.h>
#include <MG_Util/X11/DisplayGuard.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace MobileGL;
namespace X = MobileGL::MG_Util::X11;

namespace {
    // /proc/net/unix as Xwayland's process sees it under KWin: the two listening sockets KWin bound
    // and handed over with -listenfd, plus connected ones.
    const char* kProcNetUnix =
        "Num       RefCount Protocol Flags    Type St Inode Path\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 41001 @/tmp/.X11-unix/X0\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 41002 /tmp/.X11-unix/X0\n"
        "0000000000000000: 00000003 00000000 00000000 0001 03 41003 /tmp/.X11-unix/X0\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 41004 /run/user/1000/wayland-0\n"
        "0000000000000000: 00000003 00000000 00000000 0001 03 41005\n"
        "0000000000000000: 00000002 00000000 00010000 0001 01 41006 @/tmp/.X11-unix/X1\n";
} // namespace

TEST(X11DisplayGuard, DisplayNamesYieldTheirNumber) {
    EXPECT_EQ(X::ParseDisplayNumber(":0"), 0);
    EXPECT_EQ(X::ParseDisplayNumber(":1.0"), 1);
    EXPECT_EQ(X::ParseDisplayNumber("unix:2"), 2);
    EXPECT_EQ(X::ParseDisplayNumber("localhost:10.2"), 10);
    EXPECT_EQ(X::ParseDisplayNumber("[::1]:3"), 3);
    EXPECT_EQ(X::ParseDisplayNumber("/tmp/.X11-unix/X4"), 4);
    EXPECT_EQ(X::ParseDisplayNumber("@/tmp/.X11-unix/X5"), 5);
    EXPECT_EQ(X::ParseDisplayNumber(""), -1);
    EXPECT_EQ(X::ParseDisplayNumber(nullptr), -1);
    EXPECT_EQ(X::ParseDisplayNumber("wayland-0"), -1);
    EXPECT_EQ(X::ParseDisplayNumber(":x"), -1);
}

TEST(X11DisplayGuard, OnlyListeningSocketsOfThatDisplayCount) {
    const Vector<Uint64> zero = X::ListeningDisplaySocketInodes(kProcNetUnix, 0);
    ASSERT_EQ(zero.size(), 2u);
    EXPECT_EQ(zero[0], 41001u); // abstract
    EXPECT_EQ(zero[1], 41002u); // path; 41003 is a connection to it, not a listener
    EXPECT_EQ(X::ListeningDisplaySocketInodes(kProcNetUnix, 1), Vector<Uint64>{41006u});
    EXPECT_TRUE(X::ListeningDisplaySocketInodes(kProcNetUnix, 7).empty());
}

TEST(X11DisplayGuard, AnXServerNeverDialsItsOwnDisplay) {
    // Xwayland holding display 0's listening socket.
    X::ProcessFacts xwayland{"Xwayland", kProcNetUnix, {7, 41002, 9}};
    EXPECT_FALSE(X::MayDial(":0", xwayland, nullptr));
    // By executable alone (a socket held through a path /proc does not show): still refused.
    X::ProcessFacts xwaylandNoSocket{"Xwayland", kProcNetUnix, {}};
    EXPECT_FALSE(X::MayDial(":0", xwaylandNoSocket, nullptr));
    // A process that is not named like an X server but holds the listener serves it too.
    X::ProcessFacts renamed{"my-x-server", kProcNetUnix, {41001}};
    EXPECT_FALSE(X::MayDial(":0", renamed, nullptr));
    EXPECT_TRUE(X::MayDial(":1", renamed, nullptr)) << "display 1 is someone else's";
}

TEST(X11DisplayGuard, AnOrdinaryClientDialsUnlessForbidden) {
    X::ProcessFacts client{"glxgears", kProcNetUnix, {41003, 41005}}; // connected, not listening
    EXPECT_TRUE(X::MayDial(":0", client, nullptr));
    EXPECT_TRUE(X::MayDial(":0", client, "1"));
    EXPECT_FALSE(X::MayDial(":0", client, "0"));
    EXPECT_FALSE(X::MayDial(":0", client, "off"));
    EXPECT_FALSE(X::MayDial(nullptr, client, nullptr));
    EXPECT_FALSE(X::MayDial("", client, nullptr));
    EXPECT_TRUE(X::MayDial("weird-name", client, nullptr)) << "no number: only an X server refuses";
    EXPECT_TRUE(X::IsXServerProgram("Xorg"));
    EXPECT_TRUE(X::IsXServerProgram("Xvfb"));
    EXPECT_FALSE(X::IsXServerProgram("Xclock"));
}

namespace {
    // A listening abstract socket named like display `number`'s, as an X server (or KWin, for
    // Xwayland's -listenfd) binds it.
    int ListenAsDisplay(int number) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string name = "/tmp/.X11-unix/X" + std::to_string(number);
        std::memcpy(addr.sun_path + 1, name.data(), name.size()); // abstract: leading NUL
        const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), length) != 0 || ::listen(fd, 4) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    int UnusedDisplayNumber() { return 3000 + static_cast<int>(::getpid() % 2000); }
} // namespace

TEST(X11DisplayGuard, ThisProcessServesADisplayWhileItListensOnItsSocket) {
    const int number = UnusedDisplayNumber();
    const std::string name = ":" + std::to_string(number);
    EXPECT_TRUE(X::MayDialX11Display(name.c_str()));
    const int listener = ListenAsDisplay(number);
    ASSERT_GE(listener, 0) << std::strerror(errno);
    EXPECT_FALSE(X::MayDialX11Display(name.c_str()));
    ::close(listener);
    EXPECT_TRUE(X::MayDialX11Display(name.c_str()));
}

// THE DEADLOCK. In a child (a hang must not take the suite with it): listen like an X server,
// point DISPLAY at ourselves, and create the default EGL display - which used to XOpenDisplay
// DISPLAY for the configs' native visual and block in the connection setup forever.
TEST(X11DisplayGuard, AnEglDisplayInAnXServerDoesNotDialItself) {
    const int number = UnusedDisplayNumber() + 1;
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(fds[0]);
        const int listener = ListenAsDisplay(number);
        if (listener < 0) ::_exit(3);
        const std::string name = ":" + std::to_string(number);
        ::setenv("DISPLAY", name.c_str(), 1);
        ::unsetenv("MOBILEGL_X11_DIAL");
        {
            MG_State::EGLState::EGLContext state;
            const EGLDisplay display = state.GetDisplay(EGL_DEFAULT_DISPLAY);
            const char ok = display != EGL_NO_DISPLAY ? 'y' : 'n';
            (void)!::write(fds[1], &ok, 1);
        }
        ::_exit(0);
    }
    ::close(fds[1]);
    pollfd p{fds[0], POLLIN, 0};
    const int ready = ::poll(&p, 1, 15000);
    char got = 0;
    if (ready > 0) (void)!::read(fds[0], &got, 1);
    if (ready <= 0) ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::close(fds[0]);
    ASSERT_GT(ready, 0) << "eglGetDisplay blocked: MobileGL dialed the X display its own process serves";
    EXPECT_EQ(got, 'y');
}
