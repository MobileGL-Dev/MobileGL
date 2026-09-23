// MobileGL - MobileGL/MG_Test/Wire/PeerLatchTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// PH-1 (3)(4) and Ph fuzz arm 2 ("out-of-bound counts"), P7 package F2 latch (ID-P7-1).
//
// ONE ROW PER SITE, ONE SUPERVISOR PER ROW, TWO SESSIONS PER SUPERVISOR. Every row launches a
// real `--serve` supervisor (the fork-per-session shape a TCP deployment runs), and then:
//
//   session A  a forked peer that handshakes for real and then writes a record (or a control
//              frame) whose bytes no honest client encoder produces - the F2 raw-record peer
//              driver: encode something legal, then overwrite the bytes in the ring before
//              publishing, so every client-side guard has already said yes;
//   the reap   the supervisor's own log names how A's session ended: `exit=75 (latched fault)`
//              for a latched site, `signal=6` for a site that stays Fatal, `exit=0` for a refusal
//              that keeps the session;
//   the line   the FIRST `Fatal{` line in the server log is this row's own marker - so a row
//              cannot pass because some earlier check refused the record for another reason;
//   session B  a second forked peer on the SAME supervisor: Welcome from a new session pid, a
//              record applied, and - where headless EGL exists - a real clear read back.
//
// The table below IS the audit list: scripts/ci/ph_latch_sites.py reads it and every
// SessionLatch / WireProtocolLatch site in the four PH-1 (3) files, and refuses a site that
// has neither a row nor a named "unreachable from the peer" entry.
//
// WHY FORKED PEERS: ClientSession is a process singleton whose device-lost latch never resets,
// so a process that watched session A die cannot be session B. Each peer is a fork of this
// gtest process taken before any session exists; it reports through a pipe and _exits.

#include <MG_Remote/Client/ClientSession.h>
#include <MG_Remote/FatalFunnel.h>
#include <MG_Remote/Protocol/generated/protocol_generated.h>
#include <MG_Remote/Server/ServerLoop.h>
#include <MG_Remote/Server/ServerSpawn.h>
#include <MG_Remote/Transport/Doorbell.h>
#include <MG_Remote/Transport/ILink.h>
#include <MG_Remote/Transport/Ring.h>
#include <MG_Remote/Transport/SocketTransport.h>
#include <MG_Remote/Wire/PipeWireCodec.h>
#include <MG_Pipe/MGPipeRenderStateSpans.h>
#include <MG_Pipe/PipeApply.h>
#include <MG_State/GLState/ProgramState/ProgramArtifactsCodec.h>
#include <MG_State/GLState/TextureState/TextureEnum.h>
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
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace MobileGL::MG_Remote;

#if !defined(_WIN32)

namespace {

    namespace P = MobileGL::MG_Pipe;
    using MobileGL::Int32;
    using MobileGL::Uint16;
    using MobileGL::Uint32;
    using MobileGL::Uint64;
    using MobileGL::Uint8;

    // ------------------------------------------------------------------------------------
    // The supervisor
    // ------------------------------------------------------------------------------------

    std::string ServerImage() {
        if (const char* explicitPath = std::getenv("MOBILEGL_TEST_SERVER_PATH")) return explicitPath;
        return "libMobileGLServer.so";
    }

    // The session child inherits this environment; its make-current needs a headless EGL the
    // way ServerLoopEglTest's does.
    void PinHeadlessEgl() {
        if (std::getenv("EGL_PLATFORM") == nullptr) ::setenv("EGL_PLATFORM", "surfaceless", 1);
        if (std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") == nullptr) {
            const char* mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json";
            std::error_code ec;
            if (std::filesystem::exists(mesa, ec)) ::setenv("__EGL_VENDOR_LIBRARY_FILENAMES", mesa, 1);
        }
    }

    std::string ReadFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream all;
        all << in.rdbuf();
        return all.str();
    }

    int FreeLoopbackPort() {
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

    bool WaitFor(const std::function<bool()>& predicate, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return predicate();
    }

    struct Supervisor {
        Server::LaunchedServer server;
        std::string endpoint;
        std::string logBase;
        bool tcp = false;

        std::string Log() const {
            return ReadFile(MobileGL::MG_Util::Debug::RoleLogPath(
                logBase.c_str(), MobileGL::MG_Util::Debug::LogRole::Server));
        }
        // The supervisor's reap line for one session child, or "" while it has not been reaped.
        std::string ReapLine(Uint32 sessionPid) const {
            const std::string log = Log();
            const std::string needle = "session pid=" + std::to_string(sessionPid) + " reaped ";
            const auto at = log.find(needle);
            if (at == std::string::npos) return {};
            const auto end = log.find('\n', at);
            return log.substr(at, end == std::string::npos ? std::string::npos : end - at);
        }
        void Stop() {
            if (server.pid <= 0) return;
            ::kill(server.pid, SIGTERM);
            int code = -1;
            if (Server::ReapServer(server, 3000, &code) != MOBILEGL_OK) {
                ::kill(server.pid, SIGKILL);
                (void)Server::ReapServer(server, 3000, &code);
            }
            server.pid = -1;
        }
        ~Supervisor() { Stop(); }
    };

    bool Launch(const std::string& label, bool tcp, Supervisor* sup) {
        const std::string stem = "/tmp/mgl-latch-" + label + "-" + std::to_string(::getpid());
        sup->logBase = stem + ".log";
        sup->tcp = tcp;
        if (tcp) {
            const int port = FreeLoopbackPort();
            if (port <= 0) return false;
            sup->endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        } else {
            sup->endpoint = stem + ".sock";
            ::unlink(sup->endpoint.c_str());
        }
        ::setenv("MOBILEGL_LOG_FILE_PATH", sup->logBase.c_str(), 1);
        MobileGL::MG_Util::Debug::TruncateRoleLogs(sup->logBase.c_str());
        PinHeadlessEgl();
        if (Server::LaunchServerWithArgs(ServerImage(), sup->endpoint, {"--serve"}, &sup->server) !=
            MOBILEGL_OK) {
            return false;
        }
        return WaitFor([&] { return sup->Log().find("listening on") != std::string::npos; }, 10000);
    }

    // ------------------------------------------------------------------------------------
    // Forked peers
    // ------------------------------------------------------------------------------------

    struct PeerReport {
        Int32 started = 0;       // handshake answered Welcome
        Int32 forged = 0;        // the row's bytes were published
        Uint32 serverPid = 0;    // Welcome::serverPid - the session child this peer talked to
        Int32 status = -99;      // a reply status, where the row reads one
        Int32 applied = 0;       // a follow-up (or the health check's) record applied
        Int32 egl = 0;           // headless EGL came up in the session
        Uint32 pixel = 0;        // the health check's read-back texel
        char note[256] = {};
    };

    void Note(PeerReport& r, const char* text) {
        std::snprintf(r.note, sizeof(r.note), "%s", text);
    }

    using PeerBody = std::function<void(Client::ClientSession&, PeerReport&)>;

    // Runs `body` in a forked peer connected to `sup`. `stopCleanly` sends the orderly EOF (a
    // session that must end exit=0); otherwise the peer just exits once its session is gone.
    PeerReport RunPeer(const Supervisor& sup, const PeerBody& body, bool stopCleanly,
                       int timeoutMs = 30000) {
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
            PeerReport r{};
            auto& client = Client::ClientSessionInstance();
            MobileGLResult started = MOBILEGL_ERR_UNSUPPORTED;
            if (sup.tcp) {
                // The product's own TCP path: control first, the data connection after Welcome
                // with the session nonce (PH-7 (4)).
                MobileGL::MG_Config::Ipc.Control = sup.endpoint.c_str();
                MobileGL::MG_Config::Ipc.Data = "auto";
                started = client.StartSpawned();
            } else {
                std::unique_ptr<Transport::SocketTransport> transport;
                started = Transport::SocketTransport::ConnectTo(sup.endpoint, 10000, transport);
                if (started == MOBILEGL_OK) started = client.StartOverSocket(std::move(transport));
            }
            r.started = started == MOBILEGL_OK ? 1 : 0;
            r.serverPid = client.PeerServerPid();
            if (r.started) body(client, r);
            else std::snprintf(r.note, sizeof(r.note), "start rc=%d", static_cast<int>(started));
            if (stopCleanly && r.started) client.Stop();
            const ssize_t wrote = ::write(fds[1], &r, sizeof(r));
            (void)wrote;
            std::fflush(nullptr);
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
        int status = 0;
        ::waitpid(pid, &status, 0);
        return report;
    }

    // After the row's bytes: give the session time to reach them. The peer does not have to
    // outlive its session - the parent reads the supervisor's reap line for the verdict - and a
    // server that has not read the bytes yet still does after the peer's EOF (RunSession stops the
    // loop, whose final drain applies what the ring holds, and the control loop reads what the
    // socket holds before it sees the EOF).
    void AwaitSessionEnd(Client::ClientSession& client, Uint64 seq) {
        if (seq != Wire::kInvalidSeq) (void)client.WaitForApplied(seq, 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // ------------------------------------------------------------------------------------
    // The raw-record peer driver (F2's, generalised)
    // ------------------------------------------------------------------------------------

    constexpr Uint64 Align8(Uint64 value) { return (value + 7u) & ~Uint64(7u); }

    template <class T>
    std::vector<Uint8> Bytes(const T& value) {
        std::vector<Uint8> out(sizeof(T));
        std::memcpy(out.data(), &value, sizeof(T));
        return out;
    }

    // The record image a server reads: header, payload, then each tail realigned to 8 -
    // exactly MGPipeWireRecordLayout's arithmetic, so the only thing wrong with a forged
    // record is the thing the row means to be wrong.
    std::vector<Uint8> RecordImage(P::MGPWireOp op, const std::vector<Uint8>& payload,
                                   const std::vector<std::vector<Uint8>>& tails = {}) {
        std::vector<Uint8> image(sizeof(P::MGPWireRecHeader), 0);
        image.insert(image.end(), payload.begin(), payload.end());
        image.resize(static_cast<std::size_t>(Align8(image.size())), 0);
        for (std::size_t i = 0; i < tails.size(); ++i) {
            image.resize(static_cast<std::size_t>(Align8(image.size())), 0);
            image.insert(image.end(), tails[i].begin(), tails[i].end());
        }
        image.resize(static_cast<std::size_t>(Align8(image.size())), 0);
        const Uint32 callFlags = P::MGPipeCallFlagsFor(op);
        Uint16 ringFlags = Transport::kRecNone;
        if ((callFlags & static_cast<Uint32>(P::kNeedsAck)) != 0) ringFlags |= Transport::kRecNeedsAck;
        if ((callFlags & static_cast<Uint32>(P::kHasBlob)) != 0) ringFlags |= Transport::kRecHasBlob;
        if ((callFlags & static_cast<Uint32>(P::kVarTail)) != 0) ringFlags |= Transport::kRecVarTail;
        Transport::RingRecordHeader header{};
        header.kind = static_cast<Uint16>(op);
        header.flags = ringFlags;
        header.size = static_cast<Uint32>(image.size());
        std::memcpy(image.data(), &header, sizeof(header));
        return image;
    }

    // Reserves ring space for `image` by encoding a LEGAL filler of the same total size through
    // the real encoder (so the producer's cursor, seq and submittedSeq stay honest), overwrites
    // the filler's bytes with the image, optionally mutates the header, and publishes.
    //   16 bytes          MemoryBarrier
    //   24 .. 1560 bytes  BindSamplerStates with (total - 24) / 8 handles in its tail
    bool PublishImage(Client::ClientSession& client, const std::vector<Uint8>& image,
                      Uint64* outSeq,
                      const std::function<void(Transport::RingRecordHeader&)>& mutateHeader = {}) {
        if (outSeq != nullptr) *outSeq = Wire::kInvalidSeq;
        auto* link = client.DataLink();
        if (link == nullptr || !link->Attached()) return false;
        const Uint64 total = image.size();
        auto& encoder = client.Encoder();
        Uint64 seq = Wire::kInvalidSeq;
        if (total == 16) {
            P::MGPMemoryBarrier barrier{};
            barrier.Bits = 0x2000u;
            seq = encoder.EncodeRecord(P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier));
        } else {
            const Uint64 base = Align8(sizeof(P::MGPWireRecHeader) + sizeof(P::MGPSamplerStates));
            if (total < base || (total - base) % 8 != 0 || (total - base) / 8 > P::kMGPipeMaxTextureUnits)
                return false;
            P::MGPSamplerStates filler{};
            filler.Start = 0;
            filler.Count = static_cast<Uint32>((total - base) / 8);
            std::vector<P::MGPipeHandle> handles(filler.Count);
            seq = encoder.EncodeRecord(P::MGPWireOp::BindSamplerStates, &filler, sizeof(filler),
                                       handles.empty() ? nullptr : handles.data(),
                                       handles.size() * sizeof(P::MGPipeHandle));
        }
        if (seq == Wire::kInvalidSeq) return false;
        auto& commands = link->CommandsOut();
        const auto* arena = link->RecordArena();
        const Uint64 head = commands.LocalHead();
        if (arena == nullptr || arena->Base == nullptr || head < total) return false;
        auto* bytes = static_cast<Uint8*>(arena->Base) + ((head - total) & arena->Mask);
        auto* header = reinterpret_cast<Transport::RingRecordHeader*>(bytes);
        if (header->size != total) return false;
        std::memcpy(bytes, image.data(), image.size());
        if (mutateHeader) mutateHeader(*header);
        client.Producer().PublishAndNotify(seq);
        if (link->Flush() != MOBILEGL_OK) return false;
        if (outSeq != nullptr) *outSeq = seq;
        return true;
    }

    bool Forge(Client::ClientSession& client, PeerReport& r, P::MGPWireOp op,
               const std::vector<Uint8>& payload, const std::vector<std::vector<Uint8>>& tails = {},
               const std::function<void(Transport::RingRecordHeader&)>& mutateHeader = {}) {
        Uint64 seq = Wire::kInvalidSeq;
        if (!PublishImage(client, RecordImage(op, payload, tails), &seq, mutateHeader)) {
            Note(r, "the forged record could not be published");
            return false;
        }
        r.forged = 1;
        AwaitSessionEnd(client, seq);
        return true;
    }

    // A legal record through the real client path, for a row's setup.
    bool Emit(Client::ClientSession& client, P::MGPWireOp op, const void* payload, Uint64 bytes,
              const void* tail = nullptr, Uint64 tailBytes = 0) {
        Int32 status = Wire::ReplySink::kStatusError;
        const Uint64 seq = client.EmitAndWait(op, payload, bytes, tail, tailBytes, nullptr, 0, &status);
        return seq != Wire::kInvalidSeq &&
               client.WaitForApplied(seq, 5000) == Transport::SessionWait::Reached;
    }

    // A control frame straight onto the control connection - the SurfaceOp rows' carrier.
    bool SendSurfaceOp(Client::ClientSession& client, PeerReport& r, Uint8 kind, Uint8 windowKind) {
        flatbuffers::FlatBufferBuilder builder(256);
        const auto op = ::MobileGL::Wire::CreateSurfaceOp(
            builder, /*seq=*/4242, static_cast<::MobileGL::Wire::SurfaceOpKind>(kind), /*display=*/1,
            /*surface=*/1, static_cast<::MobileGL::Wire::WindowKind>(windowKind),
            /*nativeToken=*/0x1234, 16, 16, 0, 1, 1);
        const auto envelope = ::MobileGL::Wire::CreateCtrlEnvelope(
            builder, ::MobileGL::Wire::CtrlMsg::SurfaceOp, op.Union());
        ::MobileGL::Wire::FinishCtrlEnvelopeBuffer(builder, envelope);
        Transport::ITransport* control = client.Control_Plane();
        if (control == nullptr ||
            control->SendFrame(MobileGLByteSpan{builder.GetBufferPointer(), builder.GetSize()}) !=
                MOBILEGL_OK) {
            Note(r, "the control frame could not be sent");
            return false;
        }
        r.forged = 1;
        AwaitSessionEnd(client, Wire::kInvalidSeq);
        return true;
    }

    // The client's virtual EGL handles (EGLImpl mints small integers; ServerLoopEglTest's shape).
    EGLDisplay Dpy() { return reinterpret_cast<EGLDisplay>(static_cast<std::uintptr_t>(0x1)); }
    EGLSurface Surf() { return reinterpret_cast<EGLSurface>(static_cast<std::uintptr_t>(0x1)); }
    EGLContext Ctx() { return reinterpret_cast<EGLContext>(static_cast<std::uintptr_t>(0x1)); }

    bool BringUpEgl(PeerReport& r) {
        EGLint major = -1;
        EGLint minor = -1;
        if (!Server::ServerInitializeEGLDisplay(Dpy(), &major, &minor)) return Note(r, "no EGL display"), false;
        if (!Server::ServerCreateEGLPbufferSurface(Surf(), 8, 8)) return Note(r, "no pbuffer"), false;
        if (!Server::ServerMakeEGLCurrent(Dpy(), Surf(), Surf(), Ctx())) return Note(r, "no make-current"), false;
        r.egl = 1;
        return true;
    }

    // SESSION B: Welcome (RunPeer), one record applied, and - with EGL - a clear read back.
    void HealthyNextSession(Client::ClientSession& client, PeerReport& r) {
        P::MGPMemoryBarrier barrier{};
        barrier.Bits = 0x2000u;
        r.applied = Emit(client, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
        if (!BringUpEgl(r)) return;
        // The record-owned inputs a real client sends before its first clear: the default
        // framebuffer's own record, the pack state, the unit-0 windows and the context values
        // (ServerLoopEglTest's ID-49 case's shape). Without the framebuffer record the server
        // refuses the clear by name - it never reads the client's bindings.
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
        // A render state with the GL-initial colour writemask. The server reads the mask from
        // the bound CSO and nowhere else, so without one a clear writes nothing (the mask is
        // the struct's zero, not RenderState's all-true constructor).
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
        Int32 status = Wire::ReplySink::kStatusError;
        Uint64 replySize = 0;
        const Uint64 seq = client.EmitAndWait(P::MGPWireOp::ReadPixels, &read, sizeof(read), nullptr,
                                              0, texel, sizeof(texel), &status, &replySize);
        r.status = status;
        if (seq == Wire::kInvalidSeq || replySize != sizeof(texel)) return Note(r, "no read-back");
        std::memcpy(&r.pixel, texel, sizeof(r.pixel));
    }

    // ------------------------------------------------------------------------------------
    // The table
    // ------------------------------------------------------------------------------------

    enum class Outcome {
        Latched, // PH-1 (3): the session latches the named fault and closes, exit 75
        Fatal,   // a D11 / PH bound that stays a named Fatal: the session child aborts, signal 6
        Refused, // PH-3: a named refusal answered in-band; the session carries on and ends exit 0
    };

    struct Row {
        const char* name;   // gtest parameter name
        const char* file;   // the source file of the site (scripts/ci/ph_latch_sites.py reads it)
        const char* site;   // the quoted word inside the site's Fatal{Family, "word"} marker
        Outcome outcome;
        const char* marker; // must be the first `Fatal{` line of the server log (Latched/Fatal)
        bool needsEgl;      // the forging session needs a current context first
        bool (*forge)(Client::ClientSession&, PeerReport&);
    };

    // Setup helpers the rows share.
    P::MGPBlobRef Stage(Client::ClientSession& client, Uint64 bytes, Uint8 fill = 0x5A) {
        std::vector<Uint8> data(static_cast<std::size_t>(bytes), fill);
        return client.Encoder().StageBytes(data.data(), data.size());
    }

    P::MGPDrawInfo UserIndexDraw() {
        P::MGPDrawInfo info{};
        info.Mode = GL_TRIANGLES;
        info.IndexSize = 2;
        info.Flags = static_cast<Uint8>(P::kDrawHasUserIndices);
        info.InstanceCount = 1;
        info.NumDraws = 1;
        info.MinIndex = info.MaxIndex = ~0u;
        return info;
    }

    // An honest six-byte SEG_STAGE index span, and the range that consumes it.
    MobileGL::MG_Pipe::MGHostSpan IndexSpan(Client::ClientSession& client) {
        const std::vector<Uint8> indices = {0, 0, 1, 0, 2, 0};
        const P::MGPBlobRef staged = client.Encoder().StageBytes(indices.data(), indices.size());
        P::MGHostSpan span{};
        span.Ptr = nullptr;
        span.Seg = staged.Seg;
        span.Size = staged.Size;
        span.Offset = staged.Offset;
        return span;
    }

    bool ForgeUserIndexDraw(Client::ClientSession& client, PeerReport& r,
                            const std::function<void(P::MGPDrawInfo&, P::MGPDrawRange&, P::MGHostSpan&)>& mutate,
                            bool withRange = true) {
        P::MGPDrawInfo info = UserIndexDraw();
        P::MGPDrawRange range{0, 3, 0};
        P::MGHostSpan span = IndexSpan(client);
        mutate(info, range, span);
        std::vector<std::vector<Uint8>> tails;
        tails.push_back(withRange ? Bytes(range) : std::vector<Uint8>{});
        tails.push_back(Bytes(span));
        return Forge(client, r, P::MGPWireOp::DrawVbo, Bytes(info), tails);
    }

    bool CreateTexture(Client::ClientSession& client, P::MGPipeHandle handle, P::MGPipeResourceTarget target,
                       Uint32 internalFormat, Uint32 samples = 1, Uint8 immutable = 0) {
        P::MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(target);
        desc.StorageKind = static_cast<Uint8>(MobileGL::TextureStorageType::Mipmap);
        desc.InternalFormat = internalFormat;
        desc.Width = 4;
        desc.Height = 4;
        desc.Depth = 1;
        desc.ArrayLayers = 1;
        desc.Levels = 1;
        desc.Samples = samples;
        desc.Immutable = immutable;
        Int32 status = Wire::ReplySink::kStatusError;
        const Uint64 seq = client.EmitAndWait(P::MGPWireOp::ResourceCreate, &desc, sizeof(desc), nullptr, 0,
                                              nullptr, 0, &status);
        return seq != Wire::kInvalidSeq && status == Wire::ReplySink::kStatusOk &&
               client.WaitForApplied(seq, 5000) == Transport::SessionWait::Reached;
    }

    P::MGPResourceDesc TextureDesc(P::MGPipeHandle handle, P::MGPipeResourceTarget target, Uint32 format) {
        P::MGPResourceDesc desc{};
        desc.Resource = handle;
        desc.Target = static_cast<Uint8>(target);
        desc.StorageKind = static_cast<Uint8>(MobileGL::TextureStorageType::Mipmap);
        desc.InternalFormat = format;
        desc.Width = desc.Height = 4;
        desc.Depth = desc.ArrayLayers = desc.Levels = desc.Samples = 1;
        return desc;
    }

    bool DeclareLevel0(Client::ClientSession& client, P::MGPResourceDesc desc) {
        desc.HasDefinedContent = 1;
        P::MGPipeSetRespecifiedLevel(
            desc,
            P::MGPipePackSubDataTarget(static_cast<Uint32>(desc.Target),
                                       static_cast<Uint32>(MobileGL::TextureUploadTarget::Texture2D)),
            0, 4, 4, 1);
        Int32 status = Wire::ReplySink::kStatusError;
        const Uint64 seq = client.EmitAndWait(P::MGPWireOp::ResourceRespecify, &desc, sizeof(desc), nullptr,
                                              0, nullptr, 0, &status);
        return seq != Wire::kInvalidSeq && status == Wire::ReplySink::kStatusOk &&
               client.WaitForApplied(seq, 5000) == Transport::SessionWait::Reached;
    }

    P::MGPSubData TextureUpload(Client::ClientSession& client, P::MGPipeHandle handle,
                                P::MGPipeResourceTarget target, MobileGL::TextureUploadTarget upload,
                                Uint64 bytes) {
        P::MGPSubData rec{};
        rec.Res = handle;
        rec.Target = P::MGPipePackSubDataTarget(static_cast<Uint32>(target), static_cast<Uint32>(upload));
        rec.Level = 0;
        rec.UnionBox = P::MGPBox{0, 0, 0, 4, 4, 1};
        rec.Blob = Stage(client, bytes);
        rec.LevelWidth = 4;
        rec.LevelHeight = 4;
        rec.LevelDepth = 1;
        return rec;
    }

    P::MGPFramebufferState ColorFramebuffer(P::MGPipeHandle fbo, P::MGPipeHandle texture, Uint64 hash) {
        P::MGPFramebufferState state{};
        state.Fbo = fbo;
        state.Target = static_cast<Uint8>(P::MGPipeFramebufferTarget::Both);
        state.IsDefault = 0;
        state.Complete = 1;
        state.Width = state.Height = state.Layers = state.Samples = 1;
        state.FixedSampleLocations = 1;
        for (auto& drawBuffer : state.DrawBuffers) drawBuffer = -1;
        state.DrawBuffers[0] = 0;
        state.Color[0].Res = texture;
        state.Color[0].InternalFormat = static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8);
        state.Color[0].Kind = P::kMGPipeSurfaceKindTexture;
        state.Color[0].UploadTarget = static_cast<Uint16>(MobileGL::TextureUploadTarget::Texture2D);
        state.Color[0].TextureTarget = static_cast<Uint16>(MobileGL::TextureTarget::Texture2D);
        state.ContentHash = hash;
        return state;
    }

    P::MGPClear WholeClear(P::MGPipeHandle fbo) {
        P::MGPClear clear{};
        clear.Fbo = fbo;
        clear.Kind = P::kMGPipeClearKindWhole;
        clear.DrawBufferIndex = -1;
        clear.BufferMask = GL_COLOR_BUFFER_BIT;
        clear.ValueClass = P::kMGPipeClearValueClassFloat;
        return clear;
    }

#if MOBILEGL_PIPE_PUSH
    // PH-5's fixture, framed as the wire carries it: a valid archive whose first LinkArtifacts
    // vector count is one element past what the remaining bytes can hold
    // (ProgramArtifactsCodecTest.AVectorCountCannotReservePastTheRemainingArchiveBytes).
    std::vector<Uint8> ArchiveWithAnOversizedVectorCount() {
        using namespace MobileGL::MG_State::GLState;
        MobileGL::Vector<Uint8> codec;
        EncodeProgramArtifacts(LinkArtifacts{}, SpirvArtifacts{}, codec);
        MobileGL::Vector<Uint8> archive;
        EncodeProgramArchive(LinkArtifacts{}, SpirvArtifacts{}, MobileGL::Vector<Uint32>{}, archive);
        const std::size_t prefix = archive.size() - codec.size();
        const std::size_t countOffset = prefix + sizeof(Uint32) + sizeof(Uint64);
        const std::size_t available = archive.size() - countOffset - sizeof(Uint64);
        using Element = std::remove_reference_t<decltype(LinkArtifacts{}.uniformReflection)>::value_type;
        const Uint64 forged = static_cast<Uint64>(available / sizeof(Element)) + 1;
        std::memcpy(archive.data() + countOffset, &forged, sizeof(forged));
        return std::vector<Uint8>(archive.begin(), archive.end());
    }
#endif

    constexpr Uint8 kCreateWindowSurface = static_cast<Uint8>(::MobileGL::Wire::SurfaceOpKind::CreateWindowSurface);

    // ---- the rows ----------------------------------------------------------------------
    //
    // Grouped by file. Each forge returns once its bytes are published and the session is gone
    // (or, for the Refused row, once the in-band answer and a follow-up record were read).

    const Row kRows[] = {
        // ===== PipeWireCodec.cpp: the decoder's pre-gate (the generated gate's two questions)
        {"CodecOpcodeUnknown", "PipeWireCodec.cpp", "opcode", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"opcode\"} got=65535", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPMemoryBarrier b{};
             return Forge(c, r, P::MGPWireOp::MemoryBarrier, Bytes(b), {},
                          [](Transport::RingRecordHeader& h) { h.kind = 0xFFFFu; });
         }},
        {"CodecRecordShorterThanItsType", "PipeWireCodec.cpp", "record.Minimum", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"record.Minimum\"} got=8 expected=16", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPMemoryBarrier b{};
             return Forge(c, r, P::MGPWireOp::MemoryBarrier, Bytes(b), {},
                          [](Transport::RingRecordHeader& h) { h.size = 8; });
         }},
        // ===== PipeWireCodec.cpp: MGPipeWireRecordLayout's count bounds
        {"LayoutRecordSizePastTheWire", "PipeWireCodec.cpp", "record.Size", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"record.Size\"} got=", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSubData s{};
             s.RegionCount = 0x7FFFFFFFu;
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"LayoutSetVertexBuffersCount", "PipeWireCodec.cpp", "SetVertexBuffers.Count", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetVertexBuffers.Count\"} got=33 expected=32", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPVertexBuffers p{};
             p.Start = 0;
             p.Count = 33;
             return Forge(c, r, P::MGPWireOp::SetVertexBuffers, Bytes(p));
         }},
        {"LayoutSetSamplerViewsCount", "PipeWireCodec.cpp", "SetSamplerViews.Count", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetSamplerViews.Count\"} got=193 expected=192", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSamplerViews p{};
             p.Start = 192;
             p.Count = 1;
             return Forge(c, r, P::MGPWireOp::SetSamplerViews, Bytes(p));
         }},
        {"LayoutBindSamplerStatesCount", "PipeWireCodec.cpp", "BindSamplerStates.Count", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"BindSamplerStates.Count\"} got=193 expected=192", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSamplerStates p{};
             p.Start = 192;
             p.Count = 1;
             return Forge(c, r, P::MGPWireOp::BindSamplerStates, Bytes(p));
         }},
        {"LayoutSetShaderImagesCount", "PipeWireCodec.cpp", "SetShaderImages.Count", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetShaderImages.Count\"} got=193 expected=192", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPShaderImages p{};
             p.Start = 192;
             p.Count = 1;
             return Forge(c, r, P::MGPWireOp::SetShaderImages, Bytes(p));
         }},
        {"LayoutSetShaderBuffersClass", "PipeWireCodec.cpp", "SetShaderBuffers.Class", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetShaderBuffers.Class\"} got=3 expected=3", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPShaderBuffers p{};
             p.Class = 3;
             return Forge(c, r, P::MGPWireOp::SetShaderBuffers, Bytes(p));
         }},
        {"LayoutSetShaderBuffersCount", "PipeWireCodec.cpp", "SetShaderBuffers.Count", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetShaderBuffers.Count\"} got=85 expected=84", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPShaderBuffers p{};
             p.Start = 84;
             p.Count = 1;
             return Forge(c, r, P::MGPWireOp::SetShaderBuffers, Bytes(p));
         }},
        {"LayoutSetShaderBuffersHostSpanCount", "PipeWireCodec.cpp", "SetShaderBuffers.HostSpanCount",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"SetShaderBuffers.HostSpanCount\"} got=2 expected=1", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPShaderBuffers p{};
             p.Count = 1;
             p.HostSpanCount = 2;
             return Forge(c, r, P::MGPWireOp::SetShaderBuffers, Bytes(p));
         }},
        {"LayoutSetProgramBindingsBlockBindingCount", "PipeWireCodec.cpp",
         "SetProgramBindings.BlockBindingCount", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetProgramBindings.BlockBindingCount\"} got=65 expected=64", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramBindings p{};
             p.BlockBindingCount = 65;
             return Forge(c, r, P::MGPWireOp::SetProgramBindings, Bytes(p));
         }},
        {"LayoutSetProgramBindingsSamplerUnitCount", "PipeWireCodec.cpp",
         "SetProgramBindings.SamplerUnitCount", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetProgramBindings.SamplerUnitCount\"} got=257 expected=256", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramBindings p{};
             p.SamplerUnitCount = 257;
             return Forge(c, r, P::MGPWireOp::SetProgramBindings, Bytes(p));
         }},
        {"LayoutSetProgramBindingsStorageOverrideCount", "PipeWireCodec.cpp",
         "SetProgramBindings.StorageOverrideCount", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetProgramBindings.StorageOverrideCount\"} got=65 expected=64", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramBindings p{};
             p.StorageOverrideCount = 65;
             return Forge(c, r, P::MGPWireOp::SetProgramBindings, Bytes(p));
         }},
        {"LayoutSetVertexAttribDefaultsCount", "PipeWireCodec.cpp", "SetVertexAttribDefaults.Count",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"SetVertexAttribDefaults.Count\"} got=1 expected=2", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPVertexAttribDefaults p{};
             p.Mask = 0x3u;
             p.Count = 1;
             return Forge(c, r, P::MGPWireOp::SetVertexAttribDefaults, Bytes(p));
         }},
        {"LayoutBufferSubDataResidentRegionCount", "PipeWireCodec.cpp", "BufferSubDataResident.RegionCount",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"BufferSubDataResident.RegionCount\"} got=1 expected=0", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSubData p{};
             p.RegionCount = 1;
             return Forge(c, r, P::MGPWireOp::BufferSubDataResident, Bytes(p));
         }},
        {"LayoutDrawVboFlags", "PipeWireCodec.cpp", "DrawVbo.Flags", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.Flags\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPDrawInfo p{};
             p.Flags = static_cast<Uint8>(P::kDrawHasUserIndices | P::kDrawIsIndirect);
             return Forge(c, r, P::MGPWireOp::DrawVbo, Bytes(p));
         }},
        {"LayoutDrawVboNumDraws", "PipeWireCodec.cpp", "DrawVbo.NumDraws", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.NumDraws\"} got=1 expected=0", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPDrawInfo p{};
             p.Flags = static_cast<Uint8>(P::kDrawIsIndirect);
             p.NumDraws = 1;
             return Forge(c, r, P::MGPWireOp::DrawVbo, Bytes(p));
         }},
        // ===== PipeWireCodec.cpp: the tail cross-check
        {"CodecTailCrossCheck", "PipeWireCodec.cpp", "<op> tail cross-check", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetSamplerViews\"} the record declares Size=", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSamplerViews p{};
             p.Count = 2;
             P::MGPBoundView one{};
             return Forge(c, r, P::MGPWireOp::SetSamplerViews, Bytes(p), {Bytes(one)});
         }},
        // ===== PipeWireCodec.cpp: R-2's honesty arms (CheckBlobIsHonest / RequireDeclaredBlob)
        {"R2BlobSizeZeroDeclaresASegment", "PipeWireCodec.cpp", "<op>.blob Size 0 with Seg/Offset",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"CreateRenderState.blob\"} a blob with Size 0", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.Blob.Seg = 2;
             d.Blob.Offset = 8;
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"R2BlobSizeWithNoSegment", "PipeWireCodec.cpp", "<op>.blob no segment", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"CreateRenderState.blob\"} Size=16 with no segment", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.Blob.Size = 16;
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"R2BlobNotInStage", "PipeWireCodec.cpp", "<op>.blob not SEG_STAGE", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"CreateRenderState.blob\"} seg=3 offset=0 size=16 is not SEG_STAGE", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.Blob.Seg = 3;
             d.Blob.Size = 16;
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"R2BlobOutsideItsSegment", "PipeWireCodec.cpp", "<op>.blob outside the segment", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"CreateRenderState.blob\"} seg=2 offset=1099511627776 size=16 does not lie",
         false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.Blob.Seg = 2;
             d.Blob.Size = 16;
             d.Blob.Offset = Uint64{1} << 40;
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"R2ContentRecordDeclaresNoBlob", "PipeWireCodec.cpp", "<op>.blob undeclared on content",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"CreateRenderState.blob\"} the record's own fields say", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.ChunkMask = P::MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        // ===== PipeWireCodec.cpp: R-2's host-span arms (CheckHostSpanIsHonest), via a user-index draw
        {"R2HostSpanPointerCrosses", "PipeWireCodec.cpp", "host-span", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"host-span\"} MGHostSpan::Ptr is non-null", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(c, r, [](P::MGPDrawInfo&, P::MGPDrawRange&, P::MGHostSpan& s) {
                 s.Ptr = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x1000));
             });
         }},
        {"R2HostSpanSizeWithNoSegment", "PipeWireCodec.cpp", "host-span.seg", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"host-span.seg\"} got=6 expected=0", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(c, r, [](P::MGPDrawInfo&, P::MGPDrawRange&, P::MGHostSpan& s) {
                 s.Seg = 0;
             });
         }},
        {"R2HostSpanSegmentWithNoSize", "PipeWireCodec.cpp", "host-span.size", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"host-span.size\"} got=2 expected=0", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(c, r, [](P::MGPDrawInfo&, P::MGPDrawRange&, P::MGHostSpan& s) {
                 s.Size = 0;
             });
         }},
        {"R2HostSpanOutsideItsSegment", "PipeWireCodec.cpp", "host-span arm 4", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"host-span\"} seg=2 offset=1099511627776 size=6 does not lie", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(c, r, [](P::MGPDrawInfo&, P::MGPDrawRange&, P::MGHostSpan& s) {
                 s.Offset = Uint64{1} << 40;
             });
         }},
        // ===== PipeWireCodec.cpp: CheckDrawUserIndices
        {"UserIndicesShape", "PipeWireCodec.cpp", "DrawVbo.userIndices.shape", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.userIndices.shape\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(
                 c, r, [](P::MGPDrawInfo& info, P::MGPDrawRange&, P::MGHostSpan&) { info.NumDraws = 0; },
                 /*withRange=*/false);
         }},
        {"UserIndicesIndexSize", "PipeWireCodec.cpp", "DrawVbo.userIndices.IndexSize", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.userIndices.IndexSize\"} got=3", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(
                 c, r, [](P::MGPDrawInfo& info, P::MGPDrawRange&, P::MGHostSpan&) { info.IndexSize = 3; });
         }},
        {"UserIndicesStart", "PipeWireCodec.cpp", "DrawVbo.userIndices.Start", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.userIndices.Start\"} got=1", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(
                 c, r, [](P::MGPDrawInfo&, P::MGPDrawRange& range, P::MGHostSpan&) { range.Start = 1; });
         }},
        {"UserIndicesExtent", "PipeWireCodec.cpp", "DrawVbo.userIndices.extent", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"DrawVbo.userIndices.extent\"} got=200 expected=6", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return ForgeUserIndexDraw(
                 c, r, [](P::MGPDrawInfo&, P::MGPDrawRange& range, P::MGHostSpan&) { range.Count = 100; });
         }},
        // ===== PipeWireCodec.cpp: the per-arm size bounds
        {"CreateRenderStateBlobSize", "PipeWireCodec.cpp", "CreateRenderState.Blob", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"CreateRenderState.Blob\"} got=8", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {7u, 1u};
             d.ChunkMask = P::MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
             d.Blob = Stage(c, 8);
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"CreateSamplerStateParametersSize", "PipeWireCodec.cpp", "CreateSamplerState.Parameters",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"CreateSamplerState.Parameters\"} got=", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSamplerDesc d{};
             d.Cso = {5u, 1u};
             d.Parameters = Stage(c, sizeof(MobileGL::SamplerParameters) + 8);
             return Forge(c, r, P::MGPWireOp::CreateSamplerState, Bytes(d));
         }},
        {"CreateShaderStateSpirvDeclared", "PipeWireCodec.cpp", "CreateShaderState.Spirv[%u]",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"CreateShaderState.Spirv[0]\"} declares 16", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramDesc d{};
             d.Cso = {3u, 1u};
             d.Spirv[0] = Stage(c, 16);
             return Forge(c, r, P::MGPWireOp::CreateShaderState, Bytes(d));
         }},
        {"CreateShaderStateReflectionGarbage", "PipeWireCodec.cpp", "CreateShaderState.Reflection",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"CreateShaderState.Reflection\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramDesc d{};
             d.Cso = {3u, 1u};
             d.Reflection = Stage(c, 16, 0xEE);
             return Forge(c, r, P::MGPWireOp::CreateShaderState, Bytes(d));
         }},
        {"SetDynamicStateBlobSize", "PipeWireCodec.cpp", "SetDynamicState.Blob", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetDynamicState.Blob\"} got=", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPDynamicState d{};
             d.ChunkMask = 1u;
             d.Blob = Stage(c, P::MGPipeDynamicChunkBlobBytes(1u) + 8);
             return Forge(c, r, P::MGPWireOp::SetDynamicState, Bytes(d));
         }},
        {"SetProgramBindingsOverrideNameUnterminated", "PipeWireCodec.cpp",
         "SetProgramBindings.StorageOverrides.Name", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetProgramBindings.StorageOverrides.Name\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPProgramBindings p{};
             p.Cso = {3u, 1u};
             p.StorageOverrideCount = 1;
             const std::vector<Uint8> name = {'a', 'b', 'c'};
             const P::MGPBlobRef staged = c.Encoder().StageBytes(name.data(), name.size());
             P::MGPProgramStorageOverride entry{};
             entry.Name.Seg = staged.Seg;
             entry.Name.Size = staged.Size;
             entry.Name.Offset = staged.Offset;
             return Forge(c, r, P::MGPWireOp::SetProgramBindings, Bytes(p), {{}, {}, Bytes(entry)});
         }},
        {"SetResidualValueStateBlobSize", "PipeWireCodec.cpp", "SetResidualValueState.Blob",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"SetResidualValueState.Blob\"} got=16 expected=8", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPResidualValueState d{};
             d.Blob = Stage(c, 16);
             return Forge(c, r, P::MGPWireOp::SetResidualValueState, Bytes(d));
         }},
        {"ResourceSubDataLevelExtent", "PipeWireCodec.cpp", "ResourceSubData.LevelExtent", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"ResourceSubData.LevelExtent\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSubData s = TextureUpload(c, {80u, 1u}, P::MGPipeResourceTarget::Tex2D,
                                             MobileGL::TextureUploadTarget::Texture2D, 64);
             s.LevelWidth = 2; // the dirty box (4 wide) no longer fits the level it names
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"ResourceSubDataUnionBox", "PipeWireCodec.cpp", "ResourceSubData", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"ResourceSubData\"} the union box", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPSubData s = TextureUpload(c, {80u, 1u}, P::MGPipeResourceTarget::Tex2D,
                                             MobileGL::TextureUploadTarget::Texture2D, 64);
             s.LevelWidth = s.LevelHeight = s.LevelDepth = 0;
             s.UnionBox = P::MGPBox{0, 0, 0, 4, 0, 1};
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"SetStorageBlockBindingNameTooLong", "PipeWireCodec.cpp", "SetStorageBlockBinding.Name",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"SetStorageBlockBinding.Name\"} got=5000 expected=4096", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPStorageBlockBinding b{};
             b.Name = Stage(c, 5000, 'x');
             return Forge(c, r, P::MGPWireOp::SetStorageBlockBinding, Bytes(b));
         }},
        {"SetStorageBlockBindingNameUnterminated", "PipeWireCodec.cpp", "SetStorageBlockBinding.Name (NUL)",
         Outcome::Latched, "Fatal{ProtocolCorruption, \"SetStorageBlockBinding.Name\"} the staged block name", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPStorageBlockBinding b{};
             b.Name = Stage(c, 3, 'x');
             return Forge(c, r, P::MGPWireOp::SetStorageBlockBinding, Bytes(b));
         }},
        // ===== PipeApplier.cpp: the seven
        {"PresentFrameSerialZero", "PipeApplier.cpp", "Present.FrameSerial", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"Present.FrameSerial\"}", true,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPPresent p{};
             p.FrameSerial = 0;
             return Forge(c, r, P::MGPWireOp::Present, Bytes(p));
         }},
        {"MultiDrawInstanced", "PipeApplier.cpp", "MultiDraw*+INSTANCED", Outcome::Latched,
         "Fatal{UnmigratedVerb, \"MultiDrawArrays+INSTANCED\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPDrawInfo info{};
             info.Mode = GL_TRIANGLES;
             info.InstanceCount = 2;
             info.NumDraws = 2;
             info.MinIndex = info.MaxIndex = ~0u;
             const P::MGPDrawRange ranges[2] = {{0, 3, 0}, {3, 3, 0}};
             std::vector<Uint8> tail(sizeof(ranges));
             std::memcpy(tail.data(), ranges, sizeof(ranges));
             return Forge(c, r, P::MGPWireOp::DrawVbo, Bytes(info), {tail});
         }},
        {"UnitWindowSamplerViews", "PipeApplier.cpp", "SetSamplerViews.Count (unit window)", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SetSamplerViews.Count\"} - draw_vbo applies", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPContextValues values{};
             values.MaxTouchedTextureUnit = 3;
             if (!Emit(c, P::MGPWireOp::SetContextValues, &values, sizeof(values)))
                 return Note(r, "setup: set_context_values"), false;
             P::MGPSamplerViews views{};
             views.Count = 1;
             const P::MGPBoundView view{};
             if (!Emit(c, P::MGPWireOp::SetSamplerViews, &views, sizeof(views), &view, sizeof(view)))
                 return Note(r, "setup: set_sampler_views"), false;
             P::MGPDrawInfo info{};
             info.Mode = GL_TRIANGLES;
             info.InstanceCount = 1;
             info.NumDraws = 1;
             info.MinIndex = info.MaxIndex = ~0u;
             const P::MGPDrawRange range{0, 3, 0};
             return Forge(c, r, P::MGPWireOp::DrawVbo, Bytes(info), {Bytes(range)});
         }},
        {"UnitWindowSamplerStates", "PipeApplier.cpp", "BindSamplerStates.Count (unit window)", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"BindSamplerStates.Count\"} - draw_vbo applies", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPContextValues values{};
             values.MaxTouchedTextureUnit = 3;
             if (!Emit(c, P::MGPWireOp::SetContextValues, &values, sizeof(values)))
                 return Note(r, "setup: set_context_values"), false;
             P::MGPSamplerViews views{};
             views.Count = 4;
             const P::MGPBoundView four[4] = {};
             if (!Emit(c, P::MGPWireOp::SetSamplerViews, &views, sizeof(views), four, sizeof(four)))
                 return Note(r, "setup: set_sampler_views"), false;
             P::MGPSamplerStates states{};
             states.Count = 1;
             const P::MGPipeHandle one{};
             if (!Emit(c, P::MGPWireOp::BindSamplerStates, &states, sizeof(states), &one, sizeof(one)))
                 return Note(r, "setup: bind_sampler_states"), false;
             P::MGPDrawInfo info{};
             info.Mode = GL_TRIANGLES;
             info.InstanceCount = 1;
             info.NumDraws = 1;
             info.MinIndex = info.MaxIndex = ~0u;
             const P::MGPDrawRange range{0, 3, 0};
             return Forge(c, r, P::MGPWireOp::DrawVbo, Bytes(info), {Bytes(range)});
         }},
        {"ApplierResetSerial", "PipeApplier.cpp", "ApplierReset.ContextSerial", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"ApplierReset.ContextSerial\"} - the record carries 7777", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPApplierReset reset{};
             reset.ContextSerial = 7777;
             return Forge(c, r, P::MGPWireOp::ApplierReset, Bytes(reset));
         }},
        {"ObjectDeathNullHandle", "PipeApplier.cpp", "ObjectDeath.Handle", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"ObjectDeath.Handle\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPHandleOnly death{};
             return Forge(c, r, P::MGPWireOp::ObjectDeath, Bytes(death));
         }},
        {"ObjectDeathKind", "PipeApplier.cpp", "ObjectDeath.Kind", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"ObjectDeath.Kind\"} - 999", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPHandleOnly death{};
             death.Handle = {5u, 1u};
             death.Kind = 999;
             return Forge(c, r, P::MGPWireOp::ObjectDeath, Bytes(death));
         }},
        // ===== ServerLoop.cpp: DrainRing
        {"RingRecordHeaderCorrupt", "ServerLoop.cpp", "SEG_CMD record header", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SEG_CMD record header\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPMemoryBarrier b{};
             return Forge(c, r, P::MGPWireOp::MemoryBarrier, Bytes(b), {},
                          [](Transport::RingRecordHeader& h) { h.size = 12; });
         }},
        // ===== SurfaceOpCodec.cpp: the three wire refusals
        {"SurfaceOpAndroidNativeWindow", "SurfaceOpCodec.cpp", "AndroidNativeWindow@P12", Outcome::Latched,
         "Fatal{UnmigratedSurface, \"AndroidNativeWindow@P12\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return SendSurfaceOp(c, r, kCreateWindowSurface,
                                  static_cast<Uint8>(::MobileGL::Wire::WindowKind::AndroidNativeWindow));
         }},
        {"SurfaceOpMetalLayer", "SurfaceOpCodec.cpp", "MetalLayer@P12", Outcome::Latched,
         "Fatal{UnmigratedSurface, \"MetalLayer@P12\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             return SendSurfaceOp(c, r, kCreateWindowSurface,
                                  static_cast<Uint8>(::MobileGL::Wire::WindowKind::MetalLayer));
         }},
        {"SurfaceOpUnknownKind", "SurfaceOpCodec.cpp", "SurfaceOp", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"SurfaceOp\"} - a wire surface op failed validation: UnknownOpKind", false,
         [](Client::ClientSession& c, PeerReport& r) { return SendSurfaceOp(c, r, 200, 0); }},
        // ===== the D11 / PH bounds F2 landed (driver's rows, now with a next session)
        {"D11CreateRenderStateCsoSlot", "PipeApply.cpp", "CreateRenderState.Cso.Slot", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"CreateRenderState.Cso.Slot\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPRenderStateDesc d{};
             d.Cso = {P::kMGPipeMaxRenderStateCsoSlots, 1u};
             d.ChunkMask = P::MGPipeRenderStateChunkDetail::kAllPipelineHalfBits;
             d.Blob = Stage(c, P::MGPipePipelineChunkBlobBytes(d.ChunkMask));
             return Forge(c, r, P::MGPWireOp::CreateRenderState, Bytes(d));
         }},
        {"D11ReadbackPastTheReplyLimit", "PipeApplier.cpp", "PH-3 read_pixels bound (kStatusError)",
         Outcome::Refused, nullptr, false,
         [](Client::ClientSession& c, PeerReport& r) {
             P::MGPReadbackInfo info{};
             info.Box = P::MGPBox{0, 0, 0, 1, 1, 1};
             info.Format = GL_RGBA;
             info.Type = GL_UNSIGNED_BYTE;
             const auto width = static_cast<Uint32>(c.MaxReplyBytes() / 4 + 1);
             info.Box.W = width;
             info.DstSize = static_cast<Uint64>(width) * 4;
             Uint64 seq = Wire::kInvalidSeq;
             if (!PublishImage(c, RecordImage(P::MGPWireOp::ReadPixels, Bytes(info)), &seq))
                 return Note(r, "publish"), false;
             r.forged = 1;
             if (c.WaitForApplied(seq, 5000) != Transport::SessionWait::Reached) return Note(r, "not applied"), false;
             Int32 status = Wire::ReplySink::kStatusDeclined;
             Uint64 size = 99;
             if (!c.ReadReply(seq, nullptr, 0, &status, &size)) return Note(r, "no reply"), false;
             r.status = status;
             P::MGPMemoryBarrier barrier{};
             barrier.Bits = 0x2000u;
             r.applied = Emit(c, P::MGPWireOp::MemoryBarrier, &barrier, sizeof(barrier)) ? 1 : 0;
             return true;
         }},
#if MOBILEGL_PIPE_PUSH
        {"D11ArchiveVectorCount", "PipeWireCodec.cpp", "CreateShaderState.Reflection (PH-5)", Outcome::Latched,
         "Fatal{ProtocolCorruption, \"CreateShaderState.Reflection\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const std::vector<Uint8> archive = ArchiveWithAnOversizedVectorCount();
             P::MGPProgramDesc d{};
             d.Cso = {3u, 1u};
             d.Reflection = c.Encoder().StageBytes(archive.data(), archive.size());
             return Forge(c, r, P::MGPWireOp::CreateShaderState, Bytes(d));
         }},
#endif
        {"D11StagedRunPastTheLevelBound", "StagedTextureStore.h", "StagedTextureStore.CopyRunInto",
         Outcome::Fatal, "Fatal{ProtocolCorruption, \"StagedTextureStore.CopyRunInto\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{73u, 1u};
             const auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D,
                                           static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2D, desc.InternalFormat) ||
                 !DeclareLevel0(c, desc))
                 return Note(r, "setup"), false;
             P::MGPSubData s = TextureUpload(c, tex, P::MGPipeResourceTarget::Tex2D,
                                             MobileGL::TextureUploadTarget::Texture2D, 96);
             s.UnionBox = P::MGPBox{0, 1, 0, 4, 3, 1};
             s.RegionCount = 1;
             P::MGPSubRegion region{};
             region.Y = 1;
             region.W = 4;
             region.H = 3;
             region.D = 1;
             region.SrcRowStride = 32;
             region.SrcSliceStride = 96;
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s), {Bytes(region)});
         }},
        {"D11StagedExtentDisagrees", "StagedTextureStore.h", "StagedTextureStore.LevelExtent", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"StagedTextureStore.LevelExtent\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{74u, 1u};
             const auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D,
                                           static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2D, desc.InternalFormat) ||
                 !DeclareLevel0(c, desc))
                 return Note(r, "setup"), false;
             P::MGPSubData s = TextureUpload(c, tex, P::MGPipeResourceTarget::Tex2D,
                                             MobileGL::TextureUploadTarget::Texture2D, 64);
             s.LevelWidth = 8;
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"D11StagedMultisampleTarget", "StagedTextureStore.h", "StagedTextureStore.Target", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"StagedTextureStore.Target\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{75u, 1u};
             auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2DMS,
                                     static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             desc.Samples = 4;
             desc.Immutable = 1;
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2DMS, desc.InternalFormat, 4, 1))
                 return Note(r, "setup: create"), false;
             desc.HasDefinedContent = 1;
             Int32 status = Wire::ReplySink::kStatusError;
             const Uint64 seq = c.EmitAndWait(P::MGPWireOp::ResourceRespecify, &desc, sizeof(desc), nullptr, 0,
                                              nullptr, 0, &status);
             if (seq == Wire::kInvalidSeq || c.WaitForApplied(seq, 5000) != Transport::SessionWait::Reached)
                 return Note(r, "setup: respecify"), false;
             P::MGPSubData s = TextureUpload(c, tex, P::MGPipeResourceTarget::Tex2DMS,
                                             MobileGL::TextureUploadTarget::Texture2DMultisample, 64);
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"D11StagedBytelessLevel", "StagedTextureStore.h", "StagedTextureStore.LevelBound", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"StagedTextureStore.LevelBound\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{76u, 1u};
             const auto unknown = static_cast<Uint32>(MobileGL::TextureInternalFormat::Unknown);
             const auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D, unknown);
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2D, unknown) || !DeclareLevel0(c, desc))
                 return Note(r, "setup"), false;
             P::MGPSubData s = TextureUpload(c, tex, P::MGPipeResourceTarget::Tex2D,
                                             MobileGL::TextureUploadTarget::Texture2D, 64);
             return Forge(c, r, P::MGPWireOp::ResourceSubData, Bytes(s));
         }},
        {"D11RespecifyExtentCarrier", "PipeApply.cpp", "ResourceRespecify.ExtentCarrier", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"ResourceRespecify.ExtentCarrier\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{77u, 1u};
             auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D,
                                     static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2D, desc.InternalFormat))
                 return Note(r, "setup"), false;
             desc.HasDefinedContent = 1;
             P::MGPipeSetRespecifiedLevel(
                 desc,
                 P::MGPipePackSubDataTarget(static_cast<Uint32>(desc.Target),
                                            static_cast<Uint32>(MobileGL::TextureUploadTarget::Texture2D)),
                 0, 4, 4, 1);
             desc.BufSize |= Uint64{1} << 32;
             return Forge(c, r, P::MGPWireOp::ResourceRespecify, Bytes(desc));
         }},
        {"D11WholeRespecifyBufferRange", "PipeApply.cpp", "ResourceRespecify.BufferRange", Outcome::Fatal,
         "Fatal{ProtocolCorruption, \"ResourceRespecify.BufferRange\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{78u, 1u};
             auto desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D,
                                     static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             if (!CreateTexture(c, tex, P::MGPipeResourceTarget::Tex2D, desc.InternalFormat))
                 return Note(r, "setup"), false;
             desc.HasDefinedContent = 1;
             desc.BufOffset = 1;
             return Forge(c, r, P::MGPWireOp::ResourceRespecify, Bytes(desc));
         }},
        {"D11SlotPastTheBackendBound", "SlotTables.h / Managers.cpp", "BackendSlotTable.HandleSlot",
         Outcome::Fatal, "Fatal{ProtocolCorruption, \"BackendSlotTable.HandleSlot\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle fbo{20u, 1u};
             const auto fb = ColorFramebuffer(fbo, P::MGPipeHandle{1u << 20, 1u}, 1);
             if (!Emit(c, P::MGPWireOp::SetFramebufferState, &fb, sizeof(fb))) return Note(r, "setup"), false;
             return Forge(c, r, P::MGPWireOp::Clear, Bytes(WholeClear(fbo)));
         }},
        {"D11SlotGenerationBackwards", "SlotTables.h / Managers.cpp", "BackendSlotTable.Generation",
         Outcome::Fatal, "Fatal{ProtocolCorruption, \"BackendSlotTable.Generation\"}", false,
         [](Client::ClientSession& c, PeerReport& r) {
             const P::MGPipeHandle tex{37u, 2u};
             P::MGPResourceDesc desc = TextureDesc(tex, P::MGPipeResourceTarget::Tex2D,
                                                   static_cast<Uint32>(MobileGL::TextureInternalFormat::RGBA8));
             desc.BindMask = P::kMGPipeBindRenderTarget;
             desc.Width = desc.Height = 1;
             Int32 status = Wire::ReplySink::kStatusError;
             const Uint64 seq = c.EmitAndWait(P::MGPWireOp::ResourceCreate, &desc, sizeof(desc), nullptr, 0,
                                              nullptr, 0, &status);
             if (seq == Wire::kInvalidSeq || c.WaitForApplied(seq, 5000) != Transport::SessionWait::Reached)
                 return Note(r, "setup: create"), false;
             const P::MGPipeHandle fbo{20u, 1u};
             auto fb = ColorFramebuffer(fbo, tex, 1);
             const auto clear = WholeClear(fbo);
             if (!Emit(c, P::MGPWireOp::SetFramebufferState, &fb, sizeof(fb)) ||
                 !Emit(c, P::MGPWireOp::Clear, &clear, sizeof(clear)))
                 return Note(r, "setup: live twin"), false;
             fb.Color[0].Res.Gen = 1;
             fb.ContentHash = 2;
             if (!Emit(c, P::MGPWireOp::SetFramebufferState, &fb, sizeof(fb))) return Note(r, "setup: stale"), false;
             return Forge(c, r, P::MGPWireOp::Clear, Bytes(clear));
         }},
    };

    // ------------------------------------------------------------------------------------
    // One row, end to end
    // ------------------------------------------------------------------------------------

    std::string ReapExpectation(Outcome outcome) {
        switch (outcome) {
        case Outcome::Latched:
            return "exit=" + std::to_string(kSessionLatchedExitCode) + " (latched fault)";
        case Outcome::Fatal: return "signal=6";
        case Outcome::Refused: return "exit=0 ";
        }
        return "?";
    }

    // The two-session walk every row and the PH-1 (4) headline share. `forgeName` labels the
    // assertions.
    void RunRowAgainst(Supervisor& sup, const char* forgeName, Outcome outcome, const char* marker,
                       bool needsEgl, bool (*forge)(Client::ClientSession&, PeerReport&)) {
        // ---- session A: the forged bytes
        const PeerReport a = RunPeer(
            sup,
            [&](Client::ClientSession& client, PeerReport& r) {
                if (needsEgl && !BringUpEgl(r)) return;
                (void)forge(client, r);
            },
            /*stopCleanly=*/outcome == Outcome::Refused);
        ASSERT_EQ(a.started, 1) << forgeName << ": session A never got Welcome (" << a.note << ")";
        if (needsEgl && a.egl == 0) {
            GTEST_SKIP() << forgeName << ": the row needs a current context and this host has no "
                                         "headless EGL (" << a.note << ")";
        }
        ASSERT_EQ(a.forged, 1) << forgeName << ": the peer could not publish its bytes (" << a.note << ")";
        ASSERT_GT(a.serverPid, 0u);

        // ---- the reap: how A's session ended, in the supervisor's own words
        std::string reap;
        ASSERT_TRUE(WaitFor([&] { return !(reap = sup.ReapLine(a.serverPid)).empty(); }, 15000))
            << forgeName << ": the supervisor never reaped session pid " << a.serverPid << "\n"
            << sup.Log();
        EXPECT_NE(reap.find(ReapExpectation(outcome)), std::string::npos)
            << forgeName << ": session A ended as `" << reap << "`, expected `" << ReapExpectation(outcome) << "`";

        // ---- the line: the FIRST named fault is this row's own
        const std::string log = sup.Log();
        const auto firstFatal = log.find("Fatal{");
        if (outcome == Outcome::Refused) {
            EXPECT_EQ(firstFatal, std::string::npos) << forgeName << ": a refusal must not end the session\n" << log;
            EXPECT_EQ(a.status, Wire::ReplySink::kStatusError) << forgeName;
            EXPECT_EQ(a.applied, 1) << forgeName << ": the session did not carry on after the refusal";
        } else {
            ASSERT_NE(firstFatal, std::string::npos) << forgeName << ": no named fault at all\n" << log;
            const auto lineEnd = log.find('\n', firstFatal);
            const std::string firstLine = log.substr(firstFatal, lineEnd - firstFatal);
            EXPECT_NE(firstLine.find(marker), std::string::npos)
                << forgeName << ": the first named fault is not this row's:\n  first: " << firstLine
                << "\n  wanted: " << marker;
            const bool latched = log.find("SessionLatch{") != std::string::npos;
            EXPECT_EQ(latched, outcome == Outcome::Latched)
                << forgeName << ": SessionLatch line presence disagrees with the row's outcome";
        }

        // ---- session B: the same supervisor serves the next connection, and it renders
        const PeerReport b = RunPeer(sup, HealthyNextSession, /*stopCleanly=*/true);
        ASSERT_EQ(b.started, 1) << forgeName << ": the next connection got no Welcome (" << b.note << ")";
        EXPECT_NE(b.serverPid, a.serverPid) << forgeName << ": the next session is not a new session child";
        EXPECT_EQ(b.applied, 1) << forgeName << ": the next session did not apply a record";
        std::string reapB;
        EXPECT_TRUE(WaitFor([&] { return !(reapB = sup.ReapLine(b.serverPid)).empty(); }, 15000))
            << forgeName << ": the next session was never reaped";
        EXPECT_NE(reapB.find("exit=0 "), std::string::npos)
            << forgeName << ": the next session did not end cleanly: `" << reapB << "`";
        if (b.egl == 0) {
            GTEST_SKIP() << forgeName << ": latch and next-session Welcome proven; the render half "
                                         "needs headless EGL, absent here (" << b.note << ")";
        }
        EXPECT_EQ(b.status, Wire::ReplySink::kStatusOk) << forgeName << ": " << b.note;
        EXPECT_EQ(b.pixel, 0xFF00FF00u) << forgeName << ": the next session's clear did not read back green";
    }

    class PeerLatchSite : public ::testing::TestWithParam<Row> {};

} // namespace

// FUZZ ARM 2, ONE ROW PER SITE. See the file header; the row's own fields say what it proves.
TEST_P(PeerLatchSite, TheForgedBytesEndTheSessionByNameAndTheSupervisorServesARenderingNextSession) {
    const Row& row = GetParam();
    Supervisor sup;
    ASSERT_TRUE(Launch(row.name, /*tcp=*/false, &sup)) << "the --serve supervisor did not start";
    RunRowAgainst(sup, row.name, row.outcome, row.marker, row.needsEgl, row.forge);
}

INSTANTIATE_TEST_SUITE_P(FuzzArm2, PeerLatchSite, ::testing::ValuesIn(kRows),
                         [](const ::testing::TestParamInfo<Row>& info) { return std::string(info.param.name); });

// PH-1 (4), THE HEADLINE: a malformed frame latches the session, and a new connection to the
// SAME supervisor gets Welcome and renders - over the unix shm pair and over TCP stream, the two
// `--serve` shapes a deployment runs. The frame is a ring record with an opcode no row names.
namespace {
    bool ForgeUnknownOpcode(Client::ClientSession& c, PeerReport& r) {
        P::MGPMemoryBarrier b{};
        return Forge(c, r, P::MGPWireOp::MemoryBarrier, Bytes(b), {},
                     [](Transport::RingRecordHeader& h) { h.kind = 0xFFFFu; });
    }
} // namespace

TEST(PeerLatchTest, AMalformedRecordLatchesTheSessionAndTheNextUnixConnectionGetsWelcomeAndRenders) {
    Supervisor sup;
    ASSERT_TRUE(Launch("headline-unix", /*tcp=*/false, &sup));
    RunRowAgainst(sup, "headline-unix", Outcome::Latched, "Fatal{ProtocolCorruption, \"opcode\"} got=65535",
                  false, &ForgeUnknownOpcode);
}

TEST(PeerLatchTest, AMalformedRecordLatchesTheSessionAndTheNextTcpConnectionGetsWelcomeAndRenders) {
    Supervisor sup;
    ASSERT_TRUE(Launch("headline-tcp", /*tcp=*/true, &sup));
    RunRowAgainst(sup, "headline-tcp", Outcome::Latched, "Fatal{ProtocolCorruption, \"opcode\"} got=65535",
                  false, &ForgeUnknownOpcode);
}

// A malformed CONTROL frame over TCP: the SurfaceOp arm latches on the main thread (RunSession's
// loop), not the apply thread, and the next connection is served all the same.
TEST(PeerLatchTest, AMalformedControlFrameOverTcpLatchesAndTheNextConnectionRenders) {
    Supervisor sup;
    ASSERT_TRUE(Launch("headline-tcp-ctrl", /*tcp=*/true, &sup));
    RunRowAgainst(sup, "headline-tcp-ctrl", Outcome::Latched,
                  "Fatal{ProtocolCorruption, \"SurfaceOp\"} - a wire surface op failed validation: UnknownOpKind",
                  false, [](Client::ClientSession& c, PeerReport& r) { return SendSurfaceOp(c, r, 200, 0); });
}

#endif // !_WIN32
