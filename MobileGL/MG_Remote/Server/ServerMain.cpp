// MobileGL - separate-process server and TCP session supervisor.
// SPDX-License-Identifier: LGPL-3.0-only
#include "../Transport/Doorbell.h"
#include "../Transport/SocketTransport.h"
#include "../Transport/WireLog.h"
#include "../Protocol/SurfaceOpCodec.h"
#include "../Handshake.h"
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
#include <chrono>
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

void RefuseBusy(SocketTransport& transport, const char* detail) {
    // Unread Hello bytes can make close send RST and discard our Refuse.
    std::uint64_t bytes = 0;
    if (transport.ReceiveFrame({nullptr, 0}, &bytes, 2000) == MOBILEGL_ERR_BUFFER_TOO_SMALL &&
        bytes <= 1024 * 1024) {
        std::vector<std::uint8_t> hello(static_cast<std::size_t>(bytes));
        (void)transport.ReceiveFrame({hello.data(), hello.size()}, &bytes, 0);
    }
    Refuse(transport, Protocol::RefuseCode::Busy, detail);
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
    // Reject incompatible wire before backend bring-up can obscure the cause.
    if (ValidatePeerHandshake(*control, hello->abiMajor(), hello->abiMinor(),
            hello->wireFingerprint(), hello->buildFingerprint() ? hello->buildFingerprint()->c_str() : nullptr,
            hello->dialMode()) != MOBILEGL_OK) ::_exit(0);
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
        // THE FILE IDENTIFIER, ASKED FIRST AND SAID OUT LOUD (P7 wave 0).
        //
        // The plan asked for CtrlEnvelopeBufferHasIdentifier here because ServerSession::
        // ParseEnvelope (:109) asks it and this loop did not. MEASURED, THE PREMISE IS WRONG:
        // the generated VerifyCtrlEnvelopeBuffer is
        // `verifier.VerifyBuffer<CtrlEnvelope>(CtrlEnvelopeIdentifier())`
        // (protocol_generated.h:2378), so a wrong identifier was ALREADY refused - and so is
        // ParseEnvelope's second call. Recorded for the integrator: the gap that row names does
        // not exist.
        //
        // What DID exist is that both refusals were silent. A peer whose post-Welcome frame was
        // garbage got its session ended with no line in the log at all, which is the half of a
        // fuzz arm that matters - an arm that cannot tell "refused, by name" from "the child
        // went away" measures nothing. So the identifier is now asked FIRST, because it is the
        // specific diagnosis ("that is not our schema") where the verifier's is the general one,
        // and both say so. Asking it first needs the length guard the verifier would otherwise
        // have provided: BufferHasIdentifier reads bytes [4, 8).
        if (size < 8) {
            WireLogError("MG_Remote server: control frame is %llu bytes, too short to carry a "
                         "CtrlEnvelope root and identifier; ending the session",
                         static_cast<unsigned long long>(size));
            break;
        }
        if (!Protocol::CtrlEnvelopeBufferHasIdentifier(buffer.data())) {
            WireLogError("MG_Remote server: control frame carries no CtrlEnvelope identifier "
                         "(size=%llu); ending the session rather than reading its union tag",
                         static_cast<unsigned long long>(size));
            break;
        }
        flatbuffers::Verifier check(buffer.data(), static_cast<std::size_t>(size));
        if (!Protocol::VerifyCtrlEnvelopeBuffer(check)) {
            WireLogError("MG_Remote server: control frame did not verify as a CtrlEnvelope "
                         "(size=%llu); ending the session", static_cast<unsigned long long>(size));
            break;
        }
        const auto* envelope = Protocol::GetCtrlEnvelope(buffer.data());
        if (const auto* flush = envelope->msg_as_LogFlush()) {
            if (flush->ack()) break;
            if (const char* inspect = std::getenv("MOBILEGL_TEST_SERVER_COUNTERS");
                inspect && std::strcmp(inspect, "1") == 0) {
                // SyncPeerLog has fenced the client's complete command prefix.
                // Acquire the applier's publication before reading its counters;
                // this serial client sends no more records until this ack.
                const auto applied = session.DataLink()->Progress()->appliedSeq.load(std::memory_order_acquire);
                const auto& verbs = session.Applier().Verbs();
                MGLOG_I("MGPipe server counters: applied=%llu resets=%llu reset-serial=%llu deaths=%llu",
                        static_cast<unsigned long long>(applied),
                        static_cast<unsigned long long>(verbs.ApplierResets()),
                        static_cast<unsigned long long>(verbs.ExpectedApplierResetSerial()),
                        static_cast<unsigned long long>(verbs.ObjectDeaths()));
            }
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
    // _exit closes control after cleanup/flush; peer EOF is the client's fence.
    std::fflush(nullptr);
    ::_exit(0);
}

// P7 wave 0, Ph slice (1) (ID-P7-1). THE SUPERVISOR OBSERVES AND NAMES A SESSION'S DEATH.
//
// Three `waitpid(active, nullptr, WNOHANG)` calls threw the status away, so a child that died of
// SIGABRT inside SessionFail and one that returned from its control loop and _exit(0)'d were the
// SAME EVENT to the supervisor: `active = -1`, no line, nothing to count. ID-P7-1 declines the
// literal 98-site latch translation and asks for this instead, because P6.5's fork-per-session
// already delivers "the next connection is served as usual" (:302) - what was missing is that
// anybody could tell a faulted session from a finished one, which is the half of PH-1 an
// operator, a fuzz arm and a flake triage all actually need.
//
// One function where there were three call sites, so the three cannot drift on what a reap means.
// Returns true when `active` was reaped. `sessionsFaulted` is the SUPERVISOR's lifetime count,
// not this child's: a supervisor that has served twenty sessions can say how many ended badly.
bool ReapActiveSession(pid_t& active, unsigned long& sessionsFaulted) {
    if (active <= 0) return false;
    int status = 0;
    if (::waitpid(active, &status, WNOHANG) != active) return false;
    const int reaped = static_cast<int>(active);
    active = -1;
    // WIFEXITED / WIFSIGNALED / WTERMSIG, the same three questions ServerSpawn.cpp:289 asks of
    // the same kind of child - but logged rather than folded into one signed integer, because
    // this side has nobody to return the answer to.
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        if (code != 0) ++sessionsFaulted;
        WireLogError("MG_Remote server: session pid=%d reaped exit=%d sessionsFaulted=%lu",
                     reaped, code, sessionsFaulted);
    } else if (WIFSIGNALED(status)) {
        ++sessionsFaulted;
        WireLogError("MG_Remote server: session pid=%d reaped signal=%d sessionsFaulted=%lu",
                     reaped, WTERMSIG(status), sessionsFaulted);
    } else {
        // Neither exited nor signalled: WUNTRACED/WCONTINUED are not passed, so this is a status
        // this build does not understand. Counted as a fault rather than silently ignored.
        ++sessionsFaulted;
        WireLogError("MG_Remote server: session pid=%d reaped status=0x%x sessionsFaulted=%lu",
                     reaped, status, sessionsFaulted);
    }
    return true;
}

// The lifetime figure, once, on the way out. Every reap also prints the running total, so a
// supervisor taken down by SIGTERM (which is how the fixtures end it) still leaves the number
// behind - this line is for the exits the supervisor chooses.
void LogSupervisorSummary(unsigned long sessionsFaulted) {
    WireLogError("MG_Remote server: supervisor pid=%d shutting down sessionsFaulted=%lu",
                 static_cast<int>(::getpid()), sessionsFaulted);
}
}

extern "C" __attribute__((visibility("default"))) int mobilegl_server_main(int argc, char** argv) {
    // The supervisor and every forked session child log under the server role.
    // Set it here rather than trusting the launcher to have done so: log role is
    // resolved from this env at first use, and a launcher that omits it would
    // silently misfile this process's main-thread server lines as the client
    // role and never forward them. Done before any logging so the value is cached
    // as server and inherited by the fork children. DIAL/scrub stay hard checks.
    if (const char* role = std::getenv("MOBILEGL_IPC_ROLE");
        !role || std::strcmp(role, "server") != 0) {
        ::setenv("MOBILEGL_IPC_ROLE", "server", 1);
    }
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--serve") serve = true;
        else if (arg == "--max-sessions" && i + 1 < argc) {
            char* end = nullptr;
            const auto count = std::strtoul(argv[++i], &end, 10);
            // This is a concurrency limit, not a cumulative session budget.
            // P6.5 deliberately supports one active rendering session.
            if (*end || count != 1) return 71;
        } else if (endpoint.empty() && arg.compare(0, 2, "--") != 0) endpoint = arg;
        else return 71;
    }
    if (endpoint.empty()) if (const char* value = std::getenv("MOBILEGL_IPC_ENDPOINT")) endpoint = value;
    if (endpoint.empty()) return 71;
    int listener = -1;
    if (SocketTransport::Listen(endpoint, &listener) != MOBILEGL_OK) return 72;
    WireLogError("MG_Remote server: pid=%d listening on %s", static_cast<int>(::getpid()), endpoint.c_str());
    pid_t active = -1;
    unsigned long sessionsFaulted = 0;
    for (;;) {
        ReapActiveSession(active, sessionsFaulted);
        std::unique_ptr<SocketTransport> control;
        const auto accepted = SocketTransport::AcceptPair(listener, serve ? 250 : 30000, control);
        if (accepted == MOBILEGL_ERR_TIMEOUT && serve) continue;
        if (accepted != MOBILEGL_OK) { LogSupervisorSummary(sessionsFaulted); ::close(listener); return 73; }
        if (!serve) {
            ::close(listener);
            if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
            RunSession(std::move(control));
        }
        ReapActiveSession(active, sessionsFaulted);
        // exit_group closes files just before waitpid can observe the exit.
        // Allow that small scheduling window; a live peer remains Busy.
        const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (active > 0 && std::chrono::steady_clock::now() < reapDeadline) {
            if (ReapActiveSession(active, sessionsFaulted)) break;
            ::usleep(1000);
        }
        if (active > 0) {
            RefuseBusy(*control, "one session is already active");
            continue;
        }
        std::fflush(nullptr);
        const pid_t child = ::fork();
        if (child == 0) { ::close(listener); RunSession(std::move(control)); }
        if (child < 0) { RefuseBusy(*control, "could not create session child"); continue; }
        active = child;
        control->CloseLocalCopy();
    }
    LogSupervisorSummary(sessionsFaulted);
    ::close(listener);
    if (endpoint[0] != '@' && endpoint.compare(0, 6, "tcp://") != 0) ::unlink(endpoint.c_str());
    return 0;
}
