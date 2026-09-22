// MobileGL - separate-process server and TCP session supervisor.
// SPDX-License-Identifier: LGPL-3.0-only
#include "../Transport/Doorbell.h"
#include "../Transport/SocketTransport.h"
#include "../Transport/WireLog.h"
#include "../Protocol/SurfaceOpCodec.h"
#include "ServerLoop.h"
#include "ServerSession.h"
#include "SurfaceControlFrame.h"
#include <Config.h>
#include <Init.h>
#include <MG_Backend/MGPipe/PipeInputs.h>
#include <MG_Backend/ServerRole.h>
#include <MG_Util/Debug/Log.h>
#include <MG_Util/Metrics/PipeStats.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace MobileGL::MG_Remote;
using namespace MobileGL::MG_Remote::Transport;
namespace Protocol = ::MobileGL::Wire;

void Refuse(SocketTransport& transport, Protocol::RefuseCode code, const char* detail) {
    flatbuffers::FlatBufferBuilder builder(256);
    auto refusal = Protocol::CreateRefuseDirect(builder, code, detail);
    auto envelope = Protocol::CreateCtrlEnvelope(builder, Protocol::CtrlMsg::Refuse, refusal.Union());
    Protocol::FinishCtrlEnvelopeBuffer(builder, envelope);
    transport.SendFrame({builder.GetBufferPointer(), builder.GetSize()});
    WireLogError("MG_Remote server: Refuse{%s} %s", Protocol::EnumNameRefuseCode(code), detail);
}

void ForwardLog(void* user, const char* text) {
    auto& transport = *static_cast<ITransport*>(user);
    flatbuffers::FlatBufferBuilder builder(512);
    auto log = Protocol::CreateLogLine(builder, Protocol::LogLevel::Info, builder.CreateString(text));
    auto envelope = Protocol::CreateCtrlEnvelope(builder, Protocol::CtrlMsg::LogLine, log.Union());
    Protocol::FinishCtrlEnvelopeBuffer(builder, envelope);
    (void)transport.SendFrame({builder.GetBufferPointer(), builder.GetSize()});
}

struct FlushAck { ITransport* transport; std::uint64_t seq; };
void SendLogAck(void* pointer) {
    auto& ack = *static_cast<FlushAck*>(pointer);
    flatbuffers::FlatBufferBuilder builder(64);
    auto flush = Protocol::CreateLogFlush(builder, ack.seq, true);
    auto envelope = Protocol::CreateCtrlEnvelope(builder, Protocol::CtrlMsg::LogFlush, flush.Union());
    Protocol::FinishCtrlEnvelopeBuffer(builder, envelope);
    (void)ack.transport->SendFrame({builder.GetBufferPointer(), builder.GetSize()});
}

// Called only after fork: the supervisor never creates a backend or EGL context.
[[noreturn]] void RunSession(std::unique_ptr<SocketTransport> control) {
    const int selfPid = static_cast<int>(::getpid());
    const bool tcp = control->IsTcp();
    std::vector<std::uint8_t> helloFrame;
    std::uint64_t bytes = 0;
    auto received = control->ReceiveFrame({nullptr, 0}, &bytes, 10000);
    if (received != MOBILEGL_ERR_BUFFER_TOO_SMALL || bytes > 1024 * 1024) ::_exit(67);
    helloFrame.resize(static_cast<std::size_t>(bytes));
    if (control->ReceiveFrame({helloFrame.data(), helloFrame.size()}, &bytes, 0) != MOBILEGL_OK) ::_exit(67);
    flatbuffers::Verifier verifier(helloFrame.data(), helloFrame.size());
    if (!Protocol::VerifyCtrlEnvelopeBuffer(verifier)) ::_exit(67);
    const auto* hello = Protocol::GetCtrlEnvelope(helloFrame.data())->msg_as_Hello();
    if (!hello) ::_exit(67);
    if (tcp) {
        const char* expected = std::getenv("MOBILEGL_IPC_TOKEN");
        const std::string token = hello->token() ? hello->token()->str() : std::string();
        if (token != (expected ? expected : "")) {
            Refuse(*control, Protocol::RefuseCode::Authentication, "TCP token mismatch");
            ::_exit(0);
        }
    }
    MobileGL::MG_ConfigLoader::Init();
    MobileGL::MG_Config::Transport = MobileGL::MG_Config::TransportMode::Spawn;
    MobileGL::MG_Pipe::MGPipeSetServerProcessRole(true);
    // The child bypasses MobileGL::Initialize: arm its own role counters.
    MobileGL::MG_Util::PipeStats::Init();
    const auto backend = static_cast<MobileGL::BackendType>(hello->backendType());
    if (backend != MobileGL::BackendType::DirectGLES && backend != MobileGL::BackendType::DirectVulkan) {
        Refuse(*control, Protocol::RefuseCode::Backend, "unsupported Hello.backendType");
        ::_exit(0);
    }
    const char* pinned = std::getenv("MOBILEGL_BACKEND_TYPE");
    if (pinned && *pinned && backend != MobileGL::MG_Config::ActiveBackendType) {
        Refuse(*control, Protocol::RefuseCode::Backend, "Hello.backendType disagrees with pinned backend");
        ::_exit(0);
    }
    MobileGL::MG_Config::ActiveBackendType = backend;
    if (!MobileGL::MG_Backend::InitServerRoleForSpawn()) ::_exit(66);
    auto& session = Server::ServerSessionInstance();
    int serverBell[2] = {-1, -1}, clientBell[2] = {-1, -1};
    std::unique_ptr<SocketDoorbell> selfBell, peerBell;
    if (!tcp) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, serverBell) != 0 ||
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, clientBell) != 0) ::_exit(74);
        selfBell = std::make_unique<SocketDoorbell>(serverBell[0], serverBell[1], 1, true);
        peerBell = std::make_unique<SocketDoorbell>(-1, clientBell[1], 1, true);
        session.SetExternalDoorbells(selfBell.get(), peerBell.get());
    }
    const auto accepted = session.Accept(*control, &helloFrame);
    if (accepted != MOBILEGL_OK) {
        WireLogError("MG_Remote server: pid=%d Accept failed (rc=%d)", selfPid, static_cast<int>(accepted));
        std::fflush(nullptr);
        ::_exit(accepted == MOBILEGL_ERR_PROTOCOL_MISMATCH ? 0 : 67);
    }
    if (!tcp)
    {
        struct Sideband {
            std::uint32_t slot;
            std::uint64_t bytes;
        };
        using Slot = SessionSegmentSlot;
        static constexpr Slot kSlots[4] = {Slot::Cmd, Slot::Stage, Slot::Reply, Slot::Event};
        for (const Slot slot : kSlots) {
            const std::uint32_t tag = static_cast<std::uint32_t>(slot);
            const int fd = session.Shm().DescriptorFor(slot);
            if (fd < 0) {
                std::fprintf(stderr, "MG_Remote server: pid=%d segment %u has no descriptor\n",
                             selfPid, tag);
                std::fflush(nullptr);
                ::_exit(68);
            }
            Sideband sideband{tag, session.Shm().AnnouncedSize(slot)};
            if (control->ShareFd(fd, MobileGLByteSpan{&sideband, sizeof(sideband)}) !=
                MOBILEGL_OK) {
                std::fprintf(stderr, "MG_Remote server: pid=%d could not share segment %u\n",
                             selfPid, tag);
                std::fflush(nullptr);
                ::_exit(69);
            }
        }
        // SLOTS 4, 5 AND 6 ARE THE BELLS - three, not two, and the third is the
        // one the two-fd form above forces:
        //   4  serverBell[1]  the client's handle to RING THE SERVER
        //   5  clientBell[0]  the client's PARK end
        //   6  clientBell[1]  the client's handle to ring ITSELF, for the same
        //                     reason the server needs serverBell[1]
        // Sharing DUPs, so the descriptors these bells own stay valid here.
        const int bellFds[3] = {serverBell[1], clientBell[0], clientBell[1]};
        for (std::uint32_t index = 0; index < 3; ++index) {
            Sideband sideband{4 + index, 0};
            if (control->ShareFd(bellFds[index], MobileGLByteSpan{&sideband, sizeof(sideband)}) !=
                MOBILEGL_OK) {
                std::fprintf(stderr, "MG_Remote server: pid=%d could not share bell %u\n", selfPid,
                             index);
                std::fflush(nullptr);
                ::_exit(69);
            }
        }
        // clientBell[0] is the CLIENT's park end and no bell here owns it; the
        // other three belong to selfBell and peerBell and are closed with them.
        ::close(clientBell[0]);
    }

    const char* forwarding = std::getenv("MOBILEGL_IPC_LOG_FORWARD");
    if (tcp && (!forwarding || std::strcmp(forwarding, "0") != 0))
        MobileGL::MG_Util::Debug::SetLogForwarder(&ForwardLog, control.get());
    auto& loop = Server::ServerLoopInstance();
    if (loop.Start(session) != MOBILEGL_OK) ::_exit(70);
    WireLogError("MG_Remote server: pid=%d transport=spawn role=server ready control=%s data=%s",
                 selfPid, tcp ? "tcp" : "unix", tcp ? "stream" : "shm");
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        std::uint64_t size = 0;
        const auto result = control->ReceiveFrame({buffer.data(), buffer.size()}, &size, kWaitForever);
        if (result == MOBILEGL_ERR_BUFFER_TOO_SMALL) { buffer.resize(static_cast<std::size_t>(size)); continue; }
        if (result != MOBILEGL_OK) break;
        flatbuffers::Verifier check(buffer.data(), static_cast<std::size_t>(size));
        if (!Protocol::VerifyCtrlEnvelopeBuffer(check)) break;
        const auto* envelope = Protocol::GetCtrlEnvelope(buffer.data());
        if (const auto* flush = envelope->msg_as_LogFlush()) {
            if (flush->ack()) break;
            FlushAck ack{control.get(), flush->seq()};
            MobileGL::MG_Util::Debug::WithLogBarrier(&SendLogAck, &ack);
            continue;
        }
        const auto* operation = envelope->msg_as_SurfaceOp();
        if (!operation) break;
        Server::SurfaceControlFrame reply{};
        (void)ServerApplyWireSurfaceOp(*operation, &reply);
        session.FlushDataProgress();
        if (session.DataLink()) reply.eventHead = session.DataLink()->EventPublishedHead();
        flatbuffers::FlatBufferBuilder builder(256);
        EncodeSurfaceReplyFrame(reply, &builder);
        if (control->SendFrame({builder.GetBufferPointer(), builder.GetSize()}) != MOBILEGL_OK) break;
    }
    loop.Stop();
    // Publish the final server window while log forwarding is still attached.
    MobileGL::MG_Util::PipeStats::Shutdown();
    session.Close();
    MobileGL::MG_Util::Debug::SetLogForwarder(nullptr, nullptr);
    control->Shutdown();
    std::fflush(nullptr);
    ::_exit(0);
}
}

extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv) {
    const char* dial = std::getenv("MOBILEGL_IPC_DIAL");
    if (!dial || std::strcmp(dial, "no") != 0) {
        std::fprintf(stderr, "MG_Remote server: MOBILEGL_IPC_DIAL=no anti-recursion catch (a) missing\n");
        return 64;
    }
    for (const char* name : {"MOBILEGL_TRANSPORT", "MOBILEGL_IPC_SERVER_PATH", "MOBILEGL_IPC_RING_MB", "MOBILEGL_IPC_STAGE_MB"}) {
        if (std::getenv(name)) {
            std::fprintf(stderr, "MG_Remote server: envp scrub FAILED, catch (b): %s\n", name);
            return 65;
        }
    }
    std::string endpoint;
    bool serve = false;
    unsigned maxSessions = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--serve") serve = true;
        else if (arg == "--max-sessions" && i + 1 < argc) {
            char* end = nullptr;
            const auto count = std::strtoul(argv[++i], &end, 10);
            if (*end || count == 0 || count > 1000000) return 71;
            maxSessions = static_cast<unsigned>(count);
        } else if (endpoint.empty() && arg.compare(0, 2, "--") != 0) endpoint = arg;
        else return 71;
    }
    if (endpoint.empty()) if (const char* value = std::getenv("MOBILEGL_IPC_ENDPOINT")) endpoint = value;
    if (endpoint.empty()) return 71;
    int listener = -1;
    if (SocketTransport::Listen(endpoint, &listener) != MOBILEGL_OK) return 72;
    WireLogError("MG_Remote server: pid=%d listening on %s", static_cast<int>(::getpid()), endpoint.c_str());
    pid_t active = -1;
    unsigned sessions = 0;
    for (;;) {
        if (active > 0 && ::waitpid(active, nullptr, WNOHANG) == active) active = -1;
        if (maxSessions && sessions >= maxSessions && active < 0) break;
        std::unique_ptr<SocketTransport> control;
        const auto accepted = SocketTransport::AcceptPair(listener, serve ? 250 : 30000, control);
        if (accepted == MOBILEGL_ERR_TIMEOUT && serve) continue;
        if (accepted != MOBILEGL_OK) { ::close(listener); return 73; }
        if (!serve) {
            ::close(listener);
            if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
            RunSession(std::move(control));
        }
        if (active > 0 && ::waitpid(active, nullptr, WNOHANG) == active) active = -1;
        if (active > 0 || (maxSessions && sessions >= maxSessions)) {
            Refuse(*control, Protocol::RefuseCode::Busy, "one session is already active");
            continue;
        }
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) { ::close(listener); RunSession(std::move(control)); }
        if (child < 0) { Refuse(*control, Protocol::RefuseCode::Busy, "could not create session child"); continue; }
        active = child;
        ++sessions;
        control->CloseLocalCopy();
    }
    ::close(listener);
    if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
    return 0;
}
