// MobileGL - MobileGL/MG_Test/Wire/ServerProbeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// DECLINING WHEN NO SERVER IS THERE (MG_Remote/Client/ServerProbe.h).
//
// Installed as the system's EGL/GLX/GBM vendor, MobileGL is asked first by every GL process, so it
// must step aside - quickly - when the server it would dial is not running:
//   * the probe itself: a listening abstract name / path / loopback port answers Reachable, an
//     absent one Unreachable within milliseconds, and the configurations with nothing to dial
//     (monolith, fork, fd:, probing turned off) NotApplicable;
//   * the entry points, each in a fresh forked process: with the configured endpoint absent,
//     eglGetDisplay, the glvnd vendor's getPlatformDisplay and its device list all decline, and
//     the GLX vendor refuses to load - in well under a second, where the session bring-up they
//     replace waits 20 s; with a real `--serve` supervisor listening, a display is handed out.

#include "P12ServerRig.h"

#include <Init.h>
#include <MG_Impl/EGLImpl/EGLImpl.h>
#include <MG_Remote/Client/ServerProbe.h>
#include <MG_Remote/Transport/SocketTransport.h>

#include <glvnd/libeglabi.h>

#include <sys/un.h>

#include <utility>

extern "C" EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports* exports, __EGLvendorInfo* vendor,
                                 __EGLapiImports* imports);
extern "C" int __glx_Main(uint32_t version, const void* exports, void* vendor, void* imports);

using namespace MobileGL;

// A stand-in for the GBM loader's query, found by the implementation through the process's global
// symbols (the target exports them): a "device" here is just the backend name it reports.
struct FakeGbmDevice {
    const char* backend;
};
extern "C" __attribute__((visibility("default"))) const char* gbm_device_get_backend_name(void* device) {
    return static_cast<FakeGbmDevice*>(device)->backend;
}

namespace {

    namespace EGL = MobileGL::MG_Impl::EGLImpl;
    namespace Probe = MobileGL::MG_Remote::Client;
    namespace Debug = MobileGL::MG_Util::Debug;
    using MobileGL::MG_Remote::Transport::SocketTransport;

    std::string UniqueName(const char* what) {
        return std::string("mgl-probe-test-") + what + "-" + std::to_string(::getpid());
    }

    // A bare listener (no MobileGL server): all the probe needs is someone accepting connections.
    struct Listener {
        int fd = -1;
        ~Listener() {
            if (fd >= 0) ::close(fd);
        }
        bool Unix(const std::string& endpoint) { // "@name" or a path
            fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            socklen_t length = 0;
            if (endpoint[0] == '@') {
                std::memcpy(address.sun_path + 1, endpoint.c_str() + 1, endpoint.size() - 1);
                length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.size());
            } else {
                std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size());
                length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + endpoint.size() + 1);
            }
            return fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), length) == 0 && ::listen(fd, 4) == 0;
        }
        int Tcp() { // returns the port
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof(address);
            if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                ::listen(fd, 4) != 0 || ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0)
                return -1;
            return ntohs(address.sin_port);
        }
    };

    long long MillisSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }

    // The probe reads the environment over the system configuration; no case may read the host's.
    class ServerProbeTest : public ::testing::Test {
    protected:
        void SetUp() override {
            ::setenv("MOBILEGL_CONFIG_FILE", "", 1);
            ::setenv("MOBILEGL_BACKEND_FILE", "", 1);
            ::unsetenv("MOBILEGL_IPC_PROBE_TIMEOUT_MS");
            Probe::ResetServerProbeCache();
        }
        void TearDown() override {
            ::unsetenv("MOBILEGL_TRANSPORT");
            ::unsetenv("MOBILEGL_IPC_CONTROL");
            ::unsetenv("MOBILEGL_IPC_PROBE_TIMEOUT_MS");
            Probe::ResetServerProbeCache();
        }
        static void Configure(const char* transport, const std::string& control) {
            ::setenv("MOBILEGL_TRANSPORT", transport, 1);
            ::setenv("MOBILEGL_IPC_CONTROL", control.c_str(), 1);
        }
    };

    TEST_F(ServerProbeTest, AListeningEndpointIsReachableAndAnAbsentOneIsNotQuickly) {
        Listener abstract;
        const std::string name = "@" + UniqueName("abstract");
        ASSERT_TRUE(abstract.Unix(name));
        EXPECT_TRUE(SocketTransport::Probe(name, 250));

        const std::string path = "/tmp/" + UniqueName("path") + ".sock";
        ::unlink(path.c_str());
        {
            Listener filesystem;
            ASSERT_TRUE(filesystem.Unix(path));
            EXPECT_TRUE(SocketTransport::Probe(path, 250));
        }
        ::unlink(path.c_str());

        Listener tcp;
        const int port = tcp.Tcp();
        ASSERT_GT(port, 0);
        EXPECT_TRUE(SocketTransport::Probe("tcp://127.0.0.1:" + std::to_string(port), 250));

        const auto start = std::chrono::steady_clock::now();
        EXPECT_FALSE(SocketTransport::Probe("@" + UniqueName("nobody"), 250));
        EXPECT_FALSE(SocketTransport::Probe(path, 250)); // unlinked: ENOENT
        ::close(std::exchange(tcp.fd, -1));
        EXPECT_FALSE(SocketTransport::Probe("tcp://127.0.0.1:" + std::to_string(port), 250));
        EXPECT_LT(MillisSince(start), 200) << "a refused or absent endpoint must answer at once, not at the bound";
    }

    TEST_F(ServerProbeTest, TheConfigurationDecidesWhatIsProbed) {
        Listener abstract;
        const std::string name = "@" + UniqueName("configured");
        ASSERT_TRUE(abstract.Unix(name));

        Configure("spawn", "unix:" + name);
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::Reachable);
        Configure("SPAWN", "unix:@" + UniqueName("absent"));
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::Unreachable);

        // Nothing to dial: these are served some other way, never declined by a probe.
        Configure("monolith", "unix:@" + UniqueName("absent"));
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::NotApplicable);
        Configure("spawn", "fork");
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::NotApplicable);
        Configure("spawn", "fd:3,4");
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::NotApplicable);
        ::unsetenv("MOBILEGL_TRANSPORT");
        ::unsetenv("MOBILEGL_IPC_CONTROL");
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::NotApplicable);

        // MOBILEGL_IPC_PROBE_TIMEOUT_MS=0 turns probing off.
        Configure("spawn", "unix:@" + UniqueName("absent"));
        ::setenv("MOBILEGL_IPC_PROBE_TIMEOUT_MS", "0", 1);
        EXPECT_EQ(Probe::ProbeConfiguredServer(), Probe::ServerProbeResult::NotApplicable);
    }

    TEST_F(ServerProbeTest, APositiveAnswerIsCachedAndANegativeOneIsAskedAgain) {
        const std::string name = "@" + UniqueName("cache");
        Configure("spawn", "unix:" + name);
        EXPECT_FALSE(Probe::ConfiguredServerAvailable());
        {
            Listener late;
            ASSERT_TRUE(late.Unix(name));
            EXPECT_TRUE(Probe::ConfiguredServerAvailable()); // the server came up: asked again
        }
        EXPECT_TRUE(Probe::ConfiguredServerAvailable()); // and remembered
        Probe::ResetServerProbeCache();
        EXPECT_FALSE(Probe::ConfiguredServerAvailable());
    }

    // ---- the entry points, each in a fresh process ---------------------------------------------

    struct EntryReport {
        Int32 getDisplay = -1;     // 1: eglGetDisplay gave a display
        Int32 vendorDisplay = -1;  // 1: the glvnd vendor's getPlatformDisplay did
        Int32 vendorDevices = -1;  // the vendor's eglQueryDevicesEXT count
        Int32 glxLoaded = -1;      // __glx_Main's answer
        Int32 clientQueries = -1;  // 1: client extensions and a proc address answered (no display needed)
        Int64 elapsedMs = -1;      // all of the above, end to end
    };

    EntryReport RunEntryPoints(const std::string& control, const std::string& logBase, bool software = false) {
        EntryReport report{};
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) return report;
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", control.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
            if (software) ::setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
            else ::unsetenv("LIBGL_ALWAYS_SOFTWARE");
            EntryReport r{};
            const auto start = std::chrono::steady_clock::now();

            __EGLapiExports exports{};
            __EGLapiImports imports{};
            const bool vendor = __egl_Main(EGL_VENDOR_ABI_VERSION, &exports, nullptr, &imports) == EGL_TRUE;
            // What a dispatcher asks every vendor before choosing one: never a session bring-up.
            r.clientQueries = vendor && imports.getProcAddress("eglQueryString") != nullptr &&
                              EGL::QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) != nullptr &&
                              imports.getDispatchAddress("glClear") != nullptr &&
                              EGL::GetProcAddress("eglQueryDmaBufModifiersEXT") != nullptr;
            r.vendorDisplay = vendor && imports.getPlatformDisplay(EGL_NONE, EGL_DEFAULT_DISPLAY, nullptr) != EGL_NO_DISPLAY;
            r.getDisplay = EGL::GetDisplay(EGL_DEFAULT_DISPLAY) != EGL_NO_DISPLAY;
            if (vendor) {
                using QueryDevices = EGLBoolean (*)(EGLint, EGLDeviceEXT*, EGLint*);
                auto* query = reinterpret_cast<QueryDevices>(imports.getProcAddress("eglQueryDevicesEXT"));
                EGLint count = -1;
                if (query != nullptr && query(0, nullptr, &count) == EGL_TRUE) r.vendorDevices = count;
            }
            void* glxExports[16] = {};
            void* glxImports[16] = {};
            r.glxLoaded = __glx_Main(1u << 16, glxExports, nullptr, glxImports);
            r.elapsedMs = MillisSince(start);

            const ssize_t wrote = ::write(fds[1], &r, sizeof(r));
            (void)wrote;
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(fds[1]);
        pollfd pfd{fds[0], POLLIN, 0};
        if (pid > 0 && ::poll(&pfd, 1, 60000) > 0) {
            const ssize_t got = ::read(fds[0], &report, sizeof(report));
            (void)got;
        }
        ::close(fds[0]);
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        return report;
    }

    TEST_F(ServerProbeTest, WithNoServerEveryEntryPointDeclinesWellInsideASecond) {
        const std::string logBase = "/tmp/" + UniqueName("decline") + ".log";
        const EntryReport r = RunEntryPoints("unix:@" + UniqueName("no-server"), logBase);
        EXPECT_EQ(r.vendorDisplay, 0) << "the glvnd vendor must decline so the next vendor is asked";
        EXPECT_EQ(r.getDisplay, 0);
        EXPECT_EQ(r.vendorDevices, 0) << "no device either, and no error (glvnd fails its merge on one)";
        EXPECT_EQ(r.glxLoaded, 0) << "the GLX vendor must refuse to load so libGLX falls back";
        EXPECT_EQ(r.clientQueries, 1) << "client queries are answered without a server";
        EXPECT_GE(r.elapsedMs, 0);
        EXPECT_LT(r.elapsedMs, 1000) << "declining must not cost the bring-up's 20 s connect budget";
        std::error_code ec;
        std::filesystem::remove(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Client), ec);
    }

    // A GBM device another backend created is declined before MobileGL brings anything up (so a
    // loader with a second vendor gives it to its owner); one of MobileGL's own is served.
    TEST_F(ServerProbeTest, AForeignGbmDeviceIsDeclinedAndOneOfOursIsServed) {
        P12::ServerProcess server;
        if (!P12::LaunchSupervisor("probe-gbm", &server)) GTEST_SKIP() << "no `--serve` supervisor on this host";
        const std::string logBase = "/tmp/" + UniqueName("gbm") + ".log";
        int fds[2] = {-1, -1};
        ASSERT_EQ(::pipe(fds), 0);
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::close(fds[0]);
            ::setenv("MOBILEGL_LOG_FILE_PATH", logBase.c_str(), 1);
            ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
            ::setenv("MOBILEGL_IPC_CONTROL", server.endpoint.c_str(), 1);
            ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 1);
            constexpr EGLenum kPlatformGbm = 0x31D7;
            FakeGbmDevice foreign{"drm"};
            FakeGbmDevice ours{"mobilegl"};
            Int32 result[3] = {-1, -1, -1};
            result[0] = EGL::GetPlatformDisplay(kPlatformGbm, &foreign, nullptr) != EGL_NO_DISPLAY;
            result[1] = MobileGL::MG_Remote::Client::ClientSession::Active() != nullptr; // nothing brought up for it
            result[2] = EGL::GetPlatformDisplay(kPlatformGbm, &ours, nullptr) != EGL_NO_DISPLAY;
            const ssize_t wrote = ::write(fds[1], result, sizeof(result));
            (void)wrote;
            std::fflush(nullptr);
            ::_exit(0);
        }
        ::close(fds[1]);
        Int32 result[3] = {-1, -1, -1};
        pollfd pfd{fds[0], POLLIN, 0};
        if (::poll(&pfd, 1, 60000) > 0) {
            const ssize_t got = ::read(fds[0], result, sizeof(result));
            (void)got;
        }
        ::close(fds[0]);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        EXPECT_EQ(result[0], 0) << "a device from another GBM backend must be declined";
        EXPECT_EQ(result[1], 0) << "and declined before the session was brought up";
        EXPECT_EQ(result[2], 1) << "a device of MobileGL's own GBM backend is served";
        std::error_code ec;
        std::filesystem::remove(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Client), ec);
    }

    TEST_F(ServerProbeTest, WithAServerListeningADisplayIsHandedOut) {
        P12::ServerProcess server;
        if (!P12::LaunchSupervisor("probe", &server)) GTEST_SKIP() << "no `--serve` supervisor on this host";
        const std::string logBase = "/tmp/" + UniqueName("serve") + ".log";
        const EntryReport r = RunEntryPoints(server.endpoint, logBase);
        EXPECT_EQ(r.vendorDisplay, 1) << server.Log();
        EXPECT_EQ(r.getDisplay, 1);
        EXPECT_EQ(r.vendorDevices, 1);
        EXPECT_EQ(r.glxLoaded, 1);
        EXPECT_EQ(r.clientQueries, 1);

        // A process that asked for software rendering is left to the system's other GL, server or not.
        const EntryReport software = RunEntryPoints(server.endpoint, logBase, true);
        EXPECT_EQ(software.vendorDisplay, 0);
        EXPECT_EQ(software.getDisplay, 0);
        EXPECT_EQ(software.vendorDevices, 0);
        EXPECT_EQ(software.glxLoaded, 0);
        std::error_code ec;
        std::filesystem::remove(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Client), ec);
    }
} // namespace
