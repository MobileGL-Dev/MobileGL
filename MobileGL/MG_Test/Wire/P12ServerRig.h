// MobileGL - MobileGL/MG_Test/Wire/P12ServerRig.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P12 (on-screen server window): the two-process rig InProcessServerTest and ServerOwnedSurfaceTest
// share - a server (the exec'd `--serve` supervisor, or a forked process running the IN-PROCESS
// server entry on a thread), forked peers that talk to it over loopback TCP, and the healthy
// session PeerLatchTest's second session runs (a record applied and, with headless EGL, a green clear
// read back). The shapes are PeerLatchTest's, trimmed to what these two suites need; see there for
// the reasons behind each (why peers are forks, why a failing case keeps its logs).
//
// Header-only and everything in an anonymous namespace: each suite is one translation unit.

#pragma once

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/ILink.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_Util/Debug/Log.h>
#include <Config.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    namespace P12 {
        namespace P = MobileGL::MG_Pipe;
        namespace Remote = MobileGL::MG_Remote;
        namespace Debug = MobileGL::MG_Util::Debug;
        using MobileGL::Int32;
        using MobileGL::Uint32;
        using MobileGL::Uint64;
        using MobileGL::Uint8;

        inline std::string ServerImage() {
            if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) return explicitPath;
            return "libMobileGLServer.so";
        }

        // The server (and a server process forked from this one) inherits this; its make-current
        // needs a headless EGL the way ServerLoopEglTest's does.
        inline void PinHeadlessEgl() {
            if (std::getenv("EGL_PLATFORM") == nullptr) ::setenv("EGL_PLATFORM", "surfaceless", 1);
            if (std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") == nullptr) {
                const char* mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
                std::error_code ec;
                if (std::filesystem::exists(mesa, ec)) ::setenv("__EGL_VENDOR_LIBRARY_FILENAMES", mesa, 1);
            }
            ::unsetenv("DISPLAY");
            ::unsetenv("WAYLAND_DISPLAY");
        }

        inline std::string ReadFile(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            std::stringstream all;
            all << in.rdbuf();
            return all.str();
        }

        inline int FreeLoopbackPort() {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return -1;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            socklen_t len = sizeof(addr);
            int port = -1;
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
                port = ntohs(addr.sin_port);
            }
            ::close(fd);
            return port;
        }

        inline bool WaitFor(const std::function<bool()>& predicate, int timeoutMs) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (std::chrono::steady_clock::now() < deadline) {
                if (predicate()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return predicate();
        }

        inline Uint32 Count(const std::string& haystack, const std::string& needle) {
            Uint32 n = 0;
            for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) ++n;
            return n;
        }

        // A server process this test owns: its pid, its tcp:// endpoint, and the log base its roles
        // write under. Stopped with SIGTERM (then SIGKILL) and reaped; the logs are removed unless the
        // case failed - then they are the evidence the messages quote.
        struct ServerProcess {
            pid_t pid = -1;
            std::string endpoint;
            std::string logBase;
            int exitStatus = -1; // waitpid status once reaped

            std::string Log() const { return ReadFile(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Server)); }

            // Waits for the process to end on its own, up to `timeoutMs`.
            bool WaitExit(int timeoutMs) {
                if (pid <= 0) return true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                for (;;) {
                    int status = 0;
                    const pid_t got = ::waitpid(pid, &status, WNOHANG);
                    if (got == pid) {
                        exitStatus = status;
                        pid = -1;
                        return true;
                    }
                    if (got < 0) {
                        pid = -1;
                        return true;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            void Stop() {
                if (pid <= 0) return;
                ::kill(pid, SIGTERM);
                if (!WaitExit(10000)) {
                    ::kill(pid, SIGKILL);
                    (void)WaitExit(5000);
                }
            }
            ~ServerProcess() {
                Stop();
                if (logBase.empty() || ::testing::Test::HasFailure()) return;
                std::error_code ec;
                std::filesystem::remove(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Server), ec);
                std::filesystem::remove(Debug::RoleLogPath(logBase.c_str(), Debug::LogRole::Client), ec);
                std::filesystem::remove(logBase, ec);
            }
        };

        // The exec'd `--serve` supervisor (the fork-per-session shape), on a fresh loopback port.
        inline bool LaunchSupervisor(const std::string& label, ServerProcess* server) {
            server->logBase = "/tmp/mgl-p12-" + label + "-" + std::to_string(::getpid()) + ".log";
            ::setenv("MOBILEGL_LOG_FILE_PATH", server->logBase.c_str(), 1);
            PinHeadlessEgl();
            for (int attempt = 0; attempt < 5; ++attempt) {
                const int port = FreeLoopbackPort();
                if (port <= 0) return false;
                server->endpoint = "tcp://127.0.0.1:" + std::to_string(port);
                Debug::TruncateRoleLogs(server->logBase.c_str());
                Remote::Server::LaunchedServer launched;
                if (Remote::Server::LaunchServerWithArgs(ServerImage(), server->endpoint, {"--serve"}, &launched) !=
                    MOBILEGL_OK)
                    return false;
                server->pid = launched.pid;
                bool listening = false;
                (void)WaitFor(
                    [&] {
                        if (server->Log().find("listening on") != std::string::npos) return listening = true;
                        return server->WaitExit(0) && server->pid <= 0;
                    },
                    10000);
                if (listening) return true;
                if (server->pid > 0) return false; // alive and silent: not a port race
            }
            return false;
        }

        // ------------------------------------------------------------------------------------
        // Forked peers (PeerLatchTest's RunPeer, trimmed)
        // ------------------------------------------------------------------------------------

        struct PeerReport {
            Int32 started = 0;    // handshake answered Welcome
            Uint32 serverPid = 0; // Welcome::serverPid
            Int32 forged = 0;     // the peer's bad bytes went out
            Int32 applied = 0;    // a record was applied
            Int32 egl = 0;        // headless EGL came up in the session
            Int32 status = -99;   // the read-back's reply status
            Uint32 pixel = 0;     // the read-back texel
            char note[256] = {};
        };

        inline void Note(PeerReport& r, const char* text) { std::snprintf(r.note, sizeof(r.note), "%s", text); }

        using PeerBody = std::function<void(Remote::Client::ClientSession&, PeerReport&)>;

        // Runs `body` in a forked peer connected over TCP to `endpoint`. `holdMs` > 0: the peer reports
        // once `body` returns and then holds its connections open and silent that long; the caller
        // gets its pid in `heldPeer` and owes it ReleaseHeldPeer. `before` runs in the peer first (a
        // config it must differ in).
        inline PeerReport RunPeer(const std::string& endpoint, const PeerBody& body, bool stopCleanly,
                                  int timeoutMs = 30000, int holdMs = 0, pid_t* heldPeer = nullptr,
                                  const std::function<void()>& before = {}) {
            PeerReport report{};
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0) {
                Note(report, "pipe failed");
                return report;
            }
            std::fflush(nullptr);
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::close(fds[0]);
                if (before) before();
                PeerReport r{};
                auto& client = Remote::Client::ClientSessionInstance();
                MobileGL::MG_Config::Ipc.Control = endpoint.c_str();
                MobileGL::MG_Config::Ipc.Data = "auto";
                const MobileGLResult started = client.StartSpawned();
                r.started = started == MOBILEGL_OK ? 1 : 0;
                r.serverPid = client.PeerServerPid();
                if (r.started) body(client, r);
                else std::snprintf(r.note, sizeof(r.note), "start rc=%d", static_cast<int>(started));
                if (stopCleanly && r.started) client.Stop();
                const ssize_t wrote = ::write(fds[1], &r, sizeof(r));
                (void)wrote;
                std::fflush(nullptr);
                if (holdMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
                ::_exit(0);
            }
            ::close(fds[1]);
            if (pid < 0) {
                ::close(fds[0]);
                Note(report, "fork failed");
                return report;
            }
            pollfd pfd{fds[0], POLLIN, 0};
            const int ready = ::poll(&pfd, 1, timeoutMs);
            if (ready > 0) {
                const ssize_t got = ::read(fds[0], &report, sizeof(report));
                if (got != static_cast<ssize_t>(sizeof(report))) {
                    report = PeerReport{};
                    Note(report, "peer died before reporting");
                }
            } else {
                Note(report, "peer timed out");
                ::kill(pid, SIGKILL);
            }
            ::close(fds[0]);
            if (holdMs > 0 && heldPeer != nullptr && ready > 0) {
                *heldPeer = pid;
                return report;
            }
            int status = 0;
            ::waitpid(pid, &status, 0);
            return report;
        }

        inline void ReleaseHeldPeer(pid_t peer) {
            if (peer <= 0) return;
            ::kill(peer, SIGKILL);
            int status = 0;
            ::waitpid(peer, &status, 0);
        }

        // A control frame straight onto the control connection: the SurfaceOp latch rows' carrier.
        inline bool SendSurfaceOp(Remote::Client::ClientSession& client, PeerReport& r, Uint8 kind, Uint8 windowKind,
                                  Uint64 token) {
            flatbuffers::FlatBufferBuilder builder(256);
            const auto op = ::MobileGL::Wire::CreateSurfaceOp(
                builder, /*seq=*/4242, static_cast<::MobileGL::Wire::SurfaceOpKind>(kind), /*display=*/1,
                /*surface=*/1, static_cast<::MobileGL::Wire::WindowKind>(windowKind), token, 16, 16, 0, 1, 1);
            const auto envelope =
                ::MobileGL::Wire::CreateCtrlEnvelope(builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
            ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
            Remote::Transport::ITransport* control = client.Control_Plane();
            if (control == nullptr ||
                control->SendFrame(MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()}) != MOBILEGL_OK) {
                Note(r, "the control frame could not be sent");
                return false;
            }
            r.forged = 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            return true;
        }

        inline bool Emit(Remote::Client::ClientSession& client, P::MGPWireOp op, const void* payload, Uint64 bytes,
                         const void* tail = nullptr, Uint64 tailBytes = 0) {
            Int32 status = Remote::Wire::ReplySink::kStatusError;
            const Uint64 seq = client.EmitAndWait(op, payload, bytes, tail, tailBytes, nullptr, 0, &status);
            return seq != Remote::Wire::kInvalidSeq &&
                   client.WaitForApplied(seq, 5000) == Remote::Transport::SessionWait::Reached;
        }

        // The client's virtual EGL handles (EGLImpl mints small integers; ServerLoopEglTest's shape).
        inline EGLDisplay Dpy() { return reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(0x1)); }
        inline EGLSurface Surf() { return reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(0x1)); }
        inline EGLContext Ctx() { return reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(0x1)); }

        inline bool BringUpEgl(PeerReport& r) {
            EGLint major = -1;
            EGLint minor = -1;
            if (!Remote::Server::ServerInitializeEGLDisplay(Dpy(), &major, &minor)) return Note(r, "no EGL display"), false;
            if (!Remote::Server::ServerCreateEGLPbufferSurface(Surf(), 8, 8)) return Note(r, "no pbuffer"), false;
            if (!Remote::Server::ServerMakeEGLCurrent(Dpy(), Surf(), Surf(), Ctx())) return Note(r, "no make-current"), false;
            r.egl = 1;
            return true;
        }

        // A HEALTHY SESSION: one record applied and, with headless EGL, a green clear read back -
        // PeerLatchTest's session B, verbatim in what it sends.
        inline void HealthySession(Remote::Client::ClientSession& client, PeerReport& r) {
            P::MGPMemoryBarrier barrier{};
            barrier.Bits = 0x2000u;
            r.applied = Emit(client, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
            if (!BringUpEgl(r)) return;
            P::MGPFramebufferState framebuffer{};
            framebuffer.Fbo = P::kMGPipeDefaultFramebuffer;
            framebuffer.Target = static_cast<Uint8>(P::MGPipeFramebufferTarget::Both);
            framebuffer.IsDefault = 1;
            framebuffer.Complete = 1;
            framebuffer.Width = framebuffer.Height = 8;
            framebuffer.Layers = framebuffer.Samples = 1;
            framebuffer.FixedSampleLocations = 1;
            for (auto& drawBuffer : framebuffer.DrawBuffers) drawBuffer = -1;
            framebuffer.DrawBuffers[0] = 0;
            framebuffer.ContentHash = 1;
            P::MGPPixelPackState pack{};
            pack.Pack.Alignment = 4;
            P::MGPSamplerViews views{};
            views.Count = 1;
            const P::MGPBoundView unboundView{};
            P::MGPSamplerStates samplers{};
            samplers.Count = 1;
            const P::MGPipeHandle unboundSampler = P::kMGPipeNullHandle;
            const P::MGPContextValues context{};
            MobileGL::RenderStateParameters params{};
            for (auto& mask : params.ColorMasks) mask = MobileGL::BoolVec4(true, true, true, true);
            std::vector<Uint8> pipelineBytes(P::kMGPipePipelineChunkBytes, 0);
            P::MGPipeGatherPipelineBytes(params, pipelineBytes.data());
            P::MGPRenderStateDesc cso{};
            cso.Cso = P::MGPipeHandle{P::kMGPipeFirstAllocatableSlot, 1u};
            cso.BaseCso = P::kMGPipeNullHandle;
            cso.ChunkMask = P::MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
            cso.Blob = client.Encoder().StageBytes(pipelineBytes.data(), pipelineBytes.size());
            P::MGPBindRenderState bindCso{};
            bindCso.Cso = cso.Cso;
            bindCso.Version = 1;
            bindCso.PipelineVersion = 1;
            if (!Emit(client, P::MGPWireOp::CreateRenderState, &cso, sizeof(cso)) ||
                !Emit(client, P::MGPWireOp::BindRenderState, &bindCso, sizeof(bindCso)))
                return Note(r, "the render state was not applied");
            if (!Emit(client, P::MGPWireOp::SetFramebufferState, &framebuffer, sizeof(framebuffer)) ||
                !Emit(client, P::MGPWireOp::SetPixelPackState, &pack, sizeof(pack)) ||
                !Emit(client, P::MGPWireOp::SetSamplerViews, &views, sizeof(views), &unboundView, sizeof(unboundView)) ||
                !Emit(client, P::MGPWireOp::BindSamplerStates, &samplers, sizeof(samplers), &unboundSampler,
                      sizeof(unboundSampler)) ||
                !Emit(client, P::MGPWireOp::SetContextValues, &context, sizeof(context)))
                return Note(r, "the default framebuffer's inputs were not applied");
            P::MGPClear clear{};
            clear.Fbo = P::kMGPipeNullHandle;
            clear.Kind = P::kMGPipeClearKindColor;
            clear.DrawBufferIndex = 0;
            clear.BufferMask = GL_COLOR_BUFFER_BIT;
            clear.ValueClass = P::kMGPipeClearValueClassFloat;
            const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
            std::memcpy(clear.ColorValue, green, sizeof(green));
            if (!Emit(client, P::MGPWireOp::Clear, &clear, sizeof(clear))) return Note(r, "clear not applied");
            P::MGPReadbackInfo read{};
            read.Res = P::kMGPipeNullHandle;
            read.Box = P::MGPBox{0, 0, 0, 1, 1, 1};
            read.Format = GL_RGBA;
            read.Type = GL_UNSIGNED_BYTE;
            read.DstSize = 4;
            Uint8 texel[4] = {};
            Int32 status = Remote::Wire::ReplySink::kStatusError;
            Uint64 replySize = 0;
            const Uint64 seq = client.EmitAndWait(P::MGPWireOp::ReadPixels, &read, sizeof(read), nullptr, 0, texel,
                                                  sizeof(texel), &status, &replySize);
            r.status = status;
            if (seq == Remote::Wire::kInvalidSeq || replySize != sizeof(texel)) return Note(r, "no read-back");
            std::memcpy(&r.pixel, texel, sizeof(r.pixel));
        }
    } // namespace P12

} // namespace
