// MobileGL - MobileGL/MG_Test/Wire/ServerOwnedSurfaceTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window), END TO END ON THE HOST: a real MobileGL client - the EGL entry
// points, MobileGL's own initialisation, the spawn/tcp session - against a real `--serve` supervisor
// that, like every host server, owns NO display.
//
// What is proven, each on both sides of the wire:
//   * with MOBILEGL_IPC_SURFACE=server a headless client's eglCreateWindowSurface(NULL) reaches the
//     server as a ServerOwned request, the server REFUSES it BY NAME (it owns no display), the client
//     fails it BY NAME (EGL_BAD_NATIVE_WINDOW, and its own log line), and the session is NOT latched:
//     it goes on to create a pbuffer, make it current, and end exit=0;
//   * without the knob a NULL window is the client's own EGL_BAD_NATIVE_WINDOW and the server never
//     hears of it;
//   * D4: a session whose first surface was a pbuffer (offscreen) is refused a ServerOwned surface as
//     SurfaceModeMismatch - named by the server and by the client - and carries on.

#include "P12ServerRig.h"

#include <MG_Impl/EGLImpl/EGLImpl.h>

using namespace MobileGL;
using namespace P12;

namespace {

    namespace EGL = MobileGL::MG_Impl::EGLImpl;

    enum class Scenario {
        ServerOwnedFirst,       // the ServerOwned create, then a pbuffer
        NullWindowWithoutKnob,  // the knob unset: a NULL window, then a pbuffer
        PbufferThenServerOwned, // a pbuffer first (offscreen), then the ServerOwned create
    };

    struct EglPeerReport {
        Int32 initialized = 0;
        Int32 windowCreated = 0; // eglCreateWindowSurface returned a surface
        Int32 windowError = 0;   // eglGetError after it
        Int32 pbufferCreated = 0;
        Int32 madeCurrent = 0;
        Int32 madeCurrentAfter = 0; // the pbuffer still current after the refused window (mismatch case)
        Uint32 serverPid = 0;
        char note[256] = {};
    };

    // A forked peer that is a whole MobileGL client: the environment a deployment sets, then the EGL
    // entry points. Where its lines land is ClientLog's note below.
    EglPeerReport RunEglPeer(const ServerProcess& server, Scenario scenario, bool surfaceKnob,
                             const std::string& clientLogBase) {
        EglPeerReport report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", clientLogBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", server.endpoint.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
            if (surfaceKnob) ::setenv("MOBILEGL_IPC_SURFACE", "server", 1);
            else ::unsetenv("MOBILEGL_IPC_SURFACE");
            EglPeerReport r{};
            const auto note = [&r](const char* text) { std::snprintf(r.note, sizeof(r.note), "%s", text); };
            const EGLDisplay dpy = EGL::GetDisplay(EGL_DEFAULT_DISPLAY);
            EGLint major = 0;
            EGLint minor = 0;
            if (dpy == EGL_NO_DISPLAY || EGL::Initialize(dpy, &major, &minor) != EGL_TRUE) {
                note("eglInitialize failed (no spawn session?)");
            } else {
                r.initialized = 1;
                if (auto* session = Remote::Client::ClientSession::Active()) r.serverPid = session->PeerServerPid();
                (void)EGL::BindAPI(EGL_OPENGL_API);
                const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_NONE};
                EGLConfig config = nullptr;
                EGLint count = 0;
                (void)EGL::ChooseConfig(dpy, configAttribs, &config, 1, &count);
                const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
                const EGLContext context = EGL::CreateContext(dpy, config, EGL_NO_CONTEXT, contextAttribs);
                const EGLint windowAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 48, EGL_NONE};
                const EGLint pbufferAttribs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
                EGLSurface pbuffer = EGL_NO_SURFACE;
                const auto tryWindow = [&] {
                    const EGLSurface window = EGL::CreateWindowSurface(dpy, config, NativeWindowType{}, windowAttribs);
                    r.windowCreated = window != EGL_NO_SURFACE ? 1 : 0;
                    r.windowError = EGL::GetError();
                };
                const auto tryPbuffer = [&] {
                    pbuffer = EGL::CreatePbufferSurface(dpy, config, pbufferAttribs);
                    r.pbufferCreated = pbuffer != EGL_NO_SURFACE ? 1 : 0;
                    if (r.pbufferCreated) r.madeCurrent = EGL::MakeCurrent(dpy, pbuffer, pbuffer, context) == EGL_TRUE;
                };
                if (scenario == Scenario::PbufferThenServerOwned) {
                    tryPbuffer();
                    tryWindow();
                    if (r.pbufferCreated) {
                        // The session carries on on its own path after the refusal.
                        (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                        r.madeCurrentAfter = EGL::MakeCurrent(dpy, pbuffer, pbuffer, context) == EGL_TRUE;
                    }
                } else {
                    tryWindow();
                    tryPbuffer();
                }
                (void)EGL::MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (pbuffer != EGL_NO_SURFACE) (void)EGL::DestroySurface(dpy, pbuffer);
                if (context != EGL_NO_CONTEXT) (void)EGL::DestroyContext(dpy, context);
                (void)EGL::Terminate(dpy);
            }
            const ssize_t wrote = ::write(fds[1], &r, sizeof(r));
            (void)wrote;
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(fds[1]);
        if (pid < 0) {
            ::close(fds[0]);
            return report;
        }
        pollfd pfd{fds[0], POLLIN, 0};
        if (::poll(&pfd, 1, 60000) > 0) {
            if (::read(fds[0], &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report))) report = EglPeerReport{};
        } else {
            ::kill(pid, SIGKILL);
        }
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }

    // The supervisor's reap line for one session child, or "" while it has not been reaped.
    std::string ReapLine(const ServerProcess& server, Uint32 sessionPid) {
        const std::string log = server.Log();
        const std::string needle = "session pid=" + std::to_string(sessionPid) + " reaped ";
        const auto at = log.find(needle);
        if (at == std::string::npos) return {};
        const auto end = log.find('\n', at);
        return log.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }

    // WHERE THE PEER'S LINES LAND. The peer's forwarded SERVER lines are written under its own base
    // (WritePeerLog reads MOBILEGL_LOG_FILE_PATH at each write), so they never truncate the
    // supervisor's log. Its own CLIENT lines, though, go through the client-role FILE* this test
    // process already opened when it launched the supervisor (ServerSpawn logs), which the fork
    // inherits - so they are read from the supervisor base's client file. Nothing else in this
    // process writes a ServerOwned line there.
    struct ClientLog {
        std::string base;
        std::string inheritedBase;
        ClientLog(const std::string& label, const ServerProcess& server)
            : base("/tmp/mgl-p12-client-" + label + "-" + std::to_string(::getpid()) + ".log"),
              inheritedBase(server.logBase) {
            Debug::TruncateRoleLogs(base.c_str());
        }
        // This client's OWN lines (and the test process's own spawn lines, which name no surface).
        std::string Own() const {
            return ReadFile(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Client)) +
                   ReadFile(Debug::RoleLogPath(inheritedBase.c_str(), Debug::LogRole::Client));
        }
        ~ClientLog() {
            if (::testing::Test::HasFailure()) return;
            std::error_code ec;
            std::filesystem::remove(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Client), ec);
            std::filesystem::remove(Debug::RoleLogPath(base.c_str(), Debug::LogRole::Server), ec);
            std::filesystem::remove(base, ec);
        }
    };

} // namespace

// THE HEADLINE (D1 + D3 on the host). Red with the server's no-display refusal made a latch: the
// session ends exit=75 and the pbuffer after it is never created; red with the client's refusal
// naming removed: the client log has no line of its own.
TEST(ServerOwnedSurface, ADisplaylessServerRefusesItByNameOnBothSidesAndTheSessionCarriesOn) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("serverowned", &server)) << server.Log();
    ClientLog client("serverowned", server);
    const EglPeerReport r = RunEglPeer(server, Scenario::ServerOwnedFirst, /*surfaceKnob=*/true, client.base);
    ASSERT_EQ(r.initialized, 1) << r.note << "\n" << client.Own();
    EXPECT_EQ(r.windowCreated, 0) << "a server with no display created a server-owned window surface";
    EXPECT_EQ(r.windowError, EGL_BAD_NATIVE_WINDOW);

    std::string reap;
    ASSERT_TRUE(WaitFor([&] { return !(reap = ReapLine(server, r.serverPid)).empty(); }, 15000)) << server.Log();
    const std::string serverLog = server.Log();
    EXPECT_NE(serverLog.find("Refuse ServerOwned: this server owns no display"), std::string::npos)
        << "the server did not name its refusal:\n" << serverLog;
    EXPECT_EQ(serverLog.find("Fatal{"), std::string::npos) << "the refusal latched or killed the session:\n" << serverLog;
    EXPECT_NE(reap.find("exit=0 "), std::string::npos) << "the session did not end cleanly: " << reap;
    const std::string clientLog = client.Own();
    EXPECT_NE(clientLog.find("Refuse ServerOwned (NoServerDisplay)"), std::string::npos)
        << "the client did not name the refusal in its own log:\n" << clientLog;

    // Not latched: the same session went on to its pbuffer. Its make-current needs headless EGL.
    EXPECT_EQ(r.pbufferCreated, 1) << "the session did not carry on after the refusal";
    EXPECT_NE(serverLog.find("surface=pbuffer 8x8"), std::string::npos) << "no pbuffer arm proof:\n" << serverLog;
    if (r.madeCurrent == 0) GTEST_SKIP() << "refusal proven; the make-current half needs headless EGL";
}

// D1: without the knob a NULL window never leaves the client. Red with the client's null check made
// unconditional in the other direction (the knob ignored): the server logs a ServerOwned refusal.
TEST(ServerOwnedSurface, WithoutTheKnobANullWindowIsTheClientsOwnRefusal) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("nullwindow", &server)) << server.Log();
    ClientLog client("nullwindow", server);
    const EglPeerReport r = RunEglPeer(server, Scenario::NullWindowWithoutKnob, /*surfaceKnob=*/false, client.base);
    ASSERT_EQ(r.initialized, 1) << r.note;
    EXPECT_EQ(r.windowCreated, 0);
    EXPECT_EQ(r.windowError, EGL_BAD_NATIVE_WINDOW);
    ASSERT_TRUE(WaitFor([&] { return !ReapLine(server, r.serverPid).empty(); }, 15000)) << server.Log();
    EXPECT_EQ(server.Log().find("ServerOwned"), std::string::npos) << "a NULL window without the knob reached the server";
    EXPECT_EQ(r.pbufferCreated, 1);
}

// D4. The first surface was a pbuffer, so the session is offscreen; the ServerOwned create after it is
// SurfaceModeMismatch - checked BEFORE the display (this server has none, and the answer is still
// the mode) - named on both sides, and the session keeps its pbuffer. Red with the mode check deleted:
// the server answers NoServerDisplay instead.
TEST(ServerOwnedSurface, AnOffscreenSessionRefusesAServerOwnedSurfaceAsSurfaceModeMismatch) {
    ServerProcess server;
    ASSERT_TRUE(LaunchSupervisor("modemismatch", &server)) << server.Log();
    ClientLog client("modemismatch", server);
    const EglPeerReport r = RunEglPeer(server, Scenario::PbufferThenServerOwned, /*surfaceKnob=*/true, client.base);
    ASSERT_EQ(r.initialized, 1) << r.note;
    if (r.pbufferCreated == 0) GTEST_SKIP() << "the offscreen mode needs a pbuffer, and this host has no headless EGL";
    EXPECT_EQ(r.windowCreated, 0) << "an offscreen session went on-screen";
    EXPECT_EQ(r.windowError, EGL_BAD_NATIVE_WINDOW);
    std::string reap;
    ASSERT_TRUE(WaitFor([&] { return !(reap = ReapLine(server, r.serverPid)).empty(); }, 15000)) << server.Log();
    const std::string serverLog = server.Log();
    EXPECT_NE(serverLog.find("SurfaceModeMismatch - a ServerOwned CreateWindowSurface"), std::string::npos)
        << serverLog;
    EXPECT_EQ(serverLog.find("Refuse ServerOwned: this server owns no display"), std::string::npos)
        << "the mode was not asked first";
    EXPECT_EQ(serverLog.find("Fatal{"), std::string::npos) << serverLog;
    EXPECT_NE(reap.find("exit=0 "), std::string::npos) << reap;
    EXPECT_NE(client.Own().find("SurfaceModeMismatch - eglCreateWindowSurface"), std::string::npos) << client.Own();
    EXPECT_EQ(r.madeCurrentAfter, r.madeCurrent) << "the session did not keep its pbuffer after the refusal";
}
