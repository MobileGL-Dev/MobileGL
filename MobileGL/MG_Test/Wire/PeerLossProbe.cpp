// SPDX-License-Identifier: LGPL-3.0-only
// Manual P6.5 S2/half-open probe. Never registered as a device-less unit test.
#include <Init.h>
#include <MG_Impl/GLImpl/Getter/GL_Getter.h>
#include <MG_Impl/Pipe/SlotAllocator.h>
#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/Transport/Doorbell.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace MobileGL;
using namespace MobileGL::MG_Remote;
namespace {
    struct Options {
        bool kill = false;
        bool external = false;
        unsigned deadlineMs = 10000;
        unsigned armTimeoutMs = 120000;
        std::string readyFile;
        std::string startFile;
    };
    void Usage() {
        std::fprintf(stderr, "MobileGLPeerLossProbe (--kill | --external) [--deadline-ms 10000]\n"
                             "  [--ready-file PATH] [--start-file PATH] [--arm-timeout-ms 120000]\n"
                             "MOBILEGL_IPC_CONTROL=tcp://host:port is required; token/build policy use normal env.\n"
                             "--kill executes MGITEST_PEER_KILL_CMD with {pid} replaced by Welcome.serverPid.\n"
                             "--external sends no fault command. After READY, create --start-file immediately\n"
                             "before the parent's Wi-Fi cut; without a start file, type GO on stdin to arm.\n"
                             "Only descriptor hangup passes. A deadline never sets the device-lost latch.\n");
    }
    bool Number(const char* text, unsigned& result) {
        char* end = nullptr;
        const auto value = std::strtoul(text, &end, 10);
        if (!text[0] || !end || *end || !value || value > 600000) return false;
        result = static_cast<unsigned>(value);
        return true;
    }
    bool Parse(int argc, char** argv, Options& options) {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--kill")
                options.kill = true;
            else if (argument == "--external")
                options.external = true;
            else if (i + 1 < argc && argument == "--deadline-ms") {
                if (!Number(argv[++i], options.deadlineMs)) return false;
            } else if (i + 1 < argc && argument == "--arm-timeout-ms") {
                if (!Number(argv[++i], options.armTimeoutMs)) return false;
            } else if (i + 1 < argc && argument == "--ready-file")
                options.readyFile = argv[++i];
            else if (i + 1 < argc && argument == "--start-file")
                options.startFile = argv[++i];
            else
                return false;
        }
        return options.kill != options.external && (!options.kill || options.startFile.empty());
    }
    bool Exists(const std::string& path) {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }
    struct SessionCleanup {
        Client::ClientSession& session;
        ~SessionCleanup() { session.Stop(); }
    };
    // The fault helper gets its own process group. It only execs the caller's
    // explicit template; killing/reaping this helper never touches a supervisor.
    class FaultCommand {
    public:
        bool Start(const std::string& command) {
            m_pid = ::fork();
            if (m_pid == 0) {
                if (::setpgid(0, 0) != 0) ::_exit(126);
                ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
                ::_exit(127);
            }
            if (m_pid < 0) return false;
            (void)::setpgid(m_pid, m_pid);
            return true;
        }
        bool Finished() {
            if (m_pid <= 0) return true;
            const auto result = ::waitpid(m_pid, &m_status, WNOHANG);
            if (result == m_pid) {
                m_pid = -1;
                m_finished = true;
            }
            return m_finished;
        }
        bool Succeeded() const { return m_finished && WIFEXITED(m_status) && WEXITSTATUS(m_status) == 0; }
        ~FaultCommand() {
            if (m_pid <= 0 || Finished()) return;
            (void)::kill(-m_pid, SIGTERM);
            for (int i = 0; i != 20 && !Finished(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (m_pid > 0) {
                (void)::kill(-m_pid, SIGKILL);
                (void)::waitpid(m_pid, &m_status, 0);
            }
        }

    private:
        pid_t m_pid = -1;
        int m_status = -1;
        bool m_finished = false;
    };
    bool ArmExternal(const Options& options) {
        if (options.startFile.empty()) {
            std::string line;
            return std::getline(std::cin, line) && line == "GO";
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.armTimeoutMs);
        while (!Exists(options.startFile)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        Usage();
        return 0;
    }
    Options options;
    if (!Parse(argc, argv, options)) {
        Usage();
        return 64;
    }
    const char* endpoint = std::getenv("MOBILEGL_IPC_CONTROL");
    if (!endpoint || std::strncmp(endpoint, "tcp://", 6) != 0) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: MOBILEGL_IPC_CONTROL must name an existing TCP server\n");
        return 64;
    }
    if ((!options.readyFile.empty() && Exists(options.readyFile)) ||
        (!options.startFile.empty() && Exists(options.startFile))) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: readiness/start files must be fresh for this run\n");
        return 64;
    }
    const char* killTemplate = std::getenv("MGITEST_PEER_KILL_CMD");
    if (options.kill && (!killTemplate || !std::strstr(killTemplate, "{pid}"))) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: --kill requires MGITEST_PEER_KILL_CMD containing {pid}\n");
        return 64;
    }
    // No server is forked by the client. StartSpawned resolves Connect from the
    // ordinary configuration, exactly as the integration TCP lane does.
    ::setenv("MOBILEGL_TRANSPORT", "spawn", 1);
    ::setenv("MOBILEGL_IPC_DATA", "stream", 1);
    ::setenv("MOBILEGL_IPC_ROLE", "client", 1);
    ::setenv("MOBILEGL_BACKEND_TYPE", "DirectGLES", 0);
    MG_ConfigLoader::Init();
    auto& session = Client::ClientSessionInstance();
    SessionCleanup cleanup{session};
    const auto started = session.StartSpawned();
    if (started != MOBILEGL_OK || !session.ConnectDial() || !session.DataLink() ||
        session.DataLink()->Capabilities().SharedMapping) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: TCP+stream bringup rc=%d\n", started);
        return 2;
    }
    const auto peerPid = session.PeerServerPid();
    auto* bell = session.SelfDoorbellForTest();
    if (peerPid <= 1 || !bell || bell->PeerHungUp() || Client::ClientSession::DeviceLost() ||
        MG_Impl::GLImpl::GetGraphicsResetStatus() != GL_NO_ERROR) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: no healthy Welcome peer before arming\n");
        return 2;
    }
    // A real control round-trip finishes before the idle measurement. No GL
    // command or application heartbeat is sent during the detection interval.
    session.SyncPeerLog();
    if (bell->PeerHungUp() || Client::ClientSession::DeviceLost()) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: peer disappeared before readiness\n");
        return 2;
    }
    if (!options.readyFile.empty()) {
        std::ofstream ready(options.readyFile, std::ios::out | std::ios::trunc);
        ready << peerPid << '\n';
        ready.flush();
        if (!ready) {
            std::fprintf(stderr, "P65_PEER_LOSS_FAIL: cannot publish ready file\n");
            return 2;
        }
    }
    std::printf("P65_PEER_LOSS_READY pid=%u mode=%s control=%s data=stream deadline_ms=%u\n", peerPid,
                options.kill ? "kill" : "external", endpoint, options.deadlineMs);
    std::fflush(stdout);
    if (options.external && !ArmExternal(options)) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: external trigger was not armed\n");
        return 3;
    }
    if (bell->PeerHungUp()) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: peer disappeared before timer arm; timing is invalid\n");
        return 3;
    }
    std::string command;
    if (options.kill) {
        command = killTemplate;
        const auto pidText = std::to_string(peerPid);
        for (std::size_t at = 0; (at = command.find("{pid}", at)) != std::string::npos; at += pidText.size())
            command.replace(at, 5, pidText);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(options.deadlineMs);
    FaultCommand fault;
    if (options.kill && !fault.Start(command)) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: could not start the caller's kill command\n");
        return 4;
    }
    std::printf("P65_PEER_LOSS_ARMED pid=%u mode=%s\n", peerPid, options.kill ? "kill" : "external");
    std::fflush(stdout);
    while (!bell->PeerHungUp() && std::chrono::steady_clock::now() < deadline) {
        if (options.kill && fault.Finished() && !fault.Succeeded()) {
            std::fprintf(stderr, "P65_PEER_LOSS_FAIL: caller's kill command failed\n");
            return 4;
        }
        (void)bell->Park(10);
    }
    const auto hangupAt = std::chrono::steady_clock::now();
    const auto hangupMs = std::chrono::duration_cast<std::chrono::milliseconds>(hangupAt - start).count();
    if (!bell->PeerHungUp() || !bell->Dead() || hangupAt >= deadline) {
        std::fprintf(stderr,
                     "P65_PEER_LOSS_FAIL: deadline_ms=%u elapsed_ms=%lld peer_hung_up=%d dead=%d device_lost=%d\n",
                     options.deadlineMs, static_cast<long long>(hangupMs), bell->PeerHungUp(), bell->Dead(),
                     Client::ClientSession::DeviceLost());
        return 3;
    }
    if (options.kill) {
        while (!fault.Finished() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!fault.Succeeded()) {
            std::fprintf(stderr, "P65_PEER_LOSS_FAIL: kill command did not complete successfully within budget\n");
            return 4;
        }
    }
    // Exercise the production barrier's ShutDown branch, not a test-written
    // latch. The peer is already proven gone, so this valid reply-owning record
    // cannot reach a server or inspect an invented fence handle there.
    const MG_Pipe::MGPHandleOnly fence{{1, 1}, static_cast<Uint32>(MG_Pipe::MGPipeKind::Fence), 0};
    Uint32 reply = 0;
    Int32 status = Wire::ReplySink::kStatusError;
    session.EmitAndWait(MG_Pipe::MGPWireOp::FenceStatus, &fence, sizeof fence, nullptr, 0, &reply, sizeof reply,
                        &status);
    const auto lostMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const bool lost = Client::ClientSession::DeviceLost();
    const auto reset = MG_Impl::GLImpl::GetGraphicsResetStatus();
    if (!lost || reset != GL_UNKNOWN_CONTEXT_RESET || status != Wire::ReplySink::kStatusDeclined ||
        lostMs >= options.deadlineMs) {
        std::fprintf(stderr, "P65_PEER_LOSS_FAIL: production latch=%d gl_reset=0x%x reply_status=%d elapsed_ms=%lld\n",
                     lost, reset, status, static_cast<long long>(lostMs));
        return 5;
    }
    std::printf("P65_PEER_LOSS_PASS pid=%u mode=%s peer_hung_up_ms=%lld device_lost_ms=%lld "
                "deadline_ms=%u device_lost=1 gl_reset=0x%x source=production-barrier\n",
                peerPid, options.kill ? "kill" : "external", static_cast<long long>(hangupMs),
                static_cast<long long>(lostMs), options.deadlineMs, reset);
    std::fflush(stdout);
    return 0;
}
